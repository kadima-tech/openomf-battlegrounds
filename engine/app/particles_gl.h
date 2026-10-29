// Particle effects: the quads DE_CBillboardedQuadParticleRenderer builds for each live particle
// (Particles 0x40010620 Process, 0x400119b0 SetParticleVerticies), drawn after the opaque scene
// with depth testing but no depth writes. Blend 0/1 add (colour premultiplied by alpha, as
// NormalizeAlpha does); blend 2 alpha-blends.
#pragma once

#include <functional>
#include <map>
#include <vector>

#include "game/particles.h"
#include "gl_util.h"

namespace render {

class ParticleRenderer {
public:
	void load();
	// `view` is the camera's view matrix (its rows give the screen axes), `eye` its position.
	// With `glow_layer`, only the renderers that glow, for the glow target.
	void draw(const game::World &world, const float *vp, const float *view, const float *eye, bool glow_layer = false);
	// When set, only the effects of actors it accepts are drawn.
	std::function<bool(const game::GameObject *)> only;

private:
	struct Vertex {
		float pos[3];
		float uv[2];
		float rgba[4];
	};
	struct Batch {
		GLuint tex = 0;
		bool additive = true;
		std::vector<Vertex> v;
	};
	GLuint image(const de::ObjectRef &r);
	void ring(const game::ParticleInstance &in, const std::array<float, 3> &right, const std::array<float, 3> &up,
	          std::map<std::pair<GLuint, bool>, Batch> &batches);
	void stream(const game::World &world, const game::ParticleInstance &in, const float *view,
	            std::map<std::pair<GLuint, bool>, Batch> &batches);
	void triangles(const game::World &world, const game::ParticleInstance &in, const float *view, const std::array<float, 3> &cam,
	               std::map<std::pair<GLuint, bool>, Batch> &batches);
	void trail(const game::World &world, const game::Trail &t, const float *view, std::map<std::pair<GLuint, bool>, Batch> &batches);
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> images_;
	GLuint prog_ = 0, vao_ = 0, vbo_ = 0, white_ = 0;
};

}  // namespace render
