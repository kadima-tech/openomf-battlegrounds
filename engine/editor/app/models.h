// 3D models in the shell (docs/EDITOR_SPEC.md §5.5): File > Import... with the Import dialog,
// drag and drop, Export to glTF from the Content Browser and File menu, and the start-up options.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "editor.h"
#include "model_import.h"

// One page of the Import dialog. The steps are a list so an importer can add its own (the HAR
// importer's bone mapping goes between Contents and Review).
struct ImportStep {
	const char *icon = nullptr;
	std::string title;
	std::function<void(Editor &, ed::ImportPlan &)> draw;
	std::function<bool(const ed::ImportPlan &)> shown;  // null: always
	std::function<bool(const ed::ImportPlan &)> done;   // null: always
};
// The dialog's steps, in order; register_import_step inserts before the one titled `before`
// ("" appends before Review).
std::vector<ImportStep> &import_steps();
void register_import_step(ImportStep step, const std::string &before = "Review");

void models_file_menu(Editor &e);
void models_popups(Editor &e);
void models_startup(Editor &e);
// A file dropped on the window: the Import dialog for .glb / .gltf.
void models_drop(Editor &e, const std::filesystem::path &file);
// Export to glTF: a save dialog, then the object as .glb or .gltf.
void models_export_dialog(Editor &e, const ObjectRef &r);
bool models_exportable(Editor &e, const ObjectRef &r);
void models_open_import(Editor &e, const std::filesystem::path &file);
