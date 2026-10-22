// SPDX-License-Identifier: GPL-2.0-or-later
// Frame generation camera (frame_gen_camera.h). For the port's three camera sources its matrices must
// reproject like the motion vectors camera_motion.comp writes. This runs a CPU twin of that shader
// (mode 0: scene constants and flat camera, mode 2: Uncharted's velocity pass) and the matrices on a
// grid of pixels, view depths and jitters, and compares them (motion, previous view z and depth);
// then the matrices' layout and inverses, and the camera basis (orthonormal, handedness kept).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include "video_core/renderer_vulkan/flat_camera.h"
#include "video_core/renderer_vulkan/frame_gen_camera.h"

namespace {

using Vulkan::FrameGenCamera;
using Vec3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>; // row-major; the columns are right, up, forward in world

constexpr double kPi = 3.14159265358979323846;
constexpr int kWidth = 1920, kHeight = 1080;

[[noreturn]] void Fail(const char* label, const char* what, double got, double want) {
    std::printf("FAIL %s: %s (got %.9g, want %.9g)\n", label, what, got, want);
    std::exit(1);
}

void Expect(const char* label, const char* what, double got, double want, double tolerance) {
    if (!(std::abs(got - want) <= tolerance)) { // also false for NaN
        Fail(label, what, got, want);
    }
}

// ---- Poses and cameras ------------------------------------------------------------------------

Mat3 Multiply3(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) {
                out[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
            }
        }
    }
    return out;
}

Mat3 Rotation(double yaw, double pitch, double roll) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch),
                 cr = std::cos(roll), sr = std::sin(roll);
    const Mat3 about_z{cy, -sy, 0, sy, cy, 0, 0, 0, 1};
    const Mat3 about_x{1, 0, 0, 0, cp, -sp, 0, sp, cp};
    const Mat3 about_y{cr, 0, sr, 0, 1, 0, -sr, 0, cr};
    return Multiply3(Multiply3(about_z, about_x), about_y);
}

struct Pose {
    Mat3 rotation; // the view axes in world: the columns are right, up, forward
    Vec3 position;
};

Pose MakePose(double yaw, double pitch, double roll, Vec3 position, bool mirrored) {
    // A mirrored basis (forward flipped) has right x up = -fwd.
    const Mat3 base{1, 0, 0, 0, 1, 0, 0, 0, mirrored ? -1.0 : 1.0};
    return {Multiply3(Rotation(yaw, pitch, roll), base), position};
}

std::array<float, 12> ViewToWorld(const Pose& pose) { // rows [R' | t']
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 4 + c] = float(pose.rotation[r * 3 + c]);
        }
        out[r * 4 + 3] = float(pose.position[r]);
    }
    return out;
}

std::array<float, 12> WorldToView(const Pose& pose) { // rows [R'^T | -R'^T t']
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        double t = 0.0;
        for (int c = 0; c < 3; ++c) {
            out[r * 4 + c] = float(pose.rotation[c * 3 + r]);
            t -= pose.rotation[c * 3 + r] * pose.position[c];
        }
        out[r * 4 + 3] = float(t);
    }
    return out;
}

struct SceneCamera { // vk_camera_motion.h Camera
    std::array<float, 12> view, inv_view; // world to view, view to world, 3x4 rows
    std::array<float, 4> proj;            // x scale, y scale, z scale, z offset
};

SceneCamera MakeScene(const Pose& pose, const std::array<float, 4>& proj) {
    return {WorldToView(pose), ViewToWorld(pose), proj};
}

// far_z 0 is an infinite far plane (depth = 1 - near / z).
std::array<float, 4> Projection(double fov_y_degrees, double aspect, double near_z, double far_z) {
    const double ys = 1.0 / std::tan(fov_y_degrees * kPi / 360.0);
    const double zs = far_z > 0.0 ? far_z / (far_z - near_z) : 1.0;
    return {float(ys / aspect), float(ys), float(zs), float(-near_z * zs)};
}

