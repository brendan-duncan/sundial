#include "Editor.h"

#include "Clipboard.h"
#include "ImageOps.h"
#include "Resource.h"
#include "ShaderTonemap.h"
#include "Tonemap.h"

#include <Windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT,
                                                             WPARAM, LPARAM);

namespace sundial {
namespace {

constexpr wchar_t kClassName[] = L"SundialEditor";

std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

// Custom pixel shader used in HDR mode so that ImGui draws land correctly on
// an scRGB FP16 backbuffer. ImGui's vertex colors and font atlas are sRGB-
// encoded (gamma-space), but an scRGB backbuffer expects linear values, so
// we decode on the way out. The uTexIsLinear flag lets the editor mark the
// HDR-passthrough preview texture (which is already FP16 linear) so we skip
// the texture decode for that one draw.
constexpr char kHdrImguiPsSrc[] = R"HLSL(
struct PSIn {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
Texture2D     gTex  : register(t0);
SamplerState  gSamp : register(s0);
cbuffer Cfg : register(b0) {
    uint uTexIsLinear;
    uint _pad[3];
};
float3 SrgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float4 main(PSIn i) : SV_Target {
    float4 t = gTex.Sample(gSamp, i.uv);
    float3 vc = SrgbToLinear(i.col.rgb);
    float3 tc = (uTexIsLinear != 0u) ? t.rgb : SrgbToLinear(t.rgb);
    return float4(vc * tc, i.col.a * t.a);
}
)HLSL";

struct HdrImguiPipeline {
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    UINT lastTexIsLinear = 0xFFFFFFFFu;

    bool Init(ID3D11Device* dev) {
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3DCompile(kHdrImguiPsSrc, sizeof(kHdrImguiPsSrc) - 1,
                              "imgui_hdr.hlsl", nullptr, nullptr, "main",
                              "ps_5_0", 0, 0, &blob, &err))) {
            return false;
        }
        if (FAILED(dev->CreatePixelShader(blob->GetBufferPointer(),
                                          blob->GetBufferSize(), nullptr,
                                          &ps))) {
            return false;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb));
    }

    void Bind(ID3D11DeviceContext* dc, UINT texIsLinear) {
        dc->PSSetShader(ps.Get(), nullptr, 0);
        if (texIsLinear != lastTexIsLinear) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(dc->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                  &m))) {
                auto* p = static_cast<UINT*>(m.pData);
                p[0] = texIsLinear;
                p[1] = p[2] = p[3] = 0;
                dc->Unmap(cb.Get(), 0);
            }
            lastTexIsLinear = texIsLinear;
        }
        ID3D11Buffer* cbs[] = {cb.Get()};
        dc->PSSetConstantBuffers(0, 1, cbs);
    }
};

struct EditorContext {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> backbufferRtv;
    UINT clientW = 0;
    UINT clientH = 0;
    bool resizeRequested = false;
    bool exitRequested = false;
    EditorResult result;
    const Frame* source = nullptr;
    ShaderTonemap tonemap;

    // True when the monitor containing the editor is in HDR mode; turns on
    // the FP16 scRGB swap chain + custom ImGui pixel shader so the preview
    // can carry real HDR brightness.
    bool hdrMode = false;
    HdrImguiPipeline hdrPipeline;

    // Editor UI state
    TonemapParams params{};
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    int resizeW = 0, resizeH = 0;
    bool resizeLockAspect = true;
    float aspect = 1.0f;

    // Named tonemap presets stored under %APPDATA%\Sundial\presets\.
    // activePreset is the name currently shown in the combo; cleared when the
    // user picks "<unsaved>" or when a delete removes the loaded preset.
    std::vector<std::wstring> presetNames;
    std::wstring activePreset;
    char presetNameBuf[64] = "";

    // Interactive-crop drag state. cropDragAnchor is the screen-space point
    // where the drag started; the rect spans from there to the current mouse.
    bool cropDragActive = false;
    bool cropDragMoved = false;   // true once mouse has moved >threshold px
    ImVec2 cropDragAnchor{};

    // Save HDR JXR (mirrors AppSettings::saveHdrJxr while the editor runs).
    bool saveHdrJxr = true;

    // When non-empty, the Save button overwrites this path and Save As
    // opens its dialog with this directory + stem pre-filled. Empty for a
    // fresh capture (Save then writes to <outputFolder>/sundial_<ts>.png).
    std::wstring defaultSavePath;

    // User-configured save folder, mirrored from AppSettings::outputFolder.
    // Empty falls back to DefaultOutputDir() (<Pictures>/Sundial).
    std::wstring outputFolder;

    // Brief "Copied to clipboard" feedback timer.
    std::chrono::steady_clock::time_point copyFeedbackUntil{};

    // Comparison view state.
    enum class ViewMode { SdrOnly = 0, Split = 1, SideBySide = 2 };
    ViewMode viewMode = ViewMode::SdrOnly;
    bool holdHdr = false;       // SDR-only: button held to peek at HDR
    float splitPos = 0.5f;      // Split mode divider, 0..1 of preview width
};

LRESULT CALLBACK EditorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    auto* ctx = reinterpret_cast<EditorContext*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_SIZE:
            if (ctx && wp != SIZE_MINIMIZED) {
                ctx->clientW = LOWORD(lp);
                ctx->clientH = HIWORD(lp);
                ctx->resizeRequested = true;
            }
            return 0;
        case WM_CLOSE:
            if (ctx) ctx->exitRequested = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureClassRegistered() {
    static bool registered = false;
    if (registered) return;
    HICON appIcon = LoadIconW(GetModuleHandleW(nullptr),
                              MAKEINTRESOURCEW(IDI_APP_ICON));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = EditorWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = appIcon;
    wc.hIconSm = appIcon;
    wc.style = CS_OWNDC;
    RegisterClassExW(&wc);
    registered = true;
}

