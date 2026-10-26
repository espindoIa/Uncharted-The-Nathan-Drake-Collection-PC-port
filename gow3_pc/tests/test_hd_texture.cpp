// SPDX-License-Identifier: GPL-2.0-or-later
// HD UI textures (shim/gow3_hdtex.cpp): the DDS parser on files built in memory, the size and
// colour-space rules, and the lookup by hash from a folder. No Vulkan device.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <xxhash.h>

#include <vulkan/vulkan_core.h>

#include "../gpu/shim/gow3_hdtex.h"

using namespace Gow3HdTex;

static void Put(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    std::memcpy(&b[at], &v, 4);
}

constexpr uint32_t Fourcc(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

// Legacy header (fourcc != 0), DX10 header (dxgi != 0) or RGBA8 masks (both 0), then `payload`
// zeroed bytes.
static std::vector<uint8_t> MakeDds(uint32_t w, uint32_t h, uint32_t mips, uint32_t fourcc,
                                    uint32_t dxgi, size_t payload) {
    std::vector<uint8_t> b(128 + (dxgi ? 20 : 0) + payload);
    Put(b, 0, Fourcc("DDS "));
    Put(b, 4, 124);
    Put(b, 8, 0x1007 | (mips > 1 ? 0x20000 : 0));
    Put(b, 12, h);
    Put(b, 16, w);
    Put(b, 28, mips);
    Put(b, 76, 32);
    if (dxgi) {
        Put(b, 80, 4);
        Put(b, 84, Fourcc("DX10"));
        Put(b, 128, dxgi);
        Put(b, 132, 3); // 2D
        Put(b, 140, 1); // array size
    } else if (fourcc) {
        Put(b, 80, 4);
        Put(b, 84, fourcc);
    } else {
        Put(b, 80, 0x41);
        Put(b, 88, 32);
        Put(b, 92, 0xFF), Put(b, 96, 0xFF00), Put(b, 100, 0xFF0000), Put(b, 104, 0xFF000000);
    }
    return b;
}

static bool Parse(const std::vector<uint8_t>& b, Dds& dds) {
    std::string error;
    return ParseDds(b.data(), b.size(), dds, error);
}

int main() {
    Dds dds;

    // BC1 by FourCC, 8x8 with a full chain: 2x2, 1, 1, 1 blocks of 8 bytes.
    auto bc1 = MakeDds(8, 8, 4, Fourcc("DXT1"), 0, 32 + 8 + 8 + 8);
    assert(Parse(bc1, dds));
    assert(dds.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && dds.width == 8 && dds.height == 8);
    assert(dds.data_offset == 128 && dds.mips.size() == 4);
    assert(dds.mips[0].offset == 0 && dds.mips[0].size == 32 && dds.mips[0].width == 8);
    assert(dds.mips[1].offset == 32 && dds.mips[1].size == 8 && dds.mips[1].width == 4);
    assert(dds.mips[3].offset == 48 && dds.mips[3].size == 8 && dds.mips[3].width == 1);

    // BC7 sRGB through the DX10 header, 16x16, three mips of 4x4, 2x2 and 1 blocks.
    auto bc7 = MakeDds(16, 16, 3, Fourcc("DX10"), 99, 256 + 64 + 16);
    assert(Parse(bc7, dds));
    assert(dds.format == VK_FORMAT_BC7_SRGB_BLOCK && dds.data_offset == 148);
    assert(dds.mips.size() == 3 && dds.mips[0].size == 256 && dds.mips[1].offset == 256 &&
           dds.mips[2].offset == 320 && dds.mips[2].size == 16);

    // R8G8B8A8: by masks (4 bytes a texel) and by DX10.
    auto rgba = MakeDds(8, 4, 3, 0, 0, 128 + 32 + 8);
    assert(Parse(rgba, dds));
    assert(dds.format == VK_FORMAT_R8G8B8A8_UNORM && dds.mips[1].width == 4 && dds.mips[1].height == 2 &&
           dds.mips[1].offset == 128 && dds.mips[2].offset == 160 && dds.mips[2].size == 8);
    assert(Parse(MakeDds(4, 4, 1, Fourcc("DX10"), 29, 64), dds) && dds.format == VK_FORMAT_R8G8B8A8_SRGB);

    // The rest of the accepted formats; BC4 and BC5 by both FourCCs.
    assert(Parse(MakeDds(4, 4, 1, Fourcc("DXT5"), 0, 16), dds) && dds.format == VK_FORMAT_BC3_UNORM_BLOCK);
    assert(Parse(MakeDds(4, 4, 1, Fourcc("ATI1"), 0, 8), dds) && dds.format == VK_FORMAT_BC4_UNORM_BLOCK);
    assert(Parse(MakeDds(4, 4, 1, Fourcc("BC5U"), 0, 16), dds) && dds.format == VK_FORMAT_BC5_UNORM_BLOCK);
    assert(Parse(MakeDds(4, 4, 1, Fourcc("DX10"), 80, 8), dds) && dds.format == VK_FORMAT_BC4_UNORM_BLOCK);

    // A BC block is padded: a 6x2 BC1 image is 2x1 blocks.
    assert(Parse(MakeDds(6, 2, 1, Fourcc("DXT1"), 0, 16), dds) && dds.mips[0].size == 16);

    // Rejected: truncated payload, wrong magic, BC6H, DXT3, a cube map, more mips than the size
    // has, an empty buffer.
    assert(!Parse(MakeDds(8, 8, 4, Fourcc("DXT1"), 0, 55), dds));
    auto junk = bc1;
    junk[0] = 'X';
    assert(!Parse(junk, dds));
    assert(!Parse(MakeDds(4, 4, 1, Fourcc("DX10"), 95, 16), dds));
    assert(!Parse(MakeDds(4, 4, 1, Fourcc("DXT3"), 0, 16), dds));
    auto cube = MakeDds(4, 4, 1, Fourcc("DXT1"), 0, 8 * 6);
    Put(cube, 112, 0x200);
    assert(!Parse(cube, dds));
    assert(!Parse(MakeDds(4, 4, 5, Fourcc("DXT1"), 0, 64), dds));
    std::string unused;
    assert(!ParseDds(nullptr, 0, dds, unused));

    // Size rule: a whole multiple of the guest size with its aspect, at least its mips.
    assert(Parse(MakeDds(16, 16, 3, Fourcc("DXT1"), 0, 128 + 32 + 8), dds));
    Format host;
    std::string error;
    // A colour and alpha guest (RGBA8) in the given colour space.
    const auto fit = [&](const Dds& d, uint32_t w, uint32_t h, uint32_t mips, bool srgb) {
        return Fit(d, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, w, h, mips, host, error);
    };
    assert(fit(dds, 4, 4, 3, false));   // 4x
    assert(fit(dds, 16, 16, 1, false)); // 1x: only a better format
    assert(fit(dds, 8, 8, 2, false));   // 2x, fewer mips than the file
    assert(!fit(dds, 4, 4, 4, false));  // the guest has more mips
    assert(!fit(dds, 5, 5, 1, false));  // 16 / 5
    assert(!fit(dds, 8, 4, 1, false));  // multiples 2 and 4: another aspect
    assert(!fit(dds, 32, 32, 1, false)); // smaller than the guest
    assert(!fit(dds, 0, 4, 1, false));
    assert(!fit(dds, 4, 4, 0, false));

    // Colour space follows the guest, whatever the file says.
    Dds srgb_file, unorm_file, bc5_file, bc4_file;
    assert(Parse(MakeDds(8, 8, 1, Fourcc("DX10"), 99, 64), srgb_file));
    assert(Parse(MakeDds(8, 8, 1, Fourcc("DX10"), 98, 64), unorm_file));
    assert(Parse(MakeDds(8, 8, 1, Fourcc("ATI2"), 0, 64), bc5_file));
    assert(Parse(MakeDds(8, 8, 1, Fourcc("ATI1"), 0, 32), bc4_file));
    assert(fit(unorm_file, 4, 4, 1, true) && host == VK_FORMAT_BC7_SRGB_BLOCK);
    assert(fit(srgb_file, 4, 4, 1, false) && host == VK_FORMAT_BC7_UNORM_BLOCK);
    assert(fit(srgb_file, 4, 4, 1, true) && host == VK_FORMAT_BC7_SRGB_BLOCK);
    assert(Fit(bc5_file, VK_FORMAT_R8G8_UNORM, 4, 4, 1, host, error) && host == VK_FORMAT_BC5_UNORM_BLOCK);

    // Channels follow the guest too: no colour or alpha over a red-only or red-green texture
    // (BC4 over R8, BC5 over R8G8/BC5), and no BC4/BC5 over a colour and alpha one.
    assert(Fit(bc4_file, VK_FORMAT_R8_UNORM, 4, 4, 1, host, error));
    assert(Fit(bc4_file, VK_FORMAT_BC4_UNORM_BLOCK, 4, 4, 1, host, error));
    assert(Fit(bc5_file, VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 1, host, error));
    assert(!Fit(unorm_file, VK_FORMAT_R8_UNORM, 4, 4, 1, host, error));
    assert(!Fit(unorm_file, VK_FORMAT_R8G8_UNORM, 4, 4, 1, host, error));
    assert(!Fit(bc4_file, VK_FORMAT_R8G8_UNORM, 4, 4, 1, host, error));
    assert(!fit(bc4_file, 4, 4, 1, false));
    assert(!fit(bc5_file, 4, 4, 1, false));
    assert(WithColorSpace(VK_FORMAT_R8G8B8A8_UNORM, true) == VK_FORMAT_R8G8B8A8_SRGB);
    assert(WithColorSpace(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, false) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    assert(WithColorSpace(VK_FORMAT_BC3_UNORM_BLOCK, true) == VK_FORMAT_BC3_SRGB_BLOCK);
    assert(IsSrgb(VK_FORMAT_BC7_SRGB_BLOCK) && IsSrgb(VK_FORMAT_B8G8R8A8_SRGB) &&
           !IsSrgb(VK_FORMAT_BC7_UNORM_BLOCK) && !IsSrgb(VK_FORMAT_R16G16B16A16_SFLOAT));

    // The name of a dump is the hash of the guest bytes: XXH3_64.
    const uint8_t guest[64] = {1, 2, 3, 4, 5};
    assert(Hash(guest, sizeof(guest)) == XXH3_64bits(guest, sizeof(guest)));

    // Lookup from a folder (a subfolder too), validated against the guest texture, mips cut to
    // the guest's, a bad file rejected once and for good.
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / "gow3-hd-texture-test";
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder / "buttons", ec);
    const auto write = [&](const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    auto sixteen = MakeDds(16, 16, 3, Fourcc("DXT1"), 0, 128 + 32 + 8);
    for (size_t i = 128; i < sixteen.size(); ++i) {
        sixteen[i] = uint8_t(i);
    }
    write(folder / "buttons" / "00000000000000aa_4x4.dds", sixteen);
    write(folder / "00000000000000bb_4x4.dds", std::vector<uint8_t>(40, 7));
    write(folder / "00000000000000dd_8x2.dds", sixteen);
    write(folder / "notahash.dds", sixteen);
    write(folder / "00000000000000ee.dds", sixteen); // no guest size in the name: ignored
    // The same hash twice (another folder): one of them is used, the other warned about.
    write(folder / "buttons" / "00000000000000ff_4x4.dds", sixteen);
    write(folder / "00000000000000ff_4x4.dds", sixteen);
    const std::string env = "GOW3_HD_TEXTURES=" + folder.string();
    putenv(env.c_str());

    // The cheap test before hashing: only the sizes some file is named for.
    assert(HasGuestSize(4, 4) && HasGuestSize(8, 2));
    assert(!HasGuestSize(2, 8) && !HasGuestSize(16, 16) && !HasGuestSize(0, 0));
    const Format guest_rgba = VK_FORMAT_R8G8B8A8_UNORM, guest_srgb = VK_FORMAT_R8G8B8A8_SRGB;
    assert(!Find(0xcc, guest_rgba, 4, 4, 1));          // no such file
    assert(!Find(0xee, guest_rgba, 4, 4, 1));          // its name was not taken
    assert(!Find(0xbb, guest_rgba, 4, 4, 1));          // not a DDS
    assert(!Find(0xaa, guest_rgba, 5, 5, 1));          // the name says 4x4
    assert(!Find(0xdd, guest_rgba, 8, 2, 1));          // 16x16 is not the same aspect as 8x2
    assert(!Find(0xaa, VK_FORMAT_R8_UNORM, 4, 4, 1)); // BC1 over a red-only texture
    const Replacement* found = Find(0xaa, guest_srgb, 4, 4, 2);
    assert(found && found->format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
    // 26 Oct 2026: every mip of the file (16x16, 8x8, 4x4 here), not the guest's two: the sampler
    // and the view reach the whole chain (an atlas has one guest level and is drawn 3x smaller).
    assert(found->width == 16 && found->height == 16 && found->mips == 3 && found->layout.size() == 3);
    assert(found->data.size() == 128 + 32 + 8 && found->data[0] == uint8_t(128));
    assert(Find(0xaa, guest_srgb, 4, 4, 2) == found); // cached
    assert(!Find(0xaa, guest_rgba, 4, 4, 2));              // the same bytes in another colour space: not served
    assert(Find(0xff, guest_rgba, 4, 4, 1));               // a duplicate name still serves one file

    // File names: <hash16hex>_<w>x<h>, nothing else.
    uint64_t hash = 0;
    uint32_t w = 0, h = 0;
    assert(ParseFileName("00000000000000aa_4x8", hash, w, h) && hash == 0xaa && w == 4 && h == 8);
    assert(ParseFileName("FFFFFFFFFFFFFFFF_1920x1080", hash, w, h) && hash == ~0ull && w == 1920 && h == 1080);
    assert(!ParseFileName("00000000000000aa", hash, w, h));        // no size
    assert(!ParseFileName("00000000000000aa_4x8_BC1", hash, w, h)); // a dump name, not a file
    assert(!ParseFileName("00000000000000aa_4", hash, w, h));
    assert(!ParseFileName("00000000000000aa_x8", hash, w, h));
    assert(!ParseFileName("00000000000000aa_4x", hash, w, h));
    assert(!ParseFileName("00000000000000aa_0x8", hash, w, h));
    assert(!ParseFileName("00000000000000aa_-4x8", hash, w, h));
    assert(!ParseFileName("00000000000000aa_99999x8", hash, w, h)); // above the largest DDS
    assert(!ParseFileName("0000000000000aa_4x8", hash, w, h));      // 15 digits
    assert(!ParseFileName("00000000000000ag_4x8", hash, w, h));
    fs::remove_all(folder, ec);

    std::puts("hd-texture-test: ok");
    return 0;
}
