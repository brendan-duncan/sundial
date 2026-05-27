#pragma once
#include <cstdint>
#include <vector>

namespace sundial {

struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    bool isHdr = false;           // true when pixels are FP16 RGBA scRGB
    uint32_t bytesPerPixel = 4;   // 8 if HDR (FP16), 4 if SDR (BGRA8)
    float maxLuminanceNits = 80.0f;  // display peak luminance (from EDID/OS)
    float minLuminanceNits = 0.0f;
    float sdrWhiteLevelNits = 80.0f;
    std::vector<uint8_t> pixels;  // tightly packed, row-major
};

Frame CaptureFullScreen();

}  // namespace sundial
