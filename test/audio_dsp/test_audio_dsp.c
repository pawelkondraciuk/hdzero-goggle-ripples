// Host unit tests for the DVR audio DSP (src/record/audio_dsp.c).
// Build & run: make -C test/audio_dsp run   (or `make asan` for sanitizers)

#include "audio_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS 48000
#define FRAME 1024

static int failures;
static int checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static double db(double ratio) { return 20.0 * log10(ratio); }

// Analog 2nd-order Butterworth high-pass magnitude (bilinear warping is
// negligible at these frequencies for 44.1/48 kHz).
static double butter_hp_db(double f, double fc) {
    double r = f / fc;
    return db(r * r / sqrt(1.0 + r * r * r * r));
}

static void fill_sine(int16_t *buf, size_t n, int channels, int ch, double f, double amp, int fs, size_t offset) {
    for (size_t i = 0; i < n; i++)
        buf[i * channels + ch] = (int16_t)lrint(amp * sin(2.0 * M_PI * f * (double)(i + offset) / fs));
}

static double rms(const int16_t *buf, size_t n, int channels, int ch) {
    double acc = 0;
    for (size_t i = 0; i < n; i++) {
        double v = buf[i * channels + ch];
        acc += v * v;
    }
    return sqrt(acc / n);
}

// Steady-state gain in dB of a mono sine through the DSP, processed in
// recorder-sized frames.
static double measure_gain_db_clarity(int fs, int hpf, int clarity, double f) {
    AudioDsp_t dsp;
    audio_dsp_configure(&dsp, fs, 1, hpf, clarity);
    const size_t total = (size_t)fs * 3, settle = (size_t)fs * 2;
    int16_t *in = malloc(total * sizeof(int16_t));
    int16_t *out = malloc(total * sizeof(int16_t));
    fill_sine(in, total, 1, 0, f, 16000.0, fs, 0);
    memcpy(out, in, total * sizeof(int16_t));
    for (size_t pos = 0; pos < total; pos += FRAME) {
        size_t n = total - pos < FRAME ? total - pos : FRAME;
        audio_dsp_process_s16(&dsp, out + pos, n);
    }
    double g = db(rms(out + settle, total - settle, 1, 0) / rms(in + settle, total - settle, 1, 0));
    free(in);
    free(out);
    return g;
}

static double measure_gain_db(int fs, int hpf, double f) {
    return measure_gain_db_clarity(fs, hpf, 0, f);
}

static void test_hpf_response(void) {
    double g40 = measure_gain_db(FS, 120, 40);
    double g120 = measure_gain_db(FS, 120, 120);
    double g1k = measure_gain_db(FS, 120, 1000);
    printf("HPF 120 Hz @48k: 40 Hz %.2f dB (exp %.2f), 120 Hz %.2f dB (exp -3.01), 1 kHz %.3f dB\n",
           g40, butter_hp_db(40, 120), g120, g1k);
    // 1. 40 Hz clearly attenuated (~ -19.1 dB)
    CHECK(fabs(g40 - butter_hp_db(40, 120)) < 0.3, "40 Hz gain %.2f dB", g40);
    CHECK(g40 < -18.0, "40 Hz not attenuated enough: %.2f dB", g40);
    // 2. corner frequency follows Butterworth (-3.01 dB)
    CHECK(fabs(g120 + 3.01) < 0.2, "120 Hz gain %.2f dB", g120);
    // 3. 1 kHz practically unchanged
    CHECK(fabs(g1k) < 0.1, "1 kHz gain %.3f dB", g1k);

    // All selectable corners, plus slope checks one octave below/above.
    const int corners[] = {80, 120, 160, 200};
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        int fc = corners[i];
        double at = measure_gain_db(FS, fc, fc);
        double below = measure_gain_db(FS, fc, fc / 2.0);
        double above = measure_gain_db(FS, fc, fc * 4.0);
        printf("HPF %3d Hz: fc %.2f dB, fc/2 %.2f dB (exp %.2f), 4fc %.3f dB\n",
               fc, at, below, butter_hp_db(fc / 2.0, fc), above);
        CHECK(fabs(at + 3.01) < 0.2, "fc=%d gain at fc %.2f dB", fc, at);
        CHECK(fabs(below - butter_hp_db(fc / 2.0, fc)) < 0.3, "fc=%d gain at fc/2 %.2f dB", fc, below);
        CHECK(fabs(above - butter_hp_db(fc * 4.0, fc)) < 0.05, "fc=%d gain at 4fc %.3f dB", fc, above);
    }

    // 44.1 kHz capture keeps the same corner.
    double g441 = measure_gain_db(44100, 120, 120);
    CHECK(fabs(g441 + 3.01) < 0.2, "44.1 kHz gain at fc %.2f dB", g441);
}

