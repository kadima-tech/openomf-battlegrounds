#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/music.h"
#include "game/session.h"

namespace audio {

bool Mixer::open() {
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
	SDL_AudioSpec want{}, have{};
	want.freq = 44100;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = &Mixer::callback;
	want.userdata = this;
	dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (!dev_) return false;
	rate_ = have.freq;
	if (const char *rec = getenv("OPENBG_RECORD_AUDIO")) record_ = fopen(rec, "wb");
	if (!getenv("OPENBG_NO_MUSIC")) {
		music_ = std::make_unique<MusicPlayer>(rate_);
		if (!music_->ok()) music_.reset();
	}
	SDL_PauseAudioDevice(dev_, 0);
	return true;
}

void Mixer::open_offline(int rate) {
	rate_ = rate;
	offline_ = true;
	if (const char *rec = getenv("OPENBG_RECORD_AUDIO")) record_ = fopen(rec, "wb");
	if (!getenv("OPENBG_NO_MUSIC")) {
		music_ = std::make_unique<MusicPlayer>(rate_);
		if (!music_->ok()) music_.reset();
	}
}

void Mixer::pump(int frames) {
	std::vector<float> buf(size_t(frames) * 2);
	mix(buf.data(), frames);
}

Mixer::~Mixer() {
	if (dev_) SDL_CloseAudioDevice(dev_);  // before the music player goes
	if (record_) fclose(record_);
}

void Mixer::callback(void *self, Uint8 *stream, int len) {
	static_cast<Mixer *>(self)->mix(reinterpret_cast<float *>(stream), len / int(sizeof(float) * 2));
}

void Mixer::mix_voice(Voice &v, float *out, int frames) {
	if (!v.playing || !v.sound) return;
	const auto &s = *v.sound;
	const size_t n = s.samples.size() / size_t(std::max(1, s.channels));
	const uint64_t end = uint64_t(n) << kFracBits;
	if (!out || v.silent) {
		v.pos += v.step * uint64_t(frames);
		if (v.loop && end) v.pos %= end;
	} else {
		for (int i = 0; i < frames && v.pos < end; ++i) {
			size_t k = size_t(v.pos >> kFracBits);
			float frac = float(v.pos & ((uint64_t(1) << kFracBits) - 1)) / 4294967296.0f;
			auto at = [&](size_t f, int ch) {
				if (f >= n) {
					if (!v.loop) return 0.0f;
					f %= n;
				}
				return float(s.samples[f * size_t(s.channels) + size_t(ch)]) / 32768.0f;
			};
			// The same 4-point Hermite as the music's conversion (dmusic Synth.c, FINDINGS 154).
			auto hermite = [&](int ch) {
				float y0 = k ? at(k - 1, ch) : 0.0f, y1 = at(k, ch), y2 = at(k + 1, ch), y3 = at(k + 2, ch);
				float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
				return ((c3 * frac + c2) * frac + c1) * frac + y1;
			};
			float l = hermite(0);
			float r = s.channels > 1 ? hermite(1) : l;
			out[i * 2] += l * v.left;
			out[i * 2 + 1] += r * v.right;
			v.pos += v.step;
			if (v.loop && v.pos >= end) v.pos -= end;
		}
	}
	if (!v.loop && v.pos >= end) v.playing = false;
}

void Mixer::advance(int frames) {
	for (auto &v : voices_) mix_voice(v, nullptr, frames);
	for (auto &e : elements_) {
		if (e.buffer) mix_voice(e.voice, nullptr, frames);
	}
	voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice &v) { return !v.playing; }), voices_.end());
}

void Mixer::mix(float *out, int frames) {
	std::fill(out, out + frames * 2, 0.0f);
	if (music_) music_->mix(out, frames);
	std::lock_guard<std::mutex> g(lock_);
	for (auto &v : voices_) mix_voice(v, out, frames);
	for (auto &e : elements_) {
		if (e.buffer) mix_voice(e.voice, out, frames);
	}
	voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice &v) { return !v.playing; }), voices_.end());
	for (int i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
	if (record_) fwrite(out, sizeof(float), size_t(frames) * 2, record_);
}

