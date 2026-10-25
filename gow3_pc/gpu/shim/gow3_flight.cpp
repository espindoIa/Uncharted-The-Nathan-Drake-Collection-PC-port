// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 25 Oct 2026: the engine flight recorder (gow3_flight.h). OnFlip runs on the present thread
// once per presented flip and must stay far below a frame: it reads relaxed atomics, writes one
// row into a ring allocated at the first flip and checks the anomaly rules; no lock, allocation,
// file access or printing. A dump thread started at the first flip does the rest: it polls the
// trigger file and the request slots every 50 ms, copies the rows out of the ring (a per-slot
// sequence lock drops a row being rewritten) and writes the CSV. Waiting for the flips after a
// request happens there too, so a request still gets written 2 s later if the flips stop.
#include "gow3_flight.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "gow3_settings.h"
#include "gow3_toggles.h"
#include "gow3_trigger_files.h"

namespace Gow3Flight {

namespace {
constexpr const char* kEventNames[kEventCount] = {
    "UpscalerReset", "HistoryInvalidated", "NoCamera", "DrawSkippedPendingCompile",
    "PipelineCompiled", "UpscalerFeatureCreated", "StretchedOrUiFrame", "FrameGenOnOff",
    "TextureGcEvict", "MovieFrame", "LoadingFrame"};
constexpr const char* kAutoNames[kAutoKindCount] = {"stall", "reset", "skipped", "camjump"};
constexpr const char* kColumns[] = {
    "seq", "unix_ms", "t_ms", "dt_ms", "marker", "events", "event_names", "event_detail",
    "d_draws", "d_dispatches", "d_gpu_frames", "d_compiles", "d_skipped_draws", "d_gc_evictions",
    "d_images_registered", "d_image_upload_mb", "d_buffer_upload_mb", "d_copy_mb",
    "d_submissions", "d_gc_ms", "d_frame_wait_ms", "d_present_ms",
    "arena_mb", "image_mb", "images_live",
    "cam_x", "cam_y", "cam_z", "fwd_x", "fwd_y", "fwd_z", "proj_x", "proj_y", "jit_x", "jit_y",
    "out_w", "out_h", "lod_bias", "scene_w", "scene_h", "upscaler", "preset", "dlss_model",
    "fg_on", "cam_jump_deg", "cam_move", "presents"};

// A positive number from the environment, else `fallback`.
float EnvFloat(const char* name, float fallback) {
    const char* text = std::getenv(name);
    if (!text || !text[0]) {
        return fallback;
    }
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    return end != text && std::isfinite(value) && value > 0.0f ? value : fallback;
}

float Median(const float* values, std::uint32_t count) {
    float tmp[32];
    const std::uint32_t n = std::min<std::uint32_t>(count, 32);
    std::copy_n(values, n, tmp);
    std::nth_element(tmp, tmp + n / 2, tmp + n);
    return tmp[n / 2];
}
} // namespace

const char* AutoKindName(int kind) {
    return kind >= 0 && kind < kAutoKindCount ? kAutoNames[kind] : "manual";
}

Recorder::Recorder(std::uint32_t ring_size, bool auto_dumps)
    : size{ring_size}, auto_on{auto_dumps},
      cut_move{EnvFloat("GOW3_FLIGHT_CUT_MOVE", kCutMove)},
      cut_deg{EnvFloat("GOW3_FLIGHT_CUT_DEG", kCutDeg)}, ring(ring_size) {
    std::fill(std::begin(last_auto_us), std::end(last_auto_us), -kSkippedCooldownUs);
}

void Recorder::Record(Row row) {
    row.seq = last_seq.load(std::memory_order_relaxed) + 1;
    if (have_prev && row.gauges[FgOn] != prev.gauges[FgOn]) {
        row.events |= FrameGenOnOff;
    }
    // Sequence lock: odd while the slot is rewritten (Collect drops it then).
    Slot& slot = ring[row.seq % size];
    const std::uint64_t version = slot.version.load(std::memory_order_relaxed);
    slot.version.store(version + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::uint64_t words[sizeof(Row) / 8];
    std::memcpy(words, &row, sizeof(Row));
    for (std::size_t i = 0; i < sizeof(Row) / 8; ++i) {
        slot.words[i].store(words[i], std::memory_order_relaxed);
    }
    slot.version.store(version + 2, std::memory_order_release);
    last_seq.store(row.seq, std::memory_order_release);

    const std::int64_t t = row.t_us;
    const bool grace = t < kGraceUs;
    // A movie, a UI-only frame (menu, loading screen) or the presenter's own loading frame: a
    // reset, a long interval or a skipped draw next to one is expected, not a glitch. The row's
    // events are those since the previous flip, so they cover the interval before it too.
    const bool quiet_frame = row.events & (StretchedOrUiFrame | MovieFrame | LoadingFrame);
    if (quiet_frame) {
        reset_pending = false;
    }
    if (auto_on && have_prev) {
        // stall: an interval over max(60 ms, 4x the median of the last 32), not across or within
        // 2 s after a quiet frame (a level load is one long interval between flips).
        const float dt = float(t - prev.t_us) / 1000.0f;
        if (dt > 60.0f && !grace && !quiet_frame && prev.t_us - last_quiet_us >= kQuietUs &&
            dt_n >= 8 && dt > std::max(60.0f, 4.0f * Median(dts, dt_n))) {
            PostAuto(AutoStall, row.seq, row.seq + kFramesAfter, t);
        }
        dts[dt_n++ % 32] = dt;
        // reset: posted 2 s later if no quiet frame came meanwhile (or 2 s before).
        if (reset_pending && t - reset_us >= kQuietUs) {
            reset_pending = false;
            PostAuto(AutoReset, reset_seq, row.seq + kFramesAfter, t);
        }
        if ((row.events & UpscalerReset) && !reset_pending && !grace && !quiet_frame &&
            t - last_quiet_us >= kQuietUs && AutoAllowed(AutoReset, t)) {
            reset_pending = true;
            reset_seq = row.seq;
            reset_us = t;
        }
        // skipped: a draw dropped while its pipeline compiled, in a gameplay frame (no quiet
        // frame in the last 2 s); counted in every row, dumped at most once a minute.
        if ((row.events & DrawSkippedPendingCompile) && !grace && !quiet_frame &&
            t - last_quiet_us >= kQuietUs) {
            PostAuto(AutoSkipped, row.seq, row.seq + kFramesAfter, t);
        }
        // camjump: a cut that no reset or history invalidation in the flip before, the flip
        // itself or the 2 after went with (the gauges and the events can be a flip apart).
        constexpr std::uint32_t noticed = UpscalerReset | HistoryInvalidated;
        if (camjump_left > 0) {
            if (row.events & noticed) {
                camjump_left = 0;
            } else if (--camjump_left == 0) {
                PostAuto(AutoCamjump, camjump_seq, row.seq + kFramesAfter, t);
            }
        }
        float deg, move;
        if (CameraChange(prev, row, deg, move)) {
            // The move rule is relative (8x the median of the last 32 flips): the games' world
            // units are not known (frame_gen_camera.h). The floor only keeps a still camera's
            // jitter-sized moves out.
            bool jump = deg > cut_deg;
            if (!jump && move > cut_move && move_n >= 8) {
                jump = move > 8.0f * Median(moves, move_n);
            }
            moves[move_n++ % 32] = move;
            if (jump && camjump_left == 0 && !grace && !((row.events | prev.events) & noticed) &&
                AutoAllowed(AutoCamjump, t)) {
                camjump_left = 2;
                camjump_seq = row.seq;
            }
        }
    }
    if (quiet_frame) {
        last_quiet_us = t;
    }
    prev = row;
    have_prev = true;
}

bool Recorder::AutoAllowed(int kind, std::int64_t t_us) const {
    const std::int64_t cooldown = kind == AutoSkipped ? kSkippedCooldownUs : kCooldownUs;
    return auto_on && auto_dumps_done < kMaxAutoDumps && t_us - last_auto_us[kind] >= cooldown;
}

void Recorder::PostAuto(int kind, std::uint64_t marker_seq, std::uint64_t fire_seq,
                        std::int64_t t_us) {
    if (AutoAllowed(kind, t_us) && Post(kind, nullptr, marker_seq, fire_seq, t_us)) {
        last_auto_us[kind] = t_us;
        ++auto_dumps_done;
    }
}

bool Recorder::Post(int kind, const char* label, std::uint64_t marker_seq, std::uint64_t fire_seq,
                    std::int64_t now_us) {
    for (Request& request : requests) {
        int expected = 0;
        if (!request.state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
            continue;
        }
        request.kind = kind;
        std::snprintf(request.label, sizeof(request.label), "%s", label ? label : "");
        request.marker_seq = marker_seq;
        request.fire_seq = fire_seq;
        request.posted_us = now_us;
        request.state.store(2, std::memory_order_release);
        return true;
    }
    return false;
}

std::vector<Row> Recorder::Collect(std::uint64_t from, std::uint64_t to) const {
    std::vector<Row> rows;
    rows.reserve(to >= from ? to - from + 1 : 0);
    for (std::uint64_t seq = std::max<std::uint64_t>(from, 1); seq <= to; ++seq) {
        const Slot& slot = ring[seq % size];
        const std::uint64_t before = slot.version.load(std::memory_order_acquire);
        if (before & 1) {
            continue;
        }
        std::uint64_t words[sizeof(Row) / 8];
        for (std::size_t i = 0; i < sizeof(Row) / 8; ++i) {
            words[i] = slot.words[i].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.version.load(std::memory_order_relaxed) != before) {
            continue;
        }
        Row row;
        std::memcpy(&row, words, sizeof(Row));
        if (row.seq == seq) {
            rows.push_back(row);
        }
    }
    return rows;
}

bool CameraChange(const Row& a, const Row& b, float& deg, float& move) {
    deg = move = 0.0f;
    const auto length = [](const Row& r) {
        return std::sqrt(r.gauges[FwdX] * r.gauges[FwdX] + r.gauges[FwdY] * r.gauges[FwdY] +
                         r.gauges[FwdZ] * r.gauges[FwdZ]);
    };
    const float la = length(a), lb = length(b);
    // No camera yet (zero gauges) or garbage: not compared.
    if (!(la > 0.5f && la < 2.0f && lb > 0.5f && lb < 2.0f)) {
        return false;
    }
    const float dot = (a.gauges[FwdX] * b.gauges[FwdX] + a.gauges[FwdY] * b.gauges[FwdY] +
                       a.gauges[FwdZ] * b.gauges[FwdZ]) / (la * lb);
    deg = std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.29578f;
    const float dx = b.gauges[CamX] - a.gauges[CamX], dy = b.gauges[CamY] - a.gauges[CamY],
                dz = b.gauges[CamZ] - a.gauges[CamZ];
    move = std::sqrt(dx * dx + dy * dy + dz * dz);
    return std::isfinite(deg) && std::isfinite(move);
}

int CsvColumns() {
    return int(std::size(kColumns));
}

std::string Csv(const std::vector<Row>& rows, std::uint64_t marker_seq, const std::string& reason,
                std::int64_t unix_ms, std::uint32_t ring_size, const std::string& build,
                std::int64_t start_unix_ms) {
    std::string out;
    out.reserve(256 + rows.size() * 400);
    char line[2048]; // a row is under 1 KB even with every event named
    std::snprintf(line, sizeof(line), "# flight v1 unix_ms=%lld reason=%s frames=%zu ring=%u build=%s\n",
                  static_cast<long long>(unix_ms), reason.c_str(), rows.size(), ring_size,
                  build.c_str());
    out += line;
    for (int i = 0; i < CsvColumns(); ++i) {
        out += i ? "," : "";
        out += kColumns[i];
    }
    out += '\n';
    // The marker goes on the row nearest the requested one (it may have been dropped).
    std::uint64_t marked = 0, best = ~0ull;
    for (const Row& row : rows) {
        const std::uint64_t distance = row.seq > marker_seq ? row.seq - marker_seq : marker_seq - row.seq;
        if (distance < best) {
            best = distance;
            marked = row.seq;
        }
    }
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const Row& row = rows[r];
        const Row* prev = r ? &rows[r - 1] : nullptr;
        std::string names;
        for (int e = 0; e < kEventCount; ++e) {
            if (row.events & (1u << e)) {
                names += names.empty() ? "" : "|";
                names += kEventNames[e];
            }
        }
        const auto delta = [&](int id) {
            return prev ? double(row.cum[id] - prev->cum[id]) : 0.0;
        };
        float deg = 0.0f, move = 0.0f;
        if (prev) {
            CameraChange(*prev, row, deg, move);
        }
        const double t_ms = double(row.t_us) / 1000.0;
        const float* g = row.gauges;
        int n = std::snprintf(
            line, sizeof(line),
            "%llu,%lld,%.3f,%.3f,%d,0x%x,%s,%d,"
            "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.3f,%.3f,%.3f,%.0f,%.3f,%.3f,%.3f,"
            "%.1f,%.1f,%llu,"
            "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,"
            "%.0f,%.0f,%.4g,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.3f,%.6g,%.0f\n",
            static_cast<unsigned long long>(row.seq),
            static_cast<long long>(start_unix_ms + row.t_us / 1000), t_ms,
            prev ? double(row.t_us - prev->t_us) / 1000.0 : 0.0, row.seq == marked ? 1 : 0,
            row.events, names.c_str(), row.detail,
            delta(CDraws), delta(CDispatches), delta(CGpuFrames), delta(CCompiles),
            delta(CSkippedDraws), delta(CGcEvictions), delta(CImagesRegistered),
            delta(CImageUploadBytes) / 1e6, delta(CBufferUploadBytes) / 1e6, delta(CCopyBytes) / 1e6,
            delta(CSubmissions), delta(CGcNs) / 1e6, delta(CFrameWaitNs) / 1e6,
            delta(CPresentNs) / 1e6,
            double(row.cum[CArenaBytes]) / 1048576.0, double(row.cum[CImageBytes]) / 1048576.0,
            static_cast<unsigned long long>(row.cum[CImagesLive]),
            g[CamX], g[CamY], g[CamZ], g[FwdX], g[FwdY], g[FwdZ], g[ProjX], g[ProjY], g[JitX], g[JitY],
            g[OutW], g[OutH], g[LodBias], g[SceneW], g[SceneH], g[UpscalerMode], g[PresetId],
            g[DlssModel], g[FgOn], deg, move, g[Presents]);
        if (n > 0) {
            out.append(line, std::min<std::size_t>(std::size_t(n), sizeof(line) - 1));
        }
    }
    return out;
}

std::string SanitizeLabel(const std::string& text) {
    std::string out;
    for (const char raw : text) {
        if (raw == '\n' || raw == '\r') {
            break; // the first line only
        }
        // Other characters are dropped, as tools/session_recorder.py flight_label() expects: it
        // finds the dump by this reason.
        const char c = char(std::tolower(static_cast<unsigned char>(raw)));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out += c;
        }
        if (out.size() >= 24) {
            break;
        }
    }
    return out.empty() ? "manual" : out;
}

