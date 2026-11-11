#include "models.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "arena.h"
#include "gl_util.h"
#include "har_import.h"
#include "icons.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "model_export.h"
#include "robot_import.h"
#include "theme.h"
#include "widgets.h"
#include "world.h"
#include "world_state.h"

namespace fs = std::filesystem;
using namespace theme;

namespace {

std::string path_text(const fs::path &p) {
	auto u = p.u8string();
	return std::string(u.begin(), u.end());
}

unsigned role_colour(ed::Role r) {
	switch (r) {
	case ed::Role::Visual: return kTextSoft;
	case ed::Role::Floor: return 0xb18cff;
	case ed::Role::Wall: return 0x7fd1c7;
	case ed::Role::Kill:
	case ed::Role::Death: return 0xff5a5a;
	case ed::Role::JumpPad: return 0x7fd1c7;
	case ed::Role::Spawn: return 0x4fc3f7;
	case ed::Role::Pickup: return 0x8fe3a8;
	default: return kTextFaint;
	}
}

const char *role_icon(ed::Role r) {
	switch (r) {
	case ed::Role::Visual: return ICON_BOX;
	case ed::Role::Floor:
	case ed::Role::Wall: return ICON_LAYERS;
	case ed::Role::Kill:
	case ed::Role::Death: return ICON_CIRCLE_X;
	case ed::Role::JumpPad: return ICON_WIND;
	case ed::Role::Spawn: return ICON_MAP_PIN;
	case ed::Role::Pickup: return ICON_PACKAGE;
	default: return ICON_EYE_OFF;
	}
}

const char *kPreviewVs = R"(#version 410 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec2 uv;
uniform mat4 vp;
out vec3 v_n;
out vec2 v_uv;
void main() { gl_Position = vp * vec4(pos, 1.0); v_n = nrm; v_uv = uv; }
)";
const char *kPreviewFs = R"(#version 410 core
in vec3 v_n;
in vec2 v_uv;
uniform sampler2D tex;
uniform int textured;
uniform vec4 colour;
out vec4 frag;
void main() {
	vec4 c = colour;
	if (textured != 0) c *= texture(tex, v_uv);
	float l = 0.45 + 0.55 * max(dot(normalize(v_n), normalize(vec3(0.4, 0.85, 0.35))), 0.0);
	frag = vec4(c.rgb * l, c.a);
}
)";

struct PreviewMesh {
	GLuint vao = 0, vbo = 0;
	GLsizei count = 0;
	GLuint tex = 0;
	float colour[4] = {1, 1, 1, 1};
	bool overlay = false;
};

struct Preview {
	Pane pane;
	GLuint prog = 0;
	std::vector<PreviewMesh> meshes;
	std::map<int, GLuint> textures;
	FlatMesh markers;
	gltf::Vec3 lo{0, 0, 0}, hi{0, 0, 0};
	float yaw = 0.6f, pitch = 0.5f, dist = 10;
	std::string key;
	bool show_collision = true, show_visual = true;

	void clear() {
		for (auto &m : meshes) glDeleteBuffers(1, &m.vbo), glDeleteVertexArrays(1, &m.vao);
		meshes.clear();
		for (auto &[k, t] : textures) glDeleteTextures(1, &t);
		textures.clear();
		markers.clear();
		key.clear();
	}

	static std::string key_of(const ed::ImportPlan &plan) {
		std::string k = path_text(plan.source) + (plan.settings.z_up ? "z" : "y");
		for (auto &it : plan.items) k += char('a' + int(it.role));
		return k;
	}