const de::Sound *Mixer::sound(const de::World &data, const de::ObjectRef &r) {
	auto key = std::make_pair(r.file, r.index);
	auto it = cache_.find(key);
	if (it != cache_.end()) return it->second.get();
	std::unique_ptr<de::Sound> s;
	try {
		s = std::make_unique<de::Sound>(de::read_sound(data, r));
	} catch (const std::exception &e) {
		fprintf(stderr, "sound %s: %s\n", r.info().name.c_str(), e.what());
	}
	return (cache_[key] = std::move(s)).get();
}

// The speaker element's volume for a sphere speaker (DE_CSpeakerElement::GetNoiseVolume, the
// sphere's DE_CSphereAffector::IsTouching intensity): 1 within the hotspot, falling linearly to 0
// at the falloff, times the global SFX volume and the speaker's, at most 1.
static float noise_volume(float dist, float hotspot, float falloff, float global, float speaker) {
	float intensity = 0;
	if (dist <= falloff) intensity = dist < hotspot || falloff - hotspot <= 1e-5f ? 1.0f : 1.0f - (dist - hotspot) / (falloff - hotspot);
	return std::min(1.0f, global * speaker * intensity);
}

void Mixer::update(Voice &v, const de::Transform &listener, float ms) {
	if (v.on_camera) {
		auto o = listener.rotate({0, 0, -10000});
		v.at = {listener.t[0] + o[0], listener.t[1] + o[1], listener.t[2] + o[2]};
	}
	std::array<float, 3> d{v.at[0] - listener.t[0], v.at[1] - listener.t[1], v.at[2] - listener.t[2]};
	float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	// Far speakers update less often: the falloff over the distance, times the rate
	// (min(1, length / 7000 ms + 0.1)), accumulates per millisecond until it passes 200.
	v.due += v.falloff == 0 ? 10000.0f : v.falloff / (dist + 1) * v.rate * ms;
	if (v.due <= 200) return;
	v.due = 0;
	float vol = noise_volume(dist, v.hotspot, v.falloff, sfx_volume_, v.volume);
	// IDirectSoundBuffer::SetVolume(10000 log10(1 - (1 - volume x speaker volume)^2)) in 1/100 dB;
	// at 0 the call is out of range and the old volume stays.
	float shaped = 1 - vol * v.volume;
	shaped = 1 - shaped * shaped;
	if (shaped <= 0) return;
	set_gains(v, 10000.0f * std::log10(shaped), v.at, listener);
}

void Mixer::set_gains(Voice &v, float buffer_mb, const std::array<float, 3> &at, const de::Transform &listener) const {
	if (buffer_mb < -10000) return;  // DSERR_INVALIDPARAM: the buffer keeps its volume
	auto gain = [](float mb) { return std::pow(10.0f, std::max(mb, -10000.0f) / 2000.0f); };
	if (!software_3d) {
		// No hardware 3D buffers at start-up: the buffers have no 3D control, so every sound is
		// centred at its buffer volume.
		v.left = v.right = gain(buffer_mb);
		return;
	}
	// The head-relative position DirectSound3D is given: the offset in the listener's frame,
	// x negated (DirectSound is left-handed: +x right, +z ahead).
	auto local = listener.inverse_rotate({at[0] - listener.t[0], at[1] - listener.t[1], at[2] - listener.t[2]});
	std::array<float, 3> ds{-local[0], local[1], local[2]};
	// dsound.dll's WDM software 3D (no virtualisation, FINDINGS 149): a fixed -500 mB (the distance
	// gain is 1 with min = max = 9999999), then the tangent law between speakers at -90 / +90
	// degrees on x (right) and z (ahead), with a blend toward the far side within 2.5 units;
	// each channel gets 1000 log10 of its power share, truncated.
	const float X = ds[0], Z = ds[2], rh = std::sqrt(X * X + Z * Z);
	float pr = 0.5f;
	if (std::fabs(X) >= 1e-5f || std::fabs(Z) >= 1e-5f) {
		pr = std::fabs(Z) < 1e-5f ? (X > 0 ? 1.0f : 0.0f) : std::fabs(Z) / (std::fabs(Z) + rh - X);
		if (std::fabs(X) < 2.5f && std::fabs(Z) < 2.5f && rh < 2.5f) {
			float w = rh / 5 + 0.5f;
			pr = w * pr + (1 - w) * (1 - pr);
		}
	}
	auto level = [](float p) { return p <= 0 ? -10000.0f : std::trunc(1000.0f * std::log10(p)); };
	v.left = gain(buffer_mb - 500 + level(1 - pr));
	v.right = gain(buffer_mb - 500 + level(pr));
}

