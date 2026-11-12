#include "audio_dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// State magnitudes below this are flushed to zero so a decaying tail never
// lingers in (slow) denormal arithmetic. Far below 1 LSB of 16-bit audio.
#define AUDIO_DSP_TINY 1e-15

bool audio_dsp_hpf_valid(int hz) {
    return hz == 0 || hz == 80 || hz == 120 || hz == 160 || hz == 200;
}

bool audio_dsp_clarity_valid(int level) {
    return level >= 0 && level <= 3;
}

// RBJ Audio EQ Cookbook high-pass; Q = 1/sqrt(2) gives a 2nd-order
// Butterworth response (-3 dB at fc, 12 dB/octave below).
static void biquad_highpass(AudioBiquad_t *bq, double fs, double fc) {
    const double q = 0.70710678118654752440;
    const double w0 = 2.0 * M_PI * fc / fs;
    const double cw = cos(w0);
    const double alpha = sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;

    bq->b0 = ((1.0 + cw) / 2.0) / a0;
    bq->b1 = (-(1.0 + cw)) / a0;
    bq->b2 = ((1.0 + cw) / 2.0) / a0;
    bq->a1 = (-2.0 * cw) / a0;
    bq->a2 = (1.0 - alpha) / a0;
}

// RBJ peaking EQ, normalized to a0=1. Gains are relative to the fixed
// headroom applied before the chain; no AGC or signal-dependent attenuation.
static void biquad_peak(AudioBiquad_t *bq, double fs, double fc, double q, double gainDb) {
    const double a = pow(10.0, gainDb / 40.0);
    const double w = 2.0 * M_PI * fc / fs;
    const double alpha = sin(w) / (2.0 * q);
    const double cw = cos(w);
    const double a0 = 1.0 + alpha / a;
    bq->b0 = (1.0 + alpha * a) / a0;
    bq->b1 = -2.0 * cw / a0;
    bq->b2 = (1.0 - alpha * a) / a0;
    bq->a1 = -2.0 * cw / a0;
    bq->a2 = (1.0 - alpha / a) / a0;
}

void audio_dsp_reset(AudioDsp_t *dsp) {
    memset(dsp->z1, 0, sizeof(dsp->z1));
    memset(dsp->z2, 0, sizeof(dsp->z2));
}

bool audio_dsp_configure(AudioDsp_t *dsp, int sampleRate, int channels, int hpfHz, int clarity) {
    memset(dsp, 0, sizeof(*dsp));
    dsp->sampleRate = sampleRate;
    dsp->channels = channels;
    dsp->inputGain = 1.0;

    if (channels < 1 || channels > AUDIO_DSP_MAX_CHANNELS)
        return false;
    if (sampleRate < 8000 || sampleRate > 192000)
        return false;
    if (!audio_dsp_hpf_valid(hpfHz))
        hpfHz = 0;
    // Keep the corner well below Nyquist (only matters for 8 kHz capture).
    if (hpfHz > 0 && hpfHz * 4 < sampleRate) {
        biquad_highpass(&dsp->stage[dsp->nbStages++], sampleRate, hpfHz);
        dsp->hpfHz = hpfHz;
    }

    if (audio_dsp_clarity_valid(clarity) && clarity > 0) {
        static const double cutDb[] = {0.0, -1.5, -3.0, -5.0};
        const double boostDb = clarity; // Low +1, Medium +2, High +3 dB
        biquad_peak(&dsp->stage[dsp->nbStages++], sampleRate, 300.0, 0.8, cutDb[clarity]);
        biquad_peak(&dsp->stage[dsp->nbStages++], sampleRate, 3500.0, 0.9, boostDb);
        dsp->inputGain = pow(10.0, -boostDb / 20.0);
        dsp->clarity = clarity;
    }

    dsp->active = dsp->nbStages > 0;
    return dsp->active;
}

static inline int16_t saturate_s16(double v) {
    if (v >= 32767.0)
        return 32767;
    if (v <= -32768.0)
        return -32768;
    return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

void audio_dsp_process_s16(AudioDsp_t *dsp, int16_t *pcm, size_t frames) {
    if (!dsp->active || pcm == NULL)
        return;

    const int channels = dsp->channels;
    const int stages = dsp->nbStages;

    for (int ch = 0; ch < channels; ch++) {
        int16_t *p = pcm + ch;
        for (size_t i = 0; i < frames; i++, p += channels) {
            double x = *p * dsp->inputGain;
            for (int s = 0; s < stages; s++) {
                const AudioBiquad_t *bq = &dsp->stage[s];
                double y = bq->b0 * x + dsp->z1[s][ch];
                dsp->z1[s][ch] = bq->b1 * x - bq->a1 * y + dsp->z2[s][ch];
                dsp->z2[s][ch] = bq->b2 * x - bq->a2 * y;
                x = y;
            }
            *p = saturate_s16(x);
        }
        // Flush once per block: cheap and enough to stop denormal tails.
        for (int s = 0; s < stages; s++) {
            if (fabs(dsp->z1[s][ch]) < AUDIO_DSP_TINY)
                dsp->z1[s][ch] = 0.0;
            if (fabs(dsp->z2[s][ch]) < AUDIO_DSP_TINY)
                dsp->z2[s][ch] = 0.0;
        }
    }
}
