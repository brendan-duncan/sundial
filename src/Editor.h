#pragma once
#include "HdrCapture.h"
#include "Settings.h"

#include <string>

namespace sundial {

struct EditorResult {
    bool saved = false;
    AppSettings updatedSettings;  // tonemap params possibly changed
    Frame editedFrame;            // crop + resize already applied
    std::wstring outputPath;      // full path of the PNG chosen via Save As
};

// Modal editor: blocks until the user clicks Save or Cancel (or closes the
// window). Returns the cropped/resized frame plus the (potentially modified)
// tonemap params and the destination path chosen by the user.
// If saved == false the caller should not write any files.
//
// `defaultSavePath` is the path the Save button writes to (overwrites in
// place) and the path the Save As dialog defaults to. Pass an empty string
// for a fresh capture - Save will then use Pictures\Sundial\sundial_<ts>.png.
//
// When `tonemapOnly` is true the editor runs as a look-picker: no file is
// produced, the crop/resize controls are hidden, and the output buttons become
// "Use these settings" / "Cancel". On confirm, `saved` is true and only
// `updatedSettings.tonemap` is meaningful (no editedFrame/outputPath). Used by
// the video recorder to dial in the HDR->SDR look before recording bakes it in.
EditorResult RunEditor(const Frame& source, const AppSettings& settings,
                       const std::wstring& defaultSavePath = L"",
                       bool tonemapOnly = false);

}  // namespace sundial
