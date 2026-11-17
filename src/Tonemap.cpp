#include "Tonemap.h"

#include "ColorMath.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace sundial {
namespace {

float HalfToFloat(uint16_t h) {
    return DirectX::PackedVector::XMConvertHalfToFloat(
        static_cast<DirectX::PackedVector::HALF>(h));
}

// Curves --------------------------------------------------------------------

float LinearClip(float x) { return std::clamp(x, 0.0f, 1.0f); }

float Reinhard(float x) {
    x = std::max(0.0f, x);
    return x / (1.0f + x);
}

float Aces(float x) {
    // Narkowicz 2015 filmic approximation.
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    const float num = x * (a * x + b);
    const float den = x * (c * x + d) + e;
    return std::clamp(num / den, 0.0f, 1.0f);
}

float HablePartial(float x) {
    constexpr float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f;
    constexpr float E = 0.02f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float Hable(float x) {
    constexpr float kWhite = 11.2f;
    static const float kWhiteScale = 1.0f / HablePartial(kWhite);
    return std::clamp(HablePartial(x) * kWhiteScale, 0.0f, 1.0f);
}

// AgX (Blender-derived) - uses 3x3 input/output matrices and a log-space
// polynomial sigmoid. Operates on the RGB triplet as a whole rather than
// per-channel, so we expose ApplyAgX() instead of a scalar ApplyCurve case.
void AgXContrastApprox(float& r, float& g, float& b) {
    auto poly = [](float x) {
        float x2 = x * x;
        float x4 = x2 * x2;
        return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 -
               6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    };
    r = poly(r);
    g = poly(g);
    b = poly(b);
}

// Khronos PBR Neutral - operates on the RGB triplet as a whole.
void ApplyNeutral(float& r, float& g, float& b) {
    constexpr float kStart = 0.76f;   // 0.8 - 0.04
    constexpr float kDesat = 0.15f;
    float minC = std::min({r, g, b});
    float offset =
        (minC < 0.08f) ? (minC - 6.25f * minC * minC) : 0.04f;
    r -= offset;
    g -= offset;
    b -= offset;
    float peak = std::max({r, g, b});
    if (peak >= kStart) {
        const float d = 1.0f - kStart;
        const float newPeak = 1.0f - d * d / (peak + d - kStart);
        const float scaleNp = newPeak / std::max(peak, 1e-10f);
        r *= scaleNp;
        g *= scaleNp;
        b *= scaleNp;
        const float gFactor =
            1.0f - 1.0f / (kDesat * (peak - newPeak) + 1.0f);
        r = r + (newPeak - r) * gFactor;
        g = g + (newPeak - g) * gFactor;
        b = b + (newPeak - b) * gFactor;
    }
    r = std::clamp(r, 0.0f, 1.0f);
    g = std::clamp(g, 0.0f, 1.0f);
    b = std::clamp(b, 0.0f, 1.0f);
}

void ApplyAgX(float& r, float& g, float& b) {
    // Linear sRGB -> AgX working space
    constexpr float Min[3][3] = {
        {0.84247906f, 0.0784336f, 0.07922375f},
        {0.04232824f, 0.87846864f, 0.07916613f},
        {0.04237565f, 0.0784336f, 0.87914297f}};
    float r2 = Min[0][0] * r + Min[0][1] * g + Min[0][2] * b;
    float g2 = Min[1][0] * r + Min[1][1] * g + Min[1][2] * b;
    float b2 = Min[2][0] * r + Min[2][1] * g + Min[2][2] * b;
    r = r2;
    g = g2;
    b = b2;

    // Log encoding, range -12.47393 EV .. +4.026069 EV (~16.5 stops)
    constexpr float kMin = -12.47393f;
    constexpr float kMax = 4.026069f;
    constexpr float kRange = kMax - kMin;
    auto enc = [&](float v) {
        v = std::log2(std::max(v, 1e-10f));
        return std::clamp((v - kMin) / kRange, 0.0f, 1.0f);
    };
    r = enc(r);
    g = enc(g);
    b = enc(b);
    AgXContrastApprox(r, g, b);

    // AgX -> Linear sRGB
    constexpr float Mout[3][3] = {
        {1.19687900f, -0.09802088f, -0.09902974f},
        {-0.05289685f, 1.15190313f, -0.09896118f},
        {-0.05297164f, -0.09804345f, 1.15107367f}};
    r2 = Mout[0][0] * r + Mout[0][1] * g + Mout[0][2] * b;
    g2 = Mout[1][0] * r + Mout[1][1] * g + Mout[1][2] * b;
    b2 = Mout[2][0] * r + Mout[2][1] * g + Mout[2][2] * b;
    r = std::clamp(r2, 0.0f, 1.0f);
    g = std::clamp(g2, 0.0f, 1.0f);
    b = std::clamp(b2, 0.0f, 1.0f);
}

float ApplyCurveScalar(float x, TonemapCurve curve) {
    switch (curve) {
        case TonemapCurve::Reinhard: return Reinhard(x);
        case TonemapCurve::Aces:     return Aces(x);
        case TonemapCurve::Hable:    return Hable(x);
        case TonemapCurve::LinearClip:
        default:                     return LinearClip(x);
    }
}

using color::LumaWeights;

// Luminance-only hue-preserving soft knee. Operates on Y rather than
// max(r,g,b), so the chromaticity stays exactly fixed while only the
// brightness is compressed. Channels may still exceed 1.0 afterward for very
// saturated colors; FitIntoGamut() resolves that at the end of the pipeline
// without a per-channel clip. Mirrors the HLSL version in ShaderTonemap.cpp -
// keep both in sync.
void ApplyPreserveSdr(float& r, float& g, float& b, float knee,
                      const LumaWeights& w) {
    const float Y = color::Luma(r, g, b, w);
    if (Y <= 0.0f) {
        r = std::max(0.0f, r);
        g = std::max(0.0f, g);
        b = std::max(0.0f, b);
        return;
    }
    knee = std::clamp(knee, 0.05f, 0.95f);
    float Ymapped;
    if (Y <= knee) {
        Ymapped = Y;
    } else {
        const float over = Y - knee;
        const float range = 1.0f - knee;
        Ymapped = knee + over * range / (over + range);
    }
    const float s = Ymapped / Y;
    r *= s; g *= s; b *= s;
}

// BT.2390 Hermite knee on PQ-encoded values. ks is the knee start, Lmax is
// the PQ value of the target peak (i.e. PQ(1.0) for an SDR display normalized
// to its own peak).
float Bt2390Knee(float E, float ks, float Lmax) {
    if (E < ks) {
        return E;
    }
    const float t = (E - ks) / std::max(1.0f - ks, 1e-6f);
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * ks
         + (t3 - 2.0f * t2 + t) * (1.0f - ks)
         + (-2.0f * t3 + 3.0f * t2) * Lmax;
}

// ITU-R BT.2390 EETF in PQ space, applied to luminance with RGB scaled by the
// resulting ratio so hue is preserved; per-channel BT.2390 would clip
// wide-gamut highlights to weird hues. scRGB convention: 1.0 = 80 nits.
// Mirror in ShaderTonemap.cpp.
void ApplyBt2390(float& r, float& g, float& b, float sourcePeakNits,
                 float targetPeakNits, const LumaWeights& w) {
    const float Y_scrgb = color::Luma(r, g, b, w);
    if (Y_scrgb <= 0.0f) {
        r = std::max(0.0f, r); g = std::max(0.0f, g); b = std::max(0.0f, b);
        return;
    }
    const float Y_nits = std::min(Y_scrgb * 80.0f, 10000.0f);

    // Normalize the source range so the curve always operates in [0, 1]:
    // E2 = PQ(L) / PQ(sourcePeak). At E2 = 1 the input is at source peak.
    const float pqSrcPeak =
        color::PqOetf(std::min(sourcePeakNits, 10000.0f) / 10000.0f);
    const float pqTgtPeak =
        color::PqOetf(std::min(targetPeakNits, 10000.0f) / 10000.0f);
    const float maxLum =
        std::min(pqTgtPeak / std::max(pqSrcPeak, 1e-6f), 1.0f);

    const float E1 =
        color::PqOetf(Y_nits / 10000.0f) / std::max(pqSrcPeak, 1e-6f);
    const float E2 = std::clamp(E1, 0.0f, 1.0f);

    // Knee starts at the spec-recommended 1.5*Lw - 0.5 (clamped to >= 0).
    const float ks = std::max(0.0f, 1.5f * maxLum - 0.5f);
    const float E3 = Bt2390Knee(E2, ks, maxLum);

    // Back to linear nits, then back to scRGB.
    const float Y_out_nits = color::PqEotf(E3 * pqSrcPeak) * 10000.0f;
    const float Y_out_scrgb = Y_out_nits / 80.0f;

    // Apply the same ratio to RGB so hue is preserved.
    const float s = Y_out_scrgb / std::max(Y_scrgb, 1e-6f);
    r *= s; g *= s; b *= s;
}

// Hunt-effect highlight desaturation: lerp bright colors toward grayscale so
// the sun reads as white-yellow instead of saturated orange. Strength ramps
// up smoothly with luminance above the knee. Applied as a post-curve step
// for PreserveSdr and BT2390; other curves do their own desat (Neutral) or
// don't need it (clipping curves desaturate by virtue of clipping channels).
void ApplyHighlightDesat(float& r, float& g, float& b, float knee,
                         float strength, const LumaWeights& w) {
    knee = std::clamp(knee, 0.05f, 0.95f);
    strength = std::clamp(strength, 0.0f, 1.0f);
    const float Y = color::Luma(r, g, b, w);
    if (Y <= knee) {
        return;
    }
    const float t = std::clamp((Y - knee) / std::max(1.0f - knee, 1e-6f),
                               0.0f, 1.0f);
    const float s = strength * (t * t * (3.0f - 2.0f * t));  // smoothstep
    r = r + (Y - r) * s;
    g = g + (Y - g) * s;
    b = b + (Y - b) * s;
}

// Apply the selected curve. The luminance-ratio curves (PreserveSdr, BT2390)
// deliberately do NOT clamp per channel here: a saturated highlight can leave
// one channel above 1.0, and the final FitIntoGamut() brings it back by
// desaturating at constant luminance instead of flattening it.
void ApplyCurveTriplet(float& r, float& g, float& b, const TonemapParams& p,
                       const LumaWeights& w) {
    switch (p.curve) {
        case TonemapCurve::AgX:
            ApplyAgX(r, g, b);
            break;
        case TonemapCurve::Neutral:
            ApplyNeutral(r, g, b);
            break;
        case TonemapCurve::PreserveSdr:
            ApplyPreserveSdr(r, g, b, p.kneePoint, w);
            ApplyHighlightDesat(r, g, b, p.kneePoint, p.highlightDesat, w);
            break;
        case TonemapCurve::BT2390:
            // Target 80 nits (= scRGB 1.0): the curve maps anything up to
            // sourcePeakNits into [0, 1] in SDR-white-normalized units.
            ApplyBt2390(r, g, b, p.sourcePeakNits, 80.0f, w);
            ApplyHighlightDesat(r, g, b, p.kneePoint, p.highlightDesat, w);
            break;
        default:
            r = ApplyCurveScalar(r, p.curve);
            g = ApplyCurveScalar(g, p.curve);
            b = ApplyCurveScalar(b, p.curve);
            break;
    }
}

float LinearToSrgb(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.0031308f ? 12.92f * x
                           : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

float ApplyOutputGamma(float x, OutputGamma g) {
    switch (g) {
        case OutputGamma::Gamma22:
            return std::pow(std::clamp(x, 0.0f, 1.0f), 1.0f / 2.2f);
        case OutputGamma::Linear:
            return std::clamp(x, 0.0f, 1.0f);
        case OutputGamma::Srgb:
        default:
            return LinearToSrgb(x);
    }
}

// Quantize an encoded [0, 1] value to 8 bits with a small rectangular dither
// (see color::kDitherAmplitudeLsb). `noise` is in [0, 1).
uint8_t QuantizeDithered(float encoded, float noise) {
    const float v = std::clamp(encoded, 0.0f, 1.0f) * 255.0f +
                    (noise - 0.5f) * (2.0f * color::kDitherAmplitudeLsb);
    return static_cast<uint8_t>(
        std::clamp(std::floor(v + 0.5f), 0.0f, 255.0f));
}

}  // namespace

std::vector<uint8_t> TonemapToBgra8(const Frame& hdr,
                                    const TonemapParams& params) {
    const uint32_t W = hdr.width, H = hdr.height;
    std::vector<uint8_t> out(size_t(W) * H * 4);
    if (W == 0 || H == 0) {
        return out;
    }

    // Working space: the output primaries. Everything from the gamut mapping
    // onward (curve, saturation, final fit) happens in these primaries, so a
    // Display P3 output keeps colors that sRGB can't hold.
    const bool p3 = params.outputGamut == OutputGamut::DisplayP3;
    const LumaWeights& wl = p3 ? color::kLumaP3 : color::kLuma709;

    // scRGB convention: 1.0 = 80 nits. The "SDR white" slider lets the user
    // pick how bright the SDR-white anchor should be relative to that; we
    // scale the scene to put that anchor at tonemap input 1.0.
    const float whiteScale = 80.0f / std::max(1.0f, params.sdrWhiteNits);
    const float exposure = std::pow(2.0f, params.exposureEV);
    const float scale = whiteScale * exposure;
    const float sat = params.saturation;

    const float lift = std::clamp(params.blackPointLift, 0.0f, 1.0f);
    const float rolloff = std::clamp(params.highlightRolloff, 0.0f, 1.0f);
    const float temp = std::clamp(params.temperature, -1.0f, 1.0f);
    const float tint = std::clamp(params.tint, -1.0f, 1.0f);
    const float gamut = std::clamp(params.gamutCompress, 0.0f, 1.0f);

    // Pre-curve highlight knee: pull >0.7 values toward 1 less steeply.
    auto roll = [rolloff](float x) {
        constexpr float knee = 0.7f;
        if (rolloff <= 0.0f || x <= knee) {
            return x;
        }
        return knee + (x - knee) * (1.0f - rolloff);
    };

    // Scene-linear Rec.709 adjustments, then conversion to the working
    // primaries and a hue-preserving map of out-of-gamut (negative) colors.
    auto preCurve = [&](float& r, float& g, float& b) {
        // White-balance shift (rough approximation, sufficient for tweaks).
        r *= 1.0f + temp * 0.30f + tint * 0.15f;
        g *= 1.0f                  - tint * 0.30f;
        b *= 1.0f - temp * 0.30f + tint * 0.15f;

        // Per-channel gain, then exposure (whiteScale * 2^EV).
        r *= params.rGain * scale;
        g *= params.gGain * scale;
        b *= params.bGain * scale;

        r = roll(r); g = roll(g); b = roll(b);

        if (p3) {
            color::Rec709ToDisplayP3(r, g, b);
        }
        color::MapNegativesIntoGamut(r, g, b, wl);
    };

    auto postCurve = [&](float& r, float& g, float& b) {
        if (lift > 0.0f) {
            r = r + lift * (1.0f - r);
            g = g + lift * (1.0f - g);
            b = b + lift * (1.0f - b);
        }

        if (sat != 1.0f) {
            const float gray = color::Luma(r, g, b, wl);
            r = gray + (r - gray) * sat;
            g = gray + (g - gray) * sat;
            b = gray + (b - gray) * sat;
        }

        // Optional extra pull of out-of-bounds colors toward gray (a softer,
        // more desaturated look than the exact fit below).
        if (gamut > 0.0f) {
            const float maxc = std::max({r, g, b});
            const float minc = std::min({r, g, b});
            if (maxc > 1.0f || minc < 0.0f) {
                const float gray = color::Luma(r, g, b, wl);
                r = gray + (r - gray) * (1.0f - gamut);
                g = gray + (g - gray) * (1.0f - gamut);
                b = gray + (b - gray) * (1.0f - gamut);
            }
        }

        color::FitIntoGamut(r, g, b, wl);

        r = ApplyOutputGamma(r, params.outputGamma);
        g = ApplyOutputGamma(g, params.outputGamma);
        b = ApplyOutputGamma(b, params.outputGamma);
    };

    auto srgbToLinear = [](float c) {
        return c <= 0.04045f ? c / 12.92f
                             : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    // Raw linear Rec.709 read. HDR (scRGB) values keep their sign: negative
    // components are how scRGB encodes colors outside sRGB, and clamping them
    // here would shift hue before the gamut mapping gets a chance to run.
    auto readLinear = [&](uint32_t y, uint32_t x,
                          float& r, float& g, float& b) {
        const size_t i = (size_t(y) * W + x) * 4;
        if (hdr.isHdr) {
            const uint16_t* src =
                reinterpret_cast<const uint16_t*>(hdr.pixels.data());
            r = HalfToFloat(src[i + 0]);
            g = HalfToFloat(src[i + 1]);
            b = HalfToFloat(src[i + 2]);
        } else {
            const uint8_t* src = hdr.pixels.data();
            b = srgbToLinear(src[i + 0] / 255.0f);
            g = srgbToLinear(src[i + 1] / 255.0f);
            r = srgbToLinear(src[i + 2] / 255.0f);
        }
    };

    // Bitwise comparison of two source pixels' color (alpha ignored).
    const size_t bpp = hdr.bytesPerPixel;
    const size_t colorBytes = hdr.isHdr ? 6 : 3;
    auto samePixel = [&](uint32_t y0, uint32_t x0, uint32_t y1, uint32_t x1) {
        const uint8_t* p = hdr.pixels.data();
        return std::memcmp(p + (size_t(y0) * W + x0) * bpp,
                           p + (size_t(y1) * W + x1) * bpp, colorBytes) == 0;
    };

    const float sharp = std::clamp(params.sharpen, 0.0f, 1.0f);
    const float localStrength = std::clamp(params.localStrength, 0.0f, 1.0f);

    // Local tonemap. A coarse grid of mean log2-luminance (the "base" layer,
    // ~32-63 cells on the long side) is upsampled per pixel with a joint
    // bilateral filter: cells whose brightness differs a lot from the pixel's
    // own get little weight, so a bright HDR video next to dark UI doesn't
    // smear its compression across the edge (no halos). The curve is then
    // evaluated on that base luminance and its compression ratio applied to
    // the pixel, which preserves local detail inside each bright/dark region.
    // Mirrors the GPU preview in ShaderTonemap.cpp.
    const int level = LocalTonemapLevel(W, H);
    const uint32_t cw = std::max(1u, W >> level);
    const uint32_t ch = std::max(1u, H >> level);
    std::vector<float> coarse;
    if (localStrength > 0.0f) {
        std::vector<double> sum(size_t(cw) * ch, 0.0);
        std::vector<uint32_t> cnt(size_t(cw) * ch, 0);
        for (uint32_t y = 0; y < H; ++y) {
            const uint32_t cy =
                std::min(ch - 1, uint32_t((y + 0.5) * ch / H));
            for (uint32_t x = 0; x < W; ++x) {
                const uint32_t cx =
                    std::min(cw - 1, uint32_t((x + 0.5) * cw / W));
                float r, g, b;
                readLinear(y, x, r, g, b);
                const size_t c = size_t(cy) * cw + cx;
                sum[c] += std::log2(
                    std::max(color::Luma(r, g, b, color::kLuma709), 1e-6f));
                ++cnt[c];
            }
        }
        coarse.resize(sum.size());
        for (size_t c = 0; c < sum.size(); ++c) {
            coarse[c] = cnt[c] ? float(sum[c] / cnt[c]) : -20.0f;
        }
    }
    const float invTwoSigmaS2 =
        1.0f / (2.0f * kLocalSigmaSpatial * kLocalSigmaSpatial);
    const float invTwoSigmaR2 =
        1.0f / (2.0f * kLocalSigmaRange * kLocalSigmaRange);
    auto localBaseLog = [&](uint32_t x, uint32_t y, float lp) {
        const float fx = (x + 0.5f) * float(cw) / float(W) - 0.5f;
        const float fy = (y + 0.5f) * float(ch) / float(H) - 0.5f;
        const int i0 = int(std::floor(fx));
        const int j0 = int(std::floor(fy));
        float sw = 0.0f, sv = 0.0f;
        for (int dj = -1; dj <= 2; ++dj) {
            const int j = std::clamp(j0 + dj, 0, int(ch) - 1);
            const float dy = float(j0 + dj) - fy;
            for (int di = -1; di <= 2; ++di) {
                const int i = std::clamp(i0 + di, 0, int(cw) - 1);
                const float dx = float(i0 + di) - fx;
                const float lt = coarse[size_t(j) * cw + i];
                const float dl = lt - lp;
                const float wgt =
                    std::exp(-(dx * dx + dy * dy) * invTwoSigmaS2 -
                             dl * dl * invTwoSigmaR2);
                sw += wgt;
                sv += wgt * lt;
            }
        }
        return sw > 1e-20f ? sv / sw : lp;
    };

    // Rows are independent (sharpen and the local base only read the source),
    // so split them across cores: a 4K HDR frame takes ~1s single-threaded,
    // which is too slow for the frozen-screen overlay shown at hotkey time.
    auto processRows = [&](uint32_t yBegin, uint32_t yEnd) {
        for (uint32_t y = yBegin; y < yEnd; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                float r, g, b;
                readLinear(y, x, r, g, b);
                const float rawY = color::Luma(r, g, b, color::kLuma709);
                if (sharp > 0.0f) {
                    float rs = 0, gs = 0, bs = 0;
                    int count = 0;
                    auto acc = [&](uint32_t yy, uint32_t xx) {
                        float rr, gg, bb;
                        readLinear(yy, xx, rr, gg, bb);
                        rs += rr; gs += gg; bs += bb; ++count;
                    };
                    if (y > 0) {
                        acc(y - 1, x);
                    }
                    if (y + 1 < H) {
                        acc(y + 1, x);
                    }
                    if (x > 0) {
                        acc(y, x - 1);
                    }
                    if (x + 1 < W) {
                        acc(y, x + 1);
                    }
                    if (count > 0) {
                        const float inv = 1.0f / float(count);
                        const float amt = sharp * 2.0f;
                        r += (r - rs * inv) * amt;
                        g += (g - gs * inv) * amt;
                        b += (b - bs * inv) * amt;
                    }
                }
                preCurve(r, g, b);

                if (localStrength > 0.0f) {
                    // Curve response at the (edge-aware) local base luminance.
                    const float lp = std::log2(std::max(rawY, 1e-6f));
                    const float base =
                        roll(std::exp2(localBaseLog(x, y, lp)) * scale);
                    float br = base, bg = base, bb = base;
                    ApplyCurveTriplet(br, bg, bb, params, wl);
                    const float ratio =
                        std::max(color::Luma(br, bg, bb, wl), 0.0f) /
                        std::max(base, 1e-6f);
                    float lr = r * ratio, lg = g * ratio, lb = b * ratio;
                    // Detail brighter than its surroundings can push past
                    // white; roll it off softly rather than clipping.
                    ApplyPreserveSdr(lr, lg, lb, params.kneePoint, wl);

                    ApplyCurveTriplet(r, g, b, params, wl);
                    r += (lr - r) * localStrength;
                    g += (lg - g) * localStrength;
                    b += (lb - b) * localStrength;
                } else {
                    ApplyCurveTriplet(r, g, b, params, wl);
                }

                postCurve(r, g, b);
                const size_t i = (size_t(y) * W + x) * 4;
                // Dither only where the source varies: a flat region (UI
                // background, solid fills) can't band, and dithering it
                // would just speckle a value that falls between two codes.
                const bool flat = (x == 0 || samePixel(y, x, y, x - 1)) &&
                                  (x + 1 == W || samePixel(y, x, y, x + 1)) &&
                                  (y == 0 || samePixel(y, x, y - 1, x)) &&
                                  (y + 1 == H || samePixel(y, x, y + 1, x));
                if (flat) {
                    out[i + 0] = QuantizeDithered(b, 0.5f);
                    out[i + 1] = QuantizeDithered(g, 0.5f);
                    out[i + 2] = QuantizeDithered(r, 0.5f);
                } else {
                    out[i + 0] = QuantizeDithered(b, color::DitherNoise(x, y, 2));
                    out[i + 1] = QuantizeDithered(g, color::DitherNoise(x, y, 1));
                    out[i + 2] = QuantizeDithered(r, color::DitherNoise(x, y, 0));
                }
                out[i + 3] = 255;
            }
        }
    };

    const uint32_t threadCount = std::clamp<uint32_t>(
        std::thread::hardware_concurrency(), 1u,
        std::max<uint32_t>(1u, H / 64));
    if (threadCount <= 1) {
        processRows(0, H);
        return out;
    }
    std::vector<std::thread> workers;
    workers.reserve(threadCount - 1);
    const uint32_t band = (H + threadCount - 1) / threadCount;
    for (uint32_t t = 1; t < threadCount; ++t) {
        const uint32_t y0 = std::min(H, t * band);
        const uint32_t y1 = std::min(H, y0 + band);
        workers.emplace_back(processRows, y0, y1);
    }
    processRows(0, std::min(H, band));
    for (auto& w : workers) {
        w.join();
    }
    return out;
}

float AutoSdrWhite(const Frame& hdr) {
    if (!hdr.isHdr) {
        return 80.0f;
    }

    // Histogram of luminance on a log2 scale (scRGB units, 1.0 = 80 nits),
    // 32 bins per stop from 2^-8 (~0.3 nits) to 2^7 (~10240 nits), so bright
    // 1500-4000 nit displays don't all pile into the top bin. Returns the
    // 99th-percentile luminance as the SDR-white anchor.
    constexpr float kMinLog = -8.0f;
    constexpr float kMaxLog = 7.0f;
    constexpr int kBinsPerStop = 32;
    constexpr int kBins = int((kMaxLog - kMinLog) * kBinsPerStop);
    std::vector<size_t> hist(kBins, 0);
    const uint16_t* src = reinterpret_cast<const uint16_t*>(hdr.pixels.data());
    const size_t total = size_t(hdr.width) * hdr.height;
    for (size_t i = 0; i < total; ++i) {
        const float r = HalfToFloat(src[i * 4 + 0]);
        const float g = HalfToFloat(src[i * 4 + 1]);
        const float b = HalfToFloat(src[i * 4 + 2]);
        const float lum = color::Luma(r, g, b, color::kLuma709);
        const float l2 = std::log2(std::max(lum, 1e-8f));
        const int bin = std::clamp(int((l2 - kMinLog) * kBinsPerStop), 0,
                                   kBins - 1);
        hist[bin]++;
    }
    const size_t target = (total * 99) / 100;
    size_t cum = 0;
    int pctBin = kBins - 1;
    for (int i = 0; i < kBins; ++i) {
        cum += hist[i];
        if (cum >= target) {
            pctBin = i;
            break;
        }
    }
    const float pctScRgb =
        std::exp2(kMinLog + (pctBin + 0.5f) / float(kBinsPerStop));
    // Map 99th percentile to scRGB 1.0 -> sdrWhiteNits = pctScRgb * 80.
    return std::clamp(pctScRgb * 80.0f, kMinSdrWhiteNits, kMaxSdrWhiteNits);
}

}  // namespace sundial

