// The tournament editor: the session master's tree (settings, matches, branches, unlocks, ending)
// on the left, the selected item's form in the middle, and the generated scripts and the quotes
// on the right.
#include <algorithm>
#include <cmath>

#include "assets.h"
#include "code_view.h"
#include "content.h"
#include "icons.h"
#include "image_io.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "pilot.h"
#include "theme.h"
#include "tournament.h"
#include "widgets.h"

using namespace theme;

namespace {

constexpr int kSettings = -1, kEnding = -2, kBranches = -3, kUnlocks = -4;
const char *const kSpeakerLabels[] = {"Player (slot 0)", "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5"};

using Choices = std::vector<std::pair<std::string, ed::Guid>>;

std::string to_edit(const std::string &s) { return s; }

bool guid_combo(const char *id, ed::Guid *g, const Choices &choices, const char *none_label, float width = -1) {
	std::string preview = none_label ? none_label : "(none)";
	for (auto &c : choices) {
		if (c.second == *g) preview = c.first;
	}
	if (!ed::guid_zero(*g) && preview == (none_label ? none_label : "(none)")) preview = "(" + g->str() + ")";
	bool changed = false;
	ImGui::SetNextItemWidth(width);
	if (ui::begin_combo(id, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
		if (none_label && ImGui::Selectable(none_label, ed::guid_zero(*g))) *g = {}, changed = true;
		for (auto &c : choices) {
			if (ImGui::Selectable(c.first.c_str(), c.second == *g)) *g = c.second, changed = true;
		}
		ImGui::EndCombo();
	}
	return changed;
}

}  // namespace

class TournamentDocument : public Document {
public:
	TournamentDocument(Editor &e, const ed::Guid &g) : guid_(g) {
		if (e.options().asset_tab >= 0) tab_ = std::clamp(e.options().asset_tab, 0, 2);
		if (e.options().asset_select >= -4) sel_ = e.options().asset_select;
		script_compiler(e).preload();
	}
	std::string title(Editor &e) const override {
		int i = e.find_object(guid_);
		if (i < 0) return "Tournament";
		return "Tournament: " + utf8_text(e.project()->file().objects[size_t(i)].info.name_text());
	}
	const char *icon() const override { return ICON_TROPHY; }
	bool alive(Editor &e) const override { return e.find_object(guid_) >= 0; }
	bool addon_content() const override { return true; }
	bool shows(const ObjectRef &r) const override { return r.kind == ObjectRef::Addon && r.guid == guid_; }
	void draw(Editor &e) override;
	bool jump(Editor &e, int behavior, int line) {
		int i = e.find_object(guid_);
		if (i < 0) return false;
		ed::TournamentObjects o = ed::tournament_objects(*e.project(), uint32_t(i));
		if (behavior == o.bsub) tab_ = 0, sub_view_.jump_line = line;
		else if (behavior == o.bmaster) tab_ = 1, master_view_.jump_line = line;
		else return false;
		return true;
	}

private:
	void tree(Editor &e, ImVec2 size);
	void form(Editor &e, ImVec2 size, const ed::TournamentObjects &o);
	void settings_form(Editor &e, const ed::TournamentObjects &o);
	void match_form(Editor &e, int m);
	void ending_form(Editor &e);
	void branches_form();
	void unlocks_form(Editor &e);
	void quotes_list(std::vector<ed::TQuote> &qs, const char *id);
	void scripts(Editor &e, ImVec2 size, const ed::TournamentObjects &o);
	void footer(Editor &e, const ed::TournamentObjects &o);
	void guid_list(const char *label, std::vector<ed::Guid> &list, const Choices &choices);
	void import_image(Editor &e, int image, bool icon);
	const Choices &choices(Editor &e, const std::string &cls);

