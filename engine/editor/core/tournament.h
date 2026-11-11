// A tournament as the editor sees it: a Description\SessionMaster whose session master runs BMaster
// and whose sub-session handler runs BSubSession (one state per match). The editor keeps a model of
// the ladder (stored as JSON in a Value\String under the tournament's folder) and generates both
// scripts from it, in the shipped scripts' patterns; code inside "// <user ...>" blocks is kept.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "addon.h"
#include "de/pcode.h"

namespace ed {

struct TQuote {
	int slot = 1;  // 0 is the player
	std::string text;  // lines separated by "\n" (stored as <Return>)
	bool operator==(const TQuote &) const = default;
};

struct TOpponent {
	Guid pilot;    // Description\Pilot
	Guid robot;    // Description\Creature\Fighter; zero: a random robot (CreatureListGetRandom)
	Guid faction;  // Description\Faction, team games only
	bool operator==(const TOpponent &) const = default;
};

struct TDropOn {
	int slot = 0;
	Guid actor;
	float delay = 0;
	bool operator==(const TDropOn &) const = default;
};

// A method kept as written (imported from a base tournament): its qualified name, e.g.
// "MatchSearching.PlayerWon", and how it is declared.
struct TMethod {
	std::string name, source;
	bool state = false, virtual_ = false;
	bool operator==(const TMethod &) const = default;
};

constexpr int kNextMatch = -1;   // on win: the next match in the list
constexpr int kReplay = -1;      // on lose: play the match again
constexpr int kEnding = 1000;    // the ending cinematic

struct TMatch {
	std::string key;  // the script names: match<Key> and Match<Key>()
	std::string name, description, intro;
	std::vector<TOpponent> opponents;  // slots 1..
	Guid player_faction, enemy_faction;  // team games: SubSessionSetFactions
	Guid arena;  // Description\Environment; zero: random
	std::vector<TQuote> pre_quotes, win_quotes, lose_quotes;
	std::string win_commentary, lose_commentary;
	std::vector<TDropOn> drop_ons;
	int on_win = kNextMatch, on_lose = kReplay;
	int ai_offset = 0;
	bool operator==(const TMatch &) const = default;
};

struct TUnlock {
	Guid description;
	std::string text;
	bool operator==(const TUnlock &) const = default;
};

struct Tournament {
	std::string name, choose_text;
	bool listed = true, locked = false;
	Guid game_type;    // a Description\SessionMaster of Session (Demolition, Last Man Standing, ...)
	Guid score_limit;  // a Description\SessionModifier of Session
	float ai_difficulty = 0;
	std::vector<Guid> excluded_pilots, player_pilots, player_robots;
	std::vector<TMatch> matches;
	Guid ending_arena;
	std::string ending_name, ending_description;
	std::vector<std::string> ending_quotes;
	std::vector<TUnlock> unlocks;
	std::vector<TMethod> extra;  // further methods, kept verbatim
	std::string image_source, icon_source;
	bool operator==(const Tournament &) const = default;
};

struct TournamentObjects {
	int folder = -1, description = -1, matches = -1, model = -1;
	int master = -1, master_class = -1, bmaster = -1;
	int handler_description = -1, sub_master = -1, sub_class = -1, bsub = -1;
	int creator = -1, text = -1, image = -1, icon = -1;
	int value(const Project &p, const char *name) const;  // a Value\ObjectReference under the master
};

// The base game's tournaments (descriptions whose session master runs a BMaster).
std::vector<const BaseObject *> base_tournaments(const BaseData &base);
// The add-on's tournaments (those with a model).
std::vector<uint32_t> addon_tournaments(const Project &p);
TournamentObjects tournament_objects(const Project &p, uint32_t description);

// New tournament: a copy of a base tournament's description, session master, sub-session handler,
// scripts and Choose Match panel with fresh GUIDs, under a new folder; its ladder is read from
// the base scripts (blank: one match against a random opponent instead). Returns the description.
uint32_t create_tournament(Project &p, const BaseObject &base_description, const std::string &name, bool blank);

Tournament read_tournament(const Project &p, const TournamentObjects &o);
// Writes the model, the description and panel data, the session master's values and both
// generated scripts. Compiling is separate (ScriptCompiler).
// `blocks` replaces the user blocks the scripts hold now.
void write_tournament(Project &p, const TournamentObjects &o, const Tournament &t,
                      const std::map<std::string, std::string> *blocks = nullptr);

struct GeneratedScripts {
	de::PCode master, sub;
	std::vector<Guid> master_globals, sub_globals;
};
GeneratedScripts generate_scripts(const Project &p, const TournamentObjects &o, const Tournament &t);

// "// <user id>" ... "// </user>" blocks of a source, by id.
std::vector<std::pair<std::string, std::string>> user_blocks(const std::string &source);

// Choices for the editor's dropdowns (base and add-on objects of a class), by name.
std::vector<std::pair<std::string, Guid>> class_choices(const Project &p, const std::string &class_path);
std::string match_label(const Tournament &t, int target, bool lose);
// Actors the base tournaments drop on fighters (their BSubSession globals' actors).
std::vector<std::pair<std::string, Guid>> drop_on_choices(const BaseData &base);
// A script text with its user blocks' contents taken out (what the generator owns).
std::string without_user_blocks(const std::string &text);

}  // namespace ed
