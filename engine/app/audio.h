// Sound output: an SDL audio device mixing the sounds the scripts play (World::sound_requests)
// as AudioPlayer's DirectSound speakers do: each sound is a speaker sphere whose volume and
// head-relative position follow the listener every frame (DE_CDirectXSpeakerElement::Update),
// with the noise's own volume and (randomised) frequency. The speakers of the affectors in the scene
// (the arenas' ambience, the crowd's cheers) play through speaker elements that trade their state
// with the speaker every frame (DE_CSpeaker::Process), so their IsPlaying and PlayProgress come
// from the playing voice.
#pragma once

#include <SDL.h>

#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
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
	std::string debug_counts();  // OPENBG_DEBUG_MEMORY
	// Take the frame's sound requests and start them, heard from `listener` (position and axes);
	// then update_speakers.
	void play_requests(game::World &world, const de::Transform &listener);
	// The speakers' audio update. Without a device or pump, their voices advance by
	// the frame's time at 44100 Hz, as a pumped run's do.
	void update_speakers(game::World &world, const de::Transform &listener);
	// Follow the music speaker (MusicPlayer::update).
	void update_music(game::World &world) {
		if (music_) music_->update(world);
	}

private:
	static constexpr int kFracBits = 32;
	struct Voice {
		const de::Sound *sound;
		uint64_t pos = 0, step = uint64_t(1) << kFracBits;  // source frames, 32.32 fixed point
		uint32_t hz = 0;                                     // IDirectSoundBuffer::SetFrequency
		bool playing = true, loop = false, silent = false;
		float left = 1, right = 1;
		// The speaker: its sphere (centre, hotspot, falloff), volume (+0x50) and update rate
		// (+0xb0, +0xac).
		std::array<float, 3> at{0, 0, 0};
		bool on_camera = false;  // parented to the camera at (0, 0, -10000)
		float hotspot = 0, falloff = 0, volume = 1;
		float rate = 1, due = 99999.0f;
	};
	// DE_CDirectXSpeakerElement: a speaker property of an affector in the scene.
	struct Element {
		game::GameObject *speaker = nullptr;
		game::GameObject *affector = nullptr;  // +0xc: null once it left the scene
		std::string name;
		game::GameObject *noise_object = nullptr;  // its noise element (+0x78)
		const de::Sound *noise = nullptr;
		float hotspot = 0, falloff = 0;  // +0x5c, +0x60: its sphere affector's when made
		float volume = -1;               // +0x50
		float rate = 1, due = 99999.0f;  // +0xb0, +0xac
		bool loop = false;               // flag 0x800
		bool finish = false;             // flag 0x400: plays on after its affector goes
		bool doomed = false;             // flag 0x1000: the player drops it
		bool head_relative = false;      // +0xa8
		float last_volume = 0;           // +0x54
		bool by_volume = false;          // flag 0x4000000: started and stopped by its volume
		bool active = false;             // flag 0x2000000
		bool stopping = false;           // flag 0x4000
		int stop_ms = 0;                 // +0x98
		bool buffer = false;             // +0x8c: `voice` is its DirectSound buffer
		Voice voice{nullptr};
		std::array<float, 3> at{0, 0, 0};  // +0x14
		bool heard = false;                // OPENBG_DEBUG_SOUND
	};
	// DE_CDirectXSpeakerElement::Update: the voice's gains, heard from `listener`, `ms` after the
	// last update.
	void update(Voice &v, const de::Transform &listener, float ms);
	// The gains for a buffer volume (mB) at `at` (dsound's software 3D, or centred).
	void set_gains(Voice &v, float buffer_mb, const std::array<float, 3> &at, const de::Transform &listener,
	               const std::array<float, 3> *relative = nullptr) const;
	// DE_CSpeaker::Process (Core 2.123 0x20222500) and the element's Update for one speaker.
	void process(game::World &world, Element &e, const de::Transform &listener, float ms);
	void element_play(game::World &world, Element &e, const de::Transform &listener);
	void element_update(Element &e, const de::Transform &listener, float ms);
	void activate(Element &e, bool on);
	void begin_stop(Element &e);
	float element_progress(const Element &e) const;
	bool element_playing(const Element &e) const;
	static void callback(void *self, Uint8 *stream, int len);
	// Mix (or with no output only advance) one voice.
	static void mix_voice(Voice &v, float *out, int frames);
	void advance(int frames);
public:
	void mix(float *out, int frames);

private:
	const de::Sound *sound(const de::World &data, const de::ObjectRef &r);

	SDL_AudioDeviceID dev_ = 0;
	bool offline_ = false;
	int rate_ = 44100;
	std::mutex lock_;
	std::vector<Voice> voices_;
	std::vector<Element> elements_;  // in the order their affectors joined the scene
	std::set<std::pair<game::GameObject *, game::GameObject *>> watched_;  // (affector, speaker) in the scene
	std::set<const de::Sound *> loopable_;  // noise elements made loopable (+0x30)
	double carry_ = 0;  // update_speakers' own advance
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, std::unique_ptr<de::Sound>> cache_;
	std::unique_ptr<MusicPlayer> music_;
	float sfx_volume_ = 1.0f;  // DE_CSpeaker::GlobalSFXVolume (the registry default)
	bool software_3d = true;  // dsound's software 3D pan, or none (no hardware 3D buffers)
	FILE *record_ = nullptr;  // OPENBG_RECORD_AUDIO: raw f32 stereo of everything mixed
};

}  // namespace audio
