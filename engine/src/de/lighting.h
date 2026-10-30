// Scene lighting data: Light objects, the environment's ambient colour and the per-corner colour
// channels of texture stages.
//
// Light (DE_CLight::ObjProcessContents, Core 0x701baf70) v5: [object chunk: name][CValueRGBA
// advanced reference: colour (+0x38)][f32 intensity (+0x4c)][i32 mode (+0x50)][i32 +0x54]. The
// references hold the colour's advanced reference (an RGBA Value object, else the embedded one).
// DE_CStandardRenderLight::SetLight (Direct3D 0x2000f510) turns the mode into the actor mask
// the light reaches: 0 -> 6 (vertex-lit and projector-lit meshes), 1 -> 4 (projector-lit), 2 ->
// 0x10, 3 -> 8; +0x54 1 marks it dynamic. References v3 add [BOOL][reference]: the projector
// image (+0x30). DE_CD3DLight::SetupComplete (Direct3D 0x2002d1d0): a cone without one joins the
// shared cone projector (the core LightMap, additive); a cone with an Image gets its own projector
// (SetupConeProjector(DE_CLight), 0x20034750) over that image, blended by the mode and +0x54 (see
// projector_blend); a sphere without one joins the shared sphere projector, one with an image gets
// no projector pass.
//
// Environment (DE_CEnvironment::ObjProcessContents, Creature 0x1500b550) v2/v3: [CValueRGBA
// advanced reference: ambient (+900, 0.3 grey by default)][f32 ambient scale (+0x398)];
// DE_CEnvironment::ObjPrepare copies them to the scene, and DE_CStandardRenderer::UpdateScene
// sets the D3D ambient to colour x scale.
//
// DataChannel\UnitFloats under a TextureManager ("ColorDataChannel", channel name "ColorRGB"):
// [DE_CDataChannel chunk][v1: i32 components, i32 entries, entries x components bytes]; the
// texture manager fills its stage's colour element (one entry per face corner, in the stage's
// face order) from it (DE_CTextureManager::Process, Core 0x701327c0).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "de/scene.h"
#include "de/value.h"

namespace de {

struct LightInfo {
	ColorValue colour;
	float intensity = 1;
	int32_t mode = 0;
	int32_t dynamic = 0;
	ObjectRef image;  // the projector image (null: the core LightMap)
	uint32_t mask() const;  // SetLight's actor mask
};
// SetupConeProjector(DE_CLight) and SetStates (0x20033830): the blend of a light's own cone
// projector, from its mode (+0x50: 0/1 -> 0, 2 -> 2, 3 -> 1) overridden by +0x54 (1 -> 3, 2 -> 4,
// 3 -> 5). 0 and 3 are ONE/ONE, 1 and 5 SRCALPHA/INVSRCALPHA, 2 ZERO/INVSRCCOLOR (darkens), 4
// ONE/INVSRCCOLOR.
int projector_blend(const LightInfo &l);
std::optional<LightInfo> read_light(const World &w, const ObjectRef &light);

// A Light of a scene file where the file places it: at its parent sphere or cone affector,
// whose shape comes from the affector's third contents chunk (DE_CSphereAffector v2: inner
// +0x184, outer +0x17c; DE_CConeAffector v2: far begin/end, near begin/end, umbra, penumbra).
struct PlacedLight {
	LightInfo info;
	Transform world;
	bool cone = false;
	float inner = 0, outer = 0, umbra = 0, penumbra = 0;
	float near_begin = 0, near_end = 0;  // cone only
	bool position_static = false;  // its affector (DE_CActor::IsPositionStatic)
};
std::vector<PlacedLight> read_scene_lights(const World &w, const ObjectsFile &file);

// The arena's ambient colour x scale (0.3 grey when there is no environment).
std::array<float, 3> read_ambient(const World &w, const ObjectsFile &arena);

// Image\CubeEnvironment: its third references chunk lists the face images, Top, Front, Bottom,
// Left, Right, Back (the environment's own six slots, DE_CEnvironment::ObjProcessReferences).
// A cube's "HighRangeSpecular" child (or the environment's) is its specular cube
// (DE_CStandardRenderer::CubeSpecularEnvironmentImage, Direct3D 0x20009ce0).
std::array<ObjectRef, 6> read_cube_faces(const World &w, const ObjectRef &cube);
ObjectRef specular_cube(const ObjectRef &owner);

// The per-corner colours (0..1 RGB) of a texture manager's colour channel, if it has one.
std::vector<std::array<float, 3>> read_corner_colours(const ObjectsFile &f, uint32_t texture_manager);

}  // namespace de
