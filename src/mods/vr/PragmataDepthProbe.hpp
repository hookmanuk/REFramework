#pragma once

// PragmataVR: reticle depth from the game's depth buffer. The physics ray (PragmataReticleDepth.hpp) misses much of what
// is drawn (large parts of walls have no collision a ray can hit, 2026-11-02 logs), so the depth comes from what the
// game drew instead: each AFW frame, a point along the aim ray is projected into the eye just rendered, a 3 x 3 patch
// of its depth buffer is read there by a small compute shader, and the median is turned back into a world point, whose
// distance along the aim ray is the reticle depth (pragmata_reticle_depth::tick smooths it).
//
// Threading: sample() runs on the render thread (D3D12Component::on_frame, AFW path), where VR::depthTex[0] is the
// rendered eye's depth in PIXEL_SHADER_RESOURCE state (the AFW plugin reads it the same way) and VR::cameraData[eye]
// holds that eye's matrices (the reverse-Z projection the depth was written with). The commands go on the game's queue
// before the AFW command list. Results are read back one or two frames later from a ring of slots, without waiting.
// The aim ray comes from the main thread (pragmata_reticle_depth::tick) through pragmatavr::g_reticle_depth.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <spdlog/spdlog.h>

#include "../VR.hpp"
#include "d3d12/ComPtr.hpp"
#include "PragmataHud.hpp"

#pragma comment(lib, "d3dcompiler")

namespace pragmata_depth_probe {

namespace detail {

constexpr int SLOTS = 4;
constexpr int SAMPLES = 9;          // 3 x 3 patch
constexpr int SLOT_FLOATS = 16;     // per slot in the output buffers (9 used)
constexpr int STEP_PX = 3;          // spacing of the patch samples (eye image px)

inline const char* SHADER = R"(
Texture2D<float> g_depth : register(t0);
RWByteAddressBuffer g_out : register(u0);
cbuffer Params : register(b0) { int2 g_px; int2 g_size; uint g_slot; int g_step; };
[numthreads(9, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 o = int2((int)(id.x % 3) - 1, (int)(id.x / 3) - 1) * g_step;
    int2 p = clamp(g_px + o, int2(0, 0), g_size - int2(1, 1));
    g_out.Store((g_slot * 16 + id.x) * 4, asuint(g_depth.Load(int3(p, 0))));
}
)";

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct SlotMeta {
    bool used{false};
    uint64_t fence{0};
    glm::mat4 clip_to_view{1.0f};
    glm::mat4 view_to_world{1.0f};
    float ndc_x{0.0f}, ndc_y{0.0f}, ndc_step_x{0.0f}, ndc_step_y{0.0f};
    glm::vec3 o{0.0f}, d{0.0f};
};

struct Probe {
    bool failed{false};
    bool ready{false};
    ID3D12Device* device{};
    d3d12::ComPtr<ID3D12RootSignature> rs{};
    d3d12::ComPtr<ID3D12PipelineState> pso{};
    d3d12::ComPtr<ID3D12DescriptorHeap> heap{};
    UINT heap_inc{0};
    d3d12::ComPtr<ID3D12Resource> out{};      // default heap, UAV (raw)
    d3d12::ComPtr<ID3D12Resource> readback{}; // readback heap, mapped
    float* mapped{};
    d3d12::ComPtr<ID3D12Fence> fence{};
    uint64_t fence_value{0};
    std::array<d3d12::ComPtr<ID3D12CommandAllocator>, SLOTS> alloc{};
    d3d12::ComPtr<ID3D12GraphicsCommandList> list{};
    std::array<SlotMeta, SLOTS> meta{};
    int next{0};
    DXGI_FORMAT logged_format{DXGI_FORMAT_UNKNOWN};
};

inline Probe& probe() {
    static Probe p{};
    return p;
}

inline bool fail(Probe& p, const char* what, HRESULT hr = S_OK) {
    spdlog::error("[PragmataVR] Reticle depth buffer: {} failed ({:x}); depth buffer probe off for this session", what, (uint32_t)hr);
    p.failed = true;
    return false;
}

inline bool setup(Probe& p, ID3D12Device* device) {
    p.device = device;

    // Root signature: b0 = 6 constants, t0 = descriptor table (the depth SRV), u0 = root UAV (the output buffer).
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    std::array<D3D12_ROOT_PARAMETER, 3> params{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 6;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = (UINT)params.size();
    rsd.pParameters = params.data();
    d3d12::ComPtr<ID3DBlob> blob{}, err{};
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) return fail(p, "root signature serialize", hr);
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&p.rs));
    if (FAILED(hr)) return fail(p, "CreateRootSignature", hr);

