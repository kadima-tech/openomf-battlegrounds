#include "game/netplay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
	kRoster = 0x01,     // {count, {client ID, name, slot + 1, flags}}, the receiver's ID: ClientAndSlotUpdate
	kRemoteUI = 0x03,   // {page ID, choose fighter, title, autoselect, pool}: LaunchRemoteUIManager
	kRemoteDone = 0x04, // {page ID, chosen, model}: RemoteUIManagerComplete
	kStatus = 0x0f,     // u32 status: ForceStatusChange
	kLoad = 0x10,       // load authorized: AuthorizeLoading
	kResend = 0x13,     // server ID: re-send its newest state
	kUnknownId = 0x14,  // server ID: the client deletes the owner
	kIdRequest = 0x15,  // ID string: RequestIDFromServer
	kDelete = 0x16,     // ID string: the client deletes that object
	kReport = 0x17,     // squeezed status: the client's report
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
	void str(const std::string &s) {
		squeezed(uint32_t(s.size()));
		bytes(s.data(), s.size());
	}
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
	std::string str() {
		auto b = take(squeezed());
		return std::string(b.begin(), b.end());
	}
};

std::map<const World *, NetSession *> g_sessions;

// GH_SLOT_QUERRY_USER_CHOICES_COMPLETE (Session 0x4500e770): sent to the client and its slot; the
// pickers clear INT1 while the player is still choosing.
bool choices_complete(World &w, SessionClient &c) {
	if (!c.slot) return false;
	uint32_t e = new_event(w, "SlotQuerryUserChoicesComplete");
	w.memory().set<int32_t>(e + 4, 1);
	w.send_event(c.obj, e);
	w.send_event(c.slot->obj, e);
	return w.memory().get<int32_t>(e + 4) != 0;
}

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
	session = master_session(w.session.get());
	g_sessions[&w] = this;
}

NetSession::~NetSession() {
	net_managers_end(w_);
	if (auto it = g_sessions.find(&w_); it != g_sessions.end() && it->second == this) g_sessions.erase(it);
}

NetSession *net_session(World &w) {
	auto it = g_sessions.find(&w);
	return it == g_sessions.end() ? nullptr : it->second;
}

void NetSession::remove_peer(net::Endpoint *link) {
	for (auto it = peers_.begin(); it != peers_.end(); ++it) {
		if (it->link == link) {
			peers_.erase(it);
			return;
		}
	}
}

NetSession::Peer *NetSession::peer_of(net::Endpoint *link) {
	for (auto &p : peers_) {
		if (p.link == link) return &p;
	}
	return nullptr;
}

GameObject *NetSession::clock_object(const Peer &p) const { return p.creature ? p.creature : session ? session->obj : nullptr; }

void NetSession::force_clients_to_load() {
	for (auto &p : peers_) {
		if (!p.client) continue;
		Writer wr = message(kStatus);
		wr.u32(101);
		send(p, net::Channel::Reliable, std::move(wr.d));
		p.status = 2000;
		p.preloaded_status = 0;
	}
}

void NetSession::launch_remote_ui(SessionClient &c, GameObject *ui) {
	if (!session || !ui) return;
	auto it = session->ui.find(ui);
	if (it == session->ui.end()) return;
	auto &u = it->second;
	for (auto &p : peers_) {
		if (p.client != &c) continue;
		Writer wr = message(kRemoteUI);
		wr.squeezed(ui->handle);
		wr.u8(u.choose_fighter);
		wr.str(u.description);
		wr.str(u.autoselect);
		wr.squeezed(uint32_t(u.pool.size()));
		for (auto &g : u.pool) wr.bytes(g.bytes, 16);
		send(p, net::Channel::Reliable, std::move(wr.d));
	}
}

void NetSession::remote_ui_complete(GameObject *ui) {
	if (!session || peers_.empty()) return;
	auto it = session->ui.find(ui);
	if (it == session->ui.end() || !it->second.remote_id) return;
	Writer wr = message(kRemoteDone);
	wr.squeezed(it->second.remote_id);
	wr.bytes(it->second.chosen.data(), 16);
	wr.bytes(it->second.model.data(), 16);
	send(peers_[0], net::Channel::Reliable, std::move(wr.d));
	remote_ui_.erase(it->second.remote_id);
}

