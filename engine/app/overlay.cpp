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
}

GLuint OverlayRenderer::image(const game::GameObject *img) {
	if (!img || !img->file) return 0;
	auto key = std::make_pair(img->file, img->index);
	if (auto it = images_.find(key); it != images_.end()) return it->second;
	GLuint t = 0;
	try {
		auto im = de::read_image(*img->file, img->file->objects()[img->index]);
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

void OverlayRenderer::text(const FontGL &f, const game::World::Primitive &t, const float *rgba, float dx, float dy) {
	const de::Font &font = f.font;
	if (font.height <= 0 || t.text.empty()) return;
	float scale = t.height / font.height;  // screen fractions (of the height) per font unit
	const float spacing = 0;               // DE_CFlatText3 +0x9c letter spacing (default 0): letters advance by their width
	float total = 0;
	for (unsigned char ch : t.text) {
		auto &g = font.glyphs[ch];
		total += (g.present ? g.x1 - g.x0 : 0.5f) + spacing;
	}
	// Widths are in height units: convert x by the aspect ratio.
	float sx = scale / aspect_;
	float x = t.x - (t.centered ? total * sx * 0.5f : t.snap_right ? total * sx : 0) + dx;
	std::vector<float> v;
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
	if (v.empty()) return;
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(float)), v.data(), GL_STREAM_DRAW);
	glUniform4fv(glGetUniformLocation(prog_, "color"), 1, rgba);
	glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size() / 4));
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
		// A fighter's HUD shows for the fighter in focus only (stand-in for the renderer's
		// camera test).
		if (const auto *f = fighter_of(t.mesh); f && f != focus) continue;
		order.push_back(&t);
	}
	// Larger sort values are further back.
	std::stable_sort(order.begin(), order.end(), [](auto *a, auto *b) { return a->sort > b->sort; });
	auto mode = [&](int m) { glUniform1i(glGetUniformLocation(prog_, "mode"), m); };
	for (auto *t : order) {
		glBlendFunc(GL_SRC_ALPHA, t->additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
		if (t->kind == game::World::Primitive::Text) {
			auto it = fonts_.find(t->in_game_font ? "Font_Metalic" : "Font_Ariel");
			if (it == fonts_.end()) it = fonts_.begin();
			if (it == fonts_.end()) break;
			glBindTexture(GL_TEXTURE_2D, it->second.tex);
			mode(it->second.alpha ? 0 : 1);
			if (t->shadow) {
				// TextSetShadowOnCaller: 0.06 font units right and down, black at half alpha.
				float shadow[4] = {0, 0, 0, t->rgba[3] * 0.5f};
				float off = 0.06f * t->height / std::max(1e-6f, it->second.font.height);
				text(it->second, *t, shadow, off / aspect_, -off);
			}
			text(it->second, *t, t->rgba, 0, 0);
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

}  // namespace render