    d3d12::ComPtr<ID3DBlob> cs{};
    err.Reset();
    hr = D3DCompile(SHADER, std::strlen(SHADER), "PragmataDepthProbe", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr)) {
        spdlog::error("[PragmataVR] Reticle depth buffer: shader: {}", err != nullptr ? (const char*)err->GetBufferPointer() : "?");
        return fail(p, "D3DCompile", hr);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC psd{};
    psd.pRootSignature = p.rs.Get();
    psd.CS = D3D12_SHADER_BYTECODE{cs->GetBufferPointer(), cs->GetBufferSize()};
    hr = device->CreateComputePipelineState(&psd, IID_PPV_ARGS(&p.pso));
    if (FAILED(hr)) return fail(p, "CreateComputePipelineState", hr);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = SLOTS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.heap));
    if (FAILED(hr)) return fail(p, "CreateDescriptorHeap", hr);
    p.heap_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const UINT64 bytes = (UINT64)SLOTS * SLOT_FLOATS * sizeof(float);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.out));
    if (FAILED(hr)) return fail(p, "output buffer", hr);
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.readback));
    if (FAILED(hr)) return fail(p, "readback buffer", hr);
    D3D12_RANGE read_range{0, (SIZE_T)bytes};
    hr = p.readback->Map(0, &read_range, (void**)&p.mapped);
    if (FAILED(hr)) return fail(p, "readback Map", hr);

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence));
    if (FAILED(hr)) return fail(p, "CreateFence", hr);
    for (auto& a : p.alloc) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        if (FAILED(hr)) return fail(p, "CreateCommandAllocator", hr);
    }
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.alloc[0].Get(), nullptr, IID_PPV_ARGS(&p.list));
    if (FAILED(hr)) return fail(p, "CreateCommandList", hr);
    p.list->Close();
    p.ready = true;
    spdlog::info("[PragmataVR] Reticle depth buffer: probe ready");
    return true;
}

// The SRV format for a depth resource's format (DXGI_FORMAT_UNKNOWN when not a depth format the probe can read).
inline DXGI_FORMAT srv_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// A finished slot: the patch's depths back to world points, the median distance along the aim ray.
inline void read_slot(Probe& p, int s) {
    auto& m = p.meta[s];
    m.used = false;
    std::array<float, SAMPLES> dist{};
    int n = 0;
    std::array<float, SAMPLES> raw{};
    std::memcpy(raw.data(), p.mapped + s * SLOT_FLOATS, sizeof(raw));
    for (int i = 0; i < SAMPLES; ++i) {
        const float z = raw[i];
        if (!std::isfinite(z) || z <= 1e-7f) {
            continue; // reverse Z: 0 = nothing drawn (sky)
        }
        const float nx = m.ndc_x + (float)(i % 3 - 1) * m.ndc_step_x;
        const float ny = m.ndc_y - (float)(i / 3 - 1) * m.ndc_step_y; // patch rows go down the image, NDC y goes up
        glm::vec4 v = m.clip_to_view * glm::vec4{nx, ny, z, 1.0f};
        if (std::fabs(v.w) < 1e-9f) {
            continue;
        }
        v /= v.w;
        const glm::vec4 w = m.view_to_world * v;
        const float t = glm::dot(glm::vec3{w} - m.o, m.d);
        if (std::isfinite(t) && t > 0.0f) {
            dist[n++] = t;
        }
    }
    auto& out = pragmatavr::g_reticle_depth;
    float result = -1.0f; // most of the patch shows sky: nothing there
    if (n >= 5) {
        std::nth_element(dist.begin(), dist.begin() + n / 2, dist.begin() + n);
        result = dist[n / 2];
    }
    out.buf_dist_m.store(result, std::memory_order_relaxed);
    out.buf_ms.store(now_ms(), std::memory_order_relaxed);
}

} // namespace detail

