#include <Windows.h>
#include <ShellScalingApi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>

#include "Clipboard.h"
#include "Editor.h"
#include "Encoder.h"
#include "HdrCapture.h"
#include "ImageOps.h"
#include "Settings.h"
#include "ShellIntegration.h"
#include "Toast.h"
#include "Tonemap.h"
#include "Toolbar.h"
#include "TrayIcon.h"

#include <functional>
#include <vector>

namespace {
// Posted by a screenshot toast (from its own thread) back to the main thread
// when the user clicks the preview, asking the main loop to open the editor
// on the saved file. LPARAM is a heap wchar_t* path the handler frees.
constexpr UINT kMsgEditFile = WM_APP + 1;
DWORD g_mainThreadId = 0;
}  // namespace

namespace {

constexpr int kHotkeyToolbar = 1;

// Max bounding box for the toast's screenshot preview. Kept in sync with the
// thumbnail container size in Toast.cpp - we letterbox the source into this
// box (preserving aspect) and ship the already-resized BGRA8 to the toast.
constexpr uint32_t kToastThumbMaxW = 128;
constexpr uint32_t kToastThumbMaxH = 90;

void ComputeThumbSize(uint32_t srcW, uint32_t srcH,
                      uint32_t& outW, uint32_t& outH) {
    if (srcW == 0 || srcH == 0) {
        outW = outH = 0;
        return;
    }
    const double aspect = double(srcW) / double(srcH);
    const double boxAspect = double(kToastThumbMaxW) / double(kToastThumbMaxH);
    if (aspect > boxAspect) {
        outW = kToastThumbMaxW;
        outH = std::max(1u,
                        uint32_t(double(kToastThumbMaxW) / aspect + 0.5));
    } else {
        outH = kToastThumbMaxH;
        outW = std::max(1u,
                        uint32_t(double(kToastThumbMaxH) * aspect + 0.5));
    }
}

// Produce a BGRA8 thumbnail of the saved frame for the toast. Uses the same
// tonemap the PNG was written with, so the preview matches the file on disk.
// Returns an empty vector if the frame is degenerate (which we then fall
// back to a text-only toast for).
std::vector<uint8_t> MakeToastThumbnail(const sundial::Frame& frame,
                                        const sundial::TonemapParams& tonemap,
                                        uint32_t& outW, uint32_t& outH) {
    ComputeThumbSize(frame.width, frame.height, outW, outH);
    if (outW == 0 || outH == 0) return {};
    // `small` is a Windows COM IDL typedef (rpcndr.h) - avoid that name.
    sundial::Frame thumb = sundial::Resize(frame, outW, outH);
    if (thumb.isHdr) {
        return sundial::TonemapToBgra8(thumb, tonemap);
    }
    return std::move(thumb.pixels);
}

std::wstring Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

void SaveAndNotify(const sundial::Frame& frame,
                   const sundial::TonemapParams& tonemap,
                   bool saveHdrJxr,
                   const std::wstring& pngPath) {
    std::filesystem::path png = pngPath;
    std::filesystem::path jxr = png;
    jxr.replace_extension(L".jxr");
    const std::wstring stem = png.stem().wstring();
    const std::wstring dir = png.parent_path().wstring();

    std::wstring title;
    std::wstring selectInExplorer;
    if (frame.isHdr) {
        if (saveHdrJxr) {
            sundial::SaveJxrHdr(frame, jxr.wstring());
            selectInExplorer = jxr.wstring();
            title = L"HDR saved  -  " + stem + L".jxr + .png";
        } else {
            title = L"HDR saved  -  " + stem + L".png";
        }
        sundial::SavePngTonemapped(frame, tonemap, png.wstring());
        if (selectInExplorer.empty()) selectInExplorer = png.wstring();
    } else {
        sundial::SavePngSdr(frame, png.wstring());
        title = L"Saved  -  " + stem + L".png";
        selectInExplorer = png.wstring();
    }
    const std::wstring body = dir + L"\n(click the preview to edit)";

    uint32_t thumbW = 0, thumbH = 0;
    std::vector<uint8_t> thumb = MakeToastThumbnail(frame, tonemap,
                                                    thumbW, thumbH);

    // Clicking the toast re-opens the saved file in the editor (the JXR when
    // we kept one, so re-toning starts from the full HDR data; otherwise the
    // PNG). The toast lives on its own thread, so marshal back to the main
    // thread - the editor is modal and must run there.
    const std::wstring editPath = selectInExplorer;
    auto onClick = [editPath] {
        wchar_t* heapPath = _wcsdup(editPath.c_str());
        if (!heapPath) return;
        if (!PostThreadMessageW(g_mainThreadId, kMsgEditFile, 0,
                                reinterpret_cast<LPARAM>(heapPath))) {
            free(heapPath);
        }
    };
    sundial::ShowToast(title, body, std::move(onClick),
                       std::move(thumb), thumbW, thumbH);
}

// Seed tonemap values from the captured frame so each capture starts from a
// configuration matched to Windows Game Bar's HDR-to-SDR conversion. The
// user can still override every slider in the editor; on no-edit captures
// these auto-seeded values are what get applied.
//
// For HDR frames (mirrors Game Bar):
//  - sdrWhiteNits = OS-reported SDR white level (from Settings > Display >
//    HDR > "SDR content brightness"). This is exactly what Game Bar uses as
//    its SDR anchor; AutoSdrWhite content estimation isn't part of that
//    behavior, so we leave it as a manual override in the editor.
//  - sourcePeakNits = display MaxLuminance from DXGI (EDID-reported peak),
//    clamped to a sane BT.2390 range. Defines the upper end of the curve.
//  - highlightRolloff and gamutCompress stay at 0: BT.2390 already does a
//    smooth roll-off and the luminance-only scaling keeps colors in gamut.
//    These knobs only help with the per-channel curves (ACES/Hable/Neutral).
//
// For SDR frames we force the HDR-only knobs back to "off" and pull
// sourcePeakNits down to 80 so BT.2390 behaves as identity on SDR data
// (target peak == source peak ⇒ knee start at 1.0 ⇒ passthrough).
void SeedTonemapForFrame(sundial::TonemapParams& tm,
                         const sundial::Frame& frame) {
    if (frame.isHdr) {
        const float os = frame.sdrWhiteLevelNits > 0.0f
                             ? frame.sdrWhiteLevelNits
                             : 200.0f;
        tm.sdrWhiteNits = std::clamp(os, 40.0f, 400.0f);
        const float peak = frame.maxLuminanceNits > 0.0f
                               ? frame.maxLuminanceNits
                               : 1000.0f;
        tm.sourcePeakNits = std::clamp(peak, 400.0f, 4000.0f);
        tm.highlightRolloff = 0.0f;
        tm.gamutCompress = 0.0f;
    } else {
        tm.sdrWhiteNits = 80.0f;       // scRGB 1.0 = 80 nits, no rescale
        tm.exposureEV = 0.0f;
        tm.sourcePeakNits = 80.0f;     // BT.2390 becomes identity
        tm.highlightRolloff = 0.0f;
        tm.gamutCompress = 0.0f;
    }
}

void HandleCapture(sundial::AppSettings& settings, sundial::Frame frame) {
    SeedTonemapForFrame(settings.tonemap, frame);
    if (settings.editOnCapture) {
        auto result = sundial::RunEditor(frame, settings);
        if (!result.saved) return;
        settings.tonemap = result.updatedSettings.tonemap;
        settings.saveHdrJxr = result.updatedSettings.saveHdrJxr;
        sundial::SaveSettings(settings);
        SaveAndNotify(result.editedFrame, settings.tonemap,
                      settings.saveHdrJxr, result.outputPath);
        if (settings.autoCopyCapture) {
            sundial::CopyFrameToClipboard(result.editedFrame, settings.tonemap);
        }
    } else {
        const auto base =
            sundial::ResolveOutputDir(settings) / (L"sundial_" + Timestamp());
        const std::wstring png = base.wstring() + L".png";
        SaveAndNotify(frame, settings.tonemap, settings.saveHdrJxr, png);
        if (settings.autoCopyCapture) {
            sundial::CopyFrameToClipboard(frame, settings.tonemap);
        }
    }
}

void RunEditImageFlow(sundial::AppSettings& settings);
void NotifyVideoSaved(const std::wstring& videoPath);

void RunToolbarFlow(sundial::AppSettings& settings) {
    const bool prevEditOnCapture = settings.editOnCapture;
    const bool prevSaveHdrJxr = settings.saveHdrJxr;
    const bool prevAutoCopy = settings.autoCopyCapture;
    const std::wstring prevOutputFolder = settings.outputFolder;
    auto result = sundial::ShowToolbar(settings);
    if (settings.editOnCapture != prevEditOnCapture ||
        settings.saveHdrJxr != prevSaveHdrJxr ||
        settings.autoCopyCapture != prevAutoCopy ||
        settings.outputFolder != prevOutputFolder) {
        sundial::SaveSettings(settings);
    }
    switch (result.kind) {
        case sundial::ToolbarResult::Kind::FullScreen:
            HandleCapture(settings, sundial::CaptureFullScreen());
            break;
        case sundial::ToolbarResult::Kind::Area: {
            auto frame = sundial::CaptureFullScreen();
            const uint32_t x = uint32_t(std::max(0L, result.area.left));
            const uint32_t y = uint32_t(std::max(0L, result.area.top));
            const uint32_t w = uint32_t(result.area.right - result.area.left);
            const uint32_t h = uint32_t(result.area.bottom - result.area.top);
            frame = sundial::Crop(frame, x, y, w, h);
            HandleCapture(settings, std::move(frame));
            break;
        }
        case sundial::ToolbarResult::Kind::EditImage:
            RunEditImageFlow(settings);
            break;
        case sundial::ToolbarResult::Kind::VideoRecorded:
            NotifyVideoSaved(result.videoPath);
            break;
        case sundial::ToolbarResult::Kind::None:
        default:
            break;
    }
}

// Toast for a finished screen recording (path is the saved .mp4). Recordings
// aren't editable, so clicking opens Explorer at the file (done on the toast's
// own thread, which is fine for ShellExecute).
void NotifyVideoSaved(const std::wstring& videoPath) {
    std::filesystem::path p = videoPath;
    auto onClick = [videoPath] {
        const std::wstring args = L"/select,\"" + videoPath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                      SW_SHOWNORMAL);
    };
    sundial::ShowToast(L"Recording saved  -  " + p.filename().wstring(),
                       p.parent_path().wstring() +
                           L"\n(click to open in Explorer)",
                       std::move(onClick));
}

