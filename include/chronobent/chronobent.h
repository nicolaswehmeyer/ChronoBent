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
    CHRONOBENT_NOT_RESET = 6
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

#ifdef __cplusplus
}
#endif
#endif
