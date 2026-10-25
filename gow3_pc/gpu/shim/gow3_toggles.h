// gow3: optimizations that can be switched off while the game runs (GOW3_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
// gow3 (Windows): no signals. The loader's exception handler resumes the faulting thread
// in gow3_longjmp (src/compat_win.c: no unwinding through the faulting frames).
typedef unsigned long long sigjmp_buf[32];
extern "C" __attribute__((returns_twice)) int gow3_setjmp(sigjmp_buf buffer);
#define sigsetjmp(buffer, save) gow3_setjmp(buffer)
#endif

extern "C" std::uint64_t runtime_disabled_optimizations;
/// The toggle file's second number (Disabled2): switches added once the first 64 bits ran out.
extern "C" std::uint64_t runtime_disabled_optimizations2;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace Gow3Toggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    /// Bit set: off. Draws between two draw-pipe drains that bind the same small guest buffer
    /// share one constant ring copy (Uncharted 1: 70% of 2.3 GB/s of copies, 18 Oct).
    RingReuse = 1ull << 56,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    /// Tests only (bit set: zero): the upscaler gets zero motion vectors (negative control).
    UpscalerMotion = 1ull << 58,
    /// Bit set: off. Lock-free per-thread cache of mapped ranges for Rasterizer::IsMapped (the
    /// SRT walker sync asks several times per draw; the shared lock's bookkeeping cost ~7% of
    /// the GPU command thread in Uncharted 3, 18 Oct profile).
    IsMappedCache = 1ull << 59,
    /// Bit set: off. Held recursive locks in a per-thread array (common/recursive_lock.cpp)
    /// instead of a map node allocated and freed per acquisition.
    RecursiveLockArray = 1ull << 60,
    /// One upscaler frame start per frame (Uncharted draws twice into its display buffer).
    /// Upstream 0.3.1 has it at bit 59; moved here because the fork uses 59 and 60.
    OneFrameStart = 1ull << 61,
    /// Upscaler jitter for the full-screen passes in GOW3_JITTER_PS (the sun shadow pass).
    /// Upstream 0.3.1: bit 60.
    FullScreenPassJitter = 1ull << 62,
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
/// The toggle file's second number ("<first> <second>"); bit set: off.
enum : std::uint64_t {
    /// A guest write fault into GPU-written memory waits on its own thread for the GPU work that
    /// wrote the page before the GPU command thread fetches it (19 Oct: ~140 a second in U3 at
    /// 4K, ~1.4 ms each of the command thread's time waiting for the GPU).
    FaultPrewait = 1,
    /// Bit set: old behaviour. Texture GC: at most one synchronous write-back download per pass,
    /// images it keeps are touched so they do not block the walk, the deletion quota counts only
    /// freed images, bound / render-target / video-out images are kept, 512 visits a pass (A/B).
    GcPassLimits = 2,
    /// A compute queue INDIRECT_BUFFER packet does not drain the draw pipe (its packets pass the
    /// drain check one by one, as on the graphics queue). U3 at 4K DLAA M: +2% FPS, p99 -3 ms.
    ComputeNestedNoDrain = 4,
    /// A flip's frame is presented as soon as it is prepared; the guest-visible flip still waits
    /// for the fences queued before it (VideoOutDriver::SubmitFlipInternal).
    PresentBeforeFlip = 8,
    /// An end-of-pipe flip's frame is prepared when its flip packet is reached in stream order,
    /// not when its interrupt fires (that waits for the ray-cast readbacks before it, and by then
    /// the next frame's work was queued ahead of the frame's last submission).
    PrepareAtFlipPacket = 16,
    /// A GPU write starts a new constant ring epoch (no reuse of earlier copies) only when it
    /// overlaps a 1 KiB line a copy of the epoch came from, not whenever it falls between the
    /// lowest and highest copied address (19 Oct: in U3 that span covered nearly every write).
    RingPreciseWrites = 32,
    /// Draining the draw pipe for a queued command (guest fault downloads, readback landings,
    /// flips) keeps the ring epoch: those write only GPU-written memory, which is never copied.
    RingKeepOnCommands = 64,
    /// Bit set: SRT walks skip waits for queued writes (A/B only).
    SrtPipeCheck = 128,
    /// Inverted sense, opt-in diagnostic: bit set drains the pipe before copy shaders
    /// 2d215c3b, 6a6422a9 and c92150ba run.
    DrainCopyShaders = 256,
    /// Bit set: off. Use the old 256-range SRT pipe check, including overflow drains.
    SrtPendingWrites = 512,
    /// Bit set: off. Shader specialization reads a garbage buffer V# (bound as null) as it is,
    /// so its random stride/format makes new permutations (21 Oct: U3 treasure effect freezes).
    SpecGarbageSharps = 1024,
    /// Bit set: off. File reads into tracked memory fault once per 4 KiB page again (U3 chapter
    /// load, 21 Oct: thousands of write faults a second) instead of preparing the range once.
    RangeWritePrepare = 2048,
    /// Bit set: old behaviour. 21 Oct U3: DLSS at DLAA uses the guide's texture LOD bias -1 (was 0).
    DlaaMipBias = 4096,
    /// Bit set: old behaviour. 21 Oct U3: prewait even when the write misses GPU-written bytes
    /// (the shared-page path needs no wait: ~746 prewaits per 5 s, ~1.1 s waited).
    PrewaitAlways = 8192,
    /// Bit set: old behaviour. 21 Oct U3 4K Performance: DLSS created its feature on the first
    /// scene frame (80-230 ms stall); on, it is created at the first frame start in the menus.
    DlssPrecreate = 16384,
    /// Bit set: off. File reads fetch whole overwritten pages again (21 Oct U3 load:
    /// 532 pages cost 50.1 ms); on, one queued command discards their GPU bytes.
    FileReadDiscard = 32768,
    /// Bit set: off. Scene samplers keep the game's anisotropy (A/B of the anisotropy setting,
    /// Rasterizer::ForceAnisotropy). Was 4096 until the 21 Oct merge with upstream (DlaaMipBias).
    ForcedAniso = 65536,
    /// Bit set: old behaviour. 22 Oct U3 meat market: per compute dispatch, adjacent GPU-written
    /// ranges join one pending interval (one interval set insert per run), page write ticks
    /// already noted at this tick and resident blocks are not looked up again, and a shader that
    /// writes no metadata surface skips the metadata-clear check of its reads. (131072 and 262144
    /// are taken by work in progress.)
    DispatchBindFastPaths = 524288,
    /// Bit set: old behaviour. 22 Oct U3 meat market: a constant ring copy is reused after its
    /// epoch ended (~34,500 guest epochs a second) while page tracking shows the CPU cannot have
    /// written the range since and no GPU or backing write ever reached it.
    RingDurableReuse = 1048576,
    /// Bit set: old behaviour. 22 Oct 2026 U3 meat market: per compute dispatch, stage A reads
    /// each buffer V# once (the specialization's GetSharp, reused by the dispatch's write ranges
    /// and ring bindings), skips the effect shaders' second garbage check of a V# GetSharp has
    /// just passed, reuses a ring watch token no page dropped since, and skips the pending-label
    /// scan of an SRT read outside the bounds of every pending label.
    DispatchStageAMemos = 2097152,
    /// Bit set: old behaviour. 22 Oct 2026 U3 meat market (after phase 5b): the small-write page
    /// ticks go through a direct-mapped write-back cache of 16,384 pages in front of their hash
    /// map (its find/assign were 3.5% of stage B; cs 8f067540 writes the same ~3 pages per
    /// dispatch every frame). GOW3_PAGETICK_VERIFY=1 checks each cached read against the map.
    PageTickCache = 4194304,
    /// Bit set: NVIDIA Reflex is off (no low latency mode, no sleep before the first pad read of a
    /// frame, no markers unless GOW3_REFLEX_LOG is set), whatever the `reflex` setting says: a live
    /// A/B in one run (gow3_reflex.cpp, 22 Oct 2026).
    ReflexSleep = 8388608,
    /// Bit set: DLSS Frame Generation is off (real frames only, the plain frame limit), whatever
    /// the `frame_gen` setting says: a live A/B in one run (vk_presenter.cpp, 22 Oct 2026).
    FrameGeneration = 16777216,
    /// Bit set: Reflex keeps its low latency mode and markers but never sleeps (diagnostic for
    /// isolating the sleep's cost; the ReflexSleep bit still turns everything off), 22 Oct 2026
    /// (gow3_reflex.cpp).
    ReflexNoSleep = 33554432,
    /// Bit set: DLSS Frame Generation's sleep between the generated and the real present (half the
    /// last real interval) uses std::this_thread::sleep_until, which on Windows rounds to whole
    /// milliseconds; clear, the high-resolution waitable timer. A live A/B (vk_presenter.cpp,
    /// 22 Oct 2026).
    FrameGenStdSleep = 67108864,
    /// Bit set: old behaviour. Clear, while Reflex sleeps, the GPU command thread submits a
    /// sceGnmSubmitDone batch's recorded work every GOW3_REFLEX_EARLY_DRAWS draws and dispatches
    /// (default 80, 0 never) instead of only at its end. 22 Oct 2026 GPU profile (U3 ch.2, 4K
    /// DLAA, RTX 5070): with the sleep the GPU idled 3.48 ms a frame before those end-of-batch
    /// flushes, 0.21 ms without (liverpool.cpp, EarlySubmit).
    ReflexEarlySubmit = 134217728,
    /// Bit set: old behaviour. Clear, a guest read fault on a page that pending readbacks
    /// read-protect, missing all of their GPU bytes (false sharing), makes the page readable on
    /// the faulting thread; the command thread unwatches it later as before. 23 Oct 2026 U3
    /// Cartagena street (4K, FG): ~35 such faults/s, ~1.3 ms each through the command thread.
    GuestFalseSharing = 268435456,
    /// Bit set: old behaviour. Clear, a guest read of GPU bytes still on their way back waits for
    /// their submission on the faulting thread before asking the command thread to land them.
    /// 23 Oct 2026 U3 Cartagena street: ~15 such faults/s, ~10 ms each with the command thread
    /// waiting for the GPU (CompleteReadbacks) and feeding it nothing meanwhile.
    GuestReadPrewait = 536870912,
    /// Experiment, GOW3_FORWARD_COPIES=1 only (default off after the protected-destination
    /// prototype regressed). Bit set: old behaviour. Clear, a guest memcpy whose source holds
    /// GPU bytes still on their way back is made by the GPU instead, in stream order after the work that wrote them
    /// (Rasterizer::ForwardGuestCopy). 23 Oct 2026 U3 Cartagena street: PrepareInstanceForRendering
    /// (fg-draw-mgr.cpp, guest 0xe3c6e7) copies cloth-solver bone matrices into the frame's
    /// skinning palette, ~20 times a second, each read waiting ~9.5 ms for the GPU.
    ForwardedCopies = 1073741824,
    /// 23 Oct 2026: stage-A snapshots use a four-way cache instead of one direct-mapped slot.
    /// Keep the snapshots (not live guest pointers) across packets and reuse only with the
    /// existing epoch/page tokens and constant-ring lifetime checks. Set: the old cache.
    RingAssociative = 1ull << 31,
    /// Rebuild compute specializations in reusable scratch storage, rather than initializing
    /// and destroying their resource vectors for every dispatch. Set: old construction.
    ComputeSpecScratch = 1ull << 32,
    /// Validate the previous buffer-only compute variant directly against current descriptors
    /// instead of constructing temporary specialization vectors. Set: full construction.
    ComputeVariantCheck = 1ull << 33,
    /// Opt-in experiment (GOW3_COMMANDS_STAGE_B=1): cache/fault commands execute in-order on
    /// stage B rather than draining it on stage A. Set: original command-thread execution.
    CommandsOnStageB = 1ull << 34,
    /// Opt-in (GOW3_STATIC_MESH_SNAPSHOTS=1): immutable crowd inputs survive ring recycling in
    /// an append-only GPU buffer. CPU write/unmap versions invalidate lookups, not queued bytes.
    /// Set: original transient snapshots. 23 Oct short tests: no material FPS gain established.
    StaticMeshSnapshots = 1ull << 35,
    /// Opt-in (GOW3_SKINNING_WRITE_BATCH=1): acquire the overlapping output views of one crowd
    /// dispatch together. Keeps each descriptor's offset/size and each exact barrier range.
    CoalescedSkinningWrites = 1ull << 36,
    /// Opt-in (GOW3_FORWARD_COPY_DEFER=1, also needs forwarded copies): protect a memcpy
    /// destination before returning, but submit with normal work unless the CPU consumes it.
    ForwardCopyDefer = 1ull << 37,
    /// Opt-in (GOW3_DISPATCH_FUSION=1): one crowd workgroup per mesh instance, batched as Y
    /// groups with per-instance descriptor/push-data arrays. Set: one dispatch per instance.
    DispatchFusion = 1ull << 38,
    /// Set: Reflex uses the driver's/Streamline's sleep (old behaviour). Clear: the GPU-gated pacer
    /// (the guest waits until the GPU finished frame id - lag; default).
    ReflexPacer = 1ull << 39,
    /// Opt-in (GOW3_DISPATCH_RUNS=1): consecutive cs 8f067540 dispatches go to stage B as one pipe
    /// packet per run of up to 64 and are recorded as one batched dispatch (24 Oct 2026 U3
    /// Cartagena: 1,522 a frame, ~3.7 ms of stage A and ~4.4 ms of stage B). Set: one packet and
    /// one dispatch each, read live on both stages (vk_rasterizer.cpp, DispatchRunRecord).
    DispatchRuns = 1ull << 40,
    /// Bit set: old behaviour (read live). Clear, the compute releases staged in a batched
    /// dispatch run reach the Vulkan recorder as one signal (24 Oct 2026 U3 street: ~20 a run,
    /// ~666k per 5 s, each with its own download pass, Record and kick on stage B).
    RunReleaseBatch = 1ull << 41,
    /// Bit set: old behaviour (read live). Clear, a camera cut (forward axis turned over 45
    /// degrees, a move over max(GOW3_CUT_MOVE 1.0, 16x the median move), or a previous camera
    /// older than 0.5 s) resets the upscaler history and zeroes that frame's camera motion
    /// (gow3_camera_cut.h). 25 Oct 2026 flight recorder, U3 chapter 2 Continue: the first scene
    /// frame turned 161 degrees and moved 106 units with no reset.
    CameraCutReset = 1ull << 42,
    /// Bit set: old behaviour (read live). Clear, the scene LOD bias skips samplers the shader
    /// uses with an explicit LOD (Shader::SamplerResource::explicit_lod): Vulkan adds mipLodBias
    /// to those samples too, so a mip picked on purpose read the wrong level (25 Oct 2026, the
    /// player saw blurred or wrong materials).
    BiasExplicitLod = 1ull << 43,
    /// Bit set: old behaviour (read live). Clear, forced anisotropy skips samplers used only with
    /// an explicit LOD, clamped to MaxLod 0, or paired only with one-level images (atlases, LUTs,
    /// decals): anisotropy at level 0 reads across cell edges (25 Oct 2026, lines at edges).
    AnisoUnmipped = 1ull << 44,
};
inline bool Disabled2(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations2, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace Gow3Toggle

namespace Gow3Stats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Region-lock diagnostics, collected only with GOW3_FAULT_STATS=1.
inline const bool fault_stats = [] {
    const char* env = std::getenv("GOW3_FAULT_STATS");
    return env && env[0] == '1';
}();
inline std::atomic<std::uint64_t> buffer_lock_wait_ns{0}, buffer_under_lock_ns{0};
/// 21 Oct: where BufferCache::InvalidateMemory's on_flush time goes (GOW3_FAULT_STATS=1, 5 s).
/// ff_ns/ff_n by phase of ReadMemory(is_write): see FF_* below; ff_total_* is the whole on_flush.
enum FaultFlushPhase { FF_RBWAIT, FF_EARLY, FF_FETCH, FF_LAZY, FF_LAZYWRITE, FF_PREWAIT, FF_DOWNLOAD,
                       FF_OTHER, FF_COUNT };
inline std::atomic<std::uint64_t> ff_ns[FF_COUNT], ff_n[FF_COUNT], ff_total_ns{0}, ff_total_n{0},
    ff_prewait_wait_ns{0}, ff_prewait_skipped{0};
/// Stall diagnostics (GOW3_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<unsigned long> gpu_thread_id{0}; ///< Windows thread id of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Upscaler history resets for camera cuts (CameraCutReset), printed and zeroed by Frame stats.
inline std::atomic<std::uint64_t> camera_cuts{0};
/// Scene sampler binds (BiasExplicitLod, AnisoUnmipped), printed and zeroed by Frame stats.
inline std::atomic<std::uint64_t> samplers_biased{0}, bias_skipped_explicit{0}, aniso_raised{0},
    aniso_skipped_unmipped{0}, aniso_skipped_maxlod0{0}, aniso_skipped_explicit{0},
    bias_skipped_mixed{0}; // bias_skipped_explicit of which: the sampler also has implicit samples
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0}, t_gc{0}, t_readback_wait{0};
/// 21 Oct 2026: a post-movie frame spent 63 ms in image refresh; its parts, to find the cost:
/// new staging buffers, inline guest copies for images, detile (whole) and its pipeline creation.
inline std::atomic<std::uint64_t> t_staging_alloc{0}, t_refresh_copy{0}, t_detile{0},
    t_detile_pipeline{0};
/// Diagnostics are collected only with GOW3_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = std::getenv("GOW3_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// gow3 20 Oct 2026 (hybrid laptops): wall time (ns) waiting on presentation: for a free
/// presentation frame (Presenter::GetRenderFrame), for the submit lock (draw scheduler), and in
/// the swapchain's acquire and present calls (swap thread).
inline std::atomic<std::uint64_t> frame_wait_ns{0}, submit_lock_ns{0}, acquire_ns{0},
    present_ns{0};
/// gow3 20 Oct 2026 (GitHub #95, memory over long sessions): video memory held by the buffer
/// arenas (residency chunks and ranges; never given back) and by the registered images.
inline std::atomic<std::uint64_t> arena_memory_bytes{0}, image_bytes{0}, images_live{0};
/// gow3 20 Oct 2026: arena memory bound to guest ranges now, and given back so far.
inline std::atomic<std::uint64_t> arena_in_use_bytes{0}, arena_released_bytes{0};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
} // namespace Gow3Stats
