// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {
void DumpGpuCheckpoints(const Instance& instance);
void DumpDeviceFault(const Instance& instance);

// gow3 24 Oct 2026: GOW3_WAIT_TIMEOUT_MS (50..10000) shortens the wait poll and dumps the stuck submission.
static const u32 g_wait_timeout_ms = [] {
    const char* env = std::getenv("GOW3_WAIT_TIMEOUT_MS");
    return env ? std::clamp<u32>(static_cast<u32>(std::strtoul(env, nullptr, 10)), 50, 10000) : 0u;
}();
static const u64 WAIT_TIMEOUT = g_wait_timeout_ms ? u64(g_wait_timeout_ms) * 1000000 : 1000000000;

static constexpr u32 MAX_DIAGS = 8;
static SubmitDiag* g_diags[MAX_DIAGS];
static std::atomic<u32> g_num_diags{0};
static std::atomic<u64> g_sparse_semaphore{0}; // VkSemaphore handle bits

struct SparseRecord {
    u64 tick{};
    s64 ns{};
    u32 binds{}, unbinds{};
    u64 bytes{};
};
static SparseRecord g_sparse_ring[16];
static std::atomic<u32> g_sparse_count{0};

static s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

SubmitDiag* RegisterSubmitDiag(const Semaphore* semaphore) {
    // Schedulers are built at startup, before any submit; extra ones beyond MAX_DIAGS go unrecorded.
    auto* diag = new SubmitDiag;
    diag->semaphore = semaphore;
    const u32 index = g_num_diags.fetch_add(1);
    diag->index = static_cast<int>(index);
    if (index < MAX_DIAGS) {
        g_diags[index] = diag;
    }
    return diag;
}

void SetSparseSemaphore(vk::Semaphore semaphore) {
    g_sparse_semaphore.store(reinterpret_cast<u64>(static_cast<VkSemaphore>(semaphore)));
}

void NoteSparseBind(u64 tick, u32 binds, u32 unbinds, u64 bytes) {
    const u32 n = g_sparse_count.fetch_add(1);
    g_sparse_ring[n % 16] = {tick, NowNs(), binds, unbinds, bytes};
}

static const char* DiagName(const SubmitDiag& d, char (&buf)[32]) {
    static const char* const names[] = {"draw", "present", "flip"};
    if (d.log_id >= 0 && d.log_id < 3) {
        return names[d.log_id];
    }
    std::snprintf(buf, sizeof(buf), "scheduler#%d", d.index);
    return buf;
}

