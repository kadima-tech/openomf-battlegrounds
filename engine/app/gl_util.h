// Small shared helpers for the SDL2 + OpenGL 4.1 core apps.
#pragma once

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace glu {

inline GLuint compile(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(s, sizeof log, nullptr, log);
		throw std::runtime_error(std::string("shader: ") + log);
	}
	return s;
}

inline GLuint program(const char *vs, const char *fs) {
	GLuint p = glCreateProgram();
	glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
	glLinkProgram(p);
	GLint ok;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetProgramInfoLog(p, sizeof log, nullptr, log);
		throw std::runtime_error(std::string("link: ") + log);
	}
	return p;
}

inline GLuint texture_rgba(int w, int h, const uint8_t *rgba) {
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	return t;
}

// Column-major 4x4 helpers.
inline void mul(float *out, const float *a, const float *b) {
	float r[16];
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			r[j * 4 + i] = 0;
			for (int k = 0; k < 4; ++k) {
				r[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
			}
		}
	}
	memcpy(out, r, sizeof r);
}

inline void perspective(float *m, float fovy_deg, float aspect, float n, float f) {
	float t = 1.0f / std::tan(0.5f * fovy_deg * float(M_PI) / 180.0f);
	float r[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) / (n - f), -1, 0, 0, 2 * f * n / (n - f), 0};
	memcpy(m, r, sizeof r);
}

// View matrix for a camera at eye with yaw (around +Y) and pitch.
inline void fps_view(float *m, const float *eye, float yaw, float pitch) {
	float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
	float ry[16] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
	float rx[16] = {1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0, 0, 0, 0, 1};
	float t[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eye[0], -eye[1], -eye[2], 1};
	mul(m, ry, t);
	mul(m, rx, m);
}

// gluLookAt along `f` with `up` (both world directions): keeps the camera's roll.
inline void look_view(float *m, const float *eye, const float *f, const float *up) {
	auto norm = [](float *v) {
		float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
	};
	float F[3] = {f[0], f[1], f[2]};
	norm(F);
	float s[3] = {F[1] * up[2] - F[2] * up[1], F[2] * up[0] - F[0] * up[2], F[0] * up[1] - F[1] * up[0]};
	norm(s);
	float u[3] = {s[1] * F[2] - s[2] * F[1], s[2] * F[0] - s[0] * F[2], s[0] * F[1] - s[1] * F[0]};
	float r[16] = {s[0], u[0], -F[0], 0, s[1], u[1], -F[1], 0, s[2], u[2], -F[2], 0,
	               -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]), -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
	               F[0] * eye[0] + F[1] * eye[1] + F[2] * eye[2], 1};
	for (int i = 0; i < 16; ++i) m[i] = r[i];
}

inline void write_png(const char *path, int w, int h, const std::vector<uint8_t> &rgba, bool flip = true) {
	std::vector<uint8_t> raw;
	for (int i = 0; i < h; ++i) {
		int y = flip ? h - 1 - i : i;
		raw.push_back(0);
		raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
	}
	uLongf zn = compressBound(uLong(raw.size()));
	std::vector<uint8_t> z(zn);
	compress2(z.data(), &zn, raw.data(), uLong(raw.size()), 6);
	z.resize(zn);
	std::ofstream f(path, std::ios::binary);
	auto be32 = [&](uint32_t v) {
		uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
		f.write(reinterpret_cast<char *>(b), 4);
	};
	auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
		be32(uint32_t(data.size()));
		f.write(type, 4);
		f.write(reinterpret_cast<const char *>(data.data()), data.size());
		uLong crc = crc32(0, reinterpret_cast<const Bytef *>(type), 4);
		crc = crc32(crc, data.data(), uInt(data.size()));
		be32(uint32_t(crc));
	};
	f.write("\x89PNG\r\n\x1a\n", 8);
	chunk("IHDR", {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16),
	               uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0});
	chunk("IDAT", z);
	chunk("IEND", {});
}

// Offscreen colour+depth target, for screenshots from hidden windows.
inline GLuint offscreen(int w, int h) {
	GLuint fbo, color, depth;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(1, &color);
	glBindRenderbuffer(GL_RENDERBUFFER, color);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
	glGenRenderbuffers(1, &depth);
	glBindRenderbuffer(GL_RENDERBUFFER, depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
	return fbo;
}

}  // namespace glu
