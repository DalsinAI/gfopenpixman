/* Copyright (c) 2026 AmiCradle Team. MIT licence (LICENSE).
 *
 * pixman.library's core (DESIGN.md): Pixman's calls turned into GFOpenGPU
 * batches (stream v1.4, gfopenrtg include/gfopengpu/stream.h). It draws
 * nothing itself. Plain C with no Amiga headers, no C library and no
 * floating point, so the same file builds into the library and into the
 * test that runs it beside upstream Pixman (tests/pixmantest.c). */
#ifndef PX_CORE_H
#define PX_CORE_H

#include "../include/libraries/pixman.h"

/* What the core needs from where it runs. */
struct px_env {
    void *(*alloc)(void *ctx, unsigned long bytes);   /* cleared memory, or 0 */
    void (*free)(void *ctx, void *p);
    /* An address as GFOpenGPU's back ends see it (the Amiga's own). */
    unsigned long (*address)(void *ctx, const void *p);
    /* Run a batch of `words` stream words and wait for it: OGPU_OK, or an error. */
    long (*submit)(void *ctx, const void *stream, long words);
    void *ctx;
};

/* What the core counts (PX_DrawStats in a later ABI; the tests read it). */
struct px_stats {
    unsigned long batches;      /* sent to GFOpenGPU */
    unsigned long refused;      /* calls the core can't hand over (DESIGN.md, "Gaps") */
    unsigned long errors;       /* batches GFOpenGPU answered with an error */
};

pixman_image_t *px_image_create_bits(const struct px_env *e, pixman_format_code_t format, int w, int h,
                                     uint32_t *bits, int stride);
pixman_image_t *px_image_create_solid(const struct px_env *e, const pixman_color_t *color);
pixman_image_t *px_image_ref(pixman_image_t *image);
int px_image_unref(const struct px_env *e, pixman_image_t *image);
int px_image_set_transform(pixman_image_t *image, const pixman_transform_t *t);
void px_image_set_repeat(pixman_image_t *image, int repeat);
int px_image_set_filter(pixman_image_t *image, int filter, const pixman_fixed_t *params, int n);
void px_image_set_component_alpha(pixman_image_t *image, int on);
uint32_t *px_image_get_data(pixman_image_t *image);
int px_image_get_width(pixman_image_t *image);
int px_image_get_height(pixman_image_t *image);
int px_image_get_stride(pixman_image_t *image);
unsigned long px_image_get_format(pixman_image_t *image);

void px_composite32(const struct px_env *e, struct px_stats *st, int op, pixman_image_t *src,
                    pixman_image_t *mask, pixman_image_t *dest, int src_x, int src_y,
                    int mask_x, int mask_y, int dest_x, int dest_y, int width, int height);
int px_fill(const struct px_env *e, struct px_stats *st, uint32_t *bits, int stride, int bpp,
            int x, int y, int w, int h, uint32_t value);
int px_blt(const struct px_env *e, struct px_stats *st, uint32_t *src_bits, uint32_t *dst_bits,
           int src_stride, int dst_stride, int src_bpp, int dst_bpp,
           int src_x, int src_y, int dest_x, int dest_y, int w, int h);
int px_fill_rectangles(const struct px_env *e, struct px_stats *st, int op, pixman_image_t *image,
                       const pixman_color_t *color, int n, const pixman_rectangle16_t *rects);

/* px_region.c: Pixman's region32 calls, in its own data layout. */
void px_region32_init(pixman_region32_t *r);
void px_region32_init_rect(pixman_region32_t *r, int x, int y, unsigned w, unsigned h);
void px_region32_fini(const struct px_env *e, pixman_region32_t *r);
int px_region32_copy(const struct px_env *e, pixman_region32_t *dst, const pixman_region32_t *src);
int px_region32_intersect(const struct px_env *e, pixman_region32_t *dst, const pixman_region32_t *a,
                          const pixman_region32_t *b);
int px_region32_union(const struct px_env *e, pixman_region32_t *dst, const pixman_region32_t *a,
                      const pixman_region32_t *b);

#endif
