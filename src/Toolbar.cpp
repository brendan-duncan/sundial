#include "Toolbar.h"

#include <Windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>

#include "Editor.h"
#include "HdrCapture.h"
#include "ImageOps.h"
#include "Resource.h"
#include "VideoRecorder.h"

namespace sundial {
namespace {

constexpr wchar_t kClassName[] = L"SundialToolbar";
constexpr wchar_t kDimClassName[] = L"SundialToolbarDim";

constexpr int kBtnW = 110;
constexpr int kBtnH = 64;
constexpr int kPadding = 6;
constexpr int kButtonCount = 4;
constexpr int kNoteH = 24;  // hint strip drawn below the button row
constexpr int kToolbarW = kBtnW * kButtonCount + kPadding * (kButtonCount + 1);
constexpr int kToolbarH = kBtnH + kNoteH + kPadding * 3;
constexpr int kIconSize = 22;

// Mode-select toolbar buttons. Area capture is still triggered by dragging on
// the dim overlay; these four are the explicit buttons. Ids are contiguous so
// GetButtonRect's index arithmetic works.
constexpr int kIdFullScreen = 1001;
constexpr int kIdRecord = 1002;
constexpr int kIdEditImage = 1003;
constexpr int kIdSettings = 1004;
constexpr int kIdMenuEditOnCapture = 2001;
constexpr int kIdMenuSaveHdrJxr = 2002;
constexpr int kIdMenuAutoCopyCapture = 2003;
constexpr int kIdMenuOutputFolder = 2004;
constexpr int kIdMenuOutputFolderReset = 2005;

// Video-mode control bar geometry (the window is reused as a floating bar).
// The right region holds either the elapsed-time readout (while recording) or
// the "Adjust look" button (while adjusting), so it's sized to the wider of
// the two and the bar width stays fixed across phases.
constexpr int kCbPad = 10;
constexpr int kCbBtnW = 116;
constexpr int kCbBtnH = 40;
constexpr int kCbTimerW = 88;
constexpr int kCbAdjW = 132;
constexpr int kCbRightW = kCbAdjW > kCbTimerW ? kCbAdjW : kCbTimerW;
constexpr int kCbH = kCbBtnH + kCbPad * 2;
constexpr int kCbW = kCbPad * 3 + kCbBtnW + kCbRightW;
constexpr int kHintW = 360;
constexpr int kHintH = 44;

// Selection-handle hit-test results.
constexpr int kHitNone = -1;
constexpr int kHitMove = 8;
constexpr int kHitNew = 9;
constexpr int kHandleSize = 12;
constexpr int kMinSel = 24;  // minimum selection width/height in pixels

// Countdown / recording timers (on the control-bar window).
constexpr UINT_PTR kTimerCountdown = 1;
constexpr UINT_PTR kTimerRecord = 2;

constexpr COLORREF kRecordRed = RGB(232, 17, 35);

// Win10 2004+ display-affinity flag: keep a window on the physical monitor but
// exclude it from DWM-based capture (Desktop Duplication included). Defined
// locally so the build doesn't depend on the SDK header version.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Keep our overlay/control-bar chrome out of the recorded video. Harmless
// (and a silent no-op) on Windows builds that predate the flag.
void ExcludeFromCapture(HWND hwnd) {
    if (hwnd) SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
}

enum class VPhase {
    None,        // not in video mode
    PickRegion,  // video mode, no rectangle yet
    Adjust,      // rectangle exists, adjustable via handles
    Countdown,   // 3..1 countdown before recording
    Recording,   // actively recording
};

struct ToolbarState {
    AppSettings* settings = nullptr;
    ToolbarResult::Kind kind = ToolbarResult::Kind::None;
    RECT area{};
    int hoveredId = 0;
    HWND tipHwnd = nullptr;  // button tooltips (mode-select phase only)

    // Video mode.
    HWND dimHwnd = nullptr;
    int originX = 0;  // primary monitor top-left in screen coords
    int originY = 0;
    int monW = 0;
    int monH = 0;
    VPhase phase = VPhase::None;
    RECT sel{};            // selection rect in monitor-client coords
    bool hasRect = false;
    bool startHover = false;
    bool adjustHover = false;     // hover state for the "Adjust look" button
    bool tonemapAdjusted = false; // user dialed in the look via the editor
    int countdown = 0;
    ULONGLONG recordStartTick = 0;
    std::unique_ptr<VideoRecorder> recorder;
    std::wstring videoPath;
};

struct DimState {
    HWND toolbar = nullptr;  // for posting WM_CLOSE / repositioning
    ToolbarState* toolbarState = nullptr;
    bool dragging = false;
    bool dragMoved = false;  // true once the mouse has moved > threshold
    POINT anchor{};
    POINT current{};
    int activeHandle = kHitNone;  // which handle/area is being dragged
    RECT startRect{};             // selection at drag start (move/resize)
    int dimOriginX = 0;
    int dimOriginY = 0;
};

std::wstring Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond);
    return buf;
}

