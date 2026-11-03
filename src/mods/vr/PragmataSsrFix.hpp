#pragma once

// PragmataVR: reflections fix, motion vector side.
//
// The reflections fix (VR::on_scene_layer_update, ini ssr_fix) gives the main scene info's old_view_projection_matrix
// the camera that drew the previous frame's image: the OTHER eye's, as Pragmata draws one eye per frame. Screen space
// reflections then reproject that image correctly and stop sliding. But the engine derives its motion vectors from the
// same matrix, so they then encode "this eye now -> the other eye last frame": a stereo parallax that DLSS / FSR (one
// instance per eye, UpscalersWobblingFix) and the AFW warp take for motion, and the image shimmers and looks low
// resolution.
//
// Here, where REFramework already intercepts the upscaler's motion vectors (the DLSS / FSR hooks in VR.cpp), a compute
// shader swaps that camera motion for "this eye now -> this eye two frames ago": each pixel is unprojected with the
// depth, reprojected with the matrix the engine was given and with this eye's old camera, and the difference applied.
// The result is written into VR::motionVectorsDesc (REFramework's copy of the vectors), which the upscaler is handed and
// the AFW warp reads.
//
// This runs on the ENGINE's command list, between its motion vector pass and the upscaler. The engine's own resources
// are not copied or transitioned (the motion vectors are read from REFramework's copy, the depth through a shader
// resource view). Two things the engine relies on are put back: its descriptor heaps and compute root signature,
// recorded by hooks on the command list vtables (the upscaler restores what it finds bound when it starts; our heap left
// there made the engine's next draws crash the driver).
//
// The eye: the hooks' frame counters change under the render thread, so while the fix is on the eye alternates call by
// call (AFW draws the eyes in turn, one upscaler call per frame), and a sample of the engine's vectors checks the phase:
// if they match the other eye's parallax the wrong way round, the phase flips.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <spdlog/spdlog.h>

#include "../VR.hpp"
#include "d3d12/ComPtr.hpp"
#include "PragmataHud.hpp"

#pragma comment(lib, "d3dcompiler")

namespace pragmata_ssr_fix {

namespace detail {

constexpr int SLOTS = 4;
constexpr int PROBE_N = 16;    // the phase check samples a PROBE_N x PROBE_N grid over the image
constexpr int PROBE_FLOATS = 4; // per sample: mv.xy, predicted other-eye parallax.xy
constexpr UINT64 PROBE_SLOT_BYTES = (UINT64)PROBE_N * PROBE_N * PROBE_FLOATS * sizeof(float);
constexpr UINT64 CB_SLOT_BYTES = 256;

inline const char* SHADER = R"(
Texture2D<float2> g_mv : register(t0);
Texture2D<float> g_depth : register(t1);
RWByteAddressBuffer g_out : register(u0);   // R16G16_FLOAT pixels, row pitch g_pitch_out px
RWByteAddressBuffer g_probe : register(u1);
cbuffer P : register(b0) {
    float4x4 g_clip_to_world_now;   // inverse(P_now * V_now)
    float4x4 g_world_to_clip_other; // the old view-projection the engine was given: what its vectors point at
    float4x4 g_world_to_clip_same;  // P_now * V_same_last: what they should point at
    int2 g_size; int g_pitch_out; uint g_probe_slot;
    int2 g_probe_step; float2 g_uv_to_mv;
};
float2 to_uv(float4 c) { return float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)g_size.x || id.y >= (uint)g_size.y) return;
    float2 mv = g_mv.Load(int3(id.xy, 0));
    float z = g_depth.Load(int3(id.xy, 0));
    float2 uv = (float2(id.xy) + 0.5) / float2(g_size);
    float2 d_other = 0, d_same = 0;
    // Far plane pixels (reverse Z 0, the sky) still need correcting, since the eyes' projections differ: they are taken
    // as very far (about 10 km), not at infinity, whose w of 0 has no reliable sign.
    float4 w = mul(g_clip_to_world_now, float4(uv.x * 2 - 1, 1 - uv.y * 2, max(z, 1e-5), 1));
    w /= w.w;
    float4 co = mul(g_world_to_clip_other, w);
    float4 cs = mul(g_world_to_clip_same, w);
    if (co.w > 1e-6 && cs.w > 1e-6) {
        d_other = (to_uv(co) - uv) * g_uv_to_mv;
        d_same = (to_uv(cs) - uv) * g_uv_to_mv;
    }
    float2 o = mv - d_other + d_same;
    g_out.Store((id.y * g_pitch_out + id.x) * 4, f32tof16(o.x) | (f32tof16(o.y) << 16));
    uint2 step = (uint2)g_probe_step;
    if (all(id.xy % step == step / 2) && all(id.xy / step < 16)) {
        uint a = (g_probe_slot * 256 + (id.y / step.y) * 16 + id.x / step.x) * 16;
        g_probe.Store4(a, asuint(float4(mv, d_other)));
    }
}
)";

