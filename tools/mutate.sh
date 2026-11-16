#!/usr/bin/env bash
#
# Mutation-test the VFD harness: change ONE character in the VFD's GLSL,
# rebuild, and require the check that should notice to fail. A harness that
# passes a mutated shader is measuring something other than the shader.
#
# Not part of verify.sh: it builds ndtest once (arm64 only) in a copy of the
# tree, then rebuilds one object per mutant, a few minutes in all. Run it when
# a VFD check or the VFD shader changes. The working tree is never touched.
#
#     tools/mutate.sh
#
# Each run is under a time limit (perl's alarm: macOS has no `timeout`). A
# mutant that hangs the check has been noticed, so a timeout counts as caught.
#
# The table is split on `|`, so no mutant may contain one -- a `||` in GLSL
# silently becomes an empty field. The lit-segment test in the shader is
# such a line, and is left alone here for that reason. Leading whitespace is
# ignored when the text is matched: tabs are too easy to miscount.
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$( mktemp -d )"
trap 'rm -rf "$WORK"' EXIT
LIMIT=300

# the exact text | its one-character mutant | the check that must fail | what it means
FILE="source/VfdShaders.cpp"
MUTANTS=(
	"				float g = 1.0;|				float g = 2.0;|phosphor|the eye's segments twice as bright"
	"	return floor( p ) * w + clamp( p - floor( p ) - a, 0.0, w );|	return floor( p ) * w + clamp( p - floor( p ) - a, 0.0, a );|multiplex|the grid's pulse as long as its start time"
	"				float p0 = uScan.y + floor( f.y ) * uScan.z;|				float p0 = uScan.y + floor( f.x ) * uScan.z;|multiplex|the rolling shutter rolls across the columns"
	"				g = ( onTime( p1, a, w ) - onTime( p0, a, w ) ) / ( uScan.w * uSlot.z );|				g = ( onTime( p1, a, w ) - onTime( p0, a, w ) ) / ( uScan.w * uSlot.x );|multiplex|the camera normalised by the slot, not the pulse"
	"			if( uScan.x > 0.5 )|			if( uScan.x < 0.5 )|multiplex|the camera model applied to the eye"
	"	float lo = max( y - 0.5, yc - 0.5 * w );|	float lo = max( y - 0.6, yc - 0.5 * w );|filament|the pixel row's box 0.1 px too tall"
	"					shade *= 1.0 - wireCover( p.y, uWireY[ i ], uWire.x );|					shade *= 1.0 - wireCover( p.x, uWireY[ i ], uWire.x );|filament|the wires covered by x, not y"
	"	return l <= 0.0031308 ? l * 12.92 : 1.055 * pow( l, 1.0 / 2.4 ) - 0.055;|	return l <= 0.0031308 ? l * 12.92 : 1.055 * pow( l, 1.0 / 2.2 ) - 0.055;|phosphor|a 2.2 power instead of the sRGB curve"
	"			float fromBottom = float( uGrid.y - 1 - row );|			float fromBottom = float( uGrid.y - 2 - row );|skirt|every bar one segment high"
	"	int  col = int( floor( q.x / uField.z ) );|	int  col = int( floor( q.y / uField.z ) );|skirt|columns found from y"
	"			vec3 lin = min( uPhosphor * g, vec3( 1.0 ) );|			vec3 lin = min( uPhosphor * g, vec3( 0.0 ) );|skirt|every lit segment black"
	"	c = paint( c, vec3( 0.0 ), glass );|	c = paint( c, vec3( 1.0 ), glass );|skirt|the glass white"
	"		c = paint( c, uLampCol, cov( length( p - uLampA.xy ) - uLampA.z ) );|		c = paint( c, uLampCol, cov( length( p - uLampA.xy ) + uLampA.z ) );|devices|the status lamp drawn inside out (invisible)"
	"	vec2  p  = vec2( d.x * cs + d.y * sn, -d.x * sn + d.y * cs );|	vec2  p  = vec2( d.x * cs + d.y * sn, +d.x * sn + d.y * cs );|geometry|the rotation's sign: a turned display mirrored"
	"		float coverage   = cov( sdBox( p - centre, 0.5 * uSeg ) );|		float coverage   = cov( sdBox( p - centre, 0.6 * uSeg ) );|geometry|every segment 20% larger"
	"		vec2  centre     = uField.xy + ( vec2( float( col ), float( row ) ) + 0.5 ) * uField.zw;|		vec2  centre     = uField.xy + ( vec2( float( col ), float( row ) ) + 0.6 ) * uField.zw;|geometry|every segment a tenth of a cell off centre"
	"	float cs = cos( uXform.z );|	float cs = cos( uXform.z ) ;|skirt|a no-op edit, which must NOT be caught -- the control for the controls"
)

