// SPDX-License-Identifier: GPL-2.0-or-later
// Scene HDR: the tone pass shadow shader from the player's own game (see gow3_hdr_tone.h).

#include "gow3_hdr_tone.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>

namespace Gow3HdrTone {
namespace {

// SPIR-V opcodes, enumerants and the parts of the layout this file relies on.
constexpr uint32_t Magic = 0x07230203u;
constexpr size_t HeaderWords = 5;
constexpr uint32_t OpString = 7, OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15,
                   OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeFloat = 22,
                   OpTypeVector = 23, OpTypePointer = 32, OpTypeFunction = 33, OpTypeForwardPointer = 39,
                   OpConstant = 43, OpFunction = 54, OpFunctionParameter = 55,
                   OpFunctionEnd = 56, OpFunctionCall = 57, OpVariable = 59, OpStore = 62,
                   OpAccessChain = 65, OpDecorate = 71, OpCompositeConstruct = 80,
                   OpCompositeExtract = 81, OpBitcast = 124, OpFAdd = 129, OpFMul = 133,
                   OpFDiv = 136, OpPhi = 245, OpLabel = 248, OpBranchConditional = 250;
constexpr uint32_t CapabilityLinkage = 5, DecorationLinkageAttributes = 41, LinkageImport = 1,
                   ExecutionModelFragment = 4, StorageClassOutput = 3, StorageClassFunction = 7;
constexpr uint32_t GlslFClamp = 43;
constexpr const char* EpilogueFunction = "HdrTone";

struct Inst {
    std::vector<uint32_t> w;
    uint32_t op() const { return w[0] & 0xffffu; }
};

bool Parse(std::span<const uint32_t> code, std::vector<Inst>& out) {
    if (code.size() <= HeaderWords || code[0] != Magic) {
        return false;
    }
    for (size_t at = HeaderWords; at < code.size();) {
        const size_t words = code[at] >> 16;
        if (words == 0 || at + words > code.size()) {
            return false;
        }
        out.push_back({{code.begin() + at, code.begin() + at + words}});
        at += words;
    }
    return true;
}

std::vector<uint32_t> Flatten(std::span<const uint32_t> header, uint32_t bound,
                              const std::vector<Inst>& insts) {
    std::vector<uint32_t> code(header.begin(), header.begin() + HeaderWords);
    code[3] = bound;
    for (const Inst& inst : insts) {
        code.insert(code.end(), inst.w.begin(), inst.w.end());
    }
    return code;
}

Inst Make(uint32_t op, std::initializer_list<uint32_t> operands) {
    Inst inst;
    inst.w.push_back(uint32_t(operands.size() + 1) << 16 | op);
    inst.w.insert(inst.w.end(), operands);
    return inst;
}

// The word that holds an instruction's result id, for the instructions this file looks up.
size_t ResultWord(uint32_t op) {
    if (op == OpString || op == OpExtInstImport || op == OpLabel ||
        (op >= OpTypeVoid && op < OpTypeForwardPointer)) {
        return 1;
    }
    const bool typed = op == OpExtInst || (op >= 41 && op <= 52) || op == OpFunction ||
                       op == OpFunctionParameter || op == OpFunctionCall || op == OpVariable ||
                       op == OpAccessChain || op == OpCompositeConstruct ||
                       op == OpCompositeExtract || op == OpBitcast || (op >= OpFAdd && op <= 142) ||
                       op == OpPhi || op == 61;
    return typed ? 2 : 0;
}

bool StringEquals(const Inst& inst, size_t from, const char* text) {
    const size_t length = std::strlen(text);
    if ((inst.w.size() - from) * 4 <= length) {
        return false;
    }
    return std::memcmp(inst.w.data() + from, text, length + 1) == 0;
}

// The translated tone pass is patched in place of being rebuilt: three things are looked up in it.
//   1. The filmic curve's branch: the 0-1 result (a phi of clamps) and the curve's constants.
//   2. Its inputs, so the epilogue gets the scene value the curve takes.
//   3. The color it writes, which the epilogue replaces.
// Anything that does not match is a different shader (or recompiler): the caller keeps SDR.
class ToneShader {
public:
    ToneShader(std::span<const uint32_t> code, std::string& problem) : problem_(problem) {
        header_.assign(code.begin(), code.begin() + std::min(code.size(), HeaderWords));
        ok_ = Parse(code, insts_);
        if (!ok_) {
            problem_ = "the translated tone pass is not SPIR-V";
            return;
        }
        bound_ = header_[3];
        for (size_t i = 0; i < insts_.size(); ++i) {
            if (const size_t at = ResultWord(insts_[i].op()); at && insts_[i].w.size() > at) {
                def_[insts_[i].w[at]] = i;
            }
        }
    }

