// NwRemover - After Effects audio effect: auto-censor profanity via local
// whisper.cpp or Groq API. Platform presets, custom replacement sound, manual mute.
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <thread>
#include <chrono>
#include <functional>

#include "AEConfig.h"
#include "A.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectUI.h"
#include "AE_EffectSuites.h"
#include "AE_EffectVers.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "SP/SPBasic.h"

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
    P_STATUS,
    P_INFO,
    P_PROGRESS,
    P_TOPIC_ANALYSIS,
    P_BACKEND,
    P_MODEL,
    P_MODELDIR,
    P_GROQ_MODEL,
    P_APIKEY,
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

// cancel flag for whisper (read from ggml worker threads, set from the calling thread)
static std::atomic<bool> g_analysis_cancel{false};
static void update_param_name(PF_InData* in_data, PF_ParamDef* params[],
                              PF_ParamIndex idx, const char* name);
static void update_slider_value(PF_InData* in_data, PF_ParamDef* params[],
                                PF_ParamIndex idx, float v);

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

// ---- analysis pipeline (shared by render path and interactive path) ----
// Everything AE-related (param UI, sequence-data locks, PF_PROGRESS) stays on the
// thread that owns in_data. The transcription itself is pure and can run on a worker.
struct JobInputs {
    int backend = 1;
    std::string lang = "ru";
    double pad = 0.08, merge = 0.15;
    mc::Dictionary dict;
    std::string model_path;   // local
    int threads = 4;          // local
    mc::GroqOptions go;       // groq
};

static JobInputs prepare_analysis(PF_InData* in_data, PF_ParamDef* params[]) {
    JobInputs in;
    mc::Config cfg = mc::load_config(plugin_dir());
    in.dict = mc::default_dictionary();
    for (auto& w : cfg.custom_words) in.dict.add(w, {}, 1);
    for (auto& w : cfg.whitelist) in.dict.global_whitelist.push_back(mc::normalize_word(w));
    in.dict.max_severity = platform_severity((int)params[P_PLATFORM]->u.pd.value,
                                             (int)params[P_STRICTNESS]->u.sd.value);
    in.backend = (int)params[P_BACKEND]->u.pd.value;
    int lang_idx = (int)params[P_LANGUAGE]->u.pd.value;
    if (lang_idx < 1 || lang_idx > 14) lang_idx = 1;
    in.lang = kLanguages[lang_idx - 1];
    in.pad = params[P_PAD_MS]->u.sd.value / 1000.0;
    in.merge = params[P_MERGE_MS]->u.sd.value / 1000.0;
    if (in.backend == 2) {
        in.go.api_key = cfg.groq_api_key;
        in.go.api_keys = cfg.groq_api_keys;
        in.go.proxy = cfg.proxy;
        int gm = (int)params[P_GROQ_MODEL]->u.pd.value;
        in.go.model = (gm >= 1 && gm <= 2) ? kGroqModels[gm] : "whisper-large-v3";
        in.go.language = in.lang;
    } else {
        in.model_path = mc::resolve_model_path(cfg, (int)params[P_MODEL]->u.pd.value);
        in.threads = (int)params[P_THREADS]->u.sd.value;
    }
    return in;
}

// Pure transcription + cut building. No AE calls. progress_fn/abort_fn are supplied
// by the caller: the render path wires them to PF_PROGRESS/PF_ABORT (calling thread),
// the interactive path wires them to atomics read/written from a worker thread.
static mc::AnalysisResult run_transcription(const JobInputs& in, const std::vector<float>& mono,
                                            std::function<void(int)> progress_fn,
                                            std::function<bool()> abort_fn,
                                            std::vector<mc::Cut>* cuts) {
    mc::AnalysisResult r;
    if (in.backend == 2) {
        r = mc::transcribe_groq(mono, in.go);
    } else {
        mc::AnalysisOptions opt;
        opt.model_path = in.model_path;
        opt.language = in.lang;
        opt.threads = in.threads;
        opt.progress = progress_fn;
        opt.abort = abort_fn;
        r = mc::transcribe_local(mono.data(), mono.size(), opt);
    }
    if (r.ok && cuts) *cuts = mc::build_cuts(r.words, in.dict, in.pad, in.merge);
    return r;
}

