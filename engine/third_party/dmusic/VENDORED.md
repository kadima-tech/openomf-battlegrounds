GothicKit dmusic (DirectMusic style-segment player + DLS synthesizer), MIT licence (LICENSE.md).
Upstream: https://github.com/GothicKit/dmusic, commit b73b270b. include/ and src/ are copied from
it; the engine's CMakeLists.txt builds it as a static library.

Local changes:
- `src/Synth.c` is rewritten as a C port of Microsoft's DirectMusic software synthesizer, the
  "DirectMusic Software Synthesizer Sample" (DDKSynth) from the Windows Driver Kit 8.1 samples,
  MIT licence (LICENSE-DDKSynth.md), Copyright (c) Microsoft Corporation
  (https://github.com/microsoftarchive/msdn-code-gallery-microsoft). `src/_MsSynthTables.h` holds
  its lookup tables, copied from the sample. It replaces upstream's TinySoundFont-based
  synth, so `src/util/Tsf.c` and `vendor/TinySoundFont` are removed.
- `DmDlsWaveSample` records whether a `wsmp` chunk was present (`present`), so a region without
  one uses its wave's.
- `DmPerformance_playPattern` no longer cuts every sounding note and resets the synth when a pattern
  starts: the old pattern's pending note-ons and pattern change go (`DmMessageQueue_keepNoteOffs`),
  its note-offs stay, so notes that cross a pattern boundary play to their own ends, as DirectMusic's
  do.
- Style notes' `bTimeRange`, `bDurRange` and `bVelRange` randomise the start, duration and velocity
  by up to half the range either way (upstream added all three to the start time, always later).
- Style curves land on their end value at their end, and an instant or sub-5-tick curve sends its
  value once (upstream sent nothing for them: the theme's per-note volume accents were lost); a
  pitch-bend curve's reset value is the curve's.
- A band instrument's DX8 `nPitchBendRange` (flag 0x2000) is read and sets the channel's bend range
  (the theme's lead and pads use 12 semitones; upstream left 2).
- Note-on plays every region whose key and velocity ranges hold the note (DLS2 velocity ranges and
  layers), not DDKSynth's first region by key: the theme's instruments pick their samples by
  velocity (127, 126, 125, 124 and below). `OPENBG_MUSIC_SOLO=<pchannel>` mutes the other channels
  (debugging).
- A performance (and every new segment) starts on DirectMusic's default chord, C major (pattern
  0x91, scale 0xAB5AB5, chord root 12), not an all-zero one: the theme has no chord track, and its
  chord-relative parts (the pad from 16 s) played an octave low.
- `OPENBG_NO_LFO=1` turns the voices' LFO pitch modulation off (debugging).
- A band instrument finds its DLS instrument by program, bank LSB, bank MSB and drum flag (upstream
  compared only program and bank LSB, so the theme's drum kit resolved to a melodic instrument).
  `OPENBG_DEBUG_NOTES=<pchannel>` prints each note-on's key, velocity, region root and wave (debugging). `OPENBG_DEBUG_NOTES=all` prints every channel's.
- A stereo wave's regions (DLS2 `wlnk` multi-channel, channel 1 left or 2 right) each sound on their
  own side at full level, unpanned, as dmsynth does (re/FINDINGS 135; DDKSynth, DLS1, has no
  stereo waves; the theme's strings and some drums use them).
- Note-off releases every layer of the note-on it stops, as dmsynth does (each voice carries its
  note-on's serial); DDKSynth stopped one voice per note-off, leaving the other layers sounding.
- The DLS2 low-pass filter, as dmsynth does it (re/FINDINGS 150): a region's `art2` cutoff (0x500)
  and Q (0x501) pick a column and resonance row of the 2-pole coefficient tables, which the build reads
  from the game disc's dmsynth.dll (Microsoft's data, so not kept here: `re/scripts/dmsynth_filter_tables.py`
  writes `build/generated/_MsFilterTables.h`; without the disc the filter is left out), CC74
  moves the cutoff by (CC74 − 64) × 100 cents, and the coefficients ramp between control points
  like the volumes; the filter runs on the interpolated sample before the volume. The theme's
  `pad2` and Snakeirons' `seq` use it. LFO, EG2, velocity and key modulation of the cutoff are not
  ported (no instrument in the game uses them).
- `DmPerformance_loadBand`, `DmPerformance_applyBand`, `DmPerformance_releaseBand` (and a public
  `DmBand` type): a band from its RIFF, applied at once, for a segment without a band track (the
  game plays its container's band first; re/FINDINGS 150).
- Pattern selection as dmstyle does it (`FUN_7740b74f`, re/FINDINGS 151): no skip of one-measure
  groove patterns and no groove randomisation; criteria 2 → 5 on the longest patterns first, one
  time-signature group at a time, a random pick (`rand() % n`) among the survivors, the style's first
  pattern as the last resort. Executor, WTMI, SongNine and Snakeirons segments that were silent play.
  Not ported: criterion 1 (chord rhythm; no chord tracks), criterion 7 (destination groove, always
  1-100 in the data), repeat modes 1-5 (unused).
- Curves use dmime's 193-step LINEAR / SINE / LOG / EXP shapes (read from the game disc's dmime.dll
  at build time into `build/generated/_MsCurveTables.h`; without it every shape is linear) and send
  a value every max(16, duration × k / |Δ|) ticks (k 100 for pitch bend, 3 otherwise), landing on the
  end value; reset values are sent only when a segment is stopped within the curve's reset duration.
- Band items read DX8 `bd2h` and play at their physical time (the theme's second band, tick 380927).
