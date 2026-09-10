// SPDX-License-Identifier: MIT
#pragma once
#include "instrument.hpp"
#include <string>
namespace chronobent_instrument {
std::string choose_audio_file();
std::shared_ptr<const Source> load_audio_file(const std::string &path, double rate);
std::shared_ptr<const Source> resample_source(const Source &, double rate);
}
