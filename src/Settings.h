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
    PreserveSdr = 6,    // identity below ~0.9, soft knee to 1.0 for HDR highlights
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
    float sdrWhiteNits = 80.0f;     // where scRGB "1.0" should sit on the SDR output
    float exposureEV = 0.0f;         // pre-tonemap exposure in stops
    TonemapCurve curve = TonemapCurve::Neutral;
    float saturation = 1.0f;         // 1.0 = unchanged, 0 = grayscale
    float blackPointLift = 0.0f;     // post-curve shadow lift, 0..1
    float highlightRolloff = 0.0f;   // pre-curve highlight knee, 0..1
    float temperature = 0.0f;        // -1..1, negative = cool / positive = warm
    float tint = 0.0f;               // -1..1, negative = green / positive = magenta
    float gamutCompress = 0.0f;      // 0..1, pull out-of-sRGB colors toward gray
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
};

struct AppSettings {
    bool editOnCapture = true;    // on by default - users can disable in Settings
    bool saveHdrJxr = true;       // write a .jxr alongside the .png when capture is HDR
    bool autoCopyCapture = false; // copy the result to the clipboard after each capture
    TonemapParams tonemap;
};

// Settings live at %APPDATA%\Sundial\settings.ini.
std::filesystem::path SettingsFilePath();
AppSettings LoadSettings();
void SaveSettings(const AppSettings& settings);

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
