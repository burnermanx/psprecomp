#version 450
/* PSP GE fragment: texture sample, texture function, alpha test.
 *
 * The push constant carries the draw state the shader needs. Bit layout of
 * `flags` is defined by render_vulkan.c next to vk_push_constants; keep the
 * two in sync.
 *
 * Sampling is done with texelFetch and explicit wrapping so that the software
 * backend (src/render.c tex_sample) and this shader agree texel for texel.
 * The sampler's own filter is deliberately unused: bilinear is spelled out
 * here because the wrap mode has to be applied per tap. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(push_constant) uniform PC {
    int  tex_w;
    int  tex_h;
    uint flags;
    uint tex_env;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;

const uint F_CLEAR    = 1u;
const uint F_TEX      = 2u;
const uint F_CLAMP_U  = 4u;
const uint F_CLAMP_V  = 8u;
const uint F_ALPHA    = 128u;
const uint F_DOUBLE   = 256u;
const uint F_ATEST    = 512u;
const uint F_BILINEAR = 8192u;

/* Wrap one tap coordinate. Clamp and power-of-two mirror are the only modes
 * the GE has. */
ivec2 wrap_tap(ivec2 p) {
    if ((pc.flags & F_CLAMP_U) != 0u) p.x = clamp(p.x, 0, pc.tex_w - 1);
    else                              p.x = p.x & (pc.tex_w - 1);
    if ((pc.flags & F_CLAMP_V) != 0u) p.y = clamp(p.y, 0, pc.tex_h - 1);
    else                              p.y = p.y & (pc.tex_h - 1);
    return p;
}

ivec4 tap(ivec2 p) {
    return ivec4(clamp(texelFetch(tex, wrap_tap(p), 0) * 255.0 + 0.5, 0.0, 255.0));
}

/* Same rounding as src/render.c lerp8. */
int lerp8(int a, int b, float t) {
    return int(float(a) + (float(b) - float(a)) * t + 0.5);
}

/* Texel centres are at i+0.5, so the four taps around a sample point are the
 * floor of (uv - 0.5) and its neighbours. */
ivec4 sample_bilinear(vec2 uv) {
    vec2 p = uv - vec2(0.5);
    vec2 f0 = floor(p);
    vec2 f = p - f0;
    ivec2 i0 = ivec2(f0);
    ivec4 c00 = tap(i0);
    ivec4 c10 = tap(i0 + ivec2(1, 0));
    ivec4 c01 = tap(i0 + ivec2(0, 1));
    ivec4 c11 = tap(i0 + ivec2(1, 1));
    return ivec4(lerp8(lerp8(c00.r, c10.r, f.x), lerp8(c01.r, c11.r, f.x), f.y),
                 lerp8(lerp8(c00.g, c10.g, f.x), lerp8(c01.g, c11.g, f.x), f.y),
                 lerp8(lerp8(c00.b, c10.b, f.x), lerp8(c01.b, c11.b, f.x), f.y),
                 lerp8(lerp8(c00.a, c10.a, f.x), lerp8(c01.a, c11.a, f.x), f.y));
}

/* Texture function: combine the texel `t` with the fragment colour `c`.
 * Mirrors src/render.c tex_combine. */
ivec4 combine(ivec4 t, ivec4 c) {
    uint fmt = (pc.flags >> 4) & 7u;
    bool use_alpha = (pc.flags & F_ALPHA) != 0u;
    int r, g, b, a;

    if (fmt == 0u) {                      /* modulate */
        r = (t.r * c.r + 127) / 255;
        g = (t.g * c.g + 127) / 255;
        b = (t.b * c.b + 127) / 255;
        a = use_alpha ? (t.a * c.a + 127) / 255 : c.a;
    } else if (fmt == 1u) {               /* decal */
        if (use_alpha) {
            r = (t.r * t.a + c.r * (255 - t.a) + 127) / 255;
            g = (t.g * t.a + c.g * (255 - t.a) + 127) / 255;
            b = (t.b * t.a + c.b * (255 - t.a) + 127) / 255;
        } else {
            r = t.r; g = t.g; b = t.b;
        }
        a = c.a;
    } else if (fmt == 2u) {               /* blend towards tex_env */
        int er = int(pc.tex_env & 255u);
        int eg = int((pc.tex_env >> 8) & 255u);
        int eb = int((pc.tex_env >> 16) & 255u);
        r = (c.r * (255 - t.r) + er * t.r + 127) / 255;
        g = (c.g * (255 - t.g) + eg * t.g + 127) / 255;
        b = (c.b * (255 - t.b) + eb * t.b + 127) / 255;
        a = use_alpha ? (t.a * c.a + 127) / 255 : c.a;
    } else if (fmt == 3u) {               /* replace */
        r = t.r; g = t.g; b = t.b;
        a = use_alpha ? t.a : c.a;
    } else {                              /* add */
        r = t.r + c.r; g = t.g + c.g; b = t.b + c.b;
        a = use_alpha ? (t.a * c.a + 127) / 255 : c.a;
    }

    if ((pc.flags & F_DOUBLE) != 0u) { r *= 2; g *= 2; b *= 2; }
    return ivec4(r, g, b, a);
}

void main() {
    ivec4 c = ivec4(clamp(v_color * 255.0 + 0.5, 0.0, 255.0));
    ivec4 outc = c;

    bool textured = (pc.flags & F_CLEAR) == 0u &&
                    (pc.flags & F_TEX) != 0u && pc.tex_w > 0 && pc.tex_h > 0;
    if (textured) {
        ivec4 t = ((pc.flags & F_BILINEAR) != 0u) ? sample_bilinear(v_uv)
                                                  : tap(ivec2(floor(v_uv)));
        outc = clamp(combine(t, c), ivec4(0), ivec4(255));
    }

    if ((pc.flags & F_ATEST) != 0u) {
        uint f = (pc.flags >> 10) & 7u;
        int ref = int((pc.flags >> 16) & 255u);
        int mask = int((pc.flags >> 24) & 255u);
        int a = outc.a & mask, rr = ref & mask;
        bool pass;
        if (f == 0u)      pass = false;
        else if (f == 1u) pass = true;
        else if (f == 2u) pass = (a == rr);
        else if (f == 3u) pass = (a != rr);
        else if (f == 4u) pass = (a < rr);
        else if (f == 5u) pass = (a <= rr);
        else if (f == 6u) pass = (a > rr);
        else              pass = (a >= rr);
        if (!pass) discard;
    }

    o_color = vec4(outc) * (1.0 / 255.0);
}