// IDirectSoundBuffer::SetFrequency takes whole hertz (the original's __ftol truncates).
static uint64_t step_for(const de::Sound &s, float frequency, int device_rate) {
	uint64_t hz = uint64_t(std::max(0.0f, float(s.rate) * frequency));
	return (hz << 32) / uint64_t(device_rate);
}

void Mixer::activate(Element &e, bool on) {
	e.active = on;
	if (!on) {
		e.buffer = false;  // stopped and released
		return;
	}
	// DE_CDirectXSpeakerElement::Activate: the buffer starts at -10000 mB.
	if (e.buffer) {
		e.voice.playing = true;
		e.voice.left = e.voice.right = 0;
	}
}

bool Mixer::element_playing(const Element &e) const {
	// DE_CDirectXSpeakerElement::IsAudible: a looping noise element counts as playing.
	if (!e.noise) return false;
	if (loopable_.count(e.noise)) return true;
	return e.buffer && e.voice.playing;
}

float Mixer::element_progress(const Element &e) const {
	// The buffer's write cursor (our mixed position) over the file's bytes - 44; a stopped
	// buffer's cursor is back at 0.
	if (!e.buffer || !e.voice.playing || !e.noise || e.noise->file_bytes <= 44) return 0;
	uint64_t bytes = (e.voice.pos >> kFracBits) * uint64_t(e.noise->block_align);
	return float(bytes) / float(e.noise->file_bytes - 44);
}

void Mixer::element_play(game::World &world, Element &e, const de::Transform &listener) {
	if (!e.noise) return;
	if (e.loop) loopable_.insert(e.noise);  // DE_CDirectXNoiseElement::AssureLoopable
	const bool loopable = loopable_.count(e.noise) != 0;
	// A wave's Play releases the old buffer and duplicates a fresh one (CreateNewAudiopath), at
	// the noise's rate times DE_CSpeaker::Frequency (+0xb4).
	auto &sp = world.music->speaker(e.speaker);
	static const bool muted = getenv("OPENBG_NO_SOUND") != nullptr;
	e.active = false;
	e.buffer = true;
	e.voice = Voice{e.noise};
	e.voice.playing = false;
	e.voice.loop = loopable;
	e.voice.silent = muted;
	e.voice.left = e.voice.right = 0;
	e.voice.step = step_for(*e.noise, sp.active ? sp.play_frequency : 1.0f, rate_);
	if (loopable) e.by_volume = true;
	if (!e.by_volume) {
		// Not started at or beyond the element's falloff (the player then drops it), nor with
		// no SpeakerVolume.
		std::array<float, 3> d{e.at[0] - listener.t[0], e.at[1] - listener.t[1], e.at[2] - listener.t[2]};
		if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) >= e.falloff) {
			e.buffer = false;
			return;
		}
		if (sfx_volume_ * e.volume != 0 && !e.active) activate(e, true);
	}
	e.stopping = false;
}

