#pragma once
#include <Windows.h>

#include "Settings.h"

namespace sundial {

// Draws the full Sundial settings UI - snapshot formats, edit-on-capture,
// auto-copy, output folder, and the global capture hotkey - editing `s` in
// place. Shared by the editor's Settings popup and the standalone settings
// dialog so the two stay in lockstep.
//
//   owner          parents the "choose folder" picker.
//   captureIsHdr   when non-null, adds the editor's "this capture is SDR, only
//                  PNG will be written" hint (point it at the frame's isHdr).
//                  Pass nullptr when there's no specific capture (the dialog).
//   capturingHotkey  multi-frame key-capture state, owned by the caller.
//   hotkeyChanged    set to true when the hotkey combo changed this frame.
//
// Returns true if any setting changed this frame (caller persists + reacts).
// Must be called between ImGui::Begin/BeginPopup and the matching End.
bool DrawSettingsControls(AppSettings& s, HWND owner, const bool* captureIsHdr,
                          bool& capturingHotkey, bool& hotkeyChanged);

// Runs the standalone settings dialog: a small modal ImGui window (its own
// device + message loop) that edits `settings` in place, persists every change
// to settings.ini, and - when the capture hotkey changes - asks the resident
// instance to re-register it. Returns when the user closes the dialog. Used by
// the capture toolbar's Settings button and the tray icon's Settings item.
void ShowSettingsDialog(HWND owner, AppSettings& settings);

}  // namespace sundial