// a * b for 3x4 affine matrices (rows [R | t]): vk_camera_motion.cpp's Multiply, the shader's `reproject`.
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

// What Uncharted's velocity pass constant buffer gives CameraMotion::OnVelocityConstants.
struct VelocityPass {
    std::array<float, 4> uv_to_view;        // c0..c3
    std::array<float, 2> depth_to_z;        // c8, c9
    std::array<float, 12> to_previous_clip; // the previous clip x, y, w rows on this view position
    std::array<float, 12> to_world;         // c12..c23: [M | t]
};

// FOV 60 degrees (58 in the previous frame), 16:9, near 0.24, far plane `far_z` (0 = infinite, as in
// the game: depth = 1 - 0.24 / z). off_centre moves the principal point of both frames (the TAA path
// assumes it centred; the shader does not).
VelocityPass MakeVelocityPass(const Pose& current, const Pose& previous, bool off_centre,
                              double far_z = 0.0) {
    const double aspect = 16.0 / 9.0, t = std::tan(30.0 * kPi / 180.0);
    const double previous_t = std::tan(29.0 * kPi / 180.0);
    const double shift_x = off_centre ? 0.011 : 0.0, shift_y = off_centre ? -0.007 : 0.0;
    const std::array<float, 4> depth_terms = Projection(60.0, aspect, 0.24, far_z);
    VelocityPass pass{};
    pass.uv_to_view = {float(2.0 * aspect * t), float(-2.0 * t), float(-aspect * t + shift_x),
                       float(t + shift_y)};
    pass.depth_to_z = {depth_terms[2], depth_terms[3]};
    pass.to_world = ViewToWorld(current);
    // The game's previous view projection on world positions (clip x, y, w rows).
    const auto previous_view = WorldToView(previous);
    const double previous_xs = 1.0 / (aspect * previous_t), previous_ys = 1.0 / previous_t;
    const double previous_ox = off_centre ? 0.004 : 0.0, previous_oy = off_centre ? -0.003 : 0.0;
    float rows[3][4];
    for (int j = 0; j < 4; ++j) {
        rows[0][j] = float(previous_xs * previous_view[j] + previous_ox * previous_view[8 + j]);
        rows[1][j] = float(previous_ys * previous_view[4 + j] + previous_oy * previous_view[8 + j]);
        rows[2][j] = previous_view[8 + j];
    }
    // The same product as CameraMotion::OnVelocityConstants.
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 4; ++col) {
            float v = col == 3 ? rows[r][3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += rows[r][k] * pass.to_world[k * 4 + col];
            }
            pass.to_previous_clip[r * 4 + col] = v;
        }
    }
    return pass;
}

// ---- The shader's twin and the matrices -------------------------------------------------------

// camera_motion.comp's push constants.
struct Push {
    std::array<float, 12> reproject;
    std::array<float, 4> proj, prev_proj;
    std::array<float, 2> size, jitter;
    unsigned mode;
};

struct Reprojected {
    double motion[2];  // pixels, previous minus current
    double z;          // view z of the surface
    double prev_z;     // its z in the previous camera (the previous clip w)
    double prev_depth; // its depth buffer value there (the matrices only)
};

