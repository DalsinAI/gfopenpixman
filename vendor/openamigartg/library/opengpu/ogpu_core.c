/* Copyright (c) 2026 AmigaChrome Team. OpenRTG, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 *
 * GFOpenGPU's core (DESIGN.md section 5): one rasterizer for every back end.
 * The 68k runs it as the CPU back end, the Cradle's runtime runs it behind
 * ACRTG.gpu, and a PiStorm's spare ARM core runs it behind PiStorm.gpu;
 * tests/test_opengpu.c checks it against simple per-pixel references and
 * keeps golden checksums every build must match. Stream v1.4: the 2D
 * commands here; the 3D range (0x0030-0x003F) goes to ogpu_3d.c when the
 * build links it (OGPU_WITH_3D).
 *
 * Integers only, no C library, pixels read and written byte by byte in the
 * Amiga's order, so the result is the same on a big- or little-endian CPU.
 *
 * Written for the 68k (8 October 2026). The pixel loops work on bytes and
 * channels (A, R, G, B kept apart), multiply only 16-bit values (MUL16:
 * mulu.w), shift only by constants of 8 or less, and step pointers instead
 * of multiplying per pixel. On a real 68040 that is as fast as the plain
 * form; on the AC090's JIT, where a shift by a register, a 32-bit multiply
 * or a bitfield instruction costs 70 ns or more (a shift by 24 nearly 2 us),
 * it is hundreds of times faster. The 68k builds pass -mnobitfield, as GCC
 * otherwise picks bfextu for (x >> 8) & 255. OGPU_PLAIN_C=1 puts back the
 * plain form of the few helpers that differ, for when the JIT is fixed.
 */
#include "ogpu_core.h"
#ifdef OGPU_WITH_3D
#include "ogpu_3d.h"
#endif

#ifndef OGPU_PLAIN_C
#define OGPU_PLAIN_C 0
#endif

/* Each command is its own function: inlined into the run, they only make
 * it hard to read what a pixel costs. */
#if defined(__GNUC__)
#define OP_FN static __attribute__((noinline)) int
#else
#define OP_FN static int
#endif

/* The per-pixel helpers go inline into each command's loop: a call costs
 * more than the work. */
#if defined(__GNUC__)
#define PX_FN static inline __attribute__((always_inline))
#else
#define PX_FN static
#endif

/* x * y for x, y in 0..65535: one mulu.w on the 68k. GCC turns the C form
 * into a 32-bit mulu.l on a 68040, so the 68k gets it in assembly. */
#if (defined(__mc68000__) || defined(__m68k__)) && defined(__GNUC__) && !OGPU_PLAIN_C
static inline unsigned MUL16(unsigned x, unsigned y) {
    __asm__ ("mulu.w %1,%0" : "+d"(x) : "d"(y) : "cc");
    return x;
}
static inline int SMUL16(int x, int y) {
    __asm__ ("muls.w %1,%0" : "+d"(x) : "d"(y) : "cc");
    return x;
}
#else
#define MUL16(x, y) ((unsigned)(unsigned short)(x) * (unsigned)(unsigned short)(y))
#define SMUL16(x, y) ((int)(short)(x) * (int)(short)(y))
#endif

/* ---- small helpers ---------------------------------------------------------------- */

static ogpu_u32 rd32(const ogpu_u8 *p) {
    return ((ogpu_u32)p[0] << 24) | ((ogpu_u32)p[1] << 16) | ((ogpu_u32)p[2] << 8) | (ogpu_u32)p[3];
}

static int s16(ogpu_u32 v) { v &= 0xFFFFUL; return v >= 0x8000UL ? (int)v - 0x10000 : (int)v; }
static int u16(ogpu_u32 v) { return (int)(v & 0xFFFFUL); }
static int hi_s(ogpu_u32 w) { return s16(w >> 16); }
static int lo_s(ogpu_u32 w) { return s16(w); }
static int hi_u(ogpu_u32 w) { return u16(w >> 16); }
static int lo_u(ogpu_u32 w) { return u16(w); }

/* The bytes of a word, high first, without a shift by 24 (a register shift
 * on the 68k). */
static void bytes_of(ogpu_u32 v, ogpu_u8 *b) {
#if OGPU_PLAIN_C
    b[0] = (ogpu_u8)(v >> 24); b[1] = (ogpu_u8)(v >> 16); b[2] = (ogpu_u8)(v >> 8); b[3] = (ogpu_u8)v;
#else
    ogpu_u32 hi = v >> 16;                  /* swap */
    b[0] = (ogpu_u8)(hi >> 8); b[1] = (ogpu_u8)hi; b[2] = (ogpu_u8)(v >> 8); b[3] = (ogpu_u8)v;
#endif
}

/* Format tables. (A switch over grouped formats becomes GCC's "1 << format,
 * then AND" bit test: a shift by a register, slow on the AC090's JIT.) */
static const unsigned char fmt_bpp[OGPU_FMT_COUNT] = { 0, 1, 2, 4, 1, 1, 2, 2, 2, 4, 4 };
static const unsigned char fmt_colour[OGPU_FMT_COUNT] = { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1 };

static int bytes_per_pixel(int format) {
    return format > 0 && format < OGPU_FMT_COUNT ? fmt_bpp[format] : 0;
}

/* Formats with colours (COMPOSITE, MASK and the blends draw into them). */
static int is_colour(int format) {
    return format > 0 && format < OGPU_FMT_COUNT && fmt_colour[format];
}

/* A pixel as its four channels, 0-255 each. */
struct ch { unsigned a, r, g, b; };

PX_FN unsigned x5(unsigned v) { return (v << 3) | (v >> 2); }   /* 5 bits to 8 */
PX_FN unsigned x6(unsigned v) { return (v << 2) | (v >> 4); }   /* 6 bits to 8 */

/* Read a pixel's channels. CLUT8 and INDEX8 have none (callers keep them
 * out); A8 is coverage over black. */
PX_FN void ld(const ogpu_u8 *p, int f, struct ch *o) {
    unsigned hi, lo;
    switch (f) {
    case OGPU_FMT_ARGB32: o->a = p[0]; o->r = p[1]; o->g = p[2]; o->b = p[3]; return;
    case OGPU_FMT_BGRA32: o->b = p[0]; o->g = p[1]; o->r = p[2]; o->a = p[3]; return;
    case OGPU_FMT_XRGB32: o->a = 255; o->r = p[1]; o->g = p[2]; o->b = p[3]; return;
    case OGPU_FMT_RGB565: hi = p[0]; lo = p[1]; goto c565;
    case OGPU_FMT_RGB565PC: lo = p[0]; hi = p[1];
    c565:
        o->a = 255; o->r = x5(hi >> 3); o->g = x6(((hi & 7) << 3) | (lo >> 5)); o->b = x5(lo & 31);
        return;
    case OGPU_FMT_RGB555: hi = p[0]; lo = p[1]; goto c555;
    case OGPU_FMT_RGB555PC: lo = p[0]; hi = p[1];
    c555:
        o->a = 255; o->r = x5((hi >> 2) & 31); o->g = x5(((hi & 3) << 3) | (lo >> 5)); o->b = x5(lo & 31);
        return;
    case OGPU_FMT_A8: o->a = p[0]; o->r = o->g = o->b = 0; return;
    }
    o->a = 255; o->r = o->g = o->b = p[0];
}

/* Write channels as a pixel (truncating to 5 or 6 bits, as v1.0 did). */
PX_FN void st(ogpu_u8 *p, int f, unsigned a, unsigned r, unsigned g, unsigned b) {
    unsigned hi, lo;
    switch (f) {
    case OGPU_FMT_ARGB32: p[0] = (ogpu_u8)a; p[1] = (ogpu_u8)r; p[2] = (ogpu_u8)g; p[3] = (ogpu_u8)b; return;
    case OGPU_FMT_BGRA32: p[0] = (ogpu_u8)b; p[1] = (ogpu_u8)g; p[2] = (ogpu_u8)r; p[3] = (ogpu_u8)a; return;
    case OGPU_FMT_XRGB32: p[0] = 0; p[1] = (ogpu_u8)r; p[2] = (ogpu_u8)g; p[3] = (ogpu_u8)b; return;
    case OGPU_FMT_RGB565:
        p[0] = (ogpu_u8)((r & 0xF8) | (g >> 5)); p[1] = (ogpu_u8)(((g & 0x1C) << 3) | (b >> 3)); return;
    case OGPU_FMT_RGB565PC:
        p[1] = (ogpu_u8)((r & 0xF8) | (g >> 5)); p[0] = (ogpu_u8)(((g & 0x1C) << 3) | (b >> 3)); return;
    case OGPU_FMT_RGB555:
        hi = ((r & 0xF8) >> 1) | (g >> 6); lo = ((g & 0x38) << 2) | (b >> 3);
        p[0] = (ogpu_u8)hi; p[1] = (ogpu_u8)lo; return;
    case OGPU_FMT_RGB555PC:
        hi = ((r & 0xF8) >> 1) | (g >> 6); lo = ((g & 0x38) << 2) | (b >> 3);
        p[1] = (ogpu_u8)hi; p[0] = (ogpu_u8)lo; return;
    case OGPU_FMT_A8: p[0] = (ogpu_u8)a; return;
    }
    p[0] = (ogpu_u8)r;
}

/* A raw value of the target's format (FILL's colour, a pen, INVERT's mask)
 * as the bytes it is stored as. All four bytes are set (the ones a 1- or
 * 2-byte pixel doesn't use are 0), so a caller may read all four. */
static void raw_bytes(ogpu_u32 v, int f, ogpu_u8 *b) {
    ogpu_u8 w[4];
    bytes_of(v, w);
    b[1] = b[2] = b[3] = 0;
    switch (bytes_per_pixel(f)) {
    case 4: b[0] = f == OGPU_FMT_XRGB32 ? 0 : w[0]; b[1] = w[1]; b[2] = w[2]; b[3] = w[3]; return;
    case 2:
        if (f == OGPU_FMT_RGB565PC || f == OGPU_FMT_RGB555PC) { b[0] = w[3]; b[1] = w[2]; }
        else { b[0] = w[2]; b[1] = w[3]; }
        return;
    }
    b[0] = w[3];
}

