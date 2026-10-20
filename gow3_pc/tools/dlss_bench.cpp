// dlss-bench: NGX's own GPU time for one DLSS configuration, without the game.
//
// Loads gow3_dlss.dll + nvngx_dlss.dll from --dir (default: the exe's directory, then out/),
// creates a feature like the port's RecordDlss and times Evaluate with GPU timestamps. A
// configuration takes seconds instead of a game boot, so feature flags, formats and queue
// placement can be compared before touching the renderer (20 Oct: model M at 4K DLAA).
//
//   dlss-bench [--in 3840x2160] [--out 3840x2160] [--model m] [--hdr 1] [--frames 300]
//              [--color rgba16f|r11g11b10|rgba8] [--bias 1] [--queue graphics|compute]
//              [--load N] [--game-images 1] [--quality 0-4] [--dir <out folder>]
//   --load N: N 4K RGBA16F blits on the graphics queue each frame (with --queue compute they
//   run alongside DLSS); --game-images 1: colour/depth with the texture cache's flags.
//   Run it under the CLANG64 PATH (libc++.dll).
//
// Prints the median, p10 and p90 of the evaluation time and of the frame (load + DLSS).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>

#include "../gpu/dlss_bridge/gow3_dlss_bridge.h"

namespace {

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::fprintf(stderr, "%s failed: %d (line %d)\n", #x, int(r_), __LINE__);              \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

struct Options {
    uint32_t in_w = 3840, in_h = 2160, out_w = 3840, out_h = 2160;
    int model = 3; // m
    int hdr = 1;
    int frames = 300;
    int bias = 1;
    int quality = -1; // from the scale, like Dlss::QualityForScale
    bool compute_queue = false;
    int load = 0;
    int game_images = 0; // colour and depth created like the texture cache's (image.cpp)
    VkFormat color = VK_FORMAT_R16G16B16A16_SFLOAT;
    std::wstring dir;
};

bool ParseSize(const char* text, uint32_t& w, uint32_t& h) {
    return std::sscanf(text, "%ux%u", &w, &h) == 2;
}

Options Parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const char* value = argv[i + 1];
        if (key == "--in") ParseSize(value, o.in_w, o.in_h);
        else if (key == "--out") ParseSize(value, o.out_w, o.out_h);
        else if (key == "--model") o.model = std::string("jklm").find(value[0]);
        else if (key == "--hdr") o.hdr = std::atoi(value);
        else if (key == "--frames") o.frames = std::atoi(value);
        else if (key == "--bias") o.bias = std::atoi(value);
        else if (key == "--quality") o.quality = std::atoi(value);
        else if (key == "--queue") o.compute_queue = std::string(value) == "compute";
        else if (key == "--load") o.load = std::atoi(value);
        else if (key == "--game-images") o.game_images = std::atoi(value);
        else if (key == "--dir") o.dir = std::filesystem::path(value).wstring();
        else if (key == "--color") {
            const std::string f = value;
            o.color = f == "r11g11b10" ? VK_FORMAT_B10G11R11_UFLOAT_PACK32
                      : f == "rgba8"   ? VK_FORMAT_R8G8B8A8_UNORM
                                       : VK_FORMAT_R16G16B16A16_SFLOAT;
        } else {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
            std::exit(2);
        }
    }
    return o;
}

struct Image {
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    VkFormat format{};
    VkImageAspectFlags aspect{};
    uint32_t w{}, h{};
};

struct Gpu {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    uint32_t graphics_family{}, compute_family{};
    VkQueue graphics{}, compute{};
    VkPhysicalDeviceMemoryProperties memory{};
    float timestamp_ns{};
};

