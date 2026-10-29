// "Behavior" objects (DE_CPCode): script source split into named elements.
// Layout from DE_CPCode::ObjProcessContents (Core 0x70163d90) and the element readers
// DE_CPCodeSourceElement/Import/Global/Method::ProcessContents (0x70164d70..0x70166220).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "de/contents.h"

namespace de {

struct PCodeElement {
	enum Type : int32_t { kMethod = 1, kInclusion = 2, kClass = 3, kImport = 100, kGlobal, kMembers };

	Type type;
	std::string name;
	std::string source;
	uint32_t method_id = 0;  // Method v9: leading u32 (+0x54): 0 a state, 1 instant, 2+ not compiled
	bool virtual_ = false;   // Method: the byte after the source is 0
	bool excluded = false;   // Method: the next byte is set (Templates, Setup: not compiled, nor their children)
	int32_t link = -1;       // trailing per-element index: parent element (nesting), -1 = top level
	std::vector<std::string> globals;  // Global v2+: entry names
};

struct PCode {
	std::string name;
	PCodeElement import, global, members;
	std::vector<PCodeElement> elements;

	static PCode parse(Contents &c);
	// Dotted path through parent links, e.g. "Ground.Attack.LeftPunchHeavy".
	std::string qualified_name(size_t index) const;
	// The whole script as readable text, elements in stored order.
	std::string text() const;
};

}  // namespace de
