#include "schema.h"

namespace deo {

namespace {

using T = FieldType;
constexpr Block C = Block::Contents;
constexpr Block R = Block::References;
constexpr Block H = Block::Hierarchy;

// Sources are functions in the original game's modules (Core.ModuleDLL unless noted) and the
// matching notes in re/FINDINGS.md and engine/src.
const std::vector<ChunkSchema> kSchemas = {
	// Every object: DE_CObject::ObjProcessHierarchy (0x7003c110), its child objects in this file.
	{H, "", 0, "Object", {{"children", T::RefList}}, "DE_CObject::ObjProcessHierarchy 0x7003c110"},

	// Descriptions (what menus list: arenas, robots, pilots, tournaments, game types).
	{C, "Description", 0, "Description",
	 {{"name", T::Text},
	  {"resource", T::PassiveRef},
	  {"listed", T::Bool, 2},
	  {"unknown_d4", T::F32, 3},
	  {"locked_by_default", T::Bool, 3},
	  {"visible_when_locked", T::Bool, 3},
	  {"default_model", T::Bool, 3},
	  {"unknown_cc", T::Bool, 3},
	  {"unknown_d0", T::Bool, 3}},
	 "DE_CDescription::ObjProcessContents 0x701f14e0 (engine/src/game/descriptions.h)"},
	{R, "Description", 1, "Description",
	 {{"casts", T::RefList, 6},
	  {"icon", T::OptRef, 6},
	  {"large_representation", T::OptRef, 6},
	  {"components", T::RefList, 6},
	  {"replacements", T::RefList, 6},
	  {"medium_representation", T::OptRef, 6}},
	 "DE_CDescription::ObjProcessReferences 0x701f17a0 (FINDINGS 158)"},
	{C, "Description\\Environment", 1, "EnvironmentDescription",
	 {{"min_recommended_players", T::I32, 1}, {"max_recommended_players", T::I32, 1}},
	 "DE_CEnvironmentDescription::ObjProcessContents, Creature 0x1500aa20"},
	{C, "Description\\FighterModelDescription", 1, "FighterModelDescription",
	 {{"fighter_description", T::Guid, 1}}, "DE_CFighterModelDescription::ObjProcessContents (OMF)"},

	// Menu panel layout.
	{C, "ControlCreator", 0, "ControlCreator",
	 {{"name", T::Text},
	  {"layout", T::I32, 1},
	  {"alignment", T::I32, 1},
	  {"text_height", T::I32, 1},
	  {"image_scale", T::F32, 1}},
	 "DE_CControlCreator::ObjProcessContents 0x7021c270 (engine/src/game/descriptions.h)"},
	{C, "Folder", 0, "Folder", {{"name", T::Text}}, "observed: a v1 chunk holding the name"},

	// Values.
	{C, "Value", 0, "Value", {{"name", T::Text}}, "DE_CValue: the object chunk (engine/src/de/value.cpp)"},
	{C, "Value\\String", 1, "ValueString", {{"text", T::Text}}, "engine/src/game/descriptions.cpp read_string_value"},
	{C, "Value\\FLOAT", 1, "ValueFLOAT", {{"value", T::F32, 1}}, "engine/src/de/value.cpp read_float_chunk"},
	{C, "Value\\RGBA", 1, "ValueRGBA",
	 {{"r", T::F32, 1}, {"g", T::F32, 1}, {"b", T::F32, 1}, {"a", T::F32, 1}},
	 "engine/src/de/value.cpp read_rgba_chunk"},

	// Images: the pixels are in extended data entries keyed {0, sub-image index}.
	{C, "Image", 0, "Image",
	 {{"name", T::Text},
	  {"type", T::I32, 3},
	  {"width", T::I32, 3},
	  {"height", T::I32, 3},
	  {"mipmap", T::Bool, 3},
	  {"unknown_f32", T::F32, 4},
	  {"adjust_mipmap_level", T::Bool, 4}},
	 "DE_CImage::ObjProcessContents 0x7018d850 (engine/src/de/image.cpp)"},

	// Cameras.
	{C, "Camera", 0, "Camera",
	 {{"name", T::Text}, {"field_of_view", T::F32, 3}, {"near", T::F32, 4}, {"far", T::F32, 4}},
	 "DE_CCamera::ObjProcessContents 0x70187a70 (engine/app/game_view.cpp)"},

	// Script runners.
	{C, "ClassExecutor", 0, "ClassExecutor", {{"name", T::Text}, {"entry_state", T::Text}},
	 "DE_CClassExecutor::ObjProcessContents 0x701434b0 (engine/src/game/session.cpp)"},
};

bool prefix_matches(const char *prefix, const std::string &path) {
	std::string p = prefix;
	if (p.empty()) return true;
	if (path.compare(0, p.size(), p) != 0) return false;
	return path.size() == p.size() || path[p.size()] == '\\';
}

}  // namespace

const std::vector<ChunkSchema> &all_schemas() { return kSchemas; }

const ChunkSchema *find_schema(Block block, const std::string &class_path, size_t index) {
	const ChunkSchema *best = nullptr;
	size_t best_len = 0;
	for (const ChunkSchema &s : kSchemas) {
		if (s.block != block || size_t(s.index) != index || !prefix_matches(s.class_prefix, class_path)) continue;
		size_t len = std::string(s.class_prefix).size();
		if (!best || len > best_len) {
			best = &s;
			best_len = len;
		}
	}
	return best;
}

const char *type_name(FieldType t) {
	switch (t) {
	case T::U8: return "u8";
	case T::Bool: return "bool";
	case T::I32: return "i32";
	case T::U32: return "u32";
	case T::F32: return "f32";
	case T::Text: return "text";
	case T::Guid: return "guid";
	case T::PassiveRef: return "passive reference";
	case T::Ref: return "reference";
	case T::OptRef: return "optional reference";
	case T::RefList: return "reference list";
	}
	return "?";
}

}  // namespace deo
