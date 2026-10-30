// Overlay mode's own desktop windows (OVERLAY.md): each menu is a real top-level layered window, drawn here in
// software with per-pixel alpha. The look is Windows Vista Aero; other OS themes will replace the vista:: drawing.
#include "OverlayWindows.h"
#ifdef _WIN32
#include "Overlay.h"
#include "gui/game/GameController.h"
#include "gui/game/GameModel.h"
#include "graphics/VideoBuffer.h"
#include "simulation/Simulation.h"
#include "simulation/Air.h"
#include "SimulationConfig.h"
#include "gui/game/Menu.h"
#include "gui/game/tool/Tool.h"
#include "prefs/GlobalPrefs.h"
#include "common/platform/Platform.h"
#include "graphics/Renderer.h"
#include "gui/game/RenderPreset.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

namespace
{
	float uiScale = 1;
	int S(float v)
	{
		return int(std::lround(v * uiScale));
	}

	// Premultiplied BGRA, the format UpdateLayeredWindow wants.
	struct Canvas
	{
		int w = 0, h = 0;
		std::vector<uint32_t> px;

		void Resize(int nw, int nh)
		{
			w = nw;
			h = nh;
			px.assign(size_t(w) * h, 0);
		}

		// Blends rgb (0xRRGGBB) at alpha a (0-255) over pixel (x, y).
		void Blend(int x, int y, uint32_t rgb, int a)
		{
			if (x < 0 || y < 0 || x >= w || y >= h || a <= 0)
			{
				return;
			}
			auto &d = px[size_t(y) * w + x];
			int inv = 255 - a;
			auto ch = [&](int shift) {
				int s = (rgb >> shift) & 0xFF;
				int dv = (d >> shift) & 0xFF;
				return uint32_t((s * a + dv * inv) / 255) << shift;
			};
			uint32_t da = d >> 24;
			d = ch(16) | ch(8) | ch(0) | (uint32_t(a + da * inv / 255) << 24);
		}

		// A rounded rectangle with a vertical gradient (top -> bottom colour), antialiased corners.
		void RoundRect(int x, int y, int rw, int rh, int r, uint32_t top, uint32_t bottom, int a, bool squareBottom = false)
		{
			for (int j = 0; j < rh; j++)
			{
				float t = rh > 1 ? float(j) / (rh - 1) : 0;
				uint32_t c = Mix(top, bottom, t);
				for (int i = 0; i < rw; i++)
				{
					float cover = 1;
					int cx = i < r ? r - 1 - i : (i >= rw - r ? i - (rw - r) : -1);
					int cy = j < r ? r - 1 - j : (!squareBottom && j >= rh - r ? j - (rh - r) : -1);
					if (cx >= 0 && cy >= 0)
					{
						float d = std::hypot(cx + 0.5f, cy + 0.5f);
						cover = std::clamp(r - d + 0.5f, 0.f, 1.f);
					}
					Blend(x + i, y + j, c, int(a * cover));
				}
			}
		}

		// A 1px rounded outline: an outer rounded rect minus an inner one, approximated by drawing the ring.
		void RoundFrame(int x, int y, int rw, int rh, int r, uint32_t c, int a, bool squareBottom = false)
		{
			for (int j = 0; j < rh; j++)
			{
				for (int i = 0; i < rw; i++)
				{
					int cx = i < r ? r - 1 - i : (i >= rw - r ? i - (rw - r) : -1);
					int cy = j < r ? r - 1 - j : (!squareBottom && j >= rh - r ? j - (rh - r) : -1);
					float cover;
					if (cx >= 0 && cy >= 0)
					{
						float d = std::hypot(cx + 0.5f, cy + 0.5f);
						cover = std::clamp(r - d + 0.5f, 0.f, 1.f) - std::clamp(r - 1 - d + 0.5f, 0.f, 1.f);
					}
					else
					{
						cover = (i == 0 || j == 0 || i == rw - 1 || j == rh - 1) ? 1.f : 0.f;
					}
					Blend(x + i, y + j, c, int(a * cover));
				}
			}
		}

		static uint32_t Mix(uint32_t a, uint32_t b, float t)
		{
			auto ch = [&](int shift) {
				float va = float((a >> shift) & 0xFF), vb = float((b >> shift) & 0xFF);
				return uint32_t(va + (vb - va) * t + 0.5f) << shift;
			};
			return ch(16) | ch(8) | ch(0);
		}
	};

	// Text: GDI draws white-on-black into a scratch DIB, whose brightness is the coverage we blend with.
	struct Text
	{
		HDC dc = CreateCompatibleDC(nullptr);
		HFONT font = nullptr, bold = nullptr;

		void SetScale()
		{
			if (font) DeleteObject(font);
			if (bold) DeleteObject(bold);
			font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
			bold = CreateFontW(-S(12), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
		}

		SIZE Measure(const std::wstring &s, bool isBold = false)
		{
			SelectObject(dc, isBold ? bold : font);
			SIZE size{};
			GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
			return size;
		}

		// Draws s with its top-left at (x, y), clipped to maxW. glow > 0 adds Vista's soft white halo behind it.
		void Draw(Canvas &cv, int x, int y, const std::wstring &s, uint32_t rgb, bool isBold = false, int maxW = 1 << 20, int glow = 0)
		{
			auto size = Measure(s, isBold);
			int pad = glow;
			int tw = std::min(int(size.cx), maxW) + 2 * pad, th = size.cy + 2 * pad;
			if (tw <= 0 || th <= 0)
			{
				return;
			}
			BITMAPINFO bi{};
			bi.bmiHeader = { sizeof(bi.bmiHeader), tw, -th, 1, 32, BI_RGB };
			uint32_t *bits;
			auto dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, nullptr, 0);
			auto old = SelectObject(dc, dib);
			SelectObject(dc, isBold ? bold : font);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(255, 255, 255));
			RECT rc{ pad, pad, tw - pad, th - pad };
			DrawTextW(dc, s.c_str(), int(s.size()), &rc, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
			GdiFlush();
			std::vector<int> cover(size_t(tw) * th);
			for (int i = 0; i < tw * th; i++)
			{
				cover[i] = (bits[i] >> 8) & 0xFF;
			}
			if (glow)
			{
				// A box blur of the coverage, brightened: the halo.
				std::vector<int> tmp(cover.size()), halo(cover.size());
				for (int j = 0; j < th; j++)
					for (int i = 0; i < tw; i++)
					{
						int sum = 0;
						for (int k = -glow; k <= glow; k++) sum += cover[j * tw + std::clamp(i + k, 0, tw - 1)];
						tmp[j * tw + i] = sum / (2 * glow + 1);
					}
				for (int j = 0; j < th; j++)
					for (int i = 0; i < tw; i++)
					{
						int sum = 0;
						for (int k = -glow; k <= glow; k++) sum += tmp[std::clamp(j + k, 0, th - 1) * tw + i];
						halo[j * tw + i] = std::min(255, sum / (2 * glow + 1) * 4);
					}
				for (int j = 0; j < th; j++)
					for (int i = 0; i < tw; i++) cv.Blend(x - pad + i, y - pad + j, 0xFFFFFF, halo[j * tw + i] * 3 / 4);
			}
			for (int j = 0; j < th; j++)
				for (int i = 0; i < tw; i++) cv.Blend(x - pad + i, y - pad + j, rgb, cover[j * tw + i]);
			SelectObject(dc, old);
			DeleteObject(dib);
		}
	};
	Text *text;
	bool showTPTMenus = false;

