# pixman.library 2.0: a thin layer over GFOpenGPU

The Team's decision, 17 October 2026: pixman.library becomes a thin passthrough to GFOpenRTG and GFOpenGPU.

- **The ABI stays.** pixman.library keeps its public interface: the LVOs in `library/pixman_lib.sfd`, unchanged since 1.0, and the request blocks in `include/libraries/pixman.h`.
- **Every image, composite, fill, blit and transform call becomes a GFOpenGPU 2D command.**
- **The pixel work is GFOpenGPU's.**
  - On AmiCradle, it runs on the host or the GPU, behind ACRTG.gpu.
  - On bare hardware, it runs in gfopengpu.library's own 68k back end.
- **No upstream Pixman is inside the library.** Upstream Pixman 0.46.4 stays only as the reference that `tests/pixmantest.c` compares against.

`BROWSER_ACCELERATION_DESIGN.md` set out why Pixman matters to the browser. This page replaces its backend plan.

## 1. Who calls pixman.library

| Caller | How it draws today | pixman.library? |
| --- | --- | --- |
| **cairo** (gfopencairo, 1.18.6) | links upstream Pixman statically (`-lpixman-1`, built by gfopencairo's `build.sh`) | no |
| **WebKit** (gfopenbrowser) | through cairo; `src/webcore/CMakeLists.txt` links `cairo pixman-1` | no |
| **SDL 2** (gfopenrtg `library/modules/sdl2`) | its own renderer, on GFOpenGPU | no |
| `tests/pixmansmoke.c` (PixmanTest), `bench/pixbench.c` (PixBench) | the library's calls | yes |

The real users today are only these two test programs. So 2.0 changes nobody's output: no shipped program loses an operation.

What cairo would need from pixman.library before it could link it in place of `libpixman-1.a` is what cairo calls. A grep of cairo 1.18.6's `src/*.c` finds 59 names:

- **In the ABI now:**
  - images: `pixman_image_composite32` (76 call sites), `_unref` (53), `_create_bits` (21), `_ref`, `_get_data`, `_create_solid_fill`, `_set_component_alpha`, `_get_stride`, `_set_filter`, `_set_transform`, `_set_repeat`, `_get_width`, `_get_height`, `_get_format`;
  - fills and blits: `pixman_fill` (9), `pixman_blt` (3);
  - regions: `pixman_region32_init_rect`, `_fini`, `_union`, `_init`, `_intersect`, `_copy`.
- **Not in the ABI:**
  - regions: `pixman_region32_subtract` (6), `_rectangles`, `_init_rects`, `_translate`, `_not_empty`, `_n_rects`, `_extents`, `_equal`, `_contains_rectangle`, `_contains_point`;
  - images: `pixman_image_set_destroy_function` (5), `_set_clip_region32` (3), `_get_component_alpha`, `_get_depth`, `_set_dither`, `pixman_format_supported_destination`;
  - geometry: `pixman_add_triangles`, `pixman_rasterize_trapezoid`;
  - gradients: `pixman_image_create_linear_gradient`, `_radial_gradient`;
  - glyphs: the glyph cache (create, lookup, insert, remove, freeze, thaw, destroy), `pixman_composite_glyphs`, `_glyphs_no_mask`, `pixman_glyph_get_mask_format`;
  - transforms: `pixman_transform_point_3d`.

Its operators, by call sites: SRC 20, OUT_REVERSE 18, ADD 14, LERP_SRC 9, CLEAR 9, OVER 7, and 3 each for XOR, OVER_REVERSE, OUT, IN, IN_REVERSE, SATURATE and the PDF blend modes (MULTIPLY, SCREEN, OVERLAY, SOFT_LIGHT, LIGHTEN, the HSL four). Its formats are a8r8g8b8 (ARGB32), x8r8g8b8 (RGB24), a8, a1 and r5g6b5.

## 2. The call map

`library/px_core.c` turns each call into one GFOpenGPU batch (stream v1.4, gfopenrtg `include/gfopengpu/stream.h`). It sends the batch with `OGPU_Submit` and waits for its fence, so the caller's pixels are there when the call returns, as Pixman's are.

The destination is surface slot 0 and the target. The source is slot 1, and the mask is slot 2.

| LVO | Pixman call | GFOpenGPU |
| --- | --- | --- |
| PX_Version, PX_VersionString | `pixman_version` | none: the interface's version, 0.46.4 (4604) |
| PX_ImageCreateBits | `pixman_image_create_bits` | none. The core keeps the format, the size, the bits (allocated and cleared when NULL) and the stride, and refuses formats GFOpenGPU doesn't have (section 3) |
| PX_ImageCreateSolid | `pixman_image_create_solid_fill` | none: the colour's top 8 bits a channel, premultiplied as given |
| PX_ImageRef, PX_ImageUnref | ref, unref | none |
| PX_ImageSetTransform, SetRepeat, SetFilter, SetComponentAlpha | the setters | none, kept for the composite |
| PX_ImageGetData, Width, Height, Stride, Format | the getters | none |
| PX_Composite32 | `pixman_image_composite32` | Clipped to the destination, then one of the three below |
| | a solid source with no mask or an opaque solid mask, with SRC, CLEAR, or OVER of an opaque colour | **FILL** with the pixel in the destination's format |
| | an image source in the destination's format, 1:1 and wholly inside it, no mask, with SRC, or OVER from a format with no alpha | **COPY** (overlap safe; one slot when source and destination share bits) |
| | everything else the core takes | **COMPOSITE_PD** (new in stream v1.4): the operator, solid or slot source, A8, ARGB32 or solid mask, REPEAT_NORMAL tiling, integer translations as offsets |
| PX_Fill | `pixman_fill` | **FILL**: 8, 16 and 32 bits a pixel, the value raw |
| PX_Blt | `pixman_blt` | **COPY**: 8, 16 and 32 bits, overlap safe. Pixman 0.46.4 has no portable `pixman_blt`, so 1.0 needed its own fallback for this |
| PX_ImageFillRectangles | `pixman_image_fill_rectangles` | **FILL** for each rectangle (SRC, CLEAR, OVER-opaque), else **COMPOSITE_PD** with a solid source. Many rectangles a batch (256 words) |
| PX_Region32Init, InitRect, Fini, Copy, Intersect, Union | the region32 calls | none: bookkeeping in Pixman's public layout, not pixel work (`library/px_region.c`, the Team's own band method) |

