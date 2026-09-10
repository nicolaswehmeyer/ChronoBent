# ChronoBent Instrument

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

Output: `build-plugins/out/ChronoBent.component` and
`build-plugins/out/ChronoBent.vst3`. The build signs these local bundles ad hoc;
it does not install them or provide a notarized distribution signature. Copy the
AU to `~/Library/Audio/Plug-Ins/Components/`, or the VST3 to
`~/Library/Audio/Plug-Ins/VST3/`, then rescan plugins in your host. Use an instrument
track with stereo output. The DSP-only default build fetches no dependencies.

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
