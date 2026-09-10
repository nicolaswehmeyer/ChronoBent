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
| `fft` | Iterative radix-2 complex FFT, precomputed twiddles, normalized inverse |
| `vocoder` | Centered analysis, shared phase rotation, peak regions, sparse attacks, normalized overlap-add |
| `envelope` | Tapered cepstral smoothing and bounded spectral-envelope correction |
| `sinc` | 96-tap, 1024-phase interpolated Blackman-windowed low-pass resampling |
| `chronobent` | C ABI, validation, reset epochs, clipped source reads, ring storage and exact output length |
| `controls` | Shared validation and legacy/extended parameter translation |
| `processor` | Bound source, preallocated epoch pair, control preroll, retryable crossfades, seek, planar output and position |

Each instance owns its tables and working buffers. The standard C++ library is
required. File and audio-device APIs belong to the examples.

## Phase and channel handling

The automatic FFT window is the smallest power of two covering 40 ms (within
the supported 512 to 8192 frame bounds). Named Compact and Detailed profiles
select nominal 20 ms and 80 ms windows using those same bounds. Synthesis uses an eighth-window hop,
reduced further when needed to keep the analysis hop within one quarter-window.
Analysis centers are rounded from absolute positions. Phase advances use the
actual integer distance between consecutive analysis frames, not a nominal
fractional hop.

Per-bin magnitude uses total channel power. The dominant channel provides a
phase estimate, comparing that same channel across both frames. A common phase
rotation is applied to each channel's own complex spectrum. Summing channel
waveforms to derive phase would lose opposite-phase material. Nearest-peak
identity locking constrains tonal regions while retaining channel differences.
Only peak phase estimates are propagated when locking is active. Consecutive
bins with an identical rotation reuse one sine/cosine multiplier; phase history
for every channel is still retained for later reference changes.
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
math. The first combined-expression prototype changed rounding and was rejected.
The implementation retains the independent DFT oracle and frozen-output checks.

## Timing and failures

Negative synthesis frames prime overlap history before output frame zero;
source reads outside the declared input are zero-padded. Final output is clipped
to the declared duration after tail synthesis. This compensates the sample
coordinate of the result, not the time or future audio needed to compute it.
There is no claim of live-input zero latency.

Read callbacks must supply an immutable source for an epoch. A frame is fully
read and validated before phase/overlap state commits. A failed source read or
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
