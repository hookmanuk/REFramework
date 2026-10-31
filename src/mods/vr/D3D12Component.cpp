#include <openvr.h>
#include <utility/ScopeGuard.hpp>

#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>
#include <../../directxtk12-src/Src/d3dx12.h>

#include "D3D12Component.hpp"

#if defined(PRAGMATA)
#include <wincodec.h>
#include <../../directxtk12-src/Inc/ScreenGrab.h>
#include <../../directxtk12-src/Inc/DescriptorHeap.h>
#include "PragmataHud.hpp"
#include "PragmataGpuTrace.hpp"

namespace {
// PragmataVR: copies the VR mod menu (drawn by ImGui into REFramework's IMGUI render target) into the
// eye image, centered on the eye's forward direction, so the menu is readable in the headset.
struct PragmataMenuCompositor {
    std::unique_ptr<DirectX::DX12::SpriteBatch> batch{};
    DXGI_FORMAT batch_format{DXGI_FORMAT_UNKNOWN};
    d3d12::TextureContext imgui_ctx{};
    ID3D12Resource* imgui_res{nullptr};
    std::unique_ptr<DirectX::DescriptorHeap> bb_rtv_heap{};
    std::array<ID3D12Resource*, 8> bb_res{};
    d3d12::CommandContext cmds{};
    bool cmds_ready{false};
    bool failed{false};
};

PragmataMenuCompositor g_pragmata_menu{};

void pragmata_composite_menu(VR* vr, ID3D12Resource* backbuffer, UINT bb_index, bool left) {
    auto& hud = pragmatavr::HudTuning::get();
    auto& m = g_pragmata_menu;

    if (m.failed || backbuffer == nullptr || !hud.menu_open.load() || !hud.menu_rect_valid.load()) {
        return;
    }

    auto& imgui_rt = g_framework->get_rendertarget_d3d12();
    if (imgui_rt == nullptr) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto queue = hook->get_command_queue();
    const auto bb_desc = backbuffer->GetDesc();
    const auto rt_desc = imgui_rt->GetDesc();

    try {
        if (m.batch == nullptr || m.batch_format != bb_desc.Format) {
            DirectX::ResourceUploadBatch upload{device};
            upload.Begin();
            DirectX::RenderTargetState output_state{bb_desc.Format, DXGI_FORMAT_UNKNOWN};
            DirectX::SpriteBatchPipelineStateDescription pd{output_state};
            m.batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);
            upload.End(queue).wait();
            m.batch_format = bb_desc.Format;
            spdlog::info("[PragmataVR] Menu compositor ready for back buffer format {}", (int)bb_desc.Format);
        }

        if (m.imgui_res != imgui_rt.Get()) {
            m.imgui_ctx.setup(device, imgui_rt.Get(), std::nullopt, std::nullopt, L"PragmataVR menu source");
            m.imgui_res = imgui_rt.Get();
        }

        if (m.bb_rtv_heap == nullptr) {
            m.bb_rtv_heap = std::make_unique<DirectX::DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, m.bb_res.size());
        }

        const auto slot = bb_index % m.bb_res.size();
        if (m.bb_res[slot] != backbuffer) {
            device->CreateRenderTargetView(backbuffer, nullptr, m.bb_rtv_heap->GetCpuHandle(slot));
            m.bb_res[slot] = backbuffer;
        }

        if (!m.cmds_ready) {
            m.cmds_ready = m.cmds.setup(L"PragmataVR menu");
            if (!m.cmds_ready) {
                m.failed = true;
                return;
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[PragmataVR] Menu compositor setup failed: {}", e.what());
        m.failed = true;
        return;
    } catch (...) {
        spdlog::error("[PragmataVR] Menu compositor setup failed");
        m.failed = true;
        return;
    }

    const auto tune = hud.tune();

    // Source: the menu window's rectangle in the ImGui render target.
    RECT src{};
    src.left = (LONG)std::clamp(hud.menu_rect_x.load(), 0.0f, (float)rt_desc.Width - 1.0f);
    src.top = (LONG)std::clamp(hud.menu_rect_y.load(), 0.0f, (float)rt_desc.Height - 1.0f);
    src.right = (LONG)std::clamp(hud.menu_rect_x.load() + hud.menu_rect_w.load(), (float)src.left + 1.0f, (float)rt_desc.Width);
    src.bottom = (LONG)std::clamp(hud.menu_rect_y.load() + hud.menu_rect_h.load(), (float)src.top + 1.0f, (float)rt_desc.Height);

    const float src_w = (float)(src.right - src.left);
    const float src_h = (float)(src.bottom - src.top);

    // Destination: a panel fixed in the room where the menu was opened (menu distance / width / up-down settings),
    // seen from this eye's real pose. The panel's center and its right / up edges are projected into this eye, and
    // the menu is drawn as a rectangle with that size and roll, so it stays put when the head turns, tilts or moves.
    Matrix4x4f proj{};
    {
        std::shared_lock _{vr->get_runtime()->eyes_mtx};
        proj = vr->get_runtime()->projections[left ? 0 : 1];
    }

    const auto st = hud.head_state();
    if (!st.valid) {
        return;
    }

    const float menu_dist = tune->menu[pragmatavr::MenuDistance];
    const auto placement = pragmatavr::menu_quad_placement(*tune, st.menu, src_w, src_h);
    const float half_w = placement.width_m * 0.5f;
    const float half_h = placement.height_m * 0.5f;
    const auto plane = pragmatavr::make_plane(st.menu, 0.0f, tune->menu[pragmatavr::MenuPitch], menu_dist, half_w, half_h);
    const glm::mat4 view_proj = glm::mat4{proj} * pragmatavr::eye_view(st.head, vr->get_eye_transform(left ? 0 : 1));

    const float bb_w = (float)bb_desc.Width;
    const float bb_h = (float)bb_desc.Height;

    bool visible = true;
    const auto to_screen = [&](const glm::vec3& p) {
        const glm::vec4 c = view_proj * glm::vec4{p, 1.0f};
        if (c.w <= 1e-4f) {
            visible = false;
            return glm::vec2{0.0f};
        }
        return glm::vec2{(c.x / c.w + 1.0f) * 0.5f * bb_w, (1.0f - c.y / c.w) * 0.5f * bb_h};
    };

    const glm::vec2 c = to_screen(plane.center);
    const glm::vec2 r0 = to_screen(plane.center - plane.right);
    const glm::vec2 r1 = to_screen(plane.center + plane.right);
    const glm::vec2 u0 = to_screen(plane.center - plane.up);
    const glm::vec2 u1 = to_screen(plane.center + plane.up);
    if (!visible) {
        return; // the menu's spot in the room is behind this eye
    }

    if (c.x < -bb_w || c.x > bb_w * 2.0f || c.y < -bb_h || c.y > bb_h * 2.0f) {
        return; // well outside this eye's view
    }

    const glm::vec2 across = r1 - r0;
    const glm::vec2 down = u0 - u1; // screen y grows downward
    const float dst_w = glm::length(across);
    const float dst_h = glm::length(down);
    if (dst_w < 1.0f || dst_h < 1.0f) {
        return;
    }

    const float roll = std::atan2(across.y, across.x);

    m.cmds.wait(INFINITE);
    auto cl = m.cmds.cmd_list.Get();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &barrier);

    D3D12_VIEWPORT viewport{};
    viewport.Width = bb_w;
    viewport.Height = bb_h;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;

    D3D12_RECT scissor{0, 0, (LONG)bb_desc.Width, (LONG)bb_desc.Height};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m.bb_rtv_heap->GetCpuHandle(bb_index % m.bb_res.size());

    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->RSSetViewports(1, &viewport);
    cl->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {m.imgui_ctx.srv_heap->Heap()};
    cl->SetDescriptorHeaps(1, heaps);

    m.batch->SetViewport(viewport);
    m.batch->Begin(cl, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);
    m.batch->Draw(m.imgui_ctx.get_srv_gpu(), DirectX::XMUINT2{(uint32_t)rt_desc.Width, (uint32_t)rt_desc.Height},
        DirectX::XMFLOAT2{c.x, c.y}, &src, DirectX::Colors::White, roll,
        DirectX::XMFLOAT2{src_w * 0.5f, src_h * 0.5f}, DirectX::XMFLOAT2{dst_w / src_w, dst_h / src_h});
    m.batch->End();

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &barrier);

    m.cmds.has_commands = true;
    m.cmds.execute();
}

// PragmataVR Build 9: the mod menu as an OpenXR quad composition layer. Each submitted frame while the menu is open,
// the menu rectangle is copied from the ImGui render target into this swapchain, and OpenXR::end_frame submits it at
// a fixed pose in stage space. The runtime's compositor draws it with full perspective for each eye at display time,
// so the panel stays exactly where it was placed whatever the head does.
struct PragmataMenuQuad {
    std::vector<XrSwapchainImageD3D12KHR> images{};
    std::vector<std::unique_ptr<d3d12::CommandContext>> cmds{};
    bool failed{false};
};

PragmataMenuQuad g_pragmata_quad{};

void pragmata_drop_menu_swapchain(runtimes::OpenXR* openxr) {
    auto& mq = openxr->menu_quad;
    auto& q = g_pragmata_quad;

    for (auto& c : q.cmds) {
        if (c != nullptr) {
            c->wait(INFINITE);
        }
    }

    // A swapchain from an older session was destroyed with that session.
    if (mq.swapchain != XR_NULL_HANDLE && mq.owner == openxr->session) {
        xrDestroySwapchain(mq.swapchain);
    }

    mq.swapchain = XR_NULL_HANDLE;
    mq.owner = XR_NULL_HANDLE;
    mq.width = 0;
    mq.height = 0;
    q.images.clear();
    q.cmds.clear();
}

