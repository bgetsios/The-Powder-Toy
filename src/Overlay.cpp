#include "Overlay.h"
#include "prefs/GlobalPrefs.h"
#ifdef _WIN32
# include <SDL_syswm.h>
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#endif

bool overlayMode = false;

#ifdef _WIN32
namespace
{
	HWND hwnd = nullptr;
	OverlayInput input = OverlayInput::Auto;

	// A global hotkey ("Ctrl+Alt+O"), polled each frame so it works while the window is unfocused.
	struct Hotkey
	{
		bool ctrl = false, alt = false, shift = false, win = false;
		int vk = 0;
		bool wasDown = false;

		static Hotkey Parse(ByteString text)
		{
			Hotkey k;
			for (auto &part : text.PartitionBy('+'))
			{
				auto p = part.ToUpper();
				if (p == "CTRL") k.ctrl = true;
				else if (p == "ALT") k.alt = true;
				else if (p == "SHIFT") k.shift = true;
				else if (p == "WIN") k.win = true;
				else if (p.size() == 1 && isalnum((unsigned char)p[0])) k.vk = p[0];
				else if (p.size() >= 2 && p[0] == 'F') k.vk = VK_F1 - 1 + atoi(p.c_str() + 1);
			}
			return k;
		}

		static bool Down(int vk)
		{
			return GetAsyncKeyState(vk) & 0x8000;
		}

		// True once per press.
		bool Pressed()
		{
			bool down = vk && Down(vk) && ctrl == Down(VK_CONTROL) && alt == Down(VK_MENU) && shift == Down(VK_SHIFT) &&
			            win == (Down(VK_LWIN) || Down(VK_RWIN));
			bool pressed = down && !wasDown;
			wasDown = down;
			return pressed;
		}
	};
	Hotkey cycleInput, passThroughKey, lockedKey;

	void SetInput(OverlayInput newInput)
	{
		input = newInput;
		auto ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		ex = input == OverlayInput::PassThrough ? ex | WS_EX_TRANSPARENT : ex & ~LONG_PTR(WS_EX_TRANSPARENT);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
		GlobalPrefs::Ref().Set("Overlay.Input", int(input));
	}

	// A top-down 32-bit DIB the frame is scaled into, then handed to UpdateLayeredWindow.
	HDC memDC = nullptr;
	HBITMAP dib = nullptr;
	uint32_t *bits = nullptr;
	int dibW = 0, dibH = 0;
}
#endif

void ApplyOverlay(SDL_Window *window)
{
#ifdef _WIN32
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(window, &info))
	{
		return;
	}
	hwnd = info.info.win.window;
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
	auto &prefs = GlobalPrefs::Ref();
	cycleInput = Hotkey::Parse(prefs.Get("Overlay.Keys.CycleInput", ByteString("Ctrl+Alt+O")));
	passThroughKey = Hotkey::Parse(prefs.Get("Overlay.Keys.PassThrough", ByteString("Ctrl+Alt+P")));
	lockedKey = Hotkey::Parse(prefs.Get("Overlay.Keys.Locked", ByteString("Ctrl+Alt+L")));
	SetInput(prefs.Get("Overlay.Input", OverlayInput::Count, OverlayInput::Auto));
#else
	(void)window; // ponytail: Windows only; X11/Wayland/macOS each need their own shape/input-region code
#endif
}

void OverlayPresent(SDL_Window *window, const uint32_t *vid, int w, int h)
{
#ifdef _WIN32
	if (!hwnd)
	{
		return;
	}
	if (cycleInput.Pressed())
	{
		SetInput(OverlayInput((int(input) + 1) % int(OverlayInput::Count)));
	}
	if (passThroughKey.Pressed())
	{
		SetInput(input == OverlayInput::PassThrough ? OverlayInput::Auto : OverlayInput::PassThrough);
	}
	if (lockedKey.Pressed())
	{
		SetInput(input == OverlayInput::Locked ? OverlayInput::Auto : OverlayInput::Locked);
	}

	int ww, wh;
	SDL_GetWindowSize(window, &ww, &wh);
	if (ww != dibW || wh != dibH)
	{
		if (dib) DeleteObject(dib);
		if (!memDC) memDC = CreateCompatibleDC(nullptr);
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = ww;
		bi.bmiHeader.biHeight = -wh;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		dib = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, (void **)&bits, nullptr, 0);
		SelectObject(memDC, dib);
		dibW = ww;
		dibH = wh;
	}
	// Black is alpha 0: see-through, and clicks on it reach the desktop. Locked mode gives it alpha 1 instead:
	// still invisible, but it catches clicks.
	uint32_t empty = input == OverlayInput::Locked ? 0x01000000 : 0;
	for (int y = 0; y < wh; y++)
	{
		auto *src = vid + (y * h / wh) * w;
		auto *dst = bits + y * ww;
		for (int x = 0; x < ww; x++)
		{
			auto c = src[x * w / ww] & 0xFFFFFF;
			dst[x] = c ? c | 0xFF000000 : empty;
		}
	}
	POINT origin{ 0, 0 };
	SIZE size{ ww, wh };
	BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
	UpdateLayeredWindow(hwnd, nullptr, nullptr, &size, memDC, &origin, 0, &blend, ULW_ALPHA);
#else
	(void)window, (void)vid, (void)w, (void)h;
#endif
}
