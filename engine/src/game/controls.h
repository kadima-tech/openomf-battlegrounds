// The engine's 3D GUI controls (Core's DE_CControl3d family) as the launcher's interface draws
// them (DE_CGameLauncherInterface over DE_CGUIStandardInterface): a retained tree laid out by
// the controls' edges in the GUI viewport's pixels (y down), drawn as bevelled quads, text and
// images, with the top control's input routing (capture, hover, focus, modal dialogs, popups).
#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "de/scene.h"

namespace game::ui {

struct DrawItem;
struct Control;
class AppManager;
class Gui;

struct Rgba {
	float r = 1, g = 1, b = 1, a = 1;
};

// The launcher's colours (2004 exe 0x4010e0..0x4011c0) over the Windows Classic system colours.
namespace theme {
constexpr Rgba kDarkShadow{64 / 255.0f, 64 / 255.0f, 64 / 255.0f, 1};
constexpr Rgba kFace{0.5f, 0.5f, 0.5f, 0.5f};
constexpr Rgba kHilite{1, 1, 1, 1};
constexpr Rgba kCaption{0, 0, 1, 0.5f};
constexpr Rgba kText{1, 1, 1, 1};  // ColorButtonText, ColorCaptionText, ColorWindowText
constexpr Rgba kSelectedItem{10 / 255.0f, 36 / 255.0f, 106 / 255.0f, 1};
constexpr Rgba kWindowBack{0, 0, 0.1f, 0.7f};
constexpr Rgba kListSelection{0, 0, 0.5f, 1};  // CColEntry::Setup 0x700a6d30
constexpr float kGradient = 0.2f;              // GradientValue
}  // namespace theme

// Collects a frame's DrawItems in viewport pixels.
class Painter {
public:
	Painter(std::vector<DrawItem> &out, float w, float h) : out_(out), w_(w), h_(h) {}
	void rect(float x0, float y0, float x1, float y1, Rgba c);
	void quad(const float *xy, const Rgba *c);
	// DE_CBevelBackgroundProceduralMesh: four edges `bx`, `by` wide shading into the face.
	// `face` (top left, bottom left, bottom right, top right) replaces the face gradient; `edges`
	// colours the four edges, the face then transparent unless `face`.
	void bevel(float x0, float y0, float x1, float y1, float bx, float by, bool down, const Rgba *face = nullptr,
	           const Rgba *edges = nullptr);
	void bevel_filled(float x0, float y0, float x1, float y1, float bx, float by, bool down, Rgba fill, const Rgba *edges = nullptr) {
		const Rgba f[4] = {fill, fill, fill, fill};
		bevel(x0, y0, x1, y1, bx, by, down, f, edges);
	}
	void text(const std::string &t, float x, float top, float px, Rgba c, bool centred = false);
	void image(const de::ObjectRef &img, float x0, float y0, float x1, float y1, Rgba tint);
	void push_clip(float x0, float y0, float x1, float y1);
	void pop_clip();

private:
	void finish(DrawItem &d);
	std::vector<DrawItem> &out_;
	float w_, h_;
	std::vector<std::array<float, 4>> clips_;
};

// DE_CControl3d.
class Ctl {
public:
	explicit Ctl(Gui &g) : gui(g) {}
	virtual ~Ctl() = default;
	Ctl(const Ctl &) = delete;
	Ctl &operator=(const Ctl &) = delete;

	Gui &gui;
	Ctl *parent = nullptr;
	std::vector<std::unique_ptr<Ctl>> children;
	std::string name;

	template <class T = Ctl, class... A>
	T *add(A &&...a) {
		auto c = std::make_unique<T>(gui, std::forward<A>(a)...);
		c->parent = this;
		T *p = c.get();
		children.push_back(std::move(c));
		return p;
	}
	// Deleted after the current event.
	void destroy();

	// Edges: Scale* fractions of the parent's client area; OffsetFrom* and Len* in pixels (-1 unset).
	float sl = 0, st = 0, sr = 1, sb = 1;
	float from_left = -1, from_top = -1, from_right = -1, from_bottom = -1, len_x = -1, len_y = -1;
	int align = 0;      // +0x150: 2 centres a length vertically, 4 horizontally
	float border = 0;   // OffsetBorder: the children's inset
	Ctl *scale(float l, float t, float r, float b) {
		sl = l, st = t, sr = r, sb = b;
		return this;
	}

