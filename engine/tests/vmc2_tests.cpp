// VMC2 writer tests: every shipped Class's VMC2 parses and writes back byte for byte, and for the
// classes our compiler reproduces, compile -> write equals the shipped payload.
// Usage: vmc2-tests <Data dir> [-v | -vm (also each method: same, frame (differs only in the frame size) or differs)
//                                 | -vd (-vm, and both codes of each method that differs)]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "de/platform.h"
#include "de/scene.h"
#include "vm/compiler.h"
#include "vm/disasm.h"
#include "vm/vmcode.h"

namespace {

// Field-level difference between two VMClasses, for the report.
std::string first_difference(const vm::VMClass &a, const vm::VMClass &b) {
	if (a.change_info != b.change_info) return "change_info";
	if (a.classes.size() != b.classes.size()) {
		std::string m = "classes count:";
		for (auto &k : a.classes) m += " " + k.name;
		m += " | shipped:";
		for (auto &k : b.classes) m += " " + k.name;
		return m;
	}
	for (size_t i = 0; i < a.classes.size(); ++i) {
		if (a.classes[i].name != b.classes[i].name) return "class " + std::to_string(i) + " name " + a.classes[i].name + " vs " + b.classes[i].name;
		if (a.classes[i].layout_pos != b.classes[i].layout_pos) return "class " + a.classes[i].name + " layout_pos";
	}
	if (a.intrinsics.size() != b.intrinsics.size()) return "intrinsics count";
	for (size_t i = 0; i < a.intrinsics.size(); ++i) {
		if (a.intrinsics[i].class_index != b.intrinsics[i].class_index || a.intrinsics[i].signature != b.intrinsics[i].signature)
			return "intrinsic " + std::to_string(i) + " " + a.intrinsics[i].signature + " vs " + b.intrinsics[i].signature;
	}
	if (a.ref_members != b.ref_members) return "ref_members";
	if (a.behaviors.size() != b.behaviors.size()) return "behaviors count";
	auto inst_eq = [](const vm::ClassInstance &x, const vm::ClassInstance &y) {
		return x.class_index == y.class_index && x.value == y.value && x.flag == y.flag && x.count == y.count && x.class2 == y.class2;
	};
	auto insts_eq = [&](const std::vector<vm::ClassInstance> &x, const std::vector<vm::ClassInstance> &y) {
		if (x.size() != y.size()) return false;
		for (size_t i = 0; i < x.size(); ++i)
			if (!inst_eq(x[i], y[i])) return false;
		return true;
	};
	auto repl_eq = [](const std::vector<vm::CompiledMethod::Replicated> &x, const std::vector<vm::CompiledMethod::Replicated> &y) {
		if (x.size() != y.size()) return false;
		for (size_t i = 0; i < x.size(); ++i)
			if (x[i].type != y[i].type || x[i].a != y[i].a || x[i].b != y[i].b) return false;
		return true;
	};
	for (size_t bi = 0; bi < a.behaviors.size(); ++bi) {
		auto &x = a.behaviors[bi];
		auto &y = b.behaviors[bi];
		std::string at = "behavior " + std::to_string(bi) + " ";
		if (!insts_eq(x.instances, y.instances)) return at + "instances";
		if (x.methods.size() != y.methods.size()) return at + "methods count";
		for (size_t mi = 0; mi < x.methods.size(); ++mi) {
			auto &p = x.methods[mi];
			auto &q = y.methods[mi];
			if (p.has_value() != q.has_value()) return at + "method " + std::to_string(mi) + " presence";
			if (!p) continue;
			std::string m = at + p->signature + ": ";
			if (p->name != q->name) return m + "name";
			if (p->code != q->code) return m + "code";
			if (memcmp(p->flags, q->flags, sizeof p->flags)) return m + "flags";
			if (p->v34 != q->v34) return m + "slot";
			if (p->base_methods != q->base_methods) return m + "base_methods";
			if (!insts_eq(p->instances, q->instances)) return m + "instances";
			if (p->interfaces.size() != q->interfaces.size()) return m + "interfaces count";
			for (size_t i = 0; i < p->interfaces.size(); ++i)
				if (p->interfaces[i].a != q->interfaces[i].a || p->interfaces[i].b != q->interfaces[i].b || p->interfaces[i].c != q->interfaces[i].c)
					return m + "interfaces";
			if (memcmp(p->counts, q->counts, sizeof p->counts)) return m + "counts";
			if (p->signature != q->signature) return m + "signature";
			if (p->v78 != q->v78) return m + "inherit (v78)";
			if (!repl_eq(p->replicated, q->replicated)) return m + "replicated";
		}
		if (x.base_links != y.base_links) return at + "base_links";
		if (!repl_eq(x.replicated, y.replicated)) return at + "replicated";
	}
	if (a.global_refs.size() != b.global_refs.size()) return "global_refs count";
	for (size_t i = 0; i < a.global_refs.size(); ++i)
		if (a.global_refs[i].kind != b.global_refs[i].kind || a.global_refs[i].name != b.global_refs[i].name) return "global_ref " + b.global_refs[i].name;
	if (a.member_interfaces != b.member_interfaces) return "member_interfaces";
	if (a.finals.size() != b.finals.size()) return "finals count";
	for (size_t i = 0; i < a.finals.size(); ++i)
		if (a.finals[i].class_index != b.finals[i].class_index || a.finals[i].data != b.finals[i].data) return "final " + std::to_string(i);
	if (a.member_size != b.member_size)
		return "member_size " + std::to_string(a.member_size.value_or(~0u)) + " vs " + std::to_string(b.member_size.value_or(~0u));
	if (a.tail != b.tail) return "tail";
	return "";
}

}  // namespace

