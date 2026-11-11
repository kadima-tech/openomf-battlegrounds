#include "script.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "de/contents.h"
#include "de/scene.h"
#include "vm/compiler.h"
#include "vm/pcode_parser.h"
#include "vm/vmcode.h"

namespace ed {

de::PCode read_pcode(const deo::Object &behavior) {
	de::Contents c(std::vector<uint8_t>(behavior.contents.begin(), behavior.contents.end()));
	return de::PCode::parse(c);
}

namespace {

void text_block(deo::Writer &w, const std::string &s) { w.block(text_bytes(s)); }

void write_method_chunk(deo::Writer &w, const de::PCodeElement &e) {
	size_t at = w.begin_chunk(9);
	w.u32(e.method_id);
	text_block(w, e.name);
	text_block(w, e.source);
	w.u8(e.virtual_ ? 0 : 1);
	w.u8(e.excluded ? 1 : 0);
	w.u32(0);
	w.u32(0);  // an empty block: the name the method was copied from
	w.end_chunk(at);
}

}  // namespace

Bytes write_pcode(const de::PCode &p) {
	deo::Writer w;
	size_t top = w.begin_chunk(5);
	text_block(w, p.name);
	size_t imp = w.begin_chunk(2);
	text_block(w, "Import");
	w.u32(0);
	w.end_chunk(imp);
	size_t glob = w.begin_chunk(3);
	text_block(w, "Global");
	w.u32(uint32_t(p.global.globals.size()));
	for (auto &g : p.global.globals) {
		w.u32(0);
		text_block(w, g);
	}
	w.end_chunk(glob);
	de::PCodeElement members = p.members;
	members.name = "Members";
	members.method_id = 1;
	members.virtual_ = false;
	write_method_chunk(w, members);
	w.u32(uint32_t(p.elements.size()));
	for (auto &e : p.elements) {
		w.u32(uint32_t(e.type));
		write_method_chunk(w, e);
	}
	for (auto &e : p.elements) w.u32(uint32_t(e.link));
	w.end_chunk(top);
	return w.data();
}

de::PCodeElement pcode_method(const std::string &name, const std::string &source, bool state, int parent, bool virtual_) {
	de::PCodeElement e;
	e.type = de::PCodeElement::kMethod;
	e.name = name;
	e.source = source;
	e.method_id = state ? 0 : 1;
	e.link = parent;
	e.virtual_ = virtual_;
	return e;
}

namespace {

std::string unix_lines(const std::string &s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c != '\r') out += c;
	}
	return out;
}

int count_lines(const std::string &s) { return int(std::count(s.begin(), s.end(), '\n')); }

}  // namespace

ScriptText script_text(const de::PCode &p) {
	ScriptText t;
	t.text = "// Behavior: " + p.name + "\n";
	auto section = [&](const std::string &head, const std::string &source) {
		t.text += "\n// ---- " + head + "\n";
		int line = count_lines(t.text) + 1;
		std::string s = unix_lines(source);
		t.text += s;
		if (!s.empty() && s.back() != '\n') t.text += "\n";
		return line;
	};
	if (!p.global.globals.empty()) {
		std::string g;
		for (auto &n : p.global.globals) g += "// global " + n + "\n";
		section("global Global", g);
	}
	t.members_line = section("members Members", p.members.source);
	for (size_t i = 0; i < p.elements.size(); ++i) {
		auto &e = p.elements[i];
		std::string head = std::string(e.method_id == 0 ? "state " : "method ") + p.qualified_name(i);
		t.element_line.push_back(section(head, e.source));
	}
	t.element_line.push_back(t.members_line);
	return t;
}