### COMPOSITE_PD, GFOpenGPU's part of this PR

gfopenrtg's branch `pixman/passthrough-20261017` adds COMPOSITE_PD to GFOpenGPU (stream v1.4).

- **Why it was needed.** GFOpenGPU's COMPOSITE blends straight (not premultiplied) colour, as SDL wants. Pixman's colour is premultiplied, and Pixman has the Porter-Duff operators. OVER of a premultiplied source cannot be made from COMPOSITE's OVER, ADD or IN without un-premultiplying every pixel on the 68k first.
- **What it does.**
  - It takes Pixman's operator numbers, CLEAR to ADD.
  - The source is a slot or a solid colour.
  - The mask is an A8 or ARGB32 slot, or a solid alpha.
  - Either can be tiled.
  - The arithmetic is Pixman's: each product x × y / 255 rounded, sums saturating, and a pixel outside a source or mask counts as 0.
- **Where it runs.** gfopengpu.library's 68k back end runs it, and so does any back end built from the same `ogpu_core.c`.

## 3. Formats

| Pixman | GFOpenGPU | |
| --- | --- | --- |
| a8r8g8b8 | ARGB32 | bytes A, R, G, B: the Amiga's order |
| x8r8g8b8 | XRGB32 | X written as 0 (Pixman may write alpha there; both leave it unused) |
| b8g8r8a8 | BGRA32 | |
| r5g6b5 | RGB565 | 5 and 6 bits from 8 by truncation, back by repeating the top bits, as Pixman |
| x1r5g5b5 | RGB555 | |
| a8 | A8 | |

The others are refused at PX_ImageCreateBits, which returns NULL (section 4).

## 4. Gaps: what GFOpenGPU and GFOpenRTG can't do yet

The core refuses these: it counts them (`px_stats.refused`) and draws nothing. `tests/pixmantest.c` checks that a scaling transform, PAD, a component-alpha mask and SATURATE are each refused and leave the pixels alone. Each gap belongs in GFOpenGPU or GFOpenRTG, not here.

