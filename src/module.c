/* psprecomp -- loading and starting a recompiled module. See module.h. */

#include "psprecomp/module.h"
#include "psprecomp/mem.h"
#include "psprecomp/cpu.h"
#include "psprecomp/ctors.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int psp_module_load(const char *path, uint32_t span, psp_module *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "psprecomp: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *b = (n > 52) ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(b);
        fprintf(stderr, "psprecomp: cannot read %s\n", path);
        return -1;
    }
    fclose(f);

    /* A decrypted PRX is an ELF with one PT_LOAD. Its p_paddr is not a
     * physical address: it holds the file offset of the module info. */
    const uint32_t phoff = rd32(b + 28);
    if (memcmp(b, "\x7F" "ELF", 4) != 0 || (size_t)phoff + 32 > (size_t)n) {
        free(b);
        fprintf(stderr, "psprecomp: %s is not a decrypted ELF module\n", path);
        return -1;
    }
    const uint8_t *ph = b + phoff;
    const uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), paddr = rd32(ph + 12);
    const uint32_t filesz = rd32(ph + 16);
    if ((size_t)off + filesz > (size_t)n || filesz > span ||
        psp_mem_map_module(vaddr, span) != 0 ||
        psp_mem_write_block(vaddr, b + off, filesz) != 0) {
        free(b);
        fprintf(stderr, "psprecomp: cannot place %s at 0x%08X\n", path, vaddr);
        return -1;
    }

    /* Some modules (DJMAX Portable 3, for one) split code and data into two
     * PT_LOADs: .text/.rodata in the first, .data/.bss in a second one linked
     * above it. Place every further PT_LOAD inside the same span; the map is
     * calloc'd, so .bss (memsz past filesz) is already zero. */
    const uint16_t phentsize = (uint16_t)(b[42] | (b[43] << 8));
    const uint16_t phnum     = (uint16_t)(b[44] | (b[45] << 8));
    uint32_t mem_end = vaddr + rd32(ph + 20);
    for (uint16_t i = 1; phentsize >= 32 && i < phnum; i++) {
        const uint8_t *q = b + phoff + (size_t)i * phentsize;
        if ((size_t)(q - b) + 32 > (size_t)n) break;
        if (rd32(q) != 1) continue;                       /* PT_LOAD only */
        const uint32_t qoff = rd32(q + 4), qaddr = rd32(q + 8);
        const uint32_t qfile = rd32(q + 16), qmem = rd32(q + 20);
        if (qaddr < vaddr || qaddr + qmem > vaddr + span ||
            (size_t)qoff + qfile > (size_t)n ||
            psp_mem_write_block(qaddr, b + qoff, qfile) != 0) {
            free(b);
            fprintf(stderr, "psprecomp: cannot place segment %u of %s at 0x%08X\n",
                    (unsigned)i, path, qaddr);
            return -1;
        }
        if (qaddr + qmem > mem_end) mem_end = qaddr + qmem;
    }
    m->mem_end = mem_end;

    m->base = vaddr;
    m->image_end = vaddr + filesz;
    m->entry = rd32(b + 24);
    /* SceModuleInfo: u16 attr, u16 version, char name[28], then gp. */
    const uint32_t info = (paddr & 0x7FFFFFFFu) - off + vaddr;
    m->gp = psp_read32(info + 32);
    free(b);

    /* The constructor table and the constructors sit together at the end of
     * the image; a vtable elsewhere has the same shape, so only that region
     * is searched, and the entries must point into it too.
     * ponytail: right for WTF's modules (8 constructors just below .bss);
     * widen the window if another title keeps them elsewhere. */
    const uint32_t lo = m->image_end > vaddr + 0x10000u ? m->image_end - 0x10000u : vaddr;
    uint32_t t;
    int count;
    if (psp_ctors_find(lo, m->image_end, lo, m->image_end, &t, &count) == 0) {
        m->ctors = t;
        m->nctors = count;
    }
    return 0;
}

/* The loader's leftovers. $k0 is the thread pointer: libc reaches its
 * per-thread reent, and through it the heap, at *(k0 + 4). The stack is only
 * module_start's, which returns once it has started the game's threads.
 * ponytail: fixed addresses, proven on WTF; take them from sysmem if a title
 * minds. */
#define START_STACK 0x08F00000u
#define TCB_ADDR    0x08800000u
#define REENT_ADDR  0x08800100u

uint32_t psp_module_start(const psp_module *m) {
    psp_cpu.r[PSP_REG_GP] = m->gp;
    psp_cpu.r[PSP_REG_SP] = START_STACK;
    psp_cpu.r[PSP_REG_A0] = 0;
    psp_cpu.r[PSP_REG_A1] = 0;
    psp_cpu.r[26] = TCB_ADDR;                   /* $k0 */
    psp_write32(TCB_ADDR + 4, REENT_ADDR);
    /* The constructor search is a heuristic over the end of the image. A
     * plain-C module (DJMAX Portable 3: .ctors is just {-1, 0}) has no table,
     * and the search then finds a run of pointers into .rodata instead. Only
     * run a table whose every entry is recompiled code. */
    int ctors_ok = m->ctors != 0;
    for (int i = 0; ctors_ok && i < m->nctors; i++)
        if (!psp_lookup(psp_read32(m->ctors + (uint32_t)i * 4))) ctors_ok = 0;
    if (m->ctors && !ctors_ok)
        fprintf(stderr, "psprecomp: ignoring constructor table candidate at 0x%08X "
                        "(entries are not recompiled code)\n", m->ctors);
    if (ctors_ok) psp_ctors_run(m->ctors);
    psp_call_from_host(m->entry);
    return psp_cpu.r[PSP_REG_V0];
}
