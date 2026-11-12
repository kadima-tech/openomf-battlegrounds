#include "net/secure.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace net {

namespace {

constexpr uint8_t kHello = 1, kAnswer = 2, kVersion = 1;
constexpr char kSignContext[] = "OpenBG handshake 1";
constexpr char kKeyContext[] = "OpenBG keys 1";
constexpr size_t kHelloSize = 6 + 32, kAnswerSize = 6 + 32 + 32 + 64;
constexpr size_t kOverhead = 8 + 16;

void put64(uint8_t *p, uint64_t v) {
	for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint64_t get64(const uint8_t *p) {
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
	return v;
}

void nonce_for(uint8_t nonce[24], uint8_t ch, uint64_t counter) {
	memset(nonce, 0, 24);
	put64(nonce, counter);
	nonce[8] = ch;
}

bool header_ok(const std::vector<uint8_t> &d, uint8_t type, size_t size) {
	return d.size() == size && memcmp(d.data(), SecureChannel::kMagic, 4) == 0 && d[4] == type && d[5] == kVersion;
}

}  // namespace

bool random_bytes(void *out, size_t n) {
#if defined(_WIN32)
	return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	arc4random_buf(out, n);
	return true;
#else
	auto *p = static_cast<uint8_t *>(out);
	while (n > 0) {
		ssize_t got = getrandom(p, n, 0);
		if (got < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		p += got, n -= size_t(got);
	}
	return true;
#endif
}

std::string hex(const uint8_t *p, size_t n) {
	static const char *digits = "0123456789abcdef";
	std::string s;
	for (size_t i = 0; i < n; ++i) s += digits[p[i] >> 4], s += digits[p[i] & 15];
	return s;
}

std::string fingerprint(const Key32 &pub) { return hex(pub.data(), 8); }

Identity Identity::from_seed(const Key32 &seed) {
	Identity id;
	Key32 s = seed;  // crypto_ed25519_key_pair wipes its seed
	crypto_ed25519_key_pair(id.secret.data(), id.pub.data(), s.data());
	return id;
}

std::optional<Identity> Identity::from_hex(const std::string &t) {
	if (t.size() != 64) return std::nullopt;
	Key32 seed{};
	for (size_t i = 0; i < 32; ++i) {
		auto nib = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
		int hi = nib(t[2 * i]), lo = nib(t[2 * i + 1]);
		if (hi < 0 || lo < 0) return std::nullopt;
		seed[i] = uint8_t(hi << 4 | lo);
	}
	Identity id = from_seed(seed);
	crypto_wipe(seed.data(), seed.size());
	return id;
}

std::string Identity::new_seed_hex() {
	Key32 seed{};
	if (!random_bytes(seed.data(), seed.size())) return {};
	std::string s = hex(seed.data(), seed.size());
	crypto_wipe(seed.data(), seed.size());
	return s;
}

SecureChannel::SecureChannel(Role r) : role_(r) {
	random_bytes(eph_secret_.data(), eph_secret_.size());
	crypto_x25519_public_key(eph_public_.data(), eph_secret_.data());
}

SecureChannel::~SecureChannel() {
	crypto_wipe(eph_secret_.data(), eph_secret_.size());
	crypto_wipe(send_key_.data(), send_key_.size());
	crypto_wipe(recv_key_.data(), recv_key_.size());
}

std::vector<uint8_t> SecureChannel::hello() {
	std::vector<uint8_t> d(kMagic, kMagic + 4);
	d.push_back(kHello);
	d.push_back(kVersion);
	d.insert(d.end(), eph_public_.begin(), eph_public_.end());
	return d;
}

void SecureChannel::derive(const Key32 &shared, const Key32 &client_pk, const Key32 &host_pk) {
	std::vector<uint8_t> m(kKeyContext, kKeyContext + sizeof kKeyContext - 1);
	m.insert(m.end(), shared.begin(), shared.end());
	m.insert(m.end(), client_pk.begin(), client_pk.end());
	m.insert(m.end(), host_pk.begin(), host_pk.end());
	uint8_t keys[64];
	crypto_blake2b(keys, sizeof keys, m.data(), m.size());
	const uint8_t *c2s = keys, *s2c = keys + 32;
	memcpy(send_key_.data(), role_ == Role::Client ? c2s : s2c, 32);
	memcpy(recv_key_.data(), role_ == Role::Client ? s2c : c2s, 32);
	crypto_wipe(keys, sizeof keys);
	crypto_wipe(m.data(), m.size());
	crypto_wipe(eph_secret_.data(), eph_secret_.size());
	ready_ = true;
}

std::vector<uint8_t> SecureChannel::answer(const std::vector<uint8_t> &h, const Identity &id) {
	if (role_ != Role::Host || ready_ || !header_ok(h, kHello, kHelloSize)) return {};
	Key32 client_pk, shared;
	memcpy(client_pk.data(), h.data() + 6, 32);
	crypto_x25519(shared.data(), eph_secret_.data(), client_pk.data());
	std::vector<uint8_t> signed_part(kSignContext, kSignContext + sizeof kSignContext - 1);
	signed_part.insert(signed_part.end(), client_pk.begin(), client_pk.end());
	signed_part.insert(signed_part.end(), eph_public_.begin(), eph_public_.end());
	uint8_t sig[64];
	crypto_ed25519_sign(sig, id.secret.data(), signed_part.data(), signed_part.size());
	std::vector<uint8_t> d(kMagic, kMagic + 4);
	d.push_back(kAnswer);
	d.push_back(kVersion);
	d.insert(d.end(), eph_public_.begin(), eph_public_.end());
	d.insert(d.end(), id.pub.begin(), id.pub.end());
	d.insert(d.end(), sig, sig + 64);
	peer_identity_ = id.pub;
	derive(shared, client_pk, eph_public_);
	crypto_wipe(shared.data(), shared.size());
	return d;
}

bool SecureChannel::finish(const std::vector<uint8_t> &a, const std::optional<Key32> &expect, std::string *error) {
	auto fail = [error](const char *why) {
		if (error) *error = why;
		return false;
	};
	if (role_ != Role::Client || ready_ || !header_ok(a, kAnswer, kAnswerSize)) return fail("The server's handshake was not understood.");
	Key32 host_pk, identity;
	memcpy(host_pk.data(), a.data() + 6, 32);
	memcpy(identity.data(), a.data() + 38, 32);
	std::vector<uint8_t> signed_part(kSignContext, kSignContext + sizeof kSignContext - 1);
	signed_part.insert(signed_part.end(), eph_public_.begin(), eph_public_.end());
	signed_part.insert(signed_part.end(), host_pk.begin(), host_pk.end());
	if (crypto_ed25519_check(a.data() + 70, identity.data(), signed_part.data(), signed_part.size()) != 0)
		return fail("The server's handshake signature is not valid.");
	if (expect && *expect != identity) return fail("The server is not the one the lobby listed (its identity key differs).");
	Key32 shared;
	crypto_x25519(shared.data(), eph_secret_.data(), host_pk.data());
	peer_identity_ = identity;
	derive(shared, eph_public_, host_pk);
	crypto_wipe(shared.data(), shared.size());
	return true;
}

std::vector<uint8_t> SecureChannel::seal(uint8_t ch, const std::vector<uint8_t> &plain) {
	std::vector<uint8_t> out(kOverhead + plain.size());
	uint64_t counter = ++send_counter_[ch & 1];
	put64(out.data(), counter);
	uint8_t nonce[24];
	nonce_for(nonce, ch & 1, counter);
	const uint8_t ad = ch & 1;
	crypto_aead_lock(out.data() + 8, out.data() + 8 + plain.size(), send_key_.data(), nonce, &ad, 1, plain.data(), plain.size());
	return out;
}

bool SecureChannel::open(uint8_t ch, const std::vector<uint8_t> &sealed, std::vector<uint8_t> &plain) {
	ch &= 1;
	if (!ready_ || sealed.size() < kOverhead) return ++rejected, false;
	const uint64_t counter = get64(sealed.data());
	uint64_t &top = recv_top_[ch];
	if (counter == 0) return ++rejected, false;
	if (ch == 0 && counter <= top) return ++rejected, false;
	if (ch == 1 && counter <= top && (top - counter >= 64 || (recv_window_ >> (top - counter)) & 1)) return ++rejected, false;
	const size_t n = sealed.size() - kOverhead;
	plain.resize(n);
	uint8_t nonce[24];
	nonce_for(nonce, ch, counter);
	const uint8_t ad = ch;
	if (crypto_aead_unlock(plain.data(), sealed.data() + 8 + n, recv_key_.data(), nonce, &ad, 1, sealed.data() + 8, n) != 0) {
		plain.clear();
		return ++rejected, false;
	}
	if (ch == 1) {
		if (counter > top) {
			uint64_t shift = counter - top;
			recv_window_ = shift >= 64 ? 0 : recv_window_ << shift;
			recv_window_ |= 1;
			top = counter;
		} else {
			recv_window_ |= uint64_t(1) << (top - counter);
		}
	} else {
		top = counter;
	}
	return true;
}

}  // namespace net
