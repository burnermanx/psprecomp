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
 * psp_audio_output is the one host sink every guest audio path funnels into.
 * It owns ONE SDL2 device at 44.1 kHz / int16 / stereo and a ring per guest
 * channel; the device callback sums the rings and saturates to int16. That is
 * what lets two guest threads (keysounds and BGM) mix instead of queueing one
 * after the other, and what keeps the host from inserting silence between two
 * blocks of sound: silence only leaves the callback when a ring is empty.
 *
 * The histogram is deliberate dead weight on the hot path: sascore.c already
 * counts liveness via g_samples_nonzero. This is a separate, opt-in
 * proof-of-content hook.
 */
#include "psprecomp/audio_hist.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "host_clock.h"

#define HIST_BINS   512        /* magnitude buckets */
#define FULL_SCALE 32767u       /* int16 peak; dBFS reference */
#define INT_GRAN   8u           /* quantise magnitude before bucketing */

/* --- psp_audio_output: the guest audio sink -------------------------------- */

#define AUDIO_OUT_FREQ 44100    /* the PSP's 44.1 kHz soundtrack rate */
#define AUDIO_OUT_CH   2        /* the PSP output is stereo */
#define AUDIO_OUT_FMT  AUDIO_S16SYS
#define AUDIO_CHANNELS 9        /* sceAudio 0-7 plus sceVaudio's channel 8 */
#define RING_SAMPLES   AUDIO_OUT_FREQ   /* 1 s per channel; the guest paces */
#define JITTER_SAMPLES (AUDIO_OUT_FREQ * 8 / 1000)   /* measured thread jitter */
#define MARGIN_SAMPLES (AUDIO_OUT_FREQ * 10 / 1000)  /* safety margin */
#define END_TIMEOUT_US 100000u  /* a silent channel is considered finished */

typedef struct {
    int16_t *buf;               /* RING_SAMPLES stereo frames */
    uint32_t head, tail, count;
    int active;                 /* prefill done, callback may consume */
    int stopping;               /* released; drain, then stop */
    int dry;                    /* already counted the current empty episode */
    uint32_t target;            /* prefill level in frames */
    uint32_t block;             /* last block size in frames */
    uint32_t trim;              /* extra frames this channel may queue past the target */
    double lat_sum;             /* sum over samples of the queue ahead of each, frames */
    uint64_t lat_n;             /* samples counted in lat_sum (trace window) */
    uint64_t last_fill_us;
    uint64_t blocks, underruns, silence, dropped;
    uint64_t level_sum, level_n;
    uint32_t level_min, level_max;
} audio_ring;

