// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <limits>

#include "common/types.h"

namespace Vulkan {

struct SkinningWriteView {
    VAddr address;
    u64 size;
};

// 23 Oct 2026: only the observed +0/+12/+20 interleaved output layout is batched. Its views
// must overlap: the union marks exactly their original bytes, never an unwritten gap. A union
// crossing 4 KiB changes BufferCache's small-write history path, so leave that case unchanged.
constexpr u64 SkinningWriteUnion(const std::array<SkinningWriteView, 3>& views) {
    const auto [address, size] = views[0];
    if (!address || size <= 20 || size > (1ull << 20) - 20 ||
        address > std::numeric_limits<u64>::max() - size - 20 ||
        (size <= 4096 && size + 20 > 4096)) {
        return 0;
    }
    if (views[1].address != address + 12 || views[2].address != address + 20 ||
        views[1].size != size || views[2].size != size) {
        return 0;
    }
    return size + 20;
}

} // namespace Vulkan
