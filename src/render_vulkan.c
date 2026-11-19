/* psprecomp — Vulkan render backend.
 *
 * Implements the `psp_render_backend` interface using Vulkan 1.0+.
 *
 * All display-list interpretation, vertex format decoding, matrix transforms,
 * and per-vertex lighting stay in `src/hle/ge.c`. This backend receives
 * screen-space primitives (`psp_vertex`) and `psp_draw_state`, batches them
 * across each display list, caches decoded textures/CLUTs in `VkImage`s, and
 * executes the full GE pixel pipeline (texture combine, alpha test, depth
 * test, scissor, clear mode, and blending) on the GPU.
 */

#include "psprecomp/render.h"
#include "psprecomp/log.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"

#ifdef PSPRECOMP_VULKAN

#include "vk_shaders.h"
#include "vk_mini.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#define FB_W 480
#define FB_H 272

/* Internal resolution: the target is FB_W*N x FB_H*N and every vertex and
 * scissor is multiplied by N on the way in. Geometry is re-rasterized at that
 * size, which is the whole point; a 1x image blown up would not be. */
static int g_scale = 1;
#define VW (FB_W * g_scale)
#define VH (FB_H * g_scale)

#define MAX_VBO_VERTS     262144
#define MAX_DRAW_CMDS     16384
#define MAX_TEX_CACHE     512
#define MAX_TEX_UPLOADS   256
#define MAX_PIPELINES     128
#define TEX_STAGING_SIZE  (32u * 1024u * 1024u)

typedef struct {
    float    x, y, z;
    uint32_t rgba;
    float    u, v;
} vk_vertex;

typedef struct {
    int32_t  tex_w;
    int32_t  tex_h;
    uint32_t flags;
    uint32_t tex_env;
} vk_push_constants;

typedef struct {
    int               used;
    uint32_t          tex_addr;
    int               tex_bw, tex_w, tex_h, tex_fmt, tex_swizzle;
    uint32_t          clut_meta;
    uint64_t          clut_hash;
    uint64_t          data_hash;
    uint64_t          last_used;
    uint64_t          batch_used;

    VkImage           image;
    VkDeviceMemory    memory;
    VkImageView       view;
    VkDescriptorSet   desc_set;
    VkDescriptorSet   desc_set_linear;
    int               layout_ready;
    int               uv_scale;      /* 1, or the pack's integer multiple */
    int               img_w, img_h;  /* size of the image actually allocated */
} vk_tex_entry;

typedef struct {
    int               tex_slot;
    uint32_t          staging_offset;
    int               w, h;
} vk_tex_upload;

typedef struct {
    uint64_t          key;
    VkPipeline        pipeline;
} vk_pipe_entry;

typedef struct {
    uint32_t          first_vert;
    uint32_t          vert_count;
    uint64_t          pipe_key;
    int               tex_slot;
    int               tex_filter;
    int               sc_x, sc_y, sc_w, sc_h;
    float             blend_const[4];
    vk_push_constants pc;
    int               count_pixels;
} vk_draw_cmd;

/* ---- Vulkan state -------------------------------------------------------- */

static int                     g_vk_inited = 0;
static int                     g_vk_failed = 0;

static VkInstance              g_inst;
static VkPhysicalDevice        g_phys;
static VkDevice                g_dev;
static uint32_t                g_qfam;
static VkQueue                 g_queue;
static VkCommandPool           g_cmd_pool;
static VkCommandBuffer         g_cmd;
static VkFence                 g_fence;
static VkQueryPool             g_query_pool;
static int                     g_precise_query;

static VkPhysicalDeviceMemoryProperties g_mem_props;

/* Render target attachments (480x272 RGBA8 color + D16_UNORM depth) */
static VkImage                 g_color_img;
static VkDeviceMemory          g_color_mem;
static VkImageView             g_color_view;

static VkImage                 g_depth_img;
static VkDeviceMemory          g_depth_mem;
static VkImageView             g_depth_view;

static VkRenderPass            g_render_pass;
static VkFramebuffer           g_framebuffer;

/* Host-visible buffers */
static VkBuffer                g_fb_staging_buf;
static VkDeviceMemory          g_fb_staging_mem;
static uint32_t               *g_fb_staging_ptr;

static VkBuffer                g_zb_staging_buf;
static VkDeviceMemory          g_zb_staging_mem;
static uint16_t               *g_zb_staging_ptr;

static VkBuffer                g_vbo_buf;
static VkDeviceMemory          g_vbo_mem;
static vk_vertex              *g_vbo_ptr;
static uint32_t                g_vbo_count;

static VkBuffer                g_tex_staging_buf;
static VkDeviceMemory          g_tex_staging_mem;
static uint8_t                *g_tex_staging_ptr;
static uint32_t                g_tex_staging_off;

/* Descriptors, shaders, and pipelines */
static VkSampler               g_sampler;
static VkSampler               g_sampler_linear;
static VkDescriptorSetLayout   g_desc_layout;
static VkDescriptorPool        g_desc_pool;
static VkPipelineLayout        g_pipe_layout;
static VkShaderModule          g_vert_mod;
static VkShaderModule          g_frag_mod;

static vk_tex_entry            g_tex_cache[MAX_TEX_CACHE];
static int                     g_dummy_white_slot = -1;
static int                     g_dummy_magenta_slot = -1;

static vk_tex_upload           g_tex_uploads[MAX_TEX_UPLOADS];
static int                     g_num_tex_uploads;

static vk_pipe_entry           g_pipelines[MAX_PIPELINES];
static int                     g_num_pipelines;

static vk_draw_cmd             g_cmds[MAX_DRAW_CMDS];
static int                     g_num_cmds;

static uint64_t                g_use_tick = 1;
static uint64_t                g_batch_id = 1;

/* Active target state */
static uint32_t                g_fb_addr;
static uint32_t                g_fb_stride;
static int                     g_fb_fmt = 3;

static int                     g_batch_has_z;
static uint32_t                g_batch_zaddr;
static int                     g_batch_zbw;

/* Region touched by the current batch. A flush only uploads and writes back
 * this much, so drawing into a target smaller than the screen cannot stomp
 * the VRAM around it. */
static int                     g_dirty_x0 = FB_W, g_dirty_y0 = FB_H;
static int                     g_dirty_x1, g_dirty_y1;

static void vk_flush(void);

/* ---- Memory & Buffer helpers --------------------------------------------- */

static int find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < g_mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (g_mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return (int)i;
        }
    }
    return -1;
}

static int create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props, VkBuffer *out_buf,
                         VkDeviceMemory *out_mem, void **out_ptr) {
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    if (vkCreateBuffer(g_dev, &bci, NULL, out_buf) != VK_SUCCESS) return -1;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, *out_buf, &req);
    int mt = find_mem_type(req.memoryTypeBits, props | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt < 0) mt = find_mem_type(req.memoryTypeBits, props);
    if (mt < 0) return -1;

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)mt
    };
    if (vkAllocateMemory(g_dev, &mai, NULL, out_mem) != VK_SUCCESS) return -1;
    if (vkBindBufferMemory(g_dev, *out_buf, *out_mem, 0) != VK_SUCCESS) return -1;
    if (out_ptr) {
        if (vkMapMemory(g_dev, *out_mem, 0, size, 0, out_ptr) != VK_SUCCESS) return -1;
    }
    return 0;
}

static int create_image(uint32_t w, uint32_t h, VkFormat fmt,
                        VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage *out_img, VkDeviceMemory *out_mem,
                        VkImageView *out_view) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (vkCreateImage(g_dev, &ici, NULL, out_img) != VK_SUCCESS) return -1;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, *out_img, &req);
    int mt = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_mem_type(req.memoryTypeBits, 0);
    if (mt < 0) return -1;

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)mt
    };
    if (vkAllocateMemory(g_dev, &mai, NULL, out_mem) != VK_SUCCESS) return -1;
    if (vkBindImageMemory(g_dev, *out_img, *out_mem, 0) != VK_SUCCESS) return -1;

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *out_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    if (vkCreateImageView(g_dev, &ivci, NULL, out_view) != VK_SUCCESS) return -1;
    return 0;
}

/* ---- Colour pack/unpack for 16-bit framebuffers -------------------------- */

#define R(c) ((c) & 0xFFu)
#define G(c) (((c) >> 8) & 0xFFu)
#define B(c) (((c) >> 16) & 0xFFu)
#define A(c) ((c) >> 24)

static uint32_t color_pack(uint32_t c, int fmt) {
    switch (fmt) {
    case 0:  return (R(c) >> 3) | ((G(c) >> 2) << 5) | ((B(c) >> 3) << 11);
    case 1:  return (R(c) >> 3) | ((G(c) >> 3) << 5) | ((B(c) >> 3) << 10) | ((A(c) >> 7) << 15);
    case 2:  return (R(c) >> 4) | ((G(c) >> 4) << 4) | ((B(c) >> 4) << 8) | ((A(c) >> 4) << 12);
    default: return c;
    }
}

/* ---- Texture hashing & caching ------------------------------------------- */

static const int TEX_BPP[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };

static uint64_t fnv1a_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Hash of every byte. A sampled hash misses a partial update of a large
 * texture (a game that rewrites a few tiles of a 512x512 surface), which
 * leaves stale blocks on screen. Four independent lanes keep it fast. */
static uint64_t hash_full(const uint8_t *p, size_t n) {
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t h0 = 0x243F6A8885A308D3ull, h1 = 0x13198A2E03707344ull,
             h2 = 0xA4093822299F31D0ull, h3 = 0x082EFA98EC4E6C89ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        h0 = (h0 ^ w[0]) * K; h0 ^= h0 >> 29;
        h1 = (h1 ^ w[1]) * K; h1 ^= h1 >> 29;
        h2 = (h2 ^ w[2]) * K; h2 ^= h2 >> 29;
        h3 = (h3 ^ w[3]) * K; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7);
    for (; i < n; i++) { h = (h ^ p[i]) * K; h ^= h >> 31; }
    return h ^ (uint64_t)n;
}

