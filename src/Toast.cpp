#include "Toast.h"

#include <Windows.h>
#include <shellapi.h>

#include <memory>
#include <mutex>

namespace sundial {
namespace {

constexpr wchar_t kClassName[] = L"SundialToast";
constexpr int kWidth = 480;
constexpr int kHeight = 96;
constexpr int kMarginPx = 20;

struct ToastData {
    std::wstring title;
    std::wstring body;
    std::wstring openOnClickPath;  // empty = non-clickable
    int durationMs;
};

LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            auto* d = static_cast<ToastData*>(cs->lpCreateParams);
            SetTimer(hwnd, 1, d->durationMs, nullptr);
            return 0;
        }
        case WM_TIMER:
            DestroyWindow(hwnd);
            return 0;
        case WM_LBUTTONUP: {
            auto* d = reinterpret_cast<ToastData*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (d && !d->openOnClickPath.empty()) {
                const std::wstring args =
                    L"/select,\"" + d->openOnClickPath + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe",
                              args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_SETCURSOR: {
            auto* d = reinterpret_cast<ToastData*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (d && !d->openOnClickPath.empty()) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_PAINT: {
            auto* d = reinterpret_cast<ToastData*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));

            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            HBRUSH bg = CreateSolidBrush(RGB(28, 28, 28));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            // Subtle 1px border so the toast reads as a panel.
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, 0, 0, rc.right, rc.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);

            SetBkMode(hdc, TRANSPARENT);

            HFONT titleFont = CreateFontW(
                -18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT bodyFont = CreateFontW(
                -14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            RECT titleRect{16, 10, rc.right - 16, 36};
            RECT bodyRect{16, 38, rc.right - 16, rc.bottom - 10};

            HGDIOBJ oldFont = SelectObject(hdc, titleFont);
            SetTextColor(hdc, RGB(245, 245, 245));
            DrawTextW(hdc, d->title.c_str(), -1, &titleRect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(hdc, bodyFont);
            SetTextColor(hdc, RGB(180, 180, 180));
            DrawTextW(hdc, d->body.c_str(), -1, &bodyRect,
                      DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS |
                          DT_EDITCONTROL | DT_WORDBREAK);

            SelectObject(hdc, oldFont);
            DeleteObject(titleFont);
            DeleteObject(bodyFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureClassRegistered() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ToastWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);
    });
}

DWORD WINAPI ToastThread(LPVOID param) {
    std::unique_ptr<ToastData> data(static_cast<ToastData*>(param));
    EnsureClassRegistered();

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.right - kWidth - kMarginPx;
    const int y = workArea.bottom - kHeight - kMarginPx;

    // WS_EX_TRANSPARENT would block clicks. Drop it when the toast is
    // interactive so we receive WM_LBUTTONUP; keep WS_EX_NOACTIVATE so the
    // foreground window doesn't lose focus when the toast appears.
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                    WS_EX_NOACTIVATE;
    if (data->openOnClickPath.empty()) {
        exStyle |= WS_EX_TRANSPARENT;
    }

    HWND hwnd = CreateWindowExW(
        exStyle, kClassName, nullptr, WS_POPUP,
        x, y, kWidth, kHeight,
        nullptr, nullptr, GetModuleHandleW(nullptr), data.get());

    if (!hwnd) return 1;

    SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace

void ShowToast(const std::wstring& title,
               const std::wstring& body,
               const std::wstring& openOnClickPath,
               int durationMs) {
    auto* data = new ToastData{title, body, openOnClickPath, durationMs};
    HANDLE h = CreateThread(nullptr, 0, ToastThread, data, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        delete data;
    }
}

}  // namespace sundial
