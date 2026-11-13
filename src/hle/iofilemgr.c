/* psprecomp — IoFileMgrForUser.
 *
 * The PSP's file API, mapped onto a host directory. Games address the UMD as
 * `disc0:/` and the Memory Stick as `ms0:/`, so those prefixes are rewritten to
 * subdirectories of a root the host chooses.
 *
 * Reads go straight into guest memory, which means a game loading assets is
 * doing the real thing -- and a texture or model that arrives byte-correct is
 * strong evidence the recompiled code around it is behaving too.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <io.h>
#  include <direct.h>
#  include <sys/stat.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#define MAX_FILES 64
#define MAX_DIRS  16

/* Open flags, as the guest passes them. */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

typedef struct { FILE *f; int used; int raw; uint64_t lba; } io_file;

typedef struct {
    int used;
#ifdef _WIN32
    intptr_t handle;
    struct _finddata_t data;
    int first;
    int done;
#else
    DIR *dir;
#endif
} io_dir;

static io_file g_file[MAX_FILES];
static int64_t g_async_res[MAX_FILES];
static io_dir  g_dir[MAX_DIRS];
static char    g_root[512];
static uint64_t g_bytes_read;

/* Raw UMD sectors. The game's disc check Getstats a container, seeks the raw
 * device to the file's start LBA, reads one sector and checksums it, so
 * serving sectors out of the extracted file is enough. LBAs are this disc's.
 * shortcut: only these containers are mapped; add entries if another file is
 * ever read raw. */
static const struct { const char *guest; uint64_t lba; } g_disc_lba[] = {
    { "disc0:/PSP_GAME/INSDIR/DATA.TPA",    54496 },
    { "disc0:/PSP_GAME/INSDIR/STREAMI.TPA", 155184 },
    { "disc0:/PSP_GAME/INSDIR/STREAMS.TPA", 189024 },
    { "disc0:/PSP_GAME/USRDIR/STREAMV.TPA", 469104 },
};

void psp_io_set_root(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
}

const char *psp_io_root(void) { return g_root; }

void psp_io_reset(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used && g_file[i].f) fclose(g_file[i].f);
        g_file[i].used = 0;
        g_file[i].f = NULL;
        g_file[i].raw = 0;
    }
    memset(g_dir, 0, sizeof g_dir);
    g_bytes_read = 0;
    if (!g_root[0]) psp_io_set_root(".");
}

void psp_io_init(void) { g_root[0] = '\0'; psp_io_reset(); }

uint64_t psp_io_bytes_read(void) { return g_bytes_read; }

/* Rewrite a PSP path into a host path. The device prefix becomes a
 * subdirectory so a disc image and a memory-stick image can coexist under one
 * root without colliding. */
static void map_path(const char *guest, char *out, size_t cap) {
    const char *p = guest;
    const char *sub = "disc";

    if      (!strncmp(p, "disc0:", 6)) { p += 6; sub = "disc"; }
    else if (!strncmp(p, "umd0:",  5)) { p += 5; sub = "disc"; }
    else if (!strncmp(p, "ms0:",   4)) { p += 4; sub = "ms";   }
    else if (!strncmp(p, "flash0:",7)) { p += 7; sub = "flash";}
    else if (!strncmp(p, "host0:", 6)) { p += 6; sub = "host"; }

    while (*p == '/' || *p == '\\') p++;
    snprintf(out, cap, "%s/%s/%s", g_root, sub, p);
}

static void hle_Open(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t flags = psp_arg(1);

    /* The raw UMD device: no host file, just a sector cursor. */
    if (!strncmp(guest, "umd0:", 5) && (guest[5] == '\0' || guest[5] == '/')) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (g_file[i].used) continue;
            g_file[i].used = 1;
            g_file[i].raw = 1;
            g_file[i].lba = 0;
            psp_ret((uint32_t)(i + 3));
            return;
        }
        psp_ret(0x80010018);
        return;
    }

    map_path(guest, host, sizeof host);

    const char *mode = "rb";
    if (flags & PSP_O_TRUNC)                  mode = (flags & PSP_O_RDWR) == PSP_O_RDWR ? "w+b" : "wb";
    else if (flags & PSP_O_APPEND)            mode = "ab";
    else if ((flags & PSP_O_RDWR) == PSP_O_RDWR) mode = "r+b";
    else if (flags & PSP_O_WRONLY)            mode = (flags & PSP_O_CREAT) ? "wb" : "r+b";

    FILE *f = fopen(host, mode);
    if (!f && (flags & PSP_O_CREAT)) f = fopen(host, "w+b");
    /* PSPRECOMP_TRACE_IO=1 names every open and whether it worked. A game
     * that cannot find its assets usually does not crash; it waits. */
    if (getenv("PSPRECOMP_TRACE_IO"))
        fprintf(stderr, "io: open \"%s\" -> \"%s\" flags 0x%X: %s\n",
                guest, host, flags, f ? "ok" : "not found");
    if (!f) {
        /* A failed open is normal (a game probing for a save file) and is not
         * worth a warning, but the mapped path is worth knowing when a game
         * cannot find assets it expects. */
        psp_ret(0x80010002);          /* ENOENT */
        return;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used) continue;
        g_file[i].f = f;
        g_file[i].used = 1;
        psp_ret((uint32_t)(i + 3));   /* 0-2 are reserved for the std streams */
        return;
    }
    fclose(f);
    psp_ret(0x80010018);              /* too many open files */
}

static io_file *fd_arg(void) {
    int32_t fd = (int32_t)psp_arg(0) - 3;
    if (fd < 0 || fd >= MAX_FILES || !g_file[fd].used) return NULL;
    return &g_file[fd];
}

static void hle_Close(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(0x80020323); return; }
    if (h->f) fclose(h->f);
    h->f = NULL;
    h->raw = 0;
    h->used = 0;
    psp_ret(0);
}

/* Fill dst from the container whose LBA range covers the cursor. Returns the
 * bytes read and advances the cursor by whole sectors. */
static int raw_read(io_file *h, uint8_t *dst, uint32_t bytes) {
    const char *guest = NULL;
    uint64_t base = 0;
    for (size_t i = 0; i < sizeof g_disc_lba / sizeof *g_disc_lba; i++) {
        if (h->lba >= g_disc_lba[i].lba && g_disc_lba[i].lba >= base) {
            base = g_disc_lba[i].lba;
            guest = g_disc_lba[i].guest;
        }
    }
    if (!guest) return 0;
    char host[1024];
    map_path(guest, host, sizeof host);
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    fseek(f, (long)((h->lba - base) * 2048), SEEK_SET);
    size_t got = fread(dst, 1, bytes, f);
    fclose(f);
    h->lba += (got + 2047) / 2048;
    return (int)got;
}

static void hle_Read(void) {
    io_file *h = fd_arg();
    uint32_t dst = psp_arg(1), size = psp_arg(2);
    if (!h) { psp_ret(0x80020323); return; }
    if (!dst) { psp_ret(0x80020130); return; }
    if (!size) { psp_ret(0); return; }

    if (h->raw) {
        uint8_t *tmp = (uint8_t *)malloc(size);
        if (!tmp) { psp_ret(0x80020190); return; }
        int got = raw_read(h, tmp, size);
        for (int i = 0; i < got; i++) psp_write8(dst + (uint32_t)i, tmp[i]);
        free(tmp);
        g_bytes_read += (uint64_t)got;
        psp_ret((uint32_t)got);
        return;
    }

    /* Read through a host buffer and then place it, so a read that straddles
     * the end of a guest region is rejected by the memory layer rather than
     * writing past it. */
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) { psp_ret(0x80020190); return; }

    size_t got = fread(tmp, 1, size, h->f);
    if (got && psp_mem_write_block(dst, tmp, (uint32_t)got) != 0) {
        /* Fall back to byte-at-a-time so a partially mapped destination still
         * gets what fits, and the bad-access counter records the rest. */
        for (size_t i = 0; i < got; i++) psp_write8(dst + (uint32_t)i, tmp[i]);
    }
    free(tmp);
    g_bytes_read += got;
    psp_ret((uint32_t)got);
}

