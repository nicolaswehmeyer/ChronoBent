// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_SINC_HPP
#define CHRONOBENT_SINC_HPP
#include <cstddef>
#include <vector>

namespace chronobent_dsp {
// Blackman-windowed lowpass, 1024 sub-sample phases plus an endpoint.
// 96 taps through +12 semitones; 48*step taps above, rounded up to even.
// Cutoff follows the resampling ratio; adjacent table phases interpolate.
// Coefficients are normalized at DC. Preparation is off the render path.
class Sinc {
public:
    static constexpr std::size_t phases = 1024;
    explicit Sinc(double maximum_pitch);
    static std::size_t capacity(double step) noexcept;
    std::size_t taps() const noexcept { return taps_; }
    int left() const noexcept { return static_cast<int>(taps_ / 2) - 1; }
    int right() const noexcept { return static_cast<int>(taps_ / 2); }
    void prepare(double step) noexcept;
    void coefficients(double fraction, float *out) const noexcept;
private:
    std::vector<float> table_;
    std::size_t taps_ = 96;
    double cutoff_ = 0;
};
}
#endif
