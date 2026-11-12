// The network transport: one UDP socket through ENet, with the link's reliable ordered channel
// (the original's guaranteed packets) and unreliable channel (states), the original's delay
// simulator (Network\Winsock\Delay Packet Time and Added Variation), its silence and Kick Time
// rules, and LAN discovery answered on the game port (the original's broadcast query, FINDINGS 413).
// Every connection is encrypted and authenticated (net/secure.h): a peer is Connected once its
// handshake is done. The socket also carries the lobby's datagrams (net/lobby.h).
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "net/link.h"
#include "net/secure.h"

struct _ENetHost;
struct _ENetPeer;
struct _ENetEvent;

namespace net {

constexpr uint16_t kDefaultPort = 2002;  // Network\Winsock\Listener Port
// WinSockService::ObjInitContents (Core 0x7019e0b0): a port in use, the next one, up to 99 more.
constexpr int kPortRetries = 99;

struct Address {
	uint32_t ip = 0;  // network byte order, as ENet keeps it
	uint16_t port = kDefaultPort;
	std::string text() const;
	bool operator==(const Address &o) const { return ip == o.ip && port == o.port; }
	bool operator<(const Address &o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
};
// "host", "host:port" or "a.b.c.d:port"; the name is resolved. False with `error` set otherwise.
bool resolve(const std::string &spec, uint16_t default_port, Address &out, std::string *error = nullptr);

// The delay simulator: every packet sent waits `delay_ms` plus up to `variation_ms` more.
struct Delay {
	float delay_ms = 0, variation_ms = 0;
};

class EnetHost;

// A connected machine: the link's Endpoint, its round trip and silence.
class EnetPeer : public Endpoint {
public:
	void send(Channel channel, std::vector<uint8_t> data) override;
	bool receive(Packet &out) override;
	Address address() const;
	uint32_t round_trip() const;  // ENet's smoothed estimate, ms
	uint32_t silent_ms() const;   // since anything last arrived from it
	bool interrupted = false;     // silent for 6 s (SessionConnectionInterrupted)
	uint32_t id = 0;              // this host's number for it, from 1
	uint64_t bytes_in = 0, bytes_out = 0;
	// The other side's identity key, once the handshake is done.
	const Key32 *identity() const { return secure_ && secure_->ready() ? &secure_->peer_identity() : nullptr; }
	uint64_t rejected() const { return secure_ ? secure_->rejected : 0; }
	std::string refusal;          // why the handshake failed

private:
	friend class EnetHost;
	struct Held {
		uint32_t due;
		Channel channel;
		std::vector<uint8_t> data;
	};
	EnetHost *host_ = nullptr;
	_ENetPeer *peer_ = nullptr;
	std::unique_ptr<SecureChannel> secure_;
	std::deque<Packet> pending_;  // sent before the handshake finished
	uint32_t connected_at_ = 0;
	bool announced_ = false;
	std::deque<Packet> inbox_;
	std::deque<Held> held_;
	uint32_t reliable_due_ = 0;
	uint32_t last_heard_ = 0;
};

class EnetHost {
public:
	struct Event {
		enum Kind { Connected, Disconnected, Interrupted, Restored } kind;
		EnetPeer *peer;
	};
	// A server on `port` (0: any free one) for `max_peers` machines, or a client connecting to
	// `to`. Null with `error` set when the socket cannot be opened; `in_use` tells a port taken.
	static std::unique_ptr<EnetHost> listen(uint16_t port, size_t max_peers, std::string *error = nullptr, bool *in_use = nullptr);
	static std::unique_ptr<EnetHost> connect(const Address &to, std::string *error = nullptr);
	// A client socket that connects later (connect_to), after a lobby's introduction.
	static std::unique_ptr<EnetHost> client(std::string *error = nullptr);
	// Drops a connection still being made and connects to `to` instead.
	void connect_to(const Address &to);
	// A host's identity key for the handshake; a client's expectation of the host's.
	void set_identity(const Identity &id) { identity_ = id; }
	Key32 identity() const { return identity_ ? identity_->pub : Key32{}; }
	void expect_identity(std::optional<Key32> k) { expect_ = k; }
	// A datagram outside ENet (lobby messages, hole punches) from this socket.
	void send_raw(const Address &to, const std::vector<uint8_t> &data);
	// Lobby datagrams reaching this socket; a handler returns true when it took one.
	int add_raw_handler(std::function<bool(const Address &, const uint8_t *, size_t)> h);
	void remove_raw_handler(int id);
	// Addresses that are infrastructure (the lobby, relays): reachable under OPENBG_NAT=symmetric.
	void allow(const Address &a) { infrastructure_.push_back(a); }
	~EnetHost();
	// Sends what the delay simulator has released and takes in what arrived; call every frame.
	void service();
	bool poll(Event &out);
	void disconnect(EnetPeer *p, bool now = false);
	uint16_t port() const;
	void set_delay(Delay d) { delay_ = d; }
	// Network\Kick Time: a silent peer is dropped after this long (the original's 60 s).
	void set_kick_time(uint32_t ms) { kick_ms_ = ms; }
	// A LAN query (Discovery) reaching this host's socket is answered with these bytes.
	std::function<std::vector<uint8_t>()> discovery_reply;
	std::vector<EnetPeer *> peers();

private:
	EnetHost() = default;
	friend class EnetPeer;
	friend int intercept(_ENetHost *, _ENetEvent *);
	void push(EnetPeer &p, Channel ch, std::vector<uint8_t> data);
	void handshake(EnetPeer &p, std::vector<uint8_t> data);
	void fail_handshake(EnetPeer &p, const std::string &why);
	void sent_to(const Address &a);
	bool filtered(const Address &from);
	void flush(EnetPeer &p, uint32_t now);
	EnetPeer *wrap(_ENetPeer *peer);
	_ENetHost *host_ = nullptr;
	Delay delay_;
	uint32_t kick_ms_ = 60000;
	uint32_t next_id_ = 1;
	std::mt19937 rng_{2002};
	std::vector<std::unique_ptr<EnetPeer>> peers_;
	std::deque<Event> events_;
	bool server_ = false;
	std::optional<Identity> identity_;
	std::optional<Key32> expect_;
	_ENetPeer *connecting_ = nullptr;
	std::vector<std::pair<int, std::function<bool(const Address &, const uint8_t *, size_t)>>> raw_handlers_;
	int next_handler_ = 1;
	// Test conditions (OPENBG_NAT, OPENBG_NET_LOSS): emulated NAT filtering and loss of what arrives.
	int nat_ = 0;  // 0 none, 1 cone (only from addresses sent to), 2 symmetric (only infrastructure)
	float loss_ = 0;
	std::map<Address, uint32_t> sent_to_;
	std::vector<Address> infrastructure_;
};

// The client side of LAN discovery: queries the broadcast address (or one address, for a typed
// one) on a port; each host's reply comes back with its round trip.
class Discovery {
public:
	struct Reply {
		Address from;
		uint32_t ping_ms = 0;
		std::vector<uint8_t> info;
	};
	Discovery();
	~Discovery();
	bool ok() const { return socket_ >= 0; }
	void query_broadcast(uint16_t port);
	void query_broadcast(const std::vector<uint16_t> &ports);
	void query(const Address &to);
	bool receive(Reply &out);

private:
	void send_to(uint32_t ip, uint16_t port);
	intptr_t socket_ = -1;
	uint32_t sequence_ = 0;
	std::vector<std::pair<uint32_t, uint32_t>> sent_;  // sequence, time
};

// Milliseconds since the transport started (ENet's clock).
uint32_t now_ms();

}  // namespace net
