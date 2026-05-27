#include "HdrCapture.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace sundial {
namespace {

void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::system_error(hr, std::system_category(), what);
    }
}

// Walk every adapter's outputs to find the one Windows considers the primary
// monitor. EnumAdapters(0)/EnumOutputs(0) is not reliable: on hybrid-GPU
// systems and multi-monitor setups, index 0 isn't necessarily the desktop's
// primary output, and using the wrong (adapter, output) pair with DDA can
// silently yield empty frames.
ComPtr<IDXGIOutput6> GetPrimaryOutput6() {
    ComPtr<IDXGIFactory6> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)),
                  "CreateDXGIFactory2");

    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(a, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        ThrowIfFailed(hr, "EnumAdapters1");

        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(o, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            ThrowIfFailed(hr, "EnumOutputs");

            DXGI_OUTPUT_DESC desc{};
            ThrowIfFailed(output->GetDesc(&desc), "Output GetDesc");
            if (!desc.AttachedToDesktop) continue;

            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(desc.Monitor, &mi) &&
                (mi.dwFlags & MONITORINFOF_PRIMARY)) {
                ComPtr<IDXGIOutput6> output6;
                ThrowIfFailed(output.As(&output6),
                              "QueryInterface IDXGIOutput6");
                return output6;
            }
        }
    }
    throw std::runtime_error("No primary desktop output found");
}

}  // namespace

Frame CaptureFullScreen() {
    auto output6 = GetPrimaryOutput6();

    DXGI_OUTPUT_DESC1 outDesc{};
    ThrowIfFailed(output6->GetDesc1(&outDesc), "IDXGIOutput6::GetDesc1");

    ComPtr<IDXGIAdapter> adapter;
    ThrowIfFailed(output6->GetParent(IID_PPV_ARGS(&adapter)),
                  "IDXGIOutput6::GetParent");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    ThrowIfFailed(
        D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                          0, nullptr, 0, D3D11_SDK_VERSION,
                          &device, &featureLevel, &context),
        "D3D11CreateDevice");

    ComPtr<IDXGIOutput5> output5;
    ThrowIfFailed(output6.As(&output5), "QueryInterface IDXGIOutput5");

    // Order matters: DDA picks the first format the source supports. If the
    // display is in HDR mode, the desktop composition surface is FP16 scRGB
    // and we get DXGI_FORMAT_R16G16B16A16_FLOAT. Otherwise we get BGRA8.
    DXGI_FORMAT formats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_B8G8R8A8_UNORM,
    };

    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = output5->DuplicateOutput1(device.Get(), 0,
                                           ARRAYSIZE(formats), formats, &dup);
    if (FAILED(hr)) {
        // Some drivers reject DuplicateOutput1 even on Win10+; fall back to
        // the legacy entry point, which gives us SDR (BGRA8) only.
        ComPtr<IDXGIOutput1> output1;
        ThrowIfFailed(output6.As(&output1), "QueryInterface IDXGIOutput1");
        ThrowIfFailed(output1->DuplicateOutput(device.Get(), &dup),
                      "IDXGIOutput1::DuplicateOutput");
    }

    // DDA can hand back the first acquired resource before any actual present
    // has populated it, which is what produced the all-black captures. Loop
    // releasing and re-acquiring until LastPresentTime is non-zero (i.e. a
    // real present landed in the duplication). Fall back to whatever frame
    // we have after a short grace period so a perfectly static desktop still
    // captures.
    ComPtr<IDXGIResource> resource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    constexpr int kMaxAttempts = 30;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (resource) {
            dup->ReleaseFrame();
            resource.Reset();
        }
        hr = dup->AcquireNextFrame(100, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        ThrowIfFailed(hr, "IDXGIOutputDuplication::AcquireNextFrame");

        if (frameInfo.LastPresentTime.QuadPart != 0) break;
        if (attempt >= 4) break;  // accept stale frame if no presents are happening
    }
    if (!resource) {
        throw std::runtime_error("Timed out waiting for desktop frame");
    }

    ComPtr<ID3D11Texture2D> sourceTex;
    ThrowIfFailed(resource.As(&sourceTex), "QueryInterface ID3D11Texture2D");

    D3D11_TEXTURE2D_DESC desc{};
    sourceTex->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    ThrowIfFailed(device->CreateTexture2D(&stagingDesc, nullptr, &staging),
                  "CreateTexture2D(staging)");

    context->CopyResource(staging.Get(), sourceTex.Get());
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                  "ID3D11DeviceContext::Map(staging)");

    Frame frame;
    frame.width = desc.Width;
    frame.height = desc.Height;
    frame.isHdr = (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    frame.bytesPerPixel = frame.isHdr ? 8u : 4u;
    frame.maxLuminanceNits = outDesc.MaxLuminance;
    frame.minLuminanceNits = outDesc.MinLuminance;

    const size_t rowBytes = size_t(desc.Width) * frame.bytesPerPixel;
    frame.pixels.resize(rowBytes * desc.Height);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = frame.pixels.data();
    for (uint32_t y = 0; y < desc.Height; ++y) {
        memcpy(dst + size_t(y) * rowBytes,
               src + size_t(y) * mapped.RowPitch,
               rowBytes);
    }
    context->Unmap(staging.Get(), 0);
    dup->ReleaseFrame();

    return frame;
}

}  // namespace sundial
