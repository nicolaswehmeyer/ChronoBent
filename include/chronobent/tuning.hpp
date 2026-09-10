// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_TUNING_CPP_HPP
#define CHRONOBENT_TUNING_CPP_HPP
#include "tuning.h"
#include <memory>
#include <stdexcept>
namespace chronobent_cpp {
// Move-only ownership. Source PCM and worker scheduling remain with the host.
class Tuner {
public:
    Tuner(chronobent_tune_config config, uint64_t frames)
        : handle_(nullptr, chronobent_tune_destroy) {
        chronobent_tuner *raw = nullptr;
        const auto status = chronobent_tune_create(&config, frames, &raw);
        if (status != CHRONOBENT_OK) throw std::runtime_error(chronobent_status_string(status));
        handle_.reset(raw);
    }
    Tuner(Tuner &&) noexcept = default;
    Tuner &operator=(Tuner &&) noexcept = default;
    Tuner(const Tuner &) = delete;
    Tuner &operator=(const Tuner &) = delete;
    chronobent_status analyze(chronobent_read_fn read, void *source,
        chronobent_tune_progress_fn progress = nullptr, void *user = nullptr) noexcept {
        return chronobent_tune_analyze(get(), read, source, progress, user);
    }
    chronobent_status set_options(chronobent_tune_options options) noexcept {
        return chronobent_tune_set_options(get(), &options);
    }
    chronobent_status options(chronobent_tune_options &out) const noexcept {
        return chronobent_tune_get_options(get(), &out);
    }
    chronobent_status set_note(size_t index, double target) noexcept {
        return chronobent_tune_set_note(get(), index, target);
    }
    chronobent_status set_notes(const chronobent_tune_edit *edits, size_t count) noexcept {
        return chronobent_tune_set_notes(get(), edits, count);
    }
    const chronobent_tune_frame *frames(size_t &count) const noexcept {
        return chronobent_tune_frames(get(), &count);
    }
    const chronobent_tune_note *notes(size_t &count) const noexcept {
        return chronobent_tune_notes(get(), &count);
    }
    chronobent_status render(chronobent_read_fn read, void *source, uint64_t first,
        float *out, size_t frames, size_t &produced) noexcept {
        return chronobent_tune_render(get(), read, source, first, out, frames, &produced);
    }
    chronobent_tuner *get() const noexcept { return handle_.get(); }
private:
    std::unique_ptr<chronobent_tuner, decltype(&chronobent_tune_destroy)> handle_;
};
}
#endif
