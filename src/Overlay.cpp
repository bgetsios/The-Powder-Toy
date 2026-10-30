#include "Overlay.h"
#ifdef _WIN32
# include <SDL_syswm.h>
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#endif

bool overlayMode = false;

void ApplyOverlay(SDL_Window *window)
{
#ifdef _WIN32
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(window, &info))
	{
		return;
	}
	auto hwnd = info.info.win.window;
	// Layered with a colour key: pure black (TPT's empty background) is drawn see-through and clicks on it
	// go to whatever is behind the window. Particles and menus stay visible and clickable.
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
	SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
#else
	(void)window; // ponytail: Windows only; X11/Wayland/macOS each need their own shape/input-region code
#endif
}
