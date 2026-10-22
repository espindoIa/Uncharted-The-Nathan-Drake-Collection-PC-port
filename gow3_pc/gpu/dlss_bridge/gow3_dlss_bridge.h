// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), gow3 contributors
// SPDX-License-Identifier: MIT
//
// C interface between gow3 and gow3_dlss.dll. The port contains no NVIDIA code: it loads the
// bridge at run time when it is present (next to gow3-probe.exe, with nvngx_dlss.dll) and keeps
// its other upscalers when it is absent. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI 3 adds frame generation (the FrameGen* entries at the end of Gow3DlssApi); the table of
// ABI 2 is a prefix of it. Every call into NGX is serialized inside the bridge by one mutex, so
// super resolution and frame generation may be called from different threads.
#define GOW3_DLSS_BRIDGE_ABI 3

// Called from the threads that call the bridge and, when NGX logs (GOW3_NGX_LOG), from NGX's
// own: it must be thread-safe, and it must not call back into the bridge.
typedef void (*Gow3DlssLogFn)(int warning, const char* message);

typedef struct Gow3DlssImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} Gow3DlssImage;

typedef struct Gow3DlssFeature {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    int32_t quality; // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int32_t depth_inverted;
    uint32_t preset; // NVSDK_NGX_DLSS_Hint_Render_Preset value, 0 = driver default
    int32_t hdr;     // 1: linear HDR colour with automatic exposure, 0: tonemapped colour
} Gow3DlssFeature;

typedef struct Gow3DlssEvaluate {
    Gow3DlssImage color;  // sampled
    Gow3DlssImage depth;  // sampled, hardware depth
    Gow3DlssImage motion; // sampled, render-resolution pixels, current to previous
    Gow3DlssImage output; // storage, general layout
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
    float sharpness; // 0..1; 0 leaves the output unsharpened
    // Optional (image VK_NULL_HANDLE: none), render size, R8 0..1: how much to trust this
    // frame's colour over the history (blended effects without motion vectors: snow, sparks).
    Gow3DlssImage bias_mask;
} Gow3DlssEvaluate;

typedef struct Gow3FrameGenFeature {
    uint32_t width, height;                // colour (backbuffer) and interpolated frame size
    uint32_t render_width, render_height;  // size of depth and motion vectors
    uint32_t backbuffer_format;            // VkFormat of the colour image
} Gow3FrameGenFeature;

// The host puts the inputs in a shader-readable layout and the outputs in the general layout
// before the call.
typedef struct Gow3FrameGenEvaluate {
    Gow3DlssImage backbuffer;  // sampled: the frame as it would be presented (HUD included)
    Gow3DlssImage depth;       // sampled, hardware depth
    Gow3DlssImage motion;      // sampled, pixels of the motion image, current to previous
    Gow3DlssImage hudless;     // optional (image VK_NULL_HANDLE: none)
    Gow3DlssImage ui;          // optional (image VK_NULL_HANDLE: none): premultiplied UI colour + alpha
    Gow3DlssImage output;      // storage: the interpolated frame, same size and format as backbuffer
    Gow3DlssImage output_real; // optional storage (VK_NULL_HANDLE: none): copy of the real frame
    float camera_view_to_clip[16];   // row-major, post-multiplication, without jitter
    float clip_to_camera_view[16];
    float clip_to_prev_clip[16];
    float prev_clip_to_clip[16];
    float camera_pos[3], camera_up[3], camera_right[3], camera_fwd[3];
    float camera_near, camera_far, camera_fov, camera_aspect;
    float mvec_scale[2];
    float jitter[2];
    uint32_t multi_frame_count;   // 1 = 2x
    uint32_t multi_frame_index;   // 1..multi_frame_count
    // NGX flags. reset: this frame has no link to the last one (NGX ignores it unless
    // multi_frame_index is 1). camera_motion_included: the motion vectors hold camera motion.
    // not_rendering_game_frames: a menu or a video. hdr: the colour images are an HDR display
    // signal. menu_detection: let NGX look for full-screen menus.
    int32_t reset;
    int32_t depth_inverted;
    int32_t camera_motion_included;
    int32_t not_rendering_game_frames;
    int32_t hdr;
    int32_t menu_detection;
} Gow3FrameGenEvaluate;

