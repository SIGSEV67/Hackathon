#include <math.h>

#include "audio.h"
#include "noise.h"

static volatile uint8_t s_level;
static volatile uint8_t s_peak;

static void on_samples(const int16_t *samples, size_t count, void *ctx)
{
    static float smoothed;

    int64_t sum = 0, sum_sq = 0;
    for (size_t i = 0; i < count; i++) {
        sum += samples[i];
        sum_sq += (int64_t)samples[i] * samples[i];
    }
    const float mean = (float)sum / count;
    const float var = (float)sum_sq / count - mean * mean;
    const float rms = var > 0 ? sqrtf(var) : 0;

    float level = (20.0f * log10f(rms + 1.0f) - 20.0f) * (100.0f / 60.0f);
    level = level < 0 ? 0 : (level > 100 ? 100 : level);
    smoothed = level > smoothed ? level : smoothed * 0.8f + level * 0.2f;

    s_level = (uint8_t)(smoothed + 0.5f);
    if (s_level > s_peak) {
        s_peak = s_level;
    }
}

esp_err_t noise_init(void)
{
    return audio_mic_add_sink(on_samples, NULL);
}

uint8_t noise_level(void)
{
    return s_level;
}

uint8_t noise_take_peak(void)
{
    const uint8_t peak = s_peak;
    s_peak = s_level;
    return peak;
}
