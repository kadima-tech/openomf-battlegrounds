// The Windows 10/11 launcher: the same service-bay setup screen as Launcher.swift, drawn with
// GDI+ in a per-monitor DPI-aware Win32 window. All decisions live in core.cpp's LauncherModel.
#include "core.h"

#include <commctrl.h>
#include <dbt.h>
#include <dwmapi.h>
#include <objidl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

using namespace obg;
namespace gp = Gdiplus;

namespace {

constexpr UINT WM_APP_RUN = WM_APP + 1;
constexpr UINT_PTR TIMER_SPINNER = 1, TIMER_SCREENSHOT = 2;
enum Control { ID_ISO = 101, ID_CD, ID_RETRY, ID_LOG, ID_LAUNCH, ID_SETTINGS, ID_HELP, ID_COUNT };

const gp::Color amber(255, 255, 181, 41), ivory(255, 232, 227, 212), muted(255, 135, 143, 140), bay(255, 20, 23, 24);
gp::Color alpha(const gp::Color &c, double a) { return gp::Color(BYTE(std::clamp(a, 0.0, 1.0) * 255), c.GetR(), c.GetG(), c.GetB()); }
// A translucent colour flattened onto the bay background (for areas painted without blending).
gp::Color over_bay(const gp::Color &c, double a) {
	auto mix = [a](BYTE x, BYTE y) { return BYTE(std::lround(x * a + y * (1 - a))); };
	return gp::Color(255, mix(c.GetR(), bay.GetR()), mix(c.GetG(), bay.GetG()), mix(c.GetB(), bay.GetB()));
}

struct Fonts {
	std::unique_ptr<gp::Font> brand, title, subtitle, iso, cd, launch, body, small, tiny, label, mono9, mono8, mono10, mono11,
	    stage_title, icon, icon_large;
};

// The first installed family; Windows 10 and 11 ship all the preferred ones.
std::unique_ptr<gp::FontFamily> family(std::initializer_list<const wchar_t *> names, bool monospace = false) {
	for (auto name : names) {
		auto f = std::make_unique<gp::FontFamily>(name);
		if (f->GetLastStatus() == gp::Ok && f->IsAvailable()) return f;
	}
	return std::unique_ptr<gp::FontFamily>((monospace ? gp::FontFamily::GenericMonospace() : gp::FontFamily::GenericSansSerif())->Clone());
}

class Window {
public:
	explicit Window(LauncherModel &model) : model_(model) {}
	bool create(HINSTANCE instance);
	void show();
	HWND hwnd() const { return hwnd_; }
	void screenshot_after_show(std::wstring path) { screenshot_ = std::move(path); }
	void refresh() { relayout(); }  // the model changed; the error panel may have come or gone
	void confirm_install();

private:
	LauncherModel &model_;
	HWND hwnd_ = nullptr;
	HWND buttons_[ID_COUNT] = {};
	double scale_ = 1;
	Fonts fonts_;
	std::unique_ptr<gp::FontFamily> display_, ui_, mono_, icons_;
	bool has_icons_ = false;
	std::unique_ptr<gp::Bitmap> blueprint_;
	bool watching_ = false, shown_ = false;
	std::wstring screenshot_;

	struct Layout {
		gp::RectF iso, cd, source, stages, error, retry, log, launch, settings, help, footer;
		float row = 59, row_pad = 11;
	} layout_;

