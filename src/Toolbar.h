#pragma once
#include "Settings.h"

#include <Windows.h>

namespace sundial {

struct ToolbarResult {
    enum class Kind {
        None,         // user dismissed without choosing
        FullScreen,   // captured the full primary monitor
        Area,         // captured a rectangle - see `area`
        EditImage,    // user wants to open an existing file in the editor
    };
    Kind kind = Kind::None;
    RECT area{};      // valid when kind == Area, in primary-monitor pixels
};

// Snipping-Tool-style modal flow: dims the primary monitor, sets a cross
// cursor, and shows a toolbar at the top. The user can either drag a
// rectangle on screen (Area), click the toolbar's Full Screen button, or
// dismiss with a click-without-drag / ESC / right-click. The Settings
// button toggles the "Edit on Capture" / "Save HDR Image (JXR)" flags on
// the supplied settings.
ToolbarResult ShowToolbar(AppSettings& settings);

}  // namespace sundial
