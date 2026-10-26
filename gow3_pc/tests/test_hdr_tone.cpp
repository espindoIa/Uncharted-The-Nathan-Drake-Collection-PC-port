// SPDX-License-Identifier: GPL-2.0-or-later
// Scene HDR (gpu/shim/gow3_hdr_tone.cpp): the tone pass shadow shader linked from the shader
// recompiler's translation of the game's tone pass and the port's epilogue (hdr/hdr_tone.spv).
// The translation is stood in for by a small module written here with the same shape (a filmic
// branch whose merge phis feed the color stores); nothing of the game is in this file. With
// GOW3_HDR_GUEST_SPV=<a translated 0x3371ae8d, e.g. from the local shader cache> the real one is
// patched, linked and validated too (GOW3_HDR_SHADOW_OUT=<file> also writes that shadow shader,
// for spirv-cross / spirv-dis).
//   ninja -C out/gpu hdr-tone-test && ./out/gpu/hdr-tone-test.exe   (from gow3_pc)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <spirv-tools/libspirv.hpp>

#include "../gpu/shim/gow3_hdr_tone.h"

#ifndef GOW3_HDR_EPILOGUE
#error GOW3_HDR_EPILOGUE must name hdr/hdr_tone.spv
#endif

static int g_failures = 0;
#define CHECK(condition)                                                                        \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                    \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (0)

static std::vector<uint32_t> ReadWords(const char* path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<uint32_t> words(bytes.size() / 4);
    std::copy(bytes.begin(), bytes.begin() + std::ptrdiff_t(words.size() * 4),
              reinterpret_cast<char*>(words.data()));
    return words;
}

