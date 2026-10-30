#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "de/avi.h"
#include "de/image.h"
#include "de/lighting.h"
#include "de/texture.h"
#include "de/value.h"

namespace render {

namespace {

const char *kArenaVS = R"(#version 410 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv0;
layout(location = 3) in vec2 uv1;
layout(location = 4) in vec2 uv2;
layout(location = 5) in vec2 uv3;
layout(location = 6) in vec3 vcolour;
uniform mat4 vp;
uniform mat4 model;
uniform vec4 uv_xf[4];  // each stage's texture matrix: (sx, sy, tx, ty)
uniform vec3 pos_scale;  // a dynamic mesh ScaleModifier's; normals are left as they are
out vec3 v_normal;
out vec2 v_uv[4];
out vec3 v_pos;
out vec3 v_colour;
void main() {
	vec4 w = model * vec4(pos * pos_scale, 1.0);
	gl_Position = vp * w;
	v_normal = mat3(model) * normal;
	v_uv[0] = uv0 * uv_xf[0].xy + uv_xf[0].zw;
	v_uv[1] = uv1 * uv_xf[1].xy + uv_xf[1].zw;
	v_uv[2] = uv2 * uv_xf[2].xy + uv_xf[2].zw;
	v_uv[3] = uv3 * uv_xf[3].xy + uv_xf[3].zw;
	v_pos = w.xyz;
	v_colour = vcolour;
}
)";

// DE_CShader::CStage::Setup / SetStates: the first stage is its texture x its flat colour
// (TFACTOR) x the iterated diffuse colour; each stacked stage then combines with the result so
// far by its mode: Replace selects it, Modulate multiplies, Add and AddToLightMap add,
// SemiTranslucent multiplies. An environment stage samples the cube map along the camera-space
// reflection vector (texture coordinate generation 3, the renderer's view-to-world transform).
//
// The diffuse colour is the pass's vertex colour, on vertex-lit meshes lit the D3D way
// (DE_CGenericShader::ActivateAndDrawPatches flag 8: LIGHTING, COLORVERTEX, ambient and diffuse
// material from the vertex colour): colour x clamp(ambient + sum of light diffuse x N.L), with
// point lights of constant attenuation (DE_CD3DLight::SetupComplete) whose diffuse is the
// light's colour x intensity x its affector's intensity at the actor (DE_CD3DLight::Activate).
//
// A light projector pass (DE_CStandardProjectorShader::DrawPatches) adds the light's colour x
// intensity (TFACTOR, clamped) x the core "LightMap" image projected over the surface: a sphere
// light samples it twice, across the sphere's XZ and XY planes; a cone light through the cone.
const char *kArenaFS = R"(#version 410 core
in vec3 v_normal;
in vec2 v_uv[4];
in vec3 v_pos;
in vec3 v_colour;
uniform sampler2D tex0, tex1, tex2, tex3;
uniform sampler2D lightmap;
uniform samplerCube env;
uniform int stages;
uniform int has_tex[4];
uniform int has_alpha[4];
uniform int is_env[4];
uniform int mode[4];
uniform vec4 colour[4];
uniform int pass_mode;
uniform int stand_in_light;
uniform int has_env;
uniform vec3 eye;
uniform int lighting;
uniform vec3 ambient;
uniform int nlights;
uniform vec3 light_pos[8];
uniform vec3 light_diffuse[8];
uniform int splash;  // 0 none, 1 sphere projector, 2 cone projector
uniform vec3 splash_pos;
uniform mat3 splash_axes;  // rows: the cone's local X, Y, Z in world space
uniform float splash_radius, splash_near, splash_far, splash_tan;
uniform sampler2D shadow_atten;
uniform vec3 splash_colour;
uniform float splash_alpha;  // TFACTOR's alpha
uniform sampler2D bumpmap;
uniform int bump_stage;
uniform int bump_on;
uniform vec4 bump_matrix;
uniform float out_scale;    // the glow layer's blend factor (GlowFactor / SpecularFactor)
uniform float fog_density;  // the glow layer's black exponential fog (D3DFOG_EXP)
out vec4 color;
vec4 sample_stage(int i, vec2 uv) {
	if (is_env[i] != 0) {
		if (has_env == 0) return vec4(0.0);
		vec3 r = reflect(normalize(v_pos - eye), normalize(v_normal));
		// D3DTOP_BUMPENVMAP: u += du M00 + dv M10, v += du M01 + dv M11 on the next stage's
		// coordinates, here the world reflection vector; du/dv are V8U8, stored + 127.
		if (bump_on != 0) {
			vec2 buv = bump_stage == 0 ? v_uv[0] : bump_stage == 1 ? v_uv[1] : bump_stage == 2 ? v_uv[2] : v_uv[3];
			vec2 d = (texture(bumpmap, buv).rg * 255.0 - 127.0) / 127.0;
			r.xy += d.x * bump_matrix.xy + d.y * bump_matrix.zw;
		}
		return texture(env, r);
	}
	if (has_tex[i] == 0) return vec4(1.0);
	if (i == 0) return texture(tex0, uv);
	if (i == 1) return texture(tex1, uv);
	if (i == 2) return texture(tex2, uv);
	return texture(tex3, uv);
}
vec4 lightmap_at(vec2 uv) {
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(0.0);
	return texture(lightmap, uv);
}
void main() {
	if (splash != 0) {
		vec3 d = v_pos - splash_pos;
		vec4 v = vec4(0.0);
		if (splash == 1) {
			vec2 a = d.xz / (2.0 * splash_radius) + 0.5, b = d.xy / (2.0 * splash_radius) + 0.5;
			v = lightmap_at(a) * lightmap_at(b);
		} else {
			vec3 l = splash_axes * d;
			// Stage 0: DE_CD3DLight::UpdateConeUVWs' (x + w/2, y + w/2, w) with w = -2 tan(penumbra) z,
			// projected (D3DTTFF_PROJECTED): the LightMap, or the light's own image.
			if (l.z > 0.0) v = lightmap_at(-l.xy / (2.0 * l.z * splash_tan) + 0.5);
			// Stage 1: the core ShadowAttenuation ramp at (-x, z - near) / (far - near), clamped:
			// black behind the near plane, fading to nothing at the far end.
			// Its alpha equals its luminance and multiplies the alpha too (CStage::SetStates).
			vec2 at = vec2(-l.x, l.z - splash_near) / max(splash_far - splash_near, 1e-3);
			v *= texture(shadow_atten, clamp(at, 0.0, 1.0));
		}
		// No facing test: the pass draws the touched polygons the camera sees (D3DCULL_CW).
		color = vec4(splash_colour * v.rgb, v.a * splash_alpha);
		return;
	}
	vec4 c = vec4(1.0);
	for (int i = 0; i < 4; ++i) {
		if (i >= stages) break;
		vec4 t = sample_stage(i, v_uv[i]);
		// A multiplying stage whose image has alpha uses that alpha as its colour too
		// (COLORARG1 = TEXTURE | ALPHAREPLICATE, CStage flag 0x20).
		if (mode[i] == 2 && has_alpha[i] != 0) t.rgb = vec3(t.a);
		t *= colour[i];
		// An opaque first stage with alpha is alpha-tested (ALPHAREF 8, GREATEREQUAL).
		if (i == 0 && mode[0] == 0 && has_alpha[0] != 0 && t.a < 8.0 / 255.0) discard;
		if (i == 0) {
			vec3 diffuse = v_colour;
			if (lighting == 1) {
				vec3 n = normalize(v_normal), sum = ambient;
				for (int k = 0; k < 8; ++k) {
					if (k >= nlights) break;
					sum += light_diffuse[k] * max(dot(n, normalize(light_pos[k] - v_pos)), 0.0);
				}
				diffuse *= clamp(sum, 0.0, 1.0);
			}
			c = vec4(t.rgb * diffuse, t.a);
		} else if (mode[i] == 0) {
			c = t;
		} else if (mode[i] == 1 || mode[i] == 3) {
			c = vec4(c.rgb + t.rgb, c.a);
		} else {
			c *= t;
		}
	}
	if (stand_in_light != 0) {
		float d = abs(dot(normalize(v_normal), normalize(vec3(0.3, 0.8, 0.5))));
		c.rgb *= 0.45 + 0.6 * d;
	}
	if (pass_mode == 0) c.a = 1.0;
	c.rgb *= out_scale * exp(-fog_density * length(v_pos - eye));
	color = c;
}
)";

const de::World *g_world = nullptr;  // for the cube images batches name
GLuint g_specular = 0;               // the environment's "HighRangeSpecular" cube
bool g_specular_pass = false;        // drawing the specular glow passes: bind the specular cubes

// An Image\CubeEnvironment as a GL cube map (faces Top, Front, Bottom, Left, Right, Back).
GLuint cube_texture(const de::ObjectRef &cube) {
	static std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> cache;
	if (!cube || !g_world) return 0;
	auto k = std::make_pair(cube.file, cube.index);
	if (auto it = cache.find(k); it != cache.end()) return it->second;
	GLuint t = 0;
	try {
		auto faces = de::read_cube_faces(*g_world, cube);
		// GL faces: +X right, -X left, +Y top, -Y bottom, +Z front, -Z back.
		const int order[6] = {4, 3, 0, 2, 1, 5};
		bool ok = true;
		for (auto &f : faces) ok = ok && f;
		if (ok) {
			glGenTextures(1, &t);
			glBindTexture(GL_TEXTURE_CUBE_MAP, t);
			for (int f = 0; f < 6; ++f) {
				const de::ObjectRef &r = faces[size_t(order[f])];
				auto img = de::read_image(*r.file, r.info());
				auto px = img.rgba();
				glTexImage2D(GLenum(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), 0, GL_RGBA, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			}
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		}
	} catch (const std::exception &) {
		t = 0;
	}
	return cache[k] = t;
}

const de::ObjectInfo *find_child(const de::ObjectsFile &f, size_t parent, const std::string &cls, const std::string &name = "") {
	for (uint32_t c : de::ObjectsFile::children(f.object_data(f.objects()[parent]))) {
		auto &o = f.objects()[c];
		if (o.class_path == cls && (name.empty() || o.name == name)) {
			return &o;
		}
	}
	return nullptr;
}

}  // namespace

Lighting g_lighting;
de::LiveFloat g_live_values;
const int *g_texture_detail = nullptr;

namespace {
struct DetailTexture {
	GLuint t;
	int w, h;
};
std::vector<DetailTexture> g_detail_textures;
int g_applied_detail = 0;

void set_detail_level(const DetailTexture &d, int detail) {
	// SetMipMapLevel fails for a level with no pixels and the image stays at full size.
	const int level = (d.w >> detail) > 0 && (d.h >> detail) > 0 ? detail : 0;
	glBindTexture(GL_TEXTURE_2D, d.t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, level);
}
}  // namespace

void detail_texture(GLuint t, const de::Image &img) {
	if (!t || !img.adjust_level) return;
	forget_detail_texture(t);
	g_detail_textures.push_back({t, img.width, img.height});
	g_applied_detail = g_texture_detail ? *g_texture_detail : 0;
	set_detail_level(g_detail_textures.back(), g_applied_detail);
}

void forget_detail_texture(GLuint t) {
	std::erase_if(g_detail_textures, [t](const DetailTexture &d) { return d.t == t; });
}

void apply_texture_detail() {
	const int detail = g_texture_detail ? *g_texture_detail : 0;
	if (detail == g_applied_detail) return;
	g_applied_detail = detail;
	for (auto &d : g_detail_textures) set_detail_level(d, detail);
}
const bool *g_bump_enabled = nullptr;

