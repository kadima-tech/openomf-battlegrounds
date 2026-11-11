#include "assets.h"

#include <algorithm>
#include <map>

#include "har.h"
#include "icons.h"
#include "imgui_stdlib.h"
#include "theme.h"
#include "tournament.h"
#include "vm/vmcode.h"
#include "widgets.h"

using namespace theme;

namespace {

struct AssetDialogs {
	bool tournament_open = false, har_open = false;
	int base_tournament = 0;
	std::string tournament_name = "New Tournament";
	bool blank = false;
	std::string base_fighter = "Jaguar", har_name = "Jaguar Mk2";
};
AssetDialogs g_dialogs;

std::string tutf8(const std::string &latin1) { return deo::latin1_to_utf8(deo::Bytes(latin1.begin(), latin1.end())); }

void open_tournament(Editor &e, uint32_t description) {
	ed::Guid g = e.project()->file().objects[description].info.guid;
	e.add_document(make_tournament_document(e, g), ObjectRef::addon(g));
}

void open_har(Editor &e, uint32_t description) {
	ed::Guid g = e.project()->file().objects[description].info.guid;
	e.add_document(make_har_document(e, g), ObjectRef::addon(g));
}

bool make_tournament(Editor &e, const ed::BaseObject &base, const std::string &name, bool blank) {
	try {
		uint32_t d = ed::create_tournament(*e.project(), base, name, blank);
		e.log(LogLine::Ok, "New tournament " + name + (blank ? " (blank, from " : " (a copy of ") + base.name + ")");
		open_tournament(e, d);
		return true;
	} catch (const std::exception &ex) {
		e.log(LogLine::Error, std::string("Cannot make the tournament: ") + ex.what());
		return false;
	}
}

bool make_har(Editor &e, const std::string &base, const std::string &name) {
	try {
		uint32_t d = ed::create_har_variant(*e.project(), base, name);
		e.log(LogLine::Ok, "New HAR variant " + name + " (a copy of " + base + ")");
		open_har(e, d);
		return true;
	} catch (const std::exception &ex) {
		e.log(LogLine::Error, std::string("Cannot make the HAR variant: ") + ex.what());
		return false;
	}
}

}  // namespace

void assets_open_har(Editor &e, uint32_t description) { open_har(e, description); }

ed::ScriptCompiler &script_compiler(Editor &e) {
	static std::map<std::string, std::unique_ptr<ed::ScriptCompiler>> compilers;
	auto &c = compilers[e.base()->dir().string()];
	if (!c) c = std::make_unique<ed::ScriptCompiler>(e.base()->dir());
	return *c;
}

int compile_scripts(Editor &e) {
	if (!e.project() || !e.base()) return 0;
	auto classes = ed::addon_script_classes(*e.project());
	if (classes.empty()) return 0;
	if (!script_compiler(e).loaded()) e.log(LogLine::Info, "Reading the base game for the compiler...");
	int errors = 0;
	for (auto &[cls, res] : script_compiler(e).compile_all(*e.project())) {
		const std::string name = e.project()->file().objects[cls].info.name_text();
		if (res.ok) e.log(LogLine::Ok, "Compiled " + name + " into VMC2");
		for (auto &m : res.errors) e.log(LogLine::Error, name + ": " + m.str()), ++errors;
	}
	e.validate_now();
	return errors;
}

void launch_tournament(Editor &e, const std::string &name, int match) {
	e.launch_game({"--launch", name, "--start-match", std::to_string(match)});
}

void assets_content_menu(Editor &e) {
	if (ImGui::MenuItem(ICON_BOT "  New HAR Variant...", nullptr, false, e.base() != nullptr)) {
		auto go = [&e] { g_dialogs.har_open = true; };
		if (e.project()) go();
		else e.with_addon("The new HAR variant goes into an add-on.", go);
	}
	if (ImGui::MenuItem(ICON_TROPHY "  New Tournament...", nullptr, false, e.base() != nullptr)) {
		auto go = [&e] { g_dialogs.tournament_open = true; };
		if (e.project()) go();
		else e.with_addon("The new tournament goes into an add-on.", go);
	}
	if (ImGui::MenuItem(ICON_HAMMER "  Compile Scripts", nullptr, false, e.project() != nullptr)) compile_scripts(e);
}

