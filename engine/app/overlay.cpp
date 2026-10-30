#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "de/image.h"

namespace render {

namespace {

const char *kVS = R"(#version 410
layout(location = 0) in vec2 pos;  // screen fractions, y up (DE_CFlatText3::CalcScreenCoordinates)
layout(location = 1) in vec2 uv;
out vec2 v_uv;
void main() {
	v_uv = uv;
	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
})";
const char *kVS3 = R"(#version 410
layout(location = 0) in vec3 pos;  // world space
layout(location = 1) in vec2 uv;
uniform mat4 vp;
out vec2 v_uv;
void main() {
	v_uv = uv;
	gl_Position = vp * vec4(pos, 1.0);
})";
const char *kFS = R"(#version 410
in vec2 v_uv;
uniform sampler2D tex;
uniform vec4 color;
uniform int mode;  // 0 texture rgba, 1 luminance as alpha (fonts without alpha), 2 untextured
out vec4 frag;
void main() {
	vec4 t = mode == 2 ? vec4(1.0) : texture(tex, v_uv);
	float a = mode == 1 ? max(t.r, max(t.g, t.b)) : t.a;
	frag = vec4(t.rgb * color.rgb, a * color.a);
})";

const char *kVSC = R"(#version 410
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 col;
out vec4 v_col;
void main() {
	v_col = col;
	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
})";
const char *kFSC = R"(#version 410
in vec4 v_col;
out vec4 frag;
void main() { frag = v_col; })";

// The fighter a primitive's mesh belongs to, if any.
// A mesh is drawn while the actor it belongs to is renderable (+0x108; SetRendered /
// SetUnrendered, which BRenderActorIfCamInTree sets from the renderer's camera).
bool rendered(const game::GameObject *mesh) {
	for (const game::GameObject *o = mesh; o; o = o->parent) {
		if (o->is_actor) return o->visible;
	}
	return true;
}

}  // namespace

void OverlayRenderer::load(const de::World &world) {
	// DE_CFlatText3::FONT_DEFAULT = "Font_Ariel", FONT_IN_GAME_DEFAULT = "Font_Metalic"
	// (CFontInfo::CFontInfo 0x701c9110; SetInGameFont, the scripts' "complicated font").
	for (const char *name : {"Font_Ariel", "Font_Metalic"}) {
		try {
			FontGL f;
			f.font = de::read_font(world, name);
			if (f.font.image) {
				auto img = de::read_image(*f.font.image.file, f.font.image.info());
				f.alpha = img.channels.count(3) != 0;
				auto px = img.rgba();
				f.tex = glu::texture_rgba(img.width, img.height, px.data());
			}
			fonts_[name] = f;
		} catch (const std::exception &e) {
			fprintf(stderr, "font %s: %s\n", name, e.what());
		}
	}
	prog_ = glu::program(kVS, kFS);
	glGenVertexArrays(1, &vao_);
	glGenBuffers(1, &vbo_);
	glBindVertexArray(vao_);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void *>(8));
	progc_ = glu::program(kVSC, kFSC);
	glGenVertexArrays(1, &vaoc_);
	glGenBuffers(1, &vboc_);
	glBindVertexArray(vaoc_);
	glBindBuffer(GL_ARRAY_BUFFER, vboc_);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, nullptr);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, reinterpret_cast<void *>(8));
	prog3_ = glu::program(kVS3, kFS);
	glGenVertexArrays(1, &vao3_);
	glGenBuffers(1, &vbo3_);
	glBindVertexArray(vao3_);
	glBindBuffer(GL_ARRAY_BUFFER, vbo3_);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, nullptr);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, reinterpret_cast<void *>(12));
}

GLuint OverlayRenderer::image(const game::GameObject *img) {
	if (!img || !img->file) return 0;
	return image(img->file, img->index);
}

