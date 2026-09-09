// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "fft.hpp"
#include <algorithm>
#include <cmath>

namespace chronobent_dsp {
Fft::Fft(std::size_t size) : size_(size), reverse_(size), roots_(size / 2) {
    for (std::size_t i = 1; i < size; ++i)
        reverse_[i] = (reverse_[i / 2] >> 1) | ((i & 1) * (size / 2));
    for (std::size_t i = 0; i < roots_.size(); ++i) {
        const double angle = -2 * pi * static_cast<double>(i) / static_cast<double>(size);
        roots_[i] = Complex(static_cast<float>(std::cos(angle)),
                            static_cast<float>(std::sin(angle)));
    }
}

void Fft::transform(Complex *v, bool inverse) const noexcept {
    for (std::size_t i = 0; i < size_; ++i)
        if (i < reverse_[i]) std::swap(v[i], v[reverse_[i]]);
    for (std::size_t width = 2; width <= size_; width *= 2) {
        const std::size_t half = width / 2, stride = size_ / width;
        for (std::size_t base = 0; base < size_; base += width) {
            for (std::size_t k = 0; k < half; ++k) {
                const Complex root = inverse ? std::conj(roots_[k * stride]) : roots_[k * stride];
                const Complex a = v[base + k], b = v[base + k + half] * root;
                v[base + k] = a + b;
                v[base + k + half] = a - b;
            }
        }
    }
    if (inverse) {
        const float scale = 1.0f / static_cast<float>(size_);
        for (std::size_t i = 0; i < size_; ++i) v[i] *= scale;
    }
}
}
