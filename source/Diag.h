#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    A small member of the fleet's `diag` family, carried from orrery. The rest of
    the repos get a rotating log, a crash report and a diagnostics bundle; an
    FFGL plugin gets only the log, for two reasons:

    - **No crash handler.** A plugin loaded into Resolume must not install a
      process-wide signal handler. It would intercept faults that are not ours
      and interfere with the host's own handling. A plugin has no business
      deciding what happens when Resolume dies.
    - **No bundle command.** There is no UI to hang one off -- a plugin is a
      list of sliders in someone else's inspector.

    ## Why this exists here, specifically

    A meter fails in ways an operator cannot tell apart from the front. The
    needle does not move. That one symptom covers at least four completely
    different faults, and nothing but a log distinguishes them:

    - **A shader would not compile.** The vendor, renderer and version strings go
      next to it, because a shader that builds on one machine and not another is
      a driver answer, not a source answer.
    - **The font would not upload.** The scale numbers come off one small
      texture; without it the dial draws and the legend does not, which reads as
      "the labels are broken" rather than as a GL refusal.
    - **No audio reached the plugin.** Resolume fills an `FF_USAGE_FFT` buffer
      parameter. If nothing is routed to the layer, every bin is zero and a
      correct meter sits at rest -- which looks exactly like a broken one. The
      log records the measured input level as a *transition*, so a session that
      never saw a signal says so in one line.
    - **The host clock unit.** Resolume sends milliseconds and an offline harness
      sends seconds; the fleet has paid for that confusion twice. What the clock
      settled on is stated outright rather than inferred from a code read. It
      matters more here than almost anywhere: every ballistic constant in this
      plugin is a time, so a clock a thousand times fast turns a 300 ms VU
      movement into a 300 us one and the needle simply pins.

    ## Rate

    `ProcessOpenGL` runs fifty times a second. Nothing here is called from a
    per-frame path except through `stateChanged`, which logs a *transition* --
    so a wall that is black for an hour writes one line, not 180,000.
*/
namespace needle::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Log `message` only when it differs from the last message logged under `key`.
/// For the per-frame paths, where the interesting event is the change and the
/// steady state is noise.
void stateChanged( const std::string& key, const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace needle::diag