/* What COMPLEMENT flips: every bit of a pen or a 16-bit pixel, and the
 * colour (not the alpha) of a 32-bit one. */
static void flip_bytes(int f, ogpu_u8 *b) {
    b[0] = b[1] = b[2] = b[3] = 0xFF;
    if (f == OGPU_FMT_ARGB32 || f == OGPU_FMT_XRGB32) b[0] = 0;
    if (f == OGPU_FMT_BGRA32) b[3] = 0;
}

/* x * y / 255, rounded, for x and y in 0..255. */
PX_FN unsigned mul255(unsigned x, unsigned y) {
    unsigned t = MUL16(x, y) + 128;
    return (t + (t >> 8)) >> 8;
}

static ogpu_u8 *px_at(const struct ogpu_surface *s, int x, int y) {
    return s->pixels + (long)y * s->bpr + (long)x * bytes_per_pixel(s->format);
}

/* The bytes h rows of `last` bytes take at bpr apart, in *len; 0 when that
 * passes 32 bits (as it can on the 68k), which no Amiga memory holds. */
static int span_len(ogpu_u32 bpr, int h, ogpu_u32 last, ogpu_u32 *len) {
    ogpu_u32 rows = (ogpu_u32)(h - 1);
    if (rows && bpr > (0xFFFFFFFFUL - last) / rows) return 0;
    *len = bpr * rows + last;
    return 1;
}

/* The drawable rectangle: the target, less the clip. */
static void bounds(const struct ogpu_core *c, int *x0, int *y0, int *x1, int *y1) {
    const struct ogpu_surface *t = &c->slot[c->target];
    *x0 = 0; *y0 = 0; *x1 = t->w; *y1 = t->h;
    if (c->cx1 > c->cx0) {
        if (c->cx0 > *x0) *x0 = c->cx0;
        if (c->cy0 > *y0) *y0 = c->cy0;
        if (c->cx1 < *x1) *x1 = c->cx1;
        if (c->cy1 < *y1) *y1 = c->cy1;
    }
}

/* Clip a rectangle on the target to the target and the clip rectangle.
 * Returns 0 when nothing is left; otherwise *ox, *oy say how far its corner
 * moved, so a source can move with it. */
static int clip_rect(const struct ogpu_core *c, int *x, int *y, int *w, int *h, int *ox, int *oy) {
    int x0, y0, x1, y1, nx, ny, nx1, ny1;
    bounds(c, &x0, &y0, &x1, &y1);
    nx = *x < x0 ? x0 : *x;
    ny = *y < y0 ? y0 : *y;
    nx1 = *x + *w > x1 ? x1 : *x + *w;
    ny1 = *y + *h > y1 ? y1 : *y + *h;
    if (nx1 <= nx || ny1 <= ny) return 0;
    *ox = nx - *x; *oy = ny - *y;
    *x = nx; *y = ny; *w = nx1 - nx; *h = ny1 - ny;
    return 1;
}

/* Copy one pixel's bytes. */
PX_FN void put_raw(ogpu_u8 *p, int bpp, const ogpu_u8 *v) {
    p[0] = v[0];
    if (bpp > 1) { p[1] = v[1]; if (bpp > 2) { p[2] = v[2]; p[3] = v[3]; } }
}
PX_FN void xor_raw(ogpu_u8 *p, int bpp, const ogpu_u8 *v) {
    p[0] ^= v[0];
    if (bpp > 1) { p[1] ^= v[1]; if (bpp > 2) { p[2] ^= v[2]; p[3] ^= v[3]; } }
}

/* v1.4: whole 32-bit words where a row allows it (fills and copies): the
 * same bytes as the byte loops, a quarter of the stores. The word is put
 * together in memory order, so it is right on either byte order. */
typedef unsigned int ogpu_w32;          /* 32 bits on the 68k and the hosts */
PX_FN ogpu_w32 word_of(const ogpu_u8 *b) {
    ogpu_w32 w;
    ogpu_u8 *p = (ogpu_u8 *)&w;
    p[0] = b[0]; p[1] = b[1]; p[2] = b[2]; p[3] = b[3];
    return w;
}
PX_FN int aligned4(const void *p) { return ((unsigned long)p & 3) == 0; }

/* n bytes of one row from sp to dp, front to back (no overlap that matters) */
PX_FN void copy_fwd(ogpu_u8 *dp, const ogpu_u8 *sp, int n) {
    if (aligned4(dp) && aligned4(sp)) {
        ogpu_w32 *q = (ogpu_w32 *)dp;
        const ogpu_w32 *r = (const ogpu_w32 *)sp;
        int k = n >> 2;
        n &= 3;
        while (k >= 4) { q[0] = r[0]; q[1] = r[1]; q[2] = r[2]; q[3] = r[3]; q += 4; r += 4; k -= 4; }
        while (k--) *q++ = *r++;
        dp = (ogpu_u8 *)q; sp = (const ogpu_u8 *)r;
    }
    while (n--) *dp++ = *sp++;
}

/* n words of wv from q */
PX_FN void fill_words(ogpu_w32 *q, ogpu_w32 wv, int n) {
    while (n >= 4) { q[0] = wv; q[1] = wv; q[2] = wv; q[3] = wv; q += 4; n -= 4; }
    while (n--) *q++ = wv;
}

/* The draw modes of a 1-bit source (TEMPLATE, PATTERN, LINE), worked out
 * once per command. */
struct pen { ogpu_u8 fg[4], bg[4], flip[4]; int bpp, mode; };

static void pen_init(struct pen *p, int format, ogpu_u32 fg, ogpu_u32 bg, int mode) {
    raw_bytes(fg, format, p->fg);
    raw_bytes(bg, format, p->bg);
    flip_bytes(format, p->flip);
    p->bpp = bytes_per_pixel(format);
    p->mode = mode;
}

PX_FN void plot_bit(ogpu_u8 *p, const struct pen *pn, int bit) {
    if (pn->mode & OGPU_INVERSVID) bit = !bit;
    if (pn->mode & OGPU_COMPLEMENT) {
        if (bit) xor_raw(p, pn->bpp, pn->flip);
    } else if (bit) {
        put_raw(p, pn->bpp, pn->fg);
    } else if (pn->mode & OGPU_JAM2) {
        put_raw(p, pn->bpp, pn->bg);
    }
}

/* ---- blending --------------------------------------------------------------------- */

/* v1.1's operators: put source s on dp with coverage ea, by OVER, ADD or IN
 * (COMPOSITE's flags). */
PX_FN void blend11(ogpu_u8 *dp, int f, const struct ch *s, unsigned ea, ogpu_u32 flags) {
    struct ch d;
    unsigned r, g, b, oa, ia;
    if (flags & OGPU_COMP_IN) {
        ld(dp, f, &d);
        r = mul255(d.r, ea); g = mul255(d.g, ea); b = mul255(d.b, ea); oa = mul255(d.a, ea);
    } else if (flags & OGPU_COMP_ADD) {
        if (!ea) return;
        ld(dp, f, &d);
        r = d.r + mul255(s->r, ea); g = d.g + mul255(s->g, ea); b = d.b + mul255(s->b, ea); oa = d.a + ea;
    } else {
        if (ea == 255) { st(dp, f, 255, s->r, s->g, s->b); return; }
        if (!ea) return;
        ld(dp, f, &d);
        ia = 255 - ea;
        r = mul255(s->r, ea) + mul255(d.r, ia);
        g = mul255(s->g, ea) + mul255(d.g, ia);
        b = mul255(s->b, ea) + mul255(d.b, ia);
        oa = ea + mul255(d.a, ia);
    }
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (oa > 255) oa = 255;
    st(dp, f, oa, r, g, b);
}

/* v1.2's blend modes (SDL's): source s with alpha sa on dp. */
PX_FN void blend12(ogpu_u8 *dp, int f, const struct ch *s, unsigned sa, int mode) {
    struct ch d;
    unsigned r, g, b, oa, ia;
    switch (mode) {
    case OGPU_BLEND_NONE:
        st(dp, f, sa, s->r, s->g, s->b);
        return;
    case OGPU_BLEND_ADD:
        if (!sa) return;
        ld(dp, f, &d);
        r = d.r + mul255(s->r, sa); g = d.g + mul255(s->g, sa); b = d.b + mul255(s->b, sa); oa = d.a;
        break;
    case OGPU_BLEND_MOD:
        ld(dp, f, &d);
        r = mul255(s->r, d.r); g = mul255(s->g, d.g); b = mul255(s->b, d.b); oa = d.a;
        break;
    case OGPU_BLEND_MUL:
        ld(dp, f, &d);
        ia = 255 - sa;
        r = mul255(s->r, d.r) + mul255(d.r, ia);
        g = mul255(s->g, d.g) + mul255(d.g, ia);
        b = mul255(s->b, d.b) + mul255(d.b, ia);
        oa = d.a;
        break;
    default:            /* OGPU_BLEND_BLEND */
        if (sa == 255) { st(dp, f, 255, s->r, s->g, s->b); return; }
        if (!sa) return;
        ld(dp, f, &d);
        ia = 255 - sa;
        r = mul255(s->r, sa) + mul255(d.r, ia);
        g = mul255(s->g, sa) + mul255(d.g, ia);
        b = mul255(s->b, sa) + mul255(d.b, ia);
        oa = sa + mul255(d.a, ia);
        break;
    }
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (oa > 255) oa = 255;
    st(dp, f, oa, r, g, b);
}

/* An ARGB32 colour word as channels. */
static void ch_of(ogpu_u32 v, struct ch *o) {
    ogpu_u8 b[4];
    bytes_of(v, b);
    o->a = b[0]; o->r = b[1]; o->g = b[2]; o->b = b[3];
}

/* ---- the commands ----------------------------------------------------------------- */