	ed::Guid guid_;
	ed::Tournament data_, committed_;
	uint64_t read_revision_ = 0;
	int sel_ = 0;
	int tab_ = 0;
	CodeView sub_view_, master_view_;
	std::map<std::string, Choices> choice_cache_;
	uint64_t choice_revision_ = 0;
	bool compile_after_ = false;
};

std::unique_ptr<Document> make_tournament_document(Editor &e, const ed::Guid &description) {
	return std::make_unique<TournamentDocument>(e, description);
}

bool tournament_document_jump(Document *d, int behavior, int line, Editor &e) {
	auto *t = dynamic_cast<TournamentDocument *>(d);
	return t && t->jump(e, behavior, line);
}

const Choices &TournamentDocument::choices(Editor &e, const std::string &cls) {
	if (choice_revision_ != e.project()->revision() / 64) choice_cache_.clear(), choice_revision_ = e.project()->revision() / 64;
	auto it = choice_cache_.find(cls);
	if (it != choice_cache_.end()) return it->second;
	Choices c;
	if (cls == "DropOn") {
		c = ed::drop_on_choices(*e.base());
	} else if (cls == "GameType" || cls == "ScoreLimit") {
		for (auto *b : e.base()->of_class(cls == "GameType" ? "Description\\SessionMaster" : "Description\\SessionModifier")) {
			if (e.base()->files()[b->file].stem != "Session") continue;
			bool limit = b->name.find("Score Limit") != std::string::npos || b->name.find("Win") != std::string::npos;
			if (cls == "ScoreLimit" && !limit) continue;
			if (cls == "GameType" && b->name == "Match Server") continue;
			c.emplace_back(b->name, b->guid);
		}
	} else {
		c = ed::class_choices(*e.project(), cls);
	}
	return choice_cache_.emplace(cls, std::move(c)).first->second;
}

void TournamentDocument::draw(Editor &e) {
	ed::Project *p = e.project();
	if (!p) return;
	int idx = p->find(guid_);
	if (idx < 0) return;
	ed::TournamentObjects o = ed::tournament_objects(*p, uint32_t(idx));
	if (p->revision() != read_revision_) {
		data_ = committed_ = ed::read_tournament(*p, o);
		read_revision_ = p->revision();
	}
	ImGui::PushID(this);
	const float pad = ui::px(14);
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::SetCursorPos(ImVec2(pad, pad));
	float h = avail.y - pad * 2;
	float left = std::clamp(avail.x * 0.2f, ui::px(230), ui::px(300));
	float right = std::clamp(avail.x * 0.38f, ui::px(380), ui::px(760));
	tree(e, ImVec2(left, h));
	ImGui::SameLine(0, pad);
	form(e, ImVec2(avail.x - left - right - pad * 4, h), o);
	ImGui::SameLine(0, pad);
	scripts(e, ImVec2(right, h), o);
	ImGui::PopID();
	if (!(data_ == committed_)) {
		try {
			ed::write_tournament(*p, o, data_);
		} catch (const std::exception &ex) {
			e.log(LogLine::Error, std::string("Cannot write the tournament: ") + ex.what());
		}
		committed_ = data_;
		read_revision_ = p->revision();
	}
	if (compile_after_) {
		compile_after_ = false;
		compile_scripts(e);
	}
}

void TournamentDocument::tree(Editor &e, ImVec2 size) {
	ui::begin_panel("tree", size, kPanel, true, 8, ImVec2(ui::px(8), ui::px(8)));
	ImGui::PushFont(fonts.semibold, kFontSmall);
	ui::dim_text("SESSION MASTER", kTextMuted);
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	static bool matches_open = true;
	ui::TreeRow root;
	root.icon = ICON_TROPHY;
	root.icon_colour = kWarningStrong;
	root.label = data_.name;
	root.selected = sel_ == kSettings;
	if (ui::tree_row("root", root)) sel_ = kSettings;
	ui::TreeRow mr;
	mr.icon = ICON_LIST;
	mr.label = "Matches";
	mr.count = int(data_.matches.size());
	mr.depth = 1;
	mr.has_children = true;
	mr.open = &matches_open;
	ui::tree_row("matches", mr);
	if (matches_open) {
		for (size_t k = 0; k < data_.matches.size(); ++k) {
			ui::TreeRow r;
			r.icon = ICON_SWORDS;
			r.label = "M" + std::to_string(k + 1) + "  " + (data_.matches[k].name.empty() ? data_.matches[k].key : data_.matches[k].name);
			r.depth = 2;
			r.selected = sel_ == int(k);
			if (ui::tree_row(("m" + std::to_string(k)).c_str(), r)) sel_ = int(k);
		}
	}
	auto row = [&](const char *id, const char *icon, const char *label, int which, int count = -1) {
		ui::TreeRow r;
		r.icon = icon;
		r.label = label;
		r.depth = 1;
		r.count = count;
		r.selected = sel_ == which;
		if (ui::tree_row(id, r)) sel_ = which;
	};
	int branches = 0;
	for (auto &m : data_.matches) branches += (m.on_win != ed::kNextMatch) + (m.on_lose != ed::kReplay);
	row("branches", ICON_ROUTE, "Branches", kBranches, branches);
	row("unlocks", ICON_LOCK_OPEN, "Unlocks", kUnlocks, int(data_.unlocks.size()));
	row("ending", ICON_VIDEO, "Ending cinematic", kEnding);
	ImGui::Dummy(ImVec2(0, ui::px(10)));
	const float fh = ImGui::GetFrameHeight();
	if (ui::toolbar_button(ICON_PLUS, "Match", ui::Tone::Normal, true, "Add a match after the selected one", fh)) {
		ed::TMatch m;
		int n = int(data_.matches.size()) + 1;
		std::string key;
		for (;; ++n) {
			key = "Match" + std::to_string(n);
			if (std::none_of(data_.matches.begin(), data_.matches.end(), [&](const ed::TMatch &x) { return x.key == key; })) break;
		}
		m.key = key;
		m.name = "Match " + std::to_string(data_.matches.size() + 1);
		m.description = "<Slot0Name> faces <Slot1Name> in <EnvironmentName>.";
		m.opponents.resize(1);
		size_t at = sel_ >= 0 ? size_t(sel_) + 1 : data_.matches.size();
		data_.matches.insert(data_.matches.begin() + long(at), m);
		for (auto &x : data_.matches) {
			for (int *t : {&x.on_win, &x.on_lose}) {
				if (*t >= int(at) && *t < ed::kEnding) ++*t;
			}
		}
		sel_ = int(at);
	}
	ImGui::SameLine();
	bool have = sel_ >= 0 && sel_ < int(data_.matches.size());
	if (ui::icon_button("up", ICON_CHEVRON_RIGHT, "Move the match up", have && sel_ > 0)) {
		std::swap(data_.matches[size_t(sel_)], data_.matches[size_t(sel_ - 1)]);
		--sel_;
	}
	ImGui::SameLine();
	if (ui::icon_button("del", ICON_TRASH_2, "Remove the match", have && data_.matches.size() > 1)) {
		int gone = sel_;
		std::string key = "Match" + data_.matches[size_t(gone)].key;
		data_.matches.erase(data_.matches.begin() + gone);
		data_.extra.erase(std::remove_if(data_.extra.begin(), data_.extra.end(), [&](const ed::TMethod &m) { return m.name.rfind(key + ".", 0) == 0; }),
		                  data_.extra.end());
		for (auto &x : data_.matches) {
			for (int *t : {&x.on_win, &x.on_lose}) {
				if (*t == gone) *t = -1;
				else if (*t > gone && *t < ed::kEnding) --*t;
			}
		}
		sel_ = std::min(sel_, int(data_.matches.size()) - 1);
	}
	ui::end_panel();
	(void)e;
}

void TournamentDocument::form(Editor &e, ImVec2 size, const ed::TournamentObjects &o) {
	ui::begin_panel("form", size, kPanel, true, 8, ImVec2(ui::px(18), 0));
	ImGui::Dummy(ImVec2(0, ui::px(12)));
	ImGui::PushFont(fonts.semibold, kFontHeading - 4);
	std::string head = sel_ >= 0 && sel_ < int(data_.matches.size())
	                       ? "Match: M" + std::to_string(sel_ + 1) + "  " + data_.matches[size_t(sel_)].name
	                   : sel_ == kEnding   ? std::string("Ending cinematic")
	                   : sel_ == kBranches ? std::string("Branches")
	                   : sel_ == kUnlocks  ? std::string("Unlocks on completion")
	                                       : "Session Master: " + data_.name;
	ImGui::TextUnformatted(head.c_str());
	ImGui::PopFont();
	const float footer_h = ui::px(48);
	ImGui::BeginChild("body", ImVec2(0, ImGui::GetContentRegionAvail().y - footer_h), 0, 0);
	ImGui::Dummy(ImVec2(0, ui::px(8)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(10)));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(10), ui::px(7)));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(kInput));
	if (sel_ >= 0 && sel_ < int(data_.matches.size())) match_form(e, sel_);
	else if (sel_ == kEnding) ending_form(e);
	else if (sel_ == kBranches) branches_form();
	else if (sel_ == kUnlocks) unlocks_form(e);
	else settings_form(e, o);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
	ImGui::EndChild();
	footer(e, o);
	ui::end_panel();
}

