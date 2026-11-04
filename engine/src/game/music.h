// Music: the speaker the BMusicPlayer behaviour drives (Music.DEObjects "MusicPlayer") and the
// mood-driven chain of DirectMusic segments.
//
// CSpeaker (Core DE_CSpeaker; SetMood 0x70218b50 stores the mood at +0xdc) plays its active
// noise. A Noise2\DirectMusicSegment's references chunk 2 holds its container (list 0) and its
// jumps (list 1: one segment per mood, the song's columns A..J). When DirectMusic reports that a
// playing segment is almost over (DMUS_NOTIFICATION_SEGALMOSTEND,
// DE_CDirectXAudioPlayer::ProcessPerformanceMessages, AudioPlayer), the segment's jump for the
// speaker's mood (1..20) becomes the speaker's active noise and plays next; without one the
// speaker stops and the behaviour starts its queued noise again.
//
// The game side keeps the state; the app's music player renders the segments and reports their
// ends (without one, a playing segment simply never ends).
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "de/sound.h"
#include "game/runtime.h"

namespace game {

class MusicSystem {
public:
	struct Speaker {
		GameObject *active = nullptr;  // the noise it plays (CSpeaker::ActiveNoise)
		bool playing = false;
		int mood = 0;
		uint32_t serial = 0;  // bumped by every Play: the player restarts on a change
		bool queued = false;  // the last Play chained from an ending segment: start at its end
		bool read = false;
		GameObject *initial = nullptr;  // its noise in the data (DE_GLOBAL's default music)
		float volume = 1;               // its Volume value (+0x9c), which SetVolume sets
		uint32_t started = 0;           // scene ms of the last Play
		uint32_t wave_ms = 0;           // a wave noise's playing time; 0 for none or a segment
		// Any other speaker: its contents (DE_CSpeaker::
		// ObjProcessContents, Core 2.123 0x20221d70) and the state DE_CSpeaker::Process
		// (0x20222500) trades with its speaker element every audio update.
		bool loop = false, autoplay = false, finish = false, track = false;  // +0xb8, +0xbc, +0xc0, +0xc4
		uint32_t mode = 0;                                                   // +0x98: 1 head-relative
		float frequency = 1;                                 // +0xb0
		GameObject *to_play = nullptr;                       // +0x94: GetNoiseToPlay's pick
		float play_frequency = 1;                            // +0xb4
		bool play_pending = false, stop_pending = false;     // +0xcc, +0xd8
		float progress = 0;                                  // +0xd4 (playing is +0xd0)
	};
	explicit MusicSystem(World &w) : w_(w) {}
	Speaker &speaker(GameObject *s);
	// MusicPlayerSpeaker(): the music player actor's speaker.
	GameObject *music_speaker();
	// The DirectMusicContainer holding the style and instruments of `segment`.
	GameObject *container(const GameObject *segment);
	// The jump of `segment` for `mood` (null when it has none).
	GameObject *jump(const GameObject *segment, int mood);
	// DE_GLOBAL::PlayDefaultMusic (Core 0x70013d70): the music speaker plays its default noise.
	void play_default();
	// The player's report that the playing segment of `s` is almost over: chain to its jump.
	void segment_almost_over(GameObject *s);
	// ...and that it ended with nothing queued.
	void segment_ended(GameObject *s);
	// Play, timing a wave noise's buffer.
	void play(GameObject *s);
	// DE_CSpeaker::Process's +0xd4: the wave's play position over its length, 0 when none plays.
	float play_progress(GameObject *s);
	// DE_CSpeaker::SetActiveNoise (Core 2.123 0x202226a0); `loading`: as ObjProcessReferences
	// calls it.
	void set_active_noise(GameObject *s, GameObject *noise, bool loading = false);

private:
	World &w_;
	std::map<const GameObject *, uint32_t> wave_ms_;
	std::map<const GameObject *, Speaker> speakers_;
	uint32_t load_rand_ = 1;
	int draw(bool loading);
	std::map<const GameObject *, de::Sound> info_;  // noises' contents and alternates, no samples
	const de::Sound &noise_info(GameObject *noise);
};

void register_music_natives(World &w);

}  // namespace game
