// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <spirv-tools/libspirv.h>
#include "video_core/renderer_vulkan/vk_dispatch_batch_spirv.h"

int main(int argc, char** argv) {
    // A file argument also checks a real recompiled shader without needing a GPU/game.
    std::vector<u32> source;
    const bool emit_fixture = argc > 1 && std::strcmp(argv[1], "--write-fixture") == 0;
    const bool input = argc > 1 && !emit_fixture;
    if (input) {
        std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
        assert(f.good());
        source.resize(size_t(f.tellg()) / sizeof(u32));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(source.data()), source.size() * sizeof(u32));
    } else {
        const char* text = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %src %dst %push %wg %global %groups
OpExecutionMode %main LocalSize 4 1 1
OpDecorate %array ArrayStride 4
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %pushblock Block
OpMemberDecorate %pushblock 0 Offset 0
OpDecorate %src DescriptorSet 0
OpDecorate %src Binding 0
OpDecorate %src NonWritable
OpDecorate %dst DescriptorSet 0
OpDecorate %dst Binding 1
OpDecorate %wg BuiltIn WorkgroupId
OpDecorate %global BuiltIn GlobalInvocationId
OpDecorate %groups BuiltIn NumWorkgroups
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%vec = OpTypeVector %uint 3
%function = OpTypeFunction %void
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%four = OpConstant %uint 4
%array = OpTypeRuntimeArray %uint
%block = OpTypeStruct %array
%pushblock = OpTypeStruct %uint
%bufferptr = OpTypePointer StorageBuffer %block
%valueptr = OpTypePointer StorageBuffer %uint
%pushptr = OpTypePointer PushConstant %pushblock
%pushvalueptr = OpTypePointer PushConstant %uint
%inputptr = OpTypePointer Input %vec
%inputvalueptr = OpTypePointer Input %uint
%src = OpVariable %bufferptr StorageBuffer
%dst = OpVariable %bufferptr StorageBuffer
%push = OpVariable %pushptr PushConstant
%wg = OpVariable %inputptr Input
%global = OpVariable %inputptr Input
%groups = OpVariable %inputptr Input
%main = OpFunction %void None %function
%entry = OpLabel
%workgroups = OpLoad %vec %wg
%y = OpCompositeExtract %uint %workgroups 1
%globalyptr = OpAccessChain %inputvalueptr %global %one
%globaly = OpLoad %uint %globalyptr
%counts = OpLoad %vec %groups
%county = OpCompositeExtract %uint %counts 1
%pp = OpAccessChain %pushvalueptr %push %zero
%index = OpLoad %uint %pp
%globalv = OpLoad %vec %global
%x = OpCompositeExtract %uint %globalv 0
%element = OpIAdd %uint %index %x
%length = OpArrayLength %uint %src 0
%r = OpAccessChain %valueptr %src %zero %element
%value = OpLoad %uint %r
%sumy = OpIAdd %uint %y %globaly
%countdelta = OpISub %uint %county %one
%lengthdelta = OpISub %uint %length %four
%extra1 = OpIAdd %uint %sumy %countdelta
%extra = OpIAdd %uint %extra1 %lengthdelta
%answer = OpIAdd %uint %value %extra
%p = OpAccessChain %valueptr %dst %zero %element
OpStore %p %answer
OpReturn
OpFunctionEnd
)";
        const auto context = spvContextCreate(SPV_ENV_VULKAN_1_3);
        spv_binary binary = nullptr;
        spv_diagnostic diagnostic = nullptr;
        const auto result = spvTextToBinary(context, text, std::strlen(text), &binary, &diagnostic);
        if (diagnostic) std::fprintf(stderr, "%s\n", diagnostic->error);
        assert(result == SPV_SUCCESS);
        source.assign(binary->code, binary->code + binary->wordCount);
        spvBinaryDestroy(binary);
        spvDiagnosticDestroy(diagnostic);
        spvContextDestroy(context);
    }
    std::string problem;
    const u32 bindings = input && argc > 2 ? std::stoul(argv[2]) : 2;
    const auto output = Vulkan::MakeDispatchBatchSpirv(source, 16, bindings, problem);
    if (output.empty()) std::fprintf(stderr, "%s\n", problem.c_str());
    assert(!output.empty()); // transformation includes independent SPIRV-Tools validation
    assert(Vulkan::MakeDispatchBatchSpirv(source, 1, bindings, problem).empty());
    assert(Vulkan::MakeDispatchBatchSpirv(source, 33, bindings, problem).empty());
    assert(Vulkan::MakeDispatchBatchSpirv({}, 16, bindings, problem).empty());
    if (!input) assert(Vulkan::MakeDispatchBatchSpirv(source, 16, 1, problem).empty());
    if (emit_fixture) {
        assert(argc == 3);
        std::ofstream f(argv[2], std::ios::binary);
        f.write(reinterpret_cast<const char*>(output.data()), output.size() * sizeof(u32));
        assert(f.good());
    }
    std::puts("dispatch batch SPIR-V: OK (Vulkan 1.3 validator)");
}
