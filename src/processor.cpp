// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "chronobent/chronobent.h"
#include "controls.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace {
constexpr std::size_t block_frames = 256;
using chronobent_dsp::valid;
using chronobent_dsp::equal;
struct Epoch {
    std::unique_ptr<chronobent, decltype(&chronobent_destroy)> dsp{nullptr, chronobent_destroy};
    chronobent_read_fn reader = nullptr;
    void *user = nullptr;
    std::uint64_t origin = 0, position = 0;
    std::size_t channels = 0;
    std::array<float, 8> pending{};
    bool buffered = false;

    static int read(void *context, std::uint64_t first, std::size_t count, float *out) {
        const auto &self = *static_cast<Epoch *>(context);
        return self.reader(self.user, self.origin + first, count, out);
    }
    std::uint64_t remaining() const noexcept { return chronobent_output_frames(dsp.get()) - position; }
    chronobent_status render(float *out, std::size_t frames, std::size_t *got) noexcept {
        const auto status = chronobent_render(dsp.get(), read, this, out, frames, got);
        position += *got;
        return status;
    }
    // Retain one committed sample if the other side of a fade fails its read.
    chronobent_status stage() noexcept {
        if (buffered) return CHRONOBENT_OK;
        std::size_t got = 0;
        const auto status = render(pending.data(), 1, &got);
        if (status != CHRONOBENT_OK && status != CHRONOBENT_END) return status;
        if (!got) std::fill(pending.begin(), pending.end(), 0.0f);
        buffered = true;
        return CHRONOBENT_OK;
    }
};
}

struct chronobent_processor {
    std::array<Epoch, 2> epochs;
    std::array<float, block_frames * 8> scratch{};
    chronobent_read_fn reader = nullptr;
    void *user = nullptr;
    std::uint64_t length = 0, delivered = 0;
    double source_position = 0, trajectory_start = 0;
    std::uint64_t trajectory_frames = 0;
    chronobent_controls parameters{1,1,0,2,1,0};
    std::size_t channels = 0;
    unsigned active = 0;
    std::uint32_t fade_length = 0, fade_done = 0;
    bool ready = false, fading = false;

    Epoch &target() noexcept { return epochs[active ^ unsigned(fading)]; }
    const Epoch &target() const noexcept { return epochs[active ^ unsigned(fading)]; }
    bool ended() const noexcept { return !target().remaining() && !target().buffered; }
    chronobent_status prepare(Epoch &epoch, double at, const chronobent_controls &p) noexcept {
        const auto history = std::uint64_t(chronobent_window_frames(epoch.dsp.get())) * 4;
        const auto frame = static_cast<std::uint64_t>(std::floor(at));
        epoch.origin = frame > history ? frame - history : 0;
        epoch.reader = reader; epoch.user = user; epoch.position = 0; epoch.buffered = false;
        auto status = chronobent_reset_controls(epoch.dsp.get(), length - epoch.origin, &p);
        if (status != CHRONOBENT_OK) return status;
        auto skip = std::min(static_cast<std::uint64_t>(std::llround((at - double(epoch.origin)) / p.tempo)), epoch.remaining());
        // Seeking exactly to EOF must finish even when rounding would leave a sample.
        if (at == double(length)) skip = epoch.remaining();
        while (skip) {
            const auto count = std::size_t(std::min<std::uint64_t>(skip, block_frames));
            std::size_t got = 0;
            status = epoch.render(scratch.data(), count, &got);
            if (status != CHRONOBENT_OK && status != CHRONOBENT_END) return status;
            skip -= got;
        }
        return CHRONOBENT_OK;
    }
    void advance(std::size_t count) noexcept {
        delivered += count;
        trajectory_frames += count;
        source_position = std::min(double(length), trajectory_start + double(trajectory_frames) * parameters.tempo);
        if (ended()) source_position = double(length);
    }
};

extern "C" chronobent_status chronobent_processor_create(const chronobent_config *config,
    std::uint32_t transition_frames, chronobent_processor **instance) {
    if (!instance) return CHRONOBENT_INVALID_ARGUMENT;
    *instance = nullptr;
    if (transition_frames > 65536) return CHRONOBENT_INVALID_ARGUMENT;
    try {
        auto p = std::make_unique<chronobent_processor>();
        for (auto &epoch : p->epochs) {
            chronobent *raw = nullptr;
            const auto status = chronobent_create(config, &raw);
            if (status != CHRONOBENT_OK) return status;
            epoch.dsp.reset(raw); epoch.channels = config->channels;
        }
        p->channels = config->channels; p->fade_length = transition_frames;
        *instance = p.release();
    } catch (...) { return CHRONOBENT_OUT_OF_MEMORY; }
    return CHRONOBENT_OK;
}
extern "C" void chronobent_processor_destroy(chronobent_processor *instance) { delete instance; }

