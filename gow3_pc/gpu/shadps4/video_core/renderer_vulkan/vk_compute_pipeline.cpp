// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <boost/container/small_vector.hpp>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_dispatch_batch_spirv.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    auto [pipeline_result, pipe] =
        instance.GetDevice().createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Compute Pipeline {}", debug_str);
}

namespace {
std::atomic<u64> g_run_pools_created{0};
} // namespace

ComputePipeline::~ComputePipeline() {
    // Run pools go once the GPU is past the current tick (never a wait on a tick that may not
    // be submitted yet).
    if (run_pool) {
        run_pools_retired.emplace_back(run_pool, scheduler.GetWorkSemaphore()->CurrentTick());
    }
    if (run_pools_retired.empty()) {
        return;
    }
    scheduler.DeferOperation([device = instance.GetDevice(), pools = std::move(run_pools_retired)] {
        for (const auto& [pool, tick] : pools) {
            device.destroyDescriptorPool(pool);
        }
    });
}

bool ComputePipeline::EnsureBatchPipeline() const {
    return EnsureBatchVariant(fusion, BatchCapacity, "Dispatch fusion");
}

bool ComputePipeline::EnsureRunPipeline() const {
    return EnsureBatchVariant(run, RunCapacity, "Dispatch runs");
}

bool ComputePipeline::EnsureBatchVariant(BatchVariant& variant, u32 capacity,
                                         const char* name) const {
    if (variant.attempted) return bool(variant.pipeline);
    variant.attempted = true;
    const auto& info = GetStage(Shader::SwStage::Compute);
    const u32 bindings = info.buffers.size() + 1; // extra array replaces PushData
    const auto limits = instance.GetPhysicalDevice().getProperties().limits;
    if (info.pgm_hash != 0x8f067540 || info.buffers.size() != 10 ||
        !info.images.empty() || !info.samplers.empty() ||
        !instance.SupportsStorageBufferArrayIndexing() ||
        bindings * capacity > limits.maxPerStageDescriptorStorageBuffers ||
        bindings * capacity > limits.maxDescriptorSetStorageBuffers ||
        bindings * capacity > limits.maxPerStageResources) return false;
    for (u32 i = 0; i < 9; ++i) {
        if (info.buffers[i].IsSpecial() || bool(info.buffers[i].is_written) != (i >= 6))
            return false;
    }
    if (info.buffers[9].buffer_type != Shader::BufferType::Flatbuf) return false;
    const auto binary = LoadBatchComputeBinary(info.pgm_hash, compute_key.value);
    std::string problem;
    const auto batch = MakeDispatchBatchSpirv(binary, capacity, info.buffers.size(), problem);
    if (batch.empty()) {
        std::printf("%s: shader %llx not eligible: %s\n", name, info.pgm_hash, problem.c_str());
        return false;
    }
    const auto device = instance.GetDevice();
    const vk::ShaderModule module = CompileSPV(batch, device);
    std::vector<vk::DescriptorSetLayoutBinding> layout_bindings;
    for (u32 binding = 0; binding < bindings; ++binding) {
        layout_bindings.push_back({.binding = binding, .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = capacity, .stageFlags = vk::ShaderStageFlagBits::eCompute});
    }
    const vk::DescriptorSetLayoutCreateInfo layout_ci{.bindingCount = bindings,
                                                      .pBindings = layout_bindings.data()};
    // 24 Oct 2026: 704 descriptors in one set (the run variant) may pass the per-stage limits
    // above and still exceed maxPerSetDescriptors (Vulkan 1.1): ask the driver first.
    if (!device.getDescriptorSetLayoutSupport(layout_ci).supported) {
        device.destroyShaderModule(module);
        std::printf("%s: shader %llx: a set of %u x %u storage buffers is not supported\n", name,
                    info.pgm_hash, bindings, capacity);
        return false;
    }
    auto [desc_result, descriptors] = device.createDescriptorSetLayoutUnique(layout_ci);
    if (desc_result != vk::Result::eSuccess) { device.destroyShaderModule(module); return false; }
    variant.descriptors = std::move(descriptors);
    const vk::DescriptorSetLayout descriptor_layout = *variant.descriptors;
    auto [layout_result, layout] = device.createPipelineLayoutUnique(
        {.setLayoutCount = 1, .pSetLayouts = &descriptor_layout});
    if (layout_result != vk::Result::eSuccess) { device.destroyShaderModule(module); return false; }
    variant.layout = std::move(layout);
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{.requiredSubgroupSize = 64};
    const vk::PipelineShaderStageCreateInfo shader{
        .pNext = instance.IsSubgroupSize64Supported() ? &subgroup : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"};
    auto [result, pipeline] = device.createComputePipelineUnique({}, {.stage = shader, .layout = *variant.layout});
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) return false;
    variant.pipeline = std::move(pipeline);
    std::printf("%s: shader %llx validated, %u instances per dispatch, %u bindings\n", name,
                info.pgm_hash, capacity, bindings);
    return true;
}

