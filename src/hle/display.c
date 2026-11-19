/* psprecomp — sceDisplay.
 *
 * The PSP's display is a framebuffer address plus a pixel format, scanned out
 * at ~59.94 Hz. There is no blitting and no compositing: a game writes pixels
 * (usually via the GE) and tells sceDisplay where they are.
 *
 * That makes this small but pivotal — it is the first point in bring-up where
 * a recompiled game produces something you can *look at*. psp_display_capture()
 * exists for exactly that: dump whatever the game currently considers the
 * front buffer, and compare it against the same frame from an emulator.
 */

#include "psprecomp/hle.h"
#include "psprecomp/log.h"
#include "psprecomp/render.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>

#include <stdio.h>
#include <string.h>

#define PSP_SCREEN_W 480
#define PSP_SCREEN_H 272

/* Pixel formats, as passed to sceDisplaySetFrameBuf. */
#define PSP_DISPLAY_PIXEL_FORMAT_565  0
#define PSP_DISPLAY_PIXEL_FORMAT_5551 1
#define PSP_DISPLAY_PIXEL_FORMAT_4444 2
#define PSP_DISPLAY_PIXEL_FORMAT_8888 3

static uint32_t g_fb_addr;
static uint32_t g_fb_width;      /* in pixels, the stride -- usually 512 */
static uint32_t g_fb_format;
static uint32_t g_mode, g_mode_w, g_mode_h;
static uint64_t g_vblank_count;

/* EMOTIONALSENSE_TRACE_FPS=1: one line per second of guest clock with how many
 * of the vblanks showed a new picture. A frame is "new" when the snapshot taken
 * at the last SetFrameBuf differs (FNV-1a over the pixels) from the one seen at
 * the previous vblank, so it counts content, not calls. Costs one hash per
 * flip, only when the variable is set. */
static int g_tf = -1;
static uint64_t g_snap_hash, g_snap_us, g_prev_hash;
static uint64_t g_tf_vb, g_tf_flips, g_tf_chg, g_tf_distinct, g_tf_run, g_tf_maxrep, g_tf_sec;
static uint32_t g_tf_last_fb;
/* Callers of WaitVblank and SetFrameBuf (the guest return address), per second. */
typedef struct { uint32_t ra; uint32_t n; } tf_ra;
static tf_ra g_tf_wait[4], g_tf_flip[4];
static uint64_t g_tf_ge0;
/* A flicker of a few pixels (the life gauge sparkle) changes the hash without
 * the picture moving, so a frame also counts as "real" only when at least
 * 0.2% of its pixels differ from the previous vblank's. */
static uint32_t *g_tf_prev;
static size_t g_tf_prev_n;
static uint64_t g_tf_real, g_tf_rrun, g_tf_rmax;

static void tf_ra_add(tf_ra *t, uint32_t ra) {
    for (int i = 0; i < 4; i++)
        if (t[i].ra == ra || !t[i].n) { t[i].ra = ra; t[i].n++; return; }
}
static void tf_ra_print(const char *name, tf_ra *t) {
    psp_log(" %s=", name);
    for (int i = 0; i < 4 && t[i].n; i++) psp_log("%s0x%X:%u", i ? "," : "", t[i].ra, t[i].n);
    memset(t, 0, 4 * sizeof *t);
}

static int trace_fps_on(void) {
    if (g_tf < 0) { const char *e = getenv("EMOTIONALSENSE_TRACE_FPS"); g_tf = e && *e && *e != '0'; }
    return g_tf;
}

static uint64_t hash_px(const uint32_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* A completed frame, copied at the flip point so the window never reads a
 * buffer the guest is drawing into. Seqlock: odd means a copy is in progress.
 *
 * The frame is either the backend's own image at internal resolution (when it
 * has one: the Vulkan backend at N > 1 keeps the frame there and never writes
 * it back to guest VRAM) or the 480x272 guest framebuffer. */
static uint32_t *g_snap;
static int g_snap_w = PSP_SCREEN_W, g_snap_h = PSP_SCREEN_H;
static _Atomic uint32_t g_snap_seq;

