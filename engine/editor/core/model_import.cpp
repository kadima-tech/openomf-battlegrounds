#include "model_import.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>

#include "arena.h"
#include "content.h"
#include "image_io.h"
#include "model_export.h"

namespace ed {

namespace {

using V3 = std::array<float, 3>;

Bytes stored(const std::string &utf8) {
	Bytes b;
	try {
		b = deo::utf8_to_latin1(utf8);
	} catch (const deo::Error &) {
		for (unsigned char c : utf8) b.push_back(c < 0x80 ? c : '?');
	}
	b.push_back(0);
	return b;
}

std::string lower(std::string s) {
	for (auto &c : s) c = char(tolower(uint8_t(c)));
	return s;
}

// "Collision.001" and "collision" both name the Collision collection.
std::string base_name(const std::string &s) {
	std::string b = lower(s);
	auto dot = b.find('.');
	if (dot != std::string::npos && dot > 0 && b.find_first_not_of("0123456789", dot + 1) == std::string::npos) b = b.substr(0, dot);
	while (!b.empty() && (b.back() == ' ' || b.back() == '_')) b.pop_back();
	return b;
}

V3 sub(const V3 &a, const V3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3 &a, const V3 &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot(const V3 &a, const V3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(const V3 &a) { return std::sqrt(dot(a, a)); }

// ---------------------------------------------------------------------------------------------
// Objects

uint32_t add_object(Project &p, const std::string &class_path, const std::string &name, Bytes contents, Bytes references,
                    Bytes hierarchy = {1, 1, 0}) {
	deo::Object o;
	o.info.name = stored(name);
	o.info.class_path = text_bytes(class_path);
	o.info.guid = fresh_guid();
	o.info.value84 = 1;
	o.contents = std::move(contents);
	o.references = std::move(references);
	o.hierarchy = std::move(hierarchy);
	p.file().objects.push_back(std::move(o));
	return uint32_t(p.file().objects.size() - 1);
}

Ref local_ref(uint32_t i) {
	Ref r;
	r.kind = Ref::Local, r.index = i;
	return r;
}

Bytes ref_list(const std::vector<Ref> &refs) {
	deo::Writer w;
	for (const Ref &r : refs) {
		w.u8(1);
		write_ref(w, r);
	}
	w.u8(0);
	return w.data();
}

void set_children(Project &p, uint32_t parent, const std::vector<uint32_t> &kids) {
	deo::Object &o = p.file().objects[parent];
	Chunks h = split_chunks(o.hierarchy);
	std::vector<Ref> refs;
	for (uint32_t k : kids) refs.push_back(local_ref(k));
	if (h.chunks.empty()) h.chunks.push_back(RawChunk{});
	h.chunks[0].version = 1;
	h.chunks[0].body = ref_list(refs);
	o.hierarchy = join_chunks(h);
}

void add_child(Project &p, uint32_t parent, uint32_t child) {
	auto kids = hierarchy_children(p.file().objects[parent]);
	kids.push_back(child);
	set_children(p, parent, kids);
}

void rename(Project &p, uint32_t object, const std::string &name) {
	deo::Object &o = p.file().objects[object];
	o.info.name = stored(name);
	Chunks c = split_chunks(o.contents);
	if (c.chunks.empty()) return;
	deo::Reader r(c.chunks[0].body);
	r.block();
	deo::Writer w;
	w.block(stored(name));
	w.bytes(Bytes(c.chunks[0].body.begin() + long(r.pos()), c.chunks[0].body.end()));
	c.chunks[0].body = w.data();
	o.contents = join_chunks(c);
	if (o.compressed()) o.packed.clear(), o.packed_as_is = false;
}

void set_contents_chunk(Project &p, uint32_t object, size_t index, const Bytes &body) {
	deo::Object &o = p.file().objects[object];
	Chunks c = split_chunks(o.contents);
	if (c.chunks.size() <= index) throw deo::Error("short contents in " + o.info.name_text());
	c.chunks[index].body = body;
	c.chunks[index].size = {};
	o.contents = join_chunks(c);
	if (o.compressed()) o.packed.clear(), o.packed_as_is = false;
}

Bytes floats(std::initializer_list<float> v) {
	deo::Writer w;
	for (float f : v) {
		uint32_t u;
		memcpy(&u, &f, 4);
		w.u32(u);
	}
	return w.data();
}

void put_f32(deo::Writer &w, float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	w.u32(u);
}

// DE_CActor contents v7: name, position, rotation rows, four BOOLs, position mode, flags and the
// eleven bytes later versions add (zero in the shipped props).
Bytes actor_contents(const std::string &name, const de::Transform &t, int32_t mode = 1, uint32_t flags = 0x1e) {
	deo::Writer w;
	size_t at = w.begin_chunk(7);
	w.block(stored(name));
	for (float v : t.t) put_f32(w, v);
	for (float v : t.r) put_f32(w, v);
	for (int k = 0; k < 4; ++k) w.u8(1);
	w.u32(uint32_t(mode));
	w.u32(flags);
	for (int k = 0; k < 11; ++k) w.u8(0);
	w.end_chunk(at);
	return w.data();
}

uint32_t add_actor(Project &p, const std::string &name, const de::Transform &t, const std::vector<uint32_t> &meshes) {
	std::vector<Ref> comps;
	for (uint32_t m : meshes) comps.push_back(local_ref(m));
	deo::Writer r;
	r.u8(2), r.u8(0);
	size_t at = r.begin_chunk(3);
	r.bytes(ref_list(comps));
	r.end_chunk(at);
	uint32_t a = add_object(p, "Actor", name, actor_contents(name, t), r.data());
	set_children(p, a, meshes);
	return a;
}

uint32_t add_folder(Project &p, const std::string &name) {
	deo::Writer w;
	size_t at = w.begin_chunk(1);
	w.block(stored(name));
	w.end_chunk(at);
	return add_object(p, "Folder", name, w.data(), {2, 0, 1, 0});
}

// Image\Standard: DE_CImage v6 (type 0, size, mipmapped, channel mask) and DE_CStandardImage v7
// (texture format: 20 R8G8B8, 0 with alpha), as the shipped images; the pixels go in one
// uncompressed EX level (set_image_pixels' layout), RGB or RGBA planes.
uint32_t add_image(Project &p, const std::string &name, int w, int h, const std::vector<uint8_t> &rgba) {
	bool alpha = false;
	for (size_t i = 3; i < rgba.size(); i += 4) alpha = alpha || rgba[i] != 255;
	deo::Writer c;
	size_t at = c.begin_chunk(6);
	c.block(stored(name));
	c.u32(0);
	c.u32(uint32_t(w));
	c.u32(uint32_t(h));
	c.u8(1);
	put_f32(c, 0);
	c.u8(1);
	c.u32(alpha ? 0x0f : 0x07);
	c.u32(1);
	c.u32(1);
	for (int k = 0; k < 21; ++k) c.u8(0);
	c.end_chunk(at);
	at = c.begin_chunk(7);
	c.u32(alpha ? 0 : 20);
	c.u32(1);
	for (int k = 0; k < 18; ++k) c.u8(0);
	c.end_chunk(at);
	uint32_t img = add_object(p, "Image\\Standard", name, c.data(), {2, 0, 1, 1, 0, 1, 0});
	deo::Writer plain;
	for (int ch = 0; ch < (alpha ? 4 : 3); ++ch) {
		plain.u32(uint32_t(ch));
		for (int i = 0; i < w * h; ++i) plain.u8(rgba[size_t(i) * 4 + size_t(ch)]);
	}
	plain.u32(0xFFFFFFFFu);
	deo::Writer stream;
	stream.u8(0);
	stream.block(plain.data());
	deo::Writer payload;
	payload.u8(2);
	payload.block(stream.data());
	deo::Ex px;
	px.key = {0, 0};
	px.data = payload.data();
	deo::Ex src;
	src.key = text_bytes("OBJImportFilename");
	src.flag = 1;
	src.data = stored(name + ".png");
	p.file().objects[img].ex = {px, src};
	return img;
}

int pow2_at_most(int v, int cap) {
	int p = 1;
	while (p * 2 <= v && p * 2 <= cap) p *= 2;
	if (p < v && p * 2 <= cap && (v - p) > (p * 2 - v)) p *= 2;
	return std::max(1, p);
}

// ---------------------------------------------------------------------------------------------
// Meshes

struct StageSpec {
	std::string name, cls = "TextureManager";
	int image = -1;    // glTF image
	int uv_set = 0;
	int32_t mode = 0, marked = 0;
	std::array<float, 4> colour{1, 1, 1, 1};
	bool stacked = false, flag_30 = false, flag_v7 = false, disabled = false;
	Guid cube, colour_value, image_guid;
};

struct MaterialSpec {
	std::string name;
	std::vector<StageSpec> stages;
	int version = 8;
	std::array<float, 4> flat{1, 1, 1, 1};  // untextured: an image of this colour
	bool translucent = false;
};

Guid parse_guid(const json::Value *v) {
	Guid g;
	if (!v || !v->is_string() || v->as_string().size() != 32) return g;
	for (int i = 0; i < 16; ++i) g.b[i] = uint8_t(std::stoi(v->as_string().substr(size_t(i) * 2, 2), nullptr, 16));
	return g;
}

float num(const json::Value *v, float d) { return v && v->is_number() ? float(v->as_real()) : d; }
bool flag(const json::Value *v) { return v && v->is_bool() && v->as_bool(); }

MaterialSpec material_spec(const gltf::Scene &s, int index) {
	MaterialSpec m;
	if (index < 0 || size_t(index) >= s.materials.size()) {
		m.name = "Default";
		return m;
	}
	const gltf::Material &gm = s.materials[size_t(index)];
	m.name = gm.name;
	m.flat = gm.base_colour;
	m.translucent = gm.alpha == gltf::Material::Alpha::Blend;
	const json::Value *stages = gm.extras.is_object() ? gm.extras.find("de_stages") : nullptr;
	if (stages && stages->is_array() && stages->size()) {
		m.version = int(num(gm.extras.find("de_multistage_version"), 8));
		if (m.version < 5 || m.version > 8) m.version = 8;
		for (const json::Value &v : stages->items()) {
			if (!v.is_object()) continue;
			StageSpec st;
			if (auto *n = v.find("name"); n && n->is_string()) st.name = n->as_string();
			if (auto *c = v.find("class"); c && c->is_string() && c->as_string().rfind("TextureManager", 0) == 0) st.cls = c->as_string();
			st.image = int(num(v.find("image"), -1));
			if (st.image >= int(s.images.size())) st.image = -1;
			st.uv_set = int(num(v.find("uv_set"), 0));
			st.mode = int32_t(num(v.find("mode"), 0));
			st.marked = int32_t(num(v.find("marked"), 0));
			if (auto *c = v.find("colour"); c && c->is_array() && c->size() == 4) {
				for (int k = 0; k < 4; ++k) st.colour[size_t(k)] = float(c->items()[size_t(k)].as_real());
			}
			st.stacked = flag(v.find("stacked")), st.flag_30 = flag(v.find("flag_30")), st.flag_v7 = flag(v.find("flag_v7")), st.disabled = flag(v.find("disabled"));
			st.cube = parse_guid(v.find("cube_guid"));
			st.colour_value = parse_guid(v.find("colour_value_guid"));
			st.image_guid = parse_guid(v.find("image_guid"));
			m.stages.push_back(st);
		}
		return m;
	}
	StageSpec st;
	st.name = gm.base_texture ? (gm.base_texture.image < int(s.images.size()) ? s.images[size_t(gm.base_texture.image)].name : gm.name) : gm.name;
	st.image = gm.base_texture.image;
	st.uv_set = gm.base_texture.uv_set;
	if (m.translucent) st.mode = 4;
	if (st.image >= 0) st.colour = {1, 1, 1, gm.base_colour[3]};
	m.stages.push_back(st);
	return m;
}

// A glTF primitive placed in DE space by `m` (glTF space to glTF space; the DE flip is applied here).
struct Piece {
	const gltf::Primitive *prim = nullptr;
	gltf::Mat4 m = gltf::identity();
	int material = -1;
};

struct Built {
	std::vector<V3> verts;
	std::vector<de::Mesh::Face> faces;
	struct Mat {
		int material = -1;
		std::vector<uint32_t> faces;
		std::map<int, std::vector<std::array<std::array<float, 2>, 3>>> uvs;  // uv set -> per face in `faces`
	};
	std::vector<Mat> mats;
};

// Smoothing groups: faces joined across an edge whose two sides share normals are in one smooth
// region; each region gets a bit no region across a hard edge has.
void smoothing(Built &b, const std::vector<std::array<V3, 3>> &corner_n) {
	size_t nf = b.faces.size();
	std::vector<int> region(nf, -1);
	auto smooth_edge = [&](size_t f, int k, size_t g) {
		const auto &F = b.faces[f], &G = b.faces[g];
		uint16_t a = F.v[size_t(k)], c = F.v[size_t((k + 1) % 3)];
		for (uint16_t v : {a, c}) {
			int kf = -1, kg = -1;
			for (int j = 0; j < 3; ++j) {
				if (F.v[size_t(j)] == v) kf = j;
				if (G.v[size_t(j)] == v) kg = j;
			}
			if (kf < 0 || kg < 0) return false;
			if (dot(corner_n[f][size_t(kf)], corner_n[g][size_t(kg)]) < 0.999f) return false;
		}
		return true;
	};
	int regions = 0;
	for (size_t f = 0; f < nf; ++f) {
		if (region[f] >= 0) continue;
		std::vector<size_t> stack{f};
		region[f] = regions;
		while (!stack.empty()) {
			size_t cur = stack.back();
			stack.pop_back();
			for (int k = 0; k < 3; ++k) {
				uint16_t g = b.faces[cur].adjacent[size_t(k)];
				if (g == 0xffff || region[g] >= 0 || !smooth_edge(cur, k, g)) continue;
				region[g] = regions;
				stack.push_back(g);
			}
		}
		++regions;
	}
	std::vector<std::set<int>> hard(static_cast<size_t>(regions));
	for (size_t f = 0; f < nf; ++f) {
		for (int k = 0; k < 3; ++k) {
			uint16_t g = b.faces[f].adjacent[size_t(k)];
			if (g != 0xffff && region[g] != region[f]) hard[size_t(region[f])].insert(region[g]);
		}
	}
	std::vector<uint32_t> bit(static_cast<size_t>(regions), 0u);
	for (int r = 0; r < regions; ++r) {
		uint32_t taken = 0;
		for (int o : hard[size_t(r)]) taken |= bit[size_t(o)];
		uint32_t pick = 1;
		for (int k = 0; k < 32; ++k) {
			if (!(taken & (1u << k))) {
				pick = 1u << k;
				break;
			}
		}
		bit[size_t(r)] = pick;
	}
	for (size_t f = 0; f < nf; ++f) b.faces[f].flags = bit[size_t(region[f])];
}

void adjacency(Built &b) {
	std::map<std::pair<uint16_t, uint16_t>, std::vector<uint32_t>> edges;
	for (uint32_t f = 0; f < b.faces.size(); ++f) {
		for (int k = 0; k < 3; ++k) {
			uint16_t a = b.faces[f].v[size_t(k)], c = b.faces[f].v[size_t((k + 1) % 3)];
			edges[{std::min(a, c), std::max(a, c)}].push_back(f);
		}
	}
	for (uint32_t f = 0; f < b.faces.size(); ++f) {
		for (int k = 0; k < 3; ++k) {
			uint16_t a = b.faces[f].v[size_t(k)], c = b.faces[f].v[size_t((k + 1) % 3)];
			uint16_t adj = 0xffff;
			for (uint32_t g : edges[{std::min(a, c), std::max(a, c)}]) {
				if (g != f) {
					adj = uint16_t(g);
					break;
				}
			}
			b.faces[f].adjacent[size_t(k)] = adj;
		}
	}
}

// The pieces as DE meshes, each within the format's u16 limits. With `keep_order` the exporter's
// _DE_VERTEX / de_faces / de_face_flags rebuild the original vertex and face numbering.
std::vector<Built> build_meshes(const std::vector<Piece> &pieces, float units, const gltf::Mesh *source_mesh) {
	struct Tri {
		std::array<V3, 3> p;
		std::array<V3, 3> n;
		std::array<int, 3> src{-1, -1, -1};
		int face = -1;
		int mat = -1;
		const gltf::Primitive *prim;
		std::array<uint32_t, 3> gv;
	};
	std::vector<Tri> tris;
	bool exact = source_mesh && pieces.size() == source_mesh->primitives.size();
	for (const Piece &pc : pieces) {
		const auto &P = *pc.prim;
		auto src = P.custom.find("_DE_VERTEX");
		const json::Value *faces = P.extras.is_object() ? P.extras.find("de_faces") : nullptr;
		if (src == P.custom.end() || src->second.size() != P.positions.size() || !faces || !faces->is_array() || faces->size() * 3 != P.indices.size()) exact = false;
		bool mirrored = false;
		{
			gltf::Mat4 m = pc.m;
			float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
			mirrored = det < 0;
		}
		gltf::Mat4 nm = gltf::inverse(pc.m);
		for (size_t t = 0; t + 2 < P.indices.size(); t += 3) {
			Tri tr;
			tr.prim = &P;
			tr.mat = pc.material;
			// glTF (a, b, c) faces out as DE (a, c, b); a mirroring transform turns it back.
			const int order[3] = {0, mirrored ? 1 : 2, mirrored ? 2 : 1};
			for (int k = 0; k < 3; ++k) {
				uint32_t gi = P.indices[t + size_t(order[k])];
				tr.gv[size_t(k)] = gi;
				tr.p[size_t(k)] = from_gltf(gltf::transform_point(pc.m, P.positions[gi]), units);
				if (gi < P.normals.size()) {
					// Normals by the inverse transpose.
					const auto &n = P.normals[gi];
					gltf::Vec3 tn{nm[0] * n[0] + nm[1] * n[1] + nm[2] * n[2], nm[4] * n[0] + nm[5] * n[1] + nm[6] * n[2], nm[8] * n[0] + nm[9] * n[1] + nm[10] * n[2]};
					V3 d{tn[0], tn[1], -tn[2]};
					float l = length(d);
					tr.n[size_t(k)] = l > 0 ? V3{d[0] / l, d[1] / l, d[2] / l} : V3{0, 1, 0};
				} else {
					tr.n[size_t(k)] = {0, 0, 0};
				}
				if (src != P.custom.end() && gi < src->second.size()) tr.src[size_t(k)] = int(src->second[gi]);
			}
			if (faces && faces->is_array() && t / 3 < faces->size()) tr.face = int(faces->items()[t / 3].as_int());
			tris.push_back(tr);
		}
	}
	// Faces without normals get their face normal everywhere (flat).
	for (auto &tr : tris) {
		if (tr.n[0] == V3{0, 0, 0}) {
			V3 fn = cross(sub(tr.p[0], tr.p[1]), sub(tr.p[2], tr.p[1]));
			float l = length(fn);
			if (l > 0) fn = {-fn[0] / l, -fn[1] / l, -fn[2] / l};
			tr.n = {fn, fn, fn};
		}
	}
	if (exact) {
		std::vector<Tri> ordered(tris.size());
		std::vector<bool> filled(tris.size(), false);
		for (auto &t : tris) {
			if (t.face < 0 || size_t(t.face) >= ordered.size() || filled[size_t(t.face)]) {
				exact = false;
				break;
			}
			ordered[size_t(t.face)] = t;
			filled[size_t(t.face)] = true;
		}
		if (exact) tris = std::move(ordered);
	}
	std::vector<Built> out;
	size_t at = 0;
	while (at < tris.size()) {
		Built b;
		std::map<std::array<float, 3>, uint16_t> weld;
		std::vector<std::array<V3, 3>> corner_n;
		std::map<int, size_t> mat_of;
		for (; at < tris.size(); ++at) {
			const Tri &tr = tris[at];
			if (b.faces.size() >= kMaxMeshFaces || b.verts.size() + 3 > kMaxMeshVertices) break;
			de::Mesh::Face f{};
			for (int k = 0; k < 3; ++k) {
				uint16_t vi;
				if (exact && tr.src[size_t(k)] >= 0) {
					vi = uint16_t(tr.src[size_t(k)]);
					if (b.verts.size() <= vi) b.verts.resize(size_t(vi) + 1, tr.p[size_t(k)]);
					b.verts[vi] = tr.p[size_t(k)];
				} else {
					std::array<float, 3> q;
					for (int j = 0; j < 3; ++j) q[size_t(j)] = std::round(tr.p[size_t(k)][size_t(j)] * 1000.0f) / 1000.0f;
					auto [it, fresh] = weld.try_emplace(q, 0);
					if (fresh) {
						it->second = uint16_t(b.verts.size());
						b.verts.push_back(tr.p[size_t(k)]);
					}
					vi = it->second;
				}
				f.v[size_t(k)] = vi;
			}
			if (f.v[0] == f.v[1] || f.v[1] == f.v[2] || f.v[0] == f.v[2]) continue;
			b.faces.push_back(f);
			corner_n.push_back(tr.n);
			auto [mit, fresh] = mat_of.try_emplace(tr.mat, b.mats.size());
			if (fresh) {
				Built::Mat m;
				m.material = tr.mat;
				b.mats.push_back(m);
			}
			Built::Mat &m = b.mats[mit->second];
			uint32_t fi = uint32_t(b.faces.size() - 1);
			m.faces.push_back(fi);
			const auto &P = *tr.prim;
			for (size_t set = 0; set < P.uvs.size(); ++set) {
				std::array<std::array<float, 2>, 3> uv{};
				for (int k = 0; k < 3; ++k) {
					uint32_t gi = tr.gv[size_t(k)];
					if (gi < P.uvs[set].size()) uv[size_t(k)] = P.uvs[set][gi];
				}
				auto &list = m.uvs[int(set)];
				list.resize(m.faces.size() - 1);
				list.push_back(uv);
			}
		}
		for (auto &m : b.mats) {
			for (auto &[set, list] : m.uvs) list.resize(m.faces.size());
		}
		std::stable_sort(b.mats.begin(), b.mats.end(), [](const Built::Mat &x, const Built::Mat &y) { return x.material < y.material; });
		adjacency(b);
		const json::Value *ff = source_mesh && source_mesh->extras.is_object() ? source_mesh->extras.find("de_face_flags") : nullptr;
		if (exact && ff && ff->is_array() && ff->size() == b.faces.size() && out.empty() && at == tris.size()) {
			for (size_t f = 0; f < b.faces.size(); ++f) b.faces[f].flags = uint32_t(ff->items()[f].as_int());
		} else {
			smoothing(b, corner_n);
		}
		if (!b.faces.empty()) out.push_back(std::move(b));
		if (at < tris.size() && b.faces.empty()) ++at;
	}
	return out;
}

struct Writer {
	Project &p;
	const gltf::Scene &s;
	ImportResult &res;
	std::map<int, uint32_t> images;  // glTF image -> add-on image
	uint32_t maps_folder = UINT32_MAX;
	std::string prefix;

	uint32_t image(int gi) {
		if (auto it = images.find(gi); it != images.end()) return it->second;
		const gltf::Image &img = s.images[size_t(gi)];
		std::vector<uint8_t> rgba = img.rgba;
		int w = img.width, h = img.height;
		if (rgba.empty() || w <= 0 || h <= 0) {
			w = h = 4;
			rgba.assign(64, 255);
			res.notes.push_back("image " + img.name + " could not be decoded; a white one stands in");
		}
		int ow = pow2_at_most(w, kMaxTexture), oh = pow2_at_most(h, kMaxTexture);
		if (ow != w || oh != h) {
			rgba = resize_rgba(w, h, rgba, ow, oh);
			res.notes.push_back("image " + img.name + " resized from " + std::to_string(w) + "x" + std::to_string(h) + " to " + std::to_string(ow) + "x" + std::to_string(oh));
		}
		uint32_t id = add_image(p, img.name.empty() ? prefix + " Image" : img.name, ow, oh, rgba);
		if (maps_folder != UINT32_MAX) add_child(p, maps_folder, id);
		res.made.push_back(id);
		++res.images;
		images[gi] = id;
		return id;
	}

	uint32_t flat_image(const std::string &name, const std::array<float, 4> &c) {
		std::vector<uint8_t> rgba(16 * 4);
		for (size_t i = 0; i < 16; ++i) {
			for (int k = 0; k < 4; ++k) rgba[i * 4 + size_t(k)] = uint8_t(std::clamp(c[size_t(k)], 0.0f, 1.0f) * 255 + 0.5f);
		}
		uint32_t id = add_image(p, name, 4, 4, rgba);
		if (maps_folder != UINT32_MAX) add_child(p, maps_folder, id);
		res.made.push_back(id);
		++res.images;
		return id;
	}

	// A TextureManager (or \Reflection) for one stage: contents v3 [name][uv count], references
	// [object][v2: i32 n, images], ObjExtCon v1 [i32 n, n * {u32 face, 3 * (u, v)}].
	uint32_t manager(const StageSpec &st, const Built::Mat &m, uint32_t image_obj, bool has_image, const Guid &cube) {
		const bool reflection = st.cls == "TextureManager\\Reflection";
		auto uvit = m.uvs.find(st.uv_set);
		const bool with_uvs = !reflection && uvit != m.uvs.end();
		uint32_t n = with_uvs ? uint32_t(m.faces.size()) : 0;
		deo::Writer c;
		size_t at = c.begin_chunk(3);
		c.block(stored(st.name.empty() ? "Stage" : st.name));
		c.u32(n);
		c.end_chunk(at);
		if (reflection) c.u8(1), c.u8(0);
		std::vector<Ref> imgs;
		if (has_image) imgs.push_back(local_ref(image_obj));
		if (!guid_zero(cube)) {
			Ref r;
			r.kind = Ref::Pool, r.index = p.pool_index(cube);
			imgs.push_back(r);
		}
		deo::Writer r;
		r.u8(2), r.u8(0);
		at = r.begin_chunk(2);
		r.u32(uint32_t(imgs.size()));
		for (auto &x : imgs) write_ref(r, x);
		r.end_chunk(at);
		if (reflection) r.u8(1), r.u8(0);
		uint32_t tm = add_object(p, st.cls, st.name.empty() ? "Stage" : st.name, c.data(), r.data());
		if (with_uvs || !reflection) {
			deo::Writer e;
			size_t eat = e.begin_chunk(1);
			e.u32(n);
			for (uint32_t k = 0; k < n; ++k) {
				e.u32(m.faces[k]);
				for (auto &uv : uvit->second[k]) put_f32(e, uv[0]), put_f32(e, uv[1]);
			}
			e.end_chunk(eat);
			deo::Ex ex;
			ex.key = text_bytes("ObjExtCon");
			ex.data = e.data();
			p.file().objects[tm].ex.push_back(ex);
		}
		res.made.push_back(tm);
		return tm;
	}

	// TextureProcess\MultiStage: contents [v1 name][v1]; references [object][texture process][v8
	// i32 n, per stage: manager, BOOL colour is a value (else RGBA), i32 mode, BOOL stacked,
	// i32 marked, BOOL +0x30, BOOL disabled].
	uint32_t material(const MaterialSpec &ms, const Built::Mat &m) {
		std::vector<uint32_t> tms;
		std::vector<StageSpec> stages = ms.stages;
		if (stages.empty()) stages.push_back(StageSpec{});
		deo::Writer st;
		st.u32(uint32_t(stages.size()));
		for (auto &sp : stages) {
			uint32_t img = 0;
			bool has = false;
			if (sp.cls != "TextureManager\\Reflection") {
				if (sp.image >= 0) img = image(sp.image), has = true;
				else if (ms.stages.size() <= 1) img = flat_image(ms.name + " Colour", ms.flat), has = true;
			}
			uint32_t tm = manager(sp, m, img, has, sp.cls == "TextureManager\\Reflection" ? sp.cube : Guid{});
			tms.push_back(tm);
			write_ref(st, local_ref(tm));
			if (!guid_zero(sp.colour_value) && p.base().by_guid(sp.colour_value)) {
				st.u8(1);
				Ref r;
				r.kind = Ref::Pool, r.index = p.pool_index(sp.colour_value);
				write_ref(st, r);
			} else {
				st.u8(0);
				for (float v : sp.colour) put_f32(st, v);
			}
			st.u32(uint32_t(sp.mode));
			st.u8(sp.stacked);
			st.u32(uint32_t(sp.marked));
			if (ms.version >= 6) st.u8(sp.flag_30);
			if (ms.version == 7) st.u8(sp.flag_v7);
			if (ms.version == 8) st.u8(sp.disabled);
		}
		deo::Writer c;
		size_t at = c.begin_chunk(1);
		c.block(stored(ms.name));
		c.end_chunk(at);
		c.u8(1), c.u8(0);
		deo::Writer r;
		r.u8(2), r.u8(0), r.u8(1), r.u8(0);
		at = r.begin_chunk(uint8_t(ms.version));
		r.bytes(st.data());
		r.end_chunk(at);
		uint32_t mat = add_object(p, "TextureProcess\\MultiStage", ms.name, c.data(), r.data());
		set_children(p, mat, tms);
		res.made.push_back(mat);
		++res.materials;
		return mat;
	}

	// Mesh\Standard: [v4 name, flags, placement][v6 u32 vertices, u32 faces], references [object]
	// [v1 materials][v1], ObjExtCon v1 [u16 n, positions][u16 n, faces {3 * (u16 v, u16 adjacent), u32 smoothing}].
	uint32_t mesh(const Built &b, const std::string &name, uint32_t flags, int32_t placement = 0) {
		std::vector<uint32_t> mats;
		for (const auto &m : b.mats) mats.push_back(material(material_spec(s, m.material), m));
		deo::Writer c;
		size_t at = c.begin_chunk(4);
		c.block(stored(name));
		c.u32(flags);
		c.u32(uint32_t(placement));
		c.end_chunk(at);
		at = c.begin_chunk(6);
		c.u32(uint32_t(b.verts.size()));
		c.u32(uint32_t(b.faces.size()));
		c.end_chunk(at);
		std::vector<Ref> mr;
		for (uint32_t m : mats) mr.push_back(local_ref(m));
		deo::Writer r;
		r.u8(2), r.u8(0);
		at = r.begin_chunk(1);
		r.bytes(ref_list(mr));
		r.end_chunk(at);
		r.u8(1), r.u8(0);
		uint32_t mo = add_object(p, "Mesh\\Standard", name, c.data(), r.data());
		set_children(p, mo, mats);
		deo::Writer e;
		at = e.begin_chunk(1);
		e.u16(uint16_t(b.verts.size()));
		for (auto &v : b.verts) put_f32(e, v[0]), put_f32(e, v[1]), put_f32(e, v[2]);
		e.u16(uint16_t(b.faces.size()));
		for (auto &f : b.faces) {
			for (int k = 0; k < 3; ++k) e.u16(f.v[size_t(k)]), e.u16(f.adjacent[size_t(k)]);
			e.u32(f.flags);
		}
		e.end_chunk(at);
		deo::Ex ex;
		ex.key = text_bytes("ObjExtCon");
		ex.data = e.data();
		p.file().objects[mo].ex.push_back(ex);
		res.made.push_back(mo);
		++res.meshes;
		return mo;
	}

	// A collision mesh: geometry only. Its faces keep the drawn winding, which is the collision
	// meshes' too: (V0 - V1) x (V2 - V1) points into the solid (FINDINGS 170).
	uint32_t collision_mesh(const std::vector<V3> &tris, const std::string &name) {
		Built b;
		std::map<std::array<float, 3>, uint16_t> weld;
		for (size_t t = 0; t + 2 < tris.size() && b.faces.size() < kMaxMeshFaces; t += 3) {
			de::Mesh::Face f{};
			for (int k = 0; k < 3; ++k) {
				const V3 &v = tris[t + size_t(k)];
				std::array<float, 3> q{std::round(v[0] * 1000) / 1000, std::round(v[1] * 1000) / 1000, std::round(v[2] * 1000) / 1000};
				auto [it, fresh] = weld.try_emplace(q, uint16_t(b.verts.size()));
				if (fresh) {
					if (b.verts.size() >= kMaxMeshVertices) break;
					b.verts.push_back(v);
				}
				f.v[size_t(k)] = it->second;
			}
			if (f.v[0] == f.v[1] || f.v[1] == f.v[2] || f.v[0] == f.v[2]) continue;
			f.flags = 1;
			b.faces.push_back(f);
		}
		adjacency(b);
		return mesh(b, name, 0x8006);
	}
};

gltf::Mat4 up_fix(const ImportSettings &st) {
	if (!st.z_up) return gltf::identity();
	// Z-up to Y-up: a -90 degree turn about X.
	return {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1};
}

std::vector<Piece> pieces_of(const gltf::Scene &s, int node, const gltf::Mat4 &m) {
	std::vector<Piece> out;
	const gltf::Node &n = s.nodes[size_t(node)];
	if (n.mesh < 0) return out;
	for (const auto &prim : s.meshes[size_t(n.mesh)].primitives) out.push_back({&prim, m, prim.material});
	return out;
}

// Front-facing DE triangles of a node's mesh, placed by `m`.
std::vector<V3> triangles_of(const gltf::Scene &s, int node, const gltf::Mat4 &m, float units) {
	std::vector<V3> out;
	const gltf::Node &n = s.nodes[size_t(node)];
	if (n.mesh < 0) return out;
	float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
	for (const auto &prim : s.meshes[size_t(n.mesh)].primitives) {
		for (size_t t = 0; t + 2 < prim.indices.size(); t += 3) {
			const int order[3] = {0, det < 0 ? 1 : 2, det < 0 ? 2 : 1};
			for (int k : order) out.push_back(from_gltf(gltf::transform_point(m, prim.positions[prim.indices[t + size_t(k)]]), units));
		}
	}
	return out;
}

std::vector<V3> triangles_with_material(const gltf::Scene &s, int node, const gltf::Mat4 &m, float units, const std::function<bool(int)> &keep) {
	std::vector<V3> out;
	const gltf::Node &n = s.nodes[size_t(node)];
	if (n.mesh < 0) return out;
	float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
	for (const auto &prim : s.meshes[size_t(n.mesh)].primitives) {
		if (!keep(prim.material)) continue;
		for (size_t t = 0; t + 2 < prim.indices.size(); t += 3) {
			const int order[3] = {0, det < 0 ? 1 : 2, det < 0 ? 2 : 1};
			for (int k : order) out.push_back(from_gltf(gltf::transform_point(m, prim.positions[prim.indices[t + size_t(k)]]), units));
		}
	}
	return out;
}

de::Transform node_transform(const gltf::Scene &s, int node, const ImportSettings &st) {
	return from_gltf(gltf::multiply(up_fix(st), s.world(node)), st.units_per_metre);
}

Role surface_role(const std::string &material) {
	std::string m = lower(material);
	if (m.find("kill") != std::string::npos || m.find("death") != std::string::npos) return Role::Kill;
	if (m.find("wall") != std::string::npos) return Role::Wall;
	return Role::Floor;
}

}  // namespace

const char *role_name(Role r) {
	static const char *const names[] = {"Visual", "Floor", "Wall", "Kill", "Death volume", "Jump pad", "Spawn point", "Pickup spawner", "Ignore"};
	return names[std::min(size_t(r), size_t(Role::Ignore))];
}

bool ImportPlan::errors() const {
	return std::any_of(issues.begin(), issues.end(), [](const Issue &i) { return i.severity == Issue::Error; });
}

size_t ImportPlan::count(Role r) const {
	return size_t(std::count_if(items.begin(), items.end(), [&](const ImportItem &i) { return i.role == r; }));
}

std::string ImportResult::summary() const {
	std::string s = std::to_string(made.size()) + " objects: " + std::to_string(meshes) + " meshes, " + std::to_string(materials) + " materials, " +
	                std::to_string(images) + " images";
	if (spawns) s += ", " + std::to_string(spawns) + " spawn points";
	if (pickups) s += ", " + std::to_string(pickups) + " pickup spawners";
	if (nav_areas) s += ", nav net " + std::to_string(nav_areas) + " areas / " + std::to_string(nav_links) + " links";
	return s;
}

ImportPlan analyse(const std::filesystem::path &file, const ImportSettings &settings) {
	ImportPlan plan = analyse(gltf::read(file), settings);
	plan.source = file;
	if (plan.settings.name.empty()) plan.settings.name = file.stem().string();
	return plan;
}

ImportPlan analyse(gltf::Scene scene, const ImportSettings &settings) {
	ImportPlan plan;
	plan.scene = std::move(scene);
	plan.settings = settings;
	const gltf::Scene &s = plan.scene;
	if (s.extras.is_object()) {
		if (const json::Value *u = s.extras.find("de_units_per_metre"); u && u->is_number() && u->as_real() > 0) {
			plan.settings.units_per_metre = float(u->as_real());
			plan.from_editor = true;
		}
		if (plan.settings.name.empty()) {
			if (const json::Value *n = s.extras.find("de_source"); n && n->is_string()) plan.settings.name = n->as_string();
		}
	}
	static const std::map<std::string, Role> collections = {{"visual", Role::Visual},   {"collision", Role::Floor}, {"death", Role::Death},
	                                                        {"jumppad", Role::JumpPad}, {"jump pad", Role::JumpPad}, {"jumppads", Role::JumpPad},
	                                                        {"spawn", Role::Spawn},     {"spawns", Role::Spawn},    {"pickup", Role::Pickup},
	                                                        {"pickups", Role::Pickup},  {"navigation", Role::Ignore}};
	auto collection_of = [&](int node, std::string *name) -> std::optional<Role> {
		for (int n = node; n >= 0; n = s.nodes[size_t(n)].parent) {
			const gltf::Node &nd = s.nodes[size_t(n)];
			std::string key;
			if (nd.extras.is_object()) {
				if (const json::Value *c = nd.extras.find("de_collection"); c && c->is_string()) key = base_name(c->as_string());
				if (const json::Value *c = nd.extras.find("de_role"); c && c->is_string()) key = base_name(c->as_string());
			}
			if (key.empty() || !collections.count(key)) key = base_name(nd.name);
			if (auto it = collections.find(key); it != collections.end()) {
				*name = nd.name;
				return it->second;
			}
		}
		return std::nullopt;
	};
	for (size_t i = 0; i < s.nodes.size(); ++i) {
		const gltf::Node &n = s.nodes[i];
		std::string coll;
		auto role = collection_of(int(i), &coll);
		bool empty_role = role && (*role == Role::Spawn || *role == Role::Pickup);
		// A collection node itself is not an item; its children are.
		if (role && base_name(n.name) == base_name(coll) && n.mesh < 0) continue;
		if (n.mesh < 0 && !empty_role) continue;
		if (n.mesh < 0 && !n.children.empty()) continue;
		ImportItem it;
		it.node = int(i);
		it.name = n.name;
		auto path = s.path(int(i));
		for (size_t k = 0; k < path.size(); ++k) it.path += (k ? "/" : "") + path[k];
		it.collection = coll;
		if (n.mesh >= 0) {
			for (const auto &p : s.meshes[size_t(n.mesh)].primitives) it.triangles += p.triangles(), it.vertices += p.positions.size();
		}
		if (settings.kind == ImportKind::Prop) {
			it.role = n.mesh >= 0 ? Role::Visual : Role::Ignore;
		} else {
			it.role = role ? *role : (n.mesh >= 0 ? Role::Visual : Role::Ignore);
			if (it.role == Role::Floor && n.mesh >= 0) {
				// Collision: the first primitive's material names the surface; mixed meshes split at import.
				const auto &prims = s.meshes[size_t(n.mesh)].primitives;
				if (!prims.empty() && prims[0].material >= 0) it.role = surface_role(s.materials[size_t(prims[0].material)].name);
			}
			if ((it.role == Role::Spawn || it.role == Role::Pickup) && n.mesh >= 0 && n.children.empty()) {
				// A marker mesh in Spawn / Pickup stands for its position.
			}
		}
		if (n.extras.is_object()) {
			if (const json::Value *pk = n.extras.find("pickups"); pk && pk->is_array()) {
				for (auto &v : pk->items()) {
					if (v.is_string()) it.pickups.push_back(v.as_string());
				}
			} else if (pk && pk->is_string()) {
				it.pickups.push_back(pk->as_string());
			}
			if (const json::Value *im = n.extras.find("impulse"); im && im->is_array() && im->size() == 3) {
				for (int k = 0; k < 3; ++k) it.impulse[size_t(k)] = float(im->items()[size_t(k)].as_real());
				it.has_impulse = true;
			}
		}
		plan.items.push_back(std::move(it));
	}
	for (size_t m = 0; m < s.materials.size(); ++m) {
		ImportMaterial im;
		im.material = int(m);
		im.name = s.materials[m].name;
		const json::Value *st = s.materials[m].extras.is_object() ? s.materials[m].extras.find("de_stages") : nullptr;
		im.rebuilt = st && st->is_array() && st->size();
		im.stages = im.rebuilt ? st->size() : 1;
		im.image = s.materials[m].base_texture.image;
		plan.materials.push_back(im);
	}
	for (size_t i = 0; i < s.images.size(); ++i) {
		ImportImage ii;
		ii.image = int(i);
		ii.name = s.images[i].name;
		ii.width = s.images[i].width, ii.height = s.images[i].height;
		ii.out_width = pow2_at_most(std::max(1, ii.width), kMaxTexture), ii.out_height = pow2_at_most(std::max(1, ii.height), kMaxTexture);
		plan.images.push_back(ii);
	}
	revalidate(plan);
	return plan;
}

void revalidate(ImportPlan &plan, const BaseData *base) {
	plan.issues.clear();
	const gltf::Scene &s = plan.scene;
	auto add = [&](Issue::Severity sev, const std::string &t) { plan.issues.push_back({sev, 0, -1, t, 0}); };
	for (const std::string &w : s.warnings) add(Issue::Warning, w);
	std::set<int> used_images;
	std::set<int> used_materials;
	for (const auto &it : plan.items) {
		if (it.role == Role::Ignore) continue;
		const gltf::Node &n = s.nodes[size_t(it.node)];
		if (n.mesh >= 0 && (it.role == Role::Visual || plan.settings.kind == ImportKind::Prop)) {
			for (const auto &p : s.meshes[size_t(n.mesh)].primitives) {
				used_materials.insert(p.material);
				if (p.material >= 0) {
					const auto &m = s.materials[size_t(p.material)];
					if (m.base_texture) used_images.insert(m.base_texture.image);
					MaterialSpec ms = material_spec(s, p.material);
					for (auto &st : ms.stages) {
						if (st.image >= 0) used_images.insert(st.image);
						if (st.image < 0 && st.cls == "TextureManager" && m.base_texture.image < 0) {
						}
						bool has_uv = st.cls != "TextureManager" || st.uv_set < int(p.uvs.size());
						if (!has_uv && st.image >= 0) add(Issue::Warning, it.name + ": material " + m.name + " uses TEXCOORD_" + std::to_string(st.uv_set) + ", which the mesh lacks");
					}
				}
			}
			if (it.vertices > kMaxMeshVertices || it.triangles > kMaxMeshFaces) {
				add(Issue::Warning, it.name + ": " + std::to_string(it.vertices) + " vertices / " + std::to_string(it.triangles) +
				                        " triangles is past a mesh's 65,535 (u16 indices); it is split into several meshes");
			}
		}
		if (it.name.size() > 120) add(Issue::Warning, it.name + ": long names are shortened");
	}
	for (auto &im : plan.images) {
		im.used = used_images.count(im.image) > 0;
		const gltf::Image &img = s.images[size_t(im.image)];
		if (!im.used) continue;
		if (!img.error.empty()) add(Issue::Error, "Image " + im.name + ": " + img.error);
		else if (im.out_width != im.width || im.out_height != im.height)
			add(Issue::Warning, "Image " + im.name + " (" + std::to_string(im.width) + "x" + std::to_string(im.height) + ") is stored as " + std::to_string(im.out_width) + "x" +
			                        std::to_string(im.out_height) + " (power-of-two sides up to 1024, as the game's textures)");
	}
	size_t visual = plan.count(Role::Visual);
	if (plan.settings.kind == ImportKind::Prop) {
		if (!visual) add(Issue::Error, "Nothing to import: the file has no meshes");
		if (!s.skins.empty()) add(Issue::Warning, "The file has a skin: props are imported unskinned (robots come with the HAR importer)");
		if (!s.animations.empty()) add(Issue::Warning, std::to_string(s.animations.size()) + " animation(s) are not imported for props");
	} else {
		size_t floors = plan.count(Role::Floor), spawns = plan.count(Role::Spawn);
		if (!floors) add(Issue::Error, "No walkable collision: put the floor meshes in a Collision collection (material \"floor\")");
		if (spawns < 2) add(Issue::Error, "An arena needs at least 2 spawn points (empties in a Spawn collection); this file has " + std::to_string(spawns));
		else if (spawns > 16) add(Issue::Warning, std::to_string(spawns) + " spawn points: the game's sessions take at most 16");
		if (!visual) add(Issue::Warning, "No Visual meshes: the arena will show only the template's sky");
		for (const auto &it : plan.items) {
			if (it.role == Role::Pickup && it.pickups.empty()) add(Issue::Warning, "Pickup spawner " + it.name + " names no pickups (extras.pickups); it spawns energy boosts");
			if (it.role == Role::Pickup && base) {
				auto names = pickup_names(*base);
				for (auto &pk : it.pickups) {
					if (std::find(names.begin(), names.end(), pk) == names.end()) add(Issue::Error, "Pickup spawner " + it.name + ": no pickup named " + pk + " in the game");
				}
			}
		}
		if (plan.count(Role::Death) + plan.count(Role::Kill) == 0) add(Issue::Warning, "No death volume: fighters knocked off the arena fall forever");
	}
	if (plan.issues.empty() || !plan.errors()) add(Issue::Ok, "Ready to import");
}

std::vector<std::string> pickup_names(const BaseData &base) {
	static std::map<const BaseData *, std::vector<std::string>> cache;
	if (auto it = cache.find(&base); it != cache.end()) return it->second;
	std::set<std::string> names;
	for (const BaseObject *o : base.of_class("Value\\ObjectReference")) {
		if (o->name != "PickupsToSpawn") continue;
		const deo::File &f = base.model(o->file);
		const deo::Object &obj = f.objects[o->index];
		for (const Ref &r : object_references(obj.info.class_text(), obj.references)) {
			Guid g = ref_guid(f, r);
			if (const BaseObject *t = base.by_guid(g)) names.insert(t->name);
		}
	}
	std::vector<std::string> out(names.begin(), names.end());
	cache[&base] = out;
	return out;
}

namespace {

const BaseObject *find_pickup(const BaseData &base, const std::string &name) {
	for (const BaseObject *o : base.of_class("Value\\ObjectReference")) {
		if (o->name != "PickupsToSpawn") continue;
		const deo::File &f = base.model(o->file);
		const deo::Object &obj = f.objects[o->index];
		for (const Ref &r : object_references(obj.info.class_text(), obj.references)) {
			if (const BaseObject *t = base.by_guid(ref_guid(f, r)); t && t->name == name) return t;
		}
	}
	return nullptr;
}

std::vector<Piece> visual_pieces(const ImportPlan &plan, const std::function<bool(const ImportItem &)> &want) {
	std::vector<Piece> out;
	for (const auto &it : plan.items) {
		if (!want(it)) continue;
		gltf::Mat4 m = gltf::multiply(up_fix(plan.settings), plan.scene.world(it.node));
		for (auto &pc : pieces_of(plan.scene, it.node, m)) out.push_back(pc);
	}
	return out;
}

}  // namespace

ImportResult import_prop(Project &p, const ImportPlan &plan) {
	if (plan.errors()) throw deo::Error("the import has errors; see the list");
	ImportResult res;
	const std::string name = plan.settings.name.empty() ? "Prop" : plan.settings.name;
	Writer w{p, plan.scene, res, {}, UINT32_MAX, name};
	uint32_t folder = add_folder(p, name);
	res.made.push_back(folder);
	w.maps_folder = add_folder(p, name + " Maps");
	res.made.push_back(w.maps_folder);
	add_child(p, folder, w.maps_folder);
	auto pieces = visual_pieces(plan, [](const ImportItem &it) { return it.role == Role::Visual; });
	const gltf::Mesh *source = nullptr;
	if (plan.from_editor && plan.items.size() == 1 && plan.scene.nodes[size_t(plan.items[0].node)].mesh >= 0) source = &plan.scene.meshes[size_t(plan.scene.nodes[size_t(plan.items[0].node)].mesh)];
	auto built = build_meshes(pieces, plan.settings.units_per_metre, source);
	if (built.empty()) throw deo::Error("no triangles to import");
	uint32_t flags = 0x1e;
	int32_t placement = 0;
	if (source && source->extras.is_object()) {
		flags = uint32_t(num(source->extras.find("de_mesh_flags"), float(flags)));
		placement = int32_t(num(source->extras.find("de_placement"), 0));
	}
	std::vector<uint32_t> meshes;
	for (size_t k = 0; k < built.size(); ++k) {
		std::string mname = (source ? source->name : name + " Mesh") + (built.size() > 1 ? " " + std::to_string(k + 1) : "");
		uint32_t m = w.mesh(built[k], mname, flags, placement);
		add_child(p, folder, m);
		meshes.push_back(m);
	}
	// The prefab: its meshes are components only, so each placed copy shares them.
	deo::Writer r;
	r.u8(2), r.u8(0);
	size_t at = r.begin_chunk(3);
	std::vector<Ref> comps;
	for (uint32_t m : meshes) comps.push_back(local_ref(m));
	r.bytes(ref_list(comps));
	r.end_chunk(at);
	uint32_t actor = add_object(p, "Actor", name, actor_contents(name, de::Transform{}), r.data());
	add_child(p, folder, actor);
	res.made.push_back(actor);
	res.root = actor;
	p.touch();
	return res;
}

std::vector<uint32_t> prefabs(const Project &p) {
	std::vector<uint32_t> out;
	const auto &objs = p.file().objects;
	std::vector<int> parent(objs.size(), -1);
	for (uint32_t i = 0; i < objs.size(); ++i) {
		for (uint32_t c : hierarchy_children(objs[i])) {
			if (c < objs.size()) parent[c] = int(i);
		}
	}
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].info.class_text() != "Actor" || parent[i] < 0 || objs[size_t(parent[i])].info.class_text() != "Folder") continue;
		if (!hierarchy_children(objs[i]).empty()) continue;
		bool mesh = false;
		Chunks rc = split_chunks(objs[i].references);
		if (rc.chunks.size() > 1) {
			try {
				deo::Reader r(rc.chunks[1].body);
				while (r.remaining() && r.u8() == 1) {
					Ref ref = read_ref(r);
					if (ref.kind == Ref::Local && ref.index < objs.size() && objs[ref.index].info.class_text() == "Mesh\\Standard") mesh = true;
				}
			} catch (const deo::Error &) {
			}
		}
		if (mesh) out.push_back(i);
	}
	return out;
}

