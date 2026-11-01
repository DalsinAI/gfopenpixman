# GFOpenAmigaPixman Browser Acceleration Design

> **17 October 2026:** pixman.library 2.0 is a thin layer over GFOpenGPU. It has no upstream Pixman inside, and no 68040+FPU, AC090 or AC_MAGIC backend of its own. Read `DESIGN.md` for the call map, the gaps and the stages. This page is kept as the record of why Pixman matters to the browser; its backend plan below is replaced by `DESIGN.md`.

## Purpose

GFOpenAmigaPixman is not primarily a word-processor dependency. Its strategic role in AmiCradle is to become a fast, stable 2D compositing layer underneath the HTML5 browser graphics stack.

The immediate target is AmigaOS 3.x on AmiCradle with a 68040 + FPU baseline, using upstream Pixman 0.46.4 as the correctness implementation and ABI surface. The design must leave room for AC090, GFOpenGPU and host-assisted acceleration without requiring browser code to know which backend is active.

## Why It Matters

The current WebKit/Cairo build already carries Pixman as part of the rendering dependency chain. That means a large class of browser work can converge on one well-defined raster/composite layer instead of being optimised piecemeal inside WebKit.

High-value HTML5 operations include:

- opaque fills and rectangle clears;
- source-copy blits;
- ARGB compositing and alpha blending;
- image scaling and filtering;
- glyph mask compositing;
- clipping and region operations;
- canvas image-data transfers;
- intermediate Cairo image-surface composition;
- CSS opacity, shadows and layered content where Cairo resolves to image composition.

Pixman therefore becomes a performance choke point we can measure, optimise and eventually accelerate below the browser.

## Architectural Boundary

The intended stack is:

```text
HTML / CSS / Canvas / WebKit
            |
        Cairo / WebCore
            |
         Pixman API
            |
    GFOpenAmigaPixman layer
       /      |       \
  040+FPU   AC090    GFOpenGPU
 portable   magic    / host path
       \      |       /
        Amiga bitmap memory
```

WebKit and Cairo should express normal rendering operations. They must not contain AC090-specific or AmiCradle-host-specific code for basic pixel composition.

GFOpenAmigaPixman owns backend selection and acceleration.

## Baseline Contract

The first production baseline is:

- AmigaOS 3.x;
- m68k;
- 68040 + FPU;
- big-endian;
- upstream Pixman 0.46.4 semantics;
- no TLS requirement;
- single-process Amiga library model;
- portable C fallback always available.

The FPU baseline is intentional for AmiCradle's target machine. It does not imply that every hot pixel operation should use floating point. Most blend, mask, fill and copy loops remain integer/memory-bound.

## ABI

`pixman.library` exposes a compact Amiga library ABI around upstream Pixman objects and operations.

Wide/hot calls use request blocks rather than consuming a large number of registers. This gives us a stable ABI while allowing the implementation underneath to change.

Priority ABI operations are:

1. image create/ref/unref;
2. Composite32;
3. Fill;
4. Blt;
5. FillRects;
6. image filter/repeat/transform setup;
7. region32 operations;
8. glyph compositing support.

The browser should be able to move between portable C, tuned 040 code and future accelerated implementations without relinking against a different graphics API.

## First Acceleration Targets

### Tier 1: Must Optimise First

These should dominate the first profiling campaign:

- 32-bit opaque fill;
- 32-bit source copy;
- ARGB OVER composite;
- A8 mask over ARGB destination;
- solid colour through A8 glyph mask;
- same-format blit;
- nearest-neighbour image scaling.

These map closely to normal web page painting and text.

### Tier 2: Browser Richness

After Tier 1:

- bilinear scaling;
- repeated image patterns;
- translucent layer composition;
- CSS/image opacity;
- shadow masks;
- Cairo temporary-surface composition;
- Canvas put/get image-data paths.

### Tier 3: Later Specialisation

- complex Porter-Duff operators;
- uncommon pixel formats;
- wide transforms;
- exotic filters;
- low-frequency PDF blend modes.

Correctness remains upstream Pixman first; rare operations can stay on the portable fallback until profiling proves otherwise.

## 68040 + FPU Strategy

The 68040 optimisation layer should focus first on:

- loop unrolling where it improves sustained memory throughput;
- alignment-aware 32-bit loads/stores;
- eliminating avoidable function calls in hot loops;
- locally cached constants;
- reduced branch pressure;
- specialised common-format fast paths;
- avoiding repeated format conversion;
- keeping destination writes linear.

