#pragma once

// Overlay mode's own desktop windows (element palette, notices), Vista-styled. Windows only so far.
namespace OverlayWindows
{
	void Init(void *mainHwnd);
	void Tick(); // once per frame, from the main thread
	void TogglePalette();
	void SetPassThrough(bool on);
	void Notify(const char *message); // a short fading notice over TPT's window
}
