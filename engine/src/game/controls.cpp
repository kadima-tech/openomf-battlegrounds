#include "game/controls.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "de/objects_file.h"
#include "game/runtime.h"
#include "game/ui.h"

namespace game::ui {

namespace {

constexpr int kKeyReturn = 13, kKeyEscape = 27, kKeyBackspace = 8, kKeyDelete = 127;
constexpr int kKeyRight = (1 << 30) | 79, kKeyLeft = (1 << 30) | 80, kKeyDown = (1 << 30) | 81, kKeyUp = (1 << 30) | 82;
constexpr int kKeyHome = (1 << 30) | 74, kKeyPageUp = (1 << 30) | 75, kKeyEnd = (1 << 30) | 77, kKeyPageDown = (1 << 30) | 78;

Rgba scaled(Rgba c, float k) { return {std::min(1.0f, c.r * k), std::min(1.0f, c.g * k), std::min(1.0f, c.b * k), c.a}; }

std::vector<std::string> split_lines(const std::string &t) {
	std::vector<std::string> out;
	std::string cur;
	for (size_t i = 0; i < t.size(); ++i) {
		if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == 'n') {
			out.push_back(cur), cur.clear(), ++i;
		} else if (t[i] == '\n') {
			out.push_back(cur), cur.clear();
		} else if (t[i] != '\r') {
			cur += t[i];
		}
	}
	out.push_back(cur);
	return out;
}

// The break both BuildWrappedText (0x700ff8a0) and UserInformation (0x700f32b0) use: back to the
// last space, '.' or ',' before `fit` letters, keeping it on the line; ten letters when there is none.
size_t break_at(const std::string &s, size_t fit) {
	for (size_t k = std::min(fit, s.size()); k-- > 0;) {
		if (s[k] == ' ' || s[k] == '.' || s[k] == ',') return k + 1;
	}
	return std::min<size_t>(10, s.size());
}

std::vector<std::string> wrap_paragraph(const Gui &gui, std::string para, float px, float width, float limit) {
	std::vector<std::string> out;
	while (true) {
		std::vector<float> o = gui.offsets(para, px);
		if (width <= 0 || o.back() <= width || para.size() <= 1) {
			out.push_back(para);
			return out;
		}
		size_t fit = 0;
		while (fit < para.size() && o[fit + 1] < limit) ++fit;
		size_t cut = std::max<size_t>(1, break_at(para, fit));
		out.push_back(para.substr(0, cut));
		para = para.substr(cut);
		if (para.empty()) return out;
	}
}

}  // namespace

// ---- Painter ----

void Painter::finish(DrawItem &d) {
	if (!clips_.empty()) {
		auto &c = clips_.back();
		d.clip[0] = c[0] / w_, d.clip[1] = c[1] / h_, d.clip[2] = c[2] / w_, d.clip[3] = c[3] / h_;
	}
	out_.push_back(std::move(d));
}

void Painter::push_clip(float x0, float y0, float x1, float y1) {
	if (!clips_.empty()) {
		auto &c = clips_.back();
		x0 = std::max(x0, c[0]), y0 = std::max(y0, c[1]), x1 = std::min(x1, c[2]), y1 = std::min(y1, c[3]);
	}
	clips_.push_back({x0, y0, std::max(x0 + 0.001f, x1), std::max(y0 + 0.001f, y1)});
}

void Painter::pop_clip() {
	if (!clips_.empty()) clips_.pop_back();
}

void Painter::rect(float x0, float y0, float x1, float y1, Rgba c) {
	if (c.a <= 0 || x1 <= x0 || y1 <= y0) return;
	DrawItem d;
	d.kind = DrawItem::Rect;
	d.x0 = x0 / w_, d.y0 = y0 / h_, d.x1 = x1 / w_, d.y1 = y1 / h_;
	d.rgba[0] = c.r, d.rgba[1] = c.g, d.rgba[2] = c.b, d.rgba[3] = c.a;
	finish(d);
}

void Painter::quad(const float *xy, const Rgba *c) {
	DrawItem d;
	d.kind = DrawItem::Quad;
	for (int k = 0; k < 4; ++k) {
		d.pts[k * 2] = xy[k * 2] / w_, d.pts[k * 2 + 1] = xy[k * 2 + 1] / h_;
		d.cols[k * 4] = c[k].r, d.cols[k * 4 + 1] = c[k].g, d.cols[k * 4 + 2] = c[k].b, d.cols[k * 4 + 3] = c[k].a;
	}
	finish(d);
}

// The five sections' vertices (CPrimitive::SetPosition 0x701c3c70) and their colours as
// CreateBevelPanel (0x700f74f0) builds them, swapped by SetDown (0x700c3820).
void Painter::bevel(float x0, float y0, float x1, float y1, float bx, float by, bool down, const Rgba *face, const Rgba *edges) {
	if (x1 <= x0 || y1 <= y0) return;
	bx = std::min(bx, (x1 - x0) / 2), by = std::min(by, (y1 - y0) / 2);
	const float xs[4] = {x0, x0 + bx, x1 - bx, x1}, ys[4] = {y0, y0 + by, y1 - by, y1};
	const Rgba H = theme::kHilite, D = theme::kDarkShadow;
	const Rgba Fp = scaled(theme::kFace, 1 + theme::kGradient), Fm = scaled(theme::kFace, 1 - theme::kGradient);
	// top, bottom, left, right, face; each v0..v3 as (x index, y index).
	static const int at[5][4][2] = {{{0, 0}, {1, 1}, {2, 1}, {3, 0}},
	                                {{1, 2}, {0, 3}, {3, 3}, {2, 2}},
	                                {{0, 0}, {0, 3}, {1, 2}, {1, 1}},
	                                {{2, 1}, {2, 2}, {3, 3}, {3, 0}},
	                                {{1, 1}, {1, 2}, {2, 2}, {2, 1}}};
	Rgba col[5][4] = {{H, H, Fm, Fp}, {Fp, Fm, D, D}, {H, Fp, Fp, H}, {Fm, D, D, Fm}, {Fp, Fm, Fm, Fp}};
	if (down) {
		Rgba up[5][4];
		std::copy(&col[0][0], &col[0][0] + 20, &up[0][0]);
		for (int k = 0; k < 4; ++k) col[0][k] = up[1][3 - k], col[1][k] = up[0][3 - k];
		col[2][0] = up[3][1], col[2][1] = up[3][0], col[2][2] = up[3][3], col[2][3] = up[3][2];
		col[3][0] = up[2][1], col[3][1] = up[2][0], col[3][2] = up[2][3], col[3][3] = up[2][2];
		col[4][0] = up[4][1], col[4][1] = up[4][0], col[4][2] = up[4][3], col[4][3] = up[4][2];
	}
	if (edges) {
		for (int s = 0; s < 4; ++s) std::fill(col[s], col[s] + 4, *edges);
	}
	bool face_shown = true;
	if (face) std::copy(face, face + 4, col[4]);
	else if (edges) face_shown = false;
	for (int s = 0; s < 5; ++s) {
		if (s < 4 && (bx <= 0 && by <= 0)) continue;
		if (s == 4 && !face_shown) continue;
		float xy[8];
		for (int k = 0; k < 4; ++k) xy[k * 2] = xs[at[s][k][0]], xy[k * 2 + 1] = ys[at[s][k][1]];
		quad(xy, col[s]);
	}
}

