/* Copyright (c) 2026 AmiCradle Team. MIT licence (LICENSE).
 *
 * pixman.library's core (px_core.h, DESIGN.md). Each call is checked, its
 * rectangles clipped and its images described to GFOpenGPU as surfaces; the
 * pixels are GFOpenGPU's work (FILL, COPY, COMPOSITE_PD). What GFOpenGPU can't
 * do yet is refused and counted, never drawn here. */
#include "px_core.h"
#include "../vendor/openamigartg/include/gfopengpu/build.h"

#define PX_MAGIC 0x50584931UL           /* 'PXI1' */
enum { PX_BITS = 1, PX_SOLID = 2 };

struct px_image {
    unsigned long magic;
    long refs;
    int kind;
    pixman_format_code_t format;
    int ofmt;                           /* GFOpenGPU's format, 0 for a solid */
    int bpp;                            /* bytes a pixel */
    int w, h, stride;                   /* stride in bytes */
    uint32_t *bits;
    int own;                            /* the core allocated bits */
    unsigned long argb;                 /* a solid's colour, premultiplied 8888 as pixman takes it */
    int repeat, filter, component_alpha;
    int has_transform;
    pixman_transform_t transform;
};

#define IMG(p) ((struct px_image *)(p))
static int is_image(const pixman_image_t *p) { return p && IMG(p)->magic == PX_MAGIC; }

/* ---- formats ---------------------------------------------------------------------- */

/* Pixman's formats GFOpenGPU has, in the Amiga's byte order (big-endian:
 * a8r8g8b8 is the bytes A, R, G, B). */
static int ogpu_format(pixman_format_code_t f, int *bpp) {
    switch ((unsigned long)f) {
    case PIXMAN_a8r8g8b8: *bpp = 4; return OGPU_FMT_ARGB32;
    case PIXMAN_x8r8g8b8: *bpp = 4; return OGPU_FMT_XRGB32;
    case PIXMAN_b8g8r8a8: *bpp = 4; return OGPU_FMT_BGRA32;
    case PIXMAN_r5g6b5: *bpp = 2; return OGPU_FMT_RGB565;
    case PIXMAN_x1r5g5b5: *bpp = 2; return OGPU_FMT_RGB555;
    case PIXMAN_a8: *bpp = 1; return OGPU_FMT_A8;
    }
    *bpp = 0;
    return 0;
}

/* A pixman colour (16 bits a channel) as pixman takes it: the top 8 bits. */
static unsigned long argb_of(const pixman_color_t *c) {
    return (unsigned long)(c->alpha >> 8) << 24 | (unsigned long)(c->red >> 8) << 16 |
           (unsigned long)(c->green >> 8) << 8 | (unsigned long)(c->blue >> 8);
}