GLuint OverlayRenderer::image(const de::ObjectsFile *file, uint32_t index) {
	auto key = std::make_pair(file, index);
	if (auto it = images_.find(key); it != images_.end()) return it->second;
	GLuint t = 0;
	try {
		auto im = de::read_image(*file, file->objects()[index]);
		if (!im.channels.empty()) {
			auto px = im.rgba();
			t = glu::texture_rgba(im.width, im.height, px.data());
		}
	} catch (const std::exception &) {
	}
	return images_[key] = t;
}

void OverlayRenderer::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float *rgba) {
	float q[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1},
	                 {x0, y0, u0, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
	glUniform4fv(glGetUniformLocation(prog_, "color"), 1, rgba);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

void OverlayRenderer::quadv(const std::array<float, 8> &xy, const std::array<float, 8> &uv, const float *rgba) {
	float q[6][4];
	const int order[6] = {0, 1, 2, 0, 2, 3};
	for (int i = 0; i < 6; ++i) {
		size_t k = size_t(order[i]) * 2;
		q[i][0] = xy[k], q[i][1] = xy[k + 1], q[i][2] = uv[k], q[i][3] = uv[k + 1];
	}
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
	glUniform4fv(glGetUniformLocation(prog_, "color"), 1, rgba);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

std::vector<float> OverlayRenderer::text_vertices(const FontGL &f, const game::World::Primitive &t, float sx, float sy, float dx,
                                                  float dy) const {
	std::vector<float> v;
	const de::Font &font = f.font;
	if (font.height <= 0 || t.text.empty()) return v;
	const float spacing = 0;  // DE_CFlatText3 +0x9c letter spacing (default 0): letters advance by their width
	float total = 0;
	for (unsigned char ch : t.text) {
		auto &g = font.glyphs[ch];
		total += (g.present ? g.x1 - g.x0 : 0.5f) + spacing;
	}
	float x = t.x - (t.centered ? total * sx * 0.5f : t.snap_right ? total * sx : 0) + dx;
	for (unsigned char ch : t.text) {
		auto &g = font.glyphs[ch];
		float w = g.present ? g.x1 - g.x0 : 0.5f;
		if (g.present && ch != ' ') {
			float x0 = x, x1 = x + w * sx, y0 = t.y + (g.y0 + font.base) * sy + dy, y1 = t.y + (g.y1 + font.base) * sy + dy;
			// Image rows run top to bottom: the glyph's top gets the smaller v.
			float q[6][4] = {{x0, y1, g.u0, g.v0}, {x1, y1, g.u1, g.v0}, {x1, y0, g.u1, g.v1},
			                 {x0, y1, g.u0, g.v0}, {x1, y0, g.u1, g.v1}, {x0, y0, g.u0, g.v1}};
			for (auto &p : q) v.insert(v.end(), p, p + 4);
		}
		x += (w + spacing) * sx;
	}
	return v;
}

void OverlayRenderer::text(const FontGL &f, const game::World::Primitive &t, const float *rgba, float sx, float dx, float dy) {
	std::vector<float> v = text_vertices(f, t, sx, t.height, dx, dy);
	if (v.empty()) return;
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(float)), v.data(), GL_STREAM_DRAW);
	glUniform4fv(glGetUniformLocation(prog_, "color"), 1, rgba);
	glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size() / 4));
}

const OverlayRenderer::FontGL *OverlayRenderer::font_for(const game::World::Primitive &t) const {
	auto it = fonts_.find(t.in_game_font ? "Font_Metalic" : "Font_Ariel");
	if (it == fonts_.end()) it = fonts_.begin();
	return it == fonts_.end() ? nullptr : &it->second;
}

