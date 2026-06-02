#include <Windows.h>
#include <ShellScalingApi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <string>

#include "About.h"
#include "Clipboard.h"
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
#include "Updater.h"

#include <functional>
#include <vector>

namespace {
// Posted by a screenshot toast (from its own thread) back to the main thread
// when the user clicks the preview, asking the main loop to open the editor
// on the saved file. LPARAM is a heap wchar_t* path the handler frees.
constexpr UINT kMsgEditFile = WM_APP + 1;
DWORD g_mainThreadId = 0;
}  // namespace

namespace {

constexpr int kHotkeyToolbar = 1;

// Max bounding box for the toast's screenshot preview. Kept in sync with the
// thumbnail container size in Toast.cpp - we letterbox the source into this
// box (preserving aspect) and ship the already-resized BGRA8 to the toast.
constexpr uint32_t kToastThumbMaxW = 128;
constexpr uint32_t kToastThumbMaxH = 90;

void ComputeThumbSize(uint32_t srcW, uint32_t srcH,
                      uint32_t& outW, uint32_t& outH) {
    if (srcW == 0 || srcH == 0) {
        outW = outH = 0;
        return;
    }
    const double aspect = double(srcW) / double(srcH);
    const double boxAspect = double(kToastThumbMaxW) / double(kToastThumbMaxH);
    if (aspect > boxAspect) {
        outW = kToastThumbMaxW;
        outH = std::max(1u,
                        uint32_t(double(kToastThumbMaxW) / aspect + 0.5));
    } else {
        outH = kToastThumbMaxH;
        outW = std::max(1u,
                        uint32_t(double(kToastThumbMaxH) * aspect + 0.5));
    }
}

// Produce a BGRA8 thumbnail of the saved frame for the toast. Uses the same
// tonemap the PNG was written with, so the preview matches the file on disk.
// Returns an empty vector if the frame is degenerate (which we then fall
// back to a text-only toast for).
std::vector<uint8_t> MakeToastThumbnail(const sundial::Frame& frame,
                                        const sundial::TonemapParams& tonemap,
                                        uint32_t& outW, uint32_t& outH) {
    ComputeThumbSize(frame.width, frame.height, outW, outH);
    if (outW == 0 || outH == 0) return {};
    // `small` is a Windows COM IDL typedef (rpcndr.h) - avoid that name.
    sundial::Frame thumb = sundial::Resize(frame, outW, outH);
    if (thumb.isHdr) {
        return sundial::TonemapToBgra8(thumb, tonemap);
    }
    return std::move(thumb.pixels);
}

std::wstring Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

// Defined below; declared here so SaveAndNotify's toast can re-open a saved
// file in a fresh editor process, and HandleCapture can hand a capture off to
// a child editor process.
std::wstring GetExePath();
void SpawnEditorForFile(const std::wstring& path);
void SpawnEditorForFrame(const sundial::Frame& frame,
                         const sundial::AppSettings& settings);

// How a target path was chosen, which decides whether the snapshot format set
// applies. Snapshot = the implicit Save / capture target (a .png base path that
// fans out into every enabled format). ExplicitFile = a single file the user
// picked via Save As, written verbatim.
enum class SaveMode { Snapshot, ExplicitFile };

// `inChildProcess` is true when called from a short-lived editor process: the
// save toast then runs on the calling thread (so the process stays alive to
// show it) and clicking it spawns a new editor process. In the resident tray
// process it's false: the toast is async and clicking marshals to the main
// thread's editor.
void SaveAndNotify(const sundial::Frame& frame,
                   const sundial::TonemapParams& tonemap,
                   const sundial::SnapshotFormats& formats,
                   SaveMode mode,
                   const std::wstring& pngPath,
                   bool inChildProcess) {
    std::filesystem::path png = pngPath;
    const std::wstring stem = png.stem().wstring();
    const std::wstring dir = png.parent_path().wstring();

    std::wstring title;
    std::wstring selectInExplorer;
    std::wstring extraNote;

    // Save As targets exactly one file. Ultra HDR (.jpg) and HDR AVIF (.avif)
    // are only meaningful for HDR sources; from an SDR frame they fall through
    // to a plain PNG.
    const std::wstring ext = png.extension().wstring();
    const bool explicitUltraHdr =
        frame.isHdr && (_wcsicmp(ext.c_str(), L".jpg") == 0 ||
                        _wcsicmp(ext.c_str(), L".jpeg") == 0);
    const bool explicitAvif =
        frame.isHdr && _wcsicmp(ext.c_str(), L".avif") == 0;

    if (mode == SaveMode::ExplicitFile && explicitUltraHdr) {
        sundial::SaveUltraHdrJpeg(frame, tonemap, png.wstring());
        selectInExplorer = png.wstring();
        title = L"Ultra HDR saved  -  " + png.filename().wstring();
    } else if (mode == SaveMode::ExplicitFile && explicitAvif) {
        sundial::SaveAvifHdr(frame, tonemap, png.wstring(), formats.avifMode);
        selectInExplorer = png.wstring();
        title = L"HDR AVIF saved  -  " + png.filename().wstring();
    } else if (mode == SaveMode::ExplicitFile) {
        // A single explicit PNG (SDR passthrough or tonemapped HDR).
        if (frame.isHdr) {
            sundial::SavePngTonemapped(frame, tonemap, png.wstring());
        } else {
            sundial::SavePngSdr(frame, png.wstring());
        }
        selectInExplorer = png.wstring();
        title = L"Saved  -  " + png.filename().wstring();
    } else {
        // Snapshot mode: write every enabled + applicable format off the base
        // dir + stem. PNG always applies; JXR / Ultra HDR / AVIF are HDR-only.
        // Track the richest written file (jxr > avif > png > jpg) for the
        // toast's re-edit target and the Explorer selection.
        std::filesystem::path base = png;
        const std::filesystem::path pngFile = base.replace_extension(L".png");
        std::filesystem::path jxrFile = pngFile;
        jxrFile.replace_extension(L".jxr");
        std::filesystem::path jpgFile = pngFile;
        jpgFile.replace_extension(L".jpg");
        std::filesystem::path avifFile = pngFile;
        avifFile.replace_extension(L".avif");

        std::vector<std::wstring> wrote;  // extensions written, for the title
        bool wrotePng = false;

        auto writePng = [&] {
            if (frame.isHdr) {
                sundial::SavePngTonemapped(frame, tonemap, pngFile.wstring());
            } else {
                sundial::SavePngSdr(frame, pngFile.wstring());
            }
            wrotePng = true;
            wrote.push_back(L".png");
            if (selectInExplorer.empty()) selectInExplorer = pngFile.wstring();
        };

        if (formats.jxr && frame.isHdr) {
            sundial::SaveJxrHdr(frame, jxrFile.wstring());
            wrote.push_back(L".jxr");
            selectInExplorer = jxrFile.wstring();  // richest: prefer JXR
        }
#ifdef SUNDIAL_HAS_AVIF
        if (formats.avif && frame.isHdr) {
            sundial::SaveAvifHdr(frame, tonemap, avifFile.wstring(),
                                 formats.avifMode);
            wrote.push_back(L".avif");
            if (selectInExplorer.empty()) selectInExplorer = avifFile.wstring();
        }
#endif
#ifdef SUNDIAL_HAS_ULTRAHDR
        if (formats.ultraHdrJpeg && frame.isHdr) {
            sundial::SaveUltraHdrJpeg(frame, tonemap, jpgFile.wstring());
            wrote.push_back(L".jpg");
            if (selectInExplorer.empty()) selectInExplorer = jpgFile.wstring();
        }
#endif
        if (formats.png) writePng();

        // Guarantee a snapshot always produces something. If only HDR formats
        // were enabled but the capture is SDR (or every enabled format was
        // inapplicable), fall back to the universal PNG.
        if (wrote.empty()) {
            writePng();
            extraNote = L"\n(only HDR formats enabled; saved PNG)";
        }
        (void)wrotePng;

        std::wstring list = stem;  // "name.jxr + .png + .jpg"
        for (size_t i = 0; i < wrote.size(); ++i) {
            if (i) list += L" + ";
            list += wrote[i];
        }
        const wchar_t* prefix = frame.isHdr ? L"HDR saved  -  " : L"Saved  -  ";
        title = prefix + list;
    }
    const std::wstring body =
        dir + extraNote + L"\n(click the preview to edit)";

    uint32_t thumbW = 0, thumbH = 0;
    std::vector<uint8_t> thumb = MakeToastThumbnail(frame, tonemap,
                                                    thumbW, thumbH);

    // Clicking the toast re-opens the saved file in the editor (the JXR when
    // we kept one, so re-toning starts from the full HDR data; otherwise the
    // PNG). The toast lives on its own thread, so marshal back to the main
    // thread - the editor is modal and must run there.
    const std::wstring editPath = selectInExplorer;
    if (inChildProcess) {
        // We're about to exit; host the toast on this thread so it's visible,
        // and re-open via a brand-new editor process on click.
        auto onClick = [editPath] { SpawnEditorForFile(editPath); };
        sundial::ShowToastBlocking(title, body, std::move(onClick),
                                   std::move(thumb), thumbW, thumbH);
    } else {
        auto onClick = [editPath] {
            wchar_t* heapPath = _wcsdup(editPath.c_str());
            if (!heapPath) return;
            if (!PostThreadMessageW(g_mainThreadId, kMsgEditFile, 0,
                                    reinterpret_cast<LPARAM>(heapPath))) {
                free(heapPath);
            }
        };
        sundial::ShowToast(title, body, std::move(onClick),
                           std::move(thumb), thumbW, thumbH);
    }
}

void HandleCapture(sundial::AppSettings& settings, sundial::Frame frame) {
    if (settings.editOnCapture) {
        // Hand the capture to a separate editor process so the tray stays free
        // to capture again while the editor is open (and to allow several
        // editors at once). The child seeds the tonemap from the frame itself.
        SpawnEditorForFrame(frame, settings);
    } else {
        // Direct save in the tray process - no editor, so this stays in-process.
        sundial::SeedTonemapForFrame(settings.tonemap, frame);
        const auto base =
            sundial::ResolveOutputDir(settings) / (L"sundial_" + Timestamp());
        const std::wstring png = base.wstring() + L".png";
        SaveAndNotify(frame, settings.tonemap, settings.snapshot,
                      SaveMode::Snapshot, png, /*inChildProcess=*/false);
        if (settings.autoCopyCapture) {
            sundial::CopyFrameToClipboard(frame, settings.tonemap);
        }
    }
}

void RunEditImageFlow(sundial::AppSettings& settings);
void NotifyVideoSaved(const std::wstring& videoPath);

void RunToolbarFlow(sundial::AppSettings& settings) {
    const bool prevEditOnCapture = settings.editOnCapture;
    const sundial::SnapshotFormats prevSnapshot = settings.snapshot;
    const bool prevAutoCopy = settings.autoCopyCapture;
    const std::wstring prevOutputFolder = settings.outputFolder;
    auto result = sundial::ShowToolbar(settings);
    if (settings.editOnCapture != prevEditOnCapture ||
        settings.snapshot.png != prevSnapshot.png ||
        settings.snapshot.jxr != prevSnapshot.jxr ||
        settings.snapshot.ultraHdrJpeg != prevSnapshot.ultraHdrJpeg ||
        settings.autoCopyCapture != prevAutoCopy ||
        settings.outputFolder != prevOutputFolder) {
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
        case sundial::ToolbarResult::Kind::VideoRecorded:
            NotifyVideoSaved(result.videoPath);
            break;
        case sundial::ToolbarResult::Kind::None:
        default:
            break;
    }
}

// Toast for a finished screen recording (path is the saved .mp4). Recordings
// aren't editable, so clicking opens Explorer at the file (done on the toast's
// own thread, which is fine for ShellExecute).
void NotifyVideoSaved(const std::wstring& videoPath) {
    std::filesystem::path p = videoPath;
    auto onClick = [videoPath] {
        const std::wstring args = L"/select,\"" + videoPath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                      SW_SHOWNORMAL);
    };
    sundial::ShowToast(L"Recording saved  -  " + p.filename().wstring(),
                       p.parent_path().wstring() +
                           L"\n(click to open in Explorer)",
                       std::move(onClick));
}

