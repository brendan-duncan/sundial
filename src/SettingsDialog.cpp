#include "SettingsDialog.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <string>

#include "About.h"
#include "Resource.h"
#include "ShellIntegration.h"
#include "TrayIcon.h"
#include "Updater.h"

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT,
                                                             WPARAM, LPARAM);

namespace sundial {
namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

// Map an ImGui key to the Win32 virtual-key code RegisterHotKey wants. Returns
// 0 for keys we don't expose as hotkeys (modifiers, mouse, unmapped), so the
// capture loop simply ignores them. Mirrors Settings.cpp's HotkeyKeyName.
unsigned ImGuiKeyToVk(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        return unsigned('A' + (key - ImGuiKey_A));
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        return unsigned('0' + (key - ImGuiKey_0));
    }
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
        return unsigned(VK_F1 + (key - ImGuiKey_F1));
    }
    switch (key) {
        case ImGuiKey_Space: return VK_SPACE;
        case ImGuiKey_Apostrophe: return VK_OEM_7;
        case ImGuiKey_Comma: return VK_OEM_COMMA;
        case ImGuiKey_Minus: return VK_OEM_MINUS;
        case ImGuiKey_Period: return VK_OEM_PERIOD;
        case ImGuiKey_Slash: return VK_OEM_2;
        case ImGuiKey_Semicolon: return VK_OEM_1;
        case ImGuiKey_Equal: return VK_OEM_PLUS;
        case ImGuiKey_LeftBracket: return VK_OEM_4;
        case ImGuiKey_Backslash: return VK_OEM_5;
        case ImGuiKey_RightBracket: return VK_OEM_6;
        case ImGuiKey_GraveAccent: return VK_OEM_3;
        case ImGuiKey_Insert: return VK_INSERT;
        case ImGuiKey_Delete: return VK_DELETE;
        case ImGuiKey_Home: return VK_HOME;
        case ImGuiKey_End: return VK_END;
        case ImGuiKey_PageUp: return VK_PRIOR;
        case ImGuiKey_PageDown: return VK_NEXT;
        case ImGuiKey_UpArrow: return VK_UP;
        case ImGuiKey_DownArrow: return VK_DOWN;
        case ImGuiKey_LeftArrow: return VK_LEFT;
        case ImGuiKey_RightArrow: return VK_RIGHT;
        default: return 0;
    }
}

// ---- standalone dialog window (its own device + ImGui context) ------------

constexpr wchar_t kDlgClassName[] = L"SundialSettingsDlg";

struct DialogState {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT clientW = 0;
    UINT clientH = 0;
    bool resizeRequested = false;
    bool exitRequested = false;
};

LRESULT CALLBACK DlgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        return true;
    }
    auto* st = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_SIZE:
            if (st && wp != SIZE_MINIMIZED) {
                st->clientW = LOWORD(lp);
                st->clientH = HIWORD(lp);
                st->resizeRequested = true;
            }
            return 0;
        case WM_CLOSE:
            if (st) {
                st->exitRequested = true;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureDlgClassRegistered() {
    static bool registered = false;
    if (registered) {
        return;
    }
    HICON appIcon = LoadIconW(GetModuleHandleW(nullptr),
                              MAKEINTRESOURCEW(IDI_APP_ICON));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DlgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kDlgClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = appIcon;
    wc.hIconSm = appIcon;
    wc.style = CS_OWNDC;
    RegisterClassExW(&wc);
    registered = true;
}

bool CreateDeviceAndSwapChain(DialogState& st) {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &st.device,
                                 nullptr, &st.context))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(st.device.As(&dxgiDevice))) {
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
        return false;
    }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = st.clientW;
    scd.Height = st.clientH;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(st.device.Get(), st.hwnd, &scd,
                                               nullptr, nullptr,
                                               &st.swapChain))) {
        return false;
    }
    factory->MakeWindowAssociation(st.hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

void CreateBackbufferRtv(DialogState& st) {
    st.rtv.Reset();
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(st.swapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
        return;
    }
    st.device->CreateRenderTargetView(bb.Get(), nullptr, &st.rtv);
}

void ResizeSwapChain(DialogState& st) {
    if (!st.swapChain || st.clientW == 0 || st.clientH == 0) {
        return;
    }
    st.rtv.Reset();
    st.swapChain->ResizeBuffers(0, st.clientW, st.clientH, DXGI_FORMAT_UNKNOWN,
                                0);
    CreateBackbufferRtv(st);
}

}  // namespace

