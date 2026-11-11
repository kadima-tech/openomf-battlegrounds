// The tournament and HAR editors' hooks into the shell (Content menu, dialogs, start-up options,
// save, validation links), kept apart from editor.cpp.
#pragma once

#include <memory>

#include "editor.h"
#include "script.h"

ed::ScriptCompiler &script_compiler(Editor &e);
// Compiles every add-on script class; logs the result. Returns the number of errors.
int compile_scripts(Editor &e);

// Launch Game with the tournament started at a match (openbg --launch, --start-match).
void launch_tournament(Editor &e, const std::string &name, int match);

void assets_content_menu(Editor &e);
// Play Mode: the add-on's tournaments, each startable at a match (in the game).
void assets_play_menu(Editor &e);
void assets_popups(Editor &e);
bool assets_open(Editor &e, const ObjectRef &r);
void assets_startup(Editor &e);
void assets_before_save(Editor &e);
// A validation issue with a script line: opens the editor that shows that script at the line.
void assets_open_issue(Editor &e, const ed::Issue &issue);

std::unique_ptr<Document> make_tournament_document(Editor &e, const ed::Guid &description);
std::unique_ptr<Document> make_har_document(Editor &e, const ed::Guid &description);
// The open tournament document showing that behavior: jump to a line.
bool tournament_document_jump(Document *d, int behavior, int line, Editor &e);
