#pragma once

// PragmataVR Build 18: GPU trace, a diagnostic for removing the flat HUD elements from the eye images (option A).
//
// The engine draws its UI twice per frame: into the eye image and into its UI buffer (the HUD-only image the HUD panel
// is built from). To send health, stamina, weapon wheel and tutorial only to the UI buffer, we first need to know how
// those two writes happen: one draw writing both targets at once (multiple render targets), two separate draws, or a
// copy. This trace answers that at the D3D12 level, and dumps the engine's own render target setup for comparison.
//
// While a snapshot is taken, every OMSetRenderTargets for the next two eye frames is logged with the resources it binds
// (known ones named: UIBUF = the engine UI buffer, BACKBUF = the swapchain image) and the number of draws that follow
// on that command list; copies into or out of the UI buffer are logged too. RTV descriptors are mapped back to their
// resources by watching ID3D12Device::CreateRenderTargetView from the time the hooks are installed.
//
// Included from vr/D3D12Component.cpp inside its PRAGMATA anonymous namespace.

#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <optional>
#include <chrono>
#include <array>
#include <climits>
#include <string>

#include <utility/FunctionHook.hpp>

namespace pragmata_trace {
struct RtvInfo {
    ID3D12Resource* res{nullptr};
    uint32_t w{0};
    uint32_t h{0};
    uint32_t format{0};
};

struct Event {
    uint32_t tid{0};
    void* list{nullptr};
    char kind{'R'}; // R = render targets, C = copy
    uint32_t n{0};
    uint64_t handle[4]{};
    RtvInfo info[4]{};
    bool known[4]{};
    uint32_t draws{0};
    uint32_t frame{0};
};

inline std::unique_ptr<FunctionHook> g_create_rtv_hook{};
inline std::unique_ptr<FunctionHook> g_om_set_hook{};
inline std::unique_ptr<FunctionHook> g_draw_hook{};
inline std::unique_ptr<FunctionHook> g_draw_indexed_hook{};
inline std::unique_ptr<FunctionHook> g_copy_region_hook{};
inline std::unique_ptr<FunctionHook> g_copy_resource_hook{};
inline std::unique_ptr<FunctionHook> g_barrier_hook{};
inline std::unique_ptr<FunctionHook> g_clear_rtv_hook{};       // Build 30: diagnostics only
inline std::unique_ptr<FunctionHook> g_execute_indirect_hook{}; // Build 30: diagnostics only
inline std::unique_ptr<FunctionHook> g_execute_bundle_hook{};   // Build 30: diagnostics only

inline std::shared_mutex g_rtv_mtx{};
inline std::unordered_map<uint64_t, RtvInfo> g_rtvs{};
inline UINT g_rtv_increment{0};

inline std::atomic<bool> g_active{false};
inline std::atomic<uint32_t> g_frame{0};
inline std::mutex g_events_mtx{};
inline std::vector<Event> g_events{};
inline std::unordered_map<void*, size_t> g_last_event{}; // command list -> index of its last render target event
inline std::atomic<ID3D12Resource*> g_ui_buffer{nullptr};
inline std::atomic<uint64_t> g_creates_total{0};
inline std::atomic<uint64_t> g_ui_rtv_creates{0};

inline constexpr size_t MAX_EVENTS = 4000;

// Build 20 UI buffer hand-off state (see barrier()).
inline std::atomic<ID3D12Resource*> g_capture{nullptr};   // HUD panel source, kept in D3D12_RESOURCE_STATE_COMMON
inline std::atomic<bool> g_intercept{false};               // HUD panel mode on: capture at the hand-off
inline std::atomic<bool> g_erase{false};                   // Glued HUD REMOVED: clear the element areas after the copy
inline std::atomic<uint64_t> g_ui_rtv{0};                  // our own RTV of the UI buffer (for the clear)
inline std::atomic<uint64_t> g_handoffs{0};                // hand-offs seen (all time)
inline std::atomic<uint64_t> g_handoffs_window{0};         // hand-offs in the current 10 s report window
inline std::mutex g_rects_mtx{};
inline std::vector<D3D12_RECT> g_rects{};                  // element areas in UI buffer pixels

// Build 26: the engine hands out a different UI buffer texture from frame to frame (a pool). The overlay layer hook
// (VR::on_pre_overlay_layer_draw) reports each one as it is handed out (note_ui_buffer); the last few are remembered
// and any of them counts as the UI buffer at a hand-off. Each gets its own RTV (slot) for the erase.
inline constexpr int UI_RECENT = 8;
inline std::mutex g_ui_mtx{};
inline std::array<ID3D12Resource*, UI_RECENT> g_ui_recent{};
inline std::array<uint64_t, UI_RECENT> g_ui_recent_rtv{};  // our RTV for each remembered buffer (0 = not made yet)
inline int g_ui_recent_next{0};
inline std::atomic<uint64_t> g_ui_notes_window{0};         // overlay hook reports in the 10 s window
inline std::atomic<uint64_t> g_ui_new_window{0};           // reports of a buffer not seen among the recent ones
inline std::atomic<uint64_t> g_handoff_current_window{0};  // hand-offs of the buffer reported last
inline std::atomic<uint64_t> g_handoff_older_window{0};    // hand-offs of an older remembered buffer
inline std::atomic<uint64_t> g_handoff_skipped_window{0};
inline std::atomic<uint32_t> g_game_ui_w{0};
inline std::atomic<uint32_t> g_game_ui_h{0};
inline std::atomic<int> g_game_ui_fmt{0};
inline std::atomic<uint64_t> g_captures_window{0};         // hand-offs copied to the panel
inline std::atomic<uint64_t> g_drawn_handoffs_window{0};   // Build 29: hand-offs with draws since the previous one
inline std::atomic<uint64_t> g_fallback_window{0};         // Build 29: copies made by the every-pass fallback

// Build 31: pick the HUD capture by its content.
//
// Build 29 / 30 snapshots: the capture taken at a hand-off is often full (all four elements), but the panel built
// right before xrEndFrame was empty every time. The engine hands its UI buffer off about twice per eye frame and some
// of those passes are completely blank (not even the reticle; Build 30 run: a new pool texture almost every frame and
// one engine clear per frame). Which pass comes last before the panel is built is not the full one. Draw counting
// cannot tell them apart (the hooks never see the engine's UI draws: 0 draws, 0 ExecuteIndirect, 0 ExecuteBundle).
//
// So every hand-off is copied (element boxes only) into one of a few capture slots, and some rows of each element box
// are copied into a small CPU-readable buffer ("probes"). Two presents later a fence signalled on the game's queue
// says the GPU has finished, the probe rows are read, and each slot is marked with which elements actually have pixels.
// At the panel, each element is taken from the newest slot that really holds it and kept in a "good" texture, so the
// panel never shows a blank pass and keeps the last good image until a newer one arrives. An element that stops
// drawing leaves the panel through the existing 10-frame check (its box is dropped).
inline constexpr int SLOT_COUNT = 8;
inline constexpr int PROBE_ROWS_MAX = 24;
inline constexpr UINT64 PROBE_BYTES = 2400ull * 1024ull; // Build 58: 1600 -> 2400 KB (subtitles and bigger health / item pickups areas)
inline constexpr int PROBE_GROUPS = 16; // Build 61: 8 -> 16 (the reticle is group 8)
enum SlotState : int {
    SlotFree = 0,
    SlotPending = 1, // copy recorded, GPU result not known yet
    SlotReady = 2,   // probes read
    SlotInUse = 3,   // the panel is copying from it this frame
};
struct Probe {
    int group{0};
    UINT64 offset{0};
    UINT width{0};
    LONG x{0}; // Build 32: left edge and row of the probe in the UI buffer (for the content extent log)
    LONG y{0};
};
struct Slot {
    ID3D12Resource* res{nullptr};      // capture texture (owned by D3D12Component), kept in D3D12_RESOURCE_STATE_COMMON
    ID3D12Resource* readback{nullptr}; // probe rows (owned by D3D12Component), readback heap, always COPY_DEST
    const uint8_t* mapped{nullptr};
    int state{SlotFree};
    uint64_t seq{0};                   // hand-off order
    int presents{0};                   // presents since the copy was recorded
    uint64_t fence{0};                 // probe fence value that covers the copy (0 = not assigned yet)
    std::vector<D3D12_RECT> rects{};   // element boxes copied into res (same coordinates as the UI buffer)
    std::vector<int> groups{};
    std::vector<Probe> probes{};
    std::array<bool, PROBE_GROUPS> has{};
};
inline std::mutex g_slot_mtx{};
inline std::array<Slot, SLOT_COUNT> g_slots{};
inline uint64_t g_slot_seq{0};                             // guarded by g_slot_mtx
inline std::vector<int> g_rect_groups{};                   // group of each g_rects entry (guarded by g_rects_mtx)
inline Microsoft::WRL::ComPtr<ID3D12Fence> g_probe_fence{};
inline uint64_t g_probe_fence_value{0};                    // guarded by g_slot_mtx
inline std::atomic<uint64_t> g_probe_captures_window{0};   // hand-offs copied into a slot
inline std::atomic<uint64_t> g_probe_read_window{0};       // slots whose probes were read
inline std::atomic<uint64_t> g_probe_blank_window{0};      // ... with no element pixels at all
inline std::atomic<uint64_t> g_probe_evicted_window{0};    // slots reused before their probes were read
inline std::atomic<uint64_t> g_probe_noslot_window{0};     // hand-offs with no slot free
inline std::array<std::atomic<uint64_t>, PROBE_GROUPS> g_probe_has_window{};   // read slots holding each element
inline std::array<std::atomic<uint64_t>, PROBE_GROUPS> g_panel_update_window{}; // panel updates of each element
// Build 32: where each element's pixels actually were (UI buffer pixels, from the probe rows) over the 10 s window,
// next to its box, to size the boxes from measurements. Guarded by g_slot_mtx.
struct Extent {
    LONG x0{LONG_MAX}, y0{LONG_MAX}, x1{LONG_MIN}, y1{LONG_MIN};
    D3D12_RECT box{};
    bool any{false};
};
inline std::array<Extent, PROBE_GROUPS> g_extent_window{};
inline std::atomic<uint64_t> g_engine_clears_window{0};    // engine ClearRenderTargetView on a UI buffer
inline std::atomic<uint64_t> g_indirect_ui_window{0};      // ExecuteIndirect while a UI buffer is bound
inline std::atomic<uint64_t> g_bundle_ui_window{0};        // ExecuteBundle while a UI buffer is bound
inline std::atomic<uint64_t> g_draws_ui_window{0};         // DrawInstanced / DrawIndexedInstanced while a UI buffer is bound

// Called by D3D12Component when it (re)creates the capture textures and probe buffers. Pass nullptrs to stop.
inline void set_slots(const std::array<ID3D12Resource*, SLOT_COUNT>& res, const std::array<ID3D12Resource*, SLOT_COUNT>& readback) {
    std::scoped_lock _{g_slot_mtx};
    for (int i = 0; i < SLOT_COUNT; ++i) {
        g_slots[i] = Slot{};
        if (res[i] == nullptr || readback[i] == nullptr) {
            continue;
        }
        void* ptr = nullptr;
        D3D12_RANGE range{0, (SIZE_T)PROBE_BYTES};
        if (FAILED(readback[i]->Map(0, &range, &ptr)) || ptr == nullptr) {
            continue;
        }
        g_slots[i].res = res[i];
        g_slots[i].readback = readback[i];
        g_slots[i].mapped = (const uint8_t*)ptr;
    }
}

// Called on every present (each eye) from D3D12Component::on_frame, on the game's queue: signals the probe fence and
// assigns it to copies recorded two or more presents ago (the engine submits a frame's lists before presenting it).
inline void on_present(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (device == nullptr || queue == nullptr) {
        return;
    }
    if (g_probe_fence == nullptr) {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_probe_fence)))) {
            g_probe_fence.Reset();
            return;
        }
    }
    std::scoped_lock _{g_slot_mtx};
    const auto v = ++g_probe_fence_value;
    bool any = false;
    for (auto& sl : g_slots) {
        if (sl.state == SlotPending && sl.fence == 0 && ++sl.presents >= 2) {
            sl.fence = v;
            any = true;
        }
    }
    if (any) {
        queue->Signal(g_probe_fence.Get(), v);
    }
}

