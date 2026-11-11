#include "arena.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>

#include "content.h"
#include "de/contents.h"
#include "game/runtime.h"
#include "game/session.h"

namespace ed {

namespace {

std::string to_utf8(const std::string &latin1) { return deo::latin1_to_utf8(Bytes(latin1.begin(), latin1.end())); }

Bytes stored_text(const std::string &utf8) {
	Bytes b;
	try {
		b = deo::utf8_to_latin1(utf8);
	} catch (const deo::Error &) {
		for (unsigned char c : utf8) b.push_back(c < 0x80 ? c : '?');
	}
	b.push_back(0);
	return b;
}

float get_f32(const Bytes &b, size_t at) {
	float v;
	memcpy(&v, b.data() + at, 4);
	return v;
}
void put_f32(Bytes &b, size_t at, float v) { memcpy(b.data() + at, &v, 4); }
int32_t get_i32(const Bytes &b, size_t at) {
	int32_t v;
	memcpy(&v, b.data() + at, 4);
	return v;
}
void put_i32(Bytes &b, size_t at, int32_t v) { memcpy(b.data() + at, &v, 4); }

Guid to_ed(const de::Guid &g) {
	Guid o;
	memcpy(o.b, g.bytes, 16);
	return o;
}

Bytes guid_bytes(const Guid &g) { return Bytes(g.b, g.b + 16); }

void replace_all(Bytes &b, const Bytes &from, const Bytes &to) {
	if (from.empty() || b.size() < from.size()) return;
	for (size_t i = 0; i + from.size() <= b.size();) {
		if (std::equal(from.begin(), from.end(), b.begin() + long(i))) {
			std::copy(to.begin(), to.end(), b.begin() + long(i));
			i += from.size();
		} else {
			++i;
		}
	}
}

// A references chunk body as the grammar of content.cpp reads it (BOOL-terminated lists, counted
// lists, optional references, nested chunks), kept as a tree so it can be written back with the
// references mapped and the nested chunk sizes recomputed.
struct Item {
	enum Kind { List, Counted, Opt, Chunk } kind = List;
	std::vector<Ref> refs;
	std::vector<size_t> at;  // each reference's offset in the body
	uint8_t version = 0;
	deo::ChunkSize size;
	std::vector<Item> items;
};

void ref_offsets(const std::vector<Item> &items, std::vector<size_t> &out) {
	for (const Item &it : items) {
		out.insert(out.end(), it.at.begin(), it.at.end());
		ref_offsets(it.items, out);
	}
}

// Every way the grammar explains a body, up to `limit`: two that place references differently
// make the body ambiguous, and only a class layout can copy it.
struct Parser {
	const Bytes &b;
	size_t objects, pool;
	int budget = 200000;
	bool valid(const Ref &r) const {
		if (r.kind == Ref::Local) return r.index < objects;
		if (r.kind == Ref::Pool) return r.index < pool;
		return true;
	}
	bool ref(deo::Reader &r, size_t end, Item &it) {
		size_t at = r.pos();
		Ref f = read_ref(r);
		if (r.pos() > end || !valid(f)) return false;
		it.refs.push_back(f);
		it.at.push_back(at);
		return true;
	}
	bool seq(size_t at, size_t end, int depth, std::vector<Item> &acc, const std::function<bool()> &done) {
		if (at == end) return done();
		if (--budget < 0 || depth > 6) return false;
		try {
			deo::Reader r(b, at, end);
			Item it;
			bool ok = true;
			for (;;) {
				uint8_t x = r.u8();
				if (x == 0) break;
				if (x != 1 || !ref(r, end, it)) {
					ok = false;
					break;
				}
			}
			if (ok) {
				acc.push_back(std::move(it));
				if (seq(r.pos(), end, depth, acc, done)) return true;
				acc.pop_back();
			}
		} catch (const deo::Error &) {
		}
		try {
			deo::Reader r(b, at, end);
			uint32_t n = r.u32();
			if (n > 0 && n < 4096) {
				Item it;
				it.kind = Item::Counted;
				bool ok = true;
				for (uint32_t k = 0; k < n && ok; ++k) ok = ref(r, end, it);
				if (ok) {
					acc.push_back(std::move(it));
					if (seq(r.pos(), end, depth, acc, done)) return true;
					acc.pop_back();
				}
			}
		} catch (const deo::Error &) {
		}
		try {
			deo::Reader r(b, at, end);
			Item it;
			it.kind = Item::Opt;
			if (r.u8() == 1 && ref(r, end, it)) {
				acc.push_back(std::move(it));
				if (seq(r.pos(), end, depth, acc, done)) return true;
				acc.pop_back();
			}
		} catch (const deo::Error &) {
		}
		try {
			deo::Reader r(b, at, end);
			size_t e;
			Item it;
			it.kind = Item::Chunk;
			it.version = r.chunk(&e, &it.size);
			if (e <= end && e > r.pos() && it.version >= 1 && it.version <= 15) {
				acc.push_back(std::move(it));
				const size_t slot = acc.size() - 1;
				std::vector<Item> inner;
				if (seq(r.pos(), e, depth + 1, inner, [&] {
					    acc[slot].items = inner;
					    return seq(e, end, depth, acc, done);
				    }))
					return true;
				acc.pop_back();
			}
		} catch (const deo::Error &) {
		}
		return false;
	}
};

void write_items(deo::Writer &w, const std::vector<Item> &items, const std::function<Ref(const Ref &)> &map) {
	for (const Item &it : items) {
		switch (it.kind) {
		case Item::List:
			for (const Ref &r : it.refs) {
				w.u8(1);
				write_ref(w, map(r));
			}
			w.u8(0);
			break;
		case Item::Counted:
			w.u32(uint32_t(it.refs.size()));
			for (const Ref &r : it.refs) write_ref(w, map(r));
			break;
		case Item::Opt:
			w.u8(1);
			write_ref(w, map(it.refs[0]));
			break;
		case Item::Chunk: {
			size_t at = w.begin_chunk(it.version);
			write_items(w, it.items, map);
			w.end_chunk(at, it.size);
			break;
		}
		}
	}
}

// Class layouts the engine's readers document, where the grammar alone can be fooled: L a
// BOOL-terminated list, C an i32 count then references, O a BOOL then a reference, A a nested chunk
// holding an O or nothing (CAdvancedReference), o an O repeated to the end. Alternatives are tried
// in order.
struct ClassLayout {
	const char *class_prefix;
	size_t index;
	std::vector<const char *> ops;
};
const ClassLayout kClassLayouts[] = {
    {"TextureManager", 1, {"C"}},
    {"Light", 1, {"AO", "A"}},
    {"Actor", 1, {"L"}},
    {"Actor\\Affector", 2, {"OO", "O", "L"}},
    {"Actor\\Creature\\Environment", 3, {"AOOOOOOOAAo", "AOOOOOOOAo", "AOOOOOOOo"}},
    {"UniqueActorProperty\\NavigationLink", 2, {"O", "L"}},
    {"Value\\ObjectReference", 2, {"L", "O"}},
    {"Image\\CubeEnvironment", 2, {"OOOOOOo", "L"}},
    {"ScaleModifier", 1, {"AAAAAAAAA", "AAA"}},
    {"Mesh\\ParticleRenderer", 3, {"AA", "A", "AAA"}},
    {"Value\\FLOAT\\MathFunction", 3, {"OOO", "AAA"}},
};

bool map_ops(const char *ops, const Bytes &body, Bytes &out, const std::function<Ref(const Ref &)> &map) {
	try {
		deo::Reader r(body);
		deo::Writer w;
		for (const char *op = ops; *op; ++op) {
			if (*op == 'L') {
				for (;;) {
					uint8_t x = r.u8();
					w.u8(x);
					if (x == 0) break;
					if (x != 1) return false;
					write_ref(w, map(read_ref(r)));
				}
			} else if (*op == 'C') {
				uint32_t n = r.u32();
				if (n > 4096) return false;
				w.u32(n);
				for (uint32_t k = 0; k < n; ++k) write_ref(w, map(read_ref(r)));
			} else if (*op == 'O' || *op == 'o') {
				do {
					uint8_t x = r.u8();
					w.u8(x);
					if (x > 1) return false;
					if (x) write_ref(w, map(read_ref(r)));
				} while (*op == 'o' && r.remaining());
			} else if (*op == 'A') {
				deo::ChunkSize size;
				size_t end;
				uint8_t v = r.chunk(&end, &size);
				size_t at = w.begin_chunk(v);
				if (r.pos() < end) {
					uint8_t x = r.u8();
					w.u8(x);
					if (x > 1) return false;
					if (x) write_ref(w, map(read_ref(r)));
				}
				if (r.pos() != end) return false;
				w.end_chunk(at, size);
			}
		}
		if (r.remaining()) return false;
		out = w.data();
		return true;
	} catch (const deo::Error &) {
		return false;
	}
}

const ClassLayout *class_layout(const std::string &cls, size_t index) {
	const ClassLayout *best = nullptr;
	size_t best_len = 0;
	for (const ClassLayout &l : kClassLayouts) {
		size_t n = strlen(l.class_prefix);
		if (l.index == index && cls.compare(0, n, l.class_prefix) == 0 && (cls.size() == n || cls[n] == '\\') && (!best || n > best_len)) {
			best = &l, best_len = n;
		}
	}
	return best;
}

// DE_CMultiStageTextureProcess references chunk 2 (de::read_multistage's layout).
bool map_multistage(const RawChunk &in, RawChunk &out, const std::function<Ref(const Ref &)> &map) {
	out = in;
	if (in.version < 4 || in.version > 8) return false;
	try {
		deo::Reader r(in.body);
		deo::Writer w;
		uint32_t n = r.u32();
		w.u32(n);
		for (uint32_t i = 0; i < n; ++i) {
			write_ref(w, map(read_ref(r)));
			uint8_t is_ref = r.u8();
			w.u8(is_ref);
			if (is_ref) write_ref(w, map(read_ref(r)));
			else w.bytes(r.bytes(16));
			if (in.version == 4) w.block(r.block());
			else w.bytes(r.bytes(4));
			w.bytes(r.bytes(1 + 4));
			if (in.version >= 6) w.bytes(r.bytes(1));
			if (in.version >= 7) w.bytes(r.bytes(1));
		}
		if (r.remaining()) return false;
		out.body = w.data();
		return true;
	} catch (const deo::Error &) {
		return false;
	}
}

bool map_any_chunk(const std::string &cls, size_t k, const RawChunk &in, RawChunk &out, size_t objects, size_t pool,
                   const std::function<Ref(const Ref &)> &map) {
	out = in;
	if (in.body.empty()) return true;
	if (const ClassLayout *l = class_layout(cls, k)) {
		for (const char *ops : l->ops) {
			if (map_ops(ops, in.body, out.body, map)) return true;
		}
		return false;
	}
	if (cls == "TextureProcess\\MultiStage" && k == 2) return map_multistage(in, out, map);
	if (map_reference_chunk(cls, k, in, out, map)) return true;
	std::vector<std::vector<Item>> found;
	std::vector<Item> acc;
	Parser p{in.body, objects, pool};
	p.seq(0, in.body.size(), 0, acc, [&] {
		found.push_back(acc);
		if (found.size() < 2) return false;
		std::vector<size_t> a, b;
		ref_offsets(found[0], a);
		ref_offsets(found.back(), b);
		if (a == b) found.pop_back();
		return found.size() >= 2;
	});
	if (found.size() != 1) return false;
	deo::Writer w;
	write_items(w, found[0], map);
	out.body = w.data();
	return true;
}

void set_children(deo::Object &o, const std::vector<uint32_t> &kids) {
	Chunks h = split_chunks(o.hierarchy);
	deo::Writer w;
	for (uint32_t k : kids) {
		w.u8(1);
		Ref r;
		r.kind = Ref::Local, r.index = k;
		write_ref(w, r);
	}
	w.u8(0);
	if (h.chunks.empty()) {
		RawChunk c;
		c.version = 1;
		h.chunks.push_back(c);
	}
	h.chunks[0].body = w.data();
	o.hierarchy = join_chunks(h);
}

bool is_actor_class(const std::string &c) { return c == "Actor" || c.rfind("Actor\\", 0) == 0; }

bool contains(const std::string &s, const char *needle) {
	std::string a = s, b = needle;
	for (auto &ch : a) ch = char(tolower(uint8_t(ch)));
	for (auto &ch : b) ch = char(tolower(uint8_t(ch)));
	return a.find(b) != std::string::npos;
}

bool description_listed(const deo::Object &o) {
	Chunks dc = split_chunks(o.contents);
	if (dc.chunks.empty() || dc.chunks[0].version < 3) return false;
	try {
		deo::Reader r(dc.chunks[0].body);
		r.block();
		if (r.u8()) r.seek(r.sized());
		return r.u8() != 0;
	} catch (const deo::Error &) {
		return false;
	}
}

}  // namespace

const char *node_kind_name(NodeKind k) {
	static const char *const names[] = {"Environment", "Group",     "Geometry", "Light",   "Spawn Point", "Pickup Spawner", "Hazard",
	                                    "Fighting Surface", "Force Affector", "Death Volume", "Nav Area", "Nav Link", "Camera", "Effect",
	                                    "Sound", "Affector", "Property", "Value", "Other"};
	return names[std::min(size_t(k), size_t(NodeKind::Other))];
}

std::vector<ArenaEntry> base_arenas(const BaseData &base) {
	std::vector<ArenaEntry> out;
	for (const BaseObject *d : base.of_class("Description\\Environment")) {
		const deo::File &f = base.model(d->file);
		Guid res = description_resource(f.objects[d->index]);
		const BaseObject *env = base.by_guid(res);
		if (!env || env->class_path != "Actor\\Creature\\Environment") continue;
		ArenaEntry e;
		e.name = to_utf8(d->name);
		e.description = d->guid, e.environment = env->guid;
		e.description_file = int(d->file), e.environment_file = int(env->file);
		e.listed = description_listed(f.objects[d->index]);
		e.stem = base.files()[env->file].stem;
		out.push_back(e);
	}
	std::sort(out.begin(), out.end(), [](const ArenaEntry &a, const ArenaEntry &b) { return a.name < b.name; });
	return out;
}

std::vector<ArenaEntry> addon_arenas(const Project &p) {
	std::vector<ArenaEntry> out;
	const auto &objs = p.file().objects;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].info.class_text() != "Description\\Environment") continue;
		Guid res = description_resource(objs[i]);
		int env = p.find(res);
		if (env < 0 || objs[size_t(env)].info.class_text() != "Actor\\Creature\\Environment") continue;
		ArenaEntry e;
		e.name = to_utf8(objs[i].info.name_text());
		e.description = objs[i].info.guid, e.environment = res;
		e.addon = true;
		e.listed = description_listed(objs[i]);
		// The base arena the variant draws: the file most of its environment's pool references reach.
		std::map<size_t, int> votes;
		std::vector<uint32_t> stack{uint32_t(env)};
		std::set<uint32_t> seen;
		while (!stack.empty()) {
			uint32_t k = stack.back();
			stack.pop_back();
			if (!seen.insert(k).second) continue;
			bool understood = false;
			for (const Ref &r : decode_references(objs[k].info.class_text(), objs[k].references, objs.size(), p.file().guid_pool.size(), &understood)) {
				if (r.kind != Ref::Pool || r.index >= p.file().guid_pool.size()) continue;
				if (const BaseObject *b = p.base().by_guid(p.file().guid_pool[r.index])) {
					if (b->category == Category::Meshes || b->category == Category::Materials || b->category == Category::Images) ++votes[b->file];
				}
			}
			for (uint32_t c : hierarchy_children(objs[k])) stack.push_back(c);
		}
		int best = 0;
		for (auto &[file, n] : votes) {
			if (n > best) best = n, e.stem = p.base().files()[file].stem;
		}
		out.push_back(e);
	}
	return out;
}

