#include "Updater.h"

#ifdef SUNDIAL_HAS_UPDATER

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "Velopack.hpp"

namespace sundial {
namespace {

// The GitHub repository releases are published to. We use Velopack's GithubSource
// (which queries the GitHub releases API) rather than a plain
// "releases/latest/download" URL: that static path makes Velopack treat the
// location as a simple file feed and fetch "<url>/releases.<channel>.json",
// which GitHub serves a 404 for - surfacing to the user as the misleading
// "couldn't reach the update server". GithubSource resolves the latest release
// and its assets through the API, matching how `vpk upload github` publishes.
constexpr char kRepoUrl[] = "https://github.com/brendan-duncan/sundial";

// An update source backed by the GitHub releases API. Constructing it does no
// network I/O on its own; the feed is only fetched when the owning manager is
// asked to check for updates.
std::unique_ptr<Velopack::GithubSource> MakeSource() {
    return std::make_unique<Velopack::GithubSource>(kRepoUrl);
}

// Transient connectivity failures (Velopack surfaces these as messages like
// "Network(Http(StatusCode(404)))", connection/DNS/TLS/timeout errors) deserve
// a calm "try again later" rather than the raw, alarming error string. The 404
// case is real and common: GitHub briefly serves a release before all of its
// assets finish uploading, so a check that races a publish hits it.
bool IsTransientNetworkError(const std::exception& e) {
    std::string msg = e.what();
    std::transform(msg.begin(), msg.end(), msg.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const char* needle : {"network", "http", "statuscode", "timeout",
                               "timed out", "connect", "dns", "tls", "ssl",
                               "socket"}) {
        if (msg.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void DoCheck(bool silent, unsigned long mainThreadId) {
    try {
        Velopack::UpdateManager manager(MakeSource());

        // The check + download can hit a transient network blip (including the
        // publish-race 404 above), so make one quiet retry before bothering the
        // user. CheckForUpdates() returns nullopt only when already up to date.
        std::optional<Velopack::UpdateInfo> updInfo;
        for (int attempt = 0;; ++attempt) {
            try {
                updInfo = manager.CheckForUpdates();
                if (!updInfo.has_value()) {
                    if (!silent) {
                        MessageBoxW(nullptr, L"Sundial is up to date.",
                                    L"Sundial", MB_ICONINFORMATION);
                    }
                    return;
                }
                manager.DownloadUpdates(updInfo.value());
                break;  // check + download both succeeded
            } catch (const std::exception& e) {
                if (attempt == 0 && IsTransientNetworkError(e)) {
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    continue;
                }
                throw;  // not transient, or the retry also failed
            }
        }

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
            if (IsTransientNetworkError(e)) {
                MessageBoxW(nullptr,
                            L"Couldn't reach the update server.\n"
                            L"Please check your connection and try again later.",
                            L"Sundial", MB_ICONINFORMATION);
            } else {
                MessageBoxA(nullptr, e.what(), "Sundial update failed",
                            MB_ICONWARNING);
            }
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
