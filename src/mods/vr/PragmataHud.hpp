#pragma once

// PragmataVR: tuning and in-headset menu for Pragmata's 3D HUD panels.
//
// Pragmata draws its combat HUD (hacking panel, health, stamina, weapon wheel, tutorials) as
// World-mode GUI views seen through the game's GUI camera. REFramework feeds that camera the
// headset projection (see VR::hijack_camera), which brings the panels into view, but at the game's
// flat-screen layout scale.
//
// Build 11: each HUD element is tuned on its own (hacking circle, health, stamina, weapon wheel, tutorial box):
// size, x/y position and a per-eye gap, applied to each panel's GUI view just before it draws and restored right
// after (VR::on_pre_gui_draw_element / on_gui_draw_element). Size grows an element around its own center (found from
// the panel's GUI tree), so a bigger element stays where it is instead of sliding off toward the edge of the view.
// The hacking circle keeps the Build 3-6 behaviour it was tuned with, and is locked at Steve's values by default.
//
// Values live in reframework/data/pragmatavr_hud.ini. The VR mod menu edits them in the headset.
//   Gamepad (Build 10): click both thumbsticks = open/close. D-pad or left stick only navigates (Up/Down rows,
//            Left/Right to a value's INV box and between columns). A = edit the highlighted value (then Left/Right
//            change it, RB held = big steps, A or B = done), flip the highlighted INV box, toggle or run an action.
//            B = stop editing, or close. The game does not see the pad while the menu is open
//            (if it reads the pad through XInput, see pragmata_xinput_get_state in VR.cpp).
//   Keyboard: F1 open/close, arrows, Shift = big steps, Enter = A, Backspace = B.
// Pause = snapshot of both eye images. No other hotkeys.
//
// Menu drawing (Build 9): VR::on_frame draws the menu as one ImGui window (ImGui frames render into
// REFramework's IMGUI render target while the headset is active). Under OpenXR, D3D12Component::on_frame
// copies that window's rectangle into a dedicated swapchain, and OpenXR::end_frame submits it as a quad
// composition layer fixed in the room (stage space). The headset's compositor draws the quad with full
// perspective from each eye's real pose, so the menu cannot bend, tilt or follow the head. Its physical
// size comes from the window's pixel size, so it always fits every row. OpenVR keeps the Build 8 path
// (menu pasted into each eye image).
//
// Included from VR.cpp (after its includes) and from vr/D3D12Component.cpp.

#include <Windows.h>
#include <mmsystem.h>
#include <Xinput.h>

#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace pragmatavr {

// Build 61: FNV-1a 64 for GUI names (the subtitle GUI is chosen by name in the ini).
inline constexpr uint64_t name_hash(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (const char c : s) {
        h ^= (uint8_t)c;
        h *= 1099511628211ull;
    }
    return h;
}

enum class EyeMode : int {
    Current = 0,
    Swapped = 1,
    Left = 2,
    Right = 3,
};

enum Group : int {
    GroupHack = 0,
    GroupHealth = 1,
    GroupStamina = 2,
    GroupWeapons = 3,
    GroupTutorial = 4,
    GroupItems = 5, // Build 47: item pickups (ui2120Gui, top right on a monitor: the cubes collected, etc.)
    GroupObjective = 6, // Build 53: objective banner (ui2110Gui, top middle on a monitor: "Restore the power to the lunar base")
    GroupSubtitles = 7, // v0.9.2 Build 58: subtitles. Build 61: ui2010Gui again (its layout has the subtitle / name / caption
                        // panels; Build 60's ui0400Gui is the Skip / Pause / Close bar), name set by subtitle_gui in the ini
    GroupReticle = 8,   // Build 60: gun reticle (ui2001Gui, ui3300Gui). Build 61: on its own view-space layer (no AFW)
    GroupCount = 9,
};

enum Field : int {
    FieldSize = 0,
    FieldX = 1,
    FieldY = 2,
    FieldGap = 3,
    FieldCount = 4,
};

inline constexpr std::array<const char*, GroupCount> GROUP_KEYS{"hack", "health", "stamina", "weapons", "tutorial", "items", "objective", "subtitles", "reticle"};
inline constexpr std::array<const char*, GroupCount> GROUP_LABELS{"Hacking circle", "Health", "Stamina", "Weapon wheel", "Tutorial box", "Item pickups", "Objective", "Subtitles", "Reticle"};
inline constexpr std::array<const char*, GroupCount> GROUP_TITLES{"HACKING CIRCLE", "HEALTH", "STAMINA", "WEAPON WHEEL", "TUTORIAL BOX", "ITEM PICKUPS", "OBJECTIVE", "SUBTITLES", "RETICLE"};

// The ini format this build writes. Older files (Build 10 and before) had the hacking circle reset to neutral by the
// Build 8 fresh ini, so their hack.* values are ignored and the locked values below are used instead.
inline constexpr int INI_VERSION = 15;
inline constexpr int INI_VERSION_CLICK_SCALE = 15; // Build 40: click volume x5 (100% = the old 20%)
// Build 22: FLAT Up / down became positive = down. Files written as v13 have their flat_y values negated on load.
inline constexpr int INI_VERSION_FLAT_Y_DOWN = 14;
// hud_mode (Build 21) replaces the flat_test and hud_panel lines; those are ignored from any file.
// hud_fixed and hack_locked lines are ignored in files older than this (Build 11 wrote hud_fixed=0 as its default).
inline constexpr int INI_VERSION_FIXED_HUD = 12;
// hack.* lines are ignored in files older than this (Build 10 and before had them reset by Build 8).
inline constexpr int INI_VERSION_HACK = 11;

// Steve's hacking circle (Dream Air, per-panel layout, 2026-10-23). D:\Projects\PragmataVR\HACKING_CIRCLE_LOCKED.txt
inline constexpr float HACK_LOCKED_SIZE = 0.9322f;
inline constexpr float HACK_LOCKED_X = -275.0f;
inline constexpr float HACK_LOCKED_Y = -75.0f;
inline constexpr float HACK_LOCKED_GAP = 12.0f;
inline constexpr std::array<const char*, FieldCount> FIELD_KEYS{"size", "x", "y", "gap"};

// Build 22: FLAT mode grows each element around a fixed point (canvas pixels, 1920 x 1080, y down): the middle of the
// element in the game's own layout, taken from the Build 21 GUI tree logs and snapshots. Build 21 recomputed the
// center from the visible pieces every 16 frames; health and the tutorial box have only a few such pieces, so the
// center jumped as pieces showed and hid (health bounced between a few spots), and sat near the screen edge (shrinking
// pulled the element off the edge of the view). Both halves of an element (3D panel and companion) use the same point.
inline constexpr std::array<std::array<float, 2>, GroupCount> FLAT_PIVOTS{{
    {1560.0f, 445.0f}, // hacking circle
    {178.0f, 964.0f},  // health
    {960.0f, 1001.0f}, // stamina
    {1612.0f, 877.0f}, // weapon wheel
    {305.0f, 300.0f},  // tutorial box
    {1640.0f, 200.0f}, // Build 47: item pickups (estimate, top right; the content extent log measures it)
    {960.0f, 110.0f},  // Build 53: objective banner (estimate, top middle, from the game's Options preview)
    {960.0f, 930.0f},  // Build 58: subtitles: the ui2010Gui layout anchor (960, 930), bottom-anchored like its box (Build 62)
    {960.0f, 540.0f},  // Build 60: reticle (canvas centre; drawn at full size, its layer is sized by Size)
}};

// Build 25: each element's area in the game's own layout (canvas pixels, 1920 x 1080, x0 y0 x1 y1), with a margin. From
// the Build 21 GUI tree logs and the Build 17-18 UI buffer measurements. ANCHORED moves these areas (after the FLAT size
// and position) from the eye images to the room-fixed panel. The hacking circle is not anchored.
inline constexpr std::array<std::array<float, 4>, GroupCount> FLAT_BOXES{{
    {0.0f, 0.0f, 0.0f, 0.0f},          // hacking circle (stays in the eyes)
    {0.0f, 820.0f, 560.0f, 1080.0f},   // health (Build 58: top 900 -> 820, right 400 -> 560: with more max health and in
                                       // Overdrive the numbers reached about 40 px above and the bar about 85 px past the old
                                       // area, measured in the 2026-10-31 snapshots)
                                       // Older: health (Build 30: left edge 70 -> 0, the bar's left end showed in the eyes. Build 54:
                                       // bottom 1030 -> 1080, the status icons under the bar were cut off)
    {720.0f, 950.0f, 1200.0f, 1020.0f}, // stamina (Build 34: bottom 1050 -> 1020, measured)
    {1292.0f, 640.0f, 1850.0f, 1080.0f}, // weapon wheel (Build 63: left 1400 -> 1292, the weapon modifier icons left of the
                                         // wheel were cut, snapshot 1793560717; the widest that still fits beside item pickups)
                                         // Older: weapon wheel (Build 57: the full-size wheel is around its pivot 1612, 877, about
                                         // 340 x 340 with its label; was 1160,500-1575,890, measured on the shrunk wheel)
                                        // Old notes: (Build 28: the wheel reaches well above its companion's box. Build 34:
                                        // right 1920 -> 1575, bottom 1080 -> 890, left 1180 -> 1160, measured from the
                                        // Build 31 captures; the old box was about 3x the wheel and took in other HUD)
    {60.0f, 200.0f, 590.0f, 450.0f},   // tutorial box (Build 32: right 620 -> 590, bottom 480 -> 400; Build 34: bottom 450, a
                                       // 4-line tutorial reached the 400 edge; room for about 2 more lines)
    {1100.0f, 0.0f, 1920.0f, 720.0f},  // Build 47: item pickups. Build 63: right 1940 -> 1920 (room for the weapon wheel). Build 58: bottom 600 -> 720, the REM Replicator beta panel
                                       // has more rows than alpha and ran about 75 px past the old bottom. Build 57: left 1000 -> 1100 (the replicator panel starts at
                                       // about 1104), so the area fits between the tutorial box and the weapon wheel. Build 54: 1300 -> 1000 left, 420 -> 600 bottom, 1920 -> 1940
                                       // right: the hub panel ("Y" stations) and the resource counter, measured in snapshots
    {480.0f, 0.0f, 1440.0f, 360.0f},   // Build 53: objective banner (icon, objective line). Build 56: bottom 260 -> 360 for the
                                       // second and third objective lines (secondaryObjective +102, tertiaryObjective +148)
    {570.0f, 740.0f, 1350.0f, 1030.0f}, // Build 62: subtitles. ui2010Gui is laid out against the BOTTOM of the eye image (not
                                        // centred like the elements switched to Screen view): in snapshot 1793546511 its name
                                        // and line bars sat at 904 and 946 in canvas px measured from the image bottom (layout
                                        // 850 / 893 + 54). These y values are in that bottom-anchored frame (anchor_dy converts),
                                        // covering two speakers with names (773) down to the second line (1003). The area is
                                        // cut at the subtitles' own place in the band below the game view (no capture shift).
    {780.0f, 360.0f, 1140.0f, 720.0f}, // Build 61: reticle, drawn flat it sits around the canvas centre (960, 540); 360 x 360
                                       // also holds the bigger gun reticles. Shown on its own view-space layer.
}};

// Build 56: item pickups away from the hub: only the resource counter and the pickup notices (the replicator panel,
// ui2093, is not shown), so Fixed mode cuts a much smaller area out of the top middle of the eye images.
inline constexpr std::array<float, 4> ITEMS_BOX_FIELD{1150.0f, 0.0f, 1940.0f, 380.0f};
// Build 56: set on the main thread (PragmataFixes) while the item pickups' replicator panel is shown.
// Build 61: false by default (the field area), so a broken check cannot leave the big area in use everywhere.
inline std::atomic<bool> g_items_hub{false};
// Build 61: the subtitle GUI's name hash (ini subtitle_gui, default ui2010Gui), read by group_for on the render threads.
inline std::atomic<uint64_t> g_subtitle_hash{0}; // Build 63: 0 until the ini says subtitles_new=1
// Build 58: set on the main thread (PragmataFixes) while ui0400Gui (Build 60) shows subtitle text. It is drawn all the time,
// so its (large, low in the view) area is only cut out of the eyes while a line is actually on screen.
inline std::atomic<bool> g_subtitles_shown{false}; // Build 61: off until the check sees a line
// Build 62: the subtitles give way (not cut, lifted clear of the objective's capture spot) while the objective banner or a
// window / full-screen menu is drawn; set each frame by anchored_boxes_px, read by the draw callback.
inline std::atomic<bool> g_subtitle_yield{false};
// Build 62: last time (steady clock ms) a tutorial / info window (ui2201Gui) was drawn.
inline std::atomic<int64_t> g_window_drawn_ms{0};

// Build 62: where the game aims the gun reticle. app.GuiBehaviorV2.applyPostureData places ui2001Gui at the aim point
// projected with the game camera (one eye, HMD included). The marker fix hook stores the canvas position the reticle is
// drawn at and its direction (tangents in the eye's centred view, from the projection matrix), so its layer is shown in
// that direction (Build 61 assumed the canvas centre at the HUD panel's scale, which put it off the shot).
struct ReticleAim {
    std::atomic<float> rx{960.0f}, ry{540.0f}; // canvas px where the GUI draws it (after the marker fix)
    std::atomic<float> tx{0.0f}, ty{0.0f};     // tan of the direction, x right, y up
    std::atomic<float> m00{1.0f}, m11{1.0f};   // projection scale (ndc per tan)
    std::atomic<int64_t> ms{0};                // steady clock ms of the last update
};
inline ReticleAim g_reticle_aim{};

// Build 57: while a snapshot is taken, the GUI draw callbacks collect the names of every GUI drawn (steady clock ms;
// PragmataFixes logs them), to identify effects such as the low health screen edges.
inline std::atomic<int64_t> g_gui_list_until_ms{0};

inline std::atomic<int> g_items_area_mode_cache{0}; // Build 61: 0 auto, 1 always the full area, 2 always the field area (set from the ini)
inline std::array<float, 4> box_for(int g) {
    if (g == GroupItems) {
        const int m = g_items_area_mode_cache.load(std::memory_order_relaxed);
        if (m == 2 || (m == 0 && !g_items_hub.load(std::memory_order_relaxed))) {
            return ITEMS_BOX_FIELD;
        }
    }
    return FLAT_BOXES[g];
}

// Build 21: HUD modes. 3D = the Builds 11-15 per-element 3D placement. FLAT = the game's own flat HUD, as on a monitor,
// in both eyes (follows the head), with per-element size, position and eye gap. PANEL = the Build 17-20 room-fixed panel.
enum HudMode : int {
    HudMode3D = 0,
    HudModeFlat = 1,
    HudModeAnchored = 2, // Build 25 (was PANEL): FLAT tuning, health / stamina / weapons / tutorial on a room-fixed panel
};

struct GroupTune {
    float size{1.0f}; // multiplies the panel's own scale
    float x{0.0f};    // layout units (1920 x 1080 basis)
    float y{0.0f};    // layout units
    float gap{0.0f};  // per-eye horizontal shift: left eye +gap, right eye -gap
    std::array<bool, FieldCount> invert{};
};

struct HudTune {
    bool enabled{true};
    EyeMode eye_mode{EyeMode::Current};
    float zoom{1.0f};
    float depth_shift{0.0f};
    float y_sign{1.0f};
    bool log_elements{true};
    bool hack_locked{true}; // Build 25: kept for old ini files; the locks are now per element (locked[])
    // Build 25: per-element lock. While locked, the menu cannot change that element's values in any HUD mode.
    std::array<bool, GroupCount> locked{true, true, true, true, true, true, true, false, false}; // Build 58 / 60: subtitles and reticle start unlocked // v0.9.0: all locked at Steve's Quest 3 tuning
    // Build 25: mod menu sections that are collapsed (bit per MenuSection). DEVELOPER starts collapsed.
    // Build 58: a fresh install starts with every HUD element section collapsed (a less overwhelming first look); VIEW,
    // PERFORMANCE and the plain headers stay open. Existing ini files keep their own collapsed value.
    uint32_t collapsed{(1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8) | (1u << 9) |
                       (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13)};
    // Build 25: ANCHORED test aid, outlines of the areas moved to the room-fixed panel.
    bool show_boxes{false};
    // Build 55: draw in NATIVE while a full-screen menu (ui51xx: Firmware Updater, Stamp Club, Bot Database, outfits,
    // terminals) is open, where AFW leaves trails; back to AFW when it closes. Only applies when render mode is AFW.
    bool menu_native{true};
    // Build 57: hide the red low health effect at the screen edges (ui2030Gui), made for a flat monitor.
    bool hide_low_health{true};
    // Build 59: sharp textures in VR. The game's own render config is asked for the highest minimum streaming texture
    // resolution and no load level bias (PragmataTexDiag.hpp), so textures do not stay at low detail in the headset.
    bool sharp_textures{true};
    // Build 60: developer switch for the gun reticle: true = NEW (Build 61: on its own view-space layer, RETICLE Size and
    // Distance), false = ORIGINAL (drawn exactly as in v0.9.1).
    bool reticle_new{false}; // Build 63: ORIGINAL by default (experimental)
    // Build 61: the subtitle GUI (ini subtitle_gui) and whether it is pinned to its monitor anchor while the HUD panels
    // are on (the game otherwise places walk-and-talk subtitles next to the speaker, outside their capture area).
    std::string subtitle_gui{"ui2010Gui"};
    bool subtitle_pin{true};
    // Build 62: developer switch, like Reticle: true = NEW (subtitles on their own panel), false = ORIGINAL (as v0.9.1).
    bool subtitles_new{false}; // Build 63: ORIGINAL by default (experimental)
    // Build 61: item pickups area: 0 = automatic (the big area only while a replicator list is open), 1 = always the big
    // area (Builds 58-60), 2 = always the field area.
    int items_area{0};
    bool hud_fixed{true};    // HUD fixed in the room (Build 12, Steve's standing preference). Off = follows the head
    bool lock_invert{false}; // flips the fixed-HUD correction if it turns out mirrored on some runtime
    bool hide_ghost{true};   // hide Pragmata's offset "aberration" layers (zz_pragmatavr_hide_ghost.lua reads this)
    // Build 22: FLAT mode hides the frosted glass (via.gui.BlurFilter) behind the HUD elements. The blur keeps the
    // element's original size and place when the element is resized and moved, which showed as a stray panel.
    bool hide_blur{true};
    // Build 23 test: FLAT mode sets MaskMode Disable on every piece of the five HUD elements for each draw, to see
    // whether GUI masks that do not follow the resize are what cuts off the tutorial box and weapon wheel.
    bool masks_off{false}; // Build 24: no longer used (masks were not the cause); the ini line is ignored
    // Build 24: FLAT mode draws the 3D panels of health, stamina, weapon wheel and tutorial as Screen views (like their
    // companions), so both halves share one coordinate space. Off = World views with the flat projection (Builds 21-23).
    bool flat_screen{true};
    bool scale_about_center{true}; // Size grows each element around its own center (off = around the panel origin)
    // Build 17: HUD panel. 1 = health, stamina, weapon wheel and tutorial are drawn flat (Screen mode, as on a monitor)
    // into the engine's UI buffer, which is copied each frame onto a room-fixed OpenXR quad (screen.* place and size it).
    // 0 = the Builds 11-15 3D placement.
    int hud_panel{0};
    bool head_locked{false}; // Build 56: Follows Head: the element quads are placed in view space (derived, never stored)
    // Build 17 test: the engine's overlay layer draw. 0 = REFramework's default, 1 = always skipped, 2 = always drawn.
    int engine_overlay{0};
    // Build 19 test: draws that write the eye image and the UI buffer together get a throwaway image instead of the eye
    // image, so the UI reaches only the UI buffer (and the HUD panel). Off = the engine's normal drawing.
    bool glued_hud_removed{true}; // Build 20: the element areas are cleared from the UI buffer after the HUD panel copy
    // Build 21: HUD mode (HudMode). flat_test and hud_panel are derived from it (apply_hud_mode), never stored.
    int hud_mode{HudModeAnchored}; // Build 39: Fixed (room-fixed) by default, Steve's standing preference
    bool flat_test{true}; // FLAT mode: the game's own flat projection, companions left flat, flat_groups tuning
    int companion_mode{0}; // Build 15: companion layers. 0 = with element (CompanionWithElement), 1 = REFramework, 2 = hidden

