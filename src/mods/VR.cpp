#define NOMINMAX

#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtx/transform.hpp>

#include <utility/ScopeGuard.hpp>

#include <sdk/TDBVer.hpp>
#include <reframework/API.hpp>

#if TDB_VER >= 83
#include "sdk/regenny/re9/via/Window.hpp"
#include "sdk/regenny/re9/via/SceneView.hpp"
#elif TDB_VER <= 49
#include "sdk/regenny/re7/via/Window.hpp"
#include "sdk/regenny/re7/via/SceneView.hpp"
#elif TDB_VER < 69
#include "sdk/regenny/re3/via/Window.hpp"
#include "sdk/regenny/re3/via/SceneView.hpp"
#elif TDB_VER == 69
#include "sdk/regenny/re8/via/Window.hpp"
#include "sdk/regenny/re8/via/SceneView.hpp"
#elif TDB_VER == 70
#include "sdk/regenny/re2_tdb70/via/Window.hpp"
#include "sdk/regenny/re2_tdb70/via/SceneView.hpp"
#elif TDB_VER >= 71
#ifdef SF6
#include "sdk/regenny/sf6/via/Window.hpp"
#include "sdk/regenny/sf6/via/SceneView.hpp"
#elif defined(RE4)
#include "sdk/regenny/re4/via/Window.hpp"
#include "sdk/regenny/re4/via/SceneView.hpp"
#elif defined(DD2) || TDB_VER >= 74
#include "sdk/regenny/dd2/via/Window.hpp"
#include "sdk/regenny/dd2/via/SceneView.hpp"
#else
#include "sdk/regenny/mhrise_tdb71/via/Window.hpp"
#include "sdk/regenny/mhrise_tdb71/via/SceneView.hpp"
#endif
#endif

#include "sdk/Math.hpp"
#include "sdk/SceneManager.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/Renderer.hpp"
#include "sdk/Application.hpp"
#include "sdk/Renderer.hpp"
#include "sdk/REMath.hpp"
#include "sdk/REGameObject.hpp"
#include "sdk/ManagedObject.hpp"

#include "utility/Scan.hpp"
#include "utility/FunctionHook.hpp"
#include "utility/Module.hpp"
#include "utility/Memory.hpp"
#include "utility/Registry.hpp"
#include "utility/ScopeGuard.hpp"

#include "FirstPerson.hpp"
#include "ManualFlashlight.hpp"
//#include "TemporalUpscaler.hpp"
#include "VR.hpp"

#if defined(PRAGMATA)
#include "vr/PragmataHud.hpp"
#include "vr/PragmataMenuFont.hpp"
#include "vr/PragmataPerf.hpp"
#include <limits>
#endif
#include <MinHook.h>

bool inside_on_end = false;
uint32_t actual_frame_count = 0;

thread_local bool inside_gui_draw = false;

std::shared_ptr<VR>& VR::get() {
    static auto inst = std::make_shared<VR>();
    return inst;
}

std::unique_ptr<FunctionHook> g_input_hook{};
std::unique_ptr<FunctionHook> g_projection_matrix_hook2{};
std::unique_ptr<FunctionHook> g_overlay_draw_hook{};
std::unique_ptr<FunctionHook> g_post_effect_draw_hook{};
std::unique_ptr<FunctionHook> g_wwise_listener_update_hook{};
//std::unique_ptr<FunctionHook> g_get_sharpness_hook{};

#if TDB_VER <= 49
std::optional<regenny::via::Size> g_previous_size{};
#endif

// Purpose: spoof the render target size to the size of the HMD displays
void VR::on_view_get_size(REManagedObject* scene_view, float* result) {
    // There are some very dumb optimizations that cause set_DisplayType
    // to go through this hook. This function is actually something like "updateSceneView"
    static thread_local bool already_inside = false;

    if (already_inside) {
        return;
    }

    already_inside = true;

    utility::ScopeGuard _{ [&]() { already_inside = false; } };

    if (!g_framework->is_ready()) {
        return;
    }

    if (!get_runtime()->loaded) {
        return;
    }

    if (m_disable_backbuffer_size_override) {
        return;
    }

    auto regenny_view = (regenny::via::SceneView*)scene_view;
    auto window = regenny_view->window;

    static auto via_scene_view = sdk::find_type_definition("via.SceneView");
    static auto set_display_type_method = via_scene_view != nullptr ? via_scene_view->get_method("set_DisplayType") : nullptr;

    // Force the display to stretch to the window size
    if (set_display_type_method != nullptr) {
        set_display_type_method->call(sdk::get_thread_context(), regenny_view, via::DisplayType::Fit);
    } else {
#if not defined(RE7) || TDB_VER <= 49
        static auto is_sunbreak = utility::get_module_path(utility::get_executable())->find("MHRiseSunbreakDemo") != std::string::npos;

        if (is_sunbreak) {
            *(regenny::via::DisplayType*)((uintptr_t)&regenny_view->display_type + 4) = regenny::via::DisplayType::Fit;
        } else {
            regenny_view->display_type = regenny::via::DisplayType::Fit;
        }
#else
        *(regenny::via::DisplayType*)((uintptr_t)&regenny_view->display_type + 4) = regenny::via::DisplayType::Fit;
#endif
    }

    auto wanted_width = 0.0f;
    auto wanted_height = 0.0f;

    // Set the window size, which will increase the size of the backbuffer
    if (window != nullptr) {
        static const auto is_gng = utility::get_module_path(utility::get_executable())->find("makaimura_GG_RE.exe") != std::string::npos;

        auto& window_width = is_gng ? *(uint32_t*)((uintptr_t)window + 0x48) : window->width;
        auto& window_height = is_gng ? *(uint32_t*)((uintptr_t)window + 0x4C) : window->height;

        if (is_hmd_active()) {
#if TDB_VER <= 49
            if (!g_previous_size) {
                g_previous_size = regenny::via::Size{ (float)window->width, (float)window->height };
            }
#endif

            bool isFR = is_using_afw_foveated();
            uint32_t render_width = (isFR) ? round(get_hmd_width() * get_foveated_ratio()) : get_hmd_width();
            uint32_t render_height = (isFR) ? round(get_hmd_height() * get_foveated_ratio()) : get_hmd_height();

            window_width = render_width;
            window_height = render_height;

            if (m_is_d3d12 && m_d3d12.is_initialized()) {
                const auto& backbuffer_size = m_d3d12.get_backbuffer_size();

                if (backbuffer_size[0] > 0 && backbuffer_size[1] > 0) {
                    if (std::abs((int)backbuffer_size[0] - (int)render_width) > 50 || std::abs((int)backbuffer_size[1] - (int)render_height) > 50) {
                        const auto now = get_game_frame_count();

                        if (!m_backbuffer_inconsistency) {
                            m_backbuffer_inconsistency_start = now;
                            m_backbuffer_inconsistency = true;
                        }

                        const auto is_true_inconsistency = (now - m_backbuffer_inconsistency_start) >= 5;

                        if (is_true_inconsistency) {
                            // Force a reset of the backbuffer size
                            window_width = render_width + 1;
                            window_height = render_height + 1;

                            spdlog::info("[VR] Previous backbuffer size: {}x{}", backbuffer_size[0], backbuffer_size[1]);
                            if (isFR)
                                spdlog::info("[VR] Backbuffer size inconsistency detected, resetting backbuffer size to {}x{} for foveated rendering, target buffer size {}x{}",
                                    render_width, render_height, window_width, window_height);
                            else
                                spdlog::info("[VR] Backbuffer size inconsistency detected, resetting backbuffer size to {}x{}", render_width, render_height);

                            // m_backbuffer_inconsistency gets set to false on device reset.
                        }
                    }
                } else {
                    m_backbuffer_inconsistency = false;
                }
            }

            render_size[0] = window_width;
            render_size[1] = window_height;
        } else {
            m_backbuffer_inconsistency = false;

#if TDB_VER > 49
            window_width = is_gng ? (uint32_t)*(float*)((uintptr_t)window + 0x88) : (uint32_t)window->borderless_size.w;
            window_height = is_gng ? (uint32_t)*(float*)((uintptr_t)window + 0x8C) : (uint32_t)window->borderless_size.h;
#else
            if (g_previous_size) {
                window->width = (uint32_t)g_previous_size->w;
                window->height = (uint32_t)g_previous_size->h;

                g_previous_size = std::nullopt;
            }
#endif
        }

        wanted_width = (float)window_width;
        wanted_height = (float)window_height;

        // Might be usable in other games too
#if defined(SF6) || TDB_VER >= 73
        if (!is_gng) {
            window->borderless_size.w = (float)window_width;
            window->borderless_size.h = (float)window_height;
        }
#endif
    }

    //auto out = original_func(scene_view, result);

    if (!m_in_render) {
        //return original_func(scene_view, result);
    }

    // spoof the size to the HMD's size
#if TDB_VER < 73
    result[0] = wanted_width;
    result[1] = wanted_height;
#else
    // Stupid optimizations cause the game to not use the result variant of this function
    // but rather update the current scene view's size directly.
    regenny_view->size.w = wanted_width;
    regenny_view->size.h = wanted_height;
#endif
}

void VR::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (result == nullptr || !g_framework->is_ready() || !is_hmd_active() || m_disable_projection_matrix_override) {
        return;
    }

    /*if (camera != sdk::get_primary_camera()) {
        return original_func(camera, result);
    }*/

    if (!m_in_render) {
       // return original_func(camera, result);
    }

    if (m_in_lightshaft) {
        //return original_func(camera, result);
    }

    if (is_using_afw_foveated()) {
        if (camera != m_multipass_cameras[0] && camera != m_multipass_cameras[1]) {
            return;
        }
    }

#ifdef RE4
    if (const auto game_object = ((REComponent*)camera)->ownerGameObject; game_object != nullptr) {
        if (game_object->name != nullptr) {
            // Allows the sniper scope to work
            if (utility::re_string::get_view(game_object->name).starts_with(L"ScopeCamera")) {
                return;
            }
        }
    }
#endif

    // Get the projection matrix for the correct eye
    // For some reason we need to flip the projection matrix here?

    auto cameras = get_cameras();
    if (m_fix_item_inspection->value() && (camera != cameras[0] && camera != cameras[1])) {
        return;
    } else if (is_using_any_afw()) {
        std::shared_lock _{get_runtime()->eyes_mtx};
        auto eye_index = (m_render_frame_count + 1) % 2 == m_left_eye_interval ? EyeLeft : EyeRight;
        auto other_eye_index = (m_render_frame_count + 1) % 2 == m_left_eye_interval ? EyeRight : EyeLeft;
        if ((is_using_afw_foveated() && m_multipass.pass == 0)) {
            *result = get_runtime()->foveated_projections[eye_index];
        }
        else {
            if (eye_index == EyeLeft) {
                eye_index = m_frame_count % 2 == m_left_eye_interval ? EyeLeft : EyeRight;
            }
            *result = get_runtime()->projections[eye_index];
        }
    }
    else {
        *result = get_current_projection_matrix(false);
    }
}

Matrix4x4f* VR::gui_camera_get_projection_matrix_hook(REManagedObject* camera, Matrix4x4f* result) {
    auto original_func = g_projection_matrix_hook2->get_original<decltype(VR::gui_camera_get_projection_matrix_hook)>();

    auto& vr = VR::get();

    if (result == nullptr || !g_framework->is_ready() || !vr->is_hmd_active() || vr->m_disable_gui_camera_projection_matrix_override) {
        return original_func(camera, result);
    }

    /*if (camera != sdk::get_primary_camera()) {
        return original_func(camera, result);
    }*/

    if (!vr->m_in_render) {
       // return original_func(camera, result);
    }

    if (vr->m_in_lightshaft) {
        //return original_func(camera, result);
    }

#if defined(PRAGMATA)
    // PragmataVR: per-panel zoom/offset and eye selection for Pragmata's 3D HUD (pragmatavr_hud.ini).
    {
        const auto tune = pragmatavr::HudTuning::get().tune();

        // Build 16 test: the game's own flat projection, so the 3D HUD lands where it would on a monitor.
        if (tune->enabled && tune->flat_test) {
            // Build 38: the HUD elements themselves are Screen views in these modes; what is still drawn through the GUI
            // camera is anchored in the world (hacking circle, door hack, reticle, enemy markers). Give those the same
            // per-eye headset projection the world uses, so they line up on any headset without per-headset offsets.
            if (tune->world_headset && (tune->flat_screen || tune->hud_panel)) {
#if TDB_VER > 49
                *result = vr->get_current_projection_matrix(false);
#else
                *result = vr->get_current_projection_matrix(true);
#endif
                // Build 44/45: enemy health bars, boss bars and damage numbers shift without touching the game's objects:
                // move them on screen by offsetting the projection while they draw (canvas pixels -> NDC; the canvas is
                // fitted to the eye image width).
                const auto gh = pragmatavr::g_current_gui_hash;
                // Build 50: retired (never fired; markers are corrected in PragmataMarkerFix.hpp, marker_trim applies there).
                const bool is_bar = false && (gh == "ui3200Gui"_fnv || gh == "ui3210Gui"_fnv);
                const bool is_damage = false && gh == "ui3201Gui"_fnv && tune->shift_damage;
                if (is_bar || is_damage) {
                    const int f = is_damage ? pragmatavr::DamageX : pragmatavr::EnemyX;
                    const float w = (float)vr->get_hmd_width();
                    const float h = (float)vr->get_hmd_height();
                    if (w > 0.0f && h > 0.0f) {
                        const float s = (std::min)(w / 1920.0f, h / 1080.0f);
                        const float nx = tune->enemy_bar[f] * 2.0f * s / w;
                        const float ny = -tune->enemy_bar[f + 1] * 2.0f * s / h;
                        pragmatavr::adjust_projection(*result, 1.0f, nx, ny);
                        static std::atomic<int> logged_mask{0};
                        const int bit = is_damage ? 1 : 2;
                        if ((logged_mask.fetch_or(bit) & bit) == 0) {
                            spdlog::info("[PragmataVR] {}: projection shifted by NDC ({:.4f}, {:.4f})", is_damage ? "Damage numbers" : "Enemy bars", nx, ny);
                        }
                    }
                }
                return result;
            }
            return original_func(camera, result);
        }

        if (tune->enabled && tune->layout_mode == 1) {
            // Game-screen HUD: keep the game's own flat GUI projection and map it onto a virtual screen in each eye.
            original_func(camera, result);
            const Matrix4x4f flat = *result;
            const bool left = (vr->m_frame_count % 2) == vr->m_left_eye_interval;

            Matrix4x4f eye_proj{};
            {
                std::shared_lock _{vr->get_runtime()->eyes_mtx};
                eye_proj = vr->get_runtime()->projections[(uint32_t)(left ? VRRuntime::Eye::LEFT : VRRuntime::Eye::RIGHT)];
            }

            const auto eye_to_head = vr->get_eye_transform(left ? 0 : 1);
            Matrix4x4f m{};

            const auto st = pragmatavr::HudTuning::get().head_state();
            const auto view = pragmatavr::eye_view(st.head, eye_to_head);
            const auto plane = pragmatavr::hud_plane(*tune, st.hud, flat);

            if (st.valid && pragmatavr::screen_projection(m, flat, eye_proj, view, plane)) {
                *result = m;
            } else {
                *result = Matrix4x4f{0.0f}; // the virtual screen is behind this eye: draw nothing
            }

            if (tune->log_elements) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    spdlog::info("[PragmataVR] Game-screen HUD active: flat projection [0][0]={} [1][1]={}", flat[0][0], flat[1][1]);
                }
            }

            return result;
        }

        if (tune->enabled) {
            bool left = (vr->m_frame_count % 2) == vr->m_left_eye_interval;

            switch (tune->eye_mode) {
            case pragmatavr::EyeMode::Swapped: left = !left; break;
            case pragmatavr::EyeMode::Left: left = true; break;
            case pragmatavr::EyeMode::Right: left = false; break;
            default: break;
            }

            Matrix4x4f m{};
            {
                std::shared_lock _{vr->get_runtime()->eyes_mtx};
                m = vr->get_runtime()->projections[(uint32_t)(left ? VRRuntime::Eye::LEFT : VRRuntime::Eye::RIGHT)];
            }

            const float x = left ? tune->depth_shift : -tune->depth_shift;
            pragmatavr::adjust_projection(m, tune->zoom, x, 0.0f);

            // Build 14: HUD fixed in the room as a flat panel at the HUD distance.
            // Build 12-13 put the GUI camera's space straight into the room (eye_proj * eye_view * anchor). The GUI
            // space is not in meters: the HUD content sits a fraction of a unit from the camera, so each eye's
            // 3 cm offset from the head made a huge gap between the eyes (about 22% of the image in the Build 13
            // snapshots), the HUD read as very close, swung with small head moves, popped in and out of depth, and the
            // weapon wheel fell outside the left eye. Now the HUD is first projected as the player sees it when it
            // follows the head (a centered version of the eye projection, so sizes and places stay as tuned), and that
            // image is mapped onto a panel screen.distance meters in front of the HUD anchor, seen from each eye's
            // real pose (the Build 8 screen math). Depth and parallax are then those of a real panel at that distance,
            // whatever units the game uses.
            if (tune->hud_fixed) {
                const auto st = pragmatavr::HudTuning::get().head_state();
                const bool real_left = (vr->m_frame_count % 2) == vr->m_left_eye_interval;

                Matrix4x4f eye_proj{};
                Matrix4x4f left_proj{};
                Matrix4x4f right_proj{};
                {
                    std::shared_lock _{vr->get_runtime()->eyes_mtx};
                    left_proj = vr->get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
                    right_proj = vr->get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
                }
                eye_proj = real_left ? left_proj : right_proj;

                // Centered "head" projection: the two eyes' scale, no off-center shift.
                Matrix4x4f center = left_proj;
                center[0][0] = (left_proj[0][0] + right_proj[0][0]) * 0.5f;
                center[1][1] = (left_proj[1][1] + right_proj[1][1]) * 0.5f;
                center[2][0] = 0.0f;
                center[2][1] = 0.0f;
                pragmatavr::adjust_projection(center, tune->zoom, 0.0f, 0.0f);

                const float dist = tune->screen[pragmatavr::ScreenDistance];
                const float half_w = center[0][0] != 0.0f ? dist / std::fabs(center[0][0]) : dist;
                const float half_h = center[1][1] != 0.0f ? dist / std::fabs(center[1][1]) : dist;
                const auto plane = pragmatavr::make_plane(st.hud, 0.0f, 0.0f, dist, half_w, half_h);
                const auto eye_to_head = vr->get_eye_transform(real_left ? 0 : 1);
                const auto view = pragmatavr::eye_view(st.head, eye_to_head);

                if (tune->log_elements) {
                    static int logged = 0;
                    if (logged < 2 && real_left == (logged == 0)) {
                        ++logged;
                        const glm::mat4 e2h{eye_to_head};
                        spdlog::info("[PragmataVR] Fixed HUD panel: {} eye proj [0][0]={:.4f} [1][1]={:.4f} [2][0]={:.4f} [2][1]={:.4f} | "
                            "eye_to_head pos ({:.4f}, {:.4f}, {:.4f}) right ({:.4f}, {:.4f}, {:.4f}) fwd ({:.4f}, {:.4f}, {:.4f}) | "
                            "panel {:.2f} m, {:.2f} x {:.2f} m",
                            real_left ? "left" : "right", eye_proj[0][0], eye_proj[1][1], eye_proj[2][0], eye_proj[2][1],
                            e2h[3].x, e2h[3].y, e2h[3].z, e2h[0].x, e2h[0].y, e2h[0].z, e2h[2].x, e2h[2].y, e2h[2].z,
                            dist, half_w * 2.0f, half_h * 2.0f);
                    }
                }

                Matrix4x4f out{};
                if (st.valid && pragmatavr::screen_projection(out, center, eye_proj, view, plane)) {
                    m = out;
                } else if (st.valid) {
                    m = Matrix4x4f{0.0f}; // the panel is behind this eye: draw nothing
                }
            }

            if (tune->log_elements) {
                pragmatavr::HudTuning::get().note_element(pragmatavr::g_current_gui_hash, pragmatavr::g_current_gui_name);
            }

            *result = m;
            return result;
        }
    }
#endif

    // Get the projection matrix for the correct eye
    // For some reason we need to flip the projection matrix here?
#if TDB_VER > 49
    *result = vr->get_current_projection_matrix(false);
#else
    *result = vr->get_current_projection_matrix(true);
#endif

    return result;
}

void VR::on_camera_get_view_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (result == nullptr || !g_framework->is_ready()) {
        return;
    }

    if (!is_hmd_active() || m_disable_view_matrix_override) {
        return;
    }

    auto cameras = get_cameras();

    if (camera != cameras[0] && camera != cameras[1]) {
        return;
    }

    // A little note on this. We can't actually use the camera (when using multipass) to detect how to adjust the view matrix
    // For some reason, the game *always* uses the main camera when calling this function, even though it's rendering the other camera
    // So we detect which pass is getting called and adjust the view matrix accordingly
    //if (!is_using_multipass()) {
        auto& mtx = *result;

        //get the flipped eye to get the correct transform. something something right->left handedness i think
        const auto current_eye_transform = get_current_eye_transform(true);
        //auto current_head_pos = -(glm::inverse(vr->get_rotation(0)) * ((vr->get_position(0)) - vr->m_standing_origin));
        //current_head_pos.w = 0.0f;

        // Apply the complete eye transform. This fixes the need for parallel projections on all canted headsets like Pimax
        mtx = current_eye_transform * mtx;
    //}
}

HookManager::PreHookResult VR::pre_set_hdr_mode(std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
    if (!VR::get()->is_hmd_active()) {
        return HookManager::PreHookResult::CALL_ORIGINAL;
    }

    if (args.size() >= 2) {
        args[1] = 0;
    }

    return HookManager::PreHookResult::CALL_ORIGINAL;
}

void VR::inputsystem_update_hook(void* ctx, REManagedObject* input_system) {
    auto original_func = g_input_hook->get_original<decltype(VR::inputsystem_update_hook)>();

    if (!g_framework->is_ready()) {
        original_func(ctx, input_system);
        return;
    }

    auto& mod = VR::get();
    const auto now = std::chrono::steady_clock::now();
    auto is_using_controller = (now - mod->get_last_controller_update()) <= std::chrono::seconds(10);

    if (mod->get_controllers().empty()) {
        // no controllers connected, don't do anything
        original_func(ctx, input_system);
        return;
    }

    auto lstick = sdk::call_object_func<REManagedObject*>(input_system, "get_LStick", sdk::get_thread_context());
    auto rstick = sdk::call_object_func<REManagedObject*>(input_system, "get_RStick", sdk::get_thread_context());

    if (lstick == nullptr || rstick == nullptr) {
        original_func(ctx, input_system);
        return;
    }

    auto button_bits_obj = sdk::call_object_func<REManagedObject*>(input_system, "get_ButtonBits", sdk::get_thread_context(), input_system);

    if (button_bits_obj == nullptr) {
        original_func(ctx, input_system);
        return;
    }

    auto left_axis = mod->get_left_stick_axis();
    auto right_axis = mod->get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);

    // Current actual button bits used by the game
    auto& button_bits_down = *sdk::get_object_field<uint64_t>(button_bits_obj, "Down");
    auto& button_bits_on = *sdk::get_object_field<uint64_t>(button_bits_obj, "On");
    auto& button_bits_up = *sdk::get_object_field<uint64_t>(button_bits_obj, "Up");

    //button_bits_down |= mod->m_button_states_down.to_ullong();
    //button_bits_on |= mod-> m_button_states_on.to_ullong();
    //button_bits_up |= mod->m_button_states_up.to_ullong();

    auto keep_button_down = [&](app::ropeway::InputDefine::Kind button) {
        if ((mod->m_button_states_on.to_ullong() & (uint64_t)button) == 0 && (mod->m_button_states_down.to_ullong() & (uint64_t)button) == 0) {
            return;
        }

        if ((mod->m_button_states_on.to_ullong() & (uint64_t)button) == 0) {
            if (mod->m_button_states_down.to_ullong() & (uint64_t)button) {
                button_bits_on |= (uint64_t)button;
                button_bits_down &= ~(uint64_t)button;
            } else {
                button_bits_down |= (uint64_t)button;
            }
        } else {
            button_bits_down &= ~(uint64_t)button;
            button_bits_on |= (uint64_t)button;
        }
    };

    const auto deadzone = mod->m_joystick_deadzone->value();

    if (left_axis_len > deadzone) {
        mod->m_last_controller_update = now;
        is_using_controller = true;

        // Override the left stick's axis values to the VR controller's values
        Vector3f axis{ left_axis.x, left_axis.y, 0.0f };
        sdk::call_object_func<void*>(lstick, "update", sdk::get_thread_context(), lstick, &axis, &axis);

        keep_button_down(app::ropeway::InputDefine::Kind::UI_L_STICK);
    }

    if (right_axis_len > deadzone) {
        mod->m_last_controller_update = now;
        is_using_controller = true;

        // Override the right stick's axis values to the VR controller's values
        Vector3f axis{ right_axis.x, right_axis.y, 0.0f };
        sdk::call_object_func<void*>(rstick, "update", sdk::get_thread_context(), rstick, &axis, &axis);

        keep_button_down(app::ropeway::InputDefine::Kind::UI_R_STICK);
    }

    // Causes the right stick to take effect properly
    if (is_using_controller) {
        sdk::call_object_func<void*>(input_system, "set_InputMode", sdk::get_thread_context(), input_system, app::ropeway::InputDefine::InputMode::Pad);
    }

    original_func(ctx, input_system);

    mod->openvr_input_to_re2_re3(input_system);
}

bool VR::on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_ctx) {
    auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();

    const auto parent_layer = layer->get_parent();
    for(int i = 0; i <scene_layers.size(); i ++)
    {
        if (parent_layer == scene_layers[i]) {
            uiBufferTex[i] = layer->get_ui_buffer_tex_d3d12();
#if defined(PRAGMATA)
            // Build 26: the engine hands out a different UI buffer from frame to frame; tell the hand-off hook now.
            if (auto note = pragmatavr::g_note_ui_buffer.load(); note != nullptr) {
                note((void*)uiBufferTex[i]);
            }
#endif
        }
    }

    // just don't render anything at all.
    // overlays just seem to break stuff in VR.
    if (!is_hmd_active()) {
        return true;
    }

#if defined(PRAGMATA)
    // PragmataVR Build 17 test: force the engine's overlay layer draw off or on (Engine UI blend in the mod menu).
    {
        const auto t = pragmatavr::HudTuning::get().tune();
        static int logged_mode = -1;
        if (logged_mode != t->engine_overlay) {
            logged_mode = t->engine_overlay;
            spdlog::info("[PragmataVR] Engine overlay layer draw: {} (REFramework allow_engine_overlays = {})",
                t->engine_overlay == 0 ? "default" : (t->engine_overlay == 1 ? "forced off" : "forced on"), m_allow_engine_overlays->value());
        }
        if (t->engine_overlay == 1) {
            return false;
        }
        if (t->engine_overlay == 2) {
            return true;
        }
    }
#endif

    // NOT RE3
    // for some reason RE3 has weird issues with the overlay rendering
    // causing double vision
#if (TDB_VER < 70 and not defined(RE3)) or (TDB_VER >= 70 and (not defined(RE3) and not defined(RE2) and not defined(RE7) and not defined(RE4) and not defined(SF6)))
    if (m_allow_engine_overlays->value()) {
        return true;
    }
#endif

    return false;
}

bool VR::on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_ctx) {
    return true;
}

bool VR::on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
    if (!is_hmd_active()) {
        return true;
    }

    auto scene_layer = layer->get_parent();
    const auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return true;
    }
    
    static auto render_output_type = sdk::find_type_definition("via.render.RenderOutput")->get_type();
    auto render_output_component = utility::re_component::find(camera, render_output_type);

    if (render_output_component == nullptr) {
        return true;
    }

    if (!m_disable_post_effect_fix) {
        // Set the distortion type back to flatscreen mode
        // this will fix various graphical bugs
        //sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 0); // None

        if (scene_layer != nullptr) {
            m_previous_distortion_type = sdk::call_object_func_easy<uint32_t>(scene_layer, "get_DistortionType");
            m_set_next_post_effect_distortion_type = true;
            sdk::call_object_func_easy<void*>(scene_layer, "set_DistortionType", 0); // None
        }
    }

    return true;
}

void VR::on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
    if (!is_hmd_active()) {
        return;
    }

    if (!m_disable_post_effect_fix && m_set_next_post_effect_distortion_type) {
        auto scene_layer = layer->get_parent();

        // Restore the distortion type back to VR mode
        // to fix TAA
        if (scene_layer != nullptr) {
            sdk::call_object_func_easy<void*>(scene_layer, "set_DistortionType", m_previous_distortion_type); // Left
        }

        //mod->fix_temporal_effects();
        m_set_next_post_effect_distortion_type = false;
    }
}

bool VR::on_pre_post_effect_layer_update(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
    return true;
}

bool VR::on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    return true;
}

void VR::on_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) {
    if (!is_hmd_active()) {
        return;
    }

    if (!is_using_afw_foveated()) {
        return;
    }

    auto context = (sdk::renderer::RenderContext*)render_context;
    auto scene_layer = (sdk::renderer::layer::Scene*)layer->get_parent();

    if (scene_layer == nullptr) {
        return;
    }

    const auto target_state = layer->get_output_state();

    if (target_state == nullptr) {
        return;
    }

    const auto rtv = target_state->get_rtv(0);

    if (rtv == nullptr) {
        return;
    }

    const auto tex = rtv->get_texture_d3d12();

    if (tex == nullptr) {
        return;
    }

    auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();

    if (scene_layers.size() < 2) {
        return;
    }

    const auto parent_layer = layer->get_parent();

    if (parent_layer == nullptr) {
        return;
    }

    if (parent_layer == scene_layers[0]) {
        float w = (is_left_eye()) ? 0 : 1.0f;
        float red[4] = {1.0f, 0, 0, w};
        context->clear_rtv(rtv, red);
        // if (m_multipass.native_res_copies[0] != nullptr) {
        //     context->copy_texture(m_multipass.native_res_copies[0], tex);
        // }
    } else {
        // float w = (is_left_eye()) ? 0 : 1.0f;
        // float green[4] = {0, 1.0f, 0, w};
        // context->clear_rtv(rtv, green);
        // if (m_multipass.native_res_copies[1] != nullptr) {
        //    context->copy_texture(m_multipass.native_res_copies[1], tex);
        //}
    }
}

bool VR::on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    

    m_scene_update_mtx.lock();
    
    if (!is_hmd_active()) {
        return true;
    }

    if (!layer->is_fully_rendered()) {
        return true;
    }

    m_multipass.pass = 0;
    if (is_using_afw_foveated()) {
        const auto real_main_camera = sdk::get_primary_camera();
        const auto layer_camera = layer->get_main_camera_if_possible();

        if (layer_camera != nullptr) {
            if (layer_camera == real_main_camera) {
                m_multipass.pass = 0;
                m_multipass_cameras[0] = real_main_camera;
                m_multipass_cameras[1] = m_camera_duplicator.get_new_camera_counterpart(real_main_camera);
            } else if (layer_camera == m_camera_duplicator.get_new_camera_counterpart(real_main_camera)) {
                m_multipass.pass = 1;
                m_multipass_cameras[1] = m_camera_duplicator.get_new_camera_counterpart(real_main_camera);
            } else {
                return true; // dont care
            }
        } else {
            return true; // dont care
        }
    }
    
    auto scene_info = layer->get_scene_info();
    auto depth_distortion_scene_info = layer->get_depth_distortion_scene_info();
    auto filter_scene_info = layer->get_filter_scene_info();
    auto jitter_disable_scene_info = layer->get_jitter_disable_scene_info();
    auto z_prepass_scene_info = layer->get_z_prepass_scene_info();

    auto& layer_data = m_scene_layer_data[layer];

    layer_data[0].setup(scene_info);
    layer_data[1].setup(depth_distortion_scene_info);
    layer_data[2].setup(filter_scene_info);
    layer_data[3].setup(jitter_disable_scene_info);
    layer_data[4].setup(z_prepass_scene_info);

    return true;
}

void VR::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    

    utility::ScopeGuard ___([&]() {
        m_scene_update_mtx.unlock();
    });

    if (!is_hmd_active()) {
        return;
    }

    if (!layer->is_fully_rendered()) {
        return;
    }

    if (is_using_afw_foveated()) {
        const auto layer_camera = layer->get_camera();

        if (layer_camera != m_multipass_cameras[0] && layer_camera != m_multipass_cameras[1]) {
            return;
        }
    }
    auto eye_index = m_frame_count % 2 == m_left_eye_interval ? EyeLeft : EyeRight;
    auto other_eye_index = m_frame_count % 2 == m_left_eye_interval ? EyeRight : EyeLeft;

    bool is_foveated_pass = (is_using_afw_foveated() && m_multipass.pass == 0);

    if (is_foveated_pass && m_scene_layer_data[layer].size() > 0) {
        SceneLayerData& d = m_scene_layer_data[layer][0];
        oldViewMatrix[eye_index] = is_fix_dlss() ? d.old_view_matrix[eye_index] : d.old_view_matrix[other_eye_index];
        oldFoveatedProjectionMatrix[eye_index] = is_fix_dlss() ? get_runtime()->old_foveated_projections[1][eye_index]
                                                               : get_runtime()->old_foveated_projections[0][other_eye_index];
    }

    auto fix_motion_vectors = [&](SceneLayerData& d) {
        auto old_view_matrix = d.old_view_matrix[eye_index];
        auto old_projection_matrix = is_foveated_pass ? get_runtime()->old_foveated_projections[1][eye_index] : get_runtime()->projections[eye_index];
        d.scene_info->old_view_projection_matrix = old_projection_matrix * old_view_matrix;
    };

    auto& layer_data = m_scene_layer_data[layer];

    for (auto& d : layer_data) {
        if (d.scene_info != nullptr) {
            if (is_fix_dlss() && m_multipass.pass == 0) {
                fix_motion_vectors(d);
            } else if (!m_disable_temporal_fix) {
                // TAA fix
                d.scene_info->old_view_projection_matrix = d.view_projection_matrix;
            }
            if (m_multipass.pass == 0) {
                d.old_view_matrix[eye_index] = d.scene_info->view_matrix;
            }
        }
    }
}

