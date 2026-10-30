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
current mode is saved as `Overlay.Input`. A fading glass notice shows the new mode.

## Windows

TPT's menus are moving into their own desktop windows (`src/OverlayWindows.cpp`): real top-level layered windows,
drawn in software with per-pixel alpha, styled like Windows Vista Aero. They don't take keyboard focus from TPT.

- **Elements** palette: menu sections down the left (Explorer navigation pane), tools as Vista buttons, the hovered
  tool's description in the status bar. Left click = primary tool (red outline), right click = secondary (blue).
  Drag by the title bar; the position is saved (`Overlay.Windows.Palette.X/Y`). Close or Ctrl+Alt+E
  (`Overlay.Keys.Palette`) hides and shows it. In Pass-through mode it fades and lets clicks through.
- **Minimise** and close both hide a window to the tray icon.
- **Tray icon** (notification area, bottom right): left click shows or hides the Elements window; right click opens
  a menu with the Elements window, the three input modes and Exit.
- UI scale: `Overlay.UIScale` (default: screen height / 1080, in quarter steps).

## To-do

- [x] Overlay window: borderless, always on top, see-through background (2026-10-30)
- [x] Per-pixel alpha presentation (replaces the colour key; black particles still read as empty)
- [x] Input modes Auto / Pass-through / Locked with global hotkeys, saved in prefs
- [x] On-screen feedback when the mode changes (a fading glass notice)
- [ ] A settings window for the overlay options and hotkeys (instead of editing powder.pref)
- [ ] Menu access: show or hide the menu bars with a hotkey; optional auto-show when the cursor nears an edge
- [x] Element palette as its own Vista window (2026-10-30)
- [ ] Tool icons (walls, tools) on the palette buttons instead of colour swatches
- [x] Minimise button and a tray icon with a menu (2026-10-30)
- [ ] Vista-themed tray menu (it uses the native Windows 11 menu for now)
- [ ] Kingsway-style taskbar / start button to open every window; then more windows: tools and brush, render and
      display modes, simulation options, save browser, stamps
- [ ] Hide TPT's own menu bars in overlay mode once the windows cover them
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

- **Menu inspiration: Kingsway** (2017), whose whole UI is a fake retro desktop. Every TPT panel would be its own
  draggable window: element palette, tools, render modes, settings, save browser, stamps. Windows can stack and
  close, a start-button style launcher and taskbar reopen them, and items can be dragged between windows (e.g. an
  element from the palette onto a quick-bar window). Combine with the Vista theme first, then other OS themes.
- Smart menu behaviour: menus fade when unused, then come back on hover or a hotkey.
- Per-app rules: pass-through automatically while a game or full-screen app is focused.

## Related

The Rust/Bevy port (`../src`, `../tools/gen_elements.py` in the local workspace) continues alongside it as the
longer-term engine: TPT's movement core is ported, and all 195 elements' properties are generated from upstream.