uint32_t MemoryType(const Gpu& gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < gpu.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (gpu.memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    std::fprintf(stderr, "no memory type\n");
    std::exit(1);
}

// Concurrent sharing only for --queue compute (it can cost the driver its compression).
bool g_concurrent = false;

Image MakeImage(const Gpu& gpu, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                VkImageAspectFlags aspect, VkImageCreateFlags flags = 0) {
    Image img{.format = format, .aspect = aspect, .w = w, .h = h};
    const uint32_t families[2] = {gpu.graphics_family, gpu.compute_family};
    const bool shared = g_concurrent && gpu.graphics_family != gpu.compute_family;
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = flags,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = shared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = shared ? 2u : 0u,
        .pQueueFamilyIndices = shared ? families : nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(gpu.device, &info, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gpu.device, img.image, &req);
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = MemoryType(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    CHECK(vkAllocateMemory(gpu.device, &alloc, nullptr, &img.memory));
    CHECK(vkBindImageMemory(gpu.device, img.image, img.memory, 0));
    const VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    CHECK(vkCreateImageView(gpu.device, &view, nullptr, &img.view));
    return img;
}

Gow3DlssImage Wrap(const Image& img) {
    return {img.image, img.view, {img.aspect, 0, 1, 0, 1}, img.format, img.w, img.h};
}

void Barrier(VkCommandBuffer cmd, const Image& img, VkImageLayout from, VkImageLayout to) {
    const VkImageMemoryBarrier2 b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img.image,
        .subresourceRange = {img.aspect, 0, 1, 0, 1},
    };
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd, &dep);
}

void BridgeLog(int warning, const char* message) {
    std::printf("%s%s\n", warning ? "warning: " : "", message);
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))];
}

} // namespace

