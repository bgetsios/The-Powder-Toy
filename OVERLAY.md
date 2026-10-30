# Desktop overlay mode (experimental)

The to-do list and ideas for the overlay live here. Keep it current.

`powder.exe overlay:on` runs TPT as a borderless, always-on-top window over the desktop. Empty (black) pixels
are see-through. TPT presents its own frame with per-pixel alpha (`UpdateLayeredWindow`, `src/Overlay.cpp`), so
clicks on see-through pixels reach the desktop and windows behind. Windows only for now.

## Input modes

| Mode | Empty space | Particles and menus | Hotkey (default) |
|---|---|---|---|
| Auto | clicks go to the desktop | clicks go to TPT | Ctrl+Alt+O cycles modes |
| Pass-through | desktop | desktop (TPT takes no input) | Ctrl+Alt+P toggles |
| Locked | TPT (alpha 1: invisible but hit-testable) | TPT | Ctrl+Alt+L toggles |

The hotkeys are global (polled each frame, so they work while TPT is unfocused) and customisable in
`%APPDATA%\The Powder Toy\powder.pref`: `Overlay.Keys.CycleInput`, `Overlay.Keys.PassThrough` and
`Overlay.Keys.Locked`, as text like `"Ctrl+Alt+O"` (Ctrl, Alt, Shift, Win plus a letter, digit or F1-F24). The
current mode is saved as `Overlay.Input`.

## To-do

- [x] Overlay window: borderless, always on top, see-through background (2026-10-30)
- [x] Per-pixel alpha presentation (replaces the colour key; black particles still read as empty)
- [x] Input modes Auto / Pass-through / Locked with global hotkeys, saved in prefs
- [ ] On-screen feedback when the mode changes (a small toast)
- [ ] A settings window for the overlay options and hotkeys (instead of editing powder.pref)
- [ ] Menu access: show or hide the menu bars with a hotkey; optional auto-show when the cursor nears an edge
- [ ] **Menus in their own windows** (element palette, tools, options, ...), movable and dockable, scaling with
      the UI. Styled like **Windows Vista** (Aero glass title bars, gradients, glow buttons). Windows 11 has no Aero
      glass, so they're custom-drawn through a theme (skin) system rather than native controls.
- [ ] More OS themes for all windows: XP, 7, 10/11, classic Mac OS, macOS, Linux desktops; switchable in settings
- [ ] **Scaling to any resolution**: the sim covers the whole screen at a chosen pixel size, and the UI scales
      separately. TPT's sim size (RES 612x384) and window size are compile-time constants, and the UI and sim share
      one buffer at one integer scale.
  - [ ] a. RES picked at build time (meson option), so the sim fills the screen
  - [ ] b. RES at runtime: every static array sized by XRES/YRES/CELL changes (a larger refactor)
  - [ ] c. A separate UI scale: the UI draws into its own buffer, composited over the sim
- [ ] **Desktop interaction**: particles collide with and pile on open windows (window rectangles as walls,
      updated live), react to the active window, maybe desktop files and icons
- [ ] Other platforms: X11 / Wayland input regions, macOS

## Ideas

- Smart menu behaviour: menus fade when unused, then come back on hover or a hotkey.
- Per-app rules: pass-through automatically while a game or full-screen app is focused.

## Related

The Rust/Bevy port (`../src`, `../tools/gen_elements.py` in the local workspace) continues alongside it as the
longer-term engine: TPT's movement core is ported, and all 195 elements' properties are generated from upstream.
