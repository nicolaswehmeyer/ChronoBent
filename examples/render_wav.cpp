// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Nicolas Wehmeyer
// A dependency-free offline example. File I/O is deliberately outside the DSP.
#include "chronobent/chronobent.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::uint16_t u16(const unsigned char *p) { return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8); }
std::uint32_t u32(const unsigned char *p) { return std::uint32_t(u16(p)) | (std::uint32_t(u16(p + 2)) << 16); }
void write(std::FILE *out, const void *data, std::size_t size) {
    if (std::fwrite(data, 1, size, out) != size) throw std::runtime_error("Output write failed");
}
void put16(std::FILE *out, std::uint16_t x) {
    const char b[2] = {static_cast<char>(x & 255), static_cast<char>(x >> 8)};
    write(out, b, 2);
}
void put32(std::FILE *out, std::uint32_t x) { put16(out, std::uint16_t(x & 65535)); put16(out, std::uint16_t(x >> 16)); }
struct Wav {
    std::uint32_t rate = 0, channels = 0;
    std::vector<float> audio;
    static int read(void *context, std::uint64_t start, std::size_t count, float *out) {
        auto &wav = *static_cast<Wav *>(context);
        std::copy_n(wav.audio.data() + start * wav.channels, count * wav.channels, out);
        return 1;
    }
};
Wav load(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("Cannot open input WAV");
    const auto length = in.tellg();
    if (length < 12 || length > 1024*1024*1024) throw std::runtime_error("Example accepts WAV files between 12 bytes and 1 GiB");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), length)) throw std::runtime_error("Incomplete input read");
    const auto *b = bytes.data();
    if (std::memcmp(b, "RIFF", 4) || std::memcmp(b+8, "WAVE", 4)) throw std::runtime_error("Input must be little-endian RIFF/WAVE");
    const std::uint64_t end = std::uint64_t(u32(b+4)) + 8;
    if (end != bytes.size()) throw std::runtime_error("RIFF length does not match file size");
    Wav wav;
    std::uint16_t format = 0, bits = 0, align = 0;
    const unsigned char *data = nullptr;
    std::size_t data_size = 0;
    bool have_format = false, have_data = false;
    for (std::size_t offset = 12; offset < end;) {
        if (end - offset < 8) throw std::runtime_error("Truncated chunk header");
        const std::size_t size = u32(b+offset+4), start = offset+8;
        if (size > end-start || (size & 1) > end-start-size) throw std::runtime_error("Chunk exceeds RIFF bounds");
        if (!std::memcmp(b+offset, "fmt ", 4)) {
            if (have_format || size < 16) throw std::runtime_error("Invalid or repeated format chunk");
            have_format = true;
            format = u16(b+start); wav.channels = u16(b+start+2); wav.rate = u32(b+start+4);
            align = u16(b+start+12); bits = u16(b+start+14);
            if (wav.channels < 1 || wav.channels > 8 || wav.rate < 8000 || wav.rate > 192000 ||
                !((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)) ||
                align != wav.channels * (bits/8) || u32(b+start+8) != wav.rate * align)
                throw std::runtime_error("Use PCM16/24/32 or float32 WAV, 1..8 channels, 8..192 kHz");
        } else if (!std::memcmp(b+offset, "data", 4)) {
            if (have_data) throw std::runtime_error("Repeated data chunk");
            have_data = true; data = b+start; data_size = size;
        }
        offset = start + size + (size & 1);
    }
    if (!have_format || !have_data || data_size % align) throw std::runtime_error("Missing or incomplete audio frames");
    const std::size_t samples = data_size / (bits/8);
    wav.audio.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const auto *p = data + i*(bits/8);
        if (format == 3) {
            const std::uint32_t word = u32(p);
            std::memcpy(&wav.audio[i], &word, 4);
        } else {
            std::int64_t value = bits == 16 ? u16(p) : bits == 24 ? (std::uint32_t(u16(p)) | (std::uint32_t(p[2])<<16)) : u32(p);
            if (value & (INT64_C(1) << (bits-1))) value -= INT64_C(1) << bits;
            wav.audio[i] = static_cast<float>(double(value) / double(INT64_C(1) << (bits-1)));
        }
        if (!std::isfinite(wav.audio[i]) || std::abs(wav.audio[i]) > 64)
            throw std::runtime_error("Nonfinite or excessive input sample");
    }
    return wav;
}
double number(const char *arg) {
    std::size_t parsed = 0;
    const std::string text(arg);
    const double value = std::stod(text, &parsed);
    if (parsed != text.size() || !std::isfinite(value)) throw std::runtime_error("Invalid numeric argument");
    return value;
}
}