    // The patched module, empty with the problem set when the shader does not match.
    std::vector<uint32_t> Patch();

private:
    struct Curve {
        uint32_t x, c0, c1, c4, c5, c6;
    };

    bool Fail(const char* why) {
        problem_ = std::string("the translated tone pass differs from the expected one: ") + why;
        return false;
    }
    const Inst* Def(uint32_t id) const {
        const auto it = def_.find(id);
        return it == def_.end() ? nullptr : &insts_[it->second];
    }
    bool Is(const Inst* inst, uint32_t op) const { return inst && inst->op() == op; }
    uint32_t NewId() { return bound_++; }
    // a, b: the operands of a two-operand float instruction, a defined by first, b by second.
    bool Operands(const Inst* inst, uint32_t first, uint32_t second, uint32_t& a,
                  uint32_t& b) const {
        if (!inst || inst->w.size() != 5) {
            return false;
        }
        for (size_t s = 0; s < 2; ++s) {
            a = inst->w[3 + s];
            b = inst->w[4 - s];
            if (Is(Def(a), first) && Is(Def(b), second)) {
                return true;
            }
        }
        return false;
    }
    bool FilmicCurve(uint32_t clamp, Curve& curve) const;
    bool Find();

    std::string& problem_;
    bool ok_ = false;
    std::vector<uint32_t> header_;
    std::vector<Inst> insts_;
    std::unordered_map<uint32_t, size_t> def_;
    uint32_t bound_ = 0;
    // What Find collects.
    uint32_t glsl_ = 0, f32_ = 0, bool_ = 0, vec3_ = 0, zero_ = 0, out_var_ = 0, entry_fn_ = 0;
    size_t entry_at_ = 0, end_at_ = 0;
};

// clamp(c4 + (x * c5 + c6) / (x * (x + c0) + c1), 0, 1) for the curve (x the scene value).
bool ToneShader::FilmicCurve(uint32_t clamp_id, Curve& curve) const {
    const Inst* clamp = Def(clamp_id);
    if (!Is(clamp, OpExtInst) || clamp->w.size() != 8 || clamp->w[3] != glsl_ ||
        clamp->w[4] != GlslFClamp) {
        return false;
    }
    uint32_t p, c4, r, n, c1, m, k, c6;
    if (!Is(Def(clamp->w[5]), OpFAdd) || !Operands(Def(clamp->w[5]), OpFMul, OpBitcast, p, c4) ||
        !Operands(Def(p), OpFDiv, OpFAdd, r, n) || Def(r)->w.size() != 5 ||
        !Operands(Def(Def(r)->w[4]), OpFMul, OpBitcast, m, c1) ||
        !Operands(Def(n), OpFMul, OpBitcast, k, c6)) {
        return false;
    }
    // m = x * h with h = c0 + x
    const Inst* mul = Def(m);
    uint32_t x = 0, c0 = 0;
    for (size_t s = 0; s < 2 && !x; ++s) {
        const uint32_t u = mul->w[3 + s];
        const Inst* h = Def(mul->w[4 - s]);
        if (Is(h, OpFAdd) && h->w.size() == 5) {
            for (size_t t = 0; t < 2; ++t) {
                if (h->w[3 + t] == u && Is(Def(h->w[4 - t]), OpBitcast)) {
                    x = u;
                    c0 = h->w[4 - t];
                }
            }
        }
    }
    // k = x * c5
    const Inst* scale = Def(k);
    uint32_t c5 = 0;
    for (size_t s = 0; s < 2; ++s) {
        if (scale->w[3 + s] == x && Is(Def(scale->w[4 - s]), OpBitcast)) {
            c5 = scale->w[4 - s];
        }
    }
    if (!x || !c5) {
        return false;
    }
    curve = {x, c0, c1, c4, c5, c6};
    return true;
}

bool ToneShader::Find() {
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < insts_.size(); ++i) {
            const Inst& inst = insts_[i];
            switch (inst.op()) {
            case OpExtInstImport:
                if (StringEquals(inst, 2, "GLSL.std.450")) {
                    glsl_ = inst.w[1];
                }
                break;
            case OpTypeBool:
                bool_ = inst.w[1];
                break;
            case OpTypeFloat:
                if (inst.w[2] == 32 && !f32_) {
                    f32_ = inst.w[1];
                }
                break;
            case OpTypeVector:
                if (pass && inst.w[2] == f32_ && inst.w[3] == 3 && !vec3_) {
                    vec3_ = inst.w[1];
                }
                break;
            case OpConstant:
                if (pass && inst.w[1] == f32_ && inst.w[3] == 0 && !zero_) {
                    zero_ = inst.w[2];
                }
                break;
            case OpEntryPoint:
                if (inst.w[1] == ExecutionModelFragment) {
                    entry_fn_ = inst.w[2];
                }
                break;
            case OpVariable:
                if (pass == 0 && inst.w[3] == StorageClassOutput) {
                    if (out_var_) {
                        return Fail("more than one output");
                    }
                    out_var_ = inst.w[2];
                }
                break;
            default:
                break;
            }
        }
    }
    if (!glsl_ || !f32_ || !bool_ || !vec3_ || !zero_ || !out_var_ || !entry_fn_) {
        return Fail("a type, constant or the output is missing");
    }
    for (size_t i = 0; i < insts_.size(); ++i) {
        if (insts_[i].op() == OpFunction && insts_[i].w[2] == entry_fn_) {
            entry_at_ = i;
        } else if (entry_at_ && insts_[i].op() == OpFunctionEnd) {
            end_at_ = i;
            break;
        }
    }
    return end_at_ ? true : Fail("no entry function");
}

