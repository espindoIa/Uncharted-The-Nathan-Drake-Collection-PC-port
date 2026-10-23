// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 23 Oct 2026: GOW3_PIPE_STATS (gpu/shim/gow3_pipe_stats.h) without the game: stage A and
// stage B threads add to their counters while flips come at varying rates, from two threads at
// times, then idle, then flips that show frames 1-2 frames after stage A numbered them (a_seq),
// then flips with no pause (full ring), then a shutdown racing a flip thread.
// The CSV must add up to exactly what the stage threads counted, and a_seq must name the frame
// each row shows.
// Then the ways the game ends (child processes): std::_Exit with no flush in the middle of a
// burst of rows and during steady flips (only the rows queued since the writer's last pass may
// be lost, at most the last line is cut), and FlushForExit then std::_Exit (nothing queued
// before the call is lost); two launches with the same GOW3_PIPE_STATS (two files); a file
// that cannot be opened (one warning, collection off).
// Build: ninja -C out/gpu pipe-stats-test (or the g++ line in docs; run under TSan/ASan/UBSan).
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <sys/wait.h>
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

static const char* const test_dir = "pipe-stats-test.d";

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

static int RunMain() {
    const char* const csv_path = std::getenv("GOW3_PIPE_STATS");
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

    // B3: the file is GOW3_PIPE_STATS with .<UTC start>-<pid> before .csv.
    const std::string base{csv_path}, out{PS::OutputPath()};
    const std::string pid_suffix = "-" + std::to_string(static_cast<long>(getpid())) + ".csv";
    assert(out.size() == base.size() - 4 + 1 + 16 + pid_suffix.size());
    assert(out.compare(0, base.size() - 4, base, 0, base.size() - 4) == 0);
    assert(out.compare(out.size() - pid_suffix.size(), pid_suffix.size(), pid_suffix) == 0);

    // The CSV: one header, one column line, every row the same width, sums exact.
    std::FILE* file = std::fopen(out.c_str(), "r");
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
    assert(lines[0].find(" pid=" + std::to_string(static_cast<long>(getpid())) + " start_utc=") !=
           std::string::npos);
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
    std::puts("pipe stats: ok");
    return 0;
}

// ---- How the game ends: child processes that end with std::_Exit, as the game does. ----

/// A CSV as a reader that tolerates a cut last line sees it.
struct Parsed {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows; ///< complete rows
    int cut = 0;              ///< lines with the wrong width or no newline (B1: at most the last)
    bool cut_before_end = false;
};

static Parsed Parse(const std::string& path) {
    Parsed out;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    assert(file);
    std::string text;
    char buffer[65536];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) {
        text.append(buffer, n);
    }
    std::fclose(file);
    assert(text.rfind("# gow3_pipe_stats schema=1 ", 0) == 0);
    std::size_t pos = text.find('\n') + 1;
    const std::size_t columns_end = text.find('\n', pos);
    assert(columns_end != std::string::npos);
    out.columns = Split(text.substr(pos, columns_end - pos));
    pos = columns_end + 1;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        const bool complete_line = end != std::string::npos;
        const std::string line = text.substr(pos, (complete_line ? end : text.size()) - pos);
        pos = complete_line ? end + 1 : text.size();
        const auto cells = Split(line);
        if (!complete_line || cells.size() != out.columns.size()) {
            ++out.cut;
            out.cut_before_end |= pos < text.size();
            continue;
        }
        out.rows.push_back(cells);
    }
    return out;
}

static std::size_t Column(const Parsed& p, const char* name) {
    for (std::size_t i = 0; i < p.columns.size(); ++i) {
        if (p.columns[i] == name) {
            return i;
        }
    }
    std::fprintf(stderr, "missing column %s\n", name);
    std::abort();
}

static std::uint64_t Cell(const std::vector<std::string>& row, std::size_t column) {
    return std::strtoull(row[column].c_str(), nullptr, 10);
}