static void snap_capture(void) {
    const uint32_t *fpx;
    int fw, fh;
    if (psp_render_frame(&fpx, &fw, &fh) == 0) {
        if (fw != g_snap_w || fh != g_snap_h) {
            uint32_t *n = realloc(g_snap, (size_t)fw * (size_t)fh * 4u);
            if (!n) return;
            g_snap = n;
            g_snap_w = fw;
            g_snap_h = fh;
        }
        uint32_t s = atomic_load_explicit(&g_snap_seq, memory_order_relaxed);
        atomic_store_explicit(&g_snap_seq, s + 1, memory_order_relaxed);
        atomic_thread_fence(memory_order_release);
        memcpy(g_snap, fpx, (size_t)fw * (size_t)fh * 4u);
        g_snap_us = psp_host_mono_us();
        if (trace_fps_on()) g_snap_hash = hash_px(g_snap, (size_t)fw * (size_t)fh);
        atomic_store_explicit(&g_snap_seq, s + 2, memory_order_release);
        return;
    }
    if (!g_snap) {
        g_snap = malloc((size_t)g_snap_w * (size_t)g_snap_h * 4u);
        if (!g_snap) return;
    }
    uint32_t s = atomic_load_explicit(&g_snap_seq, memory_order_relaxed);
    atomic_store_explicit(&g_snap_seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    psp_display_read(g_snap);
    g_snap_us = psp_host_mono_us();
    if (trace_fps_on()) g_snap_hash = hash_px(g_snap, (size_t)g_snap_w * (size_t)g_snap_h);
    atomic_store_explicit(&g_snap_seq, s + 2, memory_order_release);
}

/* Host time at which the last completed frame was copied (monotonic us). */
uint64_t psp_display_snap_us(void) { return g_snap_us; }

void psp_display_size(int *w, int *h) { *w = g_snap_w; *h = g_snap_h; }

int psp_display_snapshot(uint32_t *out, uint32_t *seq) {
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
        if (s1 == *seq) return 0;
        if (s1 & 1) return 0;          /* copy in progress; try next loop */
        if (!g_snap) return 0;
        memcpy(out, g_snap, (size_t)g_snap_w * (size_t)g_snap_h * 4u);
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
        if (s1 == s2) { *seq = s1; return 1; }
    }
}

void psp_display_reset(void) {
    g_fb_addr = 0;
    g_fb_width = 512;
    g_fb_format = PSP_DISPLAY_PIXEL_FORMAT_8888;
    g_mode = 0;
    g_mode_w = PSP_SCREEN_W;
    g_mode_h = PSP_SCREEN_H;
    g_vblank_count = 0;
    atomic_store(&g_snap_seq, 0);
    g_snap_w = PSP_SCREEN_W;
    g_snap_h = PSP_SCREEN_H;
    if (!g_snap) g_snap = calloc((size_t)g_snap_w * (size_t)g_snap_h, 4u);
    else memset(g_snap, 0, (size_t)g_snap_w * (size_t)g_snap_h * 4u);
}

void psp_display_init(void) { psp_display_reset(); }

uint64_t psp_display_vblanks(void) { return g_vblank_count; }
uint32_t psp_display_framebuffer(void) { return g_fb_addr; }

/* Write the current framebuffer as a binary PPM. Returns 0 on success, -1 if
 * no framebuffer has been set yet (which is itself worth knowing during
 * bring-up: the game never got as far as showing anything). */
int psp_display_capture(const char *path) {
    static uint32_t px[PSP_SCREEN_W * PSP_SCREEN_H];
    const uint32_t *src;
    int w = PSP_SCREEN_W, h = PSP_SCREEN_H;
    /* A capture is the frame at internal resolution when the backend has one:
     * at N > 1 guest VRAM holds no frame at all. */
    if (psp_render_frame(&src, &w, &h) != 0) {
        if (psp_display_read(px) != 0) return -1;
        src = px;
    }

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        const uint32_t c = src[i];
        uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
        fwrite(rgb, 1, 3, f);            /* PPM is RGB; alpha is dropped */
    }
    fclose(f);
    return 0;
}

/* The front buffer, converted, for a window or a capture. Read while the game
 * runs: a frame being drawn into the *back* buffer is never seen, which is the
 * point of the game double-buffering. */
