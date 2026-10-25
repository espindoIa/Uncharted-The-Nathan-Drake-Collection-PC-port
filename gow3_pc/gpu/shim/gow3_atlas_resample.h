#pragma once
// Pure pixel code of gow3_atlas.cpp (no engine types, so tests/test_atlas_resample.cpp builds it alone).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Gow3Atlas {

// 32-bit thin 1D-tiled surface (TileMode::Thin1DThin, 13): 8x8 micro tiles of 256 bytes in rows, the
// pixel inside a tile in Z order (x0 y0 x1 y1 x2 y2; the same as tiling.comp's thin micro tiling).
inline uint32_t Thin32Offset(uint32_t x, uint32_t y, uint32_t width) {
    const uint32_t tile = (y >> 3) * (width >> 3) + (x >> 3);
    const uint32_t index = (x & 1) | ((y & 1) << 1) | (((x >> 1) & 1) << 2) | (((y >> 1) & 1) << 3) |
                           (((x >> 2) & 1) << 4) | (((y >> 2) & 1) << 5);
    return tile * 256 + index * 4;
}

// Linear RGBA (width * height * 4 bytes) from a Thin1DThin surface (width and height multiples of 8).
inline void DetileThin32(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst) {
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* s = src + Thin32Offset(x, y, width);
            uint8_t* d = dst + (static_cast<size_t>(y) * width + x) * 4;
            d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = s[3];
        }
    }
}

// Linear RGBA from a linear-aligned surface with `pitch` pixels per row.
inline void ReadLinear32(const uint8_t* src, uint32_t pitch, uint32_t width, uint32_t height, uint8_t* dst) {
    for (uint32_t y = 0; y < height; ++y) {
        std::copy_n(src + static_cast<size_t>(y) * pitch * 4, static_cast<size_t>(width) * 4,
                    dst + static_cast<size_t>(y) * width * 4);
    }
}

// RGBA8 shrunk by an integer factor: every output texel is the rounded mean of a factor x factor block
// (exact for coverage; width and height are the source's, multiples of factor).
inline std::vector<uint8_t> BoxDownsample(const uint8_t* src, uint32_t width, uint32_t height, uint32_t factor) {
    const uint32_t out_w = width / factor, out_h = height / factor;
    std::vector<uint8_t> out(static_cast<size_t>(out_w) * out_h * 4);
    for (uint32_t y = 0; y < out_h; ++y) {
        for (uint32_t x = 0; x < out_w; ++x) {
            for (uint32_t c = 0; c < 4; ++c) {
                uint32_t sum = 0;
                for (uint32_t j = 0; j < factor; ++j) {
                    for (uint32_t i = 0; i < factor; ++i) {
                        sum += src[(static_cast<size_t>(y * factor + j) * width + x * factor + i) * 4 + c];
                    }
                }
                out[(static_cast<size_t>(y) * out_w + x) * 4 + c] = static_cast<uint8_t>((sum + factor * factor / 2) / (factor * factor));
            }
        }
    }
    return out;
}

// Mean absolute difference (in 0..255 levels) over `count` bytes.
inline float MeanAbsError(const uint8_t* a, const uint8_t* b, size_t count) {
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        sum += static_cast<uint64_t>(std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return count ? static_cast<float>(static_cast<double>(sum) / static_cast<double>(count)) : 0.0f;
}

// Fraction of the `count` bytes where a and b differ by more than `limit` levels.
inline float OutlierFraction(const uint8_t* a, const uint8_t* b, size_t count, int limit) {
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        n += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > limit;
    }
    return count ? static_cast<float>(n) / static_cast<float>(count) : 0.0f;
}

// Catmull-Rom weights for the four taps around a position `t` in [0, 1) past the second tap.
inline void CubicWeights(float t, float (&w)[4]) {
    w[0] = ((-0.5f * t + 1.0f) * t - 0.5f) * t;
    w[1] = (1.5f * t - 2.5f) * t * t + 1.0f;
    w[2] = ((-1.5f * t + 2.0f) * t + 0.5f) * t;
    w[3] = (0.5f * t - 0.5f) * t * t;
}

// RGBA8 upscaled by an integer factor with a separable Catmull-Rom filter (pixel centres aligned, edges
// clamped), then every channel's contrast around 0.5 raised by `contrast` (1 = none): a coverage
// edge one source texel wide becomes 1 / contrast source texels wide, its 0.5 contour kept. This is
// the glyph reconstruction of font atlases (a font-aware one, contours fitted to the coverage, goes here).
inline std::vector<uint8_t> UpscaleBicubic(const uint8_t* src, uint32_t width, uint32_t height, uint32_t scale,
                                           float contrast = 1.0f) {
    const uint32_t out_w = width * scale, out_h = height * scale;
    // Horizontal pass into floats (height rows), then vertical.
    std::vector<float> mid(static_cast<size_t>(out_w) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t X = 0; X < out_w; ++X) {
            const float pos = (static_cast<float>(X) + 0.5f) / static_cast<float>(scale) - 0.5f;
            const float base = std::floor(pos);
            float w[4];
            CubicWeights(pos - base, w);
            for (uint32_t c = 0; c < 4; ++c) {
                float v = 0.0f;
                for (int tap = 0; tap < 4; ++tap) {
                    const int sx = std::clamp(static_cast<int>(base) - 1 + tap, 0, static_cast<int>(width) - 1);
                    v += w[tap] * src[(static_cast<size_t>(y) * width + sx) * 4 + c];
                }
                mid[(static_cast<size_t>(y) * out_w + X) * 4 + c] = v;
            }
        }
    }
    std::vector<uint8_t> out(static_cast<size_t>(out_w) * out_h * 4);
    for (uint32_t Y = 0; Y < out_h; ++Y) {
        const float pos = (static_cast<float>(Y) + 0.5f) / static_cast<float>(scale) - 0.5f;
        const float base = std::floor(pos);
        float w[4];
        CubicWeights(pos - base, w);
        for (uint32_t X = 0; X < out_w; ++X) {
            for (uint32_t c = 0; c < 4; ++c) {
                float v = 0.0f;
                for (int tap = 0; tap < 4; ++tap) {
                    const int sy = std::clamp(static_cast<int>(base) - 1 + tap, 0, static_cast<int>(height) - 1);
                    v += w[tap] * mid[(static_cast<size_t>(sy) * out_w + X) * 4 + c];
                }
                v = std::clamp(v, 0.0f, 255.0f);
                if (contrast != 1.0f) {
                    v = std::clamp((v - 127.5f) * contrast + 127.5f, 0.0f, 255.0f);
                }
                out[(static_cast<size_t>(Y) * out_w + X) * 4 + c] = static_cast<uint8_t>(v + 0.5f);
            }
        }
    }
    return out;
}

} // namespace Gow3Atlas
