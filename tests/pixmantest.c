/* Copyright (c) 2026 AmiCradle Team. MIT licence (LICENSE).
 *
 * pixman.library 2.1 against upstream Pixman 0.46.4, pixel for pixel.
 * Built by tests/run.sh for a big-endian 68040 (m68k-linux-gnu-gcc, run
 * under qemu-m68k): the library's core (library/px_core.c, px_region.c),
 * GFOpenGPU's core (vendor/openamigartg, the same C the 68k back end of
 * gfopengpu.library and the Cradle's runtime run) and upstream Pixman, built
 * from its release tarball for this test only. Pixman is never in the
 * library: it is the reference here.
 *
 * Random cases: every operator CLEAR to ADD; solid and image sources and
 * masks (A8, ARGB32, solid), repeat NONE and NORMAL, integer translations,
 * rectangles partly outside; destinations a8r8g8b8, x8r8g8b8, b8g8r8a8,
 * r5g6b5, x1r5g5b5 and a8. Then fills, blits (against a plain copy: Pixman
 * 0.46.4 has no portable pixman_blt), rectangle fills and region unions
 * and intersections. Pixels must be the same, apart from the bits a format
 * leaves unused (x8r8g8b8's top byte, x1r5g5b5's top bit). Calls the core
 * refuses (DESIGN.md, "Gaps") are counted and must be 0 here. */
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../library/px_core.h"
#include "../vendor/openamigartg/library/opengpu/ogpu_core.h"

/* On a 64-bit host, everything GFOpenGPU reads comes from memory below 4 GB
 * (MAP_32BIT), since stream addresses are 32 bits, as the Amiga's are. */
#if UINTPTR_MAX > 0xFFFFFFFFUL
#include <sys/mman.h>
static unsigned char *lo_base, *lo_next;
static void *lo_calloc(size_t n, size_t k) {
    size_t bytes = (n * k + 15) & ~(size_t)15;
    void *p;
    if (!lo_base) {
        lo_base = mmap(0, (size_t)1 << 27, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT | MAP_NORESERVE, -1, 0);
        if (lo_base == MAP_FAILED) { puts("no memory below 4 GB"); exit(2); }
        lo_next = lo_base;
    }
    if (lo_next + bytes > lo_base + ((size_t)1 << 27)) { puts("test arena full"); exit(2); }
    p = lo_next; lo_next += bytes ? bytes : 16;
    memset(p, 0, bytes);
    return p;
}
#define calloc lo_calloc
#define malloc(n) lo_calloc(1, (n))
#define free(p) ((void)(p))
#endif

/* ---- the core's environment: memory as it is, GFOpenGPU's core run in place ---- */

static ogpu_u8 *map(void *user, ogpu_u32 address, ogpu_u32 length) { (void)user; (void)length; return (ogpu_u8 *)(unsigned long)address; }
static unsigned long t_biggest;                       /* the biggest block asked for */
static void *t_alloc(void *ctx, unsigned long n) { (void)ctx; if (n > t_biggest) t_biggest = n; return calloc(1, n ? n : 1); }
static void t_free(void *ctx, void *p) { (void)ctx; free(p); }
static unsigned long t_address(void *ctx, const void *p) { (void)ctx; return (unsigned long)p; }
static long t_submit(void *ctx, const void *stream, long words) {
    struct ogpu_core c;
    (void)ctx;
    ogpu_core_init(&c);
    c.map = map;
    ogpu_core_run(&c, stream, words);
    return c.last_error;
}
static const struct px_env ENV = { t_alloc, t_free, t_address, t_submit, 0 };
static struct px_stats ST;

static unsigned seed = 1;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 8) & 0xFFFFFFu; }

static const pixman_format_code_t dfmts[] = { PIXMAN_a8r8g8b8, PIXMAN_x8r8g8b8, PIXMAN_b8g8r8a8, PIXMAN_r5g6b5, PIXMAN_x1r5g5b5, PIXMAN_a8 };
#define NDF (int)(sizeof dfmts / sizeof dfmts[0])
static int bpp_of(pixman_format_code_t f) { return PIXMAN_FORMAT_BPP(f) / 8; }

