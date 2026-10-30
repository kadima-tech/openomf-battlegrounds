#include "de/mesh.h"

#include <algorithm>
#include <cmath>

namespace de {

Mesh read_mesh(const ObjectsFile &f, const ObjectInfo &o) {
	auto d = f.object_data(o);
	Mesh m;
	{
		Contents c(d.contents);
		Chunk base = begin_chunk(c);  // DE_CMesh
		auto name = c.block();
		m.name.assign(name.begin(), name.end());
		while (!m.name.empty() && m.name.back() == 0) {
			m.name.pop_back();
		}
		if (base.version >= 3) {
			m.flags = c.u32() & ~0x1000u;
		}
		c.seek(base.end);
		Chunk std = begin_chunk(c);
		if (std.version < 6) {
			throw FormatError("Mesh: inline geometry (version " + std::to_string(std.version) + ") not supported");
		}
	}
	auto ext = f.ex_payload(d, "ObjExtCon");
	if (!ext) {
		return m;
	}
	Contents c(ext->data);
	Chunk ch = begin_chunk(c);
	if (ch.version != 1) {
		throw FormatError("Mesh: unsupported extended contents version " + std::to_string(ch.version));
	}
	m.vertices.resize(c.u16());
	for (auto &v : m.vertices) {
		v = {c.f32(), c.f32(), c.f32()};
	}
	m.faces.resize(c.u16());
	for (auto &face : m.faces) {
		for (int k = 0; k < 3; ++k) {
			face.v[k] = c.u16();
			face.adjacent[k] = c.u16();
		}
		face.flags = c.u32();
		for (auto v : face.v) {
			if (v >= m.vertices.size()) {
				throw FormatError("Mesh: vertex index out of range");
			}
		}
	}
	if (c.pos() != ch.end) {
		throw FormatError("Mesh: extended contents size mismatch");
	}
	return m;
}

namespace {

void finish_node(Octree::Node &n) {
	float d2 = 0;
	for (int k = 0; k < 3; ++k) d2 += (n.hi[k] - n.lo[k]) * (n.hi[k] - n.lo[k]);
	n.radius = std::sqrt(d2) * 0.5f;
}

void read_node(Octree &t, Contents &c, int depth) {
	if (depth > 32) throw FormatError("Octree: too deep");
	size_t at = t.nodes.size();
	t.nodes.emplace_back();
	{
		auto &n = t.nodes[at];
		n.lo = {c.f32(), c.f32(), c.f32()};
		n.hi = {c.f32(), c.f32(), c.f32()};
		finish_node(n);
	}
	for (int k = 0; k < 8; ++k) {
		if (!c.boolean()) continue;
		t.nodes[at].child[size_t(k)] = int32_t(t.nodes.size());
		read_node(t, c, depth + 1);
	}
	if (c.boolean()) {
		auto &n = t.nodes[at];
		n.leaf = true;
		uint32_t count = c.u32();
		if (count > c.remaining() / 4) throw FormatError("Octree: bad leaf");
		n.faces.resize(count);
		for (auto &f : n.faces) f = c.u32();
	}
}

// Every node holds the sorted union of the faces of the leaves under it.
void gather_up(Octree &t, size_t i) {
	auto &faces = t.nodes[i].faces;
	for (int32_t ch : t.nodes[i].child) {
		if (ch < 0) continue;
		gather_up(t, size_t(ch));
		const auto &cf = t.nodes[size_t(ch)].faces;
		faces.insert(faces.end(), cf.begin(), cf.end());
	}
	std::sort(faces.begin(), faces.end());
	faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
}

void split(Octree &t, size_t i, float max_size, int depth) {
	std::array<float, 3> lo = t.nodes[i].lo, hi = t.nodes[i].hi, size, mid;
	for (int k = 0; k < 3; ++k) size[k] = hi[k] - lo[k], mid[k] = (lo[k] + hi[k]) * 0.5f;
	const float longest = std::max({size[0], size[1], size[2]});
	bool cut[3];
	for (int k = 0; k < 3; ++k) cut[k] = longest > 0 && size[k] > max_size && size[k] / longest >= 0.5f;
	if ((!cut[0] && !cut[1] && !cut[2]) || depth > 5) return;
	int n = 0;
	for (int x = 0; x < (cut[0] ? 2 : 1); ++x) {
		for (int y = 0; y < (cut[1] ? 2 : 1); ++y) {
			for (int z = 0; z < (cut[2] ? 2 : 1); ++z) {
				Octree::Node c;
				const int side[3] = {x, y, z};
				for (int k = 0; k < 3; ++k) {
					c.lo[k] = !cut[k] ? lo[k] : side[k] ? mid[k] : lo[k];
					c.hi[k] = !cut[k] ? hi[k] : side[k] ? hi[k] : mid[k];
				}
				finish_node(c);
				t.nodes[i].child[size_t(n++)] = int32_t(t.nodes.size());
				t.nodes.push_back(std::move(c));
			}
		}
	}
	for (int k = 0; k < n; ++k) split(t, size_t(t.nodes[i].child[size_t(k)]), max_size, depth + 1);
}

bool box_touch(const std::array<float, 3> &alo, const std::array<float, 3> &ahi, const std::array<float, 3> &blo, const std::array<float, 3> &bhi) {
	for (int k = 0; k < 3; ++k) {
		if (ahi[k] < blo[k] || bhi[k] < alo[k]) return false;
	}
	return true;
}

void add_sorted(std::vector<uint32_t> &out, const std::vector<uint32_t> &ids) {
	size_t mid = out.size();
	out.insert(out.end(), ids.begin(), ids.end());
	std::inplace_merge(out.begin(), out.begin() + std::ptrdiff_t(mid), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
}

}  // namespace

Octree read_octree(const ObjectsFile &f, const ObjectInfo &o) {
	Octree t;
	try {
		auto d = f.object_data(o);
		auto ex = f.ex_payload(d, "D3DProjectedTargetOctree");
		if (!ex) return t;
		Contents c(ex->data);
		Chunk ch = begin_chunk(c);
		if (ch.version != 2 && ch.version != 3) return t;
		c.block();
		read_node(t, c, 0);
		gather_up(t, 0);
	} catch (const std::exception &) {
		t.nodes.clear();
	}
	return t;
}

Octree build_octree(const Mesh &m, float max_size) {
	Octree t;
	if (m.vertices.empty()) return t;
	Octree::Node root;
	root.lo = root.hi = m.vertices[0];
	for (auto &v : m.vertices) {
		for (int k = 0; k < 3; ++k) root.lo[k] = std::min(root.lo[k], v[k]), root.hi[k] = std::max(root.hi[k], v[k]);
	}
	finish_node(root);
	t.nodes.push_back(root);
	split(t, 0, max_size, 0);
	for (uint32_t fi = 0; fi < m.faces.size(); ++fi) {
		std::array<float, 3> lo = m.vertices[m.faces[fi].v[0]], hi = lo;
		for (uint16_t v : m.faces[fi].v) {
			for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], m.vertices[v][k]), hi[k] = std::max(hi[k], m.vertices[v][k]);
		}
		for (auto &n : t.nodes) {
			bool leaf = true;
			for (int32_t ch : n.child) leaf = leaf && ch < 0;
			if (leaf && box_touch(lo, hi, n.lo, n.hi)) n.faces.push_back(fi);
		}
	}
	for (auto &n : t.nodes) {
		n.leaf = true;
		for (int32_t ch : n.child) n.leaf = n.leaf && ch < 0;
	}
	gather_up(t, 0);
	return t;
}

