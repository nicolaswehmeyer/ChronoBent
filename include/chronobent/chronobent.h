/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nicolas Wehmeyer */
#ifndef CHRONOBENT_H
#define CHRONOBENT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(CHRONOBENT_SHARED)
# if defined(CHRONOBENT_BUILD)
#  define CHRONOBENT_API __declspec(dllexport)
# else
#  define CHRONOBENT_API __declspec(dllimport)
# endif
#elif defined(CHRONOBENT_SHARED) && defined(__GNUC__)
# define CHRONOBENT_API __attribute__((visibility("default")))
#else
# define CHRONOBENT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chronobent chronobent;

typedef enum chronobent_status {
    CHRONOBENT_OK = 0,
    CHRONOBENT_END = 1,
    CHRONOBENT_INVALID_ARGUMENT = 2,
    CHRONOBENT_OUT_OF_MEMORY = 3,
    CHRONOBENT_SOURCE_UNAVAILABLE = 4,
    CHRONOBENT_INVALID_AUDIO = 5,
    CHRONOBENT_NOT_RESET = 6,
    CHRONOBENT_BUSY = 7
} chronobent_status;

typedef struct chronobent_config {
    double sample_rate;     /* 8000..192000 Hz, input and output use this rate. */
    uint32_t channels;      /* 1..8 interleaved, phase-linked channels. */
    uint32_t window_frames; /* 0 = automatic; otherwise power of two, 512..8192. */
    uint32_t transients;    /* 0 = tonal; 1 = sparse-attack anchoring and onset resets. */
    uint32_t formants;      /* 0 = shift timbre with pitch; 1 = approximate envelope preservation. */
} chronobent_config;

/* Called synchronously on the rendering thread. Fill exactly frames*channels
 * interleaved finite floats, nominally [-1,1], absolute value <=64.
 * Return nonzero on success.
 * The library clips requests to [0,input_frames), handles zero padding, and
 * never retains this pointer. Requests may overlap or revisit earlier frames.
 * Do not change source contents during a reset/render epoch. A failed read
 * may be retried with the same epoch. Do not throw or reenter the same instance.
 * The callback owns source lifetime and realtime suitability. */
typedef int (*chronobent_read_fn)(void *user, uint64_t first_frame,
                               size_t frames, float *interleaved);

/* One instance has one calling thread; synchronize externally to transfer it.
 * create/destroy own all allocation. No library I/O, threads or global DSP state.
 * A successful create still requires reset before render. */
CHRONOBENT_API chronobent_status chronobent_create(const chronobent_config *config,
                                            chronobent **instance);
CHRONOBENT_API void chronobent_destroy(chronobent *instance);

/* Starts a new epoch at source frame zero. tempo = input frames/output frame;
 * pitch = output frequency/input frequency. Both are finite: tempo .25..4,
 * pitch .5..2. Output length is ceil(input_frames/tempo), input_frames <= 2^48.
 * Ratios are constant for an epoch. Reset discards pending audio and phase
 * history; a host changing ratios during playback supplies its own crossfade.
 * Reset performs bounded table generation but allocates nothing. An invalid
 * reset leaves the previous epoch intact. Empty input is supported. */
CHRONOBENT_API chronobent_status chronobent_reset(chronobent *instance,
    uint64_t input_frames, double tempo, double pitch);

/* Produces up to frames interleaved output frames. produced is mandatory and
 * always set when nonnull. Only [0,*produced) is written, even on source error.
 * frames may be zero; otherwise reader and output must be nonnull. Output must
 * not alias source data. Arbitrary block sizes produce identical samples.
 * Returns END at the declared duration (possibly with a final partial block).
 * Source failure/invalid audio leaves the uncommitted analysis frame retryable;
 * retain already-produced output and resume. No allocations or locks here.
 * Startup/lookahead and tails are compensated in the returned sample timeline;
 * this is a bounded-memory pull renderer, not a zero-lookahead live-input API. */
CHRONOBENT_API chronobent_status chronobent_render(chronobent *instance,
    chronobent_read_fn reader, void *user, float *output,
    size_t frames, size_t *produced);

CHRONOBENT_API uint64_t chronobent_output_frames(const chronobent *instance);
CHRONOBENT_API uint32_t chronobent_window_frames(const chronobent *instance);
CHRONOBENT_API const char *chronobent_version(void);