void Painter::text(const std::string &t, float x, float top, float px, Rgba c, bool centred) {
	if (t.empty() || c.a <= 0) return;
	DrawItem d;
	d.kind = DrawItem::Text;
	d.cell = true;
	d.text = t;
	d.x0 = x / w_, d.y0 = top / h_, d.size = px / h_, d.centered = centred;
	d.rgba[0] = c.r, d.rgba[1] = c.g, d.rgba[2] = c.b, d.rgba[3] = c.a;
	finish(d);
}

void Painter::image(const de::ObjectRef &img, float x0, float y0, float x1, float y1, Rgba tint) {
	if (!img || x1 <= x0 || y1 <= y0) return;
	DrawItem d;
	d.kind = DrawItem::Image;
	d.image = img;
	d.x0 = x0 / w_, d.y0 = y0 / h_, d.x1 = x1 / w_, d.y1 = y1 / h_;
	d.rgba[0] = tint.r, d.rgba[1] = tint.g, d.rgba[2] = tint.b, d.rgba[3] = tint.a;
	finish(d);
}

// ---- Ctl ----

void Ctl::destroy() {
	if (!parent) {
		gui.close(this);
		return;
	}
	auto &v = parent->children;
	for (auto it = v.begin(); it != v.end(); ++it) {
		if (it->get() != this) continue;
		std::unique_ptr<Ctl> me = std::move(*it);
		v.erase(it);
		gui.trash(std::move(me));
		return;
	}
}

bool Ctl::shown() const {
	for (const Ctl *c = this; c; c = c->parent) {
		if (c->hidden) return false;
	}
	return true;
}

bool Ctl::enabled() const {
	for (const Ctl *c = this; c; c = c->parent) {
		if (c->disabled) return false;
	}
	return true;
}

bool Ctl::inside(const Ctl *a) const {
	for (const Ctl *c = this; c; c = c->parent) {
		if (c == a) return true;
	}
	return false;
}

void Ctl::layout(float ax0, float ay0, float ax1, float ay1) {
	float pw = ax1 - ax0, ph = ay1 - ay0;
	float l = ax0 + sl * pw, r = ax0 + sr * pw, t = ay0 + st * ph, b = ay0 + sb * ph;
	if (from_left >= 0) l += from_left;
	if (from_right >= 0) r -= from_right;
	if (from_top >= 0) t += from_top;
	if (from_bottom >= 0) b -= from_bottom;
	if (len_x >= 0) {
		if (from_left < 0 && from_right >= 0) l = r - len_x;
		else if (from_left < 0 && (align & 4)) l = (l + r) / 2 - len_x / 2;
		r = l + len_x;
	}
	if (len_y >= 0) {
		if (from_top < 0 && from_bottom >= 0) t = b - len_y;
		else if (from_top < 0 && (align & 2)) t = (t + b) / 2 - len_y / 2;
		b = t + len_y;
	}
	place(l, t, r, b);
}

void Ctl::place(float px0, float py0, float px1, float py1) {
	x0 = px0, y0 = py0, x1 = std::max(px0, px1), y1 = std::max(py0, py1);
	layout_children();
}

void Ctl::client(float &cx0, float &cy0, float &cx1, float &cy1) const {
	float fx = visual ? frame_x : 0, fy = visual ? frame_y : 0;
	cx0 = x0 + fx + border, cy0 = y0 + fy + border, cx1 = std::max(cx0, x1 - fx - border), cy1 = std::max(cy0, y1 - fy - border);
}

void Ctl::layout_children() {
	float cx0, cy0, cx1, cy1;
	client(cx0, cy0, cx1, cy1);
	for (auto &c : children) c->layout(cx0, cy0, cx1, cy1);
}

void Ctl::pre_draw_tree() {
	pre_draw();
	for (size_t i = 0; i < children.size(); ++i) children[i]->pre_draw_tree();
}

void Ctl::paint_visual(Painter &p) {
	if (!visual) return;
	if (has_fill) p.bevel_filled(x0, y0, x1, y1, frame_x, frame_y, down, fill, has_edges ? &edges : nullptr);
	else p.bevel(x0, y0, x1, y1, frame_x, frame_y, down, nullptr, has_edges ? &edges : nullptr);
	if (image) p.image(image, x0 + frame_x, y0 + frame_y, x1 - frame_x, y1 - frame_y, image_tint);
}

void Ctl::paint_children(Painter &p) {
	for (auto &c : children) c->paint(p);
}

void Ctl::paint(Painter &p) {
	if (hidden) return;
	paint_visual(p);
	paint_self(p);
	paint_children(p);
	// ReadOnlyOn (0x700e5760): a child control over the client area, black at alpha 0.5.
	if (disabled) {
		float cx0, cy0, cx1, cy1;
		client(cx0, cy0, cx1, cy1);
		p.rect(cx0, cy0, cx1, cy1, {0, 0, 0, 0.5f});
	}
}

Ctl *Ctl::hit(float x, float y) {
	if (hidden || disabled || !contains(x, y)) return nullptr;
	for (auto it = children.rbegin(); it != children.rend(); ++it) {
		if (Ctl *h = (*it)->hit(x, y)) return h;
	}
	return takes_mouse() ? this : nullptr;
}

Ctl *Ctl::under(float x, float y) {
	if (hidden || !contains(x, y)) return nullptr;
	for (auto it = children.rbegin(); it != children.rend(); ++it) {
		if (Ctl *h = (*it)->under(x, y)) return h;
	}
	return this;
}

// ---- Text ----

Text::Text(Gui &g, std::string t, float h, Rgba c, int alignment) : Ctl(g), str(std::move(t)), px(h), color(c) { align = alignment; }

float Text::pixel_len() const { return gui.text_width(str, px); }

void Text::hug() {
	len_x = pixel_len();
	len_y = px;
}

// DE_CTextSizeRobot::ProcessControlEvent (0x700c4c20): clamped to the box's height; centred by
// the alignment bits, else left and at the bottom.
void Text::paint_self(Painter &p) {
	float hh = h() + 0.5f < px ? h() : px;
	float top = (align & 2) ? (y0 + y1) / 2 - hh / 2 : y1 - hh;
	if (align & 4) p.text(str, (x0 + x1) / 2, top, hh, color, true);
	else p.text(str, x0, top, hh, color);
}

// ---- Stack ----

void Stack::layout_children() {
	float cx0, cy0, cx1, cy1;
	client(cx0, cy0, cx1, cy1);
	float at = across ? cx0 : cy0;
	for (auto &c : children) {
		if (c->hidden) continue;
		float len = across ? std::max(0.0f, c->len_x) : std::max(0.0f, c->len_y);
		if (across) c->layout(at, cy0, at + len, cy1);
		else c->layout(cx0, at, cx1, at + len);
		at += len + spacing;
	}
}

// ---- Button ----

Button::Button(Gui &g, std::string l, Type t, float lp) : Ctl(g), type(t), label(std::move(l)), label_px(lp >= 0 ? lp : g.vy(16)) {}

// Click (0x700bdd60) and ProcessEvent's toggle (0x700bda00).
void Button::click() {
	if (type == Radio && state) return;
	if (toggle) {
		state = !state;
		if (bound) *bound = state ? 1 : 0;
	} else if (bound) {
		*bound = 1;
	}
	if (on_click) on_click();
}

