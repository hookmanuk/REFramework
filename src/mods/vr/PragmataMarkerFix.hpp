#pragma once

// PragmataVR Build 50: enemy health bars, damage numbers and other world markers placed over their target on any headset.
//
// The game places these markers with app.GuiUtil.transformWorldToScreen(world position, screen size, camera role),
// called from app.GuiBehaviorV2.applyPostureData (Builds 48 / 49 probe). It projects with the game camera's current
// projection, which in VR is one eye's off-centre projection (Quest 3 left eye: 54 deg out, 40 deg in, 44 up, 55 down).
// The off-centre part moves every marker by a fixed amount: on a Quest 3 about 230 canvas px left and 170 px up (Steve
// tuned 200 / 170 by hand in Build 41), on a Pimax Dream Air only a few px. This post-hook takes the off-centre terms
// out of the result (the projection's [2][0] and [2][1]), so the marker lands where a centred view would put it, then
// adds the player's fine-tune (marker_trim). Only results returned to applyPostureData are changed; the hacking circle
// (ui3400, own tuning) and everything else calling the function are left alone. Nothing in the game is written: the
// hook edits the function's own return value before the caller sees it.

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "HookManager.hpp"
#include "mods/MethodDatabase.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/SceneManager.hpp"
#include "sdk/Math.hpp"
#include "sdk/REComponent.hpp"
#include "sdk/REGameObject.hpp"
#include "sdk/REManagedObject.hpp"
#include "utility/String.hpp"

#include "PragmataHud.hpp"

namespace pragmata_marker_fix {

inline bool g_installed{false};
inline std::mutex g_caller_mtx{};
inline std::unordered_map<uintptr_t, bool> g_caller_ok{}; // return address -> called from GuiBehaviorV2.applyPostureData

inline thread_local bool tl_apply{false};
// Build 51: which GUI the current applyPostureData call is placing (its GuiBehaviorV2's GameObject name, hashed), so
// health bars and damage numbers get their own fine-tune. 0 = unknown.
inline thread_local size_t tl_owner_hash{0};
inline thread_local int tl_owner_depth{0};
inline std::mutex g_owner_log_mtx{};
inline std::unordered_map<size_t, bool> g_owner_logged{};
inline thread_local uintptr_t tl_size{0};

inline bool caller_ok(uintptr_t ret_addr) {
    {
        std::scoped_lock _{g_caller_mtx};
        auto it = g_caller_ok.find(ret_addr);
        if (it != g_caller_ok.end()) {
            return it->second;
        }
    }
    std::string who{};
    try {
        who = MethodDatabase::get()->find_method(ret_addr);
    } catch (...) {
    }
    const bool ok = who.find("GuiBehaviorV2.applyPostureData") != std::string::npos;
    {
        std::scoped_lock _{g_caller_mtx};
        if (g_caller_ok.size() < 256) {
            g_caller_ok[ret_addr] = ok;
        }
    }
    spdlog::info("[PragmataVR] Marker fix: caller {} -> {}", who.empty() ? fmt::format("{:x}", ret_addr) : who, ok ? "corrected" : "left alone");
    return ok;
}

inline bool readable(uintptr_t p, size_t n) {
    return p >= 0x10000 && p <= 0x7FFFFFFFFFFFull && !IsBadReadPtr((const void*)p, n);
}

inline void install() {
    if (g_installed) {
        return;
    }
    g_installed = true;

    auto t = sdk::find_type_definition("app.GuiUtil");
    auto m = t != nullptr ? t->get_method("transformWorldToScreen") : nullptr;
    if (m == nullptr || m->get_function() == nullptr) {
        spdlog::info("[PragmataVR] Marker fix: app.GuiUtil.transformWorldToScreen not found, markers not corrected");
        return;
    }

    // Build 51: note the owner of each applyPostureData call (args[0] = thread context, args[1] = this).
    if (auto bt = sdk::find_type_definition("app.GuiBehaviorV2"); bt != nullptr) {
        if (auto am = bt->get_method("applyPostureData"); am != nullptr && am->get_function() != nullptr) {
            g_hookman.add(am,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
                    ++tl_owner_depth;
                    tl_owner_hash = 0;
                    if (args.size() >= 2 && args[1] != 0 && utility::re_managed_object::is_managed_object((void*)args[1])) {
                        auto go = utility::re_component::get_game_object((REComponent*)args[1]);
                        if (go != nullptr) {
                            const auto name = utility::re_game_object::get_name(go);
                            tl_owner_hash = utility::hash(name);
                            std::scoped_lock _{g_owner_log_mtx};
                            if (g_owner_logged.size() < 64 && g_owner_logged.emplace(tl_owner_hash, true).second) {
                                spdlog::info("[PragmataVR] Marker fix: applyPostureData for {}", name);
                            }
                        }
                    }
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t& ret_val, sdk::RETypeDefinition* ret_ty, uintptr_t ret_addr) {
                    if (--tl_owner_depth <= 0) {
                        tl_owner_depth = 0;
                        tl_owner_hash = 0;
                    }
                });
        }
    }

