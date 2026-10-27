// openbg-brain: run a script class's compiled brain headless and report what it needs.
//
//   openbg-brain <file.DEObjects> <Class> [entry method name] [pulses]
//
// Every intrinsic call is logged by signature and answered with a zero result, and the run
// stops at the first opcode the VM cannot execute yet. The report drives which parts of the
// native API to implement next.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "de/objects_file.h"
#include "vm/disasm.h"
#include "vm/executor.h"
#include "vm/script.h"
#include "vm/vmcode.h"

namespace {

std::vector<uint8_t> read_file(const char *path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		throw std::runtime_error(std::string("cannot open ") + path);
	}
	return {std::istreambuf_iterator<char>(f), {}};
}

// Member stack size: the furthest member byte any method touches (ops 53-59 and 65/66).
uint32_t member_extent(const vm::ScriptClass &cls) {
	uint32_t end = 0;
	for (auto &m : cls.methods) {
		for (auto &ins : vm::decode(m.cm->code)) {
			if (ins.opcode == 65 && ins.args.size() >= 3) {
				end = std::max(end, uint32_t(ins.args[0] + ins.args[2]));
			} else if (ins.opcode == 66 && ins.args.size() >= 3) {
				end = std::max(end, uint32_t(ins.args[1] + ins.args[2]));
			} else if (ins.opcode >= 53 && ins.opcode <= 59 && !ins.args.empty()) {
				end = std::max(end, uint32_t(ins.args[0] + 4));
			}
		}
	}
	return end;
}

struct LoggingHost : vm::Host {
	std::map<std::string, uint64_t> calls;
	std::vector<std::string> order;  // first-call order
	std::vector<std::string> messages;

	void intrinsic(vm::Executor &ex, uint32_t index, uint32_t args) override {
		auto &code = ex.current_class()->code;
		std::string sig = index < code.intrinsics.size()
		                      ? code.classes[code.intrinsics[index].class_index].name + "::" + code.intrinsics[index].signature
		                      : "?" + std::to_string(index);
		if (calls[sig]++ == 0) {
			order.push_back(sig);
		}
		// Exploration aid: OPENBG_TRUE="sig1;sig2" makes those intrinsics return 1.
		static const std::string truthy = getenv("OPENBG_TRUE") ? getenv("OPENBG_TRUE") : "";
		bool yes = !truthy.empty() && (";" + truthy + ";").find(";" + sig + ";") != std::string::npos;
		ex.memory().set<uint32_t>(args, yes ? 1 : 0);  // return slot overwrites `this`
	}
	void message(vm::Executor &, const std::string &text) override { messages.push_back(text); }
};

}  // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <file.DEObjects> <Class> [entry] [pulses]\n", argv[0]);
		return 2;
	}
	std::string entry = argc > 3 ? argv[3] : "Entry";
	int pulses = argc > 4 ? atoi(argv[4]) : 60;
	try {
		de::ObjectsFile f(read_file(argv[1]));
		const de::ObjectInfo *info = nullptr;
		for (auto &o : f.objects()) {
			if (o.class_path == "Class" && o.name == argv[2]) {
				info = &o;
			}
		}
		if (!info) {
			throw std::runtime_error(std::string("no Class ") + argv[2]);
		}
		auto d = f.object_data(*info);
		auto code = f.ex_payload(d, "VMC2");
		if (!code) {
			throw std::runtime_error("class has no compiled VMCode");
		}
		vm::Memory mem;
		vm::ScriptClass cls;
		vm::load_class(cls, mem, d.contents, vm::VMClass::parse(code->data, vm::class_behavior_refs(d.references).size()));

		vm::ScriptMethod *start = cls.find_by_name(entry + "()");
		if (!start) {
			throw std::runtime_error("no entry method " + entry);
		}
		vm::Instance inst;
		inst.cls = &cls;
		inst.members_size = member_extent(cls) + 64;
		inst.members = mem.alloc(inst.members_size);
		inst.self = mem.alloc(1024);    // placeholder for the owning object
		inst.handle = mem.alloc(256);   // placeholder for the brain itself
		LoggingHost host;
		vm::Executor ex(mem, host, inst);
		printf("%s: %zu methods, %zu intrinsics, members %u bytes; starting %s\n", cls.name.c_str(), cls.methods.size(),
		       cls.code.intrinsics.size(), inst.members_size, start->cm->signature.c_str());
		ex.start(*start);
		int ran = 0;
		std::string stop = "ran all pulses";
		try {
			for (; ran < pulses; ++ran) {
				if (ex.pulse()) {
					stop = "brain finished";
					break;
				}
			}
		} catch (const std::exception &e) {
			stop = e.what();
		}
		printf("pulses: %d, instructions: %llu, stopped: %s\n", ran, (unsigned long long)ex.instructions_executed(),
		       stop.c_str());
		// Spawn: the engine calls the brain's ObjectSpawn(position, orientation, momentum).
		if (getenv("SPAWN")) {
			if (auto *mp = cls.find_by_name("bool ObjectSpawn(class CVector,class CCoordinateSystem,class CVector)")) {
				auto &m = *mp;
				printf("spawning via %s\n", m.cm->signature.c_str());
				uint32_t pos = mem.alloc(12), orient = mem.alloc(36), mom = mem.alloc(12);
				float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
				memcpy(mem.ptr(orient, 36), id, 36);
				uint8_t args[16] = {};
				memcpy(args + 4, &pos, 4);
				memcpy(args + 8, &orient, 4);
				memcpy(args + 12, &mom, 4);
				size_t before = host.order.size();
				try {
					uint64_t before_ins = ex.instructions_executed();
					ex.call(m, args);
					printf("ObjectSpawn call executed %llu instructions, now in %s\n",
					       (unsigned long long)(ex.instructions_executed() - before_ins),
					       ex.current_method() ? ex.current_method()->cm->signature.c_str() : "-");
					for (ran = 0; ran < pulses; ++ran) {
						if (ex.pulse()) {
							break;
						}
					}
					stop = "ran all pulses after spawn";
				} catch (const std::exception &e) {
					stop = e.what();
				}
				printf("after ObjectSpawn: pulses %d, stopped: %s, in %s\n", ran, stop.c_str(),
				       ex.current_method() ? ex.current_method()->cm->signature.c_str() : "-");
				printf("new intrinsics after spawn:\n");
				for (size_t i = before; i < host.order.size(); ++i) {
					printf("  %6llu  %s\n", (unsigned long long)host.calls[host.order[i]], host.order[i].c_str());
				}
				return 0;
			}
		}
		if (ex.current_method()) {
			printf("in method: %s\n", ex.current_method()->cm->signature.c_str());
		}
		printf("%zu distinct intrinsics called (first-call order):\n", host.order.size());
		for (auto &s : host.order) {
			printf("  %6llu  %s\n", (unsigned long long)host.calls[s], s.c_str());
		}
		for (auto &m : host.messages) {
			printf("  message: %s\n", m.c_str());
		}
	} catch (const std::exception &e) {
		fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
