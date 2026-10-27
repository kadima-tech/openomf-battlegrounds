// Image\AVI objects (DE_CAVIImage, Core 0x7018ac10) and the AVI files they hold.
//
// Contents: the DE_CImage chunk, then the DE_CAVIImage chunk (v2) wrapping DE_CAVI's chunk
// (DE_CAVI::ProcessContents: v1, BOOL present, DE_CDataBlock with a whole RIFF AVI file). The
// image takes its size from the video (SetResX/SetResY) and shows one frame:
// DE_CAVIImage::SetPhase (0x7018a550) picks frame trunc(phase x frames) for 0 <= phase < 1,
// frame 0 otherwise. Its pixels are the frame's red, green and blue (no alpha).
//
// The shipped videos are uncompressed 24-bit DIBs, Cinepak ("cvid") or Indeo 4/5 ("iv41",
// "iv50"); Indeo goes through openindeo (../../openindeo), converted to RGB as BT.601 studio
// range with each chroma sample covering its 4x4 pixels.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace de {

struct AviVideo {
	int32_t width = 0, height = 0;
	std::string codec;            // the video stream's compression fourcc ("DIB " for BI_RGB)
	uint32_t length = 0;          // frames in the stream (strh dwLength)
	std::vector<std::vector<uint8_t>> frames;  // RGB8, top row first; empty when not decodable
	bool decoded() const { return !frames.empty(); }
	// SetPhase's frame for a phase.
	size_t frame_for_phase(float phase) const {
		if (!(phase >= 0 && phase < 1) || length == 0) return 0;
		return size_t(float(length) * phase);
	}
};

// The RIFF AVI held by an Image\AVI's contents (empty when it has none).
std::vector<uint8_t> avi_of_image(const std::vector<uint8_t> &contents);
// Parses the file and decodes every frame of its first video stream when the codec is known.
AviVideo decode_avi(const std::vector<uint8_t> &riff);

}  // namespace de
