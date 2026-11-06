// The pilot editor: a large in-engine preview on the left (the photo, or the robot painted in the
// pilot's colours), the pilot's fields in tabs on the right.
#include <algorithm>
#include <cmath>

#include "editor.h"
#include "icons.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "preview.h"
#include "theme.h"
#include "widgets.h"

using namespace theme;

class PilotDocument : public Document {
public:
	PilotDocument(Editor &e, const ed::Guid &g) : guid_(g) {
		const Options &o = e.options();
		tab_ = std::clamp(o.pilot_tab, 0, 4);
		robot_ = o.preview == "robot" || (o.preview.empty() && tab_ == 3);
		paint_seen_ = tab_ == 3;
	}
	~PilotDocument() override { delete_texture(photo_); }
	std::string title(Editor &e) const override {
		int i = e.find_object(guid_);
		if (i < 0) return "Pilot";
		const auto &n = e.project()->file().objects[size_t(i)].info.name;
		return "Pilot: " + deo::latin1_to_utf8(deo::Bytes(n.begin(), n.end() - (n.empty() ? 0 : 1)));
	}
	const char *icon() const override { return ICON_USER; }
	bool alive(Editor &e) const override { return e.find_object(guid_) >= 0; }
	void draw(Editor &e) override;
	const ed::Guid &guid() const { return guid_; }

private:
	void draw_preview(Editor &e, ImVec2 size);
	void draw_form(Editor &e, ImVec2 size, const ed::PilotObjects &objs);
	void general(Editor &e);
	void attributes();
	void traits(Editor &e);
	void paint();
	void casts_and_flags(Editor &e, const ed::PilotObjects &objs);
	void casts_editor(Editor &e);
	void footer_bar(Editor &e, const ed::PilotObjects &objs);

	ed::Guid guid_;
	ed::PilotData data_, committed_;
	uint64_t read_revision_ = 0;
	int tab_ = 0;
	bool robot_ = false, playing_ = true, paint_seen_ = false;
	RobotPreview preview_;
	std::string fighter_ = "Jaguar", preview_error_;
	int animation_ = -1;
	float anim_t_ = 0.15f, yaw_ = 0.55f;
	GLuint photo_ = 0;
	uint64_t photo_revision_ = 0;
	int photo_w_ = 0, photo_h_ = 0;
	std::string traits_from_ = "Lucas";
	int cast_pick_ = 0;
};

std::unique_ptr<Document> make_pilot_document(Editor &e, const ed::Guid &pilot) { return std::make_unique<PilotDocument>(e, pilot); }

bool pilot_document_is(const Document *d, const ed::Guid &pilot) {
	auto *p = dynamic_cast<const PilotDocument *>(d);
	return p && p->guid() == pilot;
}

namespace {

constexpr float kLabelWidth = 180;
const char *const kSexes[3] = {"Male", "Female", "Undefined"};

}  // namespace

void PilotDocument::draw(Editor &e) {
	ed::Project &p = *e.project();
	int idx = p.find(guid_);
	if (idx < 0) return;
	ed::PilotObjects objs = ed::pilot_objects(p, uint32_t(idx));
	if (p.revision() != read_revision_) {
		try {
			data_ = committed_ = ed::read_pilot(p, objs);
		} catch (const std::exception &ex) {
			e.log(LogLine::Error, std::string("Cannot read the pilot: ") + ex.what());
		}
		read_revision_ = p.revision();
	}
	ImGui::PushID(this);
	const float pad = ui::px(14);
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::SetCursorPos(ImVec2(pad, pad));
	float h = avail.y - pad * 2;
	float left = std::clamp(avail.x * 0.33f, ui::px(320), ui::px(470));
	draw_preview(e, ImVec2(left, h));
	ImGui::SameLine(0, pad);
	draw_form(e, ImVec2(avail.x - left - pad * 3, h), objs);
	ImGui::PopID();
	if (!(data_ == committed_)) {
		try {
			ed::write_pilot(p, objs, data_);
		} catch (const std::exception &ex) {
			e.log(LogLine::Error, std::string("Cannot write the pilot: ") + ex.what());
		}
		committed_ = data_;
		read_revision_ = p.revision();
	}
}

