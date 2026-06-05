#include "HdrCapture.h"

#include "Settings.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wingdi.h>
#include <wrl/client.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sundial {
namespace {

void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::system_error(hr, std::system_category(), what);
    }
}

// Query Windows for the SDR white level of the monitor matching `gdiName`
// (a GDI device name like "\\.\DISPLAY1"). Returns 0 on failure so the
// caller can fall back to the 80-nit scRGB default.
//
// Windows lets the user set this via Settings > Display > HDR > SDR content
// brightness. The reference white level is reported as a multiplier where
// 1000 == 80 nits, so nits = value * 80 / 1000.
float QuerySdrWhiteLevelNits(const wchar_t* gdiName) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                                    &modeCount) != ERROR_SUCCESS) {
        return 0.0f;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                           &modeCount, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return 0.0f;
    }

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) {
            continue;
        }
        if (wcscmp(src.viewGdiDeviceName, gdiName) != 0) {
            continue;
        }

        // DISPLAYCONFIG_SDR_WHITE_LEVEL appeared in the Windows 10 1709 SDK
        // but isn't always reachable through the public Windows.h on older
        // toolchains. Define the struct/constant locally so we don't depend
        // on SDK version.
        struct LocalSdrWhiteLevel {
            DISPLAYCONFIG_DEVICE_INFO_HEADER header;
            ULONG SDRWhiteLevel;
        };
        constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE kGetSdrWhite =
            static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(11);
        LocalSdrWhiteLevel wl{};
        wl.header.type = kGetSdrWhite;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) != ERROR_SUCCESS) {
            return 0.0f;
        }
        return wl.SDRWhiteLevel * 80.0f / 1000.0f;
    }
    return 0.0f;
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
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        ThrowIfFailed(hr, "EnumAdapters1");

        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(o, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            ThrowIfFailed(hr, "EnumOutputs");

            DXGI_OUTPUT_DESC desc{};
            ThrowIfFailed(output->GetDesc(&desc), "Output GetDesc");
            if (!desc.AttachedToDesktop) {
                continue;
            }

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

// Sparse check for an entirely-black frame. DDA occasionally hands back an
// uncomposited (all-zero) surface on the first AcquireNextFrame after a fresh
// DuplicateOutput, which is the source of the intermittent black captures; we
// use this as a backstop to reject such frames. Samples a grid of up to ~4096
// pixels so it stays cheap even at 4K. Returns false as soon as any RGB
// channel is non-zero (alpha is ignored).
bool FrameLooksBlack(const Frame& f) {
    const size_t pixelCount = size_t(f.width) * f.height;
    if (pixelCount == 0 || f.pixels.empty()) {
        return true;
    }
    const size_t bpp = f.bytesPerPixel;
    const size_t step = std::max<size_t>(1, pixelCount / 4096);
    const uint8_t* p = f.pixels.data();
    for (size_t i = 0; i < pixelCount; i += step) {
        const uint8_t* px = p + i * bpp;
        if (f.isHdr) {
            // FP16 RGBA: R/G/B occupy the first 6 bytes (3 halfs).
            if (px[0] || px[1] || px[2] || px[3] || px[4] || px[5]) {
                return false;
            }
        } else {
            // BGRA8: B/G/R occupy bytes 0/1/2.
            if (px[0] || px[1] || px[2]) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

Frame CaptureFullScreen() {
    auto output6 = GetPrimaryOutput6();

    DXGI_OUTPUT_DESC1 outDesc{};
    ThrowIfFailed(output6->GetDesc1(&outDesc), "IDXGIOutput6::GetDesc1");

    // outDesc.DeviceName is the GDI \\.\DISPLAYn string Windows uses for the
    // monitor; we feed it into the Display Config API to look up the user-
    // configured SDR-content brightness for this output.
    const float sdrWhiteFromOs = QuerySdrWhiteLevelNits(outDesc.DeviceName);

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

    // We create a fresh duplication for every capture, so we always race the
    // very first AcquireNextFrame after DuplicateOutput: DDA can hand back an
    // uncomposited (all-black) surface before the desktop has been drawn into
    // the duplication, which is what produced the intermittent black captures.
    // Worse, our callers hide the overlay right before capturing, so a desktop
    // recomposition present is usually imminent at this point.
    //
    // To get the correct frame reliably:
    //  - Record a QPC baseline before acquiring. A frame whose LastPresentTime
    //    is *after* the baseline is guaranteed to be freshly composited (the
    //    bare "LastPresentTime != 0" check is unreliable because that timestamp
    //    can be stale from a present that happened before we started).
    //  - Copy every candidate into our own staging texture immediately, before
    //    ReleaseFrame, since DDA invalidates the source surface on release.
    //  - Accept a fresh, non-black frame as soon as we see one; after a short
    //    grace period accept any non-black frame; for a perfectly static
    //    desktop (acquire keeps timing out) keep whatever we last copied.
    LARGE_INTEGER qpcBaseline{};
    QueryPerformanceCounter(&qpcBaseline);

    ComPtr<ID3D11Texture2D> staging;
    Frame frame;
    bool haveFrame = false;
    constexpr int kMaxAttempts = 30;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        ComPtr<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        hr = dup->AcquireNextFrame(100, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (haveFrame) {
                break;  // static desktop: nothing new will present
            }
            continue;
        }
        ThrowIfFailed(hr, "IDXGIOutputDuplication::AcquireNextFrame");

        ComPtr<ID3D11Texture2D> sourceTex;
        ThrowIfFailed(resource.As(&sourceTex),
                      "QueryInterface ID3D11Texture2D");

        D3D11_TEXTURE2D_DESC desc{};
        sourceTex->GetDesc(&desc);

        if (!staging) {
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.MiscFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ThrowIfFailed(
                device->CreateTexture2D(&stagingDesc, nullptr, &staging),
                "CreateTexture2D(staging)");
        }

        // Copy off the source and release the DDA frame promptly; everything
        // below reads from our own staging copy, which survives the release.
        context->CopyResource(staging.Get(), sourceTex.Get());
        context->Flush();
        const bool freshPresent =
            frameInfo.LastPresentTime.QuadPart > qpcBaseline.QuadPart;
        dup->ReleaseFrame();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(
            context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "ID3D11DeviceContext::Map(staging)");

        frame.width = desc.Width;
        frame.height = desc.Height;
        frame.isHdr = (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        frame.bytesPerPixel = frame.isHdr ? 8u : 4u;
        frame.maxLuminanceNits = outDesc.MaxLuminance;
        frame.minLuminanceNits = outDesc.MinLuminance;
        // Fall back to scRGB convention (1.0 = 80 nits) if the OS doesn't
        // expose a value, but in practice every HDR-enabled monitor on Win10
        // 1709+ does.
        frame.sdrWhiteLevelNits = sdrWhiteFromOs > 0.0f ? sdrWhiteFromOs : 80.0f;

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
        haveFrame = true;

        const bool black = FrameLooksBlack(frame);
        // Best case: a freshly-composited, non-black frame. After a short grace
        // period accept any non-black frame; as a hard backstop accept whatever
        // we have so we never spin forever on a legitimately black screen.
        if (!black && (freshPresent || attempt >= 3)) {
            break;
        }
        if (attempt >= 12) {
            break;
        }
    }
    if (!haveFrame) {
        throw std::runtime_error("Timed out waiting for desktop frame");
    }

    return frame;
}

// Seed tonemap values from the captured frame so each capture starts from a
// configuration matched to Windows Game Bar's HDR-to-SDR conversion. The user
// can still override every slider in the editor; on no-edit captures these
// auto-seeded values are what get applied.
//
// For HDR frames (mirrors Game Bar):
//  - sdrWhiteNits = OS-reported SDR white level (Settings > Display > HDR >
//    "SDR content brightness"), exactly what Game Bar uses as its SDR anchor.
//  - sourcePeakNits = display MaxLuminance from DXGI (EDID-reported peak),
//    clamped to a sane BT.2390 range. Defines the upper end of the curve.
//  - highlightRolloff and gamutCompress stay at 0: BT.2390 already does a
//    smooth roll-off and the luminance-only scaling keeps colors in gamut.
//
// For SDR frames we force the HDR-only knobs back to "off" and pull
// sourcePeakNits down to 80 so BT.2390 behaves as identity on SDR data
// (target peak == source peak => knee start at 1.0 => passthrough).
void SeedTonemapForFrame(TonemapParams& tm, const Frame& frame) {
    if (frame.isHdr) {
        const float os =
            frame.sdrWhiteLevelNits > 0.0f ? frame.sdrWhiteLevelNits : 200.0f;
        tm.sdrWhiteNits = std::clamp(os, 40.0f, 400.0f);
        const float peak =
            frame.maxLuminanceNits > 0.0f ? frame.maxLuminanceNits : 1000.0f;
        tm.sourcePeakNits = std::clamp(peak, 400.0f, 4000.0f);
        tm.highlightRolloff = 0.0f;
        tm.gamutCompress = 0.0f;
    } else {
        tm.sdrWhiteNits = 80.0f;       // scRGB 1.0 = 80 nits, no rescale
        tm.exposureEV = 0.0f;
        tm.sourcePeakNits = 80.0f;     // BT.2390 becomes identity
        tm.highlightRolloff = 0.0f;
        tm.gamutCompress = 0.0f;
    }
}

}  // namespace sundial
