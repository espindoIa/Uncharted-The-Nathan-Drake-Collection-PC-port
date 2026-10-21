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
    /// Bit set: off. Shader specialization reads a garbage buffer V# (bound as null) as it is,
    /// so its random stride/format makes new permutations (21 Oct: U3 treasure effect freezes).
    SpecGarbageSharps = 1024,
    /// Bit set: off. File reads into tracked memory fault once per 4 KiB page again (U3 chapter
    /// load, 21 Oct: thousands of write faults a second) instead of preparing the range once.
    RangeWritePrepare = 2048,
    /// Bit set: old behaviour. 21 Oct U3: prewait even when the write misses GPU-written bytes
    /// (the shared-page path needs no wait: ~746 prewaits per 5 s, ~1.1 s waited).
    PrewaitAlways = 8192,
    /// Bit set: old behaviour. 21 Oct U3 4K Performance: DLSS created its feature on the first
    /// scene frame (80-230 ms stall); on, it is created at the first frame start in the menus.
    DlssPrecreate = 16384,
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
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
} // namespace Gow3Stats
