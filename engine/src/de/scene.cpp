#include "de/scene.h"

#include "de/lighting.h"
#include "de/value.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <tuple>
#include <fstream>
#include <functional>

namespace de {

namespace {

std::vector<uint8_t> read_file(const std::filesystem::path &p) {
	std::ifstream f(p, std::ios::binary);
	return {std::istreambuf_iterator<char>(f), {}};
}

std::array<uint8_t, 16> key(const Guid &g) {
	std::array<uint8_t, 16> k;
	memcpy(k.data(), g.bytes, 16);
	return k;
}

bool contains_ci(const std::string &s, const char *needle) {
	std::string a = s, b = needle;
	for (auto &ch : a) {
		ch = char(tolower(ch));
	}
	for (auto &ch : b) {
		ch = char(tolower(ch));
	}
	return a.find(b) != std::string::npos;
}

bool is_actor_class(const std::string &path) { return path == "Actor" || path.rfind("Actor\\", 0) == 0; }

// References chunk list after the DE_CObject chunk: BOOL-terminated references.
std::vector<Reference> actor_components(const ObjectsFile::ObjectData &d) {
	std::vector<Reference> out;
	if (d.references.empty()) {
		return out;
	}
	Contents c(d.references);
	Chunk obj = begin_chunk(c);
	c.seek(obj.end);
	if (c.remaining() == 0) {
		return out;
	}
	Chunk act = begin_chunk(c);
	if (act.version >= 1 && act.version <= 3) {
		while (c.pos() < act.end && c.boolean()) {
			out.push_back(Reference::read(c));
		}
	}
	return out;
}

std::vector<Reference> texture_images(const ObjectsFile::ObjectData &d) {
	std::vector<Reference> out;
	if (d.references.empty()) {
		return out;
	}
	Contents c(d.references);
	Chunk obj = begin_chunk(c);
	c.seek(obj.end);
	if (c.remaining() == 0) {
		return out;
	}
	Chunk tm = begin_chunk(c);
	if (tm.version == 1 || tm.version == 2) {
		int32_t n = c.i32();
		for (int32_t i = 0; i < n && c.pos() < tm.end; ++i) {
			out.push_back(Reference::read(c));
		}
	}
	return out;
}

}  // namespace

MultiStageInfo read_multistage(const ObjectsFile::ObjectData &d) {
	MultiStageInfo out;
	if (d.references.empty()) return out;
	try {
		Contents c(d.references);
		for (int skip = 0; skip < 2; ++skip) {  // DE_CObject, DE_CTextureProcess
			Chunk ch = begin_chunk(c);
			c.seek(ch.end);
			if (c.remaining() == 0) return out;
		}
		Chunk ms = begin_chunk(c);
		out.version = ms.version;
		if (ms.version < 1 || ms.version > 8) return out;
		auto mode_of = [](const std::string &name) {
			return name == "Add" ? 1 : name == "Modulate" ? 2 : name == "AddToLightMap" ? 3 : name == "SemiTranslucent" ? 4 : 0;
		};
		auto read_name = [&]() {
			uint32_t len = c.u32();
			auto b = c.bytes(len);
			std::string name(b.begin(), b.end());
			while (!name.empty() && name.back() == 0) name.pop_back();
			return name;
		};
		int32_t n = c.i32();
		if (ms.version < 4) {
			// v1: [manager][mode name][RGBA]; v2/v3 are not in the shipped data.
			for (int32_t i = 0; i < n && c.pos() < ms.end && ms.version == 1; ++i) {
				StageInfo st;
				st.manager = Reference::read(c);
				st.mode = mode_of(read_name());
				for (float &v : st.colour) v = c.f32();
				out.stages.push_back(st);
			}
			return out;
		}
		for (int32_t i = 0; i < n && c.pos() < ms.end; ++i) {
			StageInfo st;
			st.manager = Reference::read(c);
			if (c.boolean()) {
				st.colour_value = Reference::read(c);
			} else {
				for (float &v : st.colour) v = c.f32();
			}
			if (ms.version == 4) {
				st.mode = mode_of(read_name());
			} else {
				st.mode = c.i32();
			}
			st.flag_2c = c.boolean();
			out.first_marked = c.i32();
			if (ms.version >= 6) st.flag_30 = c.boolean();
			if (ms.version == 7) c.boolean();
			if (ms.version == 8) st.disabled = c.boolean();
			out.stages.push_back(st);
		}
	} catch (const std::exception &) {
	}
	return out;
}

// PE resources of type "DEOBJECTS" (named type, numeric ids): each is a whole objects file.
std::vector<std::vector<uint8_t>> pe_deobjects_resources(const std::vector<uint8_t> &d) {
	std::vector<std::vector<uint8_t>> out;
	auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8) : 0; };
	auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24) : 0; };
	if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return out;
	size_t pe = u32(0x3c);
	if (u32(pe) != 0x4550) return out;
	uint32_t nsec = u16(pe + 6), opt = u16(pe + 20);
	size_t sec = pe + 24 + opt;
	uint32_t rsrc = u32(pe + 24 + 96 + 2 * 8);
	auto offset = [&](uint32_t rva) -> size_t {
		for (uint32_t i = 0; i < nsec; ++i) {
			size_t h = sec + i * 40;
			uint32_t vs = u32(h + 8), va = u32(h + 12), rs = u32(h + 16), ro = u32(h + 20);
			if (rva >= va && rva < va + std::max(vs, rs)) return ro + (rva - va);
		}
		return 0;
	};
	size_t base = offset(rsrc);
	if (!rsrc || !base) return out;
	auto name_of = [&](uint32_t id) {
		std::string n;
		if (!(id & 0x80000000)) return n;
		size_t o = base + (id & 0x7fffffff);
		for (uint32_t k = 0, len = u16(o); k < len; ++k) n += char(u16(o + 2 + k * 2));
		return n;
	};
	std::function<void(size_t, int, bool)> walk = [&](size_t dir, int depth, bool wanted) {
		uint32_t n = u16(base + dir + 12) + u16(base + dir + 14);
		for (uint32_t k = 0; k < n; ++k) {
			uint32_t id = u32(base + dir + 16 + k * 8), ent = u32(base + dir + 20 + k * 8);
			bool w = depth == 0 ? name_of(id) == "DEOBJECTS" : wanted;
			if (ent & 0x80000000) {
				walk(ent & 0x7fffffff, depth + 1, w);
			} else if (w) {
				uint32_t rva = u32(base + ent), size = u32(base + ent + 4);
				size_t o = offset(rva);
				if (o && o + size <= d.size()) out.emplace_back(d.begin() + long(o), d.begin() + long(o + size));
			}
		}
	};
	walk(0, 0, false);
	return out;
}

