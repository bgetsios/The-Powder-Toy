#pragma once
#include <cstdint>

// Overlay mode's own desktop windows (element palette, notices), Vista-styled. Windows only so far.
namespace OverlayWindows
{
	void Init(void *mainHwnd);
	void Tick(); // once per frame, from the main thread
	void TogglePalette();
	void SetPassThrough(bool on);
	void Notify(const char *message); // a short fading notice over TPT's window
	bool HideTPTMenus(); // TPT's own side bar and tool strip, replaced by the Elements window while it's open
	// Makes black pixels of particles opaque in a w-wide 0xAARRGGBB frame, whose top-left XRES x YRES is the sim.
	void SolidParticles(uint32_t *argb, int w);
}
