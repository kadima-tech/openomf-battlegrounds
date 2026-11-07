// The Content Browser (the add-on and the whole base game, by type or by file), selection,
// opening objects, thumbnails and sound playback.
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

#include "editor.h"
#include "icons.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "pilot.h"
#include "preview.h"
#include "theme.h"
#include "widgets.h"

namespace fs = std::filesystem;
using namespace theme;

namespace {

std::string lower(std::string s) {
	for (char &c : s) c = char(tolower(uint8_t(c)));
	return s;
}

bool name_less(const std::string &a, const std::string &b) {
	return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
	                                    [](char x, char y) { return tolower(uint8_t(x)) < tolower(uint8_t(y)); });
}

std::string short_class(const std::string &cls) {
	size_t a = cls.rfind('\\');
	if (a == std::string::npos) return cls;
	size_t b = cls.rfind('\\', a - 1);
	return b == std::string::npos || a == 0 ? cls : cls.substr(b + 1);
}

const char *category_icon(int c) {
	switch (ed::Category(c)) {
		case ed::Category::Pilots: return ICON_USER;
		case ed::Category::Robots: return ICON_BOT;
		case ed::Category::Arenas: return ICON_MOUNTAIN;
		case ed::Category::Tournaments: return ICON_TROPHY;
		case ed::Category::Images: return ICON_IMAGE;
		case ed::Category::Meshes: return ICON_CUBOID;
		case ed::Category::Sounds: return ICON_VOLUME_2;
		case ed::Category::Music: return ICON_MUSIC;
		case ed::Category::Scripts: return ICON_CODE;
		case ed::Category::Descriptions: return ICON_FILE_TEXT;
		case ed::Category::Values: return ICON_HASH;
		case ed::Category::Particles: return ICON_SPARKLES;
		case ed::Category::Materials: return ICON_PAINTBRUSH;
		case ed::Category::Animation: return ICON_SPLINE;
		case ed::Category::Scene: return ICON_BOXES;
		default: return ICON_BOX;
	}
}

// Box-filtered to fit `max` pixels, keeping the aspect.
std::vector<uint8_t> shrink(const std::vector<uint8_t> &rgba, int w, int h, int max, int *ow, int *oh) {
	int tw = w, th = h;
	if (w > max || h > max) {
		if (w >= h) tw = max, th = std::max(1, h * max / w);
		else th = max, tw = std::max(1, w * max / h);
	}
	*ow = tw, *oh = th;
	if (tw == w && th == h) return rgba;
	std::vector<uint8_t> out(size_t(tw) * size_t(th) * 4);
	for (int y = 0; y < th; ++y) {
		int y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
		for (int x = 0; x < tw; ++x) {
			int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw);
			unsigned sum[4] = {};
			unsigned n = 0;
			for (int yy = y0; yy < y1; ++yy) {
				for (int xx = x0; xx < x1; ++xx, ++n) {
					const uint8_t *p = &rgba[(size_t(yy) * size_t(w) + size_t(xx)) * 4];
					for (int k = 0; k < 4; ++k) sum[k] += p[k];
				}
			}
			for (int k = 0; k < 4; ++k) out[(size_t(y) * size_t(tw) + size_t(x)) * 4 + size_t(k)] = uint8_t(sum[k] / n);
		}
	}
	return out;
}

}  // namespace

std::string utf8_text(const std::string &latin1) { return deo::latin1_to_utf8(deo::Bytes(latin1.begin(), latin1.end())); }

const char *class_icon(const std::string &cls) {
	if (cls == "Folder") return ICON_FOLDER;
	if (cls == "Behavior" || cls == "Class") return ICON_FILE_CODE;
	if (cls.rfind("Description", 0) == 0 && cls != "Description\\Pilot" && cls != "Description\\Environment" &&
	    cls != "Description\\Creature\\Fighter" && cls != "Description\\SessionMaster")
		return ICON_FILE_TEXT;
	return category_icon(int(ed::category_of(cls)));
}