void PilotDocument::draw_preview(Editor &e, ImVec2 size) {
	ui::begin_panel("preview", size, kPanel, true, 8);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float footer = ui::px(56);
	ImVec2 p0 = ImGui::GetWindowPos(), p1 = p0 + ImVec2(size.x, size.y - footer);
	const float r = ui::px(8);
	dl->AddRectFilledMultiColor(p0 + ImVec2(1, 1), p1 - ImVec2(1, 0), rgb(0x2b2742), rgb(0x2b2742), rgb(0x15161c), rgb(0x15161c));
	for (int k = 6; k >= 1; --k) {
		float rad = std::min(size.x, size.y) * 0.12f * float(k);
		dl->AddCircleFilled(ImVec2((p0.x + p1.x) * 0.5f, p0.y + (p1.y - p0.y) * 0.36f), rad, rgb(0x3a3352, 14), 64);
	}
	ed::Project &p = *e.project();
	if (!robot_) {
		if (photo_revision_ != p.revision()) {
			int idx = p.find(guid_);
			auto px = idx >= 0 ? ed::photo_rgba(p, ed::pilot_objects(p, uint32_t(idx)), &photo_w_, &photo_h_) : std::vector<uint8_t>{};
			if (!px.empty()) photo_ = upload_texture(photo_w_, photo_h_, px, photo_);
			photo_revision_ = p.revision();
		}
		if (photo_ && photo_w_ > 0) {
			// Cover the area, cropping the photo's sides.
			float aw = p1.x - p0.x, ah = p1.y - p0.y;
			float img_aspect = float(photo_w_) / float(photo_h_), area_aspect = aw / ah;
			ImVec2 uv0(0, 0), uv1(1, 1);
			if (img_aspect > area_aspect) {
				float u = area_aspect / img_aspect;
				uv0.x = (1 - u) * 0.5f, uv1.x = 1 - uv0.x;
			} else {
				float v = img_aspect / area_aspect;
				uv0.y = (1 - v) * 0.5f, uv1.y = 1 - uv0.y;
			}
			dl->AddImageRounded((ImTextureID)(intptr_t)photo_, p0 + ImVec2(1, 1), p1 - ImVec2(1, 0), uv0, uv1, IM_COL32_WHITE, r - 1,
			                    ImDrawFlags_RoundCornersTop);
		} else {
			ImGui::SetCursorScreenPos(p0);
			ImGui::BeginChild("nophoto", p1 - p0, 0, ImGuiWindowFlags_NoBackground);
			ui::centered_note(ICON_IMAGE, "No photo", "Import a PNG or BMP from the General tab.");
			ImGui::EndChild();
		}
	} else {
		if (preview_.fighter() != fighter_) {
			if (!preview_.load(e.base()->dir(), fighter_, &preview_error_)) e.log(LogLine::Error, "Preview: " + preview_error_);
			animation_ = 0;
			for (size_t k = 0; k < preview_.animations().size(); ++k) {
				if (preview_.animations()[k] == "Standing") animation_ = int(k);
			}
		}
		if (playing_) {
			anim_t_ += ImGui::GetIO().DeltaTime * 0.8f;
			if (anim_t_ > 1) anim_t_ -= 1;
		}
		ImVec2 area = p1 - p0;
		float scale = ImGui::GetIO().DisplayFramebufferScale.x;
		std::array<std::array<float, 3>, 3> colours;
		for (int k = 0; k < 3; ++k) colours[size_t(k)] = {data_.colours[size_t(k)][0], data_.colours[size_t(k)][1], data_.colours[size_t(k)][2]};
		GLuint tex = preview_.render(int(area.x * scale), int(area.y * scale), colours, animation_, anim_t_, yaw_);
		if (tex) dl->AddImage((ImTextureID)(intptr_t)tex, p0, p1, ImVec2(0, 1), ImVec2(1, 0));
		ImGui::SetCursorScreenPos(p0);
		ImGui::InvisibleButton("orbit", area);
		if (ImGui::IsItemActive()) yaw_ += ImGui::GetIO().MouseDelta.x * 0.01f;
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Drag to turn the robot");
		ImGui::PushFont(nullptr, kFontSmall);
		dl->AddText(p0 + ImVec2(ui::px(12), ui::px(10)), rgb(kTextDim), (fighter_ + " in this pilot's paint").c_str());
		ImGui::PopFont();
	}
	// Footer.
	ImVec2 f0(p0.x, p1.y);
	dl->AddRectFilled(f0, p0 + size, rgb(kPanelHead), r, ImDrawFlags_RoundCornersBottom);
	dl->AddLine(f0, ImVec2(p0.x + size.x, f0.y), rgb(kBorder));
	ImGui::SetCursorScreenPos(f0 + ImVec2(ui::px(14), (footer - ImGui::GetFrameHeight()) * 0.5f));
	ImGui::AlignTextToFramePadding();
	ImGui::PushFont(fonts.semibold, kFontUi);
	ImGui::TextUnformatted("Preview (in-engine)");
	ImGui::PopFont();
	const float combo_w = ui::px(150), fh = ImGui::GetFrameHeight(), gap = ui::px(6);
	float x = p0.x + size.x - ui::px(14) - combo_w - fh - gap - fh * 2 - gap * 2;
	ImGui::SameLine();
	ImGui::SetCursorScreenPos(ImVec2(x, f0.y + (footer - fh) * 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
	if (ui::icon_button("photo", ICON_IMAGE, "Show the photo")) robot_ = false;
	ImGui::SameLine();
	if (ui::icon_button("robot", ICON_BOT, "Show the robot in this pilot's paint, drawn by the game's fighter shader")) robot_ = true;
	ImGui::SameLine();
	if (ui::icon_button("play", playing_ && robot_ ? ICON_PAUSE : ICON_PLAY, playing_ ? "Pause" : "Play", robot_)) playing_ = !playing_;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(combo_w);
	if (!robot_) {
		const char *modes[] = {"Photo", "Robot"};
		int m = 0;
		if (ui::combo("##mode", &m, modes, 2)) robot_ = m == 1;
	} else {
		const auto &anims = preview_.animations();
		std::string current = animation_ >= 0 && size_t(animation_) < anims.size() ? anims[size_t(animation_)] : "Idle";
		if (current == "Standing") current = "Idle";
		if (ui::begin_combo("##anim", current.c_str(), ImGuiComboFlags_HeightLarge)) {
			ImGui::SeparatorText("Robot");
			for (auto &f : e.fighter_names()) {
				if (ImGui::Selectable((std::string(ICON_BOT "  ") + f).c_str(), f == fighter_)) fighter_ = f;
			}
			ImGui::SeparatorText("Animation");
			for (size_t k = 0; k < anims.size(); ++k) {
				std::string label = anims[k] == "Standing" ? "Idle (Standing)" : anims[k];
				if (ImGui::Selectable(label.c_str(), int(k) == animation_)) animation_ = int(k), anim_t_ = 0;
			}
			ImGui::EndCombo();
		}
	}
	ImGui::PopStyleVar();
	ui::end_panel();
}

void PilotDocument::draw_form(Editor &e, ImVec2 size, const ed::PilotObjects &objs) {
	ui::begin_panel("form", size, kPanel, true, 8, ImVec2(ui::px(20), 0));
	ImGui::Dummy(ImVec2(0, ui::px(14)));
	ImGui::PushFont(fonts.semibold, kFontHeading);
	ImGui::TextUnformatted(("Pilot: " + data_.name).c_str());
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	int before = tab_;
	ui::tab_strip("tabs", {"General", "Attributes", "AI Traits (31)", "Paint", "Casts & Flags"}, &tab_);
	if (tab_ == 3 && before != 3 && !paint_seen_) robot_ = true, paint_seen_ = true;
	const float footer = ui::px(48);
	ImGui::BeginChild("body", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), 0, 0);
	ImGui::Dummy(ImVec2(0, ui::px(16)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(16)));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(11), ui::px(9)));
	switch (tab_) {
		case 0: general(e); break;
		case 1: attributes(); break;
		case 2: traits(e); break;
		case 3: paint(); break;
		default: casts_and_flags(e, objs); break;
	}
	ImGui::PopStyleVar(2);
	ImGui::EndChild();
	footer_bar(e, objs);
	ui::end_panel();
}

