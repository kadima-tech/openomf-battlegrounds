// The HAR variant editor: the robot (the game's fighter shader, its spheres drawn on the bones) on
// the left, the variant's data in tabs on the right.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>

#include "assets.h"
#include "har.h"
#include "icons.h"
#include "image_io.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "pilot.h"
#include "preview.h"
#include "theme.h"
#include "widgets.h"

using namespace theme;

namespace {

// The direction mask's bits (game::InputRequirement) on a 3 x 3 pad, neutral in the middle.
const int kPadBit[9] = {7, 0, 1, 6, 8, 2, 5, 4, 3};
const char *const kPadLabel[9] = {"\xe2\x86\x96", "\xe2\x86\x91", "\xe2\x86\x97", "\xe2\x86\x90", "\xe2\x80\xa2", "\xe2\x86\x92",
                                  "\xe2\x86\x99", "\xe2\x86\x93", "\xe2\x86\x98"};
const char *const kButtons[6] = {"LP", "RP", "LK", "RK", "Jump", "Evade"};

}  // namespace

class HarDocument : public Document {
public:
	HarDocument(Editor &e, const ed::Guid &g) : guid_(g) {
		if (e.options().asset_tab >= 0) tab_ = std::clamp(e.options().asset_tab, 0, 6);
		if (e.options().asset_select >= 0) pick_ = e.options().asset_select;
		if (e.options().paint_job >= 0) paint_ = e.options().paint_job;
		render_picture_ = e.options().har_picture == "render";
	}
	~HarDocument() override { delete_texture(picture_); }
	std::string title(Editor &e) const override {
		int i = e.find_object(guid_);
		return i < 0 ? "HAR" : "HAR: " + utf8_text(e.project()->file().objects[size_t(i)].info.name_text());
	}
	const char *icon() const override { return ICON_BOT; }
	bool alive(Editor &e) const override { return e.find_object(guid_) >= 0; }
	bool addon_content() const override { return true; }
	bool shows(const ObjectRef &r) const override { return r.kind == ObjectRef::Addon && r.guid == guid_; }
	void draw(Editor &e) override;

private:
	void preview(Editor &e, ImVec2 size, const ed::HarObjects &o);
	void form(Editor &e, ImVec2 size, const ed::HarObjects &o);
	void general(Editor &e, const ed::HarObjects &o);
	void movement();
	void inputs();
	void hitboxes();
	void sounds(Editor &e);
	void paint(Editor &e, const ed::HarObjects &o);
	void script(Editor &e, const ed::HarObjects &o);

	ed::Guid guid_;
	ed::HarData data_, committed_;
	uint64_t read_revision_ = 0;
	int tab_ = 0, pick_ = 0, input_ = 0, paint_ = 0;
	uint64_t paint_revision_ = 0;
	// Choose Creature's panel (title, text, picture), re-read when the project changes.
	ed::HarPresentation pres_;
	std::string pres_title_, pres_text_;
	uint64_t pres_revision_ = 0;
	GLuint picture_ = 0;
	bool render_picture_ = false;
	void presentation(Editor &e, const ed::HarObjects &o);
	void render_picture(Editor &e, const ed::HarObjects &o);
	RobotPreview robot_;
	std::string loaded_, error_;
	int animation_ = 0;
	float anim_t_ = 0.2f, yaw_ = 0.6f;
	bool playing_ = true;
	std::string input_filter_, paint_name_ = "Paint2";
};

std::unique_ptr<Document> make_har_document(Editor &e, const ed::Guid &description) { return std::make_unique<HarDocument>(e, description); }

void HarDocument::draw(Editor &e) {
	ed::Project *p = e.project();
	if (!p) return;
	int idx = p->find(guid_);
	if (idx < 0) return;
	ed::HarObjects o = ed::har_objects(*p, uint32_t(idx));
	if (p->revision() != read_revision_) {
		data_ = committed_ = ed::read_har(*p, o);
		read_revision_ = p->revision();
	}
	ImGui::PushID(this);
	const float pad = ui::px(14);
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::SetCursorPos(ImVec2(pad, pad));
	float h = avail.y - pad * 2;
	float left = std::clamp(avail.x * 0.36f, ui::px(340), ui::px(620));
	preview(e, ImVec2(left, h), o);
	ImGui::SameLine(0, pad);
	form(e, ImVec2(avail.x - left - pad * 3, h), o);
	ImGui::PopID();
	if (!(data_ == committed_)) {
		try {
			ed::write_har(*p, o, data_);
		} catch (const std::exception &ex) {
			e.log(LogLine::Error, std::string("Cannot write the HAR: ") + ex.what());
		}
		committed_ = data_;
		read_revision_ = p->revision();
	}
}

