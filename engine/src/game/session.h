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
	uint32_t net_id = 0;               // the server's number for it (the roster's client ID)
	// DE_CClient +0x70, +0x74, +0xa4 and its kick votes {1 stay / -1 kick / 0, client ID} (0x0b / 0x0c).
	int32_t ping = -1, admin_level = 0, votes_needed = 0;
	std::vector<std::pair<int32_t, uint32_t>> kick_votes;
	int32_t my_vote = 0;  // this machine's LocalVoteToKick (-1) / LocalVoteToStay (1) on it
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
	// +0x38, the session's info: its values come before the session's own (ObjFindValueByName,
	// 2.112), as DE_CSessionSettings leaves StartingMatch there.
	GameObject *description = nullptr;
	std::vector<de::Guid> attached_guids;   // AttachedSessionModifierDescriptionGUIDs
	bool single_player = false;             // the session info's +0xdc
	bool settings_active = false;           // +0xbc: its Choose Cast page is queued or up, not auto-selecting
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
	int net_status = 1000;  // +0x12c, the client status machine (FINDINGS 403): 1000 Sequenced
	bool scene_started = false;  // its scene's Genesis (OwnEnvironment): DE_CScene +0xf0 clear
	bool watchers_active = false;  // ActivateSceneWatchers: its scene has the audio player (+0x1b8)
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
		uint32_t remote_id = 0;         // a client's copy of a page the server launched (0x03): its ID there
		bool edit_string = false;       // a CEditString (NewEditString)
		std::string input;              // its UserInput, empty when cancelled
	};
	std::map<const GameObject *, UiManager> ui;
	std::map<std::string, de::Guid> ui_choices;  // "Choose Environment" -> description GUID
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
void end_session(World &w, SessionState &s);
void end_session_object(World &w, GameObject *o);
// DE_CScene::PlayNoiseSphere / PlayNoiseAmbient play only in a scene holding an active audio
// player: a session's scene once BRulesDisplay has activated its watchers.
bool scene_audible(World &w, const GameObject *caller);
// The session an object belongs to (a session, its modifiers, slots and clients), if any.
SessionState *session_of(World &w, const GameObject *o);
// DE_CDescription::NewResource: the description's resource (fighter, pilot, environment,
// session modifier...), with a fighter's model/extra actors and the description's values.
GameObject *new_resource(World &w, GameObject *description, const de::Guid *model);
// DE_CSession::AreMasterRequirementsSatisfied's test (Core 2.123 0x20201ba0): every cast the
// master description requires is a member cast of one of the session's modifiers, those its
// attached modifier descriptions make included (AssureSessionModifiersCreated).
bool master_requirements_met(World &w, const SessionState &s);
// Add a session modifier (e.g. a description's resource: LMSScoreLimitModifier) before Begin.
void add_session_modifier(World &w, GameObject *modifier);
// DE_CStandardSession::Begin: initialise every modifier (their executors start), then Begin.
void begin_session(World &w);
// Once per frame: each session's scene passes its time, then every modifier's and slot modifier's
// Update hook runs and clients connect to slots.
void update_session(World &w);
// The end of a scene's PassTime: its delta-tick hooks and delta-ticked creatures.
void scene_delta_tick(World &w, const GameObject *session, float ms);
// Scene render-tick hooks (text fades, HUD, camera): once per rendered frame with the time that
// passed (ms). Headless runs set World::render_ticks_per_frame to tick them with each game frame.
void render_tick(World &w, float ms);
// DE_CSession::NewClient + Activate: a local client (the user's, or a computer player's).
SessionClient &add_client(World &w, bool user, const std::string &name);
SessionClient &add_client_to(World &w, SessionState &s, bool user, const std::string &name);
// DE_CStandardSessionSlot::Attach: the client takes the slot (its connect hooks and SessionSlotNewClient).
void connect_client(World &w, SessionState &s, SessionClient &c, SessionSlot &slot);
// DE_CSession::ClientDisconnected and SymbiotSlotClientDisconnect (FINDINGS 407): the slot sends
// itself SessionSlotResetAll, runs the disconnect hooks, and the client goes.
void drop_client(World &w, SessionState &s, SessionClient &c);
// DE_CStandardSession::StatusDescription (Session 0x4500fbf0).
const char *net_status_description(int status);
// The top-level session `s` belongs to (whose status the machine keeps).
SessionState *master_session(SessionState *s);
// IsJoinable: the session belongs to a network session (hosted or joined).
bool session_joinable(World &w, SessionState *s);
// The front end chose `chosen` in a launched UI manager: it completes on the next update, and
// every node's UIManagerComplete hook hears about it.
void complete_ui(World &w, GameObject *ui, const de::Guid &chosen, const de::Guid *model = nullptr);
// RemoteUIManagerActivated (Session 0x45016290): the UIManagerActivate hooks of the session's and the client's slot's modifiers.
void remote_ui_activated(World &w, SessionState &s, GameObject *ui, SessionClient *c);
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