// ClientAndSlotUpdate: every client's ID, name and slot (the original's slot info is the slot's
// own state, which the slot modifiers' states carry here).
std::string NetSession::roster_signature() {
	std::string sig;
	if (!session) return sig;
	for (auto &c : session->clients) {
		if (!c->net_id) continue;
		sig += std::to_string(c->net_id) + ':' + c->player_name + ':' + std::to_string(c->slot ? c->slot->index : -1) + ';';
	}
	return sig;
}

void NetSession::send_roster(Peer &p, const std::string &signature) {
	Writer wr = message(kRoster);
	uint32_t n = 0;
	for (auto &c : session->clients) n += c->net_id != 0;
	wr.squeezed(n);
	for (auto &c : session->clients) {
		if (!c->net_id) continue;
		wr.squeezed(c->net_id);
		wr.str(c->player_name);
		wr.squeezed(c->slot ? uint32_t(c->slot->index + 1) : 0);
		wr.u8(uint8_t((c->local && c->user) ? 1 : 0));
	}
	wr.squeezed(p.client ? p.client->net_id : 0);
	send(p, net::Channel::Reliable, std::move(wr.d));
	p.roster = signature;
	++rosters;
}

void NetSession::receive_roster(const std::vector<uint8_t> &d, size_t pos) {
	Reader r{d};
	r.pos = pos;
	struct Entry {
		uint32_t id;
		std::string name;
		int slot;
	};
	std::vector<Entry> entries;
	uint32_t n = r.squeezed();
	for (uint32_t i = 0; i < n && r.ok; ++i) {
		Entry e;
		e.id = r.squeezed();
		e.name = r.str();
		e.slot = int(r.squeezed()) - 1;
		r.u8();
		entries.push_back(e);
	}
	uint32_t me = r.squeezed();
	if (!r.ok || !session) return;
	own_id = me;
	auto keep = w_.session;
	for (auto &sp : w_.sessions) {
		if (sp.get() == session) w_.session = sp;
	}
	for (size_t i = session->clients.size(); i-- > 0;) {
		SessionClient &c = *session->clients[i];
		if (std::none_of(entries.begin(), entries.end(), [&](const Entry &e) { return e.id == c.net_id; })) drop_client(w_, *session, c);
	}
	for (auto &e : entries) {
		SessionClient *c = nullptr;
		for (auto &cl : session->clients) {
			if (cl->net_id == e.id) c = cl.get();
		}
		const bool mine = e.id == me;
		if (!c) {
			c = &add_client_to(w_, *session, mine, e.name);
			c->net_id = e.id;
			c->local = mine;
		}
		c->player_name = e.name;
		if (e.slot >= 0 && size_t(e.slot) < session->slots.size()) {
			SessionSlot &sl = *session->slots[size_t(e.slot)];
			// SetupCreatureControl: a client's own slot is the one it controls.
			sl.local = mine;
			if (sl.client != c) connect_client(w_, *session, *c, sl);
		}
	}
	w_.session = keep;
	if (session->net_status == 100) report(101);
}

void NetSession::receive_remote_ui(const std::vector<uint8_t> &d, size_t pos) {
	Reader r{d};
	r.pos = pos;
	uint32_t id = r.squeezed();
	bool fighter = r.u8() != 0;
	std::string title = r.str(), autoselect = r.str();
	uint32_t n = r.squeezed();
	std::vector<de::Guid> pool;
	for (uint32_t i = 0; i < n && r.ok; ++i) {
		de::Guid g;
		auto b = r.take(16);
		if (b.size() == 16) memcpy(g.bytes, b.data(), 16);
		pool.push_back(g);
	}
	if (!r.ok || !session) return;
	GameObject *ui = remote_ui_[id];
	if (!ui || !w_.object(ui->handle)) {
		ui = w_.create_child(nullptr, "RemoteUIManager", fighter ? "UIManager\\GenericDescriptionSelector\\ChooseFighter" : "UIManager\\GenericDescriptionSelector");
		remote_ui_[id] = ui;
	}
	auto &u = session->ui[ui];
	u.description = title;
	u.autoselect = autoselect;
	u.pool = pool;
	u.choose_fighter = fighter;
	u.remote_id = id;
	u.shown = true;
	auto keep = w_.session;
	for (auto &sp : w_.sessions) {
		if (sp.get() == session) w_.session = sp;
	}
	if (w_.launch_ui) w_.launch_ui(ui);
	else w_.log.push_back("session: no UI to show the server's \"" + title + "\"");
	w_.session = keep;
}

