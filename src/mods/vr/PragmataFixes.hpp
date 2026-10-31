#pragma once

// PragmataVR Build 36: the community fix pack's Lua HUD fixes (and our ghost layer script), ported to C++.
// Build 46: rewritten after the crash hunt of 2026-10-30.
//
// Builds 36 to 45 made these changes from the GUI draw callbacks, on the render threads, on every frame: hide a HUD
// piece before the element drew and show it again right after, look pieces up by name (a new managed string each
// time) and change the location label's colour. Steve's flat tests with VR off pinned the random worker-thread
// crashes (animation, collision, navigation, hacking jobs, all through corrupted pointers) on this code: PureDark's
// original DLL and Build 35 (no port) ran the hacking loop clean; Builds 36 to 40 crashed within a couple of hacks,
// even with the HUD code off. The hacking gauge and door hack effects it toggled are animated by the game's own
// workers during a hack, which is where the crashes clustered.
//
// Now:
//   - All changes happen on the main thread, in the application entry just before PrerenderGUI (after the game's
//     own update and behaviour code for the frame, before the GUI is prepared for drawing). Nothing is written from
//     the render threads.
//   - Each change is made once and left in place (hide only if visible, never hide and restore around a draw).
//     Turning a fix off shows again what this fix hid.
//   - Name lookups use managed strings created once and kept alive (add_ref), not new strings per lookup.
//   - The draw callback only notes which HUD elements are being drawn (no engine calls besides reading the name it
//     already read) and skips the target brackets, which changes nothing in the game.
//   - Every fix has its own switch in pragmatavr_hud.ini (fix_brackets, hide_ghost, fix_label, fix_door_vfx,
//     fix_gauge_vfx, hide_hack_backdrop).
//
// The fixes (behaviour of the original scripts):
//   zz_pragmata_transform_probe.lua        ui3203Gui, ui2160Gui (hacking target brackets, decoration): not drawn
//   zz_pragmatavr_hide_ghost.lua           offset "aberration" layer hidden on ui2100/2110/2120/2130/2140/2150/2201
//   zzzzz_pragmata_label_probe.lua         ui3010Gui (location label): Overlay off, title panel alpha 1 when the title
//                                          text is the only visible child
//   zzzzzz_pragmata_door_hack_inspect.lua  ui3500Gui, ui3510Gui: VFXtex effect textures hidden
//   zzzzzzzz_pragmata_hud_orange_test.lua  ui2130Gui: hackingGauge_VFX effect texture hidden
//   Build 42                               ui3500Gui, ui3510Gui: dimming overlays PNL_1, PNL_, rect_1, rect_ hidden

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "Mod.hpp"
#include "sdk/REComponent.hpp"
#include "sdk/REContext.hpp"
#include "sdk/REGameObject.hpp"
#include "sdk/REManagedObject.hpp"
#include "sdk/REString.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/SceneManager.hpp"
#include "sdk/Math.hpp"
#include "utility/String.hpp"

#include "PragmataHud.hpp"
#include "PragmataScreenProbe.hpp"
#include "PragmataMarkerFix.hpp"
#include "PragmataTexDiag.hpp"

class PragmataFixes : public Mod {
public:
    static std::shared_ptr<PragmataFixes>& get() {
        static auto inst = std::make_shared<PragmataFixes>();
        return inst;
    }

    std::string_view get_name() const override { return "PragmataFixes"; }