tree="$WORK/tree"
mkdir -p "$tree"
( cd "$REPO" && tar cf - --exclude='./build*' --exclude='./external' --exclude='./.git' --exclude='./.claude' . ) | ( cd "$tree" && tar xf - )
mkdir -p "$tree/external"
ln -s "$REPO/external/ffgl" "$tree/external/ffgl"
ln -s "$REPO/external/earshot" "$tree/external/earshot"
cp "$tree/$FILE" "$WORK/pristine"

echo "building ndtest (arm64) in a copy of the tree"
if ! cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >"$WORK/configure.log" 2>&1 \
   || ! cmake --build "$tree/build" --target ndtest -j4 >"$WORK/build.log" 2>&1; then
	echo "the unmutated tree does not build"
	tail -20 "$WORK/build.log"
	exit 1
fi

caught=0
missed=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r original mutant check meaning <<<"$entry"

	python3 - "$tree/$FILE" "$WORK/pristine" "$original" "$mutant" <<'PY' || { echo "  MUTANT NOT APPLIED: $meaning"; missed=$(( missed + 1 )); continue; }
import sys
path, pristine, a, b = sys.argv[1:5]
a, b = a.lstrip(), b.lstrip()
text = open(pristine).read()
if text.count(a) != 1:
    print(f"the original text occurs {text.count(a)} times, not once", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(text.replace(a, b))
PY
	# make compares mtimes to the second. A mutant written within a second of
	# the last build is not recompiled, and an object recompiled within a
	# second of the last link is not relinked: the old binary runs and the
	# mutant is "missed". Both happened here, to the first mutant after the
	# unmutated build. So the object AND the binary go first.
	find "$tree/build" -name 'VfdShaders.cpp.o' -delete
	rm -f "$tree/build/ndtest"
	if ! cmake --build "$tree/build" --target ndtest -j4 >"$WORK/build.log" 2>&1 || [ ! -x "$tree/build/ndtest" ]; then
		echo "  (did not build: $meaning)"
		missed=$(( missed + 1 ))
		continue
	fi

	expectFail=1
	case "$meaning" in *"must NOT be caught"*) expectFail=0 ;; esac

	NEEDLE_NO_DEVICES=1 perl -e 'alarm shift; exec @ARGV' "$LIMIT" "$tree/build/ndtest" "--$check" >"$WORK/out.log" 2>&1
	rc=$?
	if [ "$rc" -eq 142 ]; then
		echo "  caught  --$check timed out after ${LIMIT} s (a hang is noticed): $meaning"
		caught=$(( caught + 1 ))
	elif [ "$rc" -eq 0 ]; then
		if [ "$expectFail" = 1 ]; then
			echo "  MISSED  --$check passed the mutant: $meaning"
			grep -E "^  ok" "$WORK/out.log" | sed 's/^/          /'
			missed=$(( missed + 1 ))
		else
			echo "  ok      --$check passed, as it must: $meaning"
			caught=$(( caught + 1 ))
		fi
	else
		if [ "$expectFail" = 1 ]; then
			echo "  caught  --$check failed the mutant ($(grep -c FAIL "$WORK/out.log") assertion(s)): $meaning"
			caught=$(( caught + 1 ))
		else
			echo "  WRONG   --$check failed: $meaning"
			missed=$(( missed + 1 ))
		fi
	fi
done

echo
echo "$caught of $(( caught + missed )) mutants behaved as expected"
exit $(( missed > 0 ? 1 : 0 ))