void Mixer::element_update(Element &e, const de::Transform &listener, float ms) {
	const game::GameObject &affector = *e.speaker->parent;
	std::array<float, 3> d{listener.t[0] - e.at[0], listener.t[1] - e.at[1], listener.t[2] - e.at[2]};
	const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], dist = std::sqrt(dist2);
	// DE_CSpeakerElement::GetNoiseVolume: the affector's DE_CSphereAffector::IsTouching intensity
	// at the listener times the global SFX volume and the element's volume, at most 1.
	float intensity = 1;
	if (affector.shape == game::GameObject::Shape::Sphere) {
		if (dist2 > affector.falloff * affector.falloff) {
			intensity = 0;
		} else if (dist2 >= affector.hotspot * affector.hotspot && affector.falloff > affector.hotspot) {
			intensity = 1 - (dist - affector.hotspot) / (affector.falloff - affector.hotspot);
		}
	}
	const float v = std::min(1.0f, sfx_volume_ * e.volume * intensity);
	if (!e.stopping) {
		e.due += e.falloff == 0 ? 10000.0f : e.falloff / (dist + 1) * e.rate * ms;
		if (e.due <= 200) return;
		e.due = 0;
	}
	float shaped = 1 - v * e.volume;
	shaped = 1 - shaped * shaped;
	if (e.by_volume) {
		// Looping noises start once their volume passes 0.09 and stop below 0.03.
		if (!e.active && shaped > 0.09f) {
			e.buffer = true;
			e.voice.pos = 0;
			activate(e, true);
		} else if (e.active && shaped < 0.03f) {
			activate(e, false);
		}
	}
	if (!e.active || !e.buffer) return;
	if (!e.stopping) {
		if (shaped > 0) set_gains(e.voice, 10000.0f * std::log10(shaped), e.at, listener);
		return;
	}
	// BeginStopTransition: a wave fades out over 1500 ms of audio updates, then stops.
	if (e.stop_ms < 0) {
		activate(e, false);
		return;
	}
	float f = float(e.stop_ms) / 1500.0f * shaped;
	if (f > 0) set_gains(e.voice, 10000.0f * std::log10(f), e.at, listener);
	e.stop_ms -= int(ms);
}

void Mixer::process(game::World &world, Element &e, const de::Transform &listener, float ms) {
	auto &sp = world.music->speaker(e.speaker);
	e.at = e.speaker->parent->world().t;  // DE_CStandardSpeakerElement::Update: the affector's
	auto begin_stop = [&] {
		if (!e.stopping) e.stopping = true, e.stop_ms = 1500;
	};
	if (e.noise_object != sp.to_play) {
		e.noise_object = sp.to_play;
		e.noise = sp.to_play && sp.to_play->file ? sound(world.data(), {sp.to_play->file, sp.to_play->index}) : nullptr;
		if (!e.noise) begin_stop();
		else if (!e.loop) e.rate = std::min(1.0f, float(e.noise->length_ms) / 7000.0f + 0.1f);
	}
	const de::Sound *active = sp.active && sp.active->file ? sound(world.data(), {sp.active->file, sp.active->index}) : nullptr;
	e.volume = active ? std::min(1.0f, sp.volume * active->volume) : 1.0f;
	if (sp.loop) e.loop = true;
	if (!sp.active) {
		begin_stop();
		element_update(e, listener, ms);
		return;
	}
	if (sp.track) {
		sp.playing = element_playing(e);
		sp.progress = element_progress(e);
		if (sp.stop_pending) {
			begin_stop();
			sp.stop_pending = false;
		}
	}
	if (sp.play_pending) {
		sp.play_pending = false;
		element_play(world, e, listener);
		sp.playing = true;
		element_update(e, listener, ms);  // Play's own Update
	}
	element_update(e, listener, ms);
}

void Mixer::update_speakers(game::World &world, const de::Transform &listener) {
	sfx_volume_ = world.sfx_volume;
	software_3d = world.registry_string("Preferences\\Audio\\3D Mode") != "0";
	const float ms = float(world.frame_ms());
	game::GameObject *music = world.music->music_speaker();
	// Load the waves before the mixing thread waits on the lock.
	for (game::GameObject *s : world.music->own_speakers()) {
		if (s == music) continue;
		auto &sp = world.music->speaker(s);
		for (game::GameObject *n : {sp.active, sp.to_play}) {
			if (n && n->file) sound(world.data(), {n->file, n->index});
		}
	}
	std::lock_guard<std::mutex> g(lock_);
	for (game::GameObject *s : world.music->own_speakers()) {
		if (s == music) continue;
		auto it = std::find_if(elements_.begin(), elements_.end(), [&](const Element &e) { return e.speaker == s; });
		// An element lives while its speaker's affector is in an audible scene.
		game::GameObject *a = s->parent;
		if (s->destroyed_seq || !a || !a->active || !game::in_scene(*a) || !game::scene_audible(world, s)) {
			if (it != elements_.end()) elements_.erase(it);
			continue;
		}
		if (it == elements_.end()) {
			Element e;
			e.speaker = s;
			if (a->shape == game::GameObject::Shape::Sphere) e.hotspot = a->hotspot, e.falloff = a->falloff;
			elements_.push_back(e);
			it = elements_.end() - 1;
		}
		process(world, *it, listener, ms);
	}
	if (!dev_ && !offline_) {
		carry_ += double(rate_) * world.frame_ms() / 1000.0;
		int n = int(carry_);
		carry_ -= n;
		advance(n);
	}
	static const bool debug = getenv("OPENBG_DEBUG_SOUND") != nullptr;
	if (!debug) return;
	// Each voice's start and end, and its gains every 50 frames.
	for (auto &e : elements_) {
		auto &sp = world.music->speaker(e.speaker);
		const bool heard = e.buffer && e.voice.playing;
		auto db = [](float g) { return g > 0 ? 20 * std::log10(g) : -999.0f; };
		if (heard != e.heard || (heard && world.frame() % 50 == 0)) {
			fprintf(stderr, "[speaker %u] %s %s %s: volume %.2f, frequency %.3f, progress %.3f, gains %.1f / %.1f dB%s\n", world.frame(),
			        e.speaker->parent->name.c_str(), heard == e.heard ? "plays" : heard ? "starts" : "ends",
			        e.noise_object ? e.noise_object->name.c_str() : "-", e.volume, sp.play_frequency, sp.progress, db(e.voice.left),
			        db(e.voice.right), e.stopping ? " (stopping)" : "");
		}
		e.heard = heard;
	}
}