	void build(const ed::ImportPlan &plan) {
		clear();
		key = key_of(plan);
		if (!prog) prog = glu::program(kPreviewVs, kPreviewFs);
		const gltf::Scene &s = plan.scene;
		gltf::Mat4 fix = plan.settings.z_up ? gltf::Mat4{1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1} : gltf::identity();
		bool any = false;
		auto grow = [&](const gltf::Vec3 &p) {
			for (int k = 0; k < 3; ++k) {
				lo[size_t(k)] = any ? std::min(lo[size_t(k)], p[size_t(k)]) : p[size_t(k)];
				hi[size_t(k)] = any ? std::max(hi[size_t(k)], p[size_t(k)]) : p[size_t(k)];
			}
			any = true;
		};
		for (const auto &it : plan.items) {
			if (it.role == ed::Role::Ignore) continue;
			// Death volumes are often far larger than the arena: the view frames the rest.
			const bool frames = it.role != ed::Role::Death && it.role != ed::Role::Kill;
			gltf::Mat4 m = gltf::multiply(fix, s.world(it.node));
			const gltf::Node &n = s.nodes[size_t(it.node)];
			if (it.role == ed::Role::Spawn || it.role == ed::Role::Pickup) {
				gltf::Vec3 p = gltf::transform_point(m, {0, 0, 0});
				grow(p);
				unsigned c = role_colour(it.role);
				float h = 3, r = 1;
				markers.vertex({p[0], p[1], p[2]}, c), markers.vertex({p[0], p[1] + h, p[2]}, c);
				for (int k = 0; k < 8; ++k) {
					float a0 = float(k) * 0.785398f, a1 = float(k + 1) * 0.785398f;
					markers.vertex({p[0] + std::cos(a0) * r, p[1] + 0.05f, p[2] + std::sin(a0) * r}, c);
					markers.vertex({p[0] + std::cos(a1) * r, p[1] + 0.05f, p[2] + std::sin(a1) * r}, c);
				}
				if (it.role == ed::Role::Spawn) {
					gltf::Vec3 f = gltf::transform_direction(m, {0, 0, 1});
					markers.vertex({p[0], p[1] + 0.1f, p[2]}, c), markers.vertex({p[0] + f[0] * 2, p[1] + 0.1f, p[2] + f[2] * 2}, c);
				}
				continue;
			}
			if (n.mesh < 0) continue;
			gltf::Mat4 nm = gltf::inverse(m);
			for (const auto &prim : s.meshes[size_t(n.mesh)].primitives) {
				std::vector<float> v;
				v.reserve(prim.indices.size() * 8);
				int material = prim.material;
				const gltf::Material *mat = material >= 0 && size_t(material) < s.materials.size() ? &s.materials[size_t(material)] : nullptr;
				int uvset = mat && mat->base_texture ? mat->base_texture.uv_set : 0;
				for (uint32_t i : prim.indices) {
					gltf::Vec3 p = gltf::transform_point(m, prim.positions[i]);
					if (frames) grow(p);
					gltf::Vec3 nn{0, 1, 0};
					if (i < prim.normals.size()) {
						const auto &q = prim.normals[i];
						nn = {nm[0] * q[0] + nm[1] * q[1] + nm[2] * q[2], nm[4] * q[0] + nm[5] * q[1] + nm[6] * q[2], nm[8] * q[0] + nm[9] * q[1] + nm[10] * q[2]};
					}
					gltf::Vec2 uv{0, 0};
					if (size_t(uvset) < prim.uvs.size() && i < prim.uvs[size_t(uvset)].size()) uv = prim.uvs[size_t(uvset)][i];
					v.insert(v.end(), {p[0], p[1], p[2], nn[0], nn[1], nn[2], uv[0], uv[1]});
				}
				PreviewMesh pm;
				pm.count = GLsizei(prim.indices.size());
				if (it.role == ed::Role::Visual) {
					if (mat) memcpy(pm.colour, mat->base_colour.data(), sizeof pm.colour);
					if (mat && mat->base_texture && size_t(mat->base_texture.image) < s.images.size()) {
						int img = mat->base_texture.image;
						auto ti = textures.find(img);
						if (ti == textures.end() && !s.images[size_t(img)].rgba.empty()) {
							const auto &im = s.images[size_t(img)];
							ti = textures.emplace(img, glu::texture_rgba(im.width, im.height, im.rgba.data())).first;
						}
						if (ti != textures.end()) pm.tex = ti->second;
					}
				} else {
					unsigned c = role_colour(it.role);
					pm.colour[0] = float((c >> 16) & 255) / 255.0f, pm.colour[1] = float((c >> 8) & 255) / 255.0f, pm.colour[2] = float(c & 255) / 255.0f;
					pm.colour[3] = 0.38f;
					pm.overlay = true;
				}
				glGenVertexArrays(1, &pm.vao);
				glBindVertexArray(pm.vao);
				glGenBuffers(1, &pm.vbo);
				glBindBuffer(GL_ARRAY_BUFFER, pm.vbo);
				glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * 4), v.data(), GL_STATIC_DRAW);
				glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
				glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
				glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
				for (GLuint k = 0; k < 3; ++k) glEnableVertexAttribArray(k);
				meshes.push_back(pm);
			}
		}
		markers.upload();
		glBindVertexArray(0);
		float size = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 0.5f});
		dist = size * 1.4f;
	}

	void draw(ImVec2 size) {
		ImGuiIO &io = ImGui::GetIO();
		const int w = std::max(16, int(size.x * io.DisplayFramebufferScale.x)), h = std::max(16, int(size.y * io.DisplayFramebufferScale.y));
		GLint prev_fbo = 0, prev_vp[4];
		bind_pane(pane, w, h, &prev_fbo, prev_vp);
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glClearColor(0.071f, 0.075f, 0.094f, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		gltf::Vec3 c{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
		float eye[3] = {c[0] + std::sin(yaw) * std::cos(pitch) * dist, c[1] + std::sin(pitch) * dist, c[2] + std::cos(yaw) * std::cos(pitch) * dist};
		float f[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
		float up[3] = {0, 1, 0};
		float proj[16], view[16], vp[16];
		glu::perspective(proj, 50, float(w) / float(h), dist * 0.01f, dist * 20);
		glu::look_view(view, eye, f, up);
		glu::mul(vp, proj, view);
		glUseProgram(prog);
		glUniformMatrix4fv(glGetUniformLocation(prog, "vp"), 1, GL_FALSE, vp);
		glUniform1i(glGetUniformLocation(prog, "tex"), 0);
		glActiveTexture(GL_TEXTURE0);
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		for (int pass = 0; pass < 2; ++pass) {
			if (pass == 1) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glDepthMask(GL_FALSE);
			}
			for (auto &m : meshes) {
				if (m.overlay != (pass == 1) || (m.overlay && !show_collision) || (!m.overlay && !show_visual)) continue;
				glUniform4fv(glGetUniformLocation(prog, "colour"), 1, m.colour);
				glUniform1i(glGetUniformLocation(prog, "textured"), m.tex ? 1 : 0);
				glBindTexture(GL_TEXTURE_2D, m.tex);
				glBindVertexArray(m.vao);
				glDrawArrays(GL_TRIANGLES, 0, m.count);
			}
		}
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
		markers.draw(vp, GL_LINES, false, true);
		glBindVertexArray(0);
		glUseProgram(0);
		glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
		glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
		ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImGui::Image((ImTextureID)(intptr_t)pane.tex, size, ImVec2(0, 1), ImVec2(1, 0));
		if (ImGui::IsItemHovered()) {
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
				yaw -= io.MouseDelta.x * 0.01f;
				pitch = std::clamp(pitch + io.MouseDelta.y * 0.01f, -1.4f, 1.4f);
			}
			if (io.MouseWheel != 0) dist *= std::pow(0.88f, io.MouseWheel);
		}
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRect(p0, p0 + size, rgb(kBorder), ui::px(6));
	}
};

struct State {
	bool open_request = false;
	ed::ImportPlan plan;
	bool have_plan = false;
	std::string error;
	int step = 0;
	Preview preview;
	std::vector<std::string> arenas, pickups;
	bool scripted = false;  // opened by --import-dialog: no commit
	std::string last_dir;
};
State &state() {
	static State s;
	return s;
}

