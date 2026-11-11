// The content editor's asset paths on the game's Data folder.
//   tournament: new tournament (a copy of a base one) -> edit the ladder -> generate -> compile ->
//               save -> reopen -> the engine's readers see the description, its session master
//               and VMC2 that matches the add-on's behaviors.
//   har:        new HAR variant -> edit stats -> save -> reopen -> the engine reads the stats.
// Usage: editor-assets-tests <Data dir> [tournament|har]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "addon.h"
#include "content.h"
#include "de/scene.h"
#include "game/descriptions.h"
#include "game/runtime.h"
#include "har.h"
#include "script.h"
#include "tournament.h"
#include "validate.h"
#include "vm/compiler.h"
#include "vm/vmcode.h"

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                                   \
	do {                                                                              \
		if (!(cond)) {                                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                               \
		}                                                                             \
	} while (0)

static void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

static const ed::BaseObject *base_named(const ed::BaseData &base, const char *cls, const char *name) {
	for (const ed::BaseObject *o : base.of_class(cls)) {
		if (o->name == name) return o;
	}
	return nullptr;
}

static void tournament(const fs::path &data, const fs::path &tmp) {
	ed::BaseData base(data);
	auto bases = ed::base_tournaments(base);
	printf("base tournaments:");
	for (auto *b : bases) printf(" [%s]", b->name.c_str());
	printf("\n");
	CHECK(bases.size() >= 10);
	const ed::BaseObject *na = base_named(base, "Description\\SessionMaster", "North American Open");
	CHECK(na != nullptr);
	if (!na) return;
	const fs::path path = tmp / "TourneyMod.DEObjects";
	auto proj = ed::Project::create(base, path);
	uint32_t d = ed::create_tournament(*proj, *na, "Editor Open", false);
	ed::TournamentObjects o = ed::tournament_objects(*proj, d);
	CHECK(o.model >= 0 && o.bmaster >= 0 && o.bsub >= 0 && o.master_class >= 0 && o.sub_class >= 0);
	ed::Tournament t = ed::read_tournament(*proj, o);
	printf("imported %zu matches, %zu kept methods\n", t.matches.size(), t.extra.size());
	CHECK(t.matches.size() == 6);
	CHECK(t.name == "Editor Open");
	if (t.matches.empty()) return;
	CHECK(t.matches[0].key == "Welcome" && t.matches[0].name == "Welcome");
	CHECK(t.matches[0].opponents.size() == 2 && !ed::guid_zero(t.matches[0].opponents[0].pilot));

	// Edit: drop to two matches, an add-on-free opponent change, a lose branch, quotes.
	const ed::BaseObject *lucas = base_named(base, "Description\\Pilot", "Lucas");
	const ed::BaseObject *jaguar = base_named(base, "Description\\Creature\\Fighter", "Jaguar");
	const ed::BaseObject *desert = base_named(base, "Description\\Environment", "The Desert");
	CHECK(lucas && jaguar && desert);
	t.matches.resize(2);
	t.extra.erase(std::remove_if(t.extra.begin(), t.extra.end(), [&](const ed::TMethod &m) { return m.name.rfind("MatchWelcome", 0) != 0 && m.name.rfind("MatchMadness", 0) != 0; }), t.extra.end());
	t.matches[1].opponents = {{lucas->guid, jaguar->guid, {}}};
	t.matches[1].arena = desert->guid;
	t.matches[1].pre_quotes = {{1, "You again?\nThis won't take long."}, {0, "We'll see."}};
	t.matches[1].win_commentary = "<Slot0Name> wins the Editor Open's second round.";
	t.matches[1].lose_commentary = "Back to the start.";
	t.matches[1].on_lose = 0;
	t.ending_quotes = {"The Editor Open is over."};
	ed::write_tournament(*proj, o, t);
	auto gen = ed::generate_scripts(*proj, o, t);
	std::string text = ed::script_text(gen.sub).text;
	CHECK(text.find("SubSessionActivateSlot(1,g_FighterJaguar,g_PilotLucas);") != std::string::npos);
	CHECK(text.find("SubSessionPickEnvironment(g_EnvironmentDesert);") != std::string::npos);
	CHECK(text.find("You again?<Return>This won't take long.") != std::string::npos);
	CHECK(text.find("SetNextMatch(matchWelcome);") != std::string::npos);
	std::string mtext = ed::script_text(gen.master).text;
	CHECK(mtext.find("matchMadness,") != std::string::npos && mtext.find("m_MatchNumber=m_NextMatchNumber;") != std::string::npos);

	// A user block survives regeneration.
	{
		ed::Tournament t2 = ed::read_tournament(*proj, o);
		de::PCode sub = ed::read_pcode(proj->file().objects[size_t(o.bsub)]);
		for (auto &e : sub.elements) {
			if (e.name == "MatchMadness") {
				size_t at = e.source.find("// </user>");
				if (at != std::string::npos) e.source.insert(at, "SlotPilot(1).SetAIDifficultyOffset(3);\r\n");
			}
		}
		ed::write_behavior(*proj, uint32_t(o.bsub), sub, ed::behavior_global_targets(*proj, uint32_t(o.bsub)));
		t2.matches[1].name = "Madness II";
		ed::write_tournament(*proj, o, t2);
		std::string again = ed::script_text(ed::read_pcode(proj->file().objects[size_t(o.bsub)])).text;
		CHECK(again.find("SlotPilot(1).SetAIDifficultyOffset(3);") != std::string::npos);
		CHECK(again.find("\"Madness II\"") != std::string::npos);
	}

	// Compile both classes into VMC2.
	ed::ScriptCompiler compiler(data);
	for (auto &[cls, res] : compiler.compile_all(*proj)) {
		printf("compile %s: %s\n", proj->file().objects[cls].info.name_text().c_str(), res.ok ? "ok" : "FAILED");
		for (auto &e : res.errors) printf("  %s\n", e.str().c_str());
		CHECK(res.ok);
	}
	auto issues = ed::validate(*proj);
	for (auto &i : issues) {
		if (i.severity == ed::Issue::Error) printf("  validation error: %s\n", i.text.c_str());
		CHECK(i.severity != ed::Issue::Error);
	}
	proj->save();
	if (const char *keep = getenv("EDITOR_ASSETS_OUT")) fs::copy_file(path, fs::path(keep) / path.filename(), fs::copy_options::overwrite_existing);

	// Reopen: the same model.
	auto again = ed::Project::open(base, path);
	auto list = ed::addon_tournaments(*again);
	CHECK(list.size() == 1);
	if (list.empty()) return;
	ed::TournamentObjects o2 = ed::tournament_objects(*again, list[0]);
	ed::Tournament t3 = ed::read_tournament(*again, o2);
	CHECK(t3.matches.size() == 2 && t3.matches[1].name == "Madness II" && t3.matches[1].on_lose == 0);

	// The engine's readers: the description is listed, unlocked, and its VMC2 is current.
	set_env("OPENBG_ADDONS", path.string());
	{
		de::World w(data);
		const de::ObjectsFile *f = w.file("TourneyMod");
		CHECK(f != nullptr);
		if (f) {
			de::ObjectRef desc = w.find("Description\\SessionMaster", "Editor Open");
			CHECK(desc);
			if (desc) {
				game::DescriptionInfo info = game::read_description(w, desc);
				CHECK(info.listed && !info.locked_by_default);
			}
			for (uint32_t i = 0; i < f->objects().size(); ++i) {
				if (f->objects()[i].class_path != "Class") continue;
				auto od = f->object_data(f->objects()[i]);
				auto payload = f->ex_payload(od, "VMC2");
				CHECK(payload.has_value());
				if (!payload) continue;
				std::vector<de::ObjectRef> attached;
				for (auto &r : vm::class_behavior_refs(od.references)) attached.push_back(w.resolve(*f, r));
				auto change = vm::change_information(w, attached);
				CHECK(vm::VMClass::parse(payload->data, attached.size()).change_info == change);
			}
		}
	}
	set_env("OPENBG_ADDONS", "");
}

