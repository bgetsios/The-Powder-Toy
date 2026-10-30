# Desktop overlay mode (experimental)

`powder.exe overlay:on` runs TPT as a borderless, always-on-top window. The empty (pure black) background is
see-through, and clicks on it reach the desktop and windows behind. Particles and menus stay visible and clickable.
Windows only for now: a layered window with a black colour key and the software renderer (`src/Overlay.cpp`).

Status 2026-10-30: works (verified on a 5120x1440 screen). The window is TPT's normal size at the chosen scale.

## Requirements (bgetsios)

1. **Input routing, smart and customisable.** Decide when mouse input goes to TPT and when it goes to the desktop
   and windows. Modes:
   - *Auto* (current): particles and menus take clicks, empty space passes them through.
   - *Pass-through*: TPT takes no input. The sim keeps running over your desktop.
   - *Locked*: TPT takes all input, empty space included. Draw by painting the background 1/255 off black, so it
     isn't keyed.
   Global hotkeys (RegisterHotKey) switch modes, because the window is often unfocused. Settings live in TPT's prefs.
2. **Menu access.** Show or hide the side and bottom bars with a hotkey, plus an auto-show option (bars appear
   when the cursor nears their edge). Hidden bars draw black, so they're see-through and click-through.
3. **Scaling to any resolution.** The sim should cover the whole screen at a chosen pixel size, and the UI should
   scale separately. TPT's sim size (RES 612x384) and window size are compile-time constants, and the UI and sim
   render into one buffer at one integer scale. Plan:
   a. Pick RES at build time from the screen (a meson option), so the sim fills the screen. Cheap.
   b. Make RES a runtime value. That touches every static array sized by XRES/YRES/CELL: a larger refactor.
   c. A separate UI scale: draw the UI into its own buffer and composite it. Medium.
4. **A desktop-friendly menu in its own windows.** TPT's menus become separate, movable windows (element palette,
   tools, options, ...) instead of bars around the sim, scaling with the UI. They're styled like **Windows Vista**
   (Aero glass title bars, gradients, glow buttons). Windows 11 has no Aero glass, so the windows are custom-drawn
   with a theme (skin) system rather than native controls.
   - To-do: more OS themes for all windows (XP, 7, 10/11, classic Mac OS, macOS, Linux desktops), switchable in
     settings.
5. **Desktop interactivity (to-do).** Particles collide with and pile up on open windows (window rectangles as
   walls, updated live), react to the active window, and maybe interact with desktop files and icons.

The Rust/Bevy port (../src, ../tools/gen_elements.py) continues alongside it as the longer-term engine.
