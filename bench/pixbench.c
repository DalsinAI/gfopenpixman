/* GFOpenAmigaPixman qualification + performance harness.
 * Runs on AmigaOS 3.x and times pixman.library with timer.device E-clock.
 * Copyright (c) 2026 AmigaChrome Team. MIT. */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/pixman.h>
#include <stdio.h>
#include <string.h>

struct Library *PixmanBase;
struct Device *TimerBase;
static struct timerequest tr;

#define W 640
#define H 480
#define PIXELS ((ULONG)W * (ULONG)H)
#define ARGB_BYTES (PIXELS * 4UL)
#define MASK_BYTES PIXELS

static uint32_t *src32, *dst32;
static unsigned char *mask8;
static pixman_image_t *src_img, *dst_img, *mask_img, *solid_img;
static struct PXFill fill_req;
static struct PXBlt blt_req;
static struct PXComposite32 comp_req;
static struct PXComposite32 mask_req;
static struct PXFillRects rect_req;
static pixman_rectangle16_t rects[24];
static pixman_color_t rect_color, solid_color;
static ULONG rect_pixels;

static unsigned long checksum32(const uint32_t *p, ULONG n)
{
    ULONG h = 2166136261UL;
    while (n--) { h ^= *p++; h *= 16777619UL; }
    return h;
}

static unsigned long checksum8(const unsigned char *p, ULONG n)
{
    ULONG h = 2166136261UL;
    while (n--) { h ^= *p++; h *= 16777619UL; }
    return h;
}

static double ticks_now(ULONG *freq)
{
    struct EClockVal c;
    *freq = ReadEClock(&c);
    return (double)c.ev_hi * 4294967296.0 + (double)c.ev_lo;
}

static void init_patterns(void)
{
    ULONG i;
    for (i = 0; i < PIXELS; ++i) {
        ULONG x = i % W, y = i / W;
        src32[i] = 0x80204080UL | ((x & 31) << 16) | ((y & 31) << 8);
        dst32[i] = 0xff102030UL;
        mask8[i] = (unsigned char)((x * 5 + y * 3) & 255);
    }
    mask8[0] = 0;
    mask8[1] = 255;
}

static int setup_pixman(void)
{
    struct PXCreateBits q;
    int i;
    PixmanBase = OpenLibrary(PIXMANLIB_NAME, 1);
    if (!PixmanBase) { puts("HARNESS FAIL open pixman.library"); return 0; }

    q.format = PIXMAN_a8r8g8b8; q.width = W; q.height = H;
    q.bits = src32; q.rowstride_bytes = W * 4;
    src_img = PX_ImageCreateBits(&q);
    q.bits = dst32;
    dst_img = PX_ImageCreateBits(&q);

    q.format = PIXMAN_a8; q.bits = (uint32_t *)mask8; q.rowstride_bytes = W;
    mask_img = PX_ImageCreateBits(&q);

    solid_color.red = 0xffff; solid_color.green = 0xd000;
    solid_color.blue = 0x3000; solid_color.alpha = 0xffff;
    solid_img = PX_ImageCreateSolid(&solid_color);

    if (!src_img || !dst_img || !mask_img || !solid_img) {
        puts("HARNESS FAIL image setup"); return 0;
    }

    fill_req.bits = dst32; fill_req.stride = W; fill_req.bpp = 32;
    fill_req.x = 0; fill_req.y = 0; fill_req.width = W; fill_req.height = H;
    fill_req.xor_value = 0xff24486cUL;

    blt_req.src_bits = src32; blt_req.dst_bits = dst32;
    blt_req.src_stride = W; blt_req.dst_stride = W;
    blt_req.src_bpp = 32; blt_req.dst_bpp = 32;
    blt_req.src_x = 0; blt_req.src_y = 0; blt_req.dest_x = 0; blt_req.dest_y = 0;
    blt_req.width = W; blt_req.height = H;

    comp_req.op = PIXMAN_OP_SRC; comp_req.src = src_img; comp_req.mask = NULL;
    comp_req.dest = dst_img; comp_req.src_x = comp_req.src_y = 0;
    comp_req.mask_x = comp_req.mask_y = 0; comp_req.dest_x = comp_req.dest_y = 0;
    comp_req.width = W; comp_req.height = H;

    mask_req.op = PIXMAN_OP_OVER; mask_req.src = solid_img; mask_req.mask = mask_img;
    mask_req.dest = dst_img; mask_req.src_x = mask_req.src_y = 0;
    mask_req.mask_x = mask_req.mask_y = 0; mask_req.dest_x = mask_req.dest_y = 0;
    mask_req.width = W; mask_req.height = H;

    rect_pixels = 0;
    for (i = 0; i < 24; ++i) {
        int rw = 48 + (i % 4) * 24, rh = 20 + (i % 3) * 18;
        rects[i].x = (i * 47) % (W - 160);
        rects[i].y = (i * 31) % (H - 80);
        rects[i].width = rw; rects[i].height = rh;
        rect_pixels += (ULONG)rw * (ULONG)rh;
    }
    rect_color.red = 0x7000; rect_color.green = 0xb000;
    rect_color.blue = 0xf000; rect_color.alpha = 0xffff;
    rect_req.op = PIXMAN_OP_SRC; rect_req.image = dst_img;
    rect_req.color = &rect_color; rect_req.n_rects = 24; rect_req.rects = rects;
    return 1;
}