// UpdateStateManagersRequiredForPreLoad (Session 0x4500b9b0): every pre-load manager's newest state.
void NetSession::preload(Peer &p) {
	Writer pre = message(kPreload);
	for (auto &m : net_managers(w_)) {
		NetState st;
		if (!m.server_id || !m.preload || !net_newest_state(w_, m.owner, st)) continue;
		p.preloaded[m.owner] = st.serial;
		pre.u8(1);
		pre.squeezed(m.server_id);
		pre.str(m.id);
		pre.squeezed(st.time);
		pre.u8(uint8_t((st.authority & 3) | ((st.significance & 3) << 2)));
		pre.u32(uint32_t(st.bytes.size()));
		pre.bytes(st.bytes.data(), st.bytes.size());
	}
	pre.u8(0);
	send(p, net::Channel::Reliable, std::move(pre.d));
}

// DE_CStandardSession::Update (Session 0x4500e770) on a server: its own session proceeds to 102 by
// itself and to 103 once its local player's choices are made (at once with none); each remote
// client by its reported status gets the roster (100), the pre-load (101, 103) or, once its slot's
// choices are made, the load authorization (102, at most once a second).
void NetSession::server_flow(uint32_t now) {
	SessionState *m = session;
	if (!m) return;
	for (auto &c : m->clients) {
		if (!c->net_id) c->net_id = next_client_id_++;
	}
	int &st = m->net_status;
	if (st == 1 || st == 2) st = 100;
	else if (st == 100) st = 101;
	else if (st == 101) st = 102;
	else if (st == 102) {
		SessionClient *local = nullptr;
		for (auto &c : m->clients) {
			if (c->local && c->user) local = c.get();
		}
		if (!local || choices_complete(w_, *local)) st = 103;
	}
	const std::string sig = roster_signature();
	for (auto &p : peers_) {
		if (!p.client) continue;
		p.creature = p.client->slot ? p.client->slot->creature : nullptr;
		if (p.status == 100 || (p.status > 100 && p.status != 2000)) {
			if (p.roster != sig) send_roster(p, sig);
		}
		if ((p.status == 101 || p.status == 103) && p.preloaded_status != p.status) {
			preload(p);
			p.preloaded_status = p.status;
		}
		if (p.status != 101 && p.status != 103) p.preloaded_status = 0;
		if (p.status == 102 && now - p.authorized_at >= 1000 && choices_complete(w_, *p.client)) {
			send(p, net::Channel::Reliable, std::move(message(kLoad).d));
			p.authorized_at = now;
		}
	}
}

// A client reports every status it takes, at once: a forced one too, which the server awaits.
void NetSession::report(int status) {
	if (!session || peers_.empty()) return;
	session->net_status = status;
	Writer wr = message(kReport);
	wr.squeezed(uint32_t(status));
	send(peers_[0], net::Channel::Reliable, std::move(wr.d));
	reported_ = status;
	printf("net: status %s (%d)\n", net_status_description(status), status);
}