void TournamentDocument::guid_list(const char *label, std::vector<ed::Guid> &list, const Choices &cs) {
	const float lw = ui::px(170), fh = ImGui::GetFrameHeight();
	ImGui::PushID(label);
	for (size_t k = 0; k < list.size(); ++k) {
		ImGui::PushID(int(k));
		ui::form_label(k == 0 ? label : "", lw);
		guid_combo("##g", &list[k], cs, nullptr, ImGui::GetContentRegionAvail().x - fh - ui::px(8));
		ImGui::SameLine();
		if (ui::icon_button("x", ICON_X, "Remove")) {
			list.erase(list.begin() + long(k));
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}
	ui::form_label(list.empty() ? label : "", lw);
	if (ui::toolbar_button(ICON_PLUS, "Add", ui::Tone::Ghost, !cs.empty(), nullptr, fh)) list.push_back(cs.front().second);
	ImGui::PopID();
}

void TournamentDocument::settings_form(Editor &e, const ed::TournamentObjects &o) {
	const float lw = ui::px(170);
	ui::form_label("Name", lw);
	ImGui::SetNextItemWidth(-ui::px(4));
	ImGui::InputText("##name", &data_.name);
	ui::form_label("Choose Match text", lw);
	ImGui::InputTextMultiline("##text", &data_.choose_text, ImVec2(-ui::px(4), ui::px(96)), ImGuiInputTextFlags_WordWrap);
	ui::form_label("Choose Match image", lw);
	{
		int w = 0, h = 0, type = 0;
		if (o.image >= 0) ed::image_header(e.project()->file().objects[size_t(o.image)], &type, &w, &h);
		std::string label = o.image < 0 ? "(none)" : (data_.image_source.empty() ? "copied image" : data_.image_source) + "  " + std::to_string(w) + " x " + std::to_string(h);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ui::px(12));
		ImGui::InputText("##img", &label, ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ui::icon_button("imgb", ICON_FOLDER_OPEN, "Import a PNG or BMP (scaled to the image's size)", o.image >= 0)) import_image(e, o.image, false);
	}
	ui::form_label("List icon", lw);
	{
		std::string label = o.icon < 0 ? "(none)" : data_.icon_source.empty() ? "copied image" : data_.icon_source;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ui::px(12));
		ImGui::InputText("##icon", &label, ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ui::icon_button("iconb", ICON_FOLDER_OPEN, "Import a PNG or BMP", o.icon >= 0)) import_image(e, o.icon, true);
	}
	ui::form_label("Game type", lw);
	guid_combo("##type", &data_.game_type, choices(e, "GameType"), nullptr);
	ui::form_label("Score limit", lw);
	guid_combo("##limit", &data_.score_limit, choices(e, "ScoreLimit"), nullptr);
	ui::form_label("AI difficulty offset", lw);
	ImGui::SetNextItemWidth(ui::px(160));
	ImGui::DragFloat("##ai", &data_.ai_difficulty, 0.1f, -3, 5, "%+.1f");
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ui::dim_text("added to each opponent's difficulty (SetAIDifficultyOffset)", kTextMuted);
	ui::form_label("Listed", lw, ImGui::GetFrameHeight());
	ui::checkbox("##listed", &data_.listed);
	ImGui::SameLine(0, ui::px(24));
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Locked by default");
	ImGui::SameLine();
	ui::checkbox("##locked", &data_.locked);
	ui::form_label("Unlocked by", lw);
	{
		std::string by;
		for (uint32_t t : ed::addon_tournaments(*e.project())) {
			ed::Tournament other = ed::read_tournament(*e.project(), ed::tournament_objects(*e.project(), t));
			for (auto &u : other.unlocks) {
				if (u.description == guid_) by += (by.empty() ? "" : ", ") + other.name;
			}
		}
		ImGui::AlignTextToFramePadding();
		ui::dim_text(by.empty() ? (data_.locked ? "nothing yet: add it to another tournament's Unlocks" : "(unlocked from the start)") : by.c_str(),
		             by.empty() && data_.locked ? kWarning : kTextSoft);
	}
	ImGui::SeparatorText("Who may play it");
	guid_list("Excluded pilots", data_.excluded_pilots, choices(e, "Description\\Pilot"));
	guid_list("Player pilots only", data_.player_pilots, choices(e, "Description\\Pilot"));
	if (o.value(*e.project(), "PlayerCreatures") >= 0) guid_list("Player robots only", data_.player_robots, choices(e, "Description\\Creature\\Fighter"));
}