static uint64_t hash_texture_data(const psp_draw_state *st) {
    uint32_t total_bytes = 0;
    if (st->tex_fmt == PSP_TPSM_DXT1) {
        uint32_t bw_blk = st->tex_bw >= 4 ? (uint32_t)st->tex_bw / 4u : 1u;
        uint32_t h_blk  = st->tex_h  >= 4 ? (uint32_t)st->tex_h  / 4u : 1u;
        total_bytes = bw_blk * h_blk * 8u;
    } else if (st->tex_fmt == PSP_TPSM_DXT3 || st->tex_fmt == PSP_TPSM_DXT5) {
        uint32_t bw_blk = st->tex_bw >= 4 ? (uint32_t)st->tex_bw / 4u : 1u;
        uint32_t h_blk  = st->tex_h  >= 4 ? (uint32_t)st->tex_h  / 4u : 1u;
        total_bytes = bw_blk * h_blk * 16u;
    } else if (st->tex_fmt >= 0 && st->tex_fmt <= PSP_TPSM_T32) {
        uint32_t rowb = (uint32_t)st->tex_bw * (uint32_t)TEX_BPP[st->tex_fmt] / 8u;
        if (st->tex_swizzle && rowb < 16) rowb = 16;
        uint32_t rows = (uint32_t)st->tex_h;
        if (st->tex_swizzle && (rows & 7u)) rows = (rows + 7u) & ~7u;
        total_bytes = rowb * rows;
    }
    if (total_bytes == 0) return 0;

    const uint8_t *ptr = (const uint8_t *)psp_mem_ptr(st->tex_addr, total_bytes);
    if (!ptr) {
        /* Fallback if texture spans boundary or row wrap: hash row by row with 100% byte coverage */
        uint64_t h = 14695981039346656037ull;
        uint32_t rowb = (st->tex_fmt >= 0 && st->tex_fmt <= PSP_TPSM_T32)
                      ? (uint32_t)st->tex_bw * (uint32_t)TEX_BPP[st->tex_fmt] / 8u
                      : 0;
        if (rowb > 0) {
            for (int y = 0; y < st->tex_h; y++) {
                uint32_t raddr = st->tex_addr + (uint32_t)y * rowb;
                const uint8_t *rp = (const uint8_t *)psp_mem_ptr(raddr, rowb);
                if (rp) {
                    h ^= hash_full(rp, rowb);
                    h *= 1099511628211ull;
                } else {
                    for (uint32_t x = 0; x < rowb; x += 4) {
                        uint32_t w = psp_read32(raddr + x);
                        h ^= (uint64_t)w;
                        h *= 1099511628211ull;
                    }
                }
            }
        } else {
            for (uint32_t off = 0; off + 4 <= total_bytes; off += 4) {
                uint32_t w = psp_read32(st->tex_addr + off);
                h ^= (uint64_t)w;
                h *= 1099511628211ull;
            }
        }
        return h;
    }

    return hash_full(ptr, total_bytes);
}


/* ---- Texture dump -------------------------------------------------------
 *
 * EMOTIONALSENSE_TEX_DUMP=dir writes every texture that repeats, once, as
 * <key>.png. The key is content + CLUT + format + size and never the address:
 * the same art lands at a different address on another screen or run.
 *
 * "Repeats" is the filter that removes what is generated per frame -- text,
 * counters -- because those never come back with the same key at the same
 * address. A texture is written on its third frame, one sighting per batch.
 * Video is converted into VRAM every frame and shown for two or three
 * vblanks, so it would pass that test; addresses in VRAM are skipped.
 */

#define DUMP_SLOTS 8192
#define DUMP_ADDRS 4096

static const char *g_dump_dir;
static int         g_dump_on = -1;          /* -1 unread, 0 off, 1 on */
static FILE       *g_dump_index;
static struct { uint32_t addr; uint64_t key; uint32_t batch; uint8_t seen; }
                   g_dump_seen[DUMP_SLOTS];
static struct { uint32_t addr; uint16_t keys; } g_dump_addr[DUMP_ADDRS];
static int         g_dump_addr_n;
static int         g_dump_files;
static uint64_t    g_dump_bytes;
#define DUMP_WRITTEN 2048
static uint64_t    g_dump_written[DUMP_WRITTEN];   /* 0 = empty slot */

static int dump_enabled(void) {
    if (g_dump_on < 0) {
        const char *d = getenv("EMOTIONALSENSE_TEX_DUMP");
        g_dump_on = (d && *d) ? 1 : 0;
        if (g_dump_on) {
            char path[512];
            g_dump_dir = d;
            snprintf(path, sizeof path, "%s/index.txt", d);
            g_dump_index = fopen(path, "w");
            if (!g_dump_index) {
                psp_log("tex dump: cannot write to %s\n", d);
                g_dump_on = 0;
            } else {
                fprintf(g_dump_index, "# key addr fmt w h scene\n");
            }
        }
    }
    return g_dump_on;
}

static uint64_t dump_key(uint64_t data, uint64_t clut, int fmt, int w, int h) {
    uint64_t k = data;
    k ^= clut + 0x9E3779B97F4A7C15ull + (k << 6) + (k >> 2);
    k ^= (uint64_t)(uint32_t)fmt * 0x100000001B3ull;
    k ^= (((uint64_t)(uint32_t)w << 32) | (uint32_t)h) * 0xC2B2AE3D27D4EB4Full;
    return k;
}

/* Counts distinct keys per address: a texture whose palette animates shows up
 * as dozens of keys on one address and must stay out of the pack. */
static void dump_addr_note(uint32_t addr) {
    for (int i = 0; i < g_dump_addr_n; i++) {
        if (g_dump_addr[i].addr == addr) {
            if (g_dump_addr[i].keys < 0xFFFFu) g_dump_addr[i].keys++;
            return;
        }
    }
    if (g_dump_addr_n < DUMP_ADDRS) {
        g_dump_addr[g_dump_addr_n].addr = addr;
        g_dump_addr[g_dump_addr_n].keys = 1;
        g_dump_addr_n++;
    }
}

static void dump_write(uint64_t key, uint32_t addr, const psp_draw_state *st,
                       int w, int h) {
    /* The same art turns up at several addresses (another screen, another
     * run); the key is what names the file, so write it once. */
    uint32_t wi = (uint32_t)(key >> 40) & (DUMP_WRITTEN - 1u);
    for (int probe = 0; probe < 32; probe++, wi = (wi + 1u) & (DUMP_WRITTEN - 1u)) {
        if (g_dump_written[wi] == key) return;
        if (g_dump_written[wi] == 0) { g_dump_written[wi] = key; break; }
    }

    uint32_t *px = malloc((size_t)w * (size_t)h * 4u);
    if (!px) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) px[(size_t)y * (size_t)w + (size_t)x] = psp_tex_fetch(st, x, y);

    char path[512];
    snprintf(path, sizeof path, "%s/%016llx.png", g_dump_dir,
             (unsigned long long)key);
    if (psp_tex_png_write(path, px, w, h) == 0) {
        g_dump_files++;
        g_dump_bytes += (uint64_t)w * (uint64_t)h * 4u;
        dump_addr_note(addr);
        if (g_dump_index)
            fprintf(g_dump_index, "%016llx %08x %d %d %d %u\n",
                    (unsigned long long)key, addr, st->tex_fmt, w, h, psp_scene_now());
    }
    free(px);
}

static void tex_dump_note(const psp_draw_state *st, uint64_t data_hash,
                          uint64_t clut_hash) {
    if (!dump_enabled()) return;
    int w = st->tex_w, h = st->tex_h;
    if (w <= 0 || h <= 0) return;
    uint64_t key = dump_key(data_hash, clut_hash, st->tex_fmt, w, h);
    uint32_t addr = st->tex_addr & PSP_ADDR_MASK;
    if (addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + PSP_VRAM_SIZE) {
        /* Video goes through VRAM; EMOTIONALSENSE_TEX_DUMP_VRAM=1 keeps it in
         * the dump anyway from scene 2 on, for UI art the game stages there. */
        static int vram_ok = -1;
        if (vram_ok < 0) vram_ok = getenv("EMOTIONALSENSE_TEX_DUMP_VRAM") != NULL;
        /* Scenes 0 and 1 are the opening video and the title. */
        if (!vram_ok || psp_scene_now() < 2) return;
    }

    uint32_t i = (uint32_t)((key ^ ((uint64_t)addr * 0x9E3779B97F4A7C15ull)) >> 40)
               & (DUMP_SLOTS - 1u);
    for (int probe = 0; probe < 64; probe++, i = (i + 1u) & (DUMP_SLOTS - 1u)) {
        if (g_dump_seen[i].seen == 0) {
            g_dump_seen[i].addr = addr;
            g_dump_seen[i].key = key;
            g_dump_seen[i].batch = g_batch_id;
            g_dump_seen[i].seen = 1;
            return;
        }
        if (g_dump_seen[i].addr == addr && g_dump_seen[i].key == key) {
            if (g_dump_seen[i].batch == g_batch_id) return;   /* same frame */
            g_dump_seen[i].batch = g_batch_id;
            if (g_dump_seen[i].seen < 3) {
                g_dump_seen[i].seen++;
                if (g_dump_seen[i].seen == 3) dump_write(key, addr, st, w, h);
            }
            return;
        }
    }
}

void psp_tex_dump_report(FILE *f) {
    if (!dump_enabled()) return;
    if (g_dump_index) fclose(g_dump_index);
    g_dump_index = NULL;
    int multi = 0, worst = 0;
    uint32_t worst_addr = 0;
    for (int i = 0; i < g_dump_addr_n; i++) {
        if (g_dump_addr[i].keys > 1) multi++;
        if (g_dump_addr[i].keys > worst) {
            worst = g_dump_addr[i].keys;
            worst_addr = g_dump_addr[i].addr;
        }
    }
    fprintf(f, "tex dump: %d files, %.1f MiB, %d of %d addresses carry more than "
               "one key (worst 0x%08X with %d)\n",
            g_dump_files, (double)g_dump_bytes / (1024.0 * 1024.0),
            multi, g_dump_addr_n, worst_addr, worst);
}

/* Point both sampler variants at the entry's current view. Called whenever the
 * image is (re)created, so a recycled slot never keeps a stale view. */
