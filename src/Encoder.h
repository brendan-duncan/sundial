#pragma once
#include "HdrCapture.h"
#include "Settings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sundial {

// Encode tightly-packed, top-down BGRA8 pixels to PNG bytes in memory. Used to
// hand a real PNG to the clipboard (web/Electron apps read the "PNG" clipboard
// format rather than the DIB formats). Throws on any WIC failure.
std::vector<uint8_t> EncodePngToMemory(const uint8_t* bgra, uint32_t width,
                                       uint32_t height);

// Write FP16 scRGB pixels as a JPEG XR / HD Photo (.jxr) file. This is the
// same format Game Bar uses for HDR screenshots and is what Windows Photos
// recognizes as an HDR image.
void SaveJxrHdr(const Frame& frame, const std::wstring& path);

// Tonemap an HDR frame to SDR with the supplied parameters and write as PNG.
void SavePngTonemapped(const Frame& frame,
                       const TonemapParams& params,
                       const std::wstring& path);

// Write an HDR frame as an Ultra HDR JPEG (.jpg): a normal SDR JPEG (the
// tonemapped result, identical to what SavePngTonemapped produces) plus an
// embedded gain map, so SDR viewers see the tonemapped image and HDR-aware
// viewers (Chrome, Windows Photos, Android, macOS) recover the HDR. The base
// image uses `params`; the gain map is computed from the linear scRGB source
// scaled so SDR white maps to 1.0. Requires `frame.isHdr`. Throws on failure,
// or if the build was configured without libultrahdr (SUNDIAL_HAS_ULTRAHDR).
void SaveUltraHdrJpeg(const Frame& frame,
                      const TonemapParams& params,
                      const std::wstring& path);

// Write an HDR frame as an AVIF (.avif). `mode` selects how the HDR is stored:
//   - AvifHdrMode::Pq: a single 10-bit Rec.2020 PQ image (native HDR AVIF).
//     The image is the linear scRGB source with the editor's exposure / white
//     balance applied (the same multiplicative adjustments SaveUltraHdrJpeg's
//     HDR rendition uses), tone curve NOT applied - so it carries the full HDR
//     like the JXR, just in AVIF.
//   - AvifHdrMode::GainMap: an SDR base image (the tonemapped result, identical
//     to SavePngTonemapped) plus an embedded ISO 21496-1 gain map, so SDR
//     viewers see the tonemapped image and HDR-aware viewers recover the HDR.
// Requires `frame.isHdr`. Throws on failure, or if the build was configured
// without libavif (SUNDIAL_HAS_AVIF).
void SaveAvifHdr(const Frame& frame,
                 const TonemapParams& params,
                 const std::wstring& path,
                 AvifHdrMode mode);

// Write an already-SDR frame straight to PNG (no tonemap).
void SavePngSdr(const Frame& frame, const std::wstring& path);

// Load an image (JXR, PNG, JPEG, etc.) via WIC. JXR / HDR formats decode to
// FP16 RGBA scRGB; everything else decodes to BGRA8. Throws on any failure.
Frame LoadFrameFromFile(const std::wstring& path);

}  // namespace sundial
