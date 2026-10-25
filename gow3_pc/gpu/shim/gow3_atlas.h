#pragma once
// Font atlases at a larger size on the host (gow3_atlas.cpp).

#include <memory>
#include <vector>

#include "common/types.h"

namespace Gow3Atlas {

// Linear RGBA8 texels of an upscaled atlas.
struct Texels {
    std::vector<u8> data;
    u32 width, height;
};

// Image creation (VideoCore::Image::Image): the factor (1 = as the guest has it) the host image of a
// 2D RGBA8 texture gets per side. Above 1 only for the font atlases listed in gow3_atlas.cpp (by the
// hash of the guest data), in a layout Upscale reads (thin 1D-tiled or linear aligned).
u32 HostScale(VAddr address, u32 width, u32 height, u32 pitch, u32 num_bits, u32 levels, u32 layers,
              u32 tile_mode, u32 guest_size, bool plain_color);

// Texture cache upload (TextureCache::RefreshImage) of an image HostScale gave a factor above 1: its
// guest data as linear texels `scale` times larger; nullptr when the layout is not one Upscale reads.
// The texels come from the atlas's pre-made 4x vector redraw (assets/ui/fonts/<hash>.png, validated
// against the guest, box-downsampled for scale 2) when one exists, else from a bicubic upscale.
std::shared_ptr<const Texels> Upscale(VAddr address, u32 width, u32 height, u32 pitch, u32 tile_mode,
                                      u32 scale);

} // namespace Gow3Atlas
