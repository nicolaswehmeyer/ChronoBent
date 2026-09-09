// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#include "player.hpp"
#include "chronobent/chronobent.h"
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
struct Epoch {
    std::unique_ptr<chronobent, decltype(&chronobent_destroy)> dsp{nullptr, chronobent_destroy};
    const std::vector<float> &audio;
    std::uint64_t origin = 0, output = 0;
    Ratios ratios;
    Epoch(const std::vector<float> &source, double sr, double position, Ratios settings)
        : audio(source), ratios(settings) {
        chronobent_config config{sr,2,0,1,settings.formants ? 1u : 0u};
        chronobent *raw = nullptr;
        if (chronobent_create(&config,&raw) != CHRONOBENT_OK) throw std::runtime_error("create failed");
        dsp.reset(raw);
        const auto history = std::uint64_t(chronobent_window_frames(raw))*4;
        const auto at = std::uint64_t(std::floor(position));
        origin = at > history ? at-history : 0;
        if (chronobent_reset(raw,audio.size()/2-origin,ratios.tempo,ratios.pitch) != CHRONOBENT_OK)
            throw std::runtime_error("reset failed");
        // Align on the new epoch's nearest output sample. At the supported app
        // speeds this introduces at most one source sample of phase alignment
        // error. The host timeline remains continuous and never rounds a block.
        auto remaining = std::uint64_t(std::llround((position-double(origin))/ratios.tempo));
        remaining = std::min(remaining,chronobent_output_frames(raw));
        std::array<float,512> discard{};
        while (remaining) {
            const auto count = std::size_t(std::min<std::uint64_t>(256,remaining));
            if (render(discard.data(),count) != count) throw std::runtime_error("preroll failed");
            remaining -= count;
        }
    }
    static int read(void *context, std::uint64_t first, std::size_t count, float *out) {
        auto &self = *static_cast<Epoch *>(context);
        const auto length = self.audio.size()/2;
        if (first > length-self.origin || count > length-self.origin-first) return 0;
        std::copy_n(self.audio.data()+2*(self.origin+first),count*2,out);
        return 1;
    }
    std::uint64_t remaining() const { return chronobent_output_frames(dsp.get())-output; }
    std::size_t render(float *out, std::size_t count) {
        std::size_t got = 0;
        const auto status = chronobent_render(dsp.get(),read,this,out,count,&got);
        if (status != CHRONOBENT_OK && status != CHRONOBENT_END) throw std::runtime_error("render failed");
        output += got;
        std::fill(out+got*2,out+count*2,0);
        return got;
    }
};
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
        auto current = std::make_unique<Epoch>(audio_,sample_rate_,0,unpack(settings_.load()));
        std::unique_ptr<Epoch> next;
        std::array<float,512> old_audio{},new_audio{};
        std::size_t fade = 0;
        std::uint64_t position = 0;
        double source_position = 0;
        while (!stopping_.load()) {
            const auto free = capacity-(position-read_.load(std::memory_order_acquire));
            if (!free) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }
            if (!current->remaining() && !next) break;
            const auto desired = unpack(settings_.load());
            if (!next && (desired.pitch != current->ratios.pitch || desired.tempo != current->ratios.tempo ||
                          desired.formants != current->ratios.formants)) {
                next = std::make_unique<Epoch>(audio_,sample_rate_,source_position,desired);
                fade = 0;
            }
            auto &trajectory = next ? *next : *current;
            const auto count = std::size_t(std::min({UINT64_C(256),trajectory.remaining(),free}));
            if (!count) break;
            current->render(old_audio.data(),count);
            if (next) next->render(new_audio.data(),count);
            for (std::size_t i=0; i<count; ++i) {
                const float blend = next ? std::min(1.0f,float(fade+i+1)/1024) : 0;
                const auto index = std::size_t(position+i) & (capacity-1);
                for (std::size_t channel=0; channel<2; ++channel)
                    queue_[2*index+channel]=old_audio[2*i+channel]*(1-blend)+new_audio[2*i+channel]*blend;
                source_queue_[index]=std::min(double(frames()),source_position+double(i+1)*trajectory.ratios.tempo);
            }
            source_position=std::min(double(frames()),source_position+double(count)*trajectory.ratios.tempo);
            if (trajectory.remaining()==0) source_queue_[std::size_t(position+count-1)&(capacity-1)]=double(frames());
            if (next && (fade+=count)>=1024) current=std::move(next);
            position+=count;
            written_.store(position,std::memory_order_release);
            if (source_position>=double(frames())) break;
        }
    } catch (...) { error_.store(1); }
    finished_.store(true);
}
}