std::vector<uint32_t> ToneShader::Patch() {
    if (!ok_ || !Find()) {
        return {};
    }
    // 1. The merge of the filmic branch: three phis of clamped curve values, one incoming edge
    //    from the branch (T) and one from the block that decides it (F).
    uint32_t then_block = 0, else_block = 0, merge_block = 0, label = 0;
    std::vector<size_t> phis;
    for (size_t i = entry_at_; i < end_at_; ++i) {
        const Inst& inst = insts_[i];
        if (inst.op() == OpLabel) {
            label = inst.w[1];
        } else if (inst.op() == OpPhi && inst.w.size() == 7 && inst.w[1] == f32_ &&
                   Is(Def(inst.w[3]), OpExtInst) && Def(inst.w[3])->w[4] == GlslFClamp) {
            if (phis.empty()) {
                merge_block = label;
                then_block = inst.w[4];
                else_block = inst.w[6];
            } else if (i != phis.back() + 1 || inst.w[4] != then_block ||
                       inst.w[6] != else_block || label != merge_block) {
                Fail("the filmic branch merges in an unexpected way");
                return {};
            }
            phis.push_back(i);
        }
    }
    if (phis.size() != 3) {
        Fail("no filmic branch with three clamped results");
        return {};
    }
    // The deciding block ends in a conditional branch to the curve's block or the merge.
    uint32_t filmic = 0;
    label = 0;
    for (size_t i = entry_at_; i < end_at_; ++i) {
        const Inst& inst = insts_[i];
        if (inst.op() == OpLabel) {
            label = inst.w[1];
        } else if (inst.op() == OpBranchConditional && inst.w[2] == then_block &&
                   inst.w[3] == merge_block && label == else_block) {
            filmic = inst.w[1];
        }
    }
    if (!filmic) {
        Fail("the filmic branch has no condition");
        return {};
    }
    std::array<Curve, 3> curves;
    for (size_t c = 0; c < 3; ++c) {
        if (!FilmicCurve(insts_[phis[c]].w[3], curves[c])) {
            Fail("the filmic curve is not in the expected form");
            return {};
        }
        const Curve& first = curves[0];
        const Curve& now = curves[c];
        if (now.c0 != first.c0 || now.c1 != first.c1 || now.c4 != first.c4 ||
            now.c5 != first.c5 || now.c6 != first.c6) {
            Fail("the curve's constants differ between channels");
            return {};
        }
    }
    // 2. The color stores: the first three components of the output.
    std::array<size_t, 3> store_at{};
    std::array<size_t, 3> chain_at{};
    uint32_t found = 0;
    for (size_t i = entry_at_; i < end_at_; ++i) {
        const Inst& inst = insts_[i];
        if (inst.op() != OpStore) {
            continue;
        }
        const Inst* chain = Def(inst.w[1]);
        if (!Is(chain, OpAccessChain) || chain->w.size() != 5 || chain->w[3] != out_var_) {
            continue;
        }
        const Inst* index = Def(chain->w[4]);
        if (!Is(index, OpConstant) || index->w[3] > 2) {
            continue;
        }
        if (found & 1u << index->w[3]) {
            Fail("a color component is written twice");
            return {};
        }
        store_at[index->w[3]] = i;
        chain_at[index->w[3]] = def_.at(chain->w[2]);
        found |= 1u << index->w[3];
    }
    if (found != 7) {
        Fail("the output is not written as three components");
        return {};
    }
    const size_t call_at = *std::min_element(chain_at.begin(), chain_at.end());
    for (size_t c = 0; c < 3; ++c) {
        const auto value = def_.find(insts_[store_at[c]].w[2]);
        if (value == def_.end() || value->second >= call_at || store_at[c] <= call_at) {
            Fail("the output values are not ready before the stores");
            return {};
        }
    }

    // 3. Edits. Insertions are collected by the index they go before.
    std::map<size_t, std::vector<Inst>> before;
    // The import: LinkageAttributes "HdrTone" Import, declared with the nine arguments.
    const uint32_t fn_type = NewId(), fn = NewId();
    std::array<uint32_t, 9> arg_var{};
    {
        const size_t name_words = (std::strlen(EpilogueFunction) + 4) / 4;
        Inst decorate;
        decorate.w = {uint32_t(4 + name_words) << 16 | OpDecorate, fn,
                      DecorationLinkageAttributes};
        decorate.w.resize(3 + name_words, 0);
        std::memcpy(&decorate.w[3], EpilogueFunction, std::strlen(EpilogueFunction));
        decorate.w.push_back(LinkageImport);
        size_t type_at = 0;
        while (type_at < insts_.size() &&
               !(insts_[type_at].op() >= OpTypeVoid && insts_[type_at].op() <= OpTypeForwardPointer)) {
            ++type_at;
        }
        before[type_at].push_back(std::move(decorate));
        size_t cap_at = 0;
        for (size_t i = 0; i < insts_.size() && insts_[i].op() == OpCapability; ++i) {
            cap_at = i + 1;
        }
        before[cap_at].push_back(Make(OpCapability, {CapabilityLinkage}));
        // The epilogue takes its arguments by pointer (how DXC compiles library functions), so
        // the call site gets function variables, declared at the start of the entry function.
        auto pointer_to = [&](uint32_t type) {
            for (const Inst& inst : insts_) {
                if (inst.op() == OpTypePointer && inst.w[2] == StorageClassFunction &&
                    inst.w[3] == type) {
                    return inst.w[1];
                }
            }
            const uint32_t id = NewId();
            before[entry_at_].push_back(Make(OpTypePointer, {id, StorageClassFunction, type}));
            return id;
        };
        const uint32_t ptr_vec3 = pointer_to(vec3_), ptr_bool = pointer_to(bool_),
                       ptr_f32 = pointer_to(f32_);
        auto& decl = before[entry_at_];
        decl.push_back(Make(OpTypeFunction, {fn_type, vec3_, ptr_vec3, ptr_vec3, ptr_vec3,
                                            ptr_bool, ptr_f32, ptr_f32, ptr_f32, ptr_f32, ptr_f32}));
        decl.push_back(Make(OpFunction, {vec3_, fn, 0, fn_type}));
        const uint32_t types[] = {ptr_vec3, ptr_vec3, ptr_vec3, ptr_bool, ptr_f32,
                                  ptr_f32,  ptr_f32,  ptr_f32,  ptr_f32};
        for (const uint32_t type : types) {
            decl.push_back(Make(OpFunctionParameter, {type, NewId()}));
        }
        decl.push_back(Make(OpFunctionEnd, {}));
        if (insts_[entry_at_ + 1].op() != OpLabel) {
            Fail("the entry function does not start with a block");
            return {};
        }
        for (size_t a = 0; a < 9; ++a) {
            arg_var[a] = NewId();
            before[entry_at_ + 2].push_back(
                Make(OpVariable, {types[a], arg_var[a], StorageClassFunction}));
        }
    }
    // Phis in the merge block: the curve's input and constants (zero when it is not taken).
    auto& merge = before[phis.back() + 1];
    std::array<uint32_t, 3> scene{};
    for (size_t c = 0; c < 3; ++c) {
        scene[c] = NewId();
        merge.push_back(Make(OpPhi, {f32_, scene[c], curves[c].x, then_block,
                                     insts_[phis[c]].w[5], else_block}));
    }
    // (the else value of a curve channel is the phi's second pair: the scene value itself)
    std::array<uint32_t, 5> constants{};
    const uint32_t curve_constants[] = {curves[0].c0, curves[0].c1, curves[0].c4, curves[0].c5,
                                        curves[0].c6};
    for (size_t k = 0; k < 5; ++k) {
        constants[k] = NewId();
        merge.push_back(Make(OpPhi, {f32_, constants[k], curve_constants[k], then_block, zero_,
                                     else_block}));
    }
    // The call, before the first store: epilogue(scene, neutral, color, filmic, constants).
    auto& call = before[call_at];
    const uint32_t scene_v = NewId(), neutral_v = NewId(), color_v = NewId(), result = NewId();
    call.push_back(Make(OpCompositeConstruct, {vec3_, scene_v, scene[0], scene[1], scene[2]}));
    call.push_back(Make(OpCompositeConstruct, {vec3_, neutral_v, insts_[phis[0]].w[2],
                                               insts_[phis[1]].w[2], insts_[phis[2]].w[2]}));
    call.push_back(Make(OpCompositeConstruct, {vec3_, color_v, insts_[store_at[0]].w[2],
                                               insts_[store_at[1]].w[2], insts_[store_at[2]].w[2]}));
    const uint32_t values[] = {scene_v, neutral_v, color_v, filmic, constants[0], constants[1],
                               constants[2], constants[3], constants[4]};
    for (size_t a = 0; a < 9; ++a) {
        call.push_back(Make(OpStore, {arg_var[a], values[a]}));
    }
    call.push_back(Make(OpFunctionCall, {vec3_, result, fn, arg_var[0], arg_var[1], arg_var[2],
                                         arg_var[3], arg_var[4], arg_var[5], arg_var[6],
                                         arg_var[7], arg_var[8]}));
    for (uint32_t c = 0; c < 3; ++c) {
        const uint32_t value = NewId();
        call.push_back(Make(OpCompositeExtract, {f32_, value, result, c}));
        insts_[store_at[c]].w[2] = value;
    }

    std::vector<Inst> out;
    for (size_t i = 0; i < insts_.size(); ++i) {
        if (const auto it = before.find(i); it != before.end()) {
            for (Inst& added : it->second) {
                out.push_back(std::move(added));
            }
        }
        out.push_back(std::move(insts_[i]));
    }
    return Flatten(header_, bound_, out);
}