int psp_display_read(uint32_t *out) {
    const uint32_t addr = g_fb_addr, stride = g_fb_width, fmt = g_fb_format;
    if (!addr) return -1;
    if (fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
        for (int y = 0; y < PSP_SCREEN_H; y++) {
            uint32_t at = addr + (uint32_t)(y * (int)stride) * 4u;
            const void *src = psp_mem_ptr(at, PSP_SCREEN_W * 4u);
            if (src) {
                memcpy(out + y * PSP_SCREEN_W, src, PSP_SCREEN_W * 4u);
            } else {
                for (int x = 0; x < PSP_SCREEN_W; x++)
                    out[y * PSP_SCREEN_W + x] = psp_read32(at + (uint32_t)x * 4u);
            }
        }
        return 0;
    }
    const int bpp = 2;
    for (int y = 0; y < PSP_SCREEN_H; y++) {
        for (int x = 0; x < PSP_SCREEN_W; x++) {
            uint32_t at = addr + (uint32_t)(y * (int)stride + x) * (uint32_t)bpp;
            uint32_t px = psp_read16(at);
            out[y * PSP_SCREEN_W + x] = psp_color_expand(px, (int)fmt);
        }
    }
    return 0;
}

/* ---- the calls ----------------------------------------------------------- */

static void hle_SetMode(void) {
    g_mode   = psp_arg(0);
    g_mode_w = psp_arg(1);
    g_mode_h = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* EMOTIONALSENSE_SHOT=<vb,vb,...> and EMOTIONALSENSE_SHOT_EVERY=N capture the
 * presented frame at those vblanks into EMOTIONALSENSE_SHOT_DIR (default
 * %TEMP% on Windows, /tmp elsewhere) as frame_<vb>.ppm. */
const char *psp_host_temp_dir(void) {
    const char *d = getenv("TEMP");
    return d && *d ? d : "/tmp";
}

static void shot_if_wanted(void) {
    static int init;
    static char spec[512];
    static long every;
    static char dir[512];
    if (!init) {
        init = 1;
        snprintf(dir, sizeof dir, "%s", psp_host_temp_dir());
        const char *s = getenv("EMOTIONALSENSE_SHOT");
        if (s) snprintf(spec, sizeof spec, "%s", s);
        const char *e = getenv("EMOTIONALSENSE_SHOT_EVERY");
        if (e) every = atol(e);
        const char *d = getenv("EMOTIONALSENSE_SHOT_DIR");
        if (d && *d) snprintf(dir, sizeof dir, "%s", d);
    }
    int want = every > 0 && g_vblank_count % (uint64_t)every == 0;
    if (!want && spec[0]) {
        char buf[sizeof spec];
        snprintf(buf, sizeof buf, "%s", spec);
        for (char *p = strtok(buf, ","); p && !want; p = strtok(NULL, ","))
            want = strtoul(p, NULL, 0) == g_vblank_count;
    }
    if (!want) return;
    char path[600];
    snprintf(path, sizeof path, "%s/frame_%llu.ppm", dir, (unsigned long long)g_vblank_count);
    if (psp_display_capture(path) == 0) psp_log("Saved screenshot %s t=%.4f\n", path, (double)psp_clock_us() / 1e6);
}

static void hle_SetFrameBuf(void) {
    /* (topaddr, bufferwidth, pixelformat, sync) */
    if (getenv("EMOTIONALSENSE_DEBUG_FB")) {
        static uint32_t last_fb;
        if (psp_arg(0) != last_fb) {
            psp_log("SetFrameBuf changed: 0x%08X -> 0x%08X width=%u fmt=%u vb=%llu\n",
                    last_fb, psp_arg(0), psp_arg(1), psp_arg(2), (unsigned long long)g_vblank_count);
            last_fb = psp_arg(0);
        }
    }
    /* Finish the frame the guest just drew and snapshot it before the new
     * buffer is published; the window reads the snapshot, not live VRAM. */
    psp_render_current()->present();
    if (g_fb_addr) snap_capture();
    if (trace_fps_on()) {
        g_tf_flips++;
        tf_ra_add(g_tf_flip, psp_cpu.r[PSP_REG_RA]);
        if (psp_arg(0) != g_tf_last_fb) { g_tf_chg++; g_tf_last_fb = psp_arg(0); }
    }
    shot_if_wanted();
    g_fb_addr   = psp_arg(0);
    g_fb_width  = psp_arg(1);
    g_fb_format = psp_arg(2);
    if (!g_fb_width) g_fb_width = 512;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* There is no scanout, so a vblank wait does not wait for one: it bumps the
 * counter and gives the CPU away. A game's main loop is usually
 * `render(); WaitVblank();`, which means this counter is the frame number --
 * the most useful single number to have during bring-up, because it tells you
 * whether the game is looping or stuck -- and the yield is what lets the
 * threads that feed that loop run. */
static void trace_fps_vblank(void) {
    g_tf_vb++;
    tf_ra_add(g_tf_wait, psp_cpu.r[PSP_REG_RA]);
    if (g_snap_hash != g_prev_hash) {
        g_tf_distinct++;
        if (g_tf_run > g_tf_maxrep) g_tf_maxrep = g_tf_run;
        g_tf_run = 1;
        g_prev_hash = g_snap_hash;
    } else {
        g_tf_run++;
    }
    if (g_snap) {
        size_t n = (size_t)g_snap_w * (size_t)g_snap_h;
        if (n != g_tf_prev_n) {
            free(g_tf_prev);
            g_tf_prev = calloc(n, 4);
            g_tf_prev_n = g_tf_prev ? n : 0;
        }
        size_t diff = 0;
        if (g_tf_prev) {
            for (size_t i = 0; i < n; i++) diff += g_snap[i] != g_tf_prev[i];
            memcpy(g_tf_prev, g_snap, n * 4);
        }
        if (diff * 500 > n) {
            g_tf_real++;
            if (g_tf_rrun > g_tf_rmax) g_tf_rmax = g_tf_rrun;
            g_tf_rrun = 1;
        } else {
            g_tf_rrun++;
        }
    }
    uint64_t sec = psp_clock_us() / 1000000ull;
    if (sec != g_tf_sec) {
        if (g_tf_run > g_tf_maxrep) g_tf_maxrep = g_tf_run;
        psp_log("fps[%llu]: t=%llus vblanks=%llu flips=%llu fbchg=%llu distinct=%llu maxrep=%llu real=%llu realrep=%llu",
                (unsigned long long)g_vblank_count, (unsigned long long)sec,
                (unsigned long long)g_tf_vb, (unsigned long long)g_tf_flips,
                (unsigned long long)g_tf_chg, (unsigned long long)g_tf_distinct,
                (unsigned long long)g_tf_maxrep, (unsigned long long)g_tf_real,
                (unsigned long long)(g_tf_rrun > g_tf_rmax ? g_tf_rrun : g_tf_rmax));
        tf_ra_print("wait_ra", g_tf_wait);
        tf_ra_print("flip_ra", g_tf_flip);
        psp_log(" ge_cmds=%llu\n", (unsigned long long)(psp_ge_command_count() - g_tf_ge0));
        g_tf_ge0 = psp_ge_command_count();
        g_tf_sec = sec;
        g_tf_vb = g_tf_flips = g_tf_chg = g_tf_distinct = g_tf_maxrep = 0;
        g_tf_real = g_tf_rmax = 0;
    }
}

static void hle_WaitVblank(void) {
    g_vblank_count++;
    if (trace_fps_on()) trace_fps_vblank();
    if (getenv("EMOTIONALSENSE_DEBUG_FB") && (g_vblank_count % 100) == 0) {
        psp_log("vblank %llu\n", (unsigned long long)g_vblank_count);
    }
    /* A frame boundary is the guest asking for time to pass, so it is the third
     * point where an interrupt is delivered (src/hle/intrman.c) -- and, on one
     * CPU, the most natural place to hand the CPU over. */
    psp_threadman_frame_wait();
    /* EMOTIONALSENSE_EXP_SKIPVB=N (with EMOTIONALSENSE_EXP_FROM=<vblank>):
     * experiment, not behaviour. Every N-th vblank takes two frame periods, as
     * if the host had missed one, so the guest sees 60*(N-1)/N vblanks per
     * second of real time. Used to tell whether the gameplay follows time or
     * the frame count. */
    static int skip = -1;
    static uint64_t from;
    if (skip < 0) {
        const char *e = getenv("EMOTIONALSENSE_EXP_SKIPVB");
        skip = e ? atoi(e) : 0;
        const char *f = getenv("EMOTIONALSENSE_EXP_FROM");
        from = f ? strtoull(f, NULL, 0) : 0;
    }
    if (skip > 0 && g_vblank_count >= from && g_vblank_count % (uint64_t)skip == 0)
        psp_threadman_frame_wait();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_display_register(void) {
    psp_hle_register(0x0E20F177, "sceDisplay", "sceDisplaySetMode",           hle_SetMode);
    psp_hle_register(0x289D82FE, "sceDisplay", "sceDisplaySetFrameBuf",       hle_SetFrameBuf);
    psp_hle_register(0x36CDFADE, "sceDisplay", "sceDisplayWaitVblank",        hle_WaitVblank);
    psp_hle_register(0x46F186C3, "sceDisplay", "sceDisplayWaitVblankStartCB", hle_WaitVblank);
}