// A copy the panel should make: an element box from a slot into the good texture.
struct PanelCopy {
    ID3D12Resource* src{nullptr};
    D3D12_RECT rect{};
    int group{0};
};

// Called by the panel (before xrEndFrame): reads finished probes, then for each element in want_groups picks the
// newest slot that holds it. Slots chosen are kept from reuse until the next call (InUse); the rest are freed.
inline std::vector<PanelCopy> panel_copies(const std::vector<int>& want_groups) {
    std::vector<PanelCopy> out{};
    std::scoped_lock _{g_slot_mtx};
    for (auto& sl : g_slots) {
        if (sl.state == SlotInUse) {
            sl.state = SlotFree;
        }
    }
    const uint64_t done = g_probe_fence != nullptr ? g_probe_fence->GetCompletedValue() : 0;
    for (auto& sl : g_slots) {
        if (sl.state != SlotPending || sl.fence == 0 || done < sl.fence || sl.mapped == nullptr) {
            continue;
        }
        sl.has.fill(false);
        for (const auto& pr : sl.probes) {
            if (pr.group < 0 || pr.group >= PROBE_GROUPS) {
                continue;
            }
            const uint8_t* row = sl.mapped + pr.offset;
            LONG first = -1;
            LONG last = -1;
            for (UINT x = 0; x < pr.width; ++x) {
                if (row[x * 4 + 3] > 8) {
                    if (first < 0) {
                        first = (LONG)x;
                    }
                    last = (LONG)x;
                }
            }
            if (first >= 0) {
                sl.has[pr.group] = true;
                auto& e = g_extent_window[pr.group];
                e.x0 = (std::min)(e.x0, pr.x + first);
                e.x1 = (std::max)(e.x1, pr.x + last);
                e.y0 = (std::min)(e.y0, pr.y);
                e.y1 = (std::max)(e.y1, pr.y);
                e.any = true;
                for (size_t k = 0; k < sl.groups.size() && k < sl.rects.size(); ++k) {
                    if (sl.groups[k] == pr.group) {
                        e.box = sl.rects[k];
                    }
                }
            }
        }
        sl.state = SlotReady;
        g_probe_read_window++;
        bool any = false;
        for (int g = 0; g < PROBE_GROUPS; ++g) {
            if (sl.has[g]) {
                g_probe_has_window[g]++;
                any = true;
            }
        }
        if (!any) {
            g_probe_blank_window++;
        }
    }
    for (const int g : want_groups) {
        int best = -1;
        for (int i = 0; i < SLOT_COUNT; ++i) {
            const auto& sl = g_slots[i];
            if ((sl.state == SlotReady || sl.state == SlotInUse) && g >= 0 && g < PROBE_GROUPS && sl.has[g] &&
                (best < 0 || sl.seq > g_slots[best].seq)) {
                best = i;
            }
        }
        if (best < 0) {
            continue;
        }
        auto& sl = g_slots[best];
        for (size_t k = 0; k < sl.groups.size() && k < sl.rects.size(); ++k) {
            if (sl.groups[k] == g) {
                out.push_back(PanelCopy{sl.res, sl.rects[k], g});
                g_panel_update_window[g]++;
                break;
            }
        }
        sl.state = SlotInUse;
    }
    for (auto& sl : g_slots) {
        if (sl.state == SlotReady) {
            sl.state = SlotFree;
        }
    }
    return out;
}

