#!/usr/bin/env bash
#
# Build demo/needle-core.{js,wasm}: the plugin's own C++, compiled to
# WebAssembly for the browser demo's VFD (Type = VFD). (woofer's script, which
# is mirrorball's, pinhole's and fountain's, with needle's files.)
#
#     demo/tools/build-wasm.sh          (needs emscripten: em++ on PATH, or EMXX=...)
#
# The output is COMMITTED, because the Cloudflare deploy has no build step:
# what is in demo/ is what is served. So is demo/wasm/inputs.sha256, the hash
# of every input below and of both outputs, the SDK commit and the version
# CMakeLists.txt declares; demo/tools/check_shaders.py (run by tools/verify.sh)
# fails when a source file has changed since this last ran, which is the only
# way a stale .wasm can be noticed without emscripten. Rerun it after anything
# under source/ or external/earshot/ changes -- a regenerated StoatworksAbout.h
# and a version bump included.
#
# What goes in, and none of it is edited for the browser:
#
#   source/   every .cpp but SourcePlugin.cpp (the FF_SOURCE registration,
#             whose job the page does): Needle.cpp -- the plugin class itself,
#             its constructor, its parameter cache, ApplyVisibility, its frame
#             path and the VFD branch -- and everything it calls: Vfd.cpp
#             (vfd::Input, the only code that calls earshot; vfd::Display, the
#             columns, segments and hold), VfdRender.cpp and VfdShaders.cpp (its
#             program and GLSL), Shaders.cpp and Render.cpp (the meters' program,
#             which InitGL compiles), meter/ (the engine every frame advances),
#             Audio, Clock, Font and Diag.
#   external/earshot   the snapshot at 8124191, unpatched, built as its CMake
#             builds it with EARSHOT_CAPTURE=OFF: src/NoCapture.cpp in place of
#             miniaudio (Miniaudio.cpp and MiniaudioImpl.cpp are NOT compiled,
#             and miniaudio.h is never included), so this module has no device
#             backend at all. listDevices() is empty and selectDevice() of a
#             name reports Failed. Audio reaches the plugin only through
#             earshot's feed(), as ndtest's does (tools/ndtest/Vfd.inc).
#   external/ffgl   the FFGL SDK's CFFGLPluginManager / CFFGLPlugin (parameter
#             bookkeeping, read back by the page as a host reads it), FFGLLog,
#             and ffglex's FFGLShader and FFGLUtilities (its Log).
#   demo/wasm glue.cpp (the host's calls, as C functions, the harness's
#             programme and the page's test signals) and gl_shim.cpp (the four
#             GL entry points WebGL2 cannot take as they are).
#
# GL itself is emscripten's WebGL2 library, bound by the page to its own
# context. `-D__linux__` is for the SDK's FFGLPlatform.h, which knows Windows,
# Linux and macOS and stops the build on anything else: told it is Linux, the
# SDK includes <GL/glew.h>, which emscripten provides. Nearly every needle file
# includes the SDK (Controls.h does), so needle's files, the SDK's and the glue
# get it; earshot's do not, and include no SDK header.
#
# Flags follow CMakeLists.txt's Release build: -O3 -DNDEBUG, C++17. No
# pthreads: earshot's device worker would be a thread, and it is only ever
# started by selectDevice() with a name, which nothing here calls (the page
# never sets Audio Input). WebAssembly has no fused multiply-add, so where
# clang fuses one on arm64 the browser rounds twice, and its sin, cos, exp,
# log10 and pow are musl's rather than Apple's libm: earshot's filters and the
# test signals need not agree with the Mac's bit for bit. What reaches the GPU
# is the plugin's vfd::Picture, as float. Pictures against ndtest:
# demo/tools/crosscheck.py, and AGENTS.md, "The browser demo".
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

EMXX="${EMXX:-em++}"
command -v "$EMXX" >/dev/null 2>&1 || { echo "em++ not found (install emscripten, or set EMXX=)" >&2; exit 1; }
[[ -f external/ffgl/CMakeLists.txt ]] || { echo "FFGL SDK missing -- git submodule update --init --recursive" >&2; exit 1; }
[[ -f external/earshot/CMakeLists.txt ]] || { echo "external/earshot missing" >&2; exit 1; }

version="$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | sed -n 1p )"
[[ -n "$version" ]] || { echo "no VERSION in CMakeLists.txt" >&2; exit 1; }

