/* psprecomp -- render backend interface. See docs/RENDERER.md.
 *
 * Display-list *interpretation* has one implementation (src/hle/ge.c);
 * *presentation* is pluggable. The split matters because the software
 * rasterizer is the oracle every other backend is checked against, and it has
 * to keep working on a machine with no GPU.
 *
 * Primitives arrive assembled -- vertex format decoding happens once, in the
 * interpreter, rather than being duplicated (and mis-duplicated) per backend.
 * Texel decoding is shared the same way: psp_tex_fetch() is the one place that
 * knows the texture formats, swizzling and CLUTs.
 */
#ifndef PSPRECOMP_RENDER_H
#define PSPRECOMP_RENDER_H

#include <stdint.h>
#include <stdio.h>

/* A vertex after format decoding: screen space, colour resolved, texture
 * coordinates in texels (through mode addresses texels directly). Colours
 * here and below are 0xAABBGGRR, the GE's own byte order. */
typedef struct {
    int      x, y;
    float    fx, fy;         /* x, y before rounding, for backends that keep it (see psp_render_subpixel) */
    float    z;              /* depth, 0..65535 */
    uint32_t rgba;
    float    u, v;
    int      behind;         /* behind the eye: triangles using it are dropped */
} psp_vertex;

/* GE primitive types, from the PRIM argument's type field. */
enum {
    PSP_PRIM_POINTS = 0,
    PSP_PRIM_LINES,
    PSP_PRIM_LINE_STRIP,
    PSP_PRIM_TRIANGLES,
    PSP_PRIM_TRIANGLE_STRIP,
    PSP_PRIM_TRIANGLE_FAN,
    PSP_PRIM_SPRITES
};

/* GE texture / CLUT formats (TPSM, CMODE). 0-3 are also the framebuffer and
 * display formats. */
enum {
    PSP_TPSM_565 = 0, PSP_TPSM_5551, PSP_TPSM_4444, PSP_TPSM_8888,
    PSP_TPSM_T4, PSP_TPSM_T8, PSP_TPSM_T16, PSP_TPSM_T32,
    PSP_TPSM_DXT1, PSP_TPSM_DXT3, PSP_TPSM_DXT5
};

/* Everything about the GE's state that changes what a primitive's pixels
 * become. Filled by the interpreter from the state commands; read by the
 * backend. Field comments name the command each comes from. */
typedef struct {
    int      clear;          /* CLEAR bit 0: write vertex colour, nothing else */
    int      clear_color;    /* CLEAR bit 8: ...to RGB */
    int      clear_alpha;    /* CLEAR bit 9: ...to alpha (stencil) */
    int      clear_depth;    /* CLEAR bit 10: ...to depth */
    int      gouraud;        /* SHADE */

    int      tex_enable;     /* TME */
    uint32_t tex_addr;       /* TBP0 + TBW0 high byte */
    int      tex_bw;         /* TBW0: buffer width, texels */
    int      tex_w, tex_h;   /* TSIZE0, as sizes not logs */
    int      tex_fmt;        /* TPSM */
    int      tex_swizzle;    /* TMODE bit 0 */
    int      tex_clamp_u, tex_clamp_v;   /* TWRAP */
    int      tex_filter;     /* TEXFILTER bit 0: 0 nearest, 1 bilinear */
    int      tex_func;       /* TFUNC bits 0-2 */
    int      tex_alpha;      /* TFUNC bit 8: texture alpha is used */
    int      tex_double;     /* TFUNC bit 16: colour doubling */
    uint32_t tex_env;        /* TEC, for the blend function */

    const uint8_t *clut;     /* bytes captured at the last CLOAD */
    int      clut_fmt;       /* CMODE bits 0-1 */
    int      clut_shift;     /* CMODE bits 2-6 */
    int      clut_mask;      /* CMODE bits 8-15 */
    int      clut_start;     /* CMODE bits 16-20, in units of 16 entries */

    int      blend;          /* ABE */
    int      blend_src, blend_dst, blend_op;   /* ALPHA */
    uint32_t blend_sfix, blend_dfix;           /* SFIX, DFIX */

    int      atest;          /* ATE */
    int      atest_func, atest_ref, atest_mask; /* ATST */

    int      sc_x0, sc_y0, sc_x1, sc_y1;        /* SCISSOR1/2, inclusive */

    int      ztest;          /* ZTE */
    int      zfunc;          /* ZTST: 0 never .. 7 greater-or-equal */
    int      zwrite;         /* !ZMSK */
    uint32_t zaddr;          /* ZBP/ZBW, in VRAM; 16 bits per pixel */
    int      zbw;            /* ZBW: width in pixels */
} psp_draw_state;