std::wstring GetExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

std::wstring QuoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

// Launch a detached child sundial.exe to run an editor, then return at once.
// The child takes the pre-singleton early-exit path (see wWinMain), so it never
// registers the hotkey or tray and many can run concurrently while the resident
// process keeps capturing.
void SpawnEditorChild(const std::wstring& args) {
    const std::wstring exe = GetExePath();
    std::wstring cmd = QuoteArg(exe) + L" " + args;
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    const std::wstring exeDir =
        std::filesystem::path(exe).parent_path().wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                       0, nullptr, exeDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void SpawnEditorForFile(const std::wstring& path) {
    SpawnEditorChild(L"--edit " + QuoteArg(path));
}

// %TEMP%\sundial_handoff_<pid>_<ts>_<tick>.<ext> - unique per capture so two
// snapshots in the same second don't collide.
std::wstring HandoffTempPath(const wchar_t* ext) {
    wchar_t tempDir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tempDir);
    std::filesystem::path dir = (n > 0 && n < MAX_PATH)
                                    ? std::filesystem::path(tempDir)
                                    : std::filesystem::temp_directory_path();
    std::wstring name = L"sundial_handoff_" +
                        std::to_wstring(GetCurrentProcessId()) + L"_" +
                        Timestamp() + L"_" +
                        std::to_wstring(GetTickCount64()) + ext;
    return (dir / name).wstring();
}

