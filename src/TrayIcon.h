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
    Handler exit_;
};

}  // namespace sundial
