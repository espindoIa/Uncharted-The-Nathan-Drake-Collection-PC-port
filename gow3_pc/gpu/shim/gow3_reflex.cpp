// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 22 Oct 2026: see gow3_reflex.h. The use of the extension follows NVIDIA's own wrapper of it
// (Streamline, source/platforms/sl.chi/vknvll2.cpp, MIT): the swapchain handle and every driver
// call that names it sit under one mutex, so a swapchain that is being destroyed is never in use,
// and the sleep's semaphore wait runs outside it. This machine's driver (616.92, RTX 5070)
// advertises revision 2 of the extension: it attributes every vkQueueSubmit between two presents
// to the later present's frame and ignores VkLatencySubmissionPresentIdNV, so there is nothing to
// tag on the submissions; the markers are the whole interface. The port is CPU bound (the draw and
// GPU command threads), so a large latency gain is not expected: the aim is a correct integration
// and the driver's own timings to measure it with.
#include "gow3_reflex.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "gow3_settings.h"
#include "gow3_streamline.h"
#include "gow3_toggles.h"

namespace Gow3Reflex {

namespace {

using Clock = std::chrono::steady_clock;

/// The driver keeps 64 frame reports (the size NVIDIA's Streamline reads). 22 Oct 2026, a probe
/// outside the game (RTX 5070, driver 616.92, 300 frames, the mode never set, off, on and boost):
/// the count query answered 64 each time, every report had all its timestamps, and the newest was
/// 8 frames behind the last present. A read so covers 56 frames: every 48 frames the reads overlap
/// (the log keys on presentID) and none is missed; every 120 would log about half of them.
constexpr u32 MaxReports = 64;
constexpr u64 ReportEveryFrames = 48;
/// A frame this much older than the newest is final even without GPU times.
constexpr u32 FramesInFlight = 8;
/// vkLatencySleepNV returns at once; the semaphore it signals is the sleep. A frame longer than
/// this (under 25 FPS) times out and the game goes on unpaced.
constexpr u64 SleepTimeoutNs = 40'000'000;
/// No game frame was presented for this long (loading, pause): no sleep, the driver has no frame
/// rate to follow.
constexpr s64 IdleNs = 250'000'000;
/// Timeouts in a row before sleeping stops for a while (the driver does not release the game).
constexpr unsigned TimeoutsBeforeBackoff = 3;
constexpr s64 BackoffNs = 1'000'000'000;
constexpr s64 StatsNs = 5'000'000'000;

struct State {
    vk::Device device;
    vk::Semaphore semaphore; ///< timeline: vkLatencySleepNV signals it, the guest thread waits
    std::FILE* log = nullptr; ///< GOW3_REFLEX_LOG
    std::atomic<bool> available{false};
    /// gow3 23 Oct 2026: Streamline's sl.reflex and sl.pcl take the markers, the sleep and the mode
    /// (InitStreamline); the extension is then Streamline's and nothing here calls it.
    std::atomic<bool> streamline{false};
    /// Markers go to the driver (the mode is on, or the log): the one flag the idle paths read.
    std::atomic<bool> active{false};
    /// The mode the driver holds for the current swapchain.
    std::atomic<int> applied_mode{0};

    /// Guards the swapchain handle, the sleep counter and the driver calls that name the swapchain.
    std::mutex mutex;
    vk::SwapchainKHR swapchain;
    u64 sleep_value = 0;
    /// The driver refused the mode or a sleep: no sleeping until a new swapchain.
    bool sleep_broken = false;

    /// Flips asked for by the guest and frames prepared by the presenter: a frame's id.
    std::atomic<u64> guest_flips{0}, prepared_flips{0};
    /// Armed by a flip request: the next pad read / submit is the first of its frame.
    std::atomic<bool> first_read{true}, first_submit{true};
    std::atomic<s64> last_present_ns{0};     ///< steady clock, of the last game frame presented
    std::atomic<s64> skip_sleep_until_ns{0}; ///< after repeated timeouts
    std::atomic<unsigned> timeouts_in_row{0};
    std::atomic<u64> sleeps{0}, sleep_ns{0}, timeouts{0};