std::wstring GetExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

std::wstring PickImageToOpen(HWND owner) {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter =
        L"Images (*.jxr;*.png;*.jpg;*.jpeg)\0*.jxr;*.png;*.jpg;*.jpeg\0"
        L"JPEG XR (*.jxr)\0*.jxr\0"
        L"PNG (*.png)\0*.png\0"
        L"All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Edit Image";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) return std::wstring(fileName);
    return std::wstring{};
}

void EditExistingFile(sundial::AppSettings& settings,
                      const std::wstring& path) {
    sundial::Frame frame;
    try {
        frame = sundial::LoadFrameFromFile(path);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Sundial - couldn't open image",
                    MB_ICONERROR);
        return;
    }
    // Seed tonemap defaults from the loaded image, same as on capture, so an
    // SDR PNG doesn't inherit HDR-tuned values from the previous edit
    // session (and vice versa). LoadFrameFromFile doesn't populate display
    // metadata, so AutoSdrWhite uses the file's content distribution alone.
    SeedTonemapForFrame(settings.tonemap, frame);
    // Pre-fill the editor's Save target with the original file's path
    // (forcing a .png extension since the SDR output is always PNG).
    // Save then overwrites the original in place; Save As still opens a
    // dialog defaulted to that path.
    std::filesystem::path defaultSave = path;
    defaultSave.replace_extension(L".png");
    auto result =
        sundial::RunEditor(frame, settings, defaultSave.wstring());
    if (!result.saved) return;
    settings.tonemap = result.updatedSettings.tonemap;
    settings.saveHdrJxr = result.updatedSettings.saveHdrJxr;
    sundial::SaveSettings(settings);
    SaveAndNotify(result.editedFrame, settings.tonemap,
                  settings.saveHdrJxr, result.outputPath);
}