/* random, premultiplied pixels */
static void fill_random(uint8_t *p, pixman_format_code_t f, int stride, int w, int h) {
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint8_t *q = p + y * stride + x * bpp_of(f);
            unsigned k = rnd() % 4, a = k == 0 ? 0 : k == 1 ? 255 : rnd() & 255;
            unsigned r = rnd() % (a + 1), g = rnd() % (a + 1), b = rnd() % (a + 1);
            switch ((unsigned)f) {
            case PIXMAN_a8r8g8b8: q[0] = (uint8_t)a; q[1] = (uint8_t)r; q[2] = (uint8_t)g; q[3] = (uint8_t)b; break;
            case PIXMAN_b8g8r8a8: q[0] = (uint8_t)b; q[1] = (uint8_t)g; q[2] = (uint8_t)r; q[3] = (uint8_t)a; break;
            case PIXMAN_a8: q[0] = (uint8_t)a; break;
            default: { int i; for (i = 0; i < bpp_of(f); i++) q[i] = (uint8_t)rnd(); }
            }
        }
}

/* the bytes of row y, x0..x1, equal but for a format's unused bits */
static int same(const uint8_t *a, const uint8_t *b, pixman_format_code_t f, int n) {
    int i;
    for (i = 0; i < n; i++) {
        uint8_t m = 0xFF;
        if (f == PIXMAN_x8r8g8b8 && i % 4 == 0) m = 0;
        if (f == PIXMAN_x1r5g5b5 && i % 2 == 0) m = 0x7F;
        if ((a[i] & m) != (b[i] & m)) return 0;
    }
    return 1;
}

struct pair { pixman_image_t *up, *ours; };

static struct pair make_bits(pixman_format_code_t f, int w, int h, uint8_t **mem, int *stride_out) {
    struct pair p;
    int stride = (w * bpp_of(f) + 3) & ~3;
    uint8_t *a = calloc(1, (size_t)stride * h + 4), *b = calloc(1, (size_t)stride * h + 4);
    fill_random(a, f, stride, w, h);
    memcpy(b, a, (size_t)stride * h);
    p.up = pixman_image_create_bits(f, w, h, (uint32_t *)a, stride);
    p.ours = px_image_create_bits(&ENV, f, w, h, (uint32_t *)b, stride);
    mem[0] = a; mem[1] = b;
    *stride_out = stride;
    return p;
}

