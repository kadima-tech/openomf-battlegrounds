// Session recordings ("replays", FINDINGS 137): DE_CSessionRecord, DE_CStateManager and their
// playback. A recording is the state history of every state manager in a recorded session: for
// each manager (matched by its ID), the states its owner packed when its scripts called
// Register*State. Playback rebuilds the session from its master description and lets every
// object whose manager matches a recorded one be driven by those states.
#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "de/scene.h"

namespace game {

class World;
struct GameObject;
struct SessionState;

// DE_CStateManagerRecord: one manager's recorded states, in time order.
struct StateRecord {
	std::string id;
	std::vector<std::pair<uint32_t, std::vector<uint8_t>>> states;
	size_t cursor = 0;
	bool rebased = false;  // an earlier port build's scene-time stamps, moved to the session's clock
};

// DE_CSessionRecord.
struct SessionRecord {
	std::string name;          // the master object's name (the server name)
	de::Guid master{};         // +0x2c: the session master description
	std::vector<de::Guid> modifiers;  // +0x3c
	de::Guid factory{};        // +0x58: the session's class
	uint32_t end_ms = 0;       // +0x6c: scene ms at the end
	// Port builds before the per-session scene clocks stamped scene times on one shared clock: the
	// recorded session's Begin on it, found at playback (0 for every other recording).
	uint32_t shared_clock_begin = 0;
	int64_t date = 0;          // +0x70: time(NULL) when saved
	int32_t auto_delete = 1;   // +0x74
	float length = 0;          // +0x78: seconds
	std::string player, server;  // +0x7c, +0x84
	bool recording = false;    // +0x68
	std::vector<std::unique_ptr<StateRecord>> managers;  // +0x4c
	// Its .DEObjects file: the path, the agent (master entry) and the record's own GUID.
	std::string path, agent;
	de::Guid agent_guid{}, guid{};

	StateRecord *find(const std::string &id) const;
};

// The recordings in the data files and the Recordings folder (the Replays screen's list), newest
// first; headers only.
std::vector<std::shared_ptr<SessionRecord>> list_recordings(const de::World *data = nullptr);
std::shared_ptr<SessionRecord> load_recording(const std::string &path);
bool delete_recording(const SessionRecord &r);
bool rename_recording(SessionRecord &r, const std::string &name);
std::filesystem::path recordings_dir();

// DE_CSessionMasterDescription +0xfc (contents v1): the game type is recorded.
bool recordable(const de::ObjectRef &master_description);

// DE_CSession::Begin / End (Core 0x701f9090 / 0x701f9400): the recorder starts when the option is on,
// the session is the server and its master description is recordable; it is closed and saved at
// the end.
void replay_session_begun(World &w, SessionState &s);
void replay_session_ended(World &w, SessionState &s);
// DE_CSessionRecord::Replay (Core 0x70203e30): a server session from the record's master
// description and modifiers, replaying it. Returns false when the description is missing.
bool start_replay(World &w, const std::shared_ptr<SessionRecord> &rec);
// DE_CSession::Update before its scene passes: managers join, the recorder's end time follows the
// scene, a replay past its end ends, and a replay's managers fetch their newest recorded state.
void replay_update(World &w);
// After the scene's pass: a rewind's run-time actors go, and each manager whose fetch added a
// state restores its newest (NeedFixup, SetupOwnerForState).
void replay_after_pass(World &w);
// The session being recorded, replayed or played over the network, if any.
SessionState *replay_session(World &w);
// Register*State (CreateCurrentState): the owner's current state joins its manager's history,
// to be recorded as it ages out; RegisterGetHitState also marks the fighter hit.
// `authority` and `significance` (4: the manager's defaults) matter only to network play.
void register_state(World &w, GameObject *owner, bool got_hit = false, int authority = 4, int significance = 4);
// The pre-destruction of `o` (and its branch): their managers' histories go into the record.
void replay_destroying(World &w, GameObject &o);
// CCreature::SetInstanceNumber: part of the creature's ID.
void set_instance_number(World &w, GameObject &o, int32_t n);
// DE_CStateManager +0x78: driven by a record (IsSimulating / IsStateAlterable false).
bool replay_driven(World &w, const GameObject *o);
// DE_CCreature::Tick at its start: true when the creature skips its physics and brain this tick
// (held since a replay's clock went back, or re-taking a state of this tick).
bool creature_state_tick(World &w, GameObject &o);
// DE_CCreature::Tick after the state-changed hooks, before the tick count goes up: a hold ends
// once the clock is back where it was (AssureSequenced).
void creature_state_tick_end(World &w, GameObject &o);
// DE_CActor::CanStateInterrupt (vtable +0x138 in 2.123): may a lagged state in `next` replace a
// newer no-interrupt one in `current` (network play only; replays never ask).
bool can_state_interrupt(const GameObject &actor, const std::string &current, const std::string &next);
// DE_CCreature::ObjPrepare: a creature with no tick rate joins the scene's delta-tick list when
// its +0x1e0 is set (none in the shipped data).
bool creature_delta_ticked(const GameObject &o);
// DE_CCreature::DeltaTick (Creature 0x15016700), at the end of each scene pass.
void creature_delta_tick(World &w, GameObject &o);
// Held (+0x2fc == 2): a fighter's guide and trailers wait too.
bool replay_held(World &w, const GameObject &o);
// DE_CCreature::SetTime after the replay's clock jumped: re-ticks up to it or rewinds.
void replay_set_time(World &w, GameObject &o, uint32_t ms);
// DE_CScene::TemporalUpdate (Core 0x70070710): restored creatures of `session`'s scene whose SetTime was
// deferred.
void replay_temporal_update(World &w, const GameObject *session);
bool replaying(World &w);
// The replay's recorded end (scene ms), 0 when not replaying.
uint32_t replay_end_ms(World &w);
void forget_replay_objects(World &w);

}  // namespace game