void Mixer::play_requests(game::World &world, const de::Transform &listener) {
	update_speakers(world, listener);
	auto requests = std::move(world.sound_requests);
	world.sound_requests.clear();
	// Noises are chosen even when nothing is heard: the choice draws on the shared rand() stream.
	static const bool muted = getenv("OPENBG_NO_SOUND") != nullptr;
	const bool audible = (dev_ || offline_) && !muted;
	if (audible) {
		std::lock_guard<std::mutex> g(lock_);
		for (auto &v : voices_) update(v, listener, float(world.frame_ms()));
	}
	std::vector<Voice> started;
	for (auto &r : requests) {
		if (!r.noise || !r.noise->file) continue;
		de::ObjectRef ref{r.noise->file, r.noise->index};
		const de::Sound *s = sound(world.data(), ref);
		if (!s) continue;
		// DE_CNoise::GetNoiseToPlay: an alternate, with the alternate chance (percent).
		if (!s->alternates.empty() && game::random01(world) * 100.0f < s->alternate_chance) {
			auto &alt = s->alternates[size_t(game::random01(world) * float(s->alternates.size())) % s->alternates.size()];
			if (const de::Sound *a = sound(world.data(), alt)) s = a;
		}
		Voice v{s};
		v.volume = r.volume * s->volume;
		if (r.positional) {
			v.at = r.pos;
			v.hotspot = r.hotspot;
			v.falloff = r.falloff;
		} else {
			// DE_CScene::PlayNoiseAmbient (Core 0x20076b10): a 1e7 sphere at (0, 0, -10000) under
			// the renderer's camera, so it follows the listener and stays centred.
			v.on_camera = true;
			v.hotspot = v.falloff = 1e7f;
		}
		// DE_CDirectXSpeakerElement::Play: a speaker at or beyond its falloff is not started,
		// nor one of no volume.
		std::array<float, 3> d{v.at[0] - listener.t[0], v.at[1] - listener.t[1], v.at[2] - listener.t[2]};
		if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) >= v.falloff || sfx_volume_ * v.volume == 0) continue;
		v.rate = std::min(1.0f, float(s->length_ms) / 7000.0f + 0.1f);
		// DE_CSpeaker::SetActiveNoise (Core 0x202226a0): (1 + CenterRand(variance)) x speaker
		// frequency x noise frequency, at most 1.5; CenterRand(v) = rand() v / 32768 - v / 2.
		float center = float(game::crt_rand()) * s->frequency_variance / 32768.0f - s->frequency_variance * 0.5f;
		float freq = std::min(1.5f, (center + 1.0f) * r.frequency * s->frequency);
		if (!audible) continue;
		v.step = step_for(*s, std::max(0.05f, freq), rate_);
		v.left = v.right = 0;
		update(v, listener, 0);
		started.push_back(v);
	}
	std::lock_guard<std::mutex> g(lock_);
	voices_.insert(voices_.end(), started.begin(), started.end());
}

}  // namespace audio
