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
	// Sim-aware touches to a w-wide 0xAARRGGBB frame whose top-left XRES x YRES is the sim: black particles opaque,
	// everything outside the playable area clear, a faint frame around the area.
	void ApplySimMasks(uint32_t *argb, int w);
	// The playable area in cells, saved in prefs (Overlay.PlayArea.*).
	void SetPlayArea(int x, int y, int w, int h);
}
