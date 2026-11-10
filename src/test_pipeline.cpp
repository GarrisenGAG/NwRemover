// Headless end-to-end pipeline test + dictionary unit checks.
#include "Util.h"
#include "Analysis.h"
#include "Profanity.h"
#include "Dsp.h"
#include <cstdio>
#include <cstring>
#include <string>

static int dict_test() {
    mc::Dictionary d = mc::default_dictionary();
    const char* mat[] = {"блядь","блять","хуйня","нахуй","пиздец","пиздить","ебать","ебло",
        "заебись","уебок","ёбаный","выебывается","дрочить","говно","залупа","пидорас",
        "гандон","мудак","херня","похер","жопа","срака","сука","сучка","fuck",
        "motherfucker","bullshit","bitch","ебучий","съебаться"};
    const char* clean[] = {"небо","себя","тебя","ребёнок","хлеб","требовал","парикмахерская",
        "алгебра","учёба","мудрый","сукно","мандарин","херес","чебурек","дебет","гребень",
        "лебедь","пребывать","щебет","волшебник","сучок","мандолина","cockpit","dickens",
        "hello","country","сегодня","работает"};
    int fail = 0;
    for (auto w : mat) {
        std::string n = mc::normalize_word(mc::trim_punct(w));
        if (d.match(n).empty()) { std::printf("MISS (should flag): %s -> %s\n", w, n.c_str()); fail++; }
    }
    for (auto w : clean) {
        std::string n = mc::normalize_word(mc::trim_punct(w));
        std::string hit = d.match(n);
        if (!hit.empty()) { std::printf("FALSE POSITIVE: %s -> %s (hit %s)\n", w, n.c_str(), hit.c_str()); fail++; }
    }
    std::printf("dict_test: %zu mat, %zu clean, failures=%d\n",
                sizeof(mat)/sizeof(mat[0]), sizeof(clean)/sizeof(clean[0]), fail);
    // severity tiers: sev1 keeps only core mat, drops medium/mild
    mc::Dictionary d1 = mc::default_dictionary(); d1.max_severity = 1;
    if (d1.match(mc::normalize_word("хуйня")).empty()) { std::printf("SEV1 miss core mat\n"); fail++; }
    if (!d1.match(mc::normalize_word("shit")).empty()) { std::printf("SEV1 leaked medium (shit)\n"); fail++; }
    if (!d1.match(mc::normalize_word("жопа")).empty()) { std::printf("SEV1 leaked mild (zhopa)\n"); fail++; }
    mc::Dictionary d2 = mc::default_dictionary(); d2.max_severity = 2;
    if (d2.match(mc::normalize_word("shit")).empty()) { std::printf("SEV2 miss medium (shit)\n"); fail++; }
    if (!d2.match(mc::normalize_word("жопа")).empty()) { std::printf("SEV2 leaked mild (zhopa)\n"); fail++; }
    mc::Dictionary d3 = mc::default_dictionary(); d3.max_severity = 3;
    if (d3.match(mc::normalize_word("жопа")).empty()) { std::printf("SEV3 miss mild (zhopa)\n"); fail++; }
    std::printf("severity tiers: failures=%d\n", fail);
    return fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "dict") == 0) return dict_test();
    if (argc < 5) {
        std::printf("usage: test_pipeline dict | <in.wav> <model.bin> <lang> <out.wav> [mode]\n");
        return 1;
    }
    const char* in = argv[1]; const char* model = argv[2];
    const char* lang = argv[3]; const char* out = argv[4];
    int mode = argc > 5 ? std::atoi(argv[5]) : mc::MODE_BEEP;

    mc::WavData w = mc::read_wav(in);
    if (!w.ok) { std::printf("wav read failed\n"); return 2; }
    std::vector<float> mono = mc::to_mono16k(w);
    std::printf("input: %d Hz, %d ch, %.2f s -> mono16k %zu samples\n",
                w.rate, w.channels, w.samples.size() / (double)w.rate / w.channels, mono.size());

    mc::AnalysisOptions opt;
    opt.model_path = model; opt.language = lang; opt.threads = 4;
    opt.progress = [](int p){ if (p % 20 == 0) std::printf("progress %d%%\n", p); };
    mc::AnalysisResult r = mc::transcribe_local(mono.data(), mono.size(), opt);
    if (!r.ok) { std::printf("ERROR: %s\n", r.error.c_str()); return 3; }

    std::printf("--- words (%zu) ---\n", r.words.size());
    for (auto& wd : r.words)
        std::printf("%6.2f-%6.2f %s\n", wd.start, wd.end, wd.text.c_str());

    mc::Dictionary dict = mc::default_dictionary();
    auto cuts = mc::build_cuts(r.words, dict, 0.08, 0.15);
    std::printf("--- cuts (%zu) ---\n", cuts.size());
    for (auto& c : cuts)
        std::printf("%6.2f-%6.2f [%s]\n", c.start, c.end, c.word.c_str());

    // Apply replacement to mono16k and write output.
    mc::DspParams p; p.mode = mode;
    std::vector<float> result = mono;
    std::vector<float> csound;
    if (mode == mc::MODE_CUSTOM) {
        const char* sp = argc > 6 ? argv[6] : "";
        mc::WavData sw = mc::read_wav(sp);
        csound = mc::resample_linear(mc::mixdown_mono(sw), sw.rate, 16000);
        std::printf("custom sound: %s -> %zu samples @16k\n", sp, csound.size());
    }
    for (auto& c : cuts) {
        int64_t a = (int64_t)(c.start * 16000);
        int64_t b = (int64_t)(c.end * 16000);
        if (a < 0) a = 0;
        if (b > (int64_t)result.size()) b = result.size();
        if (b <= a) continue;
        std::vector<float> tmp((size_t)(b - a));
        if (mode == mc::MODE_CUSTOM)
            mc::process_custom(csound.data(), (int64_t)csound.size(), tmp.data(), b - a, 1, 16000.0, p.beep_level, p.fade_ms);
        else
            mc::process_interval(result.data() + a, tmp.data(), b - a, 1, 16000.0, p);
        std::memcpy(result.data() + a, tmp.data(), tmp.size() * sizeof(float));
    }
    if (!mc::write_wav16(out, result, 16000, 1)) { std::printf("wav write failed\n"); return 4; }
    std::printf("wrote %s\n", out);
    return 0;
}