	std::wstring Wide(const String &s)
	{
		return Platform::WinWiden(s.ToUtf8());
	}

	// Walls have no name in TPT (it draws them as icons), so they get their ID's tail: DEFAULT_WL_CNDTW -> CNDTW.
	std::wstring Label(Tool *tool)
	{
		if (tool->Name.size())
		{
			return Wide(tool->Name);
		}
		auto id = tool->Identifier;
		auto cut = id.rfind('_');
		return Platform::WinWiden(cut == id.npos ? id : id.substr(cut + 1));
	}

	// Windows Vista Aero. ponytail: one theme; when a second OS theme lands, these become a Theme interface.
	namespace vista
	{
		constexpr uint32_t glassTop = 0x7FA8D0, glassBottom = 0x4C7BAA; // the default Aero "sky" tint
		constexpr int glassAlpha = 205;

		// The glass frame: a translucent tinted body, a glossy upper half on the title bar, dark outer and light
		// inner edges. The client area (below the title bar, inset by the frame) is left for the caller.
		void Frame(Canvas &cv, int titleH, int border, const std::wstring &title)
		{
			int r = S(6);
			cv.RoundRect(0, 0, cv.w, cv.h, r, glassTop, glassBottom, glassAlpha);
			cv.RoundRect(1, 1, cv.w - 2, titleH / 2, r, 0xFFFFFF, 0xFFFFFF, 60, true); // gloss
			cv.RoundFrame(0, 0, cv.w, cv.h, r, 0x000000, 170);
			cv.RoundFrame(1, 1, cv.w - 2, cv.h - 2, r - 1, 0xFFFFFF, 110);
			text->Draw(cv, S(10), (titleH - text->Measure(title).cy) / 2, title, 0x000000, false, cv.w - S(80), S(5));
			// The client area's sunken edge.
			cv.RoundFrame(border - 1, titleH - 1, cv.w - 2 * border + 2, cv.h - titleH - border + 2, 0, 0x000000, 90);
		}

		// A caption button: Vista's glossy red close pill, or the light glass minimise button. hover brightens it.
		void CaptionButton(Canvas &cv, RECT rc, bool hover, bool close)
		{
			int w = rc.right - rc.left, h = rc.bottom - rc.top;
			uint32_t top = close ? (hover ? 0xF3A08A : 0xE0907C) : (hover ? 0xF0F7FF : 0xE3ECF6);
			uint32_t bottom = close ? (hover ? 0xD8341A : 0xC1361D) : (hover ? 0x5FA3E8 : 0x9AB5D3);
			cv.RoundRect(rc.left, rc.top, w, h, S(3), top, bottom, close ? 255 : 235);
			cv.RoundRect(rc.left + 1, rc.top + 1, w - 2, h / 2, S(3), 0xFFFFFF, 0xFFFFFF, 70, true);
			cv.RoundFrame(rc.left, rc.top, w, h, S(3), close ? 0x5A1E12 : 0x1F3B5A, 220);
			int cx = rc.left + w / 2, cy = rc.top + h / 2;
			if (close)
			{
				// The white x.
				int k = S(3.5f);
				for (int i = -k; i <= k; i++)
					for (int t = 0; t < S(2); t++)
					{
						cv.Blend(cx + i + t, cy + i, 0xFFFFFF, 255);
						cv.Blend(cx + i + t, cy - i, 0xFFFFFF, 255);
					}
			}
			else
			{
				// The white bar, outlined dark like Vista's.
				int bw = S(9), bh = S(3), bx = cx - bw / 2, by = cy + S(1);
				cv.RoundRect(bx - 1, by - 1, bw + 2, bh + 2, 0, 0x1F3B5A, 0x1F3B5A, 200);
				cv.RoundRect(bx, by, bw, bh, 0, 0xFFFFFF, 0xFFFFFF, 255);
			}
		}

		// A Vista push button. state: 0 normal, 1 hover, 2 pressed/selected.
		void Button(Canvas &cv, int x, int y, int w, int h, int state)
		{
			static const uint32_t top[] = { 0xF2F2F2, 0xEAF6FD, 0xE5F4FC }, mid1[] = { 0xEBEBEB, 0xD9F0FC, 0xC4E5F6 },
				mid2[] = { 0xDDDDDD, 0xBEE6FD, 0x98D1EF }, bottom[] = { 0xCFCFCF, 0xA7D9F5, 0x68B3DB },
				edge[] = { 0x707070, 0x3C7FB1, 0x2C628B };
			int r = S(3);
			cv.RoundRect(x, y, w, h / 2, r, top[state], mid1[state], 255, true);
			cv.RoundRect(x, y + h / 2, w, h - h / 2, r, mid2[state], bottom[state], 255);
			// Square off the seam between the halves.
			for (int i = 0; i < w; i++) cv.Blend(x + i, y + h / 2 - 1, mid1[state], 255);
			cv.RoundFrame(x, y, w, h, r, edge[state], 255);
			cv.RoundFrame(x + 1, y + 1, w - 2, h - 2, r - 1, 0xFFFFFF, 120);
		}

		// An Explorer navigation-pane item: plain, or a soft blue highlight.
		void NavItem(Canvas &cv, int x, int y, int w, int h, int state)
		{
			if (state == 0) return;
			cv.RoundRect(x, y, w, h, S(2), state == 2 ? 0xDCEBFC : 0xF2F8FE, state == 2 ? 0xC1DBFC : 0xE3EFFD, 255);
			cv.RoundFrame(x, y, w, h, S(2), state == 2 ? 0x7DA2CE : 0xB8D6FB, 255);
		}
	}

	// A top-level layered window that repaints its Canvas whenever Invalidate() is called.
	struct Panel
	{
		HWND hwnd = nullptr;
		Canvas cv;
		ByteString name;
		bool dirty = true;
		int opacity = 255;

		virtual ~Panel() = default;
		virtual void Paint() = 0;
		virtual LRESULT HitTest(int, int) { return HTCLIENT; }
		virtual void Mouse(UINT, int, int, WPARAM) {}

		void Create(int x, int y, int w, int h)
		{
			static bool registered = false;
			if (!registered)
			{
				WNDCLASSW wc{};
				wc.lpfnWndProc = Proc;
				wc.hInstance = GetModuleHandleW(nullptr);
				wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
				wc.lpszClassName = L"TPTOverlayPanel";
				RegisterClassW(&wc);
				registered = true;
			}
			cv.Resize(w, h);
			hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"TPTOverlayPanel",
			                       Platform::WinWiden(name).c_str(), WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(this));
		}