    // HUD layout. 0 = per-element (Builds 2-6, and Build 11 onward). 1 = "game screen" (Builds 7-10): the whole 3D HUD
    // as one virtual screen. Steve found the single screen no better and it broke the hacking circle, so Build 11
    // always uses 0 (the ini value is ignored; the game-screen code is kept for reference).
    int layout_mode{0};
    // width (degrees), distance (m), left/right (deg), up/down (deg). v0.9.0: Quest 3 tuning. Build 61: [4] = reticle distance (m),
    // 1.0 = where REFramework's own 2D UI sheet showed it (VR_2DUIDistance).
    std::array<float, 5> screen{100.0f, 2.5f, 0.0f, 0.0f, 3.0f}; // Build 62: reticle 3.0 m (was 1.0)
    std::array<bool, 5> screen_invert{};

    // Build 35: PERFORMANCE section. These drive REFramework's own settings (VR::on_present applies them), so the
    // values in re2_fw_config.txt for them no longer matter.
    bool render_afw{true};      // v0.9.0: AFW is the default for new installs (holds the refresh rate on a Quest 3); NATIVE one toggle away
    bool sharpening{true};      // PureDark's sharpening pass (full resolution, every eye)
    bool desktop_mirror{false}; // copy the headset image to the game window on the monitor (costs GPU time)
    bool perf_log{true};        // performance summary in re2_framework_log.txt every 10 s

    // Build 38: in Follows Head and Fixed modes the GUI views still drawn in the world (hacking circle, door hack, reticle,
    // enemy markers) use the headset's own eye projection, like the world itself, so they land in the right place and
    // shape on any headset. false = the game's flat (monitor) projection, as in Builds 21-37.
    bool world_headset{true};
    // Build 38: the hacking circle's values for the headset projection (around FLAT_PIVOTS like FLAT). Neutral start:
    // with the right projection the game's own placement should already line up; no eye gap is needed.
    GroupTune hack_headset{};
    // Build 39: Fixed mode draws the four panel elements in the corners of the UI buffer (outside the lens view of
    // both eyes) and cuts them out there, so nothing the game draws in the world (reticle, enemy health bars, hacking
    // circle) can be caught in their areas. false = cut them out at their own place in the game layout (Build 38).
    bool fixed_capture_corners{true};
    // Build 40: enemy health bar shift (GROUP_ENEMY). Build 41: Steve's Quest 3 values (x 200, y 170), and the damage
    // numbers (ui3201Gui) get their own pair, starting at the same values.
    // Build 50: the markers are now placed by the world-to-screen correction (PragmataMarkerFix.hpp); these are only a
    // fine-tune on top, in canvas px (ini marker_trim.*; the old enemy_bar.* lines are ignored).
    // Build 53: Steve's Quest 3 (Virtual Desktop) tuning is the default for new installs: health bars x 0, y -85; damage
    // numbers x 25, y -100 (beside and above the bar).
    std::array<float, 4> enemy_bar{0.0f, -85.0f, 25.0f, -100.0f};
    std::array<bool, 4> enemy_bar_invert{};
    // Build 41: hide the dark backdrop pieces of the door / lock hacking HUD (ui3500Gui, ui3510Gui) while it draws.
    bool hide_hack_backdrop{true};
    // Build 46: switches for the HUD fixes (PragmataFixes.hpp), now applied on the main thread. ini only.
    bool fix_brackets{true};  // ui3203Gui / ui2160Gui hacking target brackets not drawn
    bool fix_label{true};     // ui3010Gui location label brightness
    bool fix_door_vfx{true};  // ui3500Gui / ui3510Gui VFXtex effect textures hidden
    bool fix_gauge_vfx{true}; // ui2130Gui hackingGauge_VFX effect texture hidden
    bool marker_fix{true};      // Build 50: correct enemy bars / damage numbers / world markers for the headset projection
    float marker_vscale{1.6f};  // Build 50: vertical factor of that correction (1.6 matched the Quest 3 by eye)
    bool screen_probe{false}; // Build 48: diagnostic, find the game's world-to-screen function for the enemy markers (ini only)
    bool enemy_locked{true}; // Build 42: lock for the ENEMY HEALTH BARS fine-tune (fields EnemyX / EnemyY)
    bool damage_locked{true}; // Build 51: lock for the DAMAGE NUMBERS fine-tune (fields DamageX / DamageY)
    // Build 43: the damage numbers shift is off by default while a crash is tested (two Quest 3 crashes came a few
    // seconds after it first applied; damage numbers are many short-lived pieces the game spawns and removes).
    bool shift_damage{true}; // Build 45: on again, now through the projection (no game object writes)

    // Mod menu placement: a panel fixed in the room where it was opened.
    // distance (m), text size (1 = body text MENU_TEXT_DEG degrees tall, at any distance), up/down (deg),
    // click volume (0..1 of full scale)
    std::array<float, 4> menu{2.5f, 1.3f, -5.0f, 0.10f}; // Build 39: Steve's values as the default. v0.9.0: click 10%
    std::array<bool, 4> menu_invert{};
    std::array<GroupTune, GroupCount> groups{};
    // Build 21: FLAT mode values, kept apart from the 3D values (the hacking circle lock applies to 3D only).
    // x / y / gap in canvas pixels (1920 x 1080); y positive = up.
    std::array<GroupTune, GroupCount> flat_groups{};

    void apply_hud_mode() {
        // Build 28: the old 3D mode is retired (FLAT or ANCHORED only), and the developer settings that could break
        // FLAT or ANCHORED are fixed at their working values (their menu rows are gone).
        hud_mode = hud_mode == HudModeAnchored ? HudModeAnchored : HudModeFlat;
        hud_fixed = true;
        glued_hud_removed = true;
        flat_screen = true;
        hide_blur = true;
        hide_ghost = true;
        engine_overlay = 0;
        flat_test = hud_mode == HudModeFlat || hud_mode == HudModeAnchored; // ANCHORED draws the HUD exactly like FLAT
        // Build 56: both modes cut the panel elements out of the game's HUD image at full size and show each on its own
        // OpenXR quad, sized by the element's Size: Fixed in the room, Follows Head attached to the view.
        hud_panel = 1;
        head_locked = hud_mode == HudModeFlat;
    }

    HudTune() {
        // The hacking circle starts locked at Steve's tuned values. Every other element starts neutral.
        auto& h = groups[GroupHack];
        h.size = HACK_LOCKED_SIZE;
        h.x = HACK_LOCKED_X;
        h.y = HACK_LOCKED_Y;
        h.gap = HACK_LOCKED_GAP;
        hack_locked = true;

        // FLAT mode starting layout: every element smaller and pulled in toward the middle of the view, so all of it
        // starts inside the headset's field of view (the game's own layout puts them at the screen edges).
        const auto set_flat = [&](int g, float size, float x, float y) {
            flat_groups[g].size = size;
            flat_groups[g].x = x;
            flat_groups[g].y = y;
        };
        // v0.9.0: Steve's Quest 3 (Virtual Desktop) tuning on the per-element panels (Build 57) is the default for new
        // installs, the most common headset.
        set_flat(GroupHack, 0.4005f, -400.0f, 100.0f);
        set_flat(GroupHealth, 0.6749f, 450.0f, -70.0f);
        set_flat(GroupStamina, 0.9236f, 0.0f, -90.0f);
        set_flat(GroupWeapons, 0.6490f, -330.0f, 20.0f);
        set_flat(GroupTutorial, 0.5769f, 320.0f, 40.0f);
        set_flat(GroupItems, 0.5066f, -330.0f, 190.0f);
        flat_groups[GroupHack].gap = 80.0f;
        flat_groups[GroupHealth].gap = 240.0f;
        flat_groups[GroupStamina].gap = 240.0f;
        flat_groups[GroupWeapons].gap = 200.0f;
        flat_groups[GroupTutorial].gap = 250.0f;
        flat_groups[GroupItems].gap = 200.0f;
        set_flat(GroupObjective, 0.5480f, 0.0f, 120.0f);
        // v0.9.0: the hacking circle with the headset projection (World HUD projection = Headset, the default).
        hack_headset.size = 0.7903f;
        hack_headset.x = -270.0f;
        hack_headset.y = 150.0f;
        hack_headset.gap = 0.0f;
        flat_groups[GroupObjective].gap = 200.0f;
        // Build 58: subtitles start at their own place, a little smaller, raised toward the middle of the view.
        // Build 63: the subtitles' panel is placed in the same centred frame as the other elements (headset independent).
        // Steve's latest Pimax tuning (size 0.6472, x 0, y -390 against the image bottom, which is 295 px below the centred
        // frame on the Dream Air) is the default: y -390 + 295 = -95.
        set_flat(GroupSubtitles, 0.6472f, 0.0f, -95.0f);
        apply_hud_mode();
    }
};

inline GroupTune default_group(int g) {
    return HudTune{}.groups[g];
}

inline GroupTune default_flat_group(int g) {
    return HudTune{}.flat_groups[g];
}

// The values the menu shows and edits for an element in the current HUD mode.
inline GroupTune& active_group(HudTune& t, int g) {
    if (g == GroupHack && t.flat_test && t.world_headset) return t.hack_headset; // Build 38
    return t.flat_test ? t.flat_groups[g] : t.groups[g];
}
inline const GroupTune& active_group(const HudTune& t, int g) {
    if (g == GroupHack && t.flat_test && t.world_headset) return t.hack_headset; // Build 38
    return t.flat_test ? t.flat_groups[g] : t.groups[g];
}

// The hacking circle lock protects its 3D values only.
inline bool group_locked(const HudTune& t, int g) {
    return g >= 0 && g < GroupCount && t.locked[g];
}

inline const char* hud_mode_name(int m) {
    // Build 34: shown names (Steve): FIXED = the HUD stays put in one place in front of you (internally "anchored"),
    // FOLLOWS HEAD = the HUD moves with your headset (internally "flat").
    return m == HudModeFlat ? "FOLLOWS HEAD" : (m == HudModeAnchored ? "FIXED" : "3D");
}

// Which element a GUI GameObject belongs to. ui2160Gui is no longer tuned: the fix pack lists it with the hacking
// target brackets (ui3203), which sit on enemies in the world and must not be moved or scaled with the HUD.
inline int group_for(std::string_view name) {
    // Build 33: the door hacking HUD (ui3510Gui, seen opening a door) and ui3500Gui (listed with it as the other hacking
    // HUD by the fix pack's hack effects script, not seen yet) share the enemy hacking circle's values.
    if (name == "ui3400Gui" || name == "ui3510Gui" || name == "ui3500Gui") return GroupHack;
    if (name == "ui2130Gui") return GroupHealth;
    if (name == "ui2140Gui") return GroupStamina;
    if (name == "ui2150Gui") return GroupWeapons;
    // Build 32: ui2201Gui (the large tutorial window, "Hacking Enemies" with video and A = Close) is no longer in the
    // tutorial group: FLAT shrank it with the tutorial box values and ANCHORED cut a piece of it onto the panel. It is a
    // Screen view and is now drawn like the game's other menus.
    if (name == "ui2100Gui" || name == "ui2002_00Gui") return GroupTutorial;
    if (name == "ui2120Gui") return GroupItems; // Build 47: item pickups (HUD Controls calls it "Inventory Items")
    if (name == "ui2110Gui") return GroupObjective; // Build 53: objective banner (HUD Controls calls it "Objective Message")
    // Build 61: the subtitle GUI is named in the ini (subtitle_gui, default ui2010Gui). Build 60's ui0400Gui is the
    // Skip / Pause / Close bar, drawn as before again.
    if (const auto sh = g_subtitle_hash.load(std::memory_order_relaxed); sh != 0 && name_hash(name) == sh) return GroupSubtitles;
    // Build 60: the gun reticle (both aiming snapshot series drew ui2001Gui; the second gun also ui3300Gui).
    if (name == "ui2001Gui" || name == "ui3300Gui") return GroupReticle;
    return -1;
}

// Build 15: each of these HUD elements is drawn by two GUI objects in the flat game: a World-mode 3D panel (the one
// tuned above) and a Screen-mode companion with the rest of its pieces (bars, numbers, text). REFramework's generic VR
// code turns Screen-mode views into its own floating sheet in front of the game camera, so the two halves of one
// element were placed by different systems, at different depths, and the companion followed the head. The
// companion is now drawn exactly like its partner (same view settings and placement, same size and position values).
inline int companion_group(std::string_view name, std::string_view& partner) {
    if (name == "ui2131Gui") { partner = "ui2130Gui"; return GroupHealth; }
    if (name == "ui2141Gui") { partner = "ui2140Gui"; return GroupStamina; }
    if (name == "ui2151Gui") { partner = "ui2150Gui"; return GroupWeapons; }
    if (name == "ui2101Gui") { partner = "ui2100Gui"; return GroupTutorial; }
    if (name == "ui2121Gui") { partner = "ui2120Gui"; return GroupItems; } // Build 47: if the game has this companion
    if (name == "ui2111Gui") { partner = "ui2110Gui"; return GroupObjective; } // Build 53: the fix pack keeps it in Screen view
    return -1;
}

enum CompanionMode : int {
    CompanionWithElement = 0, // drawn like its 3D partner (Build 15 default)
    CompanionReframework = 1, // REFramework's generic floating sheet (Builds 1-14)
    CompanionHidden = 2,      // not drawn (test)
};

// Build 26: set by D3D12Component (pragmata_trace::note_ui_buffer); VR's overlay layer hook reports each UI buffer the
// engine hands out through it.
inline std::atomic<void (*)(void*)> g_note_ui_buffer{nullptr};

inline thread_local size_t g_current_gui_hash = 0;
inline thread_local std::string g_current_gui_name{};

// Menu click sounds. Build 9 and earlier used Beep(), which has no volume control and was too loud. The same two tones
// (1400 Hz 30 ms "ok", 220 Hz 160 ms "limit / blocked") are now made here as small WAV images and played with
// PlaySound, at the menu's Click volume (menu.click_volume, default 0.5 = half of full scale).
inline std::atomic<int> g_click_volume_pct{50};

inline const std::vector<uint8_t>& click_wav(bool ok, int volume_pct) {
    // One cached image per tone and volume step. Never freed, so an asynchronous PlaySound never reads freed memory.
    static std::mutex mtx{};
    static std::unordered_map<int, std::unique_ptr<std::vector<uint8_t>>> cache{};

    std::scoped_lock _{mtx};
    const int key = (ok ? 1000 : 0) + volume_pct;
    auto it = cache.find(key);
    if (it != cache.end()) {
        return *it->second;
    }

    constexpr uint32_t rate = 44100;
    const float freq = ok ? 1400.0f : 220.0f;
    const uint32_t samples = rate * (ok ? 30u : 160u) / 1000u;
    const uint32_t fade = rate * 3u / 1000u; // 3 ms fade in and out so the tone does not click
    const float amp = 32767.0f * (float)volume_pct / 100.0f;
    const uint32_t data_bytes = samples * 2u;

    auto wav = std::make_unique<std::vector<uint8_t>>();
    auto& w = *wav;
    w.reserve(44 + data_bytes);
    const auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back((uint8_t)(v >> (8 * i))); };
    const auto put16 = [&](uint16_t v) { w.push_back((uint8_t)v); w.push_back((uint8_t)(v >> 8)); };
    const auto tag = [&](const char* s) { w.insert(w.end(), s, s + 4); };

    tag("RIFF"); put32(36 + data_bytes); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16(1); put32(rate); put32(rate * 2); put16(2); put16(16);
    tag("data"); put32(data_bytes);

    for (uint32_t i = 0; i < samples; ++i) {
        float env = 1.0f;
        if (i < fade) env = (float)i / (float)fade;
        else if (samples - i < fade) env = (float)(samples - i) / (float)fade;
        const float s = std::sin(6.2831853f * freq * (float)i / (float)rate) * amp * env;
        put16((uint16_t)(int16_t)std::clamp(s, -32767.0f, 32767.0f));
    }

    const auto& ref = *wav;
    cache.emplace(key, std::move(wav));
    return ref;
}

