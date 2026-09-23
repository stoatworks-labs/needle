# Attributions

Needle is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Graticule bitmap font — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Font.{h,cpp} — the 5×7 bitmap font every scale number and legend is drawn from — carried across from graticule unchanged.

### Graticule Diag logger — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

The Diag log-file writer, carried across from graticule. It records the build, the GL driver, the solved ballistic constants and whether any audio reached the layer.

### Graticule host Clock — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

The Clock that measures what unit the host's SetTime arrives in, carried across from graticule.

### Graticule offline harness plumbing — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

The shape of the PNG writer and the headless GL plumbing in the ndtest harness follow graticule's. The harness only; nothing of it ships in the plugin.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Standards and published specifications

What the implementation is measured against.

- **ANSI C16.5 (IEC 60268-17) standard volume indicator** — The VU: 99 % of a step to 0 VU in 300 ms and an overshoot of 1.0–1.5 %. Those two figures determine the movement's damping ratio and natural frequency, which are solved in code rather than chosen.
- **IEC 60268-10 type II peak programme meter** — The BBC PPM: a fall-back of 20 dB in 2.8 s, and a 5 ms tone burst reading 2 dB below the same tone held. Both are one-pole time constants the detector is solved from.
- **LM3915 dot/bar display driver** — Texas Instruments (originally National Semiconductor). Its internal divider chain is a 3 dB per step logarithmic ladder over ten steps, which is what the bargraph implements.
- **6U5 / EM84 tuning indicator** — The 100-degree shadow angle with no signal on the grid — the one published figure about a magic eye. Everything else about the eye (the level-to-angle map, the warm-up, the persistence, the wear) is this repo's own model.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
