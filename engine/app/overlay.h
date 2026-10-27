// Screen overlay: what the scripts draw through CPrimitiveGeneratorMesh primitives in screen
// space: text (CTextPrimitive, in the engine's flat-text fonts from COREMeshes), quads and status
// bars (the HUD).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "de/font.h"
#include "game/runtime.h"
#include "gl_util.h"

namespace render {

class OverlayRenderer {
public:
	void load(const de::World &world);
	// Draws the primitives of every screen-space mesh (positions are fractions of the screen,
	// y up; text heights are fractions of the screen height). `focus` is the fighter whose HUD
	// shows.
	void draw(const game::World &world, int width, int height, const game::GameObject *focus);
	// Draws the primitives of every other mesh in the scene (`vp` the view-projection): the same
	// vertices in the local frame of the mesh's actor (x along the text, y up, text heights in
	// its units), depth-tested unless the primitive is an overlay.
	void draw_world(const game::World &world, const float *vp);

private:
	struct FontGL {
		de::Font font;
		GLuint tex = 0;
		bool alpha = false;  // the image has an alpha channel (else luminance is used)
	};
	void text(const FontGL &f, const game::World::Primitive &t, const float *rgba, float dx, float dy);
	// The text's glyph quads (x, y, u, v per vertex), x widths divided by `aspect`.
	std::vector<float> text_vertices(const FontGL &f, const game::World::Primitive &t, float aspect, float dx, float dy) const;
	const FontGL *font_for(const game::World::Primitive &t) const;
	void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float *rgba);
	GLuint image(const game::GameObject *img);
	std::map<std::string, FontGL> fonts_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> images_;
	float aspect_ = 16.0f / 9;
	GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
	GLuint prog3_ = 0, vao3_ = 0, vbo3_ = 0;  // world-space primitives
};

}  // namespace render