FPU use is appropriate for transforms, matrix work, gradient calculations and selected filter setup where it wins measurably.

Do not convert integer alpha compositing to floating point merely because an FPU is available.

## AC090 / AC_MAGIC Strategy

The stable Pixman boundary is deliberately suitable for AC090 acceleration.

A future AC_MAGIC backend can recognise operations such as:

- FILL32;
- BLT32;
- SRC32;
- OVER32;
- A8_MASK_OVER32;
- GLYPH_MASK32;
- SCALE_NEAREST32;
- SCALE_BILINEAR32.

The request describes buffers, strides, rectangles, formats and operator. The accelerated implementation may execute in guest CPU code, a magic-instruction path or the AmiCradle host, provided observable Pixman semantics remain identical.

Fallback must be per-operation. Unsupported combinations return to the portable Pixman implementation rather than failing the browser.

## GFOpenGPU / Host Assistance

GFOpenGPU is an optional backend, not part of the semantic contract.

It may eventually accelerate large composites, image scaling and layer assembly. Small operations must remain local when offload overhead is larger than the work itself.

Backend choice should therefore use measurable thresholds, not simply "GPU available".

## Memory Rules

Browser performance will depend as much on movement as arithmetic.

The implementation should:

- minimise temporary image copies;
- wrap existing browser/Cairo buffers where safe;
- preserve stride information;
- prefer 32-bit native working surfaces;
- avoid gratuitous chunky-to-planar conversion in the rendering core;
- keep cacheable working surfaces in Fast RAM;
- defer display-format conversion to the presentation boundary.

For AmiCradle/GFOpenRTG, the ideal path is a chunky 32-bit browser surface through composition to presentation with the fewest possible copies.

## Browser Integration

The first browser integration does not require a new WebKit rendering API.

The sequence is:

1. build and qualify GFOpenAmigaPixman;
2. link the existing Cairo/WebKit dependency path against it;
3. verify identical rendering on a fixed browser corpus;
4. instrument Pixman operations and pixels processed;
5. profile page paint workloads;
6. optimise the dominant Pixman fast paths;
7. introduce AC090/GFOpenGPU dispatch beneath the same ABI.

This keeps correctness risk below WebKit.

## Instrumentation

A debug/profile build should record low-cost counters for:

- Composite32 calls and pixels;
- Fill calls and pixels;
- Blt calls and pixels;
- glyph/mask composites;
- filter type;
- format pair;
- fallback count;
- accelerated count;
- bytes read/written estimate;
- time spent per operation class.

Counters should be readable at shutdown or through a small diagnostic tool. They must be optional in release builds.

## Correctness Gate

Performance work must never change Pixman semantics silently.

Each accelerated fast path needs differential tests against the portable upstream implementation using identical input buffers.

Minimum cases include:

- zero and one-pixel rectangles;
- odd strides;
- unaligned starts;
- clipped edges;
- transparent/opaque alpha extremes;
- overlapping blits;
- masks;
- large images;
- representative browser-sized surfaces.

The browser qualification corpus should include text-heavy pages, image-heavy pages, translucent CSS, Canvas, scrolling and repaint.

## First Build Definition

The first build is complete when:

- upstream Pixman 0.46.4 builds for m68k AmigaOS;
- `pixman.library` links with no unresolved symbols;
- target flags are 68040 + FPU;
- the Amiga ABI smoke executable builds;
- Fill, image creation, Composite32 and region operations pass on the guest;
- the produced artifacts and source revision are recorded.

This is a foundation build, not yet a claim of HTML5 speedup.

## Near-Term Milestones

### B0 - Foundation
Build and guest-smoke `pixman.library` on 68040 + FPU.

### B1 - Browser Counters
Route the WebKit/Cairo Pixman path through GFOpenAmigaPixman and collect operation frequencies.

### B2 - 040 Fast Paths
Optimise the measured Tier 1 operations.

### B3 - AC090 Dispatch
Introduce AC_MAGIC-backed fill/blit/composite primitives with portable fallback.

### B4 - Browser Benchmark
Run repeatable browser page-paint, scroll, Canvas and image-composition benchmarks and compare against B0.

## Design Rule

**Pixman is our pixel engine boundary. WebKit describes what must be painted; GFOpenAmigaPixman decides the fastest correct way to paint it.**
