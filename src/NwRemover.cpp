// NwRemover - After Effects audio effect: auto-censor profanity via local
// whisper.cpp or Groq API. Platform presets, custom replacement sound, manual mute.
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>

#include "AEConfig.h"
#include "A.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectVers.h"
#include "AE_Macros.h"
#include "Param_Utils.h"

#include "Analysis.h"
#include "Profanity.h"
#include "Dsp.h"
#include "Config.h"
#include "Groq.h"
#include "Util.h"

#define NC_NAME        "NwRemover"
#define NC_MATCH_NAME  "GH NwRemover"
#define NC_CATEGORY    "Audio"
#define NC_DESCRIPTION "Auto-censor profanity: local whisper.cpp or Groq API. Platform presets."
#define NC_MAX_CUTS    4096

enum {
    P_INPUT = 0,
    P_ENABLE,
    P_TOPIC_ANALYSIS,
    P_BACKEND,
    P_MODEL,
    P_GROQ_MODEL,
    P_LANGUAGE,
    P_THREADS,
    P_AUTO,
    P_ANALYZE,
    P_CLEAR,
    P_REPORT,
    P_END_ANALYSIS,
    P_TOPIC_DETECT,
    P_PLATFORM,
    P_STRICTNESS,
    P_PAD_MS,
    P_MERGE_MS,
    P_END_DETECT,
    P_TOPIC_REPLACE,
    P_MODE,
    P_BEEP_FREQ,
    P_BEEP_LEVEL,
    P_BEEP_WAVE,
    P_BEEP_SWEEP,
    P_PITCH_SEMI,
    P_FADE_MS,
    P_END_REPLACE,
    P_TOPIC_MANUAL,
    P_MANUAL_ENABLE,
    P_MANUAL_TIME,
    P_MANUAL_LEN,
    P_END_MANUAL,
    P_NUM_PARAMS
};

struct CutPod { double start; double end; char word[96]; };
struct SeqData {
    uint32_t magic;
    uint32_t version;
    uint32_t pending;
    uint32_t analyzed;
    uint32_t num_cuts;
    double   audio_rate;
    double   audio_dur;
    CutPod   cuts[NC_MAX_CUTS];
};
static const uint32_t NC_MAGIC = 0x3143434D;

static std::string g_plugin_dir;
static std::once_flag g_dir_once;

static void log_line(const std::string& s) {
    std::ofstream f("D:/AEplugin/NwRemover.log", std::ios::app);
    if (f) { f << s << "\n"; }
}

static std::string plugin_dir() {
    std::call_once(g_dir_once, []{
        HMODULE hm = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&plugin_dir, &hm);
        wchar_t buf[MAX_PATH] = {0};
        GetModuleFileNameW(hm, buf, MAX_PATH);
        std::wstring w(buf);
        size_t slash = w.find_last_of(L"/\\");
        if (slash != std::wstring::npos) w = w.substr(0, slash);
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        g_plugin_dir = s;
    });
    return g_plugin_dir;
}

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static const char* kLanguages[] = {"auto","ru","en","uk","be","de","fr","es","it","pl","tr","ja","ko","zh"};
static const char* kGroqModels[] = {"", "whisper-large-v3", "whisper-large-v3-turbo"};

// Platform preset -> max dictionary severity (1 severe, 2 medium, 3 mild).
static int platform_severity(int platform_popup, int strictness_slider) {
    switch (platform_popup) {
    case 2: return 2;               // YouTube
    case 3: return 3;               // TikTok
    case 4: return 3;               // Reels
    case 5: return 1;               // Twitch
    default: return std::max(1, std::min(3, strictness_slider)); // Custom
    }
}

// ---- custom replacement sound cache ----
static std::mutex g_snd_mtx;
static std::string g_snd_path;
static int g_snd_rate = 0;
static std::shared_ptr<const std::vector<float>> g_snd;

static std::shared_ptr<const std::vector<float>> get_custom_sound(const std::string& path, int rate) {
    std::lock_guard<std::mutex> lk(g_snd_mtx);
    if (g_snd && g_snd_path == path && g_snd_rate == rate) return g_snd;
    g_snd_path = path; g_snd_rate = rate;
    if (path.empty()) { g_snd.reset(); return nullptr; }
    mc::WavData w = mc::read_wav(path);
    if (!w.ok) { g_snd.reset(); return nullptr; }
    auto mono = mc::mixdown_mono(w);
    auto rs = mc::resample_linear(mono, w.rate, rate);
    if (rs.empty()) { g_snd.reset(); return nullptr; }
    g_snd = std::make_shared<const std::vector<float>>(std::move(rs));
    return g_snd;
}

