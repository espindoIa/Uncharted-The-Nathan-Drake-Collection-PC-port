// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 23 Oct 2026: GOW3_PIPE_STATS writer (see gow3_pipe_stats.h, schema v1).
#include "gow3_pipe_stats.h"

#if GOW3_PIPE_STATS_BUILD
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include "common/thread.h"

#ifndef GOW3_GIT_REV
#define GOW3_GIT_REV "unknown"
#endif

namespace Gow3PipeStats {
namespace {

using Clock = std::chrono::steady_clock;

/// A raw counter copied at each flip; the writer turns consecutive copies into a delta.
struct Source {
    const char* name; ///< column name
    const Single* counter;
    bool cycles;      ///< TSC cycles, written in ns
};

const Source sources[] = {
    {"a_idle_block_ns", &stage_a.idle_block_ns, false},
    {"a_idle_poll_ns", &stage_a.idle_poll_ns, false},
    {"a_drains", &stage_a.drains, false},
    {"a_drain_spin_ns", &stage_a.drain_spin_cyc, true},
    {"a_drain_yield_ns", &stage_a.drain_yield_cyc, true},
    {"a_drain_cmd_ns", &stage_a.drain_cmd_cyc, true},
    {"a_drain_pm4_ns", &stage_a.drain_pm4_cyc, true},
    {"a_drain_sub_ns", &stage_a.drain_sub_cyc, true},
    {"a_drain_other_ns", &stage_a.drain_other_cyc, true},
    {"a_space_waits", &stage_a.space_waits, false},
    {"a_space_spin_ns", &stage_a.space_spin_cyc, true},
    {"a_packets", &stage_a.packets, false},
    {"a_bytes", &stage_a.bytes, false},
    {"a_wake_notifies", &stage_a.wake_notifies, false},
    {"a_occ_samples", &stage_a.occ_samples, false},
    {"a_occ_0", &stage_a.occ[0], false},
    {"a_occ_lt1k", &stage_a.occ[1], false},
    {"a_occ_lt4k", &stage_a.occ[2], false},
    {"a_occ_lt16k", &stage_a.occ[3], false},
    {"a_occ_lt64k", &stage_a.occ[4], false},
    {"a_occ_lt256k", &stage_a.occ[5], false},
    {"a_occ_lt1m", &stage_a.occ[6], false},
    {"a_occ_lt4m", &stage_a.occ[7], false},
    {"a_occ_ge4m", &stage_a.occ[8], false},
    {"b_packets", &stage_b.packets, false},
    {"b_busy_ns", &stage_b.busy_cyc, true},
    {"b_wait_spin_ns", &stage_b.wait_spin_cyc, true},
    {"b_wait_block_ns", &stage_b.wait_block_cyc, true},
    {"b_sleeps", &stage_b.sleeps, false},
};
constexpr int NumSources = sizeof(sources) / sizeof(sources[0]);
enum : int { SrcDrainSpin = 3, SrcDrainYield = 4, SrcSpaceSpin = 10 };

/// Reserved for VULCAN (recorder and submission, docs/team/PIPE_STATS_SCHEMA_v1.md): written
/// empty so the column set stays the same when they are filled in.
const char* const reserved[] = {"rec_busy_ns", "rec_wait_ns", "rec_draws", "rec_pipeline_miss",
                                "rec_desc_writes", "rec_barriers", "sub_count", "sub_ns",
                                "fence_wait_ns"};

struct Row {
    u64 frame_id, t_ns, tsc, a_frame_id, toggles1, toggles2, dropped, b_max_cyc;
    u64 values[NumSources];
};

constexpr u64 RingSize = 1024; ///< ~10 s at 100 fps; the writer empties it every 100 ms

struct State {
    std::FILE* file = nullptr;
    Row ring[RingSize];
    alignas(64) std::atomic<u64> write{0}; ///< flip thread
    alignas(64) std::atomic<u64> read{0};  ///< writer thread
    alignas(64) std::atomic<bool> flipping{false};
    std::atomic<u64> dropped{0};
    std::atomic<u64> written{0};
    std::atomic<bool> stop{false};
    std::mutex mutex; ///< writer sleep only, never taken on the flip path
    std::condition_variable cv;
    std::thread thread;
    // Writer-thread state.
    Row previous{};
    bool have_previous = false;
    Clock::time_point last_flush{};
    u64 first_t_ns = 0, first_tsc = 0;
    double tsc_per_ns = 0.0;
};

/// 0 not started, 1 running, 2 closed or failed. The State is never freed: a flip racing the
/// shutdown (static destructors, other threads still running) only touches live memory.
std::atomic<int> phase{0};
std::atomic<State*> state{nullptr};
u64 frame_counter = 0; ///< flip thread only (guarded by State::flipping)
std::atomic<u64> frame_published{0};
std::atomic<u64> dropped_before_start{0};
std::once_flag shutdown_once;

u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

void WriteHeader(State& s, double tsc_hz) {
    std::fprintf(s.file,
                 "# gow3_pipe_stats schema=1 build=%s toggles=%llu,%llu tsc_hz=%.0f "
                 "threads=%u occ_sample=1/%d\n",
                 GOW3_GIT_REV,
                 static_cast<unsigned long long>(
                     __atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED)),
                 static_cast<unsigned long long>(
                     __atomic_load_n(&runtime_disabled_optimizations2, __ATOMIC_RELAXED)),
                 tsc_hz, std::thread::hardware_concurrency(), OccSampleMask + 1);
    std::fputs("frame_id,t_flip_ns,a_frame_id,toggles1,toggles2,stats_paused,dropped,"
               "a_wait_spin_ns,a_wait_yield_ns",
               s.file);
    for (const Source& source : sources) {
        std::fprintf(s.file, ",%s", source.name);
    }
    std::fputs(",b_max_packet_ns", s.file);
    for (const char* name : reserved) {
        std::fprintf(s.file, ",%s", name);
    }
    std::fputc('\n', s.file);
}

u64 ToNs(const State& s, u64 cycles) {
    return s.tsc_per_ns > 0.0 ? static_cast<u64>(static_cast<double>(cycles) / s.tsc_per_ns) : 0;
}

void WriteRow(State& s, const Row& row) {
    if (!s.have_previous) {
        // The first snapshot is the baseline: counters before it belong to no frame.
        s.previous = row;
        s.have_previous = true;
        s.first_t_ns = row.t_ns;
        s.first_tsc = row.tsc;
        return;
    }
    // Cycles per ns over the whole run so far (invariant TSC); the startup calibration is
    // used until a second has passed.
    if (row.t_ns - s.first_t_ns > 1000000000ull && row.tsc > s.first_tsc) {
        s.tsc_per_ns = static_cast<double>(row.tsc - s.first_tsc) /
                       static_cast<double>(row.t_ns - s.first_t_ns);
    }
    u64 delta[NumSources];
    for (int i = 0; i < NumSources; ++i) {
        delta[i] = row.values[i] - s.previous.values[i];
    }
    const bool paused = (row.toggles2 & Gow3Toggle::PipeStats) != 0;
    std::fprintf(s.file, "%llu,%llu,%llu,%llu,%llu,%d,%llu,%llu,%llu",
                 static_cast<unsigned long long>(row.frame_id),
                 static_cast<unsigned long long>(row.t_ns),
                 static_cast<unsigned long long>(row.a_frame_id),
                 static_cast<unsigned long long>(row.toggles1),
                 static_cast<unsigned long long>(row.toggles2), paused ? 1 : 0,
                 static_cast<unsigned long long>(row.dropped),
                 static_cast<unsigned long long>(
                     ToNs(s, delta[SrcDrainSpin] + delta[SrcSpaceSpin])),
                 static_cast<unsigned long long>(ToNs(s, delta[SrcDrainYield])));
    for (int i = 0; i < NumSources; ++i) {
        const u64 value = sources[i].cycles ? ToNs(s, delta[i]) : delta[i];
        std::fprintf(s.file, ",%llu", static_cast<unsigned long long>(value));
    }
    std::fprintf(s.file, ",%llu", static_cast<unsigned long long>(ToNs(s, row.b_max_cyc)));
    for (std::size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        std::fputc(',', s.file);
    }
    std::fputc('\n', s.file);
    s.previous = row;
    s.written.fetch_add(1, std::memory_order_relaxed);
}

/// Writer thread: empties the ring. Returns true if it wrote anything.
bool DrainRing(State& s) {
    const u64 end = s.write.load(std::memory_order_acquire);
    u64 begin = s.read.load(std::memory_order_relaxed);
    if (begin == end) {
        return false;
    }
    for (; begin != end; ++begin) {
        WriteRow(s, s.ring[begin % RingSize]);
        // Release: the flip thread may reuse the slot once `read` passes it.
        s.read.store(begin + 1, std::memory_order_release);
    }
    return true;
}

void WriterMain(State* s) {
    Common::SetCurrentThreadName("gow3:PipeStats");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    // Startup TSC calibration over 50 ms (replaced by the whole-run ratio after 1 s).
    const u64 t0 = NowNs(), c0 = Cycles();
    {
        std::unique_lock lock{s->mutex};
        s->cv.wait_for(lock, std::chrono::milliseconds(50),
                       [s] { return s->stop.load(std::memory_order_relaxed); });
    }
    const u64 t1 = NowNs(), c1 = Cycles();
    s->tsc_per_ns = t1 > t0 ? static_cast<double>(c1 - c0) / static_cast<double>(t1 - t0) : 0.0;
    WriteHeader(*s, s->tsc_per_ns * 1e9);
    std::fflush(s->file);
    s->last_flush = Clock::now();
    while (!s->stop.load(std::memory_order_acquire)) {
        DrainRing(*s);
        if (Clock::now() - s->last_flush >= std::chrono::seconds(1)) {
            std::fflush(s->file);
            s->last_flush = Clock::now();
        }
        std::unique_lock lock{s->mutex};
        s->cv.wait_for(lock, std::chrono::milliseconds(100),
                       [s] { return s->stop.load(std::memory_order_relaxed); });
    }
    DrainRing(*s);
    std::fflush(s->file);
}

bool Start() {
    const char* path = std::getenv("GOW3_PIPE_STATS");
    State* s = new State; // once, on the first flip; never freed (see `phase`)
    s->file = path ? std::fopen(path, "w") : nullptr;
    if (!s->file) {
        std::fprintf(stderr, "gow3: GOW3_PIPE_STATS: cannot open '%s'\n", path ? path : "");
        delete s;
        return false;
    }
    std::setvbuf(s->file, nullptr, _IOFBF, 1 << 16);
    state.store(s, std::memory_order_release);
    s->thread = std::thread(WriterMain, s);
    std::atexit(Shutdown);
    return true;
}

} // namespace

