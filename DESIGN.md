# Signal path and contracts

ChronoBent combines phase-based time scaling with band-limited resampling. To
produce independent tempo `t` and pitch `p`, the intermediate vocoder advances
through source audio at `t/p`, then the resampler reads that intermediate signal
at `p` samples per output frame. The final duration is `ceil(input_length/t)`.
All positions derive from absolute frame indices, so caller block sizes do not
accumulate ratio drift.

## Modules

| Unit | Responsibility |
| --- | --- |
| `fft` | Iterative radix-2 complex FFT, contiguous conjugate-symmetric stage twiddles, permutation-free entry, NEON butterflies, normalized inverse |
| `vocoder` | Centered analysis, paired real transforms, shared phase rotation, peak regions, sparse attacks, normalized overlap-add |
| `envelope` | Tapered cepstral smoothing and bounded spectral-envelope correction |
| `sinc` | 96 to 768-tap, 1024-phase interpolated Blackman-windowed low-pass resampling |
| `chronobent` | C ABI, validation, reset epochs, cached clipped source reads, mirrored ring storage and exact output length |
| `controls` | Shared validation and legacy/extended parameter translation |
| `processor` | Bound source, preallocated epoch pair, control preroll, retryable crossfades, seek, planar output and position |

Each instance owns its tables and working buffers. The standard C++ library is
required. The only platform header is the compiler's AArch64 NEON intrinsics,
included behind an architecture guard next to a scalar fallback that performs
the same operations. File and audio-device APIs belong to the examples.

## Resampling capacity

The creation-time pitch range sets filter storage capacity. Active filters keep
96 taps up to pitch 2 and use an even-rounded `48*pitch` above it, preserving
transition-band width relative to the reduced input bandwidth. Preparation and
rendering reuse allocated storage. For a given legacy ratio the coefficients
are unchanged; taps accumulate in four interleaved lanes whose fixed sum makes
a given absolute position independent of the caller's block partition. The
synthesis ring remains four windows: even the smallest ring holds 768 taps
plus a complete synthesis hop. The ring repeats its first 768 slots after its
end, so every interpolation span is one contiguous run of frames.
The maximum pitch 16 and minimum tempo .25 retain a positive integer analysis
advance at the minimum window, avoiding repeated zero-distance phase estimates.

## Phase and channel handling

The automatic FFT window is the smallest power of two covering 40 ms (within
the supported 512 to 8192 frame bounds). Named Compact and Detailed profiles
select nominal 20 ms and 80 ms windows using those same bounds. Synthesis uses an eighth-window hop,
reduced further when needed to keep the analysis hop within one quarter-window.
Analysis centers are rounded from absolute positions. Phase advances use the
actual integer distance between consecutive analysis frames, not a nominal
fractional hop.

Channel pairs share one complex transform: the pair enters as real and
imaginary parts, and because the stage twiddles are exactly conjugate
symmetric, a real input's spectrum is exactly Hermitian and the two half
spectra separate without crosstalk beyond the transform's own rounding.
Synthesis packs the two rotated half spectra the same way and reads both
channels from one inverse transform. An odd trailing channel uses the
transform alone. Silent, identical or opposite channels therefore agree within
single-precision rounding (below -130 dB) rather than bit for bit.

Per-bin magnitude uses total channel power in single precision. The dominant
channel provides a phase estimate: the phase advance is the argument of the
current bin times the conjugate of its previous analysis value, with the
products formed in double precision and a series arctangent accurate to about
1e-9 rad. A common phase rotation is applied to each channel's own complex
spectrum. Summing channel waveforms to derive phase would lose opposite-phase
material. Nearest-peak identity locking constrains tonal regions while
retaining channel differences. Only peak phase estimates are propagated when
locking is active. Consecutive bins with an identical rotation reuse one
multiplier, evaluated by a double-precision series instead of a library call;
the untouched analysis spectra of every channel become the next frame's
reference by buffer exchange rather than by copy.
These relationships are numerical contracts tested independently with opposite
and unrelated channel signals.

## Attacks and envelope

A high-crest sparse attack admits explicit time anchoring: a peak at source
position `T` is mapped to `T/(t/p)` in the intermediate timeline. The required
linear spectral phase gradient shifts the attack to this position in each
frame. Peak locking is disabled for the anchored frame because it would destroy
that gradient. Analysis and synthesis window widths adapt to the time ratio,
so the transient's magnitude support matches its mapped phase. Fast playback
uses a narrower synthesis support on tonal frames as well. Overlap weights
normalize the corresponding window products.

This conservative detector does not solve arbitrary polyphonic transient
separation. Dense material retains the tonal path with high-frequency spectral
flux resets on rapidly growing bins. Switching paths can affect surrounding
tones. Sparse-click timing tests do not predict attack quality in dense mixes.
Changes to this detector need listening tests on music as well as click tests.

