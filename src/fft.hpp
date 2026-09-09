// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_FFT_HPP
#define CHRONOBENT_FFT_HPP
#include <complex>
#include <cstddef>
#include <vector>

namespace chronobent_dsp {
constexpr double pi = 3.141592653589793238462643383279502884;
using Complex = std::complex<float>;

// In-place radix-2 transform. Tables are immutable after construction;
// caller owns scratch. The inverse includes 1/N normalization.
class Fft {
public:
    explicit Fft(std::size_t size);
    void transform(Complex *values, bool inverse) const noexcept;
private:
    std::size_t size_;
    std::vector<std::size_t> reverse_;
    std::vector<Complex> roots_;
};
}
#endif
