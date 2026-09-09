# Quality measurements and listening

Version 0.1.0 is experimental. Passing the tests below establishes numerical
and lifecycle properties on generated signals. It does not establish transparent
processing on all music, a production deadline, or equivalence to another engine.

## Reproduce the synthetic measurements

Build with examples and tests enabled, then run:

```sh
ctest --test-dir build --output-on-failure
python3 tools/measure_quality.py build/chronobent-render quality.json
```

The JSON path must be new. Omitting it uses a temporary report. The measurement
script requires only Python's standard library; its generated audio is temporary.
It contains no recordings, downloaded corpus or third-party media. Output hashes
allow exact comparisons between builds made with the same platform/toolchain.

The 48 kHz mono harness covers ±1, ±5 and ±12 semitones at original tempo,
and 0.5, 0.8, 1.25 and 2 times tempo at original pitch. It measures interpolated
positive-crossing frequency on a 997 Hz tone, quadrature amplitude variation
in overlapping 40 ms Hann windows, exact duration, isolated off-grid click
position, and peak pre/post ringing from 2–50 ms away from the processed click.
A colored-noise signal, exponential sweep and harmonic/percussive mixture add
boundedness checks. A sweep alone is not a time-scaling distortion oracle:
ordinary inverse-sweep deconvolution assumes a time-invariant mapping.

Regression gates are less than 0.1 cent tone error, less than 0.5 dB amplitude
variation, exact fixed-ratio duration and at most two samples of click-position
error. These are engineering tolerances, not audibility thresholds. Pre/post
ringing is reported without a universal threshold: fractional click positions
can exceed −50 dB outside 2 ms. That limitation must not be hidden by averaging
with integer-position cases. Amplitude stability is only one warble diagnostic;
it does not measure all phase modulation or noise-floor artifacts.

The C++ tests add 44.1/48/96 kHz tones, independent DFT verification, block-size
invariance, anti-phase and independent channels, silent channels, sparse attacks,
a synthetic vowel envelope, one out-of-band alias case, malformed inputs,
read retry, reset isolation and allocation traps. The player tests cover
half/double/fractional speeds with and without Master Tempo, source-position
continuity during automation, mixed 16–4096-frame callbacks, pause, bypass,
tails and thread teardown. These cases do not certify all audio-device timings.

## Listening protocol for future DSP changes

Use recordings you own or have permission to test. Keep their provenance and
raw files outside source control. Include exposed vocals and vowels, bass,
sustained strings/saxophone, sharp percussion, reverberant stereo material and
dense full mixes. Include ±1/±5/±7/±12 semitones, moderate and extreme tempo
changes, low-level tails, and automation during sustained notes and attacks.

Before comparing, align the intended source events, match perceived level
without clipping, and freeze the processing settings. Use randomized concealed
A/B labels and independently randomized X trials, allowing repeated audition.
Record answers before revealing identities. Predeclare excerpts, trial counts,
listeners and the analysis; report the binomial uncertainty and correct for
multiple comparisons. An inability to distinguish two results in a small test
is not evidence of universal equivalence. Unblinded developer preference is
useful for diagnosis but must not be reported as a blinded quality result.

No blinded real-music result is claimed for 0.1.0. Independent formant-position
measurements on real vowels, inter-peak phase metrics on polyphony, onset-envelope
comparisons on real percussion, and perceptual evaluation remain future work.
Use these to decide whether a pitch-synchronous path, multiple analysis
resolutions or a different phase reconstruction method earns its complexity.
