// Scripts in an add-on: Behavior objects written from a PCode structure, and Classes compiled with
// the engine's compiler into VMC2 (docs/EDITOR_SPEC.md §7, rules 9-10).
#pragma once

#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "addon.h"
#include "de/pcode.h"

namespace de {
class World;
}

namespace ed {

de::PCode read_pcode(const deo::Object &behavior);
// DE_CPCode::ObjGenerateContents: v5 with import, global (v3), members and method elements (v9).
Bytes write_pcode(const de::PCode &p);

// A method element; `state` methods are entered with RegisterState (id 0), others are instant.
de::PCodeElement pcode_method(const std::string &name, const std::string &source, bool state = false, int parent = -1,
                              bool virtual_ = false);

// The text the editors show: sections "// ---- method <qualified name>", sources with \n.
struct ScriptText {
	std::string text;
	std::vector<int> element_line;  // first source line (1-based) of each element; members at [elements]
	int members_line = 0;
};
ScriptText script_text(const de::PCode &p);

// Replaces a Behavior's script: its source elements, its global names and what they reference
// (an add-on object or a base GUID; zero for none), one empty reference chunk per element, and
// a new revision (value84). The behaviors it imports are kept.
void write_behavior(Project &p, uint32_t behavior, const de::PCode &code, const std::vector<Guid> &global_targets);
// What a Behavior's globals reference, in order.
std::vector<Guid> behavior_global_targets(const Project &p, uint32_t behavior);

struct CompileMessage {
	std::string behavior, element;  // element: the qualified method name, "Members" or empty
	int line = 0;
	std::string text;
	int object = -1;    // the add-on Behavior, when the error is in one
	int text_line = 0;  // the line in script_text() of that Behavior
	std::string str() const;
};
CompileMessage parse_compile_error(const std::string &what);

// Compiles an add-on's Classes against the base game (read once, on first use) plus the add-on.
class ScriptCompiler {
public:
	explicit ScriptCompiler(std::filesystem::path data_dir);
	~ScriptCompiler();
	struct Result {
		bool ok = false;
		std::vector<CompileMessage> errors;
		std::vector<std::string> notes;
	};
	// Writes the VMC2 extended data of the Class when it compiles.
	Result compile(Project &p, uint32_t class_object);
	// Compiles every Class in the add-on that attaches one of its Behaviors.
	std::vector<std::pair<uint32_t, Result>> compile_all(Project &p);
	bool loaded() const;
	// Reads the base game for the compiler on a background thread.
	void preload();

private:
	de::World &world();
	std::filesystem::path dir_;
	std::unique_ptr<de::World> world_;
	mutable std::mutex mutex_;
	std::thread loader_;
};

// The change information a Class's VMC2 must carry now (FINDINGS 431); false when a behavior it
// names is not found.
bool change_information(const Project &p, uint32_t class_object, std::vector<uint8_t> *out);
// The errors of the last compile of each of the project's classes (empty once they compile).
const std::vector<CompileMessage> &last_compile_errors(const Project &p);
bool compiled_once(const Project &p);

// The add-on Classes that attach `behavior` (an add-on object index).
std::vector<uint32_t> classes_attaching(const Project &p, uint32_t behavior);
// Every add-on Class whose behaviors include one of the add-on's own.
std::vector<uint32_t> addon_script_classes(const Project &p);

}  // namespace ed