void VR::wwise_listener_update_hook(void* listener) {
    auto original_func = g_wwise_listener_update_hook->get_original<decltype(VR::wwise_listener_update_hook)>();

    if (!g_framework->is_ready()) {
        original_func(listener);
        return;
    }

    auto& mod = VR::get();

    if (!mod->is_hmd_active() || !mod->get_runtime()->loaded) {
        original_func(listener);
        return;
    }

    if (!mod->m_hmd_oriented_audio->value()) {
        original_func(listener);
        return;
    }

    std::scoped_lock _{mod->m_wwise_mtx};

#if defined(RE2) || defined(RE3)
    const auto skip_camera_set = FirstPerson::get()->will_be_used();
#else
    const auto skip_camera_set = false;
#endif

    if (!skip_camera_set) {
        mod->update_audio_camera();
    }

#if TDB_VER > 49
    constexpr auto CAMERA_OFFSET = 0x50;
#else
    constexpr auto CAMERA_OFFSET = 0x58;
#endif

    auto& listener_camera = *(::REManagedObject**)((uintptr_t)listener + CAMERA_OFFSET);
    bool changed = false;

    if (listener_camera == nullptr) {
        auto primary_camera = sdk::get_primary_camera();

        if (primary_camera != nullptr) {
            listener_camera = primary_camera;
            changed = true;
        }
    }

    original_func(listener);

    if (changed) {
        listener_camera = nullptr;
    }

    if (!skip_camera_set) {
        mod->restore_audio_camera();
    }
}

// put it on the backburner
/*
float VR::get_sharpness_hook(void* tonemapping) {
    auto original_func = g_get_sharpness_hook->get_original<decltype(get_sharpness_hook)>();
    
    if (!g_framework->is_ready()) {
        return original_func(tonemapping);
    }

    auto& mod = VR::get();

    if (mod->m_disable_sharpening) {
        return 0.0f;
    }

    return original_func(tonemapping);
}
*/

NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID, NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle) {
    spdlog::info("hk_NVSDK_NGX_D3D12_CreateFeature FeatureID {}", (int)InFeatureID);
    auto result = o_NVSDK_NGX_D3D12_CreateFeature(InCmdList, InFeatureID, InParameters, OutHandle);
    const auto& vr = VR::get();
    int flag;
    InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flag);
    spdlog::info("hk_NVSDK_NGX_D3D12_CreateFeature 0x{0:x}", (INT64)result);
    if (vr->vrDLSSHandle[0] == NULL && (InFeatureID == NVSDK_NGX_Feature_SuperSampling || InFeatureID == NVSDK_NGX_Feature_RayReconstruction)) {
        vr->vrDLSSHandle[0] = *OutHandle;
        spdlog::info("Creating additional DLSS instance");
        auto result2 = o_NVSDK_NGX_D3D12_CreateFeature(InCmdList, InFeatureID, InParameters, &vr->vrDLSSHandle[1]);
        spdlog::info("Additional DLSS instance create result 0x{0:x}", (INT64)result2);
    }
    return result;
}

NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* InHandle) {
    spdlog::info("hk_NVSDK_NGX_D3D12_ReleaseFeature Starts");
    auto result = o_NVSDK_NGX_D3D12_ReleaseFeature(InHandle);
    spdlog::info("hk_NVSDK_NGX_D3D12_ReleaseFeature 0x{0:x}", (INT64)result);
    const auto& vr = VR::get();
    if (vr->vrDLSSHandle[0] == InHandle) {
        vr->vrDLSSHandle[0] = NULL;
        vr->depthDesc[0].pTexture = NULL;
        vr->depthDesc[1].pTexture = NULL;
        vr->uiBufferDesc[0].pTexture = NULL;
        vr->uiBufferDesc[1].pTexture = NULL;
        vr->uiBufferTex[0] = NULL;
        vr->uiBufferTex[1] = NULL;
        //vr->renderDepthDesc.pTexture = NULL;
        if (vr->vrDLSSHandle[1] != NULL) {
            auto result2 = o_NVSDK_NGX_D3D12_ReleaseFeature(vr->vrDLSSHandle[1]);
            spdlog::info("Additional DLSS instance release result 0x{0:x}", (INT64)result2);
            vr->vrDLSSHandle[1] = NULL;
        }
    }
    return result;
}

NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* InCmdList, const NVSDK_NGX_Handle* InFeatureHandle, NVSDK_NGX_Parameter* InParameters, void* InCallback) {
    const auto& vr = VR::get();
    if (InFeatureHandle == vr->vrDLSSHandle[0]) {
        ID3D12Resource* color;
        ID3D12Resource* depth;
        ID3D12Resource* motionVectors;
        ID3D12Resource* output;
        InParameters->Get(NVSDK_NGX_Parameter_Color, &color);
        InParameters->Get(NVSDK_NGX_Parameter_Depth, &depth);
        InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &motionVectors);
        InParameters->Get(NVSDK_NGX_Parameter_Output, &output);
        float mvScaleX;
        float mvScaleY;
        InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
        InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
        vr->rawMotionVectorsTex = motionVectors;
        if (vr->is_hmd_active() && motionVectors && vr->motionVectorsDesc.pTexture) {

            auto desc1 = motionVectors->GetDesc();
            auto desc2 = vr->motionVectorsDesc.pTexture->GetDesc();
            if ((abs((float)(desc1.Width - desc2.Width)) <= 2 && abs((float)(desc1.Height - desc2.Height)) <= 2 && desc1.Format == desc2.Format)) {
                TextureDesc src;
                src.pTexture = motionVectors;
                vr->d3d12Renderer->Copy(InCmdList, vr->motionVectorsDesc, src);
                if (vr->foveatedDepthDesc.pTexture) {
                    src.pTexture = depth;
                    vr->d3d12Renderer->Copy(InCmdList, vr->foveatedDepthDesc, src);
                }

                if (vr->is_fix_dlss()) {
                    EyeIndex nEye = (vr->get_vr_frame_count() % 2 == 0) ? EyeLeft : EyeRight;
                    auto currHandle = vr->vrDLSSHandle[0];
                    if (nEye == EyeLeft) {
                        currHandle = vr->vrDLSSHandle[0];
                    } else {
                        currHandle = vr->vrDLSSHandle[1];
                    }

                    // current rendered eye
                    auto result = o_NVSDK_NGX_D3D12_EvaluateFeature(InCmdList, currHandle, InParameters, InCallback);
                    if (NVSDK_NGX_FAILED(result)) {
                        spdlog::error("Failed DLSS 00, code = 0x{0:x}", result);
                    }
                    return result;
                }
            }
            else {
                spdlog::error("hk_NVSDK_NGX_D3D12_EvaluateFeature motionVectorsTex mistmatch! Desc1:{}x{} {}  Desc2:{}x{} {}", desc1.Width,
                    desc1.Height, (int)desc1.Format, desc2.Width, desc2.Height, (int)desc2.Format);
            }
        }
    }
    auto result = o_NVSDK_NGX_D3D12_EvaluateFeature(InCmdList, InFeatureHandle, InParameters, InCallback);
    return result;
}

ffxReturnCode_t hk_ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const void** memCb) {
    auto vr = VR::get();
    auto result = o_ffxCreateContext(context, desc, memCb);
    spdlog::info("hk_ffxCreateContext 0x{0:x}", (INT64)result);
    if (desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
        auto createDesc = reinterpret_cast<const ffxCreateContextDescUpscale*>(desc);
        vr->vrContexts[0] = *context;
        spdlog::info("Creating additional FSR context");
        auto result2 = o_ffxCreateContext(&vr->vrContexts[1], desc, NULL);
        spdlog::info("Additional FSR context create result 0x{0:x}", (INT64)result2);
    }
    return o_ffxCreateContext(context, desc, memCb);
}

ffxReturnCode_t hk_ffxDestroyContext(ffxContext* context, const void** memCb) {
    auto vr = VR::get();
    if (context == vr->vrContexts[0]) {
        spdlog::info("hk_ffxDestroyContext 0x{0:x}", (INT64)context);
        vr->vrContexts[0] = NULL;
        vr->depthDesc[0].pTexture = NULL;
        vr->depthDesc[1].pTexture = NULL;
        vr->uiBufferDesc[0].pTexture = NULL;
        vr->uiBufferDesc[1].pTexture = NULL;
        vr->uiBufferTex[0] = NULL;
        vr->uiBufferTex[1] = NULL;
        if (vr->vrContexts[1] != NULL) {
            auto result2 = o_ffxDestroyContext(&vr->vrContexts[1], NULL);
            spdlog::info("Additional FSR context release result 0x{0:x}", (INT64)result2);
            vr->vrContexts[1] = NULL;
        }
    }
    return o_ffxDestroyContext(context, memCb);
}

ffxReturnCode_t hk_ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    auto vr = VR::get();
    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        auto upscaleDesc = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);

        FfxApiResource depthApiResource = upscaleDesc->depth;
        FfxApiResource mvApiResource = upscaleDesc->motionVectors;
        float mvScaleX = upscaleDesc->motionVectorScale[0];
        float mvScaleY = upscaleDesc->motionVectorScale[1];
        if (vr->is_hmd_active() && depthApiResource.resource && mvApiResource.resource && vr->motionVectorsDesc.pTexture) {
            auto InCmdList = (ID3D12GraphicsCommandList*)upscaleDesc->commandList;
            auto depth = (ID3D12Resource*)depthApiResource.resource;
            auto motionVectors = (ID3D12Resource*)mvApiResource.resource;
            auto desc1 = motionVectors->GetDesc();
            auto desc2 = vr->motionVectorsDesc.pTexture->GetDesc();
            if ((desc1.Width == desc2.Width && desc1.Height == desc2.Height && desc1.Format == desc2.Format)) {
                TextureDesc src;
                src.pTexture = motionVectors;
                vr->d3d12Renderer->Copy(InCmdList, vr->motionVectorsDesc, src);
                if (vr->foveatedDepthDesc.pTexture) {
                    src.pTexture = depth;
                    vr->d3d12Renderer->Copy(InCmdList, vr->foveatedDepthDesc, src);
                }
                if (vr->is_fix_dlss()) {
                    EyeIndex nEye = (vr->get_vr_frame_count() % 2 == 0) ? EyeLeft : EyeRight;
                    auto currHandle = vr->vrContexts[0];
                    if (nEye == EyeLeft) {
                        currHandle = vr->vrContexts[0];
                    } else {
                        currHandle = vr->vrContexts[1];
                    }

                    // current rendered eye
                    auto result = o_ffxDispatch(&currHandle, desc);
                    if (result != 0) {
                        spdlog::error("Failed FSR 00, code = 0x{0:x}", result);
                    }
                    return result;
                }
            } else {
                spdlog::error("hk_ffxDispatch motionVectorsTex mistmatch! Desc1:{}x{} {}  Desc2:{}x{} {}", desc1.Width,
                    desc1.Height, (int)desc1.Format, desc2.Width, desc2.Height, (int)desc2.Format);
            }
        }
    }
    return o_ffxDispatch(context, desc);
}

static std::unordered_map<SIZE_T, ID3D12Resource*> rtvMap{};

decltype(&ID3D12GraphicsCommandList::ClearRenderTargetView) ptrClearRenderTargetView; // 48
void WINAPI hk_ID3D12GraphicsCommandList_ClearRenderTargetView(ID3D12GraphicsCommandList* This, D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT  ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects)
{
    const auto& vr = VR::get();
    if ((ColorRGBA[0] == 1.0f || ColorRGBA[1] == 1.0f) && rtvMap.contains(RenderTargetView.ptr)) {
        auto rtvTex = rtvMap[RenderTargetView.ptr];
        int eyeIndex = 0;
        if (ColorRGBA[0] == 1.0f) {
            //spdlog::info("Cleared RED! rtvTex = {:p}", (void*)rtvMap[RenderTargetView.ptr]);
            eyeIndex = ColorRGBA[3] == 0 ? 0 : 1;
        }
        else if (ColorRGBA[1] == 1.0f) {
            //spdlog::info("Cleared GREEN! rtvTex = {:p}", (void*)rtvMap[RenderTargetView.ptr]);
            eyeIndex = ColorRGBA[3] == 0 ? 0 : 1;
        }

        static TextureDesc tempBufferDesc[2];
        if (tempBufferDesc[eyeIndex].pTexture != rtvTex) {
            tempBufferDesc[eyeIndex].pTexture = rtvTex;
            tempBufferDesc[eyeIndex].initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            vr->d3d12Renderer->SetupTextureDesc(tempBufferDesc[eyeIndex]);
        }

        if (!vr->multipassBackupDesc[eyeIndex].pTexture) {
            auto desc = tempBufferDesc[eyeIndex].pTexture->GetDesc();
            vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, desc.Format, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, vr->multipassBackupDesc[eyeIndex], true);
        } else {
            auto desc1 = vr->multipassBackupDesc[eyeIndex].pTexture->GetDesc();
            auto desc2 = tempBufferDesc[eyeIndex].pTexture->GetDesc();
            if (desc1.Width != desc2.Width || desc1.Height != desc2.Height || desc1.Format != desc2.Format) {
                vr->d3d12Renderer->CreateTexture(desc2.Width, desc2.Height, desc2.Format, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, vr->multipassBackupDesc[eyeIndex], true);
            }
        }

        if (vr->multipassBackupDesc[eyeIndex].pTexture) {
            vr->d3d12Renderer->Copy(This, vr->multipassBackupDesc[eyeIndex], tempBufferDesc[eyeIndex]);
        }
        return;
    }

    return (This->*ptrClearRenderTargetView)(RenderTargetView, ColorRGBA, NumRects, pRects);
}

//decltype(&ID3D12GraphicsCommandList::OMSetRenderTargets) ptrOMSetRenderTargets; // 46
//void WINAPI hk_ID3D12GraphicsCommandList_OMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
//    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange,
//    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
//
//    const auto& vr = VR::get();
//    if (NumRenderTargetDescriptors > 0) {
//        for (int i = 0; i < NumRenderTargetDescriptors; i++) {
//            if (rtvMap.contains(pRenderTargetDescriptors[i].ptr) && rtvMap[pRenderTargetDescriptors[i].ptr] == vr->rawMotionVectorsTex) {
//                bool a = false;
//            }
//        }
//    }
//    (This->*ptrOMSetRenderTargets)(
//        NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
//}


//decltype(&ID3D12GraphicsCommandList::CopyTextureRegion) ptrCopyTextureRegion; // 16
//void WINAPI hk_ID3D12GraphicsCommandList_CopyTextureRegion(ID3D12GraphicsCommandList* This, const D3D12_TEXTURE_COPY_LOCATION* pDst,
//    UINT DstX, UINT DstY, UINT DstZ, _In_ const D3D12_TEXTURE_COPY_LOCATION* pSrc, _In_opt_ const D3D12_BOX* pSrcBox) {
//    const auto& vr = VR::get();
//    if (pDst->pResource == vr->m_multipass.eye_textures[0].Get() || pDst->pResource == vr->m_multipass.eye_textures[1].Get()) {
//        spdlog::info("Copy Resource dest eye texture {:p}", (void*)pDst->pResource);
//    }
//    if (pSrc->pResource == vr->m_multipass.native_textures[0].Get() || pSrc->pResource == vr->m_multipass.native_textures[1].Get()) {
//        spdlog::info("Copy Resource src native texture {:p}", (void*)pSrc->pResource);
//    }
//    return (This->*ptrCopyTextureRegion)(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
//}
//
//decltype(&ID3D12GraphicsCommandList::CopyResource) ptrCopyResource; // 17
//void WINAPI hk_ID3D12GraphicsCommandList_CopyResource(ID3D12GraphicsCommandList* This, ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) {
//    const auto& vr = VR::get();
//    if (pDstResource == vr->m_multipass.eye_textures[0].Get() || pDstResource == vr->m_multipass.eye_textures[1].Get()) {
//        spdlog::info("Copy Resource dest eye texture {:p}", (void*)pDstResource);
//    }
//    if (pSrcResource == vr->m_multipass.native_textures[0].Get() || pSrcResource == vr->m_multipass.native_textures[1].Get()) {
//        spdlog::info("Copy Resource src native texture {:p}", (void*)pSrcResource);
//    }
//    return (This->*ptrCopyResource)(pDstResource, pSrcResource);
//}

decltype(&ID3D12Device::CreateRenderTargetView) ptrCreateRenderTargetView; // 20
void WINAPI hk_ID3D12Device_CreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) 
{
    (This->*ptrCreateRenderTargetView)(pResource, pDesc, DestDescriptor);
    rtvMap[DestDescriptor.ptr] = pResource;
}

uintptr_t hookVtable(void* target, int index, void* detours) {
    uintptr_t* pVTable = *(uintptr_t**)target;
    DWORD dwOldProct = 0;
    BOOL bRet = ::VirtualProtect(pVTable, 4, PAGE_READWRITE, &dwOldProct);
    auto origFunc = pVTable[index];
    pVTable[index] = (uintptr_t)detours;
    return origFunc;
}

// Called when the mod is initialized
std::optional<std::string> VR::on_initialize_d3d_thread() try {

    // #############################
    // #Frame Warp Module Start
    // #############################
#if defined(PRAGMATA)
    // PragmataVR v0.9.0: PureDark's AFW plugin is delay-loaded and InitDevice below needs it even in Native mode. A missing
    // file would crash the game at startup; say what is wrong instead and run without VR. Also note the files that differ
    // from the tested versions (a game patch or a different plugin build may need an update of the mod).
    {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const auto game_dir = std::filesystem::path{exe_path}.parent_path();
        const auto plugin = game_dir / L"PDAFWPlugin.dll";
        std::error_code ec{};
        if (!std::filesystem::exists(plugin, ec) || LoadLibraryW(plugin.c_str()) == nullptr) {
            spdlog::error("[PragmataVR] PDAFWPlugin.dll is missing or cannot be loaded from {}. VR is off for this launch.", game_dir.string());
            MessageBoxW(nullptr,
                L"PDAFWPlugin.dll is missing from the PRAGMATA game folder, or Windows blocked it.\n\n"
                L"Extract the whole Pragmata VR zip into the game folder again (next to PRAGMATA.exe).\n"
                L"The game will start without VR this time.",
                L"Pragmata VR", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
            return "PDAFWPlugin.dll missing (see the Pragmata VR README)";
        }
        const auto plugin_size = std::filesystem::file_size(plugin, ec);
        if (!ec && plugin_size != 534528) {
            spdlog::warn("[PragmataVR] PDAFWPlugin.dll is {} bytes; Pragmata VR was tested with RE9 AFW beta.6 (534528 bytes)", plugin_size);
        }
        const auto exe_size = std::filesystem::file_size(std::filesystem::path{exe_path}, ec);
        if (!ec && exe_size != 378448800) {
            spdlog::warn("[PragmataVR] PRAGMATA.exe is {} bytes; Pragmata VR v0.9.0 was tested with game version 1.3.0.0 "
                         "(378448800 bytes). A game update may need a mod update.", exe_size);
        } else if (!ec) {
            spdlog::info("[PragmataVR] PRAGMATA.exe matches the tested game version 1.3.0.0");
        }
    }
#endif
    auto& hook = g_framework->get_d3d12_hook();
    hook->get_command_queue();
    pd::DeviceParams params{};
    params.d3d12Device = hook->get_device();
    params.d3d12Queue = hook->get_command_queue();
    d3d12Renderer = InitDevice(params);

    *(uintptr_t*)&ptrCreateRenderTargetView = hookVtable(params.d3d12Device, 20, hk_ID3D12Device_CreateRenderTargetView);

    auto cmdList = d3d12Renderer->BeginCommandList(0);
    //*(uintptr_t*)&ptrOMSetRenderTargets = hookVtable(cmdList, 46, hk_ID3D12GraphicsCommandList_OMSetRenderTargets);
    *(uintptr_t*)&ptrClearRenderTargetView = hookVtable(cmdList, 48, hk_ID3D12GraphicsCommandList_ClearRenderTargetView);
    //*(uintptr_t*)&ptrCopyTextureRegion = hookVtable(cmdList, 16, hk_ID3D12GraphicsCommandList_CopyTextureRegion);
    //*(uintptr_t*)&ptrCopyResource = hookVtable(cmdList, 17, hk_ID3D12GraphicsCommandList_CopyResource);
    d3d12Renderer->EndCommandList(0);

    if (auto mr = MH_Initialize(); mr != MH_OK) {
        spdlog::error("MH_Initialize failed: {}", (INT)mr);
    }
    
    auto dllNGX = GetModuleHandle("_nvngx.dll");
    if (!dllNGX)
        dllNGX = GetModuleHandle("nvngx.dll");
    if (!dllNGX) {
        spdlog::error("nvngx.dll not loaded!");
    } else {
        if (auto mr = MH_CreateHook(GetProcAddress(dllNGX, "NVSDK_NGX_D3D12_CreateFeature"), &hk_NVSDK_NGX_D3D12_CreateFeature,
                (PVOID*)&o_NVSDK_NGX_D3D12_CreateFeature);
            mr != MH_OK) {
            spdlog::error("Hook NVSDK_NGX_D3D12_CreateFeature Failed! {}", (INT)mr);
            return Mod::on_initialize();
        }
        if (auto mr = MH_CreateHook(GetProcAddress(dllNGX, "NVSDK_NGX_D3D12_ReleaseFeature"), &hk_NVSDK_NGX_D3D12_ReleaseFeature,
                (PVOID*)&o_NVSDK_NGX_D3D12_ReleaseFeature);
            mr != MH_OK) {
            spdlog::error("Hook NVSDK_NGX_D3D12_ReleaseFeature Failed! {}", (INT)mr);
            return Mod::on_initialize();
        }
        if (auto mr = MH_CreateHook(GetProcAddress(dllNGX, "NVSDK_NGX_D3D12_EvaluateFeature"), &hk_NVSDK_NGX_D3D12_EvaluateFeature,
                (PVOID*)&o_NVSDK_NGX_D3D12_EvaluateFeature);
            mr != MH_OK) {
            spdlog::error("Hook NVSDK_NGX_D3D12_EvaluateFeature Failed! {}", (INT)mr);
            return Mod::on_initialize();
        }
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
            throw std::runtime_error("UpscalerHooks::Install - Failed to enable hooks!");
    }
    auto dllFfx = GetModuleHandle("amd_fidelityfx_upscaler_dx12.dll");
    if (!dllFfx)
        dllFfx = GetModuleHandle("amd_fidelityfx_loader_dx12.dll");
    if (!dllFfx) {
        spdlog::error("amd_fidelityfx_upscaler_dx12.dll not loaded!");
    } else {
        if (auto mr = MH_CreateHook(GetProcAddress(dllFfx, "ffxCreateContext"), &hk_ffxCreateContext, (PVOID*)&o_ffxCreateContext);
            mr != MH_OK) {
            spdlog::error("Hook ffxCreateContext Failed! {}", (INT)mr);
            return Mod::on_initialize();
        }
        if (auto mr = MH_CreateHook(GetProcAddress(dllFfx, "ffxDestroyContext"), &hk_ffxDestroyContext, (PVOID*)&o_ffxDestroyContext);
            mr != MH_OK) {
            spdlog::error("Hook ffxDestroyContext Failed! {}", (INT)mr);
            return Mod::on_initialize();
        }
        if (auto mr = MH_CreateHook(GetProcAddress(dllFfx, "ffxDispatch"), &hk_ffxDispatch, (PVOID*)&o_ffxDispatch);
            mr != MH_OK) {
            spdlog::error("Hook ffxDispatch Failed! {}", (INT)mr);
            return Mod::on_initialize();
        }
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
            throw std::runtime_error("UpscalerHooks::Install - Failed to enable hooks!");
    }

    // #############################
    // #Frame Warp Module End
    // #############################

    auto openvr_error = initialize_openvr();

    if (openvr_error || !m_openvr->loaded) {
        if (m_openvr->error) {
            spdlog::info("OpenVR failed to load: {}", *m_openvr->error);
        }

        m_openvr->is_hmd_active = false;
        m_openvr->was_hmd_active = false;
        m_openvr->needs_pose_update = false;

        // Attempt to load OpenXR instead
        auto openxr_error = initialize_openxr();

        if (openxr_error || !m_openxr->loaded) {
            m_openxr->needs_pose_update = false;
        }
    } else {
        m_openxr->error = 
R"(OpenVR loaded first.
If you want to use OpenXR, remove the openvr_api.dll from your game folder, 
and place the openxr_loader.dll in the same folder.)";
    }

    if (!get_runtime()->loaded) {
        // this is okay. we're not going to fail the whole thing entirely
        // so we're just going to return OK, but
        // when the VR mod draws its menu, it'll say "VR is not available"
        return Mod::on_initialize();
    }

    // Check whether the user has Hardware accelerated GPU scheduling enabled
    const auto hw_schedule_value = utility::get_registry_dword(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        "HwSchMode");

    if (hw_schedule_value) {
        m_has_hw_scheduling = *hw_schedule_value == 2;
    }

    auto hijack_error = hijack_resolution();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_input();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_camera();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_wwise_listeners();

    if (hijack_error) {
        return hijack_error;
    }

    const auto renderer_t = sdk::find_type_definition("via.render.Renderer");

    if (renderer_t != nullptr) {
        const auto set_hdr_method = renderer_t->get_method("set_HDRMode");

        if (set_hdr_method != nullptr) {
            spdlog::info("Hooking setHDRMode");
            g_hookman.add(set_hdr_method, &pre_set_hdr_mode, &post_set_hdr_mode);
        }
    }

    m_init_finished = true;

    // all OK
    return Mod::on_initialize();
} catch(...) {
    spdlog::error("Exception occurred in VR::on_initialize()");

    m_runtime->error = "Exception occurred in VR::on_initialize()";
    m_openxr->dll_missing = false;
    m_openvr->dll_missing = false;
    m_openxr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->loaded = false;
    m_openvr->is_hmd_active = false;
    m_openxr->loaded = false;
    m_init_finished = false;

    return Mod::on_initialize();
}

void VR::on_lua_state_created(sol::state& lua) {
    lua.new_usertype<VR>("VR",
        "get_controllers", &VR::get_controllers,
        "get_position", &VR::get_position,
        "get_velocity", &VR::get_velocity,
        "get_angular_velocity", &VR::get_angular_velocity,
        "get_rotation", &VR::get_rotation,
        "get_transform", &VR::get_transform,
        "get_left_stick_axis", &VR::get_left_stick_axis,
        "get_right_stick_axis", &VR::get_right_stick_axis,
        "get_current_eye_transform", &VR::get_current_eye_transform,
        "get_current_projection_matrix", &VR::get_current_projection_matrix,
        "get_standing_origin", &VR::get_standing_origin,
        "set_standing_origin", &VR::set_standing_origin,
        "get_rotation_offset", &VR::get_rotation_offset,
        "set_rotation_offset", &VR::set_rotation_offset,
        "recenter_view", &VR::recenter_view,
        "get_gui_rotation_offset", &VR::get_gui_rotation_offset,
        "set_gui_rotation_offset", &VR::set_gui_rotation_offset,
        "recenter_gui", &VR::recenter_gui,
        "get_action_set", &VR::get_action_set,
        "get_active_action_set", &VR::get_active_action_set,
        "get_action_trigger", &VR::get_action_trigger,
        "get_action_grip", &VR::get_action_grip,
        "get_action_joystick", &VR::get_action_joystick,
        "get_action_joystick_click", &VR::get_action_joystick_click,
        "get_action_joystick_touch", &VR::get_action_joystick_touch,
        "get_action_trigger_touch", &VR::get_action_trigger_touch,
        "get_action_a_button_touch", &VR::get_action_a_button_touch,
        "get_action_b_button_touch", &VR::get_action_b_button_touch,
        "get_action_thumbrest_touch", &VR::get_action_thumbrest_touch,
        "get_action_touchpad_touch", &VR::get_action_touchpad_touch,
        "get_action_a_button", &VR::get_action_a_button,
        "get_action_b_button", &VR::get_action_b_button,
        "get_action_weapon_dial", &VR::get_action_weapon_dial,
        "get_action_minimap", &VR::get_action_minimap,
        "get_action_block", &VR::get_action_block,
        "get_action_dpad_up", &VR::get_action_dpad_up,
        "get_action_dpad_down", &VR::get_action_dpad_down,
        "get_action_dpad_left", &VR::get_action_dpad_left,
        "get_action_dpad_right", &VR::get_action_dpad_right,
        "get_action_heal", &VR::get_action_heal,
        "get_left_joystick", &VR::get_left_joystick,
        "get_right_joystick", &VR::get_right_joystick,
        "is_using_controllers", &VR::is_using_controllers,
        "is_openvr_loaded", &VR::is_openvr_loaded,
        "is_openxr_loaded", &VR::is_openxr_loaded,
        "is_hmd_active", &VR::is_hmd_active,
        "is_action_active", &VR::is_action_active,
        "is_using_hmd_oriented_audio", &VR::is_using_hmd_oriented_audio,
        "toggle_hmd_oriented_audio", &VR::toggle_hmd_oriented_audio,
        "apply_hmd_transform", [](VR* vr, glm::quat& rotation, Vector4f& position) {
            vr->apply_hmd_transform(rotation, position);
        },
        "trigger_haptic_vibration", &VR::trigger_haptic_vibration,
        "get_last_render_matrix", &VR::get_last_render_matrix,
        "should_handle_pause", [](VR* vr) { 
            return vr->get_runtime()->handle_pause;
        },
        "set_handle_pause", [](VR* vr, bool state) { 
            return vr->get_runtime()->handle_pause = state;
        },
        "unhide_crosshair", &VR::unhide_crosshair
    );

    lua["vrmod"] = this;
}