static void hle_Write(void) {
    io_file *h = fd_arg();
    uint32_t src = psp_arg(1), size = psp_arg(2);

    /* fd 1 and 2 are stdout/stderr: a game writing there is talking to us. */
    int32_t fd = (int32_t)psp_arg(0);
    if (fd == 1 || fd == 2) {
        for (uint32_t i = 0; i < size; i++) fputc(psp_read8(src + i), stderr);
        psp_ret(size);
        return;
    }
    if (!h) { psp_ret(0x80020323); return; }

    uint8_t *tmp = (uint8_t *)malloc(size ? size : 1);
    if (!tmp) { psp_ret(0x80020190); return; }
    for (uint32_t i = 0; i < size; i++) tmp[i] = psp_read8(src + i);
    size_t put = fwrite(tmp, 1, size, h->f);
    free(tmp);
    psp_ret((uint32_t)put);
}

/* sceIoLseek takes a 64-bit offset and returns one. Under o32 a 64-bit
 * argument is register-aligned, so it lands in $a2:$a3 rather than $a1:$a2 --
 * and the result comes back in $v0:$v1. Getting either wrong makes every seek
 * land somewhere plausible but wrong. */
static void hle_Lseek(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    uint32_t whence = psp_arg(4);
    if (!h) { psp_ret(0x80020323); return; }

    if (h->raw) {
        /* Raw device positions are in 2048-byte sectors. */
        if (whence == 0) h->lba = off;
        else if (whence == 1) h->lba += off;
        psp_cpu.r[PSP_REG_V0] = (uint32_t)h->lba;
        psp_cpu.r[PSP_REG_V1] = (uint32_t)(h->lba >> 32);
        return;
    }

    int w = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
    if (fseek(h->f, (long)(int64_t)off, w) != 0) { psp_ret(0xFFFFFFFFu); return; }

    long pos = ftell(h->f);
    psp_cpu.r[PSP_REG_V0] = (uint32_t)pos;
    psp_cpu.r[PSP_REG_V1] = 0;
}

static void hle_Rename(void) {
    char a[512], b[512], ha[1024], hb[1024];
    psp_str(psp_arg(0), a, sizeof a);
    psp_str(psp_arg(1), b, sizeof b);
    map_path(a, ha, sizeof ha);
    map_path(b, hb, sizeof hb);
    psp_ret(rename(ha, hb) == 0 ? 0 : 0x80010002);
}

/* ---- directories --------------------------------------------------------- */

static void hle_Dopen(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    if (getenv("PSPRECOMP_TRACE_IO")) fprintf(stderr, "io: Dopen %s\n", guest);

    for (int i = 0; i < MAX_DIRS; i++) {
        if (g_dir[i].used) continue;
#ifdef _WIN32
        char pattern[1088];
        snprintf(pattern, sizeof pattern, "%s/*", host);
        g_dir[i].handle = _findfirst(pattern, &g_dir[i].data);
        if (g_dir[i].handle == -1) { psp_ret(0x80010002); return; }
        g_dir[i].first = 1;
        g_dir[i].done = 0;
#else
        g_dir[i].dir = opendir(host);
        if (!g_dir[i].dir) { psp_ret(0x80010002); return; }
#endif
        g_dir[i].used = 1;
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(0x80010018);
}

/* Fill a SceIoDirent. Only the name is populated: games use Dread to enumerate
 * save slots and asset directories by name, and a wrong stat block would be
 * worse than an empty one. */
static void hle_Dread(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    uint32_t dirent = psp_arg(1);
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }

    const char *name = NULL;
#ifdef _WIN32
    if (g_dir[id].done) { psp_ret(0); return; }
    if (g_dir[id].first) {
        g_dir[id].first = 0;
        name = g_dir[id].data.name;
    } else if (_findnext(g_dir[id].handle, &g_dir[id].data) == 0) {
        name = g_dir[id].data.name;
    } else {
        g_dir[id].done = 1;
        psp_ret(0);
        return;
    }
#else
    struct dirent *de = readdir(g_dir[id].dir);
    if (!de) { psp_ret(0); return; }
    name = de->d_name;
#endif

    /* SceIoDirent: a 52-byte SceIoStat, then char d_name[256]. */
    for (int i = 0; i < 52; i++) psp_write8(dirent + (uint32_t)i, 0);
    uint32_t at = dirent + 52;
    for (uint32_t i = 0; i < 255 && name[i]; i++) psp_write8(at + i, (uint8_t)name[i]);
    psp_write8(at + (uint32_t)strlen(name), 0);

    psp_ret(1);                       /* more entries may follow */
}

