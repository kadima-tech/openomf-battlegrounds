// The game session (Session module: DE_CStandardSession, DE_CStandardSessionModifier,
// DE_CStandardSessionSlot, DE_CStandardSessionSlotModifier; Core: DE_CSession, DE_CClient).
//
// A session is a set of modifiers -- the game type's master (e.g. LastManStandingMaster) and the
// modifiers it attaches (round identification, ready/fight delay, timer, ...) -- each running its
// own class on itself through a ClassExecutor property. The master's script creates one slot per
// player client; a slot carries copies of the master's slot modifiers (creature and pilot picker,
// scoring, combo counter, ...), which pick, load and own the slot's creature. Everything else --
// rounds, knockouts, winners -- is the scripts' own logic, driven through the hooks they register
// (Begin, Update, UIManagerComplete, ...) and the generic events they send each other.
//
// Only a local, server-side session is modelled: every client is local, nothing is replicated.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "game/runtime.h"

namespace game {

struct ScriptHook {
	Brain *brain = nullptr;
	uint32_t method = 0;  // op 38 method handle
	explicit operator bool() const { return brain && method; }
};

struct SessionSlot;

struct SessionClient {
	GameObject *obj = nullptr;
	bool user = false, local = false, active = false;
	std::string player_name, description;
	SessionSlot *slot = nullptr;
	GameObject *ui_manager = nullptr;  // the UI manager launched on it, if any
};

struct SessionSlot {
	GameObject *obj = nullptr;
	int index = 0;
	std::vector<GameObject *> modifiers;  // slot modifiers, in AddModifier order
	GameObject *creature = nullptr;       // OwnCreature
	SessionClient *client = nullptr;
	bool local = false, remote_controllable = false, connectable = true, begun = false;
};

// A session modifier or slot modifier (its references, hooks and slot).
struct SessionNode {
	GameObject *obj = nullptr;
	std::vector<GameObject *> properties;       // Property(i): executors, slot modifier templates
	std::vector<de::ObjectRef> attached;        // session modifiers the master adds (v3 list)
	std::vector<de::ObjectRef> components;      // slot modifier: actors attached to the creature
	ScriptHook begin, update, set_as_master, ui_activate, ui_complete, state_changed, end;
	SessionSlot *slot = nullptr;  // slot modifiers
	bool initialized = false, begun = false;
};

struct SessionState {
	GameObject *obj = nullptr;
	SessionState *owner = nullptr;          // SubSessionOwner (+0xcc): the session that made this one
	GameObject *master_description = nullptr;  // MasterDescription / OwnMasterDescription
	std::vector<de::Guid> attached_guids;   // AttachedSessionModifierDescriptionGUIDs
	bool single_player = false;             // the session info's +0xdc
	bool ended = false;                     // End has run (EndAndDie)
	std::vector<GameObject *> modifiers;  // master first
	std::vector<std::unique_ptr<SessionSlot>> slots;
	std::vector<std::unique_ptr<SessionClient>> clients;
	std::map<const GameObject *, std::unique_ptr<SessionNode>> nodes;
	GameObject *environment = nullptr;
	uint8_t environment_guid[16] = {};  // CSession +0x38 -> +0xac (SetEnvironmentGUID)
	GameObject *camera = nullptr;
	int max_player_clients = 2;
	bool begun = false;
	std::vector<ScriptHook> slot_connect_hooks, slot_disconnect_hooks;
	// UI managers (description selectors). The menus that would show them are not built yet:
	// a launched UI whose description has a preset choice completes on the next update with it.
	struct UiManager {
		std::string description;        // SetUIDescription: the title
		std::string autoselect;         // SetAutoselectDescription
		std::vector<de::Guid> pool;     // the available descriptions
		bool choose_fighter = false;    // a ChooseFighterUIManager
		std::array<uint8_t, 16> chosen{};
		std::array<uint8_t, 16> model{};  // ChosenModel (zero: the fighter's default)
		bool pending = false;
		bool shown = false;             // launched and not yet completed
	};
	std::map<const GameObject *, UiManager> ui;
	std::map<std::string, de::Guid> ui_choices;  // "Choose Environment" -> description GUID
	bool loaded = false;  // the loading screen's SessionLoadElements has gone out
	std::shared_ptr<struct SessionRecord> record;  // +0x128: recording or replaying (replay.h)
	bool replaying = false;                        // +0xa0

	SessionNode *node(const GameObject *o) const {
		auto it = nodes.find(o);
		return it == nodes.end() ? nullptr : it->second.get();
	}
	SessionSlot *slot_of(const GameObject *o) const {
		for (auto &s : slots) {
			if (s->obj == o) return s.get();
		}
		return nullptr;
	}
	SessionClient *client_of(const GameObject *o) const {
		for (auto &c : clients) {
			if (c->obj == o) return c.get();
		}
		return nullptr;
	}
};

// Create the session with its modifiers (master first; their own attached modifiers follow) and
// an environment (the arena description's resource, or null to let a picker choose).
SessionState &start_session(World &w, const std::vector<de::ObjectRef> &modifiers);
// DE_CAppManager::DestroyAllLocalSessions (Core 0x7019bf00): every session ends and is deleted.
void destroy_all_sessions(World &w);
// The session an object belongs to (a session, its modifiers, slots and clients), if any.
SessionState *session_of(World &w, const GameObject *o);
// DE_CDescription::NewResource: the description's resource (fighter, pilot, environment,
// session modifier...), with a fighter's model/extra actors and the description's values.
GameObject *new_resource(World &w, GameObject *description, const de::Guid *model);
// Add a session modifier (e.g. a description's resource: LMSScoreLimitModifier) before Begin.
void add_session_modifier(World &w, GameObject *modifier);
// DE_CStandardSession::Begin: initialise every modifier (their executors start), then Begin.
void begin_session(World &w);
// Once per frame: every modifier's and slot modifier's Update hook, client/slot connection.
void update_session(World &w);
// Scene render-tick hooks (text fades, HUD, camera): once per rendered frame with the time that
// passed (ms). Headless runs set World::render_ticks_per_frame to tick them with each game frame.
void render_tick(World &w, float ms);
// DE_CSession::NewClient + Activate: a local client (the user's, or a computer player's).
SessionClient &add_client(World &w, bool user, const std::string &name);
// The front end chose `chosen` in a launched UI manager: it completes on the next update, and
// every node's UIManagerComplete hook hears about it.
void complete_ui(World &w, GameObject *ui, const de::Guid &chosen, const de::Guid *model = nullptr);
// DE_CTransferAgent::CPassiveReference: every passive reference's object in an object's contents,
// in order (a description's first is its resource).
std::vector<de::ObjectRef> passive_references(const de::World &data, const de::ObjectRef &r);
// Send a generic event built by `fill` (an event block) to an object (and, for the session, its
// slots): what CEvent::SendToNow does.
uint32_t new_event(World &w, const std::string &handle);

// An object's references as chunks of BOOL-terminated reference lists.
std::vector<std::vector<std::vector<de::ObjectRef>>> reference_lists(const de::World &data, const de::ObjectRef &r);
// CActor::Activate / Deactivate over a branch.

void register_session_natives(World &w);
void register_script_natives(World &w);

}  // namespace game
