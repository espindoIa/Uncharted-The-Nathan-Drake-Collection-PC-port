// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
/// gow3: the guest's full scene size: 1920x1080 (God of War III, Uncharted at 1080p) or the
/// render resolution the Uncharted resolution patch gives the game (GOW3_SCENE_SIZE=<w>x<h>).
/// Reduced scenes are fractions of it; the upscaler reconstructs it.
inline Size Native() {
    static const Size native = [] {
        Size size;
        unsigned w = 0, h = 0;
        if (const char* env = std::getenv("GOW3_SCENE_SIZE");
            env && std::sscanf(env, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 8192 &&
            h <= 8192) {
            size = {w, h};
        }
        return size;
    }();
    return native;
}
inline Size ForPreset(int preset, Size output = {}) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    constexpr std::array<double, 5> scales{1.0, 1.5, 1.7, 2.0, 3.0};
    const double scale = scales[std::clamp(preset, 0, 4)];
    return {uint32_t(std::max(1l, std::lround(output.width / scale / 2)) * 2),
            uint32_t(std::max(1l, std::lround(output.height / scale / 2)) * 2)};
}
constexpr uint32_t Pack(Size size) { return size.width | (size.height << 16); }
/// Push constant for the FragCoord conversion of a reduced scene (emit_spirv_context_get_set):
/// the scene size in 1/16 pixels of a 1920x1080 reference, so shaders scale FragCoord by
/// native / scene without knowing the native size (exact for 1080p, 1440p, 2160p).
constexpr uint32_t PackCoord(Size scene, Size native) {
    const auto unit = [](uint32_t value, uint32_t reference, uint32_t full) {
        const uint64_t scaled = (uint64_t(value) * reference * 16 + full / 2) / full;
        return uint32_t(std::clamp<uint64_t>(scaled, 1, 65535));
    };
    return unit(scene.width, 1920, native.width) | unit(scene.height, 1080, native.height) << 16;
}
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 65535u, packed >> 16} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution
