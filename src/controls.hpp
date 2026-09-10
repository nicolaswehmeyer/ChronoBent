// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_CONTROLS_HPP
#define CHRONOBENT_CONTROLS_HPP
#include "chronobent/chronobent.h"

namespace chronobent_dsp {
inline bool valid(const chronobent_controls *p) noexcept {
    return p && p->tempo >= .25 && p->tempo <= 4 && p->pitch >= .5 && p->pitch <= 2 &&
        (p->formant_scale == 0 || (p->formant_scale >= .5 && p->formant_scale <= 2)) &&
        p->envelope_ms >= 1 && p->envelope_ms <= 4 && p->transients <= 2 && !p->reserved;
}
inline bool valid(const chronobent_parameters *p) noexcept {
    return p && p->tempo >= .25 && p->tempo <= 4 && p->pitch >= .5 && p->pitch <= 2 &&
        p->transients <= 1 && p->formants <= 1;
}
inline chronobent_controls extend(const chronobent_parameters &p) noexcept {
    return {p.tempo, p.pitch, p.formants ? 1.0 : 0.0, 2, p.transients, 0};
}
inline chronobent_parameters project(const chronobent_controls &p) noexcept {
    return {p.tempo, p.pitch, p.transients ? 1u : 0u, p.formant_scale != 0 ? 1u : 0u};
}
inline bool equal(const chronobent_controls &a, const chronobent_controls &b) noexcept {
    return a.tempo == b.tempo && a.pitch == b.pitch && a.formant_scale == b.formant_scale &&
        a.envelope_ms == b.envelope_ms && a.transients == b.transients;
}
}
#endif
