// WAV I/O + resampling helpers (PCM16 / float32 input).
#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace mc {

struct WavData {
    std::vector<float> samples; // interleaved
    int rate = 0;
    int channels = 0;
    bool ok = false;
};

WavData read_wav(const std::string& path);
bool write_wav16(const std::string& path, const std::vector<float>& samples, int rate, int channels);

// Mixdown to mono + linear resample to 16 kHz.
std::vector<float> to_mono16k(const WavData& w);

// Mixdown to mono at native rate.
std::vector<float> mixdown_mono(const WavData& w);

// Linear resample mono between arbitrary rates.
std::vector<float> resample_linear(const std::vector<float>& in, int from_rate, int to_rate);

// In-memory 16-bit mono WAV (for Groq upload).
std::vector<uint8_t> wav16_bytes(const std::vector<float>& mono16k);

} // namespace mc