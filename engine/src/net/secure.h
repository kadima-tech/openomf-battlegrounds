// The game traffic's encryption and authentication (docs/NETWORK_PLAN.md 7.9), on Monocypher: an
// ephemeral X25519 exchange per connection, signed by the host's long-term Ed25519 identity, then
// XChaCha20-Poly1305 on every message with a per-channel counter and a replay window.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace net {

using Key32 = std::array<uint8_t, 32>;

// Operating system randomness; false if none could be had.
bool random_bytes(void *out, size_t n);

// A host's long-term identity: an Ed25519 key pair from a 32-byte seed.
struct Identity {
	std::array<uint8_t, 64> secret{};
	Key32 pub{};
	static Identity from_seed(const Key32 &seed);
	static std::optional<Identity> from_hex(const std::string &seed_hex);
	static std::string new_seed_hex();
};
std::string hex(const uint8_t *p, size_t n);
// The first eight bytes of a key in hex, as players compare them.
std::string fingerprint(const Key32 &pub);

// One connection's handshake and keys. The client's hello carries its ephemeral key; the host
// answers with its own, its identity and a signature over both. Every later message is sealed.
class SecureChannel {
public:
	enum class Role { Client, Host };
	static constexpr uint8_t kMagic[4] = {'O', 'B', 'G', 'K'};
	explicit SecureChannel(Role r);
	~SecureChannel();
	// The client's first message.
	std::vector<uint8_t> hello();
	// The host's answer to a hello; empty when the hello is malformed.
	std::vector<uint8_t> answer(const std::vector<uint8_t> &hello, const Identity &id);
	// The client takes the answer; `expect` (when set) must be the host's identity. False with
	// `error` set when the answer is malformed, its signature fails, or the identity differs.
	bool finish(const std::vector<uint8_t> &answer, const std::optional<Key32> &expect, std::string *error);
	bool ready() const { return ready_; }
	const Key32 &peer_identity() const { return peer_identity_; }
	// A message for channel `ch` (0 reliable, 1 unreliable): counter, ciphertext, tag.
	std::vector<uint8_t> seal(uint8_t ch, const std::vector<uint8_t> &plain);
	// False for anything forged, altered, replayed or too old.
	bool open(uint8_t ch, const std::vector<uint8_t> &sealed, std::vector<uint8_t> &plain);
	uint64_t rejected = 0;

private:
	void derive(const Key32 &shared, const Key32 &client_pk, const Key32 &host_pk);
	Role role_;
	Key32 eph_secret_{}, eph_public_{}, peer_identity_{};
	Key32 send_key_{}, recv_key_{};
	bool ready_ = false;
	uint64_t send_counter_[2] = {0, 0};
	uint64_t recv_top_[2] = {0, 0};  // highest counter taken per channel
	uint64_t recv_window_ = 0;       // the unreliable channel's last 64 counters below the top
};

}  // namespace net
