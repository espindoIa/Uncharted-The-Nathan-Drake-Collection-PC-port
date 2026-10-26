// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <vector>

// 25 Oct 2026: motion-vector vertex shaders embed this session's buffer device addresses as
// 64-bit OpConstants (EmitVertexMotion). The disk cache stores SPIR-V with these placeholders
// instead, and the loaders patch the live addresses back in.
namespace Vulkan::MotionAddresses {

// Never valid device addresses (ASCII "MOTION" + index).
constexpr std::uint64_t CachedParams = 0x4d4f54494f4e0001ull;
constexpr std::uint64_t CachedPositions = 0x4d4f54494f4e0002ull;

struct Counts {
    std::uint32_t params = 0;
    std::uint32_t positions = 0;
    std::uint32_t derived = 0; // constants derived from a base: the rewrite would leave them stale
    bool malformed = false;
};

// Replaces every 64-bit OpConstant equal to from_params/from_positions by to_params/to_positions.
inline Counts Rewrite(std::vector<std::uint32_t>& spv, std::uint64_t from_params,
                      std::uint64_t from_positions, std::uint64_t to_params,
                      std::uint64_t to_positions) {
    Counts counts;
    constexpr std::uint32_t OpConstant = 43;
    for (std::size_t i = 5; i < spv.size();) {
        const std::uint32_t word_count = spv[i] >> 16;
        if (word_count == 0 || i + word_count > spv.size()) {
            counts.malformed = true;
            break;
        }
        if ((spv[i] & 0xffff) == OpConstant && word_count == 5) {
            const std::uint64_t value = spv[i + 3] | (std::uint64_t{spv[i + 4]} << 32);
            std::uint64_t replacement = 0;
            if (value == from_params) {
                replacement = to_params;
                ++counts.params;
            } else if (value == from_positions) {
                replacement = to_positions;
                ++counts.positions;
            } else {
                if ((from_params != 0 && value - from_params < (1ull << 32)) ||
                    (from_positions != 0 && value - from_positions < (1ull << 32))) {
                    ++counts.derived;
                }
                i += word_count;
                continue;
            }
            spv[i + 3] = static_cast<std::uint32_t>(replacement);
            spv[i + 4] = static_cast<std::uint32_t>(replacement >> 32);
        }
        i += word_count;
    }
    return counts;
}

} // namespace Vulkan::MotionAddresses