std::optional<std::string> VR::initialize_openvr() {
    m_openvr = std::make_shared<runtimes::OpenVR>();
    m_openvr->loaded = false;

    if (utility::load_module_from_current_directory(L"openvr_api.dll") == nullptr) {
        spdlog::info("[VR] Could not load openvr_api.dll");

        m_openvr->dll_missing = true;
        m_openvr->error = "Could not load openvr_api.dll";
        return Mod::on_initialize();
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openvr->needs_pose_update = true;
    m_openvr->got_first_poses = false;
    m_openvr->is_hmd_active = true;
    m_openvr->was_hmd_active = true;

    auto error = vr::VRInitError_None;
	m_openvr->hmd = vr::VR_Init(&error, vr::VRApplication_Scene);

    // check if error
    if (error != vr::VRInitError_None) {
        m_openvr->error = "VR_Init failed: " + std::string{vr::VR_GetVRInitErrorAsEnglishDescription(error)};
        return Mod::on_initialize();
    }

    if (m_openvr->hmd == nullptr) {
        m_openvr->error = "VR_Init failed: HMD is null";
        return Mod::on_initialize();
    }

    // get render target size
    m_openvr->update_render_target_size();

    if (vr::VRCompositor() == nullptr) {
        m_openvr->error = "VRCompositor failed to initialize.";
        return Mod::on_initialize();
    }

    auto input_error = initialize_openvr_input();

    if (input_error) {
        m_openvr->error = *input_error;
        return Mod::on_initialize();
    }

    auto overlay_error = m_overlay_component.on_initialize_openvr();

    if (overlay_error) {
        m_openvr->error = *overlay_error;
        return Mod::on_initialize();
    }
    
    m_openvr->loaded = true;
    m_openvr->error = std::nullopt;
    m_runtime = m_openvr;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr_input() {
    const auto module_directory = REFramework::get_persistent_dir();

    // write default actions and bindings with the static strings we have
    for (auto& it : m_binding_files) {
        spdlog::info("Writing default binding file {}", it.first);

        std::ofstream file{ module_directory / it.first };
        file << it.second;
    }

    const auto actions_path = module_directory / "actions.json";
    auto input_error = vr::VRInput()->SetActionManifestPath(actions_path.string().c_str());

    if (input_error != vr::VRInputError_None) {
        return "VRInput failed to set action manifest path: " + std::to_string((uint32_t)input_error);
    }

    // get action set
    auto action_set_error = vr::VRInput()->GetActionSetHandle("/actions/default", &m_action_set);

    if (action_set_error != vr::VRInputError_None) {
        return "VRInput failed to get action set: " + std::to_string((uint32_t)action_set_error);
    }

    if (m_action_set == vr::k_ulInvalidActionSetHandle) {
        return "VRInput failed to get action set handle.";
    }

    for (auto& it : m_action_handles) {
        auto error = vr::VRInput()->GetActionHandle(it.first.c_str(), &it.second.get());

        if (error != vr::VRInputError_None) {
            return "VRInput failed to get action handle: (" + it.first + "): " + std::to_string((uint32_t)error);
        }

        if (it.second == vr::k_ulInvalidActionHandle) {
            return "VRInput failed to get action handle: (" + it.first + ")";
        }
    }

    m_active_action_set.ulActionSet = m_action_set;
    m_active_action_set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    m_active_action_set.nPriority = 0;

    detect_controllers();

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr() {
    m_openxr = std::make_shared<runtimes::OpenXR>();

    spdlog::info("[VR] Initializing OpenXR");

    if (utility::load_module_from_current_directory(L"openxr_loader.dll") == nullptr) {
        spdlog::info("[VR] Could not load openxr_loader.dll");

        m_openxr->loaded = false;
        m_openxr->error = "Could not load openxr_loader.dll";

        return std::nullopt;
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openxr->needs_pose_update = true;
    m_openxr->got_first_poses = false;

    // Step 1: Create an instance
    spdlog::info("[VR] Creating OpenXR instance");

    XrResult result{XR_SUCCESS};

    // We may just be restarting OpenXR, so try to find an existing instance first
    if (m_openxr->instance == XR_NULL_HANDLE) {
        std::vector<const char*> extensions{};

        if (g_framework->is_dx12()) {
            extensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        } else {
            extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        }
        XrResult res;
        uint32_t extCount = 0;

        res = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
        assert(res == XR_SUCCESS);

        std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
        res = xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
        assert(res == XR_SUCCESS);

        bool supportsEyeGaze = false;
        for (const auto& ext : exts) {
            if (strcmp(ext.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) {
                supportsEyeGaze = true;
                break;
            }
        }

        if (!supportsEyeGaze) {
            spdlog::info("[VR] OpenXR no eye gaze support!");
        } else {
            extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        }

        XrInstanceCreateInfo instance_create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_create_info.next = nullptr;
        instance_create_info.enabledExtensionCount = (uint32_t)extensions.size();
        instance_create_info.enabledExtensionNames = extensions.data();

        strcpy(instance_create_info.applicationInfo.applicationName, g_framework->get_game_name());
        instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        
        result = xrCreateInstance(&instance_create_info, &m_openxr->instance);

        // we can't convert the result to a string here
        // because the function requires the instance to be valid
        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr instance: " + std::to_string((int32_t)result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }

    } else {
        spdlog::info("[VR] Found existing openxr instance");
    }
    
    // Step 2: Create a system
    spdlog::info("[VR] Creating OpenXR system");

    // We may just be restarting OpenXR, so try to find an existing system first
    if (m_openxr->system == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = m_openxr->form_factor;

        result = xrGetSystem(m_openxr->instance, &system_info, &m_openxr->system);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr system: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr system");
    }

    // Step 3: Create a session
    spdlog::info("[VR] Initializing graphics info");

    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};

    if (g_framework->is_dx12()) {
        m_d3d12.openxr().initialize(session_create_info);
    } else {
        m_d3d11.openxr().initialize(session_create_info);
    }

    spdlog::info("[VR] Creating OpenXR session");
    session_create_info.systemId = m_openxr->system;
    result = xrCreateSession(m_openxr->instance, &session_create_info, &m_openxr->session);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not create openxr session: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    XrSystemEyeGazeInteractionPropertiesEXT eyeGazeProps = {.type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, .next = nullptr};
    XrSystemProperties systemProps = {.type = XR_TYPE_SYSTEM_PROPERTIES, .next = &eyeGazeProps};
    xrGetSystemProperties(m_openxr->instance, m_openxr->system, &systemProps);
    m_openxr->supportsEyeGazeInteraction = eyeGazeProps.supportsEyeGazeInteraction;

    // Step 4: Create a space
    spdlog::info("[VR] Creating OpenXR space");

    // We may just be restarting OpenXR, so try to find an existing space first

    if (m_openxr->stage_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->stage_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr stage space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    if (m_openxr->view_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->view_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr view space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    // Step 5: Get the system properties
    spdlog::info("[VR] Getting OpenXR system properties");

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    result = xrGetSystemProperties(m_openxr->instance, m_openxr->system, &system_properties);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not get system properties: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    spdlog::info("[VR] OpenXR system Name: {}", system_properties.systemName);
    spdlog::info("[VR] OpenXR system Vendor: {}", system_properties.vendorId);
    spdlog::info("[VR] OpenXR system max width: {}", system_properties.graphicsProperties.maxSwapchainImageWidth);
    spdlog::info("[VR] OpenXR system max height: {}", system_properties.graphicsProperties.maxSwapchainImageHeight);
    spdlog::info("[VR] OpenXR system supports {} layers", system_properties.graphicsProperties.maxLayerCount);
    spdlog::info("[VR] OpenXR system orientation: {}", system_properties.trackingProperties.orientationTracking);
    spdlog::info("[VR] OpenXR system position: {}", system_properties.trackingProperties.positionTracking);

    // Step 6: Get the view configuration properties
    m_openxr->update_render_target_size();

    // Step 7: Create a view
    if (!m_openxr->view_configs.empty()){
        m_openxr->views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
        m_openxr->stage_views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
    }

    if (m_openxr->view_configs.empty()) {
        m_openxr->error = "No view configurations found";
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->loaded = true;
    m_runtime = m_openxr;

    if (auto err = initialize_openxr_input()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    detect_controllers();

    if (m_init_finished) {
        // This is usually done in on_config_load
        // but the runtime can be reinitialized, so we do it here instead
        initialize_openxr_swapchains();
    }

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_input() {
    if (auto err = m_openxr->initialize_actions(VR::actions_json)) {
        m_openxr->error = err.value();
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }
    
    for (auto& it : m_action_handles) {
        auto openxr_action_name = m_openxr->translate_openvr_action_name(it.first);

        if (m_openxr->action_set.action_map.contains(openxr_action_name)) {
            it.second.get() = (decltype(it.second)::type)m_openxr->action_set.action_map[openxr_action_name];
            spdlog::info("[VR] Successfully mapped action {} to {}", it.first, openxr_action_name);
        }
    }

    m_left_joystick = (decltype(m_left_joystick))VRRuntime::Hand::LEFT;
    m_right_joystick = (decltype(m_right_joystick))VRRuntime::Hand::RIGHT;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_swapchains() {
    // This depends on the config being loaded.
    if (!m_init_finished) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating OpenXR swapchain");

    if (g_framework->is_dx12()) {
        auto err = m_d3d12.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());

            return m_openxr->error;
        }
    } else {
        auto err = m_d3d11.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());
            return m_openxr->error;
        }
    }

    return std::nullopt;
}

std::optional<std::string> VR::hijack_resolution() {
    // moved to global hook class
    return std::nullopt;
}

std::optional<std::string> VR::hijack_input() {
#if defined(RE2) || defined(RE3)
    spdlog::info("[VR] Hijacking InputSystem");

    // We're going to hook InputSystem.update so we can
    // override the analog stick values with the VR controller's
    auto func = sdk::find_native_method(game_namespace("InputSystem"), "update");

    if (func == nullptr) {
        return "VR init failed: InputSystem.update function not found.";
    }

    spdlog::info("InputSystem.update: {:x}", (uintptr_t)func);

    // Hook the native function
    g_input_hook = std::make_unique<FunctionHook>(func, inputsystem_update_hook);

    if (!g_input_hook->create()) {
        return "VR init failed: InputSystem.update native function hook failed.";
    }
#endif

    return std::nullopt;
}

std::optional<std::string> VR::hijack_camera() {
    spdlog::info("[VR] Hijacking Camera");

    const auto get_projection_matrix = (uintptr_t)sdk::find_native_method("via.Camera", "get_ProjectionMatrix");

    ///////////////////////////////
    // Hook GUI camera projection matrix start
    ///////////////////////////////
    auto func = sdk::find_native_method("via.gui.GUICamera", "get_ProjectionMatrix");

    if (func != nullptr) {
        spdlog::info("via.gui.GUICamera.get_ProjectionMatrix: {:x}", (uintptr_t)func);
        
        // Pattern scan for the native function call
        auto ref = utility::scan((uintptr_t)func, 0x100, "49 8B C8 E8");
        size_t call_displacement_offset = 4;

#if defined(PRAGMATA)
        // PragmataVR: Pragmata's GUICamera.get_ProjectionMatrix is a small wrapper that moves the
        // camera from R8 to RCX with the "4C 89 C1" encoding of mov rcx, r8, so the stock
        // "49 8B C8 E8" scan never matches and the GUI camera keeps the flat-screen projection.
        // That leaves Pragmata's 3D HUD panels (hacking panel, health, weapon wheel, tutorials)
        // far out at the edges of each eye. Based on the TokyoTKO/WinterWolf V18 fix, made
        // tolerant of small layout changes after game patches.
        {
            std::string bytes{};
            const auto code = reinterpret_cast<const uint8_t*>(func);
            for (size_t i = 0; i < 0x40; ++i) {
                bytes += fmt::format("{:02x}", code[i]);
            }
            spdlog::info("[PragmataVR] GUICamera wrapper bytes: {}", bytes);
        }

        if (!ref) {
            // Exact wrapper seen on the 2026-09 build: push rsi; sub rsp,20; mov rsi,rcx;
            // mov rcx,r8; mov rdx,rsi; call native; mov rax,rsi; add rsp,20; pop rsi; ret
            auto exact = utility::scan((uintptr_t)func, 0x20,
                "56 48 83 EC 20 48 89 CE 4C 89 C1 48 89 F2 E8 ? ? ? ? 48 89 F0 48 83 C4 20 5E C3");

            if (exact && *exact == (uintptr_t)func) {
                ref = exact;
                call_displacement_offset = 15;
                spdlog::info("[PragmataVR] GUICamera wrapper matched exact pattern");
            } else {
                // Looser fallback: "mov rcx, r8" (4C 89 C1) followed within 16 bytes by a call.
                auto mov = utility::scan((uintptr_t)func, 0x40, "4C 89 C1");

                if (mov) {
                    for (uintptr_t p = *mov + 3; p < *mov + 3 + 16; ++p) {
                        if (*reinterpret_cast<const uint8_t*>(p) == 0xE8) {
                            ref = p;
                            call_displacement_offset = 1;
                            spdlog::info("[PragmataVR] GUICamera wrapper matched loose pattern, call at +0x{:x}", p - (uintptr_t)func);
                            break;
                        }
                    }
                }

                if (!ref) {
                    spdlog::error("[PragmataVR] GUICamera wrapper not recognized; GUI projection left unhooked");
                }
            }
        }
#endif

        if (ref) {
            auto native_func = utility::calculate_absolute(*ref + call_displacement_offset);
            spdlog::info("[VR] GUICamera native projection target: {:x}", native_func);

            if (native_func != get_projection_matrix) {
                // Hook the native function
                g_projection_matrix_hook2 = std::make_unique<FunctionHook>(native_func, gui_camera_get_projection_matrix_hook);

                if (g_projection_matrix_hook2->create()) {
                    spdlog::info("Hooked via.gui.GUICamera.get_ProjectionMatrix");
                } else {
                    spdlog::error("[VR] Failed to hook via.gui.GUICamera.get_ProjectionMatrix native target");
                }
            } else {
                spdlog::info("Did not hook via.gui.GUICamera.get_ProjectionMatrix, same as via.Camera.get_ProjectionMatrix");
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> VR::hijack_wwise_listeners() {
#ifndef RE4
#ifndef SF6
#if TDB_VER < 73
    spdlog::info("[VR] Hijacking WwiseListener");

    const auto t = sdk::find_type_definition("via.wwise.WwiseListener");

    if (t == nullptr) {
        return "VR init failed: via.wwise.WwiseListener type not found.";
    }

    const auto update_method = t->get_method("update");

    if (update_method == nullptr) {
        return "VR init failed: via.wwise.WwiseListener.update method not found.";
    }

    const auto func_wrapper = update_method->get_function();

    if (func_wrapper == nullptr) {
        return "VR init failed: via.wwise.WwiseListener.update native function not found.";
    }
    
    spdlog::info("via.wwise.WwiseListener.update: {:x}", (uintptr_t)func_wrapper);
    
    // Use hde to disassemble the method and find the first jmp, which jmps to the real function
    // in the vtable
    const auto jmp = utility::scan_disasm((uintptr_t)func_wrapper, 10, "48 FF");

    if (!jmp) {
        return "VR init failed: could not find jmp opcode in via.wwise.WwiseListener.update native function.";
    }

    const auto vtable_index = *(uint8_t*)(*jmp + 3) / sizeof(void*);
    spdlog::info("via.wwise.WwiseListener.update vtable index: {}", vtable_index);
    spdlog::info("Attempting to create fake via.wwise.WwiseListener instance");

    const void* fake_obj = t->create_instance_full();

    if (fake_obj == nullptr) {
        return "VR init failed: Failed to create fake via.wwise.WwiseListener instance.";
    }
    
    spdlog::info("Attempting to read vtable from fake via.wwise.WwiseListener instance");
    auto obj_vtable = *(void***)fake_obj;

    if (obj_vtable == nullptr) {
        return "VR init failed: via.wwise.WwiseListener vtable not found.";
    }

    spdlog::info("via.wwise.WwiseListener vtable: {:x}", (uintptr_t)obj_vtable - g_framework->get_module());

    auto update_native = obj_vtable[vtable_index];

    if (update_native == 0) {
        return "VR init failed: via.wwise.WwiseListener update native not found.";
    }

    spdlog::info("via.wwise.WwiseListener.update: {:x}", (uintptr_t)update_native);

    g_wwise_listener_update_hook = std::make_unique<FunctionHook>(update_native, wwise_listener_update_hook);

    if (!g_wwise_listener_update_hook->create()) {
        return "VR init failed: via.wwise.WwiseListener update native function hook failed.";
    }
#endif
#endif
#endif

    return std::nullopt;
}

bool VR::detect_controllers() {
    // already detected
    if (!m_controllers.empty()) {
        return true;
    }

    if (get_runtime()->is_openvr()) {
        auto left_joystick_origin_error = vr::EVRInputError::VRInputError_None;
        auto right_joystick_origin_error = vr::EVRInputError::VRInputError_None;

        vr::InputOriginInfo_t left_joystick_origin_info{};
        vr::InputOriginInfo_t right_joystick_origin_info{};

        // Get input origin info for the joysticks
        // get the source input device handles for the joysticks
        auto left_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_left_joystick);

        if (left_joystick_error != vr::VRInputError_None) {
            return false;
        }

        auto right_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_right_joystick);

        if (right_joystick_error != vr::VRInputError_None) {
            return false;
        }

        left_joystick_origin_info = {};
        right_joystick_origin_info = {};

        left_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_left_joystick, &left_joystick_origin_info, sizeof(left_joystick_origin_info));
        right_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_right_joystick, &right_joystick_origin_info, sizeof(right_joystick_origin_info));
        if (left_joystick_origin_error != vr::EVRInputError::VRInputError_None || right_joystick_origin_error != vr::EVRInputError::VRInputError_None) {
            return false;
        }

        // Instead of manually going through the devices,
        // We do this. The order of the devices isn't always guaranteed to be
        // Left, and then right. Using the input state handles will always
        // Get us the correct device indices.
        m_controllers.push_back(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers.push_back(right_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(right_joystick_origin_info.trackedDeviceIndex);

        spdlog::info("Left Hand: {}", left_joystick_origin_info.trackedDeviceIndex);
        spdlog::info("Right Hand: {}", right_joystick_origin_info.trackedDeviceIndex);
    } else if (get_runtime()->is_openxr()) {
        // ezpz
        m_controllers.push_back(1);
        m_controllers.push_back(2);
        m_controllers_set.insert(1);
        m_controllers_set.insert(2);

        spdlog::info("Left Hand: {}", 1);
        spdlog::info("Right Hand: {}", 2);
    }


    return true;
}

bool VR::is_any_action_down() {
    if (!m_runtime->ready() || !is_using_controllers()) {
        return false;
    }

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();

    if (glm::length(left_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    if (glm::length(right_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    for (auto& it : m_action_handles) {
        if (is_action_active(it.second, m_left_joystick) || is_action_active(it.second, m_right_joystick)) {
            return true;
        }
    }

    return false;
}

void VR::update_hmd_state() {
    

    auto runtime = get_runtime();
    
    if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::EARLY) {
        if (runtime->is_openxr()) {
            if (g_framework->get_renderer_type() == REFramework::RendererType::D3D11) {
                if (!runtime->got_first_sync || runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                    return;
                }  
            } else if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                return;
            }

            m_openxr->begin_frame();
        } else {
            if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                return;
            }
        }
    }
    
    runtime->update_poses();

    // Update the poses used for the game
    // If we used the data directly from the WaitGetPoses call, we would have to lock a different mutex and wait a long time
    // This is because the WaitGetPoses call is blocking, and we don't want to block any game logic
    if (runtime->wants_reset_origin && runtime->ready() && runtime->got_first_valid_poses) {
        std::unique_lock _{ runtime->pose_mtx };
        set_rotation_offset(glm::identity<glm::quat>());
        m_standing_origin = get_position_unsafe(vr::k_unTrackedDeviceIndex_Hmd);

        runtime->wants_reset_origin = false;
    }

    runtime->update_matrices(m_nearz, m_farz);

    runtime->got_first_poses = true;

    // Forcefully update the camera transform after submitting the frame
    // because the game logic thread does not run in sync with the rendering thread
    // This will massively improve HMD rotation smoothness for the user
    // if this is not done, the left eye will jitter a lot
#if defined(RE2) || defined(RE3)
    const auto cameras = get_cameras();

    for (auto camera : cameras) {
        if (camera == nullptr) {
            continue;
        }

        if (camera->ownerGameObject != nullptr && camera->ownerGameObject->transform != nullptr) {
            if (camera == cameras[0]) {
                FirstPerson::get()->on_update_transform(camera->ownerGameObject->transform);
            } else if (cameras[0] != nullptr && cameras[0]->ownerGameObject != nullptr && cameras[0]->ownerGameObject->transform != nullptr) {
                auto transform0 = cameras[0]->ownerGameObject->transform;
                auto transform1 = camera->ownerGameObject->transform;

                const auto camera_joint = utility::re_transform::get_joint(*transform1, 0);

                if (camera_joint == nullptr) {
                    continue;
                }

                auto& fp = FirstPerson::get();

                const auto mat = fp->get_last_camera_matrix();
                const auto pos = mat[3];

                //transform1->angles = transform0->angles;
                //transform1->position = transform0->position;
                transform1->worldTransform = transform0->worldTransform;

                sdk::set_joint_position(camera_joint, pos);
                sdk::set_joint_rotation(camera_joint, mat);
            }
        }
    }
#endif
}

void VR::update_action_states() {
    

    auto runtime = get_runtime();

    if (runtime->wants_reinitialize) {
        return;
    }

    if (runtime->is_openvr()) {
        const auto start_time = std::chrono::high_resolution_clock::now();

        auto error = vr::VRInput()->UpdateActionState(&m_active_action_set, sizeof(m_active_action_set), 1);

        if (error != vr::VRInputError_None) {
            spdlog::error("VRInput failed to update action state: {}", (uint32_t)error);
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto time_delta = end_time - start_time;

        m_last_input_delay = time_delta;
        m_avg_input_delay = (m_avg_input_delay + time_delta) / 2;

        if ((end_time - start_time) >= std::chrono::milliseconds(30)) {
            spdlog::warn("VRInput update action state took too long: {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

            //reinitialize_openvr();
            runtime->wants_reinitialize = true;
        }   
    } else {
        get_runtime()->update_input();
    }

    if (m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (m_set_standing_key->is_key_down_once()) {
        set_standing_origin(get_position(0));
    }
}

void VR::update_camera() {
    

    if (!is_hmd_active()) {
        m_needs_camera_restore = false;
        return;
    }

    if (inside_on_end) {
        return;
    }

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        spdlog::error("VR: Failed to get primary camera!");
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            break;
        }

        static auto via_camera = sdk::find_type_definition("via.Camera");
        static auto get_near_clip_plane_method = via_camera->get_method("get_NearClipPlane");
        static auto get_far_clip_plane_method = via_camera->get_method("get_FarClipPlane");
        static auto set_far_clip_plane_method = via_camera->get_method("set_FarClipPlane");
        static auto set_fov_method = via_camera->get_method("set_FOV");
        static auto set_vertical_enable_method = via_camera->get_method("set_VerticalEnable");
        static auto set_aspect_ratio_method = via_camera->get_method("set_AspectRatio");

        if (m_use_custom_view_distance->value()) {
            set_far_clip_plane_method->call<void*>(sdk::get_thread_context(), camera, m_view_distance->value());
        }

        m_nearz = get_near_clip_plane_method->call<float>(sdk::get_thread_context(), camera);
        m_farz = get_far_clip_plane_method->call<float>(sdk::get_thread_context(), camera);

        // Disable certain effects like the 3D overlay during the sewer gators
        // section in RE7
        disable_bad_effects();
        // Disable lens distortion
        set_lens_distortion(false);

        
        const auto camera_gameobject = utility::re_component::get_game_object((REComponent*)camera);
        if (camera_gameobject != nullptr) {
            const auto component = utility::re_component::find<REComponent>(camera_gameobject->transform, "via.render.VolumetricFogControl");
            if (component != nullptr) {
                const auto t = utility::re_managed_object::get_type_definition(component);
                const auto t_name_fnv = utility::hash(t->get_full_name());
                auto ctx = sdk::get_thread_context();
                if (t_name_fnv == "via.render.VolumetricFogControl"_fnv) {
                    static auto get_enabled = t->get_method("get_Enabled");
                    static auto set_enabled = t->get_method("set_Enabled");
                    if (get_enabled != nullptr && set_enabled != nullptr) {
                        const auto enabled = get_enabled->call<bool>(ctx, component);
                        if (enabled == m_disable_volumetric_fog->value()) {
                            set_enabled->call(ctx, component, !m_disable_volumetric_fog->value());
                        }
                    }
                }
            }
        }

    #if defined(RE2) || defined(RE3)
        if (FirstPerson::get()->will_be_used()) {
            m_needs_camera_restore = false;

            auto camera_object = utility::re_component::get_game_object(camera);

            if (camera_object == nullptr || camera_object->transform == nullptr) {
                return;
            }

            auto camera_joint = utility::re_transform::get_joint(*camera_object->transform, 0);

            if (camera_joint == nullptr) {
                return;
            }

            m_original_camera_position = sdk::get_joint_position(camera_joint);
            m_original_camera_rotation = sdk::get_joint_rotation(camera_joint);
            m_original_camera_matrix = Matrix4x4f{m_original_camera_rotation};
            m_original_camera_matrix[3] = m_original_camera_position;

            return;
        }
    #endif

        auto projection_matrix = is_using_afw_foveated() ? get_projection_matrix(0) : get_current_projection_matrix(true);

        // Steps towards getting lens flares and volumetric lighting working
        // Get the FOV from the projection matrix
        const auto vfov = glm::degrees(2.0f * std::atan(1.0f / projection_matrix[1][1]));
        const auto aspect = projection_matrix[1][1] / projection_matrix[0][0];
        const auto hfov = vfov * aspect;
        
        //spdlog::info("vFOV: {}", vfov);
        //spdlog::info("Aspect: {}", aspect);

        set_fov_method->call<void*>(sdk::get_thread_context(), camera, vfov);
        set_vertical_enable_method->call<void*>(sdk::get_thread_context(), camera, true);
        set_aspect_ratio_method->call<void*>(sdk::get_thread_context(), camera, aspect);
    }

    update_camera_origin();
    m_needs_camera_restore = true;
}

void VR::update_camera_origin() {
    

    if (!is_hmd_active()) {
        return;
    }

    // this means that the user just put the headset on or something
    // and we caught them in a bad frame
    if (inside_on_end && !m_needs_camera_restore) {
        return;
    }

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        m_needs_camera_restore = false;
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            return;
        }

        auto camera_object = utility::re_component::get_game_object(camera);

        if (camera_object == nullptr || camera_object->transform == nullptr) {
            spdlog::error("VR: Failed to get camera game object or transform!");
            m_needs_camera_restore = false;
            return;
        }

        auto camera_joint = utility::re_transform::get_joint(*camera_object->transform, 0);

        if (camera_joint == nullptr) {
            spdlog::error("VR: Failed to get camera joint!");
            m_needs_camera_restore = false;
            return;
        }

        if (!inside_on_end && camera == cameras[0]) {
            m_original_camera_position = sdk::get_joint_position(camera_joint);
            m_original_camera_rotation = sdk::get_joint_rotation(camera_joint);
            m_original_camera_matrix = Matrix4x4f{m_original_camera_rotation};
            m_original_camera_matrix[3] = m_original_camera_position;
        }

        apply_hmd_transform(camera_joint);
    }
}

void VR::apply_hmd_transform(glm::quat& rotation, Vector4f& position) {
    

    const auto rotation_offset = get_rotation_offset();
    const auto current_hmd_rotation = glm::normalize(rotation_offset * glm::quat{get_rotation(0)});
    
    glm::quat new_rotation{};
    glm::quat camera_rotation{};
    
    if (!m_decoupled_pitch->value()) {
        camera_rotation = rotation;
        new_rotation = glm::normalize(rotation * current_hmd_rotation);
    } else if (m_decoupled_pitch->value()) {
        // facing forward matrix
        const auto camera_rotation_matrix = utility::math::remove_y_component(Matrix4x4f{rotation});
        camera_rotation = glm::quat{camera_rotation_matrix};
        new_rotation = glm::normalize(camera_rotation * current_hmd_rotation);
    }

    auto current_relative_pos = rotation_offset * (get_position(0) - m_standing_origin) /*+ current_relative_eye_pos*/;
    current_relative_pos.w = 0.0f;

    auto current_head_pos = camera_rotation * current_relative_pos;

    rotation = new_rotation;
    position = position + current_head_pos;
}

void VR::apply_hmd_transform(::REJoint* camera_joint) {
    

    auto rotation = m_original_camera_rotation;
    auto position = m_original_camera_position;

    apply_hmd_transform(rotation, position);

    sdk::set_joint_rotation(camera_joint, rotation);

    if (m_positional_tracking) {
        sdk::set_joint_position(camera_joint, position);   
    }
}

bool VR::is_hand_behind_head(VRRuntime::Hand hand, float sensitivity) const {
    if (hand > VRRuntime::Hand::RIGHT || !is_using_controllers()) {
        return false;
    }

    const auto hand_index = get_controllers()[(uint32_t)hand];

    const auto hmd = get_transform(0);
    const auto hand_pos = get_position(hand_index);
    const auto hmd_delta = Vector3f{hand_pos - hmd[3]};
    const auto distance = glm::length(hmd_delta);

    if (distance >= 0.3f) {
        return false;
    }

    const auto hmd_dir = glm::normalize(hmd_delta);

    const auto& hmd_forward = hmd[2];
    const auto flattened_forward = glm::normalize(Vector3f{hmd_forward.x, 0.0f, hmd_forward.z});

    const auto hand_dot_flat_raw = glm::dot(flattened_forward, hmd_dir);
    return hand_dot_flat_raw >= sensitivity;
}

void VR::update_audio_camera() {
    

    if (!is_hmd_active()) {
        return;
    }

    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto camera_object = utility::re_component::get_game_object(camera);

    if (camera_object == nullptr || camera_object->transform == nullptr) {
        return;
    }

    auto camera_joint = utility::re_transform::get_joint(*camera_object->transform, 0);

    if (camera_joint == nullptr) {
        return;
    }

    m_original_audio_camera_position = sdk::get_joint_position(camera_joint);
    m_original_audio_camera_rotation = sdk::get_joint_rotation(camera_joint);
    m_needs_audio_restore = true;

    auto rotation = m_original_audio_camera_rotation;
    auto position = m_original_audio_camera_position;

    apply_hmd_transform(rotation, position);

    sdk::set_joint_rotation(camera_joint, rotation);

    if (m_positional_tracking) {
        sdk::set_joint_position(camera_joint, position);   
    }
}

void VR::update_render_matrix() {
    

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr || camera != cameras[0]) {
            return;
        }

        auto camera_object = utility::re_component::get_game_object(camera);

        if (camera_object == nullptr || camera_object->transform == nullptr) {
            return;
        }

        auto camera_joint = utility::re_transform::get_joint(*camera_object->transform, 0);

        if (camera_joint == nullptr) {
            return;
        }

        m_render_camera_matrix = Matrix4x4f{sdk::get_joint_rotation(camera_joint)};
        m_render_camera_matrix[3] = sdk::get_joint_position(camera_joint);
    }
}

void VR::restore_audio_camera() {
    if (!m_needs_audio_restore) {
        return;
    }

#if defined(RE2) || defined(RE3)
    if (FirstPerson::get()->will_be_used()) {
        m_needs_audio_restore = false;
        return;
    }
#endif

    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    auto camera_object = utility::re_component::get_game_object(camera);

    if (camera_object == nullptr || camera_object->transform == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    //camera_object->transform->worldTransform = m_original_camera_matrix;

    auto joint = utility::re_transform::get_joint(*camera_object->transform, 0);

    if (joint == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    sdk::set_joint_rotation(joint, m_original_audio_camera_rotation);
    sdk::set_joint_position(joint, m_original_audio_camera_position);

    m_needs_audio_restore = false;
}

void VR::restore_camera() {
    

    if (!m_needs_camera_restore) {
        return;
    }

#if defined(RE2) || defined(RE3)
    if (FirstPerson::get()->will_be_used()) {
        m_needs_camera_restore = false;
        return;
    }
#endif

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        m_needs_camera_restore = false;
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            return;
        }

        auto camera_object = utility::re_component::get_game_object(camera);

        if (camera_object == nullptr || camera_object->transform == nullptr) {
            m_needs_camera_restore = false;
            return;
        }

        //camera_object->transform->worldTransform = m_original_camera_matrix;

        auto joint = utility::re_transform::get_joint(*camera_object->transform, 0);

        if (joint == nullptr) {
            m_needs_camera_restore = false;
            return;
        }

        sdk::set_joint_rotation(joint, m_original_camera_rotation);
        sdk::set_joint_position(joint, m_original_camera_position);
    }


    m_needs_camera_restore = false;
}

void VR::set_lens_distortion(bool value) {
    

    if (!m_force_lensdistortion_settings->value()) {
        return;
    }

#ifdef RE7
    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    static auto lens_distortion_tdef = sdk::find_type_definition(game_namespace("LensDistortionController"));
    static auto lens_distortion_t = lens_distortion_tdef->get_type();

    auto lens_distortion_component = utility::re_component::find(camera, lens_distortion_t);

    if (lens_distortion_component != nullptr) {
        // Get "LensDistortion" field
        auto lens_distortion = *sdk::get_object_field<REManagedObject*>(lens_distortion_component, "LensDistortion");

        if (lens_distortion != nullptr) {
            // Call "set_Enabled" method
            sdk::call_object_func<void*>(lens_distortion, "set_Enabled", sdk::get_thread_context(), lens_distortion, value);
        }
    }
#endif
}

void VR::disable_bad_effects() {
    

    auto context = sdk::get_thread_context();

    auto application = sdk::Application::get();

    // minor optimizations to prevent hashing and map lookups every frame
    static auto renderer_t = sdk::find_type_definition("via.render.Renderer");

    static auto get_render_config_method = renderer_t->get_method("get_RenderConfig");

    static auto render_config_t = sdk::find_type_definition("via.render.RenderConfig");

    static auto get_framerate_setting_method = render_config_t->get_method("get_FramerateSetting");
    static auto set_framerate_setting_method = render_config_t->get_method("set_FramerateSetting");

    static auto get_antialiasing_method = render_config_t->get_method("get_AntiAliasing");
    static auto set_antialiasing_method = render_config_t->get_method("set_AntiAliasing");

    static auto get_lens_distortion_setting_method = render_config_t->get_method("getLensDistortionSetting");
    static auto set_lens_distortion_setting_method = render_config_t->get_method("setLensDistortionSetting");

    static auto get_motion_blur_enable_method = render_config_t->get_method("get_MotionBlurEnable");
    static auto set_motion_blur_enable_method = render_config_t->get_method("set_MotionBlurEnable");

    static auto get_vsync_method = render_config_t->get_method("get_VSync");
    static auto set_vsync_method = render_config_t->get_method("set_VSync");

    static auto get_transparent_buffer_quality_method = render_config_t->get_method("get_TransparentBufferQuality");
    static auto set_transparent_buffer_quality_method = render_config_t->get_method("set_TransparentBufferQuality");

    static auto get_lensflare_enable_method = render_config_t->get_method("get_LensFlareEnable");
    static auto set_lensflare_enable_method = render_config_t->get_method("set_LensFlareEnable");

    static auto get_colorspace_method = render_config_t->get_method("get_ColorSpace");
    static auto set_colorspace_method = render_config_t->get_method("set_ColorSpace");

    static auto get_dynamic_shadow_enable_method = render_config_t->get_method("get_DynamicShadowEnable");
    static auto set_dynamic_shadow_enable_method = render_config_t->get_method("set_DynamicShadowEnable");

    static auto get_hdrmode_method = renderer_t->get_method("get_HDRMode");
    static auto set_hdrmode_method = renderer_t->get_method("set_HDRMode");

    static auto get_hdr_display_mode_enable_method = renderer_t->get_method("get_HDRDisplayModeEnable");
    static auto set_hdr_display_mode_enable_method = renderer_t->get_method("set_HDRDisplayModeEnable");

    static auto get_delay_render_enable_method = renderer_t->get_method("get_DelayRenderEnable");
    static auto set_delay_render_enable_method = renderer_t->get_method("set_DelayRenderEnable");

    auto renderer = renderer_t->get_instance();

    auto render_config = get_render_config_method->call<::REManagedObject*>(context, renderer);

    if (render_config == nullptr) {
        spdlog::info("No render config!");
        return;
    }

    static const auto is_sf6 = utility::get_module_path(utility::get_executable())->find("StreetFighter") != std::string::npos;

    if (!is_sf6 && m_force_fps_settings->value() && get_framerate_setting_method != nullptr && set_framerate_setting_method != nullptr) {
        const auto framerate_setting = get_framerate_setting_method->call<via::render::RenderConfig::FramerateType>(context, render_config);

        // Allow FPS to go above 60
        if (framerate_setting != via::render::RenderConfig::FramerateType::VARIABLE) {
            set_framerate_setting_method->call<void*>(context, render_config, via::render::RenderConfig::FramerateType::VARIABLE);
            spdlog::info("[VR] Set framerate to variable");
        }
    }
    
    // get_MaxFps on application
    if (!is_sf6 && m_force_fps_settings->value() && application->get_max_fps() <  600.0f) {
        application->set_max_fps(600.0f);

        static bool once = []() {
            spdlog::info("[VR] Max FPS set to {}", 600.0f);
            return true;
        }();
    }

    if (get_antialiasing_method != nullptr && set_antialiasing_method != nullptr) {

        if (is_using_afw_foveated()) {
            set_antialiasing_method->call<void*>(context, render_config, via::render::RenderConfig::AntiAliasingType::TAA);
        } else if (m_force_aa_settings->value()) {
            const auto antialiasing = get_antialiasing_method->call<via::render::RenderConfig::AntiAliasingType>(context, render_config);

            // Disable TAA
            switch (antialiasing) {
            case via::render::RenderConfig::AntiAliasingType::TAA:
                [[fallthrough]];
            case via::render::RenderConfig::AntiAliasingType::FXAA_TAA:
                set_antialiasing_method->call<void*>(context, render_config, via::render::RenderConfig::AntiAliasingType::NONE);
                spdlog::info("[VR] TAA disabled");
                break;
            default:
                break;
            }
        }
    }

    if (m_force_lensdistortion_settings->value() && get_lens_distortion_setting_method != nullptr && set_lens_distortion_setting_method != nullptr) {
        const auto lens_distortion_setting = get_lens_distortion_setting_method->call<via::render::RenderConfig::LensDistortionSetting>(context, render_config);

        // Disable lens distortion
        if (lens_distortion_setting != via::render::RenderConfig::LensDistortionSetting::OFF) {
            set_lens_distortion_setting_method->call<void*>(context, render_config, via::render::RenderConfig::LensDistortionSetting::OFF);
            spdlog::info("[VR] Lens distortion disabled");
        }
    }

    if (m_force_motionblur_settings->value() && get_motion_blur_enable_method != nullptr && set_motion_blur_enable_method != nullptr) {
        const auto is_motion_blur_enabled = get_motion_blur_enable_method->call<bool>(context, render_config);

        // Disable motion blur
        if (is_motion_blur_enabled) {
            set_motion_blur_enable_method->call<void*>(context, render_config, false);
            spdlog::info("[VR] Motion blur disabled");
        }
    }

    if (m_force_vsync_settings->value() && get_vsync_method != nullptr && set_vsync_method != nullptr) {
#ifndef MHRISE
        const auto vsync = get_vsync_method->call<bool>(context, render_config);

        // Disable vsync
        if (vsync) {
            set_vsync_method->call<void*>(context, render_config, false);
            spdlog::info("[VR] VSync disabled");
        }
#else
        // We are only calling set_vsync instead of checking with get_vsync in MHRise
        // because get_VSync has some insane code protection on it for some reason
        // which would increase frametimes to 15+
        set_vsync_method->call<void*>(context, render_config, false);
#endif
    }

    if (m_force_volumetrics_settings->value() && get_transparent_buffer_quality_method != nullptr && set_transparent_buffer_quality_method != nullptr) {
        const auto transparent_buffer_quality = get_transparent_buffer_quality_method->call<via::render::RenderConfig::Quality>(context, render_config);

        // Disable volumetrics
        if (transparent_buffer_quality != via::render::RenderConfig::Quality::NONE) {
            set_transparent_buffer_quality_method->call<void*>(context, render_config, via::render::RenderConfig::Quality::NONE);
            spdlog::info("[VR] Volumetrics disabled");
        }
    }

    if (m_force_lensflares_settings->value() && get_lensflare_enable_method != nullptr && set_lensflare_enable_method != nullptr) {
        const auto is_lensflare_enabled = get_lensflare_enable_method->call<bool>(context, render_config);

        // Disable lensflares
        if (is_lensflare_enabled) {
            set_lensflare_enable_method->call<void*>(context, render_config, false);
            spdlog::info("[VR] Lensflares disabled");
        }
    }

    if (get_hdrmode_method != nullptr && set_hdrmode_method != nullptr) {
        // static
        const auto is_hdr_enabled = get_hdrmode_method->call<bool>(context);

        // Disable HDR
        if (is_hdr_enabled) {
            set_hdrmode_method->call<void*>(context, false);
            
            if (set_hdr_display_mode_enable_method != nullptr) {
                set_hdr_display_mode_enable_method->call<void*>(context, false);
            }

            spdlog::info("[VR] HDR disabled");
        }
    }

    if (get_colorspace_method != nullptr && set_colorspace_method != nullptr) {
        const auto is_hdr_enabled = get_colorspace_method->call<via::render::ColorSpace>(context, render_config) == via::render::ColorSpace::HDR10;

        if (is_hdr_enabled) {
            set_colorspace_method->call<void*>(context, render_config, via::render::ColorSpace::HDTV);
            spdlog::info("[VR] HDR disabled (ColorSpace)");
        }
    }

    if (m_force_dynamic_shadows_settings->value() && get_dynamic_shadow_enable_method != nullptr && set_dynamic_shadow_enable_method != nullptr) {
        const auto is_dynamic_shadow_enabled = get_dynamic_shadow_enable_method->call<bool>(context, render_config);

        // Enable dynamic shadows
        if (!is_dynamic_shadow_enabled) {
            set_dynamic_shadow_enable_method->call<void*>(context, render_config, true);
            spdlog::info("[VR] Dynamic shadows enabled");
        }
    }

    // Causes crashes on D3D11. May be a performance detriment in VR with new rendering method.
    const auto is_new_rendering_method = is_using_afw_foveated();

    if (!is_new_rendering_method && !is_sf6 && g_framework->get_renderer_type() == REFramework::RendererType::D3D12 && m_enable_asynchronous_rendering->value()) {
        if (get_delay_render_enable_method != nullptr && set_delay_render_enable_method != nullptr) {
            const auto is_delay_render_enabled = get_delay_render_enable_method->call<bool>(context);

            if (is_delay_render_enabled == true) {
                set_delay_render_enable_method->call<void*>(context, !m_enable_asynchronous_rendering->value());
                spdlog::info("[VR] Delay render modified");
            }
        }
    } else if (is_sf6) {
        // Must be on in SF6 or left eye gets stuck
        if (set_delay_render_enable_method != nullptr) {
            set_delay_render_enable_method->call<void*>(context, true);
        }
    }

#ifdef RE7
    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto camera_game_object = utility::re_component::get_game_object(camera);

    if (camera_game_object == nullptr) {
        return;
    }
    
    auto camera_transform = camera_game_object->transform;

    if (camera_transform == nullptr) {
        return;
    }

    auto get_type = [](std::string name) {
        auto tdef = sdk::find_type_definition(name);
        return tdef->get_type();
    };

    static auto effect_player_t = get_type("via.effect.EffectPlayer");
    static auto hide_effect_for_vr_t = get_type(game_namespace("EPVStandard.HideEffectForVR"));

    // Do not draw the game object if it is hidden for VR (this was there for the original PSVR release i guess)
    for (auto child = camera_transform->child; child != nullptr; child = child->child) {
        if (utility::re_component::find(child, effect_player_t) != nullptr) {
            if (utility::re_component::find(child, hide_effect_for_vr_t) != nullptr) {
                auto game_object = child->ownerGameObject;

                game_object->shouldDraw = false;
                //sdk::call_object_func<void*>(game_object, "set_Draw", sdk::get_thread_context(), game_object, false);
            }
        }
    }
#endif
}

void VR::fix_temporal_effects() {
    // this is SO DUMB!!!!!
    const auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }
    
    static auto render_output_type = sdk::find_type_definition("via.render.RenderOutput")->get_type();
    auto render_output_component = utility::re_component::find(camera, render_output_type);

    if (render_output_component == nullptr) {
        return;
    }

    if (!get_runtime()->ready() || m_disable_temporal_fix) {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 0); // None
        return;
    }

    if (m_frame_count % 2 == m_left_eye_interval) {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 1); // left
    } else {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 2); // right
    }
}

int32_t VR::get_frame_count() const {
    return get_game_frame_count();
}

int32_t VR::get_game_frame_count() const {
    static auto renderer_type = sdk::find_type_definition("via.render.Renderer");

    if (renderer_type == nullptr) {
        renderer_type = sdk::find_type_definition("via.render.Renderer");
        spdlog::warn("VR: Failed to find renderer type, trying again next time");
        return 0;
    }

    auto renderer = renderer_type->get_instance();

    if (renderer == nullptr) {
        return 0;
    }

    static auto get_render_frame_method = renderer_type->get_method("get_RenderFrame");

    return get_render_frame_method->call<int32_t>(sdk::get_thread_context(), renderer);
}

float VR::get_standing_height() {
    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin.y;
}

Vector4f VR::get_standing_origin() {
    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin;
}

void VR::set_standing_origin(const Vector4f& origin) {
    std::unique_lock _{ get_runtime()->pose_mtx };
    
    m_standing_origin = origin;
}

glm::quat VR::get_rotation_offset() {
    std::shared_lock _{ m_rotation_mtx };

    return m_rotation_offset;
}

void VR::set_rotation_offset(const glm::quat& offset) {
    std::unique_lock _{ m_rotation_mtx };

    m_rotation_offset = offset;
}

void VR::recenter_view() {
    const auto new_rotation_offset = glm::normalize(glm::inverse(utility::math::flatten(glm::quat{get_rotation(0)})));

    set_rotation_offset(new_rotation_offset);
}

glm::quat VR::get_gui_rotation_offset() {
    std::shared_lock _{ m_gui_mtx };

    return m_gui_rotation_offset;
}

void VR::set_gui_rotation_offset(const glm::quat& offset) {
    std::unique_lock _{ m_gui_mtx };

    m_gui_rotation_offset = offset;
}

void VR::recenter_gui(const glm::quat& from) {
    const auto new_gui_offset = glm::normalize(glm::inverse(utility::math::flatten(from)));
    set_gui_rotation_offset(new_gui_offset);
}

Vector4f VR::get_current_offset() {
    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    const auto count = m_frame_count;

    if (count % 2 == m_left_eye_interval) {
        //return Vector4f{m_eye_distance * -1.0f, 0.0f, 0.0f, 0.0f};
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
    //return Vector4f{m_eye_distance, 0.0f, 0.0f, 0.0f};
}

Matrix4x4f VR::get_current_eye_transform(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    const auto count = m_frame_count;
    const auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (count % 2 == mod_count) {
        return get_runtime()->eyes[vr::Eye_Left];
    }

    return get_runtime()->eyes[vr::Eye_Right];
}

Matrix4x4f VR::get_current_projection_matrix(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    const auto count = m_frame_count;
    const auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (count % 2 == mod_count) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

Matrix4x4f VR::get_projection_matrix(uint32_t pass) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    if (pass % 2 == 0) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

Matrix4x4f VR::get_eye_transform(uint32_t pass) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    if (pass % 2 == 0) {
        return get_runtime()->eyes[vr::Eye_Left];
    }

    return get_runtime()->eyes[vr::Eye_Right];
}

void VR::on_pre_imgui_frame() {
    

    if (!get_runtime()->ready()) {
        return;
    }

    m_overlay_component.on_pre_imgui_frame();
}

glm::mat4 to_reverseZ(const glm::mat4& proj) {

	glm::mat4 transformMat = glm::mat4(
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, -1,0,
		0, 0, 1, 1
	);

    return transformMat * proj;
}

void VR::get_camera_data() {
}

void VR::update_camera_data(int frame_count) {

    if (last_update_camera_data_frame_count < frame_count) {
        last_update_camera_data_frame_count = frame_count;

        std::shared_lock _{get_runtime()->eyes_mtx};

        EyeIndex nEye = (m_render_frame_count % 2 == m_left_eye_interval) ? EyeLeft : EyeRight;
        EyeIndex nEyeOther = (m_render_frame_count % 2 == m_left_eye_interval) ? EyeRight : EyeLeft;

        glm::mat4 matEyePosRender = get_runtime()->eyes[nEye];
        glm::mat4 matEyePosProj = get_runtime()->eyes[nEyeOther];

        cameraData[nEye].camWorldToViewMatrix = glm::inverse(m_original_camera_matrix);
        cameraData[nEye].camViewToWorldMatrix = m_original_camera_matrix;
        cameraData[nEye].destViewToWorldMatrix = m_render_camera_matrix * matEyePosProj;
        cameraData[nEye].srcViewToWorldMatrix = m_render_camera_matrix * matEyePosRender;
        cameraData[nEye].destWorldToViewMatrix = glm::inverse(cameraData[nEye].destViewToWorldMatrix);
        cameraData[nEye].srcWorldToViewMatrix = glm::inverse(cameraData[nEye].srcViewToWorldMatrix);

        cameraData[nEye].destViewToClipMatrix = to_reverseZ(get_runtime()->afw_projections[nEyeOther]);
        cameraData[nEye].destClipToViewMatrix = glm::inverse(cameraData[nEye].destViewToClipMatrix);
        cameraData[nEye].srcViewToClipMatrix = to_reverseZ(get_runtime()->afw_projections[nEye]);
        cameraData[nEye].srcClipToViewMatrix = glm::inverse(cameraData[nEye].srcViewToClipMatrix);
        cameraData[nEye].camViewToClipMatrix = cameraData[nEye].srcViewToClipMatrix;
        cameraData[nEye].camClipToViewMatrix = cameraData[nEye].srcClipToViewMatrix;

        // src -> current eye current frame foveated 
        // srcPrev -> current eye previous farme foveated or other eye previous farme foveated if not using wobbling fix
        // dest -> current eye current frame foveated (to negate camera motion)

        cameraDataForMV[nEye].srcWorldToViewMatrix = cameraData[nEye].srcWorldToViewMatrix;
        cameraDataForMV[nEye].srcViewToWorldMatrix = cameraData[nEye].srcViewToWorldMatrix;
        cameraDataForMV[nEye].srcViewToClipMatrix = to_reverseZ(get_runtime()->foveated_projections[nEye]);
        cameraDataForMV[nEye].srcClipToViewMatrix = glm::inverse(cameraDataForMV[nEye].srcViewToClipMatrix);

        cameraDataForMV[nEye].srcWorldToViewMatrixPrev = oldViewMatrix[nEye];
        cameraDataForMV[nEye].srcViewToWorldMatrixPrev = glm::inverse(cameraDataForMV[nEye].srcWorldToViewMatrixPrev);
        cameraDataForMV[nEye].srcViewToClipMatrixPrev = oldFoveatedProjectionMatrix[nEye];
        cameraDataForMV[nEye].srcClipToViewMatrixPrev = glm::inverse(cameraDataForMV[nEye].srcViewToClipMatrixPrev);

        cameraDataForMV[nEye].destWorldToViewMatrix = cameraDataForMV[nEye].srcWorldToViewMatrix;
        cameraDataForMV[nEye].destViewToWorldMatrix = cameraDataForMV[nEye].srcViewToWorldMatrix;
        cameraDataForMV[nEye].destViewToClipMatrix = cameraDataForMV[nEye].srcViewToClipMatrix;
        cameraDataForMV[nEye].destClipToViewMatrix = cameraDataForMV[nEye].srcClipToViewMatrix;
    }
}

#if defined(PRAGMATA)
// PragmataVR: hide the gamepad from the game while the HUD menu is open (XInput games only).
namespace {
std::unique_ptr<FunctionHook> g_pragmata_xinput_hook{};
std::atomic<uint64_t> g_pragmata_xinput_game_calls{0};

DWORD WINAPI pragmata_xinput_get_state(DWORD index, XINPUT_STATE* state) {
    auto original = g_pragmata_xinput_hook->get_original<decltype(pragmata_xinput_get_state)>();
    const auto result = original(index, state);
    g_pragmata_xinput_game_calls++;

    if (result == ERROR_SUCCESS && state != nullptr) {
        auto& hud = pragmatavr::HudTuning::get();
        if (hud.should_block_pad()) {
            state->Gamepad = XINPUT_GAMEPAD{};
        } else {
            hud.filter_game_buttons(state->Gamepad.wButtons); // Build 58: hold L3 + R3 to open the menu
        }
    }

    return result;
}

void pragmata_setup_gamepad() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    auto& hud = pragmatavr::HudTuning::get();
    HMODULE xinput = GetModuleHandleW(L"xinput1_4.dll");
    if (xinput == nullptr) {
        xinput = LoadLibraryW(L"xinput1_4.dll");
    }

    auto get_state = xinput != nullptr ? (pragmatavr::HudTuning::XInputGetStateFn)GetProcAddress(xinput, "XInputGetState") : nullptr;
    if (get_state == nullptr) {
        spdlog::error("[PragmataVR] XInputGetState not found; gamepad menu control unavailable");
        return;
    }

    g_pragmata_xinput_hook = std::make_unique<FunctionHook>((uintptr_t)get_state, (uintptr_t)&pragmata_xinput_get_state);
    if (g_pragmata_xinput_hook->create()) {
        hud.xinput_get_state = g_pragmata_xinput_hook->get_original<decltype(pragmata_xinput_get_state)>();
        spdlog::info("[PragmataVR] Hooked XInputGetState; the game will not see the pad while the HUD menu is open");
    } else {
        g_pragmata_xinput_hook.reset();
        hud.xinput_get_state = get_state;
        spdlog::warn("[PragmataVR] Could not hook XInputGetState; menu works but the game will also see menu button presses");
    }
}

void pragmata_report_xinput_use() {
    static auto start = std::chrono::steady_clock::now();
    static bool reported = false;
    if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(60)) {
        reported = true;
        spdlog::info("[PragmataVR] Game XInputGetState calls in the first minute: {}{}", g_pragmata_xinput_game_calls.load(),
            g_pragmata_xinput_game_calls.load() == 0 ? " (game reads the pad another way; menu buttons will also reach the game)" : "");
    }
}
} // namespace

// PragmataVR: in-headset mod menu (Builds 9-10). Drawn with ImGui's foreground draw list into REFramework's IMGUI render
// target (always on top of any other ImGui window). D3D12Component::on_frame copies the menu rectangle into the OpenXR
// menu quad layer, which the headset's compositor shows fixed in the room. On OpenVR it is pasted into each eye image.
// Look: dark translucent panel, cyan accents and thin lines in the style of Pragmata's own HUD, Barlow Semi Condensed.
namespace {
namespace pragmata_menu_style {
const ImU32 PANEL_TOP = IM_COL32(12, 24, 32, 238);
const ImU32 PANEL_BOTTOM = IM_COL32(4, 9, 14, 238);
const ImU32 BORDER = IM_COL32(88, 226, 232, 64);
const ImU32 ACCENT = IM_COL32(88, 226, 232, 255);
const ImU32 ACCENT_DIM = IM_COL32(88, 226, 232, 120);
const ImU32 ACCENT_FAINT = IM_COL32(88, 226, 232, 44);
const ImU32 SELECT_LEFT = IM_COL32(88, 226, 232, 64);
const ImU32 SELECT_RIGHT = IM_COL32(88, 226, 232, 6);
const ImU32 TEXT = IM_COL32(222, 234, 238, 255);
const ImU32 TEXT_BRIGHT = IM_COL32(255, 255, 255, 255);
const ImU32 TEXT_DIM = IM_COL32(128, 150, 160, 255);
const ImU32 TEXT_OFF = IM_COL32(76, 90, 98, 255);
const ImU32 AMBER = IM_COL32(242, 184, 82, 255);
} // namespace pragmata_menu_style

// What one menu row shows in its value column.
struct PragmataRowView {
    std::string value{};
    bool show_invert{false};
    bool invert_on{false};
    bool dim{false};
    ImU32 value_color{pragmata_menu_style::TEXT_BRIGHT};
};

PragmataRowView pragmata_row_view(const pragmatavr::HudTune& t, const pragmatavr::MenuRow& row) {
    using namespace pragmata_menu_style;
    using Kind = pragmatavr::MenuRow::Kind;

    PragmataRowView v{};
    const bool per_panel_off = t.layout_mode == 1; // per-panel groups only apply in the per-panel layout

    switch (row.kind) {
    case Kind::Value:
        v.show_invert = false; // v0.9.0: INV boxes removed (directions fixed in code)
        if (row.group == pragmatavr::GROUP_SCREEN) {
            v.value = pragmatavr::HudTuning::format_screen(t, row.field);
            v.invert_on = t.screen_invert[row.field];
            v.dim = !t.hud_panel && (t.flat_test || !t.hud_fixed); // ANCHORED panel, or the room-fixed 3D HUD
        } else if (row.group == pragmatavr::GROUP_ENEMY) {
            v.value = fmt::format("{:+.0f}", t.enemy_bar[row.field]);
            v.invert_on = t.enemy_bar_invert[row.field];
            v.dim = pragmatavr::enemy_field_locked(t, row.field); // Build 51: per section lock
        } else if (row.group == pragmatavr::GROUP_MENU) {
            v.value = pragmatavr::HudTuning::format_menu(t, row.field);
            v.invert_on = t.menu_invert[row.field];
        } else if (row.group >= 0 && row.group < pragmatavr::GroupCount) {
            const auto& gt = pragmatavr::active_group(t, row.group);
            v.value = pragmatavr::HudTuning::format_value(gt, row.group, row.field);
            v.invert_on = t.groups[row.group].invert[row.field];
            v.dim = per_panel_off || pragmatavr::group_locked(t, row.group) ||
                    (row.field == pragmatavr::FieldGap && t.hud_panel && row.group != pragmatavr::GroupHack); // ANCHORED: no eye gap
        }
        break;
    case Kind::Toggle:
        v.value_color = ACCENT;
        switch (row.action) {
        case pragmatavr::ActionToggleLayout:
            v.value = t.layout_mode == 1 ? "GAME SCREEN" : "PER-PANEL";
            break;
        case pragmatavr::ActionToggleHudFixed:
            v.value = t.hud_fixed ? "FIXED IN ROOM" : "FOLLOWS HEAD";
            v.dim = t.hud_mode != pragmatavr::HudMode3D; // 3D mode only
            break;
        case pragmatavr::ActionToggleLockInvert:
            v.value = t.lock_invert ? "FLIPPED" : "NORMAL";
            v.value_color = t.lock_invert ? AMBER : TEXT_BRIGHT;
            break;
        case pragmatavr::ActionToggleGhost:
            v.value = t.hide_ghost ? "HIDDEN" : "SHOWN";
            break;
        case pragmatavr::ActionToggleBoxes:
            v.value = t.show_boxes ? "SHOWN" : "HIDDEN";
            v.value_color = t.show_boxes ? AMBER : ACCENT;
            v.dim = !t.hud_panel; // ANCHORED only
            break;
        case pragmatavr::ActionToggleSharpTextures:
            v.value = t.sharp_textures ? "ON" : "OFF";
            v.value_color = ACCENT;
            break;
        case pragmatavr::ActionToggleLowHealth:
            v.value = t.hide_low_health ? "HIDDEN" : "SHOWN";
            v.value_color = t.hide_low_health ? ACCENT : AMBER;
            break;
        case pragmatavr::ActionToggleMenuNative:
            v.value = t.menu_native ? "ON" : "OFF";
            v.value_color = t.menu_native ? ACCENT : AMBER;
            v.dim = !t.render_afw; // only matters in AFW
            break;
        case pragmatavr::ActionToggleFlatScreen:
            v.value = t.flat_screen || t.hud_panel ? "SCREEN" : "WORLD";
            v.value_color = t.flat_screen || t.hud_panel ? ACCENT : AMBER;
            v.dim = !t.flat_test || t.hud_panel; // FLAT mode only (ANCHORED always uses SCREEN)
            break;
        case pragmatavr::ActionToggleBlur:
            v.value = t.hide_blur ? "HIDDEN" : "SHOWN";
            v.dim = !t.flat_test; // FLAT mode only
            break;
        case pragmatavr::ActionToggleGluedHud:
            v.value = t.glued_hud_removed ? "REMOVED" : "SHOWN";
            v.value_color = t.glued_hud_removed ? AMBER : ACCENT;
            v.dim = !t.hud_panel; // PANEL mode only
            break;
        case pragmatavr::ActionToggleHudPanel:
            v.value = pragmatavr::hud_mode_name(t.hud_mode);
            v.value_color = t.hud_mode == pragmatavr::HudMode3D ? AMBER : ACCENT;
            break;
        case pragmatavr::ActionToggleEngineOverlay:
            v.value = t.engine_overlay == 0 ? "DEFAULT" : (t.engine_overlay == 1 ? "OFF" : "ON");
            v.value_color = t.engine_overlay == 0 ? ACCENT : AMBER;
            break;
        case pragmatavr::ActionToggleCompanion:
            v.value = t.companion_mode == pragmatavr::CompanionWithElement ? "WITH ELEMENT" :
                      (t.companion_mode == pragmatavr::CompanionReframework ? "REFRAMEWORK" : "HIDDEN");
            v.value_color = t.companion_mode == pragmatavr::CompanionWithElement ? ACCENT : AMBER;
            v.dim = t.hud_mode != pragmatavr::HudMode3D; // FLAT and PANEL place the companions themselves
            break;
        case pragmatavr::ActionToggleScaleCenter:
            v.value = t.scale_about_center ? "ELEMENT CENTER" : "PANEL ORIGIN";
            v.value_color = t.scale_about_center ? ACCENT : AMBER;
            v.dim = t.hud_mode != pragmatavr::HudMode3D; // FLAT always grows from the element center
            break;
        case pragmatavr::ActionToggleRenderMode:
            v.value = t.render_afw ? "AFW" : "NATIVE";
            v.value_color = t.render_afw ? AMBER : ACCENT;
            break;
        case pragmatavr::ActionToggleSharpening:
            v.value = t.sharpening ? "ON" : "OFF";
            break;
        case pragmatavr::ActionToggleDesktopMirror:
            v.value = t.desktop_mirror ? "ON" : "OFF";
            v.value_color = t.desktop_mirror ? AMBER : ACCENT;
            break;
        case pragmatavr::ActionTogglePerfLog:
            v.value = t.perf_log ? "ON" : "OFF";
            break;
        case pragmatavr::ActionToggleShiftDamage:
            v.value = t.shift_damage ? "ON" : "OFF";
            v.value_color = t.shift_damage ? AMBER : ACCENT;
            break;
        case pragmatavr::ActionToggleEnemyLock:
            v.value = t.enemy_locked ? "LOCKED" : "UNLOCKED";
            v.value_color = t.enemy_locked ? AMBER : ACCENT;
            break;
        case pragmatavr::ActionToggleDamageLock:
            v.value = t.damage_locked ? "LOCKED" : "UNLOCKED";
            v.value_color = t.damage_locked ? AMBER : ACCENT;
            break;
        case pragmatavr::ActionToggleHackBackdrop:
            v.value = t.hide_hack_backdrop ? "HIDDEN" : "SHOWN";
            break;
        case pragmatavr::ActionToggleCaptureCorners:
            v.value = t.fixed_capture_corners ? "CORNERS" : "HOME";
            v.value_color = t.fixed_capture_corners ? ACCENT : AMBER;
            v.dim = !t.hud_panel; // Fixed mode only
            break;
        case pragmatavr::ActionToggleWorldProj:
            v.value = t.world_headset ? "HEADSET" : "FLAT";
            v.value_color = t.world_headset ? ACCENT : AMBER;
            break;
        case pragmatavr::ActionToggleHackLock:
        default:
            {
                const bool lk = row.group >= 0 && row.group < pragmatavr::GroupCount && t.locked[row.group];
                v.value = lk ? "LOCKED" : "UNLOCKED";
                v.value_color = lk ? AMBER : ACCENT;
                v.dim = per_panel_off;
            }
            break;
        }
        break;
    case Kind::Action:
    default:
        if (row.action == pragmatavr::ActionResetScreen) {
            v.dim = t.layout_mode != 1;
        } else if (row.action == pragmatavr::ActionResetGroup) {
            v.dim = per_panel_off || pragmatavr::group_locked(t, row.group);
        }
        break;
    }

    return v;
}
} // namespace

void VR::on_frame() {
    auto& hud = pragmatavr::HudTuning::get();

    if (!hud.menu_open.load() || !is_hmd_active()) {
        hud.menu_rect_valid = false;
        return;
    }

    using namespace pragmata_menu_style;
    using Kind = pragmatavr::MenuRow::Kind;

    const auto tune = hud.tune();
    const auto& t = *tune;
    const auto& rows = hud.menu_rows();
    const int sel = hud.menu_selected.load();
    const bool editing = hud.menu_editing.load();
    const bool on_invert = hud.menu_on_invert.load() && !editing;
    const int cur_tab = std::clamp(hud.menu_tab.load(), 0, (int)pragmatavr::TabCount - 1); // Build 36

    ImFont* font = pragmatavr::g_menu_font != nullptr ? pragmatavr::g_menu_font : ImGui::GetFont();
    ImFont* bold = pragmatavr::g_menu_font_bold != nullptr ? pragmatavr::g_menu_font_bold : font;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    constexpr float NO_LIMIT = (std::numeric_limits<float>::max)();

    // ---- Metrics (all from the body text size, so the whole panel scales together) ----
    const float fs = pragmatavr::MENU_FONT_PX;
    const float small_size = std::round(fs * 0.66f);
    const float title_size = std::round(fs * 1.2f);
    const float tier_size = std::round(fs * 0.95f);
    const float header_size = std::round(fs * 0.78f);
    const float hint_size = std::round(fs * 0.8f);
    const float pad = std::round(fs * 1.1f);
    const float line_h = std::round(fs * 1.5f);
    const float gutter = std::round(fs * 1.6f);
    const float gap = std::round(fs * 0.9f);
    const float indent = std::round(fs * 0.7f);
    const float box = std::round(fs * 0.5f);
    const float header_gap_first = std::round(fs * 0.15f);
    const float header_gap = std::round(fs * 0.75f);
    const float header_body = std::round(header_size * 1.6f);
    const float tier_block = std::round(tier_size * 1.3f + fs * 0.55f);

    const auto text_w = [&](ImFont* f, float size, const char* s) {
        return f->CalcTextSizeA(size, NO_LIMIT, 0.0f, s).x;
    };

    // Letter-spaced capitals for titles and headers. Returns the width; draws only when `draw` is set.
    const auto spaced = [&](ImFont* f, float size, float x, float y, ImU32 col, const char* s, float spacing, bool draw) {
        float cx = x;
        char buf[2]{0, 0};
        for (const char* c = s; *c != 0; ++c) {
            buf[0] = *c;
            if (draw) {
                dl->AddText(f, size, ImVec2{std::floor(cx), std::floor(y)}, col, buf);
            }
            cx += text_w(f, size, buf) + spacing;
        }
        return *s != 0 ? cx - x - spacing : 0.0f;
    };

    const auto put = [&](ImFont* f, float size, float x, float y, ImU32 col, const char* s) {
        dl->AddText(f, size, ImVec2{std::floor(x), std::floor(y)}, col, s);
    };

    // Button chip like a controller prompt: outlined box with the button name, then what it does.
    const auto chip = [&](float x, float y, const char* button, const char* what, bool draw) {
        const float bw = text_w(bold, small_size, button) + small_size * 0.9f;
        const float bh = small_size * 1.35f;
        if (draw) {
            dl->AddRect(ImVec2{std::floor(x), std::floor(y)}, ImVec2{std::floor(x + bw), std::floor(y + bh)}, ACCENT_DIM, 3.0f, 0, 1.5f);
            put(bold, small_size, x + small_size * 0.45f, y + (bh - small_size) * 0.5f, TEXT_BRIGHT, button);
            put(font, small_size, x + bw + small_size * 0.45f, y + (bh - small_size) * 0.5f, TEXT_DIM, what);
        }
        return bw + small_size * 0.45f + text_w(font, small_size, what) + small_size * 1.6f;
    };

    using ChipList = std::array<std::pair<const char*, const char*>, 5>;
    static constexpr ChipList NAV_CHIPS{{
        {"L3 + R3", "Open / close"},
        {"LT / RT", "Tabs"},
        {"D-PAD", "Navigate"},
        {"A", "Select / edit"},
        {"B", "Close"},
    }};
    static constexpr ChipList EDIT_CHIPS{{
        {"LEFT / RIGHT", "Change value"},
        {"RB", "Bigger steps"},
        {"A", "Done"},
        {"B", "Done"},
        {"", ""},
    }};
    const char* KEYBOARD_LINE = "KEYBOARD    Arrows = navigate / change    PgUp / PgDn = tabs    Shift = bigger steps    Enter = select / edit / done    Backspace = back / close    F1 = open / close";
    const char* EDIT_HINT = "EDITING    Left / Right to change    RB = bigger steps    A or B = done";
    const char* INV_HINT = "A = flip which way Left / Right moves this value";
    const char* PER_PANEL_HINT = "Used only in the per-panel HUD layout (switch it under HUD SCREEN)";


    // ---- Measure: column widths fit every label, value and header in both bands ----
    float value_w = 0.0f;
    for (const char* s : {"NATIVE", "GAME SCREEN", "FIXED IN ROOM", "FOLLOWS HEAD", "PER-PANEL", "UNLOCKED", "FLIPPED", "HIDDEN",
             "+4000", "-4000", "10.0 m", "+60 deg", "140 deg", "200%", "100%", "OFF", "8.00x", "ELEMENT CENTER", "PANEL ORIGIN", "WITH ELEMENT", "REFRAMEWORK", "DEFAULT", "PANEL", "REMOVED"}) {
        value_w = (std::max)(value_w, text_w(bold, fs, s));
    }

    // v0.9.0: no INV boxes, so no room is kept for them.
    const float invert_w = 0.0f;
    const float invert_gap = 0.0f;

    std::array<float, 2> col_w{};
    std::array<std::array<float, 2>, 2> col_h{};                 // [band][column]
    std::array<std::array<bool, 2>, 2> col_first{{{true, true}, {true, true}}};
    std::vector<float> row_col_y(rows.size(), 0.0f); // each row's top within its band column (for scrolling)

    // Build 25: collapsible sections. A collapsed section shows only its header; its rows are skipped here and below.
    const auto header_tag = [&](const pragmatavr::MenuRow& r) -> std::string {
        if (r.section < 0) {
            return r.hint;
        }
        return pragmatavr::section_collapsed(t.collapsed, r.section) ? "SHOW  +" : "HIDE  -";
    };
    const char* HEADER_HINT = "A = show or hide this section";

    for (int ri = 0; ri < (int)rows.size(); ++ri) {
        const auto& row = rows[ri];
        const int c = std::clamp(row.column, 0, 1);
        const int bd = std::clamp(row.band, 0, 1);
        row_col_y[ri] = col_h[bd][c];

        if (pragmatavr::row_hidden(t.collapsed, row)) {
            continue;
        }
        // Build 36: column widths fit the rows of every tab (the panel keeps its width when the tab changes);
        // heights count only the current tab.
        const bool on_tab = row.tab == cur_tab;

        if (row.kind == Kind::Header) {
            const auto htag = header_tag(row);
            const float title = header_size * 0.95f + spaced(bold, header_size, 0, 0, 0, row.label.c_str(), header_size * 0.14f, false);
            const float tag = htag.empty() ? 0.0f : gap + text_w(font, small_size, htag.c_str());
            col_w[c] = (std::max)(col_w[c], title + tag);
            if (on_tab) {
                col_h[bd][c] += (col_first[bd][c] ? header_gap_first : header_gap) + header_body;
                col_first[bd][c] = false;
            }
            continue;
        }

        const float label = (std::max)(text_w(font, fs, row.label.c_str()), text_w(bold, fs, row.label.c_str()));
        col_w[c] = (std::max)(col_w[c], indent + label + gap + value_w + invert_gap + invert_w + indent * 0.5f);
        if (on_tab) {
            col_h[bd][c] += line_h;
        }
    }

    const std::array<float, 2> band_h{(std::max)(col_h[0][0], col_h[0][1]), (std::max)(col_h[1][0], col_h[1][1])};

    // Title, tier and footer widths.
    const float title_spacing = title_size * 0.14f;
    const float tier_spacing = tier_size * 0.14f;
    const float title_w = spaced(bold, title_size, 0, 0, 0, "PRAGMATA VR", title_spacing, false) + fs * 0.6f +
        spaced(bold, title_size, 0, 0, 0, "MOD MENU", title_spacing, false) + gap * 2.0f + text_w(font, small_size, "v0.9.2 B60");

    // Build 36: the tab bar (LT, the four tab titles, RT) replaces the two band titles.
    const float tab_gap = std::round(fs * 1.6f);
    float tabs_w = 0.0f;
    for (int ti = 0; ti < pragmatavr::TabCount; ++ti) {
        tabs_w += spaced(bold, tier_size, 0, 0, 0, pragmatavr::TAB_TITLES[ti], tier_spacing, false) + (ti ? tab_gap : 0.0f);
    }
    const float tier_w = tabs_w + 2.0f * (gap * 2.0f + text_w(bold, small_size, "LT"));

    float chips_w = 0.0f;
    for (const auto* list : {&NAV_CHIPS, &EDIT_CHIPS}) {
        float w = 0.0f;
        for (const auto& [button, what] : *list) {
            if (*button != 0) {
                w += chip(0, 0, button, what, false);
            }
        }
        chips_w = (std::max)(chips_w, w);
    }

    float hint_w = (std::max)({text_w(font, hint_size, EDIT_HINT), text_w(font, hint_size, INV_HINT), text_w(font, hint_size, PER_PANEL_HINT),
        text_w(font, hint_size, HEADER_HINT)});
    for (const auto& row : rows) {
        if (row.kind != Kind::Header) {
            hint_w = (std::max)(hint_w, text_w(font, hint_size, row.hint.c_str()));
        }
    }

    const float content_w = col_w[0] + gutter + col_w[1];
    const float panel_w = std::ceil(pad * 2.0f + (std::max)({content_w, title_w, tier_w, chips_w, hint_w, text_w(font, small_size, KEYBOARD_LINE)}));

    const float title_block = std::round(title_size * 1.25f + fs * 0.9f);
    const float content_h = tier_block + band_h[0]; // Build 36: one band per tab
    const float footer_h = std::round(fs * 0.8f + hint_size * 1.6f + small_size * 1.35f + small_size * 0.7f + small_size * 1.4f);

    // Build 12: when the rows are taller than the ImGui target, the band area scrolls (title and footer stay put) and
    // follows the highlighted row. Small cyan arrows on the right show that there is more above or below.
    const float fixed_h = pad * 0.8f + title_block + footer_h + pad * 0.8f;
    const float max_panel_h = std::floor(ImGui::GetIO().DisplaySize.y - 32.0f);
    const float view_h = std::floor((std::min)(content_h, (std::max)(line_h * 6.0f, max_panel_h - fixed_h)));
    const bool scrolls = view_h < content_h - 0.5f;
    const float panel_h = std::ceil(fixed_h + view_h);

    static float s_scroll = 0.0f;
    if (!scrolls) {
        s_scroll = 0.0f;
    } else if (sel >= 0 && sel < (int)rows.size()) {
        const int sbd = 0; // Build 36: one band per tab
        const float band_off = tier_block;
        const float sel_top = band_off + row_col_y[sel];
        const float margin = line_h * 1.5f;
        if (sel_top - margin < s_scroll) {
            s_scroll = sel_top - margin;
        }
        if (sel_top + line_h + margin > s_scroll + view_h) {
            s_scroll = sel_top + line_h + margin - view_h;
        }
        if (sbd == 0 && sel_top < view_h * 0.5f) {
            s_scroll = 0.0f; // top band: show the menu from its top
        }
        s_scroll = std::clamp(s_scroll, 0.0f, content_h - view_h);
    }

    // ---- Panel ----
    const ImVec2 p0{16.0f, 16.0f};
    const ImVec2 p1{p0.x + panel_w, p0.y + panel_h};

    dl->AddRectFilledMultiColor(p0, p1, PANEL_TOP, PANEL_TOP, PANEL_BOTTOM, PANEL_BOTTOM);
    dl->AddRect(p0, p1, BORDER, 0.0f, 0, 1.5f);

    // Corner brackets.
    {
        const float L = std::round(fs * 1.1f);
        const float th = 3.0f;
        const float o = 1.0f;
        dl->AddLine(ImVec2{p0.x + o, p0.y + o}, ImVec2{p0.x + L, p0.y + o}, ACCENT, th);
        dl->AddLine(ImVec2{p0.x + o, p0.y + o}, ImVec2{p0.x + o, p0.y + L}, ACCENT, th);
        dl->AddLine(ImVec2{p1.x - o, p0.y + o}, ImVec2{p1.x - L, p0.y + o}, ACCENT, th);
        dl->AddLine(ImVec2{p1.x - o, p0.y + o}, ImVec2{p1.x - o, p0.y + L}, ACCENT, th);
        dl->AddLine(ImVec2{p0.x + o, p1.y - o}, ImVec2{p0.x + L, p1.y - o}, ACCENT, th);
        dl->AddLine(ImVec2{p0.x + o, p1.y - o}, ImVec2{p0.x + o, p1.y - L}, ACCENT, th);
        dl->AddLine(ImVec2{p1.x - o, p1.y - o}, ImVec2{p1.x - L, p1.y - o}, ACCENT, th);
        dl->AddLine(ImVec2{p1.x - o, p1.y - o}, ImVec2{p1.x - o, p1.y - L}, ACCENT, th);
    }

    // ---- Title ----
    const float left = p0.x + pad;
    const float right = p1.x - pad;
    float y = p0.y + pad * 0.8f;
    {
        const float tx = left + spaced(bold, title_size, left, y, TEXT_BRIGHT, "PRAGMATA VR", title_spacing, true) + fs * 0.6f;
        spaced(bold, title_size, tx, y, ACCENT, "MOD MENU", title_spacing, true);
        put(font, small_size, right - text_w(font, small_size, "v0.9.2 B60"), y + (title_size - small_size) * 0.75f, TEXT_DIM, "v0.9.2 B60");

        const float ly = std::floor(y + title_size * 1.25f + fs * 0.25f);
        dl->AddLine(ImVec2{left, ly}, ImVec2{right, ly}, ACCENT_FAINT, 1.0f);
        dl->AddLine(ImVec2{left, ly}, ImVec2{left + fs * 4.0f, ly}, ACCENT, 3.0f);
        y += title_block;
    }

    // ---- Bands: MOD MENU SETTINGS on top, GAME HUD SETTINGS below, each with its own two columns ----
    const std::array<float, 2> col_x{left, left + col_w[0] + gutter};
    std::array<float, 2> band_top{};

    const float content_top = y;
    y -= std::floor(s_scroll);
    dl->PushClipRect(ImVec2{p0.x, content_top}, ImVec2{p1.x, content_top + view_h}, true);

    {
        // Build 36: tab bar. Tab titles in the band-title style: the current one white with a cyan underline, the
        // others dim; LT and RT at the ends. Same full-width rule as the band titles had.
        const float ly = std::floor(y + tier_size * 1.3f);
        put(bold, small_size, left, y + (tier_size - small_size) * 0.7f, TEXT_DIM, "LT");
        put(bold, small_size, right - text_w(bold, small_size, "RT"), y + (tier_size - small_size) * 0.7f, TEXT_DIM, "RT");
        float tx = std::floor(left + (right - left - tabs_w) * 0.5f);
        for (int ti = 0; ti < pragmatavr::TabCount; ++ti) {
            const bool active = ti == cur_tab;
            const float w = spaced(bold, tier_size, tx, y, active ? TEXT_BRIGHT : TEXT_DIM, pragmatavr::TAB_TITLES[ti], tier_spacing, true);
            if (active) {
                dl->AddLine(ImVec2{tx, ly}, ImVec2{tx + w, ly}, ACCENT, 3.0f);
            }
            tx += w + tab_gap;
        }
        dl->AddLine(ImVec2{left, ly}, ImVec2{right, ly}, ACCENT_DIM, 1.0f);
        y += tier_block;

        band_top[0] = y;
        band_top[1] = y;
        const float dx = std::floor(col_x[1] - gutter * 0.5f);
        dl->AddLine(ImVec2{dx, y + fs * 0.3f}, ImVec2{dx, y + band_h[0]}, ACCENT_FAINT, 1.0f);
        y += band_h[0];
    }

    std::array<std::array<float, 2>, 2> cy{{{band_top[0], band_top[0]}, {band_top[1], band_top[1]}}};
    col_first = {{{true, true}, {true, true}}};

    const pragmatavr::MenuRow* selected_row = nullptr;

    for (int i = 0; i < (int)rows.size(); ++i) {
        const auto& row = rows[i];
        const int c = std::clamp(row.column, 0, 1);
        const int bd = std::clamp(row.band, 0, 1);
        const float x0 = col_x[c];
        const float x1 = x0 + col_w[c];
        float& ry = cy[bd][c];

        if (row.tab != cur_tab || pragmatavr::row_hidden(t.collapsed, row)) {
            continue;
        }

        if (row.kind == Kind::Header) {
            ry += col_first[bd][c] ? header_gap_first : header_gap;
            col_first[bd][c] = false;

            // Build 25: a selectable (collapsible) header gets the same highlight as a selected row.
            if (i == sel && row.section >= 0) {
                selected_row = &row;
                const float hy0 = ry - std::round(header_size * 0.2f);
                const float hy1 = ry + header_size * 1.3f - 1.0f;
                dl->AddRectFilledMultiColor(ImVec2{x0, hy0}, ImVec2{x1, hy1}, SELECT_LEFT, SELECT_RIGHT, SELECT_RIGHT, SELECT_LEFT);
                dl->AddRectFilled(ImVec2{x0, hy0}, ImVec2{x0 + 4.0f, hy1}, ACCENT);
            }

            const float sq = std::round(header_size * 0.32f);
            const float sy = ry + (header_size - sq) * 0.55f;
            dl->AddRectFilled(ImVec2{x0, std::floor(sy)}, ImVec2{x0 + sq, std::floor(sy + sq)}, ACCENT);
            spaced(bold, header_size, x0 + header_size * 0.95f, ry, ACCENT, row.label.c_str(), header_size * 0.14f, true);

            if (const auto htag = header_tag(row); !htag.empty()) {
                const bool off = row.group >= 0 && row.group < pragmatavr::GroupCount && t.layout_mode == 1;
                const bool hsel = i == sel && row.section >= 0;
                put(font, small_size, x1 - text_w(font, small_size, htag.c_str()), ry + (header_size - small_size) * 0.6f,
                    hsel ? TEXT_BRIGHT : (off ? TEXT_OFF : TEXT_DIM), htag.c_str());
            }

            const float hl = std::floor(ry + header_size * 1.3f);
            dl->AddLine(ImVec2{x0, hl}, ImVec2{x1, hl}, ACCENT_FAINT, 1.0f);
            ry += header_body;
            continue;
        }

        const bool selected = i == sel;
        const bool row_focus = selected && !(on_invert && row.kind == Kind::Value); // highlight on the row itself
        const bool inv_focus = selected && on_invert && row.kind == Kind::Value;   // highlight on its INV box
        const bool row_editing = selected && editing && row.kind == Kind::Value;
        const auto v = pragmata_row_view(t, row);
        const float ty = ry + (line_h - fs) * 0.5f;

        if (selected) {
            selected_row = &row;
        }

        if (row_focus) {
            dl->AddRectFilledMultiColor(ImVec2{x0, ry + 2.0f}, ImVec2{x1, ry + line_h - 2.0f}, SELECT_LEFT, SELECT_RIGHT, SELECT_RIGHT, SELECT_LEFT);
            dl->AddRectFilled(ImVec2{x0, ry + 2.0f}, ImVec2{x0 + 4.0f, ry + line_h - 2.0f}, ACCENT);
        }

        const ImU32 label_col = row_focus ? TEXT_BRIGHT : (v.dim ? TEXT_OFF : TEXT);
        put(row_focus ? bold : font, fs, x0 + indent, ty, label_col, row.label.c_str());

        const float value_right = x1 - indent * 0.5f - invert_w - invert_gap;

        if (!v.value.empty()) {
            const float vw = text_w(bold, fs, v.value.c_str());
            ImU32 vcol = v.dim ? TEXT_OFF : (row_focus && v.value_color == TEXT_BRIGHT ? TEXT_BRIGHT : v.value_color);
            if (row_editing) {
                vcol = ACCENT; // the value being edited
            }
            put(bold, fs, value_right - vw, ty, vcol, v.value.c_str());

            // Left / right arrows around the value while it is being edited.
            if (row_editing) {
                const float a = std::round(fs * 0.22f);
                const float my = ry + line_h * 0.5f;
                const float lx = value_right - vw - fs * 0.45f;
                const float rx = value_right + fs * 0.45f;
                dl->AddTriangleFilled(ImVec2{lx, my}, ImVec2{lx + a, my - a}, ImVec2{lx + a, my + a}, ACCENT);
                dl->AddTriangleFilled(ImVec2{rx, my}, ImVec2{rx - a, my - a}, ImVec2{rx - a, my + a}, ACCENT);
            }
        } else if (row.kind == Kind::Action) {
            // Action rows: a small chevron in the value column.
            const float a = std::round(fs * 0.2f);
            const float my = ry + line_h * 0.5f;
            const float ax = value_right;
            const ImU32 acol = v.dim ? TEXT_OFF : (row_focus ? ACCENT : ACCENT_DIM);
            dl->AddTriangleFilled(ImVec2{ax, my}, ImVec2{ax - a * 1.2f, my - a}, ImVec2{ax - a * 1.2f, my + a}, acol);
        }

        if (v.show_invert) {
            const float bx = x1 - indent * 0.5f - invert_w;
            const float by = std::floor(ry + (line_h - box) * 0.5f);

            if (inv_focus) {
                // Highlight on the INV box: same selection fill and cyan edge, around the box and its label.
                const ImVec2 h0{bx - std::round(fs * 0.3f), ry + 2.0f};
                const ImVec2 h1{x1, ry + line_h - 2.0f};
                dl->AddRectFilled(h0, h1, SELECT_LEFT, 3.0f);
                dl->AddRect(h0, h1, ACCENT, 3.0f, 0, 1.5f);
            }

            if (v.invert_on) {
                dl->AddRectFilled(ImVec2{bx, by}, ImVec2{bx + box, by + box}, v.dim ? TEXT_OFF : ACCENT, 2.0f);
            } else {
                dl->AddRect(ImVec2{bx, by}, ImVec2{bx + box, by + box}, inv_focus ? TEXT_BRIGHT : (v.dim ? TEXT_OFF : TEXT_DIM), 2.0f, 0, 1.5f);
            }
            put(bold, small_size, bx + box + std::round(fs * 0.3f), ry + (line_h - small_size) * 0.5f,
                inv_focus ? TEXT_BRIGHT : (v.dim ? TEXT_OFF : (v.invert_on ? ACCENT : TEXT_DIM)), "INV");
        }

        ry += line_h;
    }

    dl->PopClipRect();

    if (scrolls) {
        const float a = std::round(fs * 0.28f);
        const float ax = p1.x - std::round(pad * 0.45f);
        if (s_scroll > 0.5f) {
            const float ay = content_top + a;
            dl->AddTriangleFilled(ImVec2{ax, ay - a * 0.6f}, ImVec2{ax + a, ay + a * 0.6f}, ImVec2{ax - a, ay + a * 0.6f}, ACCENT);
        }
        if (s_scroll < content_h - view_h - 0.5f) {
            const float ay = content_top + view_h - a;
            dl->AddTriangleFilled(ImVec2{ax, ay + a * 0.6f}, ImVec2{ax - a, ay - a * 0.6f}, ImVec2{ax + a, ay - a * 0.6f}, ACCENT);
        }
    }

    // ---- Footer: help for the highlighted item, controller chips, keyboard ----
    y = content_top + view_h + std::round(fs * 0.8f);
    dl->AddLine(ImVec2{left, std::floor(y - fs * 0.35f)}, ImVec2{right, std::floor(y - fs * 0.35f)}, ACCENT_FAINT, 1.0f);

    if (selected_row != nullptr) {
        const bool per_panel = selected_row->group >= 0 && selected_row->group < pragmatavr::GroupCount && t.layout_mode == 1;
        if (selected_row->kind == Kind::Header) {
            put(font, hint_size, left, y, TEXT, HEADER_HINT);
        } else if (editing) {
            put(font, hint_size, left, y, ACCENT, EDIT_HINT);
        } else if (on_invert && selected_row->kind == Kind::Value) {
            put(font, hint_size, left, y, TEXT, INV_HINT);
        } else if (per_panel) {
            put(font, hint_size, left, y, AMBER, PER_PANEL_HINT);
        } else {
            put(font, hint_size, left, y, TEXT, selected_row->hint.c_str());
        }
    }
    y += std::round(hint_size * 1.6f);

    float cx = left;
    for (const auto& [button, what] : editing ? EDIT_CHIPS : NAV_CHIPS) {
        if (*button != 0) {
            cx += chip(cx, y, button, what, true);
        }
    }
    y += std::round(small_size * 1.35f + small_size * 0.7f);

    put(font, small_size, left, y, TEXT_OFF, KEYBOARD_LINE);

    // ---- Hand the rectangle to the compositor ----
    const auto& io = ImGui::GetIO();
    if (p1.x > io.DisplaySize.x || p1.y > io.DisplaySize.y) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            spdlog::warn("[PragmataVR] Menu ({:.0f} x {:.0f}) is larger than the ImGui target ({:.0f} x {:.0f}); it will be cut off",
                panel_w, panel_h, io.DisplaySize.x, io.DisplaySize.y);
        }
    }

    hud.menu_rect_x = p0.x;
    hud.menu_rect_y = p0.y;
    hud.menu_rect_w = panel_w;
    hud.menu_rect_h = panel_h;
    hud.menu_rect_valid = true;
}
#endif

