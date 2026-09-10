// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "vocoder.hpp"
#include <algorithm>
#include <cmath>

namespace chronobent_dsp {
namespace {
double wrap(double x) noexcept { return x - 2 * pi * std::floor(x / (2 * pi) + 0.5); }
}

Vocoder::Vocoder(std::size_t size, std::size_t channels, double sample_rate, bool transients, bool formants)
    : size_(size), channels_(channels), bins_(size / 2 + 1), hop_(size / 8),
      flux_min_(std::max<std::size_t>(1, static_cast<std::size_t>(180 * static_cast<double>(size) / sample_rate))),
      transients_(transients), formant_scale_(formants ? 1 : 0), sample_rate_(sample_rate), fft_(size), envelope_(size, sample_rate), window_(size), attack_analysis_(size), attack_synthesis_(size), overlap_(size * channels), weight_(size),
      spectrum_(size * channels), previous_spectrum_(bins_ * channels),
      magnitude_(bins_), previous_magnitude_(bins_), rotation_(bins_), next_rotation_(bins_),
      reference_(bins_), peaks_(bins_) {
    for (std::size_t i = 0; i < size_; ++i)
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * pi * static_cast<double>(i) / static_cast<double>(size_)));
}

void Vocoder::reset(double rate, double pitch) noexcept {
    rate_ = rate;
    pitch_ = pitch;
    hop_ = size_ / 8;
    // Keep both analysis and synthesis windows overlapped at fast tempos.
    while (static_cast<double>(hop_) * rate_ > static_cast<double>(size_ / 4)) hop_ /= 2;
    start_ = -static_cast<std::int64_t>(size_) + static_cast<std::int64_t>(hop_);
    previous_analysis_ = 0;
    primed_ = false;
    hold_ = 0;
    flux_average_ = 0;
    for (std::size_t i = 0; i < size_; ++i) {
        const double offset = 2 * static_cast<double>(i) / static_cast<double>(size_) - 1;
        const auto taper = [offset](double width) {
            return std::abs(offset) < width ? static_cast<float>(0.5 + 0.5 * std::cos(pi * offset / width)) : 0.0f;
        };
        attack_analysis_[i] = taper(std::min(1.0, rate_));
        attack_synthesis_[i] = taper(std::min(1.0, 1 / rate_));
    }
    std::fill(overlap_.begin(), overlap_.end(), 0.0f);
    std::fill(weight_.begin(), weight_.end(), 0.0f);
    std::fill(rotation_.begin(), rotation_.end(), 0.0);
    std::fill(previous_spectrum_.begin(), previous_spectrum_.end(), Complex(0, 0));
    std::fill(previous_magnitude_.begin(), previous_magnitude_.end(), 0.0);
}

std::int64_t Vocoder::analysis_start() const noexcept {
    // Centered frames avoid a ratio-dependent half-window timeline offset.
    return static_cast<std::int64_t>(std::llround(
        static_cast<double>(start_ + static_cast<std::int64_t>(size_ / 2)) * rate_)) -
        static_cast<std::int64_t>(size_ / 2);
}