inline void play_click(bool ok) {
    // Build 40: the old 20% level is now 100% (Steve: far too loud). Old settings are rescaled on load (INI_VERSION 15).
    const int vol = std::clamp((int)std::lround(g_click_volume_pct.load() * 0.2f), 0, 100);
    if (vol == 0) {
        return;
    }

    const auto& wav = click_wav(ok, vol);
    PlaySoundA((LPCSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// One row of the VR mod menu. The menu has two columns; rows are listed left column first, then right.
struct MenuRow {
    enum class Kind { Header, Value, Toggle, Action };
    Kind kind{Kind::Header};
    int group{-1};
    int field{-1};
    int action{0};
    int column{0};
    int band{0}; // 0 = mod menu settings (top), 1 = game HUD settings (below)
    std::string label{};
    std::string hint{}; // footer help line while this row is selected (headers: small tag after the title)
    int section{-1};    // Build 25: collapsible section (MenuSection) this row belongs to; headers with one can be selected
    int tab{0};         // Build 36: menu tab (MenuTab) the row is on; only the current tab is shown and navigated
};

// Build 36: menu tabs, switched with LT / RT (Page Up / Page Down on the keyboard).
enum MenuTab : int {
    TabGeneral = 0,
    TabHud = 1,
    TabAdvanced = 2,
    TabExperimental = 3, // Build 63: work in progress (subtitles, reticle, textures)
    TabMenu = 4,
    TabCount = 5,
};
inline constexpr std::array<const char*, TabCount> TAB_TITLES{"GENERAL", "HUD", "ADVANCED", "EXPERIMENTAL", "MOD MENU"};

// Build 25: collapsible sections of the GAME HUD band (bit numbers in HudTune::collapsed).
enum MenuSection : int {
    SectionView = 0,
    SectionDeveloper = 1,
    SectionElement0 = 2, // + Group
    SectionPerformance = 7, // Build 35 (after the five element sections, bits 2 to 6)
    SectionEnemy = 8,       // Build 42: ENEMY BARS AND DAMAGE
    SectionItems = 9,       // Build 47: ITEM PICKUPS (bit 7 was already taken by PERFORMANCE)
    SectionDamage = 10,     // Build 51: DAMAGE NUMBERS (bit 8 is now ENEMY HEALTH BARS)
    SectionObjective = 11,  // Build 53: OBJECTIVE
    SectionSubtitles = 12,  // Build 58: SUBTITLES
    SectionReticle = 13,    // Build 60: RETICLE
};

inline int section_for_group(int g) {
    if (g == GroupItems) return SectionItems;
    if (g == GroupObjective) return SectionObjective;
    if (g == GroupSubtitles) return SectionSubtitles;
    if (g == GroupReticle) return SectionReticle;
    return SectionElement0 + g;
}

inline bool section_collapsed(uint32_t collapsed, int section) {
    return section >= 0 && section < 32 && (collapsed & (1u << section)) != 0;
}

// A row that is not drawn because its section is collapsed (the section's own header always is).
inline bool row_hidden(uint32_t collapsed, const MenuRow& r) {
    return r.kind != MenuRow::Kind::Header && section_collapsed(collapsed, r.section);
}

// Pseudo-group for the HUD screen rows, and its fields.
inline constexpr int GROUP_SCREEN = 100;
enum ScreenField : int {
    ScreenWidth = 0,
    ScreenDistance = 1,
    ScreenYaw = 2,
    ScreenPitch = 3,
    ScreenReticleDist = 4, // Build 61
};
inline constexpr int SCREEN_FIELDS = 5;
inline constexpr std::array<const char*, SCREEN_FIELDS> SCREEN_KEYS{"width", "distance", "yaw", "pitch", "reticle_distance"};
inline constexpr std::array<float, SCREEN_FIELDS> SCREEN_MIN{20.0f, 0.5f, -60.0f, -45.0f, 0.3f};
inline constexpr std::array<float, SCREEN_FIELDS> SCREEN_MAX{140.0f, 10.0f, 60.0f, 45.0f, 20.0f};
inline constexpr std::array<float, SCREEN_FIELDS> SCREEN_STEP{2.0f, 0.1f, 1.0f, 1.0f, 0.1f};
inline constexpr std::array<float, SCREEN_FIELDS> SCREEN_STEP_BIG{10.0f, 0.5f, 5.0f, 5.0f, 0.5f};

// Build 40: enemy health bars (ui3200Gui, boss ui3210Gui). With the headset projection they land a fixed amount left of
// and above the enemy (Quest 3 test); these move them back, in canvas pixels (y positive = down). Headset projection only.
inline constexpr int GROUP_ENEMY = 102;
enum EnemyField : int {
    EnemyX = 0,
    EnemyY = 1,
    DamageX = 2, // Build 41: damage numbers (ui3201Gui)
    DamageY = 3,
    EnemyFieldCount = 4,
};
inline constexpr std::array<const char*, 4> ENEMY_KEYS{"x", "y", "damage_x", "damage_y"};

inline bool enemy_field_locked(const HudTune& t, int field) {
    return field >= DamageX ? t.damage_locked : t.enemy_locked;
}

inline constexpr float ENEMY_MAX = 1500.0f;
inline constexpr float ENEMY_STEP = 5.0f;
inline constexpr float ENEMY_STEP_BIG = 50.0f;

inline constexpr int GROUP_MENU = 101;
enum MenuField : int {
    MenuDistance = 0,
    MenuScale = 1,
    MenuPitch = 2,
    MenuClickVolume = 3,
};
// menu.width (Build 8, meters) is gone: the panel's size now follows its text. Old ini lines for it are ignored.
inline constexpr std::array<const char*, 4> MENU_KEYS{"distance", "scale", "pitch", "click_volume"};
inline constexpr std::array<float, 4> MENU_MIN{0.8f, 0.5f, -40.0f, 0.0f};
inline constexpr std::array<float, 4> MENU_MAX{8.0f, 2.0f, 40.0f, 1.0f};
inline constexpr std::array<float, 4> MENU_STEP{0.1f, 0.05f, 1.0f, 0.1f};
inline constexpr std::array<float, 4> MENU_STEP_BIG{0.5f, 0.25f, 5.0f, 0.25f};

// Menu text: body text is drawn MENU_FONT_PX pixels tall into the menu image, and shown MENU_TEXT_DEG degrees tall
// (times menu.scale) in the headset, whatever the distance. 0.7 degrees is comfortable reading size in VR.
inline constexpr float MENU_FONT_PX = 30.0f;
inline constexpr float MENU_TEXT_DEG = 0.7f;

enum MenuAction : int {
    ActionNone = 0,
    ActionToggleLayout,
    ActionResetScreen,
    ActionResetMenu,
    ActionToggleHudFixed,
    ActionRecenter,
    ActionToggleLockInvert,
    ActionToggleGhost,
    ActionToggleBlur,
    ActionToggleMasks,
    ActionToggleFlatScreen,
    ActionToggleBoxes,
    ActionToggleHackLock,
    ActionResetGroup,
    ActionSnapshot,
    ActionToggleScaleCenter,
    ActionToggleCompanion,
    ActionToggleFlatTest,
    ActionToggleHudPanel,
    ActionToggleEngineOverlay,
    ActionToggleGluedHud,
    ActionSnapshotSeries,
    ActionClose,
    ActionToggleRenderMode,    // Build 35
    ActionToggleSharpening,    // Build 35
    ActionToggleDesktopMirror, // Build 35
    ActionTogglePerfLog,       // Build 35
    ActionToggleWorldProj,     // Build 38
    ActionToggleCaptureCorners, // Build 39
    ActionRecenterView,         // Build 40
    ActionResetEnemy,           // Build 40
    ActionToggleHackBackdrop,   // Build 41
    ActionToggleEnemyLock,      // Build 42
    ActionToggleDamageLock,     // Build 51
    ActionResetDamage,          // Build 51
    ActionToggleShiftDamage,    // Build 43
    ActionToggleMenuNative,     // Build 55
    ActionToggleLowHealth,      // Build 57
    ActionToggleSharpTextures,  // Build 59
    ActionToggleReticleMode,    // Build 60
    ActionToggleSubtitleMode,   // Build 62
    ActionReloadTextures,       // Build 62 (developer test)
};

class HudTuning {
public:
    static HudTuning& get() {
        static HudTuning instance{};
        return instance;
    }

    std::shared_ptr<const HudTune> tune() {
        maybe_reload();
        std::scoped_lock _{m_mtx};
        return m_tune;
    }

    void note_element(size_t hash, const std::string& name) {
        std::scoped_lock _{m_mtx};

        if (m_logged.emplace(hash, true).second) {
            spdlog::info("[PragmataVR] GUI camera projection requested by: {}", hash == 0 ? std::string{"(outside a GUI element draw)"} : name);
        }
    }

    // Called once per presented frame. Edge-triggered, no key repeat, only while the game has focus.
    void poll_keys() {
        poll_snapshot_series();

        DWORD fg_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
        const bool focused = fg_pid == GetCurrentProcessId();

        const auto now = std::chrono::steady_clock::now();

        for (size_t i = 0; i < KEYS.size(); ++i) {
            const bool down = (GetAsyncKeyState(KEYS[i]) & 0x8000) != 0;
            const bool was_down = m_key_down[i];
            m_key_down[i] = down;

            if (!focused || !down || was_down) {
                continue;
            }

            if (now - m_last_press[i] < std::chrono::milliseconds(120)) {
                continue;
            }

            m_last_press[i] = now;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            handle_key(KEYS[i], shift);
        }
    }

    // Gamepad menu input. get_state points at the real XInputGetState (the hook trampoline when the
    // game's XInput is hooked), so the menu still sees the pad while the game is being fed zeros.
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    std::atomic<XInputGetStateFn> xinput_get_state{nullptr};

    // True while the game should not see the pad: menu open, and a short time after it closes so the
    // closing press does not leak into the game.
    bool should_block_pad() const {
        if (menu_open.load()) {
            return true;
        }
        return std::chrono::steady_clock::now() < m_block_pad_until.load();
    }

    // Build 58: how long both stick clicks must be held to open the menu. Overdrive (and maybe other game actions) uses
    // the same L3 + R3 press, so the game does not see the pair while it is held; a short press is replayed to it.
    static constexpr int CHORD_HOLD_MS = 600;
    static constexpr int CHORD_REPLAY_MS = 150;

    // Build 58: called from the XInputGetState hook (the game's thread) with the buttons the game is about to get.
    // While L3 and R3 are both held (and the menu is closed) the pair is hidden; after a short press that did not open
    // the menu, the pair is reported held for CHORD_REPLAY_MS so the game's own L3 + R3 action still happens.
    void filter_game_buttons(WORD& buttons) const {
        constexpr WORD sticks = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
        if (now_ms_steady() < m_chord_replay_until_ms.load(std::memory_order_relaxed)) {
            buttons |= sticks;
            return;
        }
        if ((buttons & sticks) == sticks) {
            buttons &= (WORD)~sticks;
        }
    }

    static int64_t now_ms_steady() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void poll_pad() {
        auto get_state = xinput_get_state.load();
        if (get_state == nullptr) {
            return;
        }

        XINPUT_STATE st{};
        bool have = false;
        for (DWORD i = 0; i < 4; ++i) {
            if (get_state(i, &st) == ERROR_SUCCESS) {
                have = true;
                break;
            }
        }

        WORD buttons = 0;
        float lx = 0.0f;
        float ly = 0.0f;
        if (have) {
            buttons = st.Gamepad.wButtons;
            lx = st.Gamepad.sThumbLX / 32767.0f;
            ly = st.Gamepad.sThumbLY / 32767.0f;
        }

        const WORD prev = m_pad_prev;
        m_pad_prev = buttons;
        const auto pressed = [&](WORD b) { return (buttons & b) != 0 && (prev & b) == 0; };

        // Left stick as a second D-pad: one step each time it is pushed past 60% and released.
        int stick_dir = 0; // 1 up, 2 down, 3 left, 4 right
        if (ly > 0.6f) stick_dir = 1;
        else if (ly < -0.6f) stick_dir = 2;
        else if (lx < -0.6f) stick_dir = 3;
        else if (lx > 0.6f) stick_dir = 4;
        const int prev_stick = m_stick_prev;
        if (std::fabs(lx) < 0.3f && std::fabs(ly) < 0.3f) {
            m_stick_prev = 0;
        } else if (stick_dir != 0) {
            m_stick_prev = stick_dir;
        }
        const bool stick_new = stick_dir != 0 && prev_stick == 0;

        const auto now = std::chrono::steady_clock::now();
        const bool both_sticks = (buttons & XINPUT_GAMEPAD_LEFT_THUMB) && (buttons & XINPUT_GAMEPAD_RIGHT_THUMB);
        const bool both_sticks_prev = (prev & XINPUT_GAMEPAD_LEFT_THUMB) && (prev & XINPUT_GAMEPAD_RIGHT_THUMB);

        // Build 58: open = hold both stick clicks for CHORD_HOLD_MS; close = a press of both (or B). A short press while
        // the menu is closed is replayed to the game (filter_game_buttons), so Overdrive still works.
        if (both_sticks && !both_sticks_prev) {
            m_chord_start = now;
            m_chord_fired = false;
            m_chord_menu_was_open = menu_open.load();
            if (m_chord_menu_was_open && now - m_last_chord > std::chrono::milliseconds(300)) {
                m_last_chord = now;
                m_chord_fired = true;
                handle_key(VK_F1, false); // close
                return;
            }
        }
        if (both_sticks && !m_chord_fired && !m_chord_menu_was_open &&
            now - m_chord_start >= std::chrono::milliseconds(CHORD_HOLD_MS)) {
            m_chord_fired = true;
            m_last_chord = now;
            if (!menu_open.load()) {
                handle_key(VK_F1, false); // open
            }
            return;
        }
        if (!both_sticks && both_sticks_prev) {
            if (!m_chord_fired && !m_chord_menu_was_open && !menu_open.load()) {
                m_chord_replay_until_ms.store(now_ms_steady() + CHORD_REPLAY_MS, std::memory_order_relaxed);
            }
            m_chord_fired = true;
        }

        if (!menu_open.load()) {
            return;
        }

        const bool big = (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;

        // Build 36: LT / RT switch tabs (edge-triggered, past half pull).
        const bool lt = have && st.Gamepad.bLeftTrigger > 128;
        const bool rt = have && st.Gamepad.bRightTrigger > 128;
        const bool lt_new = lt && !m_lt_prev;
        const bool rt_new = rt && !m_rt_prev;
        m_lt_prev = lt;
        m_rt_prev = rt;
        if (lt_new) handle_key(VK_PRIOR, big);
        if (rt_new) handle_key(VK_NEXT, big);

        if (pressed(XINPUT_GAMEPAD_DPAD_UP) || (stick_new && stick_dir == 1)) handle_key(VK_UP, big);
        if (pressed(XINPUT_GAMEPAD_DPAD_DOWN) || (stick_new && stick_dir == 2)) handle_key(VK_DOWN, big);
        if (pressed(XINPUT_GAMEPAD_DPAD_LEFT) || (stick_new && stick_dir == 3)) handle_key(VK_LEFT, big);
        if (pressed(XINPUT_GAMEPAD_DPAD_RIGHT) || (stick_new && stick_dir == 4)) handle_key(VK_RIGHT, big);
        if (pressed(XINPUT_GAMEPAD_A)) handle_key(VK_RETURN, big);
        if (pressed(XINPUT_GAMEPAD_B)) handle_key(VK_BACK, big);
    }

    // ---- Head tracking for the fixed HUD and the menu ----
    // update_head is called once per presented frame with the HMD pose in tracking space.
    // The game-screen HUD and the mod menu are real planes in tracking space (anchor pose + offsets), seen from each
    // eye's actual pose, so they stay put when the head turns, tilts or moves.
    std::atomic<bool> recenter_requested{false};
    std::atomic<bool> reload_textures_requested{false}; // Build 62: PragmataFixes runs it on the main thread
    // Build 64: shown on the Reload textures row: RELOADING while sent, then DONE (or FAILED) for a few seconds.
    enum ReloadState : int { ReloadIdle = 0, ReloadSent = 1, ReloadDone = 2, ReloadFailed = 3 };
    std::atomic<int> reload_textures_state{ReloadIdle};
    std::atomic<int64_t> reload_textures_ms{0};
    std::atomic<bool> view_recenter_requested{false}; // Build 40: VR::on_present recenters the view (rotation and standing origin)
    std::atomic<int> companion_generation{0}; // bumped when the companion mode changes (re-logs their setup)
    std::atomic<bool> recenter_if_far{false};
    std::atomic<bool> menu_place_requested{false};

    struct Pose {
        glm::mat4 rot{1.0f};
        glm::vec3 pos{0.0f};
    };

    struct HeadState {
        Pose head{};
        Pose hud{};
        Pose menu{};
        bool valid{false};
    };

    static Matrix4x4f yaw_only(const Matrix4x4f& r) {
        const glm::vec4 f = r * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f};
        const float yaw = std::atan2(-f.x, -f.z);
        return glm::rotate(glm::mat4{1.0f}, yaw, glm::vec3{0.0f, 1.0f, 0.0f});
    }

    void update_head(const Matrix4x4f& hmd_rotation, const glm::vec3& hmd_position) {
        glm::mat4 hmd = hmd_rotation;
        hmd[3] = glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};

        std::scoped_lock _{m_head_mtx};
        m_state.head = Pose{hmd, hmd_position};

        bool recenter = recenter_requested.exchange(false) || !m_anchor_valid;

        if (recenter_if_far.exchange(false) && m_anchor_valid) {
            const glm::vec4 head_fwd = yaw_only(hmd) * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f};
            const glm::vec4 anchor_fwd = m_state.hud.rot * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f};
            if (glm::dot(glm::vec3{head_fwd}, glm::vec3{anchor_fwd}) < 0.7f) { // more than about 45 degrees away
                recenter = true;
            }
        }

        if (recenter) {
            m_state.hud = Pose{yaw_only(hmd), hmd_position};
            m_anchor_valid = true;
            spdlog::info("[PragmataVR] HUD recentered");
        }

        if (menu_place_requested.exchange(false) || !m_menu_valid) {
            m_state.menu = Pose{yaw_only(hmd), hmd_position};
            m_menu_valid = true;
        }

        m_state.valid = true;
        m_lock = glm::transpose(hmd) * m_state.hud.rot; // per-panel layout (rotation only)
    }

    // Current poses. When the HUD follows the head, its anchor is the head itself.
    HeadState head_state() {
        const auto t = tune();
        std::scoped_lock _{m_head_mtx};
        HeadState st = m_state;
        if (!t->hud_fixed) {
            st.hud = st.head;
        }
        return st;
    }

    // Per-panel layout only: view-space rotation head^-1 * anchor (identity when following the head).
    Matrix4x4f lock_rotation() {
        const auto t = tune();
        if (!t->hud_fixed) {
            return glm::mat4{1.0f};
        }

        std::scoped_lock _{m_head_mtx};
        return t->lock_invert ? glm::transpose(m_lock) : m_lock;
    }

    // ---- Menu state, read by VR::on_frame (draw) and D3D12Component::on_frame (composite) ----
    std::atomic<bool> menu_open{false};
    std::atomic<int> menu_selected{0};
    std::atomic<int> menu_tab{TabGeneral}; // Build 36: tab shown in the mod menu
    std::atomic<bool> menu_editing{false};   // A on a value row: Left / Right change it until A or B
    std::atomic<bool> menu_on_invert{false}; // highlight is on the row's INV box (A flips it)
    std::atomic<float> menu_rect_x{0.0f};
    std::atomic<float> menu_rect_y{0.0f};
    std::atomic<float> menu_rect_w{0.0f};
    std::atomic<float> menu_rect_h{0.0f};
    std::atomic<bool> menu_rect_valid{false};

    const std::vector<MenuRow>& menu_rows() const { return m_rows; }

    // ---- Snapshot request state, consumed by D3D12Component::on_frame ----
    std::atomic<int> snapshot_frames_left{0};
    std::atomic<bool> snapshot_left_done{false};
    std::atomic<bool> snapshot_right_done{false};
    std::atomic<bool> snapshot_ui_done{false};
    std::atomic<bool> trace_requested{false};     // Build 18: GPU trace for the next two eye frames
    std::atomic<bool> dump_engine_targets{false}; // Build 18: log the overlay layer's render target setup once
    std::string snapshot_stem{};

    static std::filesystem::path data_dir() {
        return REFramework::get_persistent_dir("reframework/data");
    }

    static std::string format_menu(const HudTune& t, int field) {
        switch (field) {
        case MenuDistance: return fmt::format("{:.1f} m", t.menu[0]);
        case MenuScale: return fmt::format("{:.0f}%", t.menu[1] * 100.0f);
        case MenuClickVolume: return t.menu[3] <= 0.001f ? std::string{"OFF"} : fmt::format("{:.0f}%", t.menu[3] * 100.0f);
        case MenuPitch: return fmt::format("{:+.0f} deg", t.menu[2]);
        default: return {};
        }
    }

    static std::string format_screen(const HudTune& t, int field) {
        switch (field) {
        case ScreenWidth: return fmt::format("{:.0f} deg", t.screen[0]);
        case ScreenDistance: return fmt::format("{:.1f} m", t.screen[1]);
        case ScreenYaw: return fmt::format("{:+.0f} deg", t.screen[2]);
        case ScreenPitch: return fmt::format("{:+.0f} deg", t.screen[3]);
        case ScreenReticleDist: return fmt::format("{:.1f} m", t.screen[4]);
        default: return {};
        }
    }

    // Text for one value row, e.g. "Size  0.93", used by the menu.
    static std::string format_value(const GroupTune& gt, int group, int field) {
        switch (field) {
        case FieldSize: return fmt::format("{:.2f}x", gt.size);
        case FieldX: return fmt::format("{:+.0f}", gt.x);
        case FieldY: return fmt::format("{:+.0f}", gt.y);
        case FieldGap: return fmt::format("{:+.0f}", gt.gap);
        default: return {};
        }
    }

private:
    static constexpr std::array<int, 10> KEYS{VK_F1, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, VK_PAUSE, VK_PRIOR, VK_NEXT};

    HudTuning() {
        m_tune = std::make_shared<HudTune>();
        m_key_down.fill(false);
        build_rows();
    }

    // Build 25: navigation over the rows that are showing (collapsed sections hide their rows). Collapsible headers are
    // selectable (A = show or hide). Each row's height in its band (in row units; headers are about 1.3 rows tall, the
    // first one in a column about 0.9) is kept for Left / Right. Called at startup, when the menu opens and on collapse.
    void rebuild_nav(uint32_t collapsed) {
        for (auto& n : m_nav) {
            n.clear();
        }
        std::array<std::array<float, 2>, 2> ys{};
        std::array<std::array<bool, 2>, 2> first{{{true, true}, {true, true}}};
        m_row_y.assign(m_rows.size(), 0.0f);
        for (int i = 0; i < (int)m_rows.size(); ++i) {
            const auto& r = m_rows[i];
            const int c = std::clamp(r.column, 0, 1);
            const int bd = std::clamp(r.band, 0, 1);
            if (r.tab != menu_tab.load() || row_hidden(collapsed, r)) {
                continue;
            }
            if (r.kind == MenuRow::Kind::Header) {
                const float hh = first[bd][c] ? 0.93f : 1.33f;
                if (r.section >= 0) {
                    m_row_y[i] = ys[bd][c] + hh * 0.5f;
                    m_nav[c].push_back(i);
                }
                ys[bd][c] += hh;
                first[bd][c] = false;
                continue;
            }
            m_row_y[i] = ys[bd][c] + 0.5f;
            ys[bd][c] += 1.0f;
            m_nav[c].push_back(i);
        }
    }

    void build_rows() {
        m_rows.clear();

        int column = 0;
        int band = 0;
        int section = -1;
        const auto add = [&](MenuRow::Kind kind, int group, int field, int action, std::string label, std::string hint) {
            MenuRow r{};
            r.kind = kind;
            r.group = group;
            r.field = field;
            r.action = action;
            r.column = column;
            r.band = band;
            r.label = std::move(label);
            r.hint = std::move(hint);
            r.section = section;
            m_rows.push_back(std::move(r));
        };

        const std::string value_hint = "A = edit this value";
        int tab = TabGeneral;
        const auto add_row = [&](MenuRow::Kind kind, int group, int field, int action, std::string label, std::string hint) {
            add(kind, group, field, action, std::move(label), std::move(hint));
            m_rows.back().tab = tab;
        };
        // A plain (not collapsible) section header; hint = small grey tag on the right.
        const auto add_header = [&](std::string title, std::string tag) {
            section = -1;
            add_row(MenuRow::Kind::Header, -1, -1, ActionNone, std::move(title), std::move(tag));
        };
        const auto add_section = [&](int sec, std::string title) {
            section = sec;
            add_row(MenuRow::Kind::Header, -1, -1, ActionNone, std::move(title), "");
            m_rows.back().section = sec;
        };
        const auto add_element = [&](int g) {
            add_section(section_for_group(g), GROUP_TITLES[g]);
            m_rows.back().group = g;
            add_row(MenuRow::Kind::Toggle, g, -1, ActionToggleHackLock, "Lock", "A = lock or unlock this element (locked = its values cannot change)");
            add_row(MenuRow::Kind::Value, g, FieldSize, ActionNone, "Size", "A = edit. Grows or shrinks this element where it is");
            add_row(MenuRow::Kind::Value, g, FieldX, ActionNone, "Left / right", value_hint);
            add_row(MenuRow::Kind::Value, g, FieldY, ActionNone, "Up / down", value_hint);
            add_row(MenuRow::Kind::Value, g, FieldGap, ActionNone, "Eye gap",
                "A = edit the gap between eyes. In Follows Head mode, higher = closer to you (Fixed uses HUD distance)");
            add_row(MenuRow::Kind::Action, g, -1, ActionResetGroup, "Reset to defaults", "A = back to the starting values for this HUD mode (blocked while locked)");
        };

        // Build 36: four tabs (LT / RT). Every tab uses one band with two columns.
        band = 0;

        // ==== GENERAL ====
        tab = TabGeneral;
        column = 0;
        add_header("VR", "THE BASICS");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleRenderMode, "Render mode",
            "A = Native (both eyes from the same moment, best 3D) or AFW (one eye per frame, the other warped, faster)");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleHudPanel, "HUD mode",
            "A = Fixed (the HUD stays put in one place in front of you) or Follows Head (the HUD moves with your headset)");
        add_row(MenuRow::Kind::Action, -1, -1, ActionRecenterView, "Recenter",
            "A = recenter everything where you are now: the game view (like SteamVR or Quest recenter), the HUD and this menu");
        column = 1;
        add_header("FIXED HUD", "FIXED MODE");
        add_row(MenuRow::Kind::Value, GROUP_SCREEN, ScreenDistance, ActionNone, "HUD distance",
            "A = edit. How far away the Fixed HUD sits (meters). Its size in view stays the same");
        add_row(MenuRow::Kind::Value, GROUP_SCREEN, ScreenWidth, ActionNone, "Panel size", "A = edit. Size of the Fixed HUD (degrees wide)");
        add_row(MenuRow::Kind::Value, GROUP_SCREEN, ScreenYaw, ActionNone, "Panel left / right", value_hint);
        add_row(MenuRow::Kind::Value, GROUP_SCREEN, ScreenPitch, ActionNone, "Panel up / down", value_hint);

        // ==== HUD: each element ====
        tab = TabHud;
        column = 0;
        add_element(GroupHealth);
        add_element(GroupStamina);
        add_element(GroupItems); // Build 47
        // Build 50: placement over the enemy is automatic (PragmataMarkerFix.hpp). Build 51: health bars and damage numbers
        // each get their own section to move them relative to that.
        add_section(SectionEnemy, "ENEMY HEALTH BARS");
        add_row(MenuRow::Kind::Toggle, GROUP_ENEMY, -1, ActionToggleEnemyLock, "Lock", "A = lock or unlock this element (locked = its values cannot change)");
        add_row(MenuRow::Kind::Value, GROUP_ENEMY, EnemyX, ActionNone, "Left / right", "A = edit. Moves enemy health bars relative to the enemy");
        add_row(MenuRow::Kind::Value, GROUP_ENEMY, EnemyY, ActionNone, "Up / down", "A = edit. Moves enemy health bars relative to the enemy");
        add_row(MenuRow::Kind::Action, GROUP_ENEMY, -1, ActionResetEnemy, "Reset to defaults", "A = back to the starting values (blocked while locked)");
        add_section(SectionDamage, "DAMAGE NUMBERS");
        add_row(MenuRow::Kind::Toggle, GROUP_ENEMY, -1, ActionToggleDamageLock, "Lock", "A = lock or unlock this element (locked = its values cannot change)");
        add_row(MenuRow::Kind::Value, GROUP_ENEMY, DamageX, ActionNone, "Left / right", "A = edit. Moves the damage numbers relative to the enemy");
        add_row(MenuRow::Kind::Value, GROUP_ENEMY, DamageY, ActionNone, "Up / down", "A = edit. Moves the damage numbers relative to the enemy");
        add_row(MenuRow::Kind::Action, GROUP_ENEMY, -1, ActionResetDamage, "Reset to defaults", "A = back to the starting values (blocked while locked)");
        column = 1;
        add_element(GroupHack);
        add_element(GroupWeapons);
        add_element(GroupTutorial);
        add_element(GroupObjective); // Build 53

        // ==== EXPERIMENTAL (Build 63): work in progress ====
        tab = TabExperimental;
        column = 0;
        add_header("SUBTITLES", "WORK IN PROGRESS");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleSubtitleMode, "Subtitles",
            "A = New (own panel: no AFW doubling, tune it below) or Original (drawn as the game and REFramework do)");
        add_element(GroupSubtitles); // Build 58
        add_header("TEXTURES", "WORK IN PROGRESS");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleSharpTextures, "Sharp textures",
            "A = On: the game keeps high resolution textures loaded in VR (uses more VRAM). Off: the game's own choice");
        add_row(MenuRow::Kind::Action, -1, -1, ActionReloadTextures, "Reload textures",
            "A = test: drop and reload the game's streamed textures now (may hitch). For comparing texture detail");
        column = 1;
        add_header("RETICLE", "WORK IN PROGRESS");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleReticleMode, "Reticle",
            "A = New (own layer: no AFW doubling, follows the game camera; tune it below) or Original (as before)");
        // Build 60: the reticle stays where the gun points.
        add_section(section_for_group(GroupReticle), GROUP_TITLES[GroupReticle]);
        m_rows.back().group = GroupReticle;
        add_row(MenuRow::Kind::Toggle, GroupReticle, -1, ActionToggleHackLock, "Lock", "A = lock or unlock this element (locked = its values cannot change)");
        add_row(MenuRow::Kind::Value, GroupReticle, FieldSize, ActionNone, "Size", "A = edit. Size of the gun reticle");
        add_row(MenuRow::Kind::Value, GroupReticle, FieldX, ActionNone, "Left / right",
            "A = edit. Moves the reticle sideways onto where your shots land");
        add_row(MenuRow::Kind::Value, GroupReticle, FieldY, ActionNone, "Up / down",
            "A = edit. Moves the reticle up or down onto where your shots land");
        add_row(MenuRow::Kind::Value, GROUP_SCREEN, ScreenReticleDist, ActionNone, "Distance",
            "A = edit. How far away the gun reticle is shown (meters): its depth. Its size in view stays the same");
        add_row(MenuRow::Kind::Action, GroupReticle, -1, ActionResetGroup, "Reset to defaults", "A = back to the starting values (blocked while locked)");

        // ==== ADVANCED ====
        tab = TabAdvanced;
        column = 0;
        add_header("RENDERING", "VR RENDERING");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleSharpening, "Sharpening", "A = turn the sharpening pass on or off (small GPU cost)");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleDesktopMirror, "Desktop mirror",
            "A = show the headset image in the game window on your monitor (costs GPU time, only for recording)");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionTogglePerfLog, "Performance log",
            "A = write frame time numbers to re2_framework_log.txt every 10 seconds");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleWorldProj, "World HUD projection",
            "A = Headset (hacking circle, reticle, enemy markers line up on any headset) or Flat (the old monitor projection)");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleLowHealth, "Low health edges",
            "A = hide or show the red effect at the screen edges when health is low (made for a flat monitor)");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleMenuNative, "Native in menus",
            "A = On: full-screen menus (Firmware Updater, terminals...) draw in NATIVE while open, no AFW trails. Off: keep AFW");
        column = 1;
        add_header("DEVELOPER", "TOOLS");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleHackBackdrop, "Door hack backdrop",
            "A = hide or show the dark backdrop behind the door and lock hacking HUD");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleCaptureCorners, "Fixed HUD capture",
            "A = Corners (elements cut out where nothing visible is drawn) or Home (at their place in the game layout)");
        add_row(MenuRow::Kind::Toggle, -1, -1, ActionToggleBoxes, "Show element boxes",
            "A = Fixed mode: outline the areas moved to the room-fixed panel (check nothing is cut off or doubled)");
        add_row(MenuRow::Kind::Action, -1, -1, ActionSnapshot, "Take snapshot", "A = save both eye images to reframework\\data\\snapshots");
        add_row(MenuRow::Kind::Action, -1, -1, ActionSnapshotSeries, "Snapshot series",
            "A = close the menu, wait 3 s, then 5 snapshots 1 s apart (move your head to record HUD motion)");

        // ==== MOD MENU ====
        tab = TabMenu;
        column = 0;
        add_header("PLACEMENT", "FIXED WHERE IT OPENS");
        add_row(MenuRow::Kind::Value, GROUP_MENU, MenuDistance, ActionNone, "Distance", "A = edit. Text keeps the same size at any distance");
        add_row(MenuRow::Kind::Value, GROUP_MENU, MenuScale, ActionNone, "Text size", value_hint);
        add_row(MenuRow::Kind::Value, GROUP_MENU, MenuPitch, ActionNone, "Up / down", value_hint);
        add_row(MenuRow::Kind::Action, GROUP_MENU, -1, ActionResetMenu, "Reset menu placement", "A = default distance, size and height, and move the menu in front of you");
        column = 1;
        add_header("SOUND", "");
        add_row(MenuRow::Kind::Value, GROUP_MENU, MenuClickVolume, ActionNone, "Click volume", "A = edit. Volume of the menu click sounds (0 = off)");
        add_row(MenuRow::Kind::Action, -1, -1, ActionClose, "Close menu", "A = close. B or both thumbsticks also close the menu");

        rebuild_nav(HudTune{}.collapsed);
        select_first();
    }

    // Build 38: one settings file per headset. Until the headset is known (and on OpenVR) this is pragmatavr_hud.ini;
    // set_headset_profile switches to pragmatavr_hud_<runtime>_<fov>.ini, which starts as a copy of the current values.
    static std::filesystem::path path() {
        std::scoped_lock _{profile_mtx()};
        return data_dir() / (profile_name().empty() ? std::string{"pragmatavr_hud.ini"} : "pragmatavr_hud_" + profile_name() + ".ini");
    }

    static std::mutex& profile_mtx() {
        static std::mutex m{};
        return m;
    }
    static std::string& profile_name() {
        static std::string n{};
        return n;
    }

