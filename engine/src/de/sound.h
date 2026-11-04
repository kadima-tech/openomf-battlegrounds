// Sounds (Core DE_CNoise / DE_CStreamNoise, "Noise2\Stream"): contents v3 = [name][volume +0x54]
// [frequency +0x58][frequency variance +0x5c][alternate chance % +0x60]; the samples are a RIFF
// WAVE in the "DeNoiseSampleExDataId" EX entry. A noise may list alternates (second reference
// list): GetNoiseToPlay picks one of them with the alternate chance.
#pragma once

#include <cstdint>
#include <vector>

#include "de/scene.h"

namespace de {

struct Sound {
	float volume = 1, frequency = 1, frequency_variance = 0, alternate_chance = 0;
	int rate = 22050, channels = 1;
	// DE_CStreamNoise::Length: the whole file's bytes x 1000 / nAvgBytesPerSec (integer ms).
	uint32_t length_ms = 0;
	// The RIFF file's bytes and a sample frame's: a buffer's PlayProgress is its write cursor over
	// file bytes - 44 (DE_CDirectXSpeakerElement::PlayProgress).
	uint32_t file_bytes = 0;
	int block_align = 2;
	std::vector<int16_t> samples;  // interleaved
	std::vector<ObjectRef> alternates;
};

// Throws FormatError when the object has no decodable samples.
Sound read_sound(const World &w, const ObjectRef &noise);

}  // namespace de
