#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sundial {

// Pop a small notification in the bottom-right corner of the primary
// monitor. Auto-dismisses after `durationMs`. Runs on its own worker thread
// so it never blocks the caller. If `onClick` is set, the toast is clickable
// (hand cursor); clicking invokes the callback (on the toast's own thread)
// and dismisses the toast. Use it to open the editor, open Explorer, etc. -
// marshal back to another thread inside the callback if needed.
//
// `previewBgra` is an optional BGRA8 thumbnail drawn on the left of the
// toast. Pass already-resized data (width/height are the dimensions of the
// thumbnail itself, not a max box). Empty vector = no thumbnail; the layout
// collapses the thumbnail strip in that case.
void ShowToast(const std::wstring& title,
               const std::wstring& body,
               std::function<void()> onClick = {},
               std::vector<uint8_t> previewBgra = {},
               uint32_t previewWidth = 0,
               uint32_t previewHeight = 0,
               int durationMs = 2800);

}  // namespace sundial