std::vector<std::string> arena_files(const BaseData &base, const std::string &stem, const deo::File *addon) {
	std::set<size_t> done;
	std::vector<size_t> stack;
	if (const auto *fi = base.file(stem)) stack.push_back(size_t(fi - base.files().data()));
	if (addon) {
		for (const Guid &g : addon->guid_pool) {
			if (const BaseObject *o = base.by_guid(g)) stack.push_back(o->file);
		}
	}
	while (!stack.empty()) {
		size_t f = stack.back();
		stack.pop_back();
		if (!done.insert(f).second) continue;
		for (const Guid &g : base.model(f).guid_pool) {
			if (const BaseObject *o = base.by_guid(g)) stack.push_back(o->file);
		}
	}
	std::vector<std::string> out;
	for (size_t f : done) out.push_back(base.files()[f].stem);
	return out;
}

int ArenaScene::node_for(uint32_t file_index) const {
	auto it = by_index.find(file_index);
	return it == by_index.end() ? -1 : it->second;
}

std::vector<int> ArenaScene::of_kind(NodeKind k) const {
	std::vector<int> out;
	for (size_t i = 0; i < nodes.size(); ++i) {
		if (nodes[i].kind == k) out.push_back(int(i));
	}
	return out;
}

std::optional<float> ArenaScene::surface_below(const std::array<float, 3> &p, float drop) const {
	std::optional<float> best;
	for (const SceneNode &n : nodes) {
		if (n.kind != NodeKind::Surface) continue;
		for (size_t t = 0; t + 2 < n.triangles.size(); t += 3) {
			const auto &a = n.triangles[t], &b = n.triangles[t + 1], &c = n.triangles[t + 2];
			float d0 = (b[0] - a[0]) * (p[2] - a[2]) - (b[2] - a[2]) * (p[0] - a[0]);
			float d1 = (c[0] - b[0]) * (p[2] - b[2]) - (c[2] - b[2]) * (p[0] - b[0]);
			float d2 = (a[0] - c[0]) * (p[2] - c[2]) - (a[2] - c[2]) * (p[0] - c[0]);
			if (!((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0))) continue;
			float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
			float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			if (std::fabs(ny) < 1e-6f) continue;
			float y = a[1] - (nx * (p[0] - a[0]) + nz * (p[2] - a[2])) / ny;
			if (y <= p[1] + 15.0f && p[1] - y <= drop && (!best || y > *best)) best = y;
		}
	}
	return best;
}