int main(int argc, char **argv) {
    if (argc < 5 || argc > 9) {
        std::fprintf(stderr, "Usage: chronobent-render input.wav output.wav tempo pitch-ratio [tonal|transients|mixed] [shift|preserve|formant-ratio] [envelope-ms] [compact|balanced|detailed]\n"
            "Example: chronobent-render input.wav output.wav 1.08 1.059463\n"
            "Output: float32 WAV; existing output paths are refused.\n");
        return 2;
    }
    try {
        const std::filesystem::path destination(argv[2]);
        if (std::filesystem::exists(std::filesystem::symlink_status(destination)))
            throw std::runtime_error("Output already exists");
        const double tempo = number(argv[3]), pitch = number(argv[4]);
        auto controls = chronobent_default_controls();
        controls.tempo = tempo; controls.pitch = pitch;
        if (argc >= 6) {
            const std::string mode(argv[5]);
            if (mode != "tonal" && mode != "transients" && mode != "mixed") throw std::runtime_error("Unknown transient mode");
            controls.transients = mode == "tonal" ? 0u : mode == "mixed" ? 2u : 1u;
        }
        if (argc >= 7) {
            const std::string mode(argv[6]);
            controls.formant_scale = mode == "shift" ? 0 : mode == "preserve" ? 1 : number(argv[6]);
            if (mode != "shift" && !(controls.formant_scale >= .5 && controls.formant_scale <= 2))
                throw std::runtime_error("Formant ratio must be .5..2, or shift/preserve");
        }
        if (argc >= 8) controls.envelope_ms = number(argv[7]);
        chronobent_profile profile = CHRONOBENT_PROFILE_BALANCED;
        if (argc >= 9) {
            const std::string name(argv[8]);
            if (name == "compact") profile = CHRONOBENT_PROFILE_COMPACT;
            else if (name == "detailed") profile = CHRONOBENT_PROFILE_DETAILED;
            else if (name != "balanced") throw std::runtime_error("Unknown analysis profile");
        }
        Wav input = load(argv[1]);
        chronobent_config config{};
        if (chronobent_config_for_profile(double(input.rate),input.channels,profile,&config) != CHRONOBENT_OK)
            throw std::runtime_error("Invalid source configuration");
        chronobent *raw = nullptr;
        if (chronobent_create(&config, &raw) != CHRONOBENT_OK) throw std::runtime_error("Cannot create renderer");
        std::unique_ptr<chronobent, decltype(&chronobent_destroy)> engine(raw, chronobent_destroy);
        if (chronobent_reset_controls(engine.get(), input.audio.size()/input.channels, &controls) != CHRONOBENT_OK)
            throw std::runtime_error("Controls out of range: tempo .25..4, pitch .5..2, formants .5..2, envelope 1..4 ms");
        const auto frames = chronobent_output_frames(engine.get()), bytes = frames*input.channels*4;
        if (bytes > std::numeric_limits<std::uint32_t>::max()-48) throw std::runtime_error("Output exceeds RIFF capacity");
#if defined(_WIN32)
        std::FILE *out = _wfopen(destination.c_str(), L"wbx");
#else
        std::FILE *out = std::fopen(destination.c_str(), "wbx");
#endif
        if (!out) throw std::runtime_error("Cannot exclusively create output WAV");
        std::unique_ptr<std::FILE, decltype(&std::fclose)> file(out, std::fclose);
        write(out, "RIFF", 4); put32(out, static_cast<std::uint32_t>(bytes+48)); write(out, "WAVEfmt ", 8);
        put32(out, 16); put16(out, 3); put16(out, static_cast<std::uint16_t>(input.channels));
        put32(out, input.rate); put32(out, input.rate*input.channels*4);
        put16(out, static_cast<std::uint16_t>(input.channels*4)); put16(out, 32);
        write(out, "fact", 4); put32(out, 4); put32(out, static_cast<std::uint32_t>(frames));
        write(out, "data", 4); put32(out, static_cast<std::uint32_t>(bytes));
        std::vector<float> buffer(4096*input.channels);
        for (;;) {
            std::size_t got = 0;
            const auto status = chronobent_render(engine.get(), Wav::read, &input, buffer.data(), 4096, &got);
            if (status != CHRONOBENT_OK && status != CHRONOBENT_END) throw std::runtime_error("Rendering failed");
            for (std::size_t i = 0; i < got*input.channels; ++i) {
                std::uint32_t word;
                std::memcpy(&word, &buffer[i], 4);
                put32(out, word);
            }
            if (status == CHRONOBENT_END) break;
        }
        if (std::fclose(file.release()) != 0) throw std::runtime_error("Output close failed");
        std::printf("Rendered %llu frames at %u Hz, %u channels\n",
            static_cast<unsigned long long>(frames), input.rate, input.channels);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