// "[file stem/][Class:]Name".
ObjectRef find_spec(Editor &e, const std::string &spec_in) {
	std::string spec = spec_in, stem;
	if (auto slash = spec.find('/'); slash != std::string::npos) stem = spec.substr(0, slash), spec = spec.substr(slash + 1);
	auto colon = spec.rfind(':');
	std::string cls = colon == std::string::npos ? "" : spec.substr(0, colon), name = colon == std::string::npos ? spec : spec.substr(colon + 1);
	if (ed::Project *p = e.project()) {
		for (auto &o : p->file().objects) {
			if (utf8_text(o.info.name_text()) == name && (cls.empty() || o.info.class_text() == cls) && !ed::export_kind(o.info.class_text()).empty()) return ObjectRef::addon(o.info.guid);
		}
	}
	if (ed::BaseData *b = e.base()) {
		for (auto &f : b->files()) {
			if (!stem.empty() && f.stem != stem) continue;
			for (auto &o : f.objects) {
				if (o.name == name && (cls.empty() || o.class_path == cls) && !ed::export_kind(o.class_path).empty()) return ObjectRef::base(o);
			}
		}
	}
	return {};
}

std::string class_of(Editor &e, const ObjectRef &r) {
	if (r.kind == ObjectRef::Base) return e.base()->files()[r.file].objects[r.index].class_path;
	int i = e.addon_index(r);
	return i >= 0 ? e.project()->file().objects[size_t(i)].info.class_text() : std::string();
}

void run_export(Editor &e, const ObjectRef &r, const fs::path &out) {
	ed::BaseData &base = *e.base();
	std::vector<std::string> stems;
	std::string addon_stem;
	if (r.kind == ObjectRef::Base) {
		const std::string &stem = base.files()[r.file].stem;
		stems = ed::arena_files(base, stem);
		// A robot's model file names its fighter file only through descriptions.
		if (auto pos = stem.find("Model"); pos != std::string::npos && base.file(stem.substr(0, pos))) stems.push_back(stem.substr(0, pos));
		if (base.files()[r.file].objects[r.index].class_path == "Description\\Environment") {
			ed::Guid res = ed::description_resource(base.model(r.file).objects[r.index]);
			if (const ed::BaseObject *env = base.by_guid(res)) {
				auto more = ed::arena_files(base, base.files()[env->file].stem);
				stems.insert(stems.end(), more.begin(), more.end());
			}
		}
	} else {
		stems = ed::arena_files(base, "", &e.project()->file());
		addon_stem = e.project()->path().stem().string();
	}
	std::set<std::string> keep(stems.begin(), stems.end());
	de::World w(base.dir(), [&](const std::string &s) { return keep.count(s) > 0; });
	de::ObjectRef obj;
	if (r.kind == ObjectRef::Base) {
		obj = {w.file(base.files()[r.file].stem), r.index};
	} else {
		w.set_file(addon_stem, e.project()->bytes());
		const de::ObjectsFile *f = w.file(addon_stem);
		for (uint32_t i = 0; f && i < f->objects().size(); ++i) {
			if (memcmp(f->objects()[i].guid.bytes, r.guid.b, 16) == 0) obj = {f, i};
		}
	}
	if (!obj.file) throw std::runtime_error("the object's file is not loaded");
	gltf::Scene s = ed::export_object(w, obj);
	gltf::write(s, out);
	size_t tris = 0;
	for (auto &m : s.meshes) {
		for (auto &p : m.primitives) tris += p.triangles();
	}
	e.log(LogLine::Ok, "Exported " + obj.info().name + " to " + path_text(out) + ": " + std::to_string(s.nodes.size()) + " nodes, " + std::to_string(s.meshes.size()) + " meshes (" +
	                       std::to_string(tris) + " triangles), " + std::to_string(s.materials.size()) + " materials, " + std::to_string(s.images.size()) + " images" +
	                       (s.skins.empty() ? "" : ", " + std::to_string(s.skins.size()) + " skins") + (s.animations.empty() ? "" : ", " + std::to_string(s.animations.size()) + " animations"));
	for (auto &wn : s.warnings) e.log(LogLine::Warning, wn);
}

void load_plan(Editor &e, const fs::path &file, ed::ImportKind kind, bool kind_given) {
	State &st = state();
	st.error.clear();
	st.step = 0;
	st.have_plan = false;
	st.preview.key.clear();
	try {
		ed::ImportSettings set;
		set.kind = kind;
		st.plan = ed::analyse(file, set);
		if (!kind_given) {
			// Collections make it an arena.
			bool arena = false;
			for (auto &n : st.plan.scene.nodes) {
				std::string l = n.name;
				for (auto &c : l) c = char(tolower(uint8_t(c)));
				arena = arena || l == "collision" || l == "spawn";
			}
			if (arena) {
				set.kind = ed::ImportKind::Arena;
				st.plan = ed::analyse(file, set);
			} else if (!st.plan.scene.skins.empty()) {
				set.kind = ed::ImportKind::Robot;
				st.plan = ed::analyse(file, set);
			}
		}
		if (st.plan.settings.kind == ed::ImportKind::Robot) robot_analyse(e, st.plan);
		ed::revalidate(st.plan, e.base());
		st.have_plan = true;
		st.last_dir = path_text(file.parent_path());
	} catch (const std::exception &ex) {
		st.error = ex.what();
	}
	if (e.base()) {
		st.arenas.clear();
		for (auto &a : ed::base_arenas(*e.base())) {
			if (a.listed) st.arenas.push_back(a.name);
		}
		st.pickups = ed::pickup_names(*e.base());
	}
}

void reanalyse_kind(Editor &e, ed::ImportKind kind) {
	State &st = state();
	ed::ImportSettings set = st.plan.settings;
	set.kind = kind;
	fs::path src = st.plan.source;
	std::string base_robot = st.plan.har.base_robot;
	st.plan = ed::analyse(std::move(st.plan.scene), set);
	st.plan.source = src;
	st.plan.har.base_robot = base_robot;
	if (kind == ed::ImportKind::Robot) robot_analyse(e, st.plan);
	ed::revalidate(st.plan, e.base());
}