static int tex_desc_write(vk_tex_entry *e) {
    VkDescriptorSet *sets[2] = { &e->desc_set, &e->desc_set_linear };
    VkSampler samplers[2] = { g_sampler, g_sampler_linear };
    for (int i = 0; i < 2; i++) {
        if (!*sets[i]) {
            VkDescriptorSetAllocateInfo dsai = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = g_desc_pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &g_desc_layout
            };
            if (vkAllocateDescriptorSets(g_dev, &dsai, sets[i]) != VK_SUCCESS) return -1;
        }
        VkDescriptorImageInfo dii = {
            .sampler = samplers[i],
            .imageView = e->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        VkWriteDescriptorSet wds = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = *sets[i],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &dii
        };
        vkUpdateDescriptorSets(g_dev, 1, &wds, 0, NULL);
    }
    return 0;
}

static int alloc_tex_slot(int w, int h) {
    int best = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 2; i < MAX_TEX_CACHE; i++) {
        if (!g_tex_cache[i].used) { best = i; break; }
        if (g_tex_cache[i].batch_used != g_batch_id && g_tex_cache[i].last_used < oldest) {
            oldest = g_tex_cache[i].last_used;
            best = i;
        }
    }
    if (best < 0) {
        /* All slots used in this batch: flush and retry */
        vk_flush();
        for (int i = 2; i < MAX_TEX_CACHE; i++) {
            if (g_tex_cache[i].last_used < oldest) {
                oldest = g_tex_cache[i].last_used;
                best = i;
            }
        }
    }
    vk_tex_entry *e = &g_tex_cache[best];
    if (e->used && (e->img_w != w || e->img_h != h)) {
        vkDestroyImageView(g_dev, e->view, NULL);
        vkDestroyImage(g_dev, e->image, NULL);
        vkFreeMemory(g_dev, e->memory, NULL);
        e->image = VK_NULL_HANDLE;
        e->view = VK_NULL_HANDLE;
        e->memory = VK_NULL_HANDLE;
        e->used = 0;
    }
    if (!e->used) {
        if (create_image((uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, &e->image, &e->memory, &e->view) != 0)
            return -1;
        if (tex_desc_write(e) != 0) return -1;
        e->used = 1;
        e->layout_ready = 0;
    }
    e->img_w = w;
    e->img_h = h;
    return best;
}

static int queue_solid_texture(int slot, uint32_t color) {
    vk_tex_entry *e = &g_tex_cache[slot];
    if (!e->used) {
        if (create_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, &e->image, &e->memory, &e->view) != 0)
            return -1;
        if (tex_desc_write(e) != 0) return -1;
        e->used = 1;
        e->tex_w = 1;
        e->tex_h = 1;
        e->img_w = 1;
        e->img_h = 1;
        e->layout_ready = 0;
    }
    uint32_t off = (g_tex_staging_off + 3u) & ~3u;
    *(uint32_t *)(g_tex_staging_ptr + off) = color;
    g_tex_staging_off = off + 4u;
    g_tex_uploads[g_num_tex_uploads++] = (vk_tex_upload){ slot, off, 1, 1 };
    e->batch_used = g_batch_id;
    return slot;
}

/* ---- Texture pack --------------------------------------------------------
 *
 * <data>/textures/<key>.png replaces a texture at upload time. The key is the
 * one the dump writes, so a pack is the dump directory with the files
 * upscaled: no index, no manifest, and a file that is not there simply means
 * "keep the original".
 *
 * The replacement must be an integer multiple of the original, because the
 * UVs stay in original texels. The scale is applied where the vertices are
 * emitted and the shader sees the scaled size, so wrapping and clamping keep
 * working in the scaled space.
 *
 * The whole pack is decoded once, before the first frame. Decoding on first
 * use was the obvious thing and it is wrong: a 2048x2048 decode takes tens of
 * milliseconds, the audio ring holds about forty, and the result was an
 * underrun in the middle of a song (measured at vblank 2067). */

#define PACK_MAX 1024

typedef struct {
    uint64_t  key;
    uint32_t *px;
    int       w, h;
    int       state;        /* 0 empty, 1 loaded */
} vk_pack_entry;

static vk_pack_entry g_pack[PACK_MAX];
static int           g_pack_scanned;
static char          g_pack_dir[512];
static int           g_pack_used;
static int           g_pack_skipped;
static int           g_pack_over;
static uint64_t      g_pack_bytes;
static uint64_t      g_pack_limit;
static float         g_uv_scale = 1.0f;

static uint32_t pack_slot(uint64_t key) {
    return (uint32_t)(key >> 40) & (PACK_MAX - 1u);
}

/* Insert one decoded file. Returns 0 when it fits, -1 when it does not. */
static int pack_insert(uint64_t key, uint32_t *px, int w, int h) {
    uint32_t i = pack_slot(key);
    for (int probe = 0; probe < 64; probe++, i = (i + 1u) & (PACK_MAX - 1u)) {
        vk_pack_entry *e = &g_pack[i];
        if (e->state == 0) {
            e->key = key;
            e->px = px;
            e->w = w;
            e->h = h;
            e->state = 1;
            g_pack_used++;
            g_pack_bytes += (uint64_t)w * (uint64_t)h * 4u;
            return 0;
        }
        if (e->key == key) return 0;    /* same art twice; keep the first */
    }
    return -1;
}

static void pack_load_file(const char *name) {
    /* <16 hex>.png and nothing else. */
    if (strlen(name) != 20 || strcmp(name + 16, ".png") != 0) return;
    uint64_t key = 0;
    for (int i = 0; i < 16; i++) {
        int c = name[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return;
        key = (key << 4) | (uint64_t)v;
    }
    char path[640];
    snprintf(path, sizeof path, "%s/%s", g_pack_dir, name);
    uint32_t *px = NULL;
    int w = 0, h = 0;
    if (psp_tex_png_read(path, &px, &w, &h) != 0) return;
    uint64_t size = (uint64_t)w * (uint64_t)h * 4u;
    if (g_pack_bytes + size > g_pack_limit) {
        free(px);
        if (!g_pack_over) {
            g_pack_over = 1;
            psp_log("tex pack: over the %llu MiB budget, keeping the "
                            "original textures\n",
                    (unsigned long long)(g_pack_limit >> 20));
        }
        return;
    }
    if (pack_insert(key, px, w, h) != 0) free(px);
}

static void pack_scan(void) {
    if (g_pack_scanned) return;
    g_pack_scanned = 1;

    const char *env = getenv("EMOTIONALSENSE_TEX_PACK");
    if (env && *env) {
        snprintf(g_pack_dir, sizeof g_pack_dir, "%s", env);
    } else {
        snprintf(g_pack_dir, sizeof g_pack_dir, "%s/textures", psp_io_root());
    }
    /* A 4x pack of the whole game is a few hundred MiB decoded; the budget is
     * there so a bigger pack degrades to the original art instead of failing
     * an allocation somewhere deep in the driver. */
    const char *max = getenv("EMOTIONALSENSE_TEX_PACK_MAX");
    uint64_t mib = max && *max ? strtoull(max, NULL, 10) : 1024u;
    g_pack_limit = mib * 1024u * 1024u;

#ifdef _WIN32
    char pattern[640];
    snprintf(pattern, sizeof pattern, "%s\\*", g_pack_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) pack_load_file(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(g_pack_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) pack_load_file(de->d_name);
    closedir(d);
#endif
}

static vk_pack_entry *pack_lookup(uint64_t key, int w, int h) {
    uint32_t i = pack_slot(key);
    for (int probe = 0; probe < 64; probe++, i = (i + 1u) & (PACK_MAX - 1u)) {
        vk_pack_entry *e = &g_pack[i];
        if (e->state == 0) return NULL;
        if (e->key != key) continue;
        if (w <= 0 || h <= 0 || e->w < w || e->h < h ||
            e->w % w || e->h % h || e->w / w != e->h / h) {
            g_pack_skipped++;
            return NULL;
        }
        return e;
    }
    return NULL;
}

void psp_tex_pack_report(FILE *f) {
    if (!f || (g_pack_used == 0 && g_pack_skipped == 0)) return;
    fprintf(f, "tex pack: %d textures, %.1f MiB decoded, %d skipped\n",
            g_pack_used, (double)g_pack_bytes / (1024.0 * 1024.0), g_pack_skipped);
}

/* ---- Button glyphs -------------------------------------------------------
 *
 * See psp_glyph_rule. The rewrite happens on the uploaded copy, at whatever
 * integer scale a pack gave the texture, and the cache entry carries the swap
 * state in its hash so the original and the rewritten texture never collide. */

#define GLYPH_SALT 0x5A17C0DE5A17C0DEull
static const psp_glyph_rule *g_glyph_rules;
static int g_glyph_n;

void psp_render_set_glyph_rules(const psp_glyph_rule *rules, int n) {
    g_glyph_rules = rules;
    g_glyph_n = n;
}

static int glyph_has_rule(uint64_t key, int w, int h) {
    for (int i = 0; i < g_glyph_n; i++)
        if (g_glyph_rules[i].key == key && g_glyph_rules[i].w == w && g_glyph_rules[i].h == h)
            return 1;
    return 0;
}

static int px_lum(uint32_t p) {
    return (int)(((p & 0xFF) * 30u + ((p >> 8) & 0xFF) * 59u + ((p >> 16) & 0xFF) * 11u) / 100u);
}

/* Redraw the symbol of one disc glyph (ring or cross) over its dark button. */
static void glyph_paint(uint32_t *px, int stride, int x0, int y0, int gw, int gh, char kind) {
    /* Symbol colour: the mean of the bright opaque texels; button colour: of
     * the dark ones. Both are read from the glyph, so nothing is invented. */
    uint32_t sym = 0, dark = 0;
    long sl = 0, dl = 0, sr = 0, sg = 0, sb = 0, dr = 0, dg = 0, db = 0;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            uint32_t p = px[(size_t)(y0 + y) * (size_t)stride + (size_t)(x0 + x)];
            if ((p >> 24) < 0xE0) continue;
            int l = px_lum(p);
            if (l > 120) { sl++; sr += p & 0xFF; sg += (p >> 8) & 0xFF; sb += (p >> 16) & 0xFF; }
            else if (l < 50) { dl++; dr += p & 0xFF; dg += (p >> 8) & 0xFF; db += (p >> 16) & 0xFF; }
        }
    if (!sl || !dl) return;
    sym = 0xFF000000u | (uint32_t)(sr / sl) | ((uint32_t)(sg / sl) << 8) | ((uint32_t)(sb / sl) << 16);
    dark = 0xFF000000u | (uint32_t)(dr / dl) | ((uint32_t)(dg / dl) << 8) | ((uint32_t)(db / dl) << 16);
    const float cx = (gw - 1) * 0.5f, cy = (gh - 1) * 0.5f;
    const float rin = gw * 0.37f, arm = gw * 0.26f, hw = gw * 0.07f + 0.5f;
    const float rring = gw * 0.27f, thick = gw * 0.085f + 0.4f;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            uint32_t *p = &px[(size_t)(y0 + y) * (size_t)stride + (size_t)(x0 + x)];
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx * dx + dy * dy);
            if ((*p >> 24) < 0xE0 || r > rin) continue;   /* rim and surroundings stay */
            float cov = 0.0f;
            if (kind == 'X') {
                float d1 = fabsf(dx - dy) * 0.70710678f, d2 = fabsf(dx + dy) * 0.70710678f;
                float d = d1 < d2 ? d1 : d2;
                if (fabsf(dx) <= arm + 0.5f && fabsf(dy) <= arm + 0.5f)
                    cov = hw + 0.5f - d;
            } else {
                cov = thick * 0.5f + 0.5f - fabsf(r - rring);
            }
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            uint32_t out = 0xFF000000u;
            for (int c = 0; c < 24; c += 8) {
                float a = (float)((dark >> c) & 0xFF), b = (float)((sym >> c) & 0xFF);
                out |= (uint32_t)(a + (b - a) * cov + 0.5f) << c;
            }
            *p = out;
        }
}

