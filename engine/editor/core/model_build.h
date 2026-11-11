// The object writers the model importers share (model_import.cpp): new objects with fresh GUIDs,
// hierarchy lists, images and mesh geometry in the layouts the original reads.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "addon.h"
#include "de/mesh.h"

namespace ed::build {

using V3 = std::array<float, 3>;

Bytes stored(const std::string &utf8);
uint32_t add_object(Project &p, const std::string &class_path, const std::string &name, Bytes contents, Bytes references, Bytes hierarchy = {1, 1, 0});
Ref local_ref(uint32_t index);
void set_children(Project &p, uint32_t parent, const std::vector<uint32_t> &kids);
void add_child(Project &p, uint32_t parent, uint32_t child);
// The object's name and its contents' first name block.
void rename(Project &p, uint32_t object, const std::string &name);
void put_f32(deo::Writer &w, float f);
// Image\Standard, RGB (mask 7, format 20) or RGBA (mask 15, format 0), one uncompressed level.
uint32_t add_image(Project &p, const std::string &name, int width, int height, const std::vector<uint8_t> &rgba);
int pow2_at_most(int v, int cap);

// The face across each edge (0xffff at an open edge).
void mesh_adjacency(std::vector<de::Mesh::Face> &faces);
// Smoothing groups: faces whose corner normals agree across an edge share a group bit.
void mesh_smoothing(std::vector<de::Mesh::Face> &faces, const std::vector<std::array<V3, 3>> &corner_normals);
// A Mesh\Standard's ObjExtCon payload (chunk v1: positions, faces with adjacency and smoothing).
Bytes mesh_geometry(const std::vector<V3> &vertices, const std::vector<de::Mesh::Face> &faces);

}  // namespace ed::build
