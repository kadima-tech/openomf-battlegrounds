// Screen overlay: what the scripts draw through CPrimitiveGeneratorMesh primitives in screen
// space: text (CTextPrimitive, in the engine's flat-text fonts from COREMeshes), quads and status
// bars (the HUD).
#pragma once

#include <map>
#include <string>

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

private:
	struct FontGL {
		de::Font font;
		GLuint tex = 0;
		bool alpha = false;  // the image has an alpha channel (else luminance is used)
	};
	void text(const FontGL &f, const game::World::Primitive &t, const float *rgba, float dx, float dy);
	void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float *rgba);
	GLuint image(const game::GameObject *img);
	std::map<std::string, FontGL> fonts_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> images_;
	float aspect_ = 16.0f / 9;
	GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
};

}  // namespace render