ArenaScene read_arena_scene(const de::World &w, const de::ObjectsFile &f, uint32_t root) {
	ArenaScene s;
	s.file = &f;
	const auto &objs = f.objects();
	std::vector<std::vector<uint32_t>> kids(objs.size());
	auto parents = f.parents();
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (parents[i] >= 0) kids[size_t(parents[i])].push_back(i);
	}
	std::function<int(uint32_t, int, const de::Transform &)> add = [&](uint32_t i, int parent, const de::Transform &parent_world) -> int {
		int id = int(s.nodes.size());
		s.nodes.emplace_back();
		s.by_index[i] = id;
		{
			SceneNode &n = s.nodes.back();
			n.index = i, n.parent = parent, n.name = objs[i].name, n.class_path = objs[i].class_path, n.guid = to_ed(objs[i].guid);
			n.ref = {&f, i};
			n.world = parent_world;
			if (is_actor_class(n.class_path)) {
				try {
					n.info = de::read_actor(f, objs[i]);
					n.actor = true;
					n.world = n.info.placed(parent_world);
				} catch (const std::exception &) {
				}
			}
			if (n.class_path.rfind("Actor\\Affector", 0) == 0) {
				game::GameObject g;
				g.class_path = n.class_path;
				game::read_affector(g, w, f, objs[i]);
				n.groups = g.groups;
				n.shape = SceneNode::Shape(uint8_t(g.shape));
				n.hotspot = g.hotspot, n.falloff = g.falloff;
				n.near_begin = g.near_begin, n.near_end = g.near_end, n.far_begin = g.far_begin, n.far_end = g.far_end;
				n.umbra = g.umbra, n.penumbra = g.penumbra;
				n.box_min = g.box_min, n.box_max = g.box_max;
				if (g.mesh) {
					try {
						auto m = de::read_mesh(*g.mesh.file, g.mesh.info());
						for (auto &face : m.faces) {
							for (int k = 0; k < 3; ++k) n.triangles.push_back(n.world.apply(m.vertices[face.v[size_t(k)]]));
						}
					} catch (const std::exception &) {
					}
				}
			}
			if (n.class_path == "Light") n.light = de::read_light(w, n.ref);
			if (n.class_path == "Actor\\PassiveReference") {
				auto targets = game::passive_references(w, n.ref);
				if (!targets.empty() && targets[0]) n.target = targets[0].info().name;
			}
		}
		de::Transform here = s.nodes[size_t(id)].world;
		for (uint32_t k : kids[i]) {
			int c = add(k, id, here);
			s.nodes[size_t(id)].children.push_back(c);
		}
		return id;
	};
	s.root = add(root, -1, de::Transform{});

	// Kinds, now that children are known.
	for (SceneNode &n : s.nodes) {
		auto has_child = [&](const char *name) {
			for (int c : n.children) {
				if (s.nodes[size_t(c)].name == name) return true;
			}
			return false;
		};
		auto has_child_class = [&](const char *prefix) {
			for (int c : n.children) {
				if (s.nodes[size_t(c)].class_path.rfind(prefix, 0) == 0) return true;
			}
			return false;
		};
		const std::string &c = n.class_path;
		if (c == "Actor\\Creature\\Environment") n.kind = NodeKind::Environment;
		else if (c == "Light") n.kind = NodeKind::Light;
		else if (c == "UniqueActorProperty\\NavigationLink") n.kind = NodeKind::NavLink;
		else if (n.actor && n.name.rfind("PlayerSpawnPoint", 0) == 0) n.kind = NodeKind::SpawnPoint;
		else if (n.actor && has_child("PickupsToSpawn")) n.kind = NodeKind::PickupSpawner;
		else if (n.actor && (contains(n.name, "DeathVolume") || contains(n.target, "DeathVolume"))) n.kind = NodeKind::DeathVolume;
		else if (c == "Camera" || (n.actor && has_child_class("Camera"))) n.kind = NodeKind::Camera;
		else if (c.rfind("Actor\\Affector", 0) == 0) {
			if (has_child_class("UniqueActorProperty\\NavigationLink")) n.kind = NodeKind::NavArea;
			else if (contains(n.name, "FightingSurface") && n.shape == SceneNode::Shape::Mesh) n.kind = NodeKind::Surface;
			else if (has_child_class("UniqueActorProperty\\Danger") || contains(n.name, "Attacker") || contains(n.name, "Mine")) n.kind = NodeKind::Hazard;
			else if (has_child_class("UniqueActorProperty\\Material") || contains(n.name, "JumpPad") || contains(n.name, "Force")) n.kind = NodeKind::ForceAffector;
			else if (has_child_class("Light")) n.kind = NodeKind::Light;
			else if (has_child_class("UniqueActorProperty\\Speaker")) n.kind = NodeKind::Sound;
			else n.kind = NodeKind::Affector;
		} else if (c == "Actor\\PassiveReference") {
			n.kind = contains(n.target, "Effect") || contains(n.name, "Effect") ? NodeKind::Effect : NodeKind::Group;
		} else if (n.actor) {
			n.kind = n.children.empty() ? NodeKind::Geometry : NodeKind::Group;
			for (int k : n.children) {
				if (s.nodes[size_t(k)].class_path.rfind("Mesh\\", 0) == 0) n.kind = NodeKind::Geometry;
			}
		} else if (c.rfind("Value", 0) == 0) n.kind = NodeKind::Value;
		else if (c.rfind("UniqueActorProperty", 0) == 0 || c == "ClassExecutor" || c.rfind("AnimationStream", 0) == 0) n.kind = NodeKind::Property;
	}
	// A light's affector carries the light's shape; mark it too so it reads as a light.
	for (SceneNode &n : s.nodes) {
		if (n.class_path == "Light" && n.parent >= 0 && s.nodes[size_t(n.parent)].kind == NodeKind::Affector) s.nodes[size_t(n.parent)].kind = NodeKind::Light;
	}
	// Nav links: their destination area (references chunk 2, the first list's first entry).
	for (SceneNode &n : s.nodes) {
		if (n.kind != NodeKind::NavLink) continue;
		auto lists = game::reference_lists(w, n.ref);
		if (lists.size() > 2 && !lists[2].empty() && !lists[2][0].empty() && lists[2][0][0]) {
			const de::ObjectRef &t = lists[2][0][0];
			if (t.file == &f) n.nav_to = s.node_for(t.index);
		}
	}
	bool any = false;
	for (const SceneNode &n : s.nodes) {
		if (!n.actor || n.kind == NodeKind::Light) continue;
		const auto &p = n.world.t;
		if (std::fabs(p[0]) > 20000 || std::fabs(p[1]) > 20000 || std::fabs(p[2]) > 20000) continue;
		for (int k = 0; k < 3; ++k) {
			s.lo[size_t(k)] = any ? std::min(s.lo[size_t(k)], p[size_t(k)]) : p[size_t(k)];
			s.hi[size_t(k)] = any ? std::max(s.hi[size_t(k)], p[size_t(k)]) : p[size_t(k)];
		}
		any = true;
	}
	return s;
}

