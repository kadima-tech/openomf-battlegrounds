// Viewing any object: the Details body (identity, decoded fields, references, raw chunks) and
// the object viewer document (image, script, sound, robot previews).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>

#include "editor.h"
#include "icons.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "pilot.h"
#include "preview.h"
#include "theme.h"
#include "widgets.h"

using namespace theme;

namespace {

struct Resolved {
	const deo::File *file = nullptr;
	int index = -1;
	std::string file_name;
};

Resolved resolve(Editor &e, const ObjectRef &r) {
	Resolved out;
	out.file = e.file_of(r);
	if (!out.file) return out;
	if (r.kind == ObjectRef::Base) {
		out.index = int(r.index);
		out.file_name = e.base()->files()[r.file].stem + ".DEObjects";
	} else {
		out.index = e.addon_index(r);
		out.file_name = e.project() ? e.project()->path().filename().string() : "";
	}
	if (out.index < 0 || size_t(out.index) >= out.file->objects.size()) out.file = nullptr;
	return out;
}

std::string hex_dump(const ed::Bytes &b, size_t limit) {
	std::string out;
	char line[128];
	for (size_t at = 0; at < std::min(b.size(), limit); at += 16) {
		int n = snprintf(line, sizeof line, "%06zx  ", at);
		std::string ascii;
		for (size_t k = 0; k < 16; ++k) {
			if (at + k < b.size() && at + k < limit) {
				n += snprintf(line + n, sizeof line - size_t(n), "%02x ", b[at + k]);
				ascii += b[at + k] >= 0x20 && b[at + k] < 0x7f ? char(b[at + k]) : '.';
			} else {
				n += snprintf(line + n, sizeof line - size_t(n), "   ");
			}
		}
		out += line;
		out += " " + ascii + "\n";
	}
	if (b.size() > limit) out += "... " + std::to_string(b.size() - limit) + " more bytes\n";
	return out;
}

void row(const char *label, const std::string &value, bool mono = false) {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ui::dim_text(label, kTextDim);
	ImGui::TableNextColumn();
	if (mono) ImGui::PushFont(fonts.mono, kFontSmall);
	ImGui::PushTextWrapPos(0);
	ImGui::TextUnformatted(value.c_str());
	ImGui::PopTextWrapPos();
	if (mono) ImGui::PopFont();
}

// A clickable object name: selects it in the Details panel, opens it in a viewer.
void link(Editor &e, const ObjectRef &target, const std::string &text, bool in_document) {
	if (!target) {
		ui::dim_text(text.c_str(), kTextMuted);
		return;
	}
	ImGui::PushID(&target);
	ImGui::PushStyleColor(ImGuiCol_Text, vec(kAccentText));
	std::string label = text + "##" + target.guid.str();
	if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
		if (in_document) e.open_object(target);
		else e.select(target);
	}
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip(in_document ? "Open" : "Select (double-click in a viewer opens)");
	ImGui::PopID();
}

std::string target_text(Editor &e, const ObjectRef &t, const ed::Guid &g) {
	if (!t) return g.str() + "  (not in the data)";
	const deo::File *f = e.file_of(t);
	std::string cls;
	if (t.kind == ObjectRef::Base) cls = e.base()->files()[t.file].objects[t.index].class_path + ", " + e.base()->files()[t.file].stem;
	else if (f && e.addon_index(t) >= 0) cls = f->objects[size_t(e.addon_index(t))].info.class_text() + ", add-on";
	return e.object_label(t) + "   (" + cls + ")";
}

}  // namespace

