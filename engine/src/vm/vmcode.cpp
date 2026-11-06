#include "vm/vmcode.h"

namespace vm {

using de::Contents;

namespace {

std::string text(Contents &c) {
	auto b = c.block();
	size_t n = b.size();
	while (n && b[n - 1] == 0) {
		--n;
	}
	return std::string(b.begin(), b.begin() + n);
}

ClassInstance read_instance(Contents &c) {
	ClassInstance i;
	i.class_index = c.squeezed();
	i.value = c.u32();
	i.flag = c.boolean();
	i.count = c.squeezed();
	if (i.count && c.boolean()) {
		i.class2 = c.squeezed();
	}
	return i;
}

std::vector<CompiledMethod::Replicated> read_replicated(Contents &c) {
	std::vector<CompiledMethod::Replicated> out(c.squeezed());
	for (auto &r : out) {
		r.type = c.squeezed();
		r.a = c.squeezed();
		r.b = c.squeezed();
	}
	return out;
}

std::optional<CompiledMethod> read_method(Contents &c) {
	size_t end = de::begin_sized(c);
	if (!c.boolean()) {
		c.seek(end);
		return std::nullopt;
	}
	CompiledMethod m;
	m.name = text(c);
	m.code = c.block();
	for (bool &f : m.flags) {
		f = c.boolean();
	}
	// With no source PCodeMethod attached (always true when loading VMCode) the original
	// gates these on its own flags +0x8c and +0x90.
	if (m.flags[2]) {
		m.v34 = c.squeezed();
	}
	if (m.flags[3]) {
		m.base_methods.resize(c.squeezed());
		for (auto &b : m.base_methods) {
			b = c.squeezed();
		}
	}
	m.instances.resize(c.squeezed());
	for (auto &i : m.instances) {
		i = read_instance(c);
	}
	m.interfaces.resize(c.u32());
	for (auto &i : m.interfaces) {
		i.a = c.u32();
		i.b = c.i32();
		i.c = c.boolean();
	}
	for (auto &n : m.counts) {
		n = c.squeezed();
	}
	m.signature = text(c);
	m.v78 = c.i32();
	m.replicated = read_replicated(c);
	c.seek(end);
	return m;
}

BehaviorCode read_behavior(Contents &c) {
	size_t end = de::begin_sized(c);
	BehaviorCode b;
	b.instances.resize(c.squeezed());
	for (auto &i : b.instances) {
		i = read_instance(c);
	}
	b.methods.resize(c.squeezed());
	for (auto &m : b.methods) {
		m = read_method(c);
	}
	for (size_t i = 0; i < b.methods.size(); ++i) {
		b.base_links.push_back(c.boolean() ? std::optional<uint32_t>(c.squeezed()) : std::nullopt);
	}
	b.replicated = read_replicated(c);
	c.seek(end);
	return b;
}

}  // namespace

VMClass VMClass::parse(const std::vector<uint8_t> &vmc2, size_t behavior_count) {
	Contents c(vmc2);
	de::Chunk ch = de::begin_chunk(c);
	if (ch.version != 13) {
		throw de::FormatError("VMC2: unsupported version " + std::to_string(ch.version));
	}
	VMClass v;
	v.change_info = c.block();
	v.classes.resize(c.i32());
	for (auto &k : v.classes) {
		k.name = text(c);
		k.layout_pos = c.squeezed();
	}
	v.intrinsics.resize(c.i32());
	for (auto &i : v.intrinsics) {
		i.class_index = c.squeezed();
		i.signature = text(c);
	}
	v.ref_members.resize(c.i32());
	for (auto &r : v.ref_members) {
		r = c.squeezed();
	}
	for (size_t i = 0; i < behavior_count; ++i) {
		v.behaviors.push_back(read_behavior(c));
	}
	v.global_refs.resize(c.i32());
	for (auto &g : v.global_refs) {
		g.kind = c.u32();
		g.name = text(c);
	}
	// DE_CClass::LoadVMCode (Core 0x70134...): member behavior interfaces, then the final
	// class instances (each a squeezed class index and that native class's own contents).
	if (c.remaining() >= 4) {
		v.member_interfaces.resize(c.u32());
		for (auto &m : v.member_interfaces) {
			m.first = c.u32();
			m.second = c.i32();
		}
	}
	// Each native value class deserialises its own value (DE_CPCNodeClass vtable +0xe0); the
	// layouts below are the ones the shipped classes use. An unknown class stops the walk.
	if (c.remaining() >= 4) {
		uint32_t n = c.u32();
		for (uint32_t i = 0; i < n && c.remaining() > 0; ++i) {
			size_t class_pos = c.pos();
			FinalInstance f;
			f.class_index = c.squeezed();
			const std::string name = f.class_index < v.classes.size() ? v.classes[f.class_index].name : "";
			size_t start = c.pos();
			if (name == "CObject::CColorRGBA") {
				c.bytes(16);
			} else if (name == "CObject::CVector") {
				c.bytes(12);
			} else if (name == "CObject::CCoordinateSystem") {
				c.bytes(36);
			} else if (name == "CObject::CGUID") {
				c.bytes(16);
			} else if (name == "CGenericEvent::CNamedHandle") {
				c.bytes(c.u32());
			} else if (name == "CObject::CText") {
				c.bytes(c.squeezed());
			} else {
				v.unknown_final_class = name.empty() ? "#" + std::to_string(f.class_index) : name;
				v.finals_count = n;
				c.seek(class_pos);
				break;
			}
			f.data.assign(c.data().begin() + long(start), c.data().begin() + long(c.pos()));
			v.finals.push_back(std::move(f));
		}
	}
	if (!v.finals_count && c.remaining() > 0) {
		v.member_size = c.squeezed();
	}
	auto rest = c.bytes(c.remaining());
	v.tail.assign(rest.begin(), rest.end());
	return v;
}

namespace {

using de::ContentsWriter;

void put_text(ContentsWriter &w, const std::string &s) {
	w.u32(uint32_t(s.size() + 1));
	w.bytes(s.data(), s.size());
	w.u8(0);
}

void put_instance(ContentsWriter &w, const ClassInstance &i) {
	w.squeezed(i.class_index);
	w.u32(i.value);
	w.boolean(i.flag);
	w.squeezed(i.count);
	if (i.count) {
		w.boolean(i.class2.has_value());
		if (i.class2) w.squeezed(*i.class2);
	}
}

void put_replicated(ContentsWriter &w, const std::vector<CompiledMethod::Replicated> &rs) {
	w.squeezed(uint32_t(rs.size()));
	for (auto &r : rs) {
		w.squeezed(r.type);
		w.squeezed(r.a);
		w.squeezed(r.b);
	}
}

// DE_CClassCompiledMethod::SaveVMCode (0x7013d920).
void put_method(ContentsWriter &w, const std::optional<CompiledMethod> &om) {
	size_t at = w.begin_sized();
	w.boolean(om.has_value());
	if (om) {
		const CompiledMethod &m = *om;
		put_text(w, m.name);
		w.block(m.code);
		for (bool f : m.flags) w.boolean(f);
		if (m.flags[2]) w.squeezed(m.v34.value_or(0));
		if (m.flags[3]) {
			w.squeezed(uint32_t(m.base_methods.size()));
			for (uint32_t b : m.base_methods) w.squeezed(b);
		}
		w.squeezed(uint32_t(m.instances.size()));
		for (auto &i : m.instances) put_instance(w, i);
		w.u32(uint32_t(m.interfaces.size()));
		for (auto &i : m.interfaces) {
			w.u32(i.a);
			w.i32(i.b);
			w.boolean(i.c);
		}
		for (uint32_t n : m.counts) w.squeezed(n);
		put_text(w, m.signature);
		w.i32(m.v78);
		put_replicated(w, m.replicated);
	}
	w.end_chunk(at);
}

// DE_CClassBehaviorReference::SaveVMCode (0x70136e60).
void put_behavior(ContentsWriter &w, const BehaviorCode &b) {
	size_t at = w.begin_sized();
	w.squeezed(uint32_t(b.instances.size()));
	for (auto &i : b.instances) put_instance(w, i);
	w.squeezed(uint32_t(b.methods.size()));
	for (auto &m : b.methods) put_method(w, m);
	for (size_t i = 0; i < b.methods.size(); ++i) {
		auto link = i < b.base_links.size() ? b.base_links[i] : std::nullopt;
		w.boolean(link.has_value());
		if (link) w.squeezed(*link);
	}
	put_replicated(w, b.replicated);
	w.end_chunk(at);
}

}  // namespace

std::vector<uint8_t> VMClass::write() const {
	ContentsWriter w;
	size_t at = w.begin_chunk(13);
	w.block(change_info);
	w.i32(int32_t(classes.size()));
	for (auto &k : classes) {
		put_text(w, k.name);
		w.squeezed(k.layout_pos);
	}
	w.i32(int32_t(intrinsics.size()));
	for (auto &i : intrinsics) {
		w.squeezed(i.class_index);
		put_text(w, i.signature);
	}
	w.i32(int32_t(ref_members.size()));
	for (uint32_t r : ref_members) w.squeezed(r);
	for (auto &b : behaviors) put_behavior(w, b);
	w.i32(int32_t(global_refs.size()));
	for (auto &g : global_refs) {
		w.u32(g.kind);
		put_text(w, g.name);
	}
	w.u32(uint32_t(member_interfaces.size()));
	for (auto &[id, block] : member_interfaces) {
		w.u32(id);
		w.i32(block);
	}
	w.u32(finals_count.value_or(uint32_t(finals.size())));
	for (auto &f : finals) {
		w.squeezed(f.class_index);
		w.bytes(f.data);
	}
	if (member_size) w.squeezed(*member_size);
	w.bytes(tail);
	w.end_chunk(at);
	return w.take();
}

std::vector<de::Reference> class_behavior_refs(const std::vector<uint8_t> &references) {
	Contents c(references);
	de::Chunk obj = de::begin_chunk(c);  // DE_CObject::ObjProcessReferences (0x7003bfe0)
	c.seek(obj.end);
	de::Chunk cls = de::begin_chunk(c);
	std::vector<de::Reference> out;
	if (cls.version == 2) {
		while (c.pos() < cls.end && c.boolean()) {
			out.push_back(de::Reference::read(c));
		}
	}
	return out;
}

}  // namespace vm
