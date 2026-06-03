#include "TrayIcon.h"

#include <shellapi.h>

#include <string>

#include "Resource.h"
#include "Version.h"

namespace sundial {
namespace {

constexpr wchar_t kClassName[] = L"SundialTray";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kIdCapture = 3001;
constexpr UINT kIdExit = 3002;
constexpr UINT kIdEditImage = 3003;
constexpr UINT kIdCheckUpdates = 3004;
constexpr UINT kIdAbout = 3005;
constexpr UINT kIdSettings = 3006;
constexpr UINT kIconId = 1;

void EnsureClassRegistered() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayIcon::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

LRESULT CALLBACK TrayIcon::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<TrayIcon*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->WndProc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

UINT TrayShowToolbarMessage() {
    // RegisterWindowMessage hands back the same value for a given string in
    // every process, so the second instance and the resident one agree on it
    // without sharing a header-level constant.
    static const UINT msg = RegisterWindowMessageW(L"SundialShowToolbar.v1");
    return msg;
}

UINT TrayReloadHotkeyMessage() {
    static const UINT msg = RegisterWindowMessageW(L"SundialReloadHotkey.v1");
    return msg;
}

HWND FindRunningTrayWindow() {
    // The tray window is message-only (HWND_MESSAGE parent), so it must be
    // searched for under HWND_MESSAGE rather than the desktop.
    return FindWindowExW(HWND_MESSAGE, nullptr, kClassName, nullptr);
}

LRESULT TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == TrayShowToolbarMessage()) {
        // A second launch asked us to bring up the toolbar - same as a
        // left-click on the tray icon.
        if (primary_) {
            primary_();
        }
        return 0;
    }
    if (msg == TrayReloadHotkeyMessage()) {
        // An editor process changed the capture hotkey; re-register it here on
        // the resident instance (and on this, the registering thread).
        if (reloadHotkey_) {
            reloadHotkey_();
        }
        return 0;
    }
    if (msg == kTrayMessage) {
        switch (LOWORD(lp)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                if (primary_) {
                    primary_();
                }
                return 0;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowContextMenu();
                return 0;
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        switch (LOWORD(wp)) {
            case kIdCapture:
                if (primary_) {
                    primary_();
                }
                return 0;
            case kIdEditImage:
                if (editImage_) {
                    editImage_();
                }
                return 0;
            case kIdSettings:
                if (settings_) {
                    settings_();
                }
                return 0;
            case kIdCheckUpdates:
                if (checkUpdates_) {
                    checkUpdates_();
                }
                return 0;
            case kIdAbout:
                if (about_) {
                    about_();
                }
                return 0;
            case kIdExit:
                if (exit_) {
                    exit_();
                }
                return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayIcon::ShowContextMenu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    // Disabled header showing the running version.
    const std::wstring versionItem = L"Sundial v" + AppVersionW();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, versionItem.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const std::wstring captureItem = L"Capture\t" + hotkeyAccel_;
    AppendMenuW(menu, MF_STRING, kIdCapture, captureItem.c_str());
    AppendMenuW(menu, MF_STRING, kIdEditImage, L"Edit Image...");
    AppendMenuW(menu, MF_STRING, kIdSettings, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (checkUpdates_) {
        AppendMenuW(menu, MF_STRING, kIdCheckUpdates, L"Check for Updates...");
    }
    if (about_) {
        AppendMenuW(menu, MF_STRING, kIdAbout, L"About Sundial...");
    }
    AppendMenuW(menu, MF_STRING, kIdExit, L"Exit");

    // SetForegroundWindow before TrackPopupMenu is the standard
    // workaround for the menu not dismissing when the user clicks away.
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0,
                   hwnd_, nullptr);
    DestroyMenu(menu);
}

bool TrayIcon::Initialize(const wchar_t* tooltip) {
    EnsureClassRegistered();

    hwnd_ = CreateWindowExW(0, kClassName, L"SundialTray", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        return false;
    }

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = kIconId;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kTrayMessage;
    nid_.hIcon = LoadIconW(GetModuleHandleW(nullptr),
                           MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!nid_.hIcon) {
        nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    lstrcpynW(nid_.szTip, tooltip ? tooltip : L"Sundial",
              ARRAYSIZE(nid_.szTip));
    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    return true;
}

void TrayIcon::SetHotkeyText(const std::wstring& accel) {
    hotkeyAccel_ = accel;
    if (!hwnd_) {
        return;
    }
    const std::wstring tip = L"Sundial  -  " + accel;
    lstrcpynW(nid_.szTip, tip.c_str(), ARRAYSIZE(nid_.szTip));
    NOTIFYICONDATAW mod{};
    mod.cbSize = sizeof(mod);
    mod.hWnd = hwnd_;
    mod.uID = kIconId;
    mod.uFlags = NIF_TIP;
    lstrcpynW(mod.szTip, tip.c_str(), ARRAYSIZE(mod.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &mod);
}

void TrayIcon::Shutdown() {
    if (hwnd_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

}  // namespace sundial
