# ChronoBent

ChronoBent is a C++17 library for changing audio pitch and tempo, with a C API
and an [MIT license](LICENSE).

- Separate pitch and tempo controls, including Master Tempo.
- Linked processing for mono, stereo and up to eight channels.
- Transient handling and optional formant preservation.
- Bounded memory, with no allocation or locks during rendering.
- Static and shared builds for Linux, macOS and Windows.
- Source-bound processor with smooth parameter changes, seek and planar output.
- Move-only C++ wrapper, runnable C/C++ examples and optional macOS audition app.

Version 0.2.0 is experimental. The API may change before 1.0. See
[quality measurements](QUALITY.md) for test coverage and current limits.

## Contents

- [Build and link](#build)
- [Choose an API](#choose-an-api)
- [C integration](#c-integration)
- [C++ integration](#cpp-integration)
- [API functions](#api-functions)
- [Audio callback integration](#audio-callback-integration)
- [Master Tempo](#master-tempo)
- [WAV renderer](#wav-renderer)
- [ChronoBent Lab](#chronobent-lab)
- [Performance and limits](#performance-and-limits)

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

## Choose an API

Use the **processor API** for a player, editor or app with adjustable controls.
It binds a source callback, owns two preallocated engines, crossfades parameter
changes, seeks and reports source position. It renders interleaved or planar
float audio. The **epoch API** remains available for fixed-ratio offline jobs
and hosts that already own their transitions. All seven 0.1.0 functions and
existing struct layouts remain compatible in 0.2.0.

| Parameter | Meaning | Range |
| --- | --- | --- |
| Tempo | Input frames per output frame; 2 is twice as fast | 0.25 to 4 |
| Pitch | Output frequency divided by input frequency; 2 is an octave up | 0.5 to 2 |
| Sample rate | Input and output frames per second | 8000 to 192000 |
| Channels | Phase-linked channels | 1 to 8 |

Convert semitones with `exp2(semitones / 12.0)`. Fixed-ratio output length is
`ceil(input_frames / tempo)`. Unity pitch and tempo produce an exact copy.
A processor changing tempo follows the new trajectory from the next rendered
frame; its remaining duration depends on that trajectory.

The callback supplies immutable, interleaved float audio. Reads can overlap,
look ahead or revisit previous frames. The library clips reads to the declared
source and supplies padding outside it. The caller owns decoded storage and
must keep it alive until source replacement or processor destruction.
See [API.md](API.md) and the [C header](include/chronobent/chronobent.h) for the
complete lifetime, threading, error and timing contracts.

## C integration

```c
#include <chronobent/chronobent.h>

chronobent_config config = chronobent_default_config(48000, 2);
chronobent_parameters parameters = chronobent_default_parameters();
parameters.tempo = 1.25;
chronobent_processor *processor = NULL;
chronobent_status status = chronobent_processor_create(&config, 1024, &processor);
if (status == CHRONOBENT_OK) {
    status = chronobent_processor_set_source(processor, reader, user,
                                            input_frames, &parameters);
    if (status == CHRONOBENT_OK) {
        size_t produced = 0;
        status = chronobent_processor_render(processor, output, requested, &produced);
        /* Consume exactly produced frames, including on END or source error.
           Continue rendering until END; handle a source error before retrying. */
    }
    chronobent_processor_destroy(processor);
}
```

This fragment assumes the host provides the callback, source length and output
buffer. [processor_c.c](examples/processor_c.c) is a complete, compiled example.
For fixed-ratio rendering, see [render_wav.cpp](examples/render_wav.cpp).

<a id="cpp-integration"></a>
## C++ integration

```cpp
#include <chronobent/chronobent.hpp>

chronobent_cpp::Processor processor(chronobent_default_config(48000, 2));
auto parameters = chronobent_default_parameters();
parameters.pitch = 2.0;
auto status = processor.set_source(reader, user, input_frames, parameters);
if (status == CHRONOBENT_OK) {
    size_t produced = 0;
    status = processor.render(output, requested, produced);
    // Consume produced frames and handle status before continuing.
}
```

The [C++ wrapper](include/chronobent/chronobent.hpp) is move-only and releases
its processor automatically. Only construction throws; processing methods return
the same C statuses. [processor_cpp.cpp](examples/processor_cpp.cpp) includes a
source callback and planar output. Link it with the same `chronobent::chronobent`
CMake target. The wrapper adds no DSP implementation; the linked library needs the C++ standard runtime.

## API functions

Processor functions use the `chronobent_processor_` prefix:

| Function | Purpose |
| --- | --- |
| `create`, `destroy` | Allocate or release two engines and bounded working buffers |
| `set_source` | Bind a reader, source length and initial parameters; restart at zero |
| `set_parameters` | Change tempo, pitch, transient and formant options with a crossfade |
| `seek` | Jump to an integer source frame with fresh, prerolled history |
| `render` | Produce interleaved output |
| `render_planar` | Produce one buffer per channel |
| `get_state` | Read source position, delivered frame count, parameters, fade and end state |

Utilities and fixed-epoch functions use the `chronobent_` prefix:

| Function | Purpose |
| --- | --- |
| `default_config` | Defaults for a supplied sample rate and channel count |
| `default_parameters` | Unity tempo/pitch, transients enabled, formants disabled |
| `status_string`, `version` | Human-readable status and library version |
| `create`, `destroy` | Own one fixed-epoch engine |
| `reset` | Start a fixed-ratio epoch at source zero |
| `reset_parameters` | Start an epoch and replace transient/formant options |
| `render` | Render through a callback supplied for that call |
| `output_frames`, `window_frames` | Query epoch length and resolved analysis window |

For a control update, modify a `chronobent_parameters` value and call
`set_parameters` on the rendering worker. `BUSY` means the previous fade is
still active: keep the latest desired value and retry after rendering more
audio. Repeating the current target succeeds without restarting the fade.
The [API reference](API.md#control-changes-and-seek) explains seek, preroll,
position accuracy and error recovery.

## Audio callback integration

1. Decode or cache an immutable source that supports random-access reads.
2. Create and bind a processor on a rendering worker.
3. Send coherent control commands to that worker. It applies changes and fills
   a bounded output queue.
4. Let the audio callback consume ready frames and supply silence on underrun.
5. Stop/join the worker and detach callbacks before releasing source storage.

The library starts no threads, performs no I/O and opens no audio device.
All processor calls after construction allocate nothing and take no locks,
excluding work inside the host callback. Control changes and seeks can read
history and regenerate tables; keep those calls off the audio callback.
Source length does not affect DSP working-memory size. Source storage belongs
to the host. This API needs accessible source audio and is not a live-input
processor with fixed lookahead latency.

The [Lab player](examples/pitch_lab/player.cpp) implements the worker and queue
around the public C++ wrapper. It coalesces rapid UI changes while a fade runs.
Hosts must manage their own queue flushing on seek and track output buffering
when displaying audible position.

## Master Tempo

Set processor parameters to `tempo = speed, pitch = 1.0` to change speed at the
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

[Download ChronoBent Lab 0.2.0 for macOS](https://github.com/nicolaswehmeyer/ChronoBent/releases/download/v0.2.0/ChronoBent-Lab-0.2.0-macOS-universal.dmg).
The DMG includes Apple Silicon and Intel builds for macOS 11 or later. Drag
ChronoBent Lab to Applications, then open it. The app is signed with Developer
ID and notarized by Apple. A SHA-256 checksum is available on the
[release page](https://github.com/nicolaswehmeyer/ChronoBent/releases/tag/v0.2.0).

To build the app from source:

```sh
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCHRONOBENT_BUILD_PITCH_LAB=ON
cmake --build build-mac --parallel
open 'build-mac/ChronoBent Lab.app'
```

Open an audio file and press Play. Speed ranges from 50% to 200%; pitch ranges
from -12 to +12 semitones. Master Tempo starts enabled, so speed changes retain
the selected key. Disable it to link pitch to speed. Bypass restores the
original pitch and speed. Restart returns to the beginning.

The app uses the public processor API. Control changes use a 1024-frame crossfade. The time display follows source
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
