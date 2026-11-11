// PNG and BMP files to and from RGBA, for photo import and screenshots.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ed {

struct RgbaImage {
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;  // rows top to bottom
};

// PNG (8- and 16-bit, any colour type, not interlaced) or uncompressed BMP (8, 24, 32 bit).
// Throws std::runtime_error with a message for the user.
RgbaImage load_image(const std::filesystem::path &path);
RgbaImage decode_png(const std::vector<uint8_t> &data);
RgbaImage decode_bmp(const std::vector<uint8_t> &data);

std::vector<uint8_t> encode_png(int width, int height, const std::vector<uint8_t> &rgba);
void write_png(const std::filesystem::path &path, int width, int height, const std::vector<uint8_t> &rgba);

// The centre square of an image, scaled (box filter down, bilinear up) to side x side.
std::vector<uint8_t> crop_square_resize(int width, int height, const std::vector<uint8_t> &rgba, int side);
// The whole image scaled (bilinear) to out_w x out_h.
std::vector<uint8_t> resize_rgba(int width, int height, const std::vector<uint8_t> &rgba, int out_w, int out_h);

}  // namespace ed
