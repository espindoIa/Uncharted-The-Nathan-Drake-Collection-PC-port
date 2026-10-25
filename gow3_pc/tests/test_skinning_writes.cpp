// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>

#include "video_core/renderer_vulkan/vk_skinning_writes.h"

int main() {
    using Vulkan::SkinningWriteUnion;
    using Vulkan::SkinningWriteView;
    auto views = [](u64 address, u64 size) {
        return std::array<SkinningWriteView, 3>{{{address, size}, {address + 12, size},
                                               {address + 20, size}}};
    };
    assert(SkinningWriteUnion(views(0x10000, 784)) == 804);
    assert(SkinningWriteUnion(views(0x10000, 4076)) == 4096);
    assert(SkinningWriteUnion(views(0x10000, 4077)) == 0);
    assert(SkinningWriteUnion(views(0x10000, 4096)) == 0);
    assert(SkinningWriteUnion(views(0x10000, 4097)) == 4117);
    assert(SkinningWriteUnion(views(0x10000, (1ull << 20) - 20)) == (1ull << 20));
    assert(SkinningWriteUnion(views(0x10000, 1ull << 20)) == 0);
    assert(SkinningWriteUnion(views(0, 784)) == 0);
    assert(SkinningWriteUnion(views(0x10000, 0)) == 0);
    assert(SkinningWriteUnion(views(0x10000, 20)) == 0);
    assert(SkinningWriteUnion(views(0x10000, 21)) == 41);
    assert(SkinningWriteUnion(views(std::numeric_limits<u64>::max() - 800, 784)) == 0);
    auto bad = views(0x10000, 784);
    bad[1].address += 4;
    assert(SkinningWriteUnion(bad) == 0);
    bad = views(0x10000, 784);
    bad[2].size -= 1;
    assert(SkinningWriteUnion(bad) == 0);
    // Every byte the union would mark was already covered by at least one original view.
    const auto good = views(0x10000, 784);
    for (u64 offset = 0; offset < SkinningWriteUnion(good); ++offset) {
        bool covered = false;
        for (const auto& view : good) {
            covered |= good[0].address + offset >= view.address &&
                       good[0].address + offset - view.address < view.size;
        }
        assert(covered);
    }
    std::puts("skinning write union: OK");

    // Dispatch-run spans (BuildSkinningWriteSpans).
    using Vulkan::BuildSkinningWriteSpans;
    using Vulkan::SkinningSpanRefusal;
    using Vulkan::SkinningWriteInstance;
    using Vulkan::SkinningWriteSpan;
    // Stride 28, extents 12/8/8 as in the 21 Oct trace: tracked (n - 1) * 28 + extent.
    auto instance = [&](u64 address, u64 size) {
        const u64 records = size / 28;
        return SkinningWriteInstance{views(address, size),
                                     {(records - 1) * 28 + 12, (records - 1) * 28 + 8,
                                      (records - 1) * 28 + 8}};
    };
    std::array<SkinningWriteSpan, 4> spans{};
    std::array<u8, 64> span_of{};
    u32 num_spans = 99;
    auto build = [&](std::initializer_list<SkinningWriteInstance> list, u32 max_spans = 4) {
        const std::vector<SkinningWriteInstance> run(list);
        return BuildSkinningWriteSpans(run.data(), u32(run.size()), spans.data(), max_spans,
                                       num_spans, span_of.data());
    };
    // The trace's first two instances: 393 records, the next array 16-byte aligned. Their unions
    // overlap by 16 bytes (20 past the array, 4 of padding): one span, exactly the union.
    const VAddr base = 0x1100000000;
    assert(build({instance(base, 11004), instance(base + 0x2b00, 11004)}) ==
           SkinningSpanRefusal::None);
    assert(num_spans == 1 && span_of[0] == 0 && span_of[1] == 0);
    assert(spans[0].address == base && spans[0].size == 0x2b00 + 11024);
    assert(spans[0].track_address == base && spans[0].track_size == 0x2b00 + 11004);
    // Exactly adjacent unions: one span of both.
    assert(build({instance(base, 11004), instance(base + 11024, 11004)}) ==
           SkinningSpanRefusal::None);
    assert(num_spans == 1 && spans[0].size == 2 * 11024);
    // A gap: no byte of it becomes GPU-written, two spans.
    assert(build({instance(base, 11004), instance(base + 11024 + 16, 11004),
                  instance(base + 2 * 11024 + 16, 11004)}) == SkinningSpanRefusal::None);
    assert(num_spans == 2 && span_of[0] == 0 && span_of[1] == 1 && span_of[2] == 1);
    assert(spans[0].size == 11024 && spans[1].address == base + 11024 + 16 &&
           spans[1].size == 2 * 11024);
    assert(build({instance(base, 11004), instance(base + 11024 + 16, 11004)}, 1) ==
           SkinningSpanRefusal::TooMany);
    // Tracked bytes overlap (the next instance starts inside them), in either chunk order, or
    // two instances write the same array.
    assert(build({instance(base, 11004), instance(base + 11000, 11004)}) ==
           SkinningSpanRefusal::Overlap);
    assert(build({instance(base + 11000, 11004), instance(base, 11004)}) ==
           SkinningSpanRefusal::Overlap);
    assert(build({instance(base, 11004), instance(base, 11004)}) == SkinningSpanRefusal::Overlap);
    // Chunk order is not address order: spans in address order, span_of by chunk index.
    assert(build({instance(base + 0x2b00, 11004), instance(base, 11004)}) ==
           SkinningSpanRefusal::None);
    assert(num_spans == 1 && spans[0].address == base && span_of[0] == 0 && span_of[1] == 0);
    assert(build({instance(base + 11024 + 16, 11004), instance(base, 11004)}) ==
           SkinningSpanRefusal::None);
    assert(num_spans == 2 && span_of[0] == 1 && span_of[1] == 0 && spans[0].address == base);
    // Starting exactly at the previous tracked end is allowed.
    assert(build({instance(base, 11004), instance(base + 11004, 11004)}) ==
           SkinningSpanRefusal::None);
    // Small views take the small-write path view by view: their span is acquired per view.
    assert(build({instance(base, 4088)}) == SkinningSpanRefusal::None);
    assert(num_spans == 1 && spans[0].per_view && spans[0].size == 4108);
    assert(build({instance(base, 11004), instance(base + 0x2b00, 4088),
                  instance(base + 0x2b00 + 4112 + 64, 11004)}) == SkinningSpanRefusal::None);
    assert(num_spans == 2 && spans[0].per_view && !spans[1].per_view && span_of[1] == 0);
    assert(build({instance(base, 4116)}) == SkinningSpanRefusal::None && !spans[0].per_view);
    // Views over 64 KiB stamp whole 1 MiB windows: acquired view by view, as before.
    assert(build({instance(base, 65520)}) == SkinningSpanRefusal::None && !spans[0].per_view);
    assert(build({instance(base, 65548)}) == SkinningSpanRefusal::None && spans[0].per_view);

    // Pieces of a span over 64 KiB: each over 4 KiB (in fact at least 32 KiB), at most 64 KiB,
    // together exactly the span.
    using Vulkan::SplitSkinningWrite;
    assert(SplitSkinningWrite(4116).count == 1 && SplitSkinningWrite(4116).bytes == 4116);
    assert(SplitSkinningWrite(65536).count == 1);
    assert(SplitSkinningWrite(65537).count == 2 && SplitSkinningWrite(65537).bytes == 32769);
    for (u64 size : {65537ull, 98304ull, 131072ull, 131073ull, 196609ull, 1ull << 20,
                     (1ull << 20) + 1, 64ull * 11024, 64ull * 65536, 64ull * ((1ull << 20) - 20)}) {
        const auto [count, bytes] = SplitSkinningWrite(size);
        assert(count >= 2 && bytes <= 65536 && bytes >= 32768);
        const u64 last = size - u64(count - 1) * bytes;
        assert(u64(count - 1) * bytes < size && last <= bytes && last >= 32768);
    }
    for (u64 size = 4097; size < 600000; size += 997) {
        const auto [count, bytes] = SplitSkinningWrite(size);
        const u64 last = size - u64(count - 1) * bytes;
        assert(bytes <= 65536 && last > 4096 && last <= bytes);
        assert(count == 1 || last >= 32768);
    }
    // Overflow, broken layout, tracked bytes outside a view or none.
    assert(build({instance(std::numeric_limits<u64>::max() - 11010, 11004)}) ==
           SkinningSpanRefusal::Overflow);
    auto broken = instance(base, 11004);
    broken.views[1].address += 4;
    assert(build({broken}) == SkinningSpanRefusal::Layout);
    broken = instance(base, 11004);
    broken.tracked[2] = 11005;
    assert(build({broken}) == SkinningSpanRefusal::Layout);
    broken.tracked[2] = 0;
    assert(build({broken}) == SkinningSpanRefusal::Layout);
    // A span may not cross a 1 GiB line (one arena, as for each view).
    const VAddr line = 0x1140000000;
    assert(build({instance(line - 11024, 11004)}) == SkinningSpanRefusal::None);
    assert(build({instance(line - 11010, 11004)}) == SkinningSpanRefusal::Window);
    assert(build({instance(line - 2 * 11024, 11004), instance(line - 11024 + 8, 11004)}) ==
           SkinningSpanRefusal::Window);
    // The spans hold exactly the views' bytes: none outside a view, every view byte inside.
    {
        // b's union starts 8 bytes inside a's (tracked bytes disjoint); c leaves a gap.
        const auto a = instance(0x10000, 4116), b = instance(0x10000 + 4128, 4116),
                   c = instance(0x10000 + 4128 + 4136 + 64, 4116);
        assert(build({a, b, c}) == SkinningSpanRefusal::None && num_spans == 2);
        const auto in_view = [&](VAddr at) {
            for (const auto* item : {&a, &b, &c}) {
                for (const auto& view : item->views) {
                    if (at >= view.address && at - view.address < view.size) {
                        return true;
                    }
                }
            }
            return false;
        };
        const auto in_span = [&](VAddr at) {
            for (u32 s = 0; s < num_spans; ++s) {
                if (at >= spans[s].address && at - spans[s].address < spans[s].size) {
                    return true;
                }
            }
            return false;
        };
        for (VAddr at = 0x10000 - 64; at < 0x10000 + 4128 + 4136 + 64 + 4136 + 64; ++at) {
            assert(in_view(at) == in_span(at));
        }
    }
    std::puts("skinning write spans: OK");
}
