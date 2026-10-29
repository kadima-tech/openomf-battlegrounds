// pcc: compile a Class from its behaviors' sources (vm/compiler.h) and print the result next to
// the shipped VMC2, if any, for comparison.
//
//   pcc <Data dir> <file stem> <Class> [nth class of that name]
#include <cstdio>
#include <cstdlib>

#include "de/objects_file.h"
#include "de/scene.h"
#include "vm/compiler.h"
#include "vm/disasm.h"
#include "vm/pcode_parser.h"

namespace {

void print(const vm::VMClass &c, const char *label) {
	printf("==== %s: %zu classes, %zu intrinsics, %zu finals\n", label, c.classes.size(), c.intrinsics.size(), c.finals.size());
	for (size_t b = 0; b < c.behaviors.size(); ++b) {
		auto &bc = c.behaviors[b];
		printf("== behavior %zu: %zu instances, %zu methods\n", b, bc.instances.size(), bc.methods.size());
		for (size_t m = 0; m < bc.methods.size(); ++m) {
			if (!bc.methods[m]) continue;
			auto &cm = *bc.methods[m];
			printf("  -- %s flags %d%d%d%d counts %u %u %u %u slot %s parent %s bases", cm.signature.c_str(), cm.flags[0], cm.flags[1], cm.flags[2], cm.flags[3],
			       cm.counts[0], cm.counts[1], cm.counts[2], cm.counts[3], cm.v34 ? std::to_string(*cm.v34).c_str() : "-",
			       m < bc.base_links.size() && bc.base_links[m] ? std::to_string(*bc.base_links[m]).c_str() : "-");
			for (auto x : cm.base_methods) printf(" %u", x);
			printf("\n");
			for (auto &ins : vm::decode(cm.code)) {
				std::string s = vm::format(ins);
				if (ins.opcode == 30 && !ins.args.empty() && size_t(ins.args[0]) < c.intrinsics.size()) {
					auto &in = c.intrinsics[size_t(ins.args[0])];
					s += "   ; " + c.classes[in.class_index].name + "::" + in.signature;
				}
				printf("     %s\n", s.c_str());
			}
		}
	}
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <Data dir> <file stem> <Class> [nth]\n", argv[0]);
		return 2;
	}
	de::World w(argv[1]);
	const de::ObjectsFile *f = w.file(argv[2]);
	if (!f) return 1;
	int which = argc > 4 ? atoi(argv[4]) : 0, seen = 0;
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		auto &o = f->objects()[i];
		if (o.class_path != "Class" || o.name != argv[3] || seen++ != which) continue;
		try {
			std::vector<std::string> notes;
			vm::VMClass c = vm::compile_class(w, {f, i}, &notes);
			for (auto &n : notes) fprintf(stderr, "note: %s\n", n.c_str());
			print(c, "compiled");
		} catch (const std::exception &e) {
			fprintf(stderr, "error: %s\n", e.what());
			return 1;
		}
		auto d = f->object_data(o);
		if (auto vmc2 = f->ex_payload(d, "VMC2")) print(vm::VMClass::parse(vmc2->data, vm::class_behavior_refs(d.references).size()), "shipped");
		return 0;
	}
	fprintf(stderr, "no such class\n");
	return 1;
}
