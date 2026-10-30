#pragma once
#include <SDL.h>
#include <cstdint>

// overlay:on: a borderless, always-on-top window over the desktop. Empty (black) pixels are see-through; what
// clicks do depends on the mode (OVERLAY.md). Windows only so far.
extern bool overlayMode;

enum class OverlayInput
{
	Auto,        // particles and menus take clicks, empty space passes them to the desktop
	PassThrough, // TPT takes no input; the sim just runs over the desktop
	Locked,      // TPT takes every click, empty space included
	Count,
};

void ApplyOverlay(SDL_Window *window);
// Presents the frame (0xRRGGBB pixels, w x h) scaled to the window, with per-pixel alpha. Also polls the hotkeys.
void OverlayPresent(SDL_Window *window, const uint32_t *vid, int w, int h);
