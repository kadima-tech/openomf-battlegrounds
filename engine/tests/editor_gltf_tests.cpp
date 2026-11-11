// The content editor's glTF pipeline (docs/EDITOR_SPEC.md §5.5).
//
//   editor-gltf-tests                       reader / writer round trip on generated scenes (no game data)
//   editor-gltf-tests <Data dir>            also: export -> import of a base mesh and a robot, a prop
//                                           import, and a generated arena imported, saved and read by
//                                           the engine
//   editor-gltf-tests --samples <dir>       writes the generated test models (prop.glb, arena.glb)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

#include "addon.h"
#include "arena.h"
#include "content.h"
#include "de/scene.h"
#include "de/texture.h"
#include "game/session.h"
#include "gltf.h"
#include "model_export.h"
#include "model_import.h"

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                                   \
	do {                                                                              \
		if (!(cond)) {                                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                               \
		}                                                                             \
	} while (0)

static void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

static fs::path scratch() {
	fs::path d = fs::temp_directory_path() / ("openbg-gltf-tests-" + std::to_string(rand()));
	fs::create_directories(d);
	return d;
}

// A checker texture.
static gltf::Image checker(const std::string &name, int size, uint8_t r, uint8_t g, uint8_t b, bool alpha = false) {
	gltf::Image img;
	img.name = name;
	img.width = img.height = size;
	img.rgba.resize(size_t(size * size * 4));
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			bool on = ((x / 8) + (y / 8)) % 2;
			uint8_t *p = &img.rgba[size_t((y * size + x) * 4)];
			p[0] = on ? r : uint8_t(r / 2), p[1] = on ? g : uint8_t(g / 2), p[2] = on ? b : uint8_t(b / 2), p[3] = alpha ? uint8_t(x * 255 / size) : 255;
		}
	}
	return img;
}

// An axis-aligned box as 6 faces with their own normals and UVs (24 vertices), wound outwards.
static gltf::Primitive box(gltf::Vec3 lo, gltf::Vec3 hi, int material, float uv_scale = 1) {
	gltf::Primitive p;
	p.material = material;
	p.uvs.resize(1);
	const int faces[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
	const gltf::Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (int f = 0; f < 6; ++f) {
		uint32_t base = uint32_t(p.positions.size());
		const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
		for (int k = 0; k < 4; ++k) {
			int c = faces[f][k];
			p.positions.push_back({c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]});
			p.normals.push_back(normals[f]);
			p.uvs[0].push_back({uv[k][0] * uv_scale, uv[k][1] * uv_scale});
		}
		for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) p.indices.push_back(base + i);
	}
	return p;
}

static gltf::Material material(const std::string &name, int image, gltf::Vec4 colour = {1, 1, 1, 1}) {
	gltf::Material m;
	m.name = name;
	m.base_texture.image = image;
	m.base_colour = colour;
	m.metallic = 0;
	return m;
}

static int mesh_node(gltf::Scene &s, const std::string &name, gltf::Primitive p, int parent, gltf::Vec3 at = {0, 0, 0}) {
	gltf::Mesh m;
	m.name = name;
	m.primitives.push_back(std::move(p));
	s.meshes.push_back(std::move(m));
	gltf::Node n;
	n.name = name;
	n.mesh = int(s.meshes.size()) - 1;
	n.translation = at;
	return s.add_node(std::move(n), parent);
}

static int empty_node(gltf::Scene &s, const std::string &name, int parent, gltf::Vec3 at = {0, 0, 0}) {
	gltf::Node n;
	n.name = name;
	n.translation = at;
	return s.add_node(std::move(n), parent);
}

