// The glow layer (DE_CRendererDirect3D::UpdateGlowMaps 0x20026ce0, DrawGlow 0x20028870): the
// shaders of layer 9 (GlowFactor / SpecularFactor passes) are drawn into a 256x256 target with
// its own depth (SetupGlowRenderTarget), spread by SuperSampleForGlow (five additive passes of
// the 8-tap average, radius 0 then four at 2 texels), reduced 4x4 twice (Downsample4x4: 64x64, 16x16;
// the high-quality glow) and added over the frame by the 8-sample composite (sum / 6 - 0.05) at
// the taps DrawGlow sets: four axial and four diagonal from the 256 target, then four each from
// the 64 and 16 ones at twice and three times the spread. The pixel shaders are the HLSL the
// Direct3D module carries.
#pragma once

#include "gl_util.h"

namespace render {

class GlowRenderer {
public:
	// Bind the glow target, cleared to black; draw the glow layer after this.
	void begin();
	// Spread and reduce it, then add it over the default framebuffer (w x h).
	void end(int w, int h);

private:
	struct Target {
		GLuint fbo = 0, tex = 0, depth = 0;
		int size = 0;
	};
	void make(Target &t, int size, bool depth);
	void into(const Target &t);
	Target glow_, spare_, d64_, d16_;
	GLuint vao_ = 0, super_ = 0, down_ = 0, comp_ = 0;
	GLint prev_fbo_ = 0;
	GLint prev_vp_[4] = {0, 0, 0, 0};
};

}  // namespace render
