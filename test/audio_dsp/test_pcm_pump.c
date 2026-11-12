// Exercise the actual PCM pump against the shipped SDK declarations, with
// deterministic MPI mocks. This does not emulate the hardware libraries.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../src/record/ai2aenc.c"

static Ai2Aenc_t aa;
static int16_t input[2048], original[2048];
static AUDIO_FRAME_S frame;
static int sends, releases, failures, gets, timeout_first;
static bool expect_filtered;

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) { return 0; }

ERRORTYPE AW_MPI_AI_GetFrame(AUDIO_DEV dev, AI_CHN chn, AUDIO_FRAME_S *out, AEC_FRAME_S *aec, int timeout) {
    assert(timeout > 0 && timeout <= 200);
    if (timeout_first && gets++ == 0) return FAILURE;
    *out = frame;
    return SUCCESS;
}

ERRORTYPE AW_MPI_AENC_SendFrame(AENC_CHN chn, AUDIO_FRAME_S *out, AEC_FRAME_S *aec) {
    sends++;
    assert(out->mTimeStamp == frame.mTimeStamp && out->tmp_pts == frame.tmp_pts);
    assert(out->mLen == frame.mLen && out->mSeq == frame.mSeq && out->mId == frame.mId);
    assert(memcmp(input, original, sizeof(input)) == 0); // AI buffer never modified
    if (expect_filtered) {
        assert(out->mpAddr == aa.pcmScratch);
        assert(memcmp(out->mpAddr, input, frame.mLen) != 0);
    } else assert(out->mpAddr == frame.mpAddr);
    return sends <= failures ? FAILURE : SUCCESS;
}

ERRORTYPE AW_MPI_AI_ReleaseFrame(AUDIO_DEV dev, AI_CHN chn, AUDIO_FRAME_S *out, AEC_FRAME_S *aec) {
    assert(out->mpAddr == frame.mpAddr);
    releases++;
    __atomic_store_n(&aa.bExit, true, __ATOMIC_RELEASE);
    return SUCCESS;
}

static void setup(int channels) {
    memset(&aa, 0, sizeof(aa));
    assert(audio_dsp_configure(&aa.dsp, 48000, channels, 120, 0));
    for (int i = 0; i < 2048; i++) input[i] = 16000;
    memcpy(original, input, sizeof(input));
    frame = (AUDIO_FRAME_S){.mpAddr = input, .mLen = sizeof(input),
        .mBitwidth = AUDIO_BIT_WIDTH_16,
        .mSoundmode = channels == 2 ? AUDIO_SOUND_MODE_STEREO : AUDIO_SOUND_MODE_MONO,
        .mTimeStamp = 123456, .tmp_pts = 987654, .mSeq = 42, .mId = 7};
    sends = releases = failures = gets = timeout_first = 0;
    expect_filtered = true;
}

int main(void) {
    setup(1); ai2aenc_pcmProc(&aa);
    assert(sends == 1 && releases == 1 && aa.nbPcmFrames == 1);
    setup(2); timeout_first = 1; failures = 2; ai2aenc_pcmProc(&aa);
    assert(sends == 3 && releases == 1 && aa.nbPcmFrames == 1);
    setup(1); failures = 100; ai2aenc_pcmProc(&aa);
    assert(sends == 4 && releases == 1 && aa.nbPcmDropped == 1);
    setup(1); frame.mLen = 3; expect_filtered = false; ai2aenc_pcmProc(&aa);
    assert(releases == 1 && aa.nbPcmPassthrough == 1);
    setup(1); frame.mBitwidth = AUDIO_BIT_WIDTH_32; expect_filtered = false; ai2aenc_pcmProc(&aa);
    assert(releases == 1 && aa.nbPcmPassthrough == 1);
    setup(1); frame.mSoundmode = AUDIO_SOUND_MODE_STEREO; expect_filtered = false; ai2aenc_pcmProc(&aa);
    assert(releases == 1 && aa.nbPcmPassthrough == 1);
    setup(1); assert(audio_dsp_configure(&aa.dsp, 48000, 1, 0, 3));
    ai2aenc_pcmProc(&aa);
    assert(sends == 1 && releases == 1 && aa.nbPcmFrames == 1);
    setup(1); frame.mpAddr = NULL; ai2aenc_pcmProc(&aa);
    assert(sends == 0 && releases == 1 && aa.nbPcmDropped == 1);
    puts("PCM pump: mono/stereo, timestamps, private copy, timeout, retry, drop and format guards passed");
}
