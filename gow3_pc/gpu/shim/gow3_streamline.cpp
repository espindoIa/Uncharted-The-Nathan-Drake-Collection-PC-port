// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 23 Oct 2026: see gow3_streamline.h. Compiled as C++20 (CMakeLists.txt): Streamline's
// sl_pcl.h aliases std::to_underlying under C++23, which does not compile.
#include "gow3_streamline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>

#include "sl.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"
#include "sl_security.h"
#endif

#include "gow3_settings.h"

namespace Gow3Streamline {

bool Requested() {
    static const bool requested = [] {
        const char* env = std::getenv("GOW3_NV_BACKEND");
        return !(env && (std::strcmp(env, "ngx") == 0 || std::strcmp(env, "0") == 0));
    }();
    return requested;
}

#ifdef _WIN32

namespace {

constexpr wchar_t InterposerName[] = L"sl.interposer.dll";
/// The NGX bridge's project id (gpu/dlss_bridge): the same application for NVIDIA's NGX.
constexpr char ProjectId[] = "42359704-c9f3-4806-9fb5-d469000fdb8a";
constexpr char EngineVersion[] = "gow3-windows";
const sl::ViewportHandle FrameGenViewport{0u};
const sl::ViewportHandle SrViewport{1u};

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

const char* ResultText(sl::Result result) {
    switch (result) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorIO: return "I/O error";
    case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old";
    case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off";
    case sl::Result::eErrorDeviceNotCreated: return "no device yet";
    case sl::Result::eErrorNoSupportedAdapterFound: return "no supported GPU (GeForce RTX needed)";
    case sl::Result::eErrorAdapterNotSupported: return "this GPU is not supported";
    case sl::Result::eErrorNoPlugins: return "no plugins found";
    case sl::Result::eErrorVulkanAPI: return "Vulkan error";
    case sl::Result::eErrorDXGIAPI: return "DXGI error";
    case sl::Result::eErrorD3DAPI: return "D3D error";
    case sl::Result::eErrorNRDAPI: return "NRD error";
    case sl::Result::eErrorNVAPI: return "NVAPI error";
    case sl::Result::eErrorReflexAPI: return "Reflex error";
    case sl::Result::eErrorNGXFailed: return "NGX failed";
    case sl::Result::eErrorJSONParsing: return "plugin configuration error";
    case sl::Result::eErrorMissingProxy: return "missing proxy";
    case sl::Result::eErrorMissingResourceState: return "missing resource state";
    case sl::Result::eErrorInvalidIntegration: return "invalid integration";
    case sl::Result::eErrorMissingInputParameter: return "missing input";
    case sl::Result::eErrorNotInitialized: return "not initialized";
    case sl::Result::eErrorComputeFailed: return "compute failed";
    case sl::Result::eErrorInitNotCalled: return "slInit not called";
    case sl::Result::eErrorExceptionHandler: return "exception";
    case sl::Result::eErrorInvalidParameter: return "invalid parameter";
    case sl::Result::eErrorMissingConstants: return "missing constants";
    case sl::Result::eErrorDuplicatedConstants: return "constants set twice for one frame";
    case sl::Result::eErrorMissingOrInvalidAPI: return "missing or invalid API";
    case sl::Result::eErrorCommonConstantsMissing: return "common constants missing";
    case sl::Result::eErrorUnsupportedInterface: return "unsupported interface";
    case sl::Result::eErrorFeatureMissing: return "feature not loaded (plugin or nvngx DLL missing)";
    case sl::Result::eErrorFeatureNotSupported: return "feature not supported here";
    case sl::Result::eErrorFeatureMissingHooks: return "feature hooks missing";
    case sl::Result::eErrorFeatureFailedToLoad: return "feature failed to load";
    case sl::Result::eErrorFeatureWrongPriority: return "feature priority";
    case sl::Result::eErrorFeatureMissingDependency: return "feature dependency missing";
    case sl::Result::eErrorFeatureManagerInvalidState: return "feature manager state";
    case sl::Result::eErrorInvalidState: return "invalid state";
    default: return "error";
    }
}

struct State {
    HMODULE module{};
    std::string problem;
    bool tried{}, active{};

    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slIsFeatureSupported* is_supported{};
    PFun_slIsFeatureLoaded* is_loaded{};
    PFun_slSetFeatureLoaded* set_loaded{};
    PFun_slEvaluateFeature* evaluate{};
    PFun_slFreeResources* free_resources{};
    PFun_slSetTagForFrame* set_tag{};
    PFun_slGetFeatureRequirements* requirements{};
    PFun_slSetConstants* set_constants{};
    PFun_slGetFeatureFunction* feature_function{};
    PFun_slGetNewFrameToken* frame_token{};

