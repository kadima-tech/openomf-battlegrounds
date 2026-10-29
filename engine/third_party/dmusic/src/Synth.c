// Copyright © 2024. GothicKit Contributors
// SPDX-License-Identifier: MIT-Modern-Variant
//
// The synthesizer: a C port of the Microsoft DirectMusic software synthesizer (the DLS level 1
// "DDKSynth" sample of the Windows Driver Kit 8.1: csynth.cpp, control.cpp, voice.cpp, mix.cpp,
// instr.cpp, midi.cpp; Copyright (c) Microsoft Corporation, MIT License,
// https://github.com/microsoftarchive/msdn-code-gallery-microsoft). It replaces the earlier
// TinySoundFont backend so that DirectMusic content sounds as it did through the Microsoft
// Synthesizer: the same fixed-point voices, envelopes, LFO, curves, mixing and voice stealing.
//
// As DirectMusic's IDirectMusicPerformance8::InitAudio sets it up by default, the synthesizer
// runs at 22050 Hz with 64 voices (plus the 8 extra voices the Microsoft synth keeps for
// overload); its 16-bit stereo output is resampled linearly to the caller's rate, as
// DirectSound's software mixer does with a 22 kHz buffer.
//
// Units, as in the original: STIME sample counts at the synth rate; VREL volume in 1/100 dB;
// PREL pitch in cents; PFRACT pitch ratios and VFRACT amplitudes with 12 fractional bits.
#include "_Internal.h"
#include "_MsSynthTables.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef int64_t STIME;
typedef int32_t VREL;
typedef int32_t PREL;
typedef int32_t PFRACT;
typedef int32_t VFRACT;

enum {
	DmMs_SAMPLE_RATE = 22050,
	DmMs_MAX_VOICES = 64,       // DMUS_AUDIOPARAMS default dwVoices
	DmMs_EXTRA_VOICES = 8,      // NUM_EXTRA_VOICES
	DmMs_MIN_VOLUME = -9600,    // MIN_VOLUME
	DmMs_PERCEIVED_MIN_VOLUME = -8000,
	DmMs_MINDB = -100,
	DmMs_MAXDB = 0,
	DmMs_STANDARD_PRIORITY = 0x80000000u,  // DAUD_STANDARD_VOICE_PRIORITY
	DmMs_PRIORITY_OFFSET = 1,              // DAUD_CHAN15_VOICE_PRIORITY_OFFSET
};

#define DmMs_FOREVER INT64_MAX