    // Render threads: read the name (as before), note the element as drawn, skip the brackets. No writes.
    bool on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) override {
        auto go = utility::re_component::get_game_object(gui_element);
        if (go == nullptr) {
            return true;
        }

        const auto name = utility::re_game_object::get_name(go);
        const auto h = utility::hash(name);

        // Build 48: screen probe, note when enemy bars / damage numbers draw.
        if (h == "ui3200Gui"_fnv || h == "ui3210Gui"_fnv) {
            pragmata_screen_probe::g_enemy_bar_seen_ms.store(now_ms(), std::memory_order_relaxed);
        } else if (h == "ui3201Gui"_fnv) {
            pragmata_screen_probe::g_damage_seen_ms.store(now_ms(), std::memory_order_relaxed);
        }

        // Build 57: names of every GUI drawn while a snapshot is taken (logged on the main thread).
        if (now_ms() < pragmatavr::g_gui_list_until_ms.load(std::memory_order_relaxed)) {
            std::scoped_lock _{s_gui_list_mtx};
            if (s_gui_list.size() < 128) {
                s_gui_list.emplace(name);
            }
        }

        // Build 57: the red low health effect at the screen edges.
        if (h == "ui2030Gui"_fnv && pragmatavr::HudTuning::get().tune()->hide_low_health) {
            log_once(h, "ui2030Gui: low health screen edges not drawn");
            return false;
        }

        if (h == "ui3203Gui"_fnv || h == "ui2160Gui"_fnv) {
            if (pragmatavr::HudTuning::get().tune()->fix_brackets) {
                log_once(h, fmt::format("{}: hacking target brackets / decoration not drawn", name));
                return false;
            }
            return true;
        }

        for (size_t i = 0; i < TARGETS.size(); ++i) {
            if (TARGETS[i].hash == h) {
                s_seen_ms[i].store(now_ms(), std::memory_order_relaxed);
                break;
            }
        }
        return true;
    }

    // Main thread, once per frame, before the GUI is prepared for drawing.
    void on_pre_application_entry(void* entry, const char* name, size_t hash) override {
        if (hash != "PrerenderGUI"_fnv) {
            return;
        }
        if (!s_announced) {
            s_announced = true;
            spdlog::info("[PragmataVR] Fixes: HUD fixes run on the main thread before PrerenderGUI (Build 46)");
        }
        apply();
        report_gui_list();
        pragmata_tex_diag::tick(); // Build 58: texture streaming diagnostic (logging only)
        // Build 59: Sharp textures, checked twice a second (the game may write its own values back on a settings change).
        {
            static int64_t last = 0;
            const auto t = now_ms();
            if (t - last >= 500) {
                last = t;
                pragmata_tex_diag::sharp_textures(pragmatavr::HudTuning::get().tune()->sharp_textures);
            }
        }
        pragmata_marker_fix::install(); // Build 50, once
        if (pragmatavr::HudTuning::get().tune()->screen_probe) {
            pragmata_screen_probe::install(); // Build 48, once
            pragmata_screen_probe::report();
        }
    }

