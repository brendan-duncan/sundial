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
    int    uCurve;          // 0=Linear 1=Reinhard 2=ACES 3=Hable 4=AgX 5=Neutral 6=PreserveSdr
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
    float  _pad0[2];
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

// PreserveSdr: identity below kKnee, hue-preserving soft knee that asymptotes
// to 1.0 above. Sacrifices a sliver of SDR brightness above kKnee so HDR
// highlights compress smoothly into [0,1] instead of clipping.
float3 ApplyPreserveSdr(float3 c) {
    float peak = max(c.r, max(c.g, c.b));
    if (peak <= 0.0) return c;
    const float kKnee = 0.9;
    if (peak <= kKnee) return saturate(c);
    float over = peak - kKnee;
    float range = 1.0 - kKnee;
    float newPeak = kKnee + over * range / (over + range);
    return saturate(c * (newPeak / peak));
}

float3 ApplyCurve(float3 c) {
    if (uCurve == 1) return ApplyReinhard(c);
    if (uCurve == 2) return ApplyAces(c);
    if (uCurve == 3) return ApplyHable(c);
    if (uCurve == 4) return ApplyAgX(c);
    if (uCurve == 5) return ApplyNeutral(c);
    if (uCurve == 6) return ApplyPreserveSdr(c);
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
    float _pad0[2];
};

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
    if (!vsBlob || !psBlob) return false;

    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                           vsBlob->GetBufferSize(), nullptr,
                                           &vs_)))
        return false;
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(),
                                          psBlob->GetBufferSize(), nullptr,
                                          &ps_)))
        return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CBLayout);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, &rasterizer_)))
        return false;

    D3D11_BLEND_DESC bsd{};
    bsd.RenderTarget[0].BlendEnable = FALSE;
    bsd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bsd, &blendOpaque_))) return false;

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
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) return false;
    context_->UpdateSubresource(tex.Get(), 0, nullptr, frame.pixels.data(),
                                frame.width * frame.bytesPerPixel, 0);
    if (FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr,
                                                 &sourceSrv_)))
        return false;
    context_->GenerateMips(sourceSrv_.Get());

    // Track actual mip count for the local-tonemap LOD selection.
    D3D11_TEXTURE2D_DESC actual{};
    tex->GetDesc(&actual);
    sourceMipCount_ = actual.MipLevels;
    return true;
}

bool ShaderTonemap::ResizeTarget(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (width == outputW_ && height == outputH_ && outputSdrSrv_) return true;

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
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) return false;
        if (FAILED(device_->CreateRenderTargetView(tex.Get(), nullptr, &rtv)))
            return false;
        if (FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
            return false;
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
              DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    if (!make(outputHdrTex_, outputHdrRtv_, outputHdrSrv_, hdrFormat))
        return false;
    outputW_ = width;
    outputH_ = height;
    return true;
}

void ShaderTonemap::RenderSdr(const TonemapParams& params) {
    if (!sourceSrv_ || !outputSdrRtv_) return;

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
        std::memcpy(m.pData, &cb, sizeof(cb));
        context_->Unmap(cb_.Get(), 0);
    }
    SetPipelineAndDraw(outputSdrRtv_.Get());
}

void ShaderTonemap::RenderHdrPassthrough() {
    if (!sourceSrv_ || !outputHdrRtv_) return;

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