static audio_ring g_ring[AUDIO_CHANNELS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_dev;
static int g_dev_state;         /* 0 untried, 1 ok, -1 failed */
static int g_dev_samples = 1024;
static uint32_t g_target;       /* common prefill for every channel, frames */

static uint64_t g_audio_blocks;   /* guest frames handed to the sink */
static uint64_t g_audio_maxblocks;
static uint64_t g_mag_min = FULL_SCALE;
static uint64_t g_mag_max;
static uint64_t g_mag_sum;
static uint64_t g_calls;

/* --- trace and pacing state (S1/S2) --------------------------------------- */

static int g_trace = -1;                    /* -1 unread, 0 off, 1 on */
static uint64_t g_last_cb_us;               /* last device callback */
static uint64_t g_last_report_us;
static uint64_t g_start_us;
static uint32_t g_cb_min[AUDIO_CHANNELS], g_cb_max[AUDIO_CHANNELS];
static uint32_t g_cb_n[AUDIO_CHANNELS];
static uint64_t g_underruns_play[AUDIO_CHANNELS], g_underruns_other[AUDIO_CHANNELS];
static uint64_t g_due_us[AUDIO_CHANNELS];   /* host-clock grid, the fallback */

static uint64_t host_now_us(void) {
    return host_clock_us();
}

static int trace_on(void) {
    if (g_trace < 0) {
        const char *e = getenv("EMOTIONALSENSE_TRACE_AUDIO");
        g_trace = e && e[0] && e[0] != '0';
    }
    return g_trace;
}

static uint32_t g_scene_last;
static uint64_t g_scene_changed_us;

/* Reads the scene word and remembers when it changed: an underrun in the
 * first second of a scene is the scene starting, not the song losing audio. */
static uint32_t scene_now(uint64_t now) {
    uint32_t s = psp_scene_now();
    if (s != g_scene_last) {
        g_scene_last = s;
        g_scene_changed_us = now;
    }
    return s;
}

static int16_t clamp16(int32_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* The device callback: sum every active ring into the output, saturating.
 * A ring is consumed only after its prefill target; if it runs dry while the
 * guest is still feeding it, that is a real underrun and the channel re-arms.
 * A ring that stops receiving blocks drains silently and is then retired. */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    memset(out, 0, (size_t)len);
    uint64_t now = host_now_us();
    uint32_t scene = scene_now(now);

    pthread_mutex_lock(&g_lock);
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        audio_ring *r = &g_ring[ch];
        if (!r->active) continue;
        for (int i = 0; i < frames; i++) {
            if (r->count == 0) {
                if (r->stopping || now - r->last_fill_us > END_TIMEOUT_US) {
                    r->active = 0;
                    r->stopping = 0;
                    r->dry = 0;
                } else {
                    r->silence += (uint64_t)(frames - i);
                    if (!r->dry) {
                        /* One line per dry episode, and the channel stays
                         * armed: the next block plays at once instead of
                         * waiting for the whole prefill again. That wait was
                         * the click in the middle of a long keysound. */
                        r->dry = 1;
                        r->underruns++;
                        if (scene == 3u && now - g_scene_changed_us > 1000000u)
                            g_underruns_play[ch]++;
                        else g_underruns_other[ch]++;
                        if (trace_on())
                            fprintf(stderr,
                                    "audio: underrun ch%d vblank=%llu scene=%u level=%u target=%u\n",
                                    ch, (unsigned long long)psp_display_vblanks(),
                                    scene,
                                    r->count, r->target);
                    }
                }
                break;
            }
            int32_t l = (int32_t)out[2 * i] + r->buf[2 * r->tail];
            int32_t rr = (int32_t)out[2 * i + 1] + r->buf[2 * r->tail + 1];
            out[2 * i] = clamp16(l);
            out[2 * i + 1] = clamp16(rr);
            r->tail = (r->tail + 1) % RING_SAMPLES;
            r->count--;
        }
        if (r->level_n == 0 || r->count < r->level_min) r->level_min = r->count;
        if (r->count > r->level_max) r->level_max = r->count;
        r->level_sum += r->count;
        r->level_n++;
        if (trace_on()) {
            uint32_t f = (uint32_t)frames;
            if (!g_cb_n[ch]) { g_cb_min[ch] = f; g_cb_max[ch] = f; }
            if (f < g_cb_min[ch]) g_cb_min[ch] = f;
            if (f > g_cb_max[ch]) g_cb_max[ch] = f;
            g_cb_n[ch]++;
        }
    }
    pthread_mutex_unlock(&g_lock);

    g_last_cb_us = now;
    if (trace_on()) {
        if (!g_start_us) g_start_us = now;
        if (!g_last_report_us) g_last_report_us = now;
        if (now - g_last_report_us >= 1000000u) {
            g_last_report_us = now;
            /* Counts read outside the lock: this is a diagnostic line, not a
             * measurement that has to be exact to the frame. */
            for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
                if (!g_cb_n[ch]) continue;
                fprintf(stderr,
                        "audio[%llus] ch%d level=%u/%u cb=%u/%u n=%u underruns=%llu scene=%u lat=%.1f\n",
                        (unsigned long long)((now - g_start_us) / 1000000u), ch,
                        g_ring[ch].count, g_target,
                        g_cb_min[ch], g_cb_max[ch], g_cb_n[ch],
                        (unsigned long long)g_ring[ch].underruns, scene,
                        g_ring[ch].lat_n ? g_ring[ch].lat_sum / (double)g_ring[ch].lat_n : 0.0);
                g_ring[ch].lat_sum = 0;
                g_ring[ch].lat_n = 0;
                g_cb_n[ch] = 0;
            }
        }
    }
}

