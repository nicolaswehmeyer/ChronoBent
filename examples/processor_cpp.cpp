// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/chronobent.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

int main() {
    try {
        std::array<float, 4096> source{};
        auto read = [](void *context, uint64_t first, size_t frames, float *out) -> int {
            const auto &pcm = *static_cast<std::array<float,4096> *>(context);
            if (first > pcm.size()/2 || frames > pcm.size()/2-first) return 0;
            std::copy_n(pcm.data()+first*2, frames*2, out);
            return 1;
        };
        chronobent_cpp::Processor processor(chronobent_default_config(48000,2));
        auto parameters = chronobent_default_parameters();
        parameters.pitch = 2; // One octave up, unchanged duration.
        auto status = processor.set_source(read, &source, source.size()/2, parameters);
        std::array<float,256> left{}, right{};
        float *channels[]{left.data(), right.data()};
        while (status == CHRONOBENT_OK) {
            size_t produced = 0;
            status = processor.render_planar(channels, left.size(), produced);
            // Consume only produced frames. The processor owns no audio device.
        }
        return status == CHRONOBENT_END ? 0 : 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
