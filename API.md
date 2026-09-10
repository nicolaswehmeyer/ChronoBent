# API reference

ChronoBent 0.3.0 provides a C ABI and a header-only C++ ownership wrapper.
The 0.1/0.2 C entry points, struct layouts and status numbers remain unchanged.
Link against the library built for the host architecture. The C++ standard
runtime is required even when the application calls the C API.

## Configuration and parameters

`chronobent_default_config(sample_rate, channels)` returns automatic window
selection, transient processing enabled and formants disabled. It does not
validate the supplied rate or channels; creation validates all fields.
`chronobent_default_parameters()` returns unity tempo/pitch, transients enabled
and formants disabled. Processor source/parameter calls use the parameters
argument for DSP options; config options initialize the underlying engines.

| Field | Contract |
| --- | --- |
| `sample_rate` | Finite 8000..192000 Hz, identical at input and output |
| `channels` | 1..8 phase-linked channels |
| `window_frames` | 0 for automatic, or a power of two from 512 through 8192 |
| `tempo` | Finite .25..4 input frames per output frame |
| `pitch` | Finite .5..2 output/input frequency ratio |
| `transients` | 0 for tonal processing, 1 for attack anchoring and onset resets |
| `formants` | 0 for shifted timbre, 1 for approximate spectral-envelope preservation |

All frame counts count multichannel frames, not individual samples or bytes.
Input length is limited to 2^48 frames. Samples are finite floats with absolute
value at most 64; nominal full scale is [-1,1]. Processing can exceed full scale.
No limiter is applied. Buffers must have space for the requested frame count.

## Extended controls and profiles

`chronobent_default_controls()` returns `{1, 1, 0, 2, CRISP, 0}`.
This is the extended `chronobent_controls` layout:

| Field | Contract |
| --- | --- |
| `tempo` | Finite .25..4 input frames per output frame |
| `pitch` | Finite .5..2 output/input frequency ratio |
| `formant_scale` | Zero follows pitch; otherwise finite .5..2 relative to the original source envelope |
| `envelope_ms` | Finite 1..4 ms cepstral extent; default 2; not processing latency |
| `transients` | `SMOOTH` (0), `CRISP` (1), or `MIXED` (2), with the `CHRONOBENT_TRANSIENT_` prefix |
| `reserved` | Must be zero |

An explicit formant scale of 1 preserves the source envelope. Equal pitch and
formant ratios need no envelope correction. Formant-only processing works at
unity pitch and tempo. Convert formant semitones with `exp2(st / 12.0)`.
The amplitude correction is bounded to 0.25..4 and cannot recover absent partials.
Larger `envelope_ms` retains finer envelope detail; a voice's harmonics can also
enter that estimate. This parameter does not identify or isolate vocals.

Smooth disables sparse-attack anchoring and onset resets. Crisp retains the
existing detector. Mixed uses that detector but suppresses onset resets at
spectral peaks below 500 Hz. Sparse-attack anchoring remains common to Crisp
and Mixed. No mode performs source separation or promises to suit every mix.

`chronobent_config_for_profile(rate, channels, profile, &config)` validates its
arguments and selects the smallest power-of-two window covering the profile's
nominal duration, bounded to 512..8192 frames. Output is unchanged on failure.

| Profile | Nominal duration | Window at 48 kHz | Window at 96 kHz |
| --- | --- | --- | --- |
| `CHRONOBENT_PROFILE_COMPACT` | 20 ms | 1024 | 2048 |
| `CHRONOBENT_PROFILE_BALANCED` | 40 ms | 2048 | 4096 |
| `CHRONOBENT_PROFILE_DETAILED` | 80 ms | 4096 | 8192 |

These select time/frequency resolution, not distinct DSP engines. Choose before
creation. A host changing profile creates another processor, transfers its
immutable source and seeks it before resuming playback. Fixed lookahead latency
is not implied by a window duration.

Use these additive C entry points:

