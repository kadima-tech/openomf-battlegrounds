// Bytecode interpreter: DE_CPCExecuter (Core 0x70146900..0x70154370).
//
// Execution model (ContinueExecutionInternal, 0x7014eaa0):
//  - A frame is [return value][parameters][locals]; operands are byte offsets from the frame
//    base. Calls place the callee frame right after the caller's (base + frame size).
//  - op 1 is the method prologue; it sizes and clears the frame and then suspends, which is
//    why a synchronous call runs ContinueExecution twice.
//  - op 305 (exEndFrame) suspends until the next pulse; op 319 returns.
//  - Running off the end of the outermost frame restarts the default state (state machines)
//    or finishes.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "vm/memory.h"
#include "vm/script.h"

namespace vm {

class Executor;

// Bytes handed out for an instance of a native value class (CVector, CGenericEvent,
// CAttackInfo: script-visible fields up to +0xc4).
constexpr uint32_t kNativeInstanceSize = 256;

// A running script object ("brain", DE_CClassExecutorInstance).
struct Instance {
	ScriptClass *cls = nullptr;
	uint32_t members = 0;       // member stack (CopyToMemberStack/CopyFromMemberStack)
	uint32_t members_size = 0;
	uint32_t self = 0;          // op 9: the object the brain is attached to (executer +0x50)
	uint32_t handle = 0;        // op 10: the instance itself (executer +0x54)
	bool master = true;         // IsMaster (op 113): +0x114 clear
	bool controlled = true;     // ... and +0xf8: its creature is locally controlled
	int32_t sleep = 0;          // SetSleep (ops 306/307)
	bool shut_down = false;
	bool trace_calls = false;   // OPENBG_CALLS debugging: log every method entered
	std::vector<uint32_t> ref_members;  // RefMember targets (ops 62/64), one per VMClass::ref_members
};

// Everything that needs the native object model goes through the host.
class Host {
public:
	virtual ~Host() = default;
	// op 30: call intrinsic class->intrinsics[index] with its argument block at args.
	virtual void intrinsic(Executor &ex, uint32_t index, uint32_t args) = 0;
	// op 311: ERROR()/TRACE() with formatted variable parameters.
	virtual void message(Executor &ex, const std::string &text) = 0;
	// Object-model lookups; defaults answer "null" so a brain can run before the engine
	// implements them.
	virtual uint32_t global_entry(Executor &, uint32_t /*index*/) { return 0; }       // op 8
	virtual uint32_t behavior_global_id(Executor &, uint32_t /*index*/) { return 0; } // op 26
	virtual uint32_t global_reference(Executor &, uint32_t /*index*/) { return 0; }   // op 39
	virtual uint32_t final_instance(Executor &, uint32_t /*index*/) { return 0; }     // op 61
	virtual uint32_t class_handle(Executor &, uint32_t /*index*/) { return 0; }       // op 44
	// SetupStack (DE_CClassCompiledMethod::SetupStack 0x7013e370): construct an "instance"
	// variable of VMClass class `index` (frame locals in the prologue, members at brain start).
	virtual uint32_t new_instance(Executor &ex, uint32_t index, uint32_t count);
	// op 42: copy-construct a local instance of class `index` from `source` (may be 0).
	virtual uint32_t new_local_instance(Executor &, uint32_t index, uint32_t source);
	// Behavior interfaces (ops 23-25, 27, 29, 37, 88): handles to another brain's behavior.
	virtual bool interface_valid(Executor &, uint32_t /*iface*/) { return false; }
	virtual uint32_t interface_global_id(Executor &, uint32_t /*iface*/) { return 0; }
	// Owner(interface): the object the interface's target brain runs on.
	virtual uint32_t interface_owner(Executor &, uint32_t /*iface*/) { return 0; }
	// op 37: run method `method` of the interface's behavior on its brain; `args` is the argument
	// block, `result` where the return value goes (the interface's own slot, as in the original).
	virtual void interface_execute(Executor &, uint32_t /*iface*/, uint32_t /*method*/, uint32_t /*args*/,
	                               uint32_t /*result*/) {}
	// Ops not implemented at all. Default: throw.
	virtual void unhandled(Executor &ex, int32_t opcode, std::span<const int64_t> args);
};

struct VMError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

class Executor {
public:
	Executor(Memory &mem, Host &host, Instance &inst, uint32_t stack_size = 256 * 1024);