void HarDocument::preview(Editor &e, ImVec2 size, const ed::HarObjects &o) {
	ui::begin_panel("preview", size, kPanel, true, 8);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float footer = ui::px(56);
	ImVec2 p0 = ImGui::GetWindowPos(), p1 = p0 + ImVec2(size.x, size.y - footer);
	dl->AddRectFilledMultiColor(p0 + ImVec2(1, 1), p1 - ImVec2(1, 0), rgb(0x2b2742), rgb(0x2b2742), rgb(0x15161c), rgb(0x15161c));
	std::string base = o.fighter >= 0 ? e.project()->file().objects[size_t(o.fighter)].info.name_text() : "";
	// The selected paint job's model: the add-on's own (an imported robot, a new paint job) drawn with
	// the variant's rig, or a base model.
	paint_ = o.model_descriptions.empty() ? 0 : std::clamp(paint_, 0, int(o.model_descriptions.size()) - 1);
	int own_model = -1;
	std::string base_file, base_model;
	if (!o.model_descriptions.empty()) {
		uint32_t md = uint32_t(o.model_descriptions[size_t(paint_)]);
		own_model = ed::paint_job_model(*e.project(), md);
		if (own_model < 0) {
			if (const ed::BaseObject *bm = e.base()->by_guid(ed::description_resource(e.project()->file().objects[md]))) {
				base_file = e.base()->files()[bm->file].stem;
				base_model = "#" + std::to_string(bm->index);
			}
		}
	}
	std::string key = own_model >= 0 ? "addon:" + std::to_string(own_model) + ":" + std::to_string(o.fighter) + ":" + std::to_string(paint_revision_) : base + ":" + base_file + base_model;
	if (!base.empty() && loaded_ != key) {
		loaded_ = key;
		bool ok = own_model >= 0 ? robot_.load_addon(e.project()->bytes(), uint32_t(own_model), uint32_t(o.fighter), key, &error_) : robot_.load(e.base()->dir(), base, &error_, base_file, base_model);
		if (!ok) e.log(LogLine::Error, "Preview: " + error_);
		const auto &anims = robot_.animations();
		for (size_t k = 0; k < anims.size(); ++k) {
			if (anims[k] == "Standing") animation_ = int(k);
		}
	}
	if (playing_) {
		anim_t_ += ImGui::GetIO().DeltaTime * 0.6f;
		if (anim_t_ > 1) anim_t_ -= 1;
	}
	ImVec2 area = p1 - p0;
	float scale = ImGui::GetIO().DisplayFramebufferScale.x;
	GLuint tex = robot_.render(int(area.x * scale), int(area.y * scale), {{{0.8f, 0.25f, 0.2f}, {0.85f, 0.85f, 0.9f}, {0.2f, 0.2f, 0.25f}}}, animation_, anim_t_, yaw_);
	if (tex) dl->AddImage((ImTextureID)(intptr_t)tex, p0, p1, ImVec2(0, 1), ImVec2(1, 0));
	if (tab_ == 3 || tab_ == 0) {
		for (size_t k = 0; k < data_.spheres.size(); ++k) {
			const auto &s = data_.spheres[k];
			float x, y, ppu;
			if (s.bone.empty() || !robot_.bone_screen(s.bone, &x, &y, &ppu)) continue;
			ImVec2 c = p0 + ImVec2(x / scale, y / scale);
			float r = std::max(2.0f, s.falloff * ppu / scale);
			bool sel = tab_ == 3 && int(k) == pick_;
			bool hit = ed::attack_sphere(s.name);
			unsigned col = sel ? kWarningStrong : hit ? 0xff6b6b : 0x6bb8ff;
			if (sel) dl->AddCircleFilled(c, r, rgb(col, 60), 32);
			dl->AddCircle(c, r, rgb(col, sel ? 255 : 110), 32, sel ? 2.5f : 1.0f);
			dl->AddCircleFilled(c, ui::px(2), rgb(col, 200));
		}
	}
	ImGui::SetCursorScreenPos(p0);
	ImGui::InvisibleButton("orbit", area);
	if (ImGui::IsItemActive()) yaw_ += ImGui::GetIO().MouseDelta.x * 0.01f;
	ImGui::PushFont(nullptr, kFontSmall);
	std::string job = o.model_descriptions.empty() ? "" : "  \xc2\xb7  " + utf8_text(e.project()->file().objects[size_t(o.model_descriptions[size_t(paint_)])].info.name_text());
	dl->AddText(p0 + ImVec2(ui::px(12), ui::px(10)), rgb(kTextDim), (data_.name + job + (own_model >= 0 ? "  (the add-on's model on " + base + "'s rig)" : "  (" + base + "'s model and rig)")).c_str());
	dl->AddText(p0 + ImVec2(ui::px(12), ui::px(28)), rgb(0xff6b6b), "\xe2\x97\x8f attack spheres");
	dl->AddText(p0 + ImVec2(ui::px(130), ui::px(28)), rgb(0x6bb8ff), "\xe2\x97\x8f body / hit spheres");
	ImGui::PopFont();
	ImVec2 f0(p0.x, p1.y);
	dl->AddRectFilled(f0, p0 + size, rgb(kPanelHead), ui::px(8), ImDrawFlags_RoundCornersBottom);
	dl->AddLine(f0, ImVec2(p0.x + size.x, f0.y), rgb(kBorder));
	ImGui::SetCursorScreenPos(f0 + ImVec2(ui::px(14), (footer - ImGui::GetFrameHeight()) * 0.5f));
	if (ui::icon_button("play", playing_ ? ICON_PAUSE : ICON_PLAY, playing_ ? "Pause" : "Play")) playing_ = !playing_;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(size.x - ui::px(80));
	const auto &anims = robot_.animations();
	if (ui::begin_combo("##anim", animation_ < int(anims.size()) ? anims[size_t(animation_)].c_str() : "-", ImGuiComboFlags_HeightLarge)) {
		for (size_t k = 0; k < anims.size(); ++k) {
			if (ImGui::Selectable(anims[k].c_str(), int(k) == animation_)) animation_ = int(k), anim_t_ = 0;
		}
		ImGui::EndCombo();
	}
	ui::end_panel();
}

