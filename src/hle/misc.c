/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl and sceAudio. Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"
#include "psprecomp/render.h"

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

#define BTN_FACE_SWAP 0x6000u   /* circle 0x2000, cross 0x4000 */
static int g_swap_enabled;
static int (*g_swap_now)(void);

void psp_ctrl_set_face_swap(int enabled, int (*swap_now)(void)) {
    g_swap_enabled = enabled;
    g_swap_now = swap_now;
}

/* The swap and its mask, applied to what the host delivered. */
static uint32_t face_swap(uint32_t btns) {
    static int was = -1;
    static uint32_t masked;      /* face buttons held across a change of state */
    if (!g_swap_enabled) return btns;
    int now = g_swap_now ? g_swap_now() != 0 : 1;
    psp_render_face_swap = now;     /* the on-screen glyphs follow the same gate */
    if (was >= 0 && now != was) masked = btns & BTN_FACE_SWAP;
    was = now;
    masked &= btns;              /* released: no longer masked */
    btns &= ~masked;
    if (now) {
        uint32_t c = btns & 0x2000u, x = btns & 0x4000u;
        btns = (btns & ~BTN_FACE_SWAP) | (c << 1) | (x >> 1);
    }
    return btns;
}

/* Host hook on what the game reads: after the face swap, so the filter sees the
 * buttons as the game does (cancel is always cross in a menu). */
static uint32_t (*g_ctrl_filter)(uint32_t);
void psp_ctrl_set_filter(uint32_t (*fn)(uint32_t buttons)) { g_ctrl_filter = fn; }

static void hle_CtrlSet(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* EMOTIONALSENSE_SCRIPT presses buttons on a schedule, so a headless run can
 * walk through menus. Entries are "vblank:hexmask:frames" or, relative to a
 * scene, "@scene+N:hexmask:frames": N frames after the first time the scene
 * word holds `scene`, whatever the current scene is by then. "@scene#k+N:..."
 * is the stricter form: it only fires while the current scene is still that
 * scene, and only on the k-th entry (k=0 re-arms on every entry), which is
 * what a multi-song run needs. The relative form survives a change in how long
 * the opening runs; the absolute form does not. A value starting with '@'
 * names a file with one entry per line ('#' comments). */
#define SCRIPT_MAX 2048
typedef struct {
    int relative;
    int has_nth;                    /* @scene#k+N: restrict to the k-th entry */
    uint32_t scene;
    int nth;                        /* 1-based scene entry, 0 = every entry */
    uint64_t delta;                 /* vblank, or frames after the scene */
    uint32_t mask, frames;
    int seen;
    uint32_t start_count;           /* scene entry this start belongs to */
    uint64_t start;
} script_entry;
static script_entry g_script[SCRIPT_MAX];
static int g_script_n;
static uint32_t g_scene_last;
static uint32_t g_scene_count[256];


static void script_load(void) {
    const char *s = getenv("EMOTIONALSENSE_SCRIPT");
    static char buf[131072];
    if (s && *s == '@') {
        FILE *f = fopen(s + 1, "r");
        if (!f) { fprintf(stderr, "script: cannot open %s\n", s + 1); return; }
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        for (char *p = buf; *p; p++)
            if (*p == '#' && (p == buf || p[-1] == '\n'))
                while (*p && *p != '\n') *p++ = ' ';
        s = buf;
    }
    if (!s) return;
    while (*s && g_script_n < SCRIPT_MAX) {
        script_entry *e = &g_script[g_script_n];
        unsigned long scene = 0, delta = 0, mask = 0, frames = 0, at = 0;
        int nth = 1, ok;
        memset(e, 0, sizeof *e);
        if (*s == '@') {
            ok = sscanf(s, "@%lu#%d+%lu:%lx:%lu", &scene, &nth, &delta, &mask, &frames) == 5;
            if (ok) e->has_nth = 1;
            else
                ok = sscanf(s, "@%lu+%lu:%lx:%lu", &scene, &delta, &mask, &frames) == 4;
        } else {
            ok = sscanf(s, "%lu:%lx:%lu", &at, &mask, &frames) == 3;
        }
        if (ok) {
            e->relative = (*s == '@');
            e->scene = (uint32_t)scene;
            e->nth = nth;
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
        uint32_t scene = psp_scene_now();
        if (scene && scene != g_scene_last) g_scene_count[scene & 0xFFu]++;
        g_scene_last = scene;
        for (int i = 0; i < g_script_n; i++) {
            script_entry *e = &g_script[i];
            uint64_t at;
            if (e->relative) {
                uint32_t cnt = g_scene_count[e->scene & 0xFFu];
                if (!e->has_nth) {
                    /* @scene+N: N frames after the first time the scene
                     * appeared, whatever the current scene is. */
                    if (!e->seen && scene == e->scene) {
                        e->seen = 1;
                        e->start = vb;
                    }
                    if (!e->seen) continue;
                    at = e->start + e->delta;
                } else {
                    /* @scene#k+N: only while the current scene is that scene,
                     * and only on the k-th entry (k=0 re-arms every entry). */
                    if (scene != e->scene) continue;
                    if (e->nth > 0) {
                        if (cnt != (uint32_t)e->nth) continue;
                        if (!e->seen) { e->seen = 1; e->start = vb; }
                    } else {
                        if (!e->seen || e->start_count != cnt) {
                            e->seen = 1;
                            e->start_count = cnt;
                            e->start = vb;
                        }
                    }
                    at = e->start + e->delta;
                }
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
    btns = face_swap(btns);
    if (g_ctrl_filter) btns = g_ctrl_filter(btns);
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
    psp_audio_channel_stop(ch);
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
static void audio_pace(int ch, uint64_t play_us) { psp_audio_wait(ch, play_us); }
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

static uint32_t g_scene_addr;
static uint32_t g_scene_ptr;   /* guest address of a pointer to the object holding the word */
static uint32_t g_scene_off;

/* Where the game keeps the word that names the current scene. Registered by
 * the host, read by the audio trace and the texture dump. */
void psp_set_scene_source(uint32_t addr) { g_scene_addr = addr; }

/* Same word, but found through a pointer: the object is a heap allocation, so
 * its address moves with whatever was allocated before it (a data update with
 * a bigger resident package shifts it). The fixed address stays the fallback
 * until the pointer is set. */
void psp_set_scene_source_ptr(uint32_t ptr_addr, uint32_t off, uint32_t fallback) {
    g_scene_ptr = ptr_addr; g_scene_off = off; g_scene_addr = fallback;
}

uint32_t psp_scene_now(void) {
    uint32_t a = g_scene_addr;
    if (g_scene_ptr) {
        uint32_t o = psp_read32(g_scene_ptr);
        if (o) a = o + g_scene_off;
    }
    return a ? psp_read32(a) : 0;
}

void psp_misc_reset(void) {
    /* The interrupt mask is per thread and lives in the scheduler; a reset
     * here cannot reach another thread's bracket, and should not. */
    g_exit_requested = 0;
    g_buttons = 0;
    g_analog_x = g_analog_y = 128;
    memset(g_audio, 0, sizeof g_audio);
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
