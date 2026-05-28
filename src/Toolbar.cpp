#include "Toolbar.h"

#include <Windows.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>

#include "Resource.h"

namespace sundial {
namespace {

constexpr wchar_t kClassName[] = L"SundialToolbar";
constexpr wchar_t kDimClassName[] = L"SundialToolbarDim";

constexpr int kBtnW = 110;
constexpr int kBtnH = 64;
constexpr int kPadding = 6;
constexpr int kButtonCount = 3;
constexpr int kToolbarW = kBtnW * kButtonCount + kPadding * (kButtonCount + 1);
constexpr int kToolbarH = kBtnH + kPadding * 2;
constexpr int kIconSize = 22;

// Area capture is triggered by dragging on the dim overlay - no toolbar
// button needed for it. The remaining three buttons are renumbered to stay
// contiguous so GetButtonRect's id-arithmetic indexing still works.
constexpr int kIdFullScreen = 1001;
constexpr int kIdEditImage = 1002;
constexpr int kIdSettings = 1003;
constexpr int kIdMenuEditOnCapture = 2001;
constexpr int kIdMenuSaveHdrJxr = 2002;
constexpr int kIdMenuAutoCopyCapture = 2003;
constexpr int kIdMenuOutputFolder = 2004;
constexpr int kIdMenuOutputFolderReset = 2005;

struct ToolbarState {
    AppSettings* settings = nullptr;
    ToolbarResult::Kind kind = ToolbarResult::Kind::None;
    RECT area{};
    int hoveredId = 0;
};

struct DimState {
    HWND toolbar = nullptr;        // for posting WM_CLOSE
    ToolbarState* toolbarState = nullptr;
    bool dragging = false;
    bool dragMoved = false;        // true once the mouse has moved > threshold
    POINT anchor{};
    POINT current{};
    int dimOriginX = 0;            // dim window's top-left in screen coords
    int dimOriginY = 0;
};

void DrawFullScreenIcon(HDC hdc, int cx, int cy) {
    // Four corner brackets.
    const int half = kIconSize / 2;
    const int b = kIconSize / 3;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(240, 240, 240));
    HGDIOBJ old = SelectObject(hdc, pen);
    const int l = cx - half, t = cy - half;
    const int r = cx + half, btm = cy + half;
    MoveToEx(hdc, l, t + b, nullptr); LineTo(hdc, l, t); LineTo(hdc, l + b, t);
    MoveToEx(hdc, r - b, t, nullptr); LineTo(hdc, r, t); LineTo(hdc, r, t + b);
    MoveToEx(hdc, l, btm - b, nullptr); LineTo(hdc, l, btm); LineTo(hdc, l + b, btm);
    MoveToEx(hdc, r - b, btm, nullptr); LineTo(hdc, r, btm); LineTo(hdc, r, btm - b);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

void DrawEditIcon(HDC hdc, int cx, int cy) {
    // Pencil at ~45 degrees: parallelogram body + triangular tip pointing
    // toward the bottom-left.
    HBRUSH brush = CreateSolidBrush(RGB(240, 240, 240));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(240, 240, 240));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);

    const int half = kIconSize / 2;
    const POINT bodyTop = {cx + half - 4, cy - half + 4};
    const POINT bodyBot = {cx - half + 4, cy + half - 4};
    const int p = 3;  // half-thickness perpendicular to the 45-degree axis
    POINT body[4] = {
        {bodyTop.x - p, bodyTop.y - p},
        {bodyTop.x + p, bodyTop.y + p},
        {bodyBot.x + p, bodyBot.y + p},
        {bodyBot.x - p, bodyBot.y - p},
    };
    Polygon(hdc, body, 4);