bool SceneLight::lights(const void *owner) const {
	return reaches_all || (owner && std::find(reaches.begin(), reaches.end(), owner) != reaches.end());
}

float SceneLight::affector_intensity(const std::array<float, 3> &p) const {
	float d[3] = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
	if (shape == kSphere) {
		float r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (r > outer) return 0;
		if (r < inner || outer <= inner) return 1;
		return 1 - (r - inner) / (outer - inner);
	}
	// Cone: into the affector's frame (rows are its axes); along +Z out to far-end.
	float l[3];
	for (int i = 0; i < 3; ++i) l[i] = frame.r[size_t(i * 3)] * d[0] + frame.r[size_t(i * 3 + 1)] * d[1] + frame.r[size_t(i * 3 + 2)] * d[2];
	float r = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
	if (r > outer || l[2] <= 0 || l[2] >= outer) return 0;
	float v = r < inner || outer <= inner ? 1 : 1 - (r - inner) / (outer - inner);
	float cs = l[2] / std::fmax(r, 1e-6f);
	float cu = std::cos(umbra * 0.017453292f), cp = std::cos(penumbra * 0.017453292f);
	if (cs <= cp) return 0;
	if (cs < cu) v *= cu == cp ? 0 : (cs - cp) / (cu - cp);
	return v;
}

// ---------------------------------------------------------------------------------------------
// Arena

namespace {

// Every GL texture made from an Image\AVI, with the video's decoded frames.
struct AnimatedTexture {
	de::AviVideo video;
	bool mipmap = true;
	std::vector<GLuint> textures;
	size_t shown = 0;
};
std::map<std::pair<const de::ObjectsFile *, uint32_t>, AnimatedTexture> g_animated;

std::vector<uint8_t> rgb_to_rgba(const std::vector<uint8_t> &rgb) {
	std::vector<uint8_t> out(rgb.size() / 3 * 4, 255);
	for (size_t i = 0, n = rgb.size() / 3; i < n; ++i) memcpy(&out[i * 4], &rgb[i * 3], 3);
	return out;
}

// DE_CD3DTexture::CreateTextureFromImage (Direct3D 0x2001bce0) for an AVI: one level, or a full
// chain whose level k is the frame's top-left (W >> k) x (H >> k) crop (DE_CImage::SetMipMapLevel
// is not overridden, so every level reads the full-size frame).
void upload_avi_frame(GLuint t, const AnimatedTexture &a, const std::vector<uint8_t> &px, bool create) {
	glBindTexture(GL_TEXTURE_2D, t);
	const int w = a.video.width, h = a.video.height;
	for (int k = 0;; ++k) {
		int lw = std::max(1, w >> k), lh = std::max(1, h >> k);
		std::vector<uint8_t> level(size_t(lw) * lh * 4);
		for (int y = 0; y < lh; ++y) memcpy(&level[size_t(y) * lw * 4], &px[size_t(y) * w * 4], size_t(lw) * 4);
		if (create) glTexImage2D(GL_TEXTURE_2D, k, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, level.data());
		else glTexSubImage2D(GL_TEXTURE_2D, k, 0, 0, lw, lh, GL_RGBA, GL_UNSIGNED_BYTE, level.data());
		if (!a.mipmap || (lw == 1 && lh == 1)) {
			if (create) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, k);
			break;
		}
	}
	if (create) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, a.mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
}

}  // namespace

void update_animated_textures(const std::map<std::pair<const de::ObjectsFile *, uint32_t>, float> &phases) {
	for (auto &[k, a] : g_animated) {
		auto it = phases.find(k);
		size_t f = it == phases.end() ? 0 : a.video.frame_for_phase(it->second);
		f = std::min(f, a.video.frames.size() - 1);
		if (f == a.shown) continue;
		a.shown = f;
		auto px = rgb_to_rgba(a.video.frames[f]);
		for (GLuint t : a.textures) upload_avi_frame(t, a, px, false);
	}
}

GLuint texture_of(std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> &textures_, const de::ObjectRef &r) {
	if (!r) {
		return 0;
	}
	auto k = std::make_pair(r.file, r.index);
	if (auto it = textures_.find(k); it != textures_.end()) {
		return it->second;
	}
	GLuint t = 0;
	try {
		if (r.info().class_path == "Image\\AVI") {
			AnimatedTexture &a = g_animated[k];
			if (a.textures.empty()) {
				a.video = de::decode_avi(de::avi_of_image(r.file->object_data(r.info()).contents));
				a.mipmap = de::read_image_header(r.file->object_data(r.info()).contents).mipmap;
			}
			if (a.video.decoded()) {
				auto px = rgb_to_rgba(a.video.frames[a.shown]);
				glGenTextures(1, &t);
				upload_avi_frame(t, a, px, true);
				a.textures.push_back(t);
			} else {
				g_animated.erase(k);
			}
			return textures_[k] = t;
		}
		auto img = de::read_image(*r.file, r.info());
		if (!img.channels.empty()) {
			auto px = img.rgba();
			t = glu::texture_rgba(img.width, img.height, px.data());
			detail_texture(t, img);
			if (const char *dir = getenv("OPENBG_DUMP_TEXTURES")) glu::write_png((std::string(dir) + "/" + r.info().name + ".png").c_str(), img.width, img.height, px, false);
		}
	} catch (const std::exception &e) {
		if (getenv("OPENBG_DEBUG_TEXTURES")) fprintf(stderr, "[texture] %s (%s): %s\n", r.info().name.c_str(), r.info().class_path.c_str(), e.what());
	}
	if (!t && getenv("OPENBG_DEBUG_TEXTURES")) fprintf(stderr, "[texture] %s (%s): no texture\n", r.info().name.c_str(), r.info().class_path.c_str());
	return textures_[k] = t;
}

namespace {

// DE_CD3DTexture::CreateTextureFromImage (0x2001bce0) for an image with channels 6 and 7 (the du/dv
// DE_CStandardImage::GenerateBumpUVs stores): a V8U8 texture of each byte + 0x81, kept here as
// the unsigned bytes (decoded in the shader), mipmapped like the image.
GLuint bump_texture(const de::ObjectRef &r) {
	static std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> cache;
	if (!r) return 0;
	auto k = std::make_pair(r.file, r.index);
	if (auto it = cache.find(k); it != cache.end()) return it->second;
	GLuint t = 0;
	try {
		auto img = de::read_image(*r.file, r.info());
		auto du = img.channels.find(6), dv = img.channels.find(7);
		size_t n = size_t(img.width) * size_t(img.height);
		if (du != img.channels.end() && dv != img.channels.end() && du->second.size() >= n && dv->second.size() >= n) {
			std::vector<uint8_t> px(n * 2);
			for (size_t i = 0; i < n; ++i) px[i * 2] = du->second[i], px[i * 2 + 1] = dv->second[i];
			glGenTextures(1, &t);
			glBindTexture(GL_TEXTURE_2D, t);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, img.width, img.height, 0, GL_RG, GL_UNSIGNED_BYTE, px.data());
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			if (img.mipmap || img.adjust_level) glGenerateMipmap(GL_TEXTURE_2D);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, img.mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			detail_texture(t, img);
		}
	} catch (const std::exception &) {
	}
	return cache[k] = t;
}

// DE_CImage +0x44 flag 8: the image has an alpha channel (channel 3).
bool image_has_alpha(const de::ObjectRef &r) {
	static std::map<std::pair<const de::ObjectsFile *, uint32_t>, bool> cache;
	if (!r) return false;
	auto k = std::make_pair(r.file, r.index);
	if (auto it = cache.find(k); it != cache.end()) return it->second;
	bool a = false;
	try {
		a = de::read_image(*r.file, r.info()).channels.count(3) != 0;
	} catch (const std::exception &) {
	}
	return cache[k] = a;
}

// The passes of each material: a stage that does not stack (+0x2c) starts a pass, stacked
// stages join it (DE_CMultiStageTextureProcess::Process makes one render material element per
// stage, stacking ones sharing the pass). A disabled stage (+0x34) is skipped. A pass covers the
// faces its first stage has coordinates for (an environment stage: every face of the material).
// With `keep` the batches keep their vertices
// for re-posing (MeshBatch::vertices / source).
// Faces no drawable material covers make no render element in the original (a material's stages
// are its elements); OPENBG_STAND_INS=1 draws them flat for debugging.
const bool g_stand_ins = getenv("OPENBG_STAND_INS") != nullptr;

// DE_CStandardMesh::CalculateNormals (Core 0x7012fb10) with AddNeighborFaces: each face's corner
// normal is its own face normal plus those of the faces around it (reached across edges, sharing a
// vertex with it) that share a smoothing group (face flags) and that vertex, normalised.
std::vector<std::array<std::array<float, 3>, 3>> corner_normals(const de::Mesh &mesh, const std::vector<std::array<float, 3>> &pos) {
	size_t nf = mesh.faces.size();
	std::vector<std::array<float, 3>> fn(nf, {0, 0, 1});
	for (size_t i = 0; i < nf; ++i) {
		auto &f = mesh.faces[i];
		if (f.v[0] >= pos.size() || f.v[1] >= pos.size() || f.v[2] >= pos.size()) continue;
		auto &a = pos[f.v[0]], &b = pos[f.v[1]], &c = pos[f.v[2]];
		float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		std::array<float, 3> n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
		float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (l > 0) n = {n[0] / l, n[1] / l, n[2] / l};
		if (!(l > 0) || std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) <= 0.1f) n[2] = 1;
		fn[i] = n;
	}
	auto shares_vertex = [&](size_t a, size_t b) {
		for (uint16_t x : mesh.faces[a].v)
			for (uint16_t y : mesh.faces[b].v)
				if (x == y) return true;
		return false;
	};
	std::vector<std::array<std::array<float, 3>, 3>> out(nf);
	std::vector<uint16_t> around;
	std::vector<uint32_t> seen(nf, ~0u);
	for (size_t i = 0; i < nf; ++i) {
		around.clear();
		std::vector<size_t> stack{i};
		while (!stack.empty()) {
			size_t cur = stack.back();
			stack.pop_back();
			for (uint16_t g : mesh.faces[cur].adjacent) {
				if (g == 0xffff || g >= nf || g == i || seen[g] == i || !shares_vertex(i, g)) continue;
				seen[g] = uint32_t(i);
				around.push_back(g);
				stack.push_back(g);
			}
		}
		const auto &f = mesh.faces[i];
		for (int k = 0; k < 3; ++k) {
			std::array<float, 3> n = fn[i];
			for (uint16_t g : around) {
				const auto &o = mesh.faces[g];
				if (!(f.flags & o.flags)) continue;
				for (uint16_t v : o.v) {
					if (v == f.v[size_t(k)]) n = {n[0] + fn[g][0], n[1] + fn[g][1], n[2] + fn[g][2]};
				}
			}
			float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			out[i][size_t(k)] = l > 0 ? std::array<float, 3>{n[0] / l, n[1] / l, n[2] / l} : fn[i];
		}
	}
	return out;
}

