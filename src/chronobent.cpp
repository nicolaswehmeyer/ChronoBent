// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/chronobent.h"
#include "sinc.hpp"
#include "vocoder.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {
constexpr std::uint64_t maximum_frames = UINT64_C(1) << 48;
std::uint32_t window_size(double rate) noexcept {
    std::uint32_t size = 512;
    while (size < 8192 && static_cast<double>(size) < rate * 0.04) size *= 2;
    return size;
}
bool valid_audio(float *samples, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(samples[i]) || std::abs(samples[i]) > 64.0f) return false;
    return true;
}
}

struct chronobent {
    std::size_t channels, window, ring_frames;
    chronobent_dsp::Vocoder vocoder;
    chronobent_dsp::Sinc sinc;
    std::vector<float> input, block, ring;
    std::uint64_t source_length = 0, output_length = 0, position = 0;
    std::int64_t generated = 0;
    double tempo = 1, pitch = 1, rate = 1;
    bool ready = false;
    bool formants;

    explicit chronobent(const chronobent_config &config)
        : channels(config.channels),
          window(config.window_frames ? config.window_frames : window_size(config.sample_rate)),
          ring_frames(window * 4),
          vocoder(window, channels, config.sample_rate, config.transients != 0, config.formants != 0),
          input(window * channels), block(window * channels / 4), ring(ring_frames * channels), formants(config.formants != 0) {}

    chronobent_status read(chronobent_read_fn reader, void *user,
                         std::int64_t first, std::size_t count) noexcept {
        std::fill_n(input.data(), count * channels, 0);
        const auto prefix = first < 0 ? std::min(count, static_cast<std::size_t>(-first)) : 0;
        const auto begin = first < 0 ? UINT64_C(0) : static_cast<std::uint64_t>(first);
        if (begin >= source_length || prefix == count) return CHRONOBENT_OK;
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(count - prefix, source_length - begin));
        float *destination = input.data() + prefix * channels;
        try {
            if (!reader(user, begin, take, destination)) return CHRONOBENT_SOURCE_UNAVAILABLE;
        } catch (...) {
            return CHRONOBENT_SOURCE_UNAVAILABLE;
        }
        return valid_audio(destination, take * channels) ? CHRONOBENT_OK : CHRONOBENT_INVALID_AUDIO;
    }

    chronobent_status ensure(std::int64_t last, chronobent_read_fn reader, void *user) noexcept {
        while (generated <= last) {
            const bool direct = rate == 1 && (!formants || pitch == 1);
            const auto synthesis = direct ? generated : vocoder.synthesis_start();
            const auto count = direct ? window / 8 : vocoder.hop();
            const auto status = read(reader, user, direct ? generated : vocoder.analysis_start(), direct ? count : window);
            if (status != CHRONOBENT_OK) return status;
            const float *samples = input.data();
            if (!direct) { vocoder.process(input.data(), block.data()); samples = block.data(); }
            // Startup frames synthesize the overlap history at negative times.
            for (std::size_t i = 0; i < count; ++i) {
                const auto frame = synthesis + static_cast<std::int64_t>(i);
                if (frame < 0) continue;
                const auto slot = static_cast<std::size_t>(frame) & (ring_frames - 1);
                std::copy_n(samples + i * channels, channels, ring.data() + slot * channels);
                generated = frame + 1;
            }
        }
        return CHRONOBENT_OK;
    }

    float sample(std::int64_t frame, std::size_t channel) const noexcept {
        if (frame < 0) return 0;
        return ring[(static_cast<std::size_t>(frame) & (ring_frames - 1)) * channels + channel];
    }
};

extern "C" chronobent_status chronobent_create(const chronobent_config *config, chronobent **instance) {
    if (!instance) return CHRONOBENT_INVALID_ARGUMENT;
    *instance = nullptr;
    if (!config || !(config->sample_rate >= 8000 && config->sample_rate <= 192000) ||
        !config->channels || config->channels > 8 || config->transients > 1 || config->formants > 1 ||
        (config->window_frames && (config->window_frames < 512 || config->window_frames > 8192 ||
          (config->window_frames & (config->window_frames - 1))))) return CHRONOBENT_INVALID_ARGUMENT;
    try { *instance = new chronobent(*config); }
    catch (...) { return CHRONOBENT_OUT_OF_MEMORY; }
    return CHRONOBENT_OK;
}