OP_FN op_surface(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 slot = rd32(a), address = rd32(a + 4), bpr = rd32(a + 8), wh = rd32(a + 12), format = rd32(a + 16);
    int w = hi_u(wh), h = lo_u(wh), bpp = bytes_per_pixel((int)format);
    struct ogpu_surface *s;
    ogpu_u8 *mem;
    ogpu_u32 len;
    if (slot >= OGPU_MAX_SLOTS) return OGPU_ERR_NOSURFACE;
    s = &c->slot[slot];
    s->pixels = 0;
    if (!bpp || format == OGPU_FMT_INDEX8 || !w || !h || w > OGPU_MAX_SIZE || h > OGPU_MAX_SIZE
        || bpr < (ogpu_u32)w * bpp || bpr > 0x01000000UL || !span_len(bpr, h, (ogpu_u32)w * bpp, &len))
        return OGPU_ERR_UNSUPPORTED;
    mem = c->map(c->user, address, len);
    if (!mem) return OGPU_ERR_NOMAP;
    s->pixels = mem; s->bpr = (long)bpr; s->w = w; s->h = h; s->format = (int)format;
    if (c->target == (int)slot) { c->cx0 = c->cy0 = c->cx1 = c->cy1 = 0; }
    return OGPU_OK;
}

OP_FN op_fill(struct ogpu_core *c, const ogpu_u8 *a, int invert) {
    ogpu_u32 xy = rd32(a), wh = rd32(a + 4), v = rd32(a + 8);
    int x = hi_s(xy), y = lo_s(xy), w = hi_u(wh), h = lo_u(wh), ox, oy, i, j;
    const struct ogpu_surface *t = &c->slot[c->target];
    int bpp = bytes_per_pixel(t->format);
    ogpu_u8 pv[4], m[4];
    ogpu_u8 *row;
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    raw_bytes(v, t->format, pv);
    if (invert) {
        flip_bytes(t->format, m);
        if (bpp == 4 && t->format != OGPU_FMT_XRGB32)       /* INVERT's mask reaches the alpha too, */
            m[0] = m[1] = m[2] = m[3] = 0xFF;               /* but XRGB32's X stays 0 */
        for (i = 0; i < 4; i++) pv[i] &= m[i];
    }
    row = px_at(t, x, y);
    for (j = 0; j < h; j++, row += t->bpr) {
        ogpu_u8 *p = row;
        if (invert) {
            if (bpp == 4) { ogpu_u8 b1 = pv[1], b2 = pv[2], b3 = pv[3], b0 = pv[0];
                for (i = 0; i < w; i++, p += 4) { p[0] ^= b0; p[1] ^= b1; p[2] ^= b2; p[3] ^= b3; } }
            else if (bpp == 2) { ogpu_u8 b0 = pv[0], b1 = pv[1];
                for (i = 0; i < w; i++, p += 2) { p[0] ^= b0; p[1] ^= b1; } }
            else { ogpu_u8 b0 = pv[0]; for (i = 0; i < w; i++) p[i] ^= b0; }
        } else if (bpp == 4) {
            ogpu_u8 b0 = pv[0], b1 = pv[1], b2 = pv[2], b3 = pv[3];
            if (aligned4(p)) {
                fill_words((ogpu_w32 *)p, word_of(pv), w);
            } else
                for (i = 0; i < w; i++, p += 4) { p[0] = b0; p[1] = b1; p[2] = b2; p[3] = b3; }
        } else if (bpp == 2) {
            ogpu_u8 b0 = pv[0], b1 = pv[1];
            i = 0;
            if (((unsigned long)p & 1) == 0) {
                if (!aligned4(p) && w) { p[0] = b0; p[1] = b1; p += 2; i = 1; }
                if (i + 2 <= w) {
                    ogpu_u8 q4[4];
                    int k = (w - i) >> 1;
                    q4[0] = b0; q4[1] = b1; q4[2] = b0; q4[3] = b1;
                    fill_words((ogpu_w32 *)p, word_of(q4), k);
                    i += k << 1; p += k << 2;
                }
            }
            for (; i < w; i++, p += 2) { p[0] = b0; p[1] = b1; }
        } else {
            ogpu_u8 b0 = pv[0];
            for (i = 0; i < w; i++) p[i] = b0;
        }
    }
    return OGPU_OK;
}

OP_FN op_copy(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 slot = rd32(a), sxy = rd32(a + 4), dxy = rd32(a + 8), wh = rd32(a + 12);
    const struct ogpu_surface *t = &c->slot[c->target], *s;
    int sx = hi_s(sxy), sy = lo_s(sxy), x = hi_s(dxy), y = lo_s(dxy), w = hi_u(wh), h = lo_u(wh);
    int ox, oy, bpp, row, j, n;
    if (slot >= OGPU_MAX_SLOTS || !c->slot[slot].pixels) return OGPU_ERR_NOSURFACE;
    s = &c->slot[slot];
    if (s->format != t->format) return OGPU_ERR_UNSUPPORTED;
    /* Keep the source inside its surface, moving the destination with it. */
    if (sx < 0) { x -= sx; w += sx; sx = 0; }
    if (sy < 0) { y -= sy; h += sy; sy = 0; }
    if (sx + w > s->w) w = s->w - sx;
    if (sy + h > s->h) h = s->h - sy;
    if (w <= 0 || h <= 0) return OGPU_OK;
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    sx += ox; sy += oy;
    bpp = bytes_per_pixel(t->format);
    n = w * bpp;
    for (row = 0; row < h; row++) {
        /* Overlap (one surface, or two slots on the same memory): rows bottom
         * up when the destination is later in memory, bytes backwards when
         * a row's destination starts inside its source. */
        int r = px_at(t, x, y) > px_at(s, sx, sy) ? h - 1 - row : row;
        const ogpu_u8 *sp = px_at(s, sx, sy + r);
        ogpu_u8 *dp = px_at(t, x, y + r);
        if (dp > sp && dp < sp + n) { for (j = n - 1; j >= 0; j--) dp[j] = sp[j]; }
        else copy_fwd(dp, sp, n);
    }
    return OGPU_OK;
}

OP_FN op_template(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 address = rd32(a), bpr = rd32(a + 4), first = rd32(a + 8), xy = rd32(a + 12), wh = rd32(a + 16);
    ogpu_u32 fg = rd32(a + 20), bg = rd32(a + 24);
    int mode = (int)rd32(a + 28);
    const struct ogpu_surface *t = &c->slot[c->target];
    int x = hi_s(xy), y = lo_s(xy), w = hi_u(wh), h = lo_u(wh), ox, oy, i, j;
    const ogpu_u8 *src;
    ogpu_u8 *drow;
    struct pen pn;
    if (!w || !h) return OGPU_OK;
    if (first > 0xFFFFUL || bpr > 0xFFFFUL) return OGPU_ERR_UNSUPPORTED;
    src = c->map(c->user, address, bpr * (ogpu_u32)(h - 1) + (first + (ogpu_u32)w + 7) / 8);
    if (!src) return OGPU_ERR_NOMAP;
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    pen_init(&pn, t->format, fg, bg, mode);
    src += (long)oy * (long)bpr;
    drow = px_at(t, x, y);
    for (j = 0; j < h; j++, src += bpr, drow += t->bpr) {
        ogpu_u32 bit = first + (ogpu_u32)ox;
        const ogpu_u8 *sb = src + (bit >> 3);
        unsigned m = 0x80u >> (bit & 7), byte = *sb;    /* once a row */
        ogpu_u8 *p = drow;
        for (i = 0; i < w; i++, p += pn.bpp) {
            plot_bit(p, &pn, (byte & m) != 0);
            if (!(m >>= 1) && i + 1 < w) { m = 0x80; byte = *++sb; }
        }
    }
    return OGPU_OK;
}

OP_FN op_pattern(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 address = rd32(a), rows = rd32(a + 4), xy = rd32(a + 8), wh = rd32(a + 12);
    ogpu_u32 fg = rd32(a + 16), bg = rd32(a + 20);
    int mode = (int)rd32(a + 24);
    const struct ogpu_surface *t = &c->slot[c->target];
    int x = hi_s(xy), y = lo_s(xy), w = hi_u(wh), h = lo_u(wh), ox, oy, i, j;
    const ogpu_u8 *pat;
    ogpu_u8 *drow;
    struct pen pn;
    if (!rows || rows > 256 || (rows & (rows - 1))) return OGPU_ERR_UNSUPPORTED;
    pat = c->map(c->user, address, rows * 2);
    if (!pat) return OGPU_ERR_NOMAP;
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    pen_init(&pn, t->format, fg, bg, mode);
    drow = px_at(t, x, y);
    for (j = 0; j < h; j++, drow += t->bpr) {
        const ogpu_u8 *pw = pat + (((ogpu_u32)(y + j) & (rows - 1)) << 1);
        unsigned bits = ((unsigned)pw[0] << 8) | pw[1];
        unsigned m = 0x8000u >> (x & 15);               /* once a row */
        ogpu_u8 *p = drow;
        for (i = 0; i < w; i++, p += pn.bpp) {
            plot_bit(p, &pn, (bits & m) != 0);
            if (!(m >>= 1)) m = 0x8000u;
        }
    }
    return OGPU_OK;
}

/* A Bresenham line from (x0, y0) to (x1, y1), each pixel inside the
 * drawable rectangle given to fn; with skip_last the last is left out. */
typedef void (*plot_fn)(ogpu_u8 *p, const void *arg);

