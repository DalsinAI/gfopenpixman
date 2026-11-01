/* Copyright (c) 2026 AmigaChrome Team. OpenRTG, MIT licence (LICENSE).
 * SPDX-License-Identifier: MIT
 * gfopengpu.library's public interface, 0.1 (G1: the CPU back end).
 *
 * A program builds a batch with the calls in <gfopengpu/build.h> (no library
 * calls), sends it with OGPU_Submit and, when it needs the pixels, waits for
 * its fence. OGPU_Query says before that whether an operation is done fully,
 * partly or not at all for a target format, and by which back end:
 *   answer = OGPU_Query(OGPU_OP_COMPOSITE, OGPU_FMT_RGB565);
 *   OGPU_ANSWER(answer) is OGPU_FULL, OGPU_PARTIAL or OGPU_NONE;
 *   OGPU_BACKEND(answer) is the back end's index for OGPU_BackEndName.
 * Back end 0 is always the CPU, on every Amiga. */
#ifndef OPENGPU_OPENGPU_H
#define OPENGPU_OPENGPU_H

#include "stream.h"

#define OPENGPU_NAME    "gfopengpu.library"
#define OPENGPU_VERSION 0

#define OGPU_ANSWER(q)   ((int)((q) & 0xFFUL))
#define OGPU_BACKEND(q)  ((int)(((q) >> 8) & 0xFFUL))

#define OGPU_BACKEND_CPU 0

#endif