		void Present()
		{
			if (dirty)
			{
				std::fill(cv.px.begin(), cv.px.end(), 0);
				Paint();
				dirty = false;
			}
			HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
			BITMAPINFO bi{};
			bi.bmiHeader = { sizeof(bi.bmiHeader), cv.w, -cv.h, 1, 32, BI_RGB };
			void *bits;
			auto dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
			memcpy(bits, cv.px.data(), cv.px.size() * 4);
			auto old = SelectObject(dc, dib);
			POINT origin{ 0, 0 };
			SIZE size{ cv.w, cv.h };
			BLENDFUNCTION blend{ AC_SRC_OVER, 0, BYTE(opacity), AC_SRC_ALPHA };
			UpdateLayeredWindow(hwnd, screen, nullptr, &size, dc, &origin, 0, &blend, ULW_ALPHA);
			SelectObject(dc, old);
			DeleteObject(dib);
			DeleteDC(dc);
			ReleaseDC(nullptr, screen);
		}

		void SavePosition()
		{
			RECT rc;
			GetWindowRect(hwnd, &rc);
			auto &prefs = GlobalPrefs::Ref();
			prefs.Set("Overlay.Windows." + name + ".X", int(rc.left));
			prefs.Set("Overlay.Windows." + name + ".Y", int(rc.top));
		}

