/* psprecomp — audio content histogram + the guest audio sink.
 *
 * Diagnosis aid for partial/absent audio in a static recompile. While a guest
 * thread feeds a channel's mixer, we bucket every interleave's sample
 * magnitudes into a global distribution. Two questions the game's audio never
 * tells the host on its own become answerable from here:
 *
 *   1. Is anything actually reaching a channel at all? A channel that only ever
 *      sees zeroed buffers (stub decoders) has a flat histogram; a real BGM or
 *      preview track fills it.
 *   2. Is the sample rate / block pacing in sync? Compare the bucket spacing
 *      (frames) against the rate the game expects: 44100 Hz here.
 *
 * psp_audio_output is the one host sink every guest audio path funnels into, so
 * the histogram and the guest's own block scheduler can both leave the transport
 * here. It owns an SDL2 stream device at 44.1 kHz / int16 / stereo (the PSP's
 * 2-channel 44.1 kHz, int16 output). Self-initialised on the first call. Non
 * blocking: on a full device SDL_QueueAudio simply drops the frame, so the guest
 * can never stall on a silent host.
 *
 * The histogram is deliberate dead weight on the hot path: sascore.c already
 * counts liveness via g_samples_nonzero. This is a separate, opt-in
 * proof-of-content hook. Only the two histogram functions do work; each returns
 * 0 on the common path so callers simply fall through.
 */
#include "psprecomp/audio_hist.h"
#include "psprecomp/mem.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <pthread.h>

#define HIST_BINS   512        /* magnitude buckets */
#define FULL_SCALE 32767u       /* int16 peak; dBFS reference */
#define INT_GRAN   8u           /* quantise magnitude before bucketing */

/* --- psp_audio_output: the guest audio sink -------------------------------- */

#define AUDIO_OUT_FREQ 44100    /* the PSP's 44.1 kHz soundtrack rate */
#define AUDIO_OUT_CH   2        /* the PSP output is stereo */
#define AUDIO_OUT_FMT  AUDIO_S16SYS

static uint64_t g_audio_blocks;   /* guest frames handed to SDL via psp_audio_output */
static uint64_t g_audio_maxblocks;

static uint64_t g_mag_min = FULL_SCALE;  /* smallest peak magnitude we have seen */
static uint64_t g_mag_max;        /* largest peak magnitude we have seen */
static uint64_t g_mag_sum;        /* summed per-frame peak magnitudes, for a mean */
static uint64_t g_calls;          /* sample frames folded into the histo */
static uint64_t g_dropped;        /* blocks dropped because the host queue was full */
static uint64_t g_underruns[9];   /* per channel: blocks that found the host queue empty */
static uint64_t g_ch_blocks[9];   /* per channel: blocks written */
static uint32_t g_ch_samples[9];  /* per channel: last block size in samples */

/* Two independent host streams so the guest's two audio threads never
 * interleave blocks in one queue: [0] = sceAudio channels 0-7 (SasCore effects,
 * keysounds), [1] = channel 8 (sceVaudio / BGM). SDL mixes them on the host. */
#define NSTREAMS 9   /* one host queue per guest channel: the SDL mixes them */
#define TARGET_QUEUED_BYTES (AUDIO_OUT_FREQ * 4 / 10)  /* ~100 ms target latency */
#define MAX_QUEUED_BYTES    (AUDIO_OUT_FREQ * 4 * 1)   /* 1.0s maximum queue headroom before drop */

static SDL_AudioDeviceID audio_dev[NSTREAMS];
static int audio_state;                         /* 0 untried, 1 ok, -1 failed */
static int audio_dev_failed[NSTREAMS];
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

void psprecomp_audio_reset(void) {
    g_audio_blocks = 0;
    g_audio_maxblocks = 0;
    g_mag_min = FULL_SCALE;
    g_mag_max = 0;
    g_mag_sum = 0;
    g_calls = 0;
    g_dropped = 0;
    for (int c = 0; c < 9; c++) g_underruns[c] = g_ch_blocks[c] = g_ch_samples[c] = 0;
}

void psprecomp_audio_report(void) {
    double mean = g_calls ? (double)g_mag_sum / (double)g_calls : 0.0;
    for (int c = 0; c < 9; c++)
        if (g_ch_blocks[c])
            fprintf(stderr, "psprecomp_audio_report: ch%d blocks=%llu samples/block=%u underruns=%llu\n",
                    c, (unsigned long long)g_ch_blocks[c], g_ch_samples[c],
                    (unsigned long long)g_underruns[c]);
    fprintf(stderr, "psprecomp_audio_report: blocks=%llu dropped=%llu maxblocks=%llu frames=%llu "
                "mean_dBFS=%.1f max_dBFS=%.1f min_dBFS=%.1f\n",
            (unsigned long long)g_audio_blocks,
            (unsigned long long)g_dropped,
            (unsigned long long)g_audio_maxblocks,
            (unsigned long long)g_calls,
            mean > 0.0 ? 20.0 * log10(mean / (double)FULL_SCALE) : -INFINITY,
            g_mag_max ? 20.0 * log10((double)g_mag_max / (double)FULL_SCALE) : -INFINITY,
            g_mag_min ? 20.0 * log10((double)g_mag_min / (double)FULL_SCALE) : -INFINITY);
}