// camera_motion.comp main(), the motion part, in double: mode 0 (scene constants, flat camera) and
// mode 2 (Uncharted's velocity pass). The depth went through a viewport shifted by the jitter, so
// the surface is at the pixel minus the jitter.
Reprojected Twin(const Push& k, int px, int py, float depth) {
    const double scene_u = (px + 0.5 - k.jitter[0]) / k.size[0];
    const double scene_v = (py + 0.5 - k.jitter[1]) / k.size[1];
    double prev_u, prev_v, z, prev_z;
    if (k.mode == 2) {
        // z = c9 / (depth - c8), view = (z (c0 u + c2), z (c1 v + c3), z); the rows give the
        // previous clip x, y, w.
        z = k.prev_proj[1] / (depth - k.prev_proj[0]);
        const double view[4] = {z * (k.proj[0] * scene_u + k.proj[2]),
                                z * (k.proj[1] * scene_v + k.proj[3]), z, 1.0};
        double clip[3] = {};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                clip[r] += k.reproject[r * 4 + c] * view[c];
            }
        }
        prev_u = 0.5 * clip[0] / clip[2] + 0.5;
        prev_v = 0.5 - 0.5 * clip[1] / clip[2];
        prev_z = clip[2];
    } else {
        const double ndc_x = scene_u * 2.0 - 1.0, ndc_y = 1.0 - scene_v * 2.0;
        const bool sky = depth >= 1.0f;
        z = sky ? 1.0 : k.proj[3] / std::min<double>(depth - k.proj[2], -1e-8);
        // Clear depth is an infinite direction: rotated by the camera, without translation.
        const double view[4] = {ndc_x * z / k.proj[0], ndc_y * z / k.proj[1], z, sky ? 0.0 : 1.0};
        double previous[3] = {};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                previous[r] += k.reproject[r * 4 + c] * view[c];
            }
        }
        prev_z = std::max(previous[2], 1e-4);
        prev_u = previous[0] * k.prev_proj[0] / prev_z * 0.5 + 0.5;
        prev_v = 0.5 - previous[1] * k.prev_proj[1] / prev_z * 0.5;
    }
    return {{(prev_u - scene_u) * k.size[0], (prev_v - scene_v) * k.size[1]}, z, prev_z, 0.0};
}

// The same pixel through the matrices: the clip position (ndc x z, ndc y z, depth z, z) times
// clip_to_prev_clip as a row vector, divided by w, ndc to pixels (x right, y down).
Reprojected ViaMatrices(const FrameGenCamera& camera, const Push& k, int px, int py, float depth,
                        double z) {
    const double scene_u = (px + 0.5 - k.jitter[0]) / k.size[0];
    const double scene_v = (py + 0.5 - k.jitter[1]) / k.size[1];
    const double clip[4] = {(scene_u * 2.0 - 1.0) * z, (1.0 - scene_v * 2.0) * z, depth * z, z};
    double prev[4] = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            prev[j] += clip[i] * camera.clip_to_prev_clip[i * 4 + j];
        }
    }
    const double ndc_x = prev[0] / prev[3], ndc_y = prev[1] / prev[3];
    return {{(ndc_x * 0.5 + 0.5 - scene_u) * k.size[0], (0.5 - ndc_y * 0.5 - scene_v) * k.size[1]},
            z, prev[3], prev[2] / prev[3]};
}

// 14 x 24 pixels (corners and edges included) x 7 view depths (+ sky) x 3 jitters.
void CompareGrid(const char* label, const FrameGenCamera& camera, Push push, bool with_sky) {
    // The depth terms of this and the previous frame (the velocity pass has the same in both).
    const double zs = push.mode == 2 ? push.prev_proj[0] : push.proj[2];
    const double zo = push.mode == 2 ? push.prev_proj[1] : push.proj[3];
    const double prev_zs = push.mode == 2 ? zs : push.prev_proj[2];
    const double prev_zo = push.mode == 2 ? zo : push.prev_proj[3];
    constexpr float jitters[3][2] = {{0.0f, 0.0f}, {0.37f, -0.29f}, {-0.43f, 0.21f}};
    constexpr double depths[7] = {0.3, 0.8, 2.5, 7.0, 20.0, 60.0, 150.0};
    double max_motion = 0.0, max_depth = 0.0;
    int samples = 0;
    for (const auto& jitter : jitters) {
        push.jitter = {jitter[0], jitter[1]};
        for (int iy = 0; iy < 14; ++iy) {
            for (int ix = 0; ix < 24; ++ix) {
                const int px = ix * (kWidth - 1) / 23, py = iy * (kHeight - 1) / 13;
                for (int s = 0; s < 8; ++s) {
                    const bool sky = s == 7;
                    if (sky && !with_sky) {
                        continue;
                    }
                    const float depth = sky ? 1.0f : float(zs + zo / depths[s]);
                    const Reprojected want = Twin(push, px, py, depth);
                    const Reprojected got = ViaMatrices(camera, push, px, py, depth, want.z);
                    Expect(label, "motion x", got.motion[0], want.motion[0], 1e-3);
                    Expect(label, "motion y", got.motion[1], want.motion[1], 1e-3);
                    max_motion = std::max({max_motion, std::abs(got.motion[0] - want.motion[0]),
                                           std::abs(got.motion[1] - want.motion[1])});
                    if (!sky) { // the previous depth of a direction at infinity is just zs
                        Expect(label, "previous view z", got.prev_z, want.prev_z,
                               1e-5 * want.prev_z);
                        Expect(label, "previous depth", got.prev_depth,
                               prev_zs + prev_zo / want.prev_z, 1e-5);
                        max_depth = std::max(max_depth, std::abs(got.prev_depth -
                                             (prev_zs + prev_zo / want.prev_z)));
                    }
                    ++samples;
                }
            }
        }
    }
    std::printf("frame gen camera: %s: %d samples, max error %.2e px, previous depth %.2e\n",
                label, samples, max_motion, max_depth);
}

