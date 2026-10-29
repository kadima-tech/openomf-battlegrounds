#include "de/avi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

#include "de/contents.h"
#include "de/objects_file.h"
#include "openindeo.h"

namespace de {

namespace {

uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t be24(const uint8_t *p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// --- Cinepak ---------------------------------------------------------------------------------
// The format as documented by the Cinepak bitstream and implemented by every open decoder: a
// frame of horizontal strips, each with a v4 and a v1 codebook of 256 2x2 YUV entries that
// persist between frames, and 4x4 blocks coded either by one v1 entry (scaled up) or four v4
// entries; inter frames skip unchanged blocks.

struct CinepakStrip {
	std::array<std::array<uint8_t, 12>, 256> v4{}, v1{};  // 4 RGB pixels per entry
	int y1 = 0, y2 = 0, x1 = 0, x2 = 0;
};

struct Cinepak {
	int width, height;
	std::vector<uint8_t> image;  // RGB8
	std::vector<CinepakStrip> strips;

	void codebook(std::array<std::array<uint8_t, 12>, 256> &cb, int id, const uint8_t *p, const uint8_t *end) {
		int n = (id & 0x04) ? 4 : 6;
		uint32_t flag = 0, mask = 0;
		for (int i = 0; i < 256; ++i) {
			if ((id & 0x01) && !(mask >>= 1)) {
				if (p + 4 > end) break;
				flag = be32(p), p += 4, mask = 0x80000000u;
			}
			if (!(id & 0x01) || (flag & mask)) {
				if (p + n > end) break;
				int u = n == 6 ? int8_t(p[4]) : 0, v = n == 6 ? int8_t(p[5]) : 0;
				for (int k = 0; k < 4; ++k) {
					int y = p[k];
					cb[size_t(i)][size_t(k * 3 + 0)] = uint8_t(std::clamp(y + v * 2, 0, 255));
					cb[size_t(i)][size_t(k * 3 + 1)] = uint8_t(std::clamp(y - u / 2 - v, 0, 255));
					cb[size_t(i)][size_t(k * 3 + 2)] = uint8_t(std::clamp(y + u * 2, 0, 255));
				}
				p += n;
			}
		}
	}

	void put(int x, int y, const uint8_t *rgb) {
		if (x < 0 || y < 0 || x >= width || y >= height) return;
		memcpy(&image[(size_t(y) * size_t(width) + size_t(x)) * 3], rgb, 3);
	}

	void vectors(CinepakStrip &s, int id, const uint8_t *p, const uint8_t *end) {
		uint32_t flag = 0, mask = 0;
		for (int y = s.y1; y < s.y2; y += 4) {
			for (int x = s.x1; x < s.x2; x += 4) {
				if ((id & 0x01) && !(mask >>= 1)) {
					if (p + 4 > end) return;
					flag = be32(p), p += 4, mask = 0x80000000u;
				}
				if ((id & 0x01) && !(flag & mask)) continue;  // inter frame: block unchanged
				if (!(id & 0x02) && !(mask >>= 1)) {
					if (p + 4 > end) return;
					flag = be32(p), p += 4, mask = 0x80000000u;
				}
				if ((id & 0x02) || (~flag & mask)) {
					// v1: each of the entry's pixels covers a 2x2 quarter of the block.
					if (p + 1 > end) return;
					const auto &e = s.v1[*p++];
					for (int k = 0; k < 4; ++k) {
						int qx = x + (k & 1) * 2, qy = y + (k >> 1) * 2;
						for (int d = 0; d < 4; ++d) put(qx + (d & 1), qy + (d >> 1), &e[size_t(k * 3)]);
					}
				} else {
					// v4: one entry per 2x2 quarter.
					if (p + 4 > end) return;
					for (int k = 0; k < 4; ++k) {
						const auto &e = s.v4[*p++];
						int qx = x + (k & 1) * 2, qy = y + (k >> 1) * 2;
						for (int d = 0; d < 4; ++d) put(qx + (d & 1), qy + (d >> 1), &e[size_t(d * 3)]);
					}
				}
			}
		}
	}

	void strip(CinepakStrip &s, const uint8_t *p, const uint8_t *end) {
		while (p + 4 <= end) {
			int id = p[0];
			size_t size = be24(p + 1);
			if (size < 4) return;
			const uint8_t *data = p + 4, *next = std::min(end, p + size);
			switch (id) {
			case 0x20: case 0x21: case 0x24: case 0x25: codebook(s.v4, id, data, next); break;
			case 0x22: case 0x23: case 0x26: case 0x27: codebook(s.v1, id, data, next); break;
			case 0x30: case 0x31: case 0x32: vectors(s, id, data, next); return;
			default: break;
			}
			p = next;
		}
	}

	void frame(const uint8_t *p, size_t n) {
		if (n < 10) return;
		const uint8_t *end = p + n;
		int flags = p[0];
		int count = be16(p + 8);
		p += 10;
		if (strips.size() < size_t(count)) strips.resize(size_t(count));
		int y0 = 0;
		for (int i = 0; i < count && p + 12 <= end; ++i) {
			CinepakStrip &s = strips[size_t(i)];
			int y1 = be16(p + 4);
			if (y1 == 0) {
				s.y1 = y0;
				s.y2 = y0 + be16(p + 8);
			} else {
				s.y1 = y1;
				s.y2 = be16(p + 8);
			}
			s.x1 = be16(p + 6);
			s.x2 = be16(p + 10);
			size_t size = be24(p + 1);
			if (size < 12) return;
			const uint8_t *data = p + 12, *next = std::min(end, p + size);
			if (i > 0 && !(flags & 0x01)) {
				s.v4 = strips[size_t(i - 1)].v4;
				s.v1 = strips[size_t(i - 1)].v1;
			}
			strip(s, data, next);
			p = next;
			y0 = s.y2;
		}
	}
};

// --- Indeo YVU9 -> RGB24, as Intel's codecs hand it to AVIStreamGetFrame -----------------------
// AVIFIL32's GetFrame with a NULL format takes ICM_DECOMPRESS_GET_FORMAT's answer (avifil32
// 0x73b5f73d), 24-bit BI_RGB from both codecs. Matched bit-exactly against ir41_32.ax and
// ir50_32.dll (Windows XP SP3) under emulation, on every frame of the 18 shipped Indeo videos.

uint8_t clamp8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// Table converter: ir41_32.ax 0x580d9780 (its only RGB24 path), tables built by 0x580d95f0;
// ir50_32.dll's non-MMX path (0x738d4cbd) uses identical tables. Chroma is one sample per 4x4
// block; each output is a clamp table indexed by Y + chroma offset + a 4x4 ordered dither.
void intel_rgb24_table(const oi_frame &f, uint8_t *dst) {
	static const int dr[4][4] = {{0, 1, -1, 2}, {2, -1, 1, 0}, {1, 0, 2, -1}, {-1, 2, 0, 1}};
	static const int dg[4][4] = {{-1, 3, -1, 3}, {2, 0, 0, 2}, {3, -1, 3, -1}, {0, 2, 2, 0}};
	static const int db[4][4] = {{-1, 2, 0, 1}, {0, 1, -1, 2}, {2, -1, 1, 0}, {1, 0, 2, -1}};
	auto rv = [](int i) { return ((int32_t(0xff507f00) + i * 0x15f02) >> 16) + 0xb3; };
	auto gv = [](int i) { return ((0x596700 - i * 0xb2ce) >> 16) + 0x5d; };
	auto gu = [](int i) { return ((0x2aff80 - i * 0x55ff) >> 16) + 0x2e; };
	auto bu = [](int i) { return ((int32_t(0xff223300) + i * 0x1bb9a) >> 16) + 0xe1; };
	auto lut = [](int32_t start, int j) { return clamp8((start + j * 0x12b40) >> 16); };
	for (int y = 0; y < f.height; ++y) {
		const uint8_t *yr = f.y + size_t(y) * size_t(f.y_stride);
		const uint8_t *ur = f.u + size_t(y >> 2) * size_t(f.uv_stride), *vr = f.v + size_t(y >> 2) * size_t(f.uv_stride);
		uint8_t *o = dst + size_t(y) * size_t(f.width) * 3;
		for (int x = 0; x < f.width; ++x, o += 3) {
			int u = ur[x >> 2], v = vr[x >> 2], r = rv(v), b = bu(u);
			// The green offset is summed from packed 9-bit fields, so red + blue can carry into it.
			int g = gv(v) + gu(u) + ((r + b) >> 9);
			o[0] = lut(int32_t(0xff1c0e40), yr[x] + r + dr[y & 3][x & 3]);
			o[1] = lut(int32_t(0xff4ad040), yr[x] + g + dg[y & 3][x & 3]);
			o[2] = lut(int32_t(0xfee648c0), yr[x] + b + db[y & 3][x & 3]);
		}
	}
}

// MMX converter: ir50_32.dll 0x738d41a0, taken when the CPU has MMX and the width is a multiple
// of 8 (0x738d4910). Chroma is interpolated per 2x2 pixels: within a 4x4 block the left/top
// pair uses the block's sample, the right/bottom pair the floor average with the next block's
// (the last row and column repeat). Fixed point in 1/128 with saturating 16-bit arithmetic and
// a per-pixel 4x4 bias table (constants set at 0x738d4a09..0x738d4b40).
void intel_rgb24_mmx(const oi_frame &f, uint8_t *dst) {
	static const uint16_t kr[4][4] = {{0x668c, 0x6428, 0x65c0, 0x64f4}, {0x64f4, 0x65c0, 0x6428, 0x668c},
	                                  {0x6428, 0x668c, 0x64f4, 0x65c0}, {0x65c0, 0x64f4, 0x668c, 0x6428}};
	static const uint16_t kg[4][4] = {{0x4da8, 0x4c3e, 0x4d72, 0x4c74}, {0x4d0a, 0x4cdc, 0x4c70, 0x4d76},
	                                  {0x4c3e, 0x4da8, 0x4c74, 0x4d72}, {0x4cdc, 0x4d0a, 0x4d76, 0x4c70}};
	static const uint16_t kb[4][4] = {{0x80c0, 0x7fbe, 0x81c2, 0x7ebc}, {0x81c2, 0x7ebc, 0x80c0, 0x7fbe},
	                                  {0x7fbe, 0x80c0, 0x7ebc, 0x81c2}, {0x7ebc, 0x81c2, 0x7fbe, 0x80c0}};
	auto sample = [&](const uint8_t *plane, int x, int y) {
		auto h = [&](int row) {
			const uint8_t *p = plane + size_t(std::min(row, f.chroma_height - 1)) * size_t(f.uv_stride);
			int i = x >> 2;
			return (x & 2) ? (p[i] + p[std::min(i + 1, f.chroma_width - 1)]) >> 1 : p[i];
		};
		int j = y >> 2;
		return (y & 2) ? (h(j) + h(j + 1)) >> 1 : h(j);
	};
	auto out = [](int v) { return uint8_t(std::min(std::max(v, 0) >> 7, 255)); };
	for (int y = 0; y < f.height; ++y) {
		const uint8_t *yr = f.y + size_t(y) * size_t(f.y_stride);
		uint8_t *o = dst + size_t(y) * size_t(f.width) * 3;
		for (int x = 0; x < f.width; ++x, o += 3) {
			int u = sample(f.u, x, y), v = sample(f.v, x, y);
			int ys = std::max(yr[x] - 16, 0) * 149;
			o[0] = out(std::min(ys + 204 * v, 0xffff) - kr[y & 3][x & 3]);
			o[1] = out(std::min(ys + kg[y & 3][x & 3], 0xffff) - std::min(50 * u + 104 * v, 0x7fff));
			o[2] = out(std::min(ys + std::min(258 * u, 0xffff), 0xffff) - kb[y & 3][x & 3]);
		}
	}
}

}  // namespace

std::vector<uint8_t> avi_of_image(const std::vector<uint8_t> &contents) {
	Contents c(contents);
	Chunk image = begin_chunk(c);  // DE_CImage
	c.seek(image.end);
	Chunk avi_image = begin_chunk(c);
	if (avi_image.version != 2) return {};
	Chunk avi = begin_chunk(c);  // DE_CAVI
	if (avi.version != 1 || !c.boolean()) return {};
	return c.block();
}

AviVideo decode_avi(const std::vector<uint8_t> &riff) {
	AviVideo v;
	if (riff.size() < 12 || memcmp(riff.data(), "RIFF", 4) || memcmp(riff.data() + 8, "AVI ", 4)) return v;
	const uint8_t *base = riff.data();
	size_t total = std::min(riff.size(), size_t(le32(base + 4)) + 8);
	std::vector<std::pair<size_t, size_t>> chunks;  // video frame chunks (offset, size)
	bool have_video = false;
	int bpp = 0;
	// Walk the lists depth-first; the first 'vids' stream's strf is the video format.
	std::string stream_type;
	std::function<void(size_t, size_t, bool)> walk = [&](size_t o, size_t end, bool movi) {
		while (o + 8 <= end) {
			const uint8_t *p = base + o;
			size_t size = le32(p + 4);
			if (o + 8 + size > end) size = end - o - 8;
			if (!memcmp(p, "LIST", 4) && size >= 4) {
				walk(o + 12, o + 8 + size, movi || !memcmp(p + 8, "movi", 4));
			} else if (!memcmp(p, "strh", 4) && size >= 36) {
				stream_type.assign(reinterpret_cast<const char *>(p + 8), 4);
				if (stream_type == "vids" && !have_video) v.length = le32(p + 8 + 32);
			} else if (!memcmp(p, "strf", 4) && stream_type == "vids" && !have_video && size >= 40) {
				have_video = true;
				v.width = int32_t(le32(p + 8 + 4));
				v.height = int32_t(le32(p + 8 + 8));
				bpp = le16(p + 8 + 14);
				uint32_t comp = le32(p + 8 + 16);
				v.codec = comp == 0 ? "DIB " : std::string(reinterpret_cast<const char *>(p + 8 + 16), 4);
			} else if (movi && p[0] == '0' && p[1] == '0' && (p[3] == 'c' || p[3] == 'b')) {
				chunks.push_back({o + 8, size});
			}
			o += 8 + size + (size & 1);
		}
	};
	walk(12, total, false);
	if (!have_video || v.width <= 0 || v.width > 4096) return v;
	bool bottom_up = v.height > 0;
	v.height = std::abs(v.height);
	if (v.length == 0) v.length = uint32_t(chunks.size());
	size_t n = size_t(v.width) * size_t(v.height) * 3;
	if (v.codec == "DIB " && bpp == 24) {
		size_t stride = (size_t(v.width) * 3 + 3) & ~size_t(3);
		for (auto [o, size] : chunks) {
			if (size == 0 && !v.frames.empty()) {
				v.frames.push_back(v.frames.back());
				continue;
			}
			if (size < stride * size_t(v.height)) continue;
			std::vector<uint8_t> f(n);
			for (int y = 0; y < v.height; ++y) {
				const uint8_t *row = base + o + stride * size_t(bottom_up ? v.height - 1 - y : y);
				for (int x = 0; x < v.width; ++x) {
					uint8_t *d = &f[(size_t(y) * size_t(v.width) + size_t(x)) * 3];
					d[0] = row[x * 3 + 2], d[1] = row[x * 3 + 1], d[2] = row[x * 3];
				}
			}
			v.frames.push_back(std::move(f));
		}
	} else if (v.codec == "cvid") {
		Cinepak cp{v.width, v.height, std::vector<uint8_t>(n, 0), {}};
		for (auto [o, size] : chunks) {
			if (size) cp.frame(base + o, size);
			v.frames.push_back(cp.image);
		}
	} else if (oi_decoder *dec = oi_create(OI_FOURCC(v.codec[0], v.codec[1], v.codec[2], v.codec[3]))) {
		// Indeo 4/5 through openindeo; an undecodable chunk repeats the last picture.
		bool mmx = v.codec != "iv41" && v.codec != "IV41" && v.width % 8 == 0;
		std::vector<uint8_t> last(n, 0);
		for (auto [o, size] : chunks) {
			oi_frame f;
			if (oi_decode(dec, base + o, size, &f) == OI_OK && f.width == v.width && f.height == v.height) {
				mmx ? intel_rgb24_mmx(f, last.data()) : intel_rgb24_table(f, last.data());
			}
			v.frames.push_back(last);
		}
		oi_destroy(dec);
	}
	return v;
}

}  // namespace de
