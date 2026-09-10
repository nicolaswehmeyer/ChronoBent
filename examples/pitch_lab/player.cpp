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
// A single word publishes a coherent command. Controls use 0.01 semitone,
// 0.000001 tempo ratio and 0.01 ms envelope resolution. No torn parameter sets.
std::uint64_t pack(Settings s) {
    const auto pitch = std::uint64_t(std::llround((std::clamp(s.semitones,-48.0,48.0)+48)*100));
    const auto tempo = std::uint64_t(std::llround(std::clamp(s.tempo,0.5,2.0)*1000000));
    const auto formant = std::uint64_t(std::llround((std::clamp(s.formant_semitones,-12.0,12.0)+12)*100));
    const auto envelope = std::uint64_t(std::llround(std::clamp(s.envelope_ms,1.0,4.0)*100));
    return pitch | (tempo << 14) | (std::uint64_t(s.master_tempo) << 35) |
        (std::uint64_t(s.bypass) << 36) | (std::uint64_t(s.formants) << 37) |
        (formant << 38) | (std::uint64_t(std::min(s.transients,2u)) << 50) | (envelope << 52);
}
chronobent_controls unpack(std::uint64_t word) {
    if ((word >> 36) & 1) return chronobent_default_controls();
    const double tempo = double((word >> 14) & ((1u<<21)-1))/1000000;
    const bool master = (word >> 35) & 1;
    const double semitones = double(word & ((1u<<14)-1))/100-48;
    const double formant = double((word >> 38) & ((1u<<12)-1))/100-12;
    return {tempo, master ? std::exp2(semitones/12) : tempo,
        ((word >> 37) & 1) ? std::exp2(formant/12) : 0,
        double((word >> 52) & 511u)/100, std::uint32_t((word >> 50) & 3u), 0};
}

}
Player::Player(std::vector<float> stereo, double sample_rate, Settings initial, chronobent_profile profile)
    : Player(std::make_shared<const std::vector<float>>(std::move(stereo)),sample_rate,initial,profile) {}
