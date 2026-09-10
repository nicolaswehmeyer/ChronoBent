<div align="center">

# ChronoBent

**Pitch, tempo and timbre. Independently controlled.**

An MIT-licensed C++17 audio library with a C ABI, a C++ wrapper and a native macOS lab.

[Get started](#build) · [API reference](API.md) · [Download Lab](https://github.com/nicolaswehmeyer/ChronoBent/releases/tag/v0.2.0) · [Quality and limits](QUALITY.md)

</div>

## What you can build

A player that changes speed while keeping its key. An editor that transposes
vocals while controlling their formants. A renderer that processes audio with
bounded working memory. ChronoBent supplies the DSP and source timeline;
your application owns storage, threads and audio devices.

| Control | Capability |
| --- | --- |
| Tempo | 0.25 to 4 times speed, independent of pitch |
| Pitch | One octave down to one octave up |
| Formants | Preserve the original envelope or shift it independently by up to an octave |
| Transients | Smooth, Crisp and Mixed handling |
| Analysis | Compact, Balanced and Detailed window profiles |
| Channels | 1 to 8 with linked phase processing |
| Playback | Seek, parameter crossfades, planar or interleaved output |
| Integration | C ABI, move-only C++ wrapper, CMake package, static or shared library |

**0.3.0 is experimental.** Its processing contracts have automated tests, but
musical transparency is not established across all material and settings.
The source must support random-access reads. This is not a live-input effect.

## New in 0.3.0

- Independent formant scaling, adjustable envelope resolution and Mixed transients.
- Named analysis profiles and additive C/C++ controls. Earlier C layouts and entry points remain compatible.
- An optimized FFT. Five alternating runs on an Apple M5 showed 8.18 to 11.54% lower
  median render time than 0.2.0 across ten stereo benchmark cases.
- A 162-render diagnostic matrix, alongside existing DSP, lifecycle and allocation tests.
- A redesigned Lab with waveform overview, track seeking, keyboard transport,
  independent timbre controls, analysis selection and adjustable output gain.

See [the changelog](CHANGELOG-DSP.md) and [measurement scope](QUALITY.md).
The performance result describes those inputs and that machine.

## Build

Requires CMake 3.16+, a C/C++17 compiler and the C++ standard library.
The DSP has no third-party runtime dependencies. No dependencies are downloaded
by the build. The default test selection also uses Python 3.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix /your/install/prefix
```

Link from your own CMake project:

```cmake
find_package(chronobent CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE chronobent::chronobent)
```

Static C consumers must enable the C++ linker. For other build systems,
[sources.txt](sources.txt) and [sources.mk](sources.mk) list the DSP units.
The package is tested with static/shared builds on Linux, macOS and Windows.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` | Shared library with exported C symbols |
| `CHRONOBENT_BUILD_TESTS` | `ON` | DSP, API, player and renderer regressions |
| `CHRONOBENT_BUILD_EXAMPLES` | `ON` | WAV renderer, benchmark and C/C++ consumers |
| `CHRONOBENT_BUILD_PITCH_LAB` | `OFF` | Native macOS audition app |
| `CHRONOBENT_SANITIZE` | `OFF` | Address and undefined-behavior sanitizers |

## C++ integration

```cpp
#include <chronobent/chronobent.hpp>

chronobent_config config{};
auto status = chronobent_config_for_profile(
    48000, 2, CHRONOBENT_PROFILE_BALANCED, &config);
if (status != CHRONOBENT_OK) return;

chronobent_cpp::Processor processor(config, 1024);
auto controls = chronobent_default_controls();
controls.tempo = 1.25;       // 25% faster.
controls.pitch = 1.0;        // Original key: Master Tempo.
controls.formant_scale = 1;  // Original spectral envelope.
controls.transients = CHRONOBENT_TRANSIENT_MIXED;

status = processor.set_source_controls(reader, user, input_frames, controls);
if (status == CHRONOBENT_OK) {
    size_t produced = 0;
    status = processor.render(output, requested_frames, produced);
    // Consume exactly produced frames, including on END or source error.
    // Continue until END, handling a source error before retrying.
}
```

This fragment assumes the host supplies `reader`, source storage and output
buffers. [processor_cpp.cpp](examples/processor_cpp.cpp) is a complete compiled
example. The wrapper owns its processor and is move-only. Construction can
throw; processing methods return C status values.

## C integration

The same processor is available without a C++ wrapper:

```c
chronobent_config config = chronobent_default_config(48000, 2);
chronobent_controls controls = chronobent_default_controls();
chronobent_processor *processor = NULL;
chronobent_status status = chronobent_processor_create(&config, 1024, &processor);
if (status == CHRONOBENT_OK) {
    status = chronobent_processor_set_source_controls(
        processor, reader, user, input_frames, &controls);
    /* Render, seek and change controls on the same worker thread. */
    chronobent_processor_destroy(processor);
}
```

Include [chronobent.h](include/chronobent/chronobent.h). The runnable
[processor_c.c](examples/processor_c.c) also demonstrates the retained 0.2 API.
Use the fixed-epoch API when your host already owns parameter transitions.

## Pitch, tempo and formants

`tempo` is input frames per output frame. `pitch` is output frequency divided by
input frequency. Convert semitones with `exp2(semitones / 12.0)`.

| Intent | Tempo | Pitch | Formant scale |
| --- | --- | --- | --- |
| Master Tempo at 125% | `1.25` | `1` | `0` or `1` |
| Up an octave, same duration | `1` | `2` | `0` to follow pitch, `1` to preserve timbre |
| Turntable-style playback | `1.25` | `1.25` | `0` |
| Change timbre without transposing | `1` | `1` | `exp2(formant_semitones / 12.0)` |

`formant_scale = 0` follows the pitch naturally. An explicit scale is relative
to the original source envelope. Envelope correction is approximate and bounded;
it cannot reconstruct missing harmonics or isolate a voice from a mix.

Longer analysis windows resolve closer low-frequency components but can soften
attacks. The profiles expose that tradeoff, without promising a universally best
setting. [API.md](API.md) documents exact windows, limits and control behavior.

## Host responsibilities

1. Supply immutable, interleaved float audio through a random-access callback.
2. Create and bind a processor on a rendering worker.
3. Apply coherent controls on that worker and fill a bounded output queue.
4. Let the audio callback consume prepared frames, with silence on underrun.
5. Flush queued output on seek and stop callbacks/workers before releasing storage.

After creation, processor calls allocate nothing and take no locks, excluding
work done by the source callback. Control changes and seeks can read history
and regenerate tables, so they belong on the worker. The DSP starts no threads,
performs no I/O and opens no audio device. Its working memory does not grow with
track length.

`BUSY` means a previous parameter fade must finish. Coalesce control changes and
retry after rendering. Source failures preserve the returned valid prefix and
allow retry. The [API reference](API.md) specifies ownership, thread, timing and
failure contracts. The [Lab player](examples/pitch_lab/player.cpp) shows queue
ownership and seek handoff around the public API.

## WAV renderer

Raise pitch by one semitone at the original tempo:

```sh
build/chronobent-render input.wav shifted.wav 1 1.059463094
```

Change timbre at the original pitch, with a detailed analysis window:

```sh
build/chronobent-render input.wav timbre.wav 1 1 mixed 1.189207115 2 detailed
```

Arguments after tempo and pitch select `tonal`/`transients`/`mixed`,
`shift`/`preserve`/an explicit formant ratio, envelope resolution in milliseconds,
and `compact`/`balanced`/`detailed`. The renderer reads PCM16/24/32 or float32 WAV
and writes float32 WAV. It rejects malformed input and existing output paths.
It loads the source into memory. On Windows, use `build/Release/chronobent-render.exe`.

## ChronoBent Lab

The redesigned 0.3.0 app is available from source. Its packaged download is
pending Apple notarization and package verification.
[Download the previously verified Lab 0.2.0 for macOS](https://github.com/nicolaswehmeyer/ChronoBent/releases/download/v0.2.0/ChronoBent-Lab-0.2.0-macOS-universal.dmg),
or build 0.3.0 using the command below. The app targets macOS 11 or later on
Apple Silicon and Intel. The controls described here belong to 0.3.0.

Open a track, then press **Space** to play or pause. Drag the position slider to
seek, or use the **arrow keys** to skip five seconds. Restart returns to the
beginning. Speed ranges from 50% to 200%. Master Tempo starts enabled.

Pitch, speed and timbre have separate controls. Enable independent formants and
leave timbre at zero to preserve the original envelope. Choose a transient mode
and analysis profile for your material. Bypass restores original pitch and speed.
The output gain applies equally to processed and bypass audio; it starts at
-9 dB to leave room for processed peaks. There is no hidden limiter.

The app uses Cocoa and AVFoundation and decodes local mono/stereo files at
8 to 192 kHz, up to 64 million frames. A worker feeds a 4096-frame queue.
Controls crossfade over 1024 frames; seek discards stale queued audio and blends
into the new position. The display follows consumed source positions.
The decoded source is shared immutably when changing analysis profiles.

Build the app from source:

```sh
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCHRONOBENT_BUILD_PITCH_LAB=ON
cmake --build build-mac --parallel
open 'build-mac/ChronoBent Lab.app'
```

## Measure and contribute

[QUALITY.md](QUALITY.md) separates numerical tests, performance measurements and
listening work. [DESIGN.md](DESIGN.md) explains the signal path. Run the tests
before proposing DSP changes, describe the material and settings that improve,
and include regressions that protect timing, stereo and source lifetime.

The [MIT license](LICENSE) permits use in open-source and commercial software.
Keep its copyright and license notice with redistributed copies.
[export-files.txt](export-files.txt) lists the reviewed source package;
`python3 tools/export_source.py /path/to/new.zip` scans it with Gitleaks and creates
a deterministic source archive.
