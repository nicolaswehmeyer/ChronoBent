// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "envelope.hpp"
#include <algorithm>
#include <cmath>

namespace chronobent_dsp {
Envelope::Envelope(std::size_t size, double sample_rate)
    : size_(size), cutoff_(std::min(size / 4, static_cast<std::size_t>(sample_rate * 0.002))),
      sample_rate_(sample_rate), fft_(size), scratch_(size) {}

void Envelope::configure(double milliseconds) noexcept {
    cutoff_ = std::min(size_ / 4, static_cast<std::size_t>(sample_rate_ * (milliseconds * .001)));
}

void Envelope::analyze(const float *magnitudes) noexcept {
    double maximum = 1e-12;
    for (std::size_t k = 0; k <= size_ / 2; ++k) maximum = std::max(maximum, double(magnitudes[k]));
    for (std::size_t k = 0; k <= size_ / 2; ++k) {
        scratch_[k] = Complex(static_cast<float>(std::log(std::max(double(magnitudes[k]), maximum * 1e-5))), 0);
        if (k && k < size_ / 2) scratch_[size_ - k] = scratch_[k];
    }
    fft_.transform(scratch_.data(), true);
    for (std::size_t k = 1; k < size_; ++k) {
        const auto quefrency = std::min(k, size_ - k);
        double weight = 0;
        if (quefrency <= cutoff_ / 2) weight = 1;
        else if (quefrency < cutoff_)
            weight = 0.5 + 0.5 * std::cos(pi * (double(quefrency) - double(cutoff_) / 2) / (double(cutoff_) / 2));
        scratch_[k] *= static_cast<float>(weight);
    }
    fft_.transform(scratch_.data(), false);
}

float Envelope::correction(std::size_t bin, double pitch) const noexcept {
    const double target = std::min(double(size_ / 2), double(bin) * pitch);
    const auto lower = static_cast<std::size_t>(target), upper = std::min(size_ / 2, lower + 1);
    const double mix = target - double(lower);
    const double wanted = (1 - mix) * scratch_[lower].real() + mix * scratch_[upper].real();
    // Bound boosts/cuts to 12 dB, including deep notches and silent frames.
    return static_cast<float>(std::exp(std::clamp(wanted - scratch_[bin].real(), -std::log(4.0), std::log(4.0))));
}
}
