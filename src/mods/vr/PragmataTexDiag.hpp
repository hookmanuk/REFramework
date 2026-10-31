#pragma once

// PragmataVR Build 58: texture streaming diagnostic (logging only, changes nothing in the game).
//
// In VR many textures stay at low detail (Hugh's suit, the robots), while the same textures are sharp flat on a
// monitor. Before choosing a fix (redirect the base texture files to their full resolution streaming files, or change
// what the streamer asks for), this logs:
//   1. Texture file paths the engine resolves (through REFramework's existing loose file / texture loader hooks):
//      the first unique paths of each kind (with and without "streaming" in the path) and counts every 60 s.
//   2. Once, 20 s after the first frame: every type whose name mentions streaming, mips or LOD, its matching methods,
//      and for native singletons the values of their zero-argument getters with such names.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

#include <reframework/API.hpp> // reframework::InvokeRet (Build 58: needed where the loaders include this header)
#include "sdk/RETypeDB.hpp"
#include "sdk/REGlobals.hpp"
#include "sdk/REContext.hpp"
#include "utility/String.hpp"

namespace pragmata_tex_diag {

inline constexpr size_t SAMPLES_PER_KIND = 60;

enum Source : int {
    SourcePathHash = 0,     // LooseFileLoader::handle_path (every file path the game hashes for a pak lookup)
    SourceResourceHash = 1, // LooseTextureLoader::handle_resource_hash_path (resource paths with a known extension)
    SourceCount = 2,
};

inline std::array<std::array<std::atomic<uint64_t>, 2>, SourceCount> g_counts{}; // [source][streaming]
inline std::mutex g_mtx{};
inline std::array<std::array<std::unordered_set<std::wstring>, 2>, SourceCount> g_seen{};
inline std::atomic<bool> g_full{false};

inline bool contains_ci(std::wstring_view s, std::wstring_view what) {
    if (what.size() > s.size()) {
        return false;
    }
    for (size_t i = 0; i + what.size() <= s.size(); ++i) {
        size_t j = 0;
        for (; j < what.size(); ++j) {
            if (std::towlower(s[i + j]) != std::towlower(what[j])) {
                break;
            }
        }
        if (j == what.size()) {
            return true;
        }
    }
    return false;
}

// Build 61: path logging done (Build 58 showed base files natives/STM/<path>.tex.<ver> and streaming files
// natives/STM/streaming/<path>.tex.<ver>); off, so the loader threads do no extra work.
inline constexpr bool PATH_LOG = false;

// Loader threads. Cheap when the path is not a texture.
inline void note(std::wstring_view path, int source) {
    if (!PATH_LOG || path.empty() || source < 0 || source >= SourceCount || !contains_ci(path, L".tex")) {
        return;
    }
    const int streaming = contains_ci(path, L"streaming") ? 1 : 0;
    g_counts[source][streaming].fetch_add(1, std::memory_order_relaxed);
    if (g_full.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock _{g_mtx};
    auto& seen = g_seen[source][streaming];
    if (seen.size() >= SAMPLES_PER_KIND) {
        bool all_full = true;
        for (auto& src : g_seen) {
            for (auto& k : src) {
                all_full = all_full && k.size() >= SAMPLES_PER_KIND;
            }
        }
        g_full.store(all_full, std::memory_order_relaxed);
        return;
    }
    if (seen.emplace(path).second) {
        spdlog::info("[PragmataVR] Texture path ({}, {}): {}", source == SourcePathHash ? "pak lookup" : "resource",
                     streaming ? "streaming" : "base", utility::narrow(std::wstring{path}));
    }
}

inline bool name_matches(std::string_view n) {
    static constexpr std::array<std::string_view, 7> keys{"Stream", "stream", "Mip", "mip", "LOD", "Lod", "TextureQuality"};
    for (auto k : keys) {
        if (n.find(k) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

inline std::string value_of(const reframework::InvokeRet& r, const std::string& rn) {
    // Build 58: plain conversions (MSVC rejected fmt::format with these union members as a non-constant expression).
    if (rn == "System.Boolean") return r.byte != 0 ? std::string{"1"} : std::string{"0"};
    if (rn == "System.Single") {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%.3f", (double)r.f);
        return std::string{buf};
    }
    if (rn == "System.Int32") return std::to_string((int32_t)r.dword);
    if (rn == "System.UInt32") return std::to_string((uint32_t)r.dword);
    if (rn == "System.Int64" || rn == "System.UInt64") return std::to_string((uint64_t)r.qword);
    return std::to_string((uint32_t)r.dword) + " (enum or other)";
}

// Main thread, once.
inline void dump_types() {
    auto tdb = sdk::RETypeDB::get();
    if (tdb == nullptr) {
        spdlog::info("[PragmataVR] Texture streaming types: no type database");
        return;
    }
    int types = 0;
    int lines = 0;
    for (uint32_t i = 0; i < tdb->get_num_types() && lines < 600; ++i) {
        auto t = tdb->get_type(i);
        if (t == nullptr || t->get_name() == nullptr) {
            continue;
        }
        const auto full = t->get_full_name();
        const bool via_or_app = full.rfind("via.", 0) == 0 || full.rfind("app.", 0) == 0;
        if (!via_or_app) {
            continue;
        }
        const bool type_match = name_matches(full);
        const bool render = full.rfind("via.render.", 0) == 0;
        if (!type_match && !render) {
            continue;
        }

        std::vector<std::string> methods{};
        for (auto& m : t->get_methods()) {
            const char* mn = m.get_name();
            if (mn == nullptr || !name_matches(mn)) {
                continue;
            }
            auto rt = m.get_return_type();
            methods.push_back(fmt::format("{}{}({}):{}", m.is_static() ? "static " : "", mn, m.get_num_params(),
                                          rt != nullptr ? rt->get_full_name() : std::string{"?"}));
        }
        std::vector<std::string> fields{};
        for (auto f : t->get_fields()) {
            if (f == nullptr) {
                continue;
            }
            const char* fnm = f->get_name();
            if (fnm != nullptr && name_matches(fnm)) {
                auto ft = f->get_type();
                fields.push_back(fmt::format("{}:{}", fnm, ft != nullptr ? ft->get_full_name() : std::string{"?"}));
            }
        }
        if (!type_match && methods.empty() && fields.empty()) {
            continue;
        }
        ++types;

        std::string ml{};
        for (const auto& m : methods) {
            ml += " " + m;
        }
        std::string fl{};
        for (const auto& f : fields) {
            fl += " " + f;
        }
        spdlog::info("[PragmataVR] Texture streaming type {}: methods [{}] fields [{}]", full, ml, fl);
        ++lines;

        // Values: native singleton instance, zero-argument getters with matching names returning a primitive.
        void* inst = nullptr;
        try {
            inst = sdk::get_native_singleton<void>(full);
        } catch (...) {
            inst = nullptr;
        }
        std::string values{};
        for (auto& m : t->get_methods()) {
            const char* mn = m.get_name();
            if (mn == nullptr || m.get_num_params() != 0 || !name_matches(mn) || std::string_view{mn}.rfind("get_", 0) != 0) {
                continue;
            }
            if (!m.is_static() && inst == nullptr) {
                continue;
            }
            auto rt = m.get_return_type();
            if (rt == nullptr || !rt->is_value_type()) {
                continue;
            }
            const auto rn = rt->get_full_name();
            std::vector<void*> none{};
            const auto r = m.invoke(m.is_static() ? nullptr : inst, std::span<void*>(none));
            if (r.exception_thrown) {
                continue;
            }
            values += fmt::format(" {}={}", std::string_view{mn}.substr(4), value_of(r, rn));
        }
        if (!values.empty() || inst != nullptr) {
            spdlog::info("[PragmataVR] Texture streaming values {} (singleton {}):{}", full, inst != nullptr ? "yes" : "no",
                         values.empty() ? std::string{" (none)"} : values);
            ++lines;
        }
    }
    spdlog::info("[PragmataVR] Texture streaming types: {} listed", types);
}

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ==== Build 59: Sharp textures ====
// via.render.RenderConfig (from via.render.Renderer.get_RenderConfig) has StreamingTextureLoadLevelBias and
// MinimumStreamingTextureResolution (Build 58 type dump). With the toggle on, the minimum streaming resolution is set to
// the highest value the enum offers and the load level bias to 0; with it off, the game's own values are put back.
inline int parse_number(std::string_view n) {
    int best = -1;
    int cur = -1;
    for (char ch : n) {
        if (ch >= '0' && ch <= '9') {
            cur = (cur < 0 ? 0 : cur * 10) + (ch - '0');
            if (cur > 100000) cur = 100000;
        } else {
            best = (std::max)(best, cur);
            cur = -1;
        }
    }
    return (std::max)(best, cur);
}

inline void sharp_textures(bool on) {
    static bool failed = false;
    static bool init = false;
    static sdk::REMethodDefinition* get_rc = nullptr;
    static sdk::REMethodDefinition* get_bias = nullptr;
    static sdk::REMethodDefinition* set_bias = nullptr;
    static sdk::REMethodDefinition* get_min = nullptr;
    static sdk::REMethodDefinition* set_min = nullptr;
    static uint32_t target_min = 0;
    static bool have_target = false;
    static bool have_orig = false;
    static uint32_t orig_bias = 0;
    static uint32_t orig_min = 0;
    static bool applied = false;
    if (failed) {
        return;
    }
    if (!init) {
        init = true;
        auto renderer_t = sdk::find_type_definition("via.render.Renderer");
        auto rc_t = sdk::find_type_definition("via.render.RenderConfig");
        get_rc = renderer_t != nullptr ? renderer_t->get_method("get_RenderConfig") : nullptr;
        if (rc_t != nullptr) {
            get_bias = rc_t->get_method("get_StreamingTextureLoadLevelBias");
            set_bias = rc_t->get_method("set_StreamingTextureLoadLevelBias");
            get_min = rc_t->get_method("get_MinimumStreamingTextureResolution");
            set_min = rc_t->get_method("set_MinimumStreamingTextureResolution");
        }
        if (get_rc == nullptr || get_bias == nullptr || set_bias == nullptr || get_min == nullptr || set_min == nullptr) {
            spdlog::info("[PragmataVR] Sharp textures: render config methods not found, not available");
            failed = true;
            return;
        }
        // The enum's values: pick the one whose name has the biggest number (a resolution), else the biggest value.
        std::string list{};
        int best_num = -1;
        uint32_t best_val = 0;
        uint32_t max_val = 0;
        bool any = false;
        if (auto et = sdk::find_type_definition("via.render.RenderConfig.MinimumStreamingTextureResoltuion"); et != nullptr) {
            for (auto f : et->get_fields()) {
                if (f == nullptr || !f->is_static() || !f->is_literal() || f->get_init_data() == nullptr || f->get_name() == nullptr) {
                    continue;
                }
                const uint32_t v = *(const uint32_t*)f->get_init_data();
                const std::string_view nm{f->get_name()};
                list += fmt::format(" {}={}", nm, v);
                any = true;
                max_val = (std::max)(max_val, v);
                const int num = parse_number(nm);
                if (num > best_num) {
                    best_num = num;
                    best_val = v;
                }
            }
        }
        if (any) {
            target_min = best_num > 0 ? best_val : max_val;
            have_target = true;
        }
        spdlog::info("[PragmataVR] Sharp textures: minimum resolution values{} -> target {}", list.empty() ? std::string{" (none found)"} : list,
                     have_target ? std::to_string(target_min) : std::string{"none"});
    }

    auto renderer = sdk::get_native_singleton<void>("via.render.Renderer");
    if (renderer == nullptr) {
        return;
    }
    auto ctx = sdk::get_thread_context();
    auto rc = get_rc->call<::REManagedObject*>(ctx, renderer);
    if (rc == nullptr) {
        return;
    }
    const uint32_t bias = get_bias->call<uint32_t>(ctx, rc);
    const uint32_t mn = get_min->call<uint32_t>(ctx, rc);
    if (!have_orig) {
        have_orig = true;
        orig_bias = bias;
        orig_min = mn;
        spdlog::info("[PragmataVR] Sharp textures: game values: load level bias {}, minimum resolution {}", bias, mn);
    }
    if (on) {
        bool changed = false;
        if (bias != 0) {
            set_bias->call<void*>(ctx, rc, (uint32_t)0);
            changed = true;
        }
        if (have_target && mn != target_min) {
            set_min->call<void*>(ctx, rc, target_min);
            changed = true;
        }
        if (changed) {
            applied = true;
            static int logs = 0;
            if (++logs <= 10) {
                spdlog::info("[PragmataVR] Sharp textures: on (bias {} -> 0, minimum resolution {} -> {})", bias, mn, have_target ? target_min : mn);
            }
        }
    } else if (applied) {
        applied = false;
        set_bias->call<void*>(ctx, rc, orig_bias);
        set_min->call<void*>(ctx, rc, orig_min);
        spdlog::info("[PragmataVR] Sharp textures: off (game values back: bias {}, minimum resolution {})", orig_bias, orig_min);
    }
}

// Main thread, every frame (cheap): the one-time type dump after 20 s, and path counts every 60 s.
inline void tick() {
    static int64_t first = 0;
    static bool dumped = false;
    static int64_t last_report = 0;
    const auto now = now_ms();
    if (first == 0) {
        first = now;
        last_report = now;
    }
    if (!dumped && now - first > 20000) {
        dumped = true;
        dump_types();
    }
    if (PATH_LOG && now - last_report >= 60000) {
        last_report = now;
        spdlog::info("[PragmataVR] Texture paths, last 60 s: pak lookup base {} streaming {}, resource base {} streaming {}",
                     g_counts[SourcePathHash][0].exchange(0), g_counts[SourcePathHash][1].exchange(0),
                     g_counts[SourceResourceHash][0].exchange(0), g_counts[SourceResourceHash][1].exchange(0));
    }
}

} // namespace pragmata_tex_diag
