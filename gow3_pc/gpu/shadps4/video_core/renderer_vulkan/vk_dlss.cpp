// SPDX-FileCopyrightText: Copyright 2026 IFreemz, gow3 contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include "video_core/renderer_vulkan/vk_dlss.h"
#include "gow3_settings.h"
#include "gow3_streamline.h"
#include "gow3_toggles.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../dlss_bridge/gow3_dlss_bridge.h"
#endif

namespace Vulkan {

u32 Dlss::TransformerPreset(int model) {
    // A transformer model by name (NVSDK_NGX_DLSS_Hint_Render_Preset J=10, K=11, L=12, M=13),
    // so a driver or OTA update cannot swap the model (Programming Guide 3.12.1). The setting
    // (dlss_model: launcher, menu, GOW3_DLSS_MODEL) defaults to K for every mode: the guide's
    // default for DLAA/Quality/Balanced and its cheapest transformer. The guide's Performance
    // default M cost 2.80 ms per 4K frame on an RTX 5070 against K's 1.97 ms (17 Oct profile;
    // guide table 2.98 / 2.11 ms), more than the reduced Uncharted 2 scene saves.
    return 10 + u32(std::clamp(model, 0, 3));
}

bool Dlss::FrameGenWanted() {
    if (!Gow3Settings::Get().frame_gen.load(std::memory_order_relaxed) ||
        Gow3Toggle::Disabled2(Gow3Toggle::FrameGeneration)) {
        return false;
    }
    const Dlss* dlss = Get();
    return dlss && dlss->FrameGenAvailable();
}

int Dlss::QualityForScale(float scale) {
    // DLAA only for equal sizes: any real upscale needs at least the Quality mode.
    return scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2 : scale > 1.01f ? 1 : 0;
}

#ifdef _WIN32

namespace {
constexpr wchar_t BridgeName[] = L"gow3_dlss.dll";
constexpr wchar_t NgxName[] = L"nvngx_dlss.dll";
constexpr wchar_t NgxFrameGenName[] = L"nvngx_dlssg.dll";

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

void BridgeLog(int warning, const char* message) {
    std::printf("DLSS: %s%s\n", warning ? "warning: " : "", message);
}

bool CollectExtensions(const VkExtensionProperties* required, u32 count,
                       const std::vector<vk::ExtensionProperties>& available,
                       std::vector<std::string>& storage, std::vector<const char*>& enabled) {
    storage.clear();
    for (u32 i = 0; i < count; ++i) {
        const auto& request = required[i];
        const auto match = std::ranges::find_if(available, [&](const auto& extension) {
            return std::strcmp(extension.extensionName.data(), request.extensionName) == 0 &&
                   extension.specVersion >= request.specVersion;
        });
        if (match == available.end()) {
            std::printf("DLSS: missing Vulkan extension %s\n", request.extensionName);
            return false;
        }
        storage.emplace_back(request.extensionName);
    }
    for (const auto& name : storage) {
        if (std::ranges::none_of(enabled, [&](const char* current) { return name == current; })) {
            enabled.push_back(name.c_str());
        }
    }
    return true;
}
} // namespace

struct Dlss::Impl {
    /// gow3 23 Oct 2026: Streamline loaded (gow3_streamline.h): every call goes there
    /// and the NGX bridge is not loaded.
    bool streamline{};
    HMODULE module{};
    const Gow3DlssApi* api{};
    std::vector<std::string> instance_extensions, device_extensions;
    std::optional<FeatureDesc> feature;
    std::string problem;
    bool eligible{true}, instance_ready{}, device_ready{}, initialized{}, available{};
    // Frame generation: set up beside super resolution, off on its own when anything is missing.
    std::vector<std::string> frame_gen_instance_extensions, frame_gen_device_extensions;
    std::string frame_gen_problem;
    bool frame_gen_eligible{}, frame_gen_instance_ready{}, frame_gen_device_ready{},
        frame_gen_available{};
    /// The presenter (swap thread) uses the feature, and ReleaseFeature (draw thread) drops it.
    mutable std::mutex frame_gen_mutex;
    std::optional<FrameGenDesc> frame_gen_feature;