namespace {

// The Behavior's references: object chunk, then one chunk holding the globals, the imported
// behaviors and an empty chunk per element (DE_CPCode::ObjProcessReferences).
struct BehaviorReferences {
	RawChunk object;
	uint8_t outer_version = 2, globals_version = 2, imports_version = 2;
	std::vector<Ref> globals;
	Bytes imports;  // the list as stored
};

BehaviorReferences read_behavior_references(const deo::Object &o) {
	BehaviorReferences out;
	Chunks cs = split_chunks(o.references);
	if (cs.chunks.size() < 2) throw deo::Error("a Behavior without its references chunk");
	out.object = cs.chunks[0];
	out.outer_version = cs.chunks[1].version;
	deo::Reader r(cs.chunks[1].body);
	size_t end;
	out.globals_version = r.chunk(&end);
	while (r.pos() < end) {
		if (r.u8()) out.globals.push_back(read_ref(r));
		else out.globals.push_back(Ref{});
	}
	out.imports_version = r.chunk(&end);
	out.imports = r.bytes(end - r.pos());
	return out;
}

}  // namespace

std::vector<Guid> behavior_global_targets(const Project &p, uint32_t behavior) {
	std::vector<Guid> out;
	for (auto &r : read_behavior_references(p.file().objects.at(behavior)).globals) out.push_back(p.target(r).guid);
	return out;
}

void write_behavior(Project &p, uint32_t behavior, const de::PCode &code, const std::vector<Guid> &global_targets) {
	if (global_targets.size() != code.global.globals.size()) throw deo::Error("one target per global is needed");
	BehaviorReferences old = read_behavior_references(p.file().objects.at(behavior));
	std::vector<Ref> globals;
	for (const Guid &g : global_targets) {
		Ref r;
		if (!guid_zero(g)) {
			int local = p.find(g);
			if (local >= 0) r.kind = Ref::Local, r.index = uint32_t(local);
			else r.kind = Ref::Pool, r.index = p.pool_index(g);
		}
		globals.push_back(r);
	}
	deo::Writer w;
	size_t at = w.begin_chunk(old.outer_version);
	size_t g = w.begin_chunk(old.globals_version);
	for (auto &r : globals) {
		if (r.kind == Ref::Null) {
			w.u8(0);
		} else {
			w.u8(1);
			write_ref(w, r);
		}
	}
	w.end_chunk(g);
	size_t im = w.begin_chunk(old.imports_version);
	w.bytes(old.imports);
	w.end_chunk(im);
	for (size_t k = 0; k < code.elements.size(); ++k) {
		size_t e = w.begin_chunk(1);
		w.end_chunk(e);
	}
	w.end_chunk(at);
	Chunks cs;
	cs.chunks.push_back(old.object);
	RawChunk body;
	deo::Reader rr(w.data());
	size_t end;
	body.version = rr.chunk(&end, &body.size);
	body.body = rr.bytes(end - rr.pos());
	cs.chunks.push_back(body);

	deo::Object &o = p.file().objects.at(behavior);
	o.references = join_chunks(cs);
	o.contents = write_pcode(code);
	o.packed.clear();
	o.packed_as_is = false;
	o.info.flags &= ~deo::Object::kCompressed;
	o.info.value84 += 1;
	p.touch();
}

std::string CompileMessage::str() const {
	std::string s = behavior;
	if (!element.empty()) s += "/" + element;
	if (line > 0) s += ":" + std::to_string(line);
	return s + ": " + text;
}

CompileMessage parse_compile_error(const std::string &what) {
	CompileMessage m;
	m.text = what;
	// "<Behavior>/<element>:<line>: message" or "<Behavior>/<element>: message"
	size_t slash = what.find('/');
	size_t colon = what.find(": ");
	if (slash == std::string::npos || colon == std::string::npos || slash > colon) return m;
	m.behavior = what.substr(0, slash);
	std::string where = what.substr(slash + 1, colon - slash - 1);
	size_t c = where.rfind(':');
	if (c != std::string::npos && c + 1 < where.size() && std::all_of(where.begin() + long(c) + 1, where.end(), ::isdigit)) {
		m.line = std::stoi(where.substr(c + 1));
		where = where.substr(0, c);
	}
	m.element = where;
	m.text = what.substr(colon + 2);
	return m;
}

