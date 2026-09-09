// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/chronobent.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
struct Source {
    std::vector<float> audio;
    static int read(void *context, std::uint64_t start, std::size_t count, float *out) {
        const auto &s = *static_cast<Source *>(context);
        std::copy_n(s.audio.data() + start * 2, count * 2, out);
        return 1;
    }
};
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
}

int main() {
    std::puts("sample_rate,tempo,pitch,output_seconds,render_seconds,realtime_factor,first_4096_ms,max_256_ms");
    for (double sr : {48000.0, 96000.0}) {
        const auto length = static_cast<std::size_t>(sr * 4);
        Source source{std::vector<float>(length * 2)};
        std::uint32_t noise = 21;
        for (std::size_t i = 0; i < length; ++i) {
            noise = noise * 1664525u + 1013904223u;
            const double t = static_cast<double>(i) / sr;
            const double attack = std::exp(-40 * std::fmod(t, 0.5));
            source.audio[2*i] = static_cast<float>(0.2*std::sin(2*3.141592653589793*55*t) +
                0.15*std::sin(2*3.141592653589793*440*t) + 0.2*attack*(double(noise)/4294967296.0-0.5));
            source.audio[2*i+1] = static_cast<float>(0.7*source.audio[2*i] + 0.1*std::sin(2*3.141592653589793*659.25*t));
        }
        for (auto ratios : {std::array<double, 2>{1, 0.5}, {1, 2}, {0.25, 2}, {1.08, 1}, {2, 0.5}}) {
            chronobent_config config{sr, 2, 0, 1, 0};
            chronobent *raw = nullptr;
            if (chronobent_create(&config, &raw) != CHRONOBENT_OK) return 1;
            std::unique_ptr<chronobent, decltype(&chronobent_destroy)> p(raw, chronobent_destroy);
            if (chronobent_reset(p.get(), length, ratios[0], ratios[1]) != CHRONOBENT_OK) return 2;
            std::array<float, 512> buffer{};
            std::uint64_t frames = 0;
            double first = 0, maximum = 0;
            auto start = Clock::now();
            for (;;) {
                const auto block_start = Clock::now();
                std::size_t got = 0;
                const auto status = chronobent_render(p.get(), Source::read, &source, buffer.data(), 256, &got);
                if (status != CHRONOBENT_OK && status != CHRONOBENT_END) return 3;
                maximum = std::max(maximum, seconds(block_start));
                frames += got;
                if (!first && frames >= 4096) first = seconds(start);
                if (status == CHRONOBENT_END) break;
            }
            const double elapsed = seconds(start), duration = static_cast<double>(frames) / sr;
            std::printf("%.0f,%.3f,%.3f,%.6f,%.6f,%.6f,%.3f,%.3f\n",
                sr, ratios[0], ratios[1], duration, elapsed, elapsed/duration, first*1000, maximum*1000);
        }
    }
}