public:
    // Build 38: called by the OpenXR runtime once the headset is known.
    void set_headset_profile(const std::string& key) {
        {
            std::scoped_lock _{profile_mtx()};
            if (profile_name() == key) {
                return;
            }
        }
        const auto current = copy_current();
        const auto use_key = nearest_profile(key);
        {
            std::scoped_lock _{profile_mtx()};
            profile_name() = use_key;
        }
        if (use_key != key) {
            spdlog::info("[PragmataVR] HUD settings: headset reports {}, using the existing profile {} (same runtime, FOV within 3 degrees)", key, use_key);
        }
        std::error_code ec{};
        const auto p = path();
        if (!std::filesystem::exists(p, ec)) {
            write_file(*current); // a new headset starts from the values in use now
            spdlog::info("[PragmataVR] HUD settings: new profile {} (copied from the current values)", p.filename().string());
        } else {
            spdlog::info("[PragmataVR] HUD settings: using profile {}", p.filename().string());
        }
        {
            std::scoped_lock _{m_mtx};
            m_loaded_once = false; // load the profile on the next check
            m_last_check = {};
        }
    }

private:

    // Build 47: the Pimax reported 90x82, 91x83, 92x84 and 93x85 on different days, and each new key made a new profile
    // from untuned values. If there is no file for the exact key, use the existing profile of the same runtime whose
    // FOV is closest (within 3 degrees in each direction).
    static std::string nearest_profile(const std::string& key) {
        std::error_code ec{};
        const auto dir = data_dir();
        if (std::filesystem::exists(dir / ("pragmatavr_hud_" + key + ".ini"), ec)) {
            return key;
        }
        const auto us = key.rfind('_');
        const auto ux = key.rfind('x');
        if (us == std::string::npos || ux == std::string::npos || ux < us) {
            return key;
        }
        const auto runtime = key.substr(0, us);
        int w = 0, h = 0;
        try {
            w = std::stoi(key.substr(us + 1, ux - us - 1));
            h = std::stoi(key.substr(ux + 1));
        } catch (...) {
            return key;
        }
        std::string best = key;
        int best_d = 1 << 30;
        const std::string prefix = "pragmatavr_hud_" + runtime + "_";
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const auto fn = e.path().filename().string();
            if (fn.size() <= prefix.size() + 4 || fn.compare(0, prefix.size(), prefix) != 0 || e.path().extension() != ".ini") {
                continue;
            }
            const auto dims = fn.substr(prefix.size(), fn.size() - prefix.size() - 4);
            const auto x = dims.find('x');
            if (x == std::string::npos) {
                continue;
            }
            int fw = 0, fh = 0;
            try {
                fw = std::stoi(dims.substr(0, x));
                fh = std::stoi(dims.substr(x + 1));
            } catch (...) {
                continue;
            }
            const int dw = std::abs(fw - w);
            const int dh = std::abs(fh - h);
            if (dw <= 3 && dh <= 3 && dw + dh < best_d) {
                best_d = dw + dh;
                best = runtime + "_" + dims;
            }
        }
        return best;
    }

    static std::string trim(const std::string& s) {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
            return {};
        }
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static std::string to_text(const HudTune& t) {
        std::string s{};
        s += "# PragmataVR 3D HUD tuning. Saved changes apply within about a second, no restart needed.\n";
        s += "# The VR mod menu (hold both thumbstick clicks or F1) rewrites this file whenever a value changes.\n";
        s += "# Menu: hold both thumbstick clicks (about half a second) or F1 to open; both clicks, B or F1 close. D-pad / arrows navigate, A / Enter edits the highlighted value\n";
        s += "#   (Left/Right change it, RB / Shift = big steps, A or B = done), or runs an action.\n";
        s += "#   B / Backspace = stop editing or close. Pause = snapshot of both eyes.\n";
        s += "# <element>.size / .x / .y / .gap: hack, health, stamina, weapons, tutorial. x/y/gap in layout units (1920x1080 basis).\n";
        s += "# <element>.flat_size / .flat_x / .flat_y / .flat_gap: the values used in Fixed and Follows Head modes (canvas pixels, flat_y positive = down).\n";
        s += "# scale_about_center = 1: size grows each element around its own center (hack always uses the panel origin).\n";
        s += "# <group>.<field>.invert lines are ignored since v0.9.0 (the INV boxes are gone; directions are fixed in code).\n";
        s += "# hack_locked = 1 stops the menu changing the hacking circle.\n";
        s += fmt::format("ini_version={}\n", INI_VERSION);
        s += fmt::format("enabled={}\n", t.enabled ? 1 : 0);
        s += fmt::format("eye_mode={}\n", (int)t.eye_mode);
        s += fmt::format("zoom={:.4f}\n", t.zoom);
        s += fmt::format("depth_shift={:.4f}\n", t.depth_shift);
        s += fmt::format("y_sign={:.0f}\n", t.y_sign);
        s += fmt::format("log_elements={}\n", t.log_elements ? 1 : 0);
        s += "# collapsed: mod menu sections that are collapsed (sum of: 1 VIEW, 2 DEVELOPER, 4 hack, 8 health, 16 stamina, 32 weapons, 64 tutorial,\n";
        s += "#   128 PERFORMANCE, 256 ENEMY HEALTH BARS, 512 ITEM PICKUPS, 1024 DAMAGE NUMBERS,\n";
        s += "#   2048 OBJECTIVE, 4096 SUBTITLES, 8192 RETICLE).\n";
        s += fmt::format("collapsed={}\n", t.collapsed);
        s += fmt::format("show_boxes={}\n", t.show_boxes ? 1 : 0);
        s += "# menu_native: 1 = NATIVE while a full-screen menu is open (Build 55). hide_low_health: 1 = no red screen\n";
        s += "#   edges at low health (Build 57).\n";
        s += fmt::format("hide_low_health={}\n", t.hide_low_health ? 1 : 0);
        s += "# sharp_textures: 1 = keep high resolution textures loaded in VR (Build 59).\n";
        s += fmt::format("sharp_textures={}\n", t.sharp_textures ? 1 : 0);
        s += "# reticle_new: 1 = gun reticle on its own layer (RETICLE Size and Distance), 0 = original drawing (Build 61).\n";
        s += fmt::format("reticle_new={}\n", t.reticle_new ? 1 : 0);
        s += "# subtitle_gui: the GUI that draws the subtitles (Build 61; none = subtitles not moved to a panel).\n";
        s += "# subtitle_pin: 1 = subtitles held at their screen spot while shown on their panel (not next to the speaker).\n";
        s += fmt::format("subtitle_gui={}\n", t.subtitle_gui.empty() ? std::string{"none"} : t.subtitle_gui);
        s += fmt::format("subtitle_pin={}\n", t.subtitle_pin ? 1 : 0);
        s += "# subtitles_new: 1 = subtitles on their own panel, 0 = original drawing (Build 62).\n";
        s += fmt::format("subtitles_new={}\n", t.subtitles_new ? 1 : 0);
        s += "# items_area: item pickups area. 0 = automatic (big area only while a replicator list is open), 1 = always big,\n";
        s += "#   2 = always the small field area (Build 61).\n";
        s += fmt::format("items_area={}\n", t.items_area);
        s += fmt::format("menu_native={}\n", t.menu_native ? 1 : 0);
        s += fmt::format("hud_fixed={}\n", t.hud_fixed ? 1 : 0);
        s += fmt::format("lock_invert={}\n", t.lock_invert ? 1 : 0);
        s += fmt::format("hide_ghost={}\n", t.hide_ghost ? 1 : 0);
        s += "# hide_blur = 1: FLAT mode hides the frosted glass behind HUD elements.\n";
        s += fmt::format("hide_blur={}\n", t.hide_blur ? 1 : 0);
        s += "# flat_screen = 1: FLAT mode draws the 3D panels as Screen views (0 = World views, Builds 21-23).\n";
        s += fmt::format("flat_screen={}\n", t.flat_screen ? 1 : 0);
        s += fmt::format("scale_about_center={}\n", t.scale_about_center ? 1 : 0);
        s += "# companion_mode: 0 = companion layers drawn with their element, 1 = REFramework's floating sheet, 2 = hidden.\n";
        s += fmt::format("companion_mode={}\n", t.companion_mode);
        s += "# hud_mode: 1 = Follows Head (the HUD moves with your headset), 2 = Fixed (the HUD stays put in one place in front of\n";
        s += "#   you; screen.* = its size, distance and place). Both use the <element>.flat_* values. 0 = old 3D mode (retired).\n";
        s += fmt::format("hud_mode={}\n", t.hud_mode);
        s += "# engine_overlay: test of the engine's overlay layer draw (0 = default, 1 = skipped, 2 = drawn).\n";
        s += fmt::format("engine_overlay={}\n", t.engine_overlay);
        s += "# glued_hud_removed = 1: test, the UI is kept out of the eye images (only the HUD panel shows it).\n";
        s += fmt::format("glued_hud_removed={}\n", t.glued_hud_removed ? 1 : 0);
        s += "# PERFORMANCE (Build 35). render_mode: 0 = NATIVE (Two Frame Sequential), 1 = AFW (Alternate Frame Warping).\n";
        s += "#   sharpening and desktop_mirror: 1 = on. perf_log = 1: frame time summary in re2_framework_log.txt every 10 s.\n";
        s += fmt::format("render_mode={}\n", t.render_afw ? 1 : 0);
        s += fmt::format("sharpening={}\n", t.sharpening ? 1 : 0);
        s += fmt::format("desktop_mirror={}\n", t.desktop_mirror ? 1 : 0);
        s += fmt::format("perf_log={}\n", t.perf_log ? 1 : 0);
        s += "# world_hud_projection (Build 38): 1 = hacking circle, reticle and enemy markers use the headset's own projection\n";
        s += "#   (right place and shape on any headset; hack.headset_* values), 0 = the game's flat projection (hack.flat_*).\n";
        s += fmt::format("world_hud_projection={}\n", t.world_headset ? 1 : 0);
        s += "# fixed_capture_corners (Build 39): 1 = Fixed mode cuts the panel elements out in the corners of the eye images\n";
        s += "#   (outside the lens view), 0 = at their own place in the game layout.\n";
        s += fmt::format("fixed_capture_corners={}\n", t.fixed_capture_corners ? 1 : 0);
        s += "# enemy_bar.x / .y (Build 40): enemy health bar shift in canvas pixels (y positive = down), headset projection only.\n";
        s += fmt::format("hide_hack_backdrop={}\n", t.hide_hack_backdrop ? 1 : 0);
        s += "# HUD fixes (Build 46, applied on the main thread): 1 = on. fix_brackets = no hacking target brackets,\n";
        s += "#   fix_label = location label at full brightness, fix_door_vfx / fix_gauge_vfx = door hack / hacking gauge effects hidden.\n";
        s += fmt::format("fix_brackets={}\n", t.fix_brackets ? 1 : 0);
        s += fmt::format("fix_label={}\n", t.fix_label ? 1 : 0);
        s += fmt::format("fix_door_vfx={}\n", t.fix_door_vfx ? 1 : 0);
        s += fmt::format("fix_gauge_vfx={}\n", t.fix_gauge_vfx ? 1 : 0);
        s += "# screen_probe (Build 48, diagnostic): 1 = log which game functions turn world positions into screen positions.\n";
        s += fmt::format("screen_probe={}\n", t.screen_probe ? 1 : 0);
        s += fmt::format("enemy_bar.locked={}\n", t.enemy_locked ? 1 : 0);
        s += fmt::format("damage_locked={}\n", t.damage_locked ? 1 : 0);
        s += "# shift_damage (Build 43): 1 = move the damage numbers by enemy_bar.damage_x / damage_y (off while a crash is tested).\n";
        s += fmt::format("shift_damage={}\n", t.shift_damage ? 1 : 0);
        s += "# marker_fix (Build 50): 1 = enemy bars, damage numbers and world markers corrected for the headset's projection.\n";
        s += "#   marker_vscale = vertical factor of that correction. marker_trim.x / .y = fine-tune in canvas px (y positive = down).\n";
        s += fmt::format("marker_fix={}\n", t.marker_fix ? 1 : 0);
        s += fmt::format("marker_vscale={:.3f}\n", t.marker_vscale);
        for (int i = 0; i < EnemyFieldCount; ++i) {
            s += fmt::format("marker_trim.{}={:.2f}\n", ENEMY_KEYS[i], t.enemy_bar[i]);
            s += fmt::format("marker_trim.{}.invert={}\n", ENEMY_KEYS[i], t.enemy_bar_invert[i] ? 1 : 0);
        }
        s += fmt::format("hack.headset_size={:.4f}\n", t.hack_headset.size);
        s += fmt::format("hack.headset_x={:.2f}\n", t.hack_headset.x);
        s += fmt::format("hack.headset_y={:.2f}\n", t.hack_headset.y);
        s += fmt::format("hack.headset_gap={:.2f}\n", t.hack_headset.gap);
        for (int i = 0; i < SCREEN_FIELDS; ++i) {
            if (i == ScreenDistance) continue; // written below
            s += fmt::format("screen.{}={:.2f}\n", SCREEN_KEYS[i], t.screen[i]);
        }
        s += "# screen.distance = HUD distance in meters (the room-fixed HUD is shown as a panel this far in front of you).\n";
        s += fmt::format("screen.distance={:.2f}\n", t.screen[ScreenDistance]);
        s += "# menu.distance in meters, menu.scale = text size (1.00 = 100%), menu.pitch in degrees,\n";
        s += "# menu.click_volume = click sound level (0 = off, 0.50 = half, 1.00 = full).\n";
        for (int i = 0; i < (int)MENU_KEYS.size(); ++i) {
            s += fmt::format("menu.{}={:.2f}\n", MENU_KEYS[i], t.menu[i]);
            s += fmt::format("menu.{}.invert={}\n", MENU_KEYS[i], t.menu_invert[i] ? 1 : 0);
        }

        for (int g = 0; g < GroupCount; ++g) {
            const auto& gt = t.groups[g];
            s += fmt::format("{}.size={:.4f}\n", GROUP_KEYS[g], gt.size);
            s += fmt::format("{}.x={:.2f}\n", GROUP_KEYS[g], gt.x);
            s += fmt::format("{}.y={:.2f}\n", GROUP_KEYS[g], gt.y);
            s += fmt::format("{}.gap={:.2f}\n", GROUP_KEYS[g], gt.gap);
            s += fmt::format("{}.locked={}\n", GROUP_KEYS[g], t.locked[g] ? 1 : 0);
            const auto& ft = t.flat_groups[g];
            s += fmt::format("{}.flat_size={:.4f}\n", GROUP_KEYS[g], ft.size);
            s += fmt::format("{}.flat_x={:.2f}\n", GROUP_KEYS[g], ft.x);
            s += fmt::format("{}.flat_y={:.2f}\n", GROUP_KEYS[g], ft.y);
            s += fmt::format("{}.flat_gap={:.2f}\n", GROUP_KEYS[g], ft.gap);

            for (int f = 0; f < FieldCount; ++f) {
                s += fmt::format("{}.{}.invert={}\n", GROUP_KEYS[g], FIELD_KEYS[f], gt.invert[f] ? 1 : 0);
            }
        }

        return s;
    }

    void write_file(const HudTune& t) {
        const auto p = path();
        std::error_code ec{};
        std::filesystem::create_directories(p.parent_path(), ec);

        const auto tmp = p.string() + ".tmp";
        {
            std::ofstream f{tmp, std::ios::trunc};
            if (!f) {
                spdlog::error("[PragmataVR] Could not write {}", tmp);
                return;
            }
            f << to_text(t);
        }

        std::filesystem::rename(tmp, p, ec);
        if (ec) {
            spdlog::error("[PragmataVR] Could not replace {}: {}", p.string(), ec.message());
            return;
        }

        std::scoped_lock _{m_mtx};
        m_mtime = std::filesystem::last_write_time(p, ec);
        m_loaded_once = true;
    }

    void maybe_reload() {
        const auto now = std::chrono::steady_clock::now();

        {
            std::scoped_lock _{m_mtx};
            if (now - m_last_check < std::chrono::milliseconds(500)) {
                return;
            }
            m_last_check = now;
        }

        const auto p = path();
        std::error_code ec{};

        if (!std::filesystem::exists(p, ec)) {
            write_file(HudTune{});
        }

        const auto mtime = std::filesystem::last_write_time(p, ec);
        if (ec) {
            return;
        }

        {
            std::scoped_lock _{m_mtx};
            if (m_loaded_once && mtime == m_mtime) {
                return;
            }
        }

        auto t = std::make_shared<HudTune>();
        std::ifstream f{p};
        std::string line{};

        // Read the whole file first: hack.* lines only count when the file was written by Build 11 or later.
        std::vector<std::string> lines{};
        int file_version = 0;
        while (std::getline(f, line)) {
            const auto k = trim(line.substr(0, line.find('=')));
            if (k == "ini_version" && line.find('=') != std::string::npos) {
                try { file_version = std::stoi(trim(line.substr(line.find('=') + 1))); } catch (...) {}
            }
            lines.push_back(line);
        }
        f.close();
        const bool old_file = file_version < INI_VERSION;
        const bool old_hack = file_version < INI_VERSION_HACK;
        const bool old_fixed = file_version < INI_VERSION_FIXED_HUD;
        const bool flat_y_up = file_version < INI_VERSION_FLAT_Y_DOWN;
        if (old_file) {
            spdlog::info("[PragmataVR] pragmatavr_hud.ini v{} is from an older build:{}{} (file will be rewritten as v{})", file_version,
                old_hack ? " hacking circle set to the locked values, other elements neutral;" : "",
                old_fixed ? " HUD fixed in the room" : "", INI_VERSION);
        }

        for (auto& l : lines) {
            line = l;
            const auto hash_pos = line.find('#');
            if (hash_pos != std::string::npos) {
                line = line.substr(0, hash_pos);
            }

            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }

            const auto key = trim(line.substr(0, eq));
            const auto val_str = trim(line.substr(eq + 1));
            if (key.empty() || val_str.empty()) {
                continue;
            }

            // Build 61: the one text value.
            if (key == "subtitle_gui") {
                if (val_str.size() < 64 && val_str.rfind("ui", 0) == 0) {
                    t->subtitle_gui = val_str;
                } else if (val_str == "none") {
                    t->subtitle_gui.clear();
                }
                continue;
            }

            float val = 0.0f;
            try {
                val = std::stof(val_str);
            } catch (...) {
                spdlog::warn("[PragmataVR] pragmatavr_hud.ini: bad value for {}: {}", key, val_str);
                continue;
            }

            const auto dot = key.find('.');
            if (dot == std::string::npos) {
                if (old_fixed && (key == "hud_fixed" || key == "hack_locked")) continue;
                if (key == "enabled") t->enabled = val != 0.0f;
                else if (key == "eye_mode") t->eye_mode = (EyeMode)std::clamp((int)val, 0, 3);
                else if (key == "zoom") t->zoom = std::clamp(val, 0.05f, 20.0f);
                else if (key == "depth_shift") t->depth_shift = std::clamp(val, -1.0f, 1.0f);
                else if (key == "y_sign") t->y_sign = val < 0.0f ? -1.0f : 1.0f;
                else if (key == "log_elements") t->log_elements = val != 0.0f;
                else if (key == "hack_locked") { t->hack_locked = val != 0.0f; t->locked[GroupHack] = t->hack_locked; } // old files
                else if (key == "collapsed") t->collapsed = (uint32_t)std::max(0.0f, val);
                else if (key == "show_boxes") t->show_boxes = val != 0.0f;
                else if (key == "menu_native") t->menu_native = val != 0.0f;
                else if (key == "hide_low_health") t->hide_low_health = val != 0.0f;
                else if (key == "sharp_textures") t->sharp_textures = val != 0.0f;
                else if (key == "reticle_new") t->reticle_new = val != 0.0f;
                else if (key == "subtitle_pin") t->subtitle_pin = val != 0.0f;
                else if (key == "subtitles_new") t->subtitles_new = val != 0.0f;
                else if (key == "items_area") t->items_area = std::clamp((int)val, 0, 2);
                else if (key == "hud_fixed") t->hud_fixed = val != 0.0f;
                else if (key == "lock_invert") t->lock_invert = val != 0.0f;
                else if (key == "hide_ghost") t->hide_ghost = val != 0.0f;
                else if (key == "hide_blur") t->hide_blur = val != 0.0f;
                else if (key == "flat_screen") t->flat_screen = val != 0.0f;
                else if (key == "scale_about_center") t->scale_about_center = val != 0.0f;
                else if (key == "companion_mode") t->companion_mode = std::clamp((int)val, 0, 2);
                else if (key == "hud_mode") t->hud_mode = std::clamp((int)val, 0, 2);
                // flat_test and hud_panel lines (Builds 16-20) are ignored: flat_test stayed on unseen after its row went.
                else if (key == "engine_overlay") t->engine_overlay = std::clamp((int)val, 0, 2);
                else if (key == "glued_hud_removed") t->glued_hud_removed = val != 0.0f;
                else if (key == "render_mode") t->render_afw = val != 0.0f;
                else if (key == "sharpening") t->sharpening = val != 0.0f;
                else if (key == "desktop_mirror") t->desktop_mirror = val != 0.0f;
                else if (key == "perf_log") t->perf_log = val != 0.0f;
                else if (key == "world_hud_projection") t->world_headset = val != 0.0f;
                else if (key == "fixed_capture_corners") t->fixed_capture_corners = val != 0.0f;
                else if (key == "hide_hack_backdrop") t->hide_hack_backdrop = val != 0.0f;
                else if (key == "fix_brackets") t->fix_brackets = val != 0.0f;
                else if (key == "fix_label") t->fix_label = val != 0.0f;
                else if (key == "fix_door_vfx") t->fix_door_vfx = val != 0.0f;
                else if (key == "fix_gauge_vfx") t->fix_gauge_vfx = val != 0.0f;
                else if (key == "screen_probe") t->screen_probe = val != 0.0f;
                else if (key == "marker_fix") t->marker_fix = val != 0.0f;
                else if (key == "damage_locked") t->damage_locked = val != 0.0f;
                else if (key == "marker_vscale") t->marker_vscale = std::clamp(val, 0.0f, 4.0f);
                else if (key == "shift_damage") t->shift_damage = val != 0.0f;
                // layout_mode is ignored from Build 11 on (always per-element). hud_fixed and hack_locked from an
                // older file are ignored too, so the HUD comes back exactly as the hacking circle was tuned.
                continue;
            }

            const auto group_key = key.substr(0, dot);
            auto rest = key.substr(dot + 1);
            const bool is_invert = rest.size() > 7 && rest.substr(rest.size() - 7) == ".invert";
            if (is_invert) {
                rest = rest.substr(0, rest.size() - 7);
            }

            if (group_key == "menu") {
                for (int i = 0; i < (int)MENU_KEYS.size(); ++i) {
                    if (rest == MENU_KEYS[i]) {
                        if (is_invert) t->menu_invert[i] = false; // v0.9.0: INV boxes removed (directions fixed in code)
                        else if (i == MenuClickVolume && file_version < INI_VERSION_CLICK_SCALE) t->menu[i] = std::clamp(val * 5.0f, MENU_MIN[i], MENU_MAX[i]);
                        else t->menu[i] = std::clamp(val, MENU_MIN[i], MENU_MAX[i]);
                    }
                }
                continue;
            }

            if (group_key == "enemy_bar" && rest == "locked" && !is_invert) {
                t->enemy_locked = val != 0.0f;
                continue;
            }
            if (group_key == "marker_trim") { // Build 50 (old enemy_bar.* values were for the removed manual shift)
                for (int i = 0; i < EnemyFieldCount; ++i) {
                    if (rest == ENEMY_KEYS[i]) {
                        if (is_invert) t->enemy_bar_invert[i] = false; // v0.9.0: INV boxes removed
                        else t->enemy_bar[i] = std::clamp(val, -ENEMY_MAX, ENEMY_MAX);
                    }
                }
                continue;
            }

            if (group_key == "screen") {
                for (int i = 0; i < SCREEN_FIELDS; ++i) {
                    if (rest == SCREEN_KEYS[i]) {
                        if (is_invert) t->screen_invert[i] = false; // v0.9.0: INV boxes removed
                        else t->screen[i] = std::clamp(val, SCREEN_MIN[i], SCREEN_MAX[i]);
                    }
                }
                continue;
            }

            // Build 38: hacking circle values for the headset projection.
            if (group_key == "hack" && !is_invert && rest.size() > 8 && rest.substr(0, 8) == "headset_") {
                const auto f = rest.substr(8);
                if (f == "size") t->hack_headset.size = std::clamp(val, 0.05f, 8.0f);
                else if (f == "x") t->hack_headset.x = std::clamp(val, -4000.0f, 4000.0f);
                else if (f == "y") t->hack_headset.y = std::clamp(val, -4000.0f, 4000.0f);
                else if (f == "gap") t->hack_headset.gap = std::clamp(val, -4000.0f, 4000.0f);
                continue;
            }

            for (int g = 0; g < GroupCount; ++g) {
                if (group_key != GROUP_KEYS[g]) {
                    continue;
                }
                if (old_hack && g == GroupHack) {
                    continue;
                }

                if (rest == "locked" && !is_invert) {
                    t->locked[g] = val != 0.0f;
                    continue;
                }
                const bool flat_key = rest.size() > 5 && rest.substr(0, 5) == "flat_";
                if (flat_key) {
                    rest = rest.substr(5);
                    if (is_invert) {
                        continue; // invert flags are shared by both modes
                    }
                }
                auto& gt = flat_key ? t->flat_groups[g] : t->groups[g];

                for (int fi = 0; fi < FieldCount; ++fi) {
                    if (rest != FIELD_KEYS[fi]) {
                        continue;
                    }

                    if (is_invert) {
                        gt.invert[fi] = false; // v0.9.0: INV boxes removed (directions fixed in code)
                        continue;
                    }

                    switch (fi) {
                    case FieldSize: gt.size = std::clamp(val, MIN_SIZE, MAX_SIZE); break;
                    case FieldX: gt.x = std::clamp(val, -MAX_MOVE, MAX_MOVE); break;
                    case FieldY: gt.y = std::clamp(flat_key && flat_y_up ? -val : val, -MAX_MOVE, MAX_MOVE); break;
                    case FieldGap: gt.gap = std::clamp(val, -MAX_GAP, MAX_GAP); break;
                    default: break;
                    }
                }
            }
        }

        t->apply_hud_mode();
        {
            std::string desc{};
            for (int g = 0; g < GroupCount; ++g) {
                const auto& gt = active_group(*t, g);
                desc += fmt::format(" {}(size={:.3f} x={:.0f} y={:.0f} gap={:.0f})", GROUP_KEYS[g], gt.size, gt.x, gt.y, gt.gap);
            }
            spdlog::info("[PragmataVR] Loaded pragmatavr_hud.ini v{}: HUD mode {} locks={}{}{}{}{}{}{} hud_fixed={} companion_mode={} "
                "engine_overlay={} glued_hud_removed={} hide_ghost={} | {} values:{}",
                file_version, hud_mode_name(t->hud_mode), (int)t->locked[0], (int)t->locked[1], (int)t->locked[2], (int)t->locked[3], (int)t->locked[4], (int)t->locked[5], (int)t->locked[6], t->hud_fixed, t->companion_mode, t->engine_overlay,
                t->glued_hud_removed, t->hide_ghost, hud_mode_name(t->hud_mode), desc);
        }

        g_click_volume_pct = (int)std::lround(t->menu[MenuClickVolume] * 100.0f);
        publish_names(*t);

        if (old_file) {
            write_file(*t); // upgrade the file to this build's format (also sets m_mtime)
        }

        std::scoped_lock _{m_mtx};
        m_tune = t;
        if (!old_file) {
            m_mtime = mtime;
        }
        m_loaded_once = true;
    }

    // Build 61: values the render threads read without a tune copy.
    static void publish_names(const HudTune& t) {
        g_subtitle_hash.store(t.subtitle_gui.empty() || !t.subtitles_new ? 0 : name_hash(t.subtitle_gui), std::memory_order_relaxed);
        g_items_area_mode_cache.store(t.items_area, std::memory_order_relaxed);
    }

    void commit(const std::shared_ptr<HudTune>& t) {
        g_click_volume_pct = (int)std::lround(t->menu[MenuClickVolume] * 100.0f);
        publish_names(*t);
        write_file(*t);
        std::scoped_lock _{m_mtx};
        m_tune = t;
    }

    std::shared_ptr<HudTune> copy_current() {
        std::scoped_lock _{m_mtx};
        return std::make_shared<HudTune>(*m_tune);
    }

    // Build 36: first selectable row of the current tab (left column, or the right one if the left is empty).
    void select_first() {
        if (!m_nav[0].empty()) {
            menu_selected = m_nav[0].front();
        } else if (!m_nav[1].empty()) {
            menu_selected = m_nav[1].front();
        }
        menu_on_invert = false;
    }

    // Up / Down: next selectable row in the same column, through both bands, wrapping around.
    void move_vertical(int dir) {
        const int sel = menu_selected.load();
        const int c = sel >= 0 && sel < (int)m_rows.size() ? std::clamp(m_rows[sel].column, 0, 1) : 0;
        const auto& nav = m_nav[c];
        if (nav.empty()) {
            return;
        }

        auto it = std::find(nav.begin(), nav.end(), sel);
        int pos = it == nav.end() ? 0 : (int)(it - nav.begin());
        pos = (pos + dir + (int)nav.size()) % (int)nav.size();
        menu_selected = nav[pos];

        // Stay on the INV box when moving between value rows.
        if (m_rows[nav[pos]].kind != MenuRow::Kind::Value) {
            menu_on_invert = false;
        }
    }

    // Left / Right while navigating: row -> its INV box -> the other column (nearest row in the same band).
    // Returns false when there is nothing further that way.
    bool move_horizontal(int dir) {
        const int sel = menu_selected.load();
        if (sel < 0 || sel >= (int)m_rows.size()) {
            return false;
        }

        const auto& row = m_rows[sel];
        const bool is_value = row.kind == MenuRow::Kind::Value;
        const bool on_inv = menu_on_invert.load() && is_value;

        // v0.9.0: the INV boxes are gone; Left / Right goes straight to the other column.
        menu_on_invert = false;
        (void)is_value;
        (void)on_inv;

        const int target_col = row.column + dir;
        if (target_col < 0 || target_col > 1) {
            return false;
        }

        int best = -1;
        float best_d = 1e9f;
        for (const int i : m_nav[target_col]) {
            if (m_rows[i].band != row.band) {
                continue;
            }
            const float d = std::fabs(m_row_y[i] - m_row_y[sel]);
            if (d < best_d - 0.01f) {
                best_d = d;
                best = i;
            }
        }
        if (best < 0) {
            return false;
        }

        menu_selected = best;
        menu_on_invert = false;
        return true;
    }

    // Snapshot series (Build 13): 5 snapshots one second apart, starting 3 seconds after the menu closes, so head
    // movement can be recorded with the HUD showing and the menu out of the way.
    int m_series_left{0};
    std::chrono::steady_clock::time_point m_series_next{};

    void poll_snapshot_series() {
        if (m_series_left <= 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < m_series_next || snapshot_frames_left.load() > 0) {
            return;
        }
        request_snapshot();
        --m_series_left;
        m_series_next = now + std::chrono::seconds(1);
        if (m_series_left == 0) {
            spdlog::info("[PragmataVR] Snapshot series finished");
        }
    }

    void block_pad_briefly() {
        m_block_pad_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    }

    void request_snapshot() {
        if (snapshot_frames_left.load() > 0) {
            play_click(false);
            return;
        }

        const auto stamp = std::chrono::system_clock::now();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count();
        snapshot_stem = fmt::format("snapshot_{}", secs);
        snapshot_left_done = false;
        snapshot_right_done = false;
        snapshot_ui_done = false;
        trace_requested = true; // Build 19: the overlay layer dump of Build 18 is gone (it most likely caused the crash)

        std::error_code ec{};
        std::filesystem::create_directories(data_dir() / "snapshots", ec);
        {
            std::ofstream f{data_dir() / "snapshots" / (snapshot_stem + "_settings.txt")};
            std::shared_ptr<const HudTune> cur{};
            {
                std::scoped_lock _{m_mtx};
                cur = m_tune;
            }
            f << to_text(*cur);
        }

        snapshot_frames_left = 8;
        g_gui_list_until_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + 400, std::memory_order_relaxed); // Build 57
        spdlog::info("[PragmataVR] Snapshot requested ({})", snapshot_stem);
        play_click(true);
    }

    void handle_key(int vk, bool shift) {
        if (vk == VK_PAUSE) {
            request_snapshot();
            return;
        }

        if (vk == VK_F1) {
            menu_open = !menu_open.load();
            menu_editing = false;
            menu_on_invert = false;
            if (!menu_open.load()) {
                block_pad_briefly();
            } else {
                recenter_if_far = true;
                menu_place_requested = true;
                rebuild_nav(copy_current()->collapsed);
                const int s0 = menu_selected.load();
                if (s0 < 0 || s0 >= (int)m_rows.size() || m_rows[s0].tab != menu_tab.load() || row_hidden(copy_current()->collapsed, m_rows[s0])) {
                    select_first();
                }
            }
            spdlog::info("[PragmataVR] Menu {}", menu_open.load() ? "opened" : "closed");
            play_click(true);
            return;
        }

        if (!menu_open.load()) {
            return;
        }

        const bool editing = menu_editing.load();

        // Build 36: LT / RT (Page Up / Page Down): previous / next tab, wrapping around. Not while editing a value.
        if (vk == VK_PRIOR || vk == VK_NEXT) {
            if (editing) {
                play_click(false);
                return;
            }
            const int tab = (menu_tab.load() + (vk == VK_NEXT ? 1 : TabCount - 1)) % TabCount;
            menu_tab = tab;
            menu_on_invert = false;
            rebuild_nav(copy_current()->collapsed);
            select_first();
            spdlog::info("[PragmataVR] Menu: tab {}", TAB_TITLES[tab]);
            play_click(true);
            return;
        }

        switch (vk) {
        case VK_BACK:
            if (editing) {
                menu_editing = false; // B: stop editing, back to navigation
                play_click(true);
                return;
            }
            menu_open = false;
            menu_on_invert = false;
            block_pad_briefly();
            spdlog::info("[PragmataVR] Menu closed");
            play_click(true);
            return;
        case VK_UP:
        case VK_DOWN:
            if (editing) {
                play_click(false); // finish editing with A or B first
                return;
            }
            move_vertical(vk == VK_UP ? -1 : 1);
            play_click(true);
            return;
        case VK_LEFT:
        case VK_RIGHT:
            if (!editing) {
                play_click(move_horizontal(vk == VK_RIGHT ? 1 : -1));
                return;
            }
            break; // editing: change the value below
        default:
            break;
        }

        const int sel = menu_selected.load();
        if (sel < 0 || sel >= (int)m_rows.size()) {
            return;
        }

        const auto& row = m_rows[sel];
        auto t = copy_current();

        if (vk == VK_RETURN && editing) {
            menu_editing = false; // A: done editing
            play_click(true);
            return;
        }

        if (vk == VK_RETURN && row.kind == MenuRow::Kind::Value && !menu_on_invert.load()) {
            if ((row.group >= 0 && row.group < GroupCount && group_locked(*t, row.group)) || (row.group == GROUP_ENEMY && enemy_field_locked(*t, row.field)) ||
                (row.group == GROUP_SCREEN && row.field == ScreenReticleDist && group_locked(*t, GroupReticle))) { // Build 61
                play_click(false);
                return;
            }
            menu_editing = true; // A on a value: edit it with Left / Right
            play_click(true);
            return;
        }

        if (vk == VK_RETURN && row.kind == MenuRow::Kind::Header) {
            if (row.section < 0) {
                return;
            }
            t->collapsed ^= (1u << row.section);
            commit(t);
            rebuild_nav(t->collapsed);
            spdlog::info("[PragmataVR] Menu: section {} {}", row.label, section_collapsed(t->collapsed, row.section) ? "hidden" : "shown");
            play_click(true);
            return;
        }

        if (vk == VK_RETURN) {
            switch (row.kind) {
            case MenuRow::Kind::Value: {
                // The highlight is on this row's INV box: flip which way Left / Right moves the value.
                if (row.group == GROUP_MENU) {
                    auto& minv = t->menu_invert[row.field];
                    minv = !minv;
                    commit(t);
                    play_click(true);
                    return;
                }
                if (row.group == GROUP_ENEMY) {
                    if (enemy_field_locked(*t, row.field)) {
                        play_click(false);
                        return;
                    }
                    auto& einv = t->enemy_bar_invert[row.field];
                    einv = !einv;
                    commit(t);
                    play_click(true);
                    return;
                }
                if (row.group == GROUP_SCREEN) {
                    auto& inv = t->screen_invert[row.field];
                    inv = !inv;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: screen {} invert = {}", SCREEN_KEYS[row.field], inv);
                    play_click(true);
                    return;
                }
                if (group_locked(*t, row.group)) {
                    play_click(false);
                    return;
                }
                // Invert flags are shared by both modes; the 3D set holds them.
                auto& inv = t->groups[row.group].invert[row.field];
                inv = !inv;
                commit(t);
                spdlog::info("[PragmataVR] Menu: {} {} invert = {}", GROUP_KEYS[row.group], FIELD_KEYS[row.field], inv);
                play_click(true);
                return;
            }
            case MenuRow::Kind::Toggle:
                if (row.action == ActionToggleLayout) {
                    t->layout_mode = t->layout_mode == 1 ? 0 : 1;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: HUD layout {}", t->layout_mode == 1 ? "game screen" : "per-panel");
                    play_click(true);
                } else if (row.action == ActionToggleHudFixed) {
                    t->hud_fixed = !t->hud_fixed;
                    commit(t);
                    recenter_requested = true;
                    spdlog::info("[PragmataVR] Menu: HUD {}", t->hud_fixed ? "fixed in room" : "follows head");
                    play_click(true);
                } else if (row.action == ActionToggleLockInvert) {
                    t->lock_invert = !t->lock_invert;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: fixed HUD direction {}", t->lock_invert ? "flipped" : "normal");
                    play_click(true);
                } else if (row.action == ActionToggleRenderMode) {
                    t->render_afw = !t->render_afw;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: render mode {}", t->render_afw ? "AFW" : "NATIVE");
                    play_click(true);
                } else if (row.action == ActionToggleSharpening) {
                    t->sharpening = !t->sharpening;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: sharpening {}", t->sharpening ? "on" : "off");
                    play_click(true);
                } else if (row.action == ActionToggleDesktopMirror) {
                    t->desktop_mirror = !t->desktop_mirror;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: desktop mirror {}", t->desktop_mirror ? "on" : "off");
                    play_click(true);
                } else if (row.action == ActionToggleShiftDamage) {
                    t->shift_damage = !t->shift_damage;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: damage numbers shift {}", t->shift_damage ? "on" : "off");
                    play_click(true);
                } else if (row.action == ActionToggleEnemyLock) {
                    t->enemy_locked = !t->enemy_locked;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: enemy health bars {}", t->enemy_locked ? "locked" : "unlocked");
                    play_click(true);
                } else if (row.action == ActionToggleSubtitleMode) {
                    t->subtitles_new = !t->subtitles_new;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: subtitles {}", t->subtitles_new ? "new" : "original");
                    play_click(true);
                } else if (row.action == ActionToggleReticleMode) {
                    t->reticle_new = !t->reticle_new;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: reticle {}", t->reticle_new ? "new" : "original");
                    play_click(true);
                } else if (row.action == ActionToggleSharpTextures) {
                    t->sharp_textures = !t->sharp_textures;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: sharp textures {}", t->sharp_textures ? "on" : "off");
                    play_click(true);
                } else if (row.action == ActionToggleLowHealth) {
                    t->hide_low_health = !t->hide_low_health;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: low health edges {}", t->hide_low_health ? "hidden" : "shown");
                    play_click(true);
                } else if (row.action == ActionToggleMenuNative) {
                    t->menu_native = !t->menu_native;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: native in menus {}", t->menu_native ? "on" : "off");
                    play_click(true);
                } else if (row.action == ActionToggleDamageLock) {
                    // Build 52: the Lock row is a Toggle row; Build 51 handled it in the Action branch, so A did nothing.
                    t->damage_locked = !t->damage_locked;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: damage numbers {}", t->damage_locked ? "locked" : "unlocked");
                    play_click(true);
                } else if (row.action == ActionToggleHackBackdrop) {
                    t->hide_hack_backdrop = !t->hide_hack_backdrop;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: door hack backdrop {}", t->hide_hack_backdrop ? "hidden" : "shown");
                    play_click(true);
                } else if (row.action == ActionToggleCaptureCorners) {
                    t->fixed_capture_corners = !t->fixed_capture_corners;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: Fixed HUD capture {}", t->fixed_capture_corners ? "CORNERS" : "HOME");
                    play_click(true);
                } else if (row.action == ActionToggleWorldProj) {
                    t->world_headset = !t->world_headset;
                    commit(t);
                    companion_generation++;
                    spdlog::info("[PragmataVR] Menu: world HUD projection {}", t->world_headset ? "HEADSET" : "FLAT");
                    play_click(true);
                } else if (row.action == ActionTogglePerfLog) {
                    t->perf_log = !t->perf_log;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: performance log {}", t->perf_log ? "on" : "off");
                    play_click(true);
                } else if (row.action == ActionToggleBoxes) {
                    t->show_boxes = !t->show_boxes;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: element boxes {}", t->show_boxes ? "shown" : "hidden");
                    play_click(true);
                } else if (row.action == ActionToggleFlatScreen) {
                    t->flat_screen = !t->flat_screen;
                    commit(t);
                    companion_generation++; // re-log the FLAT setup
                    spdlog::info("[PragmataVR] Menu: FLAT 3D panels {}", t->flat_screen ? "SCREEN" : "WORLD");
                    play_click(true);
                } else if (row.action == ActionToggleBlur) {
                    t->hide_blur = !t->hide_blur;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: frosted glass {}", t->hide_blur ? "hidden" : "shown");
                    play_click(true);
                } else if (row.action == ActionToggleGhost) {
                    t->hide_ghost = !t->hide_ghost;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: ghost layer {}", t->hide_ghost ? "hidden" : "shown");
                    play_click(true);
                } else if (row.action == ActionToggleHudPanel) {
                    // Build 28: FLAT <-> ANCHORED
                    t->hud_mode = t->hud_mode == HudModeAnchored ? HudModeFlat : HudModeAnchored;
                    t->apply_hud_mode();
                    commit(t);
                    companion_generation++;
                    spdlog::info("[PragmataVR] Menu: HUD mode {}", hud_mode_name(t->hud_mode));
                    play_click(true);
                } else if (row.action == ActionToggleGluedHud) {
                    t->glued_hud_removed = !t->glued_hud_removed;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: glued HUD {}", t->glued_hud_removed ? "removed" : "shown");
                    play_click(true);
                } else if (row.action == ActionToggleEngineOverlay) {
                    t->engine_overlay = (t->engine_overlay + 1) % 3;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: engine UI blend {}", t->engine_overlay == 0 ? "default" : (t->engine_overlay == 1 ? "off" : "on"));
                    play_click(true);
                } else if (row.action == ActionToggleCompanion) {
                    t->companion_mode = (t->companion_mode + 1) % 3;
                    commit(t);
                    companion_generation++;
                    spdlog::info("[PragmataVR] Menu: companion layers mode {}", t->companion_mode);
                    play_click(true);
                } else if (row.action == ActionToggleScaleCenter) {
                    t->scale_about_center = !t->scale_about_center;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: size grows from {}", t->scale_about_center ? "element center" : "panel origin");
                    play_click(true);
                } else if (row.action == ActionToggleHackLock) {
                    const int lg = std::clamp(row.group, 0, (int)GroupCount - 1);
                    t->locked[lg] = !t->locked[lg];
                    if (lg == GroupHack) {
                        t->hack_locked = t->locked[lg];
                    }
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: {} {}", GROUP_KEYS[lg], t->locked[lg] ? "locked" : "unlocked");
                    play_click(true);
                }
                return;
            case MenuRow::Kind::Action:
                // Build 64: Reload textures was handled with the toggles in Builds 62-63, so A on it did nothing.
                if (row.action == ActionReloadTextures) {
                    // Next build: 5 s cool-down between reloads (presses meanwhile get the "no" click and are ignored).
                    if (now_ms_steady() - reload_textures_ms.load() < 5000 && reload_textures_state.load() != ReloadIdle) {
                        play_click(false);
                        return;
                    }
                    reload_textures_state.store(ReloadSent);
                    reload_textures_ms.store(now_ms_steady());
                    reload_textures_requested = true;
                    spdlog::info("[PragmataVR] Menu: reload textures requested");
                    play_click(true);
                    return;
                }
                if (row.action == ActionResetMenu) {
                    const auto inverts = t->menu_invert;
                    const float volume = t->menu[MenuClickVolume];
                    t->menu = HudTune{}.menu;
                    t->menu[MenuClickVolume] = volume; // placement only
                    t->menu_invert = inverts;
                    commit(t);
                    menu_place_requested = true;
                    spdlog::info("[PragmataVR] Menu: reset menu placement");
                    play_click(true);
                } else if (row.action == ActionResetScreen) {
                    const auto inverts = t->screen_invert;
                    t->screen = HudTune{}.screen;
                    t->screen_invert = inverts;
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: reset HUD screen");
                    play_click(true);
                } else if (row.action == ActionResetEnemy || row.action == ActionResetDamage) {
                    const bool dmg = row.action == ActionResetDamage;
                    if (dmg ? t->damage_locked : t->enemy_locked) {
                        play_click(false);
                        return;
                    }
                    const auto d = HudTune{}.enemy_bar;
                    const int f0 = dmg ? DamageX : EnemyX;
                    t->enemy_bar[f0] = d[f0];
                    t->enemy_bar[f0 + 1] = d[f0 + 1];
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: reset {}", dmg ? "damage numbers" : "enemy health bars");
                    play_click(true);
                } else if (row.action == ActionRecenterView) {
                    view_recenter_requested = true;
                    recenter_requested = true;
                    menu_place_requested = true; // Build 42: one Recenter for the view, the HUD and the menu
                    spdlog::info("[PragmataVR] Menu: recenter view");
                    play_click(true);
                } else if (row.action == ActionResetGroup) {
                    if (group_locked(*t, row.group)) {
                        play_click(false);
                        return;
                    }
                    if (row.group == GroupHack && t->flat_test && t->world_headset) {
                        const auto inverts = t->hack_headset.invert;
                        t->hack_headset = GroupTune{};
                        t->hack_headset.invert = inverts;
                    } else if (t->flat_test) {
                        t->flat_groups[row.group] = default_flat_group(row.group);
                        if (row.group == GroupReticle) {
                            t->screen[ScreenReticleDist] = HudTune{}.screen[ScreenReticleDist]; // Build 61
                        }
                    } else {
                        const auto inverts = t->groups[row.group].invert;
                        t->groups[row.group] = default_group(row.group);
                        t->groups[row.group].invert = inverts;
                    }
                    commit(t);
                    spdlog::info("[PragmataVR] Menu: reset {} ({} values)", GROUP_KEYS[row.group], hud_mode_name(t->hud_mode));
                    play_click(true);
                } else if (row.action == ActionRecenter) {
                    recenter_requested = true;
                    menu_place_requested = true;
                    spdlog::info("[PragmataVR] Menu: recenter HUD");
                    play_click(true);
                } else if (row.action == ActionSnapshot) {
                    request_snapshot();
                } else if (row.action == ActionSnapshotSeries) {
                    m_series_left = 5;
                    m_series_next = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    menu_open = false;
                    menu_editing = false;
                    menu_on_invert = false;
                    block_pad_briefly();
                    spdlog::info("[PragmataVR] Snapshot series: 5 snapshots starting in 3 s");
                    play_click(true);
                } else if (row.action == ActionClose) {
                    menu_open = false;
                    block_pad_briefly();
                    play_click(true);
                }
                return;
            default:
                return;
            }
        }

        if ((vk != VK_LEFT && vk != VK_RIGHT) || row.kind != MenuRow::Kind::Value || !editing) {
            return;
        }

        if (row.group == GROUP_MENU) {
            int mdir = vk == VK_RIGHT ? 1 : -1;
            if (row.field == MenuDistance) {
                mdir = -mdir; // Build 22: Right brings the menu closer (Steve had it marked INV)
            }
            if (t->menu_invert[row.field]) {
                mdir = -mdir;
            }
            auto& v = t->menu[row.field];
            const float nv = std::clamp(v + mdir * (shift ? MENU_STEP_BIG[row.field] : MENU_STEP[row.field]),
                MENU_MIN[row.field], MENU_MAX[row.field]);
            const bool mok = nv != v;
            v = nv;
            if (mok) {
                commit(t);
            }
            play_click(mok);
            return;
        }

        if (row.group == GROUP_ENEMY) {
            if (enemy_field_locked(*t, row.field)) {
                play_click(false);
                return;
            }
            int edir = vk == VK_RIGHT ? 1 : -1;
            if (t->enemy_bar_invert[row.field]) {
                edir = -edir;
            }
            auto& v = t->enemy_bar[row.field];
            const float nv = std::clamp(v + edir * (shift ? ENEMY_STEP_BIG : ENEMY_STEP), -ENEMY_MAX, ENEMY_MAX);
            const bool eok = nv != v;
            v = nv;
            if (eok) {
                commit(t);
            }
            spdlog::info("[PragmataVR] Menu: enemy health bar x={:.0f} y={:.0f}, damage numbers x={:.0f} y={:.0f}", t->enemy_bar[0], t->enemy_bar[1],
                t->enemy_bar[2], t->enemy_bar[3]);
            play_click(eok);
            return;
        }

        if (row.group == GROUP_SCREEN) {
            int sdir = vk == VK_RIGHT ? 1 : -1;
            if (t->screen_invert[row.field]) {
                sdir = -sdir;
            }
            auto& v = t->screen[row.field];
            const float nv = std::clamp(v + sdir * (shift ? SCREEN_STEP_BIG[row.field] : SCREEN_STEP[row.field]),
                SCREEN_MIN[row.field], SCREEN_MAX[row.field]);
            const bool sok = nv != v;
            v = nv;
            if (sok) {
                commit(t);
            }
            spdlog::info("[PragmataVR] Menu: screen width={:.0f} distance={:.2f} yaw={:.0f} pitch={:.0f}{}",
                t->screen[0], t->screen[1], t->screen[2], t->screen[3], sok ? "" : " (limit)");
            play_click(sok);
            return;
        }

        if (group_locked(*t, row.group)) {
            play_click(false);
            return;
        }

        auto& gt = active_group(*t, row.group);
        int dir = vk == VK_RIGHT ? 1 : -1;
        if (t->groups[row.group].invert[row.field]) {
            dir = -dir;
        }

        bool ok = true;
        const auto step = [&](float& v, float delta, float lo, float hi) {
            const float nv = std::clamp(v + delta, lo, hi);
            ok = nv != v;
            v = nv;
        };

        switch (row.field) {
        case FieldSize: {
            const float factor = shift ? SIZE_STEP_BIG : SIZE_STEP;
            const float nv = std::clamp(dir > 0 ? gt.size * factor : gt.size / factor, MIN_SIZE, MAX_SIZE);
            ok = nv != gt.size;
            gt.size = nv;
            break;
        }
        case FieldX: step(gt.x, dir * (shift ? MOVE_STEP_BIG : MOVE_STEP), -MAX_MOVE, MAX_MOVE); break;
        case FieldY: step(gt.y, dir * (shift ? MOVE_STEP_BIG : MOVE_STEP), -MAX_MOVE, MAX_MOVE); break;
        case FieldGap: step(gt.gap, dir * (shift ? GAP_STEP_BIG : GAP_STEP), -MAX_GAP, MAX_GAP); break;
        default: return;
        }

        if (ok) {
            commit(t);
        }

        spdlog::info("[PragmataVR] Menu: {} {} ({}) -> size={:.3f} x={:.1f} y={:.1f} gap={:.1f}{}",
            GROUP_KEYS[row.group], FIELD_KEYS[row.field], hud_mode_name(t->hud_mode), gt.size, gt.x, gt.y, gt.gap, ok ? "" : " (limit)");
        play_click(ok);
    }

    static constexpr float SIZE_STEP = 1.04f;
    static constexpr float SIZE_STEP_BIG = 1.20f;
    static constexpr float MIN_SIZE = 0.2f;
    static constexpr float MAX_SIZE = 8.0f;
    static constexpr float MOVE_STEP = 5.0f; // Build 60: 10 -> 5 (Steve: finer Left / right and Up / down steps)
    static constexpr float MOVE_STEP_BIG = 50.0f;
    static constexpr float MAX_MOVE = 4000.0f;
    static constexpr float GAP_STEP = 1.0f;
    static constexpr float GAP_STEP_BIG = 5.0f;
    static constexpr float MAX_GAP = 300.0f;

    std::mutex m_mtx{};
    std::shared_ptr<const HudTune> m_tune{};
    std::filesystem::file_time_type m_mtime{};
    std::chrono::steady_clock::time_point m_last_check{};
    bool m_loaded_once{false};
    std::unordered_map<size_t, bool> m_logged{};
    std::array<bool, KEYS.size()> m_key_down{};
    std::array<std::chrono::steady_clock::time_point, KEYS.size()> m_last_press{};
    std::vector<MenuRow> m_rows{};
    bool m_lt_prev{false}; // Build 36: trigger state for tab switching
    bool m_rt_prev{false};
    std::array<std::vector<int>, 2> m_nav{}; // selectable rows per column, top to bottom
    std::vector<float> m_row_y{};             // row center height within its band, in row units
    std::mutex m_head_mtx{};
    HeadState m_state{};
    Matrix4x4f m_lock{1.0f};
    bool m_anchor_valid{false};
    bool m_menu_valid{false};
    WORD m_pad_prev{0};
    int m_stick_prev{0};
    std::chrono::steady_clock::time_point m_last_chord{};
    std::chrono::steady_clock::time_point m_chord_start{}; // Build 58: hold to open
    bool m_chord_fired{true};
    bool m_chord_menu_was_open{false};
    std::atomic<int64_t> m_chord_replay_until_ms{0};
    std::atomic<std::chrono::steady_clock::time_point> m_block_pad_until{};
};

