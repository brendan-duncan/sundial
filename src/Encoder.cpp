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

#if defined(SUNDIAL_HAS_AVIF)
#include <DirectXPackedVector.h>

#include <fstream>
#include <vector>

#include "avif/avif.h"
#endif

using Microsoft::WRL::ComPtr;

namespace sundial {
namespace {

void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::system_error(hr, std::system_category(), what);
    }
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
    if (e.error_code == UHDR_CODEC_OK) {
        return;
    }
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
        if (p) {
            uhdr_release_encoder(p);
        }
    }
};

struct UhdrDecoder {
    uhdr_codec_private_t* p = uhdr_create_decoder();
    ~UhdrDecoder() {
        if (p) {
            uhdr_release_decoder(p);
        }
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
    if (!enc.p) {
        throw std::runtime_error("uhdr_create_encoder failed");
    }

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
    if (!f) {
        return false;
    }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) {
        return false;
    }
    f.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), sz)) {
        return false;
    }

    if (!is_uhdr_image(bytes.data(), static_cast<int>(bytes.size()))) {
        return false;  // not an Ultra HDR file - let WIC handle it
    }

    UhdrDecoder dec;
    if (!dec.p) {
        return false;
    }

    uhdr_compressed_image_t in{};
    in.data = bytes.data();
    in.data_sz = bytes.size();
    in.capacity = bytes.size();
    in.cg = UHDR_CG_UNSPECIFIED;
    in.ct = UHDR_CT_UNSPECIFIED;
    in.range = UHDR_CR_UNSPECIFIED;

    if (uhdr_dec_set_image(dec.p, &in).error_code) {
        return false;
    }
    if (uhdr_dec_set_out_img_format(dec.p, UHDR_IMG_FMT_64bppRGBAHalfFloat)
            .error_code) {
        return false;
    }
    if (uhdr_dec_set_out_color_transfer(dec.p, UHDR_CT_LINEAR).error_code) {
        return false;
    }
    // Large display boost so the full recorded gain map is applied (it's
    // internally clamped to the gain map's own max boost), recovering the HDR.
    uhdr_dec_set_out_max_display_boost(dec.p, 10000.0f);
    if (uhdr_dec_probe(dec.p).error_code) {
        return false;
    }
    if (uhdr_decode(dec.p).error_code) {
        return false;
    }

    uhdr_raw_image_t* img = uhdr_get_decoded_image(dec.p);
    if (!img || !img->planes[UHDR_PLANE_PACKED]) {
        return false;
    }

    // scRGB is linear Rec.709. In practice libultrahdr decodes the HDR back into
    // Rec.709 already (cg comes back UHDR_CG_BT_709), so the common case is a
    // straight copy; we still convert if it ever hands back a wider gamut. An
    // unspecified gamut is treated as Rec.709 (identity), matching observed
    // decoder behavior - assuming a wider gamut there would distort colors.
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