void PilotDocument::general(Editor &e) {
	const float lw = ui::px(kLabelWidth);
	ui::form_label("Name", lw);
	ImGui::SetNextItemWidth(-ui::px(4));
	ImGui::InputText("##name", &data_.name);

	ui::form_label("Sex", lw);
	ImGui::SetNextItemWidth(ui::px(260));
	int sex = int(std::min<uint32_t>(data_.sex, 2));
	if (ui::combo("##sex", &sex, kSexes, 3)) data_.sex = uint32_t(sex);

	ui::form_label("Photo", lw);
	const float fh = ImGui::GetFrameHeight();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (fh + ui::px(8)) * 2 - ui::px(4));
	std::string photo = data_.photo_source.empty() ? "(no file recorded)" : data_.photo_source;
	ImGui::InputText("##photo", &photo, ImGuiInputTextFlags_ReadOnly);
	ImGui::SameLine();
	if (ui::icon_button("browse", ICON_FOLDER_OPEN, "Import a PNG or BMP (cropped to a square, stored 256 x 256)")) e.import_photo_dialog(guid_);
	ImGui::SameLine();
	if (ui::icon_button("more", ICON_ELLIPSIS, "More")) ImGui::OpenPopup("photo_more");
	if (ImGui::BeginPopup("photo_more")) {
		if (ImGui::MenuItem(ICON_UPLOAD "  Import PNG or BMP...")) e.import_photo_dialog(guid_);
		if (ImGui::MenuItem(ICON_BOT "  Show robot preview")) robot_ = true;
		if (ImGui::MenuItem(ICON_IMAGE "  Show photo preview")) robot_ = false;
		ImGui::EndPopup();
	}

	ui::form_label("Bio", lw);
	ImGui::InputTextMultiline("##bio", &data_.bio, ImVec2(-ui::px(4), ui::px(132)), ImGuiInputTextFlags_WordWrap);

	ui::form_label("Listed", lw, ImGui::GetFrameHeight());
	ui::checkbox("##listed", &data_.listed);
	ui::form_label("Locked", lw, ImGui::GetFrameHeight());
	ui::checkbox("##locked", &data_.locked);
	casts_editor(e);
}