// A client reports each status change (0x17) and asks for the IDs of its unlinked managers (0x15),
// and for the newest state of a linked one it has heard nothing of in 5 s (0x13).
void NetSession::client_flow(uint32_t now) {
	SessionState *m = session;
	if (!m || peers_.empty()) return;
	Peer &p = peers_[0];
	if (m->net_status != reported_) report(m->net_status);
	p.creature = nullptr;
	for (auto &c : m->clients) {
		if (c->net_id == own_id && c->slot) p.creature = c->slot->creature;
	}
	if (m->net_status != 1000) return;
	// A manager is asked for as the sender meets it, then at most once a second.
	for (auto &mg : net_managers(w_)) {
		if (mg.server_id || mg.id.empty()) continue;
		auto [it, fresh] = asked_.try_emplace(mg.id, now);
		if (!fresh && now - it->second < 1000) continue;
		it->second = now;
		if (getenv("OPENBG_DEBUG_NETSTATES")) printf("[net %u] asks for %s's ID\n", w_.frame(), mg.owner->name.c_str());
		Writer wr = message(kIdRequest);
		wr.str(mg.id);
		send(p, net::Channel::Unreliable, std::move(wr.d));
		++id_requests;
	}
	if (now - last_requests_ < 1000) return;
	last_requests_ = now;
	for (auto &[id, at] : linked_at_) {
		if (now - at < 5000) continue;
		at = now;
		Writer wr = message(kResend);
		wr.squeezed(id);
		send(p, net::Channel::Unreliable, std::move(wr.d));
	}
}

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
	if (cfg_.flow) {
		if (p.id_queue.empty()) return;
		Writer links = message(kLink);
		links.squeezed(uint32_t(p.id_queue.size()));
		auto managers = net_managers(w_);
		for (uint32_t id : p.id_queue) {
			auto it = std::find_if(managers.begin(), managers.end(), [id](const NetManager &m) { return m.server_id == id; });
			links.squeezed(id);
			links.str(it == managers.end() ? std::string() : it->id);
			p.linked[id] = true;
			// What went before the client knew the ID was dropped there: its newest state goes again.
			if (it != managers.end()) {
				auto &rec = p.records[it->owner];
				rec.acked = -1, rec.index = 0, rec.ever_acked = false;
			}
		}
		p.id_queue.clear();
		send(p, net::Channel::Reliable, std::move(links.d));
		return;
	}
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
	p.last_heard = now;
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
			GameObject *clock = clock_object(p);
			wr.u32(clock ? w_.precise_ms_of(clock) : 0);
			wr.u8(seq);
			send(p, net::Channel::Unreliable, std::move(wr.d));
			return;
		}
		uint32_t sent = r.u32(), server_scene = r.u32();
		uint8_t seq = r.u8();
		GameObject *clock = clock_object(p);
		if (!r.ok || seq != sync_seq_ || !clock) return;
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
		auto &sc = w_.scene_clock(w_.clock_key(clock));
		double scale = sc.scale;
		int64_t target = int64_t(server_scene) + int64_t(double(rtt) * 0.5 * scale);
		// Before its Genesis the client's scene takes the server's time to start at.
		clock_synced_ = true;
		if (cfg_.flow && !sc.started) {
			sc.initial = uint32_t(std::max<int64_t>(0, target));
			return;
		}
		int64_t here = int64_t(w_.scene_ms_of(clock)) + slew_pending;
		double d = double(target - here);
		if (std::fabs(d) <= 1000) {
			if (std::fabs(d) < 120) {
				d *= 0.5;
				if (std::fabs(d) < 40) d *= 0.5;
				if (std::fabs(d) < 10) d *= 0.5;
			}
			slew_pending += int32_t(d);
		} else {
			++clock_jumps;
			if (cfg_.flow) printf("net: scene clock set from %lld to %lld ms\n", (long long)here, (long long)target);
			// SetInitialSceneTime: a joining client's scene starts at the server's time.
			if (cfg_.flow && target > 0) {
				w_.set_initial_scene_time(w_.clock_key(clock), uint32_t(target));
				slew_pending = 0;
			} else {
				slew_pending += int32_t(d);
			}
		}
		return;
	}
	if (header != kSession) return;
	uint16_t msg = uint16_t(r.u8() | (r.u8() << 8));
	const bool server = role_ == NetRole::Server;
	// A server ignores the state traffic of a client that is not Sequenced.
	if (cfg_.flow && server && p.status != 1000 && (msg == kStates || msg == kAcks || msg == kCorrection || msg == kResend)) return;
	switch (msg) {
	case kRoster:
		if (!server) receive_roster(packet.data, r.pos);
		break;
	case kRemoteUI:
		if (!server) receive_remote_ui(packet.data, r.pos);
		break;
	case kRemoteDone: {
		uint32_t id = r.squeezed();
		auto chosen = r.take(16), model = r.take(16);
		GameObject *ui = w_.object(id);
		if (!r.ok || !server || !ui || !session || !session->ui.count(ui)) break;
		de::Guid c, mg;
		memcpy(c.bytes, chosen.data(), 16);
		memcpy(mg.bytes, model.data(), 16);
		bool has_model = std::any_of(model.begin(), model.end(), [](uint8_t b) { return b != 0; });
		complete_ui(w_, ui, c, has_model ? &mg : nullptr);
		break;
	}
	case kStatus: {
		uint32_t st = r.u32();
		if (!r.ok || !session) break;
		if (server) w_.log.push_back("Client attempted to force server status");
		else report(int(st));
		break;
	}
	case kLoad:
		if (!server && session && session->net_status == 102) report(103);
		break;
	case kReport: {
		uint32_t st = r.squeezed();
		if (!r.ok || !server) break;
		if (p.status != int(st)) printf("net: %s reports %s (%u)\n", p.client ? p.client->player_name.c_str() : "?", net_status_description(int(st)), st);
		p.status = int(st);
		p.preloaded_status = 0;
		if (st == 100) p.roster.clear();
		break;
	}
	case kIdRequest: {
		std::string id = r.str();
		if (!r.ok || !server) break;
		uint32_t found = 0;
		for (auto &m : net_managers(w_)) {
			if (m.id == id && m.server_id) found = m.server_id;
		}
		if (found) {
			if (std::find(p.id_queue.begin(), p.id_queue.end(), found) == p.id_queue.end()) p.id_queue.push_back(found);
		} else {
			Writer wr = message(kDelete);
			wr.str(id);
			send(p, net::Channel::Unreliable, std::move(wr.d));
		}
		break;
	}
	case kDelete: {
		std::string id = r.str();
		if (!r.ok || server) break;
		for (auto &m : net_managers(w_)) {
			if (m.id == id && !m.server_id) {
				w_.clean_later(m.owner);
				++deletes;
				w_.log.push_back("net: the server knows no " + m.owner->name + "; deleted");
			}
		}
		break;
	}
	case kResend: {
		uint32_t id = r.squeezed();
		if (!r.ok || !server) break;
		if (GameObject *owner = net_owner(w_, id)) {
			auto &rec = p.records[owner];
			rec.force = true, rec.acked = -1, rec.index = 0;
		} else {
			Writer wr = message(kUnknownId);
			wr.squeezed(id);
			send(p, net::Channel::Unreliable, std::move(wr.d));
		}
		break;
	}
	case kUnknownId: {
		uint32_t id = r.squeezed();
		if (!r.ok || server) break;
		if (GameObject *owner = net_owner(w_, id)) {
			w_.clean_later(owner);
			++deletes;
		}
		linked_at_.erase(id);
		break;
	}
	case kLink: {
		uint32_t n = r.squeezed();
		for (uint32_t i = 0; i < n && r.ok; ++i) {
			uint32_t id = r.squeezed();
			auto b = r.take(r.squeezed());
			if (r.ok && role_ == NetRole::Client && net_link(w_, id, std::string(b.begin(), b.end())) && cfg_.flow) linked_at_[id] = now;
			if (getenv("OPENBG_DEBUG_NETSTATES")) printf("[net %u] link %u %s\n", w_.frame(), id, net_owner(w_, id) ? net_owner(w_, id)->name.c_str() : "(none)");
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
			else w_.log.push_back("net: pre-load manager not set up: " + std::string(b.begin(), b.end()));
		}
		if (role_ == NetRole::Client && session && session->net_status == 101) report(102);
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
			linked_at_.erase(id);
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
		if (getenv("OPENBG_DEBUG_NETSTATES") && m.owner->fighter && w_.frame() % 25 == 0)
			printf("[net %u] %s newest %u a%u s%u f%x index %d acked %lld\n", w_.frame(), m.owner->name.c_str(), st.time, st.authority, st.significance, st.flags, rec.index, (long long)rec.acked);
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

void NetSession::send_clock(uint32_t now) {
	Writer wr;
	wr.u8(kClock);
	wr.u32(now);
	wr.u8(++sync_seq_);
	wr.squeezed(session ? uint32_t(session->net_status) : 1000);
	send(peers_[0], net::Channel::Unreliable, std::move(wr.d));
	last_clock_ = now;
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
		if (role_ == NetRole::Client && !peers_.empty() && clock_object(peers_[0]) && (peers_[0].creature || cfg_.flow)) send_clock(now);
	} else if (cfg_.flow && role_ == NetRole::Client && !clock_synced_ && now - last_clock_ >= 200 && !peers_.empty()) {
		// Until a first sample is taken a joining client asks every 200 ms, so that its scene
		// starts at the server's time before the fighters load.
		send_clock(now);
	}
	if (cfg_.flow) {
		if (role_ == NetRole::Server) server_flow(now);
		else client_flow(now);
	}
	for (auto &p : peers_) {
		if (role_ == NetRole::Server) announce(p);
		// States flow only while the client is Sequenced.
		if (cfg_.flow && (role_ == NetRole::Server ? p.status != 1000 : !session || session->net_status != 1000)) continue;
		send_states(p, now);
		send_acks(p, now, false);
	}
}

}  // namespace game
