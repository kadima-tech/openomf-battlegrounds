// Robot shadows (DE_CShadowCaster, Direct3D 0x20035cf0..0x20036aa0; FINDINGS 141): each robot draws
// its silhouette, grey by the shadow's strength, into a small map seen from a light point, and a
// darkening cone (light mode 2: ZERO / INVSRCCOLOR) projects that map onto the projector-lit
// meshes around it. Preferences\Renderer\Shadow Type: 0 none, 1 one shadow from straight above,
// 2 one from the static lights, 3 one from the static and one from the other lights.
#pragma once

#include <array>
#include <functional>
#include <vector>

#include "gl_util.h"
#include "render.h"

namespace render {

class ShadowRenderer {
public:
	struct Caster {
		const void *owner = nullptr;           // its render affector's owner (the lights' reach)
		std::array<float, 3> centre{0, 0, 0};  // the render affector's sphere
		float radius = 1;
		std::function<void(GLuint prog)> draw;  // its posed meshes, as drawn this frame
	};
	// After the casters are drawn: each one's shadows from the frame's lights, and their maps.
	// `texture_detail` is the applied Texture Detail (0 High .. 2 Low), `view` the main camera's
	// view matrix, `fov` its full field of view (degrees).
	void update(int type, int texture_detail, const std::vector<Caster> &casters, const Lighting &lights, const float *view, float fov);
	// The shadow cones, for the next frame's light list (layer 2: after the additive lights).
	void append(Lighting &lights) const;
	void clear() { shadows_.clear(); }

private:
	struct Shadow {
		const void *owner = nullptr;
		bool on = false;
		SceneLight cone;
		int level = -100, size = 0;
		float timer = 1;
		GLuint tex = 0, fbo = 0;
	};
	void render_map(Shadow &s, const Caster &c, float strength, float half_extent);
	std::vector<Shadow> shadows_;  // two per caster, in caster order
	GLuint prog_ = 0;
};

}  // namespace render