int main(int argc, char **argv) {
	de::init_platform();
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir> [-v]\n", argv[0]);
		return 2;
	}
	bool dump = argc > 2 && !strcmp(argv[2], "-vd");
	bool per_method = dump || (argc > 2 && !strcmp(argv[2], "-vm"));
	bool verbose = per_method || (argc > 2 && !strcmp(argv[2], "-v"));
	de::World w(argv[1]);
	size_t classes = 0, round_trip = 0, compiled = 0, identical = 0, tables = 0, methods = 0, same_code = 0, same_but_frame = 0, finals = 0;
	int failures = 0;
	std::map<std::string, size_t> reasons;
	for (auto &[stem, f] : w.files()) {
		for (uint32_t i = 0; i < f->objects().size(); ++i) {
			auto &o = f->objects()[i];
			if (o.class_path != "Class") continue;
			auto d = f->object_data(o);
			auto vmc2 = f->ex_payload(d, "VMC2");
			if (!vmc2) continue;
			++classes;
			std::string label = stem + ":" + o.name;
			vm::VMClass shipped = vm::VMClass::parse(vmc2->data, vm::class_behavior_refs(d.references).size());
			if (!shipped.tail.empty() || !shipped.member_size) {
				fprintf(stderr, "unparsed VMC2 tail: %s\n", label.c_str());
				++failures;
			}
			if (shipped.write() == vmc2->data) {
				++round_trip;
			} else {
				fprintf(stderr, "round trip differs: %s\n", label.c_str());
				++failures;
			}
			vm::VMClass mine;
			try {
				std::vector<std::string> errors;
				mine = vm::compile_class(w, {f.get(), i}, nullptr, &errors);
				if (verbose) for (auto &e : errors) printf("compiled past   %s: %s\n", label.c_str(), e.c_str());
			} catch (const std::exception &e) {
				if (verbose) printf("compile error   %s: %s\n", label.c_str(), e.what());
				++reasons["compile error"];
				continue;
			}
			++compiled;
			bool same_tables = mine.classes.size() == shipped.classes.size() && mine.intrinsics.size() == shipped.intrinsics.size();
			for (size_t k = 0; same_tables && k < mine.classes.size(); ++k)
				same_tables = mine.classes[k].name == shipped.classes[k].name && mine.classes[k].layout_pos == shipped.classes[k].layout_pos;
			for (size_t k = 0; same_tables && k < mine.intrinsics.size(); ++k)
				same_tables = mine.intrinsics[k].class_index == shipped.intrinsics[k].class_index && mine.intrinsics[k].signature == shipped.intrinsics[k].signature;
			tables += same_tables;
			finals += mine.finals.size() == shipped.finals.size() && std::equal(mine.finals.begin(), mine.finals.end(), shipped.finals.begin(), [&](const auto &x, const auto &y) {
				          return x.data == y.data && mine.classes[x.class_index].name == shipped.classes[y.class_index].name;
			          });
			for (size_t b = 0; b < shipped.behaviors.size() && b < mine.behaviors.size(); ++b) {
				for (size_t m = 0; m < shipped.behaviors[b].methods.size(); ++m) {
					auto &q = shipped.behaviors[b].methods[m];
					if (!q) continue;
					++methods;
					auto &ms = mine.behaviors[b].methods;
					if (m >= ms.size() || !ms[m] || ms[m]->signature != q->signature) continue;
					same_code += ms[m]->code == q->code;
					auto a = ms[m]->code, c = q->code;
					if (a.size() >= 8 && c.size() >= 8) std::fill(a.begin() + 4, a.begin() + 8, 0), std::fill(c.begin() + 4, c.begin() + 8, 0);
					same_but_frame += a == c && ms[m]->code != q->code;
					if (per_method)
						printf("method %s %s %s (frame %u, shipped %u)\n", ms[m]->code == q->code ? "same" : a == c ? "frame" : "differs", label.c_str(), q->signature.c_str(), ms[m]->counts[0], q->counts[0]);
					if (dump && a != c) {
						for (auto *side : {&ms[m]->code, &q->code}) {
							printf("  %s\n", side == &q->code ? "shipped:" : "ours:");
							try {
								for (auto &ins : vm::decode(*side)) printf("    %s\n", vm::format(ins).c_str());
							} catch (const std::exception &) {
							}
						}
					}
				}
			}
			for (size_t k = 0; per_method && k < mine.finals.size() && k < shipped.finals.size(); ++k) {
				auto &x = mine.finals[k], &y = shipped.finals[k];
				(void)y;
				if (std::none_of(shipped.finals.begin(), shipped.finals.end(), [&](const auto &z) { return z.data == x.data; })) printf("final unknown %s %zu %s\n", label.c_str(), k, mine.classes[x.class_index].name.c_str());
			}
			std::string diff = first_difference(mine, shipped);
			bool same_bytes = mine.write() == vmc2->data;
			if (diff.empty() != same_bytes) {
				fprintf(stderr, "writer disagrees with field comparison: %s\n", label.c_str());
				++failures;
			}
			if (same_bytes) {
				++identical;
				if (verbose) printf("identical       %s\n", label.c_str());
			} else {
				if (verbose) printf("compiler differs %s: %s\n", label.c_str(), diff.c_str());
				++reasons[diff.find(": ") != std::string::npos ? diff.substr(diff.find(": ") + 2) : diff.substr(0, diff.find(' '))];
			}
		}
	}
	printf("VMC2: %zu classes, %zu round-trip exactly; %zu compile, %zu byte-identical to the shipped payload\n", classes,
	       round_trip, compiled, identical);
	printf("  of those compiled: %zu with the shipped class and intrinsic tables; %zu of %zu methods with identical bytecode\n",
	       tables, same_code, methods);
	printf("  %zu more methods differ only in the frame size; %zu classes with the shipped finals\n", same_but_frame, finals);
	for (auto &[r, n] : reasons) printf("  %zu: %s\n", n, r.c_str());
	// Floors for the compiler's fidelity on 2.123's 255 compiled classes, so it can only improve.
	if (classes == 0 || (classes == 255 && (compiled < 239 || tables < 61 || same_code < 309 || same_code + same_but_frame < 6382))) {
		fprintf(stderr, "compiler fidelity fell below the recorded floor\n");
		++failures;
	}
	printf("%s (%d failures)\n", failures ? "FAIL" : "OK", failures);
	return failures != 0;
}