static int failures;
#define FAIL(...) do { if (++failures <= 12) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void composites(int n) {
    int iter, cases = 0;
    for (iter = 0; iter < n; iter++) {
        pixman_format_code_t df = dfmts[rnd() % NDF], sf = dfmts[rnd() % NDF];
        pixman_format_code_t mf = rnd() % 2 ? PIXMAN_a8 : PIXMAN_a8r8g8b8;
        int dw = 1 + rnd() % 48, dh = 1 + rnd() % 32, sw = 1 + rnd() % 32, sh = 1 + rnd() % 24, mw = 1 + rnd() % 32, mh = 1 + rnd() % 24;
        int op = (int)(rnd() % 13), skind = (int)(rnd() % 3), mkind = (int)(rnd() % 4);
        int x = (int)(rnd() % 50) - 8, y = (int)(rnd() % 34) - 8, w = 1 + (int)(rnd() % 60), h = 1 + (int)(rnd() % 40);
        int sx = (int)(rnd() % 40) - 10, sy = (int)(rnd() % 30) - 10, mx = (int)(rnd() % 40) - 10, my = (int)(rnd() % 30) - 10;
        uint8_t *dm[2], *sm[2] = { 0, 0 }, *mm[2] = { 0, 0 };
        int dstride, sstride, mstride, yy;
        struct pair d, s, m = { 0, 0 };
        pixman_color_t c;
        d = make_bits(df, dw, dh, dm, &dstride);
        if (skind == 0) {
            unsigned a = rnd() & 255;
            c.alpha = (uint16_t)(a * 257); c.red = (uint16_t)((rnd() % (a + 1)) * 257);
            c.green = (uint16_t)((rnd() % (a + 1)) * 257); c.blue = (uint16_t)((rnd() % (a + 1)) * 257);
            s.up = pixman_image_create_solid_fill(&c); s.ours = px_image_create_solid(&ENV, &c);
        } else {
            s = make_bits(sf, sw, sh, sm, &sstride);
            if (rnd() % 3 == 0) { pixman_image_set_repeat(s.up, PIXMAN_REPEAT_NORMAL); px_image_set_repeat(s.ours, PIXMAN_REPEAT_NORMAL); }
            if (rnd() % 4 == 0) {
                pixman_transform_t t;
                pixman_transform_init_translate(&t, pixman_int_to_fixed((int)(rnd() % 9) - 4), pixman_int_to_fixed((int)(rnd() % 9) - 4));
                pixman_image_set_transform(s.up, &t); px_image_set_transform(s.ours, &t);
            }
        }
        if (mkind == 1) {
            unsigned a = rnd() & 255;
            pixman_color_t mc = { 0, 0, 0, (uint16_t)(a * 257) };
            m.up = pixman_image_create_solid_fill(&mc); m.ours = px_image_create_solid(&ENV, &mc);
        } else if (mkind >= 2) {
            m = make_bits(mf, mw, mh, mm, &mstride);
            if (rnd() % 3 == 0) { pixman_image_set_repeat(m.up, PIXMAN_REPEAT_NORMAL); px_image_set_repeat(m.ours, PIXMAN_REPEAT_NORMAL); }
        }
        pixman_image_composite32((pixman_op_t)op, s.up, m.up, d.up, sx, sy, mx, my, x, y, w, h);
        px_composite32(&ENV, &ST, op, s.ours, m.ours, d.ours, sx, sy, mx, my, x, y, w, h);
        cases++;
        for (yy = 0; yy < dh; yy++)
            if (!same(dm[0] + yy * dstride, dm[1] + yy * dstride, df, dw * bpp_of(df))) {
                FAIL("composite %d: op %d dst %08x src %s %08x mask %d %08x, row %d", iter, op, (unsigned)df,
                     skind ? "bits" : "solid", (unsigned)sf, mkind, (unsigned)mf, yy);
                break;
            }
        pixman_image_unref(d.up); px_image_unref(&ENV, d.ours);
        pixman_image_unref(s.up); px_image_unref(&ENV, s.ours);
        if (m.up) { pixman_image_unref(m.up); px_image_unref(&ENV, m.ours); }
        free(dm[0]); free(dm[1]); free(sm[0]); free(sm[1]); free(mm[0]); free(mm[1]);
    }
    printf("composite32: %d cases\n", cases);
}

static void fills(int n) {
    int iter;
    for (iter = 0; iter < n; iter++) {
        int bpps[3] = { 8, 16, 32 }, bpp = bpps[rnd() % 3], stride = 4 + (int)(rnd() % 16);   /* uint32s a row */
        int rows = 1 + (int)(rnd() % 20), rowpx = stride * 4 / (bpp / 8);
        int x = (int)(rnd() % rowpx), y = (int)(rnd() % rows), w = (int)(rnd() % (rowpx - x + 1)), h = (int)(rnd() % (rows - y + 1));
        uint32_t v = (uint32_t)rnd() << 8 ^ rnd();
        uint32_t *a = calloc((size_t)stride * rows, 4), *b = calloc((size_t)stride * rows, 4);
        pixman_fill(a, stride, bpp, x, y, w, h, v);
        px_fill(&ENV, &ST, b, stride, bpp, x, y, w, h, v);
        if (memcmp(a, b, (size_t)stride * rows * 4)) FAIL("fill %d: bpp %d %d,%d %dx%d", iter, bpp, x, y, w, h);
        free(a); free(b);
    }
    printf("fill: %d cases\n", n);
}

