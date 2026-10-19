// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_submit_log.h"

namespace Vulkan::SubmitLog {
namespace {

FILE* File() {
    static FILE* file = [] {
        const char* path = std::getenv("GOW3_SUBMIT_LOG");
        return path && path[0] ? std::fopen(path, "w") : nullptr;
    }();
    return file;
}

std::mutex file_mutex;

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif

unsigned long ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

void Write(const char* line) {
    std::scoped_lock lock{file_mutex};
    std::fputs(line, File());
    static u32 lines = 0;
    if (++lines % 256 == 0) {
        std::fflush(File());
    }
}

// Timestamp queries: two per submission (start, end), reused a pool's length later.
constexpr u32 Slots = 8192;
vk::Device device;
vk::QueryPool pool;
double period_ns = 1.0;
std::atomic<u32> next_slot{0};

struct Watched {
    Semaphore* semaphore;
    u64 tick;
    u32 slot;
};

// One watcher per scheduler: a present submission must not be timed behind a draw tick.
struct Watcher {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Watched> queue;
    std::once_flag started;

    void Run(int sched) {
        for (;;) {
            Watched next;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [this] { return !queue.empty(); });
                next = queue.front();
                queue.pop_front();
            }
            while (!next.semaphore->WaitFor(next.tick, 1'000'000'000)) {
            }
            const double done = NowMs();
            double gpu[2] = {-1.0, -1.0};
            if (next.slot != ~0u) {
                u64 stamps[2] = {};
                if (device.getQueryPoolResults(pool, next.slot, 2, sizeof(stamps), stamps, sizeof(u64),
                                               vk::QueryResultFlagBits::e64) == vk::Result::eSuccess) {
                    gpu[0] = double(stamps[0]) * period_ns / 1e6;
                    gpu[1] = double(stamps[1]) * period_ns / 1e6;
                }
            }
            char line[160];
            std::snprintf(line, sizeof(line), "T %.3f %d %llu %.3f %.3f\n", done, sched,
                          static_cast<unsigned long long>(next.tick), gpu[0], gpu[1]);
            Write(line);
        }
    }
};
Watcher watchers[3];

} // namespace

bool Enabled() {
    return File() != nullptr;
}

double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Event(const char* tag, const char* fmt, ...) {
    if (!Enabled()) {
        return;
    }
    char line[256];
    int n = std::snprintf(line, sizeof(line), "%s %.3f %lu ", tag, NowMs(), ThreadId());
    va_list args;
    va_start(args, fmt);
    n += std::vsnprintf(line + n, sizeof(line) - n - 1, fmt, args);
    va_end(args);
    if (n > int(sizeof(line)) - 2) {
        n = int(sizeof(line)) - 2;
    }
    line[n] = '\n';
    line[n + 1] = 0;
    Write(line);
}

u32 Begin(const Instance& instance, vk::CommandBuffer cmdbuf) {
    if (!Enabled()) {
        return ~0u;
    }
    static std::once_flag created;
    std::call_once(created, [&instance] {
        device = instance.GetDevice();
        pool = Check<"submit log timestamps">(device.createQueryPool({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = Slots * 2,
        }));
        device.resetQueryPool(pool, 0, Slots * 2);
        period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    });
    const u32 slot = (next_slot.fetch_add(1, std::memory_order_relaxed) % Slots) * 2;
    device.resetQueryPool(pool, slot, 2);
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, pool, slot);
    return slot;
}

void End(vk::CommandBuffer cmdbuf, u32 slot) {
    if (slot != ~0u) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, pool, slot + 1);
    }
}

void Submitted(int sched, Semaphore* semaphore, u64 tick, u64 slot_tick, u32 slot, u32 waits,
               const SubmitTimes& times) {
    if (!Enabled() || sched < 0 || sched > 2) {
        return;
    }
    char line[200];
    std::snprintf(line, sizeof(line), "U %.3f %d %llu %llu %lu %u %.3f %.3f %.3f %.3f\n",
                  times.submitted, sched, static_cast<unsigned long long>(tick),
                  static_cast<unsigned long long>(slot_tick), ThreadId(), waits,
                  times.locked - times.start, times.recorded - times.locked,
                  times.copied - times.recorded, times.submitted - times.copied);
    Write(line);
    auto& watcher = watchers[sched];
    std::call_once(watcher.started, [sched] {
        std::thread([sched] { watchers[sched].Run(sched); }).detach();
    });
    {
        std::scoped_lock lock{watcher.mutex};
        watcher.queue.push_back({semaphore, tick, slot});
    }
    watcher.cv.notify_one();
}

} // namespace Vulkan::SubmitLog