// Render thread, AFW path, once per frame: depth = VR::depthTex[0] (this eye, pixel shader resource state), cam = its
// VR::cameraData entry.
inline void sample(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* depth, const pd::CameraData& cam) {
    using namespace detail;
    auto& p = probe();
    if (p.failed || device == nullptr || queue == nullptr || depth == nullptr) {
        return;
    }
    auto& in = pragmatavr::g_reticle_depth;
    const auto now = now_ms();
    const int64_t ray_ms = in.ray_ms.load(std::memory_order_relaxed);
    if (ray_ms == 0 || now - ray_ms > 500) {
        return; // New reticle with Depth TARGET not in use
    }
    if (!p.ready && !setup(p, device)) {
        return;
    }

    // Finished slots first (no waiting: a slot still on the GPU is read on a later frame).
    const uint64_t done = p.fence->GetCompletedValue();
    for (int s = 0; s < SLOTS; ++s) {
        if (p.meta[s].used && p.meta[s].fence <= done) {
            read_slot(p, s);
        }
    }

    const int s = p.next;
    if (p.meta[s].used) {
        return; // the GPU is behind: skip this frame
    }

    const auto desc = depth->GetDesc();
    const DXGI_FORMAT sf = srv_format(desc.Format);
    if (desc.Format != p.logged_format) {
        p.logged_format = desc.Format;
        spdlog::info("[PragmataVR] Reticle depth buffer: depth {} x {}, format {}, array {}, mips {} -> SRV format {}",
                     desc.Width, desc.Height, (int)desc.Format, desc.DepthOrArraySize, desc.MipLevels, (int)sf);
    }
    if (sf == DXGI_FORMAT_UNKNOWN || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || desc.Width == 0 || desc.Height == 0) {
        return;
    }

    // Where the aim ray is in this eye's image: a point along it (at the last depth) projected with the eye's matrices.
    const glm::vec3 o{in.ray_ox.load(std::memory_order_relaxed), in.ray_oy.load(std::memory_order_relaxed), in.ray_oz.load(std::memory_order_relaxed)};
    const glm::vec3 d{in.ray_dx.load(std::memory_order_relaxed), in.ray_dy.load(std::memory_order_relaxed), in.ray_dz.load(std::memory_order_relaxed)};
    const float guess = std::clamp(in.ray_guess_m.load(std::memory_order_relaxed), 0.5f, 200.0f);
    const glm::vec4 clip = cam.srcViewToClipMatrix * (cam.srcWorldToViewMatrix * glm::vec4{o + d * guess, 1.0f});
    if (!(clip.w > 1e-6f)) {
        return;
    }
    const float ndc_x = clip.x / clip.w;
    const float ndc_y = clip.y / clip.w;
    const float W = (float)desc.Width;
    const float H = (float)desc.Height;
    const int px = (int)std::floor((ndc_x * 0.5f + 0.5f) * W);
    const int py = (int)std::floor((0.5f - ndc_y * 0.5f) * H);
    if (px < 0 || py < 0 || px >= (int)desc.Width || py >= (int)desc.Height) {
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = sf;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = p.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)s * p.heap_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = p.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += (UINT64)s * p.heap_inc;
    device->CreateShaderResourceView(depth, &sd, cpu);

    if (FAILED(p.alloc[s]->Reset()) || FAILED(p.list->Reset(p.alloc[s].Get(), p.pso.Get()))) {
        fail(p, "command list reset");
        return;
    }
    auto* cl = p.list.Get();
    const auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter = b;
        cl->ResourceBarrier(1, &br);
    };
    constexpr auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto ALL_SR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(depth, PSR, ALL_SR);
    cl->SetComputeRootSignature(p.rs.Get());
    ID3D12DescriptorHeap* heaps[] = {p.heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    const std::array<int32_t, 6> k{px, py, (int32_t)desc.Width, (int32_t)desc.Height, s, STEP_PX};
    cl->SetComputeRoot32BitConstants(0, (UINT)k.size(), k.data(), 0);
    cl->SetComputeRootDescriptorTable(1, gpu);
    cl->SetComputeRootUnorderedAccessView(2, p.out->GetGPUVirtualAddress());
    cl->Dispatch(1, 1, 1);
    transition(depth, ALL_SR, PSR);
    transition(p.out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const UINT64 off = (UINT64)s * SLOT_FLOATS * sizeof(float);
    cl->CopyBufferRegion(p.readback.Get(), off, p.out.Get(), off, SLOT_FLOATS * sizeof(float));
    if (FAILED(cl->Close())) {
        fail(p, "command list close");
        return;
    }
    ID3D12CommandList* lists[] = {cl};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(p.fence.Get(), ++p.fence_value);

    auto& m = p.meta[s];
    m.used = true;
    m.fence = p.fence_value;
    m.clip_to_view = cam.srcClipToViewMatrix;
    m.view_to_world = cam.srcViewToWorldMatrix;
    m.ndc_x = ndc_x;
    m.ndc_y = ndc_y;
    m.ndc_step_x = 2.0f * STEP_PX / W;
    m.ndc_step_y = 2.0f * STEP_PX / H;
    m.o = o;
    m.d = d;
    p.next = (s + 1) % SLOTS;
}

} // namespace pragmata_depth_probe
