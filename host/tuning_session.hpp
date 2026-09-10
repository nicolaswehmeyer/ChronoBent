// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_TUNING_SESSION_HPP
#define CHRONOBENT_TUNING_SESSION_HPP
#include "chronobent/tuning.h"
#include <memory>
#include <string>
#include <vector>
namespace chronobent_host {
// All storage, worker and publication ownership lives outside the DSP inventory.
using Audio = std::shared_ptr<const std::vector<float>>;
struct TuneEdit { uint64_t first=0,end=0;double target=0; };
struct TuneResult {
    uint64_t revision=0;
    double sample_rate=0;
    Audio original, corrected;
    chronobent_tune_options options=chronobent_tune_default_options();
    std::vector<TuneEdit> edits;
    std::vector<chronobent_tune_frame> frames;
    std::vector<chronobent_tune_note> notes;
};
struct TuneSnapshot {
    bool busy=false;
    double progress=0;
    uint64_t requested=0;
    std::string error;
    std::shared_ptr<const TuneResult> result;
};
// Non-audio callers only. Requests coalesce, cancel obsolete work and publish
// complete immutable results. Failure preserves the last completed result.
// The host explicitly selects original or corrected PCM for its audio engine.
// Input is immutable stereo, 8..192 kHz and at most 600 seconds.
class TuningSession {
public:
    TuningSession();
    ~TuningSession();
    TuningSession(const TuningSession &)=delete;
    TuningSession &operator=(const TuningSession &)=delete;
    // Clears analysis/edits for a new input; does not start analysis.
    bool set_source(Audio,double sample_rate);
    bool analyze();
    // A project may retain exact selected PCM and an editable original. This
    // installs a complete saved result without running analysis. Notes/frames
    // may be empty; analyze lazily reconstructs them from the original source.
    bool restore(std::shared_ptr<const TuneResult>);
    // Invalid options/edits preserve the pending and completed state.
    // Edits use exact half-open note ranges from the current analysis.
    bool apply(chronobent_tune_options,const std::vector<TuneEdit> &);
    void cancel();
    void clear();
    TuneSnapshot snapshot() const;
    bool wait(unsigned milliseconds=60000) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
