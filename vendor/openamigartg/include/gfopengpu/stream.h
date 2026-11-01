/* Copyright (c) 2026 AmigaChrome Team. OpenRTG, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 *
 * OGPU stream v1.3: the bytes every GFOpenGPU back end receives (DESIGN.md
 * section 5). v1.1 added A8 coverage masks for anti-aliased text and clip
 * paths (GFOpenGfx): the A8 format, MASK, and COMPOSITE's MASK, ADD and IN
 * flags. v1.2 (8 October 2026) adds what SDL 2's renderer and Warp3D need:
 * blend modes (as SDL's), FILL_BLEND, LINES_BLEND, POINTS_BLEND,
 * COMPOSITE_AFFINE (scale, rotate, flip, colour modulation), YUV, the
 * formats of real cards' screens, and the 3D range 0x0030-0x003F, whose
 * layouts are in stream3d.h (TRIANGLES serves SDL_RenderGeometry and
 * Warp3D alike). An older back end answers OGPU_NONE to OGPU_Query for
 * anything newer than it, so a program asks before it uses any of them.
 * v1.3 (12 October 2026) adds XRGB32, ARGB32's colour without its alpha: a
 * 32-bit GFOpenRTG screen's pixels, whose top byte is always 0, so ARGB
 * pixels reach one through PIXELS on the GPU, not the 68k.
 * v1.4 (17 October 2026) adds COMPOSITE_PD: Porter-Duff compositing of
 * premultiplied colour, with pixman's operators and rounding, so
 * pixman.library hands its composites to GFOpenGPU. The 68k's own CPU back end, ACRTG.gpu on the Cradle and
 * PiStorm.gpu on a spare ARM core all read exactly this, so they draw the
 * same pixels.
 *
 * A stream is 32-bit words, big-endian (the Amiga's order) wherever it is
 * read. Each command starts with a header word: the opcode in the top 16
 * bits and the command's length in words, header included, in the bottom 16.
 * A back end that meets an opcode it doesn't know skips it by its length
 * and reports OGPU_ERR_BADOP, so newer streams fail one command, not all.
 *
 * Points and sizes are packed in one word: x (or width) in the top 16 bits,
 * y (or height) in the bottom 16, signed for points.
 *
 * Addresses are the Amiga's own. Each back end maps them to its memory: the
 * CPU back end uses them as they are, the Cradle through the guest's RAM
 * and video RAM, the PiStorm through Emu68's view of fast RAM.
 *
 * Pixels in memory are in the Amiga's byte order: RGB565 high byte first,
 * ARGB32 as the bytes A, R, G, B. Colours in commands are raw pixel values
 * in the target's format (a pen for CLUT8). */
#ifndef OPENGPU_STREAM_H
#define OPENGPU_STREAM_H

#define OGPU_STREAM_VERSION 1
#define OGPU_STREAM_MINOR   4

#define OGPU_HDR(op, words)     (((unsigned long)(op) << 16) | ((unsigned long)(words) & 0xFFFFUL))
#define OGPU_HDR_OP(w)          ((unsigned)((w) >> 16))
#define OGPU_HDR_WORDS(w)       ((unsigned)((w) & 0xFFFFUL))
#define OGPU_XY(x, y)           ((((unsigned long)(x) & 0xFFFFUL) << 16) | ((unsigned long)(y) & 0xFFFFUL))

/* Pixel formats (surfaces and PIXELS sources). */
#define OGPU_FMT_CLUT8          1   /* 1 byte: a pen */
#define OGPU_FMT_RGB565         2   /* 2 bytes, high byte first */
#define OGPU_FMT_ARGB32         3   /* 4 bytes: A, R, G, B */
#define OGPU_FMT_INDEX8         4   /* PIXELS sources only: 1 byte, looked up in a 256-entry ARGB table */
#define OGPU_FMT_A8             5   /* v1.1: 1 byte: coverage (alpha) 0-255, no colour */
/* v1.2: the screens of real cards and of a PiStorm's Picasso96, as targets
 * and sources; ACRTG needs only RGB565 and ARGB32. */
