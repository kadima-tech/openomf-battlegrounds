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
				break;
			}
			f.data.assign(c.data().begin() + long(start), c.data().begin() + long(c.pos()));
			v.finals.push_back(std::move(f));
		}
	}
	auto rest = c.bytes(c.remaining());
	v.tail.assign(rest.begin(), rest.end());
	return v;
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
