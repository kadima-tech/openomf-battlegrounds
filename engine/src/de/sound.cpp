#include "de/sound.h"

#include <cstring>

#include "de/contents.h"

namespace de {

namespace {

uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }

}  // namespace

Sound read_sound(const World &w, const ObjectRef &noise) {
	Sound s;
	auto d = noise.file->object_data(noise.info());
	{
		Contents c(d.contents);
		Chunk ch = begin_chunk(c);
		if (ch.version >= 1) {
			uint32_t n = c.u32();
			c.seek(c.pos() + n);
			// v1: volume; v2: + frequency; v3: + frequency variance, alternate chance.
			s.volume = c.f32();
			if (ch.version >= 2) s.frequency = c.f32();
			if (ch.version >= 3) {
				s.frequency_variance = c.f32();
				s.alternate_chance = c.f32();
			}
		}
	}
	// References: [object][noise: properties, alternates].
	try {
		Contents c(d.references);
		Chunk obj = begin_chunk(c);
		c.seek(obj.end);
		Chunk nz = begin_chunk(c);
		for (int list = 0; list < 2 && c.pos() < nz.end; ++list) {
			while (c.pos() < nz.end && c.boolean()) {
				ObjectRef r = w.resolve(*noise.file, Reference::read(c));
				if (list == 1 && r) s.alternates.push_back(r);
			}
		}
	} catch (const std::exception &) {
	}
	auto p = noise.file->ex_payload(d, "DeNoiseSampleExDataId");
	if (!p) throw FormatError("noise " + noise.info().name + " has no samples");
	const auto &b = p->data;
	if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) {
		throw FormatError("noise " + noise.info().name + ": not a RIFF WAVE");
	}
	int bits = 16;
	uint32_t avg_bytes = 0;
	for (size_t o = 12; o + 8 <= b.size();) {
		uint32_t len = le32(&b[o + 4]);
		const uint8_t *body = &b[o + 8];
		if (o + 8 + len > b.size()) len = uint32_t(b.size() - o - 8);
		if (!memcmp(&b[o], "fmt ", 4) && len >= 16) {
			if (le16(body) != 1) throw FormatError("noise " + noise.info().name + ": not PCM");
			s.channels = le16(body + 2);
			s.rate = int(le32(body + 4));
			avg_bytes = le32(body + 8);
			bits = le16(body + 14);
		} else if (!memcmp(&b[o], "data", 4)) {
			if (bits == 16) {
				s.samples.resize(len / 2);
				for (size_t i = 0; i < s.samples.size(); ++i) s.samples[i] = int16_t(le16(body + i * 2));
			} else if (bits == 8) {
				s.samples.resize(len);
				for (size_t i = 0; i < len; ++i) s.samples[i] = int16_t((int(body[i]) - 128) << 8);
			}
		}
		o += 8 + len + (len & 1);
	}
	if (s.samples.empty()) throw FormatError("noise " + noise.info().name + ": no samples");
	if (avg_bytes) s.length_ms = uint32_t(uint64_t(b.size()) * 1000 / avg_bytes);
	return s;
}

}  // namespace de
