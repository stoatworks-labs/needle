"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds the GLSL of `source/Shaders.cpp`. That is two copies of
the same text, and two copies drift -- quietly, because a demo that renders a
*plausible* meter looks exactly like a demo that renders the right one. The
whole claim of the page is that it runs the plugin's own shader rather than
something reimplemented to look similar, so the claim needs something enforcing
it. Nothing else can: `ndtest` drives the real plugin class and has no idea the
page exists.

------------------------------------------------------------------- what it does

`kFragmentShader` is TWO adjacent raw strings in the C++, because MSVC caps one
string literal at about 16 KB (C2026) and the fragment shader is over it. The
page carries them as two constants, `FRAGMENT_SHADER_A` and `FRAGMENT_SHADER_B`,
and joins them the way the C++ compiler joins adjacent literals -- with nothing
between. So this compares:

  * every raw string, one against one, exactly -- no whitespace normalisation,
    no comment stripping, because the comments in Shaders.cpp carry the
    reasoning (why coverage is a one-pixel linear ramp, why the pivot is masked);
  * the joined fragment text against the joined C++ text, so a split moved from
    one place to another is still caught;
  * that the C++ fragment shader is still exactly two raw strings -- if a third
    appears, the page's two constants cannot be the whole shader and this fails
    rather than comparing a prefix;
  * that plugin.js still joins the two with `FRAGMENT_SHADER_A + FRAGMENT_SHADER_B`.

Since v0.2.0 the page also runs the plugin itself, for Type = VFD: the C++
compiled to WebAssembly (demo/tools/build-wasm.sh). So this also checks:

  * `VFD_FRAGMENT_SHADER` in `demo/vfd.js` against `kVfdFragmentShader` in
    `source/VfdShaders.cpp` (one raw string), exactly. The compiled plugin
    hands GL that shader itself, and the page compiles it only if it equals
    this copy, so a stale copy stops the VFD rather than drawing something
    else; this makes the drift a verify failure instead of a dark page.
  * ndtest's harness. `demo/wasm/glue.cpp` feeds the VFD with ndtest's own
    signal functions -- `kPi` and `AmpFromDb` (tools/ndtest/main.mm),
    `kFeedRate`, `Signal`, `BandHz`, `Fade`, `Programme` and `PerBandDb`
    (tools/ndtest/Vfd.inc) -- each with its doc comment, and each must be
    ndtest's text character for character, so the page's "harness's
    programme" is what `ndtest --out` feeds a VFD.
  * No device. No file the page serves (everything in demo/ but tools/ and
    wasm/, which .assetsignore keeps off the site) may name getUserMedia,
    enumerateDevices or mediaDevices: the page never asks for a microphone or
    lists an audio device, and this keeps it so.
  * The WebAssembly. `demo/needle-core.wasm` is committed, because the deploy
    has no build step, and building it needs emscripten, which verify.sh cannot
    assume. So this checks the next best thing: `demo/wasm/inputs.sha256`,
    written by the build, records the hash of every file that went in and of
    both outputs, the SDK commit and the version CMakeLists.txt declared. A
    source file changed or added since the last build (a regenerated
    StoatworksAbout.h, a new earshot snapshot), a version bump, an SDK moved,
    or an output that is not the one the build wrote fails here, and the fix
    is to rerun build-wasm.sh. It cannot tell whether the .wasm really is what
    those inputs compile to; only rebuilding can.