	static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
	LRESULT handle(UINT, WPARAM, LPARAM);
	float px(float dip) const { return float(dip * scale_); }
	void set_dpi(UINT dpi);
	void make_fonts();
	void relayout();
	void sync();
	void paint(gp::Graphics &g, bool with_buttons);
	void paint_sidebar(gp::Graphics &g, float height);
	void paint_stages(gp::Graphics &g);
	void draw_button(gp::Graphics &g, int id, const gp::RectF &r, bool enabled, bool focused, bool pressed);
	void text(gp::Graphics &g, const std::wstring &s, const gp::Font *font, const gp::Color &color, const gp::RectF &r,
	          gp::StringAlignment h = gp::StringAlignmentNear, gp::StringAlignment v = gp::StringAlignmentNear,
	          bool wrap = false);
	// A Segoe icon-font glyph; nothing at all on a system without the font.
	void icon(gp::Graphics &g, const wchar_t *glyph, const gp::Font *font, const gp::Color &color, const gp::RectF &r,
	          gp::StringAlignment h = gp::StringAlignmentNear) {
		if (has_icons_) text(g, glyph, font, color, r, h, gp::StringAlignmentCenter);
	}
	// One line shrunk to fit its width (SwiftUI's minimumScaleFactor; lower for wide fallback fonts).
	void fitted(gp::Graphics &g, const std::wstring &s, const gp::Font *font, const gp::Color &color, const gp::RectF &r);
	float text_height(const std::wstring &s, const gp::Font *font, float width);
	void choose_iso();
	void use_cd();
	void watch_discs();
	void rescan();
	void help();
	void settings();
	bool confirm_close();
	void save_screenshot();
	gp::RectF button_rect(int id) const;
	bool enabled(int id) const;
};

// ---------------------------------------------------------------- window plumbing

bool Window::create(HINSTANCE instance) {
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof wc;
	wc.lpfnWndProc = proc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
	wc.hbrBackground = nullptr;
	wc.lpszClassName = L"OpenBGLauncher";
	RegisterClassExW(&wc);
	UINT dpi = GetDpiForSystem();
	scale_ = dpi / 96.0;
	RECT r{0, 0, LONG(px(1160)), LONG(px(820))};
	AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
	RECT work{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	int w = std::min<int>(r.right - r.left, work.right - work.left), h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
	hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT, wc.lpszClassName, L"One Must Fall: Battlegrounds",
	                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, work.left + (work.right - work.left - w) / 2,
	                        work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, instance, this);
	if (!hwnd_) return false;
	// Dark title bar (Windows 10 20H1+ uses attribute 20, earlier builds 19) and, on Windows 11,
	// a caption in the bay's own colour. Older systems ignore what they do not support.
	BOOL dark = TRUE;
	if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark))) DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof dark);
	COLORREF caption = RGB(20, 23, 24), caption_text = RGB(232, 227, 212);
	DwmSetWindowAttribute(hwnd_, 35, &caption, sizeof caption);
	DwmSetWindowAttribute(hwnd_, 36, &caption_text, sizeof caption_text);

	static const wchar_t *labels[ID_COUNT] = {};
	labels[ID_ISO] = L"Choose ISO file";
	labels[ID_CD] = L"Use game CD";
	labels[ID_RETRY] = L"Try again";
	labels[ID_LOG] = L"Open setup log";
	labels[ID_LAUNCH] = L"Launch game";
	labels[ID_SETTINGS] = L"Settings";
	labels[ID_HELP] = L"Help";
	// Files dropped on a button (the ISO panel above all) reach the window like any other drop.
	auto forward_drops = [](HWND button, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) -> LRESULT {
		if (msg == WM_DROPFILES) return SendMessageW(GetParent(button), msg, wp, lp);
		return DefSubclassProc(button, msg, wp, lp);
	};
	for (int id = ID_ISO; id < ID_COUNT; ++id) {
		buttons_[id] = CreateWindowExW(WS_EX_ACCEPTFILES, L"BUTTON", labels[id], WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
		                               0, hwnd_, HMENU(INT_PTR(id)), instance, nullptr);
		SetWindowSubclass(buttons_[id], forward_drops, 1, 0);
	}
	ui_ = family({L"Segoe UI"});
	display_ = family({L"Bahnschrift SemiBold Condensed", L"Bahnschrift Condensed", L"Bahnschrift", L"Segoe UI"});
	mono_ = family({L"Cascadia Mono", L"Consolas", L"Courier New"}, true);
	icons_ = family({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
	{
		wchar_t name[LF_FACESIZE] = {};
		icons_->GetFamilyName(name);
		has_icons_ = wcsstr(name, L"Segoe") != nullptr;
	}
	blueprint_.reset(gp::Bitmap::FromFile((model_.where.resources / L"assets" / L"robot-blueprint.png").c_str()));
	if (blueprint_ && blueprint_->GetLastStatus() != gp::Ok) blueprint_.reset();
	set_dpi(GetDpiForWindow(hwnd_));
	return true;
}

void Window::show() {
	if (!shown_) {
		shown_ = true;
		ShowWindow(hwnd_, SW_SHOWNORMAL);
		if (!screenshot_.empty()) SetTimer(hwnd_, TIMER_SCREENSHOT, 1000, nullptr);
	} else {
		ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
	}
	SetForegroundWindow(hwnd_);
	if (!watching_) watch_discs();
	sync();
}

LRESULT CALLBACK Window::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_NCCREATE) {
		auto self = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
		self->hwnd_ = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self));
	}
	auto self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Window::handle(UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_APP_RUN: {
		std::unique_ptr<std::function<void()>> f(reinterpret_cast<std::function<void()> *>(lp));
		(*f)();
		return 0;
	}
	case WM_DPICHANGED: {
		set_dpi(HIWORD(wp));
		auto r = reinterpret_cast<RECT *>(lp);
		SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_GETMINMAXINFO: {
		RECT r{0, 0, LONG(px(1000)), LONG(px(720))};
		AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, UINT(scale_ * 96));
		auto info = reinterpret_cast<MINMAXINFO *>(lp);
		info->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
		return 0;
	}
	case WM_SIZE:
		relayout();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd_, &ps);
		RECT client;
		GetClientRect(hwnd_, &client);
		if (client.right > 0 && client.bottom > 0) {
			HDC memory = CreateCompatibleDC(dc);
			HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
			HGDIOBJ old = SelectObject(memory, bitmap);
			{
				gp::Graphics g(memory);
				paint(g, false);
			}
			BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
			       memory, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
			SelectObject(memory, old);
			DeleteObject(bitmap);
			DeleteDC(memory);
		}
		EndPaint(hwnd_, &ps);
		return 0;
	}
	case WM_DRAWITEM: {
		auto item = reinterpret_cast<DRAWITEMSTRUCT *>(lp);
		RECT rc = item->rcItem;
		int w = rc.right - rc.left, h = rc.bottom - rc.top;
		if (w <= 0 || h <= 0) return TRUE;
		gp::Bitmap buffer(w, h, PixelFormat32bppARGB);
		{
			gp::Graphics g(&buffer);
			gp::RectF origin = button_rect(int(item->CtlID));
			g.TranslateTransform(-origin.X, -origin.Y);
			draw_button(g, int(item->CtlID), origin, !(item->itemState & ODS_DISABLED),
			            (item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT), item->itemState & ODS_SELECTED);
		}
		gp::Graphics out(item->hDC);
		out.DrawImage(&buffer, 0, 0, w, h);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case ID_ISO: choose_iso(); break;
		case ID_CD: use_cd(); break;
		case ID_RETRY:
			if (model_.last_source) model_.request_install(*model_.last_source);
			break;
		case ID_LOG: {
			std::wstring log = model_.log_to_open();
			std::error_code ec;
			if (fs::exists(log, ec)) ShellExecuteW(hwnd_, L"open", L"notepad.exe", quote_argument(log).c_str(), nullptr, SW_SHOWNORMAL);
			else ShellExecuteW(hwnd_, L"open", model_.where.home.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			break;
		}
		case IDOK:  // Enter
		case ID_LAUNCH:
			if (enabled(ID_LAUNCH)) model_.launch();
			break;
		case ID_SETTINGS: settings(); break;
		case ID_HELP: help(); break;
		}
		return 0;
	case WM_DROPFILES: {
		HDROP drop = HDROP(wp);
		wchar_t path[32768];
		if (DragQueryFileW(drop, 0, path, UINT(std::size(path)))) model_.request_install(path);
		DragFinish(drop);
		return 0;
	}
	case WM_DEVICECHANGE:
		if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) rescan();
		return TRUE;
	case WM_TIMER:
		if (wp == TIMER_SPINNER) {
			RECT r{LONG(layout_.stages.X), LONG(layout_.stages.Y), LONG(layout_.stages.GetRight()), LONG(layout_.stages.GetBottom())};
			InvalidateRect(hwnd_, &r, FALSE);
		} else if (wp == TIMER_SCREENSHOT) {
			KillTimer(hwnd_, TIMER_SCREENSHOT);
			save_screenshot();
			PostQuitMessage(0);
		}
		return 0;
	case WM_CLOSE:
		if (confirm_close()) DestroyWindow(hwnd_);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Window::set_dpi(UINT dpi) {
	scale_ = (dpi ? dpi : 96) / 96.0;
	make_fonts();
	relayout();
}