namespace {

struct CompileLog {
	std::map<uint64_t, std::vector<CompileMessage>> by_class;  // class GUID hash -> errors
	std::vector<CompileMessage> all;
	bool once = false;
};
std::mutex g_log_mutex;
std::map<const Project *, CompileLog> g_logs;

uint64_t guid_hash(const Guid &g) {
	uint64_t a, b;
	memcpy(&a, g.b, 8);
	memcpy(&b, g.b + 8, 8);
	return a ^ (b * 0x9E3779B97F4A7C15ull);
}

void record(const Project &p, const Guid &cls, std::vector<CompileMessage> errors) {
	std::lock_guard<std::mutex> lock(g_log_mutex);
	CompileLog &l = g_logs[&p];
	l.once = true;
	l.by_class[guid_hash(cls)] = std::move(errors);
	l.all.clear();
	for (auto &[k, v] : l.by_class) l.all.insert(l.all.end(), v.begin(), v.end());
}

// Places an error in the add-on Behavior it names, at its line in script_text().
void locate(const Project &p, uint32_t cls, CompileMessage &m) {
	for (auto &r : object_references("Class", p.file().objects[cls].references)) {
		int b = p.target(r).local;
		if (b < 0 || p.file().objects[size_t(b)].info.name_text() != m.behavior) continue;
		m.object = b;
		try {
			de::PCode code = read_pcode(p.file().objects[size_t(b)]);
			ScriptText t = script_text(code);
			int start = 0;
			if (m.element == "Members") start = t.members_line;
			for (size_t i = 0; i < code.elements.size(); ++i) {
				if (code.qualified_name(i) == m.element) start = t.element_line[i];
			}
			if (start > 0) m.text_line = start + std::max(m.line, 1) - 1;
		} catch (const std::exception &) {
		}
		return;
	}
}

}  // namespace

const std::vector<CompileMessage> &last_compile_errors(const Project &p) {
	std::lock_guard<std::mutex> lock(g_log_mutex);
	return g_logs[&p].all;
}

bool compiled_once(const Project &p) {
	std::lock_guard<std::mutex> lock(g_log_mutex);
	return g_logs[&p].once;
}

bool change_information(const Project &p, uint32_t class_object, std::vector<uint8_t> *out) {
	out->clear();
	auto value84 = [&](const Guid &g, int32_t *v) {
		int local = p.find(g);
		if (local >= 0) {
			*v = p.file().objects[size_t(local)].info.value84;
			return true;
		}
		const BaseObject *b = p.base().by_guid(g);
		if (!b) return false;
		*v = p.base().model(b->file).objects[b->index].info.value84;
		return true;
	};
	auto imports = [&](const Guid &g) {
		std::vector<Guid> out;
		const deo::Object *o = nullptr;
		const deo::File *file = nullptr;
		int local = p.find(g);
		if (local >= 0) o = &p.file().objects[size_t(local)], file = &p.file();
		else if (const BaseObject *b = p.base().by_guid(g)) file = &p.base().model(b->file), o = &file->objects[b->index];
		if (!o) return out;
		Chunks cs = split_chunks(o->references);
		if (cs.chunks.size() < 2) return out;
		try {
			deo::Reader r(cs.chunks[1].body);
			size_t end;
			r.chunk(&end);
			r.seek(end);
			r.chunk(&end);
			while (r.pos() < end && r.u8() == 1) {
				Ref ref = read_ref(r);
				Guid t = ref.kind == Ref::Local && ref.index < file->objects.size() ? file->objects[ref.index].info.guid
				         : ref.kind == Ref::Pool && ref.index < file->guid_pool.size() ? file->guid_pool[ref.index]
				                                                                       : Guid{};
				if (!guid_zero(t)) out.push_back(t);
			}
		} catch (const deo::Error &) {
		}
		return out;
	};
	bool ok = true;
	auto put = [&](int32_t v) {
		for (int k = 0; k < 4; ++k) out->push_back(uint8_t(uint32_t(v) >> (8 * k)));
	};
	for (auto &r : object_references("Class", p.file().objects.at(class_object).references)) {
		Guid g = p.target(r).guid;
		int32_t v = 0;
		if (!value84(g, &v)) {
			ok = false;
			continue;
		}
		put(v);
		for (const Guid &t : imports(g)) {
			int32_t w = 0;
			if (value84(t, &w)) put(w);
			else ok = false;
		}
	}
	return ok;
}

