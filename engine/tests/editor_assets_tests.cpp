// The content editor's asset paths on the game's Data folder.
//   tournament: new tournament (a copy of a base one) -> edit the ladder -> generate -> compile ->
//               save -> reopen -> the engine's readers see the description, its session master
//               and VMC2 that matches the add-on's behaviors.
//   har:        new HAR variant -> edit stats -> save -> reopen -> the engine reads the stats.
//   help:       Help topics for a HAR from a pages file -> the Help screen's tree lists them under the base topic.
// Usage: editor-assets-tests <Data dir> [tournament|har|help]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

#include "addon.h"
#include "content.h"
#include "de/scene.h"
#include "game/descriptions.h"
#include "game/runtime.h"
#include "har.h"
#include "help.h"
#include "image_io.h"
#include "pilot.h"
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

	// Scripted fields (--tournament-set) give the same model and scripts as the form.
	{
		ed::Tournament s = t;
		const std::pair<const char *, const char *> sets[] = {
		    {"match+", "Finale"},
		    {"match.3.opponent.1", "Lucas,Jaguar"},
		    {"match.3.opponent.2", "Lucas"},
		    {"match.3.arena", "The Desert"},
		    {"match.3.pre-quote+", "1:You again?\\nThis won't take long."},
		    {"match.3.win-quote+", "0:Told you."},
		    {"match.3.lose-quote+", "1:Again."},
		    {"match.3.lose-quotes", ""},
		    {"match.3.ai-offset", "2"},
		    {"unlock+", "Lucas"},
		    {"choose-text", "Three rounds."},
		    {"locked", "on"},
		};
		for (auto &[k, v] : sets) {
			try {
				ed::set_tournament_field(*proj, s, k, v);
			} catch (const std::exception &ex) {
				fprintf(stderr, "set %s=%s: %s\n", k, v, ex.what());
				CHECK(false);
			}
		}
		CHECK(s.matches.size() == 3 && s.matches[2].name == "Finale" && s.matches[2].key != s.matches[0].key && s.matches[2].key != s.matches[1].key);
		const ed::TMatch &m = s.matches[2];
		CHECK(m.opponents.size() == 2 && m.opponents[0].pilot == lucas->guid && m.opponents[0].robot == jaguar->guid && ed::guid_zero(m.opponents[1].robot));
		CHECK(m.arena == desert->guid && m.ai_offset == 2 && m.lose_quotes.empty() && m.win_quotes.size() == 1 && m.win_quotes[0].slot == 0);
		CHECK(m.pre_quotes.size() == 1 && m.pre_quotes[0].text == "You again?\nThis won't take long.");
		CHECK(s.unlocks.size() == t.unlocks.size() + 1 && s.unlocks.back().description == lucas->guid && s.locked && s.choose_text == "Three rounds.");
		std::string sub = ed::script_text(ed::generate_scripts(*proj, o, s).sub).text;
		CHECK(sub.find("SubSessionActivateSlot(2,CreatureListGetRandom(),g_PilotLucas);") != std::string::npos);
		for (auto &[k, v] : std::vector<std::pair<const char *, const char *>>{{"match.9.name", "x"}, {"match.1.opponent.7", "Lucas"}, {"match.1.opponent.1", "Nobody"},
		                                                                       {"match.1.pre-quote+", "no slot"}, {"match.1.pre-quote+", "6:x"}, {"match.1.arena", "Atlantis"},
		                                                                       {"unlock+", "Nothing"}, {"colour", "red"}, {"match.1.pre-quotes", "x"}}) {
			bool refused = false;
			try {
				ed::set_tournament_field(*proj, s, k, v);
			} catch (const std::exception &) {
				refused = true;
			}
			if (!refused) fprintf(stderr, "set %s=%s was taken\n", k, v);
			CHECK(refused);
		}
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
	{
		// A script constant: the behavior's members line is rewritten and the fighter's classes
		// recompile; the folded constant (1.25f) is in the fighter class's VMC2.
		ed::HarData c = ed::read_har(*proj, o);
		bool damage = false;
		for (auto &k : c.constants) {
			if (k.name == "m_InitialMeleeAttackDamageMultiplier") k.value = 1.25, damage = true;
		}
		CHECK(damage);
		ed::write_har(*proj, o, c);
		CHECK(ed::script_text(ed::read_pcode(proj->file().objects[size_t(o.behavior)])).text.find("m_InitialMeleeAttackDamageMultiplier=1.25;") != std::string::npos);
		CHECK(ed::read_har(*proj, o).constants.front().name == "m_InitialMeleeAttackDamageMultiplier");
		ed::ScriptCompiler compiler(data);
		size_t fighter_classes = 0;
		for (auto &[cls, res] : compiler.compile_all(*proj)) {
			printf("compile %s: %s\n", proj->file().objects[cls].info.name_text().c_str(), res.ok ? "ok" : "FAILED");
			for (auto &e : res.errors) printf("  %s\n", e.str().c_str());
			CHECK(res.ok);
			if (proj->file().objects[cls].info.name_text() != "PJaguar") continue;
			++fighter_classes;
			for (auto &e : proj->file().objects[cls].ex) {
				if (e.key != ed::text_bytes("VMC2")) continue;
				static const uint8_t k125[4] = {0x00, 0x00, 0xa0, 0x3f};
				CHECK(std::search(e.data.begin(), e.data.end(), k125, k125 + 4) != e.data.end());
			}
		}
		CHECK(fighter_classes == 1);
	}
	{
		// A script patch: members appended (past the stored source's terminator), a method replaced,
		// a state and a method nested under it added; the fighter class compiles with them.
		const std::string patch =
		    "Comment lines before the first section.\n"
		    "// ---- behavior BJaguar\n"
		    "// ---- members\nfinal byte m_PatchKind=7;\n"
		    "// ---- state Ground.PatchTest\nreplicate param byte Kind;\nexKillCallers();\nAnimActivate(seqStanding);\nexEndFrame();\n"
		    "// ---- method Ground.PatchTest.TryLaunch\nif (IsUserControlled() && Input.Verify(g_StandardInput.seqDoubleTapBack))\n\tGround.PatchTest(m_PatchKind);\n"
		    "// ---- method FighterChangedState\nSetGlobalInterest(InterestLevelNormal);\nBFighter::FighterChangedState();\n"
		    "// ---- behavior BSomethingElse\n// ---- method Ignored\nreturn;\n";
		auto sections = ed::parse_script_patch(patch);
		CHECK(sections.size() == 5 && sections[0].kind == "members" && sections[1].kind == "state" && sections[1].name == "Ground.PatchTest");
		de::PCode code = ed::read_pcode(proj->file().objects[size_t(o.behavior)]);
		const size_t before = code.elements.size();
		ed::ScriptPatchResult r = ed::apply_script_patch(code, sections);
		CHECK(r.added >= 2 && r.added + r.replaced == 3 && r.members == 1 && code.elements.size() == before + size_t(r.added));
		CHECK(code.members.source.find('\0') == std::string::npos && code.members.source.find("m_PatchKind=7") != std::string::npos);
		bool nested = false;
		for (size_t i = 0; i < code.elements.size(); ++i) nested |= code.qualified_name(i) == "Ground.PatchTest.TryLaunch" && code.elements[i].method_id == 1;
		CHECK(nested);
		de::PCode bad = code;
		bool threw = false;
		try {
			ed::apply_script_patch(bad, ed::parse_script_patch("// ---- behavior BJaguar\n// ---- method NoSuch.Parent.Method\nreturn;\n"));
		} catch (const std::exception &) {
			threw = true;
		}
		CHECK(threw);
		ed::write_behavior(*proj, uint32_t(o.behavior), code, ed::behavior_global_targets(*proj, uint32_t(o.behavior)));
		ed::ScriptCompiler compiler(data);
		auto res = compiler.compile(*proj, uint32_t(o.behavior_class));
		for (auto &e : res.errors) printf("  patched: %s\n", e.str().c_str());
		CHECK(res.ok);
		printf("script patch: %d added, %d replaced, members extended; PJaguar compiles\n", r.added, r.replaced);
	}
	uint32_t paint = ed::create_paint_job(*proj, o, "Paint2");
	auto images = ed::paint_images(*proj, paint);
	CHECK(images.size() >= 2);
	for (auto &img : images) {
		if (img.name != "RGBMain") continue;
		int w = 0, hh = 0, type = 0;
		ed::image_header(proj->file().objects[size_t(img.object)], &type, &w, &hh);
		std::vector<uint8_t> px(size_t(w) * size_t(hh) * 4);
		for (size_t k = 0; k < px.size(); k += 4) px[k] = 40, px[k + 1] = 200, px[k + 2] = 90, px[k + 3] = 255;
		ed::set_image_pixels(*proj, uint32_t(img.object), w, hh, px, "green.png");
	}
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
			printf("paint jobs: %zu\n", game::fighter_models(w.data(), desc).size());
			CHECK(game::fighter_models(w.data(), desc).size() >= 2);
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

// A Description with a hierarchy block listing `kids` (local indices).
static de::NewObject topic(const std::string &name, const std::vector<uint8_t> &kids = {}) {
	de::NewObject o;
	o.name = name, o.class_path = "Description", o.guid = de::new_guid();
	o.hierarchy = {1, uint8_t(1 + kids.size() * 2)};
	for (uint8_t k : kids) o.hierarchy.insert(o.hierarchy.end(), {1, uint8_t(0x32 + k)});
	o.hierarchy.push_back(0);
	return o;
}

// The Help list's tree over hand-made files: an add-on's topic named as one at the same place merges.
static void topic_tree_merges() {
	de::ObjectsFile base(de::write_objects_file("Base", de::new_guid(), {topic("Robots", {1, 2}), topic("Katana"), topic("Jaguar"), topic("Tips")}));
	de::ObjectsFile addon(de::write_objects_file("Mod", de::new_guid(),
	                                             {topic("Robots", {1}), topic("Shadow", {2}), topic("Moves"), topic("Tips", {4}), topic("Jaguar")}));
	auto sorted = [](std::vector<de::ObjectRef> e) {
		std::stable_sort(e.begin(), e.end(), [](auto &a, auto &b) { return a.info().name < b.info().name; });
		return e;
	};
	std::vector<de::ObjectRef> only{{&base, 0}, {&base, 1}, {&base, 2}, {&base, 3}};
	only = sorted(only);
	std::vector<de::ObjectRef> before = only;
	std::vector<int> tree = game::topic_tree(only);
	CHECK(only.size() == before.size() && tree.size() == only.size());
	auto row = [](const std::vector<de::ObjectRef> &e, const de::ObjectsFile *f, uint32_t i) {
		for (size_t k = 0; k < e.size(); ++k) {
			if (e[k].file == f && e[k].index == i) return int(k);
		}
		return -1;
	};
	CHECK(tree[size_t(row(only, &base, 1))] == row(only, &base, 0) && tree[size_t(row(only, &base, 0))] == -1);
	std::vector<de::ObjectRef> both;
	for (uint32_t i = 0; i < 4; ++i) both.push_back({&base, i});
	for (uint32_t i = 0; i < 5; ++i) both.push_back({&addon, i});
	both = sorted(both);
	tree = game::topic_tree(both);
	CHECK(both.size() == 7 && tree.size() == 7);
	CHECK(row(both, &addon, 0) < 0 && row(both, &addon, 3) < 0);
	CHECK(tree[size_t(row(both, &addon, 1))] == row(both, &base, 0));
	CHECK(tree[size_t(row(both, &addon, 2))] == row(both, &addon, 1));
	CHECK(tree[size_t(row(both, &addon, 4))] == row(both, &base, 3));
	CHECK(tree[size_t(row(both, &base, 2))] == row(both, &base, 0));
}

static void help(const fs::path &data, const fs::path &tmp) {
	topic_tree_merges();
	{
		const std::string text =
		    "# a comment\n== A/B\ntitle: Tee\nimage: picture\nalign: left\n\nLine one\n\nLine three\n\n\n== A/B/C\npage: X/Y\n";
		auto t = ed::parse_help_file(text);
		CHECK(t.size() == 2 && t[0].path == "A/B" && t[0].title == "Tee" && t[0].image == "picture" && t[0].left && t[0].text == "Line one\n\nLine three");
		CHECK(t[1].same_as == "X/Y" && t[1].text.empty());
		CHECK(ed::parse_help_file(ed::format_help_file(t)) == t);
		for (const char *bad : {"text first\n== A\n", "== A\npage: B\nsome text\n", "== A\nalign: up\n", "== A//B\n"}) {
			bool threw = false;
			try {
				ed::parse_help_file(bad);
			} catch (const std::exception &) {
				threw = true;
			}
			CHECK(threw);
		}
	}
	ed::BaseData base(data);
	auto paths = ed::base_help_paths(base);
	printf("base Help topics: %zu\n", paths.size());
	CHECK(paths.size() == 166);
	CHECK(std::count(paths.begin(), paths.end(), "6. HAR (Robot) Reference/Katana/Special Attacks/Razor Spin") == 1);
	const fs::path path = tmp / "HelpMod.DEObjects";
	auto proj = ed::Project::create(base, path);
	uint32_t d = ed::create_har_variant(*proj, "Jaguar", "Jaguar Mk2");
	ed::HarObjects o = ed::har_objects(*proj, d);
	ed::own_presentation(*proj, o);
	const int picture = ed::read_presentation(*proj, o).picture;
	CHECK(picture >= 0);
	const fs::path png = tmp / "move.png";
	{
		std::vector<uint8_t> px(64 * 32 * 4, 200);
		auto bytes = ed::encode_png(64, 32, px);
		std::ofstream(png, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
	}
	const std::string robot = "6. HAR (Robot) Reference/Jaguar Mk2";
	const std::string file = "== " + robot + "\ntitle: Jaguar Mk2\nimage: picture\nA faster Jaguar.\n\n== " + robot +
	                         "/Quick Reference\nJAGUAR MK2\n\n-Specials-\n\nLeap: f,f+P\n\n== " + robot + "/Special Attacks\npage: 6. HAR (Robot) Reference/Jaguar/Special Attacks\n\n== " +
	                         robot + "/Special Attacks/Leap\nimage: base:Katana\nLEAP\n\nf,f+P\n\nIt leaps.\n\n== " + robot + "/Special Attacks/Pounce\nimage: " + png.string() + "\nPOUNCE\n";
	auto topics = ed::parse_help_file(file);
	CHECK(topics.size() == 5);
	ed::write_help(*proj, topics, picture);
	const size_t objects = proj->file().objects.size();
	ed::write_help(*proj, topics, picture);
	CHECK(proj->file().objects.size() == objects);
	for (const char *bad : {"== Nowhere/Topic\nText\n", "== 6. HAR (Robot) Reference/Katana\nText\n", "== 6. HAR (Robot) Reference/X\npage: No/Such\n"}) {
		bool threw = false;
		try {
			ed::write_help(*proj, ed::parse_help_file(bad), picture);
		} catch (const std::exception &) {
			threw = true;
		}
		CHECK(threw);
	}
	{
		auto back = ed::read_help(*proj);
		CHECK(back.size() == 6);
		if (back.size() == 6) {
			CHECK(back[0].path == "6. HAR (Robot) Reference" && back[0].same_as == back[0].path);
			CHECK(back[1].path == robot && back[1].title == "Jaguar Mk2" && back[1].text == "A faster Jaguar." && back[1].image == "picture");
			CHECK(back[2].text == topics[1].text && back[2].title.empty() && back[2].image.empty());
			CHECK(!back[3].same_as.empty() && back[3].same_as.find("/Special Attacks") != std::string::npos);
			CHECK(back[4].image == "base:Katana" && back[4].text == "LEAP\n\nf,f+P\n\nIt leaps.");
			CHECK(back[5].image == "Pounce Image" && back[5].text == "POUNCE");
			// The panel's edit: a topic as read, its text changed, written back in place.
			back[5].text = "POUNCE\n\nb,f+K";
			ed::write_help(*proj, {back[1], back[5]}, picture);
			CHECK(proj->file().objects.size() == objects && ed::read_help(*proj)[5].text == "POUNCE\n\nb,f+K");
		}
	}
	for (auto &i : ed::validate(*proj)) {
		if (i.severity != ed::Issue::Ok) printf("  validation: %s\n", i.text.c_str());
		CHECK(i.severity != ed::Issue::Error);
	}
	proj->save();
	set_env("OPENBG_ADDONS", path.string());
	{
		// As the Help screen lists them: every HelpEntry description, by name, as a tree.
		de::World w(data);
		de::ObjectRef cast = w.find("Description\\Cast", "HelpEntry");
		std::vector<de::ObjectRef> entries;
		for (const auto &[stem, f] : w.files()) {
			for (uint32_t i = 0; i < f->objects().size(); ++i) {
				if (f->objects()[i].class_path.rfind("Description", 0) != 0) continue;
				for (auto &c : game::read_description(w, {f.get(), i}).casts) {
					if (cast && c.info().guid.str() == cast.info().guid.str()) entries.push_back({f.get(), i});
				}
			}
		}
		std::stable_sort(entries.begin(), entries.end(), [](auto &a, auto &b) { return a.info().name < b.info().name; });
		const size_t listed = entries.size();
		std::vector<int> tree = game::topic_tree(entries);
		CHECK(entries.size() == listed - 1);
		int har_ref = -1, mk2 = -1, leap = -1;
		for (size_t k = 0; k < entries.size(); ++k) {
			const std::string &n = entries[k].info().name;
			if (n == "6. HAR (Robot) Reference") {
				CHECK(har_ref < 0);
				har_ref = int(k);
			}
			if (n == "Jaguar Mk2") mk2 = int(k);
			if (n == "Leap") leap = int(k);
		}
		CHECK(har_ref >= 0 && mk2 >= 0 && leap >= 0);
		if (har_ref >= 0 && mk2 >= 0 && leap >= 0) {
			int katana = -1;
			for (size_t k = 0; k < entries.size(); ++k) katana = entries[k].info().name == "Katana" && entries[k].file == w.file("OMFHelp") ? int(k) : katana;
			CHECK(katana >= 0 && tree[size_t(katana)] == har_ref);
			CHECK(tree[size_t(mk2)] == har_ref);
			CHECK(entries[size_t(tree[size_t(leap)])].info().name == "Special Attacks" && tree[size_t(tree[size_t(leap)])] == mk2);
			game::ControlLayout page = game::read_control_creator(w, game::read_description(w, entries[size_t(leap)]).large);
			CHECK(page.align == 2 && page.items.size() == 2 && page.items[0].kind == game::ControlLayout::Image && page.items[1].text == " \r\nLEAP\r\n \r\nf,f+P\r\n \r\nIt leaps.");
			game::ControlLayout robot_page = game::read_control_creator(w, game::read_description(w, entries[size_t(mk2)]).large);
			CHECK(robot_page.items.size() == 3 && robot_page.items[0].kind == game::ControlLayout::Group && robot_page.items[0].items.size() == 1 &&
			      robot_page.items[0].items[0].text == "Jaguar Mk2" && robot_page.items[0].text_height == 18);
		}
	}
	set_env("OPENBG_ADDONS", "");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir> [tournament|har|help]\n", argv[0]);
		return 2;
	}
	const fs::path data = argv[1];
	const std::string which = argc > 2 ? argv[2] : "";
	fs::path tmp = fs::temp_directory_path() / ("openbg-editor-assets-" + which + "-" + std::to_string(std::random_device{}()) + std::to_string(time(nullptr)));
	fs::create_directories(tmp);
	if (which.empty() || which == "tournament") tournament(data, tmp);
	if (which.empty() || which == "har") har(data, tmp);
	if (which.empty() || which == "help") help(data, tmp);
	std::error_code ec;
	fs::remove_all(tmp, ec);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("editor asset tests passed\n");
	return 0;
}
