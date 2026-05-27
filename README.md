# Sundial: HDR Screen Capture Utility For Windows

Windows HDR-aware screen capture utility with support for HDR displays, including an editor to fine tune the conversion to SDR.

![Sundial HDR Editor](sundial_edit_side_by_side.png)

## Usage
Sundial.exe runs in the background and docks itself to the Windows tasks tray.
![Sundial Tray Icon](sundial_taskbar.png)

Invoke Sundial by clicking on the tray icon, or using the system level hotkey: **Win+Shift+X**

The Sundial toolbar will appear near the top of the monitor and the screen will darken. 

![Sundial Toolbar](sundial_toolbar.png)

* **Fullscreen**: capture the entire screen. To capture an area of the screen, just drag a rectangle on the screen for the area that you want to capture.
* **Edit Image...**: Select a JXR image to edit.
* **Settings**:
  * **Edit on Capture**: when set it will open the editor after a capture. Unchecked it will save the screenshot with the last used edit settings.
  * **Save HDR Image**: When set it will save the HDR JXR image, along with the PNG. When unset, it will only save the PNG.

## Build and run

```powershell
.\build.bat           # configures on first run, incrementally builds after
.\build\Release\sundial.exe
```

ImGui is fetched on first configure via `FetchContent` — needs internet for the initial configure.

If the build directory is stale (older generator cached), delete it:

```powershell
Remove-Item -Recurse -Force build
.\build.bat
```

## Runtime model

- Single instance enforced by a named mutex `SundialInstance.v1`.
- Global hotkey **Win+Shift+X** shows the toolbar (does not capture
  immediately).
