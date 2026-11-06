#include "game/netplay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "game/replay.h"
#include "game/runtime.h"
#include "game/session.h"

namespace game {

namespace {

// Packets: a header byte (0 a session message with a u16 number, 2 the clock sync; 0x0f our
// transport's ping, the WinSock node's own in the original), then the payload. The session messages
// keep the original numbers (docs/NETWORK_PLAN.md section 7.2).
enum Header : uint8_t { kSession = 0, kClock = 2, kTransportPing = 0x0f, kTransportPong = 0x0e };
enum Msg : uint16_t {
	kLink = 0x0d,       // {count, {server ID, ID string}}: AddFromServer
	kPreload = 0x0e,    // BOOL-ended {server ID, ID string, time, flags, newest state}
	kStates = 0x11,     // {server ID, time, flags, state}..., 0
	kAcks = 0x12,       // {server ID, raw time}..., 0
	kCorrection = 0x19, // server ID: send your version, flagged Correction
};

struct Writer {
	std::vector<uint8_t> d;
	void u8(uint8_t v) { d.push_back(v); }
	void u32(uint32_t v) { d.insert(d.end(), (uint8_t *)&v, (uint8_t *)&v + 4); }
	void squeezed(uint32_t v) {
		if (v < 0xFE) {
			u8(uint8_t(v));
		} else if (v < 0xFFFF) {
			u8(0xFE), d.push_back(uint8_t(v)), d.push_back(uint8_t(v >> 8));
		} else {
			u8(0xFF), u32(v);
		}
	}
	void bytes(const void *p, size_t n) { d.insert(d.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
};

struct Reader {
	const std::vector<uint8_t> &d;
	size_t pos = 0;
	bool ok = true;
	bool need(size_t n) {
		if (pos + n > d.size()) ok = false;
		return ok;
	}
	uint8_t u8() { return need(1) ? d[pos++] : 0; }
	uint32_t u32() {
		uint32_t v = 0;
		if (need(4)) memcpy(&v, d.data() + pos, 4), pos += 4;
		return v;
	}
	uint32_t squeezed() {
		uint8_t b = u8();
		if (b < 0xFE) return b;
		if (b == 0xFE) {
			if (!need(2)) return 0;
			uint32_t v = d[pos] | (uint32_t(d[pos + 1]) << 8);
			pos += 2;
			return v;
		}
		return u32();
	}
	std::vector<uint8_t> take(size_t n) {
		if (!need(n)) return {};
		std::vector<uint8_t> v(d.begin() + long(pos), d.begin() + long(pos + n));
		pos += n;
		return v;
	}
};

// The flags byte (ReplicateSessionStateManagerStates): authority, significance, NoInterrupt (0x10),
// TimeCritical (0x20), Correction (0x40).
uint8_t pack_flags(const NetState &s) {
	return uint8_t((s.authority & 3) | ((s.significance & 3) << 2) | ((s.flags & 2) ? 0x10 : 0) | ((s.flags & 8) ? 0x20 : 0) |
	               ((s.flags & 0x10) ? 0x40 : 0));
}
void unpack_flags(uint8_t b, NetState &s) {
	s.authority = b & 3;
	s.significance = (b >> 2) & 3;
	s.flags = ((b & 0x10) ? 2 : 0) | ((b & 0x20) ? 8 : 0) | ((b & 0x40) ? 0x10 : 0);
}

// The 2.123 sender (FINDINGS 405): ladder weights by resend index, significance factors, and per
// urgency class the keep-alive interval and how often the class is visited.
constexpr float kWeight[8] = {1, 18, 30, 70, 125, 200, 500, 500};
constexpr float kSignificance[4] = {1, 0.6f, 0.3f, 0.2f};
constexpr uint32_t kKeepAlive[5] = {10000, 5000, 1000, 700, 400};
constexpr uint32_t kVisit[5] = {1000, 300, 100, 50, 0};
constexpr size_t kMaxPacket = 500;

Writer message(Msg m) {
	Writer wr;
	wr.u8(kSession);
	wr.u8(uint8_t(m)), wr.u8(uint8_t(m >> 8));
	return wr;
}

}  // namespace

NetSession::NetSession(World &w, NetRole role, NetConfig cfg) : w_(w), role_(role), cfg_(cfg) {
	net_managers_begin(w, role, w.session.get());
}

NetSession::~NetSession() { net_managers_end(w_); }

NetSession::Peer &NetSession::add_peer(net::Endpoint *link, GameObject *creature) {
	Peer &p = peers_.emplace_back();
	p.link = link;
	p.creature = creature;
	return p;
}

uint64_t NetSession::bytes_sent() const {
	uint64_t n = 0;
	for (auto &p : peers_) n += p.bytes_sent;
	return n;
}

uint32_t NetSession::frame_ms(uint32_t real_ms) {
	int32_t out = std::max<int32_t>(0, int32_t(real_ms) + slew_pending);
	slew_pending -= out - int32_t(real_ms);
	return uint32_t(out);
}

// The service's budget credit loses (bytes + 20) / Maximum Outgoing Bytes a packet and refills in a
// second; the sender divides its intervals by 1 - (1 - credit)^2 times the node's quality.
void NetSession::send(Peer &p, net::Channel ch, std::vector<uint8_t> data) {
	credit = std::max(0.0f, credit - float(data.size() + 20) / float(cfg_.budget));
	p.bytes_sent += data.size();
	++packets_sent;
	p.link->send(ch, std::move(data));
}

float NetSession::quality() const { return std::max(0.01f, (1 - (1 - credit) * (1 - credit)) * std::min(cfg_.node_quality, 1.0f)); }

// UpdateUnconnectedSessionStateManagers and the pre-load (0x0d, 0x0e): the server names each manager
// to each client, and sends the newest state of those its pre-load needs (the client's status
// machine is phase 2: here whenever that state changes).
void NetSession::announce(Peer &p) {
	Writer links = message(kLink), pre = message(kPreload);
	bool any_pre = false;
	auto managers = net_managers(w_);
	std::vector<const NetManager *> fresh;
	for (auto &m : managers) {
		if (m.server_id && !p.linked[m.server_id]) fresh.push_back(&m), p.linked[m.server_id] = true;
	}
	links.squeezed(uint32_t(fresh.size()));
	for (auto *m : fresh) {
		links.squeezed(m->server_id);
		links.squeezed(uint32_t(m->id.size()));
		links.bytes(m->id.data(), m->id.size());
	}
	const bool n = !fresh.empty();
	for (auto &m : managers) {
		if (!m.server_id) continue;
		NetState st;
		if (!m.preload || !net_newest_state(w_, m.owner, st) || p.preloaded[m.owner] == st.serial) continue;
		p.preloaded[m.owner] = st.serial;
		pre.u8(1);
		pre.squeezed(m.server_id);
		pre.squeezed(uint32_t(m.id.size()));
		pre.bytes(m.id.data(), m.id.size());
		pre.squeezed(st.time);
		pre.u8(uint8_t((st.authority & 3) | ((st.significance & 3) << 2)));
		pre.u32(uint32_t(st.bytes.size()));
		pre.bytes(st.bytes.data(), st.bytes.size());
		any_pre = true;
	}
	if (n) send(p, net::Channel::Reliable, std::move(links.d));
	if (any_pre) {
		pre.u8(0);
		send(p, net::Channel::Reliable, std::move(pre.d));
	}
}

// Message 0x12, unreliable: 1000 ms after the first pair, or once 1.4 x that age passes the smallest
// keep-alive interval of the acknowledged managers (FINDINGS 398).
void NetSession::send_acks(Peer &p, uint32_t now, bool force) {
	if (p.acks.empty()) return;
	uint32_t age = now - p.acks_since;
	if (!force && age < 1000 && 1.4f * float(age) <= float(p.keep_alive_min)) return;
	Writer wr = message(kAcks);
	for (auto &[id, t] : p.acks) wr.squeezed(id), wr.squeezed(t);
	wr.squeezed(0);
	p.acks.clear();
	send(p, net::Channel::Unreliable, std::move(wr.d));
}

void NetSession::receive(Peer &p, const net::Packet &packet) {
	Reader r{packet.data};
	const uint32_t now = w_.global_ms();
	uint8_t header = r.u8();
	if (header == kTransportPing) {
		Writer wr;
		wr.u8(kTransportPong);
		wr.u32(r.u32());
		p.link->send(net::Channel::Unreliable, std::move(wr.d));
		return;
	}
	if (header == kTransportPong) {
		int32_t rtt = int32_t(now - r.u32());
		if (r.ok && rtt >= 0) p.ping = p.ping == 0 ? float(rtt) : p.ping + (float(rtt) - p.ping) * 0.25f;
		return;
	}
	if (header == kClock) {
		if (role_ == NetRole::Server) {
			uint32_t sent = r.u32();
			uint8_t seq = r.u8();
			Writer wr;
			wr.u8(kClock);
			wr.u32(sent);
			wr.u32(p.creature ? w_.precise_ms_of(p.creature) : 0);
			wr.u8(seq);
			send(p, net::Channel::Unreliable, std::move(wr.d));
			return;
		}
		uint32_t sent = r.u32(), server_scene = r.u32();
		uint8_t seq = r.u8();
		if (!r.ok || seq != sync_seq_ || !p.creature) return;
		int32_t rtt = int32_t(now - sent);
		if (rtt < 0) return;
		// The accept limit (+0x64): over it the sample goes and the limit grows by 25; under two
		// thirds of it the limit becomes 50 + 1.5 x the round trip (FINDINGS 403).
		if (rtt > sync_limit_) {
			sync_limit_ += 25;
			++clock_rejects;
			return;
		}
		if (1.5f * float(rtt) < float(sync_limit_)) sync_limit_ = int32_t(50 + 1.5f * float(rtt));
		double scale = w_.scene_clock(w_.clock_key(p.creature)).scale;
		int64_t target = int64_t(server_scene) + int64_t(double(rtt) * 0.5 * scale);
		int64_t here = int64_t(w_.scene_ms_of(p.creature)) + slew_pending;
		double d = double(target - here);
		if (std::fabs(d) <= 1000) {
			if (std::fabs(d) < 120) {
				d *= 0.5;
				if (std::fabs(d) < 40) d *= 0.5;
				if (std::fabs(d) < 10) d *= 0.5;
			}
		} else {
			++clock_jumps;
		}
		slew_pending += int32_t(d);
		return;
	}
	if (header != kSession) return;
	uint16_t msg = uint16_t(r.u8() | (r.u8() << 8));
	switch (msg) {
	case kLink: {
		uint32_t n = r.squeezed();
		for (uint32_t i = 0; i < n && r.ok; ++i) {
			uint32_t id = r.squeezed();
			auto b = r.take(r.squeezed());
			if (r.ok && role_ == NetRole::Client) net_link(w_, id, std::string(b.begin(), b.end()));
		}
		break;
	}
	case kPreload:
		while (r.u8() == 1 && r.ok) {
			uint32_t id = r.squeezed();
			auto b = r.take(r.squeezed());
			NetState st;
			st.time = r.squeezed();
			unpack_flags(r.u8() & 0x0f, st);
			st.bytes = r.take(r.u32());
			if (!r.ok || role_ != NetRole::Client) break;
			net_link(w_, id, std::string(b.begin(), b.end()));
			if (GameObject *owner = net_owner(w_, id)) net_receive_state(w_, owner, st, lag_ms);
		}
		break;
	case kStates:
		for (;;) {
			uint32_t id = r.squeezed();
			if (!id || !r.ok) break;
			NetState st;
			st.time = r.squeezed();
			unpack_flags(r.u8(), st);
			st.bytes = r.take(r.u32());
			if (!r.ok) break;
			GameObject *owner = net_owner(w_, id);
			// The server takes from a client only the states of the creature it controls.
			if (!owner || (role_ == NetRole::Server && owner != p.creature)) continue;
			NetTake took = net_receive_state(w_, owner, st, lag_ms);
			if (took == NetTake::Floor) continue;
			if (p.acks.empty()) p.acks_since = now, p.keep_alive_min = ~0u;
			p.acks.emplace_back(id, st.time);
			for (auto &m : net_managers(w_)) {
				if (m.owner == owner) p.keep_alive_min = std::min(p.keep_alive_min, kKeepAlive[m.urgency]);
			}
			// Rejected with nothing held: this side's own newest state goes back.
			if (took == NetTake::Rejected) p.records[owner].acked = -1;
		}
		break;
	case kAcks:
		for (;;) {
			uint32_t id = r.squeezed();
			if (!id || !r.ok) break;
			uint32_t t = r.squeezed();
			GameObject *owner = net_owner(w_, id);
			if (!owner) continue;
			auto &rec = p.records[owner];
			if (rec.time == t) rec.acked = t, rec.ever_acked = true;
			net_acked(w_, owner, t);
		}
		break;
	case kCorrection: {
		GameObject *owner = net_owner(w_, r.squeezed());
		if (!owner || role_ != NetRole::Server) break;
		auto &rec = p.records[owner];
		rec.force = true;
		rec.acked = -1;
		if (rec.index > 0) --rec.index;
		break;
	}
	default: break;
	}
}

// ReplicateSessionStateManagerStates as 2.123 has it (Session 0x1100a660, FINDINGS 405), per peer: a
// client sends the creatures it controls whose managers replicate up, the server every manager that
// replicates down, but a client's own creature only as corrections. The urgency classes are visited
// from the most urgent down to the first not due; a state is due when (lag + 50) x weight x distance x
// 0.003 / (2 x quality), times the significance factor, is at most (1 - a) + a x elapsed / keep-alive
// (a = 0.6 x quality + 0.4), tripled while unacknowledged and quadrupled again while nothing ever was.
// Authority under 2 only advances the index. One packet a pass, at most 500 bytes.
void NetSession::send_states(Peer &p, uint32_t now) {
	struct Item {
		const GameObject *owner;
		uint32_t id;
		NetState st;
		uint32_t urgency;
	};
	bool visit[5] = {false, false, false, false, true};
	for (int c = 3; c >= 0; --c) {
		if (now - class_visit_[c] < kVisit[c]) break;
		visit[c] = true;
	}
	const float q = quality(), a = 0.6f * q + 0.4f;
	std::vector<Item> due;
	auto managers = net_managers(w_);
	std::stable_sort(managers.begin(), managers.end(), [](const NetManager &x, const NetManager &y) { return x.urgency > y.urgency; });
	for (auto &m : managers) {
		if (!m.server_id || !visit[m.urgency]) continue;
		const bool server = role_ == NetRole::Server;
		bool replicate = server ? m.down : m.up && locally_controlled(w_, m.owner);
		auto &rec = p.records[m.owner];
		if (net_take_resend(w_, m.owner)) {
			if (server) {
				rec.force = true, rec.acked = -1, rec.index = 0;
			} else {
				Writer wr = message(kCorrection);
				wr.squeezed(m.server_id);
				send(p, net::Channel::Unreliable, std::move(wr.d));
				++correction_requests;
			}
		}
		if (!replicate) continue;
		NetState st;
		if (!net_newest_state(w_, m.owner, st)) continue;
		if (rec.serial != st.serial) rec.serial = st.serial, rec.time = st.time, rec.index = 0;
		const bool own = server && m.owner == p.creature;
		const bool correction = (st.flags & 0x10) || rec.force;
		if (own && !correction) continue;
		const uint32_t keep = kKeepAlive[m.urgency];
		const bool acked = rec.acked == int64_t(rec.time);
		if (acked && !rec.force) {
			if (now - rec.last_send >= keep) due.push_back({m.owner, m.server_id, std::move(st), m.urgency});
			continue;
		}
		if (st.authority < 2 && !correction) {
			if (rec.index < 7) ++rec.index;
			rec.last_send = now;
			continue;
		}
		float dist = 1;
		const GameObject *viewer = p.creature;
		if (viewer && m.owner->is_actor && viewer != m.owner && in_scene(*m.owner)) {
			auto x = m.owner->world().t, y = viewer->world().t;
			float d = std::sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2]));
			dist = std::min(1 + d / 180.0f, 4.0f);
		}
		float interval = (lag_ms + 50) * kWeight[rec.index] * dist * 0.003f / (2 * q) * kSignificance[st.significance & 3];
		if (rec.force && !own) interval = 1;
		float when = (1 - a) + a * float(now - rec.last_send) / float(keep);
		if (rec.acked == -1) when *= 3;
		if (!rec.ever_acked) when *= 4;
		if (interval <= when) due.push_back({m.owner, m.server_id, std::move(st), m.urgency});
	}
	for (int c = 0; c < 4; ++c) {
		if (visit[c]) class_visit_[c] = now;
	}
	if (due.empty()) return;
	Writer wr = message(kStates);
	size_t n = 0;
	for (auto &it : due) {
		if (n && wr.d.size() + it.st.bytes.size() + 12 > kMaxPacket) break;
		auto &rec = p.records[it.owner];
		if (rec.force) it.st.flags |= 0x10;
		wr.squeezed(it.id);
		wr.squeezed(it.st.time);
		wr.u8(pack_flags(it.st));
		wr.u32(uint32_t(it.st.bytes.size()));
		wr.bytes(it.st.bytes.data(), it.st.bytes.size());
		if (rec.index < 7) ++rec.index;
		rec.last_send = now;
		rec.force = false;
		if (role_ == NetRole::Server) net_sent(w_, it.owner, it.st.time);
		++n;
	}
	wr.squeezed(0);
	states_sent += n;
	send(p, net::Channel::Unreliable, std::move(wr.d));
}

