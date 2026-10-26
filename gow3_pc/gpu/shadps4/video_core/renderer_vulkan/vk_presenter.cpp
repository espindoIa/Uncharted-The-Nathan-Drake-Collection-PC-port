// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "gow3_flight.h"
#include "gow3_overlay.h"
#include "gow3_reflex.h"
#include "gow3_settings.h"
#include "gow3_streamline.h"
#include "gow3_toggles.h"
#include "gow3_graphics.h"
#include "gow3_trigger_files.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/amdgpu/liverpool.h" // gow3 21 Oct 2026: ServiceQueuedCommands
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>
#include <vk_mem_alloc.h>
#include "video_core/renderer_vulkan/vk_submit_log.h"
#ifdef MemoryBarrier
#undef MemoryBarrier // gow3: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

namespace {
/// gow3 20 Oct 2026: presents on their own queue (Instance::SeparatePresentQueue) take that
/// queue's lock only; on the graphics queue the submit lock, as before.
std::mutex& PresentLock(const Instance& instance) {
    static std::mutex present_queue_mutex;
    return instance.SeparatePresentQueue() ? present_queue_mutex : Scheduler::submit_mutex;
}
} // namespace

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// gow3: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();
    draw_scheduler.SetLogId(0);
    present_scheduler.SetLogId(1);
    flip_scheduler.SetLogId(2);

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }
    rasterizer->GetUpscaler().SetFrameGenRing(num_images + 1);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format,
                   vk::Format::eA2B10G10R10UnormPack32);
    Gow3Overlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);
    Gow3Settings::Get().hdr_supported = swapchain.HasHDR();
    // Before the pipeline warm-up below: SDR sessions build none of the HDR10 pipelines.
    Gow3Settings::Get().hdr_pipelines = Gow3Settings::Get().hdr && swapchain.HasHDR();

    const bool binary_cache = EmulatorSettingsImpl::Flag("GOW3_PIPELINE_BINARIES", false) &&
                              EmulatorSettings.IsPipelineCacheEnabled();
    const bool capture_only = EmulatorSettingsImpl::Flag("GOW3_PIPELINE_BINARY_ISOLATED", false) &&
                              EmulatorSettingsImpl::Flag("GOW3_PIPELINE_CAPTURE_ONLY", false);
    // gow3 18 Oct 2026: the background warm-up also with pipeline binaries. On demand, each compute
    // shader was translated on the GPU thread the first time a level used it: Uncharted 3 hitched
    // 90-170 ms right after loading (35 compiles of 10-15 ms in 5 s); with the warm-up (2,440
    // pipelines in 16 s on background threads during the title screen) none were left in play.
    // GOW3_ON_DEMAND_PIPELINES=1 keeps the on-demand start.
    if (EmulatorSettingsImpl::Flag("GOW3_ON_DEMAND_PIPELINES", false) &&
        UseOnDemandPipelines(binary_cache, instance.GetPipelineBinaryGlobalKey().keySize != 0,
                            capture_only)) {
        rasterizer->GetPipelineCache().StartOnDemand();
        return;
    }

    // gow3: by default the cached pipelines load while the game runs (a corner note shows the
    // progress). Otherwise rebuilding them takes up to minutes before the first game frame:
    // show what is happening instead of a black window.
    const char* background_warmup = std::getenv("GOW3_BACKGROUND_WARMUP");
    if ((!background_warmup || *background_warmup != '0') &&
        rasterizer->GetPipelineCache().StartBackgroundWarmUp()) {
        return;
    }
    Gow3Overlay::SetLoading(true, 0, 0);
    PresentLoadingFrame();
    auto last = std::chrono::steady_clock::now();
    rasterizer->GetPipelineCache().WarmUp([&](u32 done, u32 total) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(250)) {
            return;
        }
        last = now;
        Gow3Overlay::SetLoading(true, done, total);
        PresentLoadingFrame();
    });
    Gow3Overlay::SetLoading(false, 0, 0);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    if (overlay_image) {
        device.destroyImageView(overlay_view);
        vmaDestroyImage(instance.GetAllocator(), overlay_image, overlay_allocation);
    }
}