		static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			auto *self = (Panel *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
			if (self)
			{
				switch (msg)
				{
				case WM_NCHITTEST:
				{
					POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
					ScreenToClient(hwnd, &p);
					return self->HitTest(p.x, p.y);
				}
				case WM_MOUSEACTIVATE:
					return MA_NOACTIVATE; // keep keyboard focus on TPT
				case WM_EXITSIZEMOVE:
					self->SavePosition();
					return 0;
				case WM_MOUSEMOVE:
				{
					TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
					TrackMouseEvent(&tme);
					self->Mouse(msg, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
					return 0;
				}
				case WM_MOUSELEAVE:
				case WM_LBUTTONDOWN:
				case WM_LBUTTONUP:
				case WM_RBUTTONDOWN:
				case WM_MBUTTONDOWN:
					self->Mouse(msg, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
					return 0;
				case WM_MOUSEWHEEL:
				{
					POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
					ScreenToClient(hwnd, &p);
					self->Mouse(msg, p.x, p.y, wp);
					return 0;
				}
				}
			}
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
	};

	bool Inside(const RECT &rc, int x, int y)
	{
		return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
	}

	// The side window: every TPT menu as a tab. Elements has TPT's menu sections down the left (Explorer's
	// navigation pane) and the section's tools as Vista buttons; left click picks the primary tool, right click the
	// secondary, as in TPT. The other tabs are immediate-mode Vista controls: each paint rebuilds the list of
	// clickable rectangles (hits) that the mouse handler uses.
	struct SideWindow : Panel
	{
		struct Hit
		{
			RECT rc;
			std::function<void()> left, right;
			std::wstring tip;
		};
		std::vector<Hit> hits;
		int hover = -1;
		int tab = 0, section = 0, scroll = 0;
		int titleH, border, tabH, navW, statusH, tileW, tileH, gap;
		uint64_t shownState = 0;
		static constexpr const wchar_t *tabNames[] = { L"Elements", L"Simulation", L"Display", L"Saves", L"Overlay" };

		SideWindow()
		{
			name = "Palette"; // keeps the saved position of the old Elements window
			titleH = S(28), border = S(6), tabH = S(24), navW = S(118), statusH = S(22), tileW = S(92), tileH = S(24), gap = S(4);
		}

		std::vector<Menu *> Sections()
		{
			std::vector<Menu *> out;
			for (auto *menu : GameController::Ref().GetMenuList())
			{
				if (menu->GetVisible())
				{
					out.push_back(menu);
				}
			}
			return out;
		}

		std::vector<Tool *> Tools()
		{
			auto sections = Sections();
			std::vector<Tool *> out;
			if (section < int(sections.size()))
			{
				for (auto *tool : sections[section]->GetToolList())
				{
					if (tool->MenuVisible)
					{
						out.push_back(tool);
					}
				}
			}
			return out;
		}

		RECT CloseRect()
		{
			return { cv.w - border - S(43), 1, cv.w - border, 1 + S(18) };
		}
		RECT MinRect()
		{
			auto close = CloseRect();
			return { close.left - S(27), close.top, close.left, close.bottom };
		}
		// Below the tabs, above the status bar.
		RECT Content()
		{
			return { border, titleH + tabH, cv.w - border, cv.h - border - statusH };
		}
		RECT ToolArea()
		{
			auto c = Content();
			return { c.left + navW + gap, c.top + gap, c.right - gap, c.bottom };
		}
		int Columns()
		{
			auto a = ToolArea();
			return std::max(1, int((a.right - a.left + gap) / (tileW + gap)));
		}
		RECT TileRect(int i)
		{
			auto a = ToolArea();
			int c = Columns();
			int x = a.left + (i % c) * (tileW + gap), y = a.top + (i / c) * (tileH + gap) - scroll;
			return { x, y, x + tileW, y + tileH };
		}
		RECT NavRect(int i)
		{
			auto c = Content();
			int h = S(22);
			return { c.left + S(3), c.top + S(4) + i * h, c.left + navW - S(3), c.top + S(4) + (i + 1) * h - S(1) };
		}

		bool Hovered(const RECT &rc)
		{
			return hover >= 0 && hover < int(hits.size()) && EqualRect(&hits[hover].rc, &rc);
		}

		RECT Add(RECT rc, std::function<void()> left, std::wstring tip = L"", std::function<void()> right = nullptr)
		{
			hits.push_back({ rc, std::move(left), std::move(right), std::move(tip) });
			return rc;
		}

		// --- Vista controls; each draws itself and registers its hit rectangle.

		void PushButton(int x, int y, int w, const std::wstring &label, std::function<void()> action, const std::wstring &tip = L"", bool pressed = false)
		{
			RECT rc{ x, y, x + w, y + S(23) };
			vista::Button(cv, x, y, w, S(23), pressed ? 2 : (Hovered(rc) ? 1 : 0));
			auto size = text->Measure(label);
			text->Draw(cv, x + (w - size.cx) / 2, y + (S(23) - size.cy) / 2, label, 0x000000, false, w - S(6));
			Add(rc, std::move(action), tip);
		}

		// A check box (round = radio button) with its label; returns the width used.
		int Toggle(int x, int y, const std::wstring &label, bool on, bool round, std::function<void()> action, const std::wstring &tip = L"")
		{
			int b = S(13), w = b + S(6) + text->Measure(label).cx;
			RECT rc{ x, y, x + w, y + S(20) };
			bool hot = Hovered(rc);
			int by = y + (S(20) - b) / 2, r = round ? b / 2 : S(2);
			cv.RoundRect(x, by, b, b, r, hot ? 0xDEF3FD : 0xF4F4F4, hot ? 0xB1DFFD : 0xDEDEDE, 255);
			cv.RoundFrame(x, by, b, b, r, hot ? 0x3C7FB1 : 0x8E8F8F, 255);
			if (on && round)
			{
				int d = b / 2;
				cv.RoundRect(x + (b - d) / 2, by + (b - d) / 2, d, d, d / 2, 0x8AD0F2, 0x1C64B4, 255);
			}
			if (on && !round)
			{
				int cx = x + b / 2, cy = by + b / 2, k = S(2.5f);
				for (int t = 0; t < S(1.6f); t++)
				{
					for (int i = 0; i <= k; i++) cv.Blend(cx - k - 1 + i, cy + i - 1 + t, 0x1C3D6E, 255);
					for (int i = 0; i <= 2 * k; i++) cv.Blend(cx - 1 + i, cy + k - 1 - i + t, 0x1C3D6E, 255);
				}
			}
			text->Draw(cv, x + b + S(6), y + (S(20) - text->Measure(label).cy) / 2, label, 0x000000);
			Add(rc, std::move(action), tip);
			return w;
		}

		// A Vista group box: a rounded pale outline with a blue heading; returns the top of its inside.
		int Group(int x, int y, int w, int h, const std::wstring &title)
		{
			cv.RoundFrame(x, y + S(8), w, h - S(8), S(3), 0xD5DFE5, 255);
			auto size = text->Measure(title);
			cv.RoundRect(x + S(8), y, size.cx + S(8), size.cy, 0, 0xFFFFFF, 0xFFFFFF, 255);
			text->Draw(cv, x + S(12), y, title, 0x1E395B);
			return y + size.cy + S(4);
		}

		void Caption(int x, int y, const std::wstring &s, uint32_t rgb = 0x000000)
		{
			text->Draw(cv, x, y + (S(20) - text->Measure(s).cy) / 2, s, rgb);
		}

		// --- tabs

		void PaintElements()
		{
			auto c = Content();
			cv.RoundRect(c.left, c.top, navW, c.bottom - c.top, 0, 0xF1F5FB, 0xE5EDF8, 255);
			for (int j = c.top; j < c.bottom; j++) cv.Blend(c.left + navW, j, 0xA0AFC3, 255);
			auto sections = Sections();
			for (int i = 0; i < int(sections.size()); i++)
			{
				auto rc = NavRect(i);
				vista::NavItem(cv, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, i == section ? 2 : (Hovered(rc) ? 1 : 0));
				text->Draw(cv, rc.left + S(6), rc.top + S(3), Wide(sections[i]->GetDescription()), 0x1E395B, i == section, rc.right - rc.left - S(10));
				Add(rc, [this, i] { section = i, scroll = 0; });
			}
			auto tools = Tools();
			auto area = ToolArea();
			auto &gc = GameController::Ref();
			auto *primary = gc.GetActiveTool(0), *secondary = gc.GetActiveTool(1);
			for (int i = 0; i < int(tools.size()); i++)
			{
				auto rc = TileRect(i);
				if (rc.bottom <= area.top || rc.top >= area.bottom)
				{
					continue;
				}
				auto *tool = tools[i];
				bool selected = tool == primary || tool == secondary;
				Canvas tile;
				tile.Resize(tileW, tileH);
				vista::Button(tile, 0, 0, tileW, tileH, selected ? 2 : (Hovered(rc) ? 1 : 0));
				if (tool == primary) tile.RoundFrame(0, 0, tileW, tileH, S(3), 0xD02020, 255);
				if (tool == secondary) tile.RoundFrame(1, 1, tileW - 2, tileH - 2, S(3), 0x2050D0, 255);
				auto col = tool->Colour;
				uint32_t rgb = (col.Red << 16) | (col.Green << 8) | col.Blue;
				int sw = tileH - S(10);
				// TPT's own icon (walls, tools, some elements), scaled up; otherwise a colour swatch.
				if (auto icon = tool->GetTexture(Vec2(26, 14)))
				{
					int iw = S(26), ih = S(14), iy = (tileH - ih) / 2;
					auto size = icon->Size();
					for (int y = 0; y < ih; y++)
						for (int x = 0; x < iw; x++) tile.Blend(S(5) + x, iy + y, icon->Data()[(y * size.Y / ih) * size.X + x * size.X / iw] & 0xFFFFFF, 255);
					tile.RoundFrame(S(5) - 1, iy - 1, iw + 2, ih + 2, 0, 0x404040, 200);
					sw = iw;
				}
				else
				{
					tile.RoundRect(S(5), S(5), sw, sw, S(2), rgb, rgb, 255);
					tile.RoundFrame(S(5), S(5), sw, sw, S(2), 0x404040, 200);
				}
				text->Draw(tile, S(9) + sw, (tileH - text->Measure(Label(tool)).cy) / 2, Label(tool), 0x000000, false, tileW - sw - S(12));
				// Copy the tile in, clipped to the tool area.
				for (int y = 0; y < tileH; y++)
				{
					int dy = rc.top + y;
					if (dy < area.top || dy >= area.bottom) continue;
					for (int x = 0; x < tileW; x++)
					{
						auto p = tile.px[y * tileW + x];
						cv.Blend(rc.left + x, dy, p & 0xFFFFFF, p >> 24);
					}
				}
				RECT visible{ rc.left, std::max(rc.top, area.top), rc.right, std::min(rc.bottom, area.bottom) };
				Add(visible, [tool] { GameController::Ref().SetActiveTool(0, tool); }, Wide(tool->Description),
				    [tool] { GameController::Ref().SetActiveTool(1, tool); });
			}
		}

		void PaintSimulation()
		{
			auto c = Content();
			auto &gc = GameController::Ref();
			auto *model = gc.GetModel();
			auto *sim = model->GetSimulation();
			int x = c.left + S(10), w = c.right - c.left - S(20), y = c.top + S(8);

			// The playable area, in cells of 4 pixels.
			int top = Group(x, y, w, S(118), L"Play area");
			auto a = sim->playArea;
			Caption(x + S(12), top, L"Size: " + std::to_wstring(a.size.X * CELL) + L" \u00D7 " + std::to_wstring(a.size.Y * CELL) +
			      L" pixels   (largest " + std::to_wstring(XRES) + L" \u00D7 " + std::to_wstring(YRES) + L")");
			int step = 8; // cells
			auto set = [](Rect<int> r) { OverlayWindows::SetPlayArea(r.pos.X, r.pos.Y, r.size.X, r.size.Y); };
			// Grow or shrink about the centre.
			auto resize = [a, set](int dw, int dh) {
				auto r = a;
				r.size.X = std::clamp(r.size.X + dw, 8, CELLS.X);
				r.size.Y = std::clamp(r.size.Y + dh, 8, CELLS.Y);
				r.pos.X = std::clamp(a.pos.X - (r.size.X - a.size.X) / 2, 0, CELLS.X - r.size.X);
				r.pos.Y = std::clamp(a.pos.Y - (r.size.Y - a.size.Y) / 2, 0, CELLS.Y - r.size.Y);
				set(r);
			};
			int by = top + S(24), bw = S(34);
			Caption(x + S(12), by, L"Width");
			PushButton(x + S(60), by, bw, L"\u2212", [=] { resize(-step, 0); }, L"Narrower by 32 pixels");
			PushButton(x + S(60) + bw + S(4), by, bw, L"+", [=] { resize(step, 0); }, L"Wider by 32 pixels");
			Caption(x + S(160), by, L"Height");
			PushButton(x + S(210), by, bw, L"\u2212", [=] { resize(0, -step); }, L"Shorter by 32 pixels");
			PushButton(x + S(210) + bw + S(4), by, bw, L"+", [=] { resize(0, step); }, L"Taller by 32 pixels");
			by += S(30);
			int pw = (w - S(24) - 2 * S(6)) / 3;
			PushButton(x + S(12), by, pw, L"Whole screen", [=] { set(CELLS.OriginRect()); }, L"Use the whole sim");
			PushButton(x + S(12) + pw + S(6), by, pw, L"Stock TPT size", [=] {
				auto stock = Vec2(std::min(153, CELLS.X), std::min(96, CELLS.Y));
				set(RectSized((CELLS - stock) / 2, stock));
			}, L"612 \u00D7 384, centred: saves made at this size open in normal TPT");
			PushButton(x + S(12) + 2 * (pw + S(6)), by, pw, L"Centre", [=] { set(RectSized((CELLS - a.size) / 2, a.size)); }, L"Move the area to the middle");
			y += S(126);

			top = Group(x, y, w, S(60), L"Running");
			bool paused = gc.GetPaused();
			int bw4 = (w - S(24) - 3 * S(6)) / 4;
			PushButton(x + S(12), top, bw4, paused ? L"Resume" : L"Pause", [] { auto &g = GameController::Ref(); g.SetPaused(!g.GetPaused()); }, L"Space", paused);
			PushButton(x + S(12) + bw4 + S(6), top, bw4, L"Step", [] { GameController::Ref().FrameStep(); }, L"One frame (F)");
			PushButton(x + S(12) + 2 * (bw4 + S(6)), top, bw4, L"Reload", [] { GameController::Ref().ReloadSim(); }, L"Back to the last loaded save (F5)");
			PushButton(x + S(12) + 3 * (bw4 + S(6)), top, bw4, L"Clear", [] { GameController::Ref().ClearSim(); }, L"Empty the sim");
			y += S(68);

			top = Group(x, y, w, S(52), L"Edges");
			const wchar_t *edges[] = { L"Void", L"Solid", L"Loop" };
			const wchar_t *edgeTips[] = { L"Particles and air leave at the edges", L"The edges are walls", L"Particles wrap around (whole screen only for now)" };
			int ex = x + S(12);
			for (int i = 0; i < 3; i++)
			{
				ex += Toggle(ex, top, edges[i], gc.GetEdgeMode() == i, true, [i] { GameController::Ref().SetEdgeMode(i); }, edgeTips[i]) + S(18);
			}
			y += S(60);

			top = Group(x, y, w, S(76), L"Gravity");
			const wchar_t *gravs[] = { L"Vertical", L"Off", L"Radial", L"Custom" };
			int gx = x + S(12);
			for (int i = 0; i < 4; i++)
			{
				gx += Toggle(gx, top, gravs[i], sim->gravityMode == i, true, [sim, i] { sim->gravityMode = i; }) + S(18);
			}
			Toggle(x + S(12), top + S(24), L"Newtonian gravity", model->GetNewtonianGrvity(), false,
			       [model] { model->SetNewtonianGravity(!model->GetNewtonianGrvity()); }, L"Heavy particles pull on each other (N)");
			y += S(84);

			top = Group(x, y, w, S(100), L"Air");
			const wchar_t *airs[] = { L"On", L"Pressure off", L"Velocity off", L"Off", L"No update" };
			int ax = x + S(12), ay = top;
			for (int i = 0; i < 5; i++)
			{
				int tw = text->Measure(airs[i]).cx + S(19);
				if (ax + tw > x + w - S(12))
				{
					ax = x + S(12), ay += S(24);
				}
				ax += Toggle(ax, ay, airs[i], sim->air->airMode == i, true, [sim, i] { sim->air->airMode = i; }) + S(18);
			}
			Toggle(x + S(12), ay + S(24), L"Ambient heat", gc.GetAHeatEnable(), false, [] { GameController::Ref().ToggleAHeat(); }, L"The air carries heat (U)");
		}

		void PaintDisplay()
		{
			auto c = Content();
			auto &gc = GameController::Ref();
			auto *model = gc.GetModel();
			int x = c.left + S(10), w = c.right - c.left - S(20), y = c.top + S(8);
			auto &presets = Renderer::renderModePresets;
			int rows = (int(presets.size()) + 2) / 3;
			int top = Group(x, y, w, S(20) + rows * S(29), L"View mode");
			int bw = (w - S(24) - 2 * S(6)) / 3;
			for (int i = 0; i < int(presets.size()); i++)
			{
				PushButton(x + S(12) + (i % 3) * (bw + S(6)), top + (i / 3) * S(29), bw, Wide(presets[i].Name),
				           [i] { GameController::Ref().LoadRenderPreset(i); }, L"Number key " + std::to_wstring(i == 9 ? 0 : i + 1));
			}
			y += S(28) + rows * S(29);
			top = Group(x, y, w, S(100), L"Show");
			Toggle(x + S(12), top, L"HUD", gc.GetHudEnable(), false, [] { auto &g = GameController::Ref(); g.SetHudEnable(!g.GetHudEnable()); }, L"Frame rate, particle count and cursor info (H)");
			Toggle(x + S(12), top + S(24), L"Decorations", model->GetDecoration(), false, [] { GameController::Ref().SetDecoration(); }, L"Painted colours (Ctrl+B)");
			Toggle(x + S(12), top + S(48), L"Gravity field", model->GetGravityGrid(), false, [] { GameController::Ref().ShowGravityGrid(); }, L"Newtonian gravity lines (Ctrl+G)");
			y += S(108);
			PushButton(x, y, S(200), L"More render options\u2026", [] { GameController::Ref().OpenRenderOptions(); }, L"TPT's full render settings");
		}

		void PaintSaves()
		{
			auto c = Content();
			int x = c.left + S(10), w = c.right - c.left - S(20), y = c.top + S(8);
			int top = Group(x, y, w, S(96), L"Saves");
			int bw = (w - S(24) - S(6)) / 2;
			PushButton(x + S(12), top, bw, L"Open\u2026", [] { GameController::Ref().OpenLocalBrowse(); }, L"Your local saves");
			PushButton(x + S(12) + bw + S(6), top, bw, L"Save\u2026", [] { GameController::Ref().OpenLocalSaveWindow(false); },
			           L"Saves the play area; at 612 \u00D7 384 or smaller it opens in normal TPT");
			PushButton(x + S(12), top + S(29), bw, L"Stamps\u2026", [] { GameController::Ref().OpenStamps(); }, L"Saved selections (K)");
			PushButton(x + S(12) + bw + S(6), top + S(29), bw, L"Reload", [] { GameController::Ref().ReloadSim(); }, L"Back to the last loaded save (F5)");
			y += S(104);
			top = Group(x, y, w, S(52), L"TPT");
			PushButton(x + S(12), top, bw, L"Options\u2026", [] { GameController::Ref().OpenOptions(); }, L"TPT's own settings window");
		}

		void PaintOverlay()
		{
			auto c = Content();
			int x = c.left + S(10), w = c.right - c.left - S(20), y = c.top + S(8);
			auto &prefs = GlobalPrefs::Ref();
			auto key = [&](const char *name, const char *fallback) {
				return Platform::WinWiden(prefs.Get(ByteString("Overlay.Keys.") + name, ByteString(fallback)));
			};
			int top = Group(x, y, w, S(100), L"Clicks go to");
			const wchar_t *modes[] = { L"Auto: particles and menus to TPT, empty space to the desktop", L"Pass-through: everything to the desktop", L"Locked: everything to TPT" };
			for (int i = 0; i < int(OverlayInput::Count); i++)
			{
				Toggle(x + S(12), top + i * S(24), modes[i], int(OverlayGetInput()) == i, true, [i] { OverlaySetInput(OverlayInput(i)); });
			}
			y += S(108);
			top = Group(x, y, w, S(44), L"Window");
			Toggle(x + S(12), top, L"Show TPT's own menus too", showTPTMenus, false, [] {
				showTPTMenus = !showTPTMenus;
				GlobalPrefs::Ref().Set("Overlay.ShowTPTMenus", showTPTMenus);
			});
			y += S(52);
			top = Group(x, y, w, S(124), L"Shortcuts (any app)");
			std::wstring rows[][2] = {
				{ L"Next input mode", key("CycleInput", "Ctrl+Alt+O") },
				{ L"Pass-through on/off", key("PassThrough", "Ctrl+Alt+P") },
				{ L"Locked on/off", key("Locked", "Ctrl+Alt+L") },
				{ L"Show/hide this window", key("Palette", "Ctrl+Alt+E") },
			};
			for (int i = 0; i < 4; i++)
			{
				Caption(x + S(12), top + i * S(24), rows[i][0]);
				Caption(x + S(200), top + i * S(24), rows[i][1], 0x1E395B);
			}
			y += S(132);
			Caption(x, y, L"Change shortcuts and UI scale in powder.pref (Overlay.*) for now.", 0x6D6D6D);
		}

		void Paint() override
		{
			hits.clear();
			vista::Frame(cv, titleH, border, L"The Powder Toy");
			vista::CaptionButton(cv, MinRect(), Hovered(MinRect()), false);
			vista::CaptionButton(cv, CloseRect(), Hovered(CloseRect()), true);
			Add(MinRect(), [] { OverlayWindows::TogglePalette(); }, L"Minimise to the tray");
			Add(CloseRect(), [] { OverlayWindows::TogglePalette(); }, L"Close (the tray icon brings it back)");

			// The tab strip: grey tabs on the glass, the selected one white and joined to the page.
			int tx = border + S(4);
			cv.RoundRect(border, titleH, cv.w - 2 * border, tabH, 0, 0xF0F0F0, 0xF0F0F0, 255);
			for (int i = 0; i < int(std::size(tabNames)); i++)
			{
				int tw = text->Measure(tabNames[i]).cx + S(20);
				bool on = i == tab;
				RECT rc{ tx, titleH + (on ? S(2) : S(4)), tx + tw, titleH + tabH + (on ? 1 : 0) };
				bool hot = !on && Hovered(rc);
				cv.RoundRect(rc.left, rc.top, tw, rc.bottom - rc.top, S(3), on ? 0xFFFFFF : (hot ? 0xEAF6FD : 0xF2F2F2), on ? 0xFFFFFF : (hot ? 0xA7D9F5 : 0xE3E3E3), 255, true);
				cv.RoundFrame(rc.left, rc.top, tw, rc.bottom - rc.top + S(3), S(3), on ? 0x898C95 : (hot ? 0x3C7FB1 : 0x898C95), 255, true);
				text->Draw(cv, tx + S(10), rc.top + (rc.bottom - rc.top - text->Measure(tabNames[i]).cy) / 2, tabNames[i], 0x000000);
				Add(rc, [this, i] { tab = i, scroll = 0; });
				tx += tw + S(1);
			}
			// The page.
			auto c = Content();
			cv.RoundRect(c.left, c.top, c.right - c.left, c.bottom - c.top, 0, 0xFFFFFF, 0xFFFFFF, 255);
			for (int i = c.left; i < c.right; i++) cv.Blend(i, c.top, 0x898C95, 255);
			// The selected tab's bottom edge opens into the page.
			int sx = border + S(4);
			for (int i = 0; i < tab; i++) sx += text->Measure(tabNames[i]).cx + S(21);
			for (int i = sx + 1; i < sx + text->Measure(tabNames[tab]).cx + S(19); i++) cv.Blend(i, c.top, 0xFFFFFF, 255);

			switch (tab)
			{
			case 0: PaintElements(); break;
			case 1: PaintSimulation(); break;
			case 2: PaintDisplay(); break;
			case 3: PaintSaves(); break;
			case 4: PaintOverlay(); break;
			}

			// The status bar: what the hovered control does.
			int sy = cv.h - border - statusH;
			cv.RoundRect(border, sy, cv.w - 2 * border, statusH, 0, 0xF7F9FC, 0xDCE5F1, 255);
			for (int i = border; i < cv.w - border; i++) cv.Blend(i, sy, 0xA0AFC3, 255);
			if (hover >= 0 && hover < int(hits.size()))
			{
				text->Draw(cv, border + S(8), sy + S(3), hits[hover].tip, 0x1E395B, false, cv.w - 2 * border - S(16));
			}
		}

		LRESULT HitTest(int x, int y) override
		{
			return y < titleH && !Inside(CloseRect(), x, y) && !Inside(MinRect(), x, y) ? HTCAPTION : HTCLIENT;
		}

		void Mouse(UINT msg, int x, int y, WPARAM wp) override
		{
			int at = -1;
			if (msg != WM_MOUSELEAVE)
			{
				for (int i = 0; i < int(hits.size()); i++)
					if (Inside(hits[i].rc, x, y)) at = i;
			}
			if (at != hover)
			{
				hover = at;
				dirty = true;
			}
			if (at >= 0 && msg == WM_LBUTTONUP && hits[at].left)
			{
				auto action = hits[at].left; // the action may repaint and rebuild hits
				action();
				dirty = true;
			}
			if (at >= 0 && msg == WM_RBUTTONUP && hits[at].right)
			{
				auto action = hits[at].right;
				action();
				dirty = true;
			}
			if (msg == WM_MOUSEWHEEL && tab == 0)
			{
				auto tools = Tools();
				auto area = ToolArea();
				int rows = (int(tools.size()) + Columns() - 1) / Columns();
				int maxScroll = std::max(0, rows * (tileH + gap) - int(area.bottom - area.top));
				scroll = std::clamp(scroll - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * (tileH + gap), 0, maxScroll);
				dirty = true;
			}
		}

		// Repaint when anything shown changes, from here, from TPT's keys, or from Lua.
		void Tick()
		{
			auto &gc = GameController::Ref();
			auto *model = gc.GetModel();
			auto *sim = model->GetSimulation();
			size_t count = 0;
			for (auto *menu : gc.GetMenuList()) count += menu->GetToolList().size();
			auto a = sim->playArea;
			uint64_t state = 1469598103934665603ULL;
			auto mix = [&](uint64_t v) { state = (state ^ v) * 1099511628211ULL; };
			mix(uint64_t(uintptr_t(gc.GetActiveTool(0))));
			mix(uint64_t(uintptr_t(gc.GetActiveTool(1))));
			for (uint64_t v : { uint64_t(count), uint64_t(gc.GetPaused()), uint64_t(gc.GetEdgeMode()), uint64_t(sim->gravityMode),
			                    uint64_t(sim->air->airMode), uint64_t(model->GetNewtonianGrvity()), uint64_t(gc.GetAHeatEnable()),
			                    uint64_t(gc.GetHudEnable()), uint64_t(model->GetDecoration()), uint64_t(model->GetGravityGrid()),
			                    uint64_t(OverlayGetInput()), uint64_t(showTPTMenus), uint64_t(a.pos.X), uint64_t(a.pos.Y),
			                    uint64_t(a.size.X), uint64_t(a.size.Y) })
			{
				mix(v);
			}
			if (state != shownState)
			{
				shownState = state;
				dirty = true;
			}
		}
	};

	// A small glass notice ("Input: Locked") that fades out.
	struct Toast : Panel
	{
		std::wstring message;
		DWORD shownAt = 0;

		Toast()
		{
			name = "Toast";
		}

		void Paint() override
		{
			int r = S(6);
			cv.RoundRect(0, 0, cv.w, cv.h, r, vista::glassTop, vista::glassBottom, vista::glassAlpha);
			cv.RoundRect(1, 1, cv.w - 2, cv.h / 2, r, 0xFFFFFF, 0xFFFFFF, 60, true);
			cv.RoundFrame(0, 0, cv.w, cv.h, r, 0x000000, 170);
			cv.RoundFrame(1, 1, cv.w - 2, cv.h - 2, r - 1, 0xFFFFFF, 110);
			auto size = text->Measure(message, true);
			text->Draw(cv, (cv.w - size.cx) / 2, (cv.h - size.cy) / 2, message, 0x000000, true, cv.w, S(5));
		}

		LRESULT HitTest(int, int) override
		{
			return HTTRANSPARENT;
		}

		void Show(HWND over, const std::wstring &newMessage)
		{
			message = newMessage;
			auto size = text->Measure(message, true);
			int w = size.cx + S(48), h = S(40);
			RECT rc;
			GetWindowRect(over, &rc);
			int x = (rc.left + rc.right - w) / 2, y = rc.top + S(40);
			if (!hwnd)
			{
				Create(x, y, w, h);
				SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
			}
			cv.Resize(w, h);
			SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			shownAt = GetTickCount();
			dirty = true;
		}

		void Tick()
		{
			if (!hwnd || !shownAt) return;
			auto age = GetTickCount() - shownAt;
			if (age > 1800)
			{
				ShowWindow(hwnd, SW_HIDE);
				shownAt = 0;
				return;
			}
			opacity = age < 1200 ? 255 : int(255 * (1800 - age) / 600);
			Present();
		}
	};

	HWND mainWindow = nullptr;
	std::unique_ptr<SideWindow> palette;
	std::unique_ptr<Toast> toast;
	bool passThrough = false;

	// The notification-area (tray) icon: left click shows or hides the Elements window, right click opens a menu.
	NOTIFYICONDATAW tray{};
	constexpr UINT trayMessage = WM_APP + 1;

	LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
		if (msg == taskbarCreated)
		{
			Shell_NotifyIconW(NIM_ADD, &tray); // Explorer restarted and lost the icon
			return 0;
		}
		if (msg != trayMessage)
		{
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
		if (lp == WM_LBUTTONUP)
		{
			OverlayWindows::TogglePalette();
		}
		if (lp == WM_RBUTTONUP)
		{
			// The plain Windows menu: settings and the way into the Vista windows.
			enum { elements = 1, inputBase = 10, showMenus = 20, exitItem = 99 };
			auto &prefs = GlobalPrefs::Ref();
			auto label = [&](const wchar_t *text, const char *keyName, const char *fallback) {
				return std::wstring(text) + L"\t" + Platform::WinWiden(prefs.Get(ByteString("Overlay.Keys.") + keyName, ByteString(fallback)));
			};
			HMENU menu = CreatePopupMenu();
			bool open = palette && IsWindowVisible(palette->hwnd);
			AppendMenuW(menu, MF_STRING | (open ? MF_CHECKED : 0), elements, label(L"Elements window", "Palette", "Ctrl+Alt+E").c_str());
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			std::wstring modes[] = { L"Input: Auto", label(L"Input: Pass-through", "PassThrough", "Ctrl+Alt+P"), label(L"Input: Locked to TPT", "Locked", "Ctrl+Alt+L") };
			for (int i = 0; i < int(OverlayInput::Count); i++)
			{
				AppendMenuW(menu, MF_STRING | (int(OverlayGetInput()) == i ? MF_CHECKED : 0), inputBase + i, modes[i].c_str());
			}
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(menu, MF_STRING | (showTPTMenus ? MF_CHECKED : 0), showMenus, L"Show TPT's own menus");
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(menu, MF_STRING, exitItem, L"Exit");
			POINT p;
			GetCursorPos(&p);
			SetForegroundWindow(hwnd); // so the menu closes when you click elsewhere
			int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, hwnd, nullptr);
			PostMessageW(hwnd, WM_NULL, 0, 0);
			DestroyMenu(menu);
			if (cmd == elements)
			{
				OverlayWindows::TogglePalette();
			}
			if (cmd >= inputBase && cmd < inputBase + int(OverlayInput::Count))
			{
				OverlaySetInput(OverlayInput(cmd - inputBase));
			}
			if (cmd == showMenus)
			{
				showTPTMenus = !showTPTMenus;
				prefs.Set("Overlay.ShowTPTMenus", showTPTMenus);
			}
			if (cmd == exitItem)
			{
				SDL_Event quit{};
				quit.type = SDL_QUIT;
				SDL_PushEvent(&quit);
			}
		}
		return 0;
	}

	void AddTrayIcon()
	{
		WNDCLASSW wc{};
		wc.lpfnWndProc = TrayProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"TPTOverlayTray";
		RegisterClassW(&wc);
		tray.cbSize = sizeof(tray);
		// A hidden top-level window, not a message-only one: those miss the TaskbarCreated broadcast.
		tray.hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
		tray.uID = 1;
		tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		tray.uCallbackMessage = trayMessage;
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		tray.hIcon = ExtractIconW(wc.hInstance, exe, 0);
		if (!tray.hIcon || tray.hIcon == HICON(1))
		{
			tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		}
		wcscpy_s(tray.szTip, L"The Powder Toy (overlay)");
		Shell_NotifyIconW(NIM_ADD, &tray);
		std::atexit([] { Shell_NotifyIconW(NIM_DELETE, &tray); });
		// On a crash too. A forced kill (Task Manager's End task) can't be caught: Windows clears that icon on hover.
		static LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *e) -> LONG {
			Shell_NotifyIconW(NIM_DELETE, &tray);
			return previous ? previous(e) : EXCEPTION_CONTINUE_SEARCH;
		});
	}

	void ApplyPassThrough(Panel &panel)
	{
		auto ex = GetWindowLongPtrW(panel.hwnd, GWL_EXSTYLE);
		SetWindowLongPtrW(panel.hwnd, GWL_EXSTYLE, passThrough ? ex | WS_EX_TRANSPARENT : ex & ~LONG_PTR(WS_EX_TRANSPARENT));
		panel.opacity = passThrough ? 110 : 255;
		panel.Present();
	}
}

