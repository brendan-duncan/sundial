#pragma once
#include "HdrCapture.h"
#include "Settings.h"

#include <string>

namespace sundial {

// Write FP16 scRGB pixels as a JPEG XR / HD Photo (.jxr) file. This is the
// same format Game Bar uses for HDR screenshots and is what Windows Photos
// recognises as an HDR image.
void SaveJxrHdr(const Frame& frame, const std::wstring& path);

// Tonemap an HDR frame to SDR with the supplied parameters and write as PNG.
void SavePngTonemapped(const Frame& frame,
                       const TonemapParams& params,
                       const std::wstring& path);

// Write an already-SDR frame straight to PNG (no tonemap).
void SavePngSdr(const Frame& frame, const std::wstring& path);

// Load an image (JXR, PNG, JPEG, etc.) via WIC. JXR / HDR formats decode to
// FP16 RGBA scRGB; everything else decodes to BGRA8. Throws on any failure.
Frame LoadFrameFromFile(const std::wstring& path);

}  // namespace sundial
