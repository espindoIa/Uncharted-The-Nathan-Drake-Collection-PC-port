// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 23 Oct 2026: the DLSS mode Streamline gets for a render size (gow3_streamline.cpp). Pure, no
// Vulkan or Streamline types: tests/test_streamline_mode.cpp checks it.
//
// Streamline's sl.dlss creates the NGX feature at the mode's optimal render size
// (slDLSSGetOptimalSettings) and runs a smaller input as a sub-rectangle of it, which NGX accepts
// down to the mode's minimum (dynamic resolution). The NGX bridge instead creates the feature at the
// exact input size. The port's own render sizes do not always match a mode's ratio (Ultra Quality
// 1/1.4, Balanced 1/1.7, Drake's Fortune capped at 2880x1620 for a 4K output), so the mode is picked
// from the sizes NGX reports instead of from the ratio (Dlss::QualityForScale).

#pragma once

#include <cstdint>

namespace Gow3Streamline {

/// A mode's optimal and minimum render size; optimal 0 when the mode is not offered.
struct ModeSizes {
    uint32_t optimal_w, optimal_h, min_w, min_h;
};

/// Index 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance. The mode whose optimal
/// size holds the input and whose minimum does not exceed it; of those, the smallest optimal size
/// (the least sub-rectangle work). -1 when none does.
inline int PickDlssMode(const ModeSizes (&modes)[5], uint32_t input_w, uint32_t input_h) {
    int best = -1;
    for (int m = 0; m < 5; ++m) {
        const ModeSizes& s = modes[m];
        if (s.optimal_w == 0 || s.optimal_h == 0) {
            continue;
        }
        const bool holds = s.optimal_w >= input_w && s.optimal_h >= input_h;
        const bool above_min = s.min_w <= input_w && s.min_h <= input_h;
        if (!holds || !above_min) {
            continue;
        }
        if (best < 0 || uint64_t(s.optimal_w) * s.optimal_h <
                            uint64_t(modes[best].optimal_w) * modes[best].optimal_h) {
            best = m;
        }
    }
    return best;
}

} // namespace Gow3Streamline
