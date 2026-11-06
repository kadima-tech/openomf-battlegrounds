#include "image_io.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace ed {

namespace {

uint32_t be32(const std::vector<uint8_t> &b, size_t at) {
	if (at + 4 > b.size()) throw std::runtime_error("the image file is cut short");
	return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
}

uint32_t le32(const std::vector<uint8_t> &b, size_t at) {
	if (at + 4 > b.size()) throw std::runtime_error("the image file is cut short");
	return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

uint16_t le16(const std::vector<uint8_t> &b, size_t at) {
	if (at + 2 > b.size()) throw std::runtime_error("the image file is cut short");
	return uint16_t(b[at] | b[at + 1] << 8);
}

uint8_t paeth(int a, int b, int c) {
	int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
	return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

}  // namespace

RgbaImage decode_png(const std::vector<uint8_t> &data) {
	static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	if (data.size() < 8 || memcmp(data.data(), sig, 8) != 0) throw std::runtime_error("not a PNG file");
	int width = 0, height = 0, depth = 0, type = 0, interlace = 0;
	std::vector<uint8_t> idat, palette, trns;
	for (size_t p = 8; p + 8 <= data.size();) {
		uint32_t len = be32(data, p);
		std::string tag(data.begin() + long(p) + 4, data.begin() + long(p) + 8);
		size_t body = p + 8;
		if (body + len > data.size()) throw std::runtime_error("the PNG file is cut short");
		if (tag == "IHDR" && len >= 13) {
			width = int(be32(data, body)), height = int(be32(data, body + 4));
			depth = data[body + 8], type = data[body + 9], interlace = data[body + 12];
		} else if (tag == "PLTE") {
			palette.assign(data.begin() + long(body), data.begin() + long(body + len));
		} else if (tag == "tRNS") {
			trns.assign(data.begin() + long(body), data.begin() + long(body + len));
		} else if (tag == "IDAT") {
			idat.insert(idat.end(), data.begin() + long(body), data.begin() + long(body + len));
		} else if (tag == "IEND") {
			break;
		}
		p = body + len + 4;
	}
	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) throw std::runtime_error("the PNG has no usable size");
	if (interlace) throw std::runtime_error("interlaced PNGs are not supported; save it without interlacing");
	static const int channels_of[7] = {1, 0, 3, 1, 2, 0, 4};
	if (type > 6 || !channels_of[type]) throw std::runtime_error("unsupported PNG colour type");
	int channels = channels_of[type];
	size_t bits = size_t(channels) * size_t(depth);
	size_t stride = (size_t(width) * bits + 7) / 8, bpp = std::max<size_t>(1, bits / 8);
	std::vector<uint8_t> raw(size_t(height) * (stride + 1));
	uLongf raw_len = uLongf(raw.size());
	if (uncompress(raw.data(), &raw_len, idat.data(), uLong(idat.size())) != Z_OK || raw_len != raw.size()) {
		throw std::runtime_error("the PNG's image data is damaged");
	}
	std::vector<uint8_t> prev(stride, 0), cur(stride);
	RgbaImage out;
	out.width = width, out.height = height;
	out.rgba.assign(size_t(width) * size_t(height) * 4, 255);
	auto sample = [&](const std::vector<uint8_t> &row, size_t index) -> int {
		if (depth == 8) return row[index];
		if (depth == 16) return row[index * 2];
		size_t bit = index * size_t(depth);
		int v = (row[bit / 8] >> (8 - depth - int(bit % 8))) & ((1 << depth) - 1);
		return v;
	};
	for (int y = 0; y < height; ++y) {
		const uint8_t *src = raw.data() + size_t(y) * (stride + 1);
		uint8_t filter = src[0];
		for (size_t i = 0; i < stride; ++i) {
			int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = src[1 + i];
			switch (filter) {
				case 0: cur[i] = uint8_t(x); break;
				case 1: cur[i] = uint8_t(x + a); break;
				case 2: cur[i] = uint8_t(x + b); break;
				case 3: cur[i] = uint8_t(x + (a + b) / 2); break;
				case 4: cur[i] = uint8_t(x + paeth(a, b, c)); break;
				default: throw std::runtime_error("the PNG's image data is damaged");
			}
		}
		for (int x = 0; x < width; ++x) {
			uint8_t *px = out.rgba.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
			size_t s = size_t(x) * size_t(channels);
			int scale = depth < 8 ? 255 / ((1 << depth) - 1) : 1;
			if (type == 3) {
				size_t k = size_t(sample(cur, size_t(x)));
				if (k * 3 + 2 < palette.size()) px[0] = palette[k * 3], px[1] = palette[k * 3 + 1], px[2] = palette[k * 3 + 2];
				if (k < trns.size()) px[3] = trns[k];
			} else if (type == 0 || type == 4) {
				int g = sample(cur, s) * scale;
				px[0] = px[1] = px[2] = uint8_t(g);
				if (type == 4) px[3] = uint8_t(sample(cur, s + 1));
			} else {
				for (int k = 0; k < channels; ++k) px[k] = uint8_t(sample(cur, s + size_t(k)));
			}
		}
		std::swap(prev, cur);
	}
	return out;
}

RgbaImage decode_bmp(const std::vector<uint8_t> &data) {
	if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') throw std::runtime_error("not a BMP file");
	uint32_t offset = le32(data, 10), header = le32(data, 14);
	if (header < 40) throw std::runtime_error("unsupported BMP header");
	int32_t width = int32_t(le32(data, 18)), height = int32_t(le32(data, 22));
	uint16_t bpp = le16(data, 28);
	uint32_t compression = le32(data, 30);
	if (compression != 0 && !(compression == 3 && bpp == 32)) throw std::runtime_error("compressed BMPs are not supported");
	if (bpp != 8 && bpp != 24 && bpp != 32) throw std::runtime_error("only 8, 24 and 32-bit BMPs are supported");
	bool top_down = height < 0;
	height = std::abs(height);
	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) throw std::runtime_error("the BMP has no usable size");
	size_t stride = (size_t(width) * bpp / 8 + 3) & ~size_t(3);
	if (offset + stride * size_t(height) > data.size()) throw std::runtime_error("the BMP file is cut short");
	size_t palette = 14 + header;
	RgbaImage out;
	out.width = width, out.height = height;
	out.rgba.resize(size_t(width) * size_t(height) * 4);
	for (int y = 0; y < height; ++y) {
		const uint8_t *row = data.data() + offset + stride * size_t(top_down ? y : height - 1 - y);
		for (int x = 0; x < width; ++x) {
			uint8_t *px = out.rgba.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
			if (bpp == 8) {
				size_t k = palette + size_t(row[x]) * 4;
				if (k + 3 > data.size()) throw std::runtime_error("the BMP's palette is cut short");
				px[0] = data[k + 2], px[1] = data[k + 1], px[2] = data[k], px[3] = 255;
			} else {
				const uint8_t *s = row + size_t(x) * (bpp / 8);
				px[0] = s[2], px[1] = s[1], px[2] = s[0], px[3] = 255;
			}
		}
	}
	return out;
}