bool pragmata_create_menu_swapchain(runtimes::OpenXR* openxr, int32_t need_w, int32_t need_h) {
    auto& mq = openxr->menu_quad;
    auto& q = g_pragmata_quad;

    // The ImGui target is R8G8B8A8_UNORM holding sRGB-encoded colors. An _SRGB swapchain makes the compositor decode
    // them, so the menu shows the colors it was drawn with. Both formats are in the R8G8B8A8 family, so a plain copy works.
    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(openxr->session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    if (format_count > 0) {
        xrEnumerateSwapchainFormats(openxr->session, format_count, &format_count, formats.data());
    }

    int64_t format = 0;
    for (const auto wanted : {(int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM}) {
        if (std::find(formats.begin(), formats.end(), wanted) != formats.end()) {
            format = wanted;
            break;
        }
    }

    if (format == 0) {
        spdlog::error("[PragmataVR] OpenXR runtime offers no R8G8B8A8 swapchain format for the menu; using the eye-image fallback");
        q.failed = true;
        return false;
    }

    const auto round_up = [](int32_t v) { return (((std::max)(v, 1024) + 255) / 256) * 256; };

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = (uint32_t)round_up(need_w);
    ci.height = (uint32_t)round_up(need_h);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;

    XrSwapchain handle{XR_NULL_HANDLE};
    auto result = xrCreateSwapchain(openxr->session, &ci, &handle);
    if (result != XR_SUCCESS) {
        spdlog::error("[PragmataVR] Could not create the menu swapchain ({} x {}): {}", ci.width, ci.height, openxr->get_result_string(result));
        q.failed = true;
        return false;
    }

    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(handle, 0, &image_count, nullptr);
    q.images.assign(image_count, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    result = xrEnumerateSwapchainImages(handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)q.images.data());
    if (result != XR_SUCCESS || image_count == 0) {
        spdlog::error("[PragmataVR] Could not get the menu swapchain images: {}", openxr->get_result_string(result));
        xrDestroySwapchain(handle);
        q.images.clear();
        q.failed = true;
        return false;
    }

    q.cmds.clear();
    for (uint32_t i = 0; i < image_count; ++i) {
        auto c = std::make_unique<d3d12::CommandContext>();
        if (!c->setup((std::wstring{L"PragmataVR menu quad "} + std::to_wstring(i)).c_str())) {
            spdlog::error("[PragmataVR] Could not set up the menu quad command list");
            xrDestroySwapchain(handle);
            q.images.clear();
            q.cmds.clear();
            q.failed = true;
            return false;
        }
        q.cmds.push_back(std::move(c));
    }

    mq.swapchain = handle;
    mq.owner = openxr->session;
    mq.width = (int32_t)ci.width;
    mq.height = (int32_t)ci.height;

    spdlog::info("[PragmataVR] Menu quad layer ready: {} x {}, format {}, {} images", ci.width, ci.height, format, image_count);
    return true;
}

// Called right before OpenXR::end_frame. Leaves menu_quad.visible false unless the menu was copied this frame.
// Returns false when the quad cannot be used (the caller then falls back to pasting the menu into the eye images).
bool pragmata_update_menu_quad(runtimes::OpenXR* openxr) {
    auto& mq = openxr->menu_quad;
    auto& hud = pragmatavr::HudTuning::get();
    auto& q = g_pragmata_quad;

    mq.visible = false;

    if (q.failed) {
        return false;
    }

    if (!openxr->ready() || openxr->frame_state.shouldRender != XR_TRUE || !hud.menu_open.load() || !hud.menu_rect_valid.load()) {
        return true;
    }

    auto& imgui_rt = g_framework->get_rendertarget_d3d12();
    if (imgui_rt == nullptr) {
        return true;
    }

    const auto st = hud.head_state();
    if (!st.valid) {
        return true;
    }

    // Source: the menu rectangle in the ImGui render target.
    const auto rt_desc = imgui_rt->GetDesc();
    const float rt_w = (float)rt_desc.Width;
    const float rt_h = (float)rt_desc.Height;
    const UINT src_left = (UINT)std::clamp(hud.menu_rect_x.load(), 0.0f, rt_w - 1.0f);
    const UINT src_top = (UINT)std::clamp(hud.menu_rect_y.load(), 0.0f, rt_h - 1.0f);
    const UINT src_right = (UINT)std::clamp(std::ceil(hud.menu_rect_x.load() + hud.menu_rect_w.load()), (float)src_left + 1.0f, rt_w);
    const UINT src_bottom = (UINT)std::clamp(std::ceil(hud.menu_rect_y.load() + hud.menu_rect_h.load()), (float)src_top + 1.0f, rt_h);
    const int32_t w = (int32_t)(src_right - src_left);
    const int32_t h = (int32_t)(src_bottom - src_top);

    if (mq.swapchain != XR_NULL_HANDLE && (mq.owner != openxr->session || w > mq.width || h > mq.height)) {
        pragmata_drop_menu_swapchain(openxr);
    }

    if (mq.swapchain == XR_NULL_HANDLE && !pragmata_create_menu_swapchain(openxr, w, h)) {
        return false;
    }

    // Copy the menu into the next swapchain image.
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    auto result = xrAcquireSwapchainImage(mq.swapchain, &acquire_info, &index);
    if (result != XR_SUCCESS || index >= q.images.size()) {
        spdlog::error("[PragmataVR] Menu quad: xrAcquireSwapchainImage failed: {}", openxr->get_result_string(result));
        return true;
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(mq.swapchain, &wait_info);
    if (result != XR_SUCCESS) {
        spdlog::error("[PragmataVR] Menu quad: xrWaitSwapchainImage failed: {}", openxr->get_result_string(result));
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(mq.swapchain, &release_info);
        return true;
    }

    auto& cmds = *q.cmds[index];
    cmds.wait(INFINITE);

    D3D12_BOX box{src_left, src_top, 0, src_right, src_bottom, 1};
    cmds.copy_region(imgui_rt.Get(), q.images[index].texture, &box, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmds.execute();

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    result = xrReleaseSwapchainImage(mq.swapchain, &release_info);
    if (result != XR_SUCCESS) {
        spdlog::error("[PragmataVR] Menu quad: xrReleaseSwapchainImage failed: {}", openxr->get_result_string(result));
        return true;
    }

    // Place it in the room: where the menu was opened, at the menu distance / pitch, sized to the menu's pixels.
    const auto tune = hud.tune();
    const auto placement = pragmatavr::menu_quad_placement(*tune, st.menu, (float)w, (float)h);

    mq.pose.orientation = XrQuaternionf{placement.orientation.x, placement.orientation.y, placement.orientation.z, placement.orientation.w};
    mq.pose.position = XrVector3f{placement.position.x, placement.position.y, placement.position.z};
    mq.size = XrExtent2Df{placement.width_m, placement.height_m};
    mq.used_width = w;
    mq.used_height = h;
    mq.visible = true;

    static bool logged = false;
    if (!logged) {
        logged = true;
        spdlog::info("[PragmataVR] Menu quad shown: {} x {} px, {:.2f} x {:.2f} m at ({:.2f}, {:.2f}, {:.2f})",
            w, h, placement.width_m, placement.height_m, placement.position.x, placement.position.y, placement.position.z);
    }

    return true;
}
// PragmataVR Build 17: the HUD panel. Each left-eye frame the engine's UI buffer (health, stamina, weapon wheel and
// tutorial drawn flat in Screen mode, plus the rest of the UI) is copied to src; right before end_frame src is copied
// into its own OpenXR swapchain and submitted as a room-fixed quad (OpenXR::hud_quad). Same pattern as the menu.
struct PragmataHudPanel {
    Microsoft::WRL::ComPtr<ID3D12Resource> src{};
    d3d12::CommandContext src_cmds{};
    bool src_cmds_ready{false};
    std::chrono::steady_clock::time_point src_time{};
    int32_t src_w{0};
    int32_t src_h{0};
    DXGI_FORMAT src_format{DXGI_FORMAT_UNKNOWN};
    std::vector<XrSwapchainImageD3D12KHR> images{};
    std::vector<std::unique_ptr<d3d12::CommandContext>> cmds{};
    bool failed{false};
    // Build 25: the panel image, built from the element areas only (everything else transparent), plus box outlines.
    Microsoft::WRL::ComPtr<ID3D12Resource> comp{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> comp_heap{};
    // Build 31: more capture textures (src is slot 0) and each slot's probe buffer; see pragmata_trace::Slot.
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, pragmata_trace::SLOT_COUNT - 1> more{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, pragmata_trace::SLOT_COUNT> rb{};
    // Build 31: the last capture of each element that really held it; the panel image is built from this.
    Microsoft::WRL::ComPtr<ID3D12Resource> good{};
    // Build 34: left-eye frames spent in Follows Head mode with the Fixed textures still allocated.
    int idle_frames{0};
};

PragmataHudPanel g_pragmata_hud{};
// Build 38: per element area (same order as pragmata_trace::g_rects): where it goes on the panel, relative to its place
// in the UI buffer (the element's Left / right and Up / down). Guarded by pragmata_trace::g_rects_mtx.
std::vector<POINT> g_pragmata_rect_offsets{};
// Build 56: per element area (same order): its quad on the panel, center x / y and width / height in UI buffer pixels.
std::vector<std::array<float, 4>> g_pragmata_rect_display{};

bool pragmata_ensure_comp(PragmataHudPanel& p) {
    if (p.comp != nullptr) {
        const auto d = p.comp->GetDesc();
        if ((int32_t)d.Width == p.src_w && (int32_t)d.Height == p.src_h && d.Format == p.src_format) {
            return true;
        }
        p.comp.Reset();
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    D3D12_RESOURCE_DESC d = p.src->GetDesc();
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.comp)))) {
        spdlog::error("[PragmataVR] ANCHORED: could not create the panel image");
        return false;
    }
    if (p.comp_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.comp_heap)))) {
            p.comp.Reset();
            return false;
        }
    }
    device->CreateRenderTargetView(p.comp.Get(), nullptr, p.comp_heap->GetCPUDescriptorHandleForHeapStart());
    spdlog::info("[PragmataVR] ANCHORED: panel image ready, {} x {}", p.src_w, p.src_h);
    return true;
}

