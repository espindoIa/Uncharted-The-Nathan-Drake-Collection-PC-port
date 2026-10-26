// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <vector>
#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "common/unique_function.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_bind_helper.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {

/// gow3 diagnostics (GOW3_FAULT_STATS): which GPU command thread write into guest memory takes
/// a write fault (1 WriteData, 2 graphics WriteData inline, 3 compute WriteData, 4 DMA, 5 fence
/// fallback). Set around those writes.
inline thread_local int g_gpu_write_site = 0;
struct GpuWriteSite {
    int previous;
    explicit GpuWriteSite(int site) : previous{g_gpu_write_site} {
        g_gpu_write_site = site;
    }
    ~GpuWriteSite() {
        g_gpu_write_site = previous;
    }
};

class GraphicsPipeline;
class Runtime;

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] TemporalUpscaler& GetUpscaler() noexcept {
        return *upscaler;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    void Draw(bool is_indexed, u32 index_offset = 0, const PreparedDraw* prepared = nullptr);

    /// gow3: draw preparation workers (vk_draw_prep.h), fed and consumed by Liverpool.
    DrawPreparation& GetDrawPreparation() {
        return *draw_prep;
    }
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (host_markers_enabled) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    u32 ReadDataFromGds(u32 gsd_offset);
    bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// GPU thread, before a write the guest can observe (see Scheduler::WaitHostCopies).
    void WaitHostCopies() {
        DrainDrawPipe();
        scheduler.WaitHostCopies();
    }

    /// gow3: GPU command thread: waits until the draw recording thread has recorded every
    /// draw handed to it (vk_draw_pipe.h); no-op on other threads.
    /// `line`/`function`: the caller, for the statistics of where stage A waits.
    void DrainDrawPipe(u32 reason = DrawPipe::ReasonRasterizer, u32 line = __builtin_LINE(),
                       const char* function = __builtin_FUNCTION());
    /// Runs `task(rasterizer, copy of data)` in order with the draws: on the draw recording
    /// thread while the draw pipeline is in use (PipelinedTasks), else here after a drain.
    using OrderedTask = void (*)(Rasterizer& rasterizer, const u8* data);
    /// Returns true when the task was handed to the recording thread (not run yet).
    /// `writes_noted`: the caller passes the memory the task writes to NoteRingWrite (or
    /// NotePendingGpuWrite), so constant ring copies elsewhere stay reusable.
    /// `after_run` (gow3 24 Oct 2026, a fence write no later dispatch reads): with a dispatch
    /// run open the task joins it, after the dispatch before it (Gow3Toggle::DispatchRuns).
    bool RunInOrder(OrderedTask task, const void* data, u32 size,
                    u64 toggle = Gow3Toggle::PipelinedTasks, bool writes_noted = false,
                    bool after_run = false);
    bool CommandsInOrder() const;
    void RunCommandInOrder(Common::UniqueFunction<void>&& command);
    /// Stage A: GPU work queued for the recording thread writes [address, address + size):
    /// constant ring copies of those bytes are not reused (NotePendingGpuWrite does this too).
    void NoteRingWrite(VAddr address, u64 size);
    /// Stage A: a queued label write, separate from constant ring copy decisions.
    void NotePendingLabel(VAddr address, u64 size);
    /// Stage A: a guest submission (graphics or a compute ring's new packets) starts, or a
    /// memory wait ends: the guest may have rewritten memory copied before.
    void NewGuestEpoch();
    /// Stage A: the draw pipe position after the last handed-over packet, and whether the
    /// recording thread has run everything before a position.
    [[nodiscard]] u64 DrawPipeHead() const {
        return draw_pipe ? draw_pipe->Head() : 0;
    }
    [[nodiscard]] bool DrawPipeReached(u64 position) const {
        return !draw_pipe || draw_pipe->Reached(position);
    }
    /// Stage A: no draws waiting for the recording thread (an open dispatch run counts).
    [[nodiscard]] bool DrawPipeIdle() const {
        return (!draw_pipe || draw_pipe->Idle()) && !dispatch_run.count;
    }
    /// gow3 24 Oct 2026 (Gow3Toggle::DispatchRuns): why stage A handed its open run of crowd
    /// dispatches to stage B (statistics). The run goes first, then whatever cut it.
    enum RunCut : u32 {
        CutPacket,     ///< another packet (draw, other dispatch)
        CutTask,       ///< an in-order task
        CutDrain,      ///< a draw pipe drain
        CutIndirect,   ///< an indirect dispatch
        CutOpcode,     ///< a PM4 packet other than a no-op, user data or a direct dispatch
        CutQueueEnd,   ///< a command buffer ended
        CutYield,      ///< the command thread switches queues
        CutCommand,    ///< a queued command runs
        CutSubmission, ///< end of a submission
        CutFull,       ///< 64 instances
        CutJoin,       ///< another pipeline, queue or program registers
        CutRefused,    ///< an instance that cannot be batched
        NumRunCuts,
    };
    /// gow3 24 Oct 2026 (Gow3Toggle::RunReleaseBatch): set by Liverpool. `task` is the compute
    /// release task (`size` bytes of data); `stage` does a release's stage B part per label and
    /// mixes it into `hash` (false: run the task itself); `signal` queues one host-copy signal
    /// for the staged releases' bytes (taken out of `batch`), `hash` for verify mode.
    struct ReleaseBatchHooks {
        OrderedTask task;
        bool (*stage)(Rasterizer&, const u8* data, u64& hash);
        void (*signal)(Rasterizer&, std::vector<u8>& batch, u64 hash);
        u32 size;
    };
    static inline ReleaseBatchHooks release_hooks{nullptr, nullptr, nullptr, 0};
    /// GOW3_RUN_RELEASE_VERIFY=1: each batch's signal checks its writes and IRQs (any thread).
    static bool RunReleaseVerify();
    static void NoteReleaseVerify(u64 fences, bool mismatch);
    /// Stage A: the open run (if any) is handed to stage B first. `detail`: for CutOpcode the
    /// PM4 opcode (and SET_SH_REG's register << 8), for the statistics.
    void CutDispatchRun(u32 reason, u32 detail = 0) {
        if (dispatch_run.count) {
            FlushDispatchRun(reason, detail);
        }
    }
    /// Stage A, end of a submission: its prepared draws stay alive until the recording thread
    /// has recorded them (instead of a drain).
    void RetireSubmission();
    /// Stage A: guest memory the recording thread will write for work handed to it (storage
    /// buffers, DMA, WriteData, fences); constants overlapping it are bound there, not copied here.
    void NotePendingGpuWrite(VAddr address, u64 size);
    /// gow3: at an end-of-pipe event, the output of dispatches whose results the guest reads
    /// once it sees that event (eop_readbacks) is read back asynchronously with its pages
    /// read-protected: a guest read before the GPU is done waits for that readback only.
    /// (Delaying the event instead broke Uncharted 1: it is the frame's end-of-pipe tick, which
    /// FrameBegin asserts two frames later.)
    bool ReadBackEopResults();
    /// gow3: a fence label the runtime writes to guest memory (BufferCache::NoteCpuWrite).
    void NoteLabelWrite(VAddr address, u64 size) {
        buffer_cache.NoteCpuWrite(address, size);
    }
    /// Before a guest-visible write: fences deferred earlier are written first.
    void WaitDeferredSignals() {
        scheduler.WaitDeferredSignals();
    }
    /// Runs `signal` after the guest memory copies issued so far, without waiting here.
    /// gow3: draws go through the two-stage draw pipeline (multithreaded rendering).
    bool DrawPipeActive() const {
        return UseDrawPipe();
    }
    void SignalAfterHostCopies(std::function<void()> signal) {
        scheduler.SignalAfterHostCopies(std::move(signal));
    }
    /// The GPU command thread or the draw recording thread (fault handling runs inline there).
    bool IsGpuSideThread() const;
    bool IsGpuSideThreadId(u32 tid) const;
    /// A guest write hit a protected page.
    bool OnWriteFault(VAddr addr, bool assume_locks);
    /// gow3: the runtime is about to write [addr, addr + size) itself (file reads): everything an
    /// OnWriteFault does per page, once for the range. True when the range was prepared (also when
    /// nothing in it is tracked), false when the caller must fall back to touching pages.
    bool PrepareCpuWrite(VAddr addr, u64 size);
    /// Whole file-read page, on the GPU command thread after the draw pipe drain.
    bool DiscardCpuWritePage(VAddr page);
    /// The per-address work of a write fault (after the drain), shared by both.
    bool HandleWriteAt(VAddr addr, bool assume_locks);
    /// gow3: a guest instruction keeps faulting on one page (see BufferCache::ReleaseStuckPage).
    void ReleaseStuckPage(VAddr page, bool is_write, bool assume_locks);
    bool ReadMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// gow3 23 Oct 2026: a guest thread's read fault at `addr`, before ReadMemory: 1 when it was
    /// a false-sharing read made possible here (handled); 2 when it read GPU bytes and waited
    /// for their submission here (ReadMemory then lands them without a GPU wait); 0 otherwise.
    int GuestReadFault(VAddr addr);
    /// gow3 23 Oct 2026 (guest thread, guest memcpy): true when the GPU made the copy because its
    /// source holds GPU bytes still on their way back (toggle ForwardedCopies).
    bool ForwardGuestCopy(VAddr dst, VAddr src, u64 size);
    /// gow3: after a fault: lands a buffer readback that still read-protects the page of `addr`.
    void LandWatchedReadback(VAddr addr, bool assume_locks);
    void WaitDrawPipeForCapture();
    void ProcessDownloadImages();
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void RegisterMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    u64 Flush();
    void Finish();
    void OnSubmit();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

    std::thread::id GetGpuCommandProcessorThread();
#if defined(__linux__) || defined(_WIN32)
    u32 GetGpuCommandProcessorThreadId();
#endif

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    RenderState BeginRenderingFull(const GraphicsPipeline* pipeline);
    /// gow3: a draw continuing the open render pass with the same inputs gets the same render
    /// state (RenderStateMemo); nothing can have broken the pass in between (barriers, copies and
    /// dispatches end it).
    struct BeginSignature {
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> ids{};
        std::array<VideoCore::ImageViewInfo, AmdGpu::NUM_COLOR_BUFFERS + 1> views{};
        u32 mrt_mask = 0;
        u32 num_samples = 0;
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
        bool motion = false;
        bool scene_started = false;
        bool raster_scaling = false;
        u64 upscaler_state = 0;
        u64 generation = 0;
        u32 depth_control = 0;
        bool depth_valid = false;
        bool stencil_valid = false;
        bool operator==(const BeginSignature&) const = default;
    };
    BeginSignature MakeBeginSignature(const GraphicsPipeline* pipeline) const;
    struct BeginMemo {
        bool valid = false;
        BeginSignature signature;
        RenderState state;
        u32 scene_size = 0;
        std::array<float, 2> target_scale{};
    } begin_memo;
    u64 begin_memo_hits = 0, begin_memo_misses = 0;
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    /// Whether draws into `depth` get the upscaler's sub-pixel jitter (scene depth or its copy).
    bool JitteredDepth(VideoCore::ImageId depth);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    void UpdateViewportScissorState() const;
    void UpdateDepthStencilState() const;
    /// gow3: the squaring pipeline of a scaled MIN/MAX blend when this draw can use it.
    vk::Pipeline SquarePass(const GraphicsPipeline& pipeline) const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    bool FilterDraw();
    bool FilterDrawPasses() const;
    /// Everything of a direct draw after the pipeline selection (GPU thread or stage B).
    void DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                    bool is_indexed, u32 index_offset);
    static bool DrawPipeWanted();
    bool UseDrawPipe() const;
    bool OnStageA() const;
    /// Arguments of an indirect draw: `args` holds `max_count` commands `stride` apart.
    struct IndirectDraw {
        VAddr args;
        VAddr count; ///< the draw count, or 0 for max_count draws
        u32 stride;
        u32 max_count;
    };
    /// Everything of an indirect draw after the pipeline selection (GPU thread or stage B).
    void DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                            const IndirectDraw& indirect);
    /// Everything of an indirect dispatch after the pipeline selection.
    void DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr args, u32 size);
    /// Hands a draw (or, with `cs`, a dispatch) to the recording thread. `cs_sharps`: the
    /// dispatch's buffer V#s by resource index (PipelineCache::ComputeSharps), or null.
    void PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared, bool is_indexed,
                  u32 index_offset, const AmdGpu::ComputeProgram* cs = nullptr,
                  const IndirectDraw* indirect = nullptr,
                  const AmdGpu::Buffer* cs_sharps = nullptr);
    /// Everything of a direct dispatch after the pipeline selection (GPU thread or stage B).
    void DispatchRecord(const ComputePipeline* pipeline);
    /// gow3: a dispatch writing a display buffer as a storage image (Uncharted 3's movie
    /// frames) while frames are upscaled at the composite pass: TemporalUpscaler::
    /// OnDisplayStorageWrite (#237/#243/#222, no subtitles over movies).
    void NoteDisplayStorageWrite(const Shader::Info& cs);
    /// The compute registers of the dispatch being recorded.
    const AmdGpu::ComputeProgram& CsRegs() const;
    static void RunDrawPacket(void* rasterizer, const u8* packet, u32 size);
    struct RingBinding;
    void CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                             boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out,
                             const AmdGpu::Buffer* sharps = nullptr);
    void PrintPipeStats();
    /// The registers of the draw being recorded: stage B's copy there, else Liverpool's.
    const AmdGpu::Regs& Regs() const;
    AmdGpu::CbDbExtent CbExtent(u32 index) const;
    AmdGpu::CbDbExtent DbExtent() const;

    void BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                     Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data, u32& write_index);
    /// `on_helper`: run by the texture binding helper; out-of-date images are not refreshed.
    void BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                      Shader::Backend::Bindings& binding, u32& write_index, bool& barrier,
                      bool on_helper = false);
    bool BindResources(const Pipeline* pipeline);
    void BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                      Shader::Backend::Bindings& binding, u32& write_index);
    void ForceAnisotropy(AmdGpu::Sampler& sharp, const Shader::SamplerResource& sampler,
                         const Shader::Info& stage, const PreparedStage* prepared) const;
    /// gow3 26 Oct 2026: full LOD range for samplers of unmipped images (HD UI texture mips).
    void WidenSamplerForHdMips(AmdGpu::Sampler& sharp, const Shader::SamplerResource& sampler,
                               const Shader::Info& stage, const PreparedStage* prepared) const;
    /// gow3: a stage's resolved textures remembered by its prepared T# hashes
    /// (TextureSetMemo): a hit only redoes the per-draw effects (binding flags, transitions).
    struct TextureSetEntry {
        VideoCore::ImageId id{}; ///< after the depth redirect; null descriptor when invalid
        vk::ImageView view;
        const void* backing = nullptr;
        VideoCore::SubresourceRange range;
        bool proxy = false; ///< sampled from the scene proxy (view: SampleProxy on each use)
    };
    struct TextureSet {
        u64 key = 0;
        const Shader::Info* stage = nullptr;
        u64 generation = ~0ull;
        u64 scene_generation = 0; ///< SceneTargets::Generation of the proxy views
        u32 count = 0;
        static constexpr u32 MaxImages = 16; ///< larger sets are not memoized
        std::array<u64, MaxImages> hashes{};
        std::array<TextureSetEntry, MaxImages> entries{};
    };
    std::array<TextureSet, 32768> texture_sets{};
    /// gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1): BindTextures' resolved set, reused (only the entries
    /// of the stage's images are reset; the GPU command thread alone memoizes sets).
    std::array<TextureSetEntry, TextureSet::MaxImages> texture_set_scratch{};
    /// gow3 21 Oct 2026 diagnostics (GOW3_FAULT_TIMES=1): see OnWriteFault.
    static void NoteWriteFaultTime(VAddr addr, bool gpu_side, const std::array<u64, 5>& steps);
    u64 texture_set_hits = 0, texture_set_misses = 0;
    std::array<u64, 4> texture_set_why{}; ///< misses: other key, generation, image check, new
    /// Returns true when the stage's images were bound from the memo.
    bool BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                             u32 first_image_idx, bool& barrier, TextureSet*& slot);
    /// The prepared sharps of `stage` in the current prepared draw, if they fit it.
    const PreparedStage* FindPreparedStage(const Shader::Info& stage) const;
    /// Whether the helper may bind textures for this pipeline at all (fixed descriptor layout,
    /// no storage writes that invalidate cached images).
    bool HelperEligible(const Pipeline* pipeline) const;
    /// Whether every texture of `stage` takes the memoized path without refreshing, creating or
    /// redirecting anything that records commands or touches the buffer cache.
    bool TexturesBindableOnHelper(const Shader::Info& stage, const PreparedStage* prepared);
    static void RunTextureTask(void* rasterizer);
    static void JoinBindHelper(void* rasterizer);

    /// Display pass: counts the frame (Gow3Stats::gpu_frames) and, with GOW3_BUFFER_STATS, the lag.
    void NoteFrameStart();
    /// GOW3_GPU_PROFILE: a timestamp where a render pass starts (vk_gpu_profiler.h).
    void MarkPass(const GraphicsPipeline* pipeline, const RenderState& state);
    void BindVertexBuffers(const GraphicsPipeline* pipeline,
                           const PreparedDraw* prepared = nullptr);
    void BindIndexBuffer(u32 index_offset = 0);
    /// gow3: BindVertexBuffers/BindIndexBuffer in two halves: obtaining the buffers
    /// (vertex_binds, index_bind) and recording the binds.
    void ResolveVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared);
    void EmitVertexBuffers();
    void ResolveIndexBuffer(u32 index_offset);
    void EmitIndexBuffer();

    void ResetBindings(bool is_compute);

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    /// gow3: changes before and after every map/unmap (IsMapped's per-thread cache of mapped
    /// intervals is valid while it stays the same).
    std::atomic<u64> mapped_generation{0};
    PipelineCache pipeline_cache;
    std::unique_ptr<DrawPreparation> draw_prep;
    std::unique_ptr<CameraMotion> camera_motion; // gow3: motion vectors (docs/upscaler.md)
    std::unique_ptr<SceneTargets> scene_targets;
    bool scene_started = false;
    VAddr frame_display_address = 0; ///< display buffer of the last frame start
    std::unique_ptr<ObjectMotion> object_motion;
    bool motion_draw = false;
    u64 motion_geometry{};    ///< vertex-stream identity of the current direct draw
    bool gbuffer_draw = false;
    /// Uncharted 2: the depth of a draw whose vertex user data may hold the camera (flat_camera.h).
    VideoCore::ImageId flat_camera_depth{};
    bool flat_scene_draw = false; ///< Uncharted: a draw with the jittered scene depth
    /// Buffers written by dispatches in the draw pipe (not yet marked GPU-written; SyncSrtTables).
    std::vector<std::pair<VAddr, u64>> pipe_write_ranges;
    bool pipe_writes_overflow = false;
    /// Uncharted 2: a frame was started at the display pass and no scene draw followed yet.
    bool display_frame_open = false;
    std::unique_ptr<TemporalUpscaler> upscaler; // gow3: FSR (docs/upscaler.md)
    std::array<float, 2> draw_jitter{};         ///< viewport offset of the current draw, pixels
    std::array<float, 2> target_scale{1.0f, 1.0f}; ///< pass drawn into the upscaler's output-size images
    const bool host_markers_enabled;
    const bool guest_markers_enabled;

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    struct BoundBuffer {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u32 size;
        bool is_written;
    };
    boost::container::static_vector<BoundBuffer, Shader::NUM_BUFFERS> bound_buffers;
    /// gow3: arenas the current draw's DMA reads (SyncDmaMemory windows), for barriers.
    boost::container::small_vector<const VideoCore::Buffer*, 4> dma_read_arenas;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Shader::PushData push_data;

    // gow3: bindings point at their description instead of copying it (a hot spot): into
    // image_desc_cache for memoized lookups (pinned for the current BindTextures call), else
    // into image_desc_storage.
    using ImageBindingInfo = std::pair<VideoCore::ImageId, const VideoCore::TextureCache::ImageDesc*>;
    // gow3: texture descriptions depend only on the T# and three resource flags; building
    // them (mip layout sizes) for every binding of every draw was a hot spot.
    struct ImageDescCacheEntry {
        std::array<u64, 4> sharp{};
        u32 flags = ~0u;
        VideoCore::TextureCache::ImageDesc desc;
        // Memoized FindImage for bindings without mip overrides (TextureBindingMemo).
        u64 found_generation = ~0ULL;
        VideoCore::ImageId found_id{};
        VideoCore::TextureCache::ImageDesc found_desc;
        u64 pinned = 0; ///< bind_epoch of the BindTextures call referencing found_desc
        u64 last_use = 0;
        // Memoized FindView for found_id (after the depth redirect): valid while the entry's
        // FindImage memo holds and the image keeps this backing (TextureViewMemo).
        VideoCore::TextureCache::ViewMemo view_memo;
    };
    std::array<ImageDescCacheEntry, 4096> image_desc_cache{};
    u64 desc_use_counter = 0;
    /// Entries replacing pinned cache slots during one BindTextures call.
    boost::container::static_vector<ImageDescCacheEntry, Shader::NUM_IMAGES> image_desc_overflow;
    boost::container::static_vector<VideoCore::TextureCache::ImageDesc, Shader::NUM_IMAGES * 2>
        image_desc_storage;
    u64 bind_epoch = 0;
    // gow3: render/depth target lookups memoized by their raw register bytes while image
    // registrations are unchanged (the descriptions depend only on those registers).
    struct TargetMemo {
        std::array<u8, 256> key{};
        u32 key_size = 0;
        u64 generation = ~0ULL;
        VideoCore::ImageId image_id{};
        VideoCore::TextureCache::ImageDesc desc;
    };
    std::array<TargetMemo, 64> target_memo{};
    // gow3: consecutive draws mostly keep their targets; the slot's description (cb_descs,
    // db_desc) is then still the right one and is neither looked up nor copied.
    struct LastTarget {
        std::array<u8, 256> key{};
        u32 key_size = 0;
        u64 generation = ~0ULL;
        VideoCore::ImageId image_id{};
    };
    std::array<LastTarget, AmdGpu::NUM_COLOR_BUFFERS + 1> last_targets{}; ///< CBs, then DB
    template <typename... Parts>
    VideoCore::ImageId FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                          LastTarget& last, auto&& make_desc,
                                          const Parts&... parts);
    /// The prepared draw whose pipeline the current Draw uses (its sharps are valid), or null.
    const PreparedDraw* bind_prepared = nullptr;
    /// `hash` is ImageDescHash(sharp, res), computed by a draw-preparation worker or here.
    ImageDescCacheEntry& CachedImageDescEntry(const AmdGpu::Image& sharp,
                                              const Shader::ImageResource& res, u64 hash);
    ImageDescCacheEntry& CachedImageDescEntry(const AmdGpu::Image& sharp,
                                              const Shader::ImageResource& res) {
        return CachedImageDescEntry(sharp, res, ImageDescHash(sharp, res));
    }
    const VideoCore::TextureCache::ImageDesc& CachedImageDesc(const AmdGpu::Image& sharp,
                                                              const Shader::ImageResource& res) {
        return CachedImageDescEntry(sharp, res).desc;
    }
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    struct VertexBinds {
        VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
        VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
        VertexInputs<vk::Buffer> host_buffers;
        VertexInputs<vk::DeviceSize> host_offsets;
        VertexInputs<vk::DeviceSize> host_sizes;
        VertexInputs<vk::DeviceSize> host_strides;
        u32 num_buffers = 0;
    } vertex_binds;
    struct IndexBind {
        vk::Buffer handle;
        u64 offset;
        vk::IndexType type;
    } index_bind{};
    /// The direct draw whose vertex/index buffers BindResources may resolve early.
    struct DrawInputs {
        const GraphicsPipeline* pipeline;
        const PreparedDraw* prepared;
        u32 index_offset;
        bool is_indexed;
        bool pending;
        bool resolved;
    } draw_inputs{};
    // gow3: textures of a draw bound on the helper thread while this thread binds buffers.
    static bool BindHelperWanted();
    BindHelper bind_helper{BindHelperWanted()};
    struct TextureTask {
        struct Stage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<Stage, Shader::MaxStageTypes> stages;
        u32 count = 0;
        u32 completed = 0; ///< stages the helper bound; the rest are bound after the join
        bool barrier = false;
    } texture_task;
    /// Draws whose textures the helper bound completely, partly or not at all (GOW3_FRAME_STATS).
    u64 helper_full = 0, helper_partial = 0, helper_serial = 0;
    /// The memoized cache entry of each image binding (FindImage memo taken), else null.
    boost::container::static_vector<ImageDescCacheEntry*, Shader::NUM_IMAGES> image_binding_entries;
    bool fault_process_pending{};
    // gow3: small shader storage writes since the last submission (GOW3_SYNC_GPU_CONSTANTS).
    std::vector<std::pair<VAddr, u64>> small_gpu_writes;
    /// Ranges downloaded after every submission (GOW3_READBACK_TRIGGER "range <lo> <hi>").
    std::vector<std::pair<VAddr, u64>> synced_ranges;
    /// gow3: storage written by shaders whose results the guest reads right after the next
    /// end-of-pipe event (ReadBackEopResults). Uncharted 1's mesh ray casts (bullet impacts,
    /// blood and other decals): the event's interrupt marks the batch done and the game reads
    /// the hits at once; fences are written when recorded, so it read stale results (no hits).
    std::vector<std::pair<VAddr, u64>> eop_readbacks;
    /// gow3 18 Oct: ray-cast results (all EOP readback shaders but the 6a6422a9 copies) are read
    /// back without protecting their pages: the guest reads them after the fence, which is
    /// written once they land (GOW3_DEFER_RAYCAST_FENCES). Protected, every guest write into
    /// those pages (the next batch) faulted and waited for the GPU thread and the readback:
    /// Uncharted 3's ray-cast thread stalled with the ray-cast lock held (rooftops: 2-3 fast
    /// frames, then one of 25-35 ms). Opt-in, GOW3_OPEN_RAYCAST_READBACKS=1: measured on the
    /// rooftops it removed the faults on ray-cast pages but not the uneven frames (18 Oct).
    std::vector<std::pair<VAddr, u64>> eop_open_readbacks;
    std::vector<std::pair<VAddr, u64>> eop_soft_readbacks; ///< gow3: tiny images, unprotected
    struct ClothPeek {
        VAddr address;
        u64 size;
        u64 shader;
        u32 slot;
        bool written;
        const VideoCore::Buffer* buffer; ///< the buffer bound (arena or stream buffer)
        u64 buffer_offset;
    };
    std::vector<ClothPeek> cloth_peeks; ///< gow3 diagnostics (GOW3_CLOTH_PEEK)
    bool sync_gpu_constants = [] {
        const char* env = std::getenv("GOW3_SYNC_GPU_CONSTANTS");
        return env && env[0] == '1';
    }();
    void SyncSmallGpuWrites();
    /// gow3: before the SRT walker of `info` runs on the GPU command thread: the tables it reads
    /// that hold GPU-written bytes not yet in guest memory are downloaded (one GPU wait).
    void SyncSrtTables(const Shader::Info& info);
    struct SrtNode {
        s32 parent;      ///< -1: a pointer in the user data
        u32 offset;      ///< byte offset of the pointer in the user data or the parent table
        u32 extent;      ///< bytes the walker reads from this table
        bool dynamic;    ///< the pointer's offset is computed (not followed)
    };
    struct SrtCopy {
        u32 flat_dw;     ///< destination dword in the flattened user data
        s32 node;        ///< source table (-1: the user data)
        u32 src_off;     ///< byte offset in that table
    };
    /// Tables a walker reads and what it copies from them, decoded from its code (parents
    /// first), per walker.
    struct SrtPlan {
        std::vector<SrtNode> nodes;
        std::vector<SrtCopy> copies;
    };
    std::unordered_map<const void*, SrtPlan> srt_plans;
    /// gow3: recent walkers and their plans (SrtPlanFor).
    static constexpr u32 SrtPlanMemoSize = 32;
    std::array<std::pair<const void*, const SrtPlan*>, SrtPlanMemoSize> srt_plan_memo{};
    const SrtPlan& SrtPlanFor(const Shader::Info& info);
    /// Guest address of each plan table for this user data (0: unknown), without downloads.
    void ResolveSrtTables(const SrtPlan& plan, const Shader::Info& info, VAddr* address);