// A mesh's ScaleModifiers (de::mesh_modifiers): the static ones scale the positions once, when
// the mesh is built (Process at priority 100000, once); a dynamic one restores the positions saved
// at its first run and scales them again every frame (300000), so the last one registered wins.
struct MeshScale {
	std::array<float, 3> fixed{1, 1, 1};
	std::shared_ptr<const de::ScaleModifier> dynamic;
};
MeshScale mesh_scale(const de::World &w, const de::ObjectRef &mesh) {
	MeshScale out;
	for (auto &m : de::mesh_modifiers(w, mesh)) {
		if (m->dynamic) {
			out.dynamic = m;
			continue;
		}
		auto s = m->scale(g_live_values);
		for (int k = 0; k < 3; ++k) out.fixed[size_t(k)] *= s[size_t(k)];
	}
	return out;
}

std::vector<MeshBatch> build_batches(const de::Mesh &mesh, const std::vector<de::Material> &materials, const de::Transform &place,
                                     const std::function<GLuint(const de::ObjectRef &)> &texture, float *lo, float *hi,
                                     bool keep = false, const MeshScale &scale = {}, std::vector<bool> *drawn = nullptr) {
	std::vector<MeshBatch> out;
	const int lighting = mesh.vertex_lit() ? 1 : mesh.projector_lit() ? 2 : 0;
	std::vector<std::array<float, 3>> wpos(mesh.vertices.size()), npos(mesh.vertices.size());
	for (size_t i = 0; i < wpos.size(); ++i) {
		const auto &v = mesh.vertices[i];
		npos[i] = place.apply(v);
		wpos[i] = place.apply({v[0] * scale.fixed[0], v[1] * scale.fixed[1], v[2] * scale.fixed[2]});
		for (int j = 0; lo && j < 3; ++j) {
			lo[j] = std::fmin(lo[j], wpos[i][j]);
			hi[j] = std::fmax(hi[j], wpos[i][j]);
		}
	}
	// The vertex normals are the mesh's own (CalculateNormals); the modifier never processes them.
	auto corner = corner_normals(mesh, npos);
	std::vector<bool> covered(mesh.faces.size(), false);
	float material_glow = 0;
	auto emit = [&](const std::vector<const de::Layer *> &pass, const std::vector<uint32_t> &faces, bool stand_in, bool overlay, bool late = false) {
		std::vector<std::map<uint32_t, size_t>> uvs(pass.size());
		for (size_t s = 0; s < pass.size(); ++s) {
			for (size_t k = 0; k < pass[s]->uvs.size(); ++k) uvs[s][pass[s]->uvs[k].face] = k;
		}
		std::vector<BatchVertex> vs;
		std::vector<uint32_t> src;
		MeshBatch bt;
		for (uint32_t fi : faces) {
			if (fi >= mesh.faces.size()) continue;
			covered[fi] = true;
			auto &f = mesh.faces[fi];
			if (f.v[0] >= wpos.size() || f.v[1] >= wpos.size() || f.v[2] >= wpos.size()) continue;
			for (int k = 0; k < 3; ++k) {
				BatchVertex v{};
				memcpy(v.pos, wpos[f.v[k]].data(), sizeof v.pos);
				for (int j = 0; j < 3; ++j) bt.bmin[j] = std::fmin(bt.bmin[j], v.pos[j]), bt.bmax[j] = std::fmax(bt.bmax[j], v.pos[j]);
				memcpy(v.normal, corner[fi][size_t(k)].data(), sizeof v.normal);
				v.colour[0] = v.colour[1] = v.colour[2] = 1;
				for (size_t s = 0; s < pass.size() && s < 4; ++s) {
					if (auto it = uvs[s].find(fi); it != uvs[s].end()) {
						v.uv[s][0] = pass[s]->uvs[it->second].uv[k][0];
						v.uv[s][1] = pass[s]->uvs[it->second].uv[k][1];
						if (s == 0 && !pass[0]->corner_colours.empty()) memcpy(v.colour, pass[0]->corner_colours[it->second * 3 + size_t(k)].data(), sizeof v.colour);
					}
				}
				vs.push_back(v);
				src.push_back(f.v[k]);
			}
		}
		if (vs.empty()) return;
		bt.stages = int(std::min<size_t>(pass.size(), 4));
		for (int s = 0; s < bt.stages; ++s) {
			bt.tex[s] = pass[size_t(s)]->environment ? 0 : texture(pass[size_t(s)]->image);
			bt.mode[s] = pass[size_t(s)]->mode;
			bt.environment[s] = pass[size_t(s)]->environment;
			bt.alpha[s] = !pass[size_t(s)]->environment && image_has_alpha(pass[size_t(s)]->image);
			memcpy(bt.colour[s], pass[size_t(s)]->colour.data(), sizeof bt.colour[s]);
		}
		bt.stand_in_light = stand_in;
		bt.overlay = overlay;
		bt.late = late;
		bt.lighting = lighting;
		bt.two_sided = (mesh.flags & 0x4000) != 0;
		for (auto *l : pass) {
			if (l->environment && l->cube) {
				bt.cube = cube_texture(l->cube);
				bt.specular_cube = cube_texture(de::specular_cube(l->cube));
				break;
			}
		}
		for (size_t k = 0; k < pass.size() && k < 4; ++k) {
			if (pass[k]->bump) {
				bt.bump = bump_texture(pass[k]->image);
				bt.bump_stage = int(k);
				bt.bump_factor = pass[k]->bump_factor;
				bt.bump_rotate = pass[k]->bump_rotate;
				break;
			}
		}
		for (size_t k = 0; k < pass.size() && k < 4; ++k) {
			if (pass[k]->environment || pass[k]->modifiers.empty()) continue;
			for (auto &m : pass[k]->modifiers) {
				if (m->dynamic) bt.uv_mod[k] = m;
			}
			auto xf = pass[k]->modifiers.back()->uv(g_live_values);
			memcpy(bt.uv_xf[k], xf.data(), sizeof bt.uv_xf[k]);
		}
		bt.pos_mod = scale.dynamic;
		bt.glow = material_glow;
		bool reflects = false;
		for (auto *l : pass) reflects = reflects || l->environment;
		if (reflects) bt.specular = pass.back()->specular;
		bt.count = GLsizei(vs.size());
		glGenVertexArrays(1, &bt.vao);
		glBindVertexArray(bt.vao);
		glGenBuffers(1, &bt.vbo);
		glBindBuffer(GL_ARRAY_BUFFER, bt.vbo);
		glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vs.size() * sizeof(BatchVertex)), vs.data(), keep ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, pos));
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, normal));
		for (GLuint s = 0; s < 4; ++s) {
			glVertexAttribPointer(2 + s, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)(offsetof(BatchVertex, uv) + s * 2 * sizeof(float)));
		}
		glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, colour));
		for (GLuint i = 0; i < 7; ++i) glEnableVertexAttribArray(i);
		if (keep) {
			bt.vertices = std::move(vs);
			bt.source = std::move(src);
		}
		out.push_back(std::move(bt));
	};
	for (auto &m : materials) {
		static const char *skip = getenv("OPENBG_SKIP_MATERIAL");
		if (skip && m.name == skip) continue;
		static const char *only = getenv("OPENBG_ONLY_MATERIAL");
		if (only && m.name != only) continue;
		material_glow = m.glow;
		std::vector<std::vector<const de::Layer *>> passes;
		bool previous_drawn = false;
		for (auto &l : m.layers) {
			bool usable = !l.disabled && (l.environment || (l.image && !l.uvs.empty()));
			// The first stage starts a pass whatever its stacked flag (Process pairs a stacked
			// stage with the one before it).
			if (l.stacked && &l != &m.layers.front()) {
				// A stacked stage goes with the stage it stacks on (Process skips the pair).
				if (usable && previous_drawn && !passes.empty()) passes.back().push_back(&l);
				continue;
			}
			previous_drawn = usable;
			if (usable) passes.push_back({&l});
		}
		std::vector<uint32_t> all;
		for (auto &l : m.layers) {
			for (auto &fu : l.uvs) all.push_back(fu.face);
		}
		std::sort(all.begin(), all.end());
		all.erase(std::unique(all.begin(), all.end()), all.end());
		if (passes.empty()) {
			if (g_stand_ins) emit({}, all, true, false);
			continue;
		}
		static const bool debug = getenv("OPENBG_DEBUG_MATERIALS") != nullptr;
		if (debug) {
			fprintf(stderr, "[material] %s (%s):", m.name.c_str(), lighting == 1 ? "vertex-lit" : lighting == 2 ? "projector-lit" : "unlit");
			for (auto &p : passes) {
				fprintf(stderr, " |");
				for (auto *l : p) {
					fprintf(stderr, " %s(%s m%d c%.2f,%.2f,%.2f,%.2f%s)", l->name.c_str(), l->environment ? (l->cube ? l->cube.info().name.c_str() : "env") : l->image ? l->image.info().name.c_str() : "-", l->mode,
					        l->colour[0], l->colour[1], l->colour[2], l->colour[3], l->corner_colours.empty() ? "" : " vcol");
					if (!l->environment && image_has_alpha(l->image)) fprintf(stderr, "[A]");
				}
			}
			fprintf(stderr, "\n");
			for (auto &p : passes) {
				int degenerate = 0;
				float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
				for (auto &fu : p[0]->uvs) {
					float du = std::fabs(fu.uv[1][0] - fu.uv[0][0]) + std::fabs(fu.uv[2][0] - fu.uv[0][0]);
					float dv = std::fabs(fu.uv[1][1] - fu.uv[0][1]) + std::fabs(fu.uv[2][1] - fu.uv[0][1]);
					if (du + dv < 1e-4f) ++degenerate;
					for (int k = 0; k < 3; ++k) for (int j = 0; j < 2; ++j) lo[j] = std::fmin(lo[j], fu.uv[k][j]), hi[j] = std::fmax(hi[j], fu.uv[k][j]);
				}
				fprintf(stderr, "[material]   %s: %zu faces, %d degenerate, uv %.2f..%.2f, %.2f..%.2f\n", p[0]->name.c_str(), p[0]->uvs.size(), degenerate, lo[0], hi[0], lo[1], hi[1]);
			}
		}
		for (size_t i = 0; i < passes.size(); ++i) {
			std::vector<uint32_t> faces;
			if (passes[i][0]->environment) faces = all;
			else for (auto &fu : passes[i][0]->uvs) faces.push_back(fu.face);
			emit(passes[i], faces, false, i > 0, passes[0][0]->mode != 0);
		}
	}
	material_glow = 0;
	std::vector<uint32_t> rest;
	for (uint32_t i = 0; i < mesh.faces.size(); ++i) {
		if (!covered[i]) rest.push_back(i);
	}
	static const bool debug = getenv("OPENBG_DEBUG_MATERIALS") != nullptr;
	if (debug && !rest.empty()) fprintf(stderr, "[material]   %zu of %zu faces in no material\n", rest.size(), mesh.faces.size());
	if (g_stand_ins) emit({}, rest, true, false);
	if (drawn) *drawn = covered;
	return out;
}

const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

GLuint g_environment = 0;  // the arena environment's cube map (DE_CEnvironment +0x39c)
GLuint g_lightmap = 0;     // DE_CLight::GlobalLightMap: the core image "LightMap"
GLuint g_shadow_atten = 0; // the core image "ShadowAttenuation" (cone projectors' second stage)