bool DrawSettingsControls(AppSettings& s, HWND owner, const bool* captureIsHdr,
                          bool& capturingHotkey, bool& hotkeyChanged) {
    hotkeyChanged = false;
    bool changed = false;

    ImGui::TextUnformatted("Image Snapshot Formats");
    ImGui::TextDisabled("Video recording has its own format settings.");
    ImGui::Separator();

    changed |= ImGui::Checkbox("PNG (SDR)", &s.snapshot.png);
    changed |= ImGui::Checkbox("JPEG XR / .jxr (HDR)", &s.snapshot.jxr);
#ifdef SUNDIAL_HAS_ULTRAHDR
    changed |= ImGui::Checkbox("Ultra HDR JPEG / .jpg (SDR+HDR)",
                               &s.snapshot.ultraHdrJpeg);
#else
    ImGui::BeginDisabled();
    bool noUltra = false;
    ImGui::Checkbox("Ultra HDR JPEG / .jpg (not built)", &noUltra);
    ImGui::EndDisabled();
#endif
#ifdef SUNDIAL_HAS_AVIF
    changed |= ImGui::Checkbox("HDR AVIF / .avif (HDR)", &s.snapshot.avif);
    // The two encodings are mutually exclusive; show as a radio pair, grayed
    // until AVIF is enabled.
    if (!s.snapshot.avif) {
        ImGui::BeginDisabled();
    }
    int avifMode = static_cast<int>(s.snapshot.avifMode);
    ImGui::Indent();
    bool modeChanged = false;
    modeChanged |= ImGui::RadioButton("PQ (10-bit HDR)##avifmode", &avifMode,
                                      static_cast<int>(AvifHdrMode::Pq));
    ImGui::SameLine();
    modeChanged |= ImGui::RadioButton("Gain map (SDR+HDR)##avifmode", &avifMode,
                                      static_cast<int>(AvifHdrMode::GainMap));
    ImGui::Unindent();
    if (modeChanged) {
        s.snapshot.avifMode = static_cast<AvifHdrMode>(avifMode);
        changed = true;
    }
    if (!s.snapshot.avif) {
        ImGui::EndDisabled();
    }
#else
    ImGui::BeginDisabled();
    bool noAvif = false;
    ImGui::Checkbox("HDR AVIF / .avif (not built)", &noAvif);
    ImGui::EndDisabled();
#endif
    if (captureIsHdr && !*captureIsHdr) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
                           "Current capture is SDR - only PNG will be written.");
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();

    changed |= ImGui::Checkbox("Edit on Capture", &s.editOnCapture);
    changed |= ImGui::Checkbox("Auto Copy Capture", &s.autoCopyCapture);

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Output Folder");
    AppSettings probe{};
    probe.outputFolder = s.outputFolder;
    const std::string folderUtf8 = WideToUtf8(ResolveOutputDir(probe).wstring());
    ImGui::TextWrapped("%s", folderUtf8.c_str());
    if (ImGui::Button("Change...")) {
        const std::wstring seed = s.outputFolder.empty()
                                      ? DefaultOutputDir().wstring()
                                      : s.outputFolder;
        std::wstring picked = PickFolderDialog(owner, seed);
        if (!picked.empty()) {
            s.outputFolder = (picked == DefaultOutputDir().wstring())
                                 ? std::wstring{}
                                 : picked;
            changed = true;
        }
    }
    ImGui::SameLine();
    const bool customFolder = !s.outputFolder.empty();
    if (!customFolder) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Reset to Default")) {
        s.outputFolder.clear();
        changed = true;
    }
    if (!customFolder) {
        ImGui::EndDisabled();
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::TextUnformatted("Capture Hotkey");
    ImGui::TextDisabled("Global shortcut that opens the capture toolbar.");

    unsigned mods = s.hotkeyMods;
    auto modCheck = [&](const char* label, unsigned bit) {
        bool on = (mods & bit) != 0;
        if (ImGui::Checkbox(label, &on)) {
            mods = on ? (mods | bit) : (mods & ~bit);
        }
    };
    modCheck("Win", kHotkeyWin);
    ImGui::SameLine();
    modCheck("Ctrl", kHotkeyControl);
    ImGui::SameLine();
    modCheck("Alt", kHotkeyAlt);
    ImGui::SameLine();
    modCheck("Shift", kHotkeyShift);

    ImGui::SameLine();
    const std::string keyLabel =
        capturingHotkey ? std::string("Press a key...")
                        : ("Key: " + HotkeyKeyName(s.hotkeyVk));
    if (ImGui::Button(keyLabel.c_str(), ImVec2(150, 0))) {
        capturingHotkey = !capturingHotkey;
    }

    unsigned newVk = s.hotkeyVk;
    if (capturingHotkey) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            capturingHotkey = false;
        } else {
            for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN;
                 k < ImGuiKey_NamedKey_END;
                 k = static_cast<ImGuiKey>(k + 1)) {
                if (!ImGui::IsKeyPressed(k, false)) {
                    continue;
                }
                const unsigned vk = ImGuiKeyToVk(k);
                if (vk != 0) {
                    newVk = vk;
                    capturingHotkey = false;
                    break;
                }
            }
        }
    }

    ImGui::Text("Current: %s", HotkeyToString(mods, newVk).c_str());
    if (mods == 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
                           "Add a modifier (Win/Ctrl/Alt/Shift) so the hotkey "
                           "doesn't fire while typing.");
    }

    if (mods != s.hotkeyMods || newVk != s.hotkeyVk) {
        s.hotkeyMods = mods;
        s.hotkeyVk = newVk;
        changed = true;
        hotkeyChanged = true;
    }
    return changed;
}

