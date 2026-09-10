// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_MAC_WAV_EXPORT_HPP
#define CHRONOBENT_MAC_WAV_EXPORT_HPP
#include "tuning_session.hpp"
namespace chronobent_host {
// Worker-only host I/O. Write stereo 32-bit float WAV, then atomically replace
// the user-selected destination. Failure preserves an existing destination.
bool export_wav(Audio,double sample_rate,const std::string &path,std::string &error) noexcept;
}
#endif