// The D3D lights a vertex-lit actor at `at` takes (DE_CStandardRenderLight::UpdateLitActors: the
// lights that reach its owner and whose mask has 2): up to eight, by diffuse.
void set_vertex_lights(GLuint prog, const std::array<float, 3> &at, bool target_static, const void *owner) {
	std::vector<std::pair<float, int>> order;
	std::vector<std::array<float, 3>> diffuse(g_lighting.lights.size());
	for (size_t i = 0; i < g_lighting.lights.size(); ++i) {
		const SceneLight &l = g_lighting.lights[i];
		// The static rule is in the reach list when there is one (per render affector); without one
		// (the arena viewer) it falls back to the mesh's own position mode.
		if (!(l.mask & 2) || (l.reaches_all && l.position_static && target_static) || !l.lights(owner)) continue;
		float a = l.affector_intensity(at) * l.intensity;
		if (a <= 0) continue;
		for (int k = 0; k < 3; ++k) diffuse[i][size_t(k)] = l.colour[size_t(k)] * a;
		order.push_back({-(diffuse[i][0] + diffuse[i][1] + diffuse[i][2]), int(i)});
	}
	std::sort(order.begin(), order.end());
	float pos[8][3] = {}, dif[8][3] = {};
	int n = 0;
	for (auto &[w, i] : order) {
		if (n == 8) break;
		memcpy(pos[n], g_lighting.lights[size_t(i)].pos.data(), sizeof pos[n]);
		memcpy(dif[n], diffuse[size_t(i)].data(), sizeof dif[n]);
		++n;
	}
	glUniform1i(glGetUniformLocation(prog, "nlights"), n);
	glUniform3fv(glGetUniformLocation(prog, "light_pos"), 8, &pos[0][0]);
	glUniform3fv(glGetUniformLocation(prog, "light_diffuse"), 8, &dif[0][0]);
	glUniform3fv(glGetUniformLocation(prog, "ambient"), 1, g_lighting.ambient.data());
}

// DE_CGenericShader::AddStage's frame blend for the pass's first stage: Replace/Opaque writes
// (ONE, ZERO); Modulate (DESTCOLOR, INVSRCALPHA); Add and AddToLightMap (SRCALPHA, ONE);
// SemiTranslucent (SRCALPHA, INVSRCALPHA). Passes after a material's first draw over it with the
// depth test at less-or-equal and no depth writes.
// `at`: where the drawn actor is (its vertex-lit batches take the lights there); null for the
// static scene, whose vertex-lit batches take the lights at their own centre.
// Culling: D3DCULL_CW keeps what the original's front faces are. OPENBG_CULL=none turns it off,
// =flip culls the other side (debugging).
void set_cull(bool two_sided) {
	static const char *mode = getenv("OPENBG_CULL");
	static const bool none = mode && !strcmp(mode, "none"), flip = mode && !strcmp(mode, "flip");
	if (two_sided || none) {
		glDisable(GL_CULL_FACE);
		return;
	}
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(flip ? GL_CW : GL_CCW);
}

std::vector<DeferredDraw> *g_sink = nullptr;
float g_vp[16], g_eye[3];
// The values of the actor being drawn (ActorMeshRenderer::Item::live), for its dynamic modifiers.
const de::LiveFloat *g_item_live = nullptr;

void draw_batch_list(GLuint prog, const std::vector<const MeshBatch *> &batches, int phase, const std::array<float, 3> *at, const void *owner);

void draw_batches(GLuint prog, const std::vector<MeshBatch> &batches, int phase, const std::array<float, 3> *at = nullptr,
                  const void *owner = nullptr) {
	std::vector<const MeshBatch *> list;
	for (auto &b : batches) {
		if (phase >= 0 && b.late != (phase == 1)) continue;
		if (g_sink && b.late) {
			// A blended first pass joins the transparent layer, drawn later in its sorted order
			// with the state it would have had now.
			float model[16];
			glGetUniformfv(prog, glGetUniformLocation(prog, "model"), model);
			std::array<float, 16> m, vp;
			std::array<float, 3> eye{g_eye[0], g_eye[1], g_eye[2]};
			std::copy(model, model + 16, m.begin());
			std::copy(g_vp, g_vp + 16, vp.begin());
			std::optional<std::array<float, 3>> where;
			if (at) where = *at;
			std::array<float, 3> origin = at ? *at
			                                 : std::array<float, 3>{(b.bmin[0] + b.bmax[0]) / 2, (b.bmin[1] + b.bmax[1]) / 2, (b.bmin[2] + b.bmax[2]) / 2};
			const MeshBatch *pb = &b;
			de::LiveFloat live = g_item_live ? *g_item_live : de::LiveFloat{};
			g_sink->push_back({pb, origin, [prog, m, vp, eye, where, owner, pb, live] {
				                   glUseProgram(prog);
				                   glUniform1f(glGetUniformLocation(prog, "out_scale"), 1);
				                   glUniform1f(glGetUniformLocation(prog, "fog_density"), 0);
				                   glUniformMatrix4fv(glGetUniformLocation(prog, "vp"), 1, GL_FALSE, vp.data());
				                   glUniform3fv(glGetUniformLocation(prog, "eye"), 1, eye.data());
				                   glUniformMatrix4fv(glGetUniformLocation(prog, "model"), 1, GL_FALSE, m.data());
				                   g_item_live = &live;
				                   draw_batch_list(prog, {pb}, -1, where ? &*where : nullptr, owner);
				                   g_item_live = nullptr;
			                   }});
			continue;
		}
		list.push_back(&b);
	}
	draw_batch_list(prog, list, -1, at, owner);
}

void draw_batch_list(GLuint prog, const std::vector<const MeshBatch *> &batches, int phase, const std::array<float, 3> *at, const void *owner) {
	if (at) set_vertex_lights(prog, *at, false, owner);
	else glUniform3fv(glGetUniformLocation(prog, "ambient"), 1, g_lighting.ambient.data());
	static const char *tex_names[4] = {"tex0", "tex1", "tex2", "tex3"};
	for (int i = 0; i < 4; ++i) glUniform1i(glGetUniformLocation(prog, tex_names[i]), i);
	glUniform1i(glGetUniformLocation(prog, "env"), 4);
	for (const MeshBatch *bp : batches) {
		const MeshBatch &b = *bp;
		if (phase >= 0 && b.late != (phase == 1)) continue;
		GLuint cube = g_specular_pass ? (b.specular_cube ? b.specular_cube : g_specular) : (b.cube ? b.cube : g_environment);
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
		glUniform1i(glGetUniformLocation(prog, "has_env"), cube != 0);
		glUniform1i(glGetUniformLocation(prog, "bumpmap"), 6);
		glActiveTexture(GL_TEXTURE6);
		glBindTexture(GL_TEXTURE_2D, b.bump);
		glUniform1i(glGetUniformLocation(prog, "bump_stage"), b.bump_stage);
		glUniform1i(glGetUniformLocation(prog, "bump_on"), b.bump != 0 && (!g_bump_enabled || *g_bump_enabled));
		float bump_matrix[4];
		memcpy(bump_matrix, b.bump_matrix, sizeof bump_matrix);
		// CStage::SetStates (0x20031b60), every draw: a RotateValue turns the du/dv (radians = value x
		// 0.000723) and the factor is ignored; while it is 0 the matrix is BumpFactor x identity.
		if (b.bump_factor || b.bump_rotate) {
			const float rot = b.bump_rotate ? b.bump_rotate->eval(0, g_live_values) : 0;
			if (rot != 0) {
				const float a = rot * 0.000723f;
				bump_matrix[0] = std::cos(a), bump_matrix[1] = -std::sin(a);
				bump_matrix[2] = std::sin(a), bump_matrix[3] = std::cos(a);
			} else {
				bump_matrix[0] = bump_matrix[3] = b.bump_factor ? b.bump_factor->eval(0, g_live_values) : 0;
				bump_matrix[1] = bump_matrix[2] = 0;
			}
		}
		glUniform4fv(glGetUniformLocation(prog, "bump_matrix"), 1, bump_matrix);
		float uv_xf[4][4];
		memcpy(uv_xf, b.uv_xf, sizeof uv_xf);
		for (int s = 0; s < 4; ++s) {
			if (!b.uv_mod[s]) continue;
			auto xf = b.uv_mod[s]->uv(g_live_values);
			memcpy(uv_xf[s], xf.data(), sizeof uv_xf[s]);
		}
		glUniform4fv(glGetUniformLocation(prog, "uv_xf"), 4, &uv_xf[0][0]);
		std::array<float, 3> pos_scale{1, 1, 1};
		if (b.pos_mod) pos_scale = b.pos_mod->scale(g_item_live && *g_item_live ? *g_item_live : g_live_values);
		glUniform3fv(glGetUniformLocation(prog, "pos_scale"), 1, pos_scale.data());
		int has_tex[4], is_env[4], mode[4], has_alpha[4];
		for (int i = 0; i < 4; ++i) {
			glActiveTexture(GL_TEXTURE0 + GLenum(i));
			glBindTexture(GL_TEXTURE_2D, b.tex[i]);
			has_tex[i] = b.tex[i] != 0;
			is_env[i] = b.environment[i];
			mode[i] = b.mode[i];
			has_alpha[i] = b.alpha[i];
		}
		glUniform1iv(glGetUniformLocation(prog, "has_alpha"), 4, has_alpha);
		glUniform1i(glGetUniformLocation(prog, "stages"), b.stages);
		glUniform1iv(glGetUniformLocation(prog, "has_tex"), 4, has_tex);
		glUniform1iv(glGetUniformLocation(prog, "is_env"), 4, is_env);
		glUniform1iv(glGetUniformLocation(prog, "mode"), 4, mode);
		glUniform4fv(glGetUniformLocation(prog, "colour"), 4, &b.colour[0][0]);
		int pass_mode = b.stages ? b.mode[0] : 0;
		glUniform1i(glGetUniformLocation(prog, "pass_mode"), pass_mode);
		glUniform1i(glGetUniformLocation(prog, "stand_in_light"), b.stand_in_light);
		glUniform1i(glGetUniformLocation(prog, "lighting"), b.lighting);
		if (b.lighting == 1 && !at) {
			set_vertex_lights(prog, {(b.bmin[0] + b.bmax[0]) / 2, (b.bmin[1] + b.bmax[1]) / 2, (b.bmin[2] + b.bmax[2]) / 2}, b.position_static,
			                  b.light_owner);
		}
		set_cull(b.two_sided);
		switch (pass_mode) {
		case 1: case 3: glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
		case 2: glEnable(GL_BLEND); glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
		case 4: glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
		default: glDisable(GL_BLEND); break;
		}
		bool no_depth_write = b.overlay || pass_mode != 0;
		glDepthMask(no_depth_write ? GL_FALSE : GL_TRUE);
		glDepthFunc(b.overlay ? GL_LEQUAL : GL_LESS);
		glBindVertexArray(b.vao);
		glDrawArrays(GL_TRIANGLES, 0, b.count);
	}
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glActiveTexture(GL_TEXTURE0);
}

// DE_CConeAffector::IsTouchingSphere (Core 0x70080560): within far end + r of the apex, the
// depth along +Z overlapping near end .. far end, and the centre within |z| tan(penumbra) +
// (1 + tan) r of the axis.
bool cone_touches_sphere(const SceneLight &l, const std::array<float, 3> &c, float r) {
	const std::array<float, 3> d{c[0] - l.pos[0], c[1] - l.pos[1], c[2] - l.pos[2]};
	if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > (r + l.outer) * (r + l.outer)) return false;
	const auto local = l.frame.inverse_rotate(d);
	const float z = local[2];
	if (z - r > l.outer || l.near_end > z + r) return false;
	const float tn = std::tan(l.penumbra * 0.017453292f);
	const float radial = std::sqrt(local[0] * local[0] + local[1] * local[1]);
	return radial < z * tn || radial - (tn + 1) * r - std::fabs(z * tn) < 0;
}