// A crate prop: a textured box with a lid of another material, a child node, a skin with two
// joints and an animation (the reader must keep them; props ignore them).
static gltf::Scene prop_scene() {
	gltf::Scene s;
	s.images.push_back(checker("CrateWood", 64, 200, 140, 60));
	s.images.push_back(checker("CrateLid", 32, 90, 90, 200, true));
	s.materials.push_back(material("Wood", 0));
	s.materials.push_back(material("Lid", 1));
	s.materials.back().alpha = gltf::Material::Alpha::Blend;
	int root = empty_node(s, "Crate", -1);
	mesh_node(s, "Body", box({-2, 0, -2}, {2, 4, 2}, 0), root);
	int lid = mesh_node(s, "LidMesh", box({-2.2f, 0, -2.2f}, {2.2f, 0.4f, 2.2f}, 1), root, {0, 4, 0});
	s.nodes[size_t(lid)].rotation = {0, 0.3826834f, 0, 0.9238795f};
	int j0 = empty_node(s, "Bone0", root), j1 = empty_node(s, "Bone1", j0, {0, 0.5f, 0});
	gltf::Skin sk;
	sk.name = "Rig";
	sk.joints = {j0, j1};
	sk.inverse_binds = {gltf::identity(), gltf::compose({0, -0.5f, 0}, {0, 0, 0, 1}, {1, 1, 1})};
	s.skins.push_back(sk);
	gltf::Animation a;
	a.name = "Wobble";
	gltf::Animation::Sampler smp;
	smp.input = {0, 0.5f, 1};
	smp.output = {0, 0, 0, 1, 0, 0.0998f, 0, 0.995f, 0, 0, 0, 1};
	a.samplers.push_back(smp);
	a.channels.push_back({0, j1, gltf::Animation::Path::Rotation});
	s.animations.push_back(a);
	s.extras = json::Value::object();
	s.extras["test"] = "prop";
	return s;
}

// A small arena in metres (10 DE units each): an 80 x 80 floor with a raised platform, four
// walls, a death plane below, a jump pad, 4 spawn points and 2 pickup spawners, in the §5.5
// collections.
static gltf::Scene arena_scene() {
	gltf::Scene s;
	s.images.push_back(checker("ArenaFloor", 128, 120, 120, 140));
	s.images.push_back(checker("ArenaWall", 64, 160, 80, 60));
	s.materials.push_back(material("FloorTiles", 0));
	s.materials.push_back(material("WallPanels", 1));
	gltf::Material floor_m;
	floor_m.name = "floor";
	gltf::Material wall_m;
	wall_m.name = "wall";
	gltf::Material kill_m;
	kill_m.name = "kill";
	s.materials.push_back(floor_m);
	s.materials.push_back(wall_m);
	s.materials.push_back(kill_m);
	const int floor_c = 2, wall_c = 3, kill_c = 4;
	int visual = empty_node(s, "Visual", -1), collision = empty_node(s, "Collision", -1), death = empty_node(s, "Death", -1);
	int jump = empty_node(s, "JumpPad", -1), spawn = empty_node(s, "Spawn", -1), pickup = empty_node(s, "Pickup", -1);
	struct Box {
		const char *name;
		gltf::Vec3 lo, hi;
	};
	const Box ground{"Ground", {-40, -2, -40}, {40, 0, 40}}, platform{"Platform", {10, 0, 10}, {30, 2, 30}};
	const Box walls[4] = {{"WallN", {-40, 0, -42}, {40, 6, -40}}, {"WallS", {-40, 0, 40}, {40, 6, 42}}, {"WallW", {-42, 0, -40}, {-40, 6, 40}}, {"WallE", {40, 0, -40}, {42, 6, 40}}};
	mesh_node(s, ground.name, box(ground.lo, ground.hi, 0, 8), visual);
	mesh_node(s, platform.name, box(platform.lo, platform.hi, 0, 2), visual);
	for (auto &w : walls) mesh_node(s, w.name, box(w.lo, w.hi, 1, 4), visual);
	mesh_node(s, "GroundCollision", box(ground.lo, ground.hi, floor_c), collision);
	mesh_node(s, "PlatformCollision", box(platform.lo, platform.hi, floor_c), collision);
	for (auto &w : walls) mesh_node(s, std::string(w.name) + "Collision", box(w.lo, w.hi, wall_c), collision);
	mesh_node(s, "DeathPlane", box({-150, -52, -150}, {150, -50, 150}, kill_c), death);
	int pad = mesh_node(s, "Pad", box({-30, 0, 15}, {-26, 0.2f, 19}, floor_c), jump);
	s.nodes[size_t(pad)].extras = json::Value::object();
	json::Value imp = json::Value::array();
	imp.push(0.0), imp.push(1.0), imp.push(0.3);
	s.nodes[size_t(pad)].extras["impulse"] = imp;
	empty_node(s, "Spawn1", spawn, {-25, 0, -25});
	empty_node(s, "Spawn2", spawn, {25, 0, -25});
	empty_node(s, "Spawn3", spawn, {-25, 0, 0});
	empty_node(s, "Spawn4", spawn, {0, 0, 25});
	int p1 = empty_node(s, "EnergyPad", pickup, {0, 0, 0});
	s.nodes[size_t(p1)].extras = json::Value::object();
	json::Value pk = json::Value::array();
	pk.push("EnergyBoost");
	s.nodes[size_t(p1)].extras["pickups"] = pk;
	empty_node(s, "TopPad", pickup, {20, 2, 20});
	return s;
}

