// TextureManager objects: per-face texture coordinates for one material stage.
// ObjProcessExtendedContents (Core 0x70131f80), "ObjExtCon" EX, chunk v1:
//   i32 n, n * {u32 mesh face index, 3 * (f32 u, f32 v)}
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "de/objects_file.h"

namespace de {

struct FaceUV {
	uint32_t face;
	std::array<std::array<float, 2>, 3> uv;
};

std::vector<FaceUV> read_texture_uvs(const ObjectsFile &f, const ObjectInfo &o);

}  // namespace de
