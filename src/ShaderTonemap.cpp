#include "ShaderTonemap.h"

#include "Tonemap.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace sundial {
namespace {

constexpr char kShaderSrc[] = R"HLSL(
cbuffer TonemapCB : register(b0) {
    float  uExposure;       // pre-scale (= sdrWhiteScale * 2^EV)
    float  uSaturation;
    int    uCurve;          // 0=Linear 1=Reinhard 2=ACES 3=Hable 4=AgX 5=Neutral 6=PreserveSdr 7=BT2390
    int    uIsHdr;          // 1: source is FP16 linear scRGB; 0: source is sRGB BGRA
    int    uOutputGamma;    // 0=sRGB 1=Gamma22 2=Linear
    int    uPassthrough;    // 1: skip curve+sat+exposure (HDR comparison view)
    float  uBlackPointLift; // 0..1, post-curve shadow lift
    float  uHighlightRolloff; // 0..1, pre-curve highlight knee
    float  uTemperature;    // -1..1
    float  uTint;           // -1..1
    float  uGamutCompress;  // 0..1
    float  uSharpen;        // 0..1
    float  uRGain;
    float  uGGain;
    float  uBGain;
    int    uLinearOutput;   // 1: skip output gamma + clamp (FP16 RT)
    float  uLocalStrength;  // 0..1, blend toward local tonemap
    float  uLocalLod;       // mip level to sample for the local-average blur
    float  uKneePoint;      // 0..1, PreserveSdr/BT2390 knee start
    float  uHighlightDesat; // 0..1, Hunt-effect highlight desaturation
    float  uSourcePeakNits; // BT2390 source peak luminance in nits
    int    uHasLocal;       // 1: gLogLum is bound (local tonemap available)
    int    uDither;         // 1: dither before the 8-bit render target
    float  _pad0;           // pad to a 16-byte boundary for D3D11
};

Texture2D        gSource : register(t0);
Texture2D<float> gLogLum : register(t1);  // log2 luminance + mip chain
SamplerState     gSamp   : register(s0);

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);

// --- Mirrors of ColorMath.h (keep in sync) ---------------------------------

// Desaturate toward luminance (luminance + hue held) just enough to remove
// negative components, instead of clamping channels independently.
float3 MapNegativesIntoGamut(float3 c) {
    float Y = dot(c, kLuma709);
    if (Y <= 0.0) return float3(0, 0, 0);
    float minc = min(c.r, min(c.g, c.b));
    if (minc >= 0.0) return c;
    float t = Y / (Y - minc);
    return Y + (c - Y) * t;
}

// Fit into the [0,1] cube the same way at both ends: no per-channel clip.
float3 FitIntoGamut(float3 c) {
    c = MapNegativesIntoGamut(c);
    float Y = dot(c, kLuma709);
    if (Y >= 1.0) return float3(1, 1, 1);
    float maxc = max(c.r, max(c.g, c.b));
    if (maxc <= 1.0) return c;
    float t = (1.0 - Y) / (maxc - Y);
    return Y + (c - Y) * t;
}

// Interleaved gradient noise in [0,1); k decorrelates channels.
float DitherNoise(uint2 p, uint k) {
    float2 f = float2(p + uint2(17u, 59u) * k);
    return frac(52.9829189 * frac(0.06711056 * f.x + 0.00583715 * f.y));
}
static const float kDitherAmplitudeLsb = 0.4;
// ---------------------------------------------------------------------------

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// Full-screen triangle, no vertex buffer required.
VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 SrgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 LinearToSrgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0/2.4) - 0.055;
}

float3 ApplyOutputGamma(float3 c) {
    if (uOutputGamma == 1) return pow(saturate(c), 1.0/2.2);
    if (uOutputGamma == 2) return saturate(c);
    return LinearToSrgb(c);
}

