// 25 Oct 2026: GOW3_TEX_DUMP=<dir> writes every distinct UI art texture the games upload (mip 0,
// once per hash) so the ones worth replacing at 4x can be picked by eye. The hash is the same as
// gow3_prompts.cpp's (XXH3_64 of mip 0's raw tiled guest bytes, Gow3HdTex::Hash), so a name here
// (<hash>_<w>x<h>_<format>.png) gives the name of the <hash>_<w>x<h>.dds that replaces the texture
// (gow3_hdtex.cpp). Detiling is done here on
// the CPU because the real one runs on the GPU and its output never reaches the CPU.
// Only textures the HD UI replacement could take are dumped (TextureCache IsUiArtCandidate: 8-bit
// UNORM/sRGB and BC formats, never a render target, storage, GPU-written or aliased image).
// Decoded to PNG: RGBA8/BGRA8 and BC1/BC3/BC4/BC5, linear or 1D-tiled thin (tile mode 13: 8x8
// elements per tile, Z order inside, tiles in rows). Anything else (BC7, 2D macro tiling) is
// written as the raw guest bytes in .bin. Not above 4096 per side or 8M texels. The GPU thread only
// hashes and copies; a background thread writes (queue capped at 256 MB, then textures are dropped). Not recorded: the pixel shader that samples it (the
// texture cache upload does not know it; it would need the descriptor binding path).

#include "gow3_texdump.h"

#include "gow3_hdtex.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <fmt/format.h>
#include <vulkan/vulkan.hpp>

#include <miniz.h>

namespace Gow3TexDump {
namespace {

const std::string& Dir() {
    static const std::string dir = [] {
        const char* env = std::getenv("GOW3_TEX_DUMP");
        return env && env[0] ? std::string(env) : std::string();
    }();
    return dir;
}

// Element (x, y) of mip 0 in a thin 1D-tiled image.
size_t TiledOffset(u32 x, u32 y, u32 tiles_per_row, u32 bpe) {
    u32 z = 0;
    for (u32 b = 0; b < 3; ++b) {
        z |= ((x >> b) & 1) << (2 * b);
        z |= ((y >> b) & 1) << (2 * b + 1);
    }
    return (size_t(y / 8) * tiles_per_row + x / 8) * 64 * bpe + size_t(z) * bpe;
}

void Rgb565(u32 c, u8* o) {
    o[0] = u8(((c >> 11) & 31) * 255 / 31);
    o[1] = u8(((c >> 5) & 63) * 255 / 63);
    o[2] = u8((c & 31) * 255 / 31);
}

// BC4 channel of one block into out[i * stride] (16 texels).
void Bc4(const u8* b, u8* out, int stride) {
    u8 pal[8] = {b[0], b[1]};
    if (b[0] > b[1]) {
        for (int i = 1; i < 7; ++i) {
            pal[i + 1] = u8(((7 - i) * b[0] + i * b[1]) / 7);
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            pal[i + 1] = u8(((5 - i) * b[0] + i * b[1]) / 5);
        }
        pal[6] = 0;
        pal[7] = 255;
    }
    u64 bits = 0;
    std::memcpy(&bits, b + 2, 6);
    for (int k = 0; k < 16; ++k) {
        out[k * stride] = pal[(bits >> (3 * k)) & 7];
    }
}

// BC1 colour half of a block into rgba (16 texels); alpha is set only in punch-through mode.
void Bc1(const u8* b, u8* rgba, bool four_colors) {
    const u32 c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    u8 pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    const bool opaque4 = four_colors || c0 > c1;
    for (int i = 0; i < 3; ++i) {
        pal[2][i] = opaque4 ? u8((2 * pal[0][i] + pal[1][i]) / 3) : u8((pal[0][i] + pal[1][i]) / 2);
        pal[3][i] = opaque4 ? u8((pal[0][i] + 2 * pal[1][i]) / 3) : 0;
    }
    for (auto& p : pal) {
        p[3] = 255;
    }
    if (!opaque4) {
        pal[3][3] = 0;
    }
    const u32 bits = b[4] | b[5] << 8 | b[6] << 16 | u32(b[7]) << 24;
    for (int k = 0; k < 16; ++k) {
        std::memcpy(rgba + k * 4, pal[(bits >> (2 * k)) & 3], 4);
    }
}

// Linear element array (rows of `elems_w` elements, tight) to RGBA8; empty when not decodable.
std::vector<u8> Decode(vk::Format fmt, const u8* elems, u32 w, u32 h) {
    std::vector<u8> out(size_t(w) * h * 4);
    switch (fmt) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        std::memcpy(out.data(), elems, out.size());
        return out;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (size_t i = 0; i < out.size(); i += 4) {
            out[i] = elems[i + 2], out[i + 1] = elems[i + 1], out[i + 2] = elems[i], out[i + 3] = elems[i + 3];
        }
        return out;
    default:
        break;
    }
    int block_bytes = 0;
    switch (fmt) {
    case vk::Format::eBc1RgbUnormBlock: case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock: case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc4UnormBlock: block_bytes = 8; break;
    case vk::Format::eBc3UnormBlock: case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc5UnormBlock: block_bytes = 16; break;
    default: return {};
    }
    const u32 nbx = w / 4;
    for (u32 by = 0; by < h / 4; ++by) {
        for (u32 bx = 0; bx < nbx; ++bx) {
            const u8* blk = elems + (size_t(by) * nbx + bx) * block_bytes;
            u8 t[16 * 4] = {};
            switch (fmt) {
            case vk::Format::eBc4UnormBlock:
                Bc4(blk, t, 4);
                for (int k = 0; k < 16; ++k) {
                    t[k * 4 + 1] = t[k * 4 + 2] = t[k * 4], t[k * 4 + 3] = 255;
                }
                break;
            case vk::Format::eBc5UnormBlock:
                Bc4(blk, t, 4);
                Bc4(blk + 8, t + 1, 4);
                for (int k = 0; k < 16; ++k) {
                    t[k * 4 + 2] = 0, t[k * 4 + 3] = 255;
                }
                break;
            case vk::Format::eBc3UnormBlock:
            case vk::Format::eBc3SrgbBlock: {
                Bc1(blk + 8, t, true);
                Bc4(blk, t + 3, 4);
                break;
            }
            default:
                Bc1(blk, t, false);
                break;
            }
            for (u32 k = 0; k < 16; ++k) {
                std::memcpy(&out[((by * 4 + k / 4) * w + bx * 4 + k % 4) * 4], t + k * 4, 4);
            }
        }
    }
    return out;
}

bool WritePng(const std::string& path, const u8* rgba, u32 w, u32 h) {
    size_t size = 0;
    // Level 1: the files are only for looking at.
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba, int(w), int(h), 4, &size, 1, MZ_FALSE);
    FILE* f = png ? std::fopen(path.c_str(), "wb") : nullptr;
    const bool ok = f && std::fwrite(png, 1, size, f) == size;
    if (f) {
        std::fclose(f);
    }
    mz_free(png);
    return ok;
}