static int32_t bounds(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
// muldiv32.h MulDiv: (a * b) / c in 64 bits, truncated.
static int32_t muldiv(int32_t a, int32_t b, int32_t c) { return c == 0 ? 0 : (int32_t) (((int64_t) a * b) / c); }

// --- Instruments (instr.cpp) ---------------------------------------------------------------------

typedef struct DmMsLfo {  // CSourceLFO
	PFRACT frequency;     // increments through the sine table
	STIME delay;
	VREL mw_volume_scale, volume_scale;
	PREL mw_pitch_scale, pitch_scale;
} DmMsLfo;

typedef struct DmMsEg {  // CSourceEG
	STIME attack, decay, release;
	int32_t vel_attack_scale, key_decay_scale;  // TRELS
	int32_t sustain;                             // 0..1000
	int32_t scale;                               // pitch EG range (cents)
} DmMsEg;

typedef struct DmMsArticulation {  // CSourceArticulation
	DmMsEg pitch_eg, volume_eg;
	DmMsLfo lfo;
	int32_t default_pan;
} DmMsArticulation;

typedef struct DmMsRegion {  // CSourceRegion + CSourceSample
	uint8_t key_low, key_high, group;
	uint8_t vel_low, vel_high;  // DLS2 velocity range (rgnh RangeVelocity)
	uint8_t side;               // a stereo wave's half (wlnk channel): 1 left, 2 right, 0 both
	bool allow_overlap;
	VREL attenuation;
	PREL fine_tune;
	uint8_t root_key;
	bool one_shot;
	uint32_t loop_start, loop_end;  // samples
	uint32_t length;                // CopyFromWave: the wave's length (with its extra sample), less one for a one-shot
	uint32_t real;                  // the wave's own samples (the extra one is at this index)
	uint32_t wave_rate;
	bool eight_bit;
	void const* data;   // int16_t or int8_t samples; the extra one patched per region (pad)
	int16_t pad;        // the extra sample: the loop start's (looping) or 0 (one-shot)
	DmMsArticulation art;
	bool has_art;
} DmMsRegion;

typedef struct DmMsInstrument {
	DmDlsInstrument const* dls;
	uint32_t region_count;
	DmMsRegion* regions;
} DmMsInstrument;

typedef struct DmMsWave {  // CWave: the samples plus one, as DownloadWave makes them
	DmDlsWave const* dls;
	bool eight_bit;
	uint32_t length;  // with the extra sample
	void* data;
} DmMsWave;

// TimeCents2Samples (instr.cpp).
static STIME timecents_to_samples(int32_t tc, uint32_t rate) {
	if (tc == (int32_t) 0x80000000) return 0;
	double t = pow(2.0, (double) tc / (65536.0 * 1200.0)) * rate;
	return (STIME) (uint32_t) t;
}

// PitchCents2PitchFract (instr.cpp).
static PFRACT pitchcents_to_fract(int32_t pc, uint32_t rate) {
	double t = ((double) pc / 65536.0 - 6900.0) / 1200.0;
	t = pow(2.0, t) * 7381975040.0 / rate;
	return (PFRACT) (uint32_t) t;
}

static void DmMsEg_init(DmMsEg* eg) {
	memset(eg, 0, sizeof *eg);
	eg->sustain = 1000;
}

static void DmMsArticulation_init(DmMsArticulation* a) {
	memset(a, 0, sizeof *a);
	a->lfo.frequency = (256 * 4096 * 16 * 5) / DmMs_SAMPLE_RATE;
	DmMsEg_init(&a->volume_eg);
	DmMsEg_init(&a->pitch_eg);
}

// CSourceArticulation::Verify.
static void DmMsArticulation_verify(DmMsArticulation* a) {
	a->lfo.frequency = bounds(a->lfo.frequency, 64, 7600);
	a->lfo.delay = bounds((int32_t) a->lfo.delay, 0, 441000);
	a->lfo.volume_scale = bounds(a->lfo.volume_scale, -1200, 1200);
	a->lfo.mw_volume_scale = bounds(a->lfo.mw_volume_scale, -1200, 1200);
	a->lfo.pitch_scale = bounds(a->lfo.pitch_scale, -1200, 1200);
	a->lfo.mw_pitch_scale = bounds(a->lfo.mw_pitch_scale, -1200, 1200);
	DmMsEg* egs[2] = {&a->pitch_eg, &a->volume_eg};
	for (int i = 0; i < 2; ++i) {
		DmMsEg* eg = egs[i];
		eg->attack = bounds((int32_t) eg->attack, 0, 1764000);
		eg->decay = bounds((int32_t) eg->decay, 0, 1764000);
		eg->sustain = bounds(eg->sustain, 0, 1000);
		eg->release = bounds((int32_t) eg->release, 0, 1764000);
		eg->scale = bounds(eg->scale, -1200, 1200);
		eg->key_decay_scale = bounds(eg->key_decay_scale, -12000, 12000);
		eg->vel_attack_scale = bounds(eg->vel_attack_scale, -12000, 12000);
	}
}

// CSourceArticulation::Download, DLS level 1 connections.
static void DmMsArticulation_load(DmMsArticulation* a, DmDlsArticulator const* art, uint32_t count) {
	uint32_t rate = DmMs_SAMPLE_RATE;
	for (uint32_t k = 0; k < count; ++k) {
		for (uint32_t i = 0; i < art[k].connection_count; ++i) {
			struct DmDlsArticulatorConnection const* c = &art[k].connections[i];
			int32_t scale = c->scale;
			switch (c->source) {
			case DmDlsArticulatorSource_NONE:
				switch (c->destination) {
				case DmDlsArticulatorDestination_LFO_FREQUENCY: a->lfo.frequency = pitchcents_to_fract(scale, rate); break;
				case DmDlsArticulatorDestination_LFO_START_DELAY: a->lfo.delay = timecents_to_samples(scale, rate); break;
				case DmDlsArticulatorDestination_EG1_ATTACK_TIME: a->volume_eg.attack = timecents_to_samples(scale, rate); break;
				case DmDlsArticulatorDestination_EG1_DECAY_TIME: a->volume_eg.decay = timecents_to_samples(scale, rate); break;
				case DmDlsArticulatorDestination_EG1_SUSTAIN_LEVEL: a->volume_eg.sustain = scale >> 16; break;
				case DmDlsArticulatorDestination_EG1_RELEASE_TIME: a->volume_eg.release = timecents_to_samples(scale, rate); break;
				case DmDlsArticulatorDestination_EG2_ATTACK_TIME: a->pitch_eg.attack = timecents_to_samples(scale, rate); break;
				case DmDlsArticulatorDestination_EG2_DECAY_TIME: a->pitch_eg.decay = timecents_to_samples(scale, rate); break;
				case DmDlsArticulatorDestination_EG2_SUSTAIN_LEVEL: a->pitch_eg.sustain = scale >> 16; break;
				case DmDlsArticulatorDestination_EG2_RELEASE_TIME: a->pitch_eg.release = timecents_to_samples(scale, rate); break;
				case DmDlsArticulatorDestination_PAN: a->default_pan = (int32_t) ((scale >> 12) / 125); break;
				default: break;
				}
				break;
			case DmDlsArticulatorSource_LFO:
				if (c->control == DmDlsArticulatorSource_NONE) {
					if (c->destination == DmDlsArticulatorDestination_ATTENUATION) a->lfo.volume_scale = (int32_t) (((int64_t) scale * 10) >> 16);
					else if (c->destination == DmDlsArticulatorDestination_PITCH) a->lfo.pitch_scale = scale >> 16;
				} else if (c->control == DmDlsArticulatorSource_CC1) {
					if (c->destination == DmDlsArticulatorDestination_ATTENUATION) a->lfo.mw_volume_scale = (int32_t) (((int64_t) scale * 10) >> 16);
					else if (c->destination == DmDlsArticulatorDestination_PITCH) a->lfo.mw_pitch_scale = scale >> 16;
				}
				break;
			case DmDlsArticulatorSource_KEY_ON_VELOCITY:
				// Velocity to attenuation is ignored: the synth applies its own velocity curve.
				if (c->destination == DmDlsArticulatorDestination_EG1_ATTACK_TIME) a->volume_eg.vel_attack_scale = scale >> 16;
				else if (c->destination == DmDlsArticulatorDestination_EG2_ATTACK_TIME) a->pitch_eg.vel_attack_scale = scale >> 16;
				break;
			case DmDlsArticulatorSource_KEY_NUMBER:
				if (c->destination == DmDlsArticulatorDestination_EG1_DECAY_TIME) a->volume_eg.key_decay_scale = scale >> 16;
				else if (c->destination == DmDlsArticulatorDestination_EG2_DECAY_TIME) a->pitch_eg.key_decay_scale = scale >> 16;
				break;
			case DmDlsArticulatorSource_EG2:
				if (c->destination == DmDlsArticulatorDestination_PITCH) a->pitch_eg.scale = scale >> 16;
				break;
			default: break;
			}
		}
	}
	DmMsArticulation_verify(a);
}

// --- Channels (CControlLogic: one per performance channel) -----------------------------------------

struct DmSynthChannel {
	DmDls* dls;
	DmMsInstrument* instrument;
	int32_t transpose;
	bool drums;
	uint32_t priority;

	uint8_t volume, expression, pan, mod_wheel;
	int32_t pitch_bend;   // 0..0x3FFF
	PREL bend_range;      // RPN 0
	PREL fine_tune, coarse_tune;
	int32_t rpn, data;
	bool sustain;

	float reset_volume, reset_pan;
	int reset_pitch;
};

// --- Voices (CVoice, CVoiceEG, CVoiceLFO, CDigitalAudio) --------------------------------------------

typedef struct DmMsVoiceEg {
	DmMsEg src;
	STIME start, stop;
} DmMsVoiceEg;

typedef struct DmMsVoiceLfo {
	DmMsLfo src;
	STIME start, repeat;
} DmMsVoiceLfo;

typedef struct DmMsVoice {
	bool in_use, note_on, sustain_on, tag, allow_overlap;
	struct DmSynthChannel* channel;
	DmMsRegion const* region;
	uint8_t key;
	uint32_t priority, group;
	uint32_t note;  // the note-on that started it: its layers share it
	DmMsInstrument const* program;
	STIME start, stop, last_mix, mix_time;
	VREL volume;  // pre-LFO volume, for the end-of-release test
	int32_t default_pan;

	DmMsVoiceEg pitch_eg, volume_eg;
	DmMsVoiceLfo lfo;

	// CDigitalAudio
	VREL base_lvolume, base_rvolume;
	VREL last_lvolume, last_rvolume;
	PREL last_pitch;
	VFRACT last_lvfract, last_rvfract;
	PFRACT base_pitch, last_pitchfract;
	int64_t sample_pos;  // 12 fractional bits
	int64_t loop_start, loop_end, sample_length;
	bool one_shot;

	struct DmMsVoice* next;  // in the in-use queue
} DmMsVoice;

typedef struct DmMsEngine {
	DmMsVoice voices[DmMs_MAX_VOICES + DmMs_EXTRA_VOICES];
	DmMsVoice* in_use;  // priority order, lowest first (QueueVoice)
	STIME time;
	STIME max_span;     // 50 ms
	uint32_t notes;     // note-ons so far

	// Instruments and waves made from the DLS collections in use.
	size_t instrument_count;
	DmMsInstrument** instruments;
	size_t wave_count;
	DmMsWave** waves;

	// Output resampling: synth frames in `buffer`, `pos` the output's place between them.
	int16_t* buffer;
	size_t buffer_len, buffer_cap;
	double pos;
} DmMsEngine;

// CVoiceLFO
static STIME DmMsVoiceLfo_start(DmMsVoiceLfo* l, DmMsLfo const* src, STIME start) {
	l->src = *src;
	l->start = start;
	if (src->mw_pitch_scale == 0 && src->mw_volume_scale == 0 && src->pitch_scale == 0 && src->volume_scale == 0) {
		l->repeat = 44100;
	} else {
		l->repeat = 2097152 / src->frequency;  // (1/8 * 256 * 4096 * 16)
	}
	return l->repeat;
}

static int32_t DmMsVoiceLfo_level(DmMsVoiceLfo* l, STIME t, STIME* next) {
	t -= l->start + l->src.delay;
	if (t < 0) {
		*next = -t;
		return 0;
	}
	*next = l->repeat;
	t *= l->src.frequency;
	t >>= 12 + 4;
	return DmMs_SINE[t & 0xFF];
}

static VREL DmMsVoiceLfo_volume(DmMsVoiceLfo* l, uint8_t mod, STIME t, STIME* next) {
	VREL v = (VREL) mod * l->src.mw_volume_scale;
	v /= 127;
	v += l->src.volume_scale;
	v *= DmMsVoiceLfo_level(l, t, next);
	v /= 100;
	return v;
}

static PREL DmMsVoiceLfo_pitch(DmMsVoiceLfo* l, uint8_t mod, STIME t, STIME* next) {
	static int no_lfo = -1;
	if (no_lfo < 0) no_lfo = getenv("OPENBG_NO_LFO") != NULL;
	if (no_lfo) return 0;
	PREL p = (PREL) mod * l->src.mw_pitch_scale;
	p /= 127;
	p += l->src.pitch_scale;
	p *= DmMsVoiceLfo_level(l, t, next);
	p /= 100;
	return p;
}

static PFRACT prel_to_pfract(PREL p);

// CVoiceEG
static STIME DmMsVoiceEg_start(DmMsVoiceEg* e, DmMsEg const* src, STIME start, int key, int velocity) {
	e->start = start;
	e->stop = DmMs_FOREVER;
	e->src = *src;
	e->src.attack *= prel_to_pfract(velocity * e->src.vel_attack_scale / 127);
	e->src.attack /= 4096;
	e->src.decay *= prel_to_pfract(key * e->src.key_decay_scale / 127);
	e->src.decay /= 4096;
	e->src.decay *= (1000 - e->src.sustain);
	e->src.decay /= 1000;
	return e->src.attack;
}

static int32_t DmMsVoiceEg_level(DmMsVoiceEg* e, STIME end, STIME* next, bool volume) {
	int32_t level = 0;
	if (end <= e->stop) {
		end -= e->start;
		if (end < e->src.attack) {
			level = (int32_t) (1000 * end);
			if (e->src.attack) level = (int32_t) (level / e->src.attack);
			else level = 0;
			*next = e->src.attack - end;
			if (level < 0) level = 0;
			if (level > 1000) level = 1000;
			if (volume) level = DmMs_ATTACK[level / 5];
		} else {
			end -= e->src.attack;
			if (end < e->src.decay) {
				level = (int32_t) ((1000 - e->src.sustain) * end / e->src.decay);
				level = 1000 - level;
				if (end < ((e->src.decay >> 2) - 100)) *next = (e->src.decay >> 2) - end;
				else if (end < ((e->src.decay >> 1) - 100)) *next = (e->src.decay >> 1) - end;
				else *next = e->src.decay - end;
			} else {
				level = e->src.sustain;
				*next = 44100;
			}
		}
	} else {
		STIME bogus;
		end -= e->stop;
		if (end < e->src.release) {
			level = (int32_t) ((int64_t) DmMsVoiceEg_level(e, e->stop, &bogus, volume) * (e->src.release - end) / e->src.release);
			if (end < ((e->src.release >> 2) - 100)) *next = (e->src.release >> 2) - end;
			else if (end < ((e->src.release >> 1) - 100)) *next = (e->src.release >> 1) - end;
			else *next = e->src.release - end;
		} else {
			level = 0;
			*next = DmMs_FOREVER;
		}
	}
	return level;
}

static void DmMsVoiceEg_stop(DmMsVoiceEg* e, STIME t) {
	e->src.release *= DmMsVoiceEg_level(e, t, &e->stop, true);  // adjust for the current level
	e->src.release /= 1000;
	e->stop = t;
}

static void DmMsVoiceEg_quickStop(DmMsVoiceEg* e, STIME t) {
	e->src.release *= DmMsVoiceEg_level(e, t, &e->stop, true);
	e->src.release /= 1000;
	STIME cap = DmMs_SAMPLE_RATE / 70;
	if (e->src.release > cap) e->src.release = cap;
	e->stop = t;
}

static bool DmMsVoiceEg_inRelease(DmMsVoiceEg const* e, STIME t) { return t > e->stop; }

static VREL DmMsVoiceEg_volume(DmMsVoiceEg* e, STIME t, STIME* next) {
	VREL v = DmMsVoiceEg_level(e, t, next, true) * 96;
	v /= 10;
	return v - 9600;
}

static PREL DmMsVoiceEg_pitch(DmMsVoiceEg* e, STIME t, STIME* next) {
	if (e->src.scale == 0) {
		*next = 44100;
		return 0;
	}
	PREL p = DmMsVoiceEg_level(e, t, next, false);
	p *= e->src.scale;
	p /= 1000;
	return p;
}

// CDigitalAudio::VRELToVFRACT
static VFRACT vrel_to_vfract(VREL v) {
	v /= 10;
	if (v < DmMs_MINDB * 10) v = DmMs_MINDB * 10;
	else if (v >= DmMs_MAXDB * 10) v = DmMs_MAXDB * 10;
	return DmMs_DB_TO_VOLUME[v - DmMs_MINDB * 10];
}

// CDigitalAudio::PRELToPFRACT
static PFRACT prel_to_pfract(PREL p) {
	PFRACT f;
	PREL octave;
	if (p > 100) {
		if (p > 4800) p = 4800;
		octave = p / 100;
		p = p % 100;
		f = DmMs_CENTS[p + 100];
		f <<= octave / 12;
		octave = octave % 12;
		f *= DmMs_SEMITONES[octave + 48];
		f >>= 12;
	} else if (p < -100) {
		if (p < -4800) p = -4800;
		octave = p / 100;
		p = (-p) % 100;
		f = DmMs_CENTS[100 - p];
		f >>= ((-octave) / 12);
		octave = (-octave) % 12;
		f *= DmMs_SEMITONES[48 - octave];
		f >>= 12;
	} else {
		f = DmMs_CENTS[p + 100];
	}
	return f;
}

// The wave's samples with the region's extra one after them (CopyFromWave patches it per region).
static int32_t sample_at(DmMsVoice const* v, int64_t i) {
	DmMsRegion const* r = v->region;
	if ((uint64_t) i < r->real) return r->eight_bit ? ((int8_t const*) r->data)[i] : ((int16_t const*) r->data)[i];
	return (uint64_t) i == r->real ? r->pad : 0;
}

// CDigitalAudio::Mix16 / Mix8 (the C path): linear interpolation, the volumes and pitch stepped
// every `period` samples. Returns the samples mixed (fewer when a one-shot ends or a loop wraps).
static uint32_t DmMsVoice_mixSamples(DmMsVoice* v, int16_t* buf, uint32_t len, uint32_t period, VFRACT dlv, VFRACT drv,
                                     PFRACT dp, int64_t end, int64_t loop_len) {
	uint32_t inc = period;
	int64_t pos = v->sample_pos;
	VFRACT lvol = v->last_lvfract, rvol = v->last_rvfract;
	PFRACT pitch = v->last_pitchfract;
	PFRACT pfract = pitch << 8;
	VFRACT lfract = lvol << 8, rfract = rvol << 8;
	bool eight = v->region->eight_bit;
	uint32_t i = 0;
	for (; i < len; ++i) {
		if (pos >= end) {
			if (loop_len) pos -= loop_len;
			else break;
		}
		if (--inc == 0) {
			inc = period;
			pfract += dp;
			pitch = pfract >> 8;
			lfract += dlv;
			lvol = lfract >> 8;
			rfract += drv;
			rvol = rfract >> 8;
		}
		int64_t idx = pos >> 12;
		int32_t frac = (int32_t) (pos & 0xFFF);
		pos += pitch;
		int32_t a = sample_at(v, idx);
		int32_t m = ((sample_at(v, idx + 1) - a) * frac) >> 12;
		m += a;
		int32_t l = m * lvol, r = m * rvol;
		l >>= eight ? 5 : 13;  // the signal bumps up to 15 bits
		r >>= eight ? 5 : 13;
		int32_t sl = buf[i * 2] + l, sr = buf[i * 2 + 1] + r;
		buf[i * 2] = (int16_t) (sl > 32767 ? 32767 : sl < -32768 ? -32768 : sl);
		buf[i * 2 + 1] = (int16_t) (sr > 32767 ? 32767 : sr < -32768 ? -32768 : sr);
	}
	v->sample_pos = pos;
	return i;
}

// CDigitalAudio::Mix
static bool DmMsVoice_mixAudio(DmMsVoice* v, int16_t* buf, uint32_t len, VREL lvol, VREL rvol, PREL pitch) {
	if (len == 0) {  // instant attack
		v->last_pitchfract = (PFRACT) (((int64_t) v->base_pitch * prel_to_pfract(pitch)) >> 12);
		v->last_lvfract = vrel_to_vfract(v->base_lvolume + lvol);
		v->last_rvfract = vrel_to_vfract(v->base_rvolume + rvol);
		v->last_pitch = pitch;
		v->last_lvolume = lvol;
		v->last_rvolume = rvol;
		return true;
	}
	int32_t dmax = abs(lvol - v->last_lvolume);
	v->last_lvolume = lvol;
	int32_t d = abs(rvol - v->last_rvolume);
	if (d > dmax) dmax = d;
	v->last_rvolume = rvol;
	d = abs(pitch - v->last_pitch) << 1;
	if (d > dmax) dmax = d;
	dmax >>= 1;
	v->last_pitch = pitch;
	uint32_t period;
	if (dmax > 0) {
		period = (len << 3) / (uint32_t) dmax;
		if (period > 512) period = 512;
		else if (period < 1) period = 1;
	} else {
		period = 512;
	}
	period += 3;
	period &= 0xFFFFFFFC;

	PFRACT new_pitch = (PFRACT) (((int64_t) v->base_pitch * prel_to_pfract(pitch)) >> 12);
	PFRACT dp = muldiv(new_pitch - v->last_pitchfract, (int32_t) period << 8, (int32_t) len);
	VFRACT nl = vrel_to_vfract(v->base_lvolume + lvol), nr = vrel_to_vfract(v->base_rvolume + rvol);
	VFRACT dl = muldiv(nl - v->last_lvfract, (int32_t) period << 8, (int32_t) len);
	VFRACT dr = muldiv(nr - v->last_rvfract, (int32_t) period << 8, (int32_t) len);

	uint32_t start = 0;
	for (;;) {
		int64_t end, loop_len;
		if (v->one_shot) {
			end = v->sample_length;
			loop_len = 0;
		} else {
			end = v->loop_end;
			loop_len = v->loop_end - v->loop_start;
			if (loop_len <= new_pitch) return false;
		}
		uint32_t done = DmMsVoice_mixSamples(v, buf + start * 2, len, period, dl, dr, dp, end, loop_len);
		if (v->one_shot) {
			if (done < len) return false;
			break;
		}
		if (done >= len) break;
		start += done;
		len -= done;
		v->sample_pos -= v->loop_end - v->loop_start;
	}
	v->last_lvfract = nl;
	v->last_rvfract = nr;
	v->last_pitchfract = new_pitch;
	return true;
}

static PREL bend_of(struct DmSynthChannel const* c) {
	PREL p = c->pitch_bend - 0x2000;
	p *= c->bend_range;
	return p >> 13;  // CPitchBendIn::GetPitch
}

// CVoice::GetNewVolume
static void DmMsVoice_newVolume(DmMsEngine* e, DmMsVoice* v, STIME t, VREL* left, VREL* right) {
	(void) e;
	STIME next;
	VREL vol = DmMsVoiceEg_volume(&v->volume_eg, t, &next);
	if (next < v->mix_time) v->mix_time = next;
	v->volume = vol;
	vol += DmMsVoiceLfo_volume(&v->lfo, v->channel->mod_wheel, t, &next);
	if (next < v->mix_time) v->mix_time = next;
	vol += DmMs_MIDI_TO_VREL[v->channel->expression];
	vol += DmMs_MIDI_TO_VREL[v->channel->volume];  // m_fAllowVolumeChangeWhilePlayingNote
	// m_vrGainAdjust: 0 (CSynth::Open)
	VREL r = vol;
	int32_t pan = v->channel->pan + v->default_pan;  // m_fAllowPanWhilePlayingNote
	if (pan < 0) pan = 0;
	if (pan > 127) pan = 127;
	*left = vol + DmMs_PAN_TO_VREL[127 - pan];
	*right = r + DmMs_PAN_TO_VREL[pan];
	// A stereo wave's half sounds on its own side only, at full level and unpanned (dmsynth).
	if (v->region && v->region->side) {
		*left = v->region->side == 1 ? vol : DmMs_MIN_VOLUME;
		*right = v->region->side == 2 ? r : DmMs_MIN_VOLUME;
	}
}

// CVoice::GetNewPitch
static PREL DmMsVoice_newPitch(DmMsVoice* v, STIME t) {
	STIME next;
	PREL p = DmMsVoiceLfo_pitch(&v->lfo, v->channel->mod_wheel, t, &next);
	if (v->mix_time > next) v->mix_time = next;
	p += DmMsVoiceEg_pitch(&v->pitch_eg, t, &next);
	if (v->mix_time > next) v->mix_time = next;
	p += bend_of(v->channel);
	return p;
}

static void DmMsVoice_stop(DmMsVoice* v, STIME t) {
	if (!v->note_on) return;
	if (t <= v->start) t = v->start + 1;
	DmMsVoiceEg_stop(&v->pitch_eg, t);
	DmMsVoiceEg_stop(&v->volume_eg, t);
	v->note_on = false;
	v->sustain_on = false;
	v->stop = t;
}

static void DmMsVoice_quickStop(DmMsVoice* v, STIME t) {
	v->tag = true;
	if (v->note_on || v->sustain_on) {
		if (t <= v->start) t = v->start + 1;
		DmMsVoiceEg_stop(&v->pitch_eg, t);
		DmMsVoiceEg_quickStop(&v->volume_eg, t);
		v->note_on = false;
		v->sustain_on = false;
		v->stop = t;
	} else {
		DmMsVoiceEg_quickStop(&v->volume_eg, v->stop);
	}
}

// CVoice::StartVoice + CDigitalAudio::StartVoice
static bool DmMsVoice_start(DmMsEngine* e, DmMsVoice* v, DmMsRegion const* r, STIME t, int key, int velocity, PREL pitch) {
	struct DmSynthChannel* c = v->channel;
	VREL vol = 0;  // the master volume (GM reset: 0)
	pitch += 0;    // the region's tuning (m_prTuning, 0)
	v->group = r->group;
	v->allow_overlap = r->allow_overlap;
	vol += DmMs_MIDI_TO_VREL[velocity];
	vol += r->attenuation;
	v->default_pan = r->art.default_pan;
	v->base_lvolume = vol;  // pan applied while playing (m_fAllowPanWhilePlayingNote)
	v->base_rvolume = vol;

	v->mix_time = DmMsVoiceLfo_start(&v->lfo, &r->art.lfo, t);
	STIME m = DmMsVoiceEg_start(&v->pitch_eg, &r->art.pitch_eg, t, key, velocity);
	if (m < v->mix_time) v->mix_time = m;
	m = DmMsVoiceEg_start(&v->volume_eg, &r->art.volume_eg, t, key, velocity);
	if (m < v->mix_time) v->mix_time = m;
	if (v->mix_time > e->max_span) v->mix_time = e->max_span;
	if (r->data == NULL) return false;

	v->region = r;
	v->last_lvfract = vrel_to_vfract(DmMs_MIN_VOLUME);
	v->last_rvfract = vrel_to_vfract(DmMs_MIN_VOLUME);
	v->last_lvolume = v->last_rvolume = 0;
	v->last_pitch = 0;
	PREL base = pitch + r->fine_tune + (key - r->root_key) * 100;
	v->base_pitch = prel_to_pfract(base);
	v->base_pitch = (PFRACT) ((int64_t) v->base_pitch * r->wave_rate / DmMs_SAMPLE_RATE);
	v->last_pitchfract = v->base_pitch;
	v->one_shot = r->one_shot;
	if (r->loop_end - r->loop_start >= 0x80000) v->one_shot = true;  // loops over 1 meg
	v->sample_pos = 0;
	v->loop_start = (int64_t) r->loop_start << 12;
	v->loop_end = (int64_t) r->loop_end << 12;
	v->sample_length = (int64_t) r->length << 12;
	if (v->loop_end <= v->loop_start) v->one_shot = true;

	v->note_on = true;
	v->tag = false;
	v->start = t;
	v->last_mix = t - 1;
	v->stop = DmMs_FOREVER;
	if (v->mix_time == 0) {
		PREL p = DmMsVoice_newPitch(v, t);
		VREL l, rr;
		DmMsVoice_newVolume(e, v, t, &l, &rr);
		if (v->mix_time > e->max_span) v->mix_time = e->max_span;
		DmMsVoice_mixAudio(v, NULL, 0, l, rr, p);
	}
	v->volume = 0;
	return true;
}

// CVoice::Mix: control points every mix_time samples (at most 50 ms), the audio between.
static void DmMsVoice_mix(DmMsEngine* e, DmMsVoice* v, int16_t* buf, STIME start, STIME end) {
	bool in_use = true, full = true;
	STIME end_mix = start;
	STIME start_mix = v->start < start ? start : v->start;
	if (v->last_mix >= end) return;
	if (v->last_mix >= start_mix) start_mix = v->last_mix;
	while (start_mix < end && in_use) {
		end_mix = start_mix + v->mix_time;
		if (end_mix > end) end_mix = end;
		v->mix_time = e->max_span;
		if (v->last_mix < v->stop && v->stop < end) {
			if (v->mix_time > v->stop - v->last_mix) v->mix_time = v->stop - v->last_mix;
		}
		PREL p = DmMsVoice_newPitch(v, end_mix);
		VREL l, r;
		DmMsVoice_newVolume(e, v, end_mix, &l, &r);
		if (DmMsVoiceEg_inRelease(&v->volume_eg, end_mix) && v->volume < DmMs_PERCEIVED_MIN_VOLUME) in_use = false;
		full = DmMsVoice_mixAudio(v, buf + (start_mix - start) * 2, (uint32_t) (end_mix - start_mix), l, r, p);
		start_mix = end_mix;
	}
	v->in_use = in_use && full;
	if (!v->in_use) v->stop = end_mix;
	v->last_mix = end_mix;
}

// --- Voice lists (CSynth) -------------------------------------------------------------------------

static void DmMsEngine_unqueue(DmMsEngine* e, DmMsVoice* v) {
	for (DmMsVoice** p = &e->in_use; *p; p = &(*p)->next) {
		if (*p == v) {
			*p = v->next;
			v->next = NULL;
			return;
		}
	}
}

// QueueVoice: priority order, lowest first; older notes later within a priority.
static void DmMsEngine_queue(DmMsEngine* e, DmMsVoice* v) {
	DmMsVoice** p = &e->in_use;
	if (*p == NULL || (*p)->priority > v->priority) {
		v->next = *p;
		*p = v;
		return;
	}
	DmMsVoice* scan = *p;
	while (scan->next && scan->next->priority <= v->priority) scan = scan->next;
	v->next = scan->next;
	scan->next = v;
}

static size_t DmMsEngine_count(DmMsEngine* e) {
	size_t n = 0;
	for (DmMsVoice* v = e->in_use; v; v = v->next) ++n;
	return n;
}

// OldestVoice: the best one to cut short -- priority first, then released before held, quieter
// before louder, older before newer.
static DmMsVoice* DmMsEngine_oldest(DmMsEngine* e) {
	DmMsVoice* best = e->in_use;
	if (!best) return NULL;
	for (DmMsVoice* v = best->next; v; v = v->next) {
		if (v->tag) continue;
		if (best->tag) {
			best = v;
		} else if (v->priority <= best->priority) {
			if (v->note_on) {
				if (best->note_on && best->start > v->start) best = v;
			} else if (best->note_on || best->volume > v->volume) {
				best = v;
			}
		}
	}
	return best->tag ? NULL : best;
}

// StealVoice: a voice at or below this priority, silenced at once.
static DmMsVoice* DmMsEngine_steal(DmMsEngine* e, uint32_t priority) {
	DmMsVoice* best = NULL;
	for (DmMsVoice* v = e->in_use; v; v = v->next) {
		if (v->priority > priority) continue;
		if (!best) {
			best = v;
		} else if (!v->note_on) {
			if (best->note_on || best->volume > v->volume) best = v;
		} else if (best->start > v->start) {
			best = v;
		}
	}
	if (best) {
		best->in_use = false;
		DmMsEngine_unqueue(e, best);
	}
	return best;
}

// StealNotes: keep NUM_EXTRA_VOICES free by quick-stopping the oldest when the voice pool runs low.
static void DmMsEngine_stealNotes(DmMsEngine* e, STIME t) {
	size_t used = DmMsEngine_count(e);
	size_t total = DmMs_MAX_VOICES + DmMs_EXTRA_VOICES;
	long to_move = (long) DmMs_EXTRA_VOICES - (long) (total - used);
	if (to_move <= 0) return;
	for (DmMsVoice* v = e->in_use; v; v = v->next) {
		if (v->tag) --to_move;
	}
	for (; to_move > 0; --to_move) {
		DmMsVoice* v = DmMsEngine_oldest(e);
		if (!v) break;
		DmMsVoice_quickStop(v, t);
	}
}

static DmMsVoice* DmMsEngine_free(DmMsEngine* e) {
	for (size_t i = 0; i < DmMs_MAX_VOICES + DmMs_EXTRA_VOICES; ++i) {
		DmMsVoice* v = &e->voices[i];
		bool queued = false;
		for (DmMsVoice* q = e->in_use; q; q = q->next) queued |= q == v;
		if (!v->in_use && !queued) return v;
	}
	return NULL;
}

// --- Instrument building ---------------------------------------------------------------------------

static DmMsWave* DmMsEngine_wave(DmMsEngine* e, DmDlsWave const* w) {
	for (size_t i = 0; i < e->wave_count; ++i) {
		if (e->waves[i]->dls == w) return e->waves[i];
	}
	DmMsWave* mw = Dm_alloc(sizeof *mw);
	if (!mw) return NULL;
	memset(mw, 0, sizeof *mw);
	mw->dls = w;
	if (w->format == DmDlsWaveFormat_PCM && w->bits_per_sample == 8) {
		mw->eight_bit = true;
		mw->length = w->pcm_size + 1;
		int8_t* d = Dm_alloc(mw->length);
		if (d) {
			for (uint32_t i = 0; i < w->pcm_size; ++i) d[i] = (int8_t) ((int) w->pcm[i] - 128);
			d[w->pcm_size] = 0;
		}
		mw->data = d;
	} else {
		size_t n = DmDls_decodeSamples(w, NULL, 0);
		mw->length = (uint32_t) n + 1;
		int16_t* d = Dm_alloc(sizeof(int16_t) * mw->length);
		if (d) {
			if (w->format == DmDlsWaveFormat_PCM) {
				memcpy(d, w->pcm, n * sizeof(int16_t));
			} else {
				float* f = Dm_alloc(sizeof(float) * (n ? n : 1));
				if (f) {
					DmDls_decodeSamples(w, f, n);
					for (size_t i = 0; i < n; ++i) {
						float s = f[i] * 32767.f;
						d[i] = (int16_t) (s > 32767.f ? 32767 : s < -32768.f ? -32768 : (int) s);
					}
					Dm_free(f);
				}
			}
			d[n] = 0;
		}
		mw->data = d;
	}
	DmMsWave** grown = Dm_alloc(sizeof *grown * (e->wave_count + 1));
	if (!grown) return mw;
	if (e->waves) memcpy(grown, e->waves, sizeof *grown * e->wave_count);
	Dm_free(e->waves);
	e->waves = grown;
	e->waves[e->wave_count++] = mw;
	return mw;
}

// The DLS instrument as DirectMusic downloads it to the synth: each region with its wave sample
// (the region's wsmp, else the wave's) and its articulation (the region's, else the instrument's).
static DmMsInstrument* DmMsEngine_instrument(DmMsEngine* e, DmDls* dls, DmDlsInstrument const* ins) {
	for (size_t i = 0; i < e->instrument_count; ++i) {
		if (e->instruments[i]->dls == ins) return e->instruments[i];
	}
	DmMsInstrument* mi = Dm_alloc(sizeof *mi);
	if (!mi) return NULL;
	memset(mi, 0, sizeof *mi);
	mi->dls = ins;
	mi->region_count = ins->region_count;
	mi->regions = Dm_alloc(sizeof(DmMsRegion) * (ins->region_count ? ins->region_count : 1));
	if (!mi->regions) {
		Dm_free(mi);
		return NULL;
	}
	memset(mi->regions, 0, sizeof(DmMsRegion) * (ins->region_count ? ins->region_count : 1));
	for (uint32_t i = 0; i < ins->region_count; ++i) {
		DmDlsRegion const* src = &ins->regions[i];
		DmMsRegion* r = &mi->regions[i];
		r->key_low = (uint8_t) src->range_low;
		r->key_high = (uint8_t) src->range_high;
		// An unset velocity range (0-0, as old DLS1 regions leave it) takes every velocity.
		r->vel_low = (uint8_t) (src->velocity_high ? src->velocity_low : 0);
		r->vel_high = (uint8_t) (src->velocity_high ? src->velocity_high : 127);
		r->allow_overlap = src->nonexclusive;
		r->group = (uint8_t) src->key_group;
		if (src->link_multi_channel && (src->link_channel == 1 || src->link_channel == 2)) r->side = (uint8_t) src->link_channel;
		DmMsArticulation_init(&r->art);
		if (src->articulator_count) {
			DmMsArticulation_load(&r->art, src->articulators, src->articulator_count);
			r->has_art = true;
		} else if (ins->articulator_count) {
			DmMsArticulation_load(&r->art, ins->articulators, ins->articulator_count);
			r->has_art = true;
		} else {
			DmMsArticulation_verify(&r->art);
		}
		uint32_t index = src->link_table_index;
		if (index >= dls->pool_table_size) continue;
		// The pool table holds each wave's offset in the wave pool; the waves are stored in that
		// order, so a wave's index is the number of cues before its offset.
		uint32_t wave_index = 0;
		for (uint32_t k = 0; k < dls->pool_table_size; ++k) wave_index += dls->pool_table[k] < dls->pool_table[index];
		if (wave_index >= dls->wave_table_size) continue;
		DmDlsWave const* w = &dls->wave_table[wave_index];
		DmMsWave* mw = DmMsEngine_wave(e, w);
		if (!mw || !mw->data) continue;
		DmDlsWaveSample const* s = src->sample.present ? &src->sample : &w->sample;
		r->attenuation = (VREL) (((int64_t) s->gain * 10) >> 16);
		r->fine_tune = (int16_t) s->fine_tune;
		r->root_key = (uint8_t) s->unity_note;
		r->wave_rate = w->samples_per_second;
		r->eight_bit = mw->eight_bit;
		r->data = mw->data;
		r->length = mw->length;
		r->real = mw->length - 1;
		if (!s->looping) {
			r->one_shot = true;
			r->length--;  // CopyFromWave: the extra sample is 0
			r->pad = 0;
		} else {
			r->loop_start = s->loop_start;
			r->loop_end = s->loop_start + s->loop_length;
			if (r->loop_start >= r->length) r->loop_start = 0;
			// CopyFromWave: the extra sample repeats the loop start.
			r->pad = r->loop_start < r->real
			             ? (mw->eight_bit ? ((int8_t const*) mw->data)[r->loop_start] : ((int16_t const*) mw->data)[r->loop_start])
			             : 0;
		}
	}
	DmMsInstrument** grown = Dm_alloc(sizeof *grown * (e->instrument_count + 1));
	if (!grown) return mi;
	if (e->instruments) memcpy(grown, e->instruments, sizeof *grown * e->instrument_count);
	Dm_free(e->instruments);
	e->instruments = grown;
	e->instruments[e->instrument_count++] = mi;
	return mi;
}

// The regions a note plays: every region whose key and velocity ranges hold it. DDKSynth's
// CInstrument::ScanForRegion (DLS1) takes the first by key alone; the DLS2 synth the game ran on
// selects by velocity too and sounds every layer that matches (the theme's instruments choose
// their samples by velocity: 127, 126, 125 and below).
static uint32_t DmMsInstrument_regions(DmMsInstrument const* mi, int key, int velocity, DmMsRegion const** out, uint32_t max) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < mi->region_count && n < max; ++i) {
		DmMsRegion const* r = &mi->regions[i];
		if (key >= r->key_low && key <= r->key_high && velocity >= r->vel_low && velocity <= r->vel_high) out[n++] = r;
	}
	return n;
}