ScriptCompiler::ScriptCompiler(std::filesystem::path data_dir) : dir_(std::move(data_dir)) {}
ScriptCompiler::~ScriptCompiler() {
	if (loader_.joinable()) loader_.join();
}

void ScriptCompiler::preload() {
	if (loader_.joinable() || loaded()) return;
	loader_ = std::thread([this] {
		std::lock_guard<std::mutex> lock(mutex_);
		world();
	});
}

bool ScriptCompiler::loaded() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return world_ != nullptr;
}

de::World &ScriptCompiler::world() {
	if (!world_) world_ = std::make_unique<de::World>(dir_);
	return *world_;
}

ScriptCompiler::Result ScriptCompiler::compile(Project &p, uint32_t class_object) {
	std::lock_guard<std::mutex> lock(mutex_);
	Result res;
	de::World &w = world();
	const std::string stem = p.path().stem().string().empty() ? "EditorAddon" : p.path().stem().string();
	w.set_file(stem, p.bytes());
	de::Guid g;
	memcpy(g.bytes, p.file().objects.at(class_object).info.guid.b, 16);
	de::ObjectRef cls = w.by_guid(g);
	if (!cls) {
		res.errors.push_back({"", "", 0, "the class is not in the compiler's view of the add-on"});
		return res;
	}
	try {
		vm::VMClass code = vm::compile_class(w, cls, &res.notes);
		Bytes payload = code.write();
		deo::Object &o = p.file().objects.at(class_object);
		Bytes key = text_bytes("VMC2");
		auto it = std::find_if(o.ex.begin(), o.ex.end(), [&](const deo::Ex &e) { return e.key == key; });
		if (it == o.ex.end()) {
			o.ex.push_back({key, 1, std::move(payload)});
		} else if (it->data != payload) {
			it->data = std::move(payload);
		}
		p.touch();
		res.ok = true;
	} catch (const vm::pc::CompileError &e) {
		res.errors.push_back(parse_compile_error(e.what()));
	} catch (const std::exception &e) {
		res.errors.push_back({"", "", 0, e.what()});
	}
	for (auto &m : res.errors) locate(p, class_object, m);
	record(p, p.file().objects.at(class_object).info.guid, res.errors);
	return res;
}

std::vector<uint32_t> classes_attaching(const Project &p, uint32_t behavior) {
	std::vector<uint32_t> out;
	const auto &objs = p.file().objects;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].info.class_text() != "Class") continue;
		for (auto &r : object_references("Class", objs[i].references)) {
			if (p.target(r).local == int(behavior)) {
				out.push_back(i);
				break;
			}
		}
	}
	return out;
}

std::vector<uint32_t> addon_script_classes(const Project &p) {
	std::set<uint32_t> out;
	const auto &objs = p.file().objects;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].info.class_text() == "Behavior") {
			for (uint32_t c : classes_attaching(p, i)) out.insert(c);
		}
	}
	return {out.begin(), out.end()};
}

std::vector<std::pair<uint32_t, ScriptCompiler::Result>> ScriptCompiler::compile_all(Project &p) {
	std::vector<std::pair<uint32_t, Result>> out;
	for (uint32_t c : addon_script_classes(p)) out.emplace_back(c, compile(p, c));
	return out;
}

}  // namespace ed