    PFun_slDLSSGetOptimalSettings* dlss_optimal{};
    PFun_slDLSSSetOptions* dlss_set_options{};
    PFun_slDLSSGGetState* dlssg_state{};
    PFun_slDLSSGSetOptions* dlssg_set_options{};
    PFun_slReflexGetState* reflex_state{};
    PFun_slReflexSleep* reflex_sleep{};
    PFun_slReflexSetOptions* reflex_set_options{};
    PFun_slPCLSetMarker* pcl_marker{};

    bool dlss{}, frame_gen{}, reflex{}, pcl{};
    std::string dlss_problem, frame_gen_problem;
    uint32_t frame_gen_max{1};
    bool shut_down{};

    std::mutex sr_mutex; ///< the recording thread's DLSS calls and the release
    uint32_t sr_frame{};
    bool sr_options{};

    std::atomic<int> reflex_mode{-1};
    std::mutex fg_mutex; ///< swap thread's frame generation calls
    uint64_t fg_last_constants{~0ull};
    bool fg_mode_sent{};
    sl::DLSSGMode fg_mode{sl::DLSSGMode::eOff};
    uint32_t fg_generated{};
};
State g;

bool LoggingVerbose() {
    const char* env = std::getenv("GOW3_SL_LOG");
    return env && env[0] == '2';
}

void LogCallback(sl::LogType type, const char* message) {
    static const bool verbose = LoggingVerbose();
    if (type == sl::LogType::eInfo && !verbose) {
        return;
    }
    std::string text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    std::printf("Streamline: %s%s\n",
                type == sl::LogType::eError ? "error: " : type == sl::LogType::eWarn ? "warning: " : "",
                text.c_str());
}

template <typename T>
bool Proc(T*& out, const char* name) {
    out = reinterpret_cast<T*>(GetProcAddress(g.module, name));
    return out != nullptr;
}

template <typename T>
bool FeatureProc(sl::Feature feature, T*& out, const char* name) {
    void* function{};
    if (g.feature_function(feature, name, function) != sl::Result::eOk) {
        out = nullptr;
        return false;
    }
    out = reinterpret_cast<T*>(function);
    return true;
}

const sl::FrameToken* Token(uint64_t frame) {
    sl::FrameToken* token{};
    const uint32_t index = uint32_t(frame);
    if (g.frame_token(token, &index) != sl::Result::eOk) {
        return nullptr;
    }
    return token;
}

void Fail(const std::string& reason) {
    g.problem = reason;
    std::printf("Streamline: not used (%s); the NGX bridge stays\n", reason.c_str());
}

constexpr UINT NvidiaVendor = 0x10DE;

/// Any NVIDIA adapter in the system (DXGI): AMD and Intel PCs never load Streamline, whose
/// vkCreateDevice would add its plugins' extensions to their devices.
bool NvidiaAdapterPresent() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return true; // cannot tell: the device check in Instance::CreateDevice still applies
    }
    bool found = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 desc{};
        found = SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == NvidiaVendor;
        adapter->Release();
    }
    factory->Release();
    return found;
}

sl::Resource Describe(const Image& image) {
    sl::Resource resource(sl::ResourceType::eTex2d, reinterpret_cast<void*>(image.image), nullptr,
                          reinterpret_cast<void*>(image.view), uint32_t(image.layout));
    resource.width = image.width;
    resource.height = image.height;
    resource.nativeFormat = uint32_t(image.format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = image.usage;
    return resource;
}

sl::float4x4 Matrix(const std::array<float, 16>& m) {
    sl::float4x4 out;
    for (int r = 0; r < 4; ++r) {
        out.row[r] = sl::float4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    }
    return out;
}

sl::float4x4 Identity() {
    sl::float4x4 out;
    for (int r = 0; r < 4; ++r) {
        out.row[r] = sl::float4(r == 0 ? 1.0f : 0.0f, r == 1 ? 1.0f : 0.0f, r == 2 ? 1.0f : 0.0f,
                                r == 3 ? 1.0f : 0.0f);
    }
    return out;
}

constexpr sl::DLSSMode ModeOfIndex[5] = {sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality,
                                          sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance,
                                          sl::DLSSMode::eUltraPerformance};
constexpr const char* ModeNames[5] = {"DLAA", "Quality", "Balanced", "Performance",
                                      "Ultra Performance"};

} // namespace

