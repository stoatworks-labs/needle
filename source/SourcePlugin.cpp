/**
	The FF_SOURCE registration, and nothing else.

	**This file is listed directly in the source target, not in the shared
	object library.** `CFFGLPluginInfo` registers itself from a file-scope
	constructor and nothing ever references it by name, so in a static archive
	the linker is entitled to drop the whole translation unit -- giving a bundle
	that loads, exports `plugMain`, and reports that it contains no plugins.

	    nm -gU Needle.bundle/Contents/MacOS/Needle | grep plugMain

	That is also why the shared code is an OBJECT library rather than a STATIC
	one, and it is why `oxbow selftest` is the only check that proves a bundle
	actually registers anything.

	`ND01`: four characters, unique across the fleet. `NB01` is nib's.
*/
#include "Needle.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< needle::NeedlePlugin >,                   // Create method
	"ND01",                                                  // Plugin unique ID of maximum length 4
	"Needle",                                                // Plugin name
	2,                                                       // API major version number
	1,                                                       // API minor version number
	0,                                                       // Plugin major version number
	1,                                                       // Plugin minor version number
	FF_SOURCE,                                               // Plugin type
	"Audio meters with the ballistics their standards actually specify.\n\n"
	"A VU whose movement is solved from ANSI C16.5 -- 99% of a step in 300 ms with 1 to 1.5% overshoot, "
	"which fixes the damping and the natural frequency and leaves nothing to taste. A peak programme meter "
	"with IEC 60268-10 type II's fall-back of 20 dB in 2.8 s. A ten-step LED bargraph on the LM3915's 3 dB "
	"law, with a hold bar. And a 6U5 magic eye whose shadow closes at the reference, with warm-up, phosphor "
	"persistence and a target that can be worn out.\n\n"
	"The ballistics are integrated on the CPU at 4800 Hz, so a 60 fps frame cannot alias a 300 ms movement. "
	"Standard locks every constant to its specification; Free hands the rise, the fall and the overshoot "
	"back. Reads the host's spectrum, summed over every bin.",// Plugin description
	"Needle FFGL source"                                     // About
);

extern "C" const char* NeedleSourceBuildStamp()
{
	return "needle " NEEDLE_VERSION " source, built " __DATE__ " " __TIME__;
}