void draw_object_info(Editor &e, const ObjectRef &r, bool in_document) {
	Resolved res = resolve(e, r);
	if (!res.file) {
		if (r.kind == ObjectRef::Base) ui::centered_note(ICON_CLOCK, "Reading the file...", (e.base()->files()[r.file].stem + ".DEObjects is being read.").c_str());
		else ui::centered_note(ICON_CIRCLE_ALERT, "Object gone", "It is no longer in the add-on.");
		return;
	}
	const deo::Object &o = res.file->objects[size_t(res.index)];
	const std::string cls = o.info.class_text();
	const bool base = r.kind == ObjectRef::Base;
	ImGui::PushID(&r);
	if (!in_document) {
		ImDrawList *dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float box = ui::px(40);
		dl->AddRectFilled(p, p + ImVec2(box, box), rgb(kSelection), ui::px(6));
		ImGui::PushFont(nullptr, 20);
		ImVec2 is = ImGui::CalcTextSize(class_icon(cls));
		dl->AddText(p + (ImVec2(box, box) - is) * 0.5f + ImVec2(0, ui::px(2)), rgb(0xd8d4ff), class_icon(cls));
		ImGui::PopFont();
		ImGui::SetCursorScreenPos(p + ImVec2(box + ui::px(12), 0));
		ImGui::BeginGroup();
		ImGui::PushFont(fonts.semibold, kFontUi + 1);
		ImGui::TextUnformatted(utf8_text(o.info.name_text()).c_str());
		ImGui::PopFont();
		ui::dim_text(cls.c_str());
		ImGui::EndGroup();
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ui::px(64));
		if (base) ui::badge(ICON_LOCK " BASE", 0x3b3d4b, kTextSoft);
		else ui::badge("NEW", kNew);
		ImGui::Dummy(ImVec2(0, ui::px(4)));
		const float fh = ImGui::GetFrameHeight();
		if (ui::toolbar_button(ICON_EYE, cls == "Actor\\Creature\\Pilot" && !base ? "Open Pilot Editor" : "Open", ui::Tone::Normal, true,
		                       "Open it in a viewer tab (or double-click it)", fh)) {
			e.open_object(r);
		}
		if (base && cls == "Actor\\Creature\\Pilot") {
			ImGui::SameLine();
			const ed::BaseObject &bo = e.base()->files()[r.file].objects[r.index];
			if (ui::toolbar_button(ICON_COPY, "Copy to Add-on...", ui::Tone::Accent, true,
			                       "A copy with fresh GUIDs goes into an add-on, where you can edit it", fh)) {
				e.copy_pilot_to_addon(bo);
			}
		}
		if (base) {
			ImGui::PushTextWrapPos(0);
			ui::dim_text(cls == "Actor\\Creature\\Pilot" ? "Base game pilots are read-only: copy one to an add-on to change it."
			                                              : "Base game content is read-only. Editing this type comes in a later iteration.",
			             kTextMuted);
			ImGui::PopTextWrapPos();
		}
		ImGui::Dummy(ImVec2(0, ui::px(2)));
	}

	const ImGuiTableFlags tf = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
	ImGui::SeparatorText("Object");
	if (ImGui::BeginTable("object", 2, tf)) {
		ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ui::px(110));
		ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
		row("Class", cls);
		row("GUID", o.info.guid.str(), true);
		row("File", res.file_name + (base ? "  (base game, read-only)" : "  (add-on)"));
		row("Index", std::to_string(res.index), true);
		char flags[64];
		snprintf(flags, sizeof flags, "0x%x%s", o.info.flags, o.compressed() ? "  (compressed)" : "");
		row("Flags", flags, true);
		row("Value 84", std::to_string(o.info.value84), true);
		row("Sizes", std::to_string(o.contents.size()) + " B contents, " + std::to_string(o.references.size()) + " B references", true);
		if (!o.ex.empty()) {
			std::string keys;
			for (auto &x : o.ex) {
				std::string k = ed::text_of(x.key);
				if (x.key.size() == 2 && x.key[0] == 0) k = "pixels[" + std::to_string(x.key[1]) + "]";
				keys += (keys.empty() ? "" : ", ") + k + " (" + std::to_string(x.data.size()) + " B)";
			}
			row("Extended data", keys);
		}
		ImGui::EndTable();
	}

	auto fields = ed::describe_object(*e.base(), *res.file, uint32_t(res.index), base ? nullptr : e.project());
	if (!fields.empty()) {
		ImGui::SeparatorText("Fields");
		if (ImGui::BeginTable("fields", 2, tf)) {
			ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ui::px(130));
			ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
			for (size_t k = 0; k < fields.size(); ++k) {
				const ed::Field &f = fields[k];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ui::dim_text(f.label.c_str(), kTextDim);
				ImGui::TableNextColumn();
				ImGui::PushID(int(k));
				if (!ed::guid_zero(f.link)) {
					link(e, e.ref_for_guid(f.link), f.value, in_document);
				} else {
					ImGui::PushTextWrapPos(0);
					ImGui::TextUnformatted(f.value.c_str());
					ImGui::PopTextWrapPos();
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	std::vector<uint32_t> kids = ed::hierarchy_children(o);
	int parent = -1;
	for (uint32_t i = 0; i < res.file->objects.size() && parent < 0; ++i) {
		for (uint32_t c : ed::hierarchy_children(res.file->objects[i])) {
			if (int(c) == res.index) parent = int(i);
		}
	}
	auto local_ref = [&](uint32_t i) {
		if (base) return ObjectRef{ObjectRef::Base, r.file, i, res.file->objects[i].info.guid};
		return ObjectRef::addon(res.file->objects[i].info.guid);
	};
	if (parent >= 0 || !kids.empty()) {
		ImGui::SeparatorText("Hierarchy");
		if (parent >= 0) {
			ui::dim_text("Parent", kTextDim);
			ImGui::SameLine(ui::px(120));
			link(e, local_ref(uint32_t(parent)), utf8_text(res.file->objects[size_t(parent)].info.name_text()), in_document);
		}
		for (size_t k = 0; k < kids.size() && k < 60; ++k) {
			if (kids[k] >= res.file->objects.size()) continue;
			ui::dim_text(k ? "" : ("Children (" + std::to_string(kids.size()) + ")").c_str(), kTextDim);
			ImGui::SameLine(ui::px(120));
			ImGui::PushID(int(k));
			const auto &ci = res.file->objects[kids[k]].info;
			link(e, local_ref(kids[k]), utf8_text(ci.name_text()) + "   (" + ci.class_text() + ")", in_document);
			ImGui::PopID();
		}
		if (kids.size() > 60) ui::dim_text(("... and " + std::to_string(kids.size() - 60) + " more").c_str(), kTextMuted);
	}

	bool complete = false;
	auto refs = ed::decode_references(cls, o.references, res.file->objects.size(), res.file->guid_pool.size(), &complete);
	std::vector<ed::Guid> targets;
	std::set<std::string> seen;
	for (auto &rf : refs) {
		ed::Guid g = ed::ref_guid(*res.file, rf);
		if (!ed::guid_zero(g) && seen.insert(ed::guid_key(g)).second) targets.push_back(g);
	}
	ImGui::SeparatorText(("References (" + std::to_string(targets.size()) + ")").c_str());
	if (targets.empty()) ui::dim_text(complete ? "None." : "None decoded.", kTextMuted);
	for (size_t k = 0; k < targets.size() && k < 80; ++k) {
		ImGui::PushID(int(k) + 1000);
		ObjectRef t = e.ref_for_guid(targets[k]);
		link(e, t, target_text(e, t, targets[k]), in_document);
		ImGui::PopID();
	}
	if (targets.size() > 80) ui::dim_text(("... and " + std::to_string(targets.size() - 80) + " more").c_str(), kTextMuted);
	if (!complete) ui::dim_text("Part of this class's reference layout is not decoded yet; see Chunks.", kWarning);

	ImGui::SeparatorText("Referenced by");
	std::vector<ObjectRef> users;
	if (ed::Project *p = e.project()) {
		const auto &objs = p->file().objects;
		for (uint32_t i = 0; i < objs.size(); ++i) {
			for (auto &rf : ed::decode_references(objs[i].info.class_text(), objs[i].references, objs.size(), p->file().guid_pool.size())) {
				if (ed::ref_guid(p->file(), rf) == r.guid) {
					users.push_back(ObjectRef::addon(objs[i].info.guid));
					break;
				}
			}
		}
	}
	ed::ReferenceIndex *index = base ? e.reference_index() : nullptr;
	if (index) {
		for (const ed::BaseObject *b : index->referencers(r.guid)) users.push_back(ObjectRef::base(*b));
	}
	for (size_t k = 0; k < users.size() && k < 80; ++k) {
		ImGui::PushID(int(k) + 5000);
		link(e, users[k], target_text(e, users[k], users[k].guid), in_document);
		ImGui::PopID();
	}
	if (users.size() > 80) ui::dim_text(("... and " + std::to_string(users.size() - 80) + " more").c_str(), kTextMuted);
	if (index && !index->ready()) {
		ui::dim_text(("Indexing the base game's references... " + std::to_string(index->progress()) + " / " + std::to_string(index->total()) + " files")
		                 .c_str(),
		             kTextMuted);
	} else if (users.empty()) {
		ui::dim_text(base ? "Nothing decoded refers to it." : "Nothing in the add-on refers to it.", kTextMuted);
	}

	ImGui::Dummy(ImVec2(0, ui::px(4)));
	if (ImGui::TreeNodeEx("Chunks (raw, read-only)", ImGuiTreeNodeFlags_SpanAvailWidth)) {
		struct Block {
			const char *name;
			const ed::Bytes *bytes;
		} blocks[] = {{"Contents", &o.contents}, {"References", &o.references}, {"Hierarchy", &o.hierarchy}};
		for (auto &b : blocks) {
			ed::Chunks cs = ed::split_chunks(*b.bytes);
			for (size_t k = 0; k < cs.chunks.size(); ++k) {
				std::string title = std::string(b.name) + " chunk " + std::to_string(k) + "   v" + std::to_string(cs.chunks[k].version) + ", " +
				                    std::to_string(cs.chunks[k].body.size()) + " bytes";
				ImGui::PushID(b.name);
				ImGui::PushID(int(k));
				if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
					ImGui::PushFont(fonts.mono, kFontSmall - 1);
					ImGui::PushStyleColor(ImGuiCol_Text, vec(kTextDim));
					ImGui::TextUnformatted(hex_dump(cs.chunks[k].body, 1024).c_str());
					ImGui::PopStyleColor();
					ImGui::PopFont();
					ImGui::TreePop();
				}
				ImGui::PopID();
				ImGui::PopID();
			}
			if (!cs.tail.empty()) ui::dim_text((std::string(b.name) + ": " + std::to_string(cs.tail.size()) + " bytes after the chunks").c_str(), kTextMuted);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------
// The object viewer

namespace {

class ObjectDocument : public Document {
public:
	ObjectDocument(Editor &e, ObjectRef r) : ref_(std::move(r)) { name_ = e.object_label(ref_); }
	~ObjectDocument() override { delete_texture(texture_); }
	std::string title(Editor &) const override { return name_ + (ref_.kind == ObjectRef::Base ? "  (Base)" : ""); }
	const char *icon() const override { return ICON_EYE; }
	bool alive(Editor &e) const override { return ref_.kind == ObjectRef::Base || e.addon_index(ref_) >= 0; }
	bool addon_content() const override { return ref_.kind == ObjectRef::Addon; }
	bool shows(const ObjectRef &r) const override { return r == ref_; }
	void draw(Editor &e) override;

private:
	void preview(Editor &e, const deo::Object &o, ImVec2 size);
	void image(Editor &e, ImVec2 p0, ImVec2 p1);
	void script(const deo::Object &o, ImVec2 size);
	void sound(Editor &e, const deo::Object &o, ImVec2 p0, ImVec2 p1);
	void robot(Editor &e, const std::string &fighter, ImVec2 p0, ImVec2 p1);

	ObjectRef ref_;
	std::string name_;
	ObjectRef image_;
	bool image_tried_ = false;
	std::vector<uint8_t> rgba_;
	int w_ = 0, h_ = 0;
	bool channels_[4] = {true, true, true, true};
	bool channels_applied_[4] = {false, false, false, false};
	bool fit_ = true;
	GLuint texture_ = 0;
	std::vector<std::string> lines_;
	bool script_read_ = false;
	ed::Bytes wave_;
	std::vector<float> peaks_;
	bool wave_read_ = false;
	RobotPreview robot_;
	std::string robot_error_;
	bool robot_tried_ = false;
	int animation_ = 0;
	float anim_t_ = 0, yaw_ = 0.55f;
};

void ObjectDocument::draw(Editor &e) {
	Resolved res = resolve(e, ref_);
	ImGui::PushID(this);
	const float pad = ui::px(14);
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::SetCursorPos(ImVec2(pad, pad));
	float h = avail.y - pad * 2;
	float right = std::clamp(avail.x * 0.36f, ui::px(380), ui::px(560));
	float left = avail.x - right - pad * 3;
	if (res.file) {
		preview(e, res.file->objects[size_t(res.index)], ImVec2(left, h));
	} else {
		ui::begin_panel("preview", ImVec2(left, h), kPanel, true, 8);
		ui::centered_note(ICON_CLOCK, "Reading...", "");
		ui::end_panel();
	}
	ImGui::SameLine(0, pad);
	ui::begin_panel("info", ImVec2(right, h), kPanel, true, 8, ImVec2(ui::px(18), ui::px(14)));
	if (res.file) {
		const deo::Object &o = res.file->objects[size_t(res.index)];
		const std::string cls = o.info.class_text();
		ImGui::PushFont(fonts.semibold, kFontHeading - 4);
		ImGui::TextUnformatted(name_.c_str());
		ImGui::PopFont();
		ui::dim_text(cls.c_str());
		ImGui::Dummy(ImVec2(0, ui::px(2)));
		if (ref_.kind == ObjectRef::Base) {
			ImVec2 p = ImGui::GetCursorScreenPos();
			float w = ImGui::GetContentRegionAvail().x;
			const char *text = "Base game content is read-only: the original game cannot override a base object (a clashing GUID resolves "
			                   "inconsistently), so changes go into an add-on as copies. Editing this type comes in a later iteration.";
			float th = ImGui::CalcTextSize(text, nullptr, false, w - ui::px(44)).y;
			ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w, th + ui::px(16)), rgb(0x24233a), ui::px(6));
			ImGui::GetWindowDrawList()->AddRect(p, p + ImVec2(w, th + ui::px(16)), rgb(0x3a3566), ui::px(6));
			ImGui::SetCursorScreenPos(p + ImVec2(ui::px(10), ui::px(8)));
			ImGui::PushStyleColor(ImGuiCol_Text, vec(kAccentText));
			ImGui::TextUnformatted(ICON_LOCK);
			ImGui::PopStyleColor();
			ImGui::SameLine(0, ui::px(10));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - ui::px(44));
			ui::dim_text(text, kTextSoft);
			ImGui::PopTextWrapPos();
			ImGui::SetCursorScreenPos(p + ImVec2(0, th + ui::px(24)));
		}
		ImGui::BeginChild("details", ImVec2(0, 0), 0, 0);
		draw_object_info(e, ref_, true);
		ImGui::EndChild();
	}
	ui::end_panel();
	ImGui::PopID();
}

void ObjectDocument::preview(Editor &e, const deo::Object &o, ImVec2 size) {
	const std::string cls = o.info.class_text();
	if (cls == "Behavior") {
		script(o, size);
		return;
	}
	ui::begin_panel("preview", size, kPanel, true, 8);
	ImVec2 p0 = ImGui::GetWindowPos(), p1 = p0 + size;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilledMultiColor(p0 + ImVec2(1, 1), p1 - ImVec2(1, 1), rgb(0x22212f), rgb(0x22212f), rgb(0x15161c), rgb(0x15161c));
	if (!image_tried_) {
		bool pending = false;
		image_ = e.image_source(ref_, &pending);
		image_tried_ = !pending;
	}
	const ed::Category cat = ed::category_of(cls);
	if (cls == "Noise2\\Stream") {
		sound(e, o, p0, p1);
	} else if (cat == ed::Category::Robots && cls != "ImageFilter\\OMF2Fighter") {
		std::string fighter = name_;
		if (ref_.kind == ObjectRef::Base && cls != "Description\\Creature\\Fighter") {
			fighter = e.base()->files()[ref_.file].stem;
			if (fighter.size() > 6 && fighter.compare(fighter.size() - 6, 6, "Model1") == 0) fighter.resize(fighter.size() - 6);
		}
		robot(e, fighter, p0, p1);
	} else if (image_) {
		image(e, p0, p1);
	} else {
		ImGui::SetCursorScreenPos(p0);
		ImGui::BeginChild("none", size, 0, ImGuiWindowFlags_NoBackground);
		if (cat == ed::Category::Meshes) {
			ui::centered_note(ICON_CUBOID, "Mesh preview comes later",
			                  "Drawing a mesh needs its materials and texture managers set up as the arena viewer does; that arrives with the "
			                  "arena editor. Its fields and raw chunks are on the right.");
		} else if (cat == ed::Category::Music) {
			ui::centered_note(ICON_MUSIC, "DirectMusic segment", "Music plays in the game through its DirectMusic player; previewing it here comes later.");
		} else {
			ui::centered_note(class_icon(cls), "No preview for this type", "Its decoded fields, references and raw chunks are on the right.");
		}
		ImGui::EndChild();
	}
	ui::end_panel();
}

void ObjectDocument::image(Editor &e, ImVec2 p0, ImVec2 p1) {
	if (rgba_.empty() && image_) {
		const deo::File *f = e.file_of(image_);
		int idx = image_.kind == ObjectRef::Base ? int(image_.index) : e.addon_index(image_);
		if (f && idx >= 0) rgba_ = ed::image_rgba(f->objects[size_t(idx)], &w_, &h_);
		if (rgba_.empty()) image_ = {};
		std::fill(channels_applied_, channels_applied_ + 4, false);
	}
	if (rgba_.empty()) return;
	if (!std::equal(channels_, channels_ + 4, channels_applied_)) {
		std::vector<uint8_t> shown = rgba_;
		bool only_alpha = channels_[3] && !channels_[0] && !channels_[1] && !channels_[2];
		for (size_t i = 0; i < shown.size(); i += 4) {
			if (only_alpha) {
				shown[i] = shown[i + 1] = shown[i + 2] = rgba_[i + 3];
				shown[i + 3] = 255;
				continue;
			}
			for (int c = 0; c < 3; ++c) {
				if (!channels_[c]) shown[i + size_t(c)] = 0;
			}
			if (!channels_[3]) shown[i + 3] = 255;
		}
		texture_ = upload_texture(w_, h_, shown, texture_);
		std::copy(channels_, channels_ + 4, channels_applied_);
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float bar = ui::px(52);
	ImVec2 a0 = p0 + ImVec2(ui::px(20), ui::px(20)), a1 = p1 - ImVec2(ui::px(20), bar + ui::px(10));
	ImVec2 area = a1 - a0;
	float scale = fit_ ? std::min(area.x / float(w_), area.y / float(h_)) : 1.0f / ImGui::GetIO().DisplayFramebufferScale.x;
	if (fit_ && scale > 1) scale = std::floor(scale);
	ImVec2 sz(float(w_) * scale, float(h_) * scale);
	ImVec2 c = (a0 + a1) * 0.5f;
	ImVec2 i0 = c - sz * 0.5f, i1 = c + sz * 0.5f;
	dl->PushClipRect(a0, a1, true);
	const float sq = ui::px(10);
	for (float y = i0.y; y < i1.y; y += sq) {
		for (float x = i0.x; x < i1.x; x += sq) {
			bool dark = (int((x - i0.x) / sq) + int((y - i0.y) / sq)) & 1;
			dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + sq, i1.x), std::min(y + sq, i1.y)), rgb(dark ? 0x2a2b35 : 0x34353f));
		}
	}
	dl->AddImage((ImTextureID)(intptr_t)texture_, i0, i1);
	dl->AddRect(i0 - ImVec2(1, 1), i1 + ImVec2(1, 1), rgb(kBorder));
	dl->PopClipRect();
	ImVec2 b0(p0.x, p1.y - bar);
	dl->AddRectFilled(b0, p1, rgb(kPanelHead), ui::px(8), ImDrawFlags_RoundCornersBottom);
	dl->AddLine(b0, ImVec2(p1.x, b0.y), rgb(kBorder));
	ImGui::SetCursorScreenPos(b0 + ImVec2(ui::px(14), (bar - ImGui::GetFrameHeight()) * 0.5f));
	ImGui::AlignTextToFramePadding();
	std::string info = e.object_label(image_) + "   " + std::to_string(w_) + " x " + std::to_string(h_);
	ImGui::PushFont(fonts.semibold, kFontUi);
	ImGui::TextUnformatted(info.c_str());
	ImGui::PopFont();
	ImGui::SameLine(0, ui::px(20));
	ui::dim_text("Channels", kTextDim);
	const char *names[4] = {"R", "G", "B", "A"};
	const unsigned colours[4] = {0xff6b6b, 0x6bd88a, 0x6b9cff, 0xd3d5e0};
	for (int k = 0; k < 4; ++k) {
		ImGui::SameLine(0, ui::px(6));
		ImGui::PushID(k);
		ImGui::PushStyleColor(ImGuiCol_Button, vec(channels_[k] ? kSelection : kButton));
		ImGui::PushStyleColor(ImGuiCol_Text, vec(channels_[k] ? colours[k] : kTextFaint));
		if (ImGui::Button(names[k], ImVec2(ImGui::GetFrameHeight(), 0))) channels_[k] = !channels_[k];
		ImGui::PopStyleColor(2);
		ImGui::PopID();
	}
	ImGui::SameLine(0, ui::px(20));
	if (ImGui::Button(fit_ ? "Fit" : "1:1")) fit_ = !fit_;
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Fit to the panel / actual pixels");
}