void VR::on_present() {
#if defined(PRAGMATA)
    pragmata_setup_gamepad();
    pragmata_report_xinput_use();
    pragmatavr::HudTuning::get().poll_keys();
    pragmatavr::HudTuning::get().poll_pad();
    // Build 40: Recenter view (mod menu, GENERAL tab): forward and position reset to where the player is now, the same
    // as REFramework's own recenter and standing origin keys, so Quest players do not need a controller to recenter.
    if (pragmatavr::HudTuning::get().view_recenter_requested.exchange(false) && is_hmd_active()) {
        recenter_view();
        set_standing_origin(get_position(0));
        spdlog::info("[PragmataVR] View recentered");
    }
    if (is_hmd_active()) {
        const auto hmd_pos = get_position(0);
        pragmatavr::HudTuning::get().update_head(get_rotation(0), glm::vec3{hmd_pos.x, hmd_pos.y, hmd_pos.z});
    }

    // Build 35: PERFORMANCE section of the mod menu (pragmatavr_hud.ini) drives these REFramework settings.
    // The ini is the source of truth; re2_fw_config.txt values for them are overridden.
    {
        const auto pt = pragmatavr::HudTuning::get().tune();
        // Build 55: NATIVE while a full-screen menu is open (drawn in the last second), if menu_native is on.
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool in_menu = pt->menu_native && now_ms - pragmatavr::g_menu_drawn_ms.load(std::memory_order_relaxed) < 1000;
        static bool last_in_menu = false;
        if (pt->render_afw && in_menu != last_in_menu) {
            spdlog::info("[PragmataVR] Full-screen menu {}: render {}", in_menu ? "open" : "closed", in_menu ? "NATIVE" : "AFW");
        }
        last_in_menu = in_menu;
        const int want_technique = pt->render_afw && !in_menu ? ALTERNATE_FRAME_WARPING : SEQUENTIAL_FRAME;
        bool changed = false;

        if (m_rendering_technique->value() != want_technique) {
            m_rendering_technique->value() = want_technique;
            changed = true;
        }
        if (m_enable_sharpening->value() != pt->sharpening) {
            m_enable_sharpening->value() = pt->sharpening;
            changed = true;
        }
        if (m_desktop_fix->value() != pt->desktop_mirror) {
            m_desktop_fix->value() = pt->desktop_mirror;
            changed = true;
        }
        pragmatavr::perf::g_enabled.store(pt->perf_log, std::memory_order_relaxed);

        static bool logged_once = false;
        static bool last_perf_log = true;
        if (changed || !logged_once || last_perf_log != pt->perf_log) {
            logged_once = true;
            last_perf_log = pt->perf_log;
            spdlog::info("[PragmataVR] Perf settings: render mode {}, sharpening {} ({:.2f}), desktop mirror {}, eye image {} x {}, "
                         "resolution scale {:.2f}, perf log {}",
                is_using_sf() ? "NATIVE (Two Frame Sequential)" : "AFW (Alternate Frame Warping)",
                m_enable_sharpening->value() ? "ON" : "OFF", m_sharpness->value(), m_desktop_fix->value() ? "ON" : "OFF",
                get_hmd_width(), get_hmd_height(), m_resolution_scale->value(), pt->perf_log ? "ON" : "OFF");
        }

        if (pt->perf_log) {
            pragmatavr::perf::Stats::get().maybe_report(is_using_sf() ? "NATIVE" : "AFW");
        }
    }

    // Build 35: time spent in this function (render thread: eye submission and copies).
    struct PragmataPresentTimer {
        int64_t t0{pragmatavr::perf::now_ns()};
        ~PragmataPresentTimer() {
            if (pragmatavr::perf::g_enabled.load(std::memory_order_relaxed)) {
                pragmatavr::perf::Stats::get().present(pragmatavr::perf::now_ns() - t0);
            }
        }
    } pragmata_present_timer{};
#endif
    

#if !defined(PRAGMATA) // Build 35: PureDark's numpad hotkeys are off in PragmataVR (the mod menu has the settings)
    static bool btn1 = false;
    if (GetAsyncKeyState(VK_NUMPAD1) < 0 && btn1 == false) {
        btn1 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD1) == 0 && btn1 == true) {
        btn1 = false;
        mDebug1 = !mDebug1;
    }
    static bool btn2 = false;
    if (GetAsyncKeyState(VK_NUMPAD2) < 0 && btn2 == false) {
        btn2 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD2) == 0 && btn2 == true) {
        btn2 = false;
        mDebug2 = !mDebug2;
        //m_disable_volumetric_fog->toggle();
    }
    static bool btn3 = false;
    if (GetAsyncKeyState(VK_NUMPAD3) < 0 && btn3 == false) {
        btn3 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD3) == 0 && btn3 == true) {
        btn3 = false;
        mDebug3 = !mDebug3;
        mDebug2 = false;
        mDebug1 = false;
    }
    static bool btn4 = false;
    if (GetAsyncKeyState(VK_NUMPAD4) < 0 && btn4 == false) {
        btn4 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD4) == 0 && btn4 == true) {
        btn4 = false;
        m_fix_upscalers_wobbling->toggle();
    }
