// openbg-unlock-all: writes a registry file with everything the game locks unlocked, for
// playtesting without playing through first. The unlocks are the registry's DLock records
// (FINDINGS 158, 220, 258), the same ones play writes:
//   - id 0 on every description that is locked by default (robots, pilots, arenas, modes...);
//   - ids 100-121 on every pilot description, its 22 RECON DATA stats;
//   - ids 0x10063 + i on every session master with a Matches list, its starting matches.
// The host's arena toggle (id 0xffff) is left alone: it is a setting, not progress.
//
// Usage: openbg-unlock-all <Data dir> <out registry.txt>
// An existing output file keeps its other values (settings), so it can be pointed at a player's
// own registry.txt.
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

#include "game/descriptions.h"
#include "game/runtime.h"

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s <Data dir> <out registry.txt>\n", argv[0]);
		return 2;
	}
	game::World w(argv[1]);
	{
		std::ifstream in(argv[2], std::ios::binary);
		std::string l;
		while (std::getline(in, l)) {
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
			size_t eq = l.find('=');
			if (eq != std::string::npos) w.registry[l.substr(0, eq)] = l.substr(eq + 1);
		}
	}
	game::migrate_lock_records(w.registry);
	int descriptions = 0, pilots = 0, matches = 0;
	for (auto &[stem, f] : w.data().files()) {
		for (uint32_t i = 0; i < f->objects().size(); ++i) {
			const de::ObjectInfo &info = f->objects()[i];
			if (info.class_path.rfind("Description", 0) != 0) continue;
			de::ObjectRef d{f.get(), i};
			if (game::description_locked(w, d, 0)) {
				game::unlock_description(w, d, 0);
				++descriptions;
			}
			if (info.class_path == "Description\\Pilot") {
				for (uint32_t k = 0; k < 22; ++k) {
					if (game::description_locked(w, d, 100 + k)) game::unlock_description(w, d, 100 + k);
				}
				++pilots;
			}
			if (info.class_path == "Description\\SessionMaster") {
				game::GameObject *md = w.global_object(d);
				auto *v = md ? w.object_value(md, "Matches") : nullptr;
				if (!v) continue;
				// One match per line; entry 0 is always available (DE_CSessionSettings' Starting Match).
				size_t lines = 1;
				for (char c : v->text) lines += c == '\n';
				for (uint32_t m = 1; m < lines; ++m) {
					if (game::description_locked(w, d, 0x10063 + m)) {
						game::unlock_description(w, d, 0x10063 + m);
						++matches;
					}
				}
			}
		}
	}
	std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
	if (!out) {
		fprintf(stderr, "cannot write %s\n", argv[2]);
		return 1;
	}
	for (auto &[k, v] : w.registry) out << k << '=' << v << '\n';
	printf("unlocked %d descriptions, RECON DATA for %d pilots, %d starting matches -> %s\n", descriptions, pilots, matches, argv[2]);
	return 0;
}
