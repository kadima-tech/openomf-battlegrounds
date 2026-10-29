#include "de/font.h"

#include <algorithm>

#include "de/contents.h"
#include "de/mesh.h"

namespace de {

Font read_font(const World &w, const std::string &mesh_name) {
	const ObjectsFile *f = w.file("COREMeshes");
	if (!f) throw FormatError("no COREMeshes objects file (Engine/Core.ModuleDLL)");
	ObjectRef mesh;
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		if (f->objects()[i].name == mesh_name && f->objects()[i].class_path == "Mesh\\Standard") mesh = {f, i};
	}
	if (!mesh) throw FormatError("no font mesh " + mesh_name);
	Mesh m = read_mesh(*f, mesh.info());
	auto materials = read_materials(w, mesh);
	const Layer *layer = nullptr;
	for (auto &mat : materials) {
		for (auto &l : mat.layers) {
			if (!layer && !l.uvs.empty()) layer = &l;
		}
	}
	if (!layer || m.faces.size() < 2) throw FormatError("font " + mesh_name + " has no textured faces");
	std::vector<const std::array<std::array<float, 2>, 3> *> uv(m.faces.size(), nullptr);
	for (auto &fu : layer->uvs) {
		if (fu.face < uv.size()) uv[fu.face] = &fu.uv;
	}
	// Pair each unused face with the next face sharing one of its vertices.
	std::vector<bool> used(m.faces.size(), false);
	struct Letter {
		Font::Glyph g;
	};
	std::vector<Letter> letters;
	for (size_t i = 0; i < m.faces.size(); ++i) {
		if (used[i]) continue;
		used[i] = true;
		size_t j = i + 1;
		for (; j < m.faces.size(); ++j) {
			if (used[j]) continue;
			bool share = false;
			for (auto a : m.faces[i].v) {
				for (auto b : m.faces[j].v) share |= a == b;
			}
			if (share) break;
		}
		Font::Glyph g;
		g.x0 = g.y0 = g.u0 = g.v0 = 1e30f;
		g.x1 = g.y1 = g.u1 = g.v1 = -1e30f;
		auto take = [&](size_t fi) {
			for (int k = 0; k < 3; ++k) {
				auto &p = m.vertices[m.faces[fi].v[size_t(k)]];
				g.x0 = std::min(g.x0, p[0]), g.x1 = std::max(g.x1, p[0]);
				g.y0 = std::min(g.y0, p[1]), g.y1 = std::max(g.y1, p[1]);
				if (uv[fi]) {
					auto &t = (*uv[fi])[size_t(k)];
					g.u0 = std::min(g.u0, t[0]), g.u1 = std::max(g.u1, t[0]);
					g.v0 = std::min(g.v0, t[1]), g.v1 = std::max(g.v1, t[1]);
				}
			}
		};
		take(i);
		if (j < m.faces.size()) {
			used[j] = true;
			take(j);
		}
		g.present = true;
		letters.push_back({g});
	}
	std::stable_sort(letters.begin(), letters.end(), [](const Letter &a, const Letter &b) { return a.g.x0 < b.g.x0; });
	Font font;
	font.image = layer->image;
	float lo = 1e30f, hi = -1e30f;
	for (auto &l : letters) lo = std::min(lo, l.g.y0), hi = std::max(hi, l.g.y1);
	font.height = hi - lo;
	font.base = lo;
	for (size_t c = 0; c < letters.size() && c < 256; ++c) {
		Font::Glyph g = letters[c].g;
		g.y0 -= lo, g.y1 -= lo;  // CLetter::FixUp
		font.glyphs[c] = g;
	}
	return font;
}

}  // namespace de