void Button::mouse_up(float, float, bool over) {
	bool fire = pressed_ && over && enabled();
	pressed_ = over_ = false;
	if (fire) click();
}

void Button::paint_self(Painter &p) {
	bool pressing = pressed_ && over_;
	if (toggle && bound) state = *bound != 0;
	switch (type) {
	case Plain: {
		const de::ObjectRef &img = state && face_down ? face_down : face_up;
		if (img) p.image(img, x0, y0, x1, y1, face_tint);
		float hh = std::min(label_px, h());
		p.text(label, (x0 + x1) / 2, (y0 + y1) / 2 - hh / 2, hh, label_color, true);
		break;
	}
	case Push: {
		// DE_CButtonRobot::SetUp / SetDown (0x700c5700 / 0x700c55a0): the label's holder shifts by
		// half the border when pressed.
		float b = mini ? 1 : 2;
		bool dn = pressing || state;
		p.bevel(x0, y0, x1, y1, b, b, dn);
		float l = dn ? x0 + 1.5f * b : x0 + b, r = dn ? x1 - b : x1 - 1.5f * b;
		float t = dn ? y0 + b : y0 + 1.5f * b, bt = dn ? y1 - 1.5f * b : y1 - b;
		float hh = std::min(label_px, bt - t);
		p.text(label, (l + r) / 2, (t + bt) / 2 - hh / 2, hh, label_color, true);
		break;
	}
	case Radio:
	case Check: {
		// CreateCheckButtonControl / CreateRadioButtonControl (0x700f5ce0 / 0x700f5a90).
		float s = type == Check ? gui.vx(18) : 16;
		float bx0 = x0 + 2, by0 = (y0 + y1) / 2 - s / 2;
		p.bevel_filled(bx0, by0, bx0 + s, by0 + s, 1, 1, true, theme::kWindowBack);
		if (state || pressing) p.image(gui.image(type == Check ? "CheckMark" : "Circle"), bx0 + 1, by0 + 1, bx0 + s - 1, by0 + s - 1, theme::kText);
		float hh = std::min(label_px, h());
		p.text(label, x0 + 2 + gui.vx(4) + gui.vx(18), (y0 + y1) / 2 - hh / 2, hh, label_color);
		break;
	}
	}
}

// ---- ScrollBar ----

ScrollBar::ScrollBar(Gui &g, bool v) : Ctl(g), vertical(v) {}

void ScrollBar::track(float &t0, float &t1) const {
	float fw = gui.vx(1);
	t0 = (vertical ? y0 : x0) + fw, t1 = (vertical ? y1 : x1) - fw;
}

void ScrollBar::set_pos(float p) {
	p = std::clamp(p, 0.0f, 1.0f);
	if (std::fabs(p - pos) < 1e-6f) return;
	pos = p;
	if (on_change) on_change(pos);
}

void ScrollBar::paint_self(Painter &p) {
	float fw = gui.vx(1);
	const Rgba edge{0.2f, 0.2f, 0.2f, 0.8f};
	p.bevel_filled(x0, y0, x1, y1, fw, fw, false, {0.2f, 0.2f, 0.2f, 0.3f}, &edge);
	float t0, t1;
	track(t0, t1);
	float e0 = t0 + (1 - size) * pos * (t1 - t0), e1 = e0 + size * (t1 - t0);
	const Rgba tile{1, 1, 1, 0.8f}, white{1, 1, 1, 1};
	if (vertical) {
		float ex0 = x0 + fw, ex1 = x1 - fw;
		p.image(gui.image("CI.VertTile"), ex0 + 4, e0 + 2, ex1 - 2, e1 - 2, tile);
		float g = gui.vy(32), m = (e0 + e1) / 2;
		p.image(gui.image("CI.VertStatic"), ex0, m - g / 2, ex1, m + g / 2, white);
	} else {
		float ey0 = y0 + fw, ey1 = y1 - fw;
		p.image(gui.image("CI.HorTile"), e0 + 2, ey0 + 2, e1 - 2, ey1 - 2, tile);
		float g = gui.vx(32), m = (e0 + e1) / 2;
		p.image(gui.image("CI.HorStatic"), m - g / 2, ey0, m + g / 2, ey1, white);
	}
}

// DE_CScrollBarControlRobot: a click on the track pages by one thumb, the thumb drags.
void ScrollBar::mouse_down(float x, float y, int) {
	float t0, t1, c = vertical ? y : x;
	track(t0, t1);
	float e0 = t0 + (1 - size) * pos * (t1 - t0), e1 = e0 + size * (t1 - t0);
	if (c < e0) set_pos(pos - size);
	else if (c > e1) set_pos(pos + size);
	else dragging_ = true, grab_ = c - e0;
}

void ScrollBar::mouse_drag(float x, float y) {
	if (!dragging_ || size >= 1) return;
	float t0, t1, c = vertical ? y : x;
	track(t0, t1);
	set_pos((c - grab_ - t0) / ((1 - size) * (t1 - t0)));
}

// ProcessEvent (0x700fb3f0): a notch moves one step.
bool ScrollBar::wheel(int dy) {
	if (size >= 1) return true;
	float t0, t1;
	track(t0, t1);
	set_pos(pos - float(dy) * (step / std::max(1.0f, t1 - t0)) / (1 - size));
	return true;
}

// ---- PageViewer ----

PageViewer::PageViewer(Gui &g) : Ctl(g) {
	page = add<Canvas>();
	vbar = add<ScrollBar>(true);
	hbar = add<ScrollBar>(false);
}

// AdjustViewClient (0x700f9550): a bar only where the page is larger, the client inset by it.
void PageViewer::layout_children() {
	float fx0, fy0, fx1, fy1;
	client(fx0, fy0, fx1, fy1);
	float bw = gui.vx(24), bh = gui.vy(24);
	float cw = fx1 - fx0, ch = fy1 - fy0;
	bool v = false, hz = false;
	for (int pass = 0; pass < 2; ++pass) {
		float w = cw - (v ? bw : 0), h = ch - (hz ? bh : 0);
		ph = page_height ? page_height(w) : h;
		pw = page_width ? page_width(w) : w;
		v = ph > h + 0.5f;
		hz = pw > w + 0.5f;
	}
	cx0 = fx0, cy0 = fy0, cx1 = fx1 - (v ? bw : 0), cy1 = fy1 - (hz ? bh : 0);
	float w = cx1 - cx0, h = cy1 - cy0;
	vbar->hidden = !v, hbar->hidden = !hz;
	vbar->size = v ? std::clamp(h / ph, 0.0f, 1.0f) : 1;
	hbar->size = hz ? std::clamp(w / pw, 0.0f, 1.0f) : 1;
	if (!v) vbar->pos = 0;
	if (!hz) hbar->pos = 0;
	off_y = v ? (ph - h) * vbar->pos : 0;
	off_x = hz ? (pw - w) * hbar->pos : 0;
	page->place(cx0 - off_x, cy0 - off_y, cx0 - off_x + std::max(pw, w), cy0 - off_y + std::max(ph, h));
	vbar->place(cx1, fy0, fx1, cy1);
	hbar->place(fx0, cy1, cx1, fy1);
}

void PageViewer::paint(Painter &p) {
	if (hidden) return;
	paint_visual(p);
	p.push_clip(cx0, cy0, cx1, cy1);
	page->paint(p);
	p.pop_clip();
	vbar->paint(p);
	hbar->paint(p);
}