/* An 8888 colour as a raw pixel of a GFOpenGPU format (FILL's colour). */
static unsigned long raw_of(unsigned long argb, int ofmt) {
    unsigned long a = argb >> 24 & 255, r = argb >> 16 & 255, g = argb >> 8 & 255, b = argb & 255;
    switch (ofmt) {
    case OGPU_FMT_BGRA32: return b << 24 | g << 16 | r << 8 | a;
    case OGPU_FMT_RGB565: return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
    case OGPU_FMT_RGB555: return (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
    case OGPU_FMT_A8: return a;
    }
    return argb;                        /* ARGB32; XRGB32's X is written as 0 */
}

/* ---- images ----------------------------------------------------------------------- */

static struct px_image *new_image(const struct px_env *e) {
    struct px_image *im = e->alloc(e->ctx, sizeof *im);
    if (!im) return 0;
    im->magic = PX_MAGIC;
    im->refs = 1;
    return im;
}

pixman_image_t *px_image_create_bits(const struct px_env *e, pixman_format_code_t format, int w, int h,
                                     uint32_t *bits, int stride) {
    struct px_image *im;
    int bpp, ofmt = ogpu_format(format, &bpp);
    if (!ofmt || w < 0 || h < 0 || w > OGPU_MAX_SIZE || h > OGPU_MAX_SIZE) return 0;
    if (bits && (stride < w * bpp || (stride & 3))) return 0;
    if (!(im = new_image(e))) return 0;
    im->kind = PX_BITS; im->format = format; im->ofmt = ofmt; im->bpp = bpp; im->w = w; im->h = h;
    if (!bits && w && h) {
        stride = (w * bpp + 3) & ~3;
        if (!(bits = e->alloc(e->ctx, (unsigned long)stride * (unsigned long)h))) { e->free(e->ctx, im); return 0; }
        im->own = 1;
    } else if (!bits) stride = 0;
    im->bits = bits; im->stride = stride;
    return (pixman_image_t *)im;
}

pixman_image_t *px_image_create_solid(const struct px_env *e, const pixman_color_t *color) {
    struct px_image *im;
    if (!color || !(im = new_image(e))) return 0;
    im->kind = PX_SOLID;
    im->argb = argb_of(color);
    return (pixman_image_t *)im;
}

pixman_image_t *px_image_ref(pixman_image_t *image) {
    if (!is_image(image)) return 0;
    IMG(image)->refs++;
    return image;
}

int px_image_unref(const struct px_env *e, pixman_image_t *image) {
    struct px_image *im = IMG(image);
    if (!is_image(image)) return 0;
    if (--im->refs > 0) return 0;
    if (im->own) e->free(e->ctx, im->bits);
    im->magic = 0;
    e->free(e->ctx, im);
    return 1;
}

int px_image_set_transform(pixman_image_t *image, const pixman_transform_t *t) {
    struct px_image *im = IMG(image);
    int i, j, identity = 1;
    if (!is_image(image)) return 0;
    if (!t) { im->has_transform = 0; return 1; }
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            if (t->matrix[i][j] != (i == j ? pixman_fixed_1 : 0)) identity = 0;
    im->transform = *t;
    im->has_transform = !identity;
    return 1;
}

void px_image_set_repeat(pixman_image_t *image, int repeat) { if (is_image(image)) IMG(image)->repeat = repeat; }

int px_image_set_filter(pixman_image_t *image, int filter, const pixman_fixed_t *params, int n) {
    (void)params; (void)n;
    if (!is_image(image)) return 0;
    IMG(image)->filter = filter;
    return 1;
}

void px_image_set_component_alpha(pixman_image_t *image, int on) { if (is_image(image)) IMG(image)->component_alpha = on != 0; }
uint32_t *px_image_get_data(pixman_image_t *image) { return is_image(image) ? IMG(image)->bits : 0; }
int px_image_get_width(pixman_image_t *image) { return is_image(image) ? IMG(image)->w : 0; }
int px_image_get_height(pixman_image_t *image) { return is_image(image) ? IMG(image)->h : 0; }
int px_image_get_stride(pixman_image_t *image) { return is_image(image) ? IMG(image)->stride : 0; }
unsigned long px_image_get_format(pixman_image_t *image) {
    return is_image(image) && IMG(image)->kind == PX_BITS ? (unsigned long)IMG(image)->format : 0;
}

/* ---- batches ---------------------------------------------------------------------- */

#define BATCH_WORDS 256

static void send(const struct px_env *e, struct px_stats *st, struct OGPUBatch *b) {
    long r;
    if (!b->words) return;
    r = e->submit(e->ctx, b->buf, b->words);
    st->batches++;
    if (r != OGPU_OK) st->errors++;
    b->words = 0;
}

static void surface_of(const struct px_env *e, struct OGPUBatch *b, int slot, const struct px_image *im) {
    ogpu_surface(b, slot, e->address(e->ctx, im->bits), (unsigned long)im->stride, im->w, im->h, im->ofmt);
}

/* An integer translation (or none): its offset in *tx, *ty. 0 for any
 * other transform, which GFOpenGPU can't sample yet (DESIGN.md, "Gaps"). */
static int translation(const struct px_image *im, int *tx, int *ty) {
    const pixman_fixed_t (*m)[3] = im->transform.matrix;
    *tx = *ty = 0;
    if (!im->has_transform) return 1;
    if (m[0][0] != pixman_fixed_1 || m[1][1] != pixman_fixed_1 || m[2][2] != pixman_fixed_1 ||
        m[0][1] || m[1][0] || m[2][0] || m[2][1] || (m[0][2] & 0xFFFF) || (m[1][2] & 0xFFFF))
        return 0;
    *tx = (int)(m[0][2] >> 16);
    *ty = (int)(m[1][2] >> 16);
    return 1;
}

static int wrap(int v, int n) { v %= n; return v < 0 ? v + n : v; }

/* A source or mask, for COMPOSITE_PD: *word is its slot or colour or alpha,
 * *flags its OGPU_PD_ flags, *sx, *sy where the clipped rectangle's corner
 * reads it. 0 when GFOpenGPU can't take it. */
static int operand(const struct px_image *im, int is_mask, int slot, int x, int y, int w, int h,
                   unsigned long *word, unsigned long *flags, int *sx, int *sy) {
    int tx, ty;
    if (im->kind == PX_SOLID) {
        if (is_mask) {
            *word = im->argb >> 24;
            *flags |= OGPU_PD_MASK_SOLID;
        } else {
            *word = im->argb;
            *flags |= OGPU_PD_SOLID;
        }
        *sx = *sy = 0;
        return 1;
    }
    if (im->component_alpha && is_mask) return 0;
    if (!translation(im, &tx, &ty)) return 0;
    if (im->repeat != PIXMAN_REPEAT_NONE && im->repeat != PIXMAN_REPEAT_NORMAL) return 0;
    x += tx; y += ty;
    if (!im->w || !im->h) {                 /* nothing to read: transparent */
        if (is_mask) { *word = 0; *flags |= OGPU_PD_MASK_SOLID; } else { *word = 0; *flags |= OGPU_PD_SOLID; }
        *sx = *sy = 0;
        return 1;
    }
    if (im->repeat == PIXMAN_REPEAT_NORMAL) {
        x = wrap(x, im->w); y = wrap(y, im->h);
        *flags |= is_mask ? OGPU_PD_MASK_REPEAT : OGPU_PD_SRC_REPEAT;
    } else if (x >= im->w || y >= im->h || x + w <= 0 || y + h <= 0) {
        /* wholly outside: transparent, which also keeps the coordinates in 16 bits */
        if (is_mask) { *word = 0; *flags |= OGPU_PD_MASK_SOLID; } else { *word = 0; *flags |= OGPU_PD_SOLID; }
        *sx = *sy = 0;
        return 1;
    }
    *word = (unsigned long)slot;
    if (is_mask) *flags |= OGPU_PD_MASK;
    *sx = x; *sy = y;
    return 1;
}

void px_composite32(const struct px_env *e, struct px_stats *st, int op, pixman_image_t *src,
                    pixman_image_t *mask, pixman_image_t *dest, int src_x, int src_y,
                    int mask_x, int mask_y, int dest_x, int dest_y, int width, int height) {
    struct px_image *s = IMG(src), *m = IMG(mask), *d = IMG(dest);
    unsigned long buf[64], sword = 0, mword = 0, flags;
    struct OGPUBatch b;
    int x0, y0, x1, y1, sx, sy, mx = 0, my = 0, w, h;
    if (!is_image(src) || !is_image(dest) || d->kind != PX_BITS || (mask && !is_image(mask))) { st->refused++; return; }
    if (op < 0 || op >= OGPU_PD_OPS) { st->refused++; return; }         /* SATURATE, disjoint, conjoint, PDF */
    /* the rectangle on the destination */
    x0 = dest_x < 0 ? 0 : dest_x; y0 = dest_y < 0 ? 0 : dest_y;
    x1 = (long)dest_x + width > d->w ? d->w : dest_x + width;
    y1 = (long)dest_y + height > d->h ? d->h : dest_y + height;
    if (width <= 0 || height <= 0 || x1 <= x0 || y1 <= y0 || op == PIXMAN_OP_DST) return;
    w = x1 - x0; h = y1 - y0;
    ogpu_batch_init(&b, buf, 64);
    surface_of(e, &b, 0, d);
    ogpu_target(&b, 0);
    flags = (unsigned long)op;
    if (!operand(s, 0, 1, src_x + (x0 - dest_x), src_y + (y0 - dest_y), w, h, &sword, &flags, &sx, &sy)) { st->refused++; return; }
    if (m && !operand(m, 1, 2, mask_x + (x0 - dest_x), mask_y + (y0 - dest_y), w, h, &mword, &flags, &mx, &my)) { st->refused++; return; }
    /* a solid source with nothing (or an opaque solid) for a mask, SRC or
     * OVER-opaque or CLEAR: a FILL, which every back end has */
    if ((flags & OGPU_PD_SOLID) && (!(flags & (OGPU_PD_MASK | OGPU_PD_MASK_SOLID)) || ((flags & OGPU_PD_MASK_SOLID) && mword == 255)) &&
        (op == PIXMAN_OP_SRC || op == PIXMAN_OP_CLEAR || (op == PIXMAN_OP_OVER && (sword >> 24) == 255))) {
        ogpu_fill(&b, x0, y0, w, h, op == PIXMAN_OP_CLEAR ? 0 : raw_of(sword, d->ofmt));
        send(e, st, &b);
        return;
    }
    /* the same format 1:1, wholly inside the source, SRC (or OVER from a
     * format with no alpha): a COPY */
    if (!(flags & (OGPU_PD_SOLID | OGPU_PD_SRC_REPEAT | OGPU_PD_MASK | OGPU_PD_MASK_SOLID)) && s->ofmt == d->ofmt &&
        sx >= 0 && sy >= 0 && sx + w <= s->w && sy + h <= s->h &&
        (op == PIXMAN_OP_SRC || (op == PIXMAN_OP_OVER && PIXMAN_FORMAT_A(s->format) == 0))) {
        int slot = 1;
        if (s->bits == d->bits && s->stride == d->stride) slot = 0;
        else surface_of(e, &b, 1, s);
        ogpu_copy(&b, slot, sx, sy, x0, y0, w, h);
        send(e, st, &b);
        return;
    }
    if (!(flags & OGPU_PD_SOLID)) surface_of(e, &b, 1, s);
    if (flags & OGPU_PD_MASK) surface_of(e, &b, 2, m);
    ogpu_composite_pd(&b, flags, sword, sx, sy, mword, mx, my, x0, y0, w, h);
    send(e, st, &b);
}

/* ---- fill, blt, rectangles -------------------------------------------------------- */

static int raw_format(int bpp) {
    return bpp == 8 ? OGPU_FMT_CLUT8 : bpp == 16 ? OGPU_FMT_RGB565 : bpp == 32 ? OGPU_FMT_ARGB32 : 0;
}

int px_fill(const struct px_env *e, struct px_stats *st, uint32_t *bits, int stride, int bpp,
            int x, int y, int w, int h, uint32_t value) {
    unsigned long buf[16];
    struct OGPUBatch b;
    int f = raw_format(bpp), rowpx;
    if (!bits || !f || stride <= 0 || x < 0 || y < 0 || w < 0 || h < 0) { st->refused++; return 0; }
    if (!w || !h) return 1;
    rowpx = stride * 4 / (bpp / 8);
    if (x + w > rowpx || x + w > OGPU_MAX_SIZE || y + h > OGPU_MAX_SIZE) { st->refused++; return 0; }
    ogpu_batch_init(&b, buf, 16);
    /* one surface from row 0 to the last row drawn, as wide as a row */
    ogpu_surface(&b, 0, e->address(e->ctx, bits), (unsigned long)stride * 4, rowpx > OGPU_MAX_SIZE ? OGPU_MAX_SIZE : rowpx, y + h, f);
    ogpu_target(&b, 0);
    ogpu_fill(&b, x, y, w, h, bpp == 32 ? value : bpp == 16 ? (value & 0xFFFF) : (value & 0xFF));
    send(e, st, &b);
    return 1;
}

int px_blt(const struct px_env *e, struct px_stats *st, uint32_t *src_bits, uint32_t *dst_bits,
           int src_stride, int dst_stride, int src_bpp, int dst_bpp,
           int src_x, int src_y, int dest_x, int dest_y, int w, int h) {
    unsigned long buf[24];
    struct OGPUBatch b;
    int f = raw_format(src_bpp), spx, dpx, slot = 1;
    if (!src_bits || !dst_bits || src_bpp != dst_bpp || !f || src_stride <= 0 || dst_stride <= 0 ||
        src_x < 0 || src_y < 0 || dest_x < 0 || dest_y < 0 || w <= 0 || h <= 0) { st->refused++; return 0; }
    spx = src_stride * 4 / (src_bpp / 8);
    dpx = dst_stride * 4 / (dst_bpp / 8);
    if (src_x + w > spx || dest_x + w > dpx || src_x + w > OGPU_MAX_SIZE || dest_x + w > OGPU_MAX_SIZE ||
        src_y + h > OGPU_MAX_SIZE || dest_y + h > OGPU_MAX_SIZE) { st->refused++; return 0; }
    ogpu_batch_init(&b, buf, 24);
    ogpu_surface(&b, 0, e->address(e->ctx, dst_bits), (unsigned long)dst_stride * 4, dest_x + w, dest_y + h, f);
    if (src_bits == dst_bits && src_stride == dst_stride) {
        /* one buffer: one slot, so COPY sees the overlap; as wide and tall as both rectangles need */
        int ww = (src_x > dest_x ? src_x : dest_x) + w, hh = (src_y > dest_y ? src_y : dest_y) + h;
        b.words = 0;
        ogpu_surface(&b, 0, e->address(e->ctx, dst_bits), (unsigned long)dst_stride * 4, ww, hh, f);
        slot = 0;
    } else ogpu_surface(&b, 1, e->address(e->ctx, src_bits), (unsigned long)src_stride * 4, src_x + w, src_y + h, f);
    ogpu_target(&b, 0);
    ogpu_copy(&b, slot, src_x, src_y, dest_x, dest_y, w, h);
    send(e, st, &b);
    return 1;
}

int px_fill_rectangles(const struct px_env *e, struct px_stats *st, int op, pixman_image_t *image,
                       const pixman_color_t *color, int n, const pixman_rectangle16_t *rects) {
    struct px_image *d = IMG(image);
    unsigned long buf[BATCH_WORDS], argb;
    struct OGPUBatch b;
    int i, fill;
    if (!is_image(image) || d->kind != PX_BITS || !color || n < 0 || (n && !rects)) { st->refused++; return 0; }
    if (op < 0 || op >= OGPU_PD_OPS) { st->refused++; return 0; }
    if (op == PIXMAN_OP_DST || !d->w || !d->h) return 1;
    argb = argb_of(color);
    /* as pixman: CLEAR is SRC of nothing, OVER of an opaque colour is SRC */
    if (op == PIXMAN_OP_CLEAR) { argb = 0; op = PIXMAN_OP_SRC; }
    if (op == PIXMAN_OP_OVER && (argb >> 24) == 255) op = PIXMAN_OP_SRC;
    fill = op == PIXMAN_OP_SRC;
    ogpu_batch_init(&b, buf, BATCH_WORDS);
    for (i = 0; i < n; i++) {
        const pixman_rectangle16_t *r = &rects[i];
        if (!r->width || !r->height) continue;
        if (!b.words) { surface_of(e, &b, 0, d); ogpu_target(&b, 0); }
        if (fill) ogpu_fill(&b, r->x, r->y, r->width, r->height, raw_of(argb, d->ofmt));
        else ogpu_composite_pd(&b, (unsigned long)op | OGPU_PD_SOLID, argb, 0, 0, 0, 0, 0, r->x, r->y, r->width, r->height);
        if (b.words > BATCH_WORDS - 12) send(e, st, &b);
    }
    send(e, st, &b);
    return 1;
}
