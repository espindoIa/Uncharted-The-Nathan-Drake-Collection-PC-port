// gow3 25 Oct 2026: the upscaler's camera cut decision (shim/gow3_camera_cut.h).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "gow3_camera_cut.h"

using Gow3CameraCut::Detector;

namespace {
constexpr double kFrame = 1.0 / 60.0;

// Forward axis at a heading (degrees, around y).
void Heading(float deg, float fwd[3]) {
    const float r = deg / 57.29578f;
    fwd[0] = std::sin(r), fwd[1] = 0.0f, fwd[2] = std::cos(r);
}

// 60 frames walking 0.05 units a frame while turning 2 degrees a frame: never a cut.
void Walk(Detector& d, float pos[3], float& heading, double& t) {
    float fwd[3];
    for (int i = 0; i < 60; ++i) {
        pos[2] += 0.05f, heading += 2.0f, t += kFrame;
        Heading(heading, fwd);
        assert(!d.Check(pos, fwd, t));
    }
}
} // namespace

int main() {
    float pos[3] = {10.0f, 2.0f, -5.0f}, fwd[3];
    float heading = 0.0f;
    double t = 100.0;
    {
        Detector d;
        Heading(heading, fwd);
        assert(!d.Check(pos, fwd, t)); // the first camera: nothing to compare with
        Walk(d, pos, heading, t);
        // A 35-degree mouse flick in one frame: no cut.
        heading += 35.0f, t += kFrame;
        Heading(heading, fwd);
        assert(!d.Check(pos, fwd, t));
        Walk(d, pos, heading, t);
        // The U3 chapter 2 Continue frame: 161 degrees and 106 units: cut.
        heading += 161.0f, t += kFrame;
        pos[0] += 106.0f;
        Heading(heading, fwd);
        assert(d.Check(pos, fwd, t));
        // Walking on from there: no cut (the cut's move stays out of the median).
        Walk(d, pos, heading, t);
        // A turn alone over 45 degrees: cut; a move alone of 106 units: cut.
        heading += 50.0f, t += kFrame;
        Heading(heading, fwd);
        assert(d.Check(pos, fwd, t));
        Walk(d, pos, heading, t);
        pos[1] += 106.0f, t += kFrame;
        Heading(heading, fwd);
        assert(d.Check(pos, fwd, t));
        Walk(d, pos, heading, t);
        // Same camera after 0.6 s without camera frames (menu, movie, load): cut; after 0.4 s not.
        t += 0.6;
        assert(d.Check(pos, fwd, t));
        t += 0.4;
        assert(!d.Check(pos, fwd, t));
        // The relative rule: walking 0.05 a frame, the limit is the 1.0 floor (16 x 0.05 = 0.8).
        assert(std::abs(d.MoveLimit() - 1.0f) < 1e-6f);
        // A camera without a usable forward axis is ignored.
        const float zero[3] = {};
        assert(!d.Check(pos, zero, t + kFrame));
    }
    {
        // A vehicle at 0.5 units a frame: limit 8 units, so 0.9 is no cut, 9 is.
        Detector d;
        Heading(0.0f, fwd);
        for (int i = 0; i < 40; ++i) {
            pos[2] += 0.5f, t += kFrame;
            assert(!d.Check(pos, fwd, t));
        }
        assert(std::abs(d.MoveLimit() - 8.0f) < 1e-4f);
        pos[2] += 0.9f, t += kFrame;
        assert(!d.Check(pos, fwd, t));
        pos[2] += 9.0f, t += kFrame;
        assert(d.Check(pos, fwd, t));
    }
    {
        // GOW3_CUT_MOVE: a positive number replaces the floor, anything else keeps 1.0.
        _putenv("GOW3_CUT_MOVE=4");
        assert(Gow3CameraCut::MoveFloorFromEnv() == 4.0f);
        _putenv("GOW3_CUT_MOVE=-3");
        assert(Gow3CameraCut::MoveFloorFromEnv() == 1.0f);
        _putenv("GOW3_CUT_MOVE=abc");
        assert(Gow3CameraCut::MoveFloorFromEnv() == 1.0f);
        Detector d{4.0f};
        Heading(0.0f, fwd);
        assert(!d.Check(pos, fwd, t));
        pos[0] += 3.0f, t += kFrame;
        assert(!d.Check(pos, fwd, t)); // 3 units under the 4-unit floor
    }
    std::printf("camera-cut-test: ok\n");
    return 0;
}