bool commit(Editor &e) {
	State &st = state();
	ed::Project *p = e.project();
	if (!p) return false;
	if (st.plan.settings.kind == ed::ImportKind::Robot) return robot_commit(e, st.plan, &st.error);
	try {
		ed::ImportResult r = st.plan.settings.kind == ed::ImportKind::Prop ? ed::import_prop(*p, st.plan) : ed::import_arena(*p, st.plan);
		fs::path kept;
		try {
			if (!st.plan.source.empty()) kept = ed::keep_source(*p, st.plan.source, st.plan.settings.name);
		} catch (const std::exception &ex) {
			e.log(LogLine::Warning, std::string("The source file could not be kept in the sidecar folder: ") + ex.what());
		}
		e.log(LogLine::Ok, "Imported " + st.plan.settings.name + " (" + (st.plan.settings.kind == ed::ImportKind::Prop ? "prop" : "arena") + "): " + r.summary());
		for (auto &n : r.notes) e.log(LogLine::Info, n);
		if (!kept.empty()) e.log(LogLine::Info, "Source kept in " + path_text(kept));
		if (st.plan.settings.kind == ed::ImportKind::Arena) {
			for (auto &a : ed::addon_arenas(*p)) {
				if (a.description == p->file().objects[r.root].info.guid && e.world()) e.world()->open_arena(a);
			}
		} else {
			e.log(LogLine::Info, "Place it from the Place panel (Imported Props) in an add-on arena");
			e.select(ObjectRef::addon(p->file().objects[r.root].info.guid));
		}
		e.validate_now();
		return true;
	} catch (const std::exception &ex) {
		st.error = ex.what();
		e.log(LogLine::Error, std::string("Import failed: ") + ex.what());
		return false;
	}
}

void section(const char *label) {
	ImGui::Dummy(ImVec2(0, ui::px(4)));
	ImGui::PushStyleColor(ImGuiCol_Text, vec(kTextDim));
	ImGui::SeparatorText(label);
	ImGui::PopStyleColor();
}

void info_row(const char *label, const std::string &value, float lw) {
	ui::form_label(label, lw);
	ImGui::TextUnformatted(value.c_str());
}

void page_source(Editor &e, ed::ImportPlan &plan) {
	const float lw = ui::px(160);
	const gltf::Scene &s = plan.scene;
	section("File");
	info_row("Source", plan.source.filename().string(), lw);
	info_row("Made by", s.generator.empty() ? "(unknown)" : s.generator + (plan.from_editor ? "  (an OpenBG export)" : ""), lw);
	size_t tris = 0;
	for (auto &m : s.meshes) {
		for (auto &p : m.primitives) tris += p.triangles();
	}
	info_row("Contents", std::to_string(s.nodes.size()) + " nodes, " + std::to_string(s.meshes.size()) + " meshes (" + std::to_string(tris) + " triangles), " +
	                         std::to_string(s.materials.size()) + " materials, " + std::to_string(s.images.size()) + " images",
	         lw);
	if (!s.skins.empty() || !s.animations.empty()) info_row("Animation", std::to_string(s.skins.size()) + " skins, " + std::to_string(s.animations.size()) + " animations", lw);
	section("Import as");
	ui::form_label("Kind", lw);
	int kind = int(plan.settings.kind);
	const char *kinds[] = {ICON_BOX "  Prop", ICON_MOUNTAIN "  Arena", ICON_BOT "  Robot"};
	for (int k = 0; k < 3; ++k) {
		if (k) ImGui::SameLine(0, ui::px(6));
		if (ui::toolbar_button(nullptr, kinds[k], kind == k ? ui::Tone::Accent : ui::Tone::Normal, true, nullptr, ui::px(28)) && kind != k) reanalyse_kind(e, ed::ImportKind(k));
	}
	ui::form_label("Name", lw);
	ImGui::SetNextItemWidth(ui::px(300));
	ImGui::InputText("##name", &plan.settings.name);
	section("Space");
	ui::form_label("Units per metre", lw);
	ImGui::SetNextItemWidth(ui::px(120));
	ImGui::BeginDisabled(plan.from_editor);
	ImGui::DragFloat("##units", &plan.settings.units_per_metre, 0.1f, 0.01f, 10000, "%.2f");
	ImGui::EndDisabled();
	ImGui::SameLine();
	ui::dim_text(plan.from_editor ? "from the file" : "a robot is about 30 units (3 m at 10)", kTextMuted);
	ui::form_label("Up axis", lw);
	bool z = plan.settings.z_up;
	if (ui::toolbar_button(nullptr, "+Y", !z ? ui::Tone::Accent : ui::Tone::Normal, true, nullptr, ui::px(28))) plan.settings.z_up = false;
	ImGui::SameLine(0, ui::px(6));
	if (ui::toolbar_button(nullptr, "+Z", z ? ui::Tone::Accent : ui::Tone::Normal, true, nullptr, ui::px(28))) plan.settings.z_up = true;
	if (z != plan.settings.z_up && plan.settings.kind == ed::ImportKind::Robot) {
		if (plan.har.auto_orient) ed::detect_orientation(plan);
		ed::revalidate(plan, e.base());
	}
	if (plan.settings.kind == ed::ImportKind::Robot) robot_source_section(e, plan);
	if (plan.settings.kind == ed::ImportKind::Arena) {
		section("Arena");
		ui::form_label("Lights and sky from", lw);
		ImGui::SetNextItemWidth(ui::px(300));
		if (ui::begin_combo("##tmpl", plan.settings.template_arena.c_str(), ImGuiComboFlags_HeightLarge)) {
			for (auto &a : state().arenas) {
				if (ImGui::Selectable(a.c_str(), a == plan.settings.template_arena)) plan.settings.template_arena = a;
			}
			ImGui::EndCombo();
		}
		ui::form_label("Keep its sky", lw);
		ui::checkbox("##sky", &plan.settings.keep_sky);
		ui::form_label("Navigation net", lw);
		ui::checkbox("##nav", &plan.settings.generate_nav);
		ImGui::SameLine();
		ui::dim_text("generated over the floors (at most 64 areas, 256 links)", kTextMuted);
		if (plan.settings.generate_nav) {
			ui::form_label("Nav spacing", lw);
			ImGui::SetNextItemWidth(ui::px(120));
			ImGui::DragFloat("##spacing", &plan.settings.nav_spacing, 1, 0, 2000, plan.settings.nav_spacing > 0 ? "%.0f units" : "auto");
		}
	}
}

