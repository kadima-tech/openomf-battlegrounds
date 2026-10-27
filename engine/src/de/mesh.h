// Mesh\Standard objects (DE_CStandardMesh).
//
// Contents: DE_CMesh chunk then DE_CStandardMesh chunk (Core 0x7012ec00); v6 stores only the
// vertex/face counts. Geometry is in the "ObjExtCon" EX entry, fetched raw by
// DE_CObject::ObjLoadExtendedContents (0x70040c30) and read by
// DE_CStandardMesh::ObjProcessExtendedContents (0x7012f6b0):
//   chunk v1: u16 nverts, nverts * f32[3]; u16 nfaces, nfaces * {3 * (u16 vertex, u16 adjacent face), u32}
// Normals are computed after load (CalculateNormals); texture coordinates are not stored here.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "de/objects_file.h"

namespace de {

struct Mesh {
	std::string name;
	std::vector<std::array<float, 3>> vertices;
	struct Face {
		std::array<uint16_t, 3> v;
		std::array<uint16_t, 3> adjacent;  // inferred: face across each edge (always < face count)
		uint32_t flags;                    // inferred: smoothing group
	};
	std::vector<Face> faces;
	// DE_CMesh +0x40 (base chunk v3+, Core 0x701275e0). DE_CRendererDirect3D::CreateNewMesh reads
	// 0x08 lit (with 0x10: D3D vertex lighting, else lit by light projector passes) and 0x20
	// projected-texture target.
	uint32_t flags = 0;
	bool vertex_lit() const { return (flags & 0x18) == 0x18; }
	bool projector_lit() const { return (flags & 0x18) == 0x08; }
};

Mesh read_mesh(const ObjectsFile &f, const ObjectInfo &o);

}  // namespace de
