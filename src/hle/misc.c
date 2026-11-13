/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl and sceAudio. Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Kernel_Library ------------------------------------------------------
 * Interrupt masking. The mask belongs to the *thread* that opened the critical
 * section, not to the machine: with several guest threads on one CPU, a global
 * flag would let one thread's suspend silence interrupts inside another
 * thread's unrelated bracket. The scheduler owns that state, because it is
 * also what decides whether an interrupt may be delivered at a park. */

static void hle_CpuSuspendIntr(void) {
    psp_ret((uint32_t)psp_threadman_intr_suspend());   /* the cookie resume expects */
}

static void hle_CpuResumeIntr(void) {
    psp_threadman_intr_resume(psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- UtilsForUser -------------------------------------------------------- */

/* Cache maintenance. There is no cache to write back -- the recompiled code
 * and the GE share one flat backing store -- so these are genuinely no-ops
 * rather than unimplemented. */
static void hle_CacheOp(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_LibcTime(void) {
    time_t t = time(NULL);
    uint32_t out = psp_arg(0);
    if (out) psp_write32(out, (uint32_t)t);
    psp_ret((uint32_t)t);
}

static void hle_LibcClock(void) {
    /* Microseconds since start. A game that uses this for frame pacing needs
     * it to advance, so it is derived from the host clock rather than being a
     * constant -- a frozen clock makes a game either spin or run at infinite
     * speed, both of which look like a hang. */
    psp_ret((uint32_t)((uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC));
}

static void hle_LibcGettimeofday(void) {
    uint32_t tv = psp_arg(0);
    if (tv) {
        time_t t = time(NULL);
        psp_write32(tv, (uint32_t)t);
        psp_write32(tv + 4, 0);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* General-purpose I/O pins, wired to the debug board. Nothing is connected. */
static void hle_GetGPI(void) { psp_ret(0); }

/* ---- StdioForUser -------------------------------------------------------- */
/* These return the file descriptors, which sceIoWrite then recognises. */
static void hle_Stdin(void)  { psp_ret(0); }
static void hle_Stdout(void) { psp_ret(1); }
static void hle_Stderr(void) { psp_ret(2); }

/* ---- sceSuspendForUser --------------------------------------------------- */
/* Power management around suspend. Nothing suspends here. */
static void hle_ok(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- LoadExecForUser ----------------------------------------------------- */

static int g_exit_requested;
int psp_exit_requested(void) { return g_exit_requested; }

static void hle_ExitGame(void) {
    /* A game calling this is finished. Recording it rather than terminating
     * the process lets the host report what happened and dump its counters. */
    g_exit_requested = 1;
    fprintf(stderr, "psprecomp: the game called sceKernelExitGame\n");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RegisterExitCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- ModuleMgrForUser ---------------------------------------------------- */
/* A game-sharing microgame is self-contained and does not load further
 * modules, so these report a plausible identity rather than doing anything.
 * A title that genuinely loads PRXs at run time will need real ones. */
/* These report the id of the one loaded module.
 *
 * A previous note here recorded the opposite -- that returning an id was a
 * "plausible lie" and an error was the truthful answer -- on the strength of a
 * test showing byte-identical output either way. **That test was run while
 * `jal` never assigned `$ra`**, so every non-leaf function in the program was
 * returning through a stale register. A falsification obtained under broken
 * codegen is not a falsification.
 *
 * Re-run after that fix, the two answers differ clearly: returning an id makes
 * the game's own "libc:_getmodreent: no reent structure" diagnostic disappear
 * and drops bad memory accesses from 3 to 0.
 *
 * And an id is the *truthful* answer. The question is "which module owns this
 * address", a self-contained microgame is exactly one module, and the host has
 * loaded it. Reporting 1 states that; reporting UNKNOWN_MODULE denies a module
 * that demonstrably exists. */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE 0x80020139u
#define PSP_MAIN_MODULE_ID 1u

static void hle_GetModuleId(void)          { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_GetModuleIdByAddress(void) { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_ModuleOk(void)             { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- sceCtrl ------------------------------------------------------------- */

static uint32_t g_buttons;
static uint8_t  g_analog_x = 128, g_analog_y = 128;

void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    g_buttons = buttons;
    g_analog_x = ax;
    g_analog_y = ay;
}

static void hle_CtrlSet(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* EMOTIONALSENSE_SCRIPT presses buttons on a schedule, so a headless run can
 * walk through menus. Entries are "vblank:hexmask:frames" or, relative to a
 * scene, "@scene+N:hexmask:frames": N frames after the first time the scene
 * word holds `scene`. The relative form survives a change in how long the
 * opening runs; the absolute form does not. A value starting with '@' names a
 * file with one entry per line ('#' comments). */
#define SCRIPT_MAX 64
typedef struct {
    int relative;
    uint32_t scene;
    uint64_t delta;                 /* vblank, or frames after the scene */
    uint32_t mask, frames;
    int seen;
    uint64_t start;
} script_entry;
static script_entry g_script[SCRIPT_MAX];
static int g_script_n;

/* The scene machine object is a fixed heap allocation; its id is at +4.
 * 0x0805E870 is app, so the id is the word below. */
#define SCENE_WORD 0x0805E874u

static void script_load(void) {
    const char *s = getenv("EMOTIONALSENSE_SCRIPT");
    static char buf[4096];
    if (s && *s == '@') {
        FILE *f = fopen(s + 1, "r");
        if (!f) return;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        for (char *p = buf; *p; p++)
            if (*p == '#') while (*p && *p != '\n') *p++ = ' ';
        s = buf;
    }
    if (!s) return;
    while (*s && g_script_n < SCRIPT_MAX) {
        script_entry *e = &g_script[g_script_n];
        unsigned long scene = 0, delta = 0, mask = 0, frames = 0, at = 0;
        int ok;
        memset(e, 0, sizeof *e);
        if (*s == '@')
            ok = sscanf(s, "@%lu+%lu:%lx:%lu", &scene, &delta, &mask, &frames) == 4;
        else
            ok = sscanf(s, "%lu:%lx:%lu", &at, &mask, &frames) == 3;
        if (ok) {
            e->relative = (*s == '@');
            e->scene = (uint32_t)scene;
            e->delta = e->relative ? delta : at;
            e->mask = (uint32_t)mask;
            e->frames = (uint32_t)frames;
            g_script_n++;
        }
        while (*s && *s != ',' && *s != '\n') s++;
        if (*s == ',' || *s == '\n') s++;
        else break;
    }
}

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16. */
static void hle_ReadBufferPositive(void) {
    uint32_t buf = psp_arg(0), count = psp_arg(1);
    if (!count) count = 1;
    uint32_t btns = g_buttons;
    static int script_init;
    if (!script_init) { script_init = 1; script_load(); }
    if (g_script_n) {
        uint64_t vb = psp_display_vblanks();
        uint32_t scene = psp_read32(SCENE_WORD);
        for (int i = 0; i < g_script_n; i++) {
            script_entry *e = &g_script[i];
            uint64_t at;
            if (e->relative) {
                if (!e->seen) {
                    if (scene != e->scene) continue;
                    e->seen = 1;
                    e->start = vb;
                }
                at = e->start + e->delta;
            } else {
                at = e->delta;
            }
            if (vb >= at && vb < at + e->frames) btns |= e->mask;
        }
        static int in_trace = -1;
        if (in_trace < 0) in_trace = getenv("EMOTIONALSENSE_TRACE_INPUT") != NULL;
        if (in_trace) {
            static uint32_t l_script;
            uint32_t script_bits = btns & ~g_buttons;
            if (script_bits != l_script) {
                fprintf(stderr, "input[%llu]: script=%04X\n",
                        (unsigned long long)vb, script_bits);
                l_script = script_bits;
            }
        }
    }
    if (getenv("EMOTIONALSENSE_TEST_NAME")) {
        uint64_t vb = psp_display_vblanks();
        if (vb >= 490 && vb < 495) btns |= 0x2000u; /* CIRCLE: letter A */
        else if (vb >= 520 && vb < 525) btns |= 0x0040u; /* DOWN */
        else if (vb >= 540 && vb < 545) btns |= 0x0040u; /* DOWN */
        else if (vb >= 560 && vb < 565) btns |= 0x0040u; /* DOWN -> END */
        else if (vb >= 580 && vb < 585) btns |= 0x2000u; /* CIRCLE: confirm END */
        else if (vb >= 650 && vb < 655) btns |= 0x2000u; /* CIRCLE */
        else if (vb >= 700 && vb < 705) btns |= 0x2000u; /* CIRCLE */
        else if (vb >= 750 && vb < 755) btns |= 0x2000u; /* CIRCLE */
        else if (vb >= 800 && vb < 805) btns |= 0x2000u; /* CIRCLE */
    }
    static int ctrl_trace = -1;
    if (ctrl_trace < 0) ctrl_trace = getenv("EMOTIONALSENSE_TRACE_CTRL") != NULL;
    if (ctrl_trace && (btns || psp_read16(buf + 16) != psp_read16(buf + 18)))
        fprintf(stderr, "ctrl[%llu]: %04X last=%04X cur=%04X\n",
                (unsigned long long)psp_display_vblanks(), btns,
                psp_read16(buf + 16), psp_read16(buf + 18));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = buf + i * 16;
        psp_write32(at, (uint32_t)psp_clock_us());   /* SceCtrlData timestamp is us */
        psp_write32(at + 4, btns);
        psp_write8(at + 8, g_analog_x);
        psp_write8(at + 9, g_analog_y);
        for (int k = 10; k < 16; k++) psp_write8(at + (uint32_t)k, 0);
    }
    psp_ret(count);
}

/* ---- sceAudio ------------------------------------------------------------ */

#define AUDIO_CHANNELS 8

typedef struct { int reserved; uint32_t samples; uint32_t format; } audio_ch;
static audio_ch g_audio[AUDIO_CHANNELS];
static uint64_t g_audio_blocks;

uint64_t psp_audio_blocks(void) { return g_audio_blocks; }

static void hle_ChReserve(void) {
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: ChReserve ch %d samples %u format %u\n",
                (int)psp_arg(0), psp_arg(1), psp_arg(2));
    /* (channel, samplecount, format) -- channel -1 means "any". */
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (int i = 0; i < AUDIO_CHANNELS; i++)
            if (!g_audio[i].reserved) { ch = i; break; }
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 1;
    g_audio[ch].samples = psp_arg(1);
    g_audio[ch].format  = psp_arg(2);
    psp_ret((uint32_t)ch);
}

static void hle_ChRelease(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The blocking forms hold the caller while the hardware drains a buffer, and
 * that is what paces a game's audio thread. Returning at once lets a mixer
 * loop spin forever and starve every other guest thread.
 *
 * Under real threads the hardware queue is modelled on the host clock: a
 * buffer is due one play time after the last, and the call returns when that
 * buffer has finished. The guest therefore runs at the hardware rate, which is
 * what the video player synchronises against. Giving the guest a head start
 * made the player believe it was late on every frame and decode in recovery
 * mode: 21.5 fps against PPSSPP's 59.94, with a quarter of the decoded frames
 * never shown. The cushion against crackle now lives in psp_audio_output,
 * where it is latency in the device queue instead of a lie to the guest.
 *
 * Waiting a fixed play time per call did not work there. The scheduler is
 * cooperative, each tick of the wait handed the CPU to the game's main thread
 * for a whole frame, and WTF's audio ran at about 10 buffers a second instead
 * of 172; its title waits for a jingle that, by its own sample count, never
 * finished. A thread that falls behind now catches up without waiting, as it
 * would against a real queue.
 * Inline mode keeps the virtual-time wait. */
#if PSPRECOMP_REAL_THREADS
static uint64_t host_now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static uint64_t g_audio_due_us[AUDIO_CHANNELS];

/* EMOTIONALSENSE_AUDIO_AHEAD_MS is an A/B override for the old behaviour; the
 * default is 0, no head start. */
static uint64_t audio_ahead_us(void) {
    static int init;
    static uint64_t ahead_us;
    if (!init) {
        init = 1;
        const char *e = getenv("EMOTIONALSENSE_AUDIO_AHEAD_MS");
        int ms = e ? atoi(e) : 0;
        ahead_us = (uint64_t)(ms > 0 ? ms : 0) * 1000u;
    }
    return ahead_us;
}

static void audio_pace(int ch, uint64_t play_us) {
    if (psp_threadman_clock_virtual()) {
        /* Deterministic runs cannot pace against the host clock: how much audio
         * is queued would differ from run to run. */
        psp_threadman_delay(play_us);
        return;
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) return;
    uint64_t now = host_now_us();
    /* A small lateness is made up on the next block: the deadline follows the
     * ideal grid. Only a real stall (more than one block behind) restarts it;
     * otherwise every late call becomes a permanent rate loss, because the next
     * call always arrives after the deadline. */
    if (g_audio_due_us[ch] + play_us < now) g_audio_due_us[ch] = now;
    g_audio_due_us[ch] += play_us;
    uint64_t ahead = audio_ahead_us();
    uint64_t t = host_now_us();
    if (t + ahead < g_audio_due_us[ch])
        psp_threadman_sleep_real(g_audio_due_us[ch] - ahead - t);
}
#endif

static void hle_OutputBlocking(void) {
    uint32_t ch = psp_arg(0);
    int vol = (int)psp_arg(1);
    uint32_t buf_addr = psp_arg(2);
    uint32_t samples = (ch < AUDIO_CHANNELS && g_audio[ch].samples) ? g_audio[ch].samples : 256;
    const int16_t *buf = (const int16_t *)psp_mem_ptr(buf_addr, samples * 4);
    if (buf) {
        psp_audio_output((int)ch, vol, vol, buf, (int)samples);
    }
    g_audio_blocks++;
    const uint64_t play_us = (uint64_t)samples * 1000000u / 44100u;
#if PSPRECOMP_REAL_THREADS
    audio_pace((int)ch, play_us);
#else
    psp_threadman_delay(play_us);
#endif
    psp_ret(samples);
}

static void hle_OutputPannedBlocking(void) {
    uint32_t ch = psp_arg(0);
    int leftvol = (int)psp_arg(1);
    int rightvol = (int)psp_arg(2);
    uint32_t buf_addr = psp_arg(3);
    uint32_t samples = (ch < AUDIO_CHANNELS && g_audio[ch].samples) ? g_audio[ch].samples : 256;
    const int16_t *buf = (const int16_t *)psp_mem_ptr(buf_addr, samples * 4);
    if (buf) {
        psp_audio_output((int)ch, leftvol, rightvol, buf, (int)samples);
    }
    g_audio_blocks++;
    const uint64_t play_us = (uint64_t)samples * 1000000u / 44100u;
#if PSPRECOMP_REAL_THREADS
    audio_pace((int)ch, play_us);
#else
    psp_threadman_delay(play_us);
#endif
    psp_ret(samples);
}

static void hle_OutputPanned(void) {
    uint32_t ch = psp_arg(0);
    int leftvol = (int)psp_arg(1);
    int rightvol = (int)psp_arg(2);
    uint32_t buf_addr = psp_arg(3);
    uint32_t samples = (ch < AUDIO_CHANNELS && g_audio[ch].samples) ? g_audio[ch].samples : 256;
    const int16_t *buf = (const int16_t *)psp_mem_ptr(buf_addr, samples * 4);
    if (buf) {
        psp_audio_output((int)ch, leftvol, rightvol, buf, (int)samples);
    }
    g_audio_blocks++;
    psp_ret(samples);
}

/* The sample count a channel outputs per call. It is set at reserve time and
 * changed here; ignoring the change left WTF's channel at its reserved length,
 * so each blocking output waited for 4,096 samples while the game mixed 256,
 * and its audio ran 16 times slow. */
static void hle_SetChannelDataLen(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003); return; }   /* invalid channel */
    g_audio[ch].samples = psp_arg(1);
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: SetChannelDataLen ch %u samples %u\n", ch, psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Zero remaining means "ready for more", so a game's audio loop keeps going. */
static void hle_GetChannelRestLength(void) { psp_ret(0); }

void psp_misc_reset(void) {
    /* The interrupt mask is per thread and lives in the scheduler; a reset
     * here cannot reach another thread's bracket, and should not. */
    g_exit_requested = 0;
    g_buttons = 0;
    g_analog_x = g_analog_y = 128;
    memset(g_audio, 0, sizeof g_audio);
#if PSPRECOMP_REAL_THREADS
    memset(g_audio_due_us, 0, sizeof g_audio_due_us);
#endif
    g_audio_blocks = 0;
}

void psp_misc_init(void) { psp_misc_reset(); }

void psp_misc_register(void) {
    psp_hle_register(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr", hle_CpuSuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr",  hle_CpuResumeIntr);

    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll",           hle_CacheOp);
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll", hle_CacheOp);
    psp_hle_register(0x27CC57F0, "UtilsForUser", "sceKernelLibcTime",         hle_LibcTime);
    psp_hle_register(0x91E4F6A7, "UtilsForUser", "sceKernelLibcClock",        hle_LibcClock);
    psp_hle_register(0x71EC4271, "UtilsForUser", "sceKernelLibcGettimeofday", hle_LibcGettimeofday);
    psp_hle_register(0x37FB5C42, "UtilsForUser", "sceKernelGetGPI",           hle_GetGPI);

    psp_hle_register(0x172D316E, "StdioForUser", "sceKernelStdin",  hle_Stdin);
    psp_hle_register(0xA6BAB2E9, "StdioForUser", "sceKernelStdout", hle_Stdout);
    psp_hle_register(0xF78BA90A, "StdioForUser", "sceKernelStderr", hle_Stderr);

    psp_hle_register(0xEADB1BD7, "sceSuspendForUser", "sceKernelPowerLock",   hle_ok);
    psp_hle_register(0x3AEE7261, "sceSuspendForUser", "sceKernelPowerUnlock", hle_ok);
    psp_hle_register(0x090CCB3F, "sceSuspendForUser", "sceKernelPowerTick",   hle_ok);

    psp_hle_register(0x05572A5F, "LoadExecForUser", "sceKernelExitGame",             hle_ExitGame);
    psp_hle_register(0x4AC57943, "LoadExecForUser", "sceKernelRegisterExitCallback", hle_RegisterExitCallback);

    /* Observed but unidentified. The game calls this three times on its
     * heap-setup path, and an unimplemented call returns 0 -- which for a
     * module query means "no module" and makes heap establishment fail.
     * Eighteen plausible ModuleMgr names were tried against SHA-1 with no
     * match, so the name is genuinely unknown and is not invented here. */
    psp_hle_register_unnamed(0xF9275D98, "ModuleMgrForUser", hle_GetModuleId);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",          hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress", hle_GetModuleIdByAddress);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",          hle_ModuleOk);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",           hle_ModuleOk);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",         hle_ModuleOk);
    psp_hle_register(0x8F2DF740, "ModuleMgrForUser", "sceKernelStopUnloadSelfModuleWithStatus", hle_ModuleOk);

    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",     hle_CtrlSet);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",    hle_CtrlSet);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive",  hle_ReadBufferPositive);

    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_OutputBlocking);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_OutputPannedBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_OutputPanned);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_SetChannelDataLen);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ok);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ok);
}