#endif

    // case 1: sequential frames and rendered frame == right eye (every two frames)
    // case 2: AFW and no foveated rendering (each frame)
    // case 3: AFW and foveated rendering
    if ((is_using_sf() && m_render_frame_count % 2 == m_right_eye_interval) || 
        (is_using_afw()) ||
        (is_using_afw_foveated())) {
        ResetEvent(m_present_finished_event);
    }

    auto runtime = get_runtime();

    if (!get_runtime()->loaded) {
        return;
    }

    auto openvr = get_runtime<runtimes::OpenVR>();

    if (runtime->is_openvr()) {
        if (openvr->got_first_poses) {
            const auto hmd_activity = openvr->hmd->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            auto hmd_active = hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction || hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;

            if (hmd_active) {
                openvr->last_hmd_active_time = std::chrono::system_clock::now();
            }

            const auto now = std::chrono::system_clock::now();

            if (now - openvr->last_hmd_active_time <= std::chrono::seconds(5)) {
                hmd_active = true;
            }

            openvr->is_hmd_active = hmd_active;
            openvr->is_hmd_active = true;

            // upon headset re-entry, reinitialize OpenVR
            if (openvr->is_hmd_active && !openvr->was_hmd_active) {
                openvr->wants_reinitialize = true;
            }

            openvr->was_hmd_active = openvr->is_hmd_active;

            if (!is_hmd_active()) {
                return;
            }
        } else {
            openvr->is_hmd_active = true; // We need to force out an initial WaitGetPoses call
            openvr->was_hmd_active = true;
        }
    }

    // attempt to fix crash when reinitializing openvr
    std::scoped_lock _{m_openvr_mtx};
    m_submitted = false;
#if !defined(PRAGMATA) // Build 35: numpad hotkeys off in PragmataVR
    static bool btn5 = false;
    if (GetAsyncKeyState(VK_NUMPAD5) < 0 && btn5 == false) {
        btn5 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD5) == 0 && btn5 == true) {
        btn5 = false;
        m_enable_foveated_rendering->toggle();
    }
    static bool btn6 = false;
    if (GetAsyncKeyState(VK_NUMPAD6) < 0 && btn6 == false) {
        btn6 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD6) == 0 && btn6 == true) {
        btn6 = false;
        m_framewarp_debug->toggle();
    }
    static bool btn7 = false;
    if (GetAsyncKeyState(VK_NUMPAD7) < 0 && btn7 == false) {
        btn7 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD7) == 0 && btn7 == true) {
        btn7 = false;
        int32_t& value = m_rendering_technique->value();
        value = (value + 1) % 2;
    }
    static bool btn8 = false;
    if (GetAsyncKeyState(VK_NUMPAD8) < 0 && btn8 == false) {
        btn8 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD8) == 0 && btn8 == true) {
        btn8 = false;
        m_force_fixed_foveated->toggle();
    }
    static bool btn9 = false;
    if (GetAsyncKeyState(VK_NUMPAD9) < 0 && btn9 == false) {
        btn9 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD9) == 0 && btn9 == true) {
        btn9 = false;
        m_enable_ui_fix->toggle();
    }
    static bool btnAdd = false;
    if (GetAsyncKeyState(VK_ADD) < 0 && btnAdd == false) {
        btnAdd = true;
    }
    if (GetAsyncKeyState(VK_ADD) == 0 && btnAdd == true) {
        btnAdd = false;
        auto& sharpness = m_sharpness->value();
        sharpness += 0.05f;
    }
    static bool btnSub = false;
    if (GetAsyncKeyState(VK_SUBTRACT) < 0 && btnSub == false) {
        btnSub = true;
    }
    if (GetAsyncKeyState(VK_SUBTRACT) == 0 && btnSub == true) {
        btnSub = false;
        auto& sharpness = m_sharpness->value();
        sharpness -= 0.05f;
    }
    static bool btnMul = false;
    if (GetAsyncKeyState(VK_MULTIPLY) < 0 && btnMul == false) {
        btnMul = true;
    }
    if (GetAsyncKeyState(VK_MULTIPLY) == 0 && btnMul == true) {
        btnMul = false;
        m_enable_sharpening->toggle();
    }
#endif

    // only update camera data when not rendering FR frame (which has lower FOV)
    update_camera_data(m_render_frame_count);

    const auto renderer = g_framework->get_renderer_type();
    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (renderer == REFramework::RendererType::D3D11) {
        // if we don't do this then D3D11 OpenXR freezes for some reason.
        if (!runtime->got_first_sync) {
            runtime->synchronize_frame();
            runtime->update_poses();
        }

        m_is_d3d12 = false;
        e = m_d3d11.on_frame(this);
    } else if (renderer == REFramework::RendererType::D3D12) {
        m_is_d3d12 = true;
        e = m_d3d12.on_frame(this);
    }

    // force a waitgetposes call to fix this...
    if (e == vr::EVRCompositorError::VRCompositorError_AlreadySubmitted && runtime->is_openvr()) {
        openvr->got_first_poses = false;
        openvr->needs_pose_update = true;
    }

    m_last_frame_count = m_render_frame_count;

    if (m_submitted || runtime->needs_pose_update) {
        if (m_submitted) {
            m_overlay_component.on_post_compositor_submit();

            if (runtime->is_openvr()) {
                //vr::VRCompositor()->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
                vr::VRCompositor()->PostPresentHandoff();
            }
        }

        runtime->needs_pose_update = true;
        m_submitted = false;
    }

    // case 1: sequential frames and rendered frame == right eye (every two frames)
    // case 2: AFW and no foveated rendering (each frame)
    // case 3: AFW and foveated rendering
    if ((is_using_sf() && m_render_frame_count % 2 == m_right_eye_interval) || 
        (is_using_afw()) ||
        (is_using_afw_foveated())) {
        SetEvent(m_present_finished_event);
    }
}

void VR::on_post_present() {
    

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    const auto renderer = g_framework->get_renderer_type();

    if (renderer == REFramework::RendererType::D3D12) {
        m_d3d12.on_post_present(this);
    }
    
    // case 1: sequential frames and rendered frame == right eye (every two frames)
    // case 2: AFW and no foveated rendering (each frame)
    // case 3: AFW and foveated rendering
    if ((is_using_sf() && m_render_frame_count % 2 == m_right_eye_interval) || 
        (is_using_afw()) ||
        (is_using_afw_foveated())) {
        runtime->consume_events(nullptr);
    }

    if (!inside_on_end && runtime->wants_reinitialize) {
        std::scoped_lock _{m_openvr_mtx};

        if (runtime->is_openvr()) {
            m_openvr->wants_reinitialize = false;
            reinitialize_openvr();
        } else if (runtime->is_openxr()) {
            m_openxr->wants_reinitialize = false;
            reinitialize_openxr();
        }
    }
}

void VR::on_update_transform(RETransform* transform) {
    
}

void VR::on_update_camera_controller(RopewayPlayerCameraController* controller) {
    // get headset rotation
    /*const auto& headset_pose = m_game_poses[0];

    if (!headset_pose.bPoseIsValid) {
        return;
    }

    auto headset_matrix = Matrix4x4f{ *(Matrix3x4f*)&headset_pose.mDeviceToAbsoluteTracking };
    auto headset_rotation = glm::extractMatrixRotation(glm::rowMajor4(headset_matrix));

    headset_rotation *= get_current_rotation_offset();

    *(glm::quat*)&controller->worldRotation = glm::quat{ headset_rotation  };

    controller->worldPosition += get_current_offset();*/
}

struct GUIRestoreData {
    REComponent* element{nullptr};
    REComponent* view{nullptr};
    Vector4f original_position{ 0.0f, 0.0f, 0.0f, 1.0f };
    via::gui::ViewType view_type{ via::gui::ViewType::Screen };
    bool overlay{false};
    bool detonemap{true};
};

thread_local std::vector<std::unique_ptr<GUIRestoreData>> g_elements_to_reset{};

#if defined(PRAGMATA)
struct PragmataChildRestore {
    REManagedObject* obj{nullptr};
    Vector4f position{};
    Vector4f scale{};
    bool restore_position{false};
    bool restore_visible{false}; // Build 22: only make obj visible again (frosted glass hidden for one draw)
    bool restore_mask{false};    // Build 23: only put obj's MaskMode back
    uint32_t mask_mode{0};
};

struct PragmataPanelRestore {
    REComponent* view{nullptr};
    Vector4f position{};
    std::vector<PragmataChildRestore> children{};
    // Build 15 companions: the view's own settings and the GUI object's world matrix, put back after the draw.
    bool companion{false};
    uint32_t view_type{0};
    bool overlay{false};
    bool detonemap{false};
    Vector4f view_scale{};
    RETransform* transform{nullptr};
    Matrix4x4f world{};
};

// Build 15: the 3D partner panels' own settings (before any tuning), captured as they draw, so their Screen-mode
// companions can be drawn the same way. Also the partner's element center, so both halves grow around one point.
namespace pragmata_companion {
struct Partner {
    Vector4f position{};
    Vector4f scale{};
    Vector4f rotation{};
    bool overlay{true};
    bool detonemap{false};
    Matrix4x4f world{1.0f};
    std::optional<Vector2f> pivot{};
    std::chrono::steady_clock::time_point seen{};
};

std::mutex g_mtx{};
std::unordered_map<std::string, Partner> g_partners{};
std::unordered_map<size_t, int> g_logged{}; // name hash -> companion generation logged

void log_view(const std::string& what, const std::string& name, REComponent* view, RETransform* transform, sdk::VMContext* ctx) {
    const auto pos = sdk::call_object_func_easy<Vector4f>(view, "get_Position");
    const auto scale = sdk::call_object_func_easy<Vector4f>(view, "get_Scale");
    const auto rot = sdk::call_object_func_easy<Vector4f>(view, "get_Rotation");
    regenny::via::Size size{};
    sdk::call_object_func<void*>((REManagedObject*)view, "get_ScreenSize", &size, ctx, (REManagedObject*)view);
    const Matrix4x4f w = transform != nullptr ? transform->worldTransform : Matrix4x4f{1.0f};
    spdlog::info("[PragmataVR] {} {}: viewType={} overlay={} detonemap={} pos ({:.3f}, {:.3f}, {:.3f}) scale ({:.4f}, {:.4f}, {:.4f}) "
        "rot ({:.3f}, {:.3f}, {:.3f}, {:.3f}) screen {:.0f}x{:.0f} | world pos ({:.3f}, {:.3f}, {:.3f}) axes {:.4f} {:.4f} {:.4f}",
        what, name, sdk::call_object_func<uint32_t>(view, "get_ViewType", ctx, view),
        sdk::call_object_func<bool>(view, "get_Overlay", ctx, view), sdk::call_object_func<bool>(view, "get_Detonemap", ctx, view),
        pos.x, pos.y, pos.z, scale.x, scale.y, scale.z, rot.x, rot.y, rot.z, rot.w, size.w, size.h,
        w[3].x, w[3].y, w[3].z, glm::length(glm::vec3{w[0]}), glm::length(glm::vec3{w[1]}), glm::length(glm::vec3{w[2]}));
}
} // namespace pragmata_companion


thread_local std::vector<PragmataPanelRestore> g_pragmata_panel_restore{};