// Build 29: which pass of the UI buffer holds the HUD varies from frame to frame (some passes see it wiped). So the
// draws made into each UI buffer are counted (OMSetRenderTargets binding a remembered UI buffer, then draws on that
// command list) and the panel copy is taken only at a hand-off that had draws since the previous hand-off. If no such
// hand-off has been seen for a second (draws the hooks do not see), every pass is copied again (the Build 27 way).
struct UiDraws {
    ID3D12Resource* res{nullptr};
    uint64_t draws{0};
};
inline std::mutex g_draws_mtx{};
inline std::array<UiDraws, 16> g_ui_draws{};
inline int g_ui_draws_next{0};
inline thread_local ID3D12GraphicsCommandList* t_ui_list{nullptr};
inline thread_local ID3D12Resource* t_ui_res{nullptr};
inline std::atomic<int64_t> g_last_drawn_handoff_ms{0};

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void add_ui_draw(ID3D12Resource* r) {
    std::scoped_lock _{g_draws_mtx};
    for (auto& d : g_ui_draws) {
        if (d.res == r) {
            d.draws++;
            return;
        }
    }
    auto& d = g_ui_draws[g_ui_draws_next];
    g_ui_draws_next = (g_ui_draws_next + 1) % (int)g_ui_draws.size();
    d.res = r;
    d.draws = 1;
}

// Draws made into r since the last call for r (then reset).
inline uint64_t take_ui_draws(ID3D12Resource* r) {
    std::scoped_lock _{g_draws_mtx};
    for (auto& d : g_ui_draws) {
        if (d.res == r) {
            const auto n = d.draws;
            d.draws = 0;
            return n;
        }
    }
    return 0;
}  // hand-offs not captured (size differs from the capture)

// Build 19: the Glued HUD removal. When a render target set binds the UI buffer together with other targets (the
// engine writing a UI draw into the eye image and the UI buffer at once), the other targets are swapped for throwaway
// textures of the same size and format, so that UI reaches only the UI buffer.
inline std::atomic<bool> g_redirect{false};
inline std::atomic<uint64_t> g_redirected_sets{0};
inline std::atomic<uint64_t> g_ui_alone_sets{0};
inline std::atomic<uint64_t> g_ui_unknown_sets{0};
inline std::atomic<uint64_t> g_ui_mrt_sets{0};
inline ID3D12Device* g_device{nullptr};

