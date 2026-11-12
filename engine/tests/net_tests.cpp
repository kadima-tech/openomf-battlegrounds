// The network transport without game data: ENet on localhost (connect, both channels, the delay
// simulator, LAN discovery, disconnect), the encryption (handshake, identities, sealing, replay
// and tampering, nothing readable on the wire) and the loopback link.
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "net/enet_link.h"
#include "net/link.h"
#include "net/lobby.h"
#include "net/secure.h"

namespace {

int failures = 0;
void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++failures;
}

template <class F>
bool pump(net::EnetHost &a, net::EnetHost &b, F done, uint32_t ms = 3000) {
	uint32_t t0 = net::now_ms();
	while (net::now_ms() - t0 < ms) {
		a.service();
		b.service();
		if (done()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

// A UDP relay between a client and a server that remembers whether any datagram held `marker`.
struct Tap {
	ENetSocket sock = ENET_SOCKET_NULL;
	ENetAddress server{}, client{};
	bool have_client = false, seen = false;
	size_t datagrams = 0;
	std::string marker;
	explicit Tap(uint16_t server_port, std::string m) : marker(std::move(m)) {
		sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
		enet_socket_set_option(sock, ENET_SOCKOPT_NONBLOCK, 1);
		ENetAddress any{};
		enet_address_set_host_ip(&any, "127.0.0.1");
		any.port = 0;
		enet_socket_bind(sock, &any);
		enet_address_set_host_ip(&server, "127.0.0.1");
		server.port = server_port;
	}
	~Tap() { enet_socket_destroy(sock); }
	uint16_t port() {
		ENetAddress a{};
		enet_socket_get_address(sock, &a);
		return a.port;
	}
	void pump() {
		uint8_t buf[4096];
		for (;;) {
			ENetAddress from{};
			ENetBuffer b{};
			b.data = buf;
			b.dataLength = sizeof buf;
			int n = enet_socket_receive(sock, &from, &b, 1);
			if (n <= 0) return;
			++datagrams;
			if (std::search(buf, buf + n, marker.begin(), marker.end()) != buf + n) seen = true;
			const bool from_server = from.host == server.host && from.port == server.port;
			if (!from_server) client = from, have_client = true;
			ENetAddress to = from_server ? client : server;
			if (from_server && !have_client) continue;
			b.dataLength = size_t(n);
			enet_socket_send(sock, &to, &b, 1);
		}
	}
};

void secure_unit_tests() {
	net::Identity id = net::Identity::from_seed(net::Key32{1, 2, 3});
	net::SecureChannel c(net::SecureChannel::Role::Client), h(net::SecureChannel::Role::Host);
	auto answer = h.answer(c.hello(), id);
	std::string why;
	check(!answer.empty() && c.finish(answer, id.pub, &why) && c.ready() && h.ready(), "handshake: the client checks the host's signed identity");
	std::vector<uint8_t> msg = {'h', 'e', 'l', 'l', 'o'}, out;
	auto sealed = c.seal(1, msg);
	check(sealed.size() == msg.size() + 24 && std::search(sealed.begin(), sealed.end(), msg.begin(), msg.end()) == sealed.end(), "sealed: the plaintext is not in it");
	check(h.open(1, sealed, out) && out == msg, "sealed: opened by the other side");
	check(!h.open(1, sealed, out), "sealed: a replay is refused");
	auto tampered = c.seal(1, msg);
	tampered[10] ^= 1;
	check(!h.open(1, tampered, out), "sealed: an altered message is refused");
	auto later = c.seal(1, msg), earlier_dup = later;
	auto newest = c.seal(1, msg);
	check(h.open(1, newest, out) && h.open(1, later, out) && !h.open(1, earlier_dup, out), "unreliable: late but new is taken once");
	auto r1 = c.seal(0, msg), r2 = c.seal(0, msg);
	check(h.open(0, r2, out) && !h.open(0, r1, out), "reliable: an older counter is refused");
	auto back = h.seal(0, msg);
	check(c.open(0, back, out) && out == msg, "host to client");
	check(!c.open(1, h.seal(0, msg), out), "a message is bound to its channel");

	net::SecureChannel c2(net::SecureChannel::Role::Client), h2(net::SecureChannel::Role::Host);
	net::Identity other = net::Identity::from_seed(net::Key32{9});
	check(!c2.finish(h2.answer(c2.hello(), id), other.pub, &why) && !why.empty(), "handshake: another identity than expected is refused");
	net::SecureChannel c3(net::SecureChannel::Role::Client), h3(net::SecureChannel::Role::Host);
	auto forged = h3.answer(c3.hello(), id);
	forged[40] ^= 0x80;  // the identity key, under the signature
	check(!c3.finish(forged, std::nullopt, &why), "handshake: a forged answer is refused");
	auto seed = net::Identity::new_seed_hex();
	auto a = net::Identity::from_hex(seed), b = net::Identity::from_hex(seed);
	check(seed.size() == 64 && a && b && a->pub == b->pub && !net::Identity::from_hex("xyz"), "identity: a seed in hex gives the same key");
}

// The handshake over ENet: a client that expects another host's key is refused; the right one
// connects and nothing of what it sends is readable between the two.
void secure_transport_tests() {
	std::string err;
	auto server = net::EnetHost::listen(0, 4, &err);
	net::Identity id = net::Identity::from_seed(net::Key32{4, 5, 6});
	server->set_identity(id);
	net::Address to;
	net::resolve("127.0.0.1:" + std::to_string(server->port()), 0, to);

	auto wrong = net::EnetHost::client(&err);
	wrong->expect_identity(net::Identity::from_seed(net::Key32{7}).pub);
	wrong->connect_to(to);
	std::string refused;
	pump(*server, *wrong, [&] {
		net::EnetHost::Event e;
		while (wrong->poll(e)) {
			if (e.kind == net::EnetHost::Event::Disconnected) refused = e.peer->refusal;
			if (e.kind == net::EnetHost::Event::Connected) refused = "connected";
		}
		return !refused.empty();
	});
	check(!refused.empty() && refused != "connected", "a client expecting another identity does not connect");

	const std::string marker = "PLAINTEXT-MARKER-0123456789";
	Tap tap(server->port(), marker);
	net::Address via;
	net::resolve("127.0.0.1:" + std::to_string(tap.port()), 0, via);
	auto client = net::EnetHost::client(&err);
	client->expect_identity(id.pub);
	client->connect_to(via);
	net::EnetPeer *at_server = nullptr, *at_client = nullptr;
	auto both = [&] {
		tap.pump();
		net::EnetHost::Event e;
		while (server->poll(e)) {
			if (e.kind == net::EnetHost::Event::Connected) at_server = e.peer;
		}
		while (client->poll(e)) {
			if (e.kind == net::EnetHost::Event::Connected) at_client = e.peer;
		}
		return at_server && at_client;
	};
	check(pump(*server, *client, both), "the expected identity connects (through a tap)");
	if (!at_server || !at_client) return;
	check(at_client->identity() && *at_client->identity() == id.pub, "the client knows the host's identity");
	std::vector<uint8_t> m(marker.begin(), marker.end());
	for (int i = 0; i < 5; ++i) {
		at_client->send(net::Channel::Reliable, m);
		at_client->send(net::Channel::Unreliable, m);
		at_server->send(net::Channel::Reliable, m);
	}
	int got = 0, back = 0;
	pump(*server, *client, [&] {
		tap.pump();
		net::Packet p;
		while (at_server->receive(p)) got += p.data == m;
		while (at_client->receive(p)) back += p.data == m;
		return got == 10 && back == 5;
	});
	check(got == 10 && back == 5, "sealed messages arrive whole both ways");
	check(tap.datagrams > 0 && !tap.seen, "no datagram on the wire holds the plaintext");
}

// The lobby's wire format as the Go server reads it (server/lobby/protocol.go).
void lobby_format_tests() {
	auto m = net::lobby_message(net::LobbyMsg::Bind);
	net::LobbyMsg t;
	size_t body = 0;
	check(m.size() == 6 && net::lobby_parse(m.data(), m.size(), t, body) && t == net::LobbyMsg::Bind && body == 6, "lobby: header");
	check(!net::is_lobby_datagram((const uint8_t *)"OBGQ\x01\x00", 6), "lobby: other datagrams are told apart");
}

}  // namespace

int main() {
	std::string err;
	auto server = net::EnetHost::listen(0, 4, &err);
	check(server != nullptr, "listen on a free port");
	if (!server) return 1;
	const uint16_t port = server->port();
	server->discovery_reply = [] { return std::vector<uint8_t>{'h', 'i'}; };

	net::Address to;
	check(net::resolve("127.0.0.1:" + std::to_string(port), net::kDefaultPort, to, &err), "resolve 127.0.0.1:port");
	check(to.text() == "127.0.0.1:" + std::to_string(port), "address text");
	net::Address d;
	check(net::resolve("localhost", 2002, d) && d.port == 2002, "default port");

	auto client = net::EnetHost::connect(to, &err);
	check(client != nullptr, "connect");
	net::EnetPeer *at_server = nullptr, *at_client = nullptr;
	bool ok = pump(*server, *client, [&] {
		net::EnetHost::Event e;
		while (server->poll(e)) {
			if (e.kind == net::EnetHost::Event::Connected) at_server = e.peer;
		}
		while (client->poll(e)) {
			if (e.kind == net::EnetHost::Event::Connected) at_client = e.peer;
		}
		return at_server && at_client;
	});
	check(ok, "both sides see the connection");
	if (!ok) return 1;

	for (int i = 0; i < 50; ++i) at_client->send(net::Channel::Reliable, std::vector<uint8_t>(1 + i * 100, uint8_t(i)));
	at_client->send(net::Channel::Unreliable, {7, 7, 7});
	std::vector<net::Packet> got;
	pump(*server, *client, [&] {
		net::Packet p;
		while (at_server->receive(p)) got.push_back(p);
		return got.size() >= 51;
	});
	bool ordered = true;
	int reliable = 0;
	for (auto &p : got) {
		if (p.channel != net::Channel::Reliable) continue;
		ordered &= p.data.size() == size_t(1 + reliable * 100) && p.data[0] == uint8_t(reliable);
		++reliable;
	}
	check(reliable == 50 && ordered, "reliable packets arrive whole and in order (up to 4901 bytes)");
	check(got.size() == 51, "the unreliable packet arrives on localhost");

	client->set_delay({100, 0});
	uint32_t sent = net::now_ms();
	at_client->send(net::Channel::Reliable, {1});
	uint32_t arrived = 0;
	pump(*server, *client, [&] {
		net::Packet p;
		if (at_server->receive(p)) arrived = net::now_ms();
		return arrived != 0;
	});
	check(arrived && arrived - sent >= 100 && arrived - sent < 300, "the delay simulator holds a packet 100 ms");
	client->set_delay({});

	net::Discovery disc;
	check(disc.ok(), "discovery socket");
	disc.query(to);
	net::Discovery::Reply r;
	bool heard = pump(*server, *client, [&] { return disc.receive(r); });
	check(heard && r.info == std::vector<uint8_t>{'h', 'i'} && r.from.port == port, "a direct query is answered on the game port");
	disc.query_broadcast(port);
	heard = pump(*server, *client, [&] { return disc.receive(r); });
	check(heard, "a LAN query is answered");

	client->disconnect(at_client);
	bool gone = pump(*server, *client, [&] {
		net::EnetHost::Event e;
		while (server->poll(e)) {
			if (e.kind == net::EnetHost::Event::Disconnected) return true;
		}
		return false;
	});
	check(gone, "the server hears the disconnect");

	// Hosting again after leaving a session: the port is free once the host is gone, and taken while it lives.
	bool in_use = false;
	check(net::EnetHost::listen(port, 4, &err, &in_use) == nullptr, "a port in use cannot be listened on twice");
	check(in_use, "a port in use is told apart from other failures (the WinSock service's next-port retry)");
	client.reset();
	server.reset();
	auto again = net::EnetHost::listen(port, 4, &err);
	check(again != nullptr && again->port() == port, "the same port listens again once released");

	uint32_t clock = 0;
	net::LoopbackLink link({50, 0, 0}, [&clock] { return clock; });
	link.a().send(net::Channel::Reliable, {1, 2});
	net::Packet p;
	check(!link.b().receive(p), "loopback: not before its delay");
	clock = 50;
	check(link.b().receive(p) && p.data.size() == 2, "loopback: arrives after its delay");

	secure_unit_tests();
	secure_transport_tests();
	lobby_format_tests();

	printf("%s\n", failures ? "net-tests FAILED" : "net-tests passed");
	return failures ? 1 : 0;
}
