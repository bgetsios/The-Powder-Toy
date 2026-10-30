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
- Buttons show TPT's own icons (walls, tools) where it has them, colour swatches otherwise.
- **Minimise** and close both hide a window to the tray icon.
- While the Elements window is open, TPT's own side bar and tool strip are hidden (see-through, click-through);
  the bottom button row stays. The tray menu's "Show TPT's own menus" brings them back (`Overlay.ShowTPTMenus`).
- **Tray icon** (notification area, bottom right): a plain Windows menu on purpose. It holds settings and opens
  the Vista windows. Left click shows or hides the Elements window; right click: Elements window, the three input
  modes, Show TPT's own menus, Exit. Removed on exit and on a crash, re-added if Explorer restarts. A forced kill
  (Task Manager) can't be caught; Windows clears that leftover icon when you hover it.
- UI scale: `Overlay.UIScale` (default: the larger of Windows display scaling and screen height / 1080, in
  quarter steps).
- Black particles stay visible and clickable (the sim's particle map decides, not the colour). Not inside the
  zoom window yet.

## To-do

Done (2026-10-30): overlay window; per-pixel alpha; input modes and global hotkeys; mode notices; Elements window
(Vista) with icons, minimise and close; tray icon and menu; TPT's bars hidden while the Elements window is open;
black particles solid; DPI-aware UI scale.

Assessment, in the order I'd do it (effort: S = an hour or two, M = a session, L = several sessions):

1. **One large Vista side window with tabs for all of TPT's menus** (M per tab), docked to a screen edge, with
   tabs that can be torn off into their own windows (the Kingsway feel). The Elements window becomes its first
   tab. Then:
   - [ ] Tools and brush: brush shape and size, replace mode, deco colour picker
   - [ ] Display: render and display modes (TPT's render options)
   - [ ] Simulation: pause/step, gravity, air, heat, ambient heat, edge mode, clear
   - [ ] Saves and stamps: open and save local saves, stamp library
   - [ ] Overlay settings: input mode, hotkeys (record a key instead of editing powder.pref), UI scale
   - [ ] Once these cover the bottom button row, hide it too
2. [ ] **Full-screen sim** (M): pick RES at build time (a meson option sized to the screen at the chosen pixel
   size). The windows already scale on their own, so the old "separate UI scale" item is no longer needed. RES at
   runtime (every static XRES/YRES array) stays a later L job, only if switching screens without a rebuild matters.
   Caveat: saves bigger than 612x384 won't open in stock TPT.
3. [ ] **Particles collide with open windows** (M, needs 2): the visible windows' rectangles as walls, refreshed a
   few times a second. Then react to the active window (e.g. it shakes off sand when moved). Desktop icons (S-M,
   read from Explorer's icon list view). Files: undecided, needs a concrete idea first.
4. [ ] **Auto pass-through while a game or full-screen app has focus** (S).
5. [ ] **More OS themes** (M each): XP, 7, 10/11, classic Mac OS, macOS, Linux. Start by turning the `vista::`
   drawing into a theme interface when the second theme starts. Theme choice in Overlay settings.
6. [ ] Black particles inside the zoom window (S); menus fade when unused (S).
7. [ ] Other platforms: X11 / Wayland input regions, macOS (L; only if someone needs them).

Dropped: a Kingsway-style taskbar and start button. The tray icon plus the side window cover launching. Vista
tray menu: the tray menu stays native on purpose.

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