#define OGPU_FMT_RGB565PC       6   /* 2 bytes, low byte first */
#define OGPU_FMT_RGB555         7   /* 2 bytes, high byte first: 0RRRRRGG GGGBBBBB */
#define OGPU_FMT_RGB555PC       8   /* 2 bytes, low byte first */
#define OGPU_FMT_BGRA32         9   /* 4 bytes: B, G, R, A */
/* v1.3: ARGB32 without the alpha. Every write sets X to 0 and every read
 * takes the pixel as opaque, so a PIXELS of ARGB32 (or INDEX8) into an
 * XRGB32 target copies the colour and forces the top byte to 0 (an XRGB32
 * source's X is cleared too). COPY, which moves raw pixels, keeps them. */
#define OGPU_FMT_XRGB32         10  /* 4 bytes: X (0), R, G, B */
#define OGPU_FMT_COUNT          11
/* 16-18 (Z16, Z32, S8: depth and stencil) are stream3d.h's. */

/* Draw modes, as graphics.library's (JAM1, JAM2, COMPLEMENT, INVERSVID). */
#define OGPU_JAM1               0
#define OGPU_JAM2               1
#define OGPU_COMPLEMENT         2
#define OGPU_INVERSVID          4

/* Opcodes. Words after the header, in order:                                      words */
#define OGPU_OP_NOP             0x0000  /* (any padding)                                 1+ */
#define OGPU_OP_SURFACE         0x0001  /* slot, address, bytes per row, wh, format      6  */
#define OGPU_OP_TARGET          0x0002  /* slot: where the next commands draw            2  */
#define OGPU_OP_CLIP            0x0003  /* xy, wh: drawing stays inside; wh 0 = whole     3  */
#define OGPU_OP_FILL            0x0010  /* xy, wh, colour                                4  */
#define OGPU_OP_INVERT          0x0011  /* xy, wh, mask (pixel ^= mask)                  4  */
#define OGPU_OP_COPY            0x0012  /* source slot, source xy, xy, wh (overlap safe) 5  */
#define OGPU_OP_TEMPLATE        0x0013  /* address, bytes per row, first bit, xy, wh,
                                           fg, bg, mode                                  9  */
#define OGPU_OP_PATTERN         0x0014  /* address, rows (1, 2, 4 .. 256), xy, wh,
                                           fg, bg, mode                                  8  */
#define OGPU_OP_LINE            0x0015  /* xy0, xy1, fg, mode (both ends drawn)          5  */
#define OGPU_OP_PIXELS          0x0016  /* address, bytes per row, format, table, xy, wh 7  */
#define OGPU_OP_MASK            0x0017  /* v1.1: address, bytes per row, xy, wh, colour  6
                                           (an A8 mask in memory; colour is ARGB32)      */
#define OGPU_OP_FILL_BLEND      0x0018  /* v1.2: xy, wh, colour (ARGB32), blend           5  */
#define OGPU_OP_LINES_BLEND     0x0019  /* v1.2: address of points (one OGPU_XY word
                                           each), count, colour (ARGB32), flags          5  */
#define OGPU_OP_POINTS_BLEND    0x001A  /* v1.2: address of points, count, colour, blend  5  */
#define OGPU_OP_COMPOSITE       0x0020  /* source slot, source xy, source wh, xy, wh,
                                           alpha (0-255), flags                          8  */
                                        /* with OGPU_COMP_MASK, then: mask slot, mask xy 10 */
#define OGPU_OP_COMPOSITE_AFFINE 0x0021 /* v1.2: source slot, source xy, source wh,
                                           m00, m01, m02, m10, m11, m12, colour, flags   12 */
#define OGPU_OP_YUV             0x0022  /* v1.2: Y address, Y bytes per row, U (or UV)
                                           address, its bytes per row, V address, its
                                           bytes per row, wh, xy, format                 10 */
#define OGPU_OP_COMPOSITE_PD    0x0023  /* v1.4: operator and flags, source (slot or
                                           colour), source xy, mask (slot or alpha),
                                           mask xy, xy, wh                               8  */
/* 0x0030-0x003F: 3D (TRIANGLES, RENDER3D, TEXENV, TEXTURE, DEPTH, CLEAR3D):
 * include/gfopengpu/stream3d.h. */
#define OGPU_OP_FENCE           0x00F0  /* id: reported when everything before is done   2  */
/* 0x0080-0x008F: GPU APIs carried through GFOpenGPU, run by a back end's
 * extension hook (ogpu_core.ext) in stream order; a back end without one
 * reports OGPU_ERR_BADOP, and OGPU_Query answers OGPU_NONE. */
