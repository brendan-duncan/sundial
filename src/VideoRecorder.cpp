#include "VideoRecorder.h"

#include "ShaderTonemap.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sundial {
namespace {

// Recording cadence. DDA only delivers frames when the desktop changes, so the
// loop re-emits the last rendered frame to hold a steady output rate.
constexpr uint32_t kFps = 30;
constexpr int64_t kHnsPerSec = 10'000'000;  // 100-ns units per second

struct RecorderError {
    std::string what;
};
void Thr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw RecorderError{what};
    }
}

// Compact primary-output finder, mirroring HdrCapture's GetPrimaryOutput6().
// The recorder needs its own device (used solely on the worker thread), so it
// can't share HdrCapture's; the duplication setup is repeated here rather than
// exported, to keep the recording path self-contained.
ComPtr<IDXGIOutput6> GetPrimaryOutput6(ComPtr<IDXGIAdapter1>& outAdapter) {
    ComPtr<IDXGIFactory6> factory;
    Thr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(a, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        Thr(hr, "EnumAdapters1");

        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(o, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            Thr(hr, "EnumOutputs");

            DXGI_OUTPUT_DESC desc{};
            Thr(output->GetDesc(&desc), "Output GetDesc");
            if (!desc.AttachedToDesktop) {
                continue;
            }

            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(desc.Monitor, &mi) &&
                (mi.dwFlags & MONITORINFOF_PRIMARY)) {
                ComPtr<IDXGIOutput6> output6;
                Thr(output.As(&output6), "QueryInterface IDXGIOutput6");
                outAdapter = adapter;
                return output6;
            }
        }
    }
    throw RecorderError{"No primary desktop output found"};
}

float QuerySdrWhiteLevelNits(const wchar_t* gdiName) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                                    &modeCount) != ERROR_SUCCESS) {
        return 0.0f;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                           &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
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

// Mirror of main.cpp's SeedTonemapForFrame so a recording's HDR->SDR look
// matches a screenshot taken on the same display.
void SeedTonemapForDisplay(TonemapParams& tm, bool isHdr, float displayPeakNits,
                           float sdrWhiteNits) {
    if (isHdr) {
        const float os = sdrWhiteNits > 0.0f ? sdrWhiteNits : 200.0f;
        tm.sdrWhiteNits = std::clamp(os, 40.0f, 400.0f);
        const float peak = displayPeakNits > 0.0f ? displayPeakNits : 1000.0f;
        tm.sourcePeakNits = std::clamp(peak, 400.0f, 4000.0f);
        tm.highlightRolloff = 0.0f;
        tm.gamutCompress = 0.0f;
    } else {
        tm.sdrWhiteNits = 80.0f;
        tm.exposureEV = 0.0f;
        tm.sourcePeakNits = 80.0f;
        tm.highlightRolloff = 0.0f;
        tm.gamutCompress = 0.0f;
    }
    // Local tonemap relies on a mip chain SetSourceTexture() doesn't build.
    tm.localStrength = 0.0f;
}

int64_t QpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
int64_t QpcFreq() {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
}

// Owns the Media Foundation sink writer for one H.264 / MP4 clip.
class Mp4Writer {
public:
    void Open(const std::wstring& path, uint32_t w, uint32_t h, uint32_t fps) {
        const double bpp = 0.10;  // ~6 Mbps at 1080p30
        const uint32_t bitrate = static_cast<uint32_t>(
            std::clamp(double(w) * h * fps * bpp, 2'000'000.0, 50'000'000.0));

        ComPtr<IMFMediaType> outType;
        Thr(MFCreateMediaType(&outType), "MFCreateMediaType(out)");
        Thr(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "out major");
        Thr(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "out subtype");
        Thr(outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate), "out bitrate");
        Thr(outType->SetUINT32(MF_MT_INTERLACE_MODE,
                               MFVideoInterlace_Progressive),
            "out interlace");
        Thr(outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High),
            "out profile");
        Thr(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, w, h),
            "out size");
        Thr(MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fps, 1),
            "out rate");
        Thr(MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
            "out par");

        ComPtr<IMFMediaType> inType;
        Thr(MFCreateMediaType(&inType), "MFCreateMediaType(in)");
        Thr(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "in major");
        Thr(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "in subtype");
        Thr(inType->SetUINT32(MF_MT_INTERLACE_MODE,
                              MFVideoInterlace_Progressive),
            "in interlace");
        // Positive stride => top-down rows, matching our tonemap readback.
        Thr(inType->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4), "in stride");
        Thr(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, w, h),
            "in size");
        Thr(MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps, 1),
            "in rate");
        Thr(MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
            "in par");

        ComPtr<IMFAttributes> attrs;
        Thr(MFCreateAttributes(&attrs, 2), "MFCreateAttributes");
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

        Thr(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(),
                                      &writer_),
            "MFCreateSinkWriterFromURL");
        Thr(writer_->AddStream(outType.Get(), &stream_), "AddStream");
        Thr(writer_->SetInputMediaType(stream_, inType.Get(), nullptr),
            "SetInputMediaType");
        Thr(writer_->BeginWriting(), "BeginWriting");

        width_ = w;
        height_ = h;
        frameDur_ = kHnsPerSec / fps;
    }

    // Encode one BGRA top-down frame at the given presentation time.
    void WriteFrame(const std::vector<uint8_t>& bgra, int64_t ptsHns) {
        const DWORD size = width_ * height_ * 4;
        ComPtr<IMFMediaBuffer> buf;
        Thr(MFCreateMemoryBuffer(size, &buf), "MFCreateMemoryBuffer");
        BYTE* dst = nullptr;
        Thr(buf->Lock(&dst, nullptr, nullptr), "Buffer Lock");
        memcpy(dst, bgra.data(), size);
        Thr(buf->Unlock(), "Buffer Unlock");
        Thr(buf->SetCurrentLength(size), "SetCurrentLength");

        ComPtr<IMFSample> sample;
        Thr(MFCreateSample(&sample), "MFCreateSample");
        Thr(sample->AddBuffer(buf.Get()), "AddBuffer");
        Thr(sample->SetSampleTime(ptsHns), "SetSampleTime");
        Thr(sample->SetSampleDuration(frameDur_), "SetSampleDuration");
        Thr(writer_->WriteSample(stream_, sample.Get()), "WriteSample");
    }

    void Finalize() {
        if (writer_) {
            writer_->Finalize();
            writer_.Reset();
        }
    }