// ---- sequence data helpers ----
static SeqData* seq_lock(PF_InData* in_data) {
    if (!in_data->sequence_data) return nullptr;
    SeqData* s = (SeqData*)PF_LOCK_HANDLE(in_data->sequence_data);
    if (s && s->magic != NC_MAGIC) { PF_UNLOCK_HANDLE(in_data->sequence_data); return nullptr; }
    return s;
}
static void seq_unlock(PF_InData* in_data) {
    if (in_data->sequence_data) PF_UNLOCK_HANDLE(in_data->sequence_data);
}

static void store_cuts(PF_InData* in_data, const std::vector<mc::Cut>& cuts, double rate, double dur) {
    SeqData* s = seq_lock(in_data);
    if (!s) return;
    s->num_cuts = (uint32_t)std::min<size_t>(cuts.size(), NC_MAX_CUTS);
    s->audio_rate = rate;
    s->audio_dur = dur;
    s->pending = 0;
    s->analyzed = 1;
    for (uint32_t i = 0; i < s->num_cuts; i++) {
        s->cuts[i].start = cuts[i].start;
        s->cuts[i].end = cuts[i].end;
        strncpy(s->cuts[i].word, cuts[i].word.c_str(), 95);
        s->cuts[i].word[95] = 0;
    }
    seq_unlock(in_data);
}

static bool checkout_mono16k(PF_InData* in_data, A_long start, A_long dur,
                             A_u_long scale, std::vector<float>& out) {
    PF_LayerAudio audio = nullptr;
    PF_SndSamplePtr data0 = nullptr;
    A_long num = 0;
    PF_Err err = PF_CHECKOUT_LAYER_AUDIO(in_data, 0, start, dur, scale,
                                         0x3E800000, PF_SSS_4, PF_Channels_MONO, PF_SIGNED_FLOAT, &audio);
    if (err || !audio) return false;
    err = PF_GET_AUDIO_DATA(in_data, audio, &data0, &num, nullptr, nullptr, nullptr, nullptr);
    if (!err && data0 && num > 0) {
        num--;
        const float* f = (const float*)data0;
        out.assign(f, f + num);
    }
    PF_CHECKIN_LAYER_AUDIO(in_data, audio);
    return !out.empty();
}

static std::string run_analysis(PF_InData* in_data, PF_ParamDef* params[],
                                const std::vector<float>& mono16k, double dur) {
    mc::Config cfg = mc::load_config(plugin_dir());
    mc::Dictionary dict = mc::default_dictionary();
    for (auto& w : cfg.custom_words) dict.add(w, {}, 1);
    for (auto& w : cfg.whitelist) dict.global_whitelist.push_back(mc::normalize_word(w));
    dict.max_severity = platform_severity((int)params[P_PLATFORM]->u.pd.value,
                                          (int)params[P_STRICTNESS]->u.sd.value);

    int backend = (int)params[P_BACKEND]->u.pd.value;
    int lang_idx = (int)params[P_LANGUAGE]->u.pd.value;
    if (lang_idx < 1 || lang_idx > 14) lang_idx = 1;
    std::string lang = kLanguages[lang_idx - 1];
    double pad = params[P_PAD_MS]->u.sd.value / 1000.0;
    double merge = params[P_MERGE_MS]->u.sd.value / 1000.0;

    mc::AnalysisResult r;
    if (backend == 2) {
        mc::GroqOptions go;
        go.api_key = cfg.groq_api_key;
        go.api_keys = cfg.groq_api_keys;
        int gm = (int)params[P_GROQ_MODEL]->u.pd.value;
        go.model = (gm >= 1 && gm <= 2) ? kGroqModels[gm] : "whisper-large-v3";
        go.language = lang;
        r = mc::transcribe_groq(mono16k, go);
    } else {
        mc::AnalysisOptions opt;
        opt.model_path = mc::resolve_model_path(cfg, (int)params[P_MODEL]->u.pd.value);
        opt.language = lang;
        opt.threads = (int)params[P_THREADS]->u.sd.value;
        r = mc::transcribe_local(mono16k.data(), mono16k.size(), opt);
    }
    if (!r.ok) { log_line("ANALYSIS ERROR: " + r.error); return r.error; }
    auto cuts = mc::build_cuts(r.words, dict, pad, merge);
    store_cuts(in_data, cuts, 16000.0, dur);
    log_line("ANALYSIS OK backend=" + std::to_string(backend) + " severity=" + std::to_string(dict.max_severity) +
             " words=" + std::to_string(r.words.size()) + " cuts=" + std::to_string(cuts.size()) + " dur=" + std::to_string(dur));
    for (auto& c : cuts)
        log_line("  cut " + std::to_string(c.start) + "-" + std::to_string(c.end) + " [" + c.word + "]");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK: %zu words, %zu cuts", r.words.size(), cuts.size());
    return std::string(buf);
}

