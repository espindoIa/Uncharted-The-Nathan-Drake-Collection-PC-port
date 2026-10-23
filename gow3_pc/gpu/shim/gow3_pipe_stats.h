// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 23 Oct 2026: per-frame CPU pipeline statistics, GOW3_PIPE_STATS=<file> (off by default).
//
// Why: the stage A / stage B numbers so far are 5 s windows (GOW3_FRAME_STATS) or thread CPU
// time that counts spin waits as busy (analysis/STATUS.md:3485: "Busy% includes spin waits").
// Neither gives per-frame percentiles, how full the draw pipe is, or how long each stage spins
// versus sleeps. Schema and protocol: docs/team/PIPE_STATS_SCHEMA_v1.md (outside the repo while
// it is reviewed) and the "Pipe stats" note in analysis/STATUS.md.
//
// Rules that keep it off the hot path:
// - Every counter has one writer thread (the stage that owns it) and is a std::atomic updated
//   with store(load(relaxed) + n, relaxed): a plain mov on x86, no lock prefix. Readers (the
//   flip snapshot) load relaxed. Columns with more than one writer would use fetch_add (none
//   yet; the schema lists the writers of each column).
// - Clocks are read only in wait paths (spin, sleep, drain, full ring) and once per packet on
//   stage B, which already reads __rdtsc there.
// - The flip pushes one raw snapshot into a preallocated ring; a background thread
//   (gow3:PipeStats) turns snapshots into CSV rows. A full ring drops the row (counted).
// - Off (no GOW3_PIPE_STATS): every hook is one predictable branch on a flag read once.
//   GOW3_PIPE_STATS_BUILD=0 at compile time removes the hooks entirely.
// - Toggle second word bit 16777216 (Gow3Toggle::PipeStats) pauses collection live (A/B of the
//   instrumentation's own cost); rows keep coming with stats_paused=1 and zero counters.
#pragma once

#ifndef GOW3_PIPE_STATS_BUILD
#define GOW3_PIPE_STATS_BUILD 1
#endif

#include <atomic>
#include <cstdint>
#include <x86intrin.h>
#include "gow3_toggles.h"

