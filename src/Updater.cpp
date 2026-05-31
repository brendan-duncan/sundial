#include "Updater.h"

#ifdef SUNDIAL_HAS_UPDATER

#include <Windows.h>

#include <exception>
#include <string>
#include <thread>

#include "Velopack.hpp"

namespace sundial {
namespace {

// Where releases are published. Velopack reads the release feed and packages
// from this base URL. GitHub's "releases/latest/download" path always points
// at the newest release's assets, which is exactly what the updater needs.
//
constexpr char kUpdateUrl[] =
    "https://github.com/brendan-duncan/sundial/releases/latest/download";

void DoCheck(bool silent, unsigned long mainThreadId) {
    try {
        Velopack::UpdateManager manager(kUpdateUrl);

        auto updInfo = manager.CheckForUpdates();
        if (!updInfo.has_value()) {
            if (!silent) {
                MessageBoxW(nullptr, L"Sundial is up to date.", L"Sundial",
                            MB_ICONINFORMATION);
            }
            return;
        }

        manager.DownloadUpdates(updInfo.value());

        const wchar_t* msg =
            L"A new version of Sundial is available and has been downloaded.\n"
            L"Restart now to update?";
        if (MessageBoxW(nullptr, msg, L"Sundial update",
                        MB_ICONQUESTION | MB_YESNO) != IDYES) {
            return;
        }

        // Stage the update: Velopack launches a helper that waits for this
        // process to exit, then swaps in the new version and relaunches.
        manager.WaitExitThenApplyUpdates(updInfo.value());

        // Exit cleanly so the helper can proceed. Posting WM_QUIT lets the
        // normal shutdown (tray icon, mutex, COM) run on the GUI thread.
        if (mainThreadId != 0) {
            PostThreadMessageW(mainThreadId, WM_QUIT, 0, 0);
        } else {
            ExitProcess(0);
        }
    } catch (const std::exception& e) {
        if (!silent) {
            MessageBoxA(nullptr, e.what(), "Sundial update failed",
                        MB_ICONWARNING);
        }
    } catch (...) {
        if (!silent) {
            MessageBoxW(nullptr, L"Update check failed.",
                        L"Sundial update failed", MB_ICONWARNING);
        }
    }
}

}  // namespace

void InitUpdater() {
    // Handles Velopack's install/update/uninstall hooks and exits the process
    // in those cases. A no-op during a normal launch.
    Velopack::VelopackApp::Build().Run();
}

void CheckForUpdatesInBackground(bool silent, unsigned long mainThreadId) {
    std::thread(DoCheck, silent, mainThreadId).detach();
}

}  // namespace sundial

#else  // !SUNDIAL_HAS_UPDATER

namespace sundial {
void InitUpdater() {}
void CheckForUpdatesInBackground(bool, unsigned long) {}
}  // namespace sundial

#endif  // SUNDIAL_HAS_UPDATER
