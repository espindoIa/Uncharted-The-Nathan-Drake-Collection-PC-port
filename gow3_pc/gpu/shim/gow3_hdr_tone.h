// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Scene HDR (26 Oct 2026): the HDR10 tone pass shadow shader, derived at run time from the
// player's own game. The games' tone pass pixel shader (guest hash 3371ae8d) is Naughty Dog /
// Sony Interactive Entertainment code, so nothing of it is distributed: the shader recompiler
// translates it from the game files as for any shader, and MakeShadowShader links the port's own
// epilogue (hdr/hdr_tone.spv: RenoDX tone mapping, MIT, plus the port's anchoring) into that
// SPIR-V. Pure SPIR-V work (SPIRV-Tools), no Vulkan; tests/test_hdr_tone.cpp.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Gow3HdrTone {

// The tone pass: hash 0x3371ae8d in all three games.
constexpr uint32_t ToneShaderHash = 0x3371ae8d;

// Whether the translated shader (its OpString names the guest hash) is the tone pass.
bool IsToneShader(std::span<const uint32_t> code);

// The translated tone pass (manual interpolation: barycentrics) with the epilogue called on its
// result: same bindings and push constants, plus the settings buffer at binding 12, and the
// HDR result gamma 2.2 encoded in place of the game's SDR color. Empty with `problem` set when
// the translated shader is not in the expected form (a different recompiler version, another
// interpolation path) or the link fails: the caller keeps the SDR scene.
std::vector<uint32_t> MakeShadowShader(std::span<const uint32_t> guest,
                                       std::span<const uint32_t> epilogue, std::string& problem);

// Runtime side. Remember is called with every module the pipeline cache creates and keeps the
// tone pass's SPIR-V (a few KB per permutation, whatever the HDR setting); Shadow derives the shadow shader for that module once
// (reading hdr/hdr_tone.spv next to the executable, GOW3_HDR_SHADER_DIR overrides) and returns
// it, or empty after logging why not.
void Remember(uint64_t module, std::span<const uint32_t> code);
std::vector<uint32_t> Shadow(uint64_t module);

} // namespace Gow3HdrTone