void ObjectDocument::script(const deo::Object &o, ImVec2 size) {
	if (!script_read_) {
		script_read_ = true;
		std::string text = ed::behavior_source(o);
		if (text.empty()) text = "// This Behavior's source does not parse.";
		std::istringstream in(text);
		std::string l;
		while (std::getline(in, l)) {
			std::string expanded;
			for (char c : l) {
				if (c == '\t') expanded.append(4 - expanded.size() % 4, ' ');
				else expanded += c;
			}
			lines_.push_back(std::move(expanded));
		}
	}
	ui::begin_panel("script", size, kPanel, true, 8, ImVec2(ui::px(12), ui::px(10)));
	ImGui::AlignTextToFramePadding();
	ImGui::PushFont(fonts.semibold, kFontUi);
	ImGui::TextUnformatted((ICON_FILE_CODE "  " + name_).c_str());
	ImGui::PopFont();
	ImGui::SameLine(0, ui::px(12));
	ui::dim_text((std::to_string(lines_.size()) + " lines, read-only. Script editing (compile, diff against the base class) comes later.").c_str(), kTextMuted);
	ui::code_view("code", lines_, ImGui::GetContentRegionAvail() - ImVec2(0, ui::px(2)));
	ui::end_panel();
}

void ObjectDocument::sound(Editor &e, const deo::Object &o, ImVec2 p0, ImVec2 p1) {
	if (!wave_read_) {
		wave_read_ = true;
		wave_ = ed::sound_wave(o);
		int channels = 1, bits = 16;
		for (size_t at = 12; at + 8 <= wave_.size();) {
			uint32_t len;
			memcpy(&len, &wave_[at + 4], 4);
			if (!memcmp(&wave_[at], "fmt ", 4) && len >= 16) channels = wave_[at + 10], bits = wave_[at + 22];
			if (!memcmp(&wave_[at], "data", 4)) {
				size_t n = std::min<size_t>(len, wave_.size() - at - 8);
				const uint8_t *d = &wave_[at + 8];
				size_t frame = size_t(std::max(1, channels * bits / 8));
				size_t frames = n / frame;
				const size_t columns = 600;
				peaks_.assign(columns, 0);
				for (size_t f = 0; f < frames; ++f) {
					float v = bits == 16 ? float(int16_t(d[f * frame] | d[f * frame + 1] << 8)) / 32768.0f : (float(d[f * frame]) - 128.0f) / 128.0f;
					float &peak = peaks_[std::min(columns - 1, f * columns / std::max<size_t>(frames, 1))];
					peak = std::max(peak, std::fabs(v));
				}
			}
			at += 8 + len + (len & 1);
		}
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 w0 = p0 + ImVec2(ui::px(30), (p1.y - p0.y) * 0.25f), w1 = ImVec2(p1.x - ui::px(30), p0.y + (p1.y - p0.y) * 0.65f);
	float mid = (w0.y + w1.y) * 0.5f, half = (w1.y - w0.y) * 0.5f;
	dl->AddLine(ImVec2(w0.x, mid), ImVec2(w1.x, mid), rgb(kDivider));
	for (size_t k = 0; k < peaks_.size(); ++k) {
		float x = w0.x + (w1.x - w0.x) * float(k) / float(peaks_.size());
		float hh = std::max(1.0f, peaks_[k] * half);
		dl->AddLine(ImVec2(x, mid - hh), ImVec2(x, mid + hh), rgb(0x8fb8ff, 200), std::max(1.0f, (w1.x - w0.x) / float(peaks_.size())));
	}
	if (wave_.empty()) {
		ImGui::SetCursorScreenPos(p0);
		ImGui::BeginChild("nowave", p1 - p0, 0, ImGuiWindowFlags_NoBackground);
		ui::centered_note(ICON_VOLUME_2, "No samples", "This sound has no RIFF WAVE data.");
		ImGui::EndChild();
		return;
	}
	ImGui::SetCursorScreenPos(ImVec2(w0.x, w1.y + ui::px(24)));
	bool playing = e.sound_playing();
	if (ui::toolbar_button(playing ? ICON_SQUARE : ICON_PLAY, playing ? "Stop" : "Play", ui::Tone::Play, true, nullptr, ui::px(36))) {
		if (playing) e.stop_sound();
		else e.play_wave(wave_);
	}
	ImGui::SameLine(0, ui::px(14));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui::px(8));
	ui::dim_text((std::to_string(wave_.size() / 1024) + " KB RIFF WAVE").c_str(), kTextMuted);
}

