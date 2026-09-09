# DSP and host change log

## 0.1.0 — 2026-09-09

First experimental ChronoBent release, under MIT, with a C API and independent
CMake/static/shared builds. The existing signal path combines a linked phase
vocoder, nearest-peak locking, sparse-attack anchoring, optional approximate
spectral-envelope preservation and band-limited resampling. See [DESIGN.md](DESIGN.md).

### Accepted changes

- Propagate phase only at the peaks consumed by locked regions and reuse identical
  sine/cosine multipliers across each region. All 29 WAV outputs in the generated
  measurement set remained byte-identical to the pre-optimization baseline on
  the development Mac. Five interleaved before/after runs of the included stereo
  benchmark showed median speedups of 0.96–2.05% across its ten 48/96 kHz cases.
  This is a small CPU improvement, not an audible-quality improvement or a
  target-hardware deadline claim.
- Expose independent speed and key in the Mac audition host. Master Tempo keeps
  the selected key while speed changes; disabling it links pitch to speed.
  Fixed-speed synthetic player tests at 0.5, 1.079321, 1.25 and 2 times speed
  passed exact duration and 0.1-cent frequency gates, with measured errors below
  0.001 cent on the development Mac's test tone.
- Publish controls in one lock-free word, retain source-position metadata for
  every queued frame and crossfade coalesced changes on the worker. Mixed callback
  testing exposed incomplete queue refilling after short reads; the producer now
  fills partial free space. Pause, stereo, automation, bypass and end tests pass.
- Add reproducible generated-signal measurements and the explicit listening
  protocol in [QUALITY.md](QUALITY.md). No recordings are distributed.

### Deferred or rejected approaches

Adding a pitch-synchronous synthesis path or multiple FFT resolutions without
voiced/polyphonic admission and blinded listening evidence would introduce new
failure modes. These are deferred. Peak locking, stereo linkage and envelope
correction already exist; duplicate implementations were not added. No external
DSP source, proprietary binary implementation or copyleft DSP was incorporated.

A fixed-latency live-input interface and hand-written SIMD backends are also
deferred. The public contract remains bounded pull rendering over accessible
source audio. Compiler optimization is used, without an explicit SIMD guarantee.
The app implements worker-based parameter transitions rather than changing a
running epoch's ratios. No unmeasured transparency or general −50 dB pre-echo
claim is made.
