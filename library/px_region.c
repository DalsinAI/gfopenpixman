/* Copyright (c) 2026 AmiCradle Team. MIT licence (LICENSE).
 *
 * Pixman's region32 calls (px_core.h) in Pixman's public data layout: the
 * extents, then data, which is 0 for one rectangle (the extents) or points
 * at a size and count with that many boxes after them; an empty region has
 * a count of 0. The boxes are y-x banded: sorted by y, then x; the boxes of
 * a band share y1 and y2 and never touch; two bands next to each other
 * never have the same x spans (they would be one band).
 *
 * Union and intersection both work band by band: the region is cut at
 * every y where a box of either side starts or ends, each band's spans are
 * merged (union) or overlapped (intersection), and equal neighbouring bands
 * are joined. This is the Team's own method, written from that layout. */
#include "px_core.h"

static pixman_region32_data_t px_empty = { 0, 0 };

static const pixman_box32_t *boxes(const pixman_region32_t *r, long *n) {
    if (!r->data) { *n = r->extents.x2 > r->extents.x1 && r->extents.y2 > r->extents.y1; return &r->extents; }
    *n = r->data->numRects;
    return (const pixman_box32_t *)(r->data + 1);
}

static void set_empty(pixman_region32_t *r) {
    r->extents.x1 = r->extents.y1 = r->extents.x2 = r->extents.y2 = 0;
    r->data = &px_empty;
}

void px_region32_init(pixman_region32_t *r) { set_empty(r); }

void px_region32_init_rect(pixman_region32_t *r, int x, int y, unsigned w, unsigned h) {
    if (!w || !h) { set_empty(r); return; }
    r->extents.x1 = x; r->extents.y1 = y;
    r->extents.x2 = x + (int)w; r->extents.y2 = y + (int)h;
    r->data = 0;
}

static void drop(const struct px_env *e, pixman_region32_t *r) {
    if (r->data && r->data != &px_empty && r->data->size) e->free(e->ctx, r->data);
}

void px_region32_fini(const struct px_env *e, pixman_region32_t *r) { drop(e, r); }

/* boxes into a region: 0 when memory runs out (dst is then empty) */
static int set_boxes(const struct px_env *e, pixman_region32_t *dst, const pixman_box32_t *b, long n) {
    long i;
    pixman_region32_data_t *d;
    pixman_box32_t *o;
    drop(e, dst);
    if (n == 0) { set_empty(dst); return 1; }
    dst->extents = b[0];
    for (i = 1; i < n; i++) {
        if (b[i].x1 < dst->extents.x1) dst->extents.x1 = b[i].x1;
        if (b[i].x2 > dst->extents.x2) dst->extents.x2 = b[i].x2;
        if (b[i].y2 > dst->extents.y2) dst->extents.y2 = b[i].y2;
    }
    if (n == 1) { dst->data = 0; return 1; }
    if (!(d = e->alloc(e->ctx, sizeof *d + (unsigned long)n * sizeof *b))) { set_empty(dst); return 0; }
    d->size = n; d->numRects = n;
    o = (pixman_box32_t *)(d + 1);
    for (i = 0; i < n; i++) o[i] = b[i];
    dst->data = d;
    return 1;
}

int px_region32_copy(const struct px_env *e, pixman_region32_t *dst, const pixman_region32_t *src) {
    long n;
    const pixman_box32_t *b;
    if (dst == src) return 1;
    b = boxes(src, &n);
    if (!src->data) { drop(e, dst); *dst = *src; return 1; }
    return set_boxes(e, dst, b, n);
}

/* Region r's band that covers y0..y1, as its first box (*s) and its count;
 * 0 if r has no band there. r's band edges are among the cut points, so a
 * band covers a cut whole or not at all. y0 only grows, so *s only moves
 * forward: the walk over all cuts is linear. *l caches the band's count. */
static long band_at(const pixman_box32_t *b, long n, long *s, long *l, int y0) {
    for (;;) {
        if (*s >= n) return 0;
        if (!*l) {
            long k = *s + 1;
            while (k < n && b[k].y1 == b[*s].y1) k++;
            *l = k - *s;
        }
        if (b[*s].y2 > y0) break;
        *s += *l; *l = 0;
    }
    return b[*s].y1 <= y0 ? *l : 0;
}

/* r's next band edge above y0, or `none' when r has no more */
static int next_edge(const pixman_box32_t *b, long n, long s, int y0, int none) {
    if (s >= n) return none;
    return b[s].y1 > y0 ? b[s].y1 : b[s].y2;
}

/* room for `more' boxes after the first `used' of *out (capacity *cap):
 * the output grows as it needs to, rather than being sized for the worst
 * case up front. 0 when memory runs out. */
