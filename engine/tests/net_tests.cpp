// The network transport without game data: ENet on localhost (connect, both channels, the delay
// simulator, LAN discovery, disconnect) and the loopback link.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "net/enet_link.h"
#include "net/link.h"

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
	check(net::EnetHost::listen(port, 4, &err) == nullptr, "a port in use cannot be listened on twice");
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

	printf("%s\n", failures ? "net-tests FAILED" : "net-tests passed");
	return failures ? 1 : 0;
}