// Store results + update the panel. Must run on the in_data-owning thread.
static std::string finalize_analysis(PF_InData* in_data, PF_ParamDef* params[],
                                     const JobInputs& in, const mc::AnalysisResult& r,
                                     const std::vector<mc::Cut>& cuts, double dur,
                                     bool show_status, bool cancelled) {
    if (cancelled) {
        log_line("ANALYSIS CANCELLED");
        if (show_status) {
            update_param_name(in_data, params, P_STATUS, "Status: cancelled");
            update_param_name(in_data, params, P_INFO, "Info: analysis cancelled");
            update_slider_value(in_data, params, P_PROGRESS, 0);
        }
        return "Cancelled";
    }
    if (!r.ok) {
        log_line("ANALYSIS ERROR: " + r.error);
        if (show_status) {
            update_param_name(in_data, params, P_STATUS, "Status: error");
            std::string info = "Info: " + r.error;
            if (info.size() > 90) info.resize(90);
            update_param_name(in_data, params, P_INFO, info.c_str());
        }
        return r.error;
    }
    store_cuts(in_data, cuts, 16000.0, dur);
    log_line("ANALYSIS OK backend=" + std::to_string(in.backend) + " severity=" + std::to_string(in.dict.max_severity) +
             " words=" + std::to_string(r.words.size()) + " cuts=" + std::to_string(cuts.size()) + " dur=" + std::to_string(dur));
    for (auto& c : cuts)
        log_line("  cut " + std::to_string(c.start) + "-" + std::to_string(c.end) + " [" + c.word + "]");
    if (show_status) {
        update_param_name(in_data, params, P_STATUS, "Status: done");
        char ibuf[128];
        std::snprintf(ibuf, sizeof(ibuf), "Info: %zu cuts from %zu words", cuts.size(), r.words.size());
        update_param_name(in_data, params, P_INFO, ibuf);
        update_slider_value(in_data, params, P_PROGRESS, 100);
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK: %zu words, %zu cuts", r.words.size(), cuts.size());
    return std::string(buf);
}

// Render path: synchronous on the render thread, progress reported through AE's own
// render progress bar via PF_PROGRESS (safe from the whisper_full calling thread).
static std::string run_analysis(PF_InData* in_data, PF_ParamDef* params[],
                                const std::vector<float>& mono16k, double dur, bool show_status) {
    JobInputs in = prepare_analysis(in_data, params);
    if (show_status) update_param_name(in_data, params, P_STATUS, "Status: analyzing...");
    g_analysis_cancel = false;
    auto prog = [in_data](int p){
        if (PF_ABORT(in_data) != PF_Err_NONE) g_analysis_cancel = true;
        PF_PROGRESS(in_data, p, 100);
    };
    auto abrt = []()->bool { return g_analysis_cancel.load(); };
    std::vector<mc::Cut> cuts;
    mc::AnalysisResult r = run_transcription(in, mono16k, prog, abrt, &cuts);
    bool cancelled = g_analysis_cancel.load() && !r.ok;
    return finalize_analysis(in_data, params, in, r, cuts, dur, show_status, cancelled);
}

// ---- native AE progress dialog (PFAppSuite6) ----
static PFAppSuite6* acquire_app_suite(PF_InData* in_data) {
    if (!in_data->pica_basicP) return nullptr;
    PFAppSuite6* s = nullptr;
    if ((*in_data->pica_basicP->AcquireSuite)(kPFAppSuite, kPFAppSuiteVersion6, (const void**)&s) != 0 || !s)
        return nullptr;
    return s;
}
static void release_app_suite(PF_InData* in_data, PFAppSuite6* s) {
    if (in_data->pica_basicP && s)
        (*in_data->pica_basicP->ReleaseSuite)(kPFAppSuite, kPFAppSuiteVersion6);
}

// Interactive path (Analyze button): transcription runs on a worker thread while the
// UI thread pumps AE's native modal progress dialog. The dialog is the "normal" AE
// progress bar with a Cancel button; PF_AppProgressDialogUpdate keeps the UI alive and
// returns PF_Interrupt_CANCEL when the user cancels. whisper reads determinate percent;
// Groq is a single blocking upload so it gets the indeterminate barber-pole style.
static void run_analysis_interactive(PF_InData* in_data, PF_ParamDef* params[],
                                     const std::vector<float>& mono, double dur) {
    JobInputs in = prepare_analysis(in_data, params);
    update_param_name(in_data, params, P_STATUS, "Status: analyzing...");
    update_slider_value(in_data, params, P_PROGRESS, 0);

    std::atomic<int> progress{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    mc::AnalysisResult result;
    std::vector<mc::Cut> cuts;

    std::thread worker([&]{
        auto prog = [&progress](int p){ progress.store(p); };
        auto abrt = [&cancel]()->bool { return cancel.load(); };
        result = run_transcription(in, mono, prog, abrt, &cuts);
        done.store(true);
    });

    PFAppSuite6* app = acquire_app_suite(in_data);
    PF_AppProgressDialogP dlg = nullptr;
    bool determinate = (in.backend != 2);
    if (app && app->PF_CreateNewAppProgressDialog)
        app->PF_CreateNewAppProgressDialog(reinterpret_cast<const A_UTF16Char*>(L"NwRemover - analyzing audio"),
                                           nullptr, determinate ? FALSE : TRUE, &dlg);

    // Pump on the UI thread (~80ms cadence per SDK guidance). No AE calls from worker.
    while (!done.load()) {
        int p = progress.load();
        if (dlg && app && app->PF_AppProgressDialogUpdate) {
            PF_Err e = app->PF_AppProgressDialogUpdate(dlg, determinate ? p : 0, determinate ? 100 : 0);
            if (e == PF_Interrupt_CANCEL) cancel.store(true);
        }
        update_slider_value(in_data, params, P_PROGRESS, (float)p);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    worker.join();
    if (dlg && app && app->PF_DisposeAppProgressDialog) app->PF_DisposeAppProgressDialog(dlg);
    release_app_suite(in_data, app);

    bool cancelled = cancel.load() && !result.ok;
    finalize_analysis(in_data, params, in, result, cuts, dur, true, cancelled);
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

// ---- runtime UI updates via PF Param Utils Suite ----
static PF_ParamUtilsSuite3* acquire_param_suite(PF_InData* in_data) {
    if (!in_data->pica_basicP) return nullptr;
    PF_ParamUtilsSuite3* s = nullptr;
    if ((*in_data->pica_basicP->AcquireSuite)(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3,
                                              (const void**)&s) != 0 || !s) return nullptr;
    return s;
}
static void release_param_suite(PF_InData* in_data, PF_ParamUtilsSuite3* s) {
    if (in_data->pica_basicP && s)
        (*in_data->pica_basicP->ReleaseSuite)(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
}

// Change a param's displayed name (status labels). Cosmetic only, no undo entry.
static void update_param_name(PF_InData* in_data, PF_ParamDef* params[],
                              PF_ParamIndex idx, const char* name) {
    if (!params || !params[idx]) return;
    PF_ParamUtilsSuite3* s = acquire_param_suite(in_data);
    if (!s || !s->PF_UpdateParamUI) { release_param_suite(in_data, s); return; }
    PF_ParamDef def = *params[idx];
    PF_STRNNCPY(def.name, name, sizeof(def.name));
    (*s->PF_UpdateParamUI)(in_data->effect_ref, idx, &def);
    release_param_suite(in_data, s);
}

// Gray out params that do not belong to the selected backend.
// (PF_PUI_INVISIBLE is ignored by the AE Effect Controls panel; PF_PUI_DISABLED works.)
static void update_backend_ui(PF_InData* in_data, PF_ParamDef* params[]) {
    if (!params || !params[P_BACKEND]) return;
    PF_ParamUtilsSuite3* s = acquire_param_suite(in_data);
    static std::atomic<bool> logged{false};
    if (!s || !s->PF_UpdateParamUI) {
        if (!logged.exchange(true)) log_line("BACKEND_UI: param utils suite unavailable");
        release_param_suite(in_data, s);
        return;
    }
    bool local = ((int)params[P_BACKEND]->u.pd.value == 1);
    const PF_ParamIndex list[] = { P_MODEL, P_MODELDIR, P_THREADS, P_GROQ_MODEL, P_APIKEY };
    for (PF_ParamIndex idx : list) {
        if (!params[idx]) continue;
        PF_ParamDef def = *params[idx];
        bool show = (idx == P_MODEL || idx == P_MODELDIR || idx == P_THREADS) ? local : !local;
        if (show) def.ui_flags &= ~PF_PUI_DISABLED;
        else def.ui_flags |= PF_PUI_DISABLED;
        (*s->PF_UpdateParamUI)(in_data->effect_ref, idx, &def);
    }
    release_param_suite(in_data, s);
    if (!logged.exchange(true)) log_line(std::string("BACKEND_UI: applied local=") + (local ? "1" : "0"));
}

static std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---- Win32 dialogs ----
struct ApiKeyCtx { std::wstring value; bool ok = false; };

static LRESULT CALLBACK apikey_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        ApiKeyCtx* ctx = (ApiKeyCtx*)((LPCREATESTRUCTW)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)ctx);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND lbl = CreateWindowExW(0, L"STATIC", L"Groq API key (gsk_...):",
            WS_CHILD | WS_VISIBLE, 12, 12, 380, 20, h, NULL, NULL, NULL);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            12, 36, 392, 24, h, (HMENU)1001, NULL, NULL);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 216, 72, 92, 28, h, (HMENU)IDOK, NULL, NULL);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"Отмена",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 316, 72, 92, 28, h, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(edit, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(ok, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)f, TRUE);
        SetFocus(edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            ApiKeyCtx* ctx = (ApiKeyCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (LOWORD(w) == IDOK && ctx) {
                int len = GetWindowTextLengthW(GetDlgItem(h, 1001));
                std::vector<wchar_t> buf(len + 1, 0);
                GetDlgItemTextW(h, 1001, buf.data(), len + 1);
                ctx->value = buf.data();
                ctx->ok = true;
            }
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool api_key_input_dialog(std::wstring& out) {
    static const wchar_t* cls = L"NwRemoverKeyBox";
    WNDCLASSW wc = {};
    if (!GetClassInfoW(NULL, cls, &wc)) {
        wc.lpfnWndProc = apikey_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
    }
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = 430, hgt = 140;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - hgt) / 2;
    ApiKeyCtx ctx;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, cls,
        L"NwRemover - Groq API key",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, w, hgt, NULL, NULL, GetModuleHandleW(NULL), &ctx);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    MSG m;
    while (IsWindow(hwnd) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (ctx.ok) out = ctx.value;
    return ctx.ok;
}

// Folder picker for the models directory (shell browse).
static bool folder_pick_dialog(std::wstring& out) {
    bool com_here = false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) com_here = true; // S_OK or S_FALSE: balanced CoUninitialize
    bool ok = false;
    BROWSEINFOW bi = {};
    wchar_t title[128];
    swprintf(title, 128, L"NwRemover: папка с моделями whisper (ggml-*.bin)");
    bi.hwndOwner = NULL;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = {0};
        if (SHGetPathFromIDListW(pidl, path)) { out = path; ok = true; }
        CoTaskMemFree(pidl);
    }
    if (com_here) CoUninitialize();
    return ok;
}

struct ReportCtx { std::wstring text; };

static LRESULT CALLBACK report_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        ReportCtx* ctx = (ReportCtx*)((LPCREATESTRUCTW)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)ctx);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            12, 12, 500, 320, h, (HMENU)2001, NULL, NULL);
        HWND copy = CreateWindowExW(0, L"BUTTON", L"Копировать",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 306, 344, 100, 28, h, (HMENU)2002, NULL, NULL);
        HWND close = CreateWindowExW(0, L"BUTTON", L"Закрыть",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 414, 344, 100, 28, h, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(edit, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(copy, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(close, WM_SETFONT, (WPARAM)f, TRUE);
        SetWindowTextW(edit, ctx->text.c_str());
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 2002) { // copy to clipboard
            ReportCtx* ctx = (ReportCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (ctx && OpenClipboard(h)) {
                EmptyClipboard();
                SIZE_T bytes = (ctx->text.size() + 1) * sizeof(wchar_t);
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (g) {
                    memcpy(GlobalLock(g), ctx->text.c_str(), bytes);
                    GlobalUnlock(g);
                    SetClipboardData(CF_UNICODETEXT, g);
                }
                CloseClipboard();
            }
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void show_report_dialog(const std::wstring& text) {
    static const wchar_t* cls = L"NwRemoverReportBox";
    WNDCLASSW wc = {};
    if (!GetClassInfoW(NULL, cls, &wc)) {
        wc.lpfnWndProc = report_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
    }
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = 540, hgt = 420;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - hgt) / 2;
    ReportCtx ctx{ text };
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, cls,
        L"NwRemover - отчёт по вырезкам",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, w, hgt, NULL, NULL, GetModuleHandleW(NULL), &ctx);
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    MSG m;
    while (IsWindow(hwnd) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

static PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_SPRINTF(out_data->return_msg, "%s v1.0\n%s", NC_NAME, NC_DESCRIPTION);
    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData*, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    out_data->my_version = PF_VERSION(1, 0, 0, PF_Stage_RELEASE, 1);
    out_data->out_flags  = PF_OutFlag_AUDIO_EFFECT_ONLY | PF_OutFlag_AUDIO_FLOAT_ONLY | PF_OutFlag_AUDIO_IIR
                         | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Enable", TRUE, 0, P_ENABLE);
    // Status labels: AE renders PF_Param_NO_DATA as nothing, so use disabled
    // checkboxes (grayed) whose names we rewrite at runtime via UpdateParamUI.
    // AE requires u.bd.u.nameptr to be set for every checkbox (validation).
    static const char kSpaceName[] = " ";
    {
        AEFX_CLR_STRUCT(def);
        def.param_type = PF_Param_CHECKBOX;
        PF_STRNNCPY(def.name, "Status: ready", sizeof(def.name));
        def.u.bd.u.nameptr = kSpaceName;
        def.u.bd.value = def.u.bd.dephault = FALSE;
        def.ui_flags = PF_PUI_DISABLED;
        def.uu.id = P_STATUS;
        PF_ADD_PARAM(in_data, -1, &def);
        AEFX_CLR_STRUCT(def);
        def.param_type = PF_Param_CHECKBOX;
        def.name[0] = 0;
        def.u.bd.u.nameptr = kSpaceName;
        def.u.bd.value = def.u.bd.dephault = FALSE;
        def.ui_flags = PF_PUI_DISABLED;
        def.uu.id = P_INFO;
        PF_ADD_PARAM(in_data, -1, &def);
    }
    // In-panel progress bar: a disabled float slider whose value we drive from a
    // UI-thread timer while the async analysis runs (no separate progress window).
    {
        AEFX_CLR_STRUCT(def);
        def.param_type = PF_Param_FLOAT_SLIDER;
        PF_STRNNCPY(def.name, "Progress", sizeof(def.name));
        def.u.fs_d.valid_min = 0; def.u.fs_d.valid_max = 100;
        def.u.fs_d.slider_min = 0; def.u.fs_d.slider_max = 100;
        def.u.fs_d.value = def.u.fs_d.dephault = 0;
        def.u.fs_d.precision = 0;
        def.ui_flags = PF_PUI_DISABLED;
        def.uu.id = P_PROGRESS;
        PF_ADD_PARAM(in_data, -1, &def);
    }

    PF_ADD_TOPIC("Analysis", P_TOPIC_ANALYSIS);
    // Backend popup written manually: needs PF_ParamFlag_SUPERVISE so that
    // switching it fires USER_CHANGED_PARAM and we show/hide backend params.
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_POPUP;
    PF_STRNNCPY(def.name, "Backend", sizeof(def.name));
    def.u.pd.num_choices = 2;
    def.u.pd.value = def.u.pd.dephault = 1;
    def.u.pd.u.namesptr = "Local (whisper.cpp)|Groq API";
    def.flags = PF_ParamFlag_SUPERVISE;
    def.uu.id = P_BACKEND;
    {
        PF_Err priv_err = PF_ADD_PARAM(in_data, -1, &def);
        if (priv_err) return priv_err;
    }
    PF_ADD_POPUP("Model (local)", 6, 2, "tiny|base|small|medium|large-v3|from config", P_MODEL);
    PF_ADD_BUTTON("", "Models folder...", 0, PF_ParamFlag_SUPERVISE, P_MODELDIR);
    PF_ADD_POPUP("Model (Groq)", 2, 1, "whisper-large-v3|whisper-large-v3-turbo", P_GROQ_MODEL);
    PF_ADD_BUTTON("", "API key...", 0, PF_ParamFlag_SUPERVISE, P_APIKEY);
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

// Update a float-slider param value from within a valid AE callback (no separate window).
static void update_slider_value(PF_InData* in_data, PF_ParamDef* params[], PF_ParamIndex idx, float v) {
    if (!params || !params[idx]) return;
    PF_ParamUtilsSuite3* s = acquire_param_suite(in_data);
    if (!s || !s->PF_UpdateParamUI) { release_param_suite(in_data, s); return; }
    PF_ParamDef d = *params[idx]; d.u.fs_d.value = (PF_FpShort)v;
    (*s->PF_UpdateParamUI)(in_data->effect_ref, idx, &d);
    release_param_suite(in_data, s);
}


static PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data,
                               PF_ParamDef* params[], PF_LayerDef*, void* extra) {
    auto* e = (PF_UserChangedParamExtra*)extra;
    if (!e) return PF_Err_NONE;

    if (e->param_index == P_BACKEND) update_backend_ui(in_data, params);

    if (e->param_index == P_CLEAR) {
        SeqData* s = seq_lock(in_data);
        if (s) { s->num_cuts = 0; s->pending = 0; s->analyzed = 0; seq_unlock(in_data); }
        return PF_Err_NONE;
    }
    if (e->param_index == P_REPORT) {
        show_report_dialog(report_text(in_data));
        return PF_Err_NONE;
    }
    if (e->param_index == P_APIKEY) {
        std::wstring wkey;
        if (!api_key_input_dialog(wkey)) return PF_Err_NONE;
        std::string key = to_utf8(wkey);
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r' || key.back() == '\n')) key.pop_back();
        if (key.rfind("gsk_", 0) != 0 || key.size() < 20) {
            MessageBoxW(nullptr, L"Ключ должен начинаться с gsk_ и быть полным.", L"NwRemover", MB_OK | MB_ICONWARNING);
            return PF_Err_NONE;
        }
        if (!mc::add_groq_key(plugin_dir(), key)) {
            MessageBoxW(nullptr, L"Не удалось записать NwRemover.json", L"NwRemover", MB_OK | MB_ICONERROR);
            return PF_Err_NONE;
        }
        mc::Config cfg = mc::load_config(plugin_dir());
        wchar_t msg[160];
        swprintf(msg, 160, L"Ключ сохранён в NwRemover.json\nКлючей в пуле: %zu", cfg.groq_api_keys.size());
        MessageBoxW(nullptr, msg, L"NwRemover", MB_OK | MB_ICONINFORMATION);
        return PF_Err_NONE;
    }
    if (e->param_index == P_MODELDIR) {
        std::wstring wdir;
        if (!folder_pick_dialog(wdir)) return PF_Err_NONE;
        std::string dir = to_utf8(wdir);
        if (!mc::set_models_dir(plugin_dir(), dir)) {
            MessageBoxW(nullptr, L"Не удалось записать NwRemover.json", L"NwRemover", MB_OK | MB_ICONERROR);
            return PF_Err_NONE;
        }
        std::string info = "Info: models " + dir;
        update_param_name(in_data, params, P_INFO, info.c_str());
        return PF_Err_NONE;
    }
    if (e->param_index == P_ANALYZE) {
        std::vector<float> mono;
        bool got = checkout_mono16k(in_data, 0, in_data->total_time, in_data->time_scale, mono);
        if (got) {
            run_analysis_interactive(in_data, params, mono, mono.size() / 16000.0);
        } else {
            SeqData* s = seq_lock(in_data);
            if (s) { s->pending = 1; seq_unlock(in_data); }
            update_param_name(in_data, params, P_STATUS, "Status: armed, press Space");
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
            std::string res = run_analysis(in_data, params, mono, mono.size() / 16000.0, false);
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
    case PF_Cmd_UPDATE_PARAMS_UI:   update_backend_ui(in_data, params); return PF_Err_NONE;
    case PF_Cmd_AUDIO_SETUP:      return AudioSetup(in_data, out_data, params, output);
    case PF_Cmd_AUDIO_RENDER:     return AudioRender(in_data, out_data, params, output);
    default: return PF_Err_NONE;
    }
}