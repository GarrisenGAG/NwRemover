// Groq cloud transcription via WinHTTP (whisper-large-v3, word timestamps).
#pragma once
#include "Analysis.h"
#include <string>
#include <vector>

namespace mc {

struct GroqOptions {
    std::string api_key;                 // single key (legacy)
    std::vector<std::string> api_keys;   // pool, rotated round-robin
    std::string model = "whisper-large-v3";
    std::string language = "ru"; // "auto" -> omit
};

// mono16k: 16 kHz mono float. Uploads as WAV, returns words with timestamps.
AnalysisResult transcribe_groq(const std::vector<float>& mono16k, const GroqOptions& opt);

} // namespace mc