void Presenter::RenderOverlay(vk::CommandBuffer cmdbuf, vk::Extent2D extent) {
    if (!swapchain.GetHDR()) {
        Gow3Overlay::Render(cmdbuf, swapchain.ImageView(), extent);
        return;
    }
    // HDR10: the menu keeps its 8-bit pipeline, drawn into a cleared image of the window's size.
    const vk::Device device = instance.GetDevice();
    if (!overlay_image || overlay_extent != extent) {
        if (overlay_image) {
            // vkDeviceWaitIdle excludes queue submissions; the game scheduler submits on another
            // thread (as in Swapchain::Destroy). The present lock is only taken later, at submit.
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            Check(device.waitIdle());
            device.destroyImageView(overlay_view);
            vmaDestroyImage(instance.GetAllocator(), overlay_image, overlay_allocation);
        }
        const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = swapchain.GetSurfaceFormat().format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        });
        const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                           &overlay_allocation, nullptr) != VK_SUCCESS) {
            overlay_image = vk::Image{};
            return;
        }
        overlay_image = vk::Image{image};
        overlay_view = Check<"create overlay view">(device.createImageView(vk::ImageViewCreateInfo{
            .image = overlay_image,
            .viewType = vk::ImageViewType::e2D,
            .format = swapchain.GetSurfaceFormat().format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        overlay_extent = extent;
    }
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = overlay_image,
                                   .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(overlay_image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
                           vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
    Gow3Overlay::Render(cmdbuf, overlay_view, extent);
    barrier(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead);
    pp_pass.RenderOverlay(cmdbuf, overlay_view, swapchain.ImageView(), extent, pp_settings);
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    // gow3: HDR10 frames keep the 10-bit format up to the swapchain (an 8-bit frame would band).
    const vk::Format format = swapchain.GetImageFormat();
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// gow3: raw copy of an image for GOW3_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index, u32 pixel_bytes = 4,
                    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor,
                    u32 block_width = 1, u32 block_height = 1, u32 layer = 0) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize((w + block_width - 1) / block_width) *
                                               ((h + block_height - 1) / block_height) * pixel_bytes,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {aspect, 0, layer, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("GOW3_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

// gow3: GOW3_LIVE_VIEW=<dir>: every GOW3_LIVE_VIEW_MS (default 200) the presented image, scaled to
// GOW3_LIVE_VIEW_WIDTH (default 960) pixels wide, goes to <dir>/live.bmp (written to a temporary
// file and renamed, so a reader never sees half a frame), and <dir>/live.txt says "frame <n> ms <t>
// <w>x<h>" (n counts presented frames, t is milliseconds since the first capture). Scripted play
// (tools/play.py) watches the game this way; the window needs no focus and may be covered.
namespace {
struct LiveView {
    std::string dir;
    u32 interval_ms = 200;
    u32 width = 960;
    bool failed = false;
    std::chrono::steady_clock::time_point last{}, start{};
    std::atomic<bool> busy{false};
    vk::Image image{};
    VmaAllocation image_allocation{};
    VkBuffer buffer{};
    VmaAllocation buffer_allocation{};
    VmaAllocationInfo buffer_info{};
    u32 w = 0, h = 0, src_w = 0, src_h = 0;
    vk::Format format{};
};

LiveView* LiveViewConfig() {
    static LiveView* view = [] () -> LiveView* {
        const char* dir = std::getenv("GOW3_LIVE_VIEW");
        if (!dir || !*dir) return nullptr;
        auto* v = new LiveView{};
        v->dir = dir;
        if (const char* ms = std::getenv("GOW3_LIVE_VIEW_MS")) v->interval_ms = std::max(20, std::atoi(ms));
        if (const char* w = std::getenv("GOW3_LIVE_VIEW_WIDTH")) v->width = std::clamp(std::atoi(w), 64, 3840);
        std::error_code ec;
        std::filesystem::create_directories(v->dir, ec);
        std::printf("Live view: %s every %u ms, %u pixels wide\n", v->dir.c_str(), v->interval_ms, v->width);
        return v;
    }();
    return view;
}

// 24-bit bottom-up BMP from the captured pixels (8-bit BGRA/RGBA or 10-bit packed).
void WriteLiveView(LiveView* v, std::vector<u8> pixels, u32 w, u32 h, vk::Format format, u64 frame,
                   long long ms) {
    const u32 row = (w * 3 + 3) & ~3u;
    std::vector<u8> bmp(54 + size_t(row) * h, 0);
    auto put32 = [&](size_t at, u32 value) { std::memcpy(&bmp[at], &value, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, u32(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    bmp[26] = 1;
    bmp[28] = 24;
    put32(34, u32(row) * h);
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    const bool abgr10 = format == vk::Format::eA2B10G10R10UnormPack32;
    const bool argb10 = format == vk::Format::eA2R10G10B10UnormPack32;
    for (u32 y = 0; y < h; ++y) {
        const u8* src = pixels.data() + size_t(y) * w * 4;
        u8* dst = bmp.data() + 54 + size_t(h - 1 - y) * row;
        for (u32 x = 0; x < w; ++x, src += 4, dst += 3) {
            u8 r, g, b;
            if (abgr10 || argb10) {
                u32 p;
                std::memcpy(&p, src, 4);
                const u8 lo = u8((p >> 2) & 0xff), mid = u8((p >> 12) & 0xff), hi = u8((p >> 22) & 0xff);
                r = abgr10 ? lo : hi;
                g = mid;
                b = abgr10 ? hi : lo;
            } else if (bgra) {
                b = src[0], g = src[1], r = src[2];
            } else {
                r = src[0], g = src[1], b = src[2];
            }
            dst[0] = b, dst[1] = g, dst[2] = r;
        }
    }
    const auto bmp_path = std::filesystem::path(v->dir) / "live.bmp";
    const auto tmp_path = std::filesystem::path(v->dir) / "live.bmp.tmp";
    if (FILE* f = std::fopen(tmp_path.string().c_str(), "wb")) {
        std::fwrite(bmp.data(), 1, bmp.size(), f);
        std::fclose(f);
        std::error_code ec;
        std::filesystem::rename(tmp_path, bmp_path, ec);
        const auto txt = std::filesystem::path(v->dir) / "live.txt";
        const auto txt_tmp = std::filesystem::path(v->dir) / "live.txt.tmp";
        if (FILE* t = std::fopen(txt_tmp.string().c_str(), "w")) {
            std::fprintf(t, "frame %llu ms %lld %ux%u\n", static_cast<unsigned long long>(frame), ms, w, h);
            std::fclose(t);
            std::filesystem::rename(txt_tmp, txt, ec);
        }
    }
}

// Records the scaled copy of the presented image (General layout after the post process pass).
void CaptureLiveView(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                     vk::Image src, u32 src_w, u32 src_h, vk::Format format) {
    static std::atomic<u64> presented{0};
    const u64 frame = ++presented;
    LiveView* v = LiveViewConfig();
    if (!v || v->failed || !src_w || !src_h) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - v->last < std::chrono::milliseconds(v->interval_ms) || v->busy.load()) return;
    if (v->start == std::chrono::steady_clock::time_point{}) v->start = now;
    v->last = now;
    const u32 w = std::min(v->width, src_w);
    const u32 h = std::max(1u, u32(u64(src_h) * w / src_w));
    if (v->image && (v->w != w || v->h != h || v->format != format)) {
        vmaDestroyImage(instance.GetAllocator(), VkImage(v->image), v->image_allocation);
        vmaDestroyBuffer(instance.GetAllocator(), v->buffer, v->buffer_allocation);
        v->image = vk::Image{};
    }
    if (!v->image) {
        const auto props = instance.GetPhysicalDevice().getFormatProperties(format);
        const auto need = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst |
                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        if ((props.optimalTilingFeatures & need) != need) {
            std::printf("Live view: off (%s cannot be scaled by a blit)\n", vk::to_string(format).c_str());
            v->failed = true;
            return;
        }
        const VkImageCreateInfo image_ci{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VkFormat(format),
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        const VmaAllocationCreateInfo image_alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_ci, &image_alloc, &image,
                           &v->image_allocation, nullptr) != VK_SUCCESS) {
            v->failed = true;
            return;
        }
        const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                           .size = VkDeviceSize(w) * h * 4,
                                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo buffer_alloc{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &buffer_alloc, &v->buffer,
                            &v->buffer_allocation, &v->buffer_info) != VK_SUCCESS) {
            vmaDestroyImage(instance.GetAllocator(), image, v->image_allocation);
            v->failed = true;
            return;
        }
        v->image = vk::Image{image};
        v->w = w, v->h = h, v->format = format;
    }
    const vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::MemoryBarrier2 written{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                     .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                     .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                     .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
    const vk::ImageMemoryBarrier2 to_dst{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                         .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                         .oldLayout = vk::ImageLayout::eUndefined,
                                         .newLayout = vk::ImageLayout::eTransferDstOptimal,
                                         .image = v->image,
                                         .subresourceRange = color};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                             .pMemoryBarriers = &written,
                             .imageMemoryBarrierCount = 1,
                             .pImageMemoryBarriers = &to_dst});
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{int(src_w), int(src_h), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{int(w), int(h), 1}},
    };
    cmdbuf.blitImage(src, vk::ImageLayout::eGeneral, v->image, vk::ImageLayout::eTransferDstOptimal,
                     region, vk::Filter::eLinear);
    const vk::ImageMemoryBarrier2 to_src{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                         .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                                         .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                                         .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                                         .image = v->image,
                                         .subresourceRange = color};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_src});
    cmdbuf.copyImageToBuffer(v->image, vk::ImageLayout::eTransferSrcOptimal, v->buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    v->busy = true;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - v->start).count();
    scheduler.DeferPriorityOperation([v, allocator = instance.GetAllocator(), w, h, format, frame, ms] {
        vmaInvalidateAllocation(allocator, v->buffer_allocation, 0, VK_WHOLE_SIZE);
        const u8* mapped = static_cast<const u8*>(v->buffer_info.pMappedData);
        std::vector<u8> pixels(mapped, mapped + size_t(w) * h * 4);
        // The file write runs off the GPU threads; the next capture waits for it (busy).
        std::thread([v, pixels = std::move(pixels), w, h, format, frame, ms]() mutable {
            WriteLiveView(v, std::move(pixels), w, h, format, frame, ms);
            v->busy = false;
        }).detach();
    });
}
} // namespace