// Build 34: Follows Head mode frees the Fixed HUD's textures (about 490 MB; the OpenXR quad swapchain, about 150 MB more,
// is dropped in pragmata_update_hud_quad). They are released about a second after the switch, once no command list
// recorded before it can still copy into them, and are made again on the next switch to Fixed.
void pragmata_release_fixed_hud(PragmataHudPanel& p) {
    if (p.src == nullptr && p.comp == nullptr && p.good == nullptr) {
        p.idle_frames = 0;
        return;
    }
    if (++p.idle_frames < 60) {
        return;
    }
    p.idle_frames = 0;
    pragmata_trace::g_capture = nullptr;
    pragmata_trace::set_slots({}, {});
    p.src_cmds.wait(INFINITE);
    for (auto& c : p.cmds) {
        if (c != nullptr) {
            c->wait(INFINITE);
        }
    }
    p.src.Reset();
    for (auto& m : p.more) {
        m.Reset();
    }
    for (auto& m : p.rb) {
        m.Reset();
    }
    p.good.Reset();
    p.comp.Reset();
    p.src_w = 0;
    p.src_h = 0;
    p.src_format = DXGI_FORMAT_UNKNOWN;
    spdlog::info("[PragmataVR] Follows Head mode: Fixed HUD textures released");
}

// Called on left-eye frames from on_frame, while the UI buffer still holds this frame's UI.
void pragmata_capture_ui_buffer(VR* vr) {
    auto& p = g_pragmata_hud;
    const auto tune = pragmatavr::HudTuning::get().tune();
    if (!tune->enabled || !tune->hud_panel || p.failed) {
        if (!p.failed) {
            pragmata_release_fixed_hud(p);
        }
        return;
    }
    p.idle_frames = 0;

    // Build 27: the in-game UI buffer's size and format as recorded by the overlay hook (vr->uiBufferTex[0] may point
    // at a texture the engine has already freed, so it is not touched here).
    const auto gw = pragmata_trace::g_game_ui_w.load();
    const auto gh = pragmata_trace::g_game_ui_h.load();
    const auto gf = (DXGI_FORMAT)pragmata_trace::g_game_ui_fmt.load();
    if (gw == 0 || gh == 0 || gf == DXGI_FORMAT_UNKNOWN) {
        return;
    }

    auto device = g_framework->get_d3d12_hook()->get_device();
    D3D12_RESOURCE_DESC d0{};
    d0.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d0.Width = gw;
    d0.Height = gh;
    d0.DepthOrArraySize = 1;
    d0.MipLevels = 1;
    d0.Format = gf;
    d0.SampleDesc.Count = 1;
    d0.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (!p.src_cmds_ready) {
        p.src_cmds_ready = p.src_cmds.setup(L"PragmataVR HUD capture");
        if (!p.src_cmds_ready) {
            p.failed = true;
            return;
        }
    }

    if (p.src == nullptr || p.src_w != (int32_t)d0.Width || p.src_h != (int32_t)d0.Height || p.src_format != d0.Format) {
        pragmata_trace::g_capture = nullptr;
        pragmata_trace::set_slots({}, {});
        p.src_cmds.wait(INFINITE);
        // Build 27: the old capture is kept alive (a render thread may still be recording a copy into it).
        static std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> retired{};
        if (p.src != nullptr) {
            retired.push_back(p.src);
        }
        for (auto& m : p.more) {
            if (m != nullptr) {
                retired.push_back(m);
            }
            m.Reset();
        }
        for (auto& m : p.rb) {
            if (m != nullptr) {
                retired.push_back(m);
            }
            m.Reset();
        }
        if (p.good != nullptr) {
            retired.push_back(p.good);
        }
        p.good.Reset();
        p.src.Reset();
        D3D12_RESOURCE_DESC d = d0;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.src))) ||
            FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.good)))) {
            spdlog::error("[PragmataVR] HUD panel: could not create the capture texture");
            p.src.Reset();
            p.good.Reset();
            p.failed = true;
            return;
        }
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = pragmata_trace::PROBE_BYTES;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rhp{};
        rhp.Type = D3D12_HEAP_TYPE_READBACK;
        std::array<ID3D12Resource*, pragmata_trace::SLOT_COUNT> slots{};
        std::array<ID3D12Resource*, pragmata_trace::SLOT_COUNT> rbs{};
        int made = 0;
        for (int i = 0; i < pragmata_trace::SLOT_COUNT; ++i) {
            ID3D12Resource* tex = nullptr;
            if (i == 0) {
                tex = p.src.Get();
            } else if (SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.more[i - 1])))) {
                tex = p.more[i - 1].Get();
            }
            if (tex != nullptr &&
                SUCCEEDED(device->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.rb[i])))) {
                slots[i] = tex;
                rbs[i] = p.rb[i].Get();
                ++made;
            }
        }
        p.src_w = (int32_t)d0.Width;
        p.src_h = (int32_t)d0.Height;
        p.src_format = d0.Format;
        pragmata_trace::set_slots(slots, rbs);
        spdlog::info("[PragmataVR] HUD panel: capturing the UI buffer, {} x {}, format {}, {} capture slots with probes", p.src_w, p.src_h, (int)p.src_format, made);
    }

    // Build 20: the copy is normally taken at the UI buffer hand-off, on the engine's command list (see
    // pragmata_trace::barrier), before the element areas are cleared. Copying here, after the frame, would miss the
    // elements once they are cleared, so this only runs when no hand-off has been seen.
    pragmata_trace::g_capture = p.src.Get();
    static uint64_t last_handoffs = 0;
    const auto handoffs = pragmata_trace::g_handoffs.load();
    if (handoffs != last_handoffs) {
        last_handoffs = handoffs;
        p.src_time = std::chrono::steady_clock::now();
        static bool logged = false;
        if (!logged) {
            logged = true;
            spdlog::info("[PragmataVR] HUD panel: taking the UI buffer at the engine's hand-off");
        }
        return;
    }
    // Build 27: no copy at present time any more (it read vr->uiBufferTex[0], which may be freed).
    p.src_time = std::chrono::steady_clock::now();
}

void pragmata_drop_hud_swapchain(runtimes::OpenXR* openxr) {
    auto& hq = openxr->hud_quad;
    auto& p = g_pragmata_hud;
    for (auto& c : p.cmds) {
        if (c != nullptr) {
            c->wait(INFINITE);
        }
    }
    if (hq.swapchain != XR_NULL_HANDLE && hq.owner == openxr->session) {
        xrDestroySwapchain(hq.swapchain);
    }
    hq.swapchain = XR_NULL_HANDLE;
    hq.owner = XR_NULL_HANDLE;
    hq.width = 0;
    hq.height = 0;
    p.images.clear();
    p.cmds.clear();
}

