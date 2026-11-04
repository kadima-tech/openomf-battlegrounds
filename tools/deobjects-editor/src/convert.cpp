#include "convert.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "schema.h"

namespace deo {

namespace {

using json::Value;

// An object's name for display: its stored bytes without the NUL, as UTF-8.
std::string display_name(const Info &info) {
	std::string n = info.name_text();
	return latin1_to_utf8(Bytes(n.begin(), n.end()));
}

// ---------------------------------------------------------------------------------------------
// Small encoders shared by the whole file

// Long hex is split into lines of 32 bytes so it stays readable in an editor.
Value hex_value(const Bytes &b) {
	std::string h = to_hex(b);
	if (h.size() <= 64) return Value(h);
	Value a = Value::array();
	for (size_t i = 0; i < h.size(); i += 64) a.push(Value(h.substr(i, 64)));
	return a;
}

Bytes hex_from(const Value &v) {
	if (v.is_string()) return from_hex(v.as_string());
	std::string all;
	for (const Value &line : v.items()) all += line.as_string();
	return from_hex(all);
}

// Stored text (u32 length + bytes, NUL included): a JSON string when it is plain text,
// otherwise {"hex": ...}.
bool is_plain_text(const Bytes &raw) {
	if (raw.size() < 2 || raw.back() != 0) return false;
	return memchr(raw.data(), 0, raw.size() - 1) == nullptr;
}

Value text_value(const Bytes &raw) {
	if (raw.empty()) return Value("");
	if (is_plain_text(raw)) return Value(latin1_to_utf8(Bytes(raw.begin(), raw.end() - 1)));
	Value o = Value::object();
	o["hex"] = hex_value(raw);
	return o;
}

Bytes text_from(const Value &v) {
	if (v.is_object()) return hex_from(v.at("hex"));
	const std::string &s = v.as_string();
	if (s.empty()) return {};
	Bytes b = utf8_to_latin1(s);
	b.push_back(0);
	return b;
}

// Shortest decimal that reads back as the same float; non-finite values as their bit pattern.
Value float_value(float f) {
	if (!std::isfinite(f)) {
		uint32_t bits;
		memcpy(&bits, &f, 4);
		char buf[16];
		snprintf(buf, sizeof buf, "0x%08x", bits);
		return Value(buf);
	}
	char buf[40];
	for (int prec = 1; prec <= 9; ++prec) {
		snprintf(buf, sizeof buf, "%.*g", prec, double(f));
		if (strtof(buf, nullptr) == f) break;
	}
	return Value(strtod(buf, nullptr));
}

float float_from(const Value &v) {
	if (v.is_string()) {
		uint32_t bits = uint32_t(strtoul(v.as_string().c_str(), nullptr, 16));
		float f;
		memcpy(&f, &bits, 4);
		return f;
	}
	return float(v.as_real());
}

int64_t int_in(const Value &v, int64_t lo, int64_t hi, const char *what) {
	int64_t n = v.as_int();
	if (n < lo || n > hi) {
		throw Error(std::string(what) + " " + std::to_string(n) + " is out of range (" + std::to_string(lo) + ".." +
		            std::to_string(hi) + ")");
	}
	return n;
}

Value bool_value(uint8_t b) { return b <= 1 ? Value(b == 1) : Value(int64_t(b)); }

uint8_t bool_from(const Value &v) {
	if (v.is_bool()) return v.as_bool() ? 1 : 0;
	return uint8_t(int_in(v, 0, 255, "BOOL"));
}

// ---------------------------------------------------------------------------------------------
// References

struct RefContext {
	size_t objects = 0;
	size_t pool = 0;
	std::function<std::string(size_t)> name_of;
	std::vector<std::string> *warnings = nullptr;
	const GuidNames *names = nullptr;  // other files' objects, for display
	std::function<std::string(size_t)> pool_guid;
};

// DE_CTransferAgent::ExtractReference (0x70057690). Shown as "#<index> <name>" for an object in
// this file, "^<pool index> <guid>" for one in another file, null for none.
std::optional<Value> read_ref(Reader &r, const RefContext &ctx, const File *f) {
	uint8_t tag = r.u8();
	bool local;
	uint32_t index;
	if (tag >= 0x32) {
		local = true, index = tag - 0x32u;
	} else if (tag == 1) {
		local = true, index = r.u16();
	} else if (tag == 2) {
		local = false, index = r.u8();
	} else if (tag == 3) {
		local = false, index = r.u16();
	} else if (tag == 4) {
		local = false, index = r.u32();
	} else if (tag == 0) {
		return Value();
	} else {
		return std::nullopt;
	}
	if (local) {
		if (index >= ctx.objects) return std::nullopt;
		return Value("#" + std::to_string(index) + " " + ctx.name_of(index));
	}
	if (index >= ctx.pool) return std::nullopt;
	std::string g = f->guid_pool[index].str();
	std::string s = "^" + std::to_string(index) + " " + g;
	if (ctx.names) {
		auto it = ctx.names->find(g);
		if (it != ctx.names->end()) s += " " + it->second;
	}
	return Value(s);
}

void write_ref(Writer &w, const Value &v, const RefContext &ctx) {
	if (v.is_null()) {
		w.u8(0);
		return;
	}
	const std::string &s = v.as_string();
	if (s.empty() || (s[0] != '#' && s[0] != '^')) {
		throw Error("reference \"" + s + "\" must start with # (object in this file) or ^ (GUID pool entry)");
	}
	char *end = nullptr;
	unsigned long index = strtoul(s.c_str() + 1, &end, 10);
	if (end == s.c_str() + 1) throw Error("reference \"" + s + "\" has no index");
	std::string rest = end;
	while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
	if (s[0] == '#') {
		if (index >= ctx.objects) throw Error("reference \"" + s + "\": there is no object #" + std::to_string(index));
		if (!rest.empty() && ctx.warnings && rest != ctx.name_of(index)) {
			ctx.warnings->push_back("reference \"" + s + "\" points at object #" + std::to_string(index) + " \"" +
			                        ctx.name_of(index) + "\"");
		}
		if (index <= 0xFF - 0x32) {
			w.u8(uint8_t(0x32 + index));
		} else if (index <= 0xFFFF) {
			w.u8(1);
			w.u16(uint16_t(index));
		} else {
			throw Error("object index too large for a reference");
		}
		return;
	}
	if (index >= ctx.pool) throw Error("reference \"" + s + "\": there is no GUID pool entry " + std::to_string(index));
	if (ctx.warnings && rest.size() >= 36 && ctx.pool_guid && rest.compare(0, 36, ctx.pool_guid(index)) != 0) {
		ctx.warnings->push_back("reference \"" + s + "\": GUID pool entry " + std::to_string(index) + " is " +
		                        ctx.pool_guid(index));
	}
	if (index <= 0xFF) {
		w.u8(2);
		w.u8(uint8_t(index));
	} else if (index <= 0xFFFF) {
		w.u8(3);
		w.u16(uint16_t(index));
	} else {
		w.u8(4);
		w.u32(uint32_t(index));
	}
}

// ---------------------------------------------------------------------------------------------
// Fields

std::optional<Value> read_field(Reader &r, FieldType t, const RefContext &ctx, const File *f) {
	switch (t) {
	case FieldType::U8: return Value(int64_t(r.u8()));
	case FieldType::Bool: return bool_value(r.u8());
	case FieldType::I32: return Value(int64_t(int32_t(r.u32())));
	case FieldType::U32: return Value(int64_t(r.u32()));
	case FieldType::F32: {
		uint32_t bits = r.u32();
		float v;
		memcpy(&v, &bits, 4);
		return float_value(v);
	}
	case FieldType::Text: return text_value(r.block());
	case FieldType::Guid: return Value(r.guid().str());
	case FieldType::PassiveRef: {
		uint8_t set = r.u8();
		if (set == 0) return Value();
		if (set != 1) return std::nullopt;
		ChunkSize size;
		size_t end = r.sized(&size);
		if (end - r.pos() != 32 || size.long_size) return std::nullopt;
		Value o = Value::object();
		o["file"] = Value(r.guid().str());
		o["object"] = Value(r.guid().str());
		return o;
	}
	case FieldType::Ref: return read_ref(r, ctx, f);
	case FieldType::OptRef: {
		uint8_t set = r.u8();
		if (set == 0) return Value();
		if (set != 1) return std::nullopt;
		auto v = read_ref(r, ctx, f);
		if (!v || v->is_null()) return std::nullopt;
		return v;
	}
	case FieldType::RefList: {
		Value list = Value::array();
		while (true) {
			uint8_t more = r.u8();
			if (more == 0) break;
			if (more != 1) return std::nullopt;
			auto v = read_ref(r, ctx, f);
			if (!v) return std::nullopt;
			list.push(*v);
		}
		return list;
	}
	}
	return std::nullopt;
}

void write_field(Writer &w, FieldType t, const Value &v, const RefContext &ctx) {
	switch (t) {
	case FieldType::U8: w.u8(uint8_t(int_in(v, 0, 255, "u8"))); return;
	case FieldType::Bool: w.u8(bool_from(v)); return;
	case FieldType::I32: w.u32(uint32_t(int32_t(int_in(v, INT32_MIN, INT32_MAX, "i32")))); return;
	case FieldType::U32: w.u32(uint32_t(int_in(v, 0, UINT32_MAX, "u32"))); return;
	case FieldType::F32: {
		float f = float_from(v);
		uint32_t bits;
		memcpy(&bits, &f, 4);
		w.u32(bits);
		return;
	}
	case FieldType::Text: w.block(text_from(v)); return;
	case FieldType::Guid: w.guid(Guid::parse(v.as_string())); return;
	case FieldType::PassiveRef:
		if (v.is_null()) {
			w.u8(0);
			return;
		}
		w.u8(1);
		w.u8(32);
		w.guid(Guid::parse(v.at("file").as_string()));
		w.guid(Guid::parse(v.at("object").as_string()));
		return;
	case FieldType::Ref: write_ref(w, v, ctx); return;
	case FieldType::OptRef:
		if (v.is_null()) {
			w.u8(0);
			return;
		}
		w.u8(1);
		write_ref(w, v, ctx);
		return;
	case FieldType::RefList:
		for (const Value &e : v.items()) {
			w.u8(1);
			write_ref(w, e, ctx);
		}
		w.u8(0);
		return;
	}
}

// ---------------------------------------------------------------------------------------------
// Chunks

void encode_fields(Writer &w, const ChunkSchema &s, const Value &fields, const RefContext &ctx) {
	// Every key must be a field of the schema; fields are written in schema order, and a field
	// may only be left out together with all the fields after it.
	for (const auto &m : fields.members()) {
		bool known = false;
		for (const Field &fd : s.fields) known = known || m.first == fd.name;
		if (!known) throw Error(std::string("\"") + m.first + "\" is not a field of " + s.label);
	}
	bool ended = false;
	for (const Field &fd : s.fields) {
		const Value *v = fields.find(fd.name);
		if (!v) {
			ended = true;
			continue;
		}
		if (ended) {
			throw Error(std::string(s.label) + " field \"" + fd.name + "\" is given but an earlier field is missing");
		}
		try {
			write_field(w, fd.type, *v, ctx);
		} catch (const std::exception &e) {
			throw Error(std::string(s.label) + " field \"" + fd.name + "\" (" + type_name(fd.type) + "): " + e.what());
		}
	}
}

// One chunk's body decoded with its schema; nullopt unless the decoded fields encode back to
// exactly the same bytes.
std::optional<Value> decode_with_schema(const Bytes &data, size_t body, size_t end, int version, const ChunkSchema &s,
                                        const RefContext &ctx, const File *f) {
	Reader r(data, body, end);
	Value fields = Value::object();
	try {
		for (const Field &fd : s.fields) {
			if (version < fd.min_version || r.remaining() == 0) break;
			auto v = read_field(r, fd.type, ctx, f);
			if (!v) return std::nullopt;
			fields[fd.name] = std::move(*v);
		}
	} catch (const Error &) {
		return std::nullopt;
	}
	Bytes rest(data.begin() + long(r.pos()), data.begin() + long(end));
	Writer w;
	RefContext quiet = ctx;
	quiet.warnings = nullptr;
	try {
		encode_fields(w, s, fields, quiet);
	} catch (const std::exception &) {
		return std::nullopt;
	}
	w.bytes(rest);
	if (w.data() != Bytes(data.begin() + long(body), data.begin() + long(end))) return std::nullopt;
	Value out = Value::object();
	out["fields"] = std::move(fields);
	if (!rest.empty()) out["rest"] = hex_value(rest);
	return out;
}

// A references/hierarchy chunk with no schema: BOOL-terminated lists of references, the way
// the engine's reference_lists() reads them.
std::optional<Value> decode_lists(const Bytes &data, size_t body, size_t end, const RefContext &ctx, const File *f) {
	Reader r(data, body, end);
	Value lists = Value::array();
	try {
		while (r.remaining()) {
			auto v = read_field(r, FieldType::RefList, ctx, f);
			if (!v) return std::nullopt;
			lists.push(std::move(*v));
		}
	} catch (const Error &) {
		return std::nullopt;
	}
	// Collapse the trailing run of empty lists (often filler zeros).
	int64_t empty = 0;
	auto &items = lists.items();
	while (!items.empty() && items.back().size() == 0) {
		items.pop_back();
		++empty;
	}
	Value out = Value::object();
	out["lists"] = std::move(lists);
	if (empty) out["empty_lists"] = Value(empty);
	return out;
}

const char *block_name(Block b) {
	return b == Block::Contents ? "contents" : b == Block::References ? "references" : "hierarchy";
}

// A whole block: an array of chunks, or {"hex": ...} when it is not a clean run of chunks.
Value block_to_json(Block block, const Bytes &data, const std::string &class_path, const RefContext &ctx,
                    const File *f) {
	Value chunks = Value::array();
	Reader r(data);
	try {
		while (r.remaining()) {
			size_t end;
			ChunkSize size;
			int version = r.chunk(&end, &size);
			size_t body = r.pos();
			size_t index = chunks.size();
			Value c = Value::object();
			const ChunkSchema *s = find_schema(block, class_path, index);
			if (s) c["class"] = Value(s->label);
			c["version"] = Value(int64_t(version));
			if (size.long_size) c["long_size"] = Value(true);
			std::optional<Value> decoded;
			if (s) decoded = decode_with_schema(data, body, end, version, *s, ctx, f);
			if (!decoded && block != Block::Contents && end > body) decoded = decode_lists(data, body, end, ctx, f);
			if (decoded) {
				for (auto &m : decoded->members()) c[m.first] = std::move(m.second);
			} else if (end > body) {
				c["hex"] = hex_value(Bytes(data.begin() + long(body), data.begin() + long(end)));
			}
			chunks.push(std::move(c));
			r.seek(end);
		}
	} catch (const Error &) {
		Value o = Value::object();
		o["hex"] = hex_value(data);
		return o;
	}
	return chunks;
}

Bytes block_from_json(Block block, const Value &v, const std::string &class_path, const RefContext &ctx) {
	if (v.is_object()) return hex_from(v.at("hex"));
	Writer w;
	size_t index = 0;
	for (const Value &c : v.items()) {
		ChunkSize size;
		if (const Value *l = c.find("long_size")) size.long_size = l->as_bool();
		int version = int(int_in(c.at("version"), 0, 255, "chunk version"));
		size_t at = w.begin_chunk(uint8_t(version));
		try {
			if (const Value *fields = c.find("fields")) {
				const ChunkSchema *s = find_schema(block, class_path, index);
				if (!s) throw Error("has \"fields\" but there is no schema for it");
				encode_fields(w, *s, *fields, ctx);
				if (const Value *rest = c.find("rest")) w.bytes(hex_from(*rest));
			} else if (const Value *lists = c.find("lists")) {
				for (const Value &list : lists->items()) write_field(w, FieldType::RefList, list, ctx);
				if (const Value *n = c.find("empty_lists")) {
					for (int64_t k = int_in(*n, 0, 1 << 30, "empty_lists"); k > 0; --k) w.u8(0);
				}
			} else if (const Value *hex = c.find("hex")) {
				w.bytes(hex_from(*hex));
			}
		} catch (const std::exception &e) {
			throw Error(std::string(block_name(block)) + " chunk " + std::to_string(index) + ": " + e.what());
		}
		w.end_chunk(at, size);
		++index;
	}
	return std::move(w.data());
}

// ---------------------------------------------------------------------------------------------
// Extended data payloads

bool is_printable_text(const Bytes &b) {
	if (!is_plain_text(b)) return false;
	for (size_t i = 0; i + 1 < b.size(); ++i) {
		uint8_t c = b[i];
		if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') return false;
	}
	return true;
}

Value payload_to_json(const Bytes &data, size_t object, const std::string &tag, const SaveBlob &save) {
	Value o = Value::object();
	if (is_printable_text(data)) {
		o["text"] = Value(latin1_to_utf8(Bytes(data.begin(), data.end() - 1)));
	} else if (data.size() <= kInlineLimit) {
		o["hex"] = hex_value(data);
	} else {
		std::string path = save ? save(object, tag, data) : std::string();
		if (path.empty()) {
			o["size"] = Value(int64_t(data.size()));
		} else {
			o["file"] = Value(path);
		}
	}
	return o;
}

Bytes payload_from_json(const Value &v, const LoadBlob &load) {
	if (const Value *t = v.find("text")) {
		Bytes b = utf8_to_latin1(t->as_string());
		b.push_back(0);
		return b;
	}
	if (const Value *h = v.find("hex")) return hex_from(*h);
	if (const Value *p = v.find("file")) return load(p->as_string());
	throw Error("payload has no \"text\", \"hex\" or \"file\" (unpack writes large payloads to files)");
}

// ---------------------------------------------------------------------------------------------
// Header entries

Value info_to_json(const Info &info) {
	Value o = Value::object();
	o["version"] = Value(int64_t(info.version));
	if (info.version >= 3) {
		o["flag"] = Value(int64_t(info.flag));
		o["value84"] = Value(int64_t(info.value84));
	}
	if (info.version >= 4) o["flags"] = Value(int64_t(info.flags));
	if (!info.base_rest.empty()) o["rest"] = hex_value(info.base_rest);
	if (info.base_size.long_size) o["long_size"] = Value(true);
	if (!info.ext_rest.empty()) o["extension_rest"] = hex_value(info.ext_rest);
	if (info.ext_size.long_size) o["extension_long_size"] = Value(true);
	return o;
}

void info_from_json(Info &info, const Value &o) {
	info.version = uint8_t(int_in(o.at("version"), 1, 4, "info version"));
	if (const Value *v = o.find("flag")) info.flag = uint8_t(int_in(*v, 0, 255, "flag"));
	if (const Value *v = o.find("value84")) info.value84 = int32_t(int_in(*v, INT32_MIN, INT32_MAX, "value84"));
	if (const Value *v = o.find("flags")) info.flags = uint32_t(int_in(*v, 0, UINT32_MAX, "flags"));
	if (const Value *v = o.find("rest")) info.base_rest = hex_from(*v);
	if (const Value *v = o.find("long_size")) info.base_size.long_size = v->as_bool();
	if (const Value *v = o.find("extension_rest")) info.ext_rest = hex_from(*v);
	if (const Value *v = o.find("extension_long_size")) info.ext_size.long_size = v->as_bool();
}

RefContext context_for(const File &f) {
	RefContext ctx;
	ctx.objects = f.objects.size();
	ctx.pool = f.guid_pool.size();
	ctx.name_of = [&f](size_t i) { return display_name(f.objects[i].info); };
	ctx.pool_guid = [&f](size_t i) { return f.guid_pool[i].str(); };
	return ctx;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public entry points

json::Value index_to_json(const File &f, const std::vector<std::string> &object_paths) {
	Value o = Value::object();
	o["tool"] = Value("deobjects-edit");
	o["layout_version"] = Value(int64_t(1));
	o["signature"] = text_value(f.signature);
	o["format"] = Value(int64_t(f.format));
	Value header = Value::object();
	header["version"] = Value(int64_t(f.header_version));
	if (f.header_size.long_size) header["long_size"] = Value(true);
	if (!f.header_rest.empty()) header["rest"] = hex_value(f.header_rest);
	o["header"] = std::move(header);

	Value agent = Value::object();
	agent["name"] = text_value(f.master.name);
	agent["class"] = text_value(f.master.class_path);
	agent["guid"] = Value(f.master.guid.str());
	agent["info"] = info_to_json(f.master);
	agent["data_offset"] = Value(int64_t(f.master.data_offset));
	Value own = Value::object();
	own["version"] = Value(int64_t(f.own_version));
	if (f.own_size.long_size) own["long_size"] = Value(true);
	if (f.own_version == 1) {
		own["block"] = hex_value(f.own_block);
		own["guid"] = Value(f.own_guid.str());
		own["pool_flag"] = Value(int64_t(f.pool_flag));
		own["bool_108"] = Value(int64_t(f.own_bool));
	}
	if (!f.own_rest.empty()) own["rest"] = hex_value(f.own_rest);
	agent["contents"] = std::move(own);
	o["agent"] = std::move(agent);

	Value pool = Value::array();
	for (const Guid &g : f.guid_pool) pool.push(Value(g.str()));
	o["guid_pool"] = std::move(pool);

	Value list = Value::array();
	for (const std::string &p : object_paths) list.push(Value(p));
	o["objects"] = std::move(list);
	if (!f.trailing.empty()) o["trailing"] = hex_value(f.trailing);
	return o;
}

json::Value object_to_json(const File &f, size_t index, const SaveBlob &save, const GuidNames *names) {
	const Object &obj = f.objects[index];
	const std::string cls = obj.info.class_text();
	RefContext ctx = context_for(f);
	ctx.names = names;
	Value o = Value::object();
	o["index"] = Value(int64_t(index));
	o["name"] = text_value(obj.info.name);
	o["class"] = text_value(obj.info.class_path);
	o["guid"] = Value(obj.info.guid.str());
	o["info"] = info_to_json(obj.info);
	if (obj.compressed() && !obj.packed.empty()) {
		std::string path = save ? save(index, "packed", obj.packed) : std::string();
		if (!path.empty()) o["packed_file"] = Value(path);
	}
	o["contents"] = block_to_json(Block::Contents, obj.contents, cls, ctx, &f);
	o["references"] = block_to_json(Block::References, obj.references, cls, ctx, &f);
	o["hierarchy"] = block_to_json(Block::Hierarchy, obj.hierarchy, cls, ctx, &f);
	Value ex = Value::array();
	for (size_t k = 0; k < obj.ex.size(); ++k) {
		const Ex &e = obj.ex[k];
		Value entry = Value::object();
		entry["key"] = text_value(e.key);
		entry["flag"] = Value(int64_t(e.flag));
		Value payload = payload_to_json(e.data, index, "ex" + std::to_string(k), save);
		for (auto &m : payload.members()) entry[m.first] = std::move(m.second);
		ex.push(std::move(entry));
	}
	o["ex"] = std::move(ex);
	return o;
}

File file_from_json(const json::Value &index, const std::vector<json::Value> &objects, const LoadBlob &load,
                    std::vector<std::string> *warnings) {
	File f;
	f.signature = text_from(index.at("signature"));
	f.format = uint8_t(int_in(index.at("format"), 0, 255, "format"));
	const Value &header = index.at("header");
	f.header_version = uint8_t(int_in(header.at("version"), 0, 255, "header version"));
	if (const Value *v = header.find("long_size")) f.header_size.long_size = v->as_bool();
	if (const Value *v = header.find("rest")) f.header_rest = hex_from(*v);

	const Value &agent = index.at("agent");
	f.master.name = text_from(agent.at("name"));
	f.master.class_path = text_from(agent.at("class"));
	f.master.guid = Guid::parse(agent.at("guid").as_string());
	info_from_json(f.master, agent.at("info"));
	f.master.data_offset = uint32_t(int_in(agent.at("data_offset"), 0, UINT32_MAX, "data_offset"));
	const Value &own = agent.at("contents");
	f.own_version = uint8_t(int_in(own.at("version"), 0, 255, "agent contents version"));
	if (const Value *v = own.find("long_size")) f.own_size.long_size = v->as_bool();
	if (f.own_version == 1) {
		f.own_block = hex_from(own.at("block"));
		f.own_guid = Guid::parse(own.at("guid").as_string());
		f.pool_flag = uint8_t(int_in(own.at("pool_flag"), 0, 255, "pool_flag"));
		f.own_bool = uint8_t(int_in(own.at("bool_108"), 0, 255, "bool_108"));
	}
	if (const Value *v = own.find("rest")) f.own_rest = hex_from(*v);
	for (const Value &g : index.at("guid_pool").items()) f.guid_pool.push_back(Guid::parse(g.as_string()));
	if (const Value *v = index.find("trailing")) f.trailing = hex_from(*v);

	// Names and classes first: references are checked against them.
	f.objects.resize(objects.size());
	for (size_t i = 0; i < objects.size(); ++i) {
		const Value &o = objects[i];
		try {
			Info &info = f.objects[i].info;
			info.name = text_from(o.at("name"));
			info.class_path = text_from(o.at("class"));
			info.guid = Guid::parse(o.at("guid").as_string());
			info_from_json(info, o.at("info"));
		} catch (const std::exception &e) {
			throw Error("object " + std::to_string(i) + ": " + e.what());
		}
	}
	RefContext ctx = context_for(f);
	std::vector<std::string> notes;
	ctx.warnings = &notes;
	for (size_t i = 0; i < objects.size(); ++i) {
		const Value &o = objects[i];
		Object &obj = f.objects[i];
		const std::string cls = obj.info.class_text();
		std::string label = "object " + std::to_string(i) + " (" + display_name(obj.info) + ")";
		try {
			obj.contents = block_from_json(Block::Contents, o.at("contents"), cls, ctx);
			obj.references = block_from_json(Block::References, o.at("references"), cls, ctx);
			obj.hierarchy = block_from_json(Block::Hierarchy, o.at("hierarchy"), cls, ctx);
			if (const Value *p = o.find("packed_file")) obj.packed = load(p->as_string());
			for (const Value &entry : o.at("ex").items()) {
				Ex e;
				e.key = text_from(entry.at("key"));
				e.flag = uint8_t(int_in(entry.at("flag"), 0, 255, "flag"));
				e.data = payload_from_json(entry, load);
				obj.ex.push_back(std::move(e));
			}
		} catch (const std::exception &e) {
			throw Error(label + ": " + e.what());
		}
		if (warnings) {
			for (auto &w : notes) warnings->push_back(label + ": " + w);
		}
		notes.clear();
	}
	return f;
}

std::string object_file_name(const File &f, size_t index) {
	char num[16];
	snprintf(num, sizeof num, "%04zu", index);
	std::string name;
	for (char ch : f.objects[index].info.name_text()) {
		uint8_t c = uint8_t(ch);
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
		          c == '_' || c == '.' || c == '\'' || c == '(' || c == ')';
		name += ok ? char(c) : '_';
		if (name.size() >= 48) break;
	}
	while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
	return std::string(num) + (name.empty() ? "" : " " + name) + ".json";
}

}  // namespace deo