// The light projector passes (DE_CStandardProjectorShader: no depth writes, LESSEQUAL, always
// D3DCULL_CW): per light, the polygons of the projector-lit meshes it touches, each once
// (FillProjectedVertexBuffer copies them into the light's own buffer). A sphere light takes the
// octree cells its bounding box overlaps, a cell whole once it is smaller than the sphere; a cone
// the leaf cells whose bounding spheres it touches (DE_COctree::GetPotentialCollisions). Every
// light whose mask reaches them (projector-lit elements take 4, 8 and 0x10: DE_CD3DRenderMesh
// Element::InitialSetup) and that reaches the mesh's owner. The shared projectors (the LightMap)
// add (ONE, ONE); a cone with its own image blends by its light (SetStates: 0/3 ONE/ONE, 1/5
// SRCALPHA/INVSRCALPHA, 2 ZERO/INVSRCCOLOR, 4 ONE/INVSRCCOLOR); a sphere with its own image has
// no projector.
void draw_splashes(GLuint prog, const std::vector<ProjectorTarget> &targets) {
	static const bool off = getenv("OPENBG_NO_PROJECTORS") != nullptr;  // debugging: vertex lighting only
	if (!g_lightmap || off) return;
	glUniform1i(glGetUniformLocation(prog, "lightmap"), 5);
	glUniform1i(glGetUniformLocation(prog, "shadow_atten"), 6);
	const float one[3] = {1, 1, 1};
	glUniform3fv(glGetUniformLocation(prog, "pos_scale"), 1, one);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_2D, g_shadow_atten ? g_shadow_atten : g_lightmap);
	glEnable(GL_BLEND);
	glDepthMask(GL_FALSE);
	glDepthFunc(GL_LEQUAL);
	set_cull(false);
	std::vector<uint32_t> faces;
	std::vector<GLint> first;
	std::vector<GLsizei> count;
	for (const SceneLight &l : g_lighting.lights) {
		if (!(l.mask & 0x1c) || l.intensity <= 0 || l.outer <= 0) continue;
		if (l.has_image && l.shape == SceneLight::kSphere) continue;
		const int blend = l.image ? l.blend : 0;
		// DrawPatches' TFACTOR: the colour x intensity with alpha 1, except for the alpha-blended
		// projectors (SetupConeProjector +0x68 clear), whose alpha is the intensity instead.
		const bool alpha_blended = blend == 1 || blend == 5;
		std::array<float, 3> c;
		for (int k = 0; k < 3; ++k) c[size_t(k)] = std::clamp(l.colour[size_t(k)] * (alpha_blended ? 1.0f : l.intensity), 0.0f, 1.0f);
		glUniform1f(glGetUniformLocation(prog, "splash_alpha"), alpha_blended ? std::clamp(l.intensity, 0.0f, 1.0f) : 1.0f);
		switch (blend) {
		case 1: case 5: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
		case 2: glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); break;
		case 4: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
		default: glBlendFunc(GL_ONE, GL_ONE); break;
		}
		glActiveTexture(GL_TEXTURE5);
		glBindTexture(GL_TEXTURE_2D, l.image ? l.image : g_lightmap);
		glUniform1i(glGetUniformLocation(prog, "splash"), l.shape == SceneLight::kSphere ? 1 : 2);
		glUniform3fv(glGetUniformLocation(prog, "splash_pos"), 1, l.pos.data());
		glUniform3fv(glGetUniformLocation(prog, "splash_colour"), 1, c.data());
		glUniform1f(glGetUniformLocation(prog, "splash_radius"), l.outer);
		glUniform1f(glGetUniformLocation(prog, "splash_far"), l.outer);
		glUniform1f(glGetUniformLocation(prog, "splash_near"), l.near_begin);
		glUniform1f(glGetUniformLocation(prog, "splash_tan"), std::tan(std::clamp(l.penumbra, 1.0f, 89.0f) * 0.017453292f));
		// Row-major rotation rows are the local axes; GL takes columns, so pass it transposed.
		glUniformMatrix3fv(glGetUniformLocation(prog, "splash_axes"), 1, GL_TRUE, l.frame.r.data());
		for (auto &t : targets) {
			if ((l.reaches_all && l.position_static && t.position_static) || !l.lights(t.light_owner)) continue;
			bool overlaps = true;
			for (int k = 0; k < 3; ++k) overlaps = overlaps && l.pos[size_t(k)] + l.outer >= t.bmin[k] && l.pos[size_t(k)] - l.outer <= t.bmax[k];
			if (!overlaps) continue;
			faces.clear();
			if (l.shape == SceneLight::kSphere) {
				t.octree.sphere_faces(t.place.inverse().apply(l.pos), l.outer, faces);
			} else {
				t.octree.affector_faces([&l](const std::array<float, 3> &p, float r) { return cone_touches_sphere(l, p, r); },
				                        [&t](const std::array<float, 3> &p) { return t.place.apply(p); }, faces);
			}
			first.clear(), count.clear();
			for (uint32_t f : faces) {
				if (f >= t.drawn.size() || !t.drawn[f]) continue;
				if (!first.empty() && first.back() + count.back() == GLint(f * 3)) count.back() += 3;
				else first.push_back(GLint(f * 3)), count.push_back(3);
			}
			if (first.empty()) continue;
			glBindVertexArray(t.vao);
			glMultiDrawArrays(GL_TRIANGLES, first.data(), count.data(), GLsizei(first.size()));
		}
	}
	glUniform1i(glGetUniformLocation(prog, "splash"), 0);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glActiveTexture(GL_TEXTURE0);
}

// AssureProjectedTargetOctree: the mesh's stored "D3DProjectedTargetOctree", else one built with
// cells of at most 100 (the element has a render affector).
ProjectorTarget make_projector_target(const de::Mesh &mesh, const de::ObjectRef &object, const de::Transform &place,
                                      const std::array<float, 3> &scale, const std::vector<bool> &drawn) {
	ProjectorTarget t;
	t.place = place;
	t.drawn = drawn;
	de::Mesh scaled = mesh;
	for (auto &v : scaled.vertices) v = {v[0] * scale[0], v[1] * scale[1], v[2] * scale[2]};
	t.octree = de::read_octree(*object.file, object.info());
	if (!t.octree) t.octree = de::build_octree(scaled, 100);
	std::vector<BatchVertex> vs(scaled.faces.size() * 3);
	for (size_t f = 0; f < scaled.faces.size(); ++f) {
		for (int k = 0; k < 3; ++k) {
			BatchVertex &v = vs[f * 3 + size_t(k)];
			v = BatchVertex{};
			auto p = place.apply(scaled.vertices[scaled.faces[f].v[size_t(k)]]);
			memcpy(v.pos, p.data(), sizeof v.pos);
			v.normal[1] = 1;
			for (int j = 0; j < 3; ++j) t.bmin[j] = std::fmin(t.bmin[j], p[size_t(j)]), t.bmax[j] = std::fmax(t.bmax[j], p[size_t(j)]);
		}
	}
	glGenVertexArrays(1, &t.vao);
	glBindVertexArray(t.vao);
	glGenBuffers(1, &t.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, t.vbo);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vs.size() * sizeof(BatchVertex)), vs.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, pos));
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, normal));
	for (GLuint s = 0; s < 4; ++s) glVertexAttribPointer(2 + s, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)(offsetof(BatchVertex, uv) + s * 2 * sizeof(float)));
	glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), (void *)offsetof(BatchVertex, colour));
	for (GLuint i = 0; i < 7; ++i) glEnableVertexAttribArray(i);
	return t;
}

void use_mesh_program(GLuint prog, const float *vp, const float *eye) {
	apply_texture_detail();
	std::copy(vp, vp + 16, g_vp);
	std::copy(eye, eye + 3, g_eye);
	glUseProgram(prog);
	glUniform1f(glGetUniformLocation(prog, "out_scale"), 1);
	glUniform1f(glGetUniformLocation(prog, "fog_density"), 0);
	glUniformMatrix4fv(glGetUniformLocation(prog, "vp"), 1, GL_FALSE, vp);
	glUniform3fv(glGetUniformLocation(prog, "eye"), 1, eye);
}

}  // namespace

void set_transparent_sink(std::vector<DeferredDraw> *sink) { g_sink = sink; }

