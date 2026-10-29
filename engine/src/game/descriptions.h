// Descriptions as the menus see them (FINDINGS 127): their casts, listing and lock flags, and
// their large representation (a ControlCreator, the panel a chooser shows for the highlighted one).
#pragma once

#include <string>
#include <vector>

#include "de/scene.h"

namespace game {

// DE_CDescription::ObjProcessContents (Core 0x701f14e0): [name][resource passive reference]
// v2+ [BOOL listed +0xbc] v3 [f32 +0xd4][BOOL locked by default +0xc4][BOOL visible when locked
// +0xc8][BOOL +0xd8][BOOL +0xcc][BOOL +0xd0]; ObjProcessReferences v6 (0x701f17a0): [member
// casts +0x34][BOOL icon][BOOL large representation][components][replacements][BOOL medium].
struct DescriptionInfo {
	std::vector<de::ObjectRef> casts;
	de::ObjectRef large;
	bool listed = true;
	bool locked_by_default = false;
	bool visible_when_locked = false;
};
DescriptionInfo read_description(const de::World &data, const de::ObjectRef &description);

// A Value\String's text.
std::string read_string_value(const de::ObjectRef &value);

// DE_CControlCreator (Core: ObjProcessContents 0x7021c270, CreateNewControl 0x7021c520): contents
// v1 [name][i32 layout +0x44][i32 wrap +0x40][i32 text height +0x48, virtual pixels][f32 +0x4c];
// references, its items in order: a Value\String is a text block of that height (word-wrapped
// unless +0x40 is 0), an image keeps its own size, a ControlCreator nests. Items stack downwards,
// or across for layouts 1 and 3.
struct ControlLayout {
	enum Kind { Group, Text, Image } kind = Group;
	bool across = false;
	bool wrap = true;
	int text_height = 16;
	std::string text;
	de::ObjectRef image;
	std::vector<ControlLayout> items;
};
ControlLayout read_control_creator(const de::World &data, const de::ObjectRef &creator);

}  // namespace game