bool pragmata_create_hud_swapchain(runtimes::OpenXR* openxr, int32_t w, int32_t h, DXGI_FORMAT src_format) {
    auto& hq = openxr->hud_quad;
    auto& p = g_pragmata_hud;

    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(openxr->session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    if (format_count > 0) {
        xrEnumerateSwapchainFormats(openxr->session, format_count, &format_count, formats.data());
    }

    // The UI buffer's own format first (a plain copy), then the R8G8B8A8 family.
    int64_t format = 0;
    for (const auto wanted : {(int64_t)src_format, (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM}) {
        if (std::find(formats.begin(), formats.end(), wanted) != formats.end()) {
            format = wanted;
            break;
        }
    }
    if (format == 0) {
        spdlog::error("[PragmataVR] HUD panel: no matching OpenXR swapchain format for UI buffer format {}", (int)src_format);
        p.failed = true;
        return false;
    }

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = (uint32_t)w;
    ci.height = (uint32_t)h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;

    XrSwapchain handle{XR_NULL_HANDLE};
    auto result = xrCreateSwapchain(openxr->session, &ci, &handle);
    if (result != XR_SUCCESS) {
        spdlog::error("[PragmataVR] HUD panel: could not create the swapchain ({} x {}): {}", ci.width, ci.height, openxr->get_result_string(result));
        p.failed = true;
        return false;
    }

    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(handle, 0, &image_count, nullptr);
    p.images.assign(image_count, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    result = xrEnumerateSwapchainImages(handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)p.images.data());
    if (result != XR_SUCCESS || image_count == 0) {
        xrDestroySwapchain(handle);
        p.images.clear();
        p.failed = true;
        return false;
    }

    p.cmds.clear();
    for (uint32_t i = 0; i < image_count; ++i) {
        auto c = std::make_unique<d3d12::CommandContext>();
        if (!c->setup((std::wstring{L"PragmataVR HUD quad "} + std::to_wstring(i)).c_str())) {
            xrDestroySwapchain(handle);
            p.images.clear();
            p.cmds.clear();
            p.failed = true;
            return false;
        }
        p.cmds.push_back(std::move(c));
    }

    hq.swapchain = handle;
    hq.owner = openxr->session;
    hq.width = (int32_t)ci.width;
    hq.height = (int32_t)ci.height;
    spdlog::info("[PragmataVR] HUD panel quad ready: {} x {}, format {}, {} images", ci.width, ci.height, format, image_count);
    return true;
}

// Called right before OpenXR::end_frame.
void pragmata_update_hud_quad(runtimes::OpenXR* openxr) {
    auto& hq = openxr->hud_quad;
    auto& p = g_pragmata_hud;
    auto& hud = pragmatavr::HudTuning::get();
    hq.visible = false;

    const auto tune = hud.tune();
    // Build 34: Follows Head mode drops the quad swapchain (made again on the next switch to Fixed).
    if ((!tune->enabled || !tune->hud_panel) && hq.swapchain != XR_NULL_HANDLE) {
        pragmata_drop_hud_swapchain(openxr);
        spdlog::info("[PragmataVR] Follows Head mode: Fixed HUD quad released");
    }
    if (p.failed || !tune->enabled || !tune->hud_panel || p.src == nullptr) {
        return;
    }
    if (!openxr->ready() || openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }
    if (std::chrono::steady_clock::now() - p.src_time > std::chrono::milliseconds(200)) {
        return; // nothing captured recently
    }
    const auto st = hud.head_state();
    if (!st.valid) {
        return;
    }

    if (hq.swapchain != XR_NULL_HANDLE && (hq.owner != openxr->session || hq.width != p.src_w || hq.height != p.src_h)) {
        pragmata_drop_hud_swapchain(openxr);
    }
    if (hq.swapchain == XR_NULL_HANDLE && !pragmata_create_hud_swapchain(openxr, p.src_w, p.src_h, p.src_format)) {
        return;
    }

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    auto result = xrAcquireSwapchainImage(hq.swapchain, &acquire_info, &index);
    if (result != XR_SUCCESS || index >= p.images.size()) {
        return;
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(hq.swapchain, &wait_info);
    if (result != XR_SUCCESS) {
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(hq.swapchain, &release_info);
        return;
    }

    auto& cmds = *p.cmds[index];
    cmds.wait(INFINITE);
    p.src_cmds.wait(INFINITE); // the capture copy must be finished

    // Build 31: the panel is built from the good texture (each element's last capture that really held it).
    ID3D12Resource* cap = p.good != nullptr ? p.good.Get() : p.src.Get();

    // Build 25: the panel shows only the element areas (the same areas cleared from the eye images), so the hacking
    // circle and enemy markers stay in the eyes only. Show element boxes adds a cyan outline around each area.
    std::vector<D3D12_RECT> element_rects{};
    std::vector<std::array<float, 4>> element_display{};
    if (pragmata_ensure_comp(p)) {
        std::vector<D3D12_RECT> rects{};
        std::vector<int> groups{};
        std::vector<std::array<float, 4>> display{};
        {
            std::scoped_lock _{pragmata_trace::g_rects_mtx};
            rects = pragmata_trace::g_rects;
            groups = pragmata_trace::g_rect_groups;
            display = g_pragmata_rect_display;
        }
        display.resize(rects.size(), std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
        const auto copies = pragmata_trace::panel_copies(groups);
        // Build 60: an element that shows up again (after its box was dropped) keeps its old picture in the good texture
        // until a capture holding it arrives. If none does (the element draws nothing visible, like the tutorial box
        // flashing in when LT is pressed), the old picture was shown: once a piece of the start-up warning screen. Its
        // quad is now left out until a fresh capture of it arrives.
        {
            static std::array<bool, 32> was_wanted{};
            static std::array<bool, 32> fresh{};
            std::array<bool, 32> wanted{};
            for (const int g : groups) {
                if (g >= 0 && g < 32) {
                    wanted[g] = true;
                    if (!was_wanted[g]) {
                        fresh[g] = false;
                    }
                }
            }
            for (const auto& c : copies) {
                if (c.group >= 0 && c.group < 32) {
                    fresh[c.group] = true;
                }
            }
            was_wanted = wanted;
            for (size_t i = 0; i < groups.size() && i < rects.size(); ++i) {
                const int g = groups[i];
                if (g >= 0 && g < 32 && !fresh[g]) {
                    rects[i] = D3D12_RECT{0, 0, 0, 0};
                    display[i] = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
                }
            }
        }
        element_rects = rects;
        element_display = display;
        std::scoped_lock _{cmds.mtx};
        auto cl = cmds.cmd_list.Get();
        const auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
            D3D12_RESOURCE_BARRIER br{};
            br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            br.Transition.pResource = r;
            br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            br.Transition.StateBefore = a;
            br.Transition.StateAfter = b;
            cl->ResourceBarrier(1, &br);
        };
        const auto rtv = p.comp_heap->GetCPUDescriptorHandleForHeapStart();
        const float clear[4]{0.0f, 0.0f, 0.0f, 0.0f};

        // Build 31: newest captures that really hold an element -> good texture.
        if (!copies.empty() && p.good != nullptr) {
            tr(p.good.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            for (const auto& c : copies) {
                tr(c.src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION s_loc{};
                s_loc.pResource = c.src;
                s_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION d_loc{};
                d_loc.pResource = p.good.Get();
                d_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_BOX box{(UINT)c.rect.left, (UINT)c.rect.top, 0u, (UINT)c.rect.right, (UINT)c.rect.bottom, 1u};
                cl->CopyTextureRegion(&d_loc, (UINT)c.rect.left, (UINT)c.rect.top, 0, &s_loc, &box);
                tr(c.src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            tr(p.good.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }

        tr(p.comp.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
        tr(p.comp.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        tr(cap, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = cap;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = p.comp.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        // Build 56: each area is copied to the same place in the image; every element gets its own quad showing its
        // area (OpenXR::end_frame), placed and sized on the panel by its Size, Left / right and Up / down.
        std::vector<D3D12_RECT> dst_rects{};
        const LONG pw = (LONG)p.src_w;
        const LONG ph = (LONG)p.src_h;
        for (size_t i = 0; i < rects.size(); ++i) {
            D3D12_RECT sr = rects[i];
            sr.left = std::clamp(sr.left, 0L, pw);
            sr.right = std::clamp(sr.right, 0L, pw);
            sr.top = std::clamp(sr.top, 0L, ph);
            sr.bottom = std::clamp(sr.bottom, 0L, ph);
            if (sr.right <= sr.left || sr.bottom <= sr.top) {
                continue;
            }
            D3D12_BOX box{(UINT)sr.left, (UINT)sr.top, 0u, (UINT)sr.right, (UINT)sr.bottom, 1u};
            cl->CopyTextureRegion(&dst_loc, (UINT)sr.left, (UINT)sr.top, 0, &src_loc, &box);
            dst_rects.push_back(sr);
        }
        tr(cap, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

        if (tune->show_boxes && !dst_rects.empty()) {
            tr(p.comp.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float cyan[4]{0.0f, 0.85f, 0.9f, 1.0f};
            std::vector<D3D12_RECT> lines{};
            const LONG th = 6;
            for (const auto& r : dst_rects) {
                lines.push_back(D3D12_RECT{r.left, r.top, r.right, (std::min)(r.top + th, r.bottom)});
                lines.push_back(D3D12_RECT{r.left, (std::max)(r.bottom - th, r.top), r.right, r.bottom});
                lines.push_back(D3D12_RECT{r.left, r.top, (std::min)(r.left + th, r.right), r.bottom});
                lines.push_back(D3D12_RECT{(std::max)(r.right - th, r.left), r.top, r.right, r.bottom});
            }
            cl->ClearRenderTargetView(rtv, cyan, (UINT)lines.size(), lines.data());
            tr(p.comp.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        } else {
            tr(p.comp.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }

        tr(p.images[index].texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(p.images[index].texture, p.comp.Get());
        tr(p.images[index].texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        tr(p.comp.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        cmds.has_commands = true;
    } else {
        cmds.copy(cap, p.images[index].texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    cmds.execute();

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (xrReleaseSwapchainImage(hq.swapchain, &release_info) != XR_SUCCESS) {
        return;
    }

    // Build 56: Follows Head places the panel in view space (identity anchor, so it moves with the head with no lag);
    // Fixed places it in the room at the HUD anchor.
    const pragmatavr::HudTuning::Pose view_anchor{};
    const auto placement = pragmatavr::hud_panel_placement(*tune, tune->head_locked ? view_anchor : st.hud, (float)p.src_w, (float)p.src_h);
    hq.pose.orientation = XrQuaternionf{placement.orientation.x, placement.orientation.y, placement.orientation.z, placement.orientation.w};
    hq.pose.position = XrVector3f{placement.position.x, placement.position.y, placement.position.z};
    hq.size = XrExtent2Df{placement.width_m, placement.height_m};
    hq.used_width = p.src_w;
    hq.used_height = p.src_h;

    // Build 56: one quad per element: its area of the image, centered and sized as AnchoredBox dcx / dcy / dw / dh on the
    // panel plane. Show element boxes outlines the areas inside each quad.
    openxr->hud_head_locked = tune->head_locked;
    openxr->hud_elements.clear();
    const float mpp = p.src_w > 0 ? placement.width_m / (float)p.src_w : 0.0f;
    const glm::mat3 prot = glm::mat3_cast(placement.orientation);
    for (size_t i = 0; i < element_rects.size() && i < element_display.size(); ++i) {
        const auto& r = element_rects[i];
        const auto& dsp = element_display[i];
        const LONG x0 = std::clamp(r.left, 0L, (LONG)p.src_w);
        const LONG x1 = std::clamp(r.right, 0L, (LONG)p.src_w);
        const LONG y0 = std::clamp(r.top, 0L, (LONG)p.src_h);
        const LONG y1 = std::clamp(r.bottom, 0L, (LONG)p.src_h);
        if (x1 <= x0 + 1 || y1 <= y0 + 1 || dsp[2] <= 1.0f || dsp[3] <= 1.0f) {
            continue;
        }
        const glm::vec3 local{(dsp[0] - (float)p.src_w * 0.5f) * mpp, ((float)p.src_h * 0.5f - dsp[1]) * mpp, 0.0f};
        const glm::vec3 pos = placement.position + prot * local;
        runtimes::OpenXR::HudElementQuad e{};
        e.rect.offset = XrOffset2Di{(int32_t)x0, (int32_t)y0};
        e.rect.extent = XrExtent2Di{(int32_t)(x1 - x0), (int32_t)(y1 - y0)};
        e.pose.orientation = hq.pose.orientation;
        e.pose.position = XrVector3f{pos.x, pos.y, pos.z};
        e.size = XrExtent2Df{dsp[2] * mpp, dsp[3] * mpp};
        openxr->hud_elements.push_back(e);
    }
    hq.visible = true;

    static size_t last_count = 0;
    static bool last_head = false;
    if (openxr->hud_elements.size() != last_count || last_head != tune->head_locked) {
        last_count = openxr->hud_elements.size();
        last_head = tune->head_locked;
        spdlog::info("[PragmataVR] HUD element quads: {} ({})", last_count, last_head ? "view space, Follows Head" : "room, Fixed");
    }

    static bool logged = false;
    if (!logged) {
        logged = true;
        spdlog::info("[PragmataVR] HUD panel shown: {:.2f} x {:.2f} m at ({:.2f}, {:.2f}, {:.2f})",
            placement.width_m, placement.height_m, placement.position.x, placement.position.y, placement.position.z);
    }
}
} // namespace
#endif

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (m_openvr.left_eye_tex[0].texture == nullptr || m_force_reset) {
        setup();
    }

    auto& hook = g_framework->get_d3d12_hook();
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};

    const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer");
        return vr::VRCompositorError_None;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }
    auto eye_texture = backbuffer;

    if (!m_backbuffer_is_8bit && false) {
        auto command_list = m_backbuffer_copy.commands.cmd_list.Get();
        m_backbuffer_copy.commands.wait(INFINITE);

        // Copy current backbuffer into our copy so we can use it as an SRV.
        m_backbuffer_copy.commands.copy(backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

        float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
        m_backbuffer_copy.commands.clear_rtv(m_converted_eye_tex, clear_color, D3D12_RESOURCE_STATE_PRESENT);

        // Convert the backbuffer to 8-bit.
        render_srv_to_rtv(command_list, m_backbuffer_copy, m_converted_eye_tex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        m_backbuffer_copy.commands.execute();

        eye_texture = m_converted_eye_tex.texture;
    }

    auto runtime = vr->get_runtime();


    const auto frame_count = vr->m_render_frame_count;

#if defined(PRAGMATA)
    // PragmataVR Build 18: GPU trace around snapshots (see PragmataGpuTrace.hpp).
    {
        auto& hud = pragmatavr::HudTuning::get();
        static int trace_frames_left = 0;
        pragmata_trace::install(g_framework->get_d3d12_hook()->get_device());
        // Build 26: g_ui_buffer is now set by the overlay layer hook as the engine hands each UI buffer out.
        pragmatavr::g_note_ui_buffer = +[](void* r) { pragmata_trace::note_ui_buffer((ID3D12Resource*)r); };
        {
            const auto t = pragmatavr::HudTuning::get().tune();
            pragmata_trace::g_redirect = false; // Build 19's render target redirect is retired (the UI buffer is drawn on its own)
            pragmata_trace::g_intercept = t->enabled && t->hud_panel;
            pragmata_trace::g_erase = t->enabled && t->hud_panel && t->glued_hud_removed;

            // Build 25: ANCHORED element areas, from the FLAT tuning, every frame (they follow the menu edits).
            if (t->enabled && t->hud_panel && pragmata_trace::g_game_ui_w.load() != 0) {
                struct { uint32_t Width; uint32_t Height; } d{pragmata_trace::g_game_ui_w.load(), pragmata_trace::g_game_ui_h.load()};
                const auto boxes = pragmatavr::anchored_boxes_px(*t, (float)d.Width, (float)d.Height, vr->m_frame_count);
                std::vector<D3D12_RECT> rects{};
                std::vector<int> groups{};
                std::vector<POINT> offs{};
                std::vector<std::array<float, 4>> disp{};
                for (const auto& b : boxes) {
                    rects.push_back(D3D12_RECT{(LONG)b.x0, (LONG)b.y0, (LONG)b.x1, (LONG)b.y1});
                    groups.push_back(b.group);
                    offs.push_back(POINT{(LONG)b.ox, (LONG)b.oy});
                    disp.push_back(std::array<float, 4>{b.dcx, b.dcy, b.dw, b.dh});
                }
                static std::string last_desc{};
                std::string desc{};
                for (const auto& b : boxes) {
                    desc += fmt::format(" {}({},{})-({},{}) panel offset ({},{})", pragmatavr::GROUP_KEYS[b.group], b.x0, b.y0, b.x1, b.y1, b.ox, b.oy);
                }
                if (desc != last_desc) {
                    last_desc = desc;
                    spdlog::info("[PragmataVR] ANCHORED areas in the {} x {} UI buffer:{}", d.Width, d.Height, desc);
                }
                std::scoped_lock _{pragmata_trace::g_rects_mtx};
                pragmata_trace::g_rects = std::move(rects);
                pragmata_trace::g_rect_groups = std::move(groups);
                g_pragmata_rect_offsets = std::move(offs);
                g_pragmata_rect_display = std::move(disp);
            }
        }
        // Build 31: probe fence on the game's queue, every present.
        if (pragmata_trace::g_intercept.load()) {
            pragmata_trace::on_present(device, command_queue);
        }
        pragmata_trace::report_redirects();

        if (hud.trace_requested.exchange(false) && !pragmata_trace::g_active.load()) {
            {
                std::scoped_lock _{pragmata_trace::g_events_mtx};
                pragmata_trace::g_events.clear();
                pragmata_trace::g_last_event.clear();
            }
            pragmata_trace::g_frame = 0;
            pragmata_trace::g_active = true;
            trace_frames_left = 2;
            spdlog::info("[PragmataVR] GPU trace started for two eye frames");
        } else if (pragmata_trace::g_active.load()) {
            pragmata_trace::g_frame++;
            if (--trace_frames_left <= 0) {
                pragmata_trace::g_active = false;
                pragmata_trace::dump(backbuffer.Get());
            }
        }
    }

    // PragmataVR Build 17: capture the UI buffer for the HUD panel (left-eye frames).
    if ((frame_count % 2) == vr->m_left_eye_interval) {
        pragmata_capture_ui_buffer(vr);
    }

    // PragmataVR snapshot (Build 15): copy this eye's image and the other eye's next one (about 11 ms apart, so the two
    // match even while the head moves), then save both. Builds 3-14 saved each eye as it was captured, 0.7 s apart.
    {
        auto& hud = pragmatavr::HudTuning::get();
        static std::array<ComPtr<ID3D12Resource>, 2> snap_tex{};
        static d3d12::CommandContext snap_cmds{};
        static bool snap_cmds_ready = false;

        if (hud.snapshot_frames_left.load() > 0) {
            const bool left = (frame_count % 2) == vr->m_left_eye_interval;
            auto& done = left ? hud.snapshot_left_done : hud.snapshot_right_done;
            const int ei = left ? 0 : 1;

            // Build 38: start with a left-eye frame so the right eye comes from the next frame (the same stereo pair in
            // NATIVE). Before, a series could pair a right eye with the next frame's left eye (head moved in between).
            if (!done.load() && (left || hud.snapshot_left_done.load())) {
                done = true;
                auto device = g_framework->get_d3d12_hook()->get_device();
                const auto bb_desc = backbuffer->GetDesc();

                if (!snap_cmds_ready) {
                    snap_cmds_ready = snap_cmds.setup(L"PragmataVR snapshot");
                }

                auto& tex = snap_tex[ei];
                if (tex == nullptr || tex->GetDesc().Width != bb_desc.Width || tex->GetDesc().Height != bb_desc.Height ||
                    tex->GetDesc().Format != bb_desc.Format) {
                    tex.Reset();
                    D3D12_RESOURCE_DESC d = bb_desc;
                    d.Flags = D3D12_RESOURCE_FLAG_NONE;
                    D3D12_HEAP_PROPERTIES hp{};
                    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex)))) {
                        spdlog::error("[PragmataVR] Snapshot: could not create the copy texture");
                        tex.Reset();
                    }
                }

                if (tex != nullptr && snap_cmds_ready) {
                    snap_cmds.wait(INFINITE);
                    snap_cmds.copy(backbuffer.Get(), tex.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COMMON);
                    snap_cmds.execute();
                    snap_cmds.wait(INFINITE);
                }
            }

            // Build 16: also copy the engine's UI buffers (the HUD drawn on its own, with transparency, before it is
            // mixed into the scene; PureDark's fork reads them for frame warp). Copied on the left-eye frame.
            // Build 26: slot 0 is the UI buffer the engine handed out last (it changes from frame to frame), slot 2 is
            // what the ANCHORED panel received (its capture texture).
            static std::array<ComPtr<ID3D12Resource>, 4> ui_tex{};
            static std::array<bool, 4> ui_ok{};
            if (left && !hud.snapshot_ui_done.load()) {
                hud.snapshot_ui_done = true;
                auto device = g_framework->get_d3d12_hook()->get_device();
                for (int i = 0; i < 4; ++i) {
                    ui_ok[i] = false;
                    // Build 28: slot 3 = the panel image (what the anchored panel was last given). Slot 1 is no longer read.
                    ID3D12Resource* src = i == 0 ? pragmata_trace::g_ui_buffer.load() : (i == 1 ? nullptr : (i == 2 ? g_pragmata_hud.good.Get() : g_pragmata_hud.comp.Get()));
                    if (src == nullptr) {
                        spdlog::info("[PragmataVR] Snapshot: UI buffer {} is not available", i);
                        continue;
                    }
                    const auto d0 = src->GetDesc();
                    spdlog::info("[PragmataVR] Snapshot: UI buffer {}: {} x {}, format {}, flags {:x}, enable_ui_fix {}", i, d0.Width, d0.Height,
                        (int)d0.Format, (int)d0.Flags, vr->m_enable_ui_fix->value());
                    auto& tex = ui_tex[i];
                    if (tex == nullptr || tex->GetDesc().Width != d0.Width || tex->GetDesc().Height != d0.Height || tex->GetDesc().Format != d0.Format) {
                        tex.Reset();
                        D3D12_RESOURCE_DESC d = d0;
                        d.Flags = D3D12_RESOURCE_FLAG_NONE;
                        D3D12_HEAP_PROPERTIES hp{};
                        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex)))) {
                            tex.Reset();
                            continue;
                        }
                    }
                    if (snap_cmds_ready) {
                        snap_cmds.wait(INFINITE);
                        snap_cmds.copy(src, tex.Get(), i >= 2 ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                        snap_cmds.execute();
                        snap_cmds.wait(INFINITE);
                        ui_ok[i] = true;
                    }
                }
            }

            if (hud.snapshot_left_done.load() && hud.snapshot_right_done.load()) {
                hud.snapshot_frames_left = 0;
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const auto dir = pragmatavr::HudTuning::data_dir() / "snapshots";

                // UI buffers: PNG with transparency if WIC can convert the format, and always the exact DDS.
                // Build 26: PNG only (the 49 MB DDS copies are gone). _ui0 = UI buffer, _cap = the ANCHORED panel's capture.
                for (int i = 0; i < 4; ++i) {
                    if (!ui_ok[i] || ui_tex[i] == nullptr) {
                        continue;
                    }
                    const auto png = dir / (hud.snapshot_stem + (i == 3 ? std::string{"_panel.png"} : (i == 2 ? std::string{"_cap.png"} : fmt::format("_ui{}.png", i))));
                    auto hr = DirectX::SaveWICTextureToFile(command_queue, ui_tex[i].Get(), GUID_ContainerFormatPng, png.wstring().c_str(),
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, &GUID_WICPixelFormat32bppBGRA);
                    spdlog::info("[PragmataVR] Snapshot {} PNG {}", i == 2 ? std::string{"panel capture"} : fmt::format("UI buffer {}", i), SUCCEEDED(hr) ? "saved" : "failed");
                }

                for (int i = 0; i < 2; ++i) {
                    if (snap_tex[i] == nullptr) {
                        continue;
                    }
                    const auto png = dir / (hud.snapshot_stem + (i == 0 ? "_left.png" : "_right.png"));
                    auto hr = DirectX::SaveWICTextureToFile(command_queue, snap_tex[i].Get(), GUID_ContainerFormatPng, png.wstring().c_str(),
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, &GUID_WICPixelFormat24bppBGR);

                    if (SUCCEEDED(hr)) {
                        spdlog::info("[PragmataVR] Snapshot saved: {}", png.string());
                    } else {
                        const auto dds = dir / (hud.snapshot_stem + (i == 0 ? "_left.dds" : "_right.dds"));
                        hr = DirectX::SaveDDSTextureToFile(command_queue, snap_tex[i].Get(), dds.wstring().c_str(),
                            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
                        spdlog::info("[PragmataVR] Snapshot PNG failed, DDS {}: {}", SUCCEEDED(hr) ? "saved" : "failed too", dds.string());
                    }
                }
            } else {
                hud.snapshot_frames_left--;
            }
        }
    }

    // PragmataVR mod menu into this eye's image (after the snapshot, so snapshots show the game only).
    // Under OpenXR the menu is its own room-fixed quad layer (pragmata_update_menu_quad, before end_frame). The old
    // paste-into-each-eye path is kept for OpenVR, and as a fallback if the quad layer could not be created.
    if (!runtime->is_openxr() || g_pragmata_quad.failed) {
        pragmata_composite_menu(vr, backbuffer.Get(), backbuffer_index, (frame_count % 2) == vr->m_left_eye_interval);
    }
#endif

    
    //#############################
    //#Frame Warp Module Start
    //#############################
    EyeIndex nEye = (frame_count % 2 == vr->m_left_eye_interval) ? EyeLeft : EyeRight;
    EyeIndex nEyeOther = (frame_count % 2 == vr->m_left_eye_interval) ? EyeRight : EyeLeft;
    auto eyeFrameBuffer = m_eyeFrameBuffers.eyeFrameBuffers[nEye];
    auto otherEyeFrameBuffer = m_eyeFrameBuffers.eyeFrameBuffers[nEyeOther];
    FrameWarpEvaluateParams params;
    if ((vr->is_using_any_afw()) && (!m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture || !m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture))
        force_reset();

    auto colorDesc = eyeFrameBuffer.color.pTexture->GetDesc();
    static TextureDesc texDesc[4];
    int texIndex = m_backbuffer_is_8bit || true ? backbuffer_index : 3;
    if (texDesc[texIndex].pTexture != eye_texture.Get()) {
        texDesc[texIndex].pTexture = eye_texture.Get();
        texDesc[texIndex].initialState = D3D12_RESOURCE_STATE_PRESENT;
        if (texIndex == 3)
            texDesc[texIndex].initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        vr->d3d12Renderer->SetupTextureDesc(texDesc[texIndex]);
    }
    if (texDesc[backbuffer_index].pTexture != backbuffer.Get()) {
        texDesc[backbuffer_index].pTexture = backbuffer.Get();
        texDesc[backbuffer_index].initialState = D3D12_RESOURCE_STATE_PRESENT;
        vr->d3d12Renderer->SetupTextureDesc(texDesc[backbuffer_index]);
    }
    
    if (vr->m_enable_ui_fix->value() ) {
        for (int i = 0; i < 2; i++) {
            if (vr->uiBufferDesc[i].pTexture != vr->uiBufferTex[i]) {
                vr->uiBufferDesc[i].pTexture = vr->uiBufferTex[i];
                vr->uiBufferDesc[i].initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                vr->d3d12Renderer->SetupTextureDesc(vr->uiBufferDesc[i]);
            }
        }
        if (vr->is_using_afw_foveated() && vr->uiBufferTex[0]) {
            auto desc = vr->uiBufferTex[0]->GetDesc();
            if (vr->multipassUIBufferDesc.pTexture == NULL || vr->multipassUIBufferDesc.pTexture->GetDesc().Width != colorDesc.Width ||
                vr->multipassUIBufferDesc.pTexture->GetDesc().Height != colorDesc.Height || vr->multipassUIBufferDesc.pTexture->GetDesc().Format != desc.Format) {
                vr->d3d12Renderer->CreateTexture(colorDesc.Width, colorDesc.Height, desc.Format, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->multipassUIBufferDesc, false);
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        if (vr->depthDesc[i].pTexture != vr->depthTex[i]) {
            vr->depthDesc[i].pTexture = vr->depthTex[i];
            vr->depthDesc[i].initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            vr->d3d12Renderer->SetupTextureDesc(vr->depthDesc[i]);
            vr->depthDesc[i].type = Depth;
        }
    }
    if (vr->depthTex[0]) {
        auto desc = vr->depthTex[0]->GetDesc();
        if (vr->motionVectorsDesc.pTexture == NULL || vr->motionVectorsDesc.pTexture->GetDesc().Width != desc.Width ||
            vr->motionVectorsDesc.pTexture->GetDesc().Height != desc.Height) {
            vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->motionVectorsDesc, true);
        }
        if (vr->is_using_afw_foveated() &&
            (vr->motionVectorsCorrectedDesc.pTexture == NULL || vr->motionVectorsCorrectedDesc.pTexture->GetDesc().Width != desc.Width ||
                vr->motionVectorsCorrectedDesc.pTexture->GetDesc().Height != desc.Height)) {
            vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->motionVectorsCorrectedDesc, true);
        }
        if (vr->is_using_afw_foveated() &&
            (vr->foveatedDepthDesc.pTexture == NULL || vr->foveatedDepthDesc.pTexture->GetDesc().Width != desc.Width ||
                vr->foveatedDepthDesc.pTexture->GetDesc().Height != desc.Height)) {
            vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, desc.Format, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->foveatedDepthDesc, false);
        }
    }

    auto cmdList = vr->d3d12Renderer->BeginCommandList(backbuffer_index);

    if ((vr->is_using_any_afw()) && m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture && vr->depthTex[0]) {
        static FrameBufferDesc s_CurrentEyeFrameBuffer{};

        s_CurrentEyeFrameBuffer.color = texDesc[texIndex];
        s_CurrentEyeFrameBuffer.depth = vr->depthDesc[0];
        s_CurrentEyeFrameBuffer.motionVectors = vr->motionVectorsDesc;

	    if (!vr->get_runtime()->hiddenAreaMeshVextexBuffer[0].pVextexBuffer && vr->get_runtime()->hiddenAreaMesh[0].pVertexData) {
            UINT vertexSize = sizeof(vr::HmdVector2_t);
            vr->d3d12Renderer->CreateVertexBuffer(cmdList, vr->get_runtime()->hiddenAreaMeshVextexBuffer[0], vr->get_runtime()->hiddenAreaMesh[0].unTriangleCount * 3, vertexSize,
                (float*)vr->get_runtime()->hiddenAreaMesh[0].pVertexData);
            vr->d3d12Renderer->CreateVertexBuffer(cmdList, vr->get_runtime()->hiddenAreaMeshVextexBuffer[1], vr->get_runtime()->hiddenAreaMesh[1].unTriangleCount * 3, vertexSize,
                (float*)vr->get_runtime()->hiddenAreaMesh[1].pVertexData);
        }

        params.InCmdList = cmdList;
        params.InEyeFrameBuffer = &s_CurrentEyeFrameBuffer;
        params.InUIColorAlpha = NULL;
        params.IsHudlessColor = true;
        params.MotionVectorsType = vr->is_fix_dlss()? Normal : FromOtherEye;
        params.InMotionScale[0] = (float)colorDesc.Width;
        params.InMotionScale[1] = (float)colorDesc.Height;
        params.Mode = (FrameWarpMode)vr->m_framewarp_mode->value();
        params.EyeIndex = nEye;
        params.ClearBeforeWarping = vr->m_clear_before_framewarp->value();
        params.CameraData = &vr->cameraData[nEye];
        params.IgnoreMotionThreshold = vr->m_ignore_motion_threshold->value();
        params.Debug = vr->m_framewarp_debug->value();

        if (!vr->is_foveated_rendering()) {
            if (vr->m_enable_ui_fix->value() && vr->uiBufferDesc[0].pTexture) {
                params.InUIColorAlpha = &vr->uiBufferDesc[0];
                params.IsHudlessColor = false;
            }
            // Sharpening
            if (vr->is_enable_sharpening() && vr->get_sharpness() > 0) {
                vr->d3d12Renderer->Sharpen(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, vr->get_sharpness());
                s_CurrentEyeFrameBuffer.color = eyeFrameBuffer.color;
            }
            EvaluateFrameWarp(params);
        } else if (vr->is_using_afw_foveated()) {
            auto foveatedVP = vr->get_runtime()->foveated_viewports[nEye];
            D3D12_VIEWPORT vp;
            vp.TopLeftX = foveatedVP.TopLeftX * colorDesc.Width;
            vp.TopLeftY = foveatedVP.TopLeftY * colorDesc.Height;
            vp.Width = foveatedVP.Width * colorDesc.Width;
            vp.Height = foveatedVP.Height * colorDesc.Height;
            vp.MinDepth = 0;
            vp.MaxDepth = 1;
            FLOAT black[4] = {0, 0, 0, 0};
            if (vr->multipassBackupDesc[nEye].pTexture) {
                float edgeCutoutRatio = vr->get_edge_scan_line_fix_range();
                float blurRadius = vr->get_foveated_periphery_blur();
                edgeCutoutRatio = 1.0f - 1.0f / (1.0f + edgeCutoutRatio);
                auto desc = s_CurrentEyeFrameBuffer.color.pTexture->GetDesc();
                D3D12_BOX srcBox = (nEye == EyeRight) ? CD3DX12_BOX(edgeCutoutRatio * desc.Width, 0, desc.Width, desc.Height)
                                                      : CD3DX12_BOX(0, 0, (1 - edgeCutoutRatio) * desc.Width, desc.Height);
                if (vr->mDebug1 || (edgeCutoutRatio <= 0 && blurRadius <= 0)) {
                    vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color);
                    vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.depth, vr->depthDesc[1]);
                }else if (edgeCutoutRatio > 0 && blurRadius <= 0) {
                    vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, srcBox);
                    vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.depth, vr->depthDesc[1], srcBox);
                } else if (edgeCutoutRatio > 0 && blurRadius > 0) {
                    vr->d3d12Renderer->Crop(cmdList, otherEyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, srcBox);
                    vr->d3d12Renderer->Blur(cmdList, eyeFrameBuffer.color, otherEyeFrameBuffer.color, blurRadius);
                    vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.depth, vr->depthDesc[1], srcBox);
                } else {
                    vr->d3d12Renderer->Blur(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, blurRadius);
                    vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.depth, vr->depthDesc[1]);
                }
                vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.depth, vr->foveatedDepthDesc, vp);

                if (vr->m_enable_ui_fix->value() && vr->multipassUIBufferDesc.pTexture) {
                    if (edgeCutoutRatio > 0) {
                        vr->d3d12Renderer->Crop(cmdList, vr->multipassUIBufferDesc, vr->uiBufferDesc[1], srcBox);
                    } else {
                        vr->d3d12Renderer->Blit(cmdList, vr->multipassUIBufferDesc, vr->uiBufferDesc[1]);
                    }
                    params.InUIColorAlpha = &vr->multipassUIBufferDesc;
                    params.IsHudlessColor = false;
                }
                TonemapParams params;
                params.fGamma = 1.10f;
                params.fLowerLimit = 0.024f;
                params.fUpperLimit = 0.982f;
                params.fConvertToLimit = 0.958f;
                vr->d3d12Renderer->Tonemap(cmdList, vr->multipassBackupDesc[nEye], vr->uiBufferDesc[0], params);
                vr->d3d12Renderer->FoveatedComposite(cmdList, eyeFrameBuffer.color, vr->multipassBackupDesc[nEye], vp, vr->get_foveated_composite_params());
            } else {
                vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color);
                if (vr->m_enable_ui_fix->value() && vr->multipassUIBufferDesc.pTexture) {
                    vr->d3d12Renderer->Blit(cmdList, vr->multipassUIBufferDesc, vr->uiBufferDesc[1]);
                    params.InUIColorAlpha = &vr->multipassUIBufferDesc;
                    params.IsHudlessColor = false;
                }
            }


            vr->d3d12Renderer->Clear(cmdList, eyeFrameBuffer.motionVectors, black);
            if (vr->motionVectorsDesc.pTexture && vr->motionVectorsCorrectedDesc.pTexture) {
                CorrectMotionVectorsParams mvparams;
                mvparams.InMotionVectors = &vr->motionVectorsDesc;
                mvparams.InDepth = &vr->foveatedDepthDesc;
                mvparams.CameraData = &vr->cameraDataForMV[nEye];
                mvparams.InMotionScale[0] = (float)vr->motionVectorsDesc.pTexture->GetDesc().Width;
                mvparams.InMotionScale[1] = (float)vr->motionVectorsDesc.pTexture->GetDesc().Height;
                mvparams.CorrectMVType = ExtractObjectMotion;
                vr->d3d12Renderer->CorrectMotionVectors(cmdList, vr->motionVectorsCorrectedDesc, mvparams);
                vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.motionVectors, vr->motionVectorsCorrectedDesc, vp);
            }

            // Sharpening
            if (vr->is_enable_sharpening() && vr->get_sharpness() > 0) {
                vr->d3d12Renderer->Sharpen(cmdList, otherEyeFrameBuffer.color, eyeFrameBuffer.color, vr->get_sharpness());
                vr->d3d12Renderer->Copy(cmdList, eyeFrameBuffer.color, otherEyeFrameBuffer.color);
            }
            if (vr->m_desktop_fix->value()) {
                vr->d3d12Renderer->Blit(cmdList, texDesc[texIndex], eyeFrameBuffer.color);
            }
            s_CurrentEyeFrameBuffer.color = eyeFrameBuffer.color;
            s_CurrentEyeFrameBuffer.depth = eyeFrameBuffer.depth;
            s_CurrentEyeFrameBuffer.motionVectors = eyeFrameBuffer.motionVectors;
            params.InEyeFrameBuffer = &s_CurrentEyeFrameBuffer;
            params.MotionVectorsType = ObjectOnly;
            params.isFoveated = true;
            auto foveatedVPOther = vr->get_runtime()->foveated_viewports[nEyeOther];
            D3D12_VIEWPORT vpOther;
            vpOther.TopLeftX = foveatedVPOther.TopLeftX * colorDesc.Width;
            vpOther.TopLeftY = foveatedVPOther.TopLeftY * colorDesc.Height;
            vpOther.Width = foveatedVPOther.Width * colorDesc.Width;
            vpOther.Height = foveatedVPOther.Height * colorDesc.Height;
            params.foveatedArea = RECT(vpOther.TopLeftX, vpOther.TopLeftY, vpOther.TopLeftX + vpOther.Width, vpOther.TopLeftY + vpOther.Height);
            EvaluateFrameWarp(params);
        }

        for (int i = 0; i < 2; i++) {
            if (vr->uiBufferDesc[i].pTexture) {
                D3D12_RESOURCE_BARRIER barriers1[] = {CD3DX12_RESOURCE_BARRIER::Transition(
                    vr->uiBufferDesc[i].pTexture, vr->uiBufferDesc[i].initialState, D3D12_RESOURCE_STATE_RENDER_TARGET)};
                FLOAT black[4] = {0, 0, 0, 0};
                cmdList->ResourceBarrier(_countof(barriers1), barriers1);

                cmdList->ClearRenderTargetView(vr->uiBufferDesc[i].renderTargetViewHandle, black, 0, NULL);

                D3D12_RESOURCE_BARRIER barriers2[] = {CD3DX12_RESOURCE_BARRIER::Transition(
                    vr->uiBufferDesc[i].pTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, vr->uiBufferDesc[i].initialState)};
                cmdList->ResourceBarrier(_countof(barriers2), barriers2);
            }
        }
    } else {
        vr->d3d12Renderer->Blit(cmdList, eyeFrameBuffer.color, texDesc[texIndex]);
        for (int i = 0; i < 2; i++) {
            if (vr->uiBufferDesc[i].pTexture) {
                D3D12_RESOURCE_BARRIER barriers1[] = {CD3DX12_RESOURCE_BARRIER::Transition(
                    vr->uiBufferDesc[i].pTexture, vr->uiBufferDesc[i].initialState, D3D12_RESOURCE_STATE_RENDER_TARGET)};
                FLOAT black[4] = {0, 0, 0, 0};
                cmdList->ResourceBarrier(_countof(barriers1), barriers1);

                cmdList->ClearRenderTargetView(vr->uiBufferDesc[i].renderTargetViewHandle, black, 0, NULL);

                D3D12_RESOURCE_BARRIER barriers2[] = {CD3DX12_RESOURCE_BARRIER::Transition(
                    vr->uiBufferDesc[i].pTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, vr->uiBufferDesc[i].initialState)};
                cmdList->ResourceBarrier(_countof(barriers2), barriers2);
            }
        }
    }

    vr->d3d12Renderer->EndCommandList(backbuffer_index);

    //#############################
    //#Frame Warp Module End
    //#############################

    //bool submitFrame = (vr->is_using_sf() || vr->is_using_any_afw());
    //if (!submitFrame) {
    //    hook->ignore_next_present();
    //    return vr::EVRCompositorError::VRCompositorError_None;
    //}

    auto eye_format = backbuffer->GetDesc().Format;

    if (runtime->is_openxr()) {
        if (eye_format != m_openxr.last_format) {
            spdlog::info("[VR] OpenXR format changed from {} to {}", m_openxr.last_format, eye_format);
            m_openxr.create_swapchains();
        }
    } else {
        if (eye_format != m_openvr.last_format) {
            spdlog::info("[VR] OpenVR format changed from {} to {}", m_openvr.last_format, eye_format);
            on_reset(vr);
            setup();
        }
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (frame_count % 2 == vr->m_left_eye_interval) {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            m_openxr.copy(0, m_openvr.get_left().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            if (vr->is_using_any_afw()) {
                m_openxr.copy(1, m_openvr.get_right().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture (m_left_eye_tex0 holds the intermediate frame).
        if (runtime->is_openvr()) {
            //m_openvr.copy_left(eye_texture.Get());

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::Texture_t left_eye{(void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
            if (vr->is_using_any_afw()) {
                //auto& ctx = m_openvr.acquire_right();
                //if (params.OutEyeColor && vr->m_use_reprojection->value()) {
                //    ctx.commands.copy((ID3D12Resource*)params.OutEyeColor, ctx.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                //}
                //ctx.commands.execute();

                vr::D3D12TextureData_t right{m_openvr.get_right().texture.Get(), command_queue, 0};

                vr::Texture_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

                auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &vr->m_right_bounds);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                    return e;
                } else {
                    vr->m_submitted = true;
                }

                ++m_openvr.texture_counter;
            }
        }
    } else {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            m_openxr.copy(1, m_openvr.get_right().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            if (vr->is_using_any_afw()) {
                m_openxr.copy(0, m_openvr.get_left().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the right eye texture.
        if (runtime->is_openvr()) {
            //m_openvr.copy_right(eye_texture.Get());

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::Texture_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &vr->m_right_bounds);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }
            if (vr->is_using_any_afw()) {
                //auto& ctx = m_openvr.acquire_left();
                //if (params.OutEyeColor && vr->m_use_reprojection->value()) {
                //    ctx.commands.copy((ID3D12Resource*)params.OutEyeColor, ctx.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                //}
                //ctx.commands.execute();

                vr::D3D12TextureData_t left{m_openvr.get_left().texture.Get(), command_queue, 0};

                vr::Texture_t left_eye{(void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &vr->m_left_bounds);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    return e;
                }
            }
            ++m_openvr.texture_counter;
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (frame_count % 2 == vr->m_right_eye_interval || vr->is_using_any_afw()) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
            runtime->synchronize_frame();

            if (!runtime->got_first_poses) {
                runtime->update_poses();
            }
        }

        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

#if defined(PRAGMATA)
            pragmata_update_hud_quad(vr->m_openxr.get());
            pragmata_update_menu_quad(vr->m_openxr.get());
#endif

            auto result = vr->m_openxr->end_frame();

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

#if defined(PRAGMATA)
                // Never let the menu layer cost the game its frames: drop it and use the eye-image fallback.
                // Build 18: the runtime rejects some frames while VR starts up whatever layers are sent (Build 17 blamed the
                // HUD panel for those and switched it off for good). Drop the HUD panel for this retry, and give up on it
                // only if frames keep being rejected while it is shown.
                static int hud_rejections = 0;
                if (vr->m_openxr->hud_quad.visible) {
                    vr->m_openxr->hud_quad.visible = false;
                    if (++hud_rejections >= 120) {
                        spdlog::error("[PragmataVR] Runtime keeps rejecting frames with the HUD panel layer; HUD panel turned off");
                        g_pragmata_hud.failed = true;
                    } else if (hud_rejections == 1 || hud_rejections % 30 == 0) {
                        spdlog::warn("[PragmataVR] Frame rejected while the HUD panel was shown ({} so far); retrying without it", hud_rejections);
                    }
                } else if (vr->m_openxr->menu_quad.visible) {
                    spdlog::error("[PragmataVR] Runtime rejected the menu quad layer; switching to the eye-image menu");
                    vr->m_openxr->menu_quad.visible = false;
                    g_pragmata_quad.failed = true;
                }
#endif

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame();
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        if (vr->m_desktop_fix->value() && frame_count % 2 == vr->m_right_eye_interval) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                auto& copier = m_generic_copiers[frame_count % m_generic_copiers.size()];
                copier.wait(INFINITE);
                copier.copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                copier.execute();
            }
        }
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

void D3D12Component::on_post_present(VR* vr) {
}

void D3D12Component::on_reset(VR* vr) {
    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& copier : m_generic_copiers) {
        copier.reset();
    }
    
    m_prev_backbuffer.Reset();
    m_backbuffer_copy.reset();
    m_converted_eye_tex.reset();

    if (runtime->is_openxr() && runtime->loaded) {
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            m_openxr.create_swapchains();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_openvr.texture_counter = 0;
}

void D3D12Component::setup() {

    if (VR::get()->is_hmd_active()) {
        spdlog::info("[VR] Setting up d3d12 textures...");
    }
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};

    const auto& vr = VR::get();

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    if (backbuffer == nullptr) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return;
    }

    auto backbuffer_desc = backbuffer->GetDesc();
    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    backbuffer_desc.Width = real_backbuffer_desc.Width;
    backbuffer_desc.Height = real_backbuffer_desc.Height;

    m_openvr.last_format = backbuffer_desc.Format;

    spdlog::info("[VR] D3D12 Backbuffer width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, backbuffer_desc.Format);
    spdlog::info("[VR] D3D12 Real Backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, real_backbuffer_desc.Format);

    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || backbuffer_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

    auto backbuffer_srv_desc = backbuffer_desc;
    backbuffer_srv_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_srv_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    // Create copy of backbuffer to use as SRV to convert from HDR to 8bit
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> backbuffer_copy{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_srv_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(backbuffer_copy.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up backbuffer copy texture RTV/SRV.");
        }
    }

    auto rt_desc = backbuffer_desc;

    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    switch (backbuffer_desc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;

        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        
        default:
            spdlog::error("[OpenVR] Possibly unsupported backbuffer format: {}", backbuffer_desc.Format);
            break;
    };

    //#############################
    //#Frame Warp Module Start
    //#############################
    static uint32_t lastSize[2]{0, 0};
    static DXGI_FORMAT lastFormat = DXGI_FORMAT_UNKNOWN;
    if ((lastSize[0] != vr->get_hmd_width() || lastSize[1] != vr->get_hmd_height() || lastFormat != rt_desc.Format)) {
        FrameWarpInitParams params = {vr->get_hmd_width(), vr->get_hmd_height(), rt_desc.Format};
        m_eyeFrameBuffers = InitFrameWarp(params);
        lastSize[0] = vr->get_hmd_width();
        lastSize[1] = vr->get_hmd_height();
    }
    //#############################
    //#Frame Warp Module End
    //#############################

    // Create converted eye texture
    if (!m_backbuffer_is_8bit && false) {
        ComPtr<ID3D12Resource> eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(eye_tex.GetAddressOf())))) {
            spdlog::error("[VR] Failed to create converted eye texture.");
            return;
        }

        if (!m_converted_eye_tex.setup(device, eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up converted eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.left_eye_tex) {
        ComPtr<ID3D12Resource> left_eye_tex{};
        if (m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture != NULL) {
            left_eye_tex = m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture;
        } else {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(left_eye_tex.GetAddressOf())))) {
                spdlog::error("[VR] Failed to create left eye texture.");
                return;
            }
        }

        left_eye_tex->SetName(L"OpenVR Left Eye Texture");
        if (!ctx.setup(device, left_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up left eye texture RTV/SRV.");
        }
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ComPtr<ID3D12Resource> right_eye_tex{};
        if (m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture != NULL) {
            right_eye_tex = m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture;
        } else {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(right_eye_tex.GetAddressOf())))) {
                spdlog::error("[VR] Failed to create right eye texture.");
                return;
            }
        }

        right_eye_tex->SetName(L"OpenVR Right Eye Texture");
        if (!ctx.setup(device, right_eye_tex.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Error setting up right eye texture RTV/SRV.");
        }
    }

    for (auto& copier : m_generic_copiers) {
        copier.setup();
    }

    setup_sprite_batch_pso(rt_desc.Format);

    m_backbuffer_size[0] = real_backbuffer_desc.Width;
    m_backbuffer_size[1] = real_backbuffer_desc.Height;

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;
}

void D3D12Component::setup_sprite_batch_pso(DXGI_FORMAT output_format) {
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    DirectX::RenderTargetState output_state{output_format, DXGI_FORMAT_UNKNOWN};
    DirectX::SpriteBatchPipelineStateDescription pd{output_state};

    m_sprite_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");
}

void D3D12Component::render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    const auto dst_desc = dst.texture->GetDesc();
    const auto src_desc = src.texture->GetDesc();
    
    auto& batch = m_sprite_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)dst_desc.Width;
    viewport.Height = (float)dst_desc.Height;
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)dst_desc.Width;
    scissor_rect.bottom = (LONG)dst_desc.Height;

    // Transition dst to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst.texture.Get();

    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
    }

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { dst.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)dst_desc.Width, (LONG)dst_desc.Height };

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { src.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(src.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)src_desc.Width, (uint32_t)src_desc.Height },
        dest_rect,
        DirectX::Colors::White);

    batch->End();

    // Transition dst to dst_state
    if (dst_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = dst_state;
        command_list->ResourceBarrier(1, &barrier);
    }
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& vr = VR::get();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();
    this->contexts.resize(openxr->views.size());

    this->last_format = backbuffer_desc.Format;

    DXGI_FORMAT swapchain_format{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};

    switch (backbuffer_desc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;

        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            swapchain_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            break;
        
        default:
            spdlog::error("[VR] Possibly unsupported backbuffer format: {}", backbuffer_desc.Format);
            break;
    };
    
    for (auto i = 0; i < openxr->views.size(); ++i) {
        spdlog::info("[VR] Creating swapchain for eye {}", i);
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        backbuffer_desc.Width = vr->get_hmd_width();
        backbuffer_desc.Height = vr->get_hmd_height();

        if (swapchain_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else {
            backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        spdlog::info("[VR] Format: {}", backbuffer_desc.Format);

        // Create the swapchain.
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = swapchain_format;
        swapchain_create_info.width = backbuffer_desc.Width;
        swapchain_create_info.height = backbuffer_desc.Height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains.push_back(swapchain);

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        spdlog::info("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR Commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    for (auto i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];
        ctx.texture_contexts.clear();

        auto result = xrDestroySwapchain(VR::get()->m_openxr->swapchains[i].handle);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to destroy swapchain {}.", i);
        } else {
            spdlog::info("[VR] Destroyed swapchain {}.", i);
        }

        ctx.textures.clear();
    }

    this->contexts.clear();
    VR::get()->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    D3D12_BOX* src_box, 
    D3D12_RESOURCE_STATES src_state)
{

    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->m_openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);
            if (src_box != nullptr) {
                texture_ctx->commands.copy_region(
                    resource, 
                    ctx.textures[texture_index].texture, 
                    src_box, src_state, 
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            } else {
                texture_ctx->commands.copy(
                    resource, 
                    ctx.textures[texture_index].texture, 
                    src_state, 
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
        }
    }
}
} // namespace vrmod