// --- The synth -------------------------------------------------------------------------------------

static DmMsEngine* engine_of(DmSynth* slf) { return (DmMsEngine*) slf->engine; }

void DmSynth_init(DmSynth* slf, uint32_t sample_rate) {
	if (slf == NULL) {
		return;
	}
	memset(slf, 0, sizeof *slf);
	slf->rate = sample_rate;
	slf->volume = 1;
	DmMsEngine* e = Dm_alloc(sizeof *e);
	if (e) {
		memset(e, 0, sizeof *e);
		e->max_span = (DmMs_SAMPLE_RATE + 19) / 20;  // 50 ms
	}
	slf->engine = e;
}

void DmSynth_free(DmSynth* slf) {
	if (slf == NULL) {
		return;
	}
	DmMsEngine* e = engine_of(slf);
	if (e) {
		for (size_t i = 0; i < e->instrument_count; ++i) {
			Dm_free(e->instruments[i]->regions);
			Dm_free(e->instruments[i]);
		}
		for (size_t i = 0; i < e->wave_count; ++i) {
			Dm_free(e->waves[i]->data);
			Dm_free(e->waves[i]);
		}
		Dm_free(e->instruments);
		Dm_free(e->waves);
		Dm_free(e->buffer);
		Dm_free(e);
	}
	Dm_free(slf->channels);
	slf->engine = NULL;
}

