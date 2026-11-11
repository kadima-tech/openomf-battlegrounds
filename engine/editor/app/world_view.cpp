#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>

#include "de/lighting.h"
#include "editor.h"
#include "glow.h"
#include "icons.h"
#include "imgui_internal.h"
#include "pie.h"
#include "render.h"
#include "theme.h"
#include "widgets.h"
#include "world.h"
#include "world_state.h"

using namespace theme;
using Vec3 = std::array<float, 3>;

namespace {

Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3 &a, const Vec3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul(const Vec3 &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3 &a, const Vec3 &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float len(const Vec3 &a) { return std::sqrt(dot(a, a)); }

std::string utf8(const std::string &latin1) { return deo::latin1_to_utf8(deo::Bytes(latin1.begin(), latin1.end())); }

}  // namespace

bool invert4(const float *m, float *out) {
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (std::fabs(det) < 1e-12f) return false;
	for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
	return true;
}

namespace {

std::array<float, 4> clip_of(const float *m, const Vec3 &p) {
	return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
	        m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14], m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15]};
}

unsigned kind_colour(ed::NodeKind k) {
	switch (k) {
	case ed::NodeKind::SpawnPoint: return 0x4fc3f7;
	case ed::NodeKind::PickupSpawner: return 0x8fe3a8;
	case ed::NodeKind::Hazard: return 0xff8f6b;
	case ed::NodeKind::Surface: return 0xb18cff;
	case ed::NodeKind::ForceAffector: return 0x7fd1c7;
	case ed::NodeKind::DeathVolume: return 0xff5a5a;
	case ed::NodeKind::NavArea:
	case ed::NodeKind::NavLink: return 0xf5d36b;
	case ed::NodeKind::Light: return 0xffe08a;
	case ed::NodeKind::Camera: return 0xa9acbd;
	case ed::NodeKind::Effect: return 0xff9bd2;
	case ed::NodeKind::Sound: return 0x8fb8ff;
	default: return 0x9295a6;
	}
}

const char *kind_icon(ed::NodeKind k) {
	switch (k) {
	case ed::NodeKind::Environment: return ICON_MOUNTAIN;
	case ed::NodeKind::SpawnPoint: return ICON_MAP_PIN;
	case ed::NodeKind::PickupSpawner: return ICON_PACKAGE;
	case ed::NodeKind::Hazard: return ICON_FLAME;
	case ed::NodeKind::Surface: return ICON_LAYERS;
	case ed::NodeKind::ForceAffector: return ICON_WIND;
	case ed::NodeKind::DeathVolume: return ICON_TRIANGLE_ALERT;
	case ed::NodeKind::NavArea: return ICON_NAVIGATION;
	case ed::NodeKind::NavLink: return ICON_ROUTE;
	case ed::NodeKind::Light: return ICON_LIGHTBULB;
	case ed::NodeKind::Camera: return ICON_VIDEO;
	case ed::NodeKind::Effect: return ICON_SPARKLES;
	case ed::NodeKind::Sound: return ICON_VOLUME_2;
	case ed::NodeKind::Geometry: return ICON_BOX;
	case ed::NodeKind::Group: return ICON_FOLDER;
	case ed::NodeKind::Affector: return ICON_SCAN;
	case ed::NodeKind::Property: return ICON_COMPONENT;
	case ed::NodeKind::Value: return ICON_HASH;
	default: return ICON_FILE;
	}
}

// A small coloured-vertex renderer for the collision view and the selection outline.
const char *kFlatVs = R"(#version 410 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec4 col;
uniform mat4 vp;
out vec4 v_col;
void main() { gl_Position = vp * vec4(pos, 1.0); v_col = col; }
)";
const char *kFlatFs = R"(#version 410 core
in vec4 v_col;
out vec4 color;
void main() { color = v_col; }
)";

}  // namespace

// ---------------------------------------------------------------------------------------------
// Flat meshes

FlatMesh::~FlatMesh() { clear(); }

void FlatMesh::clear() {
	if (vbo) glDeleteBuffers(1, &vbo);
	if (vao) glDeleteVertexArrays(1, &vao);
	vbo = vao = 0;
	count = 0;
	data.clear();
}

void FlatMesh::vertex(const Vec3 &p, unsigned rgb, float a, float shade) {
	data.insert(data.end(), {p[0], p[1], p[2], float((rgb >> 16) & 255) / 255.0f * shade, float((rgb >> 8) & 255) / 255.0f * shade,
	                         float(rgb & 255) / 255.0f * shade, a});
}

void FlatMesh::upload() {
	if (!vao) glGenVertexArrays(1, &vao), glGenBuffers(1, &vbo);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<void *>(3 * sizeof(float)));
	glBindVertexArray(0);
	count = GLsizei(data.size() / 7);
	data.clear();
	data.shrink_to_fit();
}

void FlatMesh::draw(const float *vp, GLenum mode, bool depth_test, bool blend) const {
	static GLuint prog = 0;
	if (!prog) prog = glu::program(kFlatVs, kFlatFs);
	if (!vao || !count) return;
	glUseProgram(prog);
	glUniformMatrix4fv(glGetUniformLocation(prog, "vp"), 1, GL_FALSE, vp);
	if (depth_test) glEnable(GL_DEPTH_TEST);
	else glDisable(GL_DEPTH_TEST);
	glDepthMask(mode == GL_TRIANGLES ? GL_TRUE : GL_FALSE);
	glDisable(GL_CULL_FACE);
	if (blend) glEnable(GL_BLEND), glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	else glDisable(GL_BLEND);
	if (mode == GL_TRIANGLES) glEnable(GL_POLYGON_OFFSET_FILL), glPolygonOffset(1, 1);
	glBindVertexArray(vao);
	glDrawArrays(mode, 0, count);
	glBindVertexArray(0);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDepthMask(GL_TRUE);
}

// ---------------------------------------------------------------------------------------------
// Cameras

std::array<float, 3> Camera::forward() const {
	return {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

void Camera::matrices(int w, int h, float *proj, float *view, float *eye) const {
	const float aspect = float(std::max(w, 1)) / float(std::max(h, 1));
	if (mode == Perspective) {
		glu::perspective(proj, 60.0f, aspect, 4.0f, 60000.0f);
		glu::fps_view(view, pos.data(), yaw, pitch);
		memcpy(eye, pos.data(), 12);
		return;
	}
	const float hh = half_height, hw = half_height * aspect, n = 1, f = 80000;
	float o[16] = {1 / hw, 0, 0, 0, 0, 1 / hh, 0, 0, 0, 0, -2 / (f - n), 0, 0, 0, -(f + n) / (f - n), 1};
	memcpy(proj, o, sizeof o);
	Vec3 fwd, up, e;
	if (mode == Top) fwd = {0, -1, 0}, up = {0, 0, -1}, e = add(centre, {0, 40000, 0});
	else if (mode == Front) fwd = {0, 0, -1}, up = {0, 1, 0}, e = add(centre, {0, 0, 40000});
	else fwd = {-1, 0, 0}, up = {0, 1, 0}, e = add(centre, {40000, 0, 0});
	glu::look_view(view, e.data(), fwd.data(), up.data());
	memcpy(eye, e.data(), 12);
}

// ---------------------------------------------------------------------------------------------
// Loading

WorldEditor::WorldEditor(Editor &e) : e_(e) {
	panes_[1].cam.mode = Camera::Top;
	panes_[2].cam.mode = Camera::Front;
	panes_[3].cam.mode = Camera::Side;
}

WorldEditor::~WorldEditor() {
	play_.reset();
	close_arena();
	for (auto &p : panes_) {
		if (p.fbo) glDeleteFramebuffers(1, &p.fbo), glDeleteTextures(1, &p.tex), glDeleteRenderbuffers(1, &p.depth);
	}
}

WorldEditor::ArenaState::~ArenaState() {
	if (loader.joinable()) loader.join();
}

bool WorldEditor::has_arena() const { return arena_ != nullptr; }
const ed::ArenaEntry *WorldEditor::arena() const { return arena_ ? &arena_->entry : nullptr; }

void WorldEditor::close_arena() { arena_.reset(); }

void WorldEditor::open_arena(const ed::ArenaEntry &a) {
	if (!e_.base()) return;
	if (arena_ && arena_->entry.description == a.description && arena_->entry.addon == a.addon) return;
	arena_.reset();
	auto st = std::make_unique<ArenaState>();
	st->entry = a;
	st->started = ImGui::GetTime();
	const ed::BaseData *base = e_.base();
	std::vector<ed::Guid> pool;
	if (a.addon && e_.project()) pool = e_.project()->file().guid_pool;
	ArenaState *s = st.get();
	std::string stem = a.stem;
	s->loader = std::thread([s, base, pool, stem] {
		try {
			deo::File pool_file;
			pool_file.guid_pool = pool;
			auto stems = ed::arena_files(*base, stem, pool.empty() ? nullptr : &pool_file);
			std::set<std::string> keep(stems.begin(), stems.end());
			s->world = std::make_unique<de::World>(base->dir(), [&](const std::string &st) { return keep.count(st) > 0; });
			const de::ObjectsFile *f = s->world->file(stem);
			if (!f) throw std::runtime_error("no arena file " + stem);
			for (auto &item : de::collect_scene(*s->world, *f)) {
				PickMesh pm;
				pm.actor = item.actor_index;
				for (auto &face : item.mesh.faces) {
					for (int k = 0; k < 3; ++k) {
						if (face.v[size_t(k)] >= item.mesh.vertices.size()) continue;
						pm.tris.push_back(item.world.apply(item.mesh.vertices[face.v[size_t(k)]]));
					}
				}
				pm.tris.resize(pm.tris.size() / 3 * 3);
				for (auto &v : pm.tris) {
					for (int k = 0; k < 3; ++k) pm.lo[size_t(k)] = std::min(pm.lo[size_t(k)], v[size_t(k)]), pm.hi[size_t(k)] = std::max(pm.hi[size_t(k)], v[size_t(k)]);
				}
				if (!pm.tris.empty()) s->picks.push_back(std::move(pm));
			}
		} catch (const std::exception &ex) {
			s->error = ex.what();
		}
		s->loaded = true;
	});
	if (e_.sync_loads()) s->loader.join();
	arena_ = std::move(st);
	e_.log(LogLine::Info, "Opening arena " + a.name + (a.addon ? " (add-on variant of " + a.stem + ")" : " (" + a.stem + ", read-only)"));
}

void WorldEditor::finish_load() {
	ArenaState &s = *arena_;
	if (s.built || !s.loaded) return;
	if (s.loader.joinable()) s.loader.join();
	s.built = true;
	if (!s.error.empty()) {
		e_.log(LogLine::Error, "Cannot open the arena: " + s.error);
		return;
	}
	auto saved_live = render::g_live_values;
	auto saved_ambient = render::g_lighting.ambient;
	render::g_reflections_enabled = true;
	s.renderer = std::make_unique<render::ArenaRenderer>();
	s.renderer->load(*s.world, s.entry.stem);
	s.ambient = render::g_lighting.ambient;
	render::g_lighting.ambient = saved_ambient;
	render::g_live_values = saved_live;
	s.glow = std::make_unique<render::GlowRenderer>();
	refresh_scene();
	// Frame the play area (spawns, pickups, nav), not the skybox and sun.
	std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	for (auto &n : s.scene.nodes) {
		if (n.kind != ed::NodeKind::SpawnPoint && n.kind != ed::NodeKind::PickupSpawner && n.kind != ed::NodeKind::NavArea) continue;
		for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], n.world.t[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], n.world.t[size_t(k)]);
	}
	if (lo[0] > hi[0]) lo = {-800, 0, -800}, hi = {800, 200, 800};
	for (auto &p : panes_) {
		p.cam.centre = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
		p.cam.half_height = std::max(400.0f, std::max(hi[0] - lo[0], hi[2] - lo[2]) * 0.62f);
	}
	panes_[0].cam.pos = {0, 650, 1250};
	panes_[0].cam.yaw = 0, panes_[0].cam.pitch = 0.42f;
	e_.log(LogLine::Ok, "Arena " + s.entry.name + ": " + std::to_string(s.renderer->triangles) + " triangles, " + std::to_string(s.scene.nodes.size()) +
	                        " objects, " + std::to_string(s.scene.of_kind(ed::NodeKind::SpawnPoint).size()) + " spawn points");
}

