// Network play: the original's state replication (docs/NETWORK_PLAN.md) on our own transport.
//
// Every machine simulates every creature; only the owner of a creature runs its controlling
// (`master`) code: a client its own fighter, the server everything else. When scripts call
// Register*State the state joins its manager's history; the sender (DE_CStandardClientComInterface
// Version1::ReplicateSessionStateManagerStates, Session 0x4500a020) sends a client's own fighter's
// states to the server and the server's states to every client, re-sending each on a ladder. A
// receiver inserts a state at its time in the past (AddLaggedState, Core 0x701ae760), arbitrating
// by authority, significance and flags, and re-simulates that one creature up to now.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/link.h"

namespace game {

class World;
struct GameObject;
struct SessionState;

enum class NetRole : uint8_t { None, Server, Client };

// What the receiving side did with states, for the harness's measurements.
struct NetCounters {
	uint64_t received = 0, inserted = 0, rejected = 0, held = 0, held_forced = 0, discarded = 0, same = 0;
	uint64_t rollbacks = 0, rollback_ticks = 0;  // fixups that restored a state, and the ticks they re-simulated
	uint64_t corrections_in = 0;                 // received states flagged Correction that were inserted
	uint64_t registered = 0;
};

struct NetConfig {
	int budget = 16384;  // bytes a second to each peer (the original's 16 KB/s)
	float quality = 0.5f;  // the net node's vtable +0x58 (assumed; 0.5 makes the ladder's divisor 1)
};

// --- The state managers' network side (replay.cpp) ------------------------------------------

// A network session's managers join on this machine's side (`s` is the session they belong to).
void net_managers_begin(World &w, NetRole role, SessionState *s);
void net_managers_end(World &w);
NetRole net_role(World &w);
NetCounters &net_counters(World &w);

struct NetManager {
	GameObject *owner = nullptr;
	std::string id;          // DE_CStateManager::BuildID
	uint32_t server_id = 0;  // the server's numeric ID (0 on a client until linked)
	bool up = false, down = true;
};
std::vector<NetManager> net_managers(World &w);
// A client learns the server's ID for a manager ID; false while no manager has it yet.
bool net_link(World &w, uint32_t server_id, const std::string &id);
GameObject *net_owner(World &w, uint32_t server_id);
uint32_t net_server_id(World &w, const GameObject *owner);

struct NetState {
	uint32_t time = 0;
	uint8_t authority = 0, significance = 0;
	uint32_t flags = 0;
	uint64_t serial = 0;  // this machine's, to tell states apart
	std::vector<uint8_t> bytes;
};
// The newest state of `owner`'s manager the sender may replicate (not NoReplicate), packed for the
// wire; false when it has none.
bool net_newest_state(World &w, const GameObject *owner, NetState &out);
// A state received for `owner`'s manager (AddLaggedState with the packed flags).
void net_receive_state(World &w, GameObject *owner, const NetState &st);
// DE_CStateManager::SetCurrentLag: lag in scene ms (10 ms + half the ping at the scene's scale).
void net_set_lag(World &w, float lag_ms);
// The flags RegisterLastState* put on the state the owner's manager made last.
enum class LastState { NoInterrupt, NoReplicate, TimeCritical, Correction, RequiredCorrection };
void register_last_state(World &w, GameObject *owner, LastState what);
// DE_CStandardSessionSlot::SetupCreatureControl's creature +0x1f8.
bool locally_controlled(World &w, const GameObject *creature);
void set_locally_controlled(World &w, GameObject *creature, bool on);

// --- The session's network side (netplay.cpp) -----------------------------------------------

// One machine's side of a network session. The server has one peer per client, each owning the
// creature of the slot it plays; a client has one peer, the server.
class NetSession {
public:
	struct Peer {
		net::Endpoint *link = nullptr;
		GameObject *creature = nullptr;  // the creature this peer's machine controls (server side)
		float ping = 0;                  // round trip, ms
		uint32_t last_ping_sent = 0;
		uint64_t bytes_sent = 0;
		float tokens = 0;
		struct Sent {
			uint64_t serial = 0;
			uint32_t time = 0;
			int step = 0;           // the ladder's rung (CSignificantState +0xc)
			uint32_t last_ms = 0;   // +0x20
			bool any = false;
		};
		std::map<const GameObject *, Sent> sent;
		std::map<uint32_t, bool> linked;  // server: IDs announced to this peer
	};
	NetSession(World &w, NetRole role, NetConfig cfg = {});
	~NetSession();
	Peer &add_peer(net::Endpoint *link, GameObject *creature = nullptr);
	// Once per frame, after World::tick: receive, keep the lag and clock, send.
	void update();
	// The frame's time for World::tick on a client: the real time plus the clock's slew.
	uint32_t frame_ms(uint32_t real_ms);
	NetRole role() const { return role_; }
	std::deque<Peer> &peers() { return peers_; }
	// Bytes sent by this side, and state packets.
	uint64_t bytes_sent() const;
	uint64_t states_sent = 0, clock_jumps = 0;
	int32_t slew_pending = 0;
	float lag_ms = 10;

private:
	void receive(Peer &p, const net::Packet &packet);
	void send_states(Peer &p, uint32_t now);
	void announce(Peer &p);
	World &w_;
	NetRole role_;
	NetConfig cfg_;
	std::deque<Peer> peers_;
	uint32_t last_lag_ = 0;
	uint8_t sync_seq_ = 0;
};

}  // namespace game
