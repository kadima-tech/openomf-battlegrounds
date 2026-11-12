// The game's side of the lobby protocol (server/lobby, docs/NETWORK_PLAN.md 7.8): a host's listing
// and heartbeat, the session list, and the hole-punch introduction with its relay fallback. A host
// and a joining client speak to the lobby from their game socket, so the address the lobby sees is
// the NAT mapping their game traffic uses.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "net/enet_link.h"
#include "net/secure.h"

namespace net {

constexpr uint16_t kLobbyPort = 27900;

enum class LobbyMsg : uint8_t {
	Register = 1, Challenge, Registered, Unregister, List, ListReply, Connect, Intro, Peer, Punch, RelayRequest, Relay, Bind, Bound, Error
};

struct LobbyInfo {
	uint16_t protocol = 0;
	std::string version;
	std::array<uint8_t, 16> data_hash{};
	std::string server_name, session_name;
	uint8_t players = 0, max_players = 0, flags = 0;  // flags: 1 password, 2 playing
	Address local{0, 0};
	Key32 identity{};
	uint32_t instance = 0;
};

struct LobbyEntry {
	uint32_t id = 0;
	Address public_addr{0, 0};
	LobbyInfo info;
};

std::vector<uint8_t> lobby_message(LobbyMsg m);
bool lobby_parse(const uint8_t *d, size_t n, LobbyMsg &type, size_t &body);
bool is_lobby_datagram(const uint8_t *d, size_t n);

// A host's listing: registers from the game socket, answers the challenge, re-sends every
// heartbeat, punches towards the clients the lobby introduces and binds to the relays it opens.
class LobbyHost {
public:
	LobbyHost(EnetHost &host, Address lobby);
	~LobbyHost();
	std::function<LobbyInfo()> info;
	// Call every frame.
	void update();
	bool listed() const { return id_ != 0; }
	uint32_t id() const { return id_; }
	std::string status;  // for the log
	// The client a relay port stands for, so bans and reconnects see the player's own address.
	std::optional<Address> relayed_client(const Address &relay) const;

private:
	bool raw(const Address &from, const uint8_t *d, size_t n);
	void send_register();
	EnetHost &host_;
	int handler_ = 0;
	Address lobby_;
	uint64_t cookie_ = 0;
	uint32_t id_ = 0, last_sent_ = 0, interval_ = 5000, last_heard_ = 0;
	struct Punch {
		std::vector<Address> to;
		uint64_t nonce;
		uint32_t until, last = 0;
	};
	std::vector<Punch> punches_;
	struct Binding {
		Address relay, client;
		uint64_t token;
		uint32_t last = 0;
		bool bound = false;
	};
	std::vector<Binding> bindings_;
};

// Joining through the lobby: the introduction, the punch and the ENet connect to the host's
// address; after kPunchTime without a connection, the relay.
class LobbyJoin {
public:
	enum class Phase { Introducing, Punching, Relaying, Connected, Failed };
	LobbyJoin(EnetHost &host, Address lobby, const LobbyEntry &target, bool force_relay = false);
	~LobbyJoin();
	void update();
	// The ENet connection's handshake finished.
	void connected();
	Phase phase() const { return phase_; }
	std::string error;
	bool relayed() const { return relayed_; }
	static constexpr uint32_t kPunchTime = 3000, kGiveUp = 15000;

private:
	bool raw(const Address &from, const uint8_t *d, size_t n);
	void connect(const Address &to);
	EnetHost &host_;
	int handler_ = 0;
	Address lobby_;
	LobbyEntry target_;
	bool force_relay_, relayed_ = false;
	Phase phase_ = Phase::Introducing;
	uint64_t nonce_ = 0;
	uint32_t started_, phase_at_, last_sent_ = 0;
	std::vector<Address> punch_to_;
	Address relay_{0, 0};
	uint64_t token_ = 0;
	bool bound_ = false;
};

// The Join Game page's lobby list: pages of the listing every few seconds, from its own socket.
class LobbyList {
public:
	// Only sessions of network protocol `protocol` are listed.
	LobbyList(Address lobby, uint16_t protocol, uint32_t refresh_ms = kRefresh);
	~LobbyList();
	bool ok() const { return socket_ >= 0; }
	void refresh();  // asks for the list again
	// True when the list changed.
	bool update();
	const std::vector<LobbyEntry> &entries() const { return entries_; }
	std::string status;  // the original's matchmaking text: "Getting Server List", errors
	uint32_t ping_ms = 0;

private:
	static constexpr uint32_t kRefresh = 20000;  // DE_CGameSpyFactory's server list refresh (+0x144)

	void ask(uint16_t first);
	intptr_t socket_ = -1;
	Address lobby_;
	uint16_t protocol_;
	uint32_t refresh_ms_;
	uint32_t seq_ = 0, asked_at_ = 0, page_at_ = 0;
	std::vector<LobbyEntry> entries_, building_;
	bool answered_ = false;
};

}  // namespace net