static void blts(int n) {
    int iter;
    for (iter = 0; iter < n; iter++) {
        int bpps[3] = { 8, 16, 32 }, bpp = bpps[rnd() % 3], B = bpp / 8, stride = 4 + (int)(rnd() % 16), rows = 2 + (int)(rnd() % 20);
        int rowpx = stride * 4 / B, w = 1 + (int)(rnd() % (rowpx / 2)), h = 1 + (int)(rnd() % (rows / 2 + 1));
        int sx = (int)(rnd() % (rowpx - w + 1)), sy = (int)(rnd() % (rows - h + 1)), dx = (int)(rnd() % (rowpx - w + 1)), dy = (int)(rnd() % (rows - h + 1));
        int one = rnd() % 2, i, j;
        size_t bytes = (size_t)stride * rows * 4;
        uint8_t *src = malloc(bytes), *dst = malloc(bytes), *ref = malloc(bytes), *tmp = malloc(bytes);
        for (i = 0; i < (int)bytes; i++) src[i] = (uint8_t)rnd();
        for (i = 0; i < (int)bytes; i++) dst[i] = (uint8_t)rnd();
        if (one) memcpy(dst, src, bytes);
        memcpy(ref, dst, bytes);
        /* the reference: through a copy of the source, so overlap can't matter */
        memcpy(tmp, one ? ref : src, bytes);
        for (j = 0; j < h; j++)
            for (i = 0; i < w * B; i++)
                ref[(dy + j) * stride * 4 + dx * B + i] = tmp[(sy + j) * stride * 4 + sx * B + i];
        px_blt(&ENV, &ST, (uint32_t *)(one ? dst : src), (uint32_t *)dst, stride, stride, bpp, bpp, sx, sy, dx, dy, w, h);
        if (memcmp(ref, dst, bytes)) FAIL("blt %d: bpp %d %s", iter, bpp, one ? "same buffer" : "two buffers");
        free(src); free(dst); free(ref); free(tmp);
    }
    printf("blt: %d cases\n", n);
}

static void rects(int n) {
    int iter;
    for (iter = 0; iter < n; iter++) {
        pixman_format_code_t df = dfmts[rnd() % NDF];
        int dw = 1 + rnd() % 64, dh = 1 + rnd() % 48, op = (int)(rnd() % 13), k = 1 + (int)(rnd() % 8), i, yy, dstride;
        uint8_t *dm[2];
        pixman_rectangle16_t r[8];
        unsigned a = rnd() % 3 == 0 ? 255 : rnd() & 255;
        pixman_color_t c;
        struct pair d = make_bits(df, dw, dh, dm, &dstride);
        c.alpha = (uint16_t)(a * 257); c.red = (uint16_t)((rnd() % (a + 1)) * 257);
        c.green = (uint16_t)((rnd() % (a + 1)) * 257); c.blue = (uint16_t)((rnd() % (a + 1)) * 257);
        /* inside the image: upstream's SRC fills don't clip to it (a caller's
         * rectangles must be inside); pixman.library's do */
        for (i = 0; i < k; i++) {
            r[i].x = (int16_t)(rnd() % dw); r[i].y = (int16_t)(rnd() % dh);
            r[i].width = (uint16_t)(rnd() % (dw - r[i].x + 1)); r[i].height = (uint16_t)(rnd() % (dh - r[i].y + 1));
        }
        pixman_image_fill_rectangles((pixman_op_t)op, d.up, &c, k, r);
        px_fill_rectangles(&ENV, &ST, op, d.ours, &c, k, r);
        for (yy = 0; yy < dh; yy++)
            if (!same(dm[0] + yy * dstride, dm[1] + yy * dstride, df, dw * bpp_of(df))) {
                FAIL("fill_rectangles %d: op %d dst %08x row %d", iter, op, (unsigned)df, yy);
                break;
            }
        pixman_image_unref(d.up); px_image_unref(&ENV, d.ours);
        free(dm[0]); free(dm[1]);
    }
    printf("fill_rectangles: %d cases\n", n);
}

/* a random region of up to k rectangles, each up to size x size, in a span x
 * span area, built the same way on both sides */
static void rand_region_n(pixman_region32_t *up, pixman_region32_t *ours, int k, int span, unsigned size) {
    int i;
    pixman_region32_init(up); px_region32_init(ours);
    for (i = 0; i < k; i++) {
        int x = (int)(rnd() % (unsigned)span) - span / 6, y = (int)(rnd() % (unsigned)span) - span / 6;
        unsigned w = rnd() % size, h = rnd() % size;
        pixman_region32_t ru, ro;
        pixman_region32_init_rect(&ru, x, y, w, h); px_region32_init_rect(&ro, x, y, w, h);
        pixman_region32_union(up, up, &ru); px_region32_union(&ENV, ours, ours, &ro);
        pixman_region32_fini(&ru); px_region32_fini(&ENV, &ro);
    }
}
static void rand_region(pixman_region32_t *up, pixman_region32_t *ours) { rand_region_n(up, ours, (int)(rnd() % 6), 60, 30); }

