#include "Encoder.h"

#include "Tonemap.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <system_error>

#if defined(SUNDIAL_HAS_ULTRAHDR)
#include <DirectXPackedVector.h>

#include <fstream>
#include <vector>

#include "ultrahdr_api.h"
#endif

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

void EncodeToStream(IStream* stream,
                    REFGUID containerFormat,
                    REFWICPixelFormatGUID pixelFormat,
                    uint32_t width, uint32_t height,
                    uint32_t stride,
                    const uint8_t* pixels,
                    size_t pixelBytes,
                    bool jxrHdr) {
    auto factory = CreateWic();

    ComPtr<IWICBitmapEncoder> encoder;
    ThrowIfFailed(factory->CreateEncoder(containerFormat, nullptr, &encoder),
                  "WIC CreateEncoder");
    ThrowIfFailed(encoder->Initialize(stream, WICBitmapEncoderNoCache),
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
    EncodeToStream(stream.Get(), containerFormat, pixelFormat, width, height,
                   stride, pixels, pixelBytes, jxrHdr);
}

}  // namespace

std::vector<uint8_t> EncodePngToMemory(const uint8_t* bgra, uint32_t width,
                                       uint32_t height) {
    ComPtr<IStream> stream;
    ThrowIfFailed(CreateStreamOnHGlobal(nullptr, TRUE, &stream),
                  "CreateStreamOnHGlobal");
    EncodeToStream(stream.Get(), GUID_ContainerFormatPng,
                   GUID_WICPixelFormat32bppBGRA, width, height, width * 4,
                   bgra, size_t(width) * height * 4, /*jxrHdr=*/false);

    STATSTG stat{};
    ThrowIfFailed(stream->Stat(&stat, STATFLAG_NONAME), "IStream::Stat");
    const size_t size = size_t(stat.cbSize.QuadPart);

    HGLOBAL hg = nullptr;
    ThrowIfFailed(GetHGlobalFromStream(stream.Get(), &hg),
                  "GetHGlobalFromStream");
    std::vector<uint8_t> out(size);
    if (size > 0) {
        const void* src = GlobalLock(hg);
        if (src) {
            std::memcpy(out.data(), src, size);
            GlobalUnlock(hg);
        }
    }
    return out;
}

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

#if defined(SUNDIAL_HAS_ULTRAHDR)
namespace {

void ThrowIfUhdrFailed(const uhdr_error_info_t& e, const char* what) {
    if (e.error_code == UHDR_CODEC_OK) return;
    std::string msg = what;
    if (e.has_detail) {
        msg += ": ";
        msg += e.detail;
    }
    throw std::runtime_error(msg);
}

struct UhdrEncoder {
    uhdr_codec_private_t* p = uhdr_create_encoder();
    ~UhdrEncoder() {
        if (p) uhdr_release_encoder(p);
    }
};

struct UhdrDecoder {
    uhdr_codec_private_t* p = uhdr_create_decoder();
    ~UhdrDecoder() {
        if (p) uhdr_release_decoder(p);
    }
};

}  // namespace