void RunEditImageFlow(sundial::AppSettings& settings) {
    std::wstring path = PickImageToOpen(nullptr);
    if (path.empty()) return;
    try {
        EditExistingFile(settings, path);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Sundial edit failed", MB_ICONERROR);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // CLI: if launched with a file path (e.g. via "Open with Sundial"), edit
    // that one file then exit. This path bypasses the singleton / hotkey /
    // tray so it can coexist with a running tray instance.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool fileMode = false;
    std::wstring filePath;
    if (argv && argc >= 2) {
        filePath = argv[1];
        std::error_code ec;
        if (std::filesystem::exists(filePath, ec)) fileMode = true;
    }
    if (argv) LocalFree(argv);

    if (fileMode) {
        try {
            sundial::AppSettings settings = sundial::LoadSettings();
            EditExistingFile(settings, filePath);
        } catch (const std::exception& e) {
            MessageBoxA(nullptr, e.what(), "Sundial edit failed",
                        MB_ICONERROR);
        }
        CoUninitialize();
        return 0;
    }

    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"SundialInstance.v1");
    if (instanceMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"Sundial is already running. Use Task Manager to end the "
                    L"existing sundial.exe before starting a new one.",
                    L"Sundial", MB_ICONINFORMATION);
        if (instanceMutex) CloseHandle(instanceMutex);
        CoUninitialize();
        return 0;
    }

    // Register / refresh the "Open with Sundial" right-click verb for .jxr.
    sundial::RegisterJxrAssociation(GetExePath());

    if (!RegisterHotKey(nullptr, kHotkeyToolbar,
                        MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'X')) {
        wchar_t buf[256];
        swprintf_s(buf,
                   L"Sundial could not register Win+Shift+X as a hotkey "
                   L"(Win32 error %lu). Another app is probably using it.",
                   GetLastError());
        MessageBoxW(nullptr, buf, L"Sundial", MB_ICONERROR);
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
        CoUninitialize();
        return 1;
    }

    g_mainThreadId = GetCurrentThreadId();
    sundial::AppSettings settings = sundial::LoadSettings();

    auto runFlow = [&] {
        try {
            RunToolbarFlow(settings);
        } catch (const std::exception& e) {
            MessageBoxA(nullptr, e.what(), "Sundial capture failed",
                        MB_ICONERROR);
        }
    };

    sundial::TrayIcon tray;
    tray.OnPrimaryAction(runFlow);
    tray.OnEditImage([&] { RunEditImageFlow(settings); });
    tray.OnExit([] { PostQuitMessage(0); });
    tray.Initialize(L"Sundial  -  Win+Shift+X");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyToolbar) {
            runFlow();
        } else if (msg.message == kMsgEditFile) {
            // A screenshot toast was clicked: open the editor on the saved
            // file. The path was heap-allocated by the toast callback.
            wchar_t* heapPath = reinterpret_cast<wchar_t*>(msg.lParam);
            if (heapPath) {
                std::wstring path(heapPath);
                free(heapPath);
                try {
                    EditExistingFile(settings, path);
                } catch (const std::exception& e) {
                    MessageBoxA(nullptr, e.what(), "Sundial edit failed",
                                MB_ICONERROR);
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    tray.Shutdown();
    UnregisterHotKey(nullptr, kHotkeyToolbar);
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
    CoUninitialize();
    return 0;
}