// 4. Impulse: bounded, decays to exact zero (no limit cycle / instability).
static void test_impulse(void) {
    AudioDsp_t dsp;
    audio_dsp_configure(&dsp, FS, 1, 120, 0);
    const size_t n = FS; // 1 s
    int16_t *buf = calloc(n, sizeof(int16_t));
    buf[0] = 32767;
    for (size_t pos = 0; pos < n; pos += FRAME)
        audio_dsp_process_s16(&dsp, buf + pos, (n - pos) < FRAME ? n - pos : FRAME);
    int peak = 0;
    for (size_t i = 0; i < n; i++)
        if (abs(buf[i]) > peak)
            peak = abs(buf[i]);
    long tail = 0;
    for (size_t i = n / 2; i < n; i++)
        tail += abs(buf[i]);
    printf("impulse: peak %d, |tail| sum (0.5-1 s) %ld, out[0]=%d\n", peak, tail, buf[0]);
    CHECK(peak <= 32767, "impulse peak %d", peak);
    CHECK(buf[0] > 30000, "impulse onset %d", buf[0]);
    CHECK(tail == 0, "impulse tail did not decay: %ld", tail);
    free(buf);
}

// 5. Silence stays digital silence.
static void test_silence(void) {
    AudioDsp_t dsp;
    audio_dsp_configure(&dsp, FS, 2, 200, 0);
    int16_t buf[FRAME * 2];
    long nz = 0;
    for (int k = 0; k < 500; k++) {
        memset(buf, 0, sizeof(buf));
        audio_dsp_process_s16(&dsp, buf, FRAME);
        for (int i = 0; i < FRAME * 2; i++)
            nz += buf[i] != 0;
    }
    CHECK(nz == 0, "silence produced %ld non-zero samples", nz);
}

// Unclamped double reference using the same coefficients.
static void reference(const AudioDsp_t *cfg, const int16_t *in, double *out, size_t n) {
    double z1[AUDIO_DSP_MAX_STAGES] = {0}, z2[AUDIO_DSP_MAX_STAGES] = {0};
    for (size_t i = 0; i < n; i++) {
        double x = in[i];
        for (int s = 0; s < cfg->nbStages; s++) {
            const AudioBiquad_t *b = &cfg->stage[s];
            double y = b->b0 * x + z1[s];
            z1[s] = b->b1 * x - b->a1 * y + z2[s];
            z2[s] = b->b2 * x - b->a2 * y;
            x = y;
        }
        out[i] = x;
    }
}

// 6. Loud signal: full-scale square wave overshoots after the HPF; output
// must clamp to the rails without wrap-around, and match the reference
// everywhere else.
static void test_saturation(void) {
    AudioDsp_t dsp;
    audio_dsp_configure(&dsp, FS, 1, 200, 0);
    const size_t n = FS;
    int16_t *buf = malloc(n * sizeof(int16_t));
    double *ref = malloc(n * sizeof(double));
    for (size_t i = 0; i < n; i++)
        buf[i] = ((i / 240) & 1) ? -32768 : 32767; // 100 Hz full-scale square
    reference(&dsp, buf, ref, n);
    for (size_t pos = 0; pos < n; pos += FRAME)
        audio_dsp_process_s16(&dsp, buf + pos, (n - pos) < FRAME ? n - pos : FRAME);
    long clipped = 0, bad = 0;
    for (size_t i = 0; i < n; i++) {
        if (ref[i] >= 32767.0) {
            clipped++;
            bad += buf[i] != 32767;
        } else if (ref[i] <= -32768.0) {
            clipped++;
            bad += buf[i] != -32768;
        } else {
            bad += fabs(buf[i] - ref[i]) > 0.5 + 1e-9;
        }
    }
    printf("saturation: %ld clipped samples, %ld mismatches\n", clipped, bad);
    CHECK(clipped > 0, "square wave never overshot (test not exercising saturation)");
    CHECK(bad == 0, "%ld samples wrapped or deviated from reference", bad);
    free(buf);
    free(ref);
}