/* Apply every rule of `key` to the uploaded copy, `s` times the original size. */
static void glyph_rewrite(uint32_t *px, int up_w, int s, uint64_t key, int w, int h) {
    for (int i = 0; i < g_glyph_n; i++) {
        const psp_glyph_rule *r = &g_glyph_rules[i];
        if (r->key != key || r->w != w || r->h != h) continue;
        int gw = r->gw * s, gh = r->gh * s;
        if (r->kind == 'S') {
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) {
                    uint32_t *a = &px[(size_t)(r->ay * s + y) * (size_t)up_w + (size_t)(r->ax * s + x)];
                    uint32_t *b = &px[(size_t)(r->by * s + y) * (size_t)up_w + (size_t)(r->bx * s + x)];
                    uint32_t t = *a; *a = *b; *b = t;
                }
        } else if (r->kind == 'X' || r->kind == 'O') {
            glyph_paint(px, up_w, r->ax * s, r->ay * s, gw, gh, r->kind);
        }
    }
}

/* ---- BGA smoothing (psp_render_bga_smooth) ------------------------------- */
/* The background video is a 512x512 8888 VRAM texture rewritten every second
 * vblank. On the vblank a new frame N(k) arrives the screen keeps showing
 * N(k-1); on the next one it shows (N(k-1)+N(k))/2; then N(k) from the next
 * arrival on. The cost is one vblank of delay for the video alone. */
static struct {
    uint32_t *prev, *cur;
    int       valid, slot, phase;
    uint64_t  hash, vb;
    uint64_t  n_arrive, n_blend;
    double    ms;
} g_bga;

static double bga_now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static int bga_candidate(const psp_draw_state *st) {
    return psp_render_bga_smooth && st->tex_enable && st->tex_fmt == PSP_TPSM_8888 &&
           !st->tex_swizzle && st->tex_w == 512 && st->tex_h == 512 &&
           (st->tex_addr & PSP_ADDR_MASK) >= PSP_VRAM_BASE && psp_scene_now() == 3;
}

static void bga_avg(uint32_t *d, const uint32_t *a, const uint32_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        d[i] = ((a[i] >> 1) & 0x7F7F7F7Fu) + ((b[i] >> 1) & 0x7F7F7F7Fu) + (a[i] & b[i] & 0x01010101u);
}

/* Unchanged video content on a later vblank: refresh the cache entry with the
 * blend (first time) or the new frame. Returns the slot, or -1 if this is not
 * that case and the normal path must run. */
static int bga_hold(const psp_draw_state *st, uint64_t data_hash) {
    if (!g_bga.valid || data_hash != g_bga.hash) return -1;
    int slot = g_bga.slot;
    vk_tex_entry *e = &g_tex_cache[slot];
    if (!e->used || e->tex_addr != (st->tex_addr & PSP_ADDR_MASK) || e->data_hash != data_hash ||
        e->img_w != 512 || e->img_h != 512) { g_bga.valid = 0; return -1; }
    uint64_t vb = psp_display_vblanks();
    if (vb != g_bga.vb && g_bga.phase < 2) {
        double t0 = bga_now_ms();
        if (e->batch_used == g_batch_id) vk_flush();
        uint32_t bytes = 512u * 512u * 4u;
        uint32_t off = (g_tex_staging_off + 3u) & ~3u;
        if (off + bytes > TEX_STAGING_SIZE || g_num_tex_uploads >= MAX_TEX_UPLOADS) {
            vk_flush();
            off = (g_tex_staging_off + 3u) & ~3u;
        }
        uint32_t *dst = (uint32_t *)(g_tex_staging_ptr + off);
        if (g_bga.phase == 0) { bga_avg(dst, g_bga.prev, g_bga.cur, 512u * 512u); g_bga.n_blend++; }
        else memcpy(dst, g_bga.cur, bytes);
        g_bga.phase++;
        g_tex_staging_off = off + bytes;
        g_tex_uploads[g_num_tex_uploads++] = (vk_tex_upload){ slot, off, 512, 512 };
        g_bga.ms += bga_now_ms() - t0;
    }
    g_bga.vb = vb > g_bga.vb ? vb : g_bga.vb;
    e->last_used = ++g_use_tick;
    e->batch_used = g_batch_id;
    return slot;
}

/* A new video frame was decoded into `dst` (cache miss): keep it as the
 * newest, and upload the previous one instead. */
static void bga_arrive(uint32_t *dst, int slot, uint64_t data_hash) {
    double t0 = bga_now_ms();
    uint64_t vb = psp_display_vblanks();
    size_t n = 512u * 512u;
    if (!g_bga.cur) { g_bga.cur = malloc(n * 4); g_bga.prev = malloc(n * 4); }
    if (!g_bga.cur || !g_bga.prev) return;
    if (g_bga.valid && vb - g_bga.vb > 8) g_bga.valid = 0;     /* new song, stale frames */
    if (g_bga.valid) memcpy(g_bga.prev, g_bga.cur, n * 4);
    else memcpy(g_bga.prev, dst, n * 4);
    memcpy(g_bga.cur, dst, n * 4);
    memcpy(dst, g_bga.prev, n * 4);
    g_bga.valid = 1; g_bga.slot = slot; g_bga.hash = data_hash; g_bga.vb = vb; g_bga.phase = 0;
    g_bga.n_arrive++;
    g_bga.ms += bga_now_ms() - t0;
    static int stats = -1;
    if (stats < 0) stats = getenv("EMOTIONALSENSE_BGA_STATS") != NULL;
    if (stats && g_bga.n_arrive % 120 == 0)
        psp_log("bga: arrivals=%llu blends=%llu cost=%.3f ms per arrival (%.3f ms/s at 30 arrivals/s)\n",
                (unsigned long long)g_bga.n_arrive, (unsigned long long)g_bga.n_blend,
                g_bga.ms / (double)g_bga.n_arrive, 30.0 * g_bga.ms / (double)g_bga.n_arrive);
}