void SpawnEditorForFrame(const sundial::Frame& frame,
                         const sundial::AppSettings& settings) {
    // Hand the in-memory frame to the child losslessly via a temp file: JXR for
    // HDR (FP16 scRGB round-trips), PNG for SDR. The child reads it back and
    // restores the seed metadata the temp can't carry.
    const std::wstring temp = HandoffTempPath(frame.isHdr ? L".jxr" : L".png");
    try {
        if (frame.isHdr) {
            sundial::SaveJxrHdr(frame, temp);
        } else {
            sundial::SavePngSdr(frame, temp);
        }
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Sundial - couldn't open editor",
                    MB_ICONERROR);
        return;
    }

    wchar_t sdrWhite[64], maxLum[64];
    swprintf_s(sdrWhite, L"%.6f", frame.sdrWhiteLevelNits);
    swprintf_s(maxLum, L"%.6f", frame.maxLuminanceNits);
    std::wstring args = L"--edit-temp " + QuoteArg(temp) +
                        L" --seed-hdr " + (frame.isHdr ? L"1" : L"0") +
                        L" --seed-sdrwhite " + sdrWhite +
                        L" --seed-maxlum " + maxLum +
                        L" --copy " + (settings.autoCopyCapture ? L"1" : L"0");
    SpawnEditorChild(args);
}

