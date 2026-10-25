// SPDX-License-Identifier: GPL-2.0-or-later
// The DLSS mode Streamline gets for the port's render sizes (gow3_streamline_mode.h): the mode's
// optimal size must hold the input and its minimum must not exceed it, and the smallest such
// optimal size wins. Sizes as NGX reports them for a 4K output (optimal = output / ratio; the
// minimum is that of dynamic resolution, here half the optimal; DLAA's minimum is varied).
#include <cstdio>
#include <cstdlib>
#include "gow3_streamline_mode.h"

namespace {

using Gow3Streamline::ModeSizes;
using Gow3Streamline::PickDlssMode;

int failures = 0;

void Expect(const char* label, int got, int want) {
    if (got != want) {
        std::printf("FAIL %s: mode %d, want %d\n", label, got, want);
        ++failures;
    }
}

ModeSizes Mode(double ratio, double min_fraction = 0.5) {
    const auto w = uint32_t(3840 / ratio), h = uint32_t(2160 / ratio);
    return {w, h, uint32_t(w * min_fraction), uint32_t(h * min_fraction)};
}

} // namespace

int main() {
    // DLAA, Quality 1.5, Balanced 1.724, Performance 2, Ultra Performance 3.
    ModeSizes modes[5] = {Mode(1.0), Mode(1.5), Mode(1.724), Mode(2.0), Mode(3.0)};
    Expect("native 4K is DLAA", PickDlssMode(modes, 3840, 2160), 0);
    Expect("Quality's own size is Quality", PickDlssMode(modes, 2560, 1440), 1);
    Expect("Performance's own size", PickDlssMode(modes, 1920, 1080), 3);
    Expect("Ultra Performance's own size", PickDlssMode(modes, 1280, 720), 4);
    // The port's Balanced is 1/1.7: a little larger than NGX's Balanced optimal, so Quality's
    // feature (which holds it) runs it as a sub-rectangle.
    Expect("1/1.7 is above Balanced's optimal", PickDlssMode(modes, 2258, 1270), 1);
    // Ultra Quality 1/1.4 and Drake's Fortune's 2880x1620 cap fit only DLAA's feature.
    Expect("1/1.4 needs DLAA's size", PickDlssMode(modes, 2742, 1542), 0);
    Expect("2880x1620 needs DLAA's size", PickDlssMode(modes, 2880, 1620), 0);
    // When DLAA allows no smaller input (minimum = optimal), those have no mode.
    modes[0] = Mode(1.0, 1.0);
    Expect("1/1.4 without DLAA's range", PickDlssMode(modes, 2742, 1542), -1);
    Expect("native still DLAA", PickDlssMode(modes, 3840, 2160), 0);
    // A mode NGX did not report (optimal 0) is left out.
    modes[1] = {};
    Expect("Quality missing: Balanced's range is too small", PickDlssMode(modes, 2560, 1440), -1);
    Expect("below every minimum", PickDlssMode(modes, 100, 100), -1);
    // A wider-than-16:9 input (ultrawide output with the same height) is held only if both sides fit.
    ModeSizes wide[5] = {Mode(1.0), Mode(1.5), Mode(1.724), Mode(2.0), Mode(3.0)};
    Expect("wider than the optimal size", PickDlssMode(wide, 2600, 1400), 0);
    if (failures) {
        return 1;
    }
    std::puts("streamline-mode-test: all passed");
    return 0;
}
