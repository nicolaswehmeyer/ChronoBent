// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_AUDITION_PLAYER_HPP
#define CHRONOBENT_AUDITION_PLAYER_HPP
#include "chronobent/chronobent.h"
#include <array>
#include <memory>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace audition {
struct Settings {
    double semitones = 0, tempo = 1;
    bool master_tempo = true, bypass = false, formants = false;
    double formant_semitones = 0;
    unsigned transients = 1; // 0 smooth, 1 crisp, 2 mixed.
    double envelope_ms = 2;
};
// Immutable decoded stereo input. One producer owns all DSP. One audio consumer
// reads a bounded SPSC queue. UI control never touches the DSP instances.
class Player {
public:
    Player(std::vector<float> stereo, double sample_rate, Settings initial = {},
        chronobent_profile profile = CHRONOBENT_PROFILE_BALANCED);
    Player(std::shared_ptr<const std::vector<float>> stereo, double sample_rate, Settings initial = {},
        chronobent_profile profile = CHRONOBENT_PROFILE_BALANCED);
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;
    void request(double semitones, bool bypass, bool formants) noexcept;
    void request(Settings settings) noexcept;
    void pause(bool value) noexcept { paused_.store(value); }
    // One control thread. Seeks coalesce; the worker discards stale queued audio.
    // Completion is observable even while paused or at EOF. No source copying.
    bool seek(std::uint64_t source_frame) noexcept;
    bool seeking() const noexcept { return seek_request_.load() != seek_done_.load(); }
    // Noninterleaved stereo output; no allocation, waits, DSP or system calls.
    // One bounded atomic ownership attempt protects the seek queue handoff.
    // A busy handoff emits a short fade to silence, then new audio fades in.
    void pull(float *left, float *right, std::size_t frames) noexcept;
    std::uint64_t played() const noexcept { return played_frames_.load(); }
    std::uint64_t frames() const noexcept { return audio_->size() / 2; }
    double source_position() const noexcept { return source_position_.load(); }
    std::uint64_t queued() const noexcept {
        const auto consumed = read_.load();
        return written_.load() - consumed;
    }
    unsigned underruns() const noexcept { return underruns_.load(); }
    int error() const noexcept { return error_.load(); }
    bool ended() const noexcept { return !seeking() && finished_.load() && queued() == 0; }
    double sample_rate() const noexcept { return sample_rate_; }
private:
    void run() noexcept;
    static constexpr std::size_t capacity = 4096;
    std::shared_ptr<const std::vector<float>> audio_;
    chronobent_profile profile_;
    double sample_rate_;
    std::array<float, capacity * 2> queue_{};
    std::array<double, capacity> source_queue_{};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    alignas(64) std::atomic<std::uint64_t> written_{0};
    std::atomic<bool> stopping_{false}, paused_{true}, finished_{false};
    std::atomic<std::uint64_t> settings_{0};
    std::atomic<double> source_position_{0};
    std::atomic<std::uint64_t> seek_request_{0}, seek_done_{0};
    std::atomic_flag queue_handoff_ = ATOMIC_FLAG_INIT;
    std::uint32_t seek_serial_ = 0; // Control thread only.
    std::array<float, 2> last_sample_{}; // Audio consumer only.
    std::uint64_t audible_seek_ = 0; // Audio consumer only.
    unsigned fade_in_ = 0; // Audio consumer only.
    std::atomic<unsigned> underruns_{0};
    std::atomic<std::uint64_t> played_frames_{0};
    std::atomic<int> error_{0};
    std::thread worker_;
};
}
#endif