bool Init() {
    if (g.tried) {
        return g.active;
    }
    g.tried = true;
    if (!Requested()) {
        return false;
    }
    const auto directory = ExecutableDirectory();
    const auto interposer = directory / InterposerName;
    if (!std::filesystem::is_regular_file(interposer)) {
        Fail("sl.interposer.dll is missing");
        return false;
    }
    // DLSS needs the player's nvngx_dlss.dll on either path; without it Streamline has nothing to
    // run but Reflex, which the direct VK_NV_low_latency2 path covers.
    if (!std::filesystem::is_regular_file(directory / L"nvngx_dlss.dll")) {
        Fail("nvngx_dlss.dll is missing");
        return false;
    }
    if (!NvidiaAdapterPresent()) {
        Fail("no NVIDIA graphics card");
        return false;
    }
    // Streamline's integration checklist: load only an NVIDIA-signed interposer (it checks its
    // plugins itself). GOW3_SL_UNSIGNED=1 accepts Streamline's development DLLs (tests only).
    const char* unsigned_ok = std::getenv("GOW3_SL_UNSIGNED");
    if (!(unsigned_ok && unsigned_ok[0] == '1') &&
        !sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        Fail("sl.interposer.dll is not signed by NVIDIA");
        return false;
    }
    g.module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g.module) {
        Fail("sl.interposer.dll did not load (error " + std::to_string(GetLastError()) +
             "; the Visual C++ 2015-2022 runtime may be missing)");
        return false;
    }
    if (!Proc(g.init, "slInit") || !Proc(g.shutdown, "slShutdown") ||
        !Proc(g.is_supported, "slIsFeatureSupported") ||
        !Proc(g.is_loaded, "slIsFeatureLoaded") || !Proc(g.set_loaded, "slSetFeatureLoaded") ||
        !Proc(g.evaluate, "slEvaluateFeature") || !Proc(g.free_resources, "slFreeResources") ||
        !Proc(g.set_tag, "slSetTagForFrame") ||
        !Proc(g.requirements, "slGetFeatureRequirements") ||
        !Proc(g.set_constants, "slSetConstants") ||
        !Proc(g.feature_function, "slGetFeatureFunction") ||
        !Proc(g.frame_token, "slGetNewFrameToken")) {
        Fail("sl.interposer.dll lacks the Streamline 2 API");
        return false;
    }
    g.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g.module, "vkGetInstanceProcAddr"));
    g.gdpa =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(g.module, "vkGetDeviceProcAddr"));
    if (!g.gipa || !g.gdpa) {
        Fail("sl.interposer.dll has no Vulkan entry points");
        return false;
    }

    // Frame generation also needs NVIDIA's nvngx_dlssg.dll; GOW3_FRAME_GEN_EXT=0 leaves it out
    // as it does on the NGX path (the device then gets nothing frame generation asks for).
    std::vector<sl::Feature> features{sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL};
    const char* fg_ext = std::getenv("GOW3_FRAME_GEN_EXT");
    if (std::filesystem::is_regular_file(directory / L"nvngx_dlssg.dll") &&
        std::filesystem::is_regular_file(directory / L"sl.dlss_g.dll") &&
        !(fg_ext && fg_ext[0] == '0')) {
        features.push_back(sl::kFeatureDLSS_G);
    } else {
        g.frame_gen_problem = !std::filesystem::is_regular_file(directory / L"nvngx_dlssg.dll")
                                  ? "nvngx_dlssg.dll is missing"
                              : !std::filesystem::is_regular_file(directory / L"sl.dlss_g.dll")
                                  ? "sl.dlss_g.dll is missing"
                                  : "turned off (GOW3_FRAME_GEN_EXT=0)";
    }

    const std::wstring plugin_path = directory.wstring();
    const wchar_t* plugin_paths[] = {plugin_path.c_str()};
    std::wstring log_path;
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = LoggingVerbose() ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = plugin_paths;
    pref.numPathsToPlugins = 1;
    if (const char* log = std::getenv("GOW3_SL_LOG"); log && log[0] && log[0] != '0') {
        // Streamline's own log file next to the saves and caches (GOW3_GPU_USER_DIR).
        std::filesystem::path data = directory / "streamline";
        if (const char* user = std::getenv("GOW3_GPU_USER_DIR"); user && user[0]) {
            data = std::filesystem::path{user} / "streamline";
        }
        std::error_code error;
        std::filesystem::create_directories(data, error);
        log_path = data.wstring();
        pref.pathToLogsAndData = log_path.c_str();
    }
    pref.logMessageCallback = LogCallback;
    // No over-the-air updates: the plugins and models are the ones shipped with the port.
    // slSetTagForFrame (frame generation's inputs) needs eUseFrameBasedResourceTagging.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features.data();
    pref.numFeaturesToLoad = uint32_t(features.size());
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = EngineVersion;
    pref.projectId = ProjectId;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    if (const sl::Result result = g.init(pref, sl::kSDKVersion); result != sl::Result::eOk) {
        Fail(std::string{"slInit failed: "} + ResultText(result));
        return false;
    }
    g.active = true;
    std::printf("Streamline: SDK %u.%u.%u loaded (%zu features requested)\n", SL_VERSION_MAJOR,
                SL_VERSION_MINOR, SL_VERSION_PATCH, features.size());
    return true;
}