void OverlayWindows::Init(void *mainHwnd)
{
	mainWindow = HWND(mainHwnd);
	auto &prefs = GlobalPrefs::Ref();
	// Default UI scale: the larger of Windows' display scaling (TPT is DPI aware) and screen height / 1080, in
	// quarter steps. Overridable.
	HDC screen = GetDC(nullptr);
	float dpiScale = GetDeviceCaps(screen, LOGPIXELSY) / 96.f;
	ReleaseDC(nullptr, screen);
	float heightScale = GetSystemMetrics(SM_CYSCREEN) / 1080.f;
	uiScale = prefs.Get("Overlay.UIScale", std::max(1.f, std::round(std::max(dpiScale, heightScale) * 4) / 4));
	showTPTMenus = prefs.Get("Overlay.ShowTPTMenus", false);
	text = new Text;
	text->SetScale();
	AddTrayIcon();
}

void OverlayWindows::Tick()
{
	if (!mainWindow || !&GameController::Ref())
	{
		return;
	}
	static bool areaRestored = false;
	if (!areaRestored)
	{
		auto &prefs = GlobalPrefs::Ref();
		SetPlayArea(prefs.Get("Overlay.PlayArea.X", 0), prefs.Get("Overlay.PlayArea.Y", 0),
		            prefs.Get("Overlay.PlayArea.W", CELLS.X), prefs.Get("Overlay.PlayArea.H", CELLS.Y));
		areaRestored = true;
	}
	if (!palette && GlobalPrefs::Ref().Get("Overlay.Windows.Palette.Open", true))
	{
		TogglePalette();
	}
	if (palette)
	{
		palette->Tick();
		if (palette->dirty)
		{
			palette->Present();
		}
	}
	if (toast)
	{
		toast->Tick();
	}
}