    void Disable(std::string_view reason) {
        eligible = available = false;
        problem = reason;
        std::printf("DLSS: unavailable (%.*s)\n", int(reason.size()), reason.data());
    }
    void DisableFrameGen(std::string_view reason) {
        frame_gen_eligible = frame_gen_available = false;
        frame_gen_problem = reason;
        std::printf("DLSS: Frame Generation unavailable (%.*s)\n", int(reason.size()),
                    reason.data());
    }
};

Dlss* Dlss::Get() {
    static Dlss* const dlss = []() -> Dlss* {
        const char* setting = std::getenv("GOW3_DLSS");
        if (setting && setting[0] == '0') {
            return nullptr;
        }
        const auto directory = ExecutableDirectory();
        // Streamline needs nvngx_dlss.dll beside its plugins too, but not the bridge.
        const bool streamline = Gow3Streamline::Requested() && Gow3Streamline::Init();
        if ((!streamline && !std::filesystem::is_regular_file(directory / BridgeName)) ||
            !std::filesystem::is_regular_file(directory / NgxName)) {
            return nullptr;
        }
        auto* created = new Dlss;
        // The process may end through quick_exit/_exit paths: release NGX first when it can.
        std::atexit([] {
            if (Dlss* live = Get()) {
                live->Shutdown();
            }
        });
        return created;
    }();
    return dlss;
}

Dlss::Dlss() : impl{std::make_unique<Impl>()} {
    if (Gow3Streamline::Active()) {
        impl->streamline = true;
        return; // Initialize asks Streamline what runs here
    }
    const auto directory = ExecutableDirectory();
    impl->module = LoadLibraryW((directory / BridgeName).c_str());
    const auto get_api =
        impl->module ? reinterpret_cast<Gow3DlssGetApiFn>(GetProcAddress(impl->module, "Gow3DlssGetApi"))
                     : nullptr;
    impl->api = get_api ? get_api() : nullptr;
    if (!impl->api || impl->api->abi != GOW3_DLSS_BRIDGE_ABI) {
        impl->Disable("gow3_dlss.dll is missing or from another version");
        return;
    }
    // NGX writes its logs and model updates here: beside the saves and shader caches.
    std::filesystem::path data = directory / "dlss";
    if (const char* user = std::getenv("GOW3_GPU_USER_DIR"); user && user[0]) {
        data = std::filesystem::path{user} / "dlss";
    }
    std::error_code error;
    std::filesystem::create_directories(data, error);
    if (!impl->api->Configure(directory.c_str(), data.c_str(), BridgeLog)) {
        impl->Disable("bridge configuration failed");
        return;
    }
    // 22 Oct 2026: without nvngx_dlssg.dll NGX's frame generation queries fail anyway (bridge,
    // 0xbad00012); skipping them keeps the device exactly as it was without frame generation.
    // GOW3_FRAME_GEN_EXT=0 does the same with the DLL present (A/B of the extensions alone).
    const char* extensions = std::getenv("GOW3_FRAME_GEN_EXT");
    if (!std::filesystem::is_regular_file(directory / NgxFrameGenName)) {
        impl->frame_gen_problem = "nvngx_dlssg.dll is missing";
    } else if (extensions && extensions[0] == '0') {
        impl->frame_gen_problem = "turned off (GOW3_FRAME_GEN_EXT=0)";
    } else {
        impl->frame_gen_eligible = true;
    }
}

Dlss::~Dlss() {
    Shutdown();
}

void Dlss::AppendInstanceExtensions(std::vector<const char*>& enabled) {
    if (!impl->eligible || impl->streamline) {
        return; // Streamline's vkCreateInstance adds its own
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->InstanceExtensions(&count, &required)) {
        return impl->Disable("instance extension query failed");
    }
    const auto [result, extensions] = vk::enumerateInstanceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->instance_extensions, enabled)) {
        return impl->Disable("required Vulkan instance extensions are missing");
    }
    impl->instance_ready = true;
    if (!impl->frame_gen_eligible) {
        return;
    }
    // After super resolution's names were copied: NGX may hand back the same array.
    if (!impl->api->FrameGenInstanceExtensions(&count, &required)) {
        return impl->DisableFrameGen("instance extension query failed");
    }
    if (!CollectExtensions(required, count, extensions, impl->frame_gen_instance_extensions,
                           enabled)) {
        return impl->DisableFrameGen("required Vulkan instance extensions are missing");
    }
    impl->frame_gen_instance_ready = true;
}