// ---------------------------------------------------------------------------------------------
// Copies

int uncopyable_chunk(const std::string &class_path, const Bytes &references, size_t objects, size_t pool) {
	Chunks rc = split_chunks(references);
	for (size_t k = 0; k < rc.chunks.size(); ++k) {
		RawChunk out;
		if (!map_any_chunk(class_path, k, rc.chunks[k], out, objects, pool, [](const Ref &r) { return r; })) return int(k);
	}
	return -1;
}

std::vector<uint32_t> copy_tree(Project &p, const deo::File &src, bool src_is_addon, const std::vector<uint32_t> &roots,
                                std::map<uint32_t, uint32_t> *source_to_copy) {
	const Guid src_agent = src.master.guid;
	std::set<uint32_t> set;
	std::vector<uint32_t> stack(roots.begin(), roots.end());
	while (!stack.empty()) {
		uint32_t i = stack.back();
		stack.pop_back();
		if (i >= src.objects.size() || !set.insert(i).second) continue;
		for (uint32_t c : hierarchy_children(src.objects[i])) stack.push_back(c);
	}
	std::map<uint32_t, uint32_t> local;
	std::map<uint32_t, Guid> fresh;
	uint32_t next = uint32_t(p.file().objects.size());
	for (uint32_t i : set) local[i] = next++, fresh[i] = fresh_guid();
	// The add-on's pool may grow while mapping; a copy from the add-on reads its own pool as it was.
	const std::vector<Guid> src_pool = src.guid_pool;
	const size_t src_objects = src.objects.size();
	auto map = [&](const Ref &r) -> Ref {
		Ref out;
		if (r.kind == Ref::Null) return out;
		if (r.kind == Ref::Local) {
			if (r.index >= src_objects) throw deo::Error("reference to object " + std::to_string(r.index) + " is out of range");
			if (auto it = local.find(r.index); it != local.end()) {
				out.kind = Ref::Local, out.index = it->second;
			} else if (src_is_addon) {
				out.kind = Ref::Local, out.index = r.index;
			} else {
				out.kind = Ref::Pool, out.index = p.pool_index(src.objects[r.index].info.guid);
			}
			return out;
		}
		if (r.index >= src_pool.size()) throw deo::Error("GUID pool reference out of range");
		out.kind = Ref::Pool, out.index = p.pool_index(src_pool[r.index]);
		return out;
	};
	std::vector<deo::Object> made;
	for (uint32_t i : set) {
		deo::Object o = src.objects[i];
		const std::string cls = o.info.class_text();
		o.info.guid = fresh[i];
		const Bytes before = o.contents;
		for (auto &[old, g] : fresh) {
			Bytes from = guid_bytes(src_agent), to = guid_bytes(p.file().master.guid);
			Bytes og = guid_bytes(src.objects[old].info.guid), ng = guid_bytes(g);
			from.insert(from.end(), og.begin(), og.end());
			to.insert(to.end(), ng.begin(), ng.end());
			replace_all(o.contents, from, to);
			replace_all(o.contents, og, ng);
		}
		if (o.compressed() && o.contents != before) o.packed.clear(), o.packed_as_is = false;
		Chunks rc = split_chunks(o.references);
		for (size_t k = 0; k < rc.chunks.size(); ++k) {
			RawChunk out;
			if (!map_any_chunk(cls, k, rc.chunks[k], out, src_objects, src_pool.size(), map)) {
				throw deo::Error("cannot copy " + o.info.name_text() + ": the references of a " + cls + " are not decoded yet");
			}
			rc.chunks[k] = std::move(out);
		}
		o.references = join_chunks(rc);
		Chunks hc = split_chunks(o.hierarchy);
		for (size_t k = 0; k < hc.chunks.size(); ++k) {
			RawChunk out;
			if (!map_reference_chunk("Object", k == 0 ? 0 : 99, hc.chunks[k], out, map)) {
				throw deo::Error("cannot copy " + o.info.name_text() + ": unexpected hierarchy layout");
			}
			hc.chunks[k] = std::move(out);
		}
		o.hierarchy = join_chunks(hc);
		made.push_back(std::move(o));
	}
	for (auto &o : made) p.file().objects.push_back(std::move(o));
	if (source_to_copy) *source_to_copy = local;
	p.touch();
	std::vector<uint32_t> out;
	for (uint32_t r : roots) out.push_back(local.at(r));
	return out;
}