static void test_round_trip() {
	gltf::Scene s = prop_scene();
	fs::path dir = scratch();
	for (const char *name : {"prop.glb", "prop.gltf"}) {
		gltf::write(s, dir / name);
		gltf::Scene r = gltf::read(dir / name);
		CHECK(r.nodes.size() == s.nodes.size());
		CHECK(r.meshes.size() == s.meshes.size());
		CHECK(r.materials.size() == 2 && r.materials[1].alpha == gltf::Material::Alpha::Blend && r.materials[0].base_texture.image == 0);
		CHECK(r.images.size() == 2 && r.images[0].width == 64 && r.images[0].rgba == s.images[0].rgba && r.images[1].rgba == s.images[1].rgba);
		CHECK(r.skins.size() == 1 && r.skins[0].joints.size() == 2);
		CHECK(r.animations.size() == 1 && r.animations[0].channels.size() == 1 && std::fabs(r.animations[0].duration() - 1) < 1e-6f);
		CHECK(r.extras.is_object() && r.extras.find("test") && r.extras.find("test")->as_string() == "prop");
		bool same = true;
		for (size_t m = 0; m < s.meshes.size(); ++m) {
			const auto &a = s.meshes[m].primitives[0], &b = r.meshes[m].primitives[0];
			same = same && a.positions == b.positions && a.indices == b.indices && a.normals == b.normals && a.uvs == b.uvs;
		}
		CHECK(same);
		gltf::Mat4 w = r.world(2);
		CHECK(std::fabs(w[13] - 4) < 1e-6f);
		CHECK(r.path(2).size() == 2 && r.path(2)[0] == "Crate");
	}
	// An unindexed triangle strip and a JPEG-less file with an error image still read.
	gltf::Scene bad;
	gltf::Image broken;
	broken.name = "Broken";
	bad.images.push_back(broken);
	gltf::write(bad, dir / "bad.glb");
	gltf::Scene rb = gltf::read(dir / "bad.glb");
	CHECK(rb.images.size() == 1 && rb.images[0].width == 1);
	// Space conversion is its own inverse.
	de::Transform t;
	t.r = {0, 0, -1, 0, 1, 0, 1, 0, 0};
	t.t = {10, 20, 30};
	de::Transform back = ed::from_gltf(ed::to_gltf(t, 100), 100);
	float err = 0;
	for (int k = 0; k < 9; ++k) err = std::max(err, std::fabs(back.r[size_t(k)] - t.r[size_t(k)]));
	for (int k = 0; k < 3; ++k) err = std::max(err, std::fabs(back.t[size_t(k)] - t.t[size_t(k)]));
	CHECK(err < 1e-4f);
	fs::remove_all(dir);
	printf("glTF reader / writer round trip: ok\n");
}

