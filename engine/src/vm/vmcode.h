// Compiled script code ("VMC2" EX data of Class objects).
// Layout from DE_CClass::LoadVMCode (Core 0x70134530), DE_CClassBehaviorReference::LoadVMCode
// (0x701370d0) and DE_CClassCompiledMethod::LoadVMCode (0x7013de50).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "de/contents.h"
#include "de/objects_file.h"

namespace vm {

struct ClassInstance {
	uint32_t class_index;  // into VMClass::classes (VisibleClass)
	uint32_t value;
	bool flag;
	uint32_t count;
	std::optional<uint32_t> class2;
};

struct CompiledMethod {
	std::string name;       // short signature, e.g. "float Health()"
	std::vector<uint8_t> code;  // instruction stream (the method's VMCode contents)
	bool flags[4];          // +0x84 +0x88 +0x8c +0x90
	std::optional<uint32_t> v34;
	std::vector<uint32_t> base_methods;
	std::vector<ClassInstance> instances;
	struct Interface {
		uint32_t a;
		int32_t b;
		bool c;
	};
	std::vector<Interface> interfaces;
	uint32_t counts[5];     // +0x3c +0x94 +0x40 +0x44 +0x48 (stack/frame sizes, inferred)
	std::string signature;  // qualified, e.g. "BFighterStats:float Health()"
	int32_t v78;
	struct Replicated {
		uint32_t type, a, b;
	};
	std::vector<Replicated> replicated;
};

struct BehaviorCode {
	std::vector<ClassInstance> instances;
	std::vector<std::optional<CompiledMethod>> methods;  // nullopt = not compiled
	std::vector<std::optional<uint32_t>> base_links;
	std::vector<CompiledMethod::Replicated> replicated;
};

struct VMClass {
	std::vector<uint8_t> change_info;
	struct ClassRef {
		std::string name;  // native or script class, e.g. "CActor", "CObject::CVector"
		uint32_t layout_pos;
	};
	std::vector<ClassRef> classes;
	struct Intrinsic {
		uint32_t class_index;
		std::string signature;  // e.g. "class CVector WorldPosition()"
	};
	std::vector<Intrinsic> intrinsics;
	std::vector<uint32_t> ref_members;
	std::vector<BehaviorCode> behaviors;
	// Names resolved at run time (op 39, GetGlobalResolvableReferenceValue). Kind 25 names an
	// animation sequence ("seq" + sequence name); 26 is a related sequence kind (inferred).
	struct GlobalRef {
		uint32_t kind;
		std::string name;
	};
	std::vector<GlobalRef> global_refs;
	// Member behavior interfaces (DE_CClass +0x8c): {behavior global id, data block}.
	std::vector<std::pair<uint32_t, int32_t>> member_interfaces;
	// Final class instances (DE_CClass +0xbc, op 61): class index and the bytes the native class
	// deserialises its value from.
	struct FinalInstance {
		uint32_t class_index;
		std::vector<uint8_t> data;
	};
	std::vector<FinalInstance> finals;
	std::string unknown_final_class;  // set if a final's class layout is not known
	std::vector<uint8_t> tail;        // anything after the finals

	// behavior_count comes from the Class object's references (behaviors attached to it).
	static VMClass parse(const std::vector<uint8_t> &vmc2, size_t behavior_count);
};

// DE_CClass::ObjProcessReferences (0x70135df0): DE_CObject chunk, then a BOOL-terminated
// list of behavior references.
std::vector<de::Reference> class_behavior_refs(const std::vector<uint8_t> &references);

}  // namespace vm