| # | Gap | Where it goes | Size |
| --- | --- | --- | --- |
| 1 | Premultiplied Porter-Duff | GFOpenGPU | **done here** (COMPOSITE_PD, stream v1.4) |
| 2 | SATURATE, and the disjoint and conjoint operators | GFOpenGPU, more COMPOSITE_PD operators | small. SATURATE needs a divide by the source alpha, a table on the 68k |
| 3 | The PDF blend modes (MULTIPLY, SCREEN, OVERLAY, DARKEN, LIGHTEN, COLOR_DODGE, COLOR_BURN, HARD_LIGHT, SOFT_LIGHT, DIFFERENCE, EXCLUSION, HSL_HUE, HSL_SATURATION, HSL_COLOR, HSL_LUMINOSITY) | GFOpenGPU, more operators | medium. SOFT_LIGHT and the HSL four need square roots or divides, done exactly in integers |
| 4 | Transforms beyond an integer translation (scale, rotate, projective) with NEAREST, BILINEAR and the convolution filters | GFOpenGPU: COMPOSITE_AFFINE samples straight colour; a premultiplied, Pixman-exact affine and projective COMPOSITE_PD form is needed | **big**: Pixman's sample positions and bilinear weights (7-bit) must be matched exactly, and the projective divide done in integers |
| 5 | REPEAT_PAD and REPEAT_REFLECT | GFOpenGPU, two more COMPOSITE_PD repeat modes | small |
| 6 | Component-alpha masks (subpixel text) | GFOpenGPU, a COMPOSITE_PD flag, each channel its own coverage | small |
| 7 | Formats: a1 and a4 masks, r8g8b8 (24 bits), a8b8g8r8 and x8b8g8r8, b8g8r8x8, r8g8b8a8, a1r5g5b5, b5g6r5, a4r4g4b4, grey and indexed, the float and sRGB formats | GFOpenGPU formats (stream.h's list) | medium. a1 and r8g8b8 first, for cairo |
| 8 | Gradient sources (linear, radial, conical) | GFOpenGPU, a gradient source for COMPOSITE_PD | medium |
| 9 | Trapezoids, triangles, the glyph cache | the ABI first (not in it), then GFOpenGPU: an A8 rasteriser for edges, and glyph runs as one batch of MASK or COMPOSITE_PD commands | big |
| 10 | **COMPOSITE_PD on the host.** ACRTG.gpu answers GFOpenGPU's queries from a list of the opcodes it knows (amicradle-guest `libraries/acrtg.gpu`), and the Cradle's runtime runs a pinned copy of gfopenrtg's core (amicradle `vendor/openamigartg`). Neither has v1.4 yet. Until both do, a batch with COMPOSITE_PD runs on gfopengpu.library's 68k back end. FILL and COPY reach the host now | amicradle-guest (ACRTG.gpu: v1.4 behind the runtime's minor version), amicradle (the pin, and `ogpu_vk.c` for the GPU) | small, then a runtime deploy |
| 11 | Images in Fast RAM out of the GPU's reach under Vulkan | GFOpenGPU's 2D design (amicradle docs/design, Design-GFOpenGPU-2D-On-The-GPU, section 5.4) | as there |
| 12 | A screen's bitmap as a pixman image (drawing straight into a GFOpenRTG screen) | GFOpenRTG (`ORTG_BitMapInfo` gives address and format) plus one new LVO here | small |

## 5. Stages

1. **Done (this PR, gfopenpixman and gfopenrtg):**
   - fills, blits and rectangle fills;
   - every composite of the operators CLEAR to ADD, from solid or image sources, with A8, ARGB32 or solid masks, repeat NONE or NORMAL, and integer translations, into the six formats;
   - regions.
2. **Gaps 2, 5, 6 and 7** (a1, r8g8b8), in COMPOSITE_PD, with gap 10, so the common browser operations run on the host.
3. **Gaps 4 and 8**: transforms, filters and gradients.
4. **The rest of cairo's calls in the ABI** (section 1, gap 9). Then a `libpixman-1.a` stub that calls pixman.library, so cairo and WebKit use it unchanged.

## 6. Floating point

pixman.library 2.0 has none:

- Pixman's interface is fixed point: transforms and filter parameters are 16.16 integers.
- The core works in integers.
- GFOpenGPU's core is integer only.

So the library is built `-m68040 -msoft-float` and runs on an LC040 or EC040 too. `build-library.sh` fails if an FPU instruction gets in. 1.0 had 6,955 FPU instructions; 2.0 has none.

PixBench and PixmanTest still use the FPU for their timing arithmetic, as before.

## 7. Tests

- **`tests/run.sh [CASES]`.** It builds `tests/pixmantest.c` for a big-endian 68040 (m68k-linux-gnu-gcc) and runs it under qemu-m68k. Inside it are:
  - the library's core;
  - GFOpenGPU's core (`vendor/openamigartg`, the C that gfopengpu.library's 68k back end and the Cradle's runtime run);
  - upstream Pixman 0.46.4, built from its release tarball for the test only.
- **What it compares.** It runs random composites, fills, blits, rectangle fills and region unions, intersections and copies, and compares the bytes. It must be pixel-exact, apart from the bits a format leaves unused, and the core must refuse nothing.
- **The tarball.** The script does not fetch it: put `pixman-0.46.4.tar.gz` in `tarballs/` (`fetch-sources.sh` fetches it, sha256 checked).
- **On AmigaOS 3.2.3.** PixmanTest (`build-test.sh`) is the smoke test, and PixBench (`build-bench.sh`) times the library on the AC090 (68040) with GFOpenGPU's driver and without it. Its binary is unchanged from 1.0, so the figures compare directly. They are in `measurements/`.

## 8. Measured (AC090, 68040, OS 3.2.3, 17 October 2026)

`measurements/20261017-2.0.md`: PixBench, 640×480, medians of three rounds, µs a call.

| | 1.0 (upstream Pixman inside) | 2.0 with ACRTG.gpu | 2.0, GFOpenGPU's 68k back end |
| --- | --- | --- | --- |
| fill32 | 169 | **68** | 699 |
| blt32 | 473 | 1120 | 723 |
| src32 | 462 | 1614 | 747 |
| over32 | 10632 | 14871 | 19154 |
| mask_a8_over32 | 10613 | 21945 | 36098 |
| fillrects24 | 306 | **44** | 372 |
| browser paint | 10405 | 14845 | 21337 |

- **Fills.** They win at once, because GFOpenGPU's FILL runs on the host.
- **Composites.** They are slower until COMPOSITE_PD runs on the host (gap 10): today they run on gfopengpu.library's 68k back end, which loses to upstream Pixman's 68k code by 1.4 to 3.4 times.
- **COPY.** Through the host it is slower than on the 68k for Fast RAM images. That is GFOpenGPU's host path, and it belongs with gaps 10 and 11.
- **Size.** The library is 16 KB, against 1.0's 403 KB.

## 9. COMPOSITE_PD on the host (17 October 2026, later)

Gap 10 is being closed by three pull requests:

- **amicradle:** the runtime's core repinned to stream v1.4 and built with `OGPU_HOST`. It copies rows with memmove, and does the image OVER and the solid-through-A8 OVER on whole pixels, with the same bytes.
- **amicradle-guest:** ACRTG.gpu takes COMPOSITE_PD from a minor-4 runtime.
- **gfopenrtg:** the host paths in the core.

That runtime has not been deployed yet. Lab t4-g-px ran it as a build of that branch (`lab_instance.py --binary`). The figures are PixBench, 640×480, medians of three, µs a call, with each 2.0 run alternating with a 1.0 run on the deployed runtime. The host was heavily loaded (load average 33 to 39 on 12 threads), so single runs vary by up to 8 times.

| PixBench | 1.0 | 2.0, COMPOSITE_PD on the host |
| --- | --- | --- |
| fill32 | 210 | 81 |
| blt32 | 331 | 562 |
| src32 | 369 | 108 |
| over32 | 9003 | 2672 |
| mask_a8_over32 | 9953 | 2660 |
| fillrects24 | 409 | 104 |
| browser paint | 23842 | 5313 |

## 10. The gaps in the order WebKit on GFOpenGPU needs them

cairo is leaving the design, so WebKit will draw on GFOpenGPU directly. Its order differs from cairo's: a1 masks, SATURATE and the disjoint and conjoint operators were cairo's needs, and they drop to the end.

1. **Scaled images:** a premultiplied affine COMPOSITE_PD with bilinear filtering and PAD at the edges. Every `<img>` and background drawn at another size, and every page at a device-pixel ratio other than 1, needs it. This is gaps 4 and 5.
2. **Anti-aliased path coverage:** an A8 mask rasteriser for paths, for rounded-rect borders and clips, SVG and canvas fills and strokes. This is gap 9, edges first. Glyphs come later, from the font engine.
3. **Linear and radial gradients:** CSS gradients, and canvas gradients (with conical). This is gap 8, with PAD.
4. **Text:** glyph runs as one batch of MASK or COMPOSITE_PD commands; component-alpha masks for subpixel text. This is gaps 6 and 9.
5. **Blur:** separable convolution for box-shadow, text-shadow and `filter: blur()`. This is gap 4's filters, on its own.
6. **Projective transforms:** CSS 3D transforms, gap 4's other half.
7. **The PDF blend modes:** `mix-blend-mode` and canvas `globalCompositeOperation`. This is gap 3.
8. **Byte orders:** RGBA and ABGR, as image decoders and canvas ImageData hand them over (gap 7's a8b8g8r8 and r8g8b8a8). PIXELS can convert them on the way in.
9. **Last, or not at all:** REFLECT, SATURATE, disjoint and conjoint, a1 and a4, 24-bit and the other formats.

**On the host's side, beside these:**
- COMPOSITE_PD on the GPU, in `ogpu_vk.c`, for large or scaled composites once the cost model says the GPU wins;
- Fast RAM reach (gap 11), so composites into a page's backing store can run there.