// The add-on as the engine reads it, in a world with the base files the add-on reaches.
struct Loaded {
	std::unique_ptr<de::World> world;
	const de::ObjectsFile *addon = nullptr;
};
static Loaded load_addon(const fs::path &data, const ed::Project &p, const fs::path &file) {
	Loaded l;
	auto stems = ed::arena_files(p.base(), "", &p.file());
	std::set<std::string> keep(stems.begin(), stems.end());
	set_env("OPENBG_ADDONS", file.string());
	l.world = std::make_unique<de::World>(data, [&](const std::string &st) { return keep.count(st) > 0; });
	set_env("OPENBG_ADDONS", "");
	l.addon = l.world->file(file.stem().string());
	return l;
}

// Per-face geometry and UVs of a mesh, as sets (order-free), for comparing a mesh with its import.
static std::multiset<std::vector<int>> face_set(const de::Mesh &m, const std::vector<de::Material> &mats) {
	std::map<uint32_t, std::array<std::array<float, 2>, 3>> uv;
	for (auto &mat : mats) {
		if (const de::Layer *l = mat.base()) {
			for (auto &fu : l->uvs) uv[fu.face] = fu.uv;
		}
	}
	std::multiset<std::vector<int>> out;
	for (uint32_t f = 0; f < m.faces.size(); ++f) {
		std::vector<int> key;
		for (int k = 0; k < 3; ++k) {
			for (float c : m.vertices[m.faces[f].v[size_t(k)]]) key.push_back(int(std::lround(c * 100)));
			if (auto it = uv.find(f); it != uv.end()) key.push_back(int(std::lround(it->second[size_t(k)][0] * 1000))), key.push_back(int(std::lround(it->second[size_t(k)][1] * 1000)));
		}
		out.insert(key);
	}
	return out;
}