void Semaphore::DumpStuckSubmission(u64 /*tick*/) {
    const u64 stuck = KnownGpuTick() + 1;
    const auto device = instance.GetDevice();
    const auto counter = [&](vk::Semaphore s) {
        auto [r, v] = device.getSemaphoreCounterValue(s);
        return r == vk::Result::eSuccess ? static_cast<unsigned long long>(v) : ~0ull;
    };
    const SubmitDiag* own = nullptr;
    for (u32 i = 0; i < std::min(g_num_diags.load(), MAX_DIAGS); ++i) {
        if (g_diags[i]->semaphore == this) {
            own = g_diags[i];
        }
    }
    if (!own) {
        std::fprintf(stderr, "GPU wait: semaphore %p is not a scheduler's; first incomplete tick %llu\n",
                     (void*)(VkSemaphore)semaphore.get(), (unsigned long long)stuck);
        return;
    }
    char nbuf[32];
    const SubmitRecord& rec = own->ring[stuck % SubmitDiag::RING];
    if (rec.tick != stuck) {
        std::fprintf(stderr, "GPU wait: %s tick %llu has no record (not submitted yet or ring overwritten)\n",
                     DiagName(*own, nbuf), (unsigned long long)stuck);
        return;
    }
    const s64 now = NowNs();
    const vk::Semaphore sparse{reinterpret_cast<VkSemaphore>(g_sparse_semaphore.load())};
    bool waits_sparse = false;
    std::fprintf(stderr, "GPU wait: stuck %s tick %llu, submitted %.1f ms ago, %u waits%s\n",
                 DiagName(*own, nbuf), (unsigned long long)stuck, double(now - rec.submit_ns) / 1e6,
                 rec.num_waits, rec.num_waits > 8 ? " (first 8 shown)" : "");
    for (u32 i = 0; i < std::min(rec.num_waits, 8u); ++i) {
        const char* what = "other";
        char wbuf[32];
        if (sparse && rec.waits[i] == sparse) {
            what = "sparse";
            waits_sparse = true;
        } else {
            for (u32 d = 0; d < std::min(g_num_diags.load(), MAX_DIAGS); ++d) {
                if (g_diags[d]->semaphore->Handle() == rec.waits[i]) {
                    what = DiagName(*g_diags[d], wbuf);
                }
            }
        }
        std::fprintf(stderr, "  wait %p (%s) value=%llu current=%llu\n",
                     (void*)(VkSemaphore)rec.waits[i], what, (unsigned long long)rec.values[i],
                     counter(rec.waits[i]));
    }
    if (waits_sparse) {
        const u32 count = g_sparse_count.load();
        const unsigned long long done = counter(sparse);
        for (u32 k = 0; k < std::min(count, 16u); ++k) {
            const SparseRecord& s = g_sparse_ring[(count - 1 - k) % 16];
            std::fprintf(stderr,
                         "  sparse bind tick=%llu %.1f ms ago: %u binds, %u unbinds, %.2f MiB, %s\n",
                         (unsigned long long)s.tick, double(now - s.ns) / 1e6, s.binds, s.unbinds,
                         double(s.bytes) / 1048576.0, done >= s.tick ? "complete" : "NOT complete");
        }
    }
}

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        // gow3 20 Oct 2026: a device loss is often first seen here (GitHub #67); report what
        // the GPU was doing, as the waits and the submit do, before stopping.
        if (counter_result == vk::Result::eErrorDeviceLost) {
            DumpGpuCheckpoints(instance);
            DumpDeviceFault(instance);
        }
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

bool Semaphore::WaitFor(u64 tick, u64 timeout_ns) {
    if (IsFree(tick)) {
        return true;
    }
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };
    if (instance.GetDevice().waitSemaphores(&wait_info, timeout_ns) != vk::Result::eSuccess) {
        return false; // timeout (a lost device shows in the next plain Wait)
    }
    Refresh();
    return true;
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    unsigned seconds=0;
    for (;;) {
        const auto result=instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT);
        if (result==vk::Result::eSuccess) break;
        if (result==vk::Result::eErrorDeviceLost) {
            DumpGpuCheckpoints(instance);
            DumpDeviceFault(instance);
        }
        ASSERT_MSG(result==vk::Result::eTimeout, "GPU semaphore wait failed: {}",vk::to_string(result));
        if (g_wait_timeout_ms) {
            static std::atomic<u32> printed{0};
            if (printed.fetch_add(1) < 50) {
                Refresh();
                std::fprintf(stderr, "GPU wait: %u ms, target=%llu completed=%llu submitted-next=%llu\n",
                             unsigned(++seconds * g_wait_timeout_ms), (unsigned long long)tick,
                             (unsigned long long)KnownGpuTick(), (unsigned long long)CurrentTick());
                DumpStuckSubmission(tick);
            }
        } else if (++seconds==1 || seconds%10==0) {
            DumpGpuCheckpoints(instance);
            Refresh();
            std::fprintf(stderr,"GPU wait: %u seconds, target=%llu completed=%llu submitted-next=%llu\n",seconds,
                         (unsigned long long)tick,(unsigned long long)KnownGpuTick(),
                         (unsigned long long)CurrentTick());
        }
    }
    Refresh();
}

} // namespace Vulkan
