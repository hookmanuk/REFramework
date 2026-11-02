#pragma once

// PragmataVR: reticle depth. A port of uevrlib's reticule targeting (github.com/jbusfield/uevrlib, docs/reticule.md)
// to RE Engine: uevrlib traces a line from the aim origin along the aim and puts the reticule at the hit (falling back
// to its configured distance when nothing is hit). Here the trace is a via.physics ray, cast the way REFramework's own
// re2_vr_crosshair.lua does it (castRayAsync, all hits, nearest first), from the game camera along the gun reticle's
// direction. The reticle layer (D3D12Component.cpp) then shows the reticle at that depth.
//
// Threading: tick() runs on the main thread, in the application entry before PrerenderGUI (PragmataFixes), like the
// other Pragmata engine calls (see PragmataFixes.hpp, Build 46). The render thread only reads the atomics in
// pragmatavr::g_reticle_depth.
//
// The ray is asynchronous: a ray is issued, and on a later frame, once it has finished, its distance is read and the
// next one is issued. The distance is applied to the reticle's current direction (as the Lua script does), so the
// reticle never lags the aim, only its depth is a frame or two late.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "reframework/API.hpp"
#include "sdk/REManagedObject.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/Math.hpp"

#include "../VR.hpp"
#include "PragmataHud.hpp"

