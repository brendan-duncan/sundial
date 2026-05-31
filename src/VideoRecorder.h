#pragma once
#include "Settings.h"

#include <Windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace sundial {

// Continuous HDR screen-region recorder. Runs an isolated D3D11 device + DXGI
// Desktop Duplication loop on a worker thread, crops each captured frame on the
// GPU, runs it through the same HDR->SDR tonemap shader the screenshot path
// uses (ShaderTonemap), then encodes the result to H.264 / MP4 via Media
// Foundation. Output is SDR (the tonemapped result), so the file plays back
// correctly on any display.
//
// Lifetime: construct, Start(), later Stop(). One recorder records one clip;
// create a fresh instance for the next recording.
class VideoRecorder {
public:
    VideoRecorder() = default;
    ~VideoRecorder();

    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    // Begin recording the given primary-monitor region (in pixels, same
    // convention as Crop()/the toolbar's Area result) to `outputPath` (.mp4).
    // An empty/zero-area region records the full primary monitor. The tonemap
    // curve/look knobs are taken from `settings.tonemap`; per-display anchors
    // (SDR white level, source peak) are seeded from the captured display the
    // same way screenshots are. Returns false if capture/encoder setup failed.
    //
    // Pass `tonemapPreseeded = true` when `settings.tonemap` already holds a
    // display-seeded look the user dialed in (e.g. via the toolbar's "Adjust
    // look" editor): the recorder then uses those params verbatim instead of
    // re-seeding the per-display anchors. Local tonemap is still forced off
    // (the recorder has no mip chain for it).
    bool Start(const RECT& region, const std::wstring& outputPath,
               const AppSettings& settings, bool tonemapPreseeded = false);

    // Stop recording, finalize the MP4, and join the worker. Safe to call once.
    // Returns true if the file was written without a fatal error.
    bool Stop();

    bool IsRecording() const { return running_.load(); }

    // Populated when Start()/the worker hit a fatal error; empty otherwise.
    const std::string& Error() const { return error_; }

private:
    void Run(RECT region, std::wstring outputPath, AppSettings settings,
             bool tonemapPreseeded);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::string error_;
};

}  // namespace sundial
