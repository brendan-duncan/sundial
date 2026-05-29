---
name: hdr-video-recording
description: Sundial gained an HDR->SDR area/full-screen video recording prototype (Win+Shift+R)
metadata:
  type: project
---

As of 2026-05-28, Sundial has a video-recording prototype (`VideoRecorder.cpp/.h`)
on top of the existing screenshot pipeline. It records the primary monitor (or a
chosen region) to H.264/MP4 via Media Foundation, reusing `ShaderTonemap` for the
same HDR->SDR conversion screenshots use.

UI lives in the existing `Toolbar.cpp` (Snipping-Tool-style): the Win+Shift+X
toolbar has a **Record** button that enters a video mode with a persistent,
handle-adjustable selection rectangle, a floating control bar (Start/Stop +
elapsed timer), and a 3-second countdown painted in the rect center before
recording. The whole flow runs inside `ShowToolbar`, which returns
`Kind::VideoRecorded` + `videoPath`. (An earlier `Win+Shift+R` global-hotkey
approach was removed because it collides with the Windows Snipping Tool.)

**Why:** User asked for Snipping-Tool-style cropped-area screen recording with the
HDR->SDR processing already built for stills, driven from the existing toolbar.

**How to apply:** It's a prototype, not production. Known follow-ups: per-frame
GPU->CPU readback (not a pure-GPU path); primary monitor only (inherits
`HdrCapture` limitation); no audio; no on-screen REC indicator; `Start()` reports
encoder-setup failures only on `Stop()`; `DXGI_ERROR_ACCESS_LOST` (HDR/mode
toggle mid-record) stops cleanly instead of re-establishing duplication. Optional
HDR-preserving output (HEVC Main10 + HDR10 metadata) was deliberately deferred.
Capture is in `HdrCapture.cpp`, tonemap in `ShaderTonemap.cpp`.
