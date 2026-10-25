// Font atlases at a larger size on the host. The three games draw subtitles, menus and HUD text from
// bitmap atlases made for 1080p (U3 main.fnt: 42 px glyphs in a 256x256 RGBA8 atlas, one glyph per
// channel); at a 4K render size the game draws the text twice as large and bilinear filtering blurs
// every edge (25 Oct 2026 frame dumps; text_sharpen_pass.cpp tightens the edges of what is there).
// Here the host image of a known atlas is created `scale` times larger per side and uploaded from a
// reconstruction of the guest's pixels, while the guest-visible ImageInfo (size, pitch, guest_size,
// tracking) stays as the game describes it: its shaders sample with normalised coordinates, so a
// larger image with the same UVs shows the same glyphs with more texels.
//
// The atlases are recognised by the XXH3 hash of the guest data, like gow3_prompts.cpp does for the
// button icons. GOW3_ATLAS_LOG=1 prints the hash of every candidate (to add atlases),
// GOW3_ATLAS_HASHES=<hex>[,<hex>...] adds hashes for a test, GOW3_ATLAS_SCALE=1|2|4 (default 4)
// sets the factor, 1 turns it off.
//
// Upscale prefers a pre-made vector redraw of the atlas: `<exe dir>/../assets/ui/fonts/<hash>.png`
// (hash as 16 lowercase hex digits; gow3_pc/assets/ui/fonts in the source tree and in the package),
// RGBA8, 4x the guest size per side, channels in the guest's byte order. It is used only after a
// check against the guest: the asset box-downsampled to 1x must match the guest texels with a mean
// absolute error under 4 levels (25 Oct 2026: vector redraws measure 0.6-1.4; another channel order
// or game version is far above). Missing, unreadable or rejected assets fall back to the bicubic
// path; each hash is checked and logged once. GOW3_ATLAS_ASSETS=0 skips the assets (A/B);
// GOW3_ATLAS_ASSET_DIR=<folder> reads them from elsewhere (tests: UNCHARTED_PROBE_EXE builds run as
// the main checkout's Uncharted.exe, whose ../assets is not the tested tree's).

#include "gow3_atlas.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <xxhash.h>

#include "gow3_atlas_resample.h"
#include "gow3_png.h"
#include "video_core/renderer_vulkan/scene_resolution.h"

namespace Gow3Atlas {
namespace {

constexpr u32 kTileLinearAligned = 8; // AmdGpu::TileMode::DisplayLinearAligned
constexpr u32 kTileThin1D = 13;       // AmdGpu::TileMode::Thin1DThin

struct KnownAtlas {
    u64 hash;
    const char* name;
};
// Filled from GOW3_ATLAS_LOG output of each game's atlases.
constexpr KnownAtlas known_atlases[] = {
    {0x35ce544569cac222ULL, "Uncharted 3 main font (main.fnt, 256x256)"},
    {0x188453145becdc37ULL, "Uncharted 2 main font (main.fnt, 256x128)"},
    {0xb4337ad64e659f14ULL, "Uncharted 1 main font (game3.fnt, 256x128)"},
};

// Screen pixels per 1080p UI pixel: the game's render height / 1080 (GOW3_SCENE_SIZE; 2 at 4K).
float Magnification() {
    static const float m = std::max(1.0f, static_cast<float>(Vulkan::SceneResolution::Native().height) / 1080.0f);
    return m;
}

// GOW3_ATLAS_SCALE=1|2|4 (default by the render size: 1 up to 1080p, 2 up to 1920 rows, else 4; 1 = off).
u32 Scale() {
    static const u32 scale = [] {
        const char* env = std::getenv("GOW3_ATLAS_SCALE");
        const int value = env ? std::atoi(env) : (Magnification() <= 1.01f ? 1 : Magnification() <= 1.8f ? 2 : 4);
        return value == 2 || value == 4 ? static_cast<u32>(value) : 1u;
    }();
    return scale;
}

bool Logging() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_ATLAS_LOG");
        return env && env[0] == '1';
    }();
    return on;
}

bool Known(u64 hash, const char*& name) {
    for (const auto& k : known_atlases) {
        if (k.hash && k.hash == hash) {
            name = k.name;
            return true;
        }
    }
    static const std::vector<u64> extra = [] {
        std::vector<u64> list;
        for (const char* s = std::getenv("GOW3_ATLAS_HASHES"); s && *s;) {
            char* next{};
            const u64 value = std::strtoull(s, &next, 16);
            if (value > 1) {
                list.push_back(value);
            }
            s = next && *next == ',' ? next + 1 : nullptr;
        }
        return list;
    }();
    if (std::ranges::find(extra, hash) != extra.end()) {
        name = "GOW3_ATLAS_HASHES";
        return true;
    }
    return false;
}

bool Pow2(u32 v) {
    return v && !(v & (v - 1));
}

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (length < path.size()) { // a truncated name fills the buffer: grow it and ask again
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path{path}.parent_path();
}

// The guest bytes HostScale hashes and Upscale reads (one range, so both see the same hash).
size_t GuestBytes(u32 height, u32 width, u32 pitch, u32 tile_mode) {
    return static_cast<size_t>(tile_mode == kTileThin1D ? width : pitch) * height * 4;
}

struct AssetEntry {
    std::shared_ptr<const std::vector<u8>> x4, x2; // x4 null = no usable asset
};

