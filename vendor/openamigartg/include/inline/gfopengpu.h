/* Copyright (c) 2026 AmigaChrome Team. OpenRTG, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 * Inline calls for bebbo's m68k-amigaos-gcc, from library/opengpu/gfopengpu_lib.sfd. */
#ifndef _INLINE_OPENGPU_H
#define _INLINE_OPENGPU_H
#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif
#ifndef OPENGPU_BASE_NAME
#define OPENGPU_BASE_NAME OpenGPUBase
#endif
#define OGPU_Query(op, format) \
    LP2(0x1e, ULONG, OGPU_Query, ULONG, op, d0, ULONG, format, d1, , OPENGPU_BASE_NAME)
#define OGPU_BackEndName(index) \
    LP1(0x24, STRPTR, OGPU_BackEndName, ULONG, index, d0, , OPENGPU_BASE_NAME)
#define OGPU_Submit(stream, words, fence) \
    LP3(0x2a, LONG, OGPU_Submit, APTR, stream, a0, ULONG, words, d0, ULONG *, fence, a1, , OPENGPU_BASE_NAME)
#define OGPU_Wait(fence) \
    LP1(0x30, LONG, OGPU_Wait, ULONG, fence, d0, , OPENGPU_BASE_NAME)
#define OGPU_ModuleOpen(name, version, table) \
    LP3(0x36, APTR, OGPU_ModuleOpen, CONST_STRPTR, name, a0, ULONG, version, d0, APTR *, table, a1, , OPENGPU_BASE_NAME)
#define OGPU_ModuleClose(handle) \
    LP1NR(0x3c, OGPU_ModuleClose, APTR, handle, a0, , OPENGPU_BASE_NAME)
/* 0.6: GFOpenGfx (gfopengpu/gfx.h) */
#define OGFX_Version() \
    LP0(0x42, ULONG, OGFX_Version, , OPENGPU_BASE_NAME)
#define OGFX_InstallPatches() \
    LP0(0x48, LONG, OGFX_InstallPatches, , OPENGPU_BASE_NAME)
#define OGFX_SetEnabled(enabled) \
    LP1(0x4e, LONG, OGFX_SetEnabled, ULONG, enabled, d0, , OPENGPU_BASE_NAME)
#define OGFX_Status() \
    LP0(0x54, ULONG, OGFX_Status, , OPENGPU_BASE_NAME)
#define OGFX_RegisterProvider(provider) \
    LP1(0x5a, LONG, OGFX_RegisterProvider, struct OGFXProviderV1 *, provider, a0, , OPENGPU_BASE_NAME)
#define OGFX_UnregisterProvider(owner) \
    LP1(0x60, LONG, OGFX_UnregisterProvider, APTR, owner, a0, , OPENGPU_BASE_NAME)
#define OGFX_Text(rp, text, length) \
    LP3(0x66, LONG, OGFX_Text, struct RastPort *, rp, a1, STRPTR, text, a0, ULONG, length, d0, , OPENGPU_BASE_NAME)
#define OGFX_TextLength(rp, text, length) \
    LP3(0x6c, WORD, OGFX_TextLength, struct RastPort *, rp, a1, STRPTR, text, a0, ULONG, length, d0, , OPENGPU_BASE_NAME)
#define OGFX_TextExtent(rp, text, length, extent) \
    LP4NR(0x72, OGFX_TextExtent, struct RastPort *, rp, a1, STRPTR, text, a0, ULONG, length, d0, struct TextExtent *, extent, a2, , OPENGPU_BASE_NAME)
#define OGFX_TextFit(rp, text, length, extent, constraining, direction, bitWidth, bitHeight) \
    LP8(0x78, ULONG, OGFX_TextFit, struct RastPort *, rp, a1, STRPTR, text, a0, ULONG, length, d0, struct TextExtent *, extent, a2, \
        struct TextExtent *, constraining, a3, LONG, direction, d1, ULONG, bitWidth, d2, ULONG, bitHeight, d3, , OPENGPU_BASE_NAME)
#define OGFX_RectFill(rp, x0, y0, x1, y1) \
    LP5NR(0x7e, OGFX_RectFill, struct RastPort *, rp, a1, LONG, x0, d0, LONG, y0, d1, LONG, x1, d2, LONG, y1, d3, , OPENGPU_BASE_NAME)
#define OGFX_BltBitMap(src, sx, sy, dst, dx, dy, width, height, minterm, mask, temp) \
    LP11(0x84, LONG, OGFX_BltBitMap, struct BitMap *, src, a0, LONG, sx, d0, LONG, sy, d1, struct BitMap *, dst, a1, LONG, dx, d2, \
         LONG, dy, d3, LONG, width, d4, LONG, height, d5, ULONG, minterm, d6, ULONG, mask, d7, PLANEPTR, temp, a2, , OPENGPU_BASE_NAME)
#define OGFX_BltTemplate(source, sx, modulo, rp, dx, dy, width, height) \
    LP8NR(0x8a, OGFX_BltTemplate, PLANEPTR, source, a0, LONG, sx, d0, LONG, modulo, d1, struct RastPort *, rp, a1, LONG, dx, d2, \
          LONG, dy, d3, LONG, width, d4, LONG, height, d5, , OPENGPU_BASE_NAME)
#define OGFX_ScrollRaster(rp, dx, dy, x0, y0, x1, y1) \
    LP7NR(0x90, OGFX_ScrollRaster, struct RastPort *, rp, a1, LONG, dx, d0, LONG, dy, d1, LONG, x0, d2, LONG, y0, d3, \
          LONG, x1, d4, LONG, y1, d5, , OPENGPU_BASE_NAME)
/* 0.7: GFOpenGfx's look hook (gfopengpu/gfx.h) */
#define OGFX_RegisterLook(look) \
    LP1(0x96, LONG, OGFX_RegisterLook, struct OGFXLookV1 *, look, a0, , OPENGPU_BASE_NAME)
#define OGFX_UnregisterLook(owner) \
    LP1(0x9c, LONG, OGFX_UnregisterLook, APTR, owner, a0, , OPENGPU_BASE_NAME)
#endif