static int room(const struct px_env *e, pixman_box32_t **out, long *cap, long used, long more) {
    long want = *cap, i;
    pixman_box32_t *o;
    if (more <= *cap - used) return 1;
    while (more > want - used) {
        if (want > 0x7FFFFFFFL / 2 / (long)sizeof **out) return 0;
        want *= 2;
    }
    if (!(o = e->alloc(e->ctx, (unsigned long)want * sizeof *o))) return 0;
    for (i = 0; i < used; i++) o[i] = (*out)[i];
    e->free(e->ctx, *out);
    *out = o; *cap = want;
    return 1;
}

static int combine(const struct px_env *e, pixman_region32_t *dst, const pixman_region32_t *ra,
                   const pixman_region32_t *rb, int unite) {
    long na, nb, nout = 0, cap, prev_start = -1, prev_n = 0, sa = 0, la = 0, sb = 0, lb = 0;
    const pixman_box32_t *a = boxes(ra, &na), *b = boxes(rb, &nb);
    pixman_box32_t *out;
    int y0, ok;
    if (!unite && (!na || !nb)) { drop(e, dst); set_empty(dst); return 1; }
    if (!na && !nb) { drop(e, dst); set_empty(dst); return 1; }
    if (na > 0x7FFFFFFFL / 4 / (long)sizeof *out || nb > 0x7FFFFFFFL / 4 / (long)sizeof *out) return 0;
    cap = na + nb + 4;
    if (!(out = e->alloc(e->ctx, (unsigned long)cap * sizeof *out))) return 0;
    /* the cuts: every y where a box of either side starts or ends, in order */
    y0 = !na ? b[0].y1 : !nb ? a[0].y1 : a[0].y1 < b[0].y1 ? a[0].y1 : b[0].y1;
    for (;;) {
        long ka, kb, ia = 0, ib = 0, k = 0, j;
        const pixman_box32_t *pa, *pb;
        pixman_box32_t *o;
        int y1, ea, eb;
        ka = band_at(a, na, &sa, &la, y0);
        kb = band_at(b, nb, &sb, &lb, y0);
        if (!unite && (sa >= na || sb >= nb)) break;   /* nothing more in common */
        ea = next_edge(a, na, sa, y0, y0);
        eb = next_edge(b, nb, sb, y0, y0);
        if (ea == y0 && eb == y0) break;               /* both regions done */
        y1 = ea == y0 ? eb : eb == y0 ? ea : ea < eb ? ea : eb;
        pa = a + sa; pb = b + sb;
        /* the band's spans: both sorted already (a band's boxes are in x order) */
        if (!room(e, &out, &cap, nout, ka + kb)) { e->free(e->ctx, out); return 0; }
        o = out + nout;
        if (unite) {
            while (ia < ka || ib < kb) {
                int x0, x1;
                if (ib >= kb || (ia < ka && pa[ia].x1 <= pb[ib].x1)) { x0 = pa[ia].x1; x1 = pa[ia].x2; ia++; }
                else { x0 = pb[ib].x1; x1 = pb[ib].x2; ib++; }
                if (k && o[k - 1].x2 >= x0) { if (x1 > o[k - 1].x2) o[k - 1].x2 = x1; }
                else { o[k].x1 = x0; o[k].x2 = x1; k++; }
            }
        } else {
            while (ia < ka && ib < kb) {
                int x0 = pa[ia].x1 > pb[ib].x1 ? pa[ia].x1 : pb[ib].x1, x1 = pa[ia].x2 < pb[ib].x2 ? pa[ia].x2 : pb[ib].x2;
                if (x0 < x1) { o[k].x1 = x0; o[k].x2 = x1; k++; }
                if (pa[ia].x2 < pb[ib].x2) ia++; else ib++;
            }
        }
        if (!k) { prev_start = -1; y0 = y1; continue; }
        /* the same spans as the band just above, touching it: one band */
        if (prev_start >= 0 && prev_n == k && out[prev_start].y2 == y0) {
            for (j = 0; j < k; j++)
                if (out[prev_start + j].x1 != o[j].x1 || out[prev_start + j].x2 != o[j].x2) break;
            if (j == k) {
                for (j = 0; j < k; j++) out[prev_start + j].y2 = y1;
                y0 = y1;
                continue;
            }
        }
        prev_start = nout; prev_n = k;
        for (j = 0; j < k; j++) { o[j].y1 = y0; o[j].y2 = y1; }
        nout += k;
        y0 = y1;
    }
    ok = set_boxes(e, dst, out, nout);
    e->free(e->ctx, out);
    return ok;
}

int px_region32_intersect(const struct px_env *e, pixman_region32_t *dst, const pixman_region32_t *a,
                          const pixman_region32_t *b) {
    return combine(e, dst, a, b, 0);
}

int px_region32_union(const struct px_env *e, pixman_region32_t *dst, const pixman_region32_t *a,
                      const pixman_region32_t *b) {
    return combine(e, dst, a, b, 1);
}
