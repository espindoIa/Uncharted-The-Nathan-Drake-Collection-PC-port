// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 25 Oct 2026: the engine flight recorder. Players report one-frame flashes, flickers and
// ghosts that nobody can predict, so nothing is logged when they happen (a key pressed after the
// flash is too late for a capture). The recorder keeps the last GOW3_FLIGHT_FRAMES flips (default
// 1024, about 7-15 s) of the engine's numbers in a ring and writes them to a CSV when asked (the
// trigger file) or when it sees an anomaly itself, so the frames around a glitch can be studied
// afterwards. analysis/STATUS.md "25 October 2026: engine flight recorder" has the design.
//
// Environment:
//   GOW3_FLIGHT=0          disables everything (default on).
//   GOW3_FLIGHT_DIR=<dir>  output folder (default: `flight` next to GOW3_DUMP_DIR, else ./flight).
//   GOW3_FLIGHT_TRIGGER=<file>  a file whose appearance asks for a dump; its first line is the
//                          reason label (lower case, other characters than [a-z0-9_-] dropped, at
//                          most 24, default "manual"; tools/session_recorder.py flight_label()).
//   GOW3_FLIGHT_AUTO=0     no automatic (anomaly) dumps.
//   GOW3_FLIGHT_FRAMES=<n> ring size (default 1024, 64..8192).
//   GOW3_FLIGHT_CUT_MOVE=<units>  camera cut rule: the smallest move per flip that can count as
//                          a cut (default 0.25; the rule itself is relative, 8x the median move).
//   GOW3_FLIGHT_CUT_DEG=<deg>     camera cut rule: a turn per flip over this is a cut (default 30).
//
// Output: <dir>/flight_<YYYYmmdd-HHMMSS>-<mmm>_<reason>.csv (local time), written as <name>.tmp
// and renamed. Automatic dumps are named auto-<kind> (stall, reset, skipped, camjump). Line 1:
//   # flight v1 unix_ms=<ms at dump> reason=<reason> frames=<rows> ring=<size> build=<exe>/<bytes>
// line 2 the header row, then one row per flip, oldest first, up to 60 flips after the request
// (or 2 s after it if the flips stop). Columns (names are a contract with tools/session_recorder.py):
//   seq            flip number since start (1-based)
//   unix_ms        system clock ms at the flip; t_ms steady ms since start; dt_ms since the
//                  previous row (0 on the first row)
//   marker         1 on the row the dump is about (the request, the stall, the reset, the cut)
//   events         0x-prefixed hex mask of EventBit seen since the previous flip; event_names the same,
//                  |-separated; event_detail the last detail given (UpscalerReset: ResetReason)
//   d_*            change since the previous row of a cumulative counter (0 on the first row):
//                  d_draws d_dispatches d_gpu_frames (GPU command thread frames) d_compiles
//                  (shader and pipeline compiles finished) d_skipped_draws (draws dropped while
//                  their pipeline compiled in the background) d_gc_evictions (texture cache)
//                  d_images_registered d_image_upload_mb d_buffer_upload_mb d_copy_mb; only with
//                  GOW3_FRAME_STATS=1 (0 otherwise): d_submissions d_gc_ms d_frame_wait_ms
//                  d_present_ms
//   arena_mb image_mb images_live   memory levels (buffer arenas, images)
//   cam_x cam_y cam_z  camera position; fwd_x fwd_y fwd_z its forward axis (world space)
//   proj_x proj_y      projection scale (x, y); jit_x jit_y the upscaler's jitter in pixels
//   scene_w scene_h    render size; out_w out_h the upscaler's output size
//   upscaler           Gow3Settings::Upscaler (0 off, 1 FSR 3, 2/3 FSR 4, 4 TAA, 5 DLSS)
//   preset dlss_model  Gow3Settings::Preset, DLSS model (0 J, 1 K, 2 L, 3 M)
//   lod_bias           scene texture LOD bias; fg_on 1 while DLSS frame generation presents
//   cam_jump_deg cam_move  turn of the forward axis (degrees) and camera distance moved since
//                  the previous row (0 when either row has no camera)
//   presents       frames shown for the last present before this flip (1, 2 with frame
//                  generation 2x; Streamline: its own count, one more frame late; 0 unknown)
// The gauges are the latest values when the flip is recorded: the GPU thread may already be in
// the next frame. Frames made by DLSS frame generation are not flips and are not recorded
// (`presents` counts them).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Gow3Flight {

/// Off with GOW3_FLIGHT=0: the hooks below then cost one load of this flag.
inline const bool kEnabled = [] {
    const char* env = std::getenv("GOW3_FLIGHT");
    return !(env && env[0] == '0' && env[1] == '\0');
}();

