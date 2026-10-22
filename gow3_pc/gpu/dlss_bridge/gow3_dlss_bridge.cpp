// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), gow3 contributors
// SPDX-License-Identifier: MIT
//
// gow3_dlss.dll: the only part of the project that uses the NVIDIA DLSS (NGX) SDK. Built with
// MSVC (the SDK's libraries are MSVC only); the port loads it at run time.
// Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR (dlss_bridge).

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "gow3_dlss_bridge.h" // Vulkan first: the NGX headers expect its types

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>

#include <windows.h> // GetEnvironmentVariableA

namespace {

constexpr char ProjectId[] = "42359704-c9f3-4806-9fb5-d469000fdb8a";
constexpr char EngineVersion[] = "gow3-windows";

struct State {
    Gow3DlssLogFn log{};
    std::wstring dll_directory;
    std::wstring data_directory;
    const wchar_t* dll_path{};
    NVSDK_NGX_FeatureCommonInfo common{};
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    NVSDK_NGX_FeatureDiscoveryInfo frame_gen_discovery{}; // discovery with the frame generation id
    NVSDK_NGX_Parameter* capabilities{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    Gow3DlssFeature feature_desc{};
    // Its own parameter map: one map per feature keeps values from carrying over (NGX guide, 3).
    NVSDK_NGX_Parameter* frame_gen_parameters{};
    NVSDK_NGX_Handle* frame_gen_feature{};
    VkDevice device{};
    bool configured{};
    bool initialized{};
} state;

// NGX is not thread-safe, and the host calls super resolution from the draw recording thread and
// frame generation from the swap thread: every public function that touches NGX or `state` holds
// this. The NGX log callback never does (NGX may call it from any thread, inside these calls).
std::mutex ngx_mutex;

template <typename... Args>
void Log(int warning, const char* format, Args... args) {
    if (!state.log)
        return;
    std::array<char, 1024> text{};
    std::snprintf(text.data(), text.size(), format, args...);
    state.log(warning, text.data());
}

bool Check(const char* operation, NVSDK_NGX_Result result, int warning = 1) {
    if (NVSDK_NGX_FAILED(result)) {
        Log(warning, "%s failed: 0x%08x", operation, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    Log(0, "NGX: %s", message ? message : "");
}

// GOW3_NGX_LOG: 0 off (default), 1 normal, 2 verbose. NGX lines reach the host log via NgxLog.
NVSDK_NGX_Logging_Level NgxLogLevel() {
    static constexpr std::array levels{NVSDK_NGX_LOGGING_LEVEL_OFF, NVSDK_NGX_LOGGING_LEVEL_ON,
                                       NVSDK_NGX_LOGGING_LEVEL_VERBOSE};
    char text[16]{};
    const DWORD length = GetEnvironmentVariableA("GOW3_NGX_LOG", text, sizeof(text));
    return levels[length > 0 && length < sizeof(text) ? std::clamp(std::atoi(text), 0, 2) : 0];
}

int32_t Configure(const wchar_t* dll_directory, const wchar_t* data_directory, Gow3DlssLogFn log) {
    std::lock_guard lock(ngx_mutex);
    state.log = log;
    state.dll_directory = dll_directory ? dll_directory : L".";
    state.data_directory = data_directory ? data_directory : L".";
    state.dll_path = state.dll_directory.c_str();
    state.common.PathListInfo = {&state.dll_path, 1};
    state.common.LoggingInfo = {NgxLog, NgxLogLevel(), false};
    state.discovery.SDKVersion = NVSDK_NGX_Version_API;
    state.discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    state.discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    state.discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                EngineVersion};
    state.discovery.ApplicationDataPath = state.data_directory.c_str();
    state.discovery.FeatureInfo = &state.common;
    state.frame_gen_discovery = state.discovery;
    state.frame_gen_discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    state.configured = true;
    return 1;
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    std::lock_guard lock(ngx_mutex);
    VkExtensionProperties* required{};
    if (!state.configured ||
        !Check("Instance extension query",
               NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&state.discovery, count,
                                                                       &required)))
        return 0;
    *extensions = required;
    return 1;
}

int32_t DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    std::lock_guard lock(ngx_mutex);
    if (!state.configured)
        return 0;
    NVSDK_NGX_FeatureRequirement support{};
    if (!Check("Feature requirement query",
               NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &state.discovery,
                                                       &support)))
        return 0;
    if (support.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        Log(1, "DLSS is not supported on this GPU or driver (code 0x%x)",
            static_cast<unsigned>(support.FeatureSupported));
        return 0;
    }
    VkExtensionProperties* required{};
    if (!Check("Device extension query",
               NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                   instance, physical, &state.discovery, count, &required)))
        return 0;
    *extensions = required;
    return 1;
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   PFN_vkGetInstanceProcAddr get_instance_proc,
                   PFN_vkGetDeviceProcAddr get_device_proc) {
    std::lock_guard lock(ngx_mutex);
    if (!state.configured || state.initialized)
        return 0;
    if (!Check("NGX initialization",
               NVSDK_NGX_VULKAN_Init_with_ProjectID(
                   ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion,
                   state.data_directory.c_str(), instance, physical, device, get_instance_proc,
                   get_device_proc, &state.common)))
        return 0;
    state.initialized = true;
    state.device = device;
    if (!Check("Capability query", NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.capabilities)))
        return 0;
    int available{}, needs_driver{};
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_SuperSampling_Available,
                             &available);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (needs_driver) {
        Log(1, "DLSS needs a newer NVIDIA driver");
        return 0;
    }
    if (!available) {
        Log(1, "DLSS Super Resolution is not available on this system");
        return 0;
    }
    return 1;
}