extern "C" void chronobent_destroy(chronobent *instance) { delete instance; }

extern "C" chronobent_status chronobent_reset(chronobent *instance, std::uint64_t input_frames,
                                          double tempo, double pitch) {
    if (!instance || input_frames > maximum_frames || !(tempo >= 0.25 && tempo <= 4) ||
        !(pitch >= 0.5 && pitch <= 2)) return CHRONOBENT_INVALID_ARGUMENT;
    instance->source_length = input_frames;
    instance->output_length = static_cast<std::uint64_t>(std::ceil(static_cast<double>(input_frames) / tempo));
    instance->position = 0;
    instance->generated = 0;
    instance->tempo = tempo;
    instance->pitch = pitch;
    instance->rate = tempo / pitch;
    instance->vocoder.reset(instance->rate, pitch);
    instance->sinc.prepare(pitch);
    std::fill(instance->ring.begin(), instance->ring.end(), 0);
    instance->ready = true;
    return CHRONOBENT_OK;
}

extern "C" chronobent_status chronobent_render(chronobent *instance, chronobent_read_fn reader,
    void *user, float *output, std::size_t frames, std::size_t *produced) {
    if (produced) *produced = 0;
    if (!instance || !produced || frames > std::numeric_limits<std::size_t>::max() / instance->channels / sizeof(float) ||
        (frames && (!reader || !output))) return CHRONOBENT_INVALID_ARGUMENT;
    if (!instance->ready) return CHRONOBENT_NOT_RESET;
    if (instance->position == instance->output_length) return CHRONOBENT_END;
    frames = static_cast<std::size_t>(std::min<std::uint64_t>(frames, instance->output_length - instance->position));
    const auto channels = instance->channels;
    // Exact identity includes endpoints; no window, latency or resampling filter.
    if (instance->tempo == 1 && instance->pitch == 1) {
        while (*produced < frames) {
            const auto count = std::min(frames - *produced, instance->window);
            const auto status = instance->read(reader, user, static_cast<std::int64_t>(instance->position), count);
            if (status != CHRONOBENT_OK) return status;
            std::memcpy(output + *produced * channels, instance->input.data(), count * channels * sizeof(float));
            instance->position += count;
            *produced += count;
        }
    } else {
        std::array<float, chronobent_dsp::Sinc::taps> coefficients{};
        for (; *produced < frames; ++*produced, ++instance->position) {
            // Absolute indexing avoids incremental ratio drift between blocks.
            const double source = static_cast<double>(instance->position) * instance->pitch;
            const auto center = static_cast<std::int64_t>(std::floor(source));
            const bool resample = instance->pitch != 1;
            const auto status = instance->ensure(center + (resample ? chronobent_dsp::Sinc::right : 0), reader, user);
            if (status != CHRONOBENT_OK) return status;
            if (resample) instance->sinc.coefficients(source - static_cast<double>(center), coefficients);
            for (std::size_t c = 0; c < channels; ++c) {
                float value = 0;
                if (resample) {
                    for (std::size_t tap = 0; tap < chronobent_dsp::Sinc::taps; ++tap)
                        value += coefficients[tap] * instance->sample(center - chronobent_dsp::Sinc::left + static_cast<std::int64_t>(tap), c);
                } else value = instance->sample(center, c);
                output[*produced * channels + c] = value;
            }
        }
    }
    return instance->position == instance->output_length ? CHRONOBENT_END : CHRONOBENT_OK;
}

extern "C" std::uint64_t chronobent_output_frames(const chronobent *instance) {
    return instance && instance->ready ? instance->output_length : 0;
}
extern "C" std::uint32_t chronobent_window_frames(const chronobent *instance) {
    return instance ? static_cast<std::uint32_t>(instance->window) : 0;
}
extern "C" const char *chronobent_version(void) { return "0.1.0"; }
