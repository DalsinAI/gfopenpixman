/* Copyright (c) 2026 AmigaChrome Team. MIT licence (LICENSE).
 *
 * pixman.library's ABI. From 2.0 the library is a thin layer over
 * GFOpenGPU (DESIGN.md): no upstream Pixman inside. The types below are
 * Pixman 0.46.4's public interface (names, values and layouts, written here
 * from its documented API), so code written against Pixman's types builds
 * against this header unchanged. A file that includes upstream's
 * <pixman.h> first gets upstream's own definitions, which are the same. */
#ifndef LIBRARIES_PIXMAN_H
#define LIBRARIES_PIXMAN_H

#ifdef __amigaos__
#include <exec/types.h>
#endif
#include <stdint.h>

#ifndef PIXMAN_H__
/* ---- Pixman 0.46.4's public types --------------------------------------------- */

typedef int pixman_bool_t;
typedef int32_t pixman_fixed_t;
typedef int64_t pixman_fixed_48_16_t;

#define pixman_fixed_1          ((pixman_fixed_t)0x10000)
#define pixman_int_to_fixed(i)  ((pixman_fixed_t)((uint32_t)(i) << 16))
#define pixman_fixed_to_int(f)  ((int)((f) >> 16))

typedef struct pixman_color { uint16_t red, green, blue, alpha; } pixman_color_t;
typedef struct pixman_transform { pixman_fixed_t matrix[3][3]; } pixman_transform_t;
typedef struct pixman_rectangle16 { int16_t x, y; uint16_t width, height; } pixman_rectangle16_t;
typedef struct pixman_box32 { int32_t x1, y1, x2, y2; } pixman_box32_t;
typedef struct pixman_region32_data { long size; long numRects; /* pixman_box32_t rects[size] follow */ } pixman_region32_data_t;
typedef struct pixman_region32 { pixman_box32_t extents; pixman_region32_data_t *data; } pixman_region32_t;

typedef union pixman_image pixman_image_t;     /* opaque */

typedef enum {
    PIXMAN_REPEAT_NONE, PIXMAN_REPEAT_NORMAL, PIXMAN_REPEAT_PAD, PIXMAN_REPEAT_REFLECT
} pixman_repeat_t;

typedef enum {
    PIXMAN_FILTER_FAST, PIXMAN_FILTER_GOOD, PIXMAN_FILTER_BEST, PIXMAN_FILTER_NEAREST,
    PIXMAN_FILTER_BILINEAR, PIXMAN_FILTER_CONVOLUTION, PIXMAN_FILTER_SEPARABLE_CONVOLUTION
} pixman_filter_t;

typedef enum {
    PIXMAN_OP_CLEAR = 0x00, PIXMAN_OP_SRC = 0x01, PIXMAN_OP_DST = 0x02, PIXMAN_OP_OVER = 0x03,
    PIXMAN_OP_OVER_REVERSE = 0x04, PIXMAN_OP_IN = 0x05, PIXMAN_OP_IN_REVERSE = 0x06,
    PIXMAN_OP_OUT = 0x07, PIXMAN_OP_OUT_REVERSE = 0x08, PIXMAN_OP_ATOP = 0x09,
    PIXMAN_OP_ATOP_REVERSE = 0x0a, PIXMAN_OP_XOR = 0x0b, PIXMAN_OP_ADD = 0x0c,
    PIXMAN_OP_SATURATE = 0x0d,
    PIXMAN_OP_DISJOINT_CLEAR = 0x10, PIXMAN_OP_CONJOINT_CLEAR = 0x20,
    PIXMAN_OP_MULTIPLY = 0x30, PIXMAN_OP_HSL_LUMINOSITY = 0x3e
} pixman_op_t;

#define PIXMAN_FORMAT(bpp, type, a, r, g, b) \
    (((bpp) << 24) | ((type) << 16) | ((a) << 12) | ((r) << 8) | ((g) << 4) | (b))
#define PIXMAN_FORMAT_BPP(f)    (((f) >> 24) & 0xff)
#define PIXMAN_FORMAT_TYPE(f)   (((f) >> 16) & 0x3f)
#define PIXMAN_FORMAT_A(f)      (((f) >> 12) & 0x0f)
#define PIXMAN_TYPE_OTHER   0
#define PIXMAN_TYPE_A       1
#define PIXMAN_TYPE_ARGB    2
#define PIXMAN_TYPE_ABGR    3
#define PIXMAN_TYPE_COLOR   4
#define PIXMAN_TYPE_GRAY    5
#define PIXMAN_TYPE_BGRA    8
#define PIXMAN_TYPE_RGBA    9