float3 ApplyReinhard(float3 c) { return c / (1.0 + c); }
float3 ApplyAces(float3 c) {
    float3 num = c * (2.51 * c + 0.03);
    float3 den = c * (2.43 * c + 0.59) + 0.14;
    return saturate(num / den);
}
float HablePartial(float x) {
    const float A=0.15, B=0.50, C=0.10, D=0.20, E=0.02, F=0.30;
    return ((x*(A*x + C*B) + D*E) / (x*(A*x + B) + D*F)) - E/F;
}
float3 ApplyHable(float3 c) {
    const float W = 11.2;
    float scale = 1.0 / HablePartial(W);
    return saturate(float3(
        HablePartial(c.r) * scale,
        HablePartial(c.g) * scale,
        HablePartial(c.b) * scale));
}

float3 AgXContrastApprox(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4
         - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 ApplyAgX(float3 c) {
    const float3x3 Min = float3x3(
        0.84247906, 0.0784336,  0.07922375,
        0.04232824, 0.87846864, 0.07916613,
        0.04237565, 0.0784336,  0.87914297);
    const float3x3 Mout = float3x3(
        1.19687900, -0.09802088, -0.09902974,
       -0.05289685,  1.15190313, -0.09896118,
       -0.05297164, -0.09804345,  1.15107367);
    const float kMin = -12.47393;
    const float kRange = 4.026069 - (-12.47393);
    c = mul(Min, c);
    c = log2(max(c, 1e-10));
    c = saturate((c - kMin) / kRange);
    c = AgXContrastApprox(c);
    c = mul(Mout, c);
    return saturate(c);
}

float3 ApplyNeutral(float3 c) {
    const float kStart = 0.76;
    const float kDesat = 0.15;
    float minC = min(c.r, min(c.g, c.b));
    float offset = (minC < 0.08) ? (minC - 6.25 * minC * minC) : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak >= kStart) {
        float d = 1.0 - kStart;
        float newPeak = 1.0 - d * d / (peak + d - kStart);
        c *= newPeak / max(peak, 1e-10);
        float gFactor = 1.0 - 1.0 / (kDesat * (peak - newPeak) + 1.0);
        c = c + (newPeak - c) * gFactor;
    }
    return saturate(c);
}

// PreserveSdr: luminance-only hue-preserving soft knee. Identity below knee;
// above it the luminance asymptotes to 1.0 while RGB is rescaled by the
// same ratio so chromaticity is preserved exactly.
float3 ApplyPreserveSdr(float3 c) {
    float Y = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (Y <= 0.0) return max(c, 0.0);
    float knee = clamp(uKneePoint, 0.05, 0.95);
    float Ymapped;
    if (Y <= knee) {
        Ymapped = Y;
    } else {
        float over = Y - knee;
        float range = 1.0 - knee;
        Ymapped = knee + over * range / (over + range);
    }
    return c * (Ymapped / Y);
}

// PQ OETF / EOTF (SMPTE ST.2084). PqOetf maps linear-light [0, 1] -> PQ
// encoded [0, 1] where the linear-light unit is normalized to 10000 nits.
float PqOetf(float L) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    L = max(L, 0.0);
    float Lm1 = pow(L, m1);
    return pow((c1 + c2 * Lm1) / (1.0 + c3 * Lm1), m2);
}
float PqEotf(float E) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    E = saturate(E);
    float Em2 = pow(E, 1.0 / m2);
    float num = max(Em2 - c1, 0.0);
    float den = max(c2 - c3 * Em2, 1e-10);
    return pow(num / den, 1.0 / m1);
}
float Bt2390Knee(float E, float ks, float Lmax) {
    if (E < ks) return E;
    float t = (E - ks) / max(1.0 - ks, 1e-6);
    float t2 = t * t;
    float t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * ks
         + (t3 - 2.0 * t2 + t) * (1.0 - ks)
         + (-2.0 * t3 + 3.0 * t2) * Lmax;
}

