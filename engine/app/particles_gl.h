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
#include "render.h"

namespace render {

class ParticleRenderer {
public:
	void load();
	// `view` is the camera's view matrix (its rows give the screen axes), `eye` its position.
	// With `glow_layer`, only the renderers that glow, for the glow target.
	// `meshes`: the frame's deferred blended mesh passes, drawn among the effects in the layer's order.
	void draw(const game::World &world, const float *vp, const float *view, const float *eye, bool glow_layer = false,
	          const std::vector<DeferredDraw> *meshes = nullptr);
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
	void ring(const game::ParticleInstance &in, const std::array<float, 3> &right, const std::array<float, 3> &up, const float *view, float focal_px,
	          std::map<std::pair<GLuint, bool>, Batch> &batches);
	void stream(const game::World &world, const game::ParticleInstance &in, const float *view,
	            std::map<std::pair<GLuint, bool>, Batch> &batches);
	void triangles(const game::World &world, const game::ParticleInstance &in, const float *view, const std::array<float, 3> &cam,
	               std::map<std::pair<GLuint, bool>, Batch> &batches);
	void trail(const game::World &world, const game::Trail &t, const float *view, std::map<std::pair<GLuint, bool>, Batch> &batches);
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> images_;
	// The transparent layer's order (DE_CClipVolume::DrawLayer, layer 8): each element's static key
	// (master object index x 1000 + its root's shader counter + the shader index for particle
	// renderers, the shader index otherwise) plus its actor's view-space z at its last draw,
	// sorted highest first only when an element joins.
	struct Element {
		float key = 0, z = 0;
		const game::GameObject *actor = nullptr;
		uint32_t seen = 0;
		std::array<float, 3> origin{0, 0, 0};  // a mesh pass: where its actor is
		std::function<void()> mesh;           // a mesh pass: draws it
	};
	std::map<const void *, Element> elements_;
	std::vector<const void *> order_;
	std::map<const void *, int> master_index_, root_counter_;
	bool order_dirty_ = false;
	GLuint prog_ = 0, vao_ = 0, vbo_ = 0, white_ = 0;
};

}  // namespace render