// A flat panel in tracking space: center, and right/up vectors scaled to the panel's half width / half height.
struct Plane {
    glm::vec3 center{0.0f};
    glm::vec3 right{0.0f};
    glm::vec3 up{0.0f};
};

// Panel `dist` meters from the anchor, turned yaw_deg right and pitch_deg up from the anchor's forward direction.
inline Plane make_plane(const HudTuning::Pose& anchor, float yaw_deg, float pitch_deg, float dist, float half_w, float half_h) {
    const glm::mat4 aim = glm::rotate(glm::mat4{1.0f}, -glm::radians(yaw_deg), glm::vec3{0.0f, 1.0f, 0.0f}) *
                          glm::rotate(glm::mat4{1.0f}, glm::radians(pitch_deg), glm::vec3{1.0f, 0.0f, 0.0f});
    const glm::mat4 r = anchor.rot * aim;

    Plane p{};
    p.center = anchor.pos + glm::vec3{r * glm::vec4{0.0f, 0.0f, -dist, 0.0f}};
    p.right = glm::vec3{r * glm::vec4{1.0f, 0.0f, 0.0f, 0.0f}} * half_w;
    p.up = glm::vec3{r * glm::vec4{0.0f, 1.0f, 0.0f, 0.0f}} * half_h;
    return p;
}