- `chronobent_reset_controls(engine, frames, &controls)` for a fixed epoch.
- `chronobent_processor_set_source_controls(processor, reader, user, frames, &controls)` to bind a source.
- `chronobent_processor_set_controls(processor, &controls)` for a crossfaded control change.
- `chronobent_processor_get_controls(processor, &controls)` for the complete target settings.

They have the same ownership, allocation, `BUSY`, prefix and rollback contracts
as their earlier counterparts. All values are copied. Queries perform no DSP
or source reads and return `NOT_RESET` before binding. Output is untouched on
query error. Legacy `get_state` projects extended settings into its old layout:
`transients` is enabled for Crisp or Mixed and `formants` is enabled for any
explicit formant scale. Use `get_controls` to distinguish their exact values.

The legacy parameter setters reset advanced fields to their old equivalents:
2 ms envelope, Smooth/Crisp transients and either shifted or preserved formants.
Legacy `chronobent_reset` retains current options while replacing pitch/tempo.
Mixing setter families is allowed, but callers must account for that reset.

## Source ownership and reads

The callback signature is:

```c
int reader(void *user, uint64_t first_frame, size_t frames, float *interleaved);
```

It fills exactly `frames * channels` floats and returns nonzero on success.
It is called synchronously on the thread making the library call. Requests are
inside the declared source bounds but may overlap or revisit prior frames.
The source must support random access and remain immutable until it is replaced.
The library supplies zero padding before and after the source itself.

The processor retains `reader` and `user`, not the callback's output pointer.
The host owns source storage and callback context until a successful
`set_source` replaces them or `destroy` completes. No background activity
continues after a call returns. Synchronize externally if ownership moves to
another thread. Do not call back into the same processor from its reader.
Readers should return failure instead of throwing; a C++ exception escaping a
reader is converted to `SOURCE_UNAVAILABLE`.

Output must not alias source storage. Each planar output buffer must be
separate from the others. Both output formats use interleaved source reads.

## Processor lifecycle

`chronobent_processor_create(config, transition_frames, &processor)` allocates
two engines and scratch buffers. The transition length is 0..65536 output
frames; 1024 frames is about 21.3 ms at 48 kHz. On failure, the output handle
is null. `chronobent_processor_destroy(NULL)` is safe.

`chronobent_processor_set_source(processor, reader, user, input_frames, &p)`
binds the source, clears the previous transition and starts at frame zero.
It resets delivered-frame accounting. A nonempty source requires a reader;
empty input is valid and immediately ends. This call does not read the source.
Invalid arguments leave the current source and state intact.

`chronobent_processor_get_state(processor, &state)` returns:

| Field | Meaning |
| --- | --- |
| `input_frames` | Current source length |
| `delivered_frames` | Total frames returned since source binding, including before seeks |
| `source_position` | Nominal source position of the next rendered frame, clamped at source end |
| `parameters` | Current target trajectory, including during a fade |
| `transition_remaining` | Output frames left in the fade, or zero |
| `ended` | Nonzero once the target epoch has no more output |

This position describes rendered audio. A host queue or audio device may still
hold earlier frames. Store position metadata alongside queued audio to display
the audible source position. Seeking preserves `delivered_frames`; replacing
the source resets it. State output is untouched on error.

## Rendering and errors

`chronobent_processor_render(processor, output, requested, &produced)` writes
interleaved frames. `chronobent_processor_render_planar` takes an array of one
output pointer per channel. The two formats may be mixed between calls.
Arbitrary block partitions preserve output samples for the same control events
at the same delivered frame indices. Zero-frame requests do no DSP work and
allow null audio pointers; `produced` is always required.

Only the prefix of `produced` frames is written, including on errors. Tails are
untouched. Consume that prefix before retrying. The return status is:

| Status | Host action |
| --- | --- |
| `OK` | Continue; the request completed successfully |
| `END` | Consume any final prefix, then stop or seek/rebind |
| `INVALID_ARGUMENT` | Fix the arguments; invalid control requests retain the prior state |
| `OUT_OF_MEMORY` | Creation failed; release other resources or use a smaller configuration |
| `SOURCE_UNAVAILABLE` | Make the same source available, then retry; retain the produced prefix |
| `INVALID_AUDIO` | Reader returned nonfinite or excessive samples; correct the failed read before retrying |
| `NOT_RESET` | Bind a source, or reset a legacy epoch, before rendering |
| `BUSY` | A transition is active; keep the latest desired control and render until it finishes |