vk::DescriptorSet ComputePipeline::BatchDescriptorSet() const {
    return desc_heap.Commit(*fusion.descriptors);
}

// gow3 24 Oct 2026: a run set holds 11 arrays of 64 storage descriptors (704). The shared heap
// allocates 32 sets at a time from pools of 8,192 storage descriptors (vk_pipeline_cache.cpp):
// one such batch does not fit, so these sets come from pools of their own, one set at a time. A
// full pool is reused once the GPU is past the tick it was retired at (U3 Cartagena, runv4: ~81
// sets a frame, 1,522 dispatches in runs of 18.8).
vk::DescriptorSet ComputePipeline::RunDescriptorSet() const {
    constexpr u32 SetsPerPool = 64;
    const auto device = instance.GetDevice();
    auto* semaphore = scheduler.GetWorkSemaphore();
    if (!run_pool || run_pool_sets == SetsPerPool) {
        if (run_pool) {
            run_pools_retired.emplace_back(run_pool, semaphore->CurrentTick());
            run_pool = vk::DescriptorPool{};
        }
        if (!run_pools_retired.empty() && !semaphore->IsFree(run_pools_retired.front().second)) {
            semaphore->Refresh();
        }
        if (!run_pools_retired.empty() && semaphore->IsFree(run_pools_retired.front().second)) {
            run_pool = run_pools_retired.front().first;
            run_pools_retired.pop_front();
            const auto reset = device.resetDescriptorPool(run_pool);
            ASSERT_MSG(reset == vk::Result::eSuccess, "Failed to reset a run descriptor pool: {}",
                       vk::to_string(reset));
        } else {
            const auto& info = GetStage(Shader::SwStage::Compute);
            const vk::DescriptorPoolSize size{
                .type = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = u32(info.buffers.size() + 1) * RunCapacity * SetsPerPool,
            };
            auto [result, pool] = device.createDescriptorPool(
                {.maxSets = SetsPerPool, .poolSizeCount = 1, .pPoolSizes = &size});
            ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create a run descriptor pool: {}",
                       vk::to_string(result));
            run_pool = pool;
            g_run_pools_created.fetch_add(1, std::memory_order_relaxed);
        }
        run_pool_sets = 0;
    }
    const vk::DescriptorSetLayout layout = *run.descriptors;
    const vk::DescriptorSetAllocateInfo alloc{
        .descriptorPool = run_pool, .descriptorSetCount = 1, .pSetLayouts = &layout};
    vk::DescriptorSet set{};
    const auto result = device.allocateDescriptorSets(&alloc, &set);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to allocate a run descriptor set: {}",
               vk::to_string(result));
    ++run_pool_sets;
    return set;
}

u64 ComputePipeline::RunPoolsCreated() {
    return g_run_pools_created.load(std::memory_order_relaxed);
}

} // namespace Vulkan