void DmSynth_reset(DmSynth* slf) {
	if (slf == NULL) {
		return;
	}
	for (uint32_t i = 0; i < slf->channels_len; ++i) {
		struct DmSynthChannel* c = &slf->channels[i];
		c->volume = (uint8_t) (c->reset_volume * 127.f + 0.5f);
		c->pan = (uint8_t) (c->reset_pan * 127.f + 0.5f);
		c->pitch_bend = c->reset_pitch;
	}
}

static void DmSynthChannel_init(struct DmSynthChannel* c, uint32_t index) {
	memset(c, 0, sizeof *c);
	// CVolumeIn 100, CExpressionIn 127, CPanIn 64, CPitchBendIn 0x2000 with a 200 cent range.
	c->volume = 100;
	c->expression = 127;
	c->pan = 64;
	c->pitch_bend = 0x2000;
	c->bend_range = 200;
	c->rpn = 0x3FFF;
	c->reset_volume = 100.f / 127.f;
	c->reset_pan = 64.f / 127.f;
	c->reset_pitch = 0x2000;
	// GMReset: part p (MIDI channel 10 is part 0, then channels 1..9, 11..16) gets the standard
	// priority + (16 - p) x the per-channel offset.
	uint32_t midi = index % 16;
	uint32_t part = midi == 9 ? 0 : midi < 9 ? midi + 1 : midi;
	c->priority = DmMs_STANDARD_PRIORITY + (16 - part) * DmMs_PRIORITY_OFFSET;
}

