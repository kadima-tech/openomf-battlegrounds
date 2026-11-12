// A network game on the ENet transport: hosting (the listen server) or joining one, the join
// handshake (the app manager's com interface, messages 2 / 4 / 5, FINDINGS 409, with our own
// versioned hello), LAN discovery answers, the lobby (listing a hosted session, joining a listed
// one through a hole punch or the lobby's relay), and the NetSession that runs once the session
// exists. Every connection is encrypted (net/secure.h).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "game/netplay.h"
#include "net/enet_link.h"
#include "net/lobby.h"

namespace de {
class World;
}

namespace game {

constexpr uint16_t kNetProtocol = 3;  // ours; the original's handshake carried 10
const char *net_engine_version();
// MD5 over every data file's table of contents and the contents of everything but its media.
std::array<uint8_t, 16> net_data_hash(const de::World &data);

struct NetOptions {
	std::string player_name, server_name, password, admin_password;
	uint16_t port = net::kDefaultPort;
	net::Delay delay;
	uint32_t kick_ms = 60000;
	int budget = 16000;
	std::string lobby;     // Network\Lobby Server, "host[:port]"; none by default
	bool lan_only = false;  // Network\LAN Only: a hosted session is not listed on the lobby
	std::string host_key;  // Network\Host Key: the host identity's seed, hex
	bool force_relay = false;  // OPENBG_FORCE_RELAY (tests): a lobby join skips the hole punch
};
// Network\Player Name, Server Name, Server Password, Kick Time, Winsock\Listener Port, Maximum
// Outgoing Bytes, Delay Packet Time and Delay Packet Time Added Variation, Lobby Server, LAN Only
// and Host Key.
NetOptions net_options(const std::map<std::string, std::string> &registry);

// What a LAN query's answer carries (the browser's columns).
struct NetServerInfo {
	uint16_t protocol = 0;
	std::string version, server_name, session_name;
	std::array<uint8_t, 16> data_hash{};
	int players = 0, max_players = 0;
	bool password = false, playing = false;
	uint16_t port = 0;
	uint32_t instance = 0;  // tells one host's answers apart from another's (broadcast and loopback both reach it)
};
std::vector<uint8_t> encode_server_info(const NetServerInfo &i);
bool decode_server_info(const std::vector<uint8_t> &d, NetServerInfo &out);

class NetGame {
public:
	enum class State { Idle, Hosting, Connecting, Joined, Closed };
	explicit NetGame(World &w);
	~NetGame();
	// The listen server for `s`, the session the front end launched (its own player joins it locally).
	bool host(SessionState &s, const NetOptions &o, std::string *error);
	// Connects to a server ("host[:port]", or "lobby:<server name>" for a session the lobby lists);
	// the session starts when the server accepts.
	bool join(const std::string &address, const NetOptions &o, std::string *error);
	// Joins a session the lobby lists: its introduction, a hole punch, else the lobby's relay.
	bool join_listed(const net::LobbyEntry &e, const NetOptions &o, std::string *error);
	// The lobby a hosted session is listed on, while it is.
	const net::LobbyHost *lobby_host() const { return lobby_host_.get(); }
	// After each World::tick: the transport, the handshake, the session's network side.
	void update();
	// A client's frame time with the clock's slew.
	uint32_t frame_ms(uint32_t real_ms);
	void close();
	State state() const { return state_; }
	NetSession *session() { return net_.get(); }
	net::EnetHost *transport() { return host_.get(); }
	// Refusals and lost connections, for the user.
	std::function<void(const std::string &title, const std::string &text)> notify;
	// A client's session began (the front end closes its pages); the connection is gone.
	std::function<void()> on_joined, on_closed;

private:
	void lobby(net::EnetPeer &p, const net::Packet &packet);
	void start_client_session(const std::vector<uint8_t> &d, size_t pos);
	void fail(const std::string &title, const std::string &text);
	// A peer's own address: a relayed one's is the player's, not the relay's.
	net::Address origin(net::EnetPeer *p) const;
	World &w_;
	State state_ = State::Idle;
	NetOptions opt_;
	std::unique_ptr<net::EnetHost> host_;
	std::unique_ptr<NetSession> net_;
	SessionState *hosted_ = nullptr;
	net::EnetPeer *server_ = nullptr;
	std::map<net::EnetPeer *, SessionClient *> accepted_;
	std::array<uint8_t, 16> hash_{};
	uint32_t instance_ = 0;
	std::map<uint32_t, uint64_t> bans_;  // by address: until (steady ms)
	std::unique_ptr<net::LobbyHost> lobby_host_;
	std::unique_ptr<net::LobbyJoin> lobby_join_;
	std::unique_ptr<net::LobbyList> finder_;  // "lobby:<name>": the list until that session is in it
	std::string finding_;
	uint32_t finding_since_ = 0;
};
// "host[:port]" with the lobby's default port.
bool resolve_lobby(const std::string &spec, net::Address &out, std::string *error);

}  // namespace game
