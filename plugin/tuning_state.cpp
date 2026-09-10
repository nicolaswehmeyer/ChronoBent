// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "tuning_state.hpp"
#include <cmath>
#include <stdexcept>
namespace chronobent_plugin {
using json=nlohmann::json;
json encode_tuning(const std::shared_ptr<const chronobent_host::TuneResult> &result,bool corrected) {
    if(!result)return nullptr;
    const auto &o=result->options;json edits=json::array();
    for(const auto &e:result->edits)edits.push_back({e.first,e.end,e.target});
    return {{"version",1},{"corrected",corrected},{"options",{o.scale_mask,o.reference_hz,o.amount,o.retune_ms,o.preserve_vibrato,o.correct_drift,o.maximum_shift}},{"edits",edits}};
}
TuningState decode_tuning(const json &value,uint64_t frames) {
    TuningState state;if(value.is_null())return state;
    if(!frames || !value.is_object() || value.at("version")!=1 || !value.at("corrected").is_boolean())throw std::runtime_error("Invalid saved tuning metadata");
    const auto &o=value.at("options");const auto &edits=value.at("edits");
    if(!o.is_array() || o.size()!=7 || !o[0].is_number_unsigned() || !edits.is_array() || edits.size()>24000)throw std::runtime_error("Invalid tuning settings");
    for(const auto &number:o)if(!number.is_number())throw std::runtime_error("Invalid tuning value");
    const auto mask=o[0].get<uint64_t>();if(mask>4095)throw std::runtime_error("Invalid tuning scale");
    state.options={uint32_t(mask),o[1].get<double>(),o[2].get<double>(),o[3].get<double>(),o[4].get<double>(),o[5].get<double>(),o[6].get<double>()};
    if(chronobent_tune_validate_options(&state.options)!=CHRONOBENT_OK)throw std::runtime_error("Tuning setting outside supported range");
    uint64_t previous=0;state.edits.reserve(edits.size());
    for(const auto &e:edits) {
        if(!e.is_array() || e.size()!=3 || !e[0].is_number_unsigned() || !e[1].is_number_unsigned() || !e[2].is_number())throw std::runtime_error("Invalid saved note edit");
        const auto first=e[0].get<uint64_t>(),end=e[1].get<uint64_t>();const auto target=e[2].get<double>();
        if(first<previous || end<=first || end>frames || !(target>=0 && target<=127))throw std::runtime_error("Saved note edit outside the source");
        state.edits.push_back({first,end,target});previous=end;
    }
    state.present=true;state.corrected=value.at("corrected").get<bool>();return state;
}
std::shared_ptr<const chronobent_host::TuneResult> restore_tuning(const TuningState &state,
    chronobent_host::Audio selected,chronobent_host::Audio alternate,double rate) {
    if(!state.present)return nullptr;
    if(!selected || !alternate || selected->size()!=alternate->size())throw std::runtime_error("Incomplete saved tuning audio");
    auto result=std::make_shared<chronobent_host::TuneResult>();result->sample_rate=rate;
    result->original=state.corrected?alternate:selected;result->corrected=state.corrected?selected:alternate;
    result->options=state.options;result->edits=state.edits;return result;
}
}