// ---- Matrix and basis checks ------------------------------------------------------------------

// view_to_clip in the row-vector layout: clip = (x xs + z ox, y ys + z oy, z zs + zo, z).
void CheckLayout(const char* label, const FrameGenCamera& camera, double xs, double ys, double zs,
                 double zo, double ox, double oy) {
    const double want[16] = {xs, 0, 0, 0, 0, ys, 0, 0, ox, oy, zs, 1, 0, 0, zo, 0};
    for (int i = 0; i < 16; ++i) {
        Expect(label, "view_to_clip layout", camera.view_to_clip[i], want[i],
               2e-7 * std::max(1.0, std::abs(want[i])));
    }
}

// a * b is the identity.
void CheckInverse(const char* label, const char* what, const std::array<float, 16>& a,
                  const std::array<float, 16>& b, double tolerance) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += double(a[r * 4 + k]) * b[k * 4 + c];
            }
            Expect(label, what, sum, r == c ? 1.0 : 0.0, tolerance);
        }
    }
}

// right, up, forward: orthonormal, the columns of the view to world rotation (to `column_tolerance`),
// right x up = handedness * forward (+1 proper rotation, -1 mirrored); position is its translation.
void CheckBasis(const char* label, const FrameGenCamera& camera, const std::array<float, 12>& to_world,
                double handedness, double column_tolerance = 2e-5) {
    const std::array<std::array<float, 3>, 3> axes = {camera.right, camera.up, camera.fwd};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k) {
                dot += double(axes[i][k]) * axes[j][k];
            }
            Expect(label, "basis orthonormal", dot, i == j ? 1.0 : 0.0, 2e-6);
        }
        for (int k = 0; k < 3; ++k) {
            Expect(label, "basis is a column of the view to world", axes[i][k], to_world[k * 4 + i],
                   column_tolerance);
        }
    }
    const Vec3 r{camera.right[0], camera.right[1], camera.right[2]};
    const Vec3 u{camera.up[0], camera.up[1], camera.up[2]};
    const Vec3 cross{r[1] * u[2] - r[2] * u[1], r[2] * u[0] - r[0] * u[2], r[0] * u[1] - r[1] * u[0]};
    for (int k = 0; k < 3; ++k) {
        Expect(label, "right x up = handedness * forward", cross[k], handedness * camera.fwd[k], 2e-6);
        Expect(label, "position", camera.pos[k], to_world[k * 4 + 3], 0.0);
    }
}