void PilotDocument::casts_editor(Editor &e) {
	const float lw = ui::px(kLabelWidth);
	const float fh = ImGui::GetFrameHeight();
	auto choices = e.cast_choices();
	ImGui::PushID("casts");
	for (size_t k = 0; k < data_.casts.size(); ++k) {
		ImGui::PushID(int(k));
		ui::form_label(k == 0 ? "Casts" : "", lw);
		std::string name = ed::object_name(*e.project(), data_.casts[k]);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fh - ui::px(12));
		ImGui::InputText("##cast", &name, ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ui::icon_button("remove", ICON_X, "Remove from this cast")) {
			data_.casts.erase(data_.casts.begin() + long(k));
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}
	ui::form_label(data_.casts.empty() ? "Casts" : "", lw);
	std::vector<std::pair<std::string, ed::Guid>> free;
	for (auto &c : choices) {
		if (std::find(data_.casts.begin(), data_.casts.end(), c.second) == data_.casts.end()) free.push_back(c);
	}
	cast_pick_ = std::clamp(cast_pick_, 0, std::max(0, int(free.size()) - 1));
	for (size_t k = 0; k < free.size(); ++k) {
		if (free[k].first == "PilotSPLadder" || free[k].first == "MultiplayerPilot") {
			if (cast_pick_ == 0 && k) cast_pick_ = int(k);
			break;
		}
	}
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fh - ui::px(12));
	if (ui::begin_combo("##add", free.empty() ? "(in every cast)" : free[size_t(cast_pick_)].first.c_str(), ImGuiComboFlags_HeightLarge)) {
		for (size_t k = 0; k < free.size(); ++k) {
			if (ImGui::Selectable(free[k].first.c_str(), int(k) == cast_pick_)) cast_pick_ = int(k);
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ui::icon_button("add", ICON_PLUS, "Add to this cast (PilotSPLadder: single player; MultiplayerPilot: multiplayer)", !free.empty())) {
		data_.casts.push_back(free[size_t(cast_pick_)].second);
	}
	ImGui::PopID();
}

void PilotDocument::attributes() {
	const float lw = ui::px(kLabelWidth);
	ui::dim_text("Choose Pilot shows these four as bars beside the photo (0-100).");
	const char *names[4] = {"Power", "Agility", "Endurance", "Focus"};
	for (int k = 0; k < 4; ++k) {
		ImGui::PushID(k);
		ui::form_label(names[k], lw);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui::px(4));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, vec(0x1e1f28));
		ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, ui::px(14));
		float v = data_.stats[size_t(k)];
		ImVec2 p = ImGui::GetCursorScreenPos();
		float w = ImGui::CalcItemWidth();
		ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w * std::clamp(v / 100.0f, 0.0f, 1.0f), ImGui::GetFrameHeight()),
		                                          rgb(kSelection), ui::px(5));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(kInput, 0.0f));
		if (ImGui::SliderFloat("##v", &v, 0, 100, "%.0f")) data_.stats[size_t(k)] = std::round(v);
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
		ImGui::PopID();
	}
}