uint32_t create_arena_variant(Project &p, const ArenaEntry &base, const std::string &name) {
	if (base.addon || base.description_file < 0 || base.environment_file < 0) throw deo::Error("only base arenas have variants");
	const BaseData &b = p.base();
	const BaseObject *desc = b.by_guid(base.description);
	const BaseObject *env = b.by_guid(base.environment);
	if (!desc || !env) throw deo::Error("the arena's description or environment is missing");
	const deo::File &env_file = b.model(env->file);
	std::map<uint32_t, uint32_t> copied;
	uint32_t new_env = copy_tree(p, env_file, false, {env->index}, &copied)[0];
	// The fighting-surface collision mesh lives outside the environment, but the engine's floor
	// (ArenaRenderer::floor_height) looks for it by name in the arena's own file.
	for (uint32_t i = 0; i < env_file.objects.size(); ++i) {
		const auto &info = env_file.objects[i].info;
		if (info.class_text() == "Mesh\\Standard" && info.name_text().find("FightingSurfaceCollision") != std::string::npos && !copied.count(i)) {
			copy_tree(p, env_file, false, {i});
		}
	}
	const deo::File &desc_file = b.model(desc->file);
	uint32_t new_desc = copy_tree(p, desc_file, false, {desc->index})[0];
	deo::Object &d = p.file().objects[new_desc];
	// The description's resource: the base environment's passive reference, now the copy's.
	Bytes from = guid_bytes(env_file.master.guid), to = guid_bytes(p.file().master.guid);
	Bytes og = guid_bytes(base.environment), ng = guid_bytes(p.file().objects[new_env].info.guid);
	from.insert(from.end(), og.begin(), og.end());
	to.insert(to.end(), ng.begin(), ng.end());
	replace_all(d.contents, from, to);
	ArenaDescriptionData dd = read_arena_description(p, new_desc);
	dd.name = name, dd.listed = true, dd.locked = false;
	write_arena_description(p, new_desc, dd);
	return new_desc;
}

