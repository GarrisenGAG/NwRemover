#include "Analysis.h"
#include "whisper.h"
#include <algorithm>
#include <cstring>

namespace mc {

static void progress_cb(whisper_context*, whisper_state*, int progress, void* ud) {
    auto* fn = static_cast<std::function<void(int)>*>(ud);
    if (fn && *fn) (*fn)(progress);
}
static bool abort_cb(void* ud) {
    auto* fn = static_cast<std::function<bool()>*>(ud);
    return fn && *fn && (*fn)();
}

static std::vector<Word> extract_words(whisper_context* ctx) {
    std::vector<Word> out;
    int nseg = whisper_full_n_segments(ctx);
    for (int s = 0; s < nseg; s++) {
        double seg_t0 = whisper_full_get_segment_t0(ctx, s) / 100.0;
        double seg_t1 = whisper_full_get_segment_t1(ctx, s) / 100.0;
        std::vector<Word> seg_words;
        std::string cur;
        double cur_start = seg_t0;
        int nt = whisper_full_n_tokens(ctx, s);
        for (int t = 0; t < nt; t++) {
            const char* raw = whisper_full_get_token_text(ctx, s, t);
            if (!raw || !raw[0] || raw[0] == '[') continue;
            std::string txt = raw;
            whisper_token_data td = whisper_full_get_token_data(ctx, s, t);
            bool leading_space = (txt[0] == ' ');
            if (leading_space) txt = txt.substr(1);
            if (txt.empty()) continue;
            if (leading_space && !cur.empty()) {
                seg_words.push_back({cur, cur_start, cur_start});
                cur.clear();
            }
            if (cur.empty()) {
                cur_start = td.t0 > 0 ? td.t0 / 100.0 : seg_t0;
            }
            cur += txt;
        }
        if (!cur.empty()) seg_words.push_back({cur, cur_start, cur_start});
        // End of each word = start of next; last word = segment end.
        for (size_t i = 0; i < seg_words.size(); i++) {
            double e = (i + 1 < seg_words.size()) ? seg_words[i + 1].start : seg_t1;
            if (e <= seg_words[i].start) e = seg_words[i].start + 0.08;
            seg_words[i].end = e;
            out.push_back(seg_words[i]);
        }
    }
    return out;
}

AnalysisResult transcribe_local(const float* pcm, size_t n, const AnalysisOptions& opt) {
    AnalysisResult res;
    if (!pcm || n == 0) { res.error = "Empty audio"; return res; }

    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    cp.flash_attn = false;
    whisper_context* ctx = whisper_init_from_file_with_params(opt.model_path.c_str(), cp);
    if (!ctx) { res.error = "Cannot load model: " + opt.model_path; return res; }

    whisper_full_params w = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    w.n_threads = std::max(1, std::min(16, opt.threads));
    w.language = opt.language.c_str();
    w.translate = false;
    w.token_timestamps = true;
    w.split_on_word = true;
    w.no_context = true;
    w.print_progress = false;
    w.print_realtime = false;
    w.print_special = false;
    w.print_timestamps = false;
    if (opt.progress) {
        w.progress_callback = progress_cb;
        w.progress_callback_user_data = const_cast<std::function<void(int)>*>(&opt.progress);
    }
    if (opt.abort) {
        w.abort_callback = abort_cb;
        w.abort_callback_user_data = const_cast<std::function<bool()>*>(&opt.abort);
    }

    int rc = whisper_full(ctx, w, pcm, (int)n);
    if (rc != 0) {
        res.error = (rc < 0) ? "Cancelled" : "whisper_full failed: " + std::to_string(rc);
        whisper_free(ctx);
        return res;
    }
    res.words = extract_words(ctx);
    res.ok = true;
    whisper_free(ctx);
    return res;
}

std::vector<Cut> build_cuts(const std::vector<Word>& words, const Dictionary& dict,
                            double pad_sec, double merge_sec) {
    std::vector<Cut> cuts;
    for (const auto& w : words) {
        std::string norm = normalize_word(trim_punct(w.text));
        if (norm.empty()) continue;
        std::string hit = dict.match(norm);
        if (hit.empty()) continue;
        Cut c;
        c.start = std::max(0.0, w.start - pad_sec);
        c.end = w.end + pad_sec;
        c.word = w.text;
        cuts.push_back(c);
    }
    std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b){ return a.start < b.start; });
    std::vector<Cut> merged;
    for (auto& c : cuts) {
        if (!merged.empty() && c.start <= merged.back().end + merge_sec) {
            merged.back().end = std::max(merged.back().end, c.end);
            merged.back().word += "; " + c.word;
        } else {
            merged.push_back(c);
        }
    }
    return merged;
}

} // namespace mc