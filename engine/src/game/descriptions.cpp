#include "game/descriptions.h"

#include <algorithm>

#include "de/contents.h"
#include "game/runtime.h"
#include "game/session.h"

namespace game {

DescriptionInfo read_description(const de::World &data, const de::ObjectRef &d) {
	DescriptionInfo out;
	if (!d) return out;
	try {
		de::Contents c(d.file->object_data(d.info()).contents);
		de::Chunk ch = de::begin_chunk(c);
		c.string();
		if (c.u8()) c.skip(c.u8());  // the resource's passive reference: BOOL, size, {file, object} GUIDs
		if (ch.version >= 2 && c.pos() < ch.end) out.listed = c.boolean();
		if (ch.version >= 3 && c.pos() + 6 <= ch.end) {
			c.f32();
			out.locked_by_default = c.boolean();
			out.visible_when_locked = c.boolean();
			out.flag_d8 = c.boolean();
		}
	} catch (const std::exception &) {
	}
	try {
		de::Contents c(d.file->object_data(d.info()).references);
		c.seek(de::begin_chunk(c).end);  // DE_CObject's
		if (c.remaining() >= 2) {
			de::Chunk ch = de::begin_chunk(c);
			if (ch.version == 6) {
				while (c.pos() < ch.end && c.boolean()) {
					if (de::ObjectRef r = data.resolve(*d.file, de::Reference::read(c))) out.casts.push_back(r);
				}
				if (c.pos() < ch.end && c.boolean()) out.icon = data.resolve(*d.file, de::Reference::read(c));
				if (c.pos() < ch.end && c.boolean()) out.large = data.resolve(*d.file, de::Reference::read(c));
				// Resource components, then replacement objects, then the medium representation.
				for (int list = 0; list < 2; ++list) {
					while (c.pos() < ch.end && c.boolean()) de::Reference::read(c);
				}
				if (c.pos() < ch.end && c.boolean()) out.medium = data.resolve(*d.file, de::Reference::read(c));
			}
		}
	} catch (const std::exception &) {
	}
	return out;
}

std::pair<int, int> environment_players(const de::ObjectRef &d) {
	if (!d) return {0, 0};
	try {
		de::Contents c(d.file->object_data(d.info()).contents);
		c.seek(de::begin_chunk(c).end);
		de::Chunk ch = de::begin_chunk(c);
		if (ch.version == 1) {
			int lo = c.i32();
			return {lo, c.i32()};
		}
	} catch (const std::exception &) {
	}
	return {0, 0};
}

std::vector<de::ObjectRef> fighter_models(const de::World &data, const de::ObjectRef &fighter) {
	std::vector<de::ObjectRef> out;
	if (!fighter) return out;
	const de::Guid &g = fighter.info().guid;
	for (const auto &[stem, f] : data.files()) {
		for (uint32_t i = 0; i < f->objects().size(); ++i) {
			if (f->objects()[i].class_path != "Description\\FighterModelDescription") continue;
			try {
				de::Contents c(f->object_data(f->objects()[i]).contents);
				c.seek(de::begin_chunk(c).end);
				de::Chunk ch = de::begin_chunk(c);
				if (ch.version != 1) continue;
				auto b = c.bytes(16);
				if (std::equal(b.begin(), b.end(), g.bytes)) out.push_back({f.get(), i});
			} catch (const std::exception &) {
			}
		}
	}
	return out;
}

namespace {

// DE_CGUID::Get (Core 0x7001eee0): lower-case hex, 8-4-4-16.
std::string guid_key(const std::string &dashed) {
	std::string s;
	int dashes = 0;
	for (char ch : dashed) {
		if (ch == '-' && ++dashes == 4) continue;
		s += char(std::tolower(static_cast<unsigned char>(ch)));
	}
	return s;
}

// GetResourceRegistryPrefix (Core 0x70041160) + "\DLock": the description's list of unlocked ids,
// kept here as decimal ids separated by spaces in list order.
std::string lock_key(const de::ObjectRef &d) { return "ObjectInfo\\" + guid_key(d.info().guid.str()) + "\\DLock"; }

std::vector<uint32_t> unlocked_ids(const World &w, const de::ObjectRef &d) {
	std::vector<uint32_t> ids;
	std::string v = w.registry_string(lock_key(d));
	for (size_t p = 0; p < v.size();) {
		size_t q = v.find(' ', p);
		std::string t = v.substr(p, q == std::string::npos ? std::string::npos : q - p);
		if (!t.empty()) ids.push_back(uint32_t(std::stoul(t)));
		if (q == std::string::npos) break;
		p = q + 1;
	}
	return ids;
}

}  // namespace

bool description_locked(const World &w, const de::ObjectRef &d, uint32_t id) {
	if (id == 0 && !read_description(w.data(), d).locked_by_default) return false;
	auto ids = unlocked_ids(w, d);
	return std::find(ids.begin(), ids.end(), id) == ids.end();
}

// DE_CDescription::Lock(id, add) (Core 0x701f2460): the id leaves the list and, when added
// (Unlock), goes back at its end.
void lock_description(World &w, const de::ObjectRef &d, uint32_t id, bool unlock) {
	if (!d) return;
	auto ids = unlocked_ids(w, d);
	ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
	if (unlock) ids.push_back(id);
	std::string v;
	for (uint32_t i : ids) v += (v.empty() ? "" : " ") + std::to_string(i);
	w.registry[lock_key(d)] = v;
}

void unlock_description(World &w, const de::ObjectRef &d, uint32_t id) { lock_description(w, d, id, true); }

void migrate_lock_records(std::map<std::string, std::string> &reg) {
	std::map<std::string, std::string> add;
	for (auto it = reg.begin(); it != reg.end();) {
		if (it->first.rfind("DLock ", 0) != 0) {
			++it;
			continue;
		}
		std::string rest = it->first.substr(6);
		size_t sp = rest.find(' ');
		std::string key = "ObjectInfo\\" + guid_key(rest.substr(0, sp)) + "\\DLock";
		std::string id = sp == std::string::npos ? "0" : rest.substr(sp + 1);
		std::string &v = add.count(key) ? add[key] : (add[key] = reg.count(key) ? reg[key] : "");
		v += (v.empty() ? "" : " ") + id;
		it = reg.erase(it);
	}
	for (auto &[k, v] : add) reg[k] = v;
}

std::string read_string_value(const de::ObjectRef &v) {
	if (!v) return {};
	try {
		de::Contents c(v.file->object_data(v.info()).contents);
		c.seek(de::begin_chunk(c).end);
		de::begin_chunk(c);
		uint32_t n = c.u32();
		auto b = c.bytes(n);
		std::string s(b.begin(), b.end());
		while (!s.empty() && s.back() == 0) s.pop_back();
		return s;
	} catch (const std::exception &) {
		return {};
	}
}

ControlLayout read_control_creator(const de::World &data, const de::ObjectRef &cc) {
	ControlLayout out;
	if (!cc) return out;
	try {
		de::Contents c(cc.file->object_data(cc.info()).contents);
		de::Chunk ch = de::begin_chunk(c);
		c.string();
		if (ch.version == 1) {
			out.layout = c.i32();
			out.align = c.i32();
			out.text_height = c.i32();
			out.image_scale = c.f32();
		}
	} catch (const std::exception &) {
	}
	auto lists = reference_lists(data, cc);
	if (lists.size() < 2 || lists[1].empty()) return out;
	for (const de::ObjectRef &r : lists[1][0]) {
		if (!r) continue;
		const std::string &cls = r.info().class_path;
		ControlLayout item;
		if (cls == "Value\\String") {
			item.kind = ControlLayout::Text;
			item.text = read_string_value(r);
			item.text_height = out.text_height;
			item.align = out.align;
		} else if (cls.rfind("Image", 0) == 0) {
			item.kind = ControlLayout::Image;
			item.image = r;
			item.image_scale = out.image_scale;
		} else if (cls == "ControlCreator") {
			item = read_control_creator(data, r);
		} else {
			continue;
		}
		out.items.push_back(std::move(item));
	}
	return out;
}

de::ObjectRef pilot_photo(const de::World &data, const de::ObjectRef &d) {
	auto res = passive_references(data, d);
	if (res.empty()) return {};
	const de::ObjectRef &p = res[0];
	auto parents = p.file->parents();
	for (uint32_t i = 0; i < parents.size(); ++i) {
		if (parents[i] == int32_t(p.index) && p.file->objects()[i].class_path.rfind("Image", 0) == 0) return {p.file, i};
	}
	auto lists = reference_lists(data, p);
	if (lists.size() > 1 && !lists[1].empty()) {
		for (auto &r : lists[1][0]) {
			if (r && r.info().class_path.rfind("Image", 0) == 0) return r;
		}
	}
	return {};
}

}  // namespace game
