#include "Util.h"
#include <fstream>
#include <cstring>
#include <cmath>

namespace mc {

static uint32_t rd32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint16_t rd16(const char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

WavData read_wav(const std::string& path) {
    WavData w;
    std::ifstream f(path, std::ios::binary);
    if (!f) return w;
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() < 44 || std::memcmp(raw.data(), "RIFF", 4) != 0) return w;
    int format = 0, bits = 0;
    size_t pos = 12;
    const char* data = nullptr;
    uint32_t data_sz = 0;
    while (pos + 8 <= raw.size()) {
        uint32_t sz = rd32(raw.data() + pos + 4);
        const char* id = raw.data() + pos;
        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            format = rd16(raw.data() + pos + 8);
            w.channels = rd16(raw.data() + pos + 10);
            w.rate = (int)rd32(raw.data() + pos + 12);
            bits = rd16(raw.data() + pos + 22);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = raw.data() + pos + 8;
            data_sz = sz;
            if (data + data_sz > raw.data() + raw.size())
                data_sz = (uint32_t)(raw.data() + raw.size() - data);
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!data || w.channels <= 0 || w.rate <= 0) return w;
    size_t n = 0;
    if (format == 1 && bits == 16) {
        n = data_sz / 2;
        w.samples.resize(n);
        const int16_t* s = (const int16_t*)data;
        for (size_t i = 0; i < n; i++) w.samples[i] = s[i] / 32768.0f;
    } else if (format == 3 && bits == 32) {
        n = data_sz / 4;
        w.samples.resize(n);
        std::memcpy(w.samples.data(), data, n * 4);
    } else if (format == 1 && bits == 24) {
        n = data_sz / 3;
        w.samples.resize(n);
        for (size_t i = 0; i < n; i++) {
            int32_t v = (unsigned char)data[i*3] | ((unsigned char)data[i*3+1] << 8) | ((signed char)data[i*3+2] << 16);
            w.samples[i] = v / 8388608.0f;
        }
    } else if (format == 1 && bits == 8) {
        n = data_sz;
        w.samples.resize(n);
        for (size_t i = 0; i < n; i++) w.samples[i] = ((unsigned char)data[i] - 128) / 128.0f;
    } else {
        return w;
    }
    w.ok = true;
    return w;
}

static void wr32(std::vector<char>& v, uint32_t x) { const char* p = (const char*)&x; v.insert(v.end(), p, p + 4); }
static void wr16(std::vector<char>& v, uint16_t x) { const char* p = (const char*)&x; v.insert(v.end(), p, p + 2); }

static std::vector<char> build_wav16(const std::vector<float>& samples, int rate, int channels) {
    std::vector<char> v;
    uint32_t data_sz = (uint32_t)(samples.size() * 2);
    v.insert(v.end(), {'R','I','F','F'}); wr32(v, 36 + data_sz);
    v.insert(v.end(), {'W','A','V','E'});
    v.insert(v.end(), {'f','m','t',' '}); wr32(v, 16);
    wr16(v, 1); wr16(v, (uint16_t)channels); wr32(v, rate);
    wr32(v, rate * channels * 2); wr16(v, (uint16_t)(channels * 2)); wr16(v, 16);
    v.insert(v.end(), {'d','a','t','a'}); wr32(v, data_sz);
    for (float s : samples) {
        float c = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
        int16_t x = (int16_t)std::lround(c * 32767.0f);
        wr16(v, (uint16_t)x);
    }
    return v;
}

bool write_wav16(const std::string& path, const std::vector<float>& samples, int rate, int channels) {
    std::vector<char> v = build_wav16(samples, rate, channels);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(v.data(), (std::streamsize)v.size());
    return (bool)f;
}

std::vector<float> mixdown_mono(const WavData& w) {
    if (!w.ok || w.channels <= 0) return {};
    size_t frames = w.samples.size() / w.channels;
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; i++) {
        float acc = 0;
        for (int c = 0; c < w.channels; c++) acc += w.samples[i * w.channels + c];
        mono[i] = acc / w.channels;
    }
    return mono;
}

std::vector<float> resample_linear(const std::vector<float>& in, int from_rate, int to_rate) {
    if (in.empty() || from_rate <= 0 || to_rate <= 0) return {};
    if (from_rate == to_rate) return in;
    double ratio = (double)to_rate / from_rate;
    size_t out_n = (size_t)(in.size() * ratio);
    std::vector<float> out(out_n);
    for (size_t i = 0; i < out_n; i++) {
        double pos = i / ratio;
        size_t j = (size_t)pos;
        double frac = pos - j;
        float a = in[j];
        float b = (j + 1 < in.size()) ? in[j + 1] : a;
        out[i] = (float)(a + (b - a) * frac);
    }
    return out;
}

std::vector<float> to_mono16k(const WavData& w) {
    std::vector<float> mono = mixdown_mono(w);
    return resample_linear(mono, w.rate, 16000);
}

std::vector<uint8_t> wav16_bytes(const std::vector<float>& mono16k) {
    std::vector<char> v = build_wav16(mono16k, 16000, 1);
    return std::vector<uint8_t>(v.begin(), v.end());
}

} // namespace mc