void Dlss::AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                  std::vector<const char*>& enabled) {
    if (!impl->eligible || !impl->instance_ready || impl->streamline) {
        return;
    }
    const auto properties = physical.getProperties();
    if (properties.vendorID != 0x10de) {
        return impl->Disable("not an NVIDIA GPU");
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->DeviceExtensions(instance, physical, &count, &required)) {
        return impl->Disable("this GPU or driver does not support DLSS (GeForce RTX needed)");
    }
    const auto [result, extensions] = physical.enumerateDeviceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->device_extensions, enabled)) {
        return impl->Disable("required Vulkan device extensions are missing");
    }
    impl->device_ready = true;
    if (!impl->frame_gen_eligible || !impl->frame_gen_instance_ready) {
        return;
    }
    if (!impl->api->FrameGenDeviceExtensions(instance, physical, &count, &required)) {
        return impl->DisableFrameGen("this GPU or driver does not support it (RTX 40 or newer)");
    }
    if (!CollectExtensions(required, count, extensions, impl->frame_gen_device_extensions,
                           enabled)) {
        return impl->DisableFrameGen("required Vulkan device extensions are missing");
    }
    impl->frame_gen_device_ready = true;
}

void Dlss::Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device) {
    if (impl->streamline) {
        if (impl->initialized) {
            return;
        }
        impl->initialized = true;
        impl->available = Gow3Streamline::DlssReady();
        impl->problem = impl->available ? "" : Gow3Streamline::DlssProblem();
        impl->frame_gen_available = Gow3Streamline::FrameGenReady();
        impl->frame_gen_problem = impl->frame_gen_available ? "" : Gow3Streamline::FrameGenProblem();
        std::printf("DLSS: through Streamline (%s; Frame Generation %s)\n",
                    impl->available ? "ready" : impl->problem.c_str(),
                    impl->frame_gen_available ? "ready" : impl->frame_gen_problem.c_str());
        return;
    }
    if (!impl->eligible || !impl->device_ready || impl->initialized) {
        return;
    }
    impl->initialized = true;
    if (!impl->api->Initialize(instance, physical, device,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr)) {
        return impl->Disable("NVIDIA NGX initialization failed (update the GPU driver)");
    }
    impl->available = true;
    std::printf("DLSS: ready\n");
    if (!impl->frame_gen_device_ready) {
        return;
    }
    u32 max_multi_frame{};
    if (!impl->api->FrameGenSupport(&max_multi_frame)) {
        return impl->DisableFrameGen("not supported on this GPU or driver (RTX 40 or newer)");
    }
    impl->frame_gen_available = true;
    std::printf("DLSS: Frame Generation ready (up to %u generated frames per real one; 1 used)\n",
                max_multi_frame);
}

bool Dlss::FrameGenAvailable() const {
    return impl->available && impl->frame_gen_available;
}

std::string Dlss::FrameGenProblem() const {
    return !impl->available && !impl->problem.empty() ? impl->problem : impl->frame_gen_problem;
}

bool Dlss::HasFrameGenFeature(const FrameGenDesc& desc) const {
    std::scoped_lock lock{impl->frame_gen_mutex};
    return impl->frame_gen_feature && *impl->frame_gen_feature == desc;
}

bool Dlss::FrameGenCreate(vk::CommandBuffer command, const FrameGenDesc& desc) {
    if (!FrameGenAvailable() || impl->streamline) {
        return false; // Streamline: the presenter's own path (Presenter::PresentStreamlineFrameGen)
    }
    std::scoped_lock lock{impl->frame_gen_mutex};
    impl->frame_gen_feature.reset();
    const Gow3FrameGenFeature feature{desc.width, desc.height, desc.render_width,
                                      desc.render_height, u32(VkFormat(desc.format))};
    if (!impl->api->FrameGenCreate(command, &feature)) {
        return false;
    }
    impl->frame_gen_feature = desc;
    return true;
}