void WorldEditor::refresh_scene() {
	ArenaState &s = *arena_;
	if (!s.world) return;
	ed::Project *p = e_.project();
	const de::ObjectsFile *file = nullptr;
	if (s.entry.addon) {
		if (!p) return;
		try {
			s.addon_file = std::make_unique<de::ObjectsFile>(p->bytes());
		} catch (const std::exception &ex) {
			e_.log(LogLine::Error, std::string("Cannot read the add-on for the viewport: ") + ex.what());
			return;
		}
		s.addon_revision = p->revision();
		file = s.addon_file.get();
	} else {
		file = s.world->file(s.entry.stem);
	}
	if (!file) return;
	int root = -1;
	for (uint32_t i = 0; i < file->objects().size(); ++i) {
		if (memcmp(file->objects()[i].guid.bytes, s.entry.environment.b, 16) == 0) root = int(i);
	}
	if (root < 0) {
		e_.log(LogLine::Error, "The arena's environment is missing from " + file->master().name);
		return;
	}
	Vec3 keep_pos{0, 0, 0};
	ed::Guid keep_guid;
	if (s.selected >= 0 && s.selected < int(s.scene.nodes.size())) keep_guid = s.scene.nodes[size_t(s.selected)].guid;
	(void)keep_pos;
	s.scene = ed::read_arena_scene(*s.world, *file, uint32_t(root));
	s.selected = -1;
	for (size_t i = 0; i < s.scene.nodes.size(); ++i) {
		if (s.scene.nodes[i].guid == keep_guid) s.selected = int(i);
	}
	// Position-static, as DE_CActor::IsPositionStatic: the nearest set position mode up the tree is 1.
	s.position_static.assign(s.scene.nodes.size(), false);
	for (size_t i = 0; i < s.scene.nodes.size(); ++i) {
		for (int k = int(i); k >= 0; k = s.scene.nodes[size_t(k)].parent) {
			const auto &n = s.scene.nodes[size_t(k)];
			if (n.actor && n.info.position_mode != 0) {
				s.position_static[i] = n.info.position_mode == 1;
				break;
			}
		}
	}
	s.collision.clear();
	s.collision_lines.clear();
	for (auto &m : s.kind_lines) m.clear();
	for (const auto &n : s.scene.nodes) {
		if (n.triangles.empty()) continue;
		unsigned c = kind_colour(n.kind);
		for (size_t t = 0; t + 2 < n.triangles.size(); t += 3) {
			Vec3 nn = cross(sub(n.triangles[t + 1], n.triangles[t]), sub(n.triangles[t + 2], n.triangles[t]));
			float l = len(nn);
			float shade = l > 0 ? 0.45f + 0.55f * std::fabs(nn[1] / l) : 1;
			for (int k = 0; k < 3; ++k) s.collision.vertex(n.triangles[t + size_t(k)], c, 1, shade);
			for (int k = 0; k < 3; ++k) {
				s.collision_lines.vertex(n.triangles[t + size_t(k)], c, 0.55f);
				s.collision_lines.vertex(n.triangles[t + size_t((k + 1) % 3)], c, 0.55f);
			}
			FlatMesh *lit = n.kind == ed::NodeKind::Surface ? &s.kind_lines[0]
			                : n.kind == ed::NodeKind::ForceAffector || n.kind == ed::NodeKind::DeathVolume ? &s.kind_lines[1]
			                : n.kind == ed::NodeKind::Hazard ? &s.kind_lines[2] : nullptr;
			if (lit) {
				for (int k = 0; k < 3; ++k) lit->vertex(n.triangles[t + size_t(k)], c, 0.22f), lit->vertex(n.triangles[t + size_t((k + 1) % 3)], c, 0.22f);
			}
		}
	}
	s.collision.upload();
	s.collision_lines.upload();
	for (auto &m : s.kind_lines) m.upload();
	s.issues = ed::validate_arena(s.scene, s.max_players());
}

int WorldEditor::ArenaState::max_players() const { return description.max_players; }

void WorldEditor::frame() {
	if (arena_) {
		finish_load();
		ed::Project *p = e_.project();
		if (arena_->entry.addon) {
			if (!p) {
				close_arena();
			} else if (arena_->built && arena_->world && p->revision() != arena_->addon_revision && !arena_->dragging && !ImGui::IsAnyItemActive()) {
				refresh_scene();
			}
		}
		if (arena_ && arena_->entry.addon && p) {
			int d = p->find(arena_->entry.description);
			if (d >= 0) {
				try {
					arena_->description = ed::read_arena_description(*p, uint32_t(d));
				} catch (const std::exception &) {
				}
			}
		} else if (arena_ && !arena_->entry.addon && e_.base() && arena_->description.name.empty()) {
			arena_->description.name = arena_->entry.name;
			if (const ed::BaseObject *b = e_.base()->by_guid(arena_->entry.description)) {
				const deo::File &f = e_.base()->model(b->file);
				deo::File tmp;
				(void)tmp;
				auto &o = f.objects[b->index];
				ed::Chunks dc = ed::split_chunks(o.contents);
				if (dc.chunks.size() > 1 && dc.chunks[1].body.size() >= 8) {
					memcpy(&arena_->description.min_players, dc.chunks[1].body.data(), 4);
					memcpy(&arena_->description.max_players, dc.chunks[1].body.data() + 4, 4);
				}
			}
		}
		// Keep the selection in step with the editor's (Content Browser, Details links).
		if (arena_ && arena_->built) {
			const ObjectRef &sel = e_.selection();
			int now = arena_->selected;
			if (!(now >= 0 && node_ref(now) == sel)) {
				arena_->selected = -1;
				for (size_t i = 0; i < arena_->scene.nodes.size(); ++i) {
					if (sel && arena_->scene.nodes[i].guid == sel.guid) arena_->selected = int(i);
				}
			}
		}
	}
	run_script();
	play_frame();
}