typedef struct {
    const char *name;

    int  (*init)(int width, int height);
    void (*shutdown)(void);

    /* GE_FBP / GE_FBW: the framebuffer being drawn into. */
    void (*set_target)(uint32_t addr, uint32_t stride, int fmt);

    /* One assembled primitive. `count` vertices, already in screen space,
     * drawn with state `st`. */
    void (*draw)(int prim, const psp_vertex *v, int count, const psp_draw_state *st);

    /* End of a display list -- a natural point to flush batched work. */
    void (*finish)(void);

    /* sceDisplaySetFrameBuf -- show what has accumulated. */
    void (*present)(void);

    /* Optional: the frame just presented, at the backend's internal
     * resolution. Returns 0 and fills px, w and h, or -1 when the frame only
     * exists in guest VRAM (the software renderer draws straight into it).
     * The buffer stays valid until the next present. */
    int (*frame)(const uint32_t **px, int *w, int *h);
} psp_render_backend;

/* Internal resolution multiplier, 1..4. Set before the first draw; the
 * software renderer ignores it and stays at 1x, because it is the oracle the
 * others are checked against. */
extern int psp_render_scale;

/* Sub-pixel vertices. The game places the notes and bar lines of the
 * gameplay lane at fractional pixel positions (a quarter of a pixel steps,
 * from the song clock), and the GE front end rounds every vertex to the 480x272
 * grid (ge.c read_vertex). With psp_render_subpixel != 0 the fraction is kept
 * in psp_vertex.fx/fy for those draws (scene 3 only, transformed vertices),
 * and a backend rendering above 1x may use it; otherwise fx == x, fy == y.
 * 0 = off, 1 = on (default). EMOTIONALSENSE_SUBPIXEL=0|1 in the game. */
extern int psp_render_subpixel;

/* Background video (BGA) smoothing, Vulkan backend, gameplay scene only. The
 * decoded video reaches the screen as a 512x512 VRAM texture that changes
 * every second vblank (it is a 30 fps video). 0 = as the game draws it
 * (default); 1 = "blend": the new video frame is held back one vblank and
 * the in-between vblank shows the 50 % average of the previous and the new
 * frame. Only that texture is touched; the lane, notes and HUD are not.
 * EMOTIONALSENSE_BGA_SMOOTH=0|blend in the game. */
extern int psp_render_bga_smooth;

/* --tex-filter: -1 keeps the game's TEXFILTER bit, 0 forces nearest, 1 forces
 * bilinear. For comparing the two, and for looking for seams at 2x and up. */
extern int psp_render_tex_filter;

/* The presented frame at internal resolution, or -1 if the backend has none
 * (then psp_display_read() is the way to get the 480x272 frame). */
int psp_render_frame(const uint32_t **px, int *w, int *h);