struct Scratch {
    Microsoft::WRL::ComPtr<ID3D12Resource> tex{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};
inline std::mutex g_scratch_mtx{};
inline Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_scratch_heap{};
inline uint32_t g_scratch_used{0};
inline std::unordered_map<uint64_t, Scratch> g_scratch{}; // key: width, height, format
inline constexpr uint32_t SCRATCH_MAX = 32;

inline void STDMETHODCALLTYPE create_rtv(ID3D12Device* self, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle);

inline std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> scratch_rtv(const RtvInfo& info) {
    if (g_device == nullptr || info.w == 0 || info.h == 0 || info.format == 0) {
        return std::nullopt;
    }
    const uint64_t key = ((uint64_t)info.w << 40) ^ ((uint64_t)info.h << 16) ^ (uint64_t)info.format;
    std::scoped_lock _{g_scratch_mtx};
    auto it = g_scratch.find(key);
    if (it != g_scratch.end()) {
        return it->second.rtv;
    }
    if (g_scratch_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = SCRATCH_MAX;
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_scratch_heap)))) {
            return std::nullopt;
        }
    }
    if (g_scratch_used >= SCRATCH_MAX) {
        return std::nullopt;
    }
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = info.w;
    d.Height = info.h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = (DXGI_FORMAT)info.format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Scratch sc{};
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&sc.tex)))) {
        spdlog::error("[PragmataVR] Glued HUD: could not create a {} x {} format {} throwaway target", info.w, info.h, info.format);
        g_scratch[key] = Scratch{}; // do not retry every draw
        return std::nullopt;
    }
    sc.rtv = g_scratch_heap->GetCPUDescriptorHandleForHeapStart();
    sc.rtv.ptr += (SIZE_T)g_scratch_used * g_rtv_increment;
    ++g_scratch_used;
    // Created through the original function so the trace map does not record it.
    if (g_create_rtv_hook != nullptr) {
        g_create_rtv_hook->get_original<decltype(create_rtv)>()(g_device, sc.tex.Get(), nullptr, sc.rtv);
    } else {
        g_device->CreateRenderTargetView(sc.tex.Get(), nullptr, sc.rtv);
    }
    spdlog::info("[PragmataVR] Glued HUD: throwaway target {} x {} format {} ready", info.w, info.h, info.format);
    g_scratch[key] = sc;
    return sc.rtv;
}

inline void report_redirects() {
    // Build 34: one line a minute (was two or three lines every 10 s).
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(60)) {
        return;
    }
    last = now;
    g_redirected_sets = 0;
    g_ui_alone_sets = 0;
    g_ui_unknown_sets = 0;
    g_ui_mrt_sets = 0;
    g_ui_notes_window = 0;
    g_ui_new_window = 0;
    g_handoff_current_window = 0;
    g_handoff_older_window = 0;
    g_captures_window = 0;
    g_drawn_handoffs_window = 0;
    g_fallback_window = 0;
    g_engine_clears_window = 0;
    g_indirect_ui_window = 0;
    g_bundle_ui_window = 0;
    g_draws_ui_window = 0;
    const auto ho = g_handoffs_window.exchange(0);
    const auto skipped = g_handoff_skipped_window.exchange(0);
    const auto pc = g_probe_captures_window.exchange(0);
    const auto pr = g_probe_read_window.exchange(0);
    const auto pb = g_probe_blank_window.exchange(0);
    const auto pe = g_probe_evicted_window.exchange(0);
    const auto pn = g_probe_noslot_window.exchange(0);
    std::array<uint64_t, PROBE_GROUPS> upd{};
    for (int g = 0; g < PROBE_GROUPS; ++g) {
        g_probe_has_window[g] = 0;
        upd[g] = g_panel_update_window[g].exchange(0);
    }
    {
        std::scoped_lock _{g_slot_mtx};
        for (auto& e : g_extent_window) {
            e = Extent{};
        }
    }
    if (!g_intercept.load() && ho == 0) {
        return; // Follows Head mode: nothing to report
    }
    static int reports = 0;
    if (reports < 120) { // two hours
        ++reports;
        spdlog::info("[PragmataVR] Fixed HUD, last 60 s: {} hand-offs ({} not the in-game buffer), {} captured, {} checked ({} blank), "
            "{} reused before checked, {} with no slot free; panel updates health {}, stamina {}, weapons {}, tutorial {}, "
            "items {}, objective {}, subtitles {}, reticle {}",
            ho, skipped, pc, pr, pb, pe, pn, upd[1], upd[2], upd[3], upd[4], upd[5], upd[6], upd[7], upd[8]);
    }
}

inline void STDMETHODCALLTYPE create_rtv(ID3D12Device* self, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    auto original = g_create_rtv_hook->get_original<decltype(create_rtv)>();
    original(self, res, desc, handle);

    RtvInfo info{};
    info.res = res;
    if (res != nullptr) {
        const auto d = res->GetDesc();
        info.w = (uint32_t)d.Width;
        info.h = d.Height;
        info.format = desc != nullptr ? (uint32_t)desc->Format : (uint32_t)d.Format;
        if (res == g_ui_buffer.load()) {
            g_ui_rtv_creates++;
        }
    }
    g_creates_total++;
    std::unique_lock _{g_rtv_mtx};
    g_rtvs[handle.ptr] = info;
}

inline void STDMETHODCALLTYPE om_set(ID3D12GraphicsCommandList* self, UINT num, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    auto original = g_om_set_hook->get_original<decltype(om_set)>();

    // Build 29: is a remembered UI buffer (in-game size) being bound on this command list? Then its draws are counted.
    if (g_intercept.load()) {
        ID3D12Resource* bound = nullptr;
        if (rtvs != nullptr && num >= 1 && num <= 8) {
            const auto gw = g_game_ui_w.load();
            const auto gh = g_game_ui_h.load();
            std::shared_lock _{g_rtv_mtx};
            for (UINT i = 0; i < num && bound == nullptr; ++i) {
                const uint64_t h = single ? rtvs[0].ptr + (uint64_t)i * g_rtv_increment : rtvs[i].ptr;
                auto it = g_rtvs.find(h);
                if (it != g_rtvs.end() && it->second.res != nullptr && it->second.w == gw && it->second.h == gh) {
                    std::scoped_lock __{g_ui_mtx};
                    for (auto x : g_ui_recent) {
                        if (x == it->second.res) {
                            bound = x;
                            break;
                        }
                    }
                }
            }
        }
        if (bound != nullptr) {
            t_ui_list = self;
            t_ui_res = bound;
        } else if (t_ui_list == self) {
            t_ui_list = nullptr;
            t_ui_res = nullptr;
        }
    }

    // Build 19: Glued HUD removal (see g_redirect).
    if (rtvs != nullptr && num >= 1 && num <= 8 && g_ui_buffer.load() != nullptr) {
        D3D12_CPU_DESCRIPTOR_HANDLE handles[8]{};
        RtvInfo infos[8]{};
        bool has_ui = false;
        bool all_known = true;
        {
            std::shared_lock _{g_rtv_mtx};
            for (UINT i = 0; i < num; ++i) {
                handles[i].ptr = single ? rtvs[0].ptr + (SIZE_T)i * g_rtv_increment : rtvs[i].ptr;
                auto it = g_rtvs.find(handles[i].ptr);
                if (it != g_rtvs.end()) {
                    infos[i] = it->second;
                    has_ui = has_ui || infos[i].res == g_ui_buffer.load();
                } else {
                    all_known = false;
                }
            }
        }
        if (has_ui && num == 1) {
            g_ui_alone_sets++;
        } else if (has_ui && !all_known) {
            g_ui_unknown_sets++;
        } else if (has_ui && g_redirect.load()) {
            bool ok = true;
            for (UINT i = 0; i < num && ok; ++i) {
                if (infos[i].res != g_ui_buffer.load()) {
                    auto sc = scratch_rtv(infos[i]);
                    if (sc) {
                        handles[i] = *sc;
                    } else {
                        ok = false;
                    }
                }
            }
            if (ok) {
                g_redirected_sets++;
                if (g_active.load()) {
                    Event e{};
                    e.tid = GetCurrentThreadId();
                    e.list = self;
                    e.kind = 'X'; // redirected
                    e.n = num;
                    e.frame = g_frame.load();
                    std::scoped_lock _{g_events_mtx};
                    if (g_events.size() < MAX_EVENTS) {
                        g_events.push_back(e);
                    }
                }
                original(self, num, handles, FALSE, dsv);
                return;
            }
        }
        if (has_ui && num > 1 && all_known) {
            g_ui_mrt_sets++;
        }
    }

    if (g_active.load() && rtvs != nullptr) {
        Event e{};
        e.tid = GetCurrentThreadId();
        e.list = self;
        e.kind = 'R';
        e.n = num;
        e.frame = g_frame.load();
        {
            std::shared_lock _{g_rtv_mtx};
            for (UINT i = 0; i < num && i < 4; ++i) {
                const uint64_t h = single ? rtvs[0].ptr + (uint64_t)i * g_rtv_increment : rtvs[i].ptr;
                e.handle[i] = h;
                auto it = g_rtvs.find(h);
                if (it != g_rtvs.end()) {
                    e.info[i] = it->second;
                    e.known[i] = true;
                }
            }
        }
        std::scoped_lock _{g_events_mtx};
        if (g_events.size() < MAX_EVENTS) {
            g_events.push_back(e);
            g_last_event[self] = g_events.size() - 1;
        }
    }

    original(self, num, rtvs, single, dsv);
}

