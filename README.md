# ChronoBent

An independent C++17 pitch and time engine with a small C API and an MIT license.
**0.1.0 is the first experimental release.** Music listening and target-device
qualification are still required; synthetic accuracy is not a perceptual
quality rating. The API may change before 1.0.

The implementation has no external DSP dependency. It supports independent
pitch and tempo, linked mono/stereo/multichannel processing, sparse-attack timing
and optional approximate formant preservation. The FFT, phase processing,
envelope estimator and resampler are separate, newly authored compilation units.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake 3.16+, a C/C++17 compiler and a C++ standard library are required. Tests
of the WAV example additionally use Python 3's standard library. There are no
network downloads. Set `CHRONOBENT_BUILD_TESTS=OFF` and
`CHRONOBENT_BUILD_EXAMPLES=OFF` for the library alone. `BUILD_SHARED_LIBS=ON` builds
a shared library; the public C functions are the only exported DSP interface.
`CHRONOBENT_SANITIZE=ON` enables ASan/UBSan with Clang/GCC.

```sh
cmake --install build --prefix /your/install/prefix
```

An independent CMake consumer can use `find_package(chronobent CONFIG REQUIRED)`
and link `chronobent::chronobent`. Enable the C++ language/linker even for C callers
of a static build. `sources.txt` also lists all implementation units for build
systems that compile source directly; `sources.mk` consumes that same list.
The pre-1.0 public API has no stable ABI promise yet.

## Ratios and processing

- `tempo = input frames / output frame`: 1 is unchanged, 2 is twice as fast.
- `pitch = output frequency / input frequency`: 2 is one octave up.
- For semitones, use `pitch = exp2(semitones / 12.0)`.
- Tempo is 0.25–4, pitch is 0.5–2, sample rate is 8–192 kHz, channels are 1–8.
- Output length is exactly `ceil(input_frames / tempo)`. Unity is an exact copy.

[The C header](include/chronobent/chronobent.h) specifies all ownership, error and
thread contracts. The caller supplies a synchronous read callback over an
immutable interleaved float source. The renderer may request overlapping ranges,
look ahead or revisit earlier frames. It clips reads to the declared source and
pads outside it with zeros. No file, thread, device or UI belongs to the DSP.

A typical C call sequence is:

```c
#include <chronobent/chronobent.h>

chronobent_config config = {48000, 2, 0, 1, 0};
chronobent *engine = NULL;
if (chronobent_create(&config, &engine) == CHRONOBENT_OK) {
    if (chronobent_reset(engine, input_frames, 1.0, pitch_ratio) == CHRONOBENT_OK) {
        size_t produced = 0;
        chronobent_status status = chronobent_render(engine, reader, user,
                                                 output, requested, &produced);
        /* Consume only produced frames, including on END or a source error.
           Repeat until END; handle errors instead of assuming a full block. */
        (void)status;
    }
    chronobent_destroy(engine);
}
```

The snippets belong inside a caller providing `reader`, `user`, the source
length, ratios and an output buffer. See [the WAV example](examples/render_wav.cpp)
for a complete program and [the C consumer](tests/test_c.c) for a minimal test.

Allocation happens at create/destroy. Reset clears history and prepares bounded
filter/window tables without allocation. Render performs no allocation, locks
or I/O; the callback's cost and realtime suitability are the caller's
responsibility. An instance has one calling thread at a time. There is no
shared mutable DSP state. Memory is bounded by channel/window configuration,
independent of track duration.

This is a **pull renderer for accessible source audio**, with compensated
startup/tail timing. It is not a zero-lookahead live-input processor. Ratios
stay constant for each reset/render epoch. For continuous controls, run
successive epochs at corresponding source positions and crossfade their output.
Resetting a live instance without a host transition can click.

## Master Tempo in a host