bool Dlss::FrameGenEvaluate(vk::CommandBuffer command, const FrameGenFrame& frame) {
    std::scoped_lock lock{impl->frame_gen_mutex};
    if (!impl->frame_gen_feature || impl->streamline) {
        return false; // dropped by FrameGenRelease: the presenter creates it again
    }
    const auto image = [](const Resource& r) {
        const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
        return Gow3DlssImage{r.image,
                             r.view,
                             static_cast<VkImageSubresourceRange>(range),
                             static_cast<VkFormat>(r.format),
                             r.width,
                             r.height};
    };
    Gow3FrameGenEvaluate parameters{};
    parameters.backbuffer = image(frame.backbuffer);
    parameters.depth = image(frame.depth);
    parameters.motion = image(frame.motion);
    parameters.output = image(frame.output);
    const auto& camera = frame.camera;
    std::ranges::copy(camera.view_to_clip, parameters.camera_view_to_clip);
    std::ranges::copy(camera.clip_to_view, parameters.clip_to_camera_view);
    std::ranges::copy(camera.clip_to_prev_clip, parameters.clip_to_prev_clip);
    std::ranges::copy(camera.prev_clip_to_clip, parameters.prev_clip_to_clip);
    std::ranges::copy(camera.pos, parameters.camera_pos);
    std::ranges::copy(camera.up, parameters.camera_up);
    std::ranges::copy(camera.right, parameters.camera_right);
    std::ranges::copy(camera.fwd, parameters.camera_fwd);
    parameters.camera_near = camera.near_z;
    parameters.camera_far = camera.far_z;
    parameters.camera_fov = camera.fov_y;
    parameters.camera_aspect = camera.aspect;
    // The port's motion vectors are what NGX asks for already: render pixels, previous minus
    // current (frame_gen_camera.h; DLSS super resolution gets them with scale 1 too).
    parameters.mvec_scale[0] = parameters.mvec_scale[1] = 1.0f;
    parameters.jitter[0] = frame.jitter_x;
    parameters.jitter[1] = frame.jitter_y;
    parameters.multi_frame_count = 1;
    parameters.multi_frame_index = 1;
    parameters.reset = frame.reset ? 1 : 0;
    parameters.depth_inverted = 0; // 0 near .. 1 far (frame_gen_camera.h, camera_motion.comp)
    parameters.camera_motion_included = 1;
    parameters.hdr = frame.hdr ? 1 : 0;
    return impl->api->FrameGenEvaluate(command, &parameters) != 0;
}

void Dlss::FrameGenRelease() {
    std::scoped_lock lock{impl->frame_gen_mutex};
    impl->frame_gen_feature.reset();
    if (impl->api && impl->initialized) {
        impl->api->FrameGenRelease();
    }
}

bool Dlss::Available() const {
    return impl->available;
}

std::string Dlss::Problem() const {
    return impl->problem;
}

bool Dlss::HasFeature(const FeatureDesc& desc) const {
    return impl->feature && *impl->feature == desc;
}

bool Dlss::CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc) {
    if (!impl->available) {
        return false;
    }
    impl->feature.reset();
    if (impl->streamline) {
        // Streamline creates the NGX feature at the first evaluate with these options.
        if (!Gow3Streamline::SrSetOptions({desc.input_width, desc.input_height, desc.output_width,
                                           desc.output_height, desc.preset, desc.hdr})) {
            return false;
        }
        impl->feature = desc;
        return true;
    }
    const Gow3DlssFeature feature{desc.input_width,  desc.input_height, desc.output_width,
                                desc.output_height, desc.quality,      0,
                                desc.preset, desc.hdr ? 1 : 0};
    if (!impl->api->CreateFeature(command, &feature)) {
        return false;
    }
    impl->feature = desc;
    return true;
}