// Tracking space -> this eye's view space.
inline glm::mat4 eye_view(const HudTuning::Pose& head, const Matrix4x4f& eye_to_head) {
    const glm::mat4 head_world = glm::translate(glm::mat4{1.0f}, head.pos) * head.rot;
    return glm::inverse(head_world * glm::mat4{eye_to_head});
}

// Game-screen HUD: the game's flat GUI clip space (x, y in -1..1 over its own 16:9 layout) is mapped onto `plane`
// and projected into this eye: point = center + right * (x/w) + up * (y/w). As a matrix on flat clip coordinates
// that is B = [right, up, 0, center], so out = eye_proj * view * B * flat. Returns false if the panel's center is
// behind this eye.
inline bool screen_projection(Matrix4x4f& out, const Matrix4x4f& flat, const Matrix4x4f& eye_proj, const glm::mat4& view, const Plane& plane) {
    const glm::vec4 c = eye_proj * view * glm::vec4{plane.center, 1.0f};
    if (c.w <= 1e-4f) {
        return false;
    }

    glm::mat4 B{0.0f};
    B[0] = glm::vec4{plane.right, 0.0f};
    B[1] = glm::vec4{plane.up, 0.0f};
    B[2] = glm::vec4{0.0f};
    B[3] = glm::vec4{plane.center, 1.0f};

    out = eye_proj * view * B * glm::mat4{flat};
    return true;
}