bool Active() {
    return g.active;
}

const char* Problem() {
    return g.problem.c_str();
}

PFN_vkVoidFunction InstanceProc(VkInstance instance, const char* name) {
    return g.gipa ? g.gipa(instance, name) : nullptr;
}

PFN_vkVoidFunction DeviceProc(VkDevice device, const char* name) {
    return g.gdpa ? g.gdpa(device, name) : nullptr;
}

void OnDevice(VkPhysicalDevice physical, VkDevice) {
    if (!g.active) {
        return;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    const auto supported = [&](sl::Feature feature, std::string* problem) {
        bool loaded = false;
        if (g.is_loaded(feature, loaded) != sl::Result::eOk || !loaded) {
            if (problem && problem->empty()) {
                *problem = "plugin not loaded (see the Streamline lines above)";
            }
            return false;
        }
        const sl::Result result = g.is_supported(feature, adapter);
        if (result != sl::Result::eOk) {
            if (problem) {
                *problem = ResultText(result);
            }
            return false;
        }
        return true;
    };
    g.dlss = supported(sl::kFeatureDLSS, &g.dlss_problem) &&
             FeatureProc(sl::kFeatureDLSS, g.dlss_optimal, "slDLSSGetOptimalSettings") &&
             FeatureProc(sl::kFeatureDLSS, g.dlss_set_options, "slDLSSSetOptions");
    g.reflex = supported(sl::kFeatureReflex, nullptr) &&
               FeatureProc(sl::kFeatureReflex, g.reflex_state, "slReflexGetState") &&
               FeatureProc(sl::kFeatureReflex, g.reflex_sleep, "slReflexSleep") &&
               FeatureProc(sl::kFeatureReflex, g.reflex_set_options, "slReflexSetOptions");
    g.pcl = supported(sl::kFeaturePCL, nullptr) &&
            FeatureProc(sl::kFeaturePCL, g.pcl_marker, "slPCLSetMarker");
    if (g.frame_gen_problem.empty()) {
        g.frame_gen = supported(sl::kFeatureDLSS_G, &g.frame_gen_problem) &&
                      FeatureProc(sl::kFeatureDLSS_G, g.dlssg_state, "slDLSSGGetState") &&
                      FeatureProc(sl::kFeatureDLSS_G, g.dlssg_set_options, "slDLSSGSetOptions");
        if (g.frame_gen && !g.reflex) {
            // DLSS-G guide 8.0: frame generation needs sl.reflex.
            g.frame_gen = false;
            g.frame_gen_problem = "sl.reflex is not available";
        }
        if (g.frame_gen) {
            sl::DLSSGState state{};
            if (g.dlssg_state(FrameGenViewport, state, nullptr) == sl::Result::eOk) {
                g.frame_gen_max = std::max(1u, state.numFramesToGenerateMax);
            }
        }
    }
    if (g.reflex) {
        // Reflex guide 4.0: slReflexSetOptions at least once, also with Reflex off.
        SetReflexMode(0);
    }
    if (g.frame_gen && Gow3Settings::Get().frame_gen.load() == 0) {
        // Unloaded before the first swapchain while the setting is off: its hooks would otherwise
        // present every frame from an off-screen copy (DLSS-G guide 18). The presenter loads it
        // again, with a new swapchain, when the setting is switched on.
        SetFrameGenLoaded(false);
    }
    std::printf("Streamline: DLSS %s%s%s, Frame Generation %s%s%s (up to %u generated), Reflex %s, "
                "PC Latency %s\n",
                g.dlss ? "ready" : "unavailable", g.dlss ? "" : " - ", g.dlss_problem.c_str(),
                g.frame_gen ? "ready" : "unavailable", g.frame_gen ? "" : " - ",
                g.frame_gen_problem.c_str(), g.frame_gen_max, g.reflex ? "ready" : "unavailable",
                g.pcl ? "ready" : "unavailable");
}

void Shutdown() {
    if (!g.active || g.shut_down) {
        return;
    }
    g.shut_down = true;
    g.dlss = g.frame_gen = g.reflex = g.pcl = false;
    g.shutdown();
}

void Abandon(const char* reason) {
    if (!g.active) {
        return;
    }
    Shutdown();
    g.active = false;
    g.dlss_problem = g.frame_gen_problem = reason;
    Fail(reason);
}

bool DlssReady() {
    return g.active && g.dlss;
}
bool FrameGenReady() {
    return g.active && g.frame_gen;
}
bool ReflexReady() {
    return g.active && g.reflex && g.pcl;
}
const char* DlssProblem() {
    return g.dlss_problem.c_str();
}
const char* FrameGenProblem() {
    return g.frame_gen_problem.c_str();
}
uint32_t FrameGenMaxGenerated() {
    return g.frame_gen_max;
}

bool SrSetOptions(const SrOptions& options) {
    if (!DlssReady()) {
        return false;
    }
    std::scoped_lock lock{g.sr_mutex};
    sl::DLSSOptions sl_options{};
    sl_options.outputWidth = options.output_width;
    sl_options.outputHeight = options.output_height;
    sl_options.colorBuffersHDR = options.hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    sl_options.useAutoExposure = options.hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    const auto preset = static_cast<sl::DLSSPreset>(options.preset);
    sl_options.dlaaPreset = sl_options.qualityPreset = sl_options.balancedPreset =
        sl_options.performancePreset = sl_options.ultraPerformancePreset =
            sl_options.ultraQualityPreset = preset;
    ModeSizes sizes[5]{};
    for (int m = 0; m < 5; ++m) {
        sl_options.mode = ModeOfIndex[m];
        sl::DLSSOptimalSettings settings{};
        if (g.dlss_optimal(sl_options, settings) == sl::Result::eOk) {
            sizes[m] = {settings.optimalRenderWidth, settings.optimalRenderHeight,
                        settings.renderWidthMin, settings.renderHeightMin};
        }
    }
    const int mode = PickDlssMode(sizes, options.input_width, options.input_height);
    if (mode < 0) {
        std::printf("Streamline: no DLSS mode takes %ux%u -> %ux%u\n", options.input_width,
                    options.input_height, options.output_width, options.output_height);
        return false;
    }
    sl_options.mode = ModeOfIndex[mode];
    if (const sl::Result result = g.dlss_set_options(SrViewport, sl_options);
        result != sl::Result::eOk) {
        std::printf("Streamline: slDLSSSetOptions failed: %s\n", ResultText(result));
        return false;
    }
    g.sr_options = true;
    std::printf("Streamline: DLSS %s for %ux%u -> %ux%u (created at %ux%u), preset %u, HDR %d\n",
                ModeNames[mode], options.input_width, options.input_height, options.output_width,
                options.output_height, sizes[mode].optimal_w, sizes[mode].optimal_h,
                options.preset, int(options.hdr));
    return true;
}

void SrRelease() {
    if (!DlssReady()) {
        return;
    }
    std::scoped_lock lock{g.sr_mutex};
    if (g.sr_options) {
        g.free_resources(sl::kFeatureDLSS, SrViewport);
        g.sr_options = false;
    }
}

bool SrEvaluate(VkCommandBuffer cmd, const SrFrame& frame) {
    if (!DlssReady()) {
        return false;
    }
    std::scoped_lock lock{g.sr_mutex};
    if (!g.sr_options) {
        return false;
    }
    // Super resolution has a frame counter of its own on its own viewport: its frames are the
    // upscales, which the flip ordinals of Reflex and frame generation do not number.
    const sl::FrameToken* token = Token(++g.sr_frame);
    if (!token) {
        return false;
    }
    sl::Constants constants{};
    constants.cameraViewToClip = constants.clipToCameraView = constants.clipToLensClip =
        constants.clipToPrevClip = constants.prevClipToClip = Identity();
    constants.jitterOffset = {frame.jitter_x, frame.jitter_y};
    // The port's motion vectors are in render pixels, current to previous (DLSS's direction).
    constants.mvecScale = {1.0f / float(frame.motion.width), 1.0f / float(frame.motion.height)};
    constants.cameraPinholeOffset = {0.0f, 0.0f};
    constants.cameraPos = {0.0f, 0.0f, 0.0f};
    constants.cameraUp = {0.0f, 1.0f, 0.0f};
    constants.cameraRight = {1.0f, 0.0f, 0.0f};
    constants.cameraFwd = {0.0f, 0.0f, 1.0f};
    constants.cameraNear = 0.1f;
    constants.cameraFar = 3000.0f;
    constants.cameraFOV = 1.0f;
    constants.cameraAspectRatio = float(frame.output.width) / float(frame.output.height);
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = frame.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (const sl::Result result = g.set_constants(constants, *token, SrViewport);
        result != sl::Result::eOk) {
        std::printf("Streamline: slSetConstants (DLSS) failed: %s\n", ResultText(result));
        return false;
    }
    sl::Resource color = Describe(frame.color), depth = Describe(frame.depth),
                 motion = Describe(frame.motion), output = Describe(frame.output),
                 bias = Describe(frame.bias);
    const sl::Extent color_extent{0, 0, frame.color.width, frame.color.height};
    const sl::Extent depth_extent{0, 0, frame.depth.width, frame.depth.height};
    const sl::Extent motion_extent{0, 0, frame.motion.width, frame.motion.height};
    const sl::Extent output_extent{0, 0, frame.output.width, frame.output.height};
    const sl::Extent bias_extent{0, 0, frame.bias.width, frame.bias.height};
    sl::ResourceTag tags[] = {
        sl::ResourceTag{&color, sl::kBufferTypeScalingInputColor,
                        sl::ResourceLifecycle::eValidUntilEvaluate, &color_extent},
        sl::ResourceTag{&output, sl::kBufferTypeScalingOutputColor,
                        sl::ResourceLifecycle::eValidUntilEvaluate, &output_extent},
        sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate,
                        &depth_extent},
        sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors,
                        sl::ResourceLifecycle::eValidUntilEvaluate, &motion_extent},
        sl::ResourceTag{&bias, sl::kBufferTypeBiasCurrentColorHint,
                        sl::ResourceLifecycle::eValidUntilEvaluate, &bias_extent},
    };
    const uint32_t tag_count = frame.bias.image ? 5u : 4u;
    const sl::BaseStructure* inputs[6] = {&SrViewport};
    for (uint32_t i = 0; i < tag_count; ++i) {
        inputs[1 + i] = &tags[i];
    }
    const sl::Result result = g.evaluate(sl::kFeatureDLSS, *token, inputs, 1 + tag_count, cmd);
    if (result != sl::Result::eOk) {
        std::printf("Streamline: DLSS evaluate failed: %s\n", ResultText(result));
        return false;
    }
    return true;
}