/// Complete rows are frames 2, 3, ... in order; a gap is a row the ring dropped (counted).
/// Returns the last frame_id on file (1: only the baseline flip, which writes no row).
static std::uint64_t CheckRows(const Parsed& p) {
    assert(!p.cut_before_end && p.cut <= 1);
    const std::size_t c_frame = Column(p, "frame_id"), c_dropped = Column(p, "dropped");
    std::uint64_t last_frame = 1, last_dropped = 0;
    for (const auto& row : p.rows) {
        const std::uint64_t frame = Cell(row, c_frame), dropped = Cell(row, c_dropped);
        assert(dropped >= last_dropped);
        assert(frame - last_frame == 1 + (dropped - last_dropped));
        last_frame = frame;
        last_dropped = dropped;
    }
    return last_frame;
}

static void WriteInfo(std::uint64_t flips, std::uint64_t extra) {
    const std::string path = std::string(std::getenv("GOW3_PIPE_STATS")) + ".info";
    std::FILE* file = std::fopen(path.c_str(), "w");
    assert(file);
    std::fprintf(file, "%llu %llu\n", static_cast<unsigned long long>(flips),
                 static_cast<unsigned long long>(extra));
    std::fclose(file);
}

static void ReadInfoFile(const std::string& csv, std::uint64_t& flips, std::uint64_t& extra) {
    std::FILE* file = std::fopen((csv + ".info").c_str(), "r");
    assert(file);
    unsigned long long a = 0, b = 0;
    const int n = std::fscanf(file, "%llu %llu", &a, &b);
    assert(n == 2);
    (void)n;
    std::fclose(file);
    flips = a;
    extra = b;
}

/// Child: flips, then std::_Exit(0) with no Shutdown, as the window thread does.
static int RunExit(const std::string& mode) {
    assert(PS::enabled);
    PS::OnFlip(0); // baseline, starts the writer
    std::this_thread::sleep_for(100ms); // past the writer's 50 ms calibration
    if (mode == "exit-burst") {
        // Bursts of 1000 rows with no pause (the ring holds 1024), then the exit as soon as the
        // writer has started on the last burst: it lands in the middle of a pass.
        for (int burst = 0; burst < 5; ++burst) {
            for (int i = 0; i < 1000; ++i) {
                PS::OnFlip(1);
            }
            std::this_thread::sleep_for(120ms);
        }
        const std::uint64_t written = PS::RowsWritten();
        for (int i = 0; i < 1000; ++i) {
            PS::OnFlip(1);
        }
        const std::uint64_t flips = PS::FrameId();
        while (PS::RowsWritten() == written) {
            std::this_thread::sleep_for(20us);
        }
        WriteInfo(flips, PS::RowsDropped());
        std::_Exit(0);
    }
    if (mode == "exit-steady") {
        // One flip every 2 ms, then exit at once: only the rows since the last pass are lost.
        const auto end = std::chrono::steady_clock::now() + 700ms;
        while (std::chrono::steady_clock::now() < end) {
            PS::OnFlip(1);
            std::this_thread::sleep_for(2ms);
        }
        const std::uint64_t flips = PS::FrameId();
        WriteInfo(flips, PS::RowsDropped());
        std::_Exit(0);
    }
    // exit-flush: flips stopped, then FlushForExit: every flip is on file.
    // exit-flush-live: another thread keeps flipping through FlushForExit and _Exit.
    const bool live = mode == "exit-flush-live";
    std::atomic<bool> run{true};
    std::thread flipper([&] {
        while (run.load(std::memory_order_relaxed)) {
            PS::OnFlip(2);
            std::this_thread::sleep_for(1ms);
        }
    });
    std::this_thread::sleep_for(400ms);
    if (!live) {
        run.store(false);
        flipper.join();
    }
    const std::uint64_t flips = PS::FrameId();
    const auto t0 = std::chrono::steady_clock::now();
    PS::FlushForExit();
    const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    WriteInfo(flips, static_cast<std::uint64_t>(waited));
    std::_Exit(0); // `flipper` may still be flipping (live): _Exit does not join it
}