void World::add(const std::string &stem, std::vector<uint8_t> data) {
	auto f = std::make_unique<ObjectsFile>(std::move(data));
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		guids_.emplace(key(f->objects()[i].guid), ObjectRef{f.get(), i});
	}
	files_[stem] = std::move(f);
}

World::World(const std::filesystem::path &dir) {
	for (auto &e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() != ".DEObjects") {
			continue;
		}
		add(e.path().stem().string(), read_file(e.path()));
	}
	// The objects files built into the engine modules, named after their master object
	// (COREMeshes, COREImages).
	auto engine = dir.parent_path() / "Engine";
	if (std::filesystem::exists(engine / "Core.ModuleDLL")) {
		for (auto &res : pe_deobjects_resources(read_file(engine / "Core.ModuleDLL"))) {
			try {
				ObjectsFile probe(res);
				std::string stem = probe.master().name;
				if (!stem.empty() && !files_.count(stem)) add(stem, std::move(res));
			} catch (const std::exception &) {
			}
		}
	}
}

const ObjectsFile *World::file(const std::string &stem) const {
	auto it = files_.find(stem);
	return it == files_.end() ? nullptr : it->second.get();
}

ObjectRef World::by_guid(const Guid &g) const {
	auto it = guids_.find(key(g));
	return it == guids_.end() ? ObjectRef{} : it->second;
}

ObjectRef World::resolve(const ObjectsFile &from, const Reference &r) const {
	if (r.kind == Reference::kLocal && r.index < from.objects().size()) {
		return {&from, r.index};
	}
	if (r.kind == Reference::kExternal && r.index < from.guid_pool().size()) {
		return by_guid(from.guid_pool()[r.index]);
	}
	return {};
}

Transform Transform::then(const Transform &l) const {
	Transform out;
	// Child axes expressed in this (parent) frame: row_i' = sum_k l.r[i][k] * parent.row_k
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			out.r[i * 3 + j] = l.r[i * 3 + 0] * r[0 * 3 + j] + l.r[i * 3 + 1] * r[1 * 3 + j] + l.r[i * 3 + 2] * r[2 * 3 + j];
		}
	}
	out.t = apply(l.t);
	return out;
}