// A translated tone pass in miniature: scene = three buffer values; if the branch flag is set the
// filmic curve (c0 c1 c4 c5 c6 from the buffer) is taken, whose clamped result meets the scene
// value in three phis; the color written is derived from them. Channel 2 writes the commutative
// operands the other way round, as the real translation does.
static std::string SyntheticTonePass(const char* hash, bool with_curve_division = true) {
    std::string t = R"(
               OpCapability Shader
          %glsl = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %out %buf
               OpExecutionMode %main OriginUpperLeft
          %str = OpString ")";
    t += hash;
    t += R"("
               OpName %out "frag_color0"
               OpDecorate %out Location 0
               OpDecorate %rt ArrayStride 4
               OpDecorate %blk Block
               OpMemberDecorate %blk 0 Offset 0
               OpDecorate %buf Binding 0
               OpDecorate %buf DescriptorSet 0
          %void = OpTypeVoid
          %fn = OpTypeFunction %void
          %f32 = OpTypeFloat 32
          %u32 = OpTypeInt 32 0
          %bool = OpTypeBool
          %v4 = OpTypeVector %f32 4
          %rt = OpTypeRuntimeArray %u32
          %blk = OpTypeStruct %rt
          %p_buf = OpTypePointer StorageBuffer %blk
          %p_u32 = OpTypePointer StorageBuffer %u32
          %p_out = OpTypePointer Output %v4
          %p_outf = OpTypePointer Output %f32
          %buf = OpVariable %p_buf StorageBuffer
          %out = OpVariable %p_out Output
          %v3 = OpTypeVector %f32 3
          %u0 = OpConstant %u32 0
          %u1 = OpConstant %u32 1
          %u2 = OpConstant %u32 2
          %u3 = OpConstant %u32 3
          %u4 = OpConstant %u32 4
          %u5 = OpConstant %u32 5
          %u6 = OpConstant %u32 6
          %u7 = OpConstant %u32 7
          %u8 = OpConstant %u32 8
          %f0 = OpConstant %f32 0
          %f1 = OpConstant %f32 1
          %main = OpFunction %void None %fn
          %entry = OpLabel
)";
    // Buffer words 0-2: scene, 3: branch flag, 4-8: c0 c1 c4 c5 c6.
    auto load_at = [&](int index, const char* name) {
        const std::string i = std::to_string(index);
        return "%a" + i + " = OpAccessChain %p_u32 %buf %u0 %u" + i + "\n" + "%w" + i +
               " = OpLoad %u32 %a" + i + "\n" + name + " = OpBitcast %f32 %w" + i + "\n";
    };
    t += load_at(0, "%s0") + load_at(1, "%s1") + load_at(2, "%s2") + load_at(3, "%flag");
    t += R"(
          %cond = OpFOrdGreaterThan %bool %flag %f0
               OpSelectionMerge %merge None
               OpBranchConditional %cond %then %merge
          %then = OpLabel
)";
    t += load_at(4, "%c0") + load_at(5, "%c1") + load_at(6, "%c4") + load_at(7, "%c5") +
         load_at(8, "%c6");
    for (int c = 0; c < 3; ++c) {
        const std::string n = std::to_string(c);
        const std::string s = "%s" + n, x = "%x" + n;
        t += x + " = OpFMul %f32 " + s + " %c5\n";   // the curve's input: any affine step
        const bool swapped = c == 2;
        t += "%h" + n + " = OpFAdd %f32 " + (swapped ? x + " %c0" : "%c0 " + x) + "\n";
        t += "%m" + n + " = OpFMul %f32 " + (swapped ? "%h" + n + " " + x : x + " %h" + n) + "\n";
        t += "%d" + n + " = OpFAdd %f32 %m" + n + " %c1\n";
        t += "%r" + n + (with_curve_division ? " = OpFDiv %f32 %f1 %d" + n
                                             : " = OpFMul %f32 %f1 %d" + n) + "\n";
        t += "%k" + n + " = OpFMul %f32 " + (swapped ? "%c5 " + x : x + " %c5") + "\n";
        t += "%q" + n + " = OpFAdd %f32 %k" + n + " %c6\n";
        t += "%p" + n + " = OpFMul %f32 %r" + n + " %q" + n + "\n";
        t += "%e" + n + " = OpFAdd %f32 %p" + n + " %c4\n";
        t += "%cl" + n + " = OpExtInst %f32 %glsl FClamp %e" + n + " %f0 %f1\n";
    }
    t += R"(
               OpBranch %merge
          %merge = OpLabel
          %n0 = OpPhi %f32 %cl0 %then %s0 %entry
          %n1 = OpPhi %f32 %cl1 %then %s1 %entry
          %n2 = OpPhi %f32 %cl2 %then %s2 %entry
          %col0 = OpFMul %f32 %n0 %n0
          %col1 = OpFMul %f32 %n1 %n1
          %col2 = OpFMul %f32 %n2 %n2
          %o0 = OpAccessChain %p_outf %out %u0
               OpStore %o0 %col0
          %o1 = OpAccessChain %p_outf %out %u1
               OpStore %o1 %col1
          %o2 = OpAccessChain %p_outf %out %u2
               OpStore %o2 %col2
          %o3 = OpAccessChain %p_outf %out %u3
               OpStore %o3 %f0
               OpReturn
               OpFunctionEnd
)";
    return t;
}

static std::vector<uint32_t> Assemble(const std::string& text) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& at,
                                const char* message) {
        std::printf("assembler: line %zu: %s\n", at.line, message);
    });
    std::vector<uint32_t> binary;
    if (!tools.Assemble(text, &binary, SPV_TEXT_TO_BINARY_OPTION_NONE)) {
        std::printf("the synthetic module does not assemble\n");
        std::exit(2);
    }
    return binary;
}

static bool Validates(const std::vector<uint32_t>& code, std::string* why = nullptr) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&,
                                 const char* message) {
        if (why) {
            *why = message;
        }
    });
    return tools.Validate(code);
}

static std::string Text(const std::vector<uint32_t>& code) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string text;
    tools.Disassemble(code, &text, SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES);
    return text;
}

static bool Contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

