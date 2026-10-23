// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 23 Oct 2026: NVIDIA Streamline 2.14 (github.com/NVIDIA-RTX/Streamline, MIT) as the second way
// to NVIDIA's features beside the NGX bridge (gpu/dlss_bridge): DLSS super resolution (sl.dlss),
// DLSS Frame Generation (sl.dlss_g), Reflex (sl.reflex) and PC Latency markers (sl.pcl), chosen at
// start with nv_backend=streamline (setting) or GOW3_NV_BACKEND=streamline. The port links nothing
// from it: sl.interposer.dll is loaded from beside the executable after its NVIDIA signature is
// checked, and every call goes through the pointers it exports. Without the DLLs, on other GPUs or
// when anything fails before the Vulkan instance exists, the NGX bridge is used as before.
//
// Vulkan: Streamline's own vkCreateInstance / vkCreateDevice add the extensions, features and
// queues its plugins ask for (the optical flow queue of frame generation included) and hand it the
// device; the swapchain, present, acquire, surface and device-wait-idle entries go through its
// hooks. Everything else stays on vulkan-1's functions: sl.interposer's lookups would also wrap
// vkCmdBindPipeline, vkCmdPipelineBarrier, vkCreateImage and others, thousands of calls a frame on
// the CPU-bound recording threads.
//
// Only gow3_streamline.cpp includes Streamline's headers (C++20: sl_pcl.h does not compile as
// C++23); this header has no Streamline types.

#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

#include "gow3_streamline_mode.h"
#include "video_core/renderer_vulkan/frame_gen_camera.h"

namespace Gow3Streamline {

/// The backend asked for this run (setting nv_backend, GOW3_NV_BACKEND), whether or not it loads.
bool Requested();

/// Before the Vulkan instance, once: checks and loads sl.interposer.dll and calls slInit with DLSS,
/// DLSS-G, Reflex and PCL. True when Streamline is used for this run.
bool Init();

/// Streamline is used for this run (Init succeeded).
bool Active();
/// Why Streamline is not used although it was asked for (empty otherwise).
const char* Problem();

/// sl.interposer's lookups. Only for the functions Streamline must see (see above).
PFN_vkVoidFunction InstanceProc(VkInstance instance, const char* name);
PFN_vkVoidFunction DeviceProc(VkDevice device, const char* name);

/// After vkCreateDevice went through Streamline: what each feature supports on this GPU.
void OnDevice(VkPhysicalDevice physical, VkDevice device);
/// slShutdown, before the device is destroyed.
void Shutdown();

bool DlssReady();
bool FrameGenReady();
bool ReflexReady();
/// Why the feature cannot run here (empty when it can).
const char* DlssProblem();
const char* FrameGenProblem();
/// The largest numFramesToGenerate (1 = 2x).
uint32_t FrameGenMaxGenerated();

struct Image {
    VkImage image{};
    VkImageView view{};
    VkFormat format{};
    uint32_t width{}, height{};
    VkImageUsageFlags usage{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_GENERAL};
};

// ---- DLSS super resolution (viewport 1; frame generation has viewport 0) ----

struct SrOptions {
    uint32_t input_width, input_height, output_width, output_height;
    uint32_t preset; ///< DLSSPreset value: 10 J, 11 K, 12 L, 13 M
    bool hdr;        ///< linear HDR colour, automatic exposure
};
/// The mode is the one whose render size range holds the input (see PickDlssMode). False when
/// none does or Streamline refused.
bool SrSetOptions(const SrOptions& options);
/// Drops the feature (the GPU must be done with it).
void SrRelease();
struct SrFrame {
    Image color, depth, motion, output;
    Image bias; ///< optional (null image): R8, how much to trust this frame's colour
    float jitter_x, jitter_y;
    bool reset;
};
/// Records DLSS into `cmd`; the images stay in their layouts (Streamline transitions and restores).
bool SrEvaluate(VkCommandBuffer cmd, const SrFrame& frame);

// ---- Reflex and PC Latency ----

enum class Marker : uint32_t {
    SimulationStart = 0,
    SimulationEnd = 1,
    RenderSubmitStart = 2,
    RenderSubmitEnd = 3,
    PresentStart = 4,
    PresentEnd = 5,
    ControllerInputSample = 13,
};
void SetMarker(Marker marker, uint64_t frame);
/// slReflexSleep: blocks until the driver wants the frame to start (does nothing with Reflex off).
void Sleep(uint64_t frame);
/// 0 off, 1 on, 2 on + boost. Sent when it changes.
void SetReflexMode(int mode);
struct ReflexReport {
    uint64_t frame, input_sample, sim_start, sim_end, render_submit_start, render_submit_end,
        present_start, present_end, driver_start, driver_end, os_queue_start, os_queue_end,
        gpu_render_start, gpu_render_end;
};
/// The driver's last frame reports (up to 64), oldest first; 0 when there are none.
uint32_t ReflexReports(ReflexReport* out, uint32_t capacity);

// ---- DLSS Frame Generation (viewport 0) ----

/// Loads or unloads the plugin. Its swapchain hooks act at swapchain creation, so the caller
/// recreates the swapchain after a change (Streamline's DLSS-G guide, section 18).
void SetFrameGenLoaded(bool loaded);
bool FrameGenLoaded();
struct FgFrame {
    Image depth, motion; ///< render size, depth not inverted, motion in pixels (current to previous)
    Vulkan::FrameGenCamera camera;
    float jitter[2];
    bool reset;
    /// Where the game's image sits in the backbuffer (letterbox); all 0 for the whole image.
    uint32_t sub_x, sub_y, sub_w, sub_h;
};
/// Swap thread, before the present of game frame `frame` (its Reflex id): the frame's tags and
/// constants. Null clears the tags (a frame that is not a game frame).
bool FrameGenSetFrame(uint64_t frame, const FgFrame* inputs);
/// Swap thread, before the present: on with `generated` frames per real one, or off (resources
/// kept). Sent when it changes.
void FrameGenSetMode(bool on, uint32_t generated);
struct FgState {
    bool ok;
    uint32_t status;          ///< sl::DLSSGStatus bits
    uint32_t presented;       ///< frames presented since the last call
    uint32_t max_generated;
};
FgState FrameGenGetState();
/// The window (an HWND) has the keyboard focus: Streamline's DLSS-G only interpolates then.
bool WindowFocused(void* window);

} // namespace Gow3Streamline
