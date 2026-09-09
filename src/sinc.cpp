// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "sinc.hpp"
#include "fft.hpp"
#include <algorithm>
#include <cmath>

namespace chronobent_dsp {
Sinc::Sinc() : table_((phases + 1) * taps) {}

void Sinc::prepare(double step) noexcept {
    const double cutoff = 0.94 / std::max(1.0, step);
    if (cutoff == cutoff_) return;
    cutoff_ = cutoff;
    for (std::size_t phase = 0; phase <= phases; ++phase) {
        const double fraction = static_cast<double>(phase) / static_cast<double>(phases);
        double total = 0;
        for (std::size_t tap = 0; tap < taps; ++tap) {
            const double x = static_cast<double>(tap) - left - fraction;
            const double radius = x / static_cast<double>(taps / 2);
            const double window = std::abs(radius) >= 1 ? 0 :
                0.42 + 0.5 * std::cos(pi * radius) + 0.08 * std::cos(2 * pi * radius);
            const double value = window * (std::abs(x) < 1e-12 ? cutoff :
                std::sin(pi * cutoff * x) / (pi * x));
            table_[phase * taps + tap] = static_cast<float>(value);
            total += value;
        }
        for (std::size_t tap = 0; tap < taps; ++tap)
            table_[phase * taps + tap] /= static_cast<float>(total);
    }
}

void Sinc::coefficients(double fraction, std::array<float, taps> &out) const noexcept {
    const double position = fraction * static_cast<double>(phases);
    const auto index = std::min(phases - 1, static_cast<std::size_t>(position));
    const float blend = static_cast<float>(position - static_cast<double>(index));
    const float *a = table_.data() + index * taps, *b = a + taps;
    for (std::size_t i = 0; i < taps; ++i) out[i] = a[i] + blend * (b[i] - a[i]);
}
}