static int resolve_texture(const psp_draw_state *st) {
    if (st->clear || !st->tex_enable) {
        g_tex_cache[g_dummy_white_slot].batch_used = g_batch_id;
        return g_dummy_white_slot;
    }
    if (st->tex_fmt > PSP_TPSM_DXT5 || st->tex_w <= 0 || st->tex_h <= 0 || st->tex_bw <= 0) {
        (void)psp_tex_fetch(st, 0, 0); /* increments unsupported texel counter */
        g_tex_cache[g_dummy_magenta_slot].batch_used = g_batch_id;
        return g_dummy_magenta_slot;
    }

    uint32_t clut_meta = 0;
    int is_indexed = (st->tex_fmt >= PSP_TPSM_T4 && st->tex_fmt <= PSP_TPSM_T32);
    if (is_indexed) {
        clut_meta = (uint32_t)(st->clut_fmt & 3) |
                    ((uint32_t)(st->clut_shift & 0x1F) << 2) |
                    ((uint32_t)(st->clut_mask & 0xFF) << 8) |
                    ((uint32_t)(st->clut_start & 0x1F) << 16);
    }

    /* Fast path: if an entry with identical geometry/metadata was already
     * validated or uploaded in this batch, check data_hash to ensure dynamic
     * textures (canvas/animations) haven't changed in the same batch. */
    uint64_t clut_hash = 0;
    int clut_hashed = 0;
    uint64_t data_hash = hash_texture_data(st);
    if (is_indexed && st->clut) {
        clut_hash = fnv1a_bytes(st->clut, 1024);
        clut_hashed = 1;
    }

    /* A circle/cross swap changes the pixels, so the swapped copy is a
     * different cache entry from the original. The dump key and the pack
     * lookup below keep using the original hash. */
    const uint64_t data_hash_orig = data_hash;
    int glyph_swap = 0;
    if (g_glyph_n && psp_render_face_swap &&
        glyph_has_rule(dump_key(data_hash, clut_hash, st->tex_fmt, st->tex_w, st->tex_h),
                       st->tex_w, st->tex_h)) {
        glyph_swap = 1;
        data_hash ^= GLYPH_SALT;
    }

    const int bga = bga_candidate(st);
    if (bga && !glyph_swap) {
        int bs = bga_hold(st, data_hash);
        if (bs >= 0) return bs;
    }

    for (int i = 2; i < MAX_TEX_CACHE; i++) {
        vk_tex_entry *e = &g_tex_cache[i];
        if (!e->used || e->batch_used != g_batch_id) continue;
        if (e->tex_addr == (st->tex_addr & PSP_ADDR_MASK) &&
            e->tex_bw == st->tex_bw &&
            e->tex_w == st->tex_w &&
            e->tex_h == st->tex_h &&
            e->tex_fmt == st->tex_fmt &&
            e->tex_swizzle == st->tex_swizzle &&
            e->clut_meta == clut_meta &&
            e->clut_hash == clut_hash &&
            e->data_hash == data_hash) {
            e->last_used = ++g_use_tick;
            return i;
        }
    }

    if (is_indexed && st->clut && !clut_hashed) {
        clut_hash = fnv1a_bytes(st->clut, 1024);
    }

    tex_dump_note(st, data_hash_orig, clut_hash);
    {   /* EMOTIONALSENSE_TEX_TRACE=1: one line per texture resolved. */
        static int tr = -1;
        if (tr < 0) tr = getenv("EMOTIONALSENSE_TEX_TRACE") != NULL;
        if (tr)
            psp_log("tex: scene=%u addr=%08x fmt=%d %dx%d key=%016llx\n",
                    psp_scene_now(), st->tex_addr & PSP_ADDR_MASK, st->tex_fmt,
                    st->tex_w, st->tex_h,
                    (unsigned long long)dump_key(data_hash_orig, clut_hash, st->tex_fmt,
                                                 st->tex_w, st->tex_h));
    }

    vk_pack_entry *pack = pack_lookup(dump_key(data_hash_orig, clut_hash, st->tex_fmt,
                                               st->tex_w, st->tex_h),
                                      st->tex_w, st->tex_h);
    int up_w = pack ? pack->w : st->tex_w;
    int up_h = pack ? pack->h : st->tex_h;
    int uv_scale = pack ? up_w / st->tex_w : 1;

    int match_same_geom = -1;
    for (int i = 2; i < MAX_TEX_CACHE; i++) {
        vk_tex_entry *e = &g_tex_cache[i];
        if (!e->used) continue;
        if (e->tex_addr == (st->tex_addr & PSP_ADDR_MASK) &&
            e->tex_bw == st->tex_bw &&
            e->tex_w == st->tex_w &&
            e->tex_h == st->tex_h &&
            e->tex_fmt == st->tex_fmt &&
            e->tex_swizzle == st->tex_swizzle) {
            if (e->clut_meta == clut_meta &&
                e->clut_hash == clut_hash &&
                e->data_hash == data_hash) {
                e->last_used = ++g_use_tick;
                e->batch_used = g_batch_id;
                return i;
            }
            if (e->batch_used != g_batch_id && e->img_w == up_w && e->img_h == up_h)
                match_same_geom = i;
        }
    }

    uint32_t bytes_needed = (uint32_t)up_w * (uint32_t)up_h * 4u;
    uint32_t aligned_off = (g_tex_staging_off + 3u) & ~3u;
    if (aligned_off + bytes_needed > TEX_STAGING_SIZE || g_num_tex_uploads >= MAX_TEX_UPLOADS) {
        vk_flush();
        aligned_off = (g_tex_staging_off + 3u) & ~3u;
    }

    int slot = match_same_geom >= 0 ? match_same_geom : alloc_tex_slot(up_w, up_h);
    if (slot < 0) return g_dummy_white_slot;

    /* Decode texture into staging buffer */
    uint32_t *dst = (uint32_t *)(g_tex_staging_ptr + aligned_off);
    int w = st->tex_w, h = st->tex_h;
    if (pack) {
        memcpy(dst, pack->px, (size_t)up_w * (size_t)up_h * 4u);
    } else if (!st->tex_swizzle && st->tex_fmt == PSP_TPSM_8888) {
        for (int y = 0; y < h; y++) {
            uint32_t row_addr = st->tex_addr + (uint32_t)(y * st->tex_bw) * 4u;
            const void *src = psp_mem_ptr(row_addr, (uint32_t)w * 4u);
            if (src) {
                memcpy(dst + y * w, src, (size_t)w * 4u);
            } else {
                for (int x = 0; x < w; x++) dst[y * w + x] = psp_tex_fetch(st, x, y);
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                dst[y * w + x] = psp_tex_fetch(st, x, y);
            }
        }
    }

    if (bga && !pack && !glyph_swap) bga_arrive(dst, slot, data_hash);

    if (glyph_swap)
        glyph_rewrite(dst, up_w, uv_scale, dump_key(data_hash_orig, clut_hash, st->tex_fmt,
                                                    st->tex_w, st->tex_h), st->tex_w, st->tex_h);

    g_tex_staging_off = aligned_off + bytes_needed;
    g_tex_uploads[g_num_tex_uploads++] = (vk_tex_upload){ slot, aligned_off, up_w, up_h };

    vk_tex_entry *e = &g_tex_cache[slot];
    e->tex_addr = st->tex_addr & PSP_ADDR_MASK;
    e->tex_bw = st->tex_bw;
    e->tex_w = st->tex_w;
    e->tex_h = st->tex_h;
    e->tex_fmt = st->tex_fmt;
    e->tex_swizzle = st->tex_swizzle;
    e->clut_meta = clut_meta;
    e->clut_hash = clut_hash;
    e->data_hash = data_hash;
    e->last_used = ++g_use_tick;
    e->batch_used = g_batch_id;
    e->uv_scale = uv_scale;
    return slot;
}

/* ---- Pipeline cache ------------------------------------------------------ */

static const VkCompareOp ZFUNC_MAP[8] = {
    VK_COMPARE_OP_NEVER,            /* 0 */
    VK_COMPARE_OP_ALWAYS,           /* 1 */
    VK_COMPARE_OP_EQUAL,            /* 2 */
    VK_COMPARE_OP_NOT_EQUAL,        /* 3 */
    VK_COMPARE_OP_LESS,             /* 4 */
    VK_COMPARE_OP_LESS_OR_EQUAL,    /* 5 */
    VK_COMPARE_OP_GREATER,          /* 6 */
    VK_COMPARE_OP_GREATER_OR_EQUAL  /* 7 */
};

static VkBlendFactor map_blend_factor(int code, int is_src, uint32_t fix, int *uses_const) {
    switch (code) {
    case 0:  return is_src ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
    case 1:  return is_src ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 2:
    case 6:  return VK_BLEND_FACTOR_SRC_ALPHA;
    case 3:
    case 7:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 4:
    case 8:  return VK_BLEND_FACTOR_DST_ALPHA;
    case 5:
    case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default: /* 10: SFIX / DFIX */
        if ((fix & 0xFFFFFFu) == 0x000000u) return VK_BLEND_FACTOR_ZERO;
        if ((fix & 0xFFFFFFu) == 0xFFFFFFu) return VK_BLEND_FACTOR_ONE;
        if (uses_const) *uses_const = 1;
        return VK_BLEND_FACTOR_CONSTANT_COLOR;
    }
}

static VkBlendOp map_blend_op(int op) {
    switch (op) {
    case 0:  return VK_BLEND_OP_ADD;
    case 1:  return VK_BLEND_OP_SUBTRACT;
    case 2:  return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 3:  return VK_BLEND_OP_MIN;
    case 4:  return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static uint64_t make_pipe_key(const psp_draw_state *st, float out_blend_const[4]) {
    out_blend_const[0] = 0.0f;
    out_blend_const[1] = 0.0f;
    out_blend_const[2] = 0.0f;
    out_blend_const[3] = 1.0f;

    int have_z = (st->zaddr != 0 && st->zbw > 0);
    uint32_t ztest = 0, zwrite = 0, zfunc = VK_COMPARE_OP_ALWAYS;
    uint32_t blend_en = 0, src_f = VK_BLEND_FACTOR_ONE, dst_f = VK_BLEND_FACTOR_ZERO, b_op = VK_BLEND_OP_ADD;
    uint32_t write_mask = 0xFu;

    if (st->clear) {
        ztest = (st->clear_depth && have_z) ? 1u : 0u;
        zwrite = ztest;
        zfunc = VK_COMPARE_OP_ALWAYS;
        write_mask = (st->clear_color ? 7u : 0u) | (st->clear_alpha ? 8u : 0u);
    } else {
        if (st->ztest && have_z) {
            ztest = 1u;
            zwrite = st->zwrite ? 1u : 0u;
            zfunc = (uint32_t)ZFUNC_MAP[st->zfunc & 7];
        }
        if (st->blend) {
            blend_en = 1u;
            int src_const = 0, dst_const = 0;
            src_f = (uint32_t)map_blend_factor(st->blend_src, 1, st->blend_sfix, &src_const);
            dst_f = (uint32_t)map_blend_factor(st->blend_dst, 0, st->blend_dfix, &dst_const);
            b_op  = (uint32_t)map_blend_op(st->blend_op);
            if (src_const) {
                out_blend_const[0] = (float)(st->blend_sfix & 0xFFu) / 255.0f;
                out_blend_const[1] = (float)((st->blend_sfix >> 8) & 0xFFu) / 255.0f;
                out_blend_const[2] = (float)((st->blend_sfix >> 16) & 0xFFu) / 255.0f;
            } else if (dst_const) {
                out_blend_const[0] = (float)(st->blend_dfix & 0xFFu) / 255.0f;
                out_blend_const[1] = (float)((st->blend_dfix >> 8) & 0xFFu) / 255.0f;
                out_blend_const[2] = (float)((st->blend_dfix >> 16) & 0xFFu) / 255.0f;
            }
        }
    }

    return (uint64_t)ztest |
           ((uint64_t)zwrite << 1) |
           ((uint64_t)zfunc << 2) |
           ((uint64_t)blend_en << 6) |
           ((uint64_t)src_f << 8) |
           ((uint64_t)dst_f << 14) |
           ((uint64_t)b_op << 20) |
           ((uint64_t)write_mask << 24);
}

static VkPipeline get_pipeline(uint64_t key) {
    for (int i = 0; i < g_num_pipelines; i++) {
        if (g_pipelines[i].key == key) return g_pipelines[i].pipeline;
    }

    uint32_t ztest      = (uint32_t)(key & 1u);
    uint32_t zwrite     = (uint32_t)((key >> 1) & 1u);
    VkCompareOp zfunc   = (VkCompareOp)((key >> 2) & 0xFu);
    uint32_t blend_en   = (uint32_t)((key >> 6) & 1u);
    VkBlendFactor src_f = (VkBlendFactor)((key >> 8) & 0x3Fu);
    VkBlendFactor dst_f = (VkBlendFactor)((key >> 14) & 0x3Fu);
    VkBlendOp b_op      = (VkBlendOp)((key >> 20) & 0xFu);
    VkColorComponentFlags write_mask = (VkColorComponentFlags)((key >> 24) & 0xFu);

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = g_vert_mod,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = g_frag_mod,
            .pName = "main"
        }
    };

    VkVertexInputBindingDescription bind = {
        .binding = 0,
        .stride = sizeof(vk_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attrs[3] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(vk_vertex, x) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,   .offset = offsetof(vk_vertex, rgba) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = offsetof(vk_vertex, u) }
    };

    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attrs
    };

    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };

    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1
    };

    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f
    };

    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = zfunc
    };

    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = blend_en ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = src_f,
        .dstColorBlendFactor = dst_f,
        .colorBlendOp = b_op,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = write_mask
    };

    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba
    };

    VkDynamicState dyn_states[3] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS
    };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3,
        .pDynamicStates = dyn_states
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = g_pipe_layout,
        .renderPass = g_render_pass,
        .subpass = 0
    };

    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    if (g_num_pipelines < MAX_PIPELINES) {
        g_pipelines[g_num_pipelines++] = (vk_pipe_entry){ key, pipe };
    }
    return pipe;
}

