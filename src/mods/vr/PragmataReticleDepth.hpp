#pragma once

// PragmataVR: reticle depth. A port of uevrlib's reticule targeting (github.com/jbusfield/uevrlib, docs/reticule.md):
// the reticle is shown at the depth of what the aim is on, falling back to RETICLE Distance when nothing is there.
// The depth comes from the game's own depth buffer (PragmataDepthProbe.hpp, AFW render mode only): Pragmata's physics
// rays miss much of the scenery and hit invisible trigger volumes, so what was drawn is used instead.
//
// Threading: tick() runs on the main thread, in the application entry before PrerenderGUI (PragmataFixes). It hands
// the aim ray to the render thread and smooths the depth the probe reads back, through pragmatavr::g_reticle_depth.
// The reticle layer (D3D12Component.cpp) is then placed at that depth.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "../VR.hpp"
#include "PragmataHud.hpp"

namespace pragmata_reticle_depth {

namespace detail {

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct State {
    float smoothed{0.0f};
    int64_t last_tick_ms{0};
};

inline State& state() {
    static State s{};
    return s;
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
    if (!tune->reticle_new || !tune->reticle_auto_depth || !vr->is_hmd_active() ||
        now - out.seen_ms.load(std::memory_order_relaxed) > 1000) {
        st.smoothed = 0.0f;
        out.depth_m.store(0.0f, std::memory_order_relaxed);
        out.ray_ms.store(0, std::memory_order_relaxed); // depth buffer probe idle
        return;
    }

    // The reticle's direction in game camera space (x right, y up, looking down -z), as the layer places it.
    const float tx = out.tan_x.load(std::memory_order_relaxed);
    const float ty = out.tan_y.load(std::memory_order_relaxed);
    const float axis_len = std::sqrt(1.0f + tx * tx + ty * ty); // ray length per metre along the reticle's axis

    // Fixed mode: the reticle sits on the game camera's own aim (the layer is placed on the game camera's axis).
    // Follows Head: the reticle is in view space, so the aim is where the headset looks (the camera with the HMD pose).
    const Matrix4x4f cam = tune->head_locked ? vr->get_last_render_matrix() : vr->get_original_camera_matrix();
    const Vector3f origin{cam[3]};
    const Vector3f dir = glm::normalize(Vector3f{glm::mat3{cam} * glm::normalize(Vector3f{tx, ty, -1.0f})});
    if (!std::isfinite(dir.x) || !std::isfinite(origin.x)) {
        return;
    }

    // The aim ray for the depth buffer probe (render thread): it looks in the eye image where the ray is at the last depth.
    out.ray_ox.store(origin.x, std::memory_order_relaxed);
    out.ray_oy.store(origin.y, std::memory_order_relaxed);
    out.ray_oz.store(origin.z, std::memory_order_relaxed);
    out.ray_dx.store(dir.x, std::memory_order_relaxed);
    out.ray_dy.store(dir.y, std::memory_order_relaxed);
    out.ray_dz.store(dir.z, std::memory_order_relaxed);
    out.ray_guess_m.store(st.smoothed > 0.0f ? st.smoothed * axis_len : 20.0f, std::memory_order_relaxed);
    out.ray_ms.store(now, std::memory_order_relaxed);

    // What the game drew along the aim (distance along the ray), when a result is fresh. Nothing drawn there (sky),
    // closer than reticle_depth_min, or no result (not AFW): RETICLE Distance, as in uevrlib.
    const float fixed = tune->screen[pragmatavr::ScreenReticleDist];
    const float max_m = (std::max)(tune->reticle_depth_max, 2.0f);
    float target = fixed;
    const int64_t buf_ms = out.buf_ms.load(std::memory_order_relaxed);
    if (buf_ms != 0 && now - buf_ms < 300) {
        const float d = out.buf_dist_m.load(std::memory_order_relaxed);
        if (d >= tune->reticle_depth_min) {
            target = (std::min)(d, max_m) / axis_len;
        }
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