const Layer *Material::base() const {
	for (auto &l : layers) {
		if (!l.lightmap && !l.reflection && l.image && !l.uvs.empty()) {
			return &l;
		}
	}
	return nullptr;
}

const Layer *Material::light() const {
	for (auto &l : layers) {
		if (l.lightmap && l.image && !l.uvs.empty()) {
			return &l;
		}
	}
	return nullptr;
}

ActorInfo read_actor(const ObjectsFile &f, const ObjectInfo &o) {
	Contents c(f.object_data(o).contents);
	Chunk ch = begin_chunk(c);
	if (ch.version < 2 || ch.version > 7) {
		throw FormatError("Actor: unsupported version " + std::to_string(ch.version));
	}
	ActorInfo a;
	auto name = c.block();
	a.name.assign(name.begin(), name.end());
	while (!a.name.empty() && a.name.back() == 0) {
		a.name.pop_back();
	}
	for (auto &v : a.local.t) {
		v = c.f32();
	}
	for (auto &v : a.local.r) {
		v = c.f32();
	}
	if (ch.version >= 4) {
		for (int k = 0; k < 4; ++k) c.boolean();
		a.position_mode = c.i32();
		if (ch.version >= 5) a.flags = c.u32();
	}
	return a;
}

std::vector<Material> read_materials(const World &w, const ObjectRef &mesh) {
	std::vector<Material> out;
	auto layer_of = [&](const ObjectRef &tm) {
		Layer layer;
		layer.name = tm.info().name;
		layer.manager = tm;
		layer.environment = tm.info().class_path == "TextureManager\\Reflection";
		layer.reflection = layer.environment || contains_ci(layer.name, "reflection");
		layer.lightmap = contains_ci(layer.name, "lightmap");
		layer.uvs = read_texture_uvs(*tm.file, tm.info());
		layer.corner_colours = read_corner_colours(*tm.file, tm.index);
		if (layer.corner_colours.size() != layer.uvs.size() * 3) layer.corner_colours.clear();
		layer.specular = child_float(w, tm, "SpecularFactor").value_or(1);
		for (auto &ref : texture_images(tm.file->object_data(tm.info()))) {
			ObjectRef img = w.resolve(*tm.file, ref);
			if (img && img.info().class_path == "Image\\CubeEnvironment" && !layer.cube) layer.cube = img;
			bool flat = img && (img.info().class_path == "Image\\Standard" || img.info().class_path == "Image\\AVI");
			if (flat && !layer.image) layer.image = img;
		}
		if (layer.image) {
			for (uint32_t c : ObjectsFile::children(layer.image.file->object_data(layer.image.info()))) {
				const ObjectInfo &o = layer.image.file->objects()[c];
				if (o.name == "Bump" && o.class_path == "Image\\Standard") {
					layer.bump = {layer.image.file, c};
					layer.bump_factor = child_float(w, layer.bump, "BumpFactor").value_or(0);
					layer.bump_rotate = child_float(w, layer.bump, "RotateValue").value_or(0);
					if (layer.bump_factor == 0 && layer.bump_rotate == 0) layer.bump = {};
					break;
				}
			}
		}
		return layer;
	};
	for (uint32_t c : ObjectsFile::children(mesh.file->object_data(mesh.info()))) {
		auto &mat = mesh.file->objects()[c];
		if (mat.class_path != "TextureProcess\\MultiStage") {
			continue;
		}
		Material m;
		m.name = mat.name;
		m.glow = child_float(w, mesh, "GlowFactor").value_or(0);
		// The stages in the order (and with the settings) of the reference stream; the texture
		// managers in tree order when it does not parse.
		MultiStageInfo ms = read_multistage(mesh.file->object_data(mat));
		for (auto &st : ms.stages) {
			ObjectRef tm = w.resolve(*mesh.file, st.manager);
			if (!tm || tm.info().class_path.rfind("TextureManager", 0) != 0) continue;
			Layer layer = layer_of(tm);
			layer.mode = st.mode;
			layer.colour = st.colour;
			// A colour Value instead (DE_CRenderMaterialElement::FlatColor: its GetValue(RGBA&),
			// which a ModulatedRGBA takes at its time member, 0; inter values at 0 too).
			if (ObjectRef v = w.resolve(*mesh.file, st.colour_value)) layer.colour = read_color_value(w, v).eval(0);
			layer.stacked = st.flag_2c;
			layer.disabled = st.disabled;
			m.layers.push_back(std::move(layer));
		}
		if (m.layers.empty()) {
			for (uint32_t t : ObjectsFile::children(mesh.file->object_data(mat))) {
				if (mesh.file->objects()[t].class_path.rfind("TextureManager", 0) == 0) m.layers.push_back(layer_of({mesh.file, t}));
			}
		}
		out.push_back(std::move(m));
	}
	return out;
}