// DE_CEnvironment::ObjProcessReferences (Creature 0x1500b7f0) v5, the last references chunk:
// [RGBA advanced reference][seven BOOL + image slots: +0x320, +0x338, +0x32c, +0x308, +0x314,
// +0x344, +0x350][RGBA][FLOAT]; the first six make the cube environment image (+0x39c). Desert
// names them Top, Front, Bottom, Left, Right, Back, so that order is taken as the cube's faces.
GLuint load_environment(const de::World &world, const de::ObjectsFile &file) {
	for (uint32_t i = 0; i < file.objects().size(); ++i) {
		if (file.objects()[i].class_path != "Actor\\Creature\\Environment") continue;
		try {
			de::Contents c(file.object_data(file.objects()[i]).references);
			size_t last = 0;
			int version = 0;
			while (c.remaining() > 1) {
				de::Chunk ch = de::begin_chunk(c);
				last = c.pos(), version = ch.version;
				c.seek(ch.end);
			}
			if (version != 5) return 0;
			c.seek(last);
			de::Chunk adv = de::begin_chunk(c);  // CValueRGBA::CAdvancedReference (v1: BOOL, reference)
			c.seek(adv.end);
			std::array<de::ObjectRef, 6> faces;
			for (int k = 0; k < 7; ++k) {
				if (!c.boolean()) continue;
				de::ObjectRef r = world.resolve(file, de::Reference::read(c));
				if (k < 6) faces[size_t(k)] = r;
			}
			// GL faces: +X right, -X left, +Y top, -Y bottom, +Z front, -Z back.
			const int order[6] = {4, 3, 0, 2, 1, 5};
			GLuint t = 0;
			glGenTextures(1, &t);
			glBindTexture(GL_TEXTURE_CUBE_MAP, t);
			for (int f = 0; f < 6; ++f) {
				const de::ObjectRef &r = faces[size_t(order[f])];
				if (!r) return 0;
				auto img = de::read_image(*r.file, r.info());
				auto px = img.rgba();
				glTexImage2D(GLenum(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), 0, GL_RGBA, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			}
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
			return t;
		} catch (const std::exception &) {
			return 0;
		}
	}
	return 0;
}

void ArenaRenderer::load(const de::World &world, const std::string &stem) {
	prog_ = glu::program(kArenaVS, kArenaFS);
	const de::ObjectsFile *file = world.file(stem);
	if (!file) {
		throw std::runtime_error("no arena file " + stem);
	}
	g_world = &world;
	g_environment = load_environment(world, *file);
	for (uint32_t i = 0; i < file->objects().size(); ++i) {
		if (file->objects()[i].class_path == "Actor\\Creature\\Environment") {
			g_specular = cube_texture(de::specular_cube({file, i}));
			break;
		}
	}
	g_lighting.ambient = de::read_ambient(world, *file);
	if (!g_lightmap) {
		if (const de::ObjectsFile *core = world.file("COREImages")) {
			for (uint32_t i = 0; i < core->objects().size(); ++i) {
				if (core->objects()[i].name == "LightMap") g_lightmap = texture_of(textures_, {core, i});
				if (core->objects()[i].name == "ShadowAttenuation") {
					// Sampled with D3DTADDRESS_CLAMP (SetupConeProjector's stage 1).
					g_shadow_atten = texture_of(textures_, {core, i});
					glBindTexture(GL_TEXTURE_2D, g_shadow_atten);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				}
			}
		}
	}
	// Floor: the fighting-surface collision mesh (world-space; it lives in a plain folder).
	for (auto &o : file->objects()) {
		if (o.class_path == "Mesh\\Standard" && o.name.find("FightingSurfaceCollision") != std::string::npos) {
			auto m = de::read_mesh(*file, o);
			for (auto &f : m.faces) {
				floor_tris_.push_back({m.vertices[f.v[0]], m.vertices[f.v[1]], m.vertices[f.v[2]]});
			}
		}
	}
	auto tex = [this](const de::ObjectRef &r) { return texture(r); };
	auto parents = file->parents();
	for (auto &item : de::collect_scene(world, *file)) {
		std::vector<uint32_t> chain;
		for (int p = int(item.actor_index); p >= 0; p = parents[size_t(p)]) chain.push_back(uint32_t(p));
		static const bool debug = getenv("OPENBG_DEBUG_MATERIALS") != nullptr;
		if (debug) {
			float l[3] = {1e30f, 1e30f, 1e30f}, h[3] = {-1e30f, -1e30f, -1e30f};
			for (auto &v : item.mesh.vertices) {
				auto w = item.world.apply(v);
				for (int j = 0; j < 3; ++j) l[j] = std::fmin(l[j], w[j]), h[j] = std::fmax(h[j], w[j]);
			}
			fprintf(stderr, "[mesh] %s: %.0f %.0f %.0f .. %.0f %.0f %.0f\n", item.actor.c_str(), l[0], l[1], l[2], h[0], h[1], h[2]);
		}
		// Baked in world space: no arena mesh has a dynamic modifier, so one is taken as it starts.
		MeshScale ms = mesh_scale(world, item.mesh_object);
		if (ms.dynamic) {
			auto d = ms.dynamic->scale(g_live_values);
			for (int k = 0; k < 3; ++k) ms.fixed[size_t(k)] *= d[size_t(k)];
			ms.dynamic = nullptr;
		}
		std::vector<bool> drawn;
		for (auto &b : build_batches(item.mesh, item.materials, item.world, tex, lo, hi, false, ms, &drawn)) {
			b.position_static = item.position_static;
			triangles += size_t(b.count) / 3;
			batches_.push_back(b);
			chains_.push_back(chain);
		}
		if (item.mesh.projector_lit()) {
			targets_.push_back(make_projector_target(item.mesh, item.mesh_object, item.world, ms.fixed, drawn));
			targets_.back().position_static = item.position_static;
			target_chains_.push_back(chain);
		}
	}
}

void ArenaRenderer::set_moving(const std::set<uint32_t> &actors) {
	if (actors == moving_) return;
	moving_ = actors;
	// Actors only ever start moving, so their batches are simply dropped.
	size_t k = 0;
	for (size_t i = 0; i < batches_.size(); ++i) {
		bool moved = false;
		for (uint32_t a : chains_[i]) moved = moved || actors.count(a);
		if (moved) continue;
		if (k != i) batches_[k] = std::move(batches_[i]), chains_[k] = std::move(chains_[i]);
		++k;
	}
	batches_.resize(k);
	chains_.resize(k);
	k = 0;
	for (size_t i = 0; i < targets_.size(); ++i) {
		bool moved = false;
		for (uint32_t a : target_chains_[i]) moved = moved || actors.count(a);
		if (moved) continue;
		if (k != i) targets_[k] = std::move(targets_[i]), target_chains_[k] = std::move(target_chains_[i]);
		++k;
	}
	targets_.resize(k);
	target_chains_.resize(k);
}

void ArenaRenderer::set_light_owners(const std::function<const void *(const std::vector<uint32_t> &chain)> &owner_of) {
	for (size_t i = 0; i < batches_.size(); ++i) batches_[i].light_owner = owner_of(chains_[i]);
	for (size_t i = 0; i < targets_.size(); ++i) targets_[i].light_owner = owner_of(target_chains_[i]);
}

void ArenaRenderer::draw(const float *vp, const float *eye) {
	use_mesh_program(prog_, vp, eye);
	glUniformMatrix4fv(glGetUniformLocation(prog_, "model"), 1, GL_FALSE, kIdentity);
	draw_batches(prog_, batches_, 0);
	draw_splashes(prog_, targets_);
	draw_batches(prog_, batches_, 1);
}

// The glow-layer passes of `batches` (see ArenaRenderer::draw_glow); a specular one only when the
// arena has a specular cube (CubeSpecularEnvironmentImage).
static void draw_glow_batches(GLuint prog_, const std::vector<MeshBatch> &batches_, const std::array<float, 3> *at, const void *owner = nullptr) {
	std::vector<MeshBatch> one(1);
	for (auto &b : batches_) {
		if (b.glow <= 0 && (b.specular <= 0 || !g_specular)) continue;
		bool specular = b.glow <= 0;
		static const bool dbg = getenv("OPENBG_DEBUG_GLOW") != nullptr;
		static int shown = 0;
		if (dbg && shown < 40) {
			++shown;
			fprintf(stderr, "[glow] batch %d tris, glow %.2f specular %.2f, mode %d, bounds %.0f %.0f %.0f .. %.0f %.0f %.0f\n", b.count / 3, b.glow, b.specular, b.mode[0],
			        b.bmin[0], b.bmin[1], b.bmin[2], b.bmax[0], b.bmax[1], b.bmax[2]);
		}
		glUniform1f(glGetUniformLocation(prog_, "out_scale"), specular ? b.specular : std::fmin(b.glow * 0.25f, 1.0f));
		glUniform1f(glGetUniformLocation(prog_, "fog_density"), specular ? 0.0f : b.mode[0] == 3 ? 0.7f : 0.0007f);
		// The batch without its kept vertices (only the GL state is needed).
		one[0] = MeshBatch{};
		auto &c = one[0];
		c.vao = b.vao, c.vbo = b.vbo, c.count = b.count, c.stages = b.stages;
		for (int k = 0; k < 4; ++k) {
			c.tex[k] = b.tex[k], c.mode[k] = b.mode[k], c.environment[k] = b.environment[k], c.alpha[k] = b.alpha[k];
			memcpy(c.colour[k], b.colour[k], sizeof c.colour[k]);
		}
		c.lighting = b.lighting, c.cube = b.cube, c.specular_cube = b.specular_cube;
		c.two_sided = b.two_sided, c.light_owner = b.light_owner, c.position_static = b.position_static;
		c.bump = b.bump, c.bump_stage = b.bump_stage;
		memcpy(c.bump_matrix, b.bump_matrix, sizeof c.bump_matrix);
		c.bump_factor = b.bump_factor, c.bump_rotate = b.bump_rotate;
		memcpy(c.uv_xf, b.uv_xf, sizeof c.uv_xf);
		for (int k = 0; k < 4; ++k) c.uv_mod[k] = b.uv_mod[k];
		c.pos_mod = b.pos_mod;
		c.overlay = true;
		g_specular_pass = specular;
		draw_batches(prog_, one, -1, at, owner);
		g_specular_pass = false;
	}
	glUniform1f(glGetUniformLocation(prog_, "out_scale"), 1);
	glUniform1f(glGetUniformLocation(prog_, "fog_density"), 0);
}

// UpdateGlowMaps draws the glow layer's shaders (DE_CShader::GetShader flag 0x400) into the glow
// target: each pass again with the source scaled by its factor (SRCBLEND = BLENDFACTOR, the
// factor x 255 as grey; DE_CGenericShader::ActivateAndDrawPatches 0x20033286). The GlowFactor
// ones take a quarter of it, clamped to 1, and fade into black exponential fog (density 0.0007;
// 0.7 for AddToLightMap passes); the SpecularFactor ones (flag 0x800) take it whole, no fog.
void ArenaRenderer::draw_glow(const float *vp, const float *eye) {
	use_mesh_program(prog_, vp, eye);
	glUniformMatrix4fv(glGetUniformLocation(prog_, "model"), 1, GL_FALSE, kIdentity);
	draw_glow_batches(prog_, batches_, nullptr);
}

GLuint ArenaRenderer::texture(const de::ObjectRef &r) { return texture_of(textures_, r); }

float ArenaRenderer::floor_height(float x, float z, float from_y) const {
	float best = -1e30f;
	for (auto &t : floor_tris_) {
		const auto &a = t[0], &b = t[1], &c = t[2];
		float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
		if (std::fabs(d) < 1e-9f) {
			continue;
		}
		float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
		float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
		float l3 = 1 - l1 - l2;
		if (l1 < 0 || l2 < 0 || l3 < 0) {
			continue;
		}
		float y = l1 * a[1] + l2 * b[1] + l3 * c[1];
		if (y <= from_y && y > best) {
			best = y;
		}
	}
	return best;
}

bool ArenaRenderer::segment_hits(const std::array<float, 3> &p, const std::array<float, 3> &q) const {
	return segment_point(p, q).has_value();
}

std::optional<std::array<float, 3>> ArenaRenderer::segment_point(const std::array<float, 3> &p, const std::array<float, 3> &q) const {
	// Moller-Trumbore against every collision triangle, for t in (0, 1); the nearest crossing.
	float d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
	float best = 2;
	for (auto &t : floor_tris_) {
		float e1[3] = {t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]};
		float e2[3] = {t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]};
		float h[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
		float a = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
		if (std::fabs(a) < 1e-9f) continue;
		float f = 1 / a;
		float s[3] = {p[0] - t[0][0], p[1] - t[0][1], p[2] - t[0][2]};
		float u = f * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);
		if (u < 0 || u > 1) continue;
		float qv[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
		float v = f * (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]);
		if (v < 0 || u + v > 1) continue;
		float tt = f * (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]);
		if (tt > 0 && tt < 1 && tt < best) best = tt;
	}
	if (best > 1) return std::nullopt;
	return std::array<float, 3>{p[0] + d[0] * best, p[1] + d[1] * best, p[2] + d[2] * best};
}

// ---------------------------------------------------------------------------------------------
// Actors made at run time (pickups, jets, spawned props)

namespace {
de::Image paint_image(const de::Image &src, const FighterRenderer::Paint &paint, bool reflection, const std::vector<de::Image> &scaled = {});
}