static void test_mesh_round_trip(const fs::path &data, ed::BaseData &base) {
	// A textured base mesh with several materials: export, import as a prop, compare.
	const char *stem = "Grudge Match Arena";
	de::World w(data, [&](const std::string &s) { return s == stem || s == "SharedObjects"; });
	const de::ObjectsFile *f = w.file(stem);
	CHECK(f);
	if (!f) return;
	de::ObjectRef mesh;
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		if (f->objects()[i].class_path == "Mesh\\Standard" && f->objects()[i].name == "FlameThrower Base Mesh") mesh = {f, i};
	}
	CHECK(mesh);
	if (!mesh) return;
	de::Mesh src = de::read_mesh(*f, mesh.info());
	auto src_mats = de::read_materials(w, mesh);
	gltf::Scene s = ed::export_object(w, mesh);
	CHECK(s.meshes.size() == 1 && !s.images.empty());
	fs::path dir = scratch();
	gltf::write(s, dir / "mesh.glb");
	ed::ImportSettings st;
	st.kind = ed::ImportKind::Prop;
	ed::ImportPlan plan = ed::analyse(dir / "mesh.glb", st);
	CHECK(plan.from_editor && !plan.errors());
	auto project = ed::Project::create(base, dir / "GltfMesh.DEObjects");
	ed::ImportResult r = ed::import_prop(*project, plan);
	CHECK(r.meshes == 1 && r.materials == src_mats.size());
	project->save();
	Loaded l = load_addon(data, *project, dir / "GltfMesh.DEObjects");
	CHECK(l.addon);
	if (!l.addon) return;
	de::ObjectRef imported;
	for (uint32_t i = 0; i < l.addon->objects().size(); ++i) {
		if (l.addon->objects()[i].class_path == "Mesh\\Standard") imported = {l.addon, i};
	}
	CHECK(imported);
	if (!imported) return;
	de::Mesh got = de::read_mesh(*l.addon, imported.info());
	auto got_mats = de::read_materials(*l.world, imported);
	CHECK(got.vertices.size() == src.vertices.size());
	CHECK(got.faces.size() == src.faces.size());
	bool same_faces = got.faces.size() == src.faces.size();
	for (size_t i = 0; same_faces && i < got.faces.size(); ++i) {
		same_faces = got.faces[i].v == src.faces[i].v && got.faces[i].flags == src.faces[i].flags && got.faces[i].adjacent == src.faces[i].adjacent;
	}
	CHECK(same_faces);
	float err = 0;
	for (size_t i = 0; i < std::min(got.vertices.size(), src.vertices.size()); ++i) {
		for (int k = 0; k < 3; ++k) err = std::max(err, std::fabs(got.vertices[i][size_t(k)] - src.vertices[i][size_t(k)]));
	}
	CHECK(err < 1e-3f);
	CHECK(got.flags == src.flags);
	if (got.flags != src.flags) fprintf(stderr, "mesh flags %x, expected %x\n", got.flags, src.flags);
	CHECK(got_mats.size() == src_mats.size());
	for (size_t m = 0; m < std::min(got_mats.size(), src_mats.size()); ++m) {
		CHECK(got_mats[m].name == src_mats[m].name);
		CHECK(got_mats[m].layers.size() == src_mats[m].layers.size());
		for (size_t k = 0; k < std::min(got_mats[m].layers.size(), src_mats[m].layers.size()); ++k) {
			const de::Layer &a = got_mats[m].layers[k], &b = src_mats[m].layers[k];
			CHECK(a.mode == b.mode && a.stacked == b.stacked && a.environment == b.environment && a.colour == b.colour);
			CHECK(a.uvs.size() == b.uvs.size());
			bool uv_same = a.uvs.size() == b.uvs.size();
			for (size_t u = 0; uv_same && u < a.uvs.size(); ++u) uv_same = a.uvs[u].face == b.uvs[u].face && a.uvs[u].uv == b.uvs[u].uv;
			CHECK(uv_same);
			if (a.image && b.image) {
				auto ia = de::read_image(*a.image.file, a.image.info()), ib = de::read_image(*b.image.file, b.image.info());
				CHECK(ia.width == ib.width && ia.height == ib.height && ia.rgba() == ib.rgba());
			} else {
				CHECK(!a.image == !b.image);
			}
		}
	}
	CHECK(face_set(got, got_mats) == face_set(src, src_mats));
	fs::remove_all(dir);
	printf("mesh export -> import round trip (%s, %zu faces, %zu materials): ok\n", mesh.info().name.c_str(), src.faces.size(), src_mats.size());
}

static void test_robot_export(const fs::path &data) {
	de::World w(data, [](const std::string &s) { return s == "Jaguar" || s == "JaguarModel1" || s == "SharedObjects"; });
	const de::ObjectsFile *f = w.file("JaguarModel1");
	CHECK(f);
	if (!f) return;
	de::ObjectRef model;
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		if (f->objects()[i].class_path == "FighterModel") model = {f, i};
	}
	gltf::Scene s = ed::export_object(w, model);
	CHECK(s.skins.size() == 3);
	CHECK(s.animations.size() > 50);
	size_t joints = s.skins.empty() ? 0 : s.skins[0].joints.size();
	CHECK(joints >= 15);
	bool weights = true;
	for (auto &p : s.meshes[0].primitives) {
		for (auto &wt : p.weights) weights = weights && std::fabs(wt[0] + wt[1] + wt[2] + wt[3] - 1) < 1e-3f;
	}
	CHECK(weights);
	fs::path dir = scratch();
	gltf::write(s, dir / "jaguar.glb");
	gltf::Scene r = gltf::read(dir / "jaguar.glb");
	CHECK(r.skins.size() == 3 && r.animations.size() == s.animations.size());
	float lo = 1e30f, hi = -1e30f;
	for (auto &p : r.meshes[0].primitives) {
		for (auto &v : p.positions) lo = std::min(lo, v[1]), hi = std::max(hi, v[1]);
	}
	printf("robot export (Jaguar): %zu joints, %zu animations, %zu LODs, height %.2f m: ok\n", joints, r.animations.size(), r.skins.size(), hi - lo);
	fs::remove_all(dir);
}