bool HasNul(uint32_t word) {
    return !(word & 0xff000000u) || !(word & 0x00ff0000u) || !(word & 0x0000ff00u) ||
           !(word & 0x000000ffu);
}

// From SPIR-V 1.4 an entry point lists every global variable it uses; the epilogue's settings
// buffer arrives with the link, so the list is completed here.
bool ListGlobals(std::vector<uint32_t>& code) {
    std::vector<Inst> insts;
    if (!Parse(code, insts)) {
        return false;
    }
    if (code[1] >= 0x00010400u) {
        std::unordered_set<uint32_t> listed;
        std::vector<uint32_t> globals;
        for (const Inst& inst : insts) {
            if (inst.op() == OpEntryPoint) {
                size_t at = 3;
                while (!HasNul(inst.w[at])) {
                    ++at;
                }
                listed.insert(inst.w.begin() + at + 1, inst.w.end());
            } else if (inst.op() == OpVariable && inst.w[3] != StorageClassFunction) {
                globals.push_back(inst.w[2]);
            } else if (inst.op() == OpFunction) {
                break;
            }
        }
        for (Inst& inst : insts) {
            if (inst.op() == OpEntryPoint) {
                for (const uint32_t id : globals) {
                    if (!listed.contains(id)) {
                        inst.w.push_back(id);
                    }
                }
                inst.w[0] = uint32_t(inst.w.size()) << 16 | OpEntryPoint;
            }
        }
    }
    code = Flatten(code, code[3], insts);
    return true;
}

} // namespace

