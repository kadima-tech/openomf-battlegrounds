#include "validate.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "pilot.h"
#include "script.h"
#include "vm/vmcode.h"

namespace ed {

namespace {

std::string lower(std::string s) {
	for (char &c : s) c = char(tolower(uint8_t(c)));
	return s;
}

bool prefix_clash(const std::string &a, const std::string &b) {
	std::string x = lower(a), y = lower(b);
	return x.rfind(y, 0) == 0 || y.rfind(x, 0) == 0;
}

struct Sink {
	std::vector<Issue> out;
	int errors = 0, warnings = 0;
	void add(Issue::Severity s, int rule, int object, std::string text) {
		out.push_back({s, rule, object, std::move(text)});
		errors += s == Issue::Error;
		warnings += s == Issue::Warning;
	}
};

std::string object_label(const Project &p, size_t i) {
	const auto &info = p.file().objects[i].info;
	return info.name_text() + " (" + info.class_text() + ")";
}

// A passive reference (BOOL, then a sized {agent GUID, object GUID}) at `at` in a chunk body.
void check_passive(const Project &p, Sink &s, size_t object, const Bytes &body, size_t at, const char *what) {
	if (at >= body.size() || body[at] == 0) return;
	deo::Reader r(body, at + 1);
	size_t end = r.sized();
	if (end - r.pos() < 32) {
		s.add(Issue::Error, 8, int(object), object_label(p, object) + ": " + what + " reference is malformed");
		return;
	}
	Guid agent = r.guid(), target = r.guid();
	int local = p.find(target);
	if (local >= 0) {
		if (!(agent == p.agent())) {
			s.add(Issue::Error, 8, int(object), object_label(p, object) + ": " + what + " names the add-on's object with another file's agent GUID");
		}
		return;
	}
	const BaseObject *b = p.base().by_guid(target);
	if (!b) {
		s.add(Issue::Error, 8, int(object), object_label(p, object) + ": " + what + " names an object that no file has");
	} else if (!(p.base().files()[b->file].agent == agent)) {
		s.add(Issue::Warning, 8, int(object), object_label(p, object) + ": " + what + " carries the wrong agent GUID");
	}
}

}  // namespace

std::vector<Issue> validate(const Project &p) {
	Sink s;
	const deo::File &f = p.file();
	const BaseData &base = p.base();

	// 1-2: the container and the agent.
	if (f.format != 2 || f.header_version != 9 || f.master.version != 4 || f.master.class_text() != "TransferAgent\\Win\\File" ||
	    f.master.flag != 0 || f.master.flags != 0 || f.own_version != 1 || f.pool_flag != 1) {
		s.add(Issue::Warning, 2, -1, "The file header differs from what the original game's loader expects");
	} else {
		s.add(Issue::Ok, 1, -1, "File format 2, header v9, agent TransferAgent\\Win\\File");
	}
	bool agent_ok = !guid_zero(f.master.guid);
	for (auto &bf : base.files()) agent_ok &= !(bf.agent == f.master.guid);
	if (agent_ok) s.add(Issue::Ok, 2, -1, "Agent GUID is fresh");
	else s.add(Issue::Error, 2, -1, "The agent's GUID is zero or belongs to a base file");

	// 3: the agent's name.
	const std::string name = p.agent_name();
	const std::string stem = p.path().stem().string();
	bool name_ok = true;
	if (name.empty()) {
		s.add(Issue::Error, 3, -1, "The add-on's agent has no name"), name_ok = false;
	}
	for (auto &bf : base.files()) {
		if (lower(bf.stem) == lower(stem)) {
			s.add(Issue::Error, 3, -1, "The file name " + stem + ".DEObjects is a base game file's"), name_ok = false;
		} else if (!name.empty() && lower(bf.agent_name) == lower(name)) {
			s.add(Issue::Error, 3, -1, "Agent name \"" + name + "\" is already a base file's"), name_ok = false;
		} else if (!name.empty() && prefix_clash(name, bf.agent_name)) {
			s.add(Issue::Warning, 3, -1, "Agent name \"" + name + "\" and \"" + bf.agent_name + "\" are prefixes of each other (the original's FindAgent matches by prefix)"), name_ok = false;
		}
	}
	for (const char *reserved : {"DiversionsRegistry", "PhoenixRegistry", "OMFBG", "Pickups", "SharedObjects", "COREMeshes"}) {
		if (!name.empty() && prefix_clash(name, reserved)) {
			s.add(Issue::Warning, 3, -1, "Agent name \"" + name + "\" clashes with " + reserved + " by prefix"), name_ok = false;
		}
	}
	if (name_ok) s.add(Issue::Ok, 3, -1, "Agent name \"" + name + "\" is unique");

	// 4-8: the objects.
	std::set<std::string> seen;
	std::set<std::string> base_classes;
	for (auto &bf : base.files()) {
		for (auto &o : bf.objects) base_classes.insert(o.class_path);
	}
	int guid_problems = 0, ref_problems = 0, unknown_layouts = 0, scripts = 0;
	for (size_t i = 0; i < f.objects.size(); ++i) {
		const deo::Object &o = f.objects[i];
		const std::string cls = o.info.class_text();
		if (guid_zero(o.info.guid)) {
			s.add(Issue::Error, 4, int(i), object_label(p, i) + " has a zero GUID"), ++guid_problems;
		} else if (!seen.insert(guid_key(o.info.guid)).second) {
			s.add(Issue::Error, 4, int(i), object_label(p, i) + " has the GUID of another add-on object"), ++guid_problems;
		} else if (base.by_guid(o.info.guid)) {
			s.add(Issue::Error, 4, int(i), object_label(p, i) + " reuses a base game GUID (it would not replace the base object)"), ++guid_problems;
		}
		if (o.info.version != 4 || o.info.flag != 0 || (o.info.flags & ~deo::Object::kCompressed) != 0) {
			s.add(Issue::Warning, 4, int(i), object_label(p, i) + ": object info is not v4 with clear flags");
		}
		if (!base_classes.count(cls)) {
			s.add(Issue::Warning, 4, int(i), object_label(p, i) + ": class path not used by 2.123's data");
		}
		if (o.compressed() && !o.packed.empty() && (o.packed_as_is || deo::decompress(o.packed) == o.contents)) {
			uint8_t method = o.packed[0];
			if (method != 0 && method != 6 && method != 7) {
				s.add(Issue::Error, 5, int(i), object_label(p, i) + ": compression method " + std::to_string(method));
			}
		}
		bool understood = false;
		for (const Ref &r : object_references(cls, o.references, &understood)) {
			if (r.kind == Ref::Null) continue;
			if (r.kind == Ref::Local && r.index >= f.objects.size()) {
				s.add(Issue::Error, 6, int(i), object_label(p, i) + ": local reference out of range"), ++ref_problems;
			} else if (r.kind == Ref::Pool && r.index >= f.guid_pool.size()) {
				s.add(Issue::Error, 6, int(i), object_label(p, i) + ": GUID pool reference out of range"), ++ref_problems;
			} else if (!p.target(r).resolved) {
				s.add(Issue::Warning, 6, int(i), object_label(p, i) + ": reference to " + p.target(r).guid.str() + " that no file has (loads as null)"), ++ref_problems;
			}
		}
		if (!understood && !o.references.empty()) ++unknown_layouts;
		for (uint32_t c : p.children(uint32_t(i))) {
			if (c >= f.objects.size()) s.add(Issue::Error, 7, int(i), object_label(p, i) + ": child out of range");
		}
		Chunks cc = split_chunks(o.contents);
		if (cls.rfind("Description", 0) == 0 && !cc.chunks.empty()) {
			deo::Reader r(cc.chunks[0].body);
			try {
				r.block();
				check_passive(p, s, i, cc.chunks[0].body, r.pos(), "its resource");
			} catch (const deo::Error &) {
			}
			if (cls == "Description\\Pilot" && cc.chunks.size() > 1) check_passive(p, s, i, cc.chunks[1].body, 0, "its photo");
		}
		if (cls == "Class" || cls == "Behavior") ++scripts;
	}
	for (const Guid &g : f.guid_pool) {
		if (p.find(g) < 0 && !base.by_guid(g)) {
			s.add(Issue::Warning, 6, -1, "GUID pool entry " + g.str() + " is in no file"), ++ref_problems;
		}
	}
	if (!guid_problems) s.add(Issue::Ok, 4, -1, "GUIDs unique across the add-on and " + std::to_string(base.files().size()) + " base files");
	if (!ref_problems) s.add(Issue::Ok, 6, -1, "References resolve");
	if (unknown_layouts) {
		s.add(Issue::Warning, 6, -1, std::to_string(unknown_layouts) + " objects have references the editor does not decode yet (not checked)");
	}
	if (scripts) {
		// 9: VMC2 whose change information is the attached behaviors' revisions now.
		int stale = 0, current = 0;
		for (uint32_t c : addon_script_classes(p)) {
			const deo::Object &o = f.objects[c];
			std::vector<uint8_t> expect;
			bool known = change_information(p, c, &expect);
			const deo::Ex *vmc2 = nullptr;
			for (auto &e : o.ex) {
				if (text_of(e.key) == "VMC2") vmc2 = &e;
			}
			if (!vmc2) {
				s.add(Issue::Error, 9, int(c), object_label(p, c) + " has no VMC2: compile it (the original would compile its source at load)"), ++stale;
				continue;
			}
			std::vector<uint8_t> have;
			try {
				have = vm::VMClass::parse(vmc2->data, object_references("Class", o.references).size()).change_info;
			} catch (const std::exception &) {
			}
			if (known && have != expect) {
				s.add(Issue::Error, 9, int(c), object_label(p, c) + ": its VMC2 is older than its behaviors; compile it"), ++stale;
			} else {
				++current;
			}
		}
		if (!stale && current) s.add(Issue::Ok, 9, -1, std::to_string(current) + " script class(es) carry current VMC2");
		// 10: the sources compile (the last compile's messages).
		const auto &errors = last_compile_errors(p);
		for (auto &m : errors) {
			Issue i{Issue::Error, 10, m.object, m.str()};
			i.line = m.text_line;
			s.out.push_back(i);
			++s.errors;
		}
		if (errors.empty() && compiled_once(p)) s.add(Issue::Ok, 10, -1, "Every add-on script compiles with OpenBG's compiler");
	}

	// 11: pilots meant to be playable.
	for (uint32_t pi : addon_pilots(p)) {
		PilotObjects po = pilot_objects(p, pi);
		if (po.description < 0) {
			s.add(Issue::Error, 11, int(pi), object_label(p, pi) + " has no Description\\Pilot");
			continue;
		}
		PilotData d;
		try {
			d = read_pilot(p, po);
		} catch (const deo::Error &e) {
			s.add(Issue::Error, 11, int(pi), object_label(p, pi) + ": " + e.what());
			continue;
		}
		bool playable = true;
		if (d.name.empty()) s.add(Issue::Error, 11, int(pi), "A pilot has no name"), playable = false;
		for (const BaseObject *b : base.of_class("Description\\Pilot")) {
			if (!d.name.empty() && b->name == d.name) {
				s.add(Issue::Warning, 11, po.description, d.name + " has the name of a base game pilot; both will be listed");
				break;
			}
		}
		if (!d.listed) s.add(Issue::Warning, 11, po.description, d.name + " is not listed, so Choose Pilot will not show it"), playable = false;
		if (d.locked) s.add(Issue::Warning, 11, po.description, d.name + " is locked by default"), playable = false;
		bool cast = false;
		for (const Guid &g : d.casts) {
			std::string n = object_name(p, g);
			cast |= n == "PilotSPLadder" || n == "MultiplayerPilot";
		}
		if (!cast) s.add(Issue::Warning, 11, po.description, d.name + " is in neither PilotSPLadder nor MultiplayerPilot"), playable = false;
		if (po.photo < 0) s.add(Issue::Warning, 11, int(pi), d.name + " has no photo");
		if (playable) s.add(Issue::Ok, 11, po.description, d.name + " is listed, unlocked and in a pilot cast");
	}
	if (!f.objects.empty()) s.add(Issue::Ok, 12, -1, "Lists show add-on entries in file-name order (the original lists later names first)");
	return s.out;
}

}  // namespace ed