static void test_prop_import(const fs::path &data, ed::BaseData &base) {
	fs::path dir = scratch();
	gltf::write(prop_scene(), dir / "crate.glb");
	ed::ImportSettings st;
	st.kind = ed::ImportKind::Prop;
	st.name = "Crate";
	ed::ImportPlan plan = ed::analyse(dir / "crate.glb", st);
	CHECK(plan.count(ed::Role::Visual) == 2);
	CHECK(!plan.errors());
	auto project = ed::Project::create(base, dir / "GltfProp.DEObjects");
	ed::ImportResult r = ed::import_prop(*project, plan);
	CHECK(r.meshes == 1 && r.materials == 2 && r.images == 2);
	CHECK(ed::prefabs(*project).size() == 1);
	auto v = ed::validate(*project);
	for (auto &i : v) {
		if (i.severity == ed::Issue::Error) fprintf(stderr, "validation: %s\n", i.text.c_str());
		CHECK(i.severity != ed::Issue::Error);
	}
	project->save();
	Loaded l = load_addon(data, *project, dir / "GltfProp.DEObjects");
	CHECK(l.addon);
	if (l.addon) {
		de::ObjectRef actor;
		for (uint32_t i = 0; i < l.addon->objects().size(); ++i) {
			if (l.addon->objects()[i].class_path == "Actor" && l.addon->objects()[i].name == "Crate") actor = {l.addon, i};
		}
		CHECK(actor);
		auto meshes = actor ? de::actor_meshes(*l.world, actor, false) : std::vector<de::ObjectRef>{};
		CHECK(meshes.size() == 1);
		if (!meshes.empty()) {
			de::Mesh m = de::read_mesh(*l.addon, meshes[0].info());
			CHECK(m.faces.size() == 24);
			CHECK(m.vertices.size() == 16);
			auto mats = de::read_materials(*l.world, meshes[0]);
			CHECK(mats.size() == 2);
			size_t covered = 0;
			for (auto &mat : mats) {
				CHECK(mat.layers.size() == 1 && mat.layers[0].image);
				covered += mat.layers.empty() ? 0 : mat.layers[0].uvs.size();
			}
			CHECK(covered == 24);
			float top = -1e30f;
			for (auto &vx : m.vertices) top = std::max(top, vx[1]);
			CHECK(std::fabs(top - 44) < 0.01f);
		}
	}
	fs::remove_all(dir);
	printf("prop import (generated crate): ok\n");
}