static void walk_line(const struct ogpu_core *c, int x0, int y0, int x1, int y1, int skip_last, plot_fn fn, const void *arg) {
    const struct ogpu_surface *t = &c->slot[c->target];
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int stepx = x0 < x1 ? 1 : -1, stepy = y0 < y1 ? 1 : -1, err = dx - dy;
    int bx0, by0, bx1, by1, bpp = bytes_per_pixel(t->format), inside;
    long px_step = stepx > 0 ? bpp : -bpp, row_step = stepy > 0 ? t->bpr : -t->bpr;
    ogpu_u8 *p = 0;
    bounds(c, &bx0, &by0, &bx1, &by1);
    inside = x0 >= bx0 && y0 >= by0 && x0 < bx1 && y0 < by1;
    if (inside) p = px_at(t, x0, y0);
    for (;;) {
        int last = x0 == x1 && y0 == y1;
        if (inside && !(last && skip_last)) fn(p, arg);
        if (last) break;
        {
            int e2 = 2 * err, mx = 0, my = 0;
            if (e2 > -dy) { err -= dy; x0 += stepx; mx = 1; }
            if (e2 < dx) { err += dx; y0 += stepy; my = 1; }
            if (x0 >= bx0 && y0 >= by0 && x0 < bx1 && y0 < by1) {
                if (inside) { if (mx) p += px_step; if (my) p += row_step; }
                else p = px_at(t, x0, y0);
                inside = 1;
            } else inside = 0;
        }
    }
}

static void plot_pen(ogpu_u8 *p, const void *arg) { plot_bit(p, (const struct pen *)arg, 1); }

OP_FN op_line(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 p0 = rd32(a), p1 = rd32(a + 4), fg = rd32(a + 8);
    int mode = (int)rd32(a + 12) & ~OGPU_INVERSVID;
    struct pen pn;
    pen_init(&pn, c->slot[c->target].format, fg, 0, mode);
    walk_line(c, hi_s(p0), lo_s(p0), hi_s(p1), lo_s(p1), 0, plot_pen, &pn);
    return OGPU_OK;
}

OP_FN op_pixels(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 address = rd32(a), bpr = rd32(a + 4), format = rd32(a + 8), table = rd32(a + 12);
    ogpu_u32 xy = rd32(a + 16), wh = rd32(a + 20);
    const struct ogpu_surface *t = &c->slot[c->target];
    int x = hi_s(xy), y = lo_s(xy), w = hi_u(wh), h = lo_u(wh), ox, oy, i, j;
    int sbpp = bytes_per_pixel((int)format), dbpp = bytes_per_pixel(t->format);
    const ogpu_u8 *src, *ctab = 0;
    ogpu_u8 *drow;
    ogpu_u32 len;
    int raw;
    if (!sbpp || !w || !h) return sbpp ? OGPU_OK : OGPU_ERR_UNSUPPORTED;
    /* A CLUT8 target takes pens only; true-colour targets take anything
     * but bare pens, which have no colours without a table. */
    if (t->format == OGPU_FMT_CLUT8) {
        if (format != OGPU_FMT_CLUT8 && !(format == OGPU_FMT_INDEX8 && !table)) return OGPU_ERR_UNSUPPORTED;
        raw = 1;
    } else {
        if (format == OGPU_FMT_CLUT8) return OGPU_ERR_UNSUPPORTED;
        /* A8 goes only to A8: coverage has no colour, and colours no coverage. */
        if ((format == OGPU_FMT_A8) != (t->format == OGPU_FMT_A8)) return OGPU_ERR_UNSUPPORTED;
        raw = (int)format == t->format && format != OGPU_FMT_XRGB32;
        if (format == OGPU_FMT_INDEX8) {
            if (!table) return OGPU_ERR_UNSUPPORTED;
            ctab = c->map(c->user, table, 1024);
            if (!ctab) return OGPU_ERR_NOMAP;
        }
    }
    if (bpr < (ogpu_u32)w * sbpp || bpr > 0x01000000UL || !span_len(bpr, h, (ogpu_u32)w * sbpp, &len))
        return OGPU_ERR_UNSUPPORTED;
    src = c->map(c->user, address, len);
    if (!src) return OGPU_ERR_NOMAP;
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    src += (long)oy * (long)bpr + (long)ox * sbpp;
    drow = px_at(t, x, y);
    for (j = 0; j < h; j++, src += bpr, drow += t->bpr) {
        const ogpu_u8 *sp = src;
        ogpu_u8 *dp = drow;
        if (raw) {
            int n = w * dbpp;
            for (i = 0; i < n; i++) dp[i] = sp[i];
            continue;
        }
        if (t->format == OGPU_FMT_XRGB32 && (format == OGPU_FMT_ARGB32 || format == OGPU_FMT_XRGB32)) {
            /* The colour as it is, X 0: a 32-bit GFOpenRTG screen's ARGB pixels. */
            for (i = 0; i < w; i++, sp += 4, dp += 4) { dp[0] = 0; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3]; }
            continue;
        }
        for (i = 0; i < w; i++, sp += sbpp, dp += dbpp) {
            struct ch s;
            if (ctab) { const ogpu_u8 *e = ctab + ((unsigned)sp[0] << 2); s.a = e[0]; s.r = e[1]; s.g = e[2]; s.b = e[3]; }
            else ld(sp, (int)format, &s);
            st(dp, t->format, s.a, s.r, s.g, s.b);
        }
    }
    return OGPU_OK;
}

/* One channel of a and b mixed by f/256. */
PX_FN unsigned mix1(unsigned a, unsigned b, unsigned f) {
    return (MUL16(a, 256 - f) + MUL16(b, f)) >> 8;
}
PX_FN void mix(struct ch *o, const struct ch *a, const struct ch *b, unsigned f) {
    o->a = mix1(a->a, b->a, f); o->r = mix1(a->r, b->r, f); o->g = mix1(a->g, b->g, f); o->b = mix1(a->b, b->b, f);
}

/* x * bytes a pixel, by shifts. */
/* Row y of a surface, with a 16-bit multiply when the row fits in one
 * (every Amiga screen's does). */
PX_FN ogpu_u8 *row_at(const struct ogpu_surface *s, int y) {
    if (s->bpr <= 0xFFFF) return s->pixels + MUL16(y, s->bpr);
    return s->pixels + (long)y * s->bpr;
}

PX_FN long xoff(int x, int bpp) { return bpp == 4 ? (long)x << 2 : bpp == 2 ? (long)x << 1 : (long)x; }

/* A source's pixel (x, y) of a rectangle whose row pointers are known. */
PX_FN void sample_at(const struct ogpu_surface *s, const ogpu_u8 *row, int x, struct ch *o) {
    ld(row + xoff(x, fmt_bpp[s->format]), s->format, o);
}

/* Bilinear between rows r0 and r1 at x0..x1 with weights wx, wy (0-255). */
PX_FN void sample_bi(const struct ogpu_surface *s, const ogpu_u8 *r0, const ogpu_u8 *r1, int x0, int x1,
                      unsigned wx, unsigned wy, struct ch *o) {
    struct ch p00, p10, p01, p11, t0, t1;
    sample_at(s, r0, x0, &p00); sample_at(s, r0, x1, &p10);
    sample_at(s, r1, x0, &p01); sample_at(s, r1, x1, &p11);
    mix(&t0, &p00, &p10, wx);
    mix(&t1, &p01, &p11, wx);
    mix(o, &t0, &t1, wy);
}