// Returns true if the monitor that contains `hwnd` is currently in HDR mode
// (its DXGI color space is HDR10 PQ). We use this to decide whether to
// create an scRGB FP16 swap chain so the editor's preview can display real
// HDR brightness, or fall back to the standard 8-bit SDR path.
bool DisplayIsHdr(IDXGIAdapter* adapter, HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!mon) return false;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0;
         adapter->EnumOutputs(i, output.ReleaseAndGetAddressOf()) !=
             DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_OUTPUT_DESC od;
        if (FAILED(output->GetDesc(&od))) continue;
        if (od.Monitor != mon) continue;
        ComPtr<IDXGIOutput6> o6;
        if (FAILED(output.As(&o6))) continue;
        DXGI_OUTPUT_DESC1 od1{};
        if (FAILED(o6->GetDesc1(&od1))) continue;
        return od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }
    return false;
}

bool CreateDeviceAndSwapChain(EditorContext& ctx) {
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, nullptr, 0, D3D11_SDK_VERSION,
                                 &ctx.device, &fl, &ctx.context))) {
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(ctx.device.As(&dxgiDevice))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    // Use an FP16 scRGB swap chain when the editor is on an HDR display so
    // the preview can carry values above SDR white. Fall back to 8-bit if
    // that fails (older driver / display lost between detection and create).
    ctx.hdrMode = ctx.source && ctx.source->isHdr &&
                  DisplayIsHdr(adapter.Get(), ctx.hwnd);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = ctx.clientW;
    scd.Height = ctx.clientH;
    scd.Format = ctx.hdrMode ? DXGI_FORMAT_R16G16B16A16_FLOAT
                             : DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = factory->CreateSwapChainForHwnd(ctx.device.Get(), ctx.hwnd,
                                                 &scd, nullptr, nullptr,
                                                 &ctx.swapChain);
    if (FAILED(hr) && ctx.hdrMode) {
        ctx.hdrMode = false;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = factory->CreateSwapChainForHwnd(ctx.device.Get(), ctx.hwnd,
                                             &scd, nullptr, nullptr,
                                             &ctx.swapChain);
    }
    if (FAILED(hr)) return false;

    if (ctx.hdrMode) {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(ctx.swapChain.As(&sc3))) {
            sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        }
    }

    factory->MakeWindowAssociation(ctx.hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

void CreateBackbufferRtv(EditorContext& ctx) {
    ctx.backbufferRtv.Reset();
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(ctx.swapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    ctx.device->CreateRenderTargetView(bb.Get(), nullptr, &ctx.backbufferRtv);
}

Frame ProduceEditedFrame(const EditorContext& ctx) {
    Frame edited = *ctx.source;
    const bool needCrop =
        !(ctx.cropX == 0 && ctx.cropY == 0 &&
          ctx.cropW == int(ctx.source->width) &&
          ctx.cropH == int(ctx.source->height));
    if (needCrop) {
        edited = Crop(edited, uint32_t(ctx.cropX), uint32_t(ctx.cropY),
                      uint32_t(ctx.cropW), uint32_t(ctx.cropH));
    }
    if (ctx.resizeW != int(edited.width) ||
        ctx.resizeH != int(edited.height)) {
        edited = Resize(edited, uint32_t(ctx.resizeW),
                        uint32_t(ctx.resizeH));
    }
    return edited;
}

std::wstring PickSaveAsPath(HWND owner, const std::wstring& defaultPath,
                            const std::wstring& outputFolderOverride) {
    std::filesystem::path defaultDir;
    std::wstring stem;
    if (!defaultPath.empty()) {
        std::filesystem::path p = defaultPath;
        defaultDir = p.parent_path();
        stem = p.stem().wstring();
    }
    if (defaultDir.empty()) {
        AppSettings ds{};
        ds.outputFolder = outputFolderOverride;
        defaultDir = ResolveOutputDir(ds);
    }
    std::error_code ec;
    std::filesystem::create_directories(defaultDir, ec);

    if (stem.empty()) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t buf[32];
        swprintf_s(buf, L"sundial_%04d%02d%02d_%02d%02d%02d",
                   t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        stem = buf;
    }

    wchar_t fileName[MAX_PATH] = {};
    swprintf_s(fileName, L"%s.png", stem.c_str());

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.lpstrInitialDir = defaultDir.c_str();
    ofn.lpstrTitle = L"Save As";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn)) {
        return std::wstring(fileName);
    }
    return std::wstring{};
}

