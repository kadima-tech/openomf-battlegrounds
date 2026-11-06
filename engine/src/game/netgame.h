// A network game on the ENet transport: hosting (the listen server) or joining one, the join
// handshake (the app manager's com interface, messages 2 / 4 / 5, FINDINGS 409, with our own
// versioned hello), LAN discovery answers, and the NetSession that runs once the session exists.
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

namespace de {
class World;
}

namespace game {

constexpr uint16_t kNetProtocol = 1;  // ours; the original's handshake carried 10
const char *net_engine_version();
// MD5 over every data file's table of contents and the contents of everything but its media.
std::array<uint8_t, 16> net_data_hash(const de::World &data);

struct NetOptions {
	std::string player_name, server_name, password;
	uint16_t port = net::kDefaultPort;
	net::Delay delay;
	uint32_t kick_ms = 60000;
	int budget = 16000;
};
// Network\Player Name, Server Name, Server Password, Kick Time, Winsock\Listener Port, Maximum
// Outgoing Bytes, Delay Packet Time and Delay Packet Time Added Variation.
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
	// Connects to a server ("host[:port]"); the session starts when the server accepts.
	bool join(const std::string &address, const NetOptions &o, std::string *error);
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
};

}  // namespace game