RECT NormalizeRect(POINT a, POINT b) {
    RECT r;
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    return r;
}

// ---- mode-select toolbar icons ------------------------------------------

void DrawFullScreenIcon(HDC hdc, int cx, int cy) {
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

void DrawRecordIcon(HDC hdc, int cx, int cy) {
    // Filled red disc - the universal "record" glyph.
    HBRUSH brush = CreateSolidBrush(kRecordRed);
    HPEN pen = CreatePen(PS_SOLID, 1, kRecordRed);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    const int r = kIconSize / 2 - 1;
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawEditIcon(HDC hdc, int cx, int cy) {
    HBRUSH brush = CreateSolidBrush(RGB(240, 240, 240));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(240, 240, 240));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);

    const int half = kIconSize / 2;
    const POINT bodyTop = {cx + half - 4, cy - half + 4};
    const POINT bodyBot = {cx - half + 4, cy + half - 4};
    const int p = 3;
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
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(240, 240, 240));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    const int r = kIconSize / 3;
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    const int ir = r / 2;
    Ellipse(hdc, cx - ir, cy - ir, cx + ir, cy + ir);
    const int t = 3;
    const int o = r + 2;
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
        case kIdRecord:     DrawRecordIcon(hdc, cx, iconCy);     break;
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
    textRc.top = rc.top + 36;
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
    for (int id : {kIdFullScreen, kIdRecord, kIdEditImage, kIdSettings}) {
        RECT r = GetButtonRect(id);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return id;
    }
    return 0;
}

// Hover tooltip per button. The toolbar is custom-painted (no real child
// controls), so we register each button's rect with a standard tooltip
// control. TTF_SUBCLASS lets it relay hover messages without us forwarding
// them. Torn down when we leave the mode-select phase (EnterVideoMode).
void CreateButtonTooltips(HWND owner, ToolbarState* s) {
    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, owner, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!tip) return;

    struct Tip { int id; const wchar_t* text; };
    const Tip tips[] = {
        {kIdFullScreen, L"Capture the entire screen"},
        {kIdRecord, L"Record a video of a screen region"},
        {kIdEditImage, L"Open an image file in the editor"},
        {kIdSettings, L"Capture and output options"},
    };
    for (const Tip& t : tips) {
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V1_SIZE;  // works on comctl32 v5 and v6
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = owner;
        ti.uId = static_cast<UINT_PTR>(t.id);
        ti.rect = GetButtonRect(t.id);
        ti.hinst = GetModuleHandleW(nullptr);
        ti.lpszText = const_cast<wchar_t*>(t.text);
        SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 320);
    s->tipHwnd = tip;
}

void DestroyButtonTooltips(ToolbarState* s) {
    if (s->tipHwnd) {
        DestroyWindow(s->tipHwnd);
        s->tipHwnd = nullptr;
    }
}