namespace Gow3PipeStats {

using u64 = std::uint64_t;

/// One writer thread: a store of load + n (no read-modify-write instruction).
struct Single {
    std::atomic<u64> value{0};
    void Add(u64 n) noexcept {
        value.store(value.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    u64 Load() const noexcept {
        return value.load(std::memory_order_relaxed);
    }
};

/// Bytes in flight in the draw pipe when stage A commits (sampled): 0, <1K, <4K, ... >=4M.
inline constexpr int OccBuckets = 9;
inline constexpr int OccSampleMask = 15; ///< one commit in 16 is sampled

/// Written by stage A (the GPU command thread) only.
struct alignas(64) StageA {
    Single idle_block_ns;  ///< asleep on submit_cv, nothing to do
    Single idle_poll_ns;   ///< idle loop polling deferred signals (does drains and flushes)
    Single drains;         ///< drains that had to wait
    Single drain_spin_cyc; ///< drain, pause phase
    Single drain_yield_cyc;///< drain, yield phase (after 4096 pauses)
    Single drain_cmd_cyc, drain_pm4_cyc, drain_sub_cyc, drain_other_cyc; ///< by reason
    Single space_waits;    ///< Begin() found the ring full
    Single space_spin_cyc; ///< pause loop in WaitForSpace
    Single packets, bytes, wake_notifies;
    Single occ_samples;
    Single occ[OccBuckets];
};

/// Written by stage B (gow3:DrawRec) only.
struct alignas(64) StageB {
    Single packets;
    Single busy_cyc;       ///< handler time, as DrawPipe::busy_cycles
    Single wait_spin_cyc;  ///< ring empty, spinning
    Single wait_block_cyc; ///< ring empty, asleep in wake.wait
    Single sleeps;
    /// Longest packet of the current frame: epoch (low 16 bits of the frame) << 48 | cycles.
    std::atomic<u64> max_packet{0};
};

inline StageA stage_a;
inline StageB stage_b;
/// Frame number the flip snapshot is closing (written by the flip thread only).
inline std::atomic<u64> frame_epoch{0};

#if GOW3_PIPE_STATS_BUILD
/// GOW3_PIPE_STATS=<file> was set (read once).
bool Requested() noexcept;
inline const bool enabled = Requested();
#else
inline constexpr bool enabled = false;
#endif

/// Collection on: requested and not paused by the toggle.
inline bool Active() noexcept {
    if constexpr (!GOW3_PIPE_STATS_BUILD) {
        return false;
    }
    return enabled && !Gow3Toggle::Disabled2(Gow3Toggle::PipeStats);
}

inline u64 Cycles() noexcept {
    return __rdtsc();
}

inline void NoteOccupancy(u64 bytes) noexcept {
    int bucket = 0;
    if (bytes) {
        bucket = 1;
        for (u64 limit = 1024; bucket < OccBuckets - 1 && bytes >= limit; limit <<= 2) {
            ++bucket;
        }
    }
    stage_a.occ_samples.Add(1);
    stage_a.occ[bucket].Add(1);
}

inline void NotePacketB(u64 cycles) noexcept {
    stage_b.packets.Add(1);
    stage_b.busy_cyc.Add(cycles);
    const u64 epoch = frame_epoch.load(std::memory_order_relaxed) & 0xffff;
    const u64 current = stage_b.max_packet.load(std::memory_order_relaxed);
    const u64 clamped = cycles & ((1ull << 48) - 1);
    if ((current >> 48) != epoch || (current & ((1ull << 48) - 1)) < clamped) {
        stage_b.max_packet.store(epoch << 48 | clamped, std::memory_order_relaxed);
    }
}

/// Which drain-reason column a DrawPipe::Reason falls in (values from vk_draw_pipe.h).
enum DrainClass { DrainPm4, DrainCommands, DrainSubmissionEnd, DrainOther };

inline void NoteDrain(DrainClass cls, u64 spin_cycles, u64 yield_cycles) noexcept {
    const u64 total = spin_cycles + yield_cycles;
    stage_a.drains.Add(1);
    stage_a.drain_spin_cyc.Add(spin_cycles);
    stage_a.drain_yield_cyc.Add(yield_cycles);
    switch (cls) {
    case DrainPm4: stage_a.drain_pm4_cyc.Add(total); break;
    case DrainCommands: stage_a.drain_cmd_cyc.Add(total); break;
    case DrainSubmissionEnd: stage_a.drain_sub_cyc.Add(total); break;
    default: stage_a.drain_other_cyc.Add(total); break;
    }
}

#if GOW3_PIPE_STATS_BUILD
/// Flip thread, once per VideoOutDriver::Flip: numbers the frame and queues a snapshot for the
/// writer thread (started on the first call). `a_frame_id` is Gow3Stats::gpu_frames.
/// Lock-free and allocation-free after the first call; a second thread flipping at the same
/// time drops its row instead of waiting.
void OnFlip(u64 a_frame_id) noexcept;

/// Last frame number OnFlip handed out (0 before the first flip).
u64 FrameId() noexcept;

/// Writes what is queued and stops the writer (also registered with atexit). Later flips are
/// counted and ignored.
void Shutdown() noexcept;

/// Statistics for tests: rows written, rows dropped (ring full or concurrent flip).
u64 RowsWritten() noexcept;
u64 RowsDropped() noexcept;

#else
inline void OnFlip(u64) noexcept {}
inline u64 FrameId() noexcept {
    return 0;
}
inline void Shutdown() noexcept {}
inline u64 RowsWritten() noexcept {
    return 0;
}
inline u64 RowsDropped() noexcept {
    return 0;
}
#endif

} // namespace Gow3PipeStats