void ObjectDocument::robot(Editor &e, const std::string &fighter, ImVec2 p0, ImVec2 p1) {
	if (!robot_tried_) {
		robot_tried_ = true;
		if (robot_.load(e.base()->dir(), fighter, &robot_error_)) {
			for (size_t k = 0; k < robot_.animations().size(); ++k) {
				if (robot_.animations()[k] == "Standing") animation_ = int(k);
			}
		}
	}
	if (!robot_error_.empty() || robot_.fighter().empty()) {
		ImGui::SetCursorScreenPos(p0);
		ImGui::BeginChild("norobot", p1 - p0, 0, ImGuiWindowFlags_NoBackground);
		ui::centered_note(ICON_BOT, "No robot preview", robot_error_.empty() ? "This object is not a whole robot." : robot_error_.c_str());
		ImGui::EndChild();
		return;
	}
	anim_t_ += ImGui::GetIO().DeltaTime * 0.8f;
	if (anim_t_ > 1) anim_t_ -= 1;
	ImVec2 area = p1 - p0;
	float scale = ImGui::GetIO().DisplayFramebufferScale.x;
	std::array<std::array<float, 3>, 3> colours{{{0.55f, 0.55f, 0.6f}, {0.42f, 0.36f, 0.9f}, {0.85f, 0.85f, 0.9f}}};
	GLuint tex = robot_.render(int(area.x * scale), int(area.y * scale), colours, animation_, anim_t_, yaw_);
	if (tex) ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)tex, p0, p1, ImVec2(0, 1), ImVec2(1, 0));
	ImGui::SetCursorScreenPos(p0);
	ImGui::InvisibleButton("orbit", area);
	if (ImGui::IsItemActive()) yaw_ += ImGui::GetIO().MouseDelta.x * 0.01f;
	ImGui::SetCursorScreenPos(p0 + ImVec2(ui::px(12), ui::px(12)));
	ImGui::SetNextItemWidth(ui::px(200));
	const auto &anims = robot_.animations();
	std::string current = animation_ >= 0 && size_t(animation_) < anims.size() ? anims[size_t(animation_)] : "";
	if (ui::begin_combo("##anim", current.c_str(), ImGuiComboFlags_HeightLarge)) {
		for (size_t k = 0; k < anims.size(); ++k) {
			if (ImGui::Selectable(anims[k].c_str(), int(k) == animation_)) animation_ = int(k), anim_t_ = 0;
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ui::dim_text((fighter + " in a neutral paint; drag to turn").c_str(), kTextDim);
}

}  // namespace

std::unique_ptr<Document> make_object_document(Editor &e, const ObjectRef &r) { return std::make_unique<ObjectDocument>(e, r); }