unsigned class_tint(const std::string &cls) {
	switch (ed::category_of(cls)) {
		case ed::Category::Pilots: return kAccentText;
		case ed::Category::Robots: return 0x8fb8ff;
		case ed::Category::Arenas: return 0xd9b45a;
		case ed::Category::Tournaments: return 0xf5d36b;
		case ed::Category::Images: return 0x7fd1c7;
		case ed::Category::Meshes: return 0xc3a6ff;
		case ed::Category::Sounds:
		case ed::Category::Music: return 0x8fb8ff;
		case ed::Category::Scripts: return 0x8fe3a8;
		case ed::Category::Particles: return 0xff9bd2;
		default: return cls == "Folder" ? 0xd9b45a : kTextMuted;
	}
}

// ---------------------------------------------------------------------------------------------
// Objects

const deo::File *Editor::file_of(const ObjectRef &r) {
	if (r.kind == ObjectRef::Addon) return project_ ? &project_->file() : nullptr;
	if (r.kind != ObjectRef::Base || !base_ || r.file >= base_->files().size()) return nullptr;
	return sync_loads() ? &base_->model(r.file) : base_->model_async(r.file);
}

int Editor::addon_index(const ObjectRef &r) const {
	if (r.kind != ObjectRef::Addon || !project_) return -1;
	return project_->find(r.guid);
}

ObjectRef Editor::ref_for_guid(const ed::Guid &g) const {
	if (project_ && project_->find(g) >= 0) return ObjectRef::addon(g);
	if (base_) {
		if (const ed::BaseObject *b = base_->by_guid(g)) return ObjectRef::base(*b);
	}
	return {};
}

std::string Editor::object_label(const ObjectRef &r) {
	if (r.kind == ObjectRef::Base && base_) return utf8_text(base_->files()[r.file].objects[r.index].name);
	int i = addon_index(r);
	return i >= 0 ? utf8_text(project_->file().objects[size_t(i)].info.name_text()) : std::string();
}

ed::ReferenceIndex *Editor::reference_index() {
	if (!base_) return nullptr;
	if (!refs_) refs_ = std::make_unique<ed::ReferenceIndex>(*base_);
	refs_->start();
	return refs_.get();
}

void Editor::select(const ObjectRef &r) {
	selection_ = r;
	selected_object_ = addon_index(r);
}

const std::vector<int> *Editor::parents_of(size_t file) {
	auto it = parents_.find(file);
	if (it != parents_.end()) return &it->second;
	const deo::File *f = file_of(ObjectRef{ObjectRef::Base, file, 0, {}});
	if (!f) return nullptr;
	return &(parents_[file] = ed::object_parents(*f));
}

void Editor::open_object(const ObjectRef &r) {
	for (size_t i = 1; i < docs_.size(); ++i) {
		if (docs_[i]->shows(r)) {
			active_ = i;
			return;
		}
	}
	if (r.kind == ObjectRef::Addon) {
		int i = addon_index(r);
		if (i < 0) return;
		if (project_->file().objects[size_t(i)].info.class_text() == "Actor\\Creature\\Pilot") {
			open_pilot(r.guid);
			return;
		}
	} else if (r.kind == ObjectRef::Base && base_) {
		const ed::BaseObject &bo = base_->files()[r.file].objects[r.index];
		if (bo.class_path == "Actor\\Creature\\Pilot") {
			docs_.push_back(make_base_pilot_document(*this, bo));
			active_ = docs_.size() - 1;
			return;
		}
		if (bo.class_path == "Description\\Pilot") {
			const deo::File &f = base_->model(r.file);
			ed::Guid res = ed::description_resource(f.objects[r.index]);
			if (const ed::BaseObject *p = base_->by_guid(res); p && p->class_path == "Actor\\Creature\\Pilot") {
				open_object(ObjectRef::base(*p));
				return;
			}
		}
	} else {
		return;
	}
	docs_.push_back(make_object_document(*this, r));
	active_ = docs_.size() - 1;
}

