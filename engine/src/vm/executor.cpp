#include "vm/executor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vm/disasm.h"

namespace vm {

void Host::unhandled(Executor &ex, int32_t opcode, std::span<const int64_t>) {
	throw VMError("vm: opcode " + std::to_string(opcode) + " needs the object model (in " +
	              (ex.current_method() ? ex.current_method()->cm->signature : std::string("?")) + ")");
}

uint32_t Host::new_instance(Executor &ex, uint32_t, uint32_t count) {
	return ex.memory().alloc(kNativeInstanceSize * (count ? count : 1));
}

uint32_t Host::new_local_instance(Executor &ex, uint32_t, uint32_t source) {
	// Without class layouts, hand out a zeroed block (copying the source when given).
	uint32_t a = ex.memory().alloc(kNativeInstanceSize);
	if (source) {
		memcpy(ex.memory().ptr(a, kNativeInstanceSize), ex.memory().ptr(source, kNativeInstanceSize), kNativeInstanceSize);
	}
	return a;
}

Executor::Executor(Memory &mem, Host &host, Instance &inst, uint32_t stack_size)
	: mem_(mem), host_(host), inst_(inst), stack_(mem.alloc(stack_size)), stack_size_(stack_size) {}

void Executor::call(ScriptMethod &m, std::span<uint8_t> args) {
	uint32_t n = m.return_size() + m.param_size();
	if (args.size() < n) {
		throw VMError("vm: argument block too small for " + m.cm->signature);
	}
	uint32_t off = frame_size_;  // callee frame goes right after the current one
	if (off + n > stack_size_) {
		throw VMError("vm: stack overflow");
	}
	memcpy(mem_.ptr(sp() + off, n), args.data(), n);
	uint32_t saved_base = base_;
	internal_execute(m, off, true);
	memcpy(args.data(), mem_.ptr(stack_ + saved_base + off, m.return_size() ? m.return_size() : 1),
	       m.return_size());
}

void Executor::start(ScriptMethod &m) {
	++generation_;
	frames_.clear();
	inst_.sleep = 0;  // a new state runs: exSleep() only holds the state that called it
	base_ = 0;
	method_ = &m;
	cls_ = m.cls;
	frame_size_ = m.frame_size();
	pc_ = m.code_addr;
	end_ = m.code_addr + m.code_size;
	frames_.push_back({0, pc_, &m, false, {}});
}

bool Executor::pulse() {
	if (frames_.empty() || inst_.shut_down) {
		return true;
	}
	if (inst_.sleep > 0) {
		--inst_.sleep;
		return false;
	}
	return continue_execution() == 1;
}

// DE_CPCExecuter::InternalExecute(method, int, int) (Core 0x7014e580).
void Executor::internal_execute(ScriptMethod &m, uint32_t args_offset, bool sync) {
	if (pc_ != 0 && !frames_.empty()) {
		frames_.back().pc = pc_;
	}
	base_ += args_offset;
	if (base_ + m.frame_size() > stack_size_) {
		throw VMError("vm: stack overflow calling " + m.cm->signature);
	}
	cls_ = m.cls;
	method_ = &m;
	pc_ = m.code_addr;
	end_ = m.code_addr + m.code_size;
	frames_.push_back({base_, pc_, &m, true, {}});
	if (static const char *calls = getenv("OPENBG_CALLS"); calls && inst_.trace_calls) {
		fprintf(stderr, "[call] %*s%s%s\n", int(frames_.size() * 2), "", m.cm->signature.c_str(),
		        m.is_instant() ? "" : "  [state]");
	}
	continue_execution();  // runs the prologue (op 1), which suspends
	if (sync) {
		continue_execution();
	}
}

void Executor::invoke(ScriptMethod &callee, uint32_t args_offset) {
	// op 34 (Core 0x7014eaa0 case 0x22), without the cross-executer and state-teardown paths.
	if (!callee.is_instant() && method_->is_instant()) {
		// An instant method naming a state switches the brain to it: drop the running frames
		// and restart from the new state with the given arguments (case 0x22, SetState path).
		uint32_t n = callee.return_size() + callee.param_size();
		std::vector<uint8_t> args(n);
		if (n) {
			memcpy(args.data(), mem_.ptr(sp() + args_offset, n), n);
		}
		start(callee);
		if (n) {
			memcpy(mem_.ptr(stack_, n), args.data(), n);
		}
		state_switched_ = true;
		return;
	}
	internal_execute(callee, args_offset, callee.is_instant());
}

std::string Executor::format_varparams() {
	// AppendVariableParametersToString (Core 0x70153ee0): one value per declared parameter.
	std::string s;
	for (size_t i = vp_last_.first; i < vp_last_.second && i < varparams_.size(); ++i) {
		const VarParam &p = varparams_[i];
		char buf[64];
		// Type codes from AppendVariableParametersToString; values are frame slot offsets.
		switch (p.type) {
		case 1: {  // char *
			uint32_t a = get<uint32_t>(uint32_t(p.value));
			s += a ? reinterpret_cast<const char *>(mem_.ptr(a)) : "";
			break;
		}
		case 3: snprintf(buf, sizeof buf, "%u", unsigned(get<uint8_t>(uint32_t(p.value)))); s += buf; break;
		case 5: snprintf(buf, sizeof buf, "%u", unsigned(get<uint16_t>(uint32_t(p.value)))); s += buf; break;
		case 6: snprintf(buf, sizeof buf, "%d", get<int32_t>(uint32_t(p.value))); s += buf; break;
		case 7: snprintf(buf, sizeof buf, "%u", get<uint32_t>(uint32_t(p.value))); s += buf; break;
		case 8: snprintf(buf, sizeof buf, "%f", double(get<float>(uint32_t(p.value)))); s += buf; break;
		case 9: s += get<uint32_t>(uint32_t(p.value)) ? "true" : "false"; break;
		default:
			snprintf(buf, sizeof buf, "<t%d:%d>", p.type, get<int32_t>(uint32_t(p.value)));
			s += buf;
		}
	}
	return s;
}

void Executor::set_state(ScriptMethod &m, std::span<const uint8_t> args) {
	start(m);
	size_t n = std::min<size_t>(args.size(), m.return_size() + m.param_size());
	if (n) {
		memcpy(mem_.ptr(stack_, uint32_t(n)), args.data(), n);
	}
	state_switched_ = executing_ > 0;
}

int Executor::continue_execution() {
	struct Depth {
		int &d;
		explicit Depth(int &x) : d(x) { ++d; }
		~Depth() { --d; }
	} depth(executing_);
	int result = -1;
	uint64_t budget = 0;
	uint32_t resume_pc = 0;
	for (;;) {
		while (pc_ < end_) {
			const uint32_t at = pc_;
			int32_t op = mem_.get<int32_t>(pc_);
			pc_ += 4;
			++executed_;
			if (++budget > 2000000) {
				throw VMError("vm: runaway script (no yield after 2M instructions) in " + method_->cm->signature);
			}
			static const bool trace = getenv("OPENBG_TRACE") != nullptr;
			if (trace) {
				fprintf(stderr, "[vm] %s +%u op%d base=%u frames=%zu\n", method_->cm->signature.c_str(),
				        at - method_->code_addr, op, base_, frames_.size());
			}
			auto i32 = [&] { int32_t v = mem_.get<int32_t>(pc_); pc_ += 4; return v; };
			auto u32 = [&] { uint32_t v = mem_.get<uint32_t>(pc_); pc_ += 4; return v; };
			auto u16 = [&] { uint16_t v = mem_.get<uint16_t>(pc_); pc_ += 2; return v; };
			auto u8 = [&] { uint8_t v = mem_.get<uint8_t>(pc_); pc_ += 1; return v; };
			auto jump = [&](uint32_t target) { pc_ = method_->code_addr + target; };
			auto b4 = [&](uint32_t off, bool v) { set<uint32_t>(off, v ? 1 : 0); };

			switch (op) {
			case 1: {  // prologue: frame size; clear return slot and locals; suspend
				frame_size_ = uint32_t(i32());
				i32();
				uint32_t ret = method_->return_size(), params = method_->param_size();
				if (base_ + frame_size_ > stack_size_) {
					throw VMError("vm: stack overflow");
				}
				memset(mem_.ptr(sp(), ret ? ret : 1), 0, ret);
				if (frame_size_ > ret + params) {
					memset(mem_.ptr(sp() + ret + params, frame_size_ - ret - params), 0, frame_size_ - ret - params);
				}
				// SetupStack: construct the method's "instance" locals at their frame offsets.
				for (auto &ci : method_->cm->instances) {
					if (!ci.flag && ci.value + 4 <= frame_size_) {
						set<uint32_t>(ci.value, host_.new_instance(*this, ci.class_index, ci.count));
					}
				}
				result = 0;
				resume_pc = pc_;
				pc_ = end_;
				break;
			}
			case 2: {  // copy inline literal
				uint32_t dst = u32(), n = u32();
				memcpy(mem_.ptr(sp() + dst, n ? n : 1), mem_.ptr(pc_, n ? n : 1), n);
				pc_ += n;
				break;
			}
			case 3: {  // pointer to inline data (string literals)
				uint32_t dst = u32(), n = u32();
				set<uint32_t>(dst, pc_);
				pc_ += n;
				break;
			}
			case 4:
				set<uint32_t>(u32(), 0);
				break;
			case 5: {  // copy within frame
				uint32_t dst = u32(), src = u32(), n = u32();
				memmove(mem_.ptr(sp() + dst, n ? n : 1), mem_.ptr(sp() + src, n ? n : 1), n);
				break;
			}
			case 7: {  // copy to the address held in a slot
				uint32_t src = u32(), ptr_slot = u32(), n = u32();
				uint32_t to = get<uint32_t>(ptr_slot);
				memmove(mem_.ptr(to, n ? n : 1), mem_.ptr(sp() + src, n ? n : 1), n);
				break;
			}
			case 9:
				set<uint32_t>(u32(), inst_.self);
				break;
			case 10:
				set<uint32_t>(u32(), inst_.handle);
				break;
			case 20: {  // allocate local reference slots
				uint16_t n = u16();
				frames_.back().local_refs.resize(frames_.back().local_refs.size() + n, 0);
				break;
			}
			case 21: {
				uint16_t idx = u16();
				uint32_t dst = u32();
				auto &refs = frames_.back().local_refs;
				set<uint32_t>(dst, idx < refs.size() ? refs[idx] : 0);
				break;
			}
			case 22: {
				uint16_t idx = u16();
				uint32_t src = u32();
				auto &refs = frames_.back().local_refs;
				if (refs.size() <= idx) {
					refs.resize(idx + 1, 0);
				}
				refs[idx] = get<uint32_t>(src);
				break;
			}
			case 34:    // call method by class index
			case 36: {  // virtual call by slot through the running state's table
				uint32_t idx = u32(), args = u32();
				ScriptClass *in = op == 36 && inst_.cls ? inst_.cls : cls_;
				ScriptMethod *callee = op == 36 ? in->resolve_virtual(idx, current_state()) : in->method(idx);
				if (!callee) {
					throw VMError("vm: call to missing method index " + std::to_string(idx));
				}
				invoke(*callee, args);
				if (state_switched_) {
					state_switched_ = false;
					result = 0;  // the new state's prologue runs on the next pulse
					resume_pc = pc_;
					pc_ = end_;
				}
				break;
			}
			case 8: {
				uint32_t dst = u32(), idx = u32();
				set<uint32_t>(dst, host_.global_entry(*this, idx));
				break;
			}
			case 26: {
				uint32_t dst = u32(), idx = u32();
				set<uint32_t>(dst, host_.behavior_global_id(*this, idx));
				break;
			}
			case 39: {
				uint32_t idx = u32(), dst = u32();
				set<uint32_t>(dst, host_.global_reference(*this, idx));
				break;
			}
			case 61: {
				uint32_t dst = u32();
				uint16_t idx = u16();
				set<uint32_t>(dst, host_.final_instance(*this, idx));
				break;
			}
			case 44: {
				uint16_t idx = u16();
				uint32_t dst = u32();
				set<uint32_t>(dst, host_.class_handle(*this, idx));
				break;
			}
			case 42: {  // local instance: class, dst slot, source slot (layout per case 0x2a)
				uint32_t idx = u32(), dst = u32();
				set<uint32_t>(dst, host_.new_local_instance(*this, idx, get<uint32_t>(dst)));
				break;
			}
			case 62: {  // assign ref member
				uint16_t idx = u16();
				uint32_t src = u32();
				if (inst_.ref_members.size() <= idx) {
					inst_.ref_members.resize(idx + 1, 0);
				}
				inst_.ref_members[idx] = get<uint32_t>(src);
				break;
			}
			case 64: {  // ref member target
				uint16_t idx = u16();
				uint32_t dst = u32();
				set<uint32_t>(dst, idx < inst_.ref_members.size() ? inst_.ref_members[idx] : 0);
				break;
			}
			case 23: case 24: {  // interface valid / Owner(interface): the target brain's object (+0x18)
				uint32_t src = u32(), dst = u32();
				uint32_t iface = get<uint32_t>(src);
				set<uint32_t>(dst, op == 23 ? (host_.interface_valid(*this, iface) ? 1 : 0)
				                            : (host_.interface_valid(*this, iface) ? host_.interface_owner(*this, iface) : 0));
				break;
			}
			case 25: {  // copy interface handle
				uint32_t dst = u32(), src = u32();
				set<uint32_t>(dst, get<uint32_t>(src));
				break;
			}
			case 27: {  // member interface valid
				uint32_t pos = u32(), dst = u32();
				uint32_t iface = pos + 4 <= inst_.members_size ? mem_.get<uint32_t>(inst_.members + pos) : 0;
				set<uint32_t>(dst, host_.interface_valid(*this, iface) ? 1 : 0);
				break;
			}
			case 29: {  // assign member interface
				uint32_t pos = u32(), src = u32();
				if (pos + 4 <= inst_.members_size) {
					mem_.set<uint32_t>(inst_.members + pos, get<uint32_t>(src));
				}
				break;
			}
			case 37: {  // execute through a behavior interface (DE_CBehaviorInterface::Execute 0x7013e390)
				uint32_t slot = u32(), method = u32(), args = u32();
				host_.interface_execute(*this, get<uint32_t>(slot), method, sp() + args, sp() + slot);
				if (state_switched_) {  // the call put this very brain into a new state
					state_switched_ = false;
					result = 0;
					resume_pc = pc_;
					pc_ = end_;
				}
				break;
			}
			case 315: {  // KillCallers: keep only the running frame
				if (frames_.size() > 1) {
					Frame top = std::move(frames_.back());
					frames_.clear();
					frames_.push_back(std::move(top));
				}
				break;
			}
			case 30: {  // intrinsic
				uint32_t idx = u32(), args = u32();
				host_.intrinsic(*this, idx, sp() + args);
				break;
			}
			case 38: {  // method pointer
				uint32_t idx = u32(), dst = u32();
				ScriptMethod *m = cls_->method(idx);
				set<uint32_t>(dst, m ? m->handle : 0);
				break;
			}
			case 47: {  // load through pointer + offset
				uint32_t slot = u32(), off = u32();
				uint32_t p = get<uint32_t>(slot);
				if (p) {
					set<uint32_t>(slot, mem_.get<uint32_t>(p + off));
				}
				break;
			}
			case 50: {  // store through pointer + offset
				uint32_t slot = u32(), off = u32(), n = u32(), src = u32();
				uint32_t p = get<uint32_t>(slot);
				if (p) {
					memmove(mem_.ptr(p + off, n ? n : 1), mem_.ptr(sp() + src, n ? n : 1), n);
				}
				break;
			}
			case 53: case 54: case 55: case 56: case 57: case 58: case 59: {  // member ++/--
				uint32_t pos = u32();
				uint32_t a = inst_.members + pos;
				switch (op) {
				case 53: mem_.set<int32_t>(a, mem_.get<int32_t>(a) + 1); break;
				case 54: mem_.set<int16_t>(a, int16_t(mem_.get<int16_t>(a) + 1)); break;
				case 55: mem_.set<uint8_t>(a, uint8_t(mem_.get<uint8_t>(a) + 1)); break;
				case 56: mem_.set<float>(a, mem_.get<float>(a) + 1.0f); break;
				case 57: mem_.set<int32_t>(a, mem_.get<int32_t>(a) - 1); break;
				case 58: mem_.set<int16_t>(a, int16_t(mem_.get<int16_t>(a) - 1)); break;
				case 59: mem_.set<uint8_t>(a, uint8_t(mem_.get<uint8_t>(a) - 1)); break;
				}
				break;
			}
			case 65: {  // frame -> member stack
				uint32_t pos = u32(), src = u32(), n = u32();
				if (pos + n > inst_.members_size) {
					throw VMError("vm: member write out of range");
				}
				memmove(mem_.ptr(inst_.members + pos, n ? n : 1), mem_.ptr(sp() + src, n ? n : 1), n);
				break;
			}
			case 66: {  // member stack -> frame
				uint32_t dst = u32(), pos = u32(), n = u32();
				if (pos + n > inst_.members_size) {
					throw VMError("vm: member read out of range");
				}
				memmove(mem_.ptr(sp() + dst, n ? n : 1), mem_.ptr(inst_.members + pos, n ? n : 1), n);
				break;
			}
			// Conversions (in place).
			case 67: { uint32_t s = u32(); set<float>(s, -get<float>(s)); break; }
			case 72: { uint32_t s = u32(); set<float>(s, float(get<int32_t>(s))); break; }
			case 73: { uint32_t s = u32(); set<float>(s, float(get<uint32_t>(s))); break; }
			case 74: { uint32_t s = u32(); set<float>(s, float(get<uint16_t>(s))); break; }
			case 76: { uint32_t s = u32(); set<float>(s, float(get<uint8_t>(s))); break; }
			case 78: case 83: { uint32_t s = u32(); set<uint32_t>(s, get<uint16_t>(s)); break; }
			case 80: case 85: { uint32_t s = u32(); set<uint32_t>(s, get<uint8_t>(s)); break; }
			case 82: case 87: { uint32_t s = u32(); set<int32_t>(s, int32_t(get<float>(s))); break; }  // __ftol
			case 89: case 90: case 94: case 99: case 100: u32(); break;  // narrowing: no-op on little endian
			case 93: { uint32_t s = u32(); set<int16_t>(s, int16_t(int32_t(get<float>(s)))); break; }
			case 103: { uint32_t s = u32(); set<uint8_t>(s, uint8_t(int32_t(get<float>(s)))); break; }
			case 88: {  // interface has a target brain and behavior
				uint32_t s = u32();
				b4(s, host_.interface_valid(*this, get<uint32_t>(s)));
				break;
			}
			// Branches.
			case 109: { uint32_t s = u32(), t = u32(); if (get<int32_t>(s) == 0) jump(t); break; }
			case 110: { uint32_t s = u32(), t = u32(); if (get<int16_t>(s) == 0) jump(t); break; }
			case 111: { uint32_t s = u32(), t = u32(); if (get<uint8_t>(s) == 0) jump(t); break; }
			case 112: jump(u32()); break;
			case 113: { uint32_t t = u32(); if (!inst_.master) jump(t); break; }
			case 114: {  // string equality
				uint32_t a = u32(), b = u32();
				uint32_t pa = get<uint32_t>(a), pb = get<uint32_t>(b);
				bool eq = pa && pb &&
				          !strcmp(reinterpret_cast<const char *>(mem_.ptr(pa)), reinterpret_cast<const char *>(mem_.ptr(pb)));
				b4(a, eq);
				break;
			}
			// Integer ops: a op= b.
			case 115: { uint32_t s = u32(); b4(s, get<int32_t>(s) == 0); break; }
			case 118: { uint32_t a = u32(), b = u32(); b4(a, get<int32_t>(a) == get<int32_t>(b)); break; }
			case 119: { uint32_t a = u32(), b = u32(); b4(a, get<int32_t>(a) != get<int32_t>(b)); break; }
			case 121: { uint32_t a = u32(), b = u32(); set<uint32_t>(a, get<uint32_t>(a) & get<uint32_t>(b)); break; }
			case 123: { uint32_t a = u32(), b = u32(); set<uint32_t>(a, get<uint32_t>(a) | get<uint32_t>(b)); break; }
			case 124: { uint32_t a = u32(), b = u32(); b4(a, get<int32_t>(a) && get<int32_t>(b)); break; }
			case 125: { uint32_t a = u32(), b = u32(); b4(a, get<int32_t>(a) || get<int32_t>(b)); break; }
			case 126: { uint32_t s = u32(); set<int32_t>(s, get<int32_t>(s) + 1); break; }
			case 127: { uint32_t s = u32(); set<int32_t>(s, get<int32_t>(s) - 1); break; }
			case 128: case 137: { uint32_t a = u32(), b = u32(); set<int32_t>(a, get<int32_t>(a) * get<int32_t>(b)); break; }
			case 129: { uint32_t a = u32(), b = u32(); uint32_t d = get<uint32_t>(b); set<uint32_t>(a, d ? get<uint32_t>(a) / d : 0); break; }
			case 131: case 140: { uint32_t a = u32(), b = u32(); set<int32_t>(a, get<int32_t>(a) + get<int32_t>(b)); break; }
			case 132: case 141: { uint32_t a = u32(), b = u32(); set<int32_t>(a, get<int32_t>(a) - get<int32_t>(b)); break; }
			case 135: { uint32_t a = u32(), b = u32(); b4(a, get<uint32_t>(a) > get<uint32_t>(b)); break; }
			case 136: { uint32_t a = u32(), b = u32(); b4(a, get<uint32_t>(a) < get<uint32_t>(b)); break; }
			case 138: { uint32_t a = u32(), b = u32(); int32_t d = get<int32_t>(b); set<int32_t>(a, d ? get<int32_t>(a) / d : 0); break; }
			case 142: { uint32_t a = u32(), b = u32(); b4(a, get<int32_t>(a) >= get<int32_t>(b)); break; }
			case 143: { uint32_t a = u32(), b = u32(); b4(a, get<int32_t>(a) <= get<int32_t>(b)); break; }
			case 144: { uint32_t a = u32(), b = u32(); b4(a, get<int32_t>(a) > get<int32_t>(b)); break; }
			case 145: { uint32_t a = u32(), b = u32(); b4(a, get<int32_t>(a) < get<int32_t>(b)); break; }
			// Float ops.
			case 146: { uint32_t s = u32(); set<float>(s, get<float>(s) + 1.0f); break; }
			case 147: { uint32_t s = u32(); set<float>(s, get<float>(s) - 1.0f); break; }
			case 148: { uint32_t a = u32(), b = u32(); set<float>(a, get<float>(a) * get<float>(b)); break; }
			case 149: {  // division by zero yields 0
				uint32_t a = u32(), b = u32();
				float d = get<float>(b);
				set<float>(a, d == 0.0f ? 0.0f : get<float>(a) / d);
				break;
			}
			case 151: { uint32_t a = u32(), b = u32(); set<float>(a, get<float>(a) + get<float>(b)); break; }
			case 152: { uint32_t a = u32(), b = u32(); set<float>(a, get<float>(a) - get<float>(b)); break; }
			case 153: { uint32_t a = u32(), b = u32(); b4(a, !(get<float>(a) < get<float>(b))); break; }
			case 154: { uint32_t a = u32(), b = u32(); b4(a, !(get<float>(b) < get<float>(a))); break; }
			case 155: { uint32_t a = u32(), b = u32(); b4(a, !(get<float>(a) <= get<float>(b))); break; }
			case 156: { uint32_t a = u32(), b = u32(); b4(a, !(get<float>(b) <= get<float>(a))); break; }
			// Short ops.
			case 159: { uint32_t s = u32(); set<int16_t>(s, int16_t(get<int16_t>(s) - 1)); break; }
			case 162: { uint32_t a = u32(), b = u32(); b4(a, get<int16_t>(a) == get<int16_t>(b)); break; }
			case 168: { uint32_t a = u32(), b = u32(); set<int16_t>(a, int16_t(get<int16_t>(a) * get<int16_t>(b))); break; }
			case 169: { uint32_t a = u32(), b = u32(); uint16_t d = get<uint16_t>(b); set<uint16_t>(a, d ? uint16_t(get<uint16_t>(a) / d) : 0); break; }
			case 171: { uint32_t a = u32(), b = u32(); set<int16_t>(a, int16_t(get<int16_t>(a) + get<int16_t>(b))); break; }
			case 172: { uint32_t a = u32(), b = u32(); set<int16_t>(a, int16_t(get<int16_t>(a) - get<int16_t>(b))); break; }
			case 175: { uint32_t a = u32(), b = u32(); b4(a, get<uint16_t>(a) > get<uint16_t>(b)); break; }
			// Byte ops.
			case 186: { uint32_t s = u32(); set<uint8_t>(s, get<uint8_t>(s) == 0 ? 1 : 0); break; }
			case 187: { uint32_t s = u32(); set<uint8_t>(s, uint8_t(get<uint8_t>(s) + 1)); break; }
			case 188: { uint32_t s = u32(); set<uint8_t>(s, uint8_t(get<uint8_t>(s) - 1)); break; }
			case 191: { uint32_t a = u32(), b = u32(); b4(a, get<uint8_t>(a) == get<uint8_t>(b)); break; }
			case 192: { uint32_t a = u32(), b = u32(); b4(a, get<uint8_t>(a) != get<uint8_t>(b)); break; }
			case 194: { uint32_t a = u32(), b = u32(); set<uint8_t>(a, get<uint8_t>(a) & get<uint8_t>(b)); break; }
			case 200: { uint32_t a = u32(), b = u32(); set<uint8_t>(a, uint8_t(get<uint8_t>(a) + get<uint8_t>(b))); break; }
			case 201: { uint32_t a = u32(), b = u32(); set<uint8_t>(a, uint8_t(get<uint8_t>(a) - get<uint8_t>(b))); break; }
			case 202: { uint32_t a = u32(), b = u32(); b4(a, get<uint8_t>(a) >= get<uint8_t>(b)); break; }
			case 204: { uint32_t a = u32(), b = u32(); b4(a, get<uint8_t>(a) > get<uint8_t>(b)); break; }
			case 205: { uint32_t a = u32(), b = u32(); b4(a, get<uint8_t>(a) < get<uint8_t>(b)); break; }
			case 229: {  // pointer += offset (if non-null)
				uint32_t s = u32(), off = u32();
				uint32_t p = get<uint32_t>(s);
				if (p) {
					set<uint32_t>(s, p + off);
				}
				break;
			}
			// Scheduling.
			case 305:  // exEndFrame: suspend until next pulse
				result = 0;
				resume_pc = pc_;
				pc_ = end_;
				break;
			case 306:  // exSleep(n): set the sleep counter and suspend
			case 307:  // exSleep forever
				inst_.sleep = op == 306 ? get<int32_t>(u32()) : 0xfffffff;
				result = 0;
				resume_pc = pc_;
				pc_ = end_;
				break;
			case 308: inst_.sleep = 0; break;
			case 309: inst_.shut_down = true; result = 0; resume_pc = end_; pc_ = end_; break;
			case 310:  // TRACE: no case in the shipped engine (compiled out); printed on request
				if (static const bool on = getenv("OPENBG_SCRIPT_TRACE") != nullptr; on) {
					host_.message(*this, "TRACE " + format_varparams());
				}
				break;
			case 311: host_.message(*this, format_varparams()); break;
			case 312: {  // push variable-parameter frame
				uint16_t n = u16();
				int32_t start = vp_frames_.empty() ? 0 : int32_t(varparams_.size());
				vp_frames_.push_back(start);
				varparams_.resize(size_t(start) + n);
				break;
			}
			case 313: {  // declare variable parameter
				uint16_t idx = u16();
				int32_t value = i32();
				int8_t type = int8_t(u8());
				uint32_t cls = type == 10 ? u16() : 0;
				size_t at = size_t(vp_frames_.empty() ? 0 : vp_frames_.back()) + idx;
				if (varparams_.size() <= at) {
					varparams_.resize(at + 1);
				}
				varparams_[at] = {value, type, cls};
				break;
			}
			case 314:  // close the frame; its values stay for a following TRACE/ERROR (op 310/311)
				u16();
				if (!vp_frames_.empty()) {
					vp_last_ = {size_t(vp_frames_.back()), varparams_.size()};
					vp_frames_.pop_back();
				}
				break;
			case 316:
				inst_.master = false;
				if (getenv("OPENBG_DEBUG_MASTER")) host_.message(*this, "master off in " + method_->cm->signature);
				break;
			case 317:
				inst_.master = true;
				if (getenv("OPENBG_DEBUG_MASTER")) host_.message(*this, "master on in " + method_->cm->signature);
				break;
			case 318: set_default_state(cls_->method(u32())); break;
			case 319:  // return
				pc_ = end_;
				break;
			default: {
				// Object-model opcodes: decode operands generically and hand them to the host.
				std::span<const uint8_t> code(mem_.ptr(method_->code_addr, method_->code_size), method_->code_size);
				size_t p = at - method_->code_addr;
				auto ins = decode_at(code, p);
				pc_ = method_->code_addr + uint32_t(p);
				host_.unhandled(*this, op, ins.args);
				break;
			}
			}
		}

		if (result >= 0) {
			pc_ = resume_pc;
			return result;
		}

		// The current frame ran off its end.
		if (frames_.size() < 2) {
			if (!default_state_ || method_->is_instant()) {
				frames_.clear();
				base_ = 0;
				pc_ = 0;
				return 1;
			}
			start(*default_state_);
			continue;
		}
		bool sync = frames_.back().sync;
		frames_.pop_back();
		Frame &prev = frames_.back();
		base_ = prev.base;
		method_ = prev.method;
		cls_ = method_->cls;
		frame_size_ = method_->frame_size();
		pc_ = prev.pc;
		end_ = method_->code_addr + method_->code_size;
		if (sync) {
			return 0;
		}
	}
}

}  // namespace vm
