#pragma once
#include <Windows.h>
#include <shellapi.h>

#include <functional>

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
    void OnCheckUpdates(Handler h)  { checkUpdates_ = std::move(h); }
    void OnAbout(Handler h)         { about_ = std::move(h); }
    void OnExit(Handler h)          { exit_ = std::move(h); }

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
    Handler checkUpdates_;
    Handler about_;
    Handler exit_;
};

// Cross-instance signaling. A second launch of sundial.exe uses these to ask
// the already-running instance to pop its toolbar (the tray's primary action)
// instead of starting a duplicate process. FindRunningTrayWindow returns
// nullptr when no resident instance has created its tray window yet.
HWND FindRunningTrayWindow();
// Registered window message (the same value in every process) that, posted to
// the tray window, fires the primary action there.
UINT TrayShowToolbarMessage();

}  // namespace sundial