void DoCopy(EditorContext& ctx) {
    Frame edited = ProduceEditedFrame(ctx);
    if (CopyFrameToClipboard(edited, ctx.params, ctx.hwnd)) {
        ctx.copyFeedbackUntil =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
}

void ResizeSwapChain(EditorContext& ctx) {
    if (!ctx.swapChain) return;
    ctx.backbufferRtv.Reset();
    ctx.swapChain->ResizeBuffers(0, ctx.clientW, ctx.clientH,
                                 DXGI_FORMAT_UNKNOWN, 0);
    CreateBackbufferRtv(ctx);
}

enum class IconKind { None, Save, SaveAs, Copy, Cancel, Hdr, Sdr };

void DrawIcon(ImDrawList* dl, ImVec2 c, IconKind kind) {
    const ImU32 col = IM_COL32(220, 220, 220, 255);
    const float r = 6.0f;
    switch (kind) {
        case IconKind::Save: {
            // Floppy: outer square + top "shutter" strip + lower label box
            dl->AddRect({c.x - r, c.y - r}, {c.x + r, c.y + r}, col,
                        1.0f, 0, 1.4f);
            dl->AddRectFilled({c.x - r + 1, c.y - r + 1},
                              {c.x + r - 1, c.y - r * 0.3f}, col);
            dl->AddRect({c.x - r * 0.55f, c.y + r * 0.1f},
                        {c.x + r * 0.55f, c.y + r - 1}, col, 0, 0, 1.0f);
            break;
        }
        case IconKind::SaveAs: {
            // Floppy + ellipsis suffix to imply "...as something"
            dl->AddRect({c.x - r, c.y - r}, {c.x + r - 2, c.y + r - 2}, col,
                        1.0f, 0, 1.4f);
            dl->AddRectFilled({c.x - r + 1, c.y - r + 1},
                              {c.x + r - 3, c.y - r * 0.3f}, col);
            dl->AddCircleFilled({c.x + r - 2, c.y + r - 1}, 1.0f, col);
            dl->AddCircleFilled({c.x + r + 1, c.y + r - 1}, 1.0f, col);
            dl->AddCircleFilled({c.x + r + 4, c.y + r - 1}, 1.0f, col);
            break;
        }
        case IconKind::Copy: {
            // Two overlapping pages
            dl->AddRect({c.x - r, c.y - r + 2}, {c.x + r - 2, c.y + r},
                        col, 0, 0, 1.4f);
            dl->AddRect({c.x - r + 2, c.y - r}, {c.x + r, c.y + r - 2},
                        col, 0, 0, 1.4f);
            break;
        }
        case IconKind::Cancel: {
            dl->AddLine({c.x - r, c.y - r}, {c.x + r, c.y + r}, col, 1.8f);
            dl->AddLine({c.x + r, c.y - r}, {c.x - r, c.y + r}, col, 1.8f);
            break;
        }
        case IconKind::Hdr: {
            // Small "sun": centre circle + 8 short rays
            dl->AddCircleFilled(c, r * 0.45f, col);
            for (int i = 0; i < 8; ++i) {
                const float a = i * 0.7853981633f;
                const float ca = cosf(a), sa = sinf(a);
                dl->AddLine({c.x + ca * r * 0.65f, c.y + sa * r * 0.65f},
                            {c.x + ca * r * 0.95f, c.y + sa * r * 0.95f},
                            col, 1.2f);
            }
            break;
        }
        case IconKind::Sdr: {
            // Solid square, like a flat monitor
            dl->AddRectFilled({c.x - r, c.y - r * 0.6f},
                              {c.x + r, c.y + r * 0.6f}, col);
            break;
        }
        default: break;
    }
}

// Button that displays an icon at the left and a label to the right of it.
// The clickable area is the full button rect; size.x=0 auto-fits the label.
bool IconButton(const char* id, const char* label, IconKind icon,
                ImVec2 size = ImVec2(0, 30)) {
    if (size.x == 0) {
        size.x = ImGui::CalcTextSize(label).x + 32.0f;
    }
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    bool pressed = ImGui::Button("##b", size);
    ImGui::PopID();

    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 iconCenter{cursor.x + 14.0f, cursor.y + size.y * 0.5f};
    DrawIcon(dl, iconCenter, icon);

    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    const ImVec2 labelPos{cursor.x + 26.0f,
                          cursor.y + (size.y - labelSize.y) * 0.5f};
    dl->AddText(labelPos, ImGui::GetColorU32(ImGuiCol_Text), label);
    return pressed;
}

// Path the Save button writes to. If `defaultPath` is set we overwrite that
// file in place. Otherwise we generate <outputFolder>/sundial_<ts>.png, where
// outputFolder is the user-configured save location (or the platform default
// when unset).
std::wstring DefaultSavePath(const std::wstring& defaultPath,
                             const std::wstring& outputFolderOverride) {
    if (!defaultPath.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(defaultPath).parent_path(), ec);
        return defaultPath;
    }
    AppSettings ds{};
    ds.outputFolder = outputFolderOverride;
    std::filesystem::path dir = ResolveOutputDir(ds);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"sundial_%04d%02d%02d_%02d%02d%02d",
               t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond);
    return (dir / (std::wstring(buf) + L".png")).wstring();
}