static int same_region(pixman_region32_t *up, pixman_region32_t *ours) {
    int nu, i;
    pixman_box32_t *bu = pixman_region32_rectangles(up, &nu);
    long no = ours->data ? ours->data->numRects : (ours->extents.x2 > ours->extents.x1 && ours->extents.y2 > ours->extents.y1);
    const pixman_box32_t *bo = ours->data ? (const pixman_box32_t *)(ours->data + 1) : &ours->extents;
    if (nu != no) return 0;
    for (i = 0; i < nu; i++)
        if (bu[i].x1 != bo[i].x1 || bu[i].y1 != bo[i].y1 || bu[i].x2 != bo[i].x2 || bu[i].y2 != bo[i].y2) return 0;
    if (nu && memcmp(&up->extents, &ours->extents, sizeof up->extents)) return 0;
    return 1;
}

static void regions(int n) {
    int iter;
    for (iter = 0; iter < n; iter++) {
        pixman_region32_t ua, oa, ub, ob, ur, orr, uc, oc;
        rand_region(&ua, &oa);
        rand_region(&ub, &ob);
        if (!same_region(&ua, &oa)) FAIL("region %d: building", iter);
        pixman_region32_init(&ur); px_region32_init(&orr);
        pixman_region32_union(&ur, &ua, &ub); px_region32_union(&ENV, &orr, &oa, &ob);
        if (!same_region(&ur, &orr)) FAIL("region %d: union", iter);
        pixman_region32_intersect(&ur, &ua, &ub); px_region32_intersect(&ENV, &orr, &oa, &ob);
        if (!same_region(&ur, &orr)) FAIL("region %d: intersect", iter);
        pixman_region32_init(&uc); px_region32_init(&oc);
        pixman_region32_copy(&uc, &ua); px_region32_copy(&ENV, &oc, &oa);
        if (!same_region(&uc, &oc)) FAIL("region %d: copy", iter);
        pixman_region32_fini(&ua); pixman_region32_fini(&ub); pixman_region32_fini(&ur); pixman_region32_fini(&uc);
        px_region32_fini(&ENV, &oa); px_region32_fini(&ENV, &ob); px_region32_fini(&ENV, &orr); px_region32_fini(&ENV, &oc);
    }
    printf("region32: %d cases\n", n);
}

static long nboxes(const pixman_region32_t *r) {
    return r->data ? r->data->numRects : (r->extents.x2 > r->extents.x1 && r->extents.y2 > r->extents.y1);
}

/* Big regions: hundreds of rectangles, and stripes whose union has a box
 * for every crossing. The results must match Pixman's, and no block the
 * library asks for may be more than about twice the boxes in and out: the
 * output grows as it needs to, not sized for the worst case up front. */
static void big_regions(int n) {
    int iter;
    for (iter = 0; iter < n; iter++) {
        pixman_region32_t ua, oa, ub, ob, ur, orr;
        int op;
        if (iter % 4 == 3) {                        /* 60 columns and 60 rows */
            int i;
            pixman_region32_init(&ua); px_region32_init(&oa); pixman_region32_init(&ub); px_region32_init(&ob);
            for (i = 0; i < 60; i++) {
                pixman_region32_t ru, ro;
                pixman_region32_init_rect(&ru, i * 10, 0, 5, 600); px_region32_init_rect(&ro, i * 10, 0, 5, 600);
                pixman_region32_union(&ua, &ua, &ru); px_region32_union(&ENV, &oa, &oa, &ro);
                pixman_region32_fini(&ru); px_region32_fini(&ENV, &ro);
                pixman_region32_init_rect(&ru, 0, i * 10, 600, 5); px_region32_init_rect(&ro, 0, i * 10, 600, 5);
                pixman_region32_union(&ub, &ub, &ru); px_region32_union(&ENV, &ob, &ob, &ro);
                pixman_region32_fini(&ru); px_region32_fini(&ENV, &ro);
            }
        } else {
            rand_region_n(&ua, &oa, 250, 1200, 60);
            rand_region_n(&ub, &ob, 250, 1200, 60);
        }
        if (!same_region(&ua, &oa) || !same_region(&ub, &ob)) FAIL("big region %d: building", iter);
        for (op = 0; op < 2; op++) {
            long bound;
            pixman_region32_init(&ur); px_region32_init(&orr);
            t_biggest = 0;
            if (op) { pixman_region32_union(&ur, &ua, &ub); if (!px_region32_union(&ENV, &orr, &oa, &ob)) FAIL("big region %d: union failed", iter); }
            else { pixman_region32_intersect(&ur, &ua, &ub); if (!px_region32_intersect(&ENV, &orr, &oa, &ob)) FAIL("big region %d: intersect failed", iter); }
            if (!same_region(&ur, &orr)) FAIL("big region %d: %s", iter, op ? "union" : "intersect");
            bound = 2 * (nboxes(&oa) + nboxes(&ob) + nboxes(&orr) + 4) * (long)sizeof(pixman_box32_t) + 64;
            if ((long)t_biggest > bound)
                FAIL("big region %d: %s of %ld and %ld boxes asked for %lu bytes in one block (more than %ld)", iter,
                     op ? "union" : "intersect", nboxes(&oa), nboxes(&ob), t_biggest, bound);
            pixman_region32_fini(&ur); px_region32_fini(&ENV, &orr);
        }
        pixman_region32_fini(&ua); pixman_region32_fini(&ub);
        px_region32_fini(&ENV, &oa); px_region32_fini(&ENV, &ob);
    }
    printf("region32, big: %d cases\n", n);
}