ObjectRef Editor::image_source(const ObjectRef &r, bool *pending) {
	if (pending) *pending = false;
	const deo::File *f = file_of(r);
	if (!f) {
		if (pending) *pending = true;
		return {};
	}
	uint32_t index = r.kind == ObjectRef::Base ? r.index : uint32_t(std::max(0, addon_index(r)));
	if (index >= f->objects.size()) return {};
	const deo::Object &o = f->objects[index];
	const std::string cls = o.info.class_text();
	auto child_image = [&](const ObjectRef &owner, const deo::File &file, uint32_t idx) -> ObjectRef {
		for (uint32_t c : ed::hierarchy_children(file.objects[idx])) {
			if (c < file.objects.size() && file.objects[c].info.class_text() == "Image\\Standard") {
				if (owner.kind == ObjectRef::Addon) return ObjectRef::addon(file.objects[c].info.guid);
				return ObjectRef{ObjectRef::Base, owner.file, c, file.objects[c].info.guid};
			}
		}
		return {};
	};
	if (cls == "Image\\Standard") return r;
	if (cls == "Actor\\Creature\\Pilot") return child_image(r, *f, index);
	if (cls.rfind("Description", 0) == 0) {
		ed::Guid icon = ed::description_icon(*f, o);
		if (!ed::guid_zero(icon)) {
			ObjectRef ir = ref_for_guid(icon);
			if (ir.kind == ObjectRef::Addon) return ir;
			if (ir && base_->files()[ir.file].objects[ir.index].class_path == "Image\\Standard") return ir;
		}
		ed::Guid res = ed::description_resource(o);
		ObjectRef rr = ed::guid_zero(res) ? ObjectRef{} : ref_for_guid(res);
		if (rr && !(rr == r)) {
			const deo::File *rf = file_of(rr);
			if (!rf) {
				if (pending) *pending = true;
				return {};
			}
			uint32_t ri = rr.kind == ObjectRef::Base ? rr.index : uint32_t(std::max(0, addon_index(rr)));
			if (rf->objects[ri].info.class_text() == "Actor\\Creature\\Pilot") return child_image(rr, *rf, ri);
		}
	}
	return {};
}

GLuint Editor::thumbnail(const ObjectRef &r, float *aspect) {
	*aspect = 1;
	if (r.kind == ObjectRef::Addon) {
		int i = addon_index(r);
		if (i < 0) return 0;
		const std::string cls = project_->file().objects[size_t(i)].info.class_text();
		if (cls == "Actor\\Creature\\Pilot") return addon_photo_texture(*this, r.guid, addon_thumbs_);
		if (cls != "Image\\Standard") return 0;
		auto &entry = addon_thumbs_[ed::guid_key(r.guid)];
		if (entry.second == project_->revision()) return entry.first;
		int w, h;
		auto px = ed::image_rgba(project_->file().objects[size_t(i)], &w, &h);
		if (!px.empty()) {
			int tw, th;
			auto small = shrink(px, w, h, 160, &tw, &th);
			entry.first = upload_texture(tw, th, small, entry.first);
		}
		entry.second = project_->revision();
		return entry.first;
	}
	if (r.kind != ObjectRef::Base) return 0;
	const uint64_t key = uint64_t(r.file) << 32 | r.index;
	auto it = thumbs_.find(key);
	if (it != thumbs_.end() && it->second.done) {
		*aspect = it->second.aspect;
		return it->second.texture;
	}
	if (thumb_budget_ <= 0) return 0;
	const std::string &cls = base_->files()[r.file].objects[r.index].class_path;
	bool may_have = cls == "Image\\Standard" || cls == "Actor\\Creature\\Pilot" || cls.rfind("Description", 0) == 0;
	Thumb &t = thumbs_[key];
	if (!may_have) {
		t.done = true;
		return 0;
	}
	bool pending = false;
	ObjectRef src = image_source(r, &pending);
	if (!src) {
		t.done = !pending;
		return 0;
	}
	const deo::File *sf = file_of(src);
	if (!sf) return 0;
	--thumb_budget_;
	int w, h;
	uint32_t si = src.kind == ObjectRef::Base ? src.index : uint32_t(std::max(0, addon_index(src)));
	auto px = ed::image_rgba(sf->objects[si], &w, &h);
	t.done = true;
	if (px.empty()) return 0;
	int tw, th;
	auto small = shrink(px, w, h, 160, &tw, &th);
	t.texture = upload_texture(tw, th, small);
	t.aspect = float(tw) / float(th);
	*aspect = t.aspect;
	return t.texture;
}

// ---------------------------------------------------------------------------------------------
// Sound