void Octree::sphere_faces(const std::array<float, 3> &c, float r, std::vector<uint32_t> &out) const {
	if (nodes.empty()) return;
	const std::array<float, 3> lo{c[0] - r, c[1] - r, c[2] - r}, hi{c[0] + r, c[1] + r, c[2] + r};
	std::function<void(size_t)> visit = [&](size_t i) {
		const Node &n = nodes[i];
		if (!box_touch(lo, hi, n.lo, n.hi)) return;
		if (n.leaf || n.radius < r) {
			add_sorted(out, n.faces);
			return;
		}
		for (int32_t ch : n.child) {
			if (ch >= 0) visit(size_t(ch));
		}
	};
	visit(0);
}

void Octree::affector_faces(const std::function<bool(const std::array<float, 3> &, float)> &touches,
                            const std::function<std::array<float, 3>(const std::array<float, 3> &)> &to_world, std::vector<uint32_t> &out) const {
	if (nodes.empty()) return;
	std::function<void(size_t)> visit = [&](size_t i) {
		const Node &n = nodes[i];
		const std::array<float, 3> centre{(n.lo[0] + n.hi[0]) * 0.5f, (n.lo[1] + n.hi[1]) * 0.5f, (n.lo[2] + n.hi[2]) * 0.5f};
		if (!touches(to_world(centre), n.radius)) return;
		if (n.leaf) {
			add_sorted(out, n.faces);
			return;
		}
		for (int32_t ch : n.child) {
			if (ch >= 0) visit(size_t(ch));
		}
	};
	visit(0);
}

}  // namespace de
