#include "de/contents.h"

#include <cstdio>
#include <cstring>

#include "de/lzrw3a.h"
#include "de/md5.h"

namespace de {

std::string Guid::str() const {
	// Windows GUID text form: first three groups are stored little-endian.
	char buf[40];
	snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	         bytes[3], bytes[2], bytes[1], bytes[0], bytes[5], bytes[4], bytes[7], bytes[6], bytes[8], bytes[9],
	         bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
	return buf;
}

void Contents::need(size_t n) const {
	if (n > remaining()) {
		throw FormatError("Contents: read of " + std::to_string(n) + " bytes at " + std::to_string(pos_) +
		                  " exceeds size " + std::to_string(data_.size()));
	}
}

void Contents::seek(size_t p) {
	if (p > data_.size()) {
		throw FormatError("Contents: seek past end");
	}
	pos_ = p;
}

void Contents::skip(size_t n) {
	need(n);
	pos_ += n;
}

uint8_t Contents::u8() {
	need(1);
	return data_[pos_++];
}

uint16_t Contents::u16() {
	need(2);
	uint16_t v = data_[pos_] | (data_[pos_ + 1] << 8);
	pos_ += 2;
	return v;
}

uint32_t Contents::u32() {
	need(4);
	uint32_t v = data_[pos_] | (data_[pos_ + 1] << 8) | (data_[pos_ + 2] << 16) | (uint32_t(data_[pos_ + 3]) << 24);
	pos_ += 4;
	return v;
}

float Contents::f32() {
	uint32_t v = u32();
	float f;
	memcpy(&f, &v, sizeof f);
	return f;
}

std::span<const uint8_t> Contents::bytes(size_t n) {
	need(n);
	std::span<const uint8_t> s(data_.data() + pos_, n);
	pos_ += n;
	return s;
}

uint32_t Contents::squeezed() {
	uint8_t b = u8();
	if (b < 0xFE) {
		return b;
	}
	return b == 0xFE ? u16() : u32();
}

std::string Contents::string() {
	uint32_t len = u32();
	auto s = bytes(len);
	// Stored length includes the terminating NUL.
	size_t n = len && s[len - 1] == 0 ? len - 1 : len;
	return std::string(reinterpret_cast<const char *>(s.data()), n);
}

std::vector<uint8_t> Contents::block() {
	auto s = bytes(u32());
	return {s.begin(), s.end()};
}

Guid Contents::guid() {
	Guid g;
	memcpy(g.bytes, bytes(16).data(), 16);
	return g;
}

void ContentsWriter::u16(uint16_t v) {
	uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
	bytes(b, 2);
}
void ContentsWriter::u32(uint32_t v) {
	uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
	bytes(b, 4);
}
void ContentsWriter::f32(float v) {
	uint32_t u;
	memcpy(&u, &v, 4);
	u32(u);
}
void ContentsWriter::block(std::span<const uint8_t> b) {
	u32(uint32_t(b.size()));
	bytes(b);
}
void ContentsWriter::string(const std::string &s) {
	if (s.empty()) {
		u32(0);
		return;
	}
	u32(uint32_t(s.size() + 1));
	bytes(s.data(), s.size() + 1);
}
void ContentsWriter::guid_pool(const std::vector<Guid> &pool) {
	u8(1);
	u16(uint16_t(pool.size()));
	for (const Guid &g : pool) guid(g);
}
size_t ContentsWriter::begin_chunk(uint8_t version) {
	u8(version);
	return data_.size();
}
void ContentsWriter::end_chunk(size_t at) {
	size_t n = data_.size() - at;
	if (n < 0xFF) {
		data_.insert(data_.begin() + long(at), uint8_t(n));
		return;
	}
	uint8_t b[5] = {0xFF, uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
	data_.insert(data_.begin() + long(at), b, b + 5);
}

std::vector<uint8_t> Contents::decompress(std::span<const uint8_t> packed) {
	Contents c(std::vector<uint8_t>(packed.begin(), packed.end()));
	uint32_t method = c.squeezed();
	switch (method) {
	case 0: {  // stored
		return c.block();
	}
	case 6: {  // LZRW3-A, no checksum
		uint32_t n = c.squeezed();
		return lzrw3a::decompress(c.bytes(c.remaining()), n);
	}
	case 7: {  // MD5 of decoded data, then LZRW3-A
		auto digest = c.bytes(16);
		uint8_t want[16];
		memcpy(want, digest.data(), 16);
		uint32_t n = c.squeezed();
		auto out = lzrw3a::decompress(c.bytes(c.remaining()), n);
		if (memcmp(md5(out).data(), want, 16) != 0) {
			throw FormatError("Contents: decompressed block checksum mismatch");
		}
		return out;
	}
	case 5:
		// "DZip": the shipped engine rejects it too ("saved with an obsolete compression method.
		// It cannot be decompressed.", Core 0x7001a3b0 case 5). Seen on 38 sub-image entries of 20 images.
		throw ObsoleteCompression("Contents: obsolete DZip compression (unreadable by the original engine)");
	default:
		// 1-3: DE_CCompression LZSS tree coder, 4: unknown. Not seen in shipped data.
		throw FormatError("Contents: unsupported compression method " + std::to_string(method));
	}
}

}  // namespace de
