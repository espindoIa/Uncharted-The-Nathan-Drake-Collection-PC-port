// 25 Oct 2026: HD UI textures. The games' button icons, prompts and panels are 1080p-era art and
// look soft at 4K. A <hash16hex>_<w>x<h>.dds in the hd-textures folder (GOW3_HD_TEXTURES, else
// next to the exe, else the repo's gow3_pc/assets/ui/hd for source runs) replaces the w x h
// texture whose mip 0 guest bytes hash to that name with a bigger image (Image::EnsureHdBacking
// creates it, the texture cache uploads it). The name is the start of the one GOW3_TEX_DUMP=<dir>
// gives its dumps (<hash>_<w>x<h>_<format>.png), from the same Hash() below. The size is in the
// name so that the texture cache can skip the hashing of every texture no file is for
// (HasGuestSize).
// Formats: BC1, BC3, BC4, BC5, BC7 and R8G8B8A8, DX10 header or legacy FourCC. The file's own
// sRGB flag is ignored: the guest texture's colour space decides (WithColorSpace), because the
// shaders were written for it and the bytes are the same either way. The channels must be the
// guest's (Fit): BC4 over R8, BC5 over R8G8, the others over the colour and alpha formats.

#include "gow3_hdtex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <vulkan/vulkan_core.h>
#include <xxhash.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Gow3HdTex {
namespace {

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 |
           uint32_t(uint8_t(d)) << 24;
}

constexpr uint32_t HeaderSize = 4 + 124;
constexpr uint32_t Dx10HeaderSize = 20;
constexpr uint32_t MaxDimension = 16384;
constexpr uint32_t DdsdMipmapCount = 0x20000, DdpfAlphaPixels = 0x1, DdpfFourcc = 0x4, DdpfRgb = 0x40;
constexpr uint32_t Dx10Texture2D = 3, Dx10MiscCube = 4;

uint32_t Read32(const uint8_t* data, size_t at) {
    uint32_t value;
    std::memcpy(&value, data + at, 4);
    return value;
}

// Bytes per 4x4 block (or per texel for R8G8B8A8) and whether the format is block-compressed;
// false for a format the loader does not take.
bool FormatSize(Format format, uint32_t& bytes, bool& block) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        bytes = 4, block = false;
        return true;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
        bytes = 8, block = true;
        return true;
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        bytes = 16, block = true;
        return true;
    default:
        return false;
    }
}

