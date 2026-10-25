// SPDX-FileCopyrightText: Copyright 2026 IFreemz, gow3 contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// gow3: NVIDIA DLSS Super Resolution through the optional gow3_dlss.dll bridge (gpu/
// dlss_bridge, MIT), loaded at run time from next to gow3-probe.exe together with NVIDIA's
// nvngx_dlss.dll. The port itself contains no NVIDIA code; without the DLLs, on other GPUs or
// with GOW3_DLSS=0 nothing changes. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR
// (vk_dlss_ngx).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/frame_gen_camera.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Dlss {
public:
    /// The process-wide instance, or null when the DLLs are absent or GOW3_DLSS=0.
    static Dlss* Get();
    ~Dlss();

    /// Instance and device creation: adds what NGX needs (or disables DLSS when it is missing).
    void AppendInstanceExtensions(std::vector<const char*>& enabled);
    void AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                std::vector<const char*>& enabled);
    /// After the device exists.
    void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
    void Shutdown();

    [[nodiscard]] bool Available() const;
    /// Why DLSS cannot run on this system (empty when it can, or before device creation).
    [[nodiscard]] std::string Problem() const;
    /// gow3 23 Oct 2026: the calls go through Streamline (it loaded, gow3_streamline.h)
    /// instead of the NGX bridge. Frame generation then presents through Streamline's swapchain
    /// hooks, not through FrameGenCreate / FrameGenEvaluate.
    [[nodiscard]] bool ViaStreamline() const;

    struct Resource {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        vk::ImageAspectFlags aspect;
        u32 width, height;
    };
    /// NVSDK_NGX_DLSS_Hint_Render_Preset value of a transformer model (0 J, 1 K, 2 L, 3 M).
    static u32 TransformerPreset(int model);
    struct FeatureDesc {
        u32 input_width, input_height, output_width, output_height;
        int quality;
        bool hdr;
        u32 preset; ///< TransformerPreset: a new model recreates the feature

        bool operator==(const FeatureDesc&) const = default;
    };
    struct Frame {
        Resource color, depth, motion, output;
        float jitter_x, jitter_y;
        bool reset;
        float frame_ms;
        float sharpness;
        Resource bias_mask{}; ///< optional (null image): R8 reactive mask, render size
    };

    /// DLSS quality mode for an output/render size ratio (0 DLAA .. 4 Ultra Performance).
    static int QualityForScale(float scale);
    [[nodiscard]] bool HasFeature(const FeatureDesc& desc) const;
    /// Records the feature creation. The GPU must be done with the previous one.
    bool CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc);
    bool Evaluate(vk::CommandBuffer command, const Frame& frame);
    /// Super resolution only; frame generation keeps its own FrameGenRelease (22 Oct review).
    void ReleaseFeature();

    // gow3 22 Oct 2026: DLSS Frame Generation (bridge ABI 3). Only with nvngx_dlssg.dll beside
    // the exe and GOW3_FRAME_GEN_EXT not "0": then its extensions join those of super
    // resolution; anything missing turns off frame generation only.
    [[nodiscard]] bool FrameGenAvailable() const;
    /// Available, the `frame_gen` setting on and toggle FrameGeneration clear (read every frame).
    static bool FrameGenWanted();
    /// Why frame generation cannot run (empty when it can).
    [[nodiscard]] std::string FrameGenProblem() const;
    struct FrameGenDesc {
        u32 width, height;               ///< backbuffer and generated frame
        u32 render_width, render_height; ///< depth and motion vectors
        vk::Format format;               ///< backbuffer format

        bool operator==(const FrameGenDesc&) const = default;
    };
    struct FrameGenFrame {
        Resource backbuffer, depth, motion, output;
        FrameGenCamera camera;
        float jitter_x, jitter_y;
        bool reset, hdr;
    };
    [[nodiscard]] bool HasFrameGenFeature(const FrameGenDesc& desc) const;
    /// Records the feature creation. The GPU must be done with the previous one.
    bool FrameGenCreate(vk::CommandBuffer command, const FrameGenDesc& desc);
    /// One generated frame (2x) between the previous backbuffer and this one.
    bool FrameGenEvaluate(vk::CommandBuffer command, const FrameGenFrame& frame);
    /// The GPU must be done with the feature.
    void FrameGenRelease();

private:
    Dlss();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