/// Events of a flip (CSV `events`): one bit each, set from any thread.
enum EventBit : std::uint32_t {
    UpscalerReset = 1u << 0,             ///< upscaler history dropped (detail: ResetReason)
    HistoryInvalidated = 1u << 1,        ///< camera motion history dropped with it
    NoCamera = 1u << 2,                  ///< a display pass without the frame's camera
    DrawSkippedPendingCompile = 1u << 3, ///< a draw dropped: its pipeline compiles in the background
    PipelineCompiled = 1u << 4,          ///< a shader or pipeline compile finished
    UpscalerFeatureCreated = 1u << 5,    ///< upscaler resources or DLSS feature (re)created
    StretchedOrUiFrame = 1u << 6,        ///< menu/loading (UI only) or stretched frame
    FrameGenOnOff = 1u << 7,             ///< fg_on changed
    TextureGcEvict = 1u << 8,            ///< the texture cache freed an image (memory pressure)
    MovieFrame = 1u << 9,                ///< a prerecorded movie is playing
    LoadingFrame = 1u << 10,             ///< the presenter drew its own loading frame (not a flip)
};
inline constexpr int kEventCount = 11;

/// event_detail of UpscalerReset: which site in vk_temporal_upscaler.cpp dropped the history.
enum ResetReason : int {
    ResetSettingsChanged = 1, ///< upscaler, preset, output size, active or jitter changed
    ResetNotRunLastFrame = 2, ///< the upscaler did not run in the previous frame
    ResetNoInputs = 3,        ///< its trigger passed without scene color or camera
    ResetResources = 4,       ///< its resources were (re)created
    ResetUiOnly = 5,          ///< a UI-only frame (menu, loading screen)
    ResetStretched = 6,       ///< a stretched frame
    ResetFsr4Fallback = 7,    ///< FSR 4 failed: FSR 3 next frame
    ResetDlssFeature = 8,     ///< the DLSS feature was (re)created
    ResetDlssFallback = 9,    ///< DLSS failed: FSR 3 next frame
};

/// Monotonic counters of the recorder's own (the 5 s Frame stats printer zeroes its own copies).
enum CounterId : int { Compiles, SkippedDraws, GcEvictions, kCounterCount };

/// Latest values (stored as float bits).
enum GaugeId : int {
    CamX, CamY, CamZ, FwdX, FwdY, FwdZ, ProjX, ProjY, JitX, JitY, OutW, OutH, LodBias, Presents,
    kHookGaugeCount, // the rest are filled by OnFlip itself
    SceneW = kHookGaugeCount, SceneH, UpscalerMode, PresetId, DlssModel, FgOn, kGaugeCount
};

inline std::atomic<std::uint32_t> g_events{0};
inline std::atomic<int> g_event_detail{0};
inline std::atomic<std::uint64_t> g_counters[kCounterCount];
inline std::atomic<std::uint32_t> g_gauges[kHookGaugeCount];

inline void Event(EventBit bit, int detail = 0) {
    if (!kEnabled) return;
    g_events.fetch_or(bit, std::memory_order_relaxed);
    if (detail) g_event_detail.store(detail, std::memory_order_relaxed);
}
inline void Count(CounterId id, std::uint64_t n = 1) {
    if (kEnabled) g_counters[id].fetch_add(n, std::memory_order_relaxed);
}
inline void Gauge(GaugeId id, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if (kEnabled) g_gauges[id].store(bits, std::memory_order_relaxed);
}
/// The camera from a view to world 3x4 (rows [R | t]: columns right, up, forward; t position).
inline void Camera(const float* to_world, float proj_x, float proj_y) {
    Gauge(CamX, to_world[3]), Gauge(CamY, to_world[7]), Gauge(CamZ, to_world[11]);
    Gauge(FwdX, to_world[2]), Gauge(FwdY, to_world[6]), Gauge(FwdZ, to_world[10]);
    Gauge(ProjX, proj_x), Gauge(ProjY, proj_y);
}

/// Once per presented guest flip, on the present thread (VideoOutDriver::Flip). Lock-free and
/// allocation-free after the first call (which allocates the ring and starts the dump thread).
void OnFlip(bool movie, bool frame_gen);

// ---- Below: the recorder itself, public for tests/test_flight.cpp. ----

/// Cumulative values snapshotted per flip; the first kDeltaCount are written as deltas.
enum CumId : int {
    CDraws, CDispatches, CGpuFrames, CCompiles, CSkippedDraws, CGcEvictions, CImagesRegistered,
    CImageUploadBytes, CBufferUploadBytes, CCopyBytes, CSubmissions, CGcNs, CFrameWaitNs,
    CPresentNs, kDeltaCount,
    CArenaBytes = kDeltaCount, CImageBytes, CImagesLive, kCumCount
};

struct Row {
    std::uint64_t seq;
    std::int64_t t_us; ///< steady clock since start
    std::uint32_t events;
    std::int32_t detail;
    std::uint64_t cum[kCumCount];
    float gauges[kGaugeCount];
};
static_assert(sizeof(Row) % 8 == 0);

