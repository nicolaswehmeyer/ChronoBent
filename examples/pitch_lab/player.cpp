// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "player.hpp"
#include "chronobent/chronobent.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace audition {
namespace {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free &&
              std::atomic<double>::is_always_lock_free &&
              std::atomic<unsigned>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free, "Player requires lock-free callback atomics");
// A single word publishes a coherent command. Resolution is 0.00001 semitone
// and 0.000001 tempo ratio; only the control thread packs parameters.
std::uint64_t pack(Settings s) {
    const auto pitch = std::uint64_t(std::llround((std::clamp(s.semitones,-12.0,12.0)+12)*100000));
    const auto tempo = std::uint64_t(std::llround(std::clamp(s.tempo,0.5,2.0)*1000000));
    return pitch | (tempo << 22) | (std::uint64_t(s.master_tempo) << 43) |
        (std::uint64_t(s.bypass) << 44) | (std::uint64_t(s.formants) << 45);
}
struct Ratios { double pitch, tempo; bool formants; };
Ratios unpack(std::uint64_t word) {
    if ((word >> 44) & 1) return {1,1,false};
    const double tempo = double((word >> 22) & ((1u<<21)-1))/1000000;
    const bool master = (word >> 43) & 1;
    const double semitones = double(word & ((1u<<22)-1))/100000-12;
    return {master ? std::exp2(semitones/12) : tempo, tempo, master && bool((word >> 45) & 1)};
}

}
Player::Player(std::vector<float> stereo, double sample_rate, Settings initial)
    : audio_(std::move(stereo)), sample_rate_(sample_rate) {
    if (audio_.empty() || audio_.size()%2 || !(sample_rate >= 8000 && sample_rate <= 192000) ||
        !std::isfinite(initial.tempo) || !std::isfinite(initial.semitones))
        throw std::invalid_argument("Expected nonempty stereo PCM, 8..192 kHz and finite settings");
    settings_.store(pack(initial));
    worker_ = std::thread(&Player::run,this);
}
Player::~Player() { stopping_.store(true); worker_.join(); }
void Player::request(double semitones, bool bypass, bool formants) noexcept {
    request(Settings{semitones,1,true,bypass,formants});
}
void Player::request(Settings settings) noexcept {
    if (std::isfinite(settings.semitones) && std::isfinite(settings.tempo)) settings_.store(pack(settings));
}
void Player::pull(float *left, float *right, std::size_t count) noexcept {
    std::fill_n(left,count,0); std::fill_n(right,count,0);
    if (paused_.load(std::memory_order_relaxed) || stopping_.load(std::memory_order_relaxed)) return;
    const auto position = read_.load(std::memory_order_relaxed);
    const auto available = written_.load(std::memory_order_acquire)-position;
    const auto take = std::size_t(std::min<std::uint64_t>(count,available));
    for (std::size_t i=0; i<take; ++i) {
        const auto index = std::size_t(position+i) & (capacity-1);
        left[i]=queue_[2*index]; right[i]=queue_[2*index+1];
    }
    if (take) source_position_.store(source_queue_[std::size_t(position+take-1) & (capacity-1)],std::memory_order_relaxed);
    read_.store(position+take,std::memory_order_release);
    if (take != count && !finished_.load()) underruns_.fetch_add(1,std::memory_order_relaxed);
}
void Player::run() noexcept {
    try {
        chronobent_cpp::Processor processor(chronobent_default_config(sample_rate_,2),1024);
        auto source_read = [](void *context, std::uint64_t first, std::size_t count, float *out) -> int {
            const auto &source = *static_cast<const std::vector<float> *>(context);
            if (first > source.size()/2 || count > source.size()/2-first) return 0;
            std::copy_n(source.data()+first*2,count*2,out);
            return 1;
        };
        auto parameters = [](Ratios r) { return chronobent_parameters{r.tempo,r.pitch,1,r.formants ? 1u : 0u}; };
        auto check = [](chronobent_status status) {
            if (status != CHRONOBENT_OK && status != CHRONOBENT_END)
                throw std::runtime_error(chronobent_status_string(status));
        };
        check(processor.set_source(source_read,&audio_,frames(),parameters(unpack(settings_.load()))));
        std::array<float,512> output{};
        std::uint64_t position = 0;
        while (!stopping_.load()) {
            const auto free = capacity-(position-read_.load(std::memory_order_acquire));
            if (!free) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }
            // Coalesce UI automation while a fade is in flight. The reusable
            // processor owns preroll, both engines, mixing and source timeline.
            const auto command = processor.set_parameters(parameters(unpack(settings_.load())));
            if (command != CHRONOBENT_BUSY) check(command);
            chronobent_processor_state before{},after{};
            check(processor.state(before));
            std::size_t count = 0;
            const auto status = processor.render(output.data(),std::size_t(std::min<std::uint64_t>(256,free)),count);
            check(status); check(processor.state(after));
            for (std::size_t i=0; i<count; ++i) {
                const auto index = std::size_t(position+i) & (capacity-1);
                queue_[2*index]=output[2*i]; queue_[2*index+1]=output[2*i+1];
                source_queue_[index]=std::min(double(frames()),before.source_position+double(i+1)*before.parameters.tempo);
            }
            if (count) source_queue_[std::size_t(position+count-1)&(capacity-1)]=after.source_position;
            position+=count;
            written_.store(position,std::memory_order_release);
            if (status==CHRONOBENT_END) break;
        }
    } catch (...) { error_.store(1); }
    finished_.store(true);
}
}
