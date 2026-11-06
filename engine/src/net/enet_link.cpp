#include "net/enet_link.h"

#include <enet/enet.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace net {

namespace {

constexpr uint8_t kQuery[4] = {'O', 'B', 'G', 'Q'};
constexpr uint8_t kReply[4] = {'O', 'B', 'G', 'R'};
constexpr uint32_t kSilence = 6000;  // the WinSock node's interruption (FINDINGS 407)
constexpr size_t kChannels = 2;

bool init() {
	static std::once_flag once;
	static bool ok = false;
	std::call_once(once, [] { ok = enet_initialize() == 0; });
	return ok;
}

// ENetHost has no user pointer.
std::mutex g_hosts_lock;
std::map<const ENetHost *, EnetHost *> g_hosts;

uint8_t channel_id(Channel ch) { return ch == Channel::Reliable ? 0 : 1; }

}  // namespace

uint32_t now_ms() {
	init();
	return enet_time_get();
}

std::string Address::text() const {
	ENetAddress a{};
	a.host = ip;
	a.port = port;
	char buf[64] = {};
	if (enet_address_get_host_ip(&a, buf, sizeof buf) != 0) return "?";
	return std::string(buf) + ":" + std::to_string(port);
}

bool resolve(const std::string &spec, uint16_t default_port, Address &out, std::string *error) {
	if (!init()) {
		if (error) *error = "The network could not be started.";
		return false;
	}
	std::string host = spec;
	uint16_t port = default_port;
	size_t colon = spec.rfind(':');
	if (colon != std::string::npos) {
		host = spec.substr(0, colon);
		int p = atoi(spec.c_str() + colon + 1);
		if (p <= 0 || p > 65535) {
			if (error) *error = "\"" + spec + "\" has no valid port.";
			return false;
		}
		port = uint16_t(p);
	}
	while (!host.empty() && host.back() == ' ') host.pop_back();
	while (!host.empty() && host.front() == ' ') host.erase(host.begin());
	ENetAddress a{};
	if (host.empty() || enet_address_set_host(&a, host.c_str()) != 0) {
		if (error) *error = "\"" + host + "\" was not found.";
		return false;
	}
	out.ip = a.host;
	out.port = port;
	return true;
}

// --- Peers ----------------------------------------------------------------------------------

void EnetPeer::send(Channel channel, std::vector<uint8_t> data) {
	if (host_) host_->push(*this, channel, std::move(data));
}

bool EnetPeer::receive(Packet &out) {
	if (inbox_.empty()) return false;
	out = std::move(inbox_.front());
	inbox_.pop_front();
	return true;
}

Address EnetPeer::address() const {
	Address a;
	if (peer_) a.ip = peer_->address.host, a.port = peer_->address.port;
	return a;
}

uint32_t EnetPeer::round_trip() const { return peer_ ? peer_->roundTripTime : 0; }

uint32_t EnetPeer::silent_ms() const { return enet_time_get() - last_heard_; }

// --- Hosts ----------------------------------------------------------------------------------

// The host's intercept: a LAN query is answered from the game port and goes no further.
int answer_query(ENetHost *h, ENetEvent *) {
	EnetHost *self = nullptr;
	{
		std::lock_guard<std::mutex> l(g_hosts_lock);
		auto it = g_hosts.find(h);
		if (it != g_hosts.end()) self = it->second;
	}
	if (h->receivedDataLength < 9 || memcmp(h->receivedData, kQuery, 4) != 0) return 0;
	if (!self || !self->discovery_reply) return 1;
	std::vector<uint8_t> out(kReply, kReply + 4);
	out.insert(out.end(), h->receivedData + 5, h->receivedData + 9);
	auto info = self->discovery_reply();
	out.insert(out.end(), info.begin(), info.end());
	ENetBuffer b;
	b.data = out.data();
	b.dataLength = out.size();
	enet_socket_send(h->socket, &h->receivedAddress, &b, 1);
	return 1;
}

std::unique_ptr<EnetHost> EnetHost::listen(uint16_t port, size_t max_peers, std::string *error) {
	if (!init()) {
		if (error) *error = "The network could not be started.";
		return nullptr;
	}
	ENetAddress a{};
	a.host = ENET_HOST_ANY;
	a.port = port;
	std::unique_ptr<EnetHost> h(new EnetHost);
	h->host_ = enet_host_create(&a, max_peers, kChannels, 0, 0);
	if (!h->host_) {
		if (error) *error = "Port " + std::to_string(port) + " could not be opened; another program may be using it.";
		return nullptr;
	}
	{
		std::lock_guard<std::mutex> l(g_hosts_lock);
		g_hosts[h->host_] = h.get();
	}
	h->host_->intercept = answer_query;
	return h;
}

