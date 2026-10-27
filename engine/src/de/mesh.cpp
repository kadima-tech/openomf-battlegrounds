#include "de/mesh.h"

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

}  // namespace de