/* Button-glyph rewriting, for a host that shows other button symbols than the game draws.
 * A rule names a texture by the dump key (content + CLUT + format + size) and one square of
 * 18x18-ish texels that holds a button disc, and says which symbol that disc shows:
 * 'x' cross, 'o' circle, 's' square, 't' triangle. `rot` is the number of quarter turns
 * (clockwise) the texture is stored turned relative to how it is drawn (letters only).
 *
 * The host describes the look it wants per original symbol with a psp_glyph_map (id 0 =
 * the game's own, nothing is changed): `letter` != 0 repaints the disc with that letter
 * ('A', 'B', 'x' for the letter X, 'Y'; the strokes are rasterised in the renderer) in
 * `rgb` (0xRRGGBB, 0 = the glyph's own symbol colour); else `shows` != the original symbol
 * replaces the disc by the one of that symbol. Look index: 0 cross, 1 circle, 2 square,
 * 3 triangle. A change of map applies to textures uploaded afterwards (the cache keys
 * carry the id), so the host changes it at a quiet moment. The texture is changed in the
 * uploaded copy only: nothing is written anywhere, and the numbers in a rule are the only
 * game-specific data. Vulkan backend only. */
typedef struct {
    uint64_t key;
    int w, h;               /* the texture size, as part of the key */
    int x, y;               /* the square's corner in texels */
    int gw, gh;             /* its size */
    char sym;
    char rot;
} psp_glyph_rule;
typedef struct {
    char letter;
    char shows;             /* 'x' 'o' 's' 't', or 0 = the original */
    uint32_t rgb;
    char label[4];          /* look 4..7 only: what a button pill shows (see below); "" = the game's own */
} psp_glyph_look;
#define PSP_GLYPH_LOOKS 8
typedef struct {
    uint32_t id;            /* 0 = identity; every distinct look needs its own non-zero id */
    psp_glyph_look look[PSP_GLYPH_LOOKS];
} psp_glyph_map;
/* Button pills (round 36b). A rule whose `sym` is 'S' (START), 'E' (SELECT), 'l' (left shoulder) or
 * 'r' (right shoulder) names the *text box* of a pill drawn by the game (x, y, gw, gh in texels, inside
 * the pill's face): the renderer erases the game's word there (each row is blended between the two
 * texels just outside the box, so a gradient face or a black pill stays as it is) and draws the look's
 * `label`: "M" the Menu icon (three bars), "V" the View icon (two overlapping squares), "+" and "-"
 * (the Switch's plus and minus), or up to three characters of text ("L1", "RB", "L", "R": strokes
 * rasterised by the renderer), centred in the box, in `rgb` (0 = the dark of the original text).
 * Look index 4 START, 5 SELECT, 6 left shoulder, 7 right shoulder. */
void psp_render_set_glyph_rules(const psp_glyph_rule *rules, int n);
void psp_render_set_glyph_map(const psp_glyph_map *m);

/* Texture dump (EMOTIONALSENSE_TEX_DUMP). Writes RGBA8 PNGs, no dependency. */
int  psp_tex_png_write(const char *path, const uint32_t *rgba, int w, int h);
int  psp_tex_png_read(const char *path, uint32_t **rgba, int *w, int *h);
void psp_tex_dump_report(FILE *f);
void psp_tex_pack_report(FILE *f);

/* Select a backend by name ("software", "null", ...). Returns 0 on success,
 * -1 if the name is unknown, leaving the current backend in place. */
int psp_render_select(const char *name);

/* The active backend. Never NULL -- defaults to software. */
const psp_render_backend *psp_render_current(void);

/* Backends provided by the runtime. */
extern const psp_render_backend psp_render_software;
extern const psp_render_backend psp_render_null;
extern const psp_render_backend psp_render_vulkan;

/* Pixels written by the active backend -- the proof of life during bring-up. */
uint64_t psp_render_pixels(void);
void     psp_render_add_pixels(uint64_t n);
void     psp_render_reset_pixels(void);

/* One pixel in format 0-3 (565, 5551, 4444, 8888) as 0xAABBGGRR. The 16-bit
 * formats replicate their high bits into the low ones. */
uint32_t psp_color_expand(uint32_t px, int fmt);

/* The texel at integer coordinates (x, y), wrapped or clamped, as 0xAABBGGRR.
 * Anything undecodable returns opaque magenta and is counted. */
uint32_t psp_tex_fetch(const psp_draw_state *st, int x, int y);
uint64_t psp_render_unsupported_texels(void);

#endif
