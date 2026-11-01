/* Copyright (c) 2026 AmigaChrome Team. MIT licence (LICENSE).
 *
 * pixman.library 2.1 for AmigaOS 3.x: Pixman's interface (the LVOs in
 * pixman_lib.sfd, unchanged from 1.0) as a thin layer over GFOpenGPU
 * (DESIGN.md). px_core.c turns each call into a GFOpenGPU batch; the pixels
 * are drawn by gfopengpu.library: by its driver (ACRTG.gpu on AmiCradle,
 * where the host draws) or by its own 68k back end. There is no copy of
 * upstream Pixman in this library, and no floating point.
 *
 * Built bare (no startup code or C library): the RomTag's stub comes first.
 * It needs gfopengpu.library with stream v1.4 (COMPOSITE_PD); OpenLibrary
 * of pixman.library fails without it. */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <proto/exec.h>

#include "../include/libraries/pixman.h"
#include "../vendor/openamigartg/include/gfopengpu/gfopengpu.h"
#include "px_core.h"

#define REG(r, decl) register decl __asm(#r)
#define LIB_VERSION PIXMANLIB_VERSION
#define LIB_REVISION PIXMANLIB_REVISION

struct PixmanLibBase {
    struct Library lib;
    BPTR seglist;
    struct SignalSemaphore lock;        /* image references */
    struct Library *gpu;                /* gfopengpu.library */
    struct px_env env;
    struct px_stats stats;
};

/* exec's base from address 4, through a register (gcc 16's -Warray-bounds) */
static inline struct ExecBase *px_sysbase(void)
{ struct ExecBase **p = (struct ExecBase **)4; __asm__ ("" : "+r"(p)); return *p; }
#define SysBase (px_sysbase())

/* Run as a program, the library does nothing. This must stay the first
 * code in the file (-fno-toplevel-reorder). */
int start(void) { return -1; }

static const char lib_name[] = PIXMANLIB_NAME;
static const char lib_id[] =
    "pixman.library 2.1 (1.11.2026) Pixman 0.46.4's interface over GFOpenGPU, AmiCradle Team\r\n";
static const char lib_ver[] __attribute__((used)) =
    "$VER: pixman.library 2.1 (1.11.2026) Pixman 0.46.4's interface over GFOpenGPU, AmiCradle Team";

#define OpenGPUBase (base->gpu)
#include "../vendor/openamigartg/include/inline/gfopengpu.h"

/* ---- the core's environment ------------------------------------------------------- */

static void *env_alloc(void *ctx, unsigned long bytes) { (void)ctx; return AllocVec(bytes, MEMF_ANY | MEMF_CLEAR); }
static void env_free(void *ctx, void *p) { (void)ctx; if (p) FreeVec(p); }
static unsigned long env_address(void *ctx, const void *p) { (void)ctx; return (unsigned long)p; }
static long env_submit(void *ctx, const void *stream, long words)
{
    struct PixmanLibBase *base = ctx;
    ULONG fence = 0;
    LONG r = OGPU_Submit((APTR)stream, (ULONG)words, &fence);
    if (r == OGPU_OK && fence) r = OGPU_Wait(fence);
    return r;
}

/* ---- the library ------------------------------------------------------------------ */

static struct Library *lib_init(REG(d0, struct PixmanLibBase *base), REG(a0, BPTR seglist), REG(a6, struct ExecBase *sys))
{
    (void)sys;
    base->seglist = seglist;
    base->lib.lib_Revision = LIB_REVISION;
    InitSemaphore(&base->lock);
    base->env.alloc = env_alloc;
    base->env.free = env_free;
    base->env.address = env_address;
    base->env.submit = env_submit;
    base->env.ctx = base;
    return &base->lib;
}

static BPTR lib_expunge(REG(a6, struct PixmanLibBase *base));

static struct Library *lib_open(REG(a6, struct PixmanLibBase *base))
{
    base->lib.lib_OpenCnt++;
    base->lib.lib_Flags &= ~LIBF_DELEXP;
    if (!base->gpu) {
        /* stream v1.4: its CPU back end answers COMPOSITE_PD */
        struct Library *g = OpenLibrary((CONST_STRPTR)OPENGPU_NAME, OPENGPU_VERSION);
        if (g) {
            base->gpu = g;
            if (OGPU_ANSWER(OGPU_Query(OGPU_OP_COMPOSITE_PD, OGPU_FMT_ARGB32)) == OGPU_NONE) {
                base->gpu = NULL;
                CloseLibrary(g);
            }
        }
        if (!base->gpu) { base->lib.lib_OpenCnt--; return NULL; }
    }
    return &base->lib;
}