void PilotDocument::traits(Editor &e) {
	ImGui::AlignTextToFramePadding();
	ui::dim_text("Each trait has a level (0-8), a variation (0-5) and a wave (0-3); RECON DATA shows the level words.");
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Preset from");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ui::px(220));
	if (ui::begin_combo("##preset", traits_from_.c_str(), ImGuiComboFlags_HeightLarge)) {
		for (auto &n : ed::base_pilot_names(*e.base())) {
			if (ImGui::Selectable(n.c_str(), n == traits_from_)) traits_from_ = n;
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_COPY, "Apply traits", ui::Tone::Normal, true, "Copy the 31 traits from that pilot", ImGui::GetFrameHeight())) {
		try {
			ed::Project scratch(*e.base(), "", ed::Project::create(*e.base(), "scratch.DEObjects")->file());
			uint32_t made = ed::create_pilot(scratch, traits_from_, "scratch");
			data_.traits = ed::read_pilot(scratch, ed::pilot_objects(scratch, made)).traits;
			e.log(LogLine::Info, "AI traits copied from " + traits_from_);
		} catch (const std::exception &ex) {
			e.log(LogLine::Error, ex.what());
		}
	}
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(8), ui::px(4)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(4)));
	ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, ui::px(5));
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, vec(kAccent, 0.9f));
	if (ImGui::BeginTable("traits", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) {
		ImGui::TableSetupColumn("Trait", ImGuiTableColumnFlags_WidthStretch, 1.3f);
		ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn("Variation", ImGuiTableColumnFlags_WidthStretch, 0.8f);
		ImGui::TableSetupColumn("Wave", ImGuiTableColumnFlags_WidthStretch, 0.6f);
		ImGui::TableHeadersRow();
		const auto &info = ed::trait_infos();
		for (const char *group : {"Affinity", "Perception", "Emotion"}) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushFont(fonts.semibold, kFontSmall);
			ui::dim_text(group, kAccentText);
			ImGui::PopFont();
			for (size_t t = 0; t < info.size(); ++t) {
				if (std::string(info[t].group) != group) continue;
				ed::PilotTrait &v = data_.traits[t];
				ImGui::PushID(int(t));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(info[t].label);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("GetStat%s()", info[t].name);
				ImGui::TableNextColumn();
				int level = v.level;
				ImGui::SetNextItemWidth(-1);
				if (ImGui::SliderInt("##l", &level, 0, 8, ed::kTraitLevels[std::clamp(level, 0, 8)])) v.level = uint8_t(level);
				ImGui::TableNextColumn();
				int var = v.variation;
				ImGui::SetNextItemWidth(-1);
				if (ImGui::SliderInt("##v", &var, 0, 5)) v.variation = uint8_t(var);
				ImGui::TableNextColumn();
				int wave = v.wave;
				ImGui::SetNextItemWidth(-1);
				if (ImGui::SliderInt("##w", &wave, 0, 3)) v.wave = uint8_t(wave);
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}

void PilotDocument::paint() {
	const float lw = ui::px(kLabelWidth);
	ui::dim_text("The robot's paint: each colour fills one channel of the fighter's paint mask (the game's OMF2Fighter image filter).");
	const char *names[3] = {"Colour 1 (mask red)", "Colour 2 (mask green)", "Colour 3 (mask blue)"};
	for (int k = 0; k < 3; ++k) {
		ImGui::PushID(k);
		ui::form_label(names[k], lw);
		auto &c = data_.colours[size_t(k)];
		float rgbv[3] = {c[0], c[1], c[2]};
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui::px(190));
		if (ImGui::ColorEdit3("##c", rgbv, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB)) c[0] = rgbv[0], c[1] = rgbv[1], c[2] = rgbv[2];
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ui::dim_text("Weight", kTextDim);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(110));
		ImGui::DragFloat("##w", &data_.weights[size_t(k)], 0.01f, 0, 10, "%.2f");
		ImGui::PopID();
	}
	ui::form_label("Swatches", lw, ui::px(56));
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = (ImGui::GetContentRegionAvail().x - ui::px(20)) / 3;
	for (int k = 0; k < 3; ++k) {
		auto &c = data_.colours[size_t(k)];
		ImVec2 a = p + ImVec2(float(k) * (w + ui::px(10)), 0);
		ImGui::GetWindowDrawList()->AddRectFilled(a, a + ImVec2(w, ui::px(56)), ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], 1)), ui::px(6));
		ImGui::GetWindowDrawList()->AddRect(a, a + ImVec2(w, ui::px(56)), rgb(kBorder), ui::px(6));
	}
	ImGui::Dummy(ImVec2(0, ui::px(56)));
}

