#include "de/image.h"

#include "de/avi.h"

#include <cmath>
#include <cstring>

namespace de {

std::vector<uint8_t> Image::rgba() const {
	size_t n = size_t(width) * size_t(height);
	std::vector<uint8_t> out(n * 4, 255);
	auto plane = [&](int32_t id, int32_t fallback) -> const std::vector<uint8_t> * {
		auto it = channels.find(id);
		if (it == channels.end()) {
			it = channels.find(fallback);
		}
		return it == channels.end() || it->second.size() < n ? nullptr : &it->second;
	};
	// Channel ids: 0-2 colour, 3 alpha, 4 luminance (image type 3, AssureChannel(this, 4)).
	const auto *r = plane(0, 4), *g = plane(1, 4), *b = plane(2, 4), *a = plane(3, -1);
	for (size_t i = 0; i < n; ++i) {
		out[i * 4 + 0] = r ? (*r)[i] : 0;
		out[i * 4 + 1] = g ? (*g)[i] : 0;
		out[i * 4 + 2] = b ? (*b)[i] : 0;
		if (a) {
			out[i * 4 + 3] = (*a)[i];
		}
	}
	return out;
}

Image read_image_header(const std::vector<uint8_t> &contents) {
	Contents c(contents);
	Image img;
	Chunk ch = begin_chunk(c);
	auto name = c.block();
	img.name.assign(name.begin(), name.end());
	while (!img.name.empty() && img.name.back() == 0) {
		img.name.pop_back();
	}
	if (ch.version < 3) {
		throw FormatError("Image: unsupported DE_CImage version " + std::to_string(ch.version));
	}
	img.type = c.i32();
	img.width = c.i32();
	img.height = c.i32();
	if (c.pos() < ch.end) img.mipmap = c.boolean();
	c.seek(ch.end);
	if (img.width < 0 || img.height < 0 || img.width > 8192 || img.height > 8192) {
		throw FormatError("Image: implausible size");
	}
	return img;
}

namespace {

// Plane size of a sub-image payload: k planes of n bytes, k+1 ids (0..k-1 then -1).
size_t infer_plane_size(const std::vector<uint8_t> &data) {
	auto id_at = [&](size_t p) {
		int32_t v;
		memcpy(&v, data.data() + p, 4);
		return v;
	};
	for (size_t k = 1; k <= 8; ++k) {
		if (data.size() < 4 * (k + 1) || (data.size() - 4 * (k + 1)) % k) {
			continue;
		}
		size_t n = (data.size() - 4 * (k + 1)) / k;
		bool ok = true;
		for (size_t i = 0; i <= k && ok; ++i) {
			int32_t id = id_at(i * (n + 4));
			ok = i == k ? id < 0 : id >= 0;
		}
		if (ok) {
			return n;
		}
	}
	throw FormatError("Image: cannot infer sub-image layout");
}

}  // namespace

Image read_image(const ObjectsFile &f, const ObjectInfo &o, uint8_t index) {
	auto d = f.object_data(o);
	Image img = read_image_header(d.contents);
	if (o.class_path == "Image\\AVI" && index == 0) {
		// The video's first frame (SetPhase(0) on loading); its size is the video's.
		AviVideo v = decode_avi(avi_of_image(d.contents));
		img.width = v.width, img.height = v.height;
		if (v.decoded()) {
			size_t n = size_t(v.width) * size_t(v.height);
			for (int k = 0; k < 3; ++k) {
				auto &plane = img.channels[k];
				plane.resize(n);
				for (size_t i = 0; i < n; ++i) plane[i] = v.frames[0][i * 3 + size_t(k)];
			}
		}
		return img;
	}
	const char key[2] = {0, char(index)};
	auto payload = f.ex_payload(d, std::string_view(key, 2));
	if (!payload || (index == 0 && img.width * img.height == 0)) {
		return img;  // procedural/rendered or empty (0x0) images have no stored pixels
	}
	Contents p(payload->data);
	uint8_t version = p.u8();
	std::vector<uint8_t> data = version > 1 ? Contents::decompress(p.block()) : std::vector<uint8_t>(payload->data.begin() + 1, payload->data.end());
	size_t n = size_t(img.width) * size_t(img.height);
	if (index != 0) {
		if (data.size() <= 8) {
			return img;  // empty sub-image: just the terminator
		}
		n = infer_plane_size(data);
		// Sub-images seen so far are square.
		img.width = img.height = int32_t(std::lround(std::sqrt(double(n))));
		if (size_t(img.width) * size_t(img.height) != n) {
			img.width = int32_t(n);
			img.height = 1;
		}
	}
	Contents q(std::move(data));
	for (;;) {
		int32_t id = q.i32();
		if (id < 0) {
			break;
		}
		auto px = q.bytes(n);
		img.channels[id].assign(px.begin(), px.end());
	}
	return img;
}

}  // namespace de