static void har(const fs::path &data, const fs::path &tmp) {
	ed::BaseData base(data);
	const fs::path path = tmp / "HarMod.DEObjects";
	auto proj = ed::Project::create(base, path);
	uint32_t d = ed::create_har_variant(*proj, "Jaguar", "Jaguar Mk2");
	ed::HarObjects o = ed::har_objects(*proj, d);
	CHECK(o.fighter >= 0 && o.description >= 0 && !o.model_descriptions.empty() && o.inputs >= 0 && o.behavior >= 0);
	ed::HarData h = ed::read_har(*proj, o);
	printf("HAR: %zu spheres, %zu input sequences, %zu sounds, %zu constants\n", h.spheres.size(), h.inputs.size(), h.sounds.size(), h.constants.size());
	CHECK(h.name == "Jaguar Mk2" && h.listed && !h.locked);
	CHECK(h.stats[6] > 0 && h.stats[26] > 0);
	CHECK(!h.inputs.empty() && !h.spheres.empty() && !h.constants.empty());
	const float walk = h.stats[6] * 1.5f, jump = h.stats[26] + 100;
	h.stats[6] = walk, h.stats[26] = jump;
	h.spheres[0].falloff *= 2;
	h.inputs[0].steps[0].max_age += 3;
	ed::write_har(*proj, o, h);
	auto issues = ed::validate(*proj);
	for (auto &i : issues) {
		if (i.severity == ed::Issue::Error) printf("  validation error: %s\n", i.text.c_str());
		CHECK(i.severity != ed::Issue::Error);
	}
	proj->save();
	if (const char *keep = getenv("EDITOR_ASSETS_OUT")) fs::copy_file(path, fs::path(keep) / path.filename(), fs::copy_options::overwrite_existing);
	auto again = ed::Project::open(base, path);
	auto list = ed::addon_hars(*again);
	CHECK(list.size() == 1);
	if (list.empty()) return;
	ed::HarData h2 = ed::read_har(*again, ed::har_objects(*again, list[0]));
	CHECK(h2.name == "Jaguar Mk2" && h2.stats[6] == walk && h2.stats[26] == jump);
	CHECK(h2.spheres[0].falloff == h.spheres[0].falloff && h2.inputs[0] == h.inputs[0]);
	set_env("OPENBG_ADDONS", path.string());
	{
		game::World w(data);
		de::ObjectRef desc = w.data().find("Description\\Creature\\Fighter", "Jaguar Mk2");
		CHECK(desc);
		if (desc) {
			game::DescriptionInfo info = game::read_description(w.data(), desc);
			CHECK(info.listed && !info.locked_by_default);
			CHECK(!game::fighter_models(w.data(), desc).empty());
			de::ObjectRef actor = w.data().by_guid(*reinterpret_cast<const de::Guid *>(again->file().objects[size_t(ed::har_objects(*again, list[0]).fighter)].info.guid.b));
			CHECK(actor);
			if (actor) {
				auto st = game::read_fighter_stats(*actor.file, actor.info());
				CHECK(st && st->walk == walk && st->jump_up == jump);
			}
		}
	}
	set_env("OPENBG_ADDONS", "");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir> [tournament|har]\n", argv[0]);
		return 2;
	}
	const fs::path data = argv[1];
	const std::string which = argc > 2 ? argv[2] : "";
	fs::path tmp = fs::temp_directory_path() / ("openbg-editor-assets-" + std::to_string(std::rand()) + std::to_string(time(nullptr)));
	fs::create_directories(tmp);
	if (which.empty() || which == "tournament") tournament(data, tmp);
	if (which.empty() || which == "har") har(data, tmp);
	std::error_code ec;
	fs::remove_all(tmp, ec);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("editor asset tests passed\n");
	return 0;
}
