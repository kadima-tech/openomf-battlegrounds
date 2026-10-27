// Loaded script classes: compiled methods mapped into the VM address space.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "vm/memory.h"
#include "vm/vmcode.h"

namespace vm {

struct ScriptClass;

struct ScriptMethod {
	const CompiledMethod *cm = nullptr;
	ScriptClass *cls = nullptr;
	uint32_t index = 0;      // class method index (position in the class string pool)
	uint32_t code_addr = 0;  // VMCode copied into the arena: op 3 hands out pointers into it
	uint32_t code_size = 0;
	uint32_t handle = 0;     // script-visible value for "method pointers" (op 38)
	ScriptMethod *parent = nullptr;  // enclosing method (+0x14; e.g. Ground for Ground.Stand)
	int32_t vslot = -1;              // virtual method slot (+0x34, stored when flags[2])
	std::vector<ScriptMethod *> nested;  // methods whose parent is this one

	uint32_t frame_size() const { return cm->counts[0]; }  // +0x3c MaxStackSize
	uint32_t param_size() const { return cm->counts[2]; }  // +0x40
	uint32_t return_size() const { return cm->counts[3]; } // +0x44
	bool is_instant() const { return cm->flags[0]; }        // +0x84
	bool is_state() const { return cm->flags[3]; }          // +0x90
};

struct ScriptClass {
	std::string name;       // e.g. "PFighterStats"
	std::string base_path;  // native factory path, e.g. "Actor\\Creature"
	VMClass code;
	std::vector<std::string> pool;          // DE_CStringPool: method signatures by index
	std::vector<ScriptMethod *> by_index;   // GetMethodByIndex
	std::deque<ScriptMethod> methods;

	std::vector<ScriptMethod *> vtable;     // slot -> most-derived method (later behaviors win)
	// Per behavior, its method list as compiled (nullptr where not compiled): what an interface
	// call's method index refers to (DE_CBehavior +0x6c).
	std::vector<std::vector<ScriptMethod *>> behavior_methods;

	ScriptMethod *method(uint32_t index) const { return index < by_index.size() ? by_index[index] : nullptr; }
	ScriptMethod *find(const std::string &signature) const;
	// Virtual dispatch (op 36, SetupVirtualMethodTable 0x70138f10): the running state's own
	// table (CompiledMethod base methods, which include `inherit`); else methods nested in the
	// running state (or its enclosing states) override the class table for their slot.
	ScriptMethod *resolve_virtual(uint32_t slot, const ScriptMethod *running) const;
	// Most-derived method whose name (signature after "Behavior:") matches, e.g.
	// "bool ObjectSpawn(class CVector,class CCoordinateSystem,class CVector)" or "Entry()".
	ScriptMethod *find_by_name(const std::string &name) const;
};

// Build a class from its Class object contents (DE_CClass::ObjProcessContents, Core 0x70135a90)
// and parsed VMC2. Method indices follow DE_CClassCompiledMethod::LoadVMCode: a method's
// signature is looked up in the class string pool and appended if missing.
void load_class(ScriptClass &out, Memory &mem, const std::vector<uint8_t> &contents, VMClass code);

}  // namespace vm
