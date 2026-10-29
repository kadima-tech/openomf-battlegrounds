// Copyright © 2024. GothicKit Contributors
// SPDX-License-Identifier: MIT-Modern-Variant
#include "_Internal.h"

#include <stdlib.h>

DmResult DmStyle_create(DmStyle** slf) {
	if (slf == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	DmStyle* new = *slf = Dm_alloc(sizeof *new);
	if (new == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}

	new->reference_count = 1;

	DmBandList_init(&new->bands);
	DmPartList_init(&new->parts);
	DmPatternList_init(&new->patterns);

	return DmResult_SUCCESS;
}

DmStyle* DmStyle_retain(DmStyle* slf) {
	if (slf == NULL) {
		return NULL;
	}

	(void) atomic_fetch_add(&slf->reference_count, 1);
	return slf;
}

void DmStyle_release(DmStyle* slf) {
	if (slf == NULL) {
		return;
	}

	size_t refs = atomic_fetch_sub(&slf->reference_count, 1) - 1;
	if (refs > 0) {
		return;
	}

	DmPatternList_free(&slf->patterns);
	DmBandList_free(&slf->bands);
	DmPartList_free(&slf->parts);
	Dm_free(slf->backing_memory);
	Dm_free(slf);
}

DmResult DmStyle_download(DmStyle* slf, DmLoader* loader) {
	if (slf == NULL || loader == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	DmResult rv = DmResult_SUCCESS;
	for (size_t i = 0; i < slf->bands.length; ++i) {
		rv = DmBand_download(slf->bands.data[i], loader);
		if (rv != DmResult_SUCCESS) {
			break;
		}
	}

	return rv;
}

DmPart* DmStyle_findPart(DmStyle* slf, DmPartReference* pref) {
	if (slf == NULL || pref == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < slf->parts.length; ++i) {
		if (DmGuid_equals(&slf->parts.data[i].part_id, &pref->part_id)) {
			return &slf->parts.data[i];
		}
	}

	return NULL;
}

enum {
	DmPatternTag_ALIVE = 0,
	DmPatternTag_DROPPED = -1,
	DmPatternTag_LENGTH = 10,
	DmPatternTag_SIGNATURE = 11,
	DmPattern_MOTIF = 0x10,
};

static DmTimeSignature DmPattern_getTimeSignature(DmPattern const* slf, DmStyle const* sty) {
	return slf->time_signature.beat != 0 ? slf->time_signature : sty->time_signature;
}

// dmstyle FUN_77407b9e
static bool DmPattern_matchesGroove(DmPattern const* slf, DmPatternCommand const* cmd) {
	if (slf->embellishment & DmPattern_MOTIF) {
		return false;
	}

	uint32_t half = cmd->groove_range >> 1;
	uint32_t lo = cmd->groove_level < half ? 1 : cmd->groove_level - half;
	uint32_t hi = cmd->groove_level + half > 100 ? 100 : cmd->groove_level + half;
	return hi >= slf->groove_bottom && lo <= slf->groove_top;
}

// dmstyle FUN_77407be5
static bool DmPattern_matchesEmbellishment(DmPattern const* slf, DmPatternCommand const* cmd) {
	uint16_t emb = slf->embellishment;
	if (emb & DmPattern_MOTIF) {
		return false;
	}

	if ((emb >> 8) != 0) {
		return (emb >> 8) == cmd->command;
	}

	switch (cmd->command) {
	case DmCommand_GROOVE:
		return emb == 0;
	case DmCommand_FILL:
		return (emb & 1) != 0;
	case DmCommand_INTRO:
		return (emb & 4) != 0;
	case DmCommand_BREAK:
		return (emb & 2) != 0;
	case DmCommand_END:
		return (emb & 8) != 0;
	default:
		return false;
	}
}

// dmstyle FUN_77408c2a: a measure that only continues the first measure's groove needs no match.
static bool DmPattern_matchesCommands(DmPattern const* slf, DmPatternCommand const* cmds, uint32_t len) {
	if (slf->embellishment & DmPattern_MOTIF) {
		return false;
	}

	uint32_t n = len < slf->length_measures ? len : slf->length_measures;
	for (uint32_t i = 0; i < n; ++i) {
		bool continues = i > 0 && cmds[i].command == DmCommand_GROOVE && cmds[i].groove_level == cmds[0].groove_level;
		if (!continues && !(DmPattern_matchesEmbellishment(slf, &cmds[i]) && DmPattern_matchesGroove(slf, &cmds[i]))) {
			return false;
		}
	}

	return true;
}

static bool DmStyle_anyAlive(int8_t const* tags, size_t len) {
	for (size_t i = 0; i < len; ++i) {
		if (tags[i] == DmPatternTag_ALIVE) {
			return true;
		}
	}
	return false;
}

// dmstyle FUN_774061a2
static void DmStyle_restoreTag(int8_t* tags, size_t len, int8_t tag) {
	for (size_t i = 0; i < len; ++i) {
		if (tags[i] == tag) {
			tags[i] = DmPatternTag_ALIVE;
		} else if (tags[i] == DmPatternTag_ALIVE) {
			tags[i] = DmPatternTag_DROPPED;
		}
	}
}

// dmstyle FUN_7740b74f: criteria 2..5 (1 is 2 without a chord rhythm), each tried on the longest
// patterns first, one time-signature group at a time; the survivors' pick is FUN_7741619d.
DmPattern* DmStyle_getRandomPattern(DmStyle* slf, DmPatternQuery const* query) {
	if (slf == NULL || slf->patterns.length == 0) {
		return NULL;
	}

	size_t len = slf->patterns.length;
	uint32_t longest = 0;
	for (size_t i = 0; i < len; ++i) {
		if (slf->patterns.data[i].length_measures > longest) {
			longest = slf->patterns.data[i].length_measures;
		}
	}

	if (longest == 0) {
		return NULL;
	}

	int8_t* tags = Dm_alloc(len);
	DmPatternCommand* cmds = Dm_alloc(sizeof *cmds * longest);
	if (tags == NULL || cmds == NULL) {
		Dm_free(tags);
		Dm_free(cmds);
		return &slf->patterns.data[0];
	}

	uint32_t cmds_len = 1;
	static int8_t const CRITERIA[] = {2, 3, 4, 5};
	for (size_t c = 0; c < sizeof CRITERIA; ++c) {
		int8_t criterion = CRITERIA[c];
		memset(tags, DmPatternTag_ALIVE, len);

		for (;;) {
			uint32_t max_measures = 0;
			for (size_t i = 0; i < len; ++i) {
				if (tags[i] == DmPatternTag_ALIVE && slf->patterns.data[i].length_measures > max_measures) {
					max_measures = slf->patterns.data[i].length_measures;
				}
			}
			for (size_t i = 0; i < len; ++i) {
				if (tags[i] == DmPatternTag_ALIVE && slf->patterns.data[i].length_measures != max_measures) {
					tags[i] = DmPatternTag_LENGTH;
				}
			}

			for (;;) {
				// The longest measure (beats / beat), then the most beats.
				DmTimeSignature best = {0};
				for (size_t i = 0; i < len; ++i) {
					if (tags[i] != DmPatternTag_ALIVE) continue;
					DmTimeSignature sig = DmPattern_getTimeSignature(&slf->patterns.data[i], slf);
					uint32_t lhs = (uint32_t) sig.beats_per_measure * best.beat;
					uint32_t rhs = (uint32_t) best.beats_per_measure * sig.beat;
					if (lhs > rhs || (lhs == rhs && sig.beats_per_measure > best.beats_per_measure)) {
						best = sig;
					}
				}

				DmPattern* first = NULL;
				for (size_t i = 0; i < len; ++i) {
					if (tags[i] != DmPatternTag_ALIVE) continue;
					DmTimeSignature sig = DmPattern_getTimeSignature(&slf->patterns.data[i], slf);
					if (sig.beats_per_measure != best.beats_per_measure || sig.beat != best.beat) {
						tags[i] = DmPatternTag_SIGNATURE;
					} else if (first == NULL) {
						first = &slf->patterns.data[i];
					}
				}

				uint32_t measure = first ? Dm_getMeasureLength(DmPattern_getTimeSignature(first, slf)) : 0;

				// FUN_77416235: measures left before the next command, rounded up for DX8 styles.
				uint32_t measures_left =
				    measure == 0 ? 0 : (query->until_command + measure - 1) / measure;

				if (query->until_end != 0 && measure != 0) {
					cmds_len = query->until_end / measure;
					if (cmds_len == 0) cmds_len = 1;
				}
				if (cmds_len > longest) cmds_len = longest;
				query->commands(query->context, measure, cmds_len, cmds);

				for (size_t i = 0; i < len; ++i) {
					if (tags[i] != DmPatternTag_ALIVE) continue;
					DmPattern const* p = &slf->patterns.data[i];

					bool ok = false;
					switch (criterion) {
					case 2:
						ok = DmPattern_matchesCommands(p, cmds, cmds_len) &&
						    !(cmds_len != 0 && cmds_len < p->length_measures) &&
						    !(measures_left != 0 && measures_left < p->length_measures);
						break;
					case 3:
						ok = DmPattern_matchesCommands(p, cmds, 1);
						break;
					case 4:
						ok = DmPattern_matchesEmbellishment(p, &cmds[0]);
						break;
					case 5:
						ok = DmPattern_matchesGroove(p, &cmds[0]) && p->embellishment == 0;
						break;
					}

					if (!ok) {
						tags[i] = criterion;
					}
				}

				if (DmStyle_anyAlive(tags, len)) {
					goto found;
				}

				DmStyle_restoreTag(tags, len, DmPatternTag_SIGNATURE);
				if (!DmStyle_anyAlive(tags, len)) break;
			}

			DmStyle_restoreTag(tags, len, DmPatternTag_LENGTH);
			if (!DmStyle_anyAlive(tags, len)) break;
		}
	}

	Dm_free(tags);
	Dm_free(cmds);
	return &slf->patterns.data[0];

found:;
	// Repeat modes other than RANDOM (FUN_7740b58e) are not used by the game's data.
	uint32_t alive = 0;
	for (size_t i = 0; i < len; ++i) {
		alive += tags[i] == DmPatternTag_ALIVE;
	}

	uint32_t pick = Dm_rand() % alive;
	DmPattern* pttn = NULL;
	for (size_t i = 0; i < len; ++i) {
		if (tags[i] == DmPatternTag_ALIVE && pick-- == 0) {
			pttn = &slf->patterns.data[i];
			break;
		}
	}

	Dm_free(tags);
	Dm_free(cmds);
	return pttn;
}

void DmPart_init(DmPart* slf) {
	if (slf == NULL) {
		return;
	}

	memset(slf, 0, sizeof *slf);
}

void DmPart_free(DmPart* slf) {
	if (slf == NULL) {
		return;
	}

	Dm_free(slf->notes);
	Dm_free(slf->curves);
}

uint32_t DmPart_getValidVariationCount(DmPart* slf) {
	if (slf == NULL) {
		return 0;
	}

	uint32_t i = 0;
	for (; i < 32; ++i) {
		if ((slf->variation_choices[i] & 0x0FFFFFFF) == 0) {
			break;
		}
	}

	return i;
}

void DmPartReference_init(DmPartReference* slf) {
	if (slf == NULL) {
		return;
	}

	memset(slf, 0, sizeof *slf);
}

void DmPartReference_free(DmPartReference* slf) {
	(void) slf;
}

void DmPattern_init(DmPattern* slf) {
	if (slf == NULL) {
		return;
	}

	memset(slf, 0, sizeof *slf);
	DmPartReferenceList_init(&slf->parts);
}

void DmPattern_free(DmPattern* slf) {
	if (slf == NULL) {
		return;
	}

	DmPartReferenceList_free(&slf->parts);
	Dm_free(slf->rhythm);
	slf->rhythm = NULL;
	slf->rhythm_len = 0;
}
