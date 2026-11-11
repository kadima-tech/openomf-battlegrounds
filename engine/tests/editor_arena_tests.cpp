// The content editor's arena path, on the game's Data folder: the playable arenas, the Desert's
// environment tree as the viewport reads it, then an add-on variant -> edits (description, a spawn
// point moved and one added, a light recoloured) -> save -> reopen -> the same values, and the
// engine reading the variant through OPENBG_ADDONS with every reference landing where the base's
// did. Usage: editor-arena-tests <Data dir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <cstring>
#include <functional>

#include "addon.h"
#include "arena.h"
#include "content.h"
#include "de/lighting.h"
#include "de/scene.h"
#include "game/session.h"
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

static void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

static std::map<ed::NodeKind, int> kinds(const ed::ArenaScene &s) {
	std::map<ed::NodeKind, int> out;
	for (auto &n : s.nodes) ++out[n.kind];
	return out;
}

static uint32_t environment_index(const de::ObjectsFile &f, const std::string &name = "") {
	for (uint32_t i = 0; i < f.objects().size(); ++i) {
		if (f.objects()[i].class_path == "Actor\\Creature\\Environment" && (name.empty() || f.objects()[i].name == name)) return i;
	}
	return 0;
}

static std::string key_of(const de::ObjectRef &r) { return r ? std::string(reinterpret_cast<const char *>(r.info().guid.bytes), 16) : std::string(); }

