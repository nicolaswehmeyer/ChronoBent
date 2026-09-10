// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_EFFECT_HPP
#define CHRONOBENT_EFFECT_HPP
#include "instrument.hpp"
#include <memory>

namespace chronobent_effect {
enum class Mode { live, record, loop };
struct Controls {
    double semitones=0, tempo=1, formant_scale=0, mix=1, gain_db=0;
    unsigned transients=2;
    Mode mode=Mode::live;
};
struct Snapshot {
    Mode mode=Mode::live;
    double captured_seconds=0, capture_progress=0, position=0;
    uint64_t underruns=0, dropped=0, invalid_samples=0, failures=0;
    bool ready=false,auto_completed=false;
    std::string error;
    std::array<float,256> waveform{};
};
// A fixed-cadence stereo effect host, outside the pull-DSP source inventory.
// One audio owner supplies input and consumes timestamped output. A worker owns
// the DSP and a bounded cache of immutable logical input frames; missing/retired
// frames are unavailable, never returned under a different absolute index.
// Capture freezes an owned source of at most 30 seconds. Only that finite source
// can change tempo; live input always retains the host's frame cadence.
// Construction, destruction, snapshots and restore are non-audio operations.
// Suspend processing before destroying/replacing the engine. The live process
// path performs no allocation, mutex wait, I/O or DSP control/reset work.
class Engine {
public:
    static constexpr unsigned capture_seconds=30;
    explicit Engine(double sample_rate);
    ~Engine();
    Engine(const Engine &)=delete;
    Engine &operator=(const Engine &)=delete;
    unsigned latency_frames() const noexcept;
    // Audio-owner discontinuity: discard old queued sound with a generation
    // change and clear the bounded dry delay. DSP reset remains on the worker.
    void reset_audio() noexcept;
    void process(const float *left,const float *right,float *out_left,float *out_right,
                 std::size_t frames,const Controls &,bool offline=false,
                 unsigned offline_budget_ms=10000) noexcept;
    Snapshot snapshot() const;
    std::shared_ptr<const chronobent_instrument::Source> captured_source() const;
    // Invalid state preserves the previous capture. Source rate must match.
    // A null source explicitly clears the capture.
    bool restore_capture(std::shared_ptr<const chronobent_instrument::Source>);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
