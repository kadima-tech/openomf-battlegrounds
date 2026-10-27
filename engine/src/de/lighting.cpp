#include "de/lighting.h"

namespace de {

namespace {

// Byte ranges and versions of an object's top-level chunks.
std::vector<std::pair<Chunk, size_t>> chunks_of(Contents &c) {
	std::vector<std::pair<Chunk, size_t>> out;
	while (c.remaining() > 1) {
		Chunk ch = begin_chunk(c);
		if (ch.end > c.size() || ch.end < c.pos()) break;
		out.push_back({ch, c.pos()});
		c.seek(ch.end);
	}
	return out;
}

}  // namespace

uint32_t LightInfo::mask() const {
	switch (mode) {
	case 0: return 6;
	case 1: return 4;
	case 2: return 0x10;
	case 3: return 8;
	default: return 0;
	}
}

int projector_blend(const LightInfo &l) {
	int b = l.mode == 2 ? 2 : l.mode == 3 ? 1 : 0;
	if (l.dynamic == 1) b = 3;
	if (l.dynamic == 2) b = 4;
	if (l.dynamic == 3) b = 5;
	return b;
}

std::optional<LightInfo> read_light(const World &w, const ObjectRef &light) {
	if (!light || light.info().class_path != "Light") return std::nullopt;
	try {
		auto d = light.file->object_data(light.info());
		Contents c(d.contents);
		Chunk ch = begin_chunk(c);
		if (ch.version < 3) return std::nullopt;
		c.block();  // name
		LightInfo l;
		l.colour = read_embedded_color(c);
		l.intensity = c.f32();
		if (ch.version >= 4) {
			l.mode = c.i32();
			l.dynamic = c.i32();
			if (ch.version == 4) l.mode = l.dynamic = 0;
		}
		// The colour reference: the last references chunk holds its advanced reference.
		Contents rc(d.references);
		auto refs = chunks_of(rc);
		if (!refs.empty()) {
			rc.seek(refs.back().second);
			if (rc.pos() < refs.back().first.end) {
				if (ObjectRef v = read_advanced_reference(w, *light.file, rc)) l.colour = read_color_value(w, v);
				// v3: [BOOL][reference]: the light's projector image (+0x2c/+0x30).
				if (refs.back().first.version >= 3 && rc.pos() < refs.back().first.end && rc.boolean()) {
					l.image = w.resolve(*light.file, Reference::read(rc));
				}
			}
		}
		return l;
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

std::vector<PlacedLight> read_scene_lights(const World &w, const ObjectsFile &file) {
	std::vector<PlacedLight> out;
	const auto &objs = file.objects();
	auto parents = file.parents();
	auto world_of = [&](uint32_t i) {
		std::vector<uint32_t> chain;
		for (int p = int(i); p >= 0; p = parents[size_t(p)]) chain.push_back(uint32_t(p));
		Transform t;
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			const std::string &cls = objs[*it].class_path;
			if (cls != "Actor" && cls.rfind("Actor\\", 0) != 0) continue;
			try {
				t = t.then(read_actor(file, objs[*it]).local);
			} catch (const FormatError &) {
			}
		}
		return t;
	};
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].class_path != "Light" || parents[i] < 0) continue;
		uint32_t a = uint32_t(parents[i]);
		const std::string &cls = objs[a].class_path;
		if (cls != "Actor\\Affector\\Sphere" && cls != "Actor\\Affector\\Cone") continue;
		auto info = read_light(w, {&file, i});
		if (!info) continue;
		PlacedLight l;
		l.info = *info;
		l.world = world_of(a);
		l.cone = cls == "Actor\\Affector\\Cone";
		for (int p = int(a); p >= 0; p = parents[size_t(p)]) {
			const std::string &pc = objs[size_t(p)].class_path;
			if (pc != "Actor" && pc.rfind("Actor\\", 0) != 0) continue;
			try {
				ActorInfo ai = read_actor(file, objs[size_t(p)]);
				if (ai.position_mode != 0) {
					l.position_static = ai.position_mode == 1;
					break;
				}
			} catch (const FormatError &) {
			}
		}
		try {
			Contents c(file.object_data(objs[a]).contents);
			auto chunks = chunks_of(c);
			if (chunks.size() < 3 || chunks[2].first.version != 2) continue;
			c.seek(chunks[2].second);
			if (l.cone) {
				l.inner = c.f32(), l.outer = c.f32();
				l.near_begin = c.f32();
				c.f32();
				l.umbra = c.f32(), l.penumbra = c.f32();
			} else {
				l.inner = c.f32(), l.outer = c.f32();
			}
		} catch (const std::exception &) {
			continue;
		}
		out.push_back(l);
	}
	return out;
}

