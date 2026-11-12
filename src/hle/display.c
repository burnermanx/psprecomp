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
#include "psprecomp/render.h"
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

void psp_display_reset(void) {
    g_fb_addr = 0;
    g_fb_width = 512;
    g_fb_format = PSP_DISPLAY_PIXEL_FORMAT_8888;
    g_mode = 0;
    g_mode_w = PSP_SCREEN_W;
    g_mode_h = PSP_SCREEN_H;
    g_vblank_count = 0;
}

void psp_display_init(void) { psp_display_reset(); }

uint64_t psp_display_vblanks(void) { return g_vblank_count; }
uint32_t psp_display_framebuffer(void) { return g_fb_addr; }

/* Write the current framebuffer as a binary PPM. Returns 0 on success, -1 if
 * no framebuffer has been set yet (which is itself worth knowing during
 * bring-up: the game never got as far as showing anything). */
int psp_display_capture(const char *path) {
    static uint32_t px[PSP_SCREEN_W * PSP_SCREEN_H];
    if (psp_display_read(px) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", PSP_SCREEN_W, PSP_SCREEN_H);
    for (int i = 0; i < PSP_SCREEN_W * PSP_SCREEN_H; i++) {
        const uint32_t c = px[i];
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
 * /tmp) as frame_<vb>.ppm. */
static void shot_if_wanted(void) {
    static int init;
    static char spec[512];
    static long every;
    static char dir[512] = "/tmp";
    if (!init) {
        init = 1;
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
    if (psp_display_capture(path) == 0) fprintf(stderr, "Saved screenshot %s\n", path);
}

static void hle_SetFrameBuf(void) {
    /* (topaddr, bufferwidth, pixelformat, sync) */
    if (getenv("EMOTIONALSENSE_DEBUG_FB")) {
        static uint32_t last_fb;
        if (psp_arg(0) != last_fb) {
            fprintf(stderr, "SetFrameBuf changed: 0x%08X -> 0x%08X width=%u fmt=%u vb=%llu\n",
                    last_fb, psp_arg(0), psp_arg(1), psp_arg(2), (unsigned long long)g_vblank_count);
            last_fb = psp_arg(0);
        }
    }
    shot_if_wanted();
    g_fb_addr   = psp_arg(0);
    g_fb_width  = psp_arg(1);
    g_fb_format = psp_arg(2);
    if (!g_fb_width) g_fb_width = 512;
    psp_render_current()->present();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* There is no scanout, so a vblank wait does not wait for one: it bumps the
 * counter and gives the CPU away. A game's main loop is usually
 * `render(); WaitVblank();`, which means this counter is the frame number --
 * the most useful single number to have during bring-up, because it tells you
 * whether the game is looping or stuck -- and the yield is what lets the
 * threads that feed that loop run. */
static void hle_WaitVblank(void) {
    g_vblank_count++;
    if (getenv("EMOTIONALSENSE_DEBUG_FB") && (g_vblank_count % 100) == 0) {
        fprintf(stderr, "vblank %llu\n", (unsigned long long)g_vblank_count);
    }
    /* A frame boundary is the guest asking for time to pass, so it is the third
     * point where an interrupt is delivered (src/hle/intrman.c) -- and, on one
     * CPU, the most natural place to hand the CPU over. */
    psp_threadman_frame_wait();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_display_register(void) {
    psp_hle_register(0x0E20F177, "sceDisplay", "sceDisplaySetMode",           hle_SetMode);
    psp_hle_register(0x289D82FE, "sceDisplay", "sceDisplaySetFrameBuf",       hle_SetFrameBuf);
    psp_hle_register(0x36CDFADE, "sceDisplay", "sceDisplayWaitVblank",        hle_WaitVblank);
    psp_hle_register(0x46F186C3, "sceDisplay", "sceDisplayWaitVblankStartCB", hle_WaitVblank);
}
