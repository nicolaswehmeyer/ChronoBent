// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "vocoder.hpp"
#include <algorithm>
#include <cmath>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define CHRONOBENT_NEON 1
#endif

namespace chronobent_dsp {
namespace {
double wrap(double x) noexcept { return x - 2 * pi * std::floor(x / (2 * pi) + 0.5); }
// Explicit products keep the compiler from calling the generic complex
// multiplication helper with its non-finite recovery in every bin.
inline Complex multiply(Complex a, Complex b) noexcept {
    return Complex(a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real());
}
// atan for |t| <= tan(pi/8) from its series through t^21: truncation below
// 5e-10 rad, evaluated in double so the measurement keeps the spectra's
// full single-precision phase information.
inline double arctangent_small(double t) noexcept {
    const double s = t * t;
    double p = -1.0 / 21;
    p = p * s + 1.0 / 19; p = p * s - 1.0 / 17; p = p * s + 1.0 / 15;
    p = p * s - 1.0 / 13; p = p * s + 1.0 / 11; p = p * s - 1.0 / 9;
    p = p * s + 1.0 / 7; p = p * s - 1.0 / 5; p = p * s + 1.0 / 3;
    return t - t * s * p;
}
// Four-quadrant arctangent with octant reduction, accurate to about 1e-9 rad.
inline double arctangent2(double y, double x) noexcept {
    const double ax = std::fabs(x), ay = std::fabs(y);
    if (ax == 0 && ay == 0) return 0;
    const bool steep = ay > ax;
    const double t = steep ? ax / ay : ay / ax;
    double angle = t > 0.41421356237309503 ? pi / 4 + arctangent_small((t - 1) / (t + 1)) : arctangent_small(t);
    if (steep) angle = pi / 2 - angle;
    if (x < 0) angle = pi - angle;
    return y < 0 ? -angle : angle;
}
// cos/sin of an angle in [-pi, pi] by quadrant reduction and series through
// x^12 and x^13 in double; the single-precision result is correctly rounded
// except in rare halfway cases.
inline Complex unit(double angle) noexcept {
    const int quadrant = static_cast<int>(angle * (2 / pi) + (angle >= 0 ? 0.5 : -0.5));
    const double r = angle - static_cast<double>(quadrant) * (pi / 2), s = r * r;
    double sp = -1.0 / 6227020800.0; sp = sp * s + 1.0 / 39916800; sp = sp * s - 1.0 / 362880;
    sp = sp * s + 1.0 / 5040; sp = sp * s - 1.0 / 120; sp = sp * s + 1.0 / 6;
    double cp = 1.0 / 479001600; cp = cp * s - 1.0 / 3628800; cp = cp * s + 1.0 / 40320;
    cp = cp * s - 1.0 / 720; cp = cp * s + 1.0 / 24; cp = cp * s - 0.5;
    const float sine = static_cast<float>(r - r * s * sp), cosine = static_cast<float>(1 + s * cp);
    switch ((quadrant + 4) & 3) {
    case 0: return Complex(cosine, sine);
    case 1: return Complex(-sine, cosine);
    case 2: return Complex(-cosine, -sine);
    default: return Complex(sine, -cosine);
    }
}
// Stereo frame energy with the earliest strict maximum. Four lanes accumulate
// in both paths so the anchor and total do not depend on the instruction set.
void stereo_energy(const float *input, std::size_t frames, double &total, float &maximum, std::size_t &anchor) noexcept {
    float best[4] = {0, 0, 0, 0}, sum[4] = {0, 0, 0, 0};
    std::size_t index[4] = {0, 0, 0, 0};
#ifdef CHRONOBENT_NEON
    float32x4_t vbest = vdupq_n_f32(0), vsum = vdupq_n_f32(0);
    uint32x4_t vindex = vdupq_n_u32(0);
    const uint32x4_t lanes = {0, 1, 2, 3};
    for (std::size_t i = 0; i < frames; i += 4) {
        const float32x4x2_t x = vld2q_f32(input + 2 * i);
        const float32x4_t energy = vaddq_f32(vmulq_f32(x.val[0], x.val[0]), vmulq_f32(x.val[1], x.val[1]));
        const uint32x4_t greater = vcgtq_f32(energy, vbest);
        vsum = vaddq_f32(vsum, energy);
        vbest = vbslq_f32(greater, energy, vbest);
        vindex = vbslq_u32(greater, vaddq_u32(vdupq_n_u32(static_cast<std::uint32_t>(i)), lanes), vindex);
    }
    std::uint32_t found[4];
    vst1q_f32(best, vbest); vst1q_f32(sum, vsum); vst1q_u32(found, vindex);
    for (std::size_t lane = 0; lane < 4; ++lane) index[lane] = found[lane];
#else
    for (std::size_t i = 0; i < frames; ++i) {
        const float left = input[2 * i], right = input[2 * i + 1];
        const float energy = left * left + right * right;
        sum[i & 3] += energy;
        if (energy > best[i & 3]) { best[i & 3] = energy; index[i & 3] = i; }
    }
#endif
    total = (double(sum[0]) + sum[1]) + (double(sum[2]) + sum[3]);
    maximum = 0; anchor = 0;
    for (std::size_t lane = 0; lane < 4; ++lane)
        if (best[lane] > maximum || (best[lane] == maximum && best[lane] > 0 && index[lane] < anchor)) {
            maximum = best[lane]; anchor = index[lane];
        }
}
// Per-bin total power, its square root and the stronger channel of a pair.
void stereo_magnitude(const Complex *a, const Complex *b, std::size_t bins, float *magnitude, std::uint32_t *reference) noexcept {
    std::size_t k = 0;
#ifdef CHRONOBENT_NEON
    for (; k + 4 <= bins; k += 4) {
        const float32x4x2_t x = vld2q_f32(reinterpret_cast<const float *>(a + k));
        const float32x4x2_t y = vld2q_f32(reinterpret_cast<const float *>(b + k));
        const float32x4_t first = vaddq_f32(vmulq_f32(x.val[0], x.val[0]), vmulq_f32(x.val[1], x.val[1]));
        const float32x4_t second = vaddq_f32(vmulq_f32(y.val[0], y.val[0]), vmulq_f32(y.val[1], y.val[1]));
        vst1q_u32(reference + k, vshrq_n_u32(vcgtq_f32(second, first), 31));
        vst1q_f32(magnitude + k, vsqrtq_f32(vaddq_f32(first, second)));
    }
#endif
    for (; k < bins; ++k) {
        const float first = a[k].real() * a[k].real() + a[k].imag() * a[k].imag();
        const float second = b[k].real() * b[k].real() + b[k].imag() * b[k].imag();
        reference[k] = second > first ? 1 : 0;
        magnitude[k] = std::sqrt(first + second);
    }
}
// overlap[2i] += real(z[i]) * w[i]; overlap[2i+1] += imag(z[i]) * w[i].
void stereo_overlap_add(float *overlap, const Complex *z, const float *w, std::size_t frames) noexcept {
    std::size_t i = 0;
#ifdef CHRONOBENT_NEON
    for (; i + 4 <= frames; i += 4) {
        float32x4x2_t o = vld2q_f32(overlap + 2 * i);
        const float32x4x2_t p = vld2q_f32(reinterpret_cast<const float *>(z + i));
        const float32x4_t window = vld1q_f32(w + i);
        o.val[0] = vaddq_f32(o.val[0], vmulq_f32(p.val[0], window));
        o.val[1] = vaddq_f32(o.val[1], vmulq_f32(p.val[1], window));
        vst2q_f32(overlap + 2 * i, o);
    }
#endif
    for (; i < frames; ++i) {
        overlap[2 * i] += z[i].real() * w[i];
        overlap[2 * i + 1] += z[i].imag() * w[i];
    }
}
}

Vocoder::Vocoder(std::size_t size, std::size_t channels, double sample_rate, bool transients, bool formants)
    : size_(size), channels_(channels), bins_(size / 2 + 1), hop_(size / 8),
      flux_min_(std::max<std::size_t>(1, static_cast<std::size_t>(180 * static_cast<double>(size) / sample_rate))),
      transients_(transients), formant_scale_(formants ? 1 : 0), sample_rate_(sample_rate), fft_(size), envelope_(size, sample_rate), window_(size), attack_analysis_(size), attack_synthesis_(size), overlap_(size * channels), weight_(size),
      magnitude_(bins_), previous_magnitude_(bins_),
      spectrum_(bins_ * channels), previous_spectrum_(bins_ * channels), packed_(size), factor_(bins_),
      rotation_(bins_), next_rotation_(bins_),
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
    std::fill(previous_magnitude_.begin(), previous_magnitude_.end(), 0.0f);
}