static std::wstring report_text(PF_InData* in_data) {
    SeqData* s = seq_lock(in_data);
    if (!s) return L"No analysis data.";
    std::wstring out = L"Detected cuts: " + std::to_wstring(s->num_cuts) + L"\n\n";
    uint32_t shown = std::min<uint32_t>(s->num_cuts, 60);
    for (uint32_t i = 0; i < shown; i++) {
        wchar_t line[256];
        swprintf(line, 256, L"%02d:%05.2f - %02d:%05.2f  %s\n",
                 (int)s->cuts[i].start / 60, fmod(s->cuts[i].start, 60.0),
                 (int)s->cuts[i].end / 60, fmod(s->cuts[i].end, 60.0),
                 to_wide(s->cuts[i].word).c_str());
        out += line;
    }
    if (s->num_cuts > shown) out += L"...";
    seq_unlock(in_data);
    return out;
}

static PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_SPRINTF(out_data->return_msg, "%s v1.0\n%s", NC_NAME, NC_DESCRIPTION);
    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData*, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    out_data->my_version = PF_VERSION(1, 0, 0, PF_Stage_RELEASE, 1);
    out_data->out_flags  = PF_OutFlag_AUDIO_EFFECT_ONLY | PF_OutFlag_AUDIO_FLOAT_ONLY | PF_OutFlag_AUDIO_IIR;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Enable", TRUE, 0, P_ENABLE);

    PF_ADD_TOPIC("Analysis", P_TOPIC_ANALYSIS);
    PF_ADD_POPUP("Backend", 2, 1, "Local (whisper.cpp)|Groq API", P_BACKEND);
    PF_ADD_POPUP("Model (local)", 6, 2, "tiny|base|small|medium|large-v3|from config", P_MODEL);
    PF_ADD_POPUP("Model (Groq)", 2, 1, "whisper-large-v3|whisper-large-v3-turbo", P_GROQ_MODEL);
    PF_ADD_POPUP("Language", 14, 2, "auto|ru|en|uk|be|de|fr|es|it|pl|tr|ja|ko|zh", P_LANGUAGE);
    PF_ADD_SLIDER("Threads", 1, 16, 1, 16, 4, P_THREADS);
    PF_ADD_CHECKBOXX("Auto-analyze on render", FALSE, 0, P_AUTO);
    PF_ADD_BUTTON("", "Analyze", 0, PF_ParamFlag_SUPERVISE, P_ANALYZE);
    PF_ADD_BUTTON("", "Clear cuts", 0, PF_ParamFlag_SUPERVISE, P_CLEAR);
    PF_ADD_BUTTON("", "Report", 0, PF_ParamFlag_SUPERVISE, P_REPORT);
    PF_END_TOPIC(P_END_ANALYSIS);

    PF_ADD_TOPIC("Detection", P_TOPIC_DETECT);
    PF_ADD_POPUP("Platform preset", 5, 1, "Custom|YouTube|TikTok|Reels|Twitch", P_PLATFORM);
    PF_ADD_SLIDER("Strictness (custom)", 1, 3, 1, 3, 3, P_STRICTNESS);
    PF_ADD_SLIDER("Padding (ms)", 0, 500, 0, 500, 80, P_PAD_MS);
    PF_ADD_SLIDER("Merge gap (ms)", 0, 1000, 0, 1000, 150, P_MERGE_MS);
    PF_END_TOPIC(P_END_DETECT);

    PF_ADD_TOPIC("Replacement", P_TOPIC_REPLACE);
    PF_ADD_POPUP("Replace with", 5, 1, "Beep|Silence|Reverse|Pitch shift|Custom sound", P_MODE);
    PF_ADD_FLOAT_SLIDER("Beep freq (Hz)", 100, 4000, 100, 4000, 0, 1000, 1, 0, FALSE, P_BEEP_FREQ);
    PF_ADD_FLOAT_SLIDER("Replacement level", 0, 1, 0, 1, 0, 0.25f, 2, 0, FALSE, P_BEEP_LEVEL);
    PF_ADD_POPUP("Beep wave", 3, 1, "sine|square|saw", P_BEEP_WAVE);
    PF_ADD_FLOAT_SLIDER("Beep sweep (st)", -24, 24, -24, 24, 0, 0, 1, 0, FALSE, P_BEEP_SWEEP);
    PF_ADD_FLOAT_SLIDER("Pitch (st)", -12, 12, -12, 12, 0, -5, 1, 0, FALSE, P_PITCH_SEMI);
    PF_ADD_FLOAT_SLIDER("Edge fade (ms)", 0, 50, 0, 50, 0, 5, 1, 0, FALSE, P_FADE_MS);
    PF_END_TOPIC(P_END_REPLACE);

    PF_ADD_TOPIC("Manual mute", P_TOPIC_MANUAL);
    PF_ADD_CHECKBOXX("Enable manual mute", FALSE, 0, P_MANUAL_ENABLE);
    PF_ADD_FLOAT_SLIDER("Mute at (sec)", 0, 3600, 0, 3600, 0, 0, 2, 0, FALSE, P_MANUAL_TIME);
    PF_ADD_SLIDER("Mute length (ms)", 0, 5000, 0, 5000, 500, P_MANUAL_LEN);
    PF_END_TOPIC(P_END_MANUAL);

    out_data->num_params = P_NUM_PARAMS;
    return PF_Err_NONE;
}

