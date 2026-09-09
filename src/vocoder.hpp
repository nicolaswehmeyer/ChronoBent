// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_VOCODER_HPP
#define CHRONOBENT_VOCODER_HPP
#include "fft.hpp"
#include "envelope.hpp"
#include <cstdint>
#include <vector>

namespace chronobent_dsp {
class Vocoder {
public:
    Vocoder(std::size_t size, std::size_t channels, double sample_rate, bool transients, bool formants);
    void reset(double rate, double pitch) noexcept;
    // One interleaved, window-sized input frame; no retained input pointer.
    // Writes hop() interleaved samples, beginning at synthesis_start().
    void process(const float *input, float *output) noexcept;
    std::size_t hop() const noexcept { return hop_; }
    std::int64_t analysis_start() const noexcept;
    std::int64_t synthesis_start() const noexcept { return start_; }
private:
    std::size_t size_, channels_, bins_, hop_, flux_min_;
    bool transients_, formants_, primed_ = false;
    double rate_ = 1, pitch_ = 1, flux_average_ = 0;
    unsigned hold_ = 0;
    std::int64_t start_ = 0, previous_analysis_ = 0;
    Fft fft_;
    Envelope envelope_;
    std::vector<float> window_, attack_analysis_, attack_synthesis_, overlap_, weight_;
    std::vector<Complex> spectrum_;
    std::vector<double> phase_, previous_phase_, magnitude_, previous_magnitude_, rotation_, next_rotation_;
    std::vector<std::size_t> reference_, peaks_;
};
}
#endif
