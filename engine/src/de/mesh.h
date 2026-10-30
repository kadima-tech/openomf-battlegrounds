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
#include <functional>
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

// DE_COctree (Core 0x70204df0..) over a mesh's faces, in mesh space. Each node keeps the faces
// of every leaf under it (CIndexNode::CopyIDsUpTree), sorted; a leaf those whose bounds touch its box.
struct Octree {
	struct Node {
		std::array<float, 3> lo, hi;
		float radius = 0;  // half the box's diagonal (+0x28)
		bool leaf = false;
		std::array<int32_t, 8> child{-1, -1, -1, -1, -1, -1, -1, -1};
		std::vector<uint32_t> faces;
	};
	std::vector<Node> nodes;  // the root first
	explicit operator bool() const { return !nodes.empty(); }
	// GetPotentialSphereCollisions (0x702064e0): the nodes whose boxes the sphere's bounding box
	// touches, taken whole at a leaf or once smaller than the sphere.
	void sphere_faces(const std::array<float, 3> &c, float r, std::vector<uint32_t> &out) const;
	// CIndexNode::GetPotentialCollisions (0x70205e60): each node as a sphere (its centre through
	// `to_world`, its radius) asked of `touches`; a leaf that touches gives its faces.
	void affector_faces(const std::function<bool(const std::array<float, 3> &centre, float radius)> &touches,
	                    const std::function<std::array<float, 3>(const std::array<float, 3> &)> &to_world, std::vector<uint32_t> &out) const;
};
// The mesh object's stored "D3DProjectedTargetOctree" (DE_COctree::ObjProcessContents 0x702051c0,
// v2/v3); empty when it has none.
Octree read_octree(const ObjectsFile &f, const ObjectInfo &o);
// DE_COctree::Create (0x70207010) and CNode::AssureMaximumSize (0x70205450): the faces' bounding
// box, split at its centre along each axis longer than `max_size` and at least half the longest,
// no deeper than six levels below the root.
Octree build_octree(const Mesh &m, float max_size);

}  // namespace de
