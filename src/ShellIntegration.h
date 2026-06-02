#pragma once
#include <string>

namespace sundial {

// Register an HKCU shell verb so right-clicking a .jxr file in Explorer
// shows "Open with Sundial". Idempotent - safe to call on every launch.
// Path should be the absolute path to the currently running sundial.exe.
// Windows 11 shows third-party verbs under "Show more options".
void RegisterJxrAssociation(const std::wstring& exePath);

// Show the system "choose folder" dialog, seeded at `seedFolder` (if it
// exists). Returns the chosen absolute path, or an empty string if cancelled.
// `owner` may be null. Shared by the toolbar and editor settings UIs.
std::wstring PickFolderDialog(void* owner, const std::wstring& seedFolder);

// Velopack creates the run-on-startup shortcut with no arguments (vpk can't
// attach per-shortcut args), so a login launch would look identical to a
// manual one and pop the toolbar. This rewrites that shortcut - if present in
// the user's Startup folder - to pass --startup, which keeps Sundial in the
// tray at login. Idempotent (only writes when the flag is missing) and a no-op
// when no startup shortcut exists. Call on a normal resident launch.
void EnsureStartupShortcutArgs();

}  // namespace sundial
