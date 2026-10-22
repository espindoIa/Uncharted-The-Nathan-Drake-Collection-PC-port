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
    return std::clamp(Gow3Settings::Get().reflex.load(std::memory_order_relaxed), 0, 2);
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
    std::scoped_lock lock{g.mutex};
    if (g.swapchain) {
        MarkerLocked(id, marker);
    }
}

/// The driver paces the game here: vkLatencySleepNV, then the semaphore it signals when the game
/// should start sampling input for its next frame.
void Sleep() {
    const s64 start = NowNs();
    const s64 last_present = g.last_present_ns.load(std::memory_order_relaxed);
    if (last_present == 0 || start - last_present > IdleNs ||
        start < g.skip_sleep_until_ns.load(std::memory_order_relaxed)) {
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

/// Present thread, once per game frame: follows the setting and the toggle. The mode is applied
/// before `active` drops, so turning Reflex off reaches the driver although the guest paths already
/// do nothing.
void Update() {
    const int mode = EffectiveMode();
    if (mode != g.applied_mode.load(std::memory_order_relaxed)) {
        std::scoped_lock lock{g.mutex};
        if (g.swapchain) {
            ApplyLocked(mode);
        }
    }
    const bool want = mode != 0 || g.log != nullptr;
    if (g.active.load(std::memory_order_relaxed) != want) {
        g.active.store(want, std::memory_order_relaxed);
    }
}

/// The last 5 s: sleeps done, their mean, timeouts, and how far the guest's flips are ahead of the
/// presenter's prepared frames. The two counters give a frame its id: the difference should stay at
/// the frames in flight (it can read -1 for a moment); a drift would mean an unpaired flip.
void PrintStats() {
    static u64 last_sleeps = 0, last_sleep_ns = 0, last_timeouts = 0;
    const u64 sleeps = g.sleeps.load(std::memory_order_relaxed);
    const u64 sleep_ns = g.sleep_ns.load(std::memory_order_relaxed);
    const u64 timeouts = g.timeouts.load(std::memory_order_relaxed);
    const u64 count = sleeps - last_sleeps;
    const double mean_ms = count ? double(sleep_ns - last_sleep_ns) / double(count) / 1e6 : 0.0;
    const long long lag = static_cast<long long>(g.guest_flips.load(std::memory_order_relaxed)) -
                          static_cast<long long>(g.prepared_flips.load(std::memory_order_relaxed));
    std::printf("Reflex: mode=%d sleeps=%llu avg_sleep_ms=%.2f timeouts=%llu lag=%lld\n",
                EffectiveMode(), static_cast<unsigned long long>(count), mean_ms,
                static_cast<unsigned long long>(timeouts - last_timeouts), lag);
    std::fflush(stdout);
    last_sleeps = sleeps;
    last_sleep_ns = sleep_ns;
    last_timeouts = timeouts;
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
    if (const char* path = std::getenv("GOW3_REFLEX_LOG"); path && path[0]) {
        g.log = std::fopen(path, "w");
        if (g.log) {
            std::fprintf(g.log,
                         "presentID inputSampleTimeUs simStartTimeUs simEndTimeUs "
                         "renderSubmitStartTimeUs renderSubmitEndTimeUs presentStartTimeUs "
                         "presentEndTimeUs driverStartTimeUs driverEndTimeUs "
                         "osRenderQueueStartTimeUs osRenderQueueEndTimeUs gpuRenderStartTimeUs "
                         "gpuRenderEndTimeUs\n");
            std::fflush(g.log);
        }
        std::printf("Reflex: driver timings %s %s\n", g.log ? "go to" : "cannot be written to",
                    path);
    }
    g.available.store(true, std::memory_order_release);
    std::printf("Reflex: VK_NV_low_latency2 revision %u\n", revision);
}

void Shutdown(vk::Device device) {
    if (!device || device != g.device) {
        return;
    }
    g.available.store(false, std::memory_order_release);
    g.active.store(false, std::memory_order_relaxed);
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
    if (EffectiveMode() != 0) {
        Sleep();
    }
    const u64 id = g.guest_flips.load(std::memory_order_relaxed) + 1;
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
        Marker(serial, vk::LatencyMarkerNV::eRendersubmitEnd);
    }
}

void PresentStart(u64 serial) {
    if (!Available()) {
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

void Report() {
    std::scoped_lock report_lock{g.report_mutex};
    if (!g.log) {
        return;
    }
    std::array<vk::LatencyTimingsFrameReportNV, MaxReports> reports{};
    u32 count = 0;
    {
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
                     "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
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
                     static_cast<unsigned long long>(r.gpuRenderEndTimeUs));
        g.last_logged = r.presentID;
    }
    std::fflush(g.log);
}

} // namespace Gow3Reflex