bool Requested() noexcept {
    const char* path = std::getenv("GOW3_PIPE_STATS");
    return path && *path;
}

u64 FrameId() noexcept {
    return frame_published.load(std::memory_order_relaxed);
}

void OnFlip(u64 a_frame_id) noexcept {
    if (!enabled) {
        return;
    }
    int current = phase.load(std::memory_order_acquire);
    if (current == 0) {
        // Only the flip thread gets here; a second thread would see `flipping` below.
        int expected = 0;
        if (!phase.compare_exchange_strong(expected, 3, std::memory_order_acq_rel)) {
            dropped_before_start.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        bool started = false;
        try {
            started = Start();
        } catch (...) {
            started = false;
        }
        phase.store(started ? 1 : 2, std::memory_order_release);
        current = started ? 1 : 2;
    }
    if (current != 1) {
        return;
    }
    State& s = *state.load(std::memory_order_acquire);
    if (s.flipping.exchange(true, std::memory_order_acquire)) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 frame = ++frame_counter;
    frame_published.store(frame, std::memory_order_relaxed);
    const u64 w = s.write.load(std::memory_order_relaxed);
    if (w - s.read.load(std::memory_order_acquire) >= RingSize) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        Row& row = s.ring[w % RingSize];
        row.frame_id = frame;
        row.t_ns = NowNs();
        row.tsc = Cycles();
        row.a_frame_id = a_frame_id;
        row.toggles1 = __atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED);
        row.toggles2 = __atomic_load_n(&runtime_disabled_optimizations2, __ATOMIC_RELAXED);
        row.dropped = s.dropped.load(std::memory_order_relaxed) +
                      dropped_before_start.load(std::memory_order_relaxed);
        for (int i = 0; i < NumSources; ++i) {
            row.values[i] = sources[i].counter->Load();
        }
        const u64 epoch = frame_epoch.load(std::memory_order_relaxed);
        const u64 packed = stage_b.max_packet.load(std::memory_order_relaxed);
        row.b_max_cyc = (packed >> 48) == (epoch & 0xffff) ? packed & ((1ull << 48) - 1) : 0;
        s.write.store(w + 1, std::memory_order_release);
    }
    // Stage B starts a new per-frame maximum from here.
    frame_epoch.store(frame_epoch.load(std::memory_order_relaxed) + 1,
                      std::memory_order_relaxed);
    s.flipping.store(false, std::memory_order_release);
}

void Shutdown() noexcept {
    std::call_once(shutdown_once, [] {
        int expected = 1;
        if (!phase.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
            return; // never started, failed, or still starting (left to process exit)
        }
        State& s = *state.load(std::memory_order_acquire);
        {
            std::lock_guard lock{s.mutex};
            s.stop.store(true, std::memory_order_release);
        }
        s.cv.notify_all();
        if (s.thread.joinable()) {
            s.thread.join();
        }
        std::fclose(s.file);
        s.file = nullptr;
    });
}

u64 RowsWritten() noexcept {
    const State* s = state.load(std::memory_order_acquire);
    return s ? s->written.load(std::memory_order_relaxed) : 0;
}

u64 RowsDropped() noexcept {
    const State* s = state.load(std::memory_order_acquire);
    return (s ? s->dropped.load(std::memory_order_relaxed) : 0) +
           dropped_before_start.load(std::memory_order_relaxed);
}

} // namespace Gow3PipeStats
#endif