namespace pragmata_reticle_depth {

namespace detail {

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Methods {
    bool ok{false};
    sdk::REMethodDefinition* cast_async{};
    sdk::REMethodDefinition* cast_sync{};
    sdk::REMethodDefinition* set_ray{};
    sdk::REMethodDefinition* clear_options{};
    sdk::REMethodDefinition* enable_all_hits{};
    sdk::REMethodDefinition* enable_near_sort{};
    sdk::REMethodDefinition* get_filter_info{};
    sdk::REMethodDefinition* set_filter_info{};
    sdk::REMethodDefinition* filter_set_group{};
    sdk::REMethodDefinition* filter_set_layer{};
    sdk::REMethodDefinition* filter_set_mask{};
    sdk::REMethodDefinition* get_finished{};
    sdk::REMethodDefinition* get_num_contacts{};
    sdk::REMethodDefinition* get_contact{};
    sdk::REField* contact_distance{};
    uint32_t contact_size{0};
};

inline const Methods& methods() {
    static const Methods m = [] {
        Methods r{};
        const auto sys_t = sdk::find_type_definition("via.physics.System");
        const auto query_t = sdk::find_type_definition("via.physics.CastRayQuery");
        const auto result_t = sdk::find_type_definition("via.physics.CastRayResult");
        const auto filter_t = sdk::find_type_definition("via.physics.FilterInfo");
        const auto contact_t = sdk::find_type_definition("via.physics.ContactPoint");
        if (sys_t == nullptr || query_t == nullptr || result_t == nullptr || contact_t == nullptr) {
            spdlog::error("[PragmataVR] Reticle depth: via.physics types not found (system {}, query {}, result {}, contact {})",
                          sys_t != nullptr, query_t != nullptr, result_t != nullptr, contact_t != nullptr);
            return r;
        }
        r.cast_async = sys_t->get_method("castRayAsync(via.physics.CastRayQuery, via.physics.CastRayResult)");
        r.cast_sync = sys_t->get_method("castRay(via.physics.CastRayQuery, via.physics.CastRayResult)");
        r.set_ray = query_t->get_method("setRay(via.vec3, via.vec3)");
        r.clear_options = query_t->get_method("clearOptions");
        r.enable_all_hits = query_t->get_method("enableAllHits");
        r.enable_near_sort = query_t->get_method("enableNearSort");
        r.get_filter_info = query_t->get_method("get_FilterInfo");
        r.set_filter_info = query_t->get_method("set_FilterInfo");
        if (filter_t != nullptr) {
            r.filter_set_group = filter_t->get_method("set_Group");
            r.filter_set_layer = filter_t->get_method("set_Layer");
            r.filter_set_mask = filter_t->get_method("set_MaskBits");
        }
        r.get_finished = result_t->get_method("get_Finished");
        r.get_num_contacts = result_t->get_method("get_NumContactPoints");
        r.get_contact = result_t->get_method("getContactPoint(System.UInt32)");
        r.contact_distance = contact_t->get_field("Distance");
        r.contact_size = contact_t->get_valuetype_size();

        r.ok = (r.cast_async != nullptr || r.cast_sync != nullptr) && r.set_ray != nullptr && r.get_num_contacts != nullptr &&
               r.get_contact != nullptr && r.contact_distance != nullptr && r.contact_size > 0 &&
               r.contact_size <= sizeof(reframework::InvokeRet::bytes);
        spdlog::info("[PragmataVR] Reticle depth: {} (async {}, sync {}, filter {}, ContactPoint {} bytes)",
                     r.ok ? "ready" : "NOT available", r.cast_async != nullptr, r.cast_sync != nullptr,
                     r.filter_set_layer != nullptr, r.contact_size);
        return r;
    }();
    return m;
}

inline bool call_ok(const reframework::InvokeRet& ret) {
    return !ret.exception_thrown;
}

struct State {
    ::REManagedObject* query{};
    ::REManagedObject* result{};
    bool in_flight{false};
    bool async{true};
    bool disabled{false};
    float smoothed{0.0f};
    int64_t last_tick_ms{0};
    int hit_logs{0};
    int miss_logs{0};
};

inline State& state() {
    static State s{};
    return s;
}

// The first hit at least min_m from the ray start, or -1 when none.
inline float nearest_hit(const Methods& m, ::REManagedObject* result, float min_m) {
    auto nret = m.get_num_contacts->invoke(result);
    if (!call_ok(nret)) {
        return -1.0f;
    }
    const uint32_t n = (std::min)(nret.dword, 32u);
    float best = -1.0f;
    for (uint32_t i = 0; i < n; ++i) {
        std::array<void*, 1> args{(void*)(uintptr_t)i};
        auto cret = m.get_contact->invoke(result, args);
        if (!call_ok(cret)) {
            break;
        }
        const float d = *(float*)m.contact_distance->get_data_raw(cret.bytes.data(), true);
        if (!std::isfinite(d) || d < min_m) {
            continue; // the player, the camera collision, ... (uevrlib's minHitDistance)
        }
        if (best < 0.0f || d < best) {
            best = d; // near sort is asked for, but do not rely on it
        }
    }
    return best;
}

inline bool issue(const Methods& m, State& st, const pragmatavr::HudTune& tune, const Vector3f& start, const Vector3f& end) {
    if (st.query == nullptr) {
        st.query = sdk::create_instance<::REManagedObject>("via.physics.CastRayQuery");
        st.result = sdk::create_instance<::REManagedObject>("via.physics.CastRayResult");
        if (st.query == nullptr || st.result == nullptr) {
            spdlog::error("[PragmataVR] Reticle depth: could not create the ray query / result");
            st.query = st.result = nullptr;
            return false;
        }
        // Kept alive for the whole session and reused (no new objects per frame).
        utility::re_managed_object::add_ref(st.query);
        utility::re_managed_object::add_ref(st.result);
    }

    alignas(16) Vector4f s4{start.x, start.y, start.z, 0.0f};
    alignas(16) Vector4f e4{end.x, end.y, end.z, 0.0f};
    std::array<void*, 2> ray_args{(void*)&s4, (void*)&e4};
    if (!call_ok(m.set_ray->invoke(st.query, ray_args))) {
        return false;
    }
    if (m.clear_options != nullptr) m.clear_options->invoke(st.query);
    if (m.enable_all_hits != nullptr) m.enable_all_hits->invoke(st.query);
    if (m.enable_near_sort != nullptr) m.enable_near_sort->invoke(st.query);

    // Optional collision filter (re2_vr_crosshair.lua sets group 0, a layer and mask bits). Pragmata's layers are
    // not known yet, so by default the query's own filter is left as it is.
    if (tune.reticle_ray_layer >= 0 && m.get_filter_info != nullptr && m.set_filter_info != nullptr &&
        m.filter_set_layer != nullptr) {
        auto fret = m.get_filter_info->invoke(st.query);
        auto filter = call_ok(fret) ? (::REManagedObject*)fret.ptr : nullptr;
        if (filter != nullptr) {
            if (m.filter_set_group != nullptr) {
                std::array<void*, 1> a{(void*)(uintptr_t)0};
                m.filter_set_group->invoke(filter, a);
            }
            if (m.filter_set_mask != nullptr) {
                std::array<void*, 1> a{(void*)(uintptr_t)tune.reticle_ray_mask};
                m.filter_set_mask->invoke(filter, a);
            }
            std::array<void*, 1> a{(void*)(uintptr_t)(uint32_t)tune.reticle_ray_layer};
            m.filter_set_layer->invoke(filter, a);
            std::array<void*, 1> b{(void*)filter};
            m.set_filter_info->invoke(st.query, b);
        }
    }

    auto sys = sdk::get_native_singleton<void>("via.physics.System");
    if (sys == nullptr) {
        return false;
    }
    std::array<void*, 2> cast_args{(void*)st.query, (void*)st.result};
    st.async = m.cast_async != nullptr;
    const auto ret = (st.async ? m.cast_async : m.cast_sync)->invoke(sys, cast_args);
    if (!call_ok(ret)) {
        spdlog::error("[PragmataVR] Reticle depth: the ray cast threw; reticle depth off for this session");
        st.disabled = true;
        return false;
    }
    st.in_flight = true;
    return true;
}

} // namespace detail

// Main thread, once per frame (PragmataFixes, before PrerenderGUI).
inline void tick() {
    using namespace detail;
    auto& out = pragmatavr::g_reticle_depth;
    auto& st = state();
    const auto tune = pragmatavr::HudTuning::get().tune();
    const auto now = now_ms();
    const float dt = st.last_tick_ms != 0 ? (float)(std::clamp<int64_t>(now - st.last_tick_ms, 0, 250)) / 1000.0f : 0.0f;
    st.last_tick_ms = now;

    // Only while the New reticle with Depth TARGET is on, in the headset, and the game is drawing the gun reticle.
    const auto& vr = VR::get();
    if (st.disabled || !tune->reticle_new || !tune->reticle_auto_depth || !vr->is_hmd_active() ||
        now - out.seen_ms.load(std::memory_order_relaxed) > 1000) {
        st.smoothed = 0.0f;
        out.depth_m.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const auto& m = methods();
    if (!m.ok) {
        st.disabled = true;
        return;
    }

    // The reticle's direction in game camera space (x right, y up, looking down -z), as the layer places it.
    const float tx = out.tan_x.load(std::memory_order_relaxed);
    const float ty = out.tan_y.load(std::memory_order_relaxed);
    const float axis_len = std::sqrt(1.0f + tx * tx + ty * ty); // ray length per metre along the reticle's axis

    // Fixed mode: the reticle sits on the game camera's own aim (the layer is placed on the game camera's axis).
    // Follows Head: the reticle is in view space, so trace where the headset looks (the camera with the HMD pose).
    const Matrix4x4f cam = tune->head_locked ? vr->get_last_render_matrix() : vr->get_original_camera_matrix();
    const Vector3f origin{cam[3]};
    const Vector3f local_dir = glm::normalize(Vector3f{tx, ty, -1.0f});
    const Vector3f dir = glm::normalize(Vector3f{glm::mat3{cam} * local_dir});
    if (!std::isfinite(dir.x) || !std::isfinite(origin.x)) {
        return;
    }

    const float fixed = tune->screen[pragmatavr::ScreenReticleDist];
    const float max_m = (std::max)(tune->reticle_depth_max, 2.0f);
    float target = st.smoothed > 0.0f ? st.smoothed : fixed;

    // Read a finished ray (or one cast synchronously), then cast the next one.
    bool finished = !st.in_flight;
    if (st.in_flight) {
        if (!st.async || m.get_finished == nullptr) {
            finished = true;
        } else {
            auto fret = m.get_finished->invoke(st.result);
            finished = call_ok(fret) && fret.byte != 0;
        }
        if (finished) {
            st.in_flight = false;
            const float d = nearest_hit(m, st.result, tune->reticle_depth_min);
            const bool hit = d > 0.0f;
            out.hit.store(hit, std::memory_order_relaxed);
            // uevrlib: the hit distance (along the ray) when something is hit, else the configured distance.
            target = hit ? (std::min)(d, max_m) / axis_len : fixed;
            if (hit && st.hit_logs < 3) {
                ++st.hit_logs;
                spdlog::info("[PragmataVR] Reticle depth: hit at {:.2f} m along the aim ({:.2f} m deep), {}", d, target,
                             tune->head_locked ? "headset view" : "game camera");
            } else if (!hit && st.miss_logs < 3) {
                ++st.miss_logs;
                spdlog::info("[PragmataVR] Reticle depth: nothing hit within {:.0f} m, reticle at Distance {:.1f} m", max_m, fixed);
            }
        }
    }
    if (finished) {
        issue(m, st, *tune, origin, origin + dir * max_m);
    }

    target = std::clamp(target, 0.1f, max_m);
    if (st.smoothed <= 0.0f || tune->reticle_depth_smooth <= 0.0f) {
        st.smoothed = target;
    } else {
        const float a = 1.0f - std::exp(-dt / tune->reticle_depth_smooth);
        st.smoothed += (target - st.smoothed) * a;
    }
    out.depth_m.store(st.smoothed, std::memory_order_relaxed);
    out.ms.store(now, std::memory_order_relaxed);
}

} // namespace pragmata_reticle_depth