bool IsToneShader(std::span<const uint32_t> code) {
    if (code.size() <= HeaderWords || code[0] != Magic) {
        return false;
    }
    char hash[16];
    std::snprintf(hash, sizeof(hash), "0x%x", ToneShaderHash);
    // The recompiler names every shader with an OpString, before the debug names.
    for (size_t at = HeaderWords; at < code.size() && (code[at] >> 16) != 0;
         at += code[at] >> 16) {
        const uint32_t op = code[at] & 0xffffu;
        if (op == OpString) {
            const size_t words = code[at] >> 16;
            return words >= 3 && std::strncmp(reinterpret_cast<const char*>(&code[at + 2]), hash,
                                              (words - 2) * 4) == 0;
        }
        if (op == 5 /* OpName */ || op == OpDecorate || op >= OpTypeVoid) {
            return false;
        }
    }
    return false;
}

std::vector<uint32_t> MakeShadowShader(std::span<const uint32_t> guest,
                                       std::span<const uint32_t> epilogue, std::string& problem) {
    problem.clear();
    auto patched = ToneShader(guest, problem).Patch();
    if (patched.empty()) {
        return {};
    }
    std::string linker_message;
    spvtools::Context context(SPV_ENV_VULKAN_1_3);
    context.SetMessageConsumer([&](spv_message_level_t level, const char*, const spv_position_t&,
                                   const char* message) {
        if (level <= SPV_MSG_ERROR) {
            linker_message = message;
        }
    });
    std::vector<uint32_t> linked;
    const std::vector<std::vector<uint32_t>> modules = {
        std::move(patched), std::vector<uint32_t>(epilogue.begin(), epilogue.end())};
    if (spvtools::Link(context, modules, &linked, spvtools::LinkerOptions{}) != SPV_SUCCESS ||
        !ListGlobals(linked)) {
        problem = "linking the HDR epilogue failed: " + linker_message;
        return {};
    }
    return linked;
}

