#pragma once

/**
	The one number this plugin gets from the host, and everything that is not
	known about it.

	## What comes in

	Resolume fills an `FF_USAGE_FFT` buffer parameter: **64 float bins, once per
	rendered frame**. That is the entire audio surface FFGL 2.1 offers a plugin.
	There is no RMS parameter, no peak parameter and no sample stream; a plugin
	that wants a level has to make one out of the spectrum.

	## What is assumed, said outright

	Three things about that buffer are genuinely unknown here, and the fleet has
	been carrying all three as silent assumptions:

	1. **What the bin values are.** Magnitude, or power? Different repos in this
	   fleet assume different things -- regauss takes their square root before
	   using them, spasis does not -- and nobody has measured it. Here it is a
	   control, `Bin Law`, because a meter's whole job is to be right about
	   level and a factor-of-two error in decibels is not something to bury.
	2. **What full scale is.** Whether a full-scale sine produces a bin of 1.0
	   depends on the host's window, its normalisation and its own headroom.
	   Nothing here can discover that, so `Reference Level` names the dBFS the
	   meter calls zero and `Sensitivity` trims it, and the README says the
	   calibration is nominal. What CAN be chosen is which footing the level
	   stands on, and after v0.1.1 it is Resolume's own -- see "The level"
	   below.
	3. **Where the bins sit in frequency.** The fleet assumes they are linear
	   from 0 to Nyquist. That assumption has never been tested either -- **and
	   this plugin does not need it to be true.**

	Point 3 is the one worth dwelling on. The level below is taken over **all
	64 bins**, which by Parseval's theorem is the signal's power however the
	bins are laid out in frequency: reorder them, warp them, space them
	logarithmically, and the sum is unchanged. The bin-to-frequency mapping
	would only matter to something that weighted bins differently from one
	another -- a band split, an onset detector, a spectrum display -- and there
	is none of that here. So the fleet's untested assumption is one this plugin
	is structurally immune to, which is a better answer than testing it would
	have been.

	## The level: an RMS over the bins, Resolume's footing

	The level is the **root mean square over the bins** -- `sqrt( sum b^2 / n )`
	-- which is exactly what the FFGL SDK's own `ffglqs::Audio::Update` calls the
	"global volume ... between 0.0 and 1.0" (external/ffgl/source/lib/
	ffglquickstart/FFGLAudio.cpp). Dividing by a constant does not touch the
	Parseval argument above: the result still cannot depend on where the bins
	sit in frequency. It only decides where 1.0 is: every bin at 1.0 reads
	0 dBFS, and one bin of 1.0 on its own reads 20.log10(1/8) = -18.06 dBFS.

	v0.1.0 and v0.1.1 took the root of the plain SUM, which is the same number
	18.06 dB higher (the 8 is the root of 64), on the assumption that a
	full-scale sine sums to 1.0 across the spectrum. The first report from a
	real Resolume rig (needle#1, 2026-10-09) said the opposite: the needle sat
	off the end of the scale until Reference Level was at its 0 dBFS stop,
	which is the default plus the same 18 dB. So the SDK's footing is taken, and
	the shipped -18 dBFS Reference puts programme at an SDK volume of -18 dB on
	0 VU. That is a better-grounded guess, not a measurement: nobody has yet
	logged what Resolume puts in the bins for a known tone. `ndtest --defaults`
	holds the law to `ffglqs::Audio` itself, so the two cannot drift apart.

	## What this is NOT

	It is **not a sample-peak detector**, and it cannot be. A magnitude spectrum
	has no phase, so the time-domain peak inside the block is not recoverable
	from it: the same 64 magnitudes describe a waveform that peaks at the sum of
	them and one that peaks at their RMS, and there is no way to tell which
	arrived. So the PPM and the bargraph here are driven by a block RMS.

	Concretely, against a real PPM:

	  * on a sine they differ by the crest factor of a sine, 3.01 dB;
	  * on anything with a higher crest factor -- percussion, speech, anything
	    clipped -- this under-reads by that material's crest factor;
	  * a transient shorter than one video frame is invisible to it entirely,
	    because there is one spectrum per frame and 5 ms of a 16.7 ms frame is
	    already averaged into it before the plugin sees anything.

	The **ballistics** are the standard's, exactly, and that is what the harness
	checks. The **detector** is not, and that is why nothing in the harness
	claims it is. A real peak would need a real audio device; spasis already has
	that code and taking it is a v0.2 job, not a v0.1 one.
*/
namespace needle::audio
{

/// The number of bins the host is told to fill. Resolume's own figure.
inline constexpr int kBins = 64;

/// One amplitude out of one spectrum: the RMS over the bins, on the FFGL SDK's
/// footing, where every bin at 1.0 reads 1.0 (see "The level", above).
///
/// `power` selects the Bin Law: when the host sends magnitudes the result is
/// `sqrt( sum b^2 / n )`, and when it sends power it is `sqrt( sum b / n )`.
/// Both are the same quantity -- the root of the mean power per bin -- read
/// through the right assumption about what arrived.
float LevelFromSpectrum( const float* bins, int count, bool power );

} // namespace needle::audio
