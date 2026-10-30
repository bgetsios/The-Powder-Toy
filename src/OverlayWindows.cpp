// Overlay mode's own desktop windows (OVERLAY.md): each menu is a real top-level layered window, drawn here in
// software with per-pixel alpha. The look is Windows Vista Aero; other OS themes will replace the vista:: drawing.
#include "OverlayWindows.h"
#ifdef _WIN32
#include "Overlay.h"
#include "gui/game/GameController.h"
#include "gui/game/Menu.h"
#include "gui/game/tool/Tool.h"
#include "prefs/GlobalPrefs.h"
#include "common/platform/Platform.h"
#include <algorithm>
#include <cmath>
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

	// The element palette: TPT's menu sections down the left (like Explorer's navigation pane), the section's
	// tools as Vista buttons on the right, the hovered tool's description in a status bar. Left click picks the
	// primary tool, right click the secondary, as in TPT.
	struct Palette : Panel
	{
		int titleH, border, navW, statusH, tileW, tileH, gap;
		int section = 0, scroll = 0;
		int hoverNav = -1, hoverTile = -1;
		bool hoverClose = false, hoverMin = false;
		Tool *shownPrimary = nullptr, *shownSecondary = nullptr;

		Palette()
		{
			name = "Palette";
			titleH = S(28), border = S(6), navW = S(118), statusH = S(22), tileW = S(78), tileH = S(24), gap = S(4);
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
		RECT ToolArea()
		{
			return { border + navW + gap, titleH + gap, cv.w - border - gap, cv.h - border - statusH };
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
			int h = S(22);
			return { border + S(3), titleH + S(4) + i * h, border + navW - S(3), titleH + S(4) + (i + 1) * h - S(1) };
		}

		void Paint() override
		{
			vista::Frame(cv, titleH, border, L"Elements");
			vista::CaptionButton(cv, MinRect(), hoverMin, false);
			vista::CaptionButton(cv, CloseRect(), hoverClose, true);
			// Client area: the navigation pane (pale blue, Vista Explorer) and the white content area.
			cv.RoundRect(border, titleH, navW, cv.h - titleH - border, 0, 0xF1F5FB, 0xE5EDF8, 255);
			cv.RoundRect(border + navW, titleH, cv.w - 2 * border - navW, cv.h - titleH - border, 0, 0xFFFFFF, 0xFFFFFF, 255);
			for (int j = titleH; j < cv.h - border; j++) cv.Blend(border + navW, j, 0xA0AFC3, 255);
			auto sections = Sections();
			for (int i = 0; i < int(sections.size()); i++)
			{
				auto rc = NavRect(i);
				vista::NavItem(cv, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, i == section ? 2 : (i == hoverNav ? 1 : 0));
				text->Draw(cv, rc.left + S(6), rc.top + S(3), Wide(sections[i]->GetDescription()), 0x1E395B, i == section, rc.right - rc.left - S(10));
			}
			auto tools = Tools();
			auto area = ToolArea();
			auto &gc = GameController::Ref();
			shownPrimary = gc.GetActiveTool(0);
			shownSecondary = gc.GetActiveTool(1);
			for (int i = 0; i < int(tools.size()); i++)
			{
				auto rc = TileRect(i);
				if (rc.bottom <= area.top || rc.top >= area.bottom)
				{
					continue;
				}
				auto *tool = tools[i];
				bool selected = tool == shownPrimary || tool == shownSecondary;
				Canvas tile;
				tile.Resize(tileW, tileH);
				vista::Button(tile, 0, 0, tileW, tileH, selected ? 2 : (i == hoverTile ? 1 : 0));
				if (tool == shownPrimary) tile.RoundFrame(0, 0, tileW, tileH, S(3), 0xD02020, 255);
				if (tool == shownSecondary) tile.RoundFrame(1, 1, tileW - 2, tileH - 2, S(3), 0x2050D0, 255);
				auto col = tool->Colour;
				uint32_t rgb = (col.Red << 16) | (col.Green << 8) | col.Blue;
				int sw = tileH - S(10);
				tile.RoundRect(S(5), S(5), sw, sw, S(2), rgb, rgb, 255);
				tile.RoundFrame(S(5), S(5), sw, sw, S(2), 0x404040, 200);
				text->Draw(tile, S(9) + sw, (tileH - text->Measure(Label(tool)).cy) / 2, Label(tool), 0x000000, false, tileW - sw - S(12));
				// Copy the tile in, clipped to the content area.
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
			}
			// The status bar.
			int sy = cv.h - border - statusH;
			cv.RoundRect(border + navW + 1, sy, cv.w - 2 * border - navW - 1, statusH, 0, 0xF7F9FC, 0xDCE5F1, 255);
			for (int i = border + navW + 1; i < cv.w - border; i++) cv.Blend(i, sy, 0xA0AFC3, 255);
			if (hoverTile >= 0 && hoverTile < int(tools.size()))
			{
				text->Draw(cv, border + navW + S(8), sy + S(3), Wide(tools[hoverTile]->Description), 0x1E395B, false, cv.w - 2 * border - navW - S(16));
			}
		}

		LRESULT HitTest(int x, int y) override
		{
			return y < titleH && !Inside(CloseRect(), x, y) && !Inside(MinRect(), x, y) ? HTCAPTION : HTCLIENT;
		}

		void Mouse(UINT msg, int x, int y, WPARAM wp) override
		{
			auto sections = Sections();
			auto tools = Tools();
			auto area = ToolArea();
			int nav = -1, tile = -1;
			for (int i = 0; i < int(sections.size()); i++)
				if (Inside(NavRect(i), x, y)) nav = i;
			if (Inside(area, x, y))
				for (int i = 0; i < int(tools.size()); i++)
					if (Inside(TileRect(i), x, y)) tile = i;
			bool close = Inside(CloseRect(), x, y), min = Inside(MinRect(), x, y);
			if (msg == WM_MOUSELEAVE)
			{
				nav = tile = -1;
				close = min = false;
			}
			if (nav != hoverNav || tile != hoverTile || close != hoverClose || min != hoverMin)
			{
				hoverNav = nav, hoverTile = tile, hoverClose = close, hoverMin = min;
				dirty = true;
			}
			if ((msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) && tile >= 0)
			{
				GameController::Ref().SetActiveTool(msg == WM_LBUTTONDOWN ? 0 : 1, tools[tile]);
				dirty = true;
			}
			if (msg == WM_LBUTTONDOWN && nav >= 0)
			{
				section = nav;
				scroll = 0;
				hoverTile = -1;
				dirty = true;
			}
			// Both hide the window; the tray icon brings it back. ponytail: no minimise animation to the tray yet.
			if (msg == WM_LBUTTONUP && (close || min))
			{
				OverlayWindows::TogglePalette();
			}
			if (msg == WM_MOUSEWHEEL)
			{
				int rows = (int(tools.size()) + Columns() - 1) / Columns();
				int maxScroll = std::max(0, rows * (tileH + gap) - int(area.bottom - area.top));
				scroll = std::clamp(scroll - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * (tileH + gap), 0, maxScroll);
				dirty = true;
			}
		}

		void Tick()
		{
			auto &gc = GameController::Ref();
			if (gc.GetActiveTool(0) != shownPrimary || gc.GetActiveTool(1) != shownSecondary)
			{
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
	std::unique_ptr<Palette> palette;
	std::unique_ptr<Toast> toast;
	bool passThrough = false;

	// The notification-area (tray) icon: left click shows or hides the Elements window, right click opens a menu.
	NOTIFYICONDATAW tray{};
	constexpr UINT trayMessage = WM_APP + 1;

	LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
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
			enum { elements = 1, inputBase = 10, exitItem = 99 };
			// ponytail: a native menu (Windows 11 look); a Vista-themed one comes with the theme system.
			HMENU menu = CreatePopupMenu();
			bool open = palette && IsWindowVisible(palette->hwnd);
			AppendMenuW(menu, MF_STRING | (open ? MF_CHECKED : 0), elements, L"Elements window	Ctrl+Alt+E");
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			const wchar_t *modes[] = { L"Input: Auto", L"Input: Pass-through", L"Input: Locked to TPT" };
			for (int i = 0; i < int(OverlayInput::Count); i++)
			{
				AppendMenuW(menu, MF_STRING | (int(OverlayGetInput()) == i ? MF_CHECKED : 0), inputBase + i, modes[i]);
			}
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
		tray.hWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
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
	// Default UI scale from the screen height (1.0 at 1080p), overridable.
	int screenH = GetSystemMetrics(SM_CYSCREEN);
	uiScale = prefs.Get("Overlay.UIScale", std::max(1.f, std::round(screenH / 1080.f * 4) / 4));
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
	bool open = !palette || !IsWindowVisible(palette->hwnd);
	GlobalPrefs::Ref().Set("Overlay.Windows.Palette.Open", open);
	if (!open)
	{
		ShowWindow(palette->hwnd, SW_HIDE);
		return;
	}
	if (!palette)
	{
		palette = std::make_unique<Palette>();
		// Tall enough for every section in the navigation pane.
		int w = S(560), h = palette->NavRect(int(palette->Sections().size())).top + S(4) + palette->statusH / 2 + palette->border;
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
		x = prefs.Get("Overlay.Windows.Palette.X", x);
		y = prefs.Get("Overlay.Windows.Palette.Y", y);
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
#else
void OverlayWindows::Init(void *) {}
void OverlayWindows::Tick() {}
void OverlayWindows::TogglePalette() {}
void OverlayWindows::SetPassThrough(bool) {}
void OverlayWindows::Notify(const char *) {}
#endif
