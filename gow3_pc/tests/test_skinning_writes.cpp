// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <limits>

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
}
