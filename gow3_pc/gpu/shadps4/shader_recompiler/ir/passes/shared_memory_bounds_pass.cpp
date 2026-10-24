// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

// gow3: GOW3_LDS_BOUNDS: unset or 1 = every compute shader, 0 = off, or hex hashes separated by
// commas = only those.
bool LdsBoundsEnabled(u64 pgm_hash) {
    static const std::vector<u64> hashes = [] {
        std::vector<u64> list;
        const char* env = std::getenv("GOW3_LDS_BOUNDS");
        if (!env || (env[0] == '1' && env[1] == 0)) {
            return std::vector<u64>{~0ULL};
        }
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
    return std::ranges::any_of(hashes, [pgm_hash](u64 hash) { return hash == ~0ULL || hash == pgm_hash; });
}

// Access size in bytes, or 0 when the opcode is not a shared memory access.
static u32 AccessBytes(IR::Opcode op) {
    using Op = IR::Opcode;
    switch (op) {
    case Op::LoadSharedU16:
    case Op::WriteSharedU16:
        return 2;
    case Op::LoadSharedU32:
    case Op::WriteSharedU32:
    case Op::SharedAtomicIAdd32:
    case Op::SharedAtomicISub32:
    case Op::SharedAtomicSMin32:
    case Op::SharedAtomicUMin32:
    case Op::SharedAtomicSMax32:
    case Op::SharedAtomicUMax32:
    case Op::SharedAtomicInc32:
    case Op::SharedAtomicDec32:
    case Op::SharedAtomicAnd32:
    case Op::SharedAtomicOr32:
    case Op::SharedAtomicXor32:
    case Op::SharedAtomicCmpSwap32:
        return 4;
    case Op::LoadSharedU64:
    case Op::WriteSharedU64:
    case Op::SharedAtomicIAdd64:
    case Op::SharedAtomicISub64:
    case Op::SharedAtomicSMin64:
    case Op::SharedAtomicUMin64:
    case Op::SharedAtomicSMax64:
    case Op::SharedAtomicUMax64:
    case Op::SharedAtomicInc64:
    case Op::SharedAtomicDec64:
    case Op::SharedAtomicAnd64:
    case Op::SharedAtomicOr64:
    case Op::SharedAtomicXor64:
    case Op::SharedAtomicCmpSwap64:
        return 8;
    default:
        return 0;
    }
}

// gow3: GCN drops LDS writes and atomics past the declared size and reads 0 there. Uncharted 3's
// cloth solver (cs 1c957b01) runs 1024 threads on 2 KiB and relies on it; NVIDIA corrupted it.
// Out-of-range writes and atomics go to a dummy slot behind the game's LDS, loads read 0.
void SharedMemoryBoundsPass(IR::Program& program, const RuntimeInfo& runtime_info,
                            const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || !LdsBoundsEnabled(program.info.pgm_hash)) {
        return;
    }
    const u32 size = runtime_info.hw.cs.shared_memory_size;
    if (size == 0) {
        return;
    }
    const u32 dummy = Common::AlignUp(size, sizeof(u64));
    // gow3: the dummy slot must fit the device's shared memory limit (a shader at the exact max).
    if (dummy + u32(sizeof(u64)) + program.info.shared_memory_scratch_size >
        profile.max_shared_memory_size) {
        return;
    }

    std::vector<IR::Inst*> worklist;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const u32 bytes = AccessBytes(inst.GetOpcode());
            if (bytes == 0 || inst.Flags<bool>()) {
                continue;
            }
            const IR::Value addr = inst.Arg(0);
            if (addr.IsImmediate() && size >= bytes && addr.U32() <= size - bytes) {
                continue;
            }
            worklist.push_back(&inst);
        }
    }
    if (worklist.empty()) {
        return;
    }
    program.info.shared_memory_scratch_size =
        std::max(program.info.shared_memory_scratch_size, dummy + u32(sizeof(u64)) - size);

    for (IR::Inst* inst : worklist) {
        const IR::Opcode op = inst->GetOpcode();
        const u32 bytes = AccessBytes(op);
        const bool is_load = op == IR::Opcode::LoadSharedU16 || op == IR::Opcode::LoadSharedU32 ||
                             op == IR::Opcode::LoadSharedU64;
        const bool is_write = op == IR::Opcode::WriteSharedU16 ||
                              op == IR::Opcode::WriteSharedU32 || op == IR::Opcode::WriteSharedU64;
        const IR::U32 addr{inst->Arg(0)};
        const auto at = IR::Block::InstructionList::s_iterator_to(*inst);
        IR::IREmitter ir{*inst->GetParent(), at};
        const IR::U1 in_bounds = size >= bytes
                                     ? ir.ILessThanEqual(addr, ir.Imm32(size - bytes), false)
                                     : ir.Imm1(false);
        inst->SetArg(0, ir.Select(in_bounds, addr, ir.Imm32(is_load ? 0u : dummy)));
        if (is_write) {
            continue;
        }

        // Zero the result of out-of-range accesses. Uses are taken first so the new select, which
        // reads the original result, is not rewired to itself.
        std::vector<IR::Use> uses(inst->Uses().begin(), inst->Uses().end());
        if (uses.empty()) {
            continue;
        }
        IR::IREmitter after{*inst->GetParent(), std::next(at)};
        const IR::Value result{inst};
        IR::Value replacement;
        if (op == IR::Opcode::LoadSharedU16) {
            const IR::U32 wide{after.UConvert(32, IR::U16{result})};
            replacement = after.UConvert(16, IR::U32{after.Select(in_bounds, wide, after.Imm32(0u))});
        } else {
            replacement = after.Select(in_bounds, result,
                                       bytes == 8 ? IR::Value{after.Imm64(u64{0})}
                                                  : IR::Value{after.Imm32(0u)});
        }
        for (const IR::Use& use : uses) {
            use.user->SetArg(use.operand, replacement);
        }
    }
}

} // namespace Shader::Optimization