/* Additive 0.2 API. The original config layout and status values are unchanged. */
typedef struct chronobent_parameters {
    double tempo;          /* .25..4 input frames per output frame. */
    double pitch;          /* .5..2 frequency ratio; exp2(semitones/12). */
    uint32_t transients;   /* 0 or 1. */
    uint32_t formants;     /* 0 or 1. */
} chronobent_parameters;

CHRONOBENT_API chronobent_config chronobent_default_config(double sample_rate, uint32_t channels);
CHRONOBENT_API chronobent_parameters chronobent_default_parameters(void);
CHRONOBENT_API const char *chronobent_status_string(chronobent_status status);
/* Like reset, also replaces the transient/formant options for this epoch. */
CHRONOBENT_API chronobent_status chronobent_reset_parameters(chronobent *instance,
    uint64_t input_frames, const chronobent_parameters *parameters);

typedef struct chronobent_processor chronobent_processor;
typedef struct chronobent_processor_state {
    uint64_t input_frames;
    uint64_t delivered_frames; /* Cumulative since set_source; seek does not clear it. */
    double source_position;    /* Nominal next source position, clamped at end. */
    chronobent_parameters parameters; /* Target trajectory, including during fade. */
    uint32_t transition_remaining;
    uint32_t ended;
} chronobent_processor_state;

/* A source-bound worker-side renderer with two preallocated engines. One calling
 * thread, no reentry. The same source/read lifetime contract as render applies.
 * transition_frames is 0..65536; 1024 is a useful starting point. create/destroy
 * allocate/free; all other processor calls allocate nothing and take no locks.
 * Control calls can synchronously read/preroll up to four analysis windows of
 * source history and regenerate filter tables. Run them on a rendering worker,
 * not a time-critical audio callback. No implicit threads or output queue. */
CHRONOBENT_API chronobent_status chronobent_processor_create(const chronobent_config *config,
    uint32_t transition_frames, chronobent_processor **instance);
CHRONOBENT_API void chronobent_processor_destroy(chronobent_processor *instance);
/* Replaces the source and discards any transition. Reader/user must remain valid
 * until another successful set_source or destroy. Caller retains ownership.
 * Empty sources are supported; nonempty sources need a reader. No read here.
 * Invalid arguments leave the current source and state intact. */
CHRONOBENT_API chronobent_status chronobent_processor_set_source(chronobent_processor *instance,
    chronobent_read_fn reader, void *user, uint64_t input_frames,
    const chronobent_parameters *parameters);
/* Changes pitch, tempo and options at the next rendered output frame. A linear
 * crossfade uses the new tempo trajectory immediately. Alignment to the new
 * epoch's nearest sample differs by at most tempo/2 input frames. Successful
 * calls invalidate no source data. Failed preroll leaves the audible state
 * intact and may be retried. BUSY means a previous fade must first be rendered;
 * identical target parameters succeed without restarting it. */
CHRONOBENT_API chronobent_status chronobent_processor_set_parameters(chronobent_processor *instance,
    const chronobent_parameters *parameters);
/* Explicit discontinuity at an integer source frame in [0,input_frames]. Keeps
 * target parameters, cancels any fade on success. Failed preroll leaves current
 * playback intact. During a fade returns BUSY; render it or replace the source.
 * A host must separately discard its queued output when seeking. */
CHRONOBENT_API chronobent_status chronobent_processor_seek(chronobent_processor *instance,
    uint64_t source_frame);
/* Same produced/prefix/retry contract as render. Interleaved and planar calls
 * can be mixed without changing samples. Planar needs one distinct writable
 * channel buffer per configured channel, each holding frames floats. Buffers
 * must not alias one another or source data. Unproduced tails are untouched. */
CHRONOBENT_API chronobent_status chronobent_processor_render(chronobent_processor *instance,
    float *output, size_t frames, size_t *produced);
CHRONOBENT_API chronobent_status chronobent_processor_render_planar(chronobent_processor *instance,
    float *const *output, size_t frames, size_t *produced);
/* No read or DSP work. NOT_RESET before set_source; output is unchanged on error. */
CHRONOBENT_API chronobent_status chronobent_processor_get_state(const chronobent_processor *instance,
    chronobent_processor_state *state);

#ifdef __cplusplus
}
#endif
#endif