void Window::make_fonts() {
	auto font = [this](gp::FontFamily *f, float size, int style = gp::FontStyleRegular) {
		return std::make_unique<gp::Font>(f, px(size), style, gp::UnitPixel);
	};
	bool bahnschrift_named = false;
	{
		wchar_t name[LF_FACESIZE] = {};
		display_->GetFamilyName(name);
		bahnschrift_named = wcsstr(name, L"SemiBold") != nullptr;
	}
	int display_style = bahnschrift_named ? gp::FontStyleRegular : gp::FontStyleBold;
	fonts_.brand = font(display_.get(), 38, display_style);
	fonts_.title = font(display_.get(), 44, display_style);
	fonts_.iso = font(display_.get(), 27, display_style);
	fonts_.cd = font(display_.get(), 24, display_style);
	fonts_.launch = font(display_.get(), 24, display_style);
	fonts_.subtitle = font(ui_.get(), 16);
	fonts_.body = font(ui_.get(), 12);
	fonts_.small = font(ui_.get(), 11);
	fonts_.tiny = font(ui_.get(), 10);
	fonts_.label = font(ui_.get(), 10, gp::FontStyleBold);
	fonts_.stage_title = font(ui_.get(), 13, gp::FontStyleBold);
	fonts_.mono8 = font(mono_.get(), 8);
	fonts_.mono9 = font(mono_.get(), 9);
	fonts_.mono10 = font(mono_.get(), 10);
	fonts_.mono11 = font(mono_.get(), 11);
	fonts_.icon = font(icons_.get(), 13);
	fonts_.icon_large = font(icons_.get(), 11, gp::FontStyleBold);
}

float Window::text_height(const std::wstring &s, const gp::Font *font, float width) {
	HDC dc = GetDC(hwnd_);
	float h;
	{
		gp::Graphics g(dc);
		gp::RectF box;
		gp::StringFormat format;
		g.MeasureString(s.c_str(), INT(s.size()), font, gp::RectF(0, 0, width, 10000), &format, &box);
		h = box.Height;
	}
	ReleaseDC(hwnd_, dc);
	return h;
}

// Fixed header and footer; the content between adapts to the window: the disc buttons and stage
// rows give up space before anything overlaps.
void Window::relayout() {
	RECT client;
	GetClientRect(hwnd_, &client);
	float W = float(client.right), H = float(client.bottom);
	float x0 = px(266), pad = px(38), width = std::max(px(200), W - x0 - 2 * pad);
	float top = px(161), footer = px(61);
	bool error = model_.error.has_value();
	float error_h = 0;
	if (error) error_h = px(15) + std::max(px(16), text_height(*model_.error, fonts_.body.get(), width - px(30) - px(22))) + px(12) + px(26) + px(15);
	float source_h = px(136), row_pad = px(11);
	auto total = [&] {
		float stages = px(16) + px(12) + px(14) + 3 * (px(1) + 2 * row_pad + px(36)) + px(16);
		return source_h + px(12) + px(16) + px(20) + stages + px(20) + (error ? error_h + px(20) : 0) + px(51) + px(18);
	};
	float available = H - footer - top;
	while (total() > available && source_h > px(96)) source_h -= px(4);
	while (total() > available && row_pad > px(5)) row_pad -= px(1);
	layout_.row_pad = row_pad;
	layout_.row = px(1) + 2 * row_pad + px(36);
	float y = top;
	layout_.iso = gp::RectF(x0 + pad, y, width - px(15) - px(171), source_h);
	layout_.cd = gp::RectF(layout_.iso.GetRight() + px(15), y, px(171), source_h);
	y += source_h + px(12);
	layout_.source = gp::RectF(x0 + pad, y, width, px(16));
	y += px(16) + px(20);
	float stages_h = px(16) + px(12) + px(14) + 3 * layout_.row + px(16);
	layout_.stages = gp::RectF(x0 + pad, y, width, stages_h);
	y += stages_h + px(20);
	if (error) {
		layout_.error = gp::RectF(x0 + pad, y, width, error_h);
		float by = y + error_h - px(15) - px(26);
		bool retry = model_.last_source && !model_.ready;
		layout_.retry = retry ? gp::RectF(x0 + pad + px(15), by, px(92), px(26)) : gp::RectF();
		layout_.log = gp::RectF(x0 + pad + px(15) + (retry ? px(100) : 0), by, px(124), px(26));
		y += error_h + px(20);
	} else {
		layout_.error = layout_.retry = layout_.log = gp::RectF();
	}
	layout_.launch = gp::RectF(x0 + pad, y, width, px(51));
	layout_.footer = gp::RectF(x0, H - footer, W - x0, footer);
	float fy = H - footer + px(20);
	layout_.help = gp::RectF(W - px(30) - px(62), fy, px(62), px(20));
	layout_.settings = gp::RectF(layout_.help.X - px(13) - px(84), fy, px(84), px(20));
	sync();
}