- No tray icon yet — exit via Task Manager.
- Pictures path comes from `FOLDERID_Pictures`, which redirects through
  OneDrive on this user's machine. Captures actually land in
  `C:\Users\brend\OneDrive\Pictures\Sundial\`.
- Settings live at `%APPDATA%\Sundial\settings.ini` (flat `key = value`).

## Architecture (`src/`)

| File | Responsibility |
|---|---|
| `main.cpp` | wWinMain, hotkey loop, single-instance, orchestrates capture → optional editor → save → toast |
| `HdrCapture.{h,cpp}` | DXGI Desktop Duplication, primary-monitor only. Produces `Frame` (FP16 scRGB when display is in HDR mode, BGRA8 otherwise) |
| `Settings.{h,cpp}` | `AppSettings { editOnCapture, TonemapParams }`. INI-style load/save in `%APPDATA%\Sundial\settings.ini` |
| `Tonemap.{h,cpp}` | CPU tonemap (`TonemapToBgra8(frame, params)`). Used on save |
| `ShaderTonemap.{h,cpp}` | D3D11 pixel-shader version of the same tonemap for live editor preview. Must stay in sync with `Tonemap.cpp` |
| `ImageOps.{h,cpp}` | CPU crop + bilinear resize for both FP16 and BGRA8 frames |
| `Encoder.{h,cpp}` | WIC writers — JXR (`GUID_ContainerFormatWmp` + `64bppRGBAHalf`), PNG (`32bppBGRA`) |
| `Toast.{h,cpp}` | Bottom-right layered notification window. Click opens Explorer at the file. Runs on its own thread |
| `Toolbar.{h,cpp}` | Top-centre floating toolbar — Full Screen / Area / Settings. Settings is a `TrackPopupMenu` (no dialog). Dismisses on focus loss |
| `AreaSelector.{h,cpp}` | Full-screen layered overlay; drag rectangle, ESC/right-click cancels |
| `Editor.{h,cpp}` | "Edit on Capture" window — ImGui sidebar + D3D11 live preview. Tonemap sliders, crop, resize, Save/Cancel |

## Capture pipeline (HDR-relevant)

1. `IDXGIOutput5::DuplicateOutput1` requested with
   `[R16G16B16A16_FLOAT, B8G8R8A8_UNORM]` in that order — DDA returns FP16
   scRGB when the display is in HDR mode, BGRA8 otherwise.
2. `GetPrimaryOutput6()` walks every adapter/output for
   `MONITORINFOF_PRIMARY` — using `EnumAdapters(0)`/`EnumOutputs(0)`
   silently returned empty frames on this machine.
3. `AcquireNextFrame` releases-and-reacquires until
   `LastPresentTime.QuadPart != 0`, with a small attempt budget so a
   perfectly static desktop still captures. Without this, DDA's first
   resource was empty → all-black captures.
4. `Frame.isHdr == true` only when the source format came back FP16. The
   editor's `ShaderTonemap` is fed the FP16 data directly; the CPU path
   shares the same maths (must stay in sync if either changes).

## HDR → SDR knobs

Exposed in the editor and persisted in `settings.ini`:

**Curve & exposure**
- `sdrWhiteNits` — where scRGB "1.0" sits on the SDR output. Internally
  `whiteScale = 80 / sdrWhiteNits` (scRGB convention: 1.0 = 80 nits). The
  editor has an **Auto** button that picks this from the 99th-percentile
  luminance of the captured frame (`AutoSdrWhite()` in Tonemap.cpp).
- `exposureEV` — pre-tonemap exposure in stops.
- `curve` — `LinearClip | Reinhard | Aces | Hable | AgX | Neutral`. AgX is
  the Blender log-sigmoid (3x3 input/output matrices + polynomial contrast),
  Neutral is the Khronos PBR Neutral curve (gentler than ACES on
  mid-tones, used by glTF previews).
- `blackPointLift` — post-curve shadow lift; `c <- c + lift * (1 - c)`.
- `highlightRolloff` — pre-curve knee at 0.7; pulls bright values back
  before the main curve. Useful for blown-highlight HDR content.

**Color**
- `saturation` — luminance-grayscale blend, 1.0 = unchanged.
- `temperature` / `tint` — two-axis colour shift (warm/cool, green/magenta).
- `gamutCompress` — pulls colors that go out of [0,1] toward gray, instead
  of clipping. Cheap approximation; the real ACES gamut compressor is more
  involved.
- `rGain` / `gGain` / `bGain` — per-channel multipliers in scene-linear
  space (alternative to temperature/tint for white-balance tweaks).

**Detail**
- `sharpen` — unsharp-mask amount on the source (4-tap cross neighbours).
  Applied in scene-linear space *before* the curve, both on GPU preview and
  on CPU save, so the two paths match.

**Output**
- `outputGamma` — `Srgb | Gamma22 | Linear`. Linear skips the gamma encode
  entirely (PNG viewers will treat the file as sRGB and it'll look dark and
  contrasty — only useful when the file is consumed by something
  gamma-aware).

Both `Tonemap.cpp` (CPU) and `ShaderTonemap.cpp` (HLSL string literal,
compiled at runtime) implement these identically. Changing one without the
other will desync the editor preview from the saved PNG.

### Editor preview modes

- **SDR only** (default) — shows the tonemapped result. A "Hold to view HDR"
  button temporarily switches to the passthrough view (linear scRGB clipped
  to [0,1] + sRGB gamma) so the user can see where the tonemap is
  recovering highlight detail.
- **Split (SDR | HDR)** — vertical divider with a draggable handle; SDR
  left, HDR-passthrough right, same UV coordinates so the divide is
  spatially aligned.
- **Side-by-side** — two letterboxed panels with labels.

Implemented as two render targets in `ShaderTonemap`: `RenderSdr(params)`
populates the SDR SRV; `RenderHdrPassthrough()` populates the HDR one. The
editor only renders the HDR one when a comparison mode actually needs it.

## Editor flow

- Triggered when `AppSettings::editOnCapture == true` (toggled from the
  Settings popup on the toolbar).
- ImGui sidebar (~320 px) on the left, live D3D11 preview on the right.
  Preview re-renders every frame via `ShaderTonemap::Render` so slider
  changes are instant.
- Crop is shown as a dim overlay around the selection; sliders for X/Y/W/H.
  Interactive (mouse-drag) crop isn't built yet.
- Resize is width/height inputs with aspect lock.
- Save → applies crop + resize to the FP16/BGRA8 source (both go to disk),
  persists tonemap params to `settings.ini`, then writes JXR + PNG.
- Cancel → nothing is saved.

## Conventions / things to keep in mind

- `Frame.pixels` is tightly packed (no row padding): FP16 RGBA when HDR,
  BGRA8 otherwise. `bytesPerPixel` distinguishes them.
- Tonemap and shader must keep their maths identical. Currently:
  `c = max(0, src.rgb) * (80/sdrWhiteNits) * 2^EV`, apply curve, apply
  saturation, encode sRGB.
- The CMake target list at the top of `CMakeLists.txt` is the source of
  truth — adding a new `.cpp` requires editing it.
- ImGui is built as a separate static target; `target_compile_options(imgui
  PRIVATE /W0)` because ImGui sources aren't warning-clean at /W4.
- Don't pull in OneDrive vs. local-Pictures logic — `FOLDERID_Pictures`
  handles it.

## Known gaps / non-goals (right now)

- Multi-monitor area capture: AreaSelector only covers the primary monitor.
- No video capture yet.
- No HDR metadata (MaxCLL, MaxFALL, ICC) embedded in the JXR — Windows
  Photos still recognises it as HDR via the FP16 pixel format alone.
- Area capture path captures the full primary monitor then crops in CPU.
  Fine for stills, but if we add area video this becomes a hot path.

## Future tonemap ideas (not yet implemented)

- **Local tonemap (post-shot)** — operate per-region instead of globally so
  dark UIs in front of bright HDR content both stay legible. Significantly
  more involved (multi-resolution Gaussian decomposition with bilateral or
  Gaussian pyramid on the luminance channel, then recombine with the
  original chromaticity). Has to live in both `Tonemap.cpp` (CPU save) and
  `ShaderTonemap.cpp` (GPU preview), and the GPU version needs multiple
  passes through downsampled render targets to keep blur cost manageable.

If you implement any of these: add to `TonemapParams` in `Settings.h`,
implement in `Tonemap.cpp` AND `ShaderTonemap.cpp` (the maths must match
between CPU save and GPU preview), and surface in `Editor.cpp`'s sidebar
(probably under a new collapsing header).
