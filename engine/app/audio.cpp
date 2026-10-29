#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

void Mixer::mix(float *out, int frames) {
	std::fill(out, out + frames * 2, 0.0f);
	if (music_) music_->mix(out, frames);
	std::lock_guard<std::mutex> g(lock_);
	for (auto &v : voices_) {
		const auto &s = *v.sound;
		size_t n = s.samples.size() / size_t(std::max(1, s.channels));
		for (int i = 0; i < frames && size_t(v.pos) < n; ++i) {
			size_t k = size_t(v.pos);
			float frac = float(v.pos - double(k));
			auto at = [&](size_t f, int ch) { return f < n ? float(s.samples[f * size_t(s.channels) + size_t(ch)]) / 32768.0f : 0.0f; };
			float l = at(k, 0) * (1 - frac) + at(k + 1, 0) * frac;
			float r = s.channels > 1 ? at(k, 1) * (1 - frac) + at(k + 1, 1) * frac : l;
			out[i * 2] += l * v.left;
			out[i * 2 + 1] += r * v.right;
			v.pos += v.step;
		}
	}
	voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
	                             [](const Voice &v) { return size_t(v.pos) >= v.sound->samples.size() / size_t(std::max(1, v.sound->channels)); }),
	              voices_.end());
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
	std::array<float, 3> d{v.at[0] - listener.t[0], v.at[1] - listener.t[1], v.at[2] - listener.t[2]};
	float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	// Far speakers update less often: the falloff over the distance, times the rate
	// (min(1, length / 7000 ms + 0.1)), accumulates per millisecond until it passes 200.
	v.due += v.falloff == 0 ? 10000.0f : v.falloff / (dist + 1) * v.rate * ms;
	if (v.due <= 200) return;
	v.due = 0;
	float vol = noise_volume(dist, v.hotspot, v.falloff, sfx_volume_, v.volume);
	// The head-relative position DirectSound3D is given: the offset in the listener's frame,
	// x negated (DirectSound is left-handed: +x right, +z ahead).
	auto local = listener.inverse_rotate(d);
	std::array<float, 3> ds{-local[0], local[1], local[2]};
	// IDirectSoundBuffer::SetVolume(10000 log10(1 - (1 - volume x speaker volume)^2)) in 1/100 dB;
	// at 0 the call is out of range and the old volume stays.
	float shaped = 1 - vol * v.volume;
	shaped = 1 - shaped * shaped;
	if (shaped <= 0) return;
	const float buffer_mb = 10000.0f * std::log10(shaped);
	auto gain = [](float mb) { return std::pow(10.0f, std::max(mb, -10000.0f) / 2000.0f); };
	if (!software_3d) {
		// No hardware 3D buffers at start-up: the buffers have no 3D control, so every sound is
		// centred at its buffer volume.
		v.left = v.right = gain(buffer_mb);
		return;
	}
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

void Mixer::play_requests(game::World &world, const de::Transform &listener) {
	sfx_volume_ = world.sfx_volume;
	// Preferences\Audio\3D Mode (ours): 1 dsound's software 3D (default), 0 no 3D (centred).
	software_3d = world.registry_string("Preferences\\Audio\\3D Mode") != "0";
	auto requests = std::move(world.sound_requests);
	world.sound_requests.clear();
	if ((!dev_ && !offline_) || getenv("OPENBG_NO_SOUND")) return;
	std::uniform_real_distribution<float> unit(0.0f, 1.0f);
	{
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
		if (!s->alternates.empty() && unit(rng_) * 100.0f < s->alternate_chance) {
			auto &alt = s->alternates[size_t(unit(rng_) * float(s->alternates.size())) % s->alternates.size()];
			if (const de::Sound *a = sound(world.data(), alt)) s = a;
		}
		Voice v;
		v.sound = s;
		v.volume = r.volume * s->volume;
		if (r.positional) {
			v.at = r.pos;
			v.hotspot = r.hotspot;
			v.falloff = r.falloff;
		} else {
			// DE_CScene::PlayNoiseAmbient: a sphere at the origin with a 1e7 hotspot and falloff.
			v.hotspot = v.falloff = 1e7f;
		}
		// DE_CDirectXSpeakerElement::Play: a speaker at or beyond its falloff is not started,
		// nor one of no volume.
		std::array<float, 3> d{v.at[0] - listener.t[0], v.at[1] - listener.t[1], v.at[2] - listener.t[2]};
		if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) >= v.falloff || sfx_volume_ * v.volume == 0) continue;
		v.rate = std::min(1.0f, float(s->length_ms) / 7000.0f + 0.1f);
		// NewNoisePlayerSphere: frequency x noise frequency x (1 + CenterRand(variance)).
		float freq = r.frequency * s->frequency * (1 + (unit(rng_) * 2 - 1) * s->frequency_variance);
		v.step = double(s->rate) * double(std::max(0.05f, freq)) / double(rate_);
		v.left = v.right = 0;
		update(v, listener, 0);
		started.push_back(v);
	}
	std::lock_guard<std::mutex> g(lock_);
	voices_.insert(voices_.end(), started.begin(), started.end());
}

}  // namespace audio