#define OGPU_OP_VIRGL           0x0080  /* request address, request bytes: a block of
                                           ACVirgl commands for the host's virglrenderer 3  */
#define OGPU_OP_EXT_FIRST       0x0080
#define OGPU_OP_EXT_LAST        0x008F

/* OGPU_OP_COMPOSITE flags. */
#define OGPU_COMP_SRCALPHA      1   /* use the source's own alpha (ARGB32 sources) */
#define OGPU_COMP_BILINEAR      2   /* filter when scaling; nearest otherwise */
/* v1.1. Coverage (each pixel's source alpha times alpha, and times the mask)
 * is applied with OVER unless ADD or IN is set. */
#define OGPU_COMP_MASK          4   /* an A8 surface's coverage, read 1:1 against the destination rect */
#define OGPU_COMP_ADD           8   /* dst + src * coverage, each channel saturating at 255 */
#define OGPU_COMP_IN            16  /* dst * coverage: clips what is already there */
/* v1.2: one of the blend modes below, in bits 8-10, instead of OVER, ADD or IN. */
#define OGPU_COMP_BLENDMODE     128
#define OGPU_COMP_MODE(m)       ((unsigned long)(m) << 8)
#define OGPU_COMP_MODE_OF(f)    ((int)(((f) >> 8) & 7))

/* v1.2: blend modes, as SDL's (colours not premultiplied). FILL_BLEND,
 * POINTS_BLEND and COMPOSITE_AFFINE take one in their flags' bits 0-2, as
 * TRIANGLES does (stream3d.h's OGPU_TRI_BLEND_ are the same numbers); LINES_BLEND
 * too, beside its own flags; COMPOSITE with OGPU_COMP_BLENDMODE in bits 8-10.
 * The source's alpha is its own (with OGPU_COMP_SRCALPHA, where a source has
 * one) times the colour's or the alpha word's, times any mask. */
#define OGPU_BLEND_NONE         0   /* dst = src */
#define OGPU_BLEND_BLEND        1   /* dstRGB = srcRGB * srcA + dstRGB * (1 - srcA); dstA = srcA + dstA * (1 - srcA) */
#define OGPU_BLEND_ADD          2   /* dstRGB = dstRGB + srcRGB * srcA, saturating; dstA kept */
#define OGPU_BLEND_MOD          3   /* dstRGB = srcRGB * dstRGB; dstA kept */
#define OGPU_BLEND_MUL          4   /* dstRGB = srcRGB * dstRGB + dstRGB * (1 - srcA), saturating; dstA kept */
#define OGPU_BLEND_MASK         7

/* v1.4: COMPOSITE_PD. Colours are premultiplied (ARGB32's R, G and B are
 * already times A). The operator is in bits 0-7 of the first word, with
 * pixman's numbers. For each target pixel d in xy, wh (inside the clip):
 *   s = the source pixel at (source xy + offset), or the solid colour; a
 *       pixel outside the source is 0 unless SRC_REPEAT tiles it. XRGB32
 *       and the 16-bit formats read with alpha 255; A8 as (a, 0, 0, 0).
 *   with a mask: m = the mask pixel's alpha (an A8 or ARGB32 slot, 0 outside
 *       unless MASK_REPEAT), or the solid alpha; then each channel of s is
 *       s * m / 255, rounded.
 *   d = s * Fa + d * Fb, each product x * y / 255 rounded, the sum saturating
 *       at 255, with Fa and Fb by the operator (as = s's alpha, ad = d's):
 *       CLEAR 0, 0; SRC 1, 0; DST 0, 1; OVER 1, 1 - as; OVER_REVERSE 1 - ad, 1;
 *       IN ad, 0; IN_REVERSE 0, as; OUT 1 - ad, 0; OUT_REVERSE 0, 1 - as;
 *       ATOP ad, 1 - as; ATOP_REVERSE 1 - ad, as; XOR 1 - ad, 1 - as; ADD 1, 1.
 *   The pixel is then stored in the target's format (5 and 6-bit channels
 *   truncated, XRGB32's X as 0), as pixman stores it. */
