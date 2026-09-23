# demo/ — the browser demo

Live at **<https://needle-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file and `tools/` out of the upload.

    index.html    the shell
    plugin.js     this plugin's parameters, its ported engine, its shaders
    vendor/       the shared kit, copied in from stoatworks-backend — DO NOT EDIT
    tools/        check_shaders.py, run by tools/verify.sh
    _headers      CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

A **port**, not a recording and not the plugin.

The shader is the plugin's, copied across unedited. `VERTEX_SHADER`,
`FRAGMENT_SHADER_A` and `FRAGMENT_SHADER_B` at the top of `plugin.js` are the
three raw strings in `source/Shaders.cpp` — the fragment shader is two adjacent
raw strings there because MSVC caps a literal at about 16 KB — and the page joins
A + B exactly as the C++ compiler does. `tools/check_shaders.py` compares every
raw string and the joined fragment text character for character, and
`../tools/verify.sh` runs it.

The CPU half is a port: `meter/Standards`, `meter/Movement`, `meter/Engine`,
`Audio::LevelFromSpectrum`, `Controls.h`, `NeedlePlugin::BuildFrame`,
`Renderer::Draw`'s uniform packing and `Font.cpp`. **Nothing checks the port
but a reader.** Change any of those in C++ and the page silently disagrees.

**There is no audio.** The plugin's only input is Resolume's 64-bin FFT buffer.
The page generates a test signal instead — chosen in the "Test signal" dropdown —
integrates its power over each frame's interval and writes it into 64 bins as
magnitudes (a tone into one bin; pink noise as 1/f with a per-bin exponential
scatter). The plugin's own level law runs on those bins. **It is not the host's
FFT**: Resolume's window, normalisation and bin law are unknown and are not
reproduced; the page says so in its banner, its disclosure, under the transport
and in the hints on Reference Level, Sensitivity and Bin Law.

Everything else is not the plugin either: no Resolume, no composition, no FFGL,
GLSL ES 3.00 in WebGL2 rather than desktop GL 4.1 core. A pixel here is not a
measurement of a pixel there, and a needle here is not a measurement of the
ballistics — `ndtest` is.

## What is deliberately absent

- **The `Audio` buffer parameter.** A host writes it, never an operator; the
  test signal stands in for it.
- **The clip picker and the "use my own file" button.** Needle is a source with
  zero inputs. The kit builds both for every demo and `plugin.js` removes them.
- **The About block.** A text line and four link buttons exist so a *host* has
  somewhere to put them. A web page has links of its own.
- **The plugin's `Clock`.** The kit's clock is already in seconds.

## Working on it

```bash
python3 -m http.server 8947          # from this directory
python3 tools/check_shaders.py       # the copies still match the C++
../tools/verify.sh                   # everything, including the above
```

There is no build step. It is hand-written ES modules and what is committed is
what is served.

**After changing `source/Shaders.cpp`, copy it across here too** —
`check_shaders.py` will tell you which string and where the first difference
is. Do not edit the GLSL in `plugin.js` to make something compile in WebGL2:
`port()` in `vendor/gl.js` handles the version line and the precision qualifiers
and nothing else, and if a shader will not compile here the answer is to say so
on the page.

**`vendor/` is a copy.** Fix the kit in
`stoatworks-backend/resolume-demo/kit/` and re-run
`stoatworks-backend/resolume-demo/sync.sh needle`.

## Deploying

From the **repository root**, not from here:

```bash
cf-run npx wrangler deploy
curl -s 'https://needle-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

Verify by **content**, never by status code: a stale page returns a cheerful 200.

## Embed mode

`?embed=1` renders the output and nothing else, so the page can be a video
source. `?size=1920x1080`, `?bg=black|checker|white` and any parameter id work
as query parameters. The test signal is the kit's variant and is not in the URL,
so an embed runs the default 0 VU step.

    https://needle-demo.stoatworks-labs.com/?embed=1&size=1920x1080&type=1
