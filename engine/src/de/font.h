// Flat-text fonts (Core DE_CFlatText3Factory::CFontInfo::CreateFont, 0x701c9...): a font is a
// mesh in COREMeshes ("Font_Ariel", "Font_Metalic") of 512 triangles, paired into 256 letter
// quads by shared vertices; sorted left to right, quad n is character n. Each letter keeps its
// extent in the mesh and its texture coordinates; heights are relative to the lowest letter.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "de/scene.h"

namespace de {

struct Font {
	struct Glyph {
		float x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // mesh units, y from the font's bottom
		float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
		bool present = false;
	};
	std::array<Glyph, 256> glyphs;
	float height = 0;  // tallest extent of all letters (CFontInfo +0x24)
	float base = 0;    // the lowest letter's bottom in the mesh (CFontInfo +0x2c), added back by CFontInfo::Update
	ObjectRef image;
};

// Throws FormatError when the mesh is missing or not a font.
Font read_font(const World &w, const std::string &mesh_name);

}  // namespace de
