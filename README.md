# Unified XUL Platform (UXP) — Tiger PPC Fork

Fork of UXP with patches for cross-compiling to **Mac OS X 10.4 Tiger** on **PowerPC** (G3/G4/G5).

This is the platform layer for [MachFox](https://github.com/danupsher/machfox-browser), a Tiger PPC web browser.

## Tiger PPC Changes

- OpenGL compositor fixes for GLSL 1.05 / OpenGL 1.5 (Radeon 9600 era GPUs)
- Buffer rotation disabled for non-NPOT GPUs
- FBO intermediate surfaces disabled (fixes Y-flip rendering)
- GL context view attachment timing fix for Tiger compositor thread
- Cairo font rendering fixes for Tiger
- Various 10.4 SDK compatibility patches

## Building

Cross-compiled from Linux using the [PPC Tiger Cross-Compiler v1.3](https://github.com/danupsher/tiger-ppc-builds/releases/tag/gcc15-xcompiler-1.3) (GCC 15.2.0 + ld64).

See the [MachFox browser repo](https://github.com/danupsher/machfox-browser) for build instructions and `mozconfig`.

## Releases

- **[v3.1](https://github.com/danupsher/platform-uxp/releases/tag/tiger-ppc-v3.1)** — Re-linked with G3-safe runtime libraries. Fixes G4/G3 crash.
- **[v3.0](https://github.com/danupsher/platform-uxp/releases/tag/tiger-ppc-v3.0)** — Initial release with GPU compositing.

## Upstream

Based on UXP from [ArcticFoxie/ArcticFox](https://github.com/ArcticFoxie/ArcticFox) / Pale Moon. See `master` branch for unmodified upstream.

## License

Mozilla Public License v2.0. See individual directories for additional trademark notices.