void Editor::play_wave(const ed::Bytes &wav) {
	stop_sound();
	if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		log(LogLine::Error, std::string("No audio: ") + SDL_GetError());
		return;
	}
	SDL_AudioSpec spec;
	Uint8 *buf = nullptr;
	Uint32 len = 0;
	if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(wav.data(), int(wav.size())), 1, &spec, &buf, &len)) {
		log(LogLine::Error, std::string("Cannot decode the sound: ") + SDL_GetError());
		return;
	}
	spec.callback = nullptr;
	SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
	if (!dev) {
		log(LogLine::Error, std::string("Cannot open the audio device: ") + SDL_GetError());
		SDL_FreeWAV(buf);
		return;
	}
	SDL_QueueAudio(dev, buf, len);
	SDL_FreeWAV(buf);
	SDL_PauseAudioDevice(dev, 0);
	audio_device_ = dev;
}

void Editor::stop_sound() {
	if (audio_device_) SDL_CloseAudioDevice(audio_device_);
	audio_device_ = 0;
}

bool Editor::sound_playing() {
	if (audio_device_ && SDL_GetQueuedAudioSize(audio_device_) == 0) stop_sound();
	return audio_device_ != 0;
}

// ---------------------------------------------------------------------------------------------
// Content Browser

void Editor::draw_content_browser() {
	if (!ImGui::Begin((std::string(ICON_FOLDER "  Content Browser###ContentBrowser")).c_str())) {
		ImGui::End();
		return;
	}
	if (!base_) {
		ImGui::End();
		return;
	}
	const float tree_w = ui::px(250);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
	ImGui::BeginChild("tree", ImVec2(tree_w, 0));
	draw_browser_tree();
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::SameLine(0, 0);
	{
		ImVec2 a = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddLine(a, a + ImVec2(0, ImGui::GetContentRegionAvail().y), rgb(kDivider));
	}
	ImGui::SameLine(0, ui::px(12));
	ImGui::BeginChild("items", ImVec2(0, 0), 0);
	draw_browser_items();
	ImGui::EndChild();
	ImGui::End();
}

