#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sundial {

// Pop a small notification in the bottom-right corner of the primary
// monitor. Auto-dismisses after `durationMs`. Runs on its own worker thread
// so it never blocks the caller. If `openOnClickPath` is non-empty, clicking
// the toast opens Explorer with that file selected and dismisses the toast.
//
// `previewBgra` is an optional BGRA8 thumbnail drawn on the left of the
// toast. Pass already-resized data (width/height are the dimensions of the
// thumbnail itself, not a max box). Empty vector = no thumbnail; the layout
// collapses the thumbnail strip in that case.
void ShowToast(const std::wstring& title,
               const std::wstring& body,
               const std::wstring& openOnClickPath = L"",
               std::vector<uint8_t> previewBgra = {},
               uint32_t previewWidth = 0,
               uint32_t previewHeight = 0,
               int durationMs = 2800);

}  // namespace sundial