// ITU-R BT.2390 EETF in PQ space, applied to luminance with RGB rescaled by
// the resulting ratio so hue is preserved. Maps [0, sourcePeakNits] linearly
// onto SDR-display peak (treated as scRGB 1.0 = 80 nits target).
float3 ApplyBt2390(float3 c) {
    float Y_scrgb = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (Y_scrgb <= 0.0) return max(c, 0.0);
    float Y_nits = min(Y_scrgb * 80.0, 10000.0);

    float srcPeak = min(uSourcePeakNits, 10000.0);
    const float tgtPeakNits = 80.0;
    float pqSrcPeak = max(PqOetf(srcPeak / 10000.0), 1e-6);
    float pqTgtPeak = PqOetf(tgtPeakNits / 10000.0);
    float maxLum = min(pqTgtPeak / pqSrcPeak, 1.0);

    float E1 = PqOetf(Y_nits / 10000.0) / pqSrcPeak;
    float E2 = saturate(E1);
    float ks = max(0.0, 1.5 * maxLum - 0.5);
    float E3 = Bt2390Knee(E2, ks, maxLum);

    float Y_out_nits = PqEotf(E3 * pqSrcPeak) * 10000.0;
    float Y_out_scrgb = Y_out_nits / 80.0;
    return c * (Y_out_scrgb / max(Y_scrgb, 1e-6));
}

// Hunt-effect highlight desaturation. Smoothstep ramp from knee to 1.0;
// brightest pixels fade toward grayscale so the sun reads as white rather
// than a saturated chromatic blob. Applied as post-step for PreserveSdr/
// BT2390 only.
float3 ApplyHighlightDesat(float3 c) {
    float Y = dot(c, float3(0.2126, 0.7152, 0.0722));
    float knee = clamp(uKneePoint, 0.05, 0.95);
    if (Y <= knee) return c;
    float t = saturate((Y - knee) / max(1.0 - knee, 1e-6));
    float w = uHighlightDesat * (t * t * (3.0 - 2.0 * t));
    return lerp(c, float3(Y, Y, Y), w);
}

float3 ApplyCurve(float3 c) {
    if (uCurve == 1) return ApplyReinhard(c);
    if (uCurve == 2) return ApplyAces(c);
    if (uCurve == 3) return ApplyHable(c);
    if (uCurve == 4) return ApplyAgX(c);
    if (uCurve == 5) return ApplyNeutral(c);
    // Luminance-ratio curves: no per-channel clamp here - FitIntoGamut at
    // the end resolves saturated highlights at constant luminance.
    if (uCurve == 6) return ApplyHighlightDesat(ApplyPreserveSdr(c));
    if (uCurve == 7) return ApplyHighlightDesat(ApplyBt2390(c));
    return saturate(c);
}

// scRGB values keep their sign: negative components encode colors outside
// sRGB, and are gamut-mapped (not clamped) after the scene adjustments.
float3 ReadLinear(float2 uv) {
    float3 c = gSource.SampleLevel(gSamp, uv, 0).rgb;
    if (uIsHdr == 0) c = SrgbToLinear(c);
    return c;
}

float Roll(float x) {
    if (uHighlightRolloff <= 0 || x <= 0.7) return x;
    return x - (x - 0.7) * uHighlightRolloff;
}

// Joint-bilateral upsample of the coarse log-luminance level: 4x4 taps around
// the pixel's position, each weighted by spatial distance and by how close
// its log-luminance is to the pixel's own (lp). Mirrors TonemapToBgra8.
static const float kLocalSigmaSpatial = 1.0;
static const float kLocalSigmaRange = 1.0;
float LocalBaseLog(float2 uv, float lp) {
    int level = int(uLocalLod);
    uint cw, ch, levels;
    gLogLum.GetDimensions(level, cw, ch, levels);
    float2 f = uv * float2(cw, ch) - 0.5;
    int2 i0 = int2(floor(f));
    float invS = 1.0 / (2.0 * kLocalSigmaSpatial * kLocalSigmaSpatial);
    float invR = 1.0 / (2.0 * kLocalSigmaRange * kLocalSigmaRange);
    float sw = 0, sv = 0;
    [unroll] for (int dj = -1; dj <= 2; ++dj) {
        [unroll] for (int di = -1; di <= 2; ++di) {
            int2 t = i0 + int2(di, dj);
            int2 tc = clamp(t, int2(0, 0), int2(cw - 1, ch - 1));
            float2 d = float2(t) - f;
            float lt = gLogLum.Load(int3(tc, level));
            float dl = lt - lp;
            float w = exp(-dot(d, d) * invS - dl * dl * invR);
            sw += w;
            sv += w * lt;
        }
    }
    return sw > 1e-20 ? sv / sw : lp;
}

