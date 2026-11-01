/* Copyright (c) 2026 AmigaChrome Team. OpenRTG, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 *
 * Building an OGPU stream v1.2 (stream.h) in memory. Link library code: the
 * calls write words into the caller's buffer and never call gfopengpu.library,
 * so building a batch costs no library calls; OGPU_Submit sends it.
 * A batch that runs out of room sets `overflow` and stops growing. */
#ifndef OPENGPU_BUILD_H
#define OPENGPU_BUILD_H

#include "stream.h"

struct OGPUBatch {
    unsigned char *buf;     /* the stream */
    long cap;               /* words it can hold */
    long words;             /* words written */
    int overflow;           /* a command didn't fit, and was left out */
};

void ogpu_batch_init(struct OGPUBatch *b, void *buf, long cap_words);
void ogpu_surface(struct OGPUBatch *b, int slot, unsigned long address, unsigned long bpr, int w, int h, int format);
void ogpu_target(struct OGPUBatch *b, int slot);
void ogpu_clip(struct OGPUBatch *b, int x, int y, int w, int h);
void ogpu_fill(struct OGPUBatch *b, int x, int y, int w, int h, unsigned long colour);
void ogpu_invert(struct OGPUBatch *b, int x, int y, int w, int h, unsigned long mask);
void ogpu_copy(struct OGPUBatch *b, int src_slot, int sx, int sy, int x, int y, int w, int h);
void ogpu_template(struct OGPUBatch *b, unsigned long address, unsigned long bpr, unsigned long first_bit,
                   int x, int y, int w, int h, unsigned long fg, unsigned long bg, int mode);
void ogpu_pattern(struct OGPUBatch *b, unsigned long address, unsigned long rows,
                  int x, int y, int w, int h, unsigned long fg, unsigned long bg, int mode);
void ogpu_line(struct OGPUBatch *b, int x0, int y0, int x1, int y1, unsigned long fg, int mode);
void ogpu_pixels(struct OGPUBatch *b, unsigned long address, unsigned long bpr, int format, unsigned long table,
                 int x, int y, int w, int h);
void ogpu_composite(struct OGPUBatch *b, int src_slot, int sx, int sy, int sw, int sh,
                    int x, int y, int w, int h, int alpha, unsigned long flags);
/* v1.1 */
void ogpu_composite_masked(struct OGPUBatch *b, int src_slot, int sx, int sy, int sw, int sh,
                           int x, int y, int w, int h, int alpha, unsigned long flags,
                           int mask_slot, int mx, int my);
void ogpu_mask(struct OGPUBatch *b, unsigned long address, unsigned long bpr, int x, int y, int w, int h,
               unsigned long colour);
void ogpu_fence(struct OGPUBatch *b, unsigned long id);
/* A block of ACVirgl commands at address for the host's virglrenderer; ask
 * OGPU_Query(OGPU_OP_VIRGL, 0) first. */
void ogpu_virgl(struct OGPUBatch *b, unsigned long address, unsigned long bytes);
/* v1.2: SDL 2's renderer (stream.h). Ask OGPU_Query first. The 3D builders
 * (TRIANGLES and the rest) are in build3d.h. */
void ogpu_fill_blend(struct OGPUBatch *b, int x, int y, int w, int h, unsigned long argb, int mode);
void ogpu_lines_blend(struct OGPUBatch *b, unsigned long points, unsigned long count, unsigned long argb, unsigned long flags);
void ogpu_points_blend(struct OGPUBatch *b, unsigned long points, unsigned long count, unsigned long argb, int mode);
/* m: m00, m01, m02, m10, m11, m12, 16.16 (source to target) */
void ogpu_composite_affine(struct OGPUBatch *b, int src_slot, int sx, int sy, int sw, int sh,
                           const long m[6], unsigned long argb, unsigned long flags);
/* v1.4: Porter-Duff compositing of premultiplied colour (stream.h). src is a
 * slot, or the colour with OGPU_PD_SOLID; mask a slot (OGPU_PD_MASK), an
 * alpha (OGPU_PD_MASK_SOLID) or unused. Ask OGPU_Query first. */
void ogpu_composite_pd(struct OGPUBatch *b, unsigned long op_flags, unsigned long src, int sx, int sy,
                       unsigned long mask, int mx, int my, int x, int y, int w, int h);
void ogpu_yuv(struct OGPUBatch *b, unsigned long y_address, unsigned long y_bpr, unsigned long u_address, unsigned long u_bpr,
              unsigned long v_address, unsigned long v_bpr, int w, int h, int x, int y, unsigned long format);

#endif
