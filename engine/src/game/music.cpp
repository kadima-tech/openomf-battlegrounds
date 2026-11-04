#include "game/music.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "de/sound.h"
#include "de/value.h"
#include "game/session.h"

namespace game {

namespace {

const bool kDebug = getenv("OPENBG_DEBUG_MUSIC") != nullptr;

// The first reference of an object's references chunk `chunk`, list `list`.
de::ObjectRef reference_at(const de::World &data, const GameObject &o, size_t chunk, size_t list, size_t k = 0) {
	if (!o.file) return {};
	auto lists = reference_lists(data, {o.file, o.index});
	if (chunk >= lists.size() || list >= lists[chunk].size() || k >= lists[chunk][list].size()) return {};
	return lists[chunk][list][k];
}

}  // namespace

// [unique actor property][speaker: name, Volume (advanced reference), loop | v2 + autoplay |
// v3 + a frequency reference it drops | v4 + finish | v5 + track | v6 loop, autoplay, finish,
// track, squeezed 3D mode, frequency].
static void read_speaker_contents(const GameObject &s, MusicSystem::Speaker &sp) {
	if (!s.file) return;
	try {
		de::Contents c(s.file->object_data(s.file->objects()[s.index]).contents);
		c.seek(de::begin_chunk(c).end);
		de::Chunk ch = de::begin_chunk(c);
		const uint32_t name = c.u32();
		c.seek(c.pos() + name);
		if (ch.version < 1 || ch.version > 6) return;
		sp.volume = de::read_embedded_float(c).value;
		sp.loop = c.boolean();
		if (ch.version >= 2) sp.autoplay = c.boolean();
		if (ch.version >= 3 && ch.version <= 5) de::read_embedded_float(c);
		if (ch.version >= 4) c.boolean();
		if (ch.version >= 5) sp.track = c.boolean();
		if (ch.version == 6) {
			c.squeezed();
			sp.frequency = c.f32();
		}
		if (sp.frequency == 0) sp.frequency = 1;
		sp.play_pending = sp.autoplay;
	} catch (const std::exception &) {
	}
}

MusicSystem::Speaker &MusicSystem::speaker(GameObject *s) {
	Speaker &sp = speakers_[s];
	if (!sp.read && s) {
		sp.read = true;
		// UniqueActorProperty\Speaker: references chunk 2, list 1 is its noise.
		if (de::ObjectRef n = reference_at(w_.data(), *s, 2, 1)) sp.active = sp.initial = w_.global_object(n);
		read_speaker_contents(*s, sp);
		own_.push_back(s);
	}
	return sp;
}

const de::Sound &MusicSystem::noise_info(GameObject *noise) {
	auto it = info_.find(noise);
	if (it != info_.end()) return it->second;
	de::Sound s;
	try {
		s = de::read_sound(w_.data(), {noise->file, noise->index});
	} catch (const std::exception &) {
	}
	s.samples = {};
	return info_.emplace(noise, std::move(s)).first->second;
}

void MusicSystem::set_active_noise(GameObject *s, GameObject *noise) {
	Speaker &sp = speaker(s);
	if (!noise || !noise->is("Noise2")) {
		sp.active = sp.to_play = nullptr;
		return;
	}
	if (sp.active != noise) {
		sp.play_pending = sp.autoplay;
		sp.active = noise;
	}
	// DE_CNoise::GetNoiseToPlay (0x2021d4e0): an alternate with the alternate chance, by
	// MATH::Rand(100.0f) and MATH::Rand(count).
	sp.to_play = noise;
	const de::Sound *info = &noise_info(noise);
	if (!info->alternates.empty() && float(crt_rand()) * 100.0f / 32768.0f < info->alternate_chance) {
		int k = crt_rand() * int(info->alternates.size()) / 32768;
		if (GameObject *a = w_.global_object(info->alternates[size_t(k)])) {
			sp.to_play = a;
			info = &noise_info(a);
		}
	}
	// MATH::CenterRand(v) = rand() v / 32768 - v / 2; the product is capped at 1.5.
	float center = float(crt_rand()) * info->frequency_variance / 32768.0f - info->frequency_variance * 0.5f;
	sp.play_frequency = std::min(1.5f, (center + 1.0f) * sp.frequency * info->frequency);
}

GameObject *MusicSystem::music_speaker() {
	GameObject *p = w_.music_player;
	if (!p) return nullptr;
	std::vector<GameObject *> todo = {p};
	for (size_t i = 0; i < todo.size(); ++i) {
		if (todo[i]->is("UniqueActorProperty\\Speaker")) return todo[i];
		todo.insert(todo.end(), todo[i]->children.begin(), todo[i]->children.end());
	}
	for (auto &r : w_.properties_of(*p)) {
		if (r && r.info().class_path == "UniqueActorProperty\\Speaker") return w_.global_object(r);
	}
	return nullptr;
}

GameObject *MusicSystem::container(const GameObject *segment) {
	if (!segment) return nullptr;
	de::ObjectRef r = reference_at(w_.data(), *segment, 2, 0);
	return r ? w_.global_object(r) : nullptr;
}

GameObject *MusicSystem::jump(const GameObject *segment, int mood) {
	if (!segment || mood < 1 || mood > 20) return nullptr;
	de::ObjectRef r = reference_at(w_.data(), *segment, 2, 1, size_t(mood - 1));
	return r ? w_.global_object(r) : nullptr;
}

void MusicSystem::play_default() {
	w_.music_player_actor();
	GameObject *s = music_speaker();
	if (!s) return;
	Speaker &sp = speaker(s);
	sp.active = sp.initial;
	sp.playing = sp.active != nullptr;
	if (kDebug) fprintf(stderr, "[music %u] default music %s\n", w_.frame(), sp.active ? sp.active->name.c_str() : "-");
	sp.queued = false;
	++sp.serial;
}

void MusicSystem::segment_almost_over(GameObject *s) {
	Speaker &sp = speaker(s);
	GameObject *next = jump(sp.active, sp.mood);
	if (kDebug) fprintf(stderr, "[music %u] %s almost over, mood %d -> %s\n", w_.frame(), sp.active ? sp.active->name.c_str() : "-", sp.mood, next ? next->name.c_str() : "-");
	if (!next) return;
	sp.active = next;
	sp.playing = true;
	sp.queued = true;
	++sp.serial;
}

void MusicSystem::segment_ended(GameObject *s) {
	Speaker &sp = speaker(s);
	if (kDebug) fprintf(stderr, "[music %u] %s ended\n", w_.frame(), sp.active ? sp.active->name.c_str() : "-");
	sp.playing = false;
}

void MusicSystem::play(GameObject *s) {
	Speaker &sp = speaker(s);
	sp.playing = sp.active != nullptr;
	sp.queued = false;
	++sp.serial;
	sp.started = w_.scene_ms_of(s);
	sp.wave_ms = 0;
	if (GameObject *n = sp.active; n && n->file && n->is("Noise2\\Stream")) {
		auto it = wave_ms_.find(n);
		if (it == wave_ms_.end()) {
			uint32_t ms = 0;
			try {
				de::Sound snd = de::read_sound(w_.data(), {n->file, n->index});
				ms = uint32_t(float(snd.length_ms) / std::max(0.05f, snd.frequency));
			} catch (const std::exception &) {
			}
			it = wave_ms_.emplace(n, ms).first;
		}
		sp.wave_ms = it->second;
	}
}

float MusicSystem::play_progress(GameObject *s) {
	Speaker &sp = speaker(s);
	if (!sp.playing || !sp.wave_ms) return 0;
	uint32_t t = w_.scene_ms_of(s) - sp.started;
	if (t >= sp.wave_ms) {
		sp.playing = false;  // a one-shot buffer stops at its end
		return 0;
	}
	return float(t) / float(sp.wave_ms);
}

void register_music_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	reg("CObject::class CSpeaker MusicPlayerSpeaker()", [](const NativeContext &c) { c.ret_object(c.world.music->music_speaker()); });
	reg("CSpeaker::class CNoise ActiveNoise()", [](const NativeContext &c) {
		GameObject *s = c.self();
		c.ret_object(s ? c.world.music->speaker(s).active : nullptr);
	});
	// A script's own speaker trades with its element in the app's mixer (Mixer::update_speakers);
	// the music player's speaker plays its segments through the music player.
	auto is_music = [](const NativeContext &c, GameObject *s) { return s == c.world.music->music_speaker(); };
	reg("CSpeaker::SetActiveNoise(class CNoise)", [is_music](const NativeContext &c) {
		GameObject *s = c.self();
		if (!s) return;
		if (is_music(c, s)) c.world.music->speaker(s).active = c.arg_object(0);
		else c.world.music->set_active_noise(s, c.arg_object(0));
	});
	reg("CSpeaker::bool IsPlaying()", [is_music](const NativeContext &c) {
		GameObject *s = c.self();
		if (s && is_music(c, s)) c.world.music->play_progress(s);
		c.ret_bool(s && c.world.music->speaker(s).playing);
	});
	// PlayProgressCaller (Core 2.123 0x20221830): +0xd4.
	reg("CSpeaker::float PlayProgress()", [is_music](const NativeContext &c) {
		GameObject *s = c.self();
		c.ret_f32(!s ? 0.0f : is_music(c, s) ? c.world.music->play_progress(s) : c.world.music->speaker(s).progress);
	});
	// SetVolumeCaller (Core 2.123 0x20221770): the Volume value (+0x9c) takes it.
	reg("CSpeaker::SetVolume(float)", [](const NativeContext &c) {
		if (GameObject *s = c.self()) c.world.music->speaker(s).volume = c.arg<float>(0);
	});
	reg("CSpeaker::Play()", [is_music](const NativeContext &c) {
		GameObject *s = c.self();
		if (!s) return;
		auto &sp = c.world.music->speaker(s);
		if (!is_music(c, s)) {
			sp.play_pending = true;
			return;
		}
		c.world.music->play(s);
		if (kDebug) fprintf(stderr, "[music %u] play %s/%s (mood %d)\n", c.world.frame(), sp.active && sp.active->file ? sp.active->file->master().name.c_str() : "-", sp.active ? sp.active->name.c_str() : "-", sp.mood);
	});
	reg("CSpeaker::Stop()", [is_music](const NativeContext &c) {
		GameObject *s = c.self();
		if (!s) return;
		auto &sp = c.world.music->speaker(s);
		if (!is_music(c, s)) {
			sp.stop_pending = true;
			return;
		}
		if (kDebug && sp.playing) fprintf(stderr, "[music %u] stop\n", c.world.frame());
		sp.playing = false;
		++sp.serial;
	});
	reg("CSpeaker::SetMood(int)", [](const NativeContext &c) {
		GameObject *s = c.self();
		if (!s) return;
		auto &sp = c.world.music->speaker(s);
		if (kDebug && sp.mood != c.arg<int32_t>(0)) fprintf(stderr, "[music %u] mood %d\n", c.world.frame(), c.arg<int32_t>(0));
		sp.mood = c.arg<int32_t>(0);
	});
}

}  // namespace game