void TournamentDocument::quotes_list(std::vector<ed::TQuote> &qs, const char *id) {
	const float fh = ImGui::GetFrameHeight();
	ImGui::PushID(id);
	for (size_t k = 0; k < qs.size(); ++k) {
		ImGui::PushID(int(k));
		ImGui::SetNextItemWidth(ui::px(130));
		int slot = std::clamp(qs[k].slot, 0, 5);
		if (ui::combo("##sp", &slot, kSpeakerLabels, 6)) qs[k].slot = slot;
		ImGui::SameLine();
		std::string text = qs[k].text;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fh - ui::px(8));
		if (ImGui::InputTextMultiline("##t", &text, ImVec2(ImGui::GetContentRegionAvail().x - fh - ui::px(8), fh * 1.6f))) qs[k].text = text;
		ImGui::SameLine();
		if (ui::icon_button("x", ICON_X, "Remove the quote")) {
			qs.erase(qs.begin() + long(k));
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}
	if (ui::toolbar_button(ICON_PLUS, "Quote", ui::Tone::Ghost, true, "A line said by a pilot (<Slot0Name>, <Return> work)", fh)) qs.push_back({1, ""});
	ImGui::PopID();
}

void TournamentDocument::match_form(Editor &e, int mi) {
	ed::TMatch &m = data_.matches[size_t(mi)];
	const float lw = ui::px(170), fh = ImGui::GetFrameHeight();
	ui::form_label("Match name", lw);
	ImGui::SetNextItemWidth(-ui::px(4));
	ImGui::InputText("##name", &m.name);
	ui::form_label("Script name", lw);
	ImGui::AlignTextToFramePadding();
	ImGui::PushFont(fonts.mono, kFontSmall);
	ui::dim_text(("match" + m.key + "  /  Match" + m.key + "()").c_str(), kTextDim);
	ImGui::PopFont();
	const Choices &pilots = choices(e, "Description\\Pilot");
	const Choices &robots = choices(e, "Description\\Creature\\Fighter");
	for (size_t k = 0; k < m.opponents.size(); ++k) {
		ImGui::PushID(int(k));
		ui::form_label(("Opponent " + std::to_string(k + 1)).c_str(), lw);
		float w = (ImGui::GetContentRegionAvail().x - fh - ui::px(16)) * 0.5f;
		guid_combo("##pilot", &m.opponents[k].pilot, pilots, "Random pilot", w);
		ImGui::SameLine();
		guid_combo("##robot", &m.opponents[k].robot, robots, "Random robot", w);
		ImGui::SameLine();
		if (ui::icon_button("x", ICON_X, "Remove the opponent", m.opponents.size() > 1)) {
			m.opponents.erase(m.opponents.begin() + long(k));
			ImGui::PopID();
			break;
		}
		if (!ed::guid_zero(m.player_faction) || !ed::guid_zero(m.opponents[k].faction)) {
			ui::form_label("   faction", lw);
			guid_combo("##fac", &m.opponents[k].faction, choices(e, "Description\\Faction"), "(no faction)");
		}
		ImGui::PopID();
	}
	ui::form_label("", lw);
	if (ui::toolbar_button(ICON_PLUS, "Opponent", ui::Tone::Ghost, m.opponents.size() < 5, nullptr, fh)) m.opponents.push_back({});
	if (!ed::guid_zero(m.player_faction) || !ed::guid_zero(m.enemy_faction)) {
		ui::form_label("Teams", lw);
		float w = (ImGui::GetContentRegionAvail().x - ui::px(8)) * 0.5f;
		guid_combo("##pf", &m.player_faction, choices(e, "Description\\Faction"), "(none)", w);
		ImGui::SameLine();
		guid_combo("##ef", &m.enemy_faction, choices(e, "Description\\Faction"), "(none)", w);
	}
	ui::form_label("Arena", lw);
	guid_combo("##arena", &m.arena, choices(e, "Description\\Environment"), "Random arena (SinglePlayerPickRandomEnvironment)");
	ui::form_label("Intro commentary", lw);
	ImGui::InputTextMultiline("##intro", &m.intro, ImVec2(-ui::px(4), fh * 2), ImGuiInputTextFlags_WordWrap);
	ui::form_label("Next match text", lw);
	ImGui::InputTextMultiline("##desc", &m.description, ImVec2(-ui::px(4), fh * 2), ImGuiInputTextFlags_WordWrap);
	ImGui::SeparatorText("Pre-match quotes");
	quotes_list(m.pre_quotes, "pre");
	ImGui::SeparatorText("Post-match");
	ui::form_label("Win commentary", lw);
	ImGui::InputTextMultiline("##wc", &m.win_commentary, ImVec2(-ui::px(4), fh * 1.6f), ImGuiInputTextFlags_WordWrap);
	ui::form_label("Lose commentary", lw);
	ImGui::InputTextMultiline("##lc", &m.lose_commentary, ImVec2(-ui::px(4), fh * 1.6f), ImGuiInputTextFlags_WordWrap);
	ui::dim_text("Quotes after a win", kTextMuted);
	quotes_list(m.win_quotes, "win");
	ui::dim_text("Quotes after a loss", kTextMuted);
	quotes_list(m.lose_quotes, "lose");
	ImGui::SeparatorText("Drop-ons");
	const Choices &drops = choices(e, "DropOn");
	for (size_t k = 0; k < m.drop_ons.size(); ++k) {
		ImGui::PushID(int(k) + 100);
		ImGui::SetNextItemWidth(ui::px(130));
		int slot = std::clamp(m.drop_ons[k].slot, 0, 5);
		if (ui::combo("##s", &slot, kSpeakerLabels, 6)) m.drop_ons[k].slot = slot;
		ImGui::SameLine();
		guid_combo("##a", &m.drop_ons[k].actor, drops, nullptr, ImGui::GetContentRegionAvail().x - ui::px(120) - fh);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(100));
		ImGui::DragFloat("##d", &m.drop_ons[k].delay, 1, 0, 10000, "delay %.0f");
		ImGui::SameLine();
		if (ui::icon_button("x", ICON_X, "Remove")) {
			m.drop_ons.erase(m.drop_ons.begin() + long(k));
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}
	if (ui::toolbar_button(ICON_PLUS, "Drop-on", ui::Tone::Ghost, !drops.empty(), "An actor dropped on a fighter (AddDropActorOnSlotCreature)", fh))
		m.drop_ons.push_back({0, drops.front().second, 0});
	ImGui::SeparatorText("Branches");
	auto target_combo = [&](const char *id, int *t, bool lose) {
		ImGui::SetNextItemWidth(-ui::px(4));
		if (ui::begin_combo(id, (std::string(ICON_ARROW_UP_RIGHT "  ") + ed::match_label(data_, *t, lose)).c_str())) {
			if (ImGui::Selectable(lose ? "Replay this match" : "Next match", *t == -1)) *t = -1;
			for (size_t k = 0; k < data_.matches.size(); ++k) {
				if (ImGui::Selectable(ed::match_label(data_, int(k), lose).c_str(), *t == int(k))) *t = int(k);
			}
			if (ImGui::Selectable("Ending cinematic", *t >= ed::kEnding)) *t = ed::kEnding;
			ImGui::EndCombo();
		}
	};
	ui::form_label("On win", lw);
	target_combo("##win", &m.on_win, false);
	ui::form_label("On lose", lw);
	target_combo("##lose", &m.on_lose, true);
	ui::form_label("AI offset", lw);
	ImGui::SetNextItemWidth(ui::px(160));
	ImGui::SliderInt("##aio", &m.ai_offset, -3, 5, "%+d");
}

