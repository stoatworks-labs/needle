/**
 * Needle — the browser demo's VFD (Type = VFD).
 *
 * The four meters on this page are a hand port to JavaScript (plugin.js). The
 * VFD is not: it is the PLUGIN, compiled. `needle-core.wasm` is
 * `source/Needle.cpp` — the plugin class, its constructor, its parameter
 * cache, its frame path and its VFD branch — and everything that branch
 * calls: `vfd::Input` (the only code in needle that calls earshot),
 * `vfd::Display` (earshot's thirds into columns, segments at 3 dB, the peak
 * hold and its fall in audio time), `BuildVfdPicture` (every pixel the shader
 * is handed), `vfd::Renderer` and the VFD's GLSL; with earshot's snapshot
 * (8124191, unpatched, its capture backend left out) and the FFGL SDK, all
 * compiled unmodified by emscripten (demo/tools/build-wasm.sh). This file
 * plays the host: it constructs the plugin, hands it the page's parameters,
 * the clock and the audio, and asks it for frames.
 *
 * **The audio is never a microphone.** The plugin's VFD reads a capture
 * device through earshot; this module has no device backend at all
 * (EARSHOT_CAPTURE=OFF), and the plugin is told not to list devices
 * (NEEDLE_NO_DEVICES=1, its verification switch). Its audio arrives the way
 * its harness's does: generated test signals handed to earshot's feed(), the
 * path a device's samples take inside earshot (tools/ndtest/Vfd.inc). The
 * signals are generated inside the WebAssembly by demo/wasm/glue.cpp: one is
 * ndtest's own programme, copied character for character; the rest are the
 * page's. Nothing is played aloud, and nothing here names any browser audio
 * or device API (check_shaders.py enforces that for every served file).
 *
 * Why the meters are not moved onto the WebAssembly too: their port has been
 * the live page since 2026-09-24, its shaders are checked character for
 * character, and moving four instruments that already work onto a new path
 * in the same change as a fifth would put all five at risk for no gain a
 * visitor could see. So the page runs two engines and says which is which.
 */

import { port } from './vendor/gl.js';
import createNeedle from './needle-core.js';

//---------------------------------------------------------------------------
// source/VfdShaders.cpp's kVfdFragmentShader, verbatim. Do not edit here:
// demo/tools/check_shaders.py compares it character for character, and the
// shim refuses to compile anything the plugin hands GL that is not one of
// the page's checked copies.
//---------------------------------------------------------------------------