// Scene constants and flat camera (mode 0).
FrameGenCamera RunScene(const char* label, const SceneCamera& current, const SceneCamera& previous,
                        double handedness, bool with_sky) {
    const auto reproject = Multiply(previous.view, current.inv_view);
    const FrameGenCamera camera =
        Vulkan::MakeFrameGenCamera(current.inv_view, reproject, current.proj, previous.proj);
    if (!camera.valid) {
        Fail(label, "valid", 0, 1);
    }
    CheckLayout(label, camera, current.proj[0], current.proj[1], current.proj[2], current.proj[3], 0, 0);
    CheckInverse(label, "clip_to_view * view_to_clip", camera.clip_to_view, camera.view_to_clip, 1e-4);
    CheckInverse(label, "prev_clip_to_clip * clip_to_prev_clip", camera.prev_clip_to_clip,
                 camera.clip_to_prev_clip, 1e-4);
    CheckBasis(label, camera, current.inv_view, handedness);
    CompareGrid(label, camera,
                {reproject, current.proj, previous.proj, {float(kWidth), float(kHeight)}, {0.0f, 0.0f}, 0},
                with_sky);
    return camera;
}

// Uncharted's velocity pass (mode 2).
void RunVelocity(const char* label, const VelocityPass& pass, double handedness) {
    const FrameGenCamera camera = Vulkan::MakeFrameGenCameraVelocity(
        pass.uv_to_view, pass.depth_to_z, pass.to_previous_clip, pass.to_world);
    if (!camera.valid) {
        Fail(label, "valid", 0, 1);
    }
    const double c0 = pass.uv_to_view[0], c1 = pass.uv_to_view[1], c2 = pass.uv_to_view[2],
                 c3 = pass.uv_to_view[3];
    CheckLayout(label, camera, 2.0 / c0, -2.0 / c1, pass.depth_to_z[0], pass.depth_to_z[1],
                -(c0 + 2.0 * c2) / c0, 1.0 + 2.0 * c3 / c1);
    CheckInverse(label, "clip_to_view * view_to_clip", camera.clip_to_view, camera.view_to_clip, 1e-4);
    CheckInverse(label, "prev_clip_to_clip * clip_to_prev_clip", camera.prev_clip_to_clip,
                 camera.clip_to_prev_clip, 1e-4);
    CheckBasis(label, camera, pass.to_world, handedness);
    CompareGrid(label, camera,
                {pass.to_previous_clip, pass.uv_to_view,
                 {pass.depth_to_z[0], pass.depth_to_z[1], 0.0f, 0.0f},
                 {float(kWidth), float(kHeight)}, {0.0f, 0.0f}, 2},
                false);
}

// vs 3797ed97's user data in U2 chapter 1 (17 Oct 2026), as in test_flat_camera.cpp: the world to
// view (3x4 rows, then 0 0 0 1) and the projection (rows): x scale -0.8391, y scale 1.49173,
// depth = 1 - 0.24 / z.
constexpr float u2_flat_camera[] = {
    -0.991625f, -1.67228e-05f, 0.129149f, 14.1322f, 0.106125f, 0.569773f, 0.814921f, 9.96238f,
    -0.0735991f, 0.821802f, -0.565f, 26.7402f, -0.0f, -0.0f, -0.0f, 1.0f,
    -0.8391f, 0.0f, -0.0f, 0.0f, 0.0f, 1.49173f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, -0.240001f, 0.0f,
    0.0f, 1.0f, 0.0f};

} // namespace