// PragmataVR Build 11: where an element really is inside its panel.
// Pragmata lays these HUD panels out like a full game screen, with the element (health bar, weapon wheel, tutorial box)
// placed out toward a corner of that canvas. Scaling the panel's root around its origin (Builds 3-6) therefore also
// pushed the element away from the center by the same factor, so a big enough element ended up out of view. Here the
// panel's GUI tree is walked (local position and scale only, no rotation) and the center of the box around its visible
// leaf objects is used as the point to grow around. Results are cached per panel and refreshed every 16 frames.
namespace pragmata_pivot {
struct Entry {
    Vector2f pivot{0.0f, 0.0f};
    bool valid{false};
    int frame{-1000};
    bool logged{false};
};

std::mutex g_mtx{};
std::unordered_map<size_t, Entry> g_cache{};

bool is_visible(REManagedObject* o, sdk::VMContext* ctx) {
    auto td = utility::re_managed_object::get_type_definition(o);
    if (td == nullptr) {
        return true;
    }
    auto m = td->get_method("get_Visible");
    if (m == nullptr) {
        return true;
    }
    return m->call<bool>(ctx, o);
}

// Pieces that are not the element itself: helpers that draw nothing (ParamSetter), and full-screen layers (edge glows,
// screen effects, blur) that span the whole canvas. Seen in the Build 11 tree dumps of ui2130Gui and ui2150Gui.
bool skip_type(const std::string& t) {
    return t == "via.gui.ParamSetter" || t == "via.gui.Material" || t == "via.gui.Effect" || t == "via.gui.EffectTexture" ||
           t == "via.gui.BlurFilter";
}

struct Walk {
    sdk::VMContext* ctx{};
    Vector2f canvas{1920.0f, 1080.0f};
    Vector2f lo{(std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)()};
    Vector2f hi{-(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)()};
    int leaves{0};
    int nodes{0};
    std::string dump{};
    bool want_dump{false};

    // A point counts unless it sits on the canvas edge, at its top-left origin or exactly at its center: those are
    // layout anchors and full-screen layers, not the element.
    bool add(const Vector2f& p) {
        const float e = 5.0f;
        if (p.x <= e || p.y <= e || p.x >= canvas.x - e || p.y >= canvas.y - e) {
            return false;
        }
        if (std::fabs(p.x - canvas.x * 0.5f) < 2.0f && std::fabs(p.y - canvas.y * 0.5f) < 2.0f) {
            return false;
        }
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
        ++leaves;
        return true;
    }

    // obj's children, placed with the parent's accumulated offset and scale. Returns how many points were added.
    int children(REManagedObject* obj, const Vector2f& offset, const Vector2f& scale, int depth) {
        if (depth > 14) {
            return 0;
        }
        int added = 0;
        auto c = sdk::call_object_func<REManagedObject*>(obj, "get_Child", ctx, obj);
        for (int n = 0; c != nullptr && n < 256 && nodes < 1024; ++n) {
            ++nodes;
            const bool vis = is_visible(c, ctx);
            const auto p4 = sdk::call_object_func_easy<Vector4f>(c, "get_Position");
            const auto s4 = sdk::call_object_func_easy<Vector4f>(c, "get_Scale");
            const Vector2f p = offset + scale * Vector2f{p4.x, p4.y};
            const Vector2f sc = scale * Vector2f{s4.x, s4.y};
            const auto first_child = sdk::call_object_func<REManagedObject*>(c, "get_Child", ctx, c);
            auto td = utility::re_managed_object::get_type_definition(c);
            const std::string type_name = td != nullptr ? td->get_full_name() : std::string{"?"};

            if (want_dump && dump.size() < 6000 && type_name != "via.gui.ParamSetter") {
                dump += fmt::format("\n    {:>{}}{} vis={} pos=({:.0f},{:.0f}) scale=({:.2f},{:.2f}) -> ({:.0f},{:.0f})",
                    "", depth * 2, type_name, vis ? 1 : 0, p4.x, p4.y, s4.x, s4.y, p.x, p.y);
            }

            if (vis && !skip_type(type_name)) {
                // A visible piece counts as a point when nothing below it does (a leaf, or a group whose children
                // are all hidden or skipped).
                const int below = first_child != nullptr ? children(c, p, sc, depth + 1) : 0;
                added += below;
                if (below == 0 && add(p)) {
                    ++added;
                }
            }
            c = sdk::call_object_func<REManagedObject*>(c, "get_Next", ctx, c);
        }
        return added;
    }
};

// Center of the element's visible pieces, in the space the view's top-level children are positioned in.
std::optional<Vector2f> get(size_t hash, const std::string& name, REComponent* view, sdk::VMContext* ctx, int frame) {
    {
        std::scoped_lock _{g_mtx};
        auto& e = g_cache[hash];
        if (frame - e.frame >= 0 && frame - e.frame < 16) {
            return e.valid ? std::optional<Vector2f>{e.pivot} : std::nullopt;
        }
    }

    bool want_dump = false;
    {
        std::scoped_lock _{g_mtx};
        want_dump = !g_cache[hash].logged;
    }

    Walk w{};
    w.ctx = ctx;
    w.want_dump = want_dump;
    {
        regenny::via::Size gui_size{};
        sdk::call_object_func<void*>((REManagedObject*)view, "get_ScreenSize", &gui_size, ctx, (REManagedObject*)view);
        if (gui_size.w > 100.0f && gui_size.h > 100.0f) {
            w.canvas = Vector2f{gui_size.w, gui_size.h};
        }
    }
    w.children((REManagedObject*)view, Vector2f{0.0f, 0.0f}, Vector2f{1.0f, 1.0f}, 0);

    Entry out{};
    out.frame = frame;
    if (w.leaves > 0) {
        out.pivot = (w.lo + w.hi) * 0.5f;
        out.valid = true;
    }

    std::scoped_lock _{g_mtx};
    auto& e = g_cache[hash];
    out.logged = e.logged;
    if (want_dump && out.valid) {
        out.logged = true;
        spdlog::info("[PragmataVR] Element center for {}: ({:.0f}, {:.0f}) from {} visible pieces of {} objects, box ({:.0f},{:.0f})-({:.0f},{:.0f}), canvas {:.0f}x{:.0f}. Tree:{}",
            name, out.pivot.x, out.pivot.y, w.leaves, w.nodes, w.lo.x, w.lo.y, w.hi.x, w.hi.y, w.canvas.x, w.canvas.y, w.dump);
    }
    e = out;
    return out.valid ? std::optional<Vector2f>{out.pivot} : std::nullopt;
}
} // namespace pragmata_pivot

// PragmataVR Build 22: the frosted glass pieces (via.gui.BlurFilter) of a HUD view, hidden for each FLAT draw. The tree
// is walked once every 120 frames (or when the view changes); the found objects are held with add_ref so a cached
// pointer can never point at freed memory if the game rebuilds the view in between.
namespace pragmata_blur {
struct Entry {
    REComponent* view{nullptr};
    std::vector<REManagedObject*> objs{};
    int frame{-100000};
};

std::mutex g_mtx{};
std::unordered_map<size_t, Entry> g_cache{};

void collect(REManagedObject* obj, sdk::VMContext* ctx, std::vector<REManagedObject*>& out, int depth, int& nodes) {
    if (depth > 14) {
        return;
    }
    auto c = sdk::call_object_func<REManagedObject*>(obj, "get_Child", ctx, obj);
    for (int n = 0; c != nullptr && n < 256 && nodes < 1024; ++n) {
        ++nodes;
        auto td = utility::re_managed_object::get_type_definition(c);
        if (td != nullptr && td->get_full_name() == "via.gui.BlurFilter") {
            out.push_back(c);
        } else {
            collect(c, ctx, out, depth + 1, nodes);
        }
        c = sdk::call_object_func<REManagedObject*>(c, "get_Next", ctx, c);
    }
}

// Copies the cached list into out (the objects stay referenced by the cache).
void get(size_t hash, const std::string& name, REComponent* view, sdk::VMContext* ctx, int frame, std::vector<REManagedObject*>& out) {
    std::scoped_lock _{g_mtx};
    auto& e = g_cache[hash];
    if (e.view != view || frame - e.frame >= 120 || frame - e.frame < 0) {
        for (auto o : e.objs) {
            ((sdk::ManagedObject*)o)->release();
        }
        const bool first = e.view == nullptr;
        e.objs.clear();
        int nodes = 0;
        collect((REManagedObject*)view, ctx, e.objs, 0, nodes);
        for (auto o : e.objs) {
            ((sdk::ManagedObject*)o)->add_ref();
        }
        e.view = view;
        e.frame = frame;
        if (first) {
            spdlog::info("[PragmataVR] Frosted glass in {}: {} BlurFilter pieces", name, e.objs.size());
        }
    }
    out = e.objs;
}
} // namespace pragmata_blur

// PragmataVR Build 23: every piece of a HUD view that has a MaskMode, for the Element masks test, and a one-time log
// of the mask setup of each view (pieces whose MaskType or MaskMode is not the default), to find what cuts off the
// shrunk tutorial box and weapon wheel. Objects are held with add_ref, refreshed every 120 frames like pragmata_blur.
namespace pragmata_mask {
struct Entry {
    REComponent* view{nullptr};
    std::vector<REManagedObject*> objs{};
    int frame{-100000};
    bool logged{false};
};

std::mutex g_mtx{};
std::unordered_map<size_t, Entry> g_cache{};

void collect(REManagedObject* obj, sdk::VMContext* ctx, std::vector<REManagedObject*>& out, const Vector2f& offset,
    const Vector2f& scale, int depth, int& nodes, std::string* dump) {
    if (depth > 14) {
        return;
    }
    auto c = sdk::call_object_func<REManagedObject*>(obj, "get_Child", ctx, obj);
    for (int n = 0; c != nullptr && n < 256 && nodes < 1024; ++n) {
        ++nodes;
        auto td = utility::re_managed_object::get_type_definition(c);
        const auto p4 = sdk::call_object_func_easy<Vector4f>(c, "get_Position");
        const auto s4 = sdk::call_object_func_easy<Vector4f>(c, "get_Scale");
        const Vector2f p = offset + scale * Vector2f{p4.x, p4.y};
        const Vector2f sc = scale * Vector2f{s4.x, s4.y};
        if (td != nullptr && td->get_method("set_MaskMode") != nullptr) {
            out.push_back(c);
            if (dump != nullptr && dump->size() < 8000) {
                const auto mtype = td->get_method("get_MaskType") != nullptr ? sdk::call_object_func<uint32_t>(c, "get_MaskType", ctx, c) : 99u;
                const auto mmode = td->get_method("get_MaskMode") != nullptr ? sdk::call_object_func<uint32_t>(c, "get_MaskMode", ctx, c) : 99u;
                if (mtype != 0 || mmode != 0) {
                    const bool vis = td->get_method("get_Visible") != nullptr ? sdk::call_object_func<bool>(c, "get_Visible", ctx, c) : true;
                    *dump += fmt::format("\n    depth {} {} vis={} MaskType={} MaskMode={} pos=({:.0f},{:.0f}) local=({:.0f},{:.0f}) scale=({:.2f},{:.2f})",
                        depth, td->get_full_name(), vis ? 1 : 0, mtype, mmode, p.x, p.y, p4.x, p4.y, s4.x, s4.y);
                }
            }
        }
        collect(c, ctx, out, p, sc, depth + 1, nodes, dump);
        c = sdk::call_object_func<REManagedObject*>(c, "get_Next", ctx, c);
    }
}

void get(size_t hash, const std::string& name, REComponent* view, sdk::VMContext* ctx, int frame, std::vector<REManagedObject*>& out) {
    std::scoped_lock _{g_mtx};
    auto& e = g_cache[hash];
    if (e.view != view || frame - e.frame >= 120 || frame - e.frame < 0) {
        for (auto o : e.objs) {
            ((sdk::ManagedObject*)o)->release();
        }
        e.objs.clear();
        int nodes = 0;
        std::string dump{};
        const bool want_log = !e.logged;
        collect((REManagedObject*)view, ctx, e.objs, Vector2f{0.0f, 0.0f}, Vector2f{1.0f, 1.0f}, 0, nodes, want_log ? &dump : nullptr);
        for (auto o : e.objs) {
            ((sdk::ManagedObject*)o)->add_ref();
        }
        e.view = view;
        e.frame = frame;
        if (want_log) {
            e.logged = true;
            spdlog::info("[PragmataVR] Masks in {}: {} pieces of {} have a MaskMode. Pieces with a mask setting (MaskType 0 Target, "
                "1 NonTarget, 2 Mask, 3 MaskTop, 4 MaskTopMost; MaskMode 0 Keep, 1 Default, 2 Reverse, 3 Disable, 4 ApplyToParent):{}",
                name, e.objs.size(), nodes, dump.empty() ? std::string{" none"} : dump);
        }
    }
    out = e.objs;
}
} // namespace pragmata_mask
#endif

bool VR::on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) {
    

    inside_gui_draw = true;

    if (!get_runtime()->ready()) {
        return true;
    }

    auto game_object = utility::re_component::get_game_object(gui_element);

    if (game_object != nullptr && game_object->transform != nullptr) {
        auto context = sdk::get_thread_context();

        const auto name = utility::re_game_object::get_name(game_object);
        const auto name_hash = utility::hash(name);

#if defined(PRAGMATA)
        pragmatavr::g_current_gui_hash = name_hash;
        pragmatavr::g_current_gui_name = name;

        // Build 55: note full-screen menus (NATIVE while they are open, see VR::on_frame).
        if (pragmatavr::is_fullscreen_menu(name)) {
            pragmatavr::g_menu_drawn_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
            static std::mutex menu_log_mtx{};
            static std::unordered_map<size_t, bool> menu_logged{};
            std::scoped_lock _{menu_log_mtx};
            if (menu_logged.emplace(name_hash, true).second) {
                spdlog::info("[PragmataVR] Full-screen menu drawn: {}", name);
            }
        }

        // Build 40-44 shifted enemy health bars and damage numbers by writing their pieces' positions here. The game
        // spawns and removes those pieces constantly and the writes crashed it (Builds 41-44, Quest 3). Build 45: they
        // are shifted in the GUI camera projection hook instead (no game object is touched).

        // PragmataVR Build 17: HUD panel mode. Health, stamina, weapon wheel and tutorial (their 3D panels and their
        // Screen-mode companions) are drawn flat in Screen mode, exactly as on a monitor, into the engine's UI buffer.
        // D3D12Component copies that buffer each frame onto a room-fixed OpenXR quad. REFramework's generic
        // Screen-to-World sheet and the per-element 3D tuning are skipped for them.
        {
            const auto ptune = pragmatavr::HudTuning::get().tune();
            std::string_view pn{};
            const int pgroup = pragmatavr::group_for(name);
            const bool element = (pgroup >= 0 && pgroup != pragmatavr::GroupHack) || pragmatavr::companion_group(name, pn) >= 0;

            if (element && ptune->enabled && ptune->hud_panel && !ptune->flat_test) { // Build 25: ANCHORED uses the FLAT path
                auto v = sdk::call_object_func<REComponent*>(gui_element, "get_View", context, gui_element);
                if (v != nullptr) {
                    const auto vt = sdk::call_object_func<uint32_t>(v, "get_ViewType", context, v);
                    if (vt != (uint32_t)via::gui::ViewType::Screen) {
                        PragmataPanelRestore restore{};
                        restore.view = v;
                        restore.companion = true; // reuse the companion restore: view type, overlay, detonemap, scale, world
                        restore.position = sdk::call_object_func_easy<Vector4f>(v, "get_Position");
                        restore.view_scale = sdk::call_object_func_easy<Vector4f>(v, "get_Scale");
                        restore.view_type = vt;
                        restore.overlay = sdk::call_object_func<bool>(v, "get_Overlay", context, v);
                        restore.detonemap = sdk::call_object_func<bool>(v, "get_Detonemap", context, v);
                        restore.transform = game_object->transform;
                        restore.world = game_object->transform->worldTransform;
                        sdk::call_object_func<void*>(v, "set_ViewType", context, v, (uint32_t)via::gui::ViewType::Screen);
                        g_pragmata_panel_restore.push_back(std::move(restore));
                    }

                    static thread_local std::unordered_map<size_t, bool> logged_panel{};
                    if (logged_panel.emplace(name_hash, true).second) {
                        spdlog::info("[PragmataVR] HUD panel: {} drawn flat (was view type {})", name, vt);
                    }
                }
                return true; // skip REFramework's generic sheet and the 3D tuning
            }
        }

        // PragmataVR Build 21: FLAT HUD mode. The GUI camera keeps the game's own flat projection (see the projection
        // hook), so every HUD element is drawn exactly as on a monitor, in both eyes. Here each element (its 3D panel and
        // its Screen-mode companion, which stays flat) is grown around the element's center and moved, in canvas pixels:
        // child position -> center + size * (position - center) + (x + eye shift, -y), child scale -> size * scale.
        // The 3D panel's center is shared with its companion so both halves stay together. Restored after the draw.
        {
            const auto ftune = pragmatavr::HudTuning::get().tune();

            if (ftune->enabled && ftune->flat_test) {
                std::string_view fpartner{};
                int fgroup = pragmatavr::group_for(name);
                const bool fnative = fgroup >= 0;
                if (!fnative) {
                    fgroup = pragmatavr::companion_group(name, fpartner);
                }

                if (fgroup >= 0) {
                    // Build 28: ANCHORED uses boxes of drawn elements only. Build 58: ui2010Gui draws all the time, so the
                    // subtitles count as drawn only while a line is on screen.
                    if (fgroup != pragmatavr::GroupSubtitles || pragmatavr::g_subtitles_shown.load(std::memory_order_relaxed)) {
                        pragmatavr::g_group_drawn_frame[fgroup] = m_frame_count;
                    }
                    auto fview = sdk::call_object_func<REComponent*>(gui_element, "get_View", context, gui_element);
                    const auto& gt = pragmatavr::active_group(*ftune, fgroup); // Build 38: hack uses its headset values when set
                    // Build 25: ANCHORED takes depth from the panel distance, so the anchored elements get no eye gap
                    // (the UI buffer is shared by both eyes, so it must match in both).
                    // Build 60: the reticle stays in the eyes like the hacking circle (only its eye gap is applied).
                    const bool anchored = ftune->hud_panel && fgroup != pragmatavr::GroupHack && fgroup != pragmatavr::GroupReticle;
                    const float eye_shift = anchored ? 0.0f : (is_left_eye() ? gt.gap : -gt.gap);
                    // Build 38: Fixed mode draws the element at its own place (size only); the panel applies Left / right and
                    // Up / down (AnchoredBox ox / oy), so the cut-out area stays at the screen edge.
                    // Build 39: with Fixed HUD capture = CORNERS the element is drawn in a corner of the eye image instead.
                    const auto cshift = anchored ? pragmatavr::fixed_capture_shift(*ftune, fgroup, (float)get_hmd_width(), (float)get_hmd_height())
                                                 : glm::vec2{0.0f};
                    const float dx = anchored ? cshift.x : gt.x + eye_shift;
                    const float dy = anchored ? cshift.y : gt.y; // canvas y points down; Build 22: Up / down positive moves the element down
                    // Build 56: panel elements are drawn at full size; their Size is applied to their own OpenXR quad, so the
                    // game's fixed-size windows (objective, item pickups, tutorial) never cut them.
                    const float k = anchored ? 1.0f : gt.size;
                    const bool hide_blur = ftune->hide_blur;
                    // Build 24: the 3D panel (not the hacking circle) drawn as a Screen view, like its companion.
                    const auto fvt = fview != nullptr ? sdk::call_object_func<uint32_t>(fview, "get_ViewType", context, fview) : 0u;
                    const bool to_screen = fnative && fgroup != pragmatavr::GroupHack && fgroup != pragmatavr::GroupReticle &&
                                           (ftune->flat_screen || ftune->hud_panel) &&
                                           fview != nullptr && fvt != (uint32_t)via::gui::ViewType::Screen;
                    const bool screen_drawn = to_screen || (fview != nullptr && fvt == (uint32_t)via::gui::ViewType::Screen);

                    if (fview != nullptr && (k != 1.0f || dx != 0.0f || dy != 0.0f || hide_blur || to_screen)) {
                        // Build 22: fixed center per element (see pragmatavr::FLAT_PIVOTS), shared by both halves.
                        const float kk = k;
                        const Vector2f pv{ pragmatavr::FLAT_PIVOTS[fgroup][0], pragmatavr::FLAT_PIVOTS[fgroup][1] };

                        PragmataPanelRestore restore{};
                        restore.view = fview;
                        restore.position = sdk::call_object_func_easy<Vector4f>(fview, "get_Position");

                        if (to_screen) {
                            // Same restore as the PANEL mode switch: view type, overlay, detonemap, scale, world matrix.
                            restore.companion = true;
                            restore.view_scale = sdk::call_object_func_easy<Vector4f>(fview, "get_Scale");
                            restore.view_type = fvt;
                            restore.overlay = sdk::call_object_func<bool>(fview, "get_Overlay", context, fview);
                            restore.detonemap = sdk::call_object_func<bool>(fview, "get_Detonemap", context, fview);
                            restore.transform = game_object->transform;
                            restore.world = game_object->transform->worldTransform;
                            sdk::call_object_func<void*>(fview, "set_ViewType", context, fview, (uint32_t)via::gui::ViewType::Screen);
                        }

                        if (hide_blur) {
                            std::vector<REManagedObject*> blur{};
                            pragmata_blur::get(name_hash, name, fview, context, m_frame_count, blur);
                            for (auto b : blur) {
                                if (sdk::call_object_func<bool>(b, "get_Visible", context, b)) {
                                    sdk::call_object_func<void*>(b, "set_Visible", context, b, false);
                                    PragmataChildRestore br{};
                                    br.obj = b;
                                    br.restore_visible = true;
                                    restore.children.push_back(br);
                                }
                            }
                        }

                        auto c = sdk::call_object_func<REManagedObject*>(fview, "get_Child", context, fview);
                        for (int n = 0; c != nullptr && n < 64; ++n) {
                            PragmataChildRestore cr{};
                            cr.obj = c;
                            cr.position = sdk::call_object_func_easy<Vector4f>(c, "get_Position");
                            cr.scale = sdk::call_object_func_easy<Vector4f>(c, "get_Scale");
                            cr.restore_position = true;
                            Vector3f p{ pv.x + kk * (cr.position.x - pv.x) + dx, pv.y + kk * (cr.position.y - pv.y) + dy, cr.position.z };
                            Vector4f sc{ cr.scale.x * kk, cr.scale.y * kk, cr.scale.z, 1.0f };
                            sdk::call_object_func<void*>(c, "set_Position", context, c, &p);
                            sdk::call_object_func<void*>(c, "set_Scale", context, c, &sc);
                            restore.children.push_back(cr);
                            c = sdk::call_object_func<REManagedObject*>(c, "get_Next", context, c);
                        }

                        g_pragmata_panel_restore.push_back(std::move(restore));
                    }

                    // Build 47: shared by all render threads (one map per thread logged the same line many times a second).
                    static std::mutex logged_flat_mtx{};
                    static std::unordered_map<size_t, int> logged_flat{};
                    const int fgen = pragmatavr::HudTuning::get().companion_generation.load();
                    bool log_flat = false;
                    {
                        std::scoped_lock _{logged_flat_mtx};
                        auto lit = logged_flat.find(name_hash);
                        if (lit == logged_flat.end() || lit->second != fgen) {
                            logged_flat[name_hash] = fgen;
                            log_flat = true;
                        }
                    }
                    if (log_flat) {
                        const auto vt = fview != nullptr ? sdk::call_object_func<uint32_t>(fview, "get_ViewType", context, fview) : 99u;
                        // Build 24: does this GUI draw into its own render target (an image shown elsewhere)?
                        void* rt = nullptr;
                        bool has_rt_method = false;
                        if (auto gtd = utility::re_managed_object::get_type_definition((REManagedObject*)gui_element); gtd != nullptr &&
                            gtd->get_method("get_RenderTarget") != nullptr) {
                            has_rt_method = true;
                            rt = sdk::call_object_func<void*>(gui_element, "get_RenderTarget", context, gui_element);
                        }
                        spdlog::info("[PragmataVR] FLAT HUD: {} as {} ({}), view type {} (was {}), drawn {}, render target {}, size {:.2f} x {:.0f} y {:.0f} gap {:.0f}",
                            name, pragmatavr::GROUP_KEYS[fgroup], fnative ? "3D panel" : "companion", vt, fvt,
                            screen_drawn ? "as Screen view" : "as World view", has_rt_method ? (rt != nullptr ? fmt::format("{:x}", (uintptr_t)rt) : std::string{"none"}) : std::string{"(no method)"},
                            k, gt.x, gt.y, gt.gap);
                    }

                    // Build 58: the subtitles are a Screen view of their own (not a 3D panel switched to Screen): keep them
                    // flat in the HUD image as well, off REFramework's sheet, so they are cut out onto their quad.
                    if (!fnative || to_screen || fgroup == pragmatavr::GroupSubtitles) {
                        return true; // drawn as a Screen view: skip REFramework's floating sheet for it
                    }
                    // 3D panels go on as before (World mode, drawn with the flat projection; the 3D tuning is skipped)
                }
            }
        }

        // PragmataVR Build 15: Screen-mode companion layers of the HUD elements (see pragmatavr::companion_group).
        {
            std::string_view partner_name{};
            const int cgroup = pragmatavr::companion_group(name, partner_name);
            const auto tune = pragmatavr::HudTuning::get().tune();

            if (cgroup >= 0 && tune->enabled) {
                if (tune->companion_mode == pragmatavr::CompanionHidden) {
                    return false; // test: do not draw the companion at all
                }

                auto cview = sdk::call_object_func<REComponent*>(gui_element, "get_View", context, gui_element);
                std::optional<pragmata_companion::Partner> partner{};
                {
                    std::scoped_lock _{pragmata_companion::g_mtx};
                    auto it = pragmata_companion::g_partners.find(std::string{partner_name});
                    if (it != pragmata_companion::g_partners.end() &&
                        std::chrono::steady_clock::now() - it->second.seen < std::chrono::milliseconds(500)) {
                        partner = it->second;
                    }
                }

                const int gen = pragmatavr::HudTuning::get().companion_generation.load();
                bool log_now = false;
                {
                    std::scoped_lock _{pragmata_companion::g_mtx};
                    auto it = pragmata_companion::g_logged.find(name_hash);
                    if (it == pragmata_companion::g_logged.end() || it->second != gen) {
                        pragmata_companion::g_logged[name_hash] = gen;
                        log_now = true;
                    }
                }
                if (log_now && cview != nullptr) {
                    pragmata_companion::log_view(partner ? "Companion (as found)" : "Companion (partner not drawn yet)", name, cview, game_object->transform, context);
                }

                if (tune->companion_mode == pragmatavr::CompanionWithElement && cview != nullptr && partner && game_object->transform != nullptr) {
                    const auto& gt = tune->groups[cgroup];
                    const float eye_shift = is_left_eye() ? gt.gap : -gt.gap;

                    PragmataPanelRestore restore{};
                    restore.view = cview;
                    restore.companion = true;
                    restore.position = sdk::call_object_func_easy<Vector4f>(cview, "get_Position");
                    restore.view_scale = sdk::call_object_func_easy<Vector4f>(cview, "get_Scale");
                    restore.view_type = sdk::call_object_func<uint32_t>(cview, "get_ViewType", context, cview);
                    restore.overlay = sdk::call_object_func<bool>(cview, "get_Overlay", context, cview);
                    restore.detonemap = sdk::call_object_func<bool>(cview, "get_Detonemap", context, cview);
                    restore.transform = game_object->transform;
                    restore.world = game_object->transform->worldTransform;

                    // Same view settings and placement as the partner, plus the element's position values.
                    sdk::call_object_func<void*>(cview, "set_ViewType", context, cview, (uint32_t)via::gui::ViewType::World);
                    sdk::call_object_func<void*>(cview, "set_Overlay", context, cview, partner->overlay);
                    sdk::call_object_func<void*>(cview, "set_Detonemap", context, cview, partner->detonemap);
                    Vector3f np{ partner->position.x + gt.x + eye_shift, partner->position.y + gt.y * tune->y_sign, partner->position.z };
                    sdk::call_object_func<void*>(cview, "set_Position", context, cview, &np);
                    Vector4f ns = partner->scale;
                    sdk::call_object_func<void*>(cview, "set_Scale", context, cview, &ns);
                    game_object->transform->worldTransform = partner->world;

                    // Grow around the partner's element center, so both halves stay together.
                    if (gt.size != 1.0f && tune->scale_about_center && partner->pivot) {
                        const float k = gt.size;
                        const auto pv = *partner->pivot;
                        auto c = sdk::call_object_func<REManagedObject*>(cview, "get_Child", context, cview);
                        for (int n = 0; c != nullptr && n < 64; ++n) {
                            PragmataChildRestore cr{};
                            cr.obj = c;
                            cr.position = sdk::call_object_func_easy<Vector4f>(c, "get_Position");
                            cr.scale = sdk::call_object_func_easy<Vector4f>(c, "get_Scale");
                            cr.restore_position = true;
                            Vector3f p{ pv.x + k * (cr.position.x - pv.x), pv.y + k * (cr.position.y - pv.y), cr.position.z };
                            Vector4f sc{ cr.scale.x * k, cr.scale.y * k, cr.scale.z, 1.0f };
                            sdk::call_object_func<void*>(c, "set_Position", context, c, &p);
                            sdk::call_object_func<void*>(c, "set_Scale", context, c, &sc);
                            restore.children.push_back(cr);
                            c = sdk::call_object_func<REManagedObject*>(c, "get_Next", context, c);
                        }
                    }

                    g_pragmata_panel_restore.push_back(std::move(restore));

                    if (log_now) {
                        pragmata_companion::log_view("Companion (drawn with element)", name, cview, game_object->transform, context);
                    }
                    return true; // skip REFramework's generic Screen-to-World sheet for this object
                }
            }
        }

        // PragmataVR: per-element size, position and per-eye gap for Pragmata's World-mode 3D HUD panels.
        // Applied to the panel's GUI view just before it draws, restored in on_gui_draw_element.
        {
            const int group = pragmatavr::group_for(name);

            if (group >= 0) {
                const auto tune = pragmatavr::HudTuning::get().tune();
                auto pview = sdk::call_object_func<REComponent*>(gui_element, "get_View", context, gui_element);

                if (tune->enabled && !tune->flat_test && pview != nullptr &&
                    sdk::call_object_func<uint32_t>(pview, "get_ViewType", context, pview) == (uint32_t)via::gui::ViewType::World)
                {
                    const auto& gt = tune->groups[group];
                    const float eye_shift = is_left_eye() ? gt.gap : -gt.gap;
                    const float dx = gt.x + eye_shift;
                    const float dy = gt.y * tune->y_sign;

                    PragmataPanelRestore restore{};
                    restore.view = pview;
                    restore.position = sdk::call_object_func_easy<Vector4f>(pview, "get_Position");

                    // Build 15: remember this panel's own settings for its companion, and log them once per mode.
                    {
                        const int gen = pragmatavr::HudTuning::get().companion_generation.load();
                        bool log_now = false;
                        {
                            std::scoped_lock _{pragmata_companion::g_mtx};
                            auto& pt = pragmata_companion::g_partners[name];
                            pt.position = restore.position;
                            pt.scale = sdk::call_object_func_easy<Vector4f>(pview, "get_Scale");
                            pt.rotation = sdk::call_object_func_easy<Vector4f>(pview, "get_Rotation");
                            pt.overlay = sdk::call_object_func<bool>(pview, "get_Overlay", context, pview);
                            pt.detonemap = sdk::call_object_func<bool>(pview, "get_Detonemap", context, pview);
                            pt.world = game_object->transform != nullptr ? game_object->transform->worldTransform : Matrix4x4f{1.0f};
                            pt.seen = std::chrono::steady_clock::now();
                            auto it = pragmata_companion::g_logged.find(name_hash);
                            if (it == pragmata_companion::g_logged.end() || it->second != gen) {
                                pragmata_companion::g_logged[name_hash] = gen;
                                log_now = true;
                            }
                        }
                        if (log_now) {
                            pragmata_companion::log_view("3D panel", name, pview, game_object->transform, context);
                        }
                    }

                    // Element center, found before anything is changed this draw (values are restored after each draw).
                    const bool centered = group != pragmatavr::GroupHack && tune->scale_about_center;
                    std::optional<Vector2f> pivot{};
                    if (centered && gt.size != 1.0f) {
                        pivot = pragmata_pivot::get(name_hash, name, pview, context, m_frame_count);
                        std::scoped_lock _{pragmata_companion::g_mtx};
                        pragmata_companion::g_partners[name].pivot = pivot;
                    }

                    Vector3f new_position{ restore.position.x + dx, restore.position.y + dy, restore.position.z };
                    sdk::call_object_func<void*>(pview, "set_Position", context, pview, &new_position);

                    if (gt.size != 1.0f) {
                        if (pivot) {
                            // Grow every top-level piece of the panel around the element's center:
                            // position -> center + size * (position - center), scale -> size * scale.
                            const float k = gt.size;
                            auto c = sdk::call_object_func<REManagedObject*>(pview, "get_Child", context, pview);
                            for (int n = 0; c != nullptr && n < 64; ++n) {
                                PragmataChildRestore cr{};
                                cr.obj = c;
                                cr.position = sdk::call_object_func_easy<Vector4f>(c, "get_Position");
                                cr.scale = sdk::call_object_func_easy<Vector4f>(c, "get_Scale");
                                cr.restore_position = true;

                                Vector3f p{ pivot->x + k * (cr.position.x - pivot->x), pivot->y + k * (cr.position.y - pivot->y), cr.position.z };
                                Vector4f sc{ cr.scale.x * k, cr.scale.y * k, cr.scale.z, 1.0f };
                                sdk::call_object_func<void*>(c, "set_Position", context, c, &p);
                                sdk::call_object_func<void*>(c, "set_Scale", context, c, &sc);
                                restore.children.push_back(cr);

                                c = sdk::call_object_func<REManagedObject*>(c, "get_Next", context, c);
                            }
                        } else {
                            // Builds 3-6 behaviour (the hacking circle was tuned with this): scale the first child.
                            auto pchild = sdk::call_object_func<REManagedObject*>(pview, "get_Child", context, pview);

                            if (pchild != nullptr) {
                                PragmataChildRestore cr{};
                                cr.obj = pchild;
                                cr.scale = sdk::call_object_func_easy<Vector4f>(pchild, "get_Scale");

                                Vector4f new_scale{ cr.scale.x * gt.size, cr.scale.y * gt.size, cr.scale.z, 1.0f };
                                sdk::call_object_func<void*>(pchild, "set_Scale", context, pchild, &new_scale);
                                restore.children.push_back(cr);
                            }
                        }
                    }

                    g_pragmata_panel_restore.push_back(std::move(restore));

                    static thread_local std::unordered_map<size_t, bool> logged_panels{};
                    if (logged_panels.emplace(name_hash, true).second) {
                        spdlog::info("[PragmataVR] Tuning 3D HUD panel {} as {}", name, pragmatavr::GROUP_KEYS[group]);
                    }
                }
            }
        }
#endif

        switch (name_hash) {
        // Don't mess with this, causes weird black boxes on the sides of the screen
        case "GUI_PillarBox"_fnv:
        case "GUIEventPillar"_fnv:
        // These allow the cutscene transitions to display (fade to black)
        case "BlackFade"_fnv:
        case "WhiteFade"_fnv:
        case "Fade_In_Out_Black"_fnv:
        case "Fade_In_Out_White"_fnv:
        case "FadeInOutBlack"_fnv:
        case "FadeInOutWhite"_fnv:
        case "GUIBlackMask"_fnv:
        case "GenomeCodexGUI"_fnv:
        case "sm42_020_keystrokeDevice01A_gimmick"_fnv: // this one is the keypad in the locker room...
            return true;

#if defined(RE2) || defined(RE3)
        // the weird buggy overlay in the inventory
        case "GuiBack"_fnv:
            return false;
#endif

#if defined(RE4)
        case "Gui_ui2510"_fnv: // Black bars in cutscenes
            return false;
#endif

#if defined(RE9)
        case "Gui_ui0440"_fnv: // Black bars in cutscenes
            game_object->shouldDraw = false;
            return false;
        case "GuiTimelineEvent_ui1640_00"_fnv: // monitor in first cutscene
        case "GuiPlayerGimmick0040"_fnv:  //blood analyzer
            return true;
#endif

#if TDB_VER >= 73
        case "ui0199"_fnv: // weird black bars in Kunitsu-Gami
            return false;
#endif

        default:
            break;
        };

        // Certain UI elements we want to remove when in VR (FirstPerson enabled)
#if defined(RE2) || defined(RE3)
        if (std::chrono::steady_clock::now() - m_last_crosshair_hide > std::chrono::seconds(1)) {
            auto& fp = FirstPerson::get();

            if (fp->is_enabled() && fp->will_be_used()) {
                const auto has_motion_controls = this->is_using_controllers();

                switch(name_hash) {
                case "GUI_Reticle"_fnv: // Crosshair
                    if (has_motion_controls) {
                        return false;
                    }
                    
                    break;
                default:
                    break;
                }
            }
        }
#endif

#ifdef RE7
        if (name_hash == "HUD"_fnv) { // not a hero
            // Stops HUD element from being stuck to the screen
            sdk::call_object_func<REComponent*>(gui_element, "set_RenderTarget", context, gui_element, nullptr);
        }
#endif

        //if (m_ui_last_frame_counts[name_hash] == 0 || m_ui_last_frame_counts[name_hash] - m_frame_count > 1) {
        //    m_ui_last_frame_counts[name_hash] = m_frame_count;
        //    return true;
        //}
        //m_ui_last_frame_counts[name_hash] = m_frame_count;

        //spdlog::info("VR: on_pre_gui_draw_element: {}", name);
        //spdlog::info("VR: on_pre_gui_draw_element: {} {:x}", name, (uintptr_t)game_object);

        auto view = sdk::call_object_func<REComponent*>(gui_element, "get_View", context, gui_element);

        if (view != nullptr) {
            const auto current_view_type = sdk::call_object_func<uint32_t>(view, "get_ViewType", context, view);

            if (current_view_type == (uint32_t)via::gui::ViewType::Screen) {
                static sdk::RETypeDefinition* via_render_mesh_typedef = nullptr;

                if (via_render_mesh_typedef == nullptr) {
                    via_render_mesh_typedef = sdk::find_type_definition("via.render.Mesh");

                    // wait
                    if (via_render_mesh_typedef == nullptr) {
                        return true;
                    }
                }

                // we don't want to mess with any game object that has a mesh
                // because it might be something physical in the game world
                // that the player can interact with
                if (utility::re_component::find(game_object->transform, via_render_mesh_typedef->get_type()) != nullptr) {
                    return true;
                }

#ifdef RE7
                static auto ui_world_pos_attach_typedef = sdk::find_type_definition("app.UIWorldPosAttach");
#endif

                auto ui_scale = m_ui_scale_option->value();
                const auto world_ui_scale = m_world_ui_scale_option->value();

                auto& restore_data = g_elements_to_reset.emplace_back(std::make_unique<GUIRestoreData>());
                auto original_game_object_pos = sdk::get_transform_position(game_object->transform);

                restore_data->element = gui_element;
                restore_data->view = view;
                restore_data->original_position = original_game_object_pos;
                restore_data->overlay = sdk::call_object_func<bool>(view, "get_Overlay", context, view);
                restore_data->detonemap = sdk::call_object_func<bool>(view, "get_Detonemap", context, view);
                restore_data->view_type = (via::gui::ViewType)current_view_type;

                original_game_object_pos.w = 0.0f;

                // Set view type to world
                sdk::call_object_func<void*>(view, "set_ViewType", context, view, (uint32_t)via::gui::ViewType::World);

                // Set overlay = true (fixes double vision in VR)
                sdk::call_object_func<void*>(view, "set_Overlay", context, view, true);

                // Set detonemap = true (fixes weird tint)
                sdk::call_object_func<void*>(view, "set_Detonemap", context, view, true);

                // Go through the children until we hit a blur filter
                // And then remove it
                /*for (auto child = sdk::call_object_func<REComponent*>(view, "get_Child", sdk::get_thread_context(), view); child != nullptr; child = sdk::call_object_func<REComponent*>(child, "get_Child", sdk::get_thread_context(), child)) {
                    if (utility::re_managed_object::is_a(child, "via.gui.BlurFilter")) {
                        // Call remove()
                        sdk::call_object_func<void*>(child, "remove", sdk::get_thread_context(), child);
                        break;
                    }
                }*/

                auto camera = sdk::get_primary_camera();

                // Set the gui element's position to be in front of the camera
                if (camera != nullptr) {
                    auto camera_object = utility::re_component::get_game_object(camera);

                    if (camera_object != nullptr && camera_object->transform != nullptr) {
                        auto& gui_matrix = game_object->transform->worldTransform;
                        auto child = sdk::call_object_func<REManagedObject*>(view, "get_Child", context, view);

                        auto fix_2d_position = [&](const Vector4f& target_position, 
                                                    bool screen_correction = true,
                                                    std::optional<float> custom_ui_scale = std::nullopt)
                        {
                            if (!custom_ui_scale) {
                                custom_ui_scale = world_ui_scale;
                            }

                            auto delta = target_position - m_render_camera_matrix[3];
                            delta.w = 0.0f;

                            auto dir = glm::normalize(delta);
                            dir.w = 0.0f;
                            
                            // make matrix from dir
                            const auto look_mat = glm::rowMajor4(glm::lookAtLH(Vector3f{}, Vector3f{ dir }, Vector3f(0.0f, 1.0f, 0.0f)));
                            const auto look_rot = glm::quat{look_mat};

                            auto new_pos = target_position;
                            new_pos.w = 1.0f;

                            gui_matrix = look_mat;
                            gui_matrix[3] = new_pos;

                            // LESSON: DO NOT CALL THESE METHODS ON THE TRANSFORM!
                            // THEY CAUSE SOME STRANGE BUGS WHEN THE GUI ELEMENT HAS A PARENT TRANSFORM!
                            // THE GUI RENDERING FUNCTIONS PERFORM ON THE WORLD MATRIX, SO THIS IS NOT NECESSARY.
                            //sdk::set_transform_position(game_object->transform, new_pos);
                            //sdk::set_transform_rotation(game_object->transform, look_rot);
                            
                            const auto scaled_ui_scale = *custom_ui_scale * 0.01f;
                            const auto distance = glm::length(delta);
                            const auto scale = std::clamp<float>(distance * scaled_ui_scale, 0.1f, 100.0f);

                            regenny::via::Size gui_size{};
                            sdk::call_object_func<void*>(view, "get_ScreenSize", &gui_size, context, view);
                            
                            auto fix_transform_object = [&](::REManagedObject* object) {
                                static auto transform_object_type = sdk::find_type_definition("via.gui.TransformObject");

                                if (object == nullptr) {
                                    return;
                                }

                                const auto t = utility::re_managed_object::get_type_definition(object);

                                if (t == nullptr || !t->is_a(transform_object_type)) {
                                    return;
                                }

                                //sdk::call_object_func<void*>(object, "set_ResolutionAdjust", context, object, true);

                                if (screen_correction) {
                                    Vector3f half_size{ gui_size.w / 2.0f, gui_size.h / 2.0f, 0.0f };
                                    sdk::call_object_func<void*>(object, "set_Position", context, object, &half_size);
                                }

                                //Vector4f new_scale{ scale, scale, scale, 1.0f };
                                const auto old_scale = sdk::call_object_func_easy<Vector4f>(object, "get_Scale");
                                Vector4f new_scale{ old_scale.y, old_scale.y, old_scale.z, 1.0f };
                                sdk::call_object_func<void*>(object, "set_Scale", context, object, &new_scale);
                            };

                            for (auto c = child; c != nullptr; c = sdk::call_object_func<REManagedObject*>(c, "get_Next", context, c)) {
                                fix_transform_object(c);
                            }

                            // Fix for other kinds of world pos attach elements.
#ifdef RE7
                            if (name_hash == "InteractOperationCursor"_fnv) {
                                auto world_pos_attach_comp = utility::re_component::find(game_object->transform, ui_world_pos_attach_typedef->get_type());

                                // Fix the world position of the gui element
                                if (world_pos_attach_comp != nullptr) {
                                    auto target_cache = sdk::get_object_field<::REManagedObject*>(world_pos_attach_comp, "_TargetGUIElem");

                                    if (target_cache != nullptr && *target_cache != nullptr) {
                                        auto element = sdk::get_object_field<::REManagedObject*>(*target_cache, "_Element");

                                        if (element != nullptr && *element != nullptr) {
                                            Vector3f zero_size{ 0.0f, 0.0f, 0.0f };
                                            sdk::call_object_func<void*>(*element, "set_Position", context, *element, &zero_size);
                                        }
                                    }
                                }
                            }
#endif

                            gui_matrix = glm::scale(gui_matrix, Vector3f{ scale, scale, scale });
                        };

                        auto camera_transform = camera_object->transform;

                        const auto& camera_matrix = m_original_camera_matrix;
                        auto camera_position = camera_matrix[3];

                        glm::quat wanted_rotation{};
                        
                        bool wants_face_glue = false;
                        auto ui_distance = m_ui_distance_option->value();

                        switch (name_hash) {
                        case "damage_ui2102"_fnv:
                        case "NightVision_Filter"_fnv:
                            wants_face_glue = true;
                            break;
                        default:
                            break;
                        };

                        float right_world_adjust{};

                        switch (name_hash) {
                        case "GuiFront"_fnv:
                        case "GUI_Pause"_fnv:
                        case "GUIPause"_fnv:
                        case "GUIMapBg"_fnv:
                        case "BG"_fnv:
                            ui_distance += 0.1f; // give everything a little pop in the inventory.
                            break;
                        case "GUI_MapBG"_fnv:
                        case "GUI_Map"_fnv:
                        case "GUI_MapGrid"_fnv:
                        case "GUIMap"_fnv:
                        case "GUIMapIcon"_fnv:
                            ui_distance -= 0.025f; // give everything a little pop in the inventory.
                            break;
                        case "GuiCaption"_fnv:
                        case "GUIInventory"_fnv:
                        case "GUIInventoryCraft"_fnv:
                        case "GUIInventoryKeyItem"_fnv:
                        case "GUIInventoryTreasure"_fnv:
                        case "GUIBinder"_fnv:
                        case "GUIEquip"_fnv:
                            ui_distance -= 0.1f; // give everything a little pop in the inventory.
                            break;
                        case "GUI_RemainingBullet"_fnv:
                            right_world_adjust = 0.15f;
                            break;
                        case "GUI_Reticle"_fnv:
                        case "ReticleGUI"_fnv:
                        case "GUIReticle"_fnv:
                            ui_distance = 5.0f;
                            ui_scale = 2.0f;
                            break;
                        case "hud_hunterwirewindow"_fnv:
                            ui_distance = 10.0f;
                            ui_scale = 1.0f;
                            break;
                        }

                        if (ui_distance < 0.0f) {
                            ui_distance = 0.0f;
                        }

                        // glues the GUI to the camera rotation and position
                        if (wants_face_glue) {
                            ui_distance = 5.0f;

                            wanted_rotation = glm::extractMatrixRotation(m_render_camera_matrix) * Matrix4x4f{
                                -1, 0, 0, 0,
                                0, 1, 0, 0,
                                0, 0, -1, 0,
                                0, 0, 0, 1
                            };
                        } else {
                            const auto gui_rotation_offset = get_gui_rotation_offset();

                            wanted_rotation = glm::extractMatrixRotation(camera_matrix) * Matrix4x4f{
                                -1, 0, 0, 0,
                                0, 1, 0, 0,
                                0, 0, -1, 0,
                                0, 0, 0, 1
                            };

                            if (m_decoupled_pitch->value()) {
                                bool is_exception = false;

                                switch (name_hash) {
                                    case "GUIReticle"_fnv:[[fallthrough]];
                                    case "ReticleGUI"_fnv:[[fallthrough]];
                                    case "GUI_Reticle"_fnv:
                                        is_exception = true;
                                        break;
                                    default:
                                        break;
                                }

                                if (!is_exception) {
                                    wanted_rotation = utility::math::flatten(wanted_rotation);
                                }
                            }

                            wanted_rotation = gui_rotation_offset * wanted_rotation;
                        }

                        const auto wanted_rotation_mat = Matrix4x4f{wanted_rotation};

#if defined(RE9)
                        // fix double vision on button prompts when using AFW/AFR
                        if ((is_using_afw() || is_using_afw_foveated()) && name_hash == "Gui_ui2010"_fnv) {
                            const auto eye_camera_matrix = m_original_camera_matrix * get_current_eye_transform(true);
                            camera_position = eye_camera_matrix[3];
                        }
 #endif

                        gui_matrix = wanted_rotation_mat;
                        gui_matrix[3] = camera_position + (wanted_rotation_mat[2] * ui_distance) + (wanted_rotation_mat[0] * right_world_adjust);
                        gui_matrix[3].w = 1.0f;

                        // Scales the GUI so it's not massive.
                        if (!wants_face_glue) {
                            const auto scale = 1.0f / ui_scale;
                            gui_matrix = glm::scale(gui_matrix, Vector3f{ scale, scale, scale });
                        }

                        static auto gui_driver_typedef = sdk::find_type_definition(game_namespace("GUIDriver"));
                        static auto mhrise_npc_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonNpcHeadMessage"));
                        static auto mhrise_speech_balloon_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonNpcSpeechBalloon"));
                        static auto mhrise_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonHeadMessage"));
                        static auto mhrise_otomo_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonOtomoHeadMessage"));
                        static auto kg_float_icon_behavior = sdk::find_type_definition("app.FloatIconBehavior");
                        static auto kg_general_vital_gauge_behavior = sdk::find_type_definition("app.GeneralVitalGaugeBehavior");

                        static auto gameobject_elements_list = {
                            mhrise_npc_head_message_typedef,
                            mhrise_speech_balloon_typedef,
                            mhrise_head_message_typedef,
                            mhrise_otomo_head_message_typedef,
                            kg_float_icon_behavior,
                            kg_general_vital_gauge_behavior
                        };
                        
                        // Fix position of interaction icons
                        if (name_hash == "GUI_FloatIcon"_fnv || name_hash == "RogueFloatIcon"_fnv || name_hash == "Gui_FloatIcon"_fnv) { // RE2, RE3, RE4
                            if (name_hash == "GUI_FloatIcon"_fnv || name_hash == "Gui_FloatIcon"_fnv) {
                                m_last_interaction_display = std::chrono::steady_clock::now();
                            }
                        
                            fix_2d_position(original_game_object_pos);
                        } else if(gui_driver_typedef != nullptr) { // RE8
                            auto interact_icon_comp = utility::re_component::find(game_object->transform, gui_driver_typedef->get_type());

                            if (interact_icon_comp != nullptr) {
                                auto interact_icon_object = sdk::call_object_func<REGameObject*>(interact_icon_comp, "get_attachTarget", context, interact_icon_comp);

                                if (interact_icon_object != nullptr && interact_icon_object->transform != nullptr) {
                                    // call get_Position on the object
                                    Vector4f interact_icon_position{};
                                    sdk::call_object_func<Vector4f*>(interact_icon_object->transform, "get_Position", &interact_icon_position, context, interact_icon_object->transform);

                                    fix_2d_position(interact_icon_position);
                                }
                            }
                        } else {
                            // MHRise
                            for (auto element_type : gameobject_elements_list) {
                                if (element_type == nullptr) {
                                    continue;
                                }

                                auto element_comp = utility::re_component::find(game_object->transform, element_type->get_type());

                                if (element_comp == nullptr) {
                                    continue;
                                }

                                static auto get_parent_method = sdk::get_object_method(game_object->transform, "get_Parent");
                                auto parent = get_parent_method->call<::RETransform*>(context, game_object->transform);

                                if (parent != nullptr) {
                                    Vector4f offset{};

                                    std::optional<float> custom_ui_scale = std::nullopt;

                                    if (element_type == mhrise_speech_balloon_typedef) {
                                        static auto pos_data_field = mhrise_speech_balloon_typedef->get_field("posData");
                                        static auto npc_message_pos = pos_data_field->get_type()->get_field("NpcMessagePos");

                                        auto pos_data = pos_data_field->get_data<::REManagedObject*>(element_comp);

                                        if (pos_data != nullptr) {
                                            const auto y_offset = npc_message_pos->get_data<float>(pos_data);
                                            offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                        } else {
                                            offset = Vector4f{0.0f, 1.0f, 0.0f, 0.0f};
                                        }
                                    } else if (element_type == mhrise_npc_head_message_typedef) {
                                        static auto pos_data_field = mhrise_npc_head_message_typedef->get_field("posData");
                                        static auto npc_message_pos = pos_data_field->get_type()->get_field("NpcMessagePos");
                                        
                                        auto pos_data = pos_data_field->get_data<::REManagedObject*>(element_comp);

                                        if (pos_data != nullptr) {
                                            const auto y_offset = npc_message_pos->get_data<float>(pos_data);
                                            offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                        } else {
                                            offset = Vector4f{0.0f, 1.0f, 0.0f, 0.0f};
                                        }
                                    } else if (element_type == mhrise_head_message_typedef) {
                                        static auto message_pos_y_field = mhrise_head_message_typedef->get_field("MessagePosY");

                                        const auto y_offset = message_pos_y_field->get_data<float>(element_comp);
                                        offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                    } else if (element_type == mhrise_otomo_head_message_typedef) { //airou and dog
                                        offset = Vector4f{0.0, 1.0f, 0.0f, 0.0f};
                                    } else if (element_type == kg_float_icon_behavior) {
                                        static auto param_field = kg_float_icon_behavior->get_field("_Param");
                                        static auto world_pos_field = param_field != nullptr ? param_field->get_type()->get_field("WorldPos") : nullptr;

                                        if (world_pos_field != nullptr) {
                                            auto param = param_field->get_data<::REManagedObject*>(element_comp);

                                            if (param != nullptr) {
                                                offset = world_pos_field->get_data<Vector4f>(param);
                                                custom_ui_scale = 5.0f;
                                            }
                                        }
                                    } else if (element_type == kg_general_vital_gauge_behavior) {
                                        static auto param_field = kg_general_vital_gauge_behavior->get_field("_OpenParam");
                                        static auto world_pos_field = param_field != nullptr ? param_field->get_type()->get_field("WorldPos") : nullptr;

                                        if (world_pos_field != nullptr) {
                                            auto param = param_field->get_data<::REManagedObject*>(element_comp);

                                            if (param != nullptr) {
                                                offset = world_pos_field->get_data<Vector4f>(param);
                                                custom_ui_scale = 5.0f;
                                            }
                                        }
                                    }

                                    fix_2d_position(sdk::get_transform_position(parent) + offset, true, custom_ui_scale);
                                } else {
                                    fix_2d_position(original_game_object_pos);
                                }
                            }
                        }

                        // ... RE7
#ifdef RE7
                        auto world_pos_attach_comp = utility::re_component::find(game_object->transform, ui_world_pos_attach_typedef->get_type());

                        // Fix the world position of the gui element
                        if (world_pos_attach_comp != nullptr) {
                            const auto& target_pos = *sdk::get_object_field<Vector4f>(world_pos_attach_comp, "_NowTargetPos");
                            
                            fix_2d_position(target_pos);
                        }
#endif
                    }
                }
            }
        }
    } else {
        spdlog::info("VR: on_pre_gui_draw_element: nullptr gameobject");
    }

    return true;
}

