#include "game/netgame.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "de/md5.h"
#include "game/runtime.h"
#include "game/session.h"

#ifndef OPENBG_ENGINE_VERSION
#define OPENBG_ENGINE_VERSION "dev"
#endif

namespace game {

namespace {

// The pre-session messages ride on header 3 (the session's are 0..2); the numbers are the app
// manager's com interface's (Core 0x70198130): 2 join, 4 accepted, 5 refused.
constexpr uint8_t kLobby = 3;
enum : uint16_t { kJoin = 2, kAccept = 4, kRefuse = 5 };
constexpr char kMagic[] = "OMFBG-NET";

struct Out {
	std::vector<uint8_t> d;
	void u8(uint8_t v) { d.push_back(v); }
	void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
	void u32(uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); }
	void bytes(const void *p, size_t n) { d.insert(d.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
	void str(const std::string &s) {
		u16(uint16_t(std::min<size_t>(s.size(), 0xffff)));
		bytes(s.data(), std::min<size_t>(s.size(), 0xffff));
	}
};

struct In {
	const std::vector<uint8_t> &d;
	size_t pos = 0;
	bool ok = true;
	bool need(size_t n) {
		if (pos + n > d.size()) ok = false;
		return ok;
	}
	uint8_t u8() { return need(1) ? d[pos++] : 0; }
	uint16_t u16() {
		uint16_t lo = u8();
		return uint16_t(lo | (u8() << 8));
	}
	uint32_t u32() {
		uint32_t lo = u16();
		return lo | (uint32_t(u16()) << 16);
	}
	void bytes(void *out, size_t n) {
		if (need(n)) memcpy(out, d.data() + pos, n), pos += n;
	}
	std::string str() {
		size_t n = u16();
		if (!need(n)) return {};
		std::string s(d.begin() + long(pos), d.begin() + long(pos + n));
		pos += n;
		return s;
	}
};

Out lobby_message(uint16_t m) {
	Out o;
	o.u8(kLobby);
	o.u16(m);
	return o;
}

std::string hash_text(const std::array<uint8_t, 16> &h) {
	static const char *hex = "0123456789abcdef";
	std::string s;
	for (int i = 0; i < 4; ++i) s += hex[h[size_t(i)] >> 4], s += hex[h[size_t(i)] & 15];
	return s;
}

int reg_int(const std::map<std::string, std::string> &r, const char *k, int def) {
	auto it = r.find(k);
	return it == r.end() || it->second.empty() ? def : atoi(it->second.c_str());
}
std::string reg_str(const std::map<std::string, std::string> &r, const char *k) {
	auto it = r.find(k);
	return it == r.end() ? std::string() : it->second;
}

}  // namespace

const char *net_engine_version() { return OPENBG_ENGINE_VERSION; }

std::array<uint8_t, 16> net_data_hash(const de::World &data) {
	static const char *media[] = {"Image", "Sound", "Mesh", "Animation", "Music", "Wave", "Font", "Texture"};
	std::vector<uint8_t> all;
	for (auto &[stem, f] : data.files()) {
		all.insert(all.end(), stem.begin(), stem.end());
		const auto &raw = f->raw();
		std::vector<uint32_t> offsets;
		for (auto &o : f->objects()) offsets.push_back(o.data_offset);
		offsets.push_back(uint32_t(raw.size()));
		std::sort(offsets.begin(), offsets.end());
		for (auto &o : f->objects()) {
			all.insert(all.end(), o.class_path.begin(), o.class_path.end());
			all.insert(all.end(), o.name.begin(), o.name.end());
			all.insert(all.end(), o.guid.bytes, o.guid.bytes + 16);
			if (std::any_of(std::begin(media), std::end(media), [&](const char *m) { return o.class_path.rfind(m, 0) == 0; })) continue;
			auto next = std::upper_bound(offsets.begin(), offsets.end(), o.data_offset);
			if (next == offsets.end() || o.data_offset >= raw.size()) continue;
			all.insert(all.end(), raw.begin() + o.data_offset, raw.begin() + std::min<size_t>(*next, raw.size()));
		}
	}
	return de::md5(all);
}

NetOptions net_options(const std::map<std::string, std::string> &r) {
	NetOptions o;
	o.player_name = reg_str(r, "Network\\Player Name");
	o.server_name = reg_str(r, "Network\\Server Name");
	o.password = reg_str(r, "Network\\Server Password");
	o.admin_password = reg_str(r, "Network\\Remote Admin Password");
	int port = reg_int(r, "Network\\Winsock\\Listener Port", 0);
	o.port = port > 0 && port < 65536 ? uint16_t(port) : net::kDefaultPort;
	o.kick_ms = uint32_t(std::max(1000, reg_int(r, "Network\\Kick Time", 60000)));
	o.budget = std::max(1000, reg_int(r, "Network\\Winsock\\Maximum Outgoing Bytes", 16000));
	o.delay.delay_ms = float(std::max(0, reg_int(r, "Network\\Winsock\\Delay Packet Time", 0)));
	o.delay.variation_ms = float(std::max(0, reg_int(r, "Network\\Winsock\\Delay Packet Time Added Variation", 0)));
	return o;
}

std::vector<uint8_t> encode_server_info(const NetServerInfo &i) {
	Out o;
	o.bytes(kMagic, sizeof kMagic - 1);
	o.u16(i.protocol);
	o.str(i.version);
	o.bytes(i.data_hash.data(), 16);
	o.str(i.server_name);
	o.str(i.session_name);
	o.u8(uint8_t(std::clamp(i.players, 0, 255)));
	o.u8(uint8_t(std::clamp(i.max_players, 0, 255)));
	o.u8(uint8_t((i.password ? 1 : 0) | (i.playing ? 2 : 0)));
	o.u16(i.port);
	o.u32(i.instance);
	return o.d;
}

bool decode_server_info(const std::vector<uint8_t> &d, NetServerInfo &out) {
	In in{d};
	char magic[sizeof kMagic - 1];
	in.bytes(magic, sizeof magic);
	if (!in.ok || memcmp(magic, kMagic, sizeof magic) != 0) return false;
	out.protocol = in.u16();
	out.version = in.str();
	in.bytes(out.data_hash.data(), 16);
	out.server_name = in.str();
	out.session_name = in.str();
	out.players = in.u8();
	out.max_players = in.u8();
	uint8_t flags = in.u8();
	out.password = flags & 1;
	out.playing = flags & 2;
	out.port = in.u16();
	out.instance = in.u32();
	return in.ok;
}

namespace {
uint64_t steady_ms() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
}  // namespace

NetGame::NetGame(World &w) : w_(w) {}

NetGame::~NetGame() { close(); }

void NetGame::close() {
	if (host_ && state_ == State::Hosting) printf("net: stopped hosting, port %u released\n", host_->port());
	net_.reset();
	if (host_) {
		for (net::EnetPeer *p : host_->peers()) host_->disconnect(p, true);
	}
	host_.reset();
	accepted_.clear();
	server_ = nullptr;
	hosted_ = nullptr;
	if (state_ != State::Idle) state_ = State::Closed;
}

void NetGame::fail(const std::string &title, const std::string &text) {
	const bool joined = state_ == State::Joined;
	close();
	if (notify) notify(title, text);
	if (joined && on_closed) on_closed();
}

bool NetGame::host(SessionState &s, const NetOptions &o, std::string *error) {
	opt_ = o;
	host_ = net::EnetHost::listen(o.port, 16, error);
	if (!host_) {
		// WinSockService::ObjInitContents's notice (Core 0x7019e0b0), giving up on the port at once.
		if (error) *error = "Couldn't activate listening port " + std::to_string(o.port) + " because it is already active.  Giving up on activating listening port.";
		printf("net: cannot host on port %u\n", o.port);
		return false;
	}
	host_->set_delay(o.delay);
	host_->set_kick_time(o.kick_ms);
	hash_ = net_data_hash(w_.data());
	hosted_ = &s;
	s.net_status = 1;
	instance_ = net::now_ms() ^ uint32_t(uintptr_t(this)) ^ (uint32_t(o.port) << 16);
	NetConfig cfg;
	cfg.flow = true;
	cfg.budget = o.budget;
	auto keep = w_.session;
	for (auto &sp : w_.sessions) {
		if (sp.get() == &s) w_.session = sp;
	}
	net_ = std::make_unique<NetSession>(w_, NetRole::Server, cfg);
	w_.session = keep;
	net_->notify = notify;
	net_->admin_password = o.admin_password;
	net_->on_ban = [this](net::Endpoint *link, uint32_t hours) {
		for (auto &[peer, c] : accepted_) {
			if (peer != link) continue;
			bans_[peer->address().ip] = steady_ms() + uint64_t(hours) * 3600000;
			printf("net: %s banned for %u hours\n", c->player_name.c_str(), hours);
			host_->disconnect(peer, false);
		}
	};
	host_->discovery_reply = [this] {
		NetServerInfo i;
		i.protocol = kNetProtocol;
		i.version = net_engine_version();
		i.data_hash = hash_;
		i.server_name = opt_.server_name.empty() ? "Battlegrounds Server" : opt_.server_name;
		// A match server shows the match it runs (SetDisplayedSessionMaster).
		if (de::ObjectRef shown = hosted_ && hosted_->displayed_master ? w_.data().by_guid(*hosted_->displayed_master) : de::ObjectRef{}) i.session_name = shown.info().name;
		else if (hosted_ && hosted_->master_description && hosted_->master_description->file)
			i.session_name = hosted_->master_description->file->objects()[hosted_->master_description->index].name;
		i.players = hosted_ ? int(hosted_->clients.size()) : 0;
		i.max_players = hosted_ ? hosted_->max_player_clients : 0;
		i.password = !opt_.password.empty();
		i.playing = hosted_ && hosted_->begun;
		i.port = host_ ? host_->port() : 0;
		i.instance = instance_;
		return encode_server_info(i);
	};
	state_ = State::Hosting;
	printf("net: hosting on port %u, data %s\n", host_->port(), hash_text(hash_).c_str());
	return true;
}

bool NetGame::join(const std::string &address, const NetOptions &o, std::string *error) {
	opt_ = o;
	net::Address to;
	if (!net::resolve(address, o.port, to, error)) return false;
	host_ = net::EnetHost::connect(to, error);
	if (!host_) return false;
	host_->set_delay(o.delay);
	host_->set_kick_time(o.kick_ms);
	hash_ = net_data_hash(w_.data());
	state_ = State::Connecting;
	printf("net: connecting to %s, data %s\n", to.text().c_str(), hash_text(hash_).c_str());
	return true;
}

uint32_t NetGame::frame_ms(uint32_t real_ms) { return net_ && net_->role() == NetRole::Client ? net_->frame_ms(real_ms) : real_ms; }

// The join request: our hello (protocol, engine version, data hash), the player's name and the
// password. The server refuses with a reason, or accepts with the client's ID and the session info.
void NetGame::lobby(net::EnetPeer &p, const net::Packet &packet) {
	In in{packet.data};
	if (in.u8() != kLobby) return;
	uint16_t m = in.u16();
	if (state_ == State::Hosting && m == kJoin) {
		std::string magic = in.str();
		uint16_t protocol = in.u16();
		std::string version = in.str();
		std::array<uint8_t, 16> hash{};
		in.bytes(hash.data(), 16);
		std::string name = in.str(), password = in.str();
		std::string reason;
		if (!in.ok || magic != kMagic) reason = "The server did not understand the request.";
		else if (protocol != kNetProtocol) reason = "The server speaks network protocol " + std::to_string(kNetProtocol) + ", this game " + std::to_string(protocol) + ".";
		else if (version != net_engine_version()) reason = "The server runs version " + std::string(net_engine_version()) + ", this game " + version + ".";
		else if (hash != hash_) reason = "The server's game data differs from yours (" + hash_text(hash_) + " against " + hash_text(hash) + ").";
		else if (!hosted_ || !hosted_->master_description) reason = "Server Unknown";
		// A known client joining again (FINDINGS 452): same machine and name, old connection still kept.
		net::EnetPeer *previous = nullptr;
		if (reason.empty() && hosted_) {
			for (auto &[old, cl] : accepted_) {
				if (old != &p && old->address().ip == p.address().ip && cl->player_name == (name.empty() ? "Player" : name)) previous = old;
			}
		}
		if (auto b = bans_.find(p.address().ip); reason.empty() && b != bans_.end() && steady_ms() < b->second) reason = "Banned From Server";
		if (reason.empty() && !previous && int(hosted_->clients.size()) >= hosted_->max_player_clients) reason = "The server is full.";
		else if (reason.empty() && !opt_.password.empty() && password != opt_.password) reason = "The password is not correct.";
		if (!reason.empty()) {
			Out o = lobby_message(kRefuse);
			o.str(reason);
			p.send(net::Channel::Reliable, std::move(o.d));
			printf("net: refused %s (%s): %s\n", p.address().text().c_str(), name.c_str(), reason.c_str());
			return;
		}
		if (name.empty()) name = "Player";
		if (previous) {
			// DE_CStandardClient::Reconnect (Session 0x45002670, FINDINGS 452).
			SessionClient &c = *accepted_[previous];
			std::vector<uint32_t> linked;
			if (auto *op = net_->peer_of(previous)) {
				for (auto &[id, on] : op->linked) linked.push_back(id);
			}
			net_->remove_peer(previous);
			accepted_.erase(previous);
			host_->disconnect(previous, true);
			accepted_[&p] = &c;
			net_->reconnect(net_->add_peer(&p), c, linked);
			printf("net: %s reconnected from %s\n", name.c_str(), p.address().text().c_str());
		} else {
			SessionClient &c = add_client_to(w_, *hosted_, false, name);
			c.local = false;
			c.active = true;
			accepted_[&p] = &c;
			auto &peer = net_->add_peer(&p);
			peer.client = &c;
			peer.status = 2;
		}
		Out o = lobby_message(kAccept);
		o.u32(0);
		o.str(opt_.server_name);
		const de::ObjectInfo &md = hosted_->master_description->file->objects()[hosted_->master_description->index];
		o.bytes(md.guid.bytes, 16);
		o.u16(uint16_t(hosted_->attached_guids.size()));
		for (auto &g : hosted_->attached_guids) o.bytes(g.bytes, 16);
		auto sv = w_.values.find(hosted_->description);
		uint32_t starting = 0;
		if (sv != w_.values.end()) {
			auto it = sv->second.find("StartingMatch");
			if (it != sv->second.end()) starting = it->second.bits;
		}
		o.u32(starting);
		o.u32(uint32_t(hosted_->max_player_clients));
		p.send(net::Channel::Reliable, std::move(o.d));
		if (!previous) printf("net: %s joined from %s\n", name.c_str(), p.address().text().c_str());
		return;
	}
	if (state_ == State::Connecting && m == kRefuse) {
		std::string reason = in.str();
		fail("Unable To Join", reason.empty() ? "The server refused the connection." : reason);
		return;
	}
	if (state_ == State::Connecting && m == kAccept) start_client_session(packet.data, in.pos);
}

void NetGame::start_client_session(const std::vector<uint8_t> &d, size_t pos) {
	In in{d};
	in.pos = pos;
	in.u32();
	std::string server_name = in.str();
	de::Guid master;
	in.bytes(master.bytes, 16);
	std::vector<de::Guid> attached(in.u16());
	for (auto &g : attached) in.bytes(g.bytes, 16);
	uint32_t starting = in.u32();
	int max_clients = int(in.u32());
	de::ObjectRef desc = in.ok ? w_.data().by_guid(master) : de::ObjectRef{};
	auto res = desc ? passive_references(w_.data(), desc) : std::vector<de::ObjectRef>{};
	if (res.empty()) {
		fail("Unable To Join", "The server's match is not in this game's data.");
		return;
	}
	printf("net: joined %s, launching %s\n", server_name.c_str(), desc.info().name.c_str());
	auto &ss = start_session(w_, {res[0]});
	ss.master_description = w_.global_object(desc);
	ss.single_player = false;
	ss.max_player_clients = max_clients > 0 ? max_clients : 16;
	ss.attached_guids = attached;
	w_.values[ss.description]["StartingMatch"].bits = starting;
	// The join accept proceeds Awaiting Join Granted to Awaiting Client and Slot Update.
	ss.net_status = 100;
	NetConfig cfg;
	cfg.flow = true;
	cfg.budget = opt_.budget;
	net_ = std::make_unique<NetSession>(w_, NetRole::Client, cfg);
	net_->notify = notify;
	net_->add_peer(server_);
	begin_session(w_);
	state_ = State::Joined;
	if (on_joined) on_joined();
}

void NetGame::update() {
	if (!host_) return;
	// The network game lives as long as its session: a match that ends closes it.
	SessionState *s = hosted_ ? hosted_ : net_ ? net_->session : nullptr;
	if (s && std::none_of(w_.sessions.begin(), w_.sessions.end(), [&](const auto &sp) { return sp.get() == s; })) {
		printf("net: the session ended\n");
		hosted_ = nullptr;
		if (net_) net_->session = nullptr;
		close();
		return;
	}
	host_->service();
	net::EnetHost::Event e;
	while (host_->poll(e)) {
		switch (e.kind) {
		case net::EnetHost::Event::Connected:
			if (state_ == State::Connecting) {
				server_ = e.peer;
				Out o = lobby_message(kJoin);
				o.str(kMagic);
				o.u16(kNetProtocol);
				o.str(net_engine_version());
				o.bytes(hash_.data(), 16);
				o.str(opt_.player_name);
				o.str(opt_.password);
				server_->send(net::Channel::Reliable, std::move(o.d));
			}
			break;
		case net::EnetHost::Event::Disconnected:
			if (state_ == State::Hosting) {
				auto it = accepted_.find(e.peer);
				if (it != accepted_.end()) {
					printf("net: %s left\n", it->second->player_name.c_str());
					if (hosted_) {
						auto keep = w_.session;
						for (auto &sp : w_.sessions) {
							if (sp.get() == hosted_) w_.session = sp;
						}
						drop_client(w_, *hosted_, *it->second);
						w_.session = keep;
					}
					if (net_) net_->remove_peer(e.peer);
					accepted_.erase(it);
				}
			} else if (e.peer == server_) {
				fail(state_ == State::Joined ? "Connection Lost" : "Unable To Join",
				     state_ == State::Joined ? "The connection to the server was lost." : "The server did not answer.");
				return;
			}
			break;
		case net::EnetHost::Event::Interrupted:
		case net::EnetHost::Event::Restored: {
			// SessionConnectionInterrupted / Restored go to the session and the client's slot.
			const char *handle = e.kind == net::EnetHost::Event::Interrupted ? "SessionConnectionInterrupted" : "SessionConnectionRestored";
			SessionState *s = hosted_ ? hosted_ : net_ ? net_->session : nullptr;
			if (!s) break;
			uint32_t ev = new_event(w_, handle);
			w_.send_event(s->obj, ev);
			auto it = accepted_.find(e.peer);
			if (SessionClient *pc = it != accepted_.end() ? playing(w_, it->second) : nullptr; pc && pc->slot) w_.send_event(pc->slot->obj, ev);
			printf("net: %s %s\n", e.peer->address().text().c_str(), e.kind == net::EnetHost::Event::Interrupted ? "interrupted" : "restored");
			break;
		}
		}
	}
	if (!host_) return;
	for (net::EnetPeer *p : host_->peers()) {
		const bool pending = state_ == State::Hosting ? !accepted_.count(p) : state_ == State::Connecting;
		net::Packet packet;
		while (pending && host_ && p->receive(packet)) {
			lobby(*p, packet);
			if (state_ != State::Hosting && state_ != State::Connecting) break;
			if (state_ == State::Hosting && accepted_.count(p)) break;
		}
		if (!host_) return;
	}
	if (net_) net_->update();
	if (net_ && net_->leaving) {
		const std::string title = net_->leave_title, text = net_->leave_text;
		return fail(title, text);
	}
	host_->service();
}

}  // namespace game