int main() {
    constexpr double aspect = 16.0 / 9.0;

    // 1. Scene constants: a rotating and translating camera, FOV 60 degrees (58 in the previous
    //    frame), near 0.1, far 3000, standard depth. Both handedness of the view axes.
    for (const bool mirrored : {false, true}) {
        const SceneCamera current =
            MakeScene(MakePose(0.70, -0.17, 0.05, {12.5, 3.2, -40.1}, mirrored),
                      Projection(60.0, aspect, 0.1, 3000.0));
        const SceneCamera previous =
            MakeScene(MakePose(0.679, -0.165, 0.046, {12.38, 3.15, -39.97}, mirrored),
                      Projection(58.0, aspect, 0.1, 3000.0));
        const char* label = mirrored ? "scene constants, mirrored basis" : "scene constants";
        RunScene(label, current, previous, mirrored ? -1.0 : 1.0, false);

        // A camera that has not moved reprojects onto itself.
        const auto still = Vulkan::MakeFrameGenCamera(
            current.inv_view, Multiply(current.view, current.inv_view), current.proj, current.proj);
        for (int i = 0; i < 16; ++i) {
            Expect(label, "static clip_to_prev_clip is the identity", still.clip_to_prev_clip[i],
                   i % 5 == 0 ? 1.0 : 0.0, 1e-4);
        }
        // A basis off by up to 1e-3 (what the flat camera accepts) comes out orthonormal.
        SceneCamera skewed = current;
        skewed.inv_view[1] += 4e-4f;
        skewed.inv_view[2] -= 3e-4f;
        skewed.inv_view[5] += 2e-4f;
        skewed.inv_view[8] -= 3e-4f;
        const auto skew = Vulkan::MakeFrameGenCamera(
            skewed.inv_view, Multiply(previous.view, skewed.inv_view), skewed.proj, previous.proj);
        if (!skew.valid) {
            Fail(label, "skewed basis valid", 0, 1);
        }
        CheckBasis(label, skew, skewed.inv_view, mirrored ? -1.0 : 1.0, 2e-3);
    }

    // 2. Flat camera: U2's captured camera (real data), moved a little for the previous frame.
    //    Its x scale is negative: the port's view +x is the screen's left, and the view axes are a
    //    proper rotation (right x up = forward), as the header says.
    {
        const auto found = Vulkan::FindFlatCamera(u2_flat_camera, std::size(u2_flat_camera));
        if (!found) {
            Fail("flat camera", "U2 capture found", 0, 1);
        }
        const SceneCamera current{found->view, found->inv_view, found->proj};
        Pose pose{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                pose.rotation[r * 3 + c] = current.inv_view[r * 4 + c];
            }
            pose.position[r] = current.inv_view[r * 4 + 3];
        }
        // The camera turned 1.2 degrees about the world's up axis, 0.5 about x, and moved 0.2 m.
        pose.rotation = Multiply3(Rotation(-0.021, 0.009, 0.0), pose.rotation);
        pose.position = {pose.position[0] + 0.11, pose.position[1] - 0.16, pose.position[2] + 0.05};
        const SceneCamera previous = MakeScene(pose, current.proj);
        const auto camera = RunScene("flat camera (U2 capture)", current, previous, 1.0, true);
        if (!(camera.view_to_clip[0] < 0.0f)) {
            Fail("flat camera (U2 capture)", "x scale is negative", camera.view_to_clip[0], -0.8391);
        }

        // Mirroring the view x axis (x scale and right flipped) leaves clip_to_prev_clip alone.
        const auto mirror = [](SceneCamera c) {
            for (int k = 0; k < 4; ++k) {
                c.view[k] = -c.view[k];
            }
            for (int r = 0; r < 3; ++r) {
                c.inv_view[r * 4] = -c.inv_view[r * 4];
            }
            c.proj[0] = -c.proj[0];
            return c;
        };
        const SceneCamera mirrored_current = mirror(current), mirrored_previous = mirror(previous);
        const auto flipped = Vulkan::MakeFrameGenCamera(
            mirrored_current.inv_view, Multiply(mirrored_previous.view, mirrored_current.inv_view),
            mirrored_current.proj, mirrored_previous.proj);
        if (!flipped.valid) {
            Fail("mirrored flat camera", "valid", 0, 1);
        }
        for (int i = 0; i < 16; ++i) {
            Expect("mirrored flat camera", "clip_to_prev_clip unchanged", flipped.clip_to_prev_clip[i],
                   camera.clip_to_prev_clip[i], 1e-4 * std::max(1.0f, std::abs(camera.clip_to_prev_clip[i])));
        }
        for (int k = 0; k < 3; ++k) {
            Expect("mirrored flat camera", "right flipped", flipped.right[k], -camera.right[k], 2e-6);
            Expect("mirrored flat camera", "up kept", flipped.up[k], camera.up[k], 2e-6);
        }
    }

    // 3. Velocity pass: FOV 60 degrees, near 0.24; the pose is a proper rotation or a mirrored one,
    //    the frustum centred or not, the far plane infinite (the game's) or 500 (z scale above 1).
    {
        const Pose current = MakePose(1.9, 0.12, -0.04, {-7.0, 1.7, 22.5}, false);
        const Pose previous = MakePose(1.882, 0.118, -0.037, {-7.09, 1.72, 22.38}, false);
        RunVelocity("velocity pass", MakeVelocityPass(current, previous, false), 1.0);
        RunVelocity("velocity pass, off-centre, far plane 500",
                    MakeVelocityPass(current, previous, true, 500.0), 1.0);
        const Pose mirrored_current = MakePose(1.9, 0.12, -0.04, {-7.0, 1.7, 22.5}, true);
        const Pose mirrored_previous = MakePose(1.882, 0.118, -0.037, {-7.09, 1.72, 22.38}, true);
        RunVelocity("velocity pass, mirrored basis",
                    MakeVelocityPass(mirrored_current, mirrored_previous, false), -1.0);
    }

    // 4. Input that is not a camera is refused, and the general inverse.
    {
        const SceneCamera camera = MakeScene(MakePose(0.1, 0.2, 0.0, {1, 2, 3}, false),
                                             Projection(60.0, aspect, 0.1, 3000.0));
        auto flat_projection = camera.proj;
        flat_projection[0] = 0.0f;
        if (Vulkan::MakeFrameGenCamera(camera.inv_view, camera.view, flat_projection, camera.proj).valid) {
            Fail("degenerate", "zero x scale refused", 1, 0);
        }
        auto broken = camera.inv_view;
        broken[3] = std::nanf("");
        if (Vulkan::MakeFrameGenCamera(broken, camera.view, camera.proj, camera.proj).valid) {
            Fail("degenerate", "NaN position refused", 1, 0);
        }
        auto collapsed = camera.inv_view;
        collapsed[0] = collapsed[4] = collapsed[8] = 0.0f; // no right vector
        if (Vulkan::MakeFrameGenCamera(collapsed, camera.view, camera.proj, camera.proj).valid) {
            Fail("degenerate", "collapsed basis refused", 1, 0);
        }
        const VelocityPass pass = MakeVelocityPass(MakePose(0, 0, 0, {0, 0, 0}, false),
                                                   MakePose(0, 0, 0, {0, 0, 1}, false), false);
        auto no_frustum = pass.uv_to_view;
        no_frustum[0] = 0.0f;
        if (Vulkan::MakeFrameGenCameraVelocity(no_frustum, pass.depth_to_z, pass.to_previous_clip,
                                               pass.to_world).valid) {
            Fail("degenerate", "zero velocity frustum refused", 1, 0);
        }

        const std::array<float, 16> general = {2, 0.5f, -1, 0.25f, 0.3f, -3, 0.7f, 0,
                                               1, 2, 4, -0.5f, 0.1f, -0.2f, 0.6f, 5};
        const auto inverse = Vulkan::InvertMatrix4(general);
        if (!inverse) {
            Fail("InvertMatrix4", "general matrix inverted", 0, 1);
        }
        CheckInverse("InvertMatrix4", "m * inverse", general, *inverse, 1e-6);
        // A zero on the diagonal needs the pivoting.
        const std::array<float, 16> swap = {0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0};
        const auto swapped = Vulkan::InvertMatrix4(swap);
        if (!swapped) {
            Fail("InvertMatrix4", "permutation inverted", 0, 1);
        }
        CheckInverse("InvertMatrix4", "permutation * inverse", swap, *swapped, 1e-6);
        const std::array<float, 16> singular = {1, 2, 3, 4, 2, 4, 6, 8, 1, 0, 0, 1, 0, 1, 1, 0};
        if (Vulkan::InvertMatrix4(singular)) {
            Fail("InvertMatrix4", "singular matrix refused", 1, 0);
        }
    }
    std::puts("frame gen camera: ok");
    return 0;
}