void page_contents(Editor &e, ed::ImportPlan &plan) {
	ui::dim_text(plan.settings.kind == ed::ImportKind::Arena   ? "Roles come from the collections (Visual, Collision, Death, JumpPad, Spawn, Pickup) and the collision materials (floor, wall, kill)."
	             : plan.settings.kind == ed::ImportKind::Robot ? "Meshes named LOD0..LOD2 are the levels of detail; the rest is LOD0. Set a node to Ignore to leave it out."
	                                                           : "Every mesh becomes part of the prop's mesh; set a node to Ignore to leave it out.",
	             kTextMuted);
	ImGui::Dummy(ImVec2(0, ui::px(4)));
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("contents", 4, flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch, 2.2f);
	ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, ui::px(80));
	ImGui::TableSetupColumn("Becomes", ImGuiTableColumnFlags_WidthFixed, ui::px(170));
	ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 1.4f);
	ImGui::TableHeadersRow();
	bool changed = false;
	for (size_t i = 0; i < plan.items.size(); ++i) {
		auto &it = plan.items[i];
		ImGui::PushID(int(i));
		ImGui::TableNextRow(0, ui::px(30));
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, vec(role_colour(it.role)));
		ImGui::TextUnformatted(role_icon(it.role));
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextUnformatted(it.name.c_str());
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s%s", it.path.c_str(), it.collection.empty() ? "" : ("  (collection " + it.collection + ")").c_str());
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		if (it.triangles) ImGui::Text("%zu", it.triangles);
		else ui::dim_text("empty", kTextFaint);
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-1);
		const bool mesh = plan.scene.nodes[size_t(it.node)].mesh >= 0;
		if (ui::begin_combo("##role", ed::role_name(it.role))) {
			for (int r = 0; r < int(ed::Role::Count); ++r) {
				ed::Role role = ed::Role(r);
				if (plan.settings.kind != ed::ImportKind::Arena && role != ed::Role::Visual && role != ed::Role::Ignore) continue;
				if (!mesh && role != ed::Role::Spawn && role != ed::Role::Pickup && role != ed::Role::Ignore) continue;
				if (ImGui::Selectable(ed::role_name(role), it.role == role)) it.role = role, changed = true;
				if (changed && plan.settings.kind == ed::ImportKind::Robot) robot_analyse(e, plan);
			}
			ImGui::EndCombo();
		}
		ImGui::TableNextColumn();
		if (it.role == ed::Role::Pickup) {
			std::string label;
			for (auto &p : it.pickups) label += (label.empty() ? "" : ", ") + p;
			ImGui::SetNextItemWidth(-1);
			if (ui::begin_combo("##pk", label.empty() ? "EnergyBoost (default)" : label.c_str(), ImGuiComboFlags_HeightLarge)) {
				for (auto &name : state().pickups) {
					bool on = std::find(it.pickups.begin(), it.pickups.end(), name) != it.pickups.end();
					if (ImGui::Checkbox(name.c_str(), &on)) {
						if (on) it.pickups.push_back(name);
						else it.pickups.erase(std::remove(it.pickups.begin(), it.pickups.end(), name), it.pickups.end());
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
		} else if (it.role == ed::Role::JumpPad) {
			ImGui::SetNextItemWidth(-1);
			if (ImGui::DragFloat3("##imp", it.impulse.data(), 0.01f, -1, 1, "%.2f")) it.has_impulse = true;
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Launch direction (glTF space, extras.impulse)");
		} else if (plan.settings.kind == ed::ImportKind::Robot && it.role == ed::Role::Visual) {
			ImGui::AlignTextToFramePadding();
			int lod = 0;
			for (int k = 0; k < 3; ++k) {
				for (int n : plan.har.lods[size_t(k)]) lod = n == it.node ? k : lod;
			}
			const gltf::Node &nd = plan.scene.nodes[size_t(it.node)];
			ui::dim_text(("LOD" + std::to_string(lod) + (nd.skin >= 0 ? ", skinned" : ", rigid on its joint")).c_str(), nd.skin >= 0 ? kTextSoft : kWarning);
		} else if (it.vertices > ed::kMaxMeshVertices) {
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, vec(kWarning));
			ImGui::TextUnformatted(ICON_TRIANGLE_ALERT " split: over 65,535 vertices");
			ImGui::PopStyleColor();
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
	if (changed) ed::revalidate(plan, e.base());
}

void page_materials(Editor &, ed::ImportPlan &plan) {
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
	section("Materials");
	if (plan.materials.empty()) ui::dim_text("No materials: meshes get a white one", kTextMuted);
	else if (ImGui::BeginTable("mats", 3, flags)) {
		ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableSetupColumn("Becomes", ImGuiTableColumnFlags_WidthStretch, 2);
		ImGui::TableSetupColumn("Base colour", ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableHeadersRow();
		for (auto &m : plan.materials) {
			ImGui::TableNextRow(0, ui::px(26));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(m.name.c_str());
			ImGui::TableNextColumn();
			if (m.rebuilt) {
				ImGui::PushStyleColor(ImGuiCol_Text, vec(kOk));
				ImGui::Text(ICON_CIRCLE_CHECK " %zu stages, as exported", m.stages);
				ImGui::PopStyleColor();
			} else {
				ImGui::TextUnformatted("1 stage (TextureManager)");
			}
			ImGui::TableNextColumn();
			if (m.image >= 0 && size_t(m.image) < plan.images.size()) ImGui::TextUnformatted(plan.images[size_t(m.image)].name.c_str());
			else {
				const auto &c = plan.scene.materials[size_t(m.material)].base_colour;
				ImGui::ColorButton("##c", ImVec4(c[0], c[1], c[2], c[3]), ImGuiColorEditFlags_NoTooltip, ImVec2(ui::px(16), ui::px(16)));
				ImGui::SameLine();
				ui::dim_text("flat colour (a 4x4 image)", kTextMuted);
			}
		}
		ImGui::EndTable();
	}
	section("Textures");
	if (plan.images.empty()) ui::dim_text("No images", kTextMuted);
	else if (ImGui::BeginTable("imgs", 3, flags)) {
		ImGui::TableSetupColumn("Image", ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableSetupColumn("In the file", ImGuiTableColumnFlags_WidthStretch, 1);
		ImGui::TableSetupColumn("Stored as", ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableHeadersRow();
		for (auto &im : plan.images) {
			ImGui::TableNextRow(0, ui::px(26));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(im.name.c_str());
			if (!im.used) ImGui::SameLine(), ui::dim_text("(unused)", kTextFaint);
			ImGui::TableNextColumn();
			ImGui::Text("%d x %d", im.width, im.height);
			ImGui::TableNextColumn();
			const auto &img = plan.scene.images[size_t(im.image)];
			if (!img.error.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, vec(kError));
				ImGui::TextUnformatted(img.error.c_str());
				ImGui::PopStyleColor();
			} else {
				bool resized = im.out_width != im.width || im.out_height != im.height;
				ImGui::PushStyleColor(ImGuiCol_Text, vec(resized ? kWarning : kTextSoft));
				ImGui::Text("Image\\Standard %d x %d%s", im.out_width, im.out_height, resized ? " (resized)" : "");
				ImGui::PopStyleColor();
			}
		}
		ImGui::EndTable();
	}
}

void page_review(Editor &e, ed::ImportPlan &plan) {
	section("Checks");
	for (auto &i : plan.issues) {
		unsigned c = i.severity == ed::Issue::Error ? kError : i.severity == ed::Issue::Warning ? kWarning : kOk;
		const char *icon = i.severity == ed::Issue::Error ? ICON_CIRCLE_X : i.severity == ed::Issue::Warning ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_CHECK;
		ImGui::PushStyleColor(ImGuiCol_Text, vec(c));
		ImGui::TextUnformatted(icon);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::PushTextWrapPos(0);
		ImGui::TextUnformatted(i.text.c_str());
		ImGui::PopTextWrapPos();
	}
	section("What it makes");
	ImGui::PushTextWrapPos(0);
	if (plan.settings.kind == ed::ImportKind::Robot) {
		size_t from_file = 0;
		for (auto &m : plan.har.sequences) from_file += m.animation >= 0;
		ImGui::TextUnformatted(("A new HAR \"" + plan.settings.name + "\", a variant of " + plan.har.base_robot + " (its scripts, moves, inputs, sounds and hitboxes; listed, unlocked) whose model is this file's: " +
		                        "three levels of detail skinned to " + std::to_string(plan.har.bones.size()) + " rig bones, the fighter material with a paint mask, and " +
		                        std::to_string(plan.har.sequences.size()) + " sequences (" + std::to_string(from_file) + " from the file, the rest retargeted). It opens in the HAR editor, " +
		                        "shows in Choose Creature and plays in the game.")
		                           .c_str());
	} else if (plan.settings.kind == ed::ImportKind::Prop) {
		ImGui::TextUnformatted(("A folder \"" + plan.settings.name + "\" with the prefab actor, its mesh (Mesh\\Standard), " + std::to_string(plan.materials.size()) +
		                        " material(s) (TextureProcess\\MultiStage) and their images, all with fresh GUIDs. Place copies from the Place panel.")
		                           .c_str());
	} else {
		ImGui::TextUnformatted(("A new arena \"" + plan.settings.name + "\": its description (listed, unlocked, " + std::to_string(std::max<size_t>(2, plan.count(ed::Role::Spawn))) +
		                        " players) and environment (lights, music and cameras from " + plan.settings.template_arena + "), " + std::to_string(plan.count(ed::Role::Visual)) +
		                        " drawn meshes, the fighting-surface collision, " + std::to_string(plan.count(ed::Role::Death) + (plan.count(ed::Role::Kill) ? 1 : 0)) + " death volume(s), " +
		                        std::to_string(plan.count(ed::Role::JumpPad)) + " jump pad(s), " + std::to_string(plan.count(ed::Role::Spawn)) + " spawn points, " +
		                        std::to_string(plan.count(ed::Role::Pickup)) + " pickup spawner(s)" + (plan.settings.generate_nav ? " and a navigation net" : "") + ".")
		                           .c_str());
	}
	ImGui::PopTextWrapPos();
	if (ed::Project *p = e.project()) {
		section("Source");
		ui::dim_text(("Kept in " + path_text(ed::sidecar_dir(*p) / "models" / plan.settings.name) + " so the import can be redone").c_str(), kTextMuted);
	}
}

}  // namespace

static bool is_robot(const ed::ImportPlan &p) { return p.settings.kind == ed::ImportKind::Robot; }

std::vector<ImportStep> &import_steps() {
	static std::vector<ImportStep> steps = {
	    {ICON_FILE_UP, "Source", page_source, nullptr, nullptr},
	    {ICON_LIST_TREE, "Contents", page_contents, nullptr, nullptr},
	    {ICON_PALETTE, "Materials", page_materials, nullptr, nullptr},
	    {ICON_SPLINE, "Bones", robot_page_bones, is_robot, robot_bones_done},
	    {ICON_VIDEO, "Animations", robot_page_animations, is_robot, nullptr},
	    {ICON_CIRCLE_DOT, "Hitboxes", robot_page_hitboxes, is_robot, nullptr},
	    {ICON_CIRCLE_CHECK, "Review", page_review, nullptr, [](const ed::ImportPlan &p) { return !p.errors(); }},
	};
	return steps;
}

void register_import_step(ImportStep step, const std::string &before) {
	auto &steps = import_steps();
	auto it = std::find_if(steps.begin(), steps.end(), [&](const ImportStep &s) { return s.title == before; });
	steps.insert(it, std::move(step));
}

void models_open_import(Editor &e, const fs::path &file) {
	load_plan(e, file, ed::ImportKind::Prop, false);
	state().open_request = true;
}

void models_drop(Editor &e, const fs::path &file) {
	std::string ext = file.extension().string();
	for (auto &c : ext) c = char(tolower(uint8_t(c)));
	if (ext == ".glb" || ext == ".gltf") models_open_import(e, file);
	else e.log(LogLine::Warning, "Only .glb and .gltf files can be dropped here (" + file.filename().string() + ")");
}

bool models_exportable(Editor &e, const ObjectRef &r) { return r && e.base() && !ed::export_kind(class_of(e, r)).empty(); }

void models_export_dialog(Editor &e, const ObjectRef &r) {
	if (!models_exportable(e, r)) return;
	std::string name = e.object_label(r);
	for (auto &c : name) {
		if (strchr("\\/:*?\"<>|", c)) c = '_';
	}
	fs::path start = state().last_dir.empty() ? (e.project() ? e.project()->path().parent_path() : fs::current_path()) : fs::path(state().last_dir);
	e.file_dialog().open(FileDialog::SaveFile, "Export to glTF", start, {".glb", ".gltf"},
	                     [&e, r](const fs::path &p) {
		                     try {
			                     fs::path out = p;
			                     if (out.extension() != ".glb" && out.extension() != ".gltf") out += ".glb";
			                     state().last_dir = path_text(out.parent_path());
			                     run_export(e, r, out);
		                     } catch (const std::exception &ex) {
			                     e.log(LogLine::Error, std::string("Export failed: ") + ex.what());
		                     }
	                     },
	                     name + ".glb");
}

void models_file_menu(Editor &e) {
	if (ImGui::MenuItem(ICON_DOWNLOAD "  Import 3D Model...", nullptr, false, e.base() != nullptr)) {
		fs::path start = state().last_dir.empty() ? (e.project() ? e.project()->path().parent_path() : fs::current_path()) : fs::path(state().last_dir);
		e.file_dialog().open(FileDialog::OpenFile, "Import a glTF model", start, {".glb", ".gltf"}, [&e](const fs::path &p) { models_open_import(e, p); });
	}
	bool can = models_exportable(e, e.selection());
	if (ImGui::MenuItem(ICON_UPLOAD "  Export Selected to glTF...", nullptr, false, can)) models_export_dialog(e, e.selection());
	if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
		ImGui::SetTooltip("Select a mesh, prop, arena or robot (FighterModel) in the Content Browser");
	}
}

void models_popups(Editor &e) {
	State &st = state();
	// A scripted dialog (screenshots) stays up through the first frames' layout and focus changes.
	if (st.open_request || (st.scripted && !ImGui::IsPopupOpen("Import 3D Model"))) {
		ImGui::OpenPopup("Import 3D Model");
		st.open_request = false;
	}
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(vp->WorkSize.x * 0.94f, ui::px(1240)), std::min(vp->WorkSize.y * 0.9f, ui::px(760))), ImGuiCond_Appearing);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(18), ui::px(14)));
	bool open = ImGui::BeginPopupModal("Import 3D Model", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
	ImGui::PopStyleVar();
	if (!open) return;
	ImGui::PushFont(theme::fonts.semibold);
	ImGui::TextUnformatted(ICON_DOWNLOAD "  Import 3D Model");
	ImGui::PopFont();
	ImGui::SameLine();
	ui::dim_text(st.have_plan ? path_text(st.plan.source).c_str() : "", kTextMuted);
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	if (!st.have_plan) {
		ImGui::PushStyleColor(ImGuiCol_Text, vec(kError));
		ImGui::TextWrapped("%s", st.error.c_str());
		ImGui::PopStyleColor();
		if (ui::toolbar_button(nullptr, "Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ed::ImportPlan &plan = st.plan;
	auto &all = import_steps();
	std::vector<size_t> steps;
	for (size_t k = 0; k < all.size(); ++k) {
		if (!all[k].shown || all[k].shown(plan)) steps.push_back(k);
	}
	st.step = std::clamp(st.step, 0, int(steps.size()) - 1);
	const float footer = ui::px(52);
	const float body_h = ImGui::GetContentRegionAvail().y - footer;
	const float steps_w = ui::px(190), preview_w = ImGui::GetContentRegionAvail().x * 0.42f;
	ui::begin_panel("##steps", ImVec2(steps_w, body_h), kPanelHead, true, 8, ImVec2(ui::px(8), ui::px(10)));
	for (size_t k = 0; k < steps.size(); ++k) {
		const ImportStep &s = all[steps[k]];
		bool active = int(k) == st.step;
		bool ok = !s.done || s.done(plan);
		ImGui::PushID(int(k));
		ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImVec2 sz(steps_w - ui::px(16), ui::px(40));
		if (ImGui::InvisibleButton("##s", sz)) st.step = int(k);
		ImDrawList *dl = ImGui::GetWindowDrawList();
		if (active) dl->AddRectFilled(p0, p0 + sz, rgb(kSelection), ui::px(6));
		else if (ImGui::IsItemHovered()) dl->AddRectFilled(p0, p0 + sz, rgb(0xffffff, 10), ui::px(6));
		ImVec2 c(p0.x + ui::px(16), p0.y + sz.y * 0.5f);
		dl->AddCircleFilled(c, ui::px(10), rgb(active ? kAccent : int(k) < st.step ? 0x2c2d38 : 0x22232c));
		dl->AddCircle(c, ui::px(10), rgb(active ? kAccentText : kInputBorder));
		char num[8];
		snprintf(num, sizeof num, "%d", int(k) + 1);
		ImVec2 ns = ImGui::CalcTextSize(num);
		dl->AddText(ImVec2(c.x - ns.x * 0.5f, c.y - ns.y * 0.5f), rgb(active ? 0xffffff : kTextDim), num);
		std::string label = std::string(s.icon ? s.icon : "") + "  " + s.title;
		dl->AddText(ImVec2(p0.x + ui::px(34), c.y - ImGui::GetFontSize() * 0.5f), rgb(active ? 0xffffff : kLabel), label.c_str());
		if (!ok) dl->AddText(ImVec2(p0.x + sz.x - ui::px(20), c.y - ImGui::GetFontSize() * 0.5f), rgb(kError), ICON_CIRCLE_X);
		ImGui::PopID();
	}
	ImGui::Dummy(ImVec2(0, ui::px(10)));
	int errors = 0, warnings = 0;
	for (auto &i : plan.issues) errors += i.severity == ed::Issue::Error, warnings += i.severity == ed::Issue::Warning;
	if (errors) ui::status_chip(ICON_CIRCLE_X, (std::to_string(errors) + " error" + (errors > 1 ? "s" : "")).c_str(), kError);
	if (warnings) ui::status_chip(ICON_TRIANGLE_ALERT, (std::to_string(warnings) + " warning" + (warnings > 1 ? "s" : "")).c_str(), kWarning);
	if (!errors && !warnings) ui::status_chip(ICON_CIRCLE_CHECK, "No problems", kOk);
	ui::end_panel();
	ImGui::SameLine(0, ui::px(12));
	const float page_w = ImGui::GetContentRegionAvail().x - preview_w - ui::px(12);
	ImGui::BeginChild("##page", ImVec2(page_w, body_h), ImGuiChildFlags_None);
	ImGui::PushFont(theme::fonts.semibold);
	ImGui::TextUnformatted(all[steps[size_t(st.step)]].title.c_str());
	ImGui::PopFont();
	all[steps[size_t(st.step)]].draw(e, plan);
	ImGui::EndChild();
	ImGui::SameLine(0, ui::px(12));
	ImGui::BeginGroup();
	if (plan.settings.kind == ed::ImportKind::Robot) {
		robot_preview(e, plan, ImVec2(preview_w, body_h));
	} else {
		if (st.preview.key != Preview::key_of(plan)) st.preview.build(plan);
		ImVec2 psize(preview_w, body_h - ui::px(34));
		st.preview.draw(psize);
		ImGui::Checkbox("Visual", &st.preview.show_visual);
		ImGui::SameLine();
		ImGui::Checkbox("Collision and gameplay", &st.preview.show_collision);
		ImGui::SameLine();
		ui::dim_text("drag to turn, scroll to zoom", kTextFaint);
	}
	ImGui::EndGroup();
	ImGui::Dummy(ImVec2(0, ui::px(8)));
	if (!st.error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, vec(kError));
		ImGui::TextUnformatted(st.error.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
	}
	const bool last = st.step + 1 >= int(steps.size());
	float right = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + right - ui::px(340));
	if (ui::toolbar_button(nullptr, "Cancel")) {
		st.preview.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine(0, ui::px(8));
	if (ui::toolbar_button(ICON_ARROW_LEFT, "Back", ui::Tone::Normal, st.step > 0)) --st.step;
	ImGui::SameLine(0, ui::px(8));
	if (!last) {
		if (ui::toolbar_button(nullptr, "Next " ICON_CHEVRON_RIGHT, ui::Tone::Accent)) ++st.step;
	} else if (ui::toolbar_button(ICON_DOWNLOAD, "Import", ui::Tone::Accent, !plan.errors() && !st.scripted,
	                              plan.errors() ? "Fix the errors first" : nullptr)) {
		e.with_addon("Importing " + plan.settings.name + " needs an add-on to put it in. Which one?", [&e] {
			if (commit(e)) state().preview.clear();
		});
		if (e.project()) ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void models_startup(Editor &e) {
	const Options &o = e.options();
	if (!e.base()) return;
	if (!o.export_spec.empty()) {
		ObjectRef r = find_spec(e, o.export_spec);
		if (!r) {
			e.log(LogLine::Error, "--export: nothing exportable named " + o.export_spec);
		} else {
			try {
				run_export(e, r, o.export_out.empty() ? fs::path(e.object_label(r) + ".glb") : o.export_out);
			} catch (const std::exception &ex) {
				e.log(LogLine::Error, std::string("Export failed: ") + ex.what());
			}
		}
	}
	if (o.import_file.empty()) return;
	ed::ImportKind kind = o.import_kind == "arena" ? ed::ImportKind::Arena : o.import_kind == "robot" ? ed::ImportKind::Robot : ed::ImportKind::Prop;
	load_plan(e, o.import_file, kind, !o.import_kind.empty());
	State &st = state();
	if (!st.have_plan) {
		e.log(LogLine::Error, "Cannot import " + path_text(o.import_file) + ": " + st.error);
		return;
	}
	if (!o.import_name.empty()) st.plan.settings.name = o.import_name;
	if (!o.import_template.empty()) st.plan.settings.template_arena = o.import_template;
	if (st.plan.settings.kind == ed::ImportKind::Robot && (!o.import_base.empty() || !o.import_name.empty())) {
		if (!o.import_base.empty()) st.plan.har.base_robot = o.import_base;
		robot_analyse(e, st.plan);
	}
	ed::revalidate(st.plan, e.base());
	if (o.import_dialog) {
		st.scripted = true;
		st.step = std::max(0, o.import_step);
		st.open_request = true;
		return;
	}
	for (auto &i : st.plan.issues) {
		if (i.severity != ed::Issue::Ok) e.log(i.severity == ed::Issue::Error ? LogLine::Error : LogLine::Warning, "Import: " + i.text);
	}
	if (st.plan.errors() || !e.project()) {
		if (!e.project()) e.log(LogLine::Error, "--import needs an add-on (name one on the command line)");
		return;
	}
	commit(e);
}
