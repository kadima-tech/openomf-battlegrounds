// Small pure computations behind natives, kept apart so they can be tested without game data.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace game {

// MSVCRT __ftol: truncation towards zero; NaN and values outside int32 give 0x80000000.
inline int32_t ftol(double v) {
	if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;
	return int32_t(v);
}

// DE_COMF2Fighter::DampenMomentum (OMF 0x35029e20): the momentum shortened by `amount`, zero when
// it is no longer than that.
inline std::array<float, 3> dampen_momentum(const std::array<float, 3> &m, float amount) {
	float l = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
	float k = l <= amount ? 0 : (l - amount) / l;
	return {m[0] * k, m[1] * k, m[2] * k};
}

// DE_CMeshSpawner::CommonSpawnerNewParticles (Particles 0x4000d000): the vertex a new particle takes
// from a mesh of `n` vertices in order 1-4, advancing `next` (the instance's +4). Orders 3 and 4 pass
// once: at the end `pass_done` is set (the particle is dropped and spawning stops). -1 for order 0
// (a random vertex); another order stays on the current vertex. After an order 4 pass the original
// reads vertex n, one past the mesh's end (no shipped spawner uses order 4); ours takes n - 1.
struct MeshStep {
	int vertex;
	bool pass_done;
};
inline MeshStep mesh_vertex_step(int order, int n, int &next) {
	if (n <= 0 || order == 0) return {-1, false};
	int k = next < 0 ? 0 : next >= n ? n - 1 : next;
	bool done = false;
	switch (order) {
	case 1: if (++next >= n) next = 0; break;
	case 2: if (next < 1) next = n; --next; break;
	case 3: if (++next >= n) next = 0, done = true; break;
	case 4: if (next < 1) next = n, done = true; else --next; break;
	}
	return {k, done};
}

}  // namespace game
