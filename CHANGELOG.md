# 0.4.0

* Snipping-Tool-style default: captures no longer open the editor. They save
  with the current conversion settings, copy the SDR image to the clipboard,
  and show a preview toast. Clicking the toast preview opens the editor.
* Clipboard copy is on by default and now pastes correctly everywhere — the
  copied image is the SDR result with opaque alpha, published as `CF_DIBV5`,
  `CF_DIB`, and a real `PNG` blob (fixes blank/transparent paste in Paint and
  Office, and empty/invalid paste into browsers and Electron chat apps).
* **Edit on Capture** is now off by default (turn it on to jump straight into
  the editor on every capture).

# 0.3.0

* Toast will now include a preview of the screenshot it saved.
* Click on toast will take you to the save folder.