void Editor::draw_browser_tree() {
	static bool open_addon = true, open_base = true;
	if (project_) {
		std::array<int, ed::kCategoryCount> counts{};
		for (auto &o : project_->file().objects) ++counts[size_t(ed::category_of(o.info.class_text()))];
		ui::TreeRow root;
		root.icon = ICON_PACKAGE_OPEN, root.icon_colour = 0x8fe3a8, root.label = project_->agent_name() + " (Add-on)";
		root.count = int(project_->file().objects.size());
		root.has_children = true, root.open = &open_addon, root.selected = !scope_.base && scope_.category < 0;
		if (ui::tree_row("addon", root)) scope_ = Scope{false, false, -1, -1, -1};
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", project_->path().string().c_str());
		if (open_addon) {
			for (int c = 0; c < ed::kCategoryCount; ++c) {
				if (!counts[size_t(c)] && ed::Category(c) != ed::Category::Pilots) continue;
				ui::TreeRow r;
				r.icon = category_icon(c), r.icon_colour = class_tint(c == 0 ? "Actor\\Creature\\Pilot" : ""), r.label = ed::category_name(ed::Category(c));
				r.count = counts[size_t(c)], r.depth = 1, r.selected = !scope_.base && scope_.category == c;
				ImGui::PushID(c);
				if (ui::tree_row("a", r)) scope_ = Scope{false, false, c, -1, -1};
				ImGui::PopID();
			}
		}
	} else {
		ui::TreeRow r;
		r.icon = ICON_FOLDER_PLUS, r.icon_colour = kAccentText, r.label = "Open or create an add-on...";
		if (ui::tree_row("noaddon", r)) with_addon("Open an add-on to work on, or make a new one. Base content stays read-only; copies go into the add-on.", [] {});
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("No add-on open: browsing the base game only");
	}
	ImGui::Dummy(ImVec2(0, ui::px(4)));
	// Base game, by type or by file.
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(4), ui::px(4)));
	const float half = (ImGui::GetContentRegionAvail().x - ui::px(4)) * 0.5f;
	for (int k = 0; k < 2; ++k) {
		if (k) ImGui::SameLine();
		bool on = scope_.by_file == (k == 1);
		ImGui::PushStyleColor(ImGuiCol_Button, vec(on ? kSelection : kButton));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(on ? 0x4a4380 : kButtonHover));
		if (ImGui::Button(k ? ICON_FILES "  By File" : ICON_LAYOUT_GRID "  By Type", ImVec2(half, ui::px(26))) && !on) {
			scope_ = Scope{true, k == 1, -1, -1, -1};
		}
		ImGui::PopStyleColor(2);
	}
	ImGui::PopStyleVar();
	ui::TreeRow base;
	base.icon = ICON_LOCK, base.icon_colour = kTextMuted, base.label = "Base Game (Read-Only)", base.has_children = true, base.open = &open_base;
	base.count = int(base_->object_count());
	base.selected = scope_.base && scope_.category < 0 && scope_.file < 0;
	if (ui::tree_row("base", base)) scope_ = Scope{true, scope_.by_file, -1, -1, -1};
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", base_->dir().string().c_str());
	if (!open_base) return;
	if (!scope_.by_file) {
		for (int c = 0; c < ed::kCategoryCount; ++c) {
			ui::TreeRow r;
			r.icon = category_icon(c), r.icon_colour = kTextMuted, r.label = ed::category_name(ed::Category(c));
			r.count = int(base_->category(ed::Category(c)).size()), r.depth = 1;
			r.selected = scope_.base && !scope_.by_file && scope_.category == c;
			ImGui::PushID(c);
			if (ui::tree_row("b", r)) scope_ = Scope{true, false, c, -1, -1};
			ImGui::PopID();
		}
		return;
	}
	for (size_t f = 0; f < base_->files().size(); ++f) {
		const auto &fi = base_->files()[f];
		bool &open = file_open_[f];
		ui::TreeRow r;
		r.icon = ICON_FILE, r.icon_colour = 0x9a8f78, r.label = fi.stem, r.count = int(fi.objects.size()), r.depth = 1;
		r.has_children = true, r.open = &open;
		r.selected = scope_.by_file && scope_.file == int(f) && scope_.folder < 0;
		ImGui::PushID(int(f));
		if (ui::tree_row("f", r)) scope_ = Scope{true, true, -1, int(f), -1};
		if (open) {
			const std::vector<int> *parents = parents_of(f);
			const deo::File *file = parents ? file_of(ObjectRef{ObjectRef::Base, f, 0, {}}) : nullptr;
			if (!parents || !file) {
				ui::TreeRow l;
				l.label = "Reading...", l.depth = 2, l.disabled = true;
				ui::tree_row("loading", l);
			} else {
				std::vector<int> has_kids(file->objects.size(), 0);
				for (int p : *parents) {
					if (p >= 0) has_kids[size_t(p)] = 1;
				}
				std::function<void(int, int)> rows = [&](int parent, int depth) {
					for (size_t i = 0; i < parents->size(); ++i) {
						if ((*parents)[i] != parent || !has_kids[i]) continue;
						const auto &info = file->objects[i].info;
						bool &sub = file_open_[(uint64_t(f) + 1) << 32 | i];
						ui::TreeRow c;
						c.icon = info.class_text() == "Folder" ? (sub ? ICON_FOLDER_OPEN : ICON_FOLDER) : class_icon(info.class_text());
						c.icon_colour = class_tint(info.class_text());
						c.label = utf8_text(info.name_text()), c.depth = depth, c.has_children = true, c.open = &sub;
						c.selected = scope_.by_file && scope_.file == int(f) && scope_.folder == int(i);
						ImGui::PushID(int(i));
						if (ui::tree_row("o", c)) scope_ = Scope{true, true, -1, int(f), int(i)};
						if (sub && depth < 8) rows(int(i), depth + 1);
						ImGui::PopID();
					}
				};
				rows(-1, 2);
			}
		}
		ImGui::PopID();
	}
}