static BPTR lib_close(REG(a6, struct PixmanLibBase *base))
{
    base->lib.lib_OpenCnt--;
    if (base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
        return lib_expunge(base);
    return 0;
}

static BPTR lib_expunge(REG(a6, struct PixmanLibBase *base))
{
    BPTR seglist;
    if (base->lib.lib_OpenCnt) {
        base->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seglist = base->seglist;
    Remove(&base->lib.lib_Node);
    if (base->gpu) CloseLibrary(base->gpu);
    FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
    return seglist;
}

static ULONG lib_null(void) { return 0; }

/* ---- Pixman's calls --------------------------------------------------------------- */

#define E (&base->env)
#define ST (&base->stats)

static ULONG PX_Version(REG(a6, struct PixmanLibBase *base)) { (void)base; return 4604; }   /* the interface's: 0.46.4 */
static CONST_STRPTR PX_VersionString(REG(a6, struct PixmanLibBase *base)) { (void)base; return (CONST_STRPTR)"0.46.4"; }

static pixman_image_t *PX_ImageCreateBits(REG(a0, struct PXCreateBits *r), REG(a6, struct PixmanLibBase *base))
{
    if (!r) return NULL;
    return px_image_create_bits(E, r->format, r->width, r->height, r->bits, r->rowstride_bytes);
}

static pixman_image_t *PX_ImageCreateSolid(REG(a0, const pixman_color_t *c), REG(a6, struct PixmanLibBase *base))
{
    return px_image_create_solid(E, c);
}

static pixman_image_t *PX_ImageRef(REG(a0, pixman_image_t *image), REG(a6, struct PixmanLibBase *base))
{
    pixman_image_t *out;
    ObtainSemaphore(&base->lock); out = px_image_ref(image); ReleaseSemaphore(&base->lock);
    return out;
}

static BOOL PX_ImageUnref(REG(a0, pixman_image_t *image), REG(a6, struct PixmanLibBase *base))
{
    int out;
    ObtainSemaphore(&base->lock); out = px_image_unref(E, image); ReleaseSemaphore(&base->lock);
    return out ? TRUE : FALSE;
}

static BOOL PX_ImageSetTransform(REG(a0, pixman_image_t *image), REG(a1, const pixman_transform_t *t), REG(a6, struct PixmanLibBase *base))
{
    (void)base;
    return px_image_set_transform(image, t) ? TRUE : FALSE;
}

static void PX_ImageSetRepeat(REG(a0, pixman_image_t *image), REG(d0, ULONG repeat), REG(a6, struct PixmanLibBase *base))
{
    (void)base; px_image_set_repeat(image, (int)repeat);
}

static BOOL PX_ImageSetFilter(REG(a0, struct PXFilter *r), REG(a6, struct PixmanLibBase *base))
{
    (void)base;
    if (!r) return FALSE;
    return px_image_set_filter(r->image, (int)r->filter, r->params, (int)r->n_params) ? TRUE : FALSE;
}

static void PX_ImageSetComponentAlpha(REG(a0, pixman_image_t *image), REG(d0, ULONG value), REG(a6, struct PixmanLibBase *base))
{
    (void)base; px_image_set_component_alpha(image, value != 0);
}

static uint32_t *PX_ImageGetData(REG(a0, pixman_image_t *image), REG(a6, struct PixmanLibBase *base)) { (void)base; return px_image_get_data(image); }
static LONG PX_ImageGetWidth(REG(a0, pixman_image_t *image), REG(a6, struct PixmanLibBase *base)) { (void)base; return px_image_get_width(image); }
static LONG PX_ImageGetHeight(REG(a0, pixman_image_t *image), REG(a6, struct PixmanLibBase *base)) { (void)base; return px_image_get_height(image); }
static LONG PX_ImageGetStride(REG(a0, pixman_image_t *image), REG(a6, struct PixmanLibBase *base)) { (void)base; return px_image_get_stride(image); }
static ULONG PX_ImageGetFormat(REG(a0, pixman_image_t *image), REG(a6, struct PixmanLibBase *base)) { (void)base; return px_image_get_format(image); }

static void PX_Composite32(REG(a0, struct PXComposite32 *r), REG(a6, struct PixmanLibBase *base))
{
    if (!r) return;
    px_composite32(E, ST, (int)r->op, r->src, r->mask, r->dest, (int)r->src_x, (int)r->src_y,
                   (int)r->mask_x, (int)r->mask_y, (int)r->dest_x, (int)r->dest_y, (int)r->width, (int)r->height);
}

static BOOL PX_Fill(REG(a0, struct PXFill *r), REG(a6, struct PixmanLibBase *base))
{
    if (!r) return FALSE;
    return px_fill(E, ST, r->bits, (int)r->stride, (int)r->bpp, (int)r->x, (int)r->y,
                   (int)r->width, (int)r->height, r->xor_value) ? TRUE : FALSE;
}

static BOOL PX_Blt(REG(a0, struct PXBlt *r), REG(a6, struct PixmanLibBase *base))
{
    if (!r) return FALSE;
    return px_blt(E, ST, r->src_bits, r->dst_bits, (int)r->src_stride, (int)r->dst_stride,
                  (int)r->src_bpp, (int)r->dst_bpp, (int)r->src_x, (int)r->src_y,
                  (int)r->dest_x, (int)r->dest_y, (int)r->width, (int)r->height) ? TRUE : FALSE;
}

static BOOL PX_ImageFillRectangles(REG(a0, struct PXFillRects *r), REG(a6, struct PixmanLibBase *base))
{
    if (!r) return FALSE;
    return px_fill_rectangles(E, ST, (int)r->op, r->image, r->color, (int)r->n_rects, r->rects) ? TRUE : FALSE;
}

static void PX_Region32Init(REG(a0, pixman_region32_t *r), REG(a6, struct PixmanLibBase *base)) { (void)base; if (r) px_region32_init(r); }
static void PX_Region32InitRect(REG(a0, pixman_region32_t *r), REG(d0, LONG x), REG(d1, LONG y),
                                REG(d2, ULONG w), REG(d3, ULONG h), REG(a6, struct PixmanLibBase *base))
{
    (void)base; if (r) px_region32_init_rect(r, (int)x, (int)y, (unsigned)w, (unsigned)h);
}
static void PX_Region32Fini(REG(a0, pixman_region32_t *r), REG(a6, struct PixmanLibBase *base)) { if (r) px_region32_fini(E, r); }
static BOOL PX_Region32Copy(REG(a0, pixman_region32_t *dst), REG(a1, const pixman_region32_t *src), REG(a6, struct PixmanLibBase *base))
{
    return dst && src && px_region32_copy(E, dst, src) ? TRUE : FALSE;
}
static BOOL PX_Region32Intersect(REG(a0, pixman_region32_t *dst), REG(a1, const pixman_region32_t *a),
                                 REG(a2, const pixman_region32_t *b), REG(a6, struct PixmanLibBase *base))
{
    return dst && a && b && px_region32_intersect(E, dst, a, b) ? TRUE : FALSE;
}
static BOOL PX_Region32Union(REG(a0, pixman_region32_t *dst), REG(a1, const pixman_region32_t *a),
                             REG(a2, const pixman_region32_t *b), REG(a6, struct PixmanLibBase *base))
{
    return dst && a && b && px_region32_union(E, dst, a, b) ? TRUE : FALSE;
}

/* ---- the RomTag ------------------------------------------------------------------- */

static const APTR lib_vectors[] = {
    (APTR)lib_open, (APTR)lib_close, (APTR)lib_expunge, (APTR)lib_null,
    (APTR)PX_Version, (APTR)PX_VersionString,
    (APTR)PX_ImageCreateBits, (APTR)PX_ImageCreateSolid,
    (APTR)PX_ImageRef, (APTR)PX_ImageUnref,
    (APTR)PX_ImageSetTransform, (APTR)PX_ImageSetRepeat, (APTR)PX_ImageSetFilter,
    (APTR)PX_ImageSetComponentAlpha,
    (APTR)PX_ImageGetData, (APTR)PX_ImageGetWidth, (APTR)PX_ImageGetHeight,
    (APTR)PX_ImageGetStride, (APTR)PX_ImageGetFormat,
    (APTR)PX_Composite32, (APTR)PX_Fill, (APTR)PX_Blt,
    (APTR)PX_ImageFillRectangles,
    (APTR)PX_Region32Init, (APTR)PX_Region32InitRect, (APTR)PX_Region32Fini,
    (APTR)PX_Region32Copy, (APTR)PX_Region32Intersect, (APTR)PX_Region32Union,
    (APTR)-1
};

static const struct {
    ULONG size;
    const APTR *vectors;
    APTR data;
    APTR init;
} lib_inittable = {
    sizeof(struct PixmanLibBase), lib_vectors, NULL, (APTR)lib_init
};

const struct Resident lib_romtag = {
    RTC_MATCHWORD, (struct Resident *)&lib_romtag, (APTR)(&lib_romtag + 1),
    RTF_AUTOINIT, LIB_VERSION, NT_LIBRARY, 0,
    (char *)lib_name, (char *)lib_id, (APTR)&lib_inittable
};
