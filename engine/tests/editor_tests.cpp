// The content editor's pilot path, on the game's Data folder: new pilot (a copy of Lucas) ->
// edit every field -> import a photo -> save -> reopen -> the same fields, and the engine's own
// readers see the add-on's pilot through OPENBG_ADDONS. Usage: editor-tests <Data dir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "addon.h"
#include "de/image.h"
#include "de/scene.h"
#include "game/descriptions.h"
#include "game/runtime.h"
#include "image_io.h"
#include "pilot.h"
#include "validate.h"

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                                   \
	do {                                                                              \
		if (!(cond)) {                                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                               \
		}                                                                             \
	} while (0)

static std::vector<uint8_t> read_file(const fs::path &p) {
	std::ifstream f(p, std::ios::binary);
	return {std::istreambuf_iterator<char>(f), {}};
}

static void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

static ed::RgbaImage gradient(int w, int h) {
	ed::RgbaImage img;
	img.width = w, img.height = h;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			img.rgba.insert(img.rgba.end(), {uint8_t(x * 255 / (w - 1)), uint8_t(y * 255 / (h - 1)), 128, 255});
		}
	}
	return img;
}

static const ed::BaseObject *base_named(const ed::BaseData &base, const char *cls, const char *name) {
	for (const ed::BaseObject *o : base.of_class(cls)) {
		if (o->name == name) return o;
	}
	return nullptr;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir>\n", argv[0]);
		return 2;
	}
	const fs::path data = argv[1];
	ed::BaseData base(data);
	CHECK(base.files().size() > 10);
	auto names = ed::base_pilot_names(base);
	CHECK(std::find(names.begin(), names.end(), "Lucas") != names.end());

	fs::path tmp = fs::temp_directory_path() / ("openbg-editor-test-" + std::to_string(std::rand()) + std::to_string(time(nullptr)));
	fs::create_directories(tmp);
	const fs::path addon = tmp / "TestMod.DEObjects";

	// The image codecs.
	ed::RgbaImage src = gradient(300, 200);
	ed::RgbaImage png = ed::decode_png(ed::encode_png(src.width, src.height, src.rgba));
	CHECK(png.width == 300 && png.height == 200 && png.rgba == src.rgba);

	ed::PilotData want;
	{
		auto p = ed::Project::create(base, addon);
		uint32_t made = ed::create_pilot(*p, "Lucas", "Jetta");
		ed::PilotObjects o = ed::pilot_objects(*p, made);
		CHECK(o.description >= 0 && o.bio >= 0 && o.photo >= 0);
		ed::PilotData d = ed::read_pilot(*p, o);
		CHECK(d.name == "Jetta" && d.listed && !d.locked);
		CHECK(d.stats[0] == 80 && d.stats[1] == 55 && d.stats[2] == 45 && d.stats[3] == 20);
		CHECK(!d.casts.empty());
		for (auto &issue : ed::validate(*p)) {
			if (issue.severity == ed::Issue::Error) fprintf(stderr, "validation: %s\n", issue.text.c_str());
			CHECK(issue.severity != ed::Issue::Error);
		}
		d.sex = ed::kFemale;
		d.bio = "A test pilot from the editor, caf\xc3\xa9 included.";
		d.stats = {91, 12, 33, 64};
		d.colours[0] = {1, 0, 0, 1};
		d.colours[1] = {0, 1, 0, 0};
		d.colours[2] = {0.25f, 0.5f, 0.75f, 0};
		d.weights = {0.5f, 0, 1};
		for (size_t t = 0; t < d.traits.size(); ++t) d.traits[t] = {uint8_t(t % 9), uint8_t(t % 6), uint8_t(t % 4)};
		d.locked = false, d.visible_when_locked = true;
		d.casts.clear();
		for (const char *cast : {"PilotSPLadder", "MultiplayerPilot"}) {
			const ed::BaseObject *c = base_named(base, "Description\\Cast", cast);
			CHECK(c != nullptr);
			if (c) d.casts.push_back(c->guid);
		}
		ed::write_pilot(*p, o, d);
		ed::import_photo(*p, o, src.width, src.height, src.rgba, "jetta_photo.png");
		want = ed::read_pilot(*p, o);
		CHECK(want.photo_source == "jetta_photo.png");
		d.photo_source = want.photo_source;
		CHECK(want == d);
		p->save();
	}

	{
		auto p = ed::Project::open(base, addon);
		auto pilots = ed::addon_pilots(*p);
		CHECK(pilots.size() == 1);
		if (pilots.empty()) return 1;
		ed::PilotObjects o = ed::pilot_objects(*p, pilots[0]);
		ed::PilotData got = ed::read_pilot(*p, o);
		CHECK(got == want);
		int w = 0, h = 0;
		auto px = ed::photo_rgba(*p, o, &w, &h);
		CHECK(w == 256 && h == 256 && px.size() == 256 * 256 * 4);
		if (px.size() == 256 * 256 * 4) CHECK(std::abs(int(px[(128 * 256 + 128) * 4]) - 127) <= 3);
		CHECK(p->bytes() == read_file(addon));
		int errors = 0;
		for (auto &issue : ed::validate(*p)) {
			if (issue.severity == ed::Issue::Error) fprintf(stderr, "validation: %s\n", issue.text.c_str()), ++errors;
		}
		CHECK(errors == 0);
	}

	// The engine's readers, with the add-on beside a data folder of Pilots and SharedObjects.
	{
		fs::path dir = tmp / "Data";
		fs::create_directories(dir);
		for (const char *f : {"Pilots.DEObjects", "SharedObjects.DEObjects"}) fs::copy_file(data / f, dir / f);
		set_env("OPENBG_ADDONS", addon.string());
		de::World world(dir);
		const de::ObjectsFile *mod = world.file("TestMod");
		CHECK(mod != nullptr);
		if (!mod) return 1;
		de::ObjectRef pilot, desc;
		for (uint32_t i = 0; i < mod->objects().size(); ++i) {
			if (mod->objects()[i].class_path == "Actor\\Creature\\Pilot") pilot = {mod, i};
			if (mod->objects()[i].class_path == "Description\\Pilot") desc = {mod, i};
		}
		CHECK(pilot && desc);
		if (!pilot || !desc) return 1;
		auto gp = game::read_pilot(*mod, pilot.info(), 0);
		CHECK(gp.has_value());
		if (gp) {
			CHECK(gp->name == "Jetta" && gp->sex == ed::kFemale);
			CHECK(gp->stats.power == 91 && gp->stats.agility == 12 && gp->stats.endurance == 33 && gp->stats.focus == 64);
			CHECK(gp->colours[2][1] == 0.5f);
			CHECK(gp->ai.at(0x408).level == 1 && gp->ai.at(0x688).level == 30 % 9);
		}
		game::DescriptionInfo di = game::read_description(world, desc);
		CHECK(di.listed && !di.locked_by_default && di.casts.size() == 2);
		for (auto &c : di.casts) CHECK(c && c.file == world.file("SharedObjects"));
		de::ObjectRef photo = game::pilot_photo(world, desc);
		CHECK(photo && photo.file == mod);
		if (photo) {
			de::Image img = de::read_image(*mod, photo.info());
			CHECK(img.width == 256 && img.height == 256 && img.channels.size() == 4);
		}
		std::vector<de::ObjectRef> bio;
		for (uint32_t c : de::ObjectsFile::children(mod->object_data(desc.info()))) bio.push_back({mod, c});
		CHECK(bio.size() == 1 && game::read_string_value(bio[0]) == "A test pilot from the editor, caf\xe9 included.");
		set_env("OPENBG_ADDONS", "");
	}

	std::error_code ec;
	fs::remove_all(tmp, ec);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("editor pilot path ok\n");
	return 0;
}