static void op_fill(void) { PX_Fill(&fill_req); }
static void op_blt(void) { PX_Blt(&blt_req); }
static void op_src(void) { comp_req.op = PIXMAN_OP_SRC; PX_Composite32(&comp_req); }
static void op_over(void) { comp_req.op = PIXMAN_OP_OVER; PX_Composite32(&comp_req); }
static void op_mask(void) { PX_Composite32(&mask_req); }
static void op_rects(void) { PX_ImageFillRectangles(&rect_req); }

static void op_browser(void)
{
    int i;
    PX_Fill(&fill_req);
    PX_ImageFillRectangles(&rect_req);

    blt_req.src_x = 16; blt_req.src_y = 24; blt_req.dest_x = 40; blt_req.dest_y = 48;
    blt_req.width = 320; blt_req.height = 180; PX_Blt(&blt_req);
    blt_req.src_x = 220; blt_req.src_y = 110; blt_req.dest_x = 370; blt_req.dest_y = 260;
    blt_req.width = 220; blt_req.height = 140; PX_Blt(&blt_req);

    comp_req.op = PIXMAN_OP_OVER;
    for (i = 0; i < 4; ++i) {
        comp_req.src_x = 32 * i; comp_req.src_y = 16 * i;
        comp_req.dest_x = 70 + 90 * i; comp_req.dest_y = 100 + 42 * i;
        comp_req.width = 220; comp_req.height = 120;
        PX_Composite32(&comp_req);
    }

    for (i = 0; i < 8; ++i) {
        mask_req.mask_x = (i * 17) & 63; mask_req.mask_y = (i * 9) & 31;
        mask_req.dest_x = 20; mask_req.dest_y = 18 + i * 48;
        mask_req.width = 560; mask_req.height = 32;
        PX_Composite32(&mask_req);
    }

    blt_req.src_x = blt_req.src_y = blt_req.dest_x = blt_req.dest_y = 0;
    blt_req.width = W; blt_req.height = H;
    comp_req.src_x = comp_req.src_y = comp_req.dest_x = comp_req.dest_y = 0;
    comp_req.width = W; comp_req.height = H;
    mask_req.mask_x = mask_req.mask_y = mask_req.dest_x = mask_req.dest_y = 0;
    mask_req.width = W; mask_req.height = H;
}

static double timed(void (*fn)(void), ULONG loops, ULONG *freq_out)
{
    ULONG f, i;
    double a = ticks_now(&f), b;
    for (i = 0; i < loops; ++i) fn();
    b = ticks_now(&f);
    *freq_out = f;
    return (b - a) / (double)f;
}