void DrawPresetSection(EditorContext& ctx) {
    ImGui::TextUnformatted("Preset");

    const std::string activeUtf8 =
        ctx.activePreset.empty() ? "<unsaved>" : WideToUtf8(ctx.activePreset);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##presetCombo", activeUtf8.c_str())) {
        if (ImGui::Selectable("<unsaved>", ctx.activePreset.empty())) {
            ctx.activePreset.clear();
        }
        for (const auto& n : ctx.presetNames) {
            const std::string label = WideToUtf8(n);
            const bool selected = n == ctx.activePreset;
            if (ImGui::Selectable(label.c_str(), selected)) {
                TonemapParams loaded;
                if (LoadPreset(n, loaded)) {
                    ctx.params = loaded;
                    ctx.activePreset = n;
                }
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button("Save As...")) {
        const std::string seed = WideToUtf8(ctx.activePreset);
        std::snprintf(ctx.presetNameBuf, sizeof(ctx.presetNameBuf), "%s",
                      seed.c_str());
        ImGui::OpenPopup("Save Preset");
    }
    ImGui::SameLine();
    const bool hasActive = !ctx.activePreset.empty();
    if (!hasActive) ImGui::BeginDisabled();
    if (ImGui::Button("Delete")) {
        ImGui::OpenPopup("Delete Preset?");
    }
    if (!hasActive) ImGui::EndDisabled();

    if (ImGui::BeginPopupModal("Save Preset", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Name:");
        ImGui::SetNextItemWidth(280);
        const bool enter = ImGui::InputText(
            "##presetName", ctx.presetNameBuf, sizeof(ctx.presetNameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue);

        const std::wstring proposed =
            NormalizePresetName(Utf8ToWide(ctx.presetNameBuf));
        bool exists = false;
        for (const auto& n : ctx.presetNames) {
            if (n == proposed) { exists = true; break; }
        }
        if (proposed.empty()) {
            ImGui::TextDisabled("Enter a name to save.");
        } else if (exists) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
                               "Will overwrite existing preset.");
        } else {
            ImGui::TextDisabled("Will create a new preset.");
        }

        const bool canSave = !proposed.empty();
        if (!canSave) ImGui::BeginDisabled();
        if (ImGui::Button("Save", ImVec2(96, 0)) || (enter && canSave)) {
            SavePreset(proposed, ctx.params);
            ctx.presetNames = ListPresets();
            ctx.activePreset = proposed;
            ImGui::CloseCurrentPopup();
        }
        if (!canSave) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(96, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete Preset?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        const std::string name = WideToUtf8(ctx.activePreset);
        ImGui::Text("Delete preset \"%s\"?", name.c_str());
        if (ImGui::Button("Delete", ImVec2(96, 0))) {
            DeletePreset(ctx.activePreset);
            ctx.activePreset.clear();
            ctx.presetNames = ListPresets();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(96, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawOutputSection(EditorContext& ctx) {
    const ImVec2 kBtn(142, 30);
    if (IconButton("save", "Save", IconKind::Save, kBtn)) {
        ctx.result.outputPath =
            DefaultSavePath(ctx.defaultSavePath, ctx.outputFolder);
        ctx.result.saved = true;
        ctx.exitRequested = true;
    }
    ImGui::SameLine();
    if (IconButton("saveas", "Save As...", IconKind::SaveAs, kBtn)) {
        std::wstring path =
            PickSaveAsPath(ctx.hwnd, ctx.defaultSavePath, ctx.outputFolder);
        if (!path.empty()) {
            ctx.result.outputPath = path;
            ctx.result.saved = true;
            ctx.exitRequested = true;
        }
    }
    if (IconButton("copy", "Copy", IconKind::Copy, kBtn)) {
        DoCopy(ctx);
    }
    ImGui::SameLine();
    if (IconButton("cancel", "Cancel", IconKind::Cancel, kBtn)) {
        ctx.result.saved = false;
        ctx.exitRequested = true;
    }
    if (std::chrono::steady_clock::now() < ctx.copyFeedbackUntil) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                           "Copied to clipboard");
    }

    // HDR JXR toggle as a row with the sun icon next to the checkbox.
    ImGui::Dummy(ImVec2(0, 4));
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    DrawIcon(dl, ImVec2(cursor.x + 8, cursor.y + 9), IconKind::Hdr);
    ImGui::Dummy(ImVec2(20, 0));
    ImGui::SameLine();
    if (ctx.source->isHdr) {
        ImGui::Checkbox("Save HDR Image (JXR)", &ctx.saveHdrJxr);
    } else {
        ImGui::BeginDisabled();
        bool dummy = false;
        ImGui::Checkbox("Save HDR Image (JXR) - capture is SDR", &dummy);
        ImGui::EndDisabled();
    }
}

void DrawSidebar(EditorContext& ctx) {
    ImGui::BeginChild("sidebar", ImVec2(320, 0), true);

    // The output controls stay pinned at the top: render them in the outer
    // (non-scrolling) area first, then put the settings inside a nested
    // BeginChild that scrolls on its own.
    DrawOutputSection(ctx);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();

    ImGui::BeginChild("sidebar_scroll", ImVec2(0, 0), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    ImGui::TextUnformatted("HDR to SDR");
    ImGui::Separator();

    DrawPresetSection(ctx);
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Curve & exposure",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kCurves[] = {"Linear (clip)", "Reinhard", "ACES",
                                        "Hable", "AgX", "Khronos Neutral",
                                        "Preserve SDR", "BT.2390 (reference)"};
        int curve = static_cast<int>(ctx.params.curve);
        if (ImGui::Combo("Curve", &curve, kCurves, IM_ARRAYSIZE(kCurves))) {
            ctx.params.curve = static_cast<TonemapCurve>(curve);
        }
        ImGui::SliderFloat("SDR white (nits)", &ctx.params.sdrWhiteNits,
                           40.0f, 400.0f, "%.0f");
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto##sdrwhite")) {
            ctx.params.sdrWhiteNits = AutoSdrWhite(*ctx.source);
        }
        ImGui::SliderFloat("Exposure (EV)", &ctx.params.exposureEV, -4.0f,
                           4.0f, "%+.2f");
        ImGui::SliderFloat("Black point lift", &ctx.params.blackPointLift,
                           0.0f, 0.5f, "%.3f");
        ImGui::SliderFloat("Highlight rolloff",
                           &ctx.params.highlightRolloff, 0.0f, 1.0f, "%.2f");

        // Knee / desat / source peak only affect PreserveSdr + BT2390. Show
        // them in the same section so the curve's full controls are together.
        const bool kneeAware = ctx.params.curve == TonemapCurve::PreserveSdr ||
                               ctx.params.curve == TonemapCurve::BT2390;
        if (!kneeAware) ImGui::BeginDisabled();
        ImGui::SliderFloat("Knee point", &ctx.params.kneePoint, 0.05f, 0.95f,
                           "%.2f");
        ImGui::SliderFloat("Highlight desat", &ctx.params.highlightDesat,
                           0.0f, 1.0f, "%.2f");
        if (!kneeAware) ImGui::EndDisabled();

        const bool peakAware = ctx.params.curve == TonemapCurve::BT2390;
        if (!peakAware) ImGui::BeginDisabled();
        ImGui::SliderFloat("Source peak (nits)", &ctx.params.sourcePeakNits,
                           400.0f, 4000.0f, "%.0f");
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto##srcpeak")) {
            const float peak = ctx.source->maxLuminanceNits > 0.0f
                                   ? ctx.source->maxLuminanceNits
                                   : 1000.0f;
            ctx.params.sourcePeakNits = std::clamp(peak, 400.0f, 4000.0f);
        }
        if (!peakAware) ImGui::EndDisabled();
    }
    if (ImGui::CollapsingHeader("Color")) {
        ImGui::SliderFloat("Saturation", &ctx.params.saturation, 0.0f, 2.0f,
                           "%.2f");
        ImGui::SliderFloat("Temperature", &ctx.params.temperature, -1.0f,
                           1.0f, "%+.2f");
        ImGui::SliderFloat("Tint", &ctx.params.tint, -1.0f, 1.0f, "%+.2f");
        ImGui::SliderFloat("Gamut compress", &ctx.params.gamutCompress,
                           0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("R gain", &ctx.params.rGain, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("G gain", &ctx.params.gGain, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("B gain", &ctx.params.bGain, 0.0f, 2.0f, "%.2f");
    }
    if (ImGui::CollapsingHeader("Detail")) {
        ImGui::SliderFloat("Sharpen", &ctx.params.sharpen, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Local tonemap", &ctx.params.localStrength, 0.0f,
                           1.0f, "%.2f");
        ImGui::TextDisabled(
            "Local: compress brightness per-region so dark UI keeps detail\n"
            "in front of bright HDR content.");
    }
    if (ImGui::CollapsingHeader("Output")) {
        static const char* kGammas[] = {"sRGB", "Gamma 2.2",
                                        "Linear (none)"};
        int gamma = static_cast<int>(ctx.params.outputGamma);
        if (ImGui::Combo("Gamma", &gamma, kGammas,
                         IM_ARRAYSIZE(kGammas))) {
            ctx.params.outputGamma = static_cast<OutputGamma>(gamma);
        }
    }
    if (ImGui::Button("Reset tonemap")) {
        ctx.params = TonemapParams{};
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("Crop");
    ImGui::TextDisabled("Drag on the preview to redraw the crop.");
    ImGui::Separator();
    const int maxW = int(ctx.source->width);
    const int maxH = int(ctx.source->height);
    ImGui::DragInt("X", &ctx.cropX, 1, 0, maxW - 1);
    ImGui::DragInt("Y", &ctx.cropY, 1, 0, maxH - 1);
    ImGui::DragInt("Width", &ctx.cropW, 1, 1, maxW - ctx.cropX);
    ImGui::DragInt("Height", &ctx.cropH, 1, 1, maxH - ctx.cropY);
    if (ImGui::Button("Reset crop")) {
        ctx.cropX = ctx.cropY = 0;
        ctx.cropW = maxW;
        ctx.cropH = maxH;
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("Resize");
    ImGui::Separator();
    ImGui::Checkbox("Lock aspect", &ctx.resizeLockAspect);
    int prevW = ctx.resizeW;
    int prevH = ctx.resizeH;
    if (ImGui::DragInt("Out width", &ctx.resizeW, 1, 1, 16384)) {
        if (ctx.resizeLockAspect && prevW > 0) {
            ctx.resizeH = std::max(1, int(ctx.resizeW / ctx.aspect + 0.5f));
        }
    }
    if (ImGui::DragInt("Out height", &ctx.resizeH, 1, 1, 16384)) {
        if (ctx.resizeLockAspect && prevH > 0) {
            ctx.resizeW = std::max(1, int(ctx.resizeH * ctx.aspect + 0.5f));
        }
    }
    if (ImGui::Button("Reset size")) {
        ctx.resizeW = maxW;
        ctx.resizeH = maxH;
    }

    ImGui::EndChild();  // sidebar_scroll
    ImGui::EndChild();  // sidebar
}

void DrawCropOverlayAndInteraction(EditorContext& ctx, ImVec2 imgTL, float w,
                                   float h, bool interactive) {
    const float scaleX = w / float(ctx.source->width);
    const float scaleY = h / float(ctx.source->height);

    if (interactive) {
        ImGui::SetCursorScreenPos(imgTL);
        ImGui::InvisibleButton("##cropArea", ImVec2(w, h));

        const auto screenToSource = [&](ImVec2 p) {
            const float sx = (p.x - imgTL.x) / scaleX;
            const float sy = (p.y - imgTL.y) / scaleY;
            return ImVec2(
                std::clamp(sx, 0.0f, float(ctx.source->width)),
                std::clamp(sy, 0.0f, float(ctx.source->height)));
        };
        if (ImGui::IsItemActivated()) {
            ctx.cropDragActive = true;
            ctx.cropDragMoved = false;
            ctx.cropDragAnchor = ImGui::GetIO().MousePos;
        }
        if (ctx.cropDragActive && ImGui::IsItemActive()) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            // Only treat this as a crop drag once the mouse has moved a
            // small threshold; a bare click shouldn't redefine the crop.
            constexpr float kDragThresholdPx = 4.0f;
            if (!ctx.cropDragMoved) {
                const float dx = mouse.x - ctx.cropDragAnchor.x;
                const float dy = mouse.y - ctx.cropDragAnchor.y;
                if (dx * dx + dy * dy >=
                    kDragThresholdPx * kDragThresholdPx) {
                    ctx.cropDragMoved = true;
                }
            }
            if (ctx.cropDragMoved) {
                ImVec2 a = screenToSource(ctx.cropDragAnchor);
                ImVec2 c = screenToSource(mouse);
                int x0 = int(std::floor(std::min(a.x, c.x)));
                int y0 = int(std::floor(std::min(a.y, c.y)));
                int x1 = int(std::ceil(std::max(a.x, c.x)));
                int y1 = int(std::ceil(std::max(a.y, c.y)));
                ctx.cropX = std::clamp(x0, 0, int(ctx.source->width));
                ctx.cropY = std::clamp(y0, 0, int(ctx.source->height));
                ctx.cropW = std::max(1, std::clamp(x1, 0,
                                                   int(ctx.source->width)) -
                                            ctx.cropX);
                ctx.cropH = std::max(1, std::clamp(y1, 0,
                                                   int(ctx.source->height)) -
                                            ctx.cropY);
            }
        }
        if (ctx.cropDragActive && ImGui::IsItemDeactivated()) {
            ctx.cropDragActive = false;
            ctx.cropDragMoved = false;
        }
    }

    const ImVec2 cropTL{imgTL.x + ctx.cropX * scaleX,
                        imgTL.y + ctx.cropY * scaleY};
    const ImVec2 cropBR{imgTL.x + (ctx.cropX + ctx.cropW) * scaleX,
                        imgTL.y + (ctx.cropY + ctx.cropH) * scaleY};
    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 dim = IM_COL32(0, 0, 0, 130);
    const bool cropIsFull =
        ctx.cropX == 0 && ctx.cropY == 0 &&
        ctx.cropW == int(ctx.source->width) &&
        ctx.cropH == int(ctx.source->height);
    if (!cropIsFull) {
        dl->AddRectFilled({imgTL.x, imgTL.y}, {imgTL.x + w, cropTL.y}, dim);
        dl->AddRectFilled({imgTL.x, cropBR.y},
                          {imgTL.x + w, imgTL.y + h}, dim);
        dl->AddRectFilled({imgTL.x, cropTL.y}, {cropTL.x, cropBR.y}, dim);
        dl->AddRectFilled({cropBR.x, cropTL.y}, {imgTL.x + w, cropBR.y}, dim);
    }
    dl->AddRect(cropTL, cropBR, IM_COL32(255, 255, 255, 220), 0.0f, 0, 2.0f);
}

// ImGui draw callbacks for the HDR backbuffer. The "begin" callback binds
// our custom pixel shader at the start of the frame (it persists for every
// subsequent ImGui draw since SetupRenderState is only run once per
// RenderDrawData). The other two flip the uTexIsLinear flag so the
// HDR-passthrough preview texture (FP16 linear) bypasses the sRGB decode
// the UI/font draws use.
void HdrImguiBeginCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* ctx = static_cast<EditorContext*>(cmd->UserCallbackData);
    ctx->hdrPipeline.Bind(ctx->context.Get(), 0);
}
void HdrImguiTexLinearCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* ctx = static_cast<EditorContext*>(cmd->UserCallbackData);
    ctx->hdrPipeline.Bind(ctx->context.Get(), 1);
}
void HdrImguiTexSrgbCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* ctx = static_cast<EditorContext*>(cmd->UserCallbackData);
    ctx->hdrPipeline.Bind(ctx->context.Get(), 0);
}

// Helper: wrap an ImGui image draw of the HDR-passthrough texture with the
// "this texture is FP16 linear" callbacks when the editor is in HDR mode.
// In SDR mode the wrap is a no-op so the default ImGui shader applies.
void AddHdrPassthroughImage(ImDrawList* dl, EditorContext& ctx,
                            ImTextureID tex, ImVec2 a, ImVec2 b,
                            ImVec2 uv0 = ImVec2(0, 0),
                            ImVec2 uv1 = ImVec2(1, 1)) {
    const bool wrap = ctx.hdrMode && ctx.tonemap.LinearHdrOutput();
    if (wrap) dl->AddCallback(HdrImguiTexLinearCallback, &ctx);
    dl->AddImage(tex, a, b, uv0, uv1);
    if (wrap) dl->AddCallback(HdrImguiTexSrgbCallback, &ctx);
}

void DrawPreviewToolbar(EditorContext& ctx) {
    bool sdrButtonHeld = false;
    auto modeButton = [&](const char* label, EditorContext::ViewMode mode,
                          bool* holdOut = nullptr) {
        const bool selected = ctx.viewMode == mode;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label)) {
            ctx.viewMode = mode;
            ctx.holdHdr = false;
        }
        if (holdOut) *holdOut = ImGui::IsItemActive();
        if (selected) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    modeButton("SDR", EditorContext::ViewMode::SdrOnly, &sdrButtonHeld);
    modeButton("Split (SDR | HDR)", EditorContext::ViewMode::Split);
    modeButton("Side-by-side", EditorContext::ViewMode::SideBySide);
    ImGui::NewLine();  // close out the SameLine row started by modeButton

    // SDR-only mode: holding the SDR button peeks the HDR-passthrough view.
    if (ctx.viewMode == EditorContext::ViewMode::SdrOnly) {
        ctx.holdHdr = sdrButtonHeld;
        ImGui::TextDisabled("(Hold for HDR)");
    } else {
        ctx.holdHdr = false;
    }
    ImGui::Separator();
}

void DrawPreview(EditorContext& ctx) {
    ImGui::SameLine();
    ImGui::BeginChild("preview", ImVec2(0, 0), true);

    DrawPreviewToolbar(ctx);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x > 4 && avail.y > 4) {
        // Layout depends on view mode.
        const float srcAspect = float(ctx.source->width) /
                                float(std::max(1u, ctx.source->height));

        if (ctx.viewMode == EditorContext::ViewMode::SideBySide) {
            // Two letterboxed panels side by side with a small gap.
            constexpr float kGap = 12.0f;
            const float panelW = (avail.x - kGap) * 0.5f;
            const float panelH = avail.y;
            float w = panelW;
            float h = panelH;
            if (panelW / panelH > srcAspect) {
                w = panelH * srcAspect;
            } else {
                h = panelW / srcAspect;
            }
            const UINT renderW = std::max(1u, UINT(w));
            const UINT renderH = std::max(1u, UINT(h));
            ctx.tonemap.ResizeTarget(renderW, renderH);
            ctx.tonemap.RenderSdr(ctx.params);
            ctx.tonemap.RenderHdrPassthrough();

            const float yOff = (panelH - h) * 0.5f;
            const float panelXOffL = (panelW - w) * 0.5f;
            const ImVec2 sdrTL{ImGui::GetCursorScreenPos().x + panelXOffL,
                               ImGui::GetCursorScreenPos().y + yOff};
            const ImVec2 hdrTL{ImGui::GetCursorScreenPos().x + panelW + kGap +
                                   panelXOffL,
                               ImGui::GetCursorScreenPos().y + yOff};

            auto* dl = ImGui::GetWindowDrawList();
            dl->AddImage(reinterpret_cast<ImTextureID>(ctx.tonemap.GetSdrSrv()),
                         sdrTL, {sdrTL.x + w, sdrTL.y + h});
            AddHdrPassthroughImage(
                dl, ctx,
                reinterpret_cast<ImTextureID>(ctx.tonemap.GetHdrSrv()),
                hdrTL, {hdrTL.x + w, hdrTL.y + h});

            // Labels above each panel.
            dl->AddText({sdrTL.x + 6, sdrTL.y + 6}, IM_COL32(255, 255, 255, 220),
                        "SDR (tonemapped)");
            dl->AddText({hdrTL.x + 6, hdrTL.y + 6}, IM_COL32(255, 255, 255, 220),
                        "HDR (passthrough)");

            // Crop overlay drawn on the SDR panel only (interactive there).
            DrawCropOverlayAndInteraction(ctx, sdrTL, w, h, /*interactive=*/true);
            return;
        }

        // Single-panel layouts (SDR only, SDR-with-HDR-hold, Split).
        float w = avail.x;
        float h = avail.y;
        if (avail.x / avail.y > srcAspect) {
            w = avail.y * srcAspect;
        } else {
            h = avail.x / srcAspect;
        }
        const UINT renderW = std::max(1u, UINT(w));
        const UINT renderH = std::max(1u, UINT(h));
        ctx.tonemap.ResizeTarget(renderW, renderH);
        ctx.tonemap.RenderSdr(ctx.params);
        const bool needHdr =
            ctx.viewMode == EditorContext::ViewMode::Split || ctx.holdHdr;
        if (needHdr) ctx.tonemap.RenderHdrPassthrough();

        ImVec2 cursor = ImGui::GetCursorPos();
        cursor.x += (avail.x - w) * 0.5f;
        cursor.y += (avail.y - h) * 0.5f;
        ImGui::SetCursorPos(cursor);
        const ImVec2 imgTL = ImGui::GetCursorScreenPos();
        const ImVec2 imgBR{imgTL.x + w, imgTL.y + h};

        auto* dl = ImGui::GetWindowDrawList();

        if (ctx.viewMode == EditorContext::ViewMode::SdrOnly) {
            ImTextureID tex = reinterpret_cast<ImTextureID>(
                ctx.holdHdr ? ctx.tonemap.GetHdrSrv()
                            : ctx.tonemap.GetSdrSrv());
            // ImGui::Image internally calls AddImage on the current window's
            // draw list, so wrapping it with our callbacks on the same draw
            // list keeps the ordering correct.
            const bool wrapLinear =
                ctx.holdHdr && ctx.hdrMode && ctx.tonemap.LinearHdrOutput();
            if (wrapLinear) dl->AddCallback(HdrImguiTexLinearCallback, &ctx);
            ImGui::Image(tex, ImVec2(w, h));
            if (wrapLinear) dl->AddCallback(HdrImguiTexSrgbCallback, &ctx);
            if (ctx.holdHdr) {
                dl->AddText({imgTL.x + 8, imgTL.y + 6},
                            IM_COL32(255, 230, 130, 240),
                            "HDR (passthrough) - release to return to SDR");
            }
            DrawCropOverlayAndInteraction(ctx, imgTL, w, h,
                                          /*interactive=*/true);
        } else if (ctx.viewMode == EditorContext::ViewMode::Split) {
            // Reserve the layout cell.
            ImGui::Dummy(ImVec2(w, h));

            const float splitX = imgTL.x + w * ctx.splitPos;
            // SDR on the left half, HDR on the right.
            dl->AddImage(
                reinterpret_cast<ImTextureID>(ctx.tonemap.GetSdrSrv()),
                imgTL, {splitX, imgBR.y},
                {0, 0}, {ctx.splitPos, 1});
            AddHdrPassthroughImage(
                dl, ctx,
                reinterpret_cast<ImTextureID>(ctx.tonemap.GetHdrSrv()),
                {splitX, imgTL.y}, imgBR,
                {ctx.splitPos, 0}, {1, 1});
            dl->AddLine({splitX, imgTL.y}, {splitX, imgBR.y},
                        IM_COL32(255, 255, 255, 220), 2.0f);
            dl->AddText({imgTL.x + 8, imgTL.y + 6},
                        IM_COL32(255, 255, 255, 220), "SDR");
            dl->AddText({imgBR.x - 60, imgTL.y + 6},
                        IM_COL32(255, 255, 255, 220), "HDR");

            // Draggable handle on the divider.
            ImGui::SetCursorScreenPos({splitX - 8, imgTL.y});
            ImGui::InvisibleButton("##splitHandle", ImVec2(16, h));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive()) {
                const float mx = ImGui::GetIO().MousePos.x;
                ctx.splitPos =
                    std::clamp((mx - imgTL.x) / w, 0.02f, 0.98f);
            }

            // Crop overlay across the whole image, non-interactive in split.
            DrawCropOverlayAndInteraction(ctx, imgTL, w, h,
                                          /*interactive=*/false);
        }
    }
    ImGui::EndChild();
}

}  // namespace

