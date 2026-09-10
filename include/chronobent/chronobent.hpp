// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_CPP_HPP
#define CHRONOBENT_CPP_HPP
#include "chronobent.h"
#include <memory>
#include <stdexcept>
#include <utility>

namespace chronobent_cpp {
// Move-only ownership. Construction throws on failure; processing returns the
// C status unchanged, so source retry needs no exception machinery in the host.
class Processor {
public:
    explicit Processor(chronobent_config config, uint32_t transition_frames = 1024)
        : handle_(nullptr, chronobent_processor_destroy) {
        chronobent_processor *raw = nullptr;
        const auto status = chronobent_processor_create(&config, transition_frames, &raw);
        if (status != CHRONOBENT_OK) throw std::runtime_error(chronobent_status_string(status));
        handle_.reset(raw);
    }
    Processor(Processor &&) noexcept = default;
    Processor &operator=(Processor &&) noexcept = default;
    Processor(const Processor &) = delete;
    Processor &operator=(const Processor &) = delete;
    chronobent_status set_source(chronobent_read_fn read, void *user, uint64_t frames,
        chronobent_parameters parameters = chronobent_default_parameters()) noexcept {
        return chronobent_processor_set_source(get(), read, user, frames, &parameters);
    }
    chronobent_status set_parameters(chronobent_parameters parameters) noexcept {
        return chronobent_processor_set_parameters(get(), &parameters);
    }
    chronobent_status set_source_controls(chronobent_read_fn read, void *user, uint64_t frames,
        chronobent_controls controls) noexcept {
        return chronobent_processor_set_source_controls(get(), read, user, frames, &controls);
    }
    chronobent_status set_controls(chronobent_controls controls) noexcept {
        return chronobent_processor_set_controls(get(), &controls);
    }
    chronobent_status controls(chronobent_controls &out) const noexcept {
        return chronobent_processor_get_controls(get(), &out);
    }
    chronobent_status seek(uint64_t frame) noexcept { return chronobent_processor_seek(get(), frame); }
    chronobent_status render(float *out, size_t frames, size_t &produced) noexcept {
        return chronobent_processor_render(get(), out, frames, &produced);
    }
    chronobent_status render_planar(float *const *out, size_t frames, size_t &produced) noexcept {
        return chronobent_processor_render_planar(get(), out, frames, &produced);
    }
    chronobent_status state(chronobent_processor_state &out) const noexcept {
        return chronobent_processor_get_state(get(), &out);
    }
    chronobent_processor *get() const noexcept { return handle_.get(); }
private:
    std::unique_ptr<chronobent_processor, decltype(&chronobent_processor_destroy)> handle_;
};
}
#endif
