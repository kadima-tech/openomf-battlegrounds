#include "game/music.h"

#include <cstdio>
#include <cstdlib>

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
		if (de::ObjectRef n = reference_at(w_.data(), *s, 2, 1)) sp.active = w_.global_object(n);
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
		c.ret_bool(s && c.world.music->speaker(s).playing);
	});
	reg("CSpeaker::Play()", [](const NativeContext &c) {
		GameObject *s = c.self();
		if (!s) return;
		auto &sp = c.world.music->speaker(s);
		sp.playing = sp.active != nullptr;
		sp.queued = false;
		++sp.serial;
		if (kDebug) fprintf(stderr, "[music %u] play %s (mood %d)\n", c.world.frame(), sp.active ? sp.active->name.c_str() : "-", sp.mood);
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