void OverlayRenderer::draw_world(const game::World &world, const float *vp) {
	if (!prog3_) return;
	std::vector<const game::World::Primitive *> order;
	for (auto &[obj, t] : world.texts) {
		if (world.screen_space_meshes.count(t.mesh) || !t.mesh->parent || t.rgba[3] <= 0.01f) continue;
		if (t.kind != game::World::Primitive::Quad) continue;
		if (!rendered(t.mesh)) continue;
		order.push_back(&t);
	}
	if (order.empty()) return;
	std::stable_sort(order.begin(), order.end(), [](auto *a, auto *b) { return a->sort > b->sort; });
	glEnable(GL_BLEND);
	glDepthMask(GL_FALSE);
	glUseProgram(prog3_);
	glUniformMatrix4fv(glGetUniformLocation(prog3_, "vp"), 1, GL_FALSE, vp);
	glUniform1i(glGetUniformLocation(prog3_, "tex"), 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(vao3_);
	auto submit = [&](const game::World::Primitive &t, const std::vector<float> &v2, const float *rgba) {
		// Local (x, y, z) -> world through the mesh's actor.
		const de::Transform at = t.mesh->parent->world();
		std::vector<float> v;
		for (size_t i = 0; i + 3 < v2.size(); i += 4) {
			auto w = at.apply({v2[i], v2[i + 1], t.z});
			v.insert(v.end(), {w[0], w[1], w[2], v2[i + 2], v2[i + 3]});
		}
		if (v.empty()) return;
		glBindBuffer(GL_ARRAY_BUFFER, vbo3_);
		glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(float)), v.data(), GL_STREAM_DRAW);
		glUniform4fv(glGetUniformLocation(prog3_, "color"), 1, rgba);
		glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size() / 5));
	};
	for (auto *t : order) {
		if (t->modulate) glBlendFunc(GL_DST_COLOR, GL_ZERO);
		else glBlendFunc(GL_SRC_ALPHA, t->additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
		if (t->overlay) glDisable(GL_DEPTH_TEST);
		else glEnable(GL_DEPTH_TEST);
		auto mode = [&](int m) { glUniform1i(glGetUniformLocation(prog3_, "mode"), m); };
		GLuint tex = image(t->texture);
		glBindTexture(GL_TEXTURE_2D, tex);
		mode(tex ? 0 : 2);
		const float *b = t->box;
		const float *uv = t->uv;
		submit(*t, {b[0], b[1], uv[0], uv[1], b[2], b[1], uv[2], uv[1], b[2], b[3], uv[2], uv[3],
		            b[0], b[1], uv[0], uv[1], b[2], b[3], uv[2], uv[3], b[0], b[3], uv[0], uv[3]},
		       t->rgba);
	}
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}

