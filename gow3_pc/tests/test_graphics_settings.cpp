#include "test_environment.h"
// gow3.ini graphics keys (gow3_settings.cpp, gow3_graphics.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "gow3_graphics.h"
#include "gow3_settings.h"

namespace G = Gow3Graphics;

static std::string Read(const char* path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

int main() {
    assert(G::ParseDisplayMode("borderless") == G::Borderless && G::ParseDisplayMode("bogus") == G::Windowed);
    assert(G::ParseFpsLimit(120) == 120 && G::ParseFpsLimit(75) == 75);
    // b9e64a4: 0 and faster limits become 120; 4b0f2f1 added 75/90/100; other values fall back to 60.
    assert(G::ParseFpsLimit(0) == 120 && G::ParseFpsLimit(300) == 120 && G::ParseFpsLimit(7) == 60);
    // Shown-frame limits (Frame Generation) are kept as listed; PresentLimit caps them without it.
    for (int v : G::ExtendedFpsLimits) assert(G::ParseFpsLimit(v) == v);
    assert(G::ParseFpsLimit(150) == 120 && G::ParseFpsLimit(241) == 120);
    assert(G::ParseResolution("1440p") == 3 && G::ParseResolution("8K") == 0);
    assert(G::ParseEngineFps(240) == 240 && G::ParseEngineFps(90) == 120);
    assert(G::ParseFramesQueued(2) == 2 && G::ParseFramesQueued(0) == 0 && G::ParseFramesQueued(5) == 1);
    assert(G::ParseRcasStrength(100) == 100 && G::ParseRcasStrength(101) == 75 && G::ParseRcasStrength(-1) == 75);
    assert(G::RcasAttenuation(100) == 0.0f && G::RcasAttenuation(0) == 2.0f && G::RcasAttenuation(75) == 0.5f);
    assert(G::FsrPassRuns(true, false, 1280, 720, 2560, 1440) && !G::FsrPassRuns(false, false, 1280, 720, 2560, 1440));
    assert(G::FsrPassRuns(false, true, 1920, 1080, 1920, 1080) && !G::FsrPassRuns(true, false, 1920, 1080, 1920, 1080));
    assert(!G::FsrPassRuns(true, true, 2560, 1440, 1920, 1080));  // larger image: regular scaling
    assert(G::EffectiveFps(0, 120) == 120 && G::EffectiveFps(240, 120) == 120 && G::EffectiveFps(60, 240) == 60);
    // An unpatched engine steps 1/60 s per frame: presenting faster speeds the game up.
    // b9e64a4: one argument, capped at MaxFpsLimit (120); 0 and faster settings give 120.
    assert(G::PresentLimit(120) == 120 && G::PresentLimit(240) == 120 && G::PresentLimit(30) == 30);
    assert(G::PresentLimit(0) == 120 && G::PresentLimit(75) == 75);
    // Frame Generation: the limit counts shown frames, up to 240; 0 is 240. Without it, as above.
    assert(G::PresentLimit(160, true) == 160 && G::PresentLimit(165, true) == 165 && G::PresentLimit(240, true) == 240);
    assert(G::PresentLimit(0, true) == 240 && G::PresentLimit(300, true) == 240 && G::PresentLimit(75, true) == 75);
    assert(G::PresentLimit(160, false) == 120 && G::PresentLimit(240, false) == 120);
    assert(G::PresentLimit(0, false) == 120 && G::PresentLimit(75, false) == 75);

    const char* path = "graphics-settings-test.ini";
    {
        std::ofstream out(path);
        out << "display_mode=borderless\nvsync=0\nfps_limit=100\nasync_shaders=0\n"
               "render_resolution=1440p\nengine_fps=240\ndeferred_readback=0\nstale_readback=1\n"
               "fsr1=1\nrcas=0\nrcas_strength=40\nframes_queued=2\n";
    }
    SetTestEnv("GOW3_CONFIG", path);
    SetTestEnv("GOW3_ASYNC_SHADERS", "");
    auto& v = Gow3Settings::Get();
    Gow3Settings::Load();
    assert(v.display_mode == G::Borderless && !v.vsync && v.fps_limit == 100 && !v.async_shaders);
    assert(v.render_resolution == 3 && v.engine_fps == 240 && !v.deferred_readback && v.stale_readback);
    assert(v.fsr1 && !v.rcas && v.rcas_strength == 40 && v.frames_queued == 2);
    assert(!Gow3Settings::GraphicsNeedRestart());
    assert(v.applied_engine_fps == 240);  // no GOW3_ENGINE_FPS: the configured rate
    SetTestEnv("GOW3_ENGINE_FPS", "60");    // run.py: the frame rate patch did not apply
    Gow3Settings::Load();
    assert(v.engine_fps == 240 && v.applied_engine_fps == 60 && !Gow3Settings::GraphicsNeedRestart());
    // 6036c2a: only the readback modes need a restart; engine_fps no longer does.
    SetTestEnv("GOW3_ENGINE_FPS", "");
    v.engine_fps = 120;
    assert(!Gow3Settings::GraphicsNeedRestart());
    v.deferred_readback = true;
    assert(Gow3Settings::GraphicsNeedRestart());
    v.deferred_readback = false;
    v.fps_limit = 60;  // live: no restart because of it
    v.engine_fps = 240;
    assert(!Gow3Settings::GraphicsNeedRestart());

    Gow3Settings::Save();
    const std::string saved = Read(path);
    for (const char* line : {"display_mode=borderless\n", "vsync=0\n", "fps_limit=60\n", "async_shaders=0\n",
                             "render_resolution=1440p\n", "engine_fps=240\n", "deferred_readback=0\n",
                             "stale_readback=1\n", "fsr1=1\n", "rcas=0\n", "rcas_strength=40\n",
                             "frames_queued=2\n"}) {
        assert(saved.find(line) != std::string::npos);
    }

    {
        std::ofstream out(path);
        out << "display_mode=huge\nfps_limit=7\nrender_resolution=8K\nengine_fps=1\n"
               "rcas_strength=250\nframes_queued=9\n";
    }
    SetTestEnv("GOW3_ASYNC_SHADERS", "1");  // the environment still wins at start
    v.async_shaders = false;
    Gow3Settings::Load();
    assert(v.display_mode == G::Windowed && v.fps_limit == 60 && v.render_resolution == 0 && v.engine_fps == 120);
    assert(v.rcas_strength == 75 && v.frames_queued == 1);
    assert(v.async_shaders);
    {
        std::ofstream out(path);
        out << "cheat_double_jumps=1\ncheat_very_hard=1\n";
    }
    Gow3Settings::Load();
    Gow3Settings::Save();
    const std::string cheats = Read(path);
    assert(cheats.find("cheat_double_jumps=1\n") != std::string::npos);
    assert(cheats.find("cheat_very_hard=1\n") != std::string::npos);
    // 20 Oct 2026 audit: a launch-only DLSS fallback must not be saved as the player's choice.
    {
        std::ofstream out(path);
        out << "upscaler=dlss\noverlay_corner=0\n";
    }
    Gow3Settings::Load();
    Gow3Settings::ConfigureDlssSupport(false, nullptr);
    assert(v.upscaler == Gow3Settings::UpscalerFsr3);
    v.overlay_corner = 2;
    Gow3Settings::Save();
    const std::string fallback = Read(path);
    assert(fallback.find("upscaler=dlss\n") != std::string::npos);
    assert(fallback.find("overlay_corner=2\n") != std::string::npos);
    assert(!std::ifstream(std::string(path) + ".tmp").good());
    std::remove(path);
    std::puts("PASS: graphics settings");
}