export const VFD_FRAGMENT_SHADER = `#version 410 core
out vec4 fragColour;

uniform ivec2 uSize;
uniform vec4  uXform;  // centre.x, centre.y, rotation, corner
uniform vec2  uHalf;   // glass half width, half height
uniform vec4  uField;  // x0, y0 of the column-0 row-0 cell; pitch x, pitch y
uniform vec2  uSeg;    // segment w, h
uniform ivec2 uGrid;   // columns, rows
uniform float uLit[ 31 ];
uniform float uDot[ 31 ];
uniform vec3  uPhosphor;// linear
uniform vec4  uWireY;   // four wire centres, local y
uniform vec3  uWire;    // width, x0, x1
uniform vec4  uScan;    // camera, phase0, scans per row, scans per exposure
uniform vec3  uSlot;    // slot, blanking, pulse (scans)
uniform vec4  uLampA;   // x, y, radius, on
uniform vec3  uLampCol;
uniform vec3  uBackCol;
uniform float uBackA;
uniform float uMix;

float cov( float d ) { return clamp( 0.5 - d, 0.0, 1.0 ); }

vec4 over( vec4 src, vec4 dst )
{
	float a = src.a + dst.a * ( 1.0 - src.a );
	if( a <= 0.0 )
		return vec4( 0.0 );
	return vec4( ( src.rgb * src.a + dst.rgb * dst.a * ( 1.0 - src.a ) ) / a, a );
}

vec4 paint( vec4 dst, vec3 colour, float coverage )
{
	return over( vec4( colour, clamp( coverage, 0.0, 1.0 ) ), dst );
}

float sdBox( vec2 p, vec2 b )
{
	vec2 d = abs( p ) - b;
	return length( max( d, 0.0 ) ) + min( max( d.x, d.y ), 0.0 );
}

float sdRoundBox( vec2 p, vec2 b, float r )
{
	vec2 q = abs( p ) - b + vec2( r );
	return length( max( q, 0.0 ) ) + min( max( q.x, q.y ), 0.0 ) - r;
}

// The sRGB transfer function, CSS Color 4 section 10.2.
float encode( float l )
{
	return l <= 0.0031308 ? l * 12.92 : 1.055 * pow( l, 1.0 / 2.4 ) - 0.055;
}

// A grid's on-time from scan 0 to scan position p, in scans: whole scans
// times the pulse width, plus the part of this scan's pulse already past.
float onTime( float p, float a, float w )
{
	return floor( p ) * w + clamp( p - floor( p ) - a, 0.0, w );
}

// The overlap of a wire (centre yc, width w) with the pixel row centred on
// y: exact box coverage, so the rows a wire crosses darken by its width in
// total however thin it is. cov() is a one-pixel ramp and is wrong for this.
float wireCover( float y, float yc, float w )
{
	float lo = max( y - 0.5, yc - 0.5 * w );
	float hi = min( y + 0.5, yc + 0.5 * w );
	return clamp( hi - lo, 0.0, 1.0 );
}

void main()
{
	vec2 f = vec2( gl_FragCoord.x, float( uSize.y ) - gl_FragCoord.y );
	vec4 c = vec4( uBackCol, uBackA );

	vec2  d  = f - uXform.xy;
	float cs = cos( uXform.z );
	float sn = sin( uXform.z );
	vec2  p  = vec2( d.x * cs + d.y * sn, -d.x * sn + d.y * cs );

	// The glass: black, because nothing behind an unlit phosphor gives light
	// and the model has no room light to reflect.
	float glass = cov( sdRoundBox( p, uHalf, uXform.w ) );
	c = paint( c, vec3( 0.0 ), glass );

	vec2 q   = p - uField.xy;
	int  col = int( floor( q.x / uField.z ) );
	int  row = int( floor( q.y / uField.w ) );
	if( col >= 0 && col < uGrid.x && row >= 0 && row < uGrid.y )
	{
		vec2  centre     = uField.xy + ( vec2( float( col ), float( row ) ) + 0.5 ) * uField.zw;
		float coverage   = cov( sdBox( p - centre, 0.5 * uSeg ) );
		float fromBottom = float( uGrid.y - 1 - row );
		bool  lit        = fromBottom < uLit[ col ] - 0.5 || abs( fromBottom - uDot[ col ] ) < 0.5;
		if( lit && coverage > 0.0 )
		{
			// The eye sees the time average, which the drive normalises to 1.
			// A camera row integrates [t0 + r Tr/H, + E]; g is what it caught
			// of the grid's pulse train over what the average would give.
			float g = 1.0;
			if( uScan.x > 0.5 )
			{
				float a  = float( col ) * uSlot.x + uSlot.y;
				float w  = uSlot.x - uSlot.y;
				float p0 = uScan.y + floor( f.y ) * uScan.z;
				float p1 = p0 + uScan.w;
				g = ( onTime( p1, a, w ) - onTime( p0, a, w ) ) / ( uScan.w * uSlot.z );
			}
			// Linear light, clipped per channel as a sensor saturates.
			vec3 lin = min( uPhosphor * g, vec3( 1.0 ) );

			// The filaments, in front of the phosphor: opaque, unlit.
			float shade = 1.0;
			if( p.x >= uWire.y && p.x <= uWire.z )
			{
				for( int i = 0; i < 4; ++i )
					shade *= 1.0 - wireCover( p.y, uWireY[ i ], uWire.x );
			}
			lin *= shade;

			vec3 enc = vec3( encode( lin.r ), encode( lin.g ), encode( lin.b ) );
			c = paint( c, enc, coverage );
		}
	}

	if( uLampA.w > 0.5 )
		c = paint( c, uLampCol, cov( length( p - uLampA.xy ) - uLampA.z ) );

	c.a   = clamp( c.a * uMix, 0.0, 1.0 );
	c.rgb = clamp( c.rgb, 0.0, 1.0 );
	fragColour = vec4( c.rgb * c.a, c.a );
}
`;

