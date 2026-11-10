// Local whisper.cpp transcription + word aggregation + cut building.
#pragma once
#include <string>
#include <vector>
#include <functional>
#include "Profanity.h"

namespace mc {

struct Word { std::string text; double start = 0, end = 0; };
struct Cut  { double start = 0, end = 0; std::string word; };

struct AnalysisOptions {
    std::string model_path;
    std::string language = "ru";   // "auto" for detection
    int threads = 4;
    std::function<void(int)> progress;  // 0..100
    std::function<bool()> abort;        // true -> cancel
};

struct AnalysisResult {
    bool ok = false;
    std::string error;
    std::vector<Word> words;
};

// pcm: 16 kHz mono float [-1,1].
AnalysisResult transcribe_local(const float* pcm, size_t n, const AnalysisOptions& opt);

// Match words against dictionary, pad + merge into cut intervals (seconds).
std::vector<Cut> build_cuts(const std::vector<Word>& words, const Dictionary& dict,
                            double pad_sec, double merge_sec);

} // namespace mc