uint32_t place_prefab(Project &p, uint32_t prefab, uint32_t parent, const std::array<float, 9> &rotation, const std::array<float, 3> &position) {
	deo::Object o = p.file().objects.at(prefab);
	o.info.guid = fresh_guid();
	o.hierarchy = {1, 1, 0};
	p.file().objects.push_back(std::move(o));
	uint32_t made = uint32_t(p.file().objects.size() - 1);
	de::Transform t;
	t.r = rotation, t.t = position;
	set_actor_local(p, made, t);
	add_child(p, parent, made);
	p.touch();
	return made;
}

std::filesystem::path sidecar_dir(const Project &p) {
	auto path = p.path();
	return path.parent_path() / (path.stem().string() + ".assets");
}

std::filesystem::path keep_source(const Project &p, const std::filesystem::path &source, const std::string &name) {
	namespace fs = std::filesystem;
	std::string safe = name;
	for (auto &c : safe) {
		if (strchr("\\/:*?\"<>|", c)) c = '_';
	}
	fs::path dir = sidecar_dir(p) / "models" / safe;
	fs::create_directories(dir);
	fs::path dest = dir / source.filename();
	std::error_code ec;
	if (fs::exists(dest, ec) && fs::equivalent(dest, source, ec)) return dest;
	fs::copy_file(source, dest, fs::copy_options::overwrite_existing);
	std::string ext = lower(source.extension().string());
	if (ext == ".gltf") {
		// Its buffers and images, by the URIs it names.
		std::ifstream f(source, std::ios::binary);
		std::string text{std::istreambuf_iterator<char>(f), {}};
		try {
			json::Value root = json::Value::parse(text);
			for (const char *list : {"buffers", "images"}) {
				const json::Value *arr = root.find(list);
				if (!arr || !arr->is_array()) continue;
				for (auto &b : arr->items()) {
					const json::Value *uri = b.find("uri");
					if (!uri || !uri->is_string() || uri->as_string().rfind("data:", 0) == 0) continue;
					fs::path from = source.parent_path() / uri->as_string();
					fs::path to = dir / uri->as_string();
					fs::create_directories(to.parent_path());
					if (fs::exists(from, ec)) fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
				}
			}
		} catch (const std::exception &) {
		}
	}
	return dest;
}

