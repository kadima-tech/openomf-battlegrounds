#include "particles_gl.h"

#include <algorithm>
#include <cmath>

#include "de/image.h"

namespace render {

namespace {

const char *kVS = R"(#version 410
layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 rgba;
uniform mat4 vp;
out vec2 v_uv;
out vec4 v_rgba;
void main() {
	v_uv = uv;
	v_rgba = rgba;
	gl_Position = vp * vec4(pos, 1.0);
})";
const char *kFS = R"(#version 410
in vec2 v_uv;
in vec4 v_rgba;
uniform sampler2D tex;
uniform int additive;
out vec4 frag;
void main() {
	vec4 t = texture(tex, v_uv);
	// Additive quads are premultiplied: the texture's alpha scales the colour it adds.
	frag = additive == 1 ? vec4(t.rgb * t.a * v_rgba.rgb, 1.0) : t * v_rgba;
})";

using Vec = std::array<float, 3>;
Vec add(const Vec &a, const Vec &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec sub(const Vec &a, const Vec &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec mul(const Vec &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float len(const Vec &a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
Vec norm(const Vec &a) {
	float l = len(a);
	return l > 1e-9f ? mul(a, 1 / l) : a;
}
Vec cross(const Vec &a, const Vec &b) {
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec row(const std::array<float, 9> &r, int i) { return {r[size_t(i * 3)], r[size_t(i * 3 + 1)], r[size_t(i * 3 + 2)]}; }

}  // namespace

void ParticleRenderer::load() {
	prog_ = glu::program(kVS, kFS);
	glGenVertexArrays(1, &vao_);
	glGenBuffers(1, &vbo_);
	glBindVertexArray(vao_);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, pos));
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, uv));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, rgba));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	uint8_t px[4] = {255, 255, 255, 255};
	white_ = glu::texture_rgba(1, 1, px);
}

GLuint ParticleRenderer::image(const de::ObjectRef &r) {
	if (!r) return white_;
	auto k = std::make_pair(r.file, r.index);
	if (auto it = images_.find(k); it != images_.end()) return it->second;
	GLuint t = white_;
	try {
		auto img = de::read_image(*r.file, r.info());
		if (!img.channels.empty()) {
			auto px = img.rgba();
			t = glu::texture_rgba(img.width, img.height, px.data());
		}
	} catch (const std::exception &) {
	}
	return images_[k] = t;
}

// DE_CRingParticleRenderer::DE_CInstanceInfo::Process (0x40015c00): per particle, an annulus of
// 40 segments in its plane, from the inner edge (radius and height curves at the particle's age,
// times its size; inner colour) to the outer edge (outer curves and colour).
void ParticleRenderer::ring(const game::ParticleInstance &in, const std::array<float, 3> &right, const std::array<float, 3> &up,
                            std::map<std::pair<GLuint, bool>, Batch> &batches) {
	const de::ParticleRendererDef &d = *in.def;
	de::Transform at = in.actor->world();
	const bool additive = d.blend != 2;
	Batch &b = batches[{image(d.image), additive}];
	b.tex = image(d.image);
	b.additive = additive;
	constexpr int kSegments = 40;  // the angle steps by 0.05 pi
	for (const game::Particle &p : in.particles) {
		float age = std::clamp(p.age, 0.0f, 1.0f);
		Vec c = d.generator.local ? at.apply(p.pos) : p.pos;
		// The ring's plane: X and Y axes of its frame.
		Vec ax, ay;
		if (d.ring_plane == 3) {
			ax = right;
			ay = up;
		} else {
			const auto &rows = d.generator.channels[de::kChOrientation] ? p.orientation : at.r;
			Vec x = row(rows, 0), y = row(rows, 1), z = row(rows, 2);
			if (d.generator.channels[de::kChOrientation] && d.generator.local) {
				x = at.rotate(x);
				y = at.rotate(y);
				z = at.rotate(z);
			}
			if (d.ring_plane == 1) {
				y = z;  // RotateLocalX(90): the ring lies in the X-Z plane
			} else if (d.ring_plane == 2) {
				x = z;  // RotateLocalY(90): the Z-Y plane
			}
			ax = x;
			ay = y;
		}
		Vec normal = norm(cross(ax, ay));
		float r0 = d.inner_radius.eval(age) * p.size, r1 = d.outer_radius.eval(age) * p.size;
		float h0 = d.inner_height.eval(age) * p.size, h1 = d.outer_height.eval(age) * p.size;
		auto c0 = d.inner_color.eval(age), c1 = d.outer_color.eval(age);
		if (additive) {
			c0 = {c0[0] * c0[3], c0[1] * c0[3], c0[2] * c0[3], 1};
			c1 = {c1[0] * c1[3], c1[1] * c1[3], c1[2] * c1[3], 1};
		}
		auto at_angle = [&](float a, float radius, float height) {
			return add(add(c, mul(normal, height)), add(mul(ax, std::cos(a) * radius), mul(ay, std::sin(a) * radius)));
		};
		for (int i = 0; i < kSegments; ++i) {
			float a0 = float(i) / kSegments * 6.2831853f, a1 = float(i + 1) / kSegments * 6.2831853f;
			Vec q[4] = {at_angle(a0, r0, h0), at_angle(a0, r1, h1), at_angle(a1, r1, h1), at_angle(a1, r0, h0)};
			float u0 = float(i) / kSegments, u1 = float(i + 1) / kSegments;
			float uv[4][2] = {{u0, 0}, {u0, 1}, {u1, 1}, {u1, 0}};
			const std::array<float, 4> *cols[4] = {&c0, &c1, &c1, &c0};
			for (int k : {0, 1, 2, 0, 2, 3}) {
				Vertex v;
				for (int j = 0; j < 3; ++j) v.pos[j] = q[k][size_t(j)];
				v.uv[0] = uv[k][0];
				v.uv[1] = uv[k][1];
				for (int j = 0; j < 4; ++j) v.rgba[j] = (*cols[k])[size_t(j)];
				b.v.push_back(v);
			}
		}
	}
}