struct Constants {
    glm::mat4 clip_to_world_now;
    glm::mat4 world_to_clip_other;
    glm::mat4 world_to_clip_same;
    int32_t size[2];
    int32_t pitch_out;
    uint32_t probe_slot;
    int32_t probe_step[2];
    float uv_to_mv[2];
};
static_assert(sizeof(Constants) <= CB_SLOT_BYTES);

struct EyeViews {
    Matrix4x4f view_now{};       // this eye, this frame
    Matrix4x4f this_last_view{}; // this eye, two frames ago (what the upscaler and the warp want)
    Matrix4x4f other_last_vp{};  // the old view-projection the engine was given (the other eye's of last frame)
    bool valid{false};
};

struct Fix {
    bool failed{false};
    bool ready{false};
    d3d12::ComPtr<ID3D12RootSignature> rs{};
    d3d12::ComPtr<ID3D12PipelineState> pso{};
    d3d12::ComPtr<ID3D12DescriptorHeap> heap{}; // SLOTS x (mv srv, depth srv)
    UINT heap_inc{0};
    d3d12::ComPtr<ID3D12Resource> out_buf{};
    UINT64 out_buf_bytes{0};
    d3d12::ComPtr<ID3D12Resource> cb{}; // upload ring, SLOTS x CB_SLOT_BYTES
    uint8_t* cb_mapped{};
    d3d12::ComPtr<ID3D12Resource> probe{};
    d3d12::ComPtr<ID3D12Resource> probe_readback{};
    float* probe_mapped{};
    int frame{0};
    int last_flip_frame{-100};
};

inline std::array<EyeViews, 2> g_views{};
inline Fix g_fix{};
inline std::atomic<int64_t> g_corrected_ms{0};
inline uint64_t g_eye_calls{0};
inline int g_eye_phase{-1}; // -1 = not set

// The engine's descriptor heaps and compute root signature per command list, and the vtables hooked to record them
// (the engine records on command lists of more than one class).
struct ListState {
    ID3D12DescriptorHeap* heaps[2]{};
    UINT heap_count{0};
    ID3D12RootSignature* compute_rs{};
};
using SetHeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using SetRootSignatureFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
constexpr int VT_SET_DESCRIPTOR_HEAPS = 28; // ID3D12GraphicsCommandList vtable slots
constexpr int VT_SET_COMPUTE_ROOT_SIGNATURE = 29;
struct HookedVtable {
    void** vt{};
    SetHeapsFn set_heaps{};
    SetRootSignatureFn set_compute_rs{};
};
inline std::mutex g_list_mutex;
inline std::unordered_map<ID3D12GraphicsCommandList*, ListState> g_lists;
inline std::array<HookedVtable, 32> g_vtables{};
inline std::atomic<int> g_vtable_count{0};

inline const HookedVtable* hooked(ID3D12GraphicsCommandList* cl) {
    void** vt = *reinterpret_cast<void***>(cl);
    const int n = g_vtable_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_vtables[i].vt == vt) return &g_vtables[i];
    }
    return nullptr;
}

