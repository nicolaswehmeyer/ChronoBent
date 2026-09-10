// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_INSTRUMENT_HPP
#define CHRONOBENT_INSTRUMENT_HPP
#include "chronobent/chronobent.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace chronobent_instrument {
struct Source {
    double sample_rate = 48000;
    std::string name;
    std::vector<float> stereo;
};
struct Preparation {
    double tempo = 1, semitones = 0, formant_scale = 0, envelope_ms = 2;
    uint32_t transients = 2;
    chronobent_profile profile = CHRONOBENT_PROFILE_BALANCED;
    int root_note = 60;
};
struct Performance {
    double attack_ms = 8, release_ms = 240, gain_db = -9;
    bool loop = false;
};
struct Snapshot {
    bool ready = false, preparing = false;
    double progress = 0, sample_rate = 0, duration = 0;
    uint64_t revision = 0, underruns = 0, dropped_notes = 0;
    unsigned active_voices = 0;
    std::array<int,4> active_notes{{-1,-1,-1,-1}};
    std::string name, error;
    Preparation preparation;
    std::array<float,256> waveform{};
};
// A sample instrument host, separate from the DSP library. Source submission and
// snapshot/state access are control-thread operations. MIDI and render share one
// audio thread. Source buffers stay immutable. Four workers stream transformed
// voices after a prepared attack; the live callback performs no allocation,
// mutex wait, file I/O or DSP reset. Offline rendering may wait for those workers.
class Engine {
public:
    static constexpr unsigned voice_count = 4;
    static constexpr int lowest_key = -24, highest_key = 24, key_count = 49;
    explicit Engine(double rate = 48000);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    // rate must match source.sample_rate. Invalid requests preserve the source.
    bool prepare(std::shared_ptr<const Source>, Preparation);
    Snapshot snapshot() const;
    bool wait_ready(unsigned milliseconds = 60000) const;
    bool note_on(int note, double velocity) noexcept;
    void note_off(int note) noexcept;
    void sustain(bool held) noexcept;
    void all_notes_off() noexcept;
    // Called only with processing suspended or by the single audio owner.
    void reset() noexcept;
    void render(float *left, float *right, std::size_t frames,
                const Performance &, bool offline = false) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::shared_ptr<const Source> factory_source(unsigned preset, double rate);
}
#endif
