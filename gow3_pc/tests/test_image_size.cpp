// SPDX-License-Identifier: GPL-2.0-or-later
// Guest image sizes saturate instead of wrapping (texture_cache/guest_size.h, used by
// ImageInfo::UpdateSize). Upstream #57, 19 Oct 2026: a garbage T# of 960x540 RGBA8 with 6755
// layers is ~14 GB; in u32 it wrapped below the rasterizer's 1 GB plausibility check and the
// device was lost creating it. No game, Vulkan device or window required.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "video_core/texture_cache/guest_size.h"

using namespace VideoCore;

int main() {
    constexpr std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint64_t one_gb = 1ull << 30;
    // Ordinary sizes are exact.
    constexpr std::uint32_t slice = 1920u * 1080u * 4u;
    static_assert(SaturatingMipSize(slice, 1, 1) == slice);
    static_assert(SaturatingMipSize(slice, 1, 8) == slice * 8u);
    static_assert(SaturatingMipSize(slice, 4, 2) == slice * 8u);
    static_assert(SaturatingAdd(slice, slice) == slice * 2u);
    // The #57 descriptor: 960x540 RGBA8, 6755 layers. With a macro-tiled slice padded to
    // 1024x640 it is 16.5 GiB, which wrapped in u32 to 0.49 GiB: under the 1 GB check.
    constexpr std::uint32_t small_slice = 1024u * 640u * 4u;
    static_assert(std::uint32_t(std::uint64_t(small_slice) * 6755u) < one_gb); // the old wrap
    static_assert(SaturatingMipSize(small_slice, 1, 6755) == max);
    static_assert(SaturatingMipSize(small_slice, 1, 6755) >= one_gb);
    // Sums of mips saturate too.
    static_assert(SaturatingAdd(max - 1, 2) == max);
    static_assert(SaturatingAdd(SaturatingMipSize(small_slice, 1, 6755), small_slice) == max);
    std::puts("image-size-test: ok");
    return 0;
}
