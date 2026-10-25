// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the camera as NVIDIA DLSS Frame Generation (NGX: NVSDK_NGX_DLSSG_Opt_Eval_Params, the same
// fields as Streamline's sl::Constants) takes it. Pure math without Vulkan, like flat_camera.h:
// CameraMotion::GetFrameGenCamera fills it from the port's three camera sources (scene constants,
// flat camera, Uncharted's velocity pass) and tests/test_frame_gen_camera.cpp proves that its
// matrices reproject like the motion vectors camera_motion.comp writes.
//
// NGX conventions: 4x4 float matrices, ROW-MAJOR with the ROW-VECTOR convention (p' = p * M; products
// read left to right: clipToPrevClip = clipToView * viewToViewPrev * viewToClipPrev), without the TAA
// jitter (the port jitters by shifting the viewport, so the guest's matrices are jitter-free already).
// View space x right, y up, z forward; clip space x right, y up, depth 0 near .. 1 far (not
// inverted), w = view z; pixels x right, y down; motion vectors in pixels, previous minus current.
// The port's cameras (vk_camera_motion.h) are column-vector matrices (v' = R v + t): transposed here.
//
// Handedness (22 Oct 2026, from U2's captured flat camera, tests/test_flat_camera.cpp): its x scale is
// negative (-0.8391), so the port's view +x points to the screen's LEFT, and the rows of its view
// matrix are a proper rotation (x cross y = z): the port's view space is right-handed (x left, y up,
// z forward) in U2's right-handed world (screen right cross up = -forward there), where NGX's
// "x right" would be left-handed. The matrices do not care (mirroring the view x axis leaves
// clipToPrevClip alone) and the vectors are the world images of the matrices' own view axes, which is
// what Streamline's recalculateCameraMatrices rebuilds the matrices from; so `right` is the
// screen's left in U2. right x up = fwd holds when the view axes are a proper rotation, as in U2; a
// mirrored basis keeps its sign (right x up = -fwd): the handedness of the input is not assumed.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace Vulkan {

struct FrameGenCamera {
    bool valid = false;
    /// Row-major, row-vector (p' = p * M), no jitter. A pixel at ndc (x, y) with view depth z and
    /// depth buffer value d is the clip position (x z, y z, d z, z).
    std::array<float, 16> view_to_clip{}, clip_to_view{}, clip_to_prev_clip{}, prev_clip_to_clip{};
    /// World space: the position, and the world images of the view's +y, +x and +z axes
    /// (orthonormal, handedness as the input's: see above).
    std::array<float, 3> pos{}, up{}, right{}, fwd{};
    float near_z = 0.1f, far_z = 3000.0f, fov_y = 1.0f /* radians */, aspect = 16.0f / 9.0f;
};

namespace frame_gen_detail {

using Mat4 = std::array<double, 16>;
using Vec3 = std::array<double, 3>;

inline Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            for (int k = 0; k < 4; ++k) {
                out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
            }
        }
    }
    return out;
}

/// Gauss-Jordan with partial pivoting; nullopt for a (nearly) singular or non-finite matrix.
inline std::optional<Mat4> Invert(Mat4 a) {
    Mat4 inv{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    double scale = 0.0;
    for (const double v : a) {
        scale = std::max(scale, std::abs(v));
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::abs(a[r * 4 + col]) > std::abs(a[pivot * 4 + col])) {
                pivot = r;
            }
        }
        if (!(std::abs(a[pivot * 4 + col]) > 1e-12 * scale)) {
            return std::nullopt;
        }
        if (pivot != col) {
            for (int c = 0; c < 4; ++c) {
                std::swap(a[pivot * 4 + c], a[col * 4 + c]);
                std::swap(inv[pivot * 4 + c], inv[col * 4 + c]);
            }
        }
        const double inverse_pivot = 1.0 / a[col * 4 + col];
        for (int c = 0; c < 4; ++c) {
            a[col * 4 + c] *= inverse_pivot;
            inv[col * 4 + c] *= inverse_pivot;
        }
        for (int r = 0; r < 4; ++r) {
            const double factor = a[r * 4 + col];
            if (r == col || factor == 0.0) {
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                a[r * 4 + c] -= factor * a[col * 4 + c];
                inv[r * 4 + c] -= factor * inv[col * 4 + c];
            }
        }
    }
    return inv;
}