Ctl *PageViewer::hit(float x, float y) {
	if (hidden || disabled || !contains(x, y)) return nullptr;
	if (Ctl *h = vbar->hit(x, y)) return h;
	if (Ctl *h = hbar->hit(x, y)) return h;
	if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) return page->hit(x, y);
	return nullptr;
}

Ctl *PageViewer::under(float x, float y) {
	if (hidden || !contains(x, y)) return nullptr;
	if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) {
		if (Ctl *c = page->under(x, y)) return c;
	}
	return this;
}

// ProcessEvent (0x700f9730): the wheel goes to the vertical bar.
bool PageViewer::wheel(int dy) {
	if (vbar->hidden) return false;
	return vbar->wheel(dy);
}

void PageViewer::scroll_to(float top, float bottom) {
	float h = cy1 - cy0;
	if (vbar->hidden || ph <= h || h <= 0) return;
	float off = off_y;
	if (top < off) off = top;
	if (bottom > off + h) off = bottom - h;
	vbar->pos = std::clamp(off / (ph - h), 0.0f, 1.0f);
	off_y = (ph - h) * vbar->pos;
}

void PageViewer::scroll_top() { vbar->pos = hbar->pos = 0; }

// ---- TextDisplay ----

TextDisplay::TextDisplay(Gui &g, std::string text, float h, Rgba c) : Ctl(g), str(std::move(text)), px(h >= 0 ? h : g.vy(14)), color(c) {
	viewer = add<PageViewer>();
	viewer->page_height = [this](float) {
		wrap(viewer->w());
		float f = px, pitch = f + gui.vy(1) + std::floor((f + 2) / 3);
		return float(lines_.size()) * pitch + std::floor((f + 2) / 3);
	};
	viewer->page->on_paint = [this](Painter &p) {
		float f = px, pitch = f + gui.vy(1) + std::floor((f + 2) / 3);
		float cw = viewer->cx1 - viewer->cx0, left = viewer->page->x0 + (cw - widest_) / 2;
		for (size_t i = 0; i < lines_.size(); ++i) {
			float top = viewer->page->y0 + float(i) * pitch;
			if (line_align & 4) p.text(lines_[i], left + widest_ / 2, top, f, color, true);
			else p.text(lines_[i], left, top, f, color);
		}
	};
}

void TextDisplay::set_text(const std::string &t) {
	if (t == str) return;
	str = t;
	wrapped_w_ = -1;
	viewer->scroll_top();
}

// BuildWrappedText (0x700ff8a0): the width less the scroll bar's and twice the font; a line is
// broken while its letters start before that less VPU 10.
void TextDisplay::wrap(float width) {
	if (width == wrapped_w_ && wrapped_ == str) return;
	wrapped_w_ = width, wrapped_ = str;
	float W = width - gui.vy(24) - 2 * px;
	lines_.clear();
	widths_.clear();
	widest_ = 0;
	for (const std::string &para : split_lines(str)) {
		for (auto &l : wrap_paragraph(gui, para, px, W, W - gui.vx(10))) {
			lines_.push_back(l);
			widths_.push_back(gui.text_width(l, px));
			widest_ = std::max(widest_, widths_.back());
		}
	}
}

// ---- List ----

List::List(Gui &g) : Ctl(g), text_px(g.vy(16)) {
	columns = {{"", -1}};
	viewer = add<PageViewer>();
	body_ = viewer->page;
	viewer->page_height = [this](float) { return float(shown_.size()) * entry_h; };
	viewer->page_width = [this](float w) {
		if (columns.size() > 1) return w;
		float widest = 0;
		for (int i : shown_) {
			const Row &r = rows[size_t(i)];
			if (r.cells.empty()) continue;
			// CEntry::CalcMinSize (0x700a6260): (depth + 1) x 20 + 5 + the text in a tree.
			float tw = gui.text_width(r.cells[0], text_px);
			widest = std::max(widest, tree ? indent(i) + 5 + tw : tw + gui.vx(6));
		}
		return std::max(w, widest);
	};
	body_->on_paint = [this](Painter &p) {
		float pw = body_->w();
		float top_vis = viewer->cy0, bottom_vis = viewer->cy1;
		for (size_t n = 0; n < shown_.size(); ++n) {
			int i = shown_[n];
			float y = body_->y0 + float(n) * entry_h;
			if (y + entry_h < top_vis || y > bottom_vis) continue;
			const Row &r = rows[size_t(i)];
			if (tree) {
				// CEntry::SetupHierarchy / PositionEntry (0x700a72d0 / 0x700a6320): a 10-pixel
				// "Minus" / "Plus" image (filled white) at depth x 20 + 5; the cell and its highlight
				// hug the text from (depth + 1) x 20 (CalcMinSize).
				const std::string &t = r.cells.empty() ? std::string() : r.cells[0];
				float tx = body_->x0 + indent(i), tw = gui.text_width(t, text_px) + 5;
				if (i == selected) p.rect(tx, y, tx + tw, y + entry_h, theme::kListSelection);
				else if (i == hover && r.selectable) p.rect(tx, y, tx + tw, y + entry_h, {0, 0, 0.5f, 0.5f});
				if (has_kids_[size_t(i)]) {
					float bx = tx - 15, by = y + (entry_h - 10) / 2;
					p.image(gui.image(expanded_[size_t(i)] ? "Minus" : "Plus"), bx, by, bx + 10, by + 10, {1, 1, 1, 1});
				}
				Rgba c = r.color;
				if (!r.selectable) c.a *= 0.5f;
				p.text(t, tx, y + (entry_h - text_px) / 2, text_px, c);
				continue;
			}
			// CColEntry::ShowAsSelected (0x700a7510) and the hover (control 0x25): the highlight's alpha.
			if (i == selected) p.rect(body_->x0, y, body_->x1, y + entry_h, theme::kListSelection);
			else if (i == hover && r.selectable) p.rect(body_->x0, y, body_->x1, y + entry_h, {0, 0, 0.5f, 0.5f});
			float x = body_->x0;
			for (size_t k = 0; k < columns.size() && k < r.cells.size(); ++k) {
				float cw = columns[k].width < 0 ? body_->x1 - x : columns[k].width * pw;
				Rgba c = r.color;
				if (!r.selectable) c.a *= 0.5f;
				p.push_clip(x, y, x + cw, y + entry_h);
				p.text(r.cells[k], x + gui.vx(3), y + (entry_h - text_px) / 2, text_px, c);
				p.pop_clip();
				x += cw;
			}
		}
	};
	// In a tree only the box and the text's highlight take clicks (CEntry::ProcessEvent 0x700a6590).
	auto part = [this](float x, int r) {
		if (!tree || r < 0) return 1;
		float tx = body_->x0 + indent(r);
		if (has_kids_[size_t(r)] && x >= tx - 15 && x < tx - 5) return 2;
		const std::string &t = rows[size_t(r)].cells.empty() ? std::string() : rows[size_t(r)].cells[0];
		return x >= tx && x < tx + gui.text_width(t, text_px) + 5 ? 1 : 0;
	};
	body_->on_down = [this, part](float x, float y, int clicks) {
		int r = row_at(y);
		down_row_ = -1;
		gui.set_focus(this);
		if (part(x, r) == 2) {
			expanded_[size_t(r)] = !expanded_[size_t(r)];
			rebuild();
			return;
		}
		if (part(x, r) == 0) return;
		down_row_ = r;
		// DE_CCloseListOnDoubleClickRobot (0x70098dc0).
		if (clicks >= 2 && down_row_ >= 0 && down_row_ == selected && on_double) on_double(selected);
	};
	body_->on_up = [this, part](float x, float y, bool over) {
		int r = row_at(y);
		if (over && r >= 0 && r == down_row_ && part(x, r) == 1 && rows[size_t(r)].selectable && r != selected) select(r);
		down_row_ = -1;
	};
	body_->on_move = [this, part](float x, float y) {
		int r = row_at(y);
		hover = part(x, r) == 1 ? r : -1;
	};
	body_->on_leave = [this] { hover = -1; };
}