void assets_play_menu(Editor &e) {
	if (!e.project()) return;
	auto list = ed::addon_tournaments(*e.project());
	if (list.empty()) return;
	ImGui::SeparatorText("Tournament (Launch Game at a match)");
	for (uint32_t t : list) {
		ed::Tournament tour = ed::read_tournament(*e.project(), ed::tournament_objects(*e.project(), t));
		if (!ImGui::BeginMenu((std::string(ICON_TROPHY "  ") + tour.name).c_str())) continue;
		for (size_t k = 0; k < tour.matches.size(); ++k) {
			if (ImGui::MenuItem(ed::match_label(tour, int(k), false).c_str())) {
				compile_scripts(e);
				e.save();
				launch_tournament(e, tour.name, int(k));
			}
		}
		ImGui::EndMenu();
	}
}

void assets_popups(Editor &e) {
	if (g_dialogs.tournament_open) ImGui::OpenPopup("New Tournament"), g_dialogs.tournament_open = false;
	if (g_dialogs.har_open) ImGui::OpenPopup("New HAR Variant"), g_dialogs.har_open = false;
	ImGui::SetNextWindowSize(ImVec2(ui::px(520), 0));
	if (ImGui::BeginPopupModal("New Tournament", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		auto bases = ed::base_tournaments(*e.base());
		ui::dim_text("A copy of a base tournament (its description, session master, sub-session handler, scripts and Choose Match "
		             "panel) with fresh GUIDs. Its ladder is read from the base scripts, or starts blank with one match.",
		             kTextSoft);
		ImGui::Dummy(ImVec2(0, ui::px(6)));
		g_dialogs.base_tournament = std::clamp(g_dialogs.base_tournament, 0, std::max(0, int(bases.size()) - 1));
		ImGui::TextUnformatted("Copy from");
		ImGui::SetNextItemWidth(-1);
		if (ui::begin_combo("##base", bases.empty() ? "" : bases[size_t(g_dialogs.base_tournament)]->name.c_str())) {
			for (size_t k = 0; k < bases.size(); ++k) {
				std::string label = bases[k]->name + "  (" + e.base()->files()[bases[k]->file].stem + ")";
				if (ImGui::Selectable(label.c_str(), int(k) == g_dialogs.base_tournament)) g_dialogs.base_tournament = int(k);
			}
			ImGui::EndCombo();
		}
		ImGui::TextUnformatted("Name");
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##name", &g_dialogs.tournament_name);
		ui::checkbox("##blank", &g_dialogs.blank);
		ImGui::SameLine();
		ImGui::TextUnformatted("Blank ladder (one match) instead of the base's matches");
		ImGui::Dummy(ImVec2(0, ui::px(6)));
		if (ui::toolbar_button(ICON_PLUS, "Create", ui::Tone::Accent, !bases.empty() && !g_dialogs.tournament_name.empty())) {
			if (make_tournament(e, *bases[size_t(g_dialogs.base_tournament)], g_dialogs.tournament_name, g_dialogs.blank)) ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ui::toolbar_button(nullptr, "Cancel", ui::Tone::Ghost)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::SetNextWindowSize(ImVec2(ui::px(520), 0));
	if (ImGui::BeginPopupModal("New HAR Variant", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ui::dim_text("A copy of a base robot's fighter file (its objects, scripts, animations, input sequences and spheres) with fresh "
		             "GUIDs, a new Description\\Creature\\Fighter and a FighterModelDescription that uses the base paint job.",
		             kTextSoft);
		ImGui::Dummy(ImVec2(0, ui::px(6)));
		ImGui::TextUnformatted("Copy from");
		ImGui::SetNextItemWidth(-1);
		if (ui::begin_combo("##fighter", g_dialogs.base_fighter.c_str())) {
			for (auto &f : ed::base_fighters(*e.base())) {
				if (ImGui::Selectable(f.c_str(), f == g_dialogs.base_fighter)) g_dialogs.base_fighter = f, g_dialogs.har_name = f + " Mk2";
			}
			ImGui::EndCombo();
		}
		ImGui::TextUnformatted("Name");
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##name", &g_dialogs.har_name);
		ImGui::Dummy(ImVec2(0, ui::px(6)));
		if (ui::toolbar_button(ICON_PLUS, "Create", ui::Tone::Accent, !g_dialogs.har_name.empty())) {
			if (make_har(e, g_dialogs.base_fighter, g_dialogs.har_name)) ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ui::toolbar_button(nullptr, "Cancel", ui::Tone::Ghost)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

bool assets_open(Editor &e, const ObjectRef &r) {
	if (r.kind != ObjectRef::Addon || !e.project()) return false;
	int i = e.addon_index(r);
	if (i < 0) return false;
	const std::string cls = e.project()->file().objects[size_t(i)].info.class_text();
	if (cls == "Description\\SessionMaster" && ed::tournament_objects(*e.project(), uint32_t(i)).model >= 0) {
		open_tournament(e, uint32_t(i));
		return true;
	}
	if (cls == "Description\\Creature\\Fighter") {
		for (uint32_t h : ed::addon_hars(*e.project())) {
			if (int(h) == i) {
				open_har(e, h);
				return true;
			}
		}
	}
	return false;
}

void assets_startup(Editor &e) {
	const Options &o = e.options();
	if (!e.project() || !e.base()) return;
	auto split = [](const std::string &s, std::string *a, std::string *b) {
		auto c = s.find(':');
		*a = s.substr(0, c);
		*b = c == std::string::npos ? *a + " Copy" : s.substr(c + 1);
	};
	if (!o.new_tournament.empty()) {
		std::string from, name;
		split(o.new_tournament, &from, &name);
		bool blank = from.rfind("blank=", 0) == 0;
		if (blank) from = from.substr(6);
		bool exists = false;
		for (uint32_t t : ed::addon_tournaments(*e.project())) exists |= tutf8(e.project()->file().objects[t].info.name_text()) == name;
		if (!exists) {
			for (auto *b : ed::base_tournaments(*e.base())) {
				if (b->name == from) {
					make_tournament(e, *b, name, blank);
					break;
				}
			}
		}
	}
	if (!o.new_har.empty()) {
		std::string from, name;
		split(o.new_har, &from, &name);
		bool exists = false;
		for (uint32_t h : ed::addon_hars(*e.project())) exists |= tutf8(e.project()->file().objects[h].info.name_text()) == name;
		if (!exists) make_har(e, from, name);
	}
	if (!o.har_constants.empty()) {
		std::string target = o.open_har;
		if (target.empty() && !o.new_har.empty()) {
			std::string from;
			split(o.new_har, &from, &target);
		}
		for (uint32_t h : ed::addon_hars(*e.project())) {
			if (tutf8(e.project()->file().objects[h].info.name_text()) != target) continue;
			ed::HarObjects ho = ed::har_objects(*e.project(), h);
			ed::HarData d = ed::read_har(*e.project(), ho);
			for (auto &spec : o.har_constants) {
				auto eq = spec.find('=');
				bool found = false;
				for (auto &c : d.constants) {
					if (eq == std::string::npos || c.name != spec.substr(0, eq)) continue;
					c.value = atof(spec.c_str() + eq + 1);
					found = true;
				}
				e.log(found ? LogLine::Ok : LogLine::Error, (found ? "Set " : "No tunable constant ") + spec);
			}
			ed::write_har(*e.project(), ho, d);
		}
	}
	if (o.compile) compile_scripts(e);
	for (uint32_t t : ed::addon_tournaments(*e.project())) {
		if (!o.open_tournament.empty() && tutf8(e.project()->file().objects[t].info.name_text()) == o.open_tournament) open_tournament(e, t);
	}
	for (uint32_t h : ed::addon_hars(*e.project())) {
		if (!o.open_har.empty() && tutf8(e.project()->file().objects[h].info.name_text()) == o.open_har) open_har(e, h);
	}
}

void assets_before_save(Editor &e) {
	if (!e.project()) return;
	std::vector<uint8_t> want;
	bool stale = false;
	for (uint32_t c : ed::addon_script_classes(*e.project())) {
		const deo::Object &o = e.project()->file().objects[c];
		bool has = false;
		for (auto &x : o.ex) {
			if (ed::text_of(x.key) != "VMC2") continue;
			has = true;
			std::vector<uint8_t> have;
			try {
				have = vm::VMClass::parse(x.data, ed::object_references("Class", o.references).size()).change_info;
			} catch (const std::exception &) {
			}
			if (ed::change_information(*e.project(), c, &want) && have != want) stale = true;
		}
		stale |= !has;
	}
	if (stale) compile_scripts(e);
}

void assets_open_issue(Editor &e, const ed::Issue &issue) {
	if (!e.project() || issue.object < 0) return;
	for (uint32_t t : ed::addon_tournaments(*e.project())) {
		auto o = ed::tournament_objects(*e.project(), t);
		if (o.bsub != issue.object && o.bmaster != issue.object) continue;
		open_tournament(e, t);
		tournament_document_jump(e.active_document(), issue.object, issue.line, e);
		return;
	}
}
