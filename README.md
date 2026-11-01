# GFOpenAmigaPixman

`pixman.library` for AmigaOS 3.x gives Pixman's interface to Amiga programs. From 2.0 it is a thin layer over GFOpenGPU, and GFOpenGPU draws the pixels:

- on AmiCradle, on the host, behind GFOpenGPU's ACRTG.gpu driver;
- on any Amiga, in gfopengpu.library's own 68k back end.

No upstream Pixman is inside the library. Upstream Pixman 0.46.4 is the reference that the tests compare against, pixel for pixel. `DESIGN.md` has the call map, the gaps and the stages.

## Baseline

- **Interface:** Pixman 0.46.4's types and calls, through the LVOs in `library/pixman_lib.sfd` (unchanged from 1.0) and the request blocks in `include/libraries/pixman.h`.
- **Library version:** 2.1.
  - 2.1 (1 November 2026): region unions and intersections grow their output as they go and walk the bands in one pass. Before, a region of a few hundred boxes asked for a block of megabytes, and the call failed on an Amiga.
  - 2.0 (17 October 2026): the thin layer over GFOpenGPU, with no upstream Pixman inside.
- **Needs:** gfopengpu.library with GFOpenGPU stream v1.4 (COMPOSITE_PD; DalsinAI/openamigartg). Without it, `OpenLibrary("pixman.library")` fails.
- **CPU:** 68040, soft float. The library has no floating point at all, so it also runs on an LC040 or EC040. The build fails if an FPU instruction gets in.
- **Formats:** a8r8g8b8, x8r8g8b8, b8g8r8a8, r5g6b5, x1r5g5b5 and a8.
- **Operators:** CLEAR to ADD.
- **Sources:** solid colours and images, with repeat NONE or NORMAL and integer translations.
- **Masks:** A8, ARGB32 or a solid alpha.
- **Not yet:** the rest are refused and counted (`DESIGN.md`, "Gaps").

## Build

```sh
./build-library.sh      # out/lib/pixman.library (about 16 KB)
./build-test.sh         # out/tests/PixmanTest, the smoke test (tests/pixmansmoke.c)
./build-bench.sh        # out/tests/PixBench
tests/run.sh [CASES]    # pixman.library against upstream Pixman, on qemu-m68k
```

- **The stove.** All four use the os32-gcc16 stove (`STOVE=`).
- **The library's only outside code** is GFOpenGPU's batch builder, `vendor/openamigartg/library/opengpu/ogpu_build.c`. `vendor/openamigartg/OPENAMIGARTG_COMMIT` says which openamigartg commit it comes from.
- **The test needs** `m68k-linux-gnu-gcc` and `qemu-m68k`, and the Pixman release tarball in `tarballs/`. The test never fetches it: put it there, or run `fetch-sources.sh`.
- **`build-static.sh`** still builds upstream Pixman for AmigaOS as `libpixman-1.a`, for comparisons. pixman.library doesn't use it.

## Tests

- **`tests/run.sh`** runs `tests/pixmantest.c`. It is built for a big-endian 68040 and runs under qemu-m68k. It runs the library's core and GFOpenGPU's core beside upstream Pixman, on random cases:
  - composites: every operator and format above, solid and image sources and masks, repeat, translations and clipping;
  - fills, blits and rectangle fills;
  - region unions, intersections and copies, also of regions with thousands of boxes, where no block asked for may be more than about twice the boxes in and out.

  The results must be pixel-exact, apart from the bits a format leaves unused, and nothing may be refused. It also checks that a scaling transform, PAD repeat, a component-alpha mask and SATURATE are refused and leave the pixels alone.
- **PixmanTest** is the ABI smoke test on AmigaOS.
- **PixBench** is the correctness and timing harness on AmigaOS. It uses `timer.device` E-clock timing, and its binary is unchanged from 1.0.
- **Measurements.** `measurements/20261017-2.0-*` has 1.0 and 2.0 on AmiCradle's AC090 (68040, OS 3.2.3), with GFOpenGPU's driver and without it.

## Browser

`BROWSER_ACCELERATION_DESIGN.md` explains why a fast 2D layer under the browser matters. cairo and WebKit link upstream Pixman statically today (openamigacairo). `DESIGN.md` section 1 lists what cairo calls, and so what pixman.library's interface still needs before cairo can use it.

## AC090's native helpers

`achelpers/` (SPD-20) belonged to 1.0. It routed Pixman's copies and fills through amigachrome-guest's AC090 magic functions. 2.0 has no copies or fills of its own: they are GFOpenGPU's FILL and COPY. `build-static.sh` still uses the helpers when it builds upstream Pixman.

## Licensing

OpenAmigaPixman's library, ABI and test code are MIT licensed by the AmigaChrome Team. The files in `vendor/openamigartg` keep openamigartg's MIT licence. Upstream Pixman, used by the tests and `build-static.sh` only, keeps its own MIT licence and copyright notices.