    POINT tip[3] = {
        {bodyBot.x - p, bodyBot.y - p},
        {bodyBot.x + p, bodyBot.y + p},
        {cx - half, cy + half},
    };
    Polygon(hdc, tip, 3);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawSettingsIcon(HDC hdc, int cx, int cy) {
    // Simple gear: outer ring with four teeth, inner hole.
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(240, 240, 240));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    const int r = kIconSize / 3;
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    const int ir = r / 2;
    Ellipse(hdc, cx - ir, cy - ir, cx + ir, cy + ir);
    const int t = 3;          // tooth half-width
    const int o = r + 2;      // tooth distance from centre
    Rectangle(hdc, cx - t, cy - o - 1, cx + t, cy - r + 1);
    Rectangle(hdc, cx - t, cy + r - 1, cx + t, cy + o + 1);
    Rectangle(hdc, cx - o - 1, cy - t, cx - r + 1, cy + t);
    Rectangle(hdc, cx + r - 1, cy - t, cx + o + 1, cy + t);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void DrawButton(HDC hdc, const RECT& rc, int id, const wchar_t* label,
                bool hovered) {
    HBRUSH bg = CreateSolidBrush(hovered ? RGB(58, 58, 58) : RGB(40, 40, 40));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    const int cx = (rc.left + rc.right) / 2;
    const int iconCy = rc.top + 18;
    switch (id) {
        case kIdFullScreen: DrawFullScreenIcon(hdc, cx, iconCy); break;
        case kIdEditImage:  DrawEditIcon(hdc, cx, iconCy);       break;
        case kIdSettings:   DrawSettingsIcon(hdc, cx, iconCy);   break;
    }

    HFONT font = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(240, 240, 240));
    RECT textRc = rc;
    textRc.top = rc.top + 36;  // below the icon
    DrawTextW(hdc, label, -1, &textRc,
              DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

RECT GetButtonRect(int id) {
    int index = id - kIdFullScreen;
    RECT r;
    r.top = kPadding;
    r.bottom = r.top + kBtnH;
    r.left = kPadding + index * (kBtnW + kPadding);
    r.right = r.left + kBtnW;
    return r;
}

int HitTest(int x, int y) {
    for (int id : {kIdFullScreen, kIdEditImage, kIdSettings}) {
        RECT r = GetButtonRect(id);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            return id;
        }
    }
    return 0;
}

// Modal "pick a folder" dialog via IFileOpenDialog (FOS_PICKFOLDERS). Returns
// the empty string if the user cancels. `seedFolder` pre-selects a starting
// directory; pass empty to let the shell pick its default.
std::wstring PickFolder(HWND owner, const std::wstring& seedFolder) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dlg)))) {
        return {};
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST |
                    FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    dlg->SetTitle(L"Choose output folder");

    if (!seedFolder.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(seedFolder.c_str(), nullptr,
                                                  IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }

    std::wstring chosen;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dlg->GetResult(&result))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                chosen = p;
                CoTaskMemFree(p);
            }
            result->Release();
        }
    }
    dlg->Release();
    return chosen;
}