std::string FileName(std::int64_t unix_ms, const std::string& reason) {
    const std::time_t seconds = std::time_t(unix_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    char name[96];
    std::snprintf(name, sizeof(name), "flight_%s-%03d_%s.csv", stamp, int(unix_ms % 1000),
                  reason.c_str());
    return name;
}

bool WriteAtomic(const std::string& dir, const std::string& name, const std::string& text,
                 std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    const fs::path final_path = fs::path(dir) / name;
    fs::path tmp_path = final_path;
    tmp_path += ".tmp";
    FILE* f = std::fopen(tmp_path.string().c_str(), "wb");
    if (!f) {
        error = "cannot create " + tmp_path.string();
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed) {
        fs::remove(tmp_path, ec);
        error = "cannot write " + tmp_path.string();
        return false;
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        error = "cannot rename to " + final_path.string();
        return false;
    }
    return true;
}

// ---- The process-wide recorder. ----

namespace {
const auto kStart = std::chrono::steady_clock::now();
const std::int64_t kStartUnixMs =
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
std::atomic<bool> g_busy{false};
Recorder* g_recorder = nullptr; // set once, under g_busy
bool g_start_tried = false;

std::int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                 kStart)
        .count();
}

std::string OutputDir() {
    namespace fs = std::filesystem;
    if (const char* dir = std::getenv("GOW3_FLIGHT_DIR"); dir && dir[0]) {
        return dir;
    }
    if (const char* dumps = std::getenv("GOW3_DUMP_DIR"); dumps && dumps[0]) {
        fs::path path(dumps);
        if (!path.has_filename()) {
            path = path.parent_path(); // "dumps/" -> "dumps"
        }
        return (path.parent_path() / "flight").string();
    }
    return "flight";
}