void SetMarker(Marker marker, uint64_t frame) {
    if (!g.active || !g.pcl) {
        return;
    }
    if (const sl::FrameToken* token = Token(frame)) {
        g.pcl_marker(static_cast<sl::PCLMarker>(marker), *token);
    }
}

void Sleep(uint64_t frame) {
    if (!g.active || !g.reflex) {
        return;
    }
    if (const sl::FrameToken* token = Token(frame)) {
        g.reflex_sleep(*token);
    }
}

void SetReflexMode(int mode) {
    if (!g.active || !g.reflex || g.reflex_mode.exchange(mode) == mode) {
        return;
    }
    sl::ReflexOptions options{};
    options.mode = mode == 2   ? sl::ReflexMode::eLowLatencyWithBoost
                   : mode == 1 ? sl::ReflexMode::eLowLatency
                               : sl::ReflexMode::eOff;
    if (const sl::Result result = g.reflex_set_options(options); result != sl::Result::eOk) {
        std::printf("Streamline: slReflexSetOptions(mode %d) failed: %s\n", mode,
                    ResultText(result));
    }
}

uint32_t ReflexReports(ReflexReport* out, uint32_t capacity) {
    if (!g.active || !g.reflex) {
        return 0;
    }
    static sl::ReflexState state{}; // 64 reports: too large for the stack of every caller
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    state = sl::ReflexState{};
    if (g.reflex_state(state) != sl::Result::eOk || !state.latencyReportAvailable) {
        return 0;
    }
    uint32_t count = 0;
    for (const auto& r : state.frameReport) {
        if (!r.frameID || count >= capacity) {
            continue;
        }
        out[count++] = {r.frameID,           r.inputSampleTime,   r.simStartTime,
                        r.simEndTime,        r.renderSubmitStartTime, r.renderSubmitEndTime,
                        r.presentStartTime,  r.presentEndTime,    r.driverStartTime,
                        r.driverEndTime,     r.osRenderQueueStartTime, r.osRenderQueueEndTime,
                        r.gpuRenderStartTime, r.gpuRenderEndTime};
    }
    std::sort(out, out + count, [](const auto& a, const auto& b) { return a.frame < b.frame; });
    return count;
}

