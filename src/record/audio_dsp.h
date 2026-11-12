#ifndef __AUDIO_DSP_H_
#define __AUDIO_DSP_H_

/*
 * Lightweight PCM filtering for the DVR audio track.
 *
 * Pure C, no MPP dependencies, no allocation: the recorder embeds one
 * AudioDsp_t, configures it once per recording and runs it on each captured
 * frame before the AAC encoder. Coefficients are computed in
 * audio_dsp_configure(), never per sample.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_DSP_MAX_CHANNELS 2
#define AUDIO_DSP_MAX_STAGES   4

typedef struct {
    double b0, b1, b2, a1, a2; // normalized (a0 == 1)
} AudioBiquad_t;

typedef struct {
    bool active;
    int sampleRate;
    int channels;
    int hpfHz;
    int clarity; // 0=off, 1=low, 2=medium, 3=high
    double inputGain; // fixed headroom for the presence boost
    int nbStages;
    AudioBiquad_t stage[AUDIO_DSP_MAX_STAGES];
    // Transposed direct form II state, per stage and channel.
    double z1[AUDIO_DSP_MAX_STAGES][AUDIO_DSP_MAX_CHANNELS];
    double z2[AUDIO_DSP_MAX_STAGES][AUDIO_DSP_MAX_CHANNELS];
} AudioDsp_t;

// Returns true when hz is one of the supported DVR high-pass corners
// (0 = off, 80, 120, 160, 200).
bool audio_dsp_hpf_valid(int hz);
bool audio_dsp_clarity_valid(int level);

// Configure HPF and clarity independently; invalid settings mean off.
// Both off or an unsupported format leave DSP inactive. Returns dsp->active.
bool audio_dsp_configure(AudioDsp_t *dsp, int sampleRate, int channels, int hpfHz, int clarity);

// Clear the filter history (start of every recording).
void audio_dsp_reset(AudioDsp_t *dsp);

// Filter interleaved signed 16-bit samples in place. frames = samples per
// channel. No-op when the DSP is inactive.
void audio_dsp_process_s16(AudioDsp_t *dsp, int16_t *pcm, size_t frames);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_DSP_H_ */