private:
    ComPtr<IMFSinkWriter> writer_;
    DWORD stream_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int64_t frameDur_ = 0;
};

// RAII for MFStartup/MFShutdown on the worker thread.
struct MfRuntime {
    bool ok = false;
    MfRuntime() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfRuntime() {
        if (ok) {
            MFShutdown();
        }
    }
};

}  // namespace

VideoRecorder::~VideoRecorder() {
    Stop();
}

bool VideoRecorder::Start(const RECT& region, const std::wstring& outputPath,
                          const AppSettings& settings, bool tonemapPreseeded) {
    if (running_.load()) {
        return false;
    }
    error_.clear();
    stopRequested_.store(false);
    running_.store(true);
    worker_ = std::thread(&VideoRecorder::Run, this, region, outputPath,
                          settings, tonemapPreseeded);
    return true;
}

bool VideoRecorder::Stop() {
    stopRequested_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
    return error_.empty();
}

void VideoRecorder::Run(RECT region, std::wstring outputPath,
                        AppSettings settings, bool tonemapPreseeded) {
    // The worker owns its own COM apartment + MF runtime + D3D device, so
    // nothing here touches the main thread's immediate context.
    const bool comInit =
        SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    MfRuntime mf;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> dup;
    Mp4Writer mp4;
    bool finalized = false;

    try {
        if (!mf.ok) {
            throw RecorderError{"MFStartup failed"};
        }

        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput6> output6 = GetPrimaryOutput6(adapter);

        DXGI_OUTPUT_DESC1 outDesc{};
        Thr(output6->GetDesc1(&outDesc), "GetDesc1");
        const float sdrWhite = QuerySdrWhiteLevelNits(outDesc.DeviceName);

        D3D_FEATURE_LEVEL fl{};
        Thr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                              0, nullptr, 0, D3D11_SDK_VERSION, &device, &fl,
                              &context),
            "D3D11CreateDevice");

        ComPtr<IDXGIOutput5> output5;
        Thr(output6.As(&output5), "QueryInterface IDXGIOutput5");
        DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 DXGI_FORMAT_B8G8R8A8_UNORM};
        HRESULT hr = output5->DuplicateOutput1(device.Get(), 0,
                                               ARRAYSIZE(formats), formats,
                                               &dup);
        if (FAILED(hr)) {
            ComPtr<IDXGIOutput1> output1;
            Thr(output6.As(&output1), "QueryInterface IDXGIOutput1");
            Thr(output1->DuplicateOutput(device.Get(), &dup),
                "DuplicateOutput");
        }

        // Acquire one real frame to learn source dimensions + HDR-ness before
        // sizing the crop, tonemap target, and encoder.
        ComPtr<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        for (int attempt = 0; attempt < 30; ++attempt) {
            if (resource) {
                dup->ReleaseFrame();
                resource.Reset();
            }
            hr = dup->AcquireNextFrame(100, &fi, &resource);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                continue;
            }
            Thr(hr, "AcquireNextFrame(initial)");
            if (fi.LastPresentTime.QuadPart != 0) {
                break;
            }
            if (attempt >= 4) {
                break;
            }
        }
        if (!resource) {
            throw RecorderError{"Timed out waiting for first frame"};
        }

        ComPtr<ID3D11Texture2D> srcTex;
        Thr(resource.As(&srcTex), "QueryInterface ID3D11Texture2D");
        D3D11_TEXTURE2D_DESC srcDesc{};
        srcTex->GetDesc(&srcDesc);
        const bool isHdr = (srcDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);

        // Resolve the crop region against the captured surface. An empty rect
        // records the whole monitor. H.264 needs even dimensions.
        LONG rx = std::max<LONG>(0, region.left);
        LONG ry = std::max<LONG>(0, region.top);
        LONG rr = region.right > region.left ? region.right
                                             : LONG(srcDesc.Width);
        LONG rb = region.bottom > region.top ? region.bottom
                                             : LONG(srcDesc.Height);
        rr = std::min<LONG>(rr, LONG(srcDesc.Width));
        rb = std::min<LONG>(rb, LONG(srcDesc.Height));
        uint32_t rw = uint32_t(std::max<LONG>(2, rr - rx)) & ~1u;
        uint32_t rh = uint32_t(std::max<LONG>(2, rb - ry)) & ~1u;
        if (rx + LONG(rw) > LONG(srcDesc.Width)) {
            rw = (srcDesc.Width - rx) & ~1u;
        }
        if (ry + LONG(rh) > LONG(srcDesc.Height)) {
            rh = (srcDesc.Height - ry) & ~1u;
        }

        TonemapParams tm = settings.tonemap;
        if (tonemapPreseeded) {
            // The look was already dialed in (and display-seeded) via the
            // "Adjust look" editor; honor it as-is. Local tonemap still can't
            // run here (no mip chain), so force it off regardless.
            tm.localStrength = 0.0f;
        } else {
            SeedTonemapForDisplay(tm, isHdr, outDesc.MaxLuminance, sdrWhite);
        }

        // GPU crop target (matches source format so CopySubresourceRegion is a
        // straight copy) and a staging texture for SDR readback.
        D3D11_TEXTURE2D_DESC cropDesc{};
        cropDesc.Width = rw;
        cropDesc.Height = rh;
        cropDesc.MipLevels = 1;
        cropDesc.ArraySize = 1;
        cropDesc.Format = srcDesc.Format;
        cropDesc.SampleDesc.Count = 1;
        cropDesc.Usage = D3D11_USAGE_DEFAULT;
        cropDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> cropTex;
        Thr(device->CreateTexture2D(&cropDesc, nullptr, &cropTex),
            "CreateTexture2D(crop)");

        D3D11_TEXTURE2D_DESC stageDesc{};
        stageDesc.Width = rw;
        stageDesc.Height = rh;
        stageDesc.MipLevels = 1;
        stageDesc.ArraySize = 1;
        stageDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // ShaderTonemap SDR out
        stageDesc.SampleDesc.Count = 1;
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stageTex;
        Thr(device->CreateTexture2D(&stageDesc, nullptr, &stageTex),
            "CreateTexture2D(staging)");

        ShaderTonemap tonemap;
        if (!tonemap.Initialize(device.Get(), context.Get(),
                                /*linearHdrOutput=*/false)) {
            throw RecorderError{"Tonemap shader failed to compile"};
        }
        if (!tonemap.ResizeTarget(rw, rh)) {
            throw RecorderError{"Tonemap target alloc failed"};
        }

        mp4.Open(outputPath, rw, rh, kFps);

        const D3D11_BOX box{UINT(rx),      UINT(ry), 0,
                            UINT(rx + rw), UINT(ry + rh), 1};

        // Reusable BGRA frame buffer (top-down). Re-emitted when the desktop
        // is idle so output cadence stays at kFps.
        std::vector<uint8_t> bgra(size_t(rw) * rh * 4, 0);

        // Render the just-acquired first frame into `bgra`, then run the loop.
        auto renderCurrent = [&](ID3D11Texture2D* src) {
            context->CopySubresourceRegion(cropTex.Get(), 0, 0, 0, 0, src, 0,
                                           &box);
            tonemap.SetSourceTexture(cropTex.Get(), isHdr);
            tonemap.RenderSdr(tm);
            context->CopyResource(stageTex.Get(), tonemap.SdrTexture());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(context->Map(stageTex.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                return;
            }
            const uint8_t* s = static_cast<const uint8_t*>(m.pData);
            for (uint32_t y = 0; y < rh; ++y) {
                const uint8_t* sr = s + size_t(y) * m.RowPitch;
                uint8_t* dr = bgra.data() + size_t(y) * rw * 4;
                for (uint32_t x = 0; x < rw; ++x) {
                    dr[x * 4 + 0] = sr[x * 4 + 2];  // B <- R
                    dr[x * 4 + 1] = sr[x * 4 + 1];  // G
                    dr[x * 4 + 2] = sr[x * 4 + 0];  // R <- B
                    dr[x * 4 + 3] = 255;
                }
            }
            context->Unmap(stageTex.Get(), 0);
        };

        renderCurrent(srcTex.Get());
        dup->ReleaseFrame();
        resource.Reset();

        const int64_t freq = QpcFreq();
        const int64_t start = QpcNow();
        int64_t framesWritten = 0;

        while (!stopRequested_.load()) {
            // Pull the newest desktop frame if one is ready (short timeout so
            // we stay responsive). On timeout we keep the previous render.
            hr = dup->AcquireNextFrame(15, &fi, &resource);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> t;
                if (SUCCEEDED(resource.As(&t))) {
                    renderCurrent(t.Get());
                }
                dup->ReleaseFrame();
                resource.Reset();
            } else if (hr == DXGI_ERROR_ACCESS_LOST) {
                // Display mode/HDR toggle invalidated the duplication. For the
                // prototype we stop cleanly rather than re-establishing it.
                break;
            } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
                Thr(hr, "AcquireNextFrame");
            }

            // Emit frames to match elapsed wall-clock time at kFps. This holds
            // steady cadence and duplicates the last frame across idle gaps.
            const int64_t elapsedHns =
                (QpcNow() - start) * kHnsPerSec / freq;
            const int64_t target = elapsedHns * kFps / kHnsPerSec;
            while (framesWritten <= target && !stopRequested_.load()) {
                mp4.WriteFrame(bgra, framesWritten * (kHnsPerSec / kFps));
                ++framesWritten;
            }

            Sleep(2);
        }

        mp4.Finalize();
        finalized = true;
    } catch (const RecorderError& e) {
        error_ = e.what;
    } catch (const std::exception& e) {
        error_ = e.what();
    } catch (...) {
        error_ = "Unknown recording error";
    }

    if (!finalized) {
        // Best-effort flush so a partially recorded clip is still playable.
        try {
            mp4.Finalize();
        } catch (...) {
        }
    }

    running_.store(false);
    if (comInit) {
        CoUninitialize();
    }
}

}  // namespace sundial