void Editor::rebuild_items() {
	const std::string search = lower(content_filter_);
	std::string key = std::to_string(scope_.base) + "/" + std::to_string(scope_.by_file) + "/" + std::to_string(scope_.category) + "/" +
	                  std::to_string(scope_.file) + "/" + std::to_string(scope_.folder) + "/" + std::to_string(type_filter_) + "/" + search;
	if (!scope_.base) key += "/" + std::to_string(project_ ? project_->revision() : 0);
	if (scope_.base && scope_.by_file && scope_.file >= 0) key += base_->loaded(size_t(scope_.file)) || sync_loads() ? "/L" : "/-";
	if (key == items_key_) return;
	items_.clear();
	items_waiting_ = false;
	const int type = scope_.category >= 0 ? scope_.category : type_filter_;
	auto wanted = [&](const ed::BaseObject &o) {
		if (type >= 0 && int(o.category) != type) return false;
		return search.empty() || o.search.find(search) != std::string::npos;
	};
	if (!scope_.base) {
		if (project_) {
			for (auto &o : project_->file().objects) {
				const std::string cls = o.info.class_text();
				if (type >= 0 && int(ed::category_of(cls)) != type) continue;
				if (!search.empty() && lower(o.info.name_text() + "\n" + cls).find(search) == std::string::npos) continue;
				items_.push_back(ObjectRef::addon(o.info.guid));
			}
		}
	} else if (!scope_.by_file) {
		std::vector<const ed::BaseObject *> list;
		if (scope_.category >= 0) {
			for (const ed::BaseObject *o : base_->category(ed::Category(scope_.category))) {
				if (wanted(*o)) list.push_back(o);
			}
		} else {
			for (auto &f : base_->files()) {
				for (auto &o : f.objects) {
					if (wanted(o)) list.push_back(&o);
				}
			}
		}
		std::stable_sort(list.begin(), list.end(), [](const ed::BaseObject *a, const ed::BaseObject *b) {
			if (name_less(a->name, b->name)) return true;
			if (name_less(b->name, a->name)) return false;
			return a->file < b->file;
		});
		for (auto *o : list) items_.push_back(ObjectRef::base(*o));
	} else if (scope_.file < 0) {
		for (auto &f : base_->files()) {
			for (auto &o : f.objects) {
				if (!search.empty() && wanted(o)) items_.push_back(ObjectRef::base(o));
			}
		}
	} else {
		const auto &fi = base_->files()[size_t(scope_.file)];
		const std::vector<int> *parents = parents_of(size_t(scope_.file));
		if (!parents) {
			items_waiting_ = true;
		} else {
			std::vector<ObjectRef> folders, rest;
			for (size_t i = 0; i < fi.objects.size(); ++i) {
				bool in_scope = search.empty() ? (*parents)[i] == scope_.folder : true;
				if (!in_scope || !wanted(fi.objects[i])) continue;
				(fi.objects[i].class_path == "Folder" ? folders : rest).push_back(ObjectRef::base(fi.objects[i]));
			}
			items_ = std::move(folders);
			items_.insert(items_.end(), rest.begin(), rest.end());
		}
	}
	items_key_ = items_waiting_ ? std::string() : key;
}

