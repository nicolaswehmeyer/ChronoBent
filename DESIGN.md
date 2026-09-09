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

Each instance owns its tables and working buffers. The standard C++ library is
required. File and audio-device APIs belong to the examples.

## Phase and channel handling

The automatic FFT window is the smallest power of two covering 40 ms (within
the supported 512 to 8192 frame bounds). Synthesis uses an eighth-window hop,
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
a tapered 1 to 2 ms cepstral lifter. Correction compares the envelope at the source
bin and its final pitched frequency, with amplitude gain bounded to 0.25 to 4.
It improves the included synthetic vowel oracle, but is approximate for real
voices and instruments and is not a source-specific formant tracker.

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

The example player decodes immutable stereo PCM and feeds two independent DSP
epochs into an equal-gain linear crossfade. Tempo and pitch are separate ratios.
Master Tempo keeps the selected pitch while changing tempo; disabling it sets
pitch equal to tempo. Bypass sets both to one. A packed lock-free command word
publishes all controls coherently, with 0.00001-semitone and 0.000001-tempo
resolution. Repeated requests coalesce during the 1024-frame transition.

A new epoch prerolls from earlier source audio and starts at the nearest output
sample to the current source position (at most one input sample of alignment
error at the app's supported speeds). The host source timeline advances in
floating-point source frames per output frame, independently of callback sizes.
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

## Evaluation

Use blinded, level-controlled listening on vocals, exposed bass, percussion,
full mixes, sustained instruments, stereo ambience and extreme ratios. Include
short excerpts and switching behavior, rather than relying on one aggregate
score. Record CPU tail latency, memory and missed deadlines on the intended
hardware. Static-ratio tests do not cover every control transition or system audio
deadline. See [QUALITY.md](QUALITY.md) for the measurement and listening procedure.
