// A generated script's view: line numbers, syntax colours, "// <user ...>" blocks tinted, compile
// errors marked, and an edit mode where only the user blocks may change.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "imgui.h"

struct CodeView {
	int jump_line = 0;  // scroll to this line (1-based) on the next draw
	int highlight = 0;  // a line drawn as the selected one
	bool editing = false;
	std::string buffer, error;
};

// Returns true when an edit is applied; *blocks then holds the user blocks' new contents. Without
// blocks the view is read-only.
bool code_view(const char *id, CodeView &v, const std::string &text, ImVec2 size, const std::vector<std::pair<int, std::string>> &errors,
               std::map<std::string, std::string> *blocks);
