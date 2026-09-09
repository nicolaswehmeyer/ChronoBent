# Quality measurements

Version 0.2.0 has synthetic regression coverage. Blinded listening results on
music and audio-device deadline measurements are still pending.

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
