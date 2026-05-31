# 1.0.0

* Installer and automatic updates via [Velopack](https://velopack.io). Sundial
  now ships as a `Setup.exe` that installs per-user (no admin prompt). Installed
  copies check for a newer release on launch, download it in the background, and
  offer to restart into it.
* Manual **Check for Updates…** in the tray menu and the toolbar **Settings**
  menu.
* New releases are built and published automatically by a GitHub Action when a
  version tag is pushed.
* **About Sundial…** item (tray and toolbar Settings menus) showing the current
  version and the project page link.
* Imrpove visual quality of the SDR image display.

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
