# ChronoBent plugins

A four-voice sample instrument for macOS AU and VST3. Start with Glass Circuit,
Soft Current or Copper Bloom, or load your own sound. Shape its pitch, time and
timbre, press **Apply Sound**, then play it from MIDI or the on-screen keyboard.

The 0.5.0 binaries are release candidates awaiting Apple notarization. The source
build and the tests below are available now. macOS 11 or later; Apple Silicon and
Intel. AU is the format for Logic Pro; Ableton Live can load VST3 or AU on macOS.
Automated AU/VST3 and native host tests do not establish testing inside every DAW
version. Direct Logic Pro and Ableton Live session checks remain outstanding.

## Play

- **Source:** three original synthesized factory sounds, or a local mono/stereo
  file readable by macOS. Up to 120 seconds, at 8–192 kHz. Imported audio is
  converted to the host rate using the macOS high-quality converter.
- **Pitch:** -24.00 to +24.00 semitones, independently of playback speed.
- **Keyboard:** two octaves below and above the chosen root. MIDI 60 is C4 in
  this interface. The pitch knob and played note combine to a maximum ±48 st.
- **Time:** 0.500×–2.000× speed. Notes retain their transformed duration across
  the keyboard. The Time control is a speed ratio, not host-tempo synchronization.
- **Timbre:** independent formant shift, ±12 st. Turning this knob enables
  Keep Formants. At 0 st with Keep Formants on, the original envelope is retained.
- **Transients / Detail:** Smooth, Crisp or Mixed onset handling; Compact,
  Balanced or Detailed analysis. Longer windows help resolve low fundamentals.
- **Performance:** velocity, four voices with oldest-voice stealing, attack,
  release, loop, MIDI sustain (CC64), all-notes-off (CC123), all-sound-off (CC120).
  Pitch wheel and MPE are not implemented in 0.5.0.

Drag a knob vertically; hold Shift for fine adjustment. Double-click to reset,
use arrow keys on a focused knob, or type an exact value beneath it. The computer
keys A–K play one octave when an input field is not focused. MIDI activity lights
the keyboard and the four voice indicators.

Pitch, Time, Timbre, root, detail and transient changes need **Apply Sound**.
Preparing a new source keeps the current one available; publication stops its
old voices. Wait for **Ready to Play** before recording or bouncing. Attack,
release, output and loop are host-automatable performance controls. Preparation
controls deliberately do not advertise live automation.

Projects embed the decoded sample, settings and applied preparation state, so
moving or deleting the original file does not break a saved instrument. Long
samples increase project size. Four voices are the limit; this first release has
no disk-streaming source browser, sample slicing, effects rack or host sync.

The output starts at -9 dB. There is no hidden limiter. Polyphony and transformed
peaks can exceed 0 dBFS; use the output control and host meter. If the status line
reports a stream underrun, reduce simultaneous voices or increase the host audio
buffer and check the render again. Offline rendering waits for preparation and
voice workers with a bounded timeout; a timeout is reported and must be resolved
before treating a bounce as successful.

## ChronoBent FX

**ChronoBent FX** is a separate audio effect, available as AU (`aufx`) and VST3.
Insert it on an audio track or bus. It accepts mono-to-mono, mono-to-stereo and
stereo-to-stereo input/output, at host rates from 8 to 192 kHz.

In **Live** mode, Pitch (±24 st), Timbre (±12 st with Keep Formants), transient
mode, Mix (0–100%) and Output (-60 to +6 dB) can be automated while audio runs.
Live cadence stays 1×: a continuous incoming stream cannot be slowed forever
without accumulating an ever-growing delay. Time is disabled in this mode.

Press **Capture** to record the incoming track. Press **Play Capture** to loop
it, or let recording stop automatically at 30 seconds. The minimum is 50 ms.
The **Time** control now changes speed from 0.5× to 2× independently of pitch.
**Capture Again** records a replacement; **Live** returns to the incoming track.
Capture/Live selection is a manual source operation, not advertised as an
automatable parameter. There is no host-tempo sync, MIDI or file browser in FX.
Projects embed the captured stereo audio and settings; restoring them does not
require an external file. A 30-second capture at 192 kHz adds about 46 MB.

The live callback queues input for one worker that owns the DSP and immutable
logical input history. Output is timestamped and the dry path is delayed to
match. FX reports a fixed delay for each host rate, including 10,368 frames
(216 ms) at 48 kHz. Host delay compensation aligns tracks when enabled; it does
not eliminate monitoring latency. This version is intended for track processing
and captured passages, not low-latency live monitoring.

Pitch/formant and source transitions are faded. The capture loop has a short
edge fade. On missing worker output, the aligned dry path keeps advancing and
the status reports underruns; a render with such warnings needs review. The
live callback does no allocation, locking, file I/O or DSP reset. Offline render
waits on its worker with one bounded ten-second budget per host block. Reset or
bypass resume retires queued audio before a new input generation starts.

