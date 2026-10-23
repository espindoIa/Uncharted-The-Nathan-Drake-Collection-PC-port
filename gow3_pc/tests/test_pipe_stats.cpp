// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 23 Oct 2026: GOW3_PIPE_STATS (gpu/shim/gow3_pipe_stats.h) without the game: stage A and
// stage B threads add to their counters while flips come at varying rates, from two threads at
// times, then idle, then flips that show frames 1-2 frames after stage A numbered them (a_seq),
// then flips with no pause (full ring), then a shutdown racing a flip thread.
// The CSV must add up to exactly what the stage threads counted, and a_seq must name the frame
// each row shows.
// Build: ninja -C out/gpu pipe-stats-test (or the g++ line in docs; run under TSan/ASan/UBSan).
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include "gow3_pipe_stats.h"

extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
std::uint64_t runtime_disabled_optimizations2 = 0;
}
namespace Common {
enum class ThreadPriority : unsigned;
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
} // namespace Common

namespace PS = Gow3PipeStats;
using namespace std::chrono_literals;

static const char* const csv_path = "pipe-stats-test.csv";

static void SetToggles2(std::uint64_t value) {
    __atomic_store_n(&runtime_disabled_optimizations2, value, __ATOMIC_RELEASE);
}

static double ProcessCpuMs() {
    return 1000.0 * static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
}

static std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    std::string cell;
    std::istringstream in{line};
    while (std::getline(in, cell, ',')) {
        out.push_back(cell);
    }
    if (!line.empty() && line.back() == ',') {
        out.push_back("");
    }
    return out;
}

