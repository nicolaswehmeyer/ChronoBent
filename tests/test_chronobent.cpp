// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/chronobent.h"
#include "../src/fft.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {
bool forbid_allocation = false;
void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}
constexpr double pi = 3.141592653589793238462643383279502884;
using Engine = std::unique_ptr<chronobent, decltype(&chronobent_destroy)>;
struct Source {
    std::size_t channels;
    std::vector<float> audio;
    unsigned calls = 0, fail_at = 0, invalid_at = 0;
    Source(std::size_t frames, std::size_t count) : channels(count), audio(frames * count) {}
    static int read(void *user, std::uint64_t first, std::size_t count, float *out) {
        auto &s = *static_cast<Source *>(user);
        require(count != 0 && first <= s.audio.size() / s.channels &&
            count <= s.audio.size() / s.channels - first, "reader range exceeded declared source");
        ++s.calls;
        if (s.calls == s.fail_at) return 0;
        std::copy_n(s.audio.data() + first * s.channels, count * s.channels, out);
        if (s.calls == s.invalid_at) out[count / 2 * s.channels] = std::numeric_limits<float>::quiet_NaN();
        return 1;
    }
};
Engine engine(double sample_rate = 48000, std::uint32_t channels = 2, bool transients = true, bool formants = false) {
    chronobent_config config{sample_rate, channels, 0, transients ? 1u : 0u, formants ? 1u : 0u};
    chronobent *p = nullptr;
    require(chronobent_create(&config, &p) == CHRONOBENT_OK && p, "create");
    return Engine(p, chronobent_destroy);
}
std::vector<float> render(Source &source, double tempo, double pitch, std::size_t chunk = 257,
                          double sample_rate = 48000, bool transients = true, bool formants = false) {
    auto p = engine(sample_rate, static_cast<std::uint32_t>(source.channels), transients, formants);
    require(chronobent_reset(p.get(), source.audio.size() / source.channels, tempo, pitch) == CHRONOBENT_OK, "reset");
    const auto count = static_cast<std::size_t>(chronobent_output_frames(p.get()));
    std::vector<float> output(count * source.channels + 16, 19.0f);
    std::size_t written = 0;
    do {
        std::size_t got = 999;
        const auto ask = std::min(chunk, count - written + 1);
        const auto status = chronobent_render(p.get(), Source::read, &source,
            output.data() + written * source.channels, ask, &got);
        require(status == CHRONOBENT_OK || status == CHRONOBENT_END, "render status");
        written += got;
        if (status == CHRONOBENT_END) break;
        require(got != 0, "render made no progress");
    } while (true);
    require(written == count, "duration");
    for (std::size_t i = count * source.channels; i < output.size(); ++i)
        require(output[i] == 19, "wrote past produced output");
    output.resize(count * source.channels);
    for (float x : output) require(std::isfinite(x), "nonfinite output");
    return output;
}
void sine(Source &s, double sample_rate, double hz, bool opposite = false) {
    for (std::size_t i = 0; i < s.audio.size() / s.channels; ++i)
        for (std::size_t c = 0; c < s.channels; ++c)
            s.audio[i * s.channels + c] = static_cast<float>(0.5 * std::sin(2 * pi * hz * static_cast<double>(i) / sample_rate)) *
                (opposite && (c & 1) ? -1.0f : 1.0f);
}
double rms(const std::vector<float> &v, std::size_t channels, std::size_t channel) {
    const auto frames = v.size() / channels, first = frames / 4, last = frames * 3 / 4;
    double energy = 0;
    for (std::size_t i = first; i < last; ++i) energy += double(v[i * channels + channel]) * v[i * channels + channel];
    return std::sqrt(energy / static_cast<double>(last - first));
}
// Independent time-domain frequency oracle: interpolate positive crossings.
double frequency(const std::vector<float> &v, std::size_t channels, std::size_t channel, double sr) {
    double first = -1, last = -1;
    unsigned count = 0;
    const auto frames = v.size() / channels;
    for (std::size_t i = frames / 4 + 1; i < frames * 3 / 4; ++i) {
        const double a = v[(i - 1) * channels + channel], b = v[i * channels + channel];
        if (a <= 0 && b > 0) {
            const double crossing = static_cast<double>(i - 1) - a / (b - a);
            if (count++ == 0) first = crossing;
            last = crossing;
        }
    }
    require(count > 3 && last > first, "too few tone crossings");
    return sr * static_cast<double>(count - 1) / (last - first);
}