Player::Player(std::shared_ptr<const std::vector<float>> stereo, double sample_rate, Settings initial, chronobent_profile profile)
    : audio_(std::move(stereo)), profile_(profile), sample_rate_(sample_rate) {
    if (!audio_ || audio_->empty() || audio_->size()%2 || !(sample_rate >= 8000 && sample_rate <= 192000) ||
        audio_->size()/2 > UINT32_MAX || !std::isfinite(initial.tempo) || !std::isfinite(initial.semitones) ||
        profile < CHRONOBENT_PROFILE_COMPACT || profile > CHRONOBENT_PROFILE_DETAILED ||
        !std::isfinite(initial.formant_semitones) || !std::isfinite(initial.envelope_ms))
        throw std::invalid_argument("Expected nonempty stereo PCM, 8..192 kHz and finite settings");
    settings_.store(pack(initial));
    worker_ = std::thread(&Player::run,this);
}
Player::~Player() { stopping_.store(true); worker_.join(); }
void Player::request(double semitones, bool bypass, bool formants) noexcept {
    request(Settings{semitones,1,true,bypass,formants});
}
void Player::request(Settings settings) noexcept {
    if (std::isfinite(settings.semitones) && std::isfinite(settings.tempo) &&
        std::isfinite(settings.formant_semitones) && std::isfinite(settings.envelope_ms)) settings_.store(pack(settings));
}
bool Player::seek(std::uint64_t source_frame) noexcept {
    if (source_frame > frames()) return false;
    // The bounded Lab source fits in 32 bits; the upper half is a command serial.
    ++seek_serial_;
    seek_request_.store((std::uint64_t(seek_serial_) << 32) | source_frame);
    return true;
}
void Player::pull(float *left, float *right, std::size_t count) noexcept {
    std::fill_n(left,count,0); std::fill_n(right,count,0);
    if (queue_handoff_.test_and_set(std::memory_order_acquire)) {
        // The worker may be prerolling a seek. Never wait for it on this thread.
        const auto ramp = std::min<std::size_t>(count,128);
        for (std::size_t i=0; i<ramp; ++i) {
            const float gain = 1-float(i+1)/float(ramp);
            left[i] = last_sample_[0]*gain; right[i] = last_sample_[1]*gain;
        }
        last_sample_ = {}; return;
    }
    struct Release {
        std::atomic_flag &flag;
        ~Release() { flag.clear(std::memory_order_release); }
    } release{queue_handoff_};
    if (paused_.load(std::memory_order_relaxed) || stopping_.load(std::memory_order_relaxed)) {
        last_sample_ = {}; return;
    }
    const auto seek = seek_done_.load();
    if (seek != audible_seek_) { audible_seek_ = seek; fade_in_ = 128; }
    const auto position = read_.load(std::memory_order_relaxed);
    const auto available = written_.load(std::memory_order_acquire)-position;
    const auto take = std::size_t(std::min<std::uint64_t>(count,available));
    for (std::size_t i=0; i<take; ++i) {
        const auto index = std::size_t(position+i) & (capacity-1);
        const float gain = fade_in_ ? float(129-fade_in_--)/128 : 1;
        // Decay the previous output while introducing the new source position.
        // This also handles a handoff that completed between audio callbacks.
        left[i]=queue_[2*index]*gain + last_sample_[0]*(1-gain);
        right[i]=queue_[2*index+1]*gain + last_sample_[1]*(1-gain);
    }
    if (take) {
        last_sample_ = {left[take-1],right[take-1]};
        source_position_.store(source_queue_[std::size_t(position+take-1) & (capacity-1)],std::memory_order_relaxed);
    }
    played_frames_.fetch_add(take,std::memory_order_relaxed);
    read_.store(position+take,std::memory_order_release);
    if (take != count && !finished_.load()) underruns_.fetch_add(1,std::memory_order_relaxed);
}
void Player::run() noexcept {
    try {
        chronobent_config config{};
        if (chronobent_config_for_profile(sample_rate_,2,profile_,&config) != CHRONOBENT_OK)
            throw std::runtime_error("Invalid analysis profile");
        chronobent_cpp::Processor processor(config,1024,{.0625,16});
        auto source_read = [](void *context, std::uint64_t first, std::size_t count, float *out) -> int {
            const auto &source = *static_cast<const Player *>(context)->audio_;
            if (first > source.size()/2 || count > source.size()/2-first) return 0;
            std::copy_n(source.data()+first*2,count*2,out);
            return 1;
        };
        auto check = [](chronobent_status status) {
            if (status != CHRONOBENT_OK && status != CHRONOBENT_END)
                throw std::runtime_error(chronobent_status_string(status));
        };
        check(processor.set_source_controls(source_read,this,frames(),unpack(settings_.load())));
        std::array<float,512> output{};
        std::uint64_t position = 0;
        while (!stopping_.load()) {
            const auto seek = seek_request_.load();
            if (seek != seek_done_.load()) {
                // Only the worker ever mutates processor state. Complete a
                // pending parameter fade before replacing its source position.
                chronobent_processor_state state{};
                check(processor.state(state));
                while (state.transition_remaining) {
                    std::size_t discarded = 0;
                    check(processor.render(output.data(),std::min<std::size_t>(256,state.transition_remaining),discarded));
                    check(processor.state(state));
                }
                while (queue_handoff_.test_and_set(std::memory_order_acquire)) {
                    if (stopping_.load()) return;
                    std::this_thread::yield();
                }
                const auto status = processor.seek(seek & UINT32_MAX);
                if (status == CHRONOBENT_OK) {
                    read_.store(position,std::memory_order_release);
                    source_position_.store(double(seek & UINT32_MAX));
                    finished_.store(false);
                    seek_done_.store(seek);
                }
                queue_handoff_.clear(std::memory_order_release);
                check(status);
            }
            if (finished_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }
            const auto free = capacity-(position-read_.load(std::memory_order_acquire));
            if (!free) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }
            // Coalesce UI automation while a fade is in flight. The reusable
            // processor owns preroll, both engines, mixing and source timeline.
            const auto command = processor.set_controls(unpack(settings_.load()));
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
            if (status==CHRONOBENT_END) finished_.store(true);
        }
    } catch (...) { error_.store(1); }
    finished_.store(true);
}
}