void NetSession::update() {
	const uint32_t now = w_.global_ms();
	credit = std::min(1.0f, credit + 0.001f * float(now - last_frame_));
	last_frame_ = now;
	for (auto &p : peers_) {
		net::Packet packet;
		while (p.link->receive(packet)) receive(p, packet);
	}
	// AdjustLag (Session 0x4500b6d0, FINDINGS 397) once a second: 10 ms plus half the ping at the
	// scene's scale; a client sets it on every manager, the server on each client's creature's.
	if (now - last_lag_ >= 1000) {
		last_lag_ = now;
		for (auto &p : peers_) {
			Writer wr;
			wr.u8(kTransportPing);
			wr.u32(now);
			p.link->send(net::Channel::Unreliable, std::move(wr.d));
			double scale = p.creature ? w_.scene_clock(w_.clock_key(p.creature)).scale : 1;
			float lag = 10 + float(scale) * p.ping * 0.5f;
			if (role_ == NetRole::Client) {
				lag_ms = lag;
				net_set_lag(w_, lag);
			} else if (p.creature) {
				lag_ms = lag;
				net_set_lag(w_, lag, p.creature);
			}
		}
		if (role_ == NetRole::Client && !peers_.empty() && peers_[0].creature) {
			Writer wr;
			wr.u8(kClock);
			wr.u32(now);
			wr.u8(++sync_seq_);
			wr.squeezed(1000);
			send(peers_[0], net::Channel::Unreliable, std::move(wr.d));
		}
	}
	for (auto &p : peers_) {
		if (role_ == NetRole::Server) announce(p);
		send_states(p, now);
		send_acks(p, now, false);
	}
}

}  // namespace game