    g_hookman.add(m,
        [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
            tl_apply = false;
            const auto tune = pragmatavr::HudTuning::get().tune();
            if (!tune->marker_fix || args.size() < 4) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            }
            if (caller_ok(ret_addr)) {
                tl_apply = true;
                tl_size = args[3]; // via.Size (width, height) by reference, as the Build 49 samples showed (1920, 1080)
            }
            return HookManager::PreHookResult::CALL_ORIGINAL;
        },
        [](uintptr_t& ret_val, sdk::RETypeDefinition* ret_ty, uintptr_t ret_addr) {
            if (!tl_apply) {
                return;
            }
            tl_apply = false;
            if (!readable(ret_val, 3 * sizeof(float)) || !readable(tl_size, 2 * sizeof(float))) {
                return;
            }
            auto cam = sdk::get_primary_camera();
            if (cam == nullptr) {
                return;
            }
            const auto proj = sdk::call_object_func_easy<Matrix4x4f>((REManagedObject*)cam, "get_ProjectionMatrix");
            const float* size = (const float*)tl_size;
            const float w = size[0];
            const float h = size[1];
            if (!(w > 0.0f && h > 0.0f) || !std::isfinite(proj[2][0]) || !std::isfinite(proj[2][1])) {
                return;
            }
            const auto tune = pragmatavr::HudTuning::get().tune();
            float* r = (float*)ret_val;
            // Horizontal: canvas x = w/2 * (1 + ndc); the off-centre term moves ndc by -[2][0].
            const float dx = proj[2][0] * w * 0.5f;
            // Vertical: Steve's Build 41 Quest tuning (170 px) matches [2][1] * h/2 scaled by the canvas fit (1.6 on the
            // Quest 3: the 1920 x 1080 canvas fills the eye image width).
            const float dy = -proj[2][1] * h * 0.5f * tune->marker_vscale;
            // Build 51: each marker type's own fine-tune (Left / right, Up / down in the HUD tab).
            float tx = 0.0f, ty = 0.0f;
            if (tl_owner_hash == "ui3201Gui"_fnv) {
                tx = tune->enemy_bar[pragmatavr::DamageX];
                ty = tune->enemy_bar[pragmatavr::DamageY];
            } else if (tl_owner_hash == "ui3200Gui"_fnv || tl_owner_hash == "ui3210Gui"_fnv) {
                tx = tune->enemy_bar[pragmatavr::EnemyX];
                ty = tune->enemy_bar[pragmatavr::EnemyY];
            }
            r[0] += dx + tx;
            r[1] += dy + ty;

            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 3) {
                spdlog::info("[PragmataVR] Marker fix: [2][0] {:.4f} [2][1] {:.4f} size {:.0f} x {:.0f} -> shift ({:.1f}, {:.1f}) + fine-tune ({:.0f}, {:.0f})",
                    proj[2][0], proj[2][1], w, h, dx, dy, tx, ty);
            }
        });
    spdlog::info("[PragmataVR] Marker fix: hooked app.GuiUtil.transformWorldToScreen (applyPostureData callers only)");
}

} // namespace pragmata_marker_fix