#if defined(SUNDIAL_HAS_AVIF)
namespace {

void ThrowIfAvifFailed(avifResult r, const char* what) {
    if (r == AVIF_RESULT_OK) {
        return;
    }
    std::string msg = what;
    msg += ": ";
    msg += avifResultToString(r);
    throw std::runtime_error(msg);
}

// SMPTE ST 2084 (PQ). Normalized so 1.0 == 10000 cd/m^2 at both ends.
float PqOetf(float l) {  // linear [0,1] -> PQ signal [0,1]
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    l = std::max(0.0f, std::min(1.0f, l));
    const float lm1 = std::pow(l, m1);
    return std::pow((c1 + c2 * lm1) / (1.0f + c3 * lm1), m2);
}
float PqEotf(float e) {  // PQ signal [0,1] -> linear [0,1]
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    e = std::max(0.0f, std::min(1.0f, e));
    const float ep = std::pow(e, 1.0f / m2);
    const float num = std::max(ep - c1, 0.0f);
    const float den = c2 - c3 * ep;
    return std::pow(num / den, 1.0f / m1);
}
// HLG (ARIB STD-B67) inverse OETF: signal [0,1] -> scene linear [0,1].
float HlgInvOetf(float e) {
    constexpr float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    e = std::max(0.0f, e);
    if (e <= 0.5f) {
        return (e * e) / 3.0f;
    }
    return (std::exp((e - c) / a) + b) / 12.0f;
}

// Linear primary conversions (same constants as the Ultra HDR path).
void Rec709ToRec2020(float r, float g, float b, float& r2, float& g2,
                     float& b2) {
    r2 = 0.6274039f * r + 0.3292830f * g + 0.0433131f * b;
    g2 = 0.0690973f * r + 0.9195404f * g + 0.0113623f * b;
    b2 = 0.0163914f * r + 0.0880133f * g + 0.8955953f * b;
}
void Rec2020ToRec709(float r, float g, float b, float& r7, float& g7,
                     float& b7) {
    r7 = 1.6604910f * r - 0.5876411f * g - 0.0728499f * b;
    g7 = -0.1245505f * r + 1.1328999f * g - 0.0083494f * b;
    b7 = -0.0181508f * r - 0.1005789f * g + 1.1187297f * b;
}
void DisplayP3ToRec709(float r, float g, float b, float& r7, float& g7,
                       float& b7) {
    r7 = 1.2249401f * r - 0.2249401f * g + 0.0000000f * b;
    g7 = -0.0420569f * r + 1.0420569f * g + 0.0000000f * b;
    b7 = -0.0196376f * r - 0.0786361f * g + 1.0982737f * b;
}

struct AvifImagePtr {
    avifImage* p = nullptr;
    ~AvifImagePtr() { if (p) avifImageDestroy(p); }
};
struct AvifEncoderPtr {
    avifEncoder* p = avifEncoderCreate();
    ~AvifEncoderPtr() { if (p) avifEncoderDestroy(p); }
};
struct AvifDecoderPtr {
    avifDecoder* p = avifDecoderCreate();
    ~AvifDecoderPtr() { if (p) avifDecoderDestroy(p); }
};
struct AvifRgb {
    avifRGBImage img{};
    bool allocated = false;
    ~AvifRgb() { if (allocated) avifRGBImageFreePixels(&img); }
};

// The linear scRGB (Rec.709) source scaled so SDR white maps to 1.0, with the
// editor's exposure / white balance applied - the same HDR rendition
// SaveUltraHdrJpeg uses (tone curve NOT applied). Packed RGB, 3 floats/pixel.
std::vector<float> BuildNormalizedHdrLinear709(const Frame& f,
                                               const TonemapParams& params) {
    using namespace DirectX::PackedVector;
    const float whiteScale = 80.0f / std::max(1.0f, params.sdrWhiteNits);
    const float ev = std::pow(2.0f, params.exposureEV);
    const float gain[3] = {whiteScale * ev * params.rGain,
                           whiteScale * ev * params.gGain,
                           whiteScale * ev * params.bGain};
    const size_t pixelCount = size_t(f.width) * f.height;
    std::vector<float> out(pixelCount * 3);
    const HALF* src = reinterpret_cast<const HALF*>(f.pixels.data());
    for (size_t i = 0; i < pixelCount; ++i) {
        out[i * 3 + 0] = XMConvertHalfToFloat(src[i * 4 + 0]) * gain[0];
        out[i * 3 + 1] = XMConvertHalfToFloat(src[i * 4 + 1]) * gain[1];
        out[i * 3 + 2] = XMConvertHalfToFloat(src[i * 4 + 2]) * gain[2];
    }
    return out;
}

void WriteAvifToFile(const avifRWData& output, const std::wstring& path) {
    if (!output.data || output.size == 0) {
        throw std::runtime_error("avifEncoderWrite returned no data");
    }
    std::ofstream file(std::filesystem::path(path), std::ios::binary);
    bool ok = static_cast<bool>(file);
    if (ok) {
        file.write(reinterpret_cast<const char*>(output.data),
                   static_cast<std::streamsize>(output.size));
        ok = static_cast<bool>(file);
    }
    if (!ok) {
        throw std::runtime_error("failed writing AVIF output");
    }
}

}  // namespace