void OverlayRenderer::draw(const game::World &world, int width, int height) {
	if (!prog_) return;
	aspect_ = float(width) / float(std::max(1, height));
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glUseProgram(prog_);
	glBindVertexArray(vao_);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(prog_, "tex"), 0);
	std::vector<const game::World::Primitive *> order;
	for (auto &[obj, t] : world.texts) {
		// A text primitive's DE_CFlatText3 is always screen space (NewText sets its screen position).
		if (t.kind != game::World::Primitive::Text && !world.screen_space_meshes.count(t.mesh)) continue;
		if (t.rgba[3] <= 0.01f) continue;
		if (t.kind == game::World::Primitive::Text && t.text.empty()) continue;
		if (!game::in_scene(*t.mesh)) continue;  // its actor left the scene (a deactivated overlay)
		if (!rendered(t.mesh)) continue;
		order.push_back(&t);
	}
	// Larger sort values are further back.
	std::stable_sort(order.begin(), order.end(), [](auto *a, auto *b) { return a->sort > b->sort; });
	auto mode = [&](int m) { glUniform1i(glGetUniformLocation(prog_, "mode"), m); };
	for (auto *t : order) {
		if (t->modulate) glBlendFunc(GL_DST_COLOR, GL_ZERO);
		else glBlendFunc(GL_SRC_ALPHA, t->additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
		if (t->kind == game::World::Primitive::Text) {
			const FontGL *f = font_for(*t);
			if (!f) break;
			glBindTexture(GL_TEXTURE_2D, f->tex);
			mode(f->alpha ? 0 : 1);
			// CalcScreenCoordinates (0x701cdb30): font units times the size, in fractions of the screen's width
			// across and its height up.
			if (t->shadow) {
				// TextSetShadowOnCaller: 0.06 font units right and down, black at half alpha.
				float shadow[4] = {0, 0, 0, t->rgba[3] * 0.5f};
				float off = 0.06f * t->height;
				text(*f, *t, shadow, t->height, off, -off);
			}
			text(*f, *t, t->rgba, t->height, 0, 0);
			continue;
		}
		GLuint tex = image(t->texture);
		glBindTexture(GL_TEXTURE_2D, tex);
		mode(tex ? 0 : 2);
		const float *b = t->box;
		if (t->kind == game::World::Primitive::Quad) {
			quad(b[0], b[1], b[2], b[3], t->uv[0], t->uv[1], t->uv[2], t->uv[3], t->rgba);
			continue;
		}
		// CStatusBarPrimitive::UpdateBar (0x7012d210): the body (the texture's lower half) up to one
		// bar thickness short of the value's point and a cap (upper half) one thickness either side
		// of it; a cap reaching past the start is cut there, its u moved on by half the cut.
		const float x0 = std::min(b[0], b[2]), x1 = std::max(b[0], b[2]), y0 = std::min(b[1], b[3]), y1 = std::max(b[1], b[3]);
		if (!t->bottom_to_top) {
			float h = y1 - y0, p = x0 + (x1 - x0) * t->value, lo = p - h, hi = p + h;
			if (hi <= x0) continue;
			if (lo > x0) quadv({x0, y0, lo, y0, lo, y1, x0, y1}, {0.1f, 1, 0.9f, 1, 0.9f, 0.5f, 0.1f, 0.5f}, t->rgba);
			float s = std::max(lo, x0), u = lo > x0 ? 0.1f : 0.1f - (lo - x0) / h * 0.5f;
			quadv({s, y0, hi, y0, hi, y1, s, y1}, {u, 0.5f, 0.9f, 0.5f, 0.9f, 0, u, 0}, t->rgba);
		} else {
			float w = x1 - x0, p = y0 + (y1 - y0) * t->value, lo = p - w, hi = p + w;
			if (hi <= y0) continue;
			if (lo > y0) quadv({x0, y0, x1, y0, x1, lo, x0, lo}, {0.1f, 1, 0.1f, 0.5f, 0.9f, 0.5f, 0.9f, 1}, t->rgba);
			float s = std::max(lo, y0), u = lo > y0 ? 0.1f : 0.1f - (lo - y0) / w * 0.5f;
			quadv({x0, s, x1, s, x1, hi, x0, hi}, {u, 0.5f, u, 0, 0.9f, 0, 0.9f, 0.5f}, t->rgba);
		}
	}
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
}

void OverlayRenderer::begin_screen(int width, int height) {
	aspect_ = float(width) / float(std::max(1, height));
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(prog_);
	glBindVertexArray(vao_);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(prog_, "tex"), 0);
}

void OverlayRenderer::end_screen() {
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
}

void OverlayRenderer::rect(float x0, float y0, float x1, float y1, const float *rgba, int width, int height) {
	if (!prog_) return;
	begin_screen(width, height);
	glUniform1i(glGetUniformLocation(prog_, "mode"), 2);
	quad(x0, 1 - y1, x1, 1 - y0, 0, 0, 1, 1, rgba);
	end_screen();
}

void OverlayRenderer::label(const std::string &str, float x, float y, float size, const float *rgba, int width, int height, bool centered) {
	if (!prog_ || str.empty()) return;
	game::World::Primitive t;
	t.text = str;
	t.height = size;
	t.centered = centered;
	t.x = x;
	const FontGL *f = font_for(t);
	if (!f) return;
	begin_screen(width, height);
	std::vector<float> v = text_vertices(*f, t, size / aspect_, size, 0, 0);
	float lo = 1e9f, hi = -1e9f;
	for (size_t i = 1; i < v.size(); i += 4) lo = std::min(lo, v[i]), hi = std::max(hi, v[i]);
	if (!v.empty()) {
		float dy = (1 - y) - (lo + hi) / 2;
		for (size_t i = 1; i < v.size(); i += 4) v[i] += dy;
		glBindTexture(GL_TEXTURE_2D, f->tex);
		glUniform1i(glGetUniformLocation(prog_, "mode"), f->alpha ? 0 : 1);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_);
		glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(float)), v.data(), GL_STREAM_DRAW);
		glUniform4fv(glGetUniformLocation(prog_, "color"), 1, rgba);
		glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size() / 4));
	}
	end_screen();
}

