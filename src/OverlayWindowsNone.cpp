// The font editor links Overlay.cpp but has no game to build menus from.
#include "OverlayWindows.h"

void OverlayWindows::Init(void *) {}
void OverlayWindows::Tick() {}
void OverlayWindows::TogglePalette() {}
void OverlayWindows::SetPassThrough(bool) {}
void OverlayWindows::Notify(const char *) {}
bool OverlayWindows::HideTPTMenus() { return false; }
void OverlayWindows::SolidParticles(uint32_t *, int) {}
