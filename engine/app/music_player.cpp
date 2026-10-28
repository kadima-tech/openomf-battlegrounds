#include "music_player.h"

#include <dmusic.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/music.h"

namespace audio {

namespace {

const bool kDebug = getenv("OPENBG_DEBUG_MUSIC") != nullptr;

uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::string lower(std::string s) {
	for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// The RIFF form inside an object's contents (DE_CDirectMusicSegment / DE_CDirectMusicContainer:
// [object chunks][u32 size][RIFF ...]).
std::vector<uint8_t> riff_of(const game::GameObject *o) {
	if (!o || !o->file) return {};
	auto d = o->file->object_data(o->file->objects()[o->index]);
	auto &c = d.contents;
	for (size_t i = 0; i + 12 <= c.size(); ++i) {
		if (memcmp(&c[i], "RIFF", 4) != 0) continue;
		size_t n = size_t(u32(&c[i + 4])) + 8;
		if (i + n > c.size()) continue;
		return {c.begin() + long(i), c.begin() + long(i + n)};
	}
	return {};
}

// A RIFF form's name: its UNFO UNAM (UTF-16) or INFO INAM (ASCII) child.
std::string form_name(const uint8_t *p, size_t n) {
	for (size_t o = 12; o + 8 <= n;) {
		size_t len = u32(p + o + 4);
		if (o + 8 + len > n) break;
		if (!memcmp(p + o, "LIST", 4) && len >= 4 && (!memcmp(p + o + 8, "UNFO", 4) || !memcmp(p + o + 8, "INFO", 4))) {
			bool wide = !memcmp(p + o + 8, "UNFO", 4);
			for (size_t q = o + 12; q + 8 <= o + 8 + len;) {
				size_t l = u32(p + q + 4);
				if (!memcmp(p + q, wide ? "UNAM" : "INAM", 4)) {
					std::string s;
					for (size_t k = 0; k < l; k += wide ? 2 : 1) {
						if (!p[q + 8 + k]) break;
						s += char(p[q + 8 + k]);
					}
					return s;
				}
				q += 8 + l + (l & 1);
			}
		}
		o += 8 + len + (len & 1);
	}
	return {};
}

}  // namespace

MusicPlayer::MusicPlayer(int rate) : rate_(rate) {
	Dm_setLoggerDefault(kDebug ? DmLogLevel_INFO : DmLogLevel_FATAL);
	if (DmLoader_create(&loader_, DmLoader_DOWNLOAD) != DmResult_SUCCESS) return;
	DmLoader_addResolver(loader_, &MusicPlayer::resolve, this);
	if (DmPerformance_create(&perf_, uint32_t(rate)) != DmResult_SUCCESS) {
		perf_ = nullptr;
		return;
	}
	// DE_CDirectXSpeakerElement::Update sets the audiopath's volume to 2000 log10(the speaker's
	// volume) (1/100 dB); the segments' volume is 1 (DE_CNoise contents), so the music plays at
	// 0 dB, the synth's full scale.
	DmPerformance_setVolume(perf_, 1.0f);
}

MusicPlayer::~MusicPlayer() {
	for (auto &[o, s] : segments_) DmSegment_release(s);
	DmPerformance_release(perf_);
	DmLoader_release(loader_);
}

void *MusicPlayer::resolve(void *self, const char *file, size_t *len) {
	auto *p = static_cast<MusicPlayer *>(self);
	std::string key = lower(file);
	if (auto dot = key.rfind('.'); dot != std::string::npos && key.find('@') != 0) key.resize(dot);
	auto it = p->files_.find(key);
	if (it == p->files_.end()) {
		fprintf(stderr, "music: no object %s\n", file);
		return nullptr;
	}
	void *out = malloc(it->second.size());
	memcpy(out, it->second.data(), it->second.size());
	*len = it->second.size();
	return out;
}

void MusicPlayer::add_container(game::World &world, const game::GameObject *container) {
	(void)world;
	if (!container || containers_[container]) return;
	containers_[container] = true;
	auto riff = riff_of(container);
	const uint8_t *p = riff.data();
	size_t n = riff.size();
	// RIFF DMCN: LIST cosl holds a LIST cobl per object: [cobh][RIFF object].
	for (size_t o = 12; o + 12 <= n;) {
		size_t len = u32(p + o + 4);
		if (o + 8 + len > n) break;
		if (!memcmp(p + o, "LIST", 4) && !memcmp(p + o + 8, "cosl", 4)) {
			for (size_t q = o + 12; q + 12 <= o + 8 + len;) {
				size_t l = u32(p + q + 4);
				for (size_t r = q + 12; r + 12 <= q + 8 + l;) {
					size_t m = u32(p + r + 4);
					if (!memcmp(p + r, "RIFF", 4) && r + 8 + m <= n) {
						std::string name = lower(form_name(p + r, m + 8));
						if (!name.empty()) files_[name].assign(p + r, p + r + 8 + m);
						if (kDebug) fprintf(stderr, "music: %s holds %.4s \"%s\"\n", container->name.c_str(), (const char *)p + r + 8, name.c_str());
					}
					r += 8 + m + (m & 1);
				}
				q += 8 + l + (l & 1);
			}
		}
		o += 8 + len + (len & 1);
	}
}

DmSegment *MusicPlayer::segment(game::World &world, const game::GameObject *noise) {
	if (!noise || noise->class_path != "Noise2\\DirectMusicSegment") return nullptr;
	if (auto it = segments_.find(noise); it != segments_.end()) return it->second;
	DmSegment *&s = segments_[noise];
	add_container(world, world.music->container(noise));
	char key[48];
	snprintf(key, sizeof key, "@segment%p", static_cast<const void *>(noise));
	files_[key] = riff_of(noise);
	if (DmLoader_getSegment(loader_, key, &s) != DmResult_SUCCESS) {
		fprintf(stderr, "music: cannot load segment %s\n", noise->name.c_str());
		s = nullptr;
	}
	files_.erase(key);
	return s;
}

void MusicPlayer::update(game::World &world) {
	if (!perf_ || !world.music) return;
	game::GameObject *s = world.music->music_speaker();
	if (!s) return;
	auto &sp = world.music->speaker(s);
	if (sp.serial != serial_ || s != speaker_) {
		serial_ = sp.serial;
		speaker_ = s;
		DmSegment *seg = sp.playing ? segment(world, sp.active) : nullptr;
		uint64_t len = 1;
		if (seg) len = std::max<uint64_t>(1, uint64_t(DmSegment_getLength(seg) * double(DmSegment_getRepeats(seg) + 1) * rate_));
		if (kDebug) fprintf(stderr, "[music %u] %s %s (%.1f s)\n", world.frame(), sp.queued ? "queue" : seg ? "start" : "stop", sp.active && seg ? sp.active->name.c_str() : "-", double(len) / rate_);
		std::lock_guard<std::mutex> g(lock_);
		if (!seg) {
			current_ = next_ = nullptr;
			stop_now_ = true;
		} else if (sp.queued && current_) {
			next_ = seg;
			next_end_ = len;
		} else {
			current_ = seg;
			next_ = nullptr;
			end_ = len;
			pos_ = 0;
			start_now_ = true;
			stop_now_ = false;
			reported_almost_ = false;
		}
		ended_ = false;
		return;
	}
	bool ended, almost;
	{
		std::lock_guard<std::mutex> g(lock_);
		if (switched_) reported_almost_ = false, switched_ = 0;
		ended = ended_;
		ended_ = false;
		almost = current_ && !next_ && !reported_almost_ && end_ - pos_ <= uint64_t(rate_);
		if (almost) reported_almost_ = true;
	}
	if (ended) world.music->segment_ended(s);
	else if (almost) world.music->segment_almost_over(s);
}

void MusicPlayer::mix(float *out, int frames) {
	if (!perf_) return;
	std::lock_guard<std::mutex> g(lock_);
	if (stop_now_ && sounding_) DmPerformance_playSegment(perf_, nullptr, DmTiming_INSTANT);
	if (start_now_) DmPerformance_playSegment(perf_, current_, DmTiming_INSTANT);
	sounding_ = start_now_ || (sounding_ && !stop_now_);
	start_now_ = stop_now_ = false;
	scratch_.resize(size_t(frames) * 2);
	int done = 0;
	while (done < frames) {
		int n = frames - done;
		if (current_) n = int(std::min<uint64_t>(uint64_t(n), end_ - pos_));
		if (n > 0) {
			DmPerformance_renderPcm(perf_, scratch_.data() + done * 2, size_t(n) * 2, DmRenderOptions(DmRender_FLOAT | DmRender_STEREO));
			done += n;
			if (current_) pos_ += uint64_t(n);
		}
		if (current_ && pos_ >= end_) {
			if (next_) {
				current_ = next_;
				next_ = nullptr;
				end_ = next_end_;
				++switched_;
			} else {
				current_ = nullptr;
				ended_ = true;
			}
			pos_ = 0;
			DmPerformance_playSegment(perf_, current_, DmTiming_INSTANT);
			sounding_ = current_ != nullptr;
		}
	}
	for (int i = 0; i < frames * 2; ++i) out[i] += scratch_[size_t(i)];
}

}  // namespace audio