// Full-screen pass that fills gLogLum's top level (mips are then generated).
float PSLogLum(VSOut i) : SV_Target {
    float3 c = ReadLinear(i.uv);
    return log2(max(dot(c, kLuma709), 1e-6));
}

float4 PSMain(VSOut i) : SV_Target {
    float3 c = ReadLinear(i.uv);
    const float3 rawC = c;

    if (uPassthrough != 0) {
        // Linear-output mode: write raw scRGB values, unclipped (so an HDR
        // backbuffer can show >1.0 highlights). Otherwise clamp + sRGB encode
        // for an 8-bit SDR render target.
        if (uLinearOutput != 0) {
            return float4(c, 1.0);
        }
        return float4(ApplyOutputGamma(FitIntoGamut(c)), 1.0);
    }

    // Unsharp-mask sharpen using 4 neighbors of the source.
    if (uSharpen > 0) {
        uint w, h;
        gSource.GetDimensions(w, h);
        float2 px = float2(1.0 / w, 1.0 / h);
        float3 n = ReadLinear(i.uv + float2( px.x, 0))
                 + ReadLinear(i.uv + float2(-px.x, 0))
                 + ReadLinear(i.uv + float2(0,  px.y))
                 + ReadLinear(i.uv + float2(0, -px.y));
        c = c + (c - n * 0.25) * (uSharpen * 2.0);
    }

    // White balance: rough temperature/tint shift.
    c.r *= 1.0 + uTemperature * 0.30 + uTint * 0.15;
    c.g *= 1.0                        - uTint * 0.30;
    c.b *= 1.0 - uTemperature * 0.30 + uTint * 0.15;

    // Per-channel gain, exposure, pre-curve highlight knee (0.7).
    c *= float3(uRGain, uGGain, uBGain) * uExposure;
    c = float3(Roll(c.r), Roll(c.g), Roll(c.b));

    // Hue-preserving map of out-of-gamut (negative) scRGB colors.
    c = MapNegativesIntoGamut(c);

    // Local tonemap: evaluate the curve at the edge-aware local base
    // luminance and apply its compression ratio to this pixel, so detail
    // survives inside both bright HDR regions and dark UI next to them.
    if (uLocalStrength > 0 && uHasLocal != 0) {
        float lp = log2(max(dot(rawC, kLuma709), 1e-6));
        float base = Roll(exp2(LocalBaseLog(i.uv, lp)) * uExposure);
        float ratio = max(dot(ApplyCurve(float3(base, base, base)), kLuma709),
                          0.0) / max(base, 1e-6);
        float3 c_local = ApplyPreserveSdr(c * ratio);
        float3 c_global = ApplyCurve(c);
        c = lerp(c_global, c_local, uLocalStrength);
    } else {
        c = ApplyCurve(c);
    }

    if (uBlackPointLift > 0) {
        c = c + uBlackPointLift * (1.0 - c);
    }

    if (uSaturation != 1.0) {
        float gray = dot(c, kLuma709);
        c = lerp(float3(gray, gray, gray), c, uSaturation);
    }

    // Gamut compress: optional extra pull of oob colors toward gray.
    if (uGamutCompress > 0) {
        float maxc = max(c.r, max(c.g, c.b));
        float minc = min(c.r, min(c.g, c.b));
        if (maxc > 1.0 || minc < 0.0) {
            float gray = dot(c, kLuma709);
            c = lerp(c, float3(gray, gray, gray), uGamutCompress);
        }
    }

    c = ApplyOutputGamma(FitIntoGamut(c));

    // Break up 8-bit banding in smooth gradients; amplitude < 0.5 LSB keeps
    // flat UI/text pixel-exact. Matches the CPU path.
    // Flat source regions (identical neighbors) are left undithered, like the
    // CPU path, so solid UI stays clean.
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    int2 st = int2(i.uv * float2(srcW, srcH));
    int2 smax = int2(srcW - 1, srcH - 1);
    float3 s0 = gSource.Load(int3(clamp(st, 0, smax), 0)).rgb;
    bool flat =
        all(gSource.Load(int3(clamp(st + int2(1, 0), 0, smax), 0)).rgb == s0) &&
        all(gSource.Load(int3(clamp(st - int2(1, 0), 0, smax), 0)).rgb == s0) &&
        all(gSource.Load(int3(clamp(st + int2(0, 1), 0, smax), 0)).rgb == s0) &&
        all(gSource.Load(int3(clamp(st - int2(0, 1), 0, smax), 0)).rgb == s0);
    if (uDither != 0 && !flat) {
        uint2 p = uint2(i.pos.xy);
        float3 n = float3(DitherNoise(p, 0), DitherNoise(p, 1),
                          DitherNoise(p, 2)) - 0.5;
        c += n * (2.0 * kDitherAmplitudeLsb / 255.0);
    }

    return float4(c, 1.0);
}
)HLSL";