extern "C" chronobent_status chronobent_processor_set_source_controls(chronobent_processor *p,
    chronobent_read_fn reader, void *user, std::uint64_t frames, const chronobent_controls *parameters) {
    if (!p || !valid(parameters) || frames > (UINT64_C(1) << 48) || (frames && !reader))
        return CHRONOBENT_INVALID_ARGUMENT;
    auto &epoch = p->epochs[0];
    const auto status = chronobent_reset_controls(epoch.dsp.get(), frames, parameters);
    if (status != CHRONOBENT_OK) return status;
    epoch.reader = reader; epoch.user = user; epoch.origin = 0; epoch.position = 0; epoch.buffered = false;
    p->epochs[1].reader = nullptr; p->epochs[1].user = nullptr; p->epochs[1].buffered = false;
    p->reader = reader; p->user = user; p->length = frames; p->delivered = 0;
    p->source_position = 0; p->trajectory_start = 0; p->trajectory_frames = 0;
    p->parameters = *parameters; p->active = 0;
    p->fading = false; p->fade_done = 0; p->ready = true;
    return CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_processor_set_controls(chronobent_processor *p,
    const chronobent_controls *parameters) {
    if (!p || !valid(parameters)) return CHRONOBENT_INVALID_ARGUMENT;
    if (!p->ready) return CHRONOBENT_NOT_RESET;
    if (equal(*parameters, p->parameters)) return CHRONOBENT_OK;
    if (p->fading) return CHRONOBENT_BUSY;
    const auto status = p->prepare(p->epochs[p->active ^ 1], p->source_position, *parameters);
    if (status != CHRONOBENT_OK) return status;
    p->parameters = *parameters;
    p->trajectory_start = p->source_position; p->trajectory_frames = 0;
    p->fade_done = 0;
    p->fading = p->fade_length != 0 && p->epochs[p->active ^ 1].remaining() != 0;
    if (!p->fading) p->active ^= 1;
    return CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_processor_seek(chronobent_processor *p, std::uint64_t frame) {
    if (!p || frame > p->length) return CHRONOBENT_INVALID_ARGUMENT;
    if (!p->ready) return CHRONOBENT_NOT_RESET;
    if (p->fading) return CHRONOBENT_BUSY;
    const auto status = p->prepare(p->epochs[p->active ^ 1], double(frame), p->parameters);
    if (status != CHRONOBENT_OK) return status;
    p->active ^= 1; p->source_position = double(frame);
    p->trajectory_start = double(frame); p->trajectory_frames = 0;
    return CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_processor_render(chronobent_processor *p,
    float *out, std::size_t frames, std::size_t *produced) {
    if (produced) *produced = 0;
    if (!p || !produced || (frames && !out) || frames > std::numeric_limits<std::size_t>::max() / p->channels / sizeof(float))
        return CHRONOBENT_INVALID_ARGUMENT;
    if (!p->ready) return CHRONOBENT_NOT_RESET;
    while (*produced < frames && !p->ended()) {
        if (!p->fading) {
            std::size_t got = 0;
            const auto status = p->target().render(out + *produced * p->channels, frames - *produced, &got);
            *produced += got; p->advance(got);
            return status;
        }
        auto &old = p->epochs[p->active];
        auto &next = p->target();
        auto status = old.stage();
        if (status != CHRONOBENT_OK) return status;
        status = next.stage();
        if (status != CHRONOBENT_OK) return status;
        const float blend = float(p->fade_done + 1) / float(p->fade_length);
        for (std::size_t c = 0; c < p->channels; ++c)
            out[*produced * p->channels + c] = old.pending[c] * (1 - blend) + next.pending[c] * blend;
        old.buffered = false; next.buffered = false;
        ++*produced; ++p->fade_done;
        if (p->fade_done == p->fade_length || !next.remaining()) { p->active ^= 1; p->fading = false; }
        p->advance(1);
    }
    return p->ended() ? CHRONOBENT_END : CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_processor_render_planar(chronobent_processor *p,
    float *const *out, std::size_t frames, std::size_t *produced) {
    if (produced) *produced = 0;
    if (!p || !produced || (frames && !out) || frames > std::numeric_limits<std::size_t>::max() / sizeof(float))
        return CHRONOBENT_INVALID_ARGUMENT;
    if (frames) for (std::size_t c = 0; c < p->channels; ++c) if (!out[c]) return CHRONOBENT_INVALID_ARGUMENT;
    if (!p->ready) return CHRONOBENT_NOT_RESET;
    if (!frames) return p->ended() ? CHRONOBENT_END : CHRONOBENT_OK;
    while (*produced < frames) {
        std::size_t got = 0;
        const auto status = chronobent_processor_render(p, p->scratch.data(), std::min(block_frames, frames - *produced), &got);
        for (std::size_t c = 0; c < p->channels; ++c)
            for (std::size_t i = 0; i < got; ++i) out[c][*produced + i] = p->scratch[i * p->channels + c];
        *produced += got;
        if (status != CHRONOBENT_OK) return status;
    }
    return CHRONOBENT_OK;
}
extern "C" chronobent_status chronobent_processor_get_state(const chronobent_processor *p,
    chronobent_processor_state *state) {
    if (!p || !state) return CHRONOBENT_INVALID_ARGUMENT;
    if (!p->ready) return CHRONOBENT_NOT_RESET;
    *state = {p->length, p->delivered, p->source_position, chronobent_dsp::project(p->parameters),
        p->fading ? p->fade_length - p->fade_done : 0, p->ended() ? 1u : 0u};
    return CHRONOBENT_OK;
}

extern "C" chronobent_status chronobent_processor_set_source(chronobent_processor *p,
    chronobent_read_fn reader, void *user, std::uint64_t frames, const chronobent_parameters *parameters) {
    if (!valid(parameters)) return CHRONOBENT_INVALID_ARGUMENT;
    const auto controls = chronobent_dsp::extend(*parameters);
    return chronobent_processor_set_source_controls(p, reader, user, frames, &controls);
}
extern "C" chronobent_status chronobent_processor_set_parameters(chronobent_processor *p,
    const chronobent_parameters *parameters) {
    if (!valid(parameters)) return CHRONOBENT_INVALID_ARGUMENT;
    const auto controls = chronobent_dsp::extend(*parameters);
    return chronobent_processor_set_controls(p, &controls);
}
extern "C" chronobent_status chronobent_processor_get_controls(const chronobent_processor *p,
    chronobent_controls *controls) {
    if (!p || !controls) return CHRONOBENT_INVALID_ARGUMENT;
    if (!p->ready) return CHRONOBENT_NOT_RESET;
    *controls = p->parameters;
    return CHRONOBENT_OK;
}
