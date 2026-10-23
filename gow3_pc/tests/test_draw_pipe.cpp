// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 23 Oct 2026: the real DrawPipe (gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h)
// with a synthetic handler (not RunDrawPacket): a full ring with a slow consumer, stage B idle
// (spin then sleep), bursts that force sleep and wake, many wraps, shutdown with stage B asleep,
// blocked in the handler or A blocked on a full ring, and A waiting in Drain. Items must arrive
// once, in order, unchanged; a deadlock ends the test through a watchdog.
// With GOW3_PIPE_STATS=<file> set, the pipe-stats hooks must have seen the waits; unset, nothing
// is counted. Thread CPU figures are measured on Linux only (/proc, pthread CPU clocks).
// Build: ninja -C out/gpu draw-pipe-test; run it with and without GOW3_PIPE_STATS, and with
// `slow` (fewer items) under TSan/ASan/UBSan.
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#endif

extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
std::uint64_t runtime_disabled_optimizations2 = 0;
}
namespace Common {
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
} // namespace Common
namespace Gow3Profiler {
void Start(unsigned long, const char*) {}
} // namespace Gow3Profiler

using Vulkan::DrawPipe;

struct Ctx {
    std::atomic<u64> count{0}, sum{0}, next_seq{0}, order_errors{0}, bytes_errors{0};
    std::atomic<u64> delay_ns{0};
    std::atomic<bool> gate_closed{false};
    std::atomic<bool> in_handler{false};
};

static void Handler(void* c, const u8* p, u32 size) {
    auto* ctx = static_cast<Ctx*>(c);
    ctx->in_handler.store(true);
    while (ctx->gate_closed.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::microseconds(100));
    u64 seq; std::memcpy(&seq, p, 8);
    if (seq != ctx->next_seq.load(std::memory_order_relaxed)) ctx->order_errors++;
    ctx->next_seq.store(seq + 1, std::memory_order_relaxed);
    for (u32 i = 8; i < size; ++i) if (p[i] != u8(seq + i)) { ctx->bytes_errors++; break; }
    if (u64 d = ctx->delay_ns.load(std::memory_order_relaxed)) {
        auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(d);
        while (std::chrono::steady_clock::now() < until) {}
    }
    ctx->sum.fetch_add(seq, std::memory_order_relaxed);
    ctx->count.fetch_add(1, std::memory_order_release);
    ctx->in_handler.store(false);
}

static void Put(DrawPipe& pipe, u64 seq, u32 payload) {
    u8* p = pipe.Begin(payload);
    std::memcpy(p, &seq, 8);
    for (u32 i = 8; i < payload; ++i) p[i] = u8(seq + i);
    pipe.Commit(payload);
}

