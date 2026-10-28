// Sound output: an SDL audio device mixing the sounds the scripts play (World::sound_requests)
// as DE_CSpeaker would place them -- distance volume between hotspot and falloff, stereo pan from
// the listener's orientation, the noise's own volume and (randomised) frequency.
#pragma once

#include <SDL.h>

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#include "de/sound.h"
#include "music_player.h"
#include "game/runtime.h"

namespace audio {

class Mixer {
public:
	bool open();  // false when there is no audio device
	// No device: headless runs pull the mix themselves (pump), recording it with
	// OPENBG_RECORD_AUDIO.
	void open_offline(int rate);
	void pump(int frames);
	~Mixer();
	// Take the frame's sound requests and start them, heard from `listener` (position and axes).
	void play_requests(game::World &world, const de::Transform &listener);
	// Follow the music speaker (MusicPlayer::update).
	void update_music(game::World &world) {
		if (music_) music_->update(world);
	}

private:
	struct Voice {
		const de::Sound *sound;
		double pos = 0, step = 1;  // in source frames
		float left = 1, right = 1;
	};
	static void callback(void *self, Uint8 *stream, int len);
public:
	void mix(float *out, int frames);

private:
	const de::Sound *sound(const de::World &data, const de::ObjectRef &r);

	SDL_AudioDeviceID dev_ = 0;
	bool offline_ = false;
	int rate_ = 44100;
	std::mutex lock_;
	std::vector<Voice> voices_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, std::unique_ptr<de::Sound>> cache_;
	std::mt19937 rng_{2097};
	std::unique_ptr<MusicPlayer> music_;
	FILE *record_ = nullptr;  // OPENBG_RECORD_AUDIO: raw f32 stereo of everything mixed
};

}  // namespace audio
