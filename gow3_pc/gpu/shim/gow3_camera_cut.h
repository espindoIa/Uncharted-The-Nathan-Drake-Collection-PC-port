// gow3 25 Oct 2026: camera cut detection for the temporal upscaler (vk_camera_motion.cpp). The
// flight recorder saw U3 chapter 2's first scene frame after Continue turn the camera 161 degrees
// and move it 106 units with no upscaler reset, so DLSS/FSR/TAA blended the image from before
// the cut. Header-only and free of Vulkan so tests/test_camera_cut.cpp can drive it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace Gow3CameraCut {

/// Thresholds. Conservative: a mouse flick of up to ~30 degrees a frame at 60 FPS is no cut.
inline constexpr float kCutDeg = 45.0f;       ///< forward axis turn in one camera frame
inline constexpr float kMoveFactor = 16.0f;   ///< x the median move of the last 32 camera frames
inline constexpr float kMoveFloor = 1.0f;     ///< GOW3_CUT_MOVE default (world units)
inline constexpr double kStaleSeconds = 0.5;  ///< previous camera older than this (menu, movie, load)

/// GOW3_CUT_MOVE=<units>: the move floor; a value that is not a positive number keeps 1.0.
inline float MoveFloorFromEnv() {
    const char* text = std::getenv("GOW3_CUT_MOVE");
    if (!text || !text[0]) return kMoveFloor;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    return end != text && std::isfinite(value) && value > 0.0f ? value : kMoveFloor;
}

class Detector {
public:
    explicit Detector(float move_floor = kMoveFloor) : floor{move_floor} {}

    /// The frame's camera: position, forward axis (unit length expected) and the time in seconds.
    /// Returns whether it is a cut from the previous camera given here. A camera without a usable
    /// forward axis is ignored (no cut, not remembered); the first one is no cut (no history yet).
    bool Check(const float pos[3], const float fwd[3], double t) {
        const float len = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (!(len > 0.5f && len < 2.0f) || !std::isfinite(pos[0] + pos[1] + pos[2])) {
            return false;
        }
        bool cut = false;
        if (have_prev) {
            const float dot = (fwd[0] * prev_fwd[0] + fwd[1] * prev_fwd[1] + fwd[2] * prev_fwd[2]) /
                              (len * prev_len);
            const float deg = std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.29578f;
            const float dx = pos[0] - prev_pos[0], dy = pos[1] - prev_pos[1], dz = pos[2] - prev_pos[2];
            const float move = std::sqrt(dx * dx + dy * dy + dz * dz);
            cut = t - prev_t > kStaleSeconds || deg > kCutDeg || move > MoveLimit();
            // A cut's move is not walking speed: kept out of the median.
            if (!cut) moves[move_n++ % 32] = move;
        }
        std::copy_n(pos, 3, prev_pos);
        std::copy_n(fwd, 3, prev_fwd);
        prev_len = len;
        prev_t = t;
        have_prev = true;
        return cut;
    }

    /// The move over which a camera frame is a cut: max(floor, 16x the median of the last 32).
    float MoveLimit() const {
        if (move_n == 0) return floor;
        float tmp[32];
        const std::uint32_t n = std::min<std::uint32_t>(move_n, 32);
        std::copy_n(moves, n, tmp);
        std::nth_element(tmp, tmp + n / 2, tmp + n);
        return std::max(floor, kMoveFactor * tmp[n / 2]);
    }

private:
    float floor;
    float prev_pos[3]{}, prev_fwd[3]{}, prev_len = 1.0f;
    double prev_t = 0.0;
    bool have_prev = false;
    float moves[32]{};
    std::uint32_t move_n = 0;
};

} // namespace Gow3CameraCut