inline void count_draw(ID3D12GraphicsCommandList* self) {
    if (!g_active.load()) {
        return;
    }
    std::scoped_lock _{g_events_mtx};
    auto it = g_last_event.find(self);
    if (it != g_last_event.end() && it->second < g_events.size()) {
        g_events[it->second].draws++;
    }
}

inline void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, UINT d) {
    if (t_ui_list == self && t_ui_res != nullptr) {
        add_ui_draw(t_ui_res);
        g_draws_ui_window++;
    }
    count_draw(self);
    g_draw_hook->get_original<decltype(draw)>()(self, a, b, c, d);
}

inline void STDMETHODCALLTYPE draw_indexed(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (t_ui_list == self && t_ui_res != nullptr) {
        add_ui_draw(t_ui_res);
        g_draws_ui_window++;
    }
    count_draw(self);
    g_draw_indexed_hook->get_original<decltype(draw_indexed)>()(self, a, b, c, d, e);
}

inline bool is_recent_ui_buffer(ID3D12Resource* r);

// Build 30 diagnostics: how does the engine draw its UI? Builds 18-29 never saw a DrawInstanced / DrawIndexedInstanced
// while the UI buffer was bound. These count the other ways (and engine clears of the UI buffer) for the log line.
inline void STDMETHODCALLTYPE execute_indirect(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* sig, UINT max_count,
    ID3D12Resource* args, UINT64 args_offset, ID3D12Resource* count_buf, UINT64 count_offset) {
    if (t_ui_list == self && t_ui_res != nullptr) {
        g_indirect_ui_window++;
    }
    g_execute_indirect_hook->get_original<decltype(execute_indirect)>()(self, sig, max_count, args, args_offset, count_buf, count_offset);
}

inline void STDMETHODCALLTYPE execute_bundle(ID3D12GraphicsCommandList* self, ID3D12GraphicsCommandList* bundle) {
    if (t_ui_list == self && t_ui_res != nullptr) {
        g_bundle_ui_window++;
    }
    g_execute_bundle_hook->get_original<decltype(execute_bundle)>()(self, bundle);
}

inline void STDMETHODCALLTYPE clear_rtv(ID3D12GraphicsCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT color[4], UINT n,
    const D3D12_RECT* rects) {
    if (g_intercept.load()) {
        ID3D12Resource* res = nullptr;
        {
            std::shared_lock _{g_rtv_mtx};
            auto it = g_rtvs.find(h.ptr);
            if (it != g_rtvs.end()) {
                res = it->second.res;
            }
        }
        if (res != nullptr && is_recent_ui_buffer(res)) {
            g_engine_clears_window++;
        }
    }
    g_clear_rtv_hook->get_original<decltype(clear_rtv)>()(self, h, color, n, rects);
}

inline void log_copy(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, ID3D12Resource* src) {
    if (!g_active.load()) {
        return;
    }
    const auto ui = g_ui_buffer.load();
    if (ui == nullptr || (dst != ui && src != ui)) {
        return;
    }
    Event e{};
    e.tid = GetCurrentThreadId();
    e.list = self;
    e.kind = 'C';
    e.n = 2;
    e.frame = g_frame.load();
    for (int i = 0; i < 2; ++i) {
        auto r = i == 0 ? dst : src;
        e.info[i].res = r;
        e.known[i] = r != nullptr;
        if (r != nullptr) {
            const auto d = r->GetDesc();
            e.info[i].w = (uint32_t)d.Width;
            e.info[i].h = d.Height;
            e.info[i].format = (uint32_t)d.Format;
        }
    }
    std::scoped_lock _{g_events_mtx};
    if (g_events.size() < MAX_EVENTS) {
        g_events.push_back(e);
    }
}

inline void STDMETHODCALLTYPE copy_region(ID3D12GraphicsCommandList* self, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    log_copy(self, dst != nullptr ? dst->pResource : nullptr, src != nullptr ? src->pResource : nullptr);
    g_copy_region_hook->get_original<decltype(copy_region)>()(self, dst, x, y, z, src, box);
}

inline void STDMETHODCALLTYPE copy_resource(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, ID3D12Resource* src) {
    log_copy(self, dst, src);
    g_copy_resource_hook->get_original<decltype(copy_resource)>()(self, dst, src);
}

