#pragma once
// HD UI textures (gow3_hdtex.cpp): larger replacement images for the games' UI art, keyed by the
// hash and guest size gow3_texdump.cpp names its files with (<hash16hex>_<w>x<h>.dds).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Gow3HdTex {

// A VkFormat value (0 is VK_FORMAT_UNDEFINED). Not the Vulkan type: vk_common.h sets macros before
// it includes the Vulkan headers, and a header that pulls them in first breaks that.
using Format = uint32_t;

// Where one mip sits in the payload after the DDS headers.
struct Mip {
    uint32_t offset, size, width, height;
};

// A parsed DDS: format as the file stores it (colour space not yet matched to the guest),
// size, and every mip's place in the bytes from data_offset on.
struct Dds {
    Format format = 0;
    uint32_t width = 0, height = 0;
    size_t data_offset = 0;
    std::vector<Mip> mips;
};

// DX10 header (BC1/3/4/5/7, R8G8B8A8) or legacy FourCC (DXT1, DXT5, ATI1, ATI2) and RGBA8 masks.
// Fails (error set) on anything else, on cube maps, volumes and arrays, and on truncated files.
bool ParseDds(const uint8_t* data, size_t size, Dds& out, std::string& error);

bool IsSrgb(Format format);
// The same format family in the requested colour space; formats without an sRGB twin (BC4, BC5)
// come back as they are.
Format WithColorSpace(Format format, bool srgb);

// Whether the file can replace a guest texture of guest_format, guest_width x guest_height with
// guest_mips levels: an integer multiple of the guest size with the same aspect, at least as many
// mips as the guest has, and the guest's channels (R8 and BC4: red only; R8G8 and BC5: red and
// green; the rest: colour and alpha; the shaders and the T#'s swizzle were written for those, and
// a BC7 over an R8 would show colour and alpha where the guest has none). The guest's colour space
// picks host_format, the format to create.
bool Fit(const Dds& dds, Format guest_format, uint32_t guest_width, uint32_t guest_height,
         uint32_t guest_mips, Format& host_format, std::string& error);

// Splits a replacement file's name (without extension), <hash16hex>_<w>x<h>, into the hash and the
// guest size it is for. False for any other name.
bool ParseFileName(const std::string& stem, uint64_t& hash, uint32_t& guest_width,
                   uint32_t& guest_height);

// XXH3_64 of a texture's mip 0 guest bytes: the name of its dump and of its replacement.
uint64_t Hash(const void* data, size_t size);

struct Replacement {
    Format format;
    uint32_t width, height, mips; // mips: every level of the file (at least as many as the guest has)
    std::vector<uint8_t> data;
    std::vector<Mip> layout;      // offsets into data, one per mip
};

// The replacement for the guest texture whose mip 0 hashes to `hash`, or nullptr (no file, a file
// that does not fit, an unreadable one: logged once per hash). The folder is scanned on first use;
// the pointer stays valid for the process.
const Replacement* Find(uint64_t hash, Format guest_format, uint32_t guest_width,
                        uint32_t guest_height, uint32_t guest_mips);

// Whether some file is for a guest texture of this size: the cheap test before hashing a texture
// (world textures are most of the uploads, and none of them has a file).
bool HasGuestSize(uint32_t guest_width, uint32_t guest_height);

// Whether the folder has any file (a sampler widened for replacement mips is only made then).
bool AnyFiles();

} // namespace Gow3HdTex
