#include "ImageOps.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sundial {
namespace {

using DirectX::PackedVector::HALF;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;

uint8_t Clamp8(float x) {
    return static_cast<uint8_t>(std::clamp(x, 0.0f, 255.0f) + 0.5f);
}

}  // namespace

Frame Crop(const Frame& src, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (x >= src.width || y >= src.height) {
        throw std::runtime_error("Crop origin outside source");
    }
    w = std::min(w, src.width - x);
    h = std::min(h, src.height - y);
    if (w == 0 || h == 0) {
        throw std::runtime_error("Crop rectangle is empty");
    }

    Frame out;
    out.width = w;
    out.height = h;
    out.isHdr = src.isHdr;
    out.bytesPerPixel = src.bytesPerPixel;
    out.maxLuminanceNits = src.maxLuminanceNits;
    out.minLuminanceNits = src.minLuminanceNits;
    out.sdrWhiteLevelNits = src.sdrWhiteLevelNits;

    const size_t srcStride = size_t(src.width) * src.bytesPerPixel;
    const size_t dstStride = size_t(w) * src.bytesPerPixel;
    out.pixels.resize(dstStride * h);

    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t* srcRow =
            src.pixels.data() + (size_t(y) + row) * srcStride +
            size_t(x) * src.bytesPerPixel;
        uint8_t* dstRow = out.pixels.data() + size_t(row) * dstStride;
        std::memcpy(dstRow, srcRow, dstStride);
    }
    return out;
}

Frame Resize(const Frame& src, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        throw std::runtime_error("Resize target dimensions must be non-zero");
    }
    if (width == src.width && height == src.height) {
        return src;
    }

    Frame out;
    out.width = width;
    out.height = height;
    out.isHdr = src.isHdr;
    out.bytesPerPixel = src.bytesPerPixel;
    out.maxLuminanceNits = src.maxLuminanceNits;
    out.minLuminanceNits = src.minLuminanceNits;
    out.sdrWhiteLevelNits = src.sdrWhiteLevelNits;

    const size_t dstStride = size_t(width) * src.bytesPerPixel;
    out.pixels.resize(dstStride * height);

    // Sample-the-center mapping: u = (x + 0.5) * (srcW / dstW) - 0.5
    const float xRatio = float(src.width) / float(width);
    const float yRatio = float(src.height) / float(height);

    if (src.isHdr) {
        const uint16_t* srcPx =
            reinterpret_cast<const uint16_t*>(src.pixels.data());
        uint16_t* dstPx = reinterpret_cast<uint16_t*>(out.pixels.data());

        for (uint32_t dy = 0; dy < height; ++dy) {
            float sy = (dy + 0.5f) * yRatio - 0.5f;
            int y0 = int(std::floor(sy));
            int y1 = y0 + 1;
            float fy = sy - y0;
            y0 = std::clamp(y0, 0, int(src.height) - 1);
            y1 = std::clamp(y1, 0, int(src.height) - 1);

            for (uint32_t dx = 0; dx < width; ++dx) {
                float sx = (dx + 0.5f) * xRatio - 0.5f;
                int x0 = int(std::floor(sx));
                int x1 = x0 + 1;
                float fx = sx - x0;
                x0 = std::clamp(x0, 0, int(src.width) - 1);
                x1 = std::clamp(x1, 0, int(src.width) - 1);

                for (int c = 0; c < 4; ++c) {
                    float p00 = XMConvertHalfToFloat(
                        HALF(srcPx[(y0 * src.width + x0) * 4 + c]));
                    float p10 = XMConvertHalfToFloat(
                        HALF(srcPx[(y0 * src.width + x1) * 4 + c]));
                    float p01 = XMConvertHalfToFloat(
                        HALF(srcPx[(y1 * src.width + x0) * 4 + c]));
                    float p11 = XMConvertHalfToFloat(
                        HALF(srcPx[(y1 * src.width + x1) * 4 + c]));
                    float top = p00 + (p10 - p00) * fx;
                    float bot = p01 + (p11 - p01) * fx;
                    float v = top + (bot - top) * fy;
                    dstPx[(dy * width + dx) * 4 + c] = XMConvertFloatToHalf(v);
                }
            }
        }
    } else {
        const uint8_t* srcPx = src.pixels.data();
        uint8_t* dstPx = out.pixels.data();

        for (uint32_t dy = 0; dy < height; ++dy) {
            float sy = (dy + 0.5f) * yRatio - 0.5f;
            int y0 = int(std::floor(sy));
            int y1 = y0 + 1;
            float fy = sy - y0;
            y0 = std::clamp(y0, 0, int(src.height) - 1);
            y1 = std::clamp(y1, 0, int(src.height) - 1);

            for (uint32_t dx = 0; dx < width; ++dx) {
                float sx = (dx + 0.5f) * xRatio - 0.5f;
                int x0 = int(std::floor(sx));
                int x1 = x0 + 1;
                float fx = sx - x0;
                x0 = std::clamp(x0, 0, int(src.width) - 1);
                x1 = std::clamp(x1, 0, int(src.width) - 1);

                for (int c = 0; c < 4; ++c) {
                    float p00 = srcPx[(y0 * src.width + x0) * 4 + c];
                    float p10 = srcPx[(y0 * src.width + x1) * 4 + c];
                    float p01 = srcPx[(y1 * src.width + x0) * 4 + c];
                    float p11 = srcPx[(y1 * src.width + x1) * 4 + c];
                    float top = p00 + (p10 - p00) * fx;
                    float bot = p01 + (p11 - p01) * fx;
                    float v = top + (bot - top) * fy;
                    dstPx[(dy * width + dx) * 4 + c] = Clamp8(v);
                }
            }
        }
    }
    return out;
}

}  // namespace sundial