struct CBLayout {
    float exposure;
    float saturation;
    int curve;
    int isHdr;
    int outputGamma;
    int passthrough;
    float blackPointLift;
    float highlightRolloff;
    float temperature;
    float tint;
    float gamutCompress;
    float sharpen;
    float rGain;
    float gGain;
    float bGain;
    int linearOutput;
    float localStrength;
    float localLod;
    float kneePoint;
    float highlightDesat;
    float sourcePeakNits;
    int hasLocal;
    int dither;
    float _pad0;
};
static_assert(sizeof(CBLayout) % 16 == 0,
              "D3D11 constant buffers must be a multiple of 16 bytes");

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, "Tonemap.hlsl",
                            nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        // Silent failure - Initialize() returns false and the editor will
        // simply not show a preview. We avoid OutputDebugStringA spam here.
        return nullptr;
    }
    return blob;
}

}  // namespace

bool ShaderTonemap::Initialize(ID3D11Device* device,
                               ID3D11DeviceContext* context,
                               bool linearHdrOutput) {
    device_ = device;
    context_ = context;
    linearHdrOutput_ = linearHdrOutput;

    auto vsBlob = Compile("VSMain", "vs_5_0");
    auto psBlob = Compile("PSMain", "ps_5_0");
    auto lumBlob = Compile("PSLogLum", "ps_5_0");
    if (!vsBlob || !psBlob || !lumBlob) {
        return false;
    }
    if (FAILED(device_->CreatePixelShader(lumBlob->GetBufferPointer(),
                                          lumBlob->GetBufferSize(), nullptr,
                                          &psLogLum_))) {
        return false;
    }

    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                           vsBlob->GetBufferSize(), nullptr,
                                           &vs_))) {
        return false;
    }
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(),
                                          psBlob->GetBufferSize(), nullptr,
                                          &ps_))) {
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) {
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CBLayout);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, &rasterizer_))) {
        return false;
    }

    D3D11_BLEND_DESC bsd{};
    bsd.RenderTarget[0].BlendEnable = FALSE;
    bsd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bsd, &blendOpaque_))) {
        return false;
    }

    return true;
}

