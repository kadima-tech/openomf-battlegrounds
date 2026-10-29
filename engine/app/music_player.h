// Music output: renders the music speaker's DirectMusic segments with GothicKit dmusic
// (third_party/dmusic) and reports their ends back to the game's MusicSystem.
//
// A segment's style and band references name their objects by file ("A-Silent.sty", "!hexed
// DLS.dls"); in OMFBG those objects live in the segment's DirectMusicContainer, so the loader
// resolves a name against the container's objects (UNAM, or a DLS collection's INFO INAM).
//
// Timing stands in for DirectMusic's notifications: one second before the playing segment
// (with its repeats) ends the game gets DMUS_NOTIFICATION_SEGALMOSTEND (inferred: the
// performance's default prepare time); a segment it chains to then starts exactly at the end,
// and a segment with nothing queued ends there.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "game/runtime.h"

struct DmLoader;
struct DmPerformance;
struct DmSegment;
struct DmBand;

namespace audio {

class MusicPlayer {
public:
	explicit MusicPlayer(int rate);
	~MusicPlayer();
	bool ok() const { return perf_ != nullptr; }
	// Main thread, once per frame: follow the music speaker and report segment ends.
	void update(game::World &world);
	void set_volume(float global);
	// Audio thread: add the music to `frames` interleaved stereo samples.
	void mix(float *out, int frames);

private:
	float volume_ = -1;
	DmSegment *segment(game::World &world, const game::GameObject *noise);
	void add_container(game::World &world, const game::GameObject *container);
	static void *resolve(void *self, const char *file, size_t *len);

	int rate_;
	DmLoader *loader_ = nullptr;
	DmPerformance *perf_ = nullptr;
	std::map<std::string, std::vector<uint8_t>> files_;  // lower-case name without extension
	std::map<const game::GameObject *, DmSegment *> segments_;
	std::map<const game::GameObject *, bool> containers_;
	// AudioPlayer's band (DE_CDirectXNoiseElement::CheckForReload 0x100055d0): a segment's own band
	// track, else its container's first band (GetDefaultBandInterface), played before the segment
	// when it differs from the last one played (Activate 0x10004460). Null: the segment has its own.
	std::map<const game::GameObject *, std::string> container_band_;
	std::map<std::string, DmBand *> bands_;
	std::map<DmSegment *, DmBand *> segment_band_;
	DmBand *last_band_ = nullptr;
	void play(DmSegment *seg);

	// Main thread state.
	uint32_t serial_ = 0;
	bool reported_almost_ = false;
	const game::GameObject *speaker_ = nullptr;

	// Shared with the audio thread.
	std::mutex lock_;
	DmSegment *current_ = nullptr, *next_ = nullptr;
	bool start_now_ = false, stop_now_ = false, ended_ = false;
	bool sounding_ = false;  // the performance has a segment
	uint64_t pos_ = 0, end_ = 0, next_end_ = 0;  // stereo frames
	uint64_t switched_ = 0;                       // bumped when a queued segment starts
	std::vector<float> scratch_;
};

}  // namespace audio
