// SPDX-License-Identifier: GPL-2.0-or-later
// FragCoord conversion of a reduced scene (scene_resolution.h PackCoord, read by
// emit_spirv_context_get_set): the shader scales FragCoord by (1920 * 16) / packed width and
// (1080 * 16) / packed height, which must equal native / scene for every native size the
// Uncharted resolution patch gives the game.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "video_core/renderer_vulkan/scene_resolution.h"

int main() {
    using namespace Vulkan;
    for (const auto native : {SceneResolution::Size{1920, 1080}, SceneResolution::Size{2560, 1440},
                              SceneResolution::Size{3840, 2160}, SceneResolution::Size{1600, 900},
                              SceneResolution::Size{1280, 720}}) {
        const bool exact = native.width % 1920 == 0 || native.width == 2560;
        for (int preset = 1; preset < 5; ++preset) {
            const auto scene = SceneResolution::ForPreset(preset, native);
            const unsigned packed = SceneResolution::PackCoord(scene, native);
            const double sx = 1920.0 * 16 / double(packed & 65535);
            const double sy = 1080.0 * 16 / double(packed >> 16);
            const double ex = double(native.width) / scene.width;
            const double ey = double(native.height) / scene.height;
            // Exact where the units divide; else at most ~0.1 pixel at the far edge.
            const double tolerance = exact ? 1e-9 : 0.1 / native.height;
            if (std::abs(sx / ex - 1) > tolerance || std::abs(sy / ey - 1) > tolerance) {
                std::printf("native %ux%u preset %d: scale %.6f/%.6f, expected %.6f/%.6f\n",
                            native.width, native.height, preset, sx, sy, ex, ey);
                return 1;
            }
        }
    }
    // Native size: the shader keeps FragCoord (zero push constant), and so does the reference.
    assert(SceneResolution::PackCoord({1920, 1080}, {1920, 1080}) == (30720u | 17280u << 16));
    std::puts("scene coord: ok");
    return 0;
}
