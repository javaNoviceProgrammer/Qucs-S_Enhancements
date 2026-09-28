/*
 * qucscontrol_ngspice.cpp - ngspice_commands: the commands of ngspice,
 *                           each summed up in a line, by category, with
 *                           how Qucs-S writes them - and which of them the
 *                           ngspice of the settings has
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "extsimkernels/spicecompat.h"
#include "main.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

using namespace qucs_s::control;

namespace {

// Enhanced: not in stock ngspice-46, the enhanced build's (its command
// table against the stock one's); DotCard: a netlist line only, no command;
// Alias: another spelling of a command. Neither of the last two is among
// help all's lines.
enum : unsigned { Enhanced = 1, DotCard = 2, Alias = 4 };

const struct Category {
    const char* id;
    const char* title;
    const char* note;
} kCategories[] = {
    {"analysis", "Analyses",
     "Each takes the arguments of its dot-card (.tran, .ac, ...) and runs at once; run runs the deck's dot-cards. Results are "
     "vectors in a plot named after the analysis (tran1, ac1, ...)."},
    {"rf", "RF and periodic steady state",
     "Ports for sp are voltage sources with portnum and z0 (V1 in 0 dc 0 ac 1 portnum 1 z0 50). pss is stock ngspice, the others "
     "the enhanced build's. .pac, .pnoise, .pxf and .psp are dot-cards only, after a .pss's arguments."},
    {"measure", "Measurement and signal processing",
     "They act on vectors already in memory, after an analysis."},
    {"vectors", "Vectors, variables and expressions",
     "Expressions: + - * / ^, comparisons, ?:, v(n), v(n1,n2), i(vsrc), @dev[param], vdb vp vm, mag ph cph real imag db abs sqrt "
     "exp ln log10 sin cos tan atan, deriv integ, mean avg rms stddev vecmax vecmin length vector, and functions from define. $var reads "
     "a variable; a scalar is a vector of one."},
    {"output", "Plots and data files", ""},
    {"circuit", "The circuit and its devices", ""},
    {"models", "Compiled models (Verilog-A, Touchstone, XSPICE)",
     "In a deck's .control block a pre_ prefix (pre_osdi, pre_snp, pre_set, ...) runs the line before the circuit is read."},
    {"statistics", "Statistics, yield, sweeps and optimization",
     "Random values come from .param expressions: agauss(nominal, abs_variation, sigma), gauss, aunif, unif, limit; mvnorm(k) after "
     "mccorr for correlated draws."},
    {"reliability", "Post-layout and reliability", ""},
    {"debug", "Breakpoints, stepping and checkpoints", ""},
    {"script", "Control flow and scripting",
     "A .control block is a small shell-like language: blocks end with end; $var and $&vector read values. Strings: r\"...\" keeps "
     "case and spaces, f\"... {expr:.3f} ...\" is evaluated when the line runs, rf\"...\" both (the enhanced build's)."},
    {"digital", "Event-driven (XSPICE digital) nodes", ""},
    {"utility", "Help and utilities", ""},
};

const struct Command {
    const char* name;
    const char* category;
    const char* syntax;
    const char* summary;
    unsigned flags;
} kCommands[] = {
    // Analyses.
    {"ac", "analysis", "dec|oct|lin <points> <fstart> <fstop>", "Small-signal AC sweep around the operating point; a source with 'ac 1' drives it, vdb() and vp() read it.", 0},
    {"dc", "analysis", "<source> <start> <stop> <step> [<source2> <start2> <stop2> <step2>]", "DC sweep of a source (or a resistor, or temp); a second, nested sweep gives a family of curves.", 0},
    {"disto", "analysis", "dec|oct|lin <points> <fstart> <fstop> [<f2overf1>]", "Small-signal harmonic distortion (two-tone intermodulation with f2overf1); sources marked distof1/distof2; disto1/disto2 plots.", 0},
    {"noise", "analysis", "v(<out>[,<ref>]) <input-source> dec|oct|lin <points> <fstart> <fstop> [<points-per-summary>]", "Output- and input-referred noise density (noise1: onoise_spectrum, inoise_spectrum) and the integrated noise (noise2).", 0},
    {"op", "analysis", "", "DC operating point: every node voltage and source current, capacitors open and inductors shorted.", 0},
    {"pz", "analysis", "<in+> <in-> <out+> <out-> vol|cur pz|pol|zer", "Poles and zeros of the transfer function from an input node pair to an output pair.", 0},
    {"sens", "analysis", "<output> [ac dec|oct|lin <points> <fstart> <fstop>]", "DC (or AC) sensitivity of an output to every device value and model parameter, in a sens1 plot.", 0},
    {"tf", "analysis", "<output> <input-source>", "DC small-signal transfer function: the gain and the input and output resistance; output is v(n), v(n1,n2) or i(vsrc).", 0},
    {"tran", "analysis", "<tstep> <tstop> [<tstart> [<tmax>]] [uic]", "Transient (time-domain) analysis; uic starts from the initial conditions instead of an operating point.", 0},
    {"run", "analysis", "[rawfile]", "Runs the analyses of the deck's dot-cards, optionally writing a rawfile.", 0},
    // RF and periodic steady state.
    {"sp", "rf", "dec|oct|lin <points> <fstart> <fstop> [1]", "S-parameters of the ports (S_1_1, S_2_1, ...); a final 1 adds the noise figure (NF, NFmin, Cy_i_j).", 0},
    {"pss", "rf", "<fguess> <tstab> <node> <points> <harmonics> <sc_iter> <steady_coeff> [uic]", "Periodic steady state by shooting: the settled period (pss1) and its spectrum (the plot current after it); the base of .pac, .pnoise, .pxf, .psp.", 0},
    {"hb", "rf", "f0 K [points] [maxiter]", "Harmonic balance: the steady-state spectrum of a circuit driven at f0, to K harmonics.", Enhanced},
    {"hbosc", "rf", "oscnode K [fguess] [tstab]", "Autonomous harmonic balance of an oscillator: its steady state and frequency, seeded from a transient.", Enhanced},
    {"phasenoise", "rf", "fstart fstop [points]", "Oscillator phase noise L(df), from the PPV around an hbosc steady state.", Enhanced},
    {"qpss", "rf", "expr f1 f2 [periods] [maxorder] | expr f1 f2 hb [K1] [K2]", "Two-tone quasi-periodic steady state, in time or (hb) by harmonic balance; incommensurate tones allowed.", Enhanced},
    {"qpac", "rf", "f_in | dec|oct|lin N fstart fstop", "Two-tone periodic AC: each sideband's response at f_in, or swept, around a 'qpss ... hb' point.", Enhanced},
    {"qpxf", "rf", "output_node f_in | output_node dec|oct|lin N fstart fstop", "Two-tone periodic transfer from each sideband to one output, around a 'qpss ... hb' point.", Enhanced},
    {"qpnoise", "rf", "output_node f_in [cyclo] | output_node dec|oct|lin N fstart fstop", "Two-tone periodic noise at an output (cyclo: cyclostationary), around a 'qpss ... hb' point.", Enhanced},
    {"envelope", "rf", "node fc tstop [nppp N] [m M0] [maxm Mmax] [reltol t] [settle ts]", "Envelope following: the slow amplitude and phase of a carrier-driven circuit over a long time.", Enhanced},
    {"stb", "rf", "<Vprobe> <Iprobe> dec|oct|lin <N> <fstart> <fstop>", "Loop-gain stability: phase and gain margin by double injection through a series 0 V and a shunt 0 A probe; the vector loopgain.", Enhanced},
    {"loadpull", "rf", "-load <R> <L> <C> -out <node> -drive <Vsrc> -f <freq> [-supply <Vsrc>] [-z0 50] [-n 15] [-gmax 0.85] [-nper 20] [-source]", "PA load-pull (or source-pull): a transient at each load over the Smith chart; Pout, gain, PAE, efficiency in a loadpull plot.", Enhanced},
    {"rfstab", "rf", "[S11 S12 S21 S22]", "Two-port stability from sp results: Rollett K, |Delta|, mu, MSG/MAG per frequency, and whether it is unconditionally stable.", Enhanced},
    {".pac", "rf", ".pac <pss arguments> dec|oct|lin <N> <fstart> <fstop> [sideband]", "Periodic AC: the small-signal conversion to each sideband around a .pss steady state (a mixer under its LO).", Enhanced | DotCard},
    {".pnoise", "rf", ".pnoise <pss arguments> <outnode> <insrc> dec|oct|lin <N> <fstart> <fstop> [cyclo]", "Periodic noise folded from every sideband, against the offset from the carrier.", Enhanced | DotCard},
    {".pxf", "rf", ".pxf <pss arguments> <outnode> dec|oct|lin <N> <fstart> <fstop> [sideband]", "Periodic transfer function from every sideband to one output (the vector xf).", Enhanced | DotCard},
    {".psp", "rf", ".psp <pss arguments> dec|oct|lin <N> <fstart> <fstop>", "Periodic S-parameters of a pumped network, sideband by sideband.", Enhanced | DotCard},
    // Measurement and signal processing.
    {"meas", "measure", "ac|dc|tran|sp <name> <kind> ...", "Measures a result into a scalar: trig/targ delays, when, find ... at, min, max, avg, rms, pp, integ, deriv.", 0},
    {"fft", "measure", "vector ...", "FFT of transient vectors into a new frequency plot (linearize first).", 0},
    {"spec", "measure", "start_freq stop_freq step_freq vector ...", "Spectrum of transient vectors over a frequency window, into a new plot.", 0},
    {"psd", "measure", "vector ...", "Power spectral density by FFT.", 0},
    {"fourier", "measure", "fund_freq vector ...", "The DC term, first harmonics and THD of a transient waveform.", 0},
    {"eye", "measure", "<expr> -ui <T> [-tstart t0] [-threshold v]", "Eye diagram and jitter of a serial (SerDes) signal: eye height and width.", Enhanced},
    {"track", "measure", "<expr> [<expr> ...] [-range x0 x1] [-spec <spec>] [-analysis <plot|type>] [-which all|first|last|N|-N] [-edge rise|fall|both] [-at entry|exit|mid] [-prominence p] [-raw] [-output name ...]", "Every place a condition holds (a local or global max or min, a crossing, a region), as a plot trackN, with expressions read there.", Enhanced},
    {"linearize", "measure", "[vec ...]", "Resamples a transient onto an even time step (needed before fft).", 0},
    {"cutout", "measure", "[vec ...]", "Cuts a time window into a new plot, from the vectors cut-tstart to cut-tstop (let them first).", 0},
    {"transpose", "measure", "varname ...", "Transposes a multi-dimensional vector (a swept family).", 0},
    {"cross", "measure", "vecname number [vector ...]", "Makes a vector of element 'number' of each of the vectors.", 0},
    {"compose", "measure", "var parm=val ...", "Builds a vector from values or a range: compose v start=0 stop=1 step=0.1.", 0},
    {"reshape", "measure", "vector ... [shape]", "Changes the dimensions of a vector.", 0},
    {"settype", "measure", "type vec ...", "Sets a vector's type (voltage, current, time, ...) for its units.", 0},
    {"deftype", "measure", "spec name pat ...", "Defines vector and plot types.", 0},
    // Vectors, variables and expressions.
    {"let", "vectors", "varname = expr", "Creates or changes a vector from an expression: let gain = vdb(out).", 0},
    {"unlet", "vectors", "varname ...", "Deletes vectors.", 0},
    {"print", "vectors", "[col|line] expr ...", "Prints vectors' values (a table with col); > file writes them to a file.", 0},
    {"display", "vectors", "", "Lists the vectors of the current plot with their types and lengths.", 0},
    {"define", "vectors", "[[func (args)] stuff]", "Defines a function for expressions: define sq(x) x*x.", 0},
    {"undefine", "vectors", "[func ...]", "Removes functions made with define.", 0},
    {"set", "vectors", "[option] [option = value] ...", "Sets a control variable (numdgt, filetype, units, ...); $name reads it.", 0},
    {"unset", "vectors", "varname ...", "Clears a variable.", 0},
    {"setcs", "vectors", "[option] [option = value] ...", "set, keeping the value's case.", 0},
    {"option", "vectors", "[option] [option = value] ...", "Sets a simulator option (reltol, abstol, gmin, method, klu, ...), as .options does.", 0},
    {"options", "vectors", "[option] [option = value] ...", "The same as option.", 0},
    {"setscale", "vectors", "[vecname [vecname]]", "Changes the plot's default scale, or one vector's scale.", 0},
    {"remzerovec", "vectors", "", "Removes zero-length vectors.", 0},
    // Plots and data files.
    {"plot", "output", "expr ... [vs expr] [xl xlo xhi] [yl ylo yhi] [xlog] [ylog]", "Plots vectors in ngspice's own plot window.", 0},
    {"pyplot", "output", "[file] plotargs | -export <sigs> | -hist <sigs> | -contour <z> <x> <y> | -smith <sigs> | -fft <sigs> | -bode|-nyquist|-polar <sigs> | -eye <expr> -ui <T>", "matplotlib plots: with set pyplot_terminal=png (svg, pdf), pyplot <name> ... writes <name>.png; else a window. -export: the data (.npy).", Enhanced},
    {"gnuplot", "output", "file plotargs", "Plots through gnuplot, writing file.plt and file.data.", 0},
    {"hardcopy", "output", "file plotargs", "Writes a plot to a PostScript file.", 0},
    {"asciiplot", "output", "plotargs", "A plot drawn in text characters.", 0},
    {"wrdata", "output", "file plotargs", "Writes vectors as columns of numbers, each after its scale; -csv (or set wr_csv) a CSV with a header row.", 0},
    {"write", "output", "file expr ...", "Writes vectors to a rawfile (text with set filetype=ascii); load reads it back.", 0},
    {"load", "output", "file ...", "Loads a rawfile as a new plot.", 0},
    {"wrsnp", "output", "file [ri|ma|db] [s|y|z] [hz|khz|mhz|ghz]", "Writes sp results to a Touchstone .sNp file.", Enhanced},
    {"rdsnp", "output", "file [nports]", "Reads a Touchstone file (S, Y or Z, any port count) into a plot of S_i_j vectors.", Enhanced},
    {"wrs2p", "output", "file", "Writes two-port S-parameters to a Touchstone .s2p file.", 0},
    {"wrnodev", "output", "", "Saves the present node voltages to a file.", 0},
    {"diff", "output", "plotname plotname [vec ...]", "Compares two plots, vector by vector.", 0},
    {"setplot", "output", "[plotname]", "Makes another plot (tran1, ac1, noise1, ...) the current one.", 0},
    {"destroy", "output", "[plotname] ... | all", "Frees plots' data.", 0},
    // The circuit and its devices.
    {"source", "circuit", "file", "Reads a netlist or a script.", 0},
    {"listing", "circuit", "[logical] [physical] [deck] [expand] [tree]", "Prints the circuit as read (expand: subcircuits expanded).", 0},
    {"edit", "circuit", "[filename]", "Edits the deck in a text editor, then loads it.", 0},
    {"circbyline", "circuit", "line", "Enters a circuit a line at a time (.end last).", 0},
    {"alter", "circuit", "dev param=value [param=value ...] | @dev[param]=value | dev=value", "Changes device parameters for the next analysis.", 0},
    {"altermod", "circuit", "model param=value [param=value ...] | @model[param]=value", "Changes .model parameters.", 0},
    {"alterparam", "circuit", "[subckt] name=value", "Changes a .param; reset applies it.", 0},
    {"show", "circuit", "devices ... [: parameters ...]", "Prints device parameters and operating-point values (show m1 : gm id).", 0},
    {"showmod", "circuit", "models ... [: parameters ...]", "Prints model parameters.", 0},
    {"devhelp", "circuit", "[device [parameter]]", "Lists the device types, or a device's parameters.", 0},
    {"inventory", "circuit", "", "Counts the devices of each type.", 0},
    {"save", "circuit", "[all] [node ...]", "Chooses what the next analysis keeps: nodes, branch currents, @m1[id].", 0},
    {"setcirc", "circuit", "[circuit name]", "Switches between the circuits loaded.", 0},
    {"remcirc", "circuit", "", "Removes the current circuit.", 0},
    {"removecirc", "circuit", "[circuit name]", "Removes the current (or named) circuit from memory.", 0},
    {"reset", "circuit", "", "Rebuilds the circuit's state (applies alterparam; ends a run halted at a breakpoint).", 0},
    {"dump", "circuit", "", "Prints the circuit's matrix structure.", 0},
    // Compiled models.
    {"osdi", "models", "[-f] [-va] library ...", "Loads compiled Verilog-A (OSDI) libraries; -va compiles a .va with openvaf-r first, -f reloads a recompiled one.", 0},
    {"pre_osdi", "models", "[-f] [-va] library ...", "osdi, run before the circuit is read (in a deck's .control block).", Enhanced},
    {"snp", "models", "[-osdi|-native] [-maxerr <x>|-force] [-maxpoles <N>|-order <N>] file.sNp [module]", "Fits a Touchstone file to a Verilog-A n-port OSDI model (use as pre_snp, then pre_osdi the .osdi).", Enhanced},
    {"pre_snp", "models", "[-osdi|-native] [-maxerr <x>|-force] [-maxpoles <N>|-order <N>] file.sNp [module]", "snp, run before the circuit is read.", Enhanced | Alias},
    {"codemodel", "models", "library ...", "Loads XSPICE code-model (.cm) libraries.", 0},
    // Statistics, yield, sweeps and optimization.
    {"montecarlo", "statistics", "<N> [-lhs] [-warm] [-seed <s>] [-analysis <cmd>] (-spec <metric> -max <hi>|-min <lo>)... (-expr [name=]<expression>)... (-track \"<track arguments>\")... [-writemc [name=]<expression> ...]", "Monte Carlo: N samples of the random .params, the yield of the -specs (95% CI, failures per spec), -expr values in a montecarloN plot.", Enhanced},
    {"mcsample", "statistics", "lhs <N> [seed <s>] | random | off", "Latin-hypercube sampling of the random .params (a lower variance than plain Monte Carlo).", Enhanced},
    {"highsigma", "statistics", "<N> [-scale <lambda>] [-inflate <param>]... [-seed <s>] [-analysis <cmd>] -metric <expr> [-max <hi>] [-min <lo>]", "Rare-event (high-sigma) failure probability by scaled-sigma importance sampling: P(fail), its error, the sigma to fail.", Enhanced},
    {"wcd", "statistics", "-metric <expr> [-max <hi>] [-min <lo>] [-analysis <cmd>] [-maxiter <n>] [-tol <t>] [-step <h>] [-is <N> [-seed <s>]]", "Worst-case distance: the closest failure point in standardised parameter space, its sigma (beta) and Phi(-beta).", Enhanced},
    {"mccorr", "statistics", "<k> <m11> <m12> ... <mkk> | off", "Registers a k x k correlation matrix; mvnorm(1..k) in .params draws correlated normals.", Enhanced},
    {"setseed", "statistics", "[seed value]", "Reseeds the random number generator, for draws that repeat.", 0},
    {"mc_source", "statistics", "", "Reads the circuit deck again for the next Monte Carlo sample.", 0},
    {"sweep", "statistics", "<knob> (<start> <stop> <step> | lin|dec|oct <N> <start> <stop> | list <v>...) [-analysis <cmd>] [-output <expr> ...]", "Sweeps any knob (a device value, @model[param], a .param) and records an analysis's outputs into a plot.", Enhanced},
    {"corners", "statistics", "[-list <c1>[,<c2>...]] [-nonominal] [-analysis <cmd>] [-output <expr> ...] [-mc <N> <montecarlo arguments>]", "Runs the analysis at each process corner the loaded Verilog-A models declare (tt first); -mc a Monte Carlo per corner.", Enhanced},
    {"writemc", "statistics", "[name=]<expression> ...", "Adds values the script computed to the row .option savemc wrote for the last run.", Enhanced},
    {"writecorner", "statistics", "[name=]<expression> ...", "writemc for the corner file (.option savecorner).", Enhanced},
    {"writecr", "statistics", "[name=]<expression> ...", "Short for writecorner.", Enhanced},
    {"optimize", "statistics", "(-param|-mparam|-dparam) name init lo hi ... -analysis <cmd> (-minimize <expr> | -target <expr> <val> [<w>] ...) [-method nm|lm] [-maxiter N] [-tol T] [-verbose]", "Optimizer (Nelder-Mead or Levenberg-Marquardt) of device, model or .param values within bounds, to minimize an expression or meet targets.", Enhanced},
    // Post-layout and reliability.
    {"reduce", "reliability", "fmax [factor f] [file fname] [name subckt] [keep node ...]", "Reduces an R/C parasitic network (TICER) to an equivalent .subckt, valid up to fmax.", Enhanced},
    {"aging", "reliability", "t_target [rate opvar] [param ageparam] [dynamic tstop [tstep]] [verbose]", "Ages every aging-capable device to a lifetime (HCI, NBTI, TDDB) for fresh-against-aged runs.", Enhanced},
    {"emir", "reliability", "[rail V] [thresh frac] [thick m] [jmax A/m2] [n exp] [tref s] [top k] [verbose]", "Power-grid IR drop and electromigration current density per segment, with Black's-equation MTTF.", Enhanced},
    // Breakpoints, stepping and checkpoints.
    {"stop", "debug", "after <n> | when <expr> ...", "Sets a breakpoint: after a number of points, or when a condition holds (stop when v(out) > 0.9).", 0},
    {"resume", "debug", "", "Continues an analysis halted at a breakpoint.", 0},
    {"step", "debug", "[number]", "Advances an analysis by a number of points.", 0},
    {"trace", "debug", "[all] [node ...]", "Prints nodes at every point as an analysis runs.", 0},
    {"iplot", "debug", "[-w width] [-d initial_steps] [-o] [all] [node ...]", "Plots nodes in a window as an analysis runs.", 0},
    {"status", "debug", "", "Lists the breakpoints and traces.", 0},
    {"delete", "debug", "[all] [break number ...]", "Removes breakpoints and traces.", 0},
    {"savestate", "debug", "file", "Checkpoints a transient's state to a file.", Enhanced},
    {"loadstate", "debug", "file", "Restores a checkpoint and continues its transient, even in another process.", Enhanced},
    {"snsave", "debug", "file", "Saves a snapshot of the simulation.", 0},
    {"snload", "debug", "netlist snapshot", "Loads a snapshot (the circuit file and the snapshot).", 0},
    {"where", "debug", "", "Names the node or device that last failed to converge.", 0},
    // Control flow and scripting.
    {"if", "script", "condition", "Runs the lines up to else or end when the condition holds.", 0},
    {"else", "script", "", "if's other branch.", 0},
    {"end", "script", "", "Ends an if, while, dowhile, foreach or repeat block.", 0},
    {"while", "script", "condition", "Repeats a block while a condition holds (tested first).", 0},
    {"dowhile", "script", "condition", "Repeats a block while a condition holds (tested after each pass).", 0},
    {"foreach", "script", "variable value ...", "Runs a block once for each value, in $variable.", 0},
    {"repeat", "script", "[number]", "Runs a block a number of times, or forever.", 0},
    {"break", "script", "", "Leaves a loop.", 0},
    {"continue", "script", "", "Goes on with a loop's next pass.", 0},
    {"label", "script", "word", "Marks a place for goto.", 0},
    {"goto", "script", "word", "Jumps to a label.", 0},
    {"shift", "script", "[var] [number]", "Shifts argv, or a list variable, to the left.", 0},
    {"echo", "script", "[stuff ...]", "Prints text and $variables.", 0},
    {"shell", "script", "[args]", "Runs a command of the host's shell.", 0},
    {"cd", "script", "[directory]", "Changes the working directory.", 0},
    {"getcwd", "script", "", "Prints the working directory.", 0},
    {"strcmp", "script", "varname s1 s2", "Sets $varname to the comparison of two strings (0 when equal).", 0},
    {"strstr", "script", "varname s1 s2", "Sets $varname to where s2 is in s1.", 0},
    {"strslice", "script", "varname s1 offset length", "Sets $varname to part of a string.", 0},
    {"fopen", "script", "handle file_name [mode]", "Opens a file; its handle in $handle.", 0},
    {"fread", "script", "handle result [length]", "Reads a line of an open file into $result (its status in $length).", 0},
    {"fclose", "script", "handle", "Closes a file.", 0},
    // Event-driven nodes.
    {"esave", "digital", "all | none | node ...", "Chooses the event-driven nodes recorded.", 0},
    {"eprint", "digital", "node ...", "Prints event-driven nodes' values.", 0},
    {"edisplay", "digital", "", "Lists the event-driven nodes.", 0},
    {"eprvcd", "digital", "[-a] [-t timescale] node ...", "Writes event-driven nodes to a VCD file, for a waveform viewer.", 0},
    // Help and utilities.
    {"help", "utility", "[subject] ...", "Help on commands: help <command>; help all lists them.", 0},
    {"tutorial", "utility", "[subject] ...", "The same as help.", 0},
    {"newhelp", "utility", "[command name] ...", "Help on commands.", 0},
    {"oldhelp", "utility", "[command name] ...", "Help on commands, in the old form.", 0},
    {"history", "utility", "[-r] [number]", "Prints the commands given before.", 0},
    {"alias", "utility", "[[word] alias]", "Defines a command alias.", 0},
    {"unalias", "utility", "word ...", "Removes an alias.", 0},
    {"synhl", "utility", "[command line ...]", "Shows a line with the prompt's syntax highlighting.", Enhanced},
    {"version", "utility", "[number]", "Prints ngspice's version.", 0},
    {"sysinfo", "utility", "", "Prints the host's CPU and memory.", 0},
    {"rusage", "utility", "[resource ...]", "Prints the time and memory used (all: everything).", 0},
    {"cdump", "utility", "", "Prints the control structures read.", 0},
    {"mdump", "utility", "[outfile]", "Writes the circuit matrix.", 0},
    {"mrdump", "utility", "[outfile]", "Writes the matrix's right-hand side.", 0},
    {"check_ifparm", "utility", "", "Checks the device parameter tables (for developers).", 0},
    {"optran", "utility", "", "Sets optran's six flags: an operating point found by a transient when op does not converge.", 0},
    {"state", "utility", "", "Unimplemented.", 0},
    {"bug", "utility", "", "Reports an ngspice bug.", 0},
    {"rehash", "utility", "", "Rebuilds the table of host commands.", 0},
    {"aspice", "utility", "file [outfile]", "Runs a job in the background.", 0},
    {"rspice", "utility", "[input file]", "Runs a job on a remote server.", 0},
    {"jobs", "utility", "", "Lists the background jobs.", 0},
    {"quit", "utility", "", "Leaves ngspice.", 0},
    {"exit", "utility", "", "The same as quit.", Enhanced},
};

// How Qucs-S writes a command, or what to know of it in Qucs-S's run of
// ngspice: batch mode, a .control block of its own that ends with exit.
const struct {
    const char* name;
    const char* text;
} kInQucs[] = {
    {"ac", "An AC simulation block (.AC) writes it; add_analysis kind ac places one."},
    {"dc", "A parameter sweep (.SW) of a DC simulation block writes it."},
    {"op", "A DC simulation block (.DC) writes it; simulate with operating_point runs it alone."},
    {"tran", "A transient simulation block (.TR) writes it; add_analysis kind tran places one."},
    {"noise", "A noise block (.NOISE) writes it, and reads noise1's spectra into the dataset."},
    {"pz", "A pole-zero block (.PZ) writes it."},
    {"sens", "The .SENS (DC) and .SENS_AC blocks write it."},
    {"disto", "A distortion block (.DISTO) writes it."},
    {"fourier", "A Fourier block (.FOURIER) of a transient writes it, and the THD reaches the dataset."},
    {"fft", "An FFT block (.FFT) writes linearize and fft after its transient."},
    {"sp", "An S-parameter block (.SP) writes it (its Noise property the final 1); the ac power sources (Pac) are the ports."},
    {"optimize", "An ngspice optimize block (.NGOPT) writes it, ahead of the simulations, which then show the optimum."},
    {"sweep", "An ngspice sweep block (.NGSWEEP) writes it, and its values and plots reach the dataset."},
    {"montecarlo", "An ngspice Monte Carlo block (.NGMONTECARLO) writes it: add_component takes its 'specs' and 'records'."},
    {"corners", "An ngspice corners block (.NGCORNERS) writes it."},
    {"let", "A NutmegEq block's equations are let lines after an analysis (name=expression); its variables reach the dataset."},
    {"osdi", "Qucs-S writes a pre_osdi line for each compiled Verilog-A library the circuit uses (build_verilog_a compiles one)."},
    {"pre_osdi", "Qucs-S writes one for each compiled Verilog-A library the circuit uses (build_verilog_a compiles one)."},
    {"set", "A .spiceinit block (SPICEINIT) holds set lines read at ngspice's start; in a Nutmeg script, set acts on the lines after it."},
    {"option", "A SpiceOptions block (.OPTIONS) sets options for the whole run."},
    {"options", "A SpiceOptions block (.OPTIONS) sets options for the whole run."},
    {"write", "A file a Nutmeg script writes (write custom#ac1#.plot k) reaches the dataset when its Outputs property lists it."},
    {"print", "print x > custom#ac1#.print in a Nutmeg script, with the file in its Outputs property, puts scalars into the dataset."},
    {"plot", "Qucs-S runs ngspice with no window: plot draws nothing. Show results in a diagram (add_diagram), or write a picture with pyplot <file>."},
    {"iplot", "Qucs-S runs ngspice with no window: nothing is drawn."},
    {"pyplot", "Set pyplot_terminal=png (or svg, pdf) first and give a name without its extension (pyplot bode vdb(out) writes bode.png): without it pyplot opens a window, which Qucs-S's run has not. A relative name lands in the schematic's scratch folder, where ngspice runs; give a full path to keep it elsewhere."},
    {"quit", "Qucs-S ends its .control block with exit after writing the results; a quit in a Nutmeg script ends the run before they are written."},
    {"exit", "Qucs-S ends its .control block with it after writing the results; one in a Nutmeg script ends the run before they are written."},
    {"destroy", "Qucs-S writes destroy all and reset after each of its simulations: a Nutmeg script's plots do not outlive it."},
    {"reset", "Qucs-S writes destroy all and reset after each of its simulations."},
    {"source", "The circuit is the schematic's netlist: sourcing another replaces it for the rest of the run."},
    {"edit", "Needs an editor at the ngspice prompt: nothing for Qucs-S's run."},
    {"help", "For the ngspice prompt; ngspice_commands answers here."},
};

// Examples: the lines of a .control block (a Nutmeg script's SpiceCode in
// Qucs-S), and the netlist lines they need.
const struct {
    const char* name;
    const char* text;
} kExamples[] = {
    {"op", "op\nprint v(out) i(v1)"},
    {"dc", "* Vds swept for each value of Vgs\ndc Vds 0 5 0.05 Vgs 1 4 1\nprint -i(vds)"},
    {"ac", "ac dec 40 1 1meg\nlet gain_db = vdb(out)\nmeas ac f3db when vdb(out)=-3 fall=1"},
    {"tran", "tran 1u 400u\nmeas tran tr trig v(out) val=0.1 rise=1 targ v(out) val=0.9 rise=1"},
    {"noise", "noise v(out) Vin dec 20 10 1meg\nsetplot noise1\nprint onoise_spectrum"},
    {"tf", "tf v(out) Vin\n* transfer_function, output_impedance_at_v(out), vin#input_impedance\nprint all"},
    {"pz", "pz in 0 out 0 vol pz\nprint all"},
    {"sens", "sens v(out)\n* each the derivative of v(out) by that value\nprint v1 r1 r2"},
    {"disto", "* netlist: Vin in 0 dc 0.75 ac 1 distof1 1\ndisto dec 20 10 1g\nsetplot disto1\n* HD2 here; disto2 holds HD3\nprint vdb(c)"},
    {"sp", "* netlist: V1 in 0 dc 0 ac 1 portnum 1 z0 50 and V2 out 0 dc 0 ac 1 portnum 2 z0 50\nsp dec 50 1meg 100meg\nlet s21_db = db(S_2_1)\nwrsnp filter.s2p"},
    {"pss", "* netlist: .pss 1meg 1u b 1024 8 50 5u\nrun\n* the spectrum is the current plot; pss1 the period in time\nprint mag(b)\nsetplot pss1\nprint b"},
    {".pac", "* netlist: .pac 1meg 1u b 1024 6 50 5u lin 60 5k 900k 1\nrun\nprint db(b_usb1) db(b_lsb1)"},
    {".pnoise", "* netlist: .pnoise 1meg 1u b 1024 6 50 5u b vdc dec 25 1k 400k\nset sqrnoise\nrun\nprint onoise_spectrum"},
    {"hb", "hb 1meg 7\nprint mag(v(out))"},
    {"stb", "* netlist: Vprobe a b 0 in series in the loop, Iprobe 0 b 0 from ground to b\nstb Vprobe Iprobe dec 20 1 1g\nprint loopgain"},
    {"rfstab", "sp dec 50 100meg 10g\nrfstab"},
    {"meas", "tran 1u 3m\nmeas tran tr trig v(out) val=0.1 rise=1 targ v(out) val=0.9 rise=1\nmeas tran vavg avg v(out) from=1m to=2m\nac dec 10 1 1meg\nmeas ac gain max vdb(out)"},
    {"fft", "tran 1u 2m\nlinearize v(a)\nfft v(a)\nprint mag(v(a))"},
    {"fourier", "tran 1u 2m\nfourier 10k v(out)"},
    {"eye", "tran 10p 200n\neye v(rx) -ui 1n -threshold 0.5"},
    {"let", "let pwr = v(out)*i(vout)\nlet gain_db = vdb(out)\nprint vecmax(abs(v(out))) mean(pwr)"},
    {"print", "print col v(out) v(in)\nprint vmax > result.print"},
    {"wrdata", "wrdata out.dat v(out) v(in)\nwrdata -csv out.csv v(out) v(in)"},
    {"pyplot", "set pyplot_terminal=png\n* writes bode.png\npyplot bode vdb(out) xlog"},
    {"alter", "alter r1 = 2k\nalter @m1[w] = 20u\nrun"},
    {"altermod", "* nm: a .model card's name; every pair applies\naltermod nm vto=0.5 kp=150u\nop"},
    {"alterparam", "alterparam vsup=3.3\nreset\nrun"},
    {"show", "op\nshow m1 : gm id vgs"},
    {"osdi", "pre_osdi bsim4.osdi\n* reloaded after a rebuild\npre_osdi -f bsim4.osdi"},
    {"snp", "pre_snp filter.s2p\npre_osdi filter.osdi"},
    {"montecarlo", "* netlist: .param rr=agauss(1k, 50, 3), R1 in out {rr}, C1 out 0 159n\nmontecarlo 200 -seed 7 -analysis 'ac lin 1 1k 1k' -spec 'vdb(out)' -min -3.1 -max -2.9"},
    {"optimize", "* R1 of an RC low-pass for -3 dB at 10 kHz\noptimize -param r1 1k 100 10k -analysis 'ac lin 1 10k 10k' -target 'vdb(out)' -3 -method nm"},
    {"sweep", "sweep r1 1k 10k 1k -analysis 'op' -output 'v(out)'"},
    {"stop", "stop when v(out) > 0.9\ntran 1u 1m\nprint v(out)\nresume"},
    {"savestate", "tran 1n 1u\nsavestate ckpt.st\n* later, even in another process, with a .tran line in the netlist (its tstop the new end):\nloadstate ckpt.st"},
    {"foreach", "foreach f 1k 10k 100k\n  ac lin 1 $f $f\n  print vdb(out)\nend"},
    {"if", "if vecmax(v(out)) > 0.5\n  echo high\nelse\n  echo low\nend"},
    {"dowhile", "let k = 0\ndowhile k < 3\n  echo iteration $&k\n  let k = k + 1\nend"},
};

const char* const kHowInQucs =
    "In Qucs-S (ngspice chosen as its simulator), commands go into the .control block it writes, run in batch mode (no "
    "prompt, no window) and ended with exit. The simulation blocks write theirs (.AC ac, .TR tran, .DC op, .SW of a .DC dc, "
    ".NOISE, .PZ, .SENS, .DISTO, .FOURIER, .FFT, .SP; .NGOPT optimize, .NGSWEEP sweep, .NGMONTECARLO montecarlo, "
    ".NGCORNERS corners), each followed by destroy all and reset. A Nutmeg script block (type .CUSTOMSIM) holds any "
    "commands, in its SpiceCode property, with an analysis of its own; its Vars property ('v(out);v(in)') names the "
    "vectors put into the dataset, its Outputs property the files it writes itself (write custom#ac1#.plot k). A NutmegEq "
    "computes let expressions after an analysis; a .spiceinit block (SPICEINIT) holds set lines, SpiceOptions the options.";

QString text(const char* s)
{
    return QString::fromUtf8(s);
}

const Command* commandNamed(const QString& name)
{
    for (const Command& c : kCommands)
        if (name == QLatin1String(c.name)) return &c;
    return nullptr;
}

const Category* categoryOf(const Command& c)
{
    for (const Category& k : kCategories)
        if (qstrcmp(k.id, c.category) == 0) return &k;
    return nullptr;
}

template <typename Table>
QString entryOf(const Table& table, const char* name)
{
    for (const auto& e : table)
        if (qstrcmp(e.name, name) == 0) return text(e.text);
    return {};
}

// What the ngspice of the settings says of itself: its version and help
// all's lines, by command. Asked once for each program file (its path,
// time and size); a few milliseconds.
struct Installed {
    QString program;
    QString version;
    QHash<QString, QString> help;
    QString error;
};

QString ngspiceProgram()
{
    // As a simulation finds it (Ngspice's constructor): beside Qucs-S, or
    // through PATH.
    QString program = QucsSettings.NgspiceExecutable.trimmed();
    if (program.isEmpty()) return program;
    if (QFileInfo(program).isRelative()) {
        const QString beside = QFileInfo(QucsSettings.BinDir + program).absoluteFilePath();
        if (QFileInfo::exists(beside)) program = beside;
    }
    if (!QFileInfo(program).isAbsolute()) {
        const QString found = QStandardPaths::findExecutable(program);
        if (!found.isEmpty()) program = found;
    }
    return program;
}

const Installed& installed()
{
    static QString asked;
    static Installed known;
    const QString program = ngspiceProgram();
    const QFileInfo file(program);
    const QString key = QStringLiteral("%1|%2|%3").arg(program).arg(file.lastModified().toMSecsSinceEpoch()).arg(file.size());
    if (key == asked) return known;
    asked = key;
    known = Installed();
    known.program = program;
    if (program.isEmpty()) {
        known.error = tr("no ngspice is set in the settings");
        return known;
    }
    QProcess p;
    p.setWorkingDirectory(QDir::tempPath());
    p.start(program, {QStringLiteral("-b")});
    if (!p.waitForStarted(5000)) {
        known.error = tr("%1 could not be started").arg(QDir::toNativeSeparators(program));
        return known;
    }
    p.write("* commands\nR1 1 0 1\n.control\nversion\nhelp all\n.endc\n.end\n");
    p.closeWriteChannel();
    if (!p.waitForFinished(10000)) {
        p.kill();
        p.waitForFinished(1000);
        known.error = tr("%1 did not answer within 10 s").arg(QDir::toNativeSeparators(program));
        return known;
    }
    static const QRegularExpression version(QStringLiteral("^\\*\\* (ngspice-\\S+)"));
    static const QRegularExpression line(QStringLiteral("^([a-z_][a-z0-9_]*)( .*)? : "));
    for (QString l : QString::fromLocal8Bit(p.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        l = l.trimmed();
        if (const QRegularExpressionMatch m = version.match(l); m.hasMatch() && known.version.isEmpty()) known.version = m.captured(1);
        else if (const QRegularExpressionMatch n = line.match(l); n.hasMatch() && !known.help.contains(n.captured(1)))
            known.help.insert(n.captured(1), l);
    }
    if (known.help.isEmpty()) known.error = tr("%1 listed no commands (help all)").arg(QDir::toNativeSeparators(program));
    return known;
}

// Whether help all would list \a c: not a dot-card, not another spelling.
bool listed(const Command& c)
{
    return (c.flags & (DotCard | Alias)) == 0;
}

// The installed ngspice lacks \a c (when it could be asked).
bool lacking(const Installed& ng, const Command& c)
{
    if (!ng.error.isEmpty()) return false;
    if (listed(c)) return !ng.help.contains(QLatin1String(c.name));
    // A dot-card or a pre_ spelling of the enhanced build: there when a
    // command of that build is.
    if ((c.flags & Enhanced) != 0) return !ng.help.contains(QStringLiteral("hb")) && !ng.help.contains(QStringLiteral("snp"));
    return false;
}

// The line on the installed ngspice: which it is and what it lacks, or
// why it could not be asked.
QString installedText(const Installed& ng)
{
    QString s;
    if (QucsSettings.DefaultSimulator != spicecompat::simNgspice)
        s += tr("The simulator chosen in Qucs-S is not ngspice (set_simulator chooses it). ");
    if (!ng.error.isEmpty())
        return s + tr("The ngspice of the settings could not be asked (%1): the list is of the enhanced ngspice-46 build; "
                      "* marks what stock ngspice lacks.").arg(ng.error);
    int has = 0, of = 0;
    QStringList missing;
    for (const Command& c : kCommands) {
        if (!listed(c)) continue;
        ++of;
        if (ng.help.contains(QLatin1String(c.name))) ++has;
        else missing << QLatin1String(c.name);
    }
    s += tr("This ngspice (%1%2) has %3 of the %4 commands below").arg(QDir::toNativeSeparators(ng.program),
                                                                      ng.version.isEmpty() ? QString() : QStringLiteral(", ") + ng.version)
             .arg(has).arg(of);
    s += missing.isEmpty() ? QStringLiteral(".") : tr("; it lacks %1.").arg(missing.join(QStringLiteral(", ")));
    QStringList extra;
    for (auto it = ng.help.cbegin(); it != ng.help.cend(); ++it)
        if (commandNamed(it.key()) == nullptr) extra << it.key();
    std::sort(extra.begin(), extra.end());
    if (!extra.isEmpty()) s += tr(" It also has %1 (command gives its help line).").arg(extra.join(QStringLiteral(", ")));
    s += tr(" * marks what stock ngspice lacks.");
    return s;
}

// A command in a line: its name, * when stock ngspice lacks it, and what
// it does.
QString lineOf(const Installed& ng, const Command& c, bool syntax)
{
    QString s = QStringLiteral("  %1%2").arg(QLatin1String(c.name), (c.flags & Enhanced) != 0 ? QStringLiteral("*") : QString());
    if (lacking(ng, c)) s += tr(" [not in this ngspice]");
    if (syntax && *c.syntax != '\0') s += QLatin1Char(' ') + text(c.syntax);
    return s + QStringLiteral(": ") + text(c.summary);
}

// A command in full: syntax, what it does, what this ngspice says of it,
// how Qucs-S writes it, an example.
QString detailOf(const Installed& ng, const Command& c)
{
    QStringList about{text(categoryOf(c)->title)};
    if ((c.flags & DotCard) != 0) about << tr("a netlist dot-card, not a command");
    if ((c.flags & Enhanced) != 0) about << tr("the enhanced build's, not stock ngspice's");
    if (lacking(ng, c)) about << tr("not in this ngspice");
    QString s = QStringLiteral("%1 (%2)\n").arg(QLatin1String(c.name), about.join(QStringLiteral("; ")));
    // (A dot-card's syntax has its name already.)
    QString syntax = QLatin1String(c.name);
    if ((c.flags & DotCard) != 0) syntax = text(c.syntax);
    else if (*c.syntax != '\0') syntax += QLatin1Char(' ') + text(c.syntax);
    s += tr("Syntax: %1\n").arg(syntax);
    s += text(c.summary) + QLatin1Char('\n');
    if (const QString said = ng.help.value(QLatin1String(c.name)); !said.isEmpty()) s += tr("This ngspice's help: %1\n").arg(said);
    if (const QString q = entryOf(kInQucs, c.name); !q.isEmpty()) s += tr("In Qucs-S: %1\n").arg(q);
    if (QString e = entryOf(kExamples, c.name); !e.isEmpty())
        s += tr("Example:\n%1\n").arg(QStringLiteral("  ") + e.replace(QLatin1Char('\n'), QStringLiteral("\n  ")));
    return s;
}

// A name as asked: lower case, a dot-card's dot off (.tran is tran) unless
// the dot-card is itself an entry, another spelling to its command.
QString normalName(QString name)
{
    name = name.trimmed().toLower();
    if (name.startsWith(QLatin1Char('.')) && commandNamed(name) == nullptr) name.remove(0, 1);
    static const QHash<QString, QString> spellings{{QStringLiteral("measure"), QStringLiteral("meas")},
                                                   {QStringLiteral("four"), QStringLiteral("fourier")},
                                                   {QStringLiteral("opt"), QStringLiteral("option")}};
    return spellings.value(name, name);
}

// Names near \a name: those that begin with it or have it inside, then
// those a letter or two away.
QStringList nearNames(const QString& name, const Installed& ng)
{
    QStringList all;
    for (const Command& c : kCommands) all << QLatin1String(c.name);
    for (auto it = ng.help.cbegin(); it != ng.help.cend(); ++it)
        if (!all.contains(it.key())) all << it.key();
    // (The edits that make one of the other.)
    const auto distance = [](const QString& a, const QString& b) {
        QList<QList<int>> d(a.size() + 1, QList<int>(b.size() + 1));
        for (int i = 0; i <= a.size(); ++i) d[i][0] = i;
        for (int j = 0; j <= b.size(); ++j) d[0][j] = j;
        for (int i = 1; i <= a.size(); ++i)
            for (int j = 1; j <= b.size(); ++j)
                d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        return d[a.size()][b.size()];
    };
    QStringList near;
    for (const QString& n : all)
        if (name.size() >= 2 && (n.startsWith(name) || n.contains(name))) near << n;
    for (const QString& n : all)
        if (!near.contains(n) && distance(name, n) <= (name.size() > 3 ? 2 : 1)) near << n;
    return near.mid(0, 8);
}

} // namespace

QJsonObject QucsControl::ngspiceCommands(const QJsonObject& args)
{
    const Installed& ng = installed();
    QStringList names;
    const QJsonValue command = args.value(QLatin1String("command"));
    if (command.isString()) names << command.toString();
    else if (command.isArray())
        for (const QJsonValue& v : command.toArray()) {
            if (!v.isString()) return errorResult(tr("'command' is a command's name, or a list of names."));
            names << v.toString();
        }
    else if (!command.isUndefined() && !command.isNull())
        return errorResult(tr("'command' is a command's name, or a list of names."));
    names.removeIf([](const QString& n) { return n.trimmed().isEmpty(); });
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    const QString category = args.value(QLatin1String("category")).toString().trimmed().toLower();

    // Commands by name.
    if (!names.isEmpty()) {
        QStringList parts;
        for (const QString& asked : std::as_const(names)) {
            QString name = normalName(asked);
            QString before;
            if (commandNamed(name) == nullptr && !ng.help.contains(name) && name.startsWith(QLatin1String("pre_"))) {
                before = tr("pre_ runs %1 before the circuit is read (in a deck's .control block).\n").arg(name.mid(4));
                name = name.mid(4);
            }
            if (const Command* c = commandNamed(name)) parts << before + detailOf(ng, *c);
            else if (ng.help.contains(name))
                parts << before + tr("%1 (this ngspice's; not described here)\nThis ngspice's help: %2\n").arg(name, ng.help.value(name));
            else {
                const QStringList near = nearNames(name, ng);
                QString s = tr("ngspice has no command %1%2.").arg(asked.trimmed(),
                                                                  ng.error.isEmpty() ? tr(" (nor does this ngspice's help all list it)") : QString());
                if (!near.isEmpty()) s += tr(" Near it: %1.").arg(near.join(QStringLiteral(", ")));
                s += tr(" 'search' finds commands by what they do.\n");
                parts << s;
            }
        }
        return textResult(parts.join(QLatin1Char('\n')));
    }

    // Commands by what they do: every word somewhere in one's name, syntax,
    // summary, category's id (its title names what only some have),
    // Qucs-S note or help line; failing that, the most words.
    if (!search.isEmpty()) {
        const QStringList words = search.toLower().split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
        QList<std::pair<int, QString>> found;
        const auto consider = [&](const QString& haystack, const QString& line) {
            int n = 0;
            for (const QString& w : words) n += haystack.contains(w) ? 1 : 0;
            if (n > 0) found.append({n, line});
        };
        for (const Command& c : kCommands)
            consider(QStringList{QLatin1String(c.name), text(c.syntax), text(c.summary), QLatin1String(c.category),
                                 entryOf(kInQucs, c.name), ng.help.value(QLatin1String(c.name))}.join(QLatin1Char(' ')).toLower(),
                     lineOf(ng, c, false) + QStringLiteral(" [%1]").arg(QLatin1String(c.category)));
        for (auto it = ng.help.cbegin(); it != ng.help.cend(); ++it)
            if (commandNamed(it.key()) == nullptr) consider(it.value().toLower(), tr("  %1 (this ngspice's): %2").arg(it.key(), it.value()));
        const int best = found.isEmpty() ? 0 : std::max_element(found.cbegin(), found.cend())->first;
        QStringList lines;
        for (const auto& [n, line] : std::as_const(found))
            if (n == best) lines << line;
        if (lines.isEmpty())
            return textResult(tr("No ngspice command mentions %1. Its categories: %2 ('category' lists one's).")
                                  .arg(search, [] {
                                      QStringList ids;
                                      for (const Category& k : kCategories) ids << QLatin1String(k.id);
                                      return ids.join(QStringLiteral(", "));
                                  }()));
        QString head = best == words.size() ? tr("ngspice commands for \"%1\":\n").arg(search)
                                            : tr("No ngspice command mentions all of \"%1\"; those with %2 of its words:\n").arg(search).arg(best);
        return textResult(head + lines.join(QLatin1Char('\n')) + tr("\n('command' gives one's syntax, an example and how Qucs-S writes it.)"));
    }

    // A category, each command with its syntax.
    if (!category.isEmpty()) {
        const Category* k = nullptr;
        for (const Category& c : kCategories)
            if (category == QLatin1String(c.id) || category == QString::fromLatin1(c.title).toLower()) k = &c;
        if (k == nullptr) {
            QStringList ids;
            for (const Category& c : kCategories) ids << QLatin1String(c.id);
            return errorResult(tr("There is no category %1: they are %2.").arg(category, ids.join(QStringLiteral(", "))));
        }
        QString s = QStringLiteral("%1 - %2\n").arg(QLatin1String(k->id), text(k->title));
        if (*k->note != '\0') s += text(k->note) + QLatin1Char('\n');
        for (const Command& c : kCommands)
            if (qstrcmp(c.category, k->id) == 0) s += lineOf(ng, c, true) + QLatin1Char('\n');
        return textResult(s + installedText(ng));
    }

    // Everything: each command in a line, by category.
    QString s = tr("ngspice's commands, by category. 'command' gives one's syntax, an example and how Qucs-S writes it; "
                   "'search' finds commands by what they do; 'category' lists one category's with their syntax.\n");
    s += installedText(ng) + QLatin1Char('\n');
    s += text(kHowInQucs) + QLatin1Char('\n');
    for (const Category& k : kCategories) {
        s += QStringLiteral("\n%1 - %2\n").arg(QLatin1String(k.id), text(k.title));
        if (*k.note != '\0') s += QStringLiteral("  (") + text(k.note) + QStringLiteral(")\n");
        for (const Command& c : kCommands)
            if (qstrcmp(c.category, k.id) == 0) s += lineOf(ng, c, false) + QLatin1Char('\n');
    }
    return textResult(s);
}
