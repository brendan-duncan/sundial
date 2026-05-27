#pragma once
#include <string>

namespace sundial {

// Pop a small notification in the bottom-right corner of the primary
// monitor. Auto-dismisses after `durationMs`. Runs on its own worker thread
// so it never blocks the caller. If `openOnClickPath` is non-empty, clicking
// the toast opens Explorer with that file selected and dismisses the toast.
void ShowToast(const std::wstring& title,
               const std::wstring& body,
               const std::wstring& openOnClickPath = L"",
               int durationMs = 2800);

}  // namespace sundial
