#pragma once

// Auto-update support, backed by Velopack (https://velopack.io).
//
// All Velopack calls are isolated in Updater.cpp and compiled only when the
// SUNDIAL_HAS_UPDATER macro is defined (set by CMake once the velopack_libc
// library has been vendored into third_party/velopack via
// tools/setup-velopack.ps1). When the updater is not compiled in, these
// functions are no-ops, so the rest of the app never has to care whether
// updates are available.

namespace sundial {

// Must be called as the very first thing in the process entry point, before
// any other initialization. During an install / update / uninstall, Velopack
// re-invokes the exe with special hooks; this call handles those hooks and
// exits the process for us. In a normal run it returns immediately.
void InitUpdater();

// Kick off an update check on a background thread and return immediately. If a
// newer release is found it is downloaded; the user is then asked (via a
// message box) whether to restart into the new version now. On "yes", Velopack
// stages the update and we post WM_QUIT to `mainThreadId` so the normal
// shutdown path runs before the updater applies the new version and relaunches.
//
// `silent` controls only the "you're already up to date" / error feedback:
//   - silent = true  : say nothing unless an update is actually applied
//                      (use this for the automatic check on startup)
//   - silent = false : also report "no updates" / failures
//                      (use this for an explicit, user-triggered check)
//
// `mainThreadId` is the GUI thread that owns the message loop (GetCurrentThreadId
// from wWinMain). Pass 0 to fall back to a hard process exit.
void CheckForUpdatesInBackground(bool silent, unsigned long mainThreadId);

}  // namespace sundial