gp::RectF Window::button_rect(int id) const {
	switch (id) {
	case ID_ISO: return layout_.iso;
	case ID_CD: return layout_.cd;
	case ID_RETRY: return layout_.retry;
	case ID_LOG: return layout_.log;
	case ID_LAUNCH: return layout_.launch;
	case ID_SETTINGS: return layout_.settings;
	case ID_HELP: return layout_.help;
	}
	return {};
}

bool Window::enabled(int id) const {
	switch (id) {
	case ID_ISO: return !model_.busy && !model_.ready;
	case ID_CD: return !model_.busy && !model_.ready && model_.disc.has_value();
	case ID_RETRY: return !model_.busy;
	case ID_LAUNCH: return model_.ready && !model_.game;
	}
	return true;
}

// Brings child buttons, the spinner and the paint in line with the model after any change.
void Window::sync() {
	if (!hwnd_) return;
	for (int id = ID_ISO; id < ID_COUNT; ++id) {
		gp::RectF r = button_rect(id);
		bool visible = r.Width > 0 && r.Height > 0;
		SetWindowPos(buttons_[id], nullptr, int(std::lround(r.X)), int(std::lround(r.Y)), int(std::lround(r.Width)),
		             int(std::lround(r.Height)), SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
		EnableWindow(buttons_[id], enabled(id));
		InvalidateRect(buttons_[id], nullptr, FALSE);
	}
	SetWindowTextW(buttons_[ID_LAUNCH], model_.busy ? L"Preparing your game\u2026" : L"Launch game");
	bool spinning = std::any_of(model_.stages.begin(), model_.stages.end(), [](const Stage &s) { return s.status == "active"; });
	if (spinning) SetTimer(hwnd_, TIMER_SPINNER, 60, nullptr);
	else KillTimer(hwnd_, TIMER_SPINNER);
	InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- drawing

void Window::text(gp::Graphics &g, const std::wstring &s, const gp::Font *font, const gp::Color &color, const gp::RectF &r,
                  gp::StringAlignment h, gp::StringAlignment v, bool wrap) {
	gp::StringFormat format;
	format.SetAlignment(h);
	format.SetLineAlignment(v);
	format.SetTrimming(gp::StringTrimmingEllipsisCharacter);
	if (!wrap) format.SetFormatFlags(gp::StringFormatFlagsNoWrap);
	gp::SolidBrush brush(color);
	g.DrawString(s.c_str(), INT(s.size()), font, r, &format, &brush);
}

void Window::fitted(gp::Graphics &g, const std::wstring &s, const gp::Font *font, const gp::Color &color, const gp::RectF &r) {
	gp::FontFamily family;
	font->GetFamily(&family);
	gp::StringFormat format;
	format.SetFormatFlags(gp::StringFormatFlagsNoWrap);
	float size = font->GetSize(), smallest = size * 0.5f;
	for (;;) {
		gp::Font candidate(&family, size, font->GetStyle(), gp::UnitPixel);
		gp::RectF box;
		g.MeasureString(s.c_str(), INT(s.size()), &candidate, gp::RectF(0, 0, 100000, r.Height), &format, &box);
		if (box.Width <= r.Width || size <= smallest)
			return text(g, s, &candidate, color, r, gp::StringAlignmentNear, gp::StringAlignmentCenter);
		size = std::max(smallest, size * 0.96f);
	}
}

void Window::paint(gp::Graphics &g, bool with_buttons) {
	RECT client;
	GetClientRect(hwnd_, &client);
	float W = float(client.right), H = float(client.bottom);
	g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
	g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
	g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
	gp::SolidBrush background(bay);
	g.FillRectangle(&background, 0.f, 0.f, W, H);
	paint_sidebar(g, H);
	gp::SolidBrush divider(alpha(amber, 0.5));
	g.FillRectangle(&divider, px(265), 0.f, std::max(1.f, px(1)), H);

	float x0 = px(266), pad = px(38), right = W - pad;
	// Header
	float y = px(26);
	gp::SolidBrush dot(amber);
	g.FillEllipse(&dot, x0 + pad, y + px(3.5f), px(5), px(5));
	text(g, model_.ready ? L"ARENA SYSTEMS ONLINE" : L"FIRST-TIME SETUP", fonts_.mono9.get(), amber,
	     gp::RectF(x0 + pad + px(12), y, px(300), px(13)));
	text(g, L"SERVICE BAY  /  01", fonts_.mono9.get(), muted, gp::RectF(right - px(300), y, px(300), px(13)), gp::StringAlignmentFar);
	fitted(g, model_.ready ? L"You're ready for the arena." : L"Prepare for the arena.", fonts_.title.get(), ivory,
	       gp::RectF(x0 + pad - px(2), y + px(22), right - x0 - pad, px(54)));
	text(g, model_.busy ? L"Setting things up. This may take a few minutes." : L"Choose your game disc or ISO. We'll handle the rest.",
	     fonts_.subtitle.get(), alpha(ivory, 0.65), gp::RectF(x0 + pad, y + px(83), right - x0 - pad, px(24)));

	// Source line
	{
		// A small disc, or a tick once a source has been chosen.
		gp::Pen pen(muted, std::max(1.f, px(1)));
		float cx = layout_.source.X + px(6), cy = layout_.source.Y + layout_.source.Height / 2;
		if (model_.source_name) {
			gp::PointF mark[] = {{cx - px(5), cy}, {cx - px(1.5f), cy + px(3.5f)}, {cx + px(5), cy - px(4)}};
			g.DrawLines(&pen, mark, 3);
		} else {
			g.DrawEllipse(&pen, cx - px(6), cy - px(6), px(12), px(12));
			g.DrawEllipse(&pen, cx - px(1.5f), cy - px(1.5f), px(3), px(3));
		}
	}
	text(g, model_.source_name.value_or(L"Your game CD is detected automatically when inserted."), fonts_.small.get(), muted,
	     gp::RectF(layout_.source.X + px(20), layout_.source.Y, layout_.source.Width - px(20), layout_.source.Height),
	     gp::StringAlignmentNear, gp::StringAlignmentCenter);

	paint_stages(g);

	if (model_.error) {
		gp::RectF r = layout_.error;
		gp::SolidBrush panel(over_bay(amber, 0.06));
		g.FillRectangle(&panel, r);
		icon(g, L"\uE7BA", fonts_.icon.get(), amber, gp::RectF(r.X + px(15), r.Y + px(15), px(20), px(18)));
		text(g, *model_.error, fonts_.body.get(), amber,
		     gp::RectF(r.X + px(15) + px(22), r.Y + px(14), r.Width - px(30) - px(22), r.Height - px(14) - px(15 + 26 + 10)),
		     gp::StringAlignmentNear, gp::StringAlignmentNear, true);
	}

	// Footer
	gp::SolidBrush rule(alpha(ivory, 0.15));
	g.FillRectangle(&rule, layout_.footer.X, layout_.footer.Y, layout_.footer.Width, std::max(1.f, px(1)));
	float fy = layout_.footer.Y + px(20);
	icon(g, L"\uE8B7", fonts_.icon.get(), muted, gp::RectF(x0 + px(30), fy, px(18), px(20)));
	text(g, L"Everything stays in your game folder.", fonts_.tiny.get(), muted, gp::RectF(x0 + px(52), fy, px(320), px(20)),
	     gp::StringAlignmentNear, gp::StringAlignmentCenter);
	gp::SolidBrush bar(alpha(muted, 0.3));
	g.FillRectangle(&bar, layout_.settings.GetRight() + px(6), fy + px(2), std::max(1.f, px(1)), px(16));

	if (with_buttons)
		for (int id = ID_ISO; id < ID_COUNT; ++id) {
			gp::RectF r = button_rect(id);
			if (r.Width > 0 && r.Height > 0) draw_button(g, id, r, enabled(id), false, false);
		}
}

void Window::paint_sidebar(gp::Graphics &g, float H) {
	float w = px(265), x = px(25), inner = w - 2 * x;
	gp::LinearGradientBrush gradient(gp::PointF(0, 0), gp::PointF(w, H), gp::Color(255, 26, 26, 26), gp::Color(255, 14, 14, 14));
	g.FillRectangle(&gradient, 0.f, 0.f, w, H);
	gp::SolidBrush rule(alpha(amber, 0.65));
	g.FillRectangle(&rule, x, px(31), inner, std::max(1.f, px(1)));
	fitted(g, L"ONE MUST FALL", fonts_.brand.get(), ivory, gp::RectF(x - px(2), px(41), inner + px(4), px(46)));
	text(g, L"B A T T L E G R O U N D S", fonts_.label.get(), amber, gp::RectF(x, px(88), inner, px(14)));
	float oy = px(117);
	gp::SolidBrush line(muted);
	text(g, L"OPEN ENGINE", fonts_.mono8.get(), muted, gp::RectF(x, oy - px(6), inner, px(12)), gp::StringAlignmentCenter);
	g.FillRectangle(&line, x, oy, inner / 2 - px(40), std::max(1.f, px(1)));
	g.FillRectangle(&line, x + inner / 2 + px(40), oy, inner / 2 - px(40), std::max(1.f, px(1)));

	float stripe_y = H - px(22) - px(10), combat_y = stripe_y - px(24) - px(11), arena_y = combat_y - px(8) - px(12);
	gp::RectF art(x, oy + px(36), inner, std::max(px(40), arena_y - px(20) - (oy + px(36))));
	g.SetClip(art);
	gp::Pen grid(alpha(ivory, 0.04), std::max(0.5f, px(0.5f)));
	for (float gx = art.X; gx <= art.GetRight(); gx += px(24)) g.DrawLine(&grid, gx, art.Y, gx, art.GetBottom());
	for (float gy = art.Y; gy <= art.GetBottom(); gy += px(24)) g.DrawLine(&grid, art.X, gy, art.GetRight(), gy);
	if (blueprint_) {
		float iw = float(blueprint_->GetWidth()), ih = float(blueprint_->GetHeight());
		float fit = std::min(art.Width / iw, art.Height / ih) * 1.15f;
		gp::RectF dest(art.X + (art.Width - iw * fit) / 2, art.Y + (art.Height - ih * fit) / 2, iw * fit, ih * fit);
		// "Lighten" blend: the dark drafting sheet becomes transparent, its bright lines stay.
		gp::ColorMatrix m = {{{1, 0, 0, 0.42f, 0}, {0, 1, 0, 0.82f, 0}, {0, 0, 1, 0.16f, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, -0.1f, 1}}};
		gp::ImageAttributes attributes;
		attributes.SetColorMatrix(&m);
		g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
		g.DrawImage(blueprint_.get(), dest, 0, 0, iw, ih, gp::UnitPixel, &attributes);
	}
	g.ResetClip();
	text(g, L"ARENA SYSTEMS", fonts_.mono9.get(), muted, gp::RectF(x, arena_y, inner, px(12)));
	text(g, L"2.123", fonts_.mono9.get(), amber, gp::RectF(x, arena_y, inner, px(12)), gp::StringAlignmentFar);
	text(g, L"COMBAT CHASSIS / STANDBY", fonts_.mono8.get(), alpha(muted, 0.7), gp::RectF(x, combat_y, inner, px(11)));
	gp::RectF stripe(x, stripe_y, inner, px(10));
	g.SetClip(stripe);
	gp::SolidBrush hazard(alpha(amber, 0.55));
	for (float sx = x - px(30); sx < x + inner + px(30); sx += px(26)) {
		gp::PointF p[] = {{sx, stripe_y}, {sx + px(13), stripe_y}, {sx - px(3), stripe_y + px(10)}, {sx - px(16), stripe_y + px(10)}};
		g.FillPolygon(&hazard, p, 4);
	}
	g.ResetClip();
}

void Window::paint_stages(gp::Graphics &g) {
	gp::RectF r = layout_.stages;
	gp::SolidBrush panel(over_bay(ivory, 0.018));
	g.FillRectangle(&panel, r);
	gp::Pen border(alpha(ivory, 0.18), std::max(1.f, px(1)));
	g.DrawRectangle(&border, r.X, r.Y, r.Width, r.Height);
	float x = r.X + px(16), w = r.Width - px(32), y = r.Y + px(16);
	text(g, L"GAME SETUP", fonts_.mono9.get(), muted, gp::RectF(x, y, px(200), px(12)));
	text(g, L"2.123", fonts_.mono9.get(), amber, gp::RectF(x, y, w, px(12)), gp::StringAlignmentFar);
	text(g, L"TARGET VERSION", fonts_.mono9.get(), muted, gp::RectF(x, y, w - px(40), px(12)), gp::StringAlignmentFar);
	y += px(12) + px(14);
	gp::SolidBrush rule(alpha(ivory, 0.1));
	for (size_t k = 0; k < model_.stages.size(); ++k) {
		const Stage &s = model_.stages[k];
		g.FillRectangle(&rule, x, y, w, std::max(1.f, px(1)));
		float cy = y + px(1) + layout_.row_pad, mid = cy + px(18);
		wchar_t number[4];
		swprintf(number, 4, L"%02d", int(k + 1));
		text(g, number, fonts_.mono11.get(), muted, gp::RectF(x, mid - px(8), px(24), px(16)), gp::StringAlignmentNear, gp::StringAlignmentCenter);
		gp::RectF circle(x + px(38), mid - px(12.5f), px(25), px(25));
		gp::Pen ring(s.status == "waiting" ? alpha(muted, 0.5) : alpha(amber, 0.8), std::max(1.f, px(1)));
		g.DrawEllipse(&ring, circle);
		if (s.status == "done") {
			// A drawn tick: no dependence on the icon font.
			gp::Pen tick(amber, std::max(1.5f, px(1.75f)));
			gp::PointF mark[] = {{circle.X + px(7.5f), circle.Y + px(13)}, {circle.X + px(11), circle.Y + px(16.5f)}, {circle.X + px(17.5f), circle.Y + px(9)}};
			g.DrawLines(&tick, mark, 3);
		} else if (s.status == "active") {
			gp::Pen arc(amber, std::max(1.5f, px(2)));
			gp::RectF inner(circle.X + px(6), circle.Y + px(6), circle.Width - px(12), circle.Height - px(12));
			g.DrawArc(&arc, inner, float((GetTickCount64() / 4) % 360), 270.f);
		}
		float tx = circle.GetRight() + px(15), tw = w - (tx - x) - px(90);
		text(g, s.title, fonts_.stage_title.get(), ivory, gp::RectF(tx, cy, tw, px(18)));
		text(g, s.detail, fonts_.small.get(), muted, gp::RectF(tx, cy + px(20), tw, px(16)));
		std::wstring status = s.status == "done" ? L"Ready" : s.status == "active" ? L"Working\u2026" : L"Waiting";
		text(g, status, fonts_.mono10.get(), s.status == "waiting" ? muted : amber, gp::RectF(x, mid - px(8), w, px(16)),
		     gp::StringAlignmentFar, gp::StringAlignmentCenter);
		y += layout_.row;
	}
}

void Window::draw_button(gp::Graphics &g, int id, const gp::RectF &r, bool on, bool focused, bool pressed) {
	g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
	g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
	g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
	bool idle = !model_.busy && !model_.ready;
	double fade = (id == ID_ISO || id == ID_CD) && !idle ? 0.45 : 1.0;
	gp::SolidBrush base(id == ID_RETRY || id == ID_LOG ? over_bay(amber, 0.06) : bay);
	g.FillRectangle(&base, r);
	switch (id) {
	case ID_ISO: {
		gp::SolidBrush fill(over_bay(amber, pressed ? 0.10 : 0.025));
		g.FillRectangle(&fill, r);
		gp::Pen dash(alpha(amber, 0.8 * fade), std::max(1.f, px(1.5f)));
		float pattern[] = {7.f / 1.5f, 5.f / 1.5f};
		dash.SetDashPattern(pattern, 2);
		g.DrawRectangle(&dash, r.X + px(1), r.Y + px(1), r.Width - px(2), r.Height - px(2));
		gp::Color c = alpha(amber, fade);
		// A document with a plus: drawn, so it needs no icon font.
		float cx = r.X + r.Width / 2, top = r.Y + (r.Height - px(92)) / 2;  // icon, title and hint: 92 tall
		gp::Pen pen(c, std::max(1.f, px(1)));
		gp::PointF doc[] = {{cx - px(12), top}, {cx + px(5), top}, {cx + px(13), top + px(8)}, {cx + px(13), top + px(32)}, {cx - px(12), top + px(32)}};
		g.DrawPolygon(&pen, doc, 5);
		g.DrawLine(&pen, cx + px(5), top, cx + px(5), top + px(8));
		g.DrawLine(&pen, cx + px(5), top + px(8), cx + px(13), top + px(8));
		g.DrawLine(&pen, cx, top + px(12), cx, top + px(24));
		g.DrawLine(&pen, cx - px(6), top + px(18), cx + px(6), top + px(18));
		text(g, L"Choose ISO file", fonts_.iso.get(), c, gp::RectF(r.X, top + px(38), r.Width, px(32)), gp::StringAlignmentCenter);
		text(g, L"or drop an ISO here", fonts_.body.get(), alpha(ivory, 0.5 * fade), gp::RectF(r.X, top + px(74), r.Width, px(18)),
		     gp::StringAlignmentCenter);
		break;
	}
	case ID_CD: {
		gp::SolidBrush fill(over_bay(ivory, pressed ? 0.06 : 0.025));
		g.FillRectangle(&fill, r);
		gp::Pen frame(alpha(ivory, 0.23 * fade), std::max(1.f, px(1)));
		g.DrawRectangle(&frame, r.X, r.Y, r.Width - px(1), r.Height - px(1));
		gp::Color c = model_.disc ? alpha(amber, fade) : alpha(ivory, 0.7 * fade);
		float cx = r.X + r.Width / 2, cy = r.Y + (r.Height - px(91)) / 2 + px(20);  // disc, title and hint: 91 tall
		gp::Pen pen(c, std::max(1.f, px(1)));
		g.DrawEllipse(&pen, cx - px(20), cy - px(20), px(40), px(40));
		g.DrawEllipse(&pen, cx - px(5), cy - px(5), px(10), px(10));
		g.DrawArc(&pen, cx - px(14), cy - px(14), px(28), px(28), 200.f, 50.f);
		text(g, L"Use game CD", fonts_.cd.get(), c, gp::RectF(r.X, cy + px(25), r.Width, px(28)), gp::StringAlignmentCenter);
		text(g, model_.disc ? L"Disc detected \u00B7 click to set up" : L"Insert your game disc", fonts_.small.get(), alpha(muted, fade),
		     gp::RectF(r.X, cy + px(55), r.Width, px(16)), gp::StringAlignmentCenter);
		break;
	}
	case ID_RETRY:
	case ID_LOG: {
		gp::SolidBrush fill(alpha(ivory, pressed ? 0.16 : on ? 0.09 : 0.04));
		g.FillRectangle(&fill, r);
		gp::Pen frame(alpha(ivory, 0.2), std::max(1.f, px(1)));
		g.DrawRectangle(&frame, r.X, r.Y, r.Width - px(1), r.Height - px(1));
		text(g, id == ID_RETRY ? L"Try again" : L"Open setup log", fonts_.small.get(), on ? ivory : muted, r, gp::StringAlignmentCenter,
		     gp::StringAlignmentCenter);
		break;
	}
	case ID_LAUNCH: {
		gp::SolidBrush fill(on ? (pressed ? gp::Color(255, 230, 160, 30) : amber) : over_bay(ivory, 0.06));
		g.FillRectangle(&fill, r);
		gp::Pen frame(alpha(ivory, 0.14), std::max(1.f, px(1)));
		g.DrawRectangle(&frame, r.X, r.Y, r.Width - px(1), r.Height - px(1));
		gp::Color c = on ? gp::Color(255, 0, 0, 0) : alpha(muted, 0.65);
		std::wstring label = model_.busy ? L"Preparing your game\u2026" : L"Launch game";
		gp::RectF box;
		g.MeasureString(label.c_str(), INT(label.size()), fonts_.launch.get(), gp::PointF(0, 0), &box);
		float start = r.X + (r.Width - box.Width - px(24)) / 2, mid = r.Y + r.Height / 2;
		gp::SolidBrush play(c);
		gp::PointF triangle[] = {{start, mid - px(7)}, {start + px(12), mid}, {start, mid + px(7)}};
		g.FillPolygon(&play, triangle, 3);
		text(g, label, fonts_.launch.get(), c, gp::RectF(start + px(24), r.Y, box.Width + px(4), r.Height), gp::StringAlignmentNear,
		     gp::StringAlignmentCenter);
		break;
	}
	case ID_SETTINGS:
	case ID_HELP: {
		gp::Color c = pressed ? ivory : muted;
		icon(g, id == ID_SETTINGS ? L"\uE713" : L"\uE897", fonts_.icon.get(), c, gp::RectF(r.X, r.Y, px(18), r.Height));
		text(g, id == ID_SETTINGS ? L"Settings" : L"Help", fonts_.small.get(), c, gp::RectF(r.X + px(20), r.Y, r.Width - px(20), r.Height),
		     gp::StringAlignmentNear, gp::StringAlignmentCenter);
		break;
	}
	}
	if (focused) {
		gp::Pen focus(alpha(ivory, 0.8), std::max(1.f, px(1)));
		focus.SetDashStyle(gp::DashStyleDot);
		g.DrawRectangle(&focus, r.X + px(3), r.Y + px(3), r.Width - px(7), r.Height - px(7));
	}
}

// ---------------------------------------------------------------- actions

void Window::choose_iso() {
	IFileOpenDialog *dialog = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
	COMDLG_FILTERSPEC filters[] = {{L"Disc images (*.iso)", L"*.iso"}};
	dialog->SetFileTypes(1, filters);
	dialog->SetTitle(L"Choose your Battlegrounds disc image");
	dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
	std::optional<fs::path> chosen;
	if (SUCCEEDED(dialog->Show(hwnd_))) {
		IShellItem *item = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item))) {
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) chosen = fs::path(path);
			CoTaskMemFree(path);
			item->Release();
		}
	}
	dialog->Release();
	if (chosen) model_.request_install(*chosen);
}

