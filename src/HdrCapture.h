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

struct TonemapParams;  // Settings.h

// `firstFrame` takes the earliest non-black frame the duplication hands back
// instead of waiting for a fresh present. Use it when capturing before any of
// our UI has been shown, so transient on-screen state (tooltips, hover
// highlights) isn't lost to a repaint that lands while we wait.
Frame CaptureFullScreen(bool firstFrame = false);

// Query Windows for the SDR reference white level (in nits) of the monitor
// named by `gdiDeviceName` (a GDI device string like L"\\.\\DISPLAY1", as found
// in DXGI_OUTPUT_DESC::DeviceName / MONITORINFOEXW::szDevice). This is the
// user's Settings > Display > HDR > "SDR content brightness". Returns 0 on
// failure so callers can fall back to the 80-nit scRGB default. Used both at
// capture time and by the editor to lift its SDR preview/UI to the display's
// SDR white on an scRGB HDR backbuffer.
float QuerySdrWhiteLevelNits(const wchar_t* gdiDeviceName);

// Seed display-dependent tonemap anchors from a freshly captured/loaded frame
// so each capture starts matched to Windows Game Bar's HDR-to-SDR conversion:
// HDR sources get the OS SDR-white level + EDID peak as BT.2390 anchors (HDR
// knobs reset to 0); SDR sources are forced to an 80-nit identity passthrough.
// The user can still override every value afterward. Shared by the screenshot,
// edit-image, and video-recording paths so all three start from the same look.
void SeedTonemapForFrame(TonemapParams& tm, const Frame& frame);

}  // namespace sundial
