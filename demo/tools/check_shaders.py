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

The one transformation is a decode, not a normalisation. A comment in the shader
quotes `origin` in backticks, and a backtick cannot appear raw inside a
JavaScript template literal, so `plugin.js` escapes it as \\`. This unescapes
that and *rejects any other backslash on the JS side* -- there are none anywhere
in the C++, so a second escape could only be somebody hiding a difference. A
`${` would be interpolated by JavaScript, so it is rejected too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CPP = "source/Shaders.cpp"


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
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
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

    print()
    if problems:
        print(f"{problems} of {checked} shader check(s) failed -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {checked} shader checks pass: the demo's GLSL is identical to source/Shaders.cpp")
    return 0


if __name__ == "__main__":
    sys.exit(main())