int main(int argc, char** argv) {
    (void)argc;
    if (!std::getenv("GOW3_PIPE_STATS")) {
        // `enabled` is read once at startup: run again with the variable set.
#ifdef _WIN32
        _putenv_s("GOW3_PIPE_STATS", csv_path);
        return static_cast<int>(_spawnv(_P_WAIT, argv[0], argv));
#else
        setenv("GOW3_PIPE_STATS", csv_path, 1);
        execv(argv[0], argv);
        std::perror("execv");
        return 1;
#endif
    }
    assert(PS::enabled);

    // Occupancy buckets: 0, <1K, <4K, <16K, <64K, <256K, <1M, <4M, >=4M.
    const std::uint64_t sizes[] = {0, 1, 1023, 1024, 4095, 4096, 16383, 16384, 65535, 65536,
                                   262143, 262144, 1048575, 1048576, 4194303, 4194304, 16u << 20};
    const int buckets[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8};
    for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const std::uint64_t before = PS::stage_a.occ[buckets[i]].Load();
        PS::NoteOccupancy(sizes[i]);
        assert(PS::stage_a.occ[buckets[i]].Load() == before + 1);
    }

    // The toggle pauses collection; off it, collection is on.
    assert(PS::Active());
    SetToggles2(Gow3Toggle::PipeStats);
    assert(!PS::Active());
    SetToggles2(0);
    assert(PS::Active());

    // Baseline flip: starts the writer; counts from here on belong to frames.
    assert(PS::FrameId() == 0);
    PS::OnFlip(0);
    assert(PS::FrameId() == 1);
    const std::uint64_t a_base = PS::stage_a.packets.Load();
    const std::uint64_t b_base = PS::stage_b.packets.Load();

    // Variable load: A and B add in bursts, flips every 0-2 ms, a second flip thread for a while.
    std::atomic<bool> run{true};
    std::uint64_t a_total = 0, b_total = 0, b_busy_cyc = 0;
    std::thread stage_a([&] {
        std::mt19937 rng{1};
        while (run.load(std::memory_order_relaxed)) {
            const int burst = static_cast<int>(rng() % 200);
            for (int i = 0; i < burst; ++i) {
                PS::stage_a.packets.Add(1);
                PS::stage_a.bytes.Add(256);
                if (!(++a_total & PS::OccSampleMask)) {
                    PS::NoteOccupancy(rng() % (8u << 20));
                }
            }
            PS::NoteDrain(static_cast<PS::DrainClass>(rng() % 4), rng() % 1000, rng() % 100);
            if (rng() % 4 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500));
            }
        }
    });
    std::thread stage_b([&] {
        std::mt19937 rng{2};
        while (run.load(std::memory_order_relaxed)) {
            const std::uint64_t cycles = rng() % 100000;
            PS::NotePacketB(cycles);
            b_busy_cyc += cycles;
            ++b_total;
            if (rng() % 64 == 0) {
                PS::stage_b.wait_spin_cyc.Add(1000);
                PS::stage_b.wait_block_cyc.Add(5000);
                PS::stage_b.sleeps.Add(1);
                std::this_thread::yield();
            }
        }
    });
    std::atomic<bool> second{true};
    std::thread second_flipper([&] {
        while (second.load(std::memory_order_relaxed)) {
            PS::OnFlip(7);
            std::this_thread::sleep_for(300us);
        }
    });
    std::mt19937 rng{3};
    const auto load_end = std::chrono::steady_clock::now() + 1s;
    int flips = 0;
    while (std::chrono::steady_clock::now() < load_end) {
        PS::OnFlip(static_cast<std::uint64_t>(flips));
        ++flips;
        if (flips == 200) {
            second.store(false);
            second_flipper.join();
            // Pause collection for a stretch: rows go on with stats_paused=1.
            SetToggles2(Gow3Toggle::PipeStats);
        }
        if (flips == 300) {
            SetToggles2(0);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
    }
    if (second_flipper.joinable()) {
        second.store(false);
        second_flipper.join();
    }
    run.store(false);
    stage_a.join();
    stage_b.join();
    PS::OnFlip(0); // closes the last frame of the load phase

    // Idle: no flips; the writer sleeps (no busy wait).
    const double cpu_before = ProcessCpuMs();
    std::this_thread::sleep_for(500ms);
    const double idle_cpu_ms = ProcessCpuMs() - cpu_before;
    std::printf("idle: process CPU %.3f ms over 500 ms\n", idle_cpu_ms);
    assert(idle_cpu_ms < 50.0);

    // Flips lag stage A: A numbers each frame at its flip packet (NoteFlipPacket) and tags its
    // side records (what the recorder, submit and GPU files will hold) with that number; the
    // flip that shows the frame comes 1-2 frames later and passes the number on. Joining on
    // a_seq finds every frame's record; tagging with FrameId() read when A started the frame
    // (the frame_id the flip thread had reached) does not.
    struct LagFrame {
        std::uint64_t seq, value, naive_tag;
    };
    std::map<std::uint64_t, std::uint64_t> expected_seq;    // frame_id -> a_seq it shows
    std::map<std::uint64_t, std::uint64_t> side_by_seq;      // a_seq -> side record value
    std::map<std::uint64_t, std::vector<std::uint64_t>> side_by_naive; // FrameId() at start
    constexpr std::uint64_t lag_frames = 300;
    {
        std::mutex lag_mutex;
        std::deque<LagFrame> in_flight; // frames A finished and no flip has shown yet
        std::thread lag_a([&] {
            std::mt19937 lag_rng{4};
            const std::uint64_t first = PS::stage_a.seq.Load();
            for (std::uint64_t i = 0; i < lag_frames; ++i) {
                const std::uint64_t naive_tag = PS::FrameId();
                PS::stage_a.packets.Add(1);
                ++a_total;
                const std::uint64_t seq = PS::NoteFlipPacket();
                assert(seq == first + i + 1);
                for (;;) { // at most 3 frames in flight, like a bounded pipeline
                    {
                        std::scoped_lock lock{lag_mutex};
                        if (in_flight.size() < 3) {
                            in_flight.push_back({seq, seq * 10 + 7, naive_tag});
                            break;
                        }
                    }
                    std::this_thread::sleep_for(50us);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100 + lag_rng() % 300));
            }
        });
        std::mt19937 lag_rng{5};
        std::uint64_t shown = 0;
        while (shown < lag_frames) {
            const std::size_t lag = 1 + lag_rng() % 2; // this flip shows the frame 1 or 2 back
            LagFrame frame{};
            {
                std::scoped_lock lock{lag_mutex};
                const bool last = shown + in_flight.size() == lag_frames;
                if (in_flight.size() < (last ? 1 : lag + 1)) {
                    frame.seq = 0;
                } else {
                    frame = in_flight.front();
                    in_flight.pop_front();
                }
            }
            if (!frame.seq) {
                std::this_thread::sleep_for(50us);
                continue;
            }
            if (shown == 100) {
                SetToggles2(Gow3Toggle::PipeStats); // a_seq keeps counting while paused
            }
            if (shown == 150) {
                SetToggles2(0);
            }
            PS::OnFlip(0, frame.seq);
            expected_seq[PS::FrameId()] = frame.seq;
            side_by_seq[frame.seq] = frame.value;
            side_by_naive[frame.naive_tag].push_back(frame.value);
            ++shown;
            std::this_thread::sleep_for(std::chrono::microseconds(100 + lag_rng() % 300));
        }
        lag_a.join();
    }
    PS::OnFlip(0); // closes the last lag frame

    // Saturation: flips with no pause fill the ring; the extra rows are dropped, never waited on.
    const std::uint64_t dropped_before = PS::RowsDropped();
    const auto sat_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200000; ++i) {
        PS::OnFlip(0);
    }
    const double sat_ns = std::chrono::duration<double, std::nano>(
                              std::chrono::steady_clock::now() - sat_start)
                              .count() /
                          200000.0;
    std::printf("saturation: %.1f ns per flip, %llu dropped\n", sat_ns,
                static_cast<unsigned long long>(PS::RowsDropped() - dropped_before));
    assert(PS::RowsDropped() > dropped_before);

    // Shutdown while another thread keeps flipping; twice; flips after it are ignored.
    std::atomic<bool> late{true};
    std::thread late_flipper([&] {
        while (late.load(std::memory_order_relaxed)) {
            PS::OnFlip(9);
        }
    });
    std::this_thread::sleep_for(20ms);
    PS::Shutdown();
    PS::Shutdown();
    std::this_thread::sleep_for(20ms);
    late.store(false);
    late_flipper.join();
    const std::uint64_t written = PS::RowsWritten();
    PS::OnFlip(0);
    assert(PS::RowsWritten() == written);

    // The CSV: one header, one column line, every row the same width, sums exact.
    std::FILE* file = std::fopen(csv_path, "r");
    assert(file);
    std::vector<std::string> lines;
    char buffer[8192];
    while (std::fgets(buffer, sizeof(buffer), file)) {
        std::string line{buffer};
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        lines.push_back(line);
    }
    std::fclose(file);
    assert(lines.size() >= 3);
    assert(lines[0].rfind("# gow3_pipe_stats schema=1 ", 0) == 0);
    const auto columns = Split(lines[1]);
    auto column = [&](const char* name) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i] == name) {
                return i;
            }
        }
        std::fprintf(stderr, "missing column %s\n", name);
        std::abort();
    };
    const std::size_t c_seq = column("a_seq"), c_issued = column("a_seq_issued");
    std::uint64_t seq_rows = 0, last_issued = 0;
    const std::size_t c_frame = column("frame_id"), c_paused = column("stats_paused"),
                      c_a = column("a_packets"), c_b = column("b_packets"),
                      c_dropped = column("dropped"), c_sleeps = column("b_sleeps");
    column("rec_busy_ns");
    column("fence_wait_ns");
    std::uint64_t a_sum = 0, b_sum = 0, last_frame = 1, last_dropped = 0, paused_rows = 0;
    std::uint64_t sleeps_sum = 0;
    bool counted_while_paused = false;
    for (std::size_t i = 2; i < lines.size(); ++i) {
        const auto cells = Split(lines[i]);
        assert(cells.size() == columns.size());
        const std::uint64_t frame = std::strtoull(cells[c_frame].c_str(), nullptr, 10);
        assert(frame > last_frame);
        last_frame = frame;
        // a_seq: the frame the flip passed on, empty when it passed none; a_seq_issued: how
        // far stage A had numbered by then (never behind the frame shown, never decreasing).
        const std::uint64_t issued = std::strtoull(cells[c_issued].c_str(), nullptr, 10);
        assert(issued >= last_issued);
        last_issued = issued;
        const auto want = expected_seq.find(frame);
        if (want == expected_seq.end()) {
            assert(cells[c_seq].empty());
        } else {
            const std::uint64_t seq = std::strtoull(cells[c_seq].c_str(), nullptr, 10);
            assert(seq == want->second);
            assert(issued >= seq && issued - seq <= 3);
            assert(side_by_seq.at(seq) == seq * 10 + 7); // the a_seq join finds its record
            ++seq_rows;
        }
        const std::uint64_t dropped = std::strtoull(cells[c_dropped].c_str(), nullptr, 10);
        assert(dropped >= last_dropped);
        last_dropped = dropped;
        a_sum += std::strtoull(cells[c_a].c_str(), nullptr, 10);
        b_sum += std::strtoull(cells[c_b].c_str(), nullptr, 10);
        sleeps_sum += std::strtoull(cells[c_sleeps].c_str(), nullptr, 10);
        if (cells[c_paused] == "1") {
            ++paused_rows;
            // The test's stage threads ignore the toggle, so counts may still show up here;
            // in the game the hooks check Active() and add nothing.
            counted_while_paused |= cells[c_a] != "0";
        }
    }
    std::printf("rows %zu (written %llu), paused rows %llu, a %llu/%llu, b %llu/%llu\n",
                lines.size() - 2, static_cast<unsigned long long>(written),
                static_cast<unsigned long long>(paused_rows),
                static_cast<unsigned long long>(a_sum), static_cast<unsigned long long>(a_total),
                static_cast<unsigned long long>(b_sum), static_cast<unsigned long long>(b_total));
    // The naive tag: several frames share one FrameId() value and others get none.
    std::uint64_t naive_shared = 0;
    for (const auto& [tag, values] : side_by_naive) {
        naive_shared += values.size() > 1;
    }
    std::printf("lag: %llu rows joined on a_seq, %zu naive tags for %llu frames (%llu shared)\n",
                static_cast<unsigned long long>(seq_rows), side_by_naive.size(),
                static_cast<unsigned long long>(lag_frames),
                static_cast<unsigned long long>(naive_shared));
    assert(seq_rows == lag_frames);
    assert(naive_shared > 0);
    (void)counted_while_paused;
    (void)b_busy_cyc;
    assert(lines.size() - 2 == written);
    assert(paused_rows > 0);
    assert(a_sum == a_total + 0 * a_base && a_base == 0);
    assert(b_sum == b_total && b_base == 0);
    assert(sleeps_sum == PS::stage_b.sleeps.Load());
    if (!std::getenv("GOW3_PIPE_STATS_KEEP")) {
        std::remove(csv_path);
    }
    std::puts("pipe stats: ok");
    return 0;
}
