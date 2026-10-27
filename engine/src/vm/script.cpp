#include "vm/script.h"

#include <algorithm>

#include "de/objects_file.h"

namespace vm {

namespace {

std::string text(de::Contents &c) {
	auto b = c.block();
	size_t n = b.size();
	while (n && b[n - 1] == 0) {
		--n;
	}
	return std::string(b.begin(), b.begin() + n);
}

uint32_t g_next_method_handle = 1;

}  // namespace

ScriptMethod *ScriptClass::find(const std::string &signature) const {
	for (size_t i = 0; i < pool.size(); ++i) {
		if (pool[i] == signature) {
			return method(uint32_t(i));
		}
	}
	return nullptr;
}

ScriptMethod *ScriptClass::resolve_virtual(uint32_t slot, const ScriptMethod *running) const {
	// A state carries its own virtual table (its base methods, one per slot), which already
	// accounts for `inherit` and the enclosing states.
	for (const ScriptMethod *s = running; s; s = s->parent) {
		if (s->is_state() && slot < s->cm->base_methods.size()) {
			if (ScriptMethod *m = method(s->cm->base_methods[slot])) return m;
		}
	}
	for (const ScriptMethod *s = running; s; s = s->parent) {
		for (ScriptMethod *n : s->nested) {
			if (n->vslot == int32_t(slot)) {
				return n;
			}
		}
	}
	return slot < vtable.size() ? vtable[slot] : nullptr;
}

ScriptMethod *ScriptClass::find_by_name(const std::string &name) const {
	ScriptMethod *found = nullptr;
	for (auto &m : methods) {
		auto &sig = m.cm->signature;
		auto colon = sig.find(':');
		if (colon != std::string::npos && sig.compare(colon + 1, std::string::npos, name) == 0 && !m.parent) {
			found = const_cast<ScriptMethod *>(&m);  // later behaviors are more derived
		}
	}
	if (found && found->vslot >= 0 && size_t(found->vslot) < vtable.size() && vtable[size_t(found->vslot)]) {
		return vtable[size_t(found->vslot)];
	}
	return found;
}

void load_class(ScriptClass &out, Memory &mem, const std::vector<uint8_t> &contents, VMClass code) {
	de::Contents c(contents);
	de::Chunk ch = de::begin_chunk(c);
	if (ch.version >= 2) {
		out.name = text(c);
	}
	if (ch.version >= 3) {
		out.base_path = text(c);
		if (c.u8() == 1) {  // DE_CStringPool::ProcessContents
			uint32_t n = c.squeezed();
			for (uint32_t i = 0; i < n; ++i) {
				out.pool.push_back(text(c));
			}
		}
	}
	out.code = std::move(code);

	std::vector<std::pair<ScriptMethod *, uint32_t>> parent_links;
	for (auto &b : out.code.behaviors) {
		auto &table = out.behavior_methods.emplace_back(b.methods.size(), nullptr);
		for (size_t mi = 0; mi < b.methods.size(); ++mi) {
			auto &m = b.methods[mi];
			if (!m) {
				continue;
			}
			ScriptMethod &sm = out.methods.emplace_back();
			table[mi] = &sm;
			if (mi < b.base_links.size() && b.base_links[mi]) {
				parent_links.emplace_back(&sm, *b.base_links[mi]);
			}
			if (m->v34) {
				sm.vslot = int32_t(*m->v34);
				if (out.vtable.size() <= *m->v34) {
					out.vtable.resize(*m->v34 + 1, nullptr);
				}
				out.vtable[*m->v34] = &sm;
			}
			sm.cm = &*m;
			sm.cls = &out;
			sm.code_addr = mem.alloc_copy(m->code.data(), uint32_t(m->code.size()));
			sm.code_size = uint32_t(m->code.size());
			sm.handle = g_next_method_handle++;
			auto it = std::find(out.pool.begin(), out.pool.end(), m->signature);
			if (it == out.pool.end()) {
				out.pool.push_back(m->signature);
				it = out.pool.end() - 1;
			}
			sm.index = uint32_t(it - out.pool.begin());
			if (out.by_index.size() <= sm.index) {
				out.by_index.resize(sm.index + 1, nullptr);
			}
			out.by_index[sm.index] = &sm;
		}
	}
	// Parent links name class method indices, so resolve them once every method has one.
	for (auto &[m, idx] : parent_links) {
		m->parent = out.method(idx);
		if (m->parent && m->parent != m) {
			m->parent->nested.push_back(m);
		} else {
			m->parent = nullptr;
		}
	}
	// Methods nested in a state are state-scoped overrides, not class-level ones.
	for (auto &m : out.methods) {
		if (m.parent && m.vslot >= 0 && size_t(m.vslot) < out.vtable.size() && out.vtable[size_t(m.vslot)] == &m) {
			out.vtable[size_t(m.vslot)] = nullptr;
		}
	}
	for (auto &m : out.methods) {
		if (!m.parent && m.vslot >= 0) {
			out.vtable[size_t(m.vslot)] = &m;  // re-apply top-level methods in behavior order
		}
	}
}

}  // namespace vm