/* The formats pixman.library 2.0 draws (DESIGN.md, "Formats"); the rest
 * of Pixman's list is accepted by name and refused at ImageCreateBits. */
typedef enum {
    PIXMAN_a8r8g8b8 = PIXMAN_FORMAT(32, PIXMAN_TYPE_ARGB, 8, 8, 8, 8),
    PIXMAN_x8r8g8b8 = PIXMAN_FORMAT(32, PIXMAN_TYPE_ARGB, 0, 8, 8, 8),
    PIXMAN_a8b8g8r8 = PIXMAN_FORMAT(32, PIXMAN_TYPE_ABGR, 8, 8, 8, 8),
    PIXMAN_x8b8g8r8 = PIXMAN_FORMAT(32, PIXMAN_TYPE_ABGR, 0, 8, 8, 8),
    PIXMAN_b8g8r8a8 = PIXMAN_FORMAT(32, PIXMAN_TYPE_BGRA, 8, 8, 8, 8),
    PIXMAN_b8g8r8x8 = PIXMAN_FORMAT(32, PIXMAN_TYPE_BGRA, 0, 8, 8, 8),
    PIXMAN_r8g8b8 = PIXMAN_FORMAT(24, PIXMAN_TYPE_ARGB, 0, 8, 8, 8),
    PIXMAN_r5g6b5 = PIXMAN_FORMAT(16, PIXMAN_TYPE_ARGB, 0, 5, 6, 5),
    PIXMAN_b5g6r5 = PIXMAN_FORMAT(16, PIXMAN_TYPE_ABGR, 0, 5, 6, 5),
    PIXMAN_a1r5g5b5 = PIXMAN_FORMAT(16, PIXMAN_TYPE_ARGB, 1, 5, 5, 5),
    PIXMAN_x1r5g5b5 = PIXMAN_FORMAT(16, PIXMAN_TYPE_ARGB, 0, 5, 5, 5),
    PIXMAN_a8 = PIXMAN_FORMAT(8, PIXMAN_TYPE_A, 8, 0, 0, 0),
    PIXMAN_a4 = PIXMAN_FORMAT(4, PIXMAN_TYPE_A, 4, 0, 0, 0),
    PIXMAN_a1 = PIXMAN_FORMAT(1, PIXMAN_TYPE_A, 1, 0, 0, 0)
} pixman_format_code_t;
#endif /* PIXMAN_H__ */

#ifndef __amigaos__
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int16_t BOOL;
typedef const unsigned char *CONST_STRPTR;
#endif

/* ---- pixman.library ---------------------------------------------------------------- */

#define PIXMANLIB_NAME "pixman.library"
#define PIXMANLIB_VERSION 2
#define PIXMANLIB_REVISION 1

struct PXCreateBits {
    pixman_format_code_t format;
    LONG width;
    LONG height;
    uint32_t *bits;             /* NULL: the library allocates them, cleared */
    LONG rowstride_bytes;
};

struct PXComposite32 {
    pixman_op_t op;
    pixman_image_t *src;
    pixman_image_t *mask;
    pixman_image_t *dest;
    LONG src_x, src_y;
    LONG mask_x, mask_y;
    LONG dest_x, dest_y;
    LONG width, height;
};

struct PXFill {
    uint32_t *bits;
    LONG stride;                /* in uint32_t, as pixman_fill's */
    LONG bpp;
    LONG x, y, width, height;
    uint32_t xor_value;         /* the pixel value written (pixman_fill's name for it) */
};

struct PXBlt {
    uint32_t *src_bits;
    uint32_t *dst_bits;
    LONG src_stride;            /* in uint32_t */
    LONG dst_stride;
    LONG src_bpp;
    LONG dst_bpp;
    LONG src_x, src_y;
    LONG dest_x, dest_y;
    LONG width, height;
};

struct PXFilter {
    pixman_image_t *image;
    pixman_filter_t filter;
    const pixman_fixed_t *params;
    LONG n_params;
};

struct PXFillRects {
    pixman_op_t op;
    pixman_image_t *image;
    const pixman_color_t *color;
    LONG n_rects;
    const pixman_rectangle16_t *rects;
};

#endif