// ---------------------------------------------------------------------------------------------
// Arenas

namespace {

const char *const kSpawnTemplateFile = "Grudge Match Arena";
const char *const kGameplayTemplateFile = "Desert";

int base_object(const BaseData &base, const std::string &stem, const std::string &cls, const std::string &name) {
	const auto *fi = base.file(stem);
	if (!fi) return -1;
	for (const BaseObject &o : fi->objects) {
		if (o.class_path == cls && o.name == name) return int(o.index);
	}
	return -1;
}

size_t file_index(const BaseData &base, const std::string &stem) {
	const auto *fi = base.file(stem);
	if (!fi) throw deo::Error("the base data has no " + stem + ".DEObjects (needed for its arena templates)");
	return size_t(fi - base.files().data());
}

// Copies a base subtree, keeping only the children `keep` accepts (applied at every level).
uint32_t copy_pruned(Project &p, const std::string &stem, uint32_t root, const std::function<bool(const deo::File &, uint32_t parent, uint32_t child)> &keep) {
	const BaseData &base = p.base();
	deo::File src = base.model(file_index(base, stem));
	std::vector<uint32_t> stack{root};
	std::set<uint32_t> seen;
	while (!stack.empty()) {
		uint32_t o = stack.back();
		stack.pop_back();
		if (!seen.insert(o).second) continue;
		auto kids = hierarchy_children(src.objects[o]);
		std::vector<uint32_t> kept;
		for (uint32_t c : kids) {
			if (keep(src, o, c)) kept.push_back(c), stack.push_back(c);
		}
		if (kept.size() != kids.size()) {
			Chunks h = split_chunks(src.objects[o].hierarchy);
			std::vector<Ref> refs;
			for (uint32_t k : kept) refs.push_back(local_ref(k));
			if (h.chunks.empty()) h.chunks.push_back(RawChunk{});
			h.chunks[0].body = ref_list(refs);
			src.objects[o].hierarchy = join_chunks(h);
		}
	}
	return copy_tree(p, src, false, {root})[0];
}

void set_sphere(Project &p, uint32_t o, float radius) { set_contents_chunk(p, o, 2, floats({0, radius})); }

void set_box(Project &p, uint32_t o, const V3 &lo, const V3 &hi) { set_contents_chunk(p, o, 2, floats({lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]})); }