void ParticleRenderer::draw(const game::World &world, const float *vp, const float *view, const float *eye, bool glow_layer) {
	if (!prog_) return;
	const Vec right{view[0], view[4], view[8]}, up{view[1], view[5], view[9]};
	const Vec cam{eye[0], eye[1], eye[2]};
	std::map<std::pair<GLuint, bool>, Batch> batches;
	for (auto &inp : world.particles->instances()) {
		const game::ParticleInstance &in = *inp;
		if (in.particles.empty() || in.stepped + 1 < world.frame() || in.actor->meshes_disabled) continue;
		const de::ParticleRendererDef &d = *in.def;
		// The glow layer: only renderers with a GlowFactor, scaled by a quarter of it (clamped to 1),
		// in black exponential fog (density 0.0007; DE_CGenericShader::ActivateAndDrawPatches flag
		// 0x400).
		if (glow_layer && d.glow_factor <= 0) continue;
		if (d.kind == "Ring") {
			if (glow_layer) continue;
			ring(in, right, up, batches);
			continue;
		}
		if (d.kind != "BillboardedQuad") continue;  // StreamParticleRenderer and FourFaceTriangle are not drawn yet
		de::Transform at = in.actor->world();
		const bool local = d.generator.local;
		const float scale = len(row(at.r, 0));
		const bool additive = d.blend != 2;
		Batch &b = batches[{image(d.image), additive}];
		b.tex = image(d.image);
		b.additive = additive;
		// The renderer's colour (DE_CRGBA::NormalizeAlpha for the additive blends).
		std::array<float, 4> base = d.color.rgba;
		const std::array<float, 4> *set_rgb = world.particles->value_color(d.color_ref);
		if (set_rgb) base = {(*set_rgb)[0], (*set_rgb)[1], (*set_rgb)[2], (*set_rgb)[3] >= 0 ? (*set_rgb)[3] : base[3]};
		if (additive) base = {base[0] * base[3], base[1] * base[3], base[2] * base[3], 1};
		const float glow = d.cone_mode == 3 ? d.glow.eval(0) : 1;
		for (const game::Particle &p : in.particles) {
			Vec c = local ? at.apply(p.pos) : p.pos;
			float s = p.size * scale * glow;
			float hw = d.width * s, hh = d.height * s;
			// Colour (InstanceInfo::Process, the switch on +0x90).
			std::array<float, 4> col = base;
			float age = std::clamp(p.age, 0.0f, 1.0f);
			switch (d.color_mode) {
			case 1: for (auto &x : col) x *= age; break;
			case 2: for (auto &x : col) x *= 1 - age; break;
			case 3: for (auto &x : col) x *= p.intensity; break;
			case 4: col[3] *= age; break;
			case 5: col[3] *= 1 - age; break;
			case 7: col = set_rgb ? base : d.color.eval(age); break;
			case 8:
				for (int k = 0; k < 4; ++k) col[size_t(k)] = p.color[size_t(k)] * base[size_t(k)];
				break;
			default: break;
			}
			if (additive) col = {col[0] * col[3], col[1] * col[3], col[2] * col[3], 1};
			if (glow_layer) {
				float k = std::fmin(d.glow_factor * 0.25f, 1.0f) * std::exp(-0.0007f * len(sub(c, cam)));
				for (int j = 0; j < 3; ++j) col[size_t(j)] *= k;
			}
			// Texture coordinates (+0x8c): the whole image, or a frame of a 4x4 sheet.
			float u0 = 0, v0 = 0, du = 1, dv = 1;
			if (d.uv_mode == 1 || d.uv_mode == 2) {
				int frame = d.uv_mode == 1 ? std::min(15, int(age * 16)) : (p.serial & 15);
				u0 = float(frame & 3) * 0.25f;
				v0 = float(frame >> 2 & 3) * 0.25f;
				du = dv = 0.25f;
			}
			// Corners (SetParticleVerticies, the switch on +0x88).
			Vec a, bb;  // half-extent vectors: corners are c -a -b, c +a -b, c +a +b, c -a +b
			Vec axis{0, 0, 0};
			bool axial = false;
			switch (d.mode) {
			case 6: axis = row(p.orientation, 2); axial = true; break;
			case 16: axis = row(p.orientation, 0); axial = true; break;
			case 17: axis = row(p.orientation, 1); axial = true; break;
			case 7: axis = row(at.r, 2); axial = true; break;
			case 9: axis = row(at.r, 0); axial = true; break;
			case 11: axis = row(at.r, 1); axial = true; break;
			default: break;
			}
			if (axial) {
				// A quad along the axis, turned to face the camera.
				if ((d.mode == 6 || d.mode == 16 || d.mode == 17) && local) axis = at.rotate(axis);
				axis = norm(axis);
				Vec side = norm(cross(norm(sub(c, cam)), axis));
				a = mul(side, hw);
				bb = mul(axis, hh);
			} else if (d.mode >= 1 && d.mode <= 5) {
				// Streaks from the particle along a direction: towards the actor (1), its momentum
				// (2/3), the actor's momentum (4), scaled by +0x6c.
				Vec dir = d.mode == 1 ? sub(Vec{at.t[0], at.t[1], at.t[2]}, c) : p.momentum;
				if (local && d.mode != 1) dir = at.rotate(dir);
				if (d.normalize) dir = norm(dir);
				Vec end = add(c, mul(dir, d.depth_offset));
				Vec mid = mul(add(c, end), 0.5f);
				Vec along = mul(sub(end, c), 0.5f);
				if (len(along) < 1e-4f) along = mul(up, 1e-4f);
				Vec side = norm(cross(norm(sub(mid, cam)), norm(along)));
				c = mid;
				a = mul(side, hw);
				bb = add(along, mul(norm(along), hh));
			} else {
				// Screen-aligned square (0), rotated by the rotation channel (13).
				Vec r = right, u = up;
				if (d.mode == 13) {
					float ang = p.rotation * 6.2831853f, cs = std::cos(ang), sn = std::sin(ang);
					r = add(mul(right, cs), mul(up, sn));
					u = sub(mul(up, cs), mul(right, sn));
				}
				c = add(c, add(mul(r, d.offset[0] * hw), mul(u, d.offset[1] * hh)));
				a = mul(r, hw);
				bb = mul(u, hh);
			}
			Vec q[4] = {sub(sub(c, a), bb), sub(add(c, a), bb), add(add(c, a), bb), add(sub(c, a), bb)};
			float uv[4][2] = {{u0, v0 + dv}, {u0 + du, v0 + dv}, {u0 + du, v0}, {u0, v0}};
			for (int k : {0, 1, 2, 0, 2, 3}) {
				Vertex v;
				for (int j = 0; j < 3; ++j) v.pos[j] = q[k][size_t(j)];
				v.uv[0] = uv[k][0];
				v.uv[1] = uv[k][1];
				for (int j = 0; j < 4; ++j) v.rgba[j] = col[size_t(j)];
				b.v.push_back(v);
			}
		}
	}
	// Hit sparks (DE_CSparks::DE_CInstanceInfo::Process): a kite per live spark in view space from
	// its head p to its tail p - momentum x 220, its tip pushed `width` past the head and its back
	// `width` towards it, `width` to either side across the screen; colour (1, life, 0.75 life)
	// at 1 - (1 - life)^2, added like blend-0 particles (premultiplied).
	if (!glow_layer) {
		for (const game::SparkBurst &sb : world.particles->sparks()) {
			if (!sb.setup || !sb.image) continue;
			Batch &b = batches[{image(sb.image), true}];
			b.tex = image(sb.image);
			b.additive = true;
			for (const auto &sp : sb.sparks) {
				if (sp.life <= 0) continue;
				Vec tail = sub(sp.pos, mul(sp.vel, 220));
				Vec d = sub(tail, sp.pos);
				float dx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
				float dy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
				float l = std::sqrt(dx * dx + dy * dy);
				if (l == 0) l = 1e-6f;
				float ax = sp.width * dx / l, ay = sp.width * dy / l;
				auto screen = [&](const Vec &o, float x, float y) { return add(o, add(mul(right, x), mul(up, y))); };
				Vec q[4] = {screen(sp.pos, ay, -ax), screen(sp.pos, -ax, -ay), screen(sp.pos, -ay, ax), screen(tail, ax, ay)};
				float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
				float a = 1 - (1 - sp.life) * (1 - sp.life);
				std::array<float, 4> col{a, sp.life * a, 0.75f * sp.life * a, 1};
				for (int k : {0, 1, 2, 0, 2, 3}) {
					Vertex v;
					for (int j = 0; j < 3; ++j) v.pos[j] = q[k][size_t(j)];
					v.uv[0] = uv[k][0];
					v.uv[1] = uv[k][1];
					for (int j = 0; j < 4; ++j) v.rgba[j] = col[size_t(j)];
					b.v.push_back(v);
				}
			}
		}
	}
	// Attack trailers: a ribbon through the points (pos +- width), joined where neither end is cut.
	for (auto &[fighter, set] : world.particles->trailers()) {
		for (const game::AttackTrailer &t : set) {
			if (t.points.size() < 2) continue;
			Batch &b = batches[{image(t.image), true}];
			b.tex = image(t.image);
			b.additive = true;
			float ua = t.reverse_uv ? 0.0f : 1.0f, ub = 1 - ua;
			for (size_t i = 1; i < t.points.size(); ++i) {
				const auto &p0 = t.points[i - 1], &p1 = t.points[i];
				if (p0.cut || p1.cut || p1.life < 0) continue;
				Vec q[4] = {add(p0.pos, p0.width), sub(p0.pos, p0.width), sub(p1.pos, p1.width), add(p1.pos, p1.width)};
				float uv[4][2] = {{ua, 1 - p0.life}, {ub, 1 - p0.life}, {ub, 1 - p1.life}, {ua, 1 - p1.life}};
				const std::array<float, 4> *cols[4] = {&p0.color, &p0.color, &p1.color, &p1.color};
				for (int k : {0, 1, 2, 0, 2, 3}) {
					Vertex v;
					for (int j = 0; j < 3; ++j) v.pos[j] = q[k][size_t(j)];
					v.uv[0] = uv[k][0];
					v.uv[1] = uv[k][1];
					for (int j = 0; j < 3; ++j) v.rgba[j] = (*cols[k])[size_t(j)];
					v.rgba[3] = 1;
					b.v.push_back(v);
				}
			}
		}
	}
	if (batches.empty()) return;
	glUseProgram(prog_);
	glUniformMatrix4fv(glGetUniformLocation(prog_, "vp"), 1, GL_FALSE, vp);
	glUniform1i(glGetUniformLocation(prog_, "tex"), 0);
	glBindVertexArray(vao_);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glEnable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glActiveTexture(GL_TEXTURE0);
	// Alpha-blended effects first, the additive ones over them.
	for (bool pass_additive : {false, true}) {
		for (auto &[key, b] : batches) {
			if (b.additive != pass_additive || b.v.empty()) continue;
			glBlendFunc(b.additive ? GL_ONE : GL_SRC_ALPHA, b.additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
			glUniform1i(glGetUniformLocation(prog_, "additive"), b.additive ? 1 : 0);
			glBindTexture(GL_TEXTURE_2D, b.tex);
			glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(b.v.size() * sizeof(Vertex)), b.v.data(), GL_STREAM_DRAW);
			glDrawArrays(GL_TRIANGLES, 0, GLsizei(b.v.size()));
		}
	}
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

}  // namespace render
