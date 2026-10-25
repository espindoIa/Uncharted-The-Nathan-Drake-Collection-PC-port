// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Serialization {
struct Archive;
}

namespace Vulkan {

class Instance;
class Scheduler;
class DescriptorHeap;

struct ComputePipelineKey {
    size_t value;

    friend bool operator==(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return lhs.value == rhs.value;
    }
    friend bool operator!=(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return !(lhs == rhs);
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

class ComputePipeline : public Pipeline {
public:
    struct SerializationSupport {
        u32 dummy{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    ComputePipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                    const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
                    ComputePipelineKey compute_key, const Shader::Info& info,
                    vk::ShaderModule module, SerializationSupport& sdata, bool preloading);
    ~ComputePipeline();

    static constexpr u32 BatchCapacity = 16;
    bool EnsureBatchPipeline() const;
    vk::Pipeline BatchHandle() const { return *fusion.pipeline; }
    vk::PipelineLayout BatchLayout() const { return *fusion.layout; }
    vk::DescriptorSet BatchDescriptorSet() const;

    /// gow3 24 Oct 2026 (Gow3Toggle::DispatchRuns): the same transform for a run of up to 64
    /// instances. Built on the GPU command thread (stage A); the draw recording thread only
    /// reads it once a run packet reached it.
    static constexpr u32 RunCapacity = 64;
    bool EnsureRunPipeline() const;
    bool HasRunPipeline() const { return bool(run.pipeline); }
    vk::Pipeline RunHandle() const { return *run.pipeline; }
    vk::PipelineLayout RunLayout() const { return *run.layout; }
    /// Stage B: a set of the run layout from this pipeline's own pools.
    vk::DescriptorSet RunDescriptorSet() const;
    /// Run descriptor pools created so far (all pipelines).
    static u64 RunPoolsCreated();

private:
    struct BatchVariant {
        bool attempted = false;
        vk::UniquePipeline pipeline;
        vk::UniquePipelineLayout layout;
        vk::UniqueDescriptorSetLayout descriptors;
    };
    bool EnsureBatchVariant(BatchVariant& variant, u32 capacity, const char* name) const;

    ComputePipelineKey compute_key;
    mutable BatchVariant fusion, run;
    mutable vk::DescriptorPool run_pool{};
    mutable u32 run_pool_sets = 0;
    mutable std::deque<std::pair<vk::DescriptorPool, u64>> run_pools_retired;
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::ComputePipelineKey> {
    std::size_t operator()(const Vulkan::ComputePipelineKey& key) const noexcept {
        return std::hash<size_t>{}(key.value);
    }
};