// Delete handoff temp files older than an hour - leftovers from editor
// processes that crashed before deleting their own. The age threshold keeps us
// well clear of temps that a currently-open editor still owns.
void SweepStaleHandoffTemps() {
    wchar_t tempDir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tempDir);
    if (n == 0 || n >= MAX_PATH) return;
    std::error_code ec;
    const auto cutoff =
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    for (std::filesystem::directory_iterator it(tempDir, ec), end;
         it != end && !ec; it.increment(ec)) {
        const auto& p = it->path();
        if (p.filename().wstring().rfind(L"sundial_handoff_", 0) != 0) continue;
        std::error_code tec;
        if (std::filesystem::last_write_time(p, tec) < cutoff && !tec) {
            std::filesystem::remove(p, tec);
        }
    }
}

std::wstring PickImageToOpen(HWND owner) {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter =
        L"Images (*.jxr;*.avif;*.png;*.jpg;*.jpeg)\0"
        L"*.jxr;*.avif;*.png;*.jpg;*.jpeg\0"
        L"JPEG XR (*.jxr)\0*.jxr\0"
        L"HDR AVIF (*.avif)\0*.avif\0"
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
    // Seed tonemap defaults from the loaded image, same as on capture, so an
    // SDR PNG doesn't inherit HDR-tuned values from the previous edit
    // session (and vice versa). LoadFrameFromFile doesn't populate display
    // metadata, so AutoSdrWhite uses the file's content distribution alone.
    sundial::SeedTonemapForFrame(settings.tonemap, frame);
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
    settings.snapshot = result.updatedSettings.snapshot;
    sundial::SaveSettings(settings);
    const SaveMode mode =
        result.explicitPath ? SaveMode::ExplicitFile : SaveMode::Snapshot;
    SaveAndNotify(result.editedFrame, settings.tonemap, settings.snapshot,
                  mode, result.outputPath, /*inChildProcess=*/true);
}