static int Spawn(const char* self, const char* mode, const std::string& csv) {
#ifdef _WIN32
    _putenv_s("GOW3_PIPE_STATS", csv.c_str());
    const char* args[] = {self, mode, nullptr};
    return static_cast<int>(_spawnv(_P_WAIT, self, args));
#else
    // The driver runs no other thread, so setenv before fork is safe.
    setenv("GOW3_PIPE_STATS", csv.c_str(), 1);
    const pid_t pid = fork();
    if (pid == 0) {
        char* args[] = {const_cast<char*>(self), const_cast<char*>(mode), nullptr};
        execv(self, args);
        std::_Exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unsetenv("GOW3_PIPE_STATS");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}

/// The session files (B3: <stem>.<UTC start>-<pid>.csv) next to `base` (<stem>.csv).
static std::vector<std::string> Sessions(const std::string& base) {
    namespace fs = std::filesystem;
    const fs::path path{base};
    const std::string stem = path.stem().string() + ".";
    std::vector<std::string> out;
    for (const auto& entry : fs::directory_iterator(path.parent_path())) {
        const std::string name = entry.path().filename().string();
        if (name.size() > stem.size() + 4 && name.rfind(stem, 0) == 0 &&
            name.compare(name.size() - 4, 4, ".csv") == 0 &&
            name.find(".gpu.") == std::string::npos) {
            out.push_back(entry.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Runs `mode` with GOW3_PIPE_STATS=<test_dir>/<name>; returns the file it wrote.
static std::string Child(const char* self, const char* mode, const char* name) {
    const std::string base = std::string(test_dir) + "/" + name;
    const auto before = Sessions(base);
    const int code = Spawn(self, mode, base);
    if (code != 0) {
        std::fprintf(stderr, "child %s exited with %d\n", mode, code);
        std::exit(1);
    }
    const auto after = Sessions(base);
    assert(after.size() == before.size() + 1); // one new file, the old ones untouched
    for (const auto& session : after) {
        if (std::find(before.begin(), before.end(), session) == before.end()) {
            return session;
        }
    }
    std::abort();
}

/// The numbers a child wrote next to `base` before its _Exit.
static void ReadInfo(const char* name, std::uint64_t& flips, std::uint64_t& extra) {
    ReadInfoFile(std::string(test_dir) + "/" + name, flips, extra);
}

static void TestExits(const char* self) {
    // Frames after the last row on file were either dropped by a full ring (counted in
    // `dropped`) or queued and lost with the process.
    auto lost_queued = [](const Parsed& p, std::uint64_t flips, std::uint64_t dropped_total) {
        const std::uint64_t last = CheckRows(p);
        const std::uint64_t dropped_on_file =
            p.rows.empty() ? 0 : Cell(p.rows.back(), Column(p, "dropped"));
        assert(last <= flips && dropped_total >= dropped_on_file);
        return flips - last - (dropped_total - dropped_on_file);
    };
    { // _Exit in the middle of a pass: complete rows in order, at most the last line cut.
        const std::string csv = Child(self, "exit-burst", "burst.csv");
        std::uint64_t flips = 0, dropped = 0;
        ReadInfo("burst.csv", flips, dropped);
        const Parsed p = Parse(csv);
        const std::uint64_t lost = lost_queued(p, flips, dropped);
        std::printf("exit mid-pass: %llu flips, %zu complete rows, %llu queued rows lost, "
                    "%d cut line(s)\n",
                    static_cast<unsigned long long>(flips), p.rows.size(),
                    static_cast<unsigned long long>(lost), p.cut);
        std::fflush(stdout);
        assert(lost <= 1024); // at most one ring of rows
    }
    { // _Exit during steady flips: only rows since the last pass (100 ms here: ~50) are lost.
        const std::string csv = Child(self, "exit-steady", "steady.csv");
        std::uint64_t flips = 0, dropped = 0;
        ReadInfo("steady.csv", flips, dropped);
        const Parsed p = Parse(csv);
        const std::uint64_t lost = lost_queued(p, flips, dropped);
        std::printf("exit during steady flips (2 ms): %llu flips, %zu complete rows, %llu queued "
                    "rows lost, %d cut line(s)\n",
                    static_cast<unsigned long long>(flips), p.rows.size(),
                    static_cast<unsigned long long>(lost), p.cut);
        std::fflush(stdout);
        assert(dropped == 0 && lost <= 150); // 150 rows = 300 ms of flips: 3 write periods
    }
    { // FlushForExit then _Exit: every flip before the call is on file.
        const std::string csv = Child(self, "exit-flush", "flush.csv");
        std::uint64_t flips = 0, waited_us = 0;
        ReadInfo("flush.csv", flips, waited_us);
        const Parsed p = Parse(csv);
        const std::uint64_t last = CheckRows(p);
        std::printf("FlushForExit + _Exit: %llu flips, last frame on file %llu, waited %llu us\n",
                    static_cast<unsigned long long>(flips), static_cast<unsigned long long>(last),
                    static_cast<unsigned long long>(waited_us));
        std::fflush(stdout);
        assert(last == flips && p.cut == 0);
        assert(waited_us < 500000);
    }
    { // Same with a thread still flipping through the call and the exit.
        const std::string csv = Child(self, "exit-flush-live", "flush-live.csv");
        std::uint64_t flips = 0, waited_us = 0;
        ReadInfo("flush-live.csv", flips, waited_us);
        const Parsed p = Parse(csv);
        const std::uint64_t last = CheckRows(p);
        std::printf("FlushForExit + _Exit, still flipping: frame %llu at the call, last frame on "
                    "file %llu, waited %llu us\n",
                    static_cast<unsigned long long>(flips), static_cast<unsigned long long>(last),
                    static_cast<unsigned long long>(waited_us));
        std::fflush(stdout);
        // The frame number is published just before its row is queued: allow that one row.
        assert(last + 1 >= flips);
    }
}

/// Child, GOW3_PIPE_STATS in a directory that does not exist (B8): one warning, then every
/// hook sees collection off; nothing is written or counted.
static int RunOpenFails() {
    const std::string err = std::string(test_dir) + "/open-fails.err";
    if (!std::freopen(err.c_str(), "w", stderr)) {
        return 2;
    }
    assert(PS::enabled && PS::Active());
    for (int i = 0; i < 5; ++i) {
        PS::OnFlip(0);
        if (PS::Active()) { // as the hooks in vk_draw_pipe.h and liverpool.cpp do
            PS::stage_a.packets.Add(1);
        }
    }
    assert(!PS::Active() && PS::output_failed.load());
    assert(PS::stage_a.packets.Load() == 0 && PS::NoteFlipPacket() == 0);
    assert(PS::OutputPath() == nullptr && PS::RowsWritten() == 0);
    PS::FlushForExit(); // returns at once
    PS::Shutdown();
    std::fflush(stderr);
    return 0;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "main") {
        return RunMain();
    }
    if (mode == "open-fails") {
        return RunOpenFails();
    }
    if (mode.rfind("exit-", 0) == 0) {
        return RunExit(mode);
    }
    // Driver: `enabled` is read once at startup, so each case runs as a child process with
    // GOW3_PIPE_STATS set. The driver itself starts no thread. Run with the variable already
    // set (the old way), it is the main case alone.
    if (PS::enabled) {
        return RunMain();
    }
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directory(test_dir);
    Child(argv[0], "main", "main.csv");
    TestExits(argv[0]);
    { // B3: two launches with the same GOW3_PIPE_STATS write two files, both whole.
        const std::string first = Child(argv[0], "exit-flush", "twice.csv");
        const std::string second = Child(argv[0], "exit-flush", "twice.csv");
        assert(first != second && Sessions(std::string(test_dir) + "/twice.csv").size() == 2);
        for (const auto& csv : {first, second}) {
            const Parsed p = Parse(csv);
            assert(CheckRows(p) > 100 && p.cut == 0);
        }
        std::printf("two launches: %s, %s\n", first.c_str(), second.c_str());
    }
    { // B8: the file cannot be opened: one warning, collection off, the child ends normally.
        const std::string missing = std::string(test_dir) + "/missing/run.csv";
        assert(Spawn(argv[0], "open-fails", missing) == 0);
        std::FILE* file = std::fopen((std::string(test_dir) + "/open-fails.err").c_str(), "r");
        assert(file);
        int warnings = 0;
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            if (std::strstr(line, "GOW3_PIPE_STATS: cannot open") && std::strstr(line, "off")) {
                ++warnings;
                std::printf("open fails: %s", line);
            }
        }
        std::fclose(file);
        assert(warnings == 1 && !std::filesystem::exists(std::string(test_dir) + "/missing"));
    }
    if (!std::getenv("GOW3_PIPE_STATS_KEEP")) {
        std::filesystem::remove_all(test_dir);
    }
    std::puts("pipe stats exits: ok");
    return 0;
}
