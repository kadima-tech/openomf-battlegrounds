#include "de/pcode.h"

#include "de/objects_file.h"

namespace de {

namespace {

std::string block_text(Contents &c) {
	auto b = c.block();
	size_t n = b.size();
	while (n && b[n - 1] == 0) {
		--n;
	}
	return std::string(b.begin(), b.begin() + n);
}

// DE_CPCodeSourceElement (Inclusion, Class) and DE_CPCodeImport: name, source.
void read_source_element(Contents &c, PCodeElement &e) {
	Chunk ch = begin_chunk(c);
	if (ch.version >= 1) {
		e.name = block_text(c);
		e.source = block_text(c);
	}
	c.seek(ch.end);
}

// DE_CPCodeGlobal: v1 name+source; v2 name + [name]; v3 name + [i32, name].
void read_global(Contents &c, PCodeElement &e) {
	Chunk ch = begin_chunk(c);
	e.name = block_text(c);
	if (ch.version == 1) {
		e.source = block_text(c);
	} else if (ch.version >= 2) {
		int32_t n = c.i32();
		for (int32_t i = 0; i < n; ++i) {
			if (ch.version >= 3) {
				c.i32();
			}
			e.globals.push_back(block_text(c));
		}
	}
	c.seek(ch.end);
}

// DE_CPCodeMethod: v9 prefixes a u32; every version starts name, source.
void read_method(Contents &c, PCodeElement &e) {
	Chunk ch = begin_chunk(c);
	if (ch.version >= 9) {
		e.method_id = c.u32();
	}
	e.name = block_text(c);
	e.source = block_text(c);
	if (c.pos() < ch.end) e.virtual_ = c.u8() == 0;
	if (c.pos() < ch.end) e.excluded = c.u8() != 0;
	c.seek(ch.end);
}

}  // namespace

PCode PCode::parse(Contents &c) {
	PCode p;
	p.import.type = PCodeElement::kImport;
	p.global.type = PCodeElement::kGlobal;
	p.members.type = PCodeElement::kMembers;

	Chunk ch = begin_chunk(c);
	if (ch.version < 1 || ch.version > 5) {
		throw FormatError("PCode: unsupported version " + std::to_string(ch.version));
	}
	p.name = block_text(c);
	if (ch.version >= 3) {
		read_source_element(c, p.import);
	}
	if (ch.version >= 5) {
		read_global(c, p.global);
		read_method(c, p.members);
	}
	if (ch.version >= 2) {
		int32_t n = c.i32();
		for (int32_t i = 0; i < n; ++i) {
			PCodeElement e;
			e.type = static_cast<PCodeElement::Type>(c.i32());
			switch (e.type) {
			case PCodeElement::kMethod:
				read_method(c, e);
				break;
			case PCodeElement::kInclusion:
			case PCodeElement::kClass:
				read_source_element(c, e);
				break;
			default:
				throw FormatError("PCode: unknown element type " + std::to_string(e.type));
			}
			p.elements.push_back(std::move(e));
		}
		for (auto &e : p.elements) {
			e.link = c.i32();
		}
	}
	c.seek(ch.end);
	return p;
}

std::string PCode::qualified_name(size_t index) const {
	std::string name = elements[index].name;
	int32_t up = elements[index].link;
	for (int guard = 0; up >= 0 && size_t(up) < elements.size() && guard < 64; ++guard) {
		name = elements[up].name + "." + name;
		up = elements[up].link;
	}
	return name;
}

std::string PCode::text() const {
	std::string out = "// Behavior: " + name + "\n";
	auto section = [&](const char *label, const PCodeElement &e) {
		if (e.source.empty() && e.globals.empty()) {
			return;
		}
		out += "\n// ---- " + std::string(label) + " " + e.name + "\n";
		for (auto &g : e.globals) {
			out += "// global " + g + "\n";
		}
		out += e.source;
		if (!e.source.empty() && e.source.back() != '\n') {
			out += "\n";
		}
	};
	section("import", import);
	section("global", global);
	section("members", members);
	for (size_t i = 0; i < elements.size(); ++i) {
		auto &e = elements[i];
		const char *kind = e.type == PCodeElement::kMethod ? "method" : e.type == PCodeElement::kClass ? "class" : "include";
		PCodeElement named = e;
		named.name = qualified_name(i);
		section(kind, named);
	}
	return out;
}

}  // namespace de
