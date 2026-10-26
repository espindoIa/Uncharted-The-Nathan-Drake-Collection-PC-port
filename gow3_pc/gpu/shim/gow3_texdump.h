#pragma once
// Texture dump (gow3_texdump.cpp): GOW3_TEX_DUMP=<dir>.

#include "common/types.h"

namespace Gow3TexDump {

// What TextureCache::RefreshImage knows of a texture upload (mip 0 and the guest layout).
struct Texture {
    VAddr address;
    u32 mip0_size, width, height, pitch; // pitch in texels
    u32 bytes_per_element;               // texel, or 4x4 block for BC
    u32 mips, tile_mode, vk_format;
    bool block;
};

// Writes mip 0 once per guest-data hash (PNG, or .bin when the format or tiling is not decoded)
// and a line in textures.csv. Does nothing when GOW3_TEX_DUMP is unset.
void Dump(const Texture& texture);

} // namespace Gow3TexDump