// The originals for cl. A vtable copied from a hooked one after it was patched (runtime layers build some by copying) is
// not registered but reaches the hooks; the registered one whose neighbouring entries match is where it came from.
inline const HookedVtable& originals(ID3D12GraphicsCommandList* cl) {
    if (const auto* h = hooked(cl)) {
        return *h;
    }
    void** vt = *reinterpret_cast<void***>(cl);
    const int n = g_vtable_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        void** o = g_vtables[i].vt;
        if (o[VT_SET_DESCRIPTOR_HEAPS - 1] == vt[VT_SET_DESCRIPTOR_HEAPS - 1] && o[VT_SET_COMPUTE_ROOT_SIGNATURE + 1] == vt[VT_SET_COMPUTE_ROOT_SIGNATURE + 1]) {
            return g_vtables[i];
        }
    }
    return g_vtables[0];
}

inline void STDMETHODCALLTYPE hk_set_heaps(ID3D12GraphicsCommandList* cl, UINT n, ID3D12DescriptorHeap* const* heaps) {
    {
        std::scoped_lock _{g_list_mutex};
        auto& st = g_lists[cl];
        st.heap_count = (std::min)(n, 2u);
        for (UINT i = 0; i < st.heap_count; ++i) st.heaps[i] = heaps[i];
    }
    originals(cl).set_heaps(cl, n, heaps);
}

inline void STDMETHODCALLTYPE hk_set_compute_rs(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    {
        std::scoped_lock _{g_list_mutex};
        g_lists[cl].compute_rs = rs;
    }
    originals(cl).set_compute_rs(cl, rs);
}

// Hooks the vtable cl uses (once per vtable); nullptr if it cannot.
inline const HookedVtable* track_list(ID3D12GraphicsCommandList* cl) {
    if (const auto* h = hooked(cl)) {
        return h;
    }
    std::scoped_lock _{g_list_mutex};
    if (const auto* h = hooked(cl)) {
        return h;
    }
    const int n = g_vtable_count.load();
    void** vt = *reinterpret_cast<void***>(cl);
    if (vt[VT_SET_DESCRIPTOR_HEAPS] == reinterpret_cast<void*>(&hk_set_heaps)) {
        return &originals(cl); // a copy of a hooked vtable: already reaches the hooks
    }
    DWORD old{};
    if (n >= (int)g_vtables.size() || !VirtualProtect(&vt[VT_SET_DESCRIPTOR_HEAPS], 2 * sizeof(void*), PAGE_READWRITE, &old)) {
        return nullptr;
    }
    auto& h = g_vtables[n];
    h.vt = vt;
    h.set_heaps = reinterpret_cast<SetHeapsFn>(vt[VT_SET_DESCRIPTOR_HEAPS]);
    h.set_compute_rs = reinterpret_cast<SetRootSignatureFn>(vt[VT_SET_COMPUTE_ROOT_SIGNATURE]);
    g_vtable_count.store(n + 1, std::memory_order_release); // before the hooks can be reached through vt
    vt[VT_SET_DESCRIPTOR_HEAPS] = reinterpret_cast<void*>(&hk_set_heaps);
    vt[VT_SET_COMPUTE_ROOT_SIGNATURE] = reinterpret_cast<void*>(&hk_set_compute_rs);
    VirtualProtect(&vt[VT_SET_DESCRIPTOR_HEAPS], 2 * sizeof(void*), old, &old);
    return &h;
}

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// As VR.cpp's to_reverseZ: the clip matrices match the engine's reverse-Z depth buffer.
inline glm::mat4 reverse_z(const glm::mat4& proj) {
    const glm::mat4 t{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1};
    return t * proj;
}

inline DXGI_FORMAT depth_srv_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

inline bool fail(Fix& f, const char* what, HRESULT hr = S_OK) {
    spdlog::error("[PragmataVR] Reflections fix: motion vector correction: {} failed ({:x}); off for this session", what, (uint32_t)hr);
    f.failed = true;
    return false;
}

inline D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, bool uav) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return bd;
}

