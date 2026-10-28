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

void Mixer::play_requests(game::World &world, const de::Transform &listener) {
	auto requests = std::move(world.sound_requests);
	world.sound_requests.clear();
	if ((!dev_ && !offline_) || getenv("OPENBG_NO_SOUND")) return;
	std::uniform_real_distribution<float> unit(0.0f, 1.0f);
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
		// DE_CSpeakerElement::GetNoiseVolume: full within the hotspot, linear to 0 at the falloff.
		float gain = r.volume * s->volume, pan = 0;
		if (r.positional) {
			std::array<float, 3> d{r.pos[0] - listener.t[0], r.pos[1] - listener.t[1], r.pos[2] - listener.t[2]};
			float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (r.falloff > 0 && dist > r.falloff) continue;
			if (dist > r.hotspot && r.falloff > r.hotspot) gain *= (r.falloff - dist) / (r.falloff - r.hotspot);
			auto local = listener.inverse_rotate(d);
			if (dist > 1e-3f) pan = std::clamp(local[0] / dist, -1.0f, 1.0f);
		}
		if (gain <= 0.001f) continue;
		// NewNoisePlayerSphere: frequency x noise frequency x (1 + CenterRand(variance)).
		float freq = r.frequency * s->frequency * (1 + (unit(rng_) * 2 - 1) * s->frequency_variance);
		Voice v;
		v.sound = s;
		v.step = double(s->rate) * double(std::max(0.05f, freq)) / double(rate_);
		float a = (pan + 1) * 0.25f * 3.14159265f;  // equal-power pan
		v.left = gain * std::cos(a) * 1.4142f * 0.5f;
		v.right = gain * std::sin(a) * 1.4142f * 0.5f;
		std::lock_guard<std::mutex> g(lock_);
		voices_.push_back(v);
	}
}

}  // namespace audio