static PF_Err SequenceSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    out_data->sequence_data = PF_NEW_HANDLE(sizeof(SeqData));
    if (!out_data->sequence_data) return PF_Err_OUT_OF_MEMORY;
    SeqData* s = (SeqData*)PF_LOCK_HANDLE(out_data->sequence_data);
    std::memset(s, 0, sizeof(SeqData));
    s->magic = NC_MAGIC;
    s->version = 1;
    PF_UNLOCK_HANDLE(out_data->sequence_data);
    return PF_Err_NONE;
}

static PF_Err SequenceSetdown(PF_InData* in_data, PF_OutData*, PF_ParamDef*[], PF_LayerDef*) {
    if (in_data->sequence_data) PF_DISPOSE_HANDLE(in_data->sequence_data);
    return PF_Err_NONE;
}

static PF_Err SequenceResetup(PF_InData* in_data, PF_OutData*, PF_ParamDef*[], PF_LayerDef*) {
    SeqData* s = seq_lock(in_data);
    if (s) seq_unlock(in_data);
    return PF_Err_NONE;
}

static PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data,
                               PF_ParamDef* params[], PF_LayerDef*, void* extra) {
    auto* e = (PF_UserChangedParamExtra*)extra;
    if (!e) return PF_Err_NONE;

    if (e->param_index == P_CLEAR) {
        SeqData* s = seq_lock(in_data);
        if (s) { s->num_cuts = 0; s->pending = 0; s->analyzed = 0; seq_unlock(in_data); }
        return PF_Err_NONE;
    }
    if (e->param_index == P_REPORT) {
        MessageBoxW(nullptr, report_text(in_data).c_str(), L"NwRemover report", MB_OK | MB_ICONINFORMATION);
        return PF_Err_NONE;
    }
    if (e->param_index == P_ANALYZE) {
        std::vector<float> mono;
        bool got = checkout_mono16k(in_data, 0, in_data->total_time, in_data->time_scale, mono);
        if (got) {
            double dur = mono.size() / 16000.0;
            std::string res = run_analysis(in_data, params, mono, dur);
            std::wstring msg = to_wide(res);
            if (res.rfind("OK:", 0) == 0)
                MessageBoxW(nullptr, (L"Analysis complete.\n" + msg).c_str(), L"NwRemover", MB_OK | MB_ICONINFORMATION);
            else
                MessageBoxW(nullptr, (L"Analysis failed:\n" + msg).c_str(), L"NwRemover", MB_OK | MB_ICONWARNING);
        } else {
            SeqData* s = seq_lock(in_data);
            if (s) { s->pending = 1; seq_unlock(in_data); }
            MessageBoxW(nullptr,
                L"Could not read layer audio directly.\nAnalysis is armed - it will run on the next audio preview/render (press Space).",
                L"NwRemover", MB_OK | MB_ICONINFORMATION);
        }
        return PF_Err_NONE;
    }
    return PF_Err_NONE;
}

