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

// The fighter a primitive's mesh belongs to, if any.
const game::GameObject *fighter_of(const game::GameObject *o) {
	for (; o; o = o->parent) {
		if (o->fighter) return o;
	}
	return nullptr;
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

std::vector<float> OverlayRenderer::text_vertices(const FontGL &f, const game::World::Primitive &t, float aspect, float dx,
                                                  float dy) const {
	std::vector<float> v;
	const de::Font &font = f.font;
	if (font.height <= 0 || t.text.empty()) return v;
	float scale = t.height / font.height;  // primitive units (of the height) per font unit
	const float spacing = 0;               // DE_CFlatText3 +0x9c letter spacing (default 0): letters advance by their width
	float total = 0;
	for (unsigned char ch : t.text) {
		auto &g = font.glyphs[ch];
		total += (g.present ? g.x1 - g.x0 : 0.5f) + spacing;
	}
	// Widths are in height units: in screen space convert x by the aspect ratio.
	float sx = scale / aspect;
	float x = t.x - (t.centered ? total * sx * 0.5f : t.snap_right ? total * sx : 0) + dx;
	for (unsigned char ch : t.text) {
		auto &g = font.glyphs[ch];
		float w = g.present ? g.x1 - g.x0 : 0.5f;
		if (g.present && ch != ' ') {
			float x0 = x, x1 = x + w * sx, y0 = t.y + g.y0 * scale + dy, y1 = t.y + g.y1 * scale + dy;
			// Image rows run top to bottom: the glyph's top gets the smaller v.
			float q[6][4] = {{x0, y1, g.u0, g.v0}, {x1, y1, g.u1, g.v0}, {x1, y0, g.u1, g.v1},
			                 {x0, y1, g.u0, g.v0}, {x1, y0, g.u1, g.v1}, {x0, y0, g.u0, g.v1}};
			for (auto &p : q) v.insert(v.end(), p, p + 4);
		}
		x += (w + spacing) * sx;
	}
	return v;
}

void OverlayRenderer::text(const FontGL &f, const game::World::Primitive &t, const float *rgba, float dx, float dy) {
	std::vector<float> v = text_vertices(f, t, aspect_, dx, dy);
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
		if (t.kind != game::World::Primitive::Quad && (t.kind != game::World::Primitive::Text || t.text.empty())) continue;
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
		if (t->kind == game::World::Primitive::Text) {
			const FontGL *f = font_for(*t);
			if (!f) break;
			glBindTexture(GL_TEXTURE_2D, f->tex);
			mode(f->alpha ? 0 : 1);
			if (t->shadow) {
				float shadow[4] = {0, 0, 0, t->rgba[3] * 0.5f};
				float off = 0.06f * t->height / std::max(1e-6f, f->font.height);
				submit(*t, text_vertices(*f, *t, 1, off, -off), shadow);
			}
			submit(*t, text_vertices(*f, *t, 1, 0, 0), t->rgba);
			continue;
		}
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

void OverlayRenderer::draw(const game::World &world, int width, int height, const game::GameObject *focus) {
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
		if (!world.screen_space_meshes.count(t.mesh) || t.rgba[3] <= 0.01f) continue;
		if (t.kind == game::World::Primitive::Text && t.text.empty()) continue;
		if (!game::in_scene(*t.mesh)) continue;  // its actor left the scene (a deactivated overlay)
		// A fighter's HUD shows for the fighter in focus only (stand-in for the renderer's
		// camera test).
		if (const auto *f = fighter_of(t.mesh); f && f != focus) continue;
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
			if (t->shadow) {
				// TextSetShadowOnCaller: 0.06 font units right and down, black at half alpha.
				float shadow[4] = {0, 0, 0, t->rgba[3] * 0.5f};
				float off = 0.06f * t->height / std::max(1e-6f, f->font.height);
				text(*f, *t, shadow, off / aspect_, -off);
			}
			text(*f, *t, t->rgba, 0, 0);
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
		// CStatusBarPrimitive::UpdateBar: the filled body (texture's lower half) up to
		// begin + (end - begin) x value, and a glowing cap (upper half) one bar thickness either
		// side of that point.
		if (!t->bottom_to_top) {
			float h = std::fabs(b[3] - b[1]), p = b[0] + (b[2] - b[0]) * t->value;
			if (p - h > b[0]) quad(b[0], b[1], p - h, b[3], 0.1f, 0.5f, 0.9f, 1.0f, t->rgba);
			if (t->value > 0) quad(std::max(b[0], p - h), b[1], p + h, b[3], 0.1f, 0.0f, 0.9f, 0.5f, t->rgba);
		} else {
			float w = std::fabs(b[2] - b[0]), p = b[1] + (b[3] - b[1]) * t->value;
			if (p - w > b[1]) quad(b[0], b[1], b[2], p - w, 0.1f, 0.5f, 0.9f, 1.0f, t->rgba);
			if (t->value > 0) quad(b[0], std::max(b[1], p - w), b[2], p + w, 0.1f, 0.0f, 0.9f, 0.5f, t->rgba);
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
	std::vector<float> v = text_vertices(*f, t, aspect_, 0, 0);
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

float OverlayRenderer::text_width(const std::string &str, float size, int width, int height) {
	game::World::Primitive t;
	t.text = str;
	t.height = size;
	const FontGL *f = font_for(t);
	if (!f) return 0;
	std::vector<float> v = text_vertices(*f, t, float(width) / float(std::max(1, height)), 0, 0);
	float lo = 1e9f, hi = -1e9f;
	for (size_t i = 0; i < v.size(); i += 4) lo = std::min(lo, v[i]), hi = std::max(hi, v[i]);
	return v.empty() ? 0 : hi - lo;
}

}  // namespace render