void ShowSettingsMenu(HWND hwnd, ToolbarState* s) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu,
                MF_STRING |
                    (s->settings->editOnCapture ? MF_CHECKED : MF_UNCHECKED),
                kIdMenuEditOnCapture, L"Edit on Capture");
    AppendMenuW(menu,
                MF_STRING |
                    (s->settings->saveHdrJxr ? MF_CHECKED : MF_UNCHECKED),
                kIdMenuSaveHdrJxr, L"Save HDR Image (JXR)");
    AppendMenuW(menu,
                MF_STRING |
                    (s->settings->autoCopyCapture ? MF_CHECKED : MF_UNCHECKED),
                kIdMenuAutoCopyCapture, L"Auto Copy Capture");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdMenuOutputFolder, L"Output Folder...");
    // "Reset to default" stays disabled when already on the default so the
    // menu reads as "this is currently the default" without an extra label.
    AppendMenuW(menu,
                MF_STRING |
                    (s->settings->outputFolder.empty() ? MF_GRAYED : 0),
                kIdMenuOutputFolderReset,
                L"Reset Output Folder to Default");

    RECT btn = GetButtonRect(kIdSettings);
    POINT pt{btn.left, btn.bottom};
    ClientToScreen(hwnd, &pt);

    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN |
                                        TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == kIdMenuEditOnCapture) {
        s->settings->editOnCapture = !s->settings->editOnCapture;
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (cmd == kIdMenuSaveHdrJxr) {
        s->settings->saveHdrJxr = !s->settings->saveHdrJxr;
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (cmd == kIdMenuAutoCopyCapture) {
        s->settings->autoCopyCapture = !s->settings->autoCopyCapture;
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (cmd == kIdMenuOutputFolder) {
        const std::wstring seed = s->settings->outputFolder.empty()
                                      ? DefaultOutputDir().wstring()
                                      : s->settings->outputFolder;
        std::wstring picked = PickFolder(hwnd, seed);
        if (!picked.empty()) {
            // Treat picking the default location as "reset to default" so the
            // setting stays portable across users (the field is stored as a
            // literal path, not a token).
            if (picked == DefaultOutputDir().wstring()) {
                s->settings->outputFolder.clear();
            } else {
                s->settings->outputFolder = picked;
            }
        }
    } else if (cmd == kIdMenuOutputFolderReset) {
        s->settings->outputFolder.clear();
    }
}

LRESULT CALLBACK ToolbarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<ToolbarState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);

            HBRUSH bg = CreateSolidBrush(RGB(28, 28, 28));
            FillRect(hdc, &client, bg);
            DeleteObject(bg);

            DrawButton(hdc, GetButtonRect(kIdFullScreen), kIdFullScreen,
                       L"Full Screen",
                       s && s->hoveredId == kIdFullScreen);
            DrawButton(hdc, GetButtonRect(kIdEditImage), kIdEditImage,
                       L"Edit Image...",
                       s && s->hoveredId == kIdEditImage);
            DrawButton(hdc, GetButtonRect(kIdSettings), kIdSettings,
                       L"Settings",
                       s && s->hoveredId == kIdSettings);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int newHover = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (s && newHover != s->hoveredId) {
                s->hoveredId = newHover;
                InvalidateRect(hwnd, nullptr, FALSE);

                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (s) {
                s->hoveredId = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (!s) return 0;
            int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            switch (id) {
                case kIdFullScreen:
                    s->kind = ToolbarResult::Kind::FullScreen;
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    break;
                case kIdEditImage:
                    s->kind = ToolbarResult::Kind::EditImage;
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    break;
                case kIdSettings:
                    ShowSettingsMenu(hwnd, s);
                    break;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Dim-overlay window: dims the primary monitor and also serves as the
// area-selection surface. Cross cursor on hover; drag to define a
// rectangle; click without dragging cancels everything. The selected
// rectangle is punched through to fully transparent via LWA_COLORKEY so
// the user sees the live desktop inside.
RECT NormalizeRect(POINT a, POINT b) {
    RECT r;
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    return r;
}

void PaintDim(HWND hwnd, const DimState& d) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT client;
    GetClientRect(hwnd, &client);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // Background = black (will be dim-alpha'd by the layered window).
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem, &client, bg);
    DeleteObject(bg);

    if (d.dragging && d.dragMoved) {
        RECT sel = NormalizeRect(d.anchor, d.current);

        // Punch out: the colour-key colour becomes fully transparent.
        HBRUSH clear = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(mem, &sel, clear);
        DeleteObject(clear);

        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(mem, pen);
        HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, sel.left, sel.top, sel.right, sel.bottom);
        SelectObject(mem, oldBrush);
        SelectObject(mem, oldPen);
        DeleteObject(pen);

        wchar_t buf[32];
        swprintf_s(buf, L"%d x %d", sel.right - sel.left,
                   sel.bottom - sel.top);
        HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(mem, font);
        SetBkMode(mem, OPAQUE);
        SetBkColor(mem, RGB(28, 28, 28));
        SetTextColor(mem, RGB(245, 245, 245));
        RECT tr{sel.left + 4, sel.bottom + 4, sel.left + 120,
                sel.bottom + 22};
        DrawTextW(mem, buf, -1, &tr, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE);
        SelectObject(mem, oldFont);
        DeleteObject(font);
    }

    BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK DimWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<DimState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_LBUTTONDOWN:
            if (!d) return 0;
            d->dragging = true;
            d->dragMoved = false;
            d->anchor.x = GET_X_LPARAM(lp);
            d->anchor.y = GET_Y_LPARAM(lp);
            d->current = d->anchor;
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (!d || !d->dragging) return 0;
            d->current.x = GET_X_LPARAM(lp);
            d->current.y = GET_Y_LPARAM(lp);
            if (!d->dragMoved) {
                const int dx = d->current.x - d->anchor.x;
                const int dy = d->current.y - d->anchor.y;
                if (dx * dx + dy * dy >= 16) {
                    d->dragMoved = true;
                }
            }
            if (d->dragMoved) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            if (!d || !d->dragging) return 0;
            d->dragging = false;
            ReleaseCapture();
            if (d->dragMoved && d->toolbarState) {
                RECT r = NormalizeRect(d->anchor, d->current);
                if (r.right - r.left >= 2 && r.bottom - r.top >= 2) {
                    d->toolbarState->kind = ToolbarResult::Kind::Area;
                    d->toolbarState->area = r;
                }
            }
            // Click-without-drag or trivial drag => cancel (kind stays None).
            if (d->toolbar) PostMessageW(d->toolbar, WM_CLOSE, 0, 0);
            return 0;
        }
        case WM_RBUTTONUP:
            if (d && d->toolbar) PostMessageW(d->toolbar, WM_CLOSE, 0, 0);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && d && d->toolbar) {
                PostMessageW(d->toolbar, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_PAINT:
            if (d) PaintDim(hwnd, *d);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureDimClassRegistered() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DimWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kDimClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.hbrBackground = nullptr;  // we paint ourselves
    RegisterClassExW(&wc);
    registered = true;
}

HWND CreateDimOverlay(DimState* state) {
    EnsureDimClassRegistered();
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    const int x = mi.rcMonitor.left;
    const int y = mi.rcMonitor.top;
    const int w = mi.rcMonitor.right - mi.rcMonitor.left;
    const int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    state->dimOriginX = x;
    state->dimOriginY = y;
    HWND dim = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kDimClassName, nullptr, WS_POPUP,
        x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), state);
    if (!dim) return nullptr;
    // Magenta = fully transparent (the selection-rect punch-out);
    // everything else gets ~43% dim alpha.
    SetLayeredWindowAttributes(dim, RGB(255, 0, 255), 110,
                               LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(dim, SW_SHOWNA);
    return dim;
}

