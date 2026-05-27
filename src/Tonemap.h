#pragma once
#include "HdrCapture.h"
#include "Settings.h"

#include <cstdint>
#include <vector>

namespace sundial {

// CPU implementation of the tonemap. Takes an FP16 scRGB frame and produces
// 8-bit BGRA in the sRGB encoding using the parameters in `params`.
// (The same parameter set is consumed by the D3D11 pixel-shader path used
// for the editor's live preview - see ShaderTonemap.)
std::vector<uint8_t> TonemapToBgra8(const Frame& hdr,
                                    const TonemapParams& params);

// Scan the frame's luminance distribution and return an sdrWhiteNits value
// that maps roughly the 99th-percentile pixel to SDR white. Useful for an
// "Auto" button next to the SDR-white slider. For non-HDR frames returns
// the default of 80 nits.
float AutoSdrWhite(const Frame& hdr);

}  // namespace sundial