void OverlayWindows::TogglePalette()
{
	if (!&GameController::Ref())
	{
		return; // the tray icon exists before the game does
	}
	bool open = !palette || !IsWindowVisible(palette->hwnd);
	GlobalPrefs::Ref().Set("Overlay.Windows.Palette.Open", open);
	if (!open)
	{
		ShowWindow(palette->hwnd, SW_HIDE);
		return;
	}
	if (!palette)
	{
		palette = std::make_unique<SideWindow>();
		// Tall enough for every section in the navigation pane and for the Simulation tab.
		int w = S(560), h = std::max<int>(palette->NavRect(int(palette->Sections().size())).top + S(4) + palette->statusH + palette->border, S(560));
		// Default: just right of TPT's window, or its top-left corner if that's off screen.
		RECT rc;
		GetWindowRect(mainWindow, &rc);
		int x = rc.right + S(8), y = rc.top;
		if (x + w > GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN))
		{
			x = rc.left + S(8);
			y = rc.top + S(8);
		}
		auto &prefs = GlobalPrefs::Ref();
		POINT saved{ prefs.Get("Overlay.Windows.Palette.X", x), prefs.Get("Overlay.Windows.Palette.Y", y) };
		POINT grip{ saved.x + w / 2, saved.y + S(10) }; // the title bar must be on a monitor
		if (MonitorFromPoint(grip, MONITOR_DEFAULTTONULL))
		{
			x = saved.x;
			y = saved.y;
		}
		palette->Create(x, y, w, h);
	}
	palette->dirty = true;
	palette->Present();
	ShowWindow(palette->hwnd, SW_SHOWNOACTIVATE);
	ApplyPassThrough(*palette);
}