bool ShaderTonemap::SetSource(const Frame& frame) {
    sourceSrv_.Reset();
    logLumSrv_.Reset();
    logLumTex_.Reset();
    sourceIsHdr_ = frame.isHdr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = frame.width;
    td.Height = frame.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = frame.isHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                            : DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = frame.pixels.data();
    init.SysMemPitch = frame.width * frame.bytesPerPixel;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device_->CreateTexture2D(&td, &init, &tex))) {
        return false;
    }
    if (FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr,
                                                 &sourceSrv_))) {
        return false;
    }

    // Local tonemap base layer: render log2 luminance into an R16_FLOAT
    // texture and let the hardware build its mip chain (box-averaged log
    // luminance). Failure here only disables the local tonemap.
    D3D11_TEXTURE2D_DESC ld = td;
    ld.MipLevels = 0;  // full chain
    ld.Format = DXGI_FORMAT_R16_FLOAT;
    ld.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ld.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> lum;
    ComPtr<ID3D11RenderTargetView> lumRtv;
    ComPtr<ID3D11ShaderResourceView> lumSrv;
    if (SUCCEEDED(device_->CreateTexture2D(&ld, nullptr, &lum)) &&
        SUCCEEDED(device_->CreateRenderTargetView(lum.Get(), nullptr,
                                                  &lumRtv)) &&
        SUCCEEDED(device_->CreateShaderResourceView(lum.Get(), nullptr,
                                                    &lumSrv))) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                    &m))) {
            CBLayout cb{};
            cb.isHdr = sourceIsHdr_ ? 1 : 0;
            std::memcpy(m.pData, &cb, sizeof(cb));
            context_->Unmap(cb_.Get(), 0);
        }
        D3D11_VIEWPORT vp{};
        vp.Width = float(frame.width);
        vp.Height = float(frame.height);
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(rasterizer_.Get());
        ID3D11RenderTargetView* rtv = lumRtv.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        const float blendFactor[4] = {0, 0, 0, 0};
        context_->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(psLogLum_.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = {cb_.Get()};
        context_->PSSetConstantBuffers(0, 1, cbs);
        ID3D11ShaderResourceView* srvs[] = {sourceSrv_.Get(), nullptr};
        context_->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* samps[] = {sampler_.Get()};
        context_->PSSetSamplers(0, 1, samps);
        context_->Draw(3, 0);
        ID3D11RenderTargetView* nullRtv = nullptr;
        context_->OMSetRenderTargets(1, &nullRtv, nullptr);
        ID3D11ShaderResourceView* nullSrv[] = {nullptr};
        context_->PSSetShaderResources(0, 1, nullSrv);
        context_->GenerateMips(lumSrv.Get());

        D3D11_TEXTURE2D_DESC actual{};
        lum->GetDesc(&actual);
        localLevel_ = std::min(LocalTonemapLevel(frame.width, frame.height),
                               int(actual.MipLevels) - 1);
        logLumTex_ = lum;
        logLumSrv_ = lumSrv;
    }
    return true;
}

bool ShaderTonemap::SetSourceTexture(ID3D11Texture2D* tex, bool isHdr) {
    sourceSrv_.Reset();
    logLumSrv_.Reset();
    logLumTex_.Reset();
    sourceIsHdr_ = isHdr;
    // No log-luminance pyramid on this path: the recorder feeds full-res GPU
    // textures and the local tonemap isn't used during recording.
    if (!tex) {
        return false;
    }
    return SUCCEEDED(
        device_->CreateShaderResourceView(tex, nullptr, &sourceSrv_));
}

bool ShaderTonemap::ResizeTarget(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    if (width == outputW_ && height == outputH_ && outputSdrSrv_) {
        return true;
    }

    outputSdrRtv_.Reset(); outputSdrSrv_.Reset(); outputSdrTex_.Reset();
    outputHdrRtv_.Reset(); outputHdrSrv_.Reset(); outputHdrTex_.Reset();

    auto make = [&](ComPtr<ID3D11Texture2D>& tex,
                    ComPtr<ID3D11RenderTargetView>& rtv,
                    ComPtr<ID3D11ShaderResourceView>& srv,
                    DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) {
            return false;
        }
        if (FAILED(device_->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
            return false;
        }
        return true;
    };
    // SDR-tonemapped output stays 8-bit sRGB-encoded - the values are always
    // in [0,1] after tonemapping, so 8-bit is enough. HDR passthrough is FP16
    // when the editor is going to display on an HDR backbuffer (it needs
    // values above 1.0 to survive); 8-bit otherwise.
    const DXGI_FORMAT hdrFormat = linearHdrOutput_
                                      ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                      : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!make(outputSdrTex_, outputSdrRtv_, outputSdrSrv_,
              DXGI_FORMAT_R8G8B8A8_UNORM)) {
        return false;
    }
    if (!make(outputHdrTex_, outputHdrRtv_, outputHdrSrv_, hdrFormat)) {
        return false;
    }
    outputW_ = width;
    outputH_ = height;
    return true;
}