static double median3(double a, double b, double c)
{
    double t;
    if (a > b) { t = a; a = b; b = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (a > b) { t = a; a = b; b = t; }
    return b;
}

static void bench(const char *name, void (*fn)(void), ULONG pixels, ULONG bytes)
{
    ULONG loops = 1, freq, h;
    double s, a, b, c, m, mpix, mib, us;
    fn();
    do {
        s = timed(fn, loops, &freq);
        if (s >= 0.18 || loops >= 4096) break;
        loops <<= 1;
    } while (1);
    a = timed(fn, loops, &freq);
    b = timed(fn, loops, &freq);
    c = timed(fn, loops, &freq);
    m = median3(a, b, c);
    h = checksum32(dst32, PIXELS);
    mpix = ((double)loops * (double)pixels) / (m * 1000000.0);
    mib = ((double)loops * (double)bytes) / (m * 1048576.0);
    us = m * 1000000.0 / (double)loops;
    printf("BENCH name=%s loops=%lu us_x100=%lu mpix_s_x1000=%lu mib_s_x1000=%lu hash=%08lx\n",
           name, (unsigned long)loops,
           (unsigned long)(us * 100.0 + 0.5),
           (unsigned long)(mpix * 1000.0 + 0.5),
           (unsigned long)(mib * 1000.0 + 0.5),
           (unsigned long)h);
}

static void browser_bench(void)
{
    const ULONG touched =
        PIXELS + rect_pixels + 320UL*180UL + 220UL*140UL +
        4UL*220UL*120UL + 8UL*560UL*32UL;
    ULONG loops = 1, freq, h;
    double s, a, b, c, m, fps, mpix;
    op_browser();
    do {
        s = timed(op_browser, loops, &freq);
        if (s >= 0.25 || loops >= 1024) break;
        loops <<= 1;
    } while (1);
    a = timed(op_browser, loops, &freq);
    b = timed(op_browser, loops, &freq);
    c = timed(op_browser, loops, &freq);
    m = median3(a, b, c);
    h = checksum32(dst32, PIXELS);
    fps = (double)loops / m;
    mpix = ((double)loops * (double)touched) / (m * 1000000.0);
    printf("BROWSER name=paint640x480 loops=%lu us_x100=%lu frames_s_x1000=%lu touched_mpix_s_x1000=%lu hash=%08lx\n",
           (unsigned long)loops,
           (unsigned long)(m * 100000000.0 / (double)loops + 0.5),
           (unsigned long)(fps * 1000.0 + 0.5),
           (unsigned long)(mpix * 1000.0 + 0.5),
           (unsigned long)h);
}

static int correctness(void)
{
    ULONG before, after;
    int ok = 1;
    init_patterns();

    fill_req.xor_value = 0xff112233UL;
    if (!PX_Fill(&fill_req) || dst32[0] != 0xff112233UL || dst32[PIXELS-1] != 0xff112233UL) {
        puts("CHECK fill FAIL"); ok = 0;
    } else puts("CHECK fill PASS");
    fill_req.xor_value = 0xff24486cUL;

    init_patterns();
    if (!PX_Blt(&blt_req) || dst32[0] != src32[0] || dst32[12345] != src32[12345]) {
        puts("CHECK blt FAIL"); ok = 0;
    } else puts("CHECK blt PASS");

    init_patterns();
    comp_req.op = PIXMAN_OP_SRC; PX_Composite32(&comp_req);
    if (dst32[0] != src32[0] || dst32[23456] != src32[23456]) {
        puts("CHECK composite_src FAIL"); ok = 0;
    } else puts("CHECK composite_src PASS");

    init_patterns();
    before = dst32[1000]; comp_req.op = PIXMAN_OP_OVER; PX_Composite32(&comp_req); after = dst32[1000];
    if (after == before) { puts("CHECK composite_over FAIL"); ok = 0; }
    else puts("CHECK composite_over PASS");

    init_patterns();
    before = dst32[0]; PX_Composite32(&mask_req);
    if (dst32[0] != before || dst32[1] == 0xff102030UL) {
        puts("CHECK mask_over FAIL"); ok = 0;
    } else puts("CHECK mask_over PASS");

    init_patterns();
    before = checksum32(dst32, PIXELS);
    if (!PX_ImageFillRectangles(&rect_req)) { puts("CHECK rects FAIL"); ok = 0; }
    else {
        after = checksum32(dst32, PIXELS);
        if (after == before) { puts("CHECK rects FAIL"); ok = 0; }
        else puts("CHECK rects PASS");
    }

    printf("CHECK final_hash=%08lx mask_hash=%08lx result=%s\n",
           (unsigned long)checksum32(dst32, PIXELS),
           (unsigned long)checksum8(mask8, MASK_BYTES), ok ? "PASS" : "FAIL");
    return ok;
}

static void cleanup(void)
{
    if (solid_img) PX_ImageUnref(solid_img);
    if (mask_img) PX_ImageUnref(mask_img);
    if (dst_img) PX_ImageUnref(dst_img);
    if (src_img) PX_ImageUnref(src_img);
    if (PixmanBase) CloseLibrary(PixmanBase);
    if (TimerBase) CloseDevice(&tr.tr_node);
    if (mask8) FreeMem(mask8, MASK_BYTES);
    if (dst32) FreeMem(dst32, ARGB_BYTES);
    if (src32) FreeMem(src32, ARGB_BYTES);
}

int main(void)
{
    int ok;
    src32 = (uint32_t *)AllocMem(ARGB_BYTES, MEMF_ANY);
    dst32 = (uint32_t *)AllocMem(ARGB_BYTES, MEMF_ANY);
    mask8 = (unsigned char *)AllocMem(MASK_BYTES, MEMF_ANY);
    if (!src32 || !dst32 || !mask8) { puts("HARNESS FAIL memory"); cleanup(); return 20; }

    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_ECLOCK, &tr.tr_node, 0)) {
        puts("HARNESS FAIL timer.device"); cleanup(); return 20;
    }
    TimerBase = tr.tr_node.io_Device;
    init_patterns();
    if (!setup_pixman()) { cleanup(); return 20; }

    printf("HARNESS id=openamigapixman-b0 version=1 pixman=%s target=68040+FPU size=%dx%d\n",
           PX_VersionString(), W, H);
    ok = correctness();
    if (!ok) { puts("HARNESS FAIL correctness"); cleanup(); return 20; }

    init_patterns();
    bench("fill32", op_fill, PIXELS, PIXELS * 4UL);
    bench("blt32", op_blt, PIXELS, PIXELS * 8UL);
    bench("src32", op_src, PIXELS, PIXELS * 8UL);
    bench("over32", op_over, PIXELS, PIXELS * 12UL);
    bench("mask_a8_over32", op_mask, PIXELS, PIXELS * 9UL);
    bench("fillrects24", op_rects, rect_pixels, rect_pixels * 4UL);
    browser_bench();

    puts("HARNESS PASS");
    cleanup();
    return 0;
}