static void WatchGpuDone(Semaphore* semaphore, u64 tick);

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // gow3: GOW3_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to GOW3_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    // The file is polled off this thread (gow3_trigger_files.cpp; a remove() here every frame).
    static const int frame_dump_trigger = Gow3TriggerFiles::Watch("GOW3_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    const bool frame_dump = Gow3TriggerFiles::Take(frame_dump_trigger).has_value();
    // gow3: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    // 20 Oct audit: warm-up was pumped only on pipeline lookups (U1: entries waited ~4.8 s); PrepareFrame
    // runs on the GPU command thread once per flip, like the lookups.
    rasterizer->GetPipelineCache().PumpWarmUp();
    SubmitLog::Event("F0", "%llx", static_cast<unsigned long long>(cpu_address));
    Frame* frame = GetRenderFrame();
    SubmitLog::Event("F1", "%u", frame->id);
    // gow3 22 Oct 2026: the frame's Reflex id is its flip's ordinal (the guest counts the same
    // flips when it asks for them, Gow3Reflex::GuestFlipRequest).
    frame->serial = Gow3Reflex::PreparedFlip();
    // gow3 22 Oct 2026: the upscale this flip shows recorded them before (same command stream).
    frame->frame_gen = rasterizer->GetUpscaler().TakeFrameGenInputs(frame->frame_gen_inputs);

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    // Optional one-shot request: guest address and exact guest size per line, or
    // "size <width> <height>" for every cached image of that size, "max <width> <height>" for every
    // one up to that size, "range <lo> <hi>" (hex) for every one in those guest addresses (up to 256
    // in all).
    // Dump intermediate targets to locate where geometry/lighting disappears.
    if (frame_dump) {
        const char* dir = std::getenv("GOW3_DUMP_DIR");
        const auto request = std::filesystem::path(dir ? dir : "out/dump") / "guest-images.trigger";
        // 20 Oct 2026 audit: fopen on every frame while frame_dump is on; poll 4 times a second. The
        // file holds several lines (Gow3TriggerFiles::Take returns only the first), so no watcher.
        static std::chrono::steady_clock::time_point checked{};
        const auto now = std::chrono::steady_clock::now();
        const bool poll = now - checked > std::chrono::milliseconds(250);
        if (poll) {
            checked = now;
        }
        if (FILE* f = poll ? std::fopen(request.string().c_str(), "r") : nullptr) {
            std::vector<std::pair<unsigned long long, unsigned long long>> requests;
            char line[256];
            constexpr size_t MaxRequests = 256;
            while (requests.size() < MaxRequests && std::fgets(line, sizeof(line), f)) {
                unsigned long long a, b;
                unsigned w, h;
                std::vector<std::pair<VAddr, u32>> images;
                if (std::sscanf(line, "size %u %u", &w, &h) == 2) {
                    images = texture_cache.DebugImagesOfSize(w, h);
                } else if (std::sscanf(line, "max %u %u", &w, &h) == 2) {
                    images = texture_cache.DebugImagesWhere(16, w, 16, h, 0, ~VAddr{0});
                } else if (std::sscanf(line, "range %llx %llx", &a, &b) == 2) {
                    images = texture_cache.DebugImagesWhere(1, ~0u, 1, ~0u, a, b);
                } else if (std::sscanf(line, "%llx %llx", &a, &b) == 2) {
                    requests.emplace_back(a, b);
                }
                for (const auto& [image_address, image_size] : images) {
                    if (requests.size() < MaxRequests) requests.emplace_back(image_address, image_size);
                }
            }
            for (const auto& [address, size] : requests) {
                const auto id = texture_cache.FindImageFromRange(address, size, false);
                if (!id) continue;
                auto& image = texture_cache.GetImage(id);
                const auto format = image.info.pixel_format;
                const bool depth16 = format == vk::Format::eD16Unorm;
                const bool depth = format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD32Sfloat ||
                                   depth16;
                const bool bc1 = format == vk::Format::eBc1RgbaUnormBlock ||
                                 format == vk::Format::eBc1RgbaSrgbBlock;
                const bool bc5 = format == vk::Format::eBc5UnormBlock ||
                                 format == vk::Format::eBc5SnormBlock;
                const bool bc16 = bc5 || format == vk::Format::eBc3UnormBlock ||
                                  format == vk::Format::eBc3SrgbBlock ||
                                  format == vk::Format::eBc2UnormBlock ||
                                  format == vk::Format::eBc2SrgbBlock ||
                                  format == vk::Format::eBc7UnormBlock ||
                                  format == vk::Format::eBc7SrgbBlock;
                bool compressed = bc1 || bc16;
                u32 bytes = format == vk::Format::eR8Unorm ? 1
                            : format == vk::Format::eR8G8Unorm || depth16 ? 2
                            : bc16 ? 16 : (bc1 || format == vk::Format::eR16G16B16A16Sfloat ? 8 : 4);
                if (image.info.num_samples != 1) continue;
                if (!depth && !compressed && image.info.num_bits != bytes * 8) {
                    // Any other format: its texel (or 4x4 block) size.
                    compressed = image.info.props.is_block;
                    bytes = image.info.num_bits / 8;
                    if (bytes == 0) continue;
                }
                runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
                runtime.FlushBarriers();
                const auto name = std::format("guest_{:x}_{}", address, vk::to_string(format));
                DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                        image.info.size.width, image.info.size.height, name.c_str(), frame_dump_index,
                        bytes, depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                        compressed ? 4 : 1, compressed ? 4 : 1);
                // Array images (shadow cascades): every further layer as <name>_L<n>.
                for (u32 layer = 1; layer < std::min<u32>(image.info.resources.layers, 8); ++layer) {
                    const auto layer_name = std::format("{}_L{}", name, layer);
                    DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                            vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                            image.info.size.height, layer_name.c_str(), frame_dump_index, bytes,
                            depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                            compressed ? 4 : 1, compressed ? 4 : 1, layer);
                }
                // Keep enough layout/state evidence to compare the cached image with a
                // separate read-only guest-memory snapshot. Do not refresh the image here:
                // that would hide the stale-cache condition this capture is investigating.
                const auto metadata_path = std::filesystem::path(dir ? dir : "out/dump") /
                    std::format("{}_{:03}.txt", name, frame_dump_index);
                if (FILE* metadata = std::fopen(metadata_path.string().c_str(), "w")) {
                    std::fprintf(metadata,
                        "address=%llx guest_size=%x flags=%x pitch=%u tile_mode=%u "
                        "array_mode=%u bank_swizzle=%u width=%u height=%u bits=%u\n",
                        static_cast<unsigned long long>(image.info.guest_address), image.info.guest_size,
                        static_cast<unsigned>(image.flags), image.info.pitch,
                        static_cast<unsigned>(image.info.tile_mode), static_cast<unsigned>(image.info.array_mode),
                        image.info.bank_swizzle, image.info.size.width, image.info.size.height, image.info.num_bits);
                    for (u32 mip = 0; mip < image.info.resources.levels; ++mip) {
                        const auto& layout = image.info.mips_layout[mip];
                        std::fprintf(metadata, "mip=%u offset=%x size=%x pitch=%u height=%u\n",
                                     mip, layout.offset, layout.size, layout.pitch, layout.height);
                    }
                    std::fclose(metadata);
                }
                if (format == vk::Format::eD32SfloatS8Uint) {
                    const auto stencil_name = name + "_stencil";
                    DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                            image.info.size.width, image.info.size.height, stencil_name.c_str(), frame_dump_index,
                            1, vk::ImageAspectFlagBits::eStencil);
                }
            }
            std::fclose(f);
            std::filesystem::remove(request);
        }
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    // gow3: the HDR10 chain's display buffer is RGBA16F holding the game's stored values
    // (TemporalUpscaler::OnComposite): read as they are, like a 10-bit sRGB buffer.
    const bool stored_display = upscaled && display.format == vk::Format::eR16G16B16A16Sfloat;
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = stored_display ? display.format : view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        if (frame_dump) {
            // gow3: the upscaled display buffer the presenter reads (HDR10 checks compare it
            // with the "output" dump).
            const vk::MemoryBarrier2 written{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
            DumpRaw(instance, draw_scheduler, cmdbuf, display.image, vk::ImageLayout::eGeneral,
                    display.width, display.height, "display", frame_dump_index,
                    stored_display ? 8 : 4);
        }
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // gow3: FSR 1, RCAS and its strength come from the in-game menu, every frame.
    const auto& graphics = Gow3Settings::Get();
    fsr_settings.enable = graphics.fsr1;
    fsr_settings.use_rcas = graphics.rcas;
    fsr_settings.rcas_attenuation = Gow3Graphics::RcasAttenuation(graphics.rcas_strength);
    Gow3Settings::Get().image_width = int(image_size.width);
    Gow3Settings::Get().image_height = int(image_size.height);
    Gow3Settings::Get().window_width = int(frame->width);
    Gow3Settings::Get().window_height = int(frame->height);
    Gow3Settings::Get().rcas_applied =
        fsr_settings.use_rcas &&
        Gow3Graphics::FsrPassRuns(fsr_settings.enable, true, int(image_size.width), int(image_size.height),
                                  int(frame->width), int(frame->height));
    // gow3: FSR's HDR sampling squares its output (for a guest PQ image); the HDR10 encode reads
    // the game's ordinary SDR image, so FSR 1 / RCAS stay in SDR mode for it.
    const bool encode_hdr = pp_settings.hdr == HostPasses::PostProcessingPass::HdrEncode;
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr && !encode_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb ||
        stored_display;
    // The float composition is expressed in UI-white units: the scene was rebased before
    // HUD/menu blending, so its physical brightness stays independent of UI brightness.
    pp_settings.game_nits = stored_display && display.white_nits > 0.0f
                                ? display.white_nits : float(graphics.hdr_game_nits.load());
    pp_settings.peak_nits = float(graphics.hdr_peak_nits.load());
    pp_settings.ui_nits = float(graphics.hdr_ui_nits.load());
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        // gow3: what the raw files hold, for the offline HDR10 checks.
        const char* dir = std::getenv("GOW3_DUMP_DIR");
        const auto info_path = std::filesystem::path(dir && *dir ? dir : "out/dump") /
                               std::format("output_{:03}.txt", frame_dump_index);
        if (FILE* f = std::fopen(info_path.string().c_str(), "w")) {
            std::fprintf(f,
                         "output_format=%s\nguest_format=%u\nview_format=%s\nhdr_mode=%u\n"
                         "srgb_input=%u\ngame_nits=%.1f\npeak_nits=%.1f\nui_nits=%.1f\n"
                         "rcas=%d\nfsr1=%d\ninput=%ux%u\noutput=%ux%u\nstored_display=%d\n",
                         vk::to_string(swapchain.GetImageFormat()).c_str(),
                         u32(attribute.attrib.pixel_format), vk::to_string(view_info.format).c_str(),
                         pp_settings.hdr, pp_settings.srgb_input, pp_settings.game_nits,
                         pp_settings.peak_nits, pp_settings.ui_nits, int(fsr_settings.use_rcas),
                         int(fsr_settings.enable), image_size.width, image_size.height,
                         frame->width, frame->height, int(stored_display));
            std::fclose(f);
        }
        ++frame_dump_index;
    }
    CaptureLiveView(instance, draw_scheduler, cmdbuf, frame->image, frame->width, frame->height,
                    swapchain.GetSurfaceFormat().format);

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    WatchGpuDone(draw_scheduler.GetWorkSemaphore(), frame->ready_tick);
    SubmitLog::Event("F2", "%u %llu", frame->id, static_cast<unsigned long long>(frame->ready_tick));
    // gow3 22 Oct 2026: the frame's last submission is out, before the wait below: that one is the
    // GPU's (up to a frame when it is the bottleneck), and counted as render submit time it would
    // tell the driver the CPU needs that long, so Reflex would release the game that much earlier.
    Gow3Reflex::RenderSubmitEnd(frame->serial);
    Gow3Reflex::FrameQueued(frame->serial, instance.GetDevice(), frame->ready_semaphore,
                            frame->ready_tick);

    // gow3: the GPU command thread runs at most GOW3_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    // The in-game menu changes it live (Gow3Settings::frames_queued; GOW3_FRAMES_AHEAD at start).
    const u32 frames_ahead = u32(std::max(0, Gow3Settings::Get().frames_queued.load()));
    if (!frames_ahead) {
        recent_frame_ticks.clear();
    } else {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                // gow3 18 Oct 2026: while the command thread waits here for the GPU (up to a
                // frame), readbacks whose GPU work is done land meanwhile, and with them the
                // fences the guest waits for (ray-cast results). A plain wait held them until the
                // whole frame was done: Uncharted 3 on the rooftops made two or three 5-7 ms
                // frames, then one of 25-35 ms waiting for its ray casts (75 FPS, 1% lows 22 FPS
                // at the 120 limit). GOW3_POLL_WHILE_WAITING=0 waits plainly (A/B).
                static const bool poll_while_waiting = [] {
                    const char* env = std::getenv("GOW3_POLL_WHILE_WAITING");
                    return !(env && env[0] == '0');
                }();
                // gow3 20 Oct 2026 (idle CPU): the 0.25 ms polls woke the command thread up to
                // ~4,000 times a second while the GPU was the bottleneck. Readbacks land in tick
                // order once the GPU has passed their tick, so after a poll the oldest pending
                // one can land no sooner than the GPU's next finished submission: the thread
                // now sleeps until that submission (or the frame's tick) is done, lands what is
                // finished and repeats; with no readback pending it waits plainly (below). Each
                // readback still lands as soon as its submission is done (sooner than the next
                // 0.25 ms poll). Readbacks left after a poll although done (the test knobs
                // GOW3_TEST_READBACK_HOLD/LAG hold them) and a GPU that finishes nothing for
                // 1 s go back to the 0.25 ms polls. GOW3_POLL_ON_COMPLETION=0: the 0.25 ms polls.
                static const bool poll_on_completion = [] {
                    const char* env = std::getenv("GOW3_POLL_ON_COMPLETION");
                    const bool on = !(env && env[0] == '0') &&
                                    !std::getenv("GOW3_TEST_READBACK_HOLD") &&
                                    !std::getenv("GOW3_TEST_READBACK_LAG");
                    std::printf("Presenter: frames-ahead wait lands readbacks %s\n",
                                on ? "as each GPU submission finishes (GOW3_POLL_ON_COMPLETION=0: "
                                     "every 0.25 ms)"
                                   : "every 0.25 ms");
                    return on;
                }();
                // gow3 21 Oct 2026 (GOW3_WAIT_SERVICES_FAULTS=1, the CPU worker): guest fault
                // requests queued while this wait runs are served here (Uncharted 2: read faults
                // waited ~10 ms each for the command thread, 20-38% of a guest thread): the wait
                // goes in slices of at most 1 ms, re-checking the frame's tick after each slice
                // and each served request (one may submit work), and lands readbacks as each
                // submission finishes, as below. Only on the GPU command thread with the draw
                // recording thread idle (Liverpool::ServiceQueuedCommands checks).
                if (poll_while_waiting && rasterizer && liverpool &&
                    AmdGpu::Liverpool::WaitServicesFaults()) {
                    auto* semaphore = draw_scheduler.GetWorkSemaphore();
                    auto& buffer_cache = rasterizer->GetBufferCache();
                    Gow3Stats::WaitTimer timer{Gow3Stats::tick_wait_ns};
                    buffer_cache.PollReadbacksNow();
                    while (!semaphore->IsFree(tick)) {
                        liverpool->ServiceQueuedCommands();
                        if (semaphore->IsFree(tick)) {
                            break;
                        }
                        const u64 newest = buffer_cache.NewestPendingReadbackTick();
                        const u64 known = semaphore->KnownGpuTick();
                        const u64 target = newest > known ? std::min(tick, known + 1) : tick;
                        if (semaphore->WaitFor(target, 1'000'000)) {
                            buffer_cache.PollReadbacksNow();
                        }
                    }
                    liverpool->ServiceQueuedCommands();
                    buffer_cache.PollReadbacksNow();
                } else if (poll_while_waiting && rasterizer) {
                    auto* semaphore = draw_scheduler.GetWorkSemaphore();
                    auto& buffer_cache = rasterizer->GetBufferCache();
                    Gow3Stats::WaitTimer timer{Gow3Stats::tick_wait_ns};
                    if (poll_on_completion) {
                        bool on_completion = true;
                        buffer_cache.PollReadbacksNow();
                        while (!semaphore->IsFree(tick)) {
                            if (on_completion) {
                                // The newest pending readback's tick (0: none pending, also
                                // counting readbacks without data pieces, e.g. fence-only); the
                                // poll refreshed the GPU tick when any was pending.
                                const u64 newest = buffer_cache.NewestPendingReadbackTick();
                                if (newest == 0) {
                                    break; // nothing to land: the plain wait below
                                }
                                const u64 known = semaphore->KnownGpuTick();
                                if (newest > known) {
                                    if (semaphore->WaitFor(std::min(tick, known + 1),
                                                           1'000'000'000)) {
                                        buffer_cache.PollReadbacksNow();
                                        continue;
                                    }
                                }
                                on_completion = false; // the 0.25 ms polls, as before
                            }
                            if (semaphore->WaitFor(tick, 250'000)) {
                                break;
                            }
                            buffer_cache.PollReadbacksNow();
                        }
                    } else {
                        while (!semaphore->IsFree(tick)) {
                            buffer_cache.PollReadbacksNow();
                            if (semaphore->WaitFor(tick, 250'000)) {
                                break;
                            }
                        }
                    }
                    buffer_cache.PollReadbacksNow();
                }
                draw_scheduler.Wait(tick);
                SubmitLog::Event("F3", "%u %llu", frame->id, static_cast<unsigned long long>(tick));
            }
        }
    }
    return frame;
}