void SetFrameGenLoaded(bool loaded) {
    if (!FrameGenReady()) {
        return;
    }
    if (const sl::Result result = g.set_loaded(sl::kFeatureDLSS_G, loaded);
        result != sl::Result::eOk) {
        std::printf("Streamline: slSetFeatureLoaded(DLSS-G, %d) failed: %s\n", int(loaded),
                    ResultText(result));
    }
}

bool FrameGenLoaded() {
    if (!FrameGenReady()) {
        return false;
    }
    bool loaded = false;
    return g.is_loaded(sl::kFeatureDLSS_G, loaded) == sl::Result::eOk && loaded;
}

bool FrameGenSetFrame(uint64_t frame, const FgFrame* inputs) {
    if (!FrameGenReady()) {
        return false;
    }
    std::scoped_lock lock{g.fg_mutex};
    const sl::FrameToken* token = Token(frame);
    if (!token) {
        return false;
    }
    if (!inputs) {
        // DLSS-G guide 5.2: tags null while depth and motion vectors may be invalid.
        sl::ResourceTag tags[] = {
            sl::ResourceTag{nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent},
        };
        g.set_tag(*token, FrameGenViewport, tags, 2, nullptr);
        return true;
    }
    // One set of constants per frame (DLSS-G guide 17.0); a repeated frame keeps the first.
    if (g.fg_last_constants != frame) {
        g.fg_last_constants = frame;
        const auto& c = inputs->camera;
        sl::Constants constants{};
        constants.cameraViewToClip = Matrix(c.view_to_clip);
        constants.clipToCameraView = Matrix(c.clip_to_view);
        constants.clipToLensClip = Identity();
        constants.clipToPrevClip = Matrix(c.clip_to_prev_clip);
        constants.prevClipToClip = Matrix(c.prev_clip_to_clip);
        constants.jitterOffset = {inputs->jitter[0], inputs->jitter[1]};
        constants.mvecScale = {1.0f / float(inputs->motion.width),
                               1.0f / float(inputs->motion.height)};
        constants.cameraPinholeOffset = {0.0f, 0.0f};
        constants.cameraPos = {c.pos[0], c.pos[1], c.pos[2]};
        constants.cameraUp = {c.up[0], c.up[1], c.up[2]};
        constants.cameraRight = {c.right[0], c.right[1], c.right[2]};
        constants.cameraFwd = {c.fwd[0], c.fwd[1], c.fwd[2]};
        constants.cameraNear = c.near_z;
        constants.cameraFar = c.far_z;
        constants.cameraFOV = c.fov_y;
        constants.cameraAspectRatio = c.aspect;
        // frame_gen_camera.h: depth 0 near .. 1 far, motion vectors with the camera's motion.
        constants.depthInverted = sl::Boolean::eFalse;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        constants.reset = inputs->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        if (const sl::Result result = g.set_constants(constants, *token, FrameGenViewport);
            result != sl::Result::eOk) {
            std::printf("Streamline: slSetConstants (frame generation) failed: %s\n",
                        ResultText(result));
            return false;
        }
    }
    sl::Resource depth = Describe(inputs->depth), motion = Describe(inputs->motion);
    const sl::Extent depth_extent{0, 0, inputs->depth.width, inputs->depth.height};
    const sl::Extent motion_extent{0, 0, inputs->motion.width, inputs->motion.height};
    const sl::Extent backbuffer{inputs->sub_y, inputs->sub_x, inputs->sub_w, inputs->sub_h}; // top, left
    sl::ResourceTag tags[] = {
        sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                        &depth_extent},
        sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors,
                        sl::ResourceLifecycle::eValidUntilPresent, &motion_extent},
        sl::ResourceTag{nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent,
                        &backbuffer},
    };
    // The backbuffer is tagged only for a letterbox (DLSS-G guide 5.2: its extent is the point;
    // without one, 23 Oct 2026, Streamline warns "Invalid backbuffer resource extent").
    const uint32_t tag_count = inputs->sub_w ? 3u : 2u;
    if (const sl::Result result = g.set_tag(*token, FrameGenViewport, tags, tag_count, nullptr);
        result != sl::Result::eOk) {
        std::printf("Streamline: slSetTagForFrame (frame generation) failed: %s\n",
                    ResultText(result));
        return false;
    }
    return true;
}