int main(int argc, char** argv) {
    Options o = Parse(argc, argv);
    std::filesystem::path dir = o.dir;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        dir = std::filesystem::path(exe).parent_path();
        if (!std::filesystem::exists(dir / L"gow3_dlss.dll")) dir = dir.parent_path();
    }
    HMODULE bridge = LoadLibraryW((dir / L"gow3_dlss.dll").c_str());
    if (!bridge) {
        std::fprintf(stderr, "gow3_dlss.dll not found in %ls\n", dir.c_str());
        return 1;
    }
    const auto get_api =
        reinterpret_cast<Gow3DlssGetApiFn>(GetProcAddress(bridge, "Gow3DlssGetApi"));
    const Gow3DlssApi* api = get_api ? get_api() : nullptr;
    if (!api || api->abi != GOW3_DLSS_BRIDGE_ABI) {
        std::fprintf(stderr, "bridge ABI mismatch\n");
        return 1;
    }
    const std::filesystem::path data = std::filesystem::temp_directory_path() / L"dlss-bench";
    std::filesystem::create_directories(data);
    api->Configure(dir.c_str(), data.c_str(), BridgeLog);

    Gpu gpu;
    // Instance with NGX's extensions.
    uint32_t ngx_count = 0;
    const VkExtensionProperties* ngx_ext = nullptr;
    std::vector<const char*> inst_ext;
    if (api->InstanceExtensions(&ngx_count, &ngx_ext))
        for (uint32_t i = 0; i < ngx_count; ++i) inst_ext.push_back(ngx_ext[i].extensionName);
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "dlss-bench",
                                .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo inst{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                    .pApplicationInfo = &app,
                                    .enabledExtensionCount = uint32_t(inst_ext.size()),
                                    .ppEnabledExtensionNames = inst_ext.data()};
    CHECK(vkCreateInstance(&inst, nullptr, &gpu.instance));
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data());
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.vendorID == 0x10de && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            gpu.physical = d;
            gpu.timestamp_ns = p.limits.timestampPeriod;
            std::printf("GPU: %s\n", p.deviceName);
        }
    }
    if (!gpu.physical) {
        std::fprintf(stderr, "no NVIDIA GPU\n");
        return 1;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);

    std::vector<const char*> dev_ext;
    if (api->DeviceExtensions(gpu.instance, gpu.physical, &ngx_count, &ngx_ext))
        for (uint32_t i = 0; i < ngx_count; ++i) dev_ext.push_back(ngx_ext[i].extensionName);
    else {
        std::fprintf(stderr, "NGX device extensions unavailable\n");
        return 1;
    }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &family_count, families.data());
    gpu.graphics_family = gpu.compute_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
        const auto f = families[i].queueFlags;
        if ((f & VK_QUEUE_GRAPHICS_BIT) && gpu.graphics_family == UINT32_MAX) gpu.graphics_family = i;
        if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) &&
            gpu.compute_family == UINT32_MAX)
            gpu.compute_family = i;
    }
    if (gpu.compute_family == UINT32_MAX) gpu.compute_family = gpu.graphics_family;
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queues{
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = gpu.graphics_family,
         .queueCount = 1,
         .pQueuePriorities = &priority}};
    if (gpu.compute_family != gpu.graphics_family)
        queues.push_back({.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                          .queueFamilyIndex = gpu.compute_family,
                          .queueCount = 1,
                          .pQueuePriorities = &priority});

    // Every core feature the GPU has, like the port's device (NGX needs some of them).
    VkPhysicalDeviceVulkan13Features f13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                         .pNext = &f12};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(gpu.physical, &features);
    const VkDeviceCreateInfo dev{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .pNext = &features,
                                 .queueCreateInfoCount = uint32_t(queues.size()),
                                 .pQueueCreateInfos = queues.data(),
                                 .enabledExtensionCount = uint32_t(dev_ext.size()),
                                 .ppEnabledExtensionNames = dev_ext.data()};
    CHECK(vkCreateDevice(gpu.physical, &dev, nullptr, &gpu.device));
    vkGetDeviceQueue(gpu.device, gpu.graphics_family, 0, &gpu.graphics);
    vkGetDeviceQueue(gpu.device, gpu.compute_family, 0, &gpu.compute);
    if (!api->Initialize(gpu.instance, gpu.physical, gpu.device, vkGetInstanceProcAddr,
                         vkGetDeviceProcAddr)) {
        std::fprintf(stderr, "NGX initialization failed\n");
        return 1;
    }

    const VkImageUsageFlags sampled =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    g_concurrent = o.compute_queue;
    // --game-images 1: the texture cache's flags (mutable format, extended usage, storage on
    // colour targets).
    const VkImageCreateFlags game_flags =
        o.game_images ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0;
    const VkImageUsageFlags game_usage =
        o.game_images ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT : 0;
    Image color = MakeImage(gpu, o.color, o.in_w, o.in_h,
                            sampled | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | game_usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, game_flags);
    Image depth = MakeImage(gpu, VK_FORMAT_D32_SFLOAT, o.in_w, o.in_h,
                            sampled | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                (o.game_images ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
                            VK_IMAGE_ASPECT_DEPTH_BIT, game_flags);
    Image motion = MakeImage(gpu, VK_FORMAT_R16G16_SFLOAT, o.in_w, o.in_h,
                             sampled | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    Image bias = MakeImage(gpu, VK_FORMAT_R8_UNORM, o.in_w, o.in_h,
                           sampled | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    Image output = MakeImage(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, o.out_w, o.out_h,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    // Concurrent load: a large colour target cleared and blitted every frame on the graphics
    // queue (fixed-function and memory work, a stand-in for the next frame's rendering).
    Image load_a = MakeImage(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, 3840, 2160,
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    Image load_b = MakeImage(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, 3840, 2160,
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);

    auto make_pool = [&](uint32_t family) {
        VkCommandPool pool;
        const VkCommandPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                           .queueFamilyIndex = family};
        CHECK(vkCreateCommandPool(gpu.device, &info, nullptr, &pool));
        return pool;
    };
    VkCommandPool gfx_pool = make_pool(gpu.graphics_family);
    VkCommandPool dlss_pool = o.compute_queue ? make_pool(gpu.compute_family) : gfx_pool;
    VkQueue dlss_queue = o.compute_queue ? gpu.compute : gpu.graphics;
    auto make_cmd = [&](VkCommandPool pool) {
        VkCommandBuffer cmd;
        const VkCommandBufferAllocateInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .commandPool = pool,
                                               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(gpu.device, &info, &cmd));
        return cmd;
    };
    VkCommandBuffer dlss_cmd = make_cmd(dlss_pool);
    VkCommandBuffer load_cmd = make_cmd(gfx_pool);
    VkFence fence;
    const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(gpu.device, &fence_info, nullptr, &fence));
    VkQueryPool queries;
    const VkQueryPoolCreateInfo qinfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                      .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                      .queryCount = 4};
    CHECK(vkCreateQueryPool(gpu.device, &qinfo, nullptr, &queries));

    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    auto submit_wait = [&](VkQueue queue, std::vector<VkCommandBuffer> cmds) {
        const VkSubmitInfo s{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = uint32_t(cmds.size()),
                             .pCommandBuffers = cmds.data()};
        CHECK(vkQueueSubmit(queue, 1, &s, fence));
        CHECK(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(gpu.device, 1, &fence));
    };

    // Inputs: noisy colour, mid depth, small motion, half-trusted bias. Content barely moves
    // NGX's time (fixed network); it only has to be valid.
    CHECK(vkBeginCommandBuffer(load_cmd, &begin));
    for (Image* img : {&color, &motion, &bias, &output, &load_a, &load_b})
        Barrier(load_cmd, *img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    Barrier(load_cmd, depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const VkClearColorValue grey{.float32 = {0.4f, 0.35f, 0.3f, 1.0f}};
    const VkClearColorValue mv{.float32 = {0.25f, -0.1f, 0, 0}};
    const VkClearColorValue half{.float32 = {0.5f, 0, 0, 0}};
    const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(load_cmd, color.image, VK_IMAGE_LAYOUT_GENERAL, &grey, 1, &color_range);
    vkCmdClearColorImage(load_cmd, motion.image, VK_IMAGE_LAYOUT_GENERAL, &mv, 1, &color_range);
    vkCmdClearColorImage(load_cmd, bias.image, VK_IMAGE_LAYOUT_GENERAL, &half, 1, &color_range);
    const VkClearDepthStencilValue dv{0.5f, 0};
    const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(load_cmd, depth.image, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &depth_range);
    CHECK(vkEndCommandBuffer(load_cmd));
    submit_wait(gpu.graphics, {load_cmd});

    const float scale = float(o.out_w) / float(o.in_w);
    const int quality = o.quality >= 0 ? o.quality
                        : scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2
                        : scale > 1.01f ? 1 : 0;
    const Gow3DlssFeature desc{o.in_w, o.in_h, o.out_w, o.out_h, quality, 1,
                               uint32_t(10 + std::clamp(o.model, 0, 3)), o.hdr};
    CHECK(vkBeginCommandBuffer(dlss_cmd, &begin));
    const bool created = api->CreateFeature(dlss_cmd, &desc);
    CHECK(vkEndCommandBuffer(dlss_cmd));
    submit_wait(dlss_queue, {dlss_cmd});
    if (!created) {
        std::fprintf(stderr, "feature creation failed\n");
        return 1;
    }

    VkSemaphore sem{};
    if (o.load) {
        const VkSemaphoreCreateInfo si{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK(vkCreateSemaphore(gpu.device, &si, nullptr, &sem));
    }
    std::vector<double> eval_ms, frame_ms;
    const int warmup = 30;
    const auto wall_start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < warmup + o.frames; ++frame) {
        // Halton(2,3) jitter in pixels, like the port.
        auto halton = [](int i, int b) {
            float f = 1, r = 0;
            for (; i > 0; i /= b) {
                f /= b;
                r += f * (i % b);
            }
            return r;
        };
        const int phase = frame % 32 + 1;
        CHECK(vkBeginCommandBuffer(dlss_cmd, &begin));
        vkCmdResetQueryPool(dlss_cmd, queries, 0, 4);
        vkCmdWriteTimestamp2(dlss_cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, 0);
        Gow3DlssEvaluate ev{};
        ev.color = Wrap(color);
        ev.depth = Wrap(depth);
        ev.depth.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ev.motion = Wrap(motion);
        ev.output = Wrap(output);
        ev.jitter_x = halton(phase, 2) - 0.5f;
        ev.jitter_y = halton(phase, 3) - 0.5f;
        ev.reset = frame == 0;
        ev.frame_ms = 8.3f;
        ev.sharpness = 0;
        if (o.bias) ev.bias_mask = Wrap(bias);
        if (!api->Evaluate(dlss_cmd, &ev)) {
            std::fprintf(stderr, "evaluation failed\n");
            return 1;
        }
        vkCmdWriteTimestamp2(dlss_cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queries, 1);
        CHECK(vkEndCommandBuffer(dlss_cmd));
        if (o.load) {
            CHECK(vkBeginCommandBuffer(load_cmd, &begin));
            const VkImageBlit blit{
                .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .srcOffsets = {{0, 0, 0}, {3840, 2160, 1}},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .dstOffsets = {{0, 0, 0}, {3840, 2160, 1}},
            };
            for (int i = 0; i < o.load; ++i) {
                vkCmdBlitImage(load_cmd, load_a.image, VK_IMAGE_LAYOUT_GENERAL, load_b.image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
                Barrier(load_cmd, load_b, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                std::swap(load_a, load_b);
            }
            CHECK(vkEndCommandBuffer(load_cmd));
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (o.load && o.compute_queue) {
            // Both queues at once: the load on graphics, DLSS on compute.
            const VkSubmitInfo sl{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                  .commandBufferCount = 1,
                                  .pCommandBuffers = &load_cmd,
                                  .signalSemaphoreCount = 1,
                                  .pSignalSemaphores = &sem};
            CHECK(vkQueueSubmit(gpu.graphics, 1, &sl, VK_NULL_HANDLE));
            const VkSubmitInfo sd{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                  .commandBufferCount = 1,
                                  .pCommandBuffers = &dlss_cmd};
            CHECK(vkQueueSubmit(gpu.compute, 1, &sd, fence));
            CHECK(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX));
            CHECK(vkResetFences(gpu.device, 1, &fence));
            // Wait for the load too (the semaphore on an empty graphics submit).
            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            const VkSubmitInfo sw{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                  .waitSemaphoreCount = 1,
                                  .pWaitSemaphores = &sem,
                                  .pWaitDstStageMask = &stage};
            CHECK(vkQueueSubmit(gpu.graphics, 1, &sw, fence));
            CHECK(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX));
            CHECK(vkResetFences(gpu.device, 1, &fence));
        } else if (o.load) {
            submit_wait(gpu.graphics, {load_cmd, dlss_cmd});
        } else {
            submit_wait(dlss_queue, {dlss_cmd});
        }
        const double wall = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
        uint64_t ts[2];
        CHECK(vkGetQueryPoolResults(gpu.device, queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        if (frame >= warmup) {
            eval_ms.push_back(double(ts[1] - ts[0]) * gpu.timestamp_ns / 1e6);
            frame_ms.push_back(wall);
        }
    }
    const double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                         wall_start).count();
    std::printf("DLSS %ux%u -> %ux%u model %c quality %d hdr %d bias %d color %d queue %s load %d\n",
                o.in_w, o.in_h, o.out_w, o.out_h, "jklm"[std::clamp(o.model, 0, 3)], quality, o.hdr,
                o.bias, int(o.color), o.compute_queue ? "compute" : "graphics", o.load);
    std::printf("evaluate ms: median %.3f p10 %.3f p90 %.3f\n", Percentile(eval_ms, 0.5),
                Percentile(eval_ms, 0.1), Percentile(eval_ms, 0.9));
    std::printf("submit-to-done ms: median %.3f p10 %.3f p90 %.3f (%d frames, %.1f s)\n",
                Percentile(frame_ms, 0.5), Percentile(frame_ms, 0.1), Percentile(frame_ms, 0.9),
                o.frames, total_s);
    vkDeviceWaitIdle(gpu.device);
    api->Shutdown();
    return 0;
}