/// A 3x4 affine matrix with column vectors (rows [R | t], v' = R v + t) as the 4x4 row-vector
/// matrix (its transpose with the 0 0 0 1 row added).
inline Mat4 RowVectorAffine(const std::array<float, 12>& a) {
    Mat4 m{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m[i * 4 + j] = a[j * 4 + i];
        }
        m[12 + i] = a[i * 4 + 3];
    }
    m[15] = 1.0;
    return m;
}

/// View to clip, row-vector: clip = (x xs + z ox, y ys + z oy, z zs + zo, z); ox = oy = 0 for a
/// centred frustum. depth = zs + zo / z.
inline Mat4 Projection(double xs, double ys, double ox, double oy, double zs, double zo) {
    return Mat4{xs, 0, 0, 0, 0, ys, 0, 0, ox, oy, zs, 1, 0, 0, zo, 0};
}

inline double Dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

inline bool Normalize(Vec3& v) {
    const double length = std::sqrt(Dot(v, v));
    if (!(length > 1e-6)) {
        return false;
    }
    for (double& x : v) {
        x /= length;
    }
    return true;
}

template <std::size_t N>
bool Finite(const std::array<float, N>& values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}

/// The camera pose from a view to world 3x4 (rows [R | t], column vectors): position t and the
/// columns of R as right (view +x), up (+y), forward (+z), made exactly orthonormal (the flat camera
/// accepts rows off by 1e-3, and NGX's scene change detection only runs on an orthonormal basis).
/// Forward stays; right loses what it shares with it; up completes the basis on the same side as
/// the input's (right x up = +fwd for a proper rotation, -fwd for a mirrored one).
inline bool Basis(const std::array<float, 12>& a, FrameGenCamera& camera) {
    Vec3 right{a[0], a[4], a[8]}, up{a[1], a[5], a[9]}, fwd{a[2], a[6], a[10]};
    if (!Normalize(right) || !Normalize(up) || !Normalize(fwd)) {
        return false;
    }
    const double handedness = Dot(Cross(right, up), fwd);
    if (!(std::abs(handedness) > 0.5)) {
        return false;
    }
    const double shared = Dot(right, fwd);
    for (int i = 0; i < 3; ++i) {
        right[i] -= shared * fwd[i];
    }
    if (!Normalize(right)) {
        return false;
    }
    up = Cross(fwd, right);
    for (int i = 0; i < 3; ++i) {
        camera.right[i] = static_cast<float>(right[i]);
        camera.up[i] = static_cast<float>(handedness < 0.0 ? -up[i] : up[i]);
        camera.fwd[i] = static_cast<float>(fwd[i]);
        camera.pos[i] = a[i * 4 + 3];
    }
    return true;
}

/// The matrices from view to clip and from this frame's view to the previous frame's clip, and the
/// pose; invalid (all zero) when one of them cannot be inverted or is not finite.
inline FrameGenCamera Assemble(const Mat4& view_to_clip, const Mat4& view_to_prev_clip,
                               const std::array<float, 12>& view_to_world) {
    FrameGenCamera camera;
    const auto clip_to_view = Invert(view_to_clip);
    if (!clip_to_view) {
        return {};
    }
    const Mat4 clip_to_prev_clip = Mul(*clip_to_view, view_to_prev_clip);
    const auto prev_clip_to_clip = Invert(clip_to_prev_clip);
    if (!prev_clip_to_clip || !Basis(view_to_world, camera)) {
        return {};
    }
    for (int i = 0; i < 16; ++i) {
        camera.view_to_clip[i] = static_cast<float>(view_to_clip[i]);
        camera.clip_to_view[i] = static_cast<float>((*clip_to_view)[i]);
        camera.clip_to_prev_clip[i] = static_cast<float>(clip_to_prev_clip[i]);
        camera.prev_clip_to_clip[i] = static_cast<float>((*prev_clip_to_clip)[i]);
    }
    if (!Finite(camera.view_to_clip) || !Finite(camera.clip_to_view) ||
        !Finite(camera.clip_to_prev_clip) || !Finite(camera.prev_clip_to_clip) ||
        !Finite(camera.pos) || !Finite(camera.up) || !Finite(camera.right) ||
        !Finite(camera.fwd)) {
        return {};
    }
    camera.valid = true;
    return camera;
}

} // namespace frame_gen_detail

