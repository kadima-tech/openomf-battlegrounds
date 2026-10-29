#include "glow.h"

#include <cstdlib>

namespace render {

namespace {

// A screen-covering triangle.
const char *kVS = R"(#version 410 core
out vec2 v_uv;
void main() {
	vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	v_uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// SuperSampleForGlow (0x200283b0): five additive passes of the 8-tap average ("Color*=(1.0/8.0)/1.0"),
// the first at radius 0, the other four at `radius` with taps at 45..360 degrees, each saturated.
const char *kSuperFS = R"(#version 410 core
in vec2 v_uv;
uniform sampler2D src;
uniform float radius;
out vec4 color;
vec3 ring(float r) {
	vec3 c = vec3(0.0);
	for (int k = 1; k <= 8; ++k) {
		float a = float(k) * 0.785398;
		c += texture(src, v_uv + r * vec2(sin(a), cos(a))).rgb;
	}
	return clamp(c / 8.0, 0.0, 1.0);
}
void main() {
	color = vec4(clamp(ring(0.0) + 4.0 * ring(radius), 0.0, 1.0), 1.0);
}
)";

// Downsample4x4: a 4x4 block of source texels, "Color*=(1.0/16.0)*.8".
const char *kDownFS = R"(#version 410 core
in vec2 v_uv;
uniform sampler2D src;
uniform vec2 texel;
out vec4 color;
void main() {
	vec3 c = vec3(0.0);
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 4; ++x) c += texture(src, v_uv + (vec2(x, y) - 1.5) * texel).rgb;
	color = vec4(clamp(c / 16.0 * 0.8, 0.0, 1.0), 1.0);
}
)";

// GetMultiPassImageForGlow (8 textures): "Color/=6.0; Color-=.05; saturate".
const char *kCompFS = R"(#version 410 core
in vec2 v_uv;
uniform sampler2D glow, d64, d16;
uniform int high_quality;
out vec4 color;
const vec2 axial[4] = vec2[](vec2(0.005859375, 0.0), vec2(0.0, 0.0078125), vec2(-0.005859375, 0.0), vec2(0.0, -0.0078125));
const vec2 diagonal[4] = vec2[](vec2(-0.0041015623, -0.00546875), vec2(0.0041015623, -0.00546875), vec2(-0.0041015623, 0.00546875), vec2(0.0041015623, 0.00546875));
void main() {
	const float half_texel = 0.5 / 256.0;
	vec3 c = vec3(0.0);
	for (int k = 0; k < 4; ++k) {
		c += texture(glow, v_uv + axial[k] + half_texel).rgb;
		c += texture(glow, v_uv + diagonal[k] + half_texel).rgb;
	}
	vec3 sum = clamp(c / 6.0 - 0.05, 0.0, 1.0);
	if (high_quality != 0) {
		vec3 h = vec3(0.0);
		for (int k = 0; k < 4; ++k) {
			vec2 o = (axial[k] - half_texel) * 2.0;
			h += texture(d64, v_uv + o).rgb;
			h += texture(d16, v_uv + o * 1.5).rgb;
		}
		sum += clamp(h / 6.0 - 0.05, 0.0, 1.0);
	}
	color = vec4(sum, 1.0);
}
)";

}  // namespace

void GlowRenderer::make(Target &t, int size, bool depth) {
	t.size = size;
	glGenTextures(1, &t.tex);
	glBindTexture(GL_TEXTURE_2D, t.tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers(1, &t.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
	if (depth) {
		glGenRenderbuffers(1, &t.depth);
		glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
	}
}

void GlowRenderer::into(const Target &t) {
	glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
	glViewport(0, 0, t.size, t.size);
}

void GlowRenderer::begin() {
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo_);
	glGetIntegerv(GL_VIEWPORT, prev_vp_);
	if (!vao_) {
		make(glow_, 256, true);
		make(spare_, 256, false);
		make(d64_, 64, false);
		make(d16_, 16, false);
		glGenVertexArrays(1, &vao_);
		super_ = glu::program(kVS, kSuperFS);
		down_ = glu::program(kVS, kDownFS);
		comp_ = glu::program(kVS, kCompFS);
	}
	into(glow_);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void GlowRenderer::end(int w, int h) {
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	glBindVertexArray(vao_);
	glActiveTexture(GL_TEXTURE0);
	// Spread: glow -> spare, which becomes the glow (UpdateGlowMaps swaps them).
	into(spare_);
	glUseProgram(super_);
	glUniform1i(glGetUniformLocation(super_, "src"), 0);
	glUniform1f(glGetUniformLocation(super_, "radius"), 0.0078125f);
	glBindTexture(GL_TEXTURE_2D, glow_.tex);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	// Reduce: 256 -> 64 -> 16.
	glUseProgram(down_);
	glUniform1i(glGetUniformLocation(down_, "src"), 0);
	into(d64_);
	glUniform2f(glGetUniformLocation(down_, "texel"), 1.0f / 256, 1.0f / 256);
	glBindTexture(GL_TEXTURE_2D, spare_.tex);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	into(d16_);
	glUniform2f(glGetUniformLocation(down_, "texel"), 1.0f / 64, 1.0f / 64);
	glBindTexture(GL_TEXTURE_2D, d64_.tex);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	// Composite, added over the frame.
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo_));
	glViewport(prev_vp_[0], prev_vp_[1], prev_vp_[2] ? prev_vp_[2] : w, prev_vp_[3] ? prev_vp_[3] : h);
	static const bool only = getenv("OPENBG_GLOW_ONLY") != nullptr;  // debugging: the glow alone
	if (only) {
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glUseProgram(comp_);
	glUniform1i(glGetUniformLocation(comp_, "glow"), 0);
	glUniform1i(glGetUniformLocation(comp_, "d64"), 1);
	glUniform1i(glGetUniformLocation(comp_, "d16"), 2);
	glUniform1i(glGetUniformLocation(comp_, "high_quality"), 1);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, spare_.tex);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, d64_.tex);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, d16_.tex);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisable(GL_BLEND);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}

}  // namespace render