std::string BuildName() {
    namespace fs = std::filesystem;
    fs::path exe;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        exe = fs::path(std::wstring(buffer, length));
    }
#else
    std::error_code link_ec;
    exe = fs::read_symlink("/proc/self/exe", link_ec);
#endif
    std::error_code ec;
    const auto bytes = exe.empty() ? 0 : fs::file_size(exe, ec);
    std::string name = exe.empty() ? std::string("unknown") : exe.filename().string();
    std::replace(name.begin(), name.end(), ' ', '_'); // the comment line is split on spaces
    return name + "/" + std::to_string(ec ? 0 : bytes);
}

void Dump(Recorder& recorder, Recorder::Request& request, const std::string& dir,
          const std::string& build) {
    const std::uint64_t last = recorder.LastSeq();
    const std::uint64_t to = std::min(last, request.fire_seq);
    const std::uint64_t from = to >= recorder.RingSize() ? to - recorder.RingSize() + 1 : 1;
    const std::string reason = request.kind < 0 ? std::string(request.label)
                                                : std::string("auto-") + AutoKindName(request.kind);
    const std::int64_t unix_ms = kStartUnixMs + NowUs() / 1000;
    const auto rows = recorder.Collect(from, to);
    const std::string name = FileName(unix_ms, reason);
    std::string error;
    if (WriteAtomic(dir, name, Csv(rows, request.marker_seq, reason, unix_ms, recorder.RingSize(),
                                   build, kStartUnixMs),
                    error)) {
        std::printf("Flight: wrote %s (%zu flips)\n", name.c_str(), rows.size());
    } else {
        std::printf("Flight: dump %s failed: %s\n", reason.c_str(), error.c_str());
    }
    std::fflush(stdout);
}