SDK=external/ffgl/source/lib
ES=external/earshot
PLUGIN_SOURCES="Audio Clock Diag Font Needle Render Shaders Vfd VfdRender VfdShaders meter/Engine meter/Movement meter/Standards"
# earshot's CMakeLists.txt's own list, with EARSHOT_CAPTURE=OFF's NoCapture.cpp.
EARSHOT_SOURCES="Capture Engine Fft HostFft Onsets Partials Pitch Stereo Tap Tempo ThirdOctave NoCapture"
SDK_SOURCES="ffgl/FFGLPluginManager ffgl/FFGLPluginSDK ffgl/FFGLLog ffglex/FFGLShader ffglex/FFGLUtilities"

# Every source .cpp is either compiled here or the registration. A new file
# under source/ must be added to PLUGIN_SOURCES (or this line), not left out.
for file in source/*.cpp source/meter/*.cpp; do
	name="${file#source/}"
	name="${name%.cpp}"
	[[ "$name" == SourcePlugin || " $PLUGIN_SOURCES " == *" $name "* ]] \
		|| { echo "source/$name.cpp is not in PLUGIN_SOURCES -- add it, or say why it is left out" >&2; exit 1; }
done
# And every earshot source but the two miniaudio files, so a new one in a new
# snapshot cannot be left out quietly.
for file in "$ES"/src/*.cpp; do
	name="$( basename "$file" .cpp )"
	[[ "$name" == Miniaudio || "$name" == MiniaudioImpl || " $EARSHOT_SOURCES " == *" $name "* ]] \
		|| { echo "$ES/src/$name.cpp is not in EARSHOT_SOURCES -- add it, or say why it is left out" >&2; exit 1; }
done

OBJ="$( mktemp -d )"
trap 'rm -rf "$OBJ"' EXIT

INCLUDES=( -Isource -I"$SDK" -I"$ES/include" )
BASE=( -std=c++17 -DNDEBUG -DNEEDLE_VERSION="\"$version\"" "${INCLUDES[@]}" )
SDK_PLATFORM=( -D__linux__=1 )

objects=()
compile() {
	local src="$1" opt="$2"; shift 2
	local obj="$OBJ/$( echo "$src" | tr '/' '_' ).o"
	"$EMXX" "${BASE[@]}" "$opt" "$@" -c "$src" -o "$obj"
	objects+=( "$obj" )
}

for name in $PLUGIN_SOURCES; do
	compile "source/$name.cpp" -O3 "${SDK_PLATFORM[@]}"
done
for name in $EARSHOT_SOURCES; do
	compile "$ES/src/$name.cpp" -O3 -I"$ES/src"
done
for name in $SDK_SOURCES; do
	compile "$SDK/$name.cpp" -O3 "${SDK_PLATFORM[@]}"
done
compile demo/wasm/glue.cpp -O3 "${SDK_PLATFORM[@]}"
compile demo/wasm/gl_shim.cpp -O3

LINK=(
	-O3
	-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createNeedle
	-sENVIRONMENT=web
	-sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
	-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
	-sDYNAMIC_EXECUTION=0
	-sFILESYSTEM=1
	-sEXPORTED_RUNTIME_METHODS=GL,FS,UTF8ToString
	-sEXPORTED_FUNCTIONS=_malloc,_free
)
"$EMXX" "${objects[@]}" "${LINK[@]}" -o demo/needle-core.js

#---------------------------------------------------------------------------
# The manifest check_shaders.py reads. Inputs first, then outputs.
#---------------------------------------------------------------------------
{
	echo "# demo/tools/build-wasm.sh, $( "$EMXX" --version | sed -n 1p )"
	echo "# FFGL SDK $( git -C external/ffgl rev-parse HEAD )"
	echo "# NEEDLE_VERSION $version"
	for name in $PLUGIN_SOURCES; do shasum -a 256 "source/$name.cpp"; done
	for header in source/*.h source/meter/*.h; do shasum -a 256 "$header"; done
	for name in $EARSHOT_SOURCES; do shasum -a 256 "$ES/src/$name.cpp"; done
	for header in "$ES"/src/*.h "$ES"/include/earshot/*.h; do shasum -a 256 "$header"; done
	for name in $SDK_SOURCES; do shasum -a 256 "$SDK/$name.cpp"; done
	shasum -a 256 demo/wasm/glue.cpp demo/wasm/gl_shim.cpp demo/tools/build-wasm.sh
	shasum -a 256 demo/needle-core.js demo/needle-core.wasm
} > demo/wasm/inputs.sha256

# Served as static files and committed: no executable bit (pinhole's trap,
# and a sibling's: emscripten writes the .wasm 755).
chmod 644 demo/needle-core.js demo/needle-core.wasm

ls -l demo/needle-core.js demo/needle-core.wasm | awk '{ print "wrote " $NF ", " $5 " bytes" }'
