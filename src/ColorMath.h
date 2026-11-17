#pragma once
// Small shared color-science helpers used by the CPU tonemap and the encoders.
// The HLSL tonemap in ShaderTonemap.cpp mirrors the gamut-mapping and dither
// helpers here - keep them in sync.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sundial::color {

// Luminance (Y) weights of linear RGB for a given set of primaries (D65).
struct LumaWeights {
    float r, g, b;
};
inline constexpr LumaWeights kLuma709{0.2126f, 0.7152f, 0.0722f};
inline constexpr LumaWeights kLumaP3{0.2289746f, 0.6917385f, 0.0792869f};
inline constexpr LumaWeights kLuma2020{0.2627002f, 0.6779981f, 0.0593017f};

inline float Luma(float r, float g, float b, const LumaWeights& w) {
    return w.r * r + w.g * g + w.b * b;
}

// Linear primaries conversions (D65 white, no adaptation needed).
inline void Rec709ToDisplayP3(float& r, float& g, float& b) {
    const float r2 = 0.8224621f * r + 0.1775380f * g + 0.0000000f * b;
    const float g2 = 0.0331941f * r + 0.9668058f * g + 0.0000000f * b;
    const float b2 = 0.0170827f * r + 0.0723974f * g + 0.9105199f * b;
    r = r2; g = g2; b = b2;
}
inline void Rec709ToRec2020(float& r, float& g, float& b) {
    const float r2 = 0.6274039f * r + 0.3292830f * g + 0.0433131f * b;
    const float g2 = 0.0690973f * r + 0.9195404f * g + 0.0113623f * b;
    const float b2 = 0.0163914f * r + 0.0880133f * g + 0.8955953f * b;
    r = r2; g = g2; b = b2;
}
inline void Rec2020ToRec709(float& r, float& g, float& b) {
    const float r7 = 1.6604910f * r - 0.5876411f * g - 0.0728499f * b;
    const float g7 = -0.1245505f * r + 1.1328999f * g - 0.0083494f * b;
    const float b7 = -0.0181508f * r - 0.1005789f * g + 1.1187297f * b;
    r = r7; g = g7; b = b7;
}
inline void DisplayP3ToRec709(float& r, float& g, float& b) {
    const float r7 = 1.2249401f * r - 0.2249401f * g;
    const float g7 = -0.0420569f * r + 1.0420569f * g;
    const float b7 = -0.0196376f * r - 0.0786361f * g + 1.0982737f * b;
    r = r7; g = g7; b = b7;
}

// Bring a linear color with negative components (scRGB stores colors outside
// the working gamut that way) inside the gamut's lower boundary by
// desaturating toward its own luminance, holding luminance (and hue) fixed.
// This replaces a per-channel clamp to 0, which shifts hue (saturated reds go
// orange, greens go yellow) and brightens the color.
inline void MapNegativesIntoGamut(float& r, float& g, float& b,
                                  const LumaWeights& w) {
    const float Y = Luma(r, g, b, w);
    if (Y <= 0.0f) {
        r = g = b = 0.0f;
        return;
    }
    const float minc = std::min({r, g, b});
    if (minc >= 0.0f) {
        return;
    }
    const float t = Y / (Y - minc);  // in (0, 1): scales min channel to 0
    r = Y + (r - Y) * t;
    g = Y + (g - Y) * t;
    b = Y + (b - Y) * t;
}

// Fit a linear color into the [0, 1] RGB cube without clipping channels
// independently: negatives as above, then any channel above 1 is brought down
// by desaturating toward luminance (again holding luminance and hue). Colors
// whose luminance is itself >= 1 become white. Used as the last step before
// output encoding, so luminance-ratio curves (BT.2390 / Preserve SDR) and
// saturation boosts never hard-clip saturated highlights into flat patches.
inline void FitIntoGamut(float& r, float& g, float& b, const LumaWeights& w) {
    MapNegativesIntoGamut(r, g, b, w);
    const float Y = Luma(r, g, b, w);
    if (Y >= 1.0f) {
        r = g = b = 1.0f;
        return;
    }
    const float maxc = std::max({r, g, b});
    if (maxc <= 1.0f) {
        return;
    }
    const float t = (1.0f - Y) / (maxc - Y);
    r = Y + (r - Y) * t;
    g = Y + (g - Y) * t;
    b = Y + (b - Y) * t;
}

// Interleaved gradient noise (Jimenez 2014) in [0, 1). Cheap, well
// distributed, and evaluated identically by the HLSL preview. `k` decorrelates
// the three channels.
inline float DitherNoise(uint32_t x, uint32_t y, uint32_t k) {
    const float fx = float(x + 17u * k);
    const float fy = float(y + 59u * k);
    float v = 0.06711056f * fx + 0.00583715f * fy;
    v -= std::floor(v);
    v *= 52.9829189f;
    return v - std::floor(v);
}

// Amplitude of the rectangular dither added before 8-bit quantization, in
// LSBs (peak). Kept below 0.5 so values that already sit on (or within 0.1 LSB
// of) an 8-bit code - flat UI, text, SDR content - are never perturbed and
// stay pixel-exact, while smooth HDR gradients still get their banding broken
// up.
inline constexpr float kDitherAmplitudeLsb = 0.4f;

// SMPTE ST 2084 (PQ). Normalized so 1.0 == 10000 cd/m^2 at both ends.
inline float PqOetf(float l) {  // linear [0,1] -> PQ signal [0,1]
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    l = std::clamp(l, 0.0f, 1.0f);
    const float lm1 = std::pow(l, m1);
    return std::pow((c1 + c2 * lm1) / (1.0f + c3 * lm1), m2);
}
inline float PqEotf(float e) {  // PQ signal [0,1] -> linear [0,1]
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    e = std::clamp(e, 0.0f, 1.0f);
    const float ep = std::pow(e, 1.0f / m2);
    const float num = std::max(ep - c1, 0.0f);
    const float den = std::max(c2 - c3 * ep, 1e-10f);
    return std::pow(num / den, 1.0f / m1);
}
// HLG (ARIB STD-B67) inverse OETF: signal [0,1] -> scene linear [0,1].
inline float HlgInvOetf(float e) {
    constexpr float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    e = std::max(0.0f, e);
    if (e <= 0.5f) {
        return (e * e) / 3.0f;
    }
    return (std::exp((e - c) / a) + b) / 12.0f;
}

}  // namespace sundial::color
