// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <optional>

#include <condition_variable>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/frame_gen_camera.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    /// gow3 22 Oct 2026: the frame's id for NVIDIA Reflex, the ordinal of the guest flip it shows
    /// (Gow3Reflex::PreparedFlip, set when the frame is prepared); 0 for the black frames drawn
    /// without a flip.
    u64 serial{};
    /// gow3 22 Oct 2026: the frame's DLSS Frame Generation inputs (Presenter::PresentGenerated),
    /// taken from the upscaler when the frame is prepared; false for frames without an upscale.
    bool frame_gen{};
    TemporalUpscaler::FrameGenInputs frame_gen_inputs{};
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    const Instance& GetInstance() const {
        return instance;
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    /// gow3: a black frame with the loading screen (Gow3Overlay::SetLoading) only.
    void PresentLoadingFrame();
    Frame* PrepareLastFrame();

    /// gow3 22 Oct 2026: the last game frame was presented with a generated frame before it
    /// (the frame limiter paces real frames at half the limit then).
    [[nodiscard]] bool FrameGenActive() const noexcept {
        return frame_gen_active.load(std::memory_order_relaxed);
    }

private:
    Frame* GetRenderFrame();

    /// DLSS Frame Generation: evaluates and presents the frame between the last one and `frame`,
    /// then waits until `frame` is due. False (nothing presented) when it does not apply.
    bool PresentGenerated(Frame* frame);
    /// gow3 23 Oct 2026: frame generation through Streamline (Dlss::ViaStreamline): loads or
    /// unloads its plugin with the setting (the swapchain is made again around it), then hands
    /// the frame's inputs and the mode over before the present, which Streamline's hook turns
    /// into the generated frame and the real one. True when frame generation is on for it.
    void UpdateStreamlineFrameGenLoad();
    bool StreamlineFrameGen(Frame* frame, bool game_frame);
    /// Blits `source` (General layout, kept) letterboxed into the acquired swapchain image and
    /// draws the overlay over it; the image ends in the present layout.
    void BlitToSwapchain(vk::CommandBuffer cmdbuf, vk::Image source, u32 width, u32 height);

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< gow3: GOW3_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    // DLSS Frame Generation (swap thread).
    VideoCore::UniqueImage frame_gen_output;
    vk::UniqueImageView frame_gen_output_view;
    u32 frame_gen_width{}, frame_gen_height{};
    vk::Format frame_gen_format{};
    std::optional<Dlss::FrameGenDesc> frame_gen_failed; ///< creation failed for this one
    bool frame_gen_reset{true};
    FrameGenCamera frame_gen_prev_camera; ///< the last real frame evaluated, for IsCameraCut
    std::atomic<bool> frame_gen_active{false};
    std::chrono::steady_clock::time_point frame_gen_last_real{};
};

} // namespace Vulkan
