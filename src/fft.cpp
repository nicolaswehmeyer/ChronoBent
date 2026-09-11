// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "fft.hpp"
#include <algorithm>
#include <cmath>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define CHRONOBENT_NEON 1
#endif

namespace chronobent_dsp {
namespace {
// Four butterflies per step. Products stay separate statements so the scalar
// and NEON paths accumulate in the same order; the inverse conjugates the
// twiddle by swapping the signs of the two cross terms instead of branching.
template <bool Inverse>
void butterflies(Complex *a, Complex *b, const Complex *w, std::size_t count) noexcept {
#ifdef CHRONOBENT_NEON
    for (std::size_t k = 0; k < count; k += 4) {
        const float32x4x2_t va = vld2q_f32(reinterpret_cast<const float *>(a + k));
        const float32x4x2_t vb = vld2q_f32(reinterpret_cast<const float *>(b + k));
        const float32x4x2_t vw = vld2q_f32(reinterpret_cast<const float *>(w + k));
        const float32x4_t rr = vmulq_f32(vb.val[0], vw.val[0]);
        const float32x4_t ii = vmulq_f32(vb.val[1], vw.val[1]);
        const float32x4_t ri = vmulq_f32(vb.val[0], vw.val[1]);
        const float32x4_t ir = vmulq_f32(vb.val[1], vw.val[0]);
        const float32x4_t pr = Inverse ? vaddq_f32(rr, ii) : vsubq_f32(rr, ii);
        const float32x4_t pi = Inverse ? vsubq_f32(ir, ri) : vaddq_f32(ri, ir);
        float32x4x2_t sum, difference;
        sum.val[0] = vaddq_f32(va.val[0], pr); sum.val[1] = vaddq_f32(va.val[1], pi);
        difference.val[0] = vsubq_f32(va.val[0], pr); difference.val[1] = vsubq_f32(va.val[1], pi);
        vst2q_f32(reinterpret_cast<float *>(a + k), sum);
        vst2q_f32(reinterpret_cast<float *>(b + k), difference);
    }
#else
    for (std::size_t k = 0; k < count; ++k) {
        const Complex x = a[k], value = b[k], root = w[k];
        const float rr = value.real() * root.real();
        const float ii = value.imag() * root.imag();
        const float ri = value.real() * root.imag();
        const float ir = value.imag() * root.real();
        const Complex product(Inverse ? rr + ii : rr - ii, Inverse ? ir - ri : ri + ir);
        a[k] = x + product;
        b[k] = x - product;
    }
#endif
}
}

Fft::Fft(std::size_t size) : size_(size), reverse_(size), twiddles_(size > 1 ? size - 1 : 1) {
    for (std::size_t i = 1; i < size; ++i)
        reverse_[i] = static_cast<std::uint32_t>((reverse_[i / 2] >> 1) | ((i & 1) * (size / 2)));
    // Stage with `half` butterflies starts at offset half-1. The quarter wave is
    // evaluated once and mirrored so w[half-k] == -conj(w[k]) bit for bit and
    // w[half/2] is exactly -i. That symmetry keeps real-input spectra exactly
    // Hermitian, which the paired real transforms in the vocoder rely on.
    for (std::size_t half = 1; half < size; half *= 2) {
        Complex *w = twiddles_.data() + (half - 1);
        w[0] = Complex(1, 0);
        for (std::size_t k = 1; k <= half / 2; ++k) {
            const double angle = pi * static_cast<double>(k) / static_cast<double>(half);
            const float c = 2 * k == half ? 0.0f : static_cast<float>(std::cos(angle));
            const float s = 2 * k == half ? 1.0f : static_cast<float>(std::sin(angle));
            w[k] = Complex(c, -s);
            if (half - k != k) w[half - k] = Complex(-c, -s);
        }
    }
}

template <bool Inverse, bool Permute>
void Fft::stages(Complex *v) const noexcept {
    if (Permute) for (std::size_t i = 0; i < size_; ++i)
        if (i < reverse_[i]) std::swap(v[i], v[reverse_[i]]);
    if (size_ >= 2) for (std::size_t base = 0; base < size_; base += 2) {
        const Complex a = v[base], b = v[base + 1];
        v[base] = a + b; v[base + 1] = a - b;
    }
    if (size_ >= 4) for (std::size_t base = 0; base < size_; base += 4) {
        const Complex a0 = v[base], b0 = v[base + 2], a1 = v[base + 1], b1 = v[base + 3];
        // Twiddle -i (or +i inverted) is an exact swap with one negation.
        const Complex t = Inverse ? Complex(-b1.imag(), b1.real()) : Complex(b1.imag(), -b1.real());
        v[base] = a0 + b0; v[base + 2] = a0 - b0;
        v[base + 1] = a1 + t; v[base + 3] = a1 - t;
    }
    for (std::size_t half = 4; half < size_; half *= 2) {
        const Complex *w = twiddles_.data() + (half - 1);
        for (std::size_t base = 0; base < size_; base += 2 * half)
            butterflies<Inverse>(v + base, v + base + half, w, half);
    }
    if (Inverse) {
        const float scale = 1.0f / static_cast<float>(size_);
        for (std::size_t i = 0; i < size_; ++i) v[i] *= scale;
    }
}

void Fft::transform(Complex *v, bool inverse) const noexcept {
    if (inverse) stages<true, true>(v); else stages<false, true>(v);
}

void Fft::transform_permuted(Complex *v, bool inverse) const noexcept {
    if (inverse) stages<true, false>(v); else stages<false, false>(v);
}
}