static PF_Err AudioSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef*) {
    out_data->start_sampL = in_data->start_sampL;
    out_data->dur_sampL = in_data->dur_sampL;
    if (out_data->dur_sampL > in_data->total_sampL)
        out_data->dur_sampL = in_data->total_sampL;

    SeqData* s = seq_lock(in_data);
    bool pending = s && s->pending;
    bool analyzed = s && s->analyzed;
    seq_unlock(in_data);
    bool auto_on = params[P_AUTO] && params[P_AUTO]->u.bd.value;
    if (pending || (auto_on && !analyzed)) {
        std::vector<float> mono;
        if (checkout_mono16k(in_data, 0, in_data->total_sampL, in_data->time_scale, mono)) {
            std::string res = run_analysis(in_data, params, mono, mono.size() / 16000.0);
            if (res.rfind("OK:", 0) != 0) {
                SeqData* s2 = seq_lock(in_data);
                if (s2) { s2->pending = 0; s2->analyzed = 1; seq_unlock(in_data); }
            }
        } else {
            SeqData* s2 = seq_lock(in_data);
            if (s2) { s2->pending = 0; s2->analyzed = 1; seq_unlock(in_data); }
        }
    }
    return PF_Err_NONE;
}

static PF_Err AudioRender(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef*) {
    const double rate = in_data->src_snd.fi.rateF;
    const int ch = out_data->dest_snd.fi.num_channels;
    const A_long dur = out_data->dur_sampL;
    if (rate <= 0 || ch <= 0 || dur <= 0) return PF_Err_NONE;

    float* dest = (float*)out_data->dest_snd.dataP;
    const float* src = (const float*)in_data->src_snd.dataP;
    std::memcpy(dest, src, sizeof(float) * (size_t)dur * ch);

    if (!params[P_ENABLE]->u.bd.value) return PF_Err_NONE;

    mc::DspParams p;
    p.mode = (int)params[P_MODE]->u.pd.value;
    p.beep_freq = params[P_BEEP_FREQ]->u.fs_d.value;
    p.beep_level = params[P_BEEP_LEVEL]->u.fs_d.value;
    p.beep_wave = (int)params[P_BEEP_WAVE]->u.pd.value - 1;
    p.beep_sweep = params[P_BEEP_SWEEP]->u.fs_d.value;
    p.pitch_semi = params[P_PITCH_SEMI]->u.fs_d.value;
    p.fade_ms = params[P_FADE_MS]->u.fs_d.value;

    std::vector<mc::Cut> cuts;
    SeqData* s = seq_lock(in_data);
    if (s) {
        for (uint32_t i = 0; i < s->num_cuts; i++) {
            mc::Cut c; c.start = s->cuts[i].start; c.end = s->cuts[i].end; c.word = s->cuts[i].word;
            cuts.push_back(c);
        }
        seq_unlock(in_data);
    }

    // Manual mute region appended as a forced-silence action.
    bool manual = params[P_MANUAL_ENABLE] && params[P_MANUAL_ENABLE]->u.bd.value;
    double m_start = 0, m_end = 0;
    if (manual) {
        m_start = params[P_MANUAL_TIME]->u.fs_d.value;
        m_end = m_start + params[P_MANUAL_LEN]->u.sd.value / 1000.0;
    }

    // Custom replacement sound (mono, resampled to render rate).
    std::shared_ptr<const std::vector<float>> csound;
    if (p.mode == mc::MODE_CUSTOM) {
        mc::Config cfg = mc::load_config(plugin_dir());
        csound = get_custom_sound(cfg.replacement_sound, (int)rate);
    }

    static std::atomic<bool> logged_once{false};
    if (!logged_once.exchange(true))
        log_line("RENDER rate=" + std::to_string(rate) + " ch=" + std::to_string(ch) +
                 " cuts=" + std::to_string(cuts.size()) + " mode=" + std::to_string(p.mode) +
                 " manual=" + (manual ? "1" : "0"));

    const double chunk_start = in_data->start_sampL / rate;
    const double chunk_end = (in_data->start_sampL + dur) / rate;
    const PF_UFixed rate_fixed = (PF_UFixed)((uint32_t)rate << 16);

    // total actions = detected cuts + optional manual mute
    size_t nact = cuts.size() + (manual ? 1 : 0);
    for (size_t ai = 0; ai < nact; ai++) {
        double a0, b0; int mode;
        if (ai < cuts.size()) { a0 = cuts[ai].start; b0 = cuts[ai].end; mode = p.mode; }
        else { a0 = m_start; b0 = m_end; mode = mc::MODE_MUTE; }
        if (b0 <= chunk_start || a0 >= chunk_end) continue;

        double a_sec = std::max(a0, chunk_start);
        double b_sec = std::min(b0, chunk_end);
        A_long a_samp = (A_long)(a_sec * rate + 0.5) - in_data->start_sampL;
        A_long b_samp = (A_long)(b_sec * rate + 0.5) - in_data->start_sampL;
        if (a_samp < 0) a_samp = 0;
        if (b_samp > dur) b_samp = dur;
        if (b_samp <= a_samp) continue;
        A_long vis = b_samp - a_samp;

        if (mode == mc::MODE_BEEP || mode == mc::MODE_MUTE || mode == mc::MODE_CUSTOM) {
            std::vector<float> tmp((size_t)vis * ch);
            if (mode == mc::MODE_MUTE)
                std::memset(tmp.data(), 0, tmp.size() * sizeof(float));
            else if (mode == mc::MODE_CUSTOM && csound && !csound->empty())
                mc::process_custom(csound->data(), (int64_t)csound->size(), tmp.data(), vis, ch, rate, p.beep_level, p.fade_ms);
            else
                mc::process_interval(nullptr, tmp.data(), vis, ch, rate, p); // beep (or custom fallback)
            std::memcpy(dest + (size_t)a_samp * ch, tmp.data(), sizeof(float) * tmp.size());
        } else {
            // Reverse / Pitch: need full interval source audio.
            A_long ia = (A_long)(a0 * rate + 0.5);
            A_long ib = (A_long)(b0 * rate + 0.5);
            if (ia < 0) ia = 0;
            A_long n = ib - ia;
            if (n <= 0) continue;
            PF_LayerAudio audio = nullptr; PF_SndSamplePtr data0 = nullptr; A_long num = 0;
            PF_Err err = PF_CHECKOUT_LAYER_AUDIO(in_data, 0, ia, n, in_data->time_scale,
                                                 rate_fixed, PF_SSS_4, ch, PF_SIGNED_FLOAT, &audio);
            if (err || !audio) continue;
            PF_GET_AUDIO_DATA(in_data, audio, &data0, &num, nullptr, nullptr, nullptr, nullptr);
            if (data0 && num >= n) {
                mc::DspParams pm = p; pm.mode = mode;
                std::vector<float> full((size_t)n * ch), tmp((size_t)n * ch);
                std::memcpy(full.data(), data0, sizeof(float) * full.size());
                mc::process_interval(full.data(), tmp.data(), n, ch, rate, pm);
                A_long vis_start = (A_long)(a_sec * rate + 0.5) - ia;
                if (vis_start < 0) vis_start = 0;
                if (vis_start + vis <= n)
                    std::memcpy(dest + (size_t)a_samp * ch, tmp.data() + (size_t)vis_start * ch,
                                sizeof(float) * (size_t)vis * ch);
            }
            PF_CHECKIN_LAYER_AUDIO(in_data, audio);
        }
    }
    return PF_Err_NONE;
}

extern "C" __declspec(dllexport) PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output, void* extra) {
    switch (cmd) {
    case PF_Cmd_ABOUT:            return About(in_data, out_data, params, output);
    case PF_Cmd_GLOBAL_SETUP:     return GlobalSetup(in_data, out_data, params, output);
    case PF_Cmd_PARAMS_SETUP:     return ParamsSetup(in_data, out_data, params, output);
    case PF_Cmd_SEQUENCE_SETUP:   return SequenceSetup(in_data, out_data, params, output);
    case PF_Cmd_SEQUENCE_SETDOWN: return SequenceSetdown(in_data, out_data, params, output);
    case PF_Cmd_SEQUENCE_RESETUP: return SequenceResetup(in_data, out_data, params, output);
    case PF_Cmd_USER_CHANGED_PARAM: return UserChangedParam(in_data, out_data, params, output, extra);
    case PF_Cmd_AUDIO_SETUP:      return AudioSetup(in_data, out_data, params, output);
    case PF_Cmd_AUDIO_RENDER:     return AudioRender(in_data, out_data, params, output);
    default: return PF_Err_NONE;
    }
}