void VR::on_gui_draw_element(REComponent* gui_element, void* primitive_context) {
#if defined(PRAGMATA)
    pragmatavr::g_current_gui_hash = 0;
    pragmatavr::g_current_gui_name.clear();

    if (!g_pragmata_panel_restore.empty()) {
        auto pcontext = sdk::get_thread_context();

        for (auto& r : g_pragmata_panel_restore) {
            if (r.companion) {
                sdk::call_object_func<void*>(r.view, "set_ViewType", pcontext, r.view, r.view_type);
                sdk::call_object_func<void*>(r.view, "set_Overlay", pcontext, r.view, r.overlay);
                sdk::call_object_func<void*>(r.view, "set_Detonemap", pcontext, r.view, r.detonemap);
                sdk::call_object_func<void*>(r.view, "set_Scale", pcontext, r.view, &r.view_scale);
                if (r.transform != nullptr) {
                    r.transform->worldTransform = r.world;
                }
            }

            Vector3f original_position{ r.position.x, r.position.y, r.position.z };
            sdk::call_object_func<void*>(r.view, "set_Position", pcontext, r.view, &original_position);

            for (auto& cr : r.children) {
                if (cr.restore_visible) {
                    sdk::call_object_func<void*>(cr.obj, "set_Visible", pcontext, cr.obj, true);
                    continue;
                }
                if (cr.restore_mask) {
                    sdk::call_object_func<void*>(cr.obj, "set_MaskMode", pcontext, cr.obj, cr.mask_mode);
                    continue;
                }
                sdk::call_object_func<void*>(cr.obj, "set_Scale", pcontext, cr.obj, &cr.scale);
                if (cr.restore_position) {
                    Vector3f p{ cr.position.x, cr.position.y, cr.position.z };
                    sdk::call_object_func<void*>(cr.obj, "set_Position", pcontext, cr.obj, &p);
                }
            }
        }

        g_pragmata_panel_restore.clear();
    }
#endif
    

    //spdlog::info("VR: on_gui_draw_element");

    auto context = sdk::get_thread_context();

    // Restore elements back to original states
    for (auto& data : g_elements_to_reset) {
        sdk::call_object_func<void*>(data->view, "set_ViewType", context, data->view, (uint32_t)via::gui::ViewType::Screen);
        sdk::call_object_func<void*>(data->view, "set_Overlay", context, data->view, data->overlay);
        sdk::call_object_func<void*>(data->view, "set_Detonemap", context, data->view, data->detonemap);
        
        auto game_object = utility::re_component::get_game_object(data->element);

        if (game_object != nullptr && game_object->transform != nullptr) {
            //sdk::set_transform_position(game_object->transform, data->original_position);

            auto& gui_matrix = game_object->transform->worldTransform;
            gui_matrix[3] = data->original_position;
        }
    }

    g_elements_to_reset.clear();
    inside_gui_draw = false;
}

void VR::on_pre_update_before_lock_scene(void* ctx) {
    /*auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto projection_matrix = get_current_projection_matrix(true);

    // Steps towards getting lens flares and volumetric lighting working
    // Get the FOV from the projection matrix
    const auto vfov = glm::degrees(2.0f * std::atan(1.0f / projection_matrix[1][1]));
    const auto aspect = projection_matrix[1][1] / projection_matrix[0][0];
    const auto hfov = vfov * aspect;
    
    spdlog::info("vFOV: {}", vfov);
    spdlog::info("Aspect: {}", aspect);

    sdk::call_object_func<void*>(camera, "set_FOV", sdk::get_thread_context(), camera, vfov);
    sdk::call_object_func<void*>(camera, "set_VerticalEnable", sdk::get_thread_context(), camera, true);
    sdk::call_object_func<void*>(camera, "set_AspectRatio", sdk::get_thread_context(), camera, aspect);*/
}

void VR::on_pre_lightshaft_draw(void* shaft, void* render_context) {
    m_in_lightshaft = true;

    /*static auto transparent_layer_t = sdk::find_type_definition("via.render.layer.Transparent");
    auto transparent_layer = sdk::renderer::find_layer(transparent_layer_t->type);

    spdlog::info("transparent layer: {:x}", (uintptr_t)transparent_layer);

    if (transparent_layer == nullptr) {
        return;
    }

    static auto scene_layer_t = sdk::find_type_definition("via.render.layer.Scene");
    auto scene_layer = transparent_layer->find_parent(scene_layer_t->type);

    spdlog::info("scene layer: {:x}", (uintptr_t)scene_layer);

    if (scene_layer == nullptr) {
        return;
    }

    scene_layer->update();

    spdlog::info("scene layer update: {:x}", (uintptr_t)(*(void***)scene_layer)[sdk::renderer::RenderLayer::UPDATE_VTABLE_INDEX]);*/
}

void VR::on_lightshaft_draw(void* shaft, void* render_context) {
    m_in_lightshaft = false;
}

thread_local bool timed_out = false;

void VR::on_pre_begin_rendering(void* entry) {
    

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    m_in_render = true;

    // Use the gamepad/motion controller sticks to lerp the standing origin back to the center
    if (m_via_hid_gamepad.update()) {
        auto pad = sdk::call_native_func_easy<REManagedObject*>(m_via_hid_gamepad.object, m_via_hid_gamepad.t, "get_LastInputDevice");

        if (pad != nullptr) {
            // Move direction
            // It's not a Vector2f because via.vec2 is not actually 8 bytes, we don't want stack corruption to occur.
            const auto axis_l = (Vector2f)*utility::re_managed_object::get_field<Vector3f*>(pad, "AxisL");
            const auto axis_r = (Vector2f)*utility::re_managed_object::get_field<Vector3f*>(pad, "AxisR");

            // Lerp the standing origin back to HMD position
            // if the user is moving
            if (glm::length(axis_l) > 0.0f || glm::length(axis_r) > 0.0f) {
                const auto highest_length = std::max<float>(glm::length(axis_l), glm::length(axis_r));
                auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

                const auto delta = sdk::Application::get()->get_delta_time();

                new_pos.y = m_standing_origin.y;
                // Don't set the Y because it would look really strange
                m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
            }
        }

        // TODO: do the same thing for the keyboard
        // will probably require some game-specific code
    }

    detect_controllers();

    //actual_frame_count = get_game_frame_count();
    m_frame_count++;
    actual_frame_count = m_frame_count;

    /*if (!inside_on_end) {
        spdlog::info("VR: frame count: {}", m_frame_count);
    } else {
        spdlog::info("VR: frame count: {} (inside on_end)", m_frame_count);
    }*/

    // if we timed out, just return. we're assuming that the rendering will go on as normal
    if (timed_out) {
        if (inside_on_end) {
            spdlog::warn("VR: on_pre_wait_rendering: timed out inside_on_end");
        } else {
            spdlog::warn("VR: on_pre_wait_rendering: timed out");
        }

        return;
    }

    if (runtime->needs_pose_update && inside_on_end) {
        spdlog::info("VR: on_pre_wait_rendering: inside on end!");
    }
    
    // Call WaitGetPoses
    if (is_using_afw() || is_using_afw_foveated() || (!inside_on_end &&  is_left_eye())) {
        update_hmd_state();
    }

    const auto should_update_camera = is_left_eye() || is_using_afw() || is_using_afw_foveated();

    if (!inside_on_end && should_update_camera) {
        update_camera();
    } else if (inside_on_end) {
        update_camera_origin();
    }

    // update our internally stored render matrix
    update_render_matrix();
    //fix_temporal_effects(); // BAD way to do it!
}

void VR::on_begin_rendering(void* entry) {
    //spdlog::info("BeginRendering");
}

void VR::on_pre_end_rendering(void* entry) {
    

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    if (runtime->ready() && ((is_using_sf() && is_left_eye()) || is_using_afw() || is_using_afw_foveated())) {
        const auto stage = runtime->get_synchronize_stage();

        if (stage == VRRuntime::SynchronizeStage::LATE && runtime->synchronize_frame() == VRRuntime::Error::SUCCESS) {
            if (runtime->is_openxr()) {
                m_openxr->begin_frame();
            }
        }
    }

    auto root_layer = sdk::renderer::get_root_layer();
    if (root_layer != nullptr && m_frame_count > 600) {
        auto [output_parent, output_layer] = root_layer->find_layer_recursive("via.render.layer.Output");
        auto valid_scene_layers = (*output_layer)->find_fully_rendered_scene_layers();
        if (valid_scene_layers.empty()) {
            return;
        }
        if (valid_scene_layers.size() > 0) {
            depthTex[0] = valid_scene_layers[0]->get_depth_stencil_d3d12();
            //motionVectorsState = valid_scene_layers[0]->get_motion_vectors_state();
            if (valid_scene_layers.size() > 1) {
                depthTex[1] = valid_scene_layers[1]->get_depth_stencil_d3d12();
            }
        } 
    }
}

void VR::on_end_rendering(void* entry) {
    

    // we set this because we've enabled asynchronous rendering
    // by the time the next frame (right eye) starts,
    // the frame count might get modified, screwing up our logic
    // so we need a render frame count to compare against
    m_render_frame_count = m_frame_count;

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    // TODO: Check later if this is even necessary
    if ((!runtime->ready()) && !inside_on_end) {
        restore_camera();

        inside_on_end = false;
        m_in_render = false;
        return;
    }

    if (!inside_on_end) {
        auto app = sdk::Application::get();

        static auto app_type = sdk::find_type_definition("via.Application");
        static auto set_max_delta_time_fn = app_type->get_method("set_MaxDeltaTime");

        // RE8 and onwards...
        // defaults to 2, and will slow the game down if frame rate is too low
        if (set_max_delta_time_fn != nullptr) {
            // static func, no need for app
            set_max_delta_time_fn->call<void*>(sdk::get_thread_context(), 10.0f);
        }
    }

    if (is_using_afw() || is_using_afw_foveated()) {
        restore_camera();
        m_in_render = false;
        return;
    }

    if (is_using_sf() && inside_on_end) {
        return;
    }

    // Only render again on even (left eye) frames
    // We're checking == 1 because at this point, the frame has finished.
    // Meaning the previous frame was a left eye frame.
    if (!inside_on_end && ((is_using_sf() && (m_render_frame_count % 2 == m_left_eye_interval)))) {
        inside_on_end = true;
        
        // Try to render again for the right eye
        // Or for the foveated part when using foveated rendering
        auto app = sdk::Application::get();

        static auto chain = app->generate_chain("WaitRendering", "EndRendering");
        static bool do_once = true;

        if (do_once) {
            do_once = false;

            // Remove these from the chain (std::vector)
            auto entries_to_remove = std::vector<std::string> {
                "WaitRendering",
                "UpdatePhysicsCharacterController",
                "UpdateTelemetry",
                "UpdateMovie", // Causes movies to play twice as fast if ran again
                "UpdateSpeedTree",
                "UpdateHansoft",
                "UpdatePuppet",
                // The dynamics stuff causes a cloth physics step in the right eye
                "BeginRenderingDynamics",
                "BeginDynamics",
                "EndRenderingDynamics",
                "EndDynamics",
                "EndPhysics",
                "RenderDynamics",
#if TDB_VER < 73
                "RenderLandscape",
#endif
                "DevelopRenderer",
                "DrawWidget"
            };

            for (auto& entry : entries_to_remove) {
                chain.erase(std::remove_if(chain.begin(), chain.end(), [&](auto& func) {
                    return entry == func->description;
                }), chain.end());
            }
        }

#if defined(PRAGMATA)
        // Build 35: telemetry, time spent waiting for the render thread before the right-eye re-run.
        const auto pragmata_rerun_t0 = pragmatavr::perf::now_ns();
#endif
        sdk::renderer::wait_rendering();
#if defined(PRAGMATA)
        const auto pragmata_rerun_t1 = pragmatavr::perf::now_ns();
#endif
        sdk::renderer::begin_update_primitive();

        //static auto update_geometry = app->get_function("UpdateGeometry");
        static auto begin_update_effect = app->get_function("BeginUpdateEffect");
        static auto update_effect = app->get_function("UpdateEffect");
        static auto end_update_effect = app->get_function("EndUpdateEffect");
        static auto prerender_gui = app->get_function("PrerenderGUI");
        static auto begin_update_primitive = app->get_function("BeginUpdatePrimitive");

        // SO. Let me explain what's happening here.
        // If we try and just render a frame in this naive way in this order:
        // BeginUpdatePrimitive,
        // WaitRendering,
        // BeginRendering,
        // UpdatePrimitive,
        // EndPrimitive,
        // EndRendering,
        // This will end up having a chance to crash when rendering fluid effects for some reason when calling UpdatePrimitive.
        // The crash happens because some pipeline state inside the fluid simulator gets set to null.
        // So, we manually call BeginEffect, and then EndUpdateEffect
        // Which somehow solves the crash.
        // We don't call UpdateEffect because it will make effects appear to run at a higher framerate
        if (begin_update_effect != nullptr) {
            begin_update_effect->func(begin_update_effect->entry);
        }

        /*if (update_effect != nullptr) {
            update_effect->func(update_effect->entry);
        }*/

        if (end_update_effect != nullptr) {
            end_update_effect->func(end_update_effect->entry);
        }

        if (prerender_gui != nullptr) {
            prerender_gui->func(prerender_gui->entry);
        }

        for (auto func : chain) {
            //spdlog::info("Calling {}", func->description);

            func->func(func->entry);
        }

#if defined(PRAGMATA)
        if (pragmatavr::perf::g_enabled.load(std::memory_order_relaxed)) {
            const auto t2 = pragmatavr::perf::now_ns();
            pragmatavr::perf::Stats::get().rerun(pragmata_rerun_t1 - pragmata_rerun_t0, t2 - pragmata_rerun_t1);
        }
#endif

        restore_camera();

        m_in_render = false;
        inside_on_end = false;
    }
}

void VR::on_pre_wait_rendering(void* entry) {
}

