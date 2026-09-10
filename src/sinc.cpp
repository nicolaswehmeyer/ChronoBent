// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "sinc.hpp"
#include "fft.hpp"
#include <algorithm>
#include <cmath>

namespace chronobent_dsp {
std::size_t Sinc::capacity(double step) noexcept {
    return std::max(std::size_t(96), std::size_t(std::ceil(24 * step)) * 2);
}
Sinc::Sinc(double maximum_pitch) : table_((phases + 1) * capacity(maximum_pitch)) {}

void Sinc::prepare(double step) noexcept {
    const double cutoff = 0.94 / std::max(1.0, step);
    if (cutoff == cutoff_) return;
    cutoff_ = cutoff;
    taps_ = capacity(step);
    for (std::size_t phase = 0; phase <= phases; ++phase) {
        const double fraction = static_cast<double>(phase) / static_cast<double>(phases);
        double total = 0;
        for (std::size_t tap = 0; tap < taps_; ++tap) {
            const double x = static_cast<double>(tap) - left() - fraction;
            const double radius = x / static_cast<double>(taps_ / 2);
            const double window = std::abs(radius) >= 1 ? 0 :
                0.42 + 0.5 * std::cos(pi * radius) + 0.08 * std::cos(2 * pi * radius);
            const double value = window * (std::abs(x) < 1e-12 ? cutoff :
                std::sin(pi * cutoff * x) / (pi * x));
            table_[phase * taps_ + tap] = static_cast<float>(value);
            total += value;
        }
        for (std::size_t tap = 0; tap < taps_; ++tap)
            table_[phase * taps_ + tap] /= static_cast<float>(total);
    }
}

void Sinc::coefficients(double fraction, float *out) const noexcept {
    const double position = fraction * static_cast<double>(phases);
    const auto index = std::min(phases - 1, static_cast<std::size_t>(position));
    const float blend = static_cast<float>(position - static_cast<double>(index));
    const float *a = table_.data() + index * taps_, *b = a + taps_;
    for (std::size_t i = 0; i < taps_; ++i) out[i] = a[i] + blend * (b[i] - a[i]);
}
}
