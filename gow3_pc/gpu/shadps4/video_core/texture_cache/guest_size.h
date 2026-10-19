// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace VideoCore {

/// gow3 19 Oct 2026 (upstream #57): guest image sizes saturate at u32 max instead of wrapping.
/// A garbage T# of 960x540 RGBA8 with 6755 layers (Drake's Fortune after 26 minutes) is ~16 GB
/// (a 1024x640 macro-tiled slice); in u32 it wrapped to 0.49 GiB, which passed the rasterizer's
/// 1 GB plausibility check: the texture cache created a 15.8 GB image and the device was lost.
/// Saturated, it is always rejected.
[[nodiscard]] constexpr std::uint32_t SaturatingMipSize(std::uint32_t slice_size, std::uint32_t depth,
                                                       std::uint32_t layers) noexcept {
    const std::uint64_t size = std::uint64_t(slice_size) * depth * layers;
    return std::uint32_t(std::min<std::uint64_t>(size, std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] constexpr std::uint32_t SaturatingAdd(std::uint32_t a, std::uint32_t b) noexcept {
    return std::uint32_t(std::min<std::uint64_t>(std::uint64_t(a) + b,
                                                 std::numeric_limits<std::uint32_t>::max()));
}

} // namespace VideoCore