inline bool setup(Fix& f, ID3D12Device* device) {
    // Root signature: b0 (root CBV), t0 t1 (descriptor table: the two textures), u0 u1 (root UAVs, buffers).
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 2;
    srv_range.BaseShaderRegister = 0;
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = (UINT)params.size();
    rsd.pParameters = params.data();
    d3d12::ComPtr<ID3DBlob> blob{}, err{};
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) return fail(f, "root signature", hr);
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&f.rs));
    if (FAILED(hr)) return fail(f, "CreateRootSignature", hr);

    d3d12::ComPtr<ID3DBlob> cs{};
    err.Reset();
    hr = D3DCompile(SHADER, std::strlen(SHADER), "PragmataSsrFix", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr)) {
        spdlog::error("[PragmataVR] Reflections fix: shader: {}", err != nullptr ? (const char*)err->GetBufferPointer() : "?");
        return fail(f, "D3DCompile", hr);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC psd{};
    psd.pRootSignature = f.rs.Get();
    psd.CS = D3D12_SHADER_BYTECODE{cs->GetBufferPointer(), cs->GetBufferSize()};
    hr = device->CreateComputePipelineState(&psd, IID_PPV_ARGS(&f.pso));
    if (FAILED(hr)) return fail(f, "CreateComputePipelineState", hr);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = SLOTS * 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&f.heap));
    if (FAILED(hr)) return fail(f, "CreateDescriptorHeap", hr);
    f.heap_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    auto bd = buffer_desc(SLOTS * CB_SLOT_BYTES, false);
    hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&f.cb));
    if (FAILED(hr)) return fail(f, "constant buffer", hr);
    D3D12_RANGE none{0, 0};
    hr = f.cb->Map(0, &none, (void**)&f.cb_mapped);
    if (FAILED(hr)) return fail(f, "constant buffer Map", hr);

    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    bd = buffer_desc(SLOTS * PROBE_SLOT_BYTES, true);
    hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&f.probe));
    if (FAILED(hr)) return fail(f, "probe buffer", hr);
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    bd = buffer_desc(SLOTS * PROBE_SLOT_BYTES, false);
    hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&f.probe_readback));
    if (FAILED(hr)) return fail(f, "probe readback", hr);
    D3D12_RANGE rr{0, (SIZE_T)(SLOTS * PROBE_SLOT_BYTES)};
    hr = f.probe_readback->Map(0, &rr, (void**)&f.probe_mapped);
    if (FAILED(hr)) return fail(f, "probe Map", hr);
    f.ready = true;
    return true;
}

inline bool ensure_output(Fix& f, ID3D12Device* device, UINT64 bytes) {
    if (f.out_buf != nullptr && f.out_buf_bytes >= bytes) {
        return true;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto bd = buffer_desc(bytes, true);
    f.out_buf.Reset();
    const HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&f.out_buf));
    if (FAILED(hr)) return fail(f, "output buffer", hr);
    f.out_buf_bytes = bytes;
    return true;
}

// The probe written a few frames ago: do the engine's vectors match the predicted other-eye parallax (1), its reverse
// (-1: the eye guess was wrong), or can it not tell (0: too little parallax)?
inline int check_phase(const Fix& f, int slot) {
    const float* p = f.probe_mapped + (size_t)slot * PROBE_N * PROBE_N * PROBE_FLOATS;
    double mv_abs = 0, res_plus = 0, res_minus = 0;
    int n = 0;
    for (int i = 0; i < PROBE_N * PROBE_N; ++i) {
        const float mvx = p[i * 4], mvy = p[i * 4 + 1], ox = p[i * 4 + 2], oy = p[i * 4 + 3];
        if (!std::isfinite(mvx) || !std::isfinite(ox) || (ox == 0.0f && oy == 0.0f)) {
            continue;
        }
        ++n;
        mv_abs += std::hypot(mvx, mvy);
        res_plus += std::hypot(mvx - ox, mvy - oy);
        res_minus += std::hypot(mvx + ox, mvy + oy);
    }
    if (n < 64 || mv_abs / n < 1e-4) {
        return 0;
    }
    return (res_minus < 0.5 * res_plus && res_plus > 0.3 * mv_abs) ? -1 : 1;
}