// A mesh affector's references chunk 3 (v2 { v1 { BOOL, mesh } }) names `mesh`; its contents
// chunk 2 drops the stored octree of the old mesh and keeps a cell size, so the game builds one
// for the new faces (DE_CMeshAffector::AssureOctree, as Canyon's GunTriggerCollision is stored).
void set_affector_mesh(Project &p, uint32_t affector, uint32_t mesh, float cell) {
	deo::Object &o = p.file().objects[affector];
	Chunks rc = split_chunks(o.references);
	if (rc.chunks.size() < 4) throw deo::Error(o.info.name_text() + ": unexpected mesh affector references");
	deo::Writer inner;
	size_t at = inner.begin_chunk(1);
	inner.u8(1);
	write_ref(inner, local_ref(mesh));
	inner.end_chunk(at);
	rc.chunks[3].version = 2;
	rc.chunks[3].size = {};
	rc.chunks[3].body = inner.data();
	o.references = join_chunks(rc);
	deo::Writer c;
	c.u8(0);
	put_f32(c, cell);
	set_contents_chunk(p, affector, 2, c.data());
}

void set_object_references(Project &p, uint32_t value, const std::vector<Guid> &targets) {
	deo::Object &o = p.file().objects[value];
	Chunks rc = split_chunks(o.references);
	if (rc.chunks.size() < 3) throw deo::Error("unexpected Value\\ObjectReference layout");
	std::vector<Ref> refs;
	for (const Guid &g : targets) {
		Ref r;
		r.kind = Ref::Pool, r.index = p.pool_index(g);
		refs.push_back(r);
	}
	rc.chunks[2].body = ref_list(refs);
	rc.chunks[2].size = {};
	o.references = join_chunks(rc);
}

