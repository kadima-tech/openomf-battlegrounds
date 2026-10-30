#include "game/forces.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "de/contents.h"
#include "de/mesh.h"
#include "game/particles.h"
#include "game/session.h"

namespace game {
namespace {

using Vec = std::array<float, 3>;

Vec sub(const Vec &a, const Vec &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec add(const Vec &a, const Vec &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec mul(const Vec &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec &a, const Vec &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float len(const Vec &a) { return std::sqrt(dot(a, a)); }

float radius(const GameObject &a) { return a.falloff > 0 ? a.falloff : a.hotspot; }

GameObject *top_fighter(GameObject *o) {
	for (GameObject *p = o; p; p = p->parent) {
		if (p->fighter) return p;
	}
	return nullptr;
}

// DE_CAffector +0x118 (owner actor) or, for a fighter's own affectors, the fighter.
GameObject *owner_of(GameObject *a) {
	if (a->owner_actor) return a->owner_actor;
	if (GameObject *f = top_fighter(a)) return f;
	return a;
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
Vec closest_on_triangle(const Vec &p, const Vec &a, const Vec &b, const Vec &c) {
	Vec ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
	float d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	Vec bp = sub(p, b);
	float d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return add(a, mul(ab, d1 / (d1 - d3)));
	Vec cp = sub(p, c);
	float d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return add(a, mul(ac, d2 / (d2 - d6)));
	float va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
	float denom = 1 / (va + vb + vc);
	return add(a, add(mul(ab, vb * denom), mul(ac, vc * denom)));
}

// A mesh affector's triangles in its own frame, and their bounding sphere.
struct MeshShape {
	std::vector<std::array<Vec, 3>> tris;
	Vec centre{0, 0, 0};
	float radius = 0;
	// DataChannel\\Generic children of type "Byte": element name -> one byte per face
	// (contents [name, type, element name, u32][u32 1, u32 count, count bytes]).
	std::map<std::string, std::vector<uint8_t>> elements;
};

const MeshShape *mesh_shape(const de::ObjectRef &r) {
	static std::map<std::pair<const de::ObjectsFile *, uint32_t>, MeshShape> cache;
	if (!r) return nullptr;
	auto key = std::make_pair(r.file, r.index);
	auto it = cache.find(key);
	if (it != cache.end()) return &it->second;
	MeshShape &s = cache[key];
	try {
		de::Mesh m = de::read_mesh(*r.file, r.info());
		Vec lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
		for (auto &f : m.faces) {
			if (f.v[0] >= m.vertices.size() || f.v[1] >= m.vertices.size() || f.v[2] >= m.vertices.size()) continue;
			s.tris.push_back({m.vertices[f.v[0]], m.vertices[f.v[1]], m.vertices[f.v[2]]});
		}
		for (auto &v : m.vertices) {
			for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
		}
		if (!m.vertices.empty()) {
			s.centre = mul(add(lo, hi), 0.5f);
			s.radius = len(sub(hi, s.centre));
		}
		for (uint32_t ci : de::ObjectsFile::children(r.file->object_data(r.info()))) {
			auto &co = r.file->objects()[ci];
			if (co.class_path != "DataChannel\\Generic") continue;
			de::Contents c(r.file->object_data(co).contents);
			de::Chunk head = de::begin_chunk(c);
			auto text = [&]() {
				auto b = c.block();
				std::string t(b.begin(), b.end());
				while (!t.empty() && t.back() == 0) t.pop_back();
				return t;
			};
			text();
			std::string type = text(), element = text();
			c.seek(head.end);
			de::begin_chunk(c);
			c.u32();
			uint32_t n = c.u32();
			if (type != "Byte" || n > c.remaining()) continue;
			auto bytes = c.bytes(n);
			s.elements[element].assign(bytes.begin(), bytes.end());
		}
	} catch (const std::exception &) {
	}
	return &s;
}

// DE_CValueFLOAT::CAdvancedReference contents: a v1 chunk holding the embedded value's object
// and FLOAT chunks.
de::FloatValue read_strength(de::Contents &c) {
	return de::read_embedded_float(c);
}

std::vector<std::pair<size_t, size_t>> chunks_of(const de::Contents &src) {
	de::Contents c = src;
	c.seek(0);
	std::vector<std::pair<size_t, size_t>> out;
	while (c.remaining() > 1) {
		de::Chunk ch = de::begin_chunk(c);
		if (ch.end > c.size() || ch.end < c.pos()) break;
		out.emplace_back(c.pos(), ch.end);
		c.seek(ch.end);
	}
	return out;
}

uint8_t chunk_version(const de::Contents &src, size_t index) {
	de::Contents c = src;
	c.seek(0);
	for (size_t i = 0; c.remaining() > 1; ++i) {
		de::Chunk ch = de::begin_chunk(c);
		if (i == index) return ch.version;
		c.seek(ch.end);
	}
	return 0;
}

float lerp(float a, float b, float t) { return (b - a) * t + a; }

}  // namespace

// DE_CMeshAffector::IsTouchingSphere: one collision per face the sphere reaches.
std::vector<Contact> touch_faces(World &w, GameObject &source, GameObject &target) {
	(void)w;
	std::vector<Contact> out;
	if (source.shape != GameObject::Shape::Sphere || target.shape != GameObject::Shape::Mesh) return out;
	const MeshShape *m = mesh_shape(target.mesh);
	if (!m || m->tris.empty()) return out;
	float r = radius(source);
	de::Transform t = target.world();
	Vec local = t.inverse_rotate(sub(source.world().t, t.t));
	if (len(sub(local, m->centre)) > m->radius + r) return out;
	for (size_t i = 0; i < m->tris.size(); ++i) {
		auto &tri = m->tris[i];
		Vec q = closest_on_triangle(local, tri[0], tri[1], tri[2]);
		Vec push = sub(local, q);
		float dist = len(push);
		if (dist > r) continue;
		Vec fn = cross(sub(tri[1], tri[0]), sub(tri[2], tri[0]));
		float fl = len(fn);
		fn = fl > 0 ? mul(fn, 1 / fl) : Vec{0, 1, 0};
		if (dot(fn, push) < 0) fn = mul(fn, -1);
		Contact c;
		c.source = &source;
		c.touched = &target;
		c.point = t.apply(q);
		c.face_normal = t.rotate(fn);
		c.normal = dist > 1e-5f ? t.rotate(mul(push, 1 / dist)) : c.face_normal;
		c.distance = dist;
		c.depth = r - dist;
		c.face = int(i);
		out.push_back(c);
	}
	return out;
}

std::optional<Contact> touch(World &w, GameObject &source, GameObject &target) {
	// A box or mesh testing a sphere: the sphere's view of it, turned round.
	if (source.shape != GameObject::Shape::Sphere) {
		if (target.shape != GameObject::Shape::Sphere) return std::nullopt;
		auto c = touch(w, target, source);
		if (!c) return std::nullopt;
		std::swap(c->source, c->touched);
		c->normal = mul(c->normal, -1);
		c->face_normal = mul(c->face_normal, -1);
		return c;
	}
	Vec sp = source.world().t;
	float r = radius(source);
	Contact c;
	c.source = &source;
	c.touched = &target;
	switch (target.shape) {
	case GameObject::Shape::Sphere: {
		Vec tp = target.world().t;
		Vec d = sub(tp, sp);
		float dist = len(d), reach = r + radius(target);
		if (dist > reach) return std::nullopt;
		float k = dist > 1e-4f ? std::min(r / dist, 1.0f) : 0;
		c.point = add(sp, mul(d, k));
		c.normal = dist > 1e-4f ? mul(d, -1 / dist) : Vec{0, 1, 0};
		c.face_normal = c.normal;
		c.distance = dist;
		c.depth = reach - dist;
		return c;
	}
	case GameObject::Shape::Box: {
		de::Transform t = target.world();
		Vec local = t.inverse_rotate(sub(sp, t.t));
		Vec q;
		bool inside = true;
		for (int k = 0; k < 3; ++k) {
			q[k] = std::clamp(local[k], target.box_min[k], target.box_max[k]);
			if (q[k] != local[k]) inside = false;
		}
		Vec n;
		float dist;
		if (inside) {
			// Push out through the nearest face.
			int axis = 0;
			float best = 1e30f, sign = 1;
			for (int k = 0; k < 3; ++k) {
				float lo = local[k] - target.box_min[k], hi = target.box_max[k] - local[k];
				if (lo < best) best = lo, axis = k, sign = -1;
				if (hi < best) best = hi, axis = k, sign = 1;
			}
			q = local;
			q[axis] = sign > 0 ? target.box_max[axis] : target.box_min[axis];
			n = {0, 0, 0};
			n[axis] = sign;
			dist = 0;
			c.depth = r + best;
		} else {
			Vec d = sub(local, q);
			dist = len(d);
			if (dist > r) return std::nullopt;
			n = mul(d, 1 / dist);
			c.depth = r - dist;
		}
		c.point = t.apply(q);
		c.normal = c.face_normal = t.rotate(n);
		c.distance = dist;
		return c;
	}
	case GameObject::Shape::Cone: {
		// DE_CConeAffector::IsTouchingSphere (Core 0x70080560; the sphere's IsTouchingCone hands
		// it over): within far end + r of the apex, the depth along +Z overlapping near end ..
		// far end, and the centre within |z| tan(penumbra) + (1 + tan) r of the axis.
		de::Transform t = target.world();
		Vec d = sub(sp, t.t);
		if (len(d) > r + target.far_end) return std::nullopt;
		Vec local = t.inverse_rotate(d);
		float z = local[2];
		if (z - r > target.far_end || target.near_end > z + r) return std::nullopt;
		float tn = std::tan(target.penumbra * 0.017453292f);
		float radial = std::sqrt(local[0] * local[0] + local[1] * local[1]);
		if (!(radial < z * tn || radial - (tn + 1) * r - std::fabs(z * tn) < 0)) return std::nullopt;
		if (target.penumbra == 0) c.point = t.apply({0, 0, z});
		else if (r <= radial) c.point = add(sp, mul(sub(t.t, sp), r / std::max(len(sub(t.t, sp)), 1e-6f)));
		else c.point = t.t;
		Vec n = sub(sp, c.point);
		float nl = len(n);
		c.normal = c.face_normal = nl > 1e-5f ? mul(n, 1 / nl) : Vec{0, 1, 0};
		c.distance = len(sub(c.point, sp));
		return c;
	}
	case GameObject::Shape::Mesh: {
		auto all = touch_faces(w, source, target);
		if (all.empty()) return std::nullopt;
		return *std::min_element(all.begin(), all.end(), [](const Contact &a, const Contact &b) { return a.distance < b.distance; });
	}
	default:
		return std::nullopt;
	}
}

std::vector<Contact> touches(World &w, GameObject &source, uint32_t groups) {
	std::vector<Contact> out;
	GameObject *self = top_fighter(&source);
	w.walk_affectors(groups, [&](GameObject *target, bool record) {
		if (target == &source) return false;
		if (self && top_fighter(target) == self) return false;
		if (target->shape == GameObject::Shape::Mesh && source.shape == GameObject::Shape::Sphere) {
			auto faces = touch_faces(w, source, *target);
			if (record) out.insert(out.end(), faces.begin(), faces.end());
			return !faces.empty();
		}
		auto c = touch(w, source, *target);
		if (c && record) out.push_back(*c);
		return c.has_value();
	});
	return out;
}

// The segment a-b against one affector: where it first enters (0..1 along it). With `all`, every
// mesh face it crosses goes there too, in face order (DE_COctree::GetPotentialLineSegmentCollisions
// merges the cells' sorted face ids, so the candidates come in face order).
static std::optional<std::pair<float, Contact>> segment_hit(GameObject *target, const Vec &a, const Vec &b,
                                                            std::vector<std::pair<float, Contact>> *all = nullptr) {
	Vec d = sub(b, a);
	de::Transform t = target->world();
	Vec la = t.inverse_rotate(sub(a, t.t)), ld = t.inverse_rotate(d);
	std::optional<std::pair<float, Contact>> best;
	auto take = [&](float tt, const Vec &local_normal, int face, float depth = 0) {
		Contact c;
		c.touched = target;
		c.point = add(a, mul(d, tt));
		c.normal = c.face_normal = t.rotate(local_normal);
		c.distance = tt * len(d);
		c.depth = depth;
		c.face = face;
		if (all && face >= 0) all->push_back({tt, c});
		if (!best || tt < best->first) best = std::make_pair(tt, c);
	};
	if (target->shape == GameObject::Shape::Mesh) {
		const MeshShape *m = mesh_shape(target->mesh);
		if (!m) return best;
		// DE_CMeshReference::IsTouchingConeAsLineSegment (Core 0x700854e0): a face counts only when
		// the start is behind or on its plane and the end in front or on it, along the face normal
		// DE_CStandardMesh gives it, (V0 - V1) x (V2 - V1) (into the solid for arena surfaces);
		// ReverseResults (DE_CConeAffector::IsTouchingMesh) turns that normal back at the start.
		for (size_t i = 0; i < m->tris.size(); ++i) {
			auto &tri = m->tris[i];
			Vec e1 = sub(tri[1], tri[0]), e2 = sub(tri[2], tri[0]);
			Vec fn = cross(e2, e1);
			float fl = len(fn);
			if (fl < 1e-12f) continue;
			fn = mul(fn, 1 / fl);
			float z0 = dot(sub(la, tri[0]), fn), z1 = dot(sub(add(la, ld), tri[0]), fn);
			if (z1 == z0 || z0 > 0 || (z0 < 0 && z1 < 0)) continue;
			float tt = z0 / (z0 - z1);
			Vec p = sub(add(la, mul(ld, tt)), tri[0]);
			float d11 = dot(e1, e1), d12 = dot(e1, e2), d22 = dot(e2, e2), p1 = dot(p, e1), p2 = dot(p, e2);
			float den = d11 * d22 - d12 * d12;
			if (std::fabs(den) < 1e-18f) continue;
			float u = (d22 * p1 - d12 * p2) / den, v = (d11 * p2 - d12 * p1) / den;
			if (u < 0 || v < 0 || u + v > 1) continue;
			take(tt, mul(fn, -1), int(i), z0 >= 0 ? -z1 : -z0);
		}
	} else if (target->shape == GameObject::Shape::Box) {
		// DE_CBoxAffector::IsTouchingConeAsLineSegment (Core 0x7007e7a0): only a flat box has a plane
		// (InternalWorldUpdate), met by the whole line, past either end; no box in the data is flat.
		if (target->box_min[2] != target->box_max[2]) return best;
		float L = len(d);
		if (L <= 0) return best;
		Vec lu = mul(ld, 1 / L);
		if (lu[2] == 0) return best;
		float tl = (target->box_min[2] - la[2]) / lu[2];
		float px = la[0] + lu[0] * tl, py = la[1] + lu[1] * tl;
		const float xs[4] = {target->box_min[0], target->box_min[0], target->box_max[0], target->box_max[0]};
		const float ys[4] = {target->box_min[1], target->box_max[1], target->box_max[1], target->box_min[1]};
		bool inside = false;
		for (int i = 0, j = 3; i < 4; j = i++) {
			if (((ys[i] <= py && py < ys[j]) || (ys[j] <= py && py < ys[i])) &&
			    px < (py - ys[i]) * (xs[j] - xs[i]) / (ys[j] - ys[i]) + xs[i])
				inside = !inside;
		}
		if (!inside) return best;
		Contact c;
		c.touched = target;
		c.point = add(a, mul(d, tl / L));
		c.normal = c.face_normal = {0, 0, 0};
		c.distance = tl;
		if (!best || tl / L < best->first) best = std::make_pair(tl / L, c);
	} else if (target->shape == GameObject::Shape::Sphere) {
		// The cone with penumbra 0 against a sphere (DE_CConeAffector::IsTouchingSphere 0x70080560):
		// the centre within r of the axis, its depth along it within -r .. length + r; the touch
		// point on the axis at that depth.
		float r = radius(*target), L = len(d);
		if (L < 1e-6f) return best;
		Vec dir = mul(d, 1 / L), cp = sub(t.t, a);
		float z = dot(cp, dir);
		Vec radial = sub(cp, mul(dir, z));
		if (len(radial) >= r || z < -r || z - r > L) return best;
		float tt = std::clamp(z / L, 0.0f, 1.0f);
		Vec n = mul(dir, -1);
		take(tt, t.inverse_rotate(n), -1);
	}
	return best;
}

std::vector<Contact> segment_hits(World &w, const Vec &a, const Vec &b, uint32_t groups, bool one_per_mesh) {
	std::vector<Contact> out;
	w.walk_affectors(groups, [&](GameObject *target, bool record) {
		std::vector<std::pair<float, Contact>> faces;
		auto h = segment_hit(target, a, b, &faces);
		if (!record) return !faces.empty() || h.has_value();
		if (!faces.empty()) {
			// NoRepeatOwners keeps the face with the least +0x38, which a line segment leaves zero.
			if (one_per_mesh) faces.resize(1);
			for (auto &f : faces) out.push_back(f.second);
		} else if (h) {
			out.push_back(h->second);
		}
		return !faces.empty() || h.has_value();
	});
	return out;
}

std::optional<Contact> segment(World &w, const Vec &a, const Vec &b, uint32_t groups) {
	std::optional<Contact> best;
	for (GameObject *target : w.affectors()) {
		if (!in_scene(*target) || !(target->groups & groups)) continue;
		if (target->shape == GameObject::Shape::Sphere) continue;  // surfaces only
		auto h = segment_hit(target, a, b);
		if (h && (!best || h->second.distance < best->distance)) best = h->second;
	}
	return best;
}

float affector_intensity_at(World &w, GameObject &a, const Vec &p) {
	// "Intensity": set by scripts (SetFLOATValue), else the affector's Value\FLOAT child.
	float intensity = 1;
	auto vit = w.values.find(&a);
	auto it = vit != w.values.end() ? vit->second.find("Intensity") : decltype(vit->second.end()){};
	if (vit != w.values.end() && it != vit->second.end()) {
		memcpy(&intensity, &it->second.bits, 4);
	} else {
		for (GameObject *c : a.children) {
			if (c->name == "Intensity" && c->is("Value\\FLOAT") && c->file) {
				intensity = de::read_float_value(w.data(), {c->file, c->index}).value;
				break;
			}
		}
	}
	de::Transform t = a.world();
	Vec d = sub(p, t.t);
	float dist = len(d);
	float s = 0;
	if (a.shape == GameObject::Shape::Sphere) {
		if (dist <= a.hotspot) s = 1;
		else if (dist >= a.falloff || a.falloff <= a.hotspot) s = 0;
		else s = 1 - (dist - a.hotspot) / (a.falloff - a.hotspot);
	} else if (a.shape == GameObject::Shape::Box) {
		Vec l = t.inverse_rotate(d);
		s = 1;
		for (int k = 0; k < 3; ++k) {
			if (l[k] < a.box_min[k] || l[k] > a.box_max[k]) s = 0;
		}
	}
	return s * intensity;
}

// --- Definitions ----------------------------------------------------------------------------------

// DE_CCommonForce::ObjProcessContents: chunks [unique actor property][force v1][common force:
// v1 type, strength | v2 type, strength (advanced reference), size, mode, rate].
CommonForceDef &ForceSystem::force(GameObject &o) {
	auto it = forces_.find(&o);
	if (it != forces_.end()) return it->second;
	CommonForceDef &d = forces_[&o];
	d.strength.value = 1;
	if (!o.file) return d;
	try {
		de::Contents c(o.file->object_data(o.file->objects()[o.index]).contents);
		auto ch = chunks_of(c);
		if (ch.size() >= 3) {
			uint8_t v = chunk_version(c, 2);
			c.seek(ch[2].first);
			d.type = c.u32();
			if (v == 1) {
				d.strength.value = c.f32();
			} else if (v == 2) {
				d.strength = read_strength(c);
				d.size = c.f32();
				d.mode = c.i32();
				d.rate = c.i32();
			}
		}
	} catch (const std::exception &) {
	}
	return d;
}

// DE_CMaterial / DE_CParticleSpawningMaterial / DE_CCommonForceParticleSpawningMaterial
// ::ObjProcessContents and ObjProcessReferences.
const MaterialDef &ForceSystem::material(GameObject &o) {
	auto it = materials_.find(&o);
	if (it != materials_.end()) return it->second;
	MaterialDef &d = materials_[&o];
	if (!o.file) return d;
	d.spawning = o.is("UniqueActorProperty\\Material\\ParticleSpawningMaterial");
	d.common = o.is("UniqueActorProperty\\Material\\ParticleSpawningMaterial\\CommonForce");
	try {
		auto od = o.file->object_data(o.file->objects()[o.index]);
		de::Contents c(od.contents);
		auto ch = chunks_of(c);
		if (ch.size() >= 2) {
			uint8_t v = chunk_version(c, 1);
			c.seek(ch[1].first);
			auto b = c.block();
			d.element.assign(b.begin(), b.end());
			while (!d.element.empty() && d.element.back() == 0) d.element.pop_back();
			if (v >= 3) {
				d.threshold = c.i32();
				d.value = c.f32();
			}
		}
		if (d.spawning && ch.size() >= 3) {
			uint8_t v = chunk_version(c, 2);
			c.seek(ch[2].first);
			d.spawn_mode = c.i32();
			if (v >= 3) d.spawn_offset = c.f32();
		}
		if (d.common && ch.size() >= 4 && chunk_version(c, 3) > 2) {
			c.seek(ch[3].first);
			d.size_lo = c.f32(), d.size_hi = c.f32(), d.strength_lo = c.f32(), d.strength_hi = c.f32();
			d.psize_lo = c.f32(), d.psize_hi = c.f32(), d.life_lo = c.f32(), d.life_hi = c.f32();
			d.intensity_lo = c.f32();
			d.mask = c.u32();
			d.mesh_heights = c.boolean();  // +0xc4
			if (d.mesh_heights && getenv("OPENBG_DEBUG_MATERIALS")) fprintf(stderr, "[material] %s uses mesh heights (+0xc4)\n", o.name.c_str());
		}
		// References: [property][material][particle spawning v2: BOOL renderer, BOOL spawner]
		// [common force v2: BOOL actor to create].
		de::Contents r(od.references);
		auto rch = chunks_of(r);
		auto ref = [&](de::Contents &rc) -> de::ObjectRef {
			return rc.boolean() ? w_.data().resolve(*o.file, de::Reference::read(rc)) : de::ObjectRef{};
		};
		if (d.spawning && rch.size() >= 4) {
			r.seek(rch[3].first);
			d.renderer = ref(r);
			d.spawner = ref(r);
		}
		if (d.common && rch.size() >= 5) {
			r.seek(rch[4].first);
			d.actor_to_create = ref(r);
		}
	} catch (const std::exception &) {
	}
	return d;
}

// The touched affector's materials: its material children and the materials among its
// properties (DE_CMaterial::ObjPrepare registers each with its actor).
std::vector<GameObject *> ForceSystem::materials_of(GameObject &a) {
	auto it = affector_materials_.find(&a);
	if (it != affector_materials_.end()) return it->second;
	std::vector<GameObject *> out;
	for (GameObject *c : a.children) {
		if (c->is("UniqueActorProperty\\Material")) out.push_back(c);
	}
	if (a.file) {
		for (auto &r : w_.actor_properties({a.file, a.index})) {
			if (!r || r.info().class_path.rfind("UniqueActorProperty\\Material", 0) != 0) continue;
			GameObject *m = nullptr;
			for (GameObject *c : a.children) {
				if (c->file == r.file && c->index == r.index) m = c;
			}
			if (!m) m = w_.global_object(r);
			if (m) m = unique_property(a, *m);  // the copy FindPropertyByName hands the scripts
			if (m && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
		}
	}
	return affector_materials_[&a] = out;
}

// DE_CMaterial::IsValidSurface: a material with an element covers only the mesh faces whose
// byte channel of that name is set (DE_CMesh::GetElementInfo).
bool ForceSystem::valid_surface(GameObject &mo, const Contact &c) {
	const MaterialDef &m = material(mo);
	if (m.element.empty() || c.touched->shape != GameObject::Shape::Mesh) return true;
	const MeshShape *s = mesh_shape(c.touched->mesh);
	if (!s) return false;
	auto it = s->elements.find(m.element);
	return it != s->elements.end() && c.face >= 0 && size_t(c.face) < it->second.size() && it->second[size_t(c.face)];
}

// DE_CSingleCollisionInfo::Material: the first of the touched affector's materials that covers
// the contact.
GameObject *ForceSystem::material_of(const Contact &c) {
	for (GameObject *m : materials_of(*c.touched)) {
		if (valid_surface(*m, c)) return m;
	}
	return nullptr;
}

GameObject *ForceSystem::actor_of(GameObject &p) {
	auto it = owners_.find(&p);
	if (it != owners_.end()) return it->second;
	for (GameObject *a = p.parent; a; a = a->parent) {
		if (a->is_actor) return a;
	}
	return nullptr;
}

GameObject *ForceSystem::unique_property(GameObject &actor, GameObject &shared) {
	// Already the actor's own (in its branch)?
	const GameObject *root = &actor;
	while (root->parent) root = root->parent;
	const GameObject *proot = &shared;
	while (proot->parent) proot = proot->parent;
	if (root == proot) return &shared;
	auto key = std::make_pair(&actor, &shared);
	auto it = copies_.find(key);
	if (it != copies_.end()) return it->second;
	GameObject *copy = w_.copy_tree(shared);
	owners_[copy] = &actor;
	return copies_[key] = copy;
}

float ForceSystem::strength(GameObject &f) {
	CommonForceDef &d = force(f);
	float s = d.strength.eval(0);
	GameObject *a = actor_of(f);
	if (!a || d.mode == 0) return s;
	// DE_CActor::Momentum (units per ms).
	Vec m{0, 0, 0};
	for (GameObject *p = a; p; p = p->parent) {
		if (p->fighter) {
			m = mul(p->momentum, 1.0f / 16);
			break;
		}
	}
	if (d.mode == 1) return len(m) * s;
	if (d.mode == 2) return len(m) * s + s;
	return s;
}

float ForceSystem::strength_at(GameObject &f, const Vec &p) {
	GameObject *a = actor_of(f);
	if (!a || !a->is("Actor\\Affector")) return 0;
	return affector_intensity_at(w_, *a, p) * strength(f);
}

// --- Application ----------------------------------------------------------------------------------

bool ForceSystem::apply(GameObject &source, GameObject &f, uint32_t groups) {
	auto contacts = touches(w_, source, groups);
	static const bool debug = getenv("OPENBG_DEBUG_FORCES") != nullptr;
	if (debug) {
		fprintf(stderr, "[force %u] %s from %s (groups %#x, strength %.2f): %zu contact(s)", w_.frame(), f.name.c_str(),
		        source.name.c_str(), groups, strength(f), contacts.size());
		for (auto &c : contacts) fprintf(stderr, " %s@%.0f,%.0f,%.0f", c.touched->name.c_str(), c.point[0], c.point[1], c.point[2]);
		fprintf(stderr, "\n");
	}
	for (auto &c : contacts) deliver(f, c);
	return !contacts.empty();
}

void ForceSystem::deliver(GameObject &f, const Contact &c) {
	++delivered;
	for (GameObject *m : materials_of(*c.touched)) process(*m, f, c);
}

// DE_CMaterial::ProcessForce(force, results, index) and the subclasses' ProcessForce(force, info).
void ForceSystem::process(GameObject &mo, GameObject &f, const Contact &c) {
	const MaterialDef &m = material(mo);
	static const bool debug = getenv("OPENBG_DEBUG_FORCES") != nullptr;
	if (debug) {
		fprintf(stderr, "  [material] %s on %s: element '%s' mask %#x renderer %s spawner %s hooks %zu\n", mo.name.c_str(),
		        c.touched->name.c_str(), m.element.c_str(), m.mask, m.renderer ? m.renderer.info().name.c_str() : "-",
		        m.spawner ? m.spawner.info().name.c_str() : "-", hooks_.count(&mo) ? hooks_[&mo].size() : size_t(0));
	}
	if (!valid_surface(mo, c)) return;
	uint32_t now = uint32_t(scene_time_ms(w_));
	// DE_CMaterial::ProcessForce (Core 0x7020bb30): a material with a threshold has its own
	// incident recorder (+0x68); a touch counts once per touched owner (the affector's +0x118,
	// else itself) within the threshold.
	if (m.threshold > 0) {
		GameObject *who = c.touched->owner_actor ? c.touched->owner_actor : c.touched;
		auto &rec = incidents_[&mo];
		rec.threshold = uint32_t(m.threshold);
		if (!rec.add_if_absent(now, who->handle)) return;
	}
	if (m.common) {
		if (!is_common_force(f) || !(m.mask & force(f).type)) return;
		float size = force(f).size, str = strength(f);
		float ts = (size - m.size_lo) / (m.size_hi - m.size_lo);
		float tf = (str - m.strength_lo) / (m.strength_hi - m.strength_lo);
		if (size - m.size_lo < 0 || !(ts <= 1) || str - m.strength_lo < 0 || !(tf <= 1)) return;
		float intensity = m.intensity_lo != 1 ? lerp(m.intensity_lo, 1, tf) : 1;
		float psize = lerp(m.psize_lo, m.psize_hi, ts);
		float life = lerp(m.life_lo, m.life_hi, tf);
		// DE_CParticleSpawningMaterial::SpawnSingleParticle.
		if (m.renderer && m.spawner) {
			if (auto *q = w_.particles->requests(c.touched, m.renderer, m.spawner)) {
				NewParticleInfo &i = q->emplace_back();
				i.pos = c.point;
				i.size = psize;
				i.life_span = life;
				if (m.spawn_mode == 1) {
					// Oriented by the surface (y along the normal), pushed out by size x offset.
					Vec n = c.normal, x = std::fabs(n[0]) < 0.9f ? Vec{1, 0, 0} : Vec{0, 0, 1};
					Vec z = cross(x, n);
					z = mul(z, 1 / std::max(len(z), 1e-6f));
					x = cross(n, z);
					i.orientation = {x[0], x[1], x[2], n[0], n[1], n[2], z[0], z[1], z[2]};
					if (m.spawn_offset != 0) i.pos = add(i.pos, mul(n, psize * m.spawn_offset));
					if (intensity != 1) i.intensity = intensity;
				}
			}
		}
		// The created actor (+0x94) stands at the touch point, its Z along the surface normal
		// (DE_CCoordinateSystem::Set(+0x18): Y the world's up made perpendicular, X = Y x Z).
		Vec z = c.face_normal;
		float zl = len(z);
		z = zl > 0 ? mul(z, 1 / zl) : Vec{0, 0, 1};
		Vec up = std::fabs(z[1]) > 0.99f ? Vec{1, 0, 0} : Vec{0, 1, 0};
		Vec x = cross(up, z);
		x = mul(x, 1 / std::max(len(x), 1e-6f));
		Vec y = cross(z, x);
		GameObject *n = nullptr;
		if (m.actor_to_create) {
			if (GameObject *t = w_.global_object(m.actor_to_create)) {
				n = w_.copy_tree(*t);
				n->local.t = c.point;
				n->local.r = {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
			}
		}
		// +0xc4 on a mesh: the touched face's extent along that frame's X from the touch point,
		// as values BeginX (lowest) and EndX (highest) on the created actor and on the force,
		// where the material's script hook reads them (BLightningWall::ProcessForce).
		const MeshShape *ms = c.touched->shape == GameObject::Shape::Mesh ? mesh_shape(c.touched->mesh) : nullptr;
		if (m.mesh_heights && ms && c.face >= 0 && size_t(c.face) < ms->tris.size()) {
			de::Transform tw = c.touched->world();
			float tx = dot(c.point, x), lo = 9999999.0f, hi = -9999999.0f;
			for (const Vec &v : ms->tris[size_t(c.face)]) {
				float vx = dot(tw.apply(v), x);
				if (vx < lo) lo = vx;
				if (vx > hi) hi = vx;
			}
			auto set = [&](GameObject *o, const char *name, float v) {
				uint32_t bits;
				memcpy(&bits, &v, 4);
				w_.values[o][name].bits = bits;
			};
			for (GameObject *o : {n, &f}) {
				if (!o) continue;
				set(o, "BeginX", lo - tx);
				set(o, "EndX", hi - tx);
			}
		}
		if (n) w_.enter_scene(n);
	}
	// DE_CMaterial::ProcessForce(force, position, normal): the script hooks.
	auto hit = hooks_.find(&mo);
	if (hit == hooks_.end()) return;
	if (!vec_a_) vec_a_ = w_.memory().alloc(12), vec_b_ = w_.memory().alloc(12);
	auto hooks = hit->second;
	for (auto &[b, method] : hooks) {
		if (!b || b->inst.shut_down) continue;
		write_vec(w_.memory(), vec_a_, c.point);
		write_vec(w_.memory(), vec_b_, c.normal);
		std::vector<uint8_t> args(12);
		uint32_t a[3] = {f.handle, vec_a_, vec_b_};
		memcpy(args.data(), a, 12);
		b->call_handle(w_, method, args);
	}
}

void ForceSystem::tick() {
	// OPENBG_DEBUG_MAP=x0,x1,z0,z1,step: a top-down map of the fighting surfaces, printed once:
	// M the mine field, s slippery, W too steep to stand on, digits the floor height / 20 (mod
	// 10, '-' below 0), space nothing.
	static bool mapped = false;
	if (const char *spec = getenv("OPENBG_DEBUG_MAP"); spec && !mapped && w_.frame() > 5) {
		mapped = true;
		float x0 = -800, x1 = 800, z0 = -1200, z1 = 1200, st = 25;
		sscanf(spec, "%f,%f,%f,%f,%f", &x0, &x1, &z0, &z1, &st);
		for (float z = z1; z >= z0; z -= st) {
			std::string row;
			for (float x = x0; x <= x1; x += st) {
				auto c = segment(w_, {x, 400, z}, {x, -400, z}, 0x200000);
				char ch = ' ';
				if (c) {
					GameObject *m = material_of(*c);
					float v = m ? material(*m).value : 1;
					if (c->touched->name.find("Mine") != std::string::npos) ch = 'M';
					else if (v * c->normal[1] <= 0.75f) ch = v < 0.1f ? 's' : 'W';
					else ch = c->point[1] < 0 ? '-' : char('0' + int(c->point[1] / 20) % 10);
				}
				row += ch;
			}
			fprintf(stderr, "[map z %5.0f] %s\n", z, row.c_str());
		}
		// The danger the AI perceives there (TotalDangerLevel), 0-9.
		for (float z = z1; z >= z0; z -= st) {
			std::string row;
			for (float x = x0; x <= x1; x += st) {
				auto c = segment(w_, {x, 400, z}, {x, -400, z}, 0x200000);
				float d = w_.danger_at({x, c ? c->point[1] + 15 : 15, z}, nullptr, nullptr, nullptr);
				row += char('0' + std::clamp(int(d * 9.99f), 0, 9));
			}
			fprintf(stderr, "[danger z %5.0f] %s\n", z, row.c_str());
		}
	}
	uint32_t now = uint32_t(scene_time_ms(w_));
	static const bool debug = getenv("OPENBG_DEBUG_FORCES") != nullptr;
	static bool listed = false;
	if (debug && !listed && !w_.affectors().empty()) {
		listed = true;
		// The surfaces forces can reach: name, groups, shape and world bounds.
		for (GameObject *a : w_.affectors()) {
			if (a->shape != GameObject::Shape::Mesh && a->shape != GameObject::Shape::Box) continue;
			de::Transform t = a->world();
			Vec lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
			auto grow = [&](const Vec &p) {
				for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
			};
			if (a->shape == GameObject::Shape::Box) {
				grow(t.apply(a->box_min)), grow(t.apply(a->box_max));
			} else if (const MeshShape *m = mesh_shape(a->mesh)) {
				for (auto &tri : m->tris) {
					for (auto &v : tri) grow(t.apply(v));
				}
			}
			fprintf(stderr, "[surface] %s groups %#x %s %.0f,%.0f,%.0f .. %.0f,%.0f,%.0f, %zu material(s)\n", a->name.c_str(), a->groups,
			        a->shape == GameObject::Shape::Box ? "box" : "mesh", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], materials_of(*a).size());
			// OPENBG_DEBUG_FORCES=profile: surface heights along x at z = 0.
			if (a->shape == GameObject::Shape::Mesh && !strcmp(getenv("OPENBG_DEBUG_FORCES"), "profile")) {
				const MeshShape *m = mesh_shape(a->mesh);
				fprintf(stderr, "  heights:");
				for (float x = -600; x <= 600; x += 25) {
					float best = -1e30f;
					for (auto &tri : m->tris) {
						Vec A = t.apply(tri[0]), B = t.apply(tri[1]), C = t.apply(tri[2]);
						float d = (B[2] - C[2]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[2] - C[2]);
						if (std::fabs(d) < 1e-9f) continue;
						float l1 = ((B[2] - C[2]) * (x - C[0]) + (C[0] - B[0]) * (0 - C[2])) / d;
						float l2 = ((C[2] - A[2]) * (x - C[0]) + (A[0] - C[0]) * (0 - C[2])) / d;
						float l3 = 1 - l1 - l2;
						if (l1 < 0 || l2 < 0 || l3 < 0) continue;
						best = std::max(best, l1 * A[1] + l2 * B[1] + l3 * C[1]);
					}
					fprintf(stderr, " %.0f:%.0f", x, best);
				}
				fprintf(stderr, "\n");
			}
		}
	}
	for (GameObject *a : w_.affectors()) {
		if (!in_scene(*a)) continue;
		for (GameObject *c : a->children) {
			// DE_CCommonForce::Tick: a self-ticking force tests its affector against group 0x200.
			if (c->is("UniqueActorProperty\\Force\\Common")) {
				CommonForceDef &d = force(*c);
				if (d.rate <= 0) continue;
				auto it = last_tick_.find(c);
				if (it == last_tick_.end()) {
					last_tick_[c] = now;
					continue;
				}
				if (now - it->second < uint32_t(d.rate)) continue;
				it->second = now;
				if (strength(*c) > 0) apply(*a, *c, 0x200);
			}
		}
	}
}

// --- Fighter physics against surfaces ------------------------------------------------------------

namespace {

GameObject *find_in_branch(GameObject *o, const de::ObjectRef &r) {
	if (o->file == r.file && o->index == r.index) return o;
	for (GameObject *c : o->children) {
		if (GameObject *hit = find_in_branch(c, r)) return hit;
	}
	return nullptr;
}

// DE_COMF2Fighter::IsStandingState (0x35029ad0): the running state inherits "Ground()".
bool standing_state(GameObject &o) {
	if (!o.brain || !o.brain->exec) return false;
	for (auto *m = o.brain->exec->current_state(); m; m = m->parent) {
		if (m->cm->name == "Ground()") return true;
	}
	return false;
}

void call_hooks(World &w, const std::vector<std::pair<Brain *, uint32_t>> &hooks, const std::vector<uint8_t> &a) {
	auto copy = hooks;
	for (auto &[b, m] : copy) {
		if (!b || b->inst.shut_down) continue;
		auto args = a;
		b->call_handle(w, m, args);
	}
}

}  // namespace

ForceSystem::FighterSurfaces &ForceSystem::surfaces(GameObject &f) {
	FighterSurfaces &fs = fighters_[&f];
	if (fs.resolved || !f.file) return fs;
	fs.resolved = true;
	for (GameObject *c : f.children) {
		if (c->name == "OtherFighterPushSphere" && c->shape == GameObject::Shape::Sphere) fs.push_sphere = c;
	}
	fs.last_pos = f.local.t;
	// The fighter's properties: its Physics\Simple\Fighter (test affectors in references
	// chunk 2, groups in its contents) and the force it applies to what it touches.
	for (auto &r : w_.actor_properties({f.file, f.index})) {
		if (!r) continue;
		const std::string &cls = r.info().class_path;
		if (cls == "Physics\\Simple\\Fighter") {
			auto lists = reference_lists(w_.data(), r);
			if (lists.size() > 2 && !lists[2].empty() && !lists[2][0].empty()) fs.sphere = find_in_branch(&f, lists[2][0][0]);
			try {
				de::Contents cc(r.file->object_data(r.info()).contents);
				auto ch = chunks_of(cc);
				if (ch.size() >= 2 && chunk_version(cc, 1) == 3) {
					cc.seek(ch[1].first + 7 * 4);
					cc.boolean();
					cc.f32(), cc.f32(), cc.f32();
					fs.groups = cc.u32();
				}
			} catch (const std::exception &) {
			}
		} else if (r.info().name == "PlayerForce" && cls.rfind("UniqueActorProperty\\Force", 0) == 0) {
			fs.force = find_in_branch(&f, r);
			if (!fs.force) fs.force = unique_property(f, *w_.global_object(r));
			if (fs.force) owners_[fs.force] = &f;
		}
	}
	return fs;
}

std::vector<const Contact *> ForceSystem::walls(GameObject &f) {
	std::vector<const Contact *> out;
	auto &fs = surfaces(f);
	for (auto &c : fs.contacts) {
		if (fs.friction * c.normal[1] <= 0.75f) out.push_back(&c);
	}
	return out;
}

// DE_CFighterPhysics::UpdateCollisionData: the floor sphere's contacts (not the fighter's own
// affectors); a contact is a wall when its normal is no steeper than 0.75 up, and floor when it
// faces the face's own normal and the fighter is not moving away from it. Running into walls
// faster than 0.1 calls the hit-wall hooks with that speed. Then the fighter is pushed out:
// along the floor's normal's height only, along the push-back normal for the rest, to the
// deepest contact's depth.
bool ForceSystem::collide(GameObject &f, FighterSurfaces &fs, bool probe, bool was) {
	fs.contacts.clear();
	auto all = touches(w_, *fs.sphere, fs.groups);
	for (auto &c : all) {
		if (w_.affector_owner(c.touched) != &f) fs.contacts.push_back(c);
	}
	if (fs.contacts.empty()) {
		fs.friction = 1;
		return false;
	}
	// UpdateForFriction: the contact facing most upwards gives the surface's material value
	// (traction: HighTraction 3, Glass 0.4, Slippery 0.005), 1 without one.
	const Contact *top = nullptr;
	for (auto &c : fs.contacts) {
		if (c.normal[1] > 0 && (!top || c.normal[1] > top->normal[1])) top = &c;
	}
	GameObject *mat = top ? material_of(*top) : nullptr;
	fs.friction = mat ? material(*mat).value : 1;
	Vec m = f.momentum;
	bool any_direction = was || dot(m, m) <= 0.05f;
	Vec dir = any_direction ? Vec{0, 0, 0} : mul(m, 1 / len(m));
	if (probe) any_direction = false, dir = {0, -1, 0};
	Vec wall{0, 0, 0};
	int walls = 0;
	for (auto &c : fs.contacts) {
		if (fs.friction * c.normal[1] <= 0.75f) {
			if (!probe) wall = add(wall, c.normal), ++walls;
		} else if (dot(c.face_normal, c.normal) > 0.9999f && (any_direction || dot(dir, c.face_normal) < 0)) {
			f.on_floor = true;
		}
	}
	if (walls) {
		if (walls > 1) wall = mul(wall, 1 / std::max(len(wall), 1e-6f));
		float into = dot(wall, m);
		if (into < -0.1f) {
			std::vector<uint8_t> a(4);
			float power = -into;
			memcpy(a.data(), &power, 4);
			call_hooks(w_, fs.hit_wall, a);
		}
	}
	Vec push{0, 0, 0};
	float deepest = 0;
	for (auto &c : fs.contacts) {
		if (!f.on_floor || c.normal[1] <= 0.75f || dot(c.normal, c.face_normal) <= 0.9999f) {
			push = add(push, mul(c.normal, c.depth));
		} else {
			push[1] += c.depth * c.normal[1];
		}
		deepest = std::max(deepest, c.depth);
	}
	float pl = len(push);
	if (pl > 1e-6f) f.local.t = add(f.local.t, mul(push, deepest / pl));
	return true;
}

// DE_CFighterPhysics::Tick's pass against the other fighters (PushAgainstOtherFighter
// 0x350398e0, ClampMomentumAgainstOtherFighter 0x35039820).
void ForceSystem::push_against_fighters(GameObject &f) {
	// The other fighters: each push sphere pair is compared at nearly the same height (this one's
	// lifted or lowered towards the other's by its push size less the sphere's diameter); a touch
	// pushes this fighter back along the line between them, by no more than it moved this tick
	// (+0.1), and takes 5% of the momentum going into the other. Spheres touch within the sum of
	// their outer radii (DE_CSphereAffector::IsTouchingSphere 0x70089c30, +0x17c each); a fighter
	// that is not alive (DE_CCreature::IsAlive, vtable +0x17c) is not pushed against.
	FighterSurfaces &fs = surfaces(f);
	if (!fs.skip_push && fs.push_sphere) {
		float moved = len(sub(f.local.t, fs.last_pos)) + 0.1f;
		auto linked = w_.linked_fighters(f);  // walked from the last
		for (auto it = linked.rbegin(); it != linked.rend(); ++it) {
			GameObject *b = *it;
			if (!b->spawned || w_.global_interest_of(b) == 0) continue;
			FighterSurfaces &bs = surfaces(*b);
			if (bs.skip_push || !bs.push_sphere) continue;
			Vec ca = fs.push_sphere->world().t, cb = bs.push_sphere->world().t;
			if (len(sub(cb, ca)) >= 120) continue;
			float ra = fs.push_sphere->falloff, rb = bs.push_sphere->falloff;
			float y = ca[1];
			if (cb[1] <= ca[1]) {
				y = std::max(ca[1] - (b->stats.push_size - 2 * ra), cb[1]);
			} else {
				y = std::min(ca[1] + (f.stats.push_size - 2 * ra), cb[1]);
			}
			ca[1] = y;
			Vec d = sub(cb, ca);
			float dist = len(d), depth = ra + rb - dist;
			static const bool debug = getenv("OPENBG_DEBUG_PUSH") != nullptr;
			if (debug) fprintf(stderr, "[push %u] %s sphere %.1f %.1f %.1f r %.1f vs %s %.1f %.1f %.1f r %.1f: depth %.2f moved %.2f\n", w_.frame(), f.name.c_str(), ca[0], ca[1], ca[2], ra, b->name.c_str(), cb[0], cb[1], cb[2], rb, depth, moved);
			if (depth <= 0 || dist < 1e-5f) continue;
			Vec n = mul(d, 1 / dist);
			f.local.t = sub(f.local.t, mul(n, std::min(depth, moved)));
			float into = dot(f.momentum, n);
			if (into > 0) f.momentum = sub(f.momentum, mul(n, into * 0.05f));
		}
	}
	fs.skip_push = false;
}

void ForceSystem::apply_surface_collision_force(GameObject &f, float multiplier) {
	FighterSurfaces &fs = surfaces(f);
	if (!fs.force || fs.contacts.empty()) return;
	float ml = len(f.momentum);
	std::vector<const GameObject *> done;
	for (auto &c : fs.contacts) {
		const GameObject *who = c.touched->owner_actor ? c.touched->owner_actor : c.touched;
		if (std::find(done.begin(), done.end(), who) != done.end()) continue;
		done.push_back(who);
		CommonForceDef &d = force(*fs.force);
		d.strength = de::FloatValue{};
		d.strength.value = ml == 0 ? 3 : dot(f.momentum, c.normal) * multiplier * 100 + 3;
		deliver(*fs.force, c);
	}
}

bool ForceSystem::fighter_tick(GameObject &f) {
	FighterSurfaces &fs = surfaces(f);
	static const bool why = getenv("OPENBG_DEBUG_FTICK") != nullptr;
	if (!fs.sphere || !fs.groups) {
		if (why) fprintf(stderr, "[ftick %u] %s: sphere %p groups %x\n", w_.frame(), f.name.c_str(), (void *)fs.sphere, fs.groups);
		return false;
	}
	bool any = false;
	for (GameObject *a : w_.affectors()) {
		if (in_scene(*a) && (a->groups & fs.groups) && top_fighter(a) != &f) {
			any = true;
			break;
		}
	}
	if (!any) {
		if (why && w_.frame() % 200 == 0) {
			fprintf(stderr, "[ftick %u] %s: no surfaces in groups %x\n", w_.frame(), f.name.c_str(), fs.groups);
			for (GameObject *a : w_.affectors()) {
				if (a->groups & fs.groups) fprintf(stderr, "  %s active %d owner %s\n", a->name.c_str(), a->active, top_fighter(a) ? top_fighter(a)->name.c_str() : "-");
			}
		}
		return false;
	}
	// Momentum is capped at 10 units a frame.
	float speed = len(f.momentum);
	if (speed > 10) f.momentum = mul(f.momentum, 10 / speed);
	bool was = f.on_floor;
	f.on_floor = false;
	f.local.t = add(f.local.t, f.momentum);
	f.momentum = add(f.momentum, fs.accel);
	Vec moved = f.momentum;
	Vec at = f.local.t;
	collide(f, fs, false, was);
	// Momentum into a touched surface loses a tenth of its normal part per contact.
	for (auto &c : fs.contacts) {
		float ml = len(f.momentum);
		if (ml < 1e-6f) break;
		Vec d = mul(f.momentum, 1 / ml);
		float a = dot(d, c.normal);
		if (a < 0 && dot(d, c.face_normal) < 0) {
			Vec normal_part = mul(c.normal, dot(f.momentum, c.normal));
			f.momentum = add(f.momentum, mul(normal_part, a * 0.1f));
		}
	}
	push_against_fighters(f);
	// Walked off an edge while standing (DE_CFighterPhysics::Tick 0x35039970): look 2 units below
	// the moved position (before this tick's pushes) for the floor; without one, go back to where
	// the pushes left it.
	if (was && !f.on_floor && standing_state(f)) {
		Vec keep = f.local.t;
		f.local.t = {at[0], at[1] - 2, at[2]};
		if (!collide(f, fs, true, was) || !f.on_floor) {
			f.on_floor = false;
			f.local.t = keep;
			collide(f, fs, false, was);
		}
	}
	static const bool debug = getenv("OPENBG_DEBUG_SURFACES") != nullptr;
	if (debug) {
		fprintf(stderr, "[surfaces %u] %s at %.1f %.1f %.1f m %.2f %.2f %.2f%s:", w_.frame(), f.name.c_str(), f.local.t[0], f.local.t[1],
		        f.local.t[2], f.momentum[0], f.momentum[1], f.momentum[2], f.on_floor ? " floor" : "");
		for (auto &c : fs.contacts) {
			fprintf(stderr, " %s n(%.2f %.2f %.2f) face %d (%.2f %.2f %.2f) depth %.2f", c.touched->name.c_str(), c.normal[0], c.normal[1],
			        c.normal[2], c.face, c.face_normal[0], c.face_normal[1], c.face_normal[2], c.depth);
			if (const MeshShape *ms = c.touched->shape == GameObject::Shape::Mesh ? mesh_shape(c.touched->mesh) : nullptr) {
				for (auto &[e, bytes] : ms->elements) {
					if (c.face >= 0 && size_t(c.face) < bytes.size() && bytes[size_t(c.face)]) fprintf(stderr, " [%s]", e.c_str());
				}
			}
		}
		fprintf(stderr, "\n");
	}
	if (f.on_floor != was) {
		std::vector<uint8_t> a(4, 0);  // the COMF2Event (type 4 leave, 5 touch) is not built
		call_hooks(w_, was ? fs.leave_floor : fs.touch_floor, a);
	}
	if (!f.on_floor) {
		fs.accel = {0, -f.gravity * 0.037f, 0};
	} else {
		w_.surface_friction(f, fs.friction);
		f.skip_friction = false;
		fs.accel = {0, 0, 0};
	}
	f.propelled = false;
	if (f.gravity < 1) {
		f.gravity = std::min(1.0f, f.gravity + f.gravity_recover);
	} else if (f.gravity > 1) {
		f.gravity = std::max(1.0f, f.gravity - f.gravity_recover);
	}
	// Every surface touched takes the fighter's force once, as strong as it was hit (10 x the
	// speed into it).
	if (fs.force) {
		std::vector<const GameObject *> done;
		float ml = len(moved);
		for (auto &c : fs.contacts) {
			const GameObject *who = c.touched->owner_actor ? c.touched->owner_actor : c.touched;
			if (std::find(done.begin(), done.end(), who) != done.end()) continue;
			done.push_back(who);
			float s = ml > 0 ? std::fabs(dot(mul(moved, 1 / ml), c.normal) * ml * 10) : 0;
			CommonForceDef &d = force(*fs.force);
			d.strength = de::FloatValue{};
			d.strength.value = s;
			deliver(*fs.force, c);
		}
	}
	fs.last_pos = f.local.t;
	return true;
}

// --- Natives ----------------------------------------------------------------------------------------

void register_force_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	reg("CAffector::bool ApplyForce(class CForce,dword)", [](const NativeContext &c) {
		GameObject *a = c.self(), *f = c.arg_object(0);
		c.ret_bool(a && f && c.world.forces->apply(*a, *f, c.arg<uint32_t>(1)));
	});
	reg("CCommonForce::SetStrength(float)", [](const NativeContext &c) {
		if (GameObject *f = c.self()) {
			auto &d = c.world.forces->force(*f);
			d.strength = de::FloatValue{};
			d.strength.value = c.arg<float>(0);
		}
	});
	reg("CCommonForce::SetSize(float)", [](const NativeContext &c) {
		if (GameObject *f = c.self()) c.world.forces->force(*f).size = c.arg<float>(0);
	});
	reg("CCommonForce::float Strength()", [](const NativeContext &c) {
		GameObject *f = c.self();
		c.ret_f32(f ? c.world.forces->strength(*f) : 0);
	});
	reg("CCommonForce::float StrengthAtPosition(class CVector)", [](const NativeContext &c) {
		GameObject *f = c.self();
		uint32_t v = c.arg<uint32_t>(0);
		c.ret_f32(f && v ? c.world.forces->strength_at(*f, read_vec(c.mem(), v)) : 0);
	});
	reg("CMaterial::RegisterProcessForceHook(class CClassExecutorInstance,Method(class CForceclass CVectorclass CVector))",
	    [](const NativeContext &c) {
		    if (GameObject *m = c.self()) c.world.forces->add_hook(*m, c.world.brain_of(c.ex), c.arg<uint32_t>(1));
	    });
	reg("CObject::bool IsForce(class CObject)", [](const NativeContext &c) {
		GameObject *o = c.arg_object(0);
		c.ret_bool(o && c.world.forces->is_force(*o));
	});
	reg("CObject::bool IsCommonForce(class CObject)", [](const NativeContext &c) {
		GameObject *o = c.arg_object(0);
		c.ret_bool(o && c.world.forces->is_common_force(*o));
	});
	reg("CForce::class CActor Actor()", [](const NativeContext &c) {
		GameObject *f = c.self();
		c.ret_object(f ? c.world.forces->actor_of(*f) : nullptr);
	});
	reg("CUniqueActorProperty::class CActor Actor()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_object(o ? c.world.forces->actor_of(*o) : nullptr);
	});
	// DE_CUniqueActorProperty::Owner (Core 0x700937f0): its actor, or that affector's owner (+0x118).
	reg("CUniqueActorProperty::class CActor Owner()", [](const NativeContext &c) {
		GameObject *o = c.self();
		GameObject *a = o ? c.world.forces->actor_of(*o) : nullptr;
		if (a && a->shape != GameObject::Shape::None) a = c.world.affector_owner(a);
		c.ret_object(a);
	});
	// DE_CActor::Attach (0x70091c30): the object becomes this actor's child, keeping its place
	// (its world offset in this actor's frame); a fighter (DE_COMF2Fighter::Attach 0x3502c0e0)
	// puts it on its Pelvis, at the Pelvis's origin.
	reg("CActor::Attach(class CActor)", [](const NativeContext &c) {
		GameObject *o = c.self(), *k = c.arg_object(0);
		if (!o || !k || k == o) return;
		GameObject *to = o;
		de::Transform kw = k->world();
		bool was = in_scene(*k);
		if (o->fighter) {
			for (GameObject *ch : o->children) {
				if (ch->name == "Pelvis") to = ch;
			}
		}
		if (k->parent) {
			auto &v = k->parent->children;
			v.erase(std::remove(v.begin(), v.end(), k), v.end());
		}
		auto &roots = c.world.scene_roots;
		roots.erase(std::remove(roots.begin(), roots.end(), k), roots.end());
		if (to != o) {
			k->local.t = {0, 0, 0};
		} else {
			de::Transform tw = o->world();
			k->local.t = tw.inverse_rotate(sub(kw.t, tw.t));
		}
		k->parent = to;
		to->children.push_back(k);
		// It joins the scene with its new parent: prepare it (executors), not as a root.
		c.world.parented(k, was);
		c.world.enter_scene(k);
		roots.erase(std::remove(roots.begin(), roots.end(), k), roots.end());
	});
	// IsTouching(groups): is the affector touching anything in those groups?
	reg("CAffector::bool IsTouching(dword)", [](const NativeContext &c) {
		GameObject *a = c.self();
		c.ret_bool(a && !touches(c.world, *a, c.arg<uint32_t>(0)).empty());
	});
	// IsNavigable (0x3502b8f0): the segment's first surface must face up more than 0.75 and its
	// material, if any, must have a positive value (no traction hazards).
	reg("COMF2Fighter::bool IsNavigable(class CVector,class CVector)", [](const NativeContext &c) {
		GameObject *f = c.self();
		auto a = read_vec(c.mem(), c.arg<uint32_t>(0)), b = read_vec(c.mem(), c.arg<uint32_t>(1));
		uint32_t groups = f ? c.world.forces->surfaces(*f).groups : 0;
		bool ok = true;
		if (auto hit = segment(c.world, a, b, groups ? groups : 0x200000)) {
			if (hit->face_normal[1] < 0.75f) ok = false;
			else if (GameObject *m = c.world.forces->material_of(*hit); m && c.world.forces->material(*m).value <= 0) ok = false;
		}
		c.ret_bool(ok);
	});
	// --- Fighter surfaces (DE_COMF2Fighter) ---
	reg("COMF2Fighter::SkipFighterCollision()", [](const NativeContext &c) {
		if (GameObject *f = c.self()) c.world.forces->surfaces(*f).skip_push = true;
	});
	reg("COMF2Fighter::ApplySurfaceCollisionForce(float)", [](const NativeContext &c) {
		if (GameObject *f = c.self()) c.world.forces->apply_surface_collision_force(*f, c.arg<float>(0));
	});
	// SetIsRestingOnFloorOffCaller (0x35022e90): clears the physics' floor contact (+0x18c).
	// SkipFrictionCaller (0x35022200): physics +0x184, cleared by the next tick on the floor.
	reg("COMF2Fighter::SkipFriction()", [](const NativeContext &c) { if (GameObject *f = c.self()) f->skip_friction = true; });
	reg("COMF2Fighter::SetIsRestingOnFloorOff()", [](const NativeContext &c) {
		if (GameObject *f = c.self()) f->on_floor = false;
	});
	reg("COMF2Fighter::int NumWallSurfaces()", [](const NativeContext &c) {
		GameObject *f = c.self();
		c.ret_u32(f ? uint32_t(c.world.forces->walls(*f).size()) : 0);
	});
	reg("COMF2Fighter::class CVector AverageWallSurfaceNormal()", [](const NativeContext &c) {
		GameObject *f = c.self();
		Vec n{0, 0, 0};
		if (f) {
			for (auto *w : c.world.forces->walls(*f)) n = add(n, w->normal);
		}
		float l = len(n);
		if (l > 0) n = mul(n, 1 / l);
		uint32_t v = c.mem().alloc(12);
		write_vec(c.mem(), v, n);
		c.ret_u32(v);
	});
	auto hook_list = [](const char *which) {
		return [which](const NativeContext &c) {
			GameObject *f = c.self();
			if (!f) return;
			auto &fs = c.world.forces->surfaces(*f);
			auto &list = !strcmp(which, "wall") ? fs.hit_wall : !strcmp(which, "touch") ? fs.touch_floor : !strcmp(which, "leave") ? fs.leave_floor : fs.surface_change;
			list.push_back({c.world.brain_of(c.ex), c.arg<uint32_t>(1)});
		};
	};
	reg("COMF2Fighter::RegisterHitWallHook(class CClassExecutorInstance,Method(float))", hook_list("wall"));
	reg("COMF2Fighter::AddTouchFloorRecipient(class CClassExecutorInstance,Method(class COMF2Event))", hook_list("touch"));
	reg("COMF2Fighter::AddLeaveFloorRecipient(class CClassExecutorInstance,Method(class COMF2Event))", hook_list("leave"));
	reg("COMF2Fighter::AddSurfaceChangeRecipient(class CClassExecutorInstance,Method(class COMF2Event))", hook_list("surface"));
	reg("COMF2Fighter::UnregisterHitWallHook(class CClassExecutorInstance,Method(float))", [](const NativeContext &c) {
		GameObject *f = c.self();
		if (!f) return;
		auto &v = c.world.forces->surfaces(*f).hit_wall;
		Brain *b = c.world.brain_of(c.ex);
		uint32_t m = c.arg<uint32_t>(1);
		v.erase(std::remove_if(v.begin(), v.end(), [&](auto &h) { return h.first == b && h.second == m; }), v.end());
	});
	reg("CAffector::bool IsTouching(class CActor)", [](const NativeContext &c) {
		GameObject *a = c.self(), *b = c.arg_object(0);
		c.ret_bool(a && b && touch(c.world, *a, *b).has_value());
	});
}

}  // namespace game
