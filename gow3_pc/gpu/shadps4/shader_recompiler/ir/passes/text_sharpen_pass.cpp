// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 25 Oct 2026: sharp text above 1080p. The three games draw their text (menus, subtitles,
// HUD) from BMFont bitmap atlases made for 1080p (U3 main.fnt: Adelon-Serial 42 px, caps about
// 31 texels high in a 256x256 RGBA atlas, one glyph per channel). At a 4K render size the game
// scales the text 2x, so bilinear filtering spreads every edge over 3-4 pixels (frame dump of the
// U3 pause menu at 3840x2160: "Options" caps about 70 px high, soft edges). The text shader
// (ps 2e403aa4, the same in U1, U2 and U3) reads the atlas four times: the pixel, +-0.375 pixel
// for its subpixel colour fringes and a drop shadow 1.5 pixels away. Each coverage read here is
// remapped around 0.5 with a slope of the on-screen magnification (pixels per atlas texel, from
// the sampling LOD), so an edge takes about one pixel again, as at 1080p. At 1080p and below
// (one texel per pixel or less) the slope is 1 and the text is unchanged.

#include <algorithm>
#include <cstdlib>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

// GOW3_TEXT_SHARPEN: unset = the games' text shader, 0 = off, or hex hashes separated by commas.
bool TextSharpenShader(u64 pgm_hash) {
    static const std::vector<u64> hashes = [] {
        const char* env = std::getenv("GOW3_TEXT_SHARPEN");
        if (!env) {
            return std::vector<u64>{0x2e403aa4ULL};
        }
        std::vector<u64> list;
        for (const char* s = env; s && *s;) {
            char* next{};
            const u64 hash = std::strtoull(s, &next, 16);
            if (hash > 1) {
                list.push_back(hash);
            }
            s = next && *next == ',' ? next + 1 : nullptr;
        }
        return list;
    }();
    return std::ranges::find(hashes, pgm_hash) != hashes.end();
}

void TextSharpenPass(IR::Program& program) {
    if (program.info.hw_stage != HwStage::Fragment || !TextSharpenShader(program.info.pgm_hash)) {
        return;
    }
    for (IR::Block* const block : program.blocks) {
        for (auto it = block->Instructions().begin(); it != block->Instructions().end(); ++it) {
            IR::Inst& sample = *it;
            if (sample.GetOpcode() != IR::Opcode::ImageSampleImplicitLod || !sample.HasUses()) {
                continue;
            }
            // The users before the new instructions below become users too.
            const std::vector<IR::Use> users(sample.Uses().begin(), sample.Uses().end());
            IR::IREmitter ir{*block, std::next(it)};
            const auto handle = sample.Arg(0);
            const auto info = sample.Flags<IR::TextureInstInfo>();
            // Unclamped LOD = log2(texels per pixel): magnification = 2^-lod, at least 1.
            const IR::F32 lod{ir.CompositeExtract(ir.ImageQueryLod(handle, sample.Arg(1), info), 1)};
            const IR::F32 slope = ir.FPMax(ir.FPExp2(ir.FPNeg(lod)), ir.Imm32(1.0f));
            IR::Value sharp[4];
            for (u32 i = 0; i < 4; ++i) {
                const IR::F32 coverage{ir.CompositeExtract(IR::Value{&sample}, i)};
                sharp[i] = ir.FPClamp(ir.FPFma(ir.FPSub(coverage, ir.Imm32(0.5f)), slope, ir.Imm32(0.5f)),
                                      ir.Imm32(0.0f), ir.Imm32(1.0f));
            }
            const IR::Value sharpened = ir.CompositeConstruct(sharp[0], sharp[1], sharp[2], sharp[3]);
            for (const auto& [user, operand] : users) {
                user->SetArg(operand, sharpened);
            }
        }
    }
}

} // namespace Shader::Optimization