void FrameGenSetMode(bool on, uint32_t generated) {
    if (!FrameGenReady()) {
        return;
    }
    std::scoped_lock lock{g.fg_mutex};
    const sl::DLSSGMode mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    generated = std::clamp(generated, 1u, g.frame_gen_max);
    if (g.fg_mode_sent && mode == g.fg_mode && generated == g.fg_generated) {
        return;
    }
    sl::DLSSGOptions options{};
    options.mode = mode;
    options.numFramesToGenerate = generated;
    // DLSS-G guide 6.4: off keeps its resources (menus, loading screens come back quickly).
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (const sl::Result result = g.dlssg_set_options(FrameGenViewport, options);
        result != sl::Result::eOk) {
        std::printf("Streamline: slDLSSGSetOptions failed: %s\n", ResultText(result));
        return;
    }
    g.fg_mode_sent = true;
    g.fg_mode = mode;
    g.fg_generated = generated;
}

FgState FrameGenGetState() {
    if (!FrameGenReady()) {
        return {};
    }
    sl::DLSSGState state{};
    if (g.dlssg_state(FrameGenViewport, state, nullptr) != sl::Result::eOk) {
        return {};
    }
    return {state.status == sl::DLSSGStatus::eOk, uint32_t(state.status),
            state.numFramesActuallyPresented, state.numFramesToGenerateMax};
}

