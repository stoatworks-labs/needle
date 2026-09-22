# Attributions

Needle is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Needle is not
> registered there yet, so this copy is hand-written. Register it before release
> — and note that the script's `--only` flag truncates the file rather than
> filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL source is defined by this SDK's headers — there is
no other way to be loadable by Resolume Arena and Avenue. It is also the only
route audio takes into this plugin: the `FF_USAGE_FFT` buffer parameter.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for the
OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Linked by the offline harness only, never by the plugin, to deflate the PNGs
`ndtest --out` writes. It ships with macOS.

## Work from elsewhere in the fleet

### Graticule

<https://github.com/stoatworks-labs/graticule>
Licence: MIT
Copyright: Stoatworks Labs

`source/Font.{h,cpp}` — the 5×7 bitmap font every scale number is drawn from —
is carried across unchanged, along with the `Diag` logger, the `Clock` that
measures what unit the host's `SetTime` arrives in, and the shape of the PNG
writer and GL plumbing in the harness.

## The specifications this plugin implements

Not code, not linked, not shipped: these are the published numbers the ballistics
are solved from. Each is quoted in `source/meter/Standards.h` next to the
constant it determines.

### ANSI C16.5 — the VU meter

A standard volume indicator reaches 99 % of its steady deflection in 300 ms and
overshoots by 1.0 % to 1.5 %. Those two figures determine the damping ratio and
the natural frequency of the movement, and nothing else in the VU here is a free
choice. Also published as IEC 60268-17.

### IEC 60268-10 — the peak programme meter

Type II (the BBC PPM): a fall-back of 20 dB in 2.8 s, and a 5 ms tone burst
reading 2 dB below the same tone held.

### The LM3915 dot/bar display driver

Texas Instruments (originally National Semiconductor). Its internal divider
chain is a **3 dB per step** logarithmic ladder over ten steps, which is what
every LED programme meter built on it does and what the bargraph here does.

### The 6U5 / EM84 tuning indicator

The shadow angle with no signal on the grid — 100 degrees — is the one figure
about a magic eye that is published rather than chosen. Everything else about
the eye here (how the angle maps to level, the heater warm-up, the phosphor
persistence, the wear) is this repo's own model and `AGENTS.md` says so.
