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

/* ---- reader ---------------------------------------------------------------
 *
 * Enough PNG to load an upscaled texture pack: 8-bit, non-interlaced, colour
 * types 0/2/3/4/6. The inflate is the small canonical-Huffman decoder from
 * puff.c's shape, so the toolkit still needs no zlib. */

typedef struct { const uint8_t *p; size_t n, i; uint32_t bits; int nbits; } br;
typedef struct { short count[16], symbol[288]; } huff;

static int br_bits(br *b, int n) {
    while (b->nbits < n) {
        if (b->i >= b->n) return -1;
        b->bits |= (uint32_t)b->p[b->i++] << b->nbits;
        b->nbits += 8;
    }
    int v = (int)(b->bits & ((1u << n) - 1u));
    b->bits >>= n;
    b->nbits -= n;
    return v;
}

static int huff_build(huff *h, const uint8_t *lengths, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (short)i;
    return 0;
}

static int huff_decode(br *b, const huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int bit = br_bits(b, 1);
        if (bit < 0) return -1;
        code |= bit;
        int c = h->count[len];
        if (code - first < c) return h->symbol[index + (code - first)];
        index += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    return -1;
}

static const short LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short LEXT[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
static const short DEXT[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inflate_codes(br *b, uint8_t *out, size_t out_n, size_t *pos,
                         const huff *lit, const huff *dst) {
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*pos >= out_n) return -1;
            out[(*pos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int extra = br_bits(b, LEXT[sym]);
            if (extra < 0) return -1;
            size_t len = (size_t)(LBASE[sym] + extra);
            int ds = huff_decode(b, dst);
            if (ds < 0 || ds >= 30) return -1;
            extra = br_bits(b, DEXT[ds]);
            if (extra < 0) return -1;
            size_t dist = (size_t)(DBASE[ds] + extra);
            if (dist > *pos) return -1;
            for (size_t k = 0; k < len; k++) {
                if (*pos >= out_n) return -1;
                out[*pos] = out[*pos - dist];
                (*pos)++;
            }
        }
    }
}

static int inflate_dynamic(br *b, uint8_t *out, size_t out_n, size_t *pos) {
    static const uint8_t ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                       11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int hlit = br_bits(b, 5), hdist = br_bits(b, 5), hclen = br_bits(b, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) return -1;

    uint8_t lens[320];
    memset(lens, 0, sizeof lens);
    for (int i = 0; i < hclen; i++) {
        int v = br_bits(b, 3);
        if (v < 0) return -1;
        lens[ORDER[i]] = (uint8_t)v;
    }
    huff cl;
    if (huff_build(&cl, lens, 19) != 0) return -1;

    uint8_t lengths[320];
    int n = 0;
    while (n < hlit + hdist) {
        int sym = huff_decode(b, &cl);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else {
            int rep, val = 0;
            if (sym == 16) {
                if (n == 0) return -1;
                val = lengths[n - 1];
                rep = br_bits(b, 2);
                if (rep < 0) return -1;
                rep += 3;
            } else if (sym == 17) {
                rep = br_bits(b, 3);
                if (rep < 0) return -1;
                rep += 3;
            } else {
                rep = br_bits(b, 7);
                if (rep < 0) return -1;
                rep += 11;
            }
            if (n + rep > hlit + hdist) return -1;
            while (rep--) lengths[n++] = (uint8_t)val;
        }
    }
    if (lengths[256] == 0) return -1;

    huff lit, dst;
    if (huff_build(&lit, lengths, hlit) != 0) return -1;
    if (huff_build(&dst, lengths + hlit, hdist) != 0) return -1;
    return inflate_codes(b, out, out_n, pos, &lit, &dst);
}

static int inflate_fixed(br *b, uint8_t *out, size_t out_n, size_t *pos) {
    uint8_t lengths[288];
    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    huff lit;
    if (huff_build(&lit, lengths, 288) != 0) return -1;
    for (int i = 0; i < 30; i++) lengths[i] = 5;
    huff dst;
    if (huff_build(&dst, lengths, 30) != 0) return -1;
    return inflate_codes(b, out, out_n, pos, &lit, &dst);
}

/* zlib stream -> exactly out_n bytes. */
static int inflate_zlib(const uint8_t *in, size_t in_n, uint8_t *out, size_t out_n) {
    if (in_n < 6) return -1;
    if ((in[0] & 0x0F) != 8) return -1;                 /* deflate */
    if (((in[0] << 8) | in[1]) % 31 != 0) return -1;
    if (in[1] & 0x20) return -1;                        /* preset dictionary */

    br b = { in + 2, in_n - 2, 0, 0, 0 };
    size_t pos = 0;
    int final;
    do {
        final = br_bits(&b, 1);
        int type = br_bits(&b, 2);
        if (final < 0 || type < 0) return -1;
        if (type == 0) {
            b.bits = 0;
            b.nbits = 0;                                /* stored blocks are byte aligned */
            if (b.i + 4 > b.n) return -1;
            unsigned len = (unsigned)b.p[b.i] | ((unsigned)b.p[b.i + 1] << 8);
            unsigned nlen = (unsigned)b.p[b.i + 2] | ((unsigned)b.p[b.i + 3] << 8);
            b.i += 4;
            if ((len ^ 0xFFFFu) != nlen) return -1;
            if (b.i + len > b.n || pos + len > out_n) return -1;
            memcpy(out + pos, b.p + b.i, len);
            b.i += len;
            pos += len;
        } else if (type == 1) {
            if (inflate_fixed(&b, out, out_n, &pos) != 0) return -1;
        } else if (type == 2) {
            if (inflate_dynamic(&b, out, out_n, &pos) != 0) return -1;
        } else {
            return -1;
        }
    } while (!final);
    return pos == out_n ? 0 : -1;
}

static uint32_t be32_at(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int psp_tex_png_read(const char *path, uint32_t **rgba, int *out_w, int *out_h) {
    if (!path || !rgba || !out_w || !out_h) return -1;
    *rgba = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (1L << 28)) { fclose(f); return -1; }
    rewind(f);
    uint8_t *file = malloc((size_t)sz);
    if (!file) { fclose(f); return -1; }
    size_t got = fread(file, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(file); return -1; }

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (sz < 8 || memcmp(file, sig, 8) != 0) { free(file); return -1; }

    int w = 0, h = 0, depth = 0, color = 0, interlace = 0;
    uint8_t palette[256 * 3];
    int palette_n = 0;
    uint8_t *idat = NULL;
    size_t idat_n = 0;
    int rc = -1;

    size_t off = 8;
    while (off + 8 <= (size_t)sz) {
        uint32_t len = be32_at(file + off);
        const uint8_t *type = file + off + 4;
        const uint8_t *data = file + off + 8;
        if (off + 12 + len > (size_t)sz) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)be32_at(data);
            h = (int)be32_at(data + 4);
            depth = data[8];
            color = data[9];
            interlace = data[12];
        } else if (!memcmp(type, "PLTE", 4) && len >= 3) {
            palette_n = (int)(len / 3u);
            if (palette_n > 256) palette_n = 256;
            memcpy(palette, data, (size_t)palette_n * 3u);
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *n = realloc(idat, idat_n + len);
            if (!n) goto done;
            idat = n;
            memcpy(idat + idat_n, data, len);
            idat_n += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        off += 12 + len;
    }

    if (!idat || w <= 0 || h <= 0 || depth != 8 || interlace != 0) goto done;

    int channels;
    switch (color) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: goto done;
    }
    if (color == 3 && palette_n == 0) goto done;

    size_t stride = (size_t)w * (size_t)channels;
    size_t raw_n = (stride + 1u) * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) goto done;
    if (inflate_zlib(idat, idat_n, raw, raw_n) != 0) { free(raw); goto done; }

    /* Undo the per-row filters in place. */
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1u);
        int filter = row[0];
        uint8_t *cur = row + 1;
        const uint8_t *prev = y ? raw + (size_t)(y - 1) * (stride + 1u) + 1 : NULL;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)channels ? cur[x - channels] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (size_t)channels) ? prev[x - channels] : 0;
            switch (filter) {
            case 0: break;
            case 1: cur[x] = (uint8_t)(cur[x] + a); break;
            case 2: cur[x] = (uint8_t)(cur[x] + b); break;
            case 3: cur[x] = (uint8_t)(cur[x] + ((a + b) >> 1)); break;
            case 4: {
                int p = a + b - c;
                int pa = p > a ? p - a : a - p;
                int pb = p > b ? p - b : b - p;
                int pc = p > c ? p - c : c - p;
                cur[x] = (uint8_t)(cur[x] + ((pa <= pb && pa <= pc) ? a : pb <= pc ? b : c));
                break;
            }
            default: free(raw); goto done;
            }
        }
    }

    uint32_t *out = malloc((size_t)w * (size_t)h * 4u);
    if (!out) { free(raw); goto done; }
    for (int y = 0; y < h; y++) {
        const uint8_t *row = raw + (size_t)y * (stride + 1u) + 1;
        for (int x = 0; x < w; x++) {
            const uint8_t *s = row + (size_t)x * (size_t)channels;
            uint32_t r, g, b2, a;
            switch (color) {
            case 0: r = g = b2 = s[0]; a = 255; break;
            case 2: r = s[0]; g = s[1]; b2 = s[2]; a = 255; break;
            case 3: {
                int idx = s[0];
                if (idx >= palette_n) idx = 0;
                r = palette[idx * 3]; g = palette[idx * 3 + 1]; b2 = palette[idx * 3 + 2];
                a = 255;
                break;
            }
            case 4: r = g = b2 = s[0]; a = s[1]; break;
            default: r = s[0]; g = s[1]; b2 = s[2]; a = s[3]; break;
            }
            out[(size_t)y * (size_t)w + (size_t)x] =
                (a << 24) | (b2 << 16) | (g << 8) | r;
        }
    }
    free(raw);
    *rgba = out;
    *out_w = w;
    *out_h = h;
    rc = 0;

done:
    free(idat);
    free(file);
    return rc;
}
