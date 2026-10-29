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
