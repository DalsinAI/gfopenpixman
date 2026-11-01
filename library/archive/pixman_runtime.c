/* Minimal process-free runtime used by resident pixman.library.
 * Copyright (c) 2026 AmiCradle Team. MIT.
 *
 * A resident Amiga library must not depend on a caller process' libc startup
 * or stdio state. Keep the few C-runtime facilities Pixman needs local.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <stddef.h>
#ifdef PX_AC_HELPERS
#include "ac_helpers.h"     /* AC090's native helpers (build-static.sh, AC_HELPERS=1) */
#endif

extern struct ExecBase *SysBase;

struct PXAllocHeader {
    ULONG size;
};

void *malloc(size_t n)
{
    struct PXAllocHeader *h;
    ULONG bytes;
    if (n == 0) n = 1;
    if (n > 0x7ffffff0UL) return NULL;
    bytes = (ULONG)n + sizeof(*h);
    h = (struct PXAllocHeader *)AllocVec(bytes, MEMF_ANY);
    if (!h) return NULL;
    h->size = (ULONG)n;
    return (void *)(h + 1);
}

void free(void *p)
{
    struct PXAllocHeader *h;
    if (!p) return;
    h = ((struct PXAllocHeader *)p) - 1;
    FreeVec(h);
}

void *calloc(size_t n, size_t size)
{
    UBYTE *p;
    size_t total, i;
    if (n && size > ((size_t)-1) / n) return NULL;
    total = n * size;
    p = (UBYTE *)malloc(total);
    if (!p) return NULL;
#ifdef PX_AC_HELPERS
    ac_memset_auto(p, 0, total);
    (void)i;
#else
    for (i = 0; i < total; ++i) p[i] = 0;
#endif
    return p;
}

void *realloc(void *p, size_t n)
{
    struct PXAllocHeader *h;
    UBYTE *q, *src;
    size_t oldn, copy, i;
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    h = ((struct PXAllocHeader *)p) - 1;
    oldn = h->size;
    q = (UBYTE *)malloc(n);
    if (!q) return NULL;
    src = (UBYTE *)p;
    copy = oldn < n ? oldn : n;
#ifdef PX_AC_HELPERS
    ac_memcpy_auto(q, src, copy);
    (void)i;
#else
    for (i = 0; i < copy; ++i) q[i] = src[i];
#endif
    free(p);
    return q;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    UBYTE *d = (UBYTE *)dst;
    const UBYTE *s = (const UBYTE *)src;
    size_t i;
    for (i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

/* The front doors (ac_helpers.h) hand memmoves under 64 bytes to the C
 * library's: this one, so the library needs nothing of libnix's. Never built
 * on the front doors themselves (achelpers/ac_string.h is not forced here). */
void *memmove(void *dst, const void *src, size_t n)
{
    UBYTE *d = (UBYTE *)dst;
    const UBYTE *s = (const UBYTE *)src;
    size_t i;
    if (d > s && d < s + n) { for (i = n; i != 0; --i) d[i - 1] = s[i - 1]; }
    else { for (i = 0; i < n; ++i) d[i] = s[i]; }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    UBYTE *d = (UBYTE *)dst;
    size_t i;
    for (i = 0; i < n; ++i) d[i] = (UBYTE)c;
    return dst;
}

char *strchr(const char *s, int c)
{
    unsigned char ch = (unsigned char)c;
    do {
        if ((unsigned char)*s == ch) return (char *)s;
    } while (*s++);
    return NULL;
}

int strncmp(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        unsigned char ac = (unsigned char)a[i];
        unsigned char bc = (unsigned char)b[i];
        if (ac != bc) return (int)ac - (int)bc;
        if (!ac) return 0;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

/* Pixman's implementation-disable environment knob and diagnostics are
 * process conveniences, not part of the resident library contract. */
char *getenv(const char *name) { (void)name; return NULL; }
int printf(const char *fmt, ...) { (void)fmt; return 0; }
int fprintf(void *stream, const char *fmt, ...) { (void)stream; (void)fmt; return 0; }
int fputc(int c, void *stream) { (void)stream; return c; }
int fflush(void *stream) { (void)stream; return 0; }

/* newlib's stderr macro may still materialise this address in a compiled
 * diagnostic call. Our fprintf ignores it. */
char __sF[256];

int errno;