void ActorMeshRenderer::draw(const de::World &data, const std::vector<Item> &actors, const float *vp, const float *eye) {
	g_world = &data;
	if (!prog_) prog_ = glu::program(kArenaVS, kArenaFS);
	if (texture_version != built_version_) {
		for (auto &[k, list] : meshes_) {
			for (auto &b : list) {
				glDeleteBuffers(1, &b.vbo);
				glDeleteVertexArrays(1, &b.vao);
			}
		}
		meshes_.clear();
		built_version_ = texture_version;
	}
	use_mesh_program(prog_, vp, eye);
	for (const Item &a : actors) {
		if (!a.actor) continue;
		auto key = std::make_pair(a.actor.file, a.actor.index);
		auto it = meshes_.find(key);
		if (it == meshes_.end()) {
			std::vector<MeshBatch> all;
			auto tex = [this](const de::ObjectRef &r) -> GLuint {
				// A filtered image: painted like the fighter's RGBMain / ReflectionMask.
				if (r && paint_of) {
					auto k = std::make_pair(r.file, r.index);
					if (auto it = textures_.find(k); it != textures_.end()) return it->second;
					if (auto p = paint_of(r)) {
						GLuint t = 0;
						try {
							auto img = de::read_image(*r.file, r.info());
							if (!img.channels.empty()) {
								auto px = paint_image(img, *p, r.info().name == "ReflectionMask").rgba();
								t = glu::texture_rgba(img.width, img.height, px.data());
								detail_texture(t, img);
							}
						} catch (const std::exception &) {
						}
						return textures_[k] = t;
					}
				}
				return texture_of(textures_, r);
			};
			try {
				for (auto &m : de::actor_meshes(data, a.actor, false)) {
					auto mesh = de::read_mesh(*m.file, m.info());
					auto materials = de::read_materials(data, m);
					if (image_of) {
						for (auto &mat : materials) {
							for (auto &l : mat.layers) {
								if (auto img = image_of(l.manager)) l.image = *img;
							}
						}
					}
					auto b = build_batches(mesh, materials, de::Transform{}, tex, nullptr, nullptr, false, mesh_scale(data, m));
					all.insert(all.end(), b.begin(), b.end());
				}
			} catch (const std::exception &) {
			}
			it = meshes_.emplace(key, std::move(all)).first;
		}
		if (it->second.empty()) continue;
		const de::Transform &t = a.world;
		float m[16] = {t.r[0], t.r[1], t.r[2], 0, t.r[3], t.r[4], t.r[5], 0, t.r[6], t.r[7], t.r[8], 0, t.t[0], t.t[1], t.t[2], 1};
		glUniformMatrix4fv(glGetUniformLocation(prog_, "model"), 1, GL_FALSE, m);
		g_item_live = &a.live;
		draw_batches(prog_, it->second, -1, &t.t, a.light_owner);
		g_item_live = nullptr;
	}
}

void ActorMeshRenderer::draw_glow(const std::vector<Item> &actors, const float *vp, const float *eye) {
	if (!prog_) return;
	use_mesh_program(prog_, vp, eye);
	for (const Item &a : actors) {
		auto it = meshes_.find(std::make_pair(a.actor.file, a.actor.index));
		if (it == meshes_.end() || it->second.empty()) continue;
		const de::Transform &t = a.world;
		float m[16] = {t.r[0], t.r[1], t.r[2], 0, t.r[3], t.r[4], t.r[5], 0, t.r[6], t.r[7], t.r[8], 0, t.t[0], t.t[1], t.t[2], 1};
		glUniformMatrix4fv(glGetUniformLocation(prog_, "model"), 1, GL_FALSE, m);
		g_item_live = &a.live;
		draw_glow_batches(prog_, it->second, &t.t, a.light_owner);
		g_item_live = nullptr;
	}
}

// ---------------------------------------------------------------------------------------------
// Fighter

namespace {

// DE_COMF2FighterImageFilter::DE_CColorConversion::Update: per mask level i the colour's
// i x c x 256, clamped to 0..65280; Filter sums the three channels' entries and shifts by 8.
// The image named exactly ReflectionMask takes the colours lightened: normalize(c x 0.5 + 0.5).
// Then each RGBScaled image of the same size is added, scaled by UpdateStaticColorRemapperTable's
// clamp(0.7 L + 0.3, 0, 1), L a weighted sum of the unlightened colours.
de::Image paint_image(const de::Image &src, const FighterRenderer::Paint &paint, bool reflection, const std::vector<de::Image> &scaled) {
	FighterRenderer::Paint p = paint;
	if (reflection) {
		for (auto &c : p) {
			float l = 0;
			for (auto &v : c) v = v * 0.5f + 0.5f, l += v * v;
			l = std::sqrt(l);
			for (auto &v : c) v /= l > 0 ? l : 1e-6f;
		}
	}
	int table[3][256][3];
	for (int m = 0; m < 3; ++m) {
		for (int i = 0; i < 256; ++i) {
			for (int k = 0; k < 3; ++k) table[m][i][k] = int(std::clamp(float(i) * p[size_t(m)][size_t(k)] * 256.0f, 0.0f, 65280.0f));
		}
	}
	de::Image out = src;
	auto get = [&](int ch) -> const std::vector<uint8_t> * {
		auto it = src.channels.find(ch);
		return it == src.channels.end() ? nullptr : &it->second;
	};
	const auto *r = get(0), *g = get(1), *b = get(2);
	if (!r || !g || !b) return out;
	for (size_t px = 0; px < r->size(); ++px) {
		int mask[3] = {(*r)[px], (*g)[px], (*b)[px]};
		for (int k = 0; k < 3; ++k) {
			int sum = (table[0][mask[0]][k] + table[1][mask[1]][k] + table[2][mask[2]][k]) >> 8;
			out.channels[k][px] = uint8_t(std::min(sum, 255));
		}
	}
	const float L = 0.35f * (0.7f * paint[0][0] + 0.8f * paint[0][1] + 0.5f * paint[0][2]) +
	                0.10f * (0.7f * paint[1][0] + 0.8f * paint[1][1] + 0.5f * paint[1][2]) +
	                0.05f * (0.7f * paint[2][0] + 0.8f * paint[2][1] + 0.5f * paint[2][2]);
	const float gain = std::clamp(0.7f * L + 0.3f, 0.0f, 1.0f);
	for (const de::Image &sc : scaled) {
		if (sc.width != src.width || sc.height != src.height) continue;
		for (int ch = 0; ch < 3; ++ch) {
			auto it = sc.channels.find(ch);
			if (it == sc.channels.end() || it->second.size() < r->size()) break;
			for (size_t px = 0; px < r->size(); ++px)
				out.channels[ch][px] = uint8_t(std::min(int(out.channels[ch][px]) + int(float(it->second[px]) * gain), 255));
		}
	}
	return out;
}

}  // namespace

void FighterRenderer::load(const de::World &world, const de::ObjectsFile &f, const de::ObjectsFile *fighter, size_t model) {
	const auto &objs = f.objects();
	if (model >= objs.size() || objs[model].class_path != "FighterModel") model = SIZE_MAX;
	for (size_t i = 0; i < objs.size() && model == SIZE_MAX; ++i) {
		if (objs[i].class_path == "FighterModel") {
			model = i;
			break;
		}
	}
	if (model == SIZE_MAX) {
		throw std::runtime_error("no FighterModel found");
	}
	uint32_t mesh_index = 0;
	for (uint32_t c : de::ObjectsFile::children(f.object_data(objs[model]))) {
		if (objs[c].class_path != "Mesh\\Standard") {
			continue;
		}
		auto m = de::read_mesh(f, objs[c]);
		if (m.faces.size() > mesh_.faces.size()) {
			mesh_ = std::move(m);
			mesh_index = c;
		}
	}
	g_world = &world;
	materials_ = de::read_materials(world, {&f, mesh_index});
	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	for (auto &v : mesh_.vertices) {
		for (int j = 0; j < 3; ++j) {
			lo[j] = std::fmin(lo[j], v[j]);
			hi[j] = std::fmax(hi[j], v[j]);
		}
	}
	radius = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])) / 2;
	if (fighter) {
		const de::ObjectInfo *bank_obj = nullptr, *deform = nullptr;
		for (auto &o : fighter->objects()) {
			if (!bank_obj && o.class_path == "AnimationSequenceGroup") bank_obj = &o;
			if (!deform && o.class_path == "ActorDeformInfo") deform = &o;
		}
		if (bank_obj && deform) {
			bank = de::read_anim_bank(*fighter, *bank_obj);
			bones_ = de::read_skin(*fighter, *deform, f, mesh_index);
			for (auto &b : bones_) {
				bone_channel_.push_back(bank.channel(b.name));
				inv_bind_.push_back(b.bind.inverse());
			}
			rigged = true;
			load_attachments(world, f, model, mesh_index, *fighter, deform, bank_obj);
		}
	}
	// DE_CCreatureModel::Setup (OMF 0x35037470): the model's descendant images named RGBScaled* join
	// the filter's scaled list.
	scaled_.clear();
	if (model != SIZE_MAX) {
		std::vector<uint32_t> todo = de::ObjectsFile::children(f.object_data(objs[model]));
		while (!todo.empty()) {
			uint32_t i = todo.back();
			todo.pop_back();
			if (objs[i].class_path == "Image\\Standard" && objs[i].name.rfind("RGBScaled", 0) == 0) {
				try {
					scaled_.push_back(de::read_image(f, objs[i]));
				} catch (const std::exception &) {
				}
			}
			for (uint32_t c : de::ObjectsFile::children(f.object_data(objs[i]))) todo.push_back(c);
		}
	}
	prog_ = glu::program(kArenaVS, kArenaFS);
	// Until a pilot is known: the filter's own defaults (DE_COMF2FighterImageFilter::ObjInitContents).
	paint_ = {{{1.0f, 0.88f, 0.8f}, {0.36f, 0.56f, 0.48f}, {0.48f, 0.96f, 1.28f}}};
	build();
}

void FighterRenderer::draw_glow(const float *vp, const float *eye) {
	if (batches_.empty()) return;
	use_mesh_program(prog_, vp, eye);
	glUniformMatrix4fv(glGetUniformLocation(prog_, "model"), 1, GL_FALSE, kIdentity);
	draw_glow_batches(prog_, batches_, &last_at_, light_owner_);
}

// Painted once: an image is filtered only on its first decode (AssureUpdatedImage while +0x9c is 1).
void FighterRenderer::set_paint(const Paint &p) {
	if (painted_) return;
	paint_ = p;
	painted_ = true;
	build();
}

void FighterRenderer::build() {
	for (auto &b : batches_) {
		glDeleteBuffers(1, &b.vbo);
		glDeleteVertexArrays(1, &b.vao);
	}
	for (auto &[k, t] : painted_textures_) {
		forget_detail_texture(t);
		glDeleteTextures(1, &t);
	}
	painted_textures_.clear();
	auto tex = [this](const de::ObjectRef &r) -> GLuint {
		if (!r) return 0;
		const std::string &name = r.info().name;
		if (name.rfind("RGBMain", 0) != 0 && name.rfind("ReflectionMask", 0) != 0) return texture_of(textures_, r);
		auto k = std::make_pair(r.file, r.index);
		if (auto it = painted_textures_.find(k); it != painted_textures_.end()) return it->second;
		GLuint t = 0;
		try {
			auto img = de::read_image(*r.file, r.info());
			if (!img.channels.empty()) {
				auto px = paint_image(img, paint_, name == "ReflectionMask", scaled_).rgba();
				t = glu::texture_rgba(img.width, img.height, px.data());
				detail_texture(t, img);
			}
		} catch (const std::exception &) {
		}
		return painted_textures_[k] = t;
	};
	batches_ = build_batches(mesh_, materials_, de::Transform{}, tex, nullptr, nullptr, true);
	for (auto &at : attachments_) {
		for (auto &b : at.batches) {
			glDeleteBuffers(1, &b.vbo);
			glDeleteVertexArrays(1, &b.vao);
		}
		at.batches = build_batches(at.mesh, at.materials, de::Transform{}, tex, nullptr, nullptr, true);
	}
}