// Description chunk 0 (DE_CDescription v3): name, resource, listed, f32, locked, visible; then the
// environment description's v1 [i32 min players][i32 max players]; references chunk 1 the casts.
ArenaDescriptionData read_arena_description(const Project &p, uint32_t description) {
	ArenaDescriptionData d;
	const deo::Object &o = p.file().objects.at(description);
	d.name = to_utf8(o.info.name_text());
	Chunks dc = split_chunks(o.contents);
	if (dc.chunks.empty() || dc.chunks[0].version < 3) throw deo::Error("description chunk");
	deo::Reader r(dc.chunks[0].body);
	r.block();
	if (r.u8()) r.seek(r.sized());
	d.listed = r.u8() != 0;
	r.u32();
	d.locked = r.u8() != 0;
	if (dc.chunks.size() > 1 && dc.chunks[1].body.size() >= 8) {
		d.min_players = get_i32(dc.chunks[1].body, 0);
		d.max_players = get_i32(dc.chunks[1].body, 4);
	}
	Chunks rc = split_chunks(o.references);
	if (rc.chunks.size() > 1) {
		deo::Reader rr(rc.chunks[1].body);
		while (rr.remaining() && rr.u8() == 1) {
			Ref ref = read_ref(rr);
			if (ref.kind != Ref::Null) d.casts.push_back(p.target(ref).guid);
		}
	}
	return d;
}

