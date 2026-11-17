/* emotionalsense -- minimal PNG writer for the texture dump.
 *
 * RGBA8 only, deflate with stored blocks: no compression, no dependency.
 * The dump is a diagnostic and the input of the external upscaler, so file
 * size does not matter here; keeping zlib out of the toolkit does.
 */

#include "psprecomp/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_crc_table[256];
static int g_crc_ready;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[n] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n) {
    if (!g_crc_ready) crc_init();
    for (size_t i = 0; i < n; i++) c = g_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int put_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    uint32_t c = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    if (len) c = crc_update(c, data, len);
    uint8_t tail[4];
    be32(tail, c ^ 0xFFFFFFFFu);
    return fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

int psp_tex_png_write(const char *path, const uint32_t *rgba, int w, int h) {
    if (!path || !rgba || w <= 0 || h <= 0) return -1;

    size_t stride = (size_t)w * 4u + 1u;          /* filter byte + RGBA */
    size_t raw_n = stride * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        row[0] = 0;                                /* filter: none */
        memcpy(row + 1, rgba + (size_t)y * (size_t)w, (size_t)w * 4u);
    }

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); return -1; }

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    int rc = fwrite(sig, 1, 8, f) == 8 ? 0 : -1;

    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 6;    /* colour type: RGBA */
    ihdr[10] = 0;   /* deflate */
    ihdr[11] = 0;   /* adaptive filtering */
    ihdr[12] = 0;   /* no interlace */
    if (!rc) rc = put_chunk(f, "IHDR", ihdr, 13);

    /* zlib stream: header, stored deflate blocks, adler32. Built in memory
     * because PNG wants it inside a single IDAT chunk. */
    if (!rc) {
        size_t blocks = raw_n / 65535u + 1u;
        size_t zn = raw_n + blocks * 5u + 6u;
        uint8_t *z = malloc(zn);
        if (!z) {
            rc = -1;
        } else {
            size_t o = 0;
            z[o++] = 0x78; z[o++] = 0x01;
            for (size_t off = 0; off < raw_n; ) {
                size_t n = raw_n - off;
                if (n > 65535u) n = 65535u;
                z[o++] = (uint8_t)(off + n >= raw_n ? 1 : 0);   /* BFINAL, stored */
                z[o++] = (uint8_t)(n & 0xFFu);
                z[o++] = (uint8_t)(n >> 8);
                z[o++] = (uint8_t)(~n & 0xFFu);
                z[o++] = (uint8_t)((~n >> 8) & 0xFFu);
                memcpy(z + o, raw + off, n);
                o += n;
                off += n;
            }
            uint32_t a = 1, b = 0;
            for (size_t i = 0; i < raw_n; i++) {
                a = (a + raw[i]) % 65521u;
                b = (b + a) % 65521u;
            }
            z[o++] = (uint8_t)((b << 16 | a) >> 24);
            z[o++] = (uint8_t)((b << 16 | a) >> 16);
            z[o++] = (uint8_t)(a >> 8);
            z[o++] = (uint8_t)a;
            rc = put_chunk(f, "IDAT", z, (uint32_t)o);
            free(z);
        }
    }
    if (!rc) rc = put_chunk(f, "IEND", NULL, 0);

    free(raw);
    if (fclose(f) != 0) rc = -1;
    if (rc) remove(path);
    return rc;
}
