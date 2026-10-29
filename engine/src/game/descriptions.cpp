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
				if (c.pos() < ch.end && c.boolean()) de::Reference::read(c);  // icon
				if (c.pos() < ch.end && c.boolean()) out.large = data.resolve(*d.file, de::Reference::read(c));
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

bool description_locked(const World &w, const de::ObjectRef &d) {
	return read_description(w.data(), d).locked_by_default && w.registry_string("DLock " + d.info().guid.str()).empty();
}

void unlock_description(World &w, const de::ObjectRef &d) {
	if (d) w.registry["DLock " + d.info().guid.str()] = "1";
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
	int layout = 0;
	try {
		de::Contents c(cc.file->object_data(cc.info()).contents);
		de::Chunk ch = de::begin_chunk(c);
		c.string();
		if (ch.version == 1) {
			layout = c.i32();
			out.wrap = c.i32() != 0;
			out.text_height = c.i32();
		}
	} catch (const std::exception &) {
	}
	out.across = layout == 1 || layout == 3;
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
			item.wrap = out.wrap;
		} else if (cls.rfind("Image", 0) == 0) {
			item.kind = ControlLayout::Image;
			item.image = r;
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
