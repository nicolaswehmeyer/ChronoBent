# Quality measurements

Version 0.3.0 has synthetic regression coverage and local music-render diagnostics.
Blinded listening results and audio-device deadline measurements are still pending.

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