Use `chronobent_reset(engine, frames, speed, 1.0)` to change speed while keeping
the source pitch. For example, speed `1.25` produces 80% of the original
duration at the original key. To set an independent key, supply
`exp2(semitones / 12.0)` for pitch. Turntable-style playback uses equal tempo
and pitch ratios. The optional [player example](examples/pitch_lab/player.hpp)
adds continuous controls with worker-side preroll and crossfades.

## Audition

Render a WAV without changing its tempo, up one semitone:

```sh
build/chronobent-render input.wav shifted.wav 1 1.059463094
```

The example accepts RIFF PCM16/24/32 or float32 WAV and writes float32 WAV.
Optional arguments select `tonal`/`transients` and `shift`/`preserve` formants.
It refuses existing output paths and malformed input. It loads the input into
memory; this example's memory cost is separate from the bounded DSP contract.

On macOS, build the native audition app:

```sh
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCHRONOBENT_BUILD_PITCH_LAB=ON
cmake --build build-mac --parallel
open 'build-mac/ChronoBent Lab.app'
```

Open a local audio file and press Play. The speed fader spans 50–200%.
**Master Tempo is on by default:** speed changes preserve the original key
when the pitch fader is at zero. The independent ±12-semitone fader sets a
different key while retaining the selected tempo. Turn Master Tempo off for
turntable-style playback, where pitch follows speed; the pitch/formant controls
then become inactive. Bypass returns to original pitch and speed. All parameter
changes use a 1024-frame crossfade, and the time display follows source position. Playback pauses without advancing the source;
Restart returns to the beginning. The output uses equal −3.1 dB headroom in
processed and bypass modes. The status line reports queue depth and underruns.

The Mac app uses Cocoa and AVFoundation system frameworks. It decodes mono or
stereo files at 8–192 kHz, up to 64 million frames, and keeps decoded PCM in
memory. Its audio callback consumes a 4096-frame SPSC queue; DSP and transition
preroll run on a worker. Fader response includes this queue, a crossfade and
system output latency. Paused queued audio can contain the previous setting
for up to one queue after resuming. It performs no recording or uploads.

## Quality and scope

The synthetic tests check pitch frequency, exact unity/duration, arbitrary block
sizes, anti-phase and independent channels, silence, sparse off-grid attacks,
a mixed percussive stress signal, one out-of-band alias test, a synthetic vowel,
source failure/retry and reset isolation. The optional Python standard-library
measurement harness generates tones, clicks, colored noise, a sweep and a
percussive harmonic mixture; see [QUALITY.md](QUALITY.md). The independent FFT oracle computes
a direct DFT. Allocation traps check reset/render. The app tests cover queue
ownership, fractional pitch and speed changes, Master Tempo, bypass, pause/end, mixed
16–4096-frame callbacks and teardown. These are
regression tests with stated tolerances, not proof over all music.

Known limits include phase-vocoder texture on dense polyphonic material,
ambiguous attacks in mixtures, possible timbre changes around attack admission,
short-window bass resolution and difficult extreme ratios. Sparse-attack
anchoring is not source separation. Formant preservation adjusts a smooth
spectral envelope; it is not a vocal model and can affect non-vocal instruments.
No universal loudness, true-peak or alias-floor guarantee is made. Hosts should
provide appropriate output headroom.

`chronobent-benchmark` prints CPU time relative to output duration, time to the
first 4096 frames and maximum observed render-block time for synthetic material.
Run it on the actual target, under representative contention, before choosing
a queue budget. A fast development machine does not establish embedded deadlines.
See [DESIGN.md](DESIGN.md) for the signal path and remaining quality work.

## Source export

`export-files.txt` is the explicit source-package inventory. With Gitleaks
installed, `python3 tools/export_source.py /path/to/new.zip` creates a deterministic
source-only archive after scanning its exact contents. It includes no repository
history, binaries, audio, build directories or consumer integration. An existing
archive is refused. Review the selected source before publishing. The official repository is
[nicolaswehmeyer/ChronoBent](https://github.com/nicolaswehmeyer/ChronoBent).