int child_named(const Project &p, uint32_t parent, const std::string &name) {
	for (uint32_t c : hierarchy_children(p.file().objects[parent])) {
		if (p.file().objects[c].info.name_text() == name) return int(c);
	}
	return -1;
}

de::Transform look_along(const V3 &up_dir, const V3 &pos) {
	V3 y = up_dir;
	float l = length(y);
	y = l > 0 ? V3{y[0] / l, y[1] / l, y[2] / l} : V3{0, 1, 0};
	V3 ref = std::fabs(y[1]) < 0.9f ? V3{0, 1, 0} : V3{0, 0, 1};
	V3 x = cross(y, ref);
	x = {x[0] / length(x), x[1] / length(x), x[2] / length(x)};
	V3 z = cross(x, y);
	de::Transform t;
	for (int k = 0; k < 3; ++k) t.r[size_t(k)] = x[size_t(k)], t.r[size_t(3 + k)] = y[size_t(k)], t.r[size_t(6 + k)] = z[size_t(k)];
	t.t = pos;
	return t;
}

bool segment_hits(const std::vector<V3> &tris, const V3 &p, const V3 &q) {
	V3 d = sub(q, p);
	for (size_t t = 0; t + 2 < tris.size(); t += 3) {
		V3 e1 = sub(tris[t + 1], tris[t]), e2 = sub(tris[t + 2], tris[t]);
		V3 h = cross(d, e2);
		float a = dot(e1, h);
		if (std::fabs(a) < 1e-9f) continue;
		float f = 1 / a;
		V3 s = sub(p, tris[t]);
		float u = f * dot(s, h);
		if (u < 0 || u > 1) continue;
		V3 qv = cross(s, e1);
		float v = f * dot(d, qv);
		if (v < 0 || u + v > 1) continue;
		float tt = f * dot(e2, qv);
		if (tt > 0 && tt < 1) return true;
	}
	return false;
}

