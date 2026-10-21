// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: small read-only guest buffers (constants) copied by the GPU command thread (stage A of
// the draw pipeline, vk_draw_pipe.h) into this ring, so the draw recording thread only binds
// them. Positions are monotonic byte counts; a region is reused once the submission that
// recorded its last draw has completed on the GPU. Stage B stamps each packet it records with
// the submission tick (Stamp), stage A retires stamps whose tick the GPU has passed.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>

#include "common/assert.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class ConstantRing {
public:
    static constexpr u64 Capacity = 32ull << 20;

    ConstantRing(const Instance& instance, Scheduler& scheduler_)
        : scheduler{scheduler_},
          buffer{instance, 0, Capacity, VideoCore::MemoryType::Stream, "gow3 constant ring"} {
        ASSERT(!buffer.mapped_data.empty());
    }

    [[nodiscard]] vk::Buffer Handle() const noexcept {
        return buffer.Handle();
    }

    /// Stage A: `size` bytes at `alignment`; returns the ring offset, or nullopt when the ring is
    /// still in use by the GPU (the caller leaves the binding to stage B).
    std::optional<u64> Allocate(u64 size, u64 alignment) {
        u64 at = (position + alignment - 1) & ~(alignment - 1);
        if (at % Capacity + size > Capacity) {
            at += Capacity - at % Capacity; // wrap to the ring start
        }
        const auto in_use = [&] {
            return at + size > Capacity && at + size - Capacity > Floor();
        };
        if (in_use()) {
            Retire();
            if (in_use()) {
                return std::nullopt;
            }
        }
        position = at + size;
        last_allocation = at;
        return at % Capacity;
    }

    /// Stage A: the absolute position of the last allocation (for Reusable and Hold).
    [[nodiscard]] u64 LastAllocation() const noexcept {
        return last_allocation;
    }
    /// Stage A: whether the region allocated at absolute position `at` still holds its bytes.
    [[nodiscard]] bool Reusable(u64 at) const noexcept {
        return position - at < Capacity / 2;
    }
    /// Stage A: the packet being built also binds the region allocated at `at`. Regions are
    /// otherwise reused once the packet that allocated them is done on the GPU; this one must
    /// stay until the packet being built is done too (EndPacket).
    void Hold(u64 at) noexcept {
        packet_hold = std::min(packet_hold, at);
    }
    /// Stage A, after the packet's allocations and before its Position() is read.
    void EndPacket() {
        if (packet_hold == NoHold) {
            return;
        }
        // An end no earlier packet has: retired reaches it only when a stamp of this packet's
        // submission or a later one is done (stamps of one tick keep only the last end).
        ++position;
        while (!holds.empty() && holds.back().at >= packet_hold) {
            holds.pop_back(); // released no later than this one, and no lower
        }
        holds.push_back({packet_hold, position});
        packet_hold = NoHold;
    }

    [[nodiscard]] u8* Data(u64 offset) noexcept {
        return buffer.mapped_data.data() + offset;
    }
    /// The ring's buffer (late-constant refresh, vk_rasterizer.cpp).
    [[nodiscard]] VideoCore::Buffer& Storage() noexcept {
        return buffer;
    }

    /// Stage A, after writing: makes the bytes visible to the GPU.
    void Flush(u64 offset, u64 size) {
        buffer.Flush(offset, size);
    }

    /// Stage A: the position the next packet's allocations end at.
    [[nodiscard]] u64 Position() const noexcept {
        return position;
    }

    /// Stage B, after recording a packet whose allocations end at `end`. Stamps of one
    /// submission tick are merged: only the last end per tick is published. The pending pair is
    /// published once the tick has moved on, also when `end` equals the pending end.
    void Stamp(u64 end) {
        const u64 tick = scheduler.CurrentTick();
        // 20 Oct, U3 chapter 2: after a camera turn every Allocate failed (about 1,014,000/s, 0 ring
        // copies/s) until quit, because a failed Allocate leaves position unchanged, so each later
        // packet stamped the same end and an equality return placed before this publish never
        // published the pending end of an earlier tick. Retired and Floor() then stayed put. A
        // Publish that found the array full is retried here on later packets. An unchanged end keeps
        // its old pending_tick: a later reuse would have raised the end, so the pair stays safe.
        if (tick != pending_tick && pending_end != last_published) {
            Publish(pending_end, pending_tick);
        }
        if (end == pending_end) {
            return;
        }
        pending_end = end;
        pending_tick = tick;
    }

private:
    void Publish(u64 end, u64 tick) {
        const u64 at = stamps_head.load(std::memory_order_relaxed);
        if (at - stamps_tail.load(std::memory_order_acquire) >= stamps.size()) {
            return; // full: this region retires with a later stamp
        }
        stamps[at % stamps.size()] = {end, tick};
        stamps_head.store(at + 1, std::memory_order_release);
        last_published = end;
    }

    /// Positions before this are free: retired, and not held by a later packet's binding.
    u64 Floor() {
        while (!holds.empty() && retired >= holds.front().end) {
            holds.pop_front();
        }
        return holds.empty() ? retired : std::min(retired, holds.front().at);
    }

    void Retire() {
        auto* semaphore = scheduler.GetWorkSemaphore();
        semaphore->Refresh();
        u64 at = stamps_tail.load(std::memory_order_relaxed);
        const u64 head = stamps_head.load(std::memory_order_acquire);
        while (at < head && semaphore->IsFree(stamps[at % stamps.size()].tick)) {
            retired = stamps[at % stamps.size()].end;
            ++at;
        }
        stamps_tail.store(at, std::memory_order_release);
    }

    struct StampEntry {
        u64 end;
        u64 tick;
    };
    Scheduler& scheduler;
    VideoCore::Buffer buffer;
    u64 position = 0;     ///< stage A
    u64 retired = 0;      ///< stage A: the GPU is done with everything before this
    u64 last_allocation = 0; ///< stage A
    static constexpr u64 NoHold = ~u64{0};
    u64 packet_hold = NoHold; ///< stage A: lowest reused position of the packet being built
    struct HeldRegion {
        u64 at;  ///< positions from here on stay
        u64 end; ///< until retired reaches this (the end of the packet that reused them)
    };
    std::deque<HeldRegion> holds; ///< stage A: increasing `at` and `end`
    u64 pending_end = 0, pending_tick = 0, last_published = 0; ///< stage B
    std::array<StampEntry, 1024> stamps{};
    alignas(64) std::atomic<u64> stamps_head{0}; ///< stage B writes
    alignas(64) std::atomic<u64> stamps_tail{0}; ///< stage A reads
};

} // namespace Vulkan