	// Synchronous call (InternalExecute with a virtual pointer, Core 0x7014e260): args holds
	// [return][params]; on return the return bytes are written back into args.
	void call(ScriptMethod &m, std::span<uint8_t> args);
	// Start a method as the running state (asynchronous; its body runs on the next pulse).
	void start(ScriptMethod &m);
	void set_default_state(ScriptMethod *m) { default_state_ = m; }
	// DE_CClassExecutorInstance::SetState from outside the running code (an interface call): the
	// brain restarts in `m` with `args`; if it is mid-execution it stops at the current call.
	void set_state(ScriptMethod &m, std::span<const uint8_t> args);
	bool executing() const { return executing_ > 0; }
	// One pulse. Returns true when execution has finished (nothing left to run).
	bool pulse();
	bool running() const { return !frames_.empty(); }
	// Bumped whenever a state starts; lets a caller see that a call switched the state.
	uint64_t generation() const { return generation_; }

	Memory &memory() { return mem_; }
	Instance &instance() { return inst_; }
	ScriptClass *current_class() const { return cls_; }
	ScriptMethod *current_method() const { return method_; }
	// The running state (innermost non-instant frame): virtual calls resolve against its scope,
	// as SetupVirtualMethodTable builds the table per state, whichever instant method calls.
	ScriptMethod *current_state() const {
		for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
			if (it->method && !it->method->is_instant()) return it->method;
		}
		return method_;
	}
	uint32_t sp() const { return stack_ + base_; }  // address of the current frame

	uint64_t instructions_executed() const { return executed_; }

	// GenerateStatus / ProcessStatus (DE_CClassExecutorInstance, Core 0x70145930 / 0x70145b50): the
	// call stack as (method, IP offset) per frame, outermost first, each with its frame's address
	// and local references.
	struct StatusFrame {
		ScriptMethod *method;
		uint32_t ip;
		uint32_t addr;
		const std::vector<uint32_t> *local_refs;
	};
	std::vector<StatusFrame> status() const;
	// Rebuilds the stack from such frames: each frame cleared and set up after the previous one,
	// `fill` then writing its replicated locals; execution resumes in the last at its IP.
	struct RestoreFrame {
		ScriptMethod *method;
		uint32_t ip;
	};
	void restore(const std::vector<RestoreFrame> &frames, const std::function<void(ScriptMethod &, uint32_t addr, std::vector<uint32_t> &local_refs)> &fill);

private:
	struct Frame {
		uint32_t base;
		uint32_t pc;        // saved pc of this frame while a callee runs
		ScriptMethod *method;
		bool sync;          // return to the native caller when this frame finishes
		std::vector<uint32_t> local_refs;  // ops 20-22
	};
	struct VarParam {
		int32_t value;
		int8_t type;
		uint32_t cls;
	};

	void internal_execute(ScriptMethod &m, uint32_t args_offset, bool sync);
	int continue_execution();  // ContinueExecutionInternal: 0 = suspended, 1 = finished
	void invoke(ScriptMethod &callee, uint32_t args_offset);
	std::string format_varparams();

	template <typename T>
	T get(uint32_t off) { return mem_.get<T>(sp() + off); }
	template <typename T>
	void set(uint32_t off, T v) { mem_.set<T>(sp() + off, v); }

	Memory &mem_;
	Host &host_;
	Instance &inst_;
	uint32_t stack_;        // DataBlock at executer +0x1c
	uint32_t stack_size_;
	uint32_t base_ = 0;     // +0x34
	uint32_t frame_size_ = 0;  // +0x3c
	uint32_t pc_ = 0;       // +0x2c (arena address)
	uint32_t end_ = 0;      // +0x30
	ScriptMethod *method_ = nullptr;  // +0x24
	ScriptClass *cls_ = nullptr;      // +0x4c
	ScriptMethod *default_state_ = nullptr;  // +0x28
	std::vector<Frame> frames_;       // +0x40 DE_CExecuteInfo array
	std::vector<int32_t> vp_frames_;
	std::pair<size_t, size_t> vp_last_{0, 0};  // the most recently closed varparam frame  // +0x10 variable-parameter frame starts
	std::vector<VarParam> varparams_;
	uint64_t executed_ = 0;
	bool state_switched_ = false;
	uint64_t generation_ = 0;
	int executing_ = 0;  // continue_execution nesting depth
};

}  // namespace vm