void SaveUltraHdrJpeg(const Frame& f,
                      const TonemapParams& params,
                      const std::wstring& path) {
    using namespace DirectX::PackedVector;
    if (!f.isHdr) {
        throw std::logic_error("SaveUltraHdrJpeg called on a non-HDR frame");
    }
    if (f.bytesPerPixel != 8) {
        throw std::logic_error("SaveUltraHdrJpeg expects FP16 RGBA pixels");
    }

    // SDR base image: exactly what SavePngTonemapped writes, so an SDR viewer
    // sees the same result. TonemapToBgra8 yields tightly-packed BGRA8; Ultra
    // HDR's 32bppRGBA8888 wants R,G,B,A byte order, so swap R/B as we copy.
    std::vector<uint8_t> sdrBgra = TonemapToBgra8(f, params);
    std::vector<uint8_t> sdrRgba(sdrBgra.size());
    for (size_t i = 0; i + 3 < sdrBgra.size(); i += 4) {
        sdrRgba[i + 0] = sdrBgra[i + 2];  // R
        sdrRgba[i + 1] = sdrBgra[i + 1];  // G
        sdrRgba[i + 2] = sdrBgra[i + 0];  // B
        sdrRgba[i + 3] = 255;             // opaque
    }

    // HDR rendition: the linear scRGB source, scaled so SDR white maps to 1.0
    // (libultrahdr's linear-input reference white) using the same multiplicative
    // factors the tonemap applies before its curve - white scale, exposure, and
    // per-channel gain - so the SDR base and HDR intent share a white balance.
    const float whiteScale = 80.0f / std::max(1.0f, params.sdrWhiteNits);
    const float ev = std::pow(2.0f, params.exposureEV);
    const float gain[3] = {whiteScale * ev * params.rGain,
                           whiteScale * ev * params.gGain,
                           whiteScale * ev * params.bGain};
    const size_t pixelCount = size_t(f.width) * f.height;
    std::vector<HALF> hdrRgba(pixelCount * 4);
    const HALF* src = reinterpret_cast<const HALF*>(f.pixels.data());
    for (size_t i = 0; i < pixelCount; ++i) {
        // scRGB is linear Rec.709 with wide-gamut HDR colors stored as NEGATIVE
        // components. Clamping those to zero (as we used to) clips saturated
        // colors toward gray, so the gain-map HDR looked washed out next to the
        // JXR. Instead convert to linear Rec.2020 (which encloses the captured
        // gamut) - those colors become positive and survive. Tagged
        // UHDR_CG_BT_2100 (Rec.2020 primaries) below. Apply per-channel gain in
        // Rec.709 first (the space the tonemap's white balance lives in), then
        // the matrix.
        const float r = XMConvertHalfToFloat(src[i * 4 + 0]) * gain[0];
        const float g = XMConvertHalfToFloat(src[i * 4 + 1]) * gain[1];
        const float b = XMConvertHalfToFloat(src[i * 4 + 2]) * gain[2];
        const float r2 = 0.6274039f * r + 0.3292830f * g + 0.0433131f * b;
        const float g2 = 0.0690973f * r + 0.9195404f * g + 0.0113623f * b;
        const float b2 = 0.0163914f * r + 0.0880133f * g + 0.8955953f * b;
        hdrRgba[i * 4 + 0] = XMConvertFloatToHalf(std::max(0.0f, r2));
        hdrRgba[i * 4 + 1] = XMConvertFloatToHalf(std::max(0.0f, g2));
        hdrRgba[i * 4 + 2] = XMConvertFloatToHalf(std::max(0.0f, b2));
        hdrRgba[i * 4 + 3] = XMConvertFloatToHalf(1.0f);
    }

    UhdrEncoder enc;
    if (!enc.p) throw std::runtime_error("uhdr_create_encoder failed");

    uhdr_raw_image_t hdr{};
    hdr.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
    hdr.cg = UHDR_CG_BT_2100;  // Rec.2020 primaries (HDR intent converted above)
    hdr.ct = UHDR_CT_LINEAR;
    hdr.range = UHDR_CR_FULL_RANGE;
    hdr.w = f.width;
    hdr.h = f.height;
    hdr.planes[UHDR_PLANE_PACKED] = hdrRgba.data();
    hdr.stride[UHDR_PLANE_PACKED] = f.width;
    ThrowIfUhdrFailed(uhdr_enc_set_raw_image(enc.p, &hdr, UHDR_HDR_IMG),
                      "uhdr_enc_set_raw_image(HDR)");

    uhdr_raw_image_t sdr{};
    sdr.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    sdr.cg = UHDR_CG_BT_709;
    sdr.ct = UHDR_CT_SRGB;
    sdr.range = UHDR_CR_FULL_RANGE;
    sdr.w = f.width;
    sdr.h = f.height;
    sdr.planes[UHDR_PLANE_PACKED] = sdrRgba.data();
    sdr.stride[UHDR_PLANE_PACKED] = f.width;
    ThrowIfUhdrFailed(uhdr_enc_set_raw_image(enc.p, &sdr, UHDR_SDR_IMG),
                      "uhdr_enc_set_raw_image(SDR)");

    // Near-lossless base; a 3-channel gain map preserves colored highlights
    // (a luma-only map would desaturate the recovered HDR).
    ThrowIfUhdrFailed(uhdr_enc_set_quality(enc.p, 95, UHDR_BASE_IMG),
                      "uhdr_enc_set_quality(base)");
    ThrowIfUhdrFailed(uhdr_enc_set_using_multi_channel_gainmap(enc.p, 1),
                      "uhdr_enc_set_using_multi_channel_gainmap");

    ThrowIfUhdrFailed(uhdr_encode(enc.p), "uhdr_encode");

    uhdr_compressed_image_t* out = uhdr_get_encoded_stream(enc.p);
    if (!out || !out->data || out->data_sz == 0) {
        throw std::runtime_error("uhdr_get_encoded_stream returned no data");
    }

    std::ofstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not open Ultra HDR output for writing");
    }
    file.write(reinterpret_cast<const char*>(out->data),
               static_cast<std::streamsize>(out->data_sz));
    if (!file) {
        throw std::runtime_error("failed writing Ultra HDR output");
    }
}

