/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nicolas Wehmeyer */
#ifndef CHRONOBENT_TUNING_H
#define CHRONOBENT_TUNING_H
#include "chronobent.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Optional 0.6 module. Build with CHRONOBENT_BUILD_TUNING=ON (CMake default),
 * or add tuning-sources.txt to an existing source consumer. Legacy processor
 * structs, defaults and rendering do not depend on this module.
 * Recorded monophonic sources only: 1..2 linked channels, 8..192 kHz, <=600 s.
 * The caller owns immutable source PCM, I/O, workers and output storage.
 * One caller at a time per instance. All allocation is in create/destroy;
 * analysis, note edits and rendering use only preallocated derived state.
 * These are worker operations, not a live microphone/zero-lookahead contract. */
typedef struct chronobent_tuner chronobent_tuner;
typedef struct chronobent_tune_config {
    double sample_rate;
    uint32_t channels;
    double minimum_hz;       /* 40..400; default 55. */
    double maximum_hz;       /* >minimum_hz, <=1600; default 1200. */
} chronobent_tune_config;
typedef struct chronobent_tune_options {
    uint32_t scale_mask;     /* Absolute pitch classes: bit 0=C .. bit 11=B. Nonzero. */
    double reference_hz;     /* A4, 400..480; default 440. */
    double amount;           /* 0..1; zero preserves source samples exactly. */
    double retune_ms;        /* 0..400; correction-curve smoothing, not audio latency. */
    double preserve_vibrato; /* 0..1; one retains the fast part of the input contour. */
    double correct_drift;    /* 0..1; one also corrects slow within-note drift. */
    double maximum_shift;    /* 0..5 semitones; default 2. */
} chronobent_tune_options;
typedef struct chronobent_tune_frame {
    uint64_t source_frame;
    double frequency_hz;     /* Zero for unvoiced/uncertain frames. */
    double confidence;       /* Periodicity indicator, not a calibrated probability. */
    double correction_st;    /* Planned shift; zero outside admitted voiced spans. */
} chronobent_tune_frame;
typedef struct chronobent_tune_note {
    uint64_t first_frame, end_frame; /* Half-open source range. */
    double detected_midi, target_midi, confidence;
    uint32_t manual;         /* One if explicitly edited. */
} chronobent_tune_note;
typedef struct chronobent_tune_edit { size_t note_index; double target_midi; } chronobent_tune_edit;
/* Return nonzero to continue. Invoked synchronously at bounded intervals on the
 * calling worker. Returning zero aborts analysis with CHRONOBENT_CANCELLED.
 * Do not throw, reenter or destroy the tuner in this callback. */
typedef int (*chronobent_tune_progress_fn)(void *user, double fraction);

CHRONOBENT_API chronobent_tune_config chronobent_tune_default_config(double rate, uint32_t channels);
CHRONOBENT_API chronobent_tune_options chronobent_tune_default_options(void);
CHRONOBENT_API chronobent_status chronobent_tune_validate_options(const chronobent_tune_options *);
CHRONOBENT_API chronobent_status chronobent_tune_create(const chronobent_tune_config *,
    uint64_t input_frames, chronobent_tuner **);
CHRONOBENT_API void chronobent_tune_destroy(chronobent_tuner *);
/* After valid arguments, starting analysis invalidates the preceding analysis.
 * Failure/cancellation never exposes partial results; retry analyzes from zero.
 * The reader contract is the same immutable absolute-index contract as the DSP. */
CHRONOBENT_API chronobent_status chronobent_tune_analyze(chronobent_tuner *,
    chronobent_read_fn, void *source, chronobent_tune_progress_fn, void *progress_user);
/* Borrowed immutable views, valid until the next analysis/edit or destruction.
 * A null/unanalyzed instance returns NULL with count zero. Do not read views
 * concurrently with operations on their tuner; hosts publish their own copies. */
CHRONOBENT_API const chronobent_tune_frame *chronobent_tune_frames(const chronobent_tuner *, size_t *count);
CHRONOBENT_API const chronobent_tune_note *chronobent_tune_notes(const chronobent_tuner *, size_t *count);
CHRONOBENT_API chronobent_status chronobent_tune_set_options(chronobent_tuner *, const chronobent_tune_options *);
CHRONOBENT_API chronobent_status chronobent_tune_get_options(const chronobent_tuner *, chronobent_tune_options *);
/* target_midi in [0,127], within 5 st of detection. Use -1 to return this note
 * to automatic scale selection. Invalid edits leave the current plan intact. */
CHRONOBENT_API chronobent_status chronobent_tune_set_note(chronobent_tuner *, size_t index, double target_midi);
/* Atomically replace all manual edits in one plan pass. Unlisted notes revert
 * to automatic. Indices must be strictly increasing; targets use set_note bounds
 * (without -1). Null edits is valid only with count zero. */
CHRONOBENT_API chronobent_status chronobent_tune_set_notes(chronobent_tuner *,
    const chronobent_tune_edit *edits, size_t count);
/* Absolute output indexing: duration is exactly input_frames, independent of
 * block partition, seek order or earlier render calls. Output beyond EOF is not
 * written. On source error, produced names the valid prefix; retry at
 * first_frame+produced. Reader/PCM identity must match the analyzed source.
 * No source binding or storage is retained by the library. */
CHRONOBENT_API chronobent_status chronobent_tune_render(chronobent_tuner *,
    chronobent_read_fn, void *source, uint64_t first_frame, float *interleaved,
    size_t frames, size_t *produced);

#ifdef __cplusplus
}
#endif
#endif
