// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_FFT_HPP
#define CHRONOBENT_FFT_HPP
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace chronobent_dsp {
constexpr double pi = 3.141592653589793238462643383279502884;
using Complex = std::complex<float>;

// In-place radix-2 transform. Tables are immutable after construction;
// caller owns scratch. The inverse includes 1/N normalization.
// Stage twiddles are stored contiguously and exactly conjugate-symmetric, so
// a real input yields an exactly Hermitian spectrum. Two real channels can
// therefore share one complex transform and separate without crosstalk.
class Fft {
public:
    explicit Fft(std::size_t size);
    std::size_t size() const noexcept { return size_; }
    void transform(Complex *values, bool inverse) const noexcept;
    // A caller that stores element i at permutation()[i] skips the reordering pass.
    const std::uint32_t *permutation() const noexcept { return reverse_.data(); }
    void transform_permuted(Complex *values, bool inverse) const noexcept;
private:
    template <bool Inverse, bool Permute> void stages(Complex *values) const noexcept;
    std::size_t size_;
    std::vector<std::uint32_t> reverse_;
    std::vector<Complex> twiddles_;
};
}
#endif