void fft_oracle() {
    for (std::size_t n : {8u, 32u, 512u}) {
        chronobent_dsp::Fft fft(n);
        std::vector<chronobent_dsp::Complex> data(n), original(n);
        for (std::size_t i = 0; i < n; ++i)
            original[i] = data[i] = {static_cast<float>(std::sin(double(i) * 0.7)), static_cast<float>(std::cos(double(i) * 0.3))};
        fft.transform(data.data(), false);
        for (std::size_t k = 0; k < n; ++k) {
            std::complex<double> expected(0, 0);
            for (std::size_t j = 0; j < n; ++j) {
                const double angle = -2 * pi * static_cast<double>(j * k) / static_cast<double>(n);
                expected += std::complex<double>(original[j]) * std::complex<double>(std::cos(angle), std::sin(angle));
            }
            require(std::abs(std::complex<double>(data[k]) - expected) < 1e-4, "FFT differs from direct DFT");
        }
        fft.transform(data.data(), true);
        for (std::size_t i = 0; i < n; ++i) require(std::abs(data[i] - original[i]) < 1e-5f, "FFT inverse");
    }
}
void identity_and_blocks() {
    Source s(48001, 2);
    std::uint32_t noise = 17;
    for (float &x : s.audio) { noise = noise * 1664525u + 1013904223u; x = static_cast<float>(double(noise) / 4294967296.0 - 0.5); }
    require(render(s, 1, 1, 113) == s.audio, "identity is not sample-exact");
    auto a = render(s, 1.071, 0.63, 1);
    require(a == render(s, 1.071, 0.63, 257), "block size changed samples");
    require(a == render(s, 1.071, 0.63, 8192), "large block changed samples");
    require(a.size() / 2 == 44819, "fractional duration rounded incorrectly");
    Source empty(0, 2);
    require(render(empty, 1, 1).empty(), "empty source");
    for (std::size_t length : {1u, 7u, 63u, 256u}) {
        Source short_source(length, 1);
        short_source.audio[0] = 0.5f;
        for (double tempo : {0.25, 1.0, 4.0})
            for (double pitch : {0.5, 1.0, 2.0})
                require(render(short_source, tempo, pitch).size() == static_cast<std::size_t>(std::ceil(double(length) / tempo)), "short source duration");
    }
}
void tone_accuracy() {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (const auto &test : {std::array<double, 3>{55.0, 0.923, 1.0},
                                {440.0, 1.079, 0.5}, {110.0, 0.25, 2.0}, {997.0, 2.0, 1.5}}) {
            Source s(static_cast<std::size_t>(sr * 2), 2);
            sine(s, sr, test[0], true);
            const auto output = render(s, test[1], test[2], 509, sr);
            const double hz = frequency(output, 2, 0, sr), expected = test[0] * test[2];
            const double cents = 1200 * std::log2(hz / expected);
            std::printf("tone sr=%.0f source=%.0f tempo=%.3f pitch=%.3f cents=%+.4f rms=%.5f\n", sr, test[0], test[1], test[2], cents, rms(output, 2, 0));
            require(std::abs(cents) < 3.0, "pitch error exceeded three cents");
            require(rms(output, 2, 0) > 0.25 && rms(output, 2, 0) < 0.43, "tone gain changed excessively");
            for (std::size_t i = 0; i < output.size(); i += 2)
                require(std::abs(output[i] + output[i + 1]) < 2e-6, "opposite-phase stereo lost coherence");
        }
    }
    // One channel silent, and unrelated pitches in two further channels.
    Source multi(144000, 3);
    for (std::size_t i = 0; i < 144000; ++i) {
        multi.audio[3*i+1] = static_cast<float>(0.5 * std::sin(2*pi*137*double(i)/48000));
        multi.audio[3*i+2] = static_cast<float>(0.4 * std::sin(2*pi*883*double(i)/48000));
    }
    auto output = render(multi, 1.25, 0.75);
    require(rms(output, 3, 0) == 0, "silent channel acquired crosstalk");
    require(std::abs(frequency(output, 3, 1, 48000) - 137 * 0.75) < 0.2, "independent channel pitch one");
    require(std::abs(frequency(output, 3, 2, 48000) - 883 * 0.75) < 0.5, "independent channel pitch two");
}
void resampler_quality() {
    Source s(96000, 1);
    sine(s, 48000, 19000);
    auto alias = render(s, 2, 2);
    std::printf("resampler alias rejection=%.2f dB\n", 20 * std::log10(rms(alias, 1, 0) / std::sqrt(0.125)));
    require(rms(alias, 1, 0) < 0.0001, "out-of-band tone aliases");
    sine(s, 48000, 6000);
    auto pass = render(s, 2, 2);
    require(std::abs(rms(pass, 1, 0) - std::sqrt(0.125)) < 0.001, "passband attenuation");
    std::fill(s.audio.begin(), s.audio.end(), 0.3f);
    auto dc = render(s, 0.73, 0.73);
    require(std::abs(rms(dc, 1, 0) - 0.3) < 1e-6, "resampler DC normalization");
}
void attack_timing() {
    // Multiple off-grid, opposite-phase attacks check timeline compensation,
    // fractional analysis centers, pitch resampling and channel linkage.
    Source impulses(48000, 2);
    for (std::size_t at : {9999u, 24001u, 36007u}) {
        impulses.audio[at*2] = 0.5f; impulses.audio[at*2+1] = -0.5f;
    }
    for (double tempo : {0.25, 0.73, 1.25, 2.0, 4.0}) for (double pitch : {0.5, 1.0, 1.5, 2.0}) {
        const auto output = render(impulses, tempo, pitch);
        for (std::size_t source_at : {9999u, 24001u, 36007u}) {
            const auto expected = std::size_t(std::llround(double(source_at)/tempo));
            std::size_t peak = expected;
            double inside = 0, total = 0;
            for (std::size_t i = expected-512; i < expected+512; ++i) {
                const double energy = double(output[i*2])*output[i*2];
                total += energy;
                if (i+96 >= expected && i < expected+96) inside += energy;
                if (std::abs(output[i*2]) > std::abs(output[peak*2])) peak = i;
                require(std::abs(output[i*2]+output[i*2+1]) < 1e-6, "attack stereo coherence");
            }
            require(std::abs(double(peak)-double(expected)) <= 1, "sparse attack shifted by more than one sample");
            require(total > 0.001 && inside/total > 0.995, "sparse attack energy smeared beyond two ms");
            if (tempo == 0.25 && pitch == 1)
                require(inside/total > 1 - 1e-10, "integer attack shift left an alternating Nyquist residue");
        }
    }
    // Dense tonal/percussive mixtures are a separate stress case, not evidence
    // that isolated-click timing generalizes to arbitrary musical attacks.
    Source mixture(48000, 2);
    for (std::size_t i=0; i<48000; ++i) {
        const double t=double(i)/48000;
        const double beat=std::fmod(t,0.125);
        const double drum=0.35*std::exp(-beat*100)*std::sin(2*pi*(100*t+13*std::sin(17*t)));
        const float value=float(drum+0.2*std::sin(2*pi*220*t)+0.15*std::sin(2*pi*331*t));
        mixture.audio[i*2]=value; mixture.audio[i*2+1]=-value;
    }
    for (double pitch : {0.5, 0.891, 1.122, 2.0}) {
        const auto output=render(mixture,0.87,pitch);
        require(rms(output,2,0)>0.02 && rms(output,2,0)<0.6,"mixed audio level escaped bounds");
        for(std::size_t i=0;i<output.size();i+=2)
            require(std::isfinite(output[i]) && std::abs(output[i]+output[i+1])<1e-6,"mixed stereo failure");
    }
}
double vowel_envelope(double hz) {
    const auto bell = [hz](double center, double width) { const double x = (hz-center)/width; return std::exp(-0.5*x*x); };
    return 0.01 + bell(700, 140) + 0.7*bell(1250, 180) + 0.4*bell(2600, 250);
}
double partial_amplitude(const std::vector<float> &audio, double hz) {
    const std::size_t first = audio.size()/4, last = audio.size()*3/4;
    double real = 0, imag = 0;
    for (std::size_t i = first; i < last; ++i) {
        const double angle = 2*pi*hz*double(i)/48000;
        real += audio[i]*std::cos(angle); imag += audio[i]*std::sin(angle);
    }
    return 2*std::hypot(real, imag)/double(last-first);
}
void envelope_quality() {
    Source vowel(96000, 1);
    for (std::size_t i = 0; i < vowel.audio.size(); ++i) {
        double value = 0;
        for (int harmonic = 1; harmonic <= 48; ++harmonic)
            value += vowel_envelope(harmonic*100)*std::sin(2*pi*harmonic*100*double(i)/48000 + harmonic*0.7)/16;
        vowel.audio[i] = static_cast<float>(value);
    }
    const double pitch = 1.5;
    const auto shifted = render(vowel, 1.08, pitch, 257, 48000, false, false);
    const auto preserved = render(vowel, 1.08, pitch, 257, 48000, false, true);
    double old_error = 0, new_error = 0;
    for (int h = 3; h <= 25; ++h) {
        const double expected = vowel_envelope(h*100*pitch)/16;
        const double a = std::log(std::max(1e-5, partial_amplitude(shifted, h*100*pitch))/expected);
        const double b = std::log(std::max(1e-5, partial_amplitude(preserved, h*100*pitch))/expected);
        old_error += a*a; new_error += b*b;
    }
    std::printf("synthetic vowel log-envelope error: shifted=%.4f preserved=%.4f\n", std::sqrt(old_error/23), std::sqrt(new_error/23));
    require(new_error < old_error*0.6, "envelope preservation failed to improve synthetic vowel");
    Source silence(12000, 8);
    const auto quiet = render(silence, 0.25, 2, 509, 48000, true, true);
    for (float value : quiet) require(value == 0, "formant correction amplified silence");
}
void failure_and_reset() {
    Source s(32000, 2);
    sine(s, 48000, 440);
    const auto expected = render(s, 0.87, 1.73);
    for (bool invalid : {false, true}) {
        auto p = engine();
        require(chronobent_reset(p.get(), 32000, 0.87, 1.73) == CHRONOBENT_OK, "failure reset");
        s.calls = 0; s.fail_at = invalid ? 0 : 14; s.invalid_at = invalid ? 14 : 0;
        std::vector<float> output(expected.size() + 1024, 91.0f);
        std::size_t written = 0;
        bool failed = false;
        for (;;) {
            std::size_t got = 0;
            const auto status = chronobent_render(p.get(), Source::read, &s, output.data() + 2 * written, 257, &got);
            require(got <= 257, "invalid produced count");
            written += got;
            require(output[2 * written] == 91, "failure touched unproduced output");
            if (status == CHRONOBENT_SOURCE_UNAVAILABLE || status == CHRONOBENT_INVALID_AUDIO) {
                require(!failed && status == (invalid ? CHRONOBENT_INVALID_AUDIO : CHRONOBENT_SOURCE_UNAVAILABLE), "wrong source failure");
                failed = true;
            } else require(status == CHRONOBENT_OK || status == CHRONOBENT_END, "unexpected status after retry");
            if (status == CHRONOBENT_END) break;
        }
        require(failed && written * 2 == expected.size(), "source failure not exercised");
        require(std::equal(expected.begin(), expected.end(), output.begin()), "retry changed output timeline");
        std::fill(s.audio.begin(), s.audio.end(), 0);
        require(chronobent_reset(p.get(), 32000, 0.5, 2) == CHRONOBENT_OK, "reset to silence");
        std::size_t got = 0;
        require(chronobent_render(p.get(), Source::read, &s, output.data(), 4096, &got) == CHRONOBENT_OK && got == 4096, "silent source render");
        for (std::size_t i = 0; i < 2 * got; ++i) require(output[i] == 0, "reset retained previous audio");
        sine(s, 48000, 440);
    }
}
void invalid_and_allocation() {
    chronobent *raw = nullptr;
    chronobent_config config{48000, 2, 0, 1, 0};
    for (double sr : {0.0, 7999.0, 192001.0, std::numeric_limits<double>::quiet_NaN()}) {
        config.sample_rate = sr;
        require(chronobent_create(&config, &raw) == CHRONOBENT_INVALID_ARGUMENT && !raw, "invalid rate accepted");
    }
    config.sample_rate = 48000; config.window_frames = 513;
    require(chronobent_create(&config, &raw) == CHRONOBENT_INVALID_ARGUMENT, "invalid window accepted");
    auto p = engine();
    std::size_t got = 99;
    require(chronobent_render(p.get(), nullptr, nullptr, nullptr, 0, &got) == CHRONOBENT_NOT_RESET && got == 0, "render before reset");
    for (double ratio : {-1.0, 0.0, 0.249, 4.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        require(chronobent_reset(p.get(), 1234, ratio, 1) == CHRONOBENT_INVALID_ARGUMENT, "invalid tempo accepted");
    require(chronobent_reset(p.get(), (UINT64_C(1)<<48)+1, 1, 1) == CHRONOBENT_INVALID_ARGUMENT, "unrepresentable duration accepted");
    Source s(24000, 2);
    sine(s, 48000, 440);
    std::vector<float> output(8192);
    forbid_allocation = true;
    require(chronobent_reset(p.get(), 24000, 0.25, 2) == CHRONOBENT_OK, "allocation reset");
    for (int i = 0; i < 20; ++i)
        require(chronobent_render(p.get(), Source::read, &s, output.data(), 4096, &got) == CHRONOBENT_OK && got == 4096, "allocation render");
    forbid_allocation = false;
    require(chronobent_reset(p.get(), 96001, 0.25, 0.5) == CHRONOBENT_OK && chronobent_output_frames(p.get()) == 384004, "duration bound");
    require(chronobent_reset(p.get(), 8, 1, 0.49) == CHRONOBENT_INVALID_ARGUMENT && chronobent_output_frames(p.get()) == 384004, "invalid reset changed epoch");
    require(chronobent_render(p.get(), nullptr, nullptr, output.data(), 1, &got) == CHRONOBENT_INVALID_ARGUMENT && got == 0, "null reader");
}
}

void *operator new(std::size_t size) {
    require(!forbid_allocation, "render/reset allocated");
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

int main() {
    fft_oracle();
    identity_and_blocks();
    tone_accuracy();
    resampler_quality();
    attack_timing();
    envelope_quality();
    failure_and_reset();
    invalid_and_allocation();
    std::puts("chronobent: DFT oracle, identity, duration, block invariance, pitch, stereo, aliasing, failure/retry, reset and allocation checks passed");
}