bool WindowFocused(void* window) {
    return GetForegroundWindow() == static_cast<HWND>(window);
}

#else

bool Init() {
    return false;
}
bool Active() {
    return false;
}
const char* Problem() {
    return "";
}
PFN_vkVoidFunction InstanceProc(VkInstance, const char*) {
    return nullptr;
}
PFN_vkVoidFunction DeviceProc(VkDevice, const char*) {
    return nullptr;
}
void OnDevice(VkPhysicalDevice, VkDevice) {}
void Shutdown() {}
bool DlssReady() {
    return false;
}
bool FrameGenReady() {
    return false;
}
bool ReflexReady() {
    return false;
}
const char* DlssProblem() {
    return "";
}
const char* FrameGenProblem() {
    return "";
}
uint32_t FrameGenMaxGenerated() {
    return 1;
}
bool SrSetOptions(const SrOptions&) {
    return false;
}
void SrRelease() {}
bool SrEvaluate(VkCommandBuffer, const SrFrame&) {
    return false;
}
void SetMarker(Marker, uint64_t) {}
void Sleep(uint64_t) {}
void SetReflexMode(int) {}
uint32_t ReflexReports(ReflexReport*, uint32_t) {
    return 0;
}
void SetFrameGenLoaded(bool) {}
bool FrameGenLoaded() {
    return false;
}
bool FrameGenSetFrame(uint64_t, const FgFrame*) {
    return false;
}
void FrameGenSetMode(bool, uint32_t) {}
FgState FrameGenGetState() {
    return {};
}
void Abandon(const char*) {}
bool WindowFocused(void*) {
    return true;
}

#endif

} // namespace Gow3Streamline