OP_FN op_composite(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 slot = rd32(a), sxy = rd32(a + 4), swh = rd32(a + 8), dxy = rd32(a + 12), dwh = rd32(a + 16);
    ogpu_u32 alpha = rd32(a + 20) & 255, flags = rd32(a + 24);
    const struct ogpu_surface *t = &c->slot[c->target], *s, *m = 0;
    int sx = hi_s(sxy), sy = lo_s(sxy), sw = hi_u(swh), sh = lo_u(swh);
    int x = hi_s(dxy), y = lo_s(dxy), w = hi_u(dwh), h = lo_u(dwh), ox, oy, i, j, dbpp, mx = 0, my = 0;
    int mode = flags & OGPU_COMP_BLENDMODE ? OGPU_COMP_MODE_OF(flags) : -1, fast, srcalpha;
    ogpu_u32 stepx, stepy;
    ogpu_u8 *drow;
    if (slot >= OGPU_MAX_SLOTS || !c->slot[slot].pixels) return OGPU_ERR_NOSURFACE;
    s = &c->slot[slot];
    if (!is_colour(t->format) && t->format != OGPU_FMT_A8) return OGPU_ERR_UNSUPPORTED;
    if (!is_colour(s->format) && s->format != OGPU_FMT_A8) return OGPU_ERR_UNSUPPORTED;
    if ((flags & OGPU_COMP_ADD) && (flags & OGPU_COMP_IN)) return OGPU_ERR_UNSUPPORTED;
    if (mode > OGPU_BLEND_MUL) return OGPU_ERR_UNSUPPORTED;
    if (flags & OGPU_COMP_MASK) {
        ogpu_u32 mslot = rd32(a + 28), mxy = rd32(a + 32);
        if (mslot >= OGPU_MAX_SLOTS || !c->slot[mslot].pixels) return OGPU_ERR_NOSURFACE;
        m = &c->slot[mslot];
        if (m->format != OGPU_FMT_A8) return OGPU_ERR_UNSUPPORTED;
        mx = hi_s(mxy); my = lo_s(mxy);
        if (mx < 0 || my < 0 || mx + w > m->w || my + h > m->h) return OGPU_ERR_UNSUPPORTED;
    }
    if (!sw || !sh || !w || !h) return OGPU_OK;
    if (sx < 0 || sy < 0 || sx + sw > s->w || sy + sh > s->h) return OGPU_ERR_UNSUPPORTED;
    stepx = ((ogpu_u32)sw << 16) / (ogpu_u32)w;         /* once a command */
    stepy = ((ogpu_u32)sh << 16) / (ogpu_u32)h;
    dbpp = bytes_per_pixel(t->format);
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    fast = s->format == OGPU_FMT_ARGB32 && (t->format == OGPU_FMT_ARGB32 || t->format == OGPU_FMT_RGB565)
           && mode < 0 && !m && !(flags & (OGPU_COMP_ADD | OGPU_COMP_IN | OGPU_COMP_BILINEAR));
    srcalpha = (flags & OGPU_COMP_SRCALPHA) != 0;
    drow = px_at(t, x, y);
    for (j = 0; j < h; j++, drow += t->bpr) {
        ogpu_u32 py = (ogpu_u32)(oy + j) * stepy + (stepy >> 1);    /* 16.16, the pixel's centre */
        ogpu_u32 px = (ogpu_u32)ox * stepx + (stepx >> 1);
        ogpu_u8 *dp = drow;
        const ogpu_u8 *mp = m ? px_at(m, mx + ox, my + oy + j) : 0;
        const ogpu_u8 *r0, *r1;
        int y0, y1;
        unsigned wy = 0;
        if (flags & OGPU_COMP_BILINEAR) {
            long fy = (long)py - 0x8000L;
            if (fy < 0) fy = 0;
            y0 = (int)(fy >> 16); wy = (unsigned)(fy >> 8) & 255;
            if (y0 >= sh - 1) { y0 = sh - 1; wy = 0; }
            y1 = y0 + 1 < sh ? y0 + 1 : y0;
        } else {
            y0 = (int)(py >> 16);
            if (y0 >= sh) y0 = sh - 1;
            y1 = y0;
        }
        r0 = px_at(s, 0, sy + y0);                      /* once a row */
        r1 = y1 == y0 ? r0 : r0 + s->bpr;
        if (fast) {
            /* ARGB32 over ARGB32 or RGB565, nearest, OVER: the common case, without the
             * general reads and writes. */
            const ogpu_u8 *srow = r0 + xoff(sx, 4);
            for (i = 0; i < w; i++, dp += dbpp, px += stepx) {
                int nx = (int)(px >> 16);
                const ogpu_u8 *q;
                unsigned ea, ia, r, g, b;
                if (nx >= sw) nx = sw - 1;
                q = srow + xoff(nx, 4);
                ea = mul255(srcalpha ? q[0] : 255, alpha);
                if (ea == 255) { r = q[1]; g = q[2]; b = q[3]; if (dbpp == 4) dp[0] = 255; }
                else if (!ea) continue;
                else {
                    ia = 255 - ea;
                    if (dbpp == 4) {
                        r = mul255(q[1], ea) + mul255(dp[1], ia);
                        g = mul255(q[2], ea) + mul255(dp[2], ia);
                        b = mul255(q[3], ea) + mul255(dp[3], ia);
                        { unsigned oa = ea + mul255(dp[0], ia); dp[0] = (ogpu_u8)(oa > 255 ? 255 : oa); }
                    } else {
                        struct ch d;
                        ld(dp, OGPU_FMT_RGB565, &d);
                        r = mul255(q[1], ea) + mul255(d.r, ia);
                        g = mul255(q[2], ea) + mul255(d.g, ia);
                        b = mul255(q[3], ea) + mul255(d.b, ia);
                    }
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                }
                if (dbpp == 4) { dp[1] = (ogpu_u8)r; dp[2] = (ogpu_u8)g; dp[3] = (ogpu_u8)b; }
                else { dp[0] = (ogpu_u8)((r & 0xF8) | (g >> 5)); dp[1] = (ogpu_u8)(((g & 0x1C) << 3) | (b >> 3)); }
            }
            continue;
        }
        for (i = 0; i < w; i++, dp += dbpp, px += stepx) {
            struct ch sp;
            unsigned ea;
            if (flags & OGPU_COMP_BILINEAR) {
                long fx = (long)px - 0x8000L;
                int x0, x1;
                unsigned wx;
                if (fx < 0) fx = 0;
                x0 = (int)(fx >> 16); wx = (unsigned)(fx >> 8) & 255;
                if (x0 >= sw - 1) { x0 = sw - 1; wx = 0; }
                x1 = x0 + 1 < sw ? x0 + 1 : x0;
                sample_bi(s, r0, r1, sx + x0, sx + x1, wx, wy, &sp);
            } else {
                int nx = (int)(px >> 16);
                if (nx >= sw) nx = sw - 1;
                sample_at(s, r0, sx + nx, &sp);
            }
            ea = mul255((flags & OGPU_COMP_SRCALPHA) ? sp.a : 255, alpha);
            if (mp) ea = mul255(ea, mp[i]);
            if (mode >= 0) blend12(dp, t->format, &sp, ea, mode);
            else blend11(dp, t->format, &sp, ea, flags);
        }
    }
    return OGPU_OK;
}

/* MASK (v1.1): one ARGB32 colour drawn OVER the target through an A8 mask
 * in memory, as anti-aliased glyphs are. */
OP_FN op_mask(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 address = rd32(a), bpr = rd32(a + 4), xy = rd32(a + 8), wh = rd32(a + 12), colour = rd32(a + 16);
    const struct ogpu_surface *t = &c->slot[c->target];
    int x = hi_s(xy), y = lo_s(xy), w = hi_u(wh), h = lo_u(wh), ox, oy, i, j, dbpp = bytes_per_pixel(t->format);
    ogpu_u32 len;
    const ogpu_u8 *src;
    ogpu_u8 *drow;
    struct ch col;
    if (t->format == OGPU_FMT_CLUT8) return OGPU_ERR_UNSUPPORTED;
    if (!w || !h) return OGPU_OK;
    if (bpr < (ogpu_u32)w || bpr > 0x01000000UL || !span_len(bpr, h, (ogpu_u32)w, &len)) return OGPU_ERR_UNSUPPORTED;
    src = c->map(c->user, address, len);
    if (!src) return OGPU_ERR_NOMAP;
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    ch_of(colour, &col);
    src += (long)oy * (long)bpr + ox;
    drow = px_at(t, x, y);
    for (j = 0; j < h; j++, src += bpr, drow += t->bpr) {
        ogpu_u8 *dp = drow;
        for (i = 0; i < w; i++, dp += dbpp)
            blend11(dp, t->format, &col, mul255(src[i], col.a), 0);
    }
    return OGPU_OK;
}

/* ---- v1.2: SDL 2's renderer ------------------------------------------------------- */

/* FILL_BLEND: an ARGB32 colour over a rectangle by a blend mode. */
OP_FN op_fill_blend(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 xy = rd32(a), wh = rd32(a + 4), colour = rd32(a + 8);
    int mode = (int)(rd32(a + 12) & OGPU_BLEND_MASK);
    const struct ogpu_surface *t = &c->slot[c->target];
    int x = hi_s(xy), y = lo_s(xy), w = hi_u(wh), h = lo_u(wh), ox, oy, i, j, bpp = bytes_per_pixel(t->format);
    struct ch col;
    ogpu_u8 *drow;
    if (!is_colour(t->format) || mode > OGPU_BLEND_MUL) return OGPU_ERR_UNSUPPORTED;
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    ch_of(colour, &col);
    drow = px_at(t, x, y);
    for (j = 0; j < h; j++, drow += t->bpr) {
        ogpu_u8 *p = drow;
        for (i = 0; i < w; i++, p += bpp) blend12(p, t->format, &col, col.a, mode);
    }
    return OGPU_OK;
}

struct blend_arg { struct ch col; int format, mode; };
static void plot_blend(ogpu_u8 *p, const void *arg) {
    const struct blend_arg *b = arg;
    blend12(p, b->format, &b->col, b->col.a, b->mode);
}

/* LINES_BLEND: lines through points in memory, by a blend mode. */
OP_FN op_lines_blend(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 address = rd32(a), count = rd32(a + 4), colour = rd32(a + 8), flags = rd32(a + 12);
    struct blend_arg b;
    const ogpu_u8 *pts;
    ogpu_u32 i, step = flags & OGPU_LINES_STRIP ? 1 : 2;
    b.format = c->slot[c->target].format;
    b.mode = (int)(flags & OGPU_BLEND_MASK);
    if (!is_colour(b.format) || b.mode > OGPU_BLEND_MUL) return OGPU_ERR_UNSUPPORTED;
    if (count < 2) return OGPU_OK;
    if (count > 0x00FFFFFFUL) return OGPU_ERR_UNSUPPORTED;
    if (!(pts = c->map(c->user, address, count * 4))) return OGPU_ERR_NOMAP;
    ch_of(colour, &b.col);
    for (i = 0; i + 1 < count; i += step) {
        ogpu_u32 p0 = rd32(pts + i * 4), p1 = rd32(pts + i * 4 + 4);
        walk_line(c, hi_s(p0), lo_s(p0), hi_s(p1), lo_s(p1), !(flags & OGPU_LINES_LAST), plot_blend, &b);
    }
    return OGPU_OK;
}

/* POINTS_BLEND: single pixels at points in memory. */
OP_FN op_points_blend(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 address = rd32(a), count = rd32(a + 4), colour = rd32(a + 8);
    int mode = (int)(rd32(a + 12) & OGPU_BLEND_MASK);
    const struct ogpu_surface *t = &c->slot[c->target];
    const ogpu_u8 *pts;
    struct ch col;
    int x0, y0, x1, y1;
    ogpu_u32 i;
    if (!is_colour(t->format) || mode > OGPU_BLEND_MUL) return OGPU_ERR_UNSUPPORTED;
    if (!count) return OGPU_OK;
    if (count > 0x00FFFFFFUL) return OGPU_ERR_UNSUPPORTED;
    if (!(pts = c->map(c->user, address, count * 4))) return OGPU_ERR_NOMAP;
    ch_of(colour, &col);
    bounds(c, &x0, &y0, &x1, &y1);
    for (i = 0; i < count; i++) {
        ogpu_u32 p = rd32(pts + i * 4);
        int x = hi_s(p), y = lo_s(p);
        if (x >= x0 && y >= y0 && x < x1 && y < y1) blend12(px_at(t, x, y), t->format, &col, col.a, mode);
    }
    return OGPU_OK;
}

/* 16.16 helpers for COMPOSITE_AFFINE, once a command or a row: 64-bit
 * products, as long long (libgcc's on the 68k, outside the pixel loop). */
typedef long long ogpu_s64;

