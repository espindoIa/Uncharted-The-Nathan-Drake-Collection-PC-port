// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: two-stage draw pipeline (docs/parallel_gpu.md, "Two-stage draw pipeline").
//
// The GPU command thread (stage A) decodes PM4, keeps the register file and selects pipelines;
// for a direct draw it writes a packet (the register blocks changed since the previous packet,
// the stages' user data, the draw parameters) into this ring and goes on decoding. The draw
// recording thread (stage B) owns the caches, the barriers and the command recording while
// packets are in flight: it applies each packet to its own copy of the registers and runs the
// rest of the draw. Anything else stage A does on that state waits for stage B to run dry
// (Drain) and then runs on stage A as before.

#pragma once

#include <cstdlib>
#include "gow3_threads.h"
#include "gow3_profiler.h"
#include "gow3_toggles.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <x86intrin.h>

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"
#include "core/platform.h"

namespace Vulkan {

/// GOW3_DRAWREC_STATS: DrawPreparation::Build counters (vk_draw_prep.cpp), cumulative, relaxed,
/// read by the DrawPipe report. Written only with the gate on.
struct DrawPipeSubmissionStats {
    std::atomic<u64> submissions{0}, new_allocations{0}, pool_reuses{0};
};
inline DrawPipeSubmissionStats draw_pipe_submission_stats;

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);
    /// Stage B: records the fences the packets just run left batched (GOW3_COALESCE_FENCES).
    using FlushHandler = void (*)(void* context);

    /// Packet kinds Commit() is told about (order of PacketKind in vk_rasterizer.cpp).
    static constexpr u32 NumKinds = 6;

    /// GOW3_DRAWREC_STATS=1: opt-in report of this queue (stage A -> stage B) every ~5 s. It only
    /// observes: no counter takes part in the queue's control flow. Gate off = one predictable
    /// branch per site, no timestamps, increments, publications or extra loads of `consumed`.
    [[nodiscard]] static bool StatsEnabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("GOW3_DRAWREC_STATS");
            return env && env[0] == '1';
        }();
        return enabled;
    }

    DrawPipe(Handler handler_, void* context_, FlushHandler flush_handler_ = nullptr,
             void* flush_context_ = nullptr)
        : handler{handler_}, context{context_}, flush_handler{flush_handler_},
          flush_context{flush_context_} {
        ring = std::make_unique<u8[]>(Capacity);
        if (stats) {
            stats_a.window = std::chrono::steady_clock::now();
            stats_a.window_tsc = __rdtsc();
            stats_a.last_frames = Gow3Stats::gpu_frames.load(std::memory_order_relaxed);
        }
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~DrawPipe() {
        Drain();
        thread.request_stop();
        published.fetch_add(0, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the draw recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return on_stage_b;
    }

    [[nodiscard]] u32 StageBThreadId() const noexcept {
        return stage_b_tid.load(std::memory_order_acquire);
    }

    /// Stage A: space for a packet of `size` bytes (8-byte aligned), valid until Commit().
    u8* Begin(u32 size) {
        if (stats) {
            stats_a.begin_tsc = __rdtsc();
            stats_a.wait_in_packet = 0;
            stats_a.request = size;
        }
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: the rest of the ring is skipped (a header with zero size marks it).
            WaitForSpace(at, Capacity - offset);
            reinterpret_cast<Header*>(ring.get() + offset)->size = 0;
            if (stats) {
                stats_a.cum.wrap_bytes += Capacity - offset;
            }
            at += Capacity - offset;
            head = at;
            published.store(at, std::memory_order_release);
        }
        WaitForSpace(at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    /// `kind` (index of PacketKind, default Draw) only feeds GOW3_DRAWREC_STATS.
    void Commit(u32 payload_size, u32 kind = 0) {
        reinterpret_cast<Header*>(ring.get() + head % Capacity)->payload = payload_size;
        head += pending_size;
        published.store(head, std::memory_order_seq_cst);
        ++packets;
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
            if (stats) {
                ++stats_a.cum.notify_calls;
            }
        }
        if (stats) {
            CommitStats(payload_size, kind);
        }
    }

    /// Why stage A drained (statistics): a PM4 opcode, or one of these.
    enum Reason : u32 {
        ReasonConstRam = 256,
        ReasonCommands,
        ReasonCompute,
        ReasonSubmissionEnd,
        ReasonDraw,
        ReasonRasterizer,
        NumReasons,
    };
    std::array<u64, NumReasons> drains_by_reason{};
    std::array<u64, NumReasons> cycles_by_reason{};

    /// Stage A: waits until stage B has run every committed packet.
    void Drain(u32 reason = ReasonRasterizer) {
        if (consumed.load(std::memory_order_acquire) == head) {
            if (stats) {
                ++stats_a.cum.drain_empty;
            }
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = __rdtsc();
        for (u32 spins = 0; consumed.load(std::memory_order_acquire) != head; ++spins) {
            if (spins < 4096) {
                __builtin_ia32_pause();
            } else {
                std::this_thread::yield();
            }
        }
        const u64 waited = __rdtsc() - start;
        drain_cycles += waited;
        cycles_by_reason[slot] += waited;
        if (stats) {
            // End of a drain that waited, matched with the next Commit (CommitStats).
            const u64 end = start + waited;
            if (stats_a.post_open++ == 0) {
                stats_a.post_oldest = end;
            }
            stats_a.post_ends += end;
        }
    }

    /// Stage A: position after the last committed packet; Reached(position) once B ran it.
    [[nodiscard]] u64 Head() const noexcept {
        return head;
    }
    [[nodiscard]] bool Reached(u64 position) const noexcept {
        return consumed.load(std::memory_order_acquire) >= position;
    }

    [[nodiscard]] bool Idle() const noexcept {
        return consumed.load(std::memory_order_acquire) == head;
    }

    /// Statistics (stage A): packets, drains that had to wait, cycles waited, cycles stage B
    /// spent running packets.
    u64 packets = 0, drains = 0, drain_cycles = 0;
    std::atomic<u64> busy_cycles{0};

private:
    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
    };
    static constexpr u64 Capacity = 16ull << 20;

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    void WaitForSpace(u64 at, u64 size) {
        if (stats) {
            WaitForSpaceCounted(at, size);
            return;
        }
        while (at + size - consumed.load(std::memory_order_acquire) > Capacity) {
            __builtin_ia32_pause();
        }
    }

    // ---- GOW3_DRAWREC_STATS (diagnostic only; nothing below feeds the queue's control flow) ----
    //
    // Time unit: TSC ticks (__rdtsc, as Drain and busy_cycles already use), not ordered with the
    // surrounding code and not cycles executed or CPU utilisation: an interval can include
    // preemption. Microseconds = ticks / (ticks_per_us), the ratio of the report window's own TSC
    // delta to its steady_clock delta. Percentages are "% of the window's elapsed TSC ticks"
    // unless the key says otherwise.
    //
    // Cumulative counters are never reset; the report prints deltas against its own snapshots.
    // Stage B's counters are independent relaxed atomics, so a report is not an atomic snapshot
    // of A's cursors and B's counters: they can lag each other by a packet or an interval.

    /// Cumulative values owned by stage A (plain members: the GPU command thread is the only
    /// producer, Begin/Commit/Drain run there, and so does the report).
    struct Cum {
        u64 packets = 0, head = 0, drains = 0, drain_empty = 0, drain_cycles = 0;
        // Exclusive byte categories; their sum is the physical advance of `head`.
        // payload = Commit's argument (includes the producer's own 8-byte alignment), header = 8 B
        // per real packet, slack = reserved in Begin minus payload, pad64 = rounding to 64 B,
        // wrap = ring end skipped by a wrap marker (not a work packet).
        u64 payload_bytes = 0, header_bytes = 0, slack_bytes = 0, pad_bytes = 0, wrap_bytes = 0;
        std::array<u64, NumKinds> kind_packets{}, kind_payload{};
        // Queue depth (head - consumed, bytes) sampled after each Commit.
        u64 depth_sum = 0, depth_samples = 0;
        std::array<u64, 6> depth_coarse{}, depth_fine{};
        // Begin->Commit minus the WaitForSpace time inside it; WaitForSpace calls that had to
        // wait (entries), their polling turns (counted locally) and ticks.
        u64 write_ticks = 0, wait_entries = 0, wait_spins = 0, wait_ticks = 0;
        u64 notify_calls = 0;
        // Per drain that waited: ticks from its end to the next Commit (summed, one observation
        // each). Consecutive drains give overlapping intervals: not exclusive time.
        u64 post_sum = 0, post_n = 0;
    };

    /// Stage B's values (published in batches, relaxed). Max values are "since start".
    struct BVals {
        u64 packets = 0, sleep_attempts = 0, wait_returns = 0, wait_ticks = 0, idle_ticks = 0,
            idle_episodes = 0, max_wait_ticks = 0, max_idle_ticks = 0;
    };
    struct BShared {
        std::atomic<u64> packets{0}, sleep_attempts{0}, wait_returns{0}, wait_ticks{0},
            idle_ticks{0}, idle_episodes{0}, max_wait_ticks{0}, max_idle_ticks{0};
    };

    struct StatsA {
        Cum cum, last;
        BVals last_b;
        u64 last_sub_n = 0, last_sub_new = 0, last_sub_reuse = 0, last_frames = 0;
        u64 last_check_packets = 0;
        u64 begin_tsc = 0, wait_in_packet = 0, request = 0;
        u64 post_open = 0, post_ends = 0, post_oldest = 0;
        u64 depth_max = 0, post_max = 0; ///< per window, reset by the report
        std::chrono::steady_clock::time_point window;
        u64 window_tsc = 0;
    };

    void WaitForSpaceCounted(u64 at, u64 size) {
        if (at + size - consumed.load(std::memory_order_acquire) <= Capacity) {
            return;
        }
        const u64 start = __rdtsc();
        u64 spins = 0;
        while (at + size - consumed.load(std::memory_order_acquire) > Capacity) {
            __builtin_ia32_pause();
            ++spins;
        }
        const u64 waited = __rdtsc() - start;
        ++stats_a.cum.wait_entries;
        stats_a.cum.wait_spins += spins;
        stats_a.cum.wait_ticks += waited;
        stats_a.wait_in_packet += waited;
    }

    /// Stage A, after the sleep protocol of Commit.
    void CommitStats(u32 payload, u32 kind) {
        auto& a = stats_a;
        auto& c = a.cum;
        const u64 now = __rdtsc();
        c.payload_bytes += payload;
        c.header_bytes += sizeof(Header);
        c.slack_bytes += a.request - payload;
        c.pad_bytes += pending_size - sizeof(Header) - a.request;
        const u32 k = kind < NumKinds ? kind : 0;
        ++c.kind_packets[k];
        c.kind_payload[k] += payload;
        const u64 elapsed = now - a.begin_tsc;
        c.write_ticks += elapsed > a.wait_in_packet ? elapsed - a.wait_in_packet : 0;
        if (a.post_open) {
            c.post_sum += a.post_open * now - a.post_ends;
            c.post_n += a.post_open;
            a.post_max = std::max(a.post_max, now - a.post_oldest);
            a.post_open = a.post_ends = 0;
        }
        // Samples after commits only: not the time distribution of the occupancy.
        const u64 depth = head - consumed.load(std::memory_order_acquire);
        c.depth_sum += depth;
        ++c.depth_samples;
        a.depth_max = std::max(a.depth_max, depth);
        ++c.depth_coarse[depth == 0                ? 0
                         : depth == Capacity       ? 5
                         : depth * 4 <= Capacity   ? 1
                         : depth * 2 <= Capacity   ? 2
                         : depth * 4 <= 3 * Capacity ? 3
                                                     : 4];
        ++c.depth_fine[depth == 0                 ? 0
                       : depth <= (4ull << 10)    ? 1
                       : depth <= (64ull << 10)   ? 2
                       : depth <= (1ull << 20)    ? 3
                       : depth <= (8ull << 20)    ? 4
                                                  : 5];
        // At least 1024 packets since the last check, then 5 s of steady_clock; a report comes at
        // the next Commit after that (no timer, thread or wake of its own).
        if (packets - a.last_check_packets >= 1024) {
            a.last_check_packets = packets;
            ReportStats();
        }
    }

    BVals LoadB() const {
        BVals v;
        v.packets = stats_b.packets.load(std::memory_order_relaxed);
        v.sleep_attempts = stats_b.sleep_attempts.load(std::memory_order_relaxed);
        v.wait_returns = stats_b.wait_returns.load(std::memory_order_relaxed);
        v.wait_ticks = stats_b.wait_ticks.load(std::memory_order_relaxed);
        v.idle_ticks = stats_b.idle_ticks.load(std::memory_order_relaxed);
        v.idle_episodes = stats_b.idle_episodes.load(std::memory_order_relaxed);
        v.max_wait_ticks = stats_b.max_wait_ticks.load(std::memory_order_relaxed);
        v.max_idle_ticks = stats_b.max_idle_ticks.load(std::memory_order_relaxed);
        return v;
    }

    void PublishB(const BVals& v) {
        stats_b.packets.store(v.packets, std::memory_order_relaxed);
        stats_b.sleep_attempts.store(v.sleep_attempts, std::memory_order_relaxed);
        stats_b.wait_returns.store(v.wait_returns, std::memory_order_relaxed);
        stats_b.wait_ticks.store(v.wait_ticks, std::memory_order_relaxed);
        stats_b.idle_ticks.store(v.idle_ticks, std::memory_order_relaxed);
        stats_b.idle_episodes.store(v.idle_episodes, std::memory_order_relaxed);
        stats_b.max_wait_ticks.store(v.max_wait_ticks, std::memory_order_relaxed);
        stats_b.max_idle_ticks.store(v.max_idle_ticks, std::memory_order_relaxed);
    }

    /// Stage A. Two stable key=value lines (A side, then B side and Submission pool), values of
    /// the window since the previous report.
    void ReportStats() {
        auto& a = stats_a;
        const auto wall = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(wall - a.window).count();
        if (seconds < 5.0) {
            return;
        }
        using ull = unsigned long long;
        const u64 tsc = __rdtsc();
        Cum n = a.cum;
        n.packets = packets;
        n.head = head;
        n.drains = drains;
        n.drain_cycles = drain_cycles;
        const Cum& l = a.last;
        const BVals b = LoadB();
        const BVals& lb = a.last_b;
        const auto& sub = draw_pipe_submission_stats;
        const u64 sub_n = sub.submissions.load(std::memory_order_relaxed);
        const u64 sub_new = sub.new_allocations.load(std::memory_order_relaxed);
        const u64 sub_reuse = sub.pool_reuses.load(std::memory_order_relaxed);
        const u64 frames = Gow3Stats::gpu_frames.load(std::memory_order_relaxed);
        const auto D = [&](u64 Cum::*m) { return n.*m - l.*m; };

        const double ticks = double(tsc - a.window_tsc);
        const double tpu = ticks / (seconds * 1e6); // TSC ticks per microsecond
        const u64 d_packets = D(&Cum::packets);
        const u64 d_frames = frames - a.last_frames;
        const u64 phys = D(&Cum::head);
        const u64 sum = D(&Cum::payload_bytes) + D(&Cum::header_bytes) + D(&Cum::slack_bytes) +
                        D(&Cum::pad_bytes) + D(&Cum::wrap_bytes);
        const double pct_phys = phys ? 100.0 / double(phys) : 0.0;
        const u64 samples = D(&Cum::depth_samples);
        const double per_commit = samples ? 100.0 / double(samples) : 0.0;

        std::printf("DrawRec A: window_s=%.2f window_tsc=%llu ticks_per_us=%.1f packets_s=%.0f "
                    "frames=%llu packets_per_frame=%.1f "
                    "bytes_s{payload=%.0f header=%.0f slack=%.0f pad64=%.0f wrap=%.0f phys=%.0f} "
                    "bytes_pct_of_phys{payload=%.1f header=%.1f slack=%.1f pad64=%.1f wrap=%.1f} "
                    "bytes_residue=%lld",
                    seconds, ull(tsc - a.window_tsc), tpu, d_packets / seconds, ull(d_frames),
                    d_frames ? double(d_packets) / double(d_frames) : 0.0,
                    D(&Cum::payload_bytes) / seconds, D(&Cum::header_bytes) / seconds,
                    D(&Cum::slack_bytes) / seconds, D(&Cum::pad_bytes) / seconds,
                    D(&Cum::wrap_bytes) / seconds, phys / seconds,
                    D(&Cum::payload_bytes) * pct_phys, D(&Cum::header_bytes) * pct_phys,
                    D(&Cum::slack_bytes) * pct_phys, D(&Cum::pad_bytes) * pct_phys,
                    D(&Cum::wrap_bytes) * pct_phys, static_cast<long long>(phys - sum));
        static constexpr const char* kinds[NumKinds] = {"draw",     "task",     "dispatch",
                                                        "special",  "indirect", "indirect_dispatch"};
        for (u32 k = 0; k < NumKinds; ++k) {
            const u64 kn = n.kind_packets[k] - l.kind_packets[k];
            std::printf(" %s{n=%llu payload_per_packet=%.0f}", kinds[k], ull(kn),
                        kn ? double(n.kind_payload[k] - l.kind_payload[k]) / double(kn) : 0.0);
        }
        // Depth percentages are % of commits in the window (capacity = 16 MiB ring).
        std::printf(" depth{avg_bytes=%.0f max_bytes=%llu coarse_pct[0,<=25,<=50,<=75,<100,=100]=",
                    samples ? double(D(&Cum::depth_sum)) / double(samples) : 0.0,
                    ull(a.depth_max));
        for (u32 i = 0; i < 6; ++i) {
            std::printf("%s%.1f", i ? "," : "", (n.depth_coarse[i] - l.depth_coarse[i]) * per_commit);
        }
        std::printf(" fine_pct[0,<=4K,<=64K,<=1M,<=8M,>8M]=");
        for (u32 i = 0; i < 6; ++i) {
            std::printf("%s%.1f", i ? "," : "", (n.depth_fine[i] - l.depth_fine[i]) * per_commit);
        }
        const u64 post_n = D(&Cum::post_n);
        std::printf("} write_path{ticks_per_packet=%.0f pct_of_window=%.2f} "
                    "wait_space{entries_s=%.0f spins_s=%.0f pct_of_window=%.2f} notify_s=%.0f "
                    "drains_s{calls=%.0f waited=%.0f empty=%.0f} "
                    "post_drain_to_commit{n=%llu avg_us=%.1f max_us=%.1f}\n",
                    d_packets ? double(D(&Cum::write_ticks)) / double(d_packets) : 0.0,
                    100.0 * double(D(&Cum::write_ticks)) / ticks, D(&Cum::wait_entries) / seconds,
                    D(&Cum::wait_spins) / seconds, 100.0 * double(D(&Cum::wait_ticks)) / ticks,
                    D(&Cum::notify_calls) / seconds,
                    (D(&Cum::drains) + D(&Cum::drain_empty)) / seconds, D(&Cum::drains) / seconds,
                    D(&Cum::drain_empty) / seconds, ull(post_n),
                    post_n ? double(D(&Cum::post_sum)) / double(post_n) / tpu : 0.0,
                    double(a.post_max) / tpu);

        // Stage B: durations of intervals that ended in the window (an interval crossing the
        // boundary counts where it ended); % of B per window is unavailable (independent relaxed
        // fields cannot account the open part safely). Maxima are since start. pending is
        // packets(A) - packets published by B: approximate, lags.
        std::printf("DrawRec B: packets_s=%.0f pending_approx=%lld sleep_attempts_s=%.0f "
                    "wait_returns_s=%.0f idle_spin_episodes_s=%.0f idle_spin_us_per_s=%.0f "
                    "wait_us_per_s=%.0f b_pct=unavailable max_since_start_us{idle_spin=%.1f "
                    "wait=%.1f} submissions_s{built=%.0f new=%.0f pool_reuse=%.0f}\n",
                    (b.packets - lb.packets) / seconds,
                    static_cast<long long>(n.packets - b.packets),
                    (b.sleep_attempts - lb.sleep_attempts) / seconds,
                    (b.wait_returns - lb.wait_returns) / seconds,
                    (b.idle_episodes - lb.idle_episodes) / seconds,
                    double(b.idle_ticks - lb.idle_ticks) / tpu / seconds,
                    double(b.wait_ticks - lb.wait_ticks) / tpu / seconds,
                    double(b.max_idle_ticks) / tpu, double(b.max_wait_ticks) / tpu,
                    (sub_n - a.last_sub_n) / seconds, (sub_new - a.last_sub_new) / seconds,
                    (sub_reuse - a.last_sub_reuse) / seconds);

        a.last = n;
        a.last_b = b;
        a.last_sub_n = sub_n;
        a.last_sub_new = sub_new;
        a.last_sub_reuse = sub_reuse;
        a.last_frames = frames;
        a.depth_max = a.post_max = 0;
        a.window = wall;
        a.window_tsc = tsc;
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("gow3:DrawRec");
#ifdef _WIN32
        Gow3Profiler::Start(GetCurrentThreadId(), "draw recording thread");
#endif
        on_stage_b = true;
        stage_b_tid.store(static_cast<u32>(gettid()), std::memory_order_release);
        u64 at = 0;
        BVals lb; // GOW3_DRAWREC_STATS: B's own accumulators, published in batches at transitions
        while (true) {
            // Packets follow each other within microseconds while a frame is decoded: spin,
            // and sleep only after a longer pause (between frames).
            u64 available = published.load(std::memory_order_acquire);
            if (available == at) {
                // With few hardware threads (Steam Deck: 8) a long spin takes time from the
                // game's own threads. GOW3_PIPE_SPIN_US overrides.
                static const auto spin_time = std::chrono::microseconds([] {
                    if (const char* env = std::getenv("GOW3_PIPE_SPIN_US")) {
                        return std::max(0, std::atoi(env));
                    }
                    return Gow3Threads::Available() >= 12 ? 200 : 50;
                }());
                const auto spin_until = std::chrono::steady_clock::now() + spin_time;
                // Empty episode (stats): entry to exit, minus the waits inside it. The wait is
                // timed from before the sleep protocol (so it includes that handful of
                // instructions) to the return of wake.wait; nothing sits between the protocol's
                // steps. wait_calls is not counted apart: only returns are (a wait skipped
                // because a packet arrived is sleep_attempts - wait_returns).
                const u64 idle_start = stats ? __rdtsc() : 0;
                u64 episode_wait = 0;
                for (u32 spins = 1; available == at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    __builtin_ia32_pause();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        u64 wait_start = 0;
                        if (stats) {
                            ++lb.sleep_attempts;
                            wait_start = __rdtsc();
                        }
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (published.load(std::memory_order_seq_cst) == at &&
                            !stop.stop_requested()) {
                            wake.wait(seen, std::memory_order_seq_cst);
                            if (stats) {
                                const u64 waited = __rdtsc() - wait_start;
                                ++lb.wait_returns;
                                lb.wait_ticks += waited;
                                lb.max_wait_ticks = std::max(lb.max_wait_ticks, waited);
                                episode_wait += waited;
                                PublishB(lb);
                            }
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                    available = published.load(std::memory_order_acquire);
                }
                if (stats) {
                    const u64 episode = __rdtsc() - idle_start;
                    const u64 spun = episode > episode_wait ? episode - episode_wait : 0;
                    ++lb.idle_episodes;
                    lb.idle_ticks += spun;
                    lb.max_idle_ticks = std::max(lb.max_idle_ticks, spun);
                    PublishB(lb);
                }
            }
            const auto* header = reinterpret_cast<const Header*>(ring.get() + at % Capacity);
            if (header->size == 0) {
                at += Capacity - at % Capacity;
                FlushIfCaughtUp(at);
                consumed.store(at, std::memory_order_release);
                continue;
            }
            const u64 start = __rdtsc();
            // gow3 21 Oct 2026, TEST KNOB (default off): GOW3_TEST_PIPE_DELAY_NS=<n> makes stage B
            // busy-wait n ns before each packet, like a slower or loaded CPU (laptops). Used to
            // reproduce the multithreaded late-constants race: stage A walks a draw's tables
            // while the scalar copy before it still waits here (GOW3_SRT_PENDING_DRAIN).
            static const u64 test_delay_ns = [] {
                const char* env = std::getenv("GOW3_TEST_PIPE_DELAY_NS");
                return env ? std::strtoull(env, nullptr, 10) : 0ull;
            }();
            if (test_delay_ns) {
                const auto until = std::chrono::steady_clock::now() +
                                   std::chrono::nanoseconds(test_delay_ns);
                while (std::chrono::steady_clock::now() < until) {
                    __builtin_ia32_pause();
                }
            }
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            // Interrupts held by a fault handled during the packet (IrqController::HoldScope).
            Platform::IrqC::Instance()->FlushHeld();
            busy_cycles.fetch_add(__rdtsc() - start, std::memory_order_relaxed);
            at += header->size;
            FlushIfCaughtUp(at);
            consumed.store(at, std::memory_order_release);
            if (stats) {
                ++lb.packets;
                stats_b.packets.store(lb.packets, std::memory_order_relaxed);
            }
        }
    }

    /// Stage B: records the fences the packets run so far left batched once no further packet is
    /// published, *before* the position is published. Stage A's Drain() returns when `consumed`
    /// reaches `head`, so afterwards every fence the drained packets queued is recorded: a
    /// WaitDeferredSignals() that follows a Drain() cannot wait for a batch still held here.
    void FlushIfCaughtUp(u64 at) {
        if (flush_handler == nullptr ||
            published.load(std::memory_order_acquire) != at) {
            return;
        }
        flush_handler(flush_context);
    }

    static inline thread_local bool on_stage_b = false;
    Handler handler;
    void* context;
    FlushHandler flush_handler;
    void* flush_context;
    std::unique_ptr<u8[]> ring;
    u64 head = 0; ///< stage A's write position
    u32 pending_size = 0;
    alignas(64) std::atomic<u64> published{0};
    alignas(64) std::atomic<u64> consumed{0};
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<bool> sleeping{false};
    std::atomic<u32> stage_b_tid{0};
    const bool stats = StatsEnabled(); ///< GOW3_DRAWREC_STATS, read once
    // Statistics blocks: A's plain members and B's atomics, each on its own cache lines and apart
    // from the queue's functional fields. Declared before `thread`, which must be destroyed first.
    alignas(64) StatsA stats_a;
    alignas(64) BShared stats_b;
    std::jthread thread;
};

} // namespace Vulkan