/* ---- Initialization ------------------------------------------------------ */

static int vk_init(int width, int height) {
    (void)width;
    (void)height;
    if (g_vk_inited) return 0;
    if (g_vk_failed) return -1;

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "emotionalsense",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "psprecomp",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app
    };
    if (vkCreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(g_inst, &dev_count, NULL);
    if (dev_count == 0) { g_vk_failed = 1; return -1; }
    VkPhysicalDevice devs[16];
    if (dev_count > 16) dev_count = 16;
    vkEnumeratePhysicalDevices(g_inst, &dev_count, devs);

    g_phys = devs[0];
    for (uint32_t i = 0; i < dev_count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g_phys = devs[i];
            break;
        }
    }

    VkPhysicalDeviceProperties chosen_props;
    vkGetPhysicalDeviceProperties(g_phys, &chosen_props);
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_mem_props);

    VkPhysicalDeviceFeatures avail_feats;
    vkGetPhysicalDeviceFeatures(g_phys, &avail_feats);
    g_precise_query = avail_feats.occlusionQueryPrecise ? 1 : 0;

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qcount, NULL);
    VkQueueFamilyProperties qprops[32];
    if (qcount > 32) qcount = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qcount, qprops);
    g_qfam = 0;
    for (uint32_t i = 0; i < qcount; i++) {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_qfam = i;
            break;
        }
    }

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_qfam,
        .queueCount = 1,
        .pQueuePriorities = &qprio
    };
    VkPhysicalDeviceFeatures req_feats = {
        .occlusionQueryPrecise = g_precise_query ? VK_TRUE : VK_FALSE
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &dqci,
        .pEnabledFeatures = &req_feats
    };
    if (vkCreateDevice(g_phys, &dci, NULL, &g_dev) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_qfam
    };
    if (vkCreateCommandPool(g_dev, &cpci, NULL, &g_cmd_pool) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    if (vkAllocateCommandBuffers(g_dev, &cbai, &g_cmd) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(g_dev, &fci, NULL, &g_fence) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_OCCLUSION,
        .queryCount = 1
    };
    vkCreateQueryPool(g_dev, &qpci, NULL, &g_query_pool);

    /* Attachments */
    g_scale = psp_render_scale < 1 ? 1 : psp_render_scale > 4 ? 4 : psp_render_scale;
    /* Before the first frame: a decode in the middle of a song starves the
     * audio ring, which is measured, not theoretical. */
    pack_scan();
    if (create_image((uint32_t)VW, (uint32_t)VH, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, &g_color_img, &g_color_mem, &g_color_view) != 0 ||
        create_image((uint32_t)VW, (uint32_t)VH, VK_FORMAT_D16_UNORM,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, &g_depth_img, &g_depth_mem, &g_depth_view) != 0) {
        g_vk_failed = 1;
        return -1;
    }

    /* RenderPass */
    VkAttachmentDescription atts[2] = {
        {
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        },
        {
            .format = VK_FORMAT_D16_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        }
    };
    VkAttachmentReference col_ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dep_ref = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &col_ref,
        .pDepthStencilAttachment = &dep_ref
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = atts,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    if (vkCreateRenderPass(g_dev, &rpci, NULL, &g_render_pass) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    VkImageView fb_views[2] = { g_color_view, g_depth_view };
    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = g_render_pass,
        .attachmentCount = 2,
        .pAttachments = fb_views,
        .width = (uint32_t)VW,
        .height = (uint32_t)VH,
        .layers = 1
    };
    if (vkCreateFramebuffer(g_dev, &fbci, NULL, &g_framebuffer) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    /* Buffers */
    VkMemoryPropertyFlags host_props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (create_buffer((uint32_t)(VW * VH) * 4u,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      host_props, &g_fb_staging_buf, &g_fb_staging_mem, (void **)&g_fb_staging_ptr) != 0 ||
        create_buffer((uint32_t)(VW * VH) * 2u,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      host_props, &g_zb_staging_buf, &g_zb_staging_mem, (void **)&g_zb_staging_ptr) != 0 ||
        create_buffer(MAX_VBO_VERTS * sizeof(vk_vertex),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      host_props, &g_vbo_buf, &g_vbo_mem, (void **)&g_vbo_ptr) != 0 ||
        create_buffer(TEX_STAGING_SIZE,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      host_props, &g_tex_staging_buf, &g_tex_staging_mem, (void **)&g_tex_staging_ptr) != 0) {
        g_vk_failed = 1;
        return -1;
    }

    /* Sampler & Descriptors */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
    };
    if (vkCreateSampler(g_dev, &sci, NULL, &g_sampler) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(g_dev, &sci, NULL, &g_sampler_linear) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    VkDescriptorSetLayoutBinding dslb = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &dslb
    };
    if (vkCreateDescriptorSetLayout(g_dev, &dslci, NULL, &g_desc_layout) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    VkDescriptorPoolSize dps = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MAX_TEX_CACHE * 2
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_TEX_CACHE * 2,
        .poolSizeCount = 1,
        .pPoolSizes = &dps
    };
    if (vkCreateDescriptorPool(g_dev, &dpci, NULL, &g_desc_pool) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(vk_push_constants)
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &g_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr
    };
    if (vkCreatePipelineLayout(g_dev, &plci, NULL, &g_pipe_layout) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    /* Shaders */
    VkShaderModuleCreateInfo vsmci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(g_vk_vert_spv),
        .pCode = g_vk_vert_spv
    };
    VkShaderModuleCreateInfo fsmci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(g_vk_frag_spv),
        .pCode = g_vk_frag_spv
    };
    if (vkCreateShaderModule(g_dev, &vsmci, NULL, &g_vert_mod) != VK_SUCCESS ||
        vkCreateShaderModule(g_dev, &fsmci, NULL, &g_frag_mod) != VK_SUCCESS) {
        g_vk_failed = 1;
        return -1;
    }

    g_dummy_white_slot = queue_solid_texture(0, 0xFFFFFFFFu);
    g_dummy_magenta_slot = queue_solid_texture(1, 0xFFFF00FFu);

    if (g_scale > 1) {
        /* At N > 1 the target is never re-uploaded, so it enters the render
         * pass in COLOR_ATTACHMENT_OPTIMAL from the first frame on. */
        VkCommandBufferBeginInfo cbbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        };
        vkResetCommandBuffer(g_cmd, 0);
        vkBeginCommandBuffer(g_cmd, &cbbi);
        VkImageMemoryBarrier b[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = g_color_img,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = g_depth_img,
                .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 }
            }
        };
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 0, NULL, 0, NULL, 2, b);
        vkEndCommandBuffer(g_cmd);
        vkResetFences(g_dev, 1, &g_fence);
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &g_cmd
        };
        vkQueueSubmit(g_queue, 1, &si, g_fence);
        vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
    }

    g_vk_inited = 1;
    psp_log("render: Vulkan backend initialized on %s\n", chosen_props.deviceName);
    return 0;
}

static void vk_shutdown(void) {
    if (!g_vk_inited) return;
    vk_flush();
    vkDeviceWaitIdle(g_dev);
    g_vk_inited = 0;
}

/* ---- Flushing a batch to GPU --------------------------------------------- */