void VR::on_wait_rendering(void* entry) {
    

    if (!get_runtime()->loaded) {
        return;
    }

    timed_out = false;

    if (!is_hmd_active()) {
        return;
    }

    //if (is_using_afw_foveated()) {
    //    return;
    //}

    // wait for m_present_finished (std::condition_variable)
    // to be signaled
    // only on the left eye interval because we need the right eye
    // to start render work as soon as possible
    // case 1: sequential frames and rendered frame == right eye (every two frames)
    // case 2: AFW and no foveated rendering (each frame)
    // case 3: AFW and foveated rendering
    if ((is_using_sf() && m_render_frame_count % 2 == m_right_eye_interval) || is_using_afw()|| is_using_afw_foveated()) {
        if (WaitForSingleObject(m_present_finished_event, 333) == WAIT_TIMEOUT) {
            timed_out = true;
        }

        ResetEvent(m_present_finished_event);
    }
}

void VR::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (!get_runtime()->loaded) {
        return;
    }

    m_camera_duplicator.on_pre_application_entry(entry, name, hash);

    switch (hash) {
        case "UpdateHID"_fnv:
            on_pre_update_hid(entry);
            break;
        case "WaitRendering"_fnv:
            on_pre_wait_rendering(entry);
            break;
        case "BeginRendering"_fnv:
            on_pre_begin_rendering(entry);
            break;
        case "EndRendering"_fnv:
            on_pre_end_rendering(entry);
            break;
        default:
            break;
    }
}

void VR::on_application_entry(void* entry, const char* name, size_t hash) {
    if (!get_runtime()->loaded) {
        return;
    }

    m_camera_duplicator.on_application_entry(entry, name, hash);

    switch (hash) {
        case "UpdateHID"_fnv:
            on_update_hid(entry);
            break;
        case "WaitRendering"_fnv:
            on_wait_rendering(entry);
            break;
        case "BeginRendering"_fnv:
            on_begin_rendering(entry);
            break;
        case "EndRendering"_fnv:
            on_end_rendering(entry);
            break;
        default:
            break;
    }
}

void VR::on_pre_update_hid(void* entry) {
    if (!get_runtime()->loaded || !is_hmd_active()) {
        return;
    }

    update_action_states();
}

void VR::on_update_hid(void* entry) {
    if (!get_runtime()->loaded || !is_hmd_active()) {
        return;
    }

#if not defined(RE2) and not defined(RE3)
    this->openvr_input_to_re_engine();
#endif

#ifdef RE8
    if (get_runtime()->handle_pause) {
        auto padman = sdk::get_managed_singleton<::REManagedObject>(game_namespace("HIDPadManager"));

        if (padman != nullptr) {
            auto merged_pad = sdk::call_object_func_easy<::REManagedObject*>(padman, "get_mergedPad");

            if (merged_pad != nullptr) {
                auto device = sdk::get_object_field<::REManagedObject*>(merged_pad, "Device");

                if (device != nullptr && *device != nullptr) {
                    sdk::call_object_func_easy<void*>(*device, "set_Button", via::hid::GamePadButton::CRight);
                }
            }
        }

        get_runtime()->handle_pause = false;
    }
#endif
}

void VR::openvr_input_to_re2_re3(REManagedObject* input_system) {
    if (!get_runtime()->loaded) {
        return;
    }

    static auto gui_master_type = sdk::find_type_definition(game_namespace("gui.GUIMaster"));
    static auto gui_master_get_instance = gui_master_type->get_method("get_Instance");
    static auto gui_master_get_input = gui_master_type->get_method("get_Input");
    static auto gui_master_input_type = gui_master_get_input->get_return_type();

    // ??????
#ifdef RE2
    static auto input_set_is_trigger_move_up_d = gui_master_input_type->get_method("set_IsTriggerMoveUpD");
    static auto input_set_is_trigger_move_down_d = gui_master_input_type->get_method("set_IsTriggerMoveDownD");
#endif

    auto gui_master = gui_master_get_instance->call<::REManagedObject*>(sdk::get_thread_context());
    auto gui_input = gui_master != nullptr ? 
                     gui_master_get_input->call<::REManagedObject*>(sdk::get_thread_context(), gui_master) : 
                     (::REManagedObject*)nullptr;

    auto ctx = sdk::get_thread_context();

    static auto get_lstick_method = sdk::get_object_method(input_system, "get_LStick");
    static auto get_rstick_method = sdk::get_object_method(input_system, "get_RStick");
    auto lstick = get_lstick_method->call<::REManagedObject*>(ctx);
    auto rstick = get_rstick_method->call<::REManagedObject*>(ctx);

    if (lstick == nullptr || rstick == nullptr) {
        return;
    }

    static auto get_button_bits_method = sdk::get_object_method(input_system, "get_ButtonBits");
    auto button_bits_obj = get_button_bits_method->call<::REManagedObject*>(ctx, input_system);

    if (button_bits_obj == nullptr) {
        return;
    }

    static auto get_active_user_input_unit_method = sdk::get_object_method(input_system, "getActiveUserInputUnit");
    auto input_unit_obj = get_active_user_input_unit_method->call<::REManagedObject*>(ctx, input_system);

    if (input_unit_obj == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    auto is_using_controller = (now - get_last_controller_update()) <= std::chrono::seconds(10);

    const auto is_grip_down = is_action_active(m_action_grip, m_right_joystick);
    const auto is_trigger_down = is_action_active(m_action_trigger, m_right_joystick);
    const auto is_left_grip_down = is_action_active(m_action_grip, m_left_joystick);
    const auto is_left_trigger_down = is_action_active(m_action_trigger, m_left_joystick);
    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, m_left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, m_right_joystick);

    const auto is_minimap_down = is_action_active(m_action_minimap, m_left_joystick) || is_action_active(m_action_minimap, m_right_joystick);
    const auto is_left_a_button_down = is_action_active(m_action_a_button, m_left_joystick);
    const auto is_left_b_button_down = !is_minimap_down && is_action_active(m_action_b_button, m_left_joystick);
    const auto is_right_a_button_down = is_action_active(m_action_a_button, m_right_joystick);
    const auto is_right_b_button_down = is_action_active(m_action_b_button, m_right_joystick);

    const auto is_dpad_up_down = is_action_active(m_action_dpad_up, m_left_joystick) || is_action_active(m_action_dpad_up, m_right_joystick);
    const auto is_dpad_right_down = is_action_active(m_action_dpad_right, m_left_joystick) || is_action_active(m_action_dpad_right, m_right_joystick);
    const auto is_dpad_down_down = is_action_active(m_action_dpad_down, m_left_joystick) || is_action_active(m_action_dpad_down, m_right_joystick);
    const auto is_dpad_left_down = is_action_active(m_action_dpad_left, m_left_joystick) || is_action_active(m_action_dpad_left, m_right_joystick);

    const auto is_weapon_dial_down = is_action_active(m_action_weapon_dial, m_left_joystick) || is_action_active(m_action_weapon_dial, m_right_joystick);
    const auto is_re3_dodge_down = is_action_active(m_action_re3_dodge, m_left_joystick) || is_action_active(m_action_re3_dodge, m_right_joystick);
    const auto is_quickturn_down = is_action_active(m_action_re2_quickturn, m_left_joystick) || is_action_active(m_action_re2_quickturn, m_right_joystick);
    const auto is_reset_view_down = is_action_active(m_action_re2_reset_view, m_left_joystick) || is_action_active(m_action_re2_reset_view, m_right_joystick);
    const auto is_change_ammo_down = is_action_active(m_action_re2_change_ammo, m_left_joystick) || is_action_active(m_action_re2_change_ammo, m_right_joystick);
	const auto is_toggle_flashlight_down = is_action_active(m_action_re2_toggle_flashlight, m_left_joystick) || is_action_active(m_action_re2_toggle_flashlight, m_right_joystick);

    const auto is_left_system_button_down = is_action_active(m_action_system_button, m_left_joystick);
    const auto is_right_system_button_down = is_action_active(m_action_system_button, m_right_joystick);

    
    
#if defined(RE2) || defined(RE3) || defined(RE8)
    if (is_toggle_flashlight_down && !m_was_flashlight_toggle_down) {            
        ManualFlashlight::g_manual_flashlight-> toggle_flashlight();
    }    

    m_was_flashlight_toggle_down = is_toggle_flashlight_down;
#endif


#if defined(RE2) || defined(RE3)
    const auto is_firstperson_toggle_down = is_action_active(m_action_re2_firstperson_toggle, m_left_joystick) || is_action_active(m_action_re2_firstperson_toggle, m_right_joystick);

    if (is_firstperson_toggle_down && !m_was_firstperson_toggle_down) {
        FirstPerson::get()->toggle();
    }

    m_was_firstperson_toggle_down = is_firstperson_toggle_down;
#endif

#if defined(RE2) || defined(RE3)
    const auto is_gripping_weapon = FirstPerson::get()->was_gripping_weapon();
#else
    const auto is_gripping_weapon = false;
#endif

    // Current actual button bits used by the game
    auto& button_bits_down = *sdk::get_object_field<uint64_t>(button_bits_obj, "Down");
    auto& button_bits_on = *sdk::get_object_field<uint64_t>(button_bits_obj, "On");
    auto& button_bits_up = *sdk::get_object_field<uint64_t>(button_bits_obj, "Up");

    // Set button state based on our own history we keep that doesn't get overwritten by the game
    auto set_button_state = [&](app::ropeway::InputDefine::Kind kind, bool state) {
        const auto kind_uint64 = (uint64_t)kind;

        if (state) {
            m_last_controller_update = now;
            is_using_controller = true;

            button_bits_up &= ~kind_uint64;
            m_button_states_up &= ~kind_uint64;

            // if "On" state is not set
            if ((m_button_states_on.to_ullong() & kind_uint64) == 0) {
                if (m_button_states_down.to_ullong() & kind_uint64) {
                    m_button_states_on |= kind_uint64;
                    m_button_states_down &= ~kind_uint64;

                    button_bits_on |= kind_uint64;
                    button_bits_down &= ~kind_uint64;
                } else {
                    m_button_states_on &= ~kind_uint64;
                    m_button_states_down |= kind_uint64;

                    button_bits_on &= ~kind_uint64;
                    button_bits_down |= kind_uint64;
                }
            } else {
                m_button_states_down &= ~kind_uint64;
                button_bits_down &= ~kind_uint64;

                m_button_states_on |= kind_uint64;
                button_bits_on |= kind_uint64;
            }
        } else {
            if (m_button_states_down.to_ullong() & kind_uint64 || m_button_states_on.to_ullong() & kind_uint64) {
                m_button_states_up |= kind_uint64;
                button_bits_up |= kind_uint64;

                m_last_controller_update = now;
                is_using_controller = true;
            } else if (is_using_controller) {
                m_button_states_up &= ~kind_uint64;
                button_bits_up &= ~kind_uint64;
            }

            // Don't want to screw with the user's input if they aren't actively
            // Using their VR controllers
            if (is_using_controller) {
                button_bits_down &= ~kind_uint64;
                m_button_states_down &= ~kind_uint64;
                
                button_bits_on &= ~kind_uint64;
                m_button_states_on &= ~kind_uint64;
            }
        }
    };

    // Right Grip: Aim, UI Right (RB)
    set_button_state(app::ropeway::InputDefine::Kind::HOLD, is_grip_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_RIGHT, is_grip_down);

    // Left Grip: Alternate aim (grenades, knives, etc), UI left (LB)
    set_button_state(app::ropeway::InputDefine::Kind::SUPPORT_HOLD, is_left_grip_down && !is_gripping_weapon);   
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_LEFT, is_left_grip_down);

    // Right Trigger (RB): Attack, Alternate UI right (RT), GE_RTrigBottom (quick time event), GE_RTrigTop (another quick time event)
    set_button_state(app::ropeway::InputDefine::Kind::ATTACK, is_trigger_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_RIGHT_2, is_trigger_down);
    set_button_state((app::ropeway::InputDefine::Kind)18014398509481984, is_trigger_down);
    set_button_state((app::ropeway::InputDefine::Kind)9007199254740992, is_trigger_down);
    //set_button_state((app::ropeway::InputDefine::Kind)4503599627370496, is_trigger_down);

    // Left Trigger (LB): Alternate UI left (LT), DEFENSE (LB)
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_LEFT_2, is_left_trigger_down);
    set_button_state(app::ropeway::InputDefine::Kind::DEFENSE, is_left_trigger_down);

    // L3: Sprint
    set_button_state(app::ropeway::InputDefine::Kind::JOG1, is_left_joystick_click_down);

    // R3: Reset camera
    set_button_state(app::ropeway::InputDefine::Kind::RESET_CAMERA, is_reset_view_down);

    // Left B: Inventory, PRESS_START
    set_button_state(app::ropeway::InputDefine::Kind::INVENTORY, is_left_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_left_b_button_down);

    // Left A: QUICK_TURN, PRESS_START, CANCEL, DIALOG_CANCEL
    set_button_state(app::ropeway::InputDefine::Kind::QUICK_TURN, is_quickturn_down); // unique, unbound by default as it causes issues
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_left_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::CANCEL, is_left_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DIALOG_CANCEL, is_left_a_button_down);
    
    // Right A: Action, ITEM, PRESS_START, DECIDE, DIALOG_DECIDE, (1 << 51)
    set_button_state(app::ropeway::InputDefine::Kind::ACTION, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::ITEM, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DECIDE, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DIALOG_DECIDE, is_right_a_button_down);

    // only allow dodging if there's nothing to interact with nearby
    if (!is_right_a_button_down || (now - m_last_interaction_display) >= std::chrono::milliseconds(200)) {
        set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 51), is_re3_dodge_down); // RE3 dodge
    } else {
        set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 51), false); // RE3 dodge
    }
    
    // Right B: Reload, Skip Event, UI_EXCHANGE, UI_RESET, (1 << 52) (that one is RE3 only? don't see it in the enum)
    set_button_state(app::ropeway::InputDefine::Kind::RELOAD, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::SKIP_EVENT, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_EXCHANGE, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_RESET, is_right_b_button_down);
    set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 52), is_right_b_button_down);

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);

    if (!is_weapon_dial_down) {
        // DPad Up: Shortcut Up
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_UP, is_dpad_up_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_MAP_UP, is_dpad_up_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_UP, is_dpad_up_down); // IsTriggerMoveUpD in RE3

        // DPad Right: Shortcut Right
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_RIGHT, is_dpad_right_down);

        // DPad Down: Shortcut Down
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_DOWN, is_dpad_down_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_MAP_DOWN, is_dpad_down_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_DOWN, is_dpad_down_down); // IsTriggerMoveDownD in RE3

        // DPad Left: Shortcut Left
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_LEFT, is_dpad_left_down);

        // well this was really annoying to figure out
#ifdef RE2
        if (is_dpad_up_down && gui_input != nullptr) {
            input_set_is_trigger_move_up_d->call<void*>(ctx, gui_input, true);
        }

        if (is_dpad_down_down && gui_input != nullptr) {
            input_set_is_trigger_move_down_d->call<void*>(ctx, gui_input, true);
        }
#endif
    } else {
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_UP, left_axis.y > 0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_RIGHT, left_axis.x > 0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_DOWN, left_axis.y < -0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_LEFT, left_axis.x < -0.9f);
    }

    // Change Ammo
    set_button_state(app::ropeway::InputDefine::Kind::CHANGE_BULLET, is_change_ammo_down);

    // MiniMap
    set_button_state(app::ropeway::InputDefine::Kind::MINIMAP, is_minimap_down);

    // Left or Right System Button: Pause
    set_button_state(app::ropeway::InputDefine::Kind::PAUSE, is_left_system_button_down || is_right_system_button_down || get_runtime()->handle_pause);
    get_runtime()->handle_pause = false;

    // Fixes QTE bound to triggers
    if (is_using_controller) {
        const auto left_trigger_state = is_left_trigger_down ? 1.0f : 0.0f;
        const auto right_trigger_state = is_trigger_down ? 1.0f : 0.0f;

        static auto set__analog_l_method = sdk::get_object_method(input_unit_obj, "set__AnalogL");
        static auto set__analog_r_method = sdk::get_object_method(input_unit_obj, "set__AnalogR");
        set__analog_l_method->call<void*>(ctx, input_unit_obj, left_trigger_state);
        set__analog_r_method->call<void*>(ctx, input_unit_obj, right_trigger_state);
    }

    bool moved_sticks = false;
    const auto deadzone = m_joystick_deadzone->value();

    if (left_axis_len > deadzone) {
        moved_sticks = true;

        // Override the left stick's axis values to the VR controller's values
        Vector3f axis{ left_axis.x, left_axis.y, 0.0f };

        static auto update_method = sdk::get_object_method(lstick, "update");
        update_method->call<void*>(ctx, lstick, &axis, &axis);
    }

    if (right_axis_len > deadzone) {
        moved_sticks = true;

        // Override the right stick's axis values to the VR controller's values
        Vector3f axis{ right_axis.x, right_axis.y, 0.0f };

        static auto update_method = sdk::get_object_method(rstick, "update");
        update_method->call<void*>(ctx, rstick, &axis, &axis);
    }

    if (moved_sticks) {
        auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

        const auto delta = sdk::Application::get()->get_delta_time();
        const auto highest_length = std::max<float>(glm::length(right_axis), glm::length(left_axis));

        new_pos.y = m_standing_origin.y;
        // Don't set the Y because it would look really strange
        m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
    }

    set_button_state(app::ropeway::InputDefine::Kind::MOVE, left_axis_len > deadzone);
    set_button_state(app::ropeway::InputDefine::Kind::UI_L_STICK, left_axis_len > deadzone);

    set_button_state(app::ropeway::InputDefine::Kind::WATCH, right_axis_len > deadzone);
    set_button_state(app::ropeway::InputDefine::Kind::UI_R_STICK, right_axis_len > deadzone);
    //set_button_state(app::ropeway::InputDefine::Kind::RUN, right_axis_len > 0.01f);

    // Causes the right stick to take effect properly
    if (is_using_controller) {
        static auto set_input_mode_method = sdk::get_object_method(input_system, "set_InputMode");
        set_input_mode_method->call<void*>(ctx, input_system, app::ropeway::InputDefine::InputMode::Pad);
    }
}

void VR::openvr_input_to_re_engine() {
    // TODO: Get the "merged pad" and actually modify some inputs!

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);
    const auto now = std::chrono::steady_clock::now();

    bool moved_sticks = false;

    if (left_axis_len > m_joystick_deadzone->value()) {
        moved_sticks = true;
    }

    if (right_axis_len > m_joystick_deadzone->value()) {
        moved_sticks = true;
    }

    if (moved_sticks) {
        auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

        const auto delta = sdk::Application::get()->get_delta_time();
        const auto highest_length = std::max<float>(glm::length(right_axis), glm::length(left_axis));

        new_pos.y = m_standing_origin.y;
        // Don't set the Y because it would look really strange
        m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
        m_last_controller_update = now;
    }

    for (auto& it : m_action_handles) {
        if (is_action_active(it.second, m_left_joystick) || is_action_active(it.second, m_right_joystick)) {
            m_last_controller_update = now;
        }
    }
}

void VR::on_draw_ui() {
    // create VR tree entry in menu (imgui)
    if (get_runtime()->loaded) {
        ImGui::SetNextItemOpen(m_has_hw_scheduling, ImGuiCond_::ImGuiCond_FirstUseEver);
    } else {
        if (m_openvr->error && !m_openvr->dll_missing) {
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_FirstUseEver);
        } else {
            ImGui::SetNextItemOpen(false, ImGuiCond_::ImGuiCond_FirstUseEver);
        }
    }

    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    auto display_error = [](auto& runtime, std::string dll_name) {
        if (runtime == nullptr || !runtime->error && runtime->loaded) {
            return;
        }

        if (runtime->error && runtime->dll_missing) {
            ImGui::TextWrapped("%s not loaded: %s not found", runtime->name().data(), dll_name.data());
            ImGui::TextWrapped("Please drop the %s file into the game's directory if you want to use %s", dll_name.data(), runtime->name().data());
        } else if (runtime->error) {
            ImGui::TextWrapped("%s not loaded: %s", runtime->name().data(), runtime->error->c_str());
        } else {
            ImGui::TextWrapped("%s not loaded: Unknown error", runtime->name().data());
        }

        ImGui::Separator();
    };

    display_error(m_openxr, "openxr_loader.dll");
    display_error(m_openvr, "openvr_api.dll");

    if (!get_runtime()->loaded) {
        ImGui::TextWrapped("No runtime loaded.");
        return;
    }

    ImGui::TextWrapped("Hardware scheduling: %s", m_has_hw_scheduling ? "Enabled" : "Disabled");

    if (m_has_hw_scheduling) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        ImGui::TextWrapped("WARNING: Hardware-accelerated GPU scheduling is enabled. This will cause the game to run slower.");
        ImGui::TextWrapped("Go into your Windows Graphics settings and disable \"Hardware-accelerated GPU scheduling\"");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    ImGui::TextWrapped("VR Runtime: %s", get_runtime()->name().data());
    ImGui::TextWrapped("Render Resolution: %d x %d", get_runtime()->get_width(), get_runtime()->get_height());

    if (get_runtime()->is_openvr()) {
        ImGui::TextWrapped("Resolution can be changed in SteamVR");
    } else if (get_runtime()->is_openxr()) {
        if (ImGui::TreeNode("Bindings")) {
            m_openxr->display_bindings_editor();
            ImGui::TreePop();
        }

        if (m_resolution_scale->draw("Resolution Scale")) {
            m_openxr->resolution_scale = m_resolution_scale->value();
        }
    }
    
    ImGui::Combo("Sync Mode", (int*)&get_runtime()->custom_stage, "Early\0Late\0Very Late\0");
    ImGui::Separator();

    if (ImGui::Button("Set Standing Height")) {
        m_standing_origin.y = get_position(0).y;
    }

    if (ImGui::Button("Set Standing Origin") || m_set_standing_key->is_key_down_once()) {
        m_standing_origin = get_position(0);
    }

    if (ImGui::Button("Recenter View") || m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (ImGui::Button("Reinitialize Runtime")) {
        get_runtime()->wants_reinitialize = true;
    }

    //ImGui::DragFloat4("Right Bounds", (float*)&m_right_bounds, 0.005f, -2.0f, 2.0f);
    //ImGui::DragFloat4("Left Bounds", (float*)&m_left_bounds, 0.005f, -2.0f, 2.0f);

    ImGui::Separator();

    m_set_standing_key->draw("Set Standing Origin Key");
    m_recenter_view_key->draw("Recenter View Key");

    ImGui::Separator();

    m_rendering_technique->draw("Rendering Technique");
    if (m_rendering_technique->value() == ALTERNATE_FRAME_WARPING) {
        m_clear_before_framewarp->draw("Clear Before Framewarp");
        m_framewarp_debug->draw("Debug Framewarp");
        m_enable_ui_fix->draw("Enable Framewarp UI Fix");
        m_enable_sharpening->draw("Enable Sharpening");
        m_sharpness->draw("Sharpness");
        m_fix_upscalers_wobbling->draw("Fix Upscalers Wobbling");
        m_fix_item_inspection->draw("Fix Item Inspection Double Vision");
        m_disable_volumetric_fog->draw("Disable Volumetric Fog");
        m_ignore_motion_threshold->draw("Ignore Motion Threshold");
#if defined(RE9)
        m_enable_foveated_rendering->draw("Enable Foveated Rendering");
        static int delayInit = -1;
        static float pendingValue = m_foveated_ratio->value();
        if (delayInit > 0 && --delayInit == 0) {
            delayInit = -1;
            float& value = m_foveated_ratio->value();
            value = pendingValue;
        }
        if (m_enable_foveated_rendering->value()) {
            m_force_fixed_foveated->draw("Force Fixed Foveated Area");

            if (ImGui::Button(" 0.33 ")) {
                delayInit = 30;
                pendingValue = 0.3333333333f;
            }
            ImGui::SameLine(0, 4.0f);
            if (ImGui::Button(" 0.5 ")) {
                delayInit = 30;
                pendingValue = 0.5f;
            }
            ImGui::SameLine(0, 4.0f);
            ImGui::PushID(*m_foveated_ratio);
            if (ImGui::DragFloat("Foveated Ratio", &pendingValue, 0.01f, m_foveated_ratio->range().x, m_foveated_ratio->range().y)) {
                delayInit = 90;
            }
            ImGui::PopID();
            m_foveated_periphery_blurs->draw("Foveated Periphery Blur");
            m_foveated_fade->draw("Foveated Fade");
            m_foveated_rounded_radius->draw("Foveated Rounded Radius");
            m_foveated_offset_x->draw("Foveated Offset X");
            m_foveated_offset_y->draw("Foveated Offset Y");
            m_edge_scan_line_fix_range->draw("Edge Scan Line Fix Range");
        }
#endif
    }
    ImGui::Separator();

    m_decoupled_pitch->draw("Decoupled Camera Pitch");

    if (ImGui::Checkbox("Positional Tracking", &m_positional_tracking)) {
    }

    m_hmd_oriented_audio->draw("Head Oriented Audio");
    m_use_custom_view_distance->draw("Use Custom View Distance");
    m_view_distance->draw("View Distance/FarZ");
    m_motion_controls_inactivity_timer->draw("Inactivity Timer");
    m_joystick_deadzone->draw("Joystick Deadzone");

    m_ui_scale_option->draw("2D UI Scale");
    m_ui_distance_option->draw("2D UI Distance");
    m_world_ui_scale_option->draw("World-Space UI Scale");

    ImGui::DragFloat3("Overlay Rotation", (float*)&m_overlay_rotation, 0.01f, -360.0f, 360.0f);
    ImGui::DragFloat3("Overlay Position", (float*)&m_overlay_position, 0.01f, -100.0f, 100.0f);

    ImGui::Separator();
    ImGui::Text("Graphical Options");

    m_force_fps_settings->draw("Force Uncap FPS");
    m_force_aa_settings->draw("Force Disable TAA");
    m_force_motionblur_settings->draw("Force Disable Motion Blur");
    m_force_vsync_settings->draw("Force Disable V-Sync");
    m_force_lensdistortion_settings->draw("Force Disable Lens Distortion");
    m_force_volumetrics_settings->draw("Force Disable Volumetrics");
    m_force_lensflares_settings->draw("Force Disable Lens Flares");
    m_force_dynamic_shadows_settings->draw("Force Enable Dynamic Shadows");
    m_allow_engine_overlays->draw("Allow Engine Overlays");
    m_enable_asynchronous_rendering->draw("Enable Asynchronous Rendering");

    if (ImGui::TreeNode("Desktop Recording Fix")) {
        ImGui::PushID("Desktop");
        m_desktop_fix->draw("Enabled");
        m_desktop_fix_skip_present->draw("Skip Present");
        ImGui::PopID();
        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::Text("Debug info");
    m_camera_duplicator.on_draw_ui();
    

    ImGui::Checkbox("Disable Projection Matrix Override", &m_disable_projection_matrix_override);
    ImGui::Checkbox("Disable GUI Projection Matrix Override", &m_disable_gui_camera_projection_matrix_override);
    ImGui::Checkbox("Disable View Matrix Override", &m_disable_view_matrix_override);
    ImGui::Checkbox("Disable Backbuffer Size Override", &m_disable_backbuffer_size_override);
    ImGui::Checkbox("Disable Temporal Fix", &m_disable_temporal_fix);
    ImGui::Checkbox("Disable Post Effect Fix", &m_disable_post_effect_fix);
    
    const double min_ = 0.0;
    const double max_ = 25.0;
    ImGui::SliderScalar("Prediction Scale", ImGuiDataType_Double, &m_openxr->prediction_scale, &min_, &max_);

    ImGui::DragFloat4("Raw Left", (float*)&m_raw_projections[0], 0.01f, -100.0f, 100.0f);
    ImGui::DragFloat4("Raw Right", (float*)&m_raw_projections[1], 0.01f, -100.0f, 100.0f);

    // convert m_avg_input_delay (std::chrono::nanoseconds) to milliseconds (float)
    auto duration_float = std::chrono::duration<float, std::milli>(m_avg_input_delay).count();

    ImGui::DragFloat("Avg Input Processing Delay (MS)", &duration_float, 0.00001f);
}

void VR::on_device_reset() {
    std::scoped_lock _{m_openxr->sync_mtx};

    m_multipass.eye_textures[0] = nullptr;
    m_multipass.eye_textures[1] = nullptr;

    spdlog::info("VR: on_device_reset");
    m_backbuffer_inconsistency = false;
    if (g_framework->is_dx11()) {
        m_d3d11.on_reset(this);
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    }

    m_overlay_component.on_reset();

    // so i guess device resets can happen between begin and end rendering...
    if (m_in_render) {
        spdlog::info("VR: on_device_reset: in_render");

        // what the fuck
        if (m_needs_camera_restore) {
            spdlog::info("VR: on_device_reset: needs_camera_restore");
            restore_camera();
        }
    }

    if (inside_on_end) {
        spdlog::info("VR: on_device_reset: inside_on_end");
    }
}

void VR::on_config_load(const utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_load(cfg);
    }

    // Run the rest of OpenXR initialization code here that depends on config values
    if (get_runtime()->is_openxr() && get_runtime()->loaded) {
        spdlog::info("[VR] Finishing up OpenXR initialization");

        m_openxr->resolution_scale = m_resolution_scale->value();
        initialize_openxr_swapchains();
    }

    if (m_motion_controls_inactivity_timer->value() <= 10.0f) {
        m_motion_controls_inactivity_timer->value() = 30.0f;
    }
}

void VR::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

std::array<RECamera*, 2> VR::get_cameras() const {
    if (is_using_afw_foveated()) {
        return m_multipass_cameras;
    }

    return std::array<RECamera*, 2>{ sdk::get_primary_camera(), nullptr };
}

Vector4f VR::get_position(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };
    std::shared_lock __{ get_runtime()->eyes_mtx };

    return get_position_unsafe(index);
}

Vector4f VR::get_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_velocity_unsafe(index);
}

Vector4f VR::get_angular_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_angular_velocity_unsafe(index);
}

Vector4f VR::get_position_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        auto result = glm::rowMajor4(matrix)[3];
        result.w = 1.0f;

        return result;
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // HMD position
        if (index == 0 && !m_openxr->stage_views.empty()) {
            return Vector4f{ *(Vector3f*)&m_openxr->view_space_location.pose.position, 1.0f };
        } else if (index > 0) {
            return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].location.pose.position, 1.0f };
        }

        return Vector4f{};
    } 

    return Vector4f{};
}

Vector4f VR::get_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& velocity = pose.vVelocity;

        return Vector4f{ velocity.v[0], velocity.v[1], velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }

        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].velocity.linearVelocity, 0.0f };
    }

    return Vector4f{};
}

Vector4f VR::get_angular_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& angular_velocity = pose.vAngularVelocity;

        return Vector4f{ angular_velocity.v[0], angular_velocity.v[1], angular_velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }
    
        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].velocity.angularVelocity, 0.0f };
    }

    return Vector4f{};
}

Matrix4x4f VR::get_rotation(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::extractMatrixRotation(glm::rowMajor4(matrix));
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };
        std::shared_lock __{ get_runtime()->eyes_mtx };

        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            return Matrix4x4f{*(glm::quat*)&m_openxr->view_space_location.pose.orientation};
            //return Matrix4x4f{*(glm::quat*)&m_openxr->stage_views[0].pose.orientation};
        } else if (index > 0) {
            if (index == VRRuntime::Hand::LEFT+1) {
                return Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.orientation};
            } else if (index == VRRuntime::Hand::RIGHT+1) {
                return Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.orientation};
            }
        }

        return glm::identity<Matrix4x4f>();
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };

        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->view_space_location.pose.orientation};
            mat[3] = Vector4f{*(Vector3f*)&m_openxr->view_space_location.pose.position, 1.0f};
            return mat;
        } else if (index > 0) {
            if (index == VRRuntime::Hand::LEFT+1) {
                auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.orientation};
                mat[3] = Vector4f{*(Vector3f*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.position, 1.0f};
                return mat;
            } else if (index == VRRuntime::Hand::RIGHT+1) {
                auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.orientation};
                mat[3] = Vector4f{*(Vector3f*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.position, 1.0f};
                return mat;
            }
        }
    }

    return glm::identity<Matrix4x4f>();
}

vr::HmdMatrix34_t VR::get_raw_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return vr::HmdMatrix34_t{};
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        return pose.mDeviceToAbsoluteTracking;
    } else {
        spdlog::error("VR: get_raw_transform: not implemented for {}", get_runtime()->name());
        return vr::HmdMatrix34_t{};
    }
}

bool VR::is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source) const {
    if (!get_runtime()->loaded) {
        return false;
    }
    
    bool active = false;

    if (get_runtime()->is_openvr()) {
        vr::InputDigitalActionData_t data{};
        vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), source);

        active = data.bActive && data.bState;
    } else if (get_runtime()->is_openxr()) {
        active = m_openxr->is_action_active((XrAction)action, (VRRuntime::Hand)source);
    }

    if (!active && action == m_action_minimap) {
        active = is_action_active(m_action_b_button, m_left_joystick) && is_hand_behind_head(VRRuntime::Hand::LEFT);
    }

    return active;
}

Vector2f VR::get_joystick_axis(vr::VRInputValueHandle_t handle) const {
    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    if (get_runtime()->is_openvr()) {
        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_joystick, &data, sizeof(data), handle);

        const auto deadzone = m_joystick_deadzone->value();
        const auto out = Vector2f{ data.x, data.y };

        return glm::length(out) > deadzone ? out : Vector2f{};
    } else if (get_runtime()->is_openxr()) {
        if (handle == (vr::VRInputValueHandle_t)VRRuntime::Hand::LEFT) {
            auto out = m_openxr->get_left_stick_axis();
            return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
        } else if (handle == (vr::VRInputValueHandle_t)VRRuntime::Hand::RIGHT) {
            auto out = m_openxr->get_right_stick_axis();
            return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
        }
    }

    return Vector2f{};
}

Vector2f VR::get_left_stick_axis() const {
    return get_joystick_axis(m_left_joystick);
}

Vector2f VR::get_right_stick_axis() const {
    return get_joystick_axis(m_right_joystick);
}

void VR::trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source) {
    if (!get_runtime()->loaded || !is_using_controllers()) {
        return;
    }

    if (get_runtime()->is_openvr()) {
        vr::VRInput()->TriggerHapticVibrationAction(m_action_haptic, seconds_from_now, duration, frequency, amplitude, source);
    } else if (get_runtime()->is_openxr()) {
        m_openxr->trigger_haptic_vibration(duration, frequency, amplitude, (VRRuntime::Hand)source);
    }
}
