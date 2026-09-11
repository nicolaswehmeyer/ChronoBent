// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/chronobent.h"
#include "sinc.hpp"
#include "controls.hpp"
#include "vocoder.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define CHRONOBENT_NEON 1
#endif

namespace {
constexpr std::uint64_t maximum_frames = UINT64_C(1) << 48;
// Interpolation over a contiguous span of interleaved frames. Four independent
// accumulators per channel take tap t in lane t % 4; the scalar and NEON paths
// share that order, and the lane sum is fixed, so a given absolute position
// renders the same samples regardless of block partition.
void dot_stereo(const float *frames, const float *c, std::size_t taps, float *out) noexcept {
    float left[4] = {0, 0, 0, 0}, right[4] = {0, 0, 0, 0};
    std::size_t t = 0;
#ifdef CHRONOBENT_NEON
    float32x4_t l = vdupq_n_f32(0), r = vdupq_n_f32(0);
    for (; t + 4 <= taps; t += 4) {
        const float32x4x2_t x = vld2q_f32(frames + 2 * t);
        const float32x4_t k = vld1q_f32(c + t);
        l = vmlaq_f32(l, k, x.val[0]);
        r = vmlaq_f32(r, k, x.val[1]);
    }
    vst1q_f32(left, l); vst1q_f32(right, r);
#endif
    for (; t < taps; ++t) {
        left[t & 3] += c[t] * frames[2 * t];
        right[t & 3] += c[t] * frames[2 * t + 1];
    }
    out[0] = (left[0] + left[1]) + (left[2] + left[3]);
    out[1] = (right[0] + right[1]) + (right[2] + right[3]);
}
float dot_channel(const float *frames, std::size_t stride, const float *c, std::size_t taps) noexcept {
    float lanes[4] = {0, 0, 0, 0};
    for (std::size_t t = 0; t < taps; ++t) lanes[t & 3] += c[t] * frames[t * stride];
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
}
std::uint32_t window_size(double rate) noexcept {
    std::uint32_t size = 512;
    while (size < 8192 && static_cast<double>(size) < rate * 0.04) size *= 2;
    return size;
}
// Finite and within +-64: a NaN fails the magnitude comparison in both paths.
bool valid_audio(const float *samples, std::size_t count) noexcept {
    std::size_t i = 0;
    bool ok = true;
#ifdef CHRONOBENT_NEON
    const float32x4_t limit = vdupq_n_f32(64.0f);
    uint32x4_t all = vdupq_n_u32(0xFFFFFFFFu);
    for (; i + 4 <= count; i += 4) all = vandq_u32(all, vcaleq_f32(vld1q_f32(samples + i), limit));
    ok = vminvq_u32(all) != 0;
#endif
    for (; i < count; ++i) ok = ok && std::fabs(samples[i]) <= 64.0f;
    return ok;
}
}

struct chronobent {
    std::size_t channels, window, ring_frames, mirror_frames;
    chronobent_dsp::Vocoder vocoder;
    chronobent_dsp::Sinc sinc;
    // The ring repeats its first mirror_frames slots after the end, so every
    // interpolation span of at most that many taps is contiguous in memory.
    std::vector<float> input, block, ring, coefficients;
    const chronobent_pitch_range pitch_range;
    std::uint64_t source_length = 0, output_length = 0, position = 0;
    std::int64_t generated = 0, cache_first = 0;
    std::size_t cache_count = 0;
    double tempo = 1, pitch = 1, rate = 1;
    bool ready = false;
    double formant_scale;

    explicit chronobent(const chronobent_config &config, chronobent_pitch_range range)
        : channels(config.channels),
          window(config.window_frames ? config.window_frames : window_size(config.sample_rate)),
          ring_frames(window * 4), mirror_frames(chronobent_dsp::Sinc::capacity(range.maximum)),
          vocoder(window, channels, config.sample_rate, config.transients != 0, config.formants != 0),
          sinc(range.maximum),
          input(window * channels), block(window * channels / 4), ring((ring_frames + mirror_frames) * channels),
          coefficients(mirror_frames), pitch_range(range), formant_scale(config.formants ? 1 : 0) {}

