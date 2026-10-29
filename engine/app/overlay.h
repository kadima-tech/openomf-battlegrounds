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
	// y up; text heights are fractions of the screen height), those whose actor is renderable.
	void draw(const game::World &world, int width, int height);
	// Draws the primitives of every other mesh in the scene (`vp` the view-projection): the same
	// vertices in the local frame of the mesh's actor (x along the text, y up, text heights in
	// its units), depth-tested unless the primitive is an overlay.
	void draw_world(const game::World &world, const float *vp);
	// The front end's controls (game/ui.h), in screen fractions with y down: a filled rectangle,
	// and a flat-text label (Font_Ariel) of `size` screen heights centred on (x, y).
	void rect(float x0, float y0, float x1, float y1, const float *rgba, int width, int height);
	// centred on x, or starting there.
	void label(const std::string &text, float x, float y, float size, const float *rgba, int width, int height, bool centered = true);
	void image_rect(const de::ObjectRef &image, float x0, float y0, float x1, float y1, int width, int height, const float *tint = nullptr);
	// A rendered texture (origin bottom left).
	void texture_rect(GLuint tex, float x0, float y0, float x1, float y1, int width, int height);
	// The width of a label, in screen widths.
	float text_width(const std::string &text, float size, int width, int height);

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
	void begin_screen(int width, int height);
	void end_screen();
	void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float *rgba);
	GLuint image(const game::GameObject *img);
	GLuint image(const de::ObjectsFile *file, uint32_t index);
	std::map<std::string, FontGL> fonts_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> images_;
	float aspect_ = 16.0f / 9;
	GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
	GLuint prog3_ = 0, vao3_ = 0, vbo3_ = 0;  // world-space primitives
};

}  // namespace render