// The page viewer's title (+0x200) and the column headers sit over the rows.
void List::layout_children() {
	float cx0, cy0, cx1, cy1;
	client(cx0, cy0, cx1, cy1);
	viewer->place(cx0, cy0 + title_h() + header_h(), cx1, cy1);
}

void List::paint_self(Painter &p) {
	float cx0, cy0, cx1, cy1;
	client(cx0, cy0, cx1, cy1);
	if (!title.empty()) p.text(title, (cx0 + cx1) / 2, cy0 + (entry_h - text_px) / 2, text_px, theme::kText, true);
	if (columns.size() < 2) return;
	float y = cy0 + title_h(), x = cx0, pw = viewer->cx1 - viewer->cx0;
	for (auto &c : columns) {
		float cw = c.width < 0 ? cx1 - x : c.width * pw;
		p.push_clip(x, y, x + cw, y + entry_h);
		p.text(c.name, x + gui.vx(3), y + (entry_h - text_px) / 2, text_px, {0.75f, 0.8f, 0.85f, 1});
		p.pop_clip();
		x += cw;
	}
}

int List::row_at(float y) const {
	if (y < viewer->cy0 || y >= viewer->cy1) return -1;
	int n = int(std::floor((y - body_->y0) / entry_h));
	return n >= 0 && n < int(shown_.size()) ? shown_[size_t(n)] : -1;
}

// PositionAllEntries (0x700a5c40): roots, then each expanded row's children under it.
void List::rebuild() {
	size_t n = rows.size();
	shown_.clear();
	depth_.assign(n, 0);
	pos_.assign(n, -1);
	has_kids_.assign(n, 0);
	expanded_.resize(n, 0);
	auto parent = [&](size_t i) { return tree && rows[i].parent >= 0 && size_t(rows[i].parent) < n ? rows[i].parent : -1; };
	std::vector<std::vector<int>> kids(n);
	std::vector<int> roots;
	for (size_t i = 0; i < n; ++i) {
		int p = parent(i);
		if (p >= 0) kids[size_t(p)].push_back(int(i)), has_kids_[size_t(p)] = 1;
		else roots.push_back(int(i));
	}
	std::function<void(int, int)> add = [&](int i, int d) {
		depth_[size_t(i)] = d;
		pos_[size_t(i)] = int(shown_.size());
		shown_.push_back(i);
		if (expanded_[size_t(i)]) {
			for (int k : kids[size_t(i)]) add(k, d + 1);
		}
	};
	for (int r : roots) add(r, 0);
}

void List::set_rows(std::vector<Row> r) {
	rows = std::move(r);
	expanded_.assign(rows.size(), 0);
	rebuild();
	selected = -1, hover = -1;
	viewer->scroll_top();
}

// SetAsSelected / ScrollToObject (0x700a8920 / 0x700aaab0), opening its parents (AssureExpanded).
void List::select(int i, bool notify) {
	if (rows.empty()) return;
	i = std::clamp(i, 0, int(rows.size()) - 1);
	bool opened = false;
	for (int p = tree ? rows[size_t(i)].parent : -1; p >= 0 && size_t(p) < rows.size(); p = rows[size_t(p)].parent) {
		if (!expanded_[size_t(p)]) expanded_[size_t(p)] = 1, opened = true;
	}
	if (opened) rebuild();
	selected = i;
	float at = float(std::max(0, pos_[size_t(i)]));
	viewer->scroll_to(at * entry_h, (at + 1) * entry_h);
	if (notify && on_select) on_select(i);
}

// ProcessEvent (0x7009e7a0).
bool List::key(int sym) {
	if (shown_.empty()) return false;
	int count = int(shown_.size());
	int page = std::max(1, int(std::lround((viewer->cy1 - viewer->cy0) / entry_h)));
	int at = selected >= 0 ? pos_[size_t(selected)] : -1, s = at;
	if (sym == kKeyUp) s = std::max(0, at - 1);
	else if (sym == kKeyDown) s = at + 1;
	else if (sym == kKeyHome) s = 0;
	else if (sym == kKeyEnd) s = count - 1;
	else if (sym == kKeyPageUp) s = std::max(0, at - page);
	else if (sym == kKeyPageDown) s = at + page;
	else if (sym < 128 && std::isalnum(sym)) {
		// SelectNextWithChar: the next row starting with the letter.
		for (int k = 1; k <= count; ++k) {
			int r = shown_[size_t((std::max(0, at) + k) % count)];
			const std::string &c = rows[size_t(r)].cells.empty() ? std::string() : rows[size_t(r)].cells[0];
			if (!c.empty() && std::tolower(static_cast<unsigned char>(c[0])) == std::tolower(sym)) {
				s = pos_[size_t(r)];
				break;
			}
		}
	} else {
		return false;
	}
	s = std::clamp(s, 0, count - 1);
	if (s != at) select(shown_[size_t(s)]);
	return true;
}

// ---- Combo ----

Combo::Combo(Gui &g, std::vector<std::string> e, int *b) : Ctl(g), entries(std::move(e)), bound(b) {
	len_y = g.vy(25);
	align |= 2;
	inner_ = add<Ctl>();
	inner_->set_frame(2, true);
	inner_->set_fill(theme::kWindowBack);
	auto *text = inner_->add<Canvas>();
	text->from_right = g.vx(24);
	text->on_paint = [this, text](Painter &p) {
		if (selected < 0 || size_t(selected) >= entries.size()) return;
		float px = std::min(gui.vy(16), text->h());
		p.push_clip(text->x0, text->y0, text->x1, text->y1);
		p.text(entries[size_t(selected)], text->x0 + gui.vx(3), (text->y0 + text->y1) / 2 - px / 2, px, theme::kText);
		p.pop_clip();
	};
	arrow_ = inner_->add<Button>("");
	arrow_->from_right = 1;
	arrow_->len_x = g.vx(16);
	arrow_->face_up = g.image("ArrowDown");
	arrow_->face_tint = {0, 0, 0, 1};
	arrow_->on_click = [this] { open(); };
	// A press anywhere on the field drops the list, as on the arrow.
	text->on_down = [this](float, float, int) { open(); };
	if (bound) selected = *bound;
}

void Combo::pre_draw() {
	if (bound) selected = *bound;
}

void Combo::set(int i, bool notify) {
	selected = entries.empty() ? -1 : std::clamp(i, 0, int(entries.size()) - 1);
	if (bound) *bound = selected;
	if (notify && on_change) on_change(selected);
}

