// Quadric-error mesh decimation for generated LODs (docs/EDITOR_SPEC.md §5.5, robot import).
//
// Half-edge collapses (a vertex merges into a neighbour, so no new positions or skin weights are
// made) ordered by the Garland-Heckbert quadric error, with penalties for open edges, UV seams and
// skin weight changes, refusing collapses that flip a face.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ed {

struct SimplifyMesh {
	std::vector<std::array<float, 3>> vertices;
	std::vector<std::array<uint32_t, 3>> faces;
	// Per face corner, per UV layer.
	std::vector<std::vector<std::array<std::array<float, 2>, 3>>> uvs;
	// Per vertex: a short signature of its skin (weights per bone, any length), compared by distance.
	std::vector<std::vector<float>> skin;
};

// Removes faces until at most `target` remain (or nothing more can collapse). Faces keep their
// order of survival; vertices keep their indices (unreferenced ones are left for the caller).
void simplify(SimplifyMesh &m, size_t target);

}  // namespace ed
