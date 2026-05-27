#pragma once
#include "HdrCapture.h"

#include <cstdint>

namespace sundial {

// All operations preserve frame.isHdr and frame.bytesPerPixel - they work on
// both FP16 (8 bpp) and BGRA8 (4 bpp) frames.

// Returns a new frame containing only the subrectangle [x, y, w, h] of `src`.
// The rectangle is clamped to the source bounds. Throws if it ends up empty.
Frame Crop(const Frame& src, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Returns a new frame resized to `width x height` using bilinear filtering.
// For FP16 sources the filtering is done in linear scene-referred space; for
// BGRA8 it is done directly in the (sRGB-encoded) byte space.
Frame Resize(const Frame& src, uint32_t width, uint32_t height);

}  // namespace sundial