// The Drop* functions run with ngx_mutex held by the public function that calls them.
void DropFeature() {
    if (state.feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(state.feature);
        state.feature = nullptr;
    }
    if (state.parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.parameters);
        state.parameters = nullptr;
    }
}

void DropFrameGen() {
    if (state.frame_gen_feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(state.frame_gen_feature);
        state.frame_gen_feature = nullptr;
    }
    if (state.frame_gen_parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.frame_gen_parameters);
        state.frame_gen_parameters = nullptr;
    }
}

void ReleaseFeature() {
    std::lock_guard lock(ngx_mutex);
    DropFeature();
    DropFrameGen();
}

int32_t CreateFeature(VkCommandBuffer command, const Gow3DlssFeature* desc) {
    std::lock_guard lock(ngx_mutex);
    if (!state.initialized || !state.capabilities || !desc)
        return 0;
    DropFeature();
    if (!Check("Parameter allocation", NVSDK_NGX_VULKAN_AllocateParameters(&state.parameters)))
        return 0;
    for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(state.parameters, key, desc->preset);
    static constexpr std::array qualities{
        NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    // The render size NGX recommends for this mode and output (Programming Guide 3.3), logged
    // beside the port's own preset size (SceneResolution::ForPreset) so the two can be compared.
    unsigned optimal_w{}, optimal_h{}, max_w{}, max_h{}, min_w{}, min_h{};
    float optimal_sharpness{};
    if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_OPTIMAL_SETTINGS(
            state.capabilities, desc->output_width, desc->output_height,
            qualities[std::clamp(desc->quality, 0, 4)], &optimal_w, &optimal_h, &max_w, &max_h,
            &min_w, &min_h, &optimal_sharpness))) {
        const bool within = desc->input_width >= min_w && desc->input_width <= max_w &&
                            desc->input_height >= min_h && desc->input_height <= max_h;
        Log(within ? 0 : 1, "DLSS optimal render size %ux%u for %ux%u (dynamic %ux%u..%ux%u); "
            "using %ux%u%s", optimal_w, optimal_h, desc->output_width, desc->output_height,
            min_w, min_h, max_w, max_h, desc->input_width, desc->input_height,
            within ? "" : ", outside the supported range");
    } else {
        Log(1, "DLSS optimal settings query failed");
    }
    NVSDK_NGX_DLSS_Create_Params settings{};
    settings.Feature.InWidth = desc->input_width;
    settings.Feature.InHeight = desc->input_height;
    settings.Feature.InTargetWidth = desc->output_width;
    settings.Feature.InTargetHeight = desc->output_height;
    settings.Feature.InPerfQualityValue = qualities[std::clamp(desc->quality, 0, 4)];
    settings.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        (desc->depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
        (desc->hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                   : 0);
    const auto result = NGX_VULKAN_CREATE_DLSS_EXT1(state.device, command, 1, 1, &state.feature,
                                                   state.parameters, &settings);
    if (!Check("DLSS feature creation", result) || !state.feature) {
        state.feature = nullptr;
        return 0;
    }
    state.feature_desc = *desc;
    Log(0, "DLSS %ux%u -> %ux%u, quality %d, preset %u%s", desc->input_width, desc->input_height,
        desc->output_width, desc->output_height, desc->quality, desc->preset,
        desc->hdr ? ", HDR input" : "");
    return 1;
}