static void hle_Dclose(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
#ifdef _WIN32
    if (g_dir[id].handle != -1) _findclose(g_dir[id].handle);
#else
    if (g_dir[id].dir) closedir(g_dir[id].dir);
#endif
    g_dir[id].used = 0;
    psp_ret(0);
}

/* ---- async I/O and filesystem helpers ------------------------------------ */

static void hle_OpenAsync(void) {
    hle_Open();
    int32_t fd = (int32_t)psp_cpu.r[PSP_REG_V0];
    if (fd >= 3 && fd - 3 < MAX_FILES) {
        g_async_res[fd - 3] = fd;
        /* Return the file descriptor as an async handle */
        psp_ret((uint32_t)fd);
    }
}

static void hle_CloseAsync(void) {
    int32_t idx = (int32_t)psp_arg(0) - 3;
    hle_Close();
    if (idx >= 0 && idx < MAX_FILES && psp_cpu.r[PSP_REG_V0] == 0) {
        g_async_res[idx] = 0;
        psp_ret(0);
    }
}

static void hle_ReadAsync(void) {
    int32_t idx = (int32_t)psp_arg(0) - 3;
    hle_Read();
    int32_t got = (int32_t)psp_cpu.r[PSP_REG_V0];
    if (idx >= 0 && idx < MAX_FILES && got >= 0) {
        g_async_res[idx] = got;
        psp_ret(0);
    }
}

static void hle_WriteAsync(void) {
    int32_t idx = (int32_t)psp_arg(0) - 3;
    hle_Write();
    int32_t put = (int32_t)psp_cpu.r[PSP_REG_V0];
    if (idx >= 0 && idx < MAX_FILES && put >= 0) {
        g_async_res[idx] = put;
        psp_ret(0);
    }
}

static void hle_LseekAsync(void) {
    int32_t idx = (int32_t)psp_arg(0) - 3;
    hle_Lseek();
    uint32_t lo = psp_cpu.r[PSP_REG_V0], hi = psp_cpu.r[PSP_REG_V1];
    if (idx >= 0 && idx < MAX_FILES && lo != 0xFFFFFFFFu) {
        g_async_res[idx] = (int64_t)((uint64_t)lo | ((uint64_t)hi << 32));
        psp_ret(0);
    }
}

static void hle_WaitAsync(void) {
    int32_t idx = (int32_t)psp_arg(0) - 3;
    uint32_t res_ptr = psp_arg(1);
    if (idx < 0 || idx >= MAX_FILES) { psp_ret(0x80020323); return; }
    uint64_t r = (uint64_t)g_async_res[idx];
    if (res_ptr) {
        psp_write32(res_ptr + 0, (uint32_t)r);
        psp_write32(res_ptr + 4, (uint32_t)(r >> 32));
    }
    psp_ret(0);   /* 0 = operation completed */
}