OP_FN op_composite_affine(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 slot = rd32(a), sxy = rd32(a + 4), swh = rd32(a + 8), colour = rd32(a + 36), flags = rd32(a + 40);
    long m00 = (long)rd32(a + 12), m01 = (long)rd32(a + 16), m02 = (long)rd32(a + 20);
    long m10 = (long)rd32(a + 24), m11 = (long)rd32(a + 28), m12 = (long)rd32(a + 32);
    const struct ogpu_surface *t = &c->slot[c->target], *s;
    int sx = hi_s(sxy), sy = lo_s(sxy), sw = hi_u(swh), sh = lo_u(swh), mode = (int)(flags & OGPU_BLEND_MASK);
    int bx0, by0, bx1, by1, x, y, k, dbpp = bytes_per_pixel(t->format), sbpp;
    ogpu_s64 det, i00, i01, i10, i11;
    long minx = 0x7FFFFFFFL, miny = 0x7FFFFFFFL, maxx = -0x7FFFFFFFL, maxy = -0x7FFFFFFFL;
    long usz, vsz;
    struct ch col;
    if (slot >= OGPU_MAX_SLOTS || !c->slot[slot].pixels) return OGPU_ERR_NOSURFACE;
    s = &c->slot[slot];
    if (!is_colour(t->format) || !is_colour(s->format) || mode > OGPU_BLEND_MUL) return OGPU_ERR_UNSUPPORTED;
    /* 32-bit words as signed, on any host */
    if (m00 > 0x7FFFFFFFL) m00 -= 0x100000000LL;
    if (m01 > 0x7FFFFFFFL) m01 -= 0x100000000LL;
    if (m02 > 0x7FFFFFFFL) m02 -= 0x100000000LL;
    if (m10 > 0x7FFFFFFFL) m10 -= 0x100000000LL;
    if (m11 > 0x7FFFFFFFL) m11 -= 0x100000000LL;
    if (m12 > 0x7FFFFFFFL) m12 -= 0x100000000LL;
    if (!sw || !sh) return OGPU_OK;
    if (sx < 0 || sy < 0 || sx + sw > s->w || sy + sh > s->h) return OGPU_ERR_UNSUPPORTED;
    if (m00 >= 0x800000L || m00 <= -0x800000L || m01 >= 0x800000L || m01 <= -0x800000L
        || m10 >= 0x800000L || m10 <= -0x800000L || m11 >= 0x800000L || m11 <= -0x800000L)
        return OGPU_ERR_UNSUPPORTED;                /* scale beyond 127 */
    det = (ogpu_s64)m00 * m11 - (ogpu_s64)m01 * m10;   /* 32.32 */
    if (det == 0) return OGPU_OK;                   /* flat: nothing to draw */
    /* The inverse, 16.16: target offsets to source offsets. */
    i00 = (ogpu_s64)m11 * 4294967296LL / det; i01 = -((ogpu_s64)m01 * 4294967296LL) / det;
    i10 = -((ogpu_s64)m10 * 4294967296LL) / det; i11 = (ogpu_s64)m00 * 4294967296LL / det;
    if (i00 >= 0x40000000LL || i00 <= -0x40000000LL || i01 >= 0x40000000LL || i01 <= -0x40000000LL
        || i10 >= 0x40000000LL || i10 <= -0x40000000LL || i11 >= 0x40000000LL || i11 <= -0x40000000LL)
        return OGPU_ERR_UNSUPPORTED;                /* shrunk below 1/16384 */
    /* The source's corners on the target bound what can be drawn. */
    for (k = 0; k < 4; k++) {
        long u = k & 1 ? (long)sw << 16 : 0, v = k & 2 ? (long)sh << 16 : 0;
        long tx = (long)(((ogpu_s64)m00 * u + (ogpu_s64)m01 * v) / 65536) + m02;
        long ty = (long)(((ogpu_s64)m10 * u + (ogpu_s64)m11 * v) / 65536) + m12;
        if (tx < minx) minx = tx;
        if (tx > maxx) maxx = tx;
        if (ty < miny) miny = ty;
        if (ty > maxy) maxy = ty;
    }
    bounds(c, &bx0, &by0, &bx1, &by1);
    if ((minx >> 16) > bx0) bx0 = (int)(minx >> 16);
    if ((miny >> 16) > by0) by0 = (int)(miny >> 16);
    if ((maxx >> 16) + 1 < bx1) bx1 = (int)(maxx >> 16) + 1;
    if ((maxy >> 16) + 1 < by1) by1 = (int)(maxy >> 16) + 1;
    if (bx1 <= bx0 || by1 <= by0) return OGPU_OK;
    ch_of(colour, &col);
    sbpp = bytes_per_pixel(s->format);
    usz = (long)sw << 16; vsz = (long)sh << 16;
    for (y = by0; y < by1; y++) {
        /* The first pixel's centre, in source offsets, exactly; then steps. */
        ogpu_s64 dx = ((ogpu_s64)bx0 << 16) + 0x8000 - m02, dy = ((ogpu_s64)y << 16) + 0x8000 - m12;
        long u = (long)((i00 * dx + i01 * dy) >> 16), v = (long)((i10 * dx + i11 * dy) >> 16);
        long du = (long)i00, dv = (long)i10;
        ogpu_u8 *dp = px_at(t, bx0, y);
        for (x = bx0; x < bx1; x++, dp += dbpp, u += du, v += dv) {
            struct ch sp;
            unsigned sa;
            if (u < 0 || v < 0 || u >= usz || v >= vsz) continue;
            if (flags & OGPU_AFF_BILINEAR) {
                long fu = u - 0x8000L, fv = v - 0x8000L;
                int x0, y0, x1, y1;
                unsigned wx, wy;
                const ogpu_u8 *r0;
                if (fu < 0) fu = 0;
                if (fv < 0) fv = 0;
                x0 = (int)(fu >> 16); wx = (unsigned)(fu >> 8) & 255;
                y0 = (int)(fv >> 16); wy = (unsigned)(fv >> 8) & 255;
                if (x0 >= sw - 1) { x0 = sw - 1; wx = 0; }
                if (y0 >= sh - 1) { y0 = sh - 1; wy = 0; }
                x1 = x0 + 1 < sw ? x0 + 1 : x0;
                y1 = y0 + 1 < sh ? y0 + 1 : y0;
                r0 = row_at(s, sy + y0);
                sample_bi(s, r0, y1 == y0 ? r0 : r0 + s->bpr, sx + x0, sx + x1, wx, wy, &sp);
            } else {
                const ogpu_u8 *r0 = row_at(s, sy + (int)(v >> 16));
                ld(r0 + xoff(sx + (int)(u >> 16), sbpp), s->format, &sp);
            }
            sa = (flags & OGPU_AFF_SRCALPHA) ? mul255(sp.a, col.a) : col.a;
            if (col.r != 255) sp.r = mul255(sp.r, col.r);
            if (col.g != 255) sp.g = mul255(sp.g, col.g);
            if (col.b != 255) sp.b = mul255(sp.b, col.b);
            blend12(dp, t->format, &sp, sa, mode);
        }
    }
    return OGPU_OK;
}

static unsigned clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : (unsigned)v; }

/* YUV: a picture in one of the video layouts, converted 1:1 into the
 * target. Integer BT.601, BT.709 or full-range (JPEG) coefficients, 8.8. */
OP_FN op_yuv(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 ya = rd32(a), ybpr = rd32(a + 4), ua = rd32(a + 8), ubpr = rd32(a + 12), va = rd32(a + 16), vbpr = rd32(a + 20);
    ogpu_u32 wh = rd32(a + 24), xy = rd32(a + 28), fmt = rd32(a + 32);
    const struct ogpu_surface *t = &c->slot[c->target];
    int layout = (int)(fmt & 255), space = (int)((fmt >> 8) & 255);
    int w = hi_u(wh), h = lo_u(wh), x = hi_s(xy), y = lo_s(xy), ox, oy, i, j, dbpp = bytes_per_pixel(t->format);
    int ky, kr, kgu, kgv, kb, yoff;
    const ogpu_u8 *yp, *up = 0, *vp = 0;
    ogpu_u32 len;
    ogpu_u8 *drow;
    int packed = layout >= OGPU_YUV_YUY2 && layout <= OGPU_YUV_YVYU, semi = layout == OGPU_YUV_NV12 || layout == OGPU_YUV_NV21;
    if (!is_colour(t->format) || layout < OGPU_YUV_I420 || layout > OGPU_YUV_YVYU || space > OGPU_YUV_JPEG)
        return OGPU_ERR_UNSUPPORTED;
    if (!w || !h) return OGPU_OK;
    if ((w | h) & 1) return OGPU_ERR_UNSUPPORTED;
    switch (space) {
    case OGPU_YUV_BT709: ky = 298; kr = 459; kgu = 55; kgv = 136; kb = 541; yoff = 16; break;
    case OGPU_YUV_JPEG: ky = 256; kr = 359; kgu = 88; kgv = 183; kb = 454; yoff = 0; break;
    default: ky = 298; kr = 409; kgu = 100; kgv = 208; kb = 516; yoff = 16; break;
    }
    if (packed) {
        if (ybpr < (ogpu_u32)w * 2 || ybpr > 0x01000000UL || !span_len(ybpr, h, (ogpu_u32)w * 2, &len)) return OGPU_ERR_UNSUPPORTED;
        if (!(yp = c->map(c->user, ya, len))) return OGPU_ERR_NOMAP;
    } else {
        if (ybpr < (ogpu_u32)w || ybpr > 0x01000000UL || !span_len(ybpr, h, (ogpu_u32)w, &len)) return OGPU_ERR_UNSUPPORTED;
        if (!(yp = c->map(c->user, ya, len))) return OGPU_ERR_NOMAP;
        if (semi) {
            if (ubpr < (ogpu_u32)w || ubpr > 0x01000000UL || !span_len(ubpr, h / 2, (ogpu_u32)w, &len)) return OGPU_ERR_UNSUPPORTED;
            if (!(up = c->map(c->user, ua, len))) return OGPU_ERR_NOMAP;
        } else {
            if (ubpr < (ogpu_u32)w / 2 || ubpr > 0x01000000UL || !span_len(ubpr, h / 2, (ogpu_u32)w / 2, &len)) return OGPU_ERR_UNSUPPORTED;
            if (!(up = c->map(c->user, ua, len))) return OGPU_ERR_NOMAP;
            if (vbpr < (ogpu_u32)w / 2 || vbpr > 0x01000000UL || !span_len(vbpr, h / 2, (ogpu_u32)w / 2, &len)) return OGPU_ERR_UNSUPPORTED;
            if (!(vp = c->map(c->user, va, len))) return OGPU_ERR_NOMAP;
        }
    }
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    drow = px_at(t, x, y);
    for (j = 0; j < h; j++, drow += t->bpr) {
        int sy = oy + j;
        const ogpu_u8 *yr = yp + (long)sy * (long)ybpr;
        const ogpu_u8 *ur = up ? up + (long)(sy >> 1) * (long)ubpr : 0, *vr = vp ? vp + (long)(sy >> 1) * (long)vbpr : 0;
        ogpu_u8 *dp = drow;
        for (i = 0; i < w; i++, dp += dbpp) {
            int sx = ox + i, Y, U, V, cy, r, g, b;
            if (packed) {
                const ogpu_u8 *q = yr + ((long)(sx >> 1) << 2);
                switch (layout) {
                case OGPU_YUV_YUY2: Y = q[(sx & 1) << 1]; U = q[1]; V = q[3]; break;
                case OGPU_YUV_UYVY: Y = q[1 + ((sx & 1) << 1)]; U = q[0]; V = q[2]; break;
                default: Y = q[(sx & 1) << 1]; V = q[1]; U = q[3]; break;      /* YVYU */
                }
            } else {
                Y = yr[sx];
                if (semi) {
                    const ogpu_u8 *q = ur + ((sx >> 1) << 1);
                    if (layout == OGPU_YUV_NV12) { U = q[0]; V = q[1]; } else { V = q[0]; U = q[1]; }
                } else { U = ur[sx >> 1]; V = vr[sx >> 1]; }
            }
            cy = SMUL16(Y - yoff, ky) + 128;
            U -= 128; V -= 128;
            r = (cy + SMUL16(V, kr)) >> 8;
            g = (cy - SMUL16(U, kgu) - SMUL16(V, kgv)) >> 8;
            b = (cy + SMUL16(U, kb)) >> 8;
            st(dp, t->format, 255, clamp255(r), clamp255(g), clamp255(b));
        }
    }
    return OGPU_OK;
}