void HarDocument::form(Editor &e, ImVec2 size, const ed::HarObjects &o) {
	ui::begin_panel("form", size, kPanel, true, 8, ImVec2(ui::px(20), 0));
	ImGui::Dummy(ImVec2(0, ui::px(14)));
	ImGui::PushFont(fonts.semibold, kFontHeading);
	ImGui::TextUnformatted(("HAR: " + data_.name).c_str());
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	ui::tab_strip("tabs", {"General", "Movement", "Inputs", "Hitboxes", "Sounds", "Paint", "Script"}, &tab_);
	ImGui::BeginChild("body", ImVec2(0, ImGui::GetContentRegionAvail().y - ui::px(10)), 0, 0);
	ImGui::Dummy(ImVec2(0, ui::px(12)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(10)));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(10), ui::px(7)));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(kInput));
	switch (tab_) {
		case 0: general(e, o); break;
		case 1: movement(); break;
		case 2: inputs(); break;
		case 3: hitboxes(); break;
		case 4: sounds(e); break;
		case 5: paint(e, o); break;
		default: script(e, o); break;
	}
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
	ImGui::EndChild();
	ui::end_panel();
}

void HarDocument::general(Editor &e, const ed::HarObjects &o) {
	const float lw = ui::px(180);
	ui::form_label("Name", lw);
	ImGui::SetNextItemWidth(-ui::px(4));
	ImGui::InputText("##name", &data_.name);
	ui::form_label("Listed", lw, ImGui::GetFrameHeight());
	ui::checkbox("##listed", &data_.listed);
	ImGui::SameLine(0, ui::px(12));
	ImGui::AlignTextToFramePadding();
	ui::dim_text("shown in Choose Creature", kTextMuted);
	ui::form_label("Locked by default", lw, ImGui::GetFrameHeight());
	ui::checkbox("##locked", &data_.locked);
	presentation(e, o);
	ImGui::SeparatorText("What the variant holds");
	auto row = [&](const char *label, const std::string &v) {
		ui::form_label(label, lw);
		ImGui::AlignTextToFramePadding();
		ui::dim_text(v.c_str(), kTextSoft);
	};
	const auto &objs = e.project()->file().objects;
	row("Fighter", o.fighter >= 0 ? objs[size_t(o.fighter)].info.name_text() + "  (Actor\\Creature\\OMF2Fighter, a copy)" : "-");
	row("Paint jobs", std::to_string(o.model_descriptions.size()) + " FighterModelDescription(s)");
	row("Spheres", std::to_string(data_.spheres.size()));
	row("Input sequences", std::to_string(data_.inputs.size()) + " in the robot's own bank");
	row("Sounds", std::to_string(data_.sounds.size()));
	row("Script", o.behavior >= 0 ? objs[size_t(o.behavior)].info.name_text() + ", " + std::to_string(data_.constants.size()) + " tunable constants" : "-");
	ImGui::Dummy(ImVec2(0, ui::px(6)));
	ui::dim_text("Every object is a copy with a fresh GUID, so the original game lists the variant next to the base robot; "
	             "its rig and moves are the base robot's, its model too unless it was imported (File > Import 3D Model, a robot: docs/EDITOR_SPEC.md \xc2\xa7" "5.5).",
	             kTextMuted);
}

