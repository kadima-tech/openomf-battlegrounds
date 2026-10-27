#include "de/particles.h"

#include <cstring>

#include "de/contents.h"
#include "de/mesh.h"

namespace de {

namespace {

std::array<float, 6> read_box(Contents &c) {
	std::array<float, 6> b{};
	for (auto &x : b) x = c.f32();
	return b;
}

// The chunks of a references block, one per class (base first).
std::vector<std::pair<size_t, Chunk>> chunks(Contents &c) {
	std::vector<std::pair<size_t, Chunk>> out;
	while (c.remaining() > 1) {
		size_t start = c.pos();
		Chunk ch = begin_chunk(c);
		if (ch.end > c.size()) break;
		out.push_back({start, ch});
		c.seek(ch.end);
	}
	return out;
}

ParticleModifierDef read_modifier(const World &w, const ObjectRef &r) {
	ParticleModifierDef m;
	m.object = r;
	const std::string &cls = r.info().class_path;
	m.kind = cls.substr(cls.find('\\') + 1);
	auto d = r.file->object_data(r.info());
	Contents c(d.contents);
	// DE_CParticleModifier::ObjProcessContents.
	Chunk base = begin_chunk(c);
	c.block();  // name
	if (base.version >= 2) {
		m.spawn_time = c.f32();
		m.spawn_time_variation = c.f32();
		m.life_span = c.f32();
		m.life_span_variation = c.f32();
		m.spawn_size = c.f32();
		m.spawn_size_variation = c.f32();
		m.spawning = c.boolean();
		if (base.version >= 3) m.looping = c.boolean();
		if (base.version >= 4) {
			m.initial_count = c.i32();
			m.interpolate = c.i32();
			m.extra_per_spawn = c.i32();
		}
	}
	c.seek(base.end);
	if (c.remaining() < 2) return m;
	Chunk ch = begin_chunk(c);
	const uint8_t v = ch.version;
	if (m.kind == "ManualParticleSpawner") {
		if (v == 1) m.manual_orientation = c.boolean();
	} else if (m.kind == "RandomEllipsoidSpawner") {
		// v1 carried the base's timing fields here as well.
		m.box = read_box(c);
		if (v == 1) {
			m.spawn_time = c.f32();
			m.spawn_time_variation = c.f32();
			m.life_span = c.f32();
			m.life_span_variation = c.f32();
			m.spawn_size = c.f32();
			m.spawn_size_variation = c.f32();
			m.spawning = c.i32() != 0;
		}
	} else if (m.kind == "RandomBoxSpawner") {
		m.box = read_box(c);
		if (v == 1) {
			m.spawn_time = c.f32();
			m.spawn_time_variation = c.f32();
			m.life_span = c.f32();
			m.life_span_variation = c.f32();
			m.spawning = c.i32() != 0;
		} else if (v == 3) {
			for (auto &b : m.wrap) b = c.boolean();
		}
	} else if (m.kind == "Gravity" || m.kind == "Wind") {
		if (v == 1) m.strength = c.f32();
	} else if (m.kind == "MiscellaneousParticleFunctions") {
		if (v == 1) {
			m.curve = read_embedded_float(c);
			m.misc_mode = c.u32();
		}
	} else if (m.kind == "NewParticleModifier") {
		if (v == 1) {
			m.rotation_mode = c.i32();
			m.orientation_mode = c.i32();
			m.momentum_mode = c.i32();
			m.momentum_scale = c.f32();
			m.f78 = c.f32();
			m.inherit_momentum = c.boolean();
			m.inherit_cap = c.f32();
			m.inherit_scale = c.f32();
			m.color_mode = c.i32();
			m.add_momentum = c.boolean();
		}
	} else if (m.kind == "MomentumAndAcceleration") {
		if (v >= 1 && v <= 4) {
			for (auto &x : m.inherit_axes) x = c.f32();
			for (auto &x : m.random_momentum) x = c.f32();
			if (v == 1) {
				for (auto &x : m.random_momentum) x = c.f32();
			}
			if (v >= 3) {
				m.accel_mode = c.i32();
				m.accel = c.f32();
			}
			if (v == 4) {
				m.accel_angle = c.f32();
				for (auto &x : m.initial_momentum) x = c.f32();
			} else if (v == 3) {
				m.accel_angle = c.f32();
			}
		}
	} else if (m.kind == "MeshSpawner") {
		if (v == 1) {
			m.spawn_time = c.f32();
			m.spawn_time_variation = c.f32();
			m.life_span = c.f32();
			m.life_span_variation = c.f32();
			m.spawning = c.i32() != 0;
		}
		m.mesh_burst = c.i32();
		if (v == 3) m.mesh_order = c.i32();
	}
	c.seek(ch.end);

	// References: [modifier v1][subclass].
	try {
		Contents rc(d.references);
		auto list = chunks(rc);
		if (list.size() >= 2) {
			auto [start, sub] = list.back();
			rc.seek(start);
			begin_chunk(rc);
			if (m.kind == "MiscellaneousParticleFunctions" && sub.version == 1) {
				if (ObjectRef f = read_advanced_reference(w, *r.file, rc)) m.curve = read_float_value(w, f);
			} else if (m.kind == "KillWithParticles") {
				while (rc.pos() < sub.end && rc.boolean()) {
					if (ObjectRef k = w.resolve(*r.file, Reference::read(rc))) m.kill.push_back(k);
				}
			} else if (m.kind == "MeshSpawner" && sub.version == 1 && rc.pos() < sub.end && rc.boolean()) {
				m.mesh = w.resolve(*r.file, Reference::read(rc));
				if (m.mesh && m.mesh.info().class_path == "Mesh\\Standard") {
					m.mesh_vertices = read_mesh(*m.mesh.file, m.mesh.info()).vertices;
				}
			}
		}
	} catch (const std::exception &) {
	}
	return m;
}

ParticleGeneratorDef read_generator(const World &w, const ObjectRef &r) {
	ParticleGeneratorDef g;
	g.object = r;
	if (!r) return g;
	auto d = r.file->object_data(r.info());
	Contents c(d.contents);
	Chunk ch = begin_chunk(c);
	c.block();  // name
	if (ch.version >= 2) g.local = c.i32() != 0;
	if (ch.version >= 3) {
		// Setup: BOOL +0x80 orientation, +0x84 colour, +0x88 momentum, +0x8c channel 4,
		// +0x90 rotation, +0x94 channel 3.
		g.channels[kChOrientation] = c.boolean();
		g.channels[kChColor] = c.boolean();
		g.channels[kChMomentum] = c.boolean();
		g.channels[kChSize] = c.boolean();
		g.channels[kChRotation] = c.boolean();
		g.channels[kCh3] = c.boolean();
	}
	if (ch.version >= 4) g.reset_per_instance = c.boolean();
	try {
		Contents rc(d.references);
		auto list = chunks(rc);
		if (list.size() >= 2) {
			auto [start, sub] = list[1];
			rc.seek(start);
			begin_chunk(rc);
			while (rc.pos() < sub.end && rc.boolean()) {
				ObjectRef m = w.resolve(*r.file, Reference::read(rc));
				if (m && m.info().class_path.rfind("ParticleModifier\\", 0) == 0) {
					g.modifiers.push_back(read_modifier(w, m));
				}
			}
		}
	} catch (const std::exception &) {
	}
	return g;
}

}  // namespace

bool ParticleModifierDef::is_spawner() const {
	return kind.size() >= 7 && kind.compare(kind.size() - 7, 7, "Spawner") == 0;
}

bool is_particle_renderer(const ObjectRef &r) {
	return r && r.info().class_path.rfind("Mesh\\ParticleRenderer\\", 0) == 0;
}

ObjectRef read_mesh_image(const World &w, const ObjectRef &r) {
	try {
		auto d = r.file->object_data(r.info());
		Contents rc(d.references);
		auto list = chunks(rc);
		if (list.size() < 2) return {};
		auto [start, ch] = list[1];
		rc.seek(start);
		begin_chunk(rc);
		while (rc.pos() < ch.end && rc.boolean()) {
			ObjectRef t = w.resolve(*r.file, Reference::read(rc));
			if (t && t.info().class_path.rfind("Image", 0) == 0) return t;
		}
	} catch (const std::exception &) {
	}
	return {};
}

ParticleRendererDef read_particle_renderer(const World &w, const ObjectRef &r) {
	ParticleRendererDef p;
	p.object = r;
	if (!is_particle_renderer(r)) return p;
	const std::string &cls = r.info().class_path;
	p.kind = cls.substr(std::string("Mesh\\ParticleRenderer\\").size());
	auto d = r.file->object_data(r.info());
	try {
		Contents c(d.contents);
		Chunk mesh = begin_chunk(c);
		c.seek(mesh.end);
		Chunk pr = begin_chunk(c);
		if (pr.version == 2) p.blend = c.i32();
		c.seek(pr.end);
		if (p.kind == "BillboardedQuad" && c.remaining() > 1) {
			Chunk b = begin_chunk(c);
			const uint8_t v = b.version;
			if (v >= 2 && v <= 7) {
				p.color = read_embedded_color(c);
				p.depth_offset = c.f32();
				p.mode = c.i32();
				if (v >= 4) p.uv_mode = c.i32();
				if (v >= 5) p.color_mode = c.i32();
				if (v >= 4) {
					p.width = c.f32();
					p.height = c.f32();
				}
				p.normalize = c.i32();
				int32_t old_flag = 0;
				if (v == 3 || v == 4) old_flag = c.i32();
				if (v >= 6) {
					p.offset[0] = c.f32();
					p.offset[1] = c.f32();
					p.cone_mode = c.i32();
				}
				if (v == 6) {
					p.glow.value = c.f32();
				} else if (v == 7) {
					p.glow = read_embedded_float(c);
					p.offset[2] = c.f32();
					p.f84 = c.f32();
				}
				if (v < 5 && old_flag) p.color_mode = 2;
			}
			c.seek(b.end);
		} else if (p.kind == "Ring" && c.remaining() > 1) {
			Chunk b = begin_chunk(c);
			if (b.version == 1) {
				p.inner_color = read_embedded_color(c);
				p.outer_color = read_embedded_color(c);
				p.ring_plane = c.i32();
				p.ring_dc = c.i32();
				p.ring_d4 = c.i32();
				p.ring_d8 = c.i32();
				p.ring_detail = c.f32();
				p.inner_height = read_embedded_float(c);
				p.outer_height = read_embedded_float(c);
				p.inner_radius = read_embedded_float(c);
				p.outer_radius = read_embedded_float(c);
			}
			c.seek(b.end);
		}
	} catch (const std::exception &) {
	}
	try {
		Contents rc(d.references);
		auto list = chunks(rc);
		// [object][mesh: images][particle renderer: generator][billboard: colour, glow].
		if (list.size() >= 2) {
			auto [start, ch] = list[1];
			rc.seek(start);
			begin_chunk(rc);
			while (rc.pos() < ch.end && rc.boolean()) {
				ObjectRef t = w.resolve(*r.file, Reference::read(rc));
				if (!p.image && t && t.info().class_path.rfind("Image", 0) == 0) p.image = t;
			}
		}
		if (list.size() >= 3) {
			auto [start, ch] = list[2];
			rc.seek(start);
			begin_chunk(rc);
			if (ch.version == 1 && rc.pos() < ch.end && rc.boolean()) {
				p.generator = read_generator(w, w.resolve(*r.file, Reference::read(rc)));
			}
		}
		if (p.kind == "Ring" && list.size() >= 4) {
			auto [start, ch] = list[3];
			rc.seek(start);
			begin_chunk(rc);
			if (ch.version == 1) {
				for (ColorValue *v : {&p.inner_color, &p.outer_color}) {
					if (ObjectRef t = read_advanced_reference(w, *r.file, rc)) *v = read_color_value(w, t);
				}
				for (FloatValue *v : {&p.inner_height, &p.outer_height, &p.inner_radius, &p.outer_radius}) {
					if (ObjectRef t = read_advanced_reference(w, *r.file, rc)) *v = read_float_value(w, t);
				}
			}
		}
		if (p.kind == "BillboardedQuad" && list.size() >= 4) {
			auto [start, ch] = list[3];
			rc.seek(start);
			begin_chunk(rc);
			if (ch.version > 1) {
				if (ObjectRef cr = read_advanced_reference(w, *r.file, rc)) p.color = read_color_value(w, cr), p.color_ref = cr;
				if (ch.version > 2) {
					if (ObjectRef gr = read_advanced_reference(w, *r.file, rc)) p.glow = read_float_value(w, gr);
				}
			}
		}
	} catch (const std::exception &) {
	}
	p.glow_factor = child_float(w, r, "GlowFactor").value_or(0);
	return p;
}

}  // namespace de