std::int64_t Vocoder::analysis_start() const noexcept {
    // Centered frames avoid a ratio-dependent half-window timeline offset.
    return static_cast<std::int64_t>(std::llround(
        static_cast<double>(start_ + static_cast<std::int64_t>(size_ / 2)) * rate_)) -
        static_cast<std::int64_t>(size_ / 2);
}

// Two real channels share one complex transform: z = a + i*b. Because the
// transform of a real sequence is exactly Hermitian, A[k] = (Z[k] + conj(Z[N-k]))/2
// and B[k] = -i*(Z[k] - conj(Z[N-k]))/2 separate the channels without crosstalk
// beyond the transform's own rounding. An odd trailing channel uses the
// transform alone. Windowed samples are stored in transform order directly.
void Vocoder::analyze(const float *input, const float *window) noexcept {
    const std::size_t mask = size_ - 1;
    const std::uint32_t *order = fft_.permutation();
    std::size_t c = 0;
    for (; c + 1 < channels_; c += 2) {
        for (std::size_t i = 0; i < size_; ++i)
            packed_[order[i]] = Complex(input[i * channels_ + c] * window[i], input[i * channels_ + c + 1] * window[i]);
        fft_.transform_permuted(packed_.data(), false);
        Complex *a = spectrum_.data() + c * bins_, *b = a + bins_;
        for (std::size_t k = 0; k < bins_; ++k) {
            const Complex z = packed_[k], m = std::conj(packed_[(size_ - k) & mask]);
            a[k] = Complex((z.real() + m.real()) * 0.5f, (z.imag() + m.imag()) * 0.5f);
            b[k] = Complex((z.imag() - m.imag()) * 0.5f, (m.real() - z.real()) * 0.5f);
        }
    }
    if (c < channels_) {
        for (std::size_t i = 0; i < size_; ++i) packed_[order[i]] = Complex(input[i * channels_ + c] * window[i], 0);
        fft_.transform_permuted(packed_.data(), false);
        std::copy_n(packed_.data(), bins_, spectrum_.data() + c * bins_);
    }
}

