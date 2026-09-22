#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
#     tools/verify.sh
#
# ------------------------------------------------------------------ the point
#
# Each check answers a question none of the others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out
#   physics       the claims the plugin exists to make -- the VU's 300 ms and
#                 its overshoot, the PPM's fall-back, the 3 dB ladder, the
#                 eye's shadow -- measured against the CPU engine at its
#                 oversampled rate, with NO GL context anywhere near them
#   pixels        and the one check that does read pixels: that the shader
#                 draws where the CPU said, at two rasters
#   sweep         does every control change the picture
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain
#   lipo          is the macOS build really universal, or did CMake latch the
#                 architecture list before -DCMAKE_OSX_ARCHITECTURES arrived
#                 and report success anyway
#   plist         does CFBundleExecutable name the binary that is actually on
#                 disk -- if it does not, codesign reports "code object is not
#                 signed at all" about a *nested* object and mentions neither
#                 the plist nor the cause
#   codesign      the exact command the release job runs, against a copy
#   oxbow         instantiation and 120 frames in a host, which nothing else
#                 here reaches
#
# The last four are release-job work done locally on purpose. A check that only
# runs in CI, after a tag, is a check that will catch you after the tag -- and
# the fix for a bad tag is to re-point it, which strands the release unsigned
# for ever unless the autosign state file is edited by hand.
#
# ------------------------------------------------- why the build is deleted
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is where
# the architecture list lives. A developer who configured once with
# `-DCMAKE_OSX_ARCHITECTURES=arm64` for a fast iteration loop -- which is the
# documented way to work in CLAUDE.md -- leaves a tree where this script happily
# rebuilds, finds a single-architecture binary and reports it as a defect in the
# source. So: fresh configure, every time.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# A shader IS several adjacent raw strings here -- MSVC caps one literal at
# about 16 KB (C2026) and the fragment shader is over it -- so everything up to
# the terminating semicolon is joined. Lose this join and the check reports a
# syntax error in the middle of a function.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -lt 2 ]; then
		# Fewer than two shaders is a FAILURE, not a pass: there is a vertex
		# shader and a fragment shader, and the fragment one is the join above.
		# A check that silently looks at nothing is worse than no check.
		printf '   only %d shader(s) extracted -- the extraction has gone stale\n' "$n"
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

step "build (fresh, universal)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/tmp/needle-configure.log 2>&1 \
   && cmake --build "$BUILD" --parallel >/tmp/needle-build.log 2>&1; then
	pass "configured and built"
else
	fail "build failed -- see /tmp/needle-build.log"
	tail -25 /tmp/needle-build.log
	exit 1
fi

#---------------------------------------------------------------------------
# The physics. None of these opens a GL context.
#
# That is the point of them and it is this repo's main lesson. Every physical
# claim here is a claim about a differential equation, so it is measured
# against the CPU engine at its own oversampled rate -- not by reading rendered
# pixels, where the answer would depend on a rasteriser and a raster that have
# no opinion about ANSI C16.5.
#---------------------------------------------------------------------------
step "physics (no GL)"
for t in ballistics ppm steps eye prime rate defaults names font; do
	log="/tmp/needle-$t.log"
	if "$BUILD/ndtest" "--$t" >"$log" 2>&1; then
		pass "ndtest --$t"
	else
		fail "ndtest --$t -- see $log"
		grep -E "FAIL" "$log" | head -6
	fi
done

step "pixels (the one check that reads a rasteriser)"
if "$BUILD/ndtest" --pixels >/tmp/needle-pixels.log 2>&1; then
	pass "the shader draws where the CPU said, at 640x360 and 1920x1080"
else
	fail "ndtest --pixels -- see /tmp/needle-pixels.log"
	grep -E "FAIL" /tmp/needle-pixels.log | head -8
fi

step "sweep"
if python3 tools/sweep.py >/tmp/needle-sweep.log 2>/dev/null; then
	pass "no dead controls"
else
	fail "tools/sweep.py reports a dead control"
	tail -6 /tmp/needle-sweep.log
fi

BUNDLE="$BUILD/Needle.bundle"
BIN="$BUNDLE/Contents/MacOS/Needle"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Needle.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		# A source that lights pixels with no input and no file, so oxbow's
		# verdict is meaningful here.
		out=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$out" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*PASS*) pass "registers, instantiates and lights pixels" ;;
			*"id:"*) fail "registers but oxbow reports FAIL -- see: $OXBOW selftest $BUNDLE" ;;
			*) fail "oxbow did not recognise the bundle" ;;
		esac
		# And the identity a host reads, which nothing else here checks.
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		case "$probe" in *"id:          ND01"*) pass "id is ND01" ;; *) fail "wrong FFGL id" ;; esac
		case "$probe" in *"name:        Needle"*) pass "name is Needle" ;; *) fail "wrong plugin name" ;; esac
		case "$probe" in *"type:        source"*) pass "type is source" ;; *) fail "wrong plugin type" ;; esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

step "cost"
"$BUILD/ndtest" --bench 2>/dev/null | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