/* What GFOpenGPU can't do yet (DESIGN.md, "Gaps") is refused, counted and
 * not drawn: a scaling transform, PAD repeat, a component-alpha mask,
 * SATURATE. */
static void gaps(void) {
    uint32_t px[16 * 16], before[16 * 16];
    pixman_image_t *d, *s, *m;
    pixman_transform_t t;
    pixman_color_t c = { 0xffff, 0, 0, 0xffff };
    unsigned long r0 = ST.refused;
    int i;
    for (i = 0; i < 256; i++) px[i] = (uint32_t)rnd();
    memcpy(before, px, sizeof px);
    d = px_image_create_bits(&ENV, PIXMAN_a8r8g8b8, 16, 16, px, 64);
    s = px_image_create_bits(&ENV, PIXMAN_a8r8g8b8, 16, 16, 0, 0);
    m = px_image_create_bits(&ENV, PIXMAN_a8r8g8b8, 16, 16, 0, 0);
    pixman_transform_init_scale(&t, pixman_fixed_1 / 2, pixman_fixed_1 / 2);
    px_image_set_transform(s, &t);
    px_composite32(&ENV, &ST, PIXMAN_OP_OVER, s, 0, d, 0, 0, 0, 0, 0, 0, 16, 16);
    px_image_set_transform(s, 0);
    px_image_set_repeat(s, PIXMAN_REPEAT_PAD);
    px_composite32(&ENV, &ST, PIXMAN_OP_OVER, s, 0, d, 0, 0, 0, 0, 0, 0, 16, 16);
    px_image_set_repeat(s, PIXMAN_REPEAT_NONE);
    px_image_set_component_alpha(m, 1);
    px_composite32(&ENV, &ST, PIXMAN_OP_OVER, s, m, d, 0, 0, 0, 0, 0, 0, 16, 16);
    px_composite32(&ENV, &ST, PIXMAN_OP_SATURATE, s, 0, d, 0, 0, 0, 0, 0, 0, 16, 16);
    if (ST.refused - r0 != 4 || memcmp(px, before, sizeof px)) FAIL("gaps: %lu refused, pixels %s", ST.refused - r0,
                                                                    memcmp(px, before, sizeof px) ? "changed" : "kept");
    if (px_image_create_bits(&ENV, PIXMAN_r8g8b8, 4, 4, 0, 0)) FAIL("gaps: r8g8b8 was accepted");
    (void)c;
    px_image_unref(&ENV, d); px_image_unref(&ENV, s); px_image_unref(&ENV, m);
    ST.refused = r0;
    printf("gaps: refused as they should be\n");
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3000;
    const char *only = argc > 2 ? argv[2] : "cfbrg";     /* which parts: c f b r g */
    if (strchr(only, 'c')) composites(n);
    if (strchr(only, 'f')) fills(n / 3);
    if (strchr(only, 'b')) blts(n / 3);
    if (strchr(only, 'r')) rects(n / 3);
    if (strchr(only, 'g')) regions(n / 3);
    if (strchr(only, 'g')) big_regions(n / 300 + 4);
    gaps();
    printf("batches %lu, refused %lu, GFOpenGPU errors %lu\n", ST.batches, ST.refused, ST.errors);
    if (ST.refused || ST.errors) failures++;
    printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
