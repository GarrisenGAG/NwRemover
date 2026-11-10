#include "Groq.h"
#include "Util.h"
#include "json.hpp"
#include <windows.h>
#include <winhttp.h>
#include <cstring>
#include <cstdlib>
#include <atomic>

namespace mc {

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring strip_scheme(const std::string& u) {
    std::string s = u;
    size_t p = s.find("://");
    if (p != std::string::npos) s = s.substr(p + 3);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return to_wide(s);
}

// Proxy precedence: config -> HTTPS_PROXY/https_proxy/HTTP_PROXY/http_proxy -> none (automatic).
static std::wstring effective_proxy(const GroqOptions& opt) {
    if (!opt.proxy.empty()) return strip_scheme(opt.proxy);
    const char* envs[] = {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"};
    for (const char* e : envs) {
        const char* v = getenv(e);
        if (v && *v) return strip_scheme(v);
    }
    return L"";
}

static void append(std::vector<uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
}

struct HttpResult {
    bool sent = false;
    DWORD status = 0;
    std::string resp;
    std::string error;
};

static HttpResult do_request(const std::string& api_key, const std::string& boundary,
                             const std::vector<uint8_t>& body, const std::wstring& proxy_hostport) {
    HttpResult r;
    DWORD access = proxy_hostport.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                          : WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    HINTERNET hSession = WinHttpOpen(L"NwRemover/1.0", access,
                                     proxy_hostport.empty() ? WINHTTP_NO_PROXY_NAME : proxy_hostport.c_str(),
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { r.error = "WinHttpOpen failed"; return r; }
    WinHttpSetTimeouts(hSession, 10000, 15000, 60000, 120000);
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.groq.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = hConnect ? WinHttpOpenRequest(hConnect, L"POST",
        L"/openai/v1/audio/transcriptions", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (hRequest) {
        std::wstring headers = L"Authorization: Bearer " + to_wide(api_key) +
            L"\r\nContent-Type: multipart/form-data; boundary=" + to_wide(boundary);
        BOOL ok = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(),
            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
        if (ok) {
            r.sent = true;
            DWORD sz = sizeof(r.status), idx = 0;
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &sz, &idx);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                std::vector<char> buf(avail);
                DWORD got = 0;
                if (!WinHttpReadData(hRequest, buf.data(), avail, &got) || got == 0) break;
                r.resp.append(buf.data(), got);
            }
        } else {
            r.error = "WinHTTP error " + std::to_string(GetLastError());
        }
    } else {
        r.error = "WinHttpConnect/OpenRequest failed";
    }
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return r;
}

static std::atomic<size_t> g_key_index{0};

AnalysisResult transcribe_groq(const std::vector<float>& mono16k, const GroqOptions& opt) {
    AnalysisResult res;
    std::vector<std::string> keys = opt.api_keys;
    if (!opt.api_key.empty()) keys.push_back(opt.api_key);
    if (keys.empty()) { res.error = "No Groq API key (set groq_api_keys or groq_api_key in NwRemover.json)"; return res; }
    if (mono16k.empty()) { res.error = "Empty audio"; return res; }

    std::vector<uint8_t> wav = wav16_bytes(mono16k);
    if (wav.size() > 24u * 1024 * 1024) { res.error = "Audio exceeds Groq 25 MB limit; use local backend"; return res; }

    const std::string b = "----MatCutterBoundary7MA4YWxkTrZu0gW";
    std::vector<uint8_t> body;
    append(body, "--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n");
    body.insert(body.end(), wav.begin(), wav.end());
    append(body, "\r\n--" + b + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + opt.model);
    append(body, "\r\n--" + b + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\nverbose_json");
    append(body, "\r\n--" + b + "\r\nContent-Disposition: form-data; name=\"timestamp_granularities[]\"\r\n\r\nword");
    append(body, "\r\n--" + b + "\r\nContent-Disposition: form-data; name=\"temperature\"\r\n\r\n0");
    if (opt.language != "auto" && !opt.language.empty())
        append(body, "\r\n--" + b + "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n" + opt.language);
    append(body, "\r\n--" + b + "--\r\n");

    size_t nkeys = keys.size();
    size_t start = g_key_index.fetch_add(1) % nkeys;
    std::wstring proxy = effective_proxy(opt);
    std::string last_err;
    for (size_t i = 0; i < nkeys; i++) {
        const std::string& key = keys[(start + i) % nkeys];
        HttpResult hr = do_request(key, b, body, proxy);
        if (!hr.sent) { last_err = hr.error; continue; }
        if (hr.status == 200) {
            auto j = nlohmann::json::parse(hr.resp, nullptr, false);
            if (j.is_discarded()) { res.error = "Groq: bad JSON response"; return res; }
            if (j.contains("words") && j["words"].is_array()) {
                for (auto& w : j["words"]) {
                    Word wd;
                    wd.text = w.value("word", "");
                    wd.start = w.value("start", 0.0);
                    wd.end = w.value("end", wd.start);
                    if (!wd.text.empty()) res.words.push_back(wd);
                }
            }
            res.ok = true;
            return res;
        }
        if (hr.status == 429 || hr.status == 401 || hr.status == 403) {
            // rate-limited or bad key: rotate to next
            auto j = nlohmann::json::parse(hr.resp, nullptr, false);
            std::string msg = j.is_discarded() ? "" : j.value("error", nlohmann::json::object()).value("message", "");
            last_err = "HTTP " + std::to_string(hr.status) + " " + msg;
            continue;
        }
        // other error: not key-related, stop
        auto j = nlohmann::json::parse(hr.resp, nullptr, false);
        std::string msg = j.is_discarded() ? hr.resp.substr(0, 200) : j.value("error", nlohmann::json::object()).value("message", hr.resp.substr(0, 200));
        res.error = "Groq HTTP " + std::to_string(hr.status) + ": " + msg;
        return res;
    }
    res.error = "Groq: all keys failed (last: " + last_err + ")";
    return res;
}

} // namespace mc