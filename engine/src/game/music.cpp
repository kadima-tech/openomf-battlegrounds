#include "game/music.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "de/sound.h"
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

MusicSystem::Speaker &MusicSystem::speaker(GameObject *s) {
	Speaker &sp = speakers_[s];
	if (!sp.read && s) {
		sp.read = true;
		// UniqueActorProperty\Speaker: references chunk 2, list 1 is its noise.
		if (de::ObjectRef n = reference_at(w_.data(), *s, 2, 1)) sp.active = sp.initial = w_.global_object(n);
	}
	return sp;
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
	reg("CSpeaker::SetActiveNoise(class CNoise)", [](const NativeContext &c) {
		if (GameObject *s = c.self()) c.world.music->speaker(s).active = c.arg_object(0);
	});
	reg("CSpeaker::bool IsPlaying()", [](const NativeContext &c) {
		GameObject *s = c.self();
		if (s) c.world.music->play_progress(s);
		c.ret_bool(s && c.world.music->speaker(s).playing);
	});
	// PlayProgressCaller (Core 2.123 0x20221830): +0xd4.
	reg("CSpeaker::float PlayProgress()", [](const NativeContext &c) {
		GameObject *s = c.self();
		c.ret_f32(s ? c.world.music->play_progress(s) : 0.0f);
	});
	// SetVolumeCaller (Core 2.123 0x20221770): the Volume value (+0x9c) takes it.
	reg("CSpeaker::SetVolume(float)", [](const NativeContext &c) {
		if (GameObject *s = c.self()) c.world.music->speaker(s).volume = c.arg<float>(0);
	});
	reg("CSpeaker::Play()", [](const NativeContext &c) {
		GameObject *s = c.self();
		if (!s) return;
		c.world.music->play(s);
		auto &sp = c.world.music->speaker(s);
		if (kDebug) fprintf(stderr, "[music %u] play %s/%s (mood %d)\n", c.world.frame(), sp.active && sp.active->file ? sp.active->file->master().name.c_str() : "-", sp.active ? sp.active->name.c_str() : "-", sp.mood);
	});
	reg("CSpeaker::Stop()", [](const NativeContext &c) {
		GameObject *s = c.self();
		if (!s) return;
		auto &sp = c.world.music->speaker(s);
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