std::unique_ptr<EnetHost> EnetHost::connect(const Address &to, std::string *error) {
	if (!init()) {
		if (error) *error = "The network could not be started.";
		return nullptr;
	}
	std::unique_ptr<EnetHost> h(new EnetHost);
	h->host_ = enet_host_create(nullptr, 1, kChannels, 0, 0);
	if (!h->host_) {
		if (error) *error = "No network socket could be opened.";
		return nullptr;
	}
	ENetAddress a{};
	a.host = to.ip;
	a.port = to.port;
	ENetPeer *p = enet_host_connect(h->host_, &a, kChannels, 0);
	if (!p) {
		if (error) *error = "The connection could not be started.";
		return nullptr;
	}
	h->wrap(p);
	return h;
}

EnetHost::~EnetHost() {
	if (!host_) return;
	for (auto &p : peers_) {
		if (p->peer_) enet_peer_disconnect_now(p->peer_, 0), p->peer_->data = nullptr;
	}
	{
		std::lock_guard<std::mutex> l(g_hosts_lock);
		g_hosts.erase(host_);
	}
	enet_host_destroy(host_);
}

uint16_t EnetHost::port() const { return host_ ? host_->address.port : 0; }

std::vector<EnetPeer *> EnetHost::peers() {
	std::vector<EnetPeer *> out;
	for (auto &p : peers_) {
		if (p->peer_) out.push_back(p.get());
	}
	return out;
}

EnetPeer *EnetHost::wrap(ENetPeer *peer) {
	if (peer->data) return static_cast<EnetPeer *>(peer->data);
	auto &p = peers_.emplace_back(std::make_unique<EnetPeer>());
	p->host_ = this;
	p->peer_ = peer;
	p->id = next_id_++;
	p->last_heard_ = enet_time_get();
	peer->data = p.get();
	enet_peer_timeout(peer, 0, kick_ms_, kick_ms_);
	return p.get();
}

// The delay simulator holds each packet until its time; reliable ones keep their order.
void EnetHost::push(EnetPeer &p, Channel ch, std::vector<uint8_t> data) {
	if (!p.peer_) return;
	p.bytes_out += data.size();
	const uint32_t now = enet_time_get();
	if (delay_.delay_ms <= 0 && delay_.variation_ms <= 0 && p.held_.empty()) {
		p.held_.push_back({now, ch, std::move(data)});
		flush(p, now);
		return;
	}
	std::uniform_real_distribution<float> u(0, 1);
	uint32_t due = now + uint32_t(std::max(0.0f, delay_.delay_ms + u(rng_) * delay_.variation_ms));
	if (ch == Channel::Reliable) due = p.reliable_due_ = std::max(due, p.reliable_due_);
	auto at = std::find_if(p.held_.begin(), p.held_.end(), [due](const EnetPeer::Held &h) { return int32_t(h.due - due) > 0; });
	p.held_.insert(at, {due, ch, std::move(data)});
}

