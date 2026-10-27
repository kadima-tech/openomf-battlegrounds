#include "de/lzrw3a.h"

#include <array>
#include <cstring>

#include "de/contents.h"

namespace de::lzrw3a {

namespace {

// Every hash slot starts out pointing at this string, so early copy items that
// reference an unused slot still decode deterministically.
constexpr char kStart[] = "123456789012345678";
constexpr size_t kStartLen = sizeof kStart - 1;
constexpr int kFlagCopy = 1;

// 4096 slots in 512 sets of 8. Only bits 4..12 of the product are used, so the
// original's multiplier (0x...FE5F in the binary) is equivalent to 40543.
inline size_t set_of(const uint8_t *p) {
	return ((((((uint32_t)p[0] << 4) ^ p[1]) << 4) ^ p[2]) * 40543u >> 4) & 0x1FF;
}

}  // namespace

std::vector<uint8_t> decompress(std::span<const uint8_t> src, size_t out_len) {
	if (src.size() < 4) {
		throw FormatError("LZRW3-A: stream too short");
	}
	if (src[0] == kFlagCopy) {
		return {src.begin() + 4, src.end()};
	}

	// Prefix the output with kStart so table entries are plain offsets.
	std::vector<uint8_t> out(kStart, kStart + kStartLen);
	out.reserve(kStartLen + out_len + 18);
	std::array<uint32_t, 4096> table{};  // 0 == kStart
	uint32_t cycle = 0;
	uint32_t pending = 0;  // literals not yet entered in the table
	uint32_t ctrl = 1;
	size_t p = 4;
	const size_t end = src.size();

	auto insert = [&](size_t slot, size_t at) {
		table[slot] = static_cast<uint32_t>(at);
		cycle = (cycle + 1) & 7;
	};

	while (p != end) {
		if (ctrl == 1) {
			if (p + 2 > end) {
				throw FormatError("LZRW3-A: truncated control word");
			}
			ctrl = src[p] | (src[p + 1] << 8) | 0x10000;
			p += 2;
		}
		// Near the end the original processes one item per bounds check.
		int items = p > end - std::min<size_t>(end, 0x20) ? 1 : 16;
		for (; items && p != end; --items, ctrl >>= 1) {
			if ((ctrl & 1) == 0) {
				out.push_back(src[p++]);
				if (++pending == 3) {
					size_t q = out.size() - 3;
					insert(cycle + set_of(&out[q]) * 8, q);
					pending = 2;
				}
				continue;
			}
			if (p + 2 > end) {
				throw FormatError("LZRW3-A: truncated copy item");
			}
			uint8_t b1 = src[p], b2 = src[p + 1];
			p += 2;
			size_t index = ((b1 & 0xF0) << 4) | b2;
			size_t ref = table[index];
			size_t here = out.size();
			size_t len = (b1 & 0xF) + 3;
			// Byte-wise so overlapping copies (ref + len > here) repeat as in the original.
			for (size_t k = 0; k < len; ++k) {
				out.push_back(out[ref + k]);
			}
			if (pending) {
				size_t q = here - pending;
				insert(cycle + set_of(&out[q]) * 8, q);
				if (pending == 2) {
					insert(cycle + set_of(&out[q + 1]) * 8, q + 1);
				}
				pending = 0;
			}
			insert((index & ~size_t(7)) + cycle, here);
		}
	}

	if (out.size() - kStartLen != out_len) {
		throw FormatError("LZRW3-A: decoded " + std::to_string(out.size() - kStartLen) + " bytes, expected " +
		                  std::to_string(out_len));
	}
	return {out.begin() + kStartLen, out.end()};
}

}  // namespace de::lzrw3a