// Hint line below the buttons telling the user about drag-to-select area
// capture, which has no dedicated button.
void DrawToolbarNote(HDC hdc, const RECT& client) {
    HFONT font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(170, 170, 170));
    RECT noteRc = client;
    noteRc.top = client.bottom - kNoteH - kPadding;
    noteRc.bottom = client.bottom - kPadding;
    DrawTextW(hdc, L"Or drag anywhere on the screen to capture a custom area",
              -1, &noteRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// ---- selection handles ---------------------------------------------------

void GetHandleRects(const RECT& sel, RECT out[8]) {
    const int h = kHandleSize / 2;
    const int mx = (sel.left + sel.right) / 2;
    const int my = (sel.top + sel.bottom) / 2;
    const POINT pts[8] = {
        {sel.left, sel.top},  {mx, sel.top},     {sel.right, sel.top},
        {sel.right, my},      {sel.right, sel.bottom}, {mx, sel.bottom},
        {sel.left, sel.bottom}, {sel.left, my},
    };
    for (int i = 0; i < 8; ++i) {
        out[i] = {pts[i].x - h, pts[i].y - h, pts[i].x + h, pts[i].y + h};
    }
}

int HitTestSelection(const RECT& sel, POINT p) {
    RECT handles[8];
    GetHandleRects(sel, handles);
    for (int i = 0; i < 8; ++i) {
        if (PtInRect(&handles[i], p)) return i;
    }
    if (PtInRect(&sel, p)) return kHitMove;
    return kHitNone;
}

LPCWSTR CursorForHandle(int handle) {
    switch (handle) {
        case 0: case 4: return IDC_SIZENWSE;  // TL, BR
        case 2: case 6: return IDC_SIZENESW;  // TR, BL
        case 1: case 5: return IDC_SIZENS;    // T, B
        case 3: case 7: return IDC_SIZEWE;    // R, L
        case kHitMove:  return IDC_SIZEALL;
        default:        return IDC_CROSS;
    }
}

void ClampSel(RECT& sel, int monW, int monH) {
    sel.left = std::clamp<LONG>(sel.left, 0, monW);
    sel.top = std::clamp<LONG>(sel.top, 0, monH);
    sel.right = std::clamp<LONG>(sel.right, 0, monW);
    sel.bottom = std::clamp<LONG>(sel.bottom, 0, monH);
}

// Apply a resize for `handle` so the dragged edge(s) follow `p`, keeping a
// minimum size and staying within the monitor.
void ResizeSel(RECT& sel, int handle, POINT p, int monW, int monH) {
    p.x = std::clamp<LONG>(p.x, 0, monW);
    p.y = std::clamp<LONG>(p.y, 0, monH);
    const bool left = (handle == 0 || handle == 6 || handle == 7);
    const bool right = (handle == 2 || handle == 3 || handle == 4);
    const bool top = (handle == 0 || handle == 1 || handle == 2);
    const bool bottom = (handle == 4 || handle == 5 || handle == 6);
    if (left) sel.left = std::min<LONG>(p.x, sel.right - kMinSel);
    if (right) sel.right = std::max<LONG>(p.x, sel.left + kMinSel);
    if (top) sel.top = std::min<LONG>(p.y, sel.bottom - kMinSel);
    if (bottom) sel.bottom = std::max<LONG>(p.y, sel.top + kMinSel);
}

// ---- control bar (video mode) --------------------------------------------

void PositionControlBar(HWND bar, ToolbarState* s) {
    const int cx = (s->sel.left + s->sel.right) / 2;
    int x = std::clamp(cx - kCbW / 2, 0, std::max(0, s->monW - kCbW));
    int y = s->sel.bottom + 14;
    if (y + kCbH > s->monH) y = s->sel.top - 14 - kCbH;
    if (y < 0) y = std::clamp((s->monH - kCbH) / 2, 0, std::max(0, s->monH - kCbH));
    SetWindowPos(bar, HWND_TOPMOST, x + s->originX, y + s->originY, kCbW, kCbH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

RECT ControlBarButtonRect() {
    return {kCbPad, kCbPad, kCbPad + kCbBtnW, kCbPad + kCbBtnH};
}

// "Adjust look" button, in the right region (shown only while adjusting).
RECT ControlBarAdjustRect() {
    const int l = kCbPad * 2 + kCbBtnW;
    return {l, kCbPad, l + kCbAdjW, kCbPad + kCbBtnH};
}

void FormatElapsed(wchar_t* buf, size_t n, unsigned secs) {
    swprintf_s(buf, n, L"%02u:%02u", secs / 60, secs % 60);
}

void PaintControlBar(HWND hwnd, ToolbarState* s) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);

    HBRUSH bg = CreateSolidBrush(RGB(28, 28, 28));
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    const bool recording = (s->phase == VPhase::Recording);
    const bool active = (s->phase == VPhase::Recording ||
                         s->phase == VPhase::Countdown);
    const RECT btn = ControlBarButtonRect();

    HBRUSH btnBg = CreateSolidBrush(s->startHover ? RGB(58, 58, 58)
                                                  : RGB(44, 44, 44));
    FillRect(hdc, &btn, btnBg);
    DeleteObject(btnBg);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, btn.left, btn.top, btn.right, btn.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    // Glyph: red disc for Start, red square for Stop.
    const int gcx = btn.left + 20;
    const int gcy = (btn.top + btn.bottom) / 2;
    HBRUSH glyph = CreateSolidBrush(kRecordRed);
    HGDIOBJ ob = SelectObject(hdc, glyph);
    HPEN gp = CreatePen(PS_SOLID, 1, kRecordRed);
    HGDIOBJ op = SelectObject(hdc, gp);
    if (active) {
        Rectangle(hdc, gcx - 7, gcy - 7, gcx + 7, gcy + 7);
    } else {
        Ellipse(hdc, gcx - 8, gcy - 8, gcx + 8, gcy + 8);
    }
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(glyph);
    DeleteObject(gp);

    HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(240, 240, 240));
    RECT lblRc = {btn.left + 34, btn.top, btn.right, btn.bottom};
    DrawTextW(hdc, active ? L"Stop" : L"Start", -1, &lblRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (s->phase == VPhase::Adjust) {
        // "Adjust look" button: open the editor on a snapshot to tune the
        // HDR->SDR look before recording bakes it in.
        RECT adj = ControlBarAdjustRect();
        HBRUSH ab = CreateSolidBrush(s->adjustHover ? RGB(58, 58, 58)
                                                    : RGB(44, 44, 44));
        FillRect(hdc, &adj, ab);
        DeleteObject(ab);
        HPEN ap = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
        HGDIOBJ oap = SelectObject(hdc, ap);
        HGDIOBJ oab = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, adj.left, adj.top, adj.right, adj.bottom);
        SelectObject(hdc, oab);
        SelectObject(hdc, oap);
        DeleteObject(ap);
        SetTextColor(hdc, s->tonemapAdjusted ? RGB(140, 220, 140)
                                             : RGB(240, 240, 240));
        DrawTextW(hdc, s->tonemapAdjusted ? L"Look set" : L"Adjust look...",
                  -1, &adj,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        // Elapsed-time readout.
        unsigned secs = 0;
        if (recording && s->recordStartTick != 0) {
            secs = unsigned((GetTickCount64() - s->recordStartTick) / 1000);
        }
        wchar_t timeBuf[16];
        FormatElapsed(timeBuf, 16, secs);
        if (recording) SetTextColor(hdc, kRecordRed);
        RECT timeRc = {kCbPad * 2 + kCbBtnW, kCbPad, client.right - kCbPad,
                       client.bottom - kCbPad};
        DrawTextW(hdc, timeBuf, -1, &timeRc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, oldFont);
    DeleteObject(font);

    EndPaint(hwnd, &ps);
}

void PaintHint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    HBRUSH bg = CreateSolidBrush(RGB(28, 28, 28));
    FillRect(hdc, &client, bg);
    DeleteObject(bg);
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(235, 235, 235));
    DrawTextW(hdc, L"Drag to select the area to record  (Esc to cancel)", -1,
              &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
    EndPaint(hwnd, &ps);
}

// ---- recording control --------------------------------------------------

void SetDimRecordingMode(HWND dim) {
    // Make the overlay click-through and fully opaque outside the color key,
    // so only the recording border is drawn and clicks pass to the desktop.
    LONG_PTR ex = GetWindowLongPtrW(dim, GWL_EXSTYLE);
    ex |= WS_EX_TRANSPARENT;
    SetWindowLongPtrW(dim, GWL_EXSTYLE, ex);
    SetLayeredWindowAttributes(dim, RGB(255, 0, 255), 255, LWA_COLORKEY);
    SetWindowPos(dim, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    InvalidateRect(dim, nullptr, TRUE);
}

void BeginRecording(HWND bar, ToolbarState* s) {
    RECT region = s->sel;  // monitor-client coords == primary-monitor pixels
    s->videoPath = (ResolveOutputDir(*s->settings) /
                    (L"sundial_" + Timestamp()))
                       .wstring() +
                   L".mp4";

    if (s->dimHwnd) SetDimRecordingMode(s->dimHwnd);

    s->recorder = std::make_unique<VideoRecorder>();
    if (!s->recorder->Start(region, s->videoPath, *s->settings,
                            /*tonemapPreseeded=*/s->tonemapAdjusted)) {
        const std::string err = s->recorder->Error();
        MessageBoxA(bar, err.empty() ? "Could not start recording"
                                     : err.c_str(),
                    "Sundial recording", MB_ICONERROR);
        s->recorder.reset();
        PostMessageW(bar, WM_CLOSE, 0, 0);  // cancel
        return;
    }
    s->phase = VPhase::Recording;
    s->recordStartTick = GetTickCount64();
    SetTimer(bar, kTimerRecord, 250, nullptr);
    if (s->dimHwnd) InvalidateRect(s->dimHwnd, nullptr, FALSE);
    InvalidateRect(bar, nullptr, FALSE);
}

void StopAndFinish(HWND bar, ToolbarState* s) {
    KillTimer(bar, kTimerRecord);
    bool ok = false;
    if (s->recorder) {
        ok = s->recorder->Stop();
        if (!ok) {
            const std::string err = s->recorder->Error();
            MessageBoxA(bar, err.empty() ? "Recording failed" : err.c_str(),
                        "Sundial recording", MB_ICONERROR);
        }
    }
    if (ok) {
        s->kind = ToolbarResult::Kind::VideoRecorded;
    }
    PostMessageW(bar, WM_CLOSE, 0, 0);
}

// "Adjust look" pressed: snapshot the selected region and open the file-less
// look-picker editor so the user can dial in the HDR->SDR conversion before
// recording bakes it in. The chosen params are stored back into the live
// settings; BeginRecording then hands them to the recorder verbatim.
void OnAdjustLook(HWND bar, ToolbarState* s) {
    if (s->phase != VPhase::Adjust) return;

    const RECT sel = s->sel;  // monitor-client coords == primary-monitor px
    const uint32_t w = uint32_t(std::max<LONG>(1, sel.right - sel.left));
    const uint32_t h = uint32_t(std::max<LONG>(1, sel.bottom - sel.top));

    // Capture a snapshot first: our overlay + control bar are excluded from
    // capture, so they don't show up even though they're still visible here.
    Frame snap;
    try {
        snap = Crop(CaptureFullScreen(),
                    uint32_t(std::max<LONG>(0, sel.left)),
                    uint32_t(std::max<LONG>(0, sel.top)), w, h);
    } catch (const std::exception& e) {
        MessageBoxA(bar, e.what(), "Sundial - couldn't snapshot the region",
                    MB_ICONERROR);
        return;
    }

    // Seed the tonemap as a screenshot of this region would, then let the user
    // tune it. On a re-open we keep the look the user already dialed in rather
    // than reseeding the per-display anchors over their tweaks.
    AppSettings tmp = *s->settings;
    if (!s->tonemapAdjusted) SeedTonemapForFrame(tmp.tonemap, snap);

    // Hide our topmost chrome so the editor isn't stuck behind it.
    if (s->dimHwnd) ShowWindow(s->dimHwnd, SW_HIDE);
    ShowWindow(bar, SW_HIDE);

    EditorResult r = RunEditor(snap, tmp, L"", /*tonemapOnly=*/true);
    if (r.saved) {
        s->settings->tonemap = r.updatedSettings.tonemap;
        s->tonemapAdjusted = true;
    }

    // Restore the overlay + control bar and reassert their topmost z-order.
    if (s->dimHwnd) {
        ShowWindow(s->dimHwnd, SW_SHOW);
        SetWindowPos(s->dimHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ShowWindow(bar, SW_SHOW);
    SetWindowPos(bar, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(bar);
    s->adjustHover = false;
    if (s->dimHwnd) InvalidateRect(s->dimHwnd, nullptr, FALSE);
    InvalidateRect(bar, nullptr, FALSE);
}

// Start/Stop button pressed in the control bar.
void OnControlButton(HWND bar, ToolbarState* s) {
    switch (s->phase) {
        case VPhase::Adjust:
            s->phase = VPhase::Countdown;
            s->countdown = 3;
            SetTimer(bar, kTimerCountdown, 1000, nullptr);
            if (s->dimHwnd) InvalidateRect(s->dimHwnd, nullptr, FALSE);
            InvalidateRect(bar, nullptr, FALSE);
            break;
        case VPhase::Countdown:
            // Cancel the countdown, back to adjusting.
            KillTimer(bar, kTimerCountdown);
            s->phase = VPhase::Adjust;
            s->countdown = 0;
            if (s->dimHwnd) InvalidateRect(s->dimHwnd, nullptr, FALSE);
            InvalidateRect(bar, nullptr, FALSE);
            break;
        case VPhase::Recording:
            StopAndFinish(bar, s);
            break;
        default:
            break;
    }
}

// Switch the mode-select toolbar window into video mode (hint bar at top).
void EnterVideoMode(HWND hwnd, ToolbarState* s) {
    // The button rects no longer apply once the window becomes the hint /
    // control bar, so drop the per-button tooltips.
    DestroyButtonTooltips(s);
    s->phase = VPhase::PickRegion;
    s->hasRect = false;
    const int x = s->originX + (s->monW - kHintW) / 2;
    const int y = s->originY + 24;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, kHintW, kHintH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd, nullptr, TRUE);
    if (s->dimHwnd) InvalidateRect(s->dimHwnd, nullptr, FALSE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

// Called when the selection first becomes valid: show the control bar.
void ShowControlBarForSelection(HWND bar, ToolbarState* s) {
    s->phase = VPhase::Adjust;
    s->hasRect = true;
    PositionControlBar(bar, s);
    InvalidateRect(bar, nullptr, TRUE);
    SetForegroundWindow(bar);
    SetFocus(bar);
}

// ---- folder picker + settings menu (unchanged) ---------------------------

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

// ---- toolbar / control-bar window proc -----------------------------------

LRESULT CALLBACK ToolbarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<ToolbarState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            CreateButtonTooltips(
                hwnd, reinterpret_cast<ToolbarState*>(cs->lpCreateParams));
            return 0;
        }
        case WM_PAINT: {
            if (s && s->phase != VPhase::None) {
                if (s->hasRect) {
                    PaintControlBar(hwnd, s);
                } else {
                    PaintHint(hwnd);
                }
                return 0;
            }
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            HBRUSH bg = CreateSolidBrush(RGB(28, 28, 28));
            FillRect(hdc, &client, bg);
            DeleteObject(bg);
            DrawButton(hdc, GetButtonRect(kIdFullScreen), kIdFullScreen,
                       L"Full Screen", s && s->hoveredId == kIdFullScreen);
            DrawButton(hdc, GetButtonRect(kIdRecord), kIdRecord, L"Record",
                       s && s->hoveredId == kIdRecord);
            DrawButton(hdc, GetButtonRect(kIdEditImage), kIdEditImage,
                       L"Edit Image...", s && s->hoveredId == kIdEditImage);
            DrawButton(hdc, GetButtonRect(kIdSettings), kIdSettings,
                       L"Settings", s && s->hoveredId == kIdSettings);
            DrawToolbarNote(hdc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            if (!s) return 0;
            if (wp == kTimerCountdown) {
                s->countdown--;
                if (s->countdown > 0) {
                    if (s->dimHwnd) InvalidateRect(s->dimHwnd, nullptr, FALSE);
                } else {
                    KillTimer(hwnd, kTimerCountdown);
                    BeginRecording(hwnd, s);
                }
            } else if (wp == kTimerRecord) {
                InvalidateRect(hwnd, nullptr, FALSE);  // refresh elapsed time
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!s) return 0;
            if (s->phase != VPhase::None && s->hasRect) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                RECT btn = ControlBarButtonRect();
                bool h = PtInRect(&btn, pt);
                RECT adj = ControlBarAdjustRect();
                bool ah = s->phase == VPhase::Adjust && PtInRect(&adj, pt);
                if (h != s->startHover || ah != s->adjustHover) {
                    s->startHover = h;
                    s->adjustHover = ah;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&tme);
                }
                return 0;
            }
            if (s->phase == VPhase::None) {
                int newHover = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (newHover != s->hoveredId) {
                    s->hoveredId = newHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&tme);
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (s) {
                s->hoveredId = 0;
                s->startHover = false;
                s->adjustHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (!s) return 0;
            if (s->phase != VPhase::None) {
                if (s->hasRect) {
                    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    RECT btn = ControlBarButtonRect();
                    RECT adj = ControlBarAdjustRect();
                    if (PtInRect(&btn, pt)) {
                        OnControlButton(hwnd, s);
                    } else if (s->phase == VPhase::Adjust &&
                               PtInRect(&adj, pt)) {
                        OnAdjustLook(hwnd, s);
                    }
                }
                return 0;
            }
            int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            switch (id) {
                case kIdFullScreen:
                    s->kind = ToolbarResult::Kind::FullScreen;
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    break;
                case kIdRecord:
                    EnterVideoMode(hwnd, s);
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
            if (wp == VK_ESCAPE && s) {
                if (s->phase == VPhase::Recording) {
                    StopAndFinish(hwnd, s);  // Esc stops & saves
                } else if (s->phase == VPhase::Countdown) {
                    OnControlButton(hwnd, s);  // cancel countdown
                } else {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (s) DestroyButtonTooltips(s);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- dim overlay ----------------------------------------------------------

void DrawSelectionChrome(HDC mem, const RECT& sel, bool handles) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(mem, pen);
    HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, sel.left, sel.top, sel.right, sel.bottom);
    SelectObject(mem, oldBrush);
    SelectObject(mem, oldPen);
    DeleteObject(pen);

    if (handles) {
        RECT h[8];
        GetHandleRects(sel, h);
        HBRUSH fill = CreateSolidBrush(RGB(255, 255, 255));
        HPEN border = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
        HGDIOBJ ob = SelectObject(mem, fill);
        HGDIOBJ op = SelectObject(mem, border);
        for (int i = 0; i < 8; ++i) {
            Rectangle(mem, h[i].left, h[i].top, h[i].right, h[i].bottom);
        }
        SelectObject(mem, ob);
        SelectObject(mem, op);
        DeleteObject(fill);
        DeleteObject(border);
    }
}

void DrawSizeLabel(HDC mem, const RECT& sel) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d x %d", sel.right - sel.left, sel.bottom - sel.top);
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(mem, font);
    SetBkMode(mem, OPAQUE);
    SetBkColor(mem, RGB(28, 28, 28));
    SetTextColor(mem, RGB(245, 245, 245));
    RECT tr{sel.left + 4, sel.bottom + 4, sel.left + 120, sel.bottom + 22};
    DrawTextW(mem, buf, -1, &tr, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(mem, oldFont);
    DeleteObject(font);
}

void DrawCountdown(HDC mem, const RECT& sel, int n) {
    const int h = std::clamp(
        int(std::min(sel.right - sel.left, sel.bottom - sel.top)) / 2, 48, 240);
    HFONT font = CreateFontW(-h, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);
    wchar_t buf[8];
    swprintf_s(buf, L"%d", n);
    RECT r = sel;
    // Drop shadow then the number, for legibility over arbitrary content.
    SetTextColor(mem, RGB(0, 0, 0));
    RECT sh = {r.left + 2, r.top + 2, r.right + 2, r.bottom + 2};
    DrawTextW(mem, buf, -1, &sh,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(mem, RGB(255, 255, 255));
    DrawTextW(mem, buf, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mem, oldFont);
    DeleteObject(font);
}

void PaintDim(HWND hwnd, const DimState& d) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    ToolbarState* s = d.toolbarState;
    const bool recording = s && s->phase == VPhase::Recording;

    if (recording) {
        // Everything transparent; draw only a record border just OUTSIDE the
        // region so our chrome is never inside the captured rectangle.
        HBRUSH clear = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(mem, &client, clear);
        DeleteObject(clear);
        RECT b = s->sel;
        InflateRect(&b, 4, 4);
        HPEN pen = CreatePen(PS_SOLID, 3, kRecordRed);
        HGDIOBJ oldPen = SelectObject(mem, pen);
        HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, b.left, b.top, b.right, b.bottom);
        SelectObject(mem, oldBrush);
        SelectObject(mem, oldPen);
        DeleteObject(pen);
        BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return;
    }

    // Dim background (alpha applied by the layered window).
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem, &client, bg);
    DeleteObject(bg);

    // Determine the selection rect to show, and whether handles/countdown.
    RECT sel{};
    bool haveSel = false;
    bool handles = false;
    if (s && s->phase != VPhase::None) {
        if (s->phase == VPhase::PickRegion) {
            if (d.dragging && d.dragMoved) {
                sel = s->sel;
                haveSel = true;
            }
        } else {
            sel = s->sel;
            haveSel = true;
            handles = (s->phase == VPhase::Adjust);
        }
    } else if (d.dragging && d.dragMoved) {
        // Screenshot area selection.
        sel = NormalizeRect(d.anchor, d.current);
        haveSel = true;
    }

    if (haveSel) {
        HBRUSH clear = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(mem, &sel, clear);
        DeleteObject(clear);
        DrawSelectionChrome(mem, sel, handles);
        if (s && s->phase == VPhase::Countdown) {
            DrawCountdown(mem, sel, s->countdown);
        } else {
            DrawSizeLabel(mem, sel);
        }
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
    ToolbarState* s = d ? d->toolbarState : nullptr;
    const bool video = s && s->phase != VPhase::None;

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR: {
            if (video && s->phase == VPhase::Adjust && !d->dragging) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                SetCursor(LoadCursorW(nullptr,
                                      CursorForHandle(
                                          HitTestSelection(s->sel, p))));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        case WM_LBUTTONDOWN: {
            if (!d) return 0;
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            d->dragging = true;
            d->dragMoved = false;
            d->anchor = p;
            d->current = p;
            SetCapture(hwnd);
            if (video && s->phase == VPhase::Adjust) {
                int hit = HitTestSelection(s->sel, p);
                d->activeHandle = hit;
                d->startRect = s->sel;
                if (hit == kHitNone) {
                    d->activeHandle = kHitNew;  // re-select a new rect
                }
            } else if (video && s->phase == VPhase::PickRegion) {
                d->activeHandle = kHitNew;
            } else {
                d->activeHandle = kHitNew;  // screenshot area
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!d || !d->dragging) return 0;
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            d->current = p;
            if (!d->dragMoved) {
                const int dx = p.x - d->anchor.x;
                const int dy = p.y - d->anchor.y;
                if (dx * dx + dy * dy >= 16) d->dragMoved = true;
            }
            if (video) {
                if (d->activeHandle == kHitMove) {
                    const int dx = p.x - d->anchor.x;
                    const int dy = p.y - d->anchor.y;
                    RECT r = d->startRect;
                    OffsetRect(&r, dx, dy);
                    // Keep fully on-screen while moving.
                    if (r.left < 0) OffsetRect(&r, -r.left, 0);
                    if (r.top < 0) OffsetRect(&r, 0, -r.top);
                    if (r.right > s->monW) OffsetRect(&r, s->monW - r.right, 0);
                    if (r.bottom > s->monH)
                        OffsetRect(&r, 0, s->monH - r.bottom);
                    s->sel = r;
                } else if (d->activeHandle >= 0 && d->activeHandle < 8) {
                    s->sel = d->startRect;
                    ResizeSel(s->sel, d->activeHandle, p, s->monW, s->monH);
                } else {  // kHitNew
                    RECT r = NormalizeRect(d->anchor, p);
                    ClampSel(r, s->monW, s->monH);
                    s->sel = r;
                    if (s->phase == VPhase::PickRegion && d->dragMoved) {
                        // live preview only; committed on button up
                    }
                }
                if (d->toolbar && s->hasRect) PositionControlBar(d->toolbar, s);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (d->dragMoved) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!d || !d->dragging) return 0;
            d->dragging = false;
            ReleaseCapture();

            if (video) {
                if (s->phase == VPhase::PickRegion) {
                    RECT r = NormalizeRect(d->anchor, d->current);
                    ClampSel(r, s->monW, s->monH);
                    if (d->dragMoved && (r.right - r.left) >= kMinSel &&
                        (r.bottom - r.top) >= kMinSel) {
                        s->sel = r;
                        ShowControlBarForSelection(d->toolbar, s);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else {
                        // trivial click in pick mode => cancel everything
                        if (d->toolbar) PostMessageW(d->toolbar, WM_CLOSE, 0, 0);
                    }
                } else if (s->phase == VPhase::Adjust) {
                    // Re-select that ended up too small: revert to prior rect.
                    if (d->activeHandle == kHitNew &&
                        ((s->sel.right - s->sel.left) < kMinSel ||
                         (s->sel.bottom - s->sel.top) < kMinSel)) {
                        s->sel = d->startRect;
                    }
                    if (d->toolbar) PositionControlBar(d->toolbar, s);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                d->activeHandle = kHitNone;
                return 0;
            }

            // Screenshot area selection.
            if (d->dragMoved && d->toolbarState) {
                RECT r = NormalizeRect(d->anchor, d->current);
                if (r.right - r.left >= 2 && r.bottom - r.top >= 2) {
                    d->toolbarState->kind = ToolbarResult::Kind::Area;
                    d->toolbarState->area = r;
                }
            }
            if (d->toolbar) PostMessageW(d->toolbar, WM_CLOSE, 0, 0);
            return 0;
        }
        case WM_RBUTTONUP:
            // Right-click cancels (in adjust it also just cancels the flow).
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
    wc.hbrBackground = nullptr;
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
        kDimClassName, nullptr, WS_POPUP, x, y, w, h, nullptr, nullptr,
        GetModuleHandleW(nullptr), state);
    if (!dim) return nullptr;
    SetLayeredWindowAttributes(dim, RGB(255, 0, 255), 110,
                               LWA_COLORKEY | LWA_ALPHA);
    // The recording border lives on this window; never let it into the video.
    ExcludeFromCapture(dim);
    ShowWindow(dim, SW_SHOWNA);
    return dim;
}

void EnsureClassRegistered() {
    static bool registered = false;
    if (registered) return;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);  // registers the tooltip window class
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
    state.originX = mi.rcMonitor.left;
    state.originY = mi.rcMonitor.top;
    state.monW = mi.rcMonitor.right - mi.rcMonitor.left;
    state.monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    DimState dimState{};
    dimState.toolbarState = &state;
    HWND dim = CreateDimOverlay(&dimState);
    state.dimHwnd = dim;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"Sundial",
        WS_POPUP | WS_BORDER, x, y, kToolbarW, kToolbarH, nullptr, nullptr,
        GetModuleHandleW(nullptr), &state);
    if (!hwnd) {
        if (dim) DestroyWindow(dim);
        return ToolbarResult{};
    }
    // The Start/Stop control bar can sit over the selection (e.g. a tall
    // region with no room above or below); exclude it from capture so it's
    // never recorded regardless of where it lands.
    ExcludeFromCapture(hwnd);

    if (dim) {
        dimState.toolbar = hwnd;
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

    // Ensure any in-flight recording is stopped before we tear down.
    if (state.recorder) state.recorder->Stop();
    if (dim) DestroyWindow(dim);

    ToolbarResult result;
    result.kind = state.kind;
    if (state.kind == ToolbarResult::Kind::Area) {
        result.area = state.area;
    } else if (state.kind == ToolbarResult::Kind::VideoRecorded) {
        result.videoPath = state.videoPath;
    }
    return result;
}

}  // namespace sundial