// ProcessEvent (0x700de510): the list in a framed popup under the combo (above when it would
// leave the screen), VPU 130 tall; choosing closes it.
void Combo::open() {
	auto pop = std::make_unique<Ctl>(gui);
	pop->set_frame(2, false);
	pop->set_fill(theme::kWindowBack);
	float h = gui.vy(130);
	float top = y1 + h > gui.H ? y0 - h : y1;
	pop->from_left = x0, pop->from_top = top, pop->len_x = w(), pop->len_y = h;
	auto *list = pop->add<List>();
	std::vector<List::Row> rows;
	for (auto &e : entries) rows.push_back({{e}});
	list->set_rows(rows);
	Ctl *popup = pop.get();
	list->on_select = [this, popup](int i) {
		set(i, true);
		gui.close(popup);
	};
	gui.open_popup(std::move(pop));
	gui.layout();
	if (selected >= 0) list->select(selected, false);
	gui.set_focus(list);
}

// ---- Slider ----

Slider::Slider(Gui &g, float *b) : Ctl(g), bound(b) {
	if (bound) value = *bound;
}

float Slider::grab_x() const {
	const float gw = 8;
	float lo = x0 + gw, hi = x1 - gw;
	return lo + std::clamp(value, 0.0f, 1.0f) * (hi - lo);
}

// The look ProcessEvent (0x700fcb50) sets up: the channel GrabOffset / 2 in from each end, 10
// high and down; the grab mark 8 wide and up, ColorFace at alpha 0.7.
void Slider::paint_self(Painter &p) {
	float go = gui.vx(10), ch = 10, gw = 8, m = (y0 + y1) / 2;
	p.bevel_filled(x0 + go / 2, m - ch / 2, x1 - go / 2, m + ch / 2, 2, 2, true, theme::kFace);
	float g = grab_x();
	Rgba face = theme::kFace;
	face.a = 0.7f;
	p.bevel_filled(g - gw / 2, y0, g + gw / 2, y1, 1, 1, false, face);
}

// The DE_CMouseSlideRobot's hit control follows the grab mark, 2 GrabOffset wide.
Ctl *Slider::hit(float x, float y) {
	if (hidden || disabled || !contains(x, y)) return nullptr;
	float go = gui.vx(10), g = grab_x();
	return x >= g - go && x < g + go ? this : nullptr;
}

void Slider::mouse_down(float x, float, int) {
	dragging_ = true;
	mouse_drag(x, 0);
}

// ProcessMouseMove (0x700fd790).
void Slider::mouse_drag(float x, float) {
	if (!dragging_) return;
	const float gw = 8;
	float v = std::clamp((x - (x0 + gw)) / std::max(1.0f, (x1 - gw) - (x0 + gw)), 0.0f, 1.0f);
	if (v == value) return;
	value = v;
	if (bound) *bound = v;
	if (on_change) on_change(v);
}

// ---- Edit ----

Edit::Edit(Gui &g, std::string text, int max, bool num) : Ctl(g), str(std::move(text)), max_len(max), numeric(num) {
	set_frame(1, true);
	set_fill(theme::kWindowBack);
	if (max_len >= 0 && int(str.size()) > max_len) str.resize(size_t(max_len));
	caret_ = anchor_ = str.size();
}

// Focus (control 0xa / 0xb): the caret blinks at the system's CursorBlinkRate and everything is
// selected; losing it writes the text back.
void Edit::focus(bool on) {
	if (on == focused_) return;
	focused_ = on;
	if (on) {
		since_ = std::chrono::steady_clock::now();
		anchor_ = 0, caret_ = str.size();
		keep_selection_ = true;
	} else {
		anchor_ = caret_;
		if (on_commit) on_commit();
	}
}

size_t Edit::at(float x) const {
	std::vector<float> o = gui.offsets(str, gui.vy(16));
	float rel = x - (x0 + 2 - scroll_);
	size_t best = 0;
	for (size_t i = 0; i < o.size(); ++i) {
		if (std::fabs(o[i] - rel) < std::fabs(o[best] - rel)) best = i;
	}
	return best;
}

// The click that focuses the field keeps it all selected.
void Edit::mouse_down(float x, float, int) {
	if (keep_selection_) {
		keep_selection_ = false;
		return;
	}
	caret_ = anchor_ = at(x);
	since_ = std::chrono::steady_clock::now();
}

void Edit::mouse_drag(float x, float) {
	if (anchor_ != 0 || caret_ != str.size()) caret_ = at(x);
}

void Edit::insert(const std::string &t) {
	size_t a = std::min(anchor_, caret_), b = std::max(anchor_, caret_);
	std::string s = str.substr(0, a) + t + str.substr(b);
	if (max_len >= 0 && int(s.size()) > max_len) return;
	str = s;
	caret_ = anchor_ = a + t.size();
	since_ = std::chrono::steady_clock::now();
}

// DE_CEditTextFilter (0x700b5420).
bool Edit::key(int sym) {
	keep_selection_ = false;
	bool shift = gui.app.shift_held;
	auto move = [&](size_t to) {
		caret_ = to;
		if (!shift) anchor_ = caret_;
		since_ = std::chrono::steady_clock::now();
	};
	if (sym == kKeyLeft) move(caret_ > 0 ? caret_ - 1 : 0);
	else if (sym == kKeyRight) move(std::min(str.size(), caret_ + 1));
	else if (sym == kKeyHome) move(0);
	else if (sym == kKeyEnd) move(str.size());
	else if (sym == kKeyBackspace) {
		if (anchor_ == caret_ && caret_ > 0) anchor_ = caret_ - 1;
		insert("");
	} else if (sym == kKeyDelete) {
		if (anchor_ == caret_ && caret_ < str.size()) anchor_ = caret_ + 1;
		insert("");
	} else if (sym == kKeyReturn) {
		if (!on_enter) return false;
		on_enter();
	} else if (sym >= 32 && sym < 127) {
		// Printable keys arrive as text.
	} else {
		return false;
	}
	return true;
}

// DE_CEditTextFilterAlpha / Numeric (Filter 0x700b6140 / 0x700b65a0).
void Edit::text(const std::string &t) {
	keep_selection_ = false;
	std::string ok;
	for (char c : t) {
		if (static_cast<unsigned char>(c) < 32 || c == 127) continue;
		if (numeric && !std::isdigit(static_cast<unsigned char>(c)) && c != '-') continue;
		ok += c;
	}
	if (!ok.empty()) insert(ok);
}

void Edit::paint_self(Painter &p) {
	float px = gui.vy(16), tx0 = x0 + 2, tx1 = x1 - 2, top = (y0 + y1) / 2 - px / 2;
	std::vector<float> o = gui.offsets(str, px);
	float cx = o[std::min(caret_, o.size() - 1)];
	if (cx - scroll_ > tx1 - tx0) scroll_ = cx - (tx1 - tx0);
	if (cx < scroll_) scroll_ = cx;
	p.push_clip(x0 + 1, y0 + 1, x1 - 1, y1 - 1);
	if (focused_ && anchor_ != caret_) {
		float a = o[std::min(anchor_, caret_)], b = o[std::max(anchor_, caret_)];
		p.rect(tx0 - scroll_ + a, top, tx0 - scroll_ + b, top + px, theme::kSelectedItem);
	}
	p.text(str, tx0 - scroll_, top, px, theme::kText);
	if (focused_) {
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since_).count();
		if ((ms / 550) % 2 == 0) {
			float cw = 0.05f * w();
			p.rect(tx0 - scroll_ + cx, top, tx0 - scroll_ + cx + cw, top + px, {1, 1, 0, 0.8f});
		}
	}
	p.pop_clip();
}

