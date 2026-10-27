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
	};
	explicit MusicSystem(World &w) : w_(w) {}
	Speaker &speaker(GameObject *s);
	// MusicPlayerSpeaker(): the music player actor's speaker.
	GameObject *music_speaker();
	// The DirectMusicContainer holding the style and instruments of `segment`.
	GameObject *container(const GameObject *segment);
	// The jump of `segment` for `mood` (null when it has none).
	GameObject *jump(const GameObject *segment, int mood);
	// The player's report that the playing segment of `s` is almost over: chain to its jump.
	void segment_almost_over(GameObject *s);
	// ...and that it ended with nothing queued.
	void segment_ended(GameObject *s);

private:
	World &w_;
	std::map<const GameObject *, Speaker> speakers_;
};

void register_music_natives(World &w);

}  // namespace game
