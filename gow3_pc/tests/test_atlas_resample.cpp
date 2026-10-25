// Detile and bicubic upscale of gow3_atlas_resample.h (font atlases at a larger host size).
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

    std::puts("atlas resample ok");
    return 0;
}