// One texture handed from the GPU command thread to the worker: a copy of mip 0's guest bytes.
struct Job {
    Texture t;
    u64 hash;
    std::vector<u8> bytes;
};

// Detile, decode, PNG and CSV: the slow part, on the worker thread only.
void Write(const Job& job) {
    const std::string& dir = Dir();
    const Texture& t = job.t;
    const u64 hash = job.hash;
    const u8* guest = job.bytes.data();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto fmt = static_cast<vk::Format>(t.vk_format);
    const std::string fmt_name = vk::to_string(fmt);
    // Dump names are <hash>_<w>x<h>_<format>: the replacement file is <hash>_<w>x<h>.dds.
    const std::string name = fmt::format("{}/{:016x}_{}x{}_{}", dir, hash, t.width, t.height, fmt_name);

    // Elements per row/column of mip 0 (BC: blocks), padded to what the layout stores.
    const u32 div = t.block ? 4 : 1;
    const u32 ew = (t.width + div - 1) / div, eh = (t.height + div - 1) / div;
    const u32 epitch = std::max((t.pitch + div - 1) / div, ew);
    const bool linear = t.tile_mode == 8 || t.tile_mode == 31;
    const bool thin1d = t.tile_mode == 13;
    bool written = false;
    if (linear || thin1d) {
        const u32 tiles_per_row = (epitch + 7) / 8;
        const size_t need = linear ? size_t(epitch) * (eh - 1) * t.bytes_per_element + size_t(ew) * t.bytes_per_element
                                   : size_t(tiles_per_row) * ((eh + 7) / 8) * 64 * t.bytes_per_element;
        if (need <= t.mip0_size) {
            std::vector<u8> elems(size_t(ew) * eh * t.bytes_per_element);
            for (u32 y = 0; y < eh; ++y) {
                for (u32 x = 0; x < ew; ++x) {
                    const size_t src = linear ? (size_t(y) * epitch + x) * t.bytes_per_element
                                              : TiledOffset(x, y, tiles_per_row, t.bytes_per_element);
                    std::memcpy(&elems[(size_t(y) * ew + x) * t.bytes_per_element], guest + src, t.bytes_per_element);
                }
            }
            // BC decoding needs whole blocks: width/height rounded up to 4.
            auto rgba = Decode(fmt, elems.data(), ew * div, eh * div);
            if (!rgba.empty()) {
                const std::string path = std::string(name) + ".png";
                const u32 dw = ew * div, dh = eh * div;
                written = WritePng(path, rgba.data(), dw, dh);
            }
        }
    }
    if (!written) {
        if (FILE* f = std::fopen((std::string(name) + ".bin").c_str(), "wb")) {
            std::fwrite(guest, 1, t.mip0_size, f);
            std::fclose(f);
        }
    }

    const std::string csv_path = dir + "/textures.csv";
    const bool fresh = !std::filesystem::exists(csv_path, ec);
    if (FILE* f = std::fopen(csv_path.c_str(), "ab")) {
        if (fresh) {
            std::fputs("hash,width,height,vk_format,bits_per_element,mips,tile_mode,guest_address,file\n", f);
        }
        std::fprintf(f, "%016llx,%u,%u,%s,%u,%u,%u,0x%llx,%s\n", static_cast<unsigned long long>(hash), t.width,
                     t.height, fmt_name.c_str(), t.bytes_per_element * 8, t.mips, t.tile_mode,
                     static_cast<unsigned long long>(t.address), written ? "png" : "bin");
        std::fclose(f);
    }
}

