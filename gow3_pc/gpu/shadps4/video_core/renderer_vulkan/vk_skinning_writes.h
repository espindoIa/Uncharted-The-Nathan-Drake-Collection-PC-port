// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <limits>

#include "common/types.h"

namespace Vulkan {

struct SkinningWriteView {
    VAddr address;
    u64 size;
};

/// The bytes of the observed +0/+12/+20 interleaved output layout (equal sizes), or 0.
constexpr u64 SkinningWriteLayout(const std::array<SkinningWriteView, 3>& views) {
    const auto [address, size] = views[0];
    if (!address || size <= 20 || size > (1ull << 20) - 20 ||
        address > std::numeric_limits<u64>::max() - size - 20) {
        return 0;
    }
    if (views[1].address != address + 12 || views[2].address != address + 20 ||
        views[1].size != size || views[2].size != size) {
        return 0;
    }
    return size + 20;
}

// 23 Oct 2026: only the observed +0/+12/+20 interleaved output layout is batched. Its views
// must overlap: the union marks exactly their original bytes, never an unwritten gap. A union
// crossing 4 KiB changes BufferCache's small-write history path, so leave that case unchanged.
constexpr u64 SkinningWriteUnion(const std::array<SkinningWriteView, 3>& views) {
    const u64 bytes = SkinningWriteLayout(views);
    return bytes && !(views[0].size <= 4096 && bytes > 4096) ? bytes : 0;
}

/// One crowd dispatch of a run (Gow3Toggle::DispatchRuns): its three output views and the bytes
/// of each the shader can write (the barrier tracker's exact strided extent, <= the view size).
struct SkinningWriteInstance {
    std::array<SkinningWriteView, 3> views;
    std::array<u64, 3> tracked;
};

/// Consecutive instances whose output unions overlap or touch: one buffer acquisition. `address`
/// and `size` are exactly the union of their views (what BufferCache marks GPU-written, view by
/// view on the old path); `track_*` is the hull of their tracked bytes, for the barrier tracker.
/// `per_view`: an instance's views are 4 KiB or less (BufferCache's small-write history works
/// per view) or over 64 KiB (BufferCache::NoteGpuWriteWindows stamps such a write's 1 MiB
/// windows), so every view of the span is acquired on its own as before.
struct SkinningWriteSpan {
    VAddr address;
    u64 size;
    VAddr track_address;
    u64 track_size;
    bool per_view;
};

enum class SkinningSpanRefusal { None, Layout, Overflow, Overlap, Window, TooMany };

/// Above this a GPU write stamps the tick of its whole 1 MiB windows
/// (BufferCache::NoteGpuWriteWindows); up to it, only its pages' ticks.
constexpr u64 SkinningWriteChunkMax = 64 * 1024;

/// 24 Oct 2026 review: a span over 64 KiB built from views of at most 64 KiB is acquired in
/// `count` pieces of `bytes` (the last one the rest): ceil(size / 64 KiB) pieces of
/// ceil(size / count) bytes, so none stamps a window the views would not have, and none is
/// 4 KiB or less (the small-write path). Each piece is at least 32 KiB.
struct SkinningWriteChunks {
    u32 count;
    u64 bytes;
};
constexpr SkinningWriteChunks SplitSkinningWrite(u64 size) {
    const u64 count = size <= SkinningWriteChunkMax
                          ? 1
                          : (size + SkinningWriteChunkMax - 1) / SkinningWriteChunkMax;
    return {static_cast<u32>(count), (size + count - 1) / count};
}

/// 24 Oct 2026: the write spans of a run of up to 64 crowd dispatches, or why it must be recorded
/// one dispatch at a time. The 21 Oct dispatch trace has each instance's +0/+12/+20 union run
/// 20 bytes past its array and the next array start 4-16 bytes after that array (16-byte
/// aligned): the unions overlap, so they join one span (a union of overlapping intervals adds no
/// byte). Two dispatches of one batch run in any order, so the bytes they can write (tracked)
/// must not overlap: in address order, each instance starts at or after the previous tracked
/// end. Views of 4 KiB or less take BufferCache's small-write path view by view: their span is
/// acquired view by view (per_view; runv2, 24 Oct: ~30% of the U3 crowd instances). A span may
/// not cross a 1 GiB line (BufferCache::MIN_ARENA_PAGE_BITS): one arena, as for each view.
/// `span_of[i]`: the span of instance i (count <= 64). Spans come out in address order.
inline SkinningSpanRefusal BuildSkinningWriteSpans(const SkinningWriteInstance* instances,
                                                   u32 count, SkinningWriteSpan* spans,
                                                   u32 max_spans, u32& num_spans, u8* span_of) {
    constexpr u64 Window = 1ull << 30;
    num_spans = 0;
    if (count > 64) {
        return SkinningSpanRefusal::TooMany;
    }
    // Address order (insertion sort: at most 64): the chunk's order need not be the memory's.
    std::array<u8, 64> order{};
    for (u32 i = 0; i < count; ++i) {
        u32 at = i;
        for (; at > 0 && instances[order[at - 1]].views[0].address > instances[i].views[0].address;
             --at) {
            order[at] = order[at - 1];
        }
        order[at] = static_cast<u8>(i);
    }
    VAddr track_end = 0;
    constexpr u64 Max = std::numeric_limits<u64>::max();
    for (u32 n = 0; n < count; ++n) {
        const u32 i = order[n];
        const auto& views = instances[i].views;
        if (views[0].size > Max - 20 || views[0].address > Max - views[0].size - 20) {
            return SkinningSpanRefusal::Overflow;
        }
        const u64 bytes = SkinningWriteLayout(views);
        if (!bytes) {
            return SkinningSpanRefusal::Layout;
        }
        const bool small = views[0].size <= 4096 || views[0].size > SkinningWriteChunkMax;
        const VAddr address = views[0].address;
        VAddr tracked_end = address;
        for (u32 v = 0; v < 3; ++v) {
            const u64 tracked = instances[i].tracked[v];
            if (tracked == 0 || tracked > views[v].size) {
                return SkinningSpanRefusal::Layout;
            }
            tracked_end = std::max<VAddr>(tracked_end, views[v].address + tracked);
        }
        if (address < track_end) {
            return SkinningSpanRefusal::Overlap;
        }
        track_end = tracked_end;
        auto* span = num_spans ? &spans[num_spans - 1] : nullptr;
        if (span && address <= span->address + span->size) {
            span->size = std::max<u64>(span->size, address + bytes - span->address);
            span->track_size = tracked_end - span->track_address;
            span->per_view |= small;
        } else {
            if (num_spans == max_spans) {
                return SkinningSpanRefusal::TooMany;
            }
            span = &spans[num_spans++];
            *span = {address, bytes, address, tracked_end - address, small};
        }
        if (span->address / Window != (span->address + span->size - 1) / Window) {
            return SkinningSpanRefusal::Window;
        }
        span_of[i] = static_cast<u8>(num_spans - 1);
    }
    return SkinningSpanRefusal::None;
}

} // namespace Vulkan