static void hle_Getstat(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t st_ptr = psp_arg(1);
    map_path(guest, host, sizeof host);
    struct stat st;
    if (stat(host, &st) != 0) { psp_ret(0x80010002); return; }
    if (st_ptr) {
        for (uint32_t i = 0; i < 88; i += 4) psp_write32(st_ptr + i, 0);
        int is_dir = S_ISDIR(st.st_mode);
        psp_write32(st_ptr + 0, (is_dir ? 0x1000u : 0x2000u) | 0x01FFu);
        psp_write32(st_ptr + 4, is_dir ? 0x10u : 0x20u);
        /* SceIoStat: mode, attr, three 16-byte dates, u64 size at 56,
         * st_private[6] at 64. The disc check reads st_private[0] as the
         * file's start LBA. */
        uint64_t sz = is_dir ? 0 : (uint64_t)st.st_size;
        psp_write32(st_ptr + 56, (uint32_t)sz);
        psp_write32(st_ptr + 60, (uint32_t)(sz >> 32));
        for (size_t i = 0; i < sizeof g_disc_lba / sizeof *g_disc_lba; i++)
            if (!strcmp(guest, g_disc_lba[i].guest)) {
                psp_write32(st_ptr + 64, (uint32_t)g_disc_lba[i].lba);
                psp_write32(st_ptr + 68, (uint32_t)(g_disc_lba[i].lba >> 32));
            }
    }
    psp_ret(0);
}

static void hle_Chstat(void) { psp_ret(0); }

static void hle_Mkdir(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
#ifdef _WIN32
    _mkdir(host);
#else
    mkdir(host, 0755);
#endif
    psp_ret(0);
}

static void hle_Remove(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    psp_ret(remove(host) == 0 ? 0 : 0x80010002);
}

static void hle_Rmdir(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
#ifdef _WIN32
    psp_ret(_rmdir(host) == 0 ? 0 : 0x80010002);
#else
    psp_ret(rmdir(host) == 0 ? 0 : 0x80010002);
#endif
}

static void hle_Devctl(void) {
    /* (dev, cmd, indata, inlen, outdata, outlen) */
    uint32_t cmd = psp_arg(1);
    uint32_t indata = psp_arg(2);
    uint32_t outdata = psp_arg(4);
    /* 0x02425818: get free space on ms0: (writes SceDevInf via pointer at indata). */
    if (cmd == 0x02425818u && indata) {
        uint32_t buf = psp_read32(indata);
        if (buf) {
            psp_write32(buf + 0,  65536);  /* max clusters */
            psp_write32(buf + 4,  32768);  /* free clusters (~1 GiB) */
            psp_write32(buf + 8,  32768);  /* max sectors */
            psp_write32(buf + 12, 512);    /* sector size */
            psp_write32(buf + 16, 64);     /* sectors per cluster */
        }
    } else if (outdata) {
        psp_write32(outdata, 1);
    }
    psp_ret(0);
}

/* The anti-copy check at 0x14868 times two raw UMD reads. It requires >=20 ms
 * for read 1, >=30 ms for read 2, and >=50 ms from the end of CRC 1 through
 * seek 2 + read 2. Seek 25 + read 35 gives 35/35/60. */
#define UMD_SEEK_US 25000
#define UMD_READ_US 35000

static void hle_Ioctl(void) {
    io_file *h = fd_arg();
    uint32_t cmd = psp_arg(1);
    uint32_t indata = psp_arg(2);
    uint32_t outdata = psp_arg(4), outlen = psp_arg(5);

    if (h && h->raw) {
        int trace = getenv("PSPRECOMP_TRACE_IO") != NULL;
        uint64_t t0 = trace ? psp_clock_us() : 0;
        if (cmd == 0x01F100A6u && indata) {
            /* Seek to a 64-bit LBA (input is {u64 lba, u64 pad}). */
            h->lba = (uint64_t)psp_read32(indata) | ((uint64_t)psp_read32(indata + 4) << 32);
            psp_threadman_sleep_real(UMD_SEEK_US);
            if (trace)
                fprintf(stderr, "io: ioctl raw seek lba=%llu dt=%llums\n",
                        (unsigned long long)h->lba,
                        (unsigned long long)(psp_clock_us() - t0) / 1000);
            psp_ret(0);
            return;
        }
        if (cmd == 0x01F30003u) {
            /* Uncached read of N sectors; returns the sector count. */
            uint32_t sectors = indata ? psp_read32(indata) : 1;
            uint32_t bytes = sectors * 2048;
            if (bytes > outlen) bytes = outlen;
            uint8_t *tmp = (uint8_t *)malloc(bytes ? bytes : 1);
            if (!tmp) { psp_ret(0x80020190); return; }
            psp_threadman_sleep_real(UMD_READ_US);
            int got = raw_read(h, tmp, bytes);
            for (int i = 0; i < got; i++) psp_write8(outdata + (uint32_t)i, tmp[i]);
            free(tmp);
            if (trace)
                fprintf(stderr, "io: ioctl raw read lba=%llu sectors=%u dt=%llums\n",
                        (unsigned long long)h->lba, sectors,
                        (unsigned long long)(psp_clock_us() - t0) / 1000);
            psp_ret((uint32_t)((got + 2047) / 2048));
            return;
        }
        if (trace)
            fprintf(stderr, "io: ioctl raw cmd 0x%08X in=0x%08X out=0x%08X/%u lba=%llu (no-op)\n",
                    cmd, indata, outdata, outlen, (unsigned long long)h->lba);
    }
    psp_ret(0);
}

