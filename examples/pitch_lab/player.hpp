// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
#ifndef CHRONOBENT_AUDITION_PLAYER_HPP
#define CHRONOBENT_AUDITION_PLAYER_HPP
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace audition {
struct Settings {
    double semitones = 0, tempo = 1;
    bool master_tempo = true, bypass = false, formants = false;
};
// Immutable decoded stereo input. One producer owns all DSP. One audio consumer
// reads a bounded SPSC queue. UI control never touches the DSP instances.
class Player {
public:
    Player(std::vector<float> stereo, double sample_rate, Settings initial = {});
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;
    void request(double semitones, bool bypass, bool formants) noexcept;
    void request(Settings settings) noexcept;
    void pause(bool value) noexcept { paused_.store(value); }
    // Noninterleaved stereo output; no allocation, locks, DSP or system calls.
    void pull(float *left, float *right, std::size_t frames) noexcept;
    std::uint64_t played() const noexcept { return read_.load(); }
    std::uint64_t frames() const noexcept { return audio_.size() / 2; }
    double source_position() const noexcept { return source_position_.load(); }
    std::uint64_t queued() const noexcept {
        const auto consumed = read_.load();
        return written_.load() - consumed;
    }
    unsigned underruns() const noexcept { return underruns_.load(); }
    int error() const noexcept { return error_.load(); }
    bool ended() const noexcept { return finished_.load() && queued() == 0; }
    double sample_rate() const noexcept { return sample_rate_; }
private:
    void run() noexcept;
    static constexpr std::size_t capacity = 4096;
    std::vector<float> audio_;
    double sample_rate_;
    std::array<float, capacity * 2> queue_{};
    std::array<double, capacity> source_queue_{};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    alignas(64) std::atomic<std::uint64_t> written_{0};
    std::atomic<bool> stopping_{false}, paused_{true}, finished_{false};
    std::atomic<std::uint64_t> settings_{0};
    std::atomic<double> source_position_{0};
    std::atomic<unsigned> underruns_{0};
    std::atomic<int> error_{0};
    std::thread worker_;
};
}
#endif