void WorldEditor::run_script() {
	const Options &o = e_.options();
	if (script_step_ < 0) return;
	if (script_step_ == 0) {
		if (o.layout) layout_ = o.layout;
		if (o.shading >= 0) panes_[0].shading = o.shading;
		for (const char *f : {"hits", "nav", "energy"}) {
			if (o.debug.find(f) == std::string::npos) continue;
			if (!strcmp(f, "hits")) debug_.hit_spheres = true;
			if (!strcmp(f, "nav")) debug_.nav = true;
			if (!strcmp(f, "energy")) debug_.full_energy = true;
		}
		if (o.arena.empty()) {
			script_step_ = o.play ? 2 : -1;
			if (o.play) start_play(o.simulate);
			return;
		}
		open_arena_named(o.arena);
		script_step_ = 1;
	}
	if (script_step_ == 1) {
		if (!arena_ || !arena_->built) return;
		if (o.variant && !arena_->entry.addon) {
			copy_to_addon();
			return;
		}
		if (arena_->entry.addon && !arena_->built) return;
		if (!o.camera.empty()) {
			float v[5];
			if (sscanf(o.camera.c_str(), "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) set_camera(0, {v[0], v[1], v[2]}, v[3], v[4]);
		}
		if (!o.select_node.empty()) select_named(o.select_node), ImGui::SetWindowFocus("###Details");
		ed::Project *p = e_.project();
		if (!o.move_to.empty() && arena_->selected >= 0 && arena_->entry.addon && p) {
			float v[3];
			if (sscanf(o.move_to.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) == 3) {
				const auto &n = arena_->scene.nodes[size_t(arena_->selected)];
				de::Transform w = n.world;
				w.t = {v[0], v[1], v[2]};
				de::Transform parent = n.parent >= 0 ? arena_->scene.nodes[size_t(n.parent)].world : de::Transform{};
				if (int obj = p->find(n.guid); obj >= 0) ed::set_actor_local(*p, uint32_t(obj), parent.inverse().then(w));
				refresh_scene();
			}
		}
		if (!o.light_colour.empty() && arena_->entry.addon && p) {
			float v[4];
			if (sscanf(o.light_colour.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) == 4) {
				for (int k : arena_->scene.of_kind(ed::NodeKind::Light)) {
					const auto &n = arena_->scene.nodes[size_t(k)];
					if (n.class_path != "Light") continue;
					int obj = p->find(n.guid);
					ed::LightData d;
					if (obj >= 0 && ed::read_light_data(p->file().objects[size_t(obj)], &d)) {
						d.rgba = {v[0], v[1], v[2], 1}, d.intensity = v[3];
						ed::write_light_data(*p, uint32_t(obj), d);
						break;
					}
				}
				refresh_scene();
			}
		}
		if (o.save) e_.save();
		script_step_ = o.play ? 2 : -1;
		if (o.play) start_play(o.simulate);
		return;
	}
	if (script_step_ == 2) {
		if (!play_) {
			script_step_ = -1;
			return;
		}
		if (!play_->ready()) return;
		if (o.play_ticks > 0) play_->run_ticks(o.play_ticks);
		if (o.replay && !replayed_) {
			// Stop and Play again in this process: the second match must run like the first.
			e_.log(LogLine::Info, "Play: " + play_->summary());
			replayed_ = true;
			stop_play();
			start_play(o.simulate);
			return;
		}
		if (o.eject) {
			play_->set_ejected(true);
			if (!o.camera.empty()) {
				float v[5];
				if (sscanf(o.camera.c_str(), "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
					play_->eject_camera().pos = {v[0], v[1], v[2]};
					play_->eject_camera().yaw = v[3], play_->eject_camera().pitch = v[4];
				}
			}
		}
		if (o.inspector) ImGui::SetWindowFocus("###LiveInspector");
		e_.log(LogLine::Info, "Play: " + play_->summary());
		script_step_ = -1;
	}
}

std::string WorldEditor::play_mode_label() const { return settings_.robot + " vs " + settings_.opponent + (settings_.opponent_ai ? "" : " (dummy)"); }

bool WorldEditor::open(const ObjectRef &r) {
	if (!e_.base() || !r) return false;
	std::string cls;
	if (r.kind == ObjectRef::Base) cls = e_.base()->files()[r.file].objects[r.index].class_path;
	else if (int i = e_.addon_index(r); i >= 0) cls = e_.project()->file().objects[size_t(i)].info.class_text();
	if (cls != "Description\\Environment" && cls != "Actor\\Creature\\Environment") return false;
	std::vector<ed::ArenaEntry> all = ed::base_arenas(*e_.base());
	if (e_.project()) {
		auto mine = ed::addon_arenas(*e_.project());
		all.insert(all.end(), mine.begin(), mine.end());
	}
	for (auto &a : all) {
		if (a.description == r.guid || a.environment == r.guid) {
			open_arena(a);
			return true;
		}
	}
	return false;
}

void WorldEditor::open_arena_named(const std::string &name) {
	if (!e_.base()) return;
	std::vector<ed::ArenaEntry> all = ed::base_arenas(*e_.base());
	if (e_.project()) {
		auto mine = ed::addon_arenas(*e_.project());
		all.insert(all.begin(), mine.begin(), mine.end());
	}
	for (auto &a : all) {
		if (a.name == name || a.stem == name) {
			open_arena(a);
			return;
		}
	}
	e_.log(LogLine::Warning, "No arena named " + name);
}

void WorldEditor::copy_to_addon() {
	if (!arena_ || arena_->entry.addon) return;
	ed::ArenaEntry base = arena_->entry;
	e_.with_addon("Copying " + base.name + " needs an add-on to put the variant in. Which one?", [this, base] {
		ed::Project *p = e_.project();
		if (!p) return;
		try {
			uint32_t d = ed::create_arena_variant(*p, base, base.name + " Variant");
			for (auto &a : ed::addon_arenas(*p)) {
				if (a.description == p->file().objects[d].info.guid) {
					arena_.reset();
					open_arena(a);
					break;
				}
			}
			e_.log(LogLine::Ok, "New arena variant \"" + base.name + " Variant\" (a copy of " + base.name + "'s description and environment, fresh GUIDs)");
		} catch (const std::exception &ex) {
			e_.log(LogLine::Error, std::string("Cannot copy the arena: ") + ex.what());
		}
	});
}

ObjectRef WorldEditor::node_ref(int node) const {
	if (!arena_ || node < 0 || node >= int(arena_->scene.nodes.size())) return {};
	const ed::SceneNode &n = arena_->scene.nodes[size_t(node)];
	if (arena_->entry.addon) return ObjectRef::addon(n.guid);
	return ObjectRef{ObjectRef::Base, size_t(arena_->entry.environment_file), n.index, n.guid};
}

int WorldEditor::selected_node() const { return arena_ ? arena_->selected : -1; }

void WorldEditor::select_node(int node) {
	if (!arena_) return;
	arena_->selected = node;
	e_.select(node >= 0 ? node_ref(node) : ObjectRef{});
}

void WorldEditor::select_named(const std::string &name) {
	if (!arena_) return;
	for (size_t i = 0; i < arena_->scene.nodes.size(); ++i) {
		if (arena_->scene.nodes[i].name == name) {
			select_node(int(i));
			return;
		}
	}
}

void WorldEditor::set_camera(int pane, const Vec3 &pos, float yaw, float pitch) {
	panes_[size_t(pane)].cam.pos = pos, panes_[size_t(pane)].cam.yaw = yaw, panes_[size_t(pane)].cam.pitch = pitch;
}

void WorldEditor::frame_selection(int pi) {
	if (!arena_) return;
	Pane &p = panes_[size_t(pi)];
	Vec3 c;
	float r = 300;
	if (arena_->selected >= 0) {
		const auto &n = arena_->scene.nodes[size_t(arena_->selected)];
		c = n.world.t;
		if (n.shape == ed::SceneNode::Shape::Sphere) r = std::max(r, n.falloff * 1.5f);
		if (!n.triangles.empty()) {
			Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
			for (auto &v : n.triangles) {
				for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], v[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], v[size_t(k)]);
			}
			c = mul(add(lo, hi), 0.5f);
			r = std::max(r, len(sub(hi, lo)) * 0.6f);
		}
	} else {
		c = mul(add(arena_->scene.lo, arena_->scene.hi), 0.5f);
		r = std::max(600.0f, len(sub(arena_->scene.hi, arena_->scene.lo)) * 0.35f);
	}
	if (p.cam.mode == Camera::Perspective) {
		Vec3 f = p.cam.forward();
		p.cam.pos = sub(c, mul(f, r * 2.2f));
	} else {
		p.cam.centre = c;
		p.cam.half_height = r * 1.3f;
	}
}

// ---------------------------------------------------------------------------------------------
// Viewports

bool WorldEditor::draw_viewport() {
	if (!arena_ && !play_) return false;
	ImVec2 p0 = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
	if (arena_ && !arena_->built) {
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilledMultiColor(p0, p0 + size, rgb(0x1b1c26), rgb(0x1b1c26), rgb(0x101116), rgb(0x101116));
		ImVec2 card(std::min(size.x - ui::px(40), ui::px(440)), ui::px(130));
		ImVec2 c0 = p0 + (size - card) * 0.5f;
		dl->AddRectFilled(c0, c0 + card, rgb(0x1d1e26, 235), ui::px(8));
		dl->AddRect(c0, c0 + card, rgb(kBorder), ui::px(8));
		ImGui::SetCursorScreenPos(c0);
		ImGui::BeginChild("##loading", card, 0, ImGuiWindowFlags_NoBackground);
		std::string body = "Reading the files " + arena_->entry.stem + " needs (" + std::to_string(int(ImGui::GetTime() - arena_->started)) + " s)...";
		ui::centered_note(ICON_MOUNTAIN, ("Opening " + arena_->entry.name).c_str(), body.c_str());
		ImGui::EndChild();
		return true;
	}
	const float gap = ui::px(2);
	std::vector<std::pair<ImVec2, ImVec2>> rects;
	if (layout_ == 1) {
		rects.push_back({p0, size});
	} else if (layout_ == 2) {
		float w = (size.x - gap) / 2;
		rects.push_back({p0, ImVec2(w, size.y)});
		rects.push_back({p0 + ImVec2(w + gap, 0), ImVec2(w, size.y)});
	} else {
		float w = (size.x - gap) / 2, h = (size.y - gap) / 2;
		rects.push_back({p0 + ImVec2(w + gap, h + gap), ImVec2(w, h)});
		rects.push_back({p0, ImVec2(w, h)});
		rects.push_back({p0 + ImVec2(w + gap, 0), ImVec2(w, h)});
		rects.push_back({p0 + ImVec2(0, h + gap), ImVec2(w, h)});
	}
	if (active_pane_ >= int(rects.size())) active_pane_ = 0;
	for (size_t i = 0; i < rects.size(); ++i) draw_pane(int(i), rects[i].first, rects[i].second);
	return true;
}

void bind_pane(Pane &p, int w, int h, GLint *prev_fbo, GLint *prev_vp) {
	if (!p.fbo || p.tw != w || p.th != h) {
		if (!p.fbo) glGenFramebuffers(1, &p.fbo), glGenTextures(1, &p.tex), glGenRenderbuffers(1, &p.depth);
		glBindTexture(GL_TEXTURE_2D, p.tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindRenderbuffer(GL_RENDERBUFFER, p.depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
		GLint cur = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
		glBindFramebuffer(GL_FRAMEBUFFER, p.fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.tex, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, p.depth);
		glBindFramebuffer(GL_FRAMEBUFFER, GLuint(cur));
		p.tw = w, p.th = h;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, prev_fbo);
	glGetIntegerv(GL_VIEWPORT, prev_vp);
	glBindFramebuffer(GL_FRAMEBUFFER, p.fbo);
	glViewport(0, 0, w, h);
}

void WorldEditor::render_pane(Pane &p) {
	ArenaState &s = *arena_;
	const int w = std::max(16, int(p.size.x * ImGui::GetIO().DisplayFramebufferScale.x));
	const int h = std::max(16, int(p.size.y * ImGui::GetIO().DisplayFramebufferScale.y));
	GLint prev_fbo = 0, prev_vp[4];
	bind_pane(p, w, h, &prev_fbo, prev_vp);
	p.cam.matrices(w, h, p.proj, p.view, p.eye);
	glu::mul(p.vp, p.proj, p.view);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	if (p.shading == 2) glClearColor(0.07f, 0.075f, 0.09f, 1);
	else glClearColor(0.31f, 0.33f, 0.38f, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	if (s.renderer && p.shading != 2) {
		auto saved_live = render::g_live_values;
		auto saved_lights = render::g_lighting;
		const de::ObjectsFile *arena_file = s.world->file(s.entry.stem);
		const float scene_ms = float(ImGui::GetTime() * 1000.0);
		render::g_live_values = [arena_file, scene_ms](const de::ObjectRef &r) -> std::optional<float> {
			if (r.file == arena_file && r.info().name == "SceneTime") return scene_ms;
			return std::nullopt;
		};
		render::g_lighting.lights.clear();
		render::g_lighting.ambient = p.shading == 1 ? std::array<float, 3>{1, 1, 1} : s.ambient;
		render::g_lighting.serial = 0x80000000u + uint32_t(++s.light_serial);
		render::g_lighting.renderer = 77;
		render::g_lighting.scene = nullptr;
		if (p.shading == 0) {
			for (size_t i = 0; i < s.scene.nodes.size(); ++i) {
				const ed::SceneNode &n = s.scene.nodes[i];
				if (!n.light || n.parent < 0) continue;
				const ed::SceneNode &a = s.scene.nodes[size_t(n.parent)];
				if (a.shape != ed::SceneNode::Shape::Sphere && a.shape != ed::SceneNode::Shape::Cone) continue;
				render::SceneLight l;
				l.frame = a.world;
				l.pos = a.world.t;
				if (s.dragging && s.drag_node == n.parent) l.frame = s.drag_world, l.pos = s.drag_world.t;
				l.shape = a.shape == ed::SceneNode::Shape::Cone ? render::SceneLight::kCone : render::SceneLight::kSphere;
				if (l.shape == render::SceneLight::kSphere) l.inner = a.hotspot, l.outer = a.falloff;
				else l.inner = a.far_begin, l.outer = a.far_end, l.umbra = a.umbra, l.penumbra = a.penumbra, l.near_begin = a.near_begin, l.near_end = a.near_end;
				l.position_static = s.position_static[size_t(n.parent)];
				auto c = n.light->colour.eval(0);
				l.colour = {c[0] * c[3], c[1] * c[3], c[2] * c[3]};
				l.intensity = n.light->intensity;
				l.mask = n.light->mask();
				l.name = n.name;
				render::g_lighting.lights.push_back(l);
			}
		}
		s.renderer->draw(p.vp, p.eye);
		s.glow->begin();
		s.renderer->draw_glow(p.vp, p.eye);
		s.glow->end(w, h);
		render::g_live_values = saved_live;
		render::g_lighting = saved_lights;
	}
	if (p.shading == 2) {
		s.collision.draw(p.vp, GL_TRIANGLES, true, false);
		s.collision_lines.draw(p.vp, GL_LINES, true, true);
	} else {
		// Collision groups over the lit view: depth-tested wire.
		if (show_.surfaces) s.kind_lines[0].draw(p.vp, GL_LINES, true, true);
		if (show_.forces || show_.death) s.kind_lines[1].draw(p.vp, GL_LINES, true, true);
		if (show_.hazards) s.kind_lines[2].draw(p.vp, GL_LINES, true, true);
	}
	// The selected geometry's outline.
	if (s.selected >= 0) {
		const ed::SceneNode &n = s.scene.nodes[size_t(s.selected)];
		if (s.outline_node != s.selected) {
			s.outline.clear();
			s.outline_node = s.selected;
			std::set<uint32_t> actors;
			std::function<void(int)> collect = [&](int k) {
				actors.insert(s.scene.nodes[size_t(k)].index);
				for (int c : s.scene.nodes[size_t(k)].children) collect(c);
			};
			collect(s.selected);
			if (!s.entry.addon) {
				for (auto &pm : s.picks) {
					if (!actors.count(pm.actor)) continue;
					for (size_t t = 0; t + 2 < pm.tris.size(); t += 3) {
						for (int k = 0; k < 3; ++k) s.outline.vertex(pm.tris[t + size_t(k)], 0xff9f3a, 0.9f), s.outline.vertex(pm.tris[t + size_t((k + 1) % 3)], 0xff9f3a, 0.9f);
					}
				}
			}
			for (size_t t = 0; t + 2 < n.triangles.size(); t += 3) {
				for (int k = 0; k < 3; ++k) s.outline.vertex(n.triangles[t + size_t(k)], 0xff9f3a, 0.9f), s.outline.vertex(n.triangles[t + size_t((k + 1) % 3)], 0xff9f3a, 0.9f);
			}
			s.outline.upload();
		}
		s.outline.draw(p.vp, GL_LINES, false, true);
	}
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
	glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
}

void WorldEditor::draw_pane(int i, ImVec2 p0, ImVec2 size) {
	Pane &p = panes_[size_t(i)];
	p.p0 = p0, p.size = size;
	ImGui::SetCursorScreenPos(p0);
	ImGui::PushID(i);
	ImGui::InvisibleButton("##pane", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	p.hovered = ImGui::IsItemHovered();
	const bool playing_here = play_ && i == active_pane_;
	if (playing_here) play_->render(p);
	else if (arena_ && arena_->renderer) render_pane(p);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	if ((playing_here || (arena_ && arena_->renderer)) && p.tex) dl->AddImage(ImTextureID(intptr_t(p.tex)), p0, p0 + size, ImVec2(0, 1), ImVec2(1, 0));
	else dl->AddRectFilled(p0, p0 + size, rgb(0x121318));
	dl->PushClipRect(p0, p0 + size, true);
	if (arena_ && arena_->built && (!playing_here || play_->ejected())) draw_overlays(i);
	if (playing_here) play_->draw_overlays(p);
	bool gizmo_used = arena_ && arena_->built && !playing_here && gizmo(i);
	if (!gizmo_used) pane_input(i);
	// Axis triad.
	{
		ImVec2 o(p0.x + ui::px(30), p0.y + size.y - ui::px(30));
		auto axis = [&](const Vec3 &d, unsigned c, const char *l) {
			float x = p.view[0] * d[0] + p.view[4] * d[1] + p.view[8] * d[2], y = p.view[1] * d[0] + p.view[5] * d[1] + p.view[9] * d[2];
			ImVec2 e = o + ImVec2(x, -y) * ui::px(20);
			dl->AddLine(o, e, rgb(c), ui::px(2));
			dl->AddText(e + ImVec2(2, -7), rgb(c), l);
		};
		axis({1, 0, 0}, 0xff5a5a, "X");
		axis({0, 1, 0}, 0x5ad16b, "Y");
		axis({0, 0, 1}, 0x4f9cff, "Z");
	}
	if (playing_here) {
		float t = ui::px(play_->ejected() ? 2 : 3);
		dl->AddRect(p0 + ImVec2(t / 2, t / 2), p0 + size - ImVec2(t / 2, t / 2), rgb(play_->ejected() ? 0xffc98a : 0x2fbf5e), 0, 0, t);
	} else if (layout_ > 1 && i == active_pane_) {
		dl->AddRect(p0, p0 + size, rgb(kAccentLine), 0, 0, ui::px(1.5f));
	}
	dl->PopClipRect();
	pane_header(i);
	ImGui::PopID();
}

void WorldEditor::pane_header(int i) {
	Pane &p = panes_[size_t(i)];
	ImGui::SetCursorScreenPos(p.p0 + ImVec2(ui::px(8), ui::px(8)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(5), 0));
	if (ui::overlay_button(ICON_LAYOUT_GRID)) ImGui::OpenPopup("layout");
	if (ImGui::BeginPopup("layout")) {
		if (ImGui::MenuItem("1 Viewport", nullptr, layout_ == 1)) layout_ = 1;
		if (ImGui::MenuItem("2 Viewports", nullptr, layout_ == 2)) layout_ = 2;
		if (ImGui::MenuItem("4 Viewports", nullptr, layout_ == 4)) layout_ = 4;
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	static const char *const modes[] = {"Perspective", "Top", "Front", "Side"};
	if (ui::overlay_button(modes[p.cam.mode])) ImGui::OpenPopup("mode");
	if (ImGui::BeginPopup("mode")) {
		for (int m = 0; m < 4; ++m) {
			if (ImGui::MenuItem(modes[m], nullptr, p.cam.mode == m)) p.cam.mode = Camera::Mode(m);
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	static const char *const shades[] = {"Lit", "Unlit", "Collision"};
	if (ui::overlay_button(shades[p.shading])) ImGui::OpenPopup("shade");
	if (ImGui::BeginPopup("shade")) {
		for (int m = 0; m < 3; ++m) {
			if (ImGui::MenuItem(shades[m], nullptr, p.shading == m)) p.shading = m;
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	if (ui::overlay_button("Show " ICON_CHEVRON_DOWN)) ImGui::OpenPopup("show");
	if (ImGui::BeginPopup("show")) {
		ImGui::TextDisabled("GAMEPLAY");
		ImGui::MenuItem(ICON_LAYERS "  Fighting Surfaces", nullptr, &show_.surfaces);
		ImGui::MenuItem(ICON_WIND "  Force Affectors", nullptr, &show_.forces);
		ImGui::MenuItem(ICON_TRIANGLE_ALERT "  Death Volumes", nullptr, &show_.death);
		ImGui::MenuItem(ICON_MAP_PIN "  Spawn Points", nullptr, &show_.spawns);
		ImGui::MenuItem(ICON_PACKAGE "  Pickup Spawners", nullptr, &show_.pickups);
		ImGui::MenuItem(ICON_ROUTE "  Nav Links", nullptr, &show_.nav);
		ImGui::MenuItem(ICON_FLAME "  Hazards", nullptr, &show_.hazards);
		ImGui::Separator();
		ImGui::TextDisabled("ENVIRONMENT");
		ImGui::MenuItem(ICON_LIGHTBULB "  Lights", nullptr, &show_.lights);
		ImGui::MenuItem(ICON_VIDEO "  Cameras", nullptr, &show_.cameras);
		ImGui::MenuItem(ICON_SCAN "  Other Affectors", nullptr, &show_.affectors);
		ImGui::Separator();
		ImGui::MenuItem(ICON_GRID_3X3 "  Grid", nullptr, &show_.grid);
		ImGui::MenuItem(ICON_FILE_TEXT "  Labels", nullptr, &show_.labels);
		ImGui::EndPopup();
	}
	if (arena_ && arena_->built && p.size.x > ui::px(900) && !(play_ && i == active_pane_ && !play_->ejected())) {
		ImGui::SameLine(0, ui::px(10));
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui::px(4));
		std::string t = arena_->entry.name + (arena_->entry.addon ? "  (Variant)" : "  (Base, read-only)");
		ImGui::GetWindowDrawList()->AddText(ImGui::GetCursorScreenPos(), rgb(0xd3d5e0, 200), t.c_str());
	}
	// Right side: gizmo, snapping, maximise.
	if ((i == 0 || layout_ == 1) && !(play_ && i == active_pane_)) {
		float right = ui::px(8);
		const char *snap_label = snap_ ? ICON_MAGNET " 10" : ICON_MAGNET;
		for (const char *b : {ICON_MOVE, ICON_ROTATE_CW, snap_label, ICON_MAXIMIZE}) right += ImGui::CalcTextSize(b).x + ui::px(16 + 5);
		ImGui::SetCursorScreenPos(ImVec2(p.p0.x + p.size.x - right, p.p0.y + ui::px(8)));
		if (ui::overlay_button(ICON_MOVE, gizmo_mode_ == 0)) gizmo_mode_ = 0;
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Move (W)");
		ImGui::SameLine();
		if (ui::overlay_button(ICON_ROTATE_CW, gizmo_mode_ == 1)) gizmo_mode_ = 1;
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Rotate (E)");
		ImGui::SameLine();
		if (ui::overlay_button(snap_label, snap_)) snap_ = !snap_;
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Snap to 10 units and 15 degrees");
		ImGui::SameLine();
		if (ui::overlay_button(ICON_MAXIMIZE)) layout_ = layout_ == 1 ? 4 : 1;
	}
	ImGui::PopStyleVar();
	if (arena_ && arena_->built && !arena_->entry.addon && !play_ && (i == 0 || layout_ == 1)) {
		const char *label = ICON_COPY "  Copy to Add-on...";
		float w = ImGui::CalcTextSize(label).x + ui::px(28);
		ImGui::SetCursorScreenPos(ImVec2(p.p0.x + p.size.x - w - ui::px(8), p.p0.y + p.size.y - ui::px(42)));
		if (ui::toolbar_button(nullptr, label, ui::Tone::Accent, true, "Make an add-on variant of this arena to edit it (the base game stays read-only)")) copy_to_addon();
	}
}

void WorldEditor::pane_input(int i) {
	Pane &p = panes_[size_t(i)];
	ImGuiIO &io = ImGui::GetIO();
	const bool playing_here = play_ && i == active_pane_;
	if (p.hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) {
		if (!playing_here || play_->ejected()) active_pane_ = i;
		ImGui::SetWindowFocus();
	}
	if (playing_here && !play_->ejected()) return;
	Camera &c = playing_here ? play_->eject_camera() : p.cam;
	const float dt = io.DeltaTime;
	const bool rmb = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right);
	const bool mmb = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Middle);
	const bool alt_lmb = ImGui::IsItemActive() && io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
	if (c.mode == Camera::Perspective) {
		if (rmb) {
			c.yaw += io.MouseDelta.x * 0.005f;
			c.pitch = std::clamp(c.pitch + io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
			Vec3 f = c.forward(), r = {std::cos(c.yaw), 0, std::sin(c.yaw)};
			float sp = c.speed * dt * (io.KeyShift ? 3.0f : 1.0f);
			if (ImGui::IsKeyDown(ImGuiKey_W)) c.pos = add(c.pos, mul(f, sp));
			if (ImGui::IsKeyDown(ImGuiKey_S)) c.pos = sub(c.pos, mul(f, sp));
			if (ImGui::IsKeyDown(ImGuiKey_D)) c.pos = add(c.pos, mul(r, sp));
			if (ImGui::IsKeyDown(ImGuiKey_A)) c.pos = sub(c.pos, mul(r, sp));
			if (ImGui::IsKeyDown(ImGuiKey_E)) c.pos[1] += sp;
			if (ImGui::IsKeyDown(ImGuiKey_Q)) c.pos[1] -= sp;
			if (io.MouseWheel != 0) c.speed = std::clamp(c.speed * (io.MouseWheel > 0 ? 1.25f : 0.8f), 50.0f, 20000.0f);
		} else if (mmb) {
			Vec3 r = {std::cos(c.yaw), 0, std::sin(c.yaw)};
			c.pos = add(c.pos, mul(r, -io.MouseDelta.x * 2.0f));
			c.pos[1] += io.MouseDelta.y * 2.0f;
		} else if (alt_lmb) {
			Vec3 f = c.forward();
			float d = 900;
			Vec3 pivot = add(c.pos, mul(f, d));
			c.yaw += io.MouseDelta.x * 0.005f;
			c.pitch = std::clamp(c.pitch + io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
			c.pos = sub(pivot, mul(c.forward(), d));
		} else if (p.hovered && io.MouseWheel != 0) {
			c.pos = add(c.pos, mul(c.forward(), io.MouseWheel * 120.0f));
		}
	} else {
		float k = c.half_height * 2 / std::max(1.0f, p.size.y);
		Vec3 right, up;
		if (c.mode == Camera::Top) right = {1, 0, 0}, up = {0, 0, -1};
		else if (c.mode == Camera::Front) right = {1, 0, 0}, up = {0, 1, 0};
		else right = {0, 0, -1}, up = {0, 1, 0};
		if (rmb || mmb) c.centre = add(c.centre, add(mul(right, -io.MouseDelta.x * k), mul(up, io.MouseDelta.y * k)));
		if (p.hovered && io.MouseWheel != 0) c.half_height = std::clamp(c.half_height * (io.MouseWheel > 0 ? 0.85f : 1.18f), 50.0f, 30000.0f);
	}
	if (p.hovered && !io.WantTextInput && !rmb) {
		if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selection(i);
		if (!playing_here) {
			if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_mode_ = 0;
			if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_mode_ = 1;
			if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) delete_selected();
		}
	}
	// A click without a drag selects.
	if (p.hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.KeyAlt) {
		ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0);
		if (d.x * d.x + d.y * d.y < 16) {
			if (playing_here && play_->pick_fighter(p, io.MousePos)) return;
			if (arena_ && arena_->built) select_node(pick(i, io.MousePos));
		}
	}
}

namespace {

struct Projector {
	const Pane &p;
	ImVec2 to_screen(const std::array<float, 4> &c) const {
		return ImVec2(p.p0.x + (c[0] / c[3] * 0.5f + 0.5f) * p.size.x, p.p0.y + (0.5f - c[1] / c[3] * 0.5f) * p.size.y);
	}
	bool point(const Vec3 &w, ImVec2 *out) const {
		auto c = clip_of(p.vp, w);
		if (c[3] <= 1e-3f) return false;
		*out = to_screen(c);
		return true;
	}
	bool segment(const Vec3 &a, const Vec3 &b, ImVec2 *sa, ImVec2 *sb) const {
		auto ca = clip_of(p.vp, a), cb = clip_of(p.vp, b);
		const float n = 1e-2f;
		if (ca[3] < n && cb[3] < n) return false;
		if (ca[3] < n || cb[3] < n) {
			float t = (n - ca[3]) / (cb[3] - ca[3]);
			std::array<float, 4> m;
			for (int k = 0; k < 4; ++k) m[size_t(k)] = ca[size_t(k)] + (cb[size_t(k)] - ca[size_t(k)]) * t;
			if (ca[3] < n) ca = m;
			else cb = m;
		}
		*sa = to_screen(ca), *sb = to_screen(cb);
		return true;
	}
	// Pixels per world unit at a point (for marker and gizmo sizes in orthographic and perspective views).
	float scale_at(const Vec3 &w) const {
		ImVec2 a, b;
		if (!point(w, &a)) return 0;
		Vec3 right = {p.view[0], p.view[4], p.view[8]};
		if (!point(add(w, right), &b)) return 0;
		return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
	}
};

}  // namespace

void WorldEditor::draw_overlays(int i) {
	Pane &p = panes_[size_t(i)];
	ArenaState &s = *arena_;
	Projector pr{p};
	ImDrawList *dl = ImGui::GetWindowDrawList();
	auto line = [&](const Vec3 &a, const Vec3 &b, unsigned c, float th, int alpha = 255) {
		ImVec2 sa, sb;
		if (pr.segment(a, b, &sa, &sb)) dl->AddLine(sa, sb, rgb(c, unsigned(alpha)), th);
	};
	auto circle = [&](const de::Transform &t, float r, int axis, unsigned c, float th, int alpha = 200) {
		const int n = 40;
		Vec3 prev{};
		for (int k = 0; k <= n; ++k) {
			float a = float(k) / n * 6.2831853f;
			Vec3 l = axis == 0 ? Vec3{0, std::cos(a) * r, std::sin(a) * r} : axis == 1 ? Vec3{std::cos(a) * r, 0, std::sin(a) * r} : Vec3{std::cos(a) * r, std::sin(a) * r, 0};
			Vec3 wpt = add(t.t, l);
			if (k) line(prev, wpt, c, th, alpha);
			prev = wpt;
		}
	};
	// Grid on y = 0.
	if (show_.grid && p.shading != 2) {
		float step = p.cam.mode == Camera::Perspective ? 200 : std::pow(10.0f, std::floor(std::log10(p.cam.half_height / 4)));
		Vec3 c = p.cam.mode == Camera::Perspective ? Vec3{std::round(p.cam.pos[0] / step) * step, 0, std::round(p.cam.pos[2] / step) * step} : p.cam.centre;
		int n = 20;
		for (int k = -n; k <= n; ++k) {
			unsigned col = k == 0 ? 0x5a6a9a : 0x2a2c38;
			if (p.cam.mode == Camera::Top || p.cam.mode == Camera::Perspective) {
				line({c[0] + float(k) * step, 0, c[2] - float(n) * step}, {c[0] + float(k) * step, 0, c[2] + float(n) * step}, col, 1, 90);
				line({c[0] - float(n) * step, 0, c[2] + float(k) * step}, {c[0] + float(n) * step, 0, c[2] + float(k) * step}, col, 1, 90);
			}
		}
	}
	const int sel = s.selected;
	auto world_of = [&](int k) -> de::Transform {
		if (s.dragging && k == s.drag_node) return s.drag_world;
		return s.scene.nodes[size_t(k)].world;
	};
	auto shown = [&](ed::NodeKind k) {
		switch (k) {
		case ed::NodeKind::SpawnPoint: return show_.spawns;
		case ed::NodeKind::PickupSpawner: return show_.pickups;
		case ed::NodeKind::Hazard: return show_.hazards;
		case ed::NodeKind::Surface: return show_.surfaces;
		case ed::NodeKind::ForceAffector: return show_.forces;
		case ed::NodeKind::DeathVolume: return show_.death;
		case ed::NodeKind::NavArea:
		case ed::NodeKind::NavLink: return show_.nav;
		case ed::NodeKind::Light: return show_.lights;
		case ed::NodeKind::Camera: return show_.cameras;
		case ed::NodeKind::Affector:
		case ed::NodeKind::Sound: return show_.affectors;
		default: return false;
		}
	};
	// Nav links: yellow lines between areas, dots at the areas.
	if (show_.nav) {
		for (int l : s.scene.of_kind(ed::NodeKind::NavLink)) {
			const auto &n = s.scene.nodes[size_t(l)];
			if (n.nav_to < 0 || n.parent < 0) continue;
			Vec3 a = world_of(n.parent).t, b = world_of(n.nav_to).t;
			line(a, b, 0xf5d36b, ui::px(1.6f), 220);
			ImVec2 sa, sb;
			if (pr.segment(a, b, &sa, &sb)) {
				ImVec2 d = sb - sa;
				float l2 = std::sqrt(d.x * d.x + d.y * d.y);
				if (l2 > 30) {
					ImVec2 u = d / l2, m = sa + d * 0.62f, nrm(-u.y, u.x);
					dl->AddTriangleFilled(m + u * ui::px(6), m - u * ui::px(3) + nrm * ui::px(4), m - u * ui::px(3) - nrm * ui::px(4), rgb(0xf5d36b, 220));
				}
			}
		}
		for (int a : s.scene.of_kind(ed::NodeKind::NavArea)) {
			ImVec2 sp;
			if (!pr.point(world_of(a).t, &sp)) continue;
			dl->AddCircleFilled(sp, ui::px(a == sel ? 6 : 4.5f), rgb(a == sel ? 0xffffff : 0xf5d36b));
			dl->AddCircle(sp, ui::px(a == sel ? 6 : 4.5f), rgb(0x3a2f10), 0, ui::px(1));
			if (a == sel) circle(world_of(a), s.scene.nodes[size_t(a)].falloff > 0 ? s.scene.nodes[size_t(a)].falloff : s.scene.nodes[size_t(a)].hotspot, 1, 0xf5d36b, 1.5f);
		}
	}
	// Affector shapes.
	for (size_t k = 0; k < s.scene.nodes.size(); ++k) {
		const auto &n = s.scene.nodes[k];
		if (!n.actor || n.kind == ed::NodeKind::NavArea || !shown(n.kind) || n.kind == ed::NodeKind::Light) continue;
		unsigned c = kind_colour(n.kind);
		de::Transform t = world_of(int(k));
		if (n.shape == ed::SceneNode::Shape::Sphere && (n.falloff > 0 || n.hotspot > 0)) {
			float r = n.falloff > 0 ? n.falloff : n.hotspot;
			for (int ax = 0; ax < 3; ++ax) circle(t, r, ax, c, int(k) == sel ? 2.0f : 1.2f, int(k) == sel ? 255 : 150);
		} else if (n.shape == ed::SceneNode::Shape::Box) {
			Vec3 lo = n.box_min, hi = n.box_max;
			Vec3 cs[8];
			for (int b = 0; b < 8; ++b) cs[b] = t.apply({b & 1 ? hi[0] : lo[0], b & 2 ? hi[1] : lo[1], b & 4 ? hi[2] : lo[2]});
			static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
			for (auto &e : edges) line(cs[e[0]], cs[e[1]], c, int(k) == sel ? 2.0f : 1.0f, int(k) == sel ? 230 : 90);
		}
	}
	// Markers: spawn points (with their facing), pickup spawners, lights, cameras, hazards.
	struct Marker {
		int node;
		ImVec2 at;
		float depth;
	};
	std::vector<Marker> markers;
	for (size_t k = 0; k < s.scene.nodes.size(); ++k) {
		const auto &n = s.scene.nodes[k];
		bool marker = n.kind == ed::NodeKind::SpawnPoint || n.kind == ed::NodeKind::PickupSpawner || n.kind == ed::NodeKind::Camera ||
		              n.kind == ed::NodeKind::Hazard || n.kind == ed::NodeKind::DeathVolume || n.kind == ed::NodeKind::ForceAffector ||
		              (n.kind == ed::NodeKind::Light && n.class_path == "Light");
		if (!marker || !shown(n.kind)) continue;
		Vec3 wpt = n.class_path == "Light" && n.parent >= 0 ? world_of(n.parent).t : world_of(int(k)).t;
		auto c = clip_of(p.vp, wpt);
		if (c[3] <= 1e-3f) continue;
		markers.push_back({int(k), pr.to_screen(c), c[3]});
	}
	std::sort(markers.begin(), markers.end(), [](const Marker &a, const Marker &b) { return a.depth > b.depth; });
	s.marker_hits.clear();
	for (const Marker &m : markers) {
		const auto &n = s.scene.nodes[size_t(m.node)];
		unsigned c = kind_colour(n.kind);
		const bool selected = m.node == sel || (n.class_path == "Light" && n.parent == sel);
		const float r = ui::px(selected ? 11 : 9);
		if (n.kind == ed::NodeKind::SpawnPoint) {
			de::Transform t = world_of(m.node);
			Vec3 f = t.rotate({0, 0, 1});
			ImVec2 tip;
			if (pr.point(add(t.t, mul(f, 70)), &tip)) dl->AddLine(m.at, tip, rgb(c), ui::px(selected ? 3 : 2));
			dl->AddCircleFilled(m.at, r, rgb(selected ? 0xffffff : 0x10202a, 230));
			dl->AddCircle(m.at, r, rgb(c), 0, ui::px(2));
			std::string num = n.name.size() > 16 ? n.name.substr(16) : n.name;
			ImVec2 ts = ImGui::CalcTextSize(num.c_str());
			dl->AddText(m.at - ts * 0.5f, rgb(selected ? 0x10202a : c), num.c_str());
		} else {
			ImVec2 pts[4] = {m.at + ImVec2(0, -r), m.at + ImVec2(r, 0), m.at + ImVec2(0, r), m.at + ImVec2(-r, 0)};
			if (n.kind == ed::NodeKind::PickupSpawner) {
				dl->AddConvexPolyFilled(pts, 4, rgb(selected ? 0xffffff : 0x10261a, 230));
				dl->AddPolyline(pts, 4, rgb(c), ImDrawFlags_Closed, ui::px(2));
			} else {
				dl->AddCircleFilled(m.at, r, rgb(selected ? 0xffffff : 0x1a1b22, 220));
				dl->AddCircle(m.at, r, rgb(c), 0, ui::px(1.5f));
				const char *icon = kind_icon(n.kind);
				ImVec2 ts = ImGui::CalcTextSize(icon);
				dl->AddText(m.at - ts * 0.5f, rgb(selected ? 0x1a1b22 : c), icon);
			}
		}
		const bool hovered = p.hovered && std::hypot(ImGui::GetIO().MousePos.x - m.at.x, ImGui::GetIO().MousePos.y - m.at.y) < r + ui::px(3);
		if (show_.labels && (selected || hovered)) {
			std::string label = utf8(n.name);
			dl->AddText(m.at + ImVec2(r + ui::px(4), -ui::px(7)), rgb(selected ? 0xffffff : 0xd3d5e0, selected ? 255 : 170), label.c_str());
		}
		s.marker_hits.push_back({m.node, m.at, i});
	}
	// The selected node's own shape when it is not shown by a flag.
	if (sel >= 0) {
		const auto &n = s.scene.nodes[size_t(sel)];
		if (n.actor && n.shape == ed::SceneNode::Shape::Sphere && !shown(n.kind) && (n.falloff > 0 || n.hotspot > 0)) {
			for (int ax = 0; ax < 3; ++ax) circle(world_of(sel), n.falloff > 0 ? n.falloff : n.hotspot, ax, 0xff9f3a, 1.5f);
		}
	}
	// Status, bottom right.
	{
		char buf[160];
		if (p.cam.mode == Camera::Perspective) snprintf(buf, sizeof buf, "%.0f, %.0f, %.0f   speed %.0f", p.cam.pos[0], p.cam.pos[1], p.cam.pos[2], p.cam.speed);
		else snprintf(buf, sizeof buf, "%.0f units across", p.cam.half_height * 2 * p.size.x / std::max(1.0f, p.size.y));
		ImVec2 ts = ImGui::CalcTextSize(buf);
		dl->AddText(p.p0 + p.size - ts - ImVec2(ui::px(10), ui::px(8)), rgb(0xd3d5e0, 150), buf);
	}
}

int WorldEditor::pick(int i, ImVec2 mouse) {
	Pane &p = panes_[size_t(i)];
	ArenaState &s = *arena_;
	int best = -1;
	float best_d = ui::px(14);
	for (auto &m : s.marker_hits) {
		if (m.pane != i) continue;
		float d = std::sqrt((m.at.x - mouse.x) * (m.at.x - mouse.x) + (m.at.y - mouse.y) * (m.at.y - mouse.y));
		if (d < best_d) best_d = d, best = m.node;
	}
	if (best >= 0) return best;
	// Ray picks: nav areas, affector spheres and meshes, then the arena's geometry.
	float inv[16];
	if (!invert4(p.vp, inv)) return -1;
	float nx = (mouse.x - p.p0.x) / p.size.x * 2 - 1, ny = 1 - (mouse.y - p.p0.y) / p.size.y * 2;
	auto unproject = [&](float z) {
		float v[4] = {nx, ny, z, 1}, o[4] = {0, 0, 0, 0};
		for (int r = 0; r < 4; ++r) {
			for (int c = 0; c < 4; ++c) o[r] += inv[c * 4 + r] * v[c];
		}
		return Vec3{o[0] / o[3], o[1] / o[3], o[2] / o[3]};
	};
	Vec3 a = unproject(-1), b = unproject(1);
	Vec3 dir = sub(b, a);
	float dl = len(dir);
	dir = mul(dir, 1 / dl);
	float best_t = 1e30f;
	auto tri = [&](const Vec3 &v0, const Vec3 &v1, const Vec3 &v2) -> float {
		Vec3 e1 = sub(v1, v0), e2 = sub(v2, v0), h = cross(dir, e2);
		float det = dot(e1, h);
		if (std::fabs(det) < 1e-7f) return -1;
		float f = 1 / det;
		Vec3 sv = sub(a, v0);
		float u = f * dot(sv, h);
		if (u < 0 || u > 1) return -1;
		Vec3 q = cross(sv, e1);
		float v = f * dot(dir, q);
		if (v < 0 || u + v > 1) return -1;
		return f * dot(e2, q);
	};
	for (size_t k = 0; k < s.scene.nodes.size(); ++k) {
		const auto &n = s.scene.nodes[k];
		bool visible = (n.kind == ed::NodeKind::NavArea && show_.nav) || (n.kind == ed::NodeKind::Surface && show_.surfaces) ||
		               (n.kind == ed::NodeKind::ForceAffector && show_.forces) || (n.kind == ed::NodeKind::Hazard && show_.hazards) ||
		               (n.kind == ed::NodeKind::DeathVolume && show_.death) || (n.kind == ed::NodeKind::Affector && show_.affectors);
		if (!visible) continue;
		if (n.shape == ed::SceneNode::Shape::Sphere && n.kind == ed::NodeKind::NavArea) {
			float r = std::min(40.0f, n.falloff > 0 ? n.falloff : n.hotspot);
			Vec3 oc = sub(a, n.world.t);
			float bb = dot(oc, dir), cc = dot(oc, oc) - r * r, disc = bb * bb - cc;
			if (disc >= 0) {
				float t = -bb - std::sqrt(disc);
				if (t > 0 && t < best_t) best_t = t, best = int(k);
			}
		}
		for (size_t t = 0; t + 2 < n.triangles.size(); t += 3) {
			float d = tri(n.triangles[t], n.triangles[t + 1], n.triangles[t + 2]);
			if (d > 0 && d < best_t - 1) best_t = d, best = int(k);
		}
	}
	if (best >= 0 && s.scene.nodes[size_t(best)].kind == ed::NodeKind::NavArea) return best;
	if (!s.entry.addon || true) {
		for (auto &pm : s.picks) {
			// Slab test against the mesh's box first.
			float t0 = 0, t1 = best_t;
			bool hit = true;
			for (int k = 0; k < 3 && hit; ++k) {
				if (std::fabs(dir[size_t(k)]) < 1e-8f) {
					if (a[size_t(k)] < pm.lo[size_t(k)] || a[size_t(k)] > pm.hi[size_t(k)]) hit = false;
					continue;
				}
				float ta = (pm.lo[size_t(k)] - a[size_t(k)]) / dir[size_t(k)], tb = (pm.hi[size_t(k)] - a[size_t(k)]) / dir[size_t(k)];
				if (ta > tb) std::swap(ta, tb);
				t0 = std::max(t0, ta), t1 = std::min(t1, tb);
				if (t0 > t1) hit = false;
			}
			if (!hit) continue;
			for (size_t t = 0; t + 2 < pm.tris.size(); t += 3) {
				float d = tri(pm.tris[t], pm.tris[t + 1], pm.tris[t + 2]);
				if (d > 0 && d < best_t) {
					best_t = d;
					int node = s.scene.node_for(pm.actor);
					if (s.entry.addon) {
						// The variant draws the base arena: find the copy by tree position.
						node = -1;
						const de::ObjectsFile *bf = s.world->file(s.entry.stem);
						if (bf && pm.actor < bf->objects().size()) {
							const std::string &nm = bf->objects()[pm.actor].name;
							for (size_t q = 0; q < s.scene.nodes.size() && node < 0; ++q) {
								if (s.scene.nodes[q].name == nm && s.scene.nodes[q].actor) node = int(q);
							}
						}
					}
					best = node;
				}
			}
		}
	}
	return best;
}

bool WorldEditor::gizmo(int i) {
	ArenaState &s = *arena_;
	Pane &p = panes_[size_t(i)];
	int sel = s.selected;
	if (sel < 0 || !s.entry.addon) return false;
	int target = sel;
	if (s.scene.nodes[size_t(sel)].class_path == "Light" && s.scene.nodes[size_t(sel)].parent >= 0) target = s.scene.nodes[size_t(sel)].parent;
	const auto &n = s.scene.nodes[size_t(target)];
	if (!n.actor || n.kind == ed::NodeKind::Environment || n.kind == ed::NodeKind::Surface) return false;
	Projector pr{p};
	de::Transform w = s.dragging && s.drag_node == target ? s.drag_world : n.world;
	ImVec2 o;
	if (!pr.point(w.t, &o)) return false;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImGuiIO &io = ImGui::GetIO();
	const float len_px = ui::px(80);
	float scale = pr.scale_at(w.t);
	if (scale <= 0) return false;
	const float world_len = len_px / scale;
	static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	static const unsigned cols[3] = {0xff5a5a, 0x5ad16b, 0x4f9cff};
	ImVec2 mouse = io.MousePos;
	int hover = -1;
	auto seg_dist = [](ImVec2 a, ImVec2 b, ImVec2 m) {
		ImVec2 d = b - a;
		float l2 = d.x * d.x + d.y * d.y;
		float t = l2 > 0 ? std::clamp(((m.x - a.x) * d.x + (m.y - a.y) * d.y) / l2, 0.0f, 1.0f) : 0;
		ImVec2 q = a + d * t;
		return std::sqrt((m.x - q.x) * (m.x - q.x) + (m.y - q.y) * (m.y - q.y));
	};
	ImVec2 ends[3];
	if (gizmo_mode_ == 0) {
		for (int k = 0; k < 3; ++k) {
			if (!pr.point(add(w.t, mul(axes[k], world_len)), &ends[k])) ends[k] = o;
			if (p.hovered && seg_dist(o, ends[k], mouse) < ui::px(7)) hover = k;
		}
	} else {
		float r = len_px * 0.8f;
		float d = std::sqrt((mouse.x - o.x) * (mouse.x - o.x) + (mouse.y - o.y) * (mouse.y - o.y));
		if (p.hovered && std::fabs(d - r) < ui::px(7)) hover = 1;
	}
	const int axis = s.dragging ? s.drag_axis : hover;
	// Draw.
	if (gizmo_mode_ == 0) {
		for (int k = 0; k < 3; ++k) {
			unsigned c = k == axis ? 0xffe066 : cols[k];
			dl->AddLine(o, ends[k], rgb(c), ui::px(k == axis ? 4 : 3));
			ImVec2 d = ends[k] - o;
			float l2 = std::sqrt(d.x * d.x + d.y * d.y);
			if (l2 > 1) {
				ImVec2 u = d / l2, nrm(-u.y, u.x);
				dl->AddTriangleFilled(ends[k] + u * ui::px(12), ends[k] + nrm * ui::px(6), ends[k] - nrm * ui::px(6), rgb(c));
			}
		}
		dl->AddCircleFilled(o, ui::px(4), rgb(0xffffff));
	} else {
		float r = len_px * 0.8f;
		dl->AddCircle(o, r, rgb(axis == 1 ? 0xffe066 : cols[1]), 64, ui::px(axis == 1 ? 4 : 3));
		Vec3 f = w.rotate({0, 0, 1});
		ImVec2 tip;
		if (pr.point(add(w.t, mul(f, world_len * 0.8f)), &tip)) dl->AddLine(o, tip, rgb(0x5ad16b, 160), ui::px(2));
	}
	// Drag.
	if (!s.dragging && hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
		s.dragging = true;
		s.drag_node = target;
		s.drag_axis = hover;
		s.drag_world = n.world;
		s.drag_start = n.world;
		s.drag_mouse = mouse;
		active_pane_ = i;
	}
	if (s.dragging && s.drag_node == target) {
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			ImVec2 dm = mouse - s.drag_mouse;
			if (gizmo_mode_ == 0) {
				ImVec2 so, se;
				pr.point(s.drag_start.t, &so);
				pr.point(add(s.drag_start.t, axes[s.drag_axis]), &se);
				ImVec2 ad = se - so;
				float px_per_unit = std::sqrt(ad.x * ad.x + ad.y * ad.y);
				if (px_per_unit > 1e-4f) {
					float units = (dm.x * ad.x + dm.y * ad.y) / (px_per_unit * px_per_unit);
					de::Transform t = s.drag_start;
					t.t[size_t(s.drag_axis)] += units;
					if (snap_) t.t[size_t(s.drag_axis)] = std::round(t.t[size_t(s.drag_axis)] / grid_) * grid_;
					s.drag_world = t;
				}
			} else {
				float a0 = std::atan2(s.drag_mouse.y - o.y, s.drag_mouse.x - o.x), a1 = std::atan2(mouse.y - o.y, mouse.x - o.x);
				float da = a1 - a0;
				if (p.cam.mode == Camera::Perspective && p.cam.pitch < 0) da = -da;
				if (snap_) da = std::round(da / (3.14159265f / 12)) * (3.14159265f / 12);
				float c = std::cos(-da), sn = std::sin(-da);
				de::Transform t = s.drag_start;
				for (int row = 0; row < 3; ++row) {
					float x = s.drag_start.r[size_t(row * 3)], z = s.drag_start.r[size_t(row * 3 + 2)];
					t.r[size_t(row * 3)] = x * c - z * sn;
					t.r[size_t(row * 3 + 2)] = x * sn + z * c;
				}
				s.drag_world = t;
			}
		} else {
			commit_drag();
		}
		return true;
	}
	return hover >= 0;
}

void WorldEditor::commit_drag() {
	ArenaState &s = *arena_;
	s.dragging = false;
	ed::Project *p = e_.project();
	if (!p || s.drag_node < 0) return;
	const auto &n = s.scene.nodes[size_t(s.drag_node)];
	de::Transform parent = n.parent >= 0 ? s.scene.nodes[size_t(n.parent)].world : de::Transform{};
	de::Transform local = parent.inverse().then(s.drag_world);
	int obj = p->find(n.guid);
	if (obj < 0) return;
	try {
		ed::set_actor_local(*p, uint32_t(obj), local);
		e_.status("Moved " + utf8(n.name) + " to " + std::to_string(int(s.drag_world.t[0])) + ", " + std::to_string(int(s.drag_world.t[1])) + ", " +
		          std::to_string(int(s.drag_world.t[2])));
	} catch (const std::exception &ex) {
		e_.log(LogLine::Error, std::string("Cannot move ") + utf8(n.name) + ": " + ex.what());
	}
	refresh_scene();
}

// ---------------------------------------------------------------------------------------------
// Place and delete

void WorldEditor::place(const char *what) {
	if (!arena_ || !arena_->entry.addon || !e_.project()) {
		e_.log(LogLine::Warning, "Placing needs an arena variant open (Copy to Add-on on a base arena)");
		return;
	}
	ArenaState &s = *arena_;
	ed::Project &p = *e_.project();
	ed::NodeKind kind = !strcmp(what, "Spawn Point") ? ed::NodeKind::SpawnPoint : ed::NodeKind::PickupSpawner;
	auto existing = s.scene.of_kind(kind);
	if (existing.empty()) {
		e_.log(LogLine::Warning, std::string("This arena has no ") + what + " to copy from");
		return;
	}
	int from = s.selected >= 0 && s.scene.nodes[size_t(s.selected)].kind == kind ? s.selected : existing.back();
	std::string name;
	if (kind == ed::NodeKind::SpawnPoint) {
		int top = 0;
		for (int k : existing) top = std::max(top, atoi(s.scene.nodes[size_t(k)].name.c_str() + 16));
		char buf[32];
		snprintf(buf, sizeof buf, "PlayerSpawnPoint%02d", top + 1);
		name = buf;
	} else {
		name = s.scene.nodes[size_t(from)].name + " Copy";
	}
	int obj = p.find(s.scene.nodes[size_t(from)].guid);
	if (obj < 0) return;
	try {
		uint32_t made = ed::duplicate_actor(p, uint32_t(obj), name);
		// Where the active view looks, on the ground.
		Pane &pane = panes_[size_t(active_pane_)];
		Vec3 at = pane.cam.mode == Camera::Perspective ? add(pane.cam.pos, mul(pane.cam.forward(), 600)) : pane.cam.centre;
		if (auto y = s.scene.surface_below({at[0], 3000, at[2]}, 6000)) at[1] = *y;
		const auto &src = s.scene.nodes[size_t(from)];
		de::Transform world = src.world;
		world.t = at;
		de::Transform parent = src.parent >= 0 ? s.scene.nodes[size_t(src.parent)].world : de::Transform{};
		ed::set_actor_local(p, made, parent.inverse().then(world));
		ed::Guid g = p.file().objects[made].info.guid;
		refresh_scene();
		for (size_t k = 0; k < s.scene.nodes.size(); ++k) {
			if (s.scene.nodes[k].guid == g) select_node(int(k));
		}
		e_.log(LogLine::Ok, "Placed " + name);
	} catch (const std::exception &ex) {
		e_.log(LogLine::Error, std::string("Cannot place: ") + ex.what());
	}
}

void WorldEditor::delete_selected() {
	if (!arena_ || !arena_->entry.addon || !e_.project() || arena_->selected < 0) return;
	ArenaState &s = *arena_;
	const auto &n = s.scene.nodes[size_t(s.selected)];
	if (n.kind != ed::NodeKind::SpawnPoint && n.kind != ed::NodeKind::PickupSpawner) {
		e_.status("Only spawn points and pickup spawners can be removed in this iteration");
		return;
	}
	int obj = e_.project()->find(n.guid);
	if (obj < 0) return;
	std::string name = utf8(n.name);
	ed::detach_object(*e_.project(), uint32_t(obj));
	select_node(-1);
	refresh_scene();
	e_.log(LogLine::Ok, "Removed " + name + " from the arena (Ctrl+Z brings it back)");
}

bool WorldEditor::draw_place() {
	if (!arena_) return false;
	const bool editable = arena_->entry.addon;
	ui::search_box("place", "Search placables...", &arena_->place_filter, -1);
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	if (!editable) {
		ImGui::PushTextWrapPos(0);
		ui::dim_text("Base arenas are read-only. Copy this one to an add-on to place spawn points and pickup spawners.", kTextMuted);
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, ui::px(4)));
		if (ui::toolbar_button(ICON_COPY, "Copy to Add-on...", ui::Tone::Accent)) copy_to_addon();
		ImGui::Dummy(ImVec2(0, ui::px(6)));
	}
	struct Item {
		const char *icon, *label;
		unsigned colour;
		bool active;
	};
	const Item gameplay[] = {{ICON_MAP_PIN, "Spawn Point", 0x4fc3f7, true}, {ICON_PACKAGE, "Pickup Spawner", 0x8fe3a8, true},
	                         {ICON_FLAME, "Hazard", 0xff8f6b, false},        {ICON_LAYERS, "Fighting Surface", 0xb18cff, false},
	                         {ICON_WIND, "Force Affector", 0x7fd1c7, false}, {ICON_ROUTE, "Nav Link", 0xf5d36b, false}};
	const Item env[] = {{ICON_LIGHTBULB, "Light", 0xf5d36b, false}, {ICON_VIDEO, "Camera", 0xa9acbd, false}, {ICON_SPARKLES, "Effect", 0xff9bd2, false},
	                    {ICON_VOLUME_2, "Sound", 0x8fb8ff, false},     {ICON_BOXES, "Prefab", 0xc3a6ff, false}};
	static bool open_g = true, open_e = true;
	auto group = [&](const char *id, const char *icon, const char *label, bool *open, const Item *items, size_t n) {
		ui::TreeRow r;
		r.icon = icon, r.label = label, r.has_children = true, r.open = open, r.icon_colour = kTextMuted;
		ui::tree_row(id, r);
		if (!*open) return;
		for (size_t k = 0; k < n; ++k) {
			const Item &it = items[k];
			std::string f = arena_->place_filter;
			if (!f.empty() && std::string(it.label).find(f) == std::string::npos) continue;
			ui::TreeRow c;
			c.icon = it.icon, c.label = it.label, c.depth = 1, c.icon_colour = it.colour, c.disabled = !(editable && it.active);
			if (ui::tree_row(it.label, c) && editable && it.active) place(it.label);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
				ImGui::SetTooltip("%s", it.active ? (editable ? "Click to place a copy where the selected view looks" : "Copy the arena to an add-on first")
				                                  : "Arrives in a later iteration");
			}
		}
	};
	group("gameplay", ICON_GAMEPAD_2, "Gameplay", &open_g, gameplay, std::size(gameplay));
	group("environment", ICON_SUN, "Environment", &open_e, env, std::size(env));
	return true;
}
