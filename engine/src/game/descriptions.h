// Descriptions as the menus see them (FINDINGS 127): their casts, listing and lock flags, and
// their large representation (a ControlCreator, the panel a chooser shows for the highlighted one).
#pragma once

#include <map>

#include <string>
#include <utility>
#include <vector>

#include "de/scene.h"

namespace game {

class World;

// DE_CDescription::ObjProcessContents (Core 0x701f14e0): [name][resource passive reference]
// v2+ [BOOL listed +0xbc] v3 [f32 +0xd4][BOOL locked by default +0xc4][BOOL visible when locked
// +0xc8][BOOL +0xd8][BOOL +0xcc][BOOL +0xd0]; ObjProcessReferences v6 (0x701f17a0): [member
// casts +0x34][BOOL icon][BOOL large representation][components][replacements][BOOL medium].
struct DescriptionInfo {
	std::vector<de::ObjectRef> casts;
	de::ObjectRef icon, large, medium;
	bool listed = true;
	bool locked_by_default = false;
	bool visible_when_locked = false;
	bool flag_d8 = false;  // +0xd8: a fighter model description's default model
};
DescriptionInfo read_description(const de::World &data, const de::ObjectRef &description);

// DE_CDescription::IsLocked(id) (Core 0x701f2640): id 0 is locked when locked by default and not
// unlocked since; any other id is locked until unlocked. Unlock (0x70010aa0) adds the id to the
// description's registry value ObjectInfo\<guid>\DLock, the list of unlocked ids.
bool description_locked(const World &w, const de::ObjectRef &description, uint32_t id = 0);
void unlock_description(World &w, const de::ObjectRef &description, uint32_t id = 0);
void lock_description(World &w, const de::ObjectRef &description, uint32_t id, bool unlock);
// Older registry files kept a "DLock <guid> [id]" value per unlocked id.
void migrate_lock_records(std::map<std::string, std::string> &registry);

// DE_CEnvironmentDescription::ObjProcessContents (Creature 0x1500aa20): after the description's
// chunk, v1 [i32 minimum recommended players +0xdc][i32 maximum +0xe0].
std::pair<int, int> environment_players(const de::ObjectRef &environment_description);

// DE_CFighterModelDescription (OMF ObjProcessContents): after the description's chunk, v1 [GUID
// +0xf0], its fighter description. A fighter's models, in data order.
std::vector<de::ObjectRef> fighter_models(const de::World &data, const de::ObjectRef &fighter_description);

// A Value\String's text.
std::string read_string_value(const de::ObjectRef &value);

// DE_CControlCreator (Core: ObjProcessContents 0x7021c270, CreateNewControl 0x7021c520; 2.123
// 0x20224c30, 0x20224ee0): contents v1 [name][i32 layout +0x44][i32 alignment +0x40][i32 text
// height +0x48, virtual pixels][f32 image scale +0x4c]; references, its items in order: a Value\String is a
// wrapped text block of that height, an image is its size times the scale, a ControlCreator nests. Items
// stack downwards, or across for layouts 1 and 3; layouts 2 and 3 put each run of items of one class
// in a row (a column), centred when the alignment is 2. Text lines are centred at alignment 2, else
// at the left.
struct ControlLayout {
	enum Kind { Group, Text, Image } kind = Group;
	int layout = 0;
	int align = 2;
	int text_height = 16;
	float image_scale = 1;
	std::string text;
	de::ObjectRef image;
	std::vector<ControlLayout> items;
	bool across() const { return layout == 1 || layout == 3; }
};
ControlLayout read_control_creator(const de::World &data, const de::ObjectRef &creator);

// A pilot description's resource (its Pilot) and that pilot's photo (DE_CPilot::CurrentPhoto: the
// first Image among its children and properties).
de::ObjectRef pilot_photo(const de::World &data, const de::ObjectRef &pilot_description);

}  // namespace game