// Build 20: the UI buffer hand-off. The engine draws its UI into the UI buffer on its own, then turns the buffer from a
// render target into a shader input and blends it onto the eye image (the "glued" HUD). At that state change, while
// the buffer still holds the whole UI, it is copied to g_capture (the HUD panel's source) and the HUD elements' areas
// are cleared from it (g_erase), so they reach the HUD panel but not the eye images. Recorded on the engine's own
// command list, so it runs at exactly that point in the frame.

// Build 27: a ring of RTV slots. A fresh RTV is written for the UI buffer at every hand-off (Build 26 cached one per
// address, and when the engine freed a texture and reused its address the cached RTV pointed at the freed texture:
// crash in the driver at startup). ClearRenderTargetView reads the descriptor when it is recorded, so a slot can be
// reused once the ring comes round.
inline constexpr int RTV_RING = 16;
inline Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_ui_rtv_heap{};
inline UINT g_ui_rtv_step{0};
inline std::atomic<uint32_t> g_ui_rtv_next{0};

// The in-game UI buffer size and format: the largest render-target 2D texture the overlay hook has reported (menus use
// a smaller desktop-size one). Read from the texture in the hook, while it is certainly alive.
// (g_game_ui_w / g_game_ui_h / g_game_ui_fmt are declared with the other hand-off state near the top.)

// Build 28: the engine passes the UI buffer through RENDER_TARGET -> other more than once per frame, and only the pass
// right after the overlay hook hands the buffer out still holds the HUD (a later pass sees it wiped). The capture is
// taken only on that first pass: note_ui_buffer arms the buffer, the first hand-off of it disarms it.
inline std::atomic<ID3D12Resource*> g_ui_armed{nullptr};

// Called from the overlay layer hook with the UI buffer the engine is handing out right now.
inline void note_ui_buffer(ID3D12Resource* r) {
    if (r == nullptr) {
        return;
    }
    g_ui_notes_window++;
    g_ui_buffer = r;
    g_ui_armed = r;
    const auto d = r->GetDesc();
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
        (uint64_t)d.Width * d.Height > (uint64_t)g_game_ui_w.load() * g_game_ui_h.load()) {
        g_game_ui_fmt = (int)d.Format;
        g_game_ui_h = d.Height;
        g_game_ui_w = (uint32_t)d.Width;
    }
    std::scoped_lock _{g_ui_mtx};
    for (auto x : g_ui_recent) {
        if (x == r) {
            return;
        }
    }
    g_ui_new_window++;
    g_ui_recent[g_ui_recent_next] = r;
    g_ui_recent_next = (g_ui_recent_next + 1) % UI_RECENT;
}

inline bool is_recent_ui_buffer(ID3D12Resource* r) {
    std::scoped_lock _{g_ui_mtx};
    for (auto x : g_ui_recent) {
        if (x == r) {
            return true;
        }
    }
    return false;
}

// A fresh RTV for r (typed with its own format), or 0.
inline uint64_t fresh_rtv(ID3D12Resource* r, DXGI_FORMAT format) {
    if (g_device == nullptr) {
        return 0;
    }
    {
        std::scoped_lock _{g_ui_mtx};
        if (g_ui_rtv_heap == nullptr) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = RTV_RING;
            if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_ui_rtv_heap)))) {
                g_ui_rtv_heap.Reset();
                return 0;
            }
            g_ui_rtv_step = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
    }
    auto h = g_ui_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)(g_ui_rtv_next.fetch_add(1) % RTV_RING) * g_ui_rtv_step;
    D3D12_RENDER_TARGET_VIEW_DESC vd{};
    vd.Format = format;
    vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    if (g_create_rtv_hook != nullptr) {
        g_create_rtv_hook->get_original<decltype(create_rtv)>()(g_device, r, &vd, h);
    } else {
        g_device->CreateRenderTargetView(r, &vd, h);
    }
    return (uint64_t)h.ptr;
}

inline void STDMETHODCALLTYPE barrier(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* b);

