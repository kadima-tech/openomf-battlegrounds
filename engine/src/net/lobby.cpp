#include "net/lobby.h"

#include <enet/enet.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace net {

namespace {

constexpr uint8_t kMagic[4] = {'O', 'B', 'G', 'L'};
constexpr uint8_t kVersion = 1;
constexpr size_t kMinList = 128;
constexpr uint8_t kStatusOK = 0, kStatusUnknownHost = 1, kStatusNoRelay = 2;

struct W {
	std::vector<uint8_t> &d;
	void u8(uint8_t v) { d.push_back(v); }
	void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
	void u32(uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); }
	void u64(uint64_t v) { u32(uint32_t(v)), u32(uint32_t(v >> 32)); }
	void bytes(const void *p, size_t n) { d.insert(d.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
	void str(const std::string &s) {
		size_t n = std::min<size_t>(s.size(), 255);
		u16(uint16_t(n));
		bytes(s.data(), n);
	}
	void addr(const Address &a) {
		bytes(&a.ip, 4);
		u16(a.port);
	}
	void info(const LobbyInfo &i) {
		u16(i.protocol);
		str(i.version);
		bytes(i.data_hash.data(), 16);
		str(i.server_name);
		str(i.session_name);
		u8(i.players), u8(i.max_players), u8(i.flags);
		addr(i.local);
		bytes(i.identity.data(), 32);
		u32(i.instance);
	}
};

struct R {
	const uint8_t *d;
	size_t n, pos;
	bool ok = true;
	bool need(size_t k) {
		if (pos + k > n) ok = false;
		return ok;
	}
	uint8_t u8() { return need(1) ? d[pos++] : 0; }
	uint16_t u16() {
		uint16_t lo = u8();
		return uint16_t(lo | (u8() << 8));
	}
	uint32_t u32() {
		uint32_t lo = u16();
		return lo | uint32_t(u16()) << 16;
	}
	uint64_t u64() {
		uint64_t lo = u32();
		return lo | uint64_t(u32()) << 32;
	}
	void bytes(void *out, size_t k) {
		if (need(k)) memcpy(out, d + pos, k), pos += k;
	}
	std::string str() {
		size_t k = u16();
		if (!need(k)) return {};
		std::string s((const char *)d + pos, k);
		pos += k;
		return s;
	}
	Address addr() {
		Address a{0, 0};
		bytes(&a.ip, 4);
		a.port = u16();
		return a;
	}
	LobbyInfo info() {
		LobbyInfo i;
		i.protocol = u16();
		i.version = str();
		bytes(i.data_hash.data(), 16);
		i.server_name = str();
		i.session_name = str();
		i.players = u8(), i.max_players = u8(), i.flags = u8();
		i.local = addr();
		bytes(i.identity.data(), 32);
		i.instance = u32();
		return i;
	}
};

uint64_t random64() {
	uint64_t v = 0;
	random_bytes(&v, sizeof v);
	return v;
}

bool valid(const Address &a) { return a.ip != 0 && a.port != 0; }

// This machine's address on the route to `to` (a connected UDP socket sends nothing).
uint32_t local_ip_towards(const Address &to) {
	ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (s == ENET_SOCKET_NULL) return 0;
	ENetAddress a{};
	a.host = to.ip;
	a.port = to.port;
	uint32_t ip = 0;
	if (enet_socket_connect(s, &a) == 0) {
		ENetAddress mine{};
		if (enet_socket_get_address(s, &mine) == 0) ip = mine.host;
	}
	enet_socket_destroy(s);
	return ip;
}

}  // namespace

std::vector<uint8_t> lobby_message(LobbyMsg m) {
	std::vector<uint8_t> d(kMagic, kMagic + 4);
	d.push_back(kVersion);
	d.push_back(uint8_t(m));
	return d;
}

bool is_lobby_datagram(const uint8_t *d, size_t n) { return n >= 6 && memcmp(d, kMagic, 4) == 0 && d[4] == kVersion; }

bool lobby_parse(const uint8_t *d, size_t n, LobbyMsg &type, size_t &body) {
	if (!is_lobby_datagram(d, n)) return false;
	type = LobbyMsg(d[5]);
	body = 6;
	return true;
}

// --- Host -----------------------------------------------------------------------------------

LobbyHost::LobbyHost(EnetHost &host, Address lobby) : host_(host), lobby_(lobby) {
	host_.allow(lobby_);
	handler_ = host_.add_raw_handler([this](const Address &from, const uint8_t *d, size_t n) { return raw(from, d, n); });
	status = "registering with the lobby at " + lobby_.text();
}

LobbyHost::~LobbyHost() {
	if (id_) {
		auto m = lobby_message(LobbyMsg::Unregister);
		W w{m};
		w.u32(id_);
		w.u64(cookie_);
		host_.send_raw(lobby_, m);
	}
	host_.remove_raw_handler(handler_);
}

void LobbyHost::send_register() {
	LobbyInfo i = info ? info() : LobbyInfo{};
	if (!i.local.ip) i.local.ip = local_ip_towards(lobby_);
	auto m = lobby_message(LobbyMsg::Register);
	W w{m};
	w.u64(cookie_);
	w.info(i);
	host_.send_raw(lobby_, m);
	last_sent_ = now_ms();
}

void LobbyHost::update() {
	const uint32_t now = now_ms();
	if (id_ && last_heard_ && now - last_heard_ > 4 * interval_) {
		status = "the lobby stopped answering; registering again";
		id_ = 0, cookie_ = 0;
	}
	if (now - last_sent_ >= (id_ ? interval_ : 1000)) send_register();
	for (auto &p : punches_) {
		if (int32_t(now - p.until) > 0 || now - p.last < 100) continue;
		p.last = now;
		auto m = lobby_message(LobbyMsg::Punch);
		W w{m};
		w.u64(p.nonce);
		for (auto &a : p.to) host_.send_raw(a, m);
	}
	std::erase_if(punches_, [now](const Punch &p) { return int32_t(now - p.until) > 0; });
	for (auto &b : bindings_) {
		if (b.bound || now - b.last < 250) continue;
		b.last = now;
		auto m = lobby_message(LobbyMsg::Bind);
		W w{m};
		w.u64(b.token);
		host_.send_raw(b.relay, m);
	}
}

std::optional<Address> LobbyHost::relayed_client(const Address &relay) const {
	for (auto &b : bindings_) {
		if (b.relay == relay) return b.client;
	}
	return std::nullopt;
}

bool LobbyHost::raw(const Address &from, const uint8_t *d, size_t n) {
	LobbyMsg t;
	size_t body;
	if (!lobby_parse(d, n, t, body)) return false;
	R r{d, n, body};
	const uint32_t now = now_ms();
	if (t == LobbyMsg::Punch) return true;
	if (t == LobbyMsg::Bound) {
		uint64_t token = r.u64();
		for (auto &b : bindings_) {
			if (r.ok && b.token == token && b.relay == from) b.bound = true;
		}
		return true;
	}
	if (!(from == lobby_)) return false;
	last_heard_ = now;
	switch (t) {
	case LobbyMsg::Challenge:
		cookie_ = r.u64();
		if (r.ok) send_register();
		return true;
	case LobbyMsg::Registered: {
		uint32_t id = r.u32();
		uint16_t interval = r.u16();
		Address pub = r.addr();
		if (!r.ok) return true;
		if (id != id_) printf("net: listed on the lobby at %s as %u (public address %s)\n", lobby_.text().c_str(), id, pub.text().c_str());
		id_ = id;
		interval_ = std::max<uint32_t>(1000, interval);
		status = "listed";
		return true;
	}
	case LobbyMsg::Peer: {
		Punch p;
		p.nonce = r.u64();
		Address pub = r.addr(), local = r.addr();
		if (!r.ok) return true;
		if (valid(pub)) p.to.push_back(pub);
		if (valid(local) && !(local == pub)) p.to.push_back(local);
		p.until = now + 5000;
		punches_.push_back(p);
		return true;
	}
	case LobbyMsg::Relay: {
		r.u64();
		uint8_t st = r.u8();
		uint16_t port = r.u16();
		uint64_t token = r.u64();
		Address client = r.addr();
		if (!r.ok || st != kStatusOK) return true;
		Address relay{lobby_.ip, port};
		auto it = std::find_if(bindings_.begin(), bindings_.end(), [&](const Binding &b) { return b.relay == relay && b.token == token; });
		if (it != bindings_.end()) return true;
		std::erase_if(bindings_, [&](const Binding &b) { return b.relay == relay; });
		host_.allow(relay);
		bindings_.push_back({relay, client, token});
		printf("net: relaying a player from %s through the lobby's port %u\n", client.text().c_str(), port);
		return true;
	}
	case LobbyMsg::Error:
		r.u8();
		status = "the lobby refused: " + r.str();
		printf("net: %s\n", status.c_str());
		return true;
	default: return true;
	}
}

// --- Join -----------------------------------------------------------------------------------

LobbyJoin::LobbyJoin(EnetHost &host, Address lobby, const LobbyEntry &target, bool force_relay)
    : host_(host), lobby_(lobby), target_(target), force_relay_(force_relay) {
	host_.allow(lobby_);
	host_.expect_identity(target.info.identity);
	handler_ = host_.add_raw_handler([this](const Address &from, const uint8_t *d, size_t n) { return raw(from, d, n); });
	nonce_ = random64();
	started_ = phase_at_ = now_ms();
}

LobbyJoin::~LobbyJoin() { host_.remove_raw_handler(handler_); }

void LobbyJoin::connect(const Address &to) {
	printf("net: connecting to %s%s\n", to.text().c_str(), relayed_ ? " (the lobby's relay)" : "");
	host_.connect_to(to);
}

void LobbyJoin::connected() { phase_ = Phase::Connected; }

void LobbyJoin::update() {
	const uint32_t now = now_ms();
	if (phase_ == Phase::Connected || phase_ == Phase::Failed) return;
	if (now - started_ > kGiveUp) {
		phase_ = Phase::Failed;
		error = "No connection to the server could be made, directly or through the lobby's relay.";
		return;
	}
	switch (phase_) {
	case Phase::Introducing:
		if (now - phase_at_ > 5000) {
			phase_ = Phase::Failed;
			error = "The lobby at " + lobby_.text() + " does not answer.";
			return;
		}
		if (now - last_sent_ >= 500) {
			auto m = lobby_message(LobbyMsg::Connect);
			W w{m};
			w.u32(target_.id);
			w.u64(nonce_);
			w.addr({local_ip_towards(lobby_), host_.port()});
			host_.send_raw(lobby_, m);
			last_sent_ = now;
		}
		break;
	case Phase::Punching:
		if (now - phase_at_ > kPunchTime) {
			printf("net: no direct connection after %u ms; asking the lobby for a relay\n", kPunchTime);
			phase_ = Phase::Relaying, phase_at_ = now, last_sent_ = 0;
			return;
		}
		if (now - last_sent_ >= 100) {
			auto m = lobby_message(LobbyMsg::Punch);
			W w{m};
			w.u64(nonce_);
			for (auto &a : punch_to_) host_.send_raw(a, m);
			last_sent_ = now;
		}
		break;
	case Phase::Relaying:
		if (now - last_sent_ < (token_ ? 250u : 500u)) break;
		last_sent_ = now;
		if (!token_) {
			auto m = lobby_message(LobbyMsg::RelayRequest);
			W w{m};
			w.u32(target_.id);
			w.u64(nonce_);
			host_.send_raw(lobby_, m);
		} else if (!bound_) {
			auto m = lobby_message(LobbyMsg::Bind);
			W w{m};
			w.u64(token_);
			host_.send_raw(relay_, m);
		}
		break;
	default: break;
	}
}

bool LobbyJoin::raw(const Address &from, const uint8_t *d, size_t n) {
	LobbyMsg t;
	size_t body;
	if (!lobby_parse(d, n, t, body)) return false;
	R r{d, n, body};
	const uint32_t now = now_ms();
	if (t == LobbyMsg::Punch) return true;
	if (t == LobbyMsg::Bound) {
		if (r.u64() == token_ && r.ok && from == relay_ && !bound_) {
			bound_ = true;
			relayed_ = true;
			connect(relay_);
		}
		return true;
	}
	if (!(from == lobby_)) return false;
	if (t == LobbyMsg::Intro && phase_ == Phase::Introducing) {
		if (r.u64() != nonce_) return true;
		uint8_t st = r.u8();
		Address pub = r.addr(), local = r.addr(), mine = r.addr();
		if (!r.ok) return true;
		if (st != kStatusOK) {
			phase_ = Phase::Failed;
			error = st == kStatusUnknownHost ? "The session is no longer listed." : "The lobby refused the introduction.";
			return true;
		}
		punch_to_.clear();
		if (valid(pub)) punch_to_.push_back(pub);
		if (valid(local) && !(local == pub)) punch_to_.push_back(local);
		// Behind the same NAT as the host, its own address is the one that works.
		const Address direct = valid(local) && pub.ip == mine.ip ? local : pub;
		phase_at_ = now, last_sent_ = 0;
		if (force_relay_) {
			printf("net: relay forced (OPENBG_FORCE_RELAY)\n");
			phase_ = Phase::Relaying;
			return true;
		}
		phase_ = Phase::Punching;
		connect(direct);
		return true;
	}
	if (t == LobbyMsg::Relay && phase_ == Phase::Relaying && !token_) {
		if (r.u64() != nonce_) return true;
		uint8_t st = r.u8();
		uint16_t port = r.u16();
		uint64_t token = r.u64();
		if (!r.ok) return true;
		if (st != kStatusOK) {
			phase_ = Phase::Failed;
			error = st == kStatusNoRelay ? "No direct connection could be made, and the lobby has no relay free." : "The session is no longer listed.";
			return true;
		}
		relay_ = {lobby_.ip, port};
		token_ = token;
		host_.allow(relay_);
		last_sent_ = 0;
		return true;
	}
	if (t == LobbyMsg::Error) {
		r.u8();
		phase_ = Phase::Failed;
		error = "The lobby refused: " + r.str();
		return true;
	}
	return true;
}

// --- List -----------------------------------------------------------------------------------

LobbyList::LobbyList(Address lobby, uint16_t protocol, uint32_t refresh_ms) : lobby_(lobby), protocol_(protocol), refresh_ms_(refresh_ms) {
	ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (s == ENET_SOCKET_NULL) {
		status = "No Socket";
		return;
	}
	enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
	ENetAddress any{};
	any.host = ENET_HOST_ANY;
	if (enet_socket_bind(s, &any) != 0) {
		enet_socket_destroy(s);
		status = "No Socket";
		return;
	}
	socket_ = intptr_t(s);
	refresh();
}

LobbyList::~LobbyList() {
	if (socket_ >= 0) enet_socket_destroy(ENetSocket(socket_));
}

void LobbyList::ask(uint16_t first) {
	auto m = lobby_message(LobbyMsg::List);
	W w{m};
	w.u32(seq_);
	w.u16(protocol_);
	w.u16(first);
	while (m.size() < kMinList) m.push_back(0);
	ENetAddress a{};
	a.host = lobby_.ip;
	a.port = lobby_.port;
	ENetBuffer b;
	b.data = m.data();
	b.dataLength = m.size();
	enet_socket_send(ENetSocket(socket_), &a, &b, 1);
	page_at_ = now_ms();
}

void LobbyList::refresh() {
	if (socket_ < 0) return;
	++seq_;
	building_.clear();
	asked_at_ = now_ms();
	answered_ = false;
	if (status.empty() || status == "Getting Server List") status = "Getting Server List";
	ask(0);
}

bool LobbyList::update() {
	if (socket_ < 0) return false;
	const uint32_t now = now_ms();
	bool changed = false;
	uint8_t buf[2048];
	for (;;) {
		ENetAddress from{};
		ENetBuffer b;
		b.data = buf;
		b.dataLength = sizeof buf;
		int n = enet_socket_receive(ENetSocket(socket_), &from, &b, 1);
		if (n <= 0) break;
		LobbyMsg t;
		size_t body;
		if (from.host != lobby_.ip || from.port != lobby_.port || !lobby_parse(buf, size_t(n), t, body)) continue;
		R r{buf, size_t(n), body};
		if (t == LobbyMsg::Error) {
			r.u8();
			status = r.str();
			continue;
		}
		if (t != LobbyMsg::ListReply || r.u32() != seq_) continue;
		uint16_t total = r.u16(), first = r.u16();
		uint8_t count = r.u8();
		if (!r.ok || first != building_.size()) continue;
		if (!answered_) ping_ms = now - asked_at_, answered_ = true;
		for (uint8_t i = 0; i < count && r.ok; ++i) {
			LobbyEntry e;
			e.id = r.u32();
			e.public_addr = r.addr();
			e.info = r.info();
			if (r.ok) building_.push_back(std::move(e));
		}
		if (!r.ok) {
			status = "Data Error";
			continue;
		}
		if (building_.size() < total && count > 0) {
			ask(uint16_t(building_.size()));
			continue;
		}
		entries_ = building_;
		changed = true;
		status = "Servers: " + std::to_string(entries_.size());
	}
	if (!answered_ && now - asked_at_ > 3000 && status != "No Connect") status = "No Connect", changed = true;
	if (now - asked_at_ > refresh_ms_) refresh();
	else if (answered_ && building_.size() && now - page_at_ > 1000 && building_.size() != entries_.size()) ask(uint16_t(building_.size()));
	return changed;
}

}  // namespace net