Captured loops have no finite end, so the plugin reports an infinite tail.
Select a bounded range when bouncing. The output has no limiter; monitor levels
in the host, especially when changing pitch or mixing dry and wet signals.

## Build the plugins

Install Xcode command-line tools, CMake 3.16+ and Git. Fetch the two reviewed,
permissively licensed framework dependencies into a separate local directory:

```sh
python3 tools/setup_plugin_dependencies.py /absolute/path/to/chronobent-dependencies
cmake -S . -B build-plugins -DCMAKE_BUILD_TYPE=Release \
  -DCHRONOBENT_BUILD_PLUGINS=ON \
  -DIPLUG2_DIR=/absolute/path/to/chronobent-dependencies/iPlug2 \
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-plugins --parallel
ctest --test-dir build-plugins --output-on-failure
```

Output: `build-plugins/out/ChronoBent.component`, `ChronoBent.vst3`,
`ChronoBentFX.component` and `ChronoBentFX.vst3`. The build signs these local bundles ad hoc;
it does not install them or provide a notarized distribution signature. Copy the
AU to `~/Library/Audio/Plug-Ins/Components/`, or the VST3 to
`~/Library/Audio/Plug-Ins/VST3/`, then rescan plugins in your host. Use an instrument
track with stereo output for ChronoBent, or an audio insert for ChronoBent FX.
The DSP-only default build fetches no dependencies.

Pins: iPlug2 `d54f69050f517e43b941d88c2a170f0a840b9ee4`;
VST3 SDK `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`.
[Third-party notices](THIRD_PARTY_NOTICES.md) cover the code linked into the
plugins. No framework checkout or vendor source is included in this repository.
The WebView is local HTML/CSS/JavaScript, uses system fonts and makes no network
requests. The macOS frameworks are linked from the system.

## Architecture and tests

`instrument.cpp` is a host around the unchanged public ChronoBent DSP API. It
owns the immutable source, two leased preparation banks, 49 prepared attacks and
four bounded voice streams. One worker prepares the first half-second for each
key; four workers regenerate those attacks and continue each note through
65,536-frame queues. The live audio callback mixes prepared/queued frames and
performs no allocation, file access, mutex wait or processor reset. Render and
MIDI share a single audio owner. Hosts suspend processing before changing the
sample rate or resetting the plugin. Control operations and state serialization
are outside the live render path.

Bank publication uses explicit acquisition and reader leases. An inactive bank
cannot be reused until every audio, worker and snapshot reader has retired.
Elapsed time does not release source storage. Superseded preparation requests
are cancelled, voice commands are bounded and failed admission is reported.
The first release supports host rates from 8 to 192 kHz; unsupported rates produce
silence and an explicit status message.

The core DSP source inventory remains independent of plugin/UI/platform code.
Instrument regressions check exact prepared-to-streamed sample continuity,
MIDI/sustain/loop/stealing, source replacement, concurrent snapshots and live
callback allocations. Native tests check file decoding and rate conversion,
AU loading, sample-offset MIDI, offline first-note preparation, embedded-state
audio identity, invalid-state refusal, automation and reset. The optional native
editor test also checks the real WebKit bridge, integer normalization, reopening
and a rendered screenshot. Run it with an output image path:

```sh
build-plugins/plugin/chronobent-plugin-host-test \
  "$PWD/build-plugins/out/ChronoBent.component" "$PWD/native-editor.png"
auval -v aumu ChBn NWeh
```

`auval` requires the AU to be installed in the user Components directory. The
native host test loads the exact bundle path directly and does not install it.
VST3 validation uses Steinberg's validator built from the pinned SDK. These are
host-contract and synthetic audio tests, not a claim of perceptual transparency,
commercial parity or hardware-device qualification.


FX regressions check exact unity/delay and partitioning, octave shifts against
an independent full-source DSP oracle, maximum capture across wrapped history,
source/control transitions, restoration, generation reset, concurrent state and
live callback allocations. Native AU and VST3 hosts check input routing, embedded
capture audio identity, malformed-state preservation, bypass/reset and latency/
infinite-tail metadata. Run the exact effect and its optional editor image test:

```sh
build-plugins/plugin/fx/chronobent-effect-host-test \
  "$PWD/build-plugins/out/ChronoBentFX.component" "$PWD/effect-editor.png"
build-plugins/plugin/fx/chronobent-effect-vst3-test \
  "$PWD/build-plugins/out/ChronoBentFX.vst3"
auval -v aufx ChFx NWeh
```

`host.hpp`, `mac_editor.mm`, native test fixtures and the web control primitives
are shared between the two plugin variants. Each variant owns its engine,
parameters and project state. No device adapter is included in either format.
