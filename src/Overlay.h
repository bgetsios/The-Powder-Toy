#pragma once
#include <SDL.h>

// --overlay / overlay:1: a borderless, always-on-top window whose empty (black) pixels are see-through and click-through.
extern bool overlayMode;
void ApplyOverlay(SDL_Window *window);