void TournamentDocument::ending_form(Editor &e) {
	const float lw = ui::px(170), fh = ImGui::GetFrameHeight();
	ui::form_label("Cinematic arena", lw);
	guid_combo("##arena", &data_.ending_arena, choices(e, "Description\\Environment"), nullptr);
	ui::form_label("Title", lw);
	ImGui::SetNextItemWidth(-ui::px(4));
	ImGui::InputText("##n", &data_.ending_name);
	ui::form_label("Text", lw);
	ImGui::InputTextMultiline("##d", &data_.ending_description, ImVec2(-ui::px(4), fh * 2.4f), ImGuiInputTextFlags_WordWrap);
	ImGui::SeparatorText("Cinematic quotes");
	for (size_t k = 0; k < data_.ending_quotes.size(); ++k) {
		ImGui::PushID(int(k));
		ImGui::InputTextMultiline("##q", &data_.ending_quotes[k], ImVec2(ImGui::GetContentRegionAvail().x - fh - ui::px(8), fh * 2), ImGuiInputTextFlags_WordWrap);
		ImGui::SameLine();
		if (ui::icon_button("x", ICON_X, "Remove")) {
			data_.ending_quotes.erase(data_.ending_quotes.begin() + long(k));
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}
	if (ui::toolbar_button(ICON_PLUS, "Quote", ui::Tone::Ghost, true, nullptr, fh)) data_.ending_quotes.push_back("");
	ImGui::SeparatorText("Unlocks");
	unlocks_form(e);
}

void TournamentDocument::branches_form() {
	ui::dim_text("Where each match leads. A lost match is played again unless it branches elsewhere.", kTextMuted);
	if (ImGui::BeginTable("br", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
		ImGui::TableSetupColumn("Match");
		ImGui::TableSetupColumn("On win");
		ImGui::TableSetupColumn("On lose");
		ImGui::TableHeadersRow();
		for (size_t k = 0; k < data_.matches.size(); ++k) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(ed::match_label(data_, int(k), false).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) sel_ = int(k);
			ImGui::TableNextColumn();
			int win = data_.matches[k].on_win == ed::kNextMatch ? int(k) + 1 : data_.matches[k].on_win;
			ui::dim_text(ed::match_label(data_, win, false).c_str(), data_.matches[k].on_win == ed::kNextMatch ? kTextDim : kAccentText);
			ImGui::TableNextColumn();
			ui::dim_text(ed::match_label(data_, data_.matches[k].on_lose, true).c_str(), data_.matches[k].on_lose == ed::kReplay ? kTextDim : kAccentText);
		}
		ImGui::EndTable();
	}
}

void TournamentDocument::unlocks_form(Editor &e) {
	const float fh = ImGui::GetFrameHeight();
	ui::dim_text("Unlocked when the tournament is won (AddUnlock in the ending): tournaments, pilots, robots, arenas, trophies.", kTextMuted);
	Choices all;
	for (const char *c : {"Description\\SessionMaster", "Description\\Pilot", "Description\\Creature\\Fighter", "Description\\Environment", "Description\\Creature"}) {
		for (auto &x : choices(e, c)) all.push_back(x);
	}
	for (size_t k = 0; k < data_.unlocks.size(); ++k) {
		ImGui::PushID(int(k));
		guid_combo("##u", &data_.unlocks[k].description, all, nullptr, ui::px(240));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fh - ui::px(8));
		ImGui::InputText("##t", &data_.unlocks[k].text);
		ImGui::SameLine();
		if (ui::icon_button("x", ICON_X, "Remove")) {
			data_.unlocks.erase(data_.unlocks.begin() + long(k));
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}
	if (ui::toolbar_button(ICON_PLUS, "Unlock", ui::Tone::Ghost, !all.empty(), nullptr, fh)) data_.unlocks.push_back({all.front().second, "Unlocked!"});
}

void TournamentDocument::scripts(Editor &e, ImVec2 size, const ed::TournamentObjects &o) {
	ui::begin_panel("scripts", size, kPanel, true, 8, ImVec2(ui::px(12), ui::px(8)));
	ui::tab_strip("stabs", {"Script (BSubSession)", "Script (BMaster)", "Quotes"}, &tab_);
	ImGui::Dummy(ImVec2(0, ui::px(4)));
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ed::Project &p = *e.project();
	if (tab_ < 2) {
		int beh = tab_ == 0 ? o.bsub : o.bmaster;
		std::string text;
		try {
			text = ed::script_text(ed::read_pcode(p.file().objects.at(size_t(beh)))).text;
		} catch (const std::exception &ex) {
			text = std::string("// cannot read the script: ") + ex.what();
		}
		std::vector<std::pair<int, std::string>> errors;
		for (auto &m : ed::last_compile_errors(p)) {
			if (m.object == beh) errors.emplace_back(m.text_line, m.text);
		}
		std::map<std::string, std::string> blocks;
		if (code_view(tab_ == 0 ? "sub" : "master", tab_ == 0 ? sub_view_ : master_view_, text, avail, errors, &blocks)) {
			if (tab_ == 0) {
				ed::write_tournament(p, o, data_, &blocks);
			} else {
				de::PCode code = ed::read_pcode(p.file().objects.at(size_t(beh)));
				for (auto &el : code.elements) {
					for (auto &[id, body] : blocks) {
						std::string open = "// <user " + id + ">";
						size_t a = el.source.find(open);
						size_t b = a == std::string::npos ? a : el.source.find("// </user>", a);
						if (b == std::string::npos) continue;
						size_t from = el.source.find('\n', a) + 1;
						std::string dos;
						for (char c : body) {
							if (c == '\n') dos += '\r';
							dos += c;
						}
						el.source.replace(from, b - from, dos);
					}
				}
				ed::write_behavior(p, uint32_t(beh), code, ed::behavior_global_targets(p, uint32_t(beh)));
			}
			e.log(LogLine::Info, "User blocks applied; compile to check them");
		}
	} else {
		ImGui::BeginChild("quotes", avail, 0, 0);
		if (ImGui::BeginTable("q", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupColumn("Match", ImGuiTableColumnFlags_WidthFixed, ui::px(110));
			ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthFixed, ui::px(64));
			ImGui::TableSetupColumn("Speaker", ImGuiTableColumnFlags_WidthFixed, ui::px(110));
			ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			int id = 0;
			for (size_t k = 0; k < data_.matches.size(); ++k) {
				auto &m = data_.matches[k];
				for (auto [list, when] : {std::pair{&m.pre_quotes, "before"}, std::pair{&m.win_quotes, "won"}, std::pair{&m.lose_quotes, "lost"}}) {
					for (auto &q : *list) {
						ImGui::PushID(id++);
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						if (ImGui::Selectable(("M" + std::to_string(k + 1) + " " + m.name).c_str())) sel_ = int(k);
						ImGui::TableNextColumn();
						ui::dim_text(when, kTextMuted);
						ImGui::TableNextColumn();
						ImGui::SetNextItemWidth(-1);
						int slot = std::clamp(q.slot, 0, 5);
						if (ui::combo("##s", &slot, kSpeakerLabels, 6)) q.slot = slot;
						ImGui::TableNextColumn();
						ImGui::SetNextItemWidth(-1);
						ImGui::InputText("##t", &q.text);
						ImGui::PopID();
					}
				}
			}
			ImGui::EndTable();
		}
		ImGui::EndChild();
	}
	ui::end_panel();
}

void TournamentDocument::footer(Editor &e, const ed::TournamentObjects &o) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddLine(ImVec2(ImGui::GetWindowPos().x, p.y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), p.y), rgb(kBorder));
	ImGui::SetCursorScreenPos(p + ImVec2(0, (ui::px(48) - ImGui::GetFrameHeight()) * 0.5f));
	const auto &errors = ed::last_compile_errors(*e.project());
	int stale = 0;
	for (auto &i : e.issues()) stale += i.rule == 9 && i.severity == ed::Issue::Error;
	ImGui::AlignTextToFramePadding();
	ImGui::PushFont(nullptr, kFontSmall);
	if (!errors.empty()) ui::status_chip(ICON_CIRCLE_X, (std::to_string(errors.size()) + " compile error(s)").c_str(), kError);
	else if (stale) ui::status_chip(ICON_TRIANGLE_ALERT, "Not compiled since the last change", kWarning);
	else ui::status_chip(ICON_CIRCLE_CHECK, "Compiled: VMC2 current", kOk);
	ImGui::SameLine(0, ui::px(14));
	ui::dim_text((std::to_string(data_.matches.size()) + " matches").c_str(), kTextDim);
	ImGui::PopFont();
	ImGui::SameLine();
	const float fh = ImGui::GetFrameHeight();
	float bw = ui::px(320);
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - bw - ui::px(18)));
	bool loading = !script_compiler(e).loaded();
	if (ui::toolbar_button(ICON_HAMMER, loading ? "Compile (loading...)" : "Compile", ui::Tone::Normal, true, "Compile BMaster and BSubSession into VMC2", fh))
		compile_after_ = true;
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_PLAY, "Play from match", ui::Tone::Play, true, "Save, compile and launch the game at the selected match", fh)) {
		compile_scripts(e);
		e.save();
		launch_tournament(e, data_.name, std::max(0, sel_));
	}
	(void)o;
}