// Child-process flow for a freshly-captured frame handed off via a temp file
// (see SpawnEditorForFrame). Saving writes a new timestamped snapshot to the
// resolved output dir; the temp is always deleted on the way out.
void EditCapturedTemp(sundial::AppSettings& settings,
                      const std::wstring& tempPath, bool seedHdr,
                      float seedSdrWhite, float seedMaxLum,
                      bool copyToClipboard) {
    struct TempCleanup {
        std::wstring path;
        ~TempCleanup() { DeleteFileW(path.c_str()); }
    } cleanup{tempPath};

    sundial::Frame frame;
    try {
        frame = sundial::LoadFrameFromFile(tempPath);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Sundial - couldn't open capture",
                    MB_ICONERROR);
        return;
    }
    // The temp round-trips pixels but not display metadata; restore the fields
    // SeedTonemapForFrame needs from the values the capturer passed.
    frame.isHdr = seedHdr;
    frame.sdrWhiteLevelNits = seedSdrWhite;
    frame.maxLuminanceNits = seedMaxLum;

    sundial::SeedTonemapForFrame(settings.tonemap, frame);
    auto result = sundial::RunEditor(frame, settings);  // empty save target
    if (!result.saved) return;
    settings.tonemap = result.updatedSettings.tonemap;
    settings.snapshot = result.updatedSettings.snapshot;
    sundial::SaveSettings(settings);
    // Copy before the toast: SaveAndNotify's child-process toast blocks this
    // thread until dismissed, and the clipboard should be ready immediately.
    if (copyToClipboard) {
        sundial::CopyFrameToClipboard(result.editedFrame, settings.tonemap);
    }
    const SaveMode mode =
        result.explicitPath ? SaveMode::ExplicitFile : SaveMode::Snapshot;
    SaveAndNotify(result.editedFrame, settings.tonemap, settings.snapshot,
                  mode, result.outputPath, /*inChildProcess=*/true);
}

