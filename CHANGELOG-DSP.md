# Change log

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
