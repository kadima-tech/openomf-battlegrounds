// The script compiler: what DE_CClass::PrepareForExecution (Core 0x7013abb0) does for a Class the
// game ships without VMC2 -- compile its behaviors' sources (pcode_parser.h) into the VMClass the
// VM runs. Six shipped classes need it (FINDINGS 21/23): Training's, Practice's and Trophy Room's
// sub-sessions, Canyon's boulder pickup (PBoulder), PSlidingText and Flyers' PCarManager.
//
// The native API (each native class's intrinsic signatures) is read from the VMC2 of every class
// the game does ship compiled, so it is exactly what the original compiler offered those scripts.
#pragma once

#include <string>
#include <vector>

#include "de/pcode.h"
#include "de/scene.h"
#include "vm/vmcode.h"

namespace vm {

// The behaviors a class's scripts can see (op 26 numbers them; op 8 numbers their globals): the
// attached behaviors, then -- from the most-derived back to the first -- every behavior they
// reference, depth first, each once.
std::vector<de::ObjectRef> visible_behaviors(const de::World &data, const std::vector<de::ObjectRef> &attached);
std::vector<de::ObjectRef> referenced_behaviors(const de::World &data, const de::ObjectRef &behavior);
std::vector<de::ObjectRef> behavior_globals(const de::World &data, const de::ObjectRef &behavior);

// A behavior's compiled methods (by element index) in the order VMC2 lists them, the order
// interface calls (op 37) number them: its method elements depth first, each followed by the
// methods nested in it, leaving out templates and settings (method flag 2+) and excluded elements
// with what is nested in them.
std::vector<size_t> compiled_elements(const de::PCode &p);

// Compile `cls` (a Class object). Throws pc::CompileError naming the behavior and line; `notes`
// gets what was resolved outside the class (see Compiler::stray_final).
VMClass compile_class(const de::World &data, const de::ObjectRef &cls, std::vector<std::string> *notes = nullptr);

}  // namespace vm