Optional formant preservation estimates a smoothed log spectral envelope with
a tapered cepstral lifter. Its configurable extent is 1 to 4 ms, default 2 ms;
weights stay at one through half that extent, then taper to zero. Correction
compares the source-bin envelope with the envelope at `bin * pitch / formant_scale`,
with amplitude gain bounded to 0.25 to 4. A zero formant scale follows pitch
without correction. Mixed transients retain the same sparse-attack anchoring
but suppress flux-triggered phase resets at peaks below 500 Hz.
It improves the included synthetic vowel oracle, but is approximate for real
voices and instruments and is not a source-specific formant tracker.

## Bounded FFT arithmetic

The FFT uses explicit finite complex products. Source admission and the maximum
window/gain bounds keep intermediates finite and far below float overflow.
Products are separate statements to retain rounding before their sum/difference;
this avoids the generic complex nonfinite-recovery path without asking for fast
math. Stage twiddles are stored contiguously per stage, the inverse negates the
cross terms instead of branching per butterfly, and the vocoder stores windowed
samples and packed spectra directly in bit-reversed order so no permutation pass
runs. On AArch64 the butterflies, interpolation spans, frame energy, bin
magnitudes, overlap-add and input validation use NEON intrinsics; the scalar
fallback performs the same operations in the same lane order, so results differ
only through a compiler's own contraction choices. Fast math is never enabled.
The implementation retains the independent DFT oracle.

## Timing and failures

Negative synthesis frames prime overlap history before output frame zero;
source reads outside the declared input are zero-padded. Final output is clipped
to the declared duration after tail synthesis. This compensates the sample
coordinate of the result, not the time or future audio needed to compute it.
There is no claim of live-input zero latency.

Read callbacks must supply an immutable source for an epoch. Consecutive
analysis frames overlap by at least three quarters, so the engine keeps the
previous window's validated samples and fetches only the frames beyond it.
A frame is fully read and validated before phase/overlap state commits. A failed source read or
invalid sample leaves that analysis frame retryable. Already produced samples
remain committed. Retry callers retain that prefix and resume rendering.
Invalid reset arguments leave the prior epoch intact. Successful reset removes
all overlap, phase and resampler history.

No allocation or synchronization occurs in render. Reset also allocates
nothing, but recalculates ratio-dependent tables; keep it off latency-sensitive
callbacks. Callback implementations can still allocate, block or throw; callers
own their suitability. A thrown read exception becomes source-unavailable, but
callbacks should use the explicit error return instead.

## Audition host

The reusable processor feeds two preallocated DSP epochs into an equal-gain
linear crossfade. The example player decodes immutable stereo PCM and uses
that processor through the public C++ wrapper. Tempo and pitch are separate ratios.
Master Tempo keeps the selected pitch while changing tempo; disabling it sets
pitch equal to tempo. Bypass sets both to one. A packed lock-free command word
publishes all controls coherently, with 0.01-semitone, 0.000001-tempo and 0.01 ms envelope
resolution. The library itself retains double-precision controls. Repeated requests coalesce during the 1024-frame transition.

A new epoch prerolls from earlier source audio and starts at the nearest output
sample to the current source position (at most one input sample of alignment
error at the app's supported speeds). The processor computes source position from an absolute output counter in each
trajectory, independently of callback sizes.
Each queue frame carries its source position. During a speed crossfade the new
epoch defines that timeline; the old epoch contributes only its fading audio.
The final source position is the source length. Fixed-speed duration is exactly
`ceil(input_frames / tempo)`; automation duration depends on when queued control
changes take effect. No constant control-response latency is promised.

Only the worker owns DSP state. The audio callback consumes a bounded SPSC
queue, emits silence on underrun, and advances its source counter only for
consumed frames. The producer fills even partial free queue space to support
mixed callback sizes. Destruction stops and joins the worker before source
storage can retire. A captured shared owner keeps playback alive for the native
callback. The example requires always-lock-free scalar atomics at compile time.

Seek requests use a coherent frame/serial mailbox. The worker completes any
pending processor fade, obtains queue ownership, seeks, discards queued frames
and publishes the new source position. The callback makes one nonblocking atomic
ownership attempt. It fades to silence if that handoff is busy and blends into
new audio across 128 consumed frames. It never waits for preroll. Only the worker
can flush the queue; normal production remains single-producer/single-consumer.
Queue counters stay monotonic across seeks. Delivered playback counts exclude
frames discarded by seek. The worker remains available at EOF, so paused and
ended players can seek without a running audio device.

The app shares a const decoded source across profile changes and recreates only
the processor/player and audio graph. Its default output gain is -9 dB, applied
equally to bypass and processed audio. There is no limiter. Tests render the
actual native view hierarchy offscreen and exercise its transport/control handlers
without recording the desktop or opening a playback device.

See [API.md](API.md) for control failure rollback, staged crossfade reads and
explicit seek discontinuities.

## Evaluation

Use blinded, level-controlled listening on vocals, exposed bass, percussion,
full mixes, sustained instruments, stereo ambience and extreme ratios. Include
short excerpts and switching behavior, rather than relying on one aggregate
score. Record CPU tail latency, memory and missed deadlines on the intended
hardware. Static-ratio tests do not cover every control transition or system audio
deadline. See [QUALITY.md](QUALITY.md) for the measurement and listening procedure.