// 7. Off / invalid settings: inactive and bit-exact.
static void test_off(void) {
    const int settings[] = {0, 100, -5, 1000, 81};
    int16_t orig[FRAME], buf[FRAME];
    for (int i = 0; i < FRAME; i++)
        orig[i] = (int16_t)(rand() & 0xffff);
    for (size_t k = 0; k < sizeof(settings) / sizeof(settings[0]); k++) {
        AudioDsp_t dsp;
        bool active = audio_dsp_configure(&dsp, FS, 1, settings[k], 0);
        memcpy(buf, orig, sizeof(buf));
        audio_dsp_process_s16(&dsp, buf, FRAME);
        CHECK(!active, "hpf=%d should be inactive", settings[k]);
        CHECK(memcmp(buf, orig, sizeof(buf)) == 0, "hpf=%d modified the signal", settings[k]);
    }
    AudioDsp_t dsp;
    CHECK(!audio_dsp_configure(&dsp, FS, 3, 120, 0), "3 channels must be rejected");
    CHECK(!audio_dsp_configure(&dsp, FS, 0, 120, 0), "0 channels must be rejected");
    CHECK(!audio_dsp_configure(&dsp, 0, 1, 120, 0), "0 Hz sample rate must be rejected");
    CHECK(audio_dsp_hpf_valid(0) && audio_dsp_hpf_valid(80) && audio_dsp_hpf_valid(200) &&
              !audio_dsp_hpf_valid(100),
          "hpf validation");
}

// Stereo: channels filtered independently, interleaving respected.
static void test_stereo(void) {
    AudioDsp_t dsp;
    audio_dsp_configure(&dsp, FS, 2, 120, 0);
    const size_t n = FS * 2, settle = FS;
    int16_t *in = malloc(n * 2 * sizeof(int16_t));
    int16_t *out = malloc(n * 2 * sizeof(int16_t));
    fill_sine(in, n, 2, 0, 40, 16000, FS, 0);
    fill_sine(in, n, 2, 1, 1000, 16000, FS, 0);
    memcpy(out, in, n * 2 * sizeof(int16_t));
    for (size_t pos = 0; pos < n; pos += FRAME)
        audio_dsp_process_s16(&dsp, out + pos * 2, (n - pos) < FRAME ? n - pos : FRAME);
    double gl = db(rms(out + settle * 2, n - settle, 2, 0) / rms(in + settle * 2, n - settle, 2, 0));
    double gr = db(rms(out + settle * 2, n - settle, 2, 1) / rms(in + settle * 2, n - settle, 2, 1));
    printf("stereo: L 40 Hz %.2f dB, R 1 kHz %.3f dB\n", gl, gr);
    CHECK(fabs(gl - butter_hp_db(40, 120)) < 0.3, "stereo L %.2f dB", gl);
    CHECK(fabs(gr) < 0.1, "stereo R %.3f dB", gr);
    free(in);
    free(out);
}

// Reset: a reused instance behaves exactly like a fresh one (new recording).
static void test_reset(void) {
    int16_t a[FRAME], b[FRAME], warm[FRAME];
    fill_sine(warm, FRAME, 1, 0, 60, 20000, FS, 0);
    fill_sine(a, FRAME, 1, 0, 300, 12000, FS, 0);
    memcpy(b, a, sizeof(a));

    AudioDsp_t used, fresh;
    audio_dsp_configure(&used, FS, 1, 160, 0);
    audio_dsp_process_s16(&used, warm, FRAME);
    audio_dsp_reset(&used);
    audio_dsp_process_s16(&used, a, FRAME);

    audio_dsp_configure(&fresh, FS, 1, 160, 0);
    audio_dsp_process_s16(&fresh, b, FRAME);
    CHECK(memcmp(a, b, sizeof(a)) == 0, "reset did not clear filter history");
}

// 8. Long run: one hour of audio in recorder-sized frames. Gain must not
// drift, state must stay finite, and the tail must settle back to silence.
static void test_long_run(int clarity) {
    AudioDsp_t dsp;
    audio_dsp_configure(&dsp, FS, 1, 120, clarity);
    int16_t buf[FRAME];
    const size_t frames = (size_t)FS * 3600 / FRAME;
    unsigned seed = 12345;
    double first = 0, last = 0;
    size_t offset = 0;
    for (size_t k = 0; k < frames; k++, offset += FRAME) {
        for (int i = 0; i < FRAME; i++) {
            seed = seed * 1103515245u + 12345u;
            double noise = ((int)((seed >> 16) & 0x7fff) - 16384) / 8.0; // ~ -66 dBFS..
            buf[i] = (int16_t)lrint(12000.0 * sin(2.0 * M_PI * 1000.0 * (double)(offset + i) / FS) +
                                    6000.0 * sin(2.0 * M_PI * 50.0 * (double)(offset + i) / FS) + noise);
        }
        audio_dsp_process_s16(&dsp, buf, FRAME);
        if (k == 100)
            first = rms(buf, FRAME, 1, 0);
        if (k == frames - 1)
            last = rms(buf, FRAME, 1, 0);
    }
    bool finite = true;
    for (int s = 0; s < dsp.nbStages; s++)
        finite &= isfinite(dsp.z1[s][0]) && isfinite(dsp.z2[s][0]);
    // Settle back to silence.
    long nz = 0;
    for (int k = 0; k < 200; k++) {
        memset(buf, 0, sizeof(buf));
        audio_dsp_process_s16(&dsp, buf, FRAME);
        if (k >= 100)
            for (int i = 0; i < FRAME; i++)
                nz += buf[i] != 0;
    }
    printf("long run (1 h, clarity=%d): frame RMS start %.1f, end %.1f, settled non-zero %ld\n", clarity, first, last, nz);
    CHECK(finite, "filter state not finite after 1 h");
    CHECK(fabs(db(last / first)) < 0.1, "level drifted: %.1f -> %.1f", first, last);
    CHECK(nz == 0, "tail did not return to silence: %ld", nz);
}