    /// 24 Oct 2026, the pacer: per frame id % 64 the draw scheduler's timeline tick that marks the
    /// frame's GPU work done; the id is written last (release) so a reader of the id sees its tick.
    struct Queued {
        std::atomic<u64> id{0}, tick{0};
    };
    std::array<Queued, 64> queued{};
    std::atomic<VkSemaphore> pacer_semaphore{VK_NULL_HANDLE};
    std::atomic<VkDevice> pacer_device{VK_NULL_HANDLE};
    std::atomic<u64> pacer_waits{0}, pacer_wait_ns{0}, pacer_timeouts{0};
    /// GOW3_REFLEX_VARIANT, read once in Init. 22 Oct 2026, DIAGNOSTIC ONLY, not a default: where
    /// RenderSubmitStart is sent. 0 = today (first submit after a flip); 2 "chain" = frame N+1's
    /// start follows RenderSubmitEnd(N), so the span is stage B's whole frame; 3 "zero" = start
    /// right before RenderSubmitEnd (near-zero span). Tests whether the driver's pacing models
    /// simulation and render-submit as sequential CPU work while the port overlaps them.
    int variant = 0;
    std::atomic<bool> chain_started{false}; ///< variant 2: the first GuestSubmit sent frame 1's start

    /// Per frame id % 1024, for the log: 0 Reflex off, 1 on with sleep, 2 on with the sleep disabled
    /// by the ReflexNoSleep toggle. Rows carry it so an A/B needs no join by time (the driver's
    /// timestamps are not on the host clock).
    std::array<std::atomic<u8>, 1024> frame_state{};