void write_arena_description(Project &p, uint32_t description, const ArenaDescriptionData &d) {
	deo::Object &o = p.file().objects.at(description);
	o.info.name = stored_text(d.name);
	Chunks dc = split_chunks(o.contents);
	if (dc.chunks.empty() || dc.chunks[0].version < 3) throw deo::Error("description chunk");
	RawChunk &c0 = dc.chunks[0];
	deo::Reader r(c0.body);
	r.block();
	size_t name_end = r.pos();
	if (r.u8()) r.seek(r.sized());
	size_t listed = r.pos();
	r.u8();
	r.u32();
	size_t locked = r.pos();
	c0.body[listed] = d.listed, c0.body[locked] = d.locked;
	deo::Writer w;
	w.block(stored_text(d.name));
	w.bytes(Bytes(c0.body.begin() + long(name_end), c0.body.end()));
	c0.body = w.data();
	if (dc.chunks.size() > 1 && dc.chunks[1].body.size() >= 8) {
		put_i32(dc.chunks[1].body, 0, std::clamp(d.min_players, 1, 10));
		put_i32(dc.chunks[1].body, 4, std::clamp(d.max_players, 1, 10));
	}
	o.contents = join_chunks(dc);
	if (o.compressed()) o.packed.clear(), o.packed_as_is = false;
	Chunks rc = split_chunks(o.references);
	if (rc.chunks.size() > 1) {
		RawChunk &c1 = rc.chunks[1];
		deo::Reader rr(c1.body);
		while (rr.u8() == 1) read_ref(rr);
		Bytes rest(c1.body.begin() + long(rr.pos()), c1.body.end());
		deo::Writer cw;
		for (const Guid &g : d.casts) {
			Ref ref;
			int at = p.find(g);
			if (at >= 0) ref.kind = Ref::Local, ref.index = uint32_t(at);
			else ref.kind = Ref::Pool, ref.index = p.pool_index(g);
			cw.u8(1);
			write_ref(cw, ref);
		}
		cw.u8(0);
		cw.bytes(rest);
		c1.body = cw.data();
		o.references = join_chunks(rc);
	}
	p.touch();
}

de::Transform actor_local(const deo::Object &o) {
	de::Transform t;
	Chunks c = split_chunks(o.contents);
	if (c.chunks.empty()) return t;
	deo::Reader r(c.chunks[0].body);
	r.block();
	size_t at = r.pos();
	if (c.chunks[0].body.size() < at + 48) return t;
	for (int k = 0; k < 3; ++k) t.t[size_t(k)] = get_f32(c.chunks[0].body, at + size_t(k) * 4);
	for (int k = 0; k < 9; ++k) t.r[size_t(k)] = get_f32(c.chunks[0].body, at + 12 + size_t(k) * 4);
	return t;
}

void set_actor_local(Project &p, uint32_t object, const de::Transform &local) {
	deo::Object &o = p.file().objects.at(object);
	Chunks c = split_chunks(o.contents);
	if (c.chunks.empty() || c.chunks[0].version < 2) throw deo::Error("not an actor");
	deo::Reader r(c.chunks[0].body);
	r.block();
	size_t at = r.pos();
	if (c.chunks[0].body.size() < at + 48) throw deo::Error("short actor chunk");
	for (int k = 0; k < 3; ++k) put_f32(c.chunks[0].body, at + size_t(k) * 4, local.t[size_t(k)]);
	for (int k = 0; k < 9; ++k) put_f32(c.chunks[0].body, at + 12 + size_t(k) * 4, local.r[size_t(k)]);
	o.contents = join_chunks(c);
	if (o.compressed()) o.packed.clear(), o.packed_as_is = false;
	p.touch();
}

namespace {

// Light contents chunk 0 (v3+): [name][embedded colour: v1 { object chunk, rgba v1 { 4 f32 } }][f32 intensity].
bool light_layout(const Bytes &body, size_t *rgba, size_t *intensity) {
	try {
		deo::Reader r(body);
		r.block();
		size_t colour_end;
		if (r.chunk(&colour_end) != 1) return false;
		size_t obj_end;
		r.chunk(&obj_end);
		r.seek(obj_end);
		size_t rgba_end;
		if (r.chunk(&rgba_end) < 1 || rgba_end - r.pos() < 16) return false;
		*rgba = r.pos();
		r.seek(colour_end);
		*intensity = r.pos();
		return r.remaining() >= 4;
	} catch (const deo::Error &) {
		return false;
	}
}

}  // namespace

