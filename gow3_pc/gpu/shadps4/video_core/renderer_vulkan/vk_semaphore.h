// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <thread>
#include <queue>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;
class Semaphore;

// gow3 24 Oct 2026: per-submission ring so a GPU wait timeout can name the stuck submission's waits.
struct SubmitRecord {
    u64 tick{};
    s64 submit_ns{}; ///< steady_clock
    u32 num_waits{};
    vk::Semaphore waits[8];
    u64 values[8]{};
};

struct SubmitDiag {
    static constexpr u32 RING = 512;
    const Semaphore* semaphore{};
    int index{};
    int log_id{-1}; ///< Scheduler::SetLogId: draw 0, present 1, flip 2
    SubmitRecord ring[RING];
};

/// Registered at Scheduler construction, before any submit; never freed.
SubmitDiag* RegisterSubmitDiag(const Semaphore* semaphore);
/// BufferCache's bindSparse timeline semaphore (named "sparse" in the dump).
void SetSparseSemaphore(vk::Semaphore semaphore);
/// One bindSparse call (ring of the last 16, shown in the timeout dump).
void NoteSparseBind(u64 tick, u32 binds, u32 unbinds, u64 bytes);

class Semaphore {
public:
    explicit Semaphore(const Instance& instance_);
    ~Semaphore();

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return semaphore.get();
    }

    /// Refresh the known GPU tick
    void Refresh();

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick);

    /// gow3: waits at most `timeout_ns` for a tick; true when the GPU reached it.
    bool WaitFor(u64 tick, u64 timeout_ns);

protected:
    void DumpStuckSubmission(u64 tick);

    const Instance& instance;
    vk::UniqueSemaphore semaphore;    ///< Timeline semaphore.
    std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    std::atomic<u64> current_tick{1}; ///< Current logical tick.
};

} // namespace Vulkan