void Window::use_cd() {
	if (!model_.busy && model_.disc) model_.request_install(*model_.disc);
}

void Window::watch_discs() {
	watching_ = true;
	rescan();
}

// Reading a slow optical drive must not freeze the window: look for the disc on a worker thread.
void Window::rescan() {
	if (model_.preview || model_.busy || model_.ready) return;
	std::thread([post = model_.post, model = &model_] {
		std::vector<fs::path> found;
		for (auto &volume : mounted_volumes())
			if (setup_on_disc(volume)) found.push_back(volume);
		post([model, found] { model->scan_discs(found); });
	}).detach();
}

int task_dialog(HWND owner, const wchar_t *instruction, const std::wstring &content, std::initializer_list<TASKDIALOG_BUTTON> buttons,
                int default_button, bool cancel) {
	TASKDIALOGCONFIG config{};
	config.cbSize = sizeof config;
	config.hwndParent = owner;
	config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
	config.dwCommonButtons = cancel ? TDCBF_CANCEL_BUTTON : 0;
	config.pszWindowTitle = L"One Must Fall: Battlegrounds";
	config.pszMainInstruction = instruction;
	config.pszContent = content.c_str();
	config.pButtons = buttons.begin();
	config.cButtons = UINT(buttons.size());
	config.nDefaultButton = default_button;
	int pressed = IDCANCEL;
	if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) return IDCANCEL;
	return pressed;
}