void ShowSettingsDialog(HWND owner, AppSettings& settings) {
    EnsureDlgClassRegistered();

    DialogState st;

    const int winW = 470;
    const int winH = 600;

    // Center on the owner when given (else the monitor work area). The owner can
    // be a short toolbar pinned near the top of the screen, so always clamp the
    // dialog fully onto its monitor's work area - otherwise centering on a tiny,
    // high owner pushes the top of the dialog (and most of the settings) off the
    // top edge.
    HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT wa{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) {
        wa = mi.rcWork;
    }

    int x;
    int y;
    RECT anchor{};
    if (owner && GetWindowRect(owner, &anchor)) {
        x = anchor.left + ((anchor.right - anchor.left) - winW) / 2;
        y = anchor.top + ((anchor.bottom - anchor.top) - winH) / 2;
    } else {
        x = wa.left + ((wa.right - wa.left) - winW) / 2;
        y = wa.top + ((wa.bottom - wa.top) - winH) / 2;
    }
    // Keep the whole window on-screen (clamp bottom/right first, then top/left so
    // the title bar wins if the work area is smaller than the window).
    if (x + winW > wa.right) {
        x = wa.right - winW;
    }
    if (y + winH > wa.bottom) {
        y = wa.bottom - winH;
    }
    if (x < wa.left) {
        x = wa.left;
    }
    if (y < wa.top) {
        y = wa.top;
    }

    // Topmost so the dialog sits above the (topmost) capture toolbar that may
    // have launched it. A fixed-size caption window (no resize/maximize).
    st.hwnd = CreateWindowExW(
        WS_EX_TOPMOST, kDlgClassName, L"Sundial Settings",
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, winW, winH, owner,
        nullptr, GetModuleHandleW(nullptr), &st);
    if (!st.hwnd) {
        return;
    }
    RECT rc{};
    GetClientRect(st.hwnd, &rc);
    st.clientW = UINT(rc.right - rc.left);
    st.clientH = UINT(rc.bottom - rc.top);
    if (st.clientW == 0 || st.clientH == 0) {
        DestroyWindow(st.hwnd);
        return;
    }

    if (!CreateDeviceAndSwapChain(st)) {
        DestroyWindow(st.hwnd);
        return;
    }
    CreateBackbufferRtv(st);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(st.hwnd);
    ImGui_ImplDX11_Init(st.device.Get(), st.context.Get());

    ShowWindow(st.hwnd, SW_SHOW);
    SetForegroundWindow(st.hwnd);
    UpdateWindow(st.hwnd);

    bool capturingHotkey = false;
    bool anyHotkeyChange = false;
    bool wantUpdateCheck = false;
    bool wantAbout = false;

    MSG msg{};
    while (!st.exitRequested) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                // Propagate to the outer message loop (e.g. the updater asked
                // the app to quit and restart) and unwind this dialog.
                PostQuitMessage(int(msg.wParam));
                st.exitRequested = true;
            }
        }
        if (st.exitRequested) {
            break;
        }
        if (st.resizeRequested) {
            ResizeSwapChain(st);
            st.resizeRequested = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##settings", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);

        const bool wasCapturing = capturingHotkey;
        bool hotkeyChanged = false;
        const bool changed = DrawSettingsControls(settings, st.hwnd, nullptr,
                                                  capturingHotkey,
                                                  hotkeyChanged);
        if (changed) {
            SaveSettings(settings);
        }
        if (hotkeyChanged) {
            anyHotkeyChange = true;
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            st.exitRequested = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("About...", ImVec2(110, 0))) {
            wantAbout = true;
        }
#ifdef SUNDIAL_HAS_UPDATER
        ImGui::SameLine();
        if (ImGui::Button("Check for Updates...", ImVec2(170, 0))) {
            // Close first, then kick the check off the resident thread - the
            // restart-to-update path posts WM_QUIT and should unwind the main
            // loop, not run inside this nested one.
            wantUpdateCheck = true;
            st.exitRequested = true;
        }
#endif

        ImGui::End();

        // Esc closes the dialog, except on the frame it cancelled a key capture.
        if (!wasCapturing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            st.exitRequested = true;
        }

        if (wantAbout) {
            ShowAbout(st.hwnd);
            wantAbout = false;
        }

        ImGui::Render();
        ID3D11RenderTargetView* rtv = st.rtv.Get();
        const float bg[4] = {0.10f, 0.10f, 0.10f, 1.0f};
        st.context->OMSetRenderTargets(1, &rtv, nullptr);
        st.context->ClearRenderTargetView(rtv, bg);
        D3D11_VIEWPORT vp11{};
        vp11.Width = float(st.clientW);
        vp11.Height = float(st.clientH);
        vp11.MaxDepth = 1.0f;
        st.context->RSSetViewports(1, &vp11);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        st.swapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(st.hwnd);

    if (anyHotkeyChange) {
        // Ask the resident instance to re-register the hotkey (this process is
        // usually the resident one, which picks the message up on its main
        // loop right after this returns).
        if (HWND running = FindRunningTrayWindow()) {
            PostMessageW(running, TrayReloadHotkeyMessage(), 0, 0);
        }
    }

#ifdef SUNDIAL_HAS_UPDATER
    if (wantUpdateCheck) {
        CheckForUpdatesInBackground(/*silent=*/false, GetCurrentThreadId());
    }
#endif
}

}  // namespace sundial