void RunEditImageFlow(sundial::AppSettings& settings) {
    (void)settings;
    std::wstring path = PickImageToOpen(nullptr);
    if (path.empty()) return;
    // Open the file in a separate editor process so the tray keeps running.
    SpawnEditorForFile(path);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Must run before anything else: handles Velopack's install/update hooks
    // (and exits the process in those cases). A no-op on a normal launch, and
    // compiled out entirely when the updater isn't vendored.
    sundial::InitUpdater();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // CLI: editor invocations run as standalone, short-lived processes that
    // bypass the singleton / hotkey / tray, so the resident instance can keep
    // capturing while any number of editors are open.
    //   --edit <path>           edit an existing image (also a bare path, for
    //                           the "Open with Sundial" shell verb)
    //   --edit-temp <path>      edit a freshly-captured frame handed off via a
    //                           temp file; deletes the temp and saves a fresh
    //                           timestamped snapshot to the output dir
    //   --seed-hdr/-sdrwhite/-maxlum  restore the capture's display metadata
    //   --copy <0|1>            copy the edited result to the clipboard on save
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    enum class EditMode { None, File, Temp };
    EditMode editMode = EditMode::None;
    std::wstring editPath;
    bool seedHdr = false;
    float seedSdrWhite = 80.0f;
    float seedMaxLum = 80.0f;
    bool copyToClipboard = false;
    // Set when launched as part of Windows' run-on-startup (the startup entry
    // passes --startup): the app should drop straight into the tray rather than
    // flashing the toolbar over whatever the user is doing at login.
    bool startupMode = false;
    if (argv) {
        auto nextArg = [&](int& i) -> std::wstring {
            return (i + 1 < argc) ? std::wstring(argv[++i]) : std::wstring{};
        };
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (_wcsicmp(arg.c_str(), L"--startup") == 0) {
                startupMode = true;
            } else if (_wcsicmp(arg.c_str(), L"--edit") == 0) {
                editPath = nextArg(i);
                editMode = EditMode::File;
            } else if (_wcsicmp(arg.c_str(), L"--edit-temp") == 0) {
                editPath = nextArg(i);
                editMode = EditMode::Temp;
            } else if (_wcsicmp(arg.c_str(), L"--seed-hdr") == 0) {
                seedHdr = nextArg(i) == L"1";
            } else if (_wcsicmp(arg.c_str(), L"--seed-sdrwhite") == 0) {
                try { seedSdrWhite = std::stof(nextArg(i)); } catch (...) {}
            } else if (_wcsicmp(arg.c_str(), L"--seed-maxlum") == 0) {
                try { seedMaxLum = std::stof(nextArg(i)); } catch (...) {}
            } else if (_wcsicmp(arg.c_str(), L"--copy") == 0) {
                copyToClipboard = nextArg(i) == L"1";
            } else if (editMode == EditMode::None) {
                // Bare path fallback for the "Open with Sundial" shell verb.
                std::error_code ec;
                if (std::filesystem::exists(arg, ec)) {
                    editPath = arg;
                    editMode = EditMode::File;
                }
            }
        }
        LocalFree(argv);
    }

    if (editMode != EditMode::None) {
        try {
            sundial::AppSettings settings = sundial::LoadSettings();
            if (editMode == EditMode::Temp) {
                EditCapturedTemp(settings, editPath, seedHdr, seedSdrWhite,
                                 seedMaxLum, copyToClipboard);
            } else {
                EditExistingFile(settings, editPath);
            }
        } catch (const std::exception& e) {
            MessageBoxA(nullptr, e.what(), "Sundial edit failed",
                        MB_ICONERROR);
        }
        CoUninitialize();
        return 0;
    }

    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"SundialInstance.v1");
    const bool alreadyRunning =
        instanceMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning) {
        // Don't start a duplicate. A plain re-launch hands off to the resident
        // instance and asks it to pop the toolbar (so double-clicking the exe
        // behaves like clicking the tray icon). A startup launch just bows out
        // quietly - the resident instance is already in the tray.
        if (!startupMode) {
            if (HWND running = sundial::FindRunningTrayWindow()) {
                // The toolbar grabs the foreground; let the resident process do
                // so even though we (the new, foreground process) are exiting.
                DWORD pid = 0;
                GetWindowThreadProcessId(running, &pid);
                if (pid) AllowSetForegroundWindow(pid);
                PostMessageW(running, sundial::TrayShowToolbarMessage(), 0, 0);
            }
        }
        if (instanceMutex) CloseHandle(instanceMutex);
        CoUninitialize();
        return 0;
    }

    // Register / refresh the "Open with Sundial" right-click verbs (.jxr, .avif).
    sundial::RegisterImageAssociations(GetExePath());

    // If Velopack installed a run-on-startup shortcut, make sure it carries
    // --startup so login launches stay in the tray (vpk creates it argless).
    sundial::EnsureStartupShortcutArgs();

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

    g_mainThreadId = GetCurrentThreadId();
    sundial::AppSettings settings = sundial::LoadSettings();

    // Clean up any handoff temp files left behind by editor processes that
    // crashed before deleting their own (best effort).
    SweepStaleHandoffTemps();

    auto runFlow = [&] {
        // Reload first so format / edit-on-capture / output-folder changes made
        // in an editor process's Settings since the last capture take effect.
        settings = sundial::LoadSettings();
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
#ifdef SUNDIAL_HAS_UPDATER
    // Only offered when the updater is compiled in. Non-silent: reports
    // "up to date" / errors too, since the user asked explicitly.
    tray.OnCheckUpdates([] {
        sundial::CheckForUpdatesInBackground(/*silent=*/false, g_mainThreadId);
    });
#endif
    tray.OnAbout([] { sundial::ShowAbout(nullptr); });
    tray.OnExit([] { PostQuitMessage(0); });
    tray.Initialize(L"Sundial  -  Win+Shift+X");

    // Check for a newer release in the background. Silent unless an update is
    // actually downloaded (then it prompts to restart). Posts WM_QUIT to this
    // thread to exit cleanly before applying.
    sundial::CheckForUpdatesInBackground(/*silent=*/true, g_mainThreadId);

    // Launching the exe (e.g. from the Start menu) docks the tray icon above
    // and also brings up the toolbar straight away, so the app is immediately
    // usable instead of silently waiting for the Win+Shift+X hotkey. After the
    // user finishes (or dismisses) the toolbar, we drop into the tray message
    // loop and stay resident as usual. A run-on-startup launch (--startup)
    // skips this and just sits in the tray.
    if (!startupMode) {
        runFlow();
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyToolbar) {
            runFlow();
        } else if (msg.message == kMsgEditFile) {
            // A screenshot toast was clicked: open the saved file in a separate
            // editor process so the tray stays responsive. The path was
            // heap-allocated by the toast callback.
            wchar_t* heapPath = reinterpret_cast<wchar_t*>(msg.lParam);
            if (heapPath) {
                std::wstring path(heapPath);
                free(heapPath);
                SpawnEditorForFile(path);
            }
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