	// The visual (+0x1e8 bit 0): a bevel `frame` pixels wide, up or down, and its face.
	bool visual = false;
	float frame_x = 0, frame_y = 0;
	bool down = false;
	bool has_fill = false, has_edges = false;
	Rgba fill, edges;
	de::ObjectRef image;  // textures the face
	Rgba image_tint;
	bool hidden = false;
	bool disabled = false;  // ReadOnlyOn: covered and refusing input

	void set_fill(Rgba c) { visual = has_fill = true, fill = c; }
	void set_frame(float px, bool is_down) { visual = true, frame_x = frame_y = px, down = is_down; }

	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // laid out, viewport pixels
	float w() const { return x1 - x0; }
	float h() const { return y1 - y0; }
	bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
	bool shown() const;
	bool enabled() const;
	bool inside(const Ctl *ancestor) const;

	// Layout in the area given, then the children in the client area.
	void layout(float ax0, float ay0, float ax1, float ay1);
	void place(float px0, float py0, float px1, float py1);
	virtual void layout_children();
	void client(float &cx0, float &cy0, float &cx1, float &cy1) const;

	virtual void paint(Painter &p);
	void paint_visual(Painter &p);
	virtual void paint_self(Painter &) {}
	void paint_children(Painter &p);

	// Input; `hit` finds the deepest shown control that takes the mouse.
	virtual Ctl *hit(float x, float y);
	virtual bool takes_mouse() const { return false; }
	virtual void mouse_down(float, float, int) {}
	virtual void mouse_drag(float, float) {}
	virtual void mouse_up(float, float, bool) {}
	virtual void mouse_move(float, float) {}
	virtual void mouse_leave() {}
	virtual bool wheel(int) { return false; }
	virtual bool focusable() const { return false; }
	virtual void focus(bool) {}
	virtual bool key(int) { return false; }
	virtual void text(const std::string &) {}
	// The pre-draw, each frame before layout.
	virtual void pre_draw() {}
	void pre_draw_tree();
	// The deepest shown control under (x, y), taking the mouse or not.
	virtual Ctl *under(float x, float y);
};

// A control whose look and mouse are the owner's callbacks.
class Canvas : public Ctl {
public:
	using Ctl::Ctl;
	std::function<void(Painter &)> on_paint;
	std::function<void(float, float, int)> on_down;
	std::function<void(float, float, bool)> on_up;
	std::function<void(float, float)> on_move, on_drag;
	std::function<void()> on_leave;
	void paint_self(Painter &p) override {
		if (on_paint) on_paint(p);
	}
	bool takes_mouse() const override { return on_down || on_up || on_move; }
	void mouse_down(float x, float y, int c) override {
		if (on_down) on_down(x, y, c);
	}
	void mouse_up(float x, float y, bool over) override {
		if (on_up) on_up(x, y, over);
	}
	void mouse_move(float x, float y) override {
		if (on_move) on_move(x, y);
	}
	void mouse_drag(float x, float y) override {
		if (on_drag) on_drag(x, y);
	}
	void mouse_leave() override {
		if (on_leave) on_leave();
	}
};

// DE_CTextControl3d: one line of flat text `px` high, placed in its box by `align`.
class Text : public Ctl {
public:
	Text(Gui &g, std::string t, float px, Rgba c = theme::kText, int alignment = 2);
	std::string str;
	float px;
	Rgba color;
	void paint_self(Painter &p) override;
	float pixel_len() const;
	// gNewText (0x700b7870): the box hugs the text.
	void hug();
};

// A row (DE_CAutoSizeVerticalRobot) or a column of children with lengths, stacked.
class Stack : public Ctl {
public:
	using Ctl::Ctl;
	bool across = false;
	float spacing = 0;
	void layout_children() override;
};

// DE_CButtonControl3d (0x700bd420..; DE_CButtonRobot 0x700c4e00).
class Button : public Ctl {
public:
	enum Type { Plain, Push, Radio, Check };
	Button(Gui &g, std::string label, Type type = Plain, float label_px = -1);
	Type type;
	std::string label;
	float label_px;
	Rgba label_color = theme::kText;
	bool toggle = false;
	bool state = false;   // +0x1fc down
	int *bound = nullptr;  // DE_CControlRobotButtonBOOL
	bool mini = false;
	de::ObjectRef face_up, face_down;  // SetVisualFace
	Rgba face_tint;
	std::vector<int> keys;  // DE_CButtonClickOnKey
	std::function<void()> on_click;