static void vk_flush(void) {
    if (!g_vk_inited || g_num_cmds == 0 || !g_fb_addr || !g_fb_stride) {
        g_num_cmds = 0;
        g_vbo_count = 0;
        if (g_num_tex_uploads == 0) g_tex_staging_off = 0;
        g_batch_has_z = 0;
        g_batch_zaddr = 0;
        g_batch_zbw = 0;
        g_batch_id++;
        g_dirty_x0 = FB_W; g_dirty_y0 = FB_H;
        g_dirty_x1 = 0;    g_dirty_y1 = 0;
        return;
    }

    /* 1. Upload the guest framebuffer region this batch touches. At N > 1 the
     * target is the only copy of the frame: guest VRAM is never read back
     * (G0 measured zero guest reads of it) and never written to, so the
     * scaled image simply persists between batches and the guest's two
     * framebuffers both map to it. */
    int dx0 = g_dirty_x0, dy0 = g_dirty_y0, dx1 = g_dirty_x1, dy1 = g_dirty_y1;
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > FB_W) dx1 = FB_W;
    if (dy1 > FB_H) dy1 = FB_H;
    if (dx0 >= dx1 || dy0 >= dy1) { dx0 = dy0 = dx1 = dy1 = 0; }
    if (g_scale > 1) {
        dx0 = dy0 = dx1 = dy1 = 0;
    } else if (g_fb_fmt == 3) {
        for (int y = dy0; y < dy1; y++) {
            uint32_t row_addr = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + dx0) * 4u;
            const void *src = psp_mem_ptr(row_addr, (uint32_t)(dx1 - dx0) * 4u);
            if (src) {
                memcpy(g_fb_staging_ptr + y * FB_W + dx0, src, (size_t)(dx1 - dx0) * 4u);
            } else {
                for (int x = dx0; x < dx1; x++)
                    g_fb_staging_ptr[y * FB_W + x] = psp_read32(row_addr + (uint32_t)(x - dx0) * 4u);
            }
        }
    } else {
        for (int y = dy0; y < dy1; y++) {
            uint32_t row_addr = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + dx0) * 2u;
            for (int x = dx0; x < dx1; x++)
                g_fb_staging_ptr[y * FB_W + x] = psp_color_expand(psp_read16(row_addr + (uint32_t)(x - dx0) * 2u), g_fb_fmt);
        }
    }

    /* 2. Upload current guest VRAM depth buffer if used */
    if (g_scale == 1 && g_batch_has_z && g_batch_zaddr && g_batch_zbw > 0) {
        for (int y = dy0; y < dy1; y++) {
            uint32_t row_addr = g_batch_zaddr + (uint32_t)(y * g_batch_zbw + dx0) * 2u;
            const void *src = psp_mem_ptr(row_addr, (uint32_t)(dx1 - dx0) * 2u);
            if (src) {
                memcpy(g_zb_staging_ptr + y * FB_W + dx0, src, (size_t)(dx1 - dx0) * 2u);
            } else {
                for (int x = dx0; x < dx1; x++)
                    g_zb_staging_ptr[y * FB_W + x] = psp_read16(row_addr + (uint32_t)(x - dx0) * 2u);
            }
        }
    }

    /* 3. Record command buffer */
    vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(g_cmd, &cbbi);

    /* 3a. Upload any queued textures */
    if (g_num_tex_uploads > 0) {
        for (int i = 0; i < g_num_tex_uploads; i++) {
            vk_tex_entry *te = &g_tex_cache[g_tex_uploads[i].tex_slot];
            VkImageMemoryBarrier imb = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = te->image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            };
            vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);

            VkBufferImageCopy bic = {
                .bufferOffset = g_tex_uploads[i].staging_offset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageExtent = { (uint32_t)g_tex_uploads[i].w, (uint32_t)g_tex_uploads[i].h, 1 }
            };
            vkCmdCopyBufferToImage(g_cmd, g_tex_staging_buf, te->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

            imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
            te->layout_ready = 1;
        }
    }

    /* 3b. Copy staging framebuffer (and depth buffer) into attachments. At
     * N > 1 the target is persistent (see step 1), so it is already in
     * COLOR_ATTACHMENT_OPTIMAL and the render pass loads it as is. */
    VkBufferImageCopy col_copy = {
        .bufferOffset = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t)VW, (uint32_t)VH, 1 }
    };
    if (g_scale == 1) {
    VkImageMemoryBarrier pre_barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_color_img,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_depth_img,
            .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 }
        }
    };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre_barriers);

    vkCmdCopyBufferToImage(g_cmd, g_fb_staging_buf, g_color_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &col_copy);

    if (g_batch_has_z) {
        VkBufferImageCopy dep_copy = {
            .bufferOffset = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)VW, (uint32_t)VH, 1 }
        };
        vkCmdCopyBufferToImage(g_cmd, g_zb_staging_buf, g_depth_img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dep_copy);
    }

    VkImageMemoryBarrier to_att[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_color_img,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_depth_img,
            .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 }
        }
    };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 0, NULL, 0, NULL, 2, to_att);
    }

    if (g_query_pool) {
        vkCmdResetQueryPool(g_cmd, g_query_pool, 0, 1);
    }

    /* 3c. RenderPass */
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_render_pass,
        .framebuffer = g_framebuffer,
        .renderArea = { { 0, 0 }, { (uint32_t)VW, (uint32_t)VH } }
    };
    vkCmdBeginRenderPass(g_cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = { 0.0f, 0.0f, (float)VW, (float)VH, 0.0f, 1.0f };
    vkCmdSetViewport(g_cmd, 0, 1, &vp);

    VkDeviceSize vbo_off = 0;
    vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_vbo_buf, &vbo_off);

    int query_active = 0, query_used = 0;

    uint64_t last_pipe_key = UINT64_MAX;
    int last_tex = -1;
    int last_sc[4] = { -1, -1, -1, -1 };

    for (int i = 0; i < g_num_cmds; i++) {
        const vk_draw_cmd *dc = &g_cmds[i];
        if (g_query_pool) {
            if (dc->count_pixels && !query_active && !query_used) {
                vkCmdBeginQuery(g_cmd, g_query_pool, 0, g_precise_query ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
                query_active = 1;
                query_used = 1;
            } else if (!dc->count_pixels && query_active) {
                vkCmdEndQuery(g_cmd, g_query_pool, 0);
                query_active = 0;
            }
        }
        if (dc->pipe_key != last_pipe_key) {
            VkPipeline pipe = get_pipeline(dc->pipe_key);
            if (!pipe) continue;
            vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            last_pipe_key = dc->pipe_key;
        }
        const int want_tex = dc->tex_slot * 2 + (dc->tex_filter ? 1 : 0);
        if (want_tex != last_tex) {
            vk_tex_entry *te = &g_tex_cache[dc->tex_slot];
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    g_pipe_layout, 0, 1,
                                    dc->tex_filter ? &te->desc_set_linear : &te->desc_set, 0, NULL);
            last_tex = want_tex;
        }
        if (dc->sc_x != last_sc[0] || dc->sc_y != last_sc[1] ||
            dc->sc_w != last_sc[2] || dc->sc_h != last_sc[3]) {
            VkRect2D sc = { { dc->sc_x * g_scale, dc->sc_y * g_scale },
                            { (uint32_t)(dc->sc_w * g_scale), (uint32_t)(dc->sc_h * g_scale) } };
            vkCmdSetScissor(g_cmd, 0, 1, &sc);
            last_sc[0] = dc->sc_x; last_sc[1] = dc->sc_y;
            last_sc[2] = dc->sc_w; last_sc[3] = dc->sc_h;
        }
        vkCmdSetBlendConstants(g_cmd, dc->blend_const);
        vkCmdPushConstants(g_cmd, g_pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(vk_push_constants), &dc->pc);
        vkCmdDraw(g_cmd, dc->vert_count, 1, dc->first_vert, 0);
    }

    if (query_active) {
        vkCmdEndQuery(g_cmd, g_query_pool, 0);
    }
    vkCmdEndRenderPass(g_cmd);

    /* 3d. Copy attachments back to staging. Only at 1x: at N > 1 the frame
     * reaches the window through vk_copy_frame() at present time. */
    if (g_scale == 1) {
    VkImageMemoryBarrier post_barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_color_img,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_depth_img,
            .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 }
        }
    };
    vkCmdPipelineBarrier(g_cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, post_barriers);

    vkCmdCopyImageToBuffer(g_cmd, g_color_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           g_fb_staging_buf, 1, &col_copy);
    if (g_batch_has_z) {
        VkBufferImageCopy dep_copy = {
            .bufferOffset = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)VW, (uint32_t)VH, 1 }
        };
        vkCmdCopyImageToBuffer(g_cmd, g_depth_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g_zb_staging_buf, 1, &dep_copy);
    }

    VkMemoryBarrier host_mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT
    };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_mb, 0, NULL, 0, NULL);
    }

    vkEndCommandBuffer(g_cmd);

    /* 4. Submit & Wait */
    vkResetFences(g_dev, 1, &g_fence);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_cmd
    };
    vkQueueSubmit(g_queue, 1, &si, g_fence);
    vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX);

    if (g_query_pool && query_used) {
        uint64_t samples = 0;
        if (vkGetQueryPoolResults(g_dev, g_query_pool, 0, 1, sizeof(samples), &samples,
                                  sizeof(samples), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            psp_render_add_pixels(samples);
        }
    }

    /* 5. Write the touched region back to guest VRAM. Only at 1x: at N > 1
     * the frame stays in the scaled target and reaches the window through
     * vk_frame() instead of a downsampled copy in guest memory. */
    if (g_scale > 1) {
        /* nothing to write back */
    } else if (g_fb_fmt == 3) {
        for (int y = dy0; y < dy1; y++) {
            uint32_t row_addr = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + dx0) * 4u;
            void *dst = psp_mem_ptr(row_addr, (uint32_t)(dx1 - dx0) * 4u);
            if (dst) {
                memcpy(dst, g_fb_staging_ptr + y * FB_W + dx0, (size_t)(dx1 - dx0) * 4u);
            } else {
                for (int x = dx0; x < dx1; x++)
                    psp_write32(row_addr + (uint32_t)(x - dx0) * 4u, g_fb_staging_ptr[y * FB_W + x]);
            }
        }
    } else {
        for (int y = dy0; y < dy1; y++) {
            uint32_t row_addr = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + dx0) * 2u;
            for (int x = dx0; x < dx1; x++)
                psp_write16(row_addr + (uint32_t)(x - dx0) * 2u,
                            (uint16_t)color_pack(g_fb_staging_ptr[y * FB_W + x], g_fb_fmt));
        }
    }

    if (g_scale == 1 && g_batch_has_z && g_batch_zaddr && g_batch_zbw > 0) {
        for (int y = dy0; y < dy1; y++) {
            uint32_t row_addr = g_batch_zaddr + (uint32_t)(y * g_batch_zbw + dx0) * 2u;
            void *dst = psp_mem_ptr(row_addr, (uint32_t)(dx1 - dx0) * 2u);
            if (dst) {
                memcpy(dst, g_zb_staging_ptr + y * FB_W + dx0, (size_t)(dx1 - dx0) * 2u);
            } else {
                for (int x = dx0; x < dx1; x++)
                    psp_write16(row_addr + (uint32_t)(x - dx0) * 2u, g_zb_staging_ptr[y * FB_W + x]);
            }
        }
    }

    g_num_cmds = 0;
    g_vbo_count = 0;
    g_num_tex_uploads = 0;
    g_tex_staging_off = 0;
    g_batch_has_z = 0;
    g_batch_zaddr = 0;
    g_batch_zbw = 0;
    g_batch_id++;
    g_dirty_x0 = FB_W; g_dirty_y0 = FB_H;
    g_dirty_x1 = 0;    g_dirty_y1 = 0;
}

/* ---- Backend interface methods ------------------------------------------- */

static void vk_set_target(uint32_t addr, uint32_t stride, int fmt) {
    if (g_num_cmds > 0 && (addr != g_fb_addr || stride != g_fb_stride || fmt != g_fb_fmt)) {
        vk_flush();
    }
    g_fb_addr = addr;
    g_fb_stride = stride;
    g_fb_fmt = fmt;
}

/* Bilinear for this draw. In auto mode the game's TEXFILTER bit decides, with
 * one exception: the decoded video lives in VRAM at 480x272 and is stretched
 * over the whole screen, so at N > 1 nearest turns it into visible blocks.
 * Only the video is ever a VRAM texture (G0 measured that), and at 1x the
 * filter stays exactly as the game asked. */
