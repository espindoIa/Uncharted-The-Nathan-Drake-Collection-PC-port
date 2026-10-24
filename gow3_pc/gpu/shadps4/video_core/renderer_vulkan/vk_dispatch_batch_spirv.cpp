// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_dispatch_batch_spirv.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <spirv-tools/libspirv.h>
#include <spirv/unified1/spirv.hpp11>

namespace Vulkan {
namespace {
using Op = spv::Op;
constexpr u32 U(auto value) { return static_cast<u32>(value); }
struct Instruction {
    std::vector<u32> words;
    std::vector<u16> ids;
    Op Opcode() const { return Op(words[0] & 0xffff); }
};
void Emit(std::vector<u32>& out, Op op, std::initializer_list<u32> args) {
    out.push_back((u32(args.size() + 1) << 16) | U(op));
    out.insert(out.end(), args);
}
}

std::vector<u32> MakeDispatchBatchSpirv(std::span<const u32> source, u32 capacity,
                                      u32 buffer_bindings, std::string& problem) {
    auto fail = [&](const char* why) { problem = why; return std::vector<u32>{}; };
    if (source.size() < 5 || source[0] != spv::MagicNumber || capacity < 2 || capacity > 32) {
        return fail("invalid module or batch capacity");
    }
    std::vector<Instruction> instructions;
    const auto parse = [](void* data, const spv_parsed_instruction_t* parsed) -> spv_result_t {
        auto& dst = *static_cast<std::vector<Instruction>*>(data);
        auto& inst = dst.emplace_back();
        inst.words.assign(parsed->words, parsed->words + parsed->num_words);
        for (u16 i = 0; i < parsed->num_operands; ++i) {
            const auto& operand = parsed->operands[i];
            if (operand.type == SPV_OPERAND_TYPE_ID) {
                inst.ids.push_back(operand.offset);
            }
        }
        return SPV_SUCCESS;
    };
    const auto context = spvContextCreate(SPV_ENV_VULKAN_1_3);
    const auto parsed = spvBinaryParse(context, &instructions, source.data(), source.size(),
                                       nullptr, parse, nullptr);
    spvContextDestroy(context);
    if (parsed != SPV_SUCCESS) return fail("SPIR-V parse failed");

    u32 next = source[3], uint_type = 0, wg_var = 0, wg_vec = 0, main = 0, functions = 0;
    std::unordered_map<u32, std::array<u32, 2>> pointers;
    std::unordered_map<u32, u32> bindings, builtins, constants;
    std::vector<Instruction> variables;
    for (const auto& inst : instructions) {
        const auto& w = inst.words;
        switch (inst.Opcode()) {
        case Op::OpTypeInt:
            if (w[2] == 32 && w[3] == 0) uint_type = w[1];
            break;
        case Op::OpTypePointer: pointers[w[1]] = {w[2], w[3]}; break;
        case Op::OpConstant:
            if (w.size() == 4) constants[w[2]] = w[3];
            break;
        case Op::OpDecorate:
            if (w[2] == U(spv::Decoration::Binding)) bindings[w[1]] = w[3];
            if (w[2] == U(spv::Decoration::DescriptorSet) && w[3] != 0)
                return fail("descriptor set other than zero");
            if (w[2] == U(spv::Decoration::BuiltIn)) builtins[w[1]] = w[3];
            break;
        case Op::OpEntryPoint:
            if (main || w[1] != U(spv::ExecutionModel::GLCompute))
                return fail("requires one compute entry point");
            main = w[2];
            break;
        case Op::OpExecutionMode:
            if (w[2] == U(spv::ExecutionMode::LocalSize) && (w[4] != 1 || w[5] != 1))
                return fail("local Y/Z must be one");
            break;
        case Op::OpFunction: ++functions; break;
        default: break;
        }
    }
    if (!main || !uint_type || functions != 1) return fail("unsupported function/type layout");
    bool local_size = false;
    for (const auto& inst : instructions) {
        const auto& w = inst.words;
        if (inst.Opcode() == Op::OpExecutionMode && w[2] == U(spv::ExecutionMode::LocalSize)) {
            local_size = true;
        } else if (inst.Opcode() == Op::OpExecutionModeId && w[2] == U(spv::ExecutionMode::LocalSizeId)) {
            if (!constants.contains(w[4]) || !constants.contains(w[5]) ||
                constants[w[4]] != 1 || constants[w[5]] != 1)
                return fail("specialized/non-unit local Y/Z");
            local_size = true;
        }
    }
    if (!local_size) return fail("missing fixed local size");
    for (const auto& inst : instructions) {
        if (inst.Opcode() == Op::OpVariable &&
            builtins.contains(inst.words[2]) &&
            builtins[inst.words[2]] == U(spv::BuiltIn::WorkgroupId)) {
            wg_var = inst.words[2];
            wg_vec = pointers[inst.words[1]][1];
        }
    }
    std::vector<u32> types, decorations, prolog;
    const u32 zero = next++, one = next++, length = next++;
    Emit(types, Op::OpConstant, {uint_type, zero, 0});
    Emit(types, Op::OpConstant, {uint_type, one, 1});
    Emit(types, Op::OpConstant, {uint_type, length, capacity});
    const bool new_wg = !wg_var;
    if (new_wg) {
        wg_vec = next++;
        const u32 pointer = next++;
        wg_var = next++;
        Emit(types, Op::OpTypeVector, {wg_vec, uint_type, 3});
        Emit(types, Op::OpTypePointer, {pointer, U(spv::StorageClass::Input), wg_vec});
        Instruction variable;
        Emit(variable.words, Op::OpVariable, {pointer, wg_var, U(spv::StorageClass::Input)});
        variables.push_back(std::move(variable));
        Emit(decorations, Op::OpDecorate, {wg_var, U(spv::Decoration::BuiltIn), U(spv::BuiltIn::WorkgroupId)});
    }
    const u32 wg_value = next++, batch = next++;
    Emit(prolog, Op::OpLoad, {wg_vec, wg_value, wg_var});
    Emit(prolog, Op::OpCompositeExtract, {uint_type, batch, wg_value, 1});
    std::unordered_map<u32, u32> selected, array_pointers;
    u32 pushes = 0;
    for (auto& inst : instructions) {
        if (inst.Opcode() != Op::OpVariable) continue;
        auto& w = inst.words;
        const u32 storage = w[3];
        if (storage != U(spv::StorageClass::StorageBuffer) && storage != U(spv::StorageClass::PushConstant))
            continue;
        const u32 var = w[2], original_pointer = w[1];
        if (!pointers.contains(original_pointer)) return fail("unknown resource pointer type");
        if (storage == U(spv::StorageClass::PushConstant)) {
            if (++pushes != 1) return fail("multiple push blocks");
            Emit(decorations, Op::OpDecorate, {var, U(spv::Decoration::DescriptorSet), 0});
            Emit(decorations, Op::OpDecorate, {var, U(spv::Decoration::Binding), buffer_bindings});
        } else if (!bindings.contains(var) || bindings[var] >= buffer_bindings) {
            return fail("unexpected buffer binding");
        }
        u32 array_pointer;
        if (const auto it = array_pointers.find(original_pointer); it != array_pointers.end()) {
            array_pointer = it->second;
        } else {
            const u32 array = next++;
            array_pointer = next++;
            Emit(types, Op::OpTypeArray, {array, pointers[original_pointer][1], length});
            Emit(types, Op::OpTypePointer, {array_pointer, U(spv::StorageClass::StorageBuffer), array});
            array_pointers[original_pointer] = array_pointer;
        }
        w[1] = array_pointer;
        w[3] = U(spv::StorageClass::StorageBuffer);
        const u32 local = next++;
        selected[var] = local;
        Emit(prolog, Op::OpAccessChain, {original_pointer, local, var, batch});
    }
    if (!pushes || selected.size() < buffer_bindings) return fail("missing push block or resources");
    std::unordered_map<u32, u32> y_pointer_values;
    std::vector<u32> out(source.begin(), source.begin() + 5);
    bool capability = false, decorated = false, function = false, injected = false, entry_label = false;
    auto append = [&](const std::vector<u32>& w) { out.insert(out.end(), w.begin(), w.end()); };
    for (auto& inst : instructions) {
        auto& w = inst.words;
        const Op op = inst.Opcode();
        if (!capability && op != Op::OpCapability) {
            Emit(out, Op::OpCapability, {U(spv::Capability::StorageBufferArrayDynamicIndexing)});
            capability = true;
        }
        if (!decorated && (op == Op::OpTypeVoid || op == Op::OpTypeInt || op == Op::OpTypeFloat)) {
            append(decorations);
            decorated = true;
        }
        if (op == Op::OpTypePointer && w[2] == U(spv::StorageClass::PushConstant))
            w[2] = U(spv::StorageClass::StorageBuffer);
        if (op == Op::OpEntryPoint && new_wg) {
            w.push_back(wg_var);
            w[0] += 1u << 16;
        }
        if (!function && op == Op::OpVariable) {
            variables.push_back(inst); // put declarations after all original and new types
            continue;
        }
        if (op == Op::OpFunction) {
            append(types);
            for (const auto& variable : variables) append(variable.words);
            function = true;
        }
        if (function && entry_label && !injected && op != Op::OpVariable &&
            op != Op::OpLine && op != Op::OpNoLine) {
            append(prolog);
            injected = true;
        }
        if (function && op == Op::OpLabel && !entry_label) entry_label = true;
        if (function) {
            // Parsed ID operands only: NEVER rewrite numeric literals which happen to equal an ID.
            for (const u16 at : inst.ids) {
                if (const auto it = selected.find(w[at]); it != selected.end()) w[at] = it->second;
            }
            if ((op == Op::OpAccessChain || op == Op::OpInBoundsAccessChain) && builtins.contains(w[3])) {
                if (w.size() != 5 || !constants.contains(w[4])) return fail("dynamic builtin pointer");
                if (constants[w[4]] == 1) {
                    const u32 builtin = builtins[w[3]];
                    if (builtin == U(spv::BuiltIn::WorkgroupId) || builtin == U(spv::BuiltIn::GlobalInvocationId))
                        y_pointer_values[w[2]] = zero;
                    else if (builtin == U(spv::BuiltIn::NumWorkgroups)) y_pointer_values[w[2]] = one;
                }
            }
            if (op == Op::OpLoad && y_pointer_values.contains(w[3])) {
                Emit(out, Op::OpCopyObject, {w[1], w[2], y_pointer_values[w[3]]});
                continue;
            }
            if (op == Op::OpLoad && builtins.contains(w[3])) {
                const u32 builtin = builtins[w[3]];
                if (builtin == U(spv::BuiltIn::WorkgroupId) || builtin == U(spv::BuiltIn::GlobalInvocationId) ||
                    builtin == U(spv::BuiltIn::NumWorkgroups)) {
                    const u32 result = w[2], temporary = next++;
                    w[2] = temporary;
                    append(w);
                    Emit(out, Op::OpCompositeInsert, {w[1], result,
                         builtin == U(spv::BuiltIn::NumWorkgroups) ? one : zero, temporary, 1});
                    continue;
                }
            }
        }
        append(w);
    }
    out[3] = next;
    const auto validation = spvContextCreate(SPV_ENV_VULKAN_1_3);
    const auto options = spvValidatorOptionsCreate();
    spvValidatorOptionsSetScalarBlockLayout(options, true);
    spv_diagnostic diagnostic = nullptr;
    spv_const_binary_t binary{out.data(), out.size()};
    const auto valid = spvValidateWithOptions(validation, options, &binary, &diagnostic);
    if (valid != SPV_SUCCESS) problem = diagnostic ? diagnostic->error : "validation failed";
    spvDiagnosticDestroy(diagnostic);
    spvValidatorOptionsDestroy(options);
    spvContextDestroy(validation);
    if (valid != SPV_SUCCESS) return {};
    return out;
}
} // namespace Vulkan