void DumpThread(Recorder* recorder, std::string dir) {
    const int trigger = Gow3TriggerFiles::Watch("GOW3_FLIGHT_TRIGGER");
    const std::string build = BuildName();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        try {
            if (const auto text = Gow3TriggerFiles::Take(trigger)) {
                const std::uint64_t now = recorder->LastSeq();
                if (!recorder->Post(-1, SanitizeLabel(*text).c_str(), now,
                                    now + Recorder::kFramesAfter, NowUs())) {
                    std::printf("Flight: dump request dropped (4 already pending)\n");
                }
            }
            for (int i = 0; i < Recorder::kMaxRequests; ++i) {
                auto& request = recorder->RequestSlot(i);
                if (request.state.load(std::memory_order_acquire) != 2) {
                    continue;
                }
                // The flips after the request, or what came within 2 s if the game stopped.
                if (recorder->LastSeq() < request.fire_seq && NowUs() - request.posted_us < 2'000'000) {
                    continue;
                }
                Dump(*recorder, request, dir, build);
                request.state.store(0, std::memory_order_release);
            }
        } catch (const std::exception& e) {
            std::printf("Flight: dump failed: %s\n", e.what());
            std::fflush(stdout);
            for (int i = 0; i < Recorder::kMaxRequests; ++i) {
                auto& request = recorder->RequestSlot(i);
                if (request.state.load(std::memory_order_acquire) == 2) {
                    request.state.store(0, std::memory_order_release);
                }
            }
        }
    }
}