namespace {
struct Entry {
    std::vector<uint32_t> guest;
    std::vector<uint32_t> shadow;
    bool built = false;
};
std::mutex g_mutex;
std::unordered_map<uint64_t, Entry> g_entries;

std::filesystem::path EpilogueFile() {
    std::filesystem::path dir;
    if (const char* env = std::getenv("GOW3_HDR_SHADER_DIR"); env && *env) {
        dir = env;
    } else {
#ifdef _WIN32
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        dir = std::filesystem::path(std::wstring(path, length)).parent_path() / "hdr";
#else
        std::error_code error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
        dir = (error ? std::filesystem::current_path() : executable.parent_path()) / "hdr";
#endif
    }
    return dir / "hdr_tone.spv";
}

void Say(const std::string& line) {
    std::printf("HDR: %s\n", line.c_str());
    std::fflush(stdout);
}

void Build(uint64_t module, Entry& entry) {
    entry.built = true;
    const auto file = EpilogueFile();
    std::vector<uint32_t> epilogue;
    if (std::ifstream in{file, std::ios::binary | std::ios::ate}) {
        const auto bytes = size_t(in.tellg());
        if (bytes >= 20 && bytes % 4 == 0) {
            epilogue.resize(bytes / 4);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(epilogue.data()), std::streamsize(bytes));
            if (!in || epilogue[0] != Magic) {
                epilogue.clear();
            }
        }
    }
    std::string problem;
    if (epilogue.empty()) {
        problem = "no readable " + file.string();
    } else {
        entry.shadow = MakeShadowShader(entry.guest, epilogue, problem);
    }
    entry.guest = {};
    if (entry.shadow.empty()) {
        Say("no scene HDR (" + problem + "); HDR10 output of the SDR scene");
    } else {
        char text[96];
        std::snprintf(text, sizeof(text), "tone pass shader %#llx derived from the game's own",
                      static_cast<unsigned long long>(module));
        Say(text);
    }
}
} // namespace

void Remember(uint64_t module, std::span<const uint32_t> code) {
    if (!IsToneShader(code)) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    Entry& entry = g_entries[module];
    entry.guest.assign(code.begin(), code.end());
    entry.shadow.clear();
    entry.built = false;
}

std::vector<uint32_t> Shadow(uint64_t module) {
    std::scoped_lock lock{g_mutex};
    const auto it = g_entries.find(module);
    if (it == g_entries.end()) {
        return {};
    }
    if (!it->second.built) {
        Build(module, it->second);
    }
    return it->second.shadow;
}

} // namespace Gow3HdrTone