public:
    /// gow3 diagnostics (GOW3_SRT_CHECK): the guest address the walker copies flattened dword
    /// `flat_dw` from for this user data (0: unknown) and what the caches know about it.
    VAddr SrtSourceAddress(Shader::PFN_SrtWalker walker, size_t walker_size,
                           std::span<const u32> user_data, u32 flat_dw, const char** kind);
    /// gow3 diagnostics (GOW3_SRT_CHECK, 21 Oct 2026): the draw whose resource tables are being
    /// walked (colour target 0, depth target, index buffer, index count), so a table the game
    /// completed late can be matched to a pass and a mesh of a frame capture.
    struct SrtDrawContext {
        VAddr color0;
        VAddr depth;
        VAddr index;
        u32 num_indices;
    };
    SrtDrawContext CurrentDrawContext() const;

private:
    /// Diagnostics for a garbage V#: where the walker took flattened dword `flat_dw` from.
    void DescribeSharpSource(const Shader::Info& info, u32 flat_dw, const char* problem);
    /// gow3 diagnostics: a resource-table walk faulted: the tables it read, now and 3 ms later.
    void ReportSrtFault(const Shader::Info& info);
    u64 srt_syncs = 0, srt_sync_reports = 0;
    void SyncDmaMemory(const Shader::Info* stage);
    /// VS of the draw whose dynamic state is being set (diagnostic overrides).
    mutable u32 dynamic_vs_hash = 0;
    /// gow3 21 Oct 2026: its pixel shader (GOW3_DEPTH_PROBE_FILE shader filter).
    mutable u32 dynamic_ps_hash = 0;
    static inline std::atomic<bool> shadow_clear_far{false};
    static inline std::atomic<bool> depth16_clear_one{false};
    static inline std::atomic<bool> depth_bias_off{false};
    bool attachment_feedback_loop{};
    bool needs_barrier{};
    /// gow3 (GOW3_SKIP_COMPUTE_REBIND): the compute pipeline the last DispatchRecord bound and
    /// the scheduler's RecordSerial() right after its dispatch.
    vk::Pipeline bound_compute_pipeline{};
    u64 bound_compute_serial = ~u64{0};
    bool AppendSkinningBatch(const ComputePipeline* pipeline);
    void FlushSkinningBatch();
    struct SkinningBatch {
        const ComputePipeline* pipeline = nullptr;
        u32 count = 0, dim_x = 0, bindings = 0;
        Shader::PushData first_push;
        std::array<std::array<vk::DescriptorBufferInfo, ComputePipeline::BatchCapacity>,
                   Shader::NUM_BUFFERS + 1> buffers;
    } skinning_batch;
    u64 fused_dispatches = 0, fused_batches = 0, fused_singles = 0;
    // gow3: two-stage draw pipeline (stage B state; the thread is the last member so it
    // stops first).
    static inline thread_local const AmdGpu::Regs* stage_regs = nullptr;
    static inline thread_local const AmdGpu::ComputeProgram* stage_cs = nullptr;
    AmdGpu::Regs pipe_regs{};
    bool pipe_synced = false;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> pipe_cb_extent{};
    AmdGpu::CbDbExtent pipe_db_extent{};
    /// Stage A: small read-only guest buffers copied into the constant ring for a packet.
    struct RingBinding {
        u32 index;  ///< buffer resource of the stage
        u32 size;
        u64 offset; ///< in the ring
        VAddr address;
    };
    struct PendingWrite {
        VAddr begin;
        VAddr end;
        u64 position;
    };
    std::vector<PendingWrite> pending_writes;
    std::deque<PendingWrite> pending_labels; ///< stage A: labels after their task's Commit
    /// 22 Oct 2026: bounds of every label pushed since pending_labels was last empty (a superset).
    VAddr label_min = ~VAddr{0}, label_max = 0;
    u64 pending_task_position = 0; ///< latest un-noted task; earlier tasks run before it
    bool pending_complete = true; ///< all queued packets had their buffer writes noted
    VAddr pending_min = ~VAddr{0}, pending_max = 0; ///< bounds of pending_writes
    u32 pending_checks = 0;
    u64 ring_guest_copies = 0, ring_guest_bytes = 0; ///< stage A statistics (GOW3_FRAME_STATS)
    /// Stage A: guest copies in the constant ring since the last drain (RingReuse).
    struct RingCopy {
        u64 key;      ///< address ^ size << 48
        u64 epoch;
        u64 position; ///< absolute ring position
        u64 offset;   ///< in the ring
        u64 cpu_token; ///< 22 Oct 2026: MemoryTracker::CpuCleanToken before the copy (0: none)
        u64 watch_token; ///< 22 Oct 2026: RingWatchToken before the copy (0: none)
        u32 epochs;      ///< epochs in a row this range was copied in (this slot)
        bool watch_failed; ///< RingWatchToken refused this range (not asked again)
    };
    /// gow3 22 Oct 2026 (Gow3Toggle::RingDurableReuse): pages stage A write-protects itself so
    /// that copies of ranges it copies in epoch after epoch (static mesh data the ring path
    /// never hands to the buffer cache, so never CPU-clean there) can be bound again later. A
    /// CPU store into one faults; InvalidateMemory then drops the watch (RingUnwatch) and moves
    /// ring_watch_gen on, which every watch token compares with. Guarded by ring_watch_mutex
    /// (stage A adds watches, any faulting thread drops them); taken before the page locks.
    struct RingWatchPage {
        bool watched;
        u8 faults; ///< CPU writes that dropped its watch; RingWatchMaxFaults: never again
        u64 version; ///< unique promotion serial; a re-watched/re-mapped page gets a new one
    };
    std::mutex ring_watch_mutex;
    std::unordered_map<VAddr, RingWatchPage> ring_watch_pages;
    std::atomic<u64> ring_watch_gen{0};
    u64 ring_watch_version = 0; ///< under ring_watch_mutex, never reused after an unmap
    /// `probe` (GOW3_STAGEA_VERIFY): changes nothing; 0 when a page would need a new watch.
    u64 RingWatchToken(VAddr address, u64 size, bool probe = false);
    void RingUnwatch(VAddr address, u64 size, bool written);
    u64 StaticMeshWatchVersion(VAddr address, u64 size);
    bool TryStaticMeshSnapshot(u32 index, VAddr address, u64 size,
                              boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out);
    // Append-only: invalidating a lookup NEVER overwrites an earlier packet's snapshot.
    // Bounded to 64 MiB; exhausted or untracked inputs fall back to the constant ring.
    static constexpr u64 StaticMeshCapacity = 64ull << 20;
    static constexpr u64 StaticMeshOffsetFlag = 1ull << 63;
    struct StaticMeshCopy {
        VAddr address;
        u64 size;
        u64 offset;
        u64 version;
        u64 observed_gen;
    };
    std::unique_ptr<VideoCore::Buffer> static_mesh_buffer;
    std::unordered_map<u64, StaticMeshCopy> static_mesh_copies;
    u64 static_mesh_position = 0;
    u64 static_mesh_hits = 0, static_mesh_uploads = 0, static_mesh_full = 0;
    u64 static_mesh_rechecks = 0, static_mesh_checks = 0, static_mesh_mismatches = 0;
    static constexpr u32 RingCopyBits = 12;
    std::array<RingCopy, 1u << RingCopyBits> ring_copies{};
    // 23 Oct 2026 profile: CollectRingBindings is 31.6% of stage A, including 13% copying.
    // Heap storage avoids enlarging the rasterizer's stack allocation. Four candidates preserve
    // collision victims without changing the snapshot's validity or ring-allocation lifetime.
    static constexpr u32 RingSetBits = 14;
    using RingSet = std::array<RingCopy, 4>;
    std::unique_ptr<RingSet[]> ring_copy_sets{new RingSet[1u << RingSetBits]{}};
    u64 ring_epoch = 1;
    VAddr ring_epoch_min = ~VAddr{0}, ring_epoch_max = 0; ///< bounds of this epoch's copies
    u64 ring_reused_bytes = 0, ring_reuse_checks = 0, ring_reuse_mismatches = 0;
    /// gow3 19 Oct 2026: the 1 KiB lines this epoch's copies came from, as a hash table of epoch
    /// tags (a collision only starts a new epoch early). A GPU write starts a new epoch only
    /// over one of them (Gow3Toggle::RingPreciseWrites). U3 at 4K: 2.5 GB/s copied, about half of
    /// it repeating a range copied in the same flush, ~40 MB/s reused with the bounds test.
    static constexpr u32 RingLineBits = 15;
    static constexpr u32 RingLineShift = 10;
    std::unique_ptr<u64[]> ring_lines{new u64[1u << RingLineBits]{}};
    static u32 RingLineSlot(VAddr line) {
        return static_cast<u32>((line * 0x9E3779B97F4A7C15ull) >> (64 - RingLineBits));
    }
    void MarkRingLines(VAddr address, u64 size) {
        for (VAddr line = address >> RingLineShift; line <= (address + size - 1) >> RingLineShift;
             ++line) {
            ring_lines[RingLineSlot(line)] = ring_epoch;
        }
    }
    bool RingLinesOverlap(VAddr address, u64 size) const {
        if (size > (4ull << 20)) {
            return true; // not worth walking: a new epoch
        }
        for (VAddr line = address >> RingLineShift; line <= (address + size - 1) >> RingLineShift;
             ++line) {
            if (ring_lines[RingLineSlot(line)] == ring_epoch) {
                return true;
            }
        }
        return false;
    }
    /// What started each epoch: a drain, an in-order task, a GPU write, a guest submission.
    enum RingEpochCause : u32 { EpochDrain, EpochTask, EpochWrite, EpochSubmission, EpochCauses };
    std::array<u64, EpochCauses> ring_epoch_causes{};
    u64 ring_writes_kept = 0;   ///< GPU writes inside the bounds that kept the epoch
    u64 ring_command_drains_kept = 0;
    u64 ring_alloc_failures = 0; ///< copies left to stage B: the ring was still in use
    void NextRingEpoch(RingEpochCause cause = EpochDrain) {
        ++ring_epoch;
        ++ring_epoch_causes[cause];
        ring_epoch_min = ~VAddr{0};
        ring_epoch_max = 0;
    }
    void CheckRingReuse(VAddr address, const u8* copy, u64 size);
    /// gow3 21 Oct 2026 (GOW3_PIPE_WRITE_END): writes noted for the packet PostDraw is building
    /// take this position until it is committed (then its end: stage B has run it once there).
    static constexpr u64 PendingUncommitted = ~u64{0};
    bool noting_packet = false;
    std::vector<u32> uncommitted_indices; ///< pending_writes entries to set at the commit
    /// gow3 21 Oct 2026 (GOW3_SRT_PENDING_DRAIN): destinations of the copy shaders stage B runs
    /// (scalar copy HLE etc.), which the next draws' resource tables read. Small (they merge).
    bool noting_copy = false, copy_uncommitted = false;
    std::vector<PendingWrite> pending_copy_writes;
    VAddr copy_min = ~VAddr{0}, copy_max = 0;
    bool PendingCopyWriteOverlaps(VAddr address, u64 size);
    /// gow3 21 Oct 2026 (GOW3_SRT_PENDING_DRAIN): walks that drained the pipe for a pending write.
    u64 srt_pending_drains = 0, srt_pending_changed = 0, srt_pending_zero = 0, srt_pending_ns = 0;
    u64 proxy_samples = 0; ///< texture bindings that read a scene proxy (statistics)
    float sampler_lod_bias = 0.0f; ///< gow3: extra bias of this draw's samplers
    /// gow3 21 Oct 2026: anisotropy forced on this draw's mipmapped linear samplers (2-16), 0 = the
    /// game's own. Set per BindResources call, like the bias, because the setting is live.
    u32 sampler_forced_aniso = 0;
    bool sampler_graphics = false; ///< this BindResources call is a graphics draw's
    bool scene_debug_frame = false; ///< GOW3_SCENE_DEBUG: this frame's passes are printed
    bool PendingWriteOverlaps(VAddr address, u64 size);
    /// Stage A: drops pending_writes stage B has done (only with no uncommitted entry); returns
    /// how many.
    u32 PrunePendingWrites();
    void PrunePendingLabels();
    /// gow3 21 Oct 2026: stage A waits until stage B has run the packets that write the range
    /// (`writes`; not the whole pipe: the work queued after them keeps stage B busy meanwhile).
    void WaitPendingWrites(const std::vector<PendingWrite>& writes, VAddr address, u64 size);
    /// Stage B: the ring bindings of the stages of the packet being recorded.
    struct RingStage {
        const Shader::Info* info;
        const RingBinding* bindings;
        u32 count;
    };
    std::array<RingStage, Shader::MaxStageTypes> ring_stages{};
    u32 num_ring_stages = 0;
    const RingBinding* FindRingBinding(const Shader::Info& stage, u32 index) const {
        for (u32 i = 0; i < num_ring_stages; ++i) {
            if (ring_stages[i].info == &stage) {
                for (u32 j = 0; j < ring_stages[i].count; ++j) {
                    if (ring_stages[i].bindings[j].index == index) {
                        return &ring_stages[i].bindings[j];
                    }
                }
                return nullptr;
            }
        }
        return nullptr;
    }
    std::unique_ptr<ConstantRing> constant_ring;
    // gow3: Rasterizer::Flush calls (either pipe stage), and the GOW3_RING_STATS counters.
    std::atomic<u64> flush_count{0};
    u64 ring_copy_count = 0, ring_copy_bytes = 0, ring_repeat_count = 0, ring_repeat_bytes = 0;
    std::array<u64, 4> ring_size_buckets{};
    std::unordered_set<u64> ring_seen;
    u64 ring_seen_flush = ~0ULL;
    /// Submissions (prepared draws) kept alive until stage B reaches the position.
    std::deque<std::pair<u64, std::shared_ptr<const void>>> pipe_keepalive;
    /// gow3 24 Oct 2026 (Gow3Toggle::DispatchRuns, GOW3_DISPATCH_RUNS=1): consecutive cs
    /// 8f067540 dispatches staged by stage A, handed to stage B as one packet of up to 64.
    static constexpr u32 RunMaxFlat = 256; ///< flattened user data dwords of one instance
    /// An in-order task after an instance (a compute RELEASE_MEM): run by stage B after the
    /// dispatches before it are recorded.
    struct RunTask {
        OrderedTask task;
        u32 size;
        std::array<u8, 64> data;
    };
    struct RunInstance {
        AmdGpu::UserData ud;
        std::array<u32, RunMaxFlat> flat;
        boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS> rings;
        u64 ring_end; ///< constant ring position after its copies
        u32 num_tasks;
        /// runv4 (24 Oct 2026): with 2, ~16k runs per 5 s ended at a third release.
        std::array<RunTask, 4> tasks;
    };
    struct DispatchRun {
        u32 count = 0;
        u64 generation = 0;  ///< flushes so far (one inside an instance's staging moves it)
        u32 uncommitted = 0; ///< uncommitted_indices entries of the staged instances
        u32 labels = 0;      ///< pending_labels entries of the staged tasks
        const ComputePipeline* pipeline = nullptr;
        const AmdGpu::ComputeProgram* queue = nullptr; ///< the queue's compute registers
        AmdGpu::ComputeProgram cs{}; ///< the first instance's (all equal but the user data)
        std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent{};
        AmdGpu::CbDbExtent db_extent{};
        VAddr pgm_base = 0;
        u32 ud_size = 0, flat_size = 0; ///< the stage's user data and flattened user data dwords
        std::unique_ptr<RunInstance[]> instances; ///< capacity + 1: the one being staged
    } dispatch_run;
    /// Stage A: true when the dispatch joined a run or went out as a packet of its own.
    bool StageDispatchRun(const ComputePipeline* pipeline, const AmdGpu::Buffer* sharps);
    void FlushDispatchRun(u32 reason, u32 detail = 0);
    void EmitRunPacket(const RunInstance* instances, u32 count);
    /// Stage A: the first `head` uncommitted writes are the packet just committed.
    void ResolveRunWrites(u32 head);
    /// Stage B: a DispatchRun packet; its first instance is installed as for a dispatch,
    /// `tail` up to `end` holds the others.
    void DispatchRunRecord(const ComputePipeline* pipeline, const u8* tail, const u8* end);
    struct RunPlan;
    std::unique_ptr<RunPlan> run_plan; ///< stage B scratch
    std::vector<u8> release_batch; ///< stage B: staged release bytes (ReleaseBatchHooks)
    u64 release_batch_hash = 0;
    /// Stage B, GOW3_DISPATCH_RUN_VERIFY=1: the replayed instance DispatchRecord compares.
    u32 run_verify_index = ~0u;
    std::vector<std::pair<VAddr, u64>> run_verify_writes; ///< its GPU-written views
    void VerifyRunInstance();
    std::unique_ptr<DrawPipe> draw_pipe;
};

} // namespace Vulkan
