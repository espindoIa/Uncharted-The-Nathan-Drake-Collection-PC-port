// SPDX-License-Identifier: GPL-2.0-or-later
// Uncharted 2 (and engines like it): the main camera is not in a constant buffer the GPU
// reads by address. The CPU copies it into the user data (flat constants) of every scene
// vertex shader, at an offset that depends on the shader: 16 floats of world-to-view (3x4
// rows, then 0 0 0 1) followed by the 4x4 projection (rows). Captured in Among Thieves'
// chapter 1 (17 Oct): x scale -0.8391, y scale 1.4917, row 2 (0 0 1 -0.24), row 3 (0 0 1 0),
// i.e. clip w = view z, depth = 1 - near / z (near 0.24, far infinite), no game jitter.
// Shadow and light cameras use orthographic projections and never match.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace Vulkan {

struct FlatCamera {
    std::array<float, 12> view{};     ///< world to view, 3x4 rows
    std::array<float, 12> inv_view{}; ///< view to world, 3x4 rows
    std::array<float, 4> proj{};      ///< x scale, y scale, z scale, z offset
    std::size_t offset = 0;           ///< float index of the view matrix
};

/// Looks for the view + perspective projection block in `data` (`count` floats).
inline std::optional<FlatCamera> FindFlatCamera(const float* data, std::size_t count) {
    constexpr float tolerance = 1e-3f;
    for (std::size_t i = 0; i + 32 <= count; ++i) {
        const float* v = data + i;
        const float* p = v + 16;
        // Fixed zeros and ones first: they reject almost every offset cheaply.
        if (v[12] != 0.0f || v[13] != 0.0f || v[14] != 0.0f || v[15] != 1.0f) continue;
        if (p[12] != 0.0f || p[13] != 0.0f || p[15] != 0.0f || std::abs(p[14]) != 1.0f) continue;
        if (p[1] != 0.0f || p[2] != 0.0f || p[4] != 0.0f || p[6] != 0.0f) continue;
        if (p[0] == 0.0f || p[5] <= 0.0f || p[11] == 0.0f || std::isnan(p[0])) continue;
        bool orthonormal = true;
        for (int a = 0; a < 3 && orthonormal; ++a) {
            const float* ra = v + 4 * a;
            const float length = ra[0] * ra[0] + ra[1] * ra[1] + ra[2] * ra[2];
            orthonormal = std::abs(length - 1.0f) < tolerance;
            for (int b = a + 1; b < 3 && orthonormal; ++b) {
                const float* rb = v + 4 * b;
                orthonormal = std::abs(ra[0] * rb[0] + ra[1] * rb[1] + ra[2] * rb[2]) < tolerance;
            }
        }
        if (!orthonormal) continue;
        FlatCamera camera;
        for (int k = 0; k < 12; ++k) camera.view[k] = v[k];
        // Inverse of a rigid transform: transposed rotation, translation -R^T t.
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) camera.inv_view[r * 4 + c] = v[c * 4 + r];
            camera.inv_view[r * 4 + 3] =
                -(v[r] * v[3] + v[4 + r] * v[7] + v[8 + r] * v[11]);
        }
        // Clip w = p[14] * z: a camera looking down -z (w = -z) is turned to the +z
        // convention camera_motion.comp expects by negating the third view row.
        if (p[14] < 0.0f) {
            for (int k = 8; k < 12; ++k) camera.view[k] = -camera.view[k];
            for (int r = 0; r < 3; ++r) camera.inv_view[r * 4 + 2] = -camera.inv_view[r * 4 + 2];
            camera.proj = {p[0], p[5], -p[10], p[11]};
        } else {
            camera.proj = {p[0], p[5], p[10], p[11]};
        }
        camera.offset = i;
        return camera;
    }
    return std::nullopt;
}

} // namespace Vulkan