std::vector<ObjectRef> actor_meshes(const World &w, const ObjectRef &actor, bool children) {
	std::vector<ObjectRef> meshes;
	const ObjectsFile &file = *actor.file;
	const auto &objs = file.objects();
	for (uint32_t c : ObjectsFile::children(file.object_data(objs[actor.index]))) {
		if (children && objs[c].class_path == "Mesh\\Standard") meshes.push_back({&file, c});
	}
	for (auto &ref : actor_components(file.object_data(objs[actor.index]))) {
		ObjectRef m = w.resolve(file, ref);
		if (m && m.info().class_path == "Mesh\\Standard") meshes.push_back(m);
	}
	return meshes;
}

std::vector<DrawItem> collect_scene(const World &w, const ObjectsFile &file) {
	const auto &objs = file.objects();
	auto parents = file.parents();
	std::vector<Transform> world(objs.size());
	std::vector<int> state(objs.size(), 0);  // 0 = todo, 1 = in progress, 2 = done
	std::function<const Transform &(uint32_t)> world_of = [&](uint32_t i) -> const Transform & {
		if (state[i] == 2) {
			return world[i];
		}
		state[i] = 1;
		Transform parent;
		if (parents[i] >= 0 && state[uint32_t(parents[i])] != 1) {
			parent = world_of(uint32_t(parents[i]));
		}
		world[i] = parent;
		if (is_actor_class(objs[i].class_path)) {
			try {
				world[i] = parent.then(read_actor(file, objs[i]).local);
			} catch (const FormatError &) {
			}
		}
		state[i] = 2;
		return world[i];
	};

	auto materials_of = [&](const ObjectRef &mesh, DrawItem &item) { item.materials = read_materials(w, mesh); };
	std::vector<ActorInfo> info(objs.size());
	std::vector<bool> have(objs.size(), false);
	auto info_of = [&](uint32_t i) -> const ActorInfo * {
		if (!is_actor_class(objs[i].class_path)) return nullptr;
		if (!have[i]) {
			have[i] = true;
			try {
				info[i] = read_actor(file, objs[i]);
			} catch (const FormatError &) {
			}
		}
		return &info[i];
	};
	// Left out at this detail level: the actor or one above it.
	auto hidden = [&](uint32_t i) {
		for (int p = int(i); p >= 0; p = parents[size_t(p)]) {
			if (const ActorInfo *a = info_of(uint32_t(p)); a && !shown_at_detail(a->flags)) return true;
		}
		return false;
	};
	auto position_static = [&](uint32_t i) {
		for (int p = int(i); p >= 0; p = parents[size_t(p)]) {
			if (const ActorInfo *a = info_of(uint32_t(p)); a && a->position_mode != 0) return a->position_mode == 1;
		}
		return false;
	};

	std::vector<DrawItem> out;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (!is_actor_class(objs[i].class_path) || hidden(i)) {
			continue;
		}
		// DE_CD3DRenderActor::GetValidActorType: an actor draws the meshes among its components
		// (+0x20); a mesh that is only a child (a mesh affector's collision shape) is not drawn.
		std::vector<ObjectRef> meshes;
		for (auto &ref : actor_components(file.object_data(objs[i]))) {
			ObjectRef m = w.resolve(file, ref);
			if (m && m.info().class_path == "Mesh\\Standard") {
				meshes.push_back(m);
			}
		}
		std::sort(meshes.begin(), meshes.end(), [](const ObjectRef &a, const ObjectRef &b) {
			return std::tie(a.file, a.index) < std::tie(b.file, b.index);
		});
		meshes.erase(std::unique(meshes.begin(), meshes.end(),
		                         [](const ObjectRef &a, const ObjectRef &b) { return a.file == b.file && a.index == b.index; }),
		             meshes.end());
		for (auto &m : meshes) {
			DrawItem item;
			item.actor = objs[i].name;
			item.actor_index = i;
			item.mesh_object = m;
			item.mesh = read_mesh(*m.file, m.info());
			if (item.mesh.faces.empty()) {
				continue;
			}
			item.world = world_of(i);
			item.position_static = position_static(i);
			materials_of(m, item);
			out.push_back(std::move(item));
		}
	}
	return out;
}

}  // namespace de