bool read_light_data(const deo::Object &light, LightData *d) {
	Chunks c = split_chunks(light.contents);
	if (c.chunks.empty() || c.chunks[0].version < 3) return false;
	size_t rgba, intensity;
	if (!light_layout(c.chunks[0].body, &rgba, &intensity)) return false;
	for (int k = 0; k < 4; ++k) d->rgba[size_t(k)] = get_f32(c.chunks[0].body, rgba + size_t(k) * 4);
	d->intensity = get_f32(c.chunks[0].body, intensity);
	return true;
}

void write_light_data(Project &p, uint32_t light, const LightData &d) {
	deo::Object &o = p.file().objects.at(light);
	Chunks c = split_chunks(o.contents);
	size_t rgba, intensity;
	if (c.chunks.empty() || !light_layout(c.chunks[0].body, &rgba, &intensity)) throw deo::Error("unexpected light layout");
	for (int k = 0; k < 4; ++k) put_f32(c.chunks[0].body, rgba + size_t(k) * 4, d.rgba[size_t(k)]);
	put_f32(c.chunks[0].body, intensity, d.intensity);
	o.contents = join_chunks(c);
	if (o.compressed()) o.packed.clear(), o.packed_as_is = false;
	p.touch();
}

int addon_parent(const Project &p, uint32_t object) {
	const auto &objs = p.file().objects;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		for (uint32_t c : hierarchy_children(objs[i])) {
			if (c == object) return int(i);
		}
	}
	return -1;
}

uint32_t duplicate_actor(Project &p, uint32_t object, const std::string &name) {
	int parent = addon_parent(p, object);
	deo::File snapshot = p.file();
	uint32_t made = copy_tree(p, snapshot, true, {object})[0];
	deo::Object &o = p.file().objects[made];
	o.info.name = stored_text(name);
	Chunks c = split_chunks(o.contents);
	if (!c.chunks.empty()) {
		deo::Reader r(c.chunks[0].body);
		r.block();
		deo::Writer w;
		w.block(stored_text(name));
		w.bytes(Bytes(c.chunks[0].body.begin() + long(r.pos()), c.chunks[0].body.end()));
		c.chunks[0].body = w.data();
		o.contents = join_chunks(c);
		if (o.compressed()) o.packed.clear(), o.packed_as_is = false;
	}
	if (parent >= 0) {
		auto kids = hierarchy_children(p.file().objects[size_t(parent)]);
		kids.push_back(made);
		set_children(p.file().objects[size_t(parent)], kids);
	}
	p.touch();
	return made;
}

void detach_object(Project &p, uint32_t object) {
	int parent = addon_parent(p, object);
	if (parent < 0) return;
	auto kids = hierarchy_children(p.file().objects[size_t(parent)]);
	kids.erase(std::remove(kids.begin(), kids.end(), object), kids.end());
	set_children(p.file().objects[size_t(parent)], kids);
	p.touch();
}

std::vector<Issue> validate_arena(const ArenaScene &s, int max_players) {
	std::vector<Issue> out;
	auto add = [&](Issue::Severity sev, const std::string &text) { out.push_back({sev, 0, -1, text}); };
	auto spawns = s.of_kind(NodeKind::SpawnPoint);
	bool surfaces = !s.of_kind(NodeKind::Surface).empty();
	int off = 0;
	for (int i : spawns) {
		const SceneNode &n = s.nodes[size_t(i)];
		if (surfaces && !s.surface_below(n.world.t, 400)) {
			add(Issue::Error, "Spawn point " + n.name + " is not above a fighting surface");
			++off;
		}
	}
	if (int(spawns.size()) < max_players) {
		add(Issue::Error, "The arena takes up to " + std::to_string(max_players) + " players but has " + std::to_string(spawns.size()) + " spawn points");
	}
	// The nav net: every area reachable from every other over its links.
	auto areas = s.of_kind(NodeKind::NavArea);
	if (!areas.empty()) {
		std::map<int, std::vector<int>> next;
		for (int l : s.of_kind(NodeKind::NavLink)) {
			const SceneNode &n = s.nodes[size_t(l)];
			if (n.nav_to >= 0 && n.parent >= 0) next[n.parent].push_back(n.nav_to);
			else add(Issue::Warning, "Nav link " + n.name + " leads nowhere");
		}
		auto reach = [&](int from) {
			std::set<int> seen{from};
			std::vector<int> stack{from};
			while (!stack.empty()) {
				int a = stack.back();
				stack.pop_back();
				for (int b : next[a]) {
					if (seen.insert(b).second) stack.push_back(b);
				}
			}
			return seen;
		};
		std::vector<std::string> stuck;
		for (int a : areas) {
			auto r = reach(a);
			for (int b : areas) {
				if (!r.count(b)) {
					stuck.push_back(s.nodes[size_t(a)].name + " -> " + s.nodes[size_t(b)].name);
					break;
				}
			}
		}
		if (!stuck.empty()) add(Issue::Warning, "The nav net is not connected: " + std::to_string(stuck.size()) + " area(s) cannot reach every other (" + stuck[0] + ")");
	}
	if (out.empty()) add(Issue::Ok, std::to_string(spawns.size()) + " spawn points on fighting surfaces, " + std::to_string(areas.size()) + " nav areas connected");
	(void)off;
	return out;
}

}  // namespace ed