Format FromDxgi(uint32_t dxgi) {
    switch (dxgi) {
    case 28: return VK_FORMAT_R8G8B8A8_UNORM;
    case 29: return VK_FORMAT_R8G8B8A8_SRGB;
    case 71: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 72: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 77: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 78: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 80: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 83: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 98: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 99: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

} // namespace

bool IsSrgb(Format format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

Format WithColorSpace(Format format, bool srgb) {
    struct Pair {
        Format unorm, srgb;
    };
    static constexpr Pair pairs[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB},
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
        {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK},
        {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK},
    };
    for (const auto& pair : pairs) {
        if (format == pair.unorm || format == pair.srgb) {
            return srgb ? pair.srgb : pair.unorm;
        }
    }
    return format;
}

bool ParseDds(const uint8_t* data, size_t size, Dds& out, std::string& error) {
    out = {};
    if (!data || size < HeaderSize || Read32(data, 0) != Fourcc('D', 'D', 'S', ' ') ||
        Read32(data, 4) != 124 || Read32(data, 76) != 32) {
        error = "not a DDS file";
        return false;
    }
    const uint32_t flags = Read32(data, 8), height = Read32(data, 12), width = Read32(data, 16);
    const uint32_t mip_count = (flags & DdsdMipmapCount) ? std::max(Read32(data, 28), 1u) : 1u;
    const uint32_t pixel_flags = Read32(data, 80), fourcc = Read32(data, 84);
    if (width == 0 || height == 0 || width > MaxDimension || height > MaxDimension) {
        error = "bad size";
        return false;
    }
    if (Read32(data, 112) & (0x200 | 0x200000)) { // caps2: DDSCAPS2_CUBEMAP, DDSCAPS2_VOLUME
        error = "cube maps and volume textures are not supported";
        return false;
    }

    Format format = VK_FORMAT_UNDEFINED;
    size_t offset = HeaderSize;
    if ((pixel_flags & DdpfFourcc) && fourcc == Fourcc('D', 'X', '1', '0')) {
        if (size < HeaderSize + Dx10HeaderSize) {
            error = "truncated DX10 header";
            return false;
        }
        const uint32_t dxgi = Read32(data, HeaderSize), dimension = Read32(data, HeaderSize + 4),
                       misc = Read32(data, HeaderSize + 8), array_size = Read32(data, HeaderSize + 12);
        if (dimension != Dx10Texture2D || (misc & Dx10MiscCube) || array_size > 1) {
            error = "only single 2D textures are supported";
            return false;
        }
        format = FromDxgi(dxgi);
        if (format == VK_FORMAT_UNDEFINED) {
            error = "DXGI format " + std::to_string(dxgi) + " is not supported";
            return false;
        }
        offset += Dx10HeaderSize;
    } else if (pixel_flags & DdpfFourcc) {
        if (fourcc == Fourcc('D', 'X', 'T', '1')) {
            format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        } else if (fourcc == Fourcc('D', 'X', 'T', '5')) {
            format = VK_FORMAT_BC3_UNORM_BLOCK;
        } else if (fourcc == Fourcc('A', 'T', 'I', '1') || fourcc == Fourcc('B', 'C', '4', 'U')) {
            format = VK_FORMAT_BC4_UNORM_BLOCK;
        } else if (fourcc == Fourcc('A', 'T', 'I', '2') || fourcc == Fourcc('B', 'C', '5', 'U')) {
            format = VK_FORMAT_BC5_UNORM_BLOCK;
        } else {
            error = "FourCC is not supported";
            return false;
        }
    } else if ((pixel_flags & DdpfRgb) && (pixel_flags & DdpfAlphaPixels) && Read32(data, 88) == 32 &&
               Read32(data, 92) == 0x000000FF && Read32(data, 96) == 0x0000FF00 &&
               Read32(data, 100) == 0x00FF0000 && Read32(data, 104) == 0xFF000000) {
        format = VK_FORMAT_R8G8B8A8_UNORM;
    } else {
        error = "pixel format is not supported (BC1/3/4/5/7 or RGBA8)";
        return false;
    }

    uint32_t bytes = 0;
    bool block = false;
    FormatSize(format, bytes, block);
    uint32_t possible = 1;
    while ((std::max(width, height) >> possible) != 0) {
        ++possible;
    }
    if (mip_count > possible) {
        error = "more mips than the size allows";
        return false;
    }
    uint64_t at = 0;
    for (uint32_t m = 0; m < mip_count; ++m) {
        const uint32_t w = std::max(width >> m, 1u), h = std::max(height >> m, 1u);
        const uint64_t mip_size = block ? uint64_t((w + 3) / 4) * ((h + 3) / 4) * bytes : uint64_t(w) * h * bytes;
        out.mips.push_back({uint32_t(at), uint32_t(mip_size), w, h});
        at += mip_size;
        if (at > UINT32_MAX) {
            error = "too large";
            return false;
        }
    }
    if (size - offset < at) {
        error = "truncated: " + std::to_string(size - offset) + " bytes of " + std::to_string(at);
        return false;
    }
    out.format = format;
    out.width = width;
    out.height = height;
    out.data_offset = offset;
    return true;
}

namespace {

enum class Channels { Red, RedGreen, ColourAlpha };

Channels ChannelsOf(Format format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_BC4_UNORM_BLOCK:
        return Channels::Red;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_BC5_UNORM_BLOCK:
        return Channels::RedGreen;
    default:
        return Channels::ColourAlpha;
    }
}

} // namespace