void OverlayWindows::SetPassThrough(bool on)
{
	passThrough = on;
	if (palette)
	{
		ApplyPassThrough(*palette);
	}
}

void OverlayWindows::Notify(const char *message)
{
	if (!toast)
	{
		toast = std::make_unique<Toast>();
	}
	toast->Show(mainWindow, Platform::WinWiden(message));
}
bool OverlayWindows::HideTPTMenus()
{
	return !showTPTMenus && palette && IsWindowVisible(palette->hwnd);
}

void OverlayWindows::ApplySimMasks(uint32_t *argb, int w)
{
	if (!&GameController::Ref())
	{
		return;
	}
	// ponytail: assumes the sim is drawn 1:1 at the top-left; the zoom window's magnified area is not mapped.
	auto *sim = GameController::Ref().GetModel()->GetSimulation();
	auto area = sim->PlayRect();
	bool full = sim->playArea == CELLS.OriginRect();
	for (int y = 0; y < YRES; y++)
		for (int x = 0; x < XRES; x++)
		{
			auto &p = argb[y * w + x];
			if (!area.Contains({ x, y }))
			{
				p = 0;
			}
			else if (!(p & 0xFFFFFF) && (sim->pmap[y][x] || sim->photons[y][x]))
			{
				p = 0xFF000000;
			}
		}
	if (!full)
	{
		// A faint frame just outside the area, so you can see where it ends.
		auto br = area.BottomRight();
		auto mark = [&](int x, int y) {
			if (x >= 0 && y >= 0 && x < XRES && y < YRES) argb[y * w + x] = 0x785080C0;
		};
		for (int x = area.pos.X - 1; x <= br.X + 1; x++) mark(x, area.pos.Y - 1), mark(x, br.Y + 1);
		for (int y = area.pos.Y; y <= br.Y; y++) mark(area.pos.X - 1, y), mark(br.X + 1, y);
	}
}