enum AutoKind : int { AutoStall, AutoReset, AutoSkipped, AutoCamjump, kAutoKindCount };
const char* AutoKindName(int kind);

class Recorder {
public:
    static constexpr int kMaxRequests = 4;
    static constexpr std::uint32_t kFramesAfter = 60;
    static constexpr std::int64_t kGraceUs = 5'000'000;    ///< no auto dumps in the first 5 s
    static constexpr std::int64_t kCooldownUs = 15'000'000; ///< per kind (but skipped)
    /// skipped: draws skip while pipelines compile, often in the first minutes with a cold cache.
    static constexpr std::int64_t kSkippedCooldownUs = 60'000'000;
    static constexpr int kMaxAutoDumps = 30;
    /// No stall, reset or skipped dump within 2 s of a movie, UI-only or loading frame.
    static constexpr std::int64_t kQuietUs = 2'000'000;
    static constexpr float kCutMove = 0.25f; ///< GOW3_FLIGHT_CUT_MOVE default
    static constexpr float kCutDeg = 30.0f;  ///< GOW3_FLIGHT_CUT_DEG default

    struct Request {
        std::atomic<int> state{0}; ///< 0 free, 1 claimed, 2 ready for the dump thread
        int kind = -1;             ///< AutoKind, or -1 manual (label below)
        char label[32]{};
        std::uint64_t marker_seq = 0, fire_seq = 0;
        std::int64_t posted_us = 0;
    };

    Recorder(std::uint32_t ring_size, bool auto_dumps);
    std::uint32_t RingSize() const { return size; }

    /// The hot path: stores `row` (its seq is assigned) and runs the anomaly detectors. One
    /// thread at a time (OnFlip's guard).
    void Record(Row row);
    std::uint64_t LastSeq() const { return last_seq.load(std::memory_order_acquire); }

    /// Asks for a dump (any thread); false when the 4 request slots are taken.
    bool Post(int kind, const char* label, std::uint64_t marker_seq, std::uint64_t fire_seq,
              std::int64_t now_us);
    Request& RequestSlot(int i) { return requests[i]; }

    /// The recorded rows from..to (inclusive) still in the ring and not being rewritten.
    std::vector<Row> Collect(std::uint64_t from, std::uint64_t to) const;

    int AutoDumps() const { return auto_dumps_done; }
    float CutMove() const { return cut_move; }
    float CutDeg() const { return cut_deg; }

private:
    struct Slot {
        std::atomic<std::uint64_t> version{0};
        std::atomic<std::uint64_t> words[sizeof(Row) / 8];
    };
    bool AutoAllowed(int kind, std::int64_t t_us) const;
    void PostAuto(int kind, std::uint64_t marker_seq, std::uint64_t fire_seq, std::int64_t t_us);

    std::uint32_t size;
    bool auto_on;
    float cut_move, cut_deg; ///< GOW3_FLIGHT_CUT_MOVE / GOW3_FLIGHT_CUT_DEG
    std::vector<Slot> ring;
    std::atomic<std::uint64_t> last_seq{0};
    Request requests[kMaxRequests];
    // Detector state (Record's thread only).
    Row prev{};
    bool have_prev = false;
    float dts[32]{}, moves[32]{};
    std::uint32_t dt_n = 0, move_n = 0;
    std::int64_t last_auto_us[kAutoKindCount];
    int auto_dumps_done = 0;
    std::int64_t last_quiet_us = -kQuietUs; ///< last row with a movie, UI-only or loading frame
    bool reset_pending = false;
    std::uint64_t reset_seq = 0;
    std::int64_t reset_us = 0;
    int camjump_left = 0; ///< flips left to see a reset after a camera jump
    std::uint64_t camjump_seq = 0;
};

/// Camera turn (degrees) and distance between two rows; false when either has no camera.
bool CameraChange(const Row& a, const Row& b, float& deg, float& move);
/// The dump file text: the comment line, the header and the rows.
std::string Csv(const std::vector<Row>& rows, std::uint64_t marker_seq, const std::string& reason,
                std::int64_t unix_ms, std::uint32_t ring_size, const std::string& build,
                std::int64_t start_unix_ms);
/// Number of columns of the header row.
int CsvColumns();
/// The first line in lower case without characters other than [a-z0-9_-], at most 24 of them;
/// "manual" when nothing is left.
std::string SanitizeLabel(const std::string& text);
/// flight_<YYYYmmdd-HHMMSS>-<mmm>_<reason>.csv for a unix time in ms (local time).
std::string FileName(std::int64_t unix_ms, const std::string& reason);
/// Writes <dir>/<name>.tmp, then renames it to <dir>/<name>; false (and no file) on failure.
bool WriteAtomic(const std::string& dir, const std::string& name, const std::string& text,
                 std::string& error);

} // namespace Gow3Flight
