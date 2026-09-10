# Quality measurements

The 0.6.0 candidate adds recorded-monophonic tuning. Earlier processor,
extended-range and host observations below retain their original version scope.
Blinded listening results and audio-device deadline measurements are still pending.

## 0.6.0 monophonic correction

The optional tuner is independent of existing global pitch/tempo processing.
It targets recorded solo voice or melody, with linked mono/stereo processing,
original duration, conservative unvoiced handling and a maximum ±5 st correction.
Periodicity candidates and a temporal decoder reduce, but do not eliminate,
octave mistakes. Pitch-synchronous resynthesis approximately preserves the source
spectral envelope. No neural models or third-party DSP implementations are linked.

A local evaluation used the 40 short solo-vocal recordings and F0 annotations in
[vocadito](https://zenodo.org/records/5578807). Files 1–10 were the development
split. Source hashes were frozen before evaluating files 11–40; the following
30-file results were not used to change the algorithm. On 118,837 annotation
frames, frame-weighted pitch accuracy within 50 cents was **88.33%**, voiced
recall **90.58%**, unvoiced false-alarm rate **9.82%**, and octave-error rate
**0.45%**. Per-file pitch accuracy ranged from **70.14% to 98.53%**. Median of
per-file absolute voiced pitch errors was **6.11 cents**. These are detection
metrics against the supplied annotations, not an assessment of natural sound or
correct note intent. Voicing edges, breathy passages and octave errors need review.

On the development Apple Silicon Mac, median analysis wall time across those
30 recordings was **1.108% of source duration**; correction rendering was
**0.111%**. These are offline wall-time ratios for this corpus and toolchain,
not CPU utilization or live callback guarantees. Default correction output/input
RMS ratios ranged **0.9904–0.9995**; the largest float peak was **1.0057**.
Float processing has no limiter. Preserve headroom when playing or converting it.
The dataset, annotations, raw reports and listening files stay outside this source
package; only these bounded observations are published.

Independent synthetic gates cover known tone frequency, missing fundamentals,
vibrato retention and hard flattening, multiple notes with gaps, major-scale
selection, opposite-phase stereo, 8/44.1/48/96/192 kHz inputs, finite output,
source-error prefix/retry, cancellation, atomic invalid edits, exact duration,
random seek/partition identity and allocation-free analysis/edit/render calls.
A separate synthetic vowel has analytic formants at 700/1250/2600 Hz and a
115 Hz base excitation detuned by 30 cents. At -5/-2/-0.3/+2/+5 st correction,
gain-fitted harmonic-envelope relative L2 error was **2.18–5.80%** and RMS ratio
**0.856–0.987**. C++ gates independently enforce error below 10%, RMS ratio
0.8–1.1 and finite peaks below unity at -5/-0.3/+5 st. This model tests a bounded
spectral property; it does not establish formant transparency on real singing.
Wider pitch-synchronous shifts are excluded because prototype octave shifts
lost fundamental energy. The legacy processor's separate wider range remains.

Host tests check cancelled/replaced sources, immutable completed results,
source-error recovery, reanalysis with saved manual edits and concurrent observers.
Native Note Studio tests exercise analysis, controls, manual note selection,
original/tuned switching and editor reopen. Instrument and FX AU tests render a
manual 225 Hz-to-MIDI-58 change at approximately 233.05 Hz, retain both PCMs and
edits across project restore and host-rate changes, and reject malformed packets
without changing state. FX VST3 also checks dual-source and old source-only state.
Lab exports exact float32 samples and preserves an existing file on export failure.
These automated checks do not replace direct DAW sessions or blinded listening.

### 0.6 legacy compatibility and portability

All 29 generated mono hashes and 32 retained music renders match the 0.5
renderer exactly on the release Mac. Builds and installed C/C++ consumers pass
with tuning ON and OFF; OFF exports no tuning symbols. Old-header C/C++ examples
also run against the new shared library. The existing DSP units (apart from the
version/status entry point), player, instrument engine and effect engine remain
byte-identical source files.

The initial cross-platform CI exposed GCC indentation warnings and MSVC integer
zero-to-float fill warnings. The portability follow-up changes only statement
layout and zero literal types. Its universal tuning object is byte-identical to
the frozen candidate, preserving the held-out algorithm identity. Clarification
of that follow-up commit's executable comparison: the five desktop binaries match
after signature removal **and normalization of the signature-related `__LINKEDIT`
virtual size**, not as raw unsigned files. No code or data bytes are normalized.
The original failed comparison and exact subsequent scope are retained locally.

## Run the measurements

Build with examples and tests enabled, then run:

```sh
ctest --test-dir build -C Release --output-on-failure
python3 tools/measure_quality.py build/chronobent-render quality.json
```

On Windows, use `build/Release/chronobent-render.exe`. The JSON path must be new;
omitting it uses a temporary report. The script requires Python's standard
library and generates its audio in a temporary directory. Output hashes allow
exact comparisons between builds using the same platform and toolchain.

The 48 kHz mono tests cover -12, -5, -1, +1, +5 and +12 semitones at original
tempo, plus 0.5, 0.8, 1.25 and 2 times tempo at original pitch. Measurements are:

- Frequency from interpolated positive crossings of a 997 Hz tone.
- Amplitude variation from quadrature measurements in overlapping 40 ms Hann windows.
- Exact output duration.
- Isolated click position and peak ringing from 2 to 50 ms before and after it.
- Finite, bounded output for colored noise, a sweep and a percussive harmonic mix.

Tests require less than 0.1 cent tone error, less than 0.5 dB amplitude variation,
exact fixed-ratio duration and at most two samples of click-position error.
These are regression tolerances, not audibility thresholds. Ringing is reported
without a pass threshold: fractional click positions can exceed -50 dB outside
2 ms. Amplitude variation does not capture all phase modulation or noise artifacts.
A sweep is a stress input here; ordinary inverse-sweep deconvolution assumes a
time-invariant mapping and cannot directly rate time-scaling distortion.

C++ tests also cover 44.1/48/96 kHz tones, FFT comparison against a direct DFT,
block-size invariance, opposite-phase and unrelated channels, silent channels,
sparse attacks, a synthetic vowel envelope, one out-of-band alias case,
malformed input, read retry, reset isolation and allocation checks.

Player tests cover half, double and fractional speeds with Master Tempo on and
off, source-position continuity, mixed 16 to 4096-frame callbacks, pause, bypass,
end of source and thread teardown. These tests use synthetic audio and do not
measure system audio deadlines.

## 0.4.0 pitch-range measurements

Run `tools/measure_matrix.py build/chronobent-render new-output --wide` with
NumPy installed as below. The full report retains 306 renders: the original
162 cases, 120 extended-range tone cases, 15 stopband cases and nine bass/window
diagnostics. The extended tone grid covers 8, 44.1, 48, 96 and 192 kHz, ±48,
±36, ±24 and ±12.37 semitones, and .25/1/4 tempo. Frequencies stay within the
output passband. All contract gates passed on the Apple M5 development Mac;
the maximum fitted tone error among gated cases was 0.071 cents (rounded up).

The stopband tones would land at 1.5 times output Nyquist without filtering.
At +24, +36 and +48 semitones their middle-section RMS attenuation exceeded
114 dB in these cases. This is a specific far-stopband test, not an alias-floor
guarantee for arbitrary audio or frequencies near the transition band.

The retained bass diagnostic is deliberately harder to resolve: a 32 Hz input
at 44.1 kHz, shifted up 48 semitones. Compact windows produced errors up to
268 cents; Balanced up to 38 cents. Detailed brought the measured error below
0.001 cents at all three tempos. These cases are reported separately without a
pitch pass threshold. The initial matrix failed its general 2-cent threshold on
three Balanced cases; those observations are retained and motivate the explicit
window comparison. Small windows do not support accurate extreme transposition
of every bass component. Formant transparency at ±48 is not established.

C/C++ regressions enforce creation-time bounds, arbitrary fractional range
endpoints, invalid-control rollback, finite linked stereo, exact duration and
block partitions, empty/short sources, seek, transitions, source-error retry and
no allocation after creation. Lab tests exercise ±48 and fractional pitches.
The original constructors still reject values outside .5..2.

All 29 generated mono hashes and 32 local music renders in the original pitch
range matched the frozen 0.3.0 output exactly on this toolchain. Five alternating
runs of the ten existing stereo benchmark cases changed median render time by
-0.42% to +0.94%, within 1% for these runs. This is a regression comparison, not
a measured optimization or a claim of exact timing equivalence.

A separate single run of 12 wide-range stereo benchmark cases measured render
wall time at 0.9% to 44.3% of output duration. At +48 semitones and original tempo,
the ratio was 20.9% at 48 kHz and 44.2% at 96 kHz. Some 256-frame calls, including
startup, exceeded their audio duration; the largest was 29.0 ms. Use a rendering
worker and an output queue. This is not CPU utilization or device qualification.
Creation-time filter tables are 0.394 MB per engine for the legacy range versus
3.149 MB for a maximum ratio of 16; a processor has two engines plus other working
buffers. Wider support has a real memory and throughput cost.

## 0.3.0 diagnostic matrix

The extended matrix retains generated inputs, float outputs, exact commands,
renderer and audio hashes, and a JSON report. NumPy is optional for library
users and required only for this diagnostic tool:

```sh
python3 -m venv .venv
.venv/bin/pip install numpy==2.5.3
.venv/bin/python tools/measure_matrix.py build/chronobent-render matrix-output
```

The output directory must be new. `--quick` selects a smaller CI matrix.
The full 162 renders cover 44.1, 48 and 96 kHz, 40/997/9000 Hz tones,
quarter to quadruple tempo, an octave of pitch in either direction, all transient
modes and analysis profiles, opposite-phase stereo and nine source-filter vowel
cases with independently shifted envelopes. Tone frequency is refined by a
least-squares sinusoid fit. The broad admission bound is 2 cents; it supplements
the tighter 997 Hz regression above. Duration, finite output and stereo relation
are hard contracts. Fitted residual energy and envelope error are diagnostics,
without perceptual pass thresholds. All 162 cases passed on the release Mac.

The vowel model is an independent source-filter target, not a recording of a
voice. C++ tests require the independently shifted envelope to improve toward
that target at two settings. Envelope correction remains approximate. A mixed
track offers no isolated voice envelope, and missing harmonics cannot be restored. Across the eight nonidentity vowel cases, model log-envelope
error ranged from 6.24 to 15.60 dB on the release Mac. These numbers expose
remaining envelope mismatch; they are not a perceptual rating.

## 0.3.0 regression and performance comparison

On an Apple M5 Mac using Apple clang 21 and identical Release compiler settings,
all 29 generated mono outputs and 32 music renders with legacy controls matched
the frozen 0.2.0 renderer sample for sample. The music comparison used four
12-second stereo excerpts, octave shifts, half/double tempo and both legacy
formant modes. Recordings and their provenance remain local. This protects
those cases; it is not a claim about all audio or all compilers.

The FFT computes finite complex products without a generic non-finite fallback,
while preserving the separate multiply/add order. Final throughput measurements
showed 8.18% to 11.54% lower median render time in five alternating
baseline/candidate runs of ten stereo cases at 48 and 96 kHz. These measure render-loop wall time, not hardware CPU counters
or audio callback deadline reliability.

A separate local AVFoundation offline harness processes the same four music
excerpts through this library and an installed reference audio component at
-12, -5, +5 and +12 semitones, with and without envelope preservation. Input,
block size and padding are controlled. Reported component delay is compensated
before level and spectrum analysis. Differences in level-matched log spectra
show that outputs differ; they cannot identify which output sounds better.
Float captures can exceed unity, so Lab starts at -9 dB with an adjustable output
gain. No limiter is included. Commercial audio equivalence is not established.

Player tests now cover paused/playing/end-of-file seeks, queue flushing, rapid
coalesced commands and concurrent audio consumption. ThreadSanitizer checks the
queue handoff. Native offscreen view tests exercise controls, source position,
skips, profile changes and bypass; offline AVFoundation tests exercise the audio
graph. These do not replace interactive listening or physical device tests.

## 0.2.0 regression and performance comparison

On the development Apple Silicon Mac with the same Release compiler settings,
all 29 generated mono outputs and 42 additional stereo outputs matched the
frozen 0.1.0 renderer byte for byte. Stereo comparisons covered 44.1, 48 and
96 kHz, seven pitch/tempo combinations and both formant modes. This demonstrates
unchanged samples for those inputs, not universal perceptual equivalence.

Five alternating baseline/candidate benchmark runs showed 8.25% to 23.22% lower
median render time in the eight stereo cases with pitch resampling. The two
pitch-unity cases were 0.66% to 0.75% slower in that run. The optimized loop shares
ring-buffer addressing between stereo channels and retains tap accumulation
order. These are local throughput observations, not audio deadline guarantees
or measurements on every supported platform.

Processor tests add parameter transitions, all DSP options, allocation checks,
seek/restart/end behavior, control-failure rollback, planar/interleaved equality,
block partitioning, partial error output and exact recovery through repeated
source failures during a fade. C and C++ examples run as tests. The Lab app
uses this same processor implementation.

## Listening procedure

Use permitted recordings of exposed vocals, bass, sustained instruments,
percussion, reverberant stereo material and dense mixes. Test -12 to +12 semitones,
moderate and extreme speeds, quiet tails, and control changes during sustained
notes and attacks. Keep recordings outside source control.

Align source events and match playback level without clipping. Conceal and
randomize A/B labels and X trials. Allow repeated audition and record answers
before revealing labels. Choose excerpts, trial counts and analysis in advance;
report binomial uncertainty and account for multiple comparisons. A small test
with no detected difference cannot establish general equivalence.

Further measurements should cover formant positions in real vowels, phase
relationships between spectral peaks in dense mixes, and onset envelopes in
percussion. Use listening results alongside these measurements when assessing
changes to phase reconstruction, transient handling or analysis resolution.

## 0.5.0 instrument validation scope

The 0.5 release candidate adds a sample-instrument host. Apart from its version
string, the DSP implementation is byte-identical to the 0.4 development source;
the wide-range numerical and musical limitations above still apply. An instrument
is not evidence of a new time-stretching algorithm or perceptual parity.

Local universal macOS validation passes 13 CTest checks, including the existing
SDK/Lab gates plus instrument and native import/host tests. The instrument checks
match the prepared attack and streamed continuation against a separate DSP
instance (maximum absolute error below 6e-9 in the 8 kHz stereo unity fixture),
exercise looping, sustain, rapid voice stealing, source replacement and concurrent
snapshots, and forbid allocations on the live MIDI/render thread. The final
ThreadSanitizer instrument run passes. The native host tests cover embedded-state
audio identity, malformed-state refusal without parameter mutation, sample-offset
MIDI, offline first-note readiness, automation, looping and transport reset.

Native import checks preserve float WAV samples exactly at their original rate;
8/48/96/192 kHz conversions check duration, tone frequency and linked stereo.
The native WebKit editor test and browser interaction checks cover parameter
normalization, typed/fine controls, self-applying knob release and host pitch
changes measured by fundamental period, Apply state, keyboard MIDI, formant activation,
repeated open/close and actual rendered layout. VST3 validation passes 47 tests;
Apple AU validation passes. Instrument, import, AU host and Lab audio gates also
run successfully through Rosetta using the x86_64 slices. This is not a test on
an Intel physical Mac or a direct Logic Pro/Ableton Live session.

Source audio is held in memory and saved inside projects. Each preparation bank
reserves up to half a second of stereo float samples for each of 49 keys: about
9.4 MB at 48 kHz or 37.6 MB at 192 kHz, plus its source storage. Two banks allow
safe replacement. Four fixed stereo voice queues reserve another 2.1 MB; DSP
filters and scratch are additional. Long source files, conversion and project
serialization need further host memory. Four simultaneous voices are the limit.

The live callback never waits for a worker; starvation yields silence and an
underrun report, not a claimed correct render. Offline rendering permits bounded
waits and reports preparation timeouts. Wait for Ready before recording/bouncing,
and resolve reported errors before accepting output. Host buffer size, competing
plugins and machine speed still affect real-time reliability. No broad listening
qualification, host-version matrix or device-hardware qualification is claimed.
