// Detile, bicubic upscale, box downsample and error metric of gow3_atlas_resample.h (font atlases at a larger host size).
#include <cassert>
#include <cstdio>
#include <vector>

#include "../gpu/shim/gow3_atlas_resample.h"

using namespace Gow3Atlas;

int main() {
    // Z order inside an 8x8 micro tile of 4-byte pixels, tiles in rows of 256 bytes.
    assert(Thin32Offset(0, 0, 16) == 0);
    assert(Thin32Offset(1, 0, 16) == 4);
    assert(Thin32Offset(0, 1, 16) == 8);
    assert(Thin32Offset(2, 0, 16) == 16);
    assert(Thin32Offset(0, 2, 16) == 32);
    assert(Thin32Offset(4, 0, 16) == 64);
    assert(Thin32Offset(0, 4, 16) == 128);
    assert(Thin32Offset(7, 7, 16) == 63 * 4);
    assert(Thin32Offset(8, 0, 16) == 256);
    assert(Thin32Offset(0, 8, 16) == 512);

    // Detile: every texel of a tiled 16x16 surface lands where the linear image has it.
    const uint32_t w = 16, h = 16;
    std::vector<uint8_t> tiled(w * h * 4), want(w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            for (uint32_t c = 0; c < 4; ++c) {
                const uint8_t v = static_cast<uint8_t>(x * 13 + y * 7 + c * 61);
                want[(y * w + x) * 4 + c] = v;
                tiled[Thin32Offset(x, y, w) + c] = v;
            }
        }
    }
    std::vector<uint8_t> got(w * h * 4);
    DetileThin32(tiled.data(), w, h, got.data());
    assert(got == want);

    // Linear aligned with a wider pitch.
    std::vector<uint8_t> padded(24 * h * 4, 0xEE);
    for (uint32_t y = 0; y < h; ++y) {
        std::copy_n(want.data() + y * w * 4, w * 4, padded.data() + y * 24 * 4);
    }
    ReadLinear32(padded.data(), 24, w, h, got.data());
    assert(got == want);

    // Upscale: size, a flat image stays flat, a linear ramp stays a ramp away from the edges.
    for (uint32_t scale : {2u, 4u}) {
        std::vector<uint8_t> flat(w * h * 4, 77);
        const auto big = UpscaleBicubic(flat.data(), w, h, scale);
        assert(big.size() == static_cast<size_t>(w) * scale * h * scale * 4);
        for (uint8_t v : big) {
            assert(v == 77);
        }
        std::vector<uint8_t> ramp(w * h * 4, 0);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                ramp[(y * w + x) * 4] = static_cast<uint8_t>(x * 16);
            }
        }
        const auto r = UpscaleBicubic(ramp.data(), w, h, scale);
        for (uint32_t X = 3 * scale; X < (w - 3) * scale; ++X) {
            const float expect = ((static_cast<float>(X) + 0.5f) / static_cast<float>(scale) - 0.5f) * 16.0f;
            const float have = r[((8 * scale) * (w * scale) + X) * 4];
            assert(have > expect - 1.0f && have < expect + 1.0f);
        }
    }
    // A bright texel keeps its brightness next to the middle of its block (output pixel 34 of source
    // texel 8 at scale 4 sits 0.125 texel from its centre: 0.964 per axis) and never overshoots 255.
    std::vector<uint8_t> dot(w * h * 4, 0);
    for (int i = 0; i < 4; ++i) {
        dot[(8 * w + 8) * 4 + i] = 255;
    }
    const auto d = UpscaleBicubic(dot.data(), w, h, 4);
    const int centre = d[((34) * (w * 4) + 34) * 4];
    assert(centre >= 232 && centre <= 240);

    // Contrast: a step edge keeps its 0.5 contour, gets steeper, and flat areas stay flat.
    std::vector<uint8_t> step(w * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = w / 2; x < w; ++x) {
            for (uint32_t c = 0; c < 4; ++c) {
                step[(y * w + x) * 4 + c] = 255;
            }
        }
    }
    const auto soft = UpscaleBicubic(step.data(), w, h, 4);
    const auto sharp = UpscaleBicubic(step.data(), w, h, 4, 3.0f);
    auto width_of_ramp = [&](const std::vector<uint8_t>& img) {
        int n = 0;
        for (uint32_t X = 0; X < w * 4; ++X) {
            const int v = img[(8 * (w * 4) + X) * 4];
            n += v > 12 && v < 243;
        }
        return n;
    };
    assert(width_of_ramp(sharp) < width_of_ramp(soft));
    assert(sharp[(8 * (w * 4) + 2) * 4] == 0 && sharp[(8 * (w * 4) + w * 4 - 3) * 4] == 255);

    // Box downsample: block mean (rounded), size, and a 4x nearest copy of an image comes back exactly.
    std::vector<uint8_t> blocks(4 * 4 * 4, 0);
    for (uint32_t i = 0; i < 4; ++i) {
        blocks[(0 * 4 + i) * 4] = 10; // row 0 = 10, row 1 = 11: block mean 10.5 rounds to 11
        blocks[(1 * 4 + i) * 4] = 11;
    }
    const auto half = BoxDownsample(blocks.data(), 4, 4, 2);
    assert(half.size() == 2 * 2 * 4 && half[0] == 11 && half[4] == 11 && half[8] == 0);
    std::vector<uint8_t> big4(w * 4 * h * 4 * 4);
    for (uint32_t Y = 0; Y < h * 4; ++Y) {
        for (uint32_t X = 0; X < w * 4; ++X) {
            std::copy_n(want.data() + ((Y / 4) * w + X / 4) * 4, 4, big4.data() + (Y * w * 4 + X) * 4);
        }
    }
    assert(BoxDownsample(big4.data(), w * 4, h * 4, 4) == want);

    // Mean absolute error: identical 0, one byte off by 4 in 8 is 0.5.
    uint8_t ma[8] = {0, 10, 20, 30, 40, 50, 60, 70}, mb[8] = {4, 10, 20, 30, 40, 50, 60, 70};
    assert(MeanAbsError(ma, ma, 8) == 0.0f && MeanAbsError(ma, mb, 8) == 0.5f);

    // Outliers: only differences strictly above the limit count.
    uint8_t oa[4] = {0, 0, 0, 0}, ob[4] = {48, 49, 0, 255};
    assert(OutlierFraction(oa, ob, 4, 48) == 0.5f && OutlierFraction(oa, oa, 4, 48) == 0.0f);

    std::puts("atlas resample ok");
    return 0;
}