private:
    enum Kind : uint32_t {
        KindGhost = 1u << 0,
        KindGauge = 1u << 1,
        KindDoor = 1u << 2,
        KindLabel = 1u << 3,
        KindSubtitle = 1u << 4, // Build 58
    };

    struct Target {
        const char* name;
        size_t hash;
        uint32_t kinds;
    };

    static inline const std::array<Target, 11> TARGETS{{
        {"ui2100Gui", "ui2100Gui"_fnv, KindGhost},
        {"ui2110Gui", "ui2110Gui"_fnv, KindGhost},
        {"ui2120Gui", "ui2120Gui"_fnv, KindGhost},
        {"ui2130Gui", "ui2130Gui"_fnv, KindGhost | KindGauge},
        {"ui2140Gui", "ui2140Gui"_fnv, KindGhost},
        {"ui2150Gui", "ui2150Gui"_fnv, KindGhost},
        {"ui2201Gui", "ui2201Gui"_fnv, KindGhost},
        {"ui3500Gui", "ui3500Gui"_fnv, KindDoor},
        {"ui3510Gui", "ui3510Gui"_fnv, KindDoor},
        {"ui3010Gui", "ui3010Gui"_fnv, KindLabel},
        {"ui2010Gui", "ui2010Gui"_fnv, KindSubtitle}, // Build 58: subtitles shown? (read only)
    }};

    // Only elements drawn within this window are looked up (absent HUD screens cost nothing).
    static constexpr int64_t SEEN_WINDOW_MS = 2000;

    static inline std::array<std::atomic<int64_t>, 11> s_seen_ms{};
    static inline bool s_announced{false};

    // Main thread only.
    static inline std::unordered_map<size_t, bool> s_hidden_by_us{};
    static inline std::unordered_map<std::wstring, SystemString*> s_strings{};

    // Build 57: GUIs drawn during a snapshot.
    static inline std::mutex s_gui_list_mtx{};
    static inline std::set<std::string> s_gui_list{};

    static void report_gui_list() {
        if (now_ms() < pragmatavr::g_gui_list_until_ms.load(std::memory_order_relaxed)) {
            return;
        }
        std::string names{};
        {
            std::scoped_lock _{s_gui_list_mtx};
            if (s_gui_list.empty()) {
                return;
            }
            for (const auto& n : s_gui_list) {
                names += " " + n;
            }
            s_gui_list.clear();
        }
        spdlog::info("[PragmataVR] Snapshot GUIs drawn:{}", names);
    }

    static inline std::mutex s_logged_mtx{};
    static inline std::unordered_map<size_t, bool> s_logged{};

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static void log_once(size_t key, const std::string& msg) {
        std::scoped_lock _{s_logged_mtx};
        if (s_logged.emplace(key, true).second) {
            spdlog::info("[PragmataVR] Fixes: {}", msg);
        }
    }

    static std::string type_name(REManagedObject* o) {
        auto td = utility::re_managed_object::get_type_definition(o);
        return td != nullptr ? std::string{td->get_full_name()} : std::string{};
    }

    // A managed string created once and kept alive, so lookups do not allocate every frame.
    static SystemString* managed(std::wstring_view s) {
        auto it = s_strings.find(std::wstring{s});
        if (it != s_strings.end()) {
            return it->second;
        }
        auto str = sdk::VM::create_managed_string(s);
        if (str != nullptr) {
            utility::re_managed_object::add_ref((REManagedObject*)str);
        }
        s_strings.emplace(std::wstring{s}, str);
        return str;
    }

    // view:call('getObject', path) / element:call('getObject', path), as the Lua scripts did.
    static REManagedObject* get_object(REManagedObject* owner, std::wstring_view path, sdk::VMContext* ctx) {
        auto str = managed(path);
        if (str == nullptr) {
            return nullptr;
        }
        return sdk::call_object_func<REManagedObject*>(owner, "getObject", ctx, owner, str);
    }

    static REGameObject* find_game_object(REManagedObject* scene, const char* name, sdk::VMContext* ctx) {
        static auto scene_t = sdk::find_type_definition("via.Scene");
        static auto find_method = scene_t != nullptr ? scene_t->get_method("findGameObject(System.String)") : nullptr;
        if (find_method == nullptr) {
            log_once("no findGameObject"_fnv, "via.Scene.findGameObject not found, HUD fixes inactive");
            return nullptr;
        }
        auto str = managed(utility::widen(name));
        if (str == nullptr) {
            return nullptr;
        }
        return find_method->call<REGameObject*>(ctx, scene, str);
    }

    static void apply() {
        const auto tune = pragmatavr::HudTuning::get().tune();
        const auto now = now_ms();

        auto scene = sdk::get_current_scene();
        if (scene == nullptr) {
            return;
        }
        auto ctx = sdk::get_thread_context();

        for (size_t i = 0; i < TARGETS.size(); ++i) {
            const auto& tg = TARGETS[i];
            const auto seen = s_seen_ms[i].load(std::memory_order_relaxed);
            if (seen == 0 || now - seen > SEEN_WINDOW_MS) {
                continue;
            }

            auto go = find_game_object(scene, tg.name, ctx);
            if (go == nullptr || go->transform == nullptr) {
                log_once(tg.hash ^ 0x51ed270b27f3ull, fmt::format("{}: drawn but not found in the scene, skipped", tg.name));
                continue;
            }
            auto gui = utility::re_component::find<::REComponent>((::REComponent*)go->transform, "via.gui.GUI");
            if (gui == nullptr) {
                continue;
            }
            auto view = sdk::call_object_func<REManagedObject*>((REManagedObject*)gui, "get_View", ctx, (REManagedObject*)gui);

            if (tg.kinds & KindGhost) {
                ghost((REManagedObject*)gui, tg, tune->hide_ghost, ctx);
            }
            if (view == nullptr) {
                continue;
            }
            if (tg.kinds & KindGauge) {
                set_hidden(view, tg, L"main/layout/offset/over/hackingGauge_VFX/effecttexture_", "via.gui.EffectTexture", tune->fix_gauge_vfx, ctx);
            }
            if (tg.kinds & KindDoor) {
                set_hidden(view, tg, L"main/layout/VFXtex/effecttexture00", "via.gui.EffectTexture", tune->fix_door_vfx, ctx);
                set_hidden(view, tg, L"main/layout/VFXtex/effecttexture01", "via.gui.EffectTexture", tune->fix_door_vfx, ctx);
                static const std::array<const wchar_t*, 4> backdrop{L"main/layout/PNL_1", L"main/layout/PNL_", L"main/layout/rect_1", L"main/layout/rect_"};
                for (const auto* p : backdrop) {
                    set_hidden(view, tg, p, nullptr, tune->hide_hack_backdrop, ctx);
                }
            }
            if ((tg.kinds & KindLabel) && tune->fix_label) {
                label(view, tg, ctx);
            }
            // Build 56: is the replicator panel of the item pickups element shown? (Fixed mode then cuts the bigger area.)
            if (tg.hash == "ui2120Gui"_fnv) {
                items_hub(view, ctx);
            }
            // Build 54: one-time layout dumps of the objective banner and the item pickups element, to find the piece
            // that clips them when Size is below 1.
            // Build 58: is a subtitle on screen? (Fixed and Follows Head then cut its area out of the eyes.)
            if (tg.kinds & KindSubtitle) {
                subtitles(view, ctx);
            }
            if (tg.hash == "ui2110Gui"_fnv || tg.hash == "ui2120Gui"_fnv || tg.hash == "ui2100Gui"_fnv ||
                tg.hash == "ui2010Gui"_fnv) { // Build 55: tutorial too. Build 58: subtitles too
                maybe_dump(view, tg, now, ctx);
            }
        }
    }

    // Build 58: a subtitle is shown when some via.gui.Text in ui2010Gui is visible (it and every parent), has some alpha
    // and a non-empty message. If the tree holds no Text at all the check cannot work, so the area is always used.
    static void subtitles(REManagedObject* view, sdk::VMContext* ctx) {
        int nodes = 0;
        int texts = 0;
        std::string sample{};
        const bool shown = find_text(view, 0, true, nodes, texts, sample, ctx);
        // Build 59: Build 58 only counted texts under visible panels, found none while no subtitle showed, and then used
        // the area all the time (it cut holes in menus). Now every text is counted and a failed check means "not shown".
        if (texts == 0) {
            log_once("ui2010 no text"_fnv, fmt::format("ui2010Gui: no via.gui.Text found ({} nodes), subtitle area not used", nodes));
        }
        if (pragmatavr::g_subtitles_shown.exchange(shown, std::memory_order_relaxed) != shown) {
            static int changes = 0;
            if (++changes <= 40) {
                spdlog::info("[PragmataVR] Subtitles: {} ({} text pieces, {} nodes){}{}", shown ? "shown" : "hidden", texts, nodes,
                             sample.empty() ? "" : ": ", sample.substr(0, 80));
            }
        }
    }

    // Every node is visited (texts under hidden panels are counted too); `shown_chain` = this node's parents are all
    // visible with some alpha.
    static bool find_text(REManagedObject* o, int depth, bool shown_chain, int& nodes, int& texts, std::string& sample,
                          sdk::VMContext* ctx) {
        bool found = false;
        for (auto c = o; c != nullptr && nodes < 400; ) {
            ++nodes;
            bool here = shown_chain && sdk::call_object_func<bool>(c, "get_Visible", ctx, c);
            if (here) {
                const auto col = sdk::call_object_func_easy<Vector4f>(c, "get_ColorScale");
                here = !std::isfinite(col.w) || col.w > 0.02f;
            }
            if (type_name(c) == "via.gui.Text") {
                ++texts;
                if (here && !found) {
                    auto msg = sdk::call_object_func<SystemString*>(c, "get_Message", ctx, c);
                    const auto m = msg != nullptr ? utility::re_string::get_string(msg) : std::string{};
                    if (m.find_first_not_of(" \t\r\n") != std::string::npos) {
                        sample = m;
                        found = true;
                    }
                }
            }
            if (depth < 14) {
                auto child = sdk::call_object_func<REManagedObject*>(c, "get_Child", ctx, c);
                if (child != nullptr && find_text(child, depth + 1, here, nodes, texts, sample, ctx)) {
                    found = true;
                }
            }
            if (depth == 0) {
                break;
            }
            c = sdk::call_object_func<REManagedObject*>(c, "get_Next", ctx, c);
        }
        return found;
    }

    // Build 56: the replicator panel (main/layout/offset/blur/ui2093) shown = visible with some alpha, here and in its
    // layout child. Read only.
    static void items_hub(REManagedObject* view, sdk::VMContext* ctx) {
        auto panel = get_object(view, L"main/layout/offset/blur/ui2093", ctx);
        if (panel == nullptr) {
            log_once("ui2093 missing"_fnv, "ui2120Gui: replicator panel ui2093 not found, item pickups keep the full area");
            pragmatavr::g_items_hub.store(true, std::memory_order_relaxed);
            return;
        }
        const bool vis = sdk::call_object_func<bool>(panel, "get_Visible", ctx, panel);
        const auto c = sdk::call_object_func_easy<Vector4f>(panel, "get_ColorScale");
        auto lay = get_object(panel, L"layout", ctx);
        const bool lvis = lay == nullptr || sdk::call_object_func<bool>(lay, "get_Visible", ctx, lay);
        const auto lc = lay != nullptr ? sdk::call_object_func_easy<Vector4f>(lay, "get_ColorScale") : Vector4f{1.0f, 1.0f, 1.0f, 1.0f};
        const bool shown = vis && lvis && std::isfinite(c.w) && c.w > 0.05f && std::isfinite(lc.w) && lc.w > 0.05f;
        if (pragmatavr::g_items_hub.exchange(shown, std::memory_order_relaxed) != shown) {
            spdlog::info("[PragmataVR] Item pickups: replicator panel {} (visible {} alpha {:.2f}, layout visible {} alpha {:.2f})",
                         shown ? "shown, full area" : "hidden, field area", vis, c.w, lvis, lc.w);
        }
    }

    // ==== Build 54: GUI layout dump (diagnostic, main thread, read only) ====
    static inline std::unordered_map<size_t, int> s_dumps_done{};
    static inline std::unordered_map<size_t, int64_t> s_first_seen{};
    static inline std::unordered_map<std::string, bool> s_type_listed{};

    static void maybe_dump(REManagedObject* view, const Target& tg, int64_t now, sdk::VMContext* ctx) {
        auto& first = s_first_seen[tg.hash];
        if (first == 0) {
            first = now;
        }
        auto& done = s_dumps_done[tg.hash];
        // Three dumps: right away, after 3 s and after 15 s (the element animates in, and its content changes).
        static constexpr std::array<int64_t, 3> at{0, 3000, 15000};
        if (done >= (int)at.size() || now - first < at[done]) {
            return;
        }
        ++done;
        spdlog::info("[PragmataVR] Layout dump {} #{} (view type {}):", tg.name, done,
                     sdk::call_object_func<uint32_t>(view, "get_ViewType", ctx, view));
        int count = 0;
        dump_node(view, 0, count, ctx);
        spdlog::info("[PragmataVR] Layout dump {} #{} end ({} nodes)", tg.name, done, count);
    }

    static bool clip_like(std::string_view n) {
        static constexpr std::array<std::string_view, 9> keys{"Clip", "Mask", "Scissor", "Region", "Stencil", "Rect", "Size", "Overflow", "Cull"};
        for (auto k : keys) {
            if (n.find(k) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    }

    // Lists, once per GUI type, the getters whose names suggest clipping or sizes (this type and its parents).
    static void list_type(sdk::RETypeDefinition* td) {
        if (td == nullptr) {
            return;
        }
        const auto tn = td->get_full_name();
        if (!s_type_listed.emplace(tn, true).second) {
            return;
        }
        std::string names{};
        for (auto t = td; t != nullptr; t = t->get_parent_type()) {
            const auto tfn = t->get_full_name();
            if (tfn == "System.Object" || tfn == "via.Component") {
                break;
            }
            for (auto& m : t->get_methods()) {
                const char* mn = m.get_name();
                if (mn == nullptr) {
                    continue;
                }
                const std::string_view v{mn};
                if (v.rfind("get_", 0) == 0 && clip_like(v) && m.get_num_params() == 0) {
                    auto rt = m.get_return_type();
                    names += fmt::format(" {}:{}", v.substr(4), rt != nullptr ? rt->get_full_name() : std::string{"?"});
                }
            }
        }
        spdlog::info("[PragmataVR] Layout type {}:{}", tn, names.empty() ? std::string{" (no clip / size getters)"} : names);
    }

    // Values of this node's bool / float / size getters with clip-like names.
    static std::string clip_values(REManagedObject* o, sdk::RETypeDefinition* td) {
        std::string out{};
        for (auto t = td; t != nullptr; t = t->get_parent_type()) {
            const auto tfn = t->get_full_name();
            if (tfn == "System.Object" || tfn == "via.Component") {
                break;
            }
            for (auto& m : t->get_methods()) {
                const char* mn = m.get_name();
                if (mn == nullptr || m.get_num_params() != 0) {
                    continue;
                }
                const std::string_view v{mn};
                if (v.rfind("get_", 0) != 0 || !clip_like(v)) {
                    continue;
                }
                auto rt = m.get_return_type();
                const auto rn = rt != nullptr ? rt->get_full_name() : std::string{};
                if (rn != "System.Boolean" && rn != "System.Single" && rn != "via.Size" && rn != "System.Int32" && rn != "System.UInt32" &&
                    rn != "via.vec2" && rn != "via.vec4" && rn.rfind("via.gui.", 0) != 0) {
                    continue;
                }
                if (rn.rfind("via.gui.", 0) == 0 && rt != nullptr && !rt->is_value_type()) {
                    continue; // object references: skip
                }
                std::vector<void*> none{};
                const auto r = m.invoke(o, std::span<void*>(none));
                if (r.exception_thrown) {
                    continue;
                }
                const float* f = (const float*)r.bytes.data();
                if (rn == "System.Boolean") out += fmt::format(" {}={}", v.substr(4), r.byte != 0 ? 1 : 0);
                else if (rn == "System.Single") out += fmt::format(" {}={:.1f}", v.substr(4), r.f);
                else if (rn == "via.Size" || rn == "via.vec2") out += fmt::format(" {}=({:.1f},{:.1f})", v.substr(4), f[0], f[1]);
                else if (rn == "via.vec4") out += fmt::format(" {}=({:.1f},{:.1f},{:.1f},{:.1f})", v.substr(4), f[0], f[1], f[2], f[3]);
                else out += fmt::format(" {}={}", v.substr(4), r.dword);
            }
        }
        return out;
    }

    static void dump_node(REManagedObject* o, int depth, int& count, sdk::VMContext* ctx) {
        for (auto c = o; c != nullptr && count < 400; ) {
            ++count;
            auto td = utility::re_managed_object::get_type_definition(c);
            list_type(td);
            auto nm = sdk::call_object_func<SystemString*>(c, "get_Name", ctx, c);
            const auto name = nm != nullptr ? utility::re_string::get_string(nm) : std::string{};
            const bool vis = sdk::call_object_func<bool>(c, "get_Visible", ctx, c);
            const auto pos = sdk::call_object_func_easy<Vector4f>(c, "get_Position");
            const auto sc = sdk::call_object_func_easy<Vector4f>(c, "get_Scale");
            spdlog::info("[PragmataVR] Layout {:>{}}{} [{}] vis={} pos=({:.1f},{:.1f}) scale=({:.2f},{:.2f}){}", "", depth * 2, name,
                         td != nullptr ? td->get_full_name() : std::string{"?"}, vis ? 1 : 0, pos.x, pos.y, sc.x, sc.y, clip_values(c, td));
            if (depth < 12) {
                auto child = sdk::call_object_func<REManagedObject*>(c, "get_Child", ctx, c);
                if (child != nullptr) {
                    dump_node(child, depth + 1, count, ctx);
                }
            }
            if (depth == 0) {
                break; // the view itself has no siblings to list
            }
            c = sdk::call_object_func<REManagedObject*>(c, "get_Next", ctx, c);
        }
    }

    // Hide one piece while `hide` is on (only if it is visible); show it again once `hide` is off, if this fix hid it.
    static void set_hidden(REManagedObject* owner, const Target& tg, std::wstring_view path, const char* want_type, bool hide,
                           sdk::VMContext* ctx) {
        const auto key = tg.hash ^ utility::hash(utility::narrow(path));
        auto& ours = s_hidden_by_us[key];
        if (!hide && !ours) {
            return;
        }

        auto node = get_object(owner, path, ctx);
        if (node == nullptr || (want_type != nullptr && type_name(node) != want_type)) {
            log_once(key, fmt::format("{}: {} not found, skipped", tg.name, utility::narrow(path)));
            return;
        }

        if (hide) {
            if (sdk::call_object_func<bool>(node, "get_Visible", ctx, node)) {
                sdk::call_object_func<void*>(node, "set_Visible", ctx, node, false);
                ours = true;
                log_once(key ^ 0x9e3779b97f4a7c15ull, fmt::format("{}: {} hidden", tg.name, utility::narrow(path)));
            }
        } else {
            sdk::call_object_func<void*>(node, "set_Visible", ctx, node, true);
            ours = false;
        }
    }

    // zz_pragmatavr_hide_ghost.lua: the offset "aberration" layer, at the first path that exists.
    static void ghost(REManagedObject* gui, const Target& tg, bool hide, sdk::VMContext* ctx) {
        static const std::array<const wchar_t*, 3> paths{L"main/layout/offset/under", L"main/layout/under", L"main/under"};
        const auto key = tg.hash ^ 0x6a09e667f3bcc909ull;
        auto& ours = s_hidden_by_us[key];
        if (!hide && !ours) {
            return;
        }

        REManagedObject* layer = nullptr;
        const wchar_t* used = nullptr;
        for (const auto* p : paths) {
            layer = get_object(gui, p, ctx);
            if (layer != nullptr) {
                used = p;
                break;
            }
        }
        log_once(key, layer != nullptr ? fmt::format("{}: ghost layer at {}", tg.name, utility::narrow(used))
                                       : fmt::format("{}: no ghost layer at the known paths", tg.name));
        if (layer == nullptr) {
            return;
        }

        if (hide) {
            if (sdk::call_object_func<bool>(layer, "get_Visible", ctx, layer)) {
                sdk::call_object_func<void*>(layer, "set_Visible", ctx, layer, false);
            }
            ours = true;
        } else {
            sdk::call_object_func<void*>(layer, "set_Visible", ctx, layer, true);
            ours = false;
        }
    }

    // zzzzz_pragmata_label_probe.lua (V13): Overlay off; title panel alpha 1 when the title text is its only visible child.
    static void label(REManagedObject* view, const Target& tg, sdk::VMContext* ctx) {
        if (sdk::call_object_func<bool>(view, "get_Overlay", ctx, view)) {
            sdk::call_object_func<void*>(view, "set_Overlay", ctx, view, false);
        }

        auto panel = get_object(view, L"main/layout/type/item", ctx);
        if (panel == nullptr || type_name(panel) != "via.gui.Panel") {
            log_once("ui3010 missing"_fnv, "ui3010Gui: title panel missing, no brightness edit");
            return;
        }

        bool found = false;
        auto child = sdk::call_object_func<REManagedObject*>(panel, "get_Child", ctx, panel);
        for (int count = 0; child != nullptr; ++count) {
            if (count >= 32) {
                return;
            }
            if (sdk::call_object_func<bool>(child, "get_Visible", ctx, child)) {
                auto cname = sdk::call_object_func<SystemString*>(child, "get_Name", ctx, child);
                const auto cn = cname != nullptr ? utility::re_string::get_string(cname) : std::string{};
                if (cn != "name" || type_name(child) != "via.gui.Text") {
                    return;
                }
                found = true;
            }
            child = sdk::call_object_func<REManagedObject*>(child, "get_Next", ctx, child);
        }
        if (!found) {
            return;
        }

        const auto c = sdk::call_object_func_easy<Vector4f>(panel, "get_ColorScale");
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w) || c.w >= 1.0f) {
            return;
        }
        Vector4f brighter{c.x, c.y, c.z, 1.0f};
        sdk::call_object_func<void*>(panel, "set_ColorScale", ctx, panel, &brighter);
        log_once("ui3010 applied"_fnv, fmt::format("{}: title alpha set to 1", tg.name));
    }
};