static int audio_open(void) {
    pthread_mutex_lock(&g_lock);
    if (g_dev_state == 0) {
        g_dev_state = -1;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "psprecomp: SDL_InitSubSystem(AUDIO) failed (%s)\n", SDL_GetError());
        } else {
            SDL_AudioSpec want = {
                .freq = AUDIO_OUT_FREQ,
                .format = AUDIO_OUT_FMT,
                .channels = AUDIO_OUT_CH,
                .samples = 1024,
                .callback = audio_callback,
            };
            SDL_AudioSpec got = { 0 };
            g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
            if (!g_dev) {
                fprintf(stderr, "psprecomp: SDL_OpenAudioDevice failed (%s)\n", SDL_GetError());
            } else {
                g_dev_samples = got.samples > 0 ? got.samples : want.samples;
                fprintf(stderr,
                        "audio: driver=%s asked %d Hz/%d frames, got %d Hz/%d frames fmt %d ch %d\n",
                        SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
                        AUDIO_OUT_FREQ, want.samples, got.freq, got.samples,
                        (int)got.format, (int)got.channels);
                SDL_PauseAudioDevice(g_dev, 0);   /* devices open paused */
                g_dev_state = 1;
            }
        }
    }
    int ok = g_dev_state == 1;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void psp_audio_channel_stop(int channel) {
    if (channel < 0 || channel >= AUDIO_CHANNELS) return;
    pthread_mutex_lock(&g_lock);
    audio_ring *r = &g_ring[channel];
    r->stopping = 1;
    if (r->count == 0) {
        r->active = 0;
        r->stopping = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void psprecomp_audio_reset(void) {
    pthread_mutex_lock(&g_lock);
    for (int c = 0; c < AUDIO_CHANNELS; c++) {
        audio_ring *r = &g_ring[c];
        r->head = r->tail = r->count = 0;
        r->active = r->stopping = r->dry = 0;
        r->target = r->block = r->trim = 0;
        r->lat_sum = 0;
        r->lat_n = 0;
        r->last_fill_us = 0;
        r->blocks = r->underruns = r->silence = r->dropped = 0;
        r->level_sum = r->level_n = 0;
        r->level_min = UINT32_MAX;
        r->level_max = 0;
        g_cb_min[c] = g_cb_max[c] = g_cb_n[c] = 0;
        g_underruns_play[c] = g_underruns_other[c] = 0;
        g_due_us[c] = 0;
    }
    g_last_cb_us = 0;
    g_last_report_us = 0;
    g_start_us = 0;
    g_audio_blocks = 0;
    g_audio_maxblocks = 0;
    g_target = 0;
    g_mag_min = FULL_SCALE;
    g_mag_max = 0;
    g_mag_sum = 0;
    g_calls = 0;
    pthread_mutex_unlock(&g_lock);
}

void psprecomp_audio_report(void) {
    double mean = g_calls ? (double)g_mag_sum / (double)g_calls : 0.0;
    pthread_mutex_lock(&g_lock);
    for (int c = 0; c < AUDIO_CHANNELS; c++) {
        audio_ring *r = &g_ring[c];
        if (!r->blocks) continue;
        double avg = r->level_n ? (double)r->level_sum / (double)r->level_n : 0.0;
        fprintf(stderr,
                "psprecomp_audio_report: ch%d blocks=%llu samples/block=%u underruns=%llu "
                "(play=%llu other=%llu) silence=%llu dropped=%llu "
                "ring_min=%u ring_avg=%.0f ring_max=%u avg_ms=%.1f\n",
                c, (unsigned long long)r->blocks, r->block,
                (unsigned long long)r->underruns,
                (unsigned long long)g_underruns_play[c],
                (unsigned long long)g_underruns_other[c],
                (unsigned long long)r->silence,
                (unsigned long long)r->dropped,
                r->level_min == UINT32_MAX ? 0 : r->level_min, avg, r->level_max,
                avg * 1000.0 / AUDIO_OUT_FREQ);
    }
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "psprecomp_audio_report: blocks=%llu maxblocks=%llu frames=%llu "
                "mean_dBFS=%.1f max_dBFS=%.1f min_dBFS=%.1f\n",
            (unsigned long long)g_audio_blocks,
            (unsigned long long)g_audio_maxblocks,
            (unsigned long long)g_calls,
            mean > 0.0 ? 20.0 * log10(mean / (double)FULL_SCALE) : -INFINITY,
            g_mag_max ? 20.0 * log10((double)g_mag_max / (double)FULL_SCALE) : -INFINITY,
            g_mag_min ? 20.0 * log10((double)g_mag_min / (double)FULL_SCALE) : -INFINITY);
}

/* PSP volumes run 0..0x8000 (unity). */
static int scale_vol(int v) { return v < 0 ? 0 : v > 0x8000 ? 0x8000 : v; }

/* Channels that deliver small blocks (the keysounds, 256 frames) sit about
 * half a big block closer to the speaker than the music (2048 frames), because
 * latency per sample is the level at enqueue plus (block - 1) / 2. Letting
 * them queue a little more evens it out. The criterion is the block size, not
 * the channel number. EMOTIONALSENSE_KEY_TRIM=<frames> adjusts it. */
static uint32_t key_trim_for(uint32_t block) {
    if (block > 256) return 0;
    static int init;
    static uint32_t trim = 512;
    if (!init) {
        init = 1;
        const char *e = getenv("EMOTIONALSENSE_KEY_TRIM");
        if (e && e[0]) { int v = atoi(e); trim = v > 0 ? (uint32_t)v : 0; }
    }
    return trim;
}

void psp_audio_output(int channel, int leftvol, int rightvol,
                      const int16_t *buf, int samples) {
    if (!buf || samples <= 0) return;
    if (!audio_open()) return;

    const int ci = channel < 0 ? 0 : channel >= AUDIO_CHANNELS ? AUDIO_CHANNELS - 1 : channel;
    const int lv = scale_vol(leftvol), rv = scale_vol(rightvol);
    uint64_t peak = 0, sum = 0;

    pthread_mutex_lock(&g_lock);
    audio_ring *r = &g_ring[ci];
    if (!r->buf) r->buf = calloc((size_t)RING_SAMPLES * 2, sizeof(int16_t));
    if (!r->buf) { pthread_mutex_unlock(&g_lock); return; }

    if ((uint32_t)samples > RING_SAMPLES - r->count) {
        r->dropped++;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    for (int i = 0; i < samples; i++) {
        int l = ((int)buf[2 * i]     * lv) >> 15;
        int rr = ((int)buf[2 * i + 1] * rv) >> 15;
        r->buf[2 * r->head]     = (int16_t)l;
        r->buf[2 * r->head + 1] = (int16_t)rr;
        r->head = (r->head + 1) % RING_SAMPLES;
        uint64_t m = (uint64_t)abs(l), mr = (uint64_t)abs(rr);
        if (mr > m) m = mr;
        sum += m;
        if (m > peak) peak = m;
    }
    /* Latency per sample: a block that enters behind `count` frames has
     * samples with count .. count+block-1 frames in front of them. */
    r->lat_sum += (double)samples * ((double)r->count + ((double)samples - 1.0) / 2.0);
    r->lat_n += (uint64_t)samples;
    r->count += (uint32_t)samples;
    r->block = (uint32_t)samples;
    r->trim = key_trim_for(r->block);
    r->last_fill_us = host_now_us();
    r->blocks++;
    r->dry = 0;                 /* the ring is fed again: next dry is a new one */

    /* Prefill: one target for every channel, so keysounds and BGM have the
     * same latency. The largest guest block sets the floor (2048 frames =
     * 46 ms), plus the device period, measured jitter and margin. */
    if (!g_target) {
        const char *e = getenv("EMOTIONALSENSE_AUDIO_LATENCY_MS");
        int ms = e ? atoi(e) : 40;
        uint32_t cfg = ms > 0 ? (uint32_t)ms * AUDIO_OUT_FREQ / 1000 : 0;
        uint32_t base = (uint32_t)g_dev_samples + JITTER_SAMPLES + MARGIN_SAMPLES;
        g_target = base > cfg ? base : cfg;
    }
    uint32_t floor = r->block + JITTER_SAMPLES + MARGIN_SAMPLES;
    if (floor > g_target) {
        g_target = floor;
        for (int c = 0; c < AUDIO_CHANNELS; c++) {
            audio_ring *o = &g_ring[c];
            if (o->target && o->target < g_target) {
                o->target = g_target;
                o->active = 0;   /* re-prefill to the new common target */
            }
        }
    }
    if (g_target > RING_SAMPLES / 2) g_target = RING_SAMPLES / 2;
    if (!r->target) r->target = g_target;
    if (!r->active && !r->stopping && r->count >= r->target) r->active = 1;
    pthread_mutex_unlock(&g_lock);

    /* Histogram (diagnostic only; racy counters are fine across threads). */
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

/* --- S1: the device sets the pace -----------------------------------------
 *
 * Every blocking output waits until its channel's ring has room again, which is
 * what the hardware queue does on a PSP. The level of the ring IS that
 * channel's latency, so following it keeps keysounds and BGM at the same
 * distance from the speaker instead of letting each drift on its own clock. */

static int device_consuming(void) {
    if (g_dev_state != 1) return 0;
    return g_last_cb_us && host_now_us() - g_last_cb_us < 200000u;
}

/* The host-clock grid, kept as the fallback: virtual clock, no audio device
 * opened, or a callback that stopped for longer than 200 ms. */
static void audio_grid(int ch, uint64_t play_us) {
    static int ahead_init;
    static uint64_t ahead_us;
    if (!ahead_init) {
        ahead_init = 1;
        const char *e = getenv("EMOTIONALSENSE_AUDIO_AHEAD_MS");
        int ms = e ? atoi(e) : 0;
        ahead_us = (uint64_t)(ms > 0 ? ms : 0) * 1000u;
    }
    uint64_t now = host_now_us();
    if (g_due_us[ch] + 100000u < now) g_due_us[ch] = now;
    g_due_us[ch] += play_us;
    uint64_t t = host_now_us();
    if (t + ahead_us < g_due_us[ch])
        psp_threadman_sleep_real(g_due_us[ch] - ahead_us - t);
}

void psp_audio_wait(int channel, uint64_t play_us) {
    if (psp_threadman_clock_virtual()) {
        /* Deterministic runs cannot pace against the device: how much audio is
         * queued would differ from run to run. The frame wait owns the virtual
         * clock, so only hand over here. */
        psp_threadman_sleep_real(play_us);
        return;
    }
    if (channel < 0 || channel >= AUDIO_CHANNELS) return;
    if (!device_consuming()) { audio_grid(channel, play_us); return; }
    for (;;) {
        pthread_mutex_lock(&g_lock);
        uint32_t level = g_ring[channel].count;
        uint32_t target = g_target;
        if (target) target += g_ring[channel].trim;
        pthread_mutex_unlock(&g_lock);
        if (!target || level <= target) break;
        if (!device_consuming()) break;   /* the device stopped: do not hang */
        uint64_t excess_us = (uint64_t)(level - target) * 1000000u / AUDIO_OUT_FREQ;
        psp_threadman_sleep_real(excess_us ? excess_us : 1000u);
    }
}