    std::mutex report_mutex; ///< guards the log file and last_logged
    u64 last_logged = 0;     ///< the newest presentID written to the log
};
State g;

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

/// The setting, unless the toggle file switches it off (bit ReflexSleep) for a live A/B.
int EffectiveMode() {
    if (Gow3Toggle::Disabled2(Gow3Toggle::ReflexSleep)) {
        return 0;
    }
    const int mode = std::clamp(Gow3Settings::Get().reflex.load(std::memory_order_relaxed), 0, 2);
    // 23 Oct 2026: Streamline's frame generation reports eFailReflexNotDetectedAtRuntime (and its
    // guide 8.0 requires Reflex) while Reflex is off, so it runs with at least Reflex On.
    if (mode == 0 && g.streamline.load(std::memory_order_relaxed) &&
        Gow3Settings::Get().frame_gen.load(std::memory_order_relaxed) &&
        !Gow3Toggle::Disabled2(Gow3Toggle::FrameGeneration) && Gow3Streamline::FrameGenReady()) {
        return 1;
    }
    return mode;
}

Gow3Streamline::Marker StreamlineMarker(vk::LatencyMarkerNV marker) {
    using M = Gow3Streamline::Marker;
    switch (marker) {
    case vk::LatencyMarkerNV::eSimulationStart: return M::SimulationStart;
    case vk::LatencyMarkerNV::eSimulationEnd: return M::SimulationEnd;
    case vk::LatencyMarkerNV::eRendersubmitStart: return M::RenderSubmitStart;
    case vk::LatencyMarkerNV::eRendersubmitEnd: return M::RenderSubmitEnd;
    case vk::LatencyMarkerNV::ePresentStart: return M::PresentStart;
    case vk::LatencyMarkerNV::ePresentEnd: return M::PresentEnd;
    default: return M::ControllerInputSample; // the input sample (PCL has no other for it)
    }
}

/// Sets the mode on the current swapchain. Under g.mutex, with a swapchain.
void ApplyLocked(int mode) {
    const vk::LatencySleepModeInfoNV info = {
        .lowLatencyMode = mode != 0,
        .lowLatencyBoost = mode == 2,
        .minimumIntervalUs = 0,
    };
    const vk::Result result = g.device.setLatencySleepModeNV(g.swapchain, info);
    if (result != vk::Result::eSuccess) {
        // Stored anyway: no retry every frame. Markers and timings may still work.
        g.sleep_broken = mode != 0;
        std::printf("Reflex: vkSetLatencySleepModeNV(mode %d) failed: %s%s\n", mode,
                    vk::to_string(result).c_str(), mode != 0 ? "; no sleeping" : "");
    }
    g.applied_mode.store(mode, std::memory_order_relaxed);
    static int announced = -1; // a swapchain re-created with the same mode says nothing
    if (mode != announced) {
        announced = mode;
        std::printf("Reflex: mode %d (%s)\n", mode,
                    mode == 0 ? "off" : mode == 1 ? "on" : "on + boost");
    }
}

void MarkerLocked(u64 id, vk::LatencyMarkerNV marker) {
    const vk::SetLatencyMarkerInfoNV info = {.presentID = id, .marker = marker};
    g.device.setLatencyMarkerNV(g.swapchain, info);
}

void Marker(u64 id, vk::LatencyMarkerNV marker) {
    if (g.streamline.load(std::memory_order_relaxed)) {
        Gow3Streamline::SetMarker(StreamlineMarker(marker), id);
        return;
    }
    std::scoped_lock lock{g.mutex};
    if (g.swapchain) {
        MarkerLocked(id, marker);
    }
}

/// The driver paces the game here: vkLatencySleepNV, then the semaphore it signals when the game
/// should start sampling input for its next frame.
void Sleep(u64 id) {
    const s64 start = NowNs();
    const s64 last_present = g.last_present_ns.load(std::memory_order_relaxed);
    if (last_present == 0 || start - last_present > IdleNs ||
        start < g.skip_sleep_until_ns.load(std::memory_order_relaxed)) {
        return;
    }
    if (g.streamline.load(std::memory_order_relaxed)) {
        // slReflexSleep waits for the driver itself (sl.chi, vknvll2.cpp).
        Gow3Streamline::Sleep(id);
        g.sleeps.fetch_add(1, std::memory_order_relaxed);
        g.sleep_ns.fetch_add(u64(NowNs() - start), std::memory_order_relaxed);
        return;
    }
    u64 value;
    {
        std::scoped_lock lock{g.mutex};
        // Without the mode applied the semaphore would never be signalled: a wait for nothing.
        if (!g.swapchain || g.sleep_broken || g.applied_mode.load(std::memory_order_relaxed) == 0) {
            return;
        }
        value = ++g.sleep_value;
        const vk::LatencySleepInfoNV info = {.signalSemaphore = g.semaphore, .value = value};
        if (const vk::Result result = g.device.latencySleepNV(g.swapchain, info);
            result != vk::Result::eSuccess) {
            g.sleep_broken = true;
            std::printf("Reflex: vkLatencySleepNV failed: %s; no sleeping\n",
                        vk::to_string(result).c_str());
            return;
        }
    }
    const vk::SemaphoreWaitInfo wait = {
        .semaphoreCount = 1,
        .pSemaphores = &g.semaphore,
        .pValues = &value,
    };
    const s64 wait_start = NowNs();
    const vk::Result result = g.device.waitSemaphores(wait, SleepTimeoutNs);
    g.sleeps.fetch_add(1, std::memory_order_relaxed);
    g.sleep_ns.fetch_add(u64(NowNs() - wait_start), std::memory_order_relaxed);
    if (result == vk::Result::eSuccess) {
        g.timeouts_in_row.store(0, std::memory_order_relaxed);
        return;
    }
    g.timeouts.fetch_add(1, std::memory_order_relaxed);
    if (g.timeouts_in_row.fetch_add(1, std::memory_order_relaxed) + 1 >= TimeoutsBeforeBackoff) {
        g.timeouts_in_row.store(0, std::memory_order_relaxed);
        g.skip_sleep_until_ns.store(NowNs() + BackoffNs, std::memory_order_relaxed);
    }
}

/// 24 Oct 2026: the pacer stands in for the driver sleep unless ReflexPacer or ReflexNoSleep is set.
bool PacerReplacesSleep() {
    return EffectiveMode() != 0 && !Gow3Toggle::Disabled2(Gow3Toggle::ReflexPacer) &&
           !Gow3Toggle::Disabled2(Gow3Toggle::ReflexNoSleep);
}

/// 24 Oct 2026: replaces the driver sleep (measured on U3 4K FG: 12.7-14.6 ms of sleep per frame,
/// 42 FPS; without it 53 FPS) by waiting until the GPU finished frame id - lag, so it never starves.
void PacerWait(u64 id) {
    static const u64 lag = [] {
        const char* env = std::getenv("GOW3_REFLEX_PACER_LAG");
        return u64(std::clamp(env ? std::atoi(env) : 2, 1, 4));
    }();
    const s64 start = NowNs();
    const s64 last_present = g.last_present_ns.load(std::memory_order_relaxed);
    if (id <= lag || last_present == 0 || start - last_present > IdleNs) {
        return;
    }
    const u64 target = id - lag;
    const auto& slot = g.queued[target % g.queued.size()];
    if (slot.id.load(std::memory_order_acquire) != target) {
        return;
    }
    const u64 tick = slot.tick.load(std::memory_order_relaxed);
    const VkSemaphore semaphore = g.pacer_semaphore.load(std::memory_order_relaxed);
    const VkDevice raw_device = g.pacer_device.load(std::memory_order_relaxed);
    if (!semaphore || !raw_device) {
        return;
    }
    const vk::Device device{raw_device};
    const vk::Semaphore handle{semaphore};
    const vk::SemaphoreWaitInfo wait = {
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &tick,
    };
    const vk::Result result = device.waitSemaphores(wait, 50'000'000);
    if (result != vk::Result::eSuccess) {
        g.pacer_timeouts.fetch_add(1, std::memory_order_relaxed);
    }
    g.pacer_waits.fetch_add(1, std::memory_order_relaxed);
    g.pacer_wait_ns.fetch_add(u64(NowNs() - start), std::memory_order_relaxed);
}

/// Present thread, once per game frame: follows the setting and the toggle. The mode is applied
/// before `active` drops, so turning Reflex off reaches the driver although the guest paths already
/// do nothing.
void Update() {
    const int mode = EffectiveMode();
    const bool streamline = g.streamline.load(std::memory_order_relaxed);
    if (mode != g.applied_mode.load(std::memory_order_relaxed)) {
        if (streamline) {
            Gow3Streamline::SetReflexMode(mode);
            g.applied_mode.store(mode, std::memory_order_relaxed);
            std::printf("Reflex: mode %d through Streamline (%s)\n", mode,
                        mode == 0 ? "off" : mode == 1 ? "on" : "on + boost");
        } else {
            std::scoped_lock lock{g.mutex};
            if (g.swapchain) {
                ApplyLocked(mode);
            }
        }
    }
    // Streamline's Reflex guide (4.0, checklist): the markers and the sleep every frame, also with
    // Reflex off (PC Latency measures from them).
    const bool want = mode != 0 || g.log != nullptr || streamline;
    if (g.active.load(std::memory_order_relaxed) != want) {
        g.active.store(want, std::memory_order_relaxed);
    }
}

/// The last 5 s: sleeps done, their mean, timeouts, and how far the guest's flips are ahead of the
/// presenter's prepared frames. The two counters give a frame its id: the difference should stay at
/// the frames in flight (it can read -1 for a moment); a drift would mean an unpaired flip.
void PrintStats() {
    static u64 last_sleeps = 0, last_sleep_ns = 0, last_timeouts = 0;
    static u64 last_pacer_waits = 0, last_pacer_ns = 0, last_pacer_timeouts = 0;
    const u64 sleeps = g.sleeps.load(std::memory_order_relaxed);
    const u64 sleep_ns = g.sleep_ns.load(std::memory_order_relaxed);
    const u64 timeouts = g.timeouts.load(std::memory_order_relaxed);
    const u64 count = sleeps - last_sleeps;
    const double mean_ms = count ? double(sleep_ns - last_sleep_ns) / double(count) / 1e6 : 0.0;
    const long long lag = static_cast<long long>(g.guest_flips.load(std::memory_order_relaxed)) -
                          static_cast<long long>(g.prepared_flips.load(std::memory_order_relaxed));
    const u64 pacer_waits = g.pacer_waits.load(std::memory_order_relaxed);
    const u64 pacer_ns = g.pacer_wait_ns.load(std::memory_order_relaxed);
    const u64 pacer_timeouts = g.pacer_timeouts.load(std::memory_order_relaxed);
    const u64 pacer_count = pacer_waits - last_pacer_waits;
    const double pacer_ms =
        pacer_count ? double(pacer_ns - last_pacer_ns) / double(pacer_count) / 1e6 : 0.0;
    std::printf(
        "Reflex: mode=%d sleeps=%llu avg_sleep_ms=%.2f timeouts=%llu lag=%lld nosleep=%d variant=%d"
        " pacer=%d pacer_waits=%llu pacer_avg_ms=%.2f pacer_timeouts=%llu\n",
        EffectiveMode(), static_cast<unsigned long long>(count), mean_ms,
        static_cast<unsigned long long>(timeouts - last_timeouts), lag,
        Gow3Toggle::Disabled2(Gow3Toggle::ReflexNoSleep) ? 1 : 0, g.variant,
        PacerReplacesSleep() ? 1 : 0, static_cast<unsigned long long>(pacer_count), pacer_ms,
        static_cast<unsigned long long>(pacer_timeouts - last_pacer_timeouts));
    std::fflush(stdout);
    last_pacer_waits = pacer_waits;
    last_pacer_ns = pacer_ns;
    last_pacer_timeouts = pacer_timeouts;
    last_sleeps = sleeps;
    last_sleep_ns = sleep_ns;
    last_timeouts = timeouts;
}

/// GOW3_REFLEX_LOG and GOW3_REFLEX_VARIANT, once at Init or InitStreamline.
void OpenLogAndVariant() {
    if (const char* path = std::getenv("GOW3_REFLEX_LOG"); path && path[0]) {
        g.log = std::fopen(path, "w");
        if (g.log) {
            std::fprintf(g.log,
                         "presentID inputSampleTimeUs simStartTimeUs simEndTimeUs "
                         "renderSubmitStartTimeUs renderSubmitEndTimeUs presentStartTimeUs "
                         "presentEndTimeUs driverStartTimeUs driverEndTimeUs "
                         "osRenderQueueStartTimeUs osRenderQueueEndTimeUs gpuRenderStartTimeUs "
                         "gpuRenderEndTimeUs state\n");
            std::fflush(g.log);
        }
        std::printf("Reflex: driver timings %s %s\n", g.log ? "go to" : "cannot be written to",
                    path);
    }
    if (const char* v = std::getenv("GOW3_REFLEX_VARIANT"); v && (v[0] == '2' || v[0] == '3')) {
        g.variant = v[0] - '0'; // diagnostic (22 Oct 2026); anything else stays 0
        std::printf("Reflex: marker variant %d (diagnostic)\n", g.variant);
    }
}

} // namespace

bool ExtensionWanted() {
    const char* env = std::getenv("GOW3_REFLEX_EXT");
    return !(env && env[0] == '0');
}

void Init(vk::Device device, bool extension_enabled, u32 revision) {
    if (!ExtensionWanted()) {
        std::printf("Reflex: VK_NV_low_latency2 left off (GOW3_REFLEX_EXT=0)\n");
        return;
    }
    if (!extension_enabled) {
        std::printf("Reflex: VK_NV_low_latency2 not available\n");
        return;
    }
    const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
    if (!dispatch.vkSetLatencySleepModeNV || !dispatch.vkLatencySleepNV ||
        !dispatch.vkSetLatencyMarkerNV || !dispatch.vkGetLatencyTimingsNV) {
        std::printf("Reflex: VK_NV_low_latency2 revision %u, but the driver gave no functions\n",
                    revision);
        return;
    }
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    const auto [semaphore_result, semaphore] = device.createSemaphore(semaphore_chain.get());
    if (semaphore_result != vk::Result::eSuccess) {
        std::printf("Reflex: VK_NV_low_latency2 revision %u, but no timeline semaphore (%s)\n",
                    revision, vk::to_string(semaphore_result).c_str());
        return;
    }
    g.device = device;
    g.semaphore = semaphore;
    OpenLogAndVariant();
    g.available.store(true, std::memory_order_release);
    std::printf("Reflex: VK_NV_low_latency2 revision %u\n", revision);
}

void InitStreamline(bool ready) {
    if (!ready) {
        std::printf("Reflex: Streamline's sl.reflex / sl.pcl unavailable\n");
        return;
    }
    OpenLogAndVariant();
    g.streamline.store(true, std::memory_order_relaxed);
    std::printf("Reflex: through Streamline (sl.reflex, sl.pcl)\n");
}

void Shutdown(vk::Device device) {
    if (!device || device != g.device) {
        return;
    }
    g.available.store(false, std::memory_order_release);
    g.active.store(false, std::memory_order_relaxed);
    g.pacer_device.store(VK_NULL_HANDLE, std::memory_order_relaxed);
    {
        std::scoped_lock lock{g.mutex};
        g.swapchain = vk::SwapchainKHR{};
        device.destroySemaphore(g.semaphore);
        g.semaphore = vk::Semaphore{};
    }
    std::scoped_lock lock{g.report_mutex};
    if (g.log) {
        std::fclose(g.log);
        g.log = nullptr;
    }
    g.device = vk::Device{};
}

bool Available() {
    return g.available.load(std::memory_order_acquire);
}

bool Offered() {
    return Available() || g.streamline.load(std::memory_order_relaxed);
}

void OnSwapchain(vk::SwapchainKHR swapchain) {
    if (!Available()) {
        return;
    }
    std::scoped_lock lock{g.mutex};
    g.swapchain = swapchain;
    // A swapchain starts with the mode off.
    g.applied_mode.store(0, std::memory_order_relaxed);
    g.sleep_broken = false;
    if (!swapchain) {
        return;
    }
    // Mode 0: no call, the driver's default is off. The probe of 22 Oct 2026 (see MaxReports) got
    // the same markers and timings from a swapchain whose mode was never set as from one set to off.
    if (const int mode = EffectiveMode(); mode != 0) {
        ApplyLocked(mode);
    }
}

bool SleepActive() {
    return g.applied_mode.load(std::memory_order_relaxed) != 0 && EffectiveMode() != 0 &&
           !Gow3Toggle::Disabled2(Gow3Toggle::ReflexNoSleep) &&
           Gow3Toggle::Disabled2(Gow3Toggle::ReflexPacer); // 24 Oct 2026: false while the pacer sleeps
}

void FrameQueued(u64 id, vk::Device device, vk::Semaphore timeline, u64 tick) {
    if (!id) {
        return;
    }
    // 24 Oct 2026: the scheduler's work semaphore never changes; the latest is stored anyway.
    g.pacer_device.store(static_cast<VkDevice>(device), std::memory_order_relaxed);
    g.pacer_semaphore.store(static_cast<VkSemaphore>(timeline), std::memory_order_relaxed);
    auto& slot = g.queued[id % g.queued.size()];
    slot.tick.store(tick, std::memory_order_relaxed);
    slot.id.store(id, std::memory_order_release);
}

void InputSample() {
    if (!g.active.load(std::memory_order_relaxed)) {
        return;
    }
    // Pad reads come from more than one thread and several times a frame: the first one after the
    // flip request does the work. The load first: the common case is no atomic exchange.
    if (!g.first_read.load(std::memory_order_relaxed) ||
        !g.first_read.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    const bool reflex_on = EffectiveMode() != 0;
    const bool sleeps = reflex_on && !Gow3Toggle::Disabled2(Gow3Toggle::ReflexNoSleep);
    const bool pacer = PacerReplacesSleep();
    const u8 state = pacer ? 3 : sleeps ? 1 : reflex_on ? 2 : 0;
    const u64 id = g.guest_flips.load(std::memory_order_relaxed) + 1;
    const bool streamline = g.streamline.load(std::memory_order_relaxed);
    if (pacer) {
        PacerWait(id);
    // Streamline: slReflexSleep also with Reflex off (it returns at once then; Reflex guide 4.0).
    } else if (sleeps || (streamline && !Gow3Toggle::Disabled2(Gow3Toggle::ReflexNoSleep))) {
        Sleep(id);
    }
    g.frame_state[id % g.frame_state.size()].store(state, std::memory_order_relaxed);
    if (streamline) {
        Marker(id, vk::LatencyMarkerNV::eSimulationStart);
        Marker(id, vk::LatencyMarkerNV::eInputSample);
        return;
    }
    std::scoped_lock lock{g.mutex};
    if (g.swapchain) {
        MarkerLocked(id, vk::LatencyMarkerNV::eSimulationStart);
        MarkerLocked(id, vk::LatencyMarkerNV::eInputSample);
    }
}

void GuestSubmit() {
    if (!g.active.load(std::memory_order_relaxed)) {
        return;
    }
    if (g.variant != 0) {
        // Diagnostic variants (22 Oct 2026): variant 2 only starts the chain, with frame 1.
        if (g.variant == 2 && g.guest_flips.load(std::memory_order_relaxed) == 0 &&
            !g.chain_started.exchange(true, std::memory_order_relaxed)) {
            Marker(1, vk::LatencyMarkerNV::eRendersubmitStart);
        }
        return;
    }
    if (!g.first_submit.load(std::memory_order_relaxed) ||
        !g.first_submit.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    const u64 id = g.guest_flips.load(std::memory_order_relaxed) + 1;
    Marker(id, vk::LatencyMarkerNV::eRendersubmitStart);
}

void GuestFlipRequest() {
    // Counted while idle too, as PreparedFlip is: the ids must still agree when the mode is
    // switched on in the middle of a run.
    const u64 flips = g.guest_flips.fetch_add(1, std::memory_order_relaxed);
    if (g.log) {
        // The next frame's id is flips + 2: a defined state (off) even if InputSample never runs
        // for it; InputSample overwrites it. Before first_read is armed below.
        g.frame_state[(flips + 2) % g.frame_state.size()].store(0, std::memory_order_relaxed);
    }
    if (!g.active.load(std::memory_order_relaxed)) {
        return;
    }
    Marker(flips + 1, vk::LatencyMarkerNV::eSimulationEnd);
    g.first_read.store(true, std::memory_order_relaxed);
    g.first_submit.store(true, std::memory_order_relaxed);
}

u64 PreparedFlip() {
    return g.prepared_flips.fetch_add(1, std::memory_order_relaxed) + 1;
}

void RenderSubmitEnd(u64 serial) {
    if (g.active.load(std::memory_order_relaxed)) {
        if (g.variant == 3) { // diagnostic (22 Oct 2026): near-zero render-submit span
            Marker(serial, vk::LatencyMarkerNV::eRendersubmitStart);
        }
        Marker(serial, vk::LatencyMarkerNV::eRendersubmitEnd);
        if (g.variant == 2) { // diagnostic (22 Oct 2026): chain, next span starts as this one ends
            Marker(serial + 1, vk::LatencyMarkerNV::eRendersubmitStart);
        }
    }
}

void PresentStart(u64 serial) {
    if (!Available() && !g.streamline.load(std::memory_order_relaxed)) {
        return;
    }
    Update();
    if (g.active.load(std::memory_order_relaxed)) {
        Marker(serial, vk::LatencyMarkerNV::ePresentStart);
    }
}

void PresentEnd(u64 serial) {
    if (!g.active.load(std::memory_order_relaxed)) {
        return;
    }
    Marker(serial, vk::LatencyMarkerNV::ePresentEnd);
    const s64 now = NowNs();
    g.last_present_ns.store(now, std::memory_order_relaxed);
    // Present thread only from here on.
    static u64 frames = 0;
    static s64 stats_at = now;
    if (g.log && ++frames % ReportEveryFrames == 0) {
        Report();
    }
    if (now - stats_at >= StatsNs) {
        stats_at = now;
        PrintStats();
    }
}

void GeneratedPresentStart(u64 serial) {
    // Streamline presents its generated frames itself, with its own markers.
    if (g.active.load(std::memory_order_relaxed) && !g.streamline.load(std::memory_order_relaxed)) {
        Marker(serial, vk::LatencyMarkerNV::eOutOfBandPresentStart);
    }
}

void GeneratedPresentEnd(u64 serial) {
    if (g.active.load(std::memory_order_relaxed) && !g.streamline.load(std::memory_order_relaxed)) {
        Marker(serial, vk::LatencyMarkerNV::eOutOfBandPresentEnd);
    }
}

void Report() {
    std::scoped_lock report_lock{g.report_mutex};
    if (!g.log) {
        return;
    }
    std::array<vk::LatencyTimingsFrameReportNV, MaxReports> reports{};
    u32 count = 0;
    if (g.streamline.load(std::memory_order_relaxed)) {
        // The same driver reports through slReflexGetState (frame = the frame token, our id).
        std::array<Gow3Streamline::ReflexReport, MaxReports> sl_reports{};
        count = Gow3Streamline::ReflexReports(sl_reports.data(), MaxReports);
        for (u32 i = 0; i < count; ++i) {
            const auto& s = sl_reports[i];
            auto& r = reports[i];
            r.presentID = s.frame;
            r.inputSampleTimeUs = s.input_sample;
            r.simStartTimeUs = s.sim_start;
            r.simEndTimeUs = s.sim_end;
            r.renderSubmitStartTimeUs = s.render_submit_start;
            r.renderSubmitEndTimeUs = s.render_submit_end;
            r.presentStartTimeUs = s.present_start;
            r.presentEndTimeUs = s.present_end;
            r.driverStartTimeUs = s.driver_start;
            r.driverEndTimeUs = s.driver_end;
            r.osRenderQueueStartTimeUs = s.os_queue_start;
            r.osRenderQueueEndTimeUs = s.os_queue_end;
            r.gpuRenderStartTimeUs = s.gpu_render_start;
            r.gpuRenderEndTimeUs = s.gpu_render_end;
        }
    } else {
        std::scoped_lock lock{g.mutex};
        if (!g.swapchain) {
            return;
        }
        vk::GetLatencyMarkerInfoNV info = {};
        g.device.getLatencyTimingsNV(g.swapchain, &info); // without an array: how many it holds
        info.timingCount = std::min(info.timingCount, MaxReports);
        if (!info.timingCount) {
            return;
        }
        info.pTimings = reports.data();
        g.device.getLatencyTimingsNV(g.swapchain, &info);
        count = std::min(info.timingCount, MaxReports);
    }
    // Oldest first. A frame is written once it is final, never rewritten: the GPU has finished it,
    // or FramesInFlight newer ones exist (the driver left its GPU times at 0). The newest frames
    // wait for the next report.
    u32 final_count = count > FramesInFlight ? count - FramesInFlight : 0;
    for (u32 i = final_count; i < count; ++i) {
        if (reports[i].presentID && reports[i].gpuRenderEndTimeUs) {
            final_count = i + 1;
        }
    }
    for (u32 i = 0; i < final_count; ++i) {
        const auto& r = reports[i];
        if (!r.presentID || r.presentID <= g.last_logged) {
            continue;
        }
        std::fprintf(g.log,
                     "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %u\n",
                     static_cast<unsigned long long>(r.presentID),
                     static_cast<unsigned long long>(r.inputSampleTimeUs),
                     static_cast<unsigned long long>(r.simStartTimeUs),
                     static_cast<unsigned long long>(r.simEndTimeUs),
                     static_cast<unsigned long long>(r.renderSubmitStartTimeUs),
                     static_cast<unsigned long long>(r.renderSubmitEndTimeUs),
                     static_cast<unsigned long long>(r.presentStartTimeUs),
                     static_cast<unsigned long long>(r.presentEndTimeUs),
                     static_cast<unsigned long long>(r.driverStartTimeUs),
                     static_cast<unsigned long long>(r.driverEndTimeUs),
                     static_cast<unsigned long long>(r.osRenderQueueStartTimeUs),
                     static_cast<unsigned long long>(r.osRenderQueueEndTimeUs),
                     static_cast<unsigned long long>(r.gpuRenderStartTimeUs),
                     static_cast<unsigned long long>(r.gpuRenderEndTimeUs),
                     unsigned(g.frame_state[r.presentID % g.frame_state.size()].load(
                         std::memory_order_relaxed)));
        g.last_logged = r.presentID;
    }
    std::fflush(g.log);
}

} // namespace Gow3Reflex