void Window::confirm_install() {
	if (!model_.pending_source) return;
	std::wstring name = model_.pending_source->filename().wstring();
	if (name.empty()) name = L"your game CD in " + model_.pending_source->wstring();
	std::wstring content = L"Setup will install " + name +
	                       L" using the bundled update and tools. It works offline and needs at least 3 GB of free space in " +
	                       model_.where.home.wstring() + L".";
	if (task_dialog(hwnd_, L"Set up Battlegrounds?", content, {{100, L"Start setup"}}, 100, true) == 100) model_.confirm_install();
	else model_.cancel_pending();
}

void Window::help() {
	task_dialog(hwnd_, L"Getting arena-ready",
	            L"Choose an ISO of your original English Battlegrounds 2.090 disc, or insert the CD and click Use game CD. "
	            L"An inserted game CD, or an ISO you have opened in File Explorer, is detected automatically. Confirm Start setup to "
	            L"begin. Setup uses the update and tools bundled with this app, then verifies the finished installation.\n\n"
	            L"When everything is ready, the game starts. Next time, this screen is skipped automatically. Your original disc "
	            L"and ISO are never changed.\n\n"
	            L"First-time setup works offline and needs 3 GB of free space. No administrator rights, installers or "
	            L"system-wide changes are required.",
	            {{IDOK, L"Done"}}, IDOK, false);
}