NVSDK_NGX_Resource_VK Wrap(const Gow3DlssImage& image, bool writable) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
                                                  image.format, image.width, image.height,
                                                  writable);
}

int32_t Evaluate(VkCommandBuffer command, const Gow3DlssEvaluate* evaluate) {
    std::lock_guard lock(ngx_mutex);
    if (!state.feature || !state.parameters || !evaluate)
        return 0;
    auto color = Wrap(evaluate->color, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
    auto output = Wrap(evaluate->output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params parameters{};
    parameters.Feature.pInColor = &color;
    parameters.Feature.pInOutput = &output;
    parameters.Feature.InSharpness = evaluate->sharpness;
    parameters.pInDepth = &depth;
    parameters.pInMotionVectors = &motion;
    parameters.InRenderSubrectDimensions = {state.feature_desc.input_width,
                                            state.feature_desc.input_height};
    parameters.InJitterOffsetX = evaluate->jitter_x;
    parameters.InJitterOffsetY = evaluate->jitter_y;
    parameters.InReset = evaluate->reset;
    parameters.InMVScaleX = parameters.InMVScaleY = 1.0f;
    parameters.InPreExposure = parameters.InExposureScale = 1.0f;
    parameters.InFrameTimeDeltaInMsec = evaluate->frame_ms;
    NVSDK_NGX_Resource_VK bias{};
    if (evaluate->bias_mask.image != VK_NULL_HANDLE) {
        bias = Wrap(evaluate->bias_mask, false);
        parameters.pInBiasCurrentColorMask = &bias;
    }
    return Check("DLSS evaluation", NGX_VULKAN_EVALUATE_DLSS_EXT(command, state.feature,
                                                                state.parameters, &parameters))
               ? 1
               : 0;
}

void Shutdown() {
    std::lock_guard lock(ngx_mutex);
    DropFeature();
    DropFrameGen(); // before NVSDK_NGX_VULKAN_Shutdown1: the feature belongs to the NGX instance
    if (state.capabilities) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.capabilities);
        state.capabilities = nullptr;
    }
    if (state.initialized) {
        NVSDK_NGX_VULKAN_Shutdown1(state.device);
        state.initialized = false;
        state.device = VK_NULL_HANDLE;
    }
}

// Frame generation (ABI 3). Initialize ignores it: a GPU without it still gets super resolution.

// Availability from the capability map (valid after Initialize). The multi-frame maximum is 1
// (2x only) when NGX reports none.
bool FrameGenAvailable(uint32_t& max_multi_frame) {
    if (!state.initialized || !state.capabilities)
        return false;
    int available{};
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_FrameGeneration_Available,
                             &available);
    if (!available) {
        int needs_driver{};
        uint32_t init_result{};
        NVSDK_NGX_Parameter_GetI(state.capabilities,
                                 NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,
                                 &needs_driver);
        NVSDK_NGX_Parameter_GetUI(state.capabilities,
                                  NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,
                                  &init_result);
        // Not a warning: RTX 30 and older, other vendors and a missing nvngx_dlssg.dll (22 Oct,
        // RTX 5070: init result 0xbad00004, FeatureNotFound) are all expected somewhere.
        Log(0, "DLSS Frame Generation is not available (NGX init result 0x%08x%s)", init_result,
            needs_driver ? ", needs a newer NVIDIA driver" : "");
        return false;
    }
    uint32_t maximum{};
    NVSDK_NGX_Parameter_GetUI(state.capabilities, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
                              &maximum);
    max_multi_frame = std::max(maximum, 1u);
    return true;
}

int32_t FrameGenSupport(uint32_t* max_multi_frame_count) {
    std::lock_guard lock(ngx_mutex);
    uint32_t maximum{};
    if (!FrameGenAvailable(maximum))
        return 0;
    if (max_multi_frame_count)
        *max_multi_frame_count = maximum;
    return 1;
}

