// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_ENVELOPE_HPP
#define CHRONOBENT_ENVELOPE_HPP
#include "fft.hpp"
#include <vector>

namespace chronobent_dsp {
// Homomorphic spectral envelope. A tapered low-quefrency lifter separates
// broad resonances from individual partials. This is an approximate timbre
// control, not a voice model or source separator.
class Envelope {
public:
    Envelope(std::size_t size, double sample_rate);
    void configure(double milliseconds) noexcept;
    void analyze(const double *magnitudes) noexcept;
    float correction(std::size_t bin, double pitch) const noexcept;
private:
    std::size_t size_, cutoff_;
    double sample_rate_;
    Fft fft_;
    std::vector<Complex> scratch_;
};
}
#endif