/* Open the SDL audio subsystem once, and one device per guest channel on first
 * use. Blocks from two channels must not share a queue: SDL_QueueAudio only
 * appends, so channels sharing a queue play one after the other (silence from
 * a mixer thread interleaved into a video's audio) instead of being mixed. */
static int audio_dev_open(int idx) {
    pthread_mutex_lock(&g_init_lock);
    if (audio_state == 0) {
        audio_state = -1;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "psprecomp: SDL_InitSubSystem(AUDIO) failed (%s)\n", SDL_GetError());
        } else {
            audio_state = 1;
        }
    }
    int ok = audio_state == 1;
    if (ok && !audio_dev[idx]) {
        SDL_AudioSpec want = {
            .freq = AUDIO_OUT_FREQ,
            .format = AUDIO_OUT_FMT,
            .channels = AUDIO_OUT_CH,
            .samples = 1024,
        };
        SDL_AudioSpec got = { 0 };
        audio_dev[idx] = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (!audio_dev[idx]) {
            fprintf(stderr, "psprecomp: SDL_OpenAudioDevice(ch %d) failed (%s)\n", idx, SDL_GetError());
            audio_dev_failed[idx] = 1;
        } else {
            SDL_PauseAudioDevice(audio_dev[idx], 0);   /* devices open paused */
        }
    }
    ok = audio_dev[idx] != 0;
    pthread_mutex_unlock(&g_init_lock);
    return ok;
}

/* PSP volumes run 0..0x8000 (unity). */
static int scale_vol(int v) { return v < 0 ? 0 : v > 0x8000 ? 0x8000 : v; }

void psp_audio_output(int channel, int leftvol, int rightvol,
                      const int16_t *buf, int samples) {
    if (!buf || samples <= 0) return;

    const int di = channel < 0 ? 0 : channel >= NSTREAMS ? NSTREAMS - 1 : channel;
    if (audio_dev_failed[di]) return;
    if (!audio_dev[di] && !audio_dev_open(di)) return;

    const SDL_AudioDeviceID dev = audio_dev[di];
    const int lv = scale_vol(leftvol), rv = scale_vol(rightvol);
    const uint64_t bytes = (uint64_t)samples * 4;

    /* Never stall the guest on a slow host, but give generous buffer (1.0s)
     * so that thread scheduling fluctuations never cause crackling or dropped blocks. */
    if (SDL_GetQueuedAudioSize(dev) > MAX_QUEUED_BYTES) { g_dropped++; return; }

    const int ci = channel < 0 ? 0 : channel > 8 ? 8 : channel;
    g_ch_blocks[ci]++; g_ch_samples[ci] = (uint32_t)samples;
    if (g_ch_blocks[ci] > 8 && SDL_GetQueuedAudioSize(dev) == 0)
        g_underruns[ci]++;
    int16_t stack_buf[4096 * 2];
    int16_t *tmp = samples <= 4096 ? stack_buf : (int16_t *)malloc((size_t)bytes);
    if (!tmp) return;
    uint64_t peak = 0, sum = 0;
    for (int i = 0; i < samples; i++) {
        int l = ((int)buf[2 * i]     * lv) >> 15;
        int r = ((int)buf[2 * i + 1] * rv) >> 15;
        tmp[2 * i]     = (int16_t)l;
        tmp[2 * i + 1] = (int16_t)r;
        uint64_t m = (uint64_t)abs(l), mr = (uint64_t)abs(r);
        if (mr > m) m = mr;
        sum += m;
        if (m > peak) peak = m;
    }
    SDL_QueueAudio(dev, tmp, (uint32_t)bytes);
    if (tmp != stack_buf) free(tmp);

    /* Histogram (diagnostic only; racy counters are fine across two threads). */
    g_mag_sum += sum;
    g_calls += (uint64_t)samples;
    if (peak > g_mag_max) g_mag_max = peak;
    if (peak < g_mag_min) g_mag_min = peak;
    g_audio_blocks++;
    if (peak > g_audio_maxblocks) g_audio_maxblocks = peak;
}

int audio_passing_blocks(void) {
    return (int)g_audio_blocks;
}
