#include "net/link.h"

#include <algorithm>

namespace net {

class LoopbackLink::End : public Endpoint {
public:
	End(LoopbackLink &l, Direction &out, Direction &in) : link_(l), out_(out), in_(in) {}
	void send(Channel channel, std::vector<uint8_t> data) override { link_.send(out_, channel, std::move(data)); }
	bool receive(Packet &out) override { return link_.receive(in_, out); }

private:
	LoopbackLink &link_;
	Direction &out_, &in_;
};

LoopbackLink::LoopbackLink(Conditions c, std::function<uint32_t()> clock, uint32_t seed)
    : c_(c), clock_(std::move(clock)), rng_(seed) {
	a_ = std::make_unique<End>(*this, from_a_, from_b_);
	b_ = std::make_unique<End>(*this, from_b_, from_a_);
}

LoopbackLink::~LoopbackLink() = default;
Endpoint &LoopbackLink::a() { return *a_; }
Endpoint &LoopbackLink::b() { return *b_; }

void LoopbackLink::send(Direction &d, Channel ch, std::vector<uint8_t> data) {
	std::uniform_real_distribution<float> u(0, 1);
	uint32_t now = clock_();
	float delay = c_.delay_ms + (u(rng_) * 2 - 1) * c_.jitter_ms;
	bool lost = c_.loss > 0 && u(rng_) < c_.loss;
	++d.stats.packets;
	d.stats.bytes += data.size();
	if (ch == Channel::Unreliable && lost) {
		++d.stats.lost;
		return;
	}
	if (lost) delay += 2 * c_.delay_ms + 1;
	uint32_t due = now + uint32_t(std::max(0.0f, delay));
	if (ch == Channel::Reliable) due = d.reliable_due = std::max(due, d.reliable_due);
	d.flight.push_back({due, order_++, {ch, std::move(data)}});
}

bool LoopbackLink::receive(Direction &d, Packet &out) {
	uint32_t now = clock_();
	auto best = d.flight.end();
	for (auto it = d.flight.begin(); it != d.flight.end(); ++it) {
		if (it->due <= now && (best == d.flight.end() || it->due < best->due || (it->due == best->due && it->order < best->order))) best = it;
	}
	if (best == d.flight.end()) return false;
	out = std::move(best->packet);
	d.flight.erase(best);
	return true;
}

}  // namespace net