// ---- Dialog ----

namespace {

// DE_CFocusTitleBarRobot (0x700c4a90): the active caption shading from c x 0.8 on the left to
// c x 1.2 on the right. The bar's client drags the dialog by the mouse's movement (control actor
// +0x188 relays 0x21/0x22 to it, DE_CControlActor3d::ProcessEvent 0x700ea330; the dialog's frame
// control moves by the delta, DE_CTitleBarRobot::ProcessControlEvent 0x700db1b0).
class TitleBar : public Ctl {
public:
	TitleBar(Gui &g, std::string t) : Ctl(g), title(std::move(t)) {}
	std::string title;
	bool takes_mouse() const override { return true; }
	void mouse_down(float x, float y, int) override { last_x_ = x, last_y_ = y; }
	void mouse_drag(float x, float y) override {
		if (auto *d = dynamic_cast<Dialog *>(parent)) d->moved_x += x - last_x_, d->moved_y += y - last_y_;
		last_x_ = x, last_y_ = y;
	}
	void paint_self(Painter &p) override {
		Rgba dark = scaled(theme::kCaption, 0.8f), light = scaled(theme::kCaption, 1.2f);
		const Rgba face[4] = {dark, dark, light, light};
		p.bevel(x0, y0, x1, y1, 2, 2, false, face);
		float px = std::floor(h() * 0.5f);
		p.text(title, x0 + 2 + 5, (y0 + y1) / 2 - px / 2, px, theme::kText);
	}

private:
	float last_x_ = 0, last_y_ = 0;
};

class CloseBox : public Button {
public:
	explicit CloseBox(Gui &g) : Button(g, "") {}
	void paint_self(Painter &p) override {
		p.bevel(x0, y0, x1, y1, 2, 2, false);
		float px = h() * 0.9f;
		p.text("X", (x0 + x1) / 2, (y0 + y1) / 2 - px / 2, px, {0, 0, 0, 1}, true);
	}
};

}  // namespace

Dialog::Dialog(Gui &g, const std::string &title, float w, float h) : Ctl(g) {
	len_x = w, len_y = h;
	align = 6;
	set_frame(g.vx(4), false);
	frame_y = g.vy(4);
	float th = title.empty() ? 0 : g.vy(30);
	body = add<Ctl>();
	body->from_top = th;
	if (title.empty()) return;
	auto *bar = add<TitleBar>(title);
	bar->len_y = th;
	auto *x = bar->add<CloseBox>();
	x->from_right = 4, x->from_top = 4, x->from_bottom = 4, x->len_x = th - 8;
	x->on_click = [this] {
		if (on_close) on_close();
		else close();
	};
}

void Dialog::close() { gui.close(this); }

bool Dialog::key(int sym) {
	if (on_key && on_key(sym)) return true;
	std::function<Button *(Ctl *)> find = [&](Ctl *c) -> Button * {
		if (auto *b = dynamic_cast<Button *>(c); b && b->shown() && b->enabled() &&
		                                         std::find(b->keys.begin(), b->keys.end(), sym) != b->keys.end())
			return b;
		for (auto &ch : c->children) {
			if (Button *b = find(ch.get())) return b;
		}
		return nullptr;
	};
	if (Button *b = find(this)) b->click();
	return true;
}

// ---- Gui ----

Gui::Gui(AppManager &a) : app(a) {
	W = float(a.view_w), H = float(a.view_h);
	root_ = std::make_unique<Ctl>(*this);
}

Gui::~Gui() = default;

float Gui::vx(float v) const { return std::trunc(v * W / 800.0f); }
float Gui::vy(float v) const { return std::trunc(v * H / 600.0f); }

Ctl *Gui::place(const Control &m) {
	Ctl *c = root_->add<Ctl>();
	c->scale(m.left, m.top, m.right, m.bottom);
	return c;
}

std::vector<float> Gui::offsets(const std::string &t, float px) const {
	if (!app.char_offsets) return std::vector<float>(t.size() + 1, 0.0f);
	std::vector<float> o = app.char_offsets(t, px / H);
	for (float &v : o) v *= W;
	return o;
}

float Gui::text_width(const std::string &t, float px) const { return offsets(t, px).back(); }

de::ObjectRef Gui::image(const std::string &name) const {
	static std::map<std::string, de::ObjectRef> cache;
	if (auto it = cache.find(name); it != cache.end()) return it->second;
	de::ObjectRef r;
	for (const char *stem : {"COREImages", "OMFBGGUI"}) {
		const de::ObjectsFile *f = app.world.data().file(stem);
		if (!f || r) continue;
		for (uint32_t i = 0; i < f->objects().size(); ++i) {
			if (f->objects()[i].name == name && f->objects()[i].class_path.rfind("Image", 0) == 0) {
				r = {f, i};
				break;
			}
		}
	}
	return cache[name] = r;
}

void Gui::trash(std::unique_ptr<Ctl> c) {
	forget(c.get());
	trash_.push_back(std::move(c));
}

void Gui::forget(Ctl *c) {
	if (focus_ && focus_->inside(c)) focus_ = nullptr;
	if (hover_ && hover_->inside(c)) hover_ = nullptr;
	if (pressed_ && pressed_->inside(c)) pressed_ = nullptr;
}

void Gui::clear() {
	set_focus(nullptr);
	root_ = std::make_unique<Ctl>(*this);
	dialogs_.clear();
	popup_.reset();
	focus_ = hover_ = pressed_ = nullptr;
}

void Gui::set_focus(Ctl *c) {
	if (c == focus_) return;
	// DE_CTopControl3d::ProcessFocus (0x70102340): kill focus, then set focus.
	Ctl *old = focus_;
	focus_ = c;
	if (old) old->focus(false);
	if (c) c->focus(true);
}

Dialog *Gui::open_dialog(const std::string &title, float w, float h) {
	auto d = std::make_unique<Dialog>(*this, title, w, h);
	Dialog *p = d.get();
	popup_.reset();
	set_focus(nullptr);
	dialogs_.push_back(std::move(d));
	return p;
}

void Gui::open_popup(std::unique_ptr<Ctl> p) {
	if (popup_) trash(std::move(popup_));
	popup_ = std::move(p);
}

void Gui::close(Ctl *c) {
	if (popup_.get() == c) {
		trash(std::move(popup_));
		return;
	}
	for (auto it = dialogs_.begin(); it != dialogs_.end(); ++it) {
		if (it->get() != c) continue;
		std::unique_ptr<Ctl> d = std::move(*it);
		dialogs_.erase(it);
		trash(std::move(d));
		return;
	}
}

