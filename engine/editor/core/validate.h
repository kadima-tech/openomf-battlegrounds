// The add-on checks of docs/EDITOR_SPEC.md §7. Rules only the original 2.123 game enforces are
// warnings until the editor's output is tested there (§8).
#pragma once

#include <string>
#include <vector>

#include "addon.h"

namespace ed {

struct Issue {
	enum Severity { Ok, Warning, Error } severity = Ok;
	int rule = 0;      // §7 checklist item, 0 for general notes
	int object = -1;   // add-on object index, when it concerns one
	std::string text;
};

std::vector<Issue> validate(const Project &p);

}  // namespace ed