void Vocoder::process(const float *input, float *output) noexcept {
    const auto analysis = analysis_start();
    const double advance = static_cast<double>(analysis - previous_analysis_);
    // Sparse attacks have an observable time anchor. Map that anchor explicitly
    // instead of resetting every overlapping frame at a different output time.
    // Conservative crest admission leaves sustained and dense material on the
    // tonal path. This is not a general percussive/source decomposition.
    double total = 0, maximum = 0;
    std::size_t anchor = 0;
    if (transients_) for (std::size_t i = 0; i < size_; ++i) {
        double energy = 0;
        for (std::size_t c = 0; c < channels_; ++c) {
            const double value = input[i * channels_ + c];
            energy += value * value;
        }
        total += energy;
        if (energy > maximum) { maximum = energy; anchor = i; }
    }
    const bool anchored = maximum > 1e-8 && maximum * static_cast<double>(size_) > 32 * total;
    const auto &analysis_window = anchored ? attack_analysis_ : window_;
    const auto &synthesis_window = attack_synthesis_;
    const double anchor_shift = (static_cast<double>(analysis) + static_cast<double>(anchor)) / rate_ -
        static_cast<double>(start_) - static_cast<double>(anchor);
    for (std::size_t c = 0; c < channels_; ++c) {
        Complex *s = spectrum_.data() + c * size_;
        for (std::size_t i = 0; i < size_; ++i) s[i] = Complex(input[i * channels_ + c] * analysis_window[i], 0);
        fft_.transform(s, false);
    }
    double flux = 0, high_energy = 0;
    for (std::size_t k = 0; k < bins_; ++k) {
        double energy = 0, largest = -1;
        for (std::size_t c = 0; c < channels_; ++c) {
            const double power = std::norm(spectrum_[c * size_ + k]);
            energy += power;
            if (power > largest) { largest = power; reference_[k] = c; }
        }
        magnitude_[k] = std::sqrt(energy);
        if (k >= flux_min_) {
            flux += std::max(0.0, magnitude_[k] - previous_magnitude_[k]);
            high_energy += magnitude_[k];
        }
    }
    const bool onset = transients_ && primed_ && hold_ == 0 && high_energy > 1e-5 &&
        flux > 0.25 * high_energy && flux > 2.0 * flux_average_;
    flux_average_ = 0.9 * flux_average_ + 0.1 * flux;
    if (onset) hold_ = 3;
    else if (hold_ != 0) --hold_;

    std::size_t peak_count = 0;
    for (std::size_t k = 1; k + 1 < bins_; ++k)
        if (magnitude_[k] > magnitude_[k - 1] && magnitude_[k] >= magnitude_[k + 1] && magnitude_[k] > 1e-8)
            peaks_[peak_count++] = k;
    // Locked regions consume only their peak's phase estimate. Avoid computing
    // discarded estimates for every non-peak bin; anchored frames need none.
    // Retain every channel's original spectrum because peaks and reference
    // channels can change. Evaluate phase only where propagation consumes it.
    const auto propagate = [&](std::size_t k) {
        const std::size_t index = reference_[k] * bins_ + k;
        const bool reset_band = !mixed_ || double(k) * sample_rate_ / double(size_) >= 500;
        if (!primed_ || (onset && reset_band && magnitude_[k] > 1.5 * previous_magnitude_[k])) {
            next_rotation_[k] = 0;
        } else {
            const Complex now = spectrum_[reference_[k] * size_ + k];
            const Complex before = previous_spectrum_[index];
            const double phase = std::atan2(now.imag(), now.real());
            const double previous_phase = std::atan2(before.imag(), before.real());
            const double omega = 2 * pi * static_cast<double>(k) / static_cast<double>(size_);
            const double delta = omega * advance + wrap(phase - previous_phase - omega * advance);
            next_rotation_[k] = wrap(rotation_[k] + delta * (static_cast<double>(hop_) / advance - 1));
        }
    };
    if (!anchored) {
        if (peak_count) for (std::size_t i = 0; i < peak_count; ++i) propagate(peaks_[i]);
        else for (std::size_t k = 1; k + 1 < bins_; ++k) propagate(k);
    }

    // Identity phase locking: a peak's phase rotation is shared by its region.
    // Source channel magnitudes and relative phases remain independent.
    std::size_t region = 0;
    for (std::size_t k = 1; k + 1 < bins_; ++k) {
        if (anchored) {
            rotation_[k] = wrap(-2 * pi * static_cast<double>(k) * anchor_shift / static_cast<double>(size_));
        } else if (peak_count) {
            while (region + 1 < peak_count && 2 * k > peaks_[region] + peaks_[region + 1]) ++region;
            rotation_[k] = next_rotation_[peaks_[region]];
        } else rotation_[k] = next_rotation_[k];
    }
    rotation_[0] = 0;
    rotation_[bins_ - 1] = anchored ? wrap(-pi * anchor_shift) : 0;
    const double envelope_ratio = formant_scale_ == 0 ? 1 : pitch_ / formant_scale_;
    if (envelope_ratio != 1) envelope_.analyze(magnitude_.data());
    // Save the analysis spectrum before applying any synthesis rotation/gain.
    for (std::size_t c = 0; c < channels_; ++c)
        std::copy_n(spectrum_.data() + c * size_, bins_, previous_spectrum_.data() + c * bins_);
    Complex turn;
    for (std::size_t k = 0; k < bins_; ++k) {
        // A locked region shares one rotation. Reuse its complex multiplier
        // instead of evaluating identical sine/cosine pairs for every bin.
        if (k == 0 || rotation_[k] != rotation_[k-1] ||
            std::signbit(rotation_[k]) != std::signbit(rotation_[k-1]))
            turn = Complex(static_cast<float>(std::cos(rotation_[k])), static_cast<float>(std::sin(rotation_[k])));
        const float gain = envelope_ratio != 1 ? envelope_.correction(k, envelope_ratio) : 1;
        for (std::size_t c = 0; c < channels_; ++c) {
            Complex *s = spectrum_.data() + c * size_;
            s[k] *= turn * gain;
            // The unpaired real-FFT endpoints must remain real. A fractional
            // attack shift scales Nyquist by cos(pi*shift); leaving it unchanged
            // would spread a small alternating residue around the attack.
            if (k == 0 || k + 1 == bins_) s[k] = Complex(s[k].real(), 0);
            if (k != 0 && k + 1 != bins_) s[size_ - k] = std::conj(s[k]);
        }
    }
    for (std::size_t c = 0; c < channels_; ++c) {
        Complex *s = spectrum_.data() + c * size_;
        fft_.transform(s, true);
        for (std::size_t i = 0; i < size_; ++i) overlap_[i * channels_ + c] += s[i].real() * synthesis_window[i];
    }
    for (std::size_t i = 0; i < size_; ++i)
        weight_[i] += synthesis_window[i] * (anchored ? synthesis_window[i] : window_[i]);
    for (std::size_t i = 0; i < hop_; ++i)
        for (std::size_t c = 0; c < channels_; ++c)
            output[i * channels_ + c] = weight_[i] > 1e-8f ? overlap_[i * channels_ + c] / weight_[i] : 0;
    std::move(overlap_.begin() + static_cast<std::ptrdiff_t>(hop_ * channels_), overlap_.end(), overlap_.begin());
    std::fill(overlap_.end() - static_cast<std::ptrdiff_t>(hop_ * channels_), overlap_.end(), 0.0f);
    std::move(weight_.begin() + static_cast<std::ptrdiff_t>(hop_), weight_.end(), weight_.begin());
    std::fill(weight_.end() - static_cast<std::ptrdiff_t>(hop_), weight_.end(), 0.0f);
    std::copy(magnitude_.begin(), magnitude_.end(), previous_magnitude_.begin());
    previous_analysis_ = analysis;
    primed_ = true;
    start_ += static_cast<std::int64_t>(hop_);
}
}