// Confirm (0x700f2230): the text in lines VPU 14 high, wrapped as UserInformation does, and the
// buttons from the right (No, then Yes), VPU 80 wide.
void Gui::confirm(const std::string &title, const std::string &text, std::function<void(bool)> done) {
	float f = vy(14);
	std::vector<std::string> lines;
	float total = text_width(text, f);
	float wrap = total / vx(400) <= 2 ? vx(400) : total / vx(600) <= 8 ? vx(600) : vx(700);
	for (const std::string &para : split_lines(text)) {
		if (text_width(para, f) <= vx(300)) lines.push_back(para);
		else for (auto &l : wrap_paragraph(*this, para, f, wrap, wrap)) lines.push_back(l);
	}
	float widest = 0;
	for (auto &l : lines) widest = std::max(widest, text_width(l, f));
	float fx = vx(4), bodyf = vx(3), th = vy(30), row = vy(30);
	float w = std::max(widest + 10 + 2 * (fx + bodyf), 2 * vx(80) + 3 * vx(5) + 2 * (fx + bodyf) + 80);
	float h = std::max(180.0f, th + 2 * vy(4) + 2 * bodyf + 10 + 1.5f * f * float(lines.size()) + row + vy(10));
	Dialog *d = open_dialog(title, w, h);
	auto answer = std::make_shared<std::function<void(bool)>>(std::move(done));
	auto finish = [this, d, answer](bool yes) {
		close(d);
		if (*answer) (*answer)(yes);
	};
	d->on_close = [finish] { finish(false); };
	Ctl *body = d->body->add<Ctl>();
	body->set_frame(bodyf, true);
	body->set_fill(theme::kWindowBack);
	for (size_t i = 0; i < lines.size(); ++i) {
		auto *t = body->add<Text>(lines[i], f, theme::kText, 4);
		t->from_top = 10 + 1.5f * f * float(i), t->len_y = 1.5f * f;
	}
	float x = vx(5);
	for (auto [label, yes] : {std::pair{"No", false}, std::pair{"Yes", true}}) {
		auto *b = body->add<Button>(label);
		b->from_right = x, b->len_x = vx(80), b->from_bottom = vy(5), b->len_y = row;
		b->on_click = [finish, yes = yes] { finish(yes); };
		x += vx(80) + vx(5);
	}
}

// UserModalGetString (0x700e6b80): 450 x 150, an edit 30 high, Ok (Enter) and Cancel (Esc).
void Gui::get_string(const std::string &title, const std::string &initial, std::function<void(bool, const std::string &)> done) {
	Dialog *d = open_dialog(title, 450, 150);
	d->frame_x = d->frame_y = 2;
	Ctl *fill = d->body->add<Ctl>();
	fill->set_frame(2, true);
	fill->set_fill(theme::kWindowBack);
	auto *edit = fill->add<Edit>(initial);
	edit->from_left = 5, edit->from_right = 5, edit->from_top = 10, edit->len_y = 30;
	Ctl *strip = fill->add<Ctl>();
	strip->len_x = 200, strip->from_right = 10, strip->len_y = 30, strip->from_bottom = 5;
	auto answer = std::make_shared<std::function<void(bool, const std::string &)>>(std::move(done));
	auto finish = [this, d, edit, answer](bool ok) {
		std::string s = edit->str;
		close(d);
		if (*answer) (*answer)(ok, s);
	};
	auto *ok = strip->add<Button>("Ok", Button::Push);
	ok->sr = 0.45f;
	ok->keys = {kKeyReturn};
	ok->on_click = [finish] { finish(true); };
	auto *cancel = strip->add<Button>("Cancel", Button::Push);
	cancel->sl = 0.55f;
	cancel->keys = {kKeyEscape};
	cancel->on_click = [finish] { finish(false); };
	d->on_close = [finish] { finish(false); };
	layout();
	set_focus(edit);
}

void Gui::layout() {
	W = float(app.view_w), H = float(app.view_h);
	root_->pre_draw_tree();
	root_->place(0, 0, W, H);
	for (auto &d : dialogs_) {
		d->pre_draw_tree();
		float mx = 0, my = 0;
		if (auto *dlg = dynamic_cast<Dialog *>(d.get())) mx = dlg->moved_x, my = dlg->moved_y;
		d->layout(mx, my, W + mx, H + my);
	}
	if (popup_) {
		popup_->pre_draw_tree();
		popup_->layout(0, 0, W, H);
	}
}

void Gui::paint(std::vector<DrawItem> &out) {
	trash_.clear();
	layout();
	Painter p(out, W, H);
	root_->paint(p);
}

void Gui::paint_over(std::vector<DrawItem> &out) {
	Painter p(out, W, H);
	for (auto &d : dialogs_) d->paint(p);
	if (popup_) popup_->paint(p);
}

Ctl *Gui::hit(float x, float y) {
	if (popup_ && popup_->contains(x, y)) return popup_->hit(x, y);
	if (!dialogs_.empty()) return dialogs_.back()->hit(x, y);
	return root_->hit(x, y);
}

bool Gui::mouse_move(float fx, float fy) {
	trash_.clear();
	layout();
	mx_ = fx * W, my_ = fy * H;
	if (pressed_) {
		pressed_->mouse_drag(mx_, my_);
		return true;
	}
	Ctl *h = hit(mx_, my_);
	if (h != hover_) {
		if (hover_) hover_->mouse_leave();
		hover_ = h;
	}
	if (h) h->mouse_move(mx_, my_);
	return modal() || h;
}

bool Gui::mouse_button(bool down, int clicks) {
	trash_.clear();
	layout();
	if (down) {
		// The combo's popup closes when its top control loses the activation (control 0xc).
		if (popup_ && !popup_->contains(mx_, my_)) {
			close(popup_.get());
			return true;
		}
		Ctl *h = hit(mx_, my_);
		if (!h) return modal();
		pressed_ = h;
		set_focus(h);
		h->mouse_down(mx_, my_, clicks);
		return true;
	}
	if (!pressed_) return modal();
	Ctl *p = pressed_;
	pressed_ = nullptr;
	p->mouse_up(mx_, my_, hit(mx_, my_) == p);
	return true;
}

bool Gui::key(int sym) {
	trash_.clear();
	layout();
	if (popup_) {
		if (sym == kKeyEscape) {
			close(popup_.get());
			return true;
		}
		if (focus_ && focus_->inside(popup_.get()) && focus_->key(sym)) return true;
	}
	if (!dialogs_.empty()) {
		Ctl *d = dialogs_.back().get();
		if (focus_ && focus_->inside(d) && focus_->key(sym)) return true;
		d->key(sym);
		return true;
	}
	if (focus_ && focus_->shown() && focus_->key(sym)) return true;
	// DE_CButtonClickOnKey.
	std::function<Button *(Ctl *)> find = [&](Ctl *c) -> Button * {
		if (c->hidden || c->disabled) return nullptr;
		if (auto *b = dynamic_cast<Button *>(c); b && std::find(b->keys.begin(), b->keys.end(), sym) != b->keys.end()) return b;
		for (auto &ch : c->children) {
			if (Button *b = find(ch.get())) return b;
		}
		return nullptr;
	};
	if (Button *b = find(root_.get())) {
		b->click();
		return true;
	}
	return false;
}

bool Gui::text(const std::string &t) {
	trash_.clear();
	if (focus_ && dynamic_cast<Edit *>(focus_)) {
		focus_->text(t);
		return true;
	}
	return modal();
}

bool Gui::wheel(int dy) {
	trash_.clear();
	layout();
	Ctl *c = nullptr;
	if (popup_ && popup_->contains(mx_, my_)) c = popup_->under(mx_, my_);
	else if (!dialogs_.empty()) c = dialogs_.back()->under(mx_, my_);
	else c = root_->under(mx_, my_);
	for (; c; c = c->parent) {
		if (c->wheel(dy)) return true;
	}
	return modal();
}

}  // namespace game::ui