void ShaderTonemap::RenderSdr(const TonemapParams& params) {
    if (!sourceSrv_ || !outputSdrRtv_) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                &m))) {
        CBLayout cb{};
        const float whiteScale =
            80.0f / (params.sdrWhiteNits < 1.0f ? 1.0f : params.sdrWhiteNits);
        cb.exposure = whiteScale * std::pow(2.0f, params.exposureEV);
        cb.saturation = params.saturation;
        cb.curve = static_cast<int>(params.curve);
        cb.isHdr = sourceIsHdr_ ? 1 : 0;
        cb.outputGamma = static_cast<int>(params.outputGamma);
        cb.passthrough = 0;
        cb.blackPointLift = params.blackPointLift;
        cb.highlightRolloff = params.highlightRolloff;
        cb.temperature = params.temperature;
        cb.tint = params.tint;
        cb.gamutCompress = params.gamutCompress;
        cb.sharpen = params.sharpen;
        cb.rGain = params.rGain;
        cb.gGain = params.gGain;
        cb.bGain = params.bGain;
        cb.linearOutput = 0;  // SDR RT is always 8-bit sRGB-encoded
        cb.localStrength = std::clamp(params.localStrength, 0.0f, 1.0f);
        cb.localLod = float(localLevel_);
        cb.kneePoint = params.kneePoint;
        cb.highlightDesat = params.highlightDesat;
        cb.sourcePeakNits = params.sourcePeakNits;
        cb.hasLocal = logLumSrv_ ? 1 : 0;
        cb.dither = 1;  // SDR target is 8-bit
        std::memcpy(m.pData, &cb, sizeof(cb));
        context_->Unmap(cb_.Get(), 0);
    }
    SetPipelineAndDraw(outputSdrRtv_.Get());
}

void ShaderTonemap::RenderHdrPassthrough() {
    if (!sourceSrv_ || !outputHdrRtv_) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                &m))) {
        CBLayout cb{};
        cb.exposure = 1.0f;
        cb.saturation = 1.0f;
        cb.curve = 0;
        cb.isHdr = sourceIsHdr_ ? 1 : 0;
        cb.outputGamma = static_cast<int>(OutputGamma::Srgb);
        cb.passthrough = 1;
        cb.blackPointLift = 0.0f;
        cb.highlightRolloff = 0.0f;
        cb.temperature = 0.0f;
        cb.tint = 0.0f;
        cb.gamutCompress = 0.0f;
        cb.sharpen = 0.0f;
        cb.rGain = 1.0f;
        cb.gGain = 1.0f;
        cb.bGain = 1.0f;
        cb.linearOutput = linearHdrOutput_ ? 1 : 0;
        cb.localStrength = 0.0f;  // passthrough bypasses the curve entirely
        cb.localLod = 0.0f;
        cb.kneePoint = 0.75f;
        cb.highlightDesat = 0.0f;
        cb.sourcePeakNits = 1000.0f;
        cb.hasLocal = 0;
        cb.dither = 0;
        std::memcpy(m.pData, &cb, sizeof(cb));
        context_->Unmap(cb_.Get(), 0);
    }
    SetPipelineAndDraw(outputHdrRtv_.Get());
}

void ShaderTonemap::SetPipelineAndDraw(ID3D11RenderTargetView* rtv) {
    D3D11_VIEWPORT vp{};
    vp.Width = float(outputW_);
    vp.Height = float(outputH_);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(rasterizer_.Get());

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    const float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(rtv, black);
    const float blendFactor[4] = {0, 0, 0, 0};
    context_->OMSetBlendState(blendOpaque_.Get(), blendFactor, 0xFFFFFFFF);

    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {cb_.Get()};
    context_->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[] = {sourceSrv_.Get(), logLumSrv_.Get()};
    context_->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samps[] = {sampler_.Get()};
    context_->PSSetSamplers(0, 1, samps);

    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, nullSrv);
}

}  // namespace sundial