// 25 Oct 2026 player log (U1): doing all of Write() on the GPU command thread stalled frames
// 2.2-3.4 s each (716 files, 660 MB). The GPU thread now only hashes and copies mip 0; one
// low-priority worker does the rest. Heap objects that are never freed, so a worker still running
// at exit never touches a destroyed mutex (the thread is detached; the process just ends).
struct Queue {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    size_t pending_bytes = 0;
    std::unordered_set<u64> seen;                           // hashes queued or written
    std::unordered_map<u64, u64> sampled;                   // (address, size) key -> cheap content sample
    bool dropped_logged = false;
};
constexpr size_t kMaxPendingBytes = 256u << 20;
constexpr u32 kMaxSide = 4096, kMaxTexels = 8u << 20;       // UI art is smaller; keeps the disk use down

void Worker(Queue* q) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        Job job;
        {
            std::unique_lock lock{q->mutex};
            q->wake.wait(lock, [&] { return !q->jobs.empty(); });
            job = std::move(q->jobs.front());
            q->jobs.pop_front();
        }
        Write(job);
        std::scoped_lock lock{q->mutex};
        q->pending_bytes -= job.bytes.size();
    }
}

} // namespace

void Dump(const Texture& t) {
    if (Dir().empty() || t.width > kMaxSide || t.height > kMaxSide ||
        u64(t.width) * t.height > kMaxTexels) {
        return;
    }
    static Queue* const q = [] {
        auto* queue = new Queue;
        std::thread(Worker, queue).detach();
        return queue;
    }();
    const u8* guest = reinterpret_cast<const u8*>(t.address);
    // shortcut: a texture seen again at the same address and size is skipped when 3 x 256 sampled
    // bytes match, so a change confined to the unsampled bytes is missed; upgrade by hashing all.
    const size_t n = t.mip0_size, s = std::min<size_t>(256, n);
    u64 sample = Gow3HdTex::Hash(guest, s);
    sample = sample * 31 + Gow3HdTex::Hash(guest + (n - s) / 2, s);
    sample = sample * 31 + Gow3HdTex::Hash(guest + (n - s), s);
    const u64 key = t.address * 1000003u + n;
    {
        std::scoped_lock lock{q->mutex};
        auto [it, fresh] = q->sampled.try_emplace(key, sample);
        if (!fresh && it->second == sample) {
            return;
        }
        it->second = sample;
    }
    const u64 hash = Gow3HdTex::Hash(guest, n);
    std::scoped_lock lock{q->mutex};
    if (!q->seen.insert(hash).second) {
        return;
    }
    if (q->pending_bytes + n > kMaxPendingBytes) {
        q->seen.erase(hash); // retried the next time the texture uploads
        q->sampled.erase(key);
        if (!std::exchange(q->dropped_logged, true)) {
            std::fprintf(stderr, "texdump: queue full (256 MB), dropping textures until the writer catches up\n");
        }
        return;
    }
    q->pending_bytes += n;
    q->jobs.push_back({t, hash, std::vector<u8>(guest, guest + n)});
    q->wake.notify_one();
}

} // namespace Gow3TexDump
