#include "shadows.h"

#include <algorithm>
#include <cmath>

namespace render {

namespace {

const char *kVS = R"(#version 330 core
layout(location = 0) in vec3 pos;
uniform vec3 cone_pos;
uniform mat3 axes;  // rows: the cone's local X, Y, Z
uniform float half_extent;
void main() {
	vec3 l = axes * (pos - cone_pos);
	// The projector samples -l.xy / (2 l.z tan p) + 0.5: the same mapping, at this extent.
	gl_Position = vec4(-l.x / half_extent, -l.y / half_extent, 0.0, l.z);
}
)";
const char *kFS = R"(#version 330 core
uniform float strength;
out vec4 color;
void main() { color = vec4(strength, strength, strength, 1.0); }
)";

using V3 = std::array<float, 3>;
V3 sub(const V3 &a, const V3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float len(const V3 &a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
V3 scale(const V3 &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
V3 cross(const V3 &a, const V3 &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

// A frame whose +Z is `z` (DE_CActor::PointZ); the roll does not matter to a projection.
de::Transform pointing(const V3 &pos, V3 z) {
	z = scale(z, 1 / std::max(len(z), 1e-6f));
	V3 up = std::fabs(z[1]) < 0.99f ? V3{0, 1, 0} : V3{1, 0, 0};
	V3 x = cross(up, z);
	x = scale(x, 1 / std::max(len(x), 1e-6f));
	V3 y = cross(z, x);
	de::Transform t;
	t.t = pos;
	t.r = {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
	return t;
}

}  // namespace

// DE_CShadowCaster::Update (0x20036540) and CShadow::DoneAddingLights (0x20036aa0).
void ShadowRenderer::update(int type, const std::vector<Caster> &casters, const Lighting &lights, const float *view, float fov) {
	if (type <= 0) {
		shadows_.clear();
		return;
	}
	if (!prog_) prog_ = glu::program(kVS, kFS);
	shadows_.resize(casters.size() * 2);
	for (size_t ci = 0; ci < casters.size(); ++ci) {
		const Caster &c = casters[ci];
		const float r = std::max(c.radius, 1e-3f);
		float depth = view[2] * c.centre[0] + view[6] * c.centre[1] + view[10] * c.centre[2] + view[14];
		depth = -depth;  // camera-space distance in front of the eye
		// Low (1) and Medium (2): one shadow at 128 px; High (3): two at 256.
		const int base = type == 3 ? 0 : 1, used = type == 3 ? 2 : 1;
		V3 P[2] = {{0, 0, 0}, {0, 0, 0}};
		float I[2] = {0, 0}, total = 0;
		for (auto it = lights.lights.rbegin(); it != lights.lights.rend(); ++it) {
			const SceneLight &l = *it;
			if (!(l.mask & 2) || l.blend == 2 || !l.lights(c.owner)) continue;
			float w = l.affector_intensity(c.centre) * l.intensity;
			if (w <= 0) continue;
			int k = l.position_static ? 0 : 1;
			if (k == 1 && type != 3) continue;
			P[k] = {P[k][0] + w * l.pos[0], P[k][1] + w * l.pos[1], P[k][2] + w * l.pos[2]};
			I[k] += w;
			total += w;
		}
		for (int k = 0; k < 2; ++k) {
			Shadow &s = shadows_[ci * 2 + size_t(k)];
			s.owner = c.owner;
			s.on = false;
			if (k >= used || I[k] <= 0) continue;
			// AdjustShadowMapQuality (0x20036920): smaller maps further from the camera.
			float q = std::max(0.0f, 1 - 200 / (std::tan(fov * 3.14159265f / 360) * std::max(depth, 1.0f)));
			float f = std::min(float(base) + 5 * q, 5.5f);
			if (std::fabs(f - (float(s.level) + 0.5f)) > 0.8f) {
				s.level = int(f);
				s.size = 256 >> s.level;
				s.timer = 1;
			}
			float strength = I[k];
			if (strength > 0.3f) strength = std::max(0.3f, strength * (0.5f + 0.5f * strength / total));
			SceneLight &cone = s.cone;
			cone = SceneLight{};
			cone.shape = SceneLight::kCone;
			cone.blend = 2;
			cone.mask = 0x10;
			cone.colour = {1, 1, 1};
			cone.intensity = 1;
			cone.name = "shadow";
			float L;
			if (type == 1) {
				// Straight above, 4r up, the cone looking down.
				L = 4 * r;
				cone.frame = pointing({c.centre[0], c.centre[1] + L, c.centre[2]}, {0, -1, 0});
			} else {
				V3 p = scale(P[k], 1 / I[k]);
				V3 d = sub(p, c.centre);
				L = len(d);
				if (L == 0) continue;
				strength = std::min(strength, 0.6f);
				float clamped = std::clamp(L, 3 * r, 6 * r);
				p = {c.centre[0] + d[0] / L * clamped, c.centre[1] + d[1] / L * clamped, c.centre[2] + d[2] / L * clamped};
				L = clamped;
				cone.frame = pointing(p, sub(c.centre, p));
			}
			cone.pos = cone.frame.t;
			cone.penumbra = std::atan(r / L) * 57.2958f;
			cone.near_begin = L - r;
			cone.outer = 2 * L + 4 * r;
			s.on = true;
			// The map refreshes every frame near the camera, less often far away, never behind it.
			s.timer += std::clamp(250 / std::max(depth, 1.0f), 0.0f, 1.0f) * (depth < -r ? 0.0f : 3.0f);
			if (s.timer >= 1) {
				s.timer = 0;
				// UpdateShadowMap (0x20036220): the camera's FOV is twice the penumbra and its
				// projection's half extent tan(fov) / 2, a little wider than the projector's.
				render_map(s, c, std::clamp(strength, 0.0f, 1.0f), std::tan(2 * cone.penumbra * 0.017453292f) / 2);
			}
			cone.image = s.tex;
			cone.has_image = true;
		}
	}
}

void ShadowRenderer::render_map(Shadow &s, const Caster &c, float strength, float half_extent) {
	if (!c.draw || s.size <= 2) return;
	GLint prev_fbo = 0, prev_vp[4];
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_VIEWPORT, prev_vp);
	if (!s.tex) glGenTextures(1, &s.tex);
	glBindTexture(GL_TEXTURE_2D, s.tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s.size, s.size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	if (!s.fbo) glGenFramebuffers(1, &s.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
	glViewport(0, 0, s.size, s.size);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	// A one-pixel black border: nothing outside the map is shadowed.
	glViewport(1, 1, s.size - 2, s.size - 2);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glUseProgram(prog_);
	glUniform3fv(glGetUniformLocation(prog_, "cone_pos"), 1, s.cone.pos.data());
	glUniformMatrix3fv(glGetUniformLocation(prog_, "axes"), 1, GL_TRUE, s.cone.frame.r.data());
	glUniform1f(glGetUniformLocation(prog_, "half_extent"), half_extent);
	glUniform1f(glGetUniformLocation(prog_, "strength"), strength);
	c.draw(prog_);
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
	glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
	glEnable(GL_DEPTH_TEST);
}

void ShadowRenderer::append(Lighting &lights) const {
	for (const Shadow &s : shadows_) {
		if (s.on && s.tex) lights.lights.push_back(s.cone);
	}
}

}  // namespace render
