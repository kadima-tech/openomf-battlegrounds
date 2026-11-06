#include "game/netplay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "game/replay.h"
#include "game/runtime.h"
#include "game/session.h"

namespace game {

namespace {

// Our wire messages; each names the original com-interface message it stands for.
enum Msg : uint8_t {
	kLink = 1,       // the server's ID for a manager ID (the 0x15 request / 0x13 answer)
	kStates = 2,     // states (0x11): manager ID, time, flags byte, packed state; ID 0 ends
	kSync = 3,       // SequenceSceneTime (Session 0x4500b5d0): the client's clock and a sequence byte
	kSyncReply = 4,  // the server's scene time for it (ProcessPacket, Session 0x45006490)
	kPing = 5,       // the round trip the lag is made of (the net node's ping)
	kPong = 6,
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

// The re-send ladder by CSignificantState +0xc (0: a new state goes at once).
constexpr float kLadder[8] = {1, 18, 30, 70, 125, 200, 500, 500};
// Significance 1-3 shorten the interval (the sender's multipliers, read from 0x4500a020).
constexpr float kSignificance[4] = {1, 0.6f, 0.3f, 0.1f};

// The scene the clocks are kept in: the one the client's creature plays in (the match's).
const GameObject *clock_object(World &w, const GameObject *creature) {
	if (creature) return w.clock_key(creature);
	return w.session ? w.session->obj : nullptr;
}

}  // namespace

NetSession::NetSession(World &w, NetRole role, NetConfig cfg) : w_(w), role_(role), cfg_(cfg) {
	net_managers_begin(w, role, w.session.get());
	net_set_lag(w, lag_ms);
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

void NetSession::announce(Peer &p) {
	for (auto &m : net_managers(w_)) {
		if (!m.server_id || p.linked[m.server_id]) continue;
		Writer wr;
		wr.u8(kLink);
		wr.squeezed(m.server_id);
		wr.squeezed(uint32_t(m.id.size()));
		wr.bytes(m.id.data(), m.id.size());
		p.link->send(net::Channel::Reliable, std::move(wr.d));
		p.linked[m.server_id] = true;
	}
}

void NetSession::receive(Peer &p, const net::Packet &packet) {
	Reader r{packet.data};
	switch (r.u8()) {
	case kLink: {
		uint32_t id = r.squeezed();
		auto b = r.take(r.squeezed());
		if (r.ok && role_ == NetRole::Client) net_link(w_, id, std::string(b.begin(), b.end()));
		break;
	}
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
			net_receive_state(w_, owner, st);
		}
		break;
	case kSync: {
		uint32_t sent = r.u32();
		uint8_t seq = r.u8();
		Writer wr;
		wr.u8(kSyncReply);
		wr.u32(sent);
		wr.u32(p.creature ? w_.scene_ms_of(p.creature) : 0);
		wr.u8(seq);
		p.link->send(net::Channel::Unreliable, std::move(wr.d));
		break;
	}
	case kSyncReply: {
		uint32_t sent = r.u32(), server_scene = r.u32();
		uint8_t seq = r.u8();
		if (!r.ok || seq != sync_seq_ || !p.creature) break;
		int32_t rtt = int32_t(w_.global_ms() - sent);
		if (rtt < 0) break;
		// The client's scene clock goes towards the server's: a gap over 1000 ms is jumped, a
		// smaller one closed by a half, a quarter or an eighth (Session 0x45006490).
		const GameObject *clock = clock_object(w_, p.creature);
		double scale = w_.scene_clock(clock).scale;
		int64_t target = int64_t(server_scene) + int64_t(double(rtt) * 0.5 * scale);
		int64_t now = int64_t(w_.scene_ms_of(p.creature)) + slew_pending;
		double d = double(target - now);
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
		break;
	}
	case kPing: {
		uint32_t sent = r.u32();
		Writer wr;
		wr.u8(kPong);
		wr.u32(sent);
		p.link->send(net::Channel::Unreliable, std::move(wr.d));
		break;
	}
	case kPong: {
		int32_t rtt = int32_t(w_.global_ms() - r.u32());
		if (r.ok && rtt >= 0) p.ping = p.ping == 0 ? float(rtt) : p.ping + (float(rtt) - p.ping) * 0.25f;
		break;
	}
	default: break;
	}
}

// ReplicateSessionStateManagerStates (Session 0x4500a020), per peer: a client sends the states of
// the creatures it controls whose managers replicate up, the server every manager that replicates
// down, but to a client only the corrections of its own creature. Each state is sent at once, then
// re-sent on the ladder, slower with distance from the peer's creature (1 + d / 180, at most 4)
// and faster with significance; authority under 2 is never sent. The budget limits each frame.
void NetSession::send_states(Peer &p, uint32_t now) {
	struct Item {
		const GameObject *owner;
		uint32_t id;
		NetState st;
		int step;
	};
	std::vector<Item> due;
	const GameObject *viewer = p.creature;
	for (auto &m : net_managers(w_)) {
		if (!m.server_id) continue;
		bool replicate = role_ == NetRole::Server ? m.down : m.up && locally_controlled(w_, m.owner);
		if (!replicate) continue;
		NetState st;
		if (!net_newest_state(w_, m.owner, st)) continue;
		const bool own = role_ == NetRole::Server && m.owner == viewer;
		const bool correction = st.flags & 0x10;
		if (own && !correction) continue;
		Peer::Sent &s = p.sent[m.owner];
		if (!s.any || s.serial != st.serial) s = {st.serial, st.time, 0, now, true};
		if (st.authority < 2 && !correction) {
			if (s.step < 7) ++s.step;
			s.last_ms = now;
			continue;
		}
		float dist = 1;
		if (viewer && m.owner->is_actor && viewer != m.owner && in_scene(*m.owner)) {
			auto a = m.owner->world().t, b = viewer->world().t;
			float d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
			dist = std::min(1 + d / 180.0f, 4.0f);
		}
		float interval = kLadder[s.step] * 0.9f * dist / (cfg_.quality * 2) * kSignificance[st.significance & 3];
		if (s.step == 0 || float(now - s.last_ms) >= interval) due.push_back({m.owner, m.server_id, std::move(st), s.step});
	}
	if (due.empty()) return;
	std::stable_sort(due.begin(), due.end(), [](const Item &a, const Item &b) { return a.step < b.step; });
	Writer wr;
	wr.u8(kStates);
	size_t n = 0;
	for (auto &it : due) {
		size_t size = it.st.bytes.size() + 12;
		if (n && float(wr.d.size() + size) > p.tokens) break;
		wr.squeezed(it.id);
		wr.squeezed(it.st.time);
		wr.u8(pack_flags(it.st));
		wr.u32(uint32_t(it.st.bytes.size()));
		wr.bytes(it.st.bytes.data(), it.st.bytes.size());
		Peer::Sent &s = p.sent[it.owner];
		if (s.step < 7) ++s.step;
		s.last_ms = now;
		++n;
	}
	wr.squeezed(0);
	p.tokens -= float(wr.d.size());
	p.bytes_sent += wr.d.size();
	states_sent += n;
	p.link->send(net::Channel::Unreliable, std::move(wr.d));
}

void NetSession::update() {
	const uint32_t now = w_.global_ms();
	for (auto &p : peers_) {
		net::Packet packet;
		while (p.link->receive(packet)) receive(p, packet);
	}
	// AdjustLag (Session 0x4500b6d0) once a second: 10 ms plus half the ping at the scene's scale.
	if (now - last_lag_ >= 1000) {
		last_lag_ = now;
		float ping = 0;
		for (auto &p : peers_) {
			Writer wr;
			wr.u8(kPing);
			wr.u32(now);
			p.link->send(net::Channel::Unreliable, std::move(wr.d));
			ping = std::max(ping, p.ping);
		}
		double scale = w_.scene_clock(clock_object(w_, peers_.empty() ? nullptr : peers_[0].creature)).scale;
		lag_ms = 10 + float(scale) * ping * 0.5f;
		net_set_lag(w_, lag_ms);
		if (role_ == NetRole::Client && !peers_.empty() && peers_[0].creature) {
			Writer wr;
			wr.u8(kSync);
			wr.u32(now);
			wr.u8(++sync_seq_);
			peers_[0].link->send(net::Channel::Unreliable, std::move(wr.d));
		}
	}
	for (auto &p : peers_) {
		p.tokens = std::min(p.tokens + float(cfg_.budget) * float(w_.frame_ms()) / 1000.0f, float(cfg_.budget) * 0.1f);
		if (role_ == NetRole::Server) announce(p);
		send_states(p, now);
	}
}

}  // namespace game