inline void transition(ID3D12GraphicsCommandList* self, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
    ID3D12Resource* r2 = nullptr, D3D12_RESOURCE_STATES from2 = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATES to2 = D3D12_RESOURCE_STATE_COMMON) {
    D3D12_RESOURCE_BARRIER br[2]{};
    for (int i = 0; i < 2; ++i) {
        br[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br[i].Transition.pResource = i == 0 ? r : r2;
        br[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br[i].Transition.StateBefore = i == 0 ? from : from2;
        br[i].Transition.StateAfter = i == 0 ? to : to2;
    }
    g_barrier_hook->get_original<decltype(barrier)>()(self, r2 != nullptr ? 2u : 1u, br);
}

inline void raw_copy_region(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, ID3D12Resource* src, const D3D12_RECT& r) {
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{(UINT)r.left, (UINT)r.top, 0u, (UINT)r.right, (UINT)r.bottom, 1u};
    if (g_copy_region_hook != nullptr) {
        g_copy_region_hook->get_original<decltype(copy_region)>()(self, &d, (UINT)r.left, (UINT)r.top, 0, &s, &box);
    } else {
        self->CopyTextureRegion(&d, (UINT)r.left, (UINT)r.top, 0, &s, &box);
    }
}

inline void STDMETHODCALLTYPE barrier(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    auto original = g_barrier_hook->get_original<decltype(barrier)>();

    if (!g_intercept.load() || b == nullptr) {
        original(self, n, b);
        return;
    }

    bool handoff = false;
    ID3D12Resource* ui = nullptr;
    for (UINT i = 0; i < n; ++i) {
        const auto& t = b[i];
        if (t.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || t.Transition.pResource == nullptr) {
            continue;
        }
        const bool from_rt = t.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET;
        const bool to_rt = t.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (from_rt && !to_rt && !handoff && is_recent_ui_buffer(t.Transition.pResource)) {
            handoff = true;
            ui = t.Transition.pResource;
        }
    }

    if (handoff) {
        g_handoffs++;
        g_handoffs_window++;
        if (ui == g_ui_buffer.load()) {
            g_handoff_current_window++;
        } else {
            g_handoff_older_window++;
        }
        // Build 27: only the in-game UI buffer (2D render target with the capture's size and format) is captured or
        // cleared. The texture is the one being transitioned, so it is alive here.
        auto cap0 = g_capture.load();
        const auto du = ui->GetDesc();
        bool game_ui = cap0 != nullptr && du.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                       (du.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && du.MipLevels == 1 && du.DepthOrArraySize == 1;
        if (game_ui) {
            const auto dc = cap0->GetDesc();
            game_ui = dc.Width == du.Width && dc.Height == du.Height && dc.Format == du.Format;
        }
        if (!game_ui) {
            g_handoff_skipped_window++;
        } else {
            if (take_ui_draws(ui) > 0) {
                g_drawn_handoffs_window++;
            }

            std::vector<D3D12_RECT> rects{};
            std::vector<int> groups{};
            {
                std::scoped_lock _{g_rects_mtx};
                rects = g_rects;
                groups = g_rect_groups;
            }
            if (groups.size() != rects.size()) {
                groups.assign(rects.size(), -1);
            }

            // Build 31: copy the element boxes into a free slot and probe rows into its readback buffer.
            Slot* slot = nullptr;
            if (!rects.empty()) {
                std::scoped_lock _{g_slot_mtx};
                int pick = -1;
                for (int pass = 0; pass < 3 && pick < 0; ++pass) {
                    const int want = pass == 0 ? SlotFree : (pass == 1 ? SlotPending : SlotReady);
                    for (int i = 0; i < SLOT_COUNT; ++i) {
                        const auto& sl = g_slots[i];
                        if (sl.res == nullptr || sl.state != want) {
                            continue;
                        }
                        if (pick < 0 || sl.seq < g_slots[pick].seq) {
                            pick = i;
                        }
                    }
                }
                if (pick < 0) {
                    g_probe_noslot_window++;
                } else {
                    auto& sl = g_slots[pick];
                    if (sl.state == SlotPending) {
                        g_probe_evicted_window++;
                    }
                    sl.state = SlotPending;
                    sl.seq = ++g_slot_seq;
                    sl.presents = 0;
                    sl.fence = 0;
                    sl.rects = rects;
                    sl.groups = groups;
                    sl.has.fill(false);
                    sl.probes.clear();
                    UINT64 off = 0;
                    for (size_t k = 0; k < rects.size(); ++k) {
                        const auto& r = rects[k];
                        const LONG h = r.bottom - r.top;
                        const UINT w = (UINT)(r.right - r.left);
                        if (h <= 0 || w == 0) {
                            continue;
                        }
                        const UINT64 pitch = ((UINT64)w * 4 + 255) & ~255ull;
                        const int rows = std::clamp((int)(h / 6), 1, PROBE_ROWS_MAX);
                        for (int j = 0; j < rows; ++j) {
                            off = (off + 511) & ~511ull;
                            if (off + pitch > PROBE_BYTES) {
                                break;
                            }
                            Probe pr{};
                            pr.group = groups[k];
                            pr.offset = off;
                            pr.width = w;
                            pr.x = r.left;
                            pr.y = r.top + (LONG)(((float)j + 0.5f) * (float)h / (float)rows);
                            sl.probes.push_back(pr);
                            off += pitch;
                        }
                    }
                    slot = &sl;
                }
            }

            if (slot != nullptr) {
                g_captures_window++;
                g_probe_captures_window++;
                auto cap = slot->res;
                transition(self, ui, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    cap, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                for (const auto& r : rects) {
                    raw_copy_region(self, cap, ui, r);
                }
                // Probe rows: evenly spaced rows of each box, in the order the probes were laid out above.
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = ui;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                size_t pi = 0;
                for (size_t k = 0; k < rects.size() && pi < slot->probes.size(); ++k) {
                    const auto& r = rects[k];
                    const LONG h = r.bottom - r.top;
                    const UINT w = (UINT)(r.right - r.left);
                    if (h <= 0 || w == 0) {
                        continue;
                    }
                    const int rows = std::clamp((int)(h / 6), 1, PROBE_ROWS_MAX);
                    for (int j = 0; j < rows && pi < slot->probes.size(); ++j, ++pi) {
                        const LONG y = r.top + (LONG)(((float)j + 0.5f) * (float)h / (float)rows);
                        D3D12_TEXTURE_COPY_LOCATION dst{};
                        dst.pResource = slot->readback;
                        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        dst.PlacedFootprint.Offset = slot->probes[pi].offset;
                        dst.PlacedFootprint.Footprint.Format = du.Format;
                        dst.PlacedFootprint.Footprint.Width = w;
                        dst.PlacedFootprint.Footprint.Height = 1;
                        dst.PlacedFootprint.Footprint.Depth = 1;
                        dst.PlacedFootprint.Footprint.RowPitch = (UINT)(((UINT64)w * 4 + 255) & ~255ull);
                        D3D12_BOX box{(UINT)r.left, (UINT)y, 0u, (UINT)r.right, (UINT)y + 1, 1u};
                        if (g_copy_region_hook != nullptr) {
                            g_copy_region_hook->get_original<decltype(copy_region)>()(self, &dst, 0, 0, 0, &src, &box);
                        } else {
                            self->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                        }
                    }
                }
                transition(self, ui, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    cap, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            }

            // Erase the element areas so they do not reach the eye images.
            if (g_erase.load() && !rects.empty()) {
                const auto rtv = fresh_rtv(ui, du.Format);
                if (rtv != 0) {
                    const float clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
                    D3D12_CPU_DESCRIPTOR_HANDLE h{};
                    h.ptr = (SIZE_T)rtv;
                    if (g_clear_rtv_hook != nullptr) {
                        g_clear_rtv_hook->get_original<decltype(clear_rtv)>()(self, h, clear, (UINT)rects.size(), rects.data());
                    } else {
                        self->ClearRenderTargetView(h, clear, (UINT)rects.size(), rects.data());
                    }
                }
            }
        }
    }

    original(self, n, b);
}

// Our own RTV of the UI buffer for the clear; recreated when the buffer changes. Called from the present thread.
inline void update_ui_rtv(ID3D12Resource* ui) {
    static ID3D12Resource* last = nullptr;
    static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap{};
    if (ui == last || g_device == nullptr) {
        return;
    }
    last = ui;
    g_ui_rtv = 0;
    if (ui == nullptr) {
        return;
    }
    if (heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) {
            return;
        }
    }
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    if (g_create_rtv_hook != nullptr) {
        g_create_rtv_hook->get_original<decltype(create_rtv)>()(g_device, ui, nullptr, h);
    } else {
        g_device->CreateRenderTargetView(ui, nullptr, h);
    }
    g_ui_rtv = (uint64_t)h.ptr;

    // Element areas in the flat (Screen mode) layout, measured from Build 17-18 snapshots of the UI buffer, as
    // fractions of its width and height: tutorial box, health, stamina, weapon wheel.
    const auto d = ui->GetDesc();
    const float w = (float)d.Width;
    const float hh = (float)d.Height;
    static constexpr float areas[4][4] = {
        {0.00f, 0.27f, 0.30f, 0.46f}, // tutorial box
        {0.00f, 0.68f, 0.17f, 0.79f}, // health
        {0.36f, 0.71f, 0.64f, 0.81f}, // stamina
        {0.74f, 0.60f, 1.00f, 0.86f}, // weapon wheel
    };
    std::scoped_lock _{g_rects_mtx};
    g_rects.clear();
    for (const auto& a : areas) {
        g_rects.push_back(D3D12_RECT{(LONG)(a[0] * w), (LONG)(a[1] * hh), (LONG)(a[2] * w), (LONG)(a[3] * hh)});
    }
    spdlog::info("[PragmataVR] UI buffer hand-off: own RTV ready for {} x {}, {} element areas", d.Width, d.Height, g_rects.size());
}

// Hooks the D3D12 functions through the vtables of the game's device and a throwaway command list. All devices and
// command lists of the runtime share these functions, so the game's own calls go through the hooks.
inline bool install(ID3D12Device* device) {
    static bool tried = false;
    static bool ok = false;
    if (tried) {
        return ok;
    }
    tried = true;

    if (device == nullptr) {
        return false;
    }

    g_rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_device = device;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list{};
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        spdlog::error("[PragmataVR] GPU trace: could not create a command list to find the hooks");
        return false;
    }
    list->Close();

    auto dev_vt = *(uintptr_t**)device;
    auto list_vt = *(uintptr_t**)list.Get();

    const auto make = [](std::unique_ptr<FunctionHook>& h, uintptr_t target, uintptr_t dest, const char* what) {
        h = std::make_unique<FunctionHook>(target, dest);
        if (!h->create()) {
            spdlog::error("[PragmataVR] GPU trace: could not hook {}", what);
            h.reset();
            return false;
        }
        return true;
    };

    // Build 34: only the ResourceBarrier hook is needed by the Fixed HUD (capture, probes, erase). The diagnostic hooks of
    // Builds 18-32 (CreateRenderTargetView, OMSetRenderTargets, draws, copies, clears, ExecuteIndirect, ExecuteBundle)
    // are no longer installed; the code paths that used them fall back to the plain calls.
    (void)dev_vt;
    ok = make(g_barrier_hook, list_vt[26], (uintptr_t)&barrier, "ResourceBarrier");
    spdlog::info("[PragmataVR] GPU trace hooks {} (RTV descriptor size {})", ok ? "installed" : "partly failed", g_rtv_increment);
    return ok;
}

inline std::string describe(const RtvInfo& i, bool known, uint64_t handle, ID3D12Resource* backbuffer) {
    if (!known) {
        return fmt::format("?{:x}", handle);
    }
    if (i.res == g_ui_buffer.load()) {
        return fmt::format("UIBUF({}x{} f{})", i.w, i.h, i.format);
    }
    if (i.res == backbuffer) {
        return fmt::format("BACKBUF({}x{} f{})", i.w, i.h, i.format);
    }
    return fmt::format("{:x}({}x{} f{})", (uintptr_t)i.res, i.w, i.h, i.format);
}

// Writes the collected events to the log. Consecutive identical render target sets on one command list are merged.
inline void dump(ID3D12Resource* backbuffer) {
    std::vector<Event> events{};
    {
        std::scoped_lock _{g_events_mtx};
        events.swap(g_events);
        g_last_event.clear();
    }

    size_t ui_sets = 0;
    size_t ui_draws = 0;
    size_t ui_mrt = 0;
    std::string lines{};
    size_t written = 0;

    for (size_t k = 0; k < events.size(); ++k) {
        const auto& e = events[k];
        bool touches_ui = false;
        for (uint32_t i = 0; i < e.n && i < 4; ++i) {
            if (e.known[i] && e.info[i].res == g_ui_buffer.load()) {
                touches_ui = true;
            }
        }
        if (e.kind == 'R' && touches_ui) {
            ++ui_sets;
            ui_draws += e.draws;
            if (e.n > 1) {
                ++ui_mrt;
            }
        }

        // Write UI-related events, and render target sets around them, in full; skip the rest past a limit.
        bool near_ui = touches_ui;
        for (size_t j = (k > 3 ? k - 3 : 0); j < (std::min)(events.size(), k + 4) && !near_ui; ++j) {
            const auto& o = events[j];
            for (uint32_t i = 0; i < o.n && i < 4; ++i) {
                if (o.known[i] && o.info[i].res == g_ui_buffer.load()) {
                    near_ui = true;
                }
            }
        }
        if (!near_ui && written > 400) {
            continue;
        }

        std::string targets{};
        for (uint32_t i = 0; i < e.n && i < 4; ++i) {
            targets += (i > 0 ? ", " : "") + describe(e.info[i], e.known[i], e.handle[i], backbuffer);
        }
        if (e.kind == 'X') {
            lines += fmt::format("\n  #{} f{} t{} list {:x} REDIRECTED a UI-buffer set of {} targets", k, e.frame, e.tid, (uintptr_t)e.list, e.n);
        } else if (e.kind == 'C') {
            lines += fmt::format("\n  #{} f{} t{} list {:x} COPY dst {} <- src {}", k, e.frame, e.tid, (uintptr_t)e.list,
                describe(e.info[0], e.known[0], 0, backbuffer), describe(e.info[1], e.known[1], 0, backbuffer));
        } else {
            lines += fmt::format("\n  #{} f{} t{} list {:x} RT[{}] {} draws {}{}", k, e.frame, e.tid, (uintptr_t)e.list, e.n, targets,
                e.draws, touches_ui ? "  <== UI BUFFER" : "");
        }
        ++written;
        if (lines.size() > 120000) {
            lines += "\n  (log cut)";
            break;
        }
    }

    spdlog::info("[PragmataVR] GPU trace: {} events, {} render target sets bind the UI buffer ({} with more than one target, {} draws); "
        "RTVs seen created {} (UI buffer RTVs {}); UI buffer {:x}, backbuffer {:x}:{}",
        events.size(), ui_sets, ui_mrt, ui_draws, g_creates_total.load(), g_ui_rtv_creates.load(),
        (uintptr_t)g_ui_buffer.load(), (uintptr_t)backbuffer, lines);
}
} // namespace pragmata_trace
