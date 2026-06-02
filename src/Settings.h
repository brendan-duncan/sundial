#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace sundial {

enum class TonemapCurve : int {
    LinearClip = 0,
    Reinhard = 1,
    Aces = 2,
    Hable = 3,
    AgX = 4,
    Neutral = 5,        // Khronos PBR Neutral
    PreserveSdr = 6,    // luminance-based soft knee with highlight desaturation
    BT2390 = 7,         // ITU-R BT.2390 EETF (PQ-space reference HDR->SDR curve)
};

// What gamma encoding to apply on the way out. sRGB is correct for PNGs that
// will be displayed in browsers / Photos / etc. The other modes are useful
// if the output is going into a tool that expects different encoding.
enum class OutputGamma : int {
    Srgb = 0,       // standard sRGB curve (default)
    Gamma22 = 1,    // pure 2.2 gamma
    Linear = 2,     // no gamma applied (skip linear->sRGB)
};

struct TonemapParams {
    // Where scRGB "1.0" should sit on the SDR output. The base default is 80
    // (scRGB convention: 1.0 = 80 nits, no rescaling - safe for SDR
    // captures). For HDR captures, SeedTonemapForFrame() in main.cpp seeds a
    // display-appropriate value (typically 200-400 nits) before the editor
    // opens.
    float sdrWhiteNits = 80.0f;
    float exposureEV = 0.0f;         // pre-tonemap exposure in stops
    // Default BT2390: ITU-R reference HDR-to-SDR curve used by Windows Game
    // Bar / broadcasters. Operates in PQ space on luminance with RGB
    // rescaled by the resulting ratio, so hue is preserved exactly and the
    // compression curve is parameterized by source/target peak luminance
    // (the right knobs for matching the OS conversion).
    TonemapCurve curve = TonemapCurve::BT2390;
    float saturation = 1.0f;         // 1.0 = unchanged, 0 = grayscale
    float blackPointLift = 0.0f;     // post-curve shadow lift, 0..1
    // Pre-curve highlight knee. Stays at 0 for SDR sources so they pass
    // through unchanged; SeedTonemapForFrame bumps it to ~0.4 for HDR
    // captures so the sun's bright halo doesn't bleed into the sky.
    float highlightRolloff = 0.0f;
    float temperature = 0.0f;        // -1..1, negative = cool / positive = warm
    float tint = 0.0f;               // -1..1, negative = green / positive = magenta
    // Pull wide-gamut HDR colors that fall outside [0,1] toward gray instead
    // of clipping per-channel. 0 for SDR sources; SeedTonemapForFrame raises
    // it for HDR captures.
    float gamutCompress = 0.0f;
    float sharpen = 0.0f;            // 0..1, unsharp-mask amount on source
    float rGain = 1.0f;              // per-channel multiplier in scene-linear space
    float gGain = 1.0f;
    float bGain = 1.0f;
    OutputGamma outputGamma = OutputGamma::Srgb;
    // Local-tonemap blend (0 = pure global tonemap, 1 = pure local). Local
    // tonemap applies the curve based on a blurred ("local-average") sample
    // and uses that compression ratio on the full-res pixel, so dark UI in
    // front of bright HDR content keeps its dynamic range.
    float localStrength = 0.0f;
    // PreserveSdr / BT2390 knee. Below this normalized brightness the input
    // passes through unchanged; above it the curve compresses toward 1.0.
    // 0.75 matches Microsoft's HDR-to-SDR conversion behavior; lower values
    // give the curve more compression range at the cost of slightly darker
    // mid-tones.
    float kneePoint = 0.75f;
    // Highlight desaturation toward white (Hunt-effect compensation). The
    // brightest pixels lerp from chromatic toward grayscale so the sun looks
    // white-yellow instead of saturated orange. Applied by PreserveSdr and
    // BT2390; other curves ignore it. 0.5 default is moderate; 1.0 forces
    // peak highlights all the way to white.
    float highlightDesat = 0.5f;
    // Reference peak luminance for the BT.2390 curve. The display's reported
    // MaxLuminance (from DXGI_OUTPUT_DESC1) is the natural seed; for static
    // images authored to 1000 nits we cap here so the curve has a reasonable
    // source range to compress from.
    float sourcePeakNits = 1000.0f;
};

// Image-snapshot output formats. A snapshot writes every enabled format that
// applies to the captured frame (PNG always applies; JXR and Ultra HDR JPEG are
// HDR-only). The default PNG + JXR preserves the prior save_hdr_jxr=true
// behavior. Video recording has its own format settings (future) - these are
// for still snapshots only.
struct SnapshotFormats {
    bool png = true;            // PNG (SDR tonemapped, or SDR passthrough)
    bool jxr = true;            // JPEG XR / .jxr (HDR only)
    bool ultraHdrJpeg = false;  // Ultra HDR JPEG / .jpg (SDR+HDR, HDR only)
};

// True if at least one enabled format would produce a file for a frame with the
// given HDR-ness. PNG always qualifies; the HDR formats only when isHdr.
inline bool AnySnapshotFormatApplies(const SnapshotFormats& f, bool isHdr) {
    return f.png || (isHdr && (f.jxr || f.ultraHdrJpeg));
}

struct AppSettings {
    // Snipping-Tool-style default: a capture saves with the current conversion
    // settings, copies to the clipboard, and shows a toast - it does NOT open
    // the editor. Clicking the toast preview opens the editor. Flip this on in
    // Settings to jump straight into the editor on every capture instead.
    bool editOnCapture = false;
    SnapshotFormats snapshot;     // which image formats a snapshot writes
    bool autoCopyCapture = true;  // copy the SDR result to the clipboard after each capture
    // Where saves go. Empty = the platform default returned by
    // DefaultOutputDir() (<user>/Pictures/Sundial). Resolve via
    // ResolveOutputDir() rather than reading this field directly.
    std::wstring outputFolder;
    TonemapParams tonemap;
};

// Settings live at %APPDATA%\Sundial\settings.ini.
std::filesystem::path SettingsFilePath();
AppSettings LoadSettings();
void SaveSettings(const AppSettings& settings);

// <user>/Pictures/Sundial - the default save location when the user hasn't
// overridden outputFolder. Creates the directory if it doesn't exist.
std::filesystem::path DefaultOutputDir();
// outputFolder when set, otherwise DefaultOutputDir(). Always returns an
// existing directory (created on demand).
std::filesystem::path ResolveOutputDir(const AppSettings& settings);

// Named tonemap presets, stored as %APPDATA%\Sundial\presets\<name>.ini.
// NormalizePresetName strips characters that aren't allowed in Windows
// filenames (and trims surrounding whitespace/dots), so the user can type
// freely in a Save As dialog without us refusing the input.
std::wstring NormalizePresetName(std::wstring name);
std::vector<std::wstring> ListPresets();
bool LoadPreset(const std::wstring& name, TonemapParams& out);
void SavePreset(const std::wstring& name, const TonemapParams& params);
void DeletePreset(const std::wstring& name);

}  // namespace sundial
