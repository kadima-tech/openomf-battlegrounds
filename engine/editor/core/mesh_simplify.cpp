#include "mesh_simplify.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

namespace ed {

namespace {

using D3 = std::array<double, 3>;

D3 d3(const std::array<float, 3> &v) { return {v[0], v[1], v[2]}; }
D3 sub(const D3 &a, const D3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 cross(const D3 &a, const D3 &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const D3 &a, const D3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double length(const D3 &a) { return std::sqrt(dot(a, a)); }

// A symmetric 4x4 quadric: aa ab ac ad bb bc bd cc cd dd.
struct Quadric {
	std::array<double, 10> q{};
	void add_plane(const D3 &n, double d, double w) {
		const double a = n[0], b = n[1], c = n[2];
		const double v[10] = {a * a, a * b, a * c, a * d, b * b, b * c, b * d, c * c, c * d, d * d};
		for (int i = 0; i < 10; ++i) q[size_t(i)] += v[i] * w;
	}
	Quadric operator+(const Quadric &o) const {
		Quadric r;
		for (size_t i = 0; i < 10; ++i) r.q[i] = q[i] + o.q[i];
		return r;
	}
	double eval(const D3 &p) const {
		const double x = p[0], y = p[1], z = p[2];
		return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
	}
};

struct Candidate {
	double cost;
	uint32_t from, to, from_stamp, to_stamp;
	bool operator>(const Candidate &o) const { return cost > o.cost; }
};

}  // namespace

void simplify(SimplifyMesh &m, size_t target) {
	const size_t nv = m.vertices.size(), nf = m.faces.size();
	if (nf <= target || nv == 0) return;
	std::vector<D3> pos(nv);
	for (size_t i = 0; i < nv; ++i) pos[i] = d3(m.vertices[i]);
	std::vector<bool> alive(nf, true), dead(nv, false);
	std::vector<std::vector<uint32_t>> vf(nv);
	for (uint32_t f = 0; f < nf; ++f) {
		for (uint32_t v : m.faces[f]) vf[v].push_back(f);
	}
	D3 lo = pos[0], hi = pos[0];
	for (auto &p : pos) {
		for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], p[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], p[size_t(k)]);
	}
	const double diag2 = std::max(1e-12, dot(sub(hi, lo), sub(hi, lo)));
	std::vector<Quadric> quad(nv);
	std::map<std::pair<uint32_t, uint32_t>, int> edge_count;
	auto face_normal = [&](const std::array<uint32_t, 3> &f, D3 *n) {
		D3 c = cross(sub(pos[f[1]], pos[f[0]]), sub(pos[f[2]], pos[f[0]]));
		double l = length(c);
		*n = l > 0 ? D3{c[0] / l, c[1] / l, c[2] / l} : D3{0, 0, 0};
		return l;
	};
	for (uint32_t f = 0; f < nf; ++f) {
		D3 n;
		double area = face_normal(m.faces[f], &n) * 0.5;
		for (uint32_t v : m.faces[f]) quad[v].add_plane(n, -dot(n, pos[m.faces[f][0]]), area);
		for (int k = 0; k < 3; ++k) {
			uint32_t a = m.faces[f][size_t(k)], b = m.faces[f][size_t((k + 1) % 3)];
			++edge_count[{std::min(a, b), std::max(a, b)}];
		}
	}
	// Open edges hold their place: a plane through the edge, across the face, weighted heavily.
	for (uint32_t f = 0; f < nf; ++f) {
		D3 n;
		face_normal(m.faces[f], &n);
		for (int k = 0; k < 3; ++k) {
			uint32_t a = m.faces[f][size_t(k)], b = m.faces[f][size_t((k + 1) % 3)];
			if (edge_count[{std::min(a, b), std::max(a, b)}] != 1) continue;
			D3 e = sub(pos[b], pos[a]);
			D3 bn = cross(e, n);
			double l = length(bn);
			if (l <= 0) continue;
			bn = {bn[0] / l, bn[1] / l, bn[2] / l};
			double w = dot(e, e) * 20;
			quad[a].add_plane(bn, -dot(bn, pos[a]), w);
			quad[b].add_plane(bn, -dot(bn, pos[a]), w);
		}
	}
	// UV seams: a vertex whose corners disagree on layer 0.
	std::vector<bool> seam(nv, false);
	if (!m.uvs.empty() && m.uvs[0].size() == nf) {
		std::vector<std::array<float, 2>> first(nv, {NAN, NAN});
		for (uint32_t f = 0; f < nf; ++f) {
			for (int k = 0; k < 3; ++k) {
				uint32_t v = m.faces[f][size_t(k)];
				const auto &uv = m.uvs[0][f][size_t(k)];
				if (std::isnan(first[v][0])) first[v] = uv;
				else if (std::fabs(first[v][0] - uv[0]) > 1e-5f || std::fabs(first[v][1] - uv[1]) > 1e-5f) seam[v] = true;
			}
		}
	}
	auto skin_distance = [&](uint32_t a, uint32_t b) {
		if (a >= m.skin.size() || b >= m.skin.size()) return 0.0;
		const auto &x = m.skin[a], &y = m.skin[b];
		double s = 0;
		for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
			double d = (i < x.size() ? x[i] : 0) - (i < y.size() ? y[i] : 0);
			s += d * d;
		}
		return s;
	};
	std::vector<uint32_t> stamp(nv, 0);
	std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> heap;
	auto cost = [&](uint32_t from, uint32_t to) {
		double c = (quad[from] + quad[to]).eval(pos[to]);
		if (seam[from] && !seam[to]) c += diag2 * 1e-2;
		else if (seam[from]) c += diag2 * 1e-4;
		c += skin_distance(from, to) * diag2 * 5e-3;
		return std::max(0.0, c);
	};
	auto push = [&](uint32_t from, uint32_t to) { heap.push({cost(from, to), from, to, stamp[from], stamp[to]}); };
	for (uint32_t f = 0; f < nf; ++f) {
		for (int k = 0; k < 3; ++k) {
			uint32_t a = m.faces[f][size_t(k)], b = m.faces[f][size_t((k + 1) % 3)];
			if (a < b) push(a, b), push(b, a);
		}
	}
	auto neighbours = [&](uint32_t v) {
		std::vector<uint32_t> out;
		for (uint32_t f : vf[v]) {
			if (!alive[f]) continue;
			for (uint32_t w : m.faces[f]) {
				if (w != v) out.push_back(w);
			}
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	};
	size_t live = nf;
	while (live > target && !heap.empty()) {
		Candidate c = heap.top();
		heap.pop();
		const uint32_t u = c.from, v = c.to;
		if (dead[u] || dead[v] || c.from_stamp != stamp[u] || c.to_stamp != stamp[v]) continue;
		auto nu = neighbours(u), nvv = neighbours(v);
		if (!std::binary_search(nu.begin(), nu.end(), v)) continue;
		// The link condition: an interior edge shares exactly two neighbours.
		size_t common = 0;
		for (uint32_t w : nu) common += std::binary_search(nvv.begin(), nvv.end(), w) ? 1 : 0;
		if (common > 2) continue;
		bool ok = true;
		for (uint32_t f : vf[u]) {
			if (!alive[f]) continue;
			auto face = m.faces[f];
			if (face[0] == v || face[1] == v || face[2] == v) continue;
			D3 before;
			face_normal(face, &before);
			for (auto &x : face) {
				if (x == u) x = v;
			}
			D3 after;
			if (face_normal(face, &after) <= 1e-12 || dot(before, after) < 0.2) {
				ok = false;
				break;
			}
		}
		if (!ok) continue;
		// Corners of u become v's: v's UV from a collapsed face on the same side of any seam.
		std::vector<uint32_t> removed;
		for (uint32_t f : vf[u]) {
			if (!alive[f]) continue;
			const auto &face = m.faces[f];
			if (face[0] == v || face[1] == v || face[2] == v) {
				alive[f] = false;
				--live;
				removed.push_back(f);
			}
		}
		auto corner = [&](uint32_t f, uint32_t x) {
			for (int k = 0; k < 3; ++k) {
				if (m.faces[f][size_t(k)] == x) return k;
			}
			return -1;
		};
		for (uint32_t f : vf[u]) {
			if (!alive[f]) continue;
			int k = corner(f, u);
			if (k < 0) continue;
			for (size_t layer = 0; layer < m.uvs.size(); ++layer) {
				if (m.uvs[layer].size() != nf) continue;
				const auto mine = m.uvs[layer][f][size_t(k)];
				for (uint32_t r : removed) {
					int ku = corner(r, u), kv = corner(r, v);
					const auto &ru = m.uvs[layer][r][size_t(ku)];
					if (std::fabs(ru[0] - mine[0]) < 1e-5f && std::fabs(ru[1] - mine[1]) < 1e-5f) {
						m.uvs[layer][f][size_t(k)] = m.uvs[layer][r][size_t(kv)];
						break;
					}
				}
			}
			m.faces[f][size_t(k)] = v;
			vf[v].push_back(f);
		}
		quad[v] = quad[v] + quad[u];
		seam[v] = seam[v] || seam[u];
		dead[u] = true;
		++stamp[v];
		vf[u].clear();
		for (uint32_t w : neighbours(v)) {
			push(v, w);
			push(w, v);
		}
	}
	std::vector<std::array<uint32_t, 3>> faces;
	std::vector<std::vector<std::array<std::array<float, 2>, 3>>> uvs(m.uvs.size());
	for (uint32_t f = 0; f < nf; ++f) {
		if (!alive[f]) continue;
		faces.push_back(m.faces[f]);
		for (size_t layer = 0; layer < m.uvs.size(); ++layer) {
			if (m.uvs[layer].size() == nf) uvs[layer].push_back(m.uvs[layer][f]);
		}
	}
	m.faces = std::move(faces);
	m.uvs = std::move(uvs);
}

}  // namespace ed
