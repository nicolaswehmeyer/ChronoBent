# Change log

## 0.7.0 — 2026-09-11, player throughput

- Stereo channel pairs share one complex transform in analysis and synthesis.
  Exactly conjugate-symmetric stage twiddles keep real-input spectra exactly
  Hermitian, so the channels separate with crosstalk below -150 dB instead of
  exactly zero; identical or opposite channels agree within float rounding.
- Contiguous per-stage twiddles, permutation-free transforms fed in bit-reversed
  order, a branch-free inverse and AArch64 NEON butterflies with an
  identical-order scalar fallback.
- Interpolation over a mirrored ring, so every sinc span is contiguous, with
  four-lane accumulation shared by the NEON and scalar paths.
- Analysis frames retain the previous window's validated samples and fetch only
  new source frames; a failed fetch keeps that prefix and stays retryable.
- Single-precision bin magnitudes and flux, branch-free peak picking, region
  fills and buffer exchange instead of spectrum copies. The phase advance is one
  double-precision arctangent of now*conj(before) per propagating peak;
  rotation multipliers come from double-precision series, not library calls.
- Best-of-three render time fell 2.5 to 2.7 times on Apple Silicon (Clang 21,
  -O3) and 2.7 to 3.3 times with the player toolchain (GCC 7.5, -O2, AArch64)
  across the benchmark cases; two Master Tempo player cases (tempo 1.1 with
  pitch 0.5 and 2) were added. Output is not byte-identical to 0.6.0: the wide
  diagnostic matrix shows no in-band pitch change above 0.5 cent and no tone
  residual worse by more than 3 dB. Device timing is not measured.

## 0.6.0 — recorded monophonic tuning

- Optional C/C++ tuning API with periodicity/temporal analysis, scale correction,
  manual notes, vibrato/drift controls and linked pitch-synchronous rendering.
- Shared Note Studio in Lab, Instrument and FX. Explicit original/tuned selection,
  asynchronous source-safe preparation and Lab float WAV export.
- Version 2 plugin state retains original/corrected PCM and edit metadata; 0.5
  source-only states still load. Host sample-rate changes retain canonical audio.
- Legacy DSP processing, structs and source inventory retained. Tuning can be
  excluded entirely; no analysis is added to existing processor calls.
- Independent held-out vocal measurements and synthetic pitch/envelope checks;
  bounded ±5 st tuning, with no commercial perceptual-equivalence claim.
- Instrument preparation controls (pitch, time, timbre, root, detail, transients,
  formants) prepare the sound by themselves after a settled editor or host
  change; Apply Sound remains an immediate manual re-preparation. Reopened
  projects prepare the sound their controls show.
- Vocoder phase is evaluated only at bins that propagate it, from retained
  analysis spectra, and sinc coefficients are reused for consecutive equal
  fractional source positions. Output stays byte-identical; render time fell
  about 9% locally. Contributed by Domenico Valentino.

## 0.5.0 - Release candidate

- Add a macOS AU/VST3 sample instrument with four voices, three original factory
  sounds, local sample import, MIDI/sustain/loop, attack/release and embedded state.
- Add separate AU/VST3 FX with live pitch/timbre, aligned dry/wet, embedded
  30-second captures and independent captured-loop tempo. Report host latency
  and infinite tail, and retire input generations on reset/bypass resume.
- Add effect DSP-oracle, wrapped-capture, concurrency/allocation, native AU/VST3
  state/routing/bypass and editor lifecycle regressions.
- Add a sculpted local WebView interface with waveform, five knobs, typed values,
  keyboard interaction, MIDI feedback and explicit sound preparation.
- Keep source and worker ownership outside the independent DSP. Guard source
  publication/retirement, bound voice queues and keep the live callback free of
  allocation, file access, locks and DSP resets.
- Add instrument concurrency/allocation, native import, MIDI timing, state
  roundtrip/refusal, editor lifecycle and format-validation coverage.
- Include the extended pitch-range work below and a shared Lab/Instrument/FX
  product illustration. The numerical DSP is unchanged from the 0.4.0 milestone.
- Universal binary distribution remains pending Apple notarization.

## 0.4.0 - Development milestone, included in 0.5.0

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
