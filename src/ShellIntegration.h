#pragma once
#include <string>

namespace sundial {

// Register an HKCU shell verb so right-clicking a .jxr file in Explorer
// shows "Open with Sundial". Idempotent - safe to call on every launch.
// Path should be the absolute path to the currently running sundial.exe.
// Windows 11 shows third-party verbs under "Show more options".
void RegisterJxrAssociation(const std::wstring& exePath);

}  // namespace sundial
