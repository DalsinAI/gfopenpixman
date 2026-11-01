# GFOpenAmigaPixman Harness

Guest-side qualification and performance harness for `pixman.library`.

## Scope

The harness runs on AmigaOS 3.x and uses `timer.device` E-clock timing. It is built for the AmiCradle 68040 + FPU target.

It performs deterministic correctness checks and benchmarks:

- 32-bit fill;
- 32-bit blit;
- 32-bit SRC compositing;
- 32-bit OVER compositing;
- A8-mask OVER compositing (glyph-shaped path);
- batched rectangle fill;
- a mixed 640x480 browser-paint workload.

Each primitive reports microseconds per call, megapixels/s, effective MiB/s and a destination hash. The mixed workload reports synthetic frames/s and touched megapixels/s.

## Output Contract

Machine-readable result lines begin with `HARNESS`, `CHECK`, `BENCH` or `BROWSER`.

A run is valid only when it ends with `HARNESS PASS`. Correctness failure returns AmigaDOS exit code 20.

## Build

Run `./build.sh`. By default it uses the frozen first-build snapshot at `~/openamigapixman-buildcheck`. Set `PIXMAN_ROOT` to qualify another candidate without changing the harness.

## Benchmark Method

Each benchmark warms the operation, adaptively chooses a loop count that reaches at least about 180 ms (250 ms for the browser mix), runs three timed samples and reports the median.

The reported MiB/s is an effective traffic model, not a claim about physical bus traffic. It is intended for before/after comparison across GFOpenAmigaPixman implementations.