//---------------------------------------------------------------------------
// FFGL.h's parameter types, by number: what GetParamType returns.
//---------------------------------------------------------------------------

const FF_TYPE_BOOLEAN = 0;
const FF_TYPE_EVENT = 1;
const FF_TYPE_RED = 2;
const FF_TYPE_GREEN = 3;
const FF_TYPE_BLUE = 4;
const FF_TYPE_STANDARD = 10;
const FF_TYPE_OPTION = 11;
const FF_TYPE_BUFFER = 12;
const FF_TYPE_TEXT = 100;

const KIT_TYPE = {
  [FF_TYPE_BOOLEAN]: 'boolean',
  [FF_TYPE_RED]: 'colour',
  [FF_TYPE_GREEN]: 'colour',
  [FF_TYPE_BLUE]: 'colour',
  [FF_TYPE_STANDARD]: 'standard',
  [FF_TYPE_OPTION]: 'option',
};

export const FEED_RATE = 48000;

/**
 * The test signals, by the number glue.cpp's SampleAt takes. Levels are dB
 * re a full-scale sine (AES17's dBFS, the unit earshot reports a third in).
 */
export const VFD_SIGNALS = [
  {
    id: 'track', which: 1,
    name: 'A track: kick, snare, hats, bass, chords (120 BPM)',
    hint: 'This page’s own: an 8 s loop of four bars, written for the VFD — a kick on every beat, a snare on two and four, hats on the eighths, a bass line, a held chord and a sixteenth-note arpeggio, so the low, middle and top columns all move and the kicks and hats leave peak dots to fall. Generated in the WebAssembly and fed through earshot’s feed().',
  },
  {
    id: 'programme', which: 0,
    name: 'The harness’s programme: a sine in every third, each drifting ±9 dB',
    hint: 'ndtest’s own signal, copied into the glue character for character: a sine at the exact centre of each of the 31 thirds, each 14.9 dB under −18 dBFS with a 2 dB tilt, each moving ±9 dB on its own slow cycle. It is what `ndtest --out --level -18` feeds a VFD, so demo/tools/crosscheck.py compares the two pictures.',
  },
  {
    id: 'tones', which: 2,
    name: 'ISO third-octave tones in turn, 20 Hz to 20 kHz, at −18 dBFS',
    hint: 'One sine at a time at each of the 31 exact base-ten midbands (IEC 61260; ISO 266’s preferred frequencies are these, rounded), half a second each. At the default Reference Level its own column lights to the top; the neighbours light as far as earshot’s filter skirts let through, which is how a real third-octave display behaves.',
  },
  {
    id: 'sweep', which: 3,
    name: 'A sine sweep, 20 Hz to 20 kHz in 20 s, at −18 dBFS',
    hint: 'Logarithmic, so it spends the same time in every band, then two seconds of silence. The peak dots trail the sweep across the display: each waits Peak Hold after its bar leaves it, then falls at Hold Decay.',
  },
  {
    id: 'pink', which: 4,
    name: 'Pink noise, −18 dBFS overall',
    hint: 'Pink periodic noise: a 5.46 s loop with every line from 17.8 Hz to 22.4 kHz at a power proportional to 1/f and a random phase, made once by an inverse FFT in the glue. Pink noise has the same power in every third, so the display reads flat, about 14.9 dB under −18 dBFS in each column, and flickers in the low bands, where a third holds only a few dozen lines.',
  },
  {
    id: 'silence', which: 5,
    name: 'Silence (zeros fed)',
    hint: 'Exact zeros through feed(). The bars fall away at earshot’s Fast weighting, the dots wait and fall, and after three seconds earshot calls the input Silent: the status lamp goes steady red, as it would for a device handing the plugin silence.',
  },
  {
    id: 'host', which: -1,
    name: 'Nothing fed: the plugin’s default, Audio Input “None (host FFT)”',
    hint: 'What the VFD shows in Resolume until a device is chosen: earshot gets no samples, only the host’s FFT, and its third-octave bank needs samples, so every column is dark and the status lamp is amber. Nothing is fed here, and the Audio buffer is left empty. Choosing this starts a new instance, as Restart does.',
  },
];