EditorResult RunEditor(const Frame& source, const AppSettings& settings,
                       const std::wstring& defaultSavePath) {
    EditorContext ctx;
    ctx.source = &source;
    ctx.defaultSavePath = defaultSavePath;
    ctx.outputFolder = settings.outputFolder;
    ctx.result.updatedSettings = settings;
    ctx.params = settings.tonemap;
    ctx.cropX = 0;
    ctx.cropY = 0;
    ctx.cropW = int(source.width);
    ctx.cropH = int(source.height);
    ctx.resizeW = int(source.width);
    ctx.resizeH = int(source.height);
    ctx.aspect = float(source.width) / float(std::max(1u, source.height));
    ctx.saveHdrJxr = settings.saveHdrJxr;
    ctx.presetNames = ListPresets();

    EnsureClassRegistered();

    const int initW = std::min(1600, int(source.width) + 360);
    const int initH = std::min(1000, int(source.height) + 80);
    ctx.clientW = initW;
    ctx.clientH = initH;

    ctx.hwnd = CreateWindowExW(
        0, kClassName, L"Sundial - HDR to SDR Editor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, initW, initH,
        nullptr, nullptr, GetModuleHandleW(nullptr), &ctx);
    if (!ctx.hwnd) return ctx.result;

    if (!CreateDeviceAndSwapChain(ctx)) {
        DestroyWindow(ctx.hwnd);
        return ctx.result;
    }
    CreateBackbufferRtv(ctx);

    if (!ctx.tonemap.Initialize(ctx.device.Get(), ctx.context.Get(),
                                /*linearHdrOutput=*/ctx.hdrMode) ||
        !ctx.tonemap.SetSource(source)) {
        DestroyWindow(ctx.hwnd);
        return ctx.result;
    }

    if (ctx.hdrMode && !ctx.hdrPipeline.Init(ctx.device.Get())) {
        // Fall back to SDR rendering if we can't build the custom shader.
        ctx.hdrMode = false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't litter %CWD% with imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(ctx.hwnd);
    ImGui_ImplDX11_Init(ctx.device.Get(), ctx.context.Get());

    ShowWindow(ctx.hwnd, SW_SHOW);
    UpdateWindow(ctx.hwnd);

    MSG msg{};
    while (!ctx.exitRequested) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) ctx.exitRequested = true;
        }
        if (ctx.exitRequested) break;
        if (ctx.resizeRequested) {
            ResizeSwapChain(ctx);
            ctx.resizeRequested = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // In HDR mode the backbuffer is FP16 scRGB - install our custom
        // pixel shader at the very start of the draw list so every ImGui
        // draw lands on the backbuffer with the correct sRGB->linear math.
        if (ctx.hdrMode) {
            ctx.hdrPipeline.lastTexIsLinear = 0xFFFFFFFFu;
            ImGui::GetBackgroundDrawList()->AddCallback(HdrImguiBeginCallback,
                                                       &ctx);
        }

        // One root window filling the client area.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("Editor", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);

        // Global keyboard shortcuts. We suppress them while a text input is
        // active so Ctrl+X / Ctrl+S in the preset name field still do the
        // expected cut / native-input behavior.
        if (!io.WantTextInput) {
            const bool ctrl = io.KeyCtrl;
            const bool shift = io.KeyShift;
            const bool s = ImGui::IsKeyPressed(ImGuiKey_S, false);
            const bool x = ImGui::IsKeyPressed(ImGuiKey_X, false);
            if (ctrl && !shift && s) {
                ctx.result.outputPath =
                    DefaultSavePath(ctx.defaultSavePath, ctx.outputFolder);
                ctx.result.saved = true;
                ctx.exitRequested = true;
            } else if (ctrl && shift && x) {
                std::wstring path = PickSaveAsPath(ctx.hwnd,
                                                   ctx.defaultSavePath,
                                                   ctx.outputFolder);
                if (!path.empty()) {
                    ctx.result.outputPath = path;
                    ctx.result.saved = true;
                    ctx.exitRequested = true;
                }
            } else if (ctrl && !shift && x) {
                ctx.result.saved = false;
                ctx.exitRequested = true;
            }
        }

        DrawSidebar(ctx);
        DrawPreview(ctx);
        ImGui::End();

        ImGui::Render();
        // Background gray. In HDR mode the backbuffer is linear scRGB, so
        // feed the linearised value (sRGB 0.12 -> linear ~0.0127); otherwise
        // it's already in the sRGB-encoded space the SDR backbuffer expects.
        const float bgSrgb[4]   = {0.12f,   0.12f,   0.12f,   1.0f};
        const float bgLinear[4] = {0.0127f, 0.0127f, 0.0127f, 1.0f};
        const float* bg = ctx.hdrMode ? bgLinear : bgSrgb;
        ID3D11RenderTargetView* rtv = ctx.backbufferRtv.Get();
        ctx.context->OMSetRenderTargets(1, &rtv, nullptr);
        ctx.context->ClearRenderTargetView(rtv, bg);
        D3D11_VIEWPORT vp11{};
        vp11.Width = float(ctx.clientW);
        vp11.Height = float(ctx.clientH);
        vp11.MaxDepth = 1.0f;
        ctx.context->RSSetViewports(1, &vp11);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        ctx.swapChain->Present(1, 0);
    }

    if (ctx.result.saved) {
        ctx.result.updatedSettings.tonemap = ctx.params;
        ctx.result.updatedSettings.saveHdrJxr = ctx.saveHdrJxr;
        ctx.result.editedFrame = ProduceEditedFrame(ctx);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(ctx.hwnd);
    return ctx.result;
}

}  // namespace sundial
