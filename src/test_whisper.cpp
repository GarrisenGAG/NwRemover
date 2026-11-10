// Headless smoke test: whisper.cpp local transcription with word timestamps.
#include "whisper.h"
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <vector>
#include <string>

static std::vector<float> read_wav16(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return {}; }
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() < 44) return {};
    // find "data" chunk
    size_t pos = 12;
    while (pos + 8 <= raw.size()) {
        uint32_t sz = *(uint32_t*)(raw.data() + pos + 4);
        if (raw[pos] == 'd' && raw[pos+1] == 'a' && raw[pos+2] == 't' && raw[pos+3] == 'a') {
            size_t start = pos + 8;
            size_t n = (raw.size() - start) / 2;
            if (n > sz / 2) n = sz / 2;
            const int16_t* s = (const int16_t*)(raw.data() + start);
            std::vector<float> out(n);
            for (size_t i = 0; i < n; i++) out[i] = s[i] / 32768.0f;
            return out;
        }
        pos += 8 + sz + (sz & 1);
    }
    return {};
}

int main(int argc, char** argv) {
    const char* model = argc > 1 ? argv[1] : "D:/AEplugin/models/ggml-tiny.bin";
    const char* wav   = argc > 2 ? argv[2] : "D:/AEplugin/third_party/whisper.cpp/samples/jfk.wav";
    const char* lang  = argc > 3 ? argv[3] : "en";

    std::vector<float> pcm = read_wav16(wav);
    if (pcm.empty()) return 1;
    std::printf("samples=%zu (%.1f s)\n", pcm.size(), pcm.size() / 16000.0);

    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    cp.flash_attn = false;
    whisper_context* ctx = whisper_init_from_file_with_params(model, cp);
    if (!ctx) { std::printf("model load FAILED\n"); return 2; }

    whisper_full_params w = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    w.n_threads = 4;
    w.language = lang;
    w.translate = false;
    w.token_timestamps = true;
    w.split_on_word = true;
    w.print_progress = false;
    w.print_realtime = false;
    w.print_special = false;
    w.print_timestamps = false;

    int rc = whisper_full(ctx, w, pcm.data(), (int)pcm.size());
    std::printf("whisper_full rc=%d\n", rc);
    if (rc != 0) { whisper_free(ctx); return 3; }

    int nseg = whisper_full_n_segments(ctx);
    for (int s = 0; s < nseg; s++) {
        int nt = whisper_full_n_tokens(ctx, s);
        for (int t = 0; t < nt; t++) {
            const char* txt = whisper_full_get_token_text(ctx, s, t);
            whisper_token_data td = whisper_full_get_token_data(ctx, s, t);
            if (txt && txt[0] && txt[0] != '[') {
                std::printf("%6.2f-%6.2f %s\n", td.t0 / 100.0, td.t1 / 100.0, txt);
            }
        }
    }
    whisper_free(ctx);
    return 0;
}