const signalById = (id) => VFD_SIGNALS.find((s) => s.id === id) ?? VFD_SIGNALS[0];

/**
 * Load the module. `pageShaders` are plugin.js's checked copies of
 * source/Shaders.cpp (the meters' program, which the plugin's InitGL
 * compiles). Resolves to the VFD's host, or to { error } — the meters do not
 * depend on it, so a browser that will not run the WebAssembly still has
 * them.
 */
export async function loadVfd(pageShaders) {
  const copies = [
    { name: 'VERTEX_SHADER', stage: 'vertex', text: pageShaders.vertex },
    { name: 'FRAGMENT_SHADER', stage: 'fragment', text: pageShaders.fragment },
    { name: 'VFD_FRAGMENT_SHADER', stage: 'fragment', text: VFD_FRAGMENT_SHADER },
  ];
  const shaders = { matched: new Set(), refused: 0 };

  /** gl_shim.cpp's glShaderSource: only the page's own copies, ported. */
  function shaderSource(stage, text) {
    const ours = copies.filter((c) => c.stage === stage && c.text === text);
    if (!ours.length) {
      shaders.refused += 1;
      return '';
    }
    for (const c of ours) shaders.matched.add(c.name);
    return port(text, { stage });
  }

  const printed = [];
  let core;
  try {
    core = await createNeedle({
      needleShaderSource: shaderSource,
      print: (text) => { printed.push(text); },
      printErr: (text) => { printed.push(text); },
    });
  } catch (error) {
    return { error: `this browser would not run the plugin’s WebAssembly (${error.message})` };
  }
  const text = (pointer) => core.UTF8ToString(pointer);

  // Before any instance: the plugin's log in the page's memory, and the
  // switch that keeps vfd::Input from listing or opening any device.
  core._nd_prepare();

  //-------------------------------------------------------------------------
  // The declarations, read from the plugin's constructor through the SDK.
  //-------------------------------------------------------------------------
  const declared = [];
  {
    const h = core._nd_new();
    const n = core._nd_param_count(h);
    for (let i = 0; i < n; i += 1) {
      const type = core._nd_param_type(h, i);
      const d = { index: i, name: text(core._nd_param_name(h, i)), type, group: text(core._nd_param_group(h, i)) };
      d.default = type === FF_TYPE_TEXT ? null : core._nd_param_default(h, i);
      if (type === FF_TYPE_OPTION) {
        d.elements = [];
        d.values = [];
        for (let e = 0; e < core._nd_param_element_count(h, i); e += 1) {
          d.elements.push(text(core._nd_param_element_name(h, i, e)));
          d.values.push(core._nd_param_element_value(h, i, e));
        }
      }
      declared.push(d);
    }
    const inputs = [core._nd_min_inputs(h), core._nd_max_inputs(h)];
    core._nd_delete(h);
    if (inputs.join() !== '0,0') {
      return { error: `the plugin declares ${inputs.join('–')} inputs; this page treats it as a source with none` };
    }
  }
  const byName = new Map(declared.map((d) => [d.name, d]));

  /** What the panel leaves out, by the plugin's own names. */
  const LEFT_OUT = ['Audio', 'Audio Input', 'Rescan Inputs'];

  /**
   * The page's hand-written panel (plugin.js's PARAMS) against the compiled
   * constructor: every control the plugin declares, but the Audio buffer, the
   * About block, Audio Input and Rescan Inputs, in the same order, with the
   * same names, groups, types, elements and defaults. Returns the
   * differences; plugin.js logs each one as an error.
   */
  function checkPanel(params) {
    const problems = [];
    const want = declared.filter((d) => d.group !== 'About' && !LEFT_OUT.includes(d.name));
    for (const name of LEFT_OUT) {
      if (!byName.has(name)) problems.push(`the plugin no longer declares “${name}”, which the page says it leaves out`);
    }
    const n = Math.max(want.length, params.length);
    for (let i = 0; i < n; i += 1) {
      const d = want[i];
      const p = params[i];
      if (!d || !p) {
        problems.push(`the panel has ${params.length} controls, the plugin declares ${want.length} that belong on it`);
        break;
      }
      const where = `control ${i + 1} (${p.name})`;
      if (p.name !== d.name) { problems.push(`${where}: the plugin declares “${d.name}” here`); continue; }
      if (p.group !== d.group) problems.push(`${where}: group “${p.group}”, the plugin’s is “${d.group}”`);
      if (KIT_TYPE[d.type] !== p.type) problems.push(`${where}: type ${p.type}, the plugin’s is FFGL type ${d.type}`);
      let def = d.default;
      if (d.type === FF_TYPE_OPTION) {
        if (JSON.stringify(p.elements) !== JSON.stringify(d.elements)) problems.push(`${where}: elements ${JSON.stringify(p.elements)}, the plugin’s are ${JSON.stringify(d.elements)}`);
        def = d.values.findIndex((v) => v === d.default);
      } else if (d.type === FF_TYPE_BOOLEAN) {
        def = d.default > 0.5 ? 1 : 0;
      }
      if (!(Math.abs(Math.fround(p.default) - def) <= 1e-6)) problems.push(`${where}: default ${p.default}, the plugin’s is ${def}`);
    }
    return problems;
  }

  /** The host's value for a page control: what Resolume would send. */
  function hostValue(p, v) {
    const d = byName.get(p.name);
    if (!d) return null;
    switch (d.type) {
      case FF_TYPE_OPTION: return d.values[Math.max(0, Math.min(d.values.length - 1, Math.round(v)))];
      case FF_TYPE_BOOLEAN: return v > 0.5 ? 1 : 0;
      case FF_TYPE_EVENT:
      case FF_TYPE_BUFFER:
      case FF_TYPE_TEXT: return null;
      default: return Math.fround(v);
    }
  }

  /** Controls.h's conversion of a host value, compiled; NaN for none. */
  const convert = (name, v) => {
    const d = byName.get(name);
    return d ? core._nd_convert(d.index, Math.fround(v)) : Number.NaN;
  };

  //-------------------------------------------------------------------------
  // The host.
  //-------------------------------------------------------------------------
  function createRenderer(gl) {
    // emscripten's GL library, pointed at the kit's context. Nothing is turned
    // on behind the kit's back: no extensions are enabled on its behalf.
    const handle = core.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: false });

    let instance = 0;
    let pushed = new Map();
    let lastTime = null;
    let width = 0;
    let height = 0;
    let frames = 0;
    let instances = 0;
    let hostOnly = false;

    function start(w, h) {
      if (instance) core._nd_delete(instance);
      instance = core._nd_new();
      pushed = new Map();
      frames = 0;
      instances += 1;
      width = w;
      height = h;
      if (!core._nd_init_gl(instance, w, h)) {
        const why = shaders.refused > 0
          ? 'the GLSL the compiled plugin handed to GL is not this page’s checked copy, so it was not compiled'
          : `the plugin’s InitGL failed${printed.length ? `: ${printed.slice(-3).join(' ')}` : ''}`;
        core._nd_delete(instance);
        instance = 0;
        throw new Error(`the VFD could not start: ${why}`);
      }
    }

    function push(params) {
      for (const p of params.spec) {
        const value = hostValue(p, params.get(p.id));
        if (value === null || pushed.get(p.id) === value) continue;
        pushed.set(p.id, value);
        core._nd_set_float(instance, byName.get(p.name).index, value);
      }
    }

    return {
      /**
       * One VFD frame at page time `time`. A new instance on the first VFD
       * frame and on Restart (the page's clock going back); its audio is
       * begun as ndtest's Driver begins it, after the parameters are set.
       */
      render({ params, width: w, height: h, time, signal }) {
        core.GL.makeContextCurrent(handle);
        const which = signalById(signal).which;
        // A feed, once begun, is what earshot reads until a device is
        // selected, so going to or from "nothing fed" is a new instance.
        const fresh = !instance || (lastTime !== null && time < lastTime) || (which < 0) !== hostOnly;
        lastTime = time;
        if (fresh) {
          start(w, h);
          push(params);
          hostOnly = which < 0;
          if (!hostOnly) core._nd_feed_begin(instance);
        } else {
          if (w !== width || h !== height) {
            core._nd_resize(instance, w, h);
            width = w;
            height = h;
          }
          push(params);
        }
        if (!hostOnly) core._nd_feed(instance, time * FEED_RATE, which);
        if (!core._nd_process(instance, time)) throw new Error('the plugin’s ProcessOpenGL failed');

        frames += 1;
        if (frames <= 3) {
          const error = gl.getError();
          if (error !== gl.NO_ERROR) {
            gl.canvas.dataset.glError = `0x${error.toString(16)}`;
            console.error(`WebGL reported error 0x${error.toString(16)} on frame ${frames} of the VFD`);
          }
        }
        let lit = 0;
        let dots = 0;
        for (let c = 0; c < core._nd_vfd_columns(instance); c += 1) {
          lit += core._nd_vfd_lit(instance, c);
          if (core._nd_vfd_dot(instance, c) >= 0) dots += 1;
        }
        // For anything checking the page from outside.
        gl.canvas.dataset.vfdFrames = String(frames);
        gl.canvas.dataset.vfdInstances = String(instances);
        gl.canvas.dataset.vfdLit = String(lit);
        gl.canvas.dataset.vfdDots = String(dots);
        gl.canvas.dataset.vfdSignal = signalById(signal).id;
      },

      stats() {
        if (!instance) return null;
        const columns = [];
        for (let c = 0; c < core._nd_vfd_columns(instance); c += 1) {
          columns.push({
            hz: core._nd_vfd_hz(instance, c),
            present: core._nd_vfd_present(instance, c) === 1,
            level: core._nd_vfd_level(instance, c),
            lit: core._nd_vfd_lit(instance, c),
            dot: core._nd_vfd_dot(instance, c),
          });
        }
        return {
          columns,
          lamp: ['amber', 'green', 'red', 'blinking red'][core._nd_vfd_lamp(instance)] ?? '?',
          state: text(core._nd_capture_state(instance)),
          rate: core._nd_sample_rate(instance),
          thirds: core._nd_third_count(instance),
          audioTime: core._nd_audio_time(instance),
          fed: core._nd_fed(instance),
          skipped: core._nd_skipped(instance),
          devices: core._nd_devices_listed(instance),
          calls: { engines: core._nd_calls(0), lists: core._nd_calls(1), selects: core._nd_calls(2) },
          frames,
          instances,
        };
      },

      log() {
        try {
          return core.FS.readFile(text(core._nd_log_path()), { encoding: 'utf8' });
        } catch {
          return '';
        }
      },
    };
  }

  return { core, declared, checkPanel, hostValue, convert, createRenderer, shaders };
}