void EnetHost::flush(EnetPeer &p, uint32_t now) {
	while (!p.held_.empty() && int32_t(now - p.held_.front().due) >= 0 && p.peer_) {
		auto &h = p.held_.front();
		ENetPacket *pk = enet_packet_create(h.data.data(), h.data.size(), h.channel == Channel::Reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
		if (pk && enet_peer_send(p.peer_, channel_id(h.channel), pk) != 0) enet_packet_destroy(pk);
		p.held_.pop_front();
	}
}

void EnetHost::service() {
	if (!host_) return;
	const uint32_t now = enet_time_get();
	for (auto &p : peers_) flush(*p, now);
	ENetEvent e;
	while (enet_host_service(host_, &e, 0) > 0) {
		switch (e.type) {
		case ENET_EVENT_TYPE_CONNECT: {
			EnetPeer *p = wrap(e.peer);
			p->last_heard_ = enet_time_get();
			events_.push_back({Event::Connected, p});
			break;
		}
		case ENET_EVENT_TYPE_RECEIVE: {
			if (auto *p = static_cast<EnetPeer *>(e.peer->data)) {
				p->last_heard_ = enet_time_get();
				p->bytes_in += e.packet->dataLength;
				Packet in;
				in.channel = e.channelID == 0 ? Channel::Reliable : Channel::Unreliable;
				in.data.assign(e.packet->data, e.packet->data + e.packet->dataLength);
				p->inbox_.push_back(std::move(in));
				if (p->interrupted) p->interrupted = false, events_.push_back({Event::Restored, p});
			}
			enet_packet_destroy(e.packet);
			break;
		}
		case ENET_EVENT_TYPE_DISCONNECT:
			if (auto *p = static_cast<EnetPeer *>(e.peer->data)) {
				p->peer_ = nullptr;
				p->held_.clear();
				e.peer->data = nullptr;
				events_.push_back({Event::Disconnected, p});
			}
			break;
		default: break;
		}
	}
	// ENet's own pings keep lastReceiveTime fresh while the other side is alive.
	for (auto &p : peers_) {
		if (!p->peer_ || p->peer_->state != ENET_PEER_STATE_CONNECTED) continue;
		uint32_t heard = std::max(p->last_heard_, p->peer_->lastReceiveTime);
		p->last_heard_ = heard;
		if (!p->interrupted && enet_time_get() - heard > kSilence) p->interrupted = true, events_.push_back({Event::Interrupted, p.get()});
	}
	enet_host_flush(host_);
}

bool EnetHost::poll(Event &out) {
	if (events_.empty()) return false;
	out = events_.front();
	events_.pop_front();
	return true;
}

void EnetHost::disconnect(EnetPeer *p, bool now) {
	if (!p || !p->peer_) return;
	flush(*p, enet_time_get() + 0x7fffffff);
	if (now) {
		enet_peer_disconnect_now(p->peer_, 0);
		p->peer_->data = nullptr;
		p->peer_ = nullptr;
	} else {
		enet_peer_disconnect_later(p->peer_, 0);
	}
	enet_host_flush(host_);
}

// --- Discovery ------------------------------------------------------------------------------

Discovery::Discovery() {
	if (!init()) return;
	ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (s == ENET_SOCKET_NULL) return;
	enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
	enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
	ENetAddress any{};
	any.host = ENET_HOST_ANY;
	any.port = 0;
	if (enet_socket_bind(s, &any) != 0) {
		enet_socket_destroy(s);
		return;
	}
	socket_ = intptr_t(s);
}

Discovery::~Discovery() {
	if (socket_ >= 0) enet_socket_destroy(ENetSocket(socket_));
}

void Discovery::send_to(uint32_t ip, uint16_t port) {
	if (socket_ < 0) return;
	uint8_t q[9];
	memcpy(q, kQuery, 4);
	q[4] = 1;
	uint32_t seq = ++sequence_;
	memcpy(q + 5, &seq, 4);
	sent_.emplace_back(seq, enet_time_get());
	if (sent_.size() > 64) sent_.erase(sent_.begin());
	ENetAddress a{};
	a.host = ip;
	a.port = port;
	ENetBuffer b;
	b.data = q;
	b.dataLength = sizeof q;
	enet_socket_send(ENetSocket(socket_), &a, &b, 1);
}

void Discovery::query_broadcast(uint16_t port) {
	send_to(ENET_HOST_BROADCAST, port);
	// Broadcasts are not always looped back: hosts on this machine are asked directly too.
	ENetAddress local{};
	enet_address_set_host_ip(&local, "127.0.0.1");
	send_to(local.host, port);
}

void Discovery::query(const Address &to) { send_to(to.ip, to.port); }

bool Discovery::receive(Reply &out) {
	if (socket_ < 0) return false;
	uint8_t buf[2048];
	for (;;) {
		ENetAddress from{};
		ENetBuffer b;
		b.data = buf;
		b.dataLength = sizeof buf;
		int n = enet_socket_receive(ENetSocket(socket_), &from, &b, 1);
		if (n <= 0) return false;
		if (n < 8 || memcmp(buf, kReply, 4) != 0) continue;
		uint32_t seq;
		memcpy(&seq, buf + 4, 4);
		auto it = std::find_if(sent_.begin(), sent_.end(), [seq](auto &s) { return s.first == seq; });
		if (it == sent_.end()) continue;
		out.from.ip = from.host;
		out.from.port = from.port;
		out.ping_ms = enet_time_get() - it->second;
		out.info.assign(buf + 8, buf + n);
		return true;
	}
}

}  // namespace net
