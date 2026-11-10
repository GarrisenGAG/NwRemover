#include "Dsp.h"
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

namespace mc {

static const double PI = 3.14159265358979323846;

static float read_sample(const float* s, int64_t n, int ch, int c, double pos) {
    if (pos < 0) pos = 0;
    if (pos > n - 1) pos = (double)(n - 1);
    int64_t i = (int64_t)pos;
    double frac = pos - i;
    float a = s[i * ch + c];
    float b = (i + 1 < n) ? s[(i + 1) * ch + c] : a;
    return (float)(a + (b - a) * frac);
}

static void edge_fade(float* dst, int64_t n, int ch, double rate, float fade_ms) {
    int64_t f = (int64_t)(fade_ms * 0.001 * rate);
    if (f <= 0) return;
    if (f > n / 2) f = n / 2;
    for (int64_t i = 0; i < f; i++) {
        float g = (float)i / (float)f;
        for (int c = 0; c < ch; c++) {
            dst[i * ch + c] *= g;
            dst[(n - 1 - i) * ch + c] *= g;
        }
    }
}

static void granular_pitch(const float* src, float* dst, int64_t n, int ch,
                           double rate, float semi) {
    double ratio = std::pow(2.0, semi / 12.0);
    if (ratio < 0.25) ratio = 0.25;
    if (ratio > 4.0) ratio = 4.0;
    int64_t grain = (int64_t)(rate * 0.05);
    if (grain < 64) grain = 64;
    if (grain > n) grain = n;
    int64_t hop = grain / 2;
    if (hop < 1) hop = 1;
    std::memset(dst, 0, sizeof(float) * (size_t)n * ch);
    std::vector<float> win(grain);
    for (int64_t j = 0; j < grain; j++)
        win[j] = (float)(0.5 - 0.5 * std::cos(2.0 * PI * j / (grain - 1)));
    for (int c = 0; c < ch; c++) {
        for (int64_t o = 0; o < n; o += hop) {
            double rp = o * ratio;
            for (int64_t j = 0; j < grain && o + j < n; j++) {
                float v = read_sample(src, n, ch, c, rp + j * ratio);
                dst[(o + j) * ch + c] += v * win[j];
            }
        }
    }
}

void process_interval(const float* src, float* dst, int64_t n, int ch,
                      double rate, const DspParams& p) {
    if (n <= 0 || ch <= 0) return;
    switch (p.mode) {
    case MODE_MUTE:
        std::memset(dst, 0, sizeof(float) * (size_t)n * ch);
        return;
    case MODE_REVERSE:
        for (int64_t i = 0; i < n; i++)
            for (int c = 0; c < ch; c++)
                dst[i * ch + c] = src[(n - 1 - i) * ch + c];
        edge_fade(dst, n, ch, rate, p.fade_ms);
        return;
    case MODE_PITCH:
        granular_pitch(src, dst, n, ch, rate, p.pitch_semi);
        edge_fade(dst, n, ch, rate, p.fade_ms);
        return;
    case MODE_BEEP:
    default: {
        double phase = 0.0;
        float level = std::max(0.0f, std::min(1.0f, p.beep_level));
        float freq = std::max(20.0f, p.beep_freq);
        for (int64_t i = 0; i < n; i++) {
            double f = freq * std::pow(2.0, (p.beep_sweep * (double)i / (double)n) / 12.0);
            phase += f / rate;
            phase -= std::floor(phase);
            float v;
            switch (p.beep_wave) {
            case WAVE_SQUARE: v = (phase < 0.5) ? 1.0f : -1.0f; break;
            case WAVE_SAW:    v = (float)(2.0 * phase - 1.0); break;
            default:          v = (float)std::sin(2.0 * PI * phase); break;
            }
            v *= level;
            for (int c = 0; c < ch; c++) dst[i * ch + c] = v;
        }
        edge_fade(dst, n, ch, rate, p.fade_ms);
        return;
    }
    }
}

void process_custom(const float* sound, int64_t sound_n, float* dst, int64_t n,
                    int ch, double rate, float level, float fade_ms) {
    if (n <= 0 || ch <= 0) return;
    if (!sound || sound_n <= 0) { std::memset(dst, 0, sizeof(float) * (size_t)n * ch); return; }
    float g = std::max(0.0f, std::min(1.0f, level));
    for (int64_t i = 0; i < n; i++) {
        float v = sound[i % sound_n] * g;
        for (int c = 0; c < ch; c++) dst[i * ch + c] = v;
    }
    edge_fade(dst, n, ch, rate, fade_ms);
}

} // namespace mc