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

// Valid range of the SDR-white anchor. The top matches the maximum of the
// Windows "SDR content brightness" slider (480 nits).
inline constexpr float kMinSdrWhiteNits = 40.0f;
inline constexpr float kMaxSdrWhiteNits = 480.0f;

// Local tonemap: the pyramid level whose log-luminance acts as the "base"
// layer. Chosen so the coarse grid is ~32-63 texels across the long side,
// independent of resolution. Shared by the CPU path and the GPU preview so
// both pick the same scale.
inline int LocalTonemapLevel(uint32_t width, uint32_t height) {
    uint32_t longSide = width > height ? width : height;
    int level = 0;
    while ((longSide >> (level + 1)) >= 32u) {
        ++level;
    }
    return level;
}

// Local-tonemap bilateral parameters (shared with the HLSL): spatial sigma in
// coarse texels, range sigma in stops (log2 luminance).
inline constexpr float kLocalSigmaSpatial = 1.0f;
inline constexpr float kLocalSigmaRange = 1.0f;

}  // namespace sundial