// Applies the per-bin synthesis factor while packing, so the analysis spectra
// stay untouched and become the next frame's phase reference without a copy.
// The unpaired real-FFT endpoints must remain real. A fractional attack shift
// scales Nyquist by cos(pi*shift); leaving it unchanged would spread a small
// alternating residue around the attack.
void Vocoder::synthesize(const float *window) noexcept {
    const std::size_t last = bins_ - 1;
    const std::uint32_t *order = fft_.permutation();
    std::size_t c = 0;
    for (; c + 1 < channels_; c += 2) {
        const Complex *a = spectrum_.data() + c * bins_, *b = a + bins_;
        // Z = A + i*B over the full circle; both spectra are Hermitian with
        // real endpoints, so the inverse returns channel a as the real part
        // and channel b as the imaginary part.
        packed_[order[0]] = Complex(multiply(a[0], factor_[0]).real(), multiply(b[0], factor_[0]).real());
        packed_[order[last]] = Complex(multiply(a[last], factor_[last]).real(), multiply(b[last], factor_[last]).real());
        for (std::size_t k = 1; k < last; ++k) {
            const Complex fa = multiply(a[k], factor_[k]), fb = multiply(b[k], factor_[k]);
            packed_[order[k]] = Complex(fa.real() - fb.imag(), fa.imag() + fb.real());
            packed_[order[size_ - k]] = Complex(fa.real() + fb.imag(), fb.real() - fa.imag());
        }
        fft_.transform_permuted(packed_.data(), true);
        if (channels_ == 2) stereo_overlap_add(overlap_.data(), packed_.data(), window, size_);
        else {
            float *o = overlap_.data() + c;
            for (std::size_t i = 0; i < size_; ++i) {
                o[i * channels_] += packed_[i].real() * window[i];
                o[i * channels_ + 1] += packed_[i].imag() * window[i];
            }
        }
    }
    if (c < channels_) {
        const Complex *a = spectrum_.data() + c * bins_;
        packed_[order[0]] = Complex(multiply(a[0], factor_[0]).real(), 0);
        packed_[order[last]] = Complex(multiply(a[last], factor_[last]).real(), 0);
        for (std::size_t k = 1; k < last; ++k) {
            const Complex fa = multiply(a[k], factor_[k]);
            packed_[order[k]] = fa;
            packed_[order[size_ - k]] = std::conj(fa);
        }
        fft_.transform_permuted(packed_.data(), true);
        float *o = overlap_.data() + c;
        for (std::size_t i = 0; i < size_; ++i) o[i * channels_] += packed_[i].real() * window[i];
    }
}