int32_t FrameGenInstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    std::lock_guard lock(ngx_mutex);
    VkExtensionProperties* required{};
    if (!state.configured ||
        !Check("Frame generation instance extension query",
               NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
                   &state.frame_gen_discovery, count, &required), 0))
        return 0;
    *extensions = required;
    return 1;
}

int32_t FrameGenDeviceExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                 const VkExtensionProperties** extensions) {
    std::lock_guard lock(ngx_mutex);
    if (!state.configured)
        return 0;
    // Queries and the "not supported" answer log as information, not warnings: 22 Oct, without
    // nvngx_dlssg.dll beside nvngx_dlss.dll the requirement query itself fails (0xbad00012,
    // NotImplemented), and a player may well not have that file.
    NVSDK_NGX_FeatureRequirement support{};
    if (!Check("Frame generation feature requirement query",
               NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical,
                                                       &state.frame_gen_discovery, &support), 0))
        return 0;
    if (support.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        Log(0, "DLSS Frame Generation is not supported on this GPU or driver (code 0x%x)",
            static_cast<unsigned>(support.FeatureSupported));
        return 0;
    }
    VkExtensionProperties* required{};
    if (!Check("Frame generation device extension query",
               NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                   instance, physical, &state.frame_gen_discovery, count, &required), 0))
        return 0;
    *extensions = required;
    return 1;
}

int32_t FrameGenCreate(VkCommandBuffer command, const Gow3FrameGenFeature* desc) {
    std::lock_guard lock(ngx_mutex);
    if (!state.initialized || !state.capabilities || !desc)
        return 0;
    DropFrameGen();
    uint32_t maximum{};
    if (!FrameGenAvailable(maximum) ||
        !Check("Frame generation parameter allocation",
               NVSDK_NGX_VULKAN_AllocateParameters(&state.frame_gen_parameters)))
        return 0;
    NVSDK_NGX_DLSSG_Create_Params settings{};
    settings.Width = desc->width;
    settings.Height = desc->height;
    settings.NativeBackbufferFormat = desc->backbuffer_format;
    settings.RenderWidth = desc->render_width;
    settings.RenderHeight = desc->render_height;
    // 22 Oct, nvngx_dlssg.dll 310.9.16: with only the generic Width/Height that the helper sets
    // NGX logs a deprecation warning; the DLSSG ones take their place. The D3D12 helper also
    // passes the render size (what NGX expects depth and motion vectors to measure) but
    // NGX_VK_CREATE_DLSSG does not, so it is set here (on an RTX 5070 the output is the same
    // without it).
    NVSDK_NGX_Parameter_SetUI(state.frame_gen_parameters, NVSDK_NGX_DLSSG_Parameter_Width,
                              settings.Width);
    NVSDK_NGX_Parameter_SetUI(state.frame_gen_parameters, NVSDK_NGX_DLSSG_Parameter_Height,
                              settings.Height);
    NVSDK_NGX_Parameter_SetUI(state.frame_gen_parameters, NVSDK_NGX_DLSSG_Parameter_InternalWidth,
                              settings.RenderWidth);
    NVSDK_NGX_Parameter_SetUI(state.frame_gen_parameters, NVSDK_NGX_DLSSG_Parameter_InternalHeight,
                              settings.RenderHeight);
    NVSDK_NGX_Parameter_SetUI(state.frame_gen_parameters,
                              NVSDK_NGX_DLSSG_Parameter_DynamicResolution,
                              settings.DynamicResolutionScaling);
    const auto result = NGX_VK_CREATE_DLSSG(command, 1, 1, &state.frame_gen_feature,
                                            state.frame_gen_parameters, &settings);
    if (!Check("Frame generation feature creation", result) || !state.frame_gen_feature) {
        DropFrameGen();
        return 0;
    }
    Log(0, "DLSS Frame Generation %ux%u (depth and motion %ux%u), VkFormat %u, multi-frame "
        "maximum %u", desc->width, desc->height, desc->render_width, desc->render_height,
        desc->backbuffer_format, maximum);
    return 1;
}

