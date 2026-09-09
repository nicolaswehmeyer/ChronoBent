// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_SINC_HPP
#define CHRONOBENT_SINC_HPP
#include <array>
#include <cstddef>
#include <vector>

namespace chronobent_dsp {
// 96-tap Blackman-windowed lowpass, 1024 sub-sample phases plus an endpoint.
// Cutoff follows the resampling ratio; adjacent table phases interpolate.
// Coefficients are normalized at DC. Preparation is off the render path.
class Sinc {
public:
    static constexpr std::size_t taps = 96, phases = 1024;
    static constexpr int left = static_cast<int>(taps / 2) - 1;
    static constexpr int right = static_cast<int>(taps / 2);
    Sinc();
    void prepare(double step) noexcept;
    void coefficients(double fraction, std::array<float, taps> &out) const noexcept;
private:
    std::vector<float> table_;
    double cutoff_ = 0;
};
}
#endif