inline ID3D12Resource* correct_on(VR& vr, ID3D12GraphicsCommandList* cl, int eye, ID3D12Resource* depth, float mv_scale_x,
                                  float mv_scale_y, const HookedVtable& hv) {
    auto& f = g_fix;
    const auto& v = g_views[eye & 1];
    if (!v.valid || depth == nullptr || vr.motionVectorsDesc.pTexture == nullptr) {
        return nullptr;
    }
    d3d12::ComPtr<ID3D12Device> device{};
    if (FAILED(cl->GetDevice(IID_PPV_ARGS(&device))) || (!f.ready && !setup(f, device.Get()))) {
        return nullptr;
    }
    auto* const mvs = vr.motionVectorsDesc.pTexture; // REFramework's copy, a normal texture
    const auto mvd = mvs->GetDesc();
    const auto dd = depth->GetDesc();
    const DXGI_FORMAT dsf = depth_srv_format(dd.Format);
    if (dsf == DXGI_FORMAT_UNKNOWN || mvd.Width != dd.Width || mvd.Height != dd.Height || mvd.DepthOrArraySize != 1 ||
        mvd.Format != DXGI_FORMAT_R16G16_FLOAT || dd.DepthOrArraySize != 1 || dd.SampleDesc.Count != 1) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            spdlog::warn("[PragmataVR] Reflections fix: cannot correct motion vectors ({} x {} format {}, depth {} x {} format {})",
                         mvd.Width, mvd.Height, (int)mvd.Format, dd.Width, dd.Height, (int)dd.Format);
        }
        return nullptr;
    }
    const UINT w = (UINT)mvd.Width, h = mvd.Height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    fp.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
    fp.Footprint.Width = w;
    fp.Footprint.Height = h;
    fp.Footprint.Depth = 1;
    fp.Footprint.RowPitch = (w * 4 + 255) & ~255u;
    if (!ensure_output(f, device.Get(), (UINT64)fp.Footprint.RowPitch * h)) {
        return nullptr;
    }

    const int slot = f.frame % SLOTS;
    ++f.frame;
    // The probe of SLOTS - 1 frames ago (long executed); after a flip, the frames still in flight used the old phase.
    if (f.frame > SLOTS && f.frame - f.last_flip_frame > SLOTS + 1 && check_phase(f, (slot + 1) % SLOTS) < 0) {
        g_eye_phase ^= 1;
        f.last_flip_frame = f.frame;
        spdlog::info("[PragmataVR] Reflections fix: eye phase corrected");
    }

    const auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        if (a == b) return;
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter = b;
        cl->ResourceBarrier(1, &br);
    };

    // Views of REFramework's vector copy and the depth, in our heap.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = f.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * 2 * f.heap_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = f.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += (UINT64)slot * 2 * f.heap_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = mvd.Format;
    device->CreateShaderResourceView(mvs, &sd, cpu);
    cpu.ptr += f.heap_inc;
    sd.Format = dsf;
    device->CreateShaderResourceView(depth, &sd, cpu);

    const glm::mat4 p_now = reverse_z(vr.get_runtime()->projections[eye & 1]);
    Constants k{};
    k.clip_to_world_now = glm::inverse(p_now * v.view_now);
    k.world_to_clip_other = v.other_last_vp;
    k.world_to_clip_same = p_now * v.this_last_view;
    k.size[0] = (int32_t)w;
    k.size[1] = (int32_t)h;
    k.pitch_out = (int32_t)(fp.Footprint.RowPitch / 4);
    k.probe_slot = (uint32_t)slot;
    k.probe_step[0] = (std::max)(1, (int32_t)w / PROBE_N);
    k.probe_step[1] = (std::max)(1, (int32_t)h / PROBE_N);
    k.uv_to_mv[0] = mv_scale_x != 0.0f ? (float)w / mv_scale_x : 1.0f; // engine units: uv offset * size / mv scale
    k.uv_to_mv[1] = mv_scale_y != 0.0f ? (float)h / mv_scale_y : 1.0f;
    std::memcpy(f.cb_mapped + slot * CB_SLOT_BYTES, &k, sizeof(k));

    ID3D12DescriptorHeap* heaps[] = {f.heap.Get()};
    hv.set_heaps(cl, 1, heaps);
    hv.set_compute_rs(cl, f.rs.Get());
    cl->SetPipelineState(f.pso.Get());
    cl->SetComputeRootConstantBufferView(0, f.cb->GetGPUVirtualAddress() + slot * CB_SLOT_BYTES);
    cl->SetComputeRootDescriptorTable(1, gpu);
    cl->SetComputeRootUnorderedAccessView(2, f.out_buf->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(3, f.probe->GetGPUVirtualAddress());
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    // The probe to its readback slot; the result over REFramework's copy (left in its usual state).
    transition(f.probe.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(f.probe_readback.Get(), slot * PROBE_SLOT_BYTES, f.probe.Get(), slot * PROBE_SLOT_BYTES, PROBE_SLOT_BYTES);
    transition(f.probe.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(f.out_buf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const auto mvs_state = vr.motionVectorsDesc.initialState;
    transition(mvs, mvs_state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = f.out_buf.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    dst.pResource = mvs;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(mvs, D3D12_RESOURCE_STATE_COPY_DEST, mvs_state);
    transition(f.out_buf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_corrected_ms.store(now_ms(), std::memory_order_relaxed);
    static bool logged = false;
    if (!logged) {
        logged = true;
        spdlog::info("[PragmataVR] Reflections fix: motion vectors corrected to same-eye ({} x {})", w, h);
    }
    return mvs;
}

} // namespace detail

// Game thread (VR::on_scene_layer_update, main scene info, pass 0), before old_view_matrix[eye] takes this frame's view.
// other_last_vp: the old view-projection the reflections fix gives the engine this frame (nullptr if none yet).
inline void note_views(int eye, const Matrix4x4f& view_now, const Matrix4x4f& this_last_view, const Matrix4x4f* other_last_vp) {
    auto& v = detail::g_views[eye & 1];
    v.valid = other_last_vp != nullptr;
    if (!v.valid) {
        return;
    }
    v.view_now = view_now;
    v.this_last_view = this_last_view;
    v.other_last_vp = *other_last_vp;
}

// Are the motion vectors the AFW warp reads (VR::motionVectorsDesc) other-eye ones this frame? True while the reflections
// fix is on and no upscaler hook has corrected them lately (no DLSS / FSR).
inline bool mvs_are_other_eye() {
    return pragmatavr::HudTuning::get().tune()->ssr_fix != 0 &&
           detail::now_ms() - detail::g_corrected_ms.load(std::memory_order_relaxed) > 250;
}

// Render thread, upscaler hooks: the eye being drawn (0 left, 1 right). counter_eye is the hooks' frame counter guess,
// used as it is while the reflections fix is off, and to start the phase.
inline int render_eye(int counter_eye) {
    using namespace detail;
    if (pragmatavr::HudTuning::get().tune()->ssr_fix == 0) {
        g_eye_phase = -1;
        return counter_eye;
    }
    if (g_eye_phase < 0) {
        g_eye_phase = (int)((counter_eye - g_eye_calls) & 1);
    }
    return (int)((g_eye_calls++ + g_eye_phase) & 1);
}

// Render thread, in the DLSS / FSR hook after the engine's motion vectors were copied into VR::motionVectorsDesc. Returns
// the corrected motion vectors to hand the upscaler (VR::motionVectorsDesc, in its usual state), or nullptr to leave its
// input alone. mv_scale: the upscaler's motion vector scale.
inline ID3D12Resource* correct(VR& vr, ID3D12GraphicsCommandList* cl, int eye, ID3D12Resource* depth, float mv_scale_x, float mv_scale_y) {
    using namespace detail;
    const HookedVtable* hv = nullptr;
    if (g_fix.failed || pragmatavr::HudTuning::get().tune()->ssr_fix == 0 || !vr.is_fix_dlss() || cl == nullptr ||
        (hv = track_list(cl)) == nullptr) {
        return nullptr;
    }
    ListState engine{};
    {
        std::scoped_lock _{g_list_mutex};
        const auto it = g_lists.find(cl);
        if (it == g_lists.end() || it->second.heap_count == 0) {
            return nullptr; // not seen the engine bind its heaps on this list yet
        }
        engine = it->second;
    }
    ID3D12Resource* const out = correct_on(vr, cl, eye, depth, mv_scale_x, mv_scale_y, *hv);
    hv->set_heaps(cl, engine.heap_count, engine.heaps);
    if (engine.compute_rs != nullptr) {
        hv->set_compute_rs(cl, engine.compute_rs);
    }
    return out;
}

} // namespace pragmata_ssr_fix