// Copy between a host array and the NGX struct's array, which must be the same size.
template <typename To, typename From>
void Copy(To& to, const From& from) {
    static_assert(sizeof(To) == sizeof(From));
    std::memcpy(&to, &from, sizeof(To));
}

int32_t FrameGenEvaluate(VkCommandBuffer command, const Gow3FrameGenEvaluate* evaluate) {
    std::lock_guard lock(ngx_mutex);
    if (!state.frame_gen_feature || !state.frame_gen_parameters || !evaluate)
        return 0;
    auto backbuffer = Wrap(evaluate->backbuffer, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
    auto output = Wrap(evaluate->output, true);
    NVSDK_NGX_VK_DLSSG_Eval_Params resources{};
    resources.pBackbuffer = &backbuffer;
    resources.pDepth = &depth;
    resources.pMVecs = &motion;
    resources.pOutputInterpFrame = &output;
    NVSDK_NGX_Resource_VK hudless{}, ui{}, output_real{};
    if (evaluate->hudless.image != VK_NULL_HANDLE) {
        hudless = Wrap(evaluate->hudless, false);
        resources.pHudless = &hudless;
    }
    if (evaluate->ui.image != VK_NULL_HANDLE) {
        ui = Wrap(evaluate->ui, false);
        resources.pUI = &ui;
    }
    if (evaluate->output_real.image != VK_NULL_HANDLE) {
        output_real = Wrap(evaluate->output_real, true);
        resources.pOutputRealFrame = &output_real;
    }
    NVSDK_NGX_DLSSG_Opt_Eval_Params options{}; // the subrects stay zero: whole images
    options.multiFrameCount = evaluate->multi_frame_count;
    options.multiFrameIndex = evaluate->multi_frame_index;
    Copy(options.cameraViewToClip, evaluate->camera_view_to_clip);
    Copy(options.clipToCameraView, evaluate->clip_to_camera_view);
    Copy(options.clipToPrevClip, evaluate->clip_to_prev_clip);
    Copy(options.prevClipToClip, evaluate->prev_clip_to_clip);
    // No lens distortion, and the host has no matrix for it: identity, not a zero matrix.
    for (int i = 0; i < 4; ++i)
        options.clipToLensClip[i][i] = 1.0f;
    Copy(options.jitterOffset, evaluate->jitter);
    Copy(options.mvecScale, evaluate->mvec_scale);
    Copy(options.cameraPos, evaluate->camera_pos);
    Copy(options.cameraUp, evaluate->camera_up);
    Copy(options.cameraRight, evaluate->camera_right);
    Copy(options.cameraFwd, evaluate->camera_fwd);
    options.cameraNear = evaluate->camera_near;
    options.cameraFar = evaluate->camera_far;
    options.cameraFOV = evaluate->camera_fov;
    options.cameraAspectRatio = evaluate->camera_aspect;
    options.colorBuffersHDR = evaluate->hdr != 0;
    options.depthInverted = evaluate->depth_inverted != 0;
    options.cameraMotionIncluded = evaluate->camera_motion_included != 0;
    options.reset = evaluate->reset != 0;
    options.notRenderingGameFrames = evaluate->not_rendering_game_frames != 0;
    options.motionVectorsInvalidValue = FLT_MAX;
    options.menuDetectionEnabled = evaluate->menu_detection != 0;
    return Check("Frame generation evaluation",
                 NGX_VK_EVALUATE_DLSSG(command, state.frame_gen_feature,
                                       state.frame_gen_parameters, &resources, &options))
               ? 1
               : 0;
}

void FrameGenRelease() {
    std::lock_guard lock(ngx_mutex);
    DropFrameGen();
}

// Same order as the struct: the ABI 2 entries first, then the ABI 3 ones.
const Gow3DlssApi api{GOW3_DLSS_BRIDGE_ABI,
                      Configure,
                      InstanceExtensions,
                      DeviceExtensions,
                      Initialize,
                      CreateFeature,
                      Evaluate,
                      ReleaseFeature,
                      Shutdown,
                      FrameGenSupport,
                      FrameGenInstanceExtensions,
                      FrameGenDeviceExtensions,
                      FrameGenCreate,
                      FrameGenEvaluate,
                      FrameGenRelease};

} // namespace

extern "C" __declspec(dllexport) const Gow3DlssApi* Gow3DlssGetApi() {
    return &api;
}