static void test_arena_import(const fs::path &data, ed::BaseData &base) {
	fs::path dir = scratch();
	gltf::write(arena_scene(), dir / "arena.glb");
	ed::ImportSettings st;
	st.kind = ed::ImportKind::Arena;
	st.name = "Test Box Arena";
	ed::ImportPlan plan = ed::analyse(dir / "arena.glb", st);
	ed::revalidate(plan, &base);
	CHECK(plan.count(ed::Role::Visual) == 6);
	CHECK(plan.count(ed::Role::Floor) == 2);
	CHECK(plan.count(ed::Role::Wall) == 4);
	CHECK(plan.count(ed::Role::Death) == 1);
	CHECK(plan.count(ed::Role::JumpPad) == 1);
	CHECK(plan.count(ed::Role::Spawn) == 4);
	CHECK(plan.count(ed::Role::Pickup) == 2);
	for (auto &i : plan.issues) {
		if (i.severity == ed::Issue::Error) fprintf(stderr, "plan: %s\n", i.text.c_str());
	}
	CHECK(!plan.errors());
	auto project = ed::Project::create(base, dir / "GltfArena.DEObjects");
	ed::ImportResult r = ed::import_arena(*project, plan);
	printf("arena import: %s\n", r.summary().c_str());
	for (auto &n : r.notes) printf("  note: %s\n", n.c_str());
	CHECK(r.spawns == 4 && r.pickups == 2);
	CHECK(r.nav_areas >= 4 && r.nav_areas <= ed::kMaxNavAreas && r.nav_links <= ed::kMaxNavLinks);
	auto arenas = ed::addon_arenas(*project);
	CHECK(arenas.size() == 1);
	auto v = ed::validate(*project);
	for (auto &i : v) {
		if (i.severity == ed::Issue::Error) fprintf(stderr, "validation: %s\n", i.text.c_str());
		CHECK(i.severity != ed::Issue::Error);
	}
	project->save();
	fs::path keep = ed::keep_source(*project, dir / "arena.glb", st.name);
	CHECK(fs::exists(keep));
	Loaded l = load_addon(data, *project, dir / "GltfArena.DEObjects");
	CHECK(l.addon);
	if (!l.addon || arenas.empty()) return;
	uint32_t env = 0;
	for (uint32_t i = 0; i < l.addon->objects().size(); ++i) {
		if (l.addon->objects()[i].class_path == "Actor\\Creature\\Environment") env = i;
	}
	ed::ArenaScene sc = ed::read_arena_scene(*l.world, *l.addon, env);
	auto spawns = sc.of_kind(ed::NodeKind::SpawnPoint);
	CHECK(spawns.size() == 4);
	CHECK(sc.of_kind(ed::NodeKind::PickupSpawner).size() == 2);
	CHECK(sc.of_kind(ed::NodeKind::DeathVolume).size() == 1);
	CHECK(!sc.of_kind(ed::NodeKind::Surface).empty());
	CHECK(sc.of_kind(ed::NodeKind::NavArea).size() == r.nav_areas);
	auto issues = ed::validate_arena(sc, 4);
	for (auto &i : issues) {
		printf("  arena check: %s\n", i.text.c_str());
		CHECK(i.severity == ed::Issue::Ok);
	}
	// The fighting surface under a spawn point is the floor (y 0), and the platform's top at 20.
	if (!spawns.empty()) {
		auto y = sc.surface_below(sc.nodes[size_t(spawns[0])].world.t, 400);
		CHECK(y && std::fabs(*y) < 0.5f);
	}
	auto top = sc.surface_below({200, 500, -200}, 1000);
	CHECK(top && std::fabs(*top - 20) < 0.5f);
	size_t drawn = 0;
	for (auto &item : de::collect_scene(*l.world, *l.addon)) drawn += item.mesh.faces.size();
	CHECK(drawn >= 6 * 12);
	fs::remove_all(dir);
	printf("arena import (generated box arena): ok\n");
}

int main(int argc, char **argv) {
	if (argc > 2 && !strcmp(argv[1], "--samples")) {
		fs::path dir = argv[2];
		fs::create_directories(dir);
		gltf::write(prop_scene(), dir / "prop.glb");
		gltf::write(arena_scene(), dir / "arena.glb");
		printf("wrote %s and %s\n", (dir / "prop.glb").string().c_str(), (dir / "arena.glb").string().c_str());
		return 0;
	}
	srand(unsigned(time(nullptr)));
	test_round_trip();
	if (argc > 1) {
		fs::path data = argv[1];
		if (!fs::exists(data)) {
			printf("no game data at %s: skipping the import tests\n", data.string().c_str());
			return failures ? 1 : 0;
		}
		ed::BaseData base(data);
		test_mesh_round_trip(data, base);
		test_robot_export(data);
		test_prop_import(data, base);
		test_arena_import(data, base);
	}
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all glTF checks passed\n");
	return 0;
}