int main() {
    const auto epilogue = ReadWords(GOW3_HDR_EPILOGUE);
    CHECK(epilogue.size() > 100);
    CHECK(Validates(epilogue) == false); // the library still imports/exports (Linkage)

    // The stand-in must itself be a valid shader, so a failure below is the patch's.
    const auto guest = Assemble(SyntheticTonePass("0x3371ae8d"));
    std::string why;
    CHECK(Validates(guest, &why));
    if (!why.empty()) {
        std::printf("stand-in: %s\n", why.c_str());
    }
    CHECK(Gow3HdrTone::IsToneShader(guest));
    CHECK(!Gow3HdrTone::IsToneShader(Assemble(SyntheticTonePass("0x92a23f78"))));
    CHECK(!Gow3HdrTone::IsToneShader(epilogue));

    std::string problem;
    const auto shadow = Gow3HdrTone::MakeShadowShader(guest, epilogue, problem);
    CHECK(!shadow.empty());
    if (shadow.empty()) {
        std::printf("problem: %s\n", problem.c_str());
    } else {
        why.clear();
        CHECK(Validates(shadow, &why));
        if (!why.empty()) {
            std::printf("shadow: %s\n", why.c_str());
        }
        const std::string text = Text(shadow);
        CHECK(Contains(text, "OpFunctionCall"));
        CHECK(!Contains(text, "OpCapability Linkage"));
        CHECK(!Contains(text, "LinkageAttributes"));
        CHECK(Contains(text, "DescriptorSet 0"));
        CHECK(Contains(text, "Binding 12"));
        // The entry point lists the settings buffer the link brought in.
        const auto entry = text.find("OpEntryPoint");
        CHECK(entry != std::string::npos &&
              text.substr(entry, text.find('\n', entry) - entry).find("%HdrSettings") !=
                  std::string::npos);
        // The same bindings as the stand-in plus the settings buffer.
        CHECK(Contains(text, "Binding 0"));
    }

    // Not the expected shapes: no curve division, no branch at all.
    const auto no_division = Assemble(SyntheticTonePass("0x3371ae8d", false));
    CHECK(Gow3HdrTone::MakeShadowShader(no_division, epilogue, problem).empty());
    CHECK(!problem.empty());
    CHECK(Gow3HdrTone::MakeShadowShader(std::vector<uint32_t>{1, 2, 3}, epilogue, problem).empty());
    CHECK(!problem.empty());

    // A broken epilogue is reported, not thrown.
    CHECK(Gow3HdrTone::MakeShadowShader(guest, std::vector<uint32_t>(epilogue.begin(),
                                                                     epilogue.begin() + 40),
                                        problem).empty());
    CHECK(!problem.empty());

    if (const char* real = std::getenv("GOW3_HDR_GUEST_SPV"); real && *real) {
        const auto translated = ReadWords(real);
        CHECK(Gow3HdrTone::IsToneShader(translated));
        const auto out = Gow3HdrTone::MakeShadowShader(translated, epilogue, problem);
        CHECK(!out.empty());
        why.clear();
        CHECK(!out.empty() && Validates(out, &why));
        std::printf("real tone pass %s: %s%s (%zu -> %zu words)\n", real,
                    out.empty() ? "no shadow: " : "shadow ok", problem.c_str(), translated.size(),
                    out.size());
        if (!why.empty()) {
            std::printf("real: %s\n", why.c_str());
        }
        if (const char* dump = std::getenv("GOW3_HDR_SHADOW_OUT"); dump && *dump && !out.empty()) {
            std::ofstream{dump, std::ios::binary}.write(
                reinterpret_cast<const char*>(out.data()), std::streamsize(out.size() * 4));
        }
    } else {
        std::printf("real translated tone pass: skipped (GOW3_HDR_GUEST_SPV not set)\n");
    }

    std::printf(g_failures ? "hdr-tone-test: %d FAILED\n" : "hdr-tone-test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
