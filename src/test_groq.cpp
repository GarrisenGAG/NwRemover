// Live Groq backend test: transcribe via API using keys from config.
#include "Util.h"
#include "Analysis.h"
#include "Profanity.h"
#include "Config.h"
#include "Groq.h"
#include <cstdio>

int main(int argc, char** argv) {
    const char* wav = argc > 1 ? argv[1] : "D:/AEplugin/build/test_mat.wav";
    const char* lang = argc > 2 ? argv[2] : "ru";
    const char* model = argc > 3 ? argv[3] : "whisper-large-v3";

    mc::Config cfg = mc::load_config("D:/AEplugin");
    std::printf("config keys=%zu single=%s\n", cfg.groq_api_keys.size(),
                cfg.groq_api_key.empty() ? "no" : "yes");
    if (cfg.groq_api_keys.empty() && cfg.groq_api_key.empty()) {
        std::printf("NO KEYS\n"); return 2;
    }

    mc::WavData w = mc::read_wav(wav);
    if (!w.ok) { std::printf("wav read failed\n"); return 3; }
    std::vector<float> mono = mc::to_mono16k(w);
    std::printf("audio %.2f s, model=%s lang=%s\n", mono.size() / 16000.0, model, lang);

    mc::GroqOptions go;
    go.api_keys = cfg.groq_api_keys;
    go.api_key = cfg.groq_api_key;
    go.proxy = cfg.proxy;
    go.model = model;
    go.language = lang;
    mc::AnalysisResult r = mc::transcribe_groq(mono, go);
    if (!r.ok) { std::printf("GROQ ERROR: %s\n", r.error.c_str()); return 4; }

    std::printf("--- words (%zu) ---\n", r.words.size());
    for (auto& wd : r.words) std::printf("%6.2f-%6.2f %s\n", wd.start, wd.end, wd.text.c_str());

    mc::Dictionary d = mc::default_dictionary();
    d.max_severity = 3;
    auto cuts = mc::build_cuts(r.words, d, 0.08, 0.15);
    std::printf("--- cuts (%zu) ---\n", cuts.size());
    for (auto& c : cuts) std::printf("%6.2f-%6.2f [%s]\n", c.start, c.end, c.word.c_str());
    std::printf("GROQ OK\n");
    return 0;
}