`chronobent_status_string(status)` returns a static description; unknown values
return `Unknown status`. Do not free that string. `chronobent_version()` returns
the static semantic version string.

An analysis frame commits only after a successful, valid read. During a fade,
the processor retains a sample already produced by one side if the other side
fails, so retry does not skip or duplicate either signal. Control-preroll errors
discard only the uncommitted candidate; current playback stays intact.

## Control changes and seek

`chronobent_processor_set_parameters(processor, &p)` prepares the unused engine
at the current source position and starts a linear crossfade. Tempo changes
use the new trajectory immediately; the previous trajectory contributes only
its fading audio. A zero transition length switches immediately. Identical
parameters succeed without another reset, including during a transition.
Other parameter changes return `BUSY` until the transition completes.

The new epoch begins up to four analysis windows earlier in the source, then
prerolls to the nearest output sample. Its sample alignment can differ from the
nominal position by at most `tempo / 2` input frames. This is not arbitrary
fractional-frame seek precision. Fixed-ratio playback from source zero returns
exactly `ceil(input_frames / tempo)` frames. After a seek or tempo change, the
remaining count follows the new epoch's origin, rounded preroll and output
length. Source position uses an absolute counter within each trajectory and
ends exactly at the declared source length.

`chronobent_processor_seek(processor, source_frame)` accepts an integer in
`[0,input_frames]`. It prerolls fresh history with the current parameters and
switches at the next render call. Seek is an explicit discontinuity, without a
crossfade; a host can mute or fade its queue around it. It returns `BUSY` during
an active parameter transition. Seeking to the end finishes the processor;
seeking back restarts it. Failed preroll leaves current playback intact.
Already queued output belongs to the host and must be discarded separately.

## Allocation, threads and latency

Creation/destruction allocate/free. The library makes no allocations and takes
no locks in render, source binding, seek or parameter changes. Working storage
depends on channels/window size and is independent of track duration. The
processor uses two engines even when transitions are disabled.

Control calls can regenerate sinc tables and preroll bounded source history.
Rendering also needs future source data. Allocation freedom alone is not a
deadline guarantee. Use a rendering worker and an output queue for audio-device
integration; the host owns callback cost, caching, scheduling and underruns.
The library performs no I/O and starts no threads. Live input without random
access is outside this API's contract.

## Fixed-epoch API

`chronobent_create` and `chronobent_destroy` own a single engine.
`chronobent_reset(engine, input_frames, tempo, pitch)` starts at source zero
with the current transient/formant options. `chronobent_reset_parameters`
also replaces those options. Both discard history on success and preserve it
on invalid arguments. A newly created engine requires reset before render.

`chronobent_render` accepts the reader and user context on every call; the host
must supply the same immutable source throughout the epoch. It has the same
prefix, retry and allocation contract as processor rendering.
`chronobent_output_frames` reports the epoch's full output length, not remaining
frames; `chronobent_window_frames` reports the resolved analysis window.
Null handles return zero for these queries. Pitch and tempo remain fixed until
another reset. Existing hosts can continue owning their own crossfades.

## C++ wrapper

Include [chronobent.hpp](include/chronobent/chronobent.hpp) and construct
`chronobent_cpp::Processor(config, transition_frames)`. It owns the C handle,
cannot be copied and can be moved. Construction throws `std::runtime_error`
on failure. `set_source`, `set_parameters`, `seek`, `render`, `render_planar`
`state`, `set_source_controls`, `set_controls` and `controls` return the C status and are `noexcept`. `get()` exposes the borrowed
C handle; do not destroy it independently. Calls on a moved-from object return
`INVALID_ARGUMENT` through its null handle.

Complete compiled examples are [C](examples/processor_c.c),
[C++](examples/processor_cpp.cpp), [WAV](examples/render_wav.cpp) and the
[worker/queue player](examples/pitch_lab/player.cpp).