	void click();
	void paint_self(Painter &p) override;
	bool takes_mouse() const override { return true; }
	void mouse_down(float, float, int) override { pressed_ = over_ = true; }
	void mouse_drag(float x, float y) override { over_ = contains(x, y); }
	void mouse_up(float, float, bool over) override;
	void pre_draw() override {
		if (bound) state = *bound != 0;
	}

private:
	bool pressed_ = false, over_ = false;
};

// DE_CScrollBarControl3d with the launcher's look (2004 exe 0x4012b0): no arrows, a dark track
// and a CI.VertTile / CI.HorTile thumb with its CI.*Static grip.
class ScrollBar : public Ctl {
public:
	ScrollBar(Gui &g, bool vertical);
	bool vertical;
	float pos = 0, size = 1;
	float step = 14;  // +0x210, pixels
	std::function<void(float)> on_change;
	void set_pos(float p);
	void paint_self(Painter &p) override;
	bool takes_mouse() const override { return true; }
	void mouse_down(float x, float y, int) override;
	void mouse_drag(float x, float y) override;
	void mouse_up(float, float, bool) override { dragging_ = false; }
	bool wheel(int dy) override;

private:
	void track(float &t0, float &t1) const;
	bool dragging_ = false;
	float grab_ = 0;
};

// DE_CPageViewerControl3d (0x700f9070..): a clipped client showing part of its page, scroll bars
// when the page is larger.
class PageViewer : public Ctl {
public:
	explicit PageViewer(Gui &g);
	Canvas *page;
	// The page's size for a client this wide (pixels); unset: the client.
	std::function<float(float width)> page_height;
	std::function<float(float width)> page_width;
	ScrollBar *vbar = nullptr, *hbar = nullptr;
	float off_x = 0, off_y = 0;
	float cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;  // the client
	float ph = 0, pw = 0;
	void layout_children() override;
	void paint(Painter &p) override;
	Ctl *hit(float x, float y) override;
	Ctl *under(float x, float y) override;
	bool wheel(int dy) override;
	void scroll_to(float top, float bottom);
	void scroll_top();
};

// DE_CTextDisplayControl3d (0x700ff490..): text wrapped into its page viewer (BuildWrappedText
// 0x700ff8a0).
class TextDisplay : public Ctl {
public:
	TextDisplay(Gui &g, std::string text, float px = -1, Rgba c = theme::kText);
	std::string str;
	float px;
	Rgba color;
	int line_align = 4;
	PageViewer *viewer;
	void set_text(const std::string &t);

private:
	void wrap(float width);
	std::vector<std::string> lines_;
	std::vector<float> widths_;
	float widest_ = 0, wrapped_w_ = -1;
	std::string wrapped_;
};

// DE_CListControl3dEx (0x7009e190..) and its entry manager: rows of cells in a page viewer.
class List : public Ctl {
public:
	explicit List(Gui &g);
	struct Column {
		std::string name;
		float width;  // a fraction of the list; -1 the rest
	};
	struct Row {
		std::vector<std::string> cells;
		bool selectable = true;
		Rgba color = theme::kText;
		int parent = -1;  // tree: the row it is listed under
	};
	std::vector<Column> columns;
	std::vector<Row> rows;
	// The entry manager's hierarchy (+0x88, the list's +0x234): rows under their parents,
	// collapsed at first, each level in row order.
	bool tree = false;
	std::string title;
	float entry_h = 20;  // SetMinEntrySize, pixels
	float text_px;       // SizeListControlEntryText
	int selected = -1, hover = -1;
	std::function<void(int)> on_select, on_double;
	PageViewer *viewer;