/// A camera cut between two real frames (NGX wants reset=true there, or it warns "Scene change
/// detected without Reset flag" and blends two shots): either camera invalid, the view direction
/// turns more than 30 degrees, or fov_y changes by more than 20%. The 22 Oct 2026 shortcut: no
/// position test, the games' world units are not known (a teleport with the same view stays a miss);
/// add one when a unit is measured. fov_y is a default for the scene sources (MakeFrameGenCamera
/// leaves it), so that test only fires where a source fills it.
inline bool IsCameraCut(const FrameGenCamera& prev, const FrameGenCamera& cur) {
    if (!prev.valid || !cur.valid) {
        return true;
    }
    const float dot = prev.fwd[0] * cur.fwd[0] + prev.fwd[1] * cur.fwd[1] + prev.fwd[2] * cur.fwd[2];
    constexpr float cos_30_degrees = 0.8660254f;
    return !(dot >= cos_30_degrees) ||
           !(std::abs(cur.fov_y - prev.fov_y) <= 0.2f * prev.fov_y);
}

/// General 4x4 inverse (row-major, double inside); nullopt when it is singular.
inline std::optional<std::array<float, 16>> InvertMatrix4(const std::array<float, 16>& m) {
    frame_gen_detail::Mat4 a;
    std::copy(m.begin(), m.end(), a.begin());
    const auto inverse = frame_gen_detail::Invert(a);
    if (!inverse) {
        return std::nullopt;
    }
    std::array<float, 16> out;
    std::transform(inverse->begin(), inverse->end(), out.begin(),
                   [](double v) { return static_cast<float>(v); });
    return out;
}

/// Scene constants and flat camera. `view_to_world`: this frame's view to world, 3x4 rows [R | t]
/// (column vectors); `view_to_prev_view`: this frame's view to the previous frame's view, the same
/// layout (previous.view * current.inv_view, the shader's `reproject`); `proj`, `prev_proj`:
/// {x scale, y scale, z scale, z offset} of both frames. near/far/fov/aspect stay at their defaults.
inline FrameGenCamera MakeFrameGenCamera(const std::array<float, 12>& view_to_world,
                                         const std::array<float, 12>& view_to_prev_view,
                                         const std::array<float, 4>& proj,
                                         const std::array<float, 4>& prev_proj) {
    using namespace frame_gen_detail;
    const Mat4 view_to_prev_clip =
        Mul(RowVectorAffine(view_to_prev_view),
            Projection(prev_proj[0], prev_proj[1], 0, 0, prev_proj[2], prev_proj[3]));
    return Assemble(Projection(proj[0], proj[1], 0, 0, proj[2], proj[3]), view_to_prev_clip,
                    view_to_world);
}

/// Uncharted's velocity pass (CameraMotion::OnVelocityConstants): `uv_to_view` {c0, c1, c2, c3}
/// (view = (z (c0 u + c2), z (c1 v + c3), z), u right and v down in 0..1), `depth_to_z` {c8, c9}
/// (depth = c8 + c9 / z), `to_previous_clip` the 3x4 rows giving the previous clip x, y and w from
/// this frame's view position, `to_world` the camera pose, rows [M | t] (world = M view + t).
inline FrameGenCamera MakeFrameGenCameraVelocity(const std::array<float, 4>& uv_to_view,
                                                 const std::array<float, 2>& depth_to_z,
                                                 const std::array<float, 12>& to_previous_clip,
                                                 const std::array<float, 12>& to_world) {
    using namespace frame_gen_detail;
    const double c0 = uv_to_view[0], c1 = uv_to_view[1], c2 = uv_to_view[2], c3 = uv_to_view[3];
    if (c0 == 0.0 || c1 == 0.0) {
        return {};
    }
    const double zs = depth_to_z[0], zo = depth_to_z[1];
    // The scales are TaaDepthParameters' (xs = 2 / c0, ys = -2 / c1, zs = c8, zo = c9); the
    // frustum is centred (c0 / 2 + c2 = 0), the offsets keep the matrix exact if it is not.
    const Mat4 view_to_clip =
        Projection(2.0 / c0, -2.0 / c1, -(c0 + 2.0 * c2) / c0, 1.0 + 2.0 * c3 / c1, zs, zo);
    // The previous clip z row is not stored: the projection is the same in both frames, so
    // z = zs * w + zo (w, the previous view z, is the third row).
    Mat4 view_to_prev_clip{};
    for (int i = 0; i < 4; ++i) {
        view_to_prev_clip[i * 4 + 0] = to_previous_clip[i];
        view_to_prev_clip[i * 4 + 1] = to_previous_clip[4 + i];
        view_to_prev_clip[i * 4 + 2] = zs * to_previous_clip[8 + i] + (i == 3 ? zo : 0.0);
        view_to_prev_clip[i * 4 + 3] = to_previous_clip[8 + i];
    }
    return Assemble(view_to_clip, view_to_prev_clip, to_world);
}

} // namespace Vulkan
