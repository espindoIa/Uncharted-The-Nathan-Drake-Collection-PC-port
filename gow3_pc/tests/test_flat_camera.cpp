// SPDX-License-Identifier: GPL-2.0-or-later
// Uncharted 2 camera in vertex shader user data (flat_camera.h), with data from a frame capture
// of chapter 1 (17 Oct, vs 3797ed97 user data and the light pre-pass shadow vs aab8a9a6).
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iterator>
#include "video_core/renderer_vulkan/flat_camera.h"

namespace {

// vs 3797ed97 (scene color pass): object matrix at 38, light direction at 50, the camera position
// at 54 (the game's own copy, used here as the reference), view at 58, projection at 74.
constexpr float scene[] = {
    1.99213e-17f, 2.24208e-44f, 1.61625e-27f, 2.24208e-44f, 2.27813e-18f, 1.46939e-39f,
    8.40779e-45f, 1.14772e-19f, 0, 0, 0, 0, 0, 0, 0, 0, 3.06121e-38f, 2.57142e-39f, 3.69943e-43f,
    1.14309e-19f, -6.08143e-19f, 3.67366e-40f, 3.69943e-43f, 1.10916e-19f, 3.06121e-38f,
    2.57142e-39f, 3.69943e-43f, 1.13925e-19f, -6.08448e-19f, 7.34708e-40f, 3.69943e-43f,
    1.13925e-19f, 0.000594918f, 2.24208e-44f, 1.19794e-23f, 2.24208e-44f, 2.19615e-18f,
    2.24208e-44f, -0.058879f, 0.524177f, 0.849572f, 25.347f, -0.995341f, 0.0342684f,
    -0.0901246f, 10.1031f, -0.0763547f, -0.85092f, 0.519717f, -86.5094f, -0.7f, -0.456f, 0.067f,
    0.0f, 14.9247f, -27.6512f, 5.16451f, 1.0f, -0.991625f, -1.67228e-05f, 0.129149f, 14.1322f,
    0.106125f, 0.569773f, 0.814921f, 9.96238f, -0.0735991f, 0.821802f, -0.565f, 26.7402f, -0.0f,
    -0.0f, -0.0f, 1.0f, -0.8391f, 0.0f, -0.0f, 0.0f, 0.0f, 1.49173f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    -0.240001f, 0.0f, 0.0f, 1.0f, 0.0f};

// vs aab8a9a6 in the sun shadow pass: a scaled light matrix and an orthographic projection.
constexpr float shadow[] = {
    6.27084e-16f, 2.24208e-44f, 5.91657e-30f, 2.24208e-44f, 2.19838e-18f, 1.46939e-39f,
    8.40779e-45f, 1.14772e-19f, 0, 0, 0, 0, 0, 0, 0, 0, -3.1241e-19f, 1.10205e-39f, 4.35524e-42f,
    1.14309e-19f, 0.00061417f, 2.24208e-44f, 2.81025e-20f, 2.24208e-44f, 1, 0, 0, 0, 0, 1, 0, 0,
    0, 0, 1, 0, -4.87776e-09f, -0.145369f, -0.989377f, -0.404961f, 0.60348f, -0.906818f,
    0.133239f, -31.8272f, -0.7f, -1.09479f, 0.160857f, -15.9629f, -0.0f, -0.0f, -0.0f, 1.0f,
    -0.379833f, 0, 0, 0, 0, 0.302118f, 0, 0, 0, 0, 0.0878866f, 0.5f, 0, 0, 0, 1.0f};

bool Near(float a, float b, float tolerance = 2e-3f) {
    return std::abs(a - b) <= tolerance * std::max(1.0f, std::abs(b));
}

} // namespace

int main() {
    using Vulkan::FindFlatCamera;
    const auto camera = FindFlatCamera(scene, std::size(scene));
    assert(camera && camera->offset == 58);
    // Projection: x/y scale (16:9: 1.49173 / 0.8391 = 1.778), depth = 1 - 0.24 / z.
    assert(Near(camera->proj[0], -0.8391f) && Near(camera->proj[1], 1.49173f));
    assert(Near(camera->proj[2], 1.0f) && Near(camera->proj[3], -0.240001f));
    // The view-to-world translation is the camera position the game stores beside the view.
    assert(Near(camera->inv_view[3], 14.9247f) && Near(camera->inv_view[7], -27.6512f) &&
           Near(camera->inv_view[11], 5.16451f));
    // view * inv_view is the identity.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = camera->view[r * 4 + 3] * (c == 3 ? 1.0f : 0.0f);
            for (int k = 0; k < 3; ++k) sum += camera->view[r * 4 + k] * camera->inv_view[k * 4 + c];
            assert(Near(sum, r == c ? 1.0f : 0.0f));
        }
    }
    // The light's orthographic camera is not a scene camera.
    assert(!FindFlatCamera(shadow, std::size(shadow)));
    // A block cut short by the user data size is not read past its end.
    assert(!FindFlatCamera(scene, 58 + 31));
    std::puts("flat camera: ok");
    return 0;
}
