// Checks the SPIR-V address rewrite behind the motion-vector shader disk cache.
#include <cassert>
#include <cstdio>
#include <vector>

#include "video_core/renderer_vulkan/vk_motion_addresses.h"

using namespace Vulkan::MotionAddresses;

static void Const64(std::vector<std::uint32_t>& spv, std::uint64_t value) {
    spv.insert(spv.end(), {(5u << 16) | 43u, 1u, 2u, static_cast<std::uint32_t>(value),
                           static_cast<std::uint32_t>(value >> 32)});
}

int main() {
    const std::uint64_t params = 0x00007f1200100000ull, positions = 0x00007f1200900000ull;
    std::vector<std::uint32_t> spv{0x07230203u, 0x10600u, 0u, 100u, 0u};
    Const64(spv, params);
    Const64(spv, positions);
    Const64(spv, params + 16);
    spv.insert(spv.end(), {(4u << 16) | 43u, 1u, 2u, 0x1234u}); // 32-bit constant
    Const64(spv, 0x1122334455667788ull);
    const auto original = spv;

    const Counts save = Rewrite(spv, params, positions, CachedParams, CachedPositions);
    assert(save.params == 1 && save.positions == 1 && save.derived == 1 && !save.malformed);
    // Constants start at words 5, 10, 15 (derived), 20 (32-bit), 24 (unrelated); literal at +3.
    assert(spv[8] == static_cast<std::uint32_t>(CachedParams) && spv[9] == CachedParams >> 32);
    assert(spv[13] == static_cast<std::uint32_t>(CachedPositions) && spv[14] == CachedPositions >> 32);
    for (const int word : {18, 19, 23, 27, 28}) { // derived, 32-bit, unrelated: untouched
        assert(spv[word] == original[word]);
    }

    const Counts load = Rewrite(spv, CachedParams, CachedPositions, params, positions);
    assert(load.params == 1 && load.positions == 1 && load.derived == 0 && !load.malformed);
    assert(spv == original);

    std::vector<std::uint32_t> bad{0x07230203u, 0x10600u, 0u, 100u, 0u, 0u};
    assert(Rewrite(bad, params, positions, 0, 0).malformed);

    std::puts("motion-addresses-test: OK");
    return 0;
}