static void hle_Chdir(void) {
    /* DJMAX uses Chdir to point to directory context; our map_path handles absolute paths */
    psp_ret(0);
}

void psp_io_register(void) {
    psp_hle_register(0x109F50BC, "IoFileMgrForUser", "sceIoOpen",        hle_Open);
    psp_hle_register(0x89AA9906, "IoFileMgrForUser", "sceIoOpenAsync",   hle_OpenAsync);
    psp_hle_register(0x810C4BC3, "IoFileMgrForUser", "sceIoClose",       hle_Close);
    psp_hle_register(0xFF5940B6, "IoFileMgrForUser", "sceIoCloseAsync",  hle_CloseAsync);
    psp_hle_register(0x6A638D83, "IoFileMgrForUser", "sceIoRead",        hle_Read);
    psp_hle_register(0xA0B5A7C2, "IoFileMgrForUser", "sceIoReadAsync",   hle_ReadAsync);
    psp_hle_register(0x42EC03AC, "IoFileMgrForUser", "sceIoWrite",       hle_Write);
    psp_hle_register(0x0FACAB19, "IoFileMgrForUser", "sceIoWriteAsync",  hle_WriteAsync);
    psp_hle_register(0x27EB27B8, "IoFileMgrForUser", "sceIoLseek",       hle_Lseek);
    psp_hle_register(0x71B19E77, "IoFileMgrForUser", "sceIoLseekAsync",  hle_LseekAsync);
    psp_hle_register(0xE23EEC33, "IoFileMgrForUser", "sceIoWaitAsync",   hle_WaitAsync);
    psp_hle_register(0x35DBD746, "IoFileMgrForUser", "sceIoWaitAsyncCB", hle_WaitAsync);
    psp_hle_register(0x3251EA56, "IoFileMgrForUser", "sceIoPollAsync",   hle_WaitAsync);
    psp_hle_register(0xACE946E8, "IoFileMgrForUser", "sceIoGetstat",     hle_Getstat);
    psp_hle_register(0xB8A740F4, "IoFileMgrForUser", "sceIoChstat",      hle_Chstat);
    psp_hle_register(0x06A70004, "IoFileMgrForUser", "sceIoMkdir",       hle_Mkdir);
    psp_hle_register(0xF27A9C51, "IoFileMgrForUser", "sceIoRemove",      hle_Remove);
    psp_hle_register(0x1117C65F, "IoFileMgrForUser", "sceIoRmdir",       hle_Rmdir);
    psp_hle_register(0x55F4717D, "IoFileMgrForUser", "sceIoChdir",       hle_Chdir);
    psp_hle_register(0x54F5FB11, "IoFileMgrForUser", "sceIoDevctl",      hle_Devctl);
    psp_hle_register(0x63632449, "IoFileMgrForUser", "sceIoIoctl",       hle_Ioctl);
    psp_hle_register(0x779103A0, "IoFileMgrForUser", "sceIoRename",      hle_Rename);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",       hle_Dopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",       hle_Dread);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose",      hle_Dclose);
}
