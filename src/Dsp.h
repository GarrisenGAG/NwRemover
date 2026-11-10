// Audio replacement DSP: beep / mute / reverse / granular pitch shift.
#pragma once
#include <cstdint>

namespace mc {

enum ReplaceMode { MODE_BEEP = 1, MODE_MUTE = 2, MODE_REVERSE = 3, MODE_PITCH = 4, MODE_CUSTOM = 5 };
enum BeepWave { WAVE_SINE = 0, WAVE_SQUARE = 1, WAVE_SAW = 2 };

struct DspParams {
    int   mode       = MODE_BEEP;
    float beep_freq  = 1000.0f;  // Hz
    float beep_level = 0.25f;    // 0..1 amplitude
    int   beep_wave  = WAVE_SINE;
    float beep_sweep = 0.0f;     // semitones glide across interval
    float pitch_semi = -5.0f;    // semitones for MODE_PITCH
    float fade_ms    = 5.0f;     // edge fade to avoid clicks
};

// src/dst: interleaved float, n frames, ch channels. dst must not alias src.
void process_interval(const float* src, float* dst, int64_t n, int ch,
                      double rate, const DspParams& p);

// Fill dst by looping a mono replacement sound (already resampled to rate).
void process_custom(const float* sound, int64_t sound_n, float* dst, int64_t n,
                    int ch, double rate, float level, float fade_ms);

} // namespace mc