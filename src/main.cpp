#include <Windows.h>
#include <ShellScalingApi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <exception>
#include <filesystem>
#include <string>

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

#include <cstring>
#include <vector>

namespace {

constexpr int kHotkeyToolbar = 1;

std::filesystem::path OutputDir() {
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

std::wstring Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

bool CopyBgra8ToClipboard(const std::vector<uint8_t>& bgra, uint32_t width,
                          uint32_t height) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const size_t imgSize = bgra.size();
    HGLOBAL hMem =
        GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + imgSize);
    if (!hMem) {
        CloseClipboard();
        return false;
    }
    auto* hdr = static_cast<BITMAPV5HEADER*>(GlobalLock(hMem));
    ZeroMemory(hdr, sizeof(BITMAPV5HEADER));
    hdr->bV5Size = sizeof(BITMAPV5HEADER);
    hdr->bV5Width = int(width);
    hdr->bV5Height = -int(height);   // top-down rows
    hdr->bV5Planes = 1;
    hdr->bV5BitCount = 32;
    hdr->bV5Compression = BI_BITFIELDS;
    hdr->bV5SizeImage = uint32_t(imgSize);
    hdr->bV5RedMask   = 0x00FF0000;
    hdr->bV5GreenMask = 0x0000FF00;
    hdr->bV5BlueMask  = 0x000000FF;
    hdr->bV5AlphaMask = 0xFF000000;
    hdr->bV5CSType = LCS_sRGB;
    hdr->bV5Intent = LCS_GM_GRAPHICS;
    std::memcpy(reinterpret_cast<uint8_t*>(hdr) + sizeof(BITMAPV5HEADER),
                bgra.data(), imgSize);
    GlobalUnlock(hMem);
    if (!SetClipboardData(CF_DIBV5, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void CopyFrameToClipboard(const sundial::Frame& frame,
                          const sundial::TonemapParams& tonemap) {
    std::vector<uint8_t> bgra =
        frame.isHdr ? sundial::TonemapToBgra8(frame, tonemap) : frame.pixels;
    CopyBgra8ToClipboard(bgra, frame.width, frame.height);
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
    const std::wstring body = dir + L"\n(click to open in Explorer)";
    sundial::ShowToast(title, body, selectInExplorer);
}

void HandleCapture(sundial::AppSettings& settings, sundial::Frame frame) {
    if (settings.editOnCapture) {
        auto result = sundial::RunEditor(frame, settings);
        if (!result.saved) return;
        settings.tonemap = result.updatedSettings.tonemap;
        settings.saveHdrJxr = result.updatedSettings.saveHdrJxr;
        sundial::SaveSettings(settings);
        SaveAndNotify(result.editedFrame, settings.tonemap,
                      settings.saveHdrJxr, result.outputPath);
        if (settings.autoCopyCapture) {
            CopyFrameToClipboard(result.editedFrame, settings.tonemap);
        }
    } else {
        const auto base = OutputDir() / (L"sundial_" + Timestamp());
        const std::wstring png = base.wstring() + L".png";
        SaveAndNotify(frame, settings.tonemap, settings.saveHdrJxr, png);
        if (settings.autoCopyCapture) {
            CopyFrameToClipboard(frame, settings.tonemap);
        }
    }
}

void RunEditImageFlow(sundial::AppSettings& settings);

void RunToolbarFlow(sundial::AppSettings& settings) {
    const bool prevEditOnCapture = settings.editOnCapture;
    const bool prevSaveHdrJxr = settings.saveHdrJxr;
    const bool prevAutoCopy = settings.autoCopyCapture;
    auto result = sundial::ShowToolbar(settings);
    if (settings.editOnCapture != prevEditOnCapture ||
        settings.saveHdrJxr != prevSaveHdrJxr ||
        settings.autoCopyCapture != prevAutoCopy) {
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
        case sundial::ToolbarResult::Kind::None:
        default:
            break;
    }
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