void SaveAvifHdr(const Frame& f, const TonemapParams& params,
                 const std::wstring& path, AvifHdrMode mode) {
    using namespace DirectX::PackedVector;
    if (!f.isHdr) {
        throw std::logic_error("SaveAvifHdr called on a non-HDR frame");
    }
    if (f.bytesPerPixel != 8) {
        throw std::logic_error("SaveAvifHdr expects FP16 RGBA pixels");
    }

    const uint32_t W = f.width, H = f.height;
    const std::vector<float> hdr709 = BuildNormalizedHdrLinear709(f, params);

    AvifEncoderPtr enc;
    if (!enc.p) {
        throw std::runtime_error("avifEncoderCreate failed");
    }
    enc.p->maxThreads = 4;
    enc.p->speed = 6;     // still-image speed/size tradeoff (aom: 0 slow..10 fast)
    enc.p->quality = 90;  // near-visually-lossless base

    avifRWData output = AVIF_DATA_EMPTY;
    AvifImagePtr image;  // base/PQ image; outlives the encode call

    if (mode == AvifHdrMode::Pq) {
        // Native 10-bit Rec.2020 PQ. Convert the normalized linear Rec.709
        // rendition to Rec.2020, scale to absolute PQ luminance (SDR white ==
        // sdrWhiteNits), PQ-encode, and quantize to 10-bit full range.
        image.p = avifImageCreate(W, H, 10, AVIF_PIXEL_FORMAT_YUV444);
        if (!image.p) {
            throw std::runtime_error("avifImageCreate failed");
        }
        image.p->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
        image.p->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_PQ;
        image.p->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
        image.p->yuvRange = AVIF_RANGE_FULL;

        AvifRgb rgb;
        avifRGBImageSetDefaults(&rgb.img, image.p);
        rgb.img.format = AVIF_RGB_FORMAT_RGBA;
        rgb.img.depth = 10;
        rgb.img.ignoreAlpha = AVIF_TRUE;  // captures are opaque
        ThrowIfAvifFailed(avifRGBImageAllocatePixels(&rgb.img),
                          "avifRGBImageAllocatePixels(PQ)");
        rgb.allocated = true;

        const float pqScale = params.sdrWhiteNits / 10000.0f;  // norm -> PQ L
        uint16_t maxCll = 0;
        for (uint32_t y = 0; y < H; ++y) {
            uint16_t* row = reinterpret_cast<uint16_t*>(
                rgb.img.pixels + size_t(y) * rgb.img.rowBytes);
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                float r2, g2, b2;
                Rec709ToRec2020(hdr709[i * 3 + 0], hdr709[i * 3 + 1],
                                hdr709[i * 3 + 2], r2, g2, b2);
                r2 = std::max(0.0f, r2);
                g2 = std::max(0.0f, g2);
                b2 = std::max(0.0f, b2);
                const float peakNits =
                    std::max(r2, std::max(g2, b2)) * params.sdrWhiteNits;
                maxCll = std::max(maxCll,
                                  uint16_t(std::min(65535.0f, peakNits)));
                row[x * 4 + 0] =
                    uint16_t(std::lround(PqOetf(r2 * pqScale) * 1023.0f));
                row[x * 4 + 1] =
                    uint16_t(std::lround(PqOetf(g2 * pqScale) * 1023.0f));
                row[x * 4 + 2] =
                    uint16_t(std::lround(PqOetf(b2 * pqScale) * 1023.0f));
                row[x * 4 + 3] = 1023;  // opaque
            }
        }
        image.p->clli.maxCLL = maxCll;
        ThrowIfAvifFailed(avifImageRGBToYUV(image.p, &rgb.img),
                          "avifImageRGBToYUV(PQ)");
        ThrowIfAvifFailed(avifEncoderWrite(enc.p, image.p, &output),
                          "avifEncoderWrite(PQ)");
    } else {
        // Gain-map AVIF: SDR base (the tonemapped result, like the PNG / Ultra
        // HDR base) plus an ISO 21496-1 gain map that recovers the HDR.
        std::vector<uint8_t> sdrBgra = TonemapToBgra8(f, params);

        image.p = avifImageCreate(W, H, 8, AVIF_PIXEL_FORMAT_YUV444);
        if (!image.p) {
            throw std::runtime_error("avifImageCreate failed");
        }
        image.p->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
        image.p->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
        image.p->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
        image.p->yuvRange = AVIF_RANGE_FULL;

        AvifRgb baseRgb;
        avifRGBImageSetDefaults(&baseRgb.img, image.p);
        baseRgb.img.format = AVIF_RGB_FORMAT_RGBA;
        baseRgb.img.depth = 8;
        baseRgb.img.ignoreAlpha = AVIF_TRUE;  // captures are opaque
        ThrowIfAvifFailed(avifRGBImageAllocatePixels(&baseRgb.img),
                          "avifRGBImageAllocatePixels(base)");
        baseRgb.allocated = true;
        for (uint32_t y = 0; y < H; ++y) {
            uint8_t* row = baseRgb.img.pixels + size_t(y) * baseRgb.img.rowBytes;
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                row[x * 4 + 0] = sdrBgra[i * 4 + 2];  // R
                row[x * 4 + 1] = sdrBgra[i * 4 + 1];  // G
                row[x * 4 + 2] = sdrBgra[i * 4 + 0];  // B
                row[x * 4 + 3] = 255;                 // opaque
            }
        }
        ThrowIfAvifFailed(avifImageRGBToYUV(image.p, &baseRgb.img),
                          "avifImageRGBToYUV(base)");

        // Alternate (HDR) rendition for gain-map computation: the normalized
        // linear source in Rec.2020 (so wide-gamut colors stay positive),
        // half-float, SDR white == 1.0.
        AvifRgb altRgb;
        altRgb.img.width = W;
        altRgb.img.height = H;
        altRgb.img.depth = 16;
        altRgb.img.format = AVIF_RGB_FORMAT_RGBA;
        altRgb.img.isFloat = AVIF_TRUE;
        ThrowIfAvifFailed(avifRGBImageAllocatePixels(&altRgb.img),
                          "avifRGBImageAllocatePixels(alt)");
        altRgb.allocated = true;
        for (uint32_t y = 0; y < H; ++y) {
            HALF* row = reinterpret_cast<HALF*>(
                altRgb.img.pixels + size_t(y) * altRgb.img.rowBytes);
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                float r2, g2, b2;
                Rec709ToRec2020(hdr709[i * 3 + 0], hdr709[i * 3 + 1],
                                hdr709[i * 3 + 2], r2, g2, b2);
                row[x * 4 + 0] = XMConvertFloatToHalf(std::max(0.0f, r2));
                row[x * 4 + 1] = XMConvertFloatToHalf(std::max(0.0f, g2));
                row[x * 4 + 2] = XMConvertFloatToHalf(std::max(0.0f, b2));
                row[x * 4 + 3] = XMConvertFloatToHalf(1.0f);
            }
        }

        avifGainMap* gainMap = avifGainMapCreate();
        if (!gainMap) {
            throw std::runtime_error("avifGainMapCreate failed");
        }
        const avifResult gr = avifRGBImageComputeGainMap(
            &baseRgb.img, AVIF_COLOR_PRIMARIES_BT709,
            AVIF_TRANSFER_CHARACTERISTICS_SRGB, &altRgb.img,
            AVIF_COLOR_PRIMARIES_BT2020, AVIF_TRANSFER_CHARACTERISTICS_LINEAR,
            gainMap, nullptr);
        if (gr != AVIF_RESULT_OK) {
            avifGainMapDestroy(gainMap);
            ThrowIfAvifFailed(gr, "avifRGBImageComputeGainMap");
        }
        // Ownership transfers to the image; avifImageDestroy frees it.
        image.p->gainMap = gainMap;
        enc.p->qualityGainMap = 90;
        ThrowIfAvifFailed(avifEncoderWrite(enc.p, image.p, &output),
                          "avifEncoderWrite(gainmap)");
    }

    struct RWGuard {
        avifRWData* d;
        ~RWGuard() { avifRWDataFree(d); }
    } rwGuard{&output};
    WriteAvifToFile(output, path);
}