// Game-screen HUD plane from the current settings and anchor.
inline Plane hud_plane(const HudTune& t, const HudTuning::Pose& anchor, const Matrix4x4f& flat) {
    const float dist = t.screen[ScreenDistance];
    const float half_w = dist * std::tan(glm::radians(t.screen[ScreenWidth]) * 0.5f);
    const float flat_aspect = flat[0][0] != 0.0f ? std::fabs(flat[1][1] / flat[0][0]) : (16.0f / 9.0f);
    return make_plane(anchor, t.screen[ScreenYaw], t.screen[ScreenPitch], dist, half_w, half_w / flat_aspect);
}

// Mod menu panel in tracking (stage) space for the OpenXR quad layer: `dist` meters in front of the anchor, tilted
// menu.pitch degrees, facing the player. Its size follows the menu image: body text MENU_FONT_PX pixels tall is shown
// MENU_TEXT_DEG * menu.scale degrees tall, so the panel always fits its text and never stretches it.
struct MenuQuadPlacement {
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f};
    float width_m{1.0f};
    float height_m{1.0f};
};

inline MenuQuadPlacement menu_quad_placement(const HudTune& t, const HudTuning::Pose& anchor, float image_w_px, float image_h_px) {
    const float dist = t.menu[MenuDistance];
    const float text_rad = glm::radians(MENU_TEXT_DEG * t.menu[MenuScale]);
    const float meters_per_px = (2.0f * dist * std::tan(text_rad * 0.5f)) / MENU_FONT_PX;

    const glm::mat4 r = anchor.rot * glm::rotate(glm::mat4{1.0f}, glm::radians(t.menu[MenuPitch]), glm::vec3{1.0f, 0.0f, 0.0f});

    MenuQuadPlacement out{};
    out.orientation = glm::normalize(glm::quat_cast(glm::mat3{r}));
    out.position = anchor.pos + glm::vec3{r * glm::vec4{0.0f, 0.0f, -dist, 0.0f}};
    out.width_m = image_w_px * meters_per_px;
    out.height_m = image_h_px * meters_per_px;
    return out;
}

