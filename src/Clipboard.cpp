#include "Clipboard.h"

#include <cstring>

#include "Encoder.h"
#include "Tonemap.h"

namespace sundial {
namespace {

// Allocate a movable HGLOBAL holding `header` (already filled in) followed by
// the packed pixel bytes. Returns nullptr on allocation failure.
template <typename HeaderT>
HGLOBAL MakeDib(const HeaderT& header, const std::vector<uint8_t>& bgra) {
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(HeaderT) + bgra.size());
    if (!hMem) return nullptr;
    auto* dst = static_cast<uint8_t*>(GlobalLock(hMem));
    std::memcpy(dst, &header, sizeof(HeaderT));
    std::memcpy(dst + sizeof(HeaderT), bgra.data(), bgra.size());
    GlobalUnlock(hMem);
    return hMem;
}

// Copy a byte blob into a movable HGLOBAL. Returns nullptr on failure.
HGLOBAL MakeBlob(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return nullptr;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!hMem) return nullptr;
    void* dst = GlobalLock(hMem);
    std::memcpy(dst, bytes.data(), bytes.size());
    GlobalUnlock(hMem);
    return hMem;
}

// Encode `bgra` as PNG and publish it under the "PNG" clipboard format, which
// is what Chromium/Electron apps (browsers, chat clients) read on paste. Best
// effort: a WIC failure just means we fall back to the DIB formats.
void SetPngFormat(const std::vector<uint8_t>& bgra, uint32_t width,
                  uint32_t height) {
    static const UINT cfPng = RegisterClipboardFormatW(L"PNG");
    if (!cfPng) return;
    try {
        std::vector<uint8_t> png = EncodePngToMemory(bgra.data(), width, height);
        if (HGLOBAL hPng = MakeBlob(png)) {
            if (!SetClipboardData(cfPng, hPng)) GlobalFree(hPng);
        }
    } catch (...) {
        // Leave the DIB formats as the result.
    }
}

}  // namespace

bool CopyBgra8ToClipboard(std::vector<uint8_t> bgra, uint32_t width,
                          uint32_t height, HWND owner) {
    if (bgra.size() < size_t(width) * height * 4) return false;

    // DXGI Desktop Duplication hands back BGRA8 with the alpha channel left at
    // 0; the tonemap path writes 255. Force opaque either way - a DIB with a
    // zero alpha channel pastes as fully transparent (i.e. "nothing") in apps
    // that honor it, which is the classic "copy didn't work" symptom.
    for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 0xFF;

    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();

    const uint32_t imgSize = uint32_t(bgra.size());

    // CF_DIBV5: BI_RGB (not BI_BITFIELDS - the trailing-color-mask layout is
    // ambiguous across clipboard readers and was why pasting failed). For
    // 32bpp BI_RGB the in-memory order is BGRA, matching our buffer.
    BITMAPV5HEADER v5{};
    v5.bV5Size = sizeof(BITMAPV5HEADER);
    v5.bV5Width = int(width);
    v5.bV5Height = -int(height);  // top-down rows
    v5.bV5Planes = 1;
    v5.bV5BitCount = 32;
    v5.bV5Compression = BI_RGB;
    v5.bV5SizeImage = imgSize;
    v5.bV5RedMask = 0x00FF0000;
    v5.bV5GreenMask = 0x0000FF00;
    v5.bV5BlueMask = 0x000000FF;
    v5.bV5AlphaMask = 0xFF000000;
    v5.bV5CSType = LCS_sRGB;
    v5.bV5Intent = LCS_GM_GRAPHICS;

    // CF_DIB: plain 32bpp BI_RGB header. We set this explicitly so Windows
    // doesn't synthesize one from the V5 (the synthesized version carries the
    // alpha channel and trips up legacy consumers like MS Paint / Office).
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = int(width);
    bih.biHeight = -int(height);
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgSize;

    HGLOBAL hV5 = MakeDib(v5, bgra);
    HGLOBAL hDib = MakeDib(bih, bgra);
    if (!hV5 || !hDib) {
        if (hV5) GlobalFree(hV5);
        if (hDib) GlobalFree(hDib);
        CloseClipboard();
        return false;
    }

    bool ok = SetClipboardData(CF_DIBV5, hV5) != nullptr;
    if (ok) {
        if (!SetClipboardData(CF_DIB, hDib)) GlobalFree(hDib);
        // PNG for web/Electron consumers (this is what fixed pasting into
        // chat apps / browsers, which ignore the DIB formats).
        SetPngFormat(bgra, width, height);
    } else {
        GlobalFree(hV5);
        GlobalFree(hDib);
    }
    CloseClipboard();
    return ok;
}

bool CopyFrameToClipboard(const Frame& frame, const TonemapParams& tonemap,
                          HWND owner) {
    std::vector<uint8_t> bgra =
        frame.isHdr ? TonemapToBgra8(frame, tonemap) : frame.pixels;
    return CopyBgra8ToClipboard(std::move(bgra), frame.width, frame.height,
                                owner);
}

}  // namespace sundial
