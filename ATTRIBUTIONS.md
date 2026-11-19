# Attributions

Needle is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### earshot — Stoatworks earshot

<https://github.com/stoatworks-labs/earshot>  
Licence: MIT  
Copyright: Stoatworks Labs

external/earshot/ is earshot, the fleet's shared audio front end, as a snapshot: one commit taken with git archive and left unmodified, named in external/earshot/SNAPSHOT. It is built into the plugin by add_subdirectory and supplies the capture device, chosen by name with a Silent state, and the audio analysis the plugin reads every frame. It brings miniaudio with it, credited below.

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

### The VFD's device menu and status lamp — Stoatworks woofer, and spasis before it

<https://github.com/stoatworks-labs/woofer>  
Licence: MIT  
Copyright: Stoatworks Labs

The VFD's Audio Input option, whose values are earshot device keys so a composition remembers the device by name, with None (host FFT), System default, the "(none)" spare slots and Rescan Inputs (source/Vfd.cpp, from woofer's source/Audio.cpp, spasis's design before it); and the status lamp's colours and states. Adapted.

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

### miniaudio, inside earshot

<https://github.com/mackron/miniaudio>  
Licence: Public Domain (Unlicense) or MIT-0, at your choice  
Copyright: David Reid

Arrives inside the vendored earshot snapshot, at external/earshot/third_party/miniaudio/miniaudio.h, unmodified. Not vendored separately.

earshot's capture backend: it lists the capture devices and runs the audio callback, on CoreAudio and WASAPI. The plugin never calls miniaudio itself; it arrives only with earshot.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### How a VFD is driven, and its standard phosphor — Noritake Itron

<https://www.noritake-elec.com/technology/general-technical-information/vfd-operation>

"VFD Operation": grids scanned one at a time, at least 90 to 100 times a second, with about 10 to 50 µs of blanking between them (the Multiplex Rate's range and the 20 µs blanking in source/Vfd.h). "Custom VFD Glass Design Guidance", Table 6: the standard green phosphor at CIE x 0.24, y 0.41, and the 0.18 mm minimum segment width. Read 2026-11-16; only the figures are used.

### The phosphor and the filament wires, from patents — NEC; Samsung SDI; Futaba

<https://patents.google.com/patent/US6803709B2>

NEC US5128063A (ZnO:Zn, the VFD phosphor of the highest efficiency) and Samsung SDI US6919683B1 (its emission peak near 505 nm) for what the standard green is; Futaba US6803709B2 (a conventional filament 37.5 µm thick overall), US4047074A (filaments across the characters) and US4190787A (the viewer looks through the wires at the phosphor) for the four wires drawn across the glass. Read 2026-11-16; only the figures are used.

## Standards and published specifications

What the implementation is measured against.

- **ANSI C16.5 (IEC 60268-17) standard volume indicator** — The VU: 99 % of a step to 0 VU in 300 ms and an overshoot of 1.0–1.5 %. Those two figures determine the movement's damping ratio and natural frequency, which are solved in code rather than chosen.
- **IEC 60268-10 type II peak programme meter** — The BBC PPM: a fall-back of 20 dB in 2.8 s, and a 5 ms tone burst reading 2 dB below the same tone held. Both are one-pole time constants the detector is solved from.
- **LM3915 dot/bar display driver** — Texas Instruments (originally National Semiconductor). Its internal divider chain is a 3 dB per step logarithmic ladder over ten steps, which is what the bargraph implements.
- **6U5 / EM84 tuning indicator** — The 100-degree shadow angle with no signal on the grid — the one published figure about a magic eye. Everything else about the eye (the level-to-angle map, the warm-up, the persistence, the wear) is this repo's own model.
- **IEC 61260 octave-band and fractional-octave-band filters, and ISO 266** — The VFD's bands: base-ten third-octave centres and edges, each octave as three thirds combined, and the class 1 mask the harness holds every band to, read in IS 6964:2001 (the Bureau of Indian Standards' identical adoption of IEC 61260:1995), with the ISO 266 labels from its Annex A. No text of either is reproduced beyond the values the harness needs.
- **CSS Color 4 (the sRGB primaries, white point and transfer function)** — The VFD's phosphor colour: Noritake's CIE (0.24, 0.41) lies outside sRGB, so source/Vfd.cpp maps it to the gamut boundary along the line to D65, with the matrix derived from the CSS Color 4 primaries and white rather than typed in.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