bool Dlss::Evaluate(vk::CommandBuffer command, const Frame& frame) {
    if (!impl->available || !impl->feature) {
        return false;
    }
    if (impl->streamline) {
        // The upscaler keeps every image in the General layout (RecordDlss callers).
        const auto image = [](const Resource& r, VkImageUsageFlags usage) {
            return Gow3Streamline::Image{static_cast<VkImage>(r.image),
                                         static_cast<VkImageView>(r.view),
                                         static_cast<VkFormat>(r.format),
                                         r.width,
                                         r.height,
                                         usage,
                                         VK_IMAGE_LAYOUT_GENERAL};
        };
        constexpr VkImageUsageFlags read = VK_IMAGE_USAGE_SAMPLED_BIT;
        return Gow3Streamline::SrEvaluate(
            command, {image(frame.color, read), image(frame.depth, read), image(frame.motion, read),
                      image(frame.output, read | VK_IMAGE_USAGE_STORAGE_BIT),
                      frame.bias_mask.image ? image(frame.bias_mask, read) : Gow3Streamline::Image{},
                      frame.jitter_x, frame.jitter_y, frame.reset});
    }
    const auto image = [](const Resource& r) {
        const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
        return Gow3DlssImage{r.image,
                           r.view,
                           static_cast<VkImageSubresourceRange>(range),
                           static_cast<VkFormat>(r.format),
                           r.width,
                           r.height};
    };
    const Gow3DlssEvaluate parameters{image(frame.color),  image(frame.depth), image(frame.motion),
                                    image(frame.output), frame.jitter_x,     frame.jitter_y,
                                    frame.reset ? 1 : 0, frame.frame_ms,     frame.sharpness,
                                    frame.bias_mask.image ? image(frame.bias_mask) : Gow3DlssImage{}};
    return impl->api->Evaluate(command, &parameters) != 0;
}

void Dlss::ReleaseFeature() {
    impl->feature.reset();
    if (impl->streamline) {
        Gow3Streamline::SrRelease();
        return;
    }
    // 22 Oct review: frame generation state stays; the swap thread may still use that feature.
    if (impl->api) {
        impl->api->ReleaseFeature();
    }
}

bool Dlss::ViaStreamline() const {
    return impl->streamline;
}

void Dlss::Shutdown() {
    if (impl->streamline) {
        impl->available = impl->frame_gen_available = false;
        impl->feature.reset();
        Gow3Streamline::Shutdown();
        return;
    }
    impl->available = false;
    impl->feature.reset();
    {
        std::scoped_lock lock{impl->frame_gen_mutex};
        impl->frame_gen_feature.reset();
    }
    if (impl->api && impl->initialized) {
        impl->api->Shutdown();
        impl->initialized = false;
    }
}

#else

struct Dlss::Impl {};
Dlss::Dlss() = default;
Dlss::~Dlss() = default;
Dlss* Dlss::Get() {
    return nullptr;
}
void Dlss::AppendInstanceExtensions(std::vector<const char*>&) {}
void Dlss::AppendDeviceExtensions(vk::Instance, vk::PhysicalDevice, std::vector<const char*>&) {}
void Dlss::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void Dlss::Shutdown() {}
bool Dlss::Available() const {
    return false;
}
std::string Dlss::Problem() const {
    return {};
}
bool Dlss::HasFeature(const FeatureDesc&) const {
    return false;
}
bool Dlss::CreateFeature(vk::CommandBuffer, const FeatureDesc&) {
    return false;
}
bool Dlss::Evaluate(vk::CommandBuffer, const Frame&) {
    return false;
}
void Dlss::ReleaseFeature() {}
bool Dlss::ViaStreamline() const {
    return false;
}
bool Dlss::FrameGenAvailable() const {
    return false;
}
std::string Dlss::FrameGenProblem() const {
    return {};
}
bool Dlss::HasFrameGenFeature(const FrameGenDesc&) const {
    return false;
}
bool Dlss::FrameGenCreate(vk::CommandBuffer, const FrameGenDesc&) {
    return false;
}
bool Dlss::FrameGenEvaluate(vk::CommandBuffer, const FrameGenFrame&) {
    return false;
}
void Dlss::FrameGenRelease() {}

#endif

} // namespace Vulkan