// The asset of `hash` at `scale` (2 or 4), checked against the guest's linear texels; nullptr = use
// bicubic. Loaded, checked and logged once per hash.
std::shared_ptr<const std::vector<u8>> Asset(u64 hash, const u8* linear, u32 width, u32 height, u32 scale) {
    static std::mutex mutex;
    static std::unordered_map<u64, AssetEntry> cache;
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_ATLAS_ASSETS");
        return !(env && env[0] == '0');
    }();
    if (!enabled) {
        return nullptr;
    }
    std::scoped_lock lock{mutex};
    auto it = cache.find(hash);
    if (it == cache.end()) {
        AssetEntry entry;
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.png", static_cast<unsigned long long>(hash));
        const char* dir = std::getenv("GOW3_ATLAS_ASSET_DIR");
        const auto path = (dir && *dir ? std::filesystem::u8path(dir)
                                       : ExecutableDirectory() / ".." / "assets" / "ui" / "fonts") / name;
        const std::string shown = path.string();
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            if (Logging()) {
                std::printf("Atlas asset %s: missing\n", shown.c_str());
            }
        } else {
            const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
            Gow3Png::Image image;
            if (!Gow3Png::DecodeRgba(bytes.data(), bytes.size(), image)) {
                std::printf("Atlas asset %s: rejected, cannot decode\n", shown.c_str());
            } else if (image.width != static_cast<int>(width * 4) || image.height != static_cast<int>(height * 4)) {
                std::printf("Atlas asset %s: rejected, %dx%d is not 4x %ux%u\n", shown.c_str(), image.width,
                            image.height, width, height);
            } else {
                const auto small = BoxDownsample(image.rgba.data(), width * 4, height * 4, 4);
                const float mae = MeanAbsError(small.data(), linear, small.size());
                const float outliers = OutlierFraction(small.data(), linear, small.size(), 48);
                // 25 Oct 2026: vector redraws measure MAE 0.6-1.4; a wrong channel order or game version is far above.
                // Outliers catch a redraw that is right on average but wrong in a few glyphs.
                const bool ok = mae < 4.0f && outliers <= 0.005f;
                std::printf("Atlas asset %s: %s, MAE %.2f, %.3f%% of values off by more than 48%s\n", shown.c_str(),
                            ok ? "accepted" : "rejected", mae, outliers * 100.0f,
                            ok ? "" : " (channel order or game version?)");
                if (ok) {
                    entry.x4 = std::make_shared<const std::vector<u8>>(std::move(image.rgba));
                    entry.x2 = std::make_shared<const std::vector<u8>>(BoxDownsample(entry.x4->data(), width * 4, height * 4, 2));
                }
            }
        }
        std::fflush(stdout);
        it = cache.emplace(hash, std::move(entry)).first;
    }
    return scale == 4 ? it->second.x4 : it->second.x2;
}

} // namespace

u32 HostScale(VAddr address, u32 width, u32 height, u32 pitch, u32 num_bits, u32 levels, u32 layers,
              u32 tile_mode, u32 guest_size, bool plain_color) {
    const u32 scale = Scale();
    // Candidates: one-level 2D RGBA8 colour images of 128..1024 texels a side that Upscale can read.
    const bool layout = (tile_mode == kTileThin1D && guest_size == width * height * 4) ||
                        (tile_mode == kTileLinearAligned && pitch >= width);
    if (scale <= 1 || !plain_color || num_bits != 32 || levels != 1 || layers != 1 || !layout ||
        !Pow2(width) || !Pow2(height) || width < 128 || width > 1024 || height < 128 || height > 1024) {
        return 1;
    }
    const u64 hash = XXH3_64bits(reinterpret_cast<const void*>(address), GuestBytes(height, width, pitch, tile_mode));
    const char* name = "";
    const bool known = Known(hash, name);
    if (Logging()) {
        static std::mutex mutex;
        static std::unordered_set<u64> logged;
        std::scoped_lock lock{mutex};
        if (logged.insert(address ^ hash).second) {
            std::printf("Atlas: %ux%u RGBA8 (tile mode %u) at 0x%llx, hash %016llx%s%s\n", width, height, tile_mode,
                        static_cast<unsigned long long>(address), static_cast<unsigned long long>(hash),
                        known ? ": " : "", known ? name : "");
            std::fflush(stdout);
        }
    }
    return known ? scale : 1;
}

std::shared_ptr<const Texels> Upscale(VAddr address, u32 width, u32 height, u32 pitch, u32 tile_mode,
                                      u32 scale) {
    std::vector<u8> linear(static_cast<size_t>(width) * height * 4);
    const u8* guest = reinterpret_cast<const u8*>(address);
    if (tile_mode == kTileThin1D) {
        DetileThin32(guest, width, height, linear.data());
    } else if (tile_mode == kTileLinearAligned) {
        ReadLinear32(guest, pitch, width, height, linear.data());
    } else {
        return nullptr;
    }
    auto texels = std::make_shared<Texels>();
    texels->width = width * scale;
    texels->height = height * scale;
    // Recomputed here, not remembered from HostScale: the memory at `address` may hold another image by now.
    const u64 hash = XXH3_64bits(guest, GuestBytes(height, width, pitch, tile_mode));
    const char* name = "";
    if ((scale == 2 || scale == 4) && Known(hash, name)) {
        if (const auto asset = Asset(hash, linear.data(), width, height, scale)) {
            texels->data = *asset;
            return texels;
        }
    }
    // A glyph edge is one source texel wide, Magnification() screen pixels: contrast = that, so it is
    // about one pixel again (the 1080p look) whatever the render size. GOW3_ATLAS_CONTRAST=<x> (tests).
    static const float contrast = [] {
        const char* env = std::getenv("GOW3_ATLAS_CONTRAST");
        const float value = env ? static_cast<float>(std::atof(env)) : 0.0f;
        return value >= 1.0f ? value : Magnification();
    }();
    texels->data = UpscaleBicubic(linear.data(), width, height, scale, contrast);
    return texels;
}

} // namespace Gow3Atlas
