// Image\Standard objects (DE_CStandardImage).
//
// Contents: DE_CImage chunk (Core 0x7018d850: name, type, ResX, ResY, ...) followed by the
// DE_CStandardImage chunk (0x70194420). Pixels live in EX entries keyed by a 2-byte level
// id ("\0\0" = full size) and are read by DE_CStandardImageCompressor::Load (0x7018b4b0):
// u8 version; v > 1 wraps the rest in a DE_CContents::Decompress block; then repeated
// [i32 channel, ResX*ResY bytes] until a negative channel. Channels are planar 8-bit.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "de/objects_file.h"

namespace de {

struct Image {
	std::string name;
	int32_t type = 0;
	int32_t width = 0, height = 0;
	bool mipmap = true;  // +0x4c: the texture gets a mip chain
	// +0x5c (v4+, DE_CImage::IsAdjustMipMapLevel): its texture starts at the Texture Detail level.
	bool adjust_level = false;
	std::map<int32_t, std::vector<uint8_t>> channels;  // channel id -> width*height bytes

	// Interleave into RGBA8: channels 0,1,2 colour, 3 alpha (default 255), 4 luminance.
	std::vector<uint8_t> rgba() const;
};

// Header only (no pixel data); throws FormatError for unexpected layouts.
Image read_image_header(const std::vector<uint8_t> &contents);
// Header plus pixels. EX keys are {compressor id 0, sub-image index} (SetupID, 0x7018b7a0):
// index 0 is this image at its header size; other indices hold sub-images that keep their
// pixels in this owner (owner/index assignment not decoded yet), whose size is inferred from
// the payload layout.
Image read_image(const ObjectsFile &f, const ObjectInfo &o, uint8_t index = 0);

}  // namespace de