/* ---- COMPOSITE_PD (v1.4) ------------------------------------------------------------ */

/* Porter-Duff on premultiplied channels, as stream.h says: each product
 * rounded (mul255), the sum saturating. pixman's own arithmetic, so its
 * results come out the same. */
PX_FN unsigned sat8(unsigned v) { return v > 255 ? 255 : v; }
PX_FN unsigned pd2(unsigned x, unsigned fa, unsigned y, unsigned fb) {
    return sat8(mul255(x, fa) + mul255(y, fb));
}
PX_FN void pd_apply(struct ch *d, const struct ch *s, int op) {
    unsigned as = s->a, ad = d->a, fa, fb;
    switch (op) {
    case OGPU_PD_CLEAR: d->a = d->r = d->g = d->b = 0; return;
    case OGPU_PD_SRC: *d = *s; return;
    case OGPU_PD_DST: return;
    case OGPU_PD_ADD:
        d->a = sat8(s->a + d->a); d->r = sat8(s->r + d->r); d->g = sat8(s->g + d->g); d->b = sat8(s->b + d->b);
        return;
    case OGPU_PD_OVER: fa = 255; fb = 255 - as; break;
    case OGPU_PD_OVER_REVERSE: fa = 255 - ad; fb = 255; break;
    case OGPU_PD_IN: fa = ad; fb = 0; break;
    case OGPU_PD_IN_REVERSE: fa = 0; fb = as; break;
    case OGPU_PD_OUT: fa = 255 - ad; fb = 0; break;
    case OGPU_PD_OUT_REVERSE: fa = 0; fb = 255 - as; break;
    case OGPU_PD_ATOP: fa = ad; fb = 255 - as; break;
    case OGPU_PD_ATOP_REVERSE: fa = 255 - ad; fb = as; break;
    default: fa = 255 - ad; fb = 255 - as; break;       /* OGPU_PD_XOR */
    }
    d->a = pd2(s->a, fa, d->a, fb);
    d->r = pd2(s->r, fa, d->r, fb);
    d->g = pd2(s->g, fa, d->g, fb);
    d->b = pd2(s->b, fa, d->b, fb);
}

/* v mod n, for n > 0, never negative. */
static int wrap(int v, int n) {
    v %= n;
    return v < 0 ? v + n : v;
}

OP_FN op_composite_pd(struct ogpu_core *c, const ogpu_u8 *a) {
    ogpu_u32 of = rd32(a), src = rd32(a + 4), sxy = rd32(a + 8), mask = rd32(a + 12), mxy = rd32(a + 16);
    ogpu_u32 dxy = rd32(a + 20), dwh = rd32(a + 24);
    const struct ogpu_surface *t = &c->slot[c->target], *s = 0, *m = 0;
    int op = (int)(of & 255), x = hi_s(dxy), y = lo_s(dxy), w = hi_u(dwh), h = lo_u(dwh), ox, oy, i, j;
    int sx = hi_s(sxy), sy = lo_s(sxy), mx = hi_s(mxy), my = lo_s(mxy), dbpp, sbpp = 0, mbpp = 0;
    int srep = (of & OGPU_PD_SRC_REPEAT) != 0, mrep = (of & OGPU_PD_MASK_REPEAT) != 0;
    unsigned malpha = 255;
    struct ch solid = { 0, 0, 0, 0 };
    ogpu_u8 *drow;
    if (op >= OGPU_PD_OPS) return OGPU_ERR_UNSUPPORTED;
    if (!is_colour(t->format) && t->format != OGPU_FMT_A8) return OGPU_ERR_UNSUPPORTED;
    if ((of & OGPU_PD_MASK) && (of & OGPU_PD_MASK_SOLID)) return OGPU_ERR_UNSUPPORTED;
    if (of & OGPU_PD_SOLID) ch_of(src, &solid);
    else {
        if (src >= OGPU_MAX_SLOTS || !c->slot[src].pixels) return OGPU_ERR_NOSURFACE;
        s = &c->slot[src];
        if (!is_colour(s->format) && s->format != OGPU_FMT_A8) return OGPU_ERR_UNSUPPORTED;
        sbpp = bytes_per_pixel(s->format);
    }
    if (of & OGPU_PD_MASK) {
        if (mask >= OGPU_MAX_SLOTS || !c->slot[mask].pixels) return OGPU_ERR_NOSURFACE;
        m = &c->slot[mask];
        if (!is_colour(m->format) && m->format != OGPU_FMT_A8) return OGPU_ERR_UNSUPPORTED;
        mbpp = bytes_per_pixel(m->format);
    } else if (of & OGPU_PD_MASK_SOLID) malpha = (unsigned)(mask & 255);
    if (!w || !h) return OGPU_OK;
    if (!clip_rect(c, &x, &y, &w, &h, &ox, &oy)) return OGPU_OK;
    if (op == OGPU_PD_DST) return OGPU_OK;
    dbpp = bytes_per_pixel(t->format);
    drow = px_at(t, x, y);
    /* The browser's two commonest, without the general reads and writes
     * (the same arithmetic): an ARGB32 image OVER ARGB32 or XRGB32, and a
     * solid colour OVER them through an A8 mask (text). */
    if (op == OGPU_PD_OVER && (t->format == OGPU_FMT_ARGB32 || t->format == OGPU_FMT_XRGB32) && !srep && !mrep &&
        ((s && s->format == OGPU_FMT_ARGB32 && !(of & (OGPU_PD_MASK | OGPU_PD_MASK_SOLID)) &&
          sx + ox >= 0 && sy + oy >= 0 && sx + ox + w <= s->w && sy + oy + h <= s->h) ||
         (!s && m && m->format == OGPU_FMT_A8 && mx + ox >= 0 && my + oy >= 0 && mx + ox + w <= m->w && my + oy + h <= m->h))) {
        int xa = t->format == OGPU_FMT_XRGB32;
        for (j = 0; j < h; j++, drow += t->bpr) {
            ogpu_u8 *dp = drow;
            if (s) {
                const ogpu_u8 *sp = row_at(s, sy + oy + j) + xoff(sx + ox, 4);
                for (i = 0; i < w; i++, dp += 4, sp += 4) {
                    unsigned sa = sp[0], ia;
                    if (sa == 255) { dp[0] = xa ? 0 : 255; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3]; continue; }
                    if (!(sa | sp[1] | sp[2] | sp[3])) continue;
                    ia = 255 - sa;
                    dp[0] = xa ? 0 : (ogpu_u8)sat8(sa + mul255(dp[0], ia));
                    dp[1] = (ogpu_u8)sat8(sp[1] + mul255(dp[1], ia));
                    dp[2] = (ogpu_u8)sat8(sp[2] + mul255(dp[2], ia));
                    dp[3] = (ogpu_u8)sat8(sp[3] + mul255(dp[3], ia));
                }
            } else {
                const ogpu_u8 *mp = row_at(m, my + oy + j) + (mx + ox);
                for (i = 0; i < w; i++, dp += 4) {
                    unsigned ma = mp[i], ca, cr, cg, cb, ia;
                    if (!ma) continue;
                    if (ma == 255) { ca = solid.a; cr = solid.r; cg = solid.g; cb = solid.b; }
                    else { ca = mul255(solid.a, ma); cr = mul255(solid.r, ma); cg = mul255(solid.g, ma); cb = mul255(solid.b, ma); }
                    if (ca == 255) { dp[0] = xa ? 0 : 255; dp[1] = (ogpu_u8)cr; dp[2] = (ogpu_u8)cg; dp[3] = (ogpu_u8)cb; continue; }
                    if (!(ca | cr | cg | cb)) continue;
                    ia = 255 - ca;
                    dp[0] = xa ? 0 : (ogpu_u8)sat8(ca + mul255(dp[0], ia));
                    dp[1] = (ogpu_u8)sat8(cr + mul255(dp[1], ia));
                    dp[2] = (ogpu_u8)sat8(cg + mul255(dp[2], ia));
                    dp[3] = (ogpu_u8)sat8(cb + mul255(dp[3], ia));
                }
            }
        }
        return OGPU_OK;
    }
    for (j = 0; j < h; j++, drow += t->bpr) {
        ogpu_u8 *dp = drow;
        const ogpu_u8 *srow = 0, *mrow = 0;
        int scol = 0, mcol = 0, sok = 1, mok = 1;
        if (s) {
            int ry = sy + oy + j;
            if (srep) ry = wrap(ry, s->h);
            if (ry >= 0 && ry < s->h) srow = row_at(s, ry); else sok = 0;
            scol = sx + ox;
            if (srep) scol = wrap(scol, s->w);
        }
        if (m) {
            int ry = my + oy + j;
            if (mrep) ry = wrap(ry, m->h);
            if (ry >= 0 && ry < m->h) mrow = row_at(m, ry); else mok = 0;
            mcol = mx + ox;
            if (mrep) mcol = wrap(mcol, m->w);
        }
        for (i = 0; i < w; i++, dp += dbpp) {
            struct ch sp, d;
            unsigned ma = malpha;
            if (s) {
                if (sok && scol >= 0 && scol < s->w) ld(srow + xoff(scol, sbpp), s->format, &sp);
                else sp.a = sp.r = sp.g = sp.b = 0;
                if (++scol == s->w && srep) scol = 0;
            } else sp = solid;
            if (m) {
                if (mok && mcol >= 0 && mcol < m->w) {
                    struct ch mp;
                    ld(mrow + xoff(mcol, mbpp), m->format, &mp);
                    ma = mp.a;
                } else ma = 0;
                if (++mcol == m->w && mrep) mcol = 0;
            }
            if (ma != 255) {
                sp.a = mul255(sp.a, ma); sp.r = mul255(sp.r, ma); sp.g = mul255(sp.g, ma); sp.b = mul255(sp.b, ma);
            }
            if (op == OGPU_PD_OVER && sp.a == 255) {          /* covers: as SRC */
                st(dp, t->format, 255, sp.r, sp.g, sp.b);
                continue;
            }
            if (op == OGPU_PD_OVER && !(sp.a | sp.r | sp.g | sp.b)) continue;
            ld(dp, t->format, &d);
            pd_apply(&d, &sp, op);
            st(dp, t->format, d.a, d.r, d.g, d.b);
        }
    }
    return OGPU_OK;
}