namespace {

// If `path` is an AVIF, decode it into a Frame and return true: gain-map AVIFs
// recover the HDR via the gain map; PQ / HLG AVIFs decode to linear scRGB HDR;
// plain SDR AVIFs decode to BGRA8. Returns false for non-AVIF input so the
// caller falls back to WIC. Without this, WIC can only open AVIF when the OS
// AV1 Image Extension happens to be installed (and never recovers gain maps).
bool TryLoadAvifFrame(const std::wstring& path, Frame& out) {
    using namespace DirectX::PackedVector;

    std::ifstream f(std::filesystem::path(path),
                    std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) {
        return false;
    }
    f.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), sz)) {
        return false;
    }

    avifROData ro{bytes.data(), bytes.size()};
    if (!avifPeekCompatibleFileType(&ro)) {
        return false;  // not AVIF -> WIC
    }

    AvifDecoderPtr dec;
    if (!dec.p) {
        return false;
    }
    dec.p->maxThreads = 4;
    dec.p->imageContentToDecode = AVIF_IMAGE_CONTENT_ALL;  // include gain map
    if (avifDecoderSetIOMemory(dec.p, bytes.data(), bytes.size()) !=
        AVIF_RESULT_OK) {
        return false;
    }
    if (avifDecoderParse(dec.p) != AVIF_RESULT_OK) {
        return false;
    }
    if (avifDecoderNextImage(dec.p) != AVIF_RESULT_OK) {
        return false;
    }
    avifImage* img = dec.p->image;
    if (!img) {
        return false;
    }
    const uint32_t W = img->width, H = img->height;
    if (W == 0 || H == 0) {
        return false;
    }

    const bool hasGainMap = img->gainMap && img->gainMap->image;

    if (hasGainMap) {
        // Apply the gain map fully (large headroom; internally clamped to the
        // map's own max), output linear Rec.2020, then convert to scRGB Rec.709.
        AvifRgb rgb;
        rgb.img.width = W;
        rgb.img.height = H;
        rgb.img.depth = 16;
        rgb.img.format = AVIF_RGB_FORMAT_RGBA;
        rgb.img.isFloat = AVIF_TRUE;
        if (avifRGBImageAllocatePixels(&rgb.img) != AVIF_RESULT_OK) {
            return false;
        }
        rgb.allocated = true;
        avifContentLightLevelInformationBox clli{};
        if (avifImageApplyGainMap(img, img->gainMap, /*hdrHeadroom=*/14.0f,
                                  AVIF_COLOR_PRIMARIES_BT2020,
                                  AVIF_TRANSFER_CHARACTERISTICS_LINEAR, &rgb.img,
                                  &clli, nullptr) != AVIF_RESULT_OK) {
            return false;
        }
        out.width = W;
        out.height = H;
        out.isHdr = true;
        out.bytesPerPixel = 8;
        out.pixels.resize(size_t(W) * H * 8);
        HALF* dst = reinterpret_cast<HALF*>(out.pixels.data());
        for (uint32_t y = 0; y < H; ++y) {
            const HALF* row = reinterpret_cast<const HALF*>(
                rgb.img.pixels + size_t(y) * rgb.img.rowBytes);
            for (uint32_t x = 0; x < W; ++x) {
                float r7, g7, b7;
                Rec2020ToRec709(XMConvertHalfToFloat(row[x * 4 + 0]),
                                XMConvertHalfToFloat(row[x * 4 + 1]),
                                XMConvertHalfToFloat(row[x * 4 + 2]), r7, g7,
                                b7);
                const size_t d = (size_t(y) * W + x) * 4;
                dst[d + 0] = XMConvertFloatToHalf(r7);
                dst[d + 1] = XMConvertFloatToHalf(g7);
                dst[d + 2] = XMConvertFloatToHalf(b7);
                dst[d + 3] = XMConvertFloatToHalf(1.0f);
            }
        }
        return true;
    }

    const bool isPq =
        img->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_PQ;
    const bool isHlg =
        img->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_HLG;

    if (isPq || isHlg) {
        // Decode to half-float RGB (still in the PQ/HLG signal domain), then
        // linearize and rescale to scRGB (1.0 == 80 nits).
        AvifRgb rgb;
        avifRGBImageSetDefaults(&rgb.img, img);
        rgb.img.format = AVIF_RGB_FORMAT_RGBA;
        rgb.img.depth = 16;
        rgb.img.isFloat = AVIF_TRUE;
        if (avifRGBImageAllocatePixels(&rgb.img) != AVIF_RESULT_OK) {
            return false;
        }
        rgb.allocated = true;
        if (avifImageYUVToRGB(img, &rgb.img) != AVIF_RESULT_OK) {
            return false;
        }

        const bool from2020 =
            img->colorPrimaries == AVIF_COLOR_PRIMARIES_BT2020;
        const bool fromP3 =
            img->colorPrimaries == AVIF_COLOR_PRIMARIES_SMPTE432;
        // PQ: signal -> [0,1] of 10000 nits -> scRGB (/80) == *125.
        // HLG: inverse OETF -> scene [0,1], assume 1000-nit peak -> /80 == *12.5.
        const float hlgPeakScale = 1000.0f / 80.0f;

        out.width = W;
        out.height = H;
        out.isHdr = true;
        out.bytesPerPixel = 8;
        out.pixels.resize(size_t(W) * H * 8);
        HALF* dst = reinterpret_cast<HALF*>(out.pixels.data());
        for (uint32_t y = 0; y < H; ++y) {
            const HALF* row = reinterpret_cast<const HALF*>(
                rgb.img.pixels + size_t(y) * rgb.img.rowBytes);
            for (uint32_t x = 0; x < W; ++x) {
                float c[3];
                for (int k = 0; k < 3; ++k) {
                    const float e = XMConvertHalfToFloat(row[x * 4 + k]);
                    c[k] = isPq ? PqEotf(e) * 125.0f
                                : HlgInvOetf(e) * hlgPeakScale;
                }
                float r7 = c[0], g7 = c[1], b7 = c[2];
                if (from2020) {
                    Rec2020ToRec709(c[0], c[1], c[2], r7, g7, b7);
                } else if (fromP3) {
                    DisplayP3ToRec709(c[0], c[1], c[2], r7, g7, b7);
                }
                const size_t d = (size_t(y) * W + x) * 4;
                dst[d + 0] = XMConvertFloatToHalf(r7);
                dst[d + 1] = XMConvertFloatToHalf(g7);
                dst[d + 2] = XMConvertFloatToHalf(b7);
                dst[d + 3] = XMConvertFloatToHalf(1.0f);
            }
        }
        return true;
    }

    // Plain SDR AVIF -> tightly-packed BGRA8 (matches the WIC SDR path).
    AvifRgb rgb;
    avifRGBImageSetDefaults(&rgb.img, img);
    rgb.img.format = AVIF_RGB_FORMAT_BGRA;
    rgb.img.depth = 8;
    if (avifRGBImageAllocatePixels(&rgb.img) != AVIF_RESULT_OK) {
        return false;
    }
    rgb.allocated = true;
    if (avifImageYUVToRGB(img, &rgb.img) != AVIF_RESULT_OK) {
        return false;
    }
    out.width = W;
    out.height = H;
    out.isHdr = false;
    out.bytesPerPixel = 4;
    out.pixels.resize(size_t(W) * H * 4);
    for (uint32_t y = 0; y < H; ++y) {
        std::memcpy(out.pixels.data() + size_t(y) * W * 4,
                    rgb.img.pixels + size_t(y) * rgb.img.rowBytes,
                    size_t(W) * 4);
    }
    return true;
}

}  // namespace
#else
void SaveAvifHdr(const Frame&, const TonemapParams&, const std::wstring&,
                 AvifHdrMode) {
    throw std::runtime_error(
        "AVIF support was not compiled in (SUNDIAL_HAS_AVIF off)");
}
#endif

Frame LoadFrameFromFile(const std::wstring& path) {
#if defined(SUNDIAL_HAS_ULTRAHDR)
    // Ultra HDR JPEGs decode through libultrahdr (applying the gain map) so the
    // editor re-opens them as HDR. WIC would only see the SDR base image.
    {
        Frame uhdr;
        if (TryLoadUltraHdrFrame(path, uhdr)) {
            return uhdr;
        }
    }
#endif

#if defined(SUNDIAL_HAS_AVIF)
    // AVIF (PQ / HLG / gain-map / SDR) decodes through libavif. WIC has no
    // built-in AVIF decoder unless the OS AV1 Image Extension is installed, and
    // even then would never recover a gain map's HDR.
    {
        Frame avif;
        if (TryLoadAvifFrame(path, avif)) {
            return avif;
        }
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