// What the engine's own readers take from an object's references, by class.
static std::vector<std::string> engine_targets(const de::World &w, const de::ObjectRef &r) {
	std::vector<std::string> out;
	const std::string &cls = r.info().class_path;
	auto lists = game::reference_lists(w, r);
	if (cls == "Actor" || cls.rfind("Actor\\", 0) == 0) {
		if (lists.size() > 1 && !lists[1].empty()) {
			for (auto &t : lists[1][0]) out.push_back(key_of(t));
		}
	}
	if (cls == "UniqueActorProperty\\NavigationLink" && lists.size() > 2 && !lists[2].empty() && !lists[2][0].empty()) out.push_back(key_of(lists[2][0][0]));
	if (cls == "Actor\\Creature\\Environment") {
		for (auto &t : de::read_cube_faces(w, r)) out.push_back(key_of(t));
	}
	if (cls == "Light") {
		if (auto l = de::read_light(w, r)) out.push_back(key_of(l->image));
	}
	if (cls == "TextureProcess\\MultiStage") {
		auto ms = de::read_multistage(r.file->object_data(r.info()));
		for (auto &st : ms.stages) out.push_back(key_of(w.resolve(*r.file, st.manager))), out.push_back(key_of(w.resolve(*r.file, st.colour_value)));
	}
	if (cls.rfind("Mesh\\", 0) == 0) {
		for (auto &m : de::read_materials(w, r)) {
			for (auto &l : m.layers) out.push_back(key_of(l.image));
		}
	}
	return out;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir>\n", argv[0]);
		return 2;
	}
	const fs::path data = argv[1];
	ed::BaseData base(data);
	fs::path tmp = fs::temp_directory_path() / ("openbg-arena-test-" + std::to_string(std::rand()) + std::to_string(time(nullptr)));
	fs::create_directories(tmp);

	auto arenas = ed::base_arenas(base);
	int listed = 0;
	for (auto &a : arenas) printf("  arena %-20s %-16s%s\n", a.name.c_str(), a.stem.c_str(), a.listed ? " listed" : ""), listed += a.listed;
	CHECK(listed >= 11);
	const ed::ArenaEntry *desert = nullptr;
	for (auto &a : arenas) {
		if (a.stem == "Desert") desert = &a;
	}
	CHECK(desert != nullptr);
	if (!desert) return 1;

	auto files = ed::arena_files(base, "Desert");
	std::set<std::string> keep(files.begin(), files.end());
	CHECK(keep.count("Desert") && keep.count("SharedObjects"));
	CHECK(files.size() < base.files().size() / 2);

	std::array<float, 3> moved_to{0, 0, 0};
	ed::LightData lit;
	size_t base_spawns = 0;
	{
		de::World w(data, [&](const std::string &s) { return keep.count(s) > 0; });
		const de::ObjectsFile *f = w.file("Desert");
		CHECK(f != nullptr);
		if (!f) return 1;
		ed::ArenaScene s = ed::read_arena_scene(w, *f, environment_index(*f));
		auto k = kinds(s);
		for (auto &[kind, n] : k) printf("  %-16s %d\n", ed::node_kind_name(kind), n);
		base_spawns = size_t(k[ed::NodeKind::SpawnPoint]);
		CHECK(base_spawns == 16);
		CHECK(k[ed::NodeKind::PickupSpawner] >= 6);
		CHECK(k[ed::NodeKind::NavArea] >= 10);
		CHECK(k[ed::NodeKind::Surface] >= 1);
		CHECK(k[ed::NodeKind::Light] >= 1);
		int linked = 0;
		for (int l : s.of_kind(ed::NodeKind::NavLink)) linked += s.nodes[size_t(l)].nav_to >= 0;
		CHECK(linked == k[ed::NodeKind::NavLink]);
		for (auto &i : ed::validate_arena(s, 10)) {
			printf("  [desert] %s\n", i.text.c_str());
			CHECK(i.severity != ed::Issue::Error);
		}
	}

	// A variant: copy -> edit -> save -> reopen.
	const fs::path addon = tmp / "ArenaMod.DEObjects";
	{
		auto p = ed::Project::create(base, addon);
		uint32_t d = ed::create_arena_variant(*p, *desert, "Desert at Night");
		auto mine = ed::addon_arenas(*p);
		CHECK(mine.size() == 1);
		if (!mine.empty()) CHECK(mine[0].stem == "Desert" && mine[0].name == "Desert at Night");
		ed::ArenaDescriptionData dd = ed::read_arena_description(*p, d);
		CHECK(dd.name == "Desert at Night" && dd.listed && !dd.locked);
		CHECK(!dd.casts.empty());
		dd.min_players = 2, dd.max_players = 6;
		ed::write_arena_description(*p, d, dd);
		int spawn = -1, light = -1;
		for (uint32_t i = 0; i < p->file().objects.size(); ++i) {
			const auto &info = p->file().objects[i].info;
			if (spawn < 0 && info.name_text() == "PlayerSpawnPoint01") spawn = int(i);
			if (light < 0 && info.class_text() == "Light") light = int(i);
		}
		CHECK(spawn >= 0 && light >= 0);
		if (spawn >= 0) {
			de::Transform t = ed::actor_local(p->file().objects[size_t(spawn)]);
			t.t[0] += 40;
			moved_to = t.t;
			ed::set_actor_local(*p, uint32_t(spawn), t);
			uint32_t made = ed::duplicate_actor(*p, uint32_t(spawn), "PlayerSpawnPoint17");
			CHECK(ed::addon_parent(*p, made) == ed::addon_parent(*p, uint32_t(spawn)));
		}
		if (light >= 0) {
			CHECK(ed::read_light_data(p->file().objects[size_t(light)], &lit));
			lit.rgba = {0.2f, 0.3f, 0.9f, 1.0f}, lit.intensity = 0.75f;
			ed::write_light_data(*p, uint32_t(light), lit);
		}
		for (auto &i : ed::validate(*p)) {
			if (i.severity == ed::Issue::Error) printf("  [variant] %s\n", i.text.c_str());
			CHECK(i.severity != ed::Issue::Error);
		}
		p->save();
	}
	{
		auto p = ed::Project::open(base, addon);
		auto mine = ed::addon_arenas(*p);
		CHECK(mine.size() == 1);
		int d = mine.empty() ? -1 : p->find(mine[0].description);
		CHECK(d >= 0);
		if (d >= 0) {
			auto dd = ed::read_arena_description(*p, uint32_t(d));
			CHECK(dd.name == "Desert at Night" && dd.min_players == 2 && dd.max_players == 6);
		}
		for (uint32_t i = 0; i < p->file().objects.size(); ++i) {
			const auto &o = p->file().objects[i];
			if (o.info.name_text() == "PlayerSpawnPoint01") CHECK(ed::actor_local(o).t == moved_to);
			if (o.info.class_text() == "Light") {
				ed::LightData got;
				CHECK(ed::read_light_data(o, &got) && got == lit);
				break;
			}
		}
	}

	// The engine's view of the saved variant.
	{
		set_env("OPENBG_ADDONS", addon.string());
		de::World w(data, [&](const std::string &s) { return keep.count(s) > 0; });
		set_env("OPENBG_ADDONS", "");
		const de::ObjectsFile *f = w.file("ArenaMod");
		const de::ObjectsFile *bf = w.file("Desert");
		CHECK(f != nullptr && bf != nullptr);
		if (f && bf) {
			ed::ArenaScene v = ed::read_arena_scene(w, *f, environment_index(*f));
			ed::ArenaScene b = ed::read_arena_scene(w, *bf, environment_index(*bf));
			auto kv = kinds(v), kb = kinds(b);
			if (getenv("ARENA_DEBUG")) {
				for (auto *ff : {bf, f}) {
					for (uint32_t i = 0; i < ff->objects().size(); ++i) {
						if (ff->objects()[i].name != "SUN Light") continue;
						auto d = ff->object_data(ff->objects()[i]);
						printf("%s flags %u contents %s\n  refs %s\n", ff->master().name.c_str(), ff->objects()[i].flags, deo::to_hex(d.contents).c_str(), deo::to_hex(d.references).c_str());
					}
				}
			}
			CHECK(kv[ed::NodeKind::SpawnPoint] == int(base_spawns) + 1);
			CHECK(kv[ed::NodeKind::NavArea] == kb[ed::NodeKind::NavArea]);
			CHECK(kv[ed::NodeKind::Surface] == kb[ed::NodeKind::Surface]);
			for (auto &i : ed::validate_arena(v, 6)) {
				printf("  [variant] %s\n", i.text.c_str());
				CHECK(i.severity != ed::Issue::Error);
			}
			// Every reference of a copied object reaches the base object's target, or its copy.
			std::map<std::string, std::string> copy_of;  // base guid -> copy guid, matched by tree position
			std::function<void(int, int)> pair = [&](int vb, int bb) {
				copy_of[std::string(reinterpret_cast<const char *>(b.nodes[size_t(bb)].guid.b), 16)] =
				    std::string(reinterpret_cast<const char *>(v.nodes[size_t(vb)].guid.b), 16);
				const auto &vc = v.nodes[size_t(vb)].children, &bc = b.nodes[size_t(bb)].children;
				for (size_t k = 0; k < std::min(vc.size(), bc.size()); ++k) pair(vc[k], bc[k]);
			};
			pair(v.root, b.root);
			size_t compared = 0, differ = 0;
			for (auto &bn : b.nodes) {
				auto it = copy_of.find(std::string(reinterpret_cast<const char *>(bn.guid.b), 16));
				if (it == copy_of.end()) continue;
				int vi = -1;
				for (size_t k = 0; k < v.nodes.size() && vi < 0; ++k) {
					if (std::string(reinterpret_cast<const char *>(v.nodes[k].guid.b), 16) == it->second) vi = int(k);
				}
				if (vi < 0) continue;
				const de::ObjectRef vr = v.nodes[size_t(vi)].ref;
				auto tb = engine_targets(w, bn.ref), tv = engine_targets(w, vr);
				for (auto &t : tb) {
					if (copy_of.count(t)) t = copy_of[t];
				}
				++compared;
				if (tb != tv) {
					++differ;
					if (differ < 5) {
						printf("  references differ: %s (%s) %zu vs %zu\n", bn.name.c_str(), bn.class_path.c_str(), tb.size(), tv.size());
						auto nm = [&](const std::string &t) -> std::string {
							if (t.empty()) return "null";
							de::Guid g;
							memcpy(g.bytes, t.data(), 16);
							auto r = w.by_guid(g);
							return r ? r.info().name + "@" + r.file->master().name : "?";
						};
						for (size_t q = 0; q < std::max(tb.size(), tv.size()); ++q) {
							if (q < tb.size() && q < tv.size() && tb[q] == tv[q]) continue;
							printf("     %s | %s\n", q < tb.size() ? nm(tb[q]).c_str() : "-", q < tv.size() ? nm(tv[q]).c_str() : "-");
						}
					}
				}
			}
			printf("  %zu copied objects compared, %zu differ\n", compared, differ);
			CHECK(compared > 300 && differ == 0);
		}
	}

	std::error_code ec;
	fs::remove_all(tmp, ec);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("editor arena path ok\n");
	return 0;
}