// Build 17: the HUD panel quad in stage space: screen.width degrees wide at screen.distance meters, turned screen.yaw
// right and screen.pitch up from the HUD anchor, with the UI buffer's aspect.
inline MenuQuadPlacement hud_panel_placement(const HudTune& t, const HudTuning::Pose& anchor, float image_w_px, float image_h_px) {
    const float dist = t.screen[ScreenDistance];
    const float width_m = 2.0f * dist * std::tan(glm::radians(t.screen[ScreenWidth]) * 0.5f);
    const glm::mat4 aim = glm::rotate(glm::mat4{1.0f}, -glm::radians(t.screen[ScreenYaw]), glm::vec3{0.0f, 1.0f, 0.0f}) *
                          glm::rotate(glm::mat4{1.0f}, glm::radians(t.screen[ScreenPitch]), glm::vec3{1.0f, 0.0f, 0.0f});
    const glm::mat4 r = anchor.rot * aim;

    MenuQuadPlacement out{};
    out.orientation = glm::normalize(glm::quat_cast(glm::mat3{r}));
    out.position = anchor.pos + glm::vec3{r * glm::vec4{0.0f, 0.0f, -dist, 0.0f}};
    out.width_m = width_m;
    out.height_m = image_w_px > 0.0f ? width_m * image_h_px / image_w_px : width_m;
    return out;
}

// Build 25: the ANCHORED element areas in UI buffer pixels (w x h). Screen views fit the 16:9 canvas into the buffer
// width, centered vertically (checked against the Build 17-18 measurements). Each area gets the element's FLAT size
// (around FLAT_PIVOTS) and position; the eye gap is not used in ANCHORED.
struct AnchoredBox {
    int group{0};
    long x0{0}, y0{0}, x1{0}, y1{0}; // area in the UI buffer (captured and erased from the eyes)
    long ox{0}, oy{0};               // Build 38: where the area goes on the panel, relative to x0 / y0 (Left / right, Up / down)
    float dcx{0.0f}, dcy{0.0f};      // Build 56: center of the element's quad on the panel (UI buffer pixels)
    float dw{0.0f}, dh{0.0f};        // Build 56: size of the element's quad on the panel (UI buffer pixels, after Size)
};

// Build 28: the frame (VR frame count) each element last drew in FLAT / ANCHORED. Its box is only used while it draws,
// so menus and windows that hide the HUD are not cut.
inline std::array<std::atomic<int>, GroupCount> g_group_drawn_frame{};

// Build 55: last time (steady clock ms) a full-screen menu GUI (ui51xx) was drawn; render threads write, the frame reads.
inline std::atomic<int64_t> g_menu_drawn_ms{0};

// Build 55: a full-screen menu GUI: ui5100Gui to ui5199Gui. v0.9.0: also the start-up splash screens (ui1210Gui) and
// the RE Engine logo (ui0430_01Gui): before the first 3D scene AFW has no depth or motion to warp from, so they shook
// and doubled until the game's normal rendering started.
inline bool is_fullscreen_menu(std::string_view name) {
    if (name == "ui1210Gui" || name == "ui0430_01Gui") {
        return true;
    }
    return name.size() == 9 && name.compare(0, 4, "ui51") == 0 && name.compare(6, 3, "Gui") == 0;
}

// Build 39: where Fixed mode draws an element in the UI buffer, as a shift in canvas pixels from its own place (after
// its size). With fixed_capture_corners each element goes to a corner of the buffer: tutorial top left, weapon wheel top
// right, health bottom left, stamina bottom right. The corners of each eye's image are outside the lens view on any
// headset, so world HUD (reticle, enemy bars, hacking circle) drawn there is never visible anyway, and anything cut
// together with an element is invisible. The panel puts each element back at its tuned place (AnchoredBox ox / oy).
// Build 62: elements laid out against the bottom of the eye image (the subtitles) have their box and pivot in that frame;
// this converts to the centred canvas frame the rest of the code uses (canvas px).
inline float anchor_dy(int g, float w, float h) {
    if (g != GroupSubtitles || w <= 0.0f || h <= 0.0f) {
        return 0.0f;
    }
    const float s = (std::min)(w / 1920.0f, h / 1080.0f);
    const float oy = (h - 1080.0f * s) * 0.5f;
    return oy / s;
}

inline glm::vec2 fixed_capture_shift(const HudTune& t, int g, float w, float h) {
    if (!t.fixed_capture_corners || g <= GroupHack || g >= GroupCount || w <= 0.0f || h <= 0.0f) {
        return glm::vec2{0.0f};
    }
    // Build 56: elements are drawn at full size (Size is applied to their quads), so the areas use the full boxes.
    const float s = (std::min)(w / 1920.0f, h / 1080.0f);
    const float ox = (w - 1920.0f * s) * 0.5f;
    const float oy = (h - 1080.0f * s) * 0.5f;
    const auto b = box_for(g);
    const float cx0 = b[0], cy0 = b[1], cx1 = b[2], cy1 = b[3];
    constexpr float M = 4.0f; // buffer pixels from the edge
    constexpr float GAP = 8.0f;
    if (g == GroupItems) {
        // Top edge, just right of the tutorial box (top left corner); the weapon wheel takes the top right corner.
        const auto tb = box_for(GroupTutorial);
        const float left = M + s * (tb[2] - tb[0]) + GAP;
        return glm::vec2{(left - (ox + s * cx0)) / s, (M - (oy + s * cy0)) / s};
    }
    if (g == GroupObjective) {
        // Bottom edge, just right of health (bottom left corner).
        const auto hb = box_for(GroupHealth);
        const float left = M + s * (hb[2] - hb[0]) + GAP;
        return glm::vec2{(left - (ox + s * cx0)) / s, ((h - M) - (oy + s * cy1)) / s};
    }
    if (g == GroupStamina) {
        // Build 58: bottom left, just above health (health grew wider, so health + objective + stamina no longer fit
        // side by side on the bottom edge).
        const auto hb = box_for(GroupHealth);
        const float bottom_px = (h - M) - s * (hb[3] - hb[1]) - GAP;
        return glm::vec2{(M - (ox + s * cx0)) / s, (bottom_px - (oy + s * cy1)) / s};
    }
    if (g == GroupSubtitles) {
        // Build 62: cut at their own place (bottom-anchored, in the band below the game view). While they give way (the
        // objective's capture spot covers that place), they are lifted just above it and stay in the eye images.
        if (!g_subtitle_yield.load(std::memory_order_relaxed)) {
            return glm::vec2{0.0f};
        }
        const auto ob = box_for(GroupObjective);
        const float obj_top = (h - M) - s * (ob[3] - ob[1]);
        const float sub_bottom = oy + s * (cy1 + anchor_dy(g, w, h));
        return glm::vec2{0.0f, (std::min)(0.0f, (obj_top - GAP - sub_bottom) / s)};
    }
    // Build 61: the reticle takes the bottom right corner (free since stamina moved above health in Build 58).
    const bool right = g == GroupWeapons || g == GroupReticle;
    const bool bottom = g == GroupHealth || g == GroupReticle;
    const float tx = right ? ((w - M) - (ox + s * cx1)) / s : (M - (ox + s * cx0)) / s;
    const float ty = bottom ? ((h - M) - (oy + s * cy1)) / s : (M - (oy + s * cy0)) / s;
    return glm::vec2{tx, ty};
}

inline std::vector<AnchoredBox> anchored_boxes_px(const HudTune& t, float w, float h, int frame = -1) {
    std::vector<AnchoredBox> out{};
    const float s = (std::min)(w / 1920.0f, h / 1080.0f);
    const float ox = (w - 1920.0f * s) * 0.5f;
    const float oy = (h - 1080.0f * s) * 0.5f;
    // Build 62: the subtitles give way while the objective banner (its capture spot covers their place) or a window or
    // full-screen menu is shown: they are not cut then (the Riot Blaster window was cut in snapshot 1793546956).
    if (frame >= 0) {
        const int oage = frame - g_group_drawn_frame[GroupObjective].load();
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool yield = (oage >= 0 && oage <= 10) || now - g_window_drawn_ms.load(std::memory_order_relaxed) < 500 ||
                           now - g_menu_drawn_ms.load(std::memory_order_relaxed) < 500;
        if (g_subtitle_yield.exchange(yield, std::memory_order_relaxed) != yield) {
            static int logs = 0;
            if (++logs <= 40) {
                spdlog::info("[PragmataVR] Subtitles: {}", yield ? "give way (objective, window or menu shown)" : "own panel again");
            }
        }
    }
    for (int g = GroupHealth; g < GroupCount; ++g) {
        if (frame >= 0) {
            const int age = frame - g_group_drawn_frame[g].load();
            if (age < 0 || age > 10) { // Build 29: 10 frames (was 4)
                continue; // not drawn in the last few frames
            }
            if (g == GroupSubtitles && g_subtitle_yield.load(std::memory_order_relaxed)) {
                continue;
            }
        }
        auto b = box_for(g);
        const float ady = anchor_dy(g, w, h); // Build 62: bottom-anchored subtitles (capture area only)
        const auto db = b; // Build 63: the panel placement uses the centred frame, like every other element
        b[1] += ady;
        b[3] += ady;
        const auto& gt = t.flat_groups[g];
        const float px = FLAT_PIVOTS[g][0];
        const float py = FLAT_PIVOTS[g][1];
        // Build 56: the game draws the element at full size (its own layout, moved to its capture spot); this area is cut
        // out and shown on the element's own quad.
        const auto sh = fixed_capture_shift(t, g, w, h);
        AnchoredBox a{};
        a.group = g;
        a.x0 = (long)std::clamp(ox + s * (b[0] + sh.x), 0.0f, w);
        a.y0 = (long)std::clamp(oy + s * (b[1] + sh.y), 0.0f, h);
        a.x1 = (long)std::clamp(ox + s * (b[2] + sh.x), 0.0f, w);
        a.y1 = (long)std::clamp(oy + s * (b[3] + sh.y), 0.0f, h);
        // Where the quad goes on the panel (buffer pixels): the area as the Builds 21-55 in-game Size, Left / right and
        // Up / down put it (grown around the element's pivot), so existing tuning keeps its place.
        const float k = gt.size;
        const float bx0 = px + k * (db[0] - px) + gt.x;
        const float by0 = py + k * (db[1] - py) + gt.y;
        const float bx1 = px + k * (db[2] - px) + gt.x;
        const float by1 = py + k * (db[3] - py) + gt.y;
        a.dcx = ox + s * (bx0 + bx1) * 0.5f;
        a.dcy = oy + s * (by0 + by1) * 0.5f;
        a.dw = s * std::fabs(bx1 - bx0);
        a.dh = s * std::fabs(by1 - by0);
        a.ox = (long)std::lround(a.dcx - (a.x0 + a.x1) * 0.5f); // kept for the log
        a.oy = (long)std::lround(a.dcy - (a.y0 + a.y1) * 0.5f);
        if (a.x1 > a.x0 + 1 && a.y1 > a.y0 + 1) {
            out.push_back(a);
        }
    }
    return out;
}

// Build 61: capture area audit (logged when the UI buffer size changes). For each element: where its capture area sits in
// game canvas pixels (the 1920 x 1080 canvas fitted to the UI buffer width; the canvas itself is the part of the eye
// image the game's 16:9 view covers, anything outside it is the extra height of the headset image), how much of it lies
// inside the canvas, and any overlap between two elements' capture areas (there should be none).
inline std::string capture_audit(const HudTune& t, float w, float h) {
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }
    struct R { float x0, y0, x1, y1; int g; };
    std::vector<R> rs{};
    std::string out{};
    for (int g = GroupHealth; g < GroupCount; ++g) {
        for (int variant = 0; variant < (g == GroupItems ? 2 : 1); ++variant) {
            std::array<float, 4> b = FLAT_BOXES[g];
            if (g == GroupItems && variant == 1) {
                b = ITEMS_BOX_FIELD;
            }
            const auto sh = fixed_capture_shift(t, g, w, h);
            // The capture spot is placed from the box in use now (its top left corner for item pickups); each variant
            // keeps that corner and its own size.
            auto cb = box_for(g);
            cb[1] += anchor_dy(g, w, h); // Build 62
            cb[3] += anchor_dy(g, w, h);
            R r{cb[0] + sh.x, cb[1] + sh.y, cb[0] + sh.x + (b[2] - b[0]), cb[1] + sh.y + (b[3] - b[1]), g};
            const float ix0 = (std::max)(r.x0, 0.0f), iy0 = (std::max)(r.y0, 0.0f);
            const float ix1 = (std::min)(r.x1, 1920.0f), iy1 = (std::min)(r.y1, 1080.0f);
            const float area = (r.x1 - r.x0) * (r.y1 - r.y0);
            const float inside = (ix1 > ix0 && iy1 > iy0) ? (ix1 - ix0) * (iy1 - iy0) : 0.0f;
            out += fmt::format(" {}{} ({:.0f},{:.0f})-({:.0f},{:.0f}) {:.0f}% in canvas;", GROUP_KEYS[g], variant == 1 ? "(field)" : "",
                               r.x0, r.y0, r.x1, r.y1, area > 0.0f ? 100.0f * inside / area : 0.0f);
            if (variant == 0) {
                rs.push_back(r);
            }
        }
    }
    for (size_t i = 0; i < rs.size(); ++i) {
        for (size_t j = i + 1; j < rs.size(); ++j) {
            const auto& a = rs[i];
            const auto& c = rs[j];
            if (a.x0 < c.x1 && c.x0 < a.x1 && a.y0 < c.y1 && c.y0 < a.y1) {
                out += fmt::format(" OVERLAP {} / {};", GROUP_KEYS[a.g], GROUP_KEYS[c.g]);
            }
        }
    }
    return out;
}

inline void adjust_projection(Matrix4x4f& m, float zoom, float x, float y) {
    m[0][0] *= zoom;
    m[1][1] *= zoom;
    m[2][0] *= zoom;
    m[2][1] *= zoom;
    m[2][0] += x * m[2][3];
    m[2][1] += y * m[2][3];
}

} // namespace pragmatavr
