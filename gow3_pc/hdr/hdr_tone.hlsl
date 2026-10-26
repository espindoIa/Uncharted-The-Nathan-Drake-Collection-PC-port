// HDR tone replacement epilogue: the port's own code plus RenoDX (MIT, renodx/). It holds no
// game code. At run time gpu/shim/gow3_hdr_tone.cpp links HdrTone into the translated tone pass
// pixel shader (guest hash 3371ae8d) that the shader recompiler produced from the player's own
// game files, and calls it with the values it finds there. hdr_tone.spv is built from this file
// by build.ps1.
//
// Live settings use binding 12; the guest bindings 0-11 stay with the translated shader.
[[vk::binding(12, 0)]] ByteAddressBuffer HdrSettings;
#define RENODX_PEAK_WHITE_NITS asfloat(HdrSettings.Load(0))
#define RENODX_DIFFUSE_WHITE_NITS asfloat(HdrSettings.Load(4))
#include "draw.hlsl"

// Native filmic curve: (x * c5 + c6) / (x * (x + c0) + c1) + c4, before its 0-1 clamp.
float RenoDXNativeFilmic(float x, float c0, float c1, float c4, float c5, float c6)
{
    return ((x * c5) + c6) / ((x * (x + c0)) + c1) + c4;
}

// Gain that maps scene-linear input onto the native filmic response at the scene value
// whose filmic output is linear mid-grey. Falls back to the gain at scene mid-grey if the
// curve does not bracket mid-grey on [0, 64].
float RenoDXFilmicAnchorGain(float c0, float c1, float c4, float c5, float c6)
{
    const float target = 0.18f;
    float lo = 0.0f;
    float hi = 64.0f;
    if (RenoDXNativeFilmic(lo, c0, c1, c4, c5, c6) >= target || RenoDXNativeFilmic(hi, c0, c1, c4, c5, c6) <= target)
    {
        return max(RenoDXNativeFilmic(target, c0, c1, c4, c5, c6), 0.0f) / target;
    }
    [loop]
    for (int i = 0; i < 32; i++)
    {
        float mid = 0.5f * (lo + hi);
        if (RenoDXNativeFilmic(mid, c0, c1, c4, c5, c6) < target)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return target / (0.5f * (lo + hi));
}

// scene: the value the game's filmic curve takes (scene-linear); neutral: the curve's 0-1
// output (equal to scene when the curve is not taken, filmic false); color: the game's graded SDR
// result (gamma encoded); c0..c6: the curve's constants (unused when filmic is false).
// The HDR result is returned gamma 2.2 encoded, where the game writes its graded SDR value, so
// the next pass (0x92A23F78, which raises it to the power 2.2) reads it unchanged and yields
// linear HDR.
// Below the mid-grey anchor the native filmic output is used as-is, so the toe and grade stay
// vanilla; above it the gain-matched scene signal extends the highlights.
export float3 HdrTone(float3 scene, float3 neutral, float3 color, bool filmic,
                      float c0, float c1, float c4, float c5, float c6)
{
    float gain = 1.0f;
    if (filmic)
    {
        gain = RenoDXFilmicAnchorGain(c0, c1, c4, c5, c6);
    }
    float3 anchored = max(neutral, scene * gain);
    float anchor_weight = saturate(renodx::color::y::from::BT709(neutral) / 0.18f);
    anchor_weight *= anchor_weight;
    float3 untonemapped = lerp(neutral, anchored, anchor_weight);
    float3 hdr = renodx::draw::ToneMapPass(untonemapped, pow(max(color, 0.0f), 2.2f), neutral);
    return sign(hdr) * pow(abs(hdr), 1.0f / 2.2f);
}