void PilotDocument::casts_and_flags(Editor &e, const ed::PilotObjects &objs) {
	const float lw = ui::px(kLabelWidth);
	ui::form_label("Listed", lw);
	ui::checkbox("##listed", &data_.listed);
	ImGui::SameLine(0, ui::px(12));
	ImGui::AlignTextToFramePadding();
	ui::dim_text("shown in Choose Pilot");
	ui::form_label("Locked by default", lw);
	ui::checkbox("##locked", &data_.locked);
	ImGui::SameLine(0, ui::px(12));
	ImGui::AlignTextToFramePadding();
	ui::dim_text("needs an unlock (locks are kept per GUID)");
	ui::form_label("Visible when locked", lw);
	ui::checkbox("##visible", &data_.visible_when_locked);
	casts_editor(e);
	ImGui::Dummy(ImVec2(0, ui::px(4)));
	ImGui::SeparatorText("Objects in the add-on");
	ed::Project &p = *e.project();
	auto row = [&](const char *label, int index) {
		ui::form_label(label, lw);
		std::string v = index >= 0 ? p.file().objects[size_t(index)].info.guid.str() + "   #" + std::to_string(index) : "(none)";
		ImGui::SetNextItemWidth(-ui::px(4));
		ImGui::PushFont(fonts.mono, kFontSmall);
		ImGui::InputText((std::string("##") + label).c_str(), &v, ImGuiInputTextFlags_ReadOnly);
		ImGui::PopFont();
	};
	row("Pilot", objs.pilot);
	row("Description", objs.description);
	row("Bio value", objs.bio);
	row("Photo", objs.photo);
}

void PilotDocument::footer_bar(Editor &e, const ed::PilotObjects &objs) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = ImGui::GetWindowWidth();
	dl->AddLine(ImVec2(ImGui::GetWindowPos().x, p.y), ImVec2(ImGui::GetWindowPos().x + w, p.y), rgb(kBorder));
	ImGui::SetCursorScreenPos(p + ImVec2(0, (ui::px(48) - ImGui::GetFontSize()) * 0.5f));
	ImGui::PushFont(nullptr, kFontSmall);
	bool guids_ok = false;
	std::vector<const ed::Issue *> mine;
	for (auto &i : e.issues()) {
		if (i.rule == 4 && i.severity == ed::Issue::Ok) guids_ok = true;
		if (i.severity != ed::Issue::Ok && i.object >= 0 &&
		    (i.object == objs.pilot || i.object == objs.description || i.object == objs.photo || i.object == objs.bio)) {
			mine.push_back(&i);
		}
	}
	if (guids_ok) ui::status_chip(ICON_CIRCLE_CHECK, "GUIDs unique", kOk);
	else ui::status_chip(ICON_CIRCLE_X, "GUID problem", kError);
	ImGui::SameLine(0, ui::px(18));
	if (mine.empty()) ui::status_chip(ICON_CIRCLE_CHECK, "Listed, unlocked, in a pilot cast", kOk);
	for (size_t k = 0; k < mine.size() && k < 2; ++k) {
		if (k) ImGui::SameLine(0, ui::px(18));
		ui::status_chip(ICON_TRIANGLE_ALERT, mine[k]->text.c_str(), mine[k]->severity == ed::Issue::Error ? kError : kWarning);
	}
	std::string src = data_.photo_source;
	std::string ext = src.size() > 4 ? src.substr(src.size() - 4) : "";
	for (char &c : ext) c = char(tolower(uint8_t(c)));
	if (ext == ".tga") {
		ImGui::SameLine(0, ui::px(18));
		ui::status_chip(ICON_TRIANGLE_ALERT, "Photo not imported (still the copied pilot's)", kWarning);
	}
	char stats[96];
	snprintf(stats, sizeof stats, "Attributes %.0f / %.0f / %.0f / %.0f", data_.stats[0], data_.stats[1], data_.stats[2], data_.stats[3]);
	ImGui::SameLine(0, ui::px(18));
	ui::dim_text(stats, kTextDim);
	ImGui::PopFont();
}
