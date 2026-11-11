// pcc: compile a Class from its behaviors' sources (vm/compiler.h) and print the result next to
// the shipped VMC2, if any, for comparison.
//
//   pcc <Data dir> <file stem> <Class> [nth class of that name] [-o <VMC2 payload out>]
//   pcc <Data dir> <file stem> --source <Behavior>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "de/contents.h"
#include "de/objects_file.h"
#include "de/pcode.h"
#include "de/platform.h"
#include "de/scene.h"
#include "vm/compiler.h"
#include "vm/disasm.h"
#include "vm/pcode_parser.h"

namespace {

void print(const vm::VMClass &c, const char *label) {
	printf("==== %s: %zu classes, %zu intrinsics, %zu finals\n", label, c.classes.size(), c.intrinsics.size(), c.finals.size());
	for (size_t i = 0; i < c.classes.size(); ++i) printf("  class %zu: %s layout %u\n", i, c.classes[i].name.c_str(), c.classes[i].layout_pos);
	for (size_t i = 0; i < c.global_refs.size(); ++i) printf("  global ref %zu: kind %u %s\n", i, c.global_refs[i].kind, c.global_refs[i].name.c_str());
	for (size_t b = 0; b < c.behaviors.size(); ++b) {
		auto &bc = c.behaviors[b];
		printf("== behavior %zu: %zu instances, %zu methods", b, bc.instances.size(), bc.methods.size());
		for (auto &r : bc.replicated) printf(" [repl %u %u %u]", r.type, r.a, r.b);
		for (auto &in : bc.instances)
			printf(" [inst %s @%u %u %u %s]", c.classes[in.class_index].name.c_str(), in.value, in.flag, in.count, in.class2 ? c.classes[*in.class2].name.c_str() : "-");
		printf("\n");
		for (size_t m = 0; m < bc.methods.size(); ++m) {
			if (!bc.methods[m]) continue;
			auto &cm = *bc.methods[m];
			printf("  -- %s flags %d%d%d%d counts %u %u %u %u slot %s parent %s bases", cm.signature.c_str(), cm.flags[0], cm.flags[1], cm.flags[2], cm.flags[3],
			       cm.counts[0], cm.counts[1], cm.counts[2], cm.counts[3], cm.v34 ? std::to_string(*cm.v34).c_str() : "-",
			       m < bc.base_links.size() && bc.base_links[m] ? std::to_string(*bc.base_links[m]).c_str() : "-");
			for (auto x : cm.base_methods) printf(" %u", x);
			if (cm.v78 >= 0) printf(" [inherit %d]", cm.v78);
			for (auto &r : cm.replicated) printf(" [repl %u %u %u]", r.type, r.a, r.b);
			for (auto &in : cm.instances) printf(" [inst %s @%u %u %u]", c.classes[in.class_index].name.c_str(), in.value, in.flag, in.count);
			for (auto &in : cm.interfaces) printf(" [iface %u %d %d]", in.a, in.b, in.c);
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
	de::init_platform();
	if (argc < 4) {
		fprintf(stderr, "usage: %s <Data dir> <file stem> <Class> [nth]\n", argv[0]);
		return 2;
	}
	de::World w(argv[1]);
	const de::ObjectsFile *f = w.file(argv[2]);
	if (!f) return 1;
	if (!strcmp(argv[3], "--source") && argc > 4) {
		for (auto &o : f->objects()) {
			if (o.name != argv[4] || o.class_path != "Behavior") continue;
			de::Contents c(f->object_data(o).contents);
			std::string t = de::PCode::parse(c).text();
			fwrite(t.data(), 1, t.size(), stdout);
			return 0;
		}
		fprintf(stderr, "no such behavior\n");
		return 1;
	}
	const char *out = nullptr;
	if (argc > 5 && !strcmp(argv[argc - 2], "-o")) {
		out = argv[argc - 1];
		argc -= 2;
	}
	int which = argc > 4 ? atoi(argv[4]) : 0, seen = 0;
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		auto &o = f->objects()[i];
		if (o.class_path != "Class" || o.name != argv[3] || seen++ != which) continue;
		int rc = 0;
		try {
			std::vector<std::string> notes;
			vm::VMClass c = vm::compile_class(w, {f, i}, &notes);
			for (auto &n : notes) fprintf(stderr, "note: %s\n", n.c_str());
			print(c, "compiled");
			if (out) {
				auto bytes = c.write();
				std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
			}
		} catch (const std::exception &e) {
			fprintf(stderr, "error: %s\n", e.what());
			rc = 1;
		}
		auto d = f->object_data(o);
		if (auto vmc2 = f->ex_payload(d, "VMC2")) print(vm::VMClass::parse(vmc2->data, vm::class_behavior_refs(d.references).size()), "shipped");
		return rc;
	}
	fprintf(stderr, "no such class\n");
	return 1;
}
