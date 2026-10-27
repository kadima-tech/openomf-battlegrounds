#include "de/texture.h"

namespace de {

std::vector<FaceUV> read_texture_uvs(const ObjectsFile &f, const ObjectInfo &o) {
	auto d = f.object_data(o);
	auto ext = f.ex_payload(d, "ObjExtCon");
	std::vector<FaceUV> out;
	if (!ext) {
		return out;
	}
	Contents c(ext->data);
	Chunk ch = begin_chunk(c);
	if (ch.version != 1) {
		throw FormatError("TextureManager: unsupported extended contents version " + std::to_string(ch.version));
	}
	out.resize(c.i32());
	for (auto &fu : out) {
		fu.face = c.u32();
		for (auto &uv : fu.uv) {
			uv = {c.f32(), c.f32()};
		}
	}
	c.seek(ch.end);
	return out;
}

}  // namespace de