static int tex_bilinear(const psp_draw_state *st) {
    int want = psp_render_tex_filter < 0 ? st->tex_filter : psp_render_tex_filter;
    if (psp_render_tex_filter < 0 && g_scale > 1 &&
        (st->tex_addr & PSP_ADDR_MASK) >= PSP_VRAM_BASE)
        return 1;
    return want ? 1 : 0;
}

static void emit_tri(const psp_vertex *a, const psp_vertex *b, const psp_vertex *c, int gouraud) {
    if (a->behind || b->behind || c->behind) return;
    int area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;

    uint32_t col_a = gouraud ? a->rgba : c->rgba;
    uint32_t col_b = gouraud ? b->rgba : c->rgba;
    uint32_t col_c = c->rgba;

    /* Positions stay in guest pixels: the viewport already maps the 480x272
     * clip space onto the scaled target, so the triangles are rasterized at
     * N times the resolution without touching the geometry. */
    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ a->fx, a->fy, a->z, col_a,
                                            a->u * g_uv_scale, a->v * g_uv_scale };
    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ b->fx, b->fy, b->z, col_b,
                                            b->u * g_uv_scale, b->v * g_uv_scale };
    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ c->fx, c->fy, c->z, col_c,
                                            c->u * g_uv_scale, c->v * g_uv_scale };
}

static void emit_sprite(const psp_vertex *a, const psp_vertex *b) {
    int x0 = a->x < b->x ? a->x : b->x, x1 = a->x > b->x ? a->x : b->x;
    int y0 = a->y < b->y ? a->y : b->y, y1 = a->y > b->y ? a->y : b->y;
    if (x0 == x1 || y0 == y1) return;
    /* the same corners with their sub-pixel fraction (== x0.. unless the
     * lane keeps it, see psp_render_subpixel) */
    float fx0 = a->x < b->x ? a->fx : b->fx, fx1 = a->x < b->x ? b->fx : a->fx;
    float fy0 = a->y < b->y ? a->fy : b->fy, fy1 = a->y < b->y ? b->fy : a->fy;

    float u0 = a->x < b->x ? a->u : b->u;
    float u1 = a->x < b->x ? b->u : a->u;
    float v0 = a->y < b->y ? a->v : b->v;
    float v1 = a->y < b->y ? b->v : a->v;
    uint32_t col = b->rgba;
    float z = b->z;

    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ fx0, fy0, z, col,
                                            u0 * g_uv_scale, v0 * g_uv_scale };
    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ fx1, fy0, z, col,
                                            u1 * g_uv_scale, v0 * g_uv_scale };
    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ fx0, fy1, z, col,
                                            u0 * g_uv_scale, v1 * g_uv_scale };

    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ fx1, fy0, z, col,
                                            u1 * g_uv_scale, v0 * g_uv_scale };
    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ fx1, fy1, z, col,
                                            u1 * g_uv_scale, v1 * g_uv_scale };
    g_vbo_ptr[g_vbo_count++] = (vk_vertex){ fx0, fy1, z, col,
                                            u0 * g_uv_scale, v1 * g_uv_scale };
}

static void vk_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    if (!g_vk_inited && vk_init(FB_W, FB_H) != 0) {
        psp_render_software.draw(prim, v, count, st);
        return;
    }
    if (!g_fb_addr || !g_fb_stride || count <= 0) return;

    int sx0 = st->sc_x0 < 0 ? 0 : st->sc_x0;
    int sy0 = st->sc_y0 < 0 ? 0 : st->sc_y0;
    int sx1 = st->sc_x1 >= FB_W ? FB_W - 1 : st->sc_x1;
    int sy1 = st->sc_y1 >= FB_H ? FB_H - 1 : st->sc_y1;
    if (sx0 > sx1 || sy0 > sy1) return;

    int uses_z = (st->zaddr != 0 && st->zbw > 0) &&
                 ((st->clear && st->clear_depth) || (!st->clear && st->ztest));
    if (uses_z && g_batch_has_z &&
        (st->zaddr != g_batch_zaddr || st->zbw != g_batch_zbw)) {
        vk_flush();
    }

    uint32_t max_new_verts = (uint32_t)count * 3u;
    if (g_vbo_count + max_new_verts > MAX_VBO_VERTS || g_num_cmds >= MAX_DRAW_CMDS) {
        vk_flush();
    }

    int tex_slot = resolve_texture(st);
    g_uv_scale = g_tex_cache[tex_slot].uv_scale > 0
                     ? (float)g_tex_cache[tex_slot].uv_scale : 1.0f;

    if (uses_z) {
        g_batch_has_z = 1;
        g_batch_zaddr = st->zaddr;
        g_batch_zbw = st->zbw;
    }

    uint32_t first_vert = g_vbo_count;
    switch (prim) {
    case PSP_PRIM_SPRITES:
        for (int i = 0; i + 1 < count; i += 2) emit_sprite(&v[i], &v[i + 1]);
        break;
    case PSP_PRIM_TRIANGLES:
        for (int i = 0; i + 2 < count; i += 3) emit_tri(&v[i], &v[i + 1], &v[i + 2], st->gouraud);
        break;
    case PSP_PRIM_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < count; i++) emit_tri(&v[i], &v[i + 1], &v[i + 2], st->gouraud);
        break;
    case PSP_PRIM_TRIANGLE_FAN:
        for (int i = 1; i + 1 < count; i++) emit_tri(&v[0], &v[i], &v[i + 1], st->gouraud);
        break;
    default:
        break;
    }

    uint32_t vert_count = g_vbo_count - first_vert;
    if (vert_count == 0) return;

    /* Grow the dirty rect by the vertices actually emitted, clamped to the
     * scissor. */
    for (uint32_t i = first_vert; i < g_vbo_count; i++) {
        int x = (int)g_vbo_ptr[i].x, y = (int)g_vbo_ptr[i].y;
        if (x < sx0) x = sx0;
        if (x > sx1) x = sx1;
        if (y < sy0) y = sy0;
        if (y > sy1) y = sy1;
        if (x < g_dirty_x0) g_dirty_x0 = x;
        if (y < g_dirty_y0) g_dirty_y0 = y;
        if (x + 1 > g_dirty_x1) g_dirty_x1 = x + 1;
        if (y + 1 > g_dirty_y1) g_dirty_y1 = y + 1;
    }

    float blend_const[4];
    uint64_t pipe_key = make_pipe_key(st, blend_const);

    uint32_t flags = (st->clear ? 1u : 0u) |
                     (st->tex_enable ? 2u : 0u) |
                     (st->tex_clamp_u ? 4u : 0u) |
                     (st->tex_clamp_v ? 8u : 0u) |
                     ((uint32_t)(st->tex_func & 7) << 4) |
                     (st->tex_alpha ? 128u : 0u) |
                     (st->tex_double ? 256u : 0u) |
                     (st->atest ? 512u : 0u) |
                     ((uint32_t)(st->atest_func & 7) << 10) |
                     ((uint32_t)(st->atest_ref & 0xFF) << 16) |
                     ((uint32_t)(st->atest_mask & 0xFF) << 24) |
                     ((tex_bilinear(st) ? 8192u : 0u));   /* F_BILINEAR */

    vk_push_constants pc = {
        .tex_w = (int32_t)(st->tex_w * (int)g_uv_scale),
        .tex_h = (int32_t)(st->tex_h * (int)g_uv_scale),
        .flags = flags,
        .tex_env = st->tex_env
    };

    int sc_x = sx0, sc_y = sy0, sc_w = sx1 - sx0 + 1, sc_h = sy1 - sy0 + 1;
    int count_pixels = !(st->clear && !st->clear_color && !st->clear_alpha);

    /* Merge with previous draw command if state is identical */
    if (g_num_cmds > 0) {
        vk_draw_cmd *prev = &g_cmds[g_num_cmds - 1];
        if (prev->first_vert + prev->vert_count == first_vert &&
            prev->pipe_key == pipe_key &&
            prev->tex_slot == tex_slot &&
            prev->tex_filter == st->tex_filter &&
            prev->sc_x == sc_x && prev->sc_y == sc_y &&
            prev->sc_w == sc_w && prev->sc_h == sc_h &&
            prev->count_pixels == count_pixels &&
            memcmp(prev->blend_const, blend_const, sizeof(blend_const)) == 0 &&
            memcmp(&prev->pc, &pc, sizeof(pc)) == 0) {
            prev->vert_count += vert_count;
            return;
        }
    }

    vk_draw_cmd *dc = &g_cmds[g_num_cmds++];
    dc->first_vert = first_vert;
    dc->vert_count = vert_count;
    dc->pipe_key = pipe_key;
    dc->tex_slot = tex_slot;
    dc->tex_filter = st->tex_filter;
    dc->sc_x = sc_x;
    dc->sc_y = sc_y;
    dc->sc_w = sc_w;
    dc->sc_h = sc_h;
    memcpy(dc->blend_const, blend_const, sizeof(blend_const));
    dc->pc = pc;
    dc->count_pixels = count_pixels;
}

static void vk_finish(void) {
    vk_flush();
}

/* Copy the scaled target to the host-visible staging buffer, once per
 * presented frame. Only the window and captures read it. */
static void vk_copy_frame(void) {
    if (!g_vk_inited || g_scale <= 1) return;
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkResetCommandBuffer(g_cmd, 0);
    vkBeginCommandBuffer(g_cmd, &cbbi);
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_color_img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy c = {
        .bufferOffset = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t)VW, (uint32_t)VH, 1 }
    };
    vkCmdCopyImageToBuffer(g_cmd, g_color_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           g_fb_staging_buf, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkMemoryBarrier hb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT
    };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(g_cmd);
    vkResetFences(g_dev, 1, &g_fence);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &g_cmd
    };
    vkQueueSubmit(g_queue, 1, &si, g_fence);
    vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
}

static void vk_present(void) {
    vk_flush();
    vk_copy_frame();
}

/* The frame at internal resolution, for the window and for captures. At 1x
 * the frame also lives in guest VRAM, so the caller can use either. */
static int vk_frame(const uint32_t **px, int *w, int *h) {
    if (!g_vk_inited || g_scale <= 1) return -1;
    *px = g_fb_staging_ptr;
    *w = VW;
    *h = VH;
    return 0;
}

const psp_render_backend psp_render_vulkan = {
    "vulkan", vk_init, vk_shutdown, vk_set_target, vk_draw, vk_finish, vk_present,
    vk_frame
};

#endif /* PSPRECOMP_VULKAN */