void Vocoder::process(const float *input, float *output) noexcept {
    const auto analysis = analysis_start();
    const double advance = static_cast<double>(analysis - previous_analysis_);
    // Sparse attacks have an observable time anchor. Map that anchor explicitly
    // instead of resetting every overlapping frame at a different output time.
    // Conservative crest admission leaves sustained and dense material on the
    // tonal path. This is not a general percussive/source decomposition.
    double total = 0;
    float maximum = 0;
    std::size_t anchor = 0;
    if (transients_ && channels_ == 2) stereo_energy(input, size_, total, maximum, anchor);
    else if (transients_) for (std::size_t i = 0; i < size_; ++i) {
        float energy = 0;
        for (std::size_t c = 0; c < channels_; ++c) {
            const float value = input[i * channels_ + c];
            energy += value * value;
        }
        total += energy;
        if (energy > maximum) { maximum = energy; anchor = i; }
    }
    const bool anchored = maximum > 1e-8f && double(maximum) * static_cast<double>(size_) > 32 * total;
    const auto &analysis_window = anchored ? attack_analysis_ : window_;
    const auto &synthesis_window = attack_synthesis_;
    const double anchor_shift = (static_cast<double>(analysis) + static_cast<double>(anchor)) / rate_ -
        static_cast<double>(start_) - static_cast<double>(anchor);
    analyze(input, analysis_window.data());
    // Per-bin magnitude uses total channel power; the strongest channel of
    // each bin provides its phase reference. Single precision suffices for
    // peak selection, flux and the log envelope.
    if (channels_ == 2) stereo_magnitude(spectrum_.data(), spectrum_.data() + bins_, bins_, magnitude_.data(), reference_.data());
    else for (std::size_t k = 0; k < bins_; ++k) {
        float energy = 0, largest = -1;
        for (std::size_t c = 0; c < channels_; ++c) {
            const float power = std::norm(spectrum_[c * bins_ + k]);
            energy += power;
            if (power > largest) { largest = power; reference_[k] = static_cast<std::uint32_t>(c); }
        }
        magnitude_[k] = std::sqrt(energy);
    }
    double flux = 0, high_energy = 0;
    for (std::size_t k = flux_min_; k < bins_; ++k) {
        flux += std::max(0.0f, magnitude_[k] - previous_magnitude_[k]);
        high_energy += magnitude_[k];
    }
    const bool onset = transients_ && primed_ && hold_ == 0 && high_energy > 1e-5 &&
        flux > 0.25 * high_energy && flux > 2.0 * flux_average_;
    flux_average_ = 0.9 * flux_average_ + 0.1 * flux;
    if (onset) hold_ = 3;
    else if (hold_ != 0) --hold_;

    // Branch-free peak picking: dense material makes the outcome unpredictable.
    std::size_t peak_count = 0;
    for (std::size_t k = 1; k + 1 < bins_; ++k) {
        const float m = magnitude_[k];
        const int peak = int(m > magnitude_[k - 1]) & int(m >= magnitude_[k + 1]) & int(m > 1e-8f);
        peaks_[peak_count] = k;
        peak_count += static_cast<std::size_t>(peak);
    }
    // Locked regions consume only their peak's phase estimate. Avoid computing
    // discarded estimates for every non-peak bin; anchored frames need none.
    // Retain every channel's original spectrum because peaks and reference
    // channels can change. Evaluate phase only where propagation consumes it.
    // The measured phase advance is the argument of now*conj(before): one
    // single-precision arctangent per propagating bin instead of two phases.
    const auto propagate = [&](std::size_t k) {
        const std::size_t index = reference_[k] * bins_ + k;
        const bool reset_band = !mixed_ || double(k) * sample_rate_ / double(size_) >= 500;
        if (!primed_ || (onset && reset_band && magnitude_[k] > 1.5f * previous_magnitude_[k])) {
            next_rotation_[k] = 0;
        } else {
            const Complex now = spectrum_[index];
            const Complex before = previous_spectrum_[index];
            // Single-precision products are exact in double, so the small
            // cross term near zero phase advance keeps its accuracy.
            const double real = double(now.real()) * before.real() + double(now.imag()) * before.imag();
            const double imag = double(now.imag()) * before.real() - double(now.real()) * before.imag();
            const double measured = arctangent2(imag, real);
            const double omega = 2 * pi * static_cast<double>(k) / static_cast<double>(size_);
            const double delta = omega * advance + wrap(measured - omega * advance);
            next_rotation_[k] = wrap(rotation_[k] + delta * (static_cast<double>(hop_) / advance - 1));
        }
    };
    if (!anchored) {
        if (peak_count) for (std::size_t i = 0; i < peak_count; ++i) propagate(peaks_[i]);
        else for (std::size_t k = 1; k + 1 < bins_; ++k) propagate(k);
    }

    // Identity phase locking: a peak's phase rotation is shared by its region,
    // which extends to the midpoint between neighboring peaks.
    // Source channel magnitudes and relative phases remain independent.
    if (anchored) {
        for (std::size_t k = 1; k + 1 < bins_; ++k)
            rotation_[k] = wrap(-2 * pi * static_cast<double>(k) * anchor_shift / static_cast<double>(size_));
    } else if (peak_count) {
        std::size_t k = 1;
        for (std::size_t region = 0; region < peak_count; ++region) {
            const std::size_t end = region + 1 < peak_count ? (peaks_[region] + peaks_[region + 1]) / 2 + 1 : bins_ - 1;
            const double value = next_rotation_[peaks_[region]];
            for (; k < end; ++k) rotation_[k] = value;
        }
    } else std::copy(next_rotation_.begin() + 1, next_rotation_.end() - 1, rotation_.begin() + 1);
    rotation_[0] = 0;
    rotation_[bins_ - 1] = anchored ? wrap(-pi * anchor_shift) : 0;
    const double envelope_ratio = formant_scale_ == 0 ? 1 : pitch_ / formant_scale_;
    if (envelope_ratio != 1) envelope_.analyze(magnitude_.data());
    Complex turn;
    for (std::size_t k = 0; k < bins_; ++k) {
        // A locked region shares one rotation. Reuse its complex multiplier
        // instead of evaluating identical sine/cosine pairs for every bin.
        if (k == 0 || rotation_[k] != rotation_[k-1] ||
            std::signbit(rotation_[k]) != std::signbit(rotation_[k-1]))
            turn = unit(rotation_[k]);
        factor_[k] = envelope_ratio != 1 ? turn * envelope_.correction(k, envelope_ratio) : turn;
    }
    synthesize(synthesis_window.data());
    const float *product = anchored ? synthesis_window.data() : window_.data();
    for (std::size_t i = 0; i < size_; ++i) weight_[i] += synthesis_window[i] * product[i];
    for (std::size_t i = 0; i < hop_; ++i) {
        const float scale = weight_[i] > 1e-8f ? 1.0f / weight_[i] : 0.0f;
        for (std::size_t c = 0; c < channels_; ++c) output[i * channels_ + c] = overlap_[i * channels_ + c] * scale;
    }
    std::move(overlap_.begin() + static_cast<std::ptrdiff_t>(hop_ * channels_), overlap_.end(), overlap_.begin());
    std::fill(overlap_.end() - static_cast<std::ptrdiff_t>(hop_ * channels_), overlap_.end(), 0.0f);
    std::move(weight_.begin() + static_cast<std::ptrdiff_t>(hop_), weight_.end(), weight_.begin());
    std::fill(weight_.end() - static_cast<std::ptrdiff_t>(hop_), weight_.end(), 0.0f);
    // The untouched analysis spectra and magnitudes become the next reference.
    std::swap(spectrum_, previous_spectrum_);
    std::swap(magnitude_, previous_magnitude_);
    previous_analysis_ = analysis;
    primed_ = true;
    start_ += static_cast<std::int64_t>(hop_);
}
}