The one transformation is a decode, not a normalisation. A comment in the shader
quotes `origin` in backticks, and a backtick cannot appear raw inside a
JavaScript template literal, so `plugin.js` escapes it as \\`. This unescapes
that and *rejects any other backslash on the JS side* -- there are none anywhere
in the C++, so a second escape could only be somebody hiding a difference. A
`${` would be interpolated by JavaScript, so it is rejected too.
"""
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CPP = "source/Shaders.cpp"
VFD_CPP = "source/VfdShaders.cpp"
VFD_JS = "demo/vfd.js"
GLUE = "demo/wasm/glue.cpp"
MANIFEST = "demo/wasm/inputs.sha256"
OUTPUTS = ("demo/needle-core.js", "demo/needle-core.wasm")
# What build-wasm.sh does not compile: the registration (the page does its
# job) and earshot's device backend (EARSHOT_CAPTURE=OFF).
REGISTRATION = ("SourcePlugin.cpp",)
NOT_COMPILED = ("Miniaudio.cpp", "MiniaudioImpl.cpp")
DEVICE_APIS = ("getUserMedia", "enumerateDevices", "mediaDevices")
# ndtest's signal functions the glue carries: (file, what, regex for the
# declaration's first line). Each is compared with the /// lines above it.
HARNESS = [
    ("tools/ndtest/main.mm", "kPi", r"constexpr double kPi = .*;"),
    ("tools/ndtest/main.mm", "AmpFromDb", r"double AmpFromDb\( double db \)"),
    ("tools/ndtest/Vfd.inc", "kFeedRate", r"constexpr int kFeedRate = .*;"),
    ("tools/ndtest/Vfd.inc", "Signal", r"using Signal = .*;"),
    ("tools/ndtest/Vfd.inc", "BandHz", r"double BandHz\( int x \)"),
    ("tools/ndtest/Vfd.inc", "Fade", r"double Fade\( double t, double ramp \)"),
    ("tools/ndtest/Vfd.inc", "Programme", r"Signal Programme\(.*\)"),
    ("tools/ndtest/Vfd.inc", "PerBandDb", r"double PerBandDb\( double totalDb \)"),
]


def cpp_strings(source, symbol):
    """The raw-string bodies making up `symbol`, in order, or None."""
    match = re.search(
        r'const char\* const ' + symbol + r'\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;',
        source,
        re.S,
    )
    if match is None:
        return None
    return re.findall(r'R"\((.*?)\)"', match.group(1), re.S)


def from_js(source, name):
    match = re.search(r'^(?:export )?const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    interp = body.find("${")
    if interp >= 0:
        return None, f"template interpolation `${{` at line {body[:interp].count(chr(10)) + 1}"

    return body.replace("\\`", "`"), None


def first_difference(cpp_text, js_text):
    cpp_lines = cpp_text.splitlines()
    js_lines = js_text.splitlines()
    for i in range(max(len(cpp_lines), len(js_lines))):
        a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
        b = js_lines[i] if i < len(js_lines) else "<missing>"
        if a != b:
            print(f"        first difference at line {i + 1}")
            print(f"          C++: {a}")
            print(f"          js : {b}")
            return
    print("        (the difference is in line endings or a trailing newline)")


def read(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as handle:
        return handle.read()


#---------------------------------------------------------------------------
# The VFD's shader.
#---------------------------------------------------------------------------

def check_vfd_shader():
    cpp = read(VFD_CPP)
    strings = cpp_strings(cpp, "kVfdFragmentShader")
    if strings is None or len(strings) != 1:
        print(f"FAIL  kVfdFragmentShader in {VFD_CPP} is not one raw string (found {None if strings is None else len(strings)})"
              " -- the page carries exactly one; add a constant and update this check")
        return 1
    js_text, complaint = from_js(read(VFD_JS), "VFD_FRAGMENT_SHADER")
    if complaint is not None:
        print(f"FAIL  VFD_FRAGMENT_SHADER in {VFD_JS} has a {complaint}")
        return 1
    if js_text is None:
        print(f"FAIL  VFD_FRAGMENT_SHADER not found in {VFD_JS}")
        return 1
    if js_text == strings[0]:
        print(f"ok    {'VFD_FRAGMENT_SHADER':<18} matches kVfdFragmentShader ({len(js_text)} chars)")
        return 0
    print(f"FAIL  VFD_FRAGMENT_SHADER in {VFD_JS} has drifted from kVfdFragmentShader in {VFD_CPP}")
    first_difference(strings[0], js_text)
    return 1


#---------------------------------------------------------------------------
# ndtest's harness, in the glue.
#---------------------------------------------------------------------------

def item_text(source, first_line):
    """A top-level C++ item: the /// lines directly above the line matching
    `first_line` (anchored at the start of a line), that line, and -- when it
    does not end in `;` -- everything down to the `}` at the start of a line
    that closes it."""
    lines = source.split("\n")
    pattern = re.compile(first_line + r"$")
    starts = [i for i, line in enumerate(lines) if pattern.match(line)]
    if len(starts) != 1:
        return None, f"{len(starts)} declarations match"
    i = starts[0]
    top = i
    while top > 0 and lines[top - 1].startswith("///"):
        top -= 1
    if lines[i].endswith(";"):
        return "\n".join(lines[top:i + 1]), None
    end = i + 1
    while end < len(lines) and lines[end] != "}":
        end += 1
    if end == len(lines):
        return None, "no closing brace at the start of a line"
    return "\n".join(lines[top:end + 1]), None


def check_harness():
    glue = read(GLUE)
    problems = 0
    for rel, name, first_line in HARNESS:
        theirs, why = item_text(read(rel), first_line)
        if theirs is None:
            print(f"FAIL  {name} in {rel}: {why}")
            problems += 1
            continue
        ours, why = item_text(glue, first_line)
        if ours is None:
            print(f"FAIL  {name} in {GLUE}: {why}")
            problems += 1
            continue
        if ours == theirs:
            print(f"ok    {name:<18} matches {rel}'s ({len(ours)} chars)")
            continue
        print(f"FAIL  {name} in {GLUE} has drifted from {rel}'s -- copy ndtest's across")
        first_difference(theirs, ours)
        problems += 1
    return problems


def check_no_devices():
    problems = 0
    demo = os.path.join(REPO, "demo")
    served = []
    for root, dirs, files in os.walk(demo):
        rel = os.path.relpath(root, demo)
        if rel.split(os.sep)[0] in ("tools", "wasm"):
            continue  # not served (.assetsignore), and this file names them
        for name in files:
            if name.endswith((".js", ".html", ".mjs")):
                served.append(os.path.join(root, name))
    for path in sorted(served):
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for api in DEVICE_APIS:
            if api in text:
                print(f"FAIL  {os.path.relpath(path, REPO)} names {api}: the page must never ask for or list an audio device")
                problems += 1
    if problems == 0:
        print(f"ok    {'NO DEVICES':<18} none of the {len(served)} served scripts and pages names {', '.join(DEVICE_APIS)}")
    return problems


#---------------------------------------------------------------------------
# The WebAssembly.
#---------------------------------------------------------------------------

def sha256(rel):
    digest = hashlib.sha256()
    with open(os.path.join(REPO, rel), "rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def cmake_version():
    match = re.search(r"^\s*VERSION ([0-9.]+)$", read("CMakeLists.txt"), re.M)
    return None if match is None else match.group(1)


def check_wasm():
    try:
        lines = read(MANIFEST).splitlines()
    except FileNotFoundError:
        print(f"FAIL  {MANIFEST} is missing -- run demo/tools/build-wasm.sh")
        return 1

    problems = 0
    entries = 0
    recorded = set()
    pinned = version = False
    for line in lines:
        pin = re.match(r"^# FFGL SDK ([0-9a-f]{40})$", line)
        if pin:
            pinned = True
            try:
                head = subprocess.run(["git", "-C", os.path.join(REPO, "external", "ffgl"), "rev-parse", "HEAD"],
                                      capture_output=True, text=True, check=True).stdout.strip()
            except (OSError, subprocess.CalledProcessError):
                print("FAIL  cannot read the FFGL SDK's commit -- is the submodule checked out?")
                problems += 1
                continue
            if head != pin.group(1):
                print(f"FAIL  the .wasm was built against FFGL SDK {pin.group(1)[:7]}, the submodule is at {head[:7]}")
                problems += 1
            continue
        built = re.match(r"^# NEEDLE_VERSION (\S+)$", line)
        if built:
            version = True
            declared = cmake_version()
            if declared != built.group(1):
                print(f"FAIL  the .wasm was built as version {built.group(1)}, CMakeLists.txt declares {declared}"
                      " -- rerun demo/tools/build-wasm.sh")
                problems += 1
            continue
        if not line or line.startswith("#"):
            continue
        want, path = line.split(None, 1)
        recorded.add(path)
        entries += 1
        if not os.path.exists(os.path.join(REPO, path)):
            print(f"FAIL  {path} went into the .wasm and no longer exists")
            problems += 1
        elif sha256(path) != want:
            print(f"FAIL  {path} has changed since demo/needle-core.wasm was built -- rerun demo/tools/build-wasm.sh")
            problems += 1
    if not pinned:
        print(f"FAIL  {MANIFEST} does not record the FFGL SDK commit")
        problems += 1
    if not version:
        print(f"FAIL  {MANIFEST} does not record NEEDLE_VERSION")
        problems += 1

    # A source file added since the build is an input the build never saw.
    trees = [("source", REGISTRATION), ("source/meter", ()), ("external/earshot/src", NOT_COMPILED),
             ("external/earshot/include/earshot", ())]
    for tree, skipped in trees:
        for name in sorted(os.listdir(os.path.join(REPO, tree))):
            if name in skipped:
                continue
            rel = f"{tree}/{name}"
            if (name.endswith(".h") or name.endswith(".cpp")) and rel not in recorded:
                print(f"FAIL  {rel} is newer than the .wasm's manifest -- rerun demo/tools/build-wasm.sh")
                problems += 1

    for output in OUTPUTS:
        if output not in recorded:
            print(f"FAIL  {MANIFEST} does not record {output}")
            problems += 1
        elif os.stat(os.path.join(REPO, output)).st_mode & 0o111:
            print(f"FAIL  {output} is executable -- it is served as a static file (chmod 644)")
            problems += 1
    if problems == 0:
        print(f"ok    {'WASM':<18} built from these sources ({entries} files hashed, SDK pin and version agree)")
    return problems


def main():
    with open(os.path.join(REPO, CPP)) as handle:
        cpp = handle.read()
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    checked = 0

    vertex = cpp_strings(cpp, "kVertexShader")
    fragment = cpp_strings(cpp, "kFragmentShader")

    if vertex is None or len(vertex) != 1:
        print(f"FAIL  kVertexShader in {CPP} is not one raw string (found {None if vertex is None else len(vertex)})")
        return 1
    if fragment is None or len(fragment) != 2:
        print(f"FAIL  kFragmentShader in {CPP} is not two raw strings (found {None if fragment is None else len(fragment)})"
              " -- the page carries exactly two; add a constant and update this check")
        return 1

    pairs = [
        ("VERTEX_SHADER", "kVertexShader", vertex[0]),
        ("FRAGMENT_SHADER_A", "kFragmentShader (raw string 1 of 2)", fragment[0]),
        ("FRAGMENT_SHADER_B", "kFragmentShader (raw string 2 of 2)", fragment[1]),
    ]

    texts = {}
    for name, symbol, cpp_text in pairs:
        js_text, complaint = from_js(js, name)
        checked += 1
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue
        texts[name] = js_text
        if cpp_text == js_text:
            print(f"ok    {name:<18} matches {symbol} ({len(cpp_text)} chars)")
            continue
        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {CPP}")
        first_difference(cpp_text, js_text)

    # The join. Both halves can match and the page still be wrong if it joins
    # them with something in between, or not at all.
    checked += 1
    if re.search(r"^const FRAGMENT_SHADER = FRAGMENT_SHADER_A \+ FRAGMENT_SHADER_B;$", js, re.M) is None:
        print("FAIL  plugin.js does not join the fragment shader as FRAGMENT_SHADER_A + FRAGMENT_SHADER_B")
        problems += 1
    elif "FRAGMENT_SHADER_A" in texts and "FRAGMENT_SHADER_B" in texts:
        joined_js = texts["FRAGMENT_SHADER_A"] + texts["FRAGMENT_SHADER_B"]
        joined_cpp = "".join(fragment)
        if joined_js == joined_cpp:
            print(f"ok    {'FRAGMENT_SHADER':<18} joined text matches kFragmentShader ({len(joined_cpp)} chars)")
        else:
            print("FAIL  the joined fragment shader differs from kFragmentShader")
            first_difference(joined_cpp, joined_js)
            problems += 1

    checked += 1
    problems += check_vfd_shader()

    print()
    if problems:
        print(f"{problems} of {checked} shader check(s) failed -- copy the C++ across, do not edit plugin.js or vfd.js by hand")
        return 1
    print(f"all {checked} shader checks pass: the demo's GLSL is identical to source/Shaders.cpp and source/VfdShaders.cpp")

    print()
    other = check_harness() + check_no_devices() + check_wasm()
    if other:
        print()
        print(f"{other} harness, device or WebAssembly check(s) failed")
        return 1
    print()
    print("the glue's signals are ndtest's, no served file names a device API, and the .wasm's inputs are current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