#ifdef __linux__
static double TaskCpuMs(u32 tid) { // /proc schedstat: ns on CPU
    std::ifstream f("/proc/self/task/" + std::to_string(tid) + "/schedstat");
    unsigned long long ns = 0;
    f >> ns;
    return ns / 1e6;
}
static double ThreadCpuMs(std::thread& t) {
    clockid_t id;
    pthread_getcpuclockid(t.native_handle(), &id);
    timespec ts;
    clock_gettime(id, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static constexpr bool cpu_clocks = true;
#else
static double TaskCpuMs(u32) {
    return 0.0;
}
static double ThreadCpuMs(std::thread&) {
    return 0.0;
}
static constexpr bool cpu_clocks = false;
#endif

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// Watchdog: a deadlock aborts the process with a message instead of hanging.
static void Watchdog(int seconds, const char* name) {
    std::thread([=] { std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::fprintf(stderr, "DEADLOCK/TIMEOUT in %s\n", name); std::_Exit(3); }).detach();
}

int main(int argc, char**) {
    const bool slow = argc > 1;  // sanitizer runs: fewer items
    const u64 N = slow ? 20000 : 200000;
    Watchdog(slow ? 600 : 120, "whole test");

    { // 1. Saturation: 1 MiB packets (Capacity/16) and a slow consumer: the ring fills, A blocks.
        Ctx ctx; ctx.delay_ns = 2'000'000;  // 2 ms per packet
        auto pipe = std::make_unique<DrawPipe>(Handler, &ctx);
        const u32 big = 1u << 20; const u64 n = 64;
        auto t0 = std::chrono::steady_clock::now();
        std::atomic<bool> done{false};
        std::thread prod([&] { for (u64 i = 0; i < n; ++i) Put(*pipe, i, big); pipe->Drain(); done = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        double a0 = ThreadCpuMs(prod);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        double a1 = ThreadCpuMs(prod);
        prod.join();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[1 saturation] items=%llu/%llu order_err=%llu bytes_err=%llu wall=%.0f ms; "
                    "producer CPU while ring full (WaitForSpace): %.1f ms CPU / 50 ms wall\n",
                    (unsigned long long)ctx.count.load(), (unsigned long long)n,
                    (unsigned long long)ctx.order_errors.load(), (unsigned long long)ctx.bytes_errors.load(), ms, a1 - a0);
        CHECK(ctx.count == n && ctx.order_errors == 0 && ctx.bytes_errors == 0, "saturation lost/reordered items");
        pipe.reset();
    }
    { // 2. Empty queue: stage B CPU while idle (spin then futex sleep).
        Ctx ctx; auto pipe = std::make_unique<DrawPipe>(Handler, &ctx);
        while (!pipe->StageBThreadId()) std::this_thread::yield();
        Put(*pipe, 0, 64); pipe->Drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const u32 tid = pipe->StageBThreadId();
        double b0 = TaskCpuMs(tid);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        double b1 = TaskCpuMs(tid);
        std::printf("[2 idle] stage B CPU over 2000 ms idle: %.3f ms (spin budget %s)\n", b1 - b0,
                    Gow3Threads::Available() >= 12 ? "200 us" : "50 us");
        CHECK(!cpu_clocks || b1 - b0 < 20.0, "idle stage B burns CPU");
        // 2b. One packet after the sleep must still be delivered (wake path).
        Put(*pipe, 1, 64); pipe->Drain();
        CHECK(ctx.count == 2, "packet after sleep lost");
        pipe.reset();
    }
    { // 3. Bursts: random packet sizes, bursts separated by pauses > spin time (forces sleep/wake).
        Ctx ctx; auto pipe = std::make_unique<DrawPipe>(Handler, &ctx);
        std::mt19937 rng(1234); u64 seq = 0, drains_req = 0;
        const int bursts = slow ? 60 : 400;
        for (int b = 0; b < bursts; ++b) {
            const u32 k = rng() % 3000;
            for (u32 i = 0; i < k; ++i) Put(*pipe, seq++, 8 + rng() % (rng() % 50 ? 1024 : 200000));
            if (rng() % 4 == 0) { pipe->Drain(); ++drains_req; }
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2 ? 50 : 1500));
        }
        pipe->Drain();
        std::printf("[3 bursts] items=%llu/%llu order_err=%llu bytes_err=%llu drains_waited=%llu packets=%llu\n",
                    (unsigned long long)ctx.count.load(), (unsigned long long)seq,
                    (unsigned long long)ctx.order_errors.load(), (unsigned long long)ctx.bytes_errors.load(),
                    (unsigned long long)pipe->drains, (unsigned long long)pipe->packets);
        CHECK(ctx.count == seq && ctx.order_errors == 0 && ctx.bytes_errors == 0, "bursts lost items");
        pipe.reset();
    }
    { // 4. Throughput with tiny packets (wrap path exercised many times: N * 64 B > 16 MiB).
        Ctx ctx; auto pipe = std::make_unique<DrawPipe>(Handler, &ctx);
        for (u64 i = 0; i < N; ++i) Put(*pipe, i, 8 + (i % 7) * 100);
        pipe->Drain();
        CHECK(ctx.count == N && ctx.order_errors == 0, "throughput lost items");
        std::printf("[4 wrap/throughput] items=%llu/%llu ok\n", (unsigned long long)ctx.count.load(), (unsigned long long)N);
        pipe.reset();
    }
    { // 5a. Shutdown while stage B is asleep (futex wait): destructor must wake and join.
        Ctx ctx; auto pipe = std::make_unique<DrawPipe>(Handler, &ctx);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto t0 = std::chrono::steady_clock::now(); pipe.reset();
        std::printf("[5a shutdown, B asleep] destructor %.3f ms\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    { // 5b. Shutdown with packets in flight and B blocked inside the handler: destructor
      //     Drain()s (no lost items), so A is blocked in Drain until the gate opens.
        Ctx ctx; ctx.gate_closed = true; auto pipe = std::make_unique<DrawPipe>(Handler, &ctx);
        for (u64 i = 0; i < 100; ++i) Put(*pipe, i, 4096);
        std::thread opener([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); ctx.gate_closed = false; });
        pipe.reset();  // blocks in Drain until gate opens
        opener.join();
        std::printf("[5b shutdown, B blocked in handler, 100 in flight] delivered=%llu/100\n", (unsigned long long)ctx.count.load());
        CHECK(ctx.count == 100, "shutdown lost items");
    }
    { // 5c. Shutdown right after A was blocked on a full ring (WaitForSpace).
        Ctx ctx; ctx.gate_closed = true; auto pipe = std::make_unique<DrawPipe>(Handler, &ctx);
        std::atomic<bool> produced{false};
        std::thread prod([&] { for (u64 i = 0; i < 40; ++i) Put(*pipe, i, 1u << 20); produced = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const bool blocked = !produced.load();
        double a0 = ThreadCpuMs(prod);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        double a1 = ThreadCpuMs(prod);
        ctx.gate_closed = false; prod.join(); pipe.reset();
        std::printf("[5c shutdown after A blocked on full ring] A was blocked=%d, A CPU %.1f ms / 50 ms wall, delivered=%llu/40\n",
                    blocked, a1 - a0, (unsigned long long)ctx.count.load());
        CHECK(blocked && ctx.count == 40, "full-ring shutdown");
    }
    { // 6. Drain CPU: A waiting in Drain on a slow packet (pause x4096 then yield loop).
        Ctx ctx; ctx.gate_closed = true; auto pipe = std::make_unique<DrawPipe>(Handler, &ctx);
        Put(*pipe, 0, 64);
        std::thread drainer([&] { pipe->Drain(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        double a0 = ThreadCpuMs(drainer);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        double a1 = ThreadCpuMs(drainer);
        ctx.gate_closed = false; drainer.join();
        std::printf("[6 drain wait] A CPU while waiting in Drain: %.1f ms / 50 ms wall (drain_cycles=%llu)\n",
                    a1 - a0, (unsigned long long)pipe->drain_cycles);
        pipe.reset();
    }
    if (Gow3PipeStats::Active()) { // GOW3_PIPE_STATS hooks (vector/pipe-stats): the waits above were seen
        namespace PS = Gow3PipeStats;
        std::printf("[stats] a_packets=%llu a_drains=%llu drain_spin_cyc=%llu drain_yield_cyc=%llu space_waits=%llu space_spin_cyc=%llu wake_notifies=%llu occ_samples=%llu\n",
            (unsigned long long)PS::stage_a.packets.Load(), (unsigned long long)PS::stage_a.drains.Load(),
            (unsigned long long)PS::stage_a.drain_spin_cyc.Load(), (unsigned long long)PS::stage_a.drain_yield_cyc.Load(),
            (unsigned long long)PS::stage_a.space_waits.Load(), (unsigned long long)PS::stage_a.space_spin_cyc.Load(),
            (unsigned long long)PS::stage_a.wake_notifies.Load(), (unsigned long long)PS::stage_a.occ_samples.Load());
        std::printf("[stats] b_packets=%llu b_sleeps=%llu b_wait_spin_cyc=%llu b_wait_block_cyc=%llu\n",
            (unsigned long long)PS::stage_b.packets.Load(), (unsigned long long)PS::stage_b.sleeps.Load(),
            (unsigned long long)PS::stage_b.wait_spin_cyc.Load(), (unsigned long long)PS::stage_b.wait_block_cyc.Load());
        CHECK(PS::stage_a.drains.Load() > 0 && PS::stage_a.drain_yield_cyc.Load() > 0, "drain stats");
        CHECK(PS::stage_a.space_waits.Load() > 0 && PS::stage_b.sleeps.Load() > 0, "space/sleep stats");
    } else {
        CHECK(Gow3PipeStats::stage_a.packets.Load() == 0 && Gow3PipeStats::stage_b.packets.Load() == 0, "stats off: nothing counted");
    }
    std::printf(failures ? "RESULT: %d FAILURES\n" : "RESULT: PASS (%d failures)\n", failures);
    return failures ? 1 : 0;
}