namespace {

// If `path` is an Ultra HDR JPEG, decode it (applying the gain map) into an
// FP16 linear scRGB HDR Frame and return true. Returns false for a plain
// JPEG/PNG/etc. so the caller falls back to the WIC path. WIC alone only ever
// sees the SDR base image of an Ultra HDR file - this is what lets the editor
// re-tone from the recovered HDR instead of the baked SDR.
bool TryLoadUltraHdrFrame(const std::wstring& path, Frame& out) {
    using namespace DirectX::PackedVector;

    std::ifstream f(std::filesystem::path(path),
                    std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize sz = f.tellg();
    if (sz <= 0) return false;
    f.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), sz)) return false;

    if (!is_uhdr_image(bytes.data(), static_cast<int>(bytes.size()))) {
        return false;  // not an Ultra HDR file - let WIC handle it
    }

    UhdrDecoder dec;
    if (!dec.p) return false;

    uhdr_compressed_image_t in{};
    in.data = bytes.data();
    in.data_sz = bytes.size();
    in.capacity = bytes.size();
    in.cg = UHDR_CG_UNSPECIFIED;
    in.ct = UHDR_CT_UNSPECIFIED;
    in.range = UHDR_CR_UNSPECIFIED;

    if (uhdr_dec_set_image(dec.p, &in).error_code) return false;
    if (uhdr_dec_set_out_img_format(dec.p, UHDR_IMG_FMT_64bppRGBAHalfFloat)
            .error_code)
        return false;
    if (uhdr_dec_set_out_color_transfer(dec.p, UHDR_CT_LINEAR).error_code)
        return false;
    // Large display boost so the full recorded gain map is applied (it's
    // internally clamped to the gain map's own max boost), recovering the HDR.
    uhdr_dec_set_out_max_display_boost(dec.p, 10000.0f);
    if (uhdr_dec_probe(dec.p).error_code) return false;
    if (uhdr_decode(dec.p).error_code) return false;

    uhdr_raw_image_t* img = uhdr_get_decoded_image(dec.p);
    if (!img || !img->planes[UHDR_PLANE_PACKED]) return false;

    // scRGB is linear Rec.709. In practice libultrahdr decodes the HDR back into
    // Rec.709 already (cg comes back UHDR_CG_BT_709), so the common case is a
    // straight copy; we still convert if it ever hands back a wider gamut. An
    // unspecified gamut is treated as Rec.709 (identity), matching observed
    // decoder behaviour - assuming a wider gamut there would distort colors.
    const bool from2020 = img->cg == UHDR_CG_BT_2100;
    const bool fromP3 = img->cg == UHDR_CG_DISPLAY_P3;

    const uint32_t W = img->w, H = img->h;
    out.width = W;
    out.height = H;
    out.isHdr = true;
    out.bytesPerPixel = 8;
    out.pixels.resize(size_t(W) * H * 8);
    HALF* dst = reinterpret_cast<HALF*>(out.pixels.data());
    const HALF* srcPlane =
        reinterpret_cast<const HALF*>(img->planes[UHDR_PLANE_PACKED]);
    const uint32_t srcStridePix =
        img->stride[UHDR_PLANE_PACKED] ? img->stride[UHDR_PLANE_PACKED] : W;

    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const size_t s = (size_t(y) * srcStridePix + x) * 4;
            const size_t d = (size_t(y) * W + x) * 4;
            float r = XMConvertHalfToFloat(srcPlane[s + 0]);
            float g = XMConvertHalfToFloat(srcPlane[s + 1]);
            float b = XMConvertHalfToFloat(srcPlane[s + 2]);
            float r7 = r, g7 = g, b7 = b;
            if (from2020) {
                // Rec.2020 -> Rec.709 (inverse of the save-time matrix).
                r7 = 1.6604910f * r - 0.5876411f * g - 0.0728499f * b;
                g7 = -0.1245505f * r + 1.1328999f * g - 0.0083494f * b;
                b7 = -0.0181508f * r - 0.1005789f * g + 1.1187297f * b;
            } else if (fromP3) {
                // Display P3 -> Rec.709 (linear).
                r7 = 1.2249401f * r - 0.2249401f * g + 0.0000000f * b;
                g7 = -0.0420569f * r + 1.0420569f * g + 0.0000000f * b;
                b7 = -0.0196376f * r - 0.0786361f * g + 1.0982737f * b;
            }
            dst[d + 0] = XMConvertFloatToHalf(r7);
            dst[d + 1] = XMConvertFloatToHalf(g7);
            dst[d + 2] = XMConvertFloatToHalf(b7);
            dst[d + 3] = XMConvertFloatToHalf(1.0f);
        }
    }
    return true;
}

}  // namespace
#else
void SaveUltraHdrJpeg(const Frame&, const TonemapParams&,
                      const std::wstring&) {
    throw std::runtime_error(
        "Ultra HDR export was not compiled in (SUNDIAL_HAS_ULTRAHDR off)");
}
#endif

Frame LoadFrameFromFile(const std::wstring& path) {
#if defined(SUNDIAL_HAS_ULTRAHDR)
    // Ultra HDR JPEGs decode through libultrahdr (applying the gain map) so the
    // editor re-opens them as HDR. WIC would only see the SDR base image.
    {
        Frame uhdr;
        if (TryLoadUltraHdrFrame(path, uhdr)) return uhdr;
    }
#endif

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
