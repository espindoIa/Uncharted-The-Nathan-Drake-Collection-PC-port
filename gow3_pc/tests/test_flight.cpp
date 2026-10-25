// The engine flight recorder (gpu/shim/gow3_flight.h) without Vulkan or a game: ring
// wrap-around, deltas and columns of the CSV, the anomaly rules on synthetic flips (the stall
// median, the reset quiet time, skipped draws, camera cuts, cooldown and the cap), label
// sanitising and the .tmp-then-rename write.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include "../gpu/shim/gow3_flight.h"

using namespace Gow3Flight;

namespace {
constexpr std::int64_t kMs = 1000;

Row MakeRow(std::int64_t t_us, std::uint32_t events = 0, float fwd_x = 0.0f, float fwd_z = 1.0f) {
    Row row{};
    row.t_us = t_us;
    row.events = events;
    row.gauges[FwdX] = fwd_x;
    row.gauges[FwdZ] = fwd_z;
    return row;
}

int Ready(Recorder& recorder, int kind) {
    int n = 0;
    for (int i = 0; i < Recorder::kMaxRequests; ++i) {
        auto& r = recorder.RequestSlot(i);
        n += r.state.load() == 2 && r.kind == kind;
    }
    return n;
}

Recorder::Request* Find(Recorder& recorder, int kind) {
    for (int i = 0; i < Recorder::kMaxRequests; ++i) {
        auto& r = recorder.RequestSlot(i);
        if (r.state.load() == 2 && r.kind == kind) {
            return &r;
        }
    }
    return nullptr;
}

void FreeAll(Recorder& recorder) {
    for (int i = 0; i < Recorder::kMaxRequests; ++i) {
        recorder.RequestSlot(i).state.store(0);
    }
}

std::vector<std::string> Split(const std::string& line, char sep) {
    std::vector<std::string> out(1);
    for (const char c : line) {
        if (c == sep) {
            out.emplace_back();
        } else {
            out.back() += c;
        }
    }
    return out;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

void TestRingWrap() {
    Recorder recorder(64, false);
    for (int i = 0; i < 200; ++i) {
        recorder.Record(MakeRow(i * 16 * kMs));
    }
    assert(recorder.LastSeq() == 200);
    const auto rows = recorder.Collect(1, 200);
    assert(rows.size() == 64);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        assert(rows[i].seq == 137 + i);
        assert(rows[i].t_us == std::int64_t(136 + i) * 16 * kMs);
    }
    assert(recorder.Collect(190, 195).size() == 6);
}

void TestCsv() {
    Recorder recorder(128, false);
    for (int i = 0; i < 10; ++i) {
        Row row = MakeRow(i * 10 * kMs, i == 4 ? UpscalerReset | NoCamera : 0);
        row.detail = i == 4 ? ResetUiOnly : 0;
        row.cum[CDraws] = 1000 + 7 * std::uint64_t(i);
        row.cum[CImageUploadBytes] = 2'000'000 * std::uint64_t(i);
        row.cum[CImagesLive] = 42;
        row.gauges[CamX] = float(i);
        recorder.Record(row);
    }
    const auto rows = recorder.Collect(1, 10);
    const std::string text = Csv(rows, 5, "test", 1'700'000'000'123, 128, "probe.exe/1", 1'700'000'000'000);
    const auto lines = Lines(text);
    assert(lines.size() == 12);
    assert(lines[0] == "# flight v1 unix_ms=1700000000123 reason=test frames=10 ring=128 build=probe.exe/1");
    const auto header = Split(lines[1], ',');
    assert(int(header.size()) == CsvColumns());
    assert(CsvColumns() == 47);
    assert(header.back() == "presents");
    const auto column = [&](const char* name) {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == name) {
                return int(i);
            }
        }
        assert(!"missing column");
        return -1;
    };
    int markers = 0;
    for (int r = 0; r < 10; ++r) {
        const auto fields = Split(lines[2 + r], ',');
        assert(fields.size() == header.size());
        assert(std::stoull(fields[column("seq")]) == std::uint64_t(r + 1));
        assert(std::stod(fields[column("d_draws")]) == (r ? 7.0 : 0.0));
        assert(std::abs(std::stod(fields[column("d_image_upload_mb")]) - (r ? 2.0 : 0.0)) < 1e-9);
        assert(std::stod(fields[column("dt_ms")]) == (r ? 10.0 : 0.0));
        assert(std::stoll(fields[column("unix_ms")]) == 1'700'000'000'000 + r * 10);
        assert(fields[column("images_live")] == "42");
        // The cam_x ramp of 1 per row: cam_move 1 after the first row, no turn.
        assert(std::abs(std::stod(fields[column("cam_move")]) - (r ? 1.0 : 0.0)) < 1e-6);
        assert(std::stod(fields[column("cam_jump_deg")]) < 1e-3);
        markers += fields[column("marker")] == "1";
        if (r == 4) {
            assert(fields[column("events")] == "0x5");
            assert(fields[column("event_names")] == "UpscalerReset|NoCamera");
            assert(fields[column("event_detail")] == "5");
            assert(fields[column("marker")] == "1");
        } else {
            assert(fields[column("events")] == "0x0");
            assert(fields[column("event_names")].empty());
        }
    }
    assert(markers == 1);
    // A dropped marker row: the nearest one is marked.
    const auto lines2 = Lines(Csv(rows, 50, "x", 0, 128, "b", 0));
    assert(Split(lines2.back(), ',')[column("marker")] == "1");
}