void HarDocument::presentation(Editor &e, const ed::HarObjects &o) {
	ed::Project &p = *e.project();
	if (pres_revision_ != p.revision()) {
		pres_ = ed::read_presentation(p, o);
		pres_title_ = pres_.title, pres_text_ = pres_.text;
		pres_revision_ = p.revision();
		int w = 0, h = 0;
		std::vector<uint8_t> px;
		if (pres_.picture >= 0) px = ed::image_rgba(p.file().objects[size_t(pres_.picture)], &w, &h);
		if (!px.empty()) picture_ = upload_texture(w, h, px, picture_);
		else delete_texture(picture_), picture_ = 0;
	}
	if (render_picture_ && robot_.animations().size()) {
		render_picture_ = false;
		render_picture(e, o);
		if (e.options().save) e.save();
	}
	const float lw = ui::px(180);
	ImGui::SeparatorText("Choose Creature");
	if (!pres_.found) {
		ui::dim_text("The description has no Choose Creature panel.", kTextMuted);
		return;
	}
	if (!pres_.own) ui::dim_text("Shows the base robot's title, text and picture; changing one copies them into the add-on.", kTextMuted);
	ui::form_label("Title", lw);
	ImGui::SetNextItemWidth(-ui::px(4));
	ImGui::InputText("##ptitle", &pres_title_);
	bool commit = ImGui::IsItemDeactivatedAfterEdit();
	ui::form_label("Description", lw);
	ImGui::InputTextMultiline("##ptext", &pres_text_, ImVec2(-ui::px(4), ui::px(110)), ImGuiInputTextFlags_WordWrap);
	commit = commit || ImGui::IsItemDeactivatedAfterEdit();
	if (commit && (pres_title_ != pres_.title || pres_text_ != pres_.text)) {
		try {
			ed::write_presentation(p, o, pres_title_, pres_text_);
		} catch (const std::exception &ex) {
			e.log(LogLine::Error, std::string("Cannot change the panel: ") + ex.what());
		}
	}
	ui::form_label("Picture", lw);
	ImGui::BeginGroup();
	if (picture_ && pres_.width > 0) {
		float w = std::min(ImGui::GetContentRegionAvail().x - ui::px(4), ui::px(512));
		ImGui::Image((ImTextureID)(intptr_t)picture_, ImVec2(w, w * float(pres_.height) / float(pres_.width)));
	} else {
		ui::dim_text((std::to_string(pres_.width) + " x " + std::to_string(pres_.height) + ", the base robot's").c_str(), kTextDim);
	}
	if (ui::toolbar_button(ICON_UPLOAD, "Import PNG/BMP", ui::Tone::Ghost, true, "Scaled to the picture's size", ImGui::GetFrameHeight())) {
		e.file_dialog().open(FileDialog::OpenFile, "Choose Creature picture", {}, {".png", ".bmp"}, [&e, o](const std::filesystem::path &path) {
			try {
				ed::RgbaImage im = ed::load_image(path);
				ed::set_presentation_picture(*e.project(), o, im.width, im.height, im.rgba, path.filename().string());
				e.log(LogLine::Ok, "Imported " + path.filename().string());
			} catch (const std::exception &ex) {
				e.log(LogLine::Error, ex.what());
			}
		});
	}
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_CAMERA, "Render from the model", ui::Tone::Ghost, robot_.animations().size() > 0, "The selected paint job in four poses, as the shipped pictures show the robot", ImGui::GetFrameHeight())) render_picture(e, o);
	ImGui::EndGroup();
}