static DmResult DmSynth_growChannels(DmSynth* slf, size_t count) {
	if (count <= slf->channels_len) return DmResult_SUCCESS;
	struct DmSynthChannel* grown = Dm_alloc(sizeof *grown * count);
	if (grown == NULL) return DmResult_MEMORY_EXHAUSTED;
	if (slf->channels) memcpy(grown, slf->channels, sizeof *grown * slf->channels_len);
	for (size_t i = slf->channels_len; i < count; ++i) DmSynthChannel_init(&grown[i], (uint32_t) i);
	Dm_free(slf->channels);
	slf->channels = grown;
	slf->channels_len = count;
	return DmResult_SUCCESS;
}

// A band: each instrument goes to its performance channel with its bank/patch, volume, pan,
// transpose and priority (see https://documentation.help/DirectMusic/usingbands.htm); channels
// the band does not name keep what they had.
void DmSynth_sendBandUpdate(DmSynth* slf, DmBand* band) {
	if (slf == NULL || band == NULL || engine_of(slf) == NULL) {
		return;
	}
	size_t count = 0;
	for (size_t i = 0; i < band->instruments_len; ++i) {
		if (band->instruments[i].channel + 1 > count) count = band->instruments[i].channel + 1;
	}
	if (DmSynth_growChannels(slf, count) != DmResult_SUCCESS) return;
	for (size_t i = 0; i < band->instruments_len; ++i) {
		DmInstrument* ins = &band->instruments[i];
		if (ins->dls == NULL) continue;
		struct DmSynthChannel* c = &slf->channels[ins->channel];
		DmDlsInstrument* di = DmInstrument_getDlsInstrument(ins);
		c->dls = ins->dls;
		c->instrument = di ? DmMsEngine_instrument(engine_of(slf), ins->dls, di) : NULL;
		c->drums = di && (di->bank & DmDls_DRUM_KIT);
		if (ins->options & DmInstrument_VALID_PAN) {
			c->pan = ins->pan;
			c->reset_pan = (float) ins->pan / 127.f;
		}
		if (ins->options & DmInstrument_VALID_VOLUME) {
			c->volume = ins->volume;
			c->reset_volume = (float) ins->volume / 127.f;
		}
		if (ins->options & DmInstrument_VALID_TRANSPOSE) c->transpose = ins->transpose;
		if (ins->options & DmInstrument_VALID_CHANNEL_PRIORITY) c->priority = ins->channel_priority;
		if (ins->options & DmInstrument_VALID_PITCH_BEND_RANGE) c->bend_range = ins->pitch_bend_range * 100;
	}
}

