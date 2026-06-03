#pragma once
#include "Settings.h"

#include <Windows.h>

#include <string>

namespace sundial {

struct ToolbarResult {
    enum class Kind {
        None,           // user dismissed without choosing
        FullScreen,     // captured the full primary monitor
        Area,           // captured a rectangle - see `area`
        EditImage,      // user wants to open an existing file in the editor
        VideoRecorded,  // a screen recording was made - see `videoPath`
    };
    Kind kind = Kind::None;
    RECT area{};               // valid when kind == Area, primary-monitor px
    std::wstring videoPath;    // valid when kind == VideoRecorded (the .mp4)
};

// Snipping-Tool-style modal flow: dims the primary monitor, sets a cross
// cursor, and shows a toolbar at the top. The user can drag a rectangle on
// screen (Area), click Full Screen, click Record to enter video mode, click
// Edit Image, or dismiss with a click-without-drag / ESC / right-click. The
// Settings button toggles the capture flags on the supplied settings.
//
// Record enters a self-contained video flow: the user selects a persistent,
// handle-adjustable rectangle, then Start runs a 3-second countdown and
// records the region (HDR->SDR) to an .mp4 until Stop. On success the result
// is Kind::VideoRecorded with videoPath set; the whole flow stays inside this
// call, so it only returns once recording has finished or been canceled.
ToolbarResult ShowToolbar(AppSettings& settings);

}  // namespace sundial
