#include "vm/compiler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#include "de/contents.h"
#include "de/objects_file.h"
#include "de/pcode.h"
#include "vm/pcode_parser.h"

namespace vm {

using pc::CompileError;
using pc::Expr;
using pc::Stmt;

std::vector<de::ObjectRef> referenced_behaviors(const de::World &data, const de::ObjectRef &behavior) {
	std::vector<de::ObjectRef> out;
	try {
		auto d = behavior.file->object_data(behavior.info());
		de::Contents c(d.references);
		de::Chunk obj = de::begin_chunk(c);
		c.seek(obj.end);
		de::begin_chunk(c);
		de::Chunk globals = de::begin_chunk(c);
		c.seek(globals.end);
		de::Chunk pcodes = de::begin_chunk(c);
		while (c.pos() < pcodes.end && c.boolean()) {
			if (de::ObjectRef t = data.resolve(*behavior.file, de::Reference::read(c))) out.push_back(t);
		}
	} catch (const std::exception &) {
	}
	return out;
}

std::vector<de::ObjectRef> behavior_globals(const de::World &data, const de::ObjectRef &behavior) {
	std::vector<de::ObjectRef> out;
	try {
		auto d = behavior.file->object_data(behavior.info());
		de::Contents c(d.references);
		de::Chunk obj = de::begin_chunk(c);
		c.seek(obj.end);
		de::begin_chunk(c);
		de::Chunk globals = de::begin_chunk(c);
		if (globals.version == 2) {
			while (c.pos() < globals.end) out.push_back(c.boolean() ? data.resolve(*behavior.file, de::Reference::read(c)) : de::ObjectRef{});
		}
	} catch (const std::exception &) {
	}
	return out;
}

std::vector<de::ObjectRef> visible_behaviors(const de::World &data, const std::vector<de::ObjectRef> &attached) {
	std::vector<de::ObjectRef> list = attached;
	auto known = [&](const de::ObjectRef &t) {
		return std::any_of(list.begin(), list.end(), [&](const de::ObjectRef &x) { return x && x.file == t.file && x.index == t.index; });
	};
	std::function<void(const de::ObjectRef &)> visit = [&](const de::ObjectRef &beh) {
		for (const de::ObjectRef &t : referenced_behaviors(data, beh)) {
			if (!known(t)) {
				list.push_back(t);
				visit(t);
			}
		}
	};
	for (size_t i = attached.size(); i-- > 0;) {
		if (list[i]) visit(list[i]);
	}
	return list;
}

std::vector<size_t> compiled_elements(const de::PCode &p) {
	auto &els = p.elements;
	std::vector<std::vector<size_t>> kids(els.size());
	std::vector<size_t> roots, out;
	for (size_t i = 0; i < els.size(); ++i) {
		int32_t up = els[i].link;
		if (up >= 0 && size_t(up) < els.size()) kids[size_t(up)].push_back(i);
		else roots.push_back(i);
	}
	std::function<void(size_t)> walk = [&](size_t i) {
		const de::PCodeElement &e = els[i];
		if (e.type != de::PCodeElement::kMethod || e.method_id > 1 || e.excluded) return;
		out.push_back(i);
		for (size_t k : kids[i]) walk(k);
	};
	for (size_t r : roots) walk(r);
	return out;
}

namespace {

// ---------------------------------------------------------------------------------------------
// The native API

std::string short_name(const std::string &full) {
	auto p = full.rfind("::");
	return p == std::string::npos ? full : full.substr(p + 2);
}

struct Sig {
	std::string full;  // as the intrinsic table spells it
	std::string ret;   // "" for none
	std::string name;
	std::vector<std::string> params;
};

Sig parse_sig(const std::string &s) {
	Sig g;
	g.full = s;
	size_t open;
	if (s.rfind("(opp", 0) == 0 || s.find(" (opp") != std::string::npos) {
		size_t at = s.find("(opp");
		size_t close = s.find(')', at);
		g.ret = at ? s.substr(0, at - 1) : "";
		g.name = s.substr(at, close - at + 1);
		open = close + 1;
	} else {
		open = s.find('(');
		std::string head = s.substr(0, open);
		auto sp = head.rfind(' ');
		g.name = sp == std::string::npos ? head : head.substr(sp + 1);
		g.ret = sp == std::string::npos ? "" : head.substr(0, sp);
	}
	std::string inner = s.substr(open + 1, s.size() - open - 2);
	int depth = 0;
	std::string cur;
	for (char c : inner) {
		if (c == '(') ++depth;
		if (c == ')') --depth;
		if (c == ',' && depth == 0) {
			g.params.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) g.params.push_back(cur);
	return g;
}

struct NativeApi {
	std::map<std::string, std::vector<Sig>> by_class;  // full class name -> intrinsics
	std::map<std::string, std::string> full_of;        // short -> full class name
};

const NativeApi &native_api(const de::World &data) {
	static std::mutex m;
	static std::map<const de::World *, NativeApi> cache;
	std::lock_guard<std::mutex> lock(m);
	auto it = cache.find(&data);
	if (it != cache.end()) return it->second;
	NativeApi &api = cache[&data];
	std::map<std::string, std::set<std::string>> seen;
	for (auto &[stem, f] : data.files()) {
		for (uint32_t i = 0; i < f->objects().size(); ++i) {
			auto &o = f->objects()[i];
			if (o.class_path != "Class") continue;
			auto d = f->object_data(o);
			auto vmc2 = f->ex_payload(d, "VMC2");
			if (!vmc2) continue;
			VMClass c;
			try {
				c = VMClass::parse(vmc2->data, class_behavior_refs(d.references).size());
			} catch (const std::exception &) {
				continue;
			}
			for (auto &cr : c.classes) api.full_of.emplace(short_name(cr.name), cr.name);
			for (auto &in : c.intrinsics) {
				const std::string &cls = c.classes[in.class_index].name;
				if (seen[cls].insert(in.signature).second) api.by_class[cls].push_back(parse_sig(in.signature));
			}
		}
	}
	return api;
}

// The native classes' bases (the factories' hierarchy, DE_C<name>Factory::Init chaining to its
// parent's). Classes not listed derive from CObject.
const char *native_parent(const std::string &cls) {
	static const std::map<std::string, const char *> parents = {
	    {"CActor", "CObject"}, {"CAffector", "CActor"}, {"CSphereAffector", "CAffector"}, {"CConeAffector", "CAffector"},
	    {"CBoxAffector", "CAffector"}, {"CMeshAffector", "CAffector"}, {"CCreature", "CActor"}, {"COMF2Fighter", "CCreature"},
	    {"COMF2Projectile", "CCreature"}, {"CEnvironment", "CCreature"}, {"CFaction", "CCreature"}, {"CPilot", "CCreature"},
	    {"CPassiveActorReference", "CActor"}, {"CCamera", "CActor"}, {"CDescription", "CObject"}, {"CCast", "CDescription"},
	    {"CCreatureDescription", "CDescription"}, {"CFighterDescription", "CCreatureDescription"},
	    {"CEnvironmentDescription", "CDescription"}, {"CFactionDescription", "CDescription"}, {"CPilotDescription", "CDescription"},
	    {"CSessionMasterDescription", "CDescription"}, {"CUniqueActorProperty", "CObject"}, {"CMaterial", "CUniqueActorProperty"},
	    {"CSpeaker", "CUniqueActorProperty"}, {"CForce", "CUniqueActorProperty"}, {"CCommonForce", "CForce"},
	    {"CDanger", "CUniqueActorProperty"}, {"CStateManager", "CUniqueActorProperty"},
	    {"CAnimationSequenceManager", "CUniqueActorProperty"}, {"CAnimationStreamPlayer", "CUniqueActorProperty"},
	    {"CAttackTrailer", "CUniqueActorProperty"}, {"CPhysics", "CUniqueActorProperty"}, {"CSimplePhysics", "CPhysics"},
	    {"CBeamSpawner", "CParticleModifier"}, {"CManualParticleSpawner", "CParticleModifier"},
	    {"CRandomBoxSpawner", "CParticleModifier"}, {"CRandomEllipsoidSpawner", "CParticleModifier"},
	    {"CPrimitiveGeneratorMesh", "CMesh"}, {"CBillboardedQuadParticleRenderer", "CMesh"}, {"CStreamParticleRenderer", "CMesh"},
	    {"CRenderTargetImage", "CImage"}, {"CStreamNoise", "CNoise"}, {"CGenericEvent", "CEvent"}, {"COMF2Event", "CGenericEvent"},
	    {"CCollisionEvent", "CEvent"}, {"CStandardSession", "CSession"}, {"CStandardSessionModifier", "CSessionModifier"},
	    {"CStandardSessionSlotModifier", "CSessionModifier"}, {"CStandardClient", "CClient"},
	    {"CGenericDescriptionSelector", "CUIManager"}, {"CChooseFighterUIManager", "CUIManager"}, {"CRulesDisplay", "CUIManager"},
	    {"CJoystickInputManager", "CCreatureController"}, {"CStandardHookupExecutor", "CCommand"}};
	auto it = parents.find(cls);
	if (it != parents.end()) return it->second;
	return cls == "CObject" ? nullptr : "CObject";
}

// The native class a Class runs on (its base factory path, DE_CClass +0x...).
std::string native_of_path(const std::string &path) {
	static const std::map<std::string, std::string> paths = {
	    {"Actor", "CActor"}, {"Actor\\Creature", "CCreature"}, {"Actor\\Creature\\Environment", "CEnvironment"},
	    {"Actor\\Creature\\OMF2Fighter", "COMF2Fighter"}, {"Actor\\Creature\\OMF2Projectile", "COMF2Projectile"},
	    {"Actor\\Creature\\Pilot", "CPilot"}, {"Actor\\Creature\\Faction", "CFaction"},
	    {"Actor\\Affector\\Sphere", "CSphereAffector"}, {"Actor\\Affector\\Cone", "CConeAffector"},
	    {"Actor\\Affector\\Box", "CBoxAffector"}, {"SessionModifier\\StandardSessionMaster", "CStandardSessionModifier"},
	    {"StandardSessionSlotModifier", "CStandardSessionSlotModifier"}};
	auto it = paths.find(path);
	return it != paths.end() ? it->second : "CActor";
}

// The native classes' script constants (DE_CConstantList::NewConstant in the factories' Init:
// Core, Creature and OMF).
struct NativeConstant {
	const char *cls, *name;
	int64_t value;
};
const NativeConstant kNativeConstants[] = {
	    {"CObject", "cgroupUndefined", 1},
	    {"CObject", "cgroupControlForeground", 4},
	    {"CObject", "cgroupControlBackground", 8},
	    {"CObject", "cgroupVisibility", 0x10},
	    {"CObject", "cgroupLightable", 0x20},
	    {"CObject", "cgroupNoise", 0x40},
	    {"CObject", "cgroupNoiseBlocker", 0x80},
	    {"CObject", "cgroupLightBlocker", 0x100},
	    {"CObject", "cgroupMaterial", 0x200},
	    {"CObject", "cgroupForce", 0x400},
	    {"CDescription", "lockIDManualUser", 0xffff},
	    {"CDescription", "lockIDLoaded", 0x10000},
	    {"CDescription", "lockIDCompleted", 0x10001},
	    {"COMF2Manager", "cgroupAttackable", 0x100000},
	    {"COMF2Manager", "cgroupFightingSurface", 0x200000},
	    {"COMF2Manager", "cgroupProjectile", 0x400000},
	    {"COMF2Manager", "cgroupPickup", 0x800000},
	    {"CAttackInfo", "reactionSlideBack", 2},
	    {"CAttackInfo", "reactionPopUpHeadHit", 0x65},
	    {"CAttackInfo", "effectNone", 0},
	    {"CAttackInfo", "effectStandard", 1},
	    {"CAttackInfo", "effectStandardRadiate", 2},
	    {"CAttackInfo", "effectStandardFan", 3},
	    {"CAttackInfo", "effectStandardSmall", 4},
	    {"COMF2Fighter", "maStanding", 0},
	    {"COMF2Fighter", "maWalkingForward", 1},
	    {"COMF2Fighter", "maWalkingForwardTurningLeft", 2},
	    {"COMF2Fighter", "maWalkingForwardTurningRight", 3},
	    {"COMF2Fighter", "maTurningRight", 4},
	    {"COMF2Fighter", "maTurningLeft", 5},
	    {"COMF2Fighter", "maTurningRightFast", 6},
	    {"COMF2Fighter", "maTurningLeftFast", 7},
	    {"COMF2Fighter", "maWalkingBackward", 8},
	    {"COMF2Fighter", "maWalkingBackwardTurningLeft", 9},
	    {"COMF2Fighter", "maWalkingBackwardTurningRight", 10},
	    {"COMF2Fighter", "maRunning", 0xb},
	    {"COMF2Fighter", "maRunningTurningLeft", 0xc},
	    {"COMF2Fighter", "maRunningTurningRight", 0xd},
	    {"COMF2Fighter", "maJumpingTurningLeft", 0xe},
	    {"COMF2Fighter", "maJumpingTurningRight", 0xf},
	    {"COMF2Fighter", "maJumping", 0x10},
	    {"COMF2Fighter", "maJumpingAirForward", 0x11},
	    {"COMF2Fighter", "maJumpingAirBackward", 0x12},
	    {"COMF2Fighter", "maJumpingForward", 0x13},
	    {"COMF2Fighter", "maJumpingBackward", 0x14},
	    {"CPilot", "aiAffinityLeft", 0x4000},
	    {"CPilot", "aiAffinityRight", 0x8000},
	    {"CPilot", "aiAffinityLight", 1},
	    {"CPilot", "aiAffinityMedium", 1},
	    {"CPilot", "aiAffinityHeavy", 2},
	    {"CPilot", "aiAffinityPunch", 4},
	    {"CPilot", "aiAffinityKick", 8},
	    {"CPilot", "aiAffinitySpecial", 0x10},
	    {"CPilot", "aiAffinitySuper", 0x2000},
	    {"CPilot", "aiAffinityMelee", 0x20},
	    {"CPilot", "aiAffinityPreimptiveAttack", 0x40},
	    {"CPilot", "aiAffinityBackAttack", 0x80},
	    {"CPilot", "aiAffinityCounter", 0x100},
	    {"CPilot", "aiAffinityJump", 0x200},
	    {"CPilot", "aiAffinityGrapple", 0x400},
	    {"CPilot", "aiAffinityCarryable", 0x800},
	    {"CPilot", "aiAffinityPowerup", 0x1000},
	    {"CPilot", "aiAffinityMovementUrgency", 0x10000},
	    {"CPilot", "aiAffinityEvade", 0x20000},
	    {"CPilot", "aiAffinityMercy", 0x40000},
	    {"CPilot", "aiAffinityDefendObjective", 0x200000},
	    {"CPilot", "raceHuman", 0},
	    {"CPilot", "raceAlien", 1},
	    {"CPilot", "raceMachine", 2},
	    {"CPilot", "raceCyborg", 3},
	    {"CPilot", "raceAnimal", 4},
	    {"CPilot", "sexMale", 0},
	    {"CPilot", "sexFemale", 1},
	    {"CPilot", "sexUndefined", 2},
	    {"CCommand", "typeNavigation", 2},
	    {"CCommand", "typeLookAt", 4},
	    {"CCommand", "typeObserve", 8},
	    {"CCommand", "typeAttack", 0x10},
	    {"CCommand", "typeAquire", 0x20},
};

const NativeConstant *native_constant(const std::string &name, const std::string &cls = "") {
	for (auto &c : kNativeConstants) {
		if (name == c.name && (cls.empty() || cls == c.cls)) return &c;
	}
	return nullptr;
}

// Embedded native values inside a native value (CAttackInfo's vectors): op 229 makes a pointer
// to them.
std::optional<std::pair<uint32_t, const char *>> native_embedded(const std::string &cls, const std::string &field) {
	if (cls == "CAttackInfo") {
		static const std::map<std::string, uint32_t> f = {{"m_ConnectPoint", 0x7c}, {"m_EffectDirection", 0x88},
		                                                  {"m_EffectDirection2", 0x94}, {"m_TransferedMomentum", 0xa0},
		                                                  {"m_ReelDirection", 0xac}};
		auto it = f.find(field);
		if (it != f.end()) return std::make_pair(it->second, "CObject::CVector");
	}
	if (cls == "CCoordinateSystem") {
		if (field == "AxisX") return std::make_pair(0u, "CObject::CVector");
		if (field == "AxisY") return std::make_pair(12u, "CObject::CVector");
		if (field == "AxisZ") return std::make_pair(24u, "CObject::CVector");
	}
	return std::nullopt;
}

// Script-visible fields of native value classes (their factories' member registrations).
std::optional<uint32_t> native_field(const std::string &cls, const std::string &field) {
	if (cls == "CVector") {
		if (field == "x") return 0;
		if (field == "y") return 4;
		if (field == "z") return 8;
	}
	if (cls == "CAttackInfo") {
		static const std::map<std::string, uint32_t> f = {
		    {"m_ReactionType", 0x44}, {"m_EffectType", 0x48}, {"m_AttackType", 0x4c}, {"m_User", 0x54},
		    {"m_BlockPenetration", 0x58}, {"m_Damage", 0x5c}, {"m_StunDamage", 0x60}, {"m_EnergyDamage", 0x64},
		    {"m_RecoveryTime", 0x68}, {"m_DiminishDamage", 0x6c}, {"m_RetainDamage", 0x70}, {"m_MomentumAdjustAmount", 0x74},
		    {"m_GuideToLastHit", 0x78}, {"m_MaxNumberOfHits", 0xc0}, {"m_Flags", 0xc4}};
		auto it = f.find(field);
		if (it != f.end()) return it->second;
	}
	return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Types

struct Type {
	enum K { None, Void, Bool, Byte, Word, Int, DWord, Float, Str, Obj, Arr, Iface, Enum, MethodRef, ClassLit, BehLit, Null, Exec } k = None;
	std::string name;  // native class (full), behavior, enum
	std::string elem;  // array element type name
	bool numeric() const { return k == Bool || k == Byte || k == Word || k == Int || k == DWord || k == Float || k == Enum; }
	bool integral() const { return numeric() && k != Float; }
};

Type prim(Type::K k) {
	Type t;
	t.k = k;
	return t;
}

uint32_t member_size(const Type &t) {
	if (t.k == Type::Byte) return 1;
	if (t.k == Type::Word) return 2;
	return 4;
}

struct Value {  // a compile-time constant
	Type type;
	int64_t i = 0;
	double f = 0;
	std::string s;
};

// ---------------------------------------------------------------------------------------------
// Code buffer

struct Code {
	std::vector<uint8_t> bytes;
	void i32(int32_t v) {
		uint8_t b[4];
		memcpy(b, &v, 4);
		bytes.insert(bytes.end(), b, b + 4);
	}
	void u16(uint16_t v) {
		bytes.push_back(uint8_t(v));
		bytes.push_back(uint8_t(v >> 8));
	}
	void u8(uint8_t v) { bytes.push_back(v); }
	void op(int32_t o) { i32(o); }
	void op(int32_t o, int32_t a) { i32(o), i32(a); }
	void op(int32_t o, int32_t a, int32_t b) { i32(o), i32(a), i32(b); }
	void op(int32_t o, int32_t a, int32_t b, int32_t c) { i32(o), i32(a), i32(b), i32(c); }
	void op(int32_t o, int32_t a, int32_t b, int32_t c, int32_t d) { i32(o), i32(a), i32(b), i32(c), i32(d); }
	size_t here() const { return bytes.size(); }
	void patch(size_t at, int32_t v) { memcpy(bytes.data() + at, &v, 4); }
};

// ---------------------------------------------------------------------------------------------
// The class being compiled

struct MethodDecl {
	size_t beh = 0;               // attached behavior index
	size_t element = 0;           // PCode element index
	std::string name;             // its own name ("BeginAction")
	std::string qualified;        // pool name prefix for nested methods ("MatchX" / "MatchX.PlayerWon")
	std::string short_sig;        // "bool CarryableRightToss(class ETossType,class CVector)"
	std::string signature;        // "BBoulder:" + qualified + short_sig
	bool state = false, nested = false, virt = false;
	int parent = -1;              // index into methods
	std::vector<Type> params;
	Type ret;                     // None when no rparam
	std::vector<std::string> param_names;
	uint32_t pool = 0;
	int slot = -1;
	std::vector<pc::StmtPtr> body;
};

struct Member {
	std::string name;
	Type type;
	uint32_t offset = 0;  // in the member stack; a ref member's index
	bool iface = false, instance = false;
	bool ref = false;     // an object reference: a ref member (ops 62/64), not member stack bytes
};

struct FinalInst {
	std::string cls;  // full class name
	std::vector<uint8_t> data;
	int index = -1;   // in the class finals table once used
};

struct BehaviorInfo {
	de::ObjectRef ref;
	de::PCode pcode;
	std::vector<std::string> global_names;
	// Compiled methods in element order (not templates or settings): for interface calls.
	std::vector<std::pair<size_t, std::string>> methods;  // element index, short signature
	std::vector<std::vector<Type>> method_params;
	std::vector<Type> method_rets;
	std::vector<bool> method_state;
};

struct Local {
	Type type;
	uint32_t offset = 0;  // frame offset; a local ref's index
	bool constant = false;
	Value value;
	bool ref = false;     // an object reference in a state: a local ref slot (ops 20-22)
};

class Compiler {
public:
	Compiler(const de::World &data, const de::ObjectRef &cls) : data_(data), api_(native_api(data)), cls_(cls) {}

	const std::vector<std::string> &notes() const { return notes_; }

	VMClass run() {
		auto d = cls_.file->object_data(cls_.info());
		read_class_contents(d.contents);
		for (auto &r : class_behavior_refs(d.references)) {
			de::ObjectRef b = data_.resolve(*cls_.file, r);
			if (!b) throw CompileError(cls_.info().name + ": unresolved behavior");
			attached_.push_back(b);
		}
		visible_ = visible_behaviors(data_, attached_);
		for (auto &b : visible_) infos_.push_back(read_behavior(b));
		for (size_t i = 0; i < infos_.size(); ++i) {
			auto refs = behavior_globals(data_, visible_[i]);
			for (size_t g = 0; g < refs.size(); ++g) {
				std::string n = g < infos_[i].global_names.size() ? infos_[i].global_names[g] : "";
				if (!n.empty()) globals_.emplace(n, uint32_t(global_count_ + g));
			}
			global_count_ += uint32_t(refs.size());
		}
		// Enums and finals of every visible behavior; members of the attached ones, laid out from
		// the last behavior to the first.
		for (size_t i = infos_.size(); i-- > 0;) declarations(i, i < attached_.size());
		declare_methods();
		assign_pool_and_slots();
		code_.behaviors.resize(attached_.size());
		for (size_t b = 0; b < attached_.size(); ++b) {
			for (auto &mi : members_instances_[b]) code_.behaviors[b].instances.push_back(mi);
		}
		for (size_t m = 0; m < methods_.size(); ++m) compile_method(m);
		code_.classes.clear();
		for (auto &c : classes_) code_.classes.push_back({c, 0});
		for (auto &f : finals_order_) code_.finals.push_back({class_index_const(f->cls), f->data});
		return std::move(code_);
	}

private:
	const de::World &data_;
	const NativeApi &api_;
	de::ObjectRef cls_;
	std::string self_class_;
	std::vector<std::string> pool_;
	std::vector<de::ObjectRef> attached_, visible_;
	std::vector<BehaviorInfo> infos_;
	std::map<std::string, uint32_t> globals_;
	uint32_t global_count_ = 0;
	std::map<std::string, std::map<std::string, int64_t>> enums_;  // enum type -> values
	std::map<std::string, std::pair<std::string, int64_t>> enum_values_;  // value -> (enum, value)
	std::map<std::string, Value> finals_;
	std::map<std::string, FinalInst> final_insts_;
	std::vector<FinalInst *> finals_order_;
	std::map<std::string, Member> members_;
	uint32_t member_end_ = 0;
	std::map<size_t, std::vector<ClassInstance>> members_instances_;
	std::vector<MethodDecl> methods_;
	std::vector<std::string> classes_;
	std::map<std::string, uint32_t> intrinsic_index_;
	VMClass code_;
	std::map<std::string, int> slots_;  // short signature -> virtual slot
	std::set<std::string> stray_tried_;
	std::vector<std::string> notes_;

	// --- class and behavior data -----------------------------------------------------------

	static std::string text(de::Contents &c) {
		auto b = c.block();
		size_t n = b.size();
		while (n && b[n - 1] == 0) --n;
		return std::string(b.begin(), b.begin() + n);
	}

	void read_class_contents(const std::vector<uint8_t> &contents) {
		de::Contents c(contents);
		de::Chunk ch = de::begin_chunk(c);
		std::string base;
		if (ch.version >= 2) text(c);
		if (ch.version >= 3) {
			base = text(c);
			if (c.u8() == 1) {
				uint32_t n = c.squeezed();
				for (uint32_t i = 0; i < n; ++i) pool_.push_back(text(c));
			}
		}
		self_class_ = api_.full_of.count(native_of_path(base)) ? api_.full_of.at(native_of_path(base)) : native_of_path(base);
	}

	BehaviorInfo read_behavior(const de::ObjectRef &b) {
		BehaviorInfo info;
		info.ref = b;
		auto d = b.file->object_data(b.info());
		de::Contents c(d.contents);
		info.pcode = de::PCode::parse(c);
		info.global_names = info.pcode.global.globals;
		for (size_t i : compiled_elements(info.pcode)) info.methods.emplace_back(i, "");
		return info;
	}

	std::string where(size_t beh, const std::string &what) const { return visible_[beh].info().name + "/" + what; }

	// --- types -------------------------------------------------------------------------------

	bool is_behavior(const std::string &n) const {
		return std::any_of(visible_.begin(), visible_.end(), [&](const de::ObjectRef &b) { return b.info().name == n; });
	}
	int visible_index(const std::string &n) const {
		for (size_t i = 0; i < visible_.size(); ++i) {
			if (visible_[i].info().name == n) return int(i);
		}
		return -1;
	}

	std::optional<Type> type_named(const std::string &n) const {
		static const std::map<std::string, Type::K> prims = {{"int", Type::Int}, {"float", Type::Float}, {"dword", Type::DWord},
		                                                     {"word", Type::Word}, {"byte", Type::Byte}, {"bool", Type::Bool},
		                                                     {"string", Type::Str}, {"char", Type::Byte}, {"short", Type::Word}};
		auto p = prims.find(n);
		if (p != prims.end()) return prim(p->second);
		if (enums_.count(n)) {
			Type t = prim(Type::Enum);
			t.name = n;
			return t;
		}
		std::string sn = short_name(n);
		if (api_.full_of.count(sn) || api_.by_class.count(n) || (sn.size() > 1 && sn[0] == 'C' && isupper(uint8_t(sn[1])))) {
			Type t = prim(Type::Obj);
			t.name = api_.full_of.count(sn) ? api_.full_of.at(sn) : n;
			return t;
		}
		return std::nullopt;
	}

	Type decl_type(const pc::Decl &d, bool array, const std::string &where) const {
		if (d.interface) {
			Type t = prim(Type::Iface);
			t.name = d.type;
			return t;
		}
		auto t = type_named(d.type);
		if (!t) throw CompileError(where + ": unknown type " + d.type);
		if (array) {
			Type a = prim(Type::Arr);
			a.elem = d.type;
			bool prim_elem = t->k != Type::Obj;
			a.name = prim_elem ? "CObject::CSingleDimensionPrimitiveArray" : "CObject::CSingleDimensionReferenceArray";
			return a;
		}
		return *t;
	}

	std::string spell(const Type &t) const {
		switch (t.k) {
		case Type::Int: return "int";
		case Type::Float: return "float";
		case Type::DWord: return "dword";
		case Type::Word: return "word";
		case Type::Byte: return "byte";
		case Type::Bool: return "bool";
		case Type::Str: return "string";
		case Type::Obj: return "class " + short_name(t.name);
		case Type::Enum: return "class " + t.name;
		case Type::Iface: return "BRef " + t.name;
		default: return "?";
		}
	}

	bool derives(std::string cls, const std::string &base) const {
		cls = short_name(cls);
		std::string b = short_name(base);
		for (const char *c = cls.c_str(); c;) {
			if (b == c) return true;
			const char *p = native_parent(c);
			if (!p) break;
			cls = p;
			c = cls.c_str();
		}
		return false;
	}

	// --- declarations --------------------------------------------------------------------------

	std::optional<Value> fold(const Expr &e, const std::map<std::string, Local> *locals = nullptr) const {
		switch (e.kind) {
		case Expr::Int: {
			Value v;
			v.type = prim(Type::Int);
			v.i = e.ival;
			return v;
		}
		case Expr::Float: {
			Value v;
			v.type = prim(Type::Float);
			v.f = e.fval;
			return v;
		}
		case Expr::String: {
			Value v;
			v.type = prim(Type::Str);
			v.s = e.text;
			return v;
		}
		case Expr::Ident: {
			if (locals) {
				auto l = locals->find(e.text);
				if (l != locals->end()) {
					if (l->second.constant) return l->second.value;
					return std::nullopt;
				}
			}
			if (e.text == "true" || e.text == "false") {
				Value v;
				v.type = prim(Type::Bool);
				v.i = e.text == "true";
				return v;
			}
			auto f = finals_.find(e.text);
			if (f != finals_.end()) return f->second;
			auto ev = enum_values_.find(e.text);
			if (ev != enum_values_.end()) {
				Value v;
				v.type = prim(Type::Enum);
				v.type.name = ev->second.first;
				v.i = ev->second.second;
				return v;
			}
			if (members_.count(e.text) || globals_.count(e.text)) return std::nullopt;
			if (const NativeConstant *nc = native_constant(e.text)) {
				Value v;
				v.type = prim(Type::DWord);
				v.i = nc->value;
				return v;
			}
			return std::nullopt;
		}
		case Expr::Scoped: {
			auto p = e.text.rfind("::");
			if (const NativeConstant *nc = native_constant(e.text.substr(p + 2), short_name(e.text.substr(0, p)))) {
				Value v;
				v.type = prim(Type::DWord);
				v.i = nc->value;
				return v;
			}
			return std::nullopt;
		}
		case Expr::Unary: {
			auto a = fold(*e.a, locals);
			if (!a) return std::nullopt;
			if (e.text == "-") {
				if (a->type.k == Type::Float) a->f = -a->f;
				else a->i = -a->i;
				return a;
			}
			if (e.text == "!" && a->type.integral()) {
				a->i = !a->i;
				return a;
			}
			return std::nullopt;
		}
		case Expr::Binary: {
			auto a = fold(*e.a, locals), b = fold(*e.b, locals);
			if (!a || !b || !a->type.numeric() || !b->type.numeric()) return std::nullopt;
			bool fl = a->type.k == Type::Float || b->type.k == Type::Float;
			double x = a->type.k == Type::Float ? a->f : double(a->i), y = b->type.k == Type::Float ? b->f : double(b->i);
			Value v;
			v.type = fl ? prim(Type::Float) : a->type;
			const std::string &o = e.text;
			if (o == "+") fl ? v.f = x + y : v.i = a->i + b->i;
			else if (o == "-") fl ? v.f = x - y : v.i = a->i - b->i;
			else if (o == "*") fl ? v.f = x * y : v.i = a->i * b->i;
			else if (o == "/") {
				if (fl) v.f = y != 0 ? x / y : 0;
				else v.i = b->i ? a->i / b->i : 0;
			} else if (o == "|" && !fl) v.i = a->i | b->i;
			else if (o == "&" && !fl) v.i = a->i & b->i;
			else if (o == "<<" && !fl) v.i = a->i << b->i;
			else return std::nullopt;
			return v;
		}
		case Expr::Cast: {
			auto a = fold(*e.a, locals);
			auto t = type_named(e.text);
			if (!a || !t || !t->numeric()) return std::nullopt;
			if (t->k == Type::Float && a->type.k != Type::Float) a->f = double(a->i);
			if (t->k != Type::Float && a->type.k == Type::Float) a->i = int64_t(a->f);
			a->type = *t;
			return a;
		}
		default:
			return std::nullopt;
		}
	}

	void declarations(size_t bi, bool attached) {
		auto &pcode = infos_[bi].pcode;
		std::string w = where(bi, "Members");
		std::vector<pc::StmtPtr> stmts;
		try {
			stmts = pc::parse(pcode.members.source, w);
		} catch (const CompileError &) {
			if (attached) throw;
			return;  // a referenced behavior's members we cannot read: its names stay unknown
		}
		for (auto &s : stmts) {
			if (s->kind == Stmt::Enum) {
				int64_t next = 0;
				for (auto &[n, v] : s->enm->values) {
					if (v) {
						if (auto f = fold(*v)) next = f->type.k == Type::Float ? int64_t(f->f) : f->i;
					}
					enums_[s->enm->name][n] = next;
					enum_values_[n] = {s->enm->name, next};
					++next;
				}
			}
		}
		for (auto &s : stmts) {
			if (s->kind == Stmt::DeclS) {
				auto &d = *s->decl;
				for (auto &dc : d.names) {
					auto dt = type_named(d.type);
					bool class_final = d.final_ && dt && dt->k == Type::Obj;
					if (d.final_ && !d.instance && !class_final) {
						if (!dc.init) continue;
						auto v = fold(*dc.init);
						if (!v) continue;
						if (auto t = type_named(d.type); t && t->numeric() && v->type.numeric()) {
							if (t->k == Type::Float && v->type.k != Type::Float) v->f = double(v->i), v->type = *t;
							else if (t->k != Type::Float && v->type.k == Type::Float) v->i = int64_t(v->f), v->type = *t;
							else v->type = *t;
						}
						finals_.emplace(dc.name, *v);
						continue;
					}
					if (d.final_ && (d.instance || class_final)) {
						auto t = type_named(d.type);
						if (!t) continue;
						FinalInst fi;
						fi.cls = t->name;
						if (dc.init && dc.init->kind == Expr::String) fi.data = string_instance(fi.cls, dc.init->text);
						else fi.data = default_instance(fi.cls);
						final_insts_.emplace(dc.name, std::move(fi));
						continue;
					}
					if (!attached) continue;
					Member m;
					m.name = dc.name;
					m.type = decl_type(d, dc.array, w);
					m.iface = d.interface;
					m.instance = d.instance || dc.array;
					if (m.type.k == Type::Obj && !m.iface && !m.instance && !is_value_class(m.type.name)) {
						m.ref = true;
						m.offset = uint32_t(code_.ref_members.size());
						code_.ref_members.push_back(class_index(m.type.name));
						members_.emplace(m.name, m);
						continue;
					}
					uint32_t size = m.iface || m.instance ? 4 : member_size(m.type);
					member_end_ = (member_end_ + size - 1) / size * size;
					m.offset = member_end_;
					member_end_ += size;
					if (dc.array) {
						Type et = type_named(dc.array ? d.type : d.type).value_or(prim(Type::Int));
						members_instances_[bi].push_back({class_index(m.type.name), m.offset, false, et.k == Type::Obj ? 4u : member_size(et), {}});
					} else if (d.instance && !d.interface) {
						members_instances_[bi].push_back({class_index(m.type.name), m.offset, false, 0, {}});
					}
					if (m.iface) {
						int vi = visible_index(m.type.name);
						code_.member_interfaces.push_back({m.offset, vi < 0 ? 0 : vi});
					}
					members_.emplace(m.name, m);
				}
			} else if (s->kind == Stmt::ExprS) {
				try {
					final_initialiser(*s->e, w);
				} catch (const CompileError &) {
					if (attached) throw;
				}
			}
		}
	}

	std::vector<uint8_t> string_instance(const std::string &cls, const std::string &s) const {
		std::vector<uint8_t> out;
		if (cls == "CObject::CText") {
			uint32_t n = uint32_t(s.size() + 1);
			if (n < 0x80) out.push_back(uint8_t(n));
			else out.push_back(uint8_t(0x80 | (n >> 8))), out.push_back(uint8_t(n));
		} else {
			uint32_t n = uint32_t(s.size() + 1);
			out.resize(4);
			memcpy(out.data(), &n, 4);
		}
		out.insert(out.end(), s.begin(), s.end());
		out.push_back(0);
		return out;
	}

	static std::vector<uint8_t> floats(const std::vector<float> &v) {
		std::vector<uint8_t> out(v.size() * 4);
		memcpy(out.data(), v.data(), out.size());
		return out;
	}

	std::vector<uint8_t> default_instance(const std::string &cls) const {
		if (cls == "CObject::CCoordinateSystem") return floats({1, 0, 0, 0, 1, 0, 0, 0, 1});
		if (cls == "CObject::CVector") return floats({0, 0, 0});
		if (cls == "CObject::CColorRGBA") return floats({0, 0, 0, 0});
		return {};
	}

	// A final instance's set-up statement in a members section (m_Offset.Set(0,0,1.3);): run now,
	// its value becomes the final's data.
	void final_initialiser(const Expr &e, const std::string &w) {
		if (e.kind != Expr::Call || !e.a || e.a->kind != Expr::Member || e.a->a->kind != Expr::Ident) return;
		auto it = final_insts_.find(e.a->a->text);
		if (it == final_insts_.end()) return;
		FinalInst &fi = it->second;
		std::vector<double> args;
		for (auto &a : e.args) {
			auto v = fold(*a);
			if (!v || !v->type.numeric()) throw CompileError(w + ": final initialiser needs constants");
			args.push_back(v->type.k == Type::Float ? v->f : double(v->i));
		}
		const std::string &m = e.a->text;
		std::vector<float> f(fi.data.size() / 4);
		memcpy(f.data(), fi.data.data(), f.size() * 4);
		if ((fi.cls == "CObject::CVector" || fi.cls == "CObject::CColorRGBA") && m == "Set" && args.size() == f.size()) {
			for (size_t i = 0; i < f.size(); ++i) f[i] = float(args[i]);
		} else if (fi.cls == "CObject::CCoordinateSystem" && args.size() == 1 && (m == "RotateXDeg" || m == "RotateYDeg" || m == "RotateZDeg")) {
			int axis = m[6] - 'X';
			float a = float(args[0]) * 0.017453292f, c = std::cos(a), s = std::sin(a);
			for (int i = 0; i < 3; ++i) {
				float x = f[size_t(i * 3)], y = f[size_t(i * 3 + 1)], z = f[size_t(i * 3 + 2)];
				float r[3];
				if (axis == 0) r[0] = x, r[1] = y * c - z * s, r[2] = y * s + z * c;
				else if (axis == 1) r[0] = x * c + z * s, r[1] = y, r[2] = -x * s + z * c;
				else r[0] = x * c - y * s, r[1] = x * s + y * c, r[2] = z;
				for (int k = 0; k < 3; ++k) f[size_t(i * 3 + k)] = r[k];
			}
		} else {
			throw CompileError(w + ": unsupported final initialiser " + m);
		}
		fi.data = floats(f);
	}

	// Method headers: every attached behavior's methods (elements in order), their parameters
	// and their pool signatures; and the visible behaviors' method lists for interface calls.
	void declare_methods() {
		for (size_t bi = 0; bi < infos_.size(); ++bi) {
			auto &info = infos_[bi];
			auto &els = info.pcode.elements;
			std::map<size_t, int> method_of;
			for (auto &[ei, sig] : info.methods) {
				MethodDecl m;
				m.beh = bi;
				m.element = ei;
				m.name = els[ei].name;
				m.state = els[ei].method_id == 0;
				std::string w = where(bi, info.pcode.qualified_name(ei));
				try {
					m.body = pc::parse(els[ei].source, w);
					for (auto &s : m.body) {
						if (s->kind != Stmt::DeclS || !(s->decl->param || s->decl->rparam)) continue;
						for (auto &dc : s->decl->names) {
							Type t = decl_type(*s->decl, dc.array, w);
							if (s->decl->rparam) m.ret = t;
							else m.params.push_back(t), m.param_names.push_back(dc.name);
						}
					}
				} catch (const CompileError &) {
					if (bi < attached_.size()) throw;
				}
				std::string params;
				for (size_t i = 0; i < m.params.size(); ++i) params += (i ? "," : "") + spell(m.params[i]);
				m.short_sig = (m.ret.k != Type::None ? spell(m.ret) + " " : "") + m.name + "(" + params + ")";
				if (els[ei].link >= 0) {
					m.nested = true;
					std::vector<std::string> chain;
					for (int32_t up = els[ei].link; up >= 0 && size_t(up) < els.size(); up = els[size_t(up)].link) chain.push_back(els[size_t(up)].name);
					std::reverse(chain.begin(), chain.end());
					for (size_t i = 0; i < chain.size(); ++i) m.qualified += (i ? "." : "") + chain[i];
				}
				m.signature = info.ref.info().name + ":" + m.qualified + m.short_sig;
				sig = m.short_sig;
				info.method_params.push_back(m.params);
				info.method_rets.push_back(m.ret);
				info.method_state.push_back(m.state);
				if (bi < attached_.size()) {
					method_of[ei] = int(methods_.size());
					methods_.push_back(std::move(m));
				}
			}
			for (auto &[ei, mi] : method_of) {
				if (els[ei].link >= 0) {
					auto p = method_of.find(size_t(els[ei].link));
					if (p != method_of.end()) methods_[size_t(mi)].parent = p->second;
				}
			}
		}
	}

	// Pool indices as load_class assigns them (the class's pool, then each compiled method's
	// signature appended in order when missing); virtual methods (the element's flag) share a
	// slot per signature, numbered in order of appearance.
	void assign_pool_and_slots() {
		for (auto &m : methods_) {
			auto it = std::find(pool_.begin(), pool_.end(), m.signature);
			if (it == pool_.end()) {
				pool_.push_back(m.signature);
				it = pool_.end() - 1;
			}
			m.pool = uint32_t(it - pool_.begin());
		}
		for (auto &m : methods_) {
			if (m.state || !infos_[m.beh].pcode.elements[m.element].virtual_) continue;
			m.virt = true;
			auto s = slots_.find(m.short_sig);
			if (s == slots_.end()) s = slots_.emplace(m.short_sig, int(slots_.size())).first;
			m.slot = s->second;
		}
	}

	uint32_t class_index(const std::string &full) {
		auto it = std::find(classes_.begin(), classes_.end(), full);
		if (it != classes_.end()) return uint32_t(it - classes_.begin());
		classes_.push_back(full);
		return uint32_t(classes_.size() - 1);
	}
	uint32_t class_index_const(const std::string &full) { return class_index(full); }

	uint32_t intrinsic(const std::string &cls, const std::string &sig) {
		std::string key = cls + "|" + sig;
		auto it = intrinsic_index_.find(key);
		if (it != intrinsic_index_.end()) return it->second;
		uint32_t ci = class_index(cls);
		code_.intrinsics.push_back({ci, sig});
		uint32_t idx = uint32_t(code_.intrinsics.size() - 1);
		intrinsic_index_[key] = idx;
		return idx;
	}

	int final_index(FinalInst &fi) {
		if (fi.index < 0) {
			fi.index = int(finals_order_.size());
			finals_order_.push_back(&fi);
		}
		return fi.index;
	}

	// --- methods -----------------------------------------------------------------------------

	struct Ctx {
		MethodDecl *m = nullptr;
		Code code;
		std::map<std::string, Local> locals;
		uint32_t top = 0;  // first free frame byte after the locals
		uint32_t max = 0;
		std::vector<ClassInstance> instances;
		std::vector<CompiledMethod::Interface> interfaces;
		std::vector<std::vector<size_t>> breaks, continues;
		std::string where;
		uint32_t local_refs = 0;
	};
	Ctx *c_ = nullptr;

	[[noreturn]] void fail(const Expr &e, const std::string &m) const {
		throw CompileError(c_->where + ":" + std::to_string(e.line) + ": " + m);
	}
	void use(uint32_t end) { c_->max = std::max(c_->max, end); }

	void hoist(const pc::Decl &d) {
		for (auto &dc : d.names) {
			if (c_->locals.count(dc.name)) continue;  // redeclared (int t after float t): one slot
			Local l;
			if (d.final_ && !d.instance && dc.init) {
				if (auto v = fold(*dc.init, &c_->locals)) {
					l.constant = true;
					l.value = *v;
					if (auto t = type_named(d.type); t && t->numeric() && v->type.numeric()) {
						if (t->k == Type::Float && v->type.k != Type::Float) l.value.f = double(v->i);
						if (t->k != Type::Float && v->type.k == Type::Float) l.value.i = int64_t(v->f);
						l.value.type = *t;
					}
					l.type = l.value.type;
					c_->locals.emplace(dc.name, l);
					continue;
				}
			}
			l.type = decl_type(d, dc.array, c_->where);
			if (c_->m->state && l.type.k == Type::Obj && !d.instance && !d.interface && !is_value_class(l.type.name)) {
				l.ref = true;
				l.offset = c_->local_refs++;
				c_->locals.emplace(dc.name, l);
				continue;
			}
			l.offset = c_->top;
			c_->top += 4;
			if (d.instance && !d.interface) c_->instances.push_back({class_index(l.type.name), l.offset, false, 0, {}});
			if (d.interface) c_->interfaces.push_back({l.offset, visible_index(l.type.name), false});
			c_->locals.emplace(dc.name, l);
		}
	}

	void hoist_all(const std::vector<pc::StmtPtr> &list) {
		for (auto &s : list) hoist_stmt(*s);
	}
	void hoist_stmt(const Stmt &s) {
		if (s.kind == Stmt::DeclS && !s.decl->param && !s.decl->rparam) hoist(*s.decl);
		if (s.init) hoist_stmt(*s.init);
		if (s.body) hoist_stmt(*s.body);
		if (s.other) hoist_stmt(*s.other);
		for (auto &x : s.list) hoist_stmt(*x);
	}

	void compile_method(size_t mi) {
		MethodDecl &m = methods_[mi];
		Ctx ctx;
		ctx.m = &m;
		ctx.where = where(m.beh, infos_[m.beh].pcode.qualified_name(m.element));
		c_ = &ctx;
		uint32_t ret_size = m.ret.k != Type::None ? 4 : 0;
		// rparam at 0, then the params, then every other declaration of the body.
		for (auto &s : m.body) {
			if (s->kind != Stmt::DeclS || !s->decl->rparam) continue;
			for (auto &dc : s->decl->names) ctx.locals[dc.name] = {m.ret, 0, false, {}};
		}
		ctx.top = ret_size;
		for (size_t i = 0; i < m.params.size(); ++i) {
			Local l{m.params[i], ctx.top, false, {}};
			ctx.locals[m.param_names[i]] = l;
			ctx.top += 4;
		}
		uint32_t params_end = ctx.top;
		hoist_all(m.body);
		ctx.code.op(1, 0, int32_t(params_end - ret_size));
		if (ctx.local_refs) {
			ctx.code.op(20);
			ctx.code.u16(uint16_t(ctx.local_refs));
		}
		use(ctx.top);
		for (auto &s : m.body) stmt(*s);
		uint32_t frame = (ctx.max + 7) / 8 * 8 + 8;
		ctx.code.patch(4, int32_t(frame));

		CompiledMethod cm;
		cm.name = m.short_sig;
		cm.code = std::move(ctx.code.bytes);
		cm.flags[0] = !m.state;
		cm.flags[1] = !m.virt;
		cm.flags[2] = m.virt;
		cm.flags[3] = m.state;
		if (m.virt) cm.v34 = uint32_t(m.slot);
		cm.instances = std::move(ctx.instances);
		cm.interfaces = std::move(ctx.interfaces);
		cm.counts[0] = frame;
		cm.counts[1] = ctx.local_refs;
		cm.counts[2] = params_end - ret_size;
		cm.counts[3] = ret_size;
		cm.counts[4] = 4;
		cm.signature = m.signature;
		cm.v78 = -1;
		if (m.state) cm.base_methods = state_table(mi);
		auto &bc = code_.behaviors[m.beh];
		bc.methods.push_back(std::move(cm));
		if (m.parent >= 0) bc.base_links.push_back(methods_[size_t(m.parent)].pool);
		else bc.base_links.push_back(std::nullopt);
		c_ = nullptr;
	}

	// A state's virtual table: per slot, the method its nested methods (or the states it
	// inherits, or the states enclosing it) put there, else the class's most-derived top-level one.
	std::vector<uint32_t> state_table(size_t mi) {
		std::vector<uint32_t> table(slots_.size(), 0);
		std::vector<bool> set(slots_.size(), false);
		for (auto &m : methods_) {
			if (m.virt && !m.nested) table[size_t(m.slot)] = m.pool, set[size_t(m.slot)] = true;
		}
		std::function<void(size_t, int)> apply = [&](size_t s, int depth) {
			if (depth > 8) return;
			const MethodDecl &st = methods_[s];
			if (st.parent >= 0) apply(size_t(st.parent), depth + 1);
			for (auto &x : st.body) {
				if (x->kind != Stmt::Inherit) continue;
				if (int t = find_state(x->name, st.beh); t >= 0) apply(size_t(t), depth + 1);
			}
			for (auto &m : methods_) {
				if (m.virt && m.parent == int(s)) table[size_t(m.slot)] = m.pool;
			}
		};
		apply(mi, 0);
		return table;
	}

	int find_state(const std::string &name, size_t beh) const {
		int found = -1;
		for (size_t i = 0; i < methods_.size(); ++i) {
			const MethodDecl &m = methods_[i];
			std::string q = m.qualified.empty() ? m.name : m.qualified + "." + m.name;
			if (m.state && (q == name || (!m.nested && m.name == name))) {
				if (m.beh == beh) return int(i);
				found = int(i);
			}
		}
		return found;
	}

	// --- statements --------------------------------------------------------------------------

	uint32_t temp() const { return c_->top; }

	void jump_if_false(uint32_t slot, std::vector<size_t> &patches) {
		c_->code.op(109, int32_t(slot), 0);
		patches.push_back(c_->code.here() - 4);
	}
	void jump(std::vector<size_t> &patches) {
		c_->code.op(112, 0);
		patches.push_back(c_->code.here() - 4);
	}
	void land(std::vector<size_t> &patches) {
		for (size_t p : patches) c_->code.patch(p, int32_t(c_->code.here()));
		patches.clear();
	}

	void condition(const Expr &e, uint32_t t) {
		Type ty = gen(e, t);
		if (ty.k == Type::Iface) c_->code.op(88, int32_t(t));
		else if (ty.k == Type::Float) {
			// A float tests non-zero: compare with 0.0.
			c_->code.op(2, int32_t(t + 8), 4);
			c_->code.i32(0);
			c_->code.op(118, int32_t(t), int32_t(t + 8));
			c_->code.op(115, int32_t(t));
			use(t + 12);
		}
	}

	void stmt(const Stmt &s) {
		Code &code = c_->code;
		switch (s.kind) {
		case Stmt::Empty:
		case Stmt::Inherit:
		case Stmt::Enum:
			return;
		case Stmt::DeclS: {
			if (s.decl->param || s.decl->rparam || s.decl->final_) return;
			for (auto &dc : s.decl->names) {
				if (!dc.init) continue;
				auto e = std::make_unique<Expr>();
				e->kind = Expr::Ident;
				e->text = dc.name;
				e->line = s.line;
				assign(*e, "=", *dc.init);
			}
			return;
		}
		case Stmt::ExprS:
			expr_stmt(*s.e);
			return;
		case Stmt::Block:
			for (auto &x : s.list) stmt(*x);
			return;
		case Stmt::If: {
			std::vector<size_t> to_else, to_end;
			condition(*s.cond, temp());
			jump_if_false(temp(), to_else);
			stmt(*s.body);
			if (s.other) {
				jump(to_end);
				land(to_else);
				stmt(*s.other);
				land(to_end);
			} else {
				land(to_else);
			}
			return;
		}
		case Stmt::While:
		case Stmt::For: {
			if (s.init) stmt(*s.init);
			size_t top = code.here();
			std::vector<size_t> to_end;
			if (s.cond) {
				condition(*s.cond, temp());
				jump_if_false(temp(), to_end);
			}
			c_->breaks.emplace_back();
			c_->continues.emplace_back();
			stmt(*s.body);
			land(c_->continues.back());
			if (s.step) expr_stmt(*s.step);
			code.op(112, int32_t(top));
			land(to_end);
			land(c_->breaks.back());
			c_->breaks.pop_back();
			c_->continues.pop_back();
			return;
		}
		case Stmt::DoWhile: {
			size_t top = code.here();
			c_->breaks.emplace_back();
			c_->continues.emplace_back();
			stmt(*s.body);
			land(c_->continues.back());
			std::vector<size_t> to_end;
			condition(*s.cond, temp());
			jump_if_false(temp(), to_end);
			code.op(112, int32_t(top));
			land(to_end);
			land(c_->breaks.back());
			c_->breaks.pop_back();
			c_->continues.pop_back();
			return;
		}
		case Stmt::Switch: {
			uint32_t t = temp();
			Type ty = gen(*s.cond, t);
			(void)ty;
			std::vector<size_t> to_body(s.cases.size()), to_end;
			std::vector<size_t> case_jumps;
			for (size_t i = 0; i < s.cases.size(); ++i) {
				std::vector<size_t> skip;
				c_->code.op(5, int32_t(t + 8), int32_t(t), 4);
				gen(*s.cases[i].second, t + 16);
				c_->code.op(118, int32_t(t + 8), int32_t(t + 16));
				use(t + 20);
				jump_if_false(t + 8, skip);
				code.op(112, 0);
				case_jumps.push_back(code.here() - 4);
				land(skip);
			}
			std::vector<size_t> to_default;
			jump(to_default);
			c_->breaks.emplace_back();
			for (size_t i = 0; i < s.list.size(); ++i) {
				for (size_t k = 0; k < s.cases.size(); ++k) {
					if (s.cases[k].first == i) code.patch(case_jumps[k], int32_t(code.here()));
				}
				if (s.default_at == int64_t(i)) land(to_default);
				stmt(*s.list[i]);
			}
			for (size_t k = 0; k < s.cases.size(); ++k) {
				if (s.cases[k].first >= s.list.size()) code.patch(case_jumps[k], int32_t(code.here()));
			}
			land(to_default);
			land(c_->breaks.back());
			c_->breaks.pop_back();
			return;
		}
		case Stmt::Return:
			if (s.e) {
				if (c_->m->ret.k == Type::None) throw CompileError(c_->where + ":" + std::to_string(s.line) + ": return value without rparam");
				Type t = gen(*s.e, temp());
				convert(t, c_->m->ret, temp());
				code.op(5, 0, int32_t(temp()), 4);
			}
			code.op(319);
			return;
		case Stmt::Break:
			if (c_->breaks.empty()) throw CompileError(c_->where + ": break outside a loop");
			jump(c_->breaks.back());
			return;
		case Stmt::Continue:
			if (c_->continues.empty()) throw CompileError(c_->where + ": continue outside a loop");
			jump(c_->continues.back());
			return;
		}
	}

	void expr_stmt(const Expr &e) {
		switch (e.kind) {
		case Expr::Assign:
			assign(*e.a, e.text, *e.b);
			return;
		case Expr::PostInc:
		case Expr::PreInc:
		case Expr::PostDec:
		case Expr::PreDec:
			incdec(*e.a, e.kind == Expr::PostInc || e.kind == Expr::PreInc);
			return;
		default:
			gen(e, temp());
		}
	}

	// --- expressions -------------------------------------------------------------------------

	void convert(const Type &from, const Type &to, uint32_t at) {
		Code &code = c_->code;
		if (to.k == Type::Float && from.k != Type::Float && from.numeric()) {
			if (from.k == Type::DWord) code.op(73, int32_t(at));
			else if (from.k == Type::Word) code.op(74, int32_t(at));
			else if (from.k == Type::Byte) code.op(76, int32_t(at));
			else code.op(72, int32_t(at));
		} else if (to.k != Type::Float && to.numeric() && from.k == Type::Float) {
			if (to.k == Type::Word) code.op(93, int32_t(at));
			else if (to.k == Type::Byte) code.op(103, int32_t(at));
			else code.op(82, int32_t(at));
		} else if ((to.k == Type::Int || to.k == Type::DWord || to.k == Type::Bool || to.k == Type::Enum) && from.k == Type::Word) {
			code.op(78, int32_t(at));
		} else if ((to.k == Type::Int || to.k == Type::DWord || to.k == Type::Bool || to.k == Type::Enum) && from.k == Type::Byte) {
			code.op(80, int32_t(at));
		}
	}

	void constant(const Value &v, uint32_t dst) {
		Code &code = c_->code;
		if (v.type.k == Type::Str) {
			std::string s = v.s;
			for (size_t p; (p = s.find("<Return>")) != std::string::npos;) s.replace(p, 8, "\n");
			code.op(3, int32_t(dst), int32_t(s.size() + 1));
			code.bytes.insert(code.bytes.end(), s.begin(), s.end());
			code.u8(0);
		} else {
			code.op(2, int32_t(dst), 4);
			if (v.type.k == Type::Float) {
				float f = float(v.f);
				int32_t b;
				memcpy(&b, &f, 4);
				code.i32(b);
			} else {
				code.i32(int32_t(v.i));
			}
		}
		use(dst + 4);
	}

	struct Place {  // where a name lives
		enum K { None, Local, Member, Global, Final, Constant, Method, Self } k = None;
		Type type;
		uint32_t offset = 0;
		struct Member *member = nullptr;
		Value value;
		int method = -1;
		bool ref = false;
	};

	Place lookup(const std::string &name) {
		Place p;
		if (name[0] == '.') {
			if (int mi = find_method(name.substr(1)); mi >= 0) {
				p.k = Place::Method;
				p.method = mi;
				p.type = prim(Type::MethodRef);
			}
			return p;
		}
		const std::string &n = name;
		auto l = c_->locals.find(n);
		if (l != c_->locals.end()) {
			if (l->second.constant) {
				p.k = Place::Constant;
				p.value = l->second.value;
				p.type = p.value.type;
				return p;
			}
			p.k = Place::Local;
			p.type = l->second.type;
			p.offset = l->second.offset;
			p.ref = l->second.ref;
			return p;
		}
		auto m = members_.find(n);
		if (m != members_.end()) {
			p.k = Place::Member;
			p.member = &m->second;
			p.type = m->second.type;
			p.offset = m->second.offset;
			p.ref = m->second.ref;
			return p;
		}
		if (n == "true" || n == "false") {
			p.k = Place::Constant;
			p.value.type = prim(Type::Bool);
			p.value.i = n == "true";
			p.type = p.value.type;
			return p;
		}
		if (n == "this") {
			p.k = Place::Self;
			p.type = prim(Type::Obj);
			p.type.name = self_class_;
			return p;
		}
		auto f = finals_.find(n);
		if (f != finals_.end()) {
			p.k = Place::Constant;
			p.value = f->second;
			p.type = p.value.type;
			return p;
		}
		auto ev = enum_values_.find(n);
		if (ev != enum_values_.end()) {
			p.k = Place::Constant;
			p.value.type = prim(Type::Enum);
			p.value.type.name = ev->second.first;
			p.value.i = ev->second.second;
			p.type = p.value.type;
			return p;
		}
		if (const NativeConstant *nc = native_constant(n)) {
			p.k = Place::Constant;
			p.value.type = prim(Type::DWord);
			p.value.i = nc->value;
			p.type = p.value.type;
			return p;
		}
		auto g = globals_.find(n);
		if (g != globals_.end()) {
			p.k = Place::Global;
			p.offset = g->second;
			p.type = prim(Type::Obj);
			p.type.name = "CObject";
			return p;
		}
		auto fi = final_insts_.find(n);
		if (fi != final_insts_.end()) {
			p.k = Place::Final;
			p.offset = uint32_t(final_index(fi->second));
			p.type = prim(Type::Obj);
			p.type.name = fi->second.cls;
			return p;
		}
		if (int mi = find_method(n); mi >= 0) {
			p.k = Place::Method;
			p.method = mi;
			p.type = prim(Type::MethodRef);
			return p;
		}
		if (stray_final(n)) return lookup(n);
		return p;
	}

	// A final no visible behavior declares, found in another behavior of the data: Training's,
	// Practice's and Trophy Room's sub-sessions use the tournaments' m_TournamentAIDifficulty (their
	// masters predate it), PBoulder's BPickup a newer BOMFEvents' g_OMFQuerryBestPosition. The first
	// declaration in file order is taken.
	bool stray_final(const std::string &n) {
		if (stray_tried_.count(n)) return false;
		stray_tried_.insert(n);
		for (auto &[stem, f] : data_.files()) {
			for (uint32_t i = 0; i < f->objects().size(); ++i) {
				const de::ObjectInfo &o = f->objects()[i];
				if (o.class_path != "Behavior") continue;
				de::PCode pcode;
				try {
					de::Contents c(f->object_data(o).contents);
					pcode = de::PCode::parse(c);
				} catch (const std::exception &) {
					continue;
				}
				if (pcode.members.source.find(n) == std::string::npos) continue;
				std::vector<pc::StmtPtr> stmts;
				try {
					stmts = pc::parse(pcode.members.source, o.name);
				} catch (const CompileError &) {
					continue;
				}
				for (auto &st : stmts) {
					if (st->kind != Stmt::DeclS || !st->decl->final_) continue;
					auto &d = *st->decl;
					for (auto &dc : d.names) {
						if (dc.name != n) continue;
						auto dt = type_named(d.type);
						if (dt && dt->k == Type::Obj) {
							FinalInst fi;
							fi.cls = dt->name;
							fi.data = dc.init && dc.init->kind == Expr::String ? string_instance(fi.cls, dc.init->text) : default_instance(fi.cls);
							final_insts_.emplace(n, std::move(fi));
						} else if (dc.init) {
							auto v = fold(*dc.init);
							if (!v) continue;
							if (dt && dt->numeric() && v->type.numeric()) {
								if (dt->k == Type::Float && v->type.k != Type::Float) v->f = double(v->i);
								if (dt->k != Type::Float && v->type.k == Type::Float) v->i = int64_t(v->f);
								v->type = *dt;
							}
							finals_.emplace(n, *v);
						} else {
							continue;
						}
						notes_.push_back(cls_.info().name + ": " + n + " taken from " + stem + "/" + o.name);
						return true;
					}
				}
			}
		}
		return false;
	}

	// The method a bare name calls.
	int find_method(const std::string &n, size_t argc = SIZE_MAX) const {
		int found = -1;
		size_t beh = c_ ? c_->m->beh : 0;
		// The methods nested in the running method and the states enclosing it come first.
		if (c_) {
			int scope = int(c_->m - methods_.data());
			for (int guard = 0; scope >= 0 && guard < 16; ++guard) {
				for (size_t i = 0; i < methods_.size(); ++i) {
					const MethodDecl &m = methods_[i];
					if (m.parent == scope && m.name == n && (argc == SIZE_MAX || m.params.size() == argc)) return int(i);
				}
				scope = methods_[size_t(scope)].parent;
			}
		}
		// Then the most-derived behavior's (a later behavior's SetupMatch replaces an earlier one's
		// even where it is not virtual).
		(void)beh;
		for (size_t i = 0; i < methods_.size(); ++i) {
			const MethodDecl &m = methods_[i];
			if (m.nested || m.name != n || (argc != SIZE_MAX && m.params.size() != argc)) continue;
			if (found < 0 || m.beh >= methods_[size_t(found)].beh) found = int(i);
		}
		return found;
	}

	Type load(const Place &p, uint32_t dst, const Expr &e) {
		Code &code = c_->code;
		switch (p.k) {
		case Place::Local:
			if (p.ref) {
				code.op(21);
				code.u16(uint16_t(p.offset));
				code.i32(int32_t(dst));
			} else {
				code.op(5, int32_t(dst), int32_t(p.offset), 4);
			}
			break;
		case Place::Member:
			if (p.ref) {
				code.op(64);
				code.u16(uint16_t(p.offset));
				code.i32(int32_t(dst));
				break;
			}
			code.op(66, int32_t(dst), int32_t(p.offset), int32_t(p.member->iface || p.member->instance ? 4 : member_size(p.type)));
			if (!p.member->iface && !p.member->instance) {
				Type to = p.type;
				convert(p.type, to.k == Type::Byte || to.k == Type::Word ? prim(Type::Int) : to, dst);
			}
			break;
		case Place::Global:
			code.op(8, int32_t(dst), int32_t(p.offset));
			break;
		case Place::Final:
			code.op(61, int32_t(dst));
			code.u16(uint16_t(p.offset));
			break;
		case Place::Constant:
			constant(p.value, dst);
			break;
		case Place::Self:
			code.op(9, int32_t(dst));
			break;
		case Place::Method:
			code.op(38, int32_t(methods_[size_t(p.method)].pool), int32_t(dst));
			break;
		default:
			fail(e, "unknown name " + e.text);
		}
		use(dst + 4);
		if (p.type.k == Type::Byte || p.type.k == Type::Word) return prim(Type::Int);
		return p.type;
	}

	Type gen(const Expr &e, uint32_t dst) {
		Code &code = c_->code;
		use(dst + 4);
		switch (e.kind) {
		case Expr::Int:
		case Expr::Float:
		case Expr::String: {
			auto v = fold(e);
			constant(*v, dst);
			return v->type;
		}
		case Expr::Null:
			code.op(4, int32_t(dst));
			return prim(Type::Null);
		case Expr::Ident:
			return load(lookup(e.text), dst, e);
		case Expr::Cast: {
			if (auto v = fold(e, &c_->locals)) {
				constant(*v, dst);
				return v->type;
			}
			Type from = gen(*e.a, dst);
			auto to = type_named(e.text);
			if (!to) {
				if (is_behavior(e.text)) return from;
				fail(e, "unknown type " + e.text);
			}
			if (to->numeric() || from.numeric()) {
				convert(from, *to, dst);
				return *to;
			}
			return *to;
		}
		case Expr::Unary: {
			if (auto v = fold(e, &c_->locals)) {
				constant(*v, dst);
				return v->type;
			}
			Type t = gen(*e.a, dst);
			if (e.text == "!") {
				if (t.k == Type::Iface) {
					code.op(88, int32_t(dst));
				} else if (t.k == Type::Float) {
					code.op(2, int32_t(dst + 8), 4);
					code.i32(0);
					code.op(118, int32_t(dst), int32_t(dst + 8));
					use(dst + 12);
					return prim(Type::Bool);
				}
				code.op(115, int32_t(dst));
				return prim(Type::Bool);
			}
			if (e.text == "-") {
				if (t.k == Type::Float) {
					code.op(67, int32_t(dst));
					return t;
				}
				code.op(5, int32_t(dst + 8), int32_t(dst), 4);
				code.op(2, int32_t(dst), 4);
				code.i32(0);
				code.op(141, int32_t(dst), int32_t(dst + 8));
				use(dst + 12);
				return t;
			}
			fail(e, "unsupported operator " + e.text);
		}
		case Expr::Binary:
			return binary(e, dst);
		case Expr::Ternary: {
			std::vector<size_t> to_else, to_end;
			condition(*e.a, dst);
			jump_if_false(dst, to_else);
			Type t = gen(*e.b, dst);
			jump(to_end);
			land(to_else);
			Type u = gen(*e.c, dst);
			convert(u, t, dst);
			land(to_end);
			return t;
		}
		case Expr::Call:
			return call(e, dst);
		case Expr::Member: {
			// Info.effectStandardFan: a native class's constant, named through an instance.
			if (const NativeConstant *nc = native_constant(e.text)) {
				Value v;
				v.type = prim(Type::DWord);
				v.i = nc->value;
				constant(v, dst);
				return v.type;
			}
			Type t = gen(*e.a, dst);
			if (t.k == Type::Obj) {
				if (auto off = native_field(short_name(t.name), e.text)) {
					code.op(47, int32_t(dst), int32_t(*off));
					return field_type(short_name(t.name), e.text);
				}
				if (auto em = native_embedded(short_name(t.name), e.text)) {
					code.op(229, int32_t(dst), int32_t(em->first));
					Type et = prim(Type::Obj);
					et.name = em->second;
					return et;
				}
			}
			fail(e, "unknown member " + e.text);
		}
		case Expr::Assign:
			assign(*e.a, e.text, *e.b);
			return gen(*e.a, dst);
		case Expr::PreInc:
		case Expr::PreDec:
			incdec(*e.a, e.kind == Expr::PreInc);
			return gen(*e.a, dst);
		case Expr::PostInc:
		case Expr::PostDec: {
			Type t = gen(*e.a, dst);
			incdec(*e.a, e.kind == Expr::PostInc);
			return t;
		}
		case Expr::Index: {
			Type t = gen(*e.a, dst);
			if (t.k != Type::Arr) fail(e, "indexing a non-array");
			Type it = gen(*e.b, dst + 4);
			convert(it, prim(Type::Int), dst + 4);
			Type elem = type_named(t.elem).value_or(prim(Type::Int));
			const Sig *s = find_intrinsic(t.name, "<ArrayRead>", 1);
			if (!s) fail(e, "no array index operator on " + t.name);
			code.op(30, int32_t(intrinsic(t.name, s->full)), int32_t(dst));
			return elem;
		}
		case Expr::Scoped: {
			auto p = e.text.rfind("::");
			if (const NativeConstant *nc = native_constant(e.text.substr(p + 2), short_name(e.text.substr(0, p)))) {
				Value v;
				v.type = prim(Type::DWord);
				v.i = nc->value;
				constant(v, dst);
				return v.type;
			}
			fail(e, "unknown " + e.text);
		}
		}
		fail(e, "unsupported expression");
	}

	Type field_type(const std::string &cls, const std::string &field) const {
		if (cls == "CVector") return prim(Type::Float);
		if (field == "m_Damage" || field == "m_StunDamage" || field == "m_EnergyDamage" || field == "m_RecoveryTime" ||
		    field == "m_MomentumAdjustAmount" || field == "m_DiminishDamage" || field == "m_RetainDamage")
			return prim(Type::Float);
		return prim(Type::Int);
	}

	int binop(const std::string &o, const Type &t) const {
		bool f = t.k == Type::Float;
		bool u = t.k == Type::DWord || t.k == Type::Enum;
		if (o == "+") return f ? 151 : u ? 131 : 140;
		if (o == "-") return f ? 152 : u ? 132 : 141;
		if (o == "*") return f ? 148 : u ? 128 : 137;
		if (o == "/") return f ? 149 : u ? 129 : 138;
		if (o == "==") return 118;
		if (o == "!=") return 119;
		if (o == "&&") return 124;
		if (o == "||") return 125;
		if (o == "&") return 121;
		if (o == "|") return 123;
		if (o == "<") return f ? 156 : u ? 136 : 145;
		if (o == ">") return f ? 155 : u ? 135 : 144;
		if (o == "<=") return f ? 154 : 143;
		if (o == ">=") return f ? 153 : 142;
		return -1;
	}

	Type binary(const Expr &e, uint32_t dst) {
		Code &code = c_->code;
		if (auto v = fold(e, &c_->locals)) {
			constant(*v, dst);
			return v->type;
		}
		const std::string &o = e.text;
		Type a = gen(*e.a, dst);
		if (o == "&&") {
			// Short-circuits: a false left operand skips the right (|| evaluates both).
			if (a.k == Type::Iface) code.op(88, int32_t(dst));
			std::vector<size_t> skip;
			jump_if_false(dst, skip);
			Type b = gen(*e.b, dst + 8);
			use(dst + 12);
			if (b.k == Type::Iface) code.op(88, int32_t(dst + 8));
			code.op(124, int32_t(dst), int32_t(dst + 8));
			land(skip);
			return prim(Type::Bool);
		}
		// A plain local on the right is used in place.
		uint32_t rhs = dst + 8;
		Type b;
		Place rp = e.b->kind == Expr::Ident ? lookup(e.b->text) : Place{};
		auto lit = fold(*e.b, &c_->locals);
		bool left_float = a.k == Type::Float;
		if (rp.k == Place::Local && !rp.ref && rp.type.numeric() && a.numeric() && (rp.type.k == a.k || !(left_float || rp.type.k == Type::Float))) {
			rhs = rp.offset;
			b = rp.type;
		} else if (lit && lit->type.numeric() && left_float && lit->type.k != Type::Float) {
			Value v = *lit;
			v.f = double(v.i);
			v.type = prim(Type::Float);
			constant(v, rhs);
			b = v.type;
		} else if (lit && lit->type.k == Type::Float && a.integral()) {
			Value v = *lit;
			v.i = int64_t(v.f);
			v.type = prim(Type::Int);
			constant(v, rhs);
			b = v.type;
		} else {
			b = gen(*e.b, rhs);
		}
		use(dst + 12);
		if (o == "||") {
			if (a.k == Type::Iface) code.op(88, int32_t(dst));
			if (b.k == Type::Iface) code.op(88, int32_t(rhs));
			code.op(125, int32_t(dst), int32_t(rhs));
			return prim(Type::Bool);
		}
		if ((o == "==" || o == "!=") && a.k == Type::Str && b.k == Type::Str) {
			code.op(114, int32_t(dst), int32_t(rhs));
			if (o == "!=") code.op(115, int32_t(dst));
			return prim(Type::Bool);
		}
		if (a.k == Type::Obj && !a.numeric() && b.k == Type::Str) {
			if (const Sig *s = find_intrinsic(a.name, "(opp " + o + ")", 1)) {
				code.op(30, int32_t(intrinsic(owner_of(a.name, s), s->full)), int32_t(dst));
				return prim(Type::Bool);
			}
		}
		// The left operand's type decides; the right is converted to it.
		Type t = a;
		if (a.k == Type::Bool || a.k == Type::Null || a.k == Type::Obj || a.k == Type::Iface) t = prim(Type::Int);
		if (b.numeric() && rhs == dst + 8) convert(b, t, rhs);
		int op = binop(o, t);
		if (op < 0) fail(e, "unsupported operator " + o);
		code.op(op, int32_t(dst), int32_t(rhs));
		bool cmp = o == "==" || o == "!=" || o == "<" || o == ">" || o == "<=" || o == ">=";
		return cmp ? prim(Type::Bool) : t;
	}

	void incdec(const Expr &target, bool inc) {
		Code &code = c_->code;
		Place p = target.kind == Expr::Ident ? lookup(target.text) : Place{};
		if (p.k == Place::Member && (p.type.k == Type::Int || p.type.k == Type::DWord || p.type.k == Type::Float ||
		                              p.type.k == Type::Word || p.type.k == Type::Byte)) {
			int op = p.type.k == Type::Float ? (inc ? 56 : -1) : p.type.k == Type::Word ? (inc ? 54 : 58)
			         : p.type.k == Type::Byte ? (inc ? 55 : 59) : (inc ? 53 : 57);
			if (op > 0) {
				code.op(op, int32_t(p.offset));
				return;
			}
		}
		if (p.k == Place::Local && !p.ref && p.type.numeric()) {
			code.op(p.type.k == Type::Float ? (inc ? 146 : 147) : (inc ? 126 : 127), int32_t(p.offset));
			return;
		}
		// General: x = x +/- 1.
		Expr one;
		one.kind = Expr::Int;
		one.ival = 1;
		one.line = target.line;
		assign(target, inc ? "+=" : "-=", one);
	}

	// Assignments: to locals, members, interfaces, native instances (their (opp =) intrinsics),
	// and native fields (op 50).
	void assign(const Expr &lhs, const std::string &op, const Expr &rhs) {
		Code &code = c_->code;
		uint32_t t = temp();
		if (lhs.kind == Expr::Ident) {
			Place p = lookup(lhs.text);
			bool value_type = p.type.k == Type::Obj && p.k != Place::Global && is_value_class(p.type.name) &&
			                  !(p.k == Place::Member && !p.member->instance);
			if (value_type || p.type.k == Type::Arr) {
				// instance: receiver, then the operand; (opp =)(...) etc.
				load(p, t, lhs);
				Type r = gen(rhs, t + 4);
				std::string oname = op == "-=" ? "(opp -)" : "(opp " + op + ")";
				const Sig *s = find_intrinsic_typed(p.type.name, oname, {r});
				if (!s) fail(lhs, "no operator " + op + " on " + p.type.name);
				coerce(r, s->params[0], t + 4);
				code.op(30, int32_t(intrinsic(owner_of(p.type.name, s), s->full)), int32_t(t));
				return;
			}
			if (op != "=" && p.k == Place::Local && !p.ref && p.type.numeric()) {
				// In place: the operand, converted, combined straight into the local.
				Type r;
				auto lit = fold(rhs, &c_->locals);
				if (lit && lit->type.numeric() && (lit->type.k == Type::Float) != (p.type.k == Type::Float)) {
					Value v = *lit;
					if (p.type.k == Type::Float) v.f = double(v.i);
					else v.i = int64_t(v.f);
					v.type = p.type.k == Type::Float ? prim(Type::Float) : prim(Type::Int);
					constant(v, t);
					r = v.type;
				} else {
					Place rp = rhs.kind == Expr::Ident ? lookup(rhs.text) : Place{};
					if (rp.k == Place::Local && !rp.ref && rp.type.k == p.type.k) {
						int o = binop(op.substr(0, op.size() - 1), p.type);
						if (o < 0) fail(lhs, "unsupported operator " + op);
						code.op(o, int32_t(p.offset), int32_t(rp.offset));
						return;
					}
					r = gen(rhs, t);
				}
				convert(r, p.type, t);
				int o = binop(op.substr(0, op.size() - 1), p.type);
				if (o < 0) fail(lhs, "unsupported operator " + op);
				code.op(o, int32_t(p.offset), int32_t(t));
				return;
			}
			if (op != "=") {
				Expr bin;
				bin.kind = Expr::Binary;
				bin.text = op.substr(0, op.size() - 1);
				bin.line = lhs.line;
				Type a = load(p, t, lhs);
				Type b = gen(rhs, t + 8);
				use(t + 12);
				Type ty = a.k == Type::Bool ? prim(Type::Int) : a;
				if (b.numeric()) convert(b, ty, t + 8);
				int o = binop(bin.text, ty);
				if (o < 0) fail(lhs, "unsupported operator " + op);
				code.op(o, int32_t(t), int32_t(t + 8));
				store(p, ty, t, lhs);
				return;
			}
			Type r = gen(rhs, t);
			store(p, r, t, lhs);
			return;
		}
		if (lhs.kind == Expr::Member) {
			// An embedded native value (Info.m_ConnectPoint): its pointer, then its operator.
			Type ot0 = arg_type(*lhs.a);
			if (ot0.k == Type::Obj) {
				if (auto em = native_embedded(short_name(ot0.name), lhs.text)) {
					gen(*lhs.a, t);
					code.op(229, int32_t(t), int32_t(em->first));
					Type r = gen(rhs, t + 4);
					std::string oname = op == "-=" ? "(opp -)" : "(opp " + op + ")";
					const Sig *s = find_intrinsic_typed(em->second, oname, {r});
					if (!s) fail(lhs, "no operator " + op + " on " + std::string(em->second));
					coerce(r, s->params[0], t + 4);
					code.op(30, int32_t(intrinsic(owner_of(em->second, s), s->full)), int32_t(t));
					use(t + 8);
					return;
				}
			}
			// A native field: the value, then the instance pointer, then op 50.
			Type r = gen(rhs, t);
			uint32_t ptr = t + 4;
			if (op == "=" && ot0.k == Type::Obj) {
				if (native_field(short_name(ot0.name), lhs.text)) {
					convert(r, field_type(short_name(ot0.name), lhs.text), t);
					r = field_type(short_name(ot0.name), lhs.text);
				}
			}
			uint32_t slot;
			Type ot;
			if (lhs.a->kind == Expr::Ident && lookup(lhs.a->text).k == Place::Local && !lookup(lhs.a->text).ref) {
				Place lp = lookup(lhs.a->text);
				slot = lp.offset;
				ot = lp.type;
			} else {
				ot = gen(*lhs.a, ptr);
				slot = ptr;
			}
			auto off = ot.k == Type::Obj ? native_field(short_name(ot.name), lhs.text) : std::nullopt;
			if (!off) fail(lhs, "unknown member " + lhs.text);
			Type ft = field_type(short_name(ot.name), lhs.text);
			if (op != "=") {
				// field op= value: load, combine, store.
				code.op(5, int32_t(t + 8), int32_t(slot), 4);
				code.op(47, int32_t(t + 8), int32_t(*off));
				convert(r, ft, t);
				int o = binop(op.substr(0, op.size() - 1), ft);
				code.op(o, int32_t(t + 8), int32_t(t));
				code.op(5, int32_t(t), int32_t(t + 8), 4);
				use(t + 12);
			} else {
				convert(r, ft, t);
			}
			code.op(50, int32_t(slot), int32_t(*off), 4, int32_t(t));
			use(t + 8);
			return;
		}
		if (lhs.kind == Expr::Index) {
			// a[i] = v: primitive arrays hand out the element's address (<ArrayElementPosition>,
			// op 7 stores through it); reference arrays set it (<ArrayElementSet>).
			Type r = gen(rhs, t);
			uint32_t a = t + 8;
			Type at = gen(*lhs.a, a);
			if (at.k != Type::Arr) fail(lhs, "indexing a non-array");
			Type it = gen(*lhs.b, a + 4);
			convert(it, prim(Type::Int), a + 4);
			Type elem = type_named(at.elem).value_or(prim(Type::Int));
			if (op != "=") {
				const Sig *rd = find_intrinsic(at.name, "<ArrayRead>", 1);
				code.op(5, int32_t(a + 12), int32_t(a), 4);
				code.op(5, int32_t(a + 16), int32_t(a + 4), 4);
				code.op(30, int32_t(intrinsic(at.name, rd->full)), int32_t(a + 12));
				if (r.numeric()) convert(r, elem, t);
				int o = binop(op.substr(0, op.size() - 1), elem);
				if (o < 0) fail(lhs, "unsupported operator " + op);
				code.op(o, int32_t(a + 12), int32_t(t));
				code.op(5, int32_t(t), int32_t(a + 12), 4);
				use(a + 20);
			} else if (r.numeric() || elem.numeric()) {
				convert(r, elem, t);
			}
			if (const Sig *pos = find_intrinsic(at.name, "<ArrayElementPosition>", 1)) {
				code.op(30, int32_t(intrinsic(at.name, pos->full)), int32_t(a));
				code.op(7, int32_t(t), int32_t(a), 4);
			} else if (const Sig *set = find_intrinsic(at.name, "<ArrayElementSet>", 2)) {
				code.op(5, int32_t(a + 8), int32_t(t), 4);
				code.op(30, int32_t(intrinsic(at.name, set->full)), int32_t(a));
			} else {
				fail(lhs, "no element store on " + at.name);
			}
			use(a + 12);
			return;
		}
		fail(lhs, "cannot assign to this expression");
	}

	bool is_value_class(const std::string &full) const {
		return full.find("::") != std::string::npos;
	}

	void store(const Place &p, const Type &from, uint32_t t, const Expr &e) {
		Code &code = c_->code;
		switch (p.k) {
		case Place::Local:
			if (p.ref) {
				code.op(22);
				code.u16(uint16_t(p.offset));
				code.i32(int32_t(t));
			} else if (p.type.k == Type::Iface) code.op(25, int32_t(p.offset), int32_t(t));
			else {
				if (from.numeric() || p.type.numeric()) convert(from, p.type, t);
				code.op(5, int32_t(p.offset), int32_t(t), 4);
			}
			return;
		case Place::Member:
			if (p.member->iface) {
				code.op(29, int32_t(p.offset), int32_t(t));
				return;
			}
			if (p.ref) {
				code.op(62);
				code.u16(uint16_t(p.offset));
				code.i32(int32_t(t));
				return;
			}
			if (from.numeric() || p.type.numeric()) convert(from, p.type, t);
			code.op(65, int32_t(p.offset), int32_t(t), int32_t(p.member->instance ? 4 : member_size(p.type)));
			return;
		default:
			fail(e, "cannot assign to " + e.text);
		}
	}

	// --- calls -------------------------------------------------------------------------------

	Type param_type(const std::string &spelled) const {
		static const std::map<std::string, Type::K> prims = {{"int", Type::Int}, {"float", Type::Float}, {"dword", Type::DWord},
		                                                     {"word", Type::Word}, {"byte", Type::Byte}, {"bool", Type::Bool},
		                                                     {"string", Type::Str}};
		auto p = prims.find(spelled);
		if (p != prims.end()) return prim(p->second);
		if (spelled.rfind("class ", 0) == 0) {
			std::string n = spelled.substr(6);
			if (enums_.count(n)) {
				Type t = prim(Type::Enum);
				t.name = n;
				return t;
			}
			Type t = prim(Type::Obj);
			t.name = api_.full_of.count(n) ? api_.full_of.at(n) : n;
			return t;
		}
		if (spelled == "BGlobalID") return prim(Type::BehLit);
		if (spelled.rfind("classtype", 0) == 0) return prim(Type::ClassLit);
		if (spelled.rfind("Method(", 0) == 0) return prim(Type::MethodRef);
		if (spelled.rfind("BRef ", 0) == 0 || spelled == "BInterface") {
			Type t = prim(Type::Iface);
			t.name = spelled.size() > 5 ? spelled.substr(5) : "";
			return t;
		}
		return prim(Type::None);
	}

	// How well an argument of type `have` fits a parameter spelled `want` (-1: not at all).
	int fit(const Type &have, const std::string &want) const {
		Type w = param_type(want);
		if (want == "class CClassExecutorInstance") return have.k == Type::Exec ? 3 : -1;
		switch (w.k) {
		case Type::Float:
			return have.k == Type::Float ? 3 : have.numeric() ? 1 : -1;
		case Type::Int:
		case Type::DWord:
		case Type::Word:
		case Type::Byte:
		case Type::Bool:
			if (have.k == w.k) return 3;
			if (have.k == Type::Float) return 0;
			if (have.k == Type::Obj && is_value_class(have.name)) return -1;
			return have.numeric() || have.k == Type::Null || have.k == Type::Obj ? 2 : -1;
		case Type::Enum:
			return have.k == Type::Enum && have.name == w.name ? 3 : have.integral() ? 2 : -1;
		case Type::Str:
			if (have.k == Type::Obj && !candidates(have.name, "Conversion", 0).empty()) return 1;
			return have.k == Type::Str ? 3 : -1;
		case Type::BehLit:
			return have.k == Type::BehLit ? 3 : -1;
		case Type::ClassLit:
			return have.k == Type::ClassLit ? 3 : -1;
		case Type::MethodRef:
			return have.k == Type::MethodRef ? 3 : -1;
		case Type::Iface:
			return have.k == Type::Iface || have.k == Type::Null ? 2 : -1;
		case Type::Obj:
			if (have.k == Type::Null) return 2;
			if (have.k == Type::ClassLit) return short_name(w.name) == "CObjectFactory" || short_name(w.name) == "CClass" ? 3 : 1;
			if (have.k == Type::Obj) {
				if (short_name(have.name) == short_name(w.name)) return 3;
				if (derives(have.name, w.name)) return 2;
				if (short_name(w.name) == "CReferenceTarget" || short_name(w.name) == "CObject") return 2;
				if (short_name(have.name) == "CObject" || derives(w.name, have.name)) return 1;  // a downcast the script implies
				return 0;
			}
			if (have.k == Type::Iface) return 0;
			return -1;
		default:
			return -1;
		}
	}

	std::vector<const Sig *> candidates(const std::string &cls, const std::string &name, size_t argc) const {
		std::vector<const Sig *> out;
		std::string c = cls;
		for (int guard = 0; guard < 16 && !c.empty(); ++guard) {
			std::string full = api_.full_of.count(short_name(c)) ? api_.full_of.at(short_name(c)) : c;
			auto it = api_.by_class.find(full);
			if (it != api_.by_class.end()) {
				for (auto &s : it->second) {
					if (s.name == name && s.params.size() == argc) out.push_back(&s);
				}
			}
			if (!out.empty()) return out;
			const char *p = native_parent(short_name(c));
			c = p ? p : "";
		}
		return out;
	}

	const Sig *find_intrinsic(const std::string &cls, const std::string &name, size_t argc) const {
		auto c = candidates(cls, name, argc);
		return c.empty() ? nullptr : c.front();
	}

	const Sig *find_intrinsic_typed(const std::string &cls, const std::string &name, const std::vector<Type> &args) const {
		const Sig *best = nullptr;
		int best_score = -1;
		for (const Sig *s : candidates(cls, name, args.size())) {
			int score = 0;
			for (size_t i = 0; i < args.size() && score >= 0; ++i) {
				int f = fit(args[i], s->params[i]);
				score = f < 0 ? -1 : score + f;
			}
			if (score > best_score) best = s, best_score = score;
		}
		return best;
	}

	// The class the intrinsic table names: the one in the receiver's chain that declares it.
	std::string owner_of(const std::string &cls, const Sig *s) const {
		std::string c = cls;
		for (int guard = 0; guard < 16 && !c.empty(); ++guard) {
			std::string full = api_.full_of.count(short_name(c)) ? api_.full_of.at(short_name(c)) : c;
			auto it = api_.by_class.find(full);
			if (it != api_.by_class.end()) {
				for (auto &x : it->second) {
					if (&x == s) return full;
				}
			}
			const char *p = native_parent(short_name(c));
			c = p ? p : "";
		}
		return cls;
	}

	// An argument's type without generating it (for overload choice): literals, names, and calls
	// whose result type is known.
	Type arg_type(const Expr &e) {
		Code saved = c_->code;
		uint32_t max = c_->max;
		size_t inst = c_->instances.size();
		auto ints = intrinsic_index_;
		auto icount = code_.intrinsics.size();
		auto cls = classes_;
		auto fin = finals_order_.size();
		Type t = gen(e, temp() + 64);
		c_->code = std::move(saved);
		c_->max = max;
		c_->instances.resize(inst);
		intrinsic_index_ = std::move(ints);
		code_.intrinsics.resize(icount);
		classes_ = std::move(cls);
		for (size_t i = fin; i < finals_order_.size(); ++i) finals_order_[i]->index = -1;
		finals_order_.resize(fin);
		return t;
	}

	// A value at `slot` made into what a parameter spelled `want` takes: numeric conversions, and a
	// native value's conversion operator where a primitive is wanted (CText to string).
	void coerce(Type have, const std::string &want_spelled, uint32_t slot) {
		Type want = param_type(want_spelled);
		if (have.k == Type::Obj && (want.k == Type::Str || want.numeric())) {
			for (const Sig *cv : candidates(have.name, "Conversion", 0)) {
				if (param_type(cv->ret).k == want.k) {
					c_->code.op(30, int32_t(intrinsic(owner_of(have.name, cv), cv->full)), int32_t(slot));
					have = want;
					break;
				}
			}
		}
		if (have.numeric() || want.numeric()) convert(have, want, slot);
	}

	// Arguments for a parameter list at `at` (4 bytes each), converted to the parameters.
	void args_to(const std::vector<pc::ExprPtr> &args, const std::vector<std::string> &params, uint32_t at, bool copy_instances) {
		for (size_t i = 0; i < args.size(); ++i) {
			uint32_t slot = at + uint32_t(i) * 4;
			Type want = param_type(params[i]);
			Type have;
			const Expr &a = *args[i];
			if (want.k == Type::BehLit && a.kind == Expr::Ident && is_behavior(a.text)) {
				c_->code.op(26, int32_t(slot), visible_index(a.text));
				have = prim(Type::BehLit);
			} else if ((want.k == Type::Obj || want.k == Type::ClassLit) && a.kind == Expr::Ident && lookup(a.text).k == Place::None && type_named(a.text)) {
				std::string full = type_named(a.text)->name;
				c_->code.op(44, int32_t(slot));
				c_->code.bytes.resize(c_->code.bytes.size() - 4);
				c_->code.u16(uint16_t(class_index(full)));
				c_->code.i32(int32_t(slot));
				have = prim(Type::ClassLit);
			} else {
				have = gen(a, slot);
			}
			use(slot + 4);
			coerce(have, params[i], slot);
			if (copy_instances && want.k == Type::Obj && is_value_class(want.name)) {
				c_->code.op(42, int32_t(class_index(want.name)), int32_t(slot));
			}
		}
	}

	Type ret_type(const Sig &s) {
		if (s.ret.empty()) return prim(Type::Void);
		Type t = param_type(s.ret);
		if (s.ret == "BInterface") t.k = Type::Iface;
		return t;
	}

	Type builtin(const Expr &e, const std::string &n, uint32_t dst, bool &handled) {
		Code &code = c_->code;
		handled = true;
		if (n == "exSleep") {
			if (e.args.empty()) code.op(307);
			else {
				Type t = gen(*e.args[0], dst);
				convert(t, prim(Type::Int), dst);
				code.op(306, int32_t(dst));
			}
			return prim(Type::Void);
		}
		if (n == "exEndFrame") {
			code.op(305);
			return prim(Type::Void);
		}
		if (n == "exKillCallers") {
			code.op(315);
			return prim(Type::Void);
		}
		if ((n == "IsValid" || n == "Owner") && e.args.size() == 1) {
			// op 23 / 24: an interface's validity, or the object it reaches.
			uint32_t src = dst + 4;
			const Expr &a = *e.args[0];
			Place lp = a.kind == Expr::Ident ? lookup(a.text) : Place{};
			if (lp.k == Place::Local && !lp.ref) src = lp.offset;
			else gen(a, src);
			code.op(n == "IsValid" ? 23 : 24, int32_t(src), int32_t(dst));
			use(dst + 8);
			if (n == "IsValid") return prim(Type::Bool);
			Type t = prim(Type::Obj);
			t.name = "CObject";
			return t;
		}
		if (n == "exThis") {
			code.op(10, int32_t(dst));
			return prim(Type::Exec);
		}
		if (n == "TRACE" || n == "ERROR" || n == "WARNING") {
			code.op(312);
			code.u16(uint16_t(e.args.size()));
			for (size_t i = 0; i < e.args.size(); ++i) {
				uint32_t slot = dst + uint32_t(i) * 4;
				Type t = gen(*e.args[i], slot);
				int8_t type = t.k == Type::Str ? 1 : t.k == Type::Float ? 8 : t.k == Type::Bool ? 9 : t.k == Type::DWord || t.k == Type::Enum ? 7 : 6;
				if (t.k == Type::Obj) type = t.name == "CObject::CText" ? 4 : 10;
				code.op(313);
				code.u16(uint16_t(i));
				code.i32(int32_t(slot));
				code.u8(uint8_t(type));
				if (type == 10) code.u16(uint16_t(class_index(t.name)));
			}
			code.op(314);
			code.u16(uint16_t(e.args.size()));
			code.op(n == "TRACE" ? 310 : 311);
			return prim(Type::Void);
		}
		handled = false;
		return prim(Type::None);
	}

	Type call_script(const MethodDecl &m, const Expr &e, uint32_t dst, bool direct) {
		Code &code = c_->code;
		uint32_t ret = m.ret.k != Type::None ? 4 : 0;
		if (e.args.size() != m.params.size()) fail(e, m.signature + " takes " + std::to_string(m.params.size()) + " arguments");
		std::vector<std::string> spelled;
		for (auto &p : m.params) spelled.push_back(spell(p));
		args_to(e.args, spelled, dst + ret, m.state);
		if (m.virt && !direct) code.op(36, m.slot, int32_t(dst));
		else code.op(34, int32_t(m.pool), int32_t(dst));
		use(dst + ret + 4 * uint32_t(m.params.size()) + 4);
		return m.ret.k != Type::None ? m.ret : prim(Type::Void);
	}

	Type call_intrinsic(const std::string &cls, const Expr &e, const std::string &name, uint32_t dst) {
		std::vector<Type> types;
		for (auto &a : e.args) {
			if (a->kind == Expr::Ident && is_behavior(a->text) && lookup(a->text).k == Place::None) types.push_back(prim(Type::BehLit));
			else if (a->kind == Expr::Ident && lookup(a->text).k == Place::None && type_named(a->text)) types.push_back(prim(Type::ClassLit));
			else types.push_back(arg_type(*a));
		}
		const Sig *s = find_intrinsic_typed(cls, name, types);
		if (!s) {
			// Not declared on the receiver's chain: any class declaring it (a script's implied cast).
			for (auto &[c, sigs] : api_.by_class) {
				for (auto &x : sigs) {
					if (x.name == name && x.params.size() == e.args.size()) {
						s = &x;
						break;
					}
				}
				if (s) {
					args_to(e.args, s->params, dst + 4, false);
					c_->code.op(30, int32_t(intrinsic(c, s->full)), int32_t(dst));
					Type r = ret_type(*s);
					if (r.k == Type::Iface && !e.args.empty() && e.args[0]->kind == Expr::Ident) r.name = e.args[0]->text;
					return r;
				}
			}
			fail(e, "no native " + name + " on " + cls);
		}
		args_to(e.args, s->params, dst + 4, false);
		c_->code.op(30, int32_t(intrinsic(owner_of(cls, s), s->full)), int32_t(dst));
		use(dst + 4 + 4 * uint32_t(e.args.size()));
		Type r = ret_type(*s);
		if (r.k == Type::Iface && !e.args.empty() && e.args[0]->kind == Expr::Ident) r.name = e.args[0]->text;
		return r;
	}

	Type call(const Expr &e, uint32_t dst) {
		Code &code = c_->code;
		const Expr &f = *e.a;
		if (f.kind == Expr::Ident) {
			bool handled;
			Type b = builtin(e, f.text, dst, handled);
			if (handled) return b;
			std::string n = f.text[0] == '.' ? f.text.substr(1) : f.text;
			if (int mi = find_method_typed(n, e); mi >= 0) return call_script(methods_[size_t(mi)], e, dst, false);
			code.op(9, int32_t(dst));
			return call_intrinsic(self_class_, e, f.text, dst);
		}
		if (f.kind == Expr::Scoped) {
			auto p = f.text.find("::");
			std::string beh = f.text.substr(0, p), name = f.text.substr(p + 2);
			for (auto &m : methods_) {
				if (!m.nested && m.name == name && visible_[m.beh].info().name == beh && m.params.size() == e.args.size()) {
					return call_script(m, e, dst, true);
				}
			}
			fail(e, "no method " + f.text);
		}
		if (f.kind == Expr::Member) {
			if (int mi = method_path(f); mi >= 0) {
				code.op(38, int32_t(methods_[size_t(mi)].pool), int32_t(dst));  // the path, evaluated (unused)
				return call_script(methods_[size_t(mi)], e, dst, false);
			}
			Type t = gen(*f.a, dst);
			if (t.k == Type::Iface) return call_interface(t, e, f.text, dst);
			if (t.k == Type::Obj || t.k == Type::Arr || t.k == Type::Null) return call_intrinsic(t.k == Type::Null ? "CObject" : t.name, e, f.text, dst);
			if (t.k == Type::Exec) return call_intrinsic("CClassExecutorInstance", e, f.text, dst);
			fail(e, "calling " + f.text + " on a non-object");
		}
		fail(e, "unsupported call");
	}

	// Overloads (AddPilotQuote(int,...) / AddPilotQuote(class CPilotDescription,...)): the best fit
	// among the script methods find_method would consider.
	int find_method_typed(const std::string &n, const Expr &e) {
		int first = find_method(n, e.args.size());
		if (first < 0) return first;
		std::vector<int> cands;
		for (size_t i = 0; i < methods_.size(); ++i) {
			const MethodDecl &m = methods_[i];
			if (m.name == n && m.params.size() == e.args.size() && m.nested == methods_[size_t(first)].nested &&
			    m.parent == methods_[size_t(first)].parent && m.short_sig != methods_[size_t(first)].short_sig)
				cands.push_back(int(i));
		}
		if (cands.empty()) return first;
		cands.insert(cands.begin(), first);
		std::vector<Type> types;
		for (auto &a : e.args) types.push_back(arg_type(*a));
		int best = first, best_score = -1;
		for (int ci : cands) {
			int score = 0;
			for (size_t i = 0; i < types.size() && score >= 0; ++i) {
				int fs = fit(types[i], spell(methods_[size_t(ci)].params[i]));
				score = fs < 0 ? -1 : score + fs;
			}
			if (score > best_score) best = ci, best_score = score;
		}
		return best;
	}

	// A.B.C naming a method nested in another (".PlayerWon.PlayerDestroyed1"), or -1.
	int method_path(const Expr &f) {
		if (f.kind == Expr::Ident) {
			if (c_->locals.count(f.text) || members_.count(f.text)) return -1;
			Place p = lookup(f.text);
			return p.k == Place::Method ? p.method : -1;
		}
		if (f.kind != Expr::Member) return -1;
		int parent = method_path(*f.a);
		if (parent < 0) return -1;
		for (size_t i = 0; i < methods_.size(); ++i) {
			if (methods_[i].parent == parent && methods_[i].name == f.text) return int(i);
		}
		return -1;
	}

	Type call_interface(const Type &t, const Expr &e, const std::string &name, uint32_t dst) {
		int vi = visible_index(t.name);
		if (vi < 0) fail(e, "unknown interface " + t.name);
		auto &info = infos_[size_t(vi)];
		for (size_t k = 0; k < info.methods.size(); ++k) {
			const std::string &sig = info.methods[k].second;
			auto open = sig.find('(');
			std::string head = sig.substr(0, open);
			std::string mname = head.substr(head.rfind(' ') == std::string::npos ? 0 : head.rfind(' ') + 1);
			if (info.pcode.elements[info.methods[k].first].link >= 0) continue;
			if (mname != name || info.method_params[k].size() != e.args.size()) continue;
			uint32_t ret = info.method_rets[k].k != Type::None ? 4 : 0;
			std::vector<std::string> spelled;
			for (auto &p : info.method_params[k]) spelled.push_back(spell(p));
			args_to(e.args, spelled, dst + 4 + ret, info.method_state[k]);
			c_->code.op(37, int32_t(dst), int32_t(k), int32_t(dst + 4));
			use(dst + 8 + ret + 4 * uint32_t(e.args.size()));
			return info.method_rets[k].k != Type::None ? info.method_rets[k] : prim(Type::Void);
		}
		fail(e, "no method " + name + " in " + t.name);
	}
};

}  // namespace

VMClass compile_class(const de::World &data, const de::ObjectRef &cls, std::vector<std::string> *notes) {
	Compiler c(data, cls);
	VMClass out = c.run();
	if (notes) *notes = c.notes();
	return out;
}

}  // namespace vm