// QueueNotes' special commands and the MIDI recorders, applied at once.
void DmSynth_sendControl(DmSynth* slf, uint32_t channel, uint8_t control, float value) {
	if (slf == NULL || DmSynth_growChannels(slf, channel + 1) != DmResult_SUCCESS) {
		return;
	}
	struct DmSynthChannel* c = &slf->channels[channel];
	DmMsEngine* e = engine_of(slf);
	int v = (int) (value * 127.f + 0.5f);
	if (v < 0) v = 0;
	if (v > 127) v = 127;
	switch (control) {
	case 1: c->mod_wheel = (uint8_t) v; break;
	case 7: c->volume = (uint8_t) v; break;
	case 10: c->pan = (uint8_t) v; break;
	case 11: c->expression = (uint8_t) v; break;
	case 100: c->rpn = (c->rpn & 0x3f80) + v; break;  // RPN LSB
	case 101: c->rpn = (c->rpn & 0x7f) + (v << 7); break;  // RPN MSB
	case 98:
	case 99: c->rpn = 0x3FFF; break;  // NRPN
	case 38:  // data entry LSB
		c->data &= ~0x7F;
		c->data |= v;
		if (c->rpn == 1) c->fine_tune = (c->data - 8192) * 100 / 8192;
		break;
	case 6:  // data entry MSB
		c->data &= ~(0x7F << 7);
		c->data |= v << 7;
		if (c->rpn == 0) c->bend_range = v * 100;
		else if (c->rpn == 1) c->fine_tune = (c->data - 8192) * 100 / 8192;
		else if (c->rpn == 2) c->coarse_tune = 100 * (v - 64);
		break;
	case 64: {  // sustain
		c->sustain = v >= 64;
		if (!c->sustain && e) {
			for (DmMsVoice* vo = e->in_use; vo; vo = vo->next) {
				if (vo->sustain_on && vo->channel == c) DmMsVoice_stop(vo, e->time);
			}
		}
		break;
	}
	case 120:  // all sounds off
		if (e) {
			for (DmMsVoice* vo = e->in_use; vo; vo = vo->next) {
				if (vo->note_on && vo->channel == c) DmMsVoice_stop(vo, e->time);
			}
		}
		break;
	case 121:  // reset all controllers
		c->mod_wheel = 0;
		c->expression = 127;
		c->pitch_bend = 0x2000;
		break;
	case 123:  // all notes off
		if (e) {
			for (DmMsVoice* vo = e->in_use; vo; vo = vo->next) {
				if (vo->note_on && !vo->sustain_on && vo->channel == c) {
					if (c->sustain) vo->sustain_on = true;
					else DmMsVoice_stop(vo, e->time);
				}
			}
		}
		break;
	default: Dm_report(DmLogLevel_DEBUG, "DmSynth: Control change %d is not handled.", control); break;
	}
}

