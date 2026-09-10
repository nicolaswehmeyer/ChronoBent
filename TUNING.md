# Note Studio and the optional tuning API

ChronoBent 0.6 adds correction of a recorded **single voice or monophonic melody**.
Open Note Studio in Lab, Instrument or FX, analyze the source, choose a key and
scale, and adjust automatic or individual note targets. This does not separate
voices from a mix or provide low-latency microphone tuning.

## Using Note Studio

1. Load a file in Lab or Instrument, or finish a capture in FX. Open **Note Studio**.
2. Press **Analyze melody**. The graph shows the detected source contour in gray,
   the planned corrected contour in mint, and selectable note regions.
3. Choose chromatic, major or natural minor tuning and its key. A4 defaults to
   440 Hz and accepts 400–480 Hz. The limit is ±1, ±2 or ±5 semitones.
4. Select a note and drag vertically, use Up/Down, or type its MIDI target.
   Option-drag adjusts cents; Left/Right selects adjacent notes. Delete or
   **Reset note** returns the selected note to automatic tuning. Targets outside
   the selected correction limit are refused.
5. Adjust **Correction**, **Retune**, **Keep vibrato** and **Correct drift**.
   Wait for processing, then select **Use tuned audio**. **Use original audio**
   restores the unmodified source. Re-analysis retains saved manual targets.

Correction controls the applied fraction of the planned shift. Retune smooths
changes in correction over 0–400 ms; it is not an audio latency setting. Keep
vibrato preserves the fast component of the detected contour, while Correct
drift removes its slower movement within a note. Start with vibrato retained
and drift correction off, then compare the result at a matched playback level.
Uncertain/unvoiced sections stay unchanged, with fades at admitted voiced edges.
The graph represents an estimate; octave and note-boundary mistakes remain
possible. Verify each important phrase by ear.

Lab supports tuning sources up to 600 seconds, Instrument up to 120 seconds,
and FX captures up to 30 seconds. The input remains in memory. Changing source
invalidates previous analysis; generation checks prevent stale work from being
applied to a replacement source. Closing the editor retains the host session.
The UI and audio callback do not perform analysis or source conversion.

Instrument and FX save the original and selected corrected PCM, controls and
manual targets inside their version 2 project state. A project can be reopened
without the source file, return to original, and reanalyze for further editing.
Original sample-rate PCM is retained even if the host later changes its rate.
0.5 source-only states remain readable; older plugins cannot read a 0.6 dual-source
state. Both versions of the PCM increase project size and host memory usage.

Lab's **Export tuned WAV** writes the selected corrected source as stereo float32
WAV at its original rate. It exports before Lab's global pitch, speed, timbre and
output controls. The save panel explains this scope. An export is written to a
temporary file beside the destination and atomically replaces it only on success.

## Embedding the library

`CHRONOBENT_BUILD_TUNING=ON` is the standalone CMake default. Link the same
`chronobent::chronobent` target and include `chronobent/tuning.h` (C ABI) or
`chronobent/tuning.hpp` (move-only C++ wrapper). A complete compiled example is
[examples/tuning_cpp.cpp](examples/tuning_cpp.cpp); the
[C consumer](tests/test_tuning_c.c) exercises the same API without C++ syntax.

For source integration, retain [sources.txt](sources.txt) and additionally compile
[tuning-sources.txt](tuning-sources.txt). The tuning module reuses the existing
FFT and sinc implementation; do not compile duplicate copies. Set
`CHRONOBENT_BUILD_TUNING=OFF` for an SDK without tuning symbols or analysis cost.
The legacy source inventory is unchanged. The 0.6 desktop tools require ON.
CMake's imported target exposes `CHRONOBENT_HAS_TUNING` as 0 or 1.

Create a tuner for a fixed source length/rate/channel count on a worker, supply
an immutable random-access source callback to `chronobent_tune_analyze`, then
read the borrowed frame/note views. Set options and individual or batched note
edits, and render to host-owned storage. Publish only a completed immutable
result to the playback system. The existing processor can apply independent
tempo, global pitch and timbre to that result afterwards.

| Contract | Scope |
| --- | --- |
| Input | 1–2 linked channels, 8–192 kHz, at most 600 seconds |
| Detection range | Default 55–1200 Hz; configurable minimum 40–400 Hz and maximum above it up to 1600 Hz |
| Correction | 0–5 semitones maximum; default 2; automatic scale targets are bounded by that limit |
| Scale | Nonzero 12-bit absolute pitch-class mask, bit 0=C through bit 11=B |
| Manual targets | MIDI 0–127, within 5 semitones of detection; applied shift also obeys options |
| Timing | Original duration, absolute output indexing, identical samples across partitions and seeks |
| Allocation | Create/destroy only, excluding caller callbacks; derived memory grows with source duration |
| Ownership | Host owns PCM, files, worker, scheduling and output; library retains no source binding |
| Threading | One caller per instance; borrowed views cannot be read concurrently with mutations |
| Identity | Amount zero preserves PCM exactly; uncertain regions retain source samples |

Invalid options or edit batches leave the previous plan intact. `set_notes`
replaces all manual edits atomically; indices must be strictly increasing.
An empty batch returns all notes to automatic selection. A valid new analysis
invalidates old results immediately. Cancellation returns `CHRONOBENT_CANCELLED`;
failed or cancelled analysis exposes no partial result and can be retried from
zero. The synchronous progress callback must not throw or reenter the tuner.

On a render source error, `produced` identifies the valid prefix. Consume it and
retry at `first_frame + produced` after resolving the source failure. Do not
change PCM identity between analysis and rendering. Output past EOF is not
written. Storage must remain available until the caller's operation finishes.
Elapsed time is never a source lease.

The implementation combines periodicity candidates and temporal tracking with
continuous pitch-synchronous overlap-add. Its envelope preservation is
approximate, and its correction range is intentionally narrower than the legacy
global pitch processor. See [QUALITY.md](QUALITY.md) for measured accuracy,
synthetic envelope checks and unresolved perceptual limits.