void Window::settings() {
	std::wstring content = model_.where.home.wstring() +
	                       L"\n\nYour extracted and updated game files stay here. Setup tools and the update are included in the app. "
	                       L"Nothing is added to your system, registry or PATH. A verified installation opens directly into the game.";
	if (task_dialog(hwnd_, L"Your game folder", content, {{200, L"Open folder"}, {IDOK, L"Done"}}, IDOK, false) == 200) {
		std::error_code ec;
		fs::create_directories(model_.where.home, ec);
		ShellExecuteW(hwnd_, L"open", model_.where.home.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
}

bool Window::confirm_close() {
	if (!model_.busy) return true;
	int choice = task_dialog(hwnd_, L"Stop setting up the game?",
	                         L"Your original files are safe. You can restart setup whenever you're ready.",
	                         {{300, L"Keep setting up"}, {301, L"Stop setup"}}, 300, false);
	if (choice != 301) return false;
	model_.cancel();
	return true;
}

void Window::save_screenshot() {
	RECT client;
	GetClientRect(hwnd_, &client);
	gp::Bitmap image(client.right, client.bottom, PixelFormat32bppARGB);
	{
		gp::Graphics g(&image);
		paint(g, true);
	}
	CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
	image.Save(screenshot_.c_str(), &png, nullptr);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
	// Never show "There is no disk in the drive" while looking at empty optical drives.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	INITCOMMONCONTROLSEX controls{};
	controls.dwSize = sizeof controls;
	controls.dwICC = ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&controls);
	gp::GdiplusStartupInput startup;
	ULONG_PTR token = 0;
	gp::GdiplusStartup(&token, &startup, nullptr);

	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	bool preview = false;
	std::wstring screenshot;
	for (int k = 1; k < argc; ++k) {
		std::wstring a = argv[k];
		if (a == L"--preview") preview = true;
		if (a == L"--screenshot" && k + 1 < argc) preview = true, screenshot = argv[++k];
	}
	LocalFree(argv);

	int code = 0;
	{
		LauncherModel model(Locations::detect(executable_dir()), preview);
		Window window(model);
		if (!window.create(instance)) return 1;
		if (!screenshot.empty()) window.screenshot_after_show(screenshot);
		HWND hwnd = window.hwnd();
		model.post = [hwnd](std::function<void()> f) {
			auto heap = new std::function<void()>(std::move(f));
			if (!PostMessageW(hwnd, WM_APP_RUN, 0, LPARAM(heap))) delete heap;
		};
		bool asking = false;
		model.changed = [&] {
			window.refresh();
			// Every install request needs explicit consent. The prompt is modal, so it opens after
			// the click or drop that asked for it has been handled, once per request.
			if (model.pending_source && !asking) {
				asking = true;
				model.post([&] {
					if (model.pending_source) window.confirm_install();
					asking = false;
				});
			}
		};
		model.needs_window = [&] { window.show(); };
		model.hide_window = [hwnd] { ShowWindow(hwnd, SW_HIDE); };
		model.quit = [hwnd] { DestroyWindow(hwnd); };
		model.start();
		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			if (!IsDialogMessageW(hwnd, &msg)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}
		code = int(msg.wParam);
		model.cancel();
	}
	gp::GdiplusShutdown(token);
	CoUninitialize();
	return code;
}