void DmSynth_sendControlReset(DmSynth* slf, uint32_t channel, uint8_t control, float reset) {
	if (slf == NULL || channel >= slf->channels_len) {
		return;
	}
	struct DmSynthChannel* c = &slf->channels[channel];
	if (control == 7) c->reset_volume = reset;
	else if (control == 10) c->reset_pan = reset;
}

void DmSynth_sendPitchBend(DmSynth* slf, uint32_t channel, int bend) {
	if (slf == NULL || DmSynth_growChannels(slf, channel + 1) != DmResult_SUCCESS) {
		return;
	}
	slf->channels[channel].pitch_bend = bend < 0 ? 0 : bend > 0x3FFF ? 0x3FFF : bend;
}

void DmSynth_sendPitchBendReset(DmSynth* slf, uint32_t channel, int reset) {
	if (slf == NULL || channel >= slf->channels_len) {
		return;
	}
	slf->channels[channel].reset_pitch = reset;
}

static void DmSynth_noteOff(DmSynth* slf, struct DmSynthChannel* c, int key) {
	DmMsEngine* e = engine_of(slf);
	// dmsynth's note-off: the first playing voice of the key and every other layer of its note-on.
	uint32_t note = 0;
	for (DmMsVoice* v = e->in_use; v; v = v->next) {
		if (v->note_on && !v->sustain_on && v->key == key && v->channel == c && (note == 0 || v->note == note)) {
			note = v->note;
			if (c->sustain) v->sustain_on = true;
			else DmMsVoice_stop(v, e->time);
		}
	}
}

