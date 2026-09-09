# ChronoBent

ChronoBent is a C++17 library for changing audio pitch and tempo, with a C API
and an [MIT license](LICENSE).

- Separate pitch and tempo controls, including Master Tempo.
- Linked processing for mono, stereo and up to eight channels.
- Transient handling and optional formant preservation.
- Bounded memory, with no allocation or locks during rendering.
- Static and shared builds for Linux, macOS and Windows.
- WAV renderer, benchmark and optional macOS audition app.

Version 0.1.0 is experimental. The API may change before 1.0. See
[quality measurements](QUALITY.md) for test coverage and current limits.

## Build

Requires CMake 3.16+, a C/C++17 compiler and the C++ standard library. Tests of
the WAV example also require Python 3. No dependencies are downloaded.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Options:

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` | Build a shared library |
| `CHRONOBENT_BUILD_TESTS` | `ON` | Build regression tests |
| `CHRONOBENT_BUILD_EXAMPLES` | `ON` | Build the WAV renderer and benchmark |
| `CHRONOBENT_BUILD_PITCH_LAB` | `OFF` | Build ChronoBent Lab on macOS |
| `CHRONOBENT_SANITIZE` | `OFF` | Enable ASan/UBSan with Clang or GCC |

Install with `cmake --install build --config Release --prefix /your/install/prefix`.
CMake consumers can use `find_package(chronobent CONFIG REQUIRED)` and link
`chronobent::chronobent`. Static C consumers must enable the C++ linker.
For other build systems, `sources.txt` lists the implementation files and
`sources.mk` provides a Make include. The ABI may change before 1.0.

## API

The caller provides a synchronous read callback over immutable, interleaved
float audio. Reads can overlap, look ahead or revisit earlier frames. The
renderer clips reads to the declared source and pads outside it with zeros.
See the [C header](include/chronobent/chronobent.h) for the full contract.

| Parameter | Meaning | Range |
| --- | --- | --- |
| Tempo | Input frames per output frame; 2 is twice as fast | 0.25 to 4 |
| Pitch | Output frequency divided by input frequency; 2 is an octave up | 0.5 to 2 |
| Sample rate | Frames per second | 8000 to 192000 |
| Channels | Interleaved channel count | 1 to 8 |

Convert semitones with `exp2(semitones / 12.0)`. Output length is exactly
`ceil(input_frames / tempo)`. Pitch and tempo both at 1 produce an exact copy.

```c
#include <chronobent/chronobent.h>

chronobent_config config = {48000, 2, 0, 1, 0};
chronobent *engine = NULL;
if (chronobent_create(&config, &engine) == CHRONOBENT_OK) {
    if (chronobent_reset(engine, input_frames, 1.0, pitch_ratio) == CHRONOBENT_OK) {
        size_t produced = 0;
        chronobent_status status = chronobent_render(engine, reader, user,
                                                    output, requested, &produced);
        /* Consume produced frames even on END or a source error.
           Repeat until END, handling errors before continuing. */
        (void)status;
    }
    chronobent_destroy(engine);
}
```

This fragment assumes the caller supplies the source length, ratio, callback
and output buffer. The [WAV renderer](examples/render_wav.cpp) is a complete
example; [test_c.c](tests/test_c.c) shows a minimal C consumer.

Allocation happens at create/destroy. Reset clears history and prepares tables
without allocation. Render performs no allocation, locks or I/O, apart from
work done by the caller's read callback. Each instance permits one calling
thread at a time and owns its working memory. Memory use depends on channel
count and window size, not track duration.

Pitch and tempo stay constant between resets. Continuous controls require
successive render epochs aligned to the current source position, with a
crossfade between them. The [example player](examples/pitch_lab/player.hpp)
implements this on a worker thread. This API requires accessible source audio;
it does not accept live input with fixed latency.

## Master Tempo

Call `chronobent_reset(engine, frames, speed, 1.0)` to change speed at the
original key. A speed of `1.25` produces 80% of the original duration.
Set pitch separately to transpose while retaining that speed. Setting pitch
and tempo to the same ratio gives turntable-style playback.

## WAV renderer

Raise pitch by one semitone at the original tempo:

```sh
build/chronobent-render input.wav shifted.wav 1 1.059463094
```

On Windows, use `build/Release/chronobent-render.exe`. The renderer reads RIFF
PCM16/24/32 or float32 WAV and writes float32 WAV. Optional arguments select
`tonal`/`transients` and `shift`/`preserve` formants. It refuses existing output
paths and malformed input. This example loads the source into memory.

## ChronoBent Lab

Build the macOS app:

```sh
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCHRONOBENT_BUILD_PITCH_LAB=ON
cmake --build build-mac --parallel
open 'build-mac/ChronoBent Lab.app'
```

Open an audio file and press Play. Speed ranges from 50% to 200%; pitch ranges
from -12 to +12 semitones. Master Tempo starts enabled, so speed changes retain
the selected key. Disable it to link pitch to speed. Bypass restores the
original pitch and speed. Restart returns to the beginning.

Control changes use a 1024-frame crossfade. The time display follows source
position. Playback pauses without advancing the source. Both processed and
bypass output use -3.1 dB headroom. The status line shows queue depth and
underruns.

The app uses Cocoa and AVFoundation. It decodes mono or stereo files at 8 to
192 kHz, up to 64 million frames, and keeps the decoded audio in memory. A worker
renders into a 4096-frame queue consumed by the audio callback. Control response
includes queue depth, crossfade time and system output latency. After resuming
from pause, up to one queue of audio may still use the previous setting.

## Performance and limits

The phase vocoder can change the texture of dense mixes and smear ambiguous
attacks. Short windows limit bass resolution. Formant preservation adjusts a
smooth spectral envelope and can alter instruments as well as voices. Hosts
should leave output headroom and check the processed signal for clipping.

`chronobent-benchmark` reports render time relative to output duration, time to
the first 4096 frames and the longest observed render block. Measure on the
intended hardware under representative load before choosing a queue size.
See [DESIGN.md](DESIGN.md) for the signal path and [QUALITY.md](QUALITY.md) for
measurements and listening procedures.

## Source archives

`export-files.txt` lists the source package contents. With Gitleaks installed,
`python3 tools/export_source.py /path/to/new.zip` scans those files and creates
a deterministic archive. The destination must be new. The archive includes
source, examples, tests, build files, documentation and the license.