struct NavNet {
	struct Area {
		V3 at;
		float radius;
	};
	std::vector<Area> areas;
	std::vector<std::pair<int, int>> links;
};

// A grid over the walkable faces (normal within 45 degrees of up), an area where a cell's centre
// lands on one, linked to its neighbours when a robot (~30 units tall) can walk there: no wall in
// the way, no step over 12 units, no hole; coarser until it fits kMaxNavAreas / kMaxNavLinks.
NavNet generate_nav(const std::vector<V3> &floor, const std::vector<V3> &solid, float spacing) {
	NavNet net;
	if (floor.empty()) return net;
	V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	for (auto &v : floor) {
		for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], v[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], v[size_t(k)]);
	}
	float area = 0;
	for (size_t t = 0; t + 2 < floor.size(); t += 3) {
		V3 n = cross(sub(floor[t + 1], floor[t]), sub(floor[t + 2], floor[t]));
		area += std::fabs(n[1]) * 0.5f;
	}
	if (spacing <= 0) spacing = std::max(150.0f, std::sqrt(area / 40.0f));
	auto heights_at = [&](float x, float z) {
		std::vector<float> heights;
		for (size_t t = 0; t + 2 < floor.size(); t += 3) {
			const V3 &a = floor[t], &b = floor[t + 1], &c = floor[t + 2];
			float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
			if (std::fabs(d) < 1e-9f) continue;
			float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
			float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
			float l3 = 1 - l1 - l2;
			if (l1 < 0 || l2 < 0 || l3 < 0) continue;
			heights.push_back(l1 * a[1] + l2 * b[1] + l3 * c[1]);
		}
		std::sort(heights.begin(), heights.end());
		return heights;
	};
	for (int attempt = 0; attempt < 12; ++attempt, spacing *= 1.2f) {
		net = {};
		for (float x = lo[0] + spacing * 0.5f; x <= hi[0]; x += spacing) {
			for (float z = lo[2] + spacing * 0.5f; z <= hi[2]; z += spacing) {
				std::vector<float> heights = heights_at(x, z);
				float last = -1e30f;
				for (float y : heights) {
					if (y - last < 40) continue;
					net.areas.push_back({{x, y + 15, z}, spacing * 0.75f});
					last = y;
				}
			}
		}
		for (size_t a = 0; a < net.areas.size(); ++a) {
			for (size_t b = 0; b < net.areas.size(); ++b) {
				if (a == b) continue;
				const V3 &A = net.areas[a].at, &B = net.areas[b].at;
				float dx = B[0] - A[0], dz = B[2] - A[2];
				float d = std::sqrt(dx * dx + dz * dz);
				if (d > spacing * 1.5f) continue;
				bool clear = true;
				for (float h : {5.0f, 15.0f}) {
					V3 p{A[0], A[1] - 15 + h, A[2]}, q{B[0], B[1] - 15 + h, B[2]};
					clear = clear && !segment_hits(solid, p, q);
				}
				if (!clear) continue;
				// Walkable: the ground along the way never rises more than a step, never opens into a
				// hole, and ends on B's floor.
				float ground = A[1] - 15;
				int n = std::max(2, int(d / 8));
				for (int k = 1; k <= n && clear; ++k) {
					float u = float(k) / float(n);
					float best = -1e30f;
					for (float y : heights_at(A[0] + dx * u, A[2] + dz * u)) {
						if (y <= ground + 12 && y >= ground - 60) best = std::max(best, y);
					}
					clear = best > -1e29f;
					ground = best;
				}
				if (!clear || std::fabs(ground - (B[1] - 15)) > 12) continue;
				net.links.push_back({int(a), int(b)});
			}
		}
		// Areas no link reaches are dropped.
		std::vector<int> keep(net.areas.size(), 0);
		for (auto &[a, b] : net.links) keep[size_t(a)] = keep[size_t(b)] = 1;
		std::vector<int> remap(net.areas.size(), -1);
		std::vector<NavNet::Area> kept;
		for (size_t a = 0; a < net.areas.size(); ++a) {
			if (keep[a]) remap[a] = int(kept.size()), kept.push_back(net.areas[a]);
		}
		net.areas = kept;
		for (auto &[a, b] : net.links) a = remap[size_t(a)], b = remap[size_t(b)];
		if (net.areas.size() <= kMaxNavAreas && net.links.size() <= kMaxNavLinks) return net;
	}
	return net;
}

}  // namespace