// Tiles of the robot (the selected paint job) in four poses on a dark backdrop, as the shipped strips.
void HarDocument::render_picture(Editor &e, const ed::HarObjects &o) {
	const int W = pres_.width > 0 ? pres_.width : 512, H = pres_.height > 0 ? pres_.height : 128;
	const int tiles = std::max(1, W / std::max(1, H)), side = H, big = side * 2;
	const auto &anims = robot_.animations();
	auto find = [&](std::initializer_list<const char *> names) {
		for (const char *n : names) {
			for (size_t k = 0; k < anims.size(); ++k) {
				if (anims[k].find(n) != std::string::npos) return int(k);
			}
		}
		return 0;
	};
	struct Shot {
		int anim;
		float t, yaw;
	};
	const Shot shots[4] = {{find({"Standing"}), 0.2f, 0.5f}, {find({"RightPunchHeavy", "PunchHeavy", "Punch"}), 0.45f, 1.2f}, {find({"KickHeavy", "Kick"}), 0.5f, -0.9f}, {find({"Winning", "Win"}), 0.6f, 0.2f}};
	std::vector<uint8_t> strip(size_t(W) * size_t(H) * 4, 255);
	const std::array<std::array<float, 3>, 3> paint = {{{0.8f, 0.25f, 0.2f}, {0.85f, 0.85f, 0.9f}, {0.2f, 0.2f, 0.25f}}};
	for (int k = 0; k < tiles; ++k) {
		const Shot &sh = shots[k % 4];
		robot_.set_zoom(1.5f);
		robot_.render(big, big, paint, sh.anim, sh.t, sh.yaw);
		robot_.set_zoom(1);
		int pw = 0, ph = 0;
		auto px = robot_.pixels(&pw, &ph);
		if (pw != big || ph != big) continue;
		auto small = ed::resize_rgba(big, big, px, side, side);
		for (int y = 0; y < side; ++y) {
			for (int x = 0; x < side; ++x) {
				const uint8_t *s0 = &small[(size_t(y) * size_t(side) + size_t(x)) * 4];
				uint8_t *d = &strip[(size_t(y) * size_t(W) + size_t(k * side + x)) * 4];
				float a = s0[3] / 255.0f;
				// The backdrop: a dark violet gradient, lighter towards the top.
				float g = 1 - float(y) / float(side);
				const float bg[3] = {18 + 30 * g, 16 + 22 * g, 30 + 40 * g};
				for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp(s0[c] * a + bg[c] * (1 - a), 0.0f, 255.0f));
				d[3] = 255;
			}
		}
	}
	try {
		ed::set_presentation_picture(*e.project(), o, W, H, strip, "rendered");
		e.log(LogLine::Ok, "Rendered the Choose Creature picture");
	} catch (const std::exception &ex) {
		e.log(LogLine::Error, ex.what());
	}
}

void HarDocument::movement() {
	const auto &info = ed::har_stat_infos();
	const char *group = "";
	for (size_t k = 0; k < info.size(); ++k) {
		if (info[k].derived) continue;
		if (strcmp(group, info[k].group)) {
			group = info[k].group;
			ImGui::SeparatorText(group);
		}
		ImGui::PushID(int(k));
		ui::form_label(info[k].name, ui::px(200));
		ImGui::SetNextItemWidth(-ui::px(4));
		float v = data_.stats[k];
		float speed = std::max(0.001f, std::fabs(v) * 0.01f);
		if (ImGui::DragFloat("##v", &v, speed, 0, 0, "%.4g")) data_.stats[k] = v;
		ImGui::PopID();
	}
	ui::dim_text("Turning speeds are 0.6x (run) and 0.75x (walk, back-up) of the straight speeds, as the game computes them.", kTextMuted);
}