// `n` flips `step` apart from `t`; returns the time of the last one.
std::int64_t Steady(Recorder& recorder, std::int64_t t, int n, std::int64_t step = 16'600) {
    for (int i = 0; i < n; ++i) {
        recorder.Record(MakeRow(t));
        if (i + 1 < n) {
            t += step;
        }
    }
    return t;
}

void TestStall() {
    Recorder recorder(1024, true);
    // Within the 5 s grace: no dump however long.
    std::int64_t t = Steady(recorder, 0, 40);
    recorder.Record(MakeRow(t += 500 * kMs));
    assert(Ready(recorder, AutoStall) == 0);
    t = Steady(recorder, t + 16'600, 400); // on to 7.8 s
    assert(Ready(recorder, AutoStall) == 0);
    recorder.Record(MakeRow(t += 59 * kMs)); // under 60 ms
    assert(Ready(recorder, AutoStall) == 0);
    t = Steady(recorder, t + 16'600, 40);
    recorder.Record(MakeRow(t += 100 * kMs));
    auto* request = Find(recorder, AutoStall);
    assert(request && request->marker_seq == recorder.LastSeq());
    assert(request->fire_seq == recorder.LastSeq() + Recorder::kFramesAfter);
    FreeAll(recorder);
    // Cooldown: a second stall 1 s later is not dumped, one 15 s later is.
    t = Steady(recorder, t + 16'600, 60);
    recorder.Record(MakeRow(t += 200 * kMs));
    assert(Ready(recorder, AutoStall) == 0);
    t = Steady(recorder, t + 16'600, 900); // ~15 s
    recorder.Record(MakeRow(t += 200 * kMs));
    assert(Ready(recorder, AutoStall) == 1);
    FreeAll(recorder);

    // The median rule: at 50 ms flips, 150 ms is under 4x the median, 250 ms is over.
    Recorder slow(1024, true);
    t = Steady(slow, 6'000 * kMs, 40, 50 * kMs);
    slow.Record(MakeRow(t += 150 * kMs));
    assert(Ready(slow, AutoStall) == 0);
    t = Steady(slow, t + 50 * kMs, 40, 50 * kMs);
    slow.Record(MakeRow(t += 250 * kMs));
    assert(Ready(slow, AutoStall) == 1);
}

void TestReset() {
    Recorder recorder(1024, true);
    std::int64_t t = Steady(recorder, 6'000 * kMs, 30);
    recorder.Record(MakeRow(t += 16'600, UpscalerReset));
    const std::uint64_t reset_seq = recorder.LastSeq();
    t = Steady(recorder, t + 16'600, 60); // 1 s: not yet
    assert(Ready(recorder, AutoReset) == 0);
    t = Steady(recorder, t + 16'600, 70); // past 2 s quiet
    auto* request = Find(recorder, AutoReset);
    assert(request && request->marker_seq == reset_seq);
    FreeAll(recorder);

    // A UI frame 1 s before: no dump.
    Recorder before(1024, true);
    t = Steady(before, 6'000 * kMs, 30);
    before.Record(MakeRow(t += 16'600, StretchedOrUiFrame));
    t = Steady(before, t + 16'600, 60);
    before.Record(MakeRow(t += 16'600, UpscalerReset));
    Steady(before, t + 16'600, 300);
    assert(Ready(before, AutoReset) == 0);

    // A movie frame 1 s after: cancelled.
    Recorder after(1024, true);
    t = Steady(after, 6'000 * kMs, 30);
    after.Record(MakeRow(t += 16'600, UpscalerReset));
    t = Steady(after, t + 16'600, 60);
    after.Record(MakeRow(t += 16'600, MovieFrame));
    Steady(after, t + 16'600, 300);
    assert(Ready(after, AutoReset) == 0);

    // In the grace period: nothing.
    Recorder early(1024, true);
    early.Record(MakeRow(1'000 * kMs, UpscalerReset));
    Steady(early, 1'016 * kMs, 300);
    assert(Ready(early, AutoReset) == 0);
}

void TestSkippedAndCap() {
    Recorder recorder(1024, true);
    recorder.Record(MakeRow(1'000 * kMs, DrawSkippedPendingCompile)); // grace
    std::int64_t t = Steady(recorder, 1'016 * kMs, 400); // on to 7.6 s
    assert(Ready(recorder, AutoSkipped) == 0);
    recorder.Record(MakeRow(t += 16'600, DrawSkippedPendingCompile));
    assert(Ready(recorder, AutoSkipped) == 1);
    assert(Find(recorder, AutoSkipped)->marker_seq == recorder.LastSeq());
    // Its own 60 s cooldown (not 15 s), then the cap of 30 automatic dumps (steady flips
    // between: no stall dumps).
    recorder.Record(MakeRow(t += 16'600, DrawSkippedPendingCompile));
    assert(Ready(recorder, AutoSkipped) == 1);
    FreeAll(recorder);
    t = Steady(recorder, t + 16'600, 1200); // 20 s
    recorder.Record(MakeRow(t += 16'600, DrawSkippedPendingCompile));
    assert(Ready(recorder, AutoSkipped) == 0);
    int posted = 1;
    for (int i = 0; i < 40; ++i) {
        FreeAll(recorder);
        t = Steady(recorder, t + 16'600, 3620); // 60.1 s
        recorder.Record(MakeRow(t += 16'600, DrawSkippedPendingCompile));
        posted += Ready(recorder, AutoSkipped);
    }
    assert(posted == Recorder::kMaxAutoDumps);
    assert(recorder.AutoDumps() == Recorder::kMaxAutoDumps);

    // No automatic dumps with GOW3_FLIGHT_AUTO=0.
    Recorder off(1024, false);
    off.Record(MakeRow(6'000 * kMs));
    off.Record(MakeRow(6'016 * kMs, DrawSkippedPendingCompile));
    assert(Ready(off, AutoSkipped) == 0);
}

void TestCamjump() {
    // A 90 degree turn without a reset: dumped 2 flips later, marked at the turn.
    Recorder recorder(1024, true);
    std::int64_t t = Steady(recorder, 6'000 * kMs, 40);
    recorder.Record(MakeRow(t += 16'600, 0, 1.0f, 0.0f));
    const std::uint64_t cut = recorder.LastSeq();
    recorder.Record(MakeRow(t += 16'600, 0, 1.0f, 0.0f));
    assert(Ready(recorder, AutoCamjump) == 0);
    recorder.Record(MakeRow(t += 16'600, 0, 1.0f, 0.0f));
    auto* request = Find(recorder, AutoCamjump);
    assert(request && request->marker_seq == cut);

    // The same turn with a reset in the next flip: noticed, no dump.
    Recorder noticed(1024, true);
    t = Steady(noticed, 6'000 * kMs, 40);
    noticed.Record(MakeRow(t += 16'600, 0, 1.0f, 0.0f));
    noticed.Record(MakeRow(t += 16'600, HistoryInvalidated, 1.0f, 0.0f));
    // The turn back to +z comes right after the invalidation: noticed too.
    t = Steady(noticed, t + 16'600, 5);
    assert(Ready(noticed, AutoCamjump) == 0);

    // A small turn (10 degrees) is not a cut; a jump in position over 8x the median is.
    Recorder small(1024, true);
    t = Steady(small, 6'000 * kMs, 40);
    small.Record(MakeRow(t += 16'600, 0, std::sin(0.1745f), std::cos(0.1745f)));
    small.Record(MakeRow(t += 16'600, 0, std::sin(0.1745f), std::cos(0.1745f)));
    small.Record(MakeRow(t += 16'600, 0, std::sin(0.1745f), std::cos(0.1745f)));
    assert(Ready(small, AutoCamjump) == 0);
    Row moved = MakeRow(t += 16'600, 0, std::sin(0.1745f), std::cos(0.1745f));
    moved.gauges[CamX] = 50.0f;
    small.Record(moved);
    moved.t_us = t += 16'600;
    small.Record(moved);
    moved.t_us = t += 16'600;
    small.Record(moved);
    assert(Ready(small, AutoCamjump) == 1);

    // No camera (zero gauges): never compared.
    float deg, move;
    assert(!CameraChange(Row{}, MakeRow(0), deg, move));
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value); // "" removes it
#else
    if (value[0]) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

// Loading frames, movies and UI-only frames silence stall, reset and skipped for 2 s.
void TestQuietFrames() {
    // A level load: 3 s with loading frames and no flip, then flips again. No stall, nor for a
    // hitch 1 s later; one 2.5 s later is a stall.
    Recorder load(1024, true);
    std::int64_t t = Steady(load, 6'000 * kMs, 40);
    load.Record(MakeRow(t += 3'000 * kMs, LoadingFrame));
    assert(Ready(load, AutoStall) == 0);
    t = Steady(load, t + 16'600, 60);
    load.Record(MakeRow(t += 200 * kMs));
    assert(Ready(load, AutoStall) == 0);
    t = Steady(load, t + 16'600, 150);
    load.Record(MakeRow(t += 200 * kMs));
    assert(Ready(load, AutoStall) == 1);

    // Right after a movie ends (its last MovieFrame row 1 s before the interval): no stall.
    Recorder movie(1024, true);
    t = 6'000 * kMs;
    for (int i = 0; i < 40; ++i) {
        movie.Record(MakeRow(t += 16'600, MovieFrame));
    }
    t = Steady(movie, t + 16'600, 60);
    movie.Record(MakeRow(t += 500 * kMs));
    assert(Ready(movie, AutoStall) == 0);

    // A reset with a loading frame 1 s before: no dump.
    Recorder reset(1024, true);
    t = Steady(reset, 6'000 * kMs, 30);
    reset.Record(MakeRow(t += 16'600, LoadingFrame));
    t = Steady(reset, t + 16'600, 60);
    reset.Record(MakeRow(t += 16'600, UpscalerReset));
    Steady(reset, t + 16'600, 300);
    assert(Ready(reset, AutoReset) == 0);

    // Skipped draws during a movie, or 1 s after a UI-only frame: no dump; 3 s after: dumped.
    Recorder skipped(1024, true);
    t = Steady(skipped, 6'000 * kMs, 40);
    skipped.Record(MakeRow(t += 16'600, MovieFrame | DrawSkippedPendingCompile));
    t = Steady(skipped, t + 16'600, 30);
    skipped.Record(MakeRow(t += 16'600, StretchedOrUiFrame));
    t = Steady(skipped, t + 16'600, 60);
    skipped.Record(MakeRow(t += 16'600, DrawSkippedPendingCompile));
    assert(Ready(skipped, AutoSkipped) == 0);
    t = Steady(skipped, t + 16'600, 120);
    skipped.Record(MakeRow(t += 16'600, DrawSkippedPendingCompile));
    assert(Ready(skipped, AutoSkipped) == 1);
}

// `n` flips with the camera moving `step` units along x per flip from `x`; returns the time.
std::int64_t Walk(Recorder& recorder, std::int64_t t, int n, float& x, float step) {
    for (int i = 0; i < n; ++i) {
        Row row = MakeRow(t += 16'600);
        row.gauges[CamX] = x += step;
        recorder.Record(row);
    }
    return t;
}

// The relative move rule with small world units, via the env override GOW3_FLIGHT_CUT_MOVE=0.25
// (the default floor is 1.0, like the upscaler's detector): at 0.01 per flip, 0.2 is under the
// 0.25 floor (no cut) and 0.5 is a cut.
void TestCutSmallUnits() {
    SetEnv("GOW3_FLIGHT_CUT_MOVE", "0.25");
    Recorder recorder(1024, true);
    float x = 0.0f;
    std::int64_t t = Walk(recorder, 6'000 * kMs, 40, x, 0.01f);
    t = Walk(recorder, t, 1, x, 0.2f);
    t = Walk(recorder, t, 5, x, 0.01f);
    assert(Ready(recorder, AutoCamjump) == 0);
    t = Walk(recorder, t, 1, x, 0.5f);
    const std::uint64_t cut = recorder.LastSeq();
    Walk(recorder, t, 3, x, 0.01f);
    auto* request = Find(recorder, AutoCamjump);
    assert(request && request->marker_seq == cut);

    // At 0.5 per flip (a fast camera), 0.5 is no cut.
    Recorder fast(1024, true);
    x = 0.0f;
    t = Walk(fast, 6'000 * kMs, 40, x, 0.5f);
    Walk(fast, t, 3, x, 0.5f);
    assert(Ready(fast, AutoCamjump) == 0);
    SetEnv("GOW3_FLIGHT_CUT_MOVE", "");

    // Defaults match the upscaler's detector (45 degrees, 1.0 floor, 16x): the 25 Oct walking
    // frame (0.73 units after ~0.05 steps) is no cut, 5 units is.
    assert(Recorder::kCutMove == 1.0f && Recorder::kCutDeg == 45.0f);
    Recorder walking(1024, true);
    x = 0.0f;
    t = Walk(walking, 6'000 * kMs, 40, x, 0.05f);
    t = Walk(walking, t, 1, x, 0.73f);
    t = Walk(walking, t, 5, x, 0.05f);
    assert(Ready(walking, AutoCamjump) == 0);
    t = Walk(walking, t, 1, x, 5.0f);
    Walk(walking, t, 3, x, 0.05f);
    assert(Find(walking, AutoCamjump));

    // The environment overrides; garbage or non-positive values keep the defaults.
    assert(recorder.CutDeg() == Recorder::kCutDeg);
    SetEnv("GOW3_FLIGHT_CUT_MOVE", "0.05");
    SetEnv("GOW3_FLIGHT_CUT_DEG", "12.5");
    {
        Recorder tuned(64, true);
        assert(tuned.CutMove() == 0.05f && tuned.CutDeg() == 12.5f);
    }
    SetEnv("GOW3_FLIGHT_CUT_MOVE", "abc");
    SetEnv("GOW3_FLIGHT_CUT_DEG", "-3");
    {
        Recorder bad(64, true);
        assert(bad.CutMove() == Recorder::kCutMove && bad.CutDeg() == Recorder::kCutDeg);
    }
    SetEnv("GOW3_FLIGHT_CUT_MOVE", "");
    SetEnv("GOW3_FLIGHT_CUT_DEG", "");
}

// Overhead: Recorder::Record with gameplay rows (camera moving and turning, every counter
// moving, no anomaly), and the whole OnFlip (no Vulkan needed: it reads atomics and
// Gow3Settings). `step` 0.05 stays under the cut floor; 0.3 runs the 32-value median every flip.
double BenchRecord(float step) {
    constexpr int kN = 200'000;
    Recorder recorder(1024, true);
    Row rows[2] = {MakeRow(0, 0, std::sin(0.0f), std::cos(0.0f)),
                   MakeRow(0, 0, std::sin(0.0035f), std::cos(0.0035f))}; // 0.2 degree turns
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) {
        Row row = rows[i & 1];
        row.t_us = 6'000 * kMs + std::int64_t(i) * 16'600;
        row.gauges[CamX] = step * float(i);
        row.cum[CDraws] = 3000ull * std::uint64_t(i);
        row.cum[CImageUploadBytes] = 100'000ull * std::uint64_t(i);
        recorder.Record(row);
    }
    const double ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kN;
    assert(recorder.LastSeq() == kN && recorder.AutoDumps() == 0);
    return ns;
}

void TestOverhead() {
    const double walk = BenchRecord(0.05f);
    const double median = BenchRecord(0.3f);
    std::printf("flight-test: Record %.1f ns/call (walking), %.1f ns/call (median every flip)\n",
                walk, median);
    assert(walk < 2000.0 && median < 2000.0);

    // OnFlip: the first call allocates the ring and starts the dump thread (no trigger file, no
    // automatic dumps here, output into a temporary folder that stays empty).
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "gow3-flight-bench";
    SetEnv("GOW3_FLIGHT_DIR", dir.string().c_str());
    SetEnv("GOW3_FLIGHT_AUTO", "0");
    OnFlip(false, false);
    constexpr int kN = 200'000;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) {
        Count(SkippedDraws);
        Gauge(CamX, float(i));
        OnFlip(false, false);
    }
    const double ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kN;
    std::printf("flight-test: OnFlip %.1f ns/call (with one Count and one Gauge)\n", ns);
    assert(ns < 2000.0);
}

void TestLabelsAndFiles() {
    // Same as tools/session_recorder.py flight_label(): other characters dropped.
    assert(SanitizeLabel("F7 Flash!!\n") == "f7flash");
    assert(SanitizeLabel("flash") == "flash");
    assert(SanitizeLabel("a b_c-") == "ab_c-");
    assert(SanitizeLabel("") == "manual");
    assert(SanitizeLabel("\n") == "manual");
    assert(SanitizeLabel("../..\\x") == "x");
    assert(SanitizeLabel("mark-3_ghost") == "mark-3_ghost");
    assert(SanitizeLabel("abcdefghijklmnopqrstuvwxyz0123") == "abcdefghijklmnopqrstuvwx");
    assert(SanitizeLabel("first\nsecond") == "first");

    const std::string name = FileName(1'700'000'000'123, "auto-stall");
    assert(name.rfind("flight_", 0) == 0);
    assert(name.size() == std::string("flight_YYYYmmdd-HHMMSS-mmm_auto-stall.csv").size());
    assert(name.find("-123_auto-stall.csv") != std::string::npos);

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "gow3-flight-test" / "nested";
    std::error_code ec;
    fs::remove_all(dir.parent_path(), ec);
    std::string error;
    assert(WriteAtomic(dir.string(), name, "hello\n", error));
    assert(fs::exists(dir / name));
    assert(!fs::exists(dir / (name + ".tmp")));
    std::ifstream in(dir / name, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(text == "hello\n");
    in.close();
    // Replacing an existing dump of the same name works too.
    assert(WriteAtomic(dir.string(), name, "again\n", error));
    // A folder that cannot be made: false and an error, no exception.
    const fs::path blocked = dir / "file";
    std::ofstream(blocked) << "x";
    assert(!WriteAtomic((blocked / "sub").string(), name, "x", error) && !error.empty());
    fs::remove_all(dir.parent_path(), ec);
}
} // namespace

int main() {
    TestRingWrap();
    TestCsv();
    TestStall();
    TestReset();
    TestSkippedAndCap();
    TestCamjump();
    TestQuietFrames();
    TestCutSmallUnits();
    TestLabelsAndFiles();
    TestOverhead();
    std::printf("flight-test: all passed\n");
    return 0;
}
