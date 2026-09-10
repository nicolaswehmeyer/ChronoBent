// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_PLUGIN_TUNING_STATE_HPP
#define CHRONOBENT_PLUGIN_TUNING_STATE_HPP
#include "../host/tuning_session.hpp"
#include "json.hpp"
namespace chronobent_plugin {
struct TuningState {
    bool present=false,corrected=false;
    chronobent_tune_options options=chronobent_tune_default_options();
    std::vector<chronobent_host::TuneEdit> edits;
};
nlohmann::json encode_tuning(const std::shared_ptr<const chronobent_host::TuneResult> &,bool corrected);
// Throws on malformed metadata before any host state is changed. PCM remains
// governed by the enclosing instrument/effect state admission and duration cap.
TuningState decode_tuning(const nlohmann::json &,uint64_t frames);
std::shared_ptr<const chronobent_host::TuneResult> restore_tuning(const TuningState &,
    chronobent_host::Audio selected,chronobent_host::Audio alternate,double sample_rate);
}
#endif