void EnsureClassRegistered() {
    static bool registered = false;
    if (registered) return;
    HICON appIcon = LoadIconW(GetModuleHandleW(nullptr),
                              MAKEINTRESOURCEW(IDI_APP_ICON));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToolbarWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = appIcon;
    wc.hIconSm = appIcon;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

ToolbarResult ShowToolbar(AppSettings& settings) {
    EnsureClassRegistered();

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    const int workW = mi.rcWork.right - mi.rcWork.left;
    const int x = mi.rcWork.left + (workW - kToolbarW) / 2;
    const int y = mi.rcWork.top + 24;

    ToolbarState state{};
    state.settings = &settings;

    DimState dimState{};
    dimState.toolbarState = &state;
    HWND dim = CreateDimOverlay(&dimState);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kClassName, L"Sundial",
        WS_POPUP | WS_BORDER,
        x, y, kToolbarW, kToolbarH,
        nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!hwnd) {
        if (dim) DestroyWindow(dim);
        return ToolbarResult{};
    }

    if (dim) {
        dimState.toolbar = hwnd;
        // Re-raise dim, then re-raise toolbar so it lands on top.
        SetWindowPos(dim, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (dim) DestroyWindow(dim);

    ToolbarResult result;
    result.kind = state.kind;
    if (state.kind == ToolbarResult::Kind::Area) {
        // state.area is in dim-window client coords, which equal primary
        // monitor coords because the dim is positioned at the monitor's
        // top-left.
        result.area = state.area;
    }
    return result;
}

}  // namespace sundial