bool Fit(const Dds& dds, Format guest_format, uint32_t guest_width, uint32_t guest_height,
         uint32_t guest_mips, Format& host_format, std::string& error) {
    if (ChannelsOf(dds.format) != ChannelsOf(guest_format)) {
        error = "its channels (red, red and green, or colour and alpha) are not the guest texture's";
        return false;
    }
    if (guest_width == 0 || guest_height == 0 || guest_mips == 0 || dds.width % guest_width != 0 ||
        dds.height % guest_height != 0 || dds.width / guest_width != dds.height / guest_height) {
        error = std::to_string(dds.width) + "x" + std::to_string(dds.height) +
                " is not a whole multiple of the guest's " + std::to_string(guest_width) + "x" +
                std::to_string(guest_height);
        return false;
    }
    if (dds.mips.size() < guest_mips) {
        error = std::to_string(dds.mips.size()) + " mips, the guest texture has " +
                std::to_string(guest_mips);
        return false;
    }
    host_format = WithColorSpace(dds.format, IsSrgb(guest_format));
    return true;
}

bool ParseFileName(const std::string& stem, uint64_t& hash, uint32_t& guest_width,
                   uint32_t& guest_height) {
    if (stem.size() < 20 || stem[16] != '_' ||
        !std::all_of(stem.begin(), stem.begin() + 16, [](unsigned char c) { return std::isxdigit(c); }) ||
        !std::isdigit(static_cast<unsigned char>(stem[17]))) {
        return false;
    }
    char* end = nullptr;
    const unsigned long width = std::strtoul(stem.c_str() + 17, &end, 10);
    if (*end != 'x' || !std::isdigit(static_cast<unsigned char>(end[1]))) {
        return false;
    }
    const unsigned long height = std::strtoul(end + 1, &end, 10);
    if (*end || width == 0 || height == 0 || width > MaxDimension || height > MaxDimension) {
        return false;
    }
    hash = std::strtoull(stem.substr(0, 16).c_str(), nullptr, 16);
    guest_width = uint32_t(width);
    guest_height = uint32_t(height);
    return true;
}

uint64_t Hash(const void* data, size_t size) {
    return XXH3_64bits(data, size);
}

namespace {

struct Entry {
    std::filesystem::path path;
    uint32_t guest_width = 0, guest_height = 0; // from the file name
    bool loaded = false;
    bool bad = false; // unreadable or unsupported: not tried again
    bool logged_fit = false;
    Dds dds;
    std::vector<uint8_t> payload;
    std::unique_ptr<Replacement> replacement;
};

std::mutex mutex;
bool scanned = false;
std::unordered_map<uint64_t, Entry> files;
// (guest width << 32 | height) of every file: HasGuestSize, so a world texture is not hashed.
std::unordered_set<uint64_t> guest_sizes;

std::filesystem::path Folder() {
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("GOW3_HD_TEXTURES"); env && env[0]) {
        return env;
    }
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
    const fs::path packaged = exe.parent_path() / "hd-textures";
    if (fs::is_directory(packaged, ec)) {
        return packaged;
    }
    // Source runs: out/gow3-probe.exe next to assets/.
    return exe.parent_path() / ".." / "assets" / "ui" / "hd";
}

void Scan() {
    if (scanned) {
        return;
    }
    scanned = true;
    namespace fs = std::filesystem;
    const fs::path folder = Folder();
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        return;
    }
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext != ".dds") {
            continue;
        }
        uint64_t hash;
        uint32_t width, height;
        if (!ParseFileName(path.stem().string(), hash, width, height)) {
            std::printf("HD UI textures: %s ignored: the name must be <hash16hex>_<w>x<h>.dds\n",
                        path.filename().string().c_str());
            continue;
        }
        Entry& entry = files[hash];
        if (!entry.path.empty()) {
            std::printf("HD UI textures: %s ignored: %s has the same hash\n", path.string().c_str(),
                        entry.path.string().c_str());
            continue;
        }
        entry.path = path;
        entry.guest_width = width;
        entry.guest_height = height;
        guest_sizes.insert(uint64_t(width) << 32 | height);
    }
    std::printf("HD UI textures: %zu files in %s\n", files.size(), folder.string().c_str());
}