/* ---- the run ---------------------------------------------------------------------- */

void ogpu_core_init(struct ogpu_core *c) {
    int i;
    for (i = 0; i < OGPU_MAX_SLOTS; i++) c->slot[i].pixels = 0;
    c->target = -1;
    c->cx0 = c->cy0 = c->cx1 = c->cy1 = 0;
    c->last_error = OGPU_OK;
    c->error_word = 0;
    c->d3 = 0;
}

/* Words each opcode needs after its header (-1: not a command of the core;
 * -2: the 3D range, whose lengths ogpu_3d.c checks). */
static int args_for(int op) {
    switch (op) {
    case OGPU_OP_NOP: return 0;
    case OGPU_OP_SURFACE: return 5;
    case OGPU_OP_TARGET: return 1;
    case OGPU_OP_CLIP: return 2;
    case OGPU_OP_FILL: case OGPU_OP_INVERT: return 3;
    case OGPU_OP_COPY: return 4;
    case OGPU_OP_TEMPLATE: return 8;
    case OGPU_OP_PATTERN: return 7;
    case OGPU_OP_LINE: return 4;
    case OGPU_OP_PIXELS: return 6;
    case OGPU_OP_MASK: return 5;
    case OGPU_OP_FILL_BLEND: case OGPU_OP_LINES_BLEND: case OGPU_OP_POINTS_BLEND: return 4;
    case OGPU_OP_COMPOSITE: return 7;       /* 9 with OGPU_COMP_MASK, checked in the run */
    case OGPU_OP_COMPOSITE_AFFINE: return 11;
    case OGPU_OP_YUV: return 9;
    case OGPU_OP_COMPOSITE_PD: return 7;
    case OGPU_OP_FENCE: return 1;
    }
#ifdef OGPU_WITH_3D
    if (op >= 0x0030 && op <= 0x003F) return -2;
#endif
    return -1;
}

long ogpu_core_run(struct ogpu_core *c, const ogpu_u8 *stream, long words) {
    long at = 0, done = 0;
    c->last_error = OGPU_OK;
    while (at < words) {
        ogpu_u32 hdr = rd32(stream + at * 4);
        int op = (int)OGPU_HDR_OP(hdr), len = (int)OGPU_HDR_WORDS(hdr), need = args_for(op), r = OGPU_OK;
        const ogpu_u8 *a = stream + at * 4 + 4;
        if (len < 1 || at + len > words) {
            if (c->last_error == OGPU_OK) { c->last_error = OGPU_ERR_BADLEN; c->error_word = at; }
            break;
        }
        if (need >= 0 && (len - 1 < need
                          || (op == OGPU_OP_COMPOSITE && (rd32(a + 24) & OGPU_COMP_MASK) && len - 1 < 9))) {
            /* Too short for its own arguments: what follows can't be trusted either. */
            if (c->last_error == OGPU_OK) { c->last_error = OGPU_ERR_BADLEN; c->error_word = at; }
            break;
        }
        if (need == -1)
            r = op >= OGPU_OP_EXT_FIRST && op <= OGPU_OP_EXT_LAST && c->ext ? c->ext(c->user, op, stream + at * 4, len)
                                                                           : OGPU_ERR_BADOP;
#ifdef OGPU_WITH_3D
        else if (need == -2)
            r = c->d3 ? ogpu_3d_run(c, op, stream + at * 4, len) : OGPU_ERR_BADOP;
#endif
        else if (op >= OGPU_OP_FILL && op < OGPU_OP_FENCE && (c->target < 0 || !c->slot[c->target].pixels))
            r = OGPU_ERR_NOSURFACE;
        else switch (op) {
        case OGPU_OP_SURFACE: r = op_surface(c, a); break;
        case OGPU_OP_TARGET: {
            ogpu_u32 slot = rd32(a);
            if (slot >= OGPU_MAX_SLOTS || !c->slot[slot].pixels) { c->target = -1; r = OGPU_ERR_NOSURFACE; }
            else { c->target = (int)slot; c->cx0 = c->cy0 = c->cx1 = c->cy1 = 0; }
            break;
        }
        case OGPU_OP_CLIP: {
            ogpu_u32 xy = rd32(a), wh = rd32(a + 4);
            if (!hi_u(wh) || !lo_u(wh)) { c->cx0 = c->cy0 = c->cx1 = c->cy1 = 0; break; }   /* the whole target */
            c->cx0 = hi_s(xy); c->cy0 = lo_s(xy);
            c->cx1 = c->cx0 + hi_u(wh);
            c->cy1 = c->cy0 + lo_u(wh);
            break;
        }
        case OGPU_OP_FILL: r = op_fill(c, a, 0); break;
        case OGPU_OP_INVERT: r = op_fill(c, a, 1); break;
        case OGPU_OP_COPY: r = op_copy(c, a); break;
        case OGPU_OP_TEMPLATE: r = op_template(c, a); break;
        case OGPU_OP_PATTERN: r = op_pattern(c, a); break;
        case OGPU_OP_LINE: r = op_line(c, a); break;
        case OGPU_OP_PIXELS: r = op_pixels(c, a); break;
        case OGPU_OP_MASK: r = op_mask(c, a); break;
        case OGPU_OP_FILL_BLEND: r = op_fill_blend(c, a); break;
        case OGPU_OP_LINES_BLEND: r = op_lines_blend(c, a); break;
        case OGPU_OP_POINTS_BLEND: r = op_points_blend(c, a); break;
        case OGPU_OP_COMPOSITE: r = op_composite(c, a); break;
        case OGPU_OP_COMPOSITE_AFFINE: r = op_composite_affine(c, a); break;
        case OGPU_OP_YUV: r = op_yuv(c, a); break;
        case OGPU_OP_COMPOSITE_PD: r = op_composite_pd(c, a); break;
        case OGPU_OP_FENCE: if (c->fence) c->fence(c->user, rd32(a)); break;
        }
        if (r != OGPU_OK && c->last_error == OGPU_OK) { c->last_error = r; c->error_word = at; }
        if (op != OGPU_OP_NOP) done++;
        at += len;
    }
    return done;
}

int ogpu_core_supports(int op, int format) {
    int colour = is_colour(format);
#ifdef OGPU_WITH_3D
    if (op >= 0x0030 && op <= 0x003F) return ogpu3d_supports(op, format);
#endif
    if (!colour && format != OGPU_FMT_CLUT8 && format != OGPU_FMT_A8) return OGPU_NONE;
    switch (op) {
    case OGPU_OP_NOP: case OGPU_OP_SURFACE: case OGPU_OP_TARGET: case OGPU_OP_CLIP: case OGPU_OP_FENCE:
    case OGPU_OP_FILL: case OGPU_OP_INVERT: case OGPU_OP_COPY: case OGPU_OP_TEMPLATE:
    case OGPU_OP_PATTERN: case OGPU_OP_LINE:
        return OGPU_FULL;
    case OGPU_OP_PIXELS:    /* pens only on CLUT8, coverage only on A8 */
        return format == OGPU_FMT_CLUT8 || format == OGPU_FMT_A8 ? OGPU_PARTIAL : OGPU_FULL;
    case OGPU_OP_COMPOSITE: case OGPU_OP_MASK: case OGPU_OP_COMPOSITE_PD:
        return format == OGPU_FMT_CLUT8 ? OGPU_NONE : OGPU_FULL;
    case OGPU_OP_FILL_BLEND: case OGPU_OP_LINES_BLEND: case OGPU_OP_POINTS_BLEND:
    case OGPU_OP_COMPOSITE_AFFINE: case OGPU_OP_YUV:
        return colour ? OGPU_FULL : OGPU_NONE;
    }
    return OGPU_NONE;
}
