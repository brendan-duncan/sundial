#pragma once
#include <Windows.h>

#include <cstdint>
#include <vector>

#include "HdrCapture.h"
#include "Settings.h"

namespace sundial {

// Put a tightly-packed, top-down BGRA8 image on the clipboard as a standard
// SDR bitmap. Alpha is forced opaque and the image is published as both
// CF_DIBV5 and CF_DIB so it pastes into modern *and* legacy apps (Paint,
// Office, browsers). `owner` may be null. Returns false if the clipboard
// couldn't be opened/written.
bool CopyBgra8ToClipboard(std::vector<uint8_t> bgra, uint32_t width,
                          uint32_t height, HWND owner = nullptr);

// Copy a captured frame to the clipboard as an SDR image: HDR (FP16) frames
// are tonemapped through `tonemap` first (matching the saved PNG), SDR frames
// are copied as-is. This is always the SDR result - the raw HDR pixels are
// never placed on the clipboard.
bool CopyFrameToClipboard(const Frame& frame, const TonemapParams& tonemap,
                          HWND owner = nullptr);

}  // namespace sundial
