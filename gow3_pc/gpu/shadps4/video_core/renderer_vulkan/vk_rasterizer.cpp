#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <filesystem>
#include <boost/container/small_vector.hpp>
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>
#include "video_core/renderer_vulkan/hud_layout.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "gow3_flight.h"
#include "gow3_hdtex.h"
#include "gow3_settings.h"
#include "gow3_toggles.h"
#include "gow3_trigger_files.h"
#include "gow3_watch_range.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "common/debug.h"
#include "common/decoder.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "gow3_threads.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_skinning_writes.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include <vk_mem_alloc.h>

#include <optional>
#include "core/platform.h"

// page_manager.cpp: the cheap protected-pages test and the stats of Rasterizer::PrepareCpuWrite.
namespace VideoCore {
bool Gow3AnyPageWatched(VAddr addr, u64 size);
bool Gow3PageOnlyWriteWatched(VAddr addr);
extern std::atomic<u64> g_range_prepares, g_range_prepare_bytes, g_range_prepare_ns,
    g_range_prepare_pages, g_range_prepare_discarded;
// Write fault split (GOW3_FAULT_STATS): ns in buffer / texture / extend / landing, and calls.
extern std::atomic<u64> g_wf_split_ns[4], g_wf_split_calls;
} // namespace VideoCore

namespace Vulkan {
// Set by HandleWriteAt while timing its InvalidateMemory (GOW3_FAULT_STATS only).
static thread_local bool g_wf_split_active = false;
using VideoCore::g_range_prepare_pages;
using VideoCore::Gow3PageOnlyWriteWatched;
using VideoCore::Gow3AnyPageWatched;
using VideoCore::g_range_prepares;
using VideoCore::g_range_prepare_bytes;
using VideoCore::g_range_prepare_ns;

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

extern "C" void runtime_exec_watch_retarget(uint64_t address);
namespace {
// gow3 16 Oct: a depth image sampled by a draw that also has it as its depth attachment
// (Uncharted 3's half-resolution depth, 960x540 D32S8, with depth writes on) used to be bound as
// a texture in DepthStencilReadOnlyOptimal and then moved to the attachment layout before the
// render pass: the descriptor named a stale layout and every such draw had two transitions
// (Vulkan synchronization validation: write-after-write and read-after-write hazards). On NVIDIA
// objects flickered as flat-coloured shapes. Both uses now share the general layout, like a
// colour target in a feedback loop.
constexpr vk::ImageLayout DepthFeedbackLayout = vk::ImageLayout::eGeneral;
constexpr vk::PipelineStageFlags2 DepthFeedbackStages =
    vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests |
    vk::PipelineStageFlagBits2::eAllGraphics;
constexpr vk::AccessFlags2 DepthFeedbackAccess = vk::AccessFlagBits2::eShaderRead |
                                                 vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                                 vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
// Opt-in (GOW3_DEPTH_FEEDBACK_GENERAL=1) until tested in Uncharted 1 and 2; it removed the
// validation hazards in Uncharted 3 but not its flicker.
bool DepthFeedbackGeneral() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_DEPTH_FEEDBACK_GENERAL");
        return env && env[0] == '1';
    }();
    return enabled;
}
// gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1, low-end CPUs): small savings on the GPU command thread's
// per-draw path that change no result (each one names this switch).
bool CpuTrims() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_CPU_TRIMS");
        return env && env[0] == '1';
    }();
    return enabled;
}
// gow3: live trigger files (some are set for every player) are looked for at most every 250 ms;
// a file-system call per submission or frame is a kernel round trip each time.
bool TriggerPollDue(std::chrono::steady_clock::time_point& last) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(250)) {
        return false;
    }
    last = now;
    return true;
}
} // namespace
namespace {
bool SrtCheckEnabled();
void NoteSrtWalk(const Shader::Info& info);
extern Rasterizer* g_srt_check_rasterizer;
bool LateConstantsEnabled();
void NoteLateGuestCopy(VideoCore::Buffer& buffer, u64 offset, VAddr address, u32 size, u64 due);
void NoteLateFlatCopy(VideoCore::Buffer& buffer, u64 offset, const Shader::Info& stage, u32 size,
                      u64 due);
void NoteLateDue(u64 ring_end);
void RefreshLateConstants(Core::MemoryManager* memory);

struct GarbageSharp {
    bool unbound;
    bool garbage;
    u64 mapped_size;
};

// gow3: the garbage V# predicate, shared by binding and (through the hook below) shader
// specialization. See the comments at the binding site.
GarbageSharp ClassifyGarbageSharp(Core::MemoryManager* memory, const AmdGpu::Buffer& vsharp,
                                  u64 pgm_hash) {
    const bool unbound = vsharp.base_address == 0 || vsharp.GetSize() == 0;
    const u64 mapped_size =
        unbound ? 0 : memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
    // gow3 16 Oct: the treasure effect's float slots can also make a V# that lands in mapped
    // memory: after 90 minutes one with an unaligned base, a stride of ~15 KB and ~1e9
    // records was bound, the GPU hung (OBS's own device was removed with DEVICE_HUNG at the
    // same moment) and the game stopped with "Device lost during submit". For these two
    // shaders an unaligned base, a stride of 8 KiB or more or over 64 MiB is garbage too.
    // Upstream 0.5.0 (20 Oct night): cs 0c936dd6 of the same family too (Shader::IsEffectShader).
    const bool treasure_effect = Shader::IsEffectShader(pgm_hash);
    const bool garbage =
        !unbound && (mapped_size >= 1_GB ||
                     (mapped_size != vsharp.GetSize() && vsharp.num_records < 0xFFFF0000u) ||
                     (treasure_effect && ((vsharp.base_address & 3) != 0 || vsharp.stride >= 8192 ||
                                          vsharp.GetSize() > 64_MB)));
    return {unbound, garbage, mapped_size};
}

// gow3 21 Oct: specialization hook. Binding nulls garbage V#s, but specialization read their
// random stride/format: the player's U3 log had a 6.1 s frame compiling 263 permutations of
// 8f067540 (cache held 1,095 of them); an offline replay with this filter gave 1.
bool SpecGarbageFilter(const AmdGpu::Buffer& vsharp, u64 pgm_hash) {
    if (Gow3Toggle::Disabled2(Gow3Toggle::SpecGarbageSharps)) {
        return false;
    }
    return ClassifyGarbageSharp(Core::Memory::Instance(), vsharp, pgm_hash).garbage;
}

/// gow3 22 Oct 2026: Gow3Toggle::DispatchStageAMemos hits and misses, per frame on the
/// GOW3_DISPATCH_STATS lines. Stage A only, but for the filter skips (any specializing thread).
struct StageAMemoStats {
    std::atomic<u64> filter_skips{0}; ///< specializations whose effect filter checks were skipped
    u64 sharps_reused = 0, sharps_read = 0; ///< pipelined dispatches: V#s reused / read again
    u64 tokens_reused = 0, tokens_asked = 0; ///< ring watch tokens reused / asked for
    u64 label_skips = 0, label_scans = 0;    ///< SRT pipe check label scans skipped / done
    /// GOW3_STAGEA_VERIFY=1 (Shader::StageAVerify): totals where the old answer differed. The
    /// effect filter's count is Shader::g_filter_skip_mismatches (specializing threads).
    u64 sharp_mismatches = 0, token_mismatches = 0, label_mismatches = 0;
};
StageAMemoStats g_stage_a_memos;
} // namespace

/// gow3 24 Oct 2026 (Gow3Toggle::DispatchRuns): stage B's plan for one run packet: per instance
/// the descriptors and push data the per-dispatch path binds, and the output write spans.
struct Rasterizer::RunPlan {
    static constexpr u32 Capacity = ComputePipeline::RunCapacity;
    static constexpr u32 Bindings = 11; ///< the 10 buffers, then the push data array
    static constexpr u32 MaxSpans = Capacity;
    static constexpr u32 MaxTracked = 32; ///< tracked ranges (bound_buffers holds 40)
    std::array<std::array<vk::DescriptorBufferInfo, Capacity>, Bindings> infos;
    std::array<Shader::PushData, Capacity> push;
    std::array<SkinningWriteInstance, Capacity> writes;
    std::array<SkinningWriteSpan, MaxSpans> spans;
    std::array<std::pair<const VideoCore::Buffer*, u64>, MaxSpans> obtained; ///< at span.address
    std::array<std::array<std::pair<const VideoCore::Buffer*, u64>, 3>, Capacity> view_obtained;
    struct Tracked {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u64 size;
    };
    std::array<Tracked, MaxSpans> tracked;
    std::array<u8, Capacity> span_of;
    u32 num_spans = 0, num_tracked = 0;
    /// GOW3_DISPATCH_RUN_VERIFY: what the old path bound per instance (VerifyRunInstance).
    std::array<std::array<vk::DescriptorBufferInfo, Capacity>, Bindings - 1> replayed;
    std::array<Shader::PushData, Capacity> replayed_push;
    std::array<std::array<Tracked, 4>, Capacity> replayed_tracked; ///< its written ranges
    std::array<u8, Capacity> replayed_tracked_count;
    std::array<bool, Capacity> captured;
    std::array<u8, Capacity * 256> push_bytes; ///< storage offset alignment is at most 256
};

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    Shader::g_garbage_buffer_filter.store(&SpecGarbageFilter, std::memory_order_relaxed);
    // Before the rasterizer is bound: Liverpool enqueues buffers only once it sees it.
    draw_prep = std::make_unique<DrawPreparation>(pipeline_cache);
    // The effect shaders' garbage V# rule (shader_recompiler/resource.h) needs the guest mappings.
    Shader::gpu_mapped_extent = [](u64 address, u64 size) {
        return Core::Memory::Instance()->ClampRangeSize(address, size);
    };
    // Merge of upstream 0.5.0: its GetSharp filter follows the fork's live A/B bit too.
    Shader::garbage_sharp_filter_on = [] {
        return !Gow3Toggle::Disabled2(Gow3Toggle::SpecGarbageSharps);
    };
    // gow3 22 Oct 2026: for the effect family, Shader::IsGarbageEffectBuffer (in GetSharp) and
    // SpecGarbageFilter apply the same rule to the same V# through the same ClampRangeSize call
    // (unbound: not garbage; odd: garbage; else the mapped-size test), so a V# GetSharp returned
    // passes SpecGarbageFilter as well. Only while GetSharp runs the full rule (its env switch on).
    Shader::g_buffer_filter_repeats_get_sharp = [](u64 pgm_hash) {
        static const bool effect_rule = [] {
            const char* env = std::getenv("GOW3_EFFECT_SHARP_FILTER"); // as IsGarbageEffectBuffer
            return !(env && env[0] == '0');
        }();
        const bool skip = effect_rule && Shader::IsEffectShader(pgm_hash) &&
                          Shader::gpu_mapped_extent != nullptr &&
                          !Gow3Toggle::Disabled2(Gow3Toggle::DispatchStageAMemos);
        if (skip) { // a statistic: no locked add per dispatch (other threads rarely count)
            auto& n = g_stage_a_memos.filter_skips;
            n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        return skip;
    };
    scene_targets = std::make_unique<SceneTargets>(instance, scheduler, runtime, texture_cache);
    // Object motion first: it fixes the buffer addresses the motion shader variants embed.
    object_motion = std::make_unique<ObjectMotion>(instance, scheduler);
    camera_motion = std::make_unique<CameraMotion>(instance, scheduler, texture_cache, runtime);
    camera_motion->SetObjectMotion(object_motion.get());
    upscaler = std::make_unique<TemporalUpscaler>(instance, scheduler, texture_cache, runtime,
                                                  *camera_motion, *scene_targets);
    hud_aspect = HudLayout::AspectFromEnv();
    hud_wide_aspect = HudLayout::AspectFromEnv("GOW3_HUD_WIDE_ASPECT");
    hud_tracker.tonemap_ps = HudLayout::TonemapFromEnv();
    hud_tracker.composite_ps = HudLayout::CompositeFromEnv();
    if (const char* env = std::getenv("GOW3_HUD_TRACE")) {
        hud_trace = env[0] == '1';
    }
    hud_wide = HudLayout::WideFromEnv();
    if (hud_aspect > 0.0f) {
        std::printf("HUD: drawn in the centred %.4f:1 area (over the frame after pixel shader "
                    "%08llx, after its copy %08llx)\n", hud_aspect,
                    static_cast<unsigned long long>(hud_tracker.tonemap_ps),
                    static_cast<unsigned long long>(hud_tracker.composite_ps));
    }
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
        // gow3: compute shaders seen ahead of this thread compile on the compiler workers.
        liverpool->SetComputePrecompileHook(
            [this](const AmdGpu::ComputeProgram& cs) { pipeline_cache.PrecompileCompute(cs); });
    }
    memory->SetRasterizer(this);
    static Rasterizer* srt_rasterizer;
    static const bool srt_sync = [] {
        const char* env = std::getenv("GOW3_SRT_SYNC");
        return !env || env[0] != '0';
    }();
    if (srt_sync) {
        srt_rasterizer = this;
        Shader::g_srt_pre_walk = [](const Shader::Info& info) { srt_rasterizer->SyncSrtTables(info); };
    }
    static Rasterizer* report_rasterizer;
    report_rasterizer = this;
    Shader::g_srt_fault_report = [](const Shader::Info& info) { report_rasterizer->ReportSrtFault(info); };
    if (SrtCheckEnabled()) {
        g_srt_check_rasterizer = this;
        Shader::g_srt_post_walk = &NoteSrtWalk;
    }

    GpuProfiler::Init(instance, scheduler);
    if (DrawPipeWanted()) {
        constant_ring = std::make_unique<ConstantRing>(instance, scheduler);
        // staticmesh1/2 (23 Oct): no material FPS gain yet. Keep the structural prototype
        // opt-in and do not allocate its 64 MiB for players until it earns its residency.
        const char* snapshots = std::getenv("GOW3_STATIC_MESH_SNAPSHOTS");
        if (snapshots && snapshots[0] == '1') {
            static_mesh_buffer = std::make_unique<VideoCore::Buffer>(
                instance, 0, StaticMeshCapacity, VideoCore::MemoryType::Stream, "static crowd snapshots");
            ASSERT(!static_mesh_buffer->mapped_data.empty());
        }
        draw_pipe = std::make_unique<DrawPipe>(&RunDrawPacket, this, &FlushDrawPipeSignals, this);
    }
    // gow3: this thread joins the texture binding helper before it changes image state.
    runtime.SetImageAccessHook(&JoinBindHelper, this);
    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        if (LateConstantsEnabled()) {
            RefreshLateConstants(memory);
        }
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() {
    if (draw_pipe) draw_pipe->Drain();
    scheduler.FlushDeferredRecord(); // callback must not outlive this rasterizer
    liverpool->SetComputePrecompileHook({}); // before the pipeline cache goes
    Shader::g_srt_pre_walk = nullptr;
}

inline const AmdGpu::Regs& Rasterizer::Regs() const {
    return stage_regs ? *stage_regs : liverpool->regs;
}

inline AmdGpu::CbDbExtent Rasterizer::CbExtent(u32 index) const {
    return stage_regs ? pipe_cb_extent[index] : liverpool->last_cb_extent[index];
}

inline const AmdGpu::ComputeProgram& Rasterizer::CsRegs() const {
    return stage_cs ? *stage_cs : liverpool->GetCsRegs();
}

inline AmdGpu::CbDbExtent Rasterizer::DbExtent() const {
    return stage_regs ? pipe_db_extent : liverpool->last_db_extent;
}

namespace {
// gow3 16 Oct: late constants (GOW3_LATE_CONSTANTS=1). Uncharted 3 fills some per-object constants
// after it submitted the draws that read them: a PS4 GPU reads them when it runs the draw, here
// they were copied when the draw was processed. About 0.7% of resource-table walks read zeros the
// game replaced before the submission (GOW3_SRT_CHECK), and objects flickered as flat-coloured
// shapes. Constant copies and flattened resource tables made for the GPU are compared with guest
// memory right before their command buffer is submitted, and refreshed where the game changed them.
// gow3 21 Oct 2026: made cheap enough for multithreaded rendering (Uncharted 2 needs it there:
// Drake, his gun and train cars drew flat white for single frames). Before, every copy took one
// global lock and allocated a copy of its user data, and at each submission the submitting thread
// (the draw recording thread, which limits the frame rate with multithreaded rendering) copied
// every source again and compared it with the copy, ~2 million copies per 5 s, ~0.05% changed.
// Now a note is a fixed record in blocks that never move, with an XXH3 hash of the bytes copied
// (taken by the thread that made the copy, mostly the GPU command thread, which waits for the
// recording thread most of the time). The check hashes each distinct source of the submission
// once (a guest range, or a table walk with its user data) and compares hashes; only copies whose
// source changed are copied or walked again. Copies into the constant ring are checked at the
// submission of their own draw: the GPU command thread copies up to a frame ahead of the draw
// recording thread, and before, the next submission checked them, often long before their draw
// was submitted (GOW3_LATE_CONSTANTS_DEFER=0: as before).
struct LateConstant {
    VideoCore::Buffer* buffer;
    u64 offset;
    u64 due;                      // constant ring copies: ring position after the copy (else 0)
    u64 hash;                     // XXH3 of the bytes copied
    VAddr address;                // guest bytes copied (0: flattened user data)
    Shader::PFN_SrtWalker walker; // flattened user data: the walk to repeat
    u32 size;
    u32 ud_size;
    std::array<u32, Shader::NUM_USER_DATA_REGS> user_data; // the first ud_size are set
};

// Notes in blocks: the copying thread appends under the lock; the submitting thread reads the
// notes published so far without it (a block never moves) and recycles the blocks it passed.
class LateQueue {
public:
    static constexpr u32 BlockSize = 2048;
    static constexpr u64 MaxPending = 200000; // as before: beyond this, copies go unchecked

    template <typename Fill>
    void Push(Fill&& fill) {
        std::scoped_lock lock{mutex};
        if (pending >= MaxPending) {
            return;
        }
        if (!tail || tail->count == BlockSize) {
            Block* block = TakeBlock();
            if (tail) {
                tail->next = block;
            } else {
                head = block;
                read = 0;
            }
            tail = block;
        }
        fill(tail->items[tail->count++]);
        ++pending;
    }

    /// The submitting thread: `process` on each note in order while `due` holds for it, then
    /// `done_reading` (the notes may be reused after it).
    template <typename Due, typename Process, typename DoneReading>
    void Consume(Due&& due, Process&& process, DoneReading&& done_reading) {
        Block* block;
        Block* end;
        u32 at, end_count;
        {
            std::scoped_lock lock{mutex};
            if (!head) {
                return;
            }
            block = head;
            at = read;
            end = tail;
            end_count = tail->count;
        }
        u64 done = 0;
        for (;;) {
            const u32 count = block == end ? end_count : BlockSize;
            if (at == count) {
                if (block == end) {
                    break;
                }
                block = block->next;
                at = 0;
                continue;
            }
            const LateConstant& entry = block->items[at];
            if (!due(entry)) {
                break;
            }
            process(entry);
            ++at;
            ++done;
        }
        done_reading();
        std::scoped_lock lock{mutex};
        while (head != block) {
            Block* passed = head;
            head = passed->next;
            passed->count = 0;
            passed->next = nullptr;
            free_blocks.push_back(passed);
        }
        read = at;
        if (head == tail && read == head->count) {
            head->count = 0; // all read: appends start at the block's start again
            read = 0;
        }
        pending -= done;
    }

    [[nodiscard]] u64 Pending() {
        std::scoped_lock lock{mutex};
        return pending;
    }

private:
    struct Block {
        std::array<LateConstant, BlockSize> items;
        u32 count;
        Block* next;
    };

    Block* TakeBlock() {
        Block* block;
        if (free_blocks.empty()) {
            block = new Block; // kept for the process
        } else {
            block = free_blocks.back();
            free_blocks.pop_back();
        }
        block->count = 0;
        block->next = nullptr;
        return block;
    }

    std::mutex mutex;
    Block* head = nullptr; // first block with notes not yet read
    Block* tail = nullptr; // appended to
    u32 read = 0;          // next note to read in head
    u64 pending = 0;
    std::vector<Block*> free_blocks;
};

LateQueue g_late_ring;                 // constant ring copies, in ring order (GPU command thread)
LateQueue g_late_now;                  // other copies: checked at the next submission
std::atomic<u64> g_late_due_end{0};    // ring position after the draw being recorded
std::atomic<u64> g_late_refreshed{0}, g_late_checked{0}, g_late_bytes{0}, g_late_sources{0},
    g_late_walks{0}, g_late_refreshed_walks{0};
// GOW3_LATE_CONSTANTS_VERIFY=1 (diagnostics, costs what the old check did): each check also reads
// its source again and compares bytes with the copy, as before 21 Oct: copies changed since the
// note (should be 0) and checks where the byte compare and the hash compare disagree.
std::atomic<u64> g_late_verified{0}, g_late_copy_changed{0}, g_late_missed{0}, g_late_extra{0};

bool LateConstantsVerify() {
    static const bool verify = [] {
        const char* env = std::getenv("GOW3_LATE_CONSTANTS_VERIFY");
        return env && env[0] == '1';
    }();
    return verify;
}

/// GOW3_RING_REUSE_CHECK=1: every reused constant ring copy is compared with guest memory.
bool RingReuseCheck() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_RING_REUSE_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

bool LateConstantsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_LATE_CONSTANTS");
        return env && env[0] == '1';
    }();
    return enabled;
}

bool LateConstantsDefer() {
    static const bool defer = [] {
        const char* env = std::getenv("GOW3_LATE_CONSTANTS_DEFER");
        return !(env && env[0] == '0');
    }();
    return defer;
}

/// `due`: the constant ring position after the copy (checked at the submission of its draw), or 0
/// (checked at the next submission).
void NoteLateGuestCopy(VideoCore::Buffer& buffer, u64 offset, VAddr address, u32 size, u64 due) {
    const u64 hash = XXH3_64bits(buffer.mapped_data.data() + offset, size);
    (due ? g_late_ring : g_late_now).Push([&](LateConstant& entry) {
        entry.buffer = &buffer;
        entry.offset = offset;
        entry.due = due;
        entry.hash = hash;
        entry.address = address;
        entry.walker = nullptr;
        entry.size = size;
        entry.ud_size = 0;
    });
}

void NoteLateFlatCopy(VideoCore::Buffer& buffer, u64 offset, const Shader::Info& stage, u32 size,
                      u64 due) {
    if (!stage.srt_info.walker_func) {
        return; // only user data registers: nothing in memory to change
    }
    const auto user_data = stage.UserData();
    const u32 ud_size = static_cast<u32>(std::min<size_t>(user_data.size(), Shader::NUM_USER_DATA_REGS));
    const u64 hash = XXH3_64bits(buffer.mapped_data.data() + offset, size);
    (due ? g_late_ring : g_late_now).Push([&](LateConstant& entry) {
        entry.buffer = &buffer;
        entry.offset = offset;
        entry.due = due;
        entry.hash = hash;
        entry.address = 0;
        entry.walker = stage.srt_info.walker_func;
        entry.size = size;
        entry.ud_size = ud_size;
        std::memcpy(entry.user_data.data(), user_data.data(), ud_size * sizeof(u32));
    });
}

/// The draw recording thread, per packet: copies up to `ring_end` belong to draws recorded by now.
void NoteLateDue(u64 ring_end) {
    g_late_due_end.store(ring_end, std::memory_order_release);
}

/// The flattened tables `entry` would read now (user data, then the walk), in `flat`; false
/// when the walk faulted (stale pointers: left as copied, as before).
bool WalkLateTables(const LateConstant& entry, u32* flat) {
    const size_t dwords = entry.size / sizeof(u32);
    std::fill(flat, flat + dwords, 0u);
    std::memcpy(flat, entry.user_data.data(), std::min<size_t>(entry.ud_size, dwords) * sizeof(u32));
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        return false;
    }
    runtime_fault_recover = &recover;
    entry.walker(entry.user_data.data(), flat);
    runtime_fault_recover = nullptr;
    return true;
}

/// Runs in Scheduler::SubmitExecution under its submit lock (one submitting thread at a time).
void RefreshLateConstants(Core::MemoryManager* memory) {
    // Distinct sources of this submission (open addressing on a key hash): what each reads now.
    struct Source {
        u64 key;
        const LateConstant* first; // null: free slot
        u64 hash;                  // XXH3 of the bytes now
        bool valid;                // false: the walk faulted
    };
    static constexpr u32 TableSize = 16384, TableLimit = 12288;
    static std::vector<Source> table(TableSize);
    static std::vector<u32> used;
    static std::vector<u8> now, refreshed;
    u64 checked = 0, changed = 0, bytes = 0, sources = 0, walks = 0, changed_walks = 0;
    u64 verified = 0, copy_changed = 0, missed = 0, extra = 0;
    const auto same_source = [](const LateConstant& a, const LateConstant& b) {
        if (a.address || b.address) {
            return a.address == b.address && a.size == b.size;
        }
        return a.walker == b.walker && a.size == b.size && a.ud_size == b.ud_size &&
               std::memcmp(a.user_data.data(), b.user_data.data(), a.ud_size * sizeof(u32)) == 0;
    };
    // XXH3 of the bytes `entry`'s source holds now; false when unreadable (faulted walk).
    const auto hash_now = [&](const LateConstant& entry, u64& hash) {
        ++sources;
        if (now.size() < entry.size + sizeof(u32)) {
            now.resize(entry.size + sizeof(u32));
        }
        if (entry.address) {
            memory->CopySparseMemory(entry.address, now.data(), entry.size);
        } else {
            ++walks;
            if (!WalkLateTables(entry, reinterpret_cast<u32*>(now.data()))) {
                return false;
            }
        }
        hash = XXH3_64bits(now.data(), entry.size);
        return true;
    };
    const auto check = [&](const LateConstant& entry) {
        const u64 key = entry.address
                            ? XXH3_64bits_withSeed(&entry.address, sizeof(entry.address), entry.size)
                            : XXH3_64bits_withSeed(entry.user_data.data(), entry.ud_size * sizeof(u32),
                                                   reinterpret_cast<u64>(entry.walker) ^
                                                       (u64(entry.size) << 40));
        u64 hash = 0;
        bool valid = false;
        u32 slot = static_cast<u32>(key) & (TableSize - 1);
        for (;; slot = (slot + 1) & (TableSize - 1)) {
            Source& source = table[slot];
            if (!source.first) {
                valid = hash_now(entry, hash);
                if (used.size() < TableLimit) {
                    source = {key, &entry, hash, valid};
                    used.push_back(slot);
                }
                break;
            }
            if (source.key == key && same_source(*source.first, entry)) {
                hash = source.hash;
                valid = source.valid;
                break;
            }
        }
        if (!valid) {
            return;
        }
        ++checked;
        u8* dst = entry.buffer->mapped_data.data() + entry.offset;
        if (LateConstantsVerify()) {
            // The old check: the source read again, compared byte for byte with the copy.
            ++verified;
            if (XXH3_64bits(dst, entry.size) != entry.hash) {
                ++copy_changed;
            }
            if (refreshed.size() < entry.size + sizeof(u32)) {
                refreshed.resize(entry.size + sizeof(u32));
            }
            bool read = true;
            if (entry.address) {
                memory->CopySparseMemory(entry.address, refreshed.data(), entry.size);
            } else {
                read = WalkLateTables(entry, reinterpret_cast<u32*>(refreshed.data()));
            }
            if (read) {
                const bool bytes_differ = std::memcmp(dst, refreshed.data(), entry.size) != 0;
                missed += bytes_differ && hash == entry.hash;
                extra += !bytes_differ && hash != entry.hash;
            }
        }
        if (hash == entry.hash) {
            return;
        }
        // Changed since the copy: copied or walked again into it.
        if (entry.address) {
            memory->CopySparseMemory(entry.address, dst, entry.size);
        } else {
            if (refreshed.size() < entry.size + sizeof(u32)) {
                refreshed.resize(entry.size + sizeof(u32));
            }
            if (!WalkLateTables(entry, reinterpret_cast<u32*>(refreshed.data()))) {
                return;
            }
            std::memcpy(dst, refreshed.data(), entry.size);
        }
        entry.buffer->Flush(entry.offset, entry.size);
        ++changed;
        changed_walks += !entry.address;
        bytes += entry.size;
    };
    // The table points into the queue's blocks: emptied before the queue recycles them.
    const auto clear_table = [] {
        for (const u32 slot : used) {
            table[slot].first = nullptr;
        }
        used.clear();
    };
    const u64 due_end =
        LateConstantsDefer() ? g_late_due_end.load(std::memory_order_acquire) : ~u64{0};
    g_late_now.Consume([](const LateConstant&) { return true; }, [&](const LateConstant& entry) {
        check(entry);
    }, clear_table);
    g_late_ring.Consume([due_end](const LateConstant& entry) { return entry.due <= due_end; },
                        [&](const LateConstant& entry) { check(entry); }, clear_table);
    g_late_checked.fetch_add(checked, std::memory_order_relaxed);
    g_late_refreshed.fetch_add(changed, std::memory_order_relaxed);
    g_late_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_late_sources.fetch_add(sources, std::memory_order_relaxed);
    g_late_walks.fetch_add(walks, std::memory_order_relaxed);
    g_late_refreshed_walks.fetch_add(changed_walks, std::memory_order_relaxed);
    if (verified) {
        g_late_verified.fetch_add(verified, std::memory_order_relaxed);
        g_late_copy_changed.fetch_add(copy_changed, std::memory_order_relaxed);
        g_late_missed.fetch_add(missed, std::memory_order_relaxed);
        g_late_extra.fetch_add(extra, std::memory_order_relaxed);
    }
    static auto last = std::chrono::steady_clock::now();
    if (const auto t = std::chrono::steady_clock::now(); t - last >= std::chrono::seconds{5}) {
        last = t;
        const u64 refreshed_all = g_late_refreshed.exchange(0);
        const u64 refreshed_walks = g_late_refreshed_walks.exchange(0);
        std::printf("Late constants (5 s): %llu copies checked before submission, %llu refreshed "
                    "(%.1f KB; %llu guest copies, %llu table walks); %llu distinct sources read "
                    "(%llu table walks), %llu ring copies waiting for their draw\n",
                    static_cast<unsigned long long>(g_late_checked.exchange(0)),
                    static_cast<unsigned long long>(refreshed_all), g_late_bytes.exchange(0) / 1e3,
                    static_cast<unsigned long long>(refreshed_all - refreshed_walks),
                    static_cast<unsigned long long>(refreshed_walks),
                    static_cast<unsigned long long>(g_late_sources.exchange(0)),
                    static_cast<unsigned long long>(g_late_walks.exchange(0)),
                    static_cast<unsigned long long>(g_late_ring.Pending()));
        if (LateConstantsVerify()) {
            std::printf("Late constants verify (5 s): %llu checks compared byte for byte; %llu copies "
                        "changed since their note, %llu changes the hash check missed, %llu hash "
                        "changes with equal bytes\n",
                        static_cast<unsigned long long>(g_late_verified.exchange(0)),
                        static_cast<unsigned long long>(g_late_copy_changed.exchange(0)),
                        static_cast<unsigned long long>(g_late_missed.exchange(0)),
                        static_cast<unsigned long long>(g_late_extra.exchange(0)));
        }
        std::fflush(stdout);
    }
}

// gow3 diagnostics (GOW3_SRT_CHECK=1): a sample of resource-table walks (user data and the
// flattened result) is walked again at the next submission. A table the game changed in between
// was completed after the draw was processed: the walk read old descriptors (5 s statistics).
Rasterizer* g_srt_check_rasterizer = nullptr;

struct SrtWalkCheck {
    Shader::PFN_SrtWalker walker;
    size_t walker_size;
    u64 hash;
    std::vector<u32> user_data;
    std::vector<u32> flat;
    s64 walk_ns;
    // gow3 21 Oct 2026 (U3 museum red carpet): who walked (a draw-preparation worker walks ahead
    // of the GPU thread, and the GPU thread walks again and compares) and, for the GPU thread,
    // the draw: a late table only matters when the walk the GPU uses read it.
    bool worker;
    Rasterizer::SrtDrawContext draw;
};
s64 SrtNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::mutex g_srt_check_mutex;
std::vector<SrtWalkCheck> g_srt_checks;

bool SrtCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_SRT_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

void NoteSrtWalk(const Shader::Info& info) {
    static std::atomic<u32> sample{0};
    if ((sample.fetch_add(1, std::memory_order_relaxed) & 3) != 0) {
        return;
    }
    // Draw-preparation workers walk with a fault recovery point set (PipelineCache::GetProgram).
    const bool worker = runtime_fault_recover != nullptr;
    const Rasterizer::SrtDrawContext draw =
        !worker && g_srt_check_rasterizer && info.sw_stage != Shader::SwStage::Compute
            ? g_srt_check_rasterizer->CurrentDrawContext()
            : Rasterizer::SrtDrawContext{};
    std::scoped_lock lock{g_srt_check_mutex};
    if (g_srt_checks.size() >= 50000) {
        return;
    }
    g_srt_checks.push_back({info.srt_info.walker_func, info.srt_info.walker_func_size, info.pgm_hash,
                            std::vector<u32>(info.user_data.begin(), info.user_data.end()),
                            info.flattened_ud_buf, SrtNowNs(), worker, draw});
}

void CheckSrtWalks() {
    if (!SrtCheckEnabled()) {
        return;
    }
    std::vector<SrtWalkCheck> checks;
    {
        std::scoped_lock lock{g_srt_check_mutex};
        checks.swap(g_srt_checks);
    }
    static std::map<u64, std::pair<u64, u64>> stats; // shader -> (walks checked, changed)
    static std::map<std::string, u64> kinds;           // what the caches knew about the source
    static std::array<u64, 5> ages{};                  // walk to check: <2, <5, <10, <20, more ms
    static std::map<u64, u64> windows;                 // 1 MiB window of the source
    static auto last = std::chrono::steady_clock::now();
    static u32 described = 0;
    static u64 changed_gpu = 0, changed_worker = 0; // gow3 21 Oct 2026: by who walked
    std::vector<u32> now;
    for (const auto& check : checks) {
        now.assign(check.flat.size(), 0);
        std::memcpy(now.data(), check.user_data.data(),
                    std::min(check.user_data.size(), now.size()) * sizeof(u32));
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            runtime_fault_recover = nullptr;
            continue;
        }
        runtime_fault_recover = &recover;
        check.walker(check.user_data.data(), now.data());
        runtime_fault_recover = nullptr;
        auto& s = stats[check.hash];
        ++s.first;
        if (now != check.flat) {
            ++s.second;
            size_t first = 0;
            u32 differ = 0;
            for (size_t i = 0; i < now.size(); ++i) {
                if (now[i] != check.flat[i]) {
                    if (!differ++) {
                        first = i;
                    }
                }
            }
            const char* kind = "unknown";
            VAddr source = 0;
            if (g_srt_check_rasterizer) {
                source = g_srt_check_rasterizer->SrtSourceAddress(check.walker, check.walker_size,
                                                                  check.user_data, u32(first), &kind);
            }
            ++kinds[kind];
            ++windows[source >> 20];
            {
                const s64 age_ms = (SrtNowNs() - check.walk_ns) / 1000000;
                ++ages[age_ms < 2 ? 0 : age_ms < 5 ? 1 : age_ms < 10 ? 2 : age_ms < 20 ? 3 : 4];
            }
            // GOW3_SRT_WATCH=1: watch the first late-changed source for the game code writing it.
            static const bool watch = [] {
                const char* env = std::getenv("GOW3_SRT_WATCH");
                return env && env[0] == '1';
            }();
            static bool watching = false;
            if (watch && !watching && source) {
                watching = true;
                runtime_exec_watch_retarget(source);
            }
            ++(check.worker ? changed_worker : changed_gpu);
            // gow3 21 Oct 2026: GOW3_SRT_CHECK_LINES=<n> (default 16) such lines per run; GPU-thread
            // walks name their draw (colour target 0, depth, index buffer and count) to match a
            // frame capture's passes and meshes.
            static const u32 max_lines = [] {
                const char* env = std::getenv("GOW3_SRT_CHECK_LINES");
                return env ? u32(std::strtoul(env, nullptr, 10)) : 16u;
            }();
            if (described < max_lines) {
                ++described;
                std::printf("SRT check: shader %016llx table changed after the walk: %u of %zu dwords, first "
                            "dword %zu %#x -> %#x, from %#llx (%s); %s walk, %llu ms old%s\n",
                            static_cast<unsigned long long>(check.hash), differ, now.size(), first,
                            check.flat[first], now[first], static_cast<unsigned long long>(source), kind,
                            check.worker ? "worker" : "GPU thread",
                            static_cast<unsigned long long>((SrtNowNs() - check.walk_ns) / 1000000),
                            check.worker
                                ? ""
                                : fmt::format(", draw color0 {:#x} depth {:#x} ib {:#x} n {}",
                                              check.draw.color0, check.draw.depth, check.draw.index,
                                              check.draw.num_indices)
                                      .c_str());
            }
        }
    }
    if (const auto t = std::chrono::steady_clock::now(); t - last >= std::chrono::seconds{5}) {
        last = t;
        std::vector<std::pair<u64, u64>> changed;
        u64 walks = 0, total_changed = 0;
        for (const auto& [hash, s] : stats) {
            walks += s.first;
            total_changed += s.second;
            if (s.second) {
                changed.emplace_back(s.second, hash);
            }
        }
        std::ranges::sort(changed, std::greater{});
        std::string top;
        for (size_t i = 0; i < std::min<size_t>(changed.size(), 6); ++i) {
            top += fmt::format(" {:016x}x{}/{}", changed[i].second, changed[i].first,
                               stats[changed[i].second].first);
        }
        std::string kind_text, window_text;
        for (const auto& [kind, n] : kinds) {
            kind_text += fmt::format(" {} {}", kind, n);
        }
        std::vector<std::pair<u64, u64>> sorted_windows;
        for (const auto& [window, n] : windows) {
            sorted_windows.emplace_back(n, window);
        }
        std::ranges::sort(sorted_windows, std::greater{});
        for (size_t i = 0; i < std::min<size_t>(sorted_windows.size(), 5); ++i) {
            window_text += fmt::format(" {:#x}x{}", sorted_windows[i].second << 20, sorted_windows[i].first);
        }
        std::printf("SRT check (5 s): %llu sampled walks, %llu tables changed by the submission;%s; sources:%s; "
                    "windows:%s\n",
                    static_cast<unsigned long long>(walks), static_cast<unsigned long long>(total_changed),
                    top.c_str(), kind_text.c_str(), window_text.c_str());
        std::printf("SRT check (5 s): changed tables by walk age at the check: <2 ms %llu, 2-5 %llu, 5-10 %llu, "
                    "10-20 %llu, 20+ %llu\n",
                    (unsigned long long)ages[0], (unsigned long long)ages[1], (unsigned long long)ages[2],
                    (unsigned long long)ages[3], (unsigned long long)ages[4]);
        std::printf("SRT check (5 s): changed tables by walker: GPU thread %llu (the draws used them), "
                    "draw-preparation workers %llu (re-walked by the GPU thread)\n",
                    (unsigned long long)changed_gpu, (unsigned long long)changed_worker);
        changed_gpu = changed_worker = 0;
        ages = {};
        kinds.clear();
        windows.clear();
        std::fflush(stdout);
        stats.clear();
    }
}

/// gow3: compute shaders whose output the guest reads as soon as the next end-of-pipe event
/// fires (Rasterizer::eop_readbacks): Uncharted 1's Cs_RayTraceGatherHits (the closest hit of
/// each mesh ray cast).
/// GOW3_EOP_READBACK: 0 off, 1 (default) asynchronous with the pages read-protected, 2 waiting
/// for the GPU at the event (diagnostics).
int EopReadbackMode() {
    static const int mode = [] {
        const char* env = std::getenv("GOW3_EOP_READBACK");
        return env && env[0] >= '0' && env[0] <= '2' ? env[0] - '0' : 1;
    }();
    return mode;
}

/// gow3 16 Oct: Uncharted 2's 64-wide copy shader (6a6422a9, run on the GPU: its sources are
/// completed after the frame is queued) writes data the game reads back on the CPU (one value
/// in a 1 KiB block it rewrites every frame). Read back with the submission, that block counted
/// as recurring (readable, a frame old): NPCs floated in the air and Nate sank into the floor,
/// fixed by precise readbacks at ~50 FPS. As an end-of-pipe readback the fence after it waits
/// for it. GOW3_COPY64_EOP_READBACK=1.
bool Copy64EopReadback() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_COPY64_EOP_READBACK");
        return env && env[0] == '1';
    }();
    return enabled;
}

bool IsEopReadbackShader(u64 hash) {
    return (hash == 0xc92150ba || (hash == 0x6a6422a9 && Copy64EopReadback())) &&
           EopReadbackMode() != 0;
}

/// gow3: a T# that passes the format check but cannot be a real texture: Uncharted 2's treasure
/// effects fetch T#s from resource table slots that hold other data (CPU-written; the shader
/// branches using them are not taken). Built, such an image spans gigabytes or reaches past
/// mapped memory (tracked, uploaded, out of device memory) or has reserved swizzles.
/// The raw fields, before anything decodes them (unknown tile modes, reserved swizzles and
/// image types stop the decoders).
bool PlausibleTSharp(const AmdGpu::Image& tsharp) {
    if (!tsharp.Valid() || tsharp.tiling_index > 26 || tsharp.last_level < tsharp.base_level) {
        return false; // 27-30 reserved, 31 (linear general) unhandled
    }
    // gow3 24 Oct 2026 (GitHub #144, Uncharted 2, RTX 4090): data formats above BC7 (FMask and
    // packed formats 44-63) have no entry in the size tables; a garbage T# with format 63 and
    // number format 7 stopped the game in ImageInfo (pixel_format.cpp NumBitsPerBlock). No texture
    // the games sample uses them (real FMask reads become FMaskResource, never stage.images).
    // GOW3_TSHARP_FORMAT_CHECK=0 (tests): without these two checks.
    static const bool format_check = [] {
        const char* env = std::getenv("GOW3_TSHARP_FORMAT_CHECK");
        return !(env && env[0] == '0');
    }();
    if (format_check && u32(tsharp.data_format) > u32(AmdGpu::DataFormat::FormatBc7)) {
        return false;
    }
    // MSAA types keep log2(samples) in last_level: more than 8 samples stops the macro-tile size
    // code (tile.h GetMacroTileExtents, ASSERT(num_samples <= 8)).
    const auto type = static_cast<AmdGpu::ImageType>(u64(tsharp.type));
    if (format_check &&
        (type == AmdGpu::ImageType::Color2DMsaa || type == AmdGpu::ImageType::Color2DMsaaArray) &&
        tsharp.last_level > 3) {
        return false;
    }
    for (const u32 swizzle : {u32(tsharp.dst_sel_x), u32(tsharp.dst_sel_y),
                              u32(tsharp.dst_sel_z), u32(tsharp.dst_sel_w)}) {
        if (swizzle == 2 || swizzle == 3) {
            return false;
        }
    }
    return true;
}

/// gow3 20 Oct 2026 (GitHub #63): the host image a description would create, roughly (all
/// layers and slices of the first level, a third more with mips), and its layer count against
/// the device's limit. Garbage descriptions make images of gigabytes: one of 960x540 RGBA8 with
/// 6772 layers (16 GB) took all video memory and the next allocation stopped Drake's Fortune.
bool PlausibleHostImage(const VideoCore::ImageInfo& info, u32 max_layers) {
    if (info.resources.layers == 0 || info.resources.layers > max_layers) {
        return false;
    }
    // Compressed formats: num_bits is per 4x4 block.
    const u64 width = info.props.is_block ? (info.size.width + 3) / 4 : info.size.width;
    const u64 height = info.props.is_block ? (info.size.height + 3) / 4 : info.size.height;
    const u64 bytes = width * height * std::max(info.size.depth, 1u) * info.resources.layers *
                      std::max(info.num_bits / 8u, 1u);
    return bytes + (info.resources.levels > 1 ? bytes / 3 : 0) < 1_GB;
}

bool PlausibleImage(const VideoCore::ImageInfo& info, const AmdGpu::Image& tsharp,
                    Core::MemoryManager* memory, u32 max_layers) {
    if (info.guest_size == 0 || info.guest_size >= 1_GB || !PlausibleHostImage(info, max_layers)) {
        return false;
    }
    return memory->ClampRangeSize(info.guest_address, info.guest_size) == info.guest_size;
}

/// A render target or depth buffer whose registers describe an impossible image: skipped
/// (the draw goes on without that attachment), as garbage T#s are.
bool PlausibleTarget(const VideoCore::ImageInfo& info, u32 max_layers, const char* what) {
    if (info.guest_size < 1_GB && PlausibleHostImage(info, max_layers)) {
        return true;
    }
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::printf("GPU: skipping a garbage %s at %#llx: %ux%ux%u, %u layers, %u mips, %u bits\n",
                    what, static_cast<unsigned long long>(info.guest_address), info.size.width,
                    info.size.height, info.size.depth, info.resources.layers, info.resources.levels,
                    info.num_bits);
        std::fflush(stdout);
    }
    return false;
}

/// gow3 test (21 Oct 2026): GOW3_TEST_GARBAGE_TSHARP=<seconds>[,<count>]. After <seconds> from
/// the first texture bind, the next <count> T#s (default 600) become the garbage one that stopped
/// Uncharted 2 on a laptop (BC2, number format 13, a macro-tiled mode; image_info.cpp UpdateSize
/// asserted), to check that such a T# is dropped. Off by default.
/// 24 Oct 2026: a third field picks another garbage kind: fmt<N> = data format N with number
/// format 7 (fmt63 stopped Uncharted 2 in NumBitsPerBlock, GitHub #144), msaa16 = an MSAA image
/// of 16 samples (tile.h GetMacroTileExtents asserts above 8).
void TestGarbageTSharp(AmdGpu::Image& tsharp) {
    struct Params {
        double delay_s = -1.0;
        u32 count = 600;
        int format = -1;
        bool msaa16 = false;
    };
    static const Params params = [] {
        Params p;
        if (const char* env = std::getenv("GOW3_TEST_GARBAGE_TSHARP"); env && env[0]) {
            p.delay_s = std::atof(env);
            if (const char* comma = std::strchr(env, ',')) {
                p.count = static_cast<u32>(std::strtoul(comma + 1, nullptr, 10));
                if (const char* mode = std::strchr(comma + 1, ',')) {
                    if (std::strncmp(mode + 1, "fmt", 3) == 0) {
                        p.format = static_cast<int>(std::strtol(mode + 4, nullptr, 10));
                    } else if (std::strcmp(mode + 1, "msaa16") == 0) {
                        p.msaa16 = true;
                    }
                }
            }
        }
        return p;
    }();
    if (params.delay_s < 0.0 || tsharp.Address() == 0) {
        return;
    }
    static const auto start = std::chrono::steady_clock::now();
    static std::atomic<u32> injected{0};
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() <
            params.delay_s ||
        injected.load(std::memory_order_relaxed) >= params.count) {
        return;
    }
    if (params.format >= 0) {
        tsharp.data_format = u64(params.format);
        tsharp.num_format = 7;
    } else {
        if (params.msaa16) {
            tsharp.type = u64(AmdGpu::ImageType::Color2DMsaa);
            tsharp.base_level = 0;
            tsharp.last_level = 4;
            tsharp.data_format = u64(AmdGpu::DataFormat::Format8_8_8_8);
            tsharp.num_format = 0;
        } else {
            tsharp.data_format = u64(AmdGpu::DataFormat::FormatBc2);
            tsharp.num_format = 13;
        }
        for (u32 index = 0; index <= 26; ++index) {
            tsharp.tiling_index = index;
            if (AmdGpu::IsMacroTiled(AmdGpu::GetArrayMode(tsharp.GetTileMode()))) {
                break;
            }
        }
    }
    const u32 n = injected.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == params.count) {
        std::printf("GPU test: garbage T# %u of %u (data format %u, number format %u, type %u, "
                    "last level %u, tile mode %u) at %#llx\n",
                    n, params.count, u32(tsharp.data_format), u32(tsharp.num_format),
                    u32(tsharp.type), u32(tsharp.last_level), u32(tsharp.tiling_index),
                    static_cast<unsigned long long>(tsharp.Address()));
        std::fflush(stdout);
    }
}
} // namespace

namespace {
/// magic_enum::enum_contains, from a table: it scans every enumerator (per texture per draw).
bool IsKnownFormat(AmdGpu::DataFormat data_fmt, AmdGpu::NumberFormat num_fmt) {
    static const auto tables = [] {
        std::pair<std::array<bool, 256>, std::array<bool, 256>> t{};
        for (const auto value : magic_enum::enum_values<AmdGpu::DataFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.first[static_cast<u32>(value)] = true;
            }
        }
        for (const auto value : magic_enum::enum_values<AmdGpu::NumberFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.second[static_cast<u32>(value)] = true;
            }
        }
        return t;
    }();
    const u32 d = static_cast<u32>(data_fmt), n = static_cast<u32>(num_fmt);
    return d < 256 && n < 256 && tables.first[d] && tables.second[n];
}
} // namespace

bool Rasterizer::FilterDraw() {
    const auto& regs = Regs();
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

template <typename... Parts>
VideoCore::ImageId Rasterizer::FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                                  LastTarget& last, auto&& make_desc,
                                                  const Parts&... parts) {
    static_assert((sizeof(Parts) + ...) <= 256, "target memo key too large");
    std::array<u8, 256> key{};
    u32 key_size = 0;
    const auto append = [&](const auto& part) {
        std::memcpy(key.data() + key_size, &part, sizeof(part));
        key_size += sizeof(part);
    };
    (append(parts), ...);
    const u64 generation = texture_cache.RegistryGeneration();
    const bool memo_enabled = !Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo);
    if (memo_enabled && last.generation == generation && last.key_size == key_size &&
        std::memcmp(last.key.data(), key.data(), key_size) == 0) {
        texture_cache.MarkFound(last.image_id);
        return last.image_id;
    }
    const auto remember = [&](VideoCore::ImageId image_id) {
        last.key = key;
        last.key_size = key_size;
        last.generation = generation;
        last.image_id = image_id;
    };
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < key_size; i += 8) {
        u64 word;
        std::memcpy(&word, key.data() + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    auto& memo = target_memo[hash % target_memo.size()];
    if (memo.generation == generation && memo.key_size == key_size &&
        std::memcmp(memo.key.data(), key.data(), key_size) == 0 && memo_enabled) {
        desc = memo.desc;
        texture_cache.MarkFound(memo.image_id);
        remember(memo.image_id);
        return memo.image_id;
    }
    make_desc();
    if (!PlausibleTarget(desc.info, instance.GetMaxImageArrayLayers(),
                         desc.info.props.is_depth ? "depth buffer" : "render target")) {
        last.generation = ~0ULL;
        return {};
    }
    const VideoCore::ImageId image_id = texture_cache.FindImage(desc);
    if (generation == texture_cache.RegistryGeneration()) {
        memo.key = key;
        memo.key_size = key_size;
        memo.generation = generation;
        memo.image_id = image_id;
        memo.desc = desc;
        remember(image_id);
    } else {
        last.generation = ~0ULL;
    }
    return image_id;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = CbExtent(cb);
        const u32 tag = 1;
        const auto found = FindTargetMemoized(
            desc, last_targets[cb], [&] { std::construct_at(&desc, col_buf, hint); }, tag,
            col_buf, hint);
        if (!found) {
            image_id = {}; // garbage registers (FindTargetMemoized): no attachment
            continue;
        }
        image_id = bound_images.emplace_back(found);
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if (GetEffectiveDepthStencilState(regs).needs_attachment) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = DbExtent();
        auto& [image_id, desc] = db_desc;
        const u32 tag = 2;
        const auto found = FindTargetMemoized(
            desc, last_targets[AmdGpu::NUM_COLOR_BUFFERS],
            [&] {
                std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                                  htile_address, hint);
            },
            tag, regs.depth_buffer, regs.depth_view, regs.depth_control, htile_address, hint);
        image_id = found ? bound_images.emplace_back(found) : VideoCore::ImageId{};
        if (image_id) {
            auto& image = texture_cache.GetImage(image_id);
            image.binding.is_target = 1u;
        }
    } else {
        db_desc.first = {};
    }
    // gow3: the G-buffer pass (5+ color targets) holds the scene depth, and its constants the
    // main camera (shadow passes bind the same layout with the light's camera).
    gbuffer_draw = camera_motion->Enabled() && std::popcount(key.mrt_mask) >= 5 && db_desc.first;
    if (gbuffer_draw) {
        camera_motion->OnGBufferPass(db_desc.first);
    } else if (camera_motion->VelocityMode() && db_desc.first && !camera_motion->Depth()) {
        // Uncharted: no G-buffer of 5 targets. The frame's first pass at the render size that is
        // depth-only (U1's prepass) or draws RGBA16F color (U2's first scene pass) holds the scene
        // depth (after the reference port; the size is the patched render size, ultrawide too).
        const auto& depth = texture_cache.GetImage(db_desc.first);
        const auto [scene_w, scene_h] = camera_motion->SceneSize();
        bool scene_pass = !cb_descs[0].first;
        if (cb_descs[0].first) {
            const auto& color = texture_cache.GetImage(cb_descs[0].first);
            scene_pass = color.info.pixel_format == vk::Format::eR16G16B16A16Sfloat &&
                         color.info.size.width == scene_w && color.info.size.height == scene_h;
        }
        if (scene_pass && depth.info.size.width == scene_w && depth.info.size.height == scene_h &&
            (depth.info.pixel_format == vk::Format::eD32SfloatS8Uint ||
             depth.info.pixel_format == vk::Format::eD32Sfloat)) {
            camera_motion->OnGBufferPass(db_desc.first);
        }
    }
    // Uncharted 2: the first scene-size D32 draw of a frame whose vertex user data carries a
    // perspective camera gives the camera and the scene depth (light pre-pass normals).
    flat_camera_depth = {};
    if (camera_motion->FlatSource() && db_desc.first) {
        const auto& depth = texture_cache.GetImage(db_desc.first);
        const auto format = depth.info.pixel_format;
        if ((format == vk::Format::eD32Sfloat || format == vk::Format::eD32SfloatS8Uint) &&
            depth.info.size.width >= 1024) {
            display_frame_open = false;
            if (camera_motion->WantsCamera()) {
                flat_camera_depth = db_desc.first;
            }
        }
    }
    // Uncharted: scene geometry (light pre-pass, materials) draws with the jittered scene depth;
    // its textures take the upscaler's LOD bias (BindResources). Depth-only draws count too:
    // Drake's Fortune lays down the scene depth first and draws the world with an equal depth
    // test, which fails everywhere against a depth drawn at the native size and resampled.
    flat_scene_draw = camera_motion->FlatSource() && db_desc.first &&
                      (db_desc.first == flat_camera_depth || JitteredDepth(db_desc.first));
    if (upscaler->Enabled() && cb_descs[0].first) {
        upscaler->OnColorTarget(cb_descs[0].first);
    }
    // GOW3_UPSCALER_TRACE: blended draws into the scene color without the scene depth (effects
    // composited from other targets), 40 lines.
    if (static const bool trace = std::getenv("GOW3_UPSCALER_TRACE") != nullptr;
        trace && upscaler->Enabled() && cb_descs[0].first &&
        cb_descs[0].first == upscaler->SceneColor() && db_desc.first != camera_motion->Depth() &&
        regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass) {
        static int lines = 0;
        if (lines++ < 40) {
            const auto* depth = db_desc.first ? &texture_cache.GetImage(db_desc.first).info : nullptr;
            std::printf("Scene effect: vs %08llx ps %08llx indices %u instances %u depth %ux%u "
                        "test %d write %d src %d dst %d\n",
                        (unsigned long long)pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                        (unsigned long long)(key.mrt_mask ? pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash : 0),
                        regs.num_indices, regs.num_instances.NumInstances(),
                        depth ? depth->size.width : 0, depth ? depth->size.height : 0,
                        int(regs.depth_control.depth_enable), int(regs.depth_control.depth_write_enable),
                        int(regs.blend_control[0].color_src_factor),
                        int(regs.blend_control[0].color_dst_factor));
        }
    }
    // gow3: scene color: a full-size RGBA16F target drawn with the scene depth.
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        cb_descs[0].first && std::popcount(key.mrt_mask) <= 2) {
        const auto& color = texture_cache.GetImage(cb_descs[0].first);
        const auto& depth = texture_cache.GetImage(db_desc.first);
        if (color.info.pixel_format == vk::Format::eR16G16B16A16Sfloat &&
            color.info.size.width == depth.info.size.width &&
            color.info.size.height == depth.info.size.height) {
            upscaler->OnSceneColor(cb_descs[0].first);
            // Blended geometry without depth writes (not full-screen passes): transparents and
            // effects start. Blended layers writing depth (decals, wet/blood ground) are
            // surfaces and stay in the history.
            if (regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass &&
                !regs.depth_control.depth_write_enable &&
                (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
                upscaler->OnBlendedSceneDraw();
            } else if (regs.num_indices <= 6 && regs.num_instances.NumInstances() <= 1) {
                // A full-screen pass over the scene (the fog composite): the transparents are in.
                static const bool trace_composite = std::getenv("GOW3_UPSCALER_TRACE") != nullptr;
                static int composite_lines = 0;
                if (trace_composite && composite_lines < 24) {
                    ++composite_lines;
                    std::printf("Scene composite: vs %08llx ps %08llx blend %d src %d dst %d func %d "
                                "depth test %d write %d\n",
                                (unsigned long long)pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                                (unsigned long long)(key.mrt_mask ? pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash : 0),
                                int(regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass),
                                int(regs.blend_control[0].color_src_factor),
                                int(regs.blend_control[0].color_dst_factor),
                                int(regs.blend_control[0].color_func),
                                int(regs.depth_control.depth_enable), int(regs.depth_control.depth_write_enable));
                }
                upscaler->OnSceneComposite();
            }
        }
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

bool Rasterizer::JitteredDepth(VideoCore::ImageId depth) {
    if (!depth || !camera_motion->Depth()) {
        return false;
    }
    if (depth == camera_motion->Depth()) {
        return true;
    }
    // Uncharted 2 copies the scene depth (cs 6a6422a9) and draws its last scene layers
    // (characters' effects, weather) against the copy: same size and format as the scene depth.
    if (!camera_motion->FlatSource()) {
        return false;
    }
    const auto& scene = texture_cache.GetImage(camera_motion->Depth()).info;
    const auto& info = texture_cache.GetImage(depth).info;
    return info.pixel_format == scene.pixel_format && info.size.width == scene.size.width &&
           info.size.height == scene.size.height;
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = Regs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, CbExtent(0));
    if (!PlausibleTarget(desc.info, instance.GetMaxImageArrayLayers(), "fast-clear target")) {
        return; // gow3 (#63): garbage registers, see PlausibleTarget
    }
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

// gow3: two-stage draw pipeline (vk_draw_pipe.h). Packet layout, 8-byte aligned parts:
// DrawPacket, u16 block indices, u32 block words (RegDirty::BlockWords each), then per stage a
// PacketStage with its user data and flattened user data, then (verification) the full file.
// gow3 24 Oct 2026: a DispatchRun packet is a Dispatch packet of its first instance followed by
// a RunPacketHeader and the first instance's tasks, then per other instance a RunPacketInstance,
// its ring bindings, its user data and flattened user data and its tasks (each laid out as a
// Task packet; Rasterizer::EmitRunPacket).
namespace {
enum class PacketKind : u32 { Draw, Task, Dispatch, Special, Indirect, IndirectDispatch, DispatchRun };
struct RunPacketHeader {
    u32 count; ///< instances, the first included
    u32 num_tasks; ///< of the first instance
    u64 first_ring_end;
};
struct RunPacketInstance {
    u64 ring_end;
    u32 num_ring;
    u32 num_tasks;
};
struct TaskPacket {
    PacketKind kind;
    u32 size;
    Rasterizer::OrderedTask task;
};
struct DrawPacket {
    PacketKind kind;
    const Pipeline* pipeline;
    const PreparedDraw* prepared;
    u32 index_offset;
    u8 is_indexed;
    u8 regs_reset;
    u16 num_blocks;
    u32 num_stages;
    u32 verify;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent;
    AmdGpu::CbDbExtent db_extent;
    u64 ring_end; ///< constant ring position after this packet's copies
};
struct PacketStage {
    const Shader::Info* info;
    VAddr pgm_base;
    u32 ud_size;
    u32 flat_size;
    u32 num_ring;
    u32 pad;
};
constexpr u32 AlignPacket(u32 size) {
    return (size + 7) & ~7u;
}
u32 VerifyInterval() {
    static const u32 interval = [] {
        const char* env = std::getenv("GOW3_PIPE_VERIFY");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    return interval;
}
} // namespace

bool Rasterizer::DrawPipeWanted() {
    const char* env = std::getenv("GOW3_DRAW_PIPE");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Stage B spins while draws flow. Measured ahead with 16 threads (+19%) and with 4 cores /
    // 8 threads (taskset, Steam Deck-like: +18%).
    return Gow3Threads::Available() >= 8;
}

bool Rasterizer::UseDrawPipe() const {
    return draw_pipe && !host_markers_enabled && !Gow3Toggle::Disabled(Gow3Toggle::DrawPipeline);
}

bool Rasterizer::OnStageA() const {
#ifdef _WIN32
    // gow3 (Windows): libc++ compares std::thread::id through winpthreads' pthread_equal,
    // which asks the kernel (GetThreadId) for both threads: two system calls per check, many
    // per draw. The numeric thread id comes from the TEB.
    return static_cast<u32>(gettid()) == liverpool->GetGpuCommandProcessorThreadId();
#else
    return std::this_thread::get_id() == liverpool->GetGpuCommandProcessorThread();
#endif
}

namespace {
/// Stage A: cycles waited per drain site (GOW3_FRAME_STATS), keyed by function name and line.
std::unordered_map<const char*, std::unordered_map<u32, u64>> drain_sites;
} // namespace

void Rasterizer::DrainDrawPipe(u32 reason, u32 line, const char* function) {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    CutDispatchRun(CutDrain);
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    pipe_write_ranges.clear();
    pipe_writes_overflow = false;
    // gow3 19 Oct 2026: a queued command (U3: ~3,300 a second, mostly guest faults into
    // GPU-written pages) changes only GPU-written memory, which CollectRingBindings never copies.
    if (reason == DrawPipe::ReasonCommands &&
        !Gow3Toggle::Disabled2(Gow3Toggle::RingKeepOnCommands)) {
        ++ring_command_drains_kept;
    } else {
        NextRingEpoch(EpochDrain);
    }
    if (!stats) {
        draw_pipe->Drain(reason);
        pending_complete = true;
        return;
    }
    const u64 before = draw_pipe->drain_cycles;
    draw_pipe->Drain(reason);
    pending_complete = true;
    if (const u64 waited = draw_pipe->drain_cycles - before) {
        drain_sites[function][line] += waited;
    }
}

bool Rasterizer::IsGpuSideThread() const {
    return OnStageA() || DrawPipe::OnStageB() || PipelineCache::capturing_for_gpu_thread;
}

bool Rasterizer::IsGpuSideThreadId(u32 tid) const {
    // The userfaultfd thread handles the fault while the faulting thread waits: without locks
    // only if that thread owns the caches. The GPU command thread does not while the draw
    // recording thread may be running, so its faults take the locked path of guest threads.
    if (draw_pipe) {
        return tid == draw_pipe->StageBThreadId();
    }
    return tid == liverpool->GetGpuCommandProcessorThreadId();
}

namespace {
// gow3 21 Oct 2026: stale per-object constants with multithreaded rendering (Uncharted 2: Drake,
// his gun, enemies and train cars drawn flat white for single frames; Drake's Fortune: flat grey
// weapons, GitHub #23). The games write those constants with the GPU command stream itself: cs
// 2d215c3b copies them into the frame's constant area (~250-300 dispatches a frame in combat),
// and the next draws read them through their resource tables. The copy runs on the CPU
// (vk_shader_hle.cpp ExecuteScalarCopyHLE), in stream order on one thread when single-threaded;
// with the draw pipe it is a packet run later by stage B, while stage A selects the next draws'
// pipelines at once, walking (flattening) their tables and copying their constant buffers into
// the ring. Walked before stage B had made the copy, a draw kept another object's or the last
// frame's constants. Two holes, each closed by a switch below (on by default, =0 turns it off).

/// GOW3_PIPE_WRITE_END - a buffer a draw or dispatch writes is pending until stage B has run THAT
/// packet. Its position was the pipe head while the packet was being built (CollectRingBindings,
/// before Begin/Commit), i.e. its start: once stage B had run the packet before it (always so
/// when the pipe was idle), PendingWriteOverlaps let a ring copy take the bytes before the write.
bool PipeWriteEnd() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_PIPE_WRITE_END");
        return !(env && env[0] == '0');
    }();
    return on;
}

/// GOW3_SRT_PENDING_DRAIN - stage A walks a shader's resource tables only after stage B has run
/// the copy shaders queued before the draw that write into them (SyncSrtTables; only GPU-written
/// tables were synced, pending writes guarded ring copies but never walks). Only the copy
/// shaders' destinations are checked (pending_copy_writes, a few merged ranges): checking every
/// pending write per walk cost Uncharted 3's crowd scene (~1,500 writing dispatches a frame)
/// 60 -> 46 FPS on the GPU command thread. U2 at a stage B slowed to a laptop's pace
/// (GOW3_TEST_PIPE_DELAY_NS=2000): 6 white flashes in 535 frames -> 0, 25-48 walks a second
/// wait. 1 (default): wait until stage B has run the packets writing the table; 2: the same, and
/// report what the wait changed (per shader, 5 s).
int SrtPendingDrain() {
    static const int mode = [] {
        const char* env = std::getenv("GOW3_SRT_PENDING_DRAIN");
        if (!env || !env[0]) {
            return 1;
        }
        return env[0] >= '0' && env[0] <= '2' ? env[0] - '0' : 1;
    }();
    return mode;
}

/// The copy shaders stage B runs whose destinations the next draws' resource tables read: the
/// scalar copies 2d215c3b / 6a6422a9 and the copy shader fefebf9f (vk_shader_hle.cpp).
bool IsTableCopyShader(u64 hash) {
    return hash == 0x2d215c3b || hash == 0x6a6422a9 || hash == 0xfefebf9f;
}
} // namespace

void Rasterizer::NotePendingGpuWrite(VAddr address, u64 size) {
    if (!draw_pipe || !size) {
        return;
    }
    // Draws write the same buffers over and over: an entry for the range only moves on.
    const bool uncommitted = noting_packet && PipeWriteEnd();
    const u64 position = uncommitted ? PendingUncommitted : draw_pipe->Head();
    const VAddr end = address + size;
    NoteRingWrite(address, size);
    if (noting_copy) {
        // A copy shader's destination, for the resource table walks (SyncSrtTables): pending
        // until stage B has run the copy's packet (its end, set at the commit).
        copy_uncommitted |= noting_packet;
        const u64 copy_position = noting_packet ? PendingUncommitted : draw_pipe->Head();
        copy_min = std::min(copy_min, address);
        copy_max = std::max(copy_max, end);
        bool merged = false;
        for (auto& write : pending_copy_writes) {
            if (address <= write.end && write.begin <= end) {
                write.begin = std::min(write.begin, address);
                write.end = std::max(write.end, end);
                write.position = copy_position;
                merged = true;
                break;
            }
        }
        if (!merged) {
            pending_copy_writes.push_back({address, end, copy_position});
        }
    }
    for (u32 i = 0; i < pending_writes.size(); ++i) {
        auto& write = pending_writes[i];
        if (address <= write.end && write.begin <= end) {
            write.begin = std::min(write.begin, address);
            write.end = std::max(write.end, end);
            write.position = position;
            pending_min = std::min(pending_min, address);
            pending_max = std::max(pending_max, end);
            if (uncommitted) {
                uncommitted_indices.push_back(i);
            }
            return;
        }
    }
    if (uncommitted) {
        uncommitted_indices.push_back(static_cast<u32>(pending_writes.size()));
    }
    pending_writes.push_back({address, end, position});
    pending_min = std::min(pending_min, address);
    pending_max = std::max(pending_max, end);
}

void Rasterizer::PrunePendingLabels() {
    while (!pending_labels.empty() && draw_pipe->Reached(pending_labels.front().position)) {
        pending_labels.pop_front();
    }
    if (pending_labels.empty()) {
        label_min = ~VAddr{0};
        label_max = 0;
    }
}

void Rasterizer::NotePendingLabel(VAddr address, u64 size) {
    if (!draw_pipe || !size || !OnStageA()) {
        return;
    }
    PrunePendingLabels();
    // A label of a task staged in the open dispatch run: pending until the run's packet is in.
    const bool staged = dispatch_run.count != 0;
    pending_labels.push_back({address, address + size,
                              staged ? PendingUncommitted : draw_pipe->Head()});
    dispatch_run.labels += staged;
    label_min = std::min(label_min, address);
    label_max = std::max(label_max, address + size);
}

void Rasterizer::NoteRingWrite(VAddr address, u64 size) {
    if (!draw_pipe || !size || !OnStageA()) {
        return;
    }
    if (address < ring_epoch_max && ring_epoch_min < address + size) {
        if (Gow3Toggle::Disabled2(Gow3Toggle::RingPreciseWrites) ||
            RingLinesOverlap(address, size)) {
            NextRingEpoch(EpochWrite);
        } else {
            ++ring_writes_kept;
        }
    }
}

bool Rasterizer::PendingCopyWriteOverlaps(VAddr address, u64 size) {
    if (pending_copy_writes.empty() || address >= copy_max || address + size <= copy_min) {
        return false;
    }
    // Entries stage B has written are dropped (the list stays a few entries long).
    std::erase_if(pending_copy_writes, [&](const PendingWrite& write) {
        return write.position != PendingUncommitted && draw_pipe->Reached(write.position);
    });
    copy_min = ~VAddr{0};
    copy_max = 0;
    bool overlaps = false;
    const VAddr end = address + size;
    for (const auto& write : pending_copy_writes) {
        copy_min = std::min(copy_min, write.begin);
        copy_max = std::max(copy_max, write.end);
        overlaps |= address < write.end && write.begin < end;
    }
    return overlaps;
}

void Rasterizer::WaitPendingWrites(const std::vector<PendingWrite>& writes, VAddr address,
                                   u64 size) {
    u64 until = 0;
    const VAddr end = address + size;
    for (const auto& write : writes) {
        if (address < write.end && write.begin < end && write.position != PendingUncommitted) {
            until = std::max(until, write.position);
        }
    }
    for (u32 spins = 0; !draw_pipe->Reached(until); ++spins) {
        if (spins < 4096) {
            __builtin_ia32_pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void Rasterizer::NewGuestEpoch() {
    // gow3 19 Oct 2026: the guest rewrites constants between its submissions, once a label told
    // it the GPU was done with them, or while the GPU waits on a label it then writes: ring
    // copies are reused within one submission and between memory waits only.
    // Without RingPreciseWrites every in-order task (labels among them) starts an epoch anyway.
    if (draw_pipe && OnStageA() && !Gow3Toggle::Disabled2(Gow3Toggle::RingPreciseWrites)) {
        NextRingEpoch(EpochSubmission);
    }
}

bool Rasterizer::PendingWriteOverlaps(VAddr address, u64 size) {
    // Unknown cache commands can land GPU results into CPU-visible memory. Never capture
    // constants ahead of such a task; stage B binds them after the task instead.
    if (draw_pipe && !draw_pipe->Reached(pending_task_position)) {
        return true;
    }
    if (pending_writes.empty() || address >= pending_max || address + size <= pending_min) {
        return false;
    }
    // Now and then drop entries whose writes the recording thread has done: the bounds shrink.
    if ((++pending_checks & 63) == 0) {
        PrunePendingWrites();
    }
    const VAddr end = address + size;
    for (const auto& write : pending_writes) {
        if (address < write.end && write.begin < end && !draw_pipe->Reached(write.position)) {
            return true;
        }
    }
    return false;
}

u32 Rasterizer::PrunePendingWrites() {
    // Not while a packet or a dispatch run is noted: uncommitted_indices point into the vector.
    if (!uncommitted_indices.empty()) {
        return 0;
    }
    const size_t before = pending_writes.size();
    std::erase_if(pending_writes, [&](const PendingWrite& write) {
        return draw_pipe->Reached(write.position);
    });
    pending_min = ~VAddr{0};
    pending_max = 0;
    for (const auto& write : pending_writes) {
        pending_min = std::min(pending_min, write.begin);
        pending_max = std::max(pending_max, write.end);
    }
    return static_cast<u32>(before - pending_writes.size());
}

static void FixAimFocusCentre(Core::MemoryManager* memory, VideoCore::BufferCache* buffer_cache,
                              VAddr address);

namespace {
/// gow3 22 Oct 2026: stage A's guest copies into the constant ring and the copies bound again
/// instead, within their epoch or after it (Gow3Toggle::RingDurableReuse), per frame on the
/// GOW3_DISPATCH_STATS lines. Stage A only.
struct RingReuseStats {
    u64 copies = 0, copy_bytes = 0;
    u64 untracked = 0, untracked_bytes = 0; ///< copies with no CPU-clean token (not durable)
    u64 epoch = 0, epoch_bytes = 0;         ///< bound again within their epoch
    u64 durable = 0, durable_bytes = 0;     ///< bound again after their epoch ended
    u64 refused = 0;     ///< earlier copies with a token that a write may have made stale
    u64 mismatches = 0;  ///< GOW3_RING_REUSE_CHECK=1: durable reuses that differed
    u64 watched = 0;     ///< of the durable reuses, by a watch token
    u64 promoted = 0;    ///< pages stage A write-protected (RingWatchToken)
};
RingReuseStats g_ring_reuse;
/// Any thread (RingUnwatch): CPU writes that dropped a watch, pages never watched again.
std::atomic<u64> g_ring_watch_faults{0}, g_ring_watch_banned{0};
std::atomic<u32> g_ring_watched{0}; ///< pages stage A watches now
u64 s_ring_faults_seen = 0;         ///< g_ring_watch_faults at the last stats line
/// Ranges copied in this many epochs in a row are watched (static data); at most this many
/// pages (32 MiB) at once; a page whose watch CPU writes dropped this often is not watched again.
constexpr u32 RingWatchEpochs = 3;
constexpr u32 RingWatchMaxPages = 8192;
constexpr u8 RingWatchMaxFaults = 2;
constexpr u64 RingWatchMinBytes = 1024;
} // namespace

// gow3 22 Oct 2026: RingDurableReuse found the crowd mesh pages never CPU-clean (U3 meat market:
// 6,953 of 7,016 copies a frame, 24.3 of 24.4 MB): the ring path never hands them to the buffer
// cache, so nothing write-protects them. Stage A watches them itself.
u64 Rasterizer::RingWatchToken(VAddr address, u64 size, bool probe) {
    const VAddr lo = Common::AlignDown(address, VAddr{4096});
    const VAddr hi = Common::AlignUp(address + size, VAddr{4096});
    std::scoped_lock lock{ring_watch_mutex};
    u32 needed = 0;
    for (VAddr page = lo; page < hi; page += 4096) {
        const auto it = ring_watch_pages.find(page);
        if (it == ring_watch_pages.end() || !it->second.watched) {
            if (it != ring_watch_pages.end() && it->second.faults >= RingWatchMaxFaults) {
                return 0; // written by the CPU again and again: not static
            }
            ++needed;
        }
    }
    if (needed && probe) {
        return 0; // GOW3_STAGEA_VERIFY: a page would need a watch; add none
    }
    if (needed) {
        // Never pages written past page tracking (backing view), GPU-written ones, fiber stacks
        // (kept writable: a protected stack in use kills the process) or unmapped ones.
        if (g_ring_watched.load(std::memory_order_relaxed) + needed > RingWatchMaxPages ||
            !IsMapped(lo, hi - lo) || Core::MemoryManager::BackingWritten(lo, hi - lo) ||
            buffer_cache.MayBeGpuWritten(lo, hi - lo) ||
            Core::MemoryManager::KeptWritable(lo, hi - lo)) {
            return 0;
        }
        for (VAddr page = lo; page < hi; page += 4096) {
            auto& state = ring_watch_pages[page];
            if (!state.watched) {
                page_manager.UpdatePageWatchers<true>(page, 4096);
                state.watched = true;
                state.version = ++ring_watch_version;
                g_ring_watched.fetch_add(1, std::memory_order_relaxed);
                ++g_ring_reuse.promoted;
            }
        }
    }
    // Read once every page is protected: a store from here on faults and moves it on.
    return ring_watch_gen.load(std::memory_order_acquire) + 1;
}

void Rasterizer::RingUnwatch(VAddr address, u64 size, bool written) {
    if (g_ring_watched.load(std::memory_order_acquire) == 0 || size == 0) {
        return;
    }
    const VAddr lo = Common::AlignDown(address, VAddr{4096});
    const VAddr hi = Common::AlignUp(address + size, VAddr{4096});
    std::scoped_lock lock{ring_watch_mutex};
    const auto drop = [&](VAddr page, RingWatchPage& state) {
        if (state.watched) {
            // Before the protection goes: a token read after the store sees the change.
            ring_watch_gen.fetch_add(1, std::memory_order_seq_cst);
            state.watched = false;
            g_ring_watched.fetch_sub(1, std::memory_order_relaxed);
            page_manager.UpdatePageWatchers<false>(page, 4096);
            if (written) {
                g_ring_watch_faults.fetch_add(1, std::memory_order_relaxed);
                if (++state.faults == RingWatchMaxFaults) {
                    g_ring_watch_banned.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };
    if ((hi - lo) / 4096 <= ring_watch_pages.size()) {
        for (VAddr page = lo; page < hi; page += 4096) {
            if (const auto it = ring_watch_pages.find(page); it != ring_watch_pages.end()) {
                drop(page, it->second);
                if (!written) {
                    ring_watch_pages.erase(it); // unmapped: new memory may come here
                }
            }
        }
        return;
    }
    for (auto it = ring_watch_pages.begin(); it != ring_watch_pages.end();) {
        if (it->first >= lo && it->first < hi) {
            drop(it->first, it->second);
            if (!written) {
                it = ring_watch_pages.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// 23 Oct 2026: a per-range version rather than the global invalidation serial. Unrelated CPU
// writes must not invalidate every mesh. A unique monotonically increasing serial is assigned
// to each page promotion: changing any page makes the maximum newer; a missing watch refuses
// reuse. The page-locking/CPU-fault protocol itself is the same as RingWatchToken.
u64 Rasterizer::StaticMeshWatchVersion(VAddr address, u64 size) {
    std::scoped_lock lock{ring_watch_mutex};
    u64 version = 0;
    for (VAddr page = Common::AlignDown(address, VAddr{4096}); page < address + size; page += 4096) {
        const auto it = ring_watch_pages.find(page);
        if (it == ring_watch_pages.end() || !it->second.watched) {
            return 0;
        }
        version = std::max(version, it->second.version);
    }
    return version;
}

bool Rasterizer::TryStaticMeshSnapshot(
    u32 index, VAddr address, u64 size,
    boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out) {
    if (!static_mesh_buffer || LateConstantsEnabled() ||
        Gow3Toggle::Disabled(Gow3Toggle::RingReuse) ||
        Gow3Toggle::Disabled2(Gow3Toggle::StaticMeshSnapshots) ||
        Gow3Toggle::Disabled2(Gow3Toggle::RingDurableReuse)) {
        return false;
    }
    const VAddr lo = Common::AlignDown(address, VAddr{4096});
    const VAddr hi = Common::AlignUp(address + size, VAddr{4096});
    if (Core::MemoryManager::BackingWritten(address, size) ||
        buffer_cache.MayBeGpuWritten(lo, hi - lo) || PendingWriteOverlaps(address, size)) {
        return false;
    }
    const u64 key = address ^ (size << 48);
    const u64 generation = ring_watch_gen.load(std::memory_order_acquire);
    auto it = static_mesh_copies.find(key);
    if (it != static_mesh_copies.end() &&
        (it->second.address != address || it->second.size != size)) {
        // The compact hash is not a range identity. A collision can evict a lookup only;
        // the old immutable allocation remains alive for packets already using it.
        static_mesh_copies.erase(it);
        it = static_mesh_copies.end();
    }
    u64 version = 0;
    if (it != static_mesh_copies.end()) {
        auto& copy = it->second;
        if (copy.observed_gen != generation) {
            ++static_mesh_rechecks;
            version = StaticMeshWatchVersion(address, size);
            if (!version || version != copy.version) {
                static_mesh_copies.erase(it);
                it = static_mesh_copies.end();
            } else {
                copy.observed_gen = generation;
            }
        }
    }
    if (it == static_mesh_copies.end()) {
        if (!version) {
            version = StaticMeshWatchVersion(address, size);
        }
        if (!version) {
            return false; // let the existing ring's repeated-use heuristic establish watches
        }
        const u64 alignment = std::max<u64>(instance.StorageMinAlignment(), instance.UniformMinAlignment());
        const u64 offset = Common::AlignUp(static_mesh_position, alignment);
        if (offset > StaticMeshCapacity || size > StaticMeshCapacity - offset) {
            ++static_mesh_full;
            return false;
        }
        memory->CopySparseMemory(address, static_mesh_buffer->mapped_data.data() + offset, size);
        static_mesh_buffer->Flush(offset, size);
        static_mesh_position = offset + size;
        it = static_mesh_copies.emplace(key, StaticMeshCopy{address, size, offset, version, generation}).first;
        ++static_mesh_uploads;
    } else {
        ++static_mesh_hits;
    }
    const u64 offset = it->second.offset;
    // Keep sampled verification separate from RingReuseCheck (which copies every transient
    // reuse). This can accompany a short performance check without restoring all ring copies.
    static const bool check = [] {
        const char* env = std::getenv("GOW3_STATIC_MESH_CHECK");
        return env && env[0] == '1';
    }();
    if ((check || RingReuseCheck()) && ((static_mesh_hits + static_mesh_uploads) & 255) == 0) {
        thread_local std::vector<u8> bytes;
        bytes.resize(size);
        memory->CopySparseMemory(address, bytes.data(), size);
        ++static_mesh_checks;
        if (std::memcmp(bytes.data(), static_mesh_buffer->mapped_data.data() + offset, size) != 0) {
            ++static_mesh_mismatches;
            static_mesh_copies.erase(it);
            return false; // refuse stale input; original ring path takes a fresh snapshot
        }
    }
    out.push_back({index, static_cast<u32>(size), offset | StaticMeshOffsetFlag, address});
    return true;
}

void Rasterizer::CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                           boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out,
                           const AmdGpu::Buffer* sharps) {
    const bool memos = !Gow3Toggle::Disabled2(Gow3Toggle::DispatchStageAMemos);
    const PreparedStage* prepared_stage = nullptr;
    if (prepared && !Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)) {
        for (u32 i = 0; i < prepared->num_stages; ++i) {
            const auto& candidate = prepared->stages[i];
            if (&candidate.program->info == &stage &&
                candidate.num_buffers == stage.buffers.size()) {
                prepared_stage = &candidate;
                break;
            }
        }
    }
    const u64 alignment = std::max<u64>(instance.StorageMinAlignment(),
                                        instance.UniformMinAlignment());
    const bool reuse = !Gow3Toggle::Disabled(Gow3Toggle::RingReuse);
    // gow3 22 Oct 2026 (Gow3Toggle::RingDurableReuse): U3 meat market (1080p DLAA K, 82 FPS):
    // stage A copied 1.97 GB/s of guest ranges into the ring (CopySparseMemory 19-20% of the GPU
    // command thread) and bound 1.0 GB/s again without a copy, but ~34,500 guest epochs a second
    // (graphics submissions, compute rings, memory waits) ended that reuse every ~30 us. The
    // crowd skinning (cs 8f067540, ~1,530 dispatches a frame) binds each mesh's five arrays
    // (1-12 KB, ~15.9 KB with the bones) per instance; the dispatch trace showed one ring copy
    // per mesh per epoch. A copy made while page tracking had its pages CPU-clean (so a CPU
    // store there faults and marks them first) is bound again after its epoch while no page of
    // it was marked CPU-modified since and no GPU or backing-view write ever reached it; the
    // caller has checked GPU-modified and pending writes as for any copy.
    const bool durable = reuse && !Gow3Toggle::Disabled2(Gow3Toggle::RingDurableReuse) &&
                         !LateConstantsEnabled();
    const VideoCore::MemoryTracker* tracker = durable ? buffer_cache.GetMemoryTracker() : nullptr;
    // Sticky 64 KiB window bits: no backing-view write (labels, readback landings, image
    // downloads, write-throughs: page tracking does not see them) and no GPU write ever.
    const auto never_written = [&](VAddr address, u64 size) {
        const VAddr page_lo = Common::AlignDown(address, VAddr{4096});
        const VAddr page_hi = Common::AlignUp(address + size, VAddr{4096});
        return !Core::MemoryManager::BackingWritten(address, size) &&
               !buffer_cache.MayBeGpuWritten(page_lo, page_hi - page_lo);
    };
    // gow3 diagnostics (GOW3_RING_STATS=1): guest copies per 5 s, their sizes, and how many
    // repeat a range already copied since the last flush.
    static const bool ring_stats = [] {
        const char* env = std::getenv("GOW3_RING_STATS");
        return env && env[0] == '1';
    }();
    if (ring_stats) {
        static auto last = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(5)) {
            last = now;
            std::printf("Ring copies (5 s): %llu copies, %.1f MB; repeated within a flush: %llu, "
                        "%.1f MB; sizes <=256 %llu, <=1K %llu, <=4K %llu, <=16K %llu\n",
                        (unsigned long long)ring_copy_count, ring_copy_bytes / 1e6,
                        (unsigned long long)ring_repeat_count, ring_repeat_bytes / 1e6,
                        (unsigned long long)ring_size_buckets[0], (unsigned long long)ring_size_buckets[1],
                        (unsigned long long)ring_size_buckets[2], (unsigned long long)ring_size_buckets[3]);
            std::fflush(stdout);
            std::printf("Static mesh snapshots (5 s): %llu hits, %llu uploads, %llu full, %.1f MiB "
                        "resident, %llu range rechecks; verify %llu checks, %llu mismatches\n",
                        static_mesh_hits, static_mesh_uploads, static_mesh_full,
                        static_mesh_position / 1048576.0, static_mesh_rechecks,
                        static_mesh_checks, static_mesh_mismatches);
            static_mesh_hits = static_mesh_uploads = static_mesh_full = static_mesh_rechecks = 0;
            static_mesh_checks = static_mesh_mismatches = 0;
            ring_copy_count = ring_copy_bytes = ring_repeat_count = ring_repeat_bytes = 0;
            ring_size_buckets = {};
        }
    }
    const auto copy = [&](u32 index, const void* source, VAddr address, u64 size) {
        if (ring_stats && !source) {
            ++ring_copy_count;
            ring_copy_bytes += size;
            ++ring_size_buckets[size <= 256 ? 0 : size <= 1024 ? 1 : size <= 4096 ? 2 : 3];
            if (const u64 flushes = flush_count.load(std::memory_order_relaxed);
                ring_seen_flush != flushes) {
                ring_seen.clear();
                ring_seen_flush = flushes;
            }
            if (!ring_seen.insert((u64(address) << 16) ^ size).second) {
                ++ring_repeat_count;
                ring_repeat_bytes += size;
            }
        }
        RingCopy* cached = nullptr;
        if (!source && reuse) {
            // The same bytes copied earlier since the last drain: bind that copy. A drain, an
            // in-order memory write or a GPU write over a copied range starts a new epoch.
            const u64 key = address ^ (size << 48);
            cached = &ring_copies[(key * 0x9E3779B97F4A7C15ull) >> (64 - RingCopyBits)];
            if (!Gow3Toggle::Disabled2(Gow3Toggle::RingAssociative)) {
                auto& set = ring_copy_sets[(key * 0x9E3779B97F4A7C15ull) >> (64 - RingSetBits)];
                cached = &set[0];
                for (auto& entry : set) {
                    if (entry.key == key) {
                        cached = &entry;
                        break;
                    }
                    // Prefer an empty/old snapshot. Positions increase monotonically; eviction
                    // affects only reuse, never whether the ring retains an allocation.
                    if (!entry.key || (cached->key && entry.position < cached->position)) {
                        cached = &entry;
                    }
                }
            }
            if (cached->key == key && constant_ring->Reusable(cached->position)) {
                const bool later = cached->epoch != ring_epoch;
                if (later && (cached->cpu_token || cached->watch_token) && tracker) {
                    const bool by_watch =
                        cached->watch_token &&
                        ring_watch_gen.load(std::memory_order_acquire) + 1 == cached->watch_token;
                    if ((by_watch ||
                         tracker->CpuUnchangedSince(address, size, cached->cpu_token)) &&
                        never_written(address, size)) {
                        g_ring_reuse.watched += by_watch;
                        // Bound in this epoch now: a write over it ends this epoch's reuse,
                        // as for a copy made in it.
                        cached->epoch = ring_epoch;
                        ring_epoch_min = std::min(ring_epoch_min, address);
                        ring_epoch_max = std::max(ring_epoch_max, address + size);
                        MarkRingLines(address, size);
                    } else {
                        ++g_ring_reuse.refused;
                    }
                }
                if (cached->epoch == ring_epoch) {
                    constant_ring->Hold(cached->position);
                    if (RingReuseCheck()) {
                        const u64 mismatches = ring_reuse_mismatches;
                        CheckRingReuse(address, constant_ring->Data(cached->offset), size);
                        if (later) {
                            g_ring_reuse.mismatches += ring_reuse_mismatches - mismatches;
                        }
                    }
                    ring_reused_bytes += size;
                    ++(later ? g_ring_reuse.durable : g_ring_reuse.epoch);
                    (later ? g_ring_reuse.durable_bytes : g_ring_reuse.epoch_bytes) += size;
                    out.push_back({index, static_cast<u32>(size), cached->offset, address});
                    return;
                }
            }
        }
        // Taken before the copy: a CPU store into the range from here on marks its page first.
        u64 cpu_token = cached && tracker ? tracker->CpuCleanToken(address, size) : 0;
        if (cpu_token &&
            (!never_written(address, size) || Core::MemoryManager::KeptWritable(address, size))) {
            cpu_token = 0; // written past page tracking, or a fiber stack (never protected)
        }
        // The same range copied again in a later epoch: after RingWatchEpochs of them, stage A
        // write-protects its pages (static data the buffer cache never protects).
        const bool same_range = cached && cached->key == (address ^ (size << 48));
        const u32 epochs = same_range && cached->epoch != ring_epoch ? cached->epochs + 1
                           : same_range                              ? cached->epochs
                                                                     : 1;
        bool watch_failed = same_range && cached->watch_failed;
        u64 watch_token = 0;
        if (tracker && !cpu_token && !watch_failed && size >= RingWatchMinBytes &&
            epochs >= RingWatchEpochs) {
            // gow3 22 Oct 2026 (Gow3Toggle::DispatchStageAMemos; U3 meat market: RingWatchToken
            // 2.5% of the GPU command thread, a mutex and a page map lookup per page for ranges
            // copied again once the ring had recycled their copy). The same range's token while
            // ring_watch_gen has not moved: no page watch was dropped since (a drop moves it
            // first), so every page is still watched and RingWatchToken would return it again.
            if (memos && same_range && cached->watch_token &&
                ring_watch_gen.load(std::memory_order_acquire) + 1 == cached->watch_token) {
                watch_token = cached->watch_token;
                ++g_stage_a_memos.tokens_reused;
                // Verify: the full call would add watches only if a page lacked one (then it
                // is a mismatch); the probe answers without adding any. A drop on another
                // thread after the check above moves the generation: not counted.
                if (Shader::StageAVerify() &&
                    RingWatchToken(address, size, true) != watch_token &&
                    ring_watch_gen.load(std::memory_order_acquire) + 1 == watch_token) {
                    ++g_stage_a_memos.token_mismatches;
                }
            } else {
                watch_token = RingWatchToken(address, size);
                watch_failed = watch_token == 0;
                ++g_stage_a_memos.tokens_asked;
            }
        }
        const auto offset = constant_ring->Allocate(size, alignment);
        if (!offset) {
            ++ring_alloc_failures;
            return;
        }
        u8* dst = constant_ring->Data(*offset);
        if (source) {
            std::memcpy(dst, source, size);
        } else {
            memory->CopySparseMemory(address, dst, size);
            ring_guest_bytes += size;
            ++ring_guest_copies;
            ++g_ring_reuse.copies;
            g_ring_reuse.copy_bytes += size;
            if (tracker && !cpu_token && !watch_token) {
                ++g_ring_reuse.untracked;
                g_ring_reuse.untracked_bytes += size;
            }
            if (cached) {
                *cached = {address ^ (size << 48), ring_epoch, constant_ring->LastAllocation(),
                           *offset, cpu_token, watch_token, epochs, watch_failed};
                ring_epoch_min = std::min(ring_epoch_min, address);
                ring_epoch_max = std::max(ring_epoch_max, address + size);
                MarkRingLines(address, size);
            }
        }
        constant_ring->Flush(*offset, size);
        if (LateConstantsEnabled()) {
            // gow3 21 Oct 2026: checked at the submission of this draw (its ring position).
            const u64 due = constant_ring->Position();
            if (source) {
                NoteLateFlatCopy(constant_ring->Storage(), *offset, stage, static_cast<u32>(size), due);
            } else {
                NoteLateGuestCopy(constant_ring->Storage(), *offset, address, static_cast<u32>(size),
                                  due);
            }
        }
        out.push_back({index, static_cast<u32>(size), *offset, address});
    };
    for (u32 index = 0; index < stage.buffers.size(); ++index) {
        const auto& desc = stage.buffers[index];
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::Flatbuf &&
                !stage.flattened_ud_buf.empty()) {
                copy(index, stage.flattened_ud_buf.data(), 0,
                     stage.flattened_ud_buf.size() * sizeof(u32));
            }
            continue;
        }
        const auto vsharp = prepared_stage ? prepared_stage->buffer_sharps[index]
                            : sharps       ? sharps[index]
                                           : desc.GetSharp(stage);
        if (!prepared_stage && sharps && Shader::StageAVerify() && !(vsharp == desc.GetSharp(stage))) {
            ++g_stage_a_memos.sharp_mismatches; // all 16 bytes (Buffer::operator== is a memcmp)
        }
        const VAddr address = vsharp.base_address;
        if (address == 0 || vsharp.GetSize() == 0) {
            continue;
        }
        // Resident immutable inputs already have page-watch/mapping lifetime validation. On a
        // hit do not repeat ClampRangeSize and the buffer tracker's locked dirty-page lookup
        // for every tiny skinning dispatch. Pending/GPU/backing writes are checked by the pool.
        const u64 requested = vsharp.GetSize();
        if (stage.pgm_hash == 0x8f067540 && index < 5 && !desc.is_written &&
            requested >= RingWatchMinBytes && requested <= VideoCore::BufferCache::STREAM_THRESHOLD &&
            TryStaticMeshSnapshot(index, address, requested, out)) {
            continue;
        }
        const u64 size = memory->ClampRangeSize(address, vsharp.GetSize());
        if (desc.is_written) {
            NotePendingGpuWrite(address, size);
            continue;
        }
        // The stream path of BufferCache::ObtainBuffer, taken here: small, read-only, not
        // written by the GPU (now or by work still queued for the recording thread).
        if (size == 0 || size > VideoCore::BufferCache::STREAM_THRESHOLD ||
            buffer_cache.IsRegionGpuModified(address, size) || PendingWriteOverlaps(address, size)) {
            continue;
        }
        if (stage.pgm_hash == 0x9f801363 && size >= 32) {
            // As BindBuffers does for the direct path: the ring copy must see the fixed centre.
            // Stage A records no GPU commands: the guest copy only, which the ring copy takes
            // (the direct path writes it through to the GPU copy later).
            FixAimFocusCentre(memory, nullptr, address);
        }
        copy(index, nullptr, address, size);
    }
}

void Rasterizer::CheckRingReuse(VAddr address, const u8* copy, u64 size) {
    thread_local std::vector<u8> guest;
    guest.resize(size);
    memory->CopySparseMemory(address, guest.data(), size);
    ++ring_reuse_checks;
    if (std::memcmp(guest.data(), copy, size) != 0) {
        ++ring_reuse_mismatches;
    }
}

namespace {
// 21 Oct 2026: GOW3_DISPATCH_STATS=1, which compute shaders fill the pipe (roadmap phase 3).
// Stage A (GPU command thread) times the dispatch handling up to the pipe commit and counts
// dims and runs per shader; stage B (draw recording thread) adds its record time under a mutex
// (only taken with the env set). Stage A prints every 5 s.
bool DispatchStatsEnabled() {
    static const bool on = std::getenv("GOW3_DISPATCH_STATS") != nullptr;
    return on;
}

struct DispatchStatsA {
    u64 n = 0, ns = 0, small = 0, consecutive = 0, run = 0, max_run = 0;
    std::map<std::array<u32, 3>, u32> dims; // {0,0,0}: indirect
};

struct DispatchStatsState {
    std::unordered_map<u64, DispatchStatsA> a;
    u64 last_hash = 0, draws = 0, dispatches = 0, a_ns = 0;
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 last_flips = AmdGpu::g_gow3_flips.load(std::memory_order_relaxed);
    std::mutex b_mutex;
    std::unordered_map<u64, std::pair<u64, u64>> b; // hash -> {dispatches, ns}
    u64 b_dispatch_ns = 0, b_draw_ns = 0;
    std::array<u64, 13> fast_paths{}; // VideoCore::g_bind_fast_paths at the last print
};

DispatchStatsState& DispatchStats() {
    static DispatchStatsState state;
    return state;
}

void DispatchStatsDraw() {
    auto& s = DispatchStats();
    ++s.draws;
    s.last_hash = 0;
}

void DispatchStatsRecordB(bool dispatch, u64 hash, u64 ns) {
    auto& s = DispatchStats();
    std::lock_guard lock{s.b_mutex};
    if (dispatch) {
        auto& entry = s.b[hash];
        ++entry.first;
        entry.second += ns;
        s.b_dispatch_ns += ns;
    } else {
        s.b_draw_ns += ns;
    }
}

void DispatchStatsPrint(std::chrono::steady_clock::time_point now) {
    auto& s = DispatchStats();
    const u64 flips = AmdGpu::g_gow3_flips.load(std::memory_order_relaxed);
    const double frames = std::max<u64>(flips - s.last_flips, 1);
    std::unordered_map<u64, std::pair<u64, u64>> b;
    u64 b_dispatch_ns, b_draw_ns;
    {
        std::lock_guard lock{s.b_mutex};
        b.swap(s.b);
        b_dispatch_ns = std::exchange(s.b_dispatch_ns, 0);
        b_draw_ns = std::exchange(s.b_draw_ns, 0);
    }
    const double items = double(s.draws + s.dispatches);
    std::printf("Dispatch stats: %.0f frames, %.0f draws/frame, %.0f dispatches/frame (%.1f%% of "
                "pipe items), stage A dispatch %.2f ms/frame, stage B dispatch %.2f ms/frame vs "
                "draw %.2f ms/frame\n",
                frames, s.draws / frames, s.dispatches / frames,
                items > 0 ? 100.0 * s.dispatches / items : 0.0, s.a_ns / 1e6 / frames,
                b_dispatch_ns / 1e6 / frames, b_draw_ns / 1e6 / frames);
    {
        // 22 Oct 2026: the binding fast paths (Gow3Toggle::DispatchBindFastPaths), draws included.
        const auto& f = VideoCore::g_bind_fast_paths;
        const std::array<const std::atomic<u64>*, 13> counters = {
            &f.ranges_extended,    &f.ranges_started,  &f.ranges_flushed,
            &f.page_ticks_skipped, &f.page_ticks_written, &f.resident_hits,
            &f.resident_checks,    &f.meta_clear_skips, &f.meta_clear_checks,
            &f.page_tick_cache_hits, &f.page_tick_write_backs, &f.page_tick_read_hits,
            &f.page_tick_flushes};
        std::array<double, 13> per_frame{};
        for (size_t i = 0; i < counters.size(); ++i) {
            const u64 value = counters[i]->load(std::memory_order_relaxed);
            per_frame[i] = (value - s.fast_paths[i]) / frames;
            s.fast_paths[i] = value;
        }
        // Flushes (SweepTracking, every 2 s) are counted over the whole line's period.
        std::printf("  fast paths per frame: GPU-written ranges %.0f joined the pending one, %.0f "
                    "started one, %.0f set inserts; page ticks %.0f skipped, %.0f written; "
                    "residency %.0f memo hits, %.0f walks; metadata-clear check %.0f skipped, %.0f "
                    "in full; page tick cache %.0f write hits, %.0f write-backs, %.0f reader hits, "
                    "%.0f flushes since the last line",
                    per_frame[0], per_frame[1], per_frame[2], per_frame[3], per_frame[4],
                    per_frame[5], per_frame[6], per_frame[7], per_frame[8], per_frame[9],
                    per_frame[10], per_frame[11], per_frame[12] * frames);
        if (VideoCore::page_tick_verify) {
            std::printf("; %llu page tick reads differed from the map",
                        static_cast<unsigned long long>(
                            f.page_tick_mismatches.load(std::memory_order_relaxed)));
        }
        std::printf("\n");
    }
    {
        // 22 Oct 2026: stage A's constant ring copies and reuses (Gow3Toggle::RingDurableReuse).
        auto& r = g_ring_reuse;
        std::printf("  constant ring per frame: %.0f guest copies (%.0f KB; %.0f of them, %.0f KB, "
                    "with pages not CPU-clean); reused %.0f in their epoch (%.0f KB saved), %.0f "
                    "after it (%.0f KB saved); %.0f earlier copies refused after their epoch",
                    r.copies / frames, r.copy_bytes / 1e3 / frames, r.untracked / frames,
                    r.untracked_bytes / 1e3 / frames, r.epoch / frames,
                    r.epoch_bytes / 1e3 / frames, r.durable / frames,
                    r.durable_bytes / 1e3 / frames, r.refused / frames);
        if (RingReuseCheck()) {
            std::printf("; %llu reuses after their epoch differed from guest memory",
                        static_cast<unsigned long long>(r.mismatches));
        }
        std::printf("\n  constant ring watches: %.0f of the reuses after their epoch by a watch, "
                    "%.0f pages protected per frame, %llu pages watched now; CPU writes into "
                    "watched pages %llu (%.1f per frame), pages never watched again %llu\n",
                    r.watched / frames, r.promoted / frames,
                    static_cast<unsigned long long>(g_ring_watched.load()),
                    static_cast<unsigned long long>(g_ring_watch_faults.load()),
                    (g_ring_watch_faults.load() - s_ring_faults_seen) / frames,
                    static_cast<unsigned long long>(g_ring_watch_banned.load()));
        s_ring_faults_seen = g_ring_watch_faults.load();
        r = {};
    }
    {
        // 22 Oct 2026: Gow3Toggle::DispatchStageAMemos.
        auto& m = g_stage_a_memos;
        std::printf("  stage A memos per frame: dispatch V#s %.0f reused, %.0f read again; effect "
                    "filter %.0f specializations skipped; ring watch tokens %.0f reused, %.0f "
                    "asked; SRT label scans %.0f skipped, %.0f done",
                    m.sharps_reused / frames, m.sharps_read / frames,
                    m.filter_skips.exchange(0) / frames, m.tokens_reused / frames,
                    m.tokens_asked / frames, m.label_skips / frames, m.label_scans / frames);
        if (Shader::StageAVerify()) {
            std::printf("; verify mismatches (totals): V#s %llu, effect filter %llu, watch tokens "
                        "%llu, label scans %llu",
                        static_cast<unsigned long long>(m.sharp_mismatches),
                        static_cast<unsigned long long>(
                            Shader::g_filter_skip_mismatches.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(m.token_mismatches),
                        static_cast<unsigned long long>(m.label_mismatches));
        }
        std::printf("\n");
        m.sharps_reused = m.sharps_read = m.tokens_reused = m.tokens_asked = m.label_skips =
            m.label_scans = 0;
    }
    std::vector<std::pair<u64, u64>> order; // {A + B ns, hash}
    for (const auto& [hash, entry] : s.a) {
        order.emplace_back(entry.ns + b[hash].second, hash);
    }
    std::ranges::sort(order, std::greater<>{});
    for (size_t i = 0; i < std::min<size_t>(order.size(), 12); ++i) {
        const u64 hash = order[i].second;
        const auto& e = s.a[hash];
        const auto common = std::ranges::max_element(
            e.dims, [](const auto& l, const auto& r) { return l.second < r.second; });
        const auto& d = common->first;
        const auto& bb = b[hash];
        std::printf("  cs %016llx: %.1f/frame, dims %s, 1x1x1 %.0f%%, A %.1f us/each, B %.1f "
                    "us/each, consecutive %.0f%%, max run %llu\n",
                    static_cast<unsigned long long>(hash), e.n / frames,
                    (d[0] ? fmt::format("{}x{}x{}", d[0], d[1], d[2]) : std::string("indirect"))
                        .c_str(),
                    100.0 * e.small / e.n, e.ns / 1e3 / e.n, bb.first ? bb.second / 1e3 / bb.first : 0.0,
                    100.0 * e.consecutive / e.n, static_cast<unsigned long long>(e.max_run));
    }
    std::fflush(stdout);
    s.a.clear();
    s.draws = s.dispatches = s.a_ns = 0;
    s.last_hash = 0;
    s.window = now;
    s.last_flips = flips;
}

struct DispatchStatsScope {
    const bool on = DispatchStatsEnabled();
    std::chrono::steady_clock::time_point t0;
    u64 hash = 0;
    std::array<u32, 3> dims{};
    DispatchStatsScope() {
        if (on) {
            t0 = std::chrono::steady_clock::now();
        }
    }
    ~DispatchStatsScope() {
        if (!on || !hash) {
            return;
        }
        auto& s = DispatchStats();
        const auto now = std::chrono::steady_clock::now();
        const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count();
        auto& e = s.a[hash];
        ++e.n;
        e.ns += ns;
        e.small += dims == std::array<u32, 3>{1, 1, 1};
        ++e.dims[dims];
        e.run = (s.last_hash == hash) ? e.run + 1 : 1;
        e.consecutive += s.last_hash == hash;
        e.max_run = std::max(e.max_run, e.run);
        s.last_hash = hash;
        ++s.dispatches;
        s.a_ns += ns;
        if (now - s.window >= std::chrono::seconds(5)) {
            DispatchStatsPrint(now);
        }
    }
};

// 21 Oct 2026: GOW3_DISPATCH_TRACE=<hash>, can a run of single-group dispatches be merged
// (roadmap phase 3). Logs once the bindings of up to 64 consecutive dispatches of that compute
// shader, the first after 600 flips (past loading). BindBuffers notes each guest binding while a
// capture is on, DispatchRecord adds the dispatch (dims, user data, barrier) once bound. Ranges
// overlap by the exact tracked extent (as the barrier tracker sees them). Runs on stage B (or the
// GPU command thread when not pipelined; never both at once).
u64 DispatchTraceTarget() {
    static const u64 hash = [] {
        const char* env = std::getenv("GOW3_DISPATCH_TRACE");
        return env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    return hash;
}

struct DispatchTraceBinding {
    u32 index;
    u64 base, size, range;
    bool written;
    const char* path; // where BindBuffers put it
    const void* host;
    u64 host_size, host_offset;
};

struct DispatchTraceRange {
    u64 lo, hi;
    u32 dispatch, index;
    bool written;
};

struct DispatchTraceState {
    int phase = 0; // 0 waiting, 1 capturing, 2 done
    u32 n = 0, barriers = 0, ww = 0, raw = 0, war = 0, reads = 0, reads_not_static = 0;
    u64 written_bytes = 0;
    std::vector<DispatchTraceBinding> pending;
    std::vector<DispatchTraceRange> ranges;
    std::vector<u32> prev_ud;
    u32 ud_changed = 0;
    std::set<std::pair<u64, u64>> distinct_reads;
};

DispatchTraceState& DispatchTrace() {
    static DispatchTraceState state;
    return state;
}

bool DispatchTraceCapturing() {
    return DispatchTraceTarget() && DispatchTrace().phase == 1;
}

void DispatchTraceFinish(const char* why, u64 hash) {
    auto& s = DispatchTrace();
    std::string ud;
    for (u32 i = 0; i < 32; ++i) {
        if (s.ud_changed >> i & 1) {
            ud += fmt::format(" {}", i);
        }
    }
    std::printf("Dispatch trace: run ended (%s %llx) after %u dispatches\n", why,
                static_cast<unsigned long long>(hash), s.n);
    std::printf("Dispatch trace: summary: %u dispatches, %zu distinct guest buffers read, reads "
                "outside the static arena %u of %u, written %llu bytes, write-write overlaps %u, "
                "read-after-write overlaps %u, write-after-read overlaps %u, barriers after the "
                "first dispatch %u, user-data dwords that change:%s\n",
                s.n, s.distinct_reads.size(), s.reads_not_static, s.reads,
                static_cast<unsigned long long>(s.written_bytes), s.ww, s.raw, s.war,
                s.barriers, ud.empty() ? " none" : ud.c_str());
    std::fflush(stdout);
    s.phase = 2;
}

// Every dispatch on entry: starts the capture, or ends it when another shader's dispatch comes.
void DispatchTraceEnter(u64 hash) {
    auto& s = DispatchTrace();
    if (s.phase == 1 && hash != DispatchTraceTarget()) {
        DispatchTraceFinish("broken by dispatch", hash);
    } else if (s.phase == 0 && hash == DispatchTraceTarget() &&
               AmdGpu::g_gow3_flips.load(std::memory_order_relaxed) >= 600) {
        s.phase = 1;
    }
    s.pending.clear();
}

void DispatchTraceDraw() {
    if (DispatchTraceCapturing()) {
        DispatchTraceFinish("broken by a draw, hash", 0);
    }
}

// BindBuffers, a guest buffer of the compute stage: where the cache put it.
// Every non-special path: ring copy, null (unbound or garbage V#), cache buffer.
void DispatchTraceBind(u32 index, const Shader::BufferResource& desc, const AmdGpu::Buffer& sharp,
                       u64 base, u64 size, const char* path, const void* host, u64 host_size,
                       u64 offset) {
    u64 range = size;
    const u32 extent = desc.access_extent;
    if (extent != 0 && extent != ~0u && sharp.stride != 0 && extent < sharp.stride &&
        sharp.num_records != 0 && !sharp.swizzle_enable && !sharp.add_tid_enable) {
        range = std::min<u64>(size, u64(sharp.num_records - 1) * sharp.stride + extent);
    }
    DispatchTrace().pending.push_back(
        {index, base, size, range, desc.is_written, path, host, host_size, offset});
}

void DispatchTraceDispatch(const Shader::Info& cs, const AmdGpu::ComputeProgram& regs,
                           bool barrier) {
    auto& s = DispatchTrace();
    if (s.phase != 1 || cs.pgm_hash != DispatchTraceTarget()) {
        return;
    }
    const auto ud = cs.UserData();
    if (s.n == 0) {
        std::printf("Dispatch trace: shader %016llx: workgroup %ux%ux%u (partial %u,%u,%u), %zu "
                    "buffers, %zu images, %zu samplers, uses_dma %d, shared memory %u bytes "
                    "(regs %u)\n",
                    static_cast<unsigned long long>(cs.pgm_hash), regs.num_thread_x.full,
                    regs.num_thread_y.full, regs.num_thread_z.full, regs.num_thread_x.partial,
                    regs.num_thread_y.partial, regs.num_thread_z.partial, cs.buffers.size(),
                    cs.images.size(), cs.samplers.size(), int(cs.uses_dma),
                    cs.shared_memory_scratch_size, regs.SharedMemSize());
        static constexpr const char* types[] = {"buffer", "flatbuf", "bda-pagetable", "fault",
                                                "gds", "shared", "clip-planes"};
        for (u32 i = 0; i < cs.buffers.size(); ++i) {
            const auto& desc = cs.buffers[i];
            const auto sharp = desc.IsSpecial() ? AmdGpu::Buffer::Null() : desc.GetSharp(cs);
            std::printf("Dispatch trace:   res %u: %s, %s (read-only unless written), %s sharp "
                        "(load mask %#x), stride %u, extent %#x, formatted %d\n",
                        i, types[std::min<u32>(u32(desc.buffer_type), 6)],
                        desc.is_written ? "written" : "read",
                        desc.sharp_fetch.load_mask ? "user-data" : "constant",
                        desc.sharp_fetch.load_mask, u32(sharp.stride), desc.access_extent,
                        int(desc.is_formatted));
        }
        std::string text;
        for (const u32 v : ud) {
            text += fmt::format(" {:#x}", v);
        }
        std::printf("Dispatch trace: first user data (%zu dwords):%s\n", ud.size(), text.c_str());
    }
    const u32 n = ++s.n;
    if (n <= 3) {
        // The raw 16-byte V# of res 0-8 as the shader's sharp fetch reads them (before filters).
        for (u32 i = 0; i < std::min<size_t>(cs.buffers.size(), 9); ++i) {
            const auto& desc = cs.buffers[i];
            AmdGpu::Buffer raw{};
            u32 dw[4]{};
            if (!desc.IsSpecial() && desc.sharp_fetch.Fetch(cs.FlatUserData().data(), &raw)) {
                std::memcpy(dw, &raw, sizeof(dw));
            }
            std::printf("Dispatch trace: #%u V# res %u: %08x %08x %08x %08x%s\n", n, i, dw[0],
                        dw[1], dw[2], dw[3], desc.IsSpecial() ? " (special)" : "");
        }
    }
    std::string line = fmt::format("Dispatch trace: #{} dims {}x{}x{} base {},{},{} barrier {}", n,
                                   regs.dim_x, regs.dim_y, regs.dim_z, regs.start_x, regs.start_y,
                                   regs.start_z, int(barrier));
    if (n > 1 && barrier) {
        ++s.barriers;
    }
    line += " ud[";
    u32 shown = 0;
    for (u32 i = 0; i < std::min<size_t>(ud.size(), 16); ++i) {
        if (n > 1 && (i >= s.prev_ud.size() || s.prev_ud[i] != ud[i])) {
            s.ud_changed |= 1u << i;
            if (shown++ < 16) {
                line += fmt::format(" {}:{:#x}", i, ud[i]);
            }
        }
    }
    line += " ]";
    s.prev_ud.assign(ud.begin(), ud.end());
    std::string overlaps;
    std::vector<DispatchTraceRange> mine;
    for (const auto& b : s.pending) {
        line += fmt::format(" | b{} {:#x}+{} {}{} host {}+{}/{}", b.index, b.base, b.size,
                            b.written ? "W " : "R ",
                            b.path,
                            b.host, b.host_offset, b.host_size);
        if (b.range != b.size) {
            line += fmt::format(" (tracked {})", b.range);
        }
        const u64 lo = b.base, hi = b.base + b.range;
        if (b.written) {
            s.written_bytes += b.range;
        } else {
            ++s.reads;
            s.reads_not_static += std::strcmp(b.path, "static-arena") != 0;
            s.distinct_reads.emplace(b.base, b.size);
        }
        for (const auto& r : s.ranges) {
            if (r.lo >= hi || lo >= r.hi) {
                continue;
            }
            if (b.written && r.written) {
                ++s.ww;
            } else if (!b.written && r.written) {
                ++s.raw;
            } else if (b.written && !r.written) {
                ++s.war;
            } else {
                continue;
            }
            overlaps += fmt::format(" b{} {} range {} by dispatch {} (b{});", b.index,
                                    b.written ? "writes" : "reads", r.written ? "written" : "read",
                                    r.dispatch, r.index);
        }
        mine.push_back({lo, hi, n, b.index, b.written});
    }
    s.ranges.insert(s.ranges.end(), mine.begin(), mine.end());
    s.pending.clear();
    line += overlaps.empty() ? " | no overlap" : " | overlap:" + overlaps;
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (n >= 64) {
        DispatchTraceFinish("64 dispatches, last", cs.pgm_hash);
    }
}
} // namespace

void Rasterizer::PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared,
                          bool is_indexed, u32 index_offset, const AmdGpu::ComputeProgram* cs,
                          const IndirectDraw* indirect, const AmdGpu::Buffer* cs_sharps) {
    CutDispatchRun(CutPacket);
    auto& dirty = liverpool->pipe_dirty;
    if (!pipe_synced) {
        // Stage B has not run yet: its register copy starts as this one.
        pipe_regs = liverpool->regs;
        dirty.Clear();
        pipe_synced = true;
    }
    std::array<u16, AmdGpu::RegDirty::NumBlocks> blocks;
    u32 num_blocks = 0;
    dirty.ForEach([&](u32 block) { blocks[num_blocks++] = static_cast<u16>(block); });
    const auto stages =
        pipeline ? pipeline->GetStages() : std::span<const Shader::Info* const>{};
    // Constants: copied here into the ring, the recording thread only binds them.
    thread_local std::array<boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>,
                            Shader::MaxStageTypes>
        rings;
    u32 num_stages = 0;
    u32 size = sizeof(DrawPacket) + AlignPacket(num_blocks * sizeof(u16)) +
               num_blocks * AmdGpu::RegDirty::BlockWords * sizeof(u32);
    for (const auto* stage : stages) {
        if (stage) {
            auto& ring = rings[num_stages];
            ring.clear();
            if (constant_ring && !Gow3Toggle::Disabled(Gow3Toggle::ConstantRing)) {
                noting_packet = true;
                noting_copy = SrtPendingDrain() != 0 && IsTableCopyShader(stage->pgm_hash);
                CollectRingBindings(*stage, used_prepared, ring, cs ? cs_sharps : nullptr);
                noting_packet = noting_copy = false;
            } else {
                pending_complete = false;
            }
            ++num_stages;
            size += sizeof(PacketStage) +
                    AlignPacket(u32(stage->user_data.size() + stage->flattened_ud_buf.size()) *
                                sizeof(u32)) +
                    u32(ring.size() * sizeof(RingBinding));
        }
    }
    if (constant_ring) {
        constant_ring->EndPacket();
    }
    if (cs) {
        size += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        size += AlignPacket(sizeof(IndirectDraw));
    }
    const u32 interval = VerifyInterval();
    const bool verify = interval && (draw_pipe->packets % interval) == 0;
    if (verify) {
        size += sizeof(AmdGpu::Regs);
    }

    u8* out = draw_pipe->Begin(size);
    u8* const start = out;
    auto& packet = *reinterpret_cast<DrawPacket*>(out);
    packet.kind = cs         ? (indirect ? PacketKind::IndirectDispatch : PacketKind::Dispatch)
                  : indirect ? PacketKind::Indirect
                  : pipeline ? PacketKind::Draw
                             : PacketKind::Special;
    packet.pipeline = pipeline;
    packet.prepared = used_prepared;
    packet.index_offset = index_offset;
    packet.is_indexed = is_indexed;
    packet.regs_reset = dirty.reset;
    packet.num_blocks = static_cast<u16>(num_blocks);
    packet.num_stages = num_stages;
    packet.verify = verify;
    packet.cb_extent = liverpool->last_cb_extent;
    packet.db_extent = liverpool->last_db_extent;
    packet.ring_end = constant_ring ? constant_ring->Position() : 0;
    out += sizeof(DrawPacket);
    std::memcpy(out, blocks.data(), num_blocks * sizeof(u16));
    out += AlignPacket(num_blocks * sizeof(u16));
    for (u32 i = 0; i < num_blocks; ++i) {
        std::memcpy(out,
                    liverpool->regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        out += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    u32 stage_index = 0;
    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto& ring = rings[stage_index++];
        auto& header = *reinterpret_cast<PacketStage*>(out);
        header = {stage,
                  stage->pgm_base,
                  static_cast<u32>(stage->user_data.size()),
                  static_cast<u32>(stage->flattened_ud_buf.size()),
                  static_cast<u32>(ring.size()),
                  0};
        out += sizeof(PacketStage);
        std::memcpy(out, ring.data(), ring.size() * sizeof(RingBinding));
        out += ring.size() * sizeof(RingBinding);
        std::memcpy(out, stage->user_data.data(), stage->user_data.size_bytes());
        std::memcpy(out + stage->user_data.size_bytes(), stage->flattened_ud_buf.data(),
                    stage->flattened_ud_buf.size() * sizeof(u32));
        out += AlignPacket(u32(stage->user_data.size() + stage->flattened_ud_buf.size()) *
                           sizeof(u32));
    }
    if (cs) {
        std::memcpy(out, cs, sizeof(AmdGpu::ComputeProgram));
        out += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        std::memcpy(out, indirect, sizeof(IndirectDraw));
        out += AlignPacket(sizeof(IndirectDraw));
    }
    if (verify) {
        std::memcpy(out, &liverpool->regs, sizeof(AmdGpu::Regs));
        out += sizeof(AmdGpu::Regs);
    }
    dirty.Clear();
    draw_pipe->Commit(static_cast<u32>(out - start), static_cast<u32>(packet.kind));
    if (!uncommitted_indices.empty() || copy_uncommitted) {
        // GOW3_PIPE_WRITE_END: this packet's writes are done once stage B is past its end.
        const u64 end = draw_pipe->Head();
        for (const u32 i : uncommitted_indices) {
            if (pending_writes[i].position == PendingUncommitted) {
                pending_writes[i].position = end;
            }
        }
        uncommitted_indices.clear();
        if (copy_uncommitted) {
            for (auto& write : pending_copy_writes) {
                if (write.position == PendingUncommitted) {
                    write.position = end;
                }
            }
            copy_uncommitted = false;
        }
    }
    PrintPipeStats();
}

void Rasterizer::RetireSubmission() {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    CutDispatchRun(CutSubmission);
    while (!pipe_keepalive.empty() && draw_pipe->Reached(pipe_keepalive.front().first)) {
        pipe_keepalive.pop_front();
    }
    if (!draw_pipe->Idle()) {
        pipe_keepalive.emplace_back(draw_pipe->Head(), draw_prep->KeepCurrent());
    }
}

bool Rasterizer::RunInOrder(OrderedTask task, const void* data, u32 size, u64 toggle,
                            bool writes_noted, bool after_run) {
    // gow3 18 Oct 2026: the ring has one producer, the GPU command thread. On the draw recording
    // thread (an EOP flip: SignalFlip -> SubmitFlipInternal queues the flip from there) every
    // earlier packet has run already, so the task runs here, in stream order; pushed from both
    // threads, two packets could take the same ring slot.
    // (Handing tasks from other threads to the command thread instead was tried: the command
    // thread may be waiting for the very flip it would queue. Not done.)
    if (!UseDrawPipe() || Gow3Toggle::Disabled(toggle) || DrawPipe::OnStageB()) {
        DrainDrawPipe(DrawPipe::ReasonRasterizer);
        task(*this, static_cast<const u8*>(data));
        return false;
    }
    // gow3 24 Oct 2026: runv2 (U3 Cartagena) cut every run at the compute RELEASE_MEM after each
    // crowd dispatch (~300k cuts per 5 s, 0 run packets). Such a fence joins the run and is
    // written once the dispatches before it are recorded: for the ones in a batch, after the
    // whole batch (later than before, never earlier). Nothing the run's dispatches read.
    if (after_run && writes_noted && dispatch_run.count && size <= RunTask{}.data.size()) {
        auto& last = dispatch_run.instances[dispatch_run.count - 1];
        if (last.num_tasks < last.tasks.size()) {
            auto& staged = last.tasks[last.num_tasks++];
            staged.task = task;
            staged.size = size;
            std::memcpy(staged.data.data(), data, size);
            if (Gow3Toggle::Disabled2(Gow3Toggle::RingPreciseWrites)) {
                NextRingEpoch(EpochTask);
            }
            return true;
        }
    }
    // Statistics: 0 not a fence that may follow a run, 1 its instance's slots full, 2 too large.
    CutDispatchRun(CutTask, !(after_run && writes_noted)          ? 0
                            : size > RunTask{}.data.size()       ? 2
                                                                 : 1);
    // Memory it writes may be bound later: constant ring copies made before are not reused.
    // gow3 19 Oct 2026: unless the caller passes what it writes to NoteRingWrite (labels,
    // WRITE_DATA, DMA): U3 runs ~1,000 such tasks a frame, and each ended the ring's reuse.
    if (!writes_noted || Gow3Toggle::Disabled2(Gow3Toggle::RingPreciseWrites)) {
        NextRingEpoch(EpochTask);
    }
    const u32 total = sizeof(TaskPacket) + AlignPacket(size);
    u8* out = draw_pipe->Begin(total);
    *reinterpret_cast<TaskPacket*>(out) = {PacketKind::Task, size, task};
    std::memcpy(out + sizeof(TaskPacket), data, size);
    draw_pipe->Commit(total, static_cast<u32>(PacketKind::Task));
    if (!writes_noted) {
        pending_task_position = draw_pipe->Head();
    }
    return true;
}

bool Rasterizer::CommandsInOrder() const {
    static const bool experimental = [] {
        const char* env = std::getenv("GOW3_COMMANDS_STAGE_B");
        return env && env[0] == '1';
    }();
    return experimental && UseDrawPipe() && OnStageA() &&
           !Gow3Toggle::Disabled2(Gow3Toggle::CommandsOnStageB) &&
           !Gow3Toggle::Disabled(Gow3Toggle::PipelinedTasks);
}

void Rasterizer::RunCommandInOrder(Common::UniqueFunction<void>&& command) {
    auto* heap = new Common::UniqueFunction<void>(std::move(command));
    const OrderedTask task = [](Rasterizer&, const u8* data) {
        Common::UniqueFunction<void>* function = nullptr;
        std::memcpy(&function, data, sizeof(function));
        std::unique_ptr<Common::UniqueFunction<void>> owned{function};
        (*owned)();
    };
    RunInOrder(task, &heap, sizeof(heap));
}

void Rasterizer::FlushDrawPipeSignals(void* rasterizer) {
    static_cast<Rasterizer*>(rasterizer)->scheduler.FlushDeferredSignals();
}

void Rasterizer::RunDrawPacket(void* context, const u8* data, u32 size) {
    auto& self = *static_cast<Rasterizer*>(context);
    if (*reinterpret_cast<const PacketKind*>(data) == PacketKind::Task) {
        const auto& task = *reinterpret_cast<const TaskPacket*>(data);
        task.task(self, data + sizeof(TaskPacket));
        return;
    }
    const auto& packet = *reinterpret_cast<const DrawPacket*>(data);
    if (LateConstantsEnabled()) {
        // gow3 21 Oct 2026: this draw's constant ring copies are checked at the next submission.
        NoteLateDue(packet.ring_end);
    }
    const u8* in = data + sizeof(DrawPacket);
    const auto* blocks = reinterpret_cast<const u16*>(in);
    in += AlignPacket(packet.num_blocks * sizeof(u16));
    auto& regs = self.pipe_regs;
    if (packet.regs_reset) {
        regs.SetDefaults();
    }
    for (u32 i = 0; i < packet.num_blocks; ++i) {
        std::memcpy(regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords, in,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        in += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    ASSERT(packet.num_stages <= Shader::Info::MaxUdSnapshots);
    for (u32 i = 0; i < packet.num_stages; ++i) {
        const auto& stage = *reinterpret_cast<const PacketStage*>(in);
        in += sizeof(PacketStage);
        self.ring_stages[i] = {stage.info, reinterpret_cast<const RingBinding*>(in),
                               stage.num_ring};
        in += stage.num_ring * sizeof(RingBinding);
        const auto* words = reinterpret_cast<const u32*>(in);
        Shader::Info::ud_snapshots[i] = {stage.info, words, stage.ud_size, words + stage.ud_size,
                                         stage.flat_size, stage.pgm_base};
        in += AlignPacket((stage.ud_size + stage.flat_size) * sizeof(u32));
    }
    Shader::Info::num_ud_snapshots = packet.num_stages;
    self.num_ring_stages = packet.num_stages;
    const AmdGpu::ComputeProgram* cs = nullptr;
    if (packet.kind == PacketKind::Dispatch || packet.kind == PacketKind::IndirectDispatch ||
        packet.kind == PacketKind::DispatchRun) {
        cs = reinterpret_cast<const AmdGpu::ComputeProgram*>(in);
        in += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    const u8* run_tail = nullptr; // the run's other instances (DispatchRunRecord)
    if (packet.kind == PacketKind::DispatchRun) {
        run_tail = in;
        in = data + size;
    }
    const IndirectDraw* indirect = nullptr;
    if (packet.kind == PacketKind::Indirect || packet.kind == PacketKind::IndirectDispatch) {
        indirect = reinterpret_cast<const IndirectDraw*>(in);
        in += AlignPacket(sizeof(IndirectDraw));
    }
    if (packet.verify) {
        // The resource tables the GPU thread walked must read the same now: else a write
        // still queued here (WriteData, DMA) changed them after that thread read them.
        static u32 flat_reports = 0;
        for (u32 i = 0; i < packet.num_stages && flat_reports < 32; ++i) {
            const auto& snapshot = Shader::Info::ud_snapshots[i];
            const auto& srt = snapshot.info->srt_info;
            // Stages the pipeline selection did not refresh (no user data) keep their tables.
            if (!srt.walker_func || snapshot.flat_size == 0 || snapshot.user_data_size == 0) {
                continue;
            }
            std::vector<u32> flat(snapshot.flat_size);
            std::memcpy(flat.data(), snapshot.user_data,
                        std::min(snapshot.user_data_size, snapshot.flat_size) * sizeof(u32));
            // Pointers in the tables may be stale by now: a fault only skips the check.
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                continue;
            }
            runtime_fault_recover = &recover;
            srt.walker_func(snapshot.user_data, flat.data());
            runtime_fault_recover = nullptr;
            if (std::memcmp(flat.data(), snapshot.flat, snapshot.flat_size * sizeof(u32)) != 0) {
                u32 first = 0, count = 0;
                for (u32 w = 0; w < snapshot.flat_size; ++w) {
                    if (flat[w] != snapshot.flat[w]) {
                        first = count++ ? first : w;
                    }
                }
                std::printf("Draw pipe: resource tables of shader %016llx changed after the GPU "
                            "thread read them (%u of %u words, first %u: %#x -> %#x, user data "
                            "%u words)\n",
                            static_cast<unsigned long long>(snapshot.info->pgm_hash), count,
                            snapshot.flat_size, first, snapshot.flat[first], flat[first],
                            snapshot.user_data_size);
                ++flat_reports;
            }
        }
        const auto* live = reinterpret_cast<const AmdGpu::Regs*>(in);
        in += sizeof(AmdGpu::Regs);
        static u32 reports = 0;
        for (u32 block = 0; block < AmdGpu::RegDirty::NumBlocks && reports < 32; ++block) {
            const u32 at = block * AmdGpu::RegDirty::BlockWords;
            if (std::memcmp(regs.reg_array.data() + at, live->reg_array.data() + at,
                            AmdGpu::RegDirty::BlockWords * sizeof(u32)) != 0) {
                for (u32 w = at; w < at + AmdGpu::RegDirty::BlockWords; ++w) {
                    if (regs.reg_array[w] != live->reg_array[w]) {
                        std::printf("Draw pipe: register copy differs at word %#x (%#x vs %#x)\n",
                                    w, regs.reg_array[w], live->reg_array[w]);
                        ++reports;
                        break;
                    }
                }
            }
        }
    }
    ASSERT(static_cast<u32>(in - data) == size);
    stage_regs = &regs;
    self.pipe_cb_extent = packet.cb_extent;
    self.pipe_db_extent = packet.db_extent;
    const bool ds = DispatchStatsEnabled();
    const auto ds_t0 = ds ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (packet.kind == PacketKind::Special) {
        // A draw FilterDraw handles (fast clear elimination, resolve, depth copy, skip).
        FrameCapture::Poll();
        self.scheduler.PopPendingOperations();
        self.FilterDraw();
    } else if (cs) {
        stage_cs = cs;
        const auto* pipeline = static_cast<const ComputePipeline*>(packet.pipeline);
        if (run_tail) {
            self.DispatchRunRecord(pipeline, run_tail, data + size);
        } else if (indirect) {
            self.DispatchIndirectRecord(pipeline, indirect->args, indirect->stride);
        } else {
            self.DispatchRecord(pipeline);
        }
        stage_cs = nullptr;
    } else if (indirect) {
        self.DrawIndirectRecord(static_cast<const GraphicsPipeline*>(packet.pipeline),
                                packet.is_indexed, *indirect);
    } else {
        self.DrawRecord(static_cast<const GraphicsPipeline*>(packet.pipeline), packet.prepared,
                        packet.is_indexed, packet.index_offset);
    }
    if (ds && packet.kind != PacketKind::Special) {
        const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - ds_t0)
                           .count();
        const bool dispatch = cs != nullptr;
        // A run packet counts each of its dispatches (B us/each stays per dispatch).
        const u32 items = run_tail ? reinterpret_cast<const RunPacketHeader*>(run_tail)->count : 1;
        for (u32 item = 0; item < items; ++item) {
            DispatchStatsRecordB(dispatch,
                                 dispatch ? static_cast<const ComputePipeline*>(packet.pipeline)
                                                ->GetStage(Shader::SwStage::Compute)
                                                .pgm_hash
                                          : 0,
                                 ns / items);
        }
    }
    Shader::Info::num_ud_snapshots = 0;
    self.num_ring_stages = 0;
    if (self.constant_ring) {
        self.constant_ring->Stamp(packet.ring_end);
    }
}

void Rasterizer::PrintPipeStats() {
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    if (!stats || (draw_pipe->packets & 1023) != 0) {
        return;
    }
    static auto window = std::chrono::steady_clock::now();
    static u64 last_packets = 0, last_drains = 0, last_drain_cycles = 0, last_busy = 0;
    static u64 last_tsc = __rdtsc();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds < 5.0) {
        return;
    }
    const u64 tsc = __rdtsc();
    const double cycles = double(tsc - last_tsc);
    const u64 busy = draw_pipe->busy_cycles.load(std::memory_order_relaxed);
    std::printf("Draw pipe: %.0f draws/s pipelined, %.0f drains/s that waited, stage A waited "
                "%.1f%%, stage B busy %.1f%%; constant ring %.0f guest copies/s, %.1f MB/s\n",
                (draw_pipe->packets - last_packets) / seconds,
                (draw_pipe->drains - last_drains) / seconds,
                100.0 * (draw_pipe->drain_cycles - last_drain_cycles) / cycles,
                100.0 * (busy - last_busy) / cycles, ring_guest_copies / seconds,
                ring_guest_bytes / seconds / 1e6);
    if (ring_reused_bytes || RingReuseCheck()) {
        std::printf("  constant ring reuse: %.1f MB/s bound without a copy",
                    ring_reused_bytes / seconds / 1e6);
        if (RingReuseCheck()) {
            std::printf(", %llu of %llu checked reuses differed from guest memory",
                        static_cast<unsigned long long>(ring_reuse_mismatches),
                        static_cast<unsigned long long>(ring_reuse_checks));
            ring_reuse_mismatches = ring_reuse_checks = 0;
        }
        std::printf("\n");
    }
    ring_reused_bytes = 0;
    ring_guest_copies = 0;
    ring_guest_bytes = 0;
    std::printf("  constant ring epochs/s: drain %.0f, in-order task %.0f, GPU write %.0f, "
                "submission %.0f; kept over %.0f writes and %.0f command drains/s; %.0f copies/s "
                "left to stage B (ring in use)\n",
                ring_epoch_causes[EpochDrain] / seconds, ring_epoch_causes[EpochTask] / seconds,
                ring_epoch_causes[EpochWrite] / seconds,
                ring_epoch_causes[EpochSubmission] / seconds, ring_writes_kept / seconds,
                ring_command_drains_kept / seconds, ring_alloc_failures / seconds);
    ring_epoch_causes = {};
    ring_writes_kept = ring_command_drains_kept = ring_alloc_failures = 0;
    std::printf("  drains by reason/s:");
    for (u32 r = 0; r < DrawPipe::NumReasons; ++r) {
        if (const u64 n = draw_pipe->drains_by_reason[r]) {
            static constexpr const char* names[] = {"ConstRam", "commands", "compute",
                                                    "submission end", "draw", "rasterizer"};
            const double share = 100.0 * draw_pipe->cycles_by_reason[r] / cycles;
            draw_pipe->cycles_by_reason[r] = 0;
            if (r < 256) {
                std::printf(" %s=%.0f (%.1f%%)",
                            magic_enum::enum_name(AmdGpu::PM4ItOpcode(r)).data(), n / seconds,
                            share);
            } else {
                std::printf(" %s=%.0f (%.1f%%)", names[r - 256], n / seconds, share);
            }
        }
    }
    std::printf("\n  stage A waits over 0.5%%:");
    for (const auto& [function, lines] : drain_sites) {
        for (const auto& [line, waited] : lines) {
            if (waited * 200 > cycles) {
                std::printf(" %s:%u %.1f%%", function, line, 100.0 * waited / cycles);
            }
        }
    }
    std::printf("\n");
    drain_sites.clear();
    draw_pipe->drains_by_reason = {};
    window = now;
    last_tsc = tsc;
    last_packets = draw_pipe->packets;
    last_drains = draw_pipe->drains;
    last_drain_cycles = draw_pipe->drain_cycles;
    last_busy = busy;
}

bool Rasterizer::FilterDrawPasses() const {
    // FilterDraw's checks without its side effects: false when it would skip the draw or run
    // a pass of its own (fast clear elimination, resolve, depth/stencil copy).
    const auto& regs = Regs();
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear || mode == Mode::FmaskDecompress ||
        mode == Mode::Resolve || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return false;
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return !(mode == Mode::Disable && (depth_copy || stencil_copy));
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    Gow3Stats::draws.fetch_add(1, std::memory_order_relaxed);
    if (DispatchStatsEnabled()) {
        DispatchStatsDraw();
    }

    // gow3: with the draw pipeline this thread only selects the pipeline and hands the draw
    // to the recording thread (DrawRecord there); draws FilterDraw handles itself run here.
    const bool pipelined = UseDrawPipe();
    if (pipelined && !FilterDrawPasses() &&
        !Gow3Toggle::Disabled(Gow3Toggle::PipelinedMemoryWrites)) {
        PostDraw(nullptr, nullptr, false, 0);
        return;
    }
    if (!pipelined || !FilterDrawPasses()) {
        DrainDrawPipe(DrawPipe::ReasonDraw);
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline({}, prepared);
    const PreparedDraw* used_prepared = pipeline_cache.UsedPrepared();
    if (prepared) {
        draw_prep->Count(pipeline != nullptr &&
                         prepared->state.load(std::memory_order_acquire) == PreparedDraw::Ready &&
                         prepared->reg_checksum == liverpool->gfx_reg_checksum);
    }
    if (!pipeline) {
        // gow3 21 Oct 2026 (Uncharted 3 museum: a carpet never drawn, the scene's background red
        // through it): which draws find no pipeline, by their stage programs, per 5 s.
        static std::unordered_map<u64, u32> dropped;
        static auto dropped_report = std::chrono::steady_clock::now();
        const auto& regs = Regs();
        const auto program_hash = [&](u32 stage) -> u64 {
            const auto* pgm = regs.ProgramForStage(stage);
            return pgm && pgm->Address<u32*>() ? AmdGpu::GetParams(*pgm).hash : 0;
        };
        const u64 key = (program_hash(static_cast<u32>(Shader::HwStage::Fragment)) & 0xffffffffull) |
                        (u64(regs.stage_enable.raw) << 32) |
                        (u64(program_hash(static_cast<u32>(Shader::HwStage::Vertex)) & 0xffffff) << 40);
        ++dropped[key];
        if (const auto now = std::chrono::steady_clock::now();
            now - dropped_report >= std::chrono::seconds(5)) {
            dropped_report = now;
            std::string text;
            for (const auto& [k, n] : dropped) {
                text += fmt::format(" ps {:08x} vs {:06x}.. stages {:#x} x{}", u32(k), u32(k >> 40),
                                    u32((k >> 32) & 0xff), n);
            }
            std::printf("Draws without a pipeline (5 s):%s\n", text.c_str());
            std::fflush(stdout);
            dropped.clear();
        }
        return;
    }
    if (pipelined && FilterDrawPasses()) {
        PostDraw(pipeline, used_prepared, is_indexed, index_offset);
        return;
    }
    DrawRecord(pipeline, used_prepared, is_indexed, index_offset);
}


namespace {
/// Diagnostics: GOW3_SKIP_FILE=<file> lists shader hashes (hex, whitespace separated); draws
/// and dispatches using one are skipped. Re-read when the file changes, so shaders can be
/// switched off while the game runs.
/// gow3 21 Oct 2026 (U3 museum red carpet): values above 32 bits are index buffer addresses
/// (frame capture "mesh" lines); then only draws of those meshes are skipped, and only those using
/// a listed shader when shaders are listed too (one mesh in one pass).
bool SkippedShader(u64 first, u64 second = 0, u64 index_base = 0) {
    static const char* path = std::getenv("GOW3_SKIP_FILE");
    if (!path || !path[0]) {
        return false;
    }
    // 20 Oct 2026 audit: the stat + fopen ran on the draw path inside a mutex; a thread started on
    // first use re-reads the file every 500 ms and swaps the list; the draw path only copies a pointer.
    static std::mutex mutex;
    struct SkipList {
        std::vector<u64> hashes;
        std::vector<u64> meshes;
    };
    static std::shared_ptr<const SkipList> shared = std::make_shared<const SkipList>();
    static const bool poller = [] {
        std::thread([] {
            std::filesystem::file_time_type stamp{};
            for (;; std::this_thread::sleep_for(std::chrono::milliseconds(500))) {
                std::error_code ec;
                const auto time = std::filesystem::last_write_time(path, ec);
                std::shared_ptr<const SkipList> next;
                if (ec) {
                    next = std::make_shared<const SkipList>();
                    stamp = {};
                } else if (time != stamp) {
                    stamp = time;
                    auto list = std::make_shared<SkipList>();
                    if (FILE* f = std::fopen(path, "r")) {
                        unsigned long long value;
                        while (std::fscanf(f, "%llx", &value) == 1) {
                            (value > 0xffffffffull ? list->meshes : list->hashes).push_back(value);
                        }
                        std::fclose(f);
                    }
                    std::printf("Skip list: %zu shaders, %zu meshes\n", list->hashes.size(),
                                list->meshes.size());
                    next = std::move(list);
                } else {
                    continue;
                }
                std::scoped_lock lock{mutex};
                shared = std::move(next);
            }
        }).detach();
        return true;
    }();
    (void)poller;
    std::shared_ptr<const SkipList> list;
    {
        std::scoped_lock lock{mutex};
        list = shared;
    }
    const bool shader_listed = std::ranges::any_of(
        list->hashes, [&](u64 h) { return h == first || (second && h == second); });
    if (list->meshes.empty()) {
        return shader_listed;
    }
    return index_base && std::ranges::find(list->meshes, index_base) != list->meshes.end() &&
           (list->hashes.empty() || shader_listed);
}
} // namespace

// gow3 19 Oct 2026, from Supermedo/uncharted_pc #261 (kj2223): GOW3_JITTER_PS=<pixel shader
// hash>,... (tools/probe_uncharted.py: f614041e): full-screen passes that rebuild positions from
// the scene depth through an interpolated texture coordinate get the same jitter as the scene
// geometry. Unjittered, they rebuilt each pixel from depth rendered half a pixel away: the sun
// shadow pass (f614041e, shared by the three games) moved against the geometry with every jitter
// step and its shadows flickered under TAA, FSR and DLAA.
static bool JitterFullScreenPass(u64 hash) {
    static const std::vector<u64> listed = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("GOW3_JITTER_PS")) {
            for (const char* p = env; *p;) {
                char* end;
                const u64 value = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                list.push_back(u64(u32(value)));
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    return std::find(listed.begin(), listed.end(), u64(u32(hash))) != listed.end();
}

// gow3 24 Oct 2026: GOW3_JITTER_STATS=1 counts, among the draws that use the scene depth while an
// upscaler is on, those that got the jitter and those that did not (small non-instanced draws:
// decals, quads; the full-screen passes of JitterFullScreenPass are left out). Every ~5 s one line:
// frames, draws per frame of each kind and the top 5 pixel shaders of the unjittered ones. Only
// read on the thread that records draws.
static bool JitterStatsOn() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_JITTER_STATS");
        return env && env[0] == '1';
    }();
    return on;
}

static void NoteJitterStat(bool jittered, u64 ps_hash, u32 indices, u32 instances) {
    struct Unjittered {
        u64 count = 0;
        u32 indices = 0, instances = 0;
    };
    static u64 jittered_draws = 0, unjittered_draws = 0, last_frames = 0;
    static std::map<u64, Unjittered> unjittered;
    static auto last = std::chrono::steady_clock::now();
    if (jittered) {
        ++jittered_draws;
    } else {
        ++unjittered_draws;
        auto& entry = unjittered[u32(ps_hash)];
        ++entry.count;
        entry.indices = indices;
        entry.instances = instances;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) {
        return;
    }
    last = now;
    const u64 total = Gow3Stats::gpu_frames.load(std::memory_order_relaxed);
    const double frames = double(std::max<u64>(total - last_frames, 1));
    last_frames = total;
    std::vector<std::pair<u64, Unjittered>> top(unjittered.begin(), unjittered.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    std::printf("Jitter stats: %.0f frames, jittered %.1f/frame, unjittered %.1f/frame; top "
                "unjittered ps:",
                frames, double(jittered_draws) / frames, double(unjittered_draws) / frames);
    for (size_t i = 0; i < top.size() && i < 5; ++i) {
        std::printf(" %08x x%llu (idx %u inst %u)", u32(top[i].first),
                    static_cast<unsigned long long>(top[i].second.count), top[i].second.indices,
                    top[i].second.instances);
    }
    std::printf("\n");
    jittered_draws = unjittered_draws = 0;
    unjittered.clear();
}

// gow3 24 Oct 2026: GOW3_TARGET_DUMP_PS=<hex>[,<hex>...] and GOW3_TARGET_DUMP_TRIGGER=<file>: when the
// trigger file appears, in the next GPU frame (NoteFrameStart arms it, the following frame start
// disarms it) the color target 0 of up to 8 draws with a listed pixel shader is copied right after
// the draw to $GOW3_DUMP_DIR (default out/dump)/target_<hash>_<frame>_<n>_<w>x<h>_<vkformat>.raw
// and written once the GPU is done (same approach as DumpRaw in vk_presenter.cpp). The draw's
// pass ends there, so the next draw begins a new one: for measuring, not for playing.
static std::atomic<bool> g_target_dump_armed{false};
static std::atomic<u32> g_target_dump_left{0};

static const std::vector<u64>& TargetDumpShaders() {
    static const std::vector<u64> listed = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("GOW3_TARGET_DUMP_PS")) {
            for (const char* p = env; *p;) {
                char* end;
                const u64 value = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                list.push_back(u64(u32(value)));
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    return listed;
}

static void NoteTargetDumpFrame() {
    static const int trigger = TargetDumpShaders().empty()
                                   ? -1
                                   : Gow3TriggerFiles::Watch("GOW3_TARGET_DUMP_TRIGGER");
    if (trigger < 0) {
        return;
    }
    const bool armed = Gow3TriggerFiles::Take(trigger).has_value();
    g_target_dump_left.store(8, std::memory_order_relaxed);
    g_target_dump_armed.store(armed, std::memory_order_relaxed);
}

static void DumpTargetImage(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                            VideoCore::Image& image, u64 ps_hash) {
    const auto& list = TargetDumpShaders();
    if (std::find(list.begin(), list.end(), u64(u32(ps_hash))) == list.end() ||
        image.info.num_samples != 1 || image.info.num_bits < 8) {
        return;
    }
    u32 left = g_target_dump_left.load(std::memory_order_relaxed);
    if (left == 0) {
        return;
    }
    g_target_dump_left.store(left - 1, std::memory_order_relaxed);
    const u32 w = image.info.size.width, h = image.info.size.height;
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * (image.info.num_bits / 8),
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    scheduler.Record([src = image.GetImage(), buffer, w, h](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(src, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                 vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor,
                                                                          0, 0, 1},
                                                     .imageExtent = {w, h, 1}});
    });
    const char* dir = std::getenv("GOW3_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/target_%08x_%llu_%u_%ux%u_%u.raw",
                  dir && *dir ? dir : "out/dump", u32(ps_hash),
                  static_cast<unsigned long long>(Gow3Stats::gpu_frames.load(std::memory_order_relaxed)),
                  8 - left, w, h, u32(image.info.pixel_format));
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

void Rasterizer::DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                            bool is_indexed, u32 index_offset) {
    if (DispatchTraceTarget()) [[unlikely]] {
        DispatchTraceDraw();
    }
    const auto* fragment=pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    if (SkippedShader(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                      fragment ? fragment->pgm_hash : 0,
                      is_indexed ? Regs().index_base_address.Address<VAddr>() : 0)) {
        return;
    }
    scheduler.Checkpoint("draw",pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         fragment ? fragment->pgm_hash : 0);
    NoteHudDraw(fragment ? fragment->pgm_hash : 0, is_indexed);
    hud_anchor = HudLayout::Anchor::Centre;
    hud_split = false;
    if (hud_draw) {
        float min_x = 0.0f, max_x = 0.0f;
        std::string detail;
        // gow3 26 Oct 2026 (review): the trace text (16 reads and a format) is built only for the
        // first 3 draws of each shader, the ones printed below; the trace is on by default for
        // the U2 ultrawide HUD, so every HUD draw of every frame paid for it before.
        bool print = false;
        const u64 ps = fragment ? fragment->pgm_hash : 0;
        if (hud_trace) {
            static std::mutex trace_mutex;
            static std::unordered_map<u64, u32> traced;
            std::scoped_lock lock{trace_mutex};
            if (auto& n = traced[ps]; n < 3 && traced.size() <= 32) {
                ++n;
                print = true;
            }
        }
        const bool valid = HudExtent(pipeline, is_indexed, index_offset, min_x, max_x,
                                     print ? &detail : nullptr);
        // Only the in-game HUD is split by thirds in wide mode; menus stay in the 16:9 area.
        hud_anchor = HudLayout::Classify(valid, min_x, max_x, false);
        hud_split = hud_wide && hud_tracker.last_over_frame && hud_anchor != HudLayout::Anchor::Full;
        if (print) {
            {
                static constexpr const char* kNames[] = {"centre", "left", "right", "full"};
                std::printf("HUD draw: vs %08x ps %08x %u %s, x %s %.3f..%.3f -> %s%s\n",
                            u32(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash), u32(ps),
                            Regs().num_indices, is_indexed ? "indices" : "vertices",
                            valid ? "" : "(unknown)", min_x, max_x,
                            kNames[u32(hud_anchor)], hud_split ? " (split)" : "");
                const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
                const auto& fetch = pipeline->GetFetchShader();
                std::string text = fmt::format("HUD attribs: vs {:08x} ps {:08x}",
                                               u32(vs.pgm_hash), u32(ps));
                if (fetch && !fetch->attributes.empty()) {
                    for (const auto& a : fetch->attributes) {
                        const auto sharp = a.GetSharp(vs);
                        text += fmt::format(
                            "\n  attrib semantic {} step {} fmt {}/{} stride {} base {:#x} "
                            "elements {} inst_offset {}",
                            a.semantic, u32(a.GetStepRate()), u32(sharp.GetDataFmt()),
                            u32(sharp.GetNumberFmt()), sharp.GetStride(), sharp.base_address,
                            a.num_elements, a.inst_offset);
                    }
                } else {
                    text += fetch ? "\n  fetch shader has no attributes"
                                  : "\n  no fetch shader";
                    for (u32 i = 0; i < vs.buffers.size(); ++i) {
                        const auto sharp = vs.buffers[i].GetSharp(vs);
                        text += fmt::format(
                            "\n  vs buffer {}: base {:#x} stride {} records {} fmt {}/{}", i,
                            sharp.base_address, sharp.GetStride(), sharp.num_records,
                            u32(sharp.GetDataFmt()), u32(sharp.GetNumberFmt()));
                    }
                }
                if (!detail.empty()) {
                    text += "\n  " + detail;
                }
                std::printf("%s\n", text.c_str());
                std::fflush(stdout);
            }
        }
    }
    // gow3 21 Oct 2026 diagnostics (U3 museum red carpet), GOW3_MESH_DUMP=<index buffer hex>:
    // for draws of that mesh, the vertex shader's user data and the 16 dwords behind each
    // pointer of its resource table (as floats), once per vertex shader every 10 s, to compare
    // the matrices the depth pass and the shading passes read.
    if (static const u64 dump_ib = [] {
            const char* env = std::getenv("GOW3_MESH_DUMP");
            return env ? std::strtoull(env, nullptr, 16) : 0ull;
        }();
        dump_ib && is_indexed && Regs().index_base_address.Address<VAddr>() == dump_ib) {
        static std::mutex dump_mutex;
        static std::map<u64, std::chrono::steady_clock::time_point> dumped;
        // GOW3_MESH_DUMP_ALL=1: every draw of the mesh for 2 s every 15 s (all instances of
        // one frame), with the vertex shader's buffer descriptors.
        static const bool dump_all = std::getenv("GOW3_MESH_DUMP_ALL") != nullptr;
        static auto window_start = std::chrono::steady_clock::time_point{};
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{dump_mutex};
        bool due = false;
        if (dump_all) {
            if (now - window_start > std::chrono::seconds(15)) {
                window_start = now;
            }
            due = now - window_start < std::chrono::seconds(2);
        } else if (auto [it, fresh] = dumped.try_emplace(vs.pgm_hash, now);
                   fresh || now - it->second > std::chrono::seconds(10)) {
            it->second = now;
            due = true;
        }
        if (due) {
            const auto ud = vs.UserData();
            std::string text = fmt::format("Mesh dump: ib {:#x} vs {:08x} ps {:08x} user data",
                                           dump_ib, u32(vs.pgm_hash),
                                           u32(fragment ? fragment->pgm_hash : 0));
            for (size_t i = 0; i < std::min<size_t>(ud.size(), 16); ++i) {
                text += fmt::format(" {:08x}", ud[i]);
            }
            for (u32 i = 0; i < vs.buffers.size(); ++i) {
                const auto sharp = vs.buffers[i].GetSharp(vs);
                text += fmt::format("\n  vs buffer {}: base {:#x} stride {} records {} fmt {}/{}",
                                    i, sharp.base_address, sharp.GetStride(), sharp.num_records,
                                    u32(sharp.GetDataFmt()), u32(sharp.GetNumberFmt()));
            }
            if (fragment) {
                const auto& ps = *fragment;
                for (u32 i = 0; i < ps.images.size(); ++i) {
                    const auto image = ps.images[i].GetSharp(ps);
                    text += fmt::format(
                        "\n  ps image {}: {:#x} {}x{} fmt {}/{} tiling {} type {} levels {}-{} dst {}{}{}{}",
                        i, image.Address(), u32(image.width) + 1, u32(image.height) + 1,
                        u32(image.GetDataFmt()), u32(image.GetNumberFmt()),
                        u32(image.tiling_index), u32(image.type), u32(image.base_level),
                        u32(image.last_level), u32(image.dst_sel_x), u32(image.dst_sel_y),
                        u32(image.dst_sel_z), u32(image.dst_sel_w));
                }
                for (u32 i = 0; i < ps.buffers.size(); ++i) {
                    const auto sharp = ps.buffers[i].GetSharp(ps);
                    text += fmt::format("\n  ps buffer {}: base {:#x} stride {} records {} fmt {}/{}",
                                        i, sharp.base_address, sharp.GetStride(),
                                        sharp.num_records, u32(sharp.GetDataFmt()),
                                        u32(sharp.GetNumberFmt()));
                }
            }
            if (ud.size() >= 2) {
                const VAddr table = (u64(ud[1]) << 32 | ud[0]) & 0xffffffffffffull;
                u32 slots[32]{};
                memory->CopySparseMemory(table, reinterpret_cast<u8*>(slots), sizeof(slots));
                for (u32 k = 0; k < 16; ++k) {
                    const VAddr p = (u64(slots[2 * k + 1]) << 32 | slots[2 * k]) & 0xffffffffffffull;
                    if (p < 0x100000 || p >= 0x2000000000ull) {
                        continue;
                    }
                    float v[16]{};
                    memory->CopySparseMemory(p, reinterpret_cast<u8*>(v), sizeof(v));
                    text += fmt::format("\n  T[{},{}] -> {:#x}:", 2 * k, 2 * k + 1, p);
                    for (const float f : v) {
                        text += fmt::format(" {:.9g}", f);
                    }
                }
            }
            std::printf("%s\n", text.c_str());
            std::fflush(stdout);
        }
    }
    if (DrawPipe::OnStageB()) {
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
    }
    const auto& regs = Regs();
    const auto frame_start = [&] {
        scene_started = false;
        camera_motion->OnDisplayPass(cb_descs[0].first);
        if (upscaler->OnFrameStart()) {
            object_motion->InvalidateHistory();
            camera_motion->InvalidateHistory();
            Gow3Flight::Event(Gow3Flight::HistoryInvalidated); // 25 Oct 2026: flight recorder
        }
        camera_motion->SetJitter(upscaler->Jitter());
        // gow3 25 Oct 2026: this frame's jitter and texture LOD bias for the flight recorder.
        Gow3Flight::Gauge(Gow3Flight::JitX, upscaler->Jitter()[0]), Gow3Flight::Gauge(Gow3Flight::JitY, upscaler->Jitter()[1]),
            Gow3Flight::Gauge(Gow3Flight::LodBias, upscaler->SceneMipBias());
        object_motion->OnFrameStart();
        NoteFrameStart();
        static const char* scene_debug = std::getenv("GOW3_SCENE_DEBUG");
        static auto scene_debug_poll = std::chrono::steady_clock::time_point{};
        scene_debug_frame =
            scene_debug && TriggerPollDue(scene_debug_poll) && std::remove(scene_debug) == 0;
        scene_targets->debug = scene_debug_frame;
    };
    // gow3: the pass copying a finished frame to a display buffer; the previous draw's
    // target is that frame.
    // Uncharted 2 draws into the display buffer four times a frame (composite, UI, and a copy
    // back after a pass elsewhere): only the first one after scene drawing starts a frame.
    // 19 Oct 2026, from Supermedo/uncharted_pc #261 (kj2223), merged with upstream 0.3.1:
    // Uncharted 1 and 3 draw twice into the same display buffer per frame (484cdbf5, then
    // 749bbec9; measured 2 frame starts per shown frame): only the first starts a frame. The
    // second reset the upscaler's history every frame (shimmer on every edge under TAA, FSR and
    // DLAA) and skipped every other jitter phase. Uncharted 2 keeps the gate above. Toggle
    // 1 << 61 (OneFrameStart) brings the old behaviour back.
    // Upstream 0.4.0: when the game itself draws below the output size (velocity camera,
    // GOW3_RENDER_RES), frames start at the upscale instead (TemporalUpscaler::OnComposite):
    // its finished frame, the copy back and the HUD are several draws into the display buffer.
    if (camera_motion->Enabled() && !upscaler->CompositeFrames() && regs.color_buffers[0] &&
        FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address()) &&
        (camera_motion->FlatSource()
             ? !display_frame_open
             : (scene_started || regs.color_buffers[0].Address() != frame_display_address ||
                Gow3Toggle::Disabled(Gow3Toggle::OneFrameStart)))) {
        display_frame_open = true;
        frame_display_address = regs.color_buffers[0].Address();
        frame_start();
    }
    // gow3: without camera motion (Uncharted) the GPU profiler marks frames at the display pass
    // too. Its results are read with a wait four frames later, so everything recorded so far is
    // submitted first (a wait on unsubmitted timestamps hung the menu).
    else if (!camera_motion->Enabled() && GpuProfiler::Get() && regs.color_buffers[0] &&
             FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address())) {
        Flush();
        NoteFrameStart();
    }
    bind_prepared = used_prepared;
    motion_draw = pipeline->GetGraphicsKey().motion_vectors;
    motion_geometry = 0;

    PrepareRenderState(pipeline);
    if (upscaler->CompositeFrames() && fragment &&
        upscaler->OnComposite(fragment->pgm_hash, cb_descs[0].first)) {
        bind_prepared = nullptr; // as after a draw: later dispatches bind without its sharps
        frame_start();           // the guest's copy is replaced by the upscale
        return;
    }
    // 24 Oct 2026: the game's FXAA (last draw into the display buffer) is skipped under a
    // temporal AA; the image from the draw before it stays in the display buffer.
    if (fragment && regs.color_buffers[0] &&
        FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address()) &&
        upscaler->SkipsGameAa(fragment->pgm_hash)) {
        bind_prepared = nullptr;
        return;
    }
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2));
    }
    const PreparedDraw* draw_prepared = bind_prepared;
    // gow3: vertex and index buffers are resolved while the helper binds textures (their
    // commands are recorded after BeginRendering, as before).
    draw_inputs = {pipeline, draw_prepared, index_offset, is_indexed, true, false};
    const bool bound = BindResources(pipeline);
    bind_prepared = nullptr; // indirect draws and dispatches bind without prepared sharps
    const bool inputs_resolved = draw_inputs.resolved;
    draw_inputs.pending = false;
    if (!bound) {
        return;
    }
    if (instance.HasGpuCheckpoints()) {
        // Diagnostics: name this draw's resources in the device loss report.
        for (const VideoCore::ImageId image_id : bound_images) {
            const auto& image = texture_cache.GetImage(image_id);
            const auto& info = image.info;
            scheduler.Checkpoint("image", info.guest_address,
                                 u64(info.size.width) | u64(info.size.height) << 16 |
                                     u64(info.resources.levels) << 32 |
                                     u64(info.resources.layers) << 40 |
                                     u64(u32(info.pixel_format) & 0xffff) << 48);
        }
        for (const auto& bound : bound_buffers) {
            scheduler.Checkpoint("buffer",
                                 bound.buffer->CpuAddr() + bound.offset,
                                 u64(bound.size) | u64(bound.is_written) << 63);
        }
    }
    const auto state = BeginRendering(pipeline);
    if (float_color0 && !pipeline->FloatTargetHandle()) [[unlikely]] {
        SkipFloatTargetDraw(*pipeline);
        return;
    }

    if (!inputs_resolved) {
        ResolveVertexBuffers(pipeline, draw_prepared);
        if (is_indexed) {
            ResolveIndexBuffer(index_offset);
        }
    }
    // Allocate before emitting vertex/index state: a stream-buffer wrap may submit recording.
    vk::DescriptorBufferInfo hdr_settings{};
    if (upscaler->HdrChain() && pipeline->HdrShadowHandle() && !float_color0) {
        const auto& settings = Gow3Settings::Get();
        const std::array<float, 2> nits{float(settings.hdr_peak_nits.load()),
                                      float(settings.hdr_game_nits.load())};
        auto& stream = buffer_cache.GetStreamBuffer();
        const auto [data, offset] = stream.Map(sizeof(nits), instance.StorageMinAlignment());
        std::memcpy(data, nits.data(), sizeof(nits));
        stream.Commit();
        hdr_settings = {stream.Handle(), offset, sizeof(nits)};
    }
    EmitVertexBuffers();
    if (is_indexed) {
        EmitIndexBuffer();
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // gow3: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    // Uncharted pulls its vertices with buffer loads (no fetch shader, so SetupVertexInputs
    // leaves motion_geometry 0): the geometry is the set of buffers its vertex shader reads.
    if (motion_draw && !motion_geometry && camera_motion->FlatSource()) {
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        u64 hash = 0;
        for (const auto& resource : vs.buffers) {
            if (resource.IsSpecial() || resource.is_written) continue;
            const auto buffer = resource.GetSharp(vs);
            if (!buffer.Valid()) continue;
            hash = XXH3_64bits_withSeed(&buffer, sizeof(buffer), hash);
        }
        motion_geometry = hash;
    }
    // Upstream 0.5.0's character motion (velocity camera only: stencil_gate is never set with
    // the flat camera above).
    // Uncharted: the animated draws of its G-buffer passes are the skinned ones, whose vertex
    // buffers the GPU wrote this session (the skinning compute; the level's vertices stay as
    // loaded). Its shaders read vertices from buffers (no vertex inputs): the draw's identity
    // across frames comes from those buffers' sizes and strides, not their addresses (skinned
    // vertices can move between frames). (The stencil's character bit differs between the
    // games: Uncharted 2 sets it on the whole level.)
    bool character_draw = false;
    if (motion_draw && Shader::MotionVectors::stencil_gate) {
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        u64 hash = 0x6a09e667f3bcc909ULL;
        for (const auto& resource : vs.buffers) {
            if (resource.IsSpecial()) continue;
            const auto buffer = resource.GetSharp(vs);
            if (!buffer.Valid()) continue;
            hash = XXH3_64bits_withSeed(&buffer.num_records, sizeof(buffer.num_records),
                                        hash ^ buffer.GetStride());
            // Constant buffers (stride 16) are not vertices.
            character_draw |= buffer.GetStride() != 16 &&
                              buffer_cache.IsRangeGpuWrittenShared(buffer.base_address,
                                                                    buffer.GetSize());
        }
        if (character_draw && !motion_geometry) {
            motion_geometry = hash;
        }
    }
    if (motion_draw && motion_geometry &&
        (!Shader::MotionVectors::stencil_gate || character_draw)) {
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto [base_vertex, first_instance] =
            GetDrawOffsets(regs, vs, pipeline->GetFetchShader());
        const u32 index_size =
            regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
        const VAddr index_address = is_indexed
            ? regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size : 0;
        // Without a character-size skeleton the draw is gated on its bone palette changing
        // since last frame (model constants follow the camera, the palette does not). The
        // check comes first: most gated draws are static world pieces with long index lists.
        static const bool all_motion = [] {
            const char* value = std::getenv("GOW3_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        // Uncharted: no bone palettes to gate on, and its per-draw constants change every frame
        // for static objects too (pointers into per-frame memory, camera-relative matrices; the
        // camera always sways): every opaque scene draw gets history. Gating it cost a user
        // data hash and three map lookups per draw and kept nothing out while playing.
        bool gated = !all_motion && !camera_motion->FlatSource() && !character_draw;
        u64 palette = 0;
        for (const auto& resource : vs.buffers) {
            if (!gated) break;
            if (resource.IsSpecial()) continue;
            const auto buffer = resource.GetSharp(vs);
            const u32 size = buffer.GetSize();
            if (!buffer.Valid() || buffer.GetStride() != 16) continue;
            const auto role = Motion::ClassifyBuffer(size);
            if (role == Motion::BufferRole::Skeleton) {
                gated = false;
                break;
            }
            const VAddr address = buffer.base_address;
            if (role == Motion::BufferRole::SmallSkeleton &&
                memory->IsValidGpuMapping(address, 0) &&
                memory->ClampRangeSize(address, size) == size) {
                palette = XXH3_64bits_withSeed(reinterpret_cast<const void*>(address), size,
                                               palette);
            }
        }
        push_data.motion_param = 0;
        if (!gated || object_motion->Moving({.shader = vs.pgm_hash,
                                             .geometry = motion_geometry,
                                             .indices = index_address,
                                             .index_count = regs.num_indices,
                                             .instances = regs.num_instances.NumInstances(),
                                             .first_instance = first_instance},
                                            palette)) {
            Motion::VertexRange range{base_vertex, regs.num_indices};
            u64 topology = 0;
            if (is_indexed) {
                range = {};
                const u64 bytes = u64(regs.num_indices) * index_size;
                if (index_address && memory->IsValidGpuMapping(index_address, 0) &&
                    memory->ClampRangeSize(index_address, bytes) == bytes) {
                    const bool restart = regs.enable_primitive_restart != 0;
                    const u32 count = regs.num_indices;
                    const auto scanned = object_motion->IndexRange(
                        {index_address, count, index_size, s32(base_vertex), restart}, [&] {
                            const auto* data = reinterpret_cast<const void*>(index_address);
                            const auto scan_range =
                                index_size == 2
                                    ? Motion::IndexedRange(
                                          std::span(static_cast<const u16*>(data), count),
                                          s32(base_vertex), restart)
                                    : Motion::IndexedRange(
                                          std::span(static_cast<const u32*>(data), count),
                                          s32(base_vertex), restart);
                            return Motion::IndexRangeCache::Result{scan_range,
                                                                   XXH3_64bits(data, bytes)};
                        });
                    range = scanned.range;
                    topology = scanned.topology;
                }
            }
            push_data.motion_param = object_motion->PrepareDraw({
                .shader = vs.pgm_hash,
                .geometry = motion_geometry,
                .indices = index_address,
                .topology = topology,
                .index_count = regs.num_indices,
                .instances = regs.num_instances.NumInstances(),
                .first_instance = first_instance,
                .vertices = range,
            });
        }
    }
    pipeline->BindResources(set_writes, push_data);
    // gow3: jitter geometry drawn with the scene depth, not full-screen passes (a shifted
    // full-screen quad leaves an edge column unwritten).
    draw_jitter = {};
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    const bool full_screen = regs.num_indices <= 6 && regs.num_instances.NumInstances() <= 1;
    // For the jitter stats (blurry-textures branch): which kind of jitter this draw got.
    bool scene_jittered = false, pass_jittered = false;
    // gow3 25 Oct 2026 (player: whole lamp shadows flicker on and off with DLSS, TAA or FSR,
    // never without an upscaler): what this draw samples. bound_images holds its targets too;
    // binding.is_bound marks the images BindTextures sampled (reset by ResetBindings). A pass
    // that samples the jittered scene depth and tests against a shadow map (a compare sampler,
    // or another depth-format image) rebuilds positions from depth rendered up to half a pixel
    // away: where that exceeds the shadow bias, the whole shadow test flips every jitter step.
    bool samples_scene_depth = false, samples_other_depth = false;
    u32 compare_samplers = 0;
    if (upscaler->Enabled() && ps && camera_motion->Depth()) {
        for (const VideoCore::ImageId id : bound_images) {
            const auto& image = texture_cache.GetImage(id);
            if (!image.binding.is_bound || !image.info.props.is_depth) {
                continue;
            }
            (JitteredDepth(id) ? samples_scene_depth : samples_other_depth) = true;
        }
        if (samples_scene_depth) {
            for (const auto& sampler : ps->samplers) {
                compare_samplers += sampler.is_depth ? 1u : 0u;
            }
        }
    }
    const bool shadow_candidate =
        samples_scene_depth && (compare_samplers > 0 || samples_other_depth);
    const bool shadow_jitter = !Gow3Toggle::Disabled2(Gow3Toggle::ShadowPassJitter);
    // Full-screen passes: the jitter is in scene pixels. A pass drawn at another size (a
    // half-resolution shadow mask) moves by the same fraction of the screen, not the same number
    // of its own pixels (its viewport covers the screen the scene depth covers). ShadowPassJitter
    // set: the old unscaled shift.
    const auto full_screen_jitter = [&] {
        auto jitter = upscaler->Jitter();
        const auto& vp_ctl = regs.viewport_control;
        const auto& vp = regs.viewports[0];
        if (const auto depth = camera_motion->Depth(); shadow_jitter && depth &&
            vp_ctl.xscale_enable && vp_ctl.yscale_enable && vp.xscale > 0.0f && vp.yscale != 0.0f) {
            const auto& size = texture_cache.GetImage(depth).info.size;
            jitter[0] *= 2.0f * vp.xscale / float(size.width);
            jitter[1] *= 2.0f * std::abs(vp.yscale) / float(size.height);
        }
        return jitter;
    };
    // 24 Oct 2026 (JitterSmallDraws, opt-in): small non-instanced draws with the scene depth are
    // jittered too; a full-screen quad with the scene depth may then leave an edge column.
    if (upscaler->Enabled() && JitteredDepth(db_desc.first) &&
        (!full_screen || Gow3Toggle::Disabled2(Gow3Toggle::JitterSmallDraws))) {
        draw_jitter = upscaler->Jitter();
        scene_jittered = true;
    } else if (ps && upscaler->Enabled() && JitterFullScreenPass(ps->pgm_hash) &&
               !Gow3Toggle::Disabled(Gow3Toggle::FullScreenPassJitter)) {
        draw_jitter = full_screen_jitter();
        pass_jittered = true;
        static bool noted = false;
        if (!noted) {
            noted = true;
            std::printf("Upscaler: full-screen pass %08x gets the scene jitter\n",
                        u32(ps->pgm_hash));
        }
    } else if (full_screen && shadow_candidate && shadow_jitter) {
        // gow3 25 Oct 2026: the lamp (point, spot) shadow passes are other shaders than the sun's
        // f614041e in GOW3_JITTER_PS; found by what they sample instead of by hash.
        draw_jitter = full_screen_jitter();
        pass_jittered = true;
        static std::vector<u32> noted;
        if (std::ranges::find(noted, u32(ps->pgm_hash)) == noted.end()) {
            noted.push_back(u32(ps->pgm_hash));
            std::printf("Upscaler: shadow pass %08x gets the scene jitter (auto)\n",
                        u32(ps->pgm_hash));
        }
    }
    // Diagnostics, once per pixel shader: shadow pass candidates, and jittered light volumes
    // (geometry) that test shadows but may build their screen UV from FragCoord, which the
    // viewport shift does not move (no behaviour change for those).
    if (shadow_candidate) {
        const bool jittered = draw_jitter[0] != 0.0f || draw_jitter[1] != 0.0f;
        const bool light_volume = jittered && !full_screen && compare_samplers > 0;
        static std::vector<u32> seen_shadow, seen_volume;
        const u32 hash = u32(ps->pgm_hash);
        const bool new_shadow = std::ranges::find(seen_shadow, hash) == seen_shadow.end();
        const bool new_volume =
            light_volume && std::ranges::find(seen_volume, hash) == seen_volume.end();
        if (new_shadow || new_volume) {
            u32 width = 0, height = 0;
            if (cb_descs[0].first) {
                const auto& size = texture_cache.GetImage(cb_descs[0].first).info.size;
                width = size.width, height = size.height;
            }
            const u32 vs_hash = u32(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash);
            if (new_shadow) {
                seen_shadow.push_back(hash);
                std::printf("Shadow pass candidate: ps %08x vs %08x indices %u instances %u "
                            "jittered %d target %ux%u compare-samplers %u other-depth %d "
                            "screen-space %d\n",
                            hash, vs_hash, regs.num_indices, regs.num_instances.NumInstances(),
                            int(jittered), width, height, compare_samplers,
                            int(samples_other_depth), int(regs.IsClipDisabled()));
            }
            if (new_volume) {
                seen_volume.push_back(hash);
                std::printf("Light volume candidate: ps %08x vs %08x indices %u instances %u "
                            "target %ux%u compare-samplers %u\n",
                            hash, vs_hash, regs.num_indices, regs.num_instances.NumInstances(),
                            width, height, compare_samplers);
            }
            std::fflush(stdout);
        }
    }
    if (JitterStatsOn() && !pass_jittered && upscaler->Enabled() && JitteredDepth(db_desc.first)) {
        NoteJitterStat(scene_jittered, ps ? ps->pgm_hash : 0, regs.num_indices,
                       regs.num_instances.NumInstances());
    }
    if (hud_split) {
        hud_anchor = HudLayout::Anchor::Left; // first of the passes below
    }
    UpdateDynamicState(pipeline, is_indexed);
    MarkPass(pipeline, state);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const vk::Pipeline handle =
        float_color0 ? pipeline->FloatTargetHandle() : pipeline->Handle();
    const vk::Pipeline square = float_color0 ? vk::Pipeline{} : SquarePass(*pipeline);
    const u32 num_indices = regs.num_indices;
    const u32 num_instances = regs.num_instances.NumInstances();
    const u32 first_vertex = vertex_offset;
    const u32 first_instance = instance_offset;
    const auto record_draw = [=](vk::CommandBuffer cmdbuf) {
        const auto draw = [&] {
            if (is_indexed) {
                cmdbuf.drawIndexed(num_indices, num_instances, 0, s32(first_vertex),
                                   first_instance);
            } else {
                cmdbuf.draw(num_indices, num_instances, first_vertex, first_instance);
            }
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        draw();
        if (square) [[unlikely]] {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, square);
            draw();
        }
    };
    scheduler.Record(record_draw);
    if (hud_split) {
        // The other two passes: the same draw with the middle and right placement (viewport and
        // scissor re-emitted; the dynamic state tracker only sends what changed).
        for (const auto anchor : {HudLayout::Anchor::Centre, HudLayout::Anchor::Right}) {
            hud_anchor = anchor;
            UpdateDynamicState(pipeline, is_indexed);
            scheduler.Record(record_draw);
        }
        hud_split = false;
        hud_anchor = HudLayout::Anchor::Centre;
    }
    if (upscaler->HdrChain()) [[unlikely]] {
        RecordHdrShadow(*pipeline, state, is_indexed, num_indices, num_instances, first_vertex,
                        first_instance, hdr_settings);
    }
    if (g_target_dump_armed.load(std::memory_order_relaxed)) [[unlikely]] {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        if (ps && state.num_color_attachments > 0 && cb_descs[0].first) {
            DumpTargetImage(instance, scheduler, runtime, texture_cache.GetImage(cb_descs[0].first),
                            ps->pgm_hash);
        }
    }
    // gow3: the camera of the game's velocity pass (GOW3_VELOCITY_PS, Uncharted): its constant
    // buffer, as this draw reads it, for the upscaler's motion vectors.
    if (camera_motion->VelocityMode()) {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        if (ps && camera_motion->IsVelocityShader(ps->pgm_hash) && !ps->buffers.empty() &&
            !ps->buffers[0].IsSpecial()) {
            const PreparedStage* prepared_stage = nullptr;
            for (u32 i = 0; draw_prepared && i < draw_prepared->num_stages; ++i) {
                const auto& candidate = draw_prepared->stages[i];
                if (&candidate.program->info == ps && candidate.num_buffers == ps->buffers.size()) {
                    prepared_stage = &candidate;
                }
            }
            const auto sharp = prepared_stage ? prepared_stage->buffer_sharps[0]
                                              : ps->buffers[0].GetSharp(*ps);
            const u32 bytes = sharp.GetSize();
            if (sharp.base_address && bytes >= 64 * sizeof(float) &&
                memory->IsValidGpuMapping(sharp.base_address, bytes)) {
                camera_motion->OnVelocityConstants(
                    reinterpret_cast<const float*>(sharp.base_address), bytes / sizeof(float));
            }
        }
    }
    if (FrameCapture::Active()) {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        FrameCapture::Draw(vs_info.pgm_hash, ps ? ps->pgm_hash : 0, num_indices, num_instances);
        const auto& dc = regs.depth_control;
        const auto& rc = regs.depth_render_control;
        const auto& sr = regs.stencil_ref_front;
        const auto effective = GetEffectiveDepthStencilState(regs);
        char depth_note[320];
        std::snprintf(depth_note, sizeof(depth_note),
                      "\n  state vs %08x ps %08x: depth %u write %u func %u clear %u stencil %u func %u ref %u opval %u mask %u write %u clear %u effective %u%u%u%u",
                      u32(vs_info.pgm_hash), u32(ps ? ps->pgm_hash : 0), u32(dc.depth_enable),
                      u32(dc.depth_write_enable), u32(dc.depth_func), u32(rc.depth_clear_enable),
                      u32(dc.stencil_enable), u32(dc.stencil_ref_func), u32(sr.stencil_test_val),
                      u32(sr.stencil_op_val), u32(sr.stencil_mask), u32(sr.stencil_write_mask),
                      u32(rc.stencil_clear_enable), u32(effective.needs_attachment),
                      u32(effective.depth_test_enable), u32(effective.depth_write_enable),
                      u32(effective.stencil_test_enable));
        FrameCapture::Note(depth_note);
        {
            // gow3 21 Oct 2026 (U3 museum red carpet): the mesh of each draw (index buffer, first
            // index, count, base vertex, instances), to pair a depth-pass draw with the material
            // pass draw of the same mesh and with GOW3_SRT_CHECK lines.
            char mesh_note[160];
            std::snprintf(mesh_note, sizeof(mesh_note),
                          "\n  mesh vs %08x ps %08x: ib %#llx first %u n %u%s base vertex %u inst %u",
                          u32(vs_info.pgm_hash), u32(ps ? ps->pgm_hash : 0),
                          static_cast<unsigned long long>(
                              is_indexed ? regs.index_base_address.Address<VAddr>() : 0),
                          is_indexed ? index_offset : 0, num_indices,
                          is_indexed ? (regs.index_buffer_type.index_type ==
                                                AmdGpu::IndexType::Index16
                                            ? " u16"
                                            : " u32")
                                     : " (not indexed)",
                          first_vertex, num_instances);
            FrameCapture::Note(mesh_note);
        }
        {
            // gow3: depth transform evidence (D16 shadow map cleared by a z=-1 RectList quad).
            const auto& cc = regs.clipper_control;
            const auto& vc = regs.viewport_control;
            const auto& vp = regs.viewports[0];
            const auto& vd = regs.viewport_depths[0];
            char clip_note[320];
            std::snprintf(clip_note, sizeof(clip_note),
                          "\n  clip vs %08x: prim %u clip_disable %u/%u space %u zclip near/far off %u%u "
                          "vte z scale/off %u%u xy/z/w transformed %u%u%u zscale %g zoffset %g zmin %g zmax %g "
                          "vp_clamp_off %u",
                          u32(vs_info.pgm_hash), u32(regs.primitive_type), u32(cc.clip_disable),
                          u32(regs.IsClipDisabled()), u32(cc.clip_space), u32(cc.zclip_near_disable),
                          u32(cc.zclip_far_disable), u32(vc.zscale_enable), u32(vc.zoffset_enable),
                          u32(vc.xy_transformed), u32(vc.z_transformed), u32(vc.w_transformed), vp.zscale,
                          vp.zoffset, vd.zmin, vd.zmax,
                          u32(regs.depth_render_override.disable_viewport_clamp));
            FrameCapture::Note(clip_note);
            const auto& vs0 = regs.viewport_scissors[0];
            char rect_note[320];
            std::snprintf(rect_note, sizeof(rect_note),
                          "\n  rect vs %08x: vp x %g+%g y %g+%g; scissors screen %d,%d-%d,%d window %d,%d-%d,%d "
                          "generic %d,%d-%d,%d vp0 %d,%d-%d,%d (vport_scissor %u) depth slices %u-%u",
                          u32(vs_info.pgm_hash), vp.xoffset, vp.xscale, vp.yoffset, vp.yscale,
                          int(regs.screen_scissor.top_left_x), int(regs.screen_scissor.top_left_y),
                          int(regs.screen_scissor.bottom_right_x), int(regs.screen_scissor.bottom_right_y),
                          int(regs.window_scissor.top_left_x), int(regs.window_scissor.top_left_y),
                          int(regs.window_scissor.bottom_right_x), int(regs.window_scissor.bottom_right_y),
                          int(regs.generic_scissor.top_left_x), int(regs.generic_scissor.top_left_y),
                          int(regs.generic_scissor.bottom_right_x), int(regs.generic_scissor.bottom_right_y),
                          int(vs0.top_left_x), int(vs0.top_left_y), int(vs0.bottom_right_x),
                          int(vs0.bottom_right_y), u32(regs.mode_control.vport_scissor_enable),
                          u32(regs.depth_view.slice_start), u32(regs.depth_view.slice_max));
            FrameCapture::Note(rect_note);
        }
        if (const auto& bc = regs.blend_control[0]; bc.enable && regs.color_buffers[0]) {
            char note[128];
            std::snprintf(note, sizeof(note), "\n  blend ps %08x idx %u: src %u dst %u func %u z %u%u",
                          u32(ps ? ps->pgm_hash : 0), num_indices, u32(bc.color_src_factor),
                          u32(bc.color_dst_factor), u32(bc.color_func),
                          u32(regs.depth_control.depth_enable),
                          u32(regs.depth_control.depth_write_enable));
            FrameCapture::Note(note);
        }
    }
    DebugState.IncDrawCall();

    ResetBindings(false);
    scheduler.KickRecording();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;
    // gow3: like direct draws, handed to the recording thread after the pipeline selection
    // (else the GPU thread waited here for a whole frame of queued draws).
    const bool pipelined = UseDrawPipe() && FilterDrawPasses() &&
                           !Gow3Toggle::Disabled(Gow3Toggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }

    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        return;
    }
    const IndirectDraw indirect = {arg_address + offset, count_address, stride, max_count};
    if (pipelined) {
        PostDraw(pipeline, nullptr, is_indexed, 0, nullptr, &indirect);
        return;
    }
    DrawIndirectRecord(pipeline, is_indexed, indirect);
}

void Rasterizer::DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                                    const IndirectDraw& indirect) {
    if (DispatchTraceTarget()) [[unlikely]] {
        DispatchTraceDraw();
    }
    if (DrawPipe::OnStageB()) {
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
    }
    const VAddr count_address = indirect.count;
    const u32 stride = indirect.stride;
    const u32 max_count = indirect.max_count;

    // Indirect arguments may be GPU-written: leave these draws on camera fallback.
    motion_draw = false;
    motion_geometry = 0;
    {
        const auto* fragment = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        NoteHudDraw(fragment ? fragment->pgm_hash : 0, is_indexed);
        hud_anchor = HudLayout::Anchor::Centre; // GPU-written arguments: no extent
        hud_split = false;
    }
    PrepareRenderState(pipeline);
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2));
    }
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);
    if (float_color0 && !pipeline->FloatTargetHandle()) [[unlikely]] {
        SkipFloatTargetDraw(*pipeline);
        return;
    }

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer();
    }

    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(indirect.args, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // gow3: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    pipeline->BindResources(set_writes, push_data);
    draw_jitter = {};
    if (upscaler->Enabled() && JitteredDepth(db_desc.first)) {
        draw_jitter = upscaler->Jitter();
    }
    UpdateDynamicState(pipeline, is_indexed);
    MarkPass(pipeline, state);
    if (FrameCapture::Active()) {
        // Indirect draws: arguments as the CPU sees them (GPU-written ones may be stale here).
        const auto* vs = pipeline->GetStages()[u32(Shader::SwStage::Vertex)];
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        const u32* args = reinterpret_cast<const u32*>(indirect.args);
        const u32 count = count_address ? *reinterpret_cast<const u32*>(count_address) : 1;
        FrameCapture::Draw(vs ? vs->pgm_hash : 0, ps ? ps->pgm_hash : 0, args[0],
                           std::max(args[1], 1u));
        char note[200];
        std::snprintf(note, sizeof(note),
                      "\n  indirect vs %08x ps %08x: args %llx stride %u max %u count@%llx=%u "
                      "first %u %u %u %u %u",
                      u32(vs ? vs->pgm_hash : 0), u32(ps ? ps->pgm_hash : 0),
                      (unsigned long long)indirect.args, stride, max_count,
                      (unsigned long long)count_address, count, args[0], args[1], args[2], args[3],
                      args[4]);
        FrameCapture::Note(note);
    }
    scheduler.BeginRendering(state);

    ASSERT(stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                 : sizeof(VkDrawIndirectCommand)));
    const vk::Pipeline handle =
        float_color0 ? pipeline->FloatTargetHandle() : pipeline->Handle();
    const vk::Pipeline square = float_color0 ? vk::Pipeline{} : SquarePass(*pipeline);
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    const vk::Buffer counts = count_address != 0 ? count_buffer->Handle() : vk::Buffer{};
    const u64 counts_offset = count_address != 0 ? count_offset : 0;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        for (const vk::Pipeline bound : {handle, square}) {
            if (!bound) {
                break;
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, bound);
            if (is_indexed) {
                if (counts) {
                    cmdbuf.drawIndexedIndirectCount(args, args_offset, counts, counts_offset, max_count,
                                                    stride);
                } else {
                    cmdbuf.drawIndexedIndirect(args, args_offset, max_count, stride);
                }
            } else if (counts) {
                cmdbuf.drawIndirectCount(args, args_offset, counts, counts_offset, max_count, stride);
            } else {
                cmdbuf.drawIndirect(args, args_offset, max_count, stride);
            }
        }
    });
    DebugState.IncDrawCall();

    ResetBindings(false);
    scheduler.KickRecording();
}

// gow3 diagnostics (GOW3_FULL_BARRIERS=1): a full memory and execution barrier before every
// compute dispatch (all earlier GPU work done and visible). Uncharted 3's cloth flicker shows only
// above 30 FPS: a missing read-before-write dependency between a draw and the next frame's cloth
// dispatch would only bite when the two are recorded back to back (16 Oct).
static u64 g_cloth_dispatches = 0; ///< GOW3_CLOTH_PEEK dispatch numbers

// gow3 diagnostics: who reads the cloth solvers' outputs (ClothTrace, further down).
// GOW3_CLOTH_TRACE=1 (no GPU copies, unlike GOW3_CLOTH_PEEK=1, which turns it on as well).
static bool ClothTraceEnabled() {
    static const bool enabled = [] {
        const char* trace = std::getenv("GOW3_CLOTH_TRACE");
        const char* peek = std::getenv("GOW3_CLOTH_PEEK");
        return (trace && trace[0] == '1') || (peek && peek[0] == '1');
    }();
    return enabled;
}

// gow3 diagnostics (GOW3_CLOTH_TRACE): the barrier epoch after a solver dispatch put its writes
// in the barrier tracker. A read of its newest output in the same epoch must see a hazard; one
// that does not would run without a barrier after the solver (a missed hazard).
static u32 g_cloth_epoch = 0;
static u64 g_cloth_epoch_shader = 0;
static bool FullBarriersEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_FULL_BARRIERS");
        return env && env[0] == '1';
    }();
    return enabled;
}
static void RecordFullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

static bool TracedShader(u64 hash);

namespace {
/// The bytes of a bound buffer the barrier tracker sees (BindBuffers, 17 Oct 2026).
u64 TrackedBufferSize(const Shader::BufferResource& desc, const AmdGpu::Buffer& vsharp, u64 size) {
    static const bool exact = [] {
        const char* env = std::getenv("GOW3_EXACT_STRIDED_RANGES");
        return !(env && env[0] == '0');
    }();
    const u32 extent = desc.access_extent;
    if (exact && extent != 0 && extent != ~0u && vsharp.stride != 0 && extent < vsharp.stride &&
        vsharp.num_records != 0 && !vsharp.swizzle_enable && !vsharp.add_tid_enable) {
        return std::min<u64>(size, u64(vsharp.num_records - 1) * vsharp.stride + extent);
    }
    return size;
}

/// GOW3_READBACK_SKIP=<hex hash>: a shader whose writes are not read back (BindBuffers).
u64 ReadbackSkipShader() {
    static const u64 hash = [] {
        const char* env = std::getenv("GOW3_READBACK_SKIP");
        return env ? std::strtoull(env, nullptr, 16) : 0ULL;
    }();
    return hash;
}

// gow3 24 Oct 2026 (GOW3_DISPATCH_RUNS=1, Gow3Toggle::DispatchRuns): U3 Cartagena at 4K DLSS
// Quality (GOW3_DISPATCH_STATS=1): stage B 78-86% busy at ~400k pipe items/s is the frame limit,
// and cs 8f067540 is 1,522 dispatches a frame, 100% consecutive (runs up to 1,524), stage A
// 2.4 us + stage B 2.9 us each: ~3.7 ms of A and ~4.4 ms of B per 12-13 ms frame. The opt-in
// fusion (AppendSkinningBatch) cannot shorten B: it runs after BindResources, and every
// Scheduler::Record() flushes its batch, so its batches were singletons. A run is staged by
// stage A instead and goes to stage B as one packet of up to 64, recorded there as one
// vkCmdDispatch(dim_x, count, 1) with the bindings each dispatch would get; whatever stage B
// cannot prove exact is replayed through DispatchRecord one dispatch at a time.
bool DispatchRunsRequested() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_DISPATCH_RUNS");
        if (!env || env[0] != '1') {
            return false;
        }
        const auto set = [](const char* name) {
            const char* value = std::getenv(name);
            return value && value[0];
        };
        const char* upscale = std::getenv("GOW3_UPSCALE_BEFORE_CS");
        // Diagnostics that look at every dispatch on its own.
        const char* off = set("GOW3_SKIP_FILE")                      ? "GOW3_SKIP_FILE"
                          : DispatchTraceTarget()                    ? "GOW3_DISPATCH_TRACE"
                          : set("GOW3_DUMP_CS_CONST")                ? "GOW3_DUMP_CS_CONST"
                          : set("GOW3_TRACKER_CAP")                  ? "GOW3_TRACKER_CAP"
                          : set("GOW3_TRACE_BIND_RANGE")             ? "GOW3_TRACE_BIND_RANGE"
                          : FullBarriersEnabled()                    ? "GOW3_FULL_BARRIERS"
                          : LateConstantsEnabled()                   ? "GOW3_LATE_CONSTANTS"
                          : VerifyInterval()                         ? "GOW3_PIPE_VERIFY"
                          : !PipeWriteEnd()                          ? "GOW3_PIPE_WRITE_END=0"
                          : BarrierStatsEnabled()                    ? "GOW3_BARRIER_STATS"
                          : ClothTraceEnabled()                      ? "GOW3_CLOTH_TRACE/PEEK"
                          : TracedShader(0x8f067540)                 ? "GOW3_TRACE_PS"
                          : upscale && std::strtoull(upscale, nullptr, 16) == 0x8f067540
                              ? "GOW3_UPSCALE_BEFORE_CS"
                              : nullptr;
        if (off) {
            std::printf("Dispatch runs: off (%s is set)\n", off);
            return false;
        }
        if (const char* fusion = std::getenv("GOW3_DISPATCH_FUSION"); fusion && fusion[0] == '1') {
            std::printf("Dispatch runs: GOW3_DISPATCH_FUSION=1 is ignored (runs replace it)\n");
        }
        std::printf("Dispatch runs: on (Disabled2 bit 40 turns them off live)\n");
        std::fflush(stdout);
        return true;
    }();
    return on;
}

/// GOW3_DISPATCH_RUN_VERIFY=1: stage B replays every run and compares the batch plan with it.
bool RunVerifyEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_DISPATCH_RUN_VERIFY");
        return env && env[0] == '1';
    }();
    return on;
}

/// Read slots of cs 8f067540 that must be ring (or static mesh) bindings: 0-5 and the
/// flattened user data (9).
constexpr u32 RunReadSlots = 0x3f | 1u << 9;

enum RunRefusal : u32 {
    RefusedState,     ///< toggle bit, ring off, capture or profiler, DMA
    RefusedShape,     ///< dims, user data, buffers
    RefusedRegisters, ///< registers changed since the last packet
    RefusedPipeline,  ///< no batch pipeline
    RefusedReads,     ///< a read slot is not in the constant ring
    RefusedWrites,    ///< an output view is null or not the +0/+12/+20 layout
    NumRunRefusals,
};
enum RunReplay : u32 {
    ReplayToggle,
    ReplayPipeline,
    ReplayCapture,
    ReplayVerify,
    ReplayReads,
    ReplaySharps, ///< an output V# null, garbage or clamped on stage B
    ReplayMeta,
    ReplaySpans,
    ReplayManySpans, ///< more spans than tracked ranges (RunPlan::MaxTracked)
    ReplayTickMoved, ///< acquisitions could not be made at the batch's tick
    NumRunReplays,
};

/// The "Dispatch runs (5 s)" line, printed by stage A whenever GOW3_DISPATCH_RUNS=1. Stage A's
/// counters are reset per line; stage B's are written by stage B only and printed as differences.
struct RunStats {
    u64 packets = 0, packet_instances = 0, singles = 0, tasks = 0;
    u64 prunes = 0, pruned = 0, pending_max = 0; ///< pending_writes after run flushes
    std::array<u64, NumRunRefusals> refused{};
    std::array<u64, Rasterizer::NumRunCuts> cuts{};
    std::unordered_map<u32, u64> opcode_cuts; ///< opcode | SET_SH_REG register << 8
    u32 calls = 0;
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    /// Stage B: dispatches batched, dispatch calls, dispatches verified, verify mismatches
    /// (descriptors, push data, tracked ranges, written bytes), spans acquired in pieces, and
    /// window ticks a run's acquisitions changed (bookkeeping the views would not have changed).
    std::atomic<u64> batched{0}, batches{0}, verified{0}, bad_descriptors{0}, bad_push{0},
        bad_tracking{0}, bad_written{0}, chunked{0}, bad_bookkeeping{0}, reacquired{0};
    std::array<std::atomic<u64>, NumRunReplays> replays{};
    /// ReplaySpans by SkinningSpanRefusal (layout, overflow, overlap, window, too many), then
    /// spans in more than one buffer.
    std::array<std::atomic<u64>, 7> span_refusals{};
    std::array<u64, 5> task_cuts{}; ///< not a fence, fence slots full, fence too large
    std::array<u64, 4> join_cuts{}; ///< pipeline, queue, groups (dim_x), other registers
    /// Stage B's counters at the last line (10 counters, the replays, the span refusals).
    std::array<u64, 10 + NumRunReplays + 7> seen{};
};
RunStats g_run_stats;
/// GOW3_RUN_RELEASE_VERIFY: fences signalled in checked batches, batches whose writes or IRQs
/// differed from what was staged (any thread: the batches signal on the recorder or copy threads).
std::atomic<u64> g_release_verify_fences{0}, g_release_verify_mismatches{0};

void RunCount(std::atomic<u64>& counter, u64 add = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + add, std::memory_order_relaxed);
}

void PrintRunStats() {
    auto& s = g_run_stats;
    if ((++s.calls & 63) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - s.window < std::chrono::seconds(5)) {
        return;
    }
    s.window = now;
    constexpr u32 Replays = 10, Spans = Replays + NumRunReplays; // first replay, span refusal
    std::array<u64, Spans + 7> values = {
        s.batched.load(std::memory_order_relaxed),      s.batches.load(std::memory_order_relaxed),
        s.verified.load(std::memory_order_relaxed),     s.bad_descriptors.load(std::memory_order_relaxed),
        s.bad_push.load(std::memory_order_relaxed),     s.bad_tracking.load(std::memory_order_relaxed),
        s.bad_written.load(std::memory_order_relaxed),  s.chunked.load(std::memory_order_relaxed),
        s.bad_bookkeeping.load(std::memory_order_relaxed), s.reacquired.load(std::memory_order_relaxed)};
    for (u32 i = 0; i < NumRunReplays; ++i) {
        values[Replays + i] = s.replays[i].load(std::memory_order_relaxed);
    }
    for (u32 i = 0; i < 7; ++i) {
        values[Spans + i] = s.span_refusals[i].load(std::memory_order_relaxed);
    }
    std::array<u64, Spans + 7> d{};
    for (size_t i = 0; i < values.size(); ++i) {
        d[i] = values[i] - s.seen[i];
    }
    s.seen = values;
    const auto& r = s.refused;
    const auto& c = s.cuts;
    std::printf("Dispatch runs (5 s): %llu run packets of %.1f dispatches, %llu single packets; "
                "stage B batched %llu dispatches in %llu dispatch calls, replayed runs: toggle "
                "%llu, no pipeline %llu, capture %llu, verify %llu, reads %llu, V#s %llu, metadata "
                "%llu, spans %llu, over 32 spans %llu, tick moved %llu; not staged: state %llu, "
                "shape %llu, registers %llu, pipeline "
                "%llu, reads %llu, writes %llu; cuts: packet %llu, task %llu, drain %llu, indirect "
                "%llu, opcode %llu, queue end %llu, yield %llu, command %llu, submission %llu, full "
                "%llu, join %llu, refused %llu; staged fences %llu; run descriptor pools created "
                "so far %llu",
                (unsigned long long)s.packets,
                s.packets ? double(s.packet_instances) / s.packets : 0.0,
                (unsigned long long)s.singles, (unsigned long long)d[0], (unsigned long long)d[1],
                (unsigned long long)d[Replays + ReplayToggle],
                (unsigned long long)d[Replays + ReplayPipeline],
                (unsigned long long)d[Replays + ReplayCapture],
                (unsigned long long)d[Replays + ReplayVerify],
                (unsigned long long)d[Replays + ReplayReads],
                (unsigned long long)d[Replays + ReplaySharps],
                (unsigned long long)d[Replays + ReplayMeta],
                (unsigned long long)d[Replays + ReplaySpans],
                (unsigned long long)d[Replays + ReplayManySpans],
                (unsigned long long)d[Replays + ReplayTickMoved],
                (unsigned long long)r[RefusedState], (unsigned long long)r[RefusedShape],
                (unsigned long long)r[RefusedRegisters], (unsigned long long)r[RefusedPipeline],
                (unsigned long long)r[RefusedReads], (unsigned long long)r[RefusedWrites],
                (unsigned long long)c[Rasterizer::CutPacket], (unsigned long long)c[Rasterizer::CutTask],
                (unsigned long long)c[Rasterizer::CutDrain], (unsigned long long)c[Rasterizer::CutIndirect],
                (unsigned long long)c[Rasterizer::CutOpcode], (unsigned long long)c[Rasterizer::CutQueueEnd],
                (unsigned long long)c[Rasterizer::CutYield], (unsigned long long)c[Rasterizer::CutCommand],
                (unsigned long long)c[Rasterizer::CutSubmission], (unsigned long long)c[Rasterizer::CutFull],
                (unsigned long long)c[Rasterizer::CutJoin], (unsigned long long)c[Rasterizer::CutRefused],
                (unsigned long long)s.tasks,
                (unsigned long long)ComputePipeline::RunPoolsCreated());
    if (!s.opcode_cuts.empty()) {
        std::vector<std::pair<u64, u32>> top;
        for (const auto& [detail, n] : s.opcode_cuts) {
            top.emplace_back(n, detail);
        }
        std::ranges::sort(top, std::greater{});
        std::printf("; opcode cuts:");
        for (size_t i = 0; i < std::min<size_t>(top.size(), 6); ++i) {
            const u32 detail = top[i].second;
            std::printf(" %s", magic_enum::enum_name(AmdGpu::PM4ItOpcode(detail & 0xff)).data());
            if (detail >> 8) {
                std::printf("@%#x", detail >> 8);
            }
            std::printf("=%llu", (unsigned long long)top[i].first);
        }
        s.opcode_cuts.clear();
    }
    const auto& t = s.task_cuts;
    const auto& j = s.join_cuts;
    std::printf("; task cuts: other %llu, fence slots full %llu, fence too large %llu; join cuts: "
                "pipeline %llu, queue %llu, groups %llu, registers %llu; span refusals: layout "
                "%llu, overflow %llu, overlap %llu, 1 GiB line %llu, too many %llu, buffers %llu",
                (unsigned long long)t[0], (unsigned long long)t[1], (unsigned long long)t[2],
                (unsigned long long)j[0], (unsigned long long)j[1], (unsigned long long)j[2],
                (unsigned long long)j[3], (unsigned long long)d[Spans + 1],
                (unsigned long long)d[Spans + 2], (unsigned long long)d[Spans + 3],
                (unsigned long long)d[Spans + 4], (unsigned long long)d[Spans + 5],
                (unsigned long long)d[Spans + 6]);
    std::printf("; chunked spans %llu, window-tick changes %llu, re-acquisitions (tick moved) "
                "%llu; pending writes at most %llu, %llu prunes dropped %llu",
                (unsigned long long)d[7], (unsigned long long)d[8], (unsigned long long)d[9],
                (unsigned long long)s.pending_max, (unsigned long long)s.prunes,
                (unsigned long long)s.pruned);
    s.task_cuts = {};
    s.join_cuts = {};
    if (Rasterizer::RunReleaseVerify()) {
        std::printf("; release verify (totals): %llu fences, %llu batch mismatches",
                    (unsigned long long)g_release_verify_fences.load(std::memory_order_relaxed),
                    (unsigned long long)g_release_verify_mismatches.load(std::memory_order_relaxed));
    }
    if (RunVerifyEnabled()) {
        std::printf("; verify: %llu dispatches compared, mismatches: descriptors %llu, push data "
                    "%llu, tracked ranges %llu, written bytes %llu",
                    (unsigned long long)d[2], (unsigned long long)d[3], (unsigned long long)d[4],
                    (unsigned long long)d[5], (unsigned long long)d[6]);
    }
    std::printf("\n");
    std::fflush(stdout);
    s.packets = s.packet_instances = s.singles = s.tasks = 0;
    s.prunes = s.pruned = s.pending_max = 0;
    s.refused = {};
    s.cuts = {};
}
} // namespace

bool Rasterizer::RunReleaseVerify() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_RUN_RELEASE_VERIFY");
        return env && env[0] == '1';
    }();
    return on;
}

void Rasterizer::NoteReleaseVerify(u64 fences, bool mismatch) {
    g_release_verify_fences.fetch_add(fences, std::memory_order_relaxed);
    g_release_verify_mismatches.fetch_add(mismatch, std::memory_order_relaxed);
}

void Rasterizer::NoteDisplayStorageWrite(const Shader::Info& cs) {
    if (!upscaler->CompositeFrames()) {
        return;
    }
    for (const auto& image_desc : cs.images) {
        if (!image_desc.is_written) {
            continue;
        }
        const VAddr address = image_desc.GetSharp(cs).Address();
        if (address && FrameCapture::IsDisplayBuffer(address)) {
            upscaler->OnDisplayStorageWrite(address);
            return;
        }
    }
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;
    Gow3Stats::dispatches.fetch_add(1, std::memory_order_relaxed);
    DispatchStatsScope stats;
    // gow3: like draws, handed to the draw recording thread after the pipeline selection.
    bool pipelined = UseDrawPipe() && !Gow3Toggle::Disabled(Gow3Toggle::PipelinedDispatch);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (stats.on) {
        stats.hash = pipeline->GetStage(Shader::SwStage::Compute).pgm_hash;
        stats.dims = {CsRegs().dim_x, CsRegs().dim_y, CsRegs().dim_z};
    }
    // gow3 18 Oct: GOW3_DRAIN_CS=<hex hash>[,<hash>...]: these dispatches run here after a drain
    // (diagnostics). Uncharted 2's train flashed white with multithreaded rendering: later draws
    // were prepared from constants its copy shaders had yet to write. SyncSrtTables now waits for
    // queued writes to the buffers it reads; draining before the copy
    // shaders 2d215c3b/6a6422a9 and c92150ba as well (DrainCopyShaders, second word) cost 5.8%
    // of this thread. That diagnostic is opt-in: bit set means drain.
    static const std::vector<u64> drain_cs = [] {
        std::vector<u64> out;
        for (const char* env = std::getenv("GOW3_DRAIN_CS"); env && *env;) {
            char* end = nullptr;
            out.push_back(std::strtoull(env, &end, 16));
            env = end && *end == ',' ? end + 1 : nullptr;
        }
        return out;
    }();
    const u64 cs_hash = pipeline->GetStage(Shader::SwStage::Compute).pgm_hash;
    if (pipelined && (std::ranges::find(drain_cs, cs_hash) != drain_cs.end() ||
                      (Gow3Toggle::Disabled2(Gow3Toggle::DrainCopyShaders) &&
                       (cs_hash == 0x2d215c3b || cs_hash == 0x6a6422a9 || cs_hash == 0xc92150ba)))) {
        pipelined = false;
        DrainDrawPipe();
    }
    if (pipelined) {
        // Its buffer writes are bound and marked GPU-written on the recording thread: SRT walks
        // here that read them wait for it (SyncSrtTables).
        const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
        // gow3 22 Oct 2026 (Gow3Toggle::DispatchStageAMemos; U3 meat market, cs 8f067540 ~1,530
        // dispatches a frame): the V#s the specialization has just read from this Info's user
        // data, unchanged since (same thread, no walk in between), instead of a GetSharp per
        // buffer here and in CollectRingBindings (each a mapping lookup for the effect shaders:
        // GetSharp 6% of the GPU command thread, ~3.6% of it these two re-reads).
        const AmdGpu::Buffer* sharps = Gow3Toggle::Disabled2(Gow3Toggle::DispatchStageAMemos)
                                           ? nullptr
                                           : pipeline_cache.ComputeSharps(cs);
        ++(sharps ? g_stage_a_memos.sharps_reused : g_stage_a_memos.sharps_read);
        for (u32 index = 0; index < cs.buffers.size(); ++index) {
            const auto& desc = cs.buffers[index];
            if (!desc.is_written || desc.IsSpecial()) {
                continue;
            }
            const auto sharp = sharps ? sharps[index] : desc.GetSharp(cs);
            if (sharps && Shader::StageAVerify() && !(sharp == desc.GetSharp(cs))) {
                ++g_stage_a_memos.sharp_mismatches; // all 16 bytes (Buffer::operator== is a memcmp)
            }
            if (!sharp.base_address || !sharp.GetSize()) {
                continue;
            }
            if (pipe_write_ranges.size() < 256) {
                pipe_write_ranges.emplace_back(sharp.base_address, sharp.GetSize());
            } else {
                pipe_writes_overflow = true;
            }
        }
        if (DispatchRunsRequested() && StageDispatchRun(pipeline, sharps)) {
            return;
        }
        PostDraw(pipeline, nullptr, false, 0, &CsRegs(), nullptr, sharps);
        return;
    }
    DispatchRecord(pipeline);
}

bool Rasterizer::StageDispatchRun(const ComputePipeline* pipeline, const AmdGpu::Buffer* sharps) {
    auto& stats = g_run_stats;
    PrintRunStats();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (info.pgm_hash != 0x8f067540) {
        return false; // PostDraw hands the open run over first
    }
    auto& run = dispatch_run;
    const auto& cs = CsRegs();
    const auto refuse = [&](RunRefusal why) {
        ++stats.refused[why];
        CutDispatchRun(CutRefused);
        return false;
    };
    if (Gow3Toggle::Disabled2(Gow3Toggle::DispatchRuns) || !constant_ring ||
        Gow3Toggle::Disabled(Gow3Toggle::ConstantRing) || !pipe_synced || FrameCapture::Active() ||
        GpuProfiler::Get() || info.uses_dma) {
        return refuse(RefusedState);
    }
    if (cs.dim_x == 0 || cs.dim_y != 1 || cs.dim_z != 1 || info.buffers.size() != 10 ||
        info.user_data.size() != cs.user_data.size() || info.flattened_ud_buf.empty() ||
        info.flattened_ud_buf.size() > RunMaxFlat) {
        return refuse(RefusedShape);
    }
    // Run packets carry no register blocks: a dispatch after a register change goes out with them
    // as before, and a run starts with the next one.
    const auto& dirty = liverpool->pipe_dirty;
    if (dirty.reset || std::ranges::any_of(dirty.blocks, [](u64 bits) { return bits != 0; })) {
        return refuse(RefusedRegisters);
    }
    if (!pipeline->EnsureRunPipeline()) {
        return refuse(RefusedPipeline);
    }
    if (run.count &&
        (run.pipeline != pipeline || run.queue != &cs ||
         std::memcmp(&run.cs, &cs, offsetof(AmdGpu::ComputeProgram, user_data)) != 0)) {
        FlushDispatchRun(CutJoin, run.pipeline != pipeline ? 0
                                  : run.queue != &cs       ? 1
                                  : run.cs.dim_x != cs.dim_x ? 2
                                                           : 3);
    }
    if (!run.instances) {
        run.instances = std::make_unique<RunInstance[]>(ComputePipeline::RunCapacity + 1);
    }
    // What PostDraw serializes for a dispatch, staged: its constant ring copies are made now and
    // its written buffers pending until the run's packet is committed (uncommitted_indices).
    const u64 generation = run.generation;
    const u32 slot = run.count;
    {
        auto& staged = run.instances[slot];
        staged.rings.clear();
        staged.num_tasks = 0;
        noting_packet = true;
        CollectRingBindings(info, nullptr, staged.rings, sharps);
        noting_packet = false;
        constant_ring->EndPacket();
        staged.ring_end = constant_ring->Position();
        std::memcpy(staged.ud.data(), info.user_data.data(), info.user_data.size_bytes());
        std::memcpy(staged.flat.data(), info.flattened_ud_buf.data(),
                    info.flattened_ud_buf.size() * sizeof(u32));
    }
    // A drain while staging (a fault on this thread) handed the run over: this one starts anew.
    u32 index = slot;
    if (run.generation != generation) {
        if (slot != 0) {
            run.instances[0] = run.instances[slot];
        }
        index = 0;
    }
    if (index == 0) {
        run.pipeline = pipeline;
        run.queue = &cs;
        std::memcpy(&run.cs, &cs, sizeof(cs)); // the raw image the join compares
        run.cb_extent = liverpool->last_cb_extent;
        run.db_extent = liverpool->last_db_extent;
        run.pgm_base = info.pgm_base;
        run.ud_size = static_cast<u32>(info.user_data.size());
        run.flat_size = static_cast<u32>(info.flattened_ud_buf.size());
    }
    const auto& staged = run.instances[index];
    u32 reads = 0;
    for (const auto& ring : staged.rings) {
        reads |= ring.index < 32 ? 1u << ring.index : 0;
    }
    std::array<SkinningWriteView, 3> views{};
    for (u32 v = 0; v < 3; ++v) {
        const auto sharp = sharps ? sharps[6 + v] : info.buffers[6 + v].GetSharp(info);
        views[v] = {sharp.base_address, sharp.GetSize()};
    }
    // Stage B checks the V#s again (garbage, mapping, metadata) and replays what fails.
    const bool writes_ok = SkinningWriteLayout(views) != 0;
    if (reads != RunReadSlots || !writes_ok) {
        ++stats.refused[reads != RunReadSlots ? RefusedReads : RefusedWrites];
        if (index != 0) {
            FlushDispatchRun(CutRefused); // this one's writes stay pending for its own packet
        }
        EmitRunPacket(&staged, 1);
        ResolveRunWrites(static_cast<u32>(uncommitted_indices.size()));
        ++stats.singles;
        PrintPipeStats();
        return true;
    }
    run.count = index + 1;
    run.uncommitted = static_cast<u32>(uncommitted_indices.size());
    if (run.count == ComputePipeline::RunCapacity) {
        FlushDispatchRun(CutFull);
    }
    return true;
}

void Rasterizer::FlushDispatchRun(u32 reason, u32 detail) {
    auto& run = dispatch_run;
    const u32 count = run.count;
    run.count = 0;
    ++run.generation;
    EmitRunPacket(run.instances.get(), count);
    ResolveRunWrites(run.uncommitted);
    run.uncommitted = 0;
    // The staged tasks' labels (the newest pending ones) are written once the packet has run.
    const u64 end = draw_pipe->Head();
    for (auto it = pending_labels.rbegin(); run.labels && it != pending_labels.rend(); ++it) {
        if (it->position == PendingUncommitted) {
            it->position = end;
            --run.labels;
        }
    }
    run.labels = 0;
    auto& stats = g_run_stats;
    for (u32 k = 0; k < count; ++k) {
        stats.tasks += run.instances[k].num_tasks;
    }
    ++stats.cuts[std::min<u32>(reason, NumRunCuts - 1)];
    if (reason == CutOpcode) {
        ++stats.opcode_cuts[detail];
    } else if (reason == CutTask) {
        ++stats.task_cuts[std::min<u32>(detail, stats.task_cuts.size() - 1)];
    } else if (reason == CutJoin) {
        ++stats.join_cuts[std::min<u32>(detail, stats.join_cuts.size() - 1)];
    }
    if (count > 1) {
        ++stats.packets;
        stats.packet_instances += count;
    } else {
        ++stats.singles;
    }
    PrintPipeStats();
}

void Rasterizer::ResolveRunWrites(u32 head) {
    // 24 Oct 2026 review: PendingWriteOverlaps prunes only with no uncommitted entry, which an
    // open run always has: without this the list grew and NotePendingGpuWrite's merge scan was
    // 2.6% of stage A.
    const auto prune = [&] {
        auto& stats = g_run_stats;
        stats.pending_max = std::max<u64>(stats.pending_max, pending_writes.size());
        if (uncommitted_indices.empty() && pending_writes.size() > 32) {
            ++stats.prunes;
            stats.pruned += PrunePendingWrites();
        }
    };
    // As PostDraw after its commit: the writes noted for the packet are done past its end.
    if (uncommitted_indices.empty()) {
        prune();
        return;
    }
    const u64 end = draw_pipe->Head();
    head = std::min<u32>(head, static_cast<u32>(uncommitted_indices.size()));
    for (u32 i = 0; i < head; ++i) {
        auto& write = pending_writes[uncommitted_indices[i]];
        if (write.position == PendingUncommitted) {
            write.position = end;
        }
    }
    // An instance still being staged: its writes (also those merged into the packet's ranges)
    // stay pending until its own packet.
    for (u32 i = head; i < uncommitted_indices.size(); ++i) {
        pending_writes[uncommitted_indices[i]].position = PendingUncommitted;
    }
    uncommitted_indices.erase(uncommitted_indices.begin(), uncommitted_indices.begin() + head);
    prune();
}

void Rasterizer::EmitRunPacket(const RunInstance* instances, u32 count) {
    const auto& run = dispatch_run;
    const auto& info = run.pipeline->GetStage(Shader::SwStage::Compute);
    const u32 words = AlignPacket((run.ud_size + run.flat_size) * sizeof(u32));
    const auto instance_bytes = [&](const RunInstance& staged) {
        return static_cast<u32>(staged.rings.size() * sizeof(RingBinding)) + words;
    };
    const auto task_bytes = [](const RunInstance& staged) {
        u32 bytes = 0;
        for (u32 t = 0; t < staged.num_tasks; ++t) {
            bytes += sizeof(TaskPacket) + AlignPacket(staged.tasks[t].size);
        }
        return bytes;
    };
    // The tasks after each instance, laid out as Task packets are.
    const auto write_tasks = [](u8*& at, const RunInstance& staged) {
        for (u32 t = 0; t < staged.num_tasks; ++t) {
            const auto& task = staged.tasks[t];
            *reinterpret_cast<TaskPacket*>(at) = {PacketKind::Task, task.size, task.task};
            std::memcpy(at + sizeof(TaskPacket), task.data.data(), task.size);
            at += sizeof(TaskPacket) + AlignPacket(task.size);
        }
    };
    u32 size = sizeof(DrawPacket) + sizeof(PacketStage) + instance_bytes(instances[0]) +
               AlignPacket(sizeof(AmdGpu::ComputeProgram));
    if (count > 1) {
        size += sizeof(RunPacketHeader) + task_bytes(instances[0]);
        for (u32 k = 1; k < count; ++k) {
            size += sizeof(RunPacketInstance) + instance_bytes(instances[k]) +
                    task_bytes(instances[k]);
        }
    }
    u8* out = draw_pipe->Begin(size);
    u8* const start = out;
    auto& packet = *reinterpret_cast<DrawPacket*>(out);
    packet.kind = count > 1 ? PacketKind::DispatchRun : PacketKind::Dispatch;
    packet.pipeline = run.pipeline;
    packet.prepared = nullptr;
    packet.index_offset = 0;
    packet.is_indexed = false;
    packet.regs_reset = false;
    packet.num_blocks = 0;
    packet.num_stages = 1;
    packet.verify = 0;
    packet.cb_extent = run.cb_extent;
    packet.db_extent = run.db_extent;
    packet.ring_end = instances[count - 1].ring_end;
    out += sizeof(DrawPacket);
    const auto write_words = [&](const RunInstance& staged) {
        std::memcpy(out, staged.rings.data(), staged.rings.size() * sizeof(RingBinding));
        out += staged.rings.size() * sizeof(RingBinding);
        std::memcpy(out, staged.ud.data(), run.ud_size * sizeof(u32));
        std::memcpy(out + run.ud_size * sizeof(u32), staged.flat.data(), run.flat_size * sizeof(u32));
        out += words;
    };
    *reinterpret_cast<PacketStage*>(out) = {&info,        run.pgm_base,
                                            run.ud_size,  run.flat_size,
                                            static_cast<u32>(instances[0].rings.size()), 0};
    out += sizeof(PacketStage);
    write_words(instances[0]);
    // The register image as PostDraw copies it (memcpy: the bitfields' padding words too).
    std::memcpy(out, &run.cs, sizeof(AmdGpu::ComputeProgram));
    std::memcpy(out + offsetof(AmdGpu::ComputeProgram, user_data), instances[0].ud.data(),
                sizeof(AmdGpu::UserData));
    out += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    if (count > 1) {
        *reinterpret_cast<RunPacketHeader*>(out) = {count, instances[0].num_tasks,
                                                    instances[0].ring_end};
        out += sizeof(RunPacketHeader);
        write_tasks(out, instances[0]);
        for (u32 k = 1; k < count; ++k) {
            *reinterpret_cast<RunPacketInstance*>(out) = {instances[k].ring_end,
                                                          static_cast<u32>(instances[k].rings.size()),
                                                          instances[k].num_tasks};
            out += sizeof(RunPacketInstance);
            write_words(instances[k]);
            write_tasks(out, instances[k]);
        }
    }
    ASSERT(static_cast<u32>(out - start) == size);
    draw_pipe->Commit(static_cast<u32>(out - start));
    if (count == 1) {
        // A dispatch alone: its tasks follow as the Task packets RunInOrder would have made.
        for (u32 t = 0; t < instances[0].num_tasks; ++t) {
            const auto& task = instances[0].tasks[t];
            const u32 total = sizeof(TaskPacket) + AlignPacket(task.size);
            u8* task_out = draw_pipe->Begin(total);
            *reinterpret_cast<TaskPacket*>(task_out) = {PacketKind::Task, task.size, task.task};
            std::memcpy(task_out + sizeof(TaskPacket), task.data.data(), task.size);
            draw_pipe->Commit(total);
        }
    }
}

bool Rasterizer::AppendSkinningBatch(const ComputePipeline* pipeline) {
    static const bool requested = [] {
        const char* env = std::getenv("GOW3_DISPATCH_FUSION");
        return env && env[0] == '1';
    }();
    // 24 Oct 2026: never together with the dispatch runs (DispatchRunsRequested logs it).
    if (!requested || DispatchRunsRequested() || Gow3Toggle::Disabled2(Gow3Toggle::DispatchFusion) ||
        !UseDrawPipe() || !DrawPipe::OnStageB() || GpuProfiler::Get() || FrameCapture::Active() ||
        FullBarriersEnabled() || !cloth_peeks.empty()) return false;
    const auto& cs = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (info.pgm_hash != 0x8f067540 || cs.dim_y != 1 || cs.dim_z != 1 || !cs.dim_x ||
        set_writes.size() != info.buffers.size() || !pipeline->EnsureBatchPipeline()) return false;
    for (u32 i = 0; i < set_writes.size(); ++i) {
        const auto& write = set_writes[i];
        if (write.dstBinding != i || write.descriptorCount != 1 || !write.pBufferInfo ||
            write.descriptorType != vk::DescriptorType::eStorageBuffer) return false;
    }
    if (skinning_batch.count &&
        (skinning_batch.pipeline != pipeline || skinning_batch.dim_x != cs.dim_x)) {
        scheduler.FlushDeferredRecord();
    }
    // Allocate BEFORE installing/appending to the deferred batch. A stream wrap may submit:
    // earlier batches must be emitted at their original tick, not after the wrap's Finish().
    auto& stream = buffer_cache.GetStreamBuffer();
    const u64 push_offset = stream.Copy(&push_data, sizeof(push_data), instance.StorageMinAlignment());
    auto& batch = skinning_batch;
    if (!batch.count) {
        batch.pipeline = pipeline;
        batch.dim_x = cs.dim_x;
        batch.bindings = set_writes.size() + 1;
        batch.first_push = push_data;
        scheduler.DeferRecord([this] { FlushSkinningBatch(); });
    }
    for (u32 i = 0; i < set_writes.size(); ++i)
        batch.buffers[i][batch.count] = *set_writes[i].pBufferInfo;
    batch.buffers[set_writes.size()][batch.count] = {stream.Handle(), push_offset, sizeof(push_data)};
    ++batch.count;
    DebugState.IncDispatch(); // logical PS4 dispatches; physical Vulkan count is reported separately
    ResetBindings(true);    // retain exact resource hazards for EVERY original dispatch
    if (batch.count == ComputePipeline::BatchCapacity) scheduler.FlushDeferredRecord();
    return true;
}

void Rasterizer::FlushSkinningBatch() {
    auto& batch = skinning_batch;
    if (!batch.count) return;
    const auto* pipeline = batch.pipeline;
    const u32 count = batch.count, dim_x = batch.dim_x;
    batch.count = 0; // emitted commands can themselves flush/submit without re-entering the batch
    if (count == 1) {
        // An ordering boundary after one instance: ordinary shader/commands, not a padded set.
        std::vector<vk::WriteDescriptorSet> writes(batch.bindings - 1);
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i, .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &batch.buffers[i][0]};
        }
        pipeline->BindResources(writes, batch.first_push);
        const auto handle = pipeline->Handle();
        scheduler.Record([=](vk::CommandBuffer cmd) {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
            cmd.dispatch(dim_x, 1, 1);
        });
        ++fused_singles;
        if (fused_singles <= 8 || fused_singles % 50000 == 0) {
            const auto caller = scheduler.DeferredRecordBoundary();
            std::printf("Dispatch fusion singleton %llu: boundary %s:%u (%s)\n",
                        fused_singles, caller.file_name(), caller.line(), caller.function_name());
        }
        scheduler.KickRecording();
        return;
    }
    const vk::DescriptorSet set = pipeline->BatchDescriptorSet();
    std::vector<vk::WriteDescriptorSet> writes(batch.bindings);
    for (u32 i = 0; i < batch.bindings; ++i) {
        // All declared descriptors are initialized, even in a partial batch: no partially-bound
        // feature or nonuniform indexing required. Workgroup Y is uniform within each group.
        for (u32 j = count; j < ComputePipeline::BatchCapacity; ++j)
            batch.buffers[i][j] = batch.buffers[i][count - 1];
        writes[i] = {.dstSet = set, .dstBinding = i,
                     .descriptorCount = ComputePipeline::BatchCapacity,
                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                     .pBufferInfo = batch.buffers[i].data()};
    }
    instance.GetDevice().updateDescriptorSets(writes, {});
    const auto handle = pipeline->BatchHandle();
    const auto layout = pipeline->BatchLayout();
    scheduler.Checkpoint("skinning batch", count);
    scheduler.Record([=](vk::CommandBuffer cmd) {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, set, {});
        cmd.dispatch(dim_x, count, 1);
    });
    scheduler.KickRecording();
    fused_dispatches += count;
    ++fused_batches;
    if ((fused_batches & 255) == 0) {
        static auto last = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
            last = now;
            std::printf("Dispatch fusion (5 s): %llu logical -> %llu Vulkan dispatches, %llu singles, "
                        "%.2f instances/batch, %llu dispatches removed\n",
                        fused_dispatches, fused_batches, fused_singles,
                        double(fused_dispatches) / fused_batches, fused_dispatches - fused_batches);
            fused_dispatches = fused_batches = fused_singles = 0;
        }
    }
}

void Rasterizer::DispatchRecord(const ComputePipeline* pipeline) {
    if (DispatchTraceTarget()) [[unlikely]] {
        DispatchTraceEnter(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);
    }
    if (BarrierStatsEnabled()) {
        g_barrier_stats_shader = pipeline->GetStage(Shader::SwStage::Compute).pgm_hash;
    }
    if (SkippedShader(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash)) {
        return;
    }
    FrameCapture::Poll();
    gbuffer_draw = false;

    scheduler.PopPendingOperations();

    const auto& cs_program = CsRegs();

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    // After the resource binding: its transfers (image uploads, downloads) mark themselves.
    const auto mark = [&] {
        if (auto* profiler = GpuProfiler::Get()) {
            profiler->Mark(cs.pgm_hash ^ 0xD15Aull, [&] {
                return fmt::format("dispatch cs {:016x} ({}x{}x{} groups)", cs.pgm_hash,
                                   cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
            });
        }
    };
    if (upscaler->Enabled()) {
        upscaler->OnDispatch(cs.pgm_hash);
    }
    if (ExecuteShaderHLE(cs, Regs(), cs_program, *this)) {
        return;
    }
    NoteDisplayStorageWrite(cs);
    // gow3 diagnostics (GOW3_DUMP_CS_CONST=<hex shader hash>): the first 32 dwords (as floats) of
    // every buffer the dispatch reads that is up to 4 KiB, with a timestamp, 2000 lines at most.
    static const u64 dump_cs = [] {
        const char* env = std::getenv("GOW3_DUMP_CS_CONST");
        return env ? std::strtoull(env, nullptr, 16) : 0ULL;
    }();
    if (dump_cs && cs.pgm_hash == dump_cs) {
        static std::atomic<int> lines{0};
        static const auto t0 = std::chrono::steady_clock::now();
        for (u32 i = 0; i < cs.buffers.size() && lines.load() < 2000; ++i) {
            const auto& desc = cs.buffers[i];
            if (desc.IsSpecial() || desc.is_written) {
                continue;
            }
            const auto sharp = desc.GetSharp(cs);
            if (!sharp.base_address || sharp.GetSize() == 0 || sharp.GetSize() > 4096 ||
                !IsMapped(sharp.base_address, std::min<u64>(sharp.GetSize(), 128))) {
                continue;
            }
            float values[32]{};
            const u64 bytes = std::min<u64>(sharp.GetSize(), sizeof(values));
            memory->CopySparseMemory(sharp.base_address, reinterpret_cast<u8*>(values), bytes);
            std::string text;
            for (u64 k = 0; k < bytes / 4; ++k) {
                text += fmt::format(" {:.5g}", values[k]);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("CS const %llx t=%.1f buf %u @%#llx size %u:%s\n",
                        static_cast<unsigned long long>(cs.pgm_hash), ms, i,
                        static_cast<unsigned long long>(sharp.base_address), unsigned(sharp.GetSize()),
                        text.c_str());
            ++lines;
        }
        std::fflush(stdout);
    }

    // gow3 diagnostics (GOW3_TRACKER_CAP=<n>): a barrier before a dispatch once the barrier
    // tracker holds n ranges (it is emptied by barriers only). Off: 1024 cost Uncharted 3's
    // crowded street at 4K 48 -> 39 FPS (draws keep it above the cap, a barrier per dispatch).
    {
        static const size_t cap = [] {
            const char* env = std::getenv("GOW3_TRACKER_CAP");
            return env ? size_t(std::strtoull(env, nullptr, 10)) : size_t(0);
        }();
        if (cap && runtime.BarrierTrackerSize() > cap) {
            runtime.FlushBarriers();
        }
    }
    if (!BindResources(pipeline)) {
        return;
    }

    if (DispatchTraceTarget()) [[unlikely]] {
        DispatchTraceDispatch(cs, cs_program, needs_barrier);
    }
    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    mark();
    // Any resource transfer/barrier recorded by BindResources has already flushed earlier
    // batches through Scheduler::Record. Only compatible, hazard-free consecutive work fuses.
    if (AppendSkinningBatch(pipeline)) return;
    // gow3 20 Oct 2026 (GOW3_SKIP_COMPUTE_REBIND=0: always bind): the compute pipeline is not
    // bound again when the previous dispatch bound the same one and nothing but this dispatch's
    // push constants and descriptors was recorded since, in the same command buffer (the
    // scheduler's record serial: every Record() and CommandBuffer() call, and every new command
    // buffer, advance it; any other compute bind goes through one of them). Only while Vulkan
    // commands are recorded on this thread (no recording thread, no draw pipeline). Uncharted 3
    // dispatches ~1,275 times a frame, mostly the same few shaders back to back; on the
    // laptop (Ryzen 7840HS, 47 FPS, the GPU command thread CPU-bound) the driver's
    // vkCmdBindPipeline was 2.4% of that thread and vkCmdDispatch 4.3% (together 0.6% on the
    // Radeon 780M).
    static const bool skip_rebind = [] {
        const char* env = std::getenv("GOW3_SKIP_COMPUTE_REBIND");
        return !(env && env[0] == '0');
    }();
    if (run_verify_index != ~0u) [[unlikely]] {
        VerifyRunInstance(); // GOW3_DISPATCH_RUN_VERIFY: the run's batch plan against these
    }
    const u64 serial_before_bind = scheduler.RecordSerial();
    pipeline->BindResources(set_writes, push_data);
    const u64 serial_after_bind = scheduler.RecordSerial();

    const vk::Pipeline handle = pipeline->Handle();
    u32 dim_x = cs_program.dim_x, dim_y = cs_program.dim_y;
    const u32 dim_z = cs_program.dim_z;
    // gow3: movie frames at the display size: the whole frame converted (gow3_movie.h).
    Gow3Movie::CoverDisplay(cs.pgm_hash, cs_program.num_thread_x.full, cs_program.num_thread_y.full,
                            dim_x, dim_y);
    scheduler.Checkpoint("dispatch",cs.pgm_hash);
    if (FrameCapture::Active()) {
        FrameCapture::Dispatch(cs.pgm_hash, dim_x, dim_y, dim_z);
    }
    const u64 cloth_dispatch = cloth_peeks.empty() ? 0 : ++g_cloth_dispatches;
    for (const auto& peek : cloth_peeks) {
        buffer_cache.PeekGpuBuffer(peek.buffer, peek.buffer_offset, peek.address, peek.size, peek.shader,
                                   peek.slot, cloth_dispatch, 0);
    }
    const bool full_barrier = FullBarriersEnabled();
    const bool bound = skip_rebind && handle == bound_compute_pipeline &&
                       serial_before_bind == bound_compute_serial &&
                       scheduler.RecordSerial() == serial_after_bind &&
                       !scheduler.IsRecordingThreaded() && !UseDrawPipe();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        if (full_barrier) {
            RecordFullBarrier(cmdbuf);
        }
        if (!bound) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        }
        cmdbuf.dispatch(dim_x, dim_y, dim_z);
        if (full_barrier) {
            RecordFullBarrier(cmdbuf);
        }
    });
    bound_compute_pipeline = handle;
    bound_compute_serial = scheduler.RecordSerial();
    for (const auto& peek : cloth_peeks) {
        if (peek.written) {
            buffer_cache.PeekGpuBuffer(peek.buffer, peek.buffer_offset, peek.address, peek.size, peek.shader,
                                       peek.slot, cloth_dispatch, 1);
        }
    }
    cloth_peeks.clear();
    DebugState.IncDispatch();

    ResetBindings(true);
    if (ClothTraceEnabled() && (cs.pgm_hash == 0x1c957b01 || cs.pgm_hash == 0x809864d3)) {
        g_cloth_epoch = runtime.AccessEpoch();
        g_cloth_epoch_shader = cs.pgm_hash;
    }
    scheduler.KickRecording();
}

void Rasterizer::DispatchRunRecord(const ComputePipeline* pipeline, const u8* tail,
                                   const u8* end) {
    constexpr u32 Capacity = ComputePipeline::RunCapacity;
    struct Instance {
        const RingBinding* rings;
        u32 num_ring;
        const u32* ud;
        const u32* flat;
        u64 ring_end;
        const u8* tasks; ///< Task packets run after the dispatch
        u32 num_tasks;
    };
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    const auto first = Shader::Info::ud_snapshots[0];
    const auto& header = *reinterpret_cast<const RunPacketHeader*>(tail);
    const u32 count = header.count;
    ASSERT(count >= 2 && count <= Capacity && first.info == &info && num_ring_stages == 1);
    const auto skip_tasks = [](const u8*& at, u32 num_tasks) {
        for (u32 t = 0; t < num_tasks; ++t) {
            at += sizeof(TaskPacket) + AlignPacket(reinterpret_cast<const TaskPacket*>(at)->size);
        }
    };
    std::array<Instance, Capacity> runs;
    const u8* at = tail + sizeof(RunPacketHeader);
    runs[0] = {ring_stages[0].bindings, ring_stages[0].count, first.user_data, first.flat,
               header.first_ring_end, at, header.num_tasks};
    skip_tasks(at, header.num_tasks);
    const u32 words = AlignPacket((first.user_data_size + first.flat_size) * sizeof(u32));
    for (u32 k = 1; k < count; ++k) {
        const auto& record = *reinterpret_cast<const RunPacketInstance*>(at);
        at += sizeof(RunPacketInstance);
        const auto* rings = reinterpret_cast<const RingBinding*>(at);
        at += record.num_ring * sizeof(RingBinding);
        const auto* ud = reinterpret_cast<const u32*>(at);
        at += words;
        runs[k] = {rings, record.num_ring, ud, ud + first.user_data_size, record.ring_end, at,
                   record.num_tasks};
        skip_tasks(at, record.num_tasks);
    }
    ASSERT(at == end);
    // gow3 24 Oct 2026 (Gow3Toggle::RunReleaseBatch): after a batch, its compute releases (~20 a
    // run, ~666k per 5 s in U3's street) are noted one by one and signalled together: one
    // SignalAfterHostCopies whose closure writes every label and raises every IRQ in order
    // (SignalAfterHostCopies + its Flush were ~4.9% of stage B). One download pass before the
    // first: between releases nothing queues an image download or a tiny readback (the
    // per-label part only notes buffer cache state), so later passes found nothing. Any other
    // task sends the batch first. Not on the replay path.
    const auto& hooks = release_hooks;
    const auto flush_releases = [&] {
        if (!release_batch.empty()) {
            hooks.signal(*this, release_batch, release_batch_hash);
            release_batch_hash = 0;
        }
    };
    // An instance's in-order tasks, as RunDrawPacket runs Task packets (no stage state).
    const auto run_tasks = [&](const Instance& run, bool batch_releases) {
        const auto* cs = stage_cs;
        const u32 snapshots = Shader::Info::num_ud_snapshots;
        stage_cs = nullptr;
        Shader::Info::num_ud_snapshots = 0;
        num_ring_stages = 0;
        const u8* task = run.tasks;
        for (u32 t = 0; t < run.num_tasks; ++t) {
            const auto& packet = *reinterpret_cast<const TaskPacket*>(task);
            const u8* data = task + sizeof(TaskPacket);
            task += sizeof(TaskPacket) + AlignPacket(packet.size);
            if (batch_releases && packet.task == hooks.task && packet.size == hooks.size) {
                if (release_batch.empty()) {
                    ProcessDownloadImages();
                }
                if (hooks.stage(*this, data, release_batch_hash)) {
                    release_batch.insert(release_batch.end(), data, data + packet.size);
                    continue;
                }
            }
            flush_releases();
            packet.task(*this, data);
        }
        stage_cs = cs;
        Shader::Info::num_ud_snapshots = snapshots;
        num_ring_stages = 1;
    };
    // Instance k's user data and constant ring bindings, as RunDrawPacket installs a packet's.
    const auto install = [&](const Instance& run) {
        Shader::Info::ud_snapshots[0] = {&info, run.ud, first.user_data_size, run.flat,
                                         first.flat_size, first.pgm_base};
        Shader::Info::num_ud_snapshots = 1;
        ring_stages[0] = {&info, run.rings, run.num_ring};
    };
    auto& stats = g_run_stats;
    FrameCapture::Poll();
    const bool verify = RunVerifyEnabled();
    u32 replay = NumRunReplays;
    if (Gow3Toggle::Disabled2(Gow3Toggle::DispatchRuns)) {
        replay = ReplayToggle;
    } else if (!pipeline->HasRunPipeline()) {
        replay = ReplayPipeline;
    } else if (FrameCapture::Active() || GpuProfiler::Get()) {
        replay = ReplayCapture;
    }
    if (!run_plan) {
        run_plan = std::make_unique<RunPlan>();
    }
    auto& plan = *run_plan;
    const u64 alignment = instance.StorageMinAlignment();
    bool planned = false;
    if (replay == NumRunReplays) {
        if (!verify) {
            // DispatchRecord's and BindResources' work before the bindings, once for the run.
            scheduler.PopPendingOperations();
            gbuffer_draw = false;
            if (upscaler->Enabled()) {
                upscaler->OnDispatch(info.pgm_hash);
            }
            buffer_cache.PollReadbacks();
            sampler_lod_bias = 0.0f;
            sampler_graphics = false;
            sampler_forced_aniso = 0;
        }
        // Per instance what BindResources/BindBuffers would bind: reads from the constant ring
        // (or static mesh snapshots), push data, and the V#s of the three output views.
        const auto& regs = Regs();
        for (u32 k = 0; k < count && replay == NumRunReplays; ++k) {
            install(runs[k]);
            u32 reads = 0;
            for (u32 j = 0; j < runs[k].num_ring; ++j) {
                const auto& ring = runs[k].rings[j];
                if (ring.index >= RunPlan::Bindings - 1 || (ring.index >= 6 && ring.index <= 8)) {
                    reads = 0;
                    break;
                }
                const bool persistent = (ring.offset & StaticMeshOffsetFlag) != 0;
                plan.infos[ring.index][k] = vk::DescriptorBufferInfo{
                    persistent ? static_mesh_buffer->Handle() : constant_ring->Handle(),
                    ring.offset & ~StaticMeshOffsetFlag, ring.size};
                reads |= 1u << ring.index;
            }
            if (reads != RunReadSlots) {
                replay = ReplayReads;
                break;
            }
            auto& push = plan.push[k];
            push = MakeUserData(regs);
            Shader::Backend::Bindings binding{};
            info.PushUd(binding, push);
            for (u32 slot = 0; slot < 6; ++slot) {
                push.AddOffset(slot, 0);
            }
            for (u32 v = 0; v < 3 && replay == NumRunReplays; ++v) {
                const auto& desc = info.buffers[6 + v];
                const auto sharp = desc.GetSharp(info);
                const GarbageSharp gs = ClassifyGarbageSharp(memory, sharp, info.pgm_hash);
                if (gs.unbound || gs.garbage || gs.mapped_size != sharp.GetSize()) {
                    replay = ReplaySharps;
                } else if (texture_cache.IsMeta(sharp.base_address)) {
                    replay = ReplayMeta; // IsComputeMetaClear may skip or clear: as before
                } else {
                    plan.writes[k].views[v] = {sharp.base_address, sharp.GetSize()};
                    plan.writes[k].tracked[v] = TrackedBufferSize(desc, sharp, sharp.GetSize());
                }
            }
        }
        if (replay == NumRunReplays) {
            const auto refusal = BuildSkinningWriteSpans(plan.writes.data(), count,
                                                         plan.spans.data(), RunPlan::MaxSpans,
                                                         plan.num_spans, plan.span_of.data());
            if (refusal != SkinningSpanRefusal::None) {
                replay = ReplaySpans;
                RunCount(stats.span_refusals[static_cast<u32>(refusal)]);
            } else if (plan.num_spans > RunPlan::MaxTracked) {
                // More tracked ranges than bound_buffers holds: before any acquisition, the old
                // path (one hull over all spans could cover unrelated buffers of the arena).
                replay = ReplayManySpans;
            }
        }
        planned = replay == NumRunReplays;
    }
    // The batched path's acquisitions (verify mode makes none: the frame gets exactly the old
    // path, the plan is checked against it afterwards). A span: SynchronizeMemory, GpuRangesAdd
    // and the readback rewrite notes over exactly the views' union, in pieces of at most 64 KiB
    // (SplitSkinningWrite: a larger write would stamp whole 1 MiB windows the views did not); a
    // span with small or large views view by view, as before.
    // Returns false when a span's pieces or views were not one buffer at linear offsets.
    const auto acquire = [&](bool first_time) {
        bool one_buffer = true;
        for (u32 s = 0; s < plan.num_spans; ++s) {
            const auto& span = plan.spans[s];
            if (span.per_view) {
                continue;
            }
            const u64 first_window = span.address >> 20;
            const u64 last_window = (span.address + span.size - 1) >> 20;
            boost::container::small_vector<u64, 8> window_ticks;
            for (u64 window = first_window; window <= last_window; ++window) {
                window_ticks.push_back(buffer_cache.GpuWriteWindowTick(window));
            }
            const auto [pieces, bytes] = SplitSkinningWrite(span.size);
            for (u32 piece = 0; piece < pieces; ++piece) {
                const u64 at = u64(piece) * bytes;
                const auto got = buffer_cache.ObtainBuffer(
                    span.address + at, static_cast<u32>(std::min(bytes, span.size - at)), true,
                    false);
                if (piece == 0) {
                    plan.obtained[s] = got;
                } else {
                    one_buffer &= got.first == plan.obtained[s].first &&
                                  got.second == plan.obtained[s].second + at;
                }
            }
            if (pieces > 1 && first_time) {
                RunCount(stats.chunked);
            }
            // Views of 64 KiB or less stamp page ticks only: the pieces must not move a window's.
            for (u64 window = first_window; window <= last_window; ++window) {
                if (buffer_cache.GpuWriteWindowTick(window) != window_ticks[window - first_window]) {
                    RunCount(stats.bad_bookkeeping);
                    break;
                }
            }
        }
        for (u32 k = 0; k < count; ++k) {
            const u32 s = plan.span_of[k];
            const auto& span = plan.spans[s];
            for (u32 v = 0; v < 3; ++v) {
                const auto& view = plan.writes[k].views[v];
                auto& got = plan.view_obtained[k][v];
                if (span.per_view) {
                    got = buffer_cache.ObtainBuffer(view.address, static_cast<u32>(view.size), true,
                                                    info.buffers[6 + v].is_formatted);
                    if (view.address == span.address) {
                        plan.obtained[s] = got; // the span's tracking starts there
                    }
                } else {
                    got = {plan.obtained[s].first,
                           plan.obtained[s].second + (view.address - span.address)};
                }
            }
        }
        for (u32 k = 0; k < count; ++k) {
            for (u32 v = 0; v < 3; ++v) {
                one_buffer &= plan.view_obtained[k][v].first == plan.obtained[plan.span_of[k]].first;
            }
        }
        return one_buffer;
    };
    // The tick the acquisitions stamped on the written pages (re-acquired if it moves before the
    // batch is recorded).
    u64 acquired_tick = scheduler.CurrentTick();
    vk::DescriptorSet run_set{};
    if (planned && !verify) {
        // A span's tracking is one range of one buffer (never seen otherwise; the acquisitions
        // repeat harmlessly in the replay, which makes the write effects itself).
        const bool one_buffer = acquire(true);
        for (u32 k = 0; k < count; ++k) {
            for (u32 v = 0; v < 3; ++v) {
                const auto& view = plan.writes[k].views[v];
                const auto& got = plan.view_obtained[k][v];
                const u64 aligned = Common::AlignDown(got.second, alignment);
                plan.push[k].AddOffset(6 + v, static_cast<u32>(got.second - aligned));
                plan.infos[6 + v][k] = vk::DescriptorBufferInfo{
                    got.first->Handle(), aligned, view.size + (got.second - aligned)};
            }
        }
        // One tracked range per span (at most MaxTracked: more were replayed above).
        plan.num_tracked = plan.num_spans;
        for (u32 s = 0; s < plan.num_spans; ++s) {
            const auto& span = plan.spans[s];
            plan.tracked[s] = {plan.obtained[s].first,
                               plan.obtained[s].second + (span.track_address - span.address),
                               span.track_size};
        }
        if (!one_buffer) {
            planned = false;
            replay = ReplaySpans;
            RunCount(stats.span_refusals.back());
        }
    }
    if (planned && !verify) {
        // BindBuffers' write effects, now that no replay follows: a span's once (but texture
        // invalidation per view: an image matches only its own base address), per view where
        // the views were acquired one by one.
        const auto note_write = [&](VAddr base, u64 size, u32 slot) {
            Gow3WatchRange::Note("storage", info.pgm_hash, slot, base, size);
            VideoCore::NoteGpuWriter(base, size, info.pgm_hash);
            if (sync_gpu_constants && small_gpu_writes.size() < 16384 &&
                info.pgm_hash != ReadbackSkipShader()) {
                if (size <= 4096) {
                    small_gpu_writes.emplace_back(base, size);
                    VideoCore::NoteReadbackKind(base, size, VideoCore::ReadbackSmallWrite);
                } else if (buffer_cache.IsCpuWriteHot(base, size)) {
                    const VAddr write_end = base + size;
                    for (VAddr window = Common::AlignDown(base, 1_MB); window < write_end;
                         window += 1_MB) {
                        if (buffer_cache.IsCpuWriteHot(window, 1_MB)) {
                            const VAddr lo = std::max(window, base);
                            const VAddr hi = std::min(window + 1_MB, write_end);
                            small_gpu_writes.emplace_back(lo, hi - lo);
                            VideoCore::NoteReadbackKind(lo, hi - lo, VideoCore::ReadbackHotWindow);
                        }
                    }
                }
            }
        };
        for (u32 s = 0; s < plan.num_spans; ++s) {
            if (!plan.spans[s].per_view) {
                note_write(plan.spans[s].address, plan.spans[s].size, 6);
            }
        }
        for (u32 k = 0; k < count; ++k) {
            const bool per_view = plan.spans[plan.span_of[k]].per_view;
            for (u32 v = 0; v < 3; ++v) {
                const auto& view = plan.writes[k].views[v];
                if (per_view) {
                    note_write(view.address, view.size, 6 + v);
                }
                texture_cache.InvalidateMemoryFromGPU(view.address, view.size);
            }
        }
        // The barrier check and the tracking, once per span.
        for (u32 t = 0; t < plan.num_tracked; ++t) {
            const auto& range = plan.tracked[t];
            bound_buffers.emplace_back(range.buffer, range.offset, static_cast<u32>(range.size),
                                       true);
            needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, range.size, true);
        }
        if (needs_barrier) {
            runtime.FlushBarriers();
        }
        scheduler.EndRendering();
        // Push data: one stream copy, one descriptor per instance (the array replacing it).
        auto& stream = buffer_cache.GetStreamBuffer();
        const u64 stride = Common::AlignUp(u64{sizeof(Shader::PushData)}, alignment);
        ASSERT(stride <= 256);
        for (u32 k = 0; k < count; ++k) {
            std::memcpy(plan.push_bytes.data() + k * stride, &plan.push[k], sizeof(Shader::PushData));
        }
        const u64 push_offset = stream.Copy(plan.push_bytes.data(), count * stride, alignment);
        for (u32 k = 0; k < count; ++k) {
            plan.infos[RunPlan::Bindings - 1][k] = vk::DescriptorBufferInfo{
                stream.Handle(), push_offset + k * stride, sizeof(Shader::PushData)};
        }
        run_set = pipeline->RunDescriptorSet();
        std::array<vk::WriteDescriptorSet, RunPlan::Bindings> writes;
        for (u32 b = 0; b < RunPlan::Bindings; ++b) {
            // Every descriptor is written (no partially-bound feature): the last instance pads.
            for (u32 j = count; j < Capacity; ++j) {
                plan.infos[b][j] = plan.infos[b][count - 1];
            }
            writes[b] = vk::WriteDescriptorSet{.dstSet = run_set,
                                               .dstBinding = b,
                                               .descriptorCount = Capacity,
                                               .descriptorType = vk::DescriptorType::eStorageBuffer,
                                               .pBufferInfo = plan.infos[b].data()};
        }
        instance.GetDevice().updateDescriptorSets(writes, {});
        // 24 Oct 2026 review: the acquisitions stamped the pages' GPU-write tick; a submission
        // since (texture invalidation resolving a readback, the stream copy waiting for room)
        // leaves them a tick before the batch, and a copy-queue fetch (FetchableWindowTick) could
        // read the bytes before it. Acquire again at the batch's tick (the same buffers and
        // offsets; its uploads get their barrier). Else the old path.
        bool placed = true;
        for (u32 tries = 0; placed && scheduler.CurrentTick() != acquired_tick; ++tries) {
            if (tries == 4) {
                placed = false;
                break;
            }
            acquired_tick = scheduler.CurrentTick();
            const auto obtained = plan.obtained;
            const auto view_obtained = plan.view_obtained;
            placed = acquire(false) && plan.obtained == obtained &&
                     plan.view_obtained == view_obtained;
            RunCount(stats.reacquired);
            needs_barrier = false; // flushed above; only the new acquisitions' uploads count
            for (u32 t = 0; placed && t < plan.num_tracked; ++t) {
                const auto& range = plan.tracked[t];
                needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, range.size, true);
            }
            if (placed && needs_barrier) {
                runtime.FlushBarriers();
            }
            scheduler.EndRendering();
        }
        if (!placed) {
            // Never seen; the replay repeats the write effects (duplicate small-write notes).
            ResetBindings(true); // the spans' ranges: tracked, never dispatched (barriers only)
            replay = ReplayTickMoved;
            planned = false;
        }
    }
    if (planned && !verify) {
        const vk::Pipeline handle = pipeline->RunHandle();
        const vk::PipelineLayout layout = pipeline->RunLayout();
        const u32 dim_x = CsRegs().dim_x;
        const vk::DescriptorSet set = run_set;
        scheduler.Checkpoint("dispatch run", info.pgm_hash, count);
        scheduler.Record([=](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
            cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, set, {});
            cmdbuf.dispatch(dim_x, count, 1);
        });
        bound_compute_pipeline = handle;
        bound_compute_serial = scheduler.RecordSerial();
        for (u32 k = 0; k < count; ++k) {
            DebugState.IncDispatch();
        }
        ResetBindings(true); // the spans' tracked ranges into the barrier tracker
        scheduler.KickRecording();
        // Fences after their dispatch: after the batch. Batched where RunComputeReleaseMem would
        // queue each on the Vulkan recorder (else it waits and writes them itself).
        const bool batch_releases = !Gow3Toggle::Disabled2(Gow3Toggle::RunReleaseBatch) &&
                                    hooks.task && scheduler.IsRecordingDeferred();
        for (u32 k = 0; k < count; ++k) {
            run_tasks(runs[k], batch_releases);
        }
        flush_releases();
        RunCount(stats.batched, count);
        RunCount(stats.batches);
        return;
    }
    // The per-dispatch path, one instance at a time (GOW3_DISPATCH_RUN_VERIFY: compared).
    RunCount(stats.replays[replay == NumRunReplays ? ReplayVerify : replay]);
    const bool compare = verify && planned;
    if (compare) {
        run_verify_writes.clear();
        plan.captured.fill(false);
    }
    // Instance k's registers: the first instance's with its own user data (stage A compared
    // the rest of the image when it joined the run).
    AmdGpu::ComputeProgram program;
    std::memcpy(&program, &CsRegs(), sizeof(program));
    const auto* run_cs = stage_cs;
    for (u32 k = 0; k < count; ++k) {
        install(runs[k]);
        std::memcpy(program.user_data.data(), runs[k].ud, sizeof(program.user_data));
        stage_cs = &program;
        run_verify_index = compare ? k : ~0u;
        DispatchRecord(pipeline);
        run_verify_index = ~0u;
        if (constant_ring) {
            constant_ring->Stamp(runs[k].ring_end);
        }
        run_tasks(runs[k], false);
    }
    run_verify_index = ~0u;
    stage_cs = run_cs;
    if (compare) {
        // The old path's GPU-written bytes (its acquisitions) are exactly the spans' bytes.
        auto& written = run_verify_writes;
        std::ranges::sort(written);
        u32 merged = 0;
        bool same = true;
        for (size_t i = 0; i < written.size() && same;) {
            VAddr lo = written[i].first, hi = lo + written[i].second;
            for (++i; i < written.size() && written[i].first <= hi; ++i) {
                hi = std::max<VAddr>(hi, written[i].first + written[i].second);
            }
            same = merged < plan.num_spans && plan.spans[merged].address == lo &&
                   plan.spans[merged].size == hi - lo;
            ++merged;
        }
        if (!same || merged != plan.num_spans) {
            RunCount(stats.bad_written);
        }
        // What the batch would bind, from the plan and the old path's own placement of each
        // span's first byte (the batch takes every view of a span from one acquisition at that
        // offset): descriptors, push data, and the tracked ranges inside the span's.
        std::array<u32, RunPlan::MaxSpans> first_of{};
        for (u32 k = 0; k < count; ++k) {
            if (plan.writes[k].views[0].address == plan.spans[plan.span_of[k]].address) {
                first_of[plan.span_of[k]] = k;
            }
        }
        u64 bad_descriptors = 0, bad_push = 0, bad_tracking = 0;
        for (u32 k = 0; k < count; ++k) {
            const u32 s = plan.span_of[k], f = first_of[s];
            const auto& span = plan.spans[s];
            if (!plan.captured[k] || !plan.captured[f] || !plan.replayed_tracked_count[f]) {
                ++bad_descriptors;
                continue;
            }
            bool descriptors = true;
            for (const u32 b : {0u, 1u, 2u, 3u, 4u, 5u, 9u}) {
                descriptors &= plan.replayed[b][k] == plan.infos[b][k];
            }
            const auto& base_info = plan.replayed[6][f];
            const u64 base = base_info.offset + plan.replayed_push[f].buf_offsets[6];
            auto push = plan.push[k];
            for (u32 v = 0; v < 3; ++v) {
                const auto& view = plan.writes[k].views[v];
                const u64 bound = base + (view.address - span.address);
                const u64 aligned = Common::AlignDown(bound, alignment);
                push.AddOffset(6 + v, static_cast<u32>(bound - aligned));
                descriptors &= plan.replayed[6 + v][k] ==
                               vk::DescriptorBufferInfo{base_info.buffer, aligned,
                                                        view.size + (bound - aligned)};
            }
            bad_descriptors += !descriptors;
            bad_push += std::memcmp(&push, &plan.replayed_push[k], sizeof(push)) != 0;
            const auto* buffer = plan.replayed_tracked[f][0].buffer;
            const u64 lo = base + (span.track_address - span.address), hi = lo + span.track_size;
            bool tracking = true;
            for (u32 t = 0; t < plan.replayed_tracked_count[k]; ++t) {
                const auto& range = plan.replayed_tracked[k][t];
                tracking &= range.buffer == buffer && range.offset >= lo && range.offset + range.size <= hi;
            }
            bad_tracking += !tracking;
        }
        RunCount(stats.bad_descriptors, bad_descriptors);
        RunCount(stats.bad_push, bad_push);
        RunCount(stats.bad_tracking, bad_tracking);
        RunCount(stats.verified, count);
    }
}

void Rasterizer::VerifyRunInstance() {
    // GOW3_DISPATCH_RUN_VERIFY: what the old path binds for instance k, compared with the batch
    // plan once the run is replayed (DispatchRunRecord).
    const u32 k = run_verify_index;
    auto& plan = *run_plan;
    bool captured = set_writes.size() == RunPlan::Bindings - 1;
    for (u32 b = 0; captured && b < RunPlan::Bindings - 1; ++b) {
        const auto* bound = set_writes[b].pBufferInfo;
        captured = bound && set_writes[b].dstBinding == b;
        if (captured) {
            plan.replayed[b][k] = *bound;
        }
    }
    plan.replayed_push[k] = push_data;
    u8 written = 0;
    for (const auto& bound : bound_buffers) {
        if (bound.is_written) {
            if (written == plan.replayed_tracked[k].size()) {
                captured = false;
                break;
            }
            plan.replayed_tracked[k][written++] = {bound.buffer, bound.offset, bound.size};
        }
    }
    plan.replayed_tracked_count[k] = written;
    plan.captured[k] = captured;
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;
    CutDispatchRun(CutIndirect);
    DispatchStatsScope stats;
    // gow3: handed to the recording thread like direct dispatches.
    const bool pipelined = UseDrawPipe() && !Gow3Toggle::Disabled(Gow3Toggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (stats.on) {
        stats.hash = pipeline->GetStage(Shader::SwStage::Compute).pgm_hash;
    }
    if (pipelined) {
        const IndirectDraw indirect = {address + offset, 0, size, 1};
        PostDraw(pipeline, nullptr, false, 0, &CsRegs(), &indirect);
        return;
    }
    DispatchIndirectRecord(pipeline, address + offset, size);
}

void Rasterizer::DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr address,
                                        u32 size) {
    if (SkippedShader(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash)) {
        return;
    }
    scheduler.PopPendingOperations();
    const u32 offset = 0;
    NoteDisplayStorageWrite(pipeline->GetStage(Shader::SwStage::Compute));

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);
    if (FrameCapture::Active()) {
        const u32* args = reinterpret_cast<const u32*>(address + offset);
        FrameCapture::Dispatch(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash, args[0],
                               args[1], args[2]);
        FrameCapture::Note("\n  indirect dispatch (CPU view of args)");
    }

    const vk::Pipeline handle = pipeline->Handle();
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    const u64 cloth_dispatch = cloth_peeks.empty() ? 0 : ++g_cloth_dispatches;
    for (const auto& peek : cloth_peeks) {
        buffer_cache.PeekGpuBuffer(peek.buffer, peek.buffer_offset, peek.address, peek.size, peek.shader,
                                   peek.slot, cloth_dispatch, 0);
    }
    const bool full_barrier = FullBarriersEnabled();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        if (full_barrier) {
            RecordFullBarrier(cmdbuf);
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        cmdbuf.dispatchIndirect(args, args_offset);
        if (full_barrier) {
            RecordFullBarrier(cmdbuf);
        }
    });
    for (const auto& peek : cloth_peeks) {
        if (peek.written) {
            buffer_cache.PeekGpuBuffer(peek.buffer, peek.buffer_offset, peek.address, peek.size, peek.shader,
                                       peek.slot, cloth_dispatch, 1);
        }
    }
    cloth_peeks.clear();
    DebugState.IncDispatch();

    ResetBindings(true);
    if (const u64 hash = pipeline->GetStage(Shader::SwStage::Compute).pgm_hash;
        ClothTraceEnabled() && (hash == 0x1c957b01 || hash == 0x809864d3)) {
        g_cloth_epoch = runtime.AccessEpoch();
        g_cloth_epoch_shader = hash;
    }
    scheduler.KickRecording();
}

u64 Rasterizer::Flush() {
    DrainDrawPipe();
    flush_count.fetch_add(1, std::memory_order_relaxed);
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    DrainDrawPipe();
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    DrainDrawPipe();
    buffer_cache.ReclaimArenaMemory();
    buffer_cache.PollPeeks();
    memory->CheckCopiedSources();
    CheckSrtWalks();
    CheckScalarCopySources();
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    SyncSmallGpuWrites();
    texture_cache.ProcessDownloadImages();
    // Diagnostics: GOW3_RETEXTURE_FILE=<file>; creating it re-reads all CPU-sourced textures.
    // 20 Oct 2026 audit: polled off this thread; exists() here cost 25-50 ms per check (d026d97).
    static const int retexture = Gow3TriggerFiles::Watch("GOW3_RETEXTURE_FILE");
    if (Gow3TriggerFiles::Take(retexture)) {
        std::printf("Texture cache: %zu textures marked for re-upload\n", texture_cache.DebugInvalidateAll());
    }
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
    // gow3 statistics: container sizes, to find what grows while the frame rate falls.
    static auto sizes_at = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - sizes_at >= std::chrono::seconds(5)) {
        sizes_at = now;
        std::printf("Sizes: textures: %s\n", texture_cache.DebugSizes().c_str());
        std::printf("Sizes: buffers: %s\n", buffer_cache.DebugSizes().c_str());
        std::printf("Sizes: pipelines: %s; scheduler: %s\n", pipeline_cache.DebugSizes().c_str(),
                    scheduler.DebugSizes().c_str());
        std::printf("Sizes: rasterizer: %zu SRT plans, %zu keep-alive entries, %zu small GPU writes\n",
                    srt_plans.size(), pipe_keepalive.size(), small_gpu_writes.size());
        std::fflush(stdout);
    }
}

// gow3: Uncharted writes per-object constants with a GPU copy shader (2d215c3b) and its draws read
// them through SRT pointers, which the CPU-side walker flattens from guest memory. With Relaxed
// readbacks those GPU writes reach guest memory only when the CPU later writes the same page, so
// the walker kept reading zeros (ground missing, Nate unlit until a menu happened to touch the
// page). GOW3_SYNC_GPU_CONSTANTS=1 copies small shader writes back at each submission; the next
// frame's walker sees them. GOW3_READBACK_TRIGGER=<file>: "auto"/"off" switches that at run
// time; "<lo> <hi>" (hex) copies one range back once (diagnostics).
// gow3: a shader reading guest memory through computed addresses (DMA, e.g. Uncharted's tiled
// lighting cs 43b8ee5e) needs CPU writes on the GPU first. The full sync uploads every
// CPU-modified resident range (per-frame constant rings, command buffers, ...) and stalls on
// staging space: about half of the GPU thread's time in Uncharted's temple. GOW3_DMA_TRIGGER
// (file, read live) selects the mode: "full", "off", or "pointers <KiB>" = only windows of that
// size around the guest addresses in the stage's user data and flattened SRT data.
// GOW3_DMA_SYNC sets the start mode the same way. GOW3_FRAME_STATS adds 5 s statistics.
void Rasterizer::SyncDmaMemory(const Shader::Info* stage) {
    enum class Mode { Full, Off, Pointers };
    static Mode mode = Mode::Full;
    static u64 window = 1_MB;
    static const auto parse = [](const std::string& text) {
        if (text.starts_with("bias")) {
            depth_bias_off = text.find("off") != std::string::npos;
            std::printf("GPU depth bias: %s%c", depth_bias_off.load() ? "off" : "normal", 10);
            std::fflush(stdout);
            return;
        }
        if (text.starts_with("dbclear")) {
            depth16_clear_one = text.find("one") != std::string::npos;
            std::printf("GPU 16-bit depth clears: %s\n", depth16_clear_one.load() ? "1.0" : "normal");
            std::fflush(stdout);
            return;
        }
        if (text.starts_with("clear")) {
            shadow_clear_far = text.find("far") != std::string::npos;
            std::printf("GPU shadow clear quad: %s\n", shadow_clear_far.load() ? "far (1.0)" : "normal");
            std::fflush(stdout);
            return;
        }
        if (text.starts_with("cmp")) {
            const int value = text.find("never") != std::string::npos          ? 1
                              : text.find("always") != std::string::npos       ? 2
                              : text.find("border-black") != std::string::npos ? 3
                              : text.find("border-white") != std::string::npos ? 4
                              : text.find("edge") != std::string::npos         ? 5
                                                                               : 0;
            VideoCore::sampler_compare_override = value;
            static const char* const names[] = {"normal", "never", "always", "border-black", "border-white", "edge"};
            std::printf("GPU compare samplers: %s\n", names[value]);
            std::fflush(stdout);
            return;
        }
        if (text.starts_with("off")) {
            mode = Mode::Off;
        } else if (text.starts_with("pointers")) {
            mode = Mode::Pointers;
            const u64 kib = std::strtoull(text.c_str() + 8, nullptr, 10);
            window = std::clamp<u64>(kib ? kib : 1024, 64, 65536) * 1024;
        } else {
            mode = Mode::Full;
        }
        std::printf("GPU DMA sync: %s", mode == Mode::Full ? "full" : mode == Mode::Off ? "off" : "pointers");
        if (mode == Mode::Pointers) {
            std::printf(" (%llu KiB windows)", (unsigned long long)(window / 1024));
        }
        std::printf("\n");
        std::fflush(stdout);
    };
    static const bool init = [] {
        if (const char* env = std::getenv("GOW3_DMA_SYNC"); env && env[0]) {
            parse(env);
        }
        return true;
    }();
    (void)init;
    static const int trigger = Gow3TriggerFiles::Watch("GOW3_DMA_TRIGGER");
    if (const auto text = Gow3TriggerFiles::Take(trigger)) {
        parse(*text);
    }
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    const u64 bytes_before = Gow3Stats::buffer_upload_bytes.load(std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    u32 windows = 0;
    if (mode == Mode::Full || !stage) {
        buffer_cache.SynchronizeDmaBuffers();
    } else if (mode == Mode::Pointers) {
        // Guest addresses: dword pairs (V# base or raw pointer) in the direct memory range.
        boost::container::small_vector<VAddr, 16> bases;
        const auto scan = [&](std::span<const u32> data) {
            for (size_t i = 0; i + 1 < data.size(); ++i) {
                const VAddr address = data[i] | (VAddr(data[i + 1] & 0xFFFF) << 32);
                if (address < 0x1000000000ULL || address >= 0x3000000000ULL) {
                    continue;
                }
                const VAddr base = Common::AlignDown(address, 4_KB);
                if (std::ranges::find(bases, base) == bases.end()) {
                    bases.push_back(base);
                }
            }
        };
        scan(stage->user_data);
        scan(stage->flattened_ud_buf);
        // ObtainBuffer makes the window resident and uploads its CPU writes: a DMA read of a
        // page the GPU page table does not map yet returns 0 and is only mapped after the
        // submission (FaultManager), so per-frame data on fresh pages flickered (Uncharted hair
        // and cobwebs reading their shadow cascades through MUBUF ADDR64 loads). A window can
        // cover fiber stacks: those stay writable (gow3gpu.cpp Gow3KeepWritable).
        for (const VAddr base : bases) {
            if (IsMapped(base, window)) {
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(base, static_cast<u32>(window), false);
                if (std::ranges::find(dma_read_arenas, buffer) == dma_read_arenas.end()) {
                    dma_read_arenas.push_back(buffer);
                }
            }
        }
        // DMA reads go through the page table to any address of these arenas, and the barrier
        // tracker knows nothing about them: the uploads just recorded (and any other pending
        // write there) must finish before the draw reads, and the draw's reads before later
        // copies overwrite them (ResetBindings). Without this the hair's shadow cascade data
        // was read mid-upload now and then (Uncharted 3: hair lit for a frame).
        for (const auto* arena : dma_read_arenas) {
            needs_barrier |= runtime.IsBufferAccessed(arena, 0, arena->SizeBytes());
        }
        windows = static_cast<u32>(bases.size());
    }
    if (stats) {
        static u64 calls, bytes, ns, window_count;
        static auto report = std::chrono::steady_clock::now();
        const auto end = std::chrono::steady_clock::now();
        ++calls;
        bytes += Gow3Stats::buffer_upload_bytes.load(std::memory_order_relaxed) - bytes_before;
        ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        window_count += windows;
        if (end - report >= std::chrono::seconds(5)) {
            report = end;
            std::printf("DMA sync (5 s): %llu calls, %.1f MB uploaded, %.1f ms on the GPU thread, %llu windows\n",
                        (unsigned long long)calls, bytes / 1e6, ns / 1e6, (unsigned long long)window_count);
            std::fflush(stdout);
            calls = bytes = ns = window_count = 0;
        }
    }
}

bool Rasterizer::ReadBackEopResults() {
    // gow3: tiny GPU-written images (TextureCache::ProcessTinyReadbacks), unprotected: the CPU
    // reads the previous value until they land (Uncharted 2 reads its aim focus distance every
    // frame; a read fault waiting for the GPU each time stalled its threads). Queued first, so the
    // fence waits for the last readback below if there is one.
    bool soft_pending = false;
    if (!eop_soft_readbacks.empty()) {
        std::erase_if(eop_soft_readbacks, [this](const auto& r) { return !IsMapped(r.first, r.second); });
        if (!eop_soft_readbacks.empty()) {
            buffer_cache.DownloadRangesAsync(eop_soft_readbacks, false, true);
            soft_pending = true;
        }
        eop_soft_readbacks.clear();
    }
    // Ray-cast results: unprotected (see eop_open_readbacks); their fence waits for them.
    bool open_pending = false;
    if (!eop_open_readbacks.empty()) {
        std::erase_if(eop_open_readbacks,
                      [this](const auto& r) { return !IsMapped(r.first, r.second); });
        if (!eop_open_readbacks.empty()) {
            if (EopReadbackMode() == 2) {
                buffer_cache.DownloadRanges(eop_open_readbacks);
            } else {
                buffer_cache.DownloadRangesAsync(eop_open_readbacks, false, true, true);
                open_pending = true;
            }
        }
        eop_open_readbacks.clear();
    }
    if (eop_readbacks.empty()) {
        return soft_pending || open_pending;
    }
    std::erase_if(eop_readbacks, [this](const auto& r) { return !IsMapped(r.first, r.second); });
    static u32 described = 0;
    for (const auto& [address, size] : eop_readbacks) {
        if (described < 6) {
            ++described;
            std::printf("EOP readback range %#llx size %llu\n", static_cast<unsigned long long>(address),
                        static_cast<unsigned long long>(size));
        }
    }
    bool pending = false;
    if (EopReadbackMode() == 2) {
        buffer_cache.DownloadRanges(eop_readbacks);
    } else {
        buffer_cache.DownloadRangesAsync(eop_readbacks, true, true);
        pending = true;
    }
    eop_readbacks.clear();
    static u32 readbacks = 0;
    static auto last = std::chrono::steady_clock::now();
    ++readbacks;
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(5)) {
        std::printf("EOP readbacks (5 s): %u (ray cast results and 6a6422a9 copies, read-protected "
                    "until copied); fence labels inside one: %u\n",
                    readbacks, VideoCore::BufferCache::TakeLabelsInReadbacks());
        std::fflush(stdout);
        readbacks = 0;
        last = now;
    }
    return pending || open_pending;
}

void Rasterizer::SyncSmallGpuWrites() {
    buffer_cache.PollReadbacks();
    static const int trigger = Gow3TriggerFiles::Watch("GOW3_READBACK_TRIGGER");
    if (const auto taken = Gow3TriggerFiles::Take(trigger)) {
        const std::string& text = *taken;
        if (text.starts_with("auto")) {
            sync_gpu_constants = true;
            std::printf("GPU constants sync: on\n");
        } else if (text.starts_with("off")) {
            sync_gpu_constants = false;
            small_gpu_writes.clear();
            std::printf("GPU constants sync: off\n");
        } else if (text.starts_with("range")) {
            // "range <lo> <hi>": downloaded after every submission (until "range clear").
            char* end = nullptr;
            const u64 lo = std::strtoull(text.c_str() + 5, &end, 16);
            const u64 hi = end ? std::strtoull(end, nullptr, 16) : 0;
            if (text.find("clear") != std::string::npos) {
                synced_ranges.clear();
                std::printf("GPU readback ranges: cleared\n");
            } else if (hi > lo && IsMapped(lo, hi - lo)) {
                synced_ranges.emplace_back(lo, hi - lo);
                std::printf("GPU readback range %#llx-%#llx: after every submission\n",
                            (unsigned long long)lo, (unsigned long long)hi);
            } else {
                std::printf("GPU readback: ignored trigger '%s'\n", text.c_str());
            }
        } else {
            char* end = nullptr;
            const u64 lo = std::strtoull(text.c_str(), &end, 16);
            const u64 hi = end ? std::strtoull(end, nullptr, 16) : 0;
            if (hi > lo && IsMapped(lo, hi - lo)) {
                const std::pair<VAddr, u64> range{lo, hi - lo};
                buffer_cache.DownloadRanges(std::span{&range, 1});
                std::printf("GPU readback: %#llx-%#llx copied to guest memory\n",
                            (unsigned long long)lo, (unsigned long long)hi);
            } else {
                std::printf("GPU readback: ignored trigger '%s'\n", text.c_str());
            }
        }
        std::fflush(stdout);
    }
    if (!synced_ranges.empty()) {
        buffer_cache.DownloadRanges(synced_ranges);
    }
    // Pages the CPU now shares with GPU data in flight (BufferCache: shared write path).
    for (const auto& range : buffer_cache.TakeSharedReadbacks()) {
        small_gpu_writes.push_back(range);
    }
    if (!sync_gpu_constants || small_gpu_writes.empty()) {
        return;
    }
    // Merged in 256-byte granules (GOW3_SMALL_READBACK_ALIGN=<bytes>, a power of two). They were
    // 64 KiB windows ("the copy shader writes many small neighbouring pieces"): with the effect
    // shaders' small writes (cs 8f067540) spread over 30 MiB, the windows joined up and every
    // GPU-written byte between them came back each submission, ~2.3 GB per 5 s in Uncharted 2
    // and 3 that the CPU never reads (16 Oct). Neighbouring pieces still merge.
    static const u64 granule = [] {
        const char* env = std::getenv("GOW3_SMALL_READBACK_ALIGN");
        const u64 value = env ? std::strtoull(env, nullptr, 0) : 0;
        return value >= 64 && std::has_single_bit(value) ? value : u64(256);
    }();
    std::sort(small_gpu_writes.begin(), small_gpu_writes.end());
    std::vector<std::pair<VAddr, u64>> windows;
    for (const auto& [address, size] : small_gpu_writes) {
        const VAddr start = Common::AlignDown(address, granule);
        const VAddr end = Common::AlignUp(address + size, granule);
        if (!windows.empty() && start <= windows.back().first + windows.back().second) {
            const VAddr back_end = windows.back().first + windows.back().second;
            windows.back().second = std::max(back_end, end) - windows.back().first;
        } else {
            windows.emplace_back(start, end - start);
        }
    }
    small_gpu_writes.clear();
    std::erase_if(windows, [this](const auto& w) { return !IsMapped(w.first, w.second); });
    // gow3: GOW3_ASYNC_READBACK=1: the copies run after this submission and reach guest memory
    // when the GPU is done (BufferCache::PollReadbacks, per draw), with no wait for the whole
    // frame on the GPU; until then their pages are read-protected (a read waits for that
    // readback). =2: unprotected, as first tried: data the game (or the SRT walker) read the
    // next frame could be stale (Uncharted 3 "Command context dcb full" at the London pub;
    // garbage sharps for the treasure pickup effect, cs 8f067540, in U1 and U2). Off by default.
    // Since 15 Oct the default is 2 (unset): with the copy shaders and CP fills/copies written
    // through (BufferCache::WriteThrough), GPU-written resource tables downloaded on demand
    // (SyncSrtTables) and no lazy-write merges, these readbacks only keep CPU-read GPU results
    // (luminance and the like) about a frame behind, as on the console; the full wait per
    // submission cost ~12 ms per frame in heavy scenes. GOW3_ASYNC_READBACK=0 waits instead.
    static const bool async_readback = [] {
        const char* env = std::getenv("GOW3_ASYNC_READBACK");
        return !(env && env[0] == '0');
    }();
    if (async_readback) {
        buffer_cache.DownloadRangesAsync(windows);
    } else {
        buffer_cache.DownloadRanges(windows);
    }
}

// gow3: resource tables (SRT) that another shader writes earlier in the same command stream.
// Uncharted 2's treasure pickup effect (cs 7d7e7206, 8f067540) bound garbage V#s and the GPU
// faulted (device lost): the CPU-side walker reads guest memory, which receives GPU writes only
// at the per-submission readback (SyncSmallGpuWrites) or later, so a new effect's tables were not
// there yet. The tables a walker reads come from its code (PushPtr: push rdi; mov rdi,
// [rdi + off]; reads [rdi + off]; pop rdi). Tables with GPU-written bytes are downloaded before
// the walk (one GPU wait each). GOW3_SRT_SYNC=0 turns this off.
const Rasterizer::SrtPlan& Rasterizer::SrtPlanFor(const Shader::Info& info) {
    const auto& srt = info.srt_info;
    // gow3 20 Oct 2026 (laptop CPUs): the map lookup on every walk was ~1% of the GPU command
    // thread on a Ryzen 7840HS (NVIDIA GPU and 780M, Uncharted 3). Plans are never erased and the
    // map's nodes do not move, so a small cache of plan addresses by walker is safe.
    const void* walker = reinterpret_cast<const void*>(srt.walker_func);
    auto& memo = srt_plan_memo[(reinterpret_cast<uintptr_t>(walker) >> 4) & (SrtPlanMemoSize - 1)];
    if (memo.first == walker && memo.second) {
        return *memo.second;
    }
    auto [it, inserted] = srt_plans.try_emplace(walker);
    auto& plan = it->second;
    if (!inserted) {
        memo = {walker, &plan};
        return plan;
    }
    // Straight-line code: rdi is the current table (the user data at the start), rsi the
    // flattened buffer; values pass through r10.
    std::vector<s32> saved;
    s32 current = -1;
    s32 last_node = -2;
    u32 last_off = 0;
    const u8* code = reinterpret_cast<const u8*>(srt.walker_func);
    const u8* end = code + srt.walker_func_size;
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    while (code < end && ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                             inst, ops, const_cast<u8*>(code), end - code))) {
        code += inst.length;
        const bool rdi_reg = inst.operand_count_visible > 0 &&
                             ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                             ops[0].reg.value == ZYDIS_REGISTER_RDI;
        if (inst.mnemonic == ZYDIS_MNEMONIC_RET) {
            break;
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_PUSH && rdi_reg) {
            saved.push_back(current);
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_POP && rdi_reg) {
            current = saved.empty() ? -1 : saved.back();
            if (!saved.empty()) {
                saved.pop_back();
            }
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_XOR && rdi_reg) {
            // A pointer load the fault handler patched out: a null table.
            plan.nodes.push_back({current, 0, 0, true});
            current = s32(plan.nodes.size() - 1);
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_MOV && inst.operand_count_visible == 2 &&
                   ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   ops[1].mem.base == ZYDIS_REGISTER_RDI) {
            const bool indexed = ops[1].mem.index != ZYDIS_REGISTER_NONE;
            const u32 disp = ops[1].mem.disp.has_displacement ? u32(ops[1].mem.disp.value) : 0;
            if (rdi_reg) {
                plan.nodes.push_back({current, disp, 0, indexed});
                current = s32(plan.nodes.size() - 1);
            } else {
                last_node = indexed ? -2 : current;
                last_off = disp;
                if (current >= 0) {
                    // A computed read offset: the first KiB.
                    const u32 extent = indexed ? 1024 : disp + ops[1].size / 8;
                    plan.nodes[current].extent = std::max(plan.nodes[current].extent, extent);
                }
            }
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_MOV && inst.operand_count_visible == 2 &&
                   ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   ops[0].mem.base == ZYDIS_REGISTER_RSI && last_node != -2) {
            const u32 disp = ops[0].mem.disp.has_displacement ? u32(ops[0].mem.disp.value) : 0;
            for (u32 b = 0; b < ops[0].size / 8; b += 4) {
                plan.copies.push_back({(disp + b) / 4, last_node, last_off + b});
            }
        }
    }
    return plan;
}

// gow3 21 Oct 2026 (GOW3_SRT_CHECK): the draw whose tables the GPU thread (or the draw pipe's
// stage B, which has its own registers) walks right now.
Rasterizer::SrtDrawContext Rasterizer::CurrentDrawContext() const {
    const auto& regs = Regs();
    return {
        .color0 = regs.color_buffers[0] ? regs.color_buffers[0].Address() : 0,
        .depth = regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthAddress() : 0,
        .index = regs.index_base_address.Address<VAddr>(),
        .num_indices = regs.num_indices,
    };
}

VAddr Rasterizer::SrtSourceAddress(Shader::PFN_SrtWalker walker, size_t walker_size,
                                   std::span<const u32> user_data, u32 flat_dw, const char** kind) {
    Shader::Info info{};
    info.srt_info.walker_func = walker;
    info.srt_info.walker_func_size = walker_size;
    info.user_data = user_data;
    const auto& plan = SrtPlanFor(info);
    std::vector<VAddr> address(plan.nodes.size(), 0);
    ResolveSrtTables(plan, info, address.data());
    *kind = "not in plan";
    for (const auto& copy : plan.copies) {
        if (copy.flat_dw != flat_dw) {
            continue;
        }
        if (copy.node < 0) {
            *kind = "user data";
            return 0;
        }
        const VAddr source = address[copy.node] ? address[copy.node] + copy.src_off : 0;
        if (!source) {
            *kind = "table unknown";
            return 0;
        }
        if (buffer_cache.IsRangeGpuWritten(source, 4)) {
            *kind = buffer_cache.PendingReadbackTickFor(source, 4) ? "GPU-written, readback pending"
                                                                        : "GPU-written";
        } else if (buffer_cache.IsRegionCpuModified(source, 4)) {
            *kind = "CPU-written since upload";
        } else {
            *kind = "untracked/clean";
        }
        return source;
    }
    return 0;
}

void Rasterizer::ResolveSrtTables(const SrtPlan& plan, const Shader::Info& info, VAddr* address) {
    const auto user_data = std::as_bytes(info.user_data);
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& node = plan.nodes[i];
        address[i] = 0;
        if (node.dynamic) {
            continue;
        }
        u64 value = 0;
        if (node.parent < 0) {
            if (node.offset + sizeof(value) > user_data.size()) {
                continue;
            }
            std::memcpy(&value, user_data.data() + node.offset, sizeof(value));
        } else {
            const VAddr table = address[node.parent];
            if (!table || !IsMapped(table + node.offset, sizeof(value))) {
                continue;
            }
            std::memcpy(&value, reinterpret_cast<const void*>(table + node.offset), sizeof(value));
        }
        address[i] = value & 0xFFFFFFFFFFFFULL;
    }
}

void Rasterizer::SyncSrtTables(const Shader::Info& info) {
    const auto& srt = info.srt_info;
    if (!srt.walker_func || !srt.walker_func_size || !OnStageA() ||
        PipelineCache::capturing_for_gpu_thread) {
        return;
    }
    const auto& plan = SrtPlanFor(info);
    if (plan.nodes.empty()) {
        return;
    }
    // gow3 18 Oct: a dispatch still in the draw pipe has not marked its buffer writes yet, so the
    // GPU-written check below cannot see them: the walk read the tables before the GPU wrote
    // them (Uncharted 2's train still flashed white, rarely, after the copy shaders drained).
    // Reads overlapping a pending dispatch's written buffers drain the pipe first; draining
    // before every walk cost 15% in U3's bar fight. SrtPipeCheck (second word) skips it (A/B).
    // gow3 20 Oct, U3 chapter 2: ~1,500 drains/s but only one download in 5 s; the 256-range
    // overflow made almost every wait a false positive (18.8% of stage A). With the ring on,
    // its pending writes cover every stage's buffers; labels and un-noted tasks cover the rest.
    // Packets built without that tracking keep the old check until a full drain.
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    static u64 pipe_reads = 0, pipe_drains = 0;
    static std::array<u64, 5> pipe_reasons{};
    const bool check_pipe = !Gow3Toggle::Disabled2(Gow3Toggle::SrtPipeCheck);
    const bool memos = !Gow3Toggle::Disabled2(Gow3Toggle::DispatchStageAMemos);
    const auto pipe_wait = [&](VAddr address, u64 size) {
        if (!check_pipe) {
            return;
        }
        if (stats) {
            ++pipe_reads;
        }
        if (draw_pipe && constant_ring && !Gow3Toggle::Disabled(Gow3Toggle::ConstantRing) &&
            !Gow3Toggle::Disabled2(Gow3Toggle::SrtPendingWrites) && pending_complete) {
            PrunePendingLabels();
            u32 reason = 0;
            bool overlap = PendingWriteOverlaps(address, size);
            if (!overlap) {
                reason = 1;
                // gow3 22 Oct 2026 (Gow3Toggle::DispatchStageAMemos; U3 meat market: this scan
                // 2.9% of the GPU command thread): a read outside the bounds of every label
                // pushed since the list was last empty overlaps none of the pending ones.
                if (!memos || (address < label_max && label_min < address + size)) {
                    ++g_stage_a_memos.label_scans;
                    for (const auto& label : pending_labels) {
                        if (address < label.end && label.begin < address + size) {
                            overlap = true;
                            break;
                        }
                    }
                } else {
                    ++g_stage_a_memos.label_skips;
                    if (Shader::StageAVerify() &&
                        std::ranges::any_of(pending_labels, [&](const PendingWrite& label) {
                            return address < label.end && label.begin < address + size;
                        })) {
                        ++g_stage_a_memos.label_mismatches; // overlap is left as the cut said
                    }
                }
            }
            if (!overlap) {
                reason = 2;
                overlap = !draw_pipe->Reached(pending_task_position);
            }
            if (overlap) {
                if (stats) {
                    ++pipe_drains;
                    ++pipe_reasons[reason];
                }
                DrainDrawPipe();
            }
            return;
        }
        if (pipe_write_ranges.empty() && !pipe_writes_overflow) {
            return;
        }
        bool overlap = pipe_writes_overflow;
        for (const auto& [base, bytes] : pipe_write_ranges) {
            overlap = overlap || (address < base + bytes && base < address + size);
        }
        if (overlap) {
            if (stats) {
                ++pipe_drains;
                ++pipe_reasons[pipe_writes_overflow ? 3 : 4];
            }
            DrainDrawPipe();
        }
    };
    const auto user_data = std::as_bytes(info.user_data);
    boost::container::small_vector<VAddr, 16> address(plan.nodes.size(), 0);
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& node = plan.nodes[i];
        if (node.dynamic) {
            continue;
        }
        u64 value = 0;
        if (node.parent < 0) {
            if (node.offset + sizeof(value) > user_data.size()) {
                continue;
            }
            std::memcpy(&value, user_data.data() + node.offset, sizeof(value));
        } else {
            const VAddr table = address[node.parent];
            if (!table || !IsMapped(table + node.offset, sizeof(value))) {
                continue;
            }
            pipe_wait(table + node.offset, sizeof(value));
            std::memcpy(&value, reinterpret_cast<const void*>(table + node.offset), sizeof(value));
        }
        const VAddr table = value & 0xFFFFFFFFFFFFULL;
        address[i] = table;
        if (table && node.extent) {
            pipe_wait(table, node.extent);
        }
        // GOW3_SRT_PENDING_DRAIN: a copy shader queued for stage B (scalar copy HLE) that writes
        // into this table runs before the walk reads it. Parents come first in the plan, so the
        // pointers read so far are current.
        if (table && node.extent && draw_pipe && !pending_copy_writes.empty() &&
            PendingCopyWriteOverlaps(table, node.extent) && IsMapped(table, node.extent)) {
            const bool report = SrtPendingDrain() == 2;
            const u32 bytes = std::min<u32>(node.extent, 512);
            std::array<u8, 512> before;
            if (report) {
                std::memcpy(before.data(), reinterpret_cast<const void*>(table), bytes);
            }
            const auto wait_start = report ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            WaitPendingWrites(pending_copy_writes, table, node.extent);
            if (report) {
                srt_pending_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - wait_start)
                                          .count());
            }
            ++srt_pending_drains;
            if (report) {
                u32 changed = 0;
                bool zero = true;
                const auto* now = reinterpret_cast<const u8*>(table);
                for (u32 b = 0; b < bytes; ++b) {
                    changed += before[b] != now[b];
                    zero &= before[b] == 0;
                }
                static std::map<u64, std::array<u64, 3>> by_shader; // drains, changed, all zero
                static u32 lines = 0;
                auto& s = by_shader[info.pgm_hash];
                ++s[0];
                if (changed) {
                    ++s[1];
                    ++srt_pending_changed;
                    s[2] += zero;
                    srt_pending_zero += zero;
                    if (lines < 24) {
                        ++lines;
                        std::printf("SRT pending: shader %016llx table %#llx (+%u bytes, level %d): "
                                    "%u bytes changed by stage B's queued writes%s\n",
                                    static_cast<unsigned long long>(info.pgm_hash),
                                    static_cast<unsigned long long>(table), node.extent,
                                    node.parent < 0 ? 1 : 2, changed,
                                    zero ? " (were all zero)" : "");
                    }
                }
                static auto last = std::chrono::steady_clock::now();
                if (const auto t = std::chrono::steady_clock::now(); t - last >= std::chrono::seconds{5}) {
                    last = t;
                    std::string top;
                    for (const auto& [hash, n] : by_shader) {
                        if (n[1]) {
                            top += fmt::format(" {:08x}:{}/{}{}", hash, n[1], n[0],
                                               n[2] ? fmt::format("(z{})", n[2]) : std::string{});
                        }
                    }
                    std::printf("SRT pending (5 s): %llu walks waited for stage B (%.1f ms), %llu tables "
                                "changed by it (%llu all zero before); changed/waits by shader:%s\n",
                                static_cast<unsigned long long>(srt_pending_drains),
                                srt_pending_ns / 1e6,
                                static_cast<unsigned long long>(srt_pending_changed),
                                static_cast<unsigned long long>(srt_pending_zero), top.c_str());
                    std::fflush(stdout);
                    by_shader.clear();
                    srt_pending_drains = srt_pending_changed = srt_pending_zero = srt_pending_ns = 0;
                }
            }
        }
        if (!table || !node.extent || !buffer_cache.IsRangeGpuWrittenShared(table, node.extent) ||
            !IsMapped(table, node.extent)) {
            continue;
        }
        DrainDrawPipe();
        const std::pair<VAddr, u64> range{table, node.extent};
        buffer_cache.DownloadRanges(std::span{&range, 1});
        ++srt_syncs;
        if (srt_sync_reports < 16) {
            ++srt_sync_reports;
            std::printf("SRT sync: shader %016llx table %#llx (+%u bytes, level %d) had "
                        "GPU-written data; downloaded before the walk\n",
                        static_cast<unsigned long long>(info.pgm_hash),
                        static_cast<unsigned long long>(table), node.extent,
                        node.parent < 0 ? 1 : 2);
            std::fflush(stdout);
        }
    }
    if (stats) {
        static auto report = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now();
            now - report >= std::chrono::seconds(5)) {
            report = now;
            std::printf("SRT pipe check (5 s): %llu reads checked, %llu drains "
                        "(pending write %llu, "
                        "label %llu, queued task %llu; old path: overflow %llu, overlap %llu)\n",
                        static_cast<unsigned long long>(pipe_reads),
                        static_cast<unsigned long long>(pipe_drains),
                        static_cast<unsigned long long>(pipe_reasons[0]),
                        static_cast<unsigned long long>(pipe_reasons[1]),
                        static_cast<unsigned long long>(pipe_reasons[2]),
                        static_cast<unsigned long long>(pipe_reasons[3]),
                        static_cast<unsigned long long>(pipe_reasons[4]));
            std::fflush(stdout);
            pipe_reads = pipe_drains = 0;
            pipe_reasons.fill(0);
        }
    }
    // gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1): the clock every 256 walks (once a table had synced it
    // was read on every walk: ~1% of the GPU command thread).
    static u32 stat_walks = 0;
    if (stats && srt_syncs && (!CpuTrims() || (++stat_walks & 255) == 0)) {
        static auto report = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - report >= std::chrono::seconds(5)) {
            report = now;
            std::printf("SRT sync (5 s): %llu tables downloaded before the walk\n",
                        static_cast<unsigned long long>(srt_syncs));
            std::fflush(stdout);
            srt_syncs = 0;
        }
    }
}

void Rasterizer::ReportSrtFault(const Shader::Info& info) {
    static u32 reports = 0;
    if (reports >= 12 || !info.srt_info.walker_func) {
        return;
    }
    ++reports;
    const auto& plan = SrtPlanFor(info);
    const auto describe = [&](const char* when) {
        boost::container::small_vector<VAddr, 16> address(plan.nodes.size(), 0);
        ResolveSrtTables(plan, info, address.data());
        std::string text = fmt::format("SRT fault report ({}): shader {:016x}", when, info.pgm_hash);
        for (size_t i = 0; i < plan.nodes.size(); ++i) {
            const auto& node = plan.nodes[i];
            const VAddr table = address[i];
            text += fmt::format("\n  table {} at {:#x} (+{} in {}{}, {} bytes{})", i, table,
                                node.offset, node.parent < 0 ? "user data" : "table ",
                                node.parent < 0 ? std::string{} : std::to_string(node.parent),
                                node.extent, node.dynamic ? ", computed" : "");
            if (!table || !node.extent) {
                continue;
            }
            const u64 size = std::min<u64>(node.extent, 64);
            if (!IsMapped(table, size)) {
                text += " NOT MAPPED";
                continue;
            }
            text += fmt::format(" gpu-written {}:", buffer_cache.IsRangeGpuWritten(table, node.extent));
            for (u64 off = 0; off + 8 <= size; off += 8) {
                u64 value;
                std::memcpy(&value, reinterpret_cast<const void*>(table + off), sizeof(value));
                text += fmt::format(" {:016x}", value);
            }
        }
        std::printf("%s\n", text.c_str());
        std::fflush(stdout);
    };
    describe("at the walk");
    std::this_thread::sleep_for(std::chrono::milliseconds{3});
    describe("3 ms later");
}

void Rasterizer::DescribeSharpSource(const Shader::Info& info, u32 flat_dw, const char* problem) {
    static std::unordered_map<u64, u32> per_shader;
    static u32 total = 0;
    if (total >= 40 || per_shader[info.pgm_hash]++ >= 4) {
        return;
    }
    ++total;
    const auto ud = info.user_data;
    std::string text = fmt::format("Garbage V# ({}): shader {:016x} flat dword {}", problem,
                                   info.pgm_hash, flat_dw);
    if (flat_dw < info.flattened_ud_buf.size() && flat_dw + 4 <= info.flattened_ud_buf.size()) {
        text += fmt::format(" = {:08x} {:08x} {:08x} {:08x}", info.flattened_ud_buf[flat_dw],
                            info.flattened_ud_buf[flat_dw + 1], info.flattened_ud_buf[flat_dw + 2],
                            info.flattened_ud_buf[flat_dw + 3]);
    }
    text += "; user data";
    for (const u32 v : ud) {
        text += fmt::format(" {:08x}", v);
    }
    if (flat_dw < Shader::NUM_USER_DATA_REGS || !info.srt_info.walker_func) {
        std::printf("%s; from the user data\n", text.c_str());
        std::fflush(stdout);
        return;
    }
    const auto& plan = SrtPlanFor(info);
    boost::container::small_vector<VAddr, 16> address(plan.nodes.size(), 0);
    ResolveSrtTables(plan, info, address.data());
    const auto copy = std::ranges::find(plan.copies, flat_dw, &SrtCopy::flat_dw);
    if (copy == plan.copies.end()) {
        std::printf("%s; source not found in the walker (%zu tables, %zu copies)\n", text.c_str(),
                    plan.nodes.size(), plan.copies.size());
        std::fflush(stdout);
        return;
    }
    for (s32 n = copy->node; n >= 0; n = plan.nodes[n].parent) {
        const auto& node = plan.nodes[n];
        text += fmt::format("; table {} at {:#x} (+{} in {}{}, {} bytes read{})", n, address[n],
                            node.offset, node.parent < 0 ? "user data" : "table ",
                            node.parent < 0 ? std::string{} : std::to_string(node.parent),
                            node.extent, node.dynamic ? ", computed offset" : "");
        if (address[n] && node.extent) {
            text += fmt::format(" gpu-written {}",
                                buffer_cache.IsRangeGpuWritten(address[n], node.extent));
        }
    }
    if (copy->node >= 0 && address[copy->node]) {
        const VAddr src = address[copy->node] + copy->src_off;
        text += fmt::format("; read at {:#x}", src);
        if (IsMapped(src, 16)) {
            const u32* p = reinterpret_cast<const u32*>(src);
            text += fmt::format(" now {:08x} {:08x} {:08x} {:08x} gpu-written {}", p[0], p[1], p[2],
                                p[3], buffer_cache.IsRangeGpuWritten(src, 16));
        }
    }
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

const PreparedStage* Rasterizer::FindPreparedStage(const Shader::Info& stage) const {
    // Sharps a draw-preparation worker read from the same flattened user data.
    if (!bind_prepared || Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)) {
        return nullptr;
    }
    for (u32 i = 0; i < bind_prepared->num_stages; ++i) {
        const auto& candidate = bind_prepared->stages[i];
        if (&candidate.program->info == &stage &&
            candidate.num_images == stage.images.size() &&
            candidate.num_samplers == stage.samplers.size() &&
            candidate.num_buffers == stage.buffers.size()) {
            return &candidate;
        }
    }
    return nullptr;
}

bool Rasterizer::BindHelperWanted() {
    // A spinning helper pays off only with cores to spare (the recording thread spins too).
    const char* env = std::getenv("GOW3_TEXTURE_HELPER");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Measured neutral on 16 threads (docs/parallel_gpu.md): opt-in.
    return false;
}

bool Rasterizer::HelperEligible(const Pipeline* pipeline) const {
    if (pipeline->IsCompute() || !bind_helper.Available() || FrameCapture::Active() ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureBindHelper) ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo) ||
        Gow3Toggle::Disabled(Gow3Toggle::UpdateImageFastPath)) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        // Mip arrays make the descriptor layout depend on the T#s.
        for (const auto& image : stage->images) {
            if (image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
                return false;
            }
        }
        // Storage writes invalidate cached images from the buffer side.
        for (const auto& buffer : stage->buffers) {
            if (!buffer.IsSpecial() && buffer.is_written) {
                return false;
            }
        }
    }
    return true;
}

bool Rasterizer::TexturesBindableOnHelper(const Shader::Info& stage,
                                          const PreparedStage* prepared) {
    const u64 generation = texture_cache.RegistryGeneration();
    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        if (image_desc.is_written) {
            return false;
        }
        const auto tsharp =
            prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
            !memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt) || !PlausibleTSharp(tsharp)) {
            continue; // null descriptor
        }
        const auto& entry =
            prepared ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                     : CachedImageDescEntry(tsharp, image_desc);
        if (entry.found_generation != generation ||
            !PlausibleImage(entry.desc.info, tsharp, memory, instance.GetMaxImageArrayLayers())) {
            return false;
        }
        VideoCore::ImageId image_id = entry.found_id;
        auto* image = &texture_cache.GetImage(image_id);
        if (const auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
            image_id = depth_image_id;
            image = &texture_cache.GetImage(image_id);
        }
        // HD UI textures: a native-size read drops the replacement (FindTexture), which changes
        // image state: on the GPU command thread, not here.
        if (image->binding.needs_rebind || !texture_cache.IsUpToDate(image_id) ||
            ((image_desc.needs_native || image_desc.unnormalized_sampler) && image->UsingHd()) ||
            scene_targets->Tracks(image->image_uid) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
            return false;
        }
    }
    return true;
}

void Rasterizer::RunTextureTask(void* context) {
    auto& self = *static_cast<Rasterizer*>(context);
    auto& task = self.texture_task;
    for (u32 i = 0; i < task.count; ++i) {
        auto& stage = task.stages[i];
        if (!self.TexturesBindableOnHelper(*stage.info, stage.prepared)) {
            return;
        }
        auto binding = stage.binding;
        u32 write_index = stage.write_index;
        self.BindTextures(*stage.info, stage.prepared, binding, write_index, task.barrier, true);
        task.completed = i + 1;
    }
}

void Rasterizer::JoinBindHelper(void* context) {
    if (!BindHelper::OnHelper()) {
        static_cast<Rasterizer*>(context)->bind_helper.Join();
    }
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();
    buffer_cache.PollReadbacks();
    // gow3: G-buffer passes sample material textures for a scene rendered below the output
    // size; the temporal upscaler restores the detail of the output's mip level (FSR guide:
    // log2(render / output)). Shadows, post-processing and UI keep the guest's bias.
    sampler_lod_bias = 0.0f;
    if (!pipeline->IsCompute() && !Gow3Toggle::Disabled(Gow3Toggle::SceneMipBias) &&
        (std::popcount(static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey().mrt_mask &
                       0xff) >= 5 ||
         flat_scene_draw)) {
        sampler_lod_bias = upscaler->SceneMipBias();
        static float reported = 0.0f;
        if (sampler_lod_bias != reported) {
            reported = sampler_lod_bias;
            std::printf("Upscaler: scene texture LOD bias %.2f\n", sampler_lod_bias);
        }
    }
    // gow3 21 Oct 2026: the anisotropy setting is live (menu), so it is read per call; compute
    // dispatches keep the game's samplers. The forced S# changes the sampler cache's key, so no
    // descriptor memo holds an old sampler (the texture set memos remember images only).
    sampler_graphics = !pipeline->IsCompute();
    sampler_forced_aniso = 0;
    if (sampler_graphics && instance.IsAnisotropicFilteringSupported() &&
        !Gow3Toggle::Disabled2(Gow3Toggle::ForcedAniso)) {
        const int wanted = Gow3Settings::Get().anisotropy.load(std::memory_order_relaxed);
        const u32 forced = static_cast<u32>(
            std::clamp(wanted, 0, static_cast<int>(instance.MaxSamplerAnisotropy())));
        sampler_forced_aniso = forced >= 2 ? forced : 0;
    }

    bool uses_dma = false;
    const Shader::Info* dma_stage = nullptr;

    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    if (stats && ((helper_full + helper_partial + helper_serial) & 1023) == 0) {
        static auto window = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - window >= std::chrono::seconds(5)) {
            window = now;
            const double forks = std::max<double>(1, helper_full + helper_partial);
            std::printf("Texture helper: %llu binds in parallel, %llu partly, %llu serial; "
                        "per fork %.0f cycles helper task, %.0f cycles GPU thread wait\n",
                        static_cast<unsigned long long>(helper_full),
                        static_cast<unsigned long long>(helper_partial),
                        static_cast<unsigned long long>(helper_serial),
                        bind_helper.task_cycles.exchange(0) / forks,
                        bind_helper.wait_cycles / forks);
            bind_helper.wait_cycles = 0;
            helper_full = helper_partial = helper_serial = 0;
        }
    }

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(Regs());
    if (!HelperEligible(pipeline)) {
        ++helper_serial;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                              stage->samplers.size());
            stage->PushUd(binding, push_data);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            BindBuffers(*stage, prepared, binding, push_data, set_write_index);
            BindTextures(*stage, prepared, binding, set_write_index, needs_barrier);
            uses_dma |= stage->uses_dma;
            if (stage->uses_dma) {
                dma_stage = stage;
            }
        }
    } else {
        // gow3: textures on the helper, buffers here. Without mip arrays every stage's
        // descriptors are [buffers][images][samplers], one each, so both sides know their
        // binding numbers and descriptor write slots up front.
        struct BufferStage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<BufferStage, Shader::MaxStageTypes> buffer_stages;
        auto& task = texture_task;
        task.count = 0;
        task.completed = 0;
        task.barrier = false;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            const u32 num_buffers = static_cast<u32>(stage->buffers.size());
            const u32 num_descriptors =
                num_buffers + static_cast<u32>(stage->images.size() + stage->samplers.size());
            set_writes.resize(set_writes.size() + num_descriptors);
            stage->PushUd(binding, push_data);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            buffer_stages[task.count] = {stage, prepared, binding, set_write_index};
            auto texture_binding = binding;
            texture_binding.buffer += num_buffers;
            texture_binding.unified += num_buffers;
            task.stages[task.count] = {stage, prepared, texture_binding,
                                       set_write_index + num_buffers};
            ++task.count;
            binding.buffer += num_buffers;
            binding.unified += num_descriptors;
            set_write_index += num_descriptors;
            uses_dma |= stage->uses_dma;
            if (stage->uses_dma) {
                dma_stage = stage;
            }
        }
        bind_helper.Fork(&RunTextureTask, this);
        for (u32 i = 0; i < task.count; ++i) {
            auto& stage = buffer_stages[i];
            BindBuffers(*stage.info, stage.prepared, stage.binding, push_data, stage.write_index);
        }
        if (draw_inputs.pending && !Gow3Toggle::Disabled(Gow3Toggle::EarlyDrawInputs)) {
            ResolveVertexBuffers(draw_inputs.pipeline, draw_inputs.prepared);
            if (draw_inputs.is_indexed) {
                ResolveIndexBuffer(draw_inputs.index_offset);
            }
            draw_inputs.resolved = true;
        }
        bind_helper.Join();
        needs_barrier |= task.barrier;
        ++(task.completed == task.count ? helper_full
                                        : task.completed ? helper_partial : helper_serial);
        // Stages the helper left (an image needing work that records commands).
        for (u32 i = task.completed; i < task.count; ++i) {
            auto& stage = task.stages[i];
            u32 write_index = stage.write_index;
            BindTextures(*stage.info, stage.prepared, stage.binding, write_index, needs_barrier);
        }
    }

    if (uses_dma) {
        SyncDmaMemory(dma_stage);
        fault_process_pending = true;
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared) {
    ResolveVertexBuffers(pipeline, prepared);
    EmitVertexBuffers();
}

void Rasterizer::EmitVertexBuffers() {
    // gow3: only the used entries go into the recording chunk (the static vectors hold 32 of
    // each: ~3 KiB copied per draw).
    auto& v = vertex_binds;
    const bool dynamic_input = instance.IsVertexInputDynamicState();
    const u32 num_buffers = v.num_buffers;
    scheduler.ReserveRecordData(
        v.bindings.size() * sizeof(v.bindings[0]) + v.attributes.size() * sizeof(v.attributes[0]) +
        num_buffers * (sizeof(vk::Buffer) + 3 * sizeof(vk::DeviceSize)) + 128);
    if (dynamic_input) {
        scheduler.Record([bindings = scheduler.RecordData(std::span<const vk::VertexInputBindingDescription2EXT>{v.bindings.data(), v.bindings.size()}),
                          attributes = scheduler.RecordData(std::span<const vk::VertexInputAttributeDescription2EXT>{
                              v.attributes.data(), v.attributes.size()})](
                             vk::CommandBuffer cmdbuf) {
            cmdbuf.setVertexInputEXT(bindings, attributes);
        });
    }
    if (num_buffers == 0) {
        return;
    }
    const auto buffers = scheduler.RecordData(std::span<const vk::Buffer>{v.host_buffers.data(), num_buffers});
    const auto offsets = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_offsets.data(), num_buffers});
    if (dynamic_input) {
        scheduler.Record([num_buffers, buffers, offsets](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindVertexBuffers(0, num_buffers, buffers.data(), offsets.data());
        });
        return;
    }
    const auto sizes = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_sizes.data(), num_buffers});
    const auto strides = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_strides.data(), num_buffers});
    scheduler.Record([num_buffers, buffers, offsets, sizes, strides](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindVertexBuffers2(0, num_buffers, buffers.data(), offsets.data(), sizes.data(),
                                  strides.data());
    });
}

// gow3 diagnostics (GOW3_CLOTH_PEEK=1): who reads the cloth solvers' outputs (1c957b01,
// 809864d3), and whether what they read is the newest output (the solver's latest dispatch), an
// older one, or no cloth output at all (for readers that read cloth outputs at other times).
// slot ~0u with compute = a solver dispatch starts. Recording thread only (mutex anyway).
// Returns true when a read hits the newest output of a solver.

static bool ClothTrace(u64 shader, u32 slot, VAddr address, u64 size, bool written, bool compute) {
    if (!ClothTraceEnabled()) {
        return false;
    }
    bool newest = false;
    struct Output {
        VAddr end;
        u64 shader, seq;
        u32 slot;
    };
    static std::mutex mutex;
    static std::map<VAddr, Output> outputs;
    static std::map<u64, u64> latest;
    // (reader, slot) -> solver/slot -> newest, previous, older; readers' binds without cloth data
    static std::map<std::pair<u64, u32>, std::map<std::pair<u64, u32>, std::array<u64, 8>>> reads;
    static std::map<std::pair<u64, u32>, u64> misses;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    const bool solver = compute && (shader == 0x1c957b01 || shader == 0x809864d3);
    if (solver && slot == ~0u) {
        ++latest[shader];
        return false;
    }
    if (address == 0 || size == 0 || slot == ~0u) {
        return false;
    }
    const VAddr end = address + size;
    auto it = outputs.upper_bound(address);
    if (it != outputs.begin()) {
        --it;
    }
    if (written) {
        // Anything written here replaces cloth outputs it overlaps.
        while (it != outputs.end() && it->first < end) {
            if (it->second.end > address) {
                it = outputs.erase(it);
            } else {
                ++it;
            }
        }
        if (solver) {
            outputs[address] = {end, shader, latest[shader], slot};
        }
    } else {
        bool hit = false;
        for (; it != outputs.end() && it->first < end; ++it) {
            if (it->second.end <= address || (solver && it->second.shader == shader)) {
                continue;
            }
            hit = true;
            const u64 age = latest[it->second.shader] - it->second.seq;
            newest |= age == 0;
            ++reads[{shader, slot}][{it->second.shader, it->second.slot}][age < 5 ? age : age < 10 ? 5 : age < 100 ? 6 : 7];
        }
        if (!hit && reads.contains({shader, slot})) {
            ++misses[{shader, slot}];
        }
    }
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
        last = now;
        std::string text;
        for (auto& [reader, sources] : reads) {
            text += fmt::format(" {:08x}/s{}:", reader.first, reader.second);
            for (auto& [source, n] : sources) {
                text += fmt::format(" {:08x}/s{} ages 0-4 {} {} {} {} {}, 5-9 {}, 10-99 {}, 100+ {};", source.first,
                                    source.second, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
                n = {};
            }
            if (const auto miss = misses.find(reader); miss != misses.end() && miss->second) {
                text += fmt::format(" no cloth data {};", miss->second);
                miss->second = 0;
            }
        }
        std::printf("Cloth readers (5 s):%s\n", text.c_str());
        std::fflush(stdout);
    }
    return newest;
}


static void NoteClothRead(u64 shader, u32 slot, VAddr address, u64 size, u64 tracked, bool hazard,
                          u32 epoch) {
    static u64 protected_reads = 0, flushed_reads = 0, missed = 0;
    static int lines = 0;
    static auto last = std::chrono::steady_clock::now();
    if (epoch != g_cloth_epoch) {
        ++flushed_reads;
    } else if (hazard) {
        ++protected_reads;
    } else {
        ++missed;
        if (lines < 40) {
            ++lines;
            std::printf("Cloth MISSED HAZARD: reader %08llx slot %u at %#llx size %llu tracked %llu "
                        "after %08llx\n",
                        (unsigned long long)shader, slot, (unsigned long long)address,
                        (unsigned long long)size, (unsigned long long)tracked,
                        (unsigned long long)g_cloth_epoch_shader);
        }
    }
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
        last = now;
        std::printf("Cloth newest reads (5 s): %llu with a barrier pending, %llu after a flush, %llu missed\n",
                    (unsigned long long)protected_reads, (unsigned long long)flushed_reads,
                    (unsigned long long)missed);
        std::fflush(stdout);
        protected_reads = flushed_reads = missed = 0;
    }
}

void Rasterizer::ResolveVertexBuffers(const GraphicsPipeline* pipeline,
                                      const PreparedDraw* prepared) {
    const auto& regs = Regs();
    auto& v = vertex_binds;
    v.num_buffers = 0;
    v.host_buffers.clear();
    v.host_offsets.clear();
    v.host_sizes.clear();
    v.host_strides.clear();
    auto& attributes = v.attributes;
    auto& bindings = v.bindings;
    attributes.clear();
    bindings.clear();
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    // Inputs a draw-preparation worker read from the same registers and (verified) user data.
    const auto& fetch = pipeline->GetFetchShader();
    const PreparedVertexInputs* ready =
        prepared && prepared->vertex.valid && instance.IsVertexInputDynamicState() && fetch &&
                fetch->attributes.size() == prepared->vertex.count && !FrameCapture::Active() &&
                !Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)
            ? &prepared->vertex
            : nullptr;
    if (ready) {
        attributes.assign(ready->attributes, ready->attributes + ready->count);
        bindings.assign(ready->bindings, ready->bindings + ready->count);
        guest_buffers.assign(ready->buffers, ready->buffers + ready->count);
    } else {
        pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                                  regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }
    if (motion_draw && !guest_buffers.empty()) {
        // Include every stream: identical position buffers can be paired with different
        // skinning/instance data. Index topology and base offsets are matched separately.
        motion_geometry = ready ? ready->buffers_hash
                                : XXH3_64bits(guest_buffers.data(),
                                              guest_buffers.size() * sizeof(AmdGpu::Buffer));
    }
    // Object motion research: vertex streams of G-buffer draws.
    if (FrameCapture::Active() && gbuffer_draw) {
        for (const auto& vb : guest_buffers) {
            char note[128];
            std::snprintf(note, sizeof(note), "\n  gb vb at %#llx size %u stride %u",
                          (unsigned long long)vb.base_address, u32(vb.GetSize()),
                          u32(vb.GetStride()));
            FrameCapture::Note(note);
        }
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    {
        const u64 vs = pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash;
        for (u32 i = 0; i < guest_buffers.size(); ++i) {
            ClothTrace(vs, 100 + i, guest_buffers[i].base_address, guest_buffers[i].GetSize(), false, false);
        }
    }

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    VertexInputs<BufferRange> ranges_merged{};
    if (ready) {
        for (u32 i = 0; i < ready->num_ranges; ++i) {
            ranges_merged.emplace_back(ready->ranges[i].base, ready->ranges[i].end);
        }
    } else {
        for (const auto& buffer : guest_buffers) {
            if (buffer.base_address != 0 && buffer.GetSize() > 0) {
                ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
            }
        }
    }

    // gow3 diagnostics (GOW3_TRACE_BIND_RANGE=<lo>:<hi> hex): vertex buffers in the range, by
    // vertex shader, 5 s statistics.
    {
        static const std::pair<u64, u64> trace_range = [] {
            const char* env = std::getenv("GOW3_TRACE_BIND_RANGE");
            if (!env) {
                return std::pair<u64, u64>{0, 0};
            }
            char* end = nullptr;
            const u64 lo = std::strtoull(env, &end, 16);
            const u64 hi = end && *end == ':' ? std::strtoull(end + 1, nullptr, 16) : 0;
            return std::pair<u64, u64>{lo, hi};
        }();
        if (trace_range.second > trace_range.first) {
            for (const auto& buffer : guest_buffers) {
                if (buffer.base_address < trace_range.second &&
                    buffer.base_address + buffer.GetSize() > trace_range.first) {
                    static std::mutex mutex;
                    static std::map<std::pair<u64, u64>, u32> seen;
                    static auto last = std::chrono::steady_clock::now();
                    std::scoped_lock lock{mutex};
                    ++seen[{pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                            pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash}];
                    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                        last = now;
                        std::string text;
                        for (const auto& [key, n] : seen) {
                            text += fmt::format(" vs {:08x} ps {:08x} x{}", key.first, key.second, n);
                        }
                        std::printf("Vertex trace (5 s):%s (e.g. %#llx size %u stride %u)\n", text.c_str(),
                                    static_cast<unsigned long long>(buffer.base_address),
                                    unsigned(buffer.GetSize()), unsigned(buffer.GetStride()));
                        std::fflush(stdout);
                        seen.clear();
                    }
                    break;
                }
            }
        }
    }

    // Merge connecting ranges together
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
    }

    // Bind vertex buffers
    auto& host_buffers = v.host_buffers;
    auto& host_offsets = v.host_offsets;
    auto& host_sizes = v.host_sizes;
    auto& host_strides = v.host_strides;
    u32 stream_with_memory = 0;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                ready ? ranges_merged.begin() + ready->range_index[stream_with_memory++]
                      : std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                            return buffer.base_address >= range.base_address &&
                                   buffer.base_address < range.end_address;
                        });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    v.num_buffers = static_cast<u32>(guest_buffers.size());
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    ResolveIndexBuffer(index_offset);
    EmitIndexBuffer();
}

void Rasterizer::EmitIndexBuffer() {
    scheduler.Record([handle = index_bind.handle, offset = index_bind.offset,
                      type = index_bind.type](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindIndexBuffer(handle, offset, type);
    });
}

void Rasterizer::ResolveIndexBuffer(u32 index_offset) {
    const auto& regs = Regs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    index_bind = {buffer->Handle(), offset, index_type};
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    // gow3 17 Oct 2026: overlapping or adjacent ranges of one buffer with the same access are
    // tracked as one (the same tracker state, fewer inserts): Uncharted 3's crowd skinning shader
    // binds each output array through views at +0/+12/+20, ~1,500 dispatches a frame. Compute
    // only: sorting every draw's bindings cost Uncharted 1 and 2 (draw-heavy) 3-4%.
    if (is_compute && bound_buffers.size() > 1) {
        std::sort(bound_buffers.begin(), bound_buffers.end(), [](const BoundBuffer& a, const BoundBuffer& b) {
            return std::tie(a.buffer, a.is_written, a.offset) < std::tie(b.buffer, b.is_written, b.offset);
        });
        size_t out = 0;
        for (size_t i = 1; i < bound_buffers.size(); ++i) {
            auto& last = bound_buffers[out];
            const auto& next = bound_buffers[i];
            if (next.buffer == last.buffer && next.is_written == last.is_written &&
                next.offset <= last.offset + last.size) {
                const u64 end = std::max<u64>(last.offset + last.size, next.offset + next.size);
                last.size = static_cast<u32>(end - last.offset);
            } else {
                bound_buffers[++out] = next;
            }
        }
        bound_buffers.resize(out + 1);
    }
    for (const auto [buffer, offset, size, is_written] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        const auto write_flag =
            is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
        runtime.AccessBuffer(buffer, offset, size, dst_stage,
                             vk::AccessFlagBits2::eShaderRead | write_flag);
    }
    for (const auto* arena : dma_read_arenas) {
        runtime.AccessBuffer(arena, 0, arena->SizeBytes(),
                             is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                        : vk::PipelineStageFlagBits2::eAllGraphics,
                             vk::AccessFlagBits2::eShaderRead);
    }
    dma_read_arenas.clear();
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // gow3 22 Oct 2026 (Gow3Toggle::DispatchBindFastPaths): the answer is true only when a
    // written buffer is a metadata surface (and no read one is), so the written ones are looked
    // at first. U3 meat market profile: this check was 4% of the draw recording thread, a V# read
    // and lookup per binding; cs 8f067540 (~1,530 dispatches a frame) has 6 read and 3 written.
    if (!Gow3Toggle::Disabled2(Gow3Toggle::DispatchBindFastPaths)) {
        // The XOR heuristic below: such a shader is never taken for a clear.
        bool meta_write = false;
        for (u32 i = 0; i < info.buffers.size() && !info.has_bitwise_xor && !meta_write; ++i) {
            const auto& desc = info.buffers[i];
            meta_write = !desc.IsSpecial() && desc.is_written &&
                         texture_cache.IsMeta(desc.GetSharp(info).base_address).has_value();
        }
        if (!meta_write) {
            VideoCore::CountFastPath(VideoCore::g_bind_fast_paths.meta_clear_skips);
            return false;
        }
        VideoCore::CountFastPath(VideoCore::g_bind_fast_paths.meta_clear_checks);
    }

    // gow3: the cheap descriptor tests come before reading the V# (GetSharp: user data or the
    // flattened SRT, and for the effect shaders a mapped-memory query) on every dispatch; a
    // shader writing no guest buffer cannot be a metadata clear.
    bool writes_buffer = false;
    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        if (desc.IsSpecial()) {
            continue;
        }
        if (desc.is_written) {
            writes_buffer = true;
            continue;
        }
        const VAddr address = desc.GetSharp(info).base_address;
        if (texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor && writes_buffer) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            if (desc.IsSpecial() || !desc.is_written) {
                continue;
            }
            const VAddr address = desc.GetSharp(info).base_address;
            if (texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

static bool TracedShader(u64 hash);

// gow3 diagnostics (GOW3_CLOTH_CHECK=1): the CPU-written inputs of Uncharted 3's cloth compute
// shaders (cs 1c957b01, 809864d3; Sully's shirt flickers as exploding triangles above 30 FPS,
// 16 Oct) are scanned at each dispatch for non-finite or huge floats; hits are printed with the
// frame number, plus a 5 s summary of dispatches and buffers checked.
static void CheckClothInputs(Core::MemoryManager* memory, VideoCore::BufferCache& cache, u64 hash,
                             u32 slot, VAddr address, u64 size, bool ring) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_CLOTH_CHECK");
        return env && env[0] == '1';
    }();
    if (!enabled || (hash != 0x1c957b01 && hash != 0x809864d3) || !address || !size) {
        return;
    }
    static std::mutex mutex;
    static u64 checked = 0, bad_buffers = 0, gpu_written = 0;
    static int printed = 0;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    const u64 frame = Gow3Stats::gpu_frames.load(std::memory_order_relaxed);
    if (!ring && cache.IsRegionGpuModified(address, size)) {
        ++gpu_written; // GPU data: guest memory does not hold it
    } else if (size <= 64_KB && memory->IsValidGpuMapping(address, size)) {
        ++checked;
        static std::vector<float> values;
        values.resize(size / 4);
        memory->CopySparseMemory(address, reinterpret_cast<u8*>(values.data()), values.size() * 4);
        u32 bad = 0, first = 0;
        for (u32 i = 0; i < values.size(); ++i) {
            const float v = values[i];
            const u32 bits = std::bit_cast<u32>(v);
            // Integers stored as floats look like denormals or tiny values: only NaN/inf and
            // magnitudes over 1e6 that are not plain integers count.
            const bool nonfinite = ((bits >> 23) & 0xff) == 0xff;
            const bool huge = std::isfinite(v) && std::abs(v) > 1e6f && (bits & 0xff000000u) != 0;
            if (nonfinite || huge) {
                if (!bad) {
                    first = i;
                }
                ++bad;
            }
        }
        if (bad) {
            ++bad_buffers;
            if (printed < 200) {
                ++printed;
                std::printf("Cloth check: frame %llu shader %08llx slot %u%s at %#llx size %llu: %u bad "
                            "floats, first at +%u = %g (%08x)\n",
                            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(hash), slot,
                            ring ? " (ring)" : "", static_cast<unsigned long long>(address),
                            static_cast<unsigned long long>(size), bad, first * 4, values[first],
                            std::bit_cast<u32>(values[first]));
            }
        }
    }
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
        last = now;
        std::printf("Cloth check (5 s): %llu input buffers checked, %llu with bad floats, %llu GPU-written\n",
                    static_cast<unsigned long long>(checked), static_cast<unsigned long long>(bad_buffers),
                    static_cast<unsigned long long>(gpu_written));
        std::fflush(stdout);
        checked = bad_buffers = gpu_written = 0;
    }
}

// gow3 16 Oct: Uncharted 2's aim depth of field focuses on the depth under the crosshair: ps
// 9f801363 samples a 24-pixel box of the scene depth at uv = c[6] * (ndc * c[2] + 0.5) + c[0]
// (and 1 - c[1] + ... for v), c = 8 floats the game writes per frame. c[0], c[1] are the crosshair
// in pixels of the HUD's 1920x1080 space times 1 / render size (c[6], c[7]): with our render
// resolution above 1080p the box left the crosshair (4K: uv 0.25 instead of 0.5, the depth of the
// wall beside Nate) and everything behind the target was blurred while aiming. The centre is
// corrected to the HUD space for every render size (no change at 1080p). GOW3_AIM_FOCUS_FIX=0: off.
static void FixAimFocusCentre(Core::MemoryManager* memory, VideoCore::BufferCache* buffer_cache,
                              VAddr address) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_AIM_FOCUS_FIX");
        return !(env && env[0] == '0');
    }();
    // gow3 20 Oct 2026: what the fix did per 5 s (calls, skips by reason, corrections, writes
    // through to the GPU's copy), printed while the shader runs.
    static std::atomic<u32> n_calls{0}, n_unmapped{0}, n_odd{0}, n_native{0}, n_same{0}, n_fixed{0},
        n_through{0};
    static std::atomic<s64> n_last{0};
    ++n_calls;
    if (const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        now - n_last.load() >= 5) {
        n_last = now;
        std::printf("Aim focus (5 s): %u binds; skipped %u unmapped, %u not the box constants, %u at "
                    "1080p, %u already corrected; %u corrected (%u through to the GPU copy)\n",
                    n_calls.exchange(0), n_unmapped.exchange(0), n_odd.exchange(0), n_native.exchange(0),
                    n_same.exchange(0), n_fixed.exchange(0), n_through.exchange(0));
        std::fflush(stdout);
    }
    if (!enabled || !memory->IsValidGpuMapping(address, 32)) {
        ++n_unmapped;
        return;
    }
    float c[8];
    memory->CopySparseMemory(address, reinterpret_cast<u8*>(c), sizeof(c));
    const float inv_width = c[6], inv_height = c[7];
    if (!(inv_width > 0.0f && inv_width < 1.0f && inv_height > 0.0f && inv_height < 1.0f)) {
        ++n_odd;
        return;
    }
    const float x_scale = 1920.0f * inv_width, y_scale = 1080.0f * inv_height;
    if (std::abs(x_scale - 1.0f) < 1e-4f && std::abs(y_scale - 1.0f) < 1e-4f) {
        ++n_native;
        return; // 1080p: already right
    }
    // The block may be bound by more than one draw: values corrected once stay as they are.
    static std::mutex mutex;
    // Merge of upstream 0.5.0 (21 Oct 2026): `through` is false when the fork's ring path
    // (stage A, no buffer cache) corrected the guest copy only; the next direct bind writes the
    // corrected values through to the GPU copy without correcting them again.
    struct Corrected {
        float x, y;
        bool through;
    };
    static std::unordered_map<VAddr, Corrected> corrected;
    std::scoped_lock lock{mutex};
    if (const auto it = corrected.find(address);
        it != corrected.end() && it->second.x == c[0] && it->second.y == c[1]) {
        if (!it->second.through && buffer_cache) {
            const float values[2] = {c[0], c[1]};
            it->second.through = buffer_cache->WriteThrough(address, values, sizeof(values));
            n_through += it->second.through;
        }
        ++n_same;
        return;
    }
    ++n_fixed;
    const float fixed[2] = {c[0] / x_scale, c[1] / y_scale};
    // gow3 20 Oct 2026: written through to the GPU's copy as well (BufferCache::WriteThrough).
    // A write to the backing view alone is not seen by the page tracking: when another draw had
    // already uploaded the block in that frame, the GPU kept the uncorrected centre. The user
    // saw the aim blur again in Uncharted 2 at 4K: the focus read back alternated between the
    // target's 2.1 m and 5.64 m, the wall beside Nate.
    const bool through = buffer_cache && buffer_cache->WriteThrough(address, fixed, sizeof(fixed));
    if (through) {
        ++n_through;
    } else {
        memory->TryWriteBacking(reinterpret_cast<void*>(address), fixed, sizeof(fixed));
    }
    if (corrected.size() >= 512) {
        corrected.clear();
    }
    corrected[address] = {fixed[0], fixed[1], through};
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::printf("Aim focus: crosshair box centre %.4f,%.4f -> %.4f,%.4f (render %ux%u)\n", c[0], c[1],
                    fixed[0], fixed[1], u32(1.0f / inv_width + 0.5f), u32(1.0f / inv_height + 0.5f));
        std::fflush(stdout);
    }
}

void Rasterizer::BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                             Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data, u32& write_index) {
    const u64 alignment = instance.StorageMinAlignment();
    const bool compute_stage = stage.hw_stage == Shader::HwStage::Compute;
    // 23 Oct profile (prof_st1): ObtainBuffer is 15.4% of stage B, GpuRangesAdd 4.7%.
    // The crowd kernel writes position/normal/tangent through overlapping views of one output
    // allocation (+0/+12/+20). Acquire/synchronize their exact contiguous union once, not three
    // times. This memo lasts ONE dispatch: no cached residency, content or lifetime assumptions.
    struct WriteGroup {
        const VideoCore::Buffer* buffer = nullptr;
        VAddr address = 0;
        u64 size = 0, offset = 0;
    } write_group;
    static const bool batch_writes = [] {
        const char* env = std::getenv("GOW3_SKINNING_WRITE_BATCH");
        return env && env[0] == '1';
    }();
    const bool coalesce = batch_writes && compute_stage && stage.pgm_hash == 0x8f067540 &&
                          !Gow3Toggle::Disabled2(Gow3Toggle::CoalescedSkinningWrites);
    static u64 write_groups = 0, write_group_refused = 0, write_group_bytes = 0;
    ClothTrace(stage.pgm_hash, ~0u, 0, 0, false, compute_stage);
    for (u32 buffer_index = 0; buffer_index < stage.buffers.size(); ++buffer_index) {
        const auto& desc = stage.buffers[buffer_index];
        const RingBinding* ring = num_ring_stages ? FindRingBinding(stage, buffer_index) : nullptr;
        if (!desc.IsSpecial() && !desc.is_written &&
            (stage.pgm_hash == 0x1c957b01 || stage.pgm_hash == 0x809864d3)) {
            if (ring) {
                CheckClothInputs(memory, buffer_cache, stage.pgm_hash, buffer_index, ring->address,
                                 ring->size, true);
            } else {
                const auto vsharp = prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
                CheckClothInputs(memory, buffer_cache, stage.pgm_hash, buffer_index, vsharp.base_address,
                                 vsharp.GetSize(), false);
            }
        }
        if (TracedShader(stage.pgm_hash) && !desc.IsSpecial()) {
            static std::atomic<int> traced{0};
            if (traced.fetch_add(1) < 40) {
                VAddr address = 0;
                u64 size = 0;
                if (ring) {
                    address = ring->address;
                    size = ring->size;
                } else {
                    const auto vsharp = prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
                    address = vsharp.base_address;
                    size = vsharp.GetSize();
                }
                std::string text = fmt::format("PS trace buffer: shader {:08x} slot {} at {:#x} size {}{}:",
                                               stage.pgm_hash, buffer_index, address, size, ring ? " (ring)" : "");
                if (address && memory->IsValidGpuMapping(address, 0)) {
                    float values[16]{};
                    memory->CopySparseMemory(address, reinterpret_cast<u8*>(values),
                                             std::min<u64>(size, sizeof(values)));
                    for (u32 i = 0; i < std::min<u64>(size / 4, 16); ++i) {
                        text += fmt::format(" {}", values[i]);
                    }
                }
                std::printf("%s\n", text.c_str());
                std::fflush(stdout);
            }
        }
        if (ring) {
            const bool persistent = (ring->offset & StaticMeshOffsetFlag) != 0;
            const u64 offset = ring->offset & ~StaticMeshOffsetFlag;
            const u8* snapshot = persistent ? static_mesh_buffer->mapped_data.data() + offset
                                            : constant_ring->Data(offset);
            // Uncharted 2's camera in the vertex user data: with multithreaded rendering the flat
            // user data reaches this stage as a ring copy made at stage A (the stage's own copy
            // may already hold a later draw's), so the camera is read from the ring.
            if (desc.buffer_type == Shader::BufferType::Flatbuf && flat_camera_depth &&
                stage.sw_stage == Shader::SwStage::Vertex &&
                camera_motion->OnFlatConstants(
                    reinterpret_cast<const u32*>(snapshot),
                    ring->size / sizeof(u32), flat_camera_depth)) {
                flat_camera_depth = {};
            }
            // Copied by the GPU command thread into the constant ring (read-only).
            if (FrameCapture::Active() && memory->IsValidGpuMapping(ring->address, 0)) {
                FrameCapture::Buffer(stage.pgm_hash, buffer_index, ring->address,
                                      snapshot, ring->size);
            }
            if (!desc.IsSpecial()) {
                if (compute_stage && DispatchTraceCapturing()) [[unlikely]] {
                    DispatchTraceBind(buffer_index, desc, AmdGpu::Buffer{}, ring->address, ring->size,
                                       persistent ? "static-mesh-snapshot" : "constant-ring-copy",
                                       nullptr, 0, offset);
                }
                if (ring->size == 864 && gbuffer_draw &&
                    memory->IsValidGpuMapping(ring->address, 0)) {
                    camera_motion->OnConstants(reinterpret_cast<const float*>(ring->address));
                }
                push_data.AddOffset(binding.buffer, 0);
            }
            buffer_infos.emplace_back(persistent ? static_mesh_buffer->Handle() : constant_ring->Handle(),
                                      offset, ring->size);
        } else if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |= runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.FlatUserData().size() * sizeof(u32);
                if (FrameCapture::Active()) {
                    FrameCapture::Buffer(stage.pgm_hash, binding.buffer, 0,
                                         stage.FlatUserData().data(), ubo_size);
                }
                if (flat_camera_depth && stage.sw_stage == Shader::SwStage::Vertex &&
                    camera_motion->OnFlatConstants(stage.FlatUserData().data(),
                                                   stage.FlatUserData().size(),
                                                   flat_camera_depth)) {
                    flat_camera_depth = {};
                }
                const u64 offset =
                    vk_buffer.Copy(stage.FlatUserData().data(), ubo_size, alignment);
                if (LateConstantsEnabled()) {
                    NoteLateFlatCopy(vk_buffer, offset, stage, ubo_size, 0);
                }
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (Regs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = Regs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = CsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp =
                prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
            if (vsharp.GetSize() == 864 && gbuffer_draw &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                camera_motion->OnConstants(reinterpret_cast<const float*>(vsharp.base_address));
            }
            // Object motion research: the vertex shader buffers of G-buffer draws.
            if (FrameCapture::Active() && gbuffer_draw && stage.sw_stage == Shader::SwStage::Vertex &&
                vsharp.base_address != 0 && memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                const auto* f = reinterpret_cast<const float*>(vsharp.base_address);
                char note[192];
                std::snprintf(note, sizeof(note),
                              "\n  gb vs %08x slot %u at %#llx size %u stride %u: %g %g %g %g",
                              u32(stage.pgm_hash), binding.buffer,
                              (unsigned long long)vsharp.base_address, u32(vsharp.GetSize()),
                              u32(vsharp.GetStride()), f[0], f[1], f[2], f[3]);
                FrameCapture::Note(note);
            }
            if (FrameCapture::Active() && vsharp.base_address != 0 && vsharp.GetSize() != 0 &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                FrameCapture::Buffer(stage.pgm_hash, binding.buffer, vsharp.base_address,
                                     reinterpret_cast<const void*>(vsharp.base_address),
                                     memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize()));
            }
            // gow3: garbage V#s. Uncharted 2's treasure effect shaders (cs 7d7e7206, 8f067540)
            // fetch V#s from table slots that hold floats for that effect (CPU-written: the
            // branches using them are not taken). Bound, they span random guest memory: written
            // ones made gigabytes of game data "GPU-modified" (stale GPU copies written back over
            // it: the game's fg-draw-mgr assert), and the arena over them ran out of device
            // memory. A V# reaching past mapped memory, other than an unbounded raw buffer
            // (num_records ~0), or spanning 1 GiB is bound as null. Normal play has none.
            const GarbageSharp gs = ClassifyGarbageSharp(memory, vsharp, stage.pgm_hash);
            const bool unbound = gs.unbound;
            const u64 mapped_size = gs.mapped_size;
            const bool garbage = gs.garbage;
            if (garbage) {
                static u32 garbage_reports = 0;
                if (garbage_reports < 32) {
                    ++garbage_reports;
                    LOG_ERROR(Render, "Ignoring a garbage V# for stage {:#x}: {:#x}, {} of {} bytes "
                              "mapped, num_records {}, stride {}", stage.pgm_hash,
                              vsharp.base_address, mapped_size, vsharp.GetSize(),
                              vsharp.num_records, vsharp.stride);
                }
                if (!prepared && (desc.sharp_fetch.load_mask & 1)) {
                    DescribeSharpSource(stage, desc.sharp_fetch.offsets[0], "garbage");
                }
            }
            if (unbound || garbage) {
                if (compute_stage && DispatchTraceCapturing()) [[unlikely]] {
                    DispatchTraceBind(buffer_index, desc, vsharp, vsharp.base_address,
                                      vsharp.GetSize(), garbage ? "null-garbage-sharp" : "null-unbound",
                                      nullptr, 0, 0);
                }
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                // gow3 diagnostics (GOW3_TRACE_BIND_RANGE=<lo>:<hi> hex): bindings that overlap
                // the range, by shader and slot, 5 s statistics.
                {
                    static const std::pair<u64, u64> trace_range = [] {
                        const char* env = std::getenv("GOW3_TRACE_BIND_RANGE");
                        if (!env) {
                            return std::pair<u64, u64>{0, 0};
                        }
                        char* end = nullptr;
                        const u64 lo = std::strtoull(env, &end, 16);
                        const u64 hi = end && *end == ':' ? std::strtoull(end + 1, nullptr, 16) : 0;
                        return std::pair<u64, u64>{lo, hi};
                    }();
                    if (trace_range.second > trace_range.first &&
                        vsharp.base_address < trace_range.second &&
                        vsharp.base_address + size > trace_range.first) {
                        static std::mutex mutex;
                        static std::map<std::tuple<u64, u32, bool>, u32> seen;
                        static auto last = std::chrono::steady_clock::now();
                        std::scoped_lock lock{mutex};
                        ++seen[{stage.pgm_hash, binding.buffer, bool(desc.is_written)}];
                        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                            last = now;
                            std::string text;
                            for (const auto& [key, n] : seen) {
                                text += fmt::format(" {:08x}/slot{}{}x{}", std::get<0>(key), std::get<1>(key),
                                                    std::get<2>(key) ? "(write)" : "", n);
                            }
                            std::printf("Bind trace (5 s):%s\n", text.c_str());
                            std::fflush(stdout);
                            seen.clear();
                        }
                    }
                }
                if (size != vsharp.GetSize()) {
                    LOG_ERROR(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsharp.GetSize(), size, stage.pgm_hash);
                    if (!prepared && (desc.sharp_fetch.load_mask & 1)) {
                        DescribeSharpSource(stage, desc.sharp_fetch.offsets[0], "size");
                    }
                }
                if (stage.pgm_hash == 0x9f801363 && !desc.is_written && size >= 32) {
                    FixAimFocusCentre(memory, &buffer_cache, vsharp.base_address);
                }
                static const bool cloth_peek = [] {
                    const char* env = std::getenv("GOW3_CLOTH_PEEK");
                    return env && env[0] == '1';
                }();

                const bool reads_new_cloth = ClothTrace(stage.pgm_hash, buffer_index, vsharp.base_address,
                                                        size, desc.is_written, compute_stage);
                VideoCore::g_stream_stats_key = stage.pgm_hash << 8 | (buffer_index & 0xff);
                VideoCore::g_stream_stats_used =
                    desc.access_extent != 0 && desc.access_extent != ~0u && vsharp.stride != 0 &&
                            vsharp.num_records != 0 && !vsharp.swizzle_enable
                        ? u64(vsharp.num_records - 1) * vsharp.stride + desc.access_extent
                        : 0;
                if (coalesce && buffer_index == 6 && stage.buffers.size() >= 9 &&
                    desc.is_written && size == vsharp.GetSize() && size) {
                    std::array<SkinningWriteView, 3> views{};
                    views[0] = {vsharp.base_address, size};
                    bool eligible = true;
                    for (u32 slot = 7; slot <= 8; ++slot) {
                        const auto& other = stage.buffers[slot];
                        if (other.IsSpecial() || !other.is_written) {
                            eligible = false;
                            break;
                        }
                        const auto sharp = prepared ? prepared->buffer_sharps[slot]
                                                    : other.GetSharp(stage);
                        if (!sharp) {
                            eligible = false;
                            break;
                        }
                        views[slot - 6] = {sharp.base_address, sharp.GetSize()};
                    }
                    const u64 bytes = SkinningWriteUnion(views);
                    eligible = eligible && bytes &&
                               memory->ClampRangeSize(vsharp.base_address, bytes) == bytes;
                    if (eligible) {
                        // All descriptors here remain storage buffers. SynchronizeMemory's
                        // is_texel_buffer path is only taken for !is_written, so formatted
                        // stores have exactly the same acquisition semantics as raw stores.
                        const auto [buffer, offset] =
                            buffer_cache.ObtainBuffer(vsharp.base_address, bytes, true, false);
                        write_group = {buffer, vsharp.base_address, bytes, offset};
                        ++write_groups;
                        write_group_bytes += bytes;
                    } else {
                        ++write_group_refused;
                    }
                    if (((write_groups + write_group_refused) & 4095) == 0) {
                        static auto report = std::chrono::steady_clock::now();
                        if (const auto now = std::chrono::steady_clock::now();
                            now - report >= std::chrono::seconds{5}) {
                            report = now;
                            std::printf("Skinning write groups (5 s): %llu groups, %llu refused, "
                                        "%llu acquisitions saved, %.1f MB union bytes\n",
                                        write_groups, write_group_refused, write_groups * 2,
                                        write_group_bytes / 1e6);
                            write_groups = write_group_refused = write_group_bytes = 0;
                        }
                    }
                }
                const bool grouped = write_group.buffer && desc.is_written &&
                                     buffer_index >= 6 && buffer_index <= 8 &&
                                     vsharp.base_address >= write_group.address &&
                                     size <= write_group.size &&
                                     vsharp.base_address - write_group.address <= write_group.size - size;
                const auto [buffer, offset] = grouped
                    ? std::pair{write_group.buffer,
                                write_group.offset + vsharp.base_address - write_group.address}
                    : buffer_cache.ObtainBuffer(vsharp.base_address, size, desc.is_written,
                                                desc.is_formatted);
                if (run_verify_index != ~0u && desc.is_written) [[unlikely]] {
                    run_verify_writes.emplace_back(vsharp.base_address, size);
                }
                if (compute_stage && DispatchTraceCapturing()) [[unlikely]] {
                    DispatchTraceBind(buffer_index, desc, vsharp, vsharp.base_address, size,
                                      desc.is_written ? "gpu-written-buffer"
                                      : VideoCore::g_obtained_static ? "static-arena"
                                                                     : "stream-copy-or-gpu-buffer",
                                      buffer, buffer->SizeBytes(), offset);
                }
                if (cloth_peek && (stage.pgm_hash == 0x1c957b01 || stage.pgm_hash == 0x809864d3) &&
                    size <= 1_MB) {
                    cloth_peeks.push_back({vsharp.base_address, size, stage.pgm_hash, buffer_index,
                                           desc.is_written, buffer, offset});
                }
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                // gow3 17 Oct 2026: a strided buffer covers (records - 1) * stride plus the bytes
                // the shader reaches in a record, not a whole stride more. Uncharted's effect
                // shader (cs 8f067540, ~1500 dispatches a frame in crowds) writes one record array
                // per dispatch through views at +0/+12/+20: the +20 view's V# range ran 20 bytes
                // into the next dispatch's array, so every dispatch waited for the one before
                // (~10 ms of GPU time a frame). Needs the tracker's gap merge (BarrierTracker::
                // InsertRange): without those barriers its tree was never emptied (Uncharted 2's
                // museum 79 -> 41 FPS); with it 79 -> 85. GOW3_EXACT_STRIDED_RANGES=0: whole ranges.
                // (TrackedBufferSize, shared with the dispatch runs since 24 Oct 2026.)
                const u64 tracked_size = TrackedBufferSize(desc, vsharp, size);
                // gow3 17 Oct 2026: static ranges from the arena (never GPU-written mesh data,
                // ~10k binds a frame in Uncharted 3's crowds) are tracked in 64 KiB pieces: far
                // fewer barrier tracker nodes; a wider read range only adds barriers.
                // GOW3_STATIC_TRACK_KB (default 64, 0 = exact).
                static const u64 static_track = [] {
                    const char* env = std::getenv("GOW3_STATIC_TRACK_KB");
                    return (env ? std::strtoull(env, nullptr, 10) : 64ull) * 1024;
                }();
                if (VideoCore::g_obtained_static && static_track && !desc.is_written) {
                    const u64 lo = offset & ~(static_track - 1);
                    const u64 hi = Common::AlignUp(offset + tracked_size, static_track);
                    bound_buffers.emplace_back(buffer, lo, u32(std::min<u64>(hi - lo, buffer->SizeBytes() - lo)),
                                               false);
                } else {
                    bound_buffers.emplace_back(buffer, offset, tracked_size, desc.is_written);
                }
                if (desc.is_written) {
                    Gow3WatchRange::Note("storage", stage.pgm_hash, binding.buffer,
                                         vsharp.base_address, size);
                    VideoCore::NoteGpuWriter(vsharp.base_address, size, stage.pgm_hash);
                    if (IsEopReadbackShader(stage.pgm_hash) &&
                        eop_readbacks.size() + eop_open_readbacks.size() < 256) {
                        // The whole bound buffer counts as GPU-written (up to 9 MiB for
                        // 6a6422a9); read back only what this dispatch writes: dword i for
                        // i < min(groups * 64, s6, num_records) (vk_shader_hle.cpp). Only its
                        // small copies: the game reads a value in 1 KiB blocks (~160 per second),
                        // while its two large copies per frame (~0.2 and ~2.4 MB, never read in
                        // a precise-readback run) were 300 MB/s of readback.
                        u64 readback_size = size;
                        if (stage.pgm_hash == 0x6a6422a9) {
                            const auto ud = stage.UserData();
                            readback_size = 0;
                            if (vsharp.stride == 4 && ud.size() > 6) {
                                const u64 count = std::min<u64>(
                                    {u64(CsRegs().dim_x) * 64, ud[6], vsharp.num_records});
                                readback_size = std::min<u64>(size, count * 4);
                            }
                            if (readback_size > 64_KB) {
                                readback_size = 0;
                            }
                        }
                        static const bool open_raycasts = [] {
                            const char* open = std::getenv("GOW3_OPEN_RAYCAST_READBACKS");
                            const char* defer = std::getenv("GOW3_DEFER_RAYCAST_FENCES");
                            return open && open[0] == '1' && !(defer && defer[0] == '0');
                        }();
                        if (readback_size) {
                            (open_raycasts && stage.pgm_hash != 0x6a6422a9 ? eop_open_readbacks
                                                                          : eop_readbacks)
                                .emplace_back(vsharp.base_address, readback_size);
                        }
                    }
                    // Small writes (constants) and writes into windows where the CPU writes into
                    // GPU data (each such CPU write otherwise downloads with a full GPU wait).
                    const u64 readback_skip = ReadbackSkipShader();
                    if (size <= 4096 && Gow3Stats::enabled) {
                        // gow3: a shader whose first 3 writes were printed is remembered lock-free
                        // (direct-mapped by hash; a collision only takes the lock again), so the
                        // mutex and map are not taken per written binding of every dispatch.
                        static std::array<std::atomic<u64>, 256> writers_done{};
                        auto& writer_done =
                            writers_done[(stage.pgm_hash * 0x9E3779B97F4A7C15ull) >> 56];
                        if (writer_done.load(std::memory_order_relaxed) != ~stage.pgm_hash) {
                            static std::mutex writers_mutex;
                            static std::unordered_map<u64, u32> writers;
                            std::scoped_lock lock{writers_mutex};
                            const u32 written = writers[stage.pgm_hash]++;
                            if (written < 3) {
                                std::printf("Small GPU write: shader %#llx slot %u at %#llx size %llu\n",
                                            static_cast<unsigned long long>(stage.pgm_hash),
                                            binding.buffer,
                                            static_cast<unsigned long long>(vsharp.base_address),
                                            static_cast<unsigned long long>(size));
                            }
                            if (written >= 2) {
                                writer_done.store(~stage.pgm_hash, std::memory_order_relaxed);
                            }
                        }
                    }
                    if (sync_gpu_constants && small_gpu_writes.size() < 16384 &&
                        stage.pgm_hash != readback_skip) {
                        if (size <= 4096) {
                            small_gpu_writes.emplace_back(vsharp.base_address, size);
                            VideoCore::NoteReadbackKind(vsharp.base_address, size,
                                                        VideoCore::ReadbackSmallWrite);
                        } else if (buffer_cache.IsCpuWriteHot(vsharp.base_address, size)) {
                            // Only the hot 1 MiB windows of a large write.
                            const VAddr end = vsharp.base_address + size;
                            for (VAddr window = Common::AlignDown(vsharp.base_address, 1_MB);
                                 window < end; window += 1_MB) {
                                if (buffer_cache.IsCpuWriteHot(window, 1_MB)) {
                                    const VAddr lo = std::max(window, VAddr(vsharp.base_address));
                                    const VAddr hi = std::min(window + 1_MB, end);
                                    small_gpu_writes.emplace_back(lo, hi - lo);
                                    VideoCore::NoteReadbackKind(lo, hi - lo,
                                                                VideoCore::ReadbackHotWindow);
                                }
                            }
                        }
                    }
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                }
                {
                    const bool hazard = runtime.IsBufferAccessed(buffer, bound_buffers.back().offset,
                                                                 bound_buffers.back().size,
                                                                 desc.is_written);
                    needs_barrier |= hazard;
                    if (reads_new_cloth && !desc.is_written) {
                        NoteClothRead(stage.pgm_hash, buffer_index, vsharp.base_address, size,
                                      bound_buffers.back().size, needs_barrier, runtime.AccessEpoch());
                    }
                    // gow3 diagnostics (GOW3_BARRIER_STATS=1): which binding of which shader
                    // reports a hazard (first 40, then 5 s counts by shader/slot).
                    if (hazard && BarrierStatsEnabled()) {
                        static u32 logged = 0;
                        static std::map<std::tuple<u64, u32, bool, bool>, u32> counts;
                        static auto last = std::chrono::steady_clock::now();
                        const bool raw = runtime.IsBufferAccessed(buffer, offset, size, false);
                        ++counts[{stage.pgm_hash, binding.buffer, bool(desc.is_written), raw}];
                        if (logged < 40 && stage.pgm_hash == 0x8f067540) {
                            ++logged;
                            std::printf("Hazard: shader %08llx slot %u %s %#llx+%llu tracked %u, stride %u, "
                                        "records %u, extent %#x, swizzle %u tid %u (%s)\n",
                                        static_cast<unsigned long long>(stage.pgm_hash), binding.buffer,
                                        desc.is_written ? "write" : "read",
                                        static_cast<unsigned long long>(vsharp.base_address),
                                        static_cast<unsigned long long>(size), bound_buffers.back().size,
                                        u32(vsharp.stride), u32(vsharp.num_records), desc.access_extent,
                                        u32(vsharp.swizzle_enable), u32(vsharp.add_tid_enable),
                                        raw ? "after a GPU write" : "after a read only");
                        }
                        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                            last = now;
                            std::vector<std::pair<u32, std::tuple<u64, u32, bool, bool>>> order;
                            for (const auto& [key, n] : counts) {
                                order.emplace_back(n, key);
                            }
                            std::ranges::sort(order, std::greater{});
                            std::string text;
                            for (size_t i = 0; i < std::min<size_t>(order.size(), 10); ++i) {
                                const auto& [shader, slot, written, after_write] = order[i].second;
                                text += fmt::format(" {:08x}/slot{} {}{}={};", shader, slot,
                                                    written ? "write" : "read",
                                                    after_write ? " after write" : " after read",
                                                    order[i].first);
                            }
                            std::printf("Hazards (5 s):%s\n", text.c_str());
                            std::fflush(stdout);
                            counts.clear();
                        }
                    }
                }
            }
        }

        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

Rasterizer::ImageDescCacheEntry& Rasterizer::CachedImageDescEntry(const AmdGpu::Image& sharp,
                                                                   const Shader::ImageResource& res,
                                                                   u64 hash) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2 |
                      u32(res.needs_native || res.unnormalized_sampler)
                          << 3; // ImageDesc::needs_guest_size (HD UI textures)
    // Two-way set associative: a frame binds a few thousand distinct T#s.
    auto* set = &image_desc_cache[(hash % (image_desc_cache.size() / 2)) * 2];
    const auto matches = [&](const ImageDescCacheEntry& e) {
        return e.flags == flags && e.sharp == key;
    };
    const bool disabled = Gow3Toggle::Disabled(Gow3Toggle::ImageDescCache);
    ImageDescCacheEntry* slot = nullptr;
    if (!disabled && matches(set[0])) {
        slot = &set[0];
    } else if (!disabled && matches(set[1])) {
        slot = &set[1];
    } else {
        // Replace the least recently used way that no binding of this call points at.
        slot = set[0].last_use <= set[1].last_use ? &set[0] : &set[1];
        if (slot->pinned == bind_epoch) {
            slot = slot == &set[0] ? &set[1] : &set[0];
        }
        if (slot->pinned == bind_epoch) {
            slot = &image_desc_overflow.emplace_back();
        }
    }
    slot->last_use = ++desc_use_counter;
    auto& entry = *slot;
    if (entry.flags != flags || entry.sharp != key ||
        Gow3Toggle::Disabled(Gow3Toggle::ImageDescCache)) {
        entry.desc = VideoCore::TextureCache::ImageDesc{sharp, res};
        entry.sharp = key;
        entry.flags = flags;
        entry.found_generation = ~0ULL;
        entry.view_memo = {};
    }
    return entry;
}

// gow3 diagnostics: GOW3_TRACE_PS=<hex hash>[,...] (see BeginRenderingFull): their textures too.
static bool TracedShader(u64 hash) {
    static const std::vector<u64> hashes = [] {
        std::vector<u64> out;
        const char* env = std::getenv("GOW3_TRACE_PS");
        while (env && *env) {
            char* end = nullptr;
            out.push_back(std::strtoull(env, &end, 16));
            env = end && *end == ',' ? end + 1 : nullptr;
        }
        return out;
    }();
    return !hashes.empty() && std::ranges::find(hashes, hash) != hashes.end();
}

void Rasterizer::BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index,
                              bool& barrier, bool on_helper) {
    // gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1): a stage without images only binds its samplers (the
    // path below did the same after clearing its lists and initializing ~1 KiB of memo state).
    if (CpuTrims() && stage.images.empty()) {
        BindSamplers(stage, prepared, binding, write_index);
        return;
    }
    const u32 first_image_idx = image_infos.size();
    TextureSet* set_slot = nullptr;
    if (!on_helper && BindTexturesFromSet(stage, prepared, first_image_idx, barrier, set_slot)) {
        // Descriptor writes: one per image (no mip arrays in a memoized set).
        for (u32 i = 0; i < stage.images.size(); ++i) {
            auto& set_write = set_writes[write_index++];
            set_write.dstSet = VK_NULL_HANDLE;
            set_write.dstBinding = binding.unified++;
            set_write.dstArrayElement = 0;
            set_write.descriptorCount = 1;
            set_write.descriptorType = vk::DescriptorType::eSampledImage;
            set_write.pImageInfo = &image_infos[first_image_idx + i];
        }
        BindSamplers(stage, prepared, binding, write_index);
        return;
    }
    image_bindings.clear();
    image_binding_entries.clear();
    image_desc_overflow.clear();
    image_desc_storage.clear();
    ++bind_epoch;
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
    // TextureSetMemo: what this call resolves, to remember when the set qualifies.
    // GOW3_CPU_TRIMS=1: a reused array; only a memoizing call (GPU command thread) resets the
    // entries of its images, the only ones it writes and copies.
    std::optional<std::array<TextureSetEntry, TextureSet::MaxImages>> resolved_local;
    auto& resolved = CpuTrims() ? texture_set_scratch : resolved_local.emplace();
    bool set_ok = set_slot != nullptr;
    if (CpuTrims() && set_ok) {
        std::fill_n(resolved.begin(), std::min<size_t>(stage.images.size(), resolved.size()),
                    TextureSetEntry{});
    }
    // SampleSceneProxies: bindings that may read a reduced scene proxy instead of the native
    // image (normalized sampling only; see Shader::ImageResource::needs_native).
    boost::container::small_vector<bool, 16> binding_proxy_ok;
    const bool sample_proxies = !Gow3Toggle::Disabled(Gow3Toggle::SampleSceneProxies) &&
                                !on_helper && scene_targets->Reduced();

    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        auto fetched_tsharp =
            prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
        TestGarbageTSharp(fetched_tsharp);
        const auto tsharp = fetched_tsharp;
        if (TracedShader(stage.pgm_hash)) {
            static std::atomic<int> traced{0};
            if (traced.fetch_add(1) < 80) {
                std::printf("PS trace texture: shader %08llx image %u at %#llx %ux%u pitch %u levels %u-%u "
                            "format %u/%u\n",
                            static_cast<unsigned long long>(stage.pgm_hash), image_index,
                            static_cast<unsigned long long>(tsharp.Address()), u32(tsharp.width) + 1,
                            u32(tsharp.height) + 1, u32(tsharp.pitch) + 1, u32(tsharp.base_level),
                            u32(tsharp.last_level), u32(tsharp.GetDataFmt()), u32(tsharp.GetNumberFmt()));
                std::fflush(stdout);
            }
        }
        // gow3: a hash lookup per texture per draw for a diagnostic only.
        static const bool warn_meta = std::getenv("GOW3_WARN_META_TEXTURE") != nullptr;
        if (warn_meta && texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt) || !PlausibleTSharp(tsharp)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            if (!prepared && !on_helper && (image_desc.sharp_fetch.load_mask & 1)) {
                DescribeSharpSource(stage, image_desc.sharp_fetch.offsets[0], "texture");
            }
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        // Same as image_desc.NumBindings(stage), without fetching the T# again.
        const u32 num_bindings =
            mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex
                ? static_cast<u32>(tsharp.last_level - tsharp.base_level + 1)
                : 1u;
        const bool proxy_candidate = sample_proxies && num_bindings == 1 &&
                                     !image_desc.needs_native && !image_desc.is_written &&
                                     mip_fallback_mode == Shader::MipStorageFallbackMode::None;

        auto& desc_entry =
            prepared ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                     : CachedImageDescEntry(tsharp, image_desc);
        if (!PlausibleImage(desc_entry.desc.info, tsharp, memory, instance.GetMaxImageArrayLayers())) {
            static u32 reports = 0;
            if (reports < 32) {
                ++reports;
                LOG_WARNING(Render_Vulkan,
                            "Ignoring a garbage T# for stage {:#x}: {:#x}, {} bytes, {}x{}",
                            stage.pgm_hash, tsharp.Address(), desc_entry.desc.info.guest_size,
                            tsharp.width + 1, tsharp.height + 1);
            }
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }
        // GOW3_SCENE_DEBUG: proxied scene targets this binding reads at the native size, and why.
        const auto debug_native = [&](const VideoCore::Image& image) {
            if (!scene_debug_frame || proxy_candidate || !image.scene_proxy) return;
            std::printf("Scene native read: %s %016llx %s %ux%u needs_native %d written %d "
                        "bindings %u mip_fallback %d\n",
                        stage.sw_stage == Shader::SwStage::Compute ? "cs" : "gfx",
                        (unsigned long long)stage.pgm_hash,
                        vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                        image.info.size.height, int(image_desc.needs_native),
                        int(image_desc.is_written), num_bindings, int(mip_fallback_mode));
        };
        for (auto i = 0; i < num_bindings; i++) {
            // gow3: a plain binding (no mip override) of the same T# resolves to the same image
            // while no image was registered or unregistered.
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                desc_entry.found_generation == texture_cache.RegistryGeneration() &&
                !Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo)) {
                desc_entry.pinned = bind_epoch;
                auto& [image_id, _] =
                    image_bindings.emplace_back(desc_entry.found_id, &desc_entry.found_desc);
                image_binding_entries.push_back(&desc_entry);
                binding_proxy_ok.push_back(proxy_candidate);
                texture_cache.MarkFound(image_id);
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                debug_native(*image);
                if (image->binding.is_bound) {
                    image->binding.force_general |= image_desc.is_written;
                }
                image->binding.is_bound = 1u;
                continue;
            }
            auto& desc = image_desc_storage.emplace_back(desc_entry.desc);
            auto& [image_id, _] = image_bindings.emplace_back(VideoCore::ImageId{}, &desc);
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(proxy_candidate);

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            const u64 generation = texture_cache.RegistryGeneration();
            image_id = texture_cache.FindImage(desc);
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                generation == texture_cache.RegistryGeneration() &&
                desc_entry.pinned != bind_epoch) {
                desc_entry.found_generation = generation;
                desc_entry.found_id = image_id;
                desc_entry.found_desc = desc;
                desc_entry.view_memo = {};
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            debug_native(*image);
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (u32 binding_index = 0; binding_index < image_bindings.size(); ++binding_index) {
        auto& [image_id, desc_ptr] = image_bindings[binding_index];
        auto* memo_entry = image_binding_entries[binding_index];
        const auto& desc = *desc_ptr;
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (is_storage && image_id) {
            u64 lo, hi;
            if (Gow3WatchRange::Range(lo, hi)) {
                const auto& written = texture_cache.GetImage(image_id);
                Gow3WatchRange::Note("storage image", stage.pgm_hash, binding_index,
                                     written.info.guest_address, written.info.guest_size);
            }
        }
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                auto& rebind_desc = image_desc_storage.emplace_back(desc);
                desc_ptr = &rebind_desc;
                image_id = texture_cache.FindImage(rebind_desc);
                memo_entry = nullptr;
                set_ok = false;
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(
                image_id, desc, memo_entry ? &memo_entry->view_memo : nullptr, !on_helper);
            const auto binding = image.binding;

            // gow3: a proxied scene image sampled with normalized coordinates reads the proxy:
            // no resample to the native size (the native image is not touched).
            if (binding_proxy_ok[binding_index] && !is_storage && !binding.force_general &&
                !binding.is_target &&
                !(upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
                if (const auto proxy = scene_targets->SampleProxy(image, desc.view_info)) {
                    image.usage.texture = 1u;
                    image_infos.emplace_back(VK_NULL_HANDLE, proxy->view, proxy->layout);
                    if (set_ok && binding_index < resolved.size()) {
                        resolved[binding_index] = {image_id, proxy->view, image.backing,
                                                   desc.view_info.range, true};
                    }
                    ++proxy_samples;
                    continue;
                }
            }
            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage; a depth target sampled by its own draw shares the general layout with the
            // attachment (DepthFeedbackLayout).
            if (DepthFeedbackGeneral() && binding.is_target && image.info.props.is_depth &&
                !is_storage) {
                barrier |= runtime.Transit(&image, DepthFeedbackLayout, DepthFeedbackStages,
                                           DepthFeedbackAccess);
            } else if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
                }
            } else {
                if (is_storage) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;
            if (FrameCapture::Active()) {
                FrameCapture::Sampled(image.info, is_storage);
            }

            vk::ImageView view = *image_view.image_view;
            vk::ImageLayout layout = image.backing->state.layout;
            if (upscaler->Enabled()) {
                // gow3: the display pass reads the upscaled frame (scaled presets).
                upscaler->RedirectSampled(image_id, image_view.info, view, layout);
            }
            image_infos.emplace_back(VK_NULL_HANDLE, view, layout);
            if (set_ok && binding_index < resolved.size()) {
                resolved[binding_index] = {image_id, *image_view.image_view, image.backing,
                                           desc.view_info.range};
                set_ok = !is_storage && !binding.force_general && !binding.is_target;
            }
        }
    }
    if (set_ok && image_bindings.size() == stage.images.size()) {
        // Remember the set: the same T#s resolve the same way while no image is (un)registered.
        set_slot->generation = texture_cache.RegistryGeneration();
        set_slot->scene_generation = scene_targets->Generation();
        set_slot->count = static_cast<u32>(stage.images.size());
        std::copy_n(resolved.begin(), set_slot->count, set_slot->entries.begin());
    } else if (set_slot) {
        set_slot->key = 0;
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& desc = *image_bindings[image_binding_idx].second;
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    BindSamplers(stage, prepared, binding, write_index);
}

void Rasterizer::BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index) {
    for (u32 sampler_index = 0; sampler_index < stage.samplers.size(); ++sampler_index) {
        const auto& sampler = stage.samplers[sampler_index];
        auto ssharp =
            prepared ? prepared->sampler_sharps[sampler_index] : sampler.GetSharp(stage);
        ForceAnisotropy(ssharp, sampler, stage, prepared);
        WidenSamplerForHdMips(ssharp, sampler, stage, prepared);
        // gow3 25 Oct 2026 (player: blurred or wrong materials): Vulkan adds mipLodBias to
        // explicit-LOD samples too, so a sampler the shader uses with a chosen mip gets no
        // scene bias (one used both ways neither: the safe side). BiasExplicitLod: old way.
        float lod_bias = sampler.is_depth ? 0.0f : sampler_lod_bias;
        if (lod_bias != 0.0f) {
            if (sampler.explicit_lod && !Gow3Toggle::Disabled2(Gow3Toggle::BiasExplicitLod)) {
                lod_bias = 0.0f;
                if (Gow3Stats::enabled) {
                    Gow3Stats::bias_skipped_explicit.fetch_add(1, std::memory_order_relaxed);
                    if (sampler.implicit_lod) {
                        Gow3Stats::bias_skipped_mixed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else if (Gow3Stats::enabled) {
                Gow3Stats::samplers_biased.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const auto vk_sampler =
            texture_cache.GetSampler(ssharp, Regs().ta_bc_base, sampler.is_depth, lod_bias);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

// gow3 26 Oct 2026 (HD UI textures): a replacement image has the file's whole mip chain and is
// 2-4x the guest's size, and the guest's S# was written for the small one: with MaxLod 0 and no mip
// filter it would read level 0 only. The game draws a 4x file ~3x smaller than it is, and level 0
// alone skips texels (thin strokes and soft glows lose brightness). A sampler whose paired images
// are all unmipped (the T# has one level; the pairing is SamplerResource::image_mask) and one of
// them has a file (its guest size is named by one) gets the full LOD range and a linear mip
// filter. For a one-level image Vulkan clamps the LOD to level 0 whatever the sampler says, so a
// guest image is read exactly as before. 26 Oct log (GOW3_HD_LOG=1) of the title/menu screens:
// the icon samplers of U1-U3 use derivative LODs ("implicit"), so the larger image picks its own
// level; an explicit-LOD-only sampler would need a bias, none was seen and none is added.
void Rasterizer::WidenSamplerForHdMips(AmdGpu::Sampler& sharp, const Shader::SamplerResource& sampler,
                                       const Shader::Info& stage, const PreparedStage* prepared) const {
    static const bool any_files = Gow3HdTex::AnyFiles();
    static const bool hd_log = std::getenv("GOW3_HD_LOG") != nullptr;
    if (!any_files || sampler.is_depth || !Gow3Settings::Get().hd_ui.load(std::memory_order_relaxed) ||
        Gow3Toggle::Disabled2(Gow3Toggle::HdUiTextures) ||
        (sharp.xy_min_filter != AmdGpu::Filter::Bilinear &&
         sharp.xy_min_filter != AmdGpu::Filter::AnisoLinear) ||
        sampler.image_mask == 0 || sampler.image_mask == ~0u) {
        return;
    }
    bool has_file = false;
    for (u32 mask = sampler.image_mask; mask != 0; mask &= mask - 1) {
        const u32 index = static_cast<u32>(std::countr_zero(mask));
        if (index >= stage.images.size()) {
            return;
        }
        const auto tsharp = prepared ? prepared->image_sharps[index] : stage.images[index].GetSharp(stage);
        if (tsharp.last_level != tsharp.base_level) {
            return; // a mipped guest image: the game's own LOD range
        }
        has_file |= Gow3HdTex::HasGuestSize(tsharp.width + 1, tsharp.height + 1);
    }
    if (!has_file) {
        return; // none of the paired textures has a file for its size
    }
    if (hd_log) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1) < 30) {
            std::printf("HD sampler widened: min %u mag %u mip %u maxlod %u explicit %d implicit %d images %08x\n",
                        u32(sharp.xy_min_filter.Value()), u32(sharp.xy_mag_filter.Value()),
                        u32(sharp.mip_filter.Value()), u32(sharp.max_lod.Value()), sampler.explicit_lod,
                        sampler.implicit_lod, sampler.image_mask);
        }
    }
    sharp.max_lod.Assign(15 * 256);
    sharp.mip_filter.Assign(AmdGpu::MipFilter::Linear);
}

// gow3 21 Oct 2026 (ported from the Bloodborne PC port, same core): the PS4 builds sample most scene
// textures with little or no anisotropic filtering. Behind the temporal upscaler the output shows
// the detail of the output's mip level, and surfaces seen at a grazing angle (floors, walls along
// the view) stay blurred. Only mipmapped linear samplers of graphics draws are raised; depth
// compares, point filters and unmipped samplers (UI, post-processing) keep the game's setup.
void Rasterizer::ForceAnisotropy(AmdGpu::Sampler& sharp, const Shader::SamplerResource& sampler,
                                 const Shader::Info& stage, const PreparedStage* prepared) const {
    const bool is_depth = sampler.is_depth;
    if (sampler_graphics && !is_depth) {
        // The game's filters, once per kind: min filter, mip filter, ratio (evidence of what the
        // games ask for, logged whether or not the setting raises them).
        static std::atomic<u64> seen{0};
        const u32 kind = u32(sharp.xy_min_filter.Value()) * 3 * 5 + u32(sharp.mip_filter.Value()) * 5 +
                         u32(sharp.max_aniso.Value());
        if (kind < 64 && !(seen.fetch_or(1ull << kind) & (1ull << kind))) {
            std::printf("Sampler: game uses min filter %u, mip filter %u, anisotropy %.0fx\n",
                        u32(sharp.xy_min_filter.Value()), u32(sharp.mip_filter.Value()), sharp.MaxAniso());
        }
    }
    const u32 forced = sampler_forced_aniso;
    if (forced < 2 || is_depth || sharp.mip_filter == AmdGpu::MipFilter::None ||
        (sharp.xy_min_filter != AmdGpu::Filter::Bilinear &&
         sharp.xy_min_filter != AmdGpu::Filter::AnisoLinear)) {
        return;
    }
    if (AmdGpu::IsAnisoFilter(sharp.xy_min_filter) && sharp.MaxAniso() >= static_cast<float>(forced)) {
        return;
    }
    // gow3 25 Oct 2026 (player: lines at texture edges): the comment above promised unmipped
    // samplers keep the game's setup, but only the mip filter was checked. Anisotropy at level 0
    // widens the footprint along one axis and reads across atlas cells and LUT rows. Skipped:
    // samplers used only with an explicit LOD (no derivatives to widen), clamped to MaxLod 0, or
    // paired only with one-level images. An unknown pairing (binding >= 32) is raised as before.
    if (!Gow3Toggle::Disabled2(Gow3Toggle::AnisoUnmipped)) {
        if (sampler.explicit_lod && !sampler.implicit_lod) {
            if (Gow3Stats::enabled) {
                Gow3Stats::aniso_skipped_explicit.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        if (sharp.max_lod.Value() == 0) {
            if (Gow3Stats::enabled) {
                Gow3Stats::aniso_skipped_maxlod0.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        if (sampler.image_mask != 0 && sampler.image_mask != ~0u) {
            bool mipmapped = false;
            for (u32 mask = sampler.image_mask; mask != 0 && !mipmapped; mask &= mask - 1) {
                const u32 index = static_cast<u32>(std::countr_zero(mask));
                if (index >= stage.images.size()) {
                    mipmapped = true; // not expected; keep the old behaviour
                    break;
                }
                const auto tsharp =
                    prepared ? prepared->image_sharps[index] : stage.images[index].GetSharp(stage);
                mipmapped = tsharp.last_level != tsharp.base_level;
            }
            if (!mipmapped) {
                if (Gow3Stats::enabled) {
                    Gow3Stats::aniso_skipped_unmipped.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
        }
    }
    if (Gow3Stats::enabled) {
        Gow3Stats::aniso_raised.fetch_add(1, std::memory_order_relaxed);
    }
    const auto ratio = forced >= 16  ? AmdGpu::AnisoRatio::Sixteen
                       : forced >= 8 ? AmdGpu::AnisoRatio::Eight
                       : forced >= 4 ? AmdGpu::AnisoRatio::Four
                                     : AmdGpu::AnisoRatio::Two;
    sharp.xy_min_filter.Assign(AmdGpu::Filter::AnisoLinear);
    if (sharp.xy_mag_filter == AmdGpu::Filter::Bilinear) {
        sharp.xy_mag_filter.Assign(AmdGpu::Filter::AnisoLinear);
    }
    sharp.max_aniso.Assign(ratio);
}

Rasterizer::BeginSignature Rasterizer::MakeBeginSignature(const GraphicsPipeline* pipeline) const {
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    BeginSignature sig;
    const u32 num_color = std::bit_width(key.mrt_mask);
    for (u32 cb = 0; cb < num_color && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        sig.ids[cb] = cb_descs[cb].first;
        if (cb_descs[cb].first) {
            sig.views[cb] = cb_descs[cb].second.view_info;
        }
        sig.color_samples[cb] = key.color_samples[cb];
    }
    sig.ids[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
    if (db_desc.first) {
        sig.views[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.second.view_info;
    }
    sig.mrt_mask = key.mrt_mask;
    sig.num_samples = key.num_samples;
    sig.motion = key.motion_vectors;
    sig.scene_started = scene_started;
    sig.raster_scaling = upscaler->RasterScaling();
    sig.upscaler_state = upscaler->RedirectState();
    sig.generation = texture_cache.RegistryGeneration();
    std::memcpy(&sig.depth_control, &regs.depth_control, sizeof(u32));
    sig.depth_valid = regs.depth_buffer.DepthValid();
    sig.stencil_valid = regs.depth_buffer.StencilValid();
    return sig;
}

bool Rasterizer::BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                                     u32 first_image_idx, bool& barrier, TextureSet*& slot) {
    const u32 count = static_cast<u32>(stage.images.size());
    if (!prepared || count == 0 || count > TextureSet::MaxImages ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureSetMemo) || FrameCapture::Active()) {
        return false;
    }
    for (const auto& image : stage.images) {
        if (image.is_written || image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
            return false;
        }
    }
    u64 key = XXH3_64bits_withSeed(prepared->image_hashes, count * sizeof(u64),
                                   reinterpret_cast<u64>(&stage));
    key |= 1; // 0 marks an empty slot
    auto& set = texture_sets[key % texture_sets.size()];
    slot = &set;
    const u64 generation = texture_cache.RegistryGeneration();
    const bool match = set.key == key && set.stage == &stage && set.count == count &&
                       set.generation == generation &&
                       set.scene_generation == scene_targets->Generation() &&
                       std::equal(set.hashes.begin(), set.hashes.begin() + count,
                                  prepared->image_hashes);
    if (!match) {
        ++texture_set_why[set.key != key ? 0 : set.generation != generation ? 1 : 3];
        ++texture_set_misses;
        set.key = key;
        set.stage = &stage;
        set.count = 0;
        set.generation = ~0ull;
        std::copy_n(prepared->image_hashes, count, set.hashes.begin());
        return false;
    }
    // Every image still has the backing its view belongs to and needs no refresh.
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(entry.id);
        // Proxy entries need a current proxy; native entries of a proxied image go through
        // BindTextures, which may sample the proxy instead.
        const bool proxies_on = !Gow3Toggle::Disabled(Gow3Toggle::SampleSceneProxies);
        if (image.backing != entry.backing || image.binding.needs_rebind ||
            image.binding.is_target || !texture_cache.IsUpToDate(entry.id) ||
            (entry.proxy ? !proxies_on ||
                               !scene_targets->ProxyCurrent(image, entry.range.base.level,
                                                            entry.range.extent.levels)
                         : image.scene_proxy && proxies_on) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(entry.id))) {
            ++texture_set_why[2];
            ++texture_set_misses;
            set.generation = ~0ull;
            return false;
        }
    }
    ++texture_set_hits;
    slot = nullptr;
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            continue;
        }
        texture_cache.MarkFound(entry.id);
        auto& image = texture_cache.GetImage(entry.id);
        image.binding.is_bound = 1u;
        bound_images.emplace_back(entry.id);
        image.usage.texture = 1u;
        if (entry.proxy) {
            image_infos.emplace_back(VK_NULL_HANDLE, entry.view,
                                     scene_targets->PrepareSample(image, entry.range.base.level));
            ++proxy_samples;
            continue;
        }
        if (DepthFeedbackGeneral() && image.info.props.is_depth && image.binding.is_target) {
            barrier |= runtime.Transit(&image, DepthFeedbackLayout, DepthFeedbackStages,
                                       DepthFeedbackAccess);
            image_infos.emplace_back(VK_NULL_HANDLE, entry.view, image.backing->state.layout);
            continue;
        }
        const auto new_layout = image.info.props.is_depth
                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier |= runtime.Transit(&image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                                   vk::AccessFlagBits2::eShaderRead, entry.range);
        image_infos.emplace_back(VK_NULL_HANDLE, entry.view, image.backing->state.layout);
    }
    (void)first_image_idx;
    return true;
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    // gow3: RenderStateMemo (see BeginMemo).
    const bool memo_on = !Gow3Toggle::Disabled(Gow3Toggle::RenderStateMemo) && !FrameCapture::Active();
    if (Gow3Stats::enabled && ((begin_memo_hits + begin_memo_misses) & 0x3FFFF) == 0x3FFFF) {
        std::printf("Render state memo: %llu hits, %llu misses; texture sets: %llu hits, %llu "
                    "misses\n",
                    static_cast<unsigned long long>(begin_memo_hits),
                    static_cast<unsigned long long>(begin_memo_misses),
                    static_cast<unsigned long long>(texture_set_hits),
                    static_cast<unsigned long long>(texture_set_misses));
        std::printf("  texture set misses: other key %llu, generation %llu, image check %llu, "
                    "other %llu\n",
                    (unsigned long long)texture_set_why[0], (unsigned long long)texture_set_why[1],
                    (unsigned long long)texture_set_why[2], (unsigned long long)texture_set_why[3]);
        std::printf("  scene proxies sampled directly: %llu bindings\n",
                    static_cast<unsigned long long>(proxy_samples));
        proxy_samples = 0;
        texture_set_why = {};
        begin_memo_hits = begin_memo_misses = texture_set_hits = texture_set_misses = 0;
    }
    BeginSignature signature;
    if (memo_on) {
        signature = MakeBeginSignature(pipeline);
        const auto& regs = Regs();
        bool usable = begin_memo.valid && scheduler.IsRenderingWith(begin_memo.state) &&
                      !regs.depth_render_control.depth_clear_enable &&
                      !regs.depth_render_control.stencil_clear_enable &&
                      signature == begin_memo.signature;
        for (const auto id : signature.ids) {
            if (usable && id) {
                const auto& image = texture_cache.GetImage(id);
                usable = !image.binding.is_bound && !image.binding.needs_rebind;
            }
        }
        if (usable) {
            ++begin_memo_hits;
            attachment_feedback_loop = false;
            push_data.scene_size = begin_memo.scene_size;
            target_scale = begin_memo.target_scale;
            NoteHudTarget(begin_memo.state);
            float_color0 = begin_memo.float_color0;
            return begin_memo.state;
        }
        ++begin_memo_misses;
    }
    begin_memo.valid = false;
    RenderState state = BeginRenderingFull(pipeline);
    if (memo_on && !attachment_feedback_loop) {
        // (is_clear of the depth attachment shares its bytes with has_depth.)
        bool clears = state.depth_stencil_attachment.depth_clear ||
                      state.depth_stencil_attachment.stencil_clear;
        for (u32 i = 0; i < state.num_color_attachments; ++i) {
            clears |= state.color_attachments[i].is_clear != 0;
        }
        if (!clears) {
            // Recomputed after the full path: it may have started the scene (scene_started).
            begin_memo = {true, MakeBeginSignature(pipeline), state, push_data.scene_size,
                          target_scale, float_color0};
        }
    }
    NoteHudTarget(state);
    return state;
}

// gow3: Uncharted 2 ultrawide HUD (hud_layout.h, HudLayout::Tracker): which draws are the HUD's.
void Rasterizer::NoteHudDraw(u64 ps_hash, bool indexed) {
    hud_draw = false;
    if (hud_aspect <= 0.0f) {
        return;
    }
    const auto& cb = Regs().color_buffers[0];
    if (!cb) {
        return;
    }
    const VAddr target = cb.Address();
    const u64 frames = hud_tracker.frames;
    hud_draw = hud_tracker.Draw(ps_hash, target, FrameCapture::IsDisplayBuffer(target), indexed);
    if (hud_trace) {
        static constexpr size_t kShaders = 12;
        static u64 shaders[kShaders]{};
        static u32 shader_count = 0;
        if (hud_draw && shader_count < kShaders &&
            std::find(shaders, shaders + shader_count, ps_hash) == shaders + shader_count) {
            shaders[shader_count++] = ps_hash;
        }
        if (hud_tracker.frames != frames && hud_tracker.frames % 300 == 0) {
            std::string list;
            for (u32 i = 0; i < shader_count; ++i) {
                list += fmt::format(" {:08x}", shaders[i]);
            }
            std::printf("HUD trace: %llu frames; over the frame %llu draws (%llu not indexed, kept), "
                        "after the copy %llu; HUD shaders:%s\n",
                        static_cast<unsigned long long>(hud_tracker.frames),
                        static_cast<unsigned long long>(hud_tracker.over_frame_draws),
                        static_cast<unsigned long long>(hud_tracker.over_frame_skipped),
                        static_cast<unsigned long long>(hud_tracker.after_copy_draws), list.c_str());
            std::fflush(stdout);
        }
    }
}

bool Rasterizer::HudExtent(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset,
                           float& min_x, float& max_x, std::string* detail) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    const auto& regs = Regs();
    const auto& fetch = pipeline->GetFetchShader();
    if (regs.num_indices == 0) {
        return false;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    // The position: the per-vertex float attribute of 2-4 components (32 or 16 bits) with the
    // lowest semantic.
    const Shader::Gcn::VertexAttribute* position = nullptr;
    AmdGpu::Buffer buffer{};
    bool half = false;
    const auto min_stride_of = [](bool is_half) { return is_half ? 4u : 8u; };
    if (fetch && !fetch->attributes.empty()) {
        for (const auto& attrib : fetch->attributes) {
            if (attrib.GetStepRate() != InstanceIdType::None) {
                continue;
            }
            const auto sharp = attrib.GetSharp(vs);
            const auto fmt = sharp.GetDataFmt();
            const bool is_half = fmt == AmdGpu::DataFormat::Format16_16 ||
                                 fmt == AmdGpu::DataFormat::Format16_16_16_16;
            if (sharp.GetNumberFmt() != AmdGpu::NumberFormat::Float ||
                (!is_half && fmt != AmdGpu::DataFormat::Format32_32 &&
                 fmt != AmdGpu::DataFormat::Format32_32_32 &&
                 fmt != AmdGpu::DataFormat::Format32_32_32_32)) {
                continue;
            }
            if (!position || attrib.semantic < position->semantic) {
                position = &attrib;
                buffer = sharp;
                half = is_half;
            }
        }
        if (!position || buffer.GetStride() < min_stride_of(half)) {
            return false;
        }
    } else {
        // No fetch shader: the first vertex buffer of the shader that looks like vertices (best
        // effort; the position is taken as the first two floats).
        bool found = false;
        for (u32 i = 0; i < vs.buffers.size() && !found; ++i) {
            const auto sharp = vs.buffers[i].GetSharp(vs);
            const auto fmt = sharp.GetDataFmt();
            const u32 stride = sharp.GetStride();
            const bool float_fmt = (fmt == AmdGpu::DataFormat::Format32_32 ||
                                    fmt == AmdGpu::DataFormat::Format32_32_32 ||
                                    fmt == AmdGpu::DataFormat::Format32_32_32_32) &&
                                   sharp.GetNumberFmt() == AmdGpu::NumberFormat::Float;
            const bool raw = fmt == AmdGpu::DataFormat::FormatInvalid ||
                             fmt == AmdGpu::DataFormat::Format32 ||
                             fmt == AmdGpu::DataFormat::Format8;
            if (stride >= 8 && stride <= 256 && sharp.base_address != 0 && (float_fmt || raw)) {
                buffer = sharp;
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    const u32 stride = buffer.GetStride();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs, fetch);
    const u32 count = std::min<u32>(regs.num_indices, 4096);
    const auto readable = [&](VAddr address, u64 size) {
        return memory->IsValidGpuMapping(address, 0) &&
               memory->ClampRangeSize(address, size) == size;
    };
    min_x = std::numeric_limits<float>::max();
    max_x = std::numeric_limits<float>::lowest();
    bool any = false;
    if (detail) {
        // The first 4 vertices' first 4 floats of the chosen attribute (raw, before any index).
        *detail = fmt::format("chosen: {} base {:#x} stride {}{}", position
                                  ? fmt::format("semantic {}", position->semantic)
                                  : std::string{"vertex buffer"},
                              u64(buffer.base_address), stride, half ? " half" : "");
        for (u32 v = 0; v < 4; ++v) {
            *detail += fmt::format("\n    v{}:", v);
            for (u32 c = 0; c < 4; ++c) {
                const u32 size = half ? 2 : 4;
                const VAddr address = buffer.base_address + u64(u64(vertex_offset) + v) * stride +
                                      c * size;
                if (!memory->IsValidGpuMapping(address, 0) ||
                    memory->ClampRangeSize(address, size) != size) {
                    *detail += " ?";
                    continue;
                }
                float f;
                if (half) {
                    u16 h;
                    std::memcpy(&h, reinterpret_cast<const void*>(address), 2);
                    f = HudLayout::HalfToFloat(h);
                } else {
                    std::memcpy(&f, reinterpret_cast<const void*>(address), 4);
                }
                *detail += fmt::format(" {:.4f}", f);
            }
        }
    }
    const auto take = [&](u64 vertex) {
        const VAddr address = buffer.base_address + vertex * stride;
        if (!readable(address, half ? sizeof(u16) : sizeof(float))) {
            return;
        }
        float x;
        if (half) {
            u16 h;
            std::memcpy(&h, reinterpret_cast<const void*>(address), sizeof(h));
            x = HudLayout::HalfToFloat(h);
        } else {
            std::memcpy(&x, reinterpret_cast<const void*>(address), sizeof(x));
        }
        if (std::isfinite(x)) {
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            any = true;
        }
    };
    if (is_indexed) {
        const bool index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
        const u32 index_size = index16 ? 2 : 4;
        const VAddr indices = regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size;
        if (!readable(indices, u64(count) * index_size)) {
            return false;
        }
        const u32 restart = index16 ? 0xffffu : 0xffffffffu;
        for (u32 i = 0; i < count; ++i) {
            u32 index;
            if (index16) {
                u16 v;
                std::memcpy(&v, reinterpret_cast<const void*>(indices + i * 2), 2);
                index = v;
            } else {
                std::memcpy(&index, reinterpret_cast<const void*>(indices + u64(i) * 4), 4);
            }
            if (index != restart) {
                take(u64(index) + vertex_offset);
            }
        }
    } else {
        for (u32 i = 0; i < count; ++i) {
            take(u64(vertex_offset) + i);
        }
    }
    return any;
}

void Rasterizer::NoteHudTarget(const RenderState& state) {
    hud_k = 1.0f;
    if (hud_draw && hud_anchor != HudLayout::Anchor::Full) {
        hud_k = HudLayout::Factor(hud_aspect, state.width, state.height);
        hud_width = state.width;
        hud_height = state.height;
    }
}

RenderState Rasterizer::BeginRenderingFull(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    float_color0 = false;
    using VulkanUpscalerTarget = TemporalUpscaler::Target;
    VulkanUpscalerTarget redirect{};
    bool color_redirected = false, depth_redirected = false;
    std::pair<u32, u32> guest_extent{1, 1};
    std::pair<vk::ImageView, vk::ImageLayout> original_color{};
    const auto& regs = Regs();
    const auto& key = pipeline->GetGraphicsKey();
    {
        u64 lo, hi;
        if (Gow3WatchRange::Range(lo, hi)) {
            const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
            for (u32 cb = 0; cb < cb_descs.size(); ++cb) {
                if (cb_descs[cb].first) {
                    const auto& image = texture_cache.GetImage(cb_descs[cb].first);
                    Gow3WatchRange::Note("render target", ps ? ps->pgm_hash : 0, cb,
                                         image.info.guest_address, image.info.guest_size);
                }
            }
        }
    }
    // gow3 diagnostics (GOW3_TRACE_PS=<hex hash>[,<hash>...]): draws with these pixel shaders:
    // render targets, depth, viewport and scissors (60 lines at most).
    {
        static const std::vector<u64> trace_ps = [] {
            std::vector<u64> out;
            const char* env = std::getenv("GOW3_TRACE_PS");
            while (env && *env) {
                char* end = nullptr;
                out.push_back(std::strtoull(env, &end, 16));
                env = end && *end == ',' ? end + 1 : nullptr;
            }
            return out;
        }();
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        static std::atomic<int> traced{0};
        if (ps && !trace_ps.empty() && std::ranges::find(trace_ps, ps->pgm_hash) != trace_ps.end() &&
            traced.fetch_add(1) < 60) {
            const auto* vs = pipeline->GetStages()[u32(Shader::SwStage::Vertex)];
            std::string text = fmt::format("PS trace: ps {:08x} vs {:08x}", ps->pgm_hash, vs ? vs->pgm_hash : 0);
            for (u32 cb = 0; cb < cb_descs.size(); ++cb) {
                if (cb_descs[cb].first) {
                    const auto& image = texture_cache.GetImage(cb_descs[cb].first);
                    text += fmt::format("; rt{} {:#x} {}x{} {}", cb, image.info.guest_address,
                                        image.info.size.width, image.info.size.height,
                                        vk::to_string(image.info.pixel_format));
                }
            }
            if (db_desc.first) {
                const auto& depth = texture_cache.GetImage(db_desc.first);
                text += fmt::format("; depth {:#x} {}x{}", depth.info.guest_address, depth.info.size.width,
                                    depth.info.size.height);
            }
            const auto& vp = regs.viewports[0];
            text += fmt::format("; viewport scale {} {} offset {} {}", vp.xscale, vp.yscale, vp.xoffset,
                                vp.yoffset);
            const auto& s = regs.screen_scissor;
            text += fmt::format("; screen scissor {},{}-{},{}", s.top_left_x, s.top_left_y, s.bottom_right_x,
                                s.bottom_right_y);
            const auto scissor = [](const AmdGpu::ViewportScissor& v) {
                return fmt::format("{},{}-{},{}", u32(v.top_left_x), u32(v.top_left_y), u32(v.bottom_right_x),
                                   u32(v.bottom_right_y));
            };
            text += fmt::format("; window {} generic {} vp0 {}; window offset {},{}", scissor(regs.window_scissor),
                                scissor(regs.generic_scissor), scissor(regs.viewport_scissors[0]),
                                s32(regs.window_offset.window_x_offset), s32(regs.window_offset.window_y_offset));
            std::printf("%s\n", text.c_str());
            std::fflush(stdout);
        }
    }
    // The scene starts with God of War III's G-buffer (5+ targets) or, in Uncharted, the first
    // draw with the camera's scene depth (the light pre-pass): from there to the upscale the
    // scene-size targets are drawn reduced (presets below Native AA / DLAA).
    if (db_desc.first && (std::popcount(key.mrt_mask & 0x7f) >= 5 || flat_scene_draw) &&
        scene_targets->EligibleScene(texture_cache.GetImage(db_desc.first))) scene_started = true;
    // A proxy attachment cannot represent MSAA or a feedback loop that reads the
    // same image through the guest's native descriptor during this draw.
    bool reduced = scene_started && upscaler->RasterScaling() && key.num_samples == 1;
    // GOW3_SCENE_DEBUG=<file>: once the file exists, why the passes of one frame keep the
    // native size (see NoteFrameStart).
    const bool debug_pass = scene_debug_frame && scene_started;
    std::string why;
    if (debug_pass) {
        why = fmt::format("pass vs {:08x} ps {:08x} mrt {:#x} samples {} raster_scaling {}:",
                          pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                          key.mrt_mask ? pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash
                                       : 0,
                          key.mrt_mask, u32(key.num_samples), upscaler->RasterScaling());
    }
    // Only this pass's attachments: slots past the mask's width keep earlier passes' targets,
    // often the G-buffer images the lighting passes sample (bound), which kept those native.
    const u32 num_attachments = Gow3Toggle::Disabled(Gow3Toggle::SceneAttachmentsOnly)
                                    ? u32(cb_descs.size())
                                    : u32(std::bit_width(key.mrt_mask));
    // All attachments of a reduced pass share one proxy size (scene or half resolution).
    std::optional<SceneResolution::Size> pass_size;
    const auto same_size = [&](const VideoCore::Image& image, u32 level) {
        const auto proxy = scene_targets->ProxySize(image, level);
        if (!pass_size) pass_size = proxy;
        return *pass_size == proxy;
    };
    for (u32 cb = 0; cb < num_attachments; ++cb) {
        const auto& [id, desc] = cb_descs[cb];
        if (id) {
            const auto& image = texture_cache.GetImage(id);
            // Mip levels of eligible chains have their own proxies (SceneTargets::Get).
            if (!scene_targets->Eligible(image) || image.binding.is_bound ||
                image.binding.needs_rebind || desc.view_info.range.base.layer ||
                !same_size(image, desc.view_info.range.base.level)) reduced = false;
            if (debug_pass) {
                why += fmt::format(" [{:x} {} {}x{} eligible {} bound {} rebind {} level {} layer {}]",
                                   image.info.guest_address,
                                   vk::to_string(image.info.pixel_format), image.info.size.width,
                                   image.info.size.height, scene_targets->Eligible(image),
                                   u32(image.binding.is_bound), u32(image.binding.needs_rebind),
                                   desc.view_info.range.base.level,
                                   desc.view_info.range.base.layer);
            }
        }
    }
    if (db_desc.first && (!scene_targets->Eligible(texture_cache.GetImage(db_desc.first)) ||
                          !same_size(texture_cache.GetImage(db_desc.first), 0))) {
        reduced = false;
    }
    if (debug_pass) {
        if (db_desc.first) {
            const auto& depth = texture_cache.GetImage(db_desc.first);
            why += fmt::format(" depth [{:x} {} {}x{} eligible {}]", depth.info.guest_address,
                               vk::to_string(depth.info.pixel_format), depth.info.size.width,
                               depth.info.size.height, scene_targets->Eligible(depth));
        }
        std::printf("Scene pass: reduced %d %s\n", reduced, why.c_str());
    }
    push_data.scene_size =
        reduced ? SceneResolution::PackCoord(scene_targets->Size(), SceneResolution::Native()) : 0;
    if (Gow3Stats::enabled) {
        Gow3Stats::reduced_draws.fetch_add(reduced, std::memory_order_relaxed);
        Gow3Stats::scene_draws.fetch_add(scene_started, std::memory_order_relaxed);
    }
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            last_targets[cb].generation = ~0ULL; // FindImage may rewrite the description
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (reduced) {
            // The native image is resolved lazily when a shader/copy actually reads it.
        } else if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image->usage.render_target = 1u;
        if (cb == 0 && upscaler->Enabled()) {
            color_redirected = upscaler->RedirectColor(image_id, desc.view_info, redirect);
            float_color0 = color_redirected && upscaler->RedirectedToFloat();
            if (color_redirected) {
                guest_extent = {image->info.size.width, image->info.size.height};
                original_color = {attachment.image_view, attachment.image_layout};
                attachment.image_view = redirect.view;
                attachment.image_layout = redirect.layout;
            }
        }
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        if (!reduced && DepthFeedbackGeneral() && image.binding.is_bound) {
            needs_barrier |= runtime.Transit(&image, DepthFeedbackLayout, DepthFeedbackStages,
                                             DepthFeedbackAccess);
        } else if (!reduced) needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        {
            static const u64 trace_address = [] {
                const char* env = std::getenv("GOW3_TRACE_IMAGE");
                return env ? std::strtoull(env, nullptr, 16) : 0ULL;
            }();
            if (trace_address && image.info.guest_address == trace_address) {
                static std::atomic<int> lines{0};
                if (lines.fetch_add(1) < 300) {
                    std::printf("Trace image: depth attachment, reduced %d, storage %d, stencil write %d, "
                                "depth write %d, layout %s, state access %s\n",
                                int(reduced), int(desc.view_info.is_storage), int(stencil_write),
                                int(regs.depth_control.depth_write_enable),
                                vk::to_string(image.backing->state.layout).c_str(),
                                vk::to_string(image.backing->state.access_mask).c_str());
                    std::fflush(stdout);
                }
            }
        }
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            // gow3 diagnostics (Uncharted shadow map D16 0x11352d0000 found cleared to 0): every
            // distinct depth clear (format, value, how it was requested) once, and the live switch
            // "dbclear one" (GOW3_DMA_TRIGGER) clearing 16-bit depth targets to 1.0.
            if (is_depth_clear) {
                const bool meta = !(regs.depth_render_control.depth_clear_enable &&
                                    regs.depth_control.depth_enable &&
                                    regs.depth_control.depth_write_enable);
                static std::mutex clear_mutex;
                static std::vector<u64> clears_seen;
                const u64 key = (u64(std::bit_cast<u32>(regs.depth_clear)) << 32) |
                                (u64(image.info.pixel_format) << 1) | u64(meta);
                std::scoped_lock lock{clear_mutex};
                if (std::find(clears_seen.begin(), clears_seen.end(), key) == clears_seen.end()) {
                    clears_seen.push_back(key);
                    std::printf("GPU: depth clear %s %ux%u at %#llx to %g (%s)\n",
                                vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                                image.info.size.height, (unsigned long long)image.info.guest_address,
                                regs.depth_clear, meta ? "HTILE meta clear" : "DB_RENDER_CONTROL clear");
                    std::fflush(stdout);
                }
                if (depth16_clear_one.load(std::memory_order_relaxed) &&
                    image.info.pixel_format == vk::Format::eD16Unorm) {
                    attachment.clear_value[0] = std::bit_cast<u32>(1.0f);
                }
            }
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image.usage.depth_target = true;
        if (upscaler->Enabled()) {
            VulkanUpscalerTarget depth_redirect;
            depth_redirected = upscaler->RedirectDepth(image_id, depth_redirect);
            if (depth_redirected) {
                guest_extent = {image.info.size.width, image.info.size.height};
                attachment.image_view = depth_redirect.view;
                attachment.image_layout = depth_redirect.layout;
                redirect = depth_redirect;
            }
        }
    } else {
        state.depth_stencil_attachment = {};
    }

    // gow3: a pass drawn into the upscaler's output-size images (UI, display pass): every
    // attachment must be redirected, viewports and scissors are scaled.
    target_scale = {1.0f, 1.0f};
    if (color_redirected || depth_redirected) {
        u32 color_targets = 0;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            color_targets += state.color_attachments[cb].image_view ? 1 : 0;
        }
        const bool has_depth = bool(db_desc.first);
        if ((color_targets > 0 && !color_redirected) || (has_depth && !depth_redirected) ||
            color_targets > 1) {
            static u32 warned = 0;
            if (warned < 8) {
                ++warned;
                const auto describe = [&](VideoCore::ImageId id) {
                    if (!id) {
                        return std::string{"-"};
                    }
                    const auto& info = texture_cache.GetImage(id).info;
                    return fmt::format("{:#x} {}x{} {}", info.guest_address, info.size.width,
                                       info.size.height, vk::to_string(info.pixel_format));
                };
                std::printf("Upscaler: pass not redirected (color %s: %s, depth %s: %s, %u "
                            "targets)\n",
                            color_redirected ? "yes" : "no", describe(cb_descs[0].first).c_str(),
                            depth_redirected ? "yes" : "no", describe(db_desc.first).c_str(),
                            state.num_color_attachments);
            }
            if (color_redirected) {
                state.color_attachments[0].image_view = original_color.first;
                state.color_attachments[0].image_layout = original_color.second;
            }
            if (depth_redirected) {
                auto& depth_image = texture_cache.GetImage(db_desc.first);
                state.depth_stencil_attachment.image_view =
                    *texture_cache.FindDepthTarget(db_desc.first, db_desc.second).image_view;
                state.depth_stencil_attachment.image_layout = depth_image.backing->state.layout;
            }
        } else {
            state.width = redirect.width;
            state.height = redirect.height;
            // The UI movie viewport is now 1920x1080 even when the scene is 960x540.
            // A native viewport (or native window-space UI vertices) must not be doubled
            // again. The final display copy still uses the guest render-size coordinates.
            const auto& viewport = regs.viewports[0];
            const bool native_coordinates = redirect.native_ui &&
                (UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2) ||
                 (regs.IsClipDisabled() && !regs.viewport_control.xscale_enable &&
                  !regs.viewport_control.yscale_enable));
            target_scale = UiComposition::Scale(guest_extent.first, guest_extent.second,
                                                redirect.width, redirect.height,
                                                native_coordinates);
        }
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    if (reduced) {
        // Half-resolution passes keep the scene's factor (proxy / native = scene / native).
        const auto size = scene_targets->Size();
        const auto native = SceneResolution::Native();
        const auto proxy = pass_size.value_or(size);
        state.width = proxy.width;
        state.height = proxy.height;
        target_scale = {float(size.width) / float(native.width),
                        float(size.height) / float(native.height)};
    }
    if (FrameCapture::Active()) {
        std::array<const VideoCore::ImageInfo*, AmdGpu::NUM_COLOR_BUFFERS> colors{};
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            if (cb_descs[cb].first) {
                colors[cb] = &texture_cache.GetImage(cb_descs[cb].first).info;
            }
        }
        const auto* depth =
            db_desc.first ? &texture_cache.GetImage(db_desc.first).info : nullptr;
        FrameCapture::BeginPass(colors.data(), state.num_color_attachments, depth);
    }
    // gow3: object motion vector attachment of G-buffer pipelines.
    if (key.motion_vectors) {
        object_motion->Attach(state, state.width, state.height);
    }
    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = CbExtent(0);
    const auto& mrt1_hint = CbExtent(1);
    VideoCore::TextureCache::ImageDesc mrt0_desc{Regs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{Regs().color_buffers[1], mrt1_hint};
    if (const u32 max_layers = instance.GetMaxImageArrayLayers();
        !PlausibleTarget(mrt0_desc.info, max_layers, "resolve source") ||
        !PlausibleTarget(mrt1_desc.info, max_layers, "resolve target")) {
        return; // gow3 (#63): garbage registers, see PlausibleTarget
    }
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 Regs().color_buffers[0].Address(),
                                 Regs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = Regs();

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), true);
    if (const u32 max_layers = instance.GetMaxImageArrayLayers();
        !PlausibleTarget(read_desc.info, max_layers, "depth copy source") ||
        !PlausibleTarget(write_desc.info, max_layers, "depth copy target")) {
        return; // gow3 (#63): garbage registers, see PlausibleTarget
    }

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = Regs().depth_view.slice_start;
    sub_range.extent.layers = Regs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    DrainDrawPipe();
    if (!is_gds) {
        Gow3WatchRange::Note("fill", value, 0, address, num_bytes);
    }
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
        // gow3: small fills into pages holding other GPU data (fence and frame labels the game
        // reads on the CPU) land in guest memory too, not only in the GPU's copy.
        if (num_bytes <= 64_KB) {
            const std::vector<u32> pattern(num_bytes / sizeof(u32), value);
            if (buffer_cache.WriteThrough(address, pattern.data(), num_bytes)) {
                return;
            }
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
    // gow3: GPU-side writes into guest memory come back with this submission's readback.
    if (!is_gds && sync_gpu_constants && num_bytes <= 4096 && small_gpu_writes.size() < 16384) {
        small_gpu_writes.emplace_back(address, num_bytes);
        VideoCore::NoteReadbackKind(address, num_bytes, VideoCore::ReadbackCpWrite);
    }
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    DrainDrawPipe();
    // gow3: the two GPU-modified lookups only choose the watch line's text (GOW3_WATCH_RANGE).
    if (!dst_gds && !Gow3WatchRange::Ranges().empty()) {
        Gow3WatchRange::Note(buffer_cache.IsRegionGpuModified(dst, num_bytes) ||
                                     (!src_gds && buffer_cache.IsRegionGpuModified(src, num_bytes))
                                 ? "dma copy (gpu path)"
                                 : "dma copy",
                             src, 0, dst, num_bytes);
    }
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    // gow3: a small copy whose source bytes are current in guest memory, into a page holding
    // other GPU data: made in guest memory and in the GPU's copy alike (BufferCache::WriteThrough)
    // so the CPU sees it without a download.
    if (!dst_gds && !src_gds && num_bytes <= 64_KB && dst % 4 == 0 && num_bytes % 4 == 0 &&
        IsMapped(src, num_bytes) && !buffer_cache.IsRangeGpuWritten(src, num_bytes) &&
        !texture_cache.FindImageFromRange(src, num_bytes)) {
        std::vector<u8> data(num_bytes);
        if (!memory->TryReadBacking(src, data.data(), num_bytes)) {
            std::memcpy(data.data(), std::bit_cast<const void*>(src), num_bytes);
        }
        if (buffer_cache.WriteThrough(dst, data.data(), num_bytes)) {
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
    // gow3: GPU-side copies into guest memory (GDS counters the CPU reads) come back with this
    // submission's readback, so the CPU's writes next to them wait for that readback at most.
    if (!dst_gds && sync_gpu_constants && num_bytes <= 4096 && small_gpu_writes.size() < 16384) {
        small_gpu_writes.emplace_back(dst, num_bytes);
        VideoCore::NoteReadbackKind(dst, num_bytes, VideoCore::ReadbackCpWrite);
    }
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    DrainDrawPipe();
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    DrainDrawPipe();
    // gow3 22 Oct 2026: write faults (HandleWriteAt), file reads, decoded video, guest mprotect.
    RingUnwatch(addr, size, true);
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    if (g_wf_split_active) {
        const auto t0 = std::chrono::steady_clock::now();
        texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuWrite);
        const auto t1 = std::chrono::steady_clock::now();
        buffer_cache.InvalidateMemory(addr, size, assume_locks);
        const auto t2 = std::chrono::steady_clock::now();
        texture_cache.InvalidateMemory(addr, size);
        const auto t3 = std::chrono::steady_clock::now();
        using std::chrono::duration_cast, std::chrono::nanoseconds;
        VideoCore::g_wf_split_ns[0].fetch_add(duration_cast<nanoseconds>(t2 - t1).count(),
                                              std::memory_order_relaxed);
        VideoCore::g_wf_split_ns[1].fetch_add(
            duration_cast<nanoseconds>((t1 - t0) + (t3 - t2)).count(), std::memory_order_relaxed);
        return true;
    }
    texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuWrite);
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::OnWriteFault(VAddr addr, bool assume_locks) {
    // gow3: GOW3_FAULT_STATS=0 is off, as for the other fault statistics.
    static const bool stats = [] {
        const char* env = std::getenv("GOW3_FAULT_STATS");
        return env && env[0] != '0';
    }();
    if (stats && OnStageA()) {
        static std::array<u64, 8> sites{};
        static std::map<u64, u64> windows;
        static auto last = std::chrono::steady_clock::now();
        ++sites[std::clamp(g_gpu_write_site, 0, 7)];
        ++windows[addr >> 20];
        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
            last = now;
            std::string text;
            for (const auto& [window, n] : windows) {
                text += fmt::format(" {:#x}:{}", window << 20, n);
            }
            std::printf("GPU thread write faults (5 s): WriteData %llu, inline %llu, compute %llu, DMA %llu, "
                        "fence %llu, other %llu; windows%s\n",
                        (unsigned long long)sites[1], (unsigned long long)sites[2],
                        (unsigned long long)sites[3], (unsigned long long)sites[4],
                        (unsigned long long)sites[5], (unsigned long long)sites[0], text.c_str());
            std::fflush(stdout);
            sites = {};
            windows.clear();
        }
    }
    // gow3 21 Oct 2026 diagnostics (GOW3_FAULT_TIMES=1, write-fault hitches on the laptop): how
    // long each write fault's handling takes, by step, and the slowest ones per 5 s.
    static const bool times = [] {
        const char* env = std::getenv("GOW3_FAULT_TIMES");
        return env && env[0] == '1';
    }();
    if (!times) {
        DrainDrawPipe();
        WaitDrawPipeForCapture();
        return HandleWriteAt(addr, assume_locks);
    }
    // InvalidateMemory's steps, timed one by one.
    const auto t0 = std::chrono::steady_clock::now();
    DrainDrawPipe();
    WaitDrawPipeForCapture();
    RingUnwatch(addr, 8, true); // as InvalidateMemory does
    if (!IsMapped(addr, 8)) {
        return false;
    }
    texture_cache.ResolveReadbacks(addr, 8, assume_locks, VideoCore::ReadbackSource::CpuWrite);
    const auto t1 = std::chrono::steady_clock::now();
    buffer_cache.InvalidateMemory(addr, 8, assume_locks);
    const auto t2 = std::chrono::steady_clock::now();
    texture_cache.InvalidateMemory(addr, 8);
    const auto t3 = std::chrono::steady_clock::now();
    buffer_cache.ExtendWriteFault(addr);
    const auto t4 = std::chrono::steady_clock::now();
    LandWatchedReadback(addr, assume_locks);
    const auto t5 = std::chrono::steady_clock::now();
    const std::array<u64, 5> steps{u64((t1 - t0).count()), u64((t2 - t1).count()),
                                   u64((t3 - t2).count()), u64((t4 - t3).count()),
                                   u64((t5 - t4).count())};
    NoteWriteFaultTime(addr, assume_locks, steps);
    return true;
}

bool Rasterizer::HandleWriteAt(VAddr addr, bool assume_locks) {
    static const bool split_stats = [] {
        const char* env = std::getenv("GOW3_FAULT_STATS");
        return env && env[0] == '1';
    }();
    if (split_stats) {
        // Same steps as below, timed; sums are printed by the page manager's 5 s fault stats.
        using Clock = std::chrono::steady_clock;
        const auto ns = [](Clock::time_point a, Clock::time_point b) {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        };
        g_wf_split_active = true; // InvalidateMemory splits its buffer / texture parts
        const bool ok = InvalidateMemory(addr, 8, assume_locks);
        g_wf_split_active = false;
        if (!ok) {
            return false;
        }
        const auto t0 = Clock::now();
        buffer_cache.ExtendWriteFault(addr);
        const auto t1 = Clock::now();
        LandWatchedReadback(addr, assume_locks);
        const auto t2 = Clock::now();
        VideoCore::g_wf_split_ns[2].fetch_add(ns(t0, t1), std::memory_order_relaxed);
        VideoCore::g_wf_split_ns[3].fetch_add(ns(t1, t2), std::memory_order_relaxed);
        VideoCore::g_wf_split_calls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!InvalidateMemory(addr, 8, assume_locks)) {
        return false;
    }
    buffer_cache.ExtendWriteFault(addr);
    LandWatchedReadback(addr, assume_locks);
    return true;
}

bool Rasterizer::DiscardCpuWritePage(VAddr page) {
    if (!Gow3PageOnlyWriteWatched(page) || !IsMapped(page, 4096)) {
        return false;
    }
    // Check before the buffer discard takes its locks; GPU-owned images keep HandleWriteAt.
    if (texture_cache.PageHasGpuImageAuthority(page, 4096)) {
        return false;
    }
    // Texture copies may protect only writes (stale readbacks). Resolving one keeps that page
    // on the old path; pages actually discarded never wait for a GPU copy.
    if (texture_cache.ResolveReadbacks(page, 4096, true, VideoCore::ReadbackSource::CpuWrite) ||
        !buffer_cache.DiscardCpuWritePage(page)) {
        return false;
    }
    texture_cache.InvalidateMemory(page, 4096);
    return true;
}

// gow3 21 Oct 2026: the player's U3 chapter load: up to 6,348 write faults/s from the game's file
// reads (touch_for_write: one store per 4 KiB page), 1,049 one-page protects in a 103 ms frame
// (96.4 ms in write-fault handlers), a 215 ms frame with 203 ms in them. The faults the touches
// would cause, minus the exception dispatch: the same per-address work (HandleWriteAt) at the
// touch addresses whose page is still protected, one drain before the loop (InvalidateMemory
// drains per call anyway). One InvalidateMemory over the whole range was rejected (21 Oct review):
// BufferCache::ReadMemory fetches only the first page's GPU bytes before marking the whole range
// CPU-modified, and the bulk readback cancellation rounds out to pages, dropping GPU authority
// for untouched boundary bytes.
bool Rasterizer::PrepareCpuWrite(VAddr addr, u64 size) {
    if (!size) {
        return true;
    }
    if (!Gow3AnyPageWatched(addr, size)) {
        return true; // Nothing protected: the copy faults nowhere, and no GPU bytes to bring back.
    }
    const auto start = std::chrono::steady_clock::now();
    const bool assume_locks = IsGpuSideThread();
    // As in the fault handler (page_manager.cpp): interrupts landed here wait for this thread's
    // next safe point, it may hold cache locks the interrupt handlers take.
    std::optional<Platform::IrqController::HoldScope> hold_interrupts;
    if (assume_locks) {
        hold_interrupts.emplace();
    }
    DrainDrawPipe();
    WaitDrawPipeForCapture();
    const VAddr end = addr + size;
    u64 pages = 0;
    std::vector<VAddr> discarded;
    if (!Gow3Toggle::Disabled2(Gow3Toggle::FileReadDiscard) &&
        (!assume_locks || OnStageA())) {
        for (VAddr at = addr; at < end; at = Common::AlignDown(at, VAddr{4096}) + 4096) {
            if ((at & 4095) == 0 && end - at >= 4096 && Gow3PageOnlyWriteWatched(at) &&
                buffer_cache.IsRangeGpuWrittenShared(at, 4096)) {
                discarded.push_back(at);
            }
        }
        if (!discarded.empty()) {
            // 21 Oct U3: 532 pages/50.1 ms. One queue hop per read, without fetching each page;
            // Liverpool drains the draw pipe before commands, exactly as for flush_request.
            const auto discard = [this, &discarded] {
                std::erase_if(discarded, [this](VAddr page) { return !DiscardCpuWritePage(page); });
            };
            if (assume_locks) {
                discard();
            } else {
                liverpool->SendCommand<true>(discard, "file-discard");
            }
        }
    }
    pages = discarded.size();
    // The touch addresses of touch_for_write: the buffer itself, then each page base after it.
    for (VAddr at = addr; at < end; at = Common::AlignDown(at, VAddr{4096}) + 4096) {
        if (std::binary_search(discarded.begin(), discarded.end(), at)) {
            continue;
        }
        if (!Gow3PageOnlyWriteWatched(at)) {
            // Unprotected (maybe by an earlier window): the touch would not fault. Read-watched:
            // left to the touches (21 Oct review: their load takes the read fault, which lands
            // the readback, before any write fault).
            continue;
        }
        if (!HandleWriteAt(at, assume_locks)) {
            return false; // The page touches handle what faults.
        }
        ++pages;
    }
    g_range_prepare_pages.fetch_add(pages, std::memory_order_relaxed);
    VideoCore::g_range_prepare_discarded.fetch_add(discarded.size(), std::memory_order_relaxed);
    g_range_prepares.fetch_add(1, std::memory_order_relaxed);
    g_range_prepare_bytes.fetch_add(size, std::memory_order_relaxed);
    g_range_prepare_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
            .count(),
        std::memory_order_relaxed);
    return true;
}

void Rasterizer::NoteWriteFaultTime(VAddr addr, bool gpu_side, const std::array<u64, 5>& steps) {
    static constexpr u64 Limits[] = {20'000, 100'000, 1'000'000, 10'000'000, ~0ull};
    static std::atomic<u64> counts[5], sums[5], step_ns[5], gpu_side_count;
    static std::mutex slow_mutex;
    static std::vector<std::tuple<u64, VAddr, std::array<u64, 5>, bool>> slow;
    static std::atomic<s64> last_report{0};
    u64 total = 0;
    for (u32 i = 0; i < 5; ++i) {
        total += steps[i];
        step_ns[i].fetch_add(steps[i], std::memory_order_relaxed);
    }
    u32 bucket = 0;
    while (total >= Limits[bucket]) {
        ++bucket;
    }
    counts[bucket].fetch_add(1, std::memory_order_relaxed);
    sums[bucket].fetch_add(total, std::memory_order_relaxed);
    gpu_side_count.fetch_add(gpu_side, std::memory_order_relaxed);
    if (total >= 1'000'000) {
        std::scoped_lock lock{slow_mutex};
        slow.emplace_back(total, addr, steps, gpu_side);
    }
    const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = last_report.load(std::memory_order_relaxed);
    if (last == 0) {
        last_report.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 5 || !last_report.compare_exchange_strong(last, now)) {
        return;
    }
    u64 n[5], ns[5], all = 0, all_ns = 0;
    for (u32 i = 0; i < 5; ++i) {
        n[i] = counts[i].exchange(0);
        ns[i] = sums[i].exchange(0);
        all += n[i];
        all_ns += ns[i];
    }
    std::string text = fmt::format(
        "Write fault times (5 s): {} faults ({} GPU-side), {:.1f} ms; <20us {} {:.1f} ms, <100us {} "
        "{:.1f} ms, <1ms {} {:.1f} ms, <10ms {} {:.1f} ms, >=10ms {} {:.1f} ms; steps: image "
        "readbacks {:.1f} ms, buffers {:.1f} ms, images {:.1f} ms, extend {:.1f} ms, land {:.1f} ms",
        all, gpu_side_count.exchange(0), all_ns / 1e6, n[0], ns[0] / 1e6, n[1], ns[1] / 1e6, n[2],
        ns[2] / 1e6, n[3], ns[3] / 1e6, n[4], ns[4] / 1e6, step_ns[0].exchange(0) / 1e6,
        step_ns[1].exchange(0) / 1e6, step_ns[2].exchange(0) / 1e6, step_ns[3].exchange(0) / 1e6,
        step_ns[4].exchange(0) / 1e6);
    {
        std::scoped_lock lock{slow_mutex};
        std::ranges::sort(slow, std::greater{});
        for (size_t i = 0; i < std::min<size_t>(slow.size(), 8); ++i) {
            const auto& [t, a, s, side] = slow[i];
            text += fmt::format("\n  slow write fault {:#x}: {:.2f} ms (image readbacks {:.2f}, "
                                "buffers {:.2f}, images {:.2f}, extend {:.2f}, land {:.2f}){}",
                                a, t / 1e6, s[0] / 1e6, s[1] / 1e6, s[2] / 1e6, s[3] / 1e6,
                                s[4] / 1e6, side ? " GPU-side" : "");
        }
        slow.clear();
    }
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

void Rasterizer::ReleaseStuckPage(VAddr page, bool is_write, bool assume_locks) {
    DrainDrawPipe();
    buffer_cache.ReleaseStuckPage(page, is_write, assume_locks);
}

// gow3 18 Oct 2026: a fault handled while a pending readback still read-protects the page would
// fault again on the retried access (BufferCache::LandReadbacksWatching): land that readback.
void Rasterizer::LandWatchedReadback(VAddr addr, bool assume_locks) {
    if (!page_manager.IsReadWatched(addr)) {
        return;
    }
    static std::atomic<u32> landed{0};
    if (const u32 n = landed.fetch_add(1, std::memory_order_relaxed); n < 8 || (n & (n - 1)) == 0) {
        std::printf("Readback watch: a fault at %#llx left its page read-protected: landing the "
                    "readback (%u so far)\n",
                    static_cast<unsigned long long>(addr), n + 1);
        std::fflush(stdout);
    }
    const VAddr page = Common::AlignDown(addr, VAddr{4096});
    if (assume_locks) {
        buffer_cache.LandReadbacksWatching(page);
        return;
    }
    // gow3 21 Oct 2026 (GPU worker): GOW3_LAND_ON_GUEST=1 - the faulting guest thread waits for
    // the GPU itself instead of the command thread. LandReadbacksWatching waits for the readback's
    // submission on the command thread (~5 ms each on the desktop's Radeon iGPU, 20-50 per 5 s in
    // Uncharted 2): meanwhile nothing new reached the GPU, which ran dry and then idled until the
    // next submission (2.5-3.4 ms of idle GPU per 33 ms frame). Here the command thread only lands
    // what has finished and names the newest readback still holding the page; the guest thread
    // (holding no cache locks) waits for that submission, then asks again (other readbacks may
    // hold the page by then). It never waits for work not yet submitted (the command thread might
    // itself be waiting for this thread): the old path then, as after 4 rounds.
    static const bool on_guest = [] {
        const char* env = std::getenv("GOW3_LAND_ON_GUEST");
        return env && env[0] == '1';
    }();
    if (on_guest) {
        static std::atomic<u32> done_at_once{0}, waited{0}, fallback{0};
        static std::atomic<u64> wait_us{0};
        static std::atomic<s64> last_report{0};
        const auto report = [] {
            const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            s64 last = last_report.load(std::memory_order_relaxed);
            if (now - last >= 5 && last_report.compare_exchange_strong(last, now)) {
                std::printf("Readback watch landings (5 s): %u found landed, %u after the guest "
                            "thread waited (%.1f ms), %u on the command thread\n",
                            done_at_once.exchange(0), waited.exchange(0), wait_us.exchange(0) / 1e3,
                            fallback.exchange(0));
                std::fflush(stdout);
            }
        };
        auto* semaphore = scheduler.GetWorkSemaphore();
        bool waited_here = false;
        for (int round = 0; round < 4; ++round) {
            u64 tick = 0;
            bool submitted = false;
            liverpool->SendCommand<true>(
                [this, page, &tick, &submitted] {
                    buffer_cache.PollReadbacksNow();
                    tick = buffer_cache.WatchingTick(page);
                    submitted = tick != 0 && tick < scheduler.CurrentTick();
                },
                "fault-watch");
            if (tick == 0) {
                if (page_manager.IsReadWatched(page)) {
                    break; // a watch no readback owns: the old path reports it
                }
                ++(waited_here ? waited : done_at_once);
                report();
                return;
            }
            if (!submitted) {
                break;
            }
            const auto start = std::chrono::steady_clock::now();
            semaphore->Wait(tick);
            wait_us += std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
            waited_here = true;
        }
        ++fallback;
        report();
    }
    liverpool->SendCommand<true>([this, page] { buffer_cache.LandReadbacksWatching(page); },
                                 "fault-land");
}

// gow3 18 Oct 2026: a shader capture for the GPU command thread (which waits for it) handles its
// faults inline and may land readbacks, which the draw recording thread owns while it runs: with
// multithreaded rendering both landed the same readback (staging buffer freed twice, page watcher
// count below zero, heap corruption). The command thread pushes nothing meanwhile, so the
// recording thread runs dry: the capture waits for that first (logged after 2 s).
void Rasterizer::WaitDrawPipeForCapture() {
    if (!draw_pipe || !PipelineCache::capturing_for_gpu_thread || DrawPipe::OnStageB()) {
        return;
    }
    const auto warn_at = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool warned = false;
    while (!draw_pipe->Idle()) {
        if (!warned && std::chrono::steady_clock::now() >= warn_at) {
            warned = true;
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
                std::printf("Draw pipe: shader capture fault waiting for the recording thread "
                            "for 2 s\n");
            }
        }
        std::this_thread::yield();
    }
}

// gow3 23 Oct 2026 (U3 Cartagena street, 4K with FG; mode-1 async readbacks): guest read faults
// on read-protected readback pages cost ~950 ms per 5 s across the game's threads, 40.6 real FPS
// against 50.9 with the pages left readable (unsafe: stale reads). About 70% read only the CPU's
// own bytes beside the GPU bytes (~1.3 ms each through the command thread, which then unwatches
// the page); the rest read GPU bytes still on their way back, and the command thread waited ~10
// ms for the GPU per fault, feeding it nothing meanwhile. Here the faulting thread does the first
// alone and waits for the second itself (toggles GuestFalseSharing, GuestReadPrewait).
int Rasterizer::GuestReadFault(VAddr addr) {
    const bool open = !Gow3Toggle::Disabled2(Gow3Toggle::GuestFalseSharing);
    const bool prewait = !Gow3Toggle::Disabled2(Gow3Toggle::GuestReadPrewait);
    if (!open && !prewait && !Gow3Stats::fault_stats) {
        return 0;
    }
    const auto read = buffer_cache.LookupWatchedRead(addr, 8);
    if (read.owners == 0) {
        return 0; // not a buffer readback's watch (or landed meanwhile): the usual path
    }
    const u64 tick = std::max(read.tick, read.strict_tick);
    if (!tick) {
        return open && buffer_cache.OpenFalseSharingPage(addr, read.owners) ? 1 : 0;
    }
    const auto start = std::chrono::steady_clock::now();
    if (prewait && tick < scheduler.CurrentTick()) {
        scheduler.GetWorkSemaphore()->Wait(tick);
    }
    // A strict forwarded-copy guard can precede submission. Do NOT wait on that tick here:
    // the normal fault command runs Scheduler::Wait, which flushes the open command buffer.
    // GOW3_FAULT_STATS: how old the readback was when the guest read its GPU bytes (queued at the
    // submission that holds the GPU work), and how long the guest then waited for the GPU.
    if (Gow3Stats::fault_stats) {
        static std::atomic<u64> count{0}, age_us{0}, wait_us{0}, wait_max_us{0};
        static std::atomic<s64> last_s{0};
        const auto now = std::chrono::steady_clock::now();
        const auto us = [](auto d) {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
        };
        ++count;
        age_us += us(start - read.queued);
        const u64 waited = us(now - start);
        wait_us += waited;
        for (u64 m = wait_max_us.load(); waited > m && !wait_max_us.compare_exchange_weak(m, waited);) {
        }
        const s64 second =
            std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        if (s64 last = last_s.load(); second - last >= 5 && last_s.compare_exchange_strong(last, second)) {
            const u64 n = count.exchange(0);
            std::printf("Guest reads of GPU bytes on their way back (5 s): %llu, readback queued %.2f "
                        "ms before on average, then waited %.2f ms on average (%.2f at most)\n",
                        static_cast<unsigned long long>(n), n ? age_us.exchange(0) / 1e3 / n : 0.0,
                        n ? wait_us.exchange(0) / 1e3 / n : 0.0, wait_max_us.exchange(0) / 1e3);
            std::fflush(stdout);
        }
    }
    return 2;
}

// gow3 23 Oct 2026 (U3 Cartagena street): after GuestReadFault the guest threads still waited
// ~9.5 ms per read of GPU bytes, 74-108 per 5 s, all of them one memcpy in
// PrepareInstanceForRendering (fg-draw-mgr.cpp, guest 0xe3c6e7): the instance's bone matrices
// (written by the cloth solvers cs 1c957b01 / 809864d3) copied into the frame's skinning palette,
// which the GPU's skinning reads. The GPU makes that copy instead, in stream order after the
// solver; the palette's CPU view comes back with the submission's small-write readback.
bool Rasterizer::ForwardGuestCopy(VAddr dst, VAddr src, u64 size) {
    if (!size || size > 1_MB || src + size < src || dst + size < dst ||
        ((src | dst | size) & 3) || (dst < src + size && src < dst + size) || IsGpuSideThread()) {
        return false;
    }
    const VAddr first_page = Common::AlignDown(src, VAddr{4096});
    bool watched = false;
    for (VAddr page = first_page; page < src + size; page += 4096) {
        if (page_manager.IsReadWatched(page)) {
            watched = true;
            break;
        }
    }
    if (!watched) {
        return false;
    }
    bool gpu_bytes = false;
    for (VAddr page = first_page; page < src + size && !gpu_bytes; page += 4096) {
        const VAddr lo = std::max(page, src);
        const VAddr hi = std::min(page + 4096, src + size);
        gpu_bytes = buffer_cache.LookupWatchedRead(lo, hi - lo).tick != 0;
    }
    // Only beside GPU bytes (false sharing): the plain copy opens the pages on this thread.
    if (!gpu_bytes || !IsMapped(dst, size) ||
        !IsMapped(src, size)) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    bool done = false;
    std::vector<u8> expected;
    liverpool->SendFaultCommand(
        [this, dst, src, size, &done, &expected] {
            DrainDrawPipe();
            // Landed meanwhile, or bytes the texture cache owns: the guest copies as usual.
            if (!buffer_cache.IsRegionGpuModified(src, size) ||
                texture_cache.FindImageFromRange(src, size) ||
                texture_cache.FindImageFromRange(dst, size)) {
                return;
            }
            // A diagnostic copy checks the destination's immediate CPU-consumer contract,
            // including a false-sharing read first. Capture a known source BEFORE the GPU
            // copy; only this sampled verifier deliberately waits/lands the source early.
            static const bool verify = [] {
                const char* env = std::getenv("GOW3_FORWARD_COPY_VERIFY");
                return env && env[0] == '1';
            }();
            static u64 copies = 0;
            if (verify && ((copies++ & 127) == 0)) {
                const std::pair<VAddr, u64> source{src, size};
                buffer_cache.DownloadRanges(std::span{&source, 1});
                expected.resize(size);
                if (!memory->TryReadBacking(src, expected.data(), size)) {
                    memory->CopySparseMemory(src, expected.data(), size);
                }
            }
            texture_cache.InvalidateMemoryFromGPU(dst, size);
            const u32 bytes = static_cast<u32>(size);
            const auto [src_buffer, src_offset] = buffer_cache.ObtainBuffer(src, bytes, false, true);
            const auto [dst_buffer, dst_offset] = buffer_cache.ObtainBuffer(dst, bytes, true, true);
            const vk::BufferCopy copy = {
                .srcOffset = src_offset,
                .dstOffset = dst_offset,
                .size = size,
            };
            runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
            // memcpy is not an asynchronous API: protect its destination BEFORE returning.
            // The deferred prototype protects even before submission, without allowing a
            // false-sharing read to open the page. A CPU consumer submits/lands it on demand;
            // GPU-only consumers use the queued result without a per-copy submission/drain.
            const std::pair<VAddr, u64> range{dst, size};
            static const bool defer_requested = [] {
                const char* env = std::getenv("GOW3_FORWARD_COPY_DEFER");
                return env && env[0] == '1';
            }();
            const bool deferred = defer_requested &&
                                  !Gow3Toggle::Disabled2(Gow3Toggle::ForwardCopyDefer);
            buffer_cache.DownloadRangesAsync(std::span{&range, 1}, true, false, false, deferred);
            VideoCore::NoteReadbackKind(dst, size, VideoCore::ReadbackCpWrite);
            if (!deferred) {
                Flush();
                buffer_cache.PollReadbacksNow();
            }
            done = true;
        },
        "forward-copy");
    if (done && !expected.empty()) {
        if ((dst & 4095) && IsMapped(dst - 1, 1)) {
            const volatile u8 neighbour = *reinterpret_cast<const volatile u8*>(dst - 1);
            (void)neighbour;
        }
        std::vector<u8> actual(size);
        std::memcpy(actual.data(), reinterpret_cast<const void*>(dst), size); // MUST fault/land if pending
        const bool match = actual == expected;
        std::printf("Forwarded copy guard verify: %s, %llu bytes at %#llx (immediate CPU read)\n",
                    match ? "OK" : "MISMATCH", size, dst);
        ASSERT_MSG(match, "forwarded memcpy exposed stale destination bytes");
    }
    if (Gow3Stats::fault_stats) {
        static std::atomic<u64> forwarded{0}, declined{0}, bytes{0}, us{0};
        static std::atomic<s64> last_s{0};
        ++(done ? forwarded : declined);
        bytes += done ? size : 0;
        const auto now = std::chrono::steady_clock::now();
        us += static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
        const s64 second =
            std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        if (s64 last = last_s.load(); second - last >= 5 && last_s.compare_exchange_strong(last, second)) {
            const u64 n = forwarded.exchange(0), m = declined.exchange(0);
            std::printf("Forwarded guest copies (5 s): %llu made by the GPU (%.1f KB), %llu left to the "
                        "guest; %.2f ms on average for the command thread\n",
                        static_cast<unsigned long long>(n), bytes.exchange(0) / 1024.0,
                        static_cast<unsigned long long>(m), n + m ? us.exchange(0) / 1e3 / (n + m) : 0.0);
            std::fflush(stdout);
        }
    }
    return done;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    DrainDrawPipe();
    WaitDrawPipeForCapture();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    const bool image_readback =
        texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuRead);
    if (!image_readback || buffer_cache.IsRegionGpuModified(addr, size)) {
        buffer_cache.ReadMemory(addr, size, false, assume_locks);
    }
    LandWatchedReadback(addr, assume_locks);
    return true;
}

void Rasterizer::ProcessDownloadImages() {
    DrainDrawPipe();
    texture_cache.ProcessDownloadImages();
    for (const auto& range : texture_cache.TakeTinyReadbacks()) {
        if (eop_soft_readbacks.size() < 64) {
            eop_soft_readbacks.push_back(range);
        }
    }
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    // gow3: mapped intervals found before, per thread, while no map/unmap happened since
    // (Gow3Toggle::IsMappedCache). A range inside one is mapped without taking the lock.
    struct CachedInterval {
        VAddr lo = 0, hi = 0;
        u64 generation = ~0ull;
    };
    thread_local std::array<CachedInterval, 8> cached{};
    thread_local u32 next_slot = 0;
    const bool use_cache = !Gow3Toggle::Disabled(Gow3Toggle::IsMappedCache);
    if (use_cache) {
        const u64 generation = mapped_generation.load(std::memory_order_acquire);
        for (const auto& entry : cached) {
            if (entry.generation == generation && addr >= entry.lo && addr + size <= entry.hi) {
                return true;
            }
        }
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    if (!use_cache) {
        return boost::icl::contains(mapped_ranges, range);
    }
    // Read under the lock: map/unmap change it while holding it exclusively.
    const u64 generation = mapped_generation.load(std::memory_order_acquire);
    const auto it = mapped_ranges.find(addr);
    if (it == mapped_ranges.end() || it->upper() < addr + size) {
        return false;
    }
    cached[next_slot++ % cached.size()] = {it->lower(), it->upper(), generation};
    return true;
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_generation.fetch_add(1, std::memory_order_acq_rel);
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        mapped_generation.fetch_add(1, std::memory_order_acq_rel);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

} // namespace Vulkan
namespace VideoCore {
extern std::atomic<u64> g_invalidate_downloads, g_invalidate_download_ns;
}
namespace Vulkan {

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    // gow3 19 Oct 2026: time of each part (U1: ~23 ms per 1 MiB unmap, a 1.8 s hitch before a
    // cutscene); a line when they took 20 ms or more since the last one (at most every 0.25 s).
    using Clock = std::chrono::steady_clock;
    static std::atomic<u64> unmaps{0}, drain_ns{0}, buffer_ns{0}, texture_ns{0};
    static std::atomic<s64> last_report{0};
    const auto ns = [](Clock::duration d) {
        return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    };
    const auto t0 = Clock::now();
    DrainDrawPipe();
    RingUnwatch(addr, size, false); // gow3 22 Oct 2026: stage A's own watches go too
    const auto t1 = Clock::now();
    buffer_cache.InvalidateMemory(addr, size);
    const auto t2 = Clock::now();
    texture_cache.UnmapMemory(addr, size);
    const auto t3 = Clock::now();
    unmaps.fetch_add(1, std::memory_order_relaxed);
    drain_ns.fetch_add(ns(t1 - t0), std::memory_order_relaxed);
    buffer_ns.fetch_add(ns(t2 - t1), std::memory_order_relaxed);
    texture_ns.fetch_add(ns(t3 - t2), std::memory_order_relaxed);
    const s64 now = s64(ns(t3.time_since_epoch()));
    s64 last = last_report.load(std::memory_order_relaxed);
    if (now - last >= 250'000'000 &&
        drain_ns.load(std::memory_order_relaxed) + buffer_ns.load(std::memory_order_relaxed) +
                texture_ns.load(std::memory_order_relaxed) >= 20'000'000 &&
        last_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        std::printf("GPU: unmap time: %llu unmaps; draw pipe %.1f ms, buffer cache %.1f ms (%llu "
                    "downloads of GPU data, %.1f ms), texture cache %.1f ms\n",
                    (unsigned long long)unmaps.exchange(0), drain_ns.exchange(0) / 1e6,
                    buffer_ns.exchange(0) / 1e6,
                    (unsigned long long)VideoCore::g_invalidate_downloads.exchange(0),
                    VideoCore::g_invalidate_download_ns.exchange(0) / 1e6,
                    texture_ns.exchange(0) / 1e6);
        std::fflush(stdout);
    }
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_generation.fetch_add(1, std::memory_order_acq_rel);
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        mapped_generation.fetch_add(1, std::memory_order_acq_rel);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    {
        const auto* vs = pipeline ? pipeline->GetStages()[u32(Shader::SwStage::Vertex)] : nullptr;
        dynamic_vs_hash = vs ? u32(vs->pgm_hash) : 0;
        const auto* ps = pipeline ? pipeline->GetStages()[u32(Shader::SwStage::Fragment)] : nullptr;
        dynamic_ps_hash = ps ? u32(ps->pgm_hash) : 0;
    }
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.CommitWith(instance.IsDepthBoundsSupported(),
                             instance.IsDynamicColorWriteMaskSupported(),
                             instance.IsAttachmentFeedbackLoopLayoutSupported(),
                             [&](auto&& command) { scheduler.Record(std::move(command)); });
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = Regs();

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            // gow3: sub-pixel jitter of scene geometry for the temporal upscaler; the same
            // shift as jittering the projection.
            viewport.x = (xoffset - xscale) * target_scale[0] + draw_jitter[0];
            viewport.y = (yoffset - yscale) * target_scale[1] + draw_jitter[1];
            viewport.width = xscale * 2.0f * target_scale[0];
            viewport.height = yscale * 2.0f * target_scale[1];
            if (hud_k < 1.0f) {
                const float x0 = HudLayout::Place(viewport.x, float(hud_width), float(hud_height),
                                                hud_k, hud_anchor, hud_wide_aspect);
                const float x1 = HudLayout::Place(viewport.x + viewport.width, float(hud_width),
                                                  float(hud_height), hud_k, hud_anchor,
                                                  hud_wide_aspect);
                viewport.x = x0;
                viewport.width = x1 - x0;
            }
        }

        // gow3 diagnostic ("clear far" in GOW3_DMA_TRIGGER): Uncharted's shadow map clear quad
        // (vs a7fbc2a8, z = -1) writes depth 1.0 instead of the clamped 0.
        if (shadow_clear_far.load(std::memory_order_relaxed) && dynamic_vs_hash == 0xa7fbc2a8u) {
            viewport.minDepth = viewport.maxDepth = 1.0f;
        }
        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        if (target_scale[0] != 1.0f || target_scale[1] != 1.0f) {
            const auto scale = [](s32 v, float f) { return s32(std::lround(float(v) * f)); };
            const s32 x0 = scale(vp_scsr.top_left_x, target_scale[0]);
            const s32 y0 = scale(vp_scsr.top_left_y, target_scale[1]);
            const s32 x1 = scale(vp_scsr.top_left_x + s32(vp_scsr.GetWidth()), target_scale[0]);
            const s32 y1 = scale(vp_scsr.top_left_y + s32(vp_scsr.GetHeight()), target_scale[1]);
            scissors.push_back({
                .offset = {x0, y0},
                .extent = {u32(std::max(x1 - x0, 0)), u32(std::max(y1 - y0, 0))},
            });
        } else {
            scissors.push_back({
                .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
                .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
            });
        }
        if (hud_k < 1.0f && (hud_split || !regs.IsClipDisabled())) {
            auto& s = scissors.back();
            const s32 game_x0 = s.offset.x, game_x1 = s.offset.x + s32(s.extent.width);
            const auto columns =
                hud_split ? HudLayout::PassColumns(hud_anchor, game_x0, game_x1, hud_width,
                                                   hud_height, hud_k, hud_wide_aspect)
                          : HudLayout::Scissor(game_x0, game_x1, hud_width, hud_height, hud_k,
                                               hud_anchor, hud_wide_aspect);
            s.offset.x = columns.x0;
            s.extent.width = u32(std::max(columns.x1 - columns.x0, 0));
        }
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

// gow3: HDR10 scene (TemporalUpscaler::HdrShadow). The tone pass is drawn again with its RenoDX
// replacement into the Tone shadow; the pass after it again with the Tone shadow in place of its
// input (binding 2) into the Scene shadow, through its float-target pipeline: the game's own
// math (distortion, overlays, the 2.2 power) on the HDR values. Same constants, buffers and
// render area; the guest's own draws are untouched.
void Rasterizer::RecordHdrShadow(const GraphicsPipeline& pipeline, const RenderState& state,
                                 bool is_indexed, u32 num_indices, u32 num_instances,
                                 u32 first_vertex, u32 first_instance,
                                 const vk::DescriptorBufferInfo& settings) {
    using Shadow = TemporalUpscaler::HdrShadow;
    const auto* ps = pipeline.GetStages()[u32(Shader::SwStage::Fragment)];
    if (!ps || state.num_color_attachments != 1 || float_color0) {
        return;
    }
    Pipeline::DescriptorWrites writes = set_writes;
    vk::DescriptorImageInfo tone_info{};
    vk::Pipeline handle{};
    Shadow which{};
    if (pipeline.HdrShadowHandle()) {
        handle = pipeline.HdrShadowHandle();
        which = Shadow::Tone;
        writes.push_back({.dstBinding = 12,
                          .descriptorCount = 1,
                          .descriptorType = vk::DescriptorType::eStorageBuffer,
                          .pBufferInfo = &settings});
    } else if (upscaler->HdrPostPass(ps->pgm_hash) && pipeline.FloatTargetHandle()) {
        tone_info = {.imageView = upscaler->ToneShadowView(),
                     .imageLayout = vk::ImageLayout::eGeneral};
        bool swapped = false;
        for (auto& write : writes) {
            if (write.dstBinding == 2 && write.descriptorCount == 1 &&
                write.descriptorType == vk::DescriptorType::eSampledImage) {
                write.pImageInfo = &tone_info;
                swapped = true;
            }
        }
        if (!swapped) {
            return;
        }
        handle = pipeline.FloatTargetHandle();
        which = Shadow::Scene;
    } else {
        return;
    }
    const auto target = upscaler->HdrShadowTarget(which, state.width, state.height);
    scheduler.EndRendering();
    const auto shadow_barrier = [&](vk::ImageLayout old_layout, vk::PipelineStageFlags2 src,
                                    vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst,
                                    vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src,
            .srcAccessMask = src_access,
            .dstStageMask = dst,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = target.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        scheduler.Record([b](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    };
    shadow_barrier(vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eAllCommands,
                   vk::AccessFlagBits2::eMemoryRead, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                   vk::AccessFlagBits2::eColorAttachmentWrite);
    RenderState shadow = state;
    shadow.color_attachments[0].image_view = target.view;
    shadow.color_attachments[0].image_layout = vk::ImageLayout::eGeneral;
    shadow.color_attachments[0].is_clear = 0;
    shadow.depth_stencil_attachment.depth_clear = false;
    shadow.depth_stencil_attachment.stencil_clear = false;
    scheduler.BeginRendering(shadow);
    pipeline.BindResources(writes, push_data);
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        if (is_indexed) {
            cmdbuf.drawIndexed(num_indices, num_instances, 0, s32(first_vertex), first_instance);
        } else {
            cmdbuf.draw(num_indices, num_instances, first_vertex, first_instance);
        }
    });
    scheduler.EndRendering();
    shadow_barrier(vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                   vk::AccessFlagBits2::eColorAttachmentWrite,
                   vk::PipelineStageFlagBits2::eAllCommands,
                   vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderStorageRead);
    upscaler->HdrShadowDrawn(which);
}

// gow3: a draw sent to the HDR10 chain's float HUD / display image whose pipeline has no
// RGBA16F variant (not one RGBA8 target, or a squaring blend): skipped instead of recorded with
// a mismatched attachment format. Named for the first few pixel shaders.
void Rasterizer::SkipFloatTargetDraw(const GraphicsPipeline& pipeline) const {
    static std::atomic<u32> skipped{0};
    if (skipped++ < 8) {
        const auto* ps = pipeline.GetStages()[u32(Shader::SwStage::Fragment)];
        std::printf("HDR: draw skipped, pixel shader %016llx has no float-target pipeline\n",
                    static_cast<unsigned long long>(ps ? ps->pgm_hash : 0));
        std::fflush(stdout);
    }
}

vk::Pipeline Rasterizer::SquarePass(const GraphicsPipeline& pipeline) const {
    const vk::Pipeline square = pipeline.SquarePassHandle();
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_SQUARE_PASS");
        return !env || env[0] != '0';
    }();
    if (!square || !enabled) [[likely]] {
        return {};
    }
    // MIN(s * s, d * d) = MIN(s, d)^2: the second draw needs the first one's coverage, which a
    // depth or stencil write would change.
    const auto& regs = Regs();
    const bool depth_write = regs.depth_control.depth_enable &&
                             regs.depth_control.depth_write_enable && regs.depth_buffer.DepthValid();
    if (depth_write || regs.depth_control.stencil_enable) {
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render_Vulkan, "Scaled MIN/MAX blend not squared: the draw writes depth "
                                       "or stencil");
        }
        return {};
    }
    return square;
}

// Opt-in A/B diagnostic, never a default rendering workaround. Change the file to
// 0 (guest compare), 1 (Equal -> LessOrEqual), or 2 (Equal -> Always).
// gow3 21 Oct 2026: hex shader hashes after the mode (low 32 bits, e.g. "2 8270ec0e f3d7d4eb")
// limit it to draws whose vertex or pixel shader is listed (to find one mesh's material draw).
// Also 3 (Equal -> GreaterOrEqual) and "4 <n>" (Equal -> LessOrEqual with the fragments pulled
// n depth bias units closer): how far a failing material draw is behind the stored depth.
static int g_depth_probe_bias = 0;
static int DepthCompareProbe(u32 vs_hash, u32 ps_hash) {
    static const char* path = std::getenv("GOW3_DEPTH_PROBE_FILE");
    if (!path || !path[0]) return 0;
    // 20 Oct 2026 audit: the fopen ran on the draw path every 250 ms; a thread started on first use
    // polls the file and publishes the mode and the shader list (upstream 0.5.0's format).
    struct Probe {
        int mode = 0;
        std::vector<u32> only;
    };
    static std::mutex mutex;
    static std::shared_ptr<const Probe> shared = std::make_shared<const Probe>();
    static std::atomic<int> bias{0};
    static const bool poller = [] {
        std::thread([] {
            for (;; std::this_thread::sleep_for(std::chrono::milliseconds(250))) {
                auto next = std::make_shared<Probe>();
                int next_bias = 0;
                if (FILE* f = std::fopen(path, "r")) {
                    if (std::fscanf(f, "%d", &next->mode) != 1 || next->mode < 0 || next->mode > 4) {
                        next->mode = 0;
                    }
                    if (next->mode == 4 && std::fscanf(f, "%d", &next_bias) != 1) next_bias = 0;
                    unsigned long long value;
                    while (std::fscanf(f, "%llx", &value) == 1) {
                        next->only.push_back(u32(value));
                    }
                    std::fclose(f);
                }
                if (bias.exchange(next_bias) != next_bias) {
                    std::printf("Depth compare probe: bias %d units\n", next_bias);
                }
                std::scoped_lock lock{mutex};
                if (shared->mode != next->mode || shared->only != next->only) {
                    std::printf("Depth compare probe: %d (%s)\n", next->mode,
                                next->only.empty()
                                    ? "all draws"
                                    : fmt::format("{} shaders", next->only.size()).c_str());
                }
                shared = std::move(next);
            }
        }).detach();
        return true;
    }();
    (void)poller;
    std::shared_ptr<const Probe> probe;
    {
        std::scoped_lock lock{mutex};
        probe = shared;
    }
    g_depth_probe_bias = bias.load(std::memory_order_relaxed);
    if (!probe->only.empty() && std::ranges::find(probe->only, vs_hash) == probe->only.end() &&
        std::ranges::find(probe->only, ps_hash) == probe->only.end()) {
        return 0;
    }
    return probe->mode;
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_stencil = GetEffectiveDepthStencilState(regs);
    const bool depth_test_enabled = depth_stencil.depth_test_enable;
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    dynamic_state.SetDepthWriteEnabled(depth_stencil.depth_write_enable &&
                                       !regs.depth_render_control.depth_clear_enable);
    int probe = 0;
    if (depth_test_enabled) {
        auto compare = LiverpoolToVK::CompareOp(regs.depth_control.depth_func);
        if (!depth_stencil.depth_write_enable && compare == vk::CompareOp::eEqual) {
            probe = DepthCompareProbe(dynamic_vs_hash, dynamic_ps_hash);
            if (probe == 1 || probe == 4) compare = vk::CompareOp::eLessOrEqual;
            if (probe == 2) compare = vk::CompareOp::eAlways;
            if (probe == 3) compare = vk::CompareOp::eGreaterOrEqual;
        }
        dynamic_state.SetDepthCompareOp(compare);
    }

    const bool depth_bounds_test_enabled = depth_stencil.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled =
        regs.polygon_control.NeedsBias() && !depth_bias_off.load(std::memory_order_relaxed);
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        {
            // gow3 diagnostics: each distinct polygon offset setup once (Uncharted D16 shadows).
            static std::mutex bias_mutex;
            static std::vector<std::array<u32, 6>> bias_seen;
            const std::array<u32, 6> key{std::bit_cast<u32>(regs.poly_offset.front_offset),
                                         std::bit_cast<u32>(regs.poly_offset.front_scale),
                                         std::bit_cast<u32>(regs.poly_offset.back_offset),
                                         std::bit_cast<u32>(regs.poly_offset.depth_bias),
                                         regs.poly_offset_db_fmt.raw,
                                         u32(regs.depth_buffer.z_info.format)};
            std::scoped_lock lock{bias_mutex};
            if (bias_seen.size() < 64 && std::ranges::find(bias_seen, key) == bias_seen.end()) {
                bias_seen.push_back(key);
                std::printf("GPU: depth bias front offset %g scale %g back offset %g clamp %g, "
                            "db fmt %#x, z format %u (front %u back %u)\n",
                            regs.poly_offset.front_offset, regs.poly_offset.front_scale,
                            regs.poly_offset.back_offset, regs.poly_offset.depth_bias,
                            regs.poly_offset_db_fmt.raw, u32(regs.depth_buffer.z_info.format),
                            u32(regs.polygon_control.enable_polygon_offset_front),
                            u32(regs.polygon_control.enable_polygon_offset_back));
                std::fflush(stdout);
            }
        }
        // gow3 (from the MediEvil port): the constant offset is in units of the depth format the
        // game names (PA_SU_POLY_OFFSET_DB_FMT_CNTL: 2^-16 for Z16). When the host depth buffer
        // has another precision the same number is off by a power of two (shadow acne/flicker).
        float constant_scale = 1.0f;
        if (const auto db_fmt = regs.poly_offset_db_fmt; !db_fmt.IsFloat() && db_fmt.NumDbBits() > 0) {
            static thread_local u32 cached_key = ~0u;
            static thread_local s32 cached_host_bits = 0;
            const u32 key = (u32(regs.depth_buffer.z_info.format) << 8) |
                            u32(regs.depth_buffer.stencil_info.format);
            if (key != cached_key) {
                cached_key = key;
                switch (instance.GetSupportedFormat(
                    LiverpoolToVK::DepthFormat(regs.depth_buffer.z_info.format,
                                               regs.depth_buffer.stencil_info.format),
                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment)) {
                case vk::Format::eD16Unorm:
                case vk::Format::eD16UnormS8Uint:
                    cached_host_bits = 16;
                    break;
                case vk::Format::eX8D24UnormPack32:
                case vk::Format::eD24UnormS8Uint:
                case vk::Format::eD32Sfloat: // float: 2^(e-23), 2^-24 for depths in [0.5, 1)
                case vk::Format::eD32SfloatS8Uint:
                    cached_host_bits = 24;
                    break;
                default:
                    cached_host_bits = 0;
                    break;
                }
            }
            if (cached_host_bits != 0 && cached_host_bits != db_fmt.NumDbBits()) {
                constant_scale = std::ldexp(1.0f, cached_host_bits - db_fmt.NumDbBits());
                static std::atomic<bool> reported{false};
                if (!reported.exchange(true)) {
                    std::printf("GPU: depth bias in %d-bit units scaled x%g for the host depth format\n",
                                db_fmt.NumDbBits(), constant_scale);
                    std::fflush(stdout);
                }
            }
        }
        dynamic_state.SetDepthBias(
            (front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset) * constant_scale,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }
    if (probe == 4) { // diagnostics only (DepthCompareProbe)
        dynamic_state.SetDepthBiasEnabled(true);
        dynamic_state.SetDepthBias(-float(g_depth_probe_bias), 0.0f, 0.0f);
    }

    const bool stencil_test_enabled = depth_stencil.stencil_test_enable;
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#if defined(__linux__) || defined(_WIN32)
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

} // namespace Vulkan

namespace Vulkan {

void Rasterizer::MarkPass(const GraphicsPipeline* pipeline, const RenderState& state) {
    auto* profiler = GpuProfiler::Get();
    if (!profiler || !scheduler.WillBeginRendering(state)) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    std::array<u64, AmdGpu::NUM_COLOR_BUFFERS + 6> parts{};
    parts[0] = state.width | u64(state.height) << 32;
    parts[1] = vs.pgm_hash;
    parts[2] = ps ? ps->pgm_hash : 0;
    parts[3] = db_desc.first ? u64(texture_cache.GetImage(db_desc.first).info.pixel_format) : 0;
    for (u32 cb = 0; cb < state.num_color_attachments && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        parts[4 + cb] = cb_descs[cb].first
                            ? u64(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format) + 1
                            : 0;
    }
    const u64 key = XXH3_64bits(parts.data(), sizeof(parts));
    profiler->Mark(key, [&] {
        std::string targets;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            targets += cb_descs[cb].first
                           ? vk::to_string(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format)
                           : std::string{"-"};
            targets += ' ';
        }
        return fmt::format("pass {}x{} [{}] depth {} vs {:016x} ps {:016x}", state.width,
                           state.height, targets,
                           db_desc.first ? vk::to_string(
                                               texture_cache.GetImage(db_desc.first).info.pixel_format)
                                         : std::string{"-"},
                           vs.pgm_hash, ps ? ps->pgm_hash : 0);
    });
}

void Rasterizer::NoteFrameStart() {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->BeginFrame();
    }
    const u64 frame = Gow3Stats::gpu_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    NoteTargetDumpFrame();
    // GOW3_BUFFER_STATS=1: how many earlier frames the GPU has not finished when the GPU thread
    // starts a frame — the lag that decides whether guest memory can be read in place.
    static const bool stats = [] {
        const char* value = std::getenv("GOW3_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    if (!stats) {
        return;
    }
    static std::array<u64, 16> frame_ticks{};
    static std::array<u64, 17> lag_histogram{};
    u32 lag = 0;
    for (u32 back = 1; back < frame_ticks.size() && back < frame; ++back) {
        const u64 tick = frame_ticks[(frame - back) % frame_ticks.size()];
        if (tick && !scheduler.IsFree(tick)) {
            lag = back;
        }
    }
    ++lag_histogram[lag];
    frame_ticks[frame % frame_ticks.size()] = scheduler.CurrentTick();
    if (frame % 600 == 0) {
        std::printf("GPU lag at frame start (frames not finished: count):");
        for (u32 i = 0; i < lag_histogram.size(); ++i) {
            if (lag_histogram[i]) {
                std::printf(" %u:%llu", i, static_cast<unsigned long long>(lag_histogram[i]));
            }
        }
        std::printf("\n");
        lag_histogram = {};
    }
}

} // namespace Vulkan