#define OGPU_PD_CLEAR           0
#define OGPU_PD_SRC             1
#define OGPU_PD_DST             2
#define OGPU_PD_OVER            3
#define OGPU_PD_OVER_REVERSE    4
#define OGPU_PD_IN              5
#define OGPU_PD_IN_REVERSE      6
#define OGPU_PD_OUT             7
#define OGPU_PD_OUT_REVERSE     8
#define OGPU_PD_ATOP            9
#define OGPU_PD_ATOP_REVERSE    10
#define OGPU_PD_XOR             11
#define OGPU_PD_ADD             12
#define OGPU_PD_OPS             13      /* operators 0 .. OGPU_PD_OPS - 1; others are UNSUPPORTED */
#define OGPU_PD_SOLID           0x0100  /* the source word is a premultiplied ARGB32 colour, not a slot */
#define OGPU_PD_MASK            0x0200  /* a mask: the mask word is a slot (A8 or ARGB32) */
#define OGPU_PD_MASK_SOLID      0x0400  /* a mask: the mask word's low byte is a constant alpha */
#define OGPU_PD_SRC_REPEAT      0x0800  /* the source tiles (pixman's REPEAT_NORMAL) */
#define OGPU_PD_MASK_REPEAT     0x1000  /* the mask tiles */

/* v1.2: LINES_BLEND's flags, above its blend mode. */
#define OGPU_LINES_STRIP        8   /* each point joins the next (else pairs: 0-1, 2-3, ...) */
#define OGPU_LINES_LAST         16  /* draw each line's last point (else it is left out, so
                                       a strip's joints are blended once) */

/* v1.2: COMPOSITE_AFFINE. The matrix, 16.16 signed, takes a point of the
 * source (from its corner at source xy) to the target:
 *   x = m00 u + m01 v + m02,  y = m10 u + m11 v + m12.
 * Each target pixel whose centre maps inside the source rectangle is drawn,
 * from the source pixel there (or the four round it, with BILINEAR), times
 * the colour (ARGB32; its alpha is the global alpha; 0xFFFFFFFF changes
 * nothing). The matrix's own scale is at most 127 either way. Flags: a blend
 * mode in bits 0-2, OGPU_COMP_SRCALPHA (8 here: the source's own alpha),
 * OGPU_COMP_BILINEAR (16 here). */
#define OGPU_AFF_SRCALPHA       8
#define OGPU_AFF_BILINEAR       16

/* v1.2: YUV's format word: the layout in bits 0-7, the colour space in bits
 * 8-15. Planar layouts give the Y, U and V planes' addresses (YV12 is I420
 * with its planes named in the file's order: give U's and V's addresses as
 * they are); NV12 and NV21 give Y and the interleaved UV (V's address is 0);
 * packed layouts give their one plane as Y. Drawn 1:1 into the target
 * (RGB565 or ARGB32 and the v1.2 formats) at xy, inside the clip; wh is the
 * picture's size, even. */
#define OGPU_YUV_I420           1
#define OGPU_YUV_YV12           2
#define OGPU_YUV_NV12           3
#define OGPU_YUV_NV21           4
#define OGPU_YUV_YUY2           5
#define OGPU_YUV_UYVY           6
#define OGPU_YUV_YVYU           7
#define OGPU_YUV_BT601          0   /* video range, 16-235 */
#define OGPU_YUV_BT709          1
#define OGPU_YUV_JPEG           2   /* full range */
#define OGPU_YUV_FORMAT(layout, space) ((unsigned long)(layout) | ((unsigned long)(space) << 8))

#define OGPU_MAX_SLOTS          16
#define OGPU_MAX_SIZE           16384   /* widths and heights */

/* Back end results. */
#define OGPU_OK                 0
#define OGPU_ERR_BADOP          -1  /* an opcode this back end doesn't know (skipped) */
#define OGPU_ERR_BADLEN         -2  /* a length that doesn't fit the command or the stream (stops) */
#define OGPU_ERR_NOSURFACE      -3  /* a slot that wasn't described, or no target */
#define OGPU_ERR_NOMAP          -4  /* an address the back end can't reach */
#define OGPU_ERR_UNSUPPORTED    -5  /* a format pair the command can't do */
#define OGPU_ERR_DEVICE         -6  /* the GPU failed: the batch may be partly drawn (later ones run on the CPU) */
#define OGPU_ERR_EXPIRED        -7  /* OGPU_Wait: the fence is done, but too old for its result to be kept */

/* What a back end can do, per operation and target format (OGPU_Query). */
#define OGPU_NONE               0
#define OGPU_PARTIAL            1
#define OGPU_FULL               2

#endif