    // Fills count frames from first; frames outside the declared source are zero.
    chronobent_status fetch(chronobent_read_fn reader, void *user, std::int64_t first,
                            std::size_t count, float *destination) noexcept {
        std::fill_n(destination, count * channels, 0.0f);
        const auto prefix = first < 0 ? std::min(count, static_cast<std::size_t>(-first)) : 0;
        const auto begin = first < 0 ? UINT64_C(0) : static_cast<std::uint64_t>(first);
        if (begin >= source_length || prefix == count) return CHRONOBENT_OK;
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(count - prefix, source_length - begin));
        float *target = destination + prefix * channels;
        try {
            if (!reader(user, begin, take, target)) return CHRONOBENT_SOURCE_UNAVAILABLE;
        } catch (...) {
            return CHRONOBENT_SOURCE_UNAVAILABLE;
        }
        return valid_audio(target, take * channels) ? CHRONOBENT_OK : CHRONOBENT_INVALID_AUDIO;
    }
    chronobent_status read(chronobent_read_fn reader, void *user, std::int64_t first, std::size_t count) noexcept {
        cache_count = 0;
        return fetch(reader, user, first, count, input.data());
    }
    // Consecutive analysis frames overlap by at least three quarters. Keep the
    // frames already validated from the previous window and fetch only those
    // beyond it; an epoch's source is immutable, so retained frames stay valid.
    // A failed fetch keeps the validated prefix and leaves the frame retryable.
    chronobent_status read_window(chronobent_read_fn reader, void *user, std::int64_t first) noexcept {
        const auto end = cache_first + static_cast<std::int64_t>(cache_count);
        if (cache_count && first >= cache_first && first < end) {
            const auto keep = static_cast<std::size_t>(end - first);
            if (first != cache_first)
                std::memmove(input.data(), input.data() + static_cast<std::size_t>(first - cache_first) * channels,
                             keep * channels * sizeof(float));
            cache_count = keep;
        } else cache_count = 0;
        cache_first = first;
        if (cache_count < window) {
            const auto status = fetch(reader, user, first + static_cast<std::int64_t>(cache_count),
                                      window - cache_count, input.data() + cache_count * channels);
            if (status != CHRONOBENT_OK) return status;
            cache_count = window;
        }
        return CHRONOBENT_OK;
    }

    chronobent_status ensure(std::int64_t last, chronobent_read_fn reader, void *user) noexcept {
        while (generated <= last) {
            const bool direct = rate == 1 && (formant_scale == 0 || formant_scale == pitch);
            const auto synthesis = direct ? generated : vocoder.synthesis_start();
            const auto count = direct ? window / 8 : vocoder.hop();
            const auto status = direct ? read(reader, user, generated, count)
                                       : read_window(reader, user, vocoder.analysis_start());
            if (status != CHRONOBENT_OK) return status;
            const float *samples = input.data();
            if (!direct) { vocoder.process(input.data(), block.data()); samples = block.data(); }
            // Startup frames synthesize the overlap history at negative times.
            for (std::size_t i = 0; i < count; ++i) {
                const auto frame = synthesis + static_cast<std::int64_t>(i);
                if (frame < 0) continue;
                const auto slot = static_cast<std::size_t>(frame) & (ring_frames - 1);
                std::copy_n(samples + i * channels, channels, ring.data() + slot * channels);
                if (slot < mirror_frames)
                    std::copy_n(samples + i * channels, channels, ring.data() + (slot + ring_frames) * channels);
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
    const chronobent_pitch_range range{.5,2};
    return chronobent_create_with_pitch_range(config, &range, instance);
}
extern "C" chronobent_status chronobent_create_with_pitch_range(const chronobent_config *config,
    const chronobent_pitch_range *range, chronobent **instance) {
    if (!instance) return CHRONOBENT_INVALID_ARGUMENT;
    *instance = nullptr;
    if (!chronobent_dsp::valid(range) || !config || !(config->sample_rate >= 8000 && config->sample_rate <= 192000) ||
        !config->channels || config->channels > 8 || config->transients > 1 || config->formants > 1 ||
        (config->window_frames && (config->window_frames < 512 || config->window_frames > 8192 ||
          (config->window_frames & (config->window_frames - 1))))) return CHRONOBENT_INVALID_ARGUMENT;
    try { *instance = new chronobent(*config, *range); }
    catch (...) { return CHRONOBENT_OUT_OF_MEMORY; }
    return CHRONOBENT_OK;
}

extern "C" void chronobent_destroy(chronobent *instance) { delete instance; }

extern "C" chronobent_status chronobent_reset(chronobent *instance, std::uint64_t input_frames,
                                          double tempo, double pitch) {
    if (!instance || input_frames > maximum_frames || !(tempo >= 0.25 && tempo <= 4) ||
        !chronobent_dsp::admits(instance->pitch_range, pitch)) return CHRONOBENT_INVALID_ARGUMENT;
    instance->source_length = input_frames;
    instance->output_length = static_cast<std::uint64_t>(std::ceil(static_cast<double>(input_frames) / tempo));
    instance->position = 0;
    instance->generated = 0;
    instance->cache_count = 0;
    instance->tempo = tempo;
    instance->pitch = pitch;
    instance->rate = tempo / pitch;
    instance->vocoder.reset(instance->rate, pitch);
    instance->sinc.prepare(pitch);
    std::fill(instance->ring.begin(), instance->ring.end(), 0.0f);
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
    if (instance->tempo == 1 && instance->pitch == 1 &&
        (instance->formant_scale == 0 || instance->formant_scale == 1)) {
        while (*produced < frames) {
            const auto count = std::min(frames - *produced, instance->window);
            const auto status = instance->read(reader, user, static_cast<std::int64_t>(instance->position), count);
            if (status != CHRONOBENT_OK) return status;
            std::memcpy(output + *produced * channels, instance->input.data(), count * channels * sizeof(float));
            instance->position += count;
            *produced += count;
        }
    } else {
        float *coefficients = instance->coefficients.data();
        double coefficient_fraction = -1;
        for (; *produced < frames; ++*produced, ++instance->position) {
            // Absolute indexing avoids incremental ratio drift between blocks.
            const double source = static_cast<double>(instance->position) * instance->pitch;
            const auto center = static_cast<std::int64_t>(std::floor(source));
            const bool resample = instance->pitch != 1;
            const auto status = instance->ensure(center + (resample ? instance->sinc.right() : 0), reader, user);
            if (status != CHRONOBENT_OK) return status;
            if (resample) {
                const double fraction = source - static_cast<double>(center);
                // Repeated phases use identical coefficients; retain their exact
                // arithmetic while avoiding redundant work (notably pitch=2).
                if (fraction != coefficient_fraction) {
                    instance->sinc.coefficients(fraction, coefficients);
                    coefficient_fraction = fraction;
                }
            }
            if (!resample) {
                for (std::size_t c = 0; c < channels; ++c) output[*produced * channels + c] = instance->sample(center, c);
                continue;
            }
            const auto first = center - instance->sinc.left();
            const auto taps = instance->sinc.taps();
            if (first >= 0) {
                // The mirrored ring keeps the whole span contiguous.
                const float *span = instance->ring.data() +
                    (static_cast<std::size_t>(first) & (instance->ring_frames - 1)) * channels;
                if (channels == 2) dot_stereo(span, coefficients, taps, output + *produced * 2);
                else for (std::size_t c = 0; c < channels; ++c)
                    output[*produced * channels + c] = dot_channel(span + c, channels, coefficients, taps);
                continue;
            }
            // Only the leading taps of the first frames reach before the source.
            for (std::size_t c = 0; c < channels; ++c) {
                float lanes[4] = {0, 0, 0, 0};
                for (std::size_t tap = 0; tap < taps; ++tap)
                    lanes[tap & 3] += coefficients[tap] * instance->sample(first + static_cast<std::int64_t>(tap), c);
                output[*produced * channels + c] = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
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
extern "C" const char *chronobent_version(void) { return "0.7.0"; }

extern "C" chronobent_config chronobent_default_config(double sample_rate, std::uint32_t channels) {
    return {sample_rate, channels, 0, 1, 0};
}
extern "C" chronobent_parameters chronobent_default_parameters(void) { return {1, 1, 1, 0}; }
extern "C" const char *chronobent_status_string(chronobent_status status) {
    switch (status) {
    case CHRONOBENT_OK: return "OK";
    case CHRONOBENT_END: return "End of source";
    case CHRONOBENT_INVALID_ARGUMENT: return "Invalid argument";
    case CHRONOBENT_OUT_OF_MEMORY: return "Out of memory";
    case CHRONOBENT_SOURCE_UNAVAILABLE: return "Source unavailable";
    case CHRONOBENT_INVALID_AUDIO: return "Invalid audio";
    case CHRONOBENT_NOT_RESET: return "Source or epoch not initialized";
    case CHRONOBENT_BUSY: return "Transition in progress";
    case CHRONOBENT_CANCELLED: return "Analysis cancelled";
    default: return "Unknown status";
    }
}
extern "C" chronobent_status chronobent_reset_parameters(chronobent *instance,
    std::uint64_t input_frames, const chronobent_parameters *p) {
    if (!chronobent_dsp::valid(p)) return CHRONOBENT_INVALID_ARGUMENT;
    const auto controls = chronobent_dsp::extend(*p);
    return chronobent_reset_controls(instance, input_frames, &controls);
}
extern "C" chronobent_controls chronobent_default_controls(void) { return {1, 1, 0, 2, 1, 0}; }
extern "C" chronobent_status chronobent_reset_controls(chronobent *instance,
    std::uint64_t input_frames, const chronobent_controls *p) {
    if (!instance || !chronobent_dsp::valid(p) || !chronobent_dsp::admits(instance->pitch_range, p->pitch) || input_frames > maximum_frames)
        return CHRONOBENT_INVALID_ARGUMENT;
    instance->formant_scale = p->formant_scale;
    instance->vocoder.options(p->transients, p->formant_scale, p->envelope_ms);
    return chronobent_reset(instance, input_frames, p->tempo, p->pitch);
}
extern "C" chronobent_status chronobent_config_for_profile(double rate, std::uint32_t channels,
    chronobent_profile profile, chronobent_config *config) {
    if (!config || !(rate >= 8000 && rate <= 192000) || !channels || channels > 8 ||
        profile < CHRONOBENT_PROFILE_COMPACT || profile > CHRONOBENT_PROFILE_DETAILED)
        return CHRONOBENT_INVALID_ARGUMENT;
    auto result = chronobent_default_config(rate, channels);
    result.window_frames = window_size(rate * (profile == CHRONOBENT_PROFILE_COMPACT ? .5 :
                                               profile == CHRONOBENT_PROFILE_DETAILED ? 2 : 1));
    *config = result;
    return CHRONOBENT_OK;
}

extern "C" chronobent_status chronobent_get_pitch_range(const chronobent *instance, chronobent_pitch_range *range) {
    if (!instance || !range) return CHRONOBENT_INVALID_ARGUMENT;
    *range = instance->pitch_range;
    return CHRONOBENT_OK;
}