// QueueNotes, note on.
void DmSynth_sendNoteOn(DmSynth* slf, uint32_t channel, uint8_t note, uint8_t velocity) {
	if (slf == NULL || engine_of(slf) == NULL || channel >= slf->channels_len) {
		return;
	}
	// OPENBG_MUSIC_SOLO=<pchannel>: only that channel sounds (debugging).
	static int solo = -2;
	if (solo == -2) solo = getenv("OPENBG_MUSIC_SOLO") ? atoi(getenv("OPENBG_MUSIC_SOLO")) : -1;
	if (solo >= 0 && (int) channel != solo) return;
	DmMsEngine* e = engine_of(slf);
	struct DmSynthChannel* c = &slf->channels[channel];
	int key = note + c->transpose;
	if (key < 0 || key > 127) return;
	if (velocity == 0) {
		DmSynth_noteOff(slf, c, key);
		return;
	}
	if (c->instrument == NULL) return;
	DmMsRegion const* layers[16];
	uint32_t count = DmMsInstrument_regions(c->instrument, key, velocity, layers, 16);
	if (count == 0) return;
	uint32_t serial = ++e->notes ? e->notes : ++e->notes;
	bool exclusive = false;
	for (uint32_t i = 0; i < count; ++i) exclusive = exclusive || !layers[i]->allow_overlap;
	if (exclusive) {
		for (DmMsVoice* v = e->in_use; v; v = v->next) {
			if (v->channel == c && v->key == key) DmMsVoice_quickStop(v, e->time);
		}
	}
	for (uint32_t i = 0; i < count; ++i) {
		DmMsRegion const* r = layers[i];
		if (r->group != 0) {
			for (DmMsVoice* v = e->in_use; v; v = v->next) {
				if (v->group == r->group && v->channel == c && v->program == c->instrument) DmMsVoice_quickStop(v, e->time);
			}
		}
		DmMsVoice* v = DmMsEngine_free(e);
		if (v == NULL) v = DmMsEngine_steal(e, c->priority);
		if (v == NULL) return;  // a lost note
		PREL pitch = c->fine_tune;
		if (!c->drums) pitch += c->coarse_tune;
		memset(v, 0, sizeof *v);
		v->key = (uint8_t) key;
		v->channel = c;
		v->program = c->instrument;
		v->priority = c->priority;
		v->note = serial;
		if (DmMsVoice_start(e, v, r, e->time, key, velocity, pitch)) {
			v->in_use = true;
			DmMsEngine_queue(e, v);
		}
		static int dbg = -2;
		if (dbg == -2) dbg = !getenv("OPENBG_DEBUG_NOTES") ? -1 : strcmp(getenv("OPENBG_DEBUG_NOTES"), "all") == 0 ? -3 : atoi(getenv("OPENBG_DEBUG_NOTES"));
		if (dbg == -3 || (dbg >= 0 && (int) channel == dbg)) {
			fprintf(stderr, "[note %lld] ch %u vol %d expr %d pan %d key %d vel %d root %d rate %u len %u loop %u-%u one_shot %d pitch %d\n", (long long) e->time, channel, c->volume, c->expression, c->pan, key, velocity,
			        r->root_key, r->wave_rate, r->length, r->loop_start, r->loop_end, r->one_shot, v->base_pitch);
		}
	}
}

void DmSynth_sendNoteOff(DmSynth* slf, uint32_t channel, uint8_t note) {
	if (slf == NULL || engine_of(slf) == NULL || channel >= slf->channels_len) {
		return;
	}
	struct DmSynthChannel* c = &slf->channels[channel];
	DmSynth_noteOff(slf, c, note + c->transpose);
}

void DmSynth_sendNoteOffAll(DmSynth* slf, uint32_t channel) {
	if (slf == NULL || engine_of(slf) == NULL || channel >= slf->channels_len) {
		return;
	}
	DmMsEngine* e = engine_of(slf);
	struct DmSynthChannel* c = &slf->channels[channel];
	for (DmMsVoice* v = e->in_use; v; v = v->next) {
		if (v->note_on && v->channel == c) DmMsVoice_stop(v, e->time);
	}
}

void DmSynth_sendNoteOffEverything(DmSynth* slf) {
	if (slf == NULL) {
		return;
	}
	for (uint32_t i = 0; i < slf->channels_len; ++i) DmSynth_sendNoteOffAll(slf, i);
}

void DmSynth_setVolume(DmSynth* slf, float vol) {
	if (slf == NULL) {
		return;
	}
	slf->volume = vol;
}

// CSynth::Mix + FinishMix: `frames` stereo frames at 22050 Hz into `out` (zeroed first).
static void DmMsEngine_mix(DmMsEngine* e, int16_t* out, uint32_t frames) {
	memset(out, 0, sizeof(int16_t) * 2 * frames);
	STIME end = e->time + frames;
	DmMsEngine_stealNotes(e, end);
	for (DmMsVoice *v = e->in_use, *next; v; v = next) {
		next = v->next;
		DmMsVoice_mix(e, v, out, e->time, end);
		if (!v->in_use) DmMsEngine_unqueue(e, v);
	}
	for (uint32_t i = 0; i < frames * 2; ++i) {  // FinishMix: double, clamped
		int32_t s = out[i] << 1;
		out[i] = (int16_t) (s < -32767 ? -32767 : s > 32767 ? 32767 : s);
	}
	e->time = end;
}

// Renders `len` samples (frames x channels) at the caller's rate: the synth frames resampled
// linearly. Each call mixes the synth frames it needs in one CSynth::Mix (the performance renders
// up to each event, so a call is the stretch between events, as the DirectSound sink's buffers
// were); events between calls take effect at the synth's current frame.
size_t DmSynth_render(DmSynth* slf, void* buf, size_t len, DmRenderOptions fmt) {
	int channels = (fmt & DmRender_STEREO) ? 2 : 1;
	size_t frames = len / (size_t) channels;
	size_t bytes = fmt & DmRender_FLOAT ? len * 4 : len * 2;
	DmMsEngine* e = engine_of(slf);
	if (e == NULL || frames == 0) {
		return bytes;
	}
	double step = (double) DmMs_SAMPLE_RATE / (double) slf->rate;
	// The synth frames up to the one after the last output frame's.
	size_t need = (size_t) (e->pos + step * (double) (frames - 1)) + 2;
	if (need > e->buffer_len) {
		if (need > e->buffer_cap) {
			size_t cap = e->buffer_cap ? e->buffer_cap : 1024;
			while (cap < need) cap *= 2;
			int16_t* grown = Dm_alloc(sizeof(int16_t) * 2 * cap);
			if (!grown) return bytes;
			if (e->buffer) memcpy(grown, e->buffer, sizeof(int16_t) * 2 * e->buffer_len);
			Dm_free(e->buffer);
			e->buffer = grown;
			e->buffer_cap = cap;
		}
		DmMsEngine_mix(e, e->buffer + e->buffer_len * 2, (uint32_t) (need - e->buffer_len));
		e->buffer_len = need;
	}
	for (size_t f = 0; f < frames; ++f) {
		size_t i0 = (size_t) e->pos;
		double t = e->pos - (double) i0;
		float l = (float) (e->buffer[i0 * 2] + (e->buffer[(i0 + 1) * 2] - e->buffer[i0 * 2]) * t) / 32768.f;
		float r = (float) (e->buffer[i0 * 2 + 1] + (e->buffer[(i0 + 1) * 2 + 1] - e->buffer[i0 * 2 + 1]) * t) / 32768.f;
		l *= slf->volume;
		r *= slf->volume;
		if (channels == 1) {
			float m = (l + r) * 0.5f;
			if (fmt & DmRender_FLOAT) ((float*) buf)[f] = m;
			else ((int16_t*) buf)[f] = (int16_t) (m * 32767.f);
		} else if (fmt & DmRender_FLOAT) {
			((float*) buf)[f * 2] = l;
			((float*) buf)[f * 2 + 1] = r;
		} else {
			((int16_t*) buf)[f * 2] = (int16_t) (l * 32767.f);
			((int16_t*) buf)[f * 2 + 1] = (int16_t) (r * 32767.f);
		}
		e->pos += step;
	}
	// Drop the synth frames behind us.
	size_t drop = (size_t) e->pos;
	if (drop > 0 && drop <= e->buffer_len) {
		memmove(e->buffer, e->buffer + drop * 2, sizeof(int16_t) * 2 * (e->buffer_len - drop));
		e->buffer_len -= drop;
		e->pos -= (double) drop;
	}
	return bytes;
}