void FighterRenderer::draw(const float *vp, const float *eye, const de::Transform &world, const AnimState &a) {
	light_owner_ = a.light_owner;
	if (rigged && a.pose && a.pose->size() == bank.channels.size()) {
		last_pose_ = *a.pose;
		draw_pose(vp, eye, world, a.pose);
		draw_attachments(vp, eye, world, *a.pose, a);
		return;
	}
	auto valid = [&](int q) { return q >= 0 && size_t(q) < bank.sequences.size(); };
	if (rigged && a.captured && valid(a.initial_seq) && valid(a.final_seq)) {
		auto pose = de::blend_poses(bank.pose(bank.sequences[size_t(a.initial_seq)], a.initial_t),
		                            bank.pose(bank.sequences[size_t(a.final_seq)], a.final_t), std::clamp(a.t, 0.0f, 1.0f));
		follow(pose, a);
		last_pose_ = pose;
		draw_pose(vp, eye, world, &pose);
		draw_attachments(vp, eye, world, pose, a);
		return;
	}
	if (!rigged || a.sequence < 0 || size_t(a.sequence) >= bank.sequences.size()) {
		draw_pose(vp, eye, world, nullptr);
		return;
	}
	const auto &seq = bank.sequences[size_t(a.sequence)];
	if (a.activations != seen_activations_) {
		seen_activations_ = a.activations;
		if (a.prelude) {
			captured_ = last_pose_;  // AnimCaptureAsInitial
		}
	}
	std::vector<de::Transform> pose;
	if (a.prelude && !captured_.empty()) {
		pose = de::blend_poses(captured_, bank.pose(seq, a.prelude_time), std::clamp(a.t, 0.0f, 1.0f));
	} else {
		pose = bank.pose(seq, a.prelude ? a.prelude_time : a.t);
	}
	follow(pose, a);
	last_pose_ = pose;
	draw_pose(vp, eye, world, &pose);
	draw_attachments(vp, eye, world, pose, a);
}

void FighterRenderer::follow(std::vector<de::Transform> &pose, const AnimState &a) const {
	int pc = a.pelvis ? bank.channel("Pelvis") : -1;
	if (pc < 0 || size_t(pc) >= pose.size()) return;
	de::Transform delta = a.pelvis->then(pose[size_t(pc)].inverse());
	for (auto &p : pose) p = delta.then(p);
}

void FighterRenderer::draw(const float *vp, const float *eye, const de::Transform &world, int sequence, float t) {
	light_owner_ = nullptr;
	if (rigged && sequence >= 0 && size_t(sequence) < bank.sequences.size()) {
		auto pose = bank.pose(bank.sequences[size_t(sequence)], t);
		draw_pose(vp, eye, world, &pose);
	} else {
		draw_pose(vp, eye, world, nullptr);
	}
}

namespace {

// Linear blend skinning (inferred): v' = sum_b w_b * pre * Pose_b * Bind_b^-1 * v.
std::vector<std::array<float, 3>> skin_vertices(const de::Mesh &mesh, const std::vector<de::SkinBone> &bones, const std::vector<int> &channel,
                                                const std::vector<de::Transform> &inv_bind, const std::vector<de::Transform> &pose,
                                                const de::Transform &pre) {
	std::vector<std::array<float, 3>> pos = mesh.vertices;
	std::vector<de::Transform> skin(bones.size());
	for (size_t b = 0; b < bones.size(); ++b) {
		int c = channel[b];
		skin[b] = c >= 0 && size_t(c) < pose.size() ? pre.then(pose[size_t(c)]).then(inv_bind[b]) : pre;
	}
	for (size_t v = 0; v < pos.size(); ++v) {
		float acc[3] = {0, 0, 0}, wsum = 0;
		for (size_t b = 0; b < bones.size(); ++b) {
			auto &w = bones[b].weights;
			if (v >= w.size() || !w[v]) {
				continue;
			}
			auto p = skin[b].apply(mesh.vertices[v]);
			float wf = w[v] / 255.0f;
			for (int k = 0; k < 3; ++k) {
				acc[k] += p[k] * wf;
			}
			wsum += wf;
		}
		if (wsum > 0) {
			pos[v] = {acc[0] / wsum, acc[1] / wsum, acc[2] / wsum};
		}
	}
	return pos;
}

// World-space positions into the batches, with face-summed normals.
void upload_vertices(std::vector<MeshBatch> &batches, const de::Mesh &mesh, const std::vector<std::array<float, 3>> &pos) {
	std::vector<std::array<float, 3>> vn(pos.size(), {0, 0, 0});
	for (auto &face : mesh.faces) {
		auto &a = pos[face.v[0]], &b = pos[face.v[1]], &c = pos[face.v[2]];
		float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
		for (auto v : face.v) {
			for (int k = 0; k < 3; ++k) {
				vn[v][k] += n[k];
			}
		}
	}
	for (auto &b : batches) {
		for (size_t i = 0; i < b.vertices.size(); ++i) {
			uint32_t v = b.source[i];
			memcpy(b.vertices[i].pos, pos[v].data(), sizeof b.vertices[i].pos);
			memcpy(b.vertices[i].normal, vn[v].data(), sizeof b.vertices[i].normal);
		}
		glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
		glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(b.vertices.size() * sizeof(BatchVertex)), b.vertices.data());
	}
}

}  // namespace

void FighterRenderer::draw_flat() const {
	auto draw = [](const std::vector<MeshBatch> &batches) {
		for (const MeshBatch &b : batches) {
			glBindVertexArray(b.vao);
			glDrawArrays(GL_TRIANGLES, 0, b.count);
		}
	};
	draw(batches_);
	for (const Attachment &at : attachments_) draw(at.batches);
	glBindVertexArray(0);
}

void FighterRenderer::draw_pose(const float *vp, const float *eye, const de::Transform &world, const std::vector<de::Transform> *posep) {
	std::vector<std::array<float, 3>> pos =
	    posep ? skin_vertices(mesh_, bones_, bone_channel_, inv_bind_, *posep, de::Transform{}) : mesh_.vertices;
	for (auto &p : pos) {
		p = world.apply(p);
	}
	upload_vertices(batches_, mesh_, pos);
	use_mesh_program(prog_, vp, eye);
	glUniformMatrix4fv(glGetUniformLocation(prog_, "model"), 1, GL_FALSE, kIdentity);
	last_at_ = world.t;
	draw_batches(prog_, batches_, -1, &world.t, light_owner_);
}

// Every further ActorDeformInfo of the fighter: the model mesh (after the body's, in the model's
// child order) its bones weight, the further AnimationSequenceGroup that has its bones, and the
// body bone above the bones' actors in the fighter's object tree.
void FighterRenderer::load_attachments(const de::World &world, const de::ObjectsFile &f, size_t model, uint32_t body_mesh,
                                       const de::ObjectsFile &fighter, const de::ObjectInfo *body_deform, const de::ObjectInfo *body_bank) {
	const auto &fo = fighter.objects();
	std::map<uint32_t, uint32_t> parent;
	for (uint32_t i = 0; i < fo.size(); ++i) {
		try {
			for (uint32_t c : de::ObjectsFile::children(fighter.object_data(fo[i]))) parent[c] = i;
		} catch (const std::exception &) {
		}
	}
	auto model_children = de::ObjectsFile::children(f.object_data(f.objects()[model]));
	for (auto &d : fo) {
		if (d.class_path != "ActorDeformInfo" || &d == body_deform) continue;
		Attachment at;
		uint32_t mesh_index = UINT32_MAX;
		// The candidates after the body's mesh first, then the rest.
		std::vector<uint32_t> order;
		auto it = std::find(model_children.begin(), model_children.end(), body_mesh);
		if (it != model_children.end()) order.insert(order.end(), it + 1, model_children.end());
		order.insert(order.end(), model_children.begin(), model_children.end());
		for (uint32_t c : order) {
			if (c == body_mesh || f.objects()[c].class_path != "Mesh\\Standard") continue;
			try {
				auto bones = de::read_skin(fighter, d, f, c);
				bool weighted = false;
				for (auto &b : bones) {
					for (auto w : b.weights) weighted |= w != 0;
				}
				if (weighted) {
					at.bones = std::move(bones);
					mesh_index = c;
					break;
				}
			} catch (const std::exception &) {
			}
		}
		if (mesh_index == UINT32_MAX || at.bones.empty()) continue;
		for (auto &b : fo) {
			if (b.class_path != "AnimationSequenceGroup" || &b == body_bank) continue;
			auto bk = de::read_anim_bank(fighter, b);
			if (bk.channel(at.bones[0].name) >= 0) {
				at.bank = std::move(bk);
				break;
			}
		}
		if (at.bank.channels.empty()) continue;
		// Up the tree from the first bone's actor to a body bone.
		for (uint32_t i = 0; i < fo.size(); ++i) {
			if (fo[i].name != at.bones[0].name || fo[i].class_path != "Actor") continue;
			uint32_t below = i;
			for (auto p = parent.find(i); p != parent.end(); p = parent.find(p->second)) {
				int ch = bank.channel(fo[p->second].name);
				if (ch >= 0) {
					at.parent_channel = ch;
					at.root = fo[below].name;
					break;
				}
				below = p->second;
			}
			break;
		}
		if (at.parent_channel < 0) continue;
		at.mesh = de::read_mesh(f, f.objects()[mesh_index]);
		at.materials = de::read_materials(world, {&f, mesh_index});
		for (auto &b : at.bones) {
			at.bone_channel.push_back(at.bank.channel(b.name));
			at.inv_bind.push_back(b.bind.inverse());
		}
		attachments_.push_back(std::move(at));
	}
}

std::vector<std::string> FighterRenderer::attachment_roots() const {
	std::vector<std::string> out;
	for (auto &at : attachments_) out.push_back(at.root);
	return out;
}

void FighterRenderer::draw_attachments(const float *vp, const float *eye, const de::Transform &world, const std::vector<de::Transform> &body,
                                       const AnimState &a) {
	for (size_t i = 0; i < attachments_.size(); ++i) {
		Attachment &at = attachments_[i];
		if (at.parent_channel < 0 || size_t(at.parent_channel) >= body.size()) continue;
		AnimState::Attachment st = i < a.attachments.size() ? a.attachments[i] : AnimState::Attachment{};
		std::vector<de::Transform> pose;
		if (!st.actors.empty()) {
			de::AnimSequence none;
			pose = at.bank.pose(none, 0, true);
			for (auto &[name, t] : st.actors) {
				int ch = at.bank.channel(name);
				if (ch >= 0) pose[size_t(ch)] = t;
			}
		} else if (st.sequence >= 0 && size_t(st.sequence) < at.bank.sequences.size()) {
			const auto &seq = at.bank.sequences[size_t(st.sequence)];
			if (st.activations != at.seen_activations) {
				at.seen_activations = st.activations;
				if (st.prelude) at.captured = at.last_pose;
			}
			if (st.prelude && !at.captured.empty()) {
				pose = de::blend_poses(at.captured, at.bank.pose(seq, st.prelude_time, true), std::clamp(st.t, 0.0f, 1.0f));
			} else {
				pose = at.bank.pose(seq, st.prelude ? st.prelude_time : st.t, true);
			}
		} else {
			// No sequence yet: the bank's rest pose (every channel's rest transform).
			de::AnimSequence none;
			pose = at.bank.pose(none, 0, true);
		}
		at.last_pose = pose;
		auto pos = skin_vertices(at.mesh, at.bones, at.bone_channel, at.inv_bind, pose, body[size_t(at.parent_channel)]);
		for (auto &p : pos) p = world.apply(p);
		upload_vertices(at.batches, at.mesh, pos);
		use_mesh_program(prog_, vp, eye);
		glUniformMatrix4fv(glGetUniformLocation(prog_, "model"), 1, GL_FALSE, kIdentity);
		draw_batches(prog_, at.batches, -1, &world.t, light_owner_);
	}
}

}  // namespace render
