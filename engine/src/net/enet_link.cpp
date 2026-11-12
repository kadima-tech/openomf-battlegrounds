#include "net/enet_link.h"

#include <enet/enet.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <mstcpip.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace net {

namespace {

constexpr uint8_t kQuery[4] = {'O', 'B', 'G', 'Q'};
constexpr uint8_t kReply[4] = {'O', 'B', 'G', 'R'};
constexpr uint8_t kLobbyMagic[4] = {'O', 'B', 'G', 'L'};
constexpr uint32_t kSilence = 6000;  // the WinSock node's interruption (FINDINGS 407)
constexpr uint32_t kHandshakeTime = 10000;
constexpr uint32_t kNatMapping = 30000;  // an emulated NAT forgets an address sent to this long ago
constexpr size_t kChannels = 2;

// OPENBG_NET_WIRE=<file> (tests): every datagram this process's game sockets receive, raw, each as
// a 4-byte little-endian length and the bytes.
void wire_log(const uint8_t *d, size_t n) {
	static std::mutex lock;
	static FILE *f = [] {
		const char *path = getenv("OPENBG_NET_WIRE");
		return path && *path ? fopen(path, "ab") : nullptr;
	}();
	if (!f) return;
	std::lock_guard<std::mutex> l(lock);
	uint8_t len[4] = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
	fwrite(len, 1, 4, f);
	fwrite(d, 1, n, f);
	fflush(f);
}

// The test conditions, from the process's environment.
void test_conditions(int &nat, float &loss) {
	const char *n = getenv("OPENBG_NAT");
	nat = !n ? 0 : !strcmp(n, "cone") ? 1 : !strcmp(n, "symmetric") ? 2 : 0;
	const char *l = getenv("OPENBG_NET_LOSS");
	loss = l ? std::clamp(float(atof(l)), 0.0f, 1.0f) : 0.0f;
}

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

// WSAEADDRINUSE / EADDRINUSE from a bind of our own (ENet keeps no error).
bool port_taken(uint16_t port) {
	ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (s == ENET_SOCKET_NULL) return false;
	ENetAddress a{};
	a.host = ENET_HOST_ANY;
	a.port = port;
	bool taken = false;
	if (enet_socket_bind(s, &a) != 0) {
#ifdef _WIN32
		taken = WSAGetLastError() == WSAEADDRINUSE;
#else
		taken = errno == EADDRINUSE;
#endif
	}
	enet_socket_destroy(s);
	return taken;
}

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
	if (!host_) return;
	if (!secure_ || !secure_->ready()) {
		pending_.push_back({channel, std::move(data)});
		return;
	}
	host_->push(*this, channel, secure_->seal(channel_id(channel), data));
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

// The intercept, before ENet sees a datagram: the test conditions drop some, a LAN query is
// answered from the game port, and lobby datagrams go to their handlers.
int intercept(ENetHost *h, ENetEvent *) {
	EnetHost *self = nullptr;
	{
		std::lock_guard<std::mutex> l(g_hosts_lock);
		auto it = g_hosts.find(h);
		if (it != g_hosts.end()) self = it->second;
	}
	const uint8_t *d = h->receivedData;
	const size_t n = h->receivedDataLength;
	wire_log(d, n);
	if (!self) return 0;
	const Address from{h->receivedAddress.host, h->receivedAddress.port};
	if (self->filtered(from)) return 1;
	if (n >= 9 && memcmp(d, kQuery, 4) == 0) {
		if (!self->discovery_reply) return 1;
		std::vector<uint8_t> out(kReply, kReply + 4);
		out.insert(out.end(), d + 5, d + 9);
		auto info = self->discovery_reply();
		out.insert(out.end(), info.begin(), info.end());
		ENetBuffer b;
		b.data = out.data();
		b.dataLength = out.size();
		enet_socket_send(h->socket, &h->receivedAddress, &b, 1);
		return 1;
	}
	if (n >= 6 && memcmp(d, kLobbyMagic, 4) == 0) {
		auto handlers = self->raw_handlers_;
		for (auto &[id, fn] : handlers) {
			if (fn(from, d, n)) break;
		}
		return 1;
	}
	return 0;
}

bool EnetHost::filtered(const Address &from) {
	if (loss_ > 0 && std::uniform_real_distribution<float>(0, 1)(rng_) < loss_) return true;
	if (!nat_) return false;
	if (std::find(infrastructure_.begin(), infrastructure_.end(), from) != infrastructure_.end()) return false;
	if (nat_ == 2) return true;
	for (auto &p : peers_) {
		if (p->peer_ && p->peer_->state == ENET_PEER_STATE_CONNECTED && p->address() == from) return false;
	}
	auto it = sent_to_.find(from);
	return it == sent_to_.end() || enet_time_get() - it->second > kNatMapping;
}

void EnetHost::sent_to(const Address &a) { sent_to_[a] = enet_time_get(); }

std::unique_ptr<EnetHost> EnetHost::listen(uint16_t port, size_t max_peers, std::string *error, bool *in_use) {
	if (in_use) *in_use = false;
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
		if (in_use) *in_use = port_taken(port);
		return nullptr;
	}
	{
		std::lock_guard<std::mutex> l(g_hosts_lock);
		g_hosts[h->host_] = h.get();
	}
	h->host_->intercept = intercept;
	h->server_ = true;
	Key32 seed;
	random_bytes(seed.data(), seed.size());
	h->identity_ = Identity::from_seed(seed);
	test_conditions(h->nat_, h->loss_);
	return h;
}