bool Load(uint64_t hash, Entry& entry) {
    entry.loaded = true;
    std::ifstream file(entry.path, std::ios::binary | std::ios::ate);
    std::string error;
    if (!file) {
        error = "cannot be opened";
    } else {
        const std::streamsize size = file.tellg();
        std::vector<uint8_t> bytes(size_t(std::max<std::streamsize>(size, 0)));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            error = "cannot be read";
        } else if (ParseDds(bytes.data(), bytes.size(), entry.dds, error)) {
            entry.payload.assign(bytes.begin() + entry.dds.data_offset, bytes.end());
            return true;
        }
    }
    entry.bad = true;
    std::printf("HD UI textures: %016llx_%ux%u.dds rejected: %s\n", static_cast<unsigned long long>(hash),
                entry.guest_width, entry.guest_height, error.c_str());
    return false;
}

} // namespace

const Replacement* Find(uint64_t hash, Format guest_format, uint32_t guest_width,
                        uint32_t guest_height, uint32_t guest_mips) {
    std::scoped_lock lock{mutex};
    Scan();
    const auto it = files.find(hash);
    if (it == files.end() || it->second.bad || (!it->second.loaded && !Load(hash, it->second))) {
        return nullptr;
    }
    Entry& entry = it->second;
    Format format;
    std::string error;
    // The name's size is the guest's: the same bytes at another size are not this texture.
    bool fits = false;
    if (guest_width != entry.guest_width || guest_height != entry.guest_height) {
        error = "the texture is " + std::to_string(guest_width) + "x" + std::to_string(guest_height) +
                ", the name says " + std::to_string(entry.guest_width) + "x" +
                std::to_string(entry.guest_height);
    } else {
        fits = Fit(entry.dds, guest_format, guest_width, guest_height, guest_mips, format, error);
    }
    if (!fits) {
        if (!entry.logged_fit) {
            entry.logged_fit = true;
            std::printf("HD UI textures: %016llx_%ux%u.dds rejected: %s\n", static_cast<unsigned long long>(hash),
                        entry.guest_width, entry.guest_height, error.c_str());
        }
        return nullptr;
    }
    if (!entry.replacement) {
        auto replacement = std::make_unique<Replacement>();
        replacement->format = format;
        replacement->width = entry.dds.width;
        replacement->height = entry.dds.height;
        // 26 Oct 2026: every mip of the file, not just as many as the guest has. The atlas has one
        // level and the game draws its icons ~3x smaller than the 4x file: with level 0 alone the
        // GPU skips texels, and thin strokes and soft glows lose brightness (a title-screen
        // cross: blue stroke peak 171 -> 107 on a 2048 atlas read at level 0 only). The sampler
        // gets the same range (Rasterizer::BindSamplers).
        replacement->mips = uint32_t(entry.dds.mips.size());
        replacement->layout = entry.dds.mips;
        const Mip& last = replacement->layout.back();
        // The payload moves, it is not copied: the file's bytes are held once.
        replacement->data = std::move(entry.payload);
        replacement->data.resize(last.offset + last.size);
        replacement->data.shrink_to_fit();
        entry.payload = {};
        entry.replacement = std::move(replacement);
        std::printf("HD UI textures: %016llx.dds %ux%u -> guest %ux%u\n", static_cast<unsigned long long>(hash),
                    entry.dds.width, entry.dds.height, guest_width, guest_height);
    } else if (entry.replacement->format != format) {
        return nullptr; // the same bytes under another guest colour space (not seen; not worth a second copy)
    }
    return entry.replacement.get();
}

bool AnyFiles() {
    std::scoped_lock lock{mutex};
    Scan();
    return !guest_sizes.empty();
}

bool HasGuestSize(uint32_t guest_width, uint32_t guest_height) {
    std::scoped_lock lock{mutex};
    Scan();
    return guest_sizes.count(uint64_t(guest_width) << 32 | guest_height) != 0;
}

} // namespace Gow3HdTex
