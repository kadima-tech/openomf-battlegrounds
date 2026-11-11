#include "model_export.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "arena.h"
#include "de/anim.h"
#include "de/image.h"
#include "de/texture.h"
#include "game/session.h"

namespace ed {

gltf::Vec3 to_gltf(const std::array<float, 3> &p, float u) { return {p[0] / u, p[1] / u, -p[2] / u}; }
std::array<float, 3> from_gltf(const gltf::Vec3 &p, float u) { return {p[0] * u, p[1] * u, -p[2] * u}; }

gltf::Mat4 to_gltf(const de::Transform &t, float u) {
	// Columns of the DE matrix are its rows r0..r2 (the local axes); S M S negates row 2 and column 2.
	gltf::Mat4 m = gltf::identity();
	for (int c = 0; c < 3; ++c) {
		for (int r = 0; r < 3; ++r) {
			float v = t.r[size_t(c * 3 + r)];
			if ((c == 2) != (r == 2)) v = -v;
			m[size_t(c * 4 + r)] = v;
		}
	}
	auto p = to_gltf(t.t, u);
	m[12] = p[0], m[13] = p[1], m[14] = p[2];
	return m;
}

de::Transform from_gltf(const gltf::Mat4 &m, float u) {
	de::Transform t;
	std::array<std::array<float, 3>, 3> axis;
	for (int c = 0; c < 3; ++c) {
		for (int r = 0; r < 3; ++r) {
			float v = m[size_t(c * 4 + r)];
			if ((c == 2) != (r == 2)) v = -v;
			axis[size_t(c)][size_t(r)] = v;
		}
	}
	// Gram-Schmidt: scale goes to the vertices, the actor keeps a rotation.
	auto norm = [](std::array<float, 3> v) {
		float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		return l > 1e-12f ? std::array<float, 3>{v[0] / l, v[1] / l, v[2] / l} : v;
	};
	auto dot = [](const std::array<float, 3> &a, const std::array<float, 3> &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	auto x = norm(axis[0]);
	auto y = axis[1];
	float d = dot(x, y);
	y = norm({y[0] - d * x[0], y[1] - d * x[1], y[2] - d * x[2]});
	std::array<float, 3> z{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
	if (dot(z, axis[2]) < 0) z = {-z[0], -z[1], -z[2]};
	for (int k = 0; k < 3; ++k) t.r[size_t(k)] = x[size_t(k)], t.r[size_t(3 + k)] = y[size_t(k)], t.r[size_t(6 + k)] = z[size_t(k)];
	t.t = from_gltf(gltf::Vec3{m[12], m[13], m[14]}, u);
	return t;
}

std::string export_kind(const std::string &c) {
	if (c == "Mesh\\Standard") return "mesh";
	if (c == "Mesh\\LODSwitchingMesh") return "lod";
	if (c == "FighterModel" || c == "Description\\Fighter" || c == "Description\\FighterModel") return "robot";
	if (c == "Actor\\Creature\\Environment" || c == "Description\\Environment") return "arena";
	if (c == "Actor" || c == "Actor\\PassiveReference" || c.rfind("Actor\\Affector", 0) == 0) return "prop";
	return "";
}

std::vector<std::array<std::array<float, 3>, 3>> corner_normals(const de::Mesh &mesh) {
	const auto &pos = mesh.vertices;
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
		else n = {0, 0, 1};
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

namespace {

std::string guid_text(const de::Guid &g) {
	static const char *hex = "0123456789abcdef";
	std::string s;
	for (int i = 0; i < 16; ++i) s += hex[g.bytes[i] >> 4], s += hex[g.bytes[i] & 15];
	return s;
}

json::Value jvec(const float *v, size_t n) {
	json::Value a = json::Value::array();
	for (size_t i = 0; i < n; ++i) a.push(double(v[i]));
	return a;
}

// TextureProcess\MultiStage references chunk 2 as stored (de::read_multistage reads the same).
struct StageRecord {
	de::Reference manager, colour_value;
	std::array<float, 4> colour{1, 1, 1, 1};
	int32_t mode = 0, marked = 0;
	bool stacked = false, flag_30 = false, flag_v7 = false, disabled = false;
};
std::vector<StageRecord> read_stage_records(const de::ObjectsFile::ObjectData &d, int *version) {
	std::vector<StageRecord> out;
	*version = 0;
	try {
		de::Contents c(d.references);
		for (int skip = 0; skip < 2; ++skip) {
			de::Chunk ch = de::begin_chunk(c);
			c.seek(ch.end);
		}
		de::Chunk ms = de::begin_chunk(c);
		*version = ms.version;
		if (ms.version < 4 || ms.version > 8) return out;
		int32_t n = c.i32();
		for (int32_t i = 0; i < n && c.pos() < ms.end; ++i) {
			StageRecord st;
			st.manager = de::Reference::read(c);
			if (c.boolean()) st.colour_value = de::Reference::read(c);
			else
				for (float &v : st.colour) v = c.f32();
			if (ms.version == 4) {
				uint32_t len = c.u32();
				auto b = c.bytes(len);
				std::string name(b.begin(), b.end());
				while (!name.empty() && name.back() == 0) name.pop_back();
				st.mode = name == "Add" ? 1 : name == "Modulate" ? 2 : name == "AddToLightMap" ? 3 : name == "SemiTranslucent" ? 4 : 0;
			} else {
				st.mode = c.i32();
			}
			st.stacked = c.boolean();
			st.marked = c.i32();
			if (ms.version >= 6) st.flag_30 = c.boolean();
			if (ms.version == 7) st.flag_v7 = c.boolean();
			if (ms.version == 8) st.disabled = c.boolean();
			out.push_back(st);
		}
	} catch (const std::exception &) {
	}
	return out;
}

std::vector<de::ObjectRef> manager_images(const de::World &w, const de::ObjectRef &tm) {
	std::vector<de::ObjectRef> out;
	auto d = tm.file->object_data(tm.info());
	try {
		de::Contents c(d.references);
		de::Chunk obj = de::begin_chunk(c);
		c.seek(obj.end);
		if (c.remaining() == 0) return out;
		de::Chunk ch = de::begin_chunk(c);
		if (ch.version == 1 || ch.version == 2) {
			int32_t n = c.i32();
			for (int32_t i = 0; i < n && c.pos() < ch.end; ++i) out.push_back(w.resolve(*tm.file, de::Reference::read(c)));
		}
	} catch (const std::exception &) {
	}
	return out;
}

struct Exporter {
	const de::World &w;
	ExportOptions opt;
	gltf::Scene s;
	std::map<std::pair<uint64_t, uint32_t>, int> images, meshes;
	std::map<std::pair<uint64_t, uint32_t>, std::vector<uint16_t>> mesh_vertices;  // glTF vertex -> DE vertex, per primitive concatenated

	static std::pair<uint64_t, uint32_t> key(const de::ObjectRef &r) { return {r.file->serial(), r.index}; }

	int image(const de::ObjectRef &r) {
		if (!r || r.info().class_path != "Image\\Standard") return -1;
		auto k = key(r);
		if (auto it = images.find(k); it != images.end()) return it->second;
		gltf::Image img;
		img.name = r.info().name;
		try {
			de::Image di = de::read_image(*r.file, r.info());
			img.width = di.width, img.height = di.height;
			img.rgba = di.rgba();
		} catch (const std::exception &e) {
			s.warnings.push_back("image " + img.name + ": " + e.what());
		}
		int id = int(s.images.size());
		s.images.push_back(std::move(img));
		images[k] = id;
		return id;
	}

	// One glTF mesh for a Mesh\Standard: a primitive per material, plus one for faces no material covers.
	int mesh(const de::ObjectRef &mo) {
		auto k = key(mo);
		if (auto it = meshes.find(k); it != meshes.end()) return it->second;
		de::Mesh m = de::read_mesh(*mo.file, mo.info());
		auto normals = corner_normals(m);
		gltf::Mesh gm;
		gm.name = mo.info().name;
		json::Value flags = json::Value::array();
		for (auto &f : m.faces) flags.push(int64_t(f.flags));
		gm.extras = json::Value::object();
		gm.extras["de_mesh_flags"] = int64_t(m.flags);
		gm.extras["de_placement"] = m.placement_times;
		gm.extras["de_vertex_count"] = int64_t(m.vertices.size());
		gm.extras["de_face_flags"] = std::move(flags);
		std::vector<bool> used(m.faces.size(), false);
		auto mat_objects = de::ObjectsFile::children(mo.file->object_data(mo.info()));
		for (uint32_t c : mat_objects) {
			const auto &mi = mo.file->objects()[c];
			if (mi.class_path != "TextureProcess\\MultiStage") continue;
			int version = 0;
			auto records = read_stage_records(mo.file->object_data(mi), &version);
			struct Stage {
				StageRecord rec;
				de::ObjectRef tm;
				std::map<uint32_t, std::array<std::array<float, 2>, 3>> uv;
				std::vector<de::ObjectRef> imgs;
				int set = -1;
			};
			std::vector<Stage> stages;
			for (auto &r : records) {
				Stage st;
				st.rec = r;
				st.tm = w.resolve(*mo.file, r.manager);
				if (st.tm && st.tm.info().class_path.rfind("TextureManager", 0) == 0) {
					try {
						for (auto &fu : de::read_texture_uvs(*st.tm.file, st.tm.info())) st.uv[fu.face] = fu.uv;
					} catch (const std::exception &) {
					}
					st.imgs = manager_images(w, st.tm);
				}
				stages.push_back(std::move(st));
			}
			// Faces of this material: those any stage has coordinates for.
			std::vector<uint32_t> faces;
			for (auto &st : stages) {
				for (auto &[f, uv] : st.uv) {
					if (f < m.faces.size() && !used[f]) faces.push_back(f), used[f] = true;
				}
			}
			std::sort(faces.begin(), faces.end());
			// UV sets: the base stage first, identical layouts shared.
			std::vector<const Stage *> set_owner;
			auto base_stage = std::find_if(stages.begin(), stages.end(), [](const Stage &st) {
				return st.tm && st.tm.info().class_path == "TextureManager" && !st.uv.empty();
			});
			auto assign = [&](Stage &st) {
				if (st.uv.empty() || st.set >= 0) return;
				for (size_t k = 0; k < set_owner.size(); ++k) {
					if (set_owner[k]->uv == st.uv) {
						st.set = int(k);
						return;
					}
				}
				st.set = int(set_owner.size());
				set_owner.push_back(&st);
			};
			if (base_stage != stages.end()) assign(*base_stage);
			for (auto &st : stages) assign(st);
			gltf::Material gmat;
			gmat.name = mi.name;
			gmat.metallic = 0;
			gmat.roughness = 1;
			json::Value jst = json::Value::array();
			for (auto &st : stages) {
				json::Value v = json::Value::object();
				v["name"] = st.tm ? st.tm.info().name : std::string();
				v["class"] = st.tm ? st.tm.info().class_path : std::string();
				int img = -1;
				de::ObjectRef image_obj, cube;
				for (auto &r : st.imgs) {
					if (!r) continue;
					if (r.info().class_path == "Image\\Standard" && !image_obj) image_obj = r;
					if (r.info().class_path == "Image\\CubeEnvironment" && !cube) cube = r;
				}
				if (image_obj) img = image(image_obj);
				v["image"] = img;
				if (image_obj) v["image_guid"] = guid_text(image_obj.info().guid);
				if (cube) v["cube_guid"] = guid_text(cube.info().guid);
				if (st.tm) v["manager_guid"] = guid_text(st.tm.info().guid);
				v["uv_set"] = st.set;
				v["mode"] = st.rec.mode;
				v["colour"] = jvec(st.rec.colour.data(), 4);
				if (de::ObjectRef cv = w.resolve(*mo.file, st.rec.colour_value)) v["colour_value_guid"] = guid_text(cv.info().guid);
				v["stacked"] = st.rec.stacked;
				v["marked"] = st.rec.marked;
				v["flag_30"] = st.rec.flag_30;
				v["flag_v7"] = st.rec.flag_v7;
				v["disabled"] = st.rec.disabled;
				jst.push(std::move(v));
				if (!gmat.base_texture && img >= 0 && st.set >= 0 && st.tm.info().class_path == "TextureManager" && !st.rec.disabled) {
					gmat.base_texture.image = img;
					gmat.base_texture.uv_set = st.set;
					if (st.rec.mode == 4) gmat.alpha = gltf::Material::Alpha::Blend;
				}
			}
			gmat.extras = json::Value::object();
			gmat.extras["de_stages"] = std::move(jst);
			gmat.extras["de_multistage_version"] = version;
			gmat.extras["de_guid"] = guid_text(mi.guid);
			if (m.flags & 0x4000) gmat.double_sided = true;
			int mat_index = int(s.materials.size());
			s.materials.push_back(std::move(gmat));
			if (faces.empty()) continue;
			std::vector<std::map<uint32_t, std::array<std::array<float, 2>, 3>> *> sets;
			for (auto *o : set_owner) sets.push_back(const_cast<std::map<uint32_t, std::array<std::array<float, 2>, 3>> *>(&o->uv));
			gm.primitives.push_back(primitive(m, normals, faces, sets, mat_index));
		}
		std::vector<uint32_t> rest;
		for (uint32_t f = 0; f < m.faces.size(); ++f) {
			if (!used[f]) rest.push_back(f);
		}
		if (!rest.empty()) gm.primitives.push_back(primitive(m, normals, rest, {}, -1));
		int id = int(s.meshes.size());
		s.meshes.push_back(std::move(gm));
		meshes[k] = id;
		return id;
	}

	gltf::Primitive primitive(const de::Mesh &m, const std::vector<std::array<std::array<float, 3>, 3>> &normals, const std::vector<uint32_t> &faces,
	                          const std::vector<std::map<uint32_t, std::array<std::array<float, 2>, 3>> *> &sets, int material) {
		gltf::Primitive p;
		p.material = material;
		p.uvs.resize(sets.size());
		std::vector<float> source;
		std::map<std::vector<float>, uint32_t> seen;
		json::Value jfaces = json::Value::array();
		for (uint32_t fi : faces) {
			const auto &f = m.faces[fi];
			jfaces.push(int64_t(fi));
			const int order[3] = {0, 2, 1};
			for (int kk = 0; kk < 3; ++kk) {
				int k = order[kk];
				uint16_t v = f.v[size_t(k)];
				std::vector<float> key{float(v)};
				auto n = normals[fi][size_t(k)];
				key.insert(key.end(), {n[0], n[1], n[2]});
				for (auto *set : sets) {
					auto it = set->find(fi);
					std::array<float, 2> uv = it == set->end() ? std::array<float, 2>{0, 0} : it->second[size_t(k)];
					key.insert(key.end(), {uv[0], uv[1]});
				}
				auto [it, fresh] = seen.try_emplace(key, uint32_t(p.positions.size()));
				if (fresh) {
					p.positions.push_back(to_gltf(m.vertices[v], opt.units_per_metre));
					p.normals.push_back({n[0], n[1], -n[2]});
					for (size_t s2 = 0; s2 < sets.size(); ++s2) p.uvs[s2].push_back({key[4 + s2 * 2], key[5 + s2 * 2]});
					source.push_back(float(v));
				}
				p.indices.push_back(it->second);
			}
		}
		p.custom["_DE_VERTEX"] = std::move(source);
		p.extras = json::Value::object();
		p.extras["de_faces"] = std::move(jfaces);
		return p;
	}

	int node(const std::string &name, int parent, const gltf::Mat4 &local = gltf::identity()) {
		gltf::Node n;
		n.name = name;
		gltf::decompose(local, &n.translation, &n.rotation, &n.scale);
		return s.add_node(std::move(n), parent);
	}

	int mesh_node(const de::ObjectRef &mo, int parent, const gltf::Mat4 &local = gltf::identity()) {
		int n = node(mo.info().name, parent, local);
		s.nodes[size_t(n)].mesh = mesh(mo);
		return n;
	}

	// LOD0.. under one node; each LOD's switch value in its extras.
	int lod(const de::ObjectRef &lo, int parent, const gltf::Mat4 &local = gltf::identity()) {
		int n = node(lo.info().name, parent, local);
		auto lists = lod_entries(lo);
		for (size_t k = 0; k < lists.size(); ++k) {
			int c = node("LOD" + std::to_string(k), n);
			s.nodes[size_t(c)].mesh = mesh(lists[k].first);
			s.nodes[size_t(c)].extras = json::Value::object();
			s.nodes[size_t(c)].extras["de_lod_switch"] = double(lists[k].second);
			s.nodes[size_t(c)].extras["de_name"] = lists[k].first.info().name;
		}
		return n;
	}

	// Mesh\LODSwitchingMesh references chunk 1: BOOL-terminated [mesh reference][f32 switch value].
	std::vector<std::pair<de::ObjectRef, float>> lod_entries(const de::ObjectRef &lo) {
		std::vector<std::pair<de::ObjectRef, float>> out;
		try {
			de::Contents c(lo.file->object_data(lo.info()).references);
			de::Chunk obj = de::begin_chunk(c);
			c.seek(obj.end);
			de::Chunk ch = de::begin_chunk(c);
			while (c.pos() < ch.end && c.boolean()) {
				de::ObjectRef m = w.resolve(*lo.file, de::Reference::read(c));
				float sw = c.f32();
				if (m && m.info().class_path == "Mesh\\Standard") out.push_back({m, sw});
			}
		} catch (const std::exception &) {
		}
		return out;
	}

	// An actor and the actors under it, with their meshes (children and components).
	void prop(const de::ObjectRef &a, int parent, bool root) {
		de::Transform local;
		try {
			local = de::read_actor(*a.file, a.info()).local;
		} catch (const std::exception &) {
		}
		int n = node(a.info().name, parent, root ? gltf::identity() : to_gltf(local, opt.units_per_metre));
		for (const de::ObjectRef &m : de::actor_meshes(w, a)) mesh_node(m, n);
		for (uint32_t c : de::ObjectsFile::children(a.file->object_data(a.info()))) {
			const auto &ci = a.file->objects()[c];
			if (ci.class_path == "Actor" || ci.class_path.rfind("Actor\\", 0) == 0) prop({a.file, c}, n, false);
			else if (ci.class_path == "Mesh\\LODSwitchingMesh") lod({a.file, c}, n);
		}
	}

	int collection(const std::string &name) {
		int n = node(name, -1);
		s.nodes[size_t(n)].extras = json::Value::object();
		s.nodes[size_t(n)].extras["de_collection"] = name;
		return n;
	}

	// A triangle soup (world space, DE units) as a mesh with "floor" and "wall" primitives.
	int soup(const std::string &name, const std::vector<std::array<float, 3>> &tris, bool split_walls, int &floor_mat, int &wall_mat) {
		gltf::Mesh gm;
		gm.name = name;
		gltf::Primitive floor, wall;
		auto ensure = [&](int &id, const char *mname, gltf::Vec4 c) {
			if (id >= 0) return;
			gltf::Material mt;
			mt.name = mname;
			mt.base_colour = c;
			mt.metallic = 0;
			id = int(s.materials.size());
			s.materials.push_back(mt);
		};
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			const auto &a = tris[t], &b = tris[t + 1], &c = tris[t + 2];
			float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
			float ny = u[2] * v[0] - u[0] * v[2];
			float nx = u[1] * v[2] - u[2] * v[1], nz = u[0] * v[1] - u[1] * v[0];
			float l = std::sqrt(nx * nx + ny * ny + nz * nz);
			bool is_floor = !split_walls || (l > 0 && std::fabs(ny / l) > 0.5f);
			gltf::Primitive &p = is_floor ? floor : wall;
			for (int k : {0, 2, 1}) {
				p.indices.push_back(uint32_t(p.positions.size()));
				p.positions.push_back(to_gltf(tris[t + size_t(k)], opt.units_per_metre));
			}
		}
		if (!floor.positions.empty()) {
			ensure(floor_mat, "floor", {0.69f, 0.55f, 1, 1});
			floor.material = floor_mat;
			gm.primitives.push_back(std::move(floor));
		}
		if (!wall.positions.empty()) {
			ensure(wall_mat, "wall", {0.5f, 0.82f, 0.78f, 1});
			wall.material = wall_mat;
			gm.primitives.push_back(std::move(wall));
		}
		if (gm.primitives.empty()) return -1;
		s.meshes.push_back(std::move(gm));
		return int(s.meshes.size()) - 1;
	}

	void arena(const de::ObjectRef &env) {
		int visual = collection("Visual");
		auto parents = env.file->parents();
		std::map<uint32_t, int> actor_nodes;
		for (auto &item : de::collect_scene(w, *env.file)) {
			bool under = false;
			for (int p = int(item.actor_index); p >= 0; p = parents[size_t(p)]) under = under || uint32_t(p) == env.index;
			if (!under) continue;
			int n = mesh_node(item.mesh_object, visual, to_gltf(item.world, opt.units_per_metre));
			s.nodes[size_t(n)].name = item.actor;
		}
		if (!opt.gameplay) return;
		ArenaScene sc = read_arena_scene(w, *env.file, env.index);
		int floor_mat = -1, wall_mat = -1, kill_mat = -1;
		int collision = -1, death = -1, jump = -1, spawn = -1, pickup = -1, nav = -1;
		auto coll = [&](int &id, const char *name) {
			if (id < 0) id = collection(name);
			return id;
		};
		for (const SceneNode &n : sc.nodes) {
			if (n.kind == NodeKind::Surface || (n.shape == SceneNode::Shape::Mesh && (n.groups & 0x200000) && n.kind != NodeKind::Hazard && n.kind != NodeKind::ForceAffector)) {
				int m = soup(n.name, n.triangles, true, floor_mat, wall_mat);
				if (m < 0) continue;
				int nn = node(n.name, coll(collision, "Collision"));
				s.nodes[size_t(nn)].mesh = m;
			} else if (n.kind == NodeKind::DeathVolume) {
				std::vector<std::array<float, 3>> tris = n.triangles;
				if (tris.empty() && n.shape == SceneNode::Shape::Box) tris = box_tris(n.world, n.box_min, n.box_max);
				int dummy = -1;
				int m = soup(n.name, tris, false, kill_mat, dummy);
				int nn = node(n.name, coll(death, "Death"), m < 0 ? to_gltf(n.world, opt.units_per_metre) : gltf::identity());
				if (m >= 0) s.nodes[size_t(nn)].mesh = m, s.materials[size_t(kill_mat)].name = "kill";
			} else if (n.kind == NodeKind::ForceAffector && n.shape == SceneNode::Shape::Mesh) {
				int dummy = -1, pad = -1;
				int m = soup(n.name, n.triangles, false, pad, dummy);
				if (m < 0) continue;
				s.materials[size_t(pad)].name = "jumppad";
				int nn = node(n.name, coll(jump, "JumpPad"));
				s.nodes[size_t(nn)].mesh = m;
				auto up = n.world.rotate({0, 1, 0});
				gltf::Vec3 dir = to_gltf(up, 1);
				s.nodes[size_t(nn)].extras = json::Value::object();
				s.nodes[size_t(nn)].extras["impulse"] = jvec(dir.data(), 3);
			} else if (n.kind == NodeKind::SpawnPoint) {
				node(n.name, coll(spawn, "Spawn"), to_gltf(n.world, opt.units_per_metre));
			} else if (n.kind == NodeKind::PickupSpawner) {
				int nn = node(n.name, coll(pickup, "Pickup"), to_gltf(n.world, opt.units_per_metre));
				json::Value list = json::Value::array();
				for (int c : n.children) {
					const SceneNode &cn = sc.nodes[size_t(c)];
					if (cn.name != "PickupsToSpawn") continue;
					for (auto &chunk : game::reference_lists(w, cn.ref)) {
						for (auto &l : chunk) {
							for (auto &r : l) {
								if (r) list.push(r.info().name);
							}
						}
					}
				}
				s.nodes[size_t(nn)].extras = json::Value::object();
				s.nodes[size_t(nn)].extras["pickups"] = std::move(list);
			} else if (n.kind == NodeKind::NavArea) {
				int nn = node(n.name, coll(nav, "Navigation"), to_gltf(n.world, opt.units_per_metre));
				json::Value links = json::Value::array();
				for (int c : n.children) {
					const SceneNode &cn = sc.nodes[size_t(c)];
					if (cn.kind == NodeKind::NavLink && cn.nav_to >= 0) links.push(sc.nodes[size_t(cn.nav_to)].name);
				}
				s.nodes[size_t(nn)].extras = json::Value::object();
				s.nodes[size_t(nn)].extras["radius"] = double((n.falloff > 0 ? n.falloff : n.hotspot) / opt.units_per_metre);
				s.nodes[size_t(nn)].extras["links"] = std::move(links);
			}
		}
	}

	static std::vector<std::array<float, 3>> box_tris(const de::Transform &t, const std::array<float, 3> &lo, const std::array<float, 3> &hi) {
		std::array<std::array<float, 3>, 8> c;
		for (int i = 0; i < 8; ++i) c[size_t(i)] = t.apply({i & 1 ? hi[0] : lo[0], i & 2 ? hi[1] : lo[1], i & 4 ? hi[2] : lo[2]});
		const int q[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
		std::vector<std::array<float, 3>> out;
		for (auto &f : q) {
			for (int k : {0, 1, 2, 0, 2, 3}) out.push_back(c[size_t(f[k])]);
		}
		return out;
	}

	void robot(const de::ObjectRef &model_in) {
		de::ObjectRef model = model_in;
		const de::ObjectsFile *mf = model.file;
		// The fighter file: <Name> for <Name>Model1.
		std::string stem;
		for (auto &[st, f] : w.files()) {
			if (f.get() == mf) stem = st;
		}
		std::string fighter_stem = stem;
		if (auto p = fighter_stem.find("Model"); p != std::string::npos) fighter_stem = fighter_stem.substr(0, p);
		const de::ObjectsFile *ff = w.file(fighter_stem);
		std::vector<std::pair<de::ObjectRef, float>> lods;
		for (uint32_t c : de::ObjectsFile::children(mf->object_data(model.info()))) {
			if (mf->objects()[c].class_path == "Mesh\\LODSwitchingMesh") lods = lod_entries({mf, c});
		}
		if (lods.empty()) {
			for (uint32_t c : de::ObjectsFile::children(mf->object_data(model.info()))) {
				if (mf->objects()[c].class_path == "Mesh\\Standard") lods.push_back({{mf, c}, 0});
			}
			std::sort(lods.begin(), lods.end(), [&](auto &a, auto &b) { return de::read_mesh(*a.first.file, a.first.info()).faces.size() > de::read_mesh(*b.first.file, b.first.info()).faces.size(); });
		}
		if (lods.empty()) throw std::runtime_error(model.info().name + " has no meshes");
		int root = node(model.info().name, -1);
		de::AnimBank bank;
		const de::ObjectInfo *deform = nullptr;
		if (ff) {
			for (auto &o : ff->objects()) {
				if (o.class_path == "AnimationSequenceGroup" && bank.channels.empty()) bank = de::read_anim_bank(*ff, o);
				if (o.class_path == "ActorDeformInfo" && !deform) deform = &o;
			}
		} else {
			s.warnings.push_back("no fighter file " + fighter_stem + ": the skeleton and animations are left out");
		}
		std::vector<int> joint_node(bank.channels.size(), -1);
		int armature = -1;
		std::vector<de::Transform> rest_world(bank.channels.size());
		if (!bank.channels.empty()) {
			armature = node("Armature", root);
			for (size_t c = 0; c < bank.channels.size(); ++c) {
				const auto &ch = bank.channels[c];
				de::Transform local = de::transform_from(ch.rest_pos, ch.rest_rot);
				int parent = ch.parent >= 0 && size_t(ch.parent) < c ? joint_node[size_t(ch.parent)] : armature;
				rest_world[c] = ch.parent >= 0 && size_t(ch.parent) < c ? rest_world[size_t(ch.parent)].then(local) : local;
				joint_node[c] = node(ch.name, parent, to_gltf(local, opt.units_per_metre));
			}
		}
		for (size_t l = 0; l < lods.size() && l < 3; ++l) {
			const de::ObjectRef &mo = lods[l].first;
			int gm = mesh(mo);
			int n = node("LOD" + std::to_string(l), root);
			s.nodes[size_t(n)].mesh = gm;
			s.nodes[size_t(n)].extras = json::Value::object();
			s.nodes[size_t(n)].extras["de_lod_switch"] = double(lods[l].second);
			s.nodes[size_t(n)].extras["de_name"] = mo.info().name;
			if (!deform || bank.channels.empty()) continue;
			auto bones = de::read_skin(*ff, *deform, *mo.file, mo.index);
			gltf::Skin skin;
			skin.name = mo.info().name;
			skin.skeleton = armature;
			std::vector<int> bone_joint(bones.size(), -1);
			for (size_t c = 0; c < bank.channels.size(); ++c) {
				skin.joints.push_back(joint_node[c]);
				de::Transform bind = rest_world[c];
				for (auto &b : bones) {
					if (b.name == bank.channels[c].name) bind = b.bind;
				}
				skin.inverse_binds.push_back(gltf::inverse(to_gltf(bind, opt.units_per_metre)));
			}
			for (size_t b = 0; b < bones.size(); ++b) bone_joint[b] = bank.channel(bones[b].name);
			for (auto &p : s.meshes[size_t(gm)].primitives) {
				auto &src = p.custom["_DE_VERTEX"];
				p.joints.assign(p.positions.size(), {0, 0, 0, 0});
				p.weights.assign(p.positions.size(), {0, 0, 0, 0});
				for (size_t v = 0; v < src.size(); ++v) {
					size_t dv = size_t(src[v]);
					std::vector<std::pair<float, int>> inf;
					for (size_t b = 0; b < bones.size(); ++b) {
						if (bone_joint[b] >= 0 && dv < bones[b].weights.size() && bones[b].weights[dv]) inf.push_back({bones[b].weights[dv] / 255.0f, bone_joint[b]});
					}
					std::sort(inf.begin(), inf.end(), [](auto &a, auto &b) { return a.first > b.first; });
					if (inf.size() > 4) inf.resize(4);
					float sum = 0;
					for (auto &i : inf) sum += i.first;
					if (inf.empty()) {
						p.joints[v] = {0, 0, 0, 0};
						p.weights[v] = {1, 0, 0, 0};
						continue;
					}
					for (size_t k = 0; k < inf.size(); ++k) {
						p.joints[v][k] = uint16_t(inf[k].second);
						p.weights[v][k] = inf[k].first / sum;
					}
				}
			}
			s.nodes[size_t(n)].skin = int(s.skins.size());
			s.skins.push_back(std::move(skin));
		}
		if (!opt.animations || bank.channels.empty()) return;
		for (const auto &seq : bank.sequences) {
			gltf::Animation a;
			a.name = seq.name;
			a.extras = json::Value::object();
			a.extras["de_normalised"] = true;
			for (const auto &st : seq.streams) {
				if (st.channel >= joint_node.size()) continue;
				const auto &ch = bank.channels[st.channel];
				if (!st.pos.empty()) {
					gltf::Animation::Sampler smp;
					for (auto &k : st.pos) {
						smp.input.push_back(std::clamp(k.t, 0.0f, 1.0f));
						auto p = to_gltf(k.p, opt.units_per_metre);
						smp.output.insert(smp.output.end(), p.begin(), p.end());
					}
					a.channels.push_back({int(a.samplers.size()), joint_node[st.channel], gltf::Animation::Path::Translation});
					a.samplers.push_back(std::move(smp));
				}
				if (!st.rot.empty()) {
					gltf::Animation::Sampler smp;
					for (auto &k : st.rot) {
						smp.input.push_back(std::clamp(k.t, 0.0f, 1.0f));
						de::Transform t = de::transform_from(ch.rest_pos, k.q);
						gltf::Vec4 q;
						gltf::decompose(to_gltf(t, opt.units_per_metre), nullptr, &q, nullptr);
						smp.output.insert(smp.output.end(), q.begin(), q.end());
					}
					a.channels.push_back({int(a.samplers.size()), joint_node[st.channel], gltf::Animation::Path::Rotation});
					a.samplers.push_back(std::move(smp));
				}
			}
			for (auto &smp : a.samplers) {
				// glTF needs strictly increasing key times.
				for (size_t k = 1; k < smp.input.size(); ++k) smp.input[k] = std::max(smp.input[k], smp.input[k - 1] + 1e-5f);
			}
			if (!a.channels.empty()) s.animations.push_back(std::move(a));
		}
	}
};

}  // namespace

gltf::Scene export_object(const de::World &w, const de::ObjectRef &object, const ExportOptions &opt) {
	if (!object) throw std::runtime_error("no object");
	Exporter ex{w, opt, {}, {}, {}, {}};
	ex.s.generator = "OpenBG content editor";
	ex.s.extras = json::Value::object();
	ex.s.extras["de_units_per_metre"] = double(opt.units_per_metre);
	ex.s.extras["de_source"] = object.info().name;
	ex.s.extras["de_class"] = object.info().class_path;
	const std::string cls = object.info().class_path;
	std::string kind = export_kind(cls);
	ex.s.extras["de_kind"] = kind;
	if (kind == "mesh") {
		ex.mesh_node(object, -1);
	} else if (kind == "lod") {
		ex.lod(object, -1);
	} else if (kind == "prop") {
		ex.prop(object, -1, true);
	} else if (kind == "arena") {
		de::ObjectRef env = object;
		if (cls == "Description\\Environment") {
			auto refs = game::passive_references(w, object);
			if (refs.empty() || !refs[0] || refs[0].info().class_path != "Actor\\Creature\\Environment") throw std::runtime_error("the description's environment is not loaded");
			env = refs[0];
		}
		ex.arena(env);
	} else if (kind == "robot") {
		de::ObjectRef model = object;
		if (cls != "FighterModel") {
			model = {};
			for (auto &r : game::passive_references(w, object)) {
				if (r && r.info().class_path == "FighterModel") model = r;
			}
			if (!model) {
				for (uint32_t i = 0; i < object.file->objects().size(); ++i) {
					if (object.file->objects()[i].class_path == "FighterModel") model = {object.file, i};
				}
			}
			if (!model) throw std::runtime_error("no FighterModel for " + object.info().name);
		}
		ex.robot(model);
	} else {
		throw std::runtime_error(cls + " objects cannot be exported");
	}
	return std::move(ex.s);
}

}  // namespace ed