std::unique_ptr<EnetHost> EnetHost::client(std::string *error) {
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
	{
		std::lock_guard<std::mutex> l(g_hosts_lock);
		g_hosts[h->host_] = h.get();
	}
	h->host_->intercept = intercept;
	test_conditions(h->nat_, h->loss_);
	return h;
}

std::unique_ptr<EnetHost> EnetHost::connect(const Address &to, std::string *error) {
	auto h = client(error);
	if (h) h->connect_to(to);
	if (h && !h->connecting_) {
		if (error) *error = "The connection could not be started.";
		return nullptr;
	}
	return h;
}

void EnetHost::connect_to(const Address &to) {
	if (connecting_ && connecting_->state != ENET_PEER_STATE_CONNECTED) {
		if (auto *p = static_cast<EnetPeer *>(connecting_->data)) p->peer_ = nullptr;
		connecting_->data = nullptr;
		enet_peer_reset(connecting_);
	}
	ENetAddress a{};
	a.host = to.ip;
	a.port = to.port;
	sent_to(to);
	connecting_ = enet_host_connect(host_, &a, kChannels, 0);
	if (connecting_) wrap(connecting_);
}

void EnetHost::send_raw(const Address &to, const std::vector<uint8_t> &data) {
	if (!host_) return;
	ENetAddress a{};
	a.host = to.ip;
	a.port = to.port;
	ENetBuffer b;
	b.data = const_cast<uint8_t *>(data.data());
	b.dataLength = data.size();
	sent_to(to);
	enet_socket_send(host_->socket, &a, &b, 1);
}

int EnetHost::add_raw_handler(std::function<bool(const Address &, const uint8_t *, size_t)> h) {
	raw_handlers_.emplace_back(next_handler_, std::move(h));
	return next_handler_++;
}

