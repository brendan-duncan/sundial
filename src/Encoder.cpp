#include "Encoder.h"

#include "Tonemap.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <stdexcept>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace sundial {
namespace {

void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::system_error(hr, std::system_category(), what);
}

ComPtr<IWICImagingFactory> CreateWic() {
    ComPtr<IWICImagingFactory> factory;
    ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory)),
                  "CoCreateInstance(WICImagingFactory)");
    return factory;
}

void WriteImage(const std::wstring& path,
                REFGUID containerFormat,
                REFWICPixelFormatGUID pixelFormat,
                uint32_t width, uint32_t height,
                uint32_t stride,
                const uint8_t* pixels,
                size_t pixelBytes,
                bool jxrHdr) {
    auto factory = CreateWic();

    ComPtr<IWICStream> stream;
    ThrowIfFailed(factory->CreateStream(&stream), "WIC CreateStream");
    ThrowIfFailed(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
                  "IWICStream::InitializeFromFilename");

    ComPtr<IWICBitmapEncoder> encoder;
    ThrowIfFailed(factory->CreateEncoder(containerFormat, nullptr, &encoder),
                  "WIC CreateEncoder");
    ThrowIfFailed(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
                  "IWICBitmapEncoder::Initialize");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    ThrowIfFailed(encoder->CreateNewFrame(&frame, &props),
                  "IWICBitmapEncoder::CreateNewFrame");

    if (jxrHdr) {
        // Near-lossless quality; the JXR codec handles FP16 natively so
        // we don't need to touch transform options.
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v{};
        v.vt = VT_R4;
        v.fltVal = 0.95f;
        props->Write(1, &opt, &v);
    }

    ThrowIfFailed(frame->Initialize(props.Get()),
                  "IWICBitmapFrameEncode::Initialize");
    ThrowIfFailed(frame->SetSize(width, height),
                  "IWICBitmapFrameEncode::SetSize");

    WICPixelFormatGUID requested = pixelFormat;
    ThrowIfFailed(frame->SetPixelFormat(&requested),
                  "IWICBitmapFrameEncode::SetPixelFormat");
    if (requested != pixelFormat) {
        throw std::runtime_error(
            "WIC encoder rejected the requested pixel format");
    }

    ThrowIfFailed(frame->WritePixels(height, stride,
                                     static_cast<UINT>(pixelBytes),
                                     const_cast<BYTE*>(pixels)),
                  "IWICBitmapFrameEncode::WritePixels");
    ThrowIfFailed(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    ThrowIfFailed(encoder->Commit(), "IWICBitmapEncoder::Commit");
}

}  // namespace

void SaveJxrHdr(const Frame& f, const std::wstring& path) {
    if (!f.isHdr) {
        throw std::logic_error("SaveJxrHdr called on a non-HDR frame");
    }
    const uint32_t stride = f.width * 8;
    WriteImage(path, GUID_ContainerFormatWmp,
               GUID_WICPixelFormat64bppRGBAHalf,
               f.width, f.height, stride,
               f.pixels.data(), f.pixels.size(),
               /*jxrHdr=*/true);
}

void SavePngTonemapped(const Frame& f,
                       const TonemapParams& params,
                       const std::wstring& path) {
    if (!f.isHdr) {
        throw std::logic_error("SavePngTonemapped called on a non-HDR frame");
    }
    auto sdr = TonemapToBgra8(f, params);
    const uint32_t stride = f.width * 4;
    WriteImage(path, GUID_ContainerFormatPng,
               GUID_WICPixelFormat32bppBGRA,
               f.width, f.height, stride,
               sdr.data(), sdr.size(),
               /*jxrHdr=*/false);
}

Frame LoadFrameFromFile(const std::wstring& path) {
    auto factory = CreateWic();

    ComPtr<IWICBitmapDecoder> decoder;
    ThrowIfFailed(
        factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder),
        "WIC CreateDecoderFromFilename");

    ComPtr<IWICBitmapFrameDecode> wicFrame;
    ThrowIfFailed(decoder->GetFrame(0, &wicFrame), "WIC GetFrame");

    UINT width = 0, height = 0;
    ThrowIfFailed(wicFrame->GetSize(&width, &height), "WIC GetSize");

    WICPixelFormatGUID srcFormat{};
    ThrowIfFailed(wicFrame->GetPixelFormat(&srcFormat),
                  "WIC GetPixelFormat");

    // Anything that came in as float / wide-gamut gets decoded to FP16
    // scRGB so the editor pipeline treats it as HDR.
    const bool isHdr =
        srcFormat == GUID_WICPixelFormat64bppRGBAHalf ||
        srcFormat == GUID_WICPixelFormat64bppPRGBAHalf ||
        srcFormat == GUID_WICPixelFormat128bppRGBAFloat ||
        srcFormat == GUID_WICPixelFormat128bppPRGBAFloat ||
        srcFormat == GUID_WICPixelFormat96bppRGBFloat ||
        srcFormat == GUID_WICPixelFormat48bppRGB ||
        srcFormat == GUID_WICPixelFormat64bppRGBA;

    WICPixelFormatGUID target = isHdr ? GUID_WICPixelFormat64bppRGBAHalf
                                      : GUID_WICPixelFormat32bppBGRA;

    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(factory->CreateFormatConverter(&converter),
                  "WIC CreateFormatConverter");
    ThrowIfFailed(
        converter->Initialize(wicFrame.Get(), target,
                              WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeMedianCut),
        "WIC FormatConverter::Initialize");

    Frame out;
    out.width = width;
    out.height = height;
    out.isHdr = isHdr;
    out.bytesPerPixel = isHdr ? 8u : 4u;
    const uint32_t stride = width * out.bytesPerPixel;
    out.pixels.resize(size_t(stride) * height);
    ThrowIfFailed(
        converter->CopyPixels(nullptr, stride,
                              static_cast<UINT>(out.pixels.size()),
                              out.pixels.data()),
        "WIC FormatConverter::CopyPixels");
    return out;
}

void SavePngSdr(const Frame& f, const std::wstring& path) {
    if (f.isHdr) {
        throw std::logic_error("SavePngSdr called on an HDR frame");
    }
    const uint32_t stride = f.width * 4;
    WriteImage(path, GUID_ContainerFormatPng,
               GUID_WICPixelFormat32bppBGRA,
               f.width, f.height, stride,
               f.pixels.data(), f.pixels.size(),
               /*jxrHdr=*/false);
}

}  // namespace sundial