std::array<float, 3> read_ambient(const World &w, const ObjectsFile &arena) {
	(void)w;
	for (auto &o : arena.objects()) {
		if (o.class_path != "Actor\\Creature\\Environment") continue;
		try {
			Contents c(arena.object_data(o).contents);
			auto chunks = chunks_of(c);
			if (chunks.empty()) break;
			auto &last = chunks.back();
			if (last.first.version < 2) break;
			c.seek(last.second);
			ColorValue amb = read_embedded_color(c);
			float scale = c.f32();
			return {amb.rgba[0] * scale, amb.rgba[1] * scale, amb.rgba[2] * scale};
		} catch (const std::exception &) {
		}
		break;
	}
	return {0.3f, 0.3f, 0.3f};
}

std::array<ObjectRef, 6> read_cube_faces(const World &w, const ObjectRef &cube) {
	std::array<ObjectRef, 6> out;
	if (!cube) return out;
	try {
		Contents c(cube.file->object_data(cube.info()).references);
		auto refs = chunks_of(c);
		if (refs.size() < 3) return out;
		c.seek(refs[2].second);
		size_t k = 0;
		while (k < 6 && c.pos() < refs[2].first.end && c.boolean()) out[k++] = w.resolve(*cube.file, Reference::read(c));
	} catch (const std::exception &) {
	}
	return out;
}

ObjectRef specular_cube(const ObjectRef &owner) {
	if (!owner) return {};
	for (uint32_t c : ObjectsFile::children(owner.file->object_data(owner.info()))) {
		const ObjectInfo &o = owner.file->objects()[c];
		if (o.name == "HighRangeSpecular" && o.class_path == "Image\\CubeEnvironment") return {owner.file, c};
	}
	return {};
}

std::vector<std::array<float, 3>> read_corner_colours(const ObjectsFile &f, uint32_t texture_manager) {
	std::vector<std::array<float, 3>> out;
	for (uint32_t ci : ObjectsFile::children(f.object_data(f.objects()[texture_manager]))) {
		auto &o = f.objects()[ci];
		if (o.class_path != "DataChannel\\UnitFloats") continue;
		try {
			Contents c(f.object_data(o).contents);
			Chunk base = begin_chunk(c);  // DE_CDataChannel: name, channel name ("ColorRGB"), ...
			c.block();
			std::string channel;
			{
				auto b = c.block();
				channel.assign(b.begin(), b.end());
				while (!channel.empty() && channel.back() == 0) channel.pop_back();
			}
			c.seek(base.end);
			if (channel != "ColorRGB") continue;
			Chunk uf = begin_chunk(c);
			if (uf.version != 1) continue;
			int32_t components = c.i32(), entries = c.i32();
			if (components < 3 || entries <= 0) continue;
			auto px = c.bytes(size_t(components) * size_t(entries));
			out.resize(size_t(entries));
			for (size_t i = 0; i < out.size(); ++i) {
				for (int k = 0; k < 3; ++k) out[i][size_t(k)] = px[i * size_t(components) + size_t(k)] / 255.0f;
			}
			return out;
		} catch (const std::exception &) {
		}
	}
	return out;
}

}  // namespace de
