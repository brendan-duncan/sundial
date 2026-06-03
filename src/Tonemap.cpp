#include "Tonemap.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>

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

// Luminance-only hue-preserving soft knee. Operates on Y = dot(c, Rec.709
// weights) rather than max(r,g,b), so the chromaticity stays exactly fixed
// while only the brightness is compressed. Mirrors the HLSL version in
// ShaderTonemap.cpp - keep both in sync.
void ApplyPreserveSdr(float& r, float& g, float& b, float knee) {
    const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
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

// ITU-R BT.2390 EETF (Electro-Optical Transfer Function) in PQ space. This is
// the reference HDR-to-SDR tone-mapping curve Microsoft and the broadcast
// industry use. We operate on luminance only and scale RGB by the resulting
// ratio so hue is preserved; per-channel BT.2390 would clip wide-gamut
// highlights to weird hues. Mirror in ShaderTonemap.cpp.
//
// PQ encoding: maps linear nits [0, 10000] -> normalized [0, 1] via SMPTE
// ST.2084. scRGB convention: 1.0 = 80 nits, so L_pq = scRGB * 80 / 10000.
float PqOetf(float L) {
    // L in [0, 1] where 1 = 10000 nits.
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    L = std::max(0.0f, L);
    const float Lm1 = std::pow(L, m1);
    return std::pow((c1 + c2 * Lm1) / (1.0f + c3 * Lm1), m2);
}

float PqEotf(float E) {
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    E = std::clamp(E, 0.0f, 1.0f);
    const float Em2 = std::pow(E, 1.0f / m2);
    const float num = std::max(Em2 - c1, 0.0f);
    const float den = c2 - c3 * Em2;
    return std::pow(num / std::max(den, 1e-10f), 1.0f / m1);
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

void ApplyBt2390(float& r, float& g, float& b,
                 float sourcePeakNits, float targetPeakNits) {
    // scRGB linear -> nits; cap at 10000 (PQ ceiling).
    const float Y_scrgb = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (Y_scrgb <= 0.0f) {
        r = std::max(0.0f, r); g = std::max(0.0f, g); b = std::max(0.0f, b);
        return;
    }
    const float Y_nits = std::min(Y_scrgb * 80.0f, 10000.0f);

    // Normalize the source range so the curve always operates in [0, 1]:
    // E2 = PQ(L) / PQ(sourcePeak). At E2 = 1 the input is at source peak.
    const float pqSrcPeak = PqOetf(std::min(sourcePeakNits, 10000.0f) / 10000.0f);
    const float pqTgtPeak = PqOetf(std::min(targetPeakNits, 10000.0f) / 10000.0f);
    const float maxLum = std::min(pqTgtPeak / std::max(pqSrcPeak, 1e-6f), 1.0f);

    const float E1 = PqOetf(Y_nits / 10000.0f) / std::max(pqSrcPeak, 1e-6f);
    const float E2 = std::clamp(E1, 0.0f, 1.0f);

    // Knee starts at the spec-recommended 1.5*Lw - 0.5 (clamped to >= 0).
    const float ks = std::max(0.0f, 1.5f * maxLum - 0.5f);
    const float E3 = Bt2390Knee(E2, ks, maxLum);

    // Back to linear nits, then back to scRGB.
    const float Y_out_pq = E3 * pqSrcPeak;
    const float Y_out_nits = PqEotf(Y_out_pq) * 10000.0f;
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
void ApplyHighlightDesat(float& r, float& g, float& b,
                         float knee, float strength) {
    knee = std::clamp(knee, 0.05f, 0.95f);
    strength = std::clamp(strength, 0.0f, 1.0f);
    const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (Y <= knee) {
        return;
    }
    const float t = std::clamp((Y - knee) / std::max(1.0f - knee, 1e-6f),
                               0.0f, 1.0f);
    const float w = strength * (t * t * (3.0f - 2.0f * t));  // smoothstep
    r = r + (Y - r) * w;
    g = g + (Y - g) * w;
    b = b + (Y - b) * w;
}

void ApplyCurveTriplet(float& r, float& g, float& b, TonemapCurve curve,
                       float kneePoint, float highlightDesat,
                       float sourcePeakNits) {
    if (curve == TonemapCurve::AgX) {
        ApplyAgX(r, g, b);
    } else if (curve == TonemapCurve::Neutral) {
        ApplyNeutral(r, g, b);
    } else if (curve == TonemapCurve::PreserveSdr) {
        ApplyPreserveSdr(r, g, b, kneePoint);
        ApplyHighlightDesat(r, g, b, kneePoint, highlightDesat);
        r = std::clamp(r, 0.0f, 1.0f);
        g = std::clamp(g, 0.0f, 1.0f);
        b = std::clamp(b, 0.0f, 1.0f);
    } else if (curve == TonemapCurve::BT2390) {
        // For BT.2390 the source-peak scale already lives in scRGB units; we
        // pass 80 nits as the target (= scRGB 1.0) so the curve maps anything
        // up to sourcePeakNits into [0, 1].
        ApplyBt2390(r, g, b, sourcePeakNits, 80.0f);
        ApplyHighlightDesat(r, g, b, kneePoint, highlightDesat);
        r = std::clamp(r, 0.0f, 1.0f);
        g = std::clamp(g, 0.0f, 1.0f);
        b = std::clamp(b, 0.0f, 1.0f);
    } else {
        r = ApplyCurveScalar(r, curve);
        g = ApplyCurveScalar(g, curve);
        b = ApplyCurveScalar(b, curve);
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

uint8_t ToByte(float x) {
    return static_cast<uint8_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f);
}

}  // namespace

std::vector<uint8_t> TonemapToBgra8(const Frame& hdr,
                                    const TonemapParams& params) {
    std::vector<uint8_t> out(size_t(hdr.width) * hdr.height * 4);

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

    auto preCurve = [&](float& r, float& g, float& b) {
        // White-balance shift (rough approximation, sufficient for tweaks).
        r *= 1.0f + temp * 0.30f + tint * 0.15f;
        g *= 1.0f                  - tint * 0.30f;
        b *= 1.0f - temp * 0.30f + tint * 0.15f;

        // Per-channel gain.
        r *= params.rGain;
        g *= params.gGain;
        b *= params.bGain;

        // Exposure (whiteScale * 2^EV).
        r *= scale;
        g *= scale;
        b *= scale;

        // Pre-curve highlight knee: pull >0.7 values toward 1 less steeply.
        if (rolloff > 0.0f) {
            auto roll = [rolloff](float x) {
                constexpr float knee = 0.7f;
                if (x <= knee) {
                    return x;
                }
                return knee + (x - knee) * (1.0f - rolloff);
            };
            r = roll(r); g = roll(g); b = roll(b);
        }
    };

    auto postCurve = [&](float& r, float& g, float& b) {
        if (lift > 0.0f) {
            r = r + lift * (1.0f - r);
            g = g + lift * (1.0f - g);
            b = b + lift * (1.0f - b);
        }

        if (sat != 1.0f) {
            const float gray = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            r = gray + (r - gray) * sat;
            g = gray + (g - gray) * sat;
            b = gray + (b - gray) * sat;
        }

        // Gamut compression: pull saturated, out-of-bounds colors toward
        // gray. Cheap approximation (the proper ACES gamut compressor is
        // significantly more involved).
        if (gamut > 0.0f) {
            const float maxc = std::max({r, g, b});
            const float minc = std::min({r, g, b});
            if (maxc > 1.0f || minc < 0.0f) {
                const float gray =
                    0.2126f * r + 0.7152f * g + 0.0722f * b;
                r = gray + (r - gray) * (1.0f - gamut);
                g = gray + (g - gray) * (1.0f - gamut);
                b = gray + (b - gray) * (1.0f - gamut);
            }
        }

        r = ApplyOutputGamma(r, params.outputGamma);
        g = ApplyOutputGamma(g, params.outputGamma);
        b = ApplyOutputGamma(b, params.outputGamma);
    };

    auto srgbToLinear = [](float c) {
        return c <= 0.04045f ? c / 12.92f
                             : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    auto readLinear = [&](uint32_t y, uint32_t x,
                          float& r, float& g, float& b) {
        const size_t i = (size_t(y) * hdr.width + x) * 4;
        if (hdr.isHdr) {
            const uint16_t* src =
                reinterpret_cast<const uint16_t*>(hdr.pixels.data());
            r = std::max(0.0f, HalfToFloat(src[i + 0]));
            g = std::max(0.0f, HalfToFloat(src[i + 1]));
            b = std::max(0.0f, HalfToFloat(src[i + 2]));
        } else {
            const uint8_t* src = hdr.pixels.data();
            b = srgbToLinear(src[i + 0] / 255.0f);
            g = srgbToLinear(src[i + 1] / 255.0f);
            r = srgbToLinear(src[i + 2] / 255.0f);
        }
    };

    const float sharp = std::clamp(params.sharpen, 0.0f, 1.0f);
    const float localStrength = std::clamp(params.localStrength, 0.0f, 1.0f);

    // Local tonemap: compute the curve's compression ratio on a global
    // average of the source and apply that ratio to each pixel before
    // blending toward the per-pixel global curve. We use a single average
    // (no spatial blur) to match the GPU path, which samples the smallest
    // mip of the source - this avoids the haloing artifacts a finer blur
    // produces around bright HDR regions. The "local" name is preserved for
    // continuity; a proper edge-aware (bilateral / Laplacian-pyramid) version
    // is tracked in the README as future work.
    float ratio_r = 1.0f, ratio_g = 1.0f, ratio_b = 1.0f;
    if (localStrength > 0.0f) {
        double sumR = 0, sumG = 0, sumB = 0;
        const size_t total = size_t(hdr.width) * hdr.height;
        for (uint32_t y = 0; y < hdr.height; ++y) {
            for (uint32_t x = 0; x < hdr.width; ++x) {
                float r, g, b;
                readLinear(y, x, r, g, b);
                sumR += r; sumG += g; sumB += b;
            }
        }
        float avgR = float(sumR / std::max<size_t>(total, 1));
        float avgG = float(sumG / std::max<size_t>(total, 1));
        float avgB = float(sumB / std::max<size_t>(total, 1));
        preCurve(avgR, avgG, avgB);
        float curveR = avgR, curveG = avgG, curveB = avgB;
        ApplyCurveTriplet(curveR, curveG, curveB, params.curve,
                          params.kneePoint, params.highlightDesat,
                          params.sourcePeakNits);
        ratio_r = curveR / std::max(avgR, 1e-5f);
        ratio_g = curveG / std::max(avgG, 1e-5f);
        ratio_b = curveB / std::max(avgB, 1e-5f);
    }

    for (uint32_t y = 0; y < hdr.height; ++y) {
        for (uint32_t x = 0; x < hdr.width; ++x) {
            float r, g, b;
            readLinear(y, x, r, g, b);
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
                if (y + 1 < hdr.height) {
                    acc(y + 1, x);
                }
                if (x > 0) {
                    acc(y, x - 1);
                }
                if (x + 1 < hdr.width) {
                    acc(y, x + 1);
                }
                if (count > 0) {
                    const float inv = 1.0f / float(count);
                    const float amt = sharp * 2.0f;
                    r += (r - rs * inv) * amt;
                    g += (g - gs * inv) * amt;
                    b += (b - bs * inv) * amt;
                    r = std::max(0.0f, r);
                    g = std::max(0.0f, g);
                    b = std::max(0.0f, b);
                }
            }
            preCurve(r, g, b);

            if (localStrength > 0.0f) {
                const float r_local = std::clamp(r * ratio_r, 0.0f, 1.0f);
                const float g_local = std::clamp(g * ratio_g, 0.0f, 1.0f);
                const float b_local = std::clamp(b * ratio_b, 0.0f, 1.0f);
                float r_global = r, g_global = g, b_global = b;
                ApplyCurveTriplet(r_global, g_global, b_global, params.curve,
                                  params.kneePoint, params.highlightDesat,
                                  params.sourcePeakNits);
                r = r_global + (r_local - r_global) * localStrength;
                g = g_global + (g_local - g_global) * localStrength;
                b = b_global + (b_local - b_global) * localStrength;
            } else {
                ApplyCurveTriplet(r, g, b, params.curve, params.kneePoint,
                                  params.highlightDesat, params.sourcePeakNits);
            }

            postCurve(r, g, b);
            const size_t i = (size_t(y) * hdr.width + x) * 4;
            out[i + 0] = ToByte(b);
            out[i + 1] = ToByte(g);
            out[i + 2] = ToByte(r);
            out[i + 3] = 255;
        }
    }
    return out;
}

float AutoSdrWhite(const Frame& hdr) {
    if (!hdr.isHdr) {
        return 80.0f;
    }

    // Build a 256-bin histogram of luminance in scRGB units (1.0 = 80 nits)
    // and return the 99th-percentile bin center scaled to nits.
    constexpr int kBins = 256;
    constexpr float kMaxScRgb = 12.5f;  // ~1000 nits, covers most HDR content
    int hist[kBins] = {};
    const uint16_t* src = reinterpret_cast<const uint16_t*>(hdr.pixels.data());
    const size_t total = size_t(hdr.width) * hdr.height;
    for (size_t i = 0; i < total; ++i) {
        float r = std::max(0.0f, HalfToFloat(src[i * 4 + 0]));
        float g = std::max(0.0f, HalfToFloat(src[i * 4 + 1]));
        float b = std::max(0.0f, HalfToFloat(src[i * 4 + 2]));
        float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        int bin =
            std::clamp(int(lum / kMaxScRgb * (kBins - 1)), 0, kBins - 1);
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
    const float pctScRgb = (pctBin + 0.5f) * kMaxScRgb / kBins;
    // Map 99th percentile to scRGB 1.0 -> sdrWhiteNits = pctScRgb * 80.
    float nits = pctScRgb * 80.0f;
    return std::clamp(nits, 40.0f, 400.0f);
}

}  // namespace sundial