// gow3 diagnostics (GOW3_DISPLAY_LOG=<file>): when each game frame's present submission finished
// on the GPU (from then on it can be shown), "<interval ms> <steady clock ms>" per line: display
// pacing without PresentMon. A thread waits on the frames' present fences in order (a frame's fence is reset only
// when it is reused, a whole frame pool later).
static void WatchDisplay(vk::Device device, vk::Fence fence) {
    static FILE* file = [] {
        const char* path = std::getenv("GOW3_DISPLAY_LOG");
        return path && path[0] ? std::fopen(path, "w") : nullptr;
    }();
    if (!file) {
        return;
    }
    static std::mutex mutex;
    static std::condition_variable cv;
    static std::deque<vk::Fence> fences;
    static std::once_flag started;
    std::call_once(started, [device] {
        std::thread([device] {
            auto last = std::chrono::steady_clock::now();
            u32 lines = 0;
            for (;;) {
                vk::Fence next;
                {
                    std::unique_lock lock{mutex};
                    cv.wait(lock, [] { return !fences.empty(); });
                    next = fences.front();
                    fences.pop_front();
                }
                if (device.waitForFences(next, true, 1'000'000'000) != vk::Result::eSuccess) {
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                std::fprintf(file, "%.3f %.3f\n",
                             std::chrono::duration<double, std::milli>(now - last).count(),
                             std::chrono::duration<double, std::milli>(now.time_since_epoch()).count());
                last = now;
                if (++lines % 64 == 0) {
                    std::fflush(file);
                }
            }
        }).detach();
    });
    {
        std::scoped_lock lock{mutex};
        fences.push_back(fence);
    }
    cv.notify_one();
}

// gow3 diagnostics (GOW3_DISPLAY_LOG=<file>): <file>.gpu gets the time the GPU finished each
// flipped frame (its ready tick), in the same format.
static void WatchGpuDone(Semaphore* semaphore, u64 tick) {
    static FILE* file = [] {
        const char* path = std::getenv("GOW3_DISPLAY_LOG");
        return path && path[0] ? std::fopen((std::string{path} + ".gpu").c_str(), "w") : nullptr;
    }();
    if (!file) {
        return;
    }
    static std::mutex mutex;
    static std::condition_variable cv;
    static std::deque<u64> ticks;
    static std::once_flag started;
    std::call_once(started, [semaphore] {
        std::thread([semaphore] {
            auto last = std::chrono::steady_clock::now();
            u32 lines = 0;
            for (;;) {
                u64 next;
                {
                    std::unique_lock lock{mutex};
                    cv.wait(lock, [] { return !ticks.empty(); });
                    next = ticks.front();
                    ticks.pop_front();
                }
                while (!semaphore->WaitFor(next, 1'000'000'000)) {
                }
                const auto now = std::chrono::steady_clock::now();
                std::fprintf(file, "%.3f %.3f\n",
                             std::chrono::duration<double, std::milli>(now - last).count(),
                             std::chrono::duration<double, std::milli>(now.time_since_epoch()).count());
                last = now;
                if (++lines % 64 == 0) {
                    std::fflush(file);
                }
            }
        }).detach();
    });
    {
        std::scoped_lock lock{mutex};
        ticks.push_back(tick);
    }
    cv.notify_one();
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    // gow3 22 Oct 2026: a blank frame the guest flipped (buffer -1, not the present thread's black
    // frame) is a flip like any other for Reflex: the guest counted it (GuestFlipRequest).
    frame->serial = present_thread ? 0 : Gow3Reflex::PreparedFlip();
    frame->frame_gen = false;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::PresentLoadingFrame() {
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return;
    }
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (!swapchain.AcquireNextImage()) {
        return;
    }
    // gow3 25 Oct 2026: not a flip, so the flight recorder would see the load as one long
    // interval (an auto-stall dump); the event tells it a loading frame was shown.
    Gow3Flight::Event(Gow3Flight::LoadingFrame);
    const vk::Image image = swapchain.Image();
    const vk::Extent2D extent = swapchain.GetExtent();
    const auto cmdbuf = present_scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = range,
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                           range);
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eColorAttachmentWrite);
    RenderOverlay(cmdbuf, extent);
    barrier(vk::ImageLayout::eColorAttachmentOptimal, swapchain.FinalLayout(),
            vk::AccessFlagBits::eColorAttachmentWrite, {});
    SubmitInfo info{};
    if (!swapchain.IsHeadless()) {
        info.AddWait(swapchain.GetImageAcquiredSemaphore());
        info.AddSignal(swapchain.GetPresentReadySemaphore());
    }
    present_scheduler.Flush(info);
    if (const Dlss* dlss = Dlss::Get(); dlss && dlss->ViaStreamline()) {
        Gow3Streamline::FrameGenSetMode(false, 1); // a loading frame has nothing to interpolate
        frame_gen_active.store(false, std::memory_order_relaxed);
    }
    if (swapchain.IsHeadless()) present_scheduler.Finish();
    {
        std::scoped_lock present_lock{PresentLock(instance)};
        swapchain.Present();
    }
    // A few frames a second during start-up: waiting keeps semaphores and the swapchain simple.
    present_scheduler.Finish();
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // gow3: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

    // Recreate the swapchain if the window was resized.
    SubmitLog::Event("A0", "%u %llu %d", frame->id, static_cast<unsigned long long>(frame->ready_tick),
                     int(is_game_frame));
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    // gow3 22 Oct 2026: DLSS Frame Generation presents its frame first. Any frame without one
    // (frame generation off or skipped, black, loading or repeated frames) makes the next
    // evaluation a reset: the two frames it would interpolate between are not neighbours.
    // gow3 23 Oct 2026: through Streamline its swapchain hooks generate and present the frame
    // inside the present below; this only hands it the frame's inputs and the mode.
    if (const Dlss* dlss = Dlss::Get(); dlss && dlss->ViaStreamline()) {
        UpdateStreamlineFrameGenLoad();
        frame_gen_active.store(
            StreamlineFrameGen(frame, is_game_frame && !is_reusing_frame), std::memory_order_relaxed);
    } else {
        if (!is_game_frame || is_reusing_frame || !PresentGenerated(frame)) {
            frame_gen_reset = true;
            frame_gen_active.store(false, std::memory_order_relaxed);
        }
        // gow3 25 Oct 2026: flight recorder `presents` (PresentGenerated shows one generated
        // frame before the real one).
        Gow3Flight::Gauge(Gow3Flight::Presents,
                          frame_gen_active.load(std::memory_order_relaxed) ? 2.0f : 1.0f);
    }

    const bool acquired = swapchain.AcquireNextImage();
    SubmitLog::Event("A1", "%u", frame->id);
    if (!acquired) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            // 22 Oct review: PresentGenerated may have submitted an evaluation reading
            // frame->image and the ring slot, and present_done is still the old signaled fence,
            // so GetRenderFrame would not wait for it. Rare path: wait for the present queue.
            present_scheduler.Finish();
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // gow3: the game frame is blitted (letterboxed) straight into the swapchain image.
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        BlitToSwapchain(cmdbuf, frame->image, frame->width, frame->height);
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    if (!swapchain.IsHeadless()) info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    if (!swapchain.IsHeadless()) info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);
    // No WSI acquire/present semaphores exist offscreen. Finish the real GPU
    // submission before image reuse; the frame-ready timeline and present-done
    // fence above remain intact. This mode prioritizes diagnostics over speed.
    if (swapchain.IsHeadless()) scheduler.Finish();
    SubmitLog::Event("A2", "%u", frame->id);
    if (is_game_frame && !is_reusing_frame && !instance.HasPresentWait()) {
        WatchDisplay(instance.GetDevice(), frame->present_done);
    }

    // Present to swapchain.
    // gow3 22 Oct 2026: Reflex's present markers, for game frames only (not the redraw of the last
    // frame, nor the black or loading frames), outside the present lock: markers are driver calls.
    // With the present queue shared with the graphics queue the start marker comes before the wait
    // for the submit lock, so it also counts that wait.
    const bool reflex_frame = is_game_frame && !is_reusing_frame && frame->serial != 0;
    if (reflex_frame) {
        Gow3Reflex::PresentStart(frame->serial);
    }
    bool presented;
    {
        std::scoped_lock present_lock{PresentLock(instance)};
        presented = swapchain.Present();
    }
    // Recreate waits for device idle under the submit lock. Release the present lock first
    // because the shared-queue path uses that same mutex.
    if (!presented && window.GetWidth() != 0 && window.GetHeight() != 0) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (reflex_frame) {
        Gow3Reflex::PresentEnd(frame->serial);
    }
    SubmitLog::Event("A3", "%u", frame->id);

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

void Presenter::BlitToSwapchain(vk::CommandBuffer cmdbuf, vk::Image source, u32 width,
                                u32 height) {
    const vk::Image swapchain_image = swapchain.Image();
    {
        const vk::Extent2D extent = swapchain.GetExtent();
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ImageBlit blit = MakeImageBlitFit(width, height, extent.width, extent.height);
        // 20 Oct audit: the 4K clear cost GPU time every present; only letterbox bars need it.
        const bool covers = blit.dstOffsets[0].x == 0 && blit.dstOffsets[0].y == 0 &&
                            blit.dstOffsets[1].x == s32(extent.width) &&
                            blit.dstOffsets[1].y == s32(extent.height);
        if (!covers) {
            const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
            cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black,
                                   color_range);
            const vk::MemoryBarrier clear_done{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        }
        cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        // gow3: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = Gow3Overlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : swapchain.FinalLayout(),
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            RenderOverlay(cmdbuf, extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = swapchain.FinalLayout(),
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
}

namespace {

// gow3 22 Oct 2026: DLSS Frame Generation pacing (swap thread only): the real frame half the last
// real interval after the generated present returns. A deadline pacer (absolute deadlines half a
// filtered period apart) was measured and removed the same day (U3 chapter 2, 1080p DLAA, limit
// 160): it took the real rate from 80 to 52-65 FPS and put 37-48% of displayed intervals above
// 1.5x the median (3% with this sleep), because its period came from arrival intervals it shaped
// itself and could only ratchet up (period_ms 19.3, wait_ms 17.7 of 19).
using FgClock = std::chrono::steady_clock;

double FgMs(FgClock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

/// Sleeps until `deadline` on common/thread.cpp's high-resolution waitable timer (libc++'s
/// sleep_until rounds up to whole milliseconds on Windows: up to ~1 ms late, driver.cpp's note of
/// 20 Oct). Returns the milliseconds slept.
double FgSleepUntil(FgClock::time_point deadline) {
    const auto now = FgClock::now();
    if (deadline <= now) {
        return 0.0;
    }
    Common::AccurateSleep(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now),
                          nullptr, false);
    return FgMs(FgClock::now() - now);
}

/// 5 s of samples: the mean of all, the p99 of the last 512.
struct FgSamples {
    std::array<float, 512> ring{};
    u32 count{};
    double sum{};
    void Add(double ms) {
        ring[count % ring.size()] = float(ms);
        ++count;
        sum += ms;
    }
    double Mean() const {
        return count ? sum / count : 0.0;
    }
    double P99() {
        const u32 n = std::min<u32>(count, u32(ring.size()));
        if (n == 0) {
            return 0.0;
        }
        const auto it = ring.begin() + (n - 1) * 99 / 100;
        std::nth_element(ring.begin(), it, ring.begin() + n);
        return *it;
    }
};

/// Statistics, printed every 5 s: eval = evaluate+flush, present = acquire..present return,
/// gap = the slept time before the real present, interval = the raw real interval it halves.
struct FgStats {
    FgClock::time_point stats_at{};
    u32 real{}, gen{}, menu{}, noinput{}, hdr{}, acquire{}, cuts{};
    FgSamples eval, present, gap, interval;

    void Print(bool std_sleep) {
        std::printf("FrameGen: real=%u gen=%u skipped(menu=%u noinput=%u hdr=%u acquire=%u) "
                    "eval_ms=%.2f/%.2f present_ms=%.2f gap_ms=%.2f/%.2f real_interval_ms=%.2f "
                    "std_sleep=%d cuts=%u\n",
                    real, gen, menu, noinput, hdr, acquire, eval.Mean(), eval.P99(),
                    present.Mean(), gap.Mean(), gap.P99(), interval.Mean(), std_sleep ? 1 : 0,
                    cuts);
        std::fflush(stdout);
        *this = FgStats{.stats_at = stats_at};
    }
};
FgStats fg_stats;

} // namespace

bool Presenter::PresentGenerated(Frame* frame) {
    auto& settings = Gow3Settings::Get();
    if (settings.frame_gen_supported) {
        // 22 Oct 2026: the inputs are the upscaler's depth and motion vectors; with anti-aliasing
        // off there are none.
        settings.frame_gen_problem =
            settings.frame_gen && settings.upscaler == Gow3Settings::UpscalerOff
                ? "needs anti-aliasing (TAA, FSR or DLSS) for its depth and motion vectors"
                : nullptr;
    }
    // Not for frames without an upscale (menus, movies, loading), with the in-game menu open (it
    // is drawn over both frames and would smear), or right after an HDR switch (the frame image
    // and the feature still have the other format).
    if (!Dlss::FrameGenWanted()) {
        return false; // frame generation off: no pacing, no statistics
    }
    auto& stats = fg_stats;
    const bool std_sleep = Gow3Toggle::Disabled2(Gow3Toggle::FrameGenStdSleep);
    {
        const auto now = FgClock::now();
        if (stats.stats_at == FgClock::time_point{}) {
            stats.stats_at = now;
        } else if (now - stats.stats_at >= std::chrono::seconds(5)) {
            stats.stats_at = now;
            stats.Print(std_sleep);
        }
    }
    ++stats.real;
    if (!frame->frame_gen) {
        ++stats.noinput;
        return false;
    }
    if (Gow3Overlay::CapturesInput()) {
        ++stats.menu;
        return false;
    }
    if (frame->is_hdr != swapchain.GetHDR()) {
        ++stats.hdr;
        return false;
    }
    Dlss* dlss = Dlss::Get();
    const auto& inputs = frame->frame_gen_inputs;
    // gow3 26 Oct 2026: the frame images' format (10-bit while HDR10 output is on, RecreateFrame).
    const vk::Format format = swapchain.GetImageFormat();
    const Dlss::FrameGenDesc desc{frame->width, frame->height, inputs.depth.width,
                                  inputs.depth.height, format};
    if (frame_gen_failed == desc) {
        return false;
    }
    static std::array<bool, 2> storable_said{};
    const bool storable = bool(instance.GetPhysicalDevice().getFormatProperties(format)
                                   .optimalTilingFeatures &
                               vk::FormatFeatureFlagBits::eStorageImage);
    if (!storable) {
        bool& said = storable_said[swapchain.GetHDR()];
        if (!said) {
            said = true;
            std::printf("Frame generation: off (%s cannot be a storage image)\n",
                        vk::to_string(format).c_str());
        }
        return false;
    }
    auto& scheduler = present_scheduler;
    const vk::Device device = instance.GetDevice();
    if (!frame_gen_output || frame_gen_width != frame->width ||
        frame_gen_height != frame->height || frame_gen_format != format ||
        !dlss->HasFrameGenFeature(desc)) {
        // The previous output and feature may still be in use by submitted presents (a window
        // resize, an HDR switch or a new render size: rare).
        scheduler.Finish();
        if (!frame_gen_output || frame_gen_width != frame->width ||
            frame_gen_height != frame->height || frame_gen_format != format) {
            frame_gen_output_view.reset();
            frame_gen_output = VideoCore::UniqueImage(device, instance.GetAllocator());
            frame_gen_output.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {frame->width, frame->height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferSrc,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            frame_gen_output_view = Check(device.createImageViewUnique({
                .image = vk::Image(frame_gen_output),
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
            frame_gen_width = frame->width;
            frame_gen_height = frame->height;
            frame_gen_format = format;
        }
        if (!dlss->HasFrameGenFeature(desc)) {
            if (!dlss->FrameGenCreate(scheduler.CommandBuffer(), desc)) {
                std::printf("Frame generation: feature creation failed (%ux%u, depth and motion "
                            "%ux%u); off for this size\n",
                            desc.width, desc.height, desc.render_width, desc.render_height);
                frame_gen_failed = desc;
                return false;
            }
            std::printf("Frame generation: feature created (%ux%u, depth and motion %ux%u, %s)\n",
                        desc.width, desc.height, desc.render_width, desc.render_height,
                        vk::to_string(format).c_str());
            frame_gen_reset = true;
        }
    }

    // The real frames' interval, for the pacing below.
    const auto start = std::chrono::steady_clock::now();
    const auto real_interval = start - frame_gen_last_real;
    frame_gen_last_real = start;

    // 22 Oct 2026: a camera cut resets NGX (U3 cutscenes with the development nvngx_dlssg.dll
    // warned "Scene change detected without Reset flag"). Not counted when a reset is pending anyway.
    const bool camera_cut = Vulkan::IsCameraCut(frame_gen_prev_camera, inputs.camera);
    frame_gen_prev_camera = inputs.camera;
    if (camera_cut && !frame_gen_reset) {
        ++stats.cuts;
    }

    const vk::Image output = vk::Image(frame_gen_output);
    const vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const auto cmdbuf = scheduler.CommandBuffer();
    // The last blit read the output: its contents are not needed.
    const vk::ImageMemoryBarrier2 to_write{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output,
        .subresourceRange = color,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
    // The backbuffer is the frame image as Present shows it (game HUD included; the overlay is
    // drawn after, on both frames). No hudless image: the upscaled scene the port has is before
    // the game's post, tonemap or the presenter's sharpening, and NGX needs it equal to the
    // backbuffer outside the HUD (DLSS-FG guide, pHudless).
    const bool ok = dlss->FrameGenEvaluate(
        cmdbuf, {
                    .backbuffer = {frame->image, frame->image_view, format,
                                   vk::ImageAspectFlagBits::eColor, frame->width, frame->height},
                    .depth = inputs.depth,
                    .motion = inputs.motion,
                    .output = {output, *frame_gen_output_view, format,
                               vk::ImageAspectFlagBits::eColor, frame->width, frame->height},
                    .camera = inputs.camera,
                    .jitter_x = inputs.jitter[0],
                    .jitter_y = inputs.jitter[1],
                    .reset = frame_gen_reset || inputs.reset || camera_cut,
                    .hdr = frame->is_hdr,
                });
    // NGX may leave Vulkan state changed (DLSS guide 5.2.5).
    scheduler.GetDynamicState().Invalidate();
    if (!ok) {
        static bool printed = false;
        if (!printed) {
            printed = true;
            std::printf("Frame generation: evaluation failed; real frames only\n");
        }
        return false; // the barrier above goes out with the real frame's present
    }
    frame_gen_reset = false;
    const vk::MemoryBarrier2 written{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                     .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                     .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                     .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
    // A submission of its own: its first wait (the frame's rendering) covers all stages, NGX's
    // compute included (Scheduler::Flush: the second wait covers color output only).
    {
        SubmitInfo info{};
        info.AddWait(frame->ready_semaphore, frame->ready_tick);
        scheduler.Flush(info);
    }
    stats.eval.Add(FgMs(FgClock::now() - start));
    const auto present_start = FgClock::now();
    if (!swapchain.AcquireNextImage()) {
        ++stats.acquire;
        // 22 Oct review: eSuboptimalKHR still acquires an image and signals the semaphore, but
        // frame_index does not advance; Present would acquire again on the same semaphore
        // (VUID 01286) and leak the image. Recreate here as Present does. The output and the
        // feature follow the frame and format checks above on the next call; Present resets.
        if (window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
        return false;
    }
    BlitToSwapchain(scheduler.CommandBuffer(), output, frame->width, frame->height);
    {
        SubmitInfo info{};
        info.AddWait(swapchain.GetImageAcquiredSemaphore());
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        scheduler.Flush(info);
    }
    // 22 Oct 2026: out-of-band present markers with the real frame's id (VK_NV_low_latency2
    // revision 2: a present without markers counts as a new application frame).
    const bool reflex_marked = frame->serial != 0;
    if (reflex_marked) {
        Gow3Reflex::GeneratedPresentStart(frame->serial);
    }
    bool present_failed = false;
    {
        std::scoped_lock present_lock{PresentLock(instance)};
        present_failed = !swapchain.Present();
    }
    // Recreate waits for device idle under the submit lock (not under the present lock, which
    // the shared-queue path uses too).
    if (present_failed && window.GetWidth() != 0 && window.GetHeight() != 0) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (reflex_marked) {
        Gow3Reflex::GeneratedPresentEnd(frame->serial);
    }
    if (present_failed) {
        return false;
    }
    frame_gen_active.store(true, std::memory_order_relaxed);
    ++stats.gen;
    stats.present.Add(FgMs(FgClock::now() - present_start));
    // The real frame half a real interval after the generated one (DLSS-FG guide 7: equal
    // spacing). The first frame after a pause (an interval over 100 ms) does not wait.
    if (real_interval < std::chrono::milliseconds(100)) {
        stats.interval.Add(FgMs(real_interval));
        const auto slept_from = FgClock::now();
        if (std_sleep) {
            std::this_thread::sleep_until(slept_from + real_interval / 2);
            stats.gap.Add(FgMs(FgClock::now() - slept_from));
        } else {
            stats.gap.Add(FgSleepUntil(slept_from + real_interval / 2));
        }
    }
    return true;
}

void Presenter::UpdateStreamlineFrameGenLoad() {
    if (!Gow3Streamline::FrameGenReady()) {
        return;
    }
    const bool wanted = Dlss::FrameGenWanted();
    if (wanted == Gow3Streamline::FrameGenLoaded()) {
        return;
    }
    // Streamline's DLSS-G guide 18: loaded, the plugin presents through its own queue from
    // off-screen copies, which costs a copy and a queue hand-over per frame even with the mode
    // off; it acts at swapchain creation, so the swapchain is made again around the change.
    Gow3Streamline::FrameGenSetMode(false, 1);
    present_scheduler.Finish();
    swapchain.RecreateAround([wanted] { Gow3Streamline::SetFrameGenLoaded(wanted); });
    frame_gen_reset = true;
    std::printf("Frame generation (Streamline): %s; swapchain recreated\n",
                wanted ? "loaded" : "unloaded");
}

bool Presenter::StreamlineFrameGen(Frame* frame, bool game_frame) {
    static auto stats_at = std::chrono::steady_clock::now();
    static auto undoubled_since = std::chrono::steady_clock::time_point{};
    static bool undoubled_reported = false;
    static u32 real = 0, generated = 0, doubled = 0;
    if (!Dlss::FrameGenWanted() || !Gow3Streamline::FrameGenLoaded()) {
        frame_gen_reset = true;
        Gow3Flight::Gauge(Gow3Flight::Presents, 1.0f);
        return false; // unloaded (UpdateStreamlineFrameGenLoad): nothing to hand over
    }
    // 23 Oct 2026: Streamline decides per present whether it really interpolates. Its log says
    // "DLSS-G disabled: window not focused" while another window has the focus (2 of 3 automated
    // U3 sessions: 60 real and 60 shown FPS, the limiter halving real frames for nothing). The
    // frames it presented for the last real one (2 when it added one, 1 when not) decide whether
    // the limiter counts shown frames (Presenter::FrameGenActive), one frame late.
    const auto fg_state = Gow3Streamline::FrameGenGetState();
    const bool last_doubled = fg_state.presented >= 2;
    // gow3 25 Oct 2026: flight recorder `presents` (0 when Streamline gave no state).
    Gow3Flight::Gauge(Gow3Flight::Presents, float(fg_state.presented));
    const auto off = [&] {
        frame_gen_reset = true;
        if (game_frame && frame->serial != 0) {
            Gow3Streamline::FrameGenSetFrame(frame->serial, nullptr);
        }
        Gow3Streamline::FrameGenSetMode(false, 1);
        return false;
    };
    if (const auto now = std::chrono::steady_clock::now();
        now - stats_at >= std::chrono::seconds(5)) {
        // with_inputs: frames handed over with the mode on; doubled: frames Streamline presented
        // with a generated one.
        std::printf("FrameGen (Streamline): real=%u with_inputs=%u doubled=%u status=0x%x\n", real,
                    generated, doubled, fg_state.status);
        std::fflush(stdout);
        stats_at = now;
        real = generated = doubled = 0;
    }
    ++real;
    doubled += last_doubled ? 1 : 0;
    // As PresentGenerated: game frames with an upscale's inputs, not under the in-game menu and
    // not right after an HDR switch.
    if (!game_frame || frame->serial == 0 || !frame->frame_gen || Gow3Overlay::CapturesInput() ||
        frame->is_hdr != swapchain.GetHDR()) {
        return off();
    }
    const auto& inputs = frame->frame_gen_inputs;
    const bool camera_cut = Vulkan::IsCameraCut(frame_gen_prev_camera, inputs.camera);
    frame_gen_prev_camera = inputs.camera;
    // Where the letterboxed game image lands in the backbuffer (BlitToSwapchain).
    const vk::Extent2D extent = swapchain.GetExtent();
    const vk::ImageBlit blit = MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height);
    const bool covers = blit.dstOffsets[0].x == 0 && blit.dstOffsets[0].y == 0 &&
                        blit.dstOffsets[1].x == s32(extent.width) &&
                        blit.dstOffsets[1].y == s32(extent.height);
    const auto image = [](const Dlss::Resource& r) {
        return Gow3Streamline::Image{static_cast<VkImage>(r.image), static_cast<VkImageView>(r.view),
                                     static_cast<VkFormat>(r.format), r.width, r.height,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL};
    };
    Gow3Streamline::FgFrame fg{
        .depth = image(inputs.depth),
        .motion = image(inputs.motion),
        .camera = inputs.camera,
        .jitter = {inputs.jitter[0], inputs.jitter[1]},
        .reset = frame_gen_reset || inputs.reset || camera_cut,
    };
    if (!covers) {
        fg.sub_x = u32(blit.dstOffsets[0].x);
        fg.sub_y = u32(blit.dstOffsets[0].y);
        fg.sub_w = u32(blit.dstOffsets[1].x - blit.dstOffsets[0].x);
        fg.sub_h = u32(blit.dstOffsets[1].y - blit.dstOffsets[0].y);
    }
    if (!Gow3Streamline::FrameGenSetFrame(frame->serial, &fg)) {
        return off();
    }
    Gow3Streamline::FrameGenSetMode(true, 1);
    frame_gen_reset = false;
    ++generated;
    // Said once per stretch: on, but not interpolating for 2 s (Streamline's own reason is in its
    // log with GOW3_SL_LOG=2, usually the window focus).
    const auto now = std::chrono::steady_clock::now();
    if (last_doubled) {
        undoubled_since = {};
        undoubled_reported = false;
    } else if (undoubled_since == std::chrono::steady_clock::time_point{}) {
        undoubled_since = now;
    } else if (!undoubled_reported && now - undoubled_since >= std::chrono::seconds(2)) {
        undoubled_reported = true;
        const bool focused = Gow3Streamline::WindowFocused(window.GetWindowInfo().render_surface);
        std::printf("Frame generation (Streamline): on but not interpolating for 2 s (game window "
                    "%s); the limiter counts real frames meanwhile\n",
                    focused ? "has the focus" : "does not have the focus");
    }
    return last_doubled;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    // gow3 20 Oct 2026: the time this waits for presentation (counted; Stall lines).
    Gow3Stats::WaitTimer wait_timer{Gow3Stats::frame_wait_ns};
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
