#include "ShaderTonemap.h"

#include <d3dcompiler.h>

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
    float  _pad0[3];        // pad to a 16-byte boundary for D3D11
};

Texture2D   gSource : register(t0);
SamplerState gSamp  : register(s0);

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
    if (uCurve == 6) return saturate(ApplyHighlightDesat(ApplyPreserveSdr(c)));
    if (uCurve == 7) return saturate(ApplyHighlightDesat(ApplyBt2390(c)));
    return saturate(c);
}

float3 ReadLinear(float2 uv) {
    float3 c = max(0, gSource.SampleLevel(gSamp, uv, 0).rgb);
    if (uIsHdr == 0) c = SrgbToLinear(c);
    return c;
}

float3 ReadLinearLod(float2 uv, float lod) {
    float3 c = max(0, gSource.SampleLevel(gSamp, uv, lod).rgb);
    if (uIsHdr == 0) c = SrgbToLinear(c);
    return c;
}

float4 PSMain(VSOut i) : SV_Target {
    float3 c = ReadLinear(i.uv);

    if (uPassthrough != 0) {
        // Linear-output mode: write raw scRGB values, unclipped (so an HDR
        // backbuffer can show >1.0 highlights). Otherwise clamp + sRGB encode
        // for an 8-bit SDR render target.
        if (uLinearOutput != 0) {
            return float4(c, 1.0);
        }
        return float4(ApplyOutputGamma(saturate(c)), 1.0);
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
        c = max(0.0, c + (c - n * 0.25) * (uSharpen * 2.0));
    }

    // White balance: rough temperature/tint shift.
    c.r *= 1.0 + uTemperature * 0.30 + uTint * 0.15;
    c.g *= 1.0                        - uTint * 0.30;
    c.b *= 1.0 - uTemperature * 0.30 + uTint * 0.15;

    // Per-channel gain.
    c *= float3(uRGain, uGGain, uBGain);

    // Exposure.
    c *= uExposure;

    // Highlight rolloff (pre-curve knee at 0.7).
    if (uHighlightRolloff > 0) {
        float knee = 0.7;
        float3 over = max(c - knee, 0.0);
        c = c - over * uHighlightRolloff;
    }

    // Local tonemap: compute the curve's compression ratio on a blurred
    // sample of the source and apply that ratio to the (sharpened) full-res
    // pixel. Result: dark UI in front of bright HDR content keeps its
    // dynamic range, because the curve only compresses brightness on a
    // per-region basis instead of globally.
    if (uLocalStrength > 0) {
        float3 cb = ReadLinearLod(i.uv, uLocalLod);
        cb.r *= 1.0 + uTemperature * 0.30 + uTint * 0.15;
        cb.g *= 1.0                        - uTint * 0.30;
        cb.b *= 1.0 - uTemperature * 0.30 + uTint * 0.15;
        cb *= float3(uRGain, uGGain, uBGain);
        cb *= uExposure;
        if (uHighlightRolloff > 0) {
            float3 over = max(cb - 0.7, 0.0);
            cb = cb - over * uHighlightRolloff;
        }
        float3 cb_tm = ApplyCurve(cb);
        float3 ratio = cb_tm / max(cb, 1e-5);
        float3 c_local = saturate(c * ratio);
        float3 c_global = ApplyCurve(c);
        c = lerp(c_global, c_local, uLocalStrength);
    } else {
        c = ApplyCurve(c);
    }

    if (uBlackPointLift > 0) {
        c = c + uBlackPointLift * (1.0 - c);
    }

    if (uSaturation != 1.0) {
        float gray = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = saturate(lerp(float3(gray, gray, gray), c, uSaturation));
    }

    // Gamut compress: pull oob colors toward gray.
    if (uGamutCompress > 0) {
        float maxc = max(c.r, max(c.g, c.b));
        float minc = min(c.r, min(c.g, c.b));
        if (maxc > 1.0 || minc < 0.0) {
            float gray = dot(c, float3(0.2126, 0.7152, 0.0722));
            c = lerp(c, float3(gray, gray, gray), uGamutCompress);
        }
    }

    return float4(ApplyOutputGamma(c), 1.0);
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
    float _pad0[3];
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
    if (!vsBlob || !psBlob) {
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
    sourceIsHdr_ = frame.isHdr;

    // Allocate a full mip chain so the local-tonemap pass can sample a
    // coarse LOD as its "local-average blur". GenerateMips requires the
    // texture to be a render target.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = frame.width;
    td.Height = frame.height;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = frame.isHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                            : DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) {
        return false;
    }
    context_->UpdateSubresource(tex.Get(), 0, nullptr, frame.pixels.data(),
                                frame.width * frame.bytesPerPixel, 0);
    if (FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr,
                                                 &sourceSrv_))) {
        return false;
    }
    context_->GenerateMips(sourceSrv_.Get());

    // Track actual mip count for the local-tonemap LOD selection.
    D3D11_TEXTURE2D_DESC actual{};
    tex->GetDesc(&actual);
    sourceMipCount_ = actual.MipLevels;
    return true;
}

bool ShaderTonemap::SetSourceTexture(ID3D11Texture2D* tex, bool isHdr) {
    sourceSrv_.Reset();
    sourceIsHdr_ = isHdr;
    // No mip chain on this path: the recorder feeds full-res GPU textures and
    // local tonemap (which samples a coarse LOD) isn't used during recording.
    sourceMipCount_ = 1;
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
        cb.localLod = float(sourceMipCount_ > 1 ? sourceMipCount_ - 1 : 0);
        cb.kneePoint = params.kneePoint;
        cb.highlightDesat = params.highlightDesat;
        cb.sourcePeakNits = params.sourcePeakNits;
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
    ID3D11ShaderResourceView* srvs[] = {sourceSrv_.Get()};
    context_->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samps[] = {sampler_.Get()};
    context_->PSSetSamplers(0, 1, samps);

    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[] = {nullptr};
    context_->PSSetShaderResources(0, 1, nullSrv);
}

}  // namespace sundial