void Editor::draw_browser_items() {
	ImGui::AlignTextToFramePadding();
	ui::dim_text("Type:", kTextDim);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ui::px(150));
	const int type = scope_.category >= 0 ? scope_.category : type_filter_;
	ImGui::BeginDisabled(scope_.category >= 0);
	if (ui::begin_combo("##type", type < 0 ? "All" : ed::category_name(ed::Category(type)), ImGuiComboFlags_HeightLarge)) {
		if (ImGui::Selectable("All", type < 0)) type_filter_ = -1;
		for (int c = 0; c < ed::kCategoryCount; ++c) {
			if (ImGui::Selectable((std::string(category_icon(c)) + "  " + ed::category_name(ed::Category(c))).c_str(), type == c)) type_filter_ = c;
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	std::string hint = "Search " + std::string(scope_.base ? std::to_string(base_->object_count()) + " objects" : "the add-on") + "...";
	ui::search_box("content", hint.c_str(), &content_filter_, ui::px(280));
	rebuild_items();
	ImGui::SameLine(0, ui::px(14));
	ImGui::AlignTextToFramePadding();
	char count[64];
	snprintf(count, sizeof count, "%zu item%s", items_.size(), items_.size() == 1 ? "" : "s");
	ui::dim_text(count, kTextMuted);
	const float fh = ImGui::GetFrameHeight();
	float right = ImGui::GetContentRegionMax().x - fh * 2 - ui::px(6);
	if (!scope_.base || !project_) right -= ui::px(130);
	ImGui::SameLine(right);
	if (!scope_.base || !project_) {
		if (ui::toolbar_button(ICON_PLUS, "New Pilot", ui::Tone::Normal, true, "A new pilot copied from a base pilot, in an add-on", fh)) new_pilot_dialog();
		ImGui::SameLine(0, ui::px(10));
	}
	if (ui::icon_button("grid", ICON_LAYOUT_GRID, "Tiles", true)) list_view_ = false;
	ImGui::SameLine(0, ui::px(6));
	if (ui::icon_button("list", ICON_LIST, "List", true)) list_view_ = true;

	ImGui::PushFont(nullptr, kFontSmall);
	if (scope_.base) {
		ImGui::AlignTextToFramePadding();
		ui::dim_text(ICON_LOCK, kTextMuted);
		ImGui::SameLine(0, ui::px(6));
		if (scope_.by_file && scope_.file >= 0) {
			const auto &fi = base_->files()[size_t(scope_.file)];
			std::vector<int> chain;
			if (const std::vector<int> *parents = parents_of(size_t(scope_.file))) {
				for (int at = scope_.folder; at >= 0 && chain.size() < 32; at = (*parents)[size_t(at)]) chain.push_back(at);
			}
			if (ImGui::SmallButton((fi.stem + ".DEObjects").c_str())) scope_.folder = -1;
			for (size_t k = chain.size(); k-- > 0;) {
				ImGui::SameLine(0, ui::px(4));
				ui::dim_text(ICON_CHEVRON_RIGHT, kTextFaint);
				ImGui::SameLine(0, ui::px(4));
				ImGui::PushID(int(k));
				if (ImGui::SmallButton(utf8_text(fi.objects[size_t(chain[k])].name).c_str())) scope_.folder = chain[k];
				ImGui::PopID();
			}
			if (scope_.folder >= 0) {
				ImGui::SameLine(0, ui::px(10));
				if (ImGui::SmallButton(ICON_ARROW_LEFT " Up")) {
					const std::vector<int> *parents = parents_of(size_t(scope_.file));
					scope_.folder = parents ? (*parents)[size_t(scope_.folder)] : -1;
				}
			}
		} else {
			std::string where = scope_.by_file ? (content_filter_.empty() ? "Pick a file on the left, or search every file" : "Search results in every file")
			                    : scope_.category >= 0 ? std::string("Base game: ") + ed::category_name(ed::Category(scope_.category))
			                                           : std::string("Base game: every object");
			ui::dim_text((where + "  -  read-only; double-click to view, copy a pilot to an add-on to change it").c_str(), kTextMuted);
		}
	} else {
		ImGui::AlignTextToFramePadding();
		ui::dim_text(project_ ? ("Add-on: " + project_->path().filename().string()).c_str() : "No add-on open", kTextMuted);
	}
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(0, ui::px(2)));

	ImGui::BeginChild("grid");
	if (items_waiting_) {
		ui::centered_note(ICON_CLOCK, "Reading the file...", "Large files take a moment the first time; they stay in memory after.");
		ImGui::EndChild();
		return;
	}
	if (items_.empty()) {
		if (!scope_.base && !project_) {
			ui::centered_note(ICON_PACKAGE_OPEN, "No add-on open", "Open or create an add-on to see its content here.");
		} else if (!scope_.base && content_filter_.empty()) {
			ui::centered_note(ICON_USER, "Nothing here yet", "Copy a base pilot (right-click > Copy to Add-on) or use New Pilot.");
		} else {
			ui::centered_note(ICON_SEARCH, "No matches", "Nothing matches the type and search.");
		}
		ImGui::EndChild();
		return;
	}
	auto activate = [&](const ObjectRef &r, bool dbl) {
		select(r);
		if (!dbl) return;
		if (r.kind == ObjectRef::Base && scope_.by_file) {
			const ed::BaseObject &o = base_->files()[r.file].objects[r.index];
			const std::vector<int> *parents = parents_of(r.file);
			bool kids = parents && std::find(parents->begin(), parents->end(), int(r.index)) != parents->end();
			bool viewer = o.category == ed::Category::Pilots || o.category == ed::Category::Images || o.category == ed::Category::Sounds ||
			              o.category == ed::Category::Scripts || o.class_path.rfind("Description", 0) == 0;
			if (kids && (o.class_path == "Folder" || !viewer)) {
				scope_ = Scope{true, true, -1, int(r.file), int(r.index)};
				content_filter_.clear();
				return;
			}
		}
		open_object(r);
	};
	auto context = [&](const ObjectRef &r) {
		if (!ImGui::BeginPopupContextItem("ctx")) return;
		if (ImGui::MenuItem(ICON_EYE "  Open")) activate(r, true);
		if (r.kind == ObjectRef::Base) {
			const ed::BaseObject &o = base_->files()[r.file].objects[r.index];
			if (o.class_path == "Actor\\Creature\\Pilot" && ImGui::MenuItem(ICON_COPY "  Copy to Add-on...")) copy_pilot_to_addon(o);
			if (ImGui::MenuItem(ICON_FOLDER_TREE "  Show in File")) {
				const std::vector<int> *parents = parents_of(r.file);
				scope_ = Scope{true, true, -1, int(r.file), parents ? (*parents)[r.index] : -1};
				file_open_[r.file] = true;
				content_filter_.clear();
			}
		}
		if (ImGui::MenuItem(ICON_HASH "  Copy GUID")) ImGui::SetClipboardText(r.guid.str().c_str());
		ImGui::EndPopup();
	};
	auto info_of = [&](const ObjectRef &r, std::string *name, std::string *cls, std::string *file) {
		if (r.kind == ObjectRef::Base) {
			const ed::BaseObject &o = base_->files()[r.file].objects[r.index];
			*name = utf8_text(o.name), *cls = o.class_path, *file = base_->files()[r.file].stem;
		} else {
			int i = addon_index(r);
			if (i < 0) return false;
			const auto &info = project_->file().objects[size_t(i)].info;
			*name = utf8_text(info.name_text()), *cls = info.class_text(), *file = project_->path().stem().string();
		}
		return true;
	};
	if (list_view_) {
		if (ImGui::BeginTable("list", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2);
			ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 2);
			ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 1);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ui::px(60));
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			ImGuiListClipper clip;
			clip.Begin(int(items_.size()));
			while (clip.Step()) {
				for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
					const ObjectRef &r = items_[size_t(k)];
					std::string name, cls, file;
					if (!info_of(r, &name, &cls, &file)) continue;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::PushID(k);
					ImGui::PushStyleColor(ImGuiCol_Text, vec(class_tint(cls)));
					ImGui::TextUnformatted(class_icon(cls));
					ImGui::PopStyleColor();
					ImGui::SameLine();
					if (ImGui::Selectable(name.c_str(), selection_ == r, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
						activate(r, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
					}
					context(r);
					ImGui::TableNextColumn();
					ui::dim_text(cls.c_str());
					ImGui::TableNextColumn();
					ui::dim_text(file.c_str(), kTextMuted);
					ImGui::TableNextColumn();
					ui::dim_text(r.kind == ObjectRef::Base ? ICON_LOCK " BASE" : "NEW", r.kind == ObjectRef::Base ? kTextFaint : kOk);
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
		ImGui::EndChild();
		return;
	}
	const ImVec2 tile_size(ui::px(150), ui::px(160));
	const float gap = ui::px(14);
	const float avail = ImGui::GetContentRegionAvail().x;
	const int per_row = std::max(1, int((avail + gap) / (tile_size.x + gap)));
	const int rows = int((items_.size() + size_t(per_row) - 1) / size_t(per_row));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
	ImGuiListClipper clip;
	clip.Begin(rows, tile_size.y + gap);
	while (clip.Step()) {
		for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
			for (int col = 0; col < per_row; ++col) {
				size_t k = size_t(row) * size_t(per_row) + size_t(col);
				if (k >= items_.size()) break;
				const ObjectRef &r = items_[k];
				std::string name, cls, file;
				if (!info_of(r, &name, &cls, &file)) continue;
				if (col) ImGui::SameLine();
				ui::Tile t;
				float aspect = 1;
				GLuint tex = thumbnail(r, &aspect);
				t.texture = (ImTextureID)(intptr_t)tex;
				t.aspect = cls == "Actor\\Creature\\Pilot" || cls == "Description\\Pilot" ? 0 : aspect;
				t.icon = class_icon(cls), t.tint = class_tint(cls);
				t.label = name, t.sublabel = short_class(cls);
				t.selected = selection_ == r;
				if (r.kind == ObjectRef::Base) t.badge = "BASE";
				else t.is_new = true;
				bool dbl = false;
				ImGui::PushID(int(k));
				if (ui::tile("t", t, tile_size, &dbl)) select(r);
				if (dbl) activate(r, true);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
					ImGui::SetTooltip("%s\n%s\n%s.DEObjects%s", name.c_str(), cls.c_str(), file.c_str(),
					                  r.kind == ObjectRef::Base ? "  (base game, read-only)" : "  (add-on)");
				}
				context(r);
				ImGui::PopID();
			}
		}
	}
	ImGui::PopStyleVar();
	ImGui::EndChild();
}
