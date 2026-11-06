// The byte link between two machines of a network session: a reliable ordered channel (session
// and UI messages, the original's "guaranteed" packets) and an unreliable one (game states, which
// the state managers' scheduler re-sends). The ENet transport implements Endpoint later; the
// loopback link below connects two sessions in one process with a simulated delay, jitter and loss.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace net {

enum class Channel : uint8_t { Reliable, Unreliable };

struct Packet {
	Channel channel = Channel::Unreliable;
	std::vector<uint8_t> data;
};

class Endpoint {
public:
	virtual ~Endpoint() = default;
	virtual void send(Channel channel, std::vector<uint8_t> data) = 0;
	virtual bool receive(Packet &out) = 0;
};

// One direction's delay and its spread (uniform, ms), and the share of unreliable packets lost. A
// lost reliable packet arrives a round trip later, as a resend would.
struct Conditions {
	float delay_ms = 0, jitter_ms = 0, loss = 0;
};

struct LinkStats {
	uint64_t packets = 0, bytes = 0, lost = 0;
};

class LoopbackLink {
public:
	// `clock` is both machines' time in ms.
	LoopbackLink(Conditions c, std::function<uint32_t()> clock, uint32_t seed = 1);
	~LoopbackLink();
	Endpoint &a();
	Endpoint &b();
	const LinkStats &stats_from_a() const { return from_a_.stats; }
	const LinkStats &stats_from_b() const { return from_b_.stats; }

private:
	struct InFlight {
		uint32_t due;
		uint64_t order;
		Packet packet;
	};
	struct Direction {
		std::deque<InFlight> flight;
		uint32_t reliable_due = 0;  // reliable packets keep their order
		LinkStats stats;
	};
	class End;
	void send(Direction &d, Channel ch, std::vector<uint8_t> data);
	bool receive(Direction &d, Packet &out);
	Conditions c_;
	std::function<uint32_t()> clock_;
	std::mt19937 rng_;
	uint64_t order_ = 0;
	Direction from_a_, from_b_;
	std::unique_ptr<End> a_, b_;
};

}  // namespace net
