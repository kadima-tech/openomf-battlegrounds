#include "help.h"

#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

#include "content.h"
#include "har.h"
#include "image_io.h"
#include "model_build.h"
#include "pilot.h"

namespace ed {

namespace {

// COREMeshes' Description\Cast "HelpEntry" (Core.ModuleDLL), the cast DE_CHelpUIManager lists.
const Guid &help_cast() {
	static const Guid g = Guid::parse("70e04b27-b3a7-4149-b9c7-5eeb0325d973");
	return g;
}

std::string utf8_name(const deo::Object &o) {
	const std::string n = o.info.name_text();
	return deo::latin1_to_utf8(Bytes(n.begin(), n.end()));
}

std::vector<std::string> split_lines(const std::string &text) {
	std::vector<std::string> out;
	size_t p = 0;
	for (;;) {
		size_t q = text.find('\n', p);
		std::string line = text.substr(p, q == std::string::npos ? std::string::npos : q - p);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		out.push_back(line);
		if (q == std::string::npos) return out;
		p = q + 1;
	}
}

std::string join_lines(const std::vector<std::string> &lines, const char *sep) {
	std::string out;
	for (size_t i = 0; i < lines.size(); ++i) out += (i ? sep : "") + lines[i];
	return out;
}

std::string parent_path(const std::string &path) {
	size_t slash = path.rfind('/');
	return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string leaf_name(const std::string &path) { return path.substr(path.rfind('/') + 1); }

std::string trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

struct DescRefs {
	std::vector<Ref> casts;
	Ref large;
	bool ok = false;
};

DescRefs desc_refs(const deo::Object &d) {
	DescRefs out;
	Chunks c = split_chunks(d.references);
	if (c.chunks.size() < 2 || c.chunks[1].version != 6) return out;
	try {
		deo::Reader r(c.chunks[1].body);
		while (r.u8()) out.casts.push_back(read_ref(r));
		if (r.u8()) read_ref(r);
		if (r.u8()) out.large = read_ref(r);
		out.ok = true;
	} catch (const deo::Error &) {
	}
	return out;
}

bool is_help(const deo::File &f, const deo::Object &o) {
	if (o.info.class_text().rfind("Description", 0) != 0) return false;
	for (const Ref &c : desc_refs(o).casts) {
		if (ref_guid(f, c) == help_cast()) return true;
	}
	return false;
}

// Each help topic's path through the help topics above it.
std::map<uint32_t, std::string> topic_paths(const deo::File &f) {
	std::map<uint32_t, std::string> out;
	std::vector<int> parents = object_parents(f);
	std::vector<bool> help(f.objects.size());
	for (size_t i = 0; i < f.objects.size(); ++i) help[i] = is_help(f, f.objects[i]);
	for (size_t i = 0; i < f.objects.size(); ++i) {
		if (!help[i]) continue;
		std::string path = utf8_name(f.objects[i]);
		for (int up = parents[i]; up >= 0 && help[size_t(up)]; up = parents[size_t(up)]) path = utf8_name(f.objects[size_t(up)]) + "/" + path;
		out[uint32_t(i)] = path;
	}
	return out;
}

struct BaseHelp {
	size_t file = SIZE_MAX;
	std::map<std::string, Guid> pages;  // each topic's large representation
	// A leaf topic whose page is one text block in a creator of its own: what new topics copy.
	uint32_t topic = 0, creator = 0, text = 0;
	bool templated = false;
};

BaseHelp base_help(const BaseData &b) {
	BaseHelp out;
	const BaseData::FileIndex *fi = b.file("OMFHelp");
	if (!fi) return out;
	out.file = size_t(fi - b.files().data());
	const deo::File &f = b.model(out.file);
	for (auto &[i, path] : topic_paths(f)) {
		const deo::Object &d = f.objects[i];
		DescRefs dr = desc_refs(d);
		out.pages[path] = ref_guid(f, dr.large);
		if (out.templated || !hierarchy_children(d).empty() || dr.large.kind != Ref::Local) continue;
		const deo::Object &cc = f.objects[dr.large.index];
		if (cc.info.class_text() != "ControlCreator") continue;
		auto items = object_references("ControlCreator", cc.references);
		if (items.size() != 1 || items[0].kind != Ref::Local || f.objects[items[0].index].info.class_text() != "Value\\String") continue;
		out.topic = i, out.creator = dr.large.index, out.text = items[0].index, out.templated = true;
	}
	return out;
}

Ref pool_ref(Project &p, const Guid &g) {
	Ref r;
	r.kind = Ref::Pool, r.index = p.pool_index(g);
	return r;
}

void set_large(Project &p, uint32_t d, const Ref &large) {
	deo::Object &o = p.file().objects[d];
	Chunks rc = split_chunks(o.references);
	if (rc.chunks.size() < 2) throw deo::Error("the template description has no references chunk");
	RawChunk out;
	if (!map_reference_chunk("Description", 1, rc.chunks[1], out, [&](const Ref &r) { return p.target(r).guid == help_cast() ? r : large; })) {
		throw deo::Error("cannot set a help topic's page");
	}
	rc.chunks[1] = std::move(out);
	o.references = join_chunks(rc);
}

// ControlCreator contents: [name][i32 layout][i32 alignment][i32 text height][f32 image scale].
void set_creator(Project &p, uint32_t cc, int align, int height, float scale, const std::vector<Ref> &items) {
	deo::Object &o = p.file().objects[cc];
	Chunks c = split_chunks(o.contents);
	deo::Reader r(c.chunks.at(0).body);
	r.block();
	size_t at = r.pos();
	deo::Writer w;
	w.bytes(Bytes(c.chunks[0].body.begin(), c.chunks[0].body.begin() + long(at)));
	w.u32(2);
	w.u32(uint32_t(align));
	w.u32(uint32_t(height));
	build::put_f32(w, scale);
	w.bytes(Bytes(c.chunks[0].body.begin() + long(at + 16), c.chunks[0].body.end()));
	c.chunks[0].body = w.data();
	o.contents = join_chunks(c);
	Chunks rc = split_chunks(o.references);
	deo::Writer list;
	for (const Ref &item : items) {
		list.u8(1);
		write_ref(list, item);
	}
	list.u8(0);
	rc.chunks.at(1).version = 1;
	rc.chunks[1].body = list.data();
	o.references = join_chunks(rc);
}

int creator_align(const deo::Object &cc) {
	try {
		Chunks c = split_chunks(cc.contents);
		deo::Reader r(c.chunks.at(0).body);
		r.block();
		r.u32();
		return int(r.u32());
	} catch (const std::exception &) {
		return 2;
	}
}

// As the shipped pages: CR LF lines, a space for a blank one, and a blank first line under a title or picture.
std::string stored_text(const HelpTopic &t) {
	std::vector<std::string> lines = split_lines(t.text);
	if (!t.title.empty() || !t.image.empty()) lines.insert(lines.begin(), "");
	for (auto &l : lines) {
		if (l.empty()) l = " ";
	}
	return join_lines(lines, "\r\n");
}

std::string read_text(const std::string &stored, bool spaced) {
	std::vector<std::string> lines = split_lines(stored);
	for (auto &l : lines) {
		if (l == " ") l.clear();
	}
	if (spaced && lines.size() > 1 && lines[0].empty()) lines.erase(lines.begin());
	return join_lines(lines, "\n");
}

struct AddonTopic {
	std::string path;
	uint32_t desc;
};

std::vector<AddonTopic> addon_topics(const Project &p) {
	std::vector<AddonTopic> out;
	for (auto &[i, path] : topic_paths(p.file())) out.push_back({path, i});
	return out;
}

// The page's own creator and its parts, when the add-on holds them.
struct OwnPage {
	int creator = -1, text = -1, title_creator = -1, title = -1;
	Ref image;
};

OwnPage own_page(const Project &p, uint32_t desc) {
	OwnPage out;
	DescRefs dr = desc_refs(p.file().objects[desc]);
	Project::Target t = p.target(dr.large);
	if (t.local < 0 || p.file().objects[size_t(t.local)].info.class_text() != "ControlCreator") return out;
	out.creator = t.local;
	for (const Ref &r : object_references("ControlCreator", p.file().objects[size_t(t.local)].references)) {
		Project::Target it = p.target(r);
		std::string cls = it.local >= 0 ? p.file().objects[size_t(it.local)].info.class_text() : "";
		if (it.local < 0 && it.resolved) {
			if (const BaseObject *b = p.base().by_guid(it.guid)) cls = b->class_path;
		}
		if (cls == "Value\\String" && it.local >= 0) out.text = it.local;
		else if (cls == "ControlCreator" && it.local >= 0 && out.title_creator < 0) {
			out.title_creator = it.local;
			for (const Ref &tr : object_references("ControlCreator", p.file().objects[size_t(it.local)].references)) {
				Project::Target tt = p.target(tr);
				if (tt.local >= 0 && p.file().objects[size_t(tt.local)].info.class_text() == "Value\\String") out.title = tt.local;
			}
		} else if (cls.rfind("Image", 0) == 0) {
			out.image = r;
		}
	}
	return out;
}

}  // namespace

std::vector<HelpTopic> parse_help_file(const std::string &text) {
	std::vector<HelpTopic> out;
	bool body = false;
	int n = 0;
	auto fail = [&](const std::string &what) { throw deo::Error("help file line " + std::to_string(n) + ": " + what); };
	for (const std::string &line : split_lines(text)) {
		++n;
		if (!line.empty() && line[0] == '#') continue;
		if (line.rfind("==", 0) == 0) {
			HelpTopic t;
			t.path = trim(line.substr(2));
			if (t.path.empty() || t.path.front() == '/' || t.path.back() == '/' || t.path.find("//") != std::string::npos) fail("a topic needs a path of names separated by '/'");
			out.push_back(t);
			body = false;
			continue;
		}
		if (out.empty()) {
			if (!trim(line).empty()) fail("text before the first \"== path\"");
			continue;
		}
		HelpTopic &t = out.back();
		if (!body) {
			if (trim(line).empty()) continue;
			auto key = [&](const char *k, std::string *v) {
				if (line.rfind(k, 0) != 0) return false;
				*v = trim(line.substr(strlen(k)));
				return true;
			};
			std::string align;
			if (key("title:", &t.title) || key("image:", &t.image) || key("page:", &t.same_as)) continue;
			if (key("align:", &align)) {
				if (align != "left" && align != "centre" && align != "center") fail("align is left or centre");
				t.left = align == "left";
				continue;
			}
			body = true;
		}
		t.text += (t.text.empty() ? "" : "\n") + line;
	}
	for (HelpTopic &t : out) {
		std::vector<std::string> lines = split_lines(t.text);
		while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
		t.text = join_lines(lines, "\n");
		if (!t.same_as.empty() && (!t.text.empty() || !t.title.empty() || !t.image.empty())) throw deo::Error("help topic " + t.path + ": \"page:\" shows another page, so it takes no text, title or image");
	}
	return out;
}

std::string format_help_file(const std::vector<HelpTopic> &topics) {
	std::string out;
	for (const HelpTopic &t : topics) {
		if (!out.empty()) out += "\n";
		out += "== " + t.path + "\n";
		if (!t.same_as.empty()) out += "page: " + t.same_as + "\n";
		if (!t.title.empty()) out += "title: " + t.title + "\n";
		if (!t.image.empty()) out += "image: " + t.image + "\n";
		if (t.left) out += "align: left\n";
		if (!t.text.empty()) out += t.text + "\n";
	}
	return out;
}

std::vector<std::string> base_help_paths(const BaseData &base) {
	std::vector<std::string> out;
	for (auto &[path, page] : base_help(base).pages) out.push_back(path);
	return out;
}

std::vector<HelpTopic> read_help(const Project &p, std::vector<int> *pages) {
	std::vector<HelpTopic> out;
	BaseHelp bh = base_help(p.base());
	if (pages) pages->clear();
	for (const AddonTopic &a : addon_topics(p)) {
		HelpTopic t;
		t.path = a.path;
		OwnPage own = own_page(p, a.desc);
		if (own.creator < 0) {
			Guid large = p.target(desc_refs(p.file().objects[a.desc]).large).guid;
			for (auto &[path, page] : bh.pages) {
				if (page == large && t.same_as.empty()) t.same_as = path;
			}
		} else {
			t.left = creator_align(p.file().objects[size_t(own.creator)]) == 0;
			if (own.title >= 0) t.title = string_text(p.file().objects[size_t(own.title)]);
			// write_help names the images it imports after the topic; another of the add-on's is the HAR's picture.
			if (own.image.kind == Ref::Local) {
				std::string name = utf8_name(p.file().objects[own.image.index]);
				t.image = name == leaf_name(t.path) + " Image" ? name : "picture";
			}
			else if (own.image.kind == Ref::Pool) {
				const BaseObject *b = p.base().by_guid(p.target(own.image).guid);
				t.image = b ? "base:" + b->name : "base:?";
			}
			if (own.text >= 0) t.text = read_text(string_text(p.file().objects[size_t(own.text)]), !t.title.empty() || !t.image.empty());
		}
		if (pages) pages->push_back(own.text);
		out.push_back(std::move(t));
	}
	return out;
}

void write_help(Project &p, const std::vector<HelpTopic> &topics, int picture) {
	BaseHelp bh = base_help(p.base());
	if (bh.file == SIZE_MAX || !bh.templated) throw deo::Error("the base data has no OMFHelp help topics to copy");
	std::map<std::string, uint32_t> made;
	for (const AddonTopic &a : addon_topics(p)) made[a.path] = a.desc;
	// Every mistake is found before anything is written.
	std::map<std::string, Guid> base_images;
	std::map<std::string, RgbaImage> files;
	{
		std::set<std::string> known;
		for (auto &[path, d] : made) known.insert(path);
		const deo::File &f = p.base().model(bh.file);
		for (const HelpTopic &t : topics) {
			auto fail = [&](const std::string &what) { throw deo::Error("help topic " + t.path + ": " + what); };
			if (bh.pages.count(t.path)) fail("it is a base topic; add topics under it");
			std::string up = parent_path(t.path);
			while (!up.empty() && !known.count(up) && bh.pages.count(up)) up = parent_path(up);
			if (!up.empty() && !known.count(up)) fail("no topic " + up + " above it (list it first, or name a base topic)");
			if (!t.same_as.empty() && !bh.pages.count(t.same_as)) fail("no base topic " + t.same_as);
			if (t.image == "picture" && picture < 0) fail("\"image: picture\" needs a HAR with a picture of its own");
			if (t.image.rfind("base:", 0) == 0) {
				for (const deo::Object &o : f.objects) {
					if (o.info.class_text().rfind("Image", 0) == 0 && utf8_name(o) == t.image.substr(5)) base_images[t.image] = o.info.guid;
				}
				if (!base_images.count(t.image)) fail("OMFHelp has no image " + t.image.substr(5));
			} else if (!t.image.empty() && t.image != "picture" && t.image != leaf_name(t.path) + " Image") {
				files[t.image] = load_image(t.image);
			}
			known.insert(t.path);
		}
	}
	auto new_topic = [&](const std::string &path, int parent) {
		uint32_t d = p.copy_from_base(bh.file, {bh.topic})[0];
		build::rename(p, d, leaf_name(path));
		if (parent >= 0) build::add_child(p, uint32_t(parent), d);
		made[path] = d;
		return d;
	};
	std::function<int(const std::string &, const std::string &)> topic_at = [&](const std::string &path, const std::string &child) -> int {
		if (path.empty()) return -1;
		if (auto it = made.find(path); it != made.end()) return int(it->second);
		auto base = bh.pages.find(path);
		if (base == bh.pages.end()) throw deo::Error("help topic " + child + ": no topic " + path + " above it");
		int parent = topic_at(parent_path(path), path);
		uint32_t anchor = new_topic(path, parent);
		set_large(p, anchor, pool_ref(p, base->second));
		return int(anchor);
	};
	for (const HelpTopic &t : topics) {
		int parent = topic_at(parent_path(t.path), t.path);
		auto it = made.find(t.path);
		uint32_t d = it != made.end() ? it->second : new_topic(t.path, parent);
		if (!t.same_as.empty()) {
			set_large(p, d, pool_ref(p, bh.pages.at(t.same_as)));
			continue;
		}
		OwnPage own = own_page(p, d);
		if (own.creator < 0 || own.text < 0) {
			auto pair = p.copy_from_base(bh.file, {bh.creator, bh.text});
			own.creator = int(pair[0]), own.text = int(pair[1]);
			build::rename(p, pair[0], leaf_name(t.path) + " Page");
			build::rename(p, pair[1], leaf_name(t.path) + " Text");
			set_large(p, d, build::local_ref(pair[0]));
		}
		std::vector<Ref> items;
		if (!t.title.empty()) {
			if (own.title_creator < 0 || own.title < 0) {
				auto pair = p.copy_from_base(bh.file, {bh.creator, bh.text});
				own.title_creator = int(pair[0]), own.title = int(pair[1]);
				build::rename(p, pair[0], leaf_name(t.path) + " Title");
				build::rename(p, pair[1], leaf_name(t.path) + " Title Text");
			}
			set_string_text(p.file().objects[size_t(own.title)], t.title);
			set_creator(p, uint32_t(own.title_creator), 2, 18, 1, {build::local_ref(uint32_t(own.title))});
			items.push_back(build::local_ref(uint32_t(own.title_creator)));
		}
		if (t.image == "picture") {
			items.push_back(build::local_ref(uint32_t(picture)));
		} else if (t.image.rfind("base:", 0) == 0) {
			items.push_back(pool_ref(p, base_images.at(t.image)));
		} else if (!t.image.empty()) {
			const std::string name = leaf_name(t.path) + " Image";
			bool mine = own.image.kind == Ref::Local && utf8_name(p.file().objects[own.image.index]) == name;
			auto file = files.find(t.image);
			if (mine && file != files.end()) {
				set_image_pixels(p, own.image.index, file->second.width, file->second.height, file->second.rgba, std::filesystem::path(t.image).filename().string());
			}
			if (mine) items.push_back(own.image);
			else if (file != files.end()) items.push_back(build::local_ref(build::add_image(p, name, file->second.width, file->second.height, file->second.rgba)));
		}
		set_string_text(p.file().objects[size_t(own.text)], stored_text(t));
		items.push_back(build::local_ref(uint32_t(own.text)));
		set_creator(p, uint32_t(own.creator), t.left ? 0 : 2, 13, 0.75f, items);
	}
	p.touch();
}

}  // namespace ed
