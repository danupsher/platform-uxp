# Unified XUL Platform (UXP) — Tiger PPC Fork

Fork of UXP with patches for cross-compiling to **Mac OS X 10.4 Tiger** on **PowerPC** (G3/G4/G5).

This is the platform layer for [MachFox](https://github.com/danupsher/machfox-browser), a Tiger PPC web browser.

## Tiger PPC Changes

### JIT / JavaScript
- Full PPC JIT backend — Baseline JIT + Ion optimizing compiler (35+ PPC-specific fixes)
- Baseline JIT now compiles all web page JavaScript (interpreter-only restriction removed)
- Ion for internal scripts only (web content Ion bailout fix in progress)
- r28 register reserved in NonAllocatableMask (root cause of systemic Ion crash)
- Native regexp with big-endian multi-character load fix (CanReadUnaligned)
- outOfLineTruncateSlow implemented for PPC
- Ion optimizations: GVN, inlining, PGO, range analysis, OSR all enabled
- MIn VM call disabled on PPC BE (genuine value-passing bug, zero practical impact)

### Media
- H.264 decode via ffvpx with PPC AltiVec SIMD acceleration
- FFVPXRuntimeLinker fixed for XP_DARWIN (dlopen/dlsym)

### GPU / Compositing
- OpenGL compositor fixes for GLSL 1.05 / OpenGL 1.5 (Radeon 9600 era GPUs)
- Buffer rotation disabled for non-NPOT GPUs
- FBO intermediate surfaces disabled (fixes Y-flip rendering)
- GL context view attachment timing fix for Tiger compositor thread

### Platform
- Cairo font rendering fixes for Tiger
- Various 10.4 SDK compatibility patches

## Building

Cross-compiled from Linux using the [PPC Tiger Cross-Compiler v1.3](https://github.com/danupsher/tiger-ppc-builds/releases/tag/gcc15-xcompiler-1.3) (GCC 15.2.0 + ld64).

See the [MachFox browser repo](https://github.com/danupsher/machfox-browser) for build instructions and `mozconfig`.

## Releases

- **[v3.4](https://github.com/danupsher/platform-uxp/releases/tag/tiger-ppc-v3.4)** — Baseline JIT for all web content (was interpreter-only). Ion overflow bailout fix. G5 only.
- **[v3.3](https://github.com/danupsher/platform-uxp/releases/tag/tiger-ppc-v3.3)** — Full Ion JIT, native regexp, r28 fix, H.264 AltiVec. YouTube working.
- **[v3.1](https://github.com/danupsher/platform-uxp/releases/tag/tiger-ppc-v3.1)** — Re-linked with G3-safe runtime libraries. Fixes G4/G3 crash.
- **[v3.0](https://github.com/danupsher/platform-uxp/releases/tag/tiger-ppc-v3.0)** — Initial release with GPU compositing.

## Upstream

Based on UXP from [ArcticFoxie/ArcticFox](https://github.com/ArcticFoxie/ArcticFox) / Pale Moon. See `master` branch for unmodified upstream.
