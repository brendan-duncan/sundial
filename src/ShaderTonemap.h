#pragma once
#include "HdrCapture.h"
#include "Settings.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace sundial {

// Encapsulates the D3D11 pipeline used by the editor's live preview: takes
// an FP16 scRGB or BGRA8 sRGB source texture and renders the tonemap into a
// render-target texture whose SRV can be handed straight to ImGui::Image.
class ShaderTonemap {
public:
    // When linearHdrOutput is true, the HDR-passthrough render target uses
    // R16G16B16A16_FLOAT and the shader writes raw linear scRGB values
    // (unclipped, no sRGB encode). The SDR-tonemapped target keeps its 8-bit
    // sRGB-encoded format in either case - only the HDR preview needs to
    // carry values above 1.0 for an HDR backbuffer to display authentically.
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    bool linearHdrOutput = false);

    // True if the HDR-passthrough output is FP16 linear (vs 8-bit sRGB).
    bool LinearHdrOutput() const { return linearHdrOutput_; }

    // Upload a Frame into a GPU texture suitable as input for Render(). Call
    // once per source change (capture, crop, resize).
    bool SetSource(const Frame& frame);

    // (Re)create the render target texture at the given dimensions. The SRV
    // returned by GetOutputSrv() is invalidated until this is called.
    bool ResizeTarget(uint32_t width, uint32_t height);

    // Apply the current params to the source and render the SDR result into
    // the SDR output. SRV available via GetSdrSrv().
    void RenderSdr(const TonemapParams& params);

    // Render an HDR "passthrough" view: linear scRGB -> sRGB-gamma, no curve
    // or saturation applied. This is what the user would see with no
    // tonemap; useful for SDR/HDR comparison. SRV via GetHdrSrv().
    void RenderHdrPassthrough();

    ID3D11ShaderResourceView* GetSdrSrv() const { return outputSdrSrv_.Get(); }
    ID3D11ShaderResourceView* GetHdrSrv() const { return outputHdrSrv_.Get(); }
    uint32_t OutputWidth() const { return outputW_; }
    uint32_t OutputHeight() const { return outputH_; }

private:
    void SetPipelineAndDraw(ID3D11RenderTargetView* rtv);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceSrv_;
    bool sourceIsHdr_ = false;
    uint32_t sourceMipCount_ = 1;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputSdrTex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputSdrRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputSdrSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputHdrTex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputHdrRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputHdrSrv_;
    uint32_t outputW_ = 0;
    uint32_t outputH_ = 0;
    bool linearHdrOutput_ = false;
};

}  // namespace sundial