void OverlayWindows::SetPlayArea(int x, int y, int w, int h)
{
	if (!&GameController::Ref())
	{
		return;
	}
	auto area = RectSized(Vec2(x, y), Vec2(w, h)) & CELLS.OriginRect();
	if (area.size.X < 8 || area.size.Y < 8)
	{
		area = CELLS.OriginRect(); // too small to play in: back to the whole sim
	}
	GameController::Ref().GetModel()->GetSimulation()->playArea = area;
	auto &prefs = GlobalPrefs::Ref();
	prefs.Set("Overlay.PlayArea.X", area.pos.X);
	prefs.Set("Overlay.PlayArea.Y", area.pos.Y);
	prefs.Set("Overlay.PlayArea.W", area.size.X);
	prefs.Set("Overlay.PlayArea.H", area.size.Y);
}

#else
void OverlayWindows::Init(void *) {}
void OverlayWindows::Tick() {}
void OverlayWindows::TogglePalette() {}
void OverlayWindows::SetPassThrough(bool) {}
void OverlayWindows::Notify(const char *) {}
bool OverlayWindows::HideTPTMenus() { return false; }
void OverlayWindows::ApplySimMasks(uint32_t *, int) {}
void OverlayWindows::SetPlayArea(int, int, int, int) {}
#endif
