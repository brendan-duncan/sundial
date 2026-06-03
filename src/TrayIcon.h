#pragma once
#include <Windows.h>
#include <shellapi.h>

#include <functional>
#include <string>

namespace sundial {

// Adds a notification-area icon. Left-click / double-click invokes the
// "primary" handler (usually shows the toolbar). Right-click pops a context
// menu with "Capture" and "Exit" items.
class TrayIcon {
public:
    using Handler = std::function<void()>;

    bool Initialize(const wchar_t* tooltip);
    void Shutdown();

    void OnPrimaryAction(Handler h) { primary_ = std::move(h); }
    void OnEditImage(Handler h)     { editImage_ = std::move(h); }
    void OnSettings(Handler h)      { settings_ = std::move(h); }
    void OnCheckUpdates(Handler h)  { checkUpdates_ = std::move(h); }
    void OnAbout(Handler h)         { about_ = std::move(h); }
    void OnExit(Handler h)          { exit_ = std::move(h); }
    // Fired when another Sundial process (an editor) changes the capture hotkey
    // and posts TrayReloadHotkeyMessage; the handler reloads settings and
    // re-registers the hotkey on this (the resident) process's thread.
    void OnReloadHotkey(Handler h)  { reloadHotkey_ = std::move(h); }

    // Update the accelerator label shown in the tooltip and context menu (e.g.
    // "Win+Shift+X") to match the currently registered hotkey.
    void SetHotkeyText(const std::wstring& accel);

    // Static because the WNDCLASS registration helper needs it from a
    // free-function context. Not called by clients.
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);

private:
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void ShowContextMenu();

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    Handler primary_;
    Handler editImage_;
    Handler settings_;
    Handler checkUpdates_;
    Handler about_;
    Handler exit_;
    Handler reloadHotkey_;
    std::wstring hotkeyAccel_ = L"Win+Shift+X";
};

// Cross-instance signaling. A second launch of sundial.exe uses these to ask
// the already-running instance to pop its toolbar (the tray's primary action)
// instead of starting a duplicate process. FindRunningTrayWindow returns
// nullptr when no resident instance has created its tray window yet.
HWND FindRunningTrayWindow();
// Registered window message (the same value in every process) that, posted to
// the tray window, fires the primary action there.
UINT TrayShowToolbarMessage();
// Registered window message posted by an editor process after it changes the
// capture hotkey, asking the resident instance to reload settings and
// re-register the hotkey (which must happen on the thread that registered it).
UINT TrayReloadHotkeyMessage();

}  // namespace sundial