RgbaImage load_image(const std::filesystem::path &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) throw std::runtime_error("cannot open " + path.string());
	std::vector<uint8_t> data{std::istreambuf_iterator<char>(f), {}};
	if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M') return decode_bmp(data);
	return decode_png(data);
}

std::vector<uint8_t> encode_png(int width, int height, const std::vector<uint8_t> &rgba) {
	std::vector<uint8_t> raw;
	raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
	for (int y = 0; y < height; ++y) {
		raw.push_back(0);
		auto row = rgba.begin() + long(size_t(y) * size_t(width) * 4);
		raw.insert(raw.end(), row, row + long(width) * 4);
	}
	uLongf zlen = compressBound(uLong(raw.size()));
	std::vector<uint8_t> z(zlen);
	compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6);
	z.resize(zlen);
	std::vector<uint8_t> out = {137, 80, 78, 71, 13, 10, 26, 10};
	auto chunk = [&](const char *tag, const std::vector<uint8_t> &body) {
		uint32_t n = uint32_t(body.size());
		for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(n >> s));
		size_t start = out.size();
		out.insert(out.end(), tag, tag + 4);
		out.insert(out.end(), body.begin(), body.end());
		uint32_t crc = uint32_t(crc32(0, out.data() + start, uInt(out.size() - start)));
		for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(crc >> s));
	};
	std::vector<uint8_t> ihdr(13, 0);
	for (int s = 0; s < 4; ++s) ihdr[size_t(s)] = uint8_t(uint32_t(width) >> (24 - 8 * s)), ihdr[size_t(4 + s)] = uint8_t(uint32_t(height) >> (24 - 8 * s));
	ihdr[8] = 8, ihdr[9] = 6;
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
	return out;
}

void write_png(const std::filesystem::path &path, int width, int height, const std::vector<uint8_t> &rgba) {
	auto png = encode_png(width, height, rgba);
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) throw std::runtime_error("cannot write " + path.string());
	f.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
}

std::vector<uint8_t> crop_square_resize(int width, int height, const std::vector<uint8_t> &rgba, int side) {
	int crop = std::min(width, height), x0 = (width - crop) / 2, y0 = (height - crop) / 2;
	std::vector<uint8_t> out(size_t(side) * size_t(side) * 4);
	float scale = float(crop) / float(side);
	for (int y = 0; y < side; ++y) {
		for (int x = 0; x < side; ++x) {
			float acc[4] = {};
			float fx0 = float(x) * scale, fy0 = float(y) * scale;
			if (scale > 1) {
				int sx0 = int(fx0), sy0 = int(fy0), sx1 = std::max(sx0 + 1, int(fx0 + scale)), sy1 = std::max(sy0 + 1, int(fy0 + scale));
				int n = 0;
				for (int sy = sy0; sy < sy1 && sy < crop; ++sy) {
					for (int sx = sx0; sx < sx1 && sx < crop; ++sx, ++n) {
						const uint8_t *p = rgba.data() + (size_t(y0 + sy) * size_t(width) + size_t(x0 + sx)) * 4;
						for (int k = 0; k < 4; ++k) acc[k] += p[k];
					}
				}
				for (float &a : acc) a /= float(std::max(n, 1));
			} else {
				float sx = std::clamp((float(x) + 0.5f) * scale - 0.5f, 0.0f, float(crop - 1));
				float sy = std::clamp((float(y) + 0.5f) * scale - 0.5f, 0.0f, float(crop - 1));
				int ix = int(sx), iy = int(sy), jx = std::min(ix + 1, crop - 1), jy = std::min(iy + 1, crop - 1);
				float fx = sx - float(ix), fy = sy - float(iy);
				auto at = [&](int px, int py, int k) {
					return float(rgba[(size_t(y0 + py) * size_t(width) + size_t(x0 + px)) * 4 + size_t(k)]);
				};
				for (int k = 0; k < 4; ++k) {
					acc[k] = (at(ix, iy, k) * (1 - fx) + at(jx, iy, k) * fx) * (1 - fy) + (at(ix, jy, k) * (1 - fx) + at(jx, jy, k) * fx) * fy;
				}
			}
			for (int k = 0; k < 4; ++k) out[(size_t(y) * size_t(side) + size_t(x)) * 4 + size_t(k)] = uint8_t(std::lround(std::clamp(acc[k], 0.0f, 255.0f)));
		}
	}
	return out;
}

}  // namespace ed
