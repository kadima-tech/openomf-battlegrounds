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
float dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec &a, const Vec &b) {
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec row(const std::array<float, 9> &r, int i) { return {r[size_t(i * 3)], r[size_t(i * 3 + 1)], r[size_t(i * 3 + 2)]}; }

// The streams' screen space. The original projects each point (x, y over a depth clamped from
// below) and offsets it in pixels; ours works on the projection plane (x / depth, y / depth),
// which is the same picture for square pixels, and puts each vertex back in world space at its
// point's depth.
struct Screen {
	const float *v;  // the view matrix, column-major
	float depth(const Vec &w) const { return -(v[2] * w[0] + v[6] * w[1] + v[10] * w[2] + v[14]); }
	// {x / depth, y / depth, depth}, depth at least `near`.
	Vec project(const Vec &w, float near) const {
		float ex = v[0] * w[0] + v[4] * w[1] + v[8] * w[2] + v[12];
		float ey = v[1] * w[0] + v[5] * w[1] + v[9] * w[2] + v[13];
		float d = std::max(depth(w), near);
		return {ex / d, ey / d, d};
	}
	Vec unproject(float qx, float qy, float d) const {
		const float e[3] = {qx * d - v[12], qy * d - v[13], -d - v[14]};
		Vec w;
		for (int j = 0; j < 3; ++j) w[size_t(j)] = e[0] * v[4 * j] + e[1] * v[4 * j + 1] + e[2] * v[4 * j + 2];
		return w;
	}
};

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

// DE_CRingParticleRenderer::DE_CInstanceInfo::Process (0x40015c00): per live particle, an annulus
// of 20 segments, vertex = pos + sin(a) r A + cos(a) r B + h N in the plane mode's frame (A, B, N),
// from the inner edge (radius and height curves at the particle's age, times its size; inner
// colour) to the outer edge. U runs detail x (rotation + i / 20) round the ring, V 0 inner to 1 outer.
void ParticleRenderer::ring(const game::ParticleInstance &in, const std::array<float, 3> &right, const std::array<float, 3> &up,
                            std::map<std::pair<GLuint, bool>, Batch> &batches) {
	const de::ParticleRendererDef &d = *in.def;
	if (d.ring_plane < 0 || d.ring_plane > 3) return;
	de::Transform at = in.actor->world();
	const bool local = d.generator.local, oriented = d.generator.channels[de::kChOrientation];
	const bool additive = d.blend != 2;
	Batch &b = batches[{image(d.image), additive}];
	b.tex = image(d.image);
	b.additive = additive;
	constexpr int kSegments = 20;
	auto neg = [](const Vec &v) { return Vec{-v[0], -v[1], -v[2]}; };
	for (const game::Particle &p : in.particles) {
		Vec c = local ? at.apply(p.pos) : p.pos;
		Vec A, B, N{0, 0, 0};
		if (d.ring_plane == 3) {
			A = right, B = up;
		} else {
			// Identity in the mesh's space (RotateX / RotateY of it), or the particle's orientation
			// turned by RotateLocalX / RotateLocalY.
			Vec x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
			if (oriented) x = row(p.orientation, 0), y = row(p.orientation, 1), z = row(p.orientation, 2);
			if (d.ring_plane == 0) A = x, B = y, N = z;
			else if (d.ring_plane == 1) A = x, B = oriented ? neg(z) : z, N = oriented ? y : neg(y);
			else A = oriented ? z : neg(z), B = y, N = oriented ? neg(x) : x;
			if (local) A = at.rotate(A), B = at.rotate(B), N = at.rotate(N);
		}
		float r0 = d.inner_radius.eval(p.age) * p.size, r1 = d.outer_radius.eval(p.age) * p.size;
		float h0 = 0, h1 = 0;
		if (d.ring_plane != 3) h0 = d.inner_height.eval(p.age) * p.size, h1 = d.outer_height.eval(p.age) * p.size;
		auto c0 = d.inner_color.eval(p.age), c1 = d.outer_color.eval(p.age);
		if (additive) {
			c0 = {c0[0] * c0[3], c0[1] * c0[3], c0[2] * c0[3], 1};
			c1 = {c1[0] * c1[3], c1[1] * c1[3], c1[2] * c1[3], 1};
		}
		const float rot = d.generator.channels[de::kChRotation] ? p.rotation : 0;
		auto at_angle = [&](int i, float radius, float height) {
			float a = float(i) * 0.05f * 6.2831855f;
			return add(add(c, mul(N, height)), add(mul(A, std::sin(a) * radius), mul(B, std::cos(a) * radius)));
		};
		for (int i = 0; i < kSegments; ++i) {
			// Vertices 2i (inner) and 2i + 1 (outer); triangles (v, v+2, v+3) and (v, v+3, v+1).
			Vec q[4] = {at_angle(i, r0, h0), at_angle(i, r1, h1), at_angle(i + 1, r0, h0), at_angle(i + 1, r1, h1)};
			float u0 = d.ring_detail * (rot + float(i) / kSegments), u1 = d.ring_detail * (rot + float(i + 1) / kSegments);
			float uv[4][2] = {{u0, 0}, {u0, 1}, {u1, 0}, {u1, 1}};
			const std::array<float, 4> *cols[4] = {&c0, &c1, &c0, &c1};
			for (int k : {0, 2, 3, 0, 3, 1}) {
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

// DE_CStreamParticleRenderer::DE_CInstanceInfo::Process (0x400172b0): a ribbon through the live
// particles in order, built in screen space. Each point is projected (depth at least 2); each
// segment's offset is perpendicular on screen to the sum of its own and the previous segment's
// screen direction, size (of its first particle) x width (+0x6c) / the depth of its second; a
// joint takes the average of its two segments' vertices. A segment from a particle to the next
// is left out where the renderer breaks streams (+0x80): at a particle without flag 2 (a
// lightning bolt's last point), or, with +0x84, where the two are more than +0x84 ms of age
// apart. Texture modes 0-2 draw each segment as its own quad (the geometry is the same).
void ParticleRenderer::stream(const game::World &world, const game::ParticleInstance &in, const float *view,
                              std::map<std::pair<GLuint, bool>, Batch> &batches) {
	const de::ParticleRendererDef &d = *in.def;
	de::Transform at = in.actor->world();
	const bool local = d.generator.local;
	const bool additive = d.blend != 2;
	Batch &b = batches[{image(d.image), additive}];
	b.tex = image(d.image);
	b.additive = additive;
	const Screen screen{view};
	const float now = world.scene_ms();
	std::array<float, 4> base = d.color.rgba;
	const std::array<float, 4> *set_rgb = world.particles->value_color(d.color_ref);
	if (set_rgb) base = {(*set_rgb)[0], (*set_rgb)[1], (*set_rgb)[2], (*set_rgb)[3] >= 0 ? (*set_rgb)[3] : base[3]};
	const auto &ps = in.particles;
	const size_t n = ps.size();
	auto pos = [&](size_t i) { return local ? at.apply(ps[i].pos) : ps[i].pos; };
	auto connected = [&](size_t i) {
		if (i + 1 >= n) return false;
		if (!d.stream_breaks) return true;
		if (d.stream_max_gap != 0) {
			float life = ps[i].inv_life != 0 ? 1 / ps[i].inv_life : 0;
			return (ps[i].age - ps[i + 1].age) * life <= d.stream_max_gap;
		}
		return (ps[i].flags & 2) != 0;
	};
	auto colour = [&](const game::Particle &p) {
		std::array<float, 4> c = base;
		float age = std::clamp(p.age, 0.0f, 1.0f);
		switch (d.stream_color_mode) {
		case 1: for (auto &x : c) x *= age; break;
		case 2: for (auto &x : c) x *= 1 - age; break;
		case 4:
			if (!set_rgb) c = d.color.eval(age);
			c = {c[0] * c[3], c[1] * c[3], c[2] * c[3], 1};
			break;
		case 5: c = {c[0] * c[3] * p.intensity, c[1] * c[3] * p.intensity, c[2] * c[3] * p.intensity, 1}; break;
		default: break;  // 0 and 3: constant
		}
		if (additive) c = {c[0] * c[3], c[1] * c[3], c[2] * c[3], 1};
		return c;
	};
	size_t i = 0;
	while (i < n) {
		// A run of connected particles: first .. last.
		size_t first = i;
		while (connected(i)) ++i;
		size_t last = i++;
		if (last == first) continue;
		const size_t m = last - first + 1;
		std::vector<Vec> q(m);
		for (size_t k = 0; k < m; ++k) q[k] = screen.project(pos(first + k), 2);
		// Per joint the summed offsets of the segments meeting there, and how many.
		std::vector<std::array<float, 2>> off(m, {0, 0});
		std::vector<int> count(m, 0);
		float px = 0, py = 0;
		for (size_t k = 0; k + 1 < m; ++k) {
			float dx = q[k + 1][0] - q[k][0], dy = q[k + 1][1] - q[k][1];
			float tx = px + dx, ty = py + dy;
			px = dx;
			py = dy;
			float l = std::sqrt(tx * tx + ty * ty) + 1e-6f;
			float hw = ps[first + k].size * d.stream_width / q[k + 1][2];
			float ox = -ty / l * hw, oy = tx / l * hw;
			for (size_t j : {k, k + 1}) {
				off[j][0] += ox;
				off[j][1] += oy;
				++count[j];
			}
		}
		std::vector<Vec> left(m), right(m);
		for (size_t k = 0; k < m; ++k) {
			float ox = off[k][0] / float(count[k]), oy = off[k][1] / float(count[k]);
			left[k] = screen.unproject(q[k][0] - ox, q[k][1] - oy, q[k][2]);
			right[k] = screen.unproject(q[k][0] + ox, q[k][1] + oy, q[k][2]);
		}
		for (size_t k = first; k < last; ++k) {
			const game::Particle &p0 = ps[k], &p1 = ps[k + 1];
			float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
			if (d.stream_texture_mode == 2) {
				int frame = p0.serial & 15;
				u0 = float(frame & 3) * 0.25f;
				v0 = float(frame >> 2 & 3) * 0.25f;
				u1 = u0 + 0.25f;
				v1 = v0 + 0.25f;
			} else if (d.stream_texture_mode == 4) {
				v0 = p0.age * d.stream_uv_scale + now * d.stream_uv_scroll;
				v1 = p1.age * d.stream_uv_scale + now * d.stream_uv_scroll;
			}
			auto c0 = colour(p0), c1 = colour(p1);
			size_t a = k - first;
			Vec quad[4] = {left[a], right[a], right[a + 1], left[a + 1]};
			float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
			const std::array<float, 4> *cols[4] = {&c0, &c0, &c1, &c1};
			for (int t : {0, 1, 2, 0, 2, 3}) {
				Vertex v;
				for (int j = 0; j < 3; ++j) v.pos[j] = quad[t][size_t(j)];
				v.uv[0] = uv[t][0];
				v.uv[1] = uv[t][1];
				for (int j = 0; j < 4; ++j) v.rgba[j] = (*cols[t])[size_t(j)];
				b.v.push_back(v);
			}
		}
	}
}

// DE_CFourFaceTriangleParticleRenderer::DE_CInstanceInfo::Process (0x400145d0): per particle its
// triangle as three faces round a centre vertex at the particle (faces 0-3-2, 0-1-3, 1-2-3). The
// centre takes the colour at the particle's age; the corners are black (additive blends) or the
// colour with no alpha (alpha blend), so each puff fades out to its edges. In mode 0 the corners
// are in the particle's frame (the actor's for a local generator); modes 1-3 turn them to face
// the camera about its X, Y or Z axis.
void ParticleRenderer::triangles(const game::World &world, const game::ParticleInstance &in, const float *view,
                                 const std::array<float, 3> &cam, std::map<std::pair<GLuint, bool>, Batch> &batches) {
	const de::ParticleRendererDef &d = *in.def;
	de::Transform at = in.actor->world();
	const bool local = d.generator.local;
	const bool additive = d.blend != 2;
	Batch &b = batches[{image(d.image), additive}];
	b.tex = image(d.image);
	b.additive = additive;
	const std::array<float, 4> *set_rgb = world.particles->value_color(d.color_ref);
	const Vec cam_axis[3] = {{view[0], view[4], view[8]}, {view[1], view[5], view[9]}, {view[2], view[6], view[10]}};
	for (const game::Particle &p : in.particles) {
		Vec c = local ? at.apply(p.pos) : p.pos;
		float age = std::clamp(p.age, 0.0f, 1.0f);
		std::array<float, 4> col = set_rgb ? *set_rgb : d.color.eval(age);
		if (set_rgb && col[3] < 0) col[3] = d.color.eval(age)[3];
		std::array<float, 4> edge = {col[0], col[1], col[2], 0};
		if (additive) {
			col = {col[0] * col[3], col[1] * col[3], col[2] * col[3], 1};
			edge = {0, 0, 0, 1};
		}
		Vec rx{1, 0, 0}, ry{0, 1, 0}, rz{0, 0, 1};
		if (d.tri_facing >= 1 && d.tri_facing <= 3) {
			Vec dir = norm(sub(c, cam));
			Vec a = cam_axis[d.tri_facing - 1];
			rx = norm(cross(dir, a));
			ry = a;
			rz = cross(rx, cam_axis[1]);
		} else if (local) {
			rx = row(at.r, 0);
			ry = row(at.r, 1);
			rz = row(at.r, 2);
		}
		Vec q[4];
		for (int k = 0; k < 3; ++k) {
			const float *v = &p.tri[size_t(k * 3)];
			q[k] = add(c, add(add(mul(rx, v[0]), mul(ry, v[1])), mul(rz, v[2])));
		}
		q[3] = c;
		for (int k : {0, 3, 2, 0, 1, 3, 1, 2, 3}) {
			Vertex v;
			for (int j = 0; j < 3; ++j) v.pos[j] = q[k][size_t(j)];
			v.uv[0] = p.tri_uv[size_t(k * 2)];
			v.uv[1] = p.tri_uv[size_t(k * 2 + 1)];
			const auto &cc = k == 3 ? col : edge;
			for (int j = 0; j < 4; ++j) v.rgba[j] = cc[size_t(j)];
			b.v.push_back(v);
		}
	}
}

// DE_CPointStreamer::DE_CInstanceInfo::Process (Core 0x701d17f0): a ribbon from the oldest point to
// the actor, half as wide as the start width + (end - start) x age / life, alpha likewise (the
// oldest point transparent, the actor's end the start width and alpha); texture v the points'
// time / texture length, u across. Mode 0/1 colour grey x alpha, mode 2 white with alpha; the
// blend is the particle renderers' (material 8 / 4 / 0x40: additive, additive, alpha). Built in
// screen space: each point's offset is perpendicular on screen to the way to the next point (the
// actor's to the way from the last), its half width / the point's depth; where either point is
// within depth 1 of the camera the point has no width and no alpha.
void ParticleRenderer::trail(const game::World &world, const game::Trail &t, const float *view,
                             std::map<std::pair<GLuint, bool>, Batch> &batches) {
	const de::PointStreamerDef &d = *t.def;
	if (t.points.empty()) return;
	const bool additive = d.mode != 2;
	Batch &b = batches[{image(d.image), additive}];
	b.tex = image(d.image);
	b.additive = additive;
	const Screen screen{view};
	const float now = world.scene_ms();
	const float base_v = d.texture_length != 0 ? std::floor(now / d.texture_length) : 0;
	struct P {
		Vec pos;
		float half, alpha, v;
	};
	std::vector<P> pts;
	for (size_t i = 0; i < t.points.size(); ++i) {
		const auto &p = t.points[i];
		float age = now - p.time;
		float k = d.life != 0 ? age / d.life : 0;
		pts.push_back({p.pos, d.width0 + (d.width1 - d.width0) * k, i == 0 ? 0 : d.alpha0 + (d.alpha1 - d.alpha0) * k, p.v - base_v});
	}
	pts.push_back({t.actor->world().t, d.width0, d.alpha0, d.texture_length != 0 ? now / d.texture_length - base_v : 0});
	const size_t n = pts.size();
	if (n < 2) return;
	std::vector<Vec> q(n);
	std::vector<bool> near(n);
	for (size_t k = 0; k < n; ++k) {
		near[k] = screen.depth(pts[k].pos) <= 1;
		q[k] = screen.project(pts[k].pos, 1);
	}
	std::vector<Vec> left(n), right(n);
	for (size_t k = 0; k < n; ++k) {
		size_t a = k + 1 < n ? k : k - 1, z = a + 1;
		float ox = 0, oy = 0;
		if (!near[a] && !near[z]) {
			float dx = q[z][0] - q[a][0], dy = q[z][1] - q[a][1];
			float l = std::sqrt(dx * dx + dy * dy);
			float hw = pts[k].half / q[k][2];
			if (l > 0) {
				ox = -dy / l * hw;
				oy = dx / l * hw;
			}
		} else {
			pts[k].alpha = 0;
		}
		left[k] = screen.unproject(q[k][0] - ox, q[k][1] - oy, q[k][2]);
		right[k] = screen.unproject(q[k][0] + ox, q[k][1] + oy, q[k][2]);
	}
	auto colour = [&](float a) {
		a = std::clamp(a, 0.0f, 1.0f);
		return additive ? std::array<float, 4>{a, a, a, 1} : std::array<float, 4>{1, 1, 1, a};
	};
	for (size_t k = 0; k + 1 < n; ++k) {
		auto c0 = colour(pts[k].alpha), c1 = colour(pts[k + 1].alpha);
		Vec quad[4] = {left[k], right[k], right[k + 1], left[k + 1]};
		float uv[4][2] = {{0, pts[k].v}, {1, pts[k].v}, {1, pts[k + 1].v}, {0, pts[k + 1].v}};
		const std::array<float, 4> *cols[4] = {&c0, &c0, &c1, &c1};
		for (int i : {0, 1, 2, 0, 2, 3}) {
			Vertex v;
			for (int j = 0; j < 3; ++j) v.pos[j] = quad[i][size_t(j)];
			v.uv[0] = uv[i][0];
			v.uv[1] = uv[i][1];
			for (int j = 0; j < 4; ++j) v.rgba[j] = (*cols[i])[size_t(j)];
			b.v.push_back(v);
		}
	}
}

void ParticleRenderer::draw(const game::World &world, const float *vp, const float *view, const float *eye, bool glow_layer) {
	if (!prog_) return;
	const Vec right{view[0], view[4], view[8]}, up{view[1], view[5], view[9]};
	const Vec cam{eye[0], eye[1], eye[2]};
	std::map<std::pair<GLuint, bool>, Batch> batches;
	if (!glow_layer) {
		for (auto &t : world.particles->trails()) {
			if (t->stepped == world.frame() && !t->actor->meshes_disabled && (!only || only(t->actor))) trail(world, *t, view, batches);
		}
	}
	for (auto &inp : world.particles->instances()) {
		const game::ParticleInstance &in = *inp;
		if (in.particles.empty() || in.stepped + 1 < world.frame() || in.actor->meshes_disabled || (only && !only(in.actor))) continue;
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
		if (d.kind == "StreamParticleRenderer") {
			if (glow_layer) continue;
			stream(world, in, view, batches);
			continue;
		}
		if (d.kind == "FourFaceTriangle") {
			if (glow_layer) continue;
			triangles(world, in, view, cam, batches);
			continue;
		}
		if (d.kind != "BillboardedQuad") continue;
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
		// Cone modes 1 and 2 (InstanceInfo::Process): the actor's Z axis against the view fades or
		// kills the renderer's colour; their width / height scaling never reaches the geometry.
		const Vec az = row(at.r, 2), fwd{-view[2], -view[6], -view[10]};
		if (d.cone_mode == 1) {
			Vec v = norm(sub(Vec{at.t[0], at.t[1], at.t[2]}, cam));
			float f = (1 - d.glow.eval(0)) * dot(v, norm(az)) + 1;
			bool kill = f > 1;
			if (!kill && in.actor->shape == game::GameObject::Shape::Cone) {
				float s1 = std::sin(in.actor->umbra * 0.017453292f), s2 = std::sin(in.actor->penumbra * 0.017453292f);
				if (f >= s2) kill = true;
				else if (f >= s1) base[3] *= (s2 - f) / (s2 - s1);
			}
			if (kill) base = {0, 0, 0, 0};
		} else if (d.cone_mode == 2) {
			float zc = std::fabs(dot(norm(az), fwd));
			if (zc > 0.9f) base[3] *= 1 - (zc - 0.9f) * 10;
		}
		// The quad extents (+0x38 height, +0x3c width, times the actor's scale); modes 6..23 use the
		// bare size unless width or height differs from 1.
		const bool use_wh = d.width != 1 || d.height != 1;
		auto camera_depth = [&](const Vec &w) { return -(view[2] * w[0] + view[6] * w[1] + view[10] * w[2] + view[14]); };
		for (const game::Particle &p : in.particles) {
			if (d.mode == 8 || d.mode == 10 || d.mode == 12 || d.mode < 0 || d.mode > 23) break;
			Vec c = local ? at.apply(p.pos) : p.pos;
			const bool camera_space = d.mode <= 5 || d.mode == 13;
			const float pz = camera_depth(c);
			if (camera_space && pz <= d.near_limit) continue;
			// Cone mode 3: the glow value at the particle's age replaces the actor's scale.
			float ex_h = d.height * scale, ex_w = d.width * scale;
			if (d.cone_mode == 3) {
				float g = d.glow.eval(p.age);
				ex_h = g * d.height, ex_w = g * d.width;
			}
			float hw = ex_w * p.size, hh = ex_h * p.size;
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
			// Texture coordinates (+0x8c): the whole image, or a frame of a 4x4 sheet (mode 1: age x
			// 16 unclamped, so age 1 wraps to frame 0).
			float u0 = 0, v0 = 0, du = 1, dv = 1;
			if (d.uv_mode == 1 || d.uv_mode == 2) {
				int frame = d.uv_mode == 1 ? int(age * 16) : (p.serial & 15);
				u0 = float(frame & 3) * 0.25f;
				v0 = float(frame >> 2 & 3) * 0.25f;
				du = dv = 0.25f;
			}
			// Corners (SetParticleVerticies, the switch on +0x88), in the order v0..v3.
			Vec q[4];
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
			auto quad = [&](const Vec &o, const Vec &a, const Vec &bb) {
				q[0] = sub(sub(o, a), bb), q[1] = sub(add(o, a), bb), q[2] = add(add(o, a), bb), q[3] = add(sub(o, a), bb);
			};
			if (axial) {
				// A quad along the axis, turned to face the camera.
				if ((d.mode == 6 || d.mode == 16 || d.mode == 17) && local) axis = at.rotate(axis);
				float sw = use_wh ? hw : p.size, sh = use_wh ? hh : p.size;
				if (use_wh) axis = norm(axis);
				Vec side = norm(cross(norm(sub(c, cam)), norm(axis)));
				quad(c, mul(side, sw), mul(axis, sh));
			} else if (d.mode >= 1 && d.mode <= 5) {
				// A kite in screen space at the particle's depth: its sides at the particle, its tip
				// half a width past the end point (+0x6c along the direction), its back half a width
				// behind; half-width size x height x the actor's scale.
				if (d.mode == 2) continue;  // the original leaves the direction unset
				Vec dir = d.mode == 1 ? sub(Vec{at.t[0], at.t[1], at.t[2]}, c)
				        : d.mode == 3 ? p.momentum
				        : d.mode == 4 ? Vec{in.actor->momentum[0], in.actor->momentum[1], in.actor->momentum[2]}
				                      : az;
				if (d.normalize) dir = norm(dir);
				Vec e = add(c, mul(dir, d.streak_length));
				float ez = camera_depth(e);
				if (ez < 5) continue;
				// The end point's screen offset, taken back to world units at the particle's depth.
				Vec pe = sub(e, cam), pc = sub(c, cam);
				float dx = (dot(pe, right) / ez - dot(pc, right) / pz) * pz, dy = (dot(pe, up) / ez - dot(pc, up) / pz) * pz;
				float l = std::sqrt(dx * dx + dy * dy);
				if (l == 0) l = 1e-6f;
				float nx = dx / l, ny = dy / l, w = hh;
				auto screen = [&](const Vec &o, float x, float y) { return add(o, add(mul(right, x), mul(up, y))); };
				q[0] = screen(c, w * ny, -w * nx);
				q[1] = screen(c, dx + w * nx, dy + w * ny);
				q[2] = screen(c, -w * ny, w * nx);
				q[3] = screen(c, -w * nx, -w * ny);
			} else if (d.mode == 14 || d.mode == 15 || (d.mode >= 18 && d.mode <= 23)) {
				// Quads in the generator's space (the actor's for a local one) from the raw position.
				const bool own = d.mode >= 21;
				if (own && !d.generator.channels[de::kChOrientation]) continue;
				float ea = (use_wh || own) ? ex_h * p.size : p.size, eb = (use_wh || own) ? ex_w * p.size : p.size;
				Vec P = p.pos;
				const float ang = d.generator.channels[de::kChRotation] && !own ? p.rotation * 6.2831850f : 0;
				const float cs = std::cos(ang), sn = std::sin(ang);
				const float ox = d.offset[0], oy = d.offset[1];
				auto to_world = [&](const Vec &v) { return local ? at.apply(v) : v; };
				if (d.mode == 15) {
					Vec u{0, ea, 0}, v{eb, 0, 0};
					if (ox != 0) P = add(P, mul(u, ox));
					if (oy != 0) P = add(P, mul(v, oy));
					u = {-sn * ea, cs * ea, 0}, v = {cs * eb, sn * eb, 0};
					if (!local) u = at.rotate(u), v = at.rotate(v);
					Vec corner[4] = {sub(add(P, u), v), add(add(P, u), v), add(sub(P, u), v), sub(sub(P, u), v)};
					for (int k = 0; k < 4; ++k) q[k] = to_world(corner[k]);
				} else if (own) {
					const auto &o = p.orientation;
					auto rot = [&](float x, float y, float z) { return add(add(mul(row(o, 0), x), mul(row(o, 1), y)), mul(row(o, 2), z)); };
					Vec corner[4];
					if (d.mode == 21) {
						if (ox != 0) P[0] += ea * ox;
						if (oy != 0) P[1] += eb * oy;
						Vec r[4] = {rot(ea, -eb, 0), rot(ea, eb, 0), rot(-ea, eb, 0), rot(-ea, -eb, 0)};
						for (int k = 0; k < 4; ++k) corner[k] = add(P, r[k]);
					} else if (d.mode == 22) {
						if (ox != 0) P[0] += ea * ox;
						if (oy != 0) P[2] += eb * oy;
						Vec r[4] = {rot(ea, 0, -eb), rot(ea, 0, eb), rot(-ea, 0, eb), rot(-ea, 0, -eb)};
						for (int k = 0; k < 4; ++k) corner[k] = add(P, r[k]);
					} else {
						if (ox != 0) P[1] += ea * ox;
						if (oy != 0) P[2] += eb * oy;
						Vec r[4] = {rot(0, ea, -eb), rot(0, ea, eb), rot(0, -ea, eb), rot(0, -ea, -eb)};
						for (int k = 0; k < 4; ++k) corner[k] = add(P, r[k]);
					}
					for (int k = 0; k < 4; ++k) q[k] = to_world(corner[k]);
				} else {
					// (a, b, 0) turned about Z by the rotation channel; XY (14, 18), XZ (19), YZ (20, whose
					// fourth corner keeps the original's y = a').
					float a2 = ea * cs - eb * sn, b2 = ea * sn + eb * cs;
					if (ox != 0) P[0] += a2 * ox;
					if (oy != 0) P[1] += b2 * oy;
					float x = P[0], y = P[1], z = P[2];
					Vec corner[4];
					if (d.mode == 19) {
						corner[0] = {x - a2, y, z - b2}, corner[1] = {x + a2, y, z - b2}, corner[2] = {x + a2, y, z + b2}, corner[3] = {x - a2, y, z + b2};
					} else if (d.mode == 20) {
						corner[0] = {x, y - a2, z - b2}, corner[1] = {x, y + a2, z - b2}, corner[2] = {x, y + a2, z + b2}, corner[3] = {x, a2, z + b2};
					} else {
						corner[0] = {x - a2, y - b2, z}, corner[1] = {x + a2, y - b2, z}, corner[2] = {x + a2, y + b2, z}, corner[3] = {x - a2, y + b2, z};
					}
					for (int k = 0; k < 4; ++k) q[k] = to_world(corner[k]);
				}
			} else {
				// Screen-aligned square (0), rotated by the rotation channel (13).
				Vec r = right, u = up;
				if (d.mode == 13) {
					float ang = p.rotation * 6.2831853f, cs = std::cos(ang), sn = std::sin(ang);
					r = add(mul(right, cs), mul(up, sn));
					u = sub(mul(up, cs), mul(right, sn));
				}
				quad(add(c, add(mul(r, d.offset[0] * hw), mul(u, d.offset[1] * hh))), mul(r, hw), mul(u, hh));
				// The z offset (+0x80) moves only the depth: along the view ray, keeping the footprint.
				if (d.offset[2] != 0) {
					float k = std::max(pz + d.offset[2], d.near_limit) / pz;
					for (Vec &v : q) v = add(cam, mul(sub(v, cam), k));
				}
			}
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
			float ua = t.reverse_uv ? 1.0f : 0.0f, ub = 1 - ua;
			for (size_t i = 1; i < t.points.size(); ++i) {
				const auto &p0 = t.points[i - 1], &p1 = t.points[i];
				if (p0.cut || p1.cut || p1.life < 0) continue;
				Vec q[4] = {add(p0.pos, p0.width), sub(p0.pos, p0.width), sub(p1.pos, p1.width), add(p1.pos, p1.width)};
				float uv[4][2] = {{ua, p0.v}, {ub, p0.v}, {ub, p1.v}, {ua, p1.v}};
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
