/*
 * openamigapixman: forced into every compile of upstream Pixman (-include)
 * when build-static.sh runs with AC_HELPERS=1. Pixman's memcpy, memmove and
 * memset calls go through the front doors of AC090's native helpers
 * (amigachrome-guest's ac_helpers.h): under 64 bytes, GCC's own code as
 * before (pixman.library's own small memcpy and memset in
 * library/pixman_runtime.c, which is never built with this header); from 64
 * bytes, the tagged functions, which run as host code on AmiCradle and as
 * 68k code written for the 68020 and 68040 elsewhere. As openamigaimage's
 * achelpers/ac_string.h.
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 */
#ifndef OAP_AC_STRING_H
#define OAP_AC_STRING_H
#include <string.h>
#include "ac_helpers.h"
#undef memcpy
#undef memmove
#undef memset
#define memcpy(d, s, n) ac_memcpy_auto(d, s, n)
#define memmove(d, s, n) ac_memmove_auto(d, s, n)
#define memset(d, c, n) ac_memset_auto(d, c, n)
#endif