void OverlayRenderer::image_rect(const de::ObjectRef &img, float x0, float y0, float x1, float y1, int width, int height, const float *tint) {
	if (!prog_ || !img) return;
	GLuint tex = image(img.file, img.index);
	if (!tex) return;
	begin_screen(width, height);
	glBindTexture(GL_TEXTURE_2D, tex);
	glUniform1i(glGetUniformLocation(prog_, "mode"), 0);
	const float white[4] = {1, 1, 1, 1};
	quad(x0, 1 - y1, x1, 1 - y0, 0, 1, 1, 0, tint ? tint : white);
	end_screen();
}

void OverlayRenderer::texture_rect(GLuint tex, float x0, float y0, float x1, float y1, int width, int height) {
	if (!prog_ || !tex) return;
	begin_screen(width, height);
	glBindTexture(GL_TEXTURE_2D, tex);
	glUniform1i(glGetUniformLocation(prog_, "mode"), 0);
	const float white[4] = {1, 1, 1, 1};
	quad(x0, 1 - y1, x1, 1 - y0, 0, 0, 1, 1, white);
	end_screen();
}

float OverlayRenderer::text_width(const std::string &str, float size, int width, int height) {
	game::World::Primitive t;
	t.text = str;
	t.height = size;
	const FontGL *f = font_for(t);
	if (!f) return 0;
	std::vector<float> v = text_vertices(*f, t, size * float(std::max(1, height)) / float(std::max(1, width)), size, 0, 0);
	float lo = 1e9f, hi = -1e9f;
	for (size_t i = 0; i < v.size(); i += 4) lo = std::min(lo, v[i]), hi = std::max(hi, v[i]);
	return v.empty() ? 0 : hi - lo;
}

void OverlayRenderer::label_cell(const std::string &str, float x, float y, float size, const float *rgba, int width, int height, bool centered) {
	if (!prog_ || str.empty()) return;
	game::World::Primitive t;
	t.text = str;
	t.height = size;
	t.centered = centered;
	t.x = x;
	t.y = 1 - (y + size);
	const FontGL *f = font_for(t);
	if (!f) return;
	begin_screen(width, height);
	glBindTexture(GL_TEXTURE_2D, f->tex);
	glUniform1i(glGetUniformLocation(prog_, "mode"), f->alpha ? 0 : 1);
	text(*f, t, rgba, size / aspect_, 0, 0);
	end_screen();
}

std::vector<float> OverlayRenderer::char_offsets(const std::string &str, float size, int width, int height) {
	std::vector<float> o{0};
	game::World::Primitive t;
	t.height = size;
	const FontGL *f = font_for(t);
	if (!f || f->font.height <= 0) return std::vector<float>(str.size() + 1, 0.0f);
	float sx = size * float(std::max(1, height)) / float(std::max(1, width));
	for (unsigned char ch : str) {
		auto &g = f->font.glyphs[ch];
		o.push_back(o.back() + (g.present ? g.x1 - g.x0 : 0.5f) * sx);
	}
	return o;
}

void OverlayRenderer::quad4(const float *xy, const float *rgba, int width, int height) {
	if (!progc_) return;
	float v[6][6];
	const int order[6] = {0, 1, 2, 0, 2, 3};
	for (int i = 0; i < 6; ++i) {
		int k = order[i];
		v[i][0] = xy[k * 2], v[i][1] = 1 - xy[k * 2 + 1];
		for (int c = 0; c < 4; ++c) v[i][2 + c] = rgba[k * 4 + c];
	}
	aspect_ = float(width) / float(std::max(1, height));
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(progc_);
	glBindVertexArray(vaoc_);
	glBindBuffer(GL_ARRAY_BUFFER, vboc_);
	glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	end_screen();
}

}  // namespace render