ImportResult import_arena(Project &p, const ImportPlan &plan) {
	if (plan.errors()) throw deo::Error("the import has errors; see the list");
	const BaseData &base = p.base();
	const ImportSettings &st = plan.settings;
	const gltf::Scene &s = plan.scene;
	const float units = st.units_per_metre;
	ImportResult res;
	const std::string name = st.name.empty() ? "Imported Arena" : st.name;

	// The template arena: description and environment, keeping its lights, cameras, values, sounds
	// and (optionally) sky; its geometry and gameplay objects are left behind.
	auto arenas = base_arenas(base);
	const ArenaEntry *tmpl = nullptr;
	for (auto &a : arenas) {
		if (lower(a.name) == lower(st.template_arena) || lower(a.stem) == lower(st.template_arena)) tmpl = &a;
	}
	for (auto &a : arenas) {
		if (!tmpl && a.listed && lower(a.stem).find(lower(st.template_arena)) != std::string::npos) tmpl = &a;
	}
	if (!tmpl) throw deo::Error("no base arena " + st.template_arena);
	const size_t env_file = size_t(tmpl->environment_file);
	auto keep_env_child = [&](const deo::File &f, uint32_t parent, uint32_t c) {
		const deo::Object &o = f.objects[c];
		const std::string cls = o.info.class_text(), nm = lower(o.info.name_text());
		if (f.objects[parent].info.guid != tmpl->environment) return true;
		if (cls == "Light" || cls.rfind("Value", 0) == 0 || cls == "ScaleModifier" || cls.rfind("Image", 0) == 0 || cls == "Camera") return true;
		if (nm.find("visib") != std::string::npos || nm.find("observerteleport") != std::string::npos || nm.find("environment map camera") != std::string::npos) return true;
		if (st.keep_sky && (nm.rfind("sky", 0) == 0 || nm.find("cloud") != std::string::npos) && cls == "Actor") return true;
		if (cls == "Actor\\Affector\\Sphere" || cls == "Actor\\Affector\\Cone") {
			for (uint32_t k : hierarchy_children(o)) {
				const std::string kc = f.objects[k].info.class_text();
				if (kc == "Light" || kc == "UniqueActorProperty\\Speaker") return true;
			}
		}
		return false;
	};
	std::map<uint32_t, uint32_t> copied;
	(void)copied;
	const BaseObject *tdesc = base.by_guid(tmpl->description);
	const BaseObject *tenv = base.by_guid(tmpl->environment);
	if (!tdesc || !tenv) throw deo::Error("the template arena's description or environment is missing");
	uint32_t env = copy_pruned(p, base.files()[env_file].stem, tenv->index, keep_env_child);
	res.made.push_back(env);
	rename(p, env, name);
	uint32_t desc = copy_tree(p, base.model(tdesc->file), false, {tdesc->index})[0];
	res.made.push_back(desc);
	{
		deo::Object &d = p.file().objects[desc];
		Bytes from(base.model(env_file).master.guid.b, base.model(env_file).master.guid.b + 16), to(p.file().master.guid.b, p.file().master.guid.b + 16);
		from.insert(from.end(), tmpl->environment.b, tmpl->environment.b + 16);
		const Guid &ng = p.file().objects[env].info.guid;
		to.insert(to.end(), ng.b, ng.b + 16);
		for (size_t i = 0; i + from.size() <= d.contents.size(); ++i) {
			if (std::equal(from.begin(), from.end(), d.contents.begin() + long(i))) std::copy(to.begin(), to.end(), d.contents.begin() + long(i));
		}
		if (d.compressed()) d.packed.clear(), d.packed_as_is = false;
	}
	res.root = desc;

	// Visual geometry: one actor per node, baked in world space.
	Writer w{p, s, res, {}, UINT32_MAX, name};
	uint32_t maps = add_folder(p, name + " Maps");
	res.made.push_back(maps);
	w.maps_folder = maps;
	uint32_t geometry = add_actor(p, name + " Geometry", de::Transform{}, {});
	res.made.push_back(geometry);
	add_child(p, env, geometry);
	for (const auto &it : plan.items) {
		if (it.role != Role::Visual) continue;
		gltf::Mat4 m = gltf::multiply(up_fix(st), s.world(it.node));
		auto built = build_meshes(pieces_of(s, it.node, m), units, nullptr);
		for (size_t k = 0; k < built.size(); ++k) {
			std::string mname = it.name + (built.size() > 1 ? " " + std::to_string(k + 1) : "") + " Mesh";
			uint32_t mesh = w.mesh(built[k], mname, 0x1e);
			uint32_t a = add_actor(p, it.name + (built.size() > 1 ? " " + std::to_string(k + 1) : ""), de::Transform{}, {mesh});
			res.made.push_back(a);
			add_child(p, geometry, a);
		}
	}

	// Collision: floors and walls in one fighting-surface mesh affector.
	std::vector<V3> solid, floor, kill;
	for (const auto &it : plan.items) {
		if (it.role != Role::Floor && it.role != Role::Wall && it.role != Role::Kill) continue;
		gltf::Mat4 m = gltf::multiply(up_fix(st), s.world(it.node));
		auto by_material = [&](Role want) {
			return triangles_with_material(s, it.node, m, units, [&](int mat) {
				Role r = mat >= 0 ? surface_role(s.materials[size_t(mat)].name) : it.role;
				if (it.role == Role::Wall && r == Role::Floor) r = Role::Wall;
				if (it.role == Role::Kill) r = Role::Kill;
				return r == want;
			});
		};
		auto f = by_material(Role::Floor), wl = by_material(Role::Wall), k = by_material(Role::Kill);
		floor.insert(floor.end(), f.begin(), f.end());
		solid.insert(solid.end(), f.begin(), f.end());
		solid.insert(solid.end(), wl.begin(), wl.end());
		kill.insert(kill.end(), k.begin(), k.end());
	}
	const size_t dfile = file_index(base, kGameplayTemplateFile);
	(void)dfile;
	int coll_src = base_object(base, kGameplayTemplateFile, "Actor\\Affector\\Mesh", "FightingSurfaceCollisionAffector");
	if (coll_src < 0) throw deo::Error("Desert's FightingSurfaceCollisionAffector is missing (the collision template)");
	{
		uint32_t cm = w.collision_mesh(solid, "FightingSurfaceCollision Mesh");
		uint32_t ca = copy_pruned(p, kGameplayTemplateFile, uint32_t(coll_src), [](const deo::File &, uint32_t, uint32_t) { return false; });
		res.made.push_back(ca);
		rename(p, ca, "FightingSurfaceCollisionAffector");
		set_actor_local(p, ca, de::Transform{});
		set_affector_mesh(p, ca, cm, 200);
		add_child(p, env, ca);
		add_child(p, maps, cm);
		if (solid.size() / 3 > kMaxMeshFaces) res.notes.push_back("collision past 65,535 faces was cut");
	}

	// Death volumes: a box affector (Grudge Match's DeathVolume, with its executor and material)
	// over each Death mesh and the kill surfaces.
	int death_src = base_object(base, kSpawnTemplateFile, "Actor\\Affector\\Box", "DeathVolume");
	std::vector<std::pair<std::string, std::vector<V3>>> death_sets;
	for (const auto &it : plan.items) {
		if (it.role == Role::Death) death_sets.push_back({it.name, triangles_of(s, it.node, gltf::multiply(up_fix(st), s.world(it.node)), units)});
	}
	if (!kill.empty()) death_sets.push_back({"KillSurface", kill});
	int dv_n = 0;
	for (auto &[dname, tris] : death_sets) {
		if (tris.empty() || death_src < 0) continue;
		V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
		for (auto &v : tris) {
			for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], v[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], v[size_t(k)]);
		}
		V3 c{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
		uint32_t dv = copy_pruned(p, kSpawnTemplateFile, uint32_t(death_src), [](const deo::File &, uint32_t, uint32_t) { return true; });
		res.made.push_back(dv);
		rename(p, dv, dv_n++ ? "DeathVolume" + std::to_string(dv_n) : std::string("DeathVolume"));
		de::Transform t;
		t.t = c;
		set_actor_local(p, dv, t);
		set_box(p, dv, {lo[0] - c[0], lo[1] - c[1], lo[2] - c[2]}, {hi[0] - c[0], hi[1] - c[1], hi[2] - c[2]});
		add_child(p, env, dv);
	}

	// Jump pads: Desert's JumpPad1 (launcher executor and material, effects), turned so its +Y is
	// the launch direction, its collision shape the pad mesh.
	int pad_src = base_object(base, kGameplayTemplateFile, "Actor\\Affector\\Mesh", "JumpPad1");
	int pad_n = 0;
	for (const auto &it : plan.items) {
		if (it.role != Role::JumpPad || pad_src < 0) continue;
		auto tris = triangles_of(s, it.node, gltf::multiply(up_fix(st), s.world(it.node)), units);
		if (tris.empty()) continue;
		V3 c{0, 0, 0};
		for (auto &v : tris) c = {c[0] + v[0], c[1] + v[1], c[2] + v[2]};
		c = {c[0] / float(tris.size()), c[1] / float(tris.size()), c[2] / float(tris.size())};
		gltf::Vec3 dir = gltf::transform_direction(up_fix(st), {it.impulse[0], it.impulse[1], it.impulse[2]});
		de::Transform t = look_along(from_gltf(dir, 1), c);
		de::Transform inv = t.inverse();
		std::vector<V3> local;
		for (auto &v : tris) local.push_back(inv.apply(v));
		uint32_t pad = copy_pruned(p, kGameplayTemplateFile, uint32_t(pad_src), [](const deo::File &f, uint32_t, uint32_t c) {
			return f.objects[c].info.class_text() != "Mesh\\Standard";
		});
		res.made.push_back(pad);
		rename(p, pad, "JumpPad" + std::to_string(++pad_n));
		set_actor_local(p, pad, t);
		uint32_t pm = w.collision_mesh(local, "JumpPad" + std::to_string(pad_n) + " Mesh");
		add_child(p, pad, pm);
		set_affector_mesh(p, pad, pm, 30);
		add_child(p, env, pad);
	}

	// Spawn points: Grudge Match's PlayerSpawnPoint01 (a passive reference to the shared spawn
	// point), named in order.
	int spawn_src = base_object(base, kSpawnTemplateFile, "Actor\\PassiveReference", "PlayerSpawnPoint01");
	if (spawn_src < 0) throw deo::Error("the spawn point template is missing");
	uint32_t spawn_parent = add_actor(p, "SpawnParent", de::Transform{}, {});
	res.made.push_back(spawn_parent);
	add_child(p, env, spawn_parent);
	std::vector<const ImportItem *> spawns;
	for (const auto &it : plan.items) {
		if (it.role == Role::Spawn) spawns.push_back(&it);
	}
	std::sort(spawns.begin(), spawns.end(), [](auto *a, auto *b) { return a->name < b->name; });
	for (const ImportItem *it : spawns) {
		uint32_t sp = copy_pruned(p, kSpawnTemplateFile, uint32_t(spawn_src), [](const deo::File &, uint32_t, uint32_t) { return true; });
		char nm[32];
		snprintf(nm, sizeof nm, "PlayerSpawnPoint%02d", int(++res.spawns));
		rename(p, sp, nm);
		de::Transform t = node_transform(s, it->node, st);
		set_actor_local(p, sp, t);
		add_child(p, spawn_parent, sp);
		res.made.push_back(sp);
	}

	// Pickup spawners: Grudge Match's EnergyPickupSpawner01 with PickupsToSpawn set.
	int pick_src = base_object(base, kSpawnTemplateFile, "Actor\\PassiveReference", "EnergyPickupSpawner01");
	for (const auto &it : plan.items) {
		if (it.role != Role::Pickup || pick_src < 0) continue;
		uint32_t pk = copy_pruned(p, kSpawnTemplateFile, uint32_t(pick_src), [](const deo::File &, uint32_t, uint32_t) { return true; });
		rename(p, pk, it.name);
		set_actor_local(p, pk, node_transform(s, it.node, st));
		std::vector<Guid> targets;
		for (const std::string &n : it.pickups) {
			if (const BaseObject *b = find_pickup(base, n)) targets.push_back(b->guid);
			else res.notes.push_back("no pickup named " + n);
		}
		int list = child_named(p, pk, "PickupsToSpawn");
		if (list >= 0 && !targets.empty()) set_object_references(p, uint32_t(list), targets);
		add_child(p, env, pk);
		res.made.push_back(pk);
		++res.pickups;
	}

	// The navigation net: Desert's NavParent, one area (sphere, group 0x2000) and one link as templates.
	if (st.generate_nav) {
		NavNet net = generate_nav(floor, solid, st.nav_spacing);
		int nav_src = base_object(base, kGameplayTemplateFile, "Actor\\Affector\\Sphere", "NavParent");
		if (nav_src >= 0 && !net.areas.empty()) {
			bool area_kept = false, link_kept = false;
			uint32_t nav = copy_pruned(p, kGameplayTemplateFile, uint32_t(nav_src), [&](const deo::File &f, uint32_t parent, uint32_t c) {
				const std::string cls = f.objects[c].info.class_text();
				if (f.objects[parent].info.name_text() == "NavParent") {
					if (area_kept || cls != "Actor\\Affector\\Sphere") return false;
					return area_kept = true;
				}
				if (link_kept || cls != "UniqueActorProperty\\NavigationLink") return false;
				return link_kept = true;
			});
			res.made.push_back(nav);
			V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
			for (auto &a : net.areas) {
				for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], a.at[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], a.at[size_t(k)]);
			}
			de::Transform nt;
			nt.t = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
			set_actor_local(p, nav, nt);
			float r = length(sub(hi, lo)) / 2 + net.areas[0].radius;
			set_sphere(p, nav, r);
			add_child(p, env, nav);
			auto kids = hierarchy_children(p.file().objects[nav]);
			if (kids.empty()) throw deo::Error("the navigation template has no area");
			uint32_t area0 = kids[0];
			auto lk = hierarchy_children(p.file().objects[area0]);
			if (lk.empty()) throw deo::Error("the navigation template has no link");
			uint32_t link0 = lk[0];
			set_children(p, area0, {});
			std::vector<uint32_t> areas;
			for (size_t a = 0; a < net.areas.size(); ++a) {
				uint32_t ao = a == 0 ? area0 : duplicate_actor(p, area0, "Nav" + std::to_string(a));
				if (a == 0) rename(p, ao, "Nav0");
				de::Transform t;
				t.t = sub(net.areas[a].at, nt.t);
				set_actor_local(p, ao, t);
				set_sphere(p, ao, net.areas[a].radius);
				areas.push_back(ao);
				res.made.push_back(ao);
			}
			// Links: copies of the template link, references chunk 2 { BOOL, destination area }.
			deo::Object link_proto = p.file().objects[link0];
			bool first = true;
			for (auto &[a, b] : net.links) {
				uint32_t lo_i;
				if (first) {
					lo_i = link0;
					first = false;
				} else {
					deo::Object o = link_proto;
					o.info.guid = fresh_guid();
					p.file().objects.push_back(o);
					lo_i = uint32_t(p.file().objects.size() - 1);
				}
				rename(p, lo_i, "Nav" + std::to_string(a) + "WalkToNav" + std::to_string(b));
				deo::Object &lo_o = p.file().objects[lo_i];
				Chunks rc = split_chunks(lo_o.references);
				if (rc.chunks.size() < 3) throw deo::Error("unexpected navigation link layout");
				bool list_form = false;
				try {
					deo::Reader rr(rc.chunks[2].body);
					if (rr.u8() == 1) read_ref(rr);
					list_form = rr.remaining() > 0;
				} catch (const deo::Error &) {
				}
				deo::Writer lw;
				lw.u8(1);
				write_ref(lw, local_ref(areas[size_t(b)]));
				if (list_form) lw.u8(0);
				rc.chunks[2].body = lw.data();
				rc.chunks[2].size = {};
				lo_o.references = join_chunks(rc);
				add_child(p, areas[size_t(a)], lo_i);
				res.made.push_back(lo_i);
			}
			if (first) detach_object(p, link0);
			res.nav_areas = net.areas.size(), res.nav_links = net.links.size();
		}
	}

	// The visibility sphere and the description's player counts follow the new arena.
	V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	for (auto &v : solid) {
		for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], v[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], v[size_t(k)]);
	}
	if (!solid.empty()) {
		for (uint32_t c : hierarchy_children(p.file().objects[env])) {
			const deo::Object &o = p.file().objects[c];
			if (o.info.class_text() != "Actor\\Affector\\Sphere" || lower(o.info.name_text()).find("visib") == std::string::npos) continue;
			de::Transform t;
			t.t = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
			set_actor_local(p, c, t);
			float r = length(sub(hi, lo)) / 2 + 2000;
			set_sphere(p, c, r);
		}
	}
	ArenaDescriptionData dd = read_arena_description(p, desc);
	dd.name = name, dd.listed = true, dd.locked = false;
	dd.max_players = std::clamp(int(res.spawns), 2, std::max(2, dd.max_players));
	dd.min_players = std::min(dd.min_players, dd.max_players);
	write_arena_description(p, desc, dd);
	p.touch();
	return res;
}

}  // namespace ed