typedef struct Gow3DlssApi {
    uint32_t abi;
    // UTF-16 paths: the directory with nvngx_dlss.dll, and a writable data directory.
    // The environment variable GOW3_NGX_LOG (0 off, the default; 1 normal; 2 verbose), read
    // here, sets how much NGX logs: its lines reach `log` prefixed "NGX: ".
    int32_t (*Configure)(const wchar_t* dll_directory, const wchar_t* data_directory,
                         Gow3DlssLogFn log);
    // Vulkan extensions NGX needs. The arrays stay valid until Shutdown.
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(VkCommandBuffer command, const Gow3DlssFeature* feature);
    int32_t (*Evaluate)(VkCommandBuffer command, const Gow3DlssEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature. Releases the frame
    // generation feature too.
    void (*ReleaseFeature)(void);
    void (*Shutdown)(void);

    // ABI 3: DLSS Frame Generation through NGX (RTX 40 and newer; more than one generated frame
    // per real one on RTX 50), which also needs nvngx_dlssg.dll beside nvngx_dlss.dll.
    // Initialize does not fail when it is unsupported: only FrameGenSupport reports that.
    // After Initialize. 1: supported, and *max_multi_frame_count is the largest multi_frame_count
    // (>= 1; 1 is 2x only, 3 up to 4x). 0: not supported.
    int32_t (*FrameGenSupport)(uint32_t* max_multi_frame_count);
    // InstanceExtensions / DeviceExtensions for frame generation: only its own list, the host
    // merges them. Like those, they work before Initialize. NGX may hand back the same array for
    // both features, so copy the names before the next query.
    int32_t (*FrameGenInstanceExtensions)(uint32_t* count,
                                          const VkExtensionProperties** extensions);
    int32_t (*FrameGenDeviceExtensions)(VkInstance instance, VkPhysicalDevice physical,
                                        uint32_t* count, const VkExtensionProperties** extensions);
    // Releases a previous frame generation feature first. Create it again when width, height
    // or format change.
    int32_t (*FrameGenCreate)(VkCommandBuffer command, const Gow3FrameGenFeature* feature);
    // One call interpolates one frame: multi_frame_count calls per real frame, with
    // multi_frame_index 1..multi_frame_count. NGX takes the first call after Create as a reset.
    int32_t (*FrameGenEvaluate)(VkCommandBuffer command, const Gow3FrameGenEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature.
    void (*FrameGenRelease)(void);
} Gow3DlssApi;

typedef const Gow3DlssApi* (*Gow3DlssGetApiFn)(void);

#ifdef __cplusplus
// The table of ABI 2 stays a prefix of the table of ABI 3: after `abi` the entries follow in
// this order, one pointer each.
static_assert(offsetof(Gow3DlssApi, Configure) == 1 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, InstanceExtensions) == 2 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, DeviceExtensions) == 3 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, Initialize) == 4 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, CreateFeature) == 5 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, Evaluate) == 6 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, ReleaseFeature) == 7 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, Shutdown) == 8 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, FrameGenSupport) == 9 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, FrameGenInstanceExtensions) == 10 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, FrameGenDeviceExtensions) == 11 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, FrameGenCreate) == 12 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, FrameGenEvaluate) == 13 * sizeof(void*) &&
                  offsetof(Gow3DlssApi, FrameGenRelease) == 14 * sizeof(void*),
              "Gow3DlssApi: the ABI 2 entries must stay first, in order");
#endif

#ifdef __cplusplus
}
#endif