void HarDocument::inputs() {
	if (data_.inputs.empty()) {
		ui::dim_text("This robot's bank has no input sequences.", kTextMuted);
		return;
	}
	input_ = std::clamp(input_, 0, int(data_.inputs.size()) - 1);
	float list_w = ui::px(220);
	ImGui::BeginChild("seqs", ImVec2(list_w, 0), ImGuiChildFlags_Borders);
	for (size_t k = 0; k < data_.inputs.size(); ++k) {
		if (ImGui::Selectable(data_.inputs[k].name.c_str(), int(k) == input_)) input_ = int(k);
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("steps", ImVec2(0, 0), 0);
	ed::HarInput &in = data_.inputs[size_t(input_)];
	ImGui::PushFont(fonts.semibold, kFontUi);
	ImGui::TextUnformatted(in.name.c_str());
	ImGui::PopFont();
	ui::dim_text("Steps are matched newest last. A direction or button lit is required; age and gap are frames (0 = any).", kTextMuted);
	const float cell = ui::px(30);
	for (size_t s = 0; s < in.steps.size(); ++s) {
		ed::HarStep &st = in.steps[s];
		ImGui::PushID(int(s));
		ImGui::BeginGroup();
		ImGui::PushFont(fonts.semibold, kFontSmall);
		ui::dim_text(("Step " + std::to_string(s + 1)).c_str(), kAccentText);
		ImGui::PopFont();
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				int bit = kPadBit[r * 3 + c];
				bool on = (st.directions >> bit) & 1;
				ImGui::PushID(r * 3 + c);
				ImGui::PushStyleColor(ImGuiCol_Button, vec(on ? kSelection : kButton));
				ImGui::PushStyleColor(ImGuiCol_Text, vec(on ? kText : kTextFaint));
				if (ImGui::Button(kPadLabel[r * 3 + c], ImVec2(cell, cell))) st.directions ^= uint16_t(1 << bit);
				ImGui::PopStyleColor(2);
				ImGui::PopID();
				if (c < 2) ImGui::SameLine(0, ui::px(3));
			}
		}
		ImGui::EndGroup();
		ImGui::SameLine(0, ui::px(16));
		ImGui::BeginGroup();
		ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
		for (int b = 0; b < 6; ++b) {
			bool req = (st.mask >> b) & 1, down = (st.value >> b) & 1;
			ImGui::PushID(100 + b);
			unsigned bg = !req ? kButton : down ? 0x2f8f4e : 0x6a3030;
			ImGui::PushStyleColor(ImGuiCol_Button, vec(bg));
			if (ImGui::Button(kButtons[b], ImVec2(ui::px(56), cell))) {
				if (!req) st.mask |= uint16_t(1 << b), st.value |= uint16_t(1 << b);
				else if (down) st.value &= uint16_t(~(1 << b));
				else st.mask &= uint16_t(~(1 << b));
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(!req ? "Any" : down ? "Pressed" : "Released");
			ImGui::PopStyleColor();
			ImGui::PopID();
			if (b % 3 != 2) ImGui::SameLine(0, ui::px(3));
		}
		ImGui::EndGroup();
		ImGui::SameLine(0, ui::px(16));
		ImGui::BeginGroup();
		ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
		auto u16 = [](const char *label, uint16_t *v) {
			int x = *v;
			ImGui::SetNextItemWidth(ui::px(90));
			if (ImGui::DragInt(label, &x, 0.2f, 0, 600)) *v = uint16_t(std::clamp(x, 0, 65535));
		};
		u16("min age", &st.min_age);
		ImGui::SameLine();
		u16("max age", &st.max_age);
		u16("min gap", &st.min_gap);
		ImGui::SameLine();
		u16("max gap", &st.max_gap);
		ImGui::EndGroup();
		ImGui::SameLine();
		if (ui::icon_button("x", ICON_X, "Remove the step", in.steps.size() > 1)) {
			in.steps.erase(in.steps.begin() + long(s));
			ImGui::PopID();
			break;
		}
		ImGui::Separator();
		ImGui::PopID();
	}
	if (ui::toolbar_button(ICON_PLUS, "Step", ui::Tone::Ghost, true, nullptr, ImGui::GetFrameHeight())) in.steps.push_back({});
	ImGui::EndChild();
}