	void select(int i, bool notify = true);
	void set_rows(std::vector<Row> r);
	void layout_children() override;
	void paint_self(Painter &p) override;
	bool focusable() const override { return true; }
	bool key(int sym) override;
	bool wheel(int dy) override { return viewer->wheel(dy); }

private:
	float title_h() const { return title.empty() ? 0 : entry_h; }
	float header_h() const { return columns.size() > 1 ? entry_h : 0; }
	int row_at(float y) const;
	void rebuild();
	float indent(int row) const { return float(depth_[size_t(row)] + 1) * 20; }
	Canvas *body_;
	int down_row_ = -1;
	std::vector<int> shown_, depth_;  // the rows in display order; each row's depth
	std::vector<char> expanded_, has_kids_;
	std::vector<int> pos_;  // each row's display position, -1 hidden
};

// DE_CComboBoxControl3d (0x700dddb0..): the selected entry in a down frame, the ArrowDown button
// opening a list under it.
class Combo : public Ctl {
public:
	Combo(Gui &g, std::vector<std::string> entries, int *bound = nullptr);
	std::vector<std::string> entries;
	int selected = 0;
	int *bound = nullptr;  // DE_CINTComboBoxRobot
	std::function<void(int)> on_change;
	void set(int i, bool notify);
	void open();
	void pre_draw() override;

private:
	Ctl *inner_;
	Button *arrow_;
};

// DE_CSliderControl3d (0x700fc110..), horizontal: a down channel and an up grab mark.
class Slider : public Ctl {
public:
	explicit Slider(Gui &g, float *bound = nullptr);
	float value = 0;
	float *bound;  // DE_CFLOATSliderRobot
	std::function<void(float)> on_change;
	void paint_self(Painter &p) override;
	Ctl *hit(float x, float y) override;
	bool takes_mouse() const override { return true; }
	void mouse_down(float x, float, int) override;
	void mouse_drag(float x, float) override;
	void mouse_up(float, float, bool) override { dragging_ = false; }
	void pre_draw() override {
		if (bound && !dragging_) value = *bound;
	}

private:
	float grab_x() const;
	bool dragging_ = false;
};

// DE_CEditTextControl3d (0x700b1e40..): one line in a down panel, a yellow caret, the selection.
class Edit : public Ctl {
public:
	Edit(Gui &g, std::string text, int max_len = -1, bool numeric = false);
	std::string str;
	int max_len;
	bool numeric;
	std::function<void()> on_commit, on_enter;
	void paint_self(Painter &p) override;
	bool takes_mouse() const override { return true; }
	bool focusable() const override { return true; }
	void focus(bool on) override;
	void mouse_down(float x, float, int) override;
	void mouse_drag(float x, float) override;
	bool key(int sym) override;
	void text(const std::string &t) override;

private:
	size_t at(float x) const;
	void insert(const std::string &t);
	size_t caret_ = 0, anchor_ = 0;
	float scroll_ = 0;
	bool focused_ = false, keep_selection_ = false;
	std::chrono::steady_clock::time_point since_;
};

// DE_CDialog3d (0x700f0d80..): a modal frame with a title bar and its close box.
class Dialog : public Ctl {
public:
	Dialog(Gui &g, const std::string &title, float w, float h);
	Ctl *body;
	std::function<void()> on_close;  // the close box, Esc
	std::function<bool(int)> on_key;
	float moved_x = 0, moved_y = 0;  // dragged by the title bar, pixels
	void close();
	bool key(int sym) override;
};

class Gui {
public:
	explicit Gui(AppManager &a);
	~Gui();
	AppManager &app;
	float W = 1024, H = 768;
	// DE_CWindow::ConvertToVPU (0x700d4fa0): 800 x 600 virtual pixels to viewport pixels.
	float vx(float v) const;
	float vy(float v) const;

	Ctl &root() { return *root_; }
	// A plain control over a page marker's rectangle.
	Ctl *place(const Control &marker);
	float text_width(const std::string &t, float px) const;
	std::vector<float> offsets(const std::string &t, float px) const;
	// GetImage: Core's images, then the launcher's OMFBGGUI.
	de::ObjectRef image(const std::string &name) const;

	// DE_CDialog3d::Confirm (0x700f2230) with Yes and No; DE_CControl3d::UserModalGetString
	// (0x700e6b80).
	void confirm(const std::string &title, const std::string &text, std::function<void(bool)> done);
	void get_string(const std::string &title, const std::string &initial, std::function<void(bool, const std::string &)> done);
	Dialog *open_dialog(const std::string &title, float w, float h);
	void close(Ctl *dialog_or_popup);
	void open_popup(std::unique_ptr<Ctl> popup);
	bool modal() const { return !dialogs_.empty(); }

	void clear();
	void trash(std::unique_ptr<Ctl> c);
	void set_focus(Ctl *c);
	Ctl *focused() const { return focus_; }

	void layout();
	void paint(std::vector<DrawItem> &out);
	void paint_over(std::vector<DrawItem> &out);  // dialogs and popups
	// Input in screen fractions; true when the controls took it.
	bool mouse_move(float fx, float fy);
	bool mouse_button(bool down, int clicks);
	bool key(int sym);
	bool text(const std::string &t);
	bool wheel(int dy);

private:
	Ctl *hit(float x, float y);
	void forget(Ctl *c);
	std::unique_ptr<Ctl> root_;
	std::vector<std::unique_ptr<Ctl>> dialogs_;
	std::unique_ptr<Ctl> popup_;
	std::vector<std::unique_ptr<Ctl>> trash_;
	Ctl *focus_ = nullptr, *hover_ = nullptr, *pressed_ = nullptr;
	float mx_ = -1, my_ = -1;
};

}  // namespace game::ui