void TournamentDocument::import_image(Editor &e, int image, bool icon) {
	ed::Guid g = guid_;
	e.file_dialog().open(FileDialog::OpenFile, icon ? "Import list icon" : "Import Choose Match image", {}, {".png", ".bmp"},
	                     [&e, g, image, icon](const std::filesystem::path &path) {
		                     try {
			                     ed::RgbaImage img = ed::load_image(path);
			                     int w = 0, h = 0, type = 0;
			                     ed::image_header(e.project()->file().objects.at(size_t(image)), &type, &w, &h);
			                     if (w <= 0 || h <= 0) w = img.width, h = img.height;
			                     ed::set_image_pixels(*e.project(), uint32_t(image), w, h, ed::resize_rgba(img.width, img.height, img.rgba, w, h),
			                                          path.filename().string());
			                     int d = e.find_object(g);
			                     if (d >= 0) {
				                     auto o = ed::tournament_objects(*e.project(), uint32_t(d));
				                     ed::Tournament t = ed::read_tournament(*e.project(), o);
				                     (icon ? t.icon_source : t.image_source) = path.filename().string();
				                     ed::write_tournament(*e.project(), o, t);
			                     }
			                     e.log(LogLine::Ok, "Imported " + path.filename().string());
		                     } catch (const std::exception &ex) {
			                     e.log(LogLine::Error, std::string("Cannot import: ") + ex.what());
		                     }
	                     });
}
