// Robot (HAR) import in the Import dialog (docs/EDITOR_SPEC.md §5.5, editor/core/har_import.h): the
// Source page's robot settings, the Bones, Animations and Hitboxes pages, the preview (the new
// robot beside its base, both skeletons, click a joint to map it) and the commit.
#pragma once

#include <string>

#include "editor.h"
#include "model_import.h"

void robot_source_section(Editor &e, ed::ImportPlan &plan);
void robot_page_bones(Editor &e, ed::ImportPlan &plan);
void robot_page_animations(Editor &e, ed::ImportPlan &plan);
void robot_page_hitboxes(Editor &e, ed::ImportPlan &plan);
bool robot_bones_done(const ed::ImportPlan &plan);

// analyse_robot, then the mapping kept with an earlier import of the same name.
void robot_analyse(Editor &e, ed::ImportPlan &plan);
void robot_preview(Editor &e, ed::ImportPlan &plan, ImVec2 size);
void robot_preview_reset();
// Imports into the open add-on and opens the new HAR; false with *error on failure.
bool robot_commit(Editor &e, ed::ImportPlan &plan, std::string *error);
