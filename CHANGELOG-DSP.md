# Change log

## 0.4.0 - Unreleased

- Add creation-time pitch ranges up to 1/16..16 (±48 semitones) in C and C++.
- Preserve original creation limits, ABI layouts and legacy filter coefficients.
- Scale the anti-alias filter from 96 to 768 taps for high upward shifts,
  with capacity reserved at creation and allocation-free control/render calls.
- Expand Lab and the WAV renderer to the full range, including fractional pitches.
- Add range admission/rollback, wide seek/fade/retry and allocation tests,
  plus multi-rate tone and stopband diagnostics.


## 0.3.0 - 2026-09-10

- Add independent formant scaling, 1 to 4 ms envelope resolution, and Mixed
  transient handling that preserves low-frequency onset phase continuity.
- Add Compact, Balanced and Detailed analysis profiles and complete C/C++
  controls, retaining earlier ABI layouts and entry points.
- Reduce FFT overhead while preserving tested legacy samples. See
  [measurements](QUALITY.md#030-regression-and-performance-comparison).
- Add a reproducible 162-render diagnostic matrix with retained input/output
  hashes, multi-rate tones, extreme ratios, stereo and vowel-envelope cases.
- Redesign Lab with a waveform overview, track seek, five-second skips,
  keyboard transport, timbre controls, analysis selection and output gain.
- Keep decoded source storage immutable across profile changes. Flush queued
  audio on seek with a nonwaiting callback handoff and short output blend.
- Extend source-error, transition, allocation, concurrent-seek and native UI
  tests. Rewrite the README around runnable integration and measured limits.

## 0.2.0 - 2026-09-09

- Add a source-bound processor with two preallocated engines, smooth parameter
  changes, seek, state queries, planar output and retryable transitions.
- Add default configuration/parameters, status descriptions, option-aware epoch
  reset, a move-only C++ wrapper and runnable C/C++ integration examples.
- Retain the 0.1.0 functions, config layout and status values.
- Move Lab parameter transitions into the public processor implementation.
  Control changes no longer allocate new engines.
- Share stereo resampler addressing while retaining summation order. Local
  median render time fell 8.25% to 23.22% in eight pitch-resampling benchmark
  cases; all 29 mono and 42 stereo reference outputs stayed byte-identical.
  See [measurement scope](QUALITY.md#020-regression-and-performance-comparison).
- Expand README integration sections and add the complete [API reference](API.md).
- Set app bundle version metadata directly from the CMake project version.

## 0.1.0 - 2026-09-09

First experimental release.

- C API with separate tempo and pitch ratios, exact output duration and
  bounded-memory rendering over a source callback.
- Linked multichannel phase processing, nearest-peak locking, sparse-attack
  anchoring, optional spectral-envelope preservation and band-limited resampling.
- Phase propagation at spectral peaks and reuse of phase multipliers within
  locked regions. In five interleaved benchmark runs on the development Mac,
  median processing speed improved by 0.96% to 2.05% across ten 48/96 kHz cases.
  All 29 generated measurement outputs matched the baseline byte for byte.
- ChronoBent Lab for macOS, with speed, pitch, Master Tempo and bypass controls.
  Parameter changes use worker-side preroll and a 1024-frame crossfade.
- Source-position tracking through speed changes and support for partial queue
  refills with mixed callback sizes.
- Static and shared CMake builds, installed C consumers and Linux, macOS and
  Windows CI.
- Regression tests, sanitizer builds, WAV renderer, benchmark and generated-signal
  [quality measurements](QUALITY.md).

The API may change before 1.0. Music listening tests and target audio-device
measurements are pending. See [DESIGN.md](DESIGN.md) for processing details.
