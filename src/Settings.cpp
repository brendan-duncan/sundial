#include "Settings.h"

#include <Windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

namespace sundial {
namespace {

std::filesystem::path AppDataDir() {
    PWSTR path = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
                                       &path))) {
        dir = path;
        CoTaskMemFree(path);
    } else {
        dir = L".";
    }
    dir /= L"Sundial";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()),
                                nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::filesystem::path PresetsDir() {
    auto dir = AppDataDir() / L"presets";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string Trim(std::string s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(s.front())) s.erase(s.begin());
    while (!s.empty() && isSpace(s.back())) s.pop_back();
    return s;
}

void WriteTonemapParams(std::ostream& f, const TonemapParams& p) {
    f << "sdr_white_nits = " << p.sdrWhiteNits << "\n";
    f << "exposure_ev = " << p.exposureEV << "\n";
    f << "curve = " << static_cast<int>(p.curve) << "\n";
    f << "saturation = " << p.saturation << "\n";
    f << "black_point_lift = " << p.blackPointLift << "\n";
    f << "highlight_rolloff = " << p.highlightRolloff << "\n";
    f << "temperature = " << p.temperature << "\n";
    f << "tint = " << p.tint << "\n";
    f << "gamut_compress = " << p.gamutCompress << "\n";
    f << "sharpen = " << p.sharpen << "\n";
    f << "r_gain = " << p.rGain << "\n";
    f << "g_gain = " << p.gGain << "\n";
    f << "b_gain = " << p.bGain << "\n";
    f << "output_gamma = " << static_cast<int>(p.outputGamma) << "\n";
    f << "local_strength = " << p.localStrength << "\n";
    f << "knee_point = " << p.kneePoint << "\n";
    f << "highlight_desat = " << p.highlightDesat << "\n";
    f << "source_peak_nits = " << p.sourcePeakNits << "\n";
}

// Apply a single key=value pair to a TonemapParams. Returns false if the key
// isn't a tonemap field (caller can then try other keys).
bool ApplyTonemapKey(TonemapParams& p, const std::string& key,
                     const std::string& value) {
    try {
        if (key == "sdr_white_nits") p.sdrWhiteNits = std::stof(value);
        else if (key == "exposure_ev") p.exposureEV = std::stof(value);
        else if (key == "curve") {
            int c = std::clamp(std::stoi(value), 0, 7);
            p.curve = static_cast<TonemapCurve>(c);
        }
        else if (key == "saturation") p.saturation = std::stof(value);
        else if (key == "black_point_lift") p.blackPointLift = std::stof(value);
        else if (key == "highlight_rolloff") p.highlightRolloff = std::stof(value);
        else if (key == "temperature") p.temperature = std::stof(value);
        else if (key == "tint") p.tint = std::stof(value);
        else if (key == "gamut_compress") p.gamutCompress = std::stof(value);
        else if (key == "sharpen") p.sharpen = std::stof(value);
        else if (key == "r_gain") p.rGain = std::stof(value);
        else if (key == "g_gain") p.gGain = std::stof(value);
        else if (key == "b_gain") p.bGain = std::stof(value);
        else if (key == "output_gamma") {
            int g = std::clamp(std::stoi(value), 0, 2);
            p.outputGamma = static_cast<OutputGamma>(g);
        }
        else if (key == "local_strength") {
            p.localStrength = std::clamp(std::stof(value), 0.0f, 1.0f);
        }
        else if (key == "knee_point") {
            p.kneePoint = std::clamp(std::stof(value), 0.05f, 0.95f);
        }
        else if (key == "highlight_desat") {
            p.highlightDesat = std::clamp(std::stof(value), 0.0f, 1.0f);
        }
        else if (key == "source_peak_nits") {
            p.sourcePeakNits = std::clamp(std::stof(value), 80.0f, 10000.0f);
        }
        else return false;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

std::filesystem::path SettingsFilePath() {
    return AppDataDir() / L"settings.ini";
}

std::filesystem::path DefaultOutputDir() {
    PWSTR pictures = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr,
                                       &pictures))) {
        dir = pictures;
        CoTaskMemFree(pictures);
    } else {
        dir = L".";
    }
    dir /= L"Sundial";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path ResolveOutputDir(const AppSettings& settings) {
    if (settings.outputFolder.empty()) return DefaultOutputDir();
    std::filesystem::path dir = settings.outputFolder;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

AppSettings LoadSettings() {
    AppSettings s;
    std::ifstream f(SettingsFilePath());
    if (!f) return s;

    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (key == "edit_on_capture") {
            s.editOnCapture = (value == "true" || value == "1");
        } else if (key == "save_hdr_jxr") {
            s.saveHdrJxr = (value == "true" || value == "1");
        } else if (key == "auto_copy_capture") {
            s.autoCopyCapture = (value == "true" || value == "1");
        } else if (key == "output_folder") {
            s.outputFolder = Utf8ToWide(value);
        } else {
            ApplyTonemapKey(s.tonemap, key, value);
        }
    }
    return s;
}

void SaveSettings(const AppSettings& s) {
    std::ofstream f(SettingsFilePath(), std::ios::trunc);
    if (!f) return;
    f << "edit_on_capture = " << (s.editOnCapture ? "true" : "false") << "\n";
    f << "save_hdr_jxr = " << (s.saveHdrJxr ? "true" : "false") << "\n";
    f << "auto_copy_capture = " << (s.autoCopyCapture ? "true" : "false")
      << "\n";
    f << "output_folder = " << WideToUtf8(s.outputFolder) << "\n";
    WriteTonemapParams(f, s.tonemap);
}

std::wstring NormalizePresetName(std::wstring name) {
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t c : name) {
        if (c < 32) continue;
        switch (c) {
            case L'<': case L'>': case L':': case L'"':
            case L'/': case L'\\': case L'|': case L'?': case L'*':
                continue;
        }
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) {
        out.pop_back();
    }
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    return out;
}

std::vector<std::wstring> ListPresets() {
    std::vector<std::wstring> names;
    std::error_code ec;
    std::filesystem::directory_iterator it(PresetsDir(), ec);
    if (ec) return names;
    for (const auto& entry : it) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() != L".ini") continue;
        names.push_back(p.stem().wstring());
    }
    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return CompareStringOrdinal(a.c_str(), int(a.size()),
                                              b.c_str(), int(b.size()),
                                              TRUE) == CSTR_LESS_THAN;
              });
    return names;
}

bool LoadPreset(const std::wstring& name, TonemapParams& out) {
    auto norm = NormalizePresetName(name);
    if (norm.empty()) return false;
    auto path = PresetsDir() / (norm + L".ini");
    std::ifstream f(path);
    if (!f) return false;

    out = TonemapParams{};
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));
        if (key.empty()) continue;
        ApplyTonemapKey(out, key, value);
    }
    return true;
}

void SavePreset(const std::wstring& name, const TonemapParams& params) {
    auto norm = NormalizePresetName(name);
    if (norm.empty()) return;
    auto path = PresetsDir() / (norm + L".ini");
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    WriteTonemapParams(f, params);
}

void DeletePreset(const std::wstring& name) {
    auto norm = NormalizePresetName(name);
    if (norm.empty()) return;
    auto path = PresetsDir() / (norm + L".ini");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace sundial