static void test_clarity(void) {
    const double cuts[] = {0, -1.5, -3, -5};
    const int rates[] = {8000, 44100, 48000};
    for (int r = 0; r < 3; r++) {
        for (int level = 1; level <= 3; level++) {
            double low = measure_gain_db_clarity(rates[r], 0, level, 300);
            double presence = measure_gain_db_clarity(rates[r], 0, level, 3500);
            double bass = measure_gain_db_clarity(rates[r], 120, level, 40);
            printf("Clarity %d @%d: 300Hz %.2f dB, 3500Hz %.2f dB, HPF+40Hz %.2f dB\n",
                   level, rates[r], low, presence, bass);
            CHECK(fabs(low - (cuts[level] - level)) < 0.08, "low-mid EQ response");
            CHECK(fabs(presence) < 0.08, "presence boost must include fixed headroom");
            CHECK(presence - low > level * 2, "speech presence relative to low-mid");
            CHECK(bass < -19.0 - level, "combined HPF/clarity bass attenuation");
        }
    }
    CHECK(audio_dsp_clarity_valid(0) && audio_dsp_clarity_valid(3) &&
          !audio_dsp_clarity_valid(-1) && !audio_dsp_clarity_valid(4), "clarity validation");
    AudioDsp_t dsp, fresh;
    int16_t block[2048], reference[2048];
    CHECK(!audio_dsp_configure(&dsp, FS, 1, 0, 99), "invalid clarity defaults off");
    CHECK(audio_dsp_configure(&dsp, FS, 2, 0, 3), "clarity works independently of HPF");
    CHECK(dsp.nbStages == 2 && dsp.inputGain < 1, "EQ stages and headroom");
    for (int i = 0; i < 1024; i++) { block[2*i] = 32767; block[2*i+1] = 0; }
    audio_dsp_process_s16(&dsp, block, 1024);
    bool silent_right = true;
    for (int i = 0; i < 1024; i++) silent_right &= block[2*i+1] == 0;
    CHECK(silent_right, "clarity channel states must be independent");
    audio_dsp_reset(&dsp);
    audio_dsp_configure(&fresh, FS, 2, 0, 3);
    memset(block, 0, sizeof(block)); block[0] = 32767; block[1] = -32768;
    memcpy(reference, block, sizeof(block));
    audio_dsp_process_s16(&dsp, block, 1024);
    audio_dsp_process_s16(&fresh, reference, 1024);
    CHECK(!memcmp(block, reference, sizeof(block)), "clarity reset between recordings");
    // Combined chain: full-scale deterministic broadband input, then silence.
    audio_dsp_configure(&dsp, FS, 2, 200, 3);
    unsigned noise = 1;
    for (int k = 0; k < 500; k++) {
        for (int i = 0; i < 2048; i++) {
            noise = noise * 1664525u + 1013904223u;
            block[i] = (int16_t)(noise >> 16);
        }
        audio_dsp_process_s16(&dsp, block, 1024);
    }
    for (int k = 0; k < 200; k++) {
        memset(block, 0, sizeof(block));
        audio_dsp_process_s16(&dsp, block, 1024);
    }
    bool silence = true, finite = true;
    for (int i = 0; i < 2048; i++) silence &= block[i] == 0;
    for (int st = 0; st < dsp.nbStages; st++)
        for (int ch = 0; ch < 2; ch++)
            finite &= isfinite(dsp.z1[st][ch]) && isfinite(dsp.z2[st][ch]);
    CHECK(silence && finite, "combined loud-signal stability and decay to silence");
    audio_dsp_configure(&dsp, FS, 2, 0, 0);
    for (int i = 0; i < 2048; i++) block[i] = reference[i] = (int16_t)i;
    audio_dsp_process_s16(&dsp, block, 1024);
    CHECK(!memcmp(block, reference, sizeof(block)), "both filters Off after reconfiguration");
}

int main(void) {
    test_hpf_response();
    test_clarity();
    test_impulse();
    test_silence();
    test_saturation();
    test_off();
    test_stereo();
    test_reset();
    test_long_run(0);
    test_long_run(3);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