void EnetHost::remove_raw_handler(int id) {
	std::erase_if(raw_handlers_, [id](const auto &e) { return e.first == id; });
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

void EnetHost::fail_handshake(EnetPeer &p, const std::string &why) {
	p.refusal = why;
	const bool tell = p.announced_ || !server_;
	if (p.peer_) {
		enet_peer_disconnect_now(p.peer_, 0);
		p.peer_->data = nullptr;
		p.peer_ = nullptr;
	}
	p.held_.clear();
	p.pending_.clear();
	if (tell) events_.push_back({Event::Disconnected, &p});
}

// The host answers the hello; the client checks the answer. Then the peer is Connected, and what
// was sent before goes out sealed.
void EnetHost::handshake(EnetPeer &p, std::vector<uint8_t> data) {
	if (server_) {
		auto answer = identity_ ? p.secure_->answer(data, *identity_) : std::vector<uint8_t>{};
		if (answer.empty()) return fail_handshake(p, "The client's handshake was not understood.");
		push(p, Channel::Reliable, std::move(answer));
	} else {
		std::string why;
		if (!p.secure_->finish(data, expect_, &why)) return fail_handshake(p, why);
	}
	p.announced_ = true;
	events_.push_back({Event::Connected, &p});
	while (!p.pending_.empty()) {
		Packet q = std::move(p.pending_.front());
		p.pending_.pop_front();
		push(p, q.channel, p.secure_->seal(channel_id(q.channel), q.data));
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
			// No packet throttle (the WinSock service had none): it drops whole bursts of unsequenced packets.
			e.peer->packetThrottleDeceleration = 0;
			e.peer->packetThrottle = e.peer->packetThrottleLimit;
			if (e.peer == connecting_) connecting_ = nullptr;
			EnetPeer *p = wrap(e.peer);
			p->last_heard_ = p->connected_at_ = enet_time_get();
			p->secure_ = std::make_unique<SecureChannel>(server_ ? SecureChannel::Role::Host : SecureChannel::Role::Client);
			if (!server_) push(*p, Channel::Reliable, p->secure_->hello());
			break;
		}
		case ENET_EVENT_TYPE_RECEIVE: {
			if (auto *p = static_cast<EnetPeer *>(e.peer->data)) {
				p->last_heard_ = enet_time_get();
				p->bytes_in += e.packet->dataLength;
				std::vector<uint8_t> data(e.packet->data, e.packet->data + e.packet->dataLength);
				if (!p->secure_ || !p->secure_->ready()) {
					if (p->secure_ && e.channelID == 0) handshake(*p, std::move(data));
				} else {
					Packet in;
					in.channel = e.channelID == 0 ? Channel::Reliable : Channel::Unreliable;
					if (p->secure_->open(e.channelID, data, in.data)) p->inbox_.push_back(std::move(in));
				}
				if (p->interrupted) p->interrupted = false, events_.push_back({Event::Restored, p});
			}
			enet_packet_destroy(e.packet);
			break;
		}
		case ENET_EVENT_TYPE_DISCONNECT:
			if (e.peer == connecting_) connecting_ = nullptr;
			if (auto *p = static_cast<EnetPeer *>(e.peer->data)) {
				p->peer_ = nullptr;
				p->held_.clear();
				e.peer->data = nullptr;
				if (p->announced_ || !server_) events_.push_back({Event::Disconnected, p});
			}
			break;
		default: break;
		}
	}
	// ENet's own pings keep lastReceiveTime fresh while the other side is alive.
	for (auto &p : peers_) {
		if (!p->peer_ || p->peer_->state != ENET_PEER_STATE_CONNECTED) continue;
		if (!p->announced_) {
			if (enet_time_get() - p->connected_at_ > kHandshakeTime) fail_handshake(*p, "The connection's handshake did not finish.");
			continue;
		}
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
#if defined(_WIN32) && defined(SIO_UDP_CONNRESET)
	// Asking ports nobody listens on must not fill the socket with WSAECONNRESET.
	BOOL report = FALSE;
	DWORD bytes = 0;
	WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &bytes, nullptr, nullptr);
#endif
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
	if (sent_.size() > 1024) sent_.erase(sent_.begin(), sent_.begin() + 256);
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

void Discovery::query_broadcast(const std::vector<uint16_t> &ports) {
	for (uint16_t p : ports) query_broadcast(p);
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