Recorder* Start() {
    try {
        const char* frames_env = std::getenv("GOW3_FLIGHT_FRAMES");
        const long frames = frames_env && frames_env[0] ? std::strtol(frames_env, nullptr, 10) : 1024;
        const auto ring = std::uint32_t(std::clamp(frames, 64l, 8192l));
        const char* auto_env = std::getenv("GOW3_FLIGHT_AUTO");
        const bool auto_dumps = !(auto_env && auto_env[0] == '0');
        auto recorder = std::make_unique<Recorder>(ring, auto_dumps);
        const std::string dir = OutputDir();
        std::thread(DumpThread, recorder.get(), dir).detach();
        std::printf("Flight: recording the last %u flips; dumps to %s (automatic %s)\n", ring,
                    dir.c_str(), auto_dumps ? "on" : "off");
        std::fflush(stdout);
        return recorder.release(); // lives until the process ends (the dump thread uses it)
    } catch (const std::exception& e) {
        std::printf("Flight: off (%s)\n", e.what());
        std::fflush(stdout);
        return nullptr;
    }
}
} // namespace

void OnFlip(bool movie, bool frame_gen) {
    // A second caller at the same time drops its row instead of waiting.
    if (!kEnabled || g_busy.exchange(true, std::memory_order_acquire)) {
        return;
    }
    if (!g_recorder && !g_start_tried) [[unlikely]] {
        g_start_tried = true;
        g_recorder = Start();
    }
    if (Recorder* recorder = g_recorder) {
        constexpr auto relaxed = std::memory_order_relaxed;
        Row row{};
        row.t_us = NowUs();
        row.events = g_events.exchange(0, relaxed) | (movie ? std::uint32_t(MovieFrame) : 0u);
        row.detail = g_event_detail.exchange(0, relaxed);
        // Gow3Stats counters that nothing resets (the Frame stats printer zeroes other ones).
        row.cum[CDraws] = Gow3Stats::draws.load(relaxed);
        row.cum[CDispatches] = Gow3Stats::dispatches.load(relaxed);
        row.cum[CGpuFrames] = Gow3Stats::gpu_frames.load(relaxed);
        row.cum[CCompiles] = g_counters[Compiles].load(relaxed);
        row.cum[CSkippedDraws] = g_counters[SkippedDraws].load(relaxed);
        row.cum[CGcEvictions] = g_counters[GcEvictions].load(relaxed);
        row.cum[CImagesRegistered] = Gow3Stats::images_registered.load(relaxed);
        row.cum[CImageUploadBytes] = Gow3Stats::image_upload_bytes.load(relaxed);
        row.cum[CBufferUploadBytes] = Gow3Stats::buffer_upload_bytes.load(relaxed);
        row.cum[CCopyBytes] = Gow3Stats::copy_bytes.load(relaxed);
        row.cum[CSubmissions] = Gow3Stats::submissions.load(relaxed);
        row.cum[CGcNs] = Gow3Stats::t_gc.load(relaxed);
        row.cum[CFrameWaitNs] = Gow3Stats::frame_wait_ns.load(relaxed);
        row.cum[CPresentNs] = Gow3Stats::present_ns.load(relaxed);
        row.cum[CArenaBytes] = Gow3Stats::arena_memory_bytes.load(relaxed);
        row.cum[CImageBytes] = Gow3Stats::image_bytes.load(relaxed);
        row.cum[CImagesLive] = Gow3Stats::images_live.load(relaxed);
        for (int i = 0; i < kHookGaugeCount; ++i) {
            const std::uint32_t bits = g_gauges[i].load(relaxed);
            std::memcpy(&row.gauges[i], &bits, sizeof(bits));
        }
        const auto& settings = Gow3Settings::Get();
        row.gauges[SceneW] = float(settings.active_render_width.load(relaxed));
        row.gauges[SceneH] = float(settings.active_render_height.load(relaxed));
        row.gauges[UpscalerMode] = float(settings.upscaler.load(relaxed));
        row.gauges[PresetId] = float(settings.preset.load(relaxed));
        row.gauges[DlssModel] = float(settings.dlss_model.load(relaxed));
        row.gauges[FgOn] = frame_gen ? 1.0f : 0.0f;
        recorder->Record(row);
    }
    g_busy.store(false, std::memory_order_release);
}

} // namespace Gow3Flight
