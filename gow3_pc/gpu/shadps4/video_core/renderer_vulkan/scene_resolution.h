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
/// gow3 19 Oct 2026: Ultra Quality (preset 5, listed after Native AA): 71% of the output per axis.
/// DLSS model M's cost follows its input size: at 4K output on an RTX 5070 7.9 ms with a 4K input
/// (DLAA), 6.0 at 1.2x, 5.3 at 1.3x, 2.9 at 2x. U3 bar fight against DLAA, model M (PresentMon,
/// alternating windows): 1.2x +8.7% FPS, 1.3x +15.9%, 1.4x +24.1%, Quality (1.5x) +30.8%.
/// GOW3_ULTRA_QUALITY_SCALE=<1.05..1.49> (tests) replaces the scale.
inline double UltraQualityScale() {
    static const double scale = [] {
        const char* env = std::getenv("GOW3_ULTRA_QUALITY_SCALE");
        const double value = env ? std::atof(env) : 0.0;
        return value >= 1.05 && value <= 1.49 ? value : 1.4;
    }();
    return scale;
}
inline Size ForPreset(int preset, Size output = {}) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    const std::array<double, 6> scales{1.0, 1.5, 1.7, 2.0, 3.0, UltraQualityScale()};
    const double scale = scales[std::clamp(preset, 0, 5)];
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