void HarDocument::hitboxes() {
	ui::dim_text("Sphere affectors under the fighter: hotspot (full effect) and falloff (reach) in model units. "
	             "The preview draws them on their bones; the selected one is orange.",
	             kTextMuted);
	if (ImGui::BeginTable("spheres", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
		ImGui::TableSetupColumn("Sphere", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("Hotspot", ImGuiTableColumnFlags_WidthStretch, 0.8f);
		ImGui::TableSetupColumn("Falloff", ImGuiTableColumnFlags_WidthStretch, 0.8f);
		ImGui::TableHeadersRow();
		for (size_t k = 0; k < data_.spheres.size(); ++k) {
			auto &s = data_.spheres[k];
			ImGui::PushID(int(k));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(s.name.c_str(), int(k) == pick_, ImGuiSelectableFlags_AllowOverlap)) pick_ = int(k);
			ImGui::TableNextColumn();
			ui::dim_text(s.bone.c_str(), kTextDim);
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1);
			if (ImGui::DragFloat("##h", &s.hotspot, 0.1f, 0, 1000, "%.2f")) pick_ = int(k);
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1);
			if (ImGui::DragFloat("##f", &s.falloff, 0.1f, 0, 1000, "%.2f")) pick_ = int(k);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

void HarDocument::sounds(Editor &e) {
	ui::dim_text("The robot's own sounds (Noise2\\Stream). Replace one with a WAV file.", kTextMuted);
	for (auto &s : data_.sounds) {
		ImGui::PushID(s.object);
		ui::form_label(s.name.c_str(), ui::px(200));
		std::string src = s.source.empty() ? "(base sound)" : s.source;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (ImGui::GetFrameHeight() + ui::px(8)) * 2);
		ImGui::InputText("##src", &src, ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ui::icon_button("play", ICON_PLAY, "Play")) e.play_wave(ed::sound_wave(e.project()->file().objects[size_t(s.object)]));
		ImGui::SameLine();
		int obj = s.object;
		if (ui::icon_button("imp", ICON_FOLDER_OPEN, "Replace with a WAV file")) {
			e.file_dialog().open(FileDialog::OpenFile, "Replace sound", {}, {".wav"}, [&e, obj](const std::filesystem::path &path) {
				std::ifstream f(path, std::ios::binary);
				ed::Bytes wav{std::istreambuf_iterator<char>(f), {}};
				if (wav.size() < 12 || memcmp(wav.data(), "RIFF", 4) || memcmp(wav.data() + 8, "WAVE", 4)) {
					e.log(LogLine::Error, path.filename().string() + " is not a WAV file");
					return;
				}
				ed::import_sound(*e.project(), uint32_t(obj), wav, path.filename().string());
				e.log(LogLine::Ok, "Imported " + path.filename().string());
			});
		}
		ImGui::PopID();
	}
}

void HarDocument::paint(Editor &e, const ed::HarObjects &o) {
	ui::dim_text("Paint jobs are Choose Creature's Model list. A new one copies the selected one's model; replace its images (export one to paint over it).", kTextMuted);
	ui::dim_text("RGBStatic: the model's own colours. RGBMain: the paint mask (R, G, B take the pilot's colours). ReflectionMask, Bump: the shine.", kTextDim);
	const auto &objs = e.project()->file().objects;
	if (ImGui::BeginTable("jobs", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
		ImGui::TableSetupColumn("Paint job", ImGuiTableColumnFlags_WidthStretch, 1);
		ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 1.2f);
		for (size_t k = 0; k < o.model_descriptions.size(); ++k) {
			int m = o.model_descriptions[k];
			ImGui::PushID(m);
			ImGui::TableNextRow(0, ui::px(28));
			ImGui::TableNextColumn();
			if (ImGui::Selectable(utf8_text(objs[size_t(m)].info.name_text()).c_str(), int(k) == paint_, ImGuiSelectableFlags_SpanAllColumns)) paint_ = int(k);
			ImGui::TableNextColumn();
			int own = ed::paint_job_model(*e.project(), uint32_t(m));
			ui::dim_text(own >= 0 ? (utf8_text(objs[size_t(own)].info.name_text()) + "  (in the add-on)").c_str() : "the base robot's (read-only)", own >= 0 ? kTextSoft : kTextDim);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (o.model_descriptions.empty()) return;
	const int job = o.model_descriptions[size_t(paint_)];
	ImGui::SeparatorText(utf8_text(objs[size_t(job)].info.name_text()).c_str());
	auto images = ed::paint_images(*e.project(), uint32_t(job));
	if (images.empty()) ui::dim_text("Uses the base robot's model: make a new paint job to change its images.", kTextDim);
	for (auto &img : images) {
		ImGui::PushID(img.object);
		int w = 0, h = 0, type = 0;
		ed::image_header(objs[size_t(img.object)], &type, &w, &h);
		ui::form_label(img.name.c_str(), ui::px(200));
		ImGui::AlignTextToFramePadding();
		ui::dim_text((std::to_string(w) + " x " + std::to_string(h)).c_str(), kTextDim);
		ImGui::SameLine(ui::px(330));
		int obj = img.object;
		if (ui::toolbar_button(ICON_UPLOAD, "Import PNG/BMP", ui::Tone::Ghost, true, "Replaces the image, scaled to its size", ImGui::GetFrameHeight())) {
			e.file_dialog().open(FileDialog::OpenFile, "Import " + img.name, {}, {".png", ".bmp"}, [this, &e, obj, w, h](const std::filesystem::path &path) {
				try {
					ed::RgbaImage im = ed::load_image(path);
					int tw = w > 0 ? w : im.width, th = h > 0 ? h : im.height;
					ed::set_image_pixels(*e.project(), uint32_t(obj), tw, th, ed::resize_rgba(im.width, im.height, im.rgba, tw, th), path.filename().string());
					++paint_revision_;
					e.log(LogLine::Ok, "Imported " + path.filename().string());
				} catch (const std::exception &ex) {
					e.log(LogLine::Error, ex.what());
				}
			});
		}
		ImGui::SameLine();
		std::string stem = img.name;
		if (ui::toolbar_button(ICON_DOWNLOAD, "Export PNG", ui::Tone::Ghost, true, nullptr, ImGui::GetFrameHeight())) {
			e.file_dialog().open(FileDialog::SaveFile, "Export " + img.name, stem + ".png", {".png"}, [&e, obj](const std::filesystem::path &path) {
				int iw = 0, ih = 0;
				auto px = ed::image_rgba(e.project()->file().objects[size_t(obj)], &iw, &ih);
				if (px.empty()) {
					e.log(LogLine::Error, "The image could not be decoded");
					return;
				}
				ed::write_png(path, iw, ih, px);
				e.log(LogLine::Ok, "Wrote " + path.filename().string());
			});
		}
		ImGui::PopID();
	}
	ImGui::Dummy(ImVec2(0, ui::px(8)));
	for (int m : o.model_descriptions) {
		if (utf8_text(objs[size_t(m)].info.name_text()) == paint_name_) paint_name_ = "Paint" + std::to_string(o.model_descriptions.size() + 1);
	}
	ImGui::SetNextItemWidth(ui::px(220));
	ImGui::InputText("##pname", &paint_name_);
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_PALETTE, "New paint job", ui::Tone::Accent, !paint_name_.empty(), "A copy of the selected paint job's model (several MB)", ImGui::GetFrameHeight())) {
		try {
			ed::create_paint_job(*e.project(), o, paint_name_, job);
			paint_ = int(o.model_descriptions.size());
			e.log(LogLine::Ok, "New paint job " + paint_name_);
		} catch (const std::exception &ex) {
			e.log(LogLine::Error, std::string("Cannot make the paint job: ") + ex.what());
		}
	}
}

void HarDocument::script(Editor &e, const ed::HarObjects &o) {
	ui::dim_text("Numeric finals of the robot's behavior: changing one rewrites that line of its members and recompiles the class into VMC2.",
	             kTextMuted);
	if (ImGui::BeginTable("consts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
	                      ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() - ui::px(20)))) {
		ImGui::TableSetupColumn("Constant", ImGuiTableColumnFlags_WidthStretch, 1.3f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, ui::px(140));
		ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableHeadersRow();
		for (size_t k = 0; k < data_.constants.size(); ++k) {
			auto &c = data_.constants[k];
			ImGui::PushID(int(k));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushFont(fonts.mono, kFontSmall);
			ImGui::TextUnformatted(c.name.c_str());
			ImGui::PopFont();
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1);
			float v = float(c.value);
			if (ImGui::DragFloat("##v", &v, c.type == "float" ? 0.01f : 1.0f, 0, 0, c.type == "float" ? "%.3f" : "%.0f")) c.value = c.type == "float" ? double(v) : std::round(v);
			ImGui::TableNextColumn();
			ui::dim_text(c.comment.c_str(), kTextMuted);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	const auto &errors = ed::last_compile_errors(*e.project());
	if (ui::toolbar_button(ICON_HAMMER, "Compile", ui::Tone::Normal, o.behavior_class >= 0, nullptr, ImGui::GetFrameHeight())) compile_scripts(e);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	int stale = 0;
	for (auto &i : e.issues()) stale += i.rule == 9 && i.severity == ed::Issue::Error && i.object == o.behavior_class;
	if (!errors.empty()) ui::status_chip(ICON_CIRCLE_X, errors.front().str().c_str(), kError);
	else if (stale) ui::status_chip(ICON_TRIANGLE_ALERT, "Changed since the last compile (Save compiles)", kWarning);
	else ui::status_chip(ICON_CIRCLE_CHECK, "VMC2 current", kOk);
}
