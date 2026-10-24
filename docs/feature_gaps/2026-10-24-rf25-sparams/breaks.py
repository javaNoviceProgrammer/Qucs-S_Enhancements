#!/usr/bin/env python3
"""breaks.py: the crossing from a quasi-static extraction to a circuit -
the loss and vacuum matrices kept whole, the bridge into the engine's own
line and coupled lines, the sweep, the quasi-static limit, and the
Touchstone written of them - undone on purpose. Sources are put back
whatever happens. BRK_ONLY_CHECK=1 checks the anchors; HUNT_RUNS=<dir>."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-sparams-breaks")
os.makedirs(RUNS, exist_ok=True)

P, S, R = "rf25/rf25_sparams.cpp", "rf25/rf25_static.cpp", "multiphysics/fem_results.cpp"
D, T = "rf25doc.cpp", "qucscontrol_rf25.cpp"
SP, ST, DOC, CTL = "test_rf25_sparams", "test_rf25_static", "test_rf25_doc", "test_qucs_control"
LINE = "aNetIsTheEnginesOwnLine"
MATCH = "aMatchedLineReflectsNothingAndDelaysByBetaL"
LOSSY = "aLossyLinesTransmissionIsItsAttenuation"
SWEEP = "aSweepIsOneSolveAndManyFrequencies"
MODES = "twoLinesAreCoupledLinesWithTheirModes"
GAP = "theCouplingFallsAsTheLinesAreMovedApart"
LOSSM = "theLossMatrixCouplesAsWellAsLoads"
VACUUM = "theVacuumMatrixIsKeptWhole"
LIMIT = "theQuasiStaticLimitIsSaidRatherThanAssumed"
WRITE = "theEnginesWritersTakeAnRf25Answer"
REFUSE = "whatCannotBeMadeIntoLinesIsRefused"
STUDY = "theStudysOwnFrequenciesAreRead"
WROTE = "theSolvedModelIsWrittenAsATouchstone"
TOOL = "anRf25ModelIsSolvedThroughItsTool"

BREAKS = [
 # ---- the matrices kept whole rather than reduced
 ("the loss of a layer is taken as one number a net, losing how it couples two",
  [(S, "                    out.dielectricLoss[i][j] += tand * was * dCdEps;",
    "                    if (i == j) out.dielectricLoss[i][j] += tand * was * dCdEps;")],
  [(SP, LOSSM)]),

 ("a layer's loss is counted whatever share of the field it holds",
  [(S, "                    const double dCdEps = (nudgedMaxwell.at(i).at(j) - out.maxwell.at(i).at(j)) / step;\n                    out.dielectricLoss[i][j] += tand * was * dCdEps;",
    "                    out.dielectricLoss[i][j] += tand * out.maxwell.at(i).at(j);")],
  [(SP, LOSSM), (ST, "aLinesResistanceAndConductanceAreThoseOfItsLosses")]),

 ("a net's own loss tangent is its row of the loss matrix over nothing in particular",
  [(S, "        out.lossTangent << (here != 0 ? lost / here : 0.0);", "        out.lossTangent << (here != 0 ? lost : 0.0);")],
  [(SP, LOSSM)]),

 ("the vacuum matrix is the stack's own",
  [(S, "    solve(fromVacuum, &out.maxwell0);", "    solve(fromStack, &out.maxwell0);")],
  [(SP, VACUUM), (ST, "aMicrostripsImpedanceIsHammerstadAndJensens")]),

 # ---- the bridge into one line
 # (LOSSY cannot see this one: it takes alpha from the same broken line
 #  it measures, so a crossing is self-consistent there.)
 ("a line's R and G cross over as each other",
  [(P, "    out.R = lp.r;\n    out.L = lp.l;\n    out.G = lp.g;\n    out.C = lp.c;",
    "    out.R = lp.g;\n    out.L = lp.l;\n    out.G = lp.r;\n    out.C = lp.c;")],
  [(SP, LINE)]),

 ("the propagation does not cross over at all",
  [(P, "    out.Zc = lp.zc;\n    out.gamma = lp.gamma;", "    out.Zc = lp.zc;")],
  [(SP, LINE), (SP, MATCH)]),

 ("a refused line is answered rather than refused",
  [(P, "    if (!lp.ok()) {\n        out.error = lp.problems.join(QStringLiteral(\" \"));\n        return out;\n    }",
    "    if (false) return out;")],
  [(SP, LINE), (SP, REFUSE)]),

 ("the effective permittivity is the quasi-static one however much beta says otherwise",
  [(P, "    if (k0 > 0 && lp.gamma.imag() > 0) {", "    if (false) {")],
  [(SP, LINE)]),

 # ---- the bridge into coupled lines
 ("the capacitance matrix is not per metre",
  [(P, "                r[std::size_t(i)][std::size_t(j)] = scale * 0.5 * (m.at(i).at(j) + m.at(j).at(i)) / length;",
    "                r[std::size_t(i)][std::size_t(j)] = scale * 0.5 * (m.at(i).at(j) + m.at(j).at(i));")],
  [(SP, MODES)]),

 ("the matrices are taken as they come, asymmetry and all",
  [(P, "                r[std::size_t(i)][std::size_t(j)] = scale * 0.5 * (m.at(i).at(j) + m.at(j).at(i)) / length;",
    "                r[std::size_t(i)][std::size_t(j)] = scale * m.at(i).at(j) / length;")],
  [(SP, MODES)]),

 ("the inductance matrix is the vacuum capacitance rather than its inverse",
  [(P, "    const Eigen::MatrixXd l = lu.inverse() / (kLightSpeed * kLightSpeed);",
    "    const Eigen::MatrixXd l = C0m / (kLightSpeed * kLightSpeed);")],
  [(SP, MODES)]),

 ("the inductance leaves out the speed of light",
  [(P, "    const Eigen::MatrixXd l = lu.inverse() / (kLightSpeed * kLightSpeed);", "    const Eigen::MatrixXd l = lu.inverse();")],
  [(SP, MODES)]),

 ("the conductance matrix is not w times the loss",
  [(P, "    out.G = out.rlgc ? perMetre(extraction.dielectricLoss, omega) : fem::CoupledLines::Matrix(N, std::vector<double>(N, 0));",
    "    out.G = out.rlgc ? perMetre(extraction.dielectricLoss, 1) : fem::CoupledLines::Matrix(N, std::vector<double>(N, 0));")],
  [(SP, LOSSM)]),

 ("each net's resistance is the surface resistance alone, the crowding and the length dropped",
  [(P, "            out.R[std::size_t(i)][std::size_t(i)] = zs.real() * length * extraction.crowding.value(i);",
    "            out.R[std::size_t(i)][std::size_t(i)] = zs.real();")],
  [(SP, MODES)]),

 # ---- the one mode solver, shared with the engine
 ("the mode solver takes matrices of any shape at all",
  [(R, "    if (n < 1 || !square(out.R) || !square(out.L) || !square(out.G) || !square(out.C) || !square(out.C0)) {",
    "    if (false) {")],
  [(SP, REFUSE)]),

 ("the mode solver takes a frequency of none",
  [(R, "    if (!(w > 0)) {\n        out.error = tr(\"a line's modes want a frequency\");\n        return false;\n    }",
    "    if (false) return false;")],
  [(SP, REFUSE)]),

 ("the modes of a run before are kept and added to",
  [(R, "    out.modes.clear();\n    Eigen::MatrixXd C(n, n), C0m(n, n);", "    Eigen::MatrixXd C(n, n), C0m(n, n);")],
  [(SP, REFUSE)]),

 ("a lossless line's modes are asked for at no frequency",
  [(P, "    fem::coupledModes(out, out.rlgc ? omega : 2 * M_PI * 1e9);", "    fem::coupledModes(out, omega);")],
  [(SP, MODES)]),

 # ---- the sweep
 ("a logarithmic sweep is spaced linearly",
  [(P, "        out << (logarithmic ? from * std::pow(to / from, t) : from + t * (to - from));",
    "        out << (from + t * (to - from));")],
  [(SP, SWEEP)]),

 # (WROTE counts the rows, which this does not change: SWEEP holds the
 #  last point to where it was asked to end.)
 ("a sweep's last point is not where it was asked to end",
  [(P, "        const double t = double(i) / double(points - 1);", "        const double t = double(i) / double(points);")],
  [(SP, SWEEP)]),

 ("a sweep that ends below where it starts is swept anyway",
  [(P, "    if (to < from) return refuse(tr(\"A sweep ends above where it starts: %1 Hz to %2 Hz.\")",
    "    if (false) return refuse(tr(\"A sweep ends above where it starts: %1 Hz to %2 Hz.\")")],
  [(SP, SWEEP)]),

 ("a logarithmic sweep is let start at nothing",
  [(P, "    if (logarithmic && !(from > 0))", "    if (false)")],
  [(SP, SWEEP)]),

 ("a sweep of no points is swept",
  [(P, "    if (points < 1) return refuse(tr(\"A sweep wants at least one point.\"));", "    if (false) return refuse(tr(\"A sweep wants at least one point.\"));")],
  [(SP, SWEEP)]),

 ("one frequency of a sweep being none is passed on to the S-parameters",
  [(P, "        if (!(f > 0)) {\n            out.error = tr(\"a line's S-parameters want a frequency above nothing: at none it has no impedance and no \"\n                           \"propagation, only the capacitance it was solved for\");\n            return out;\n        }",
    "        if (false) return out;")],
  [(SP, SWEEP)]),

 ("the study's own spacing is ignored",
  [(P, "    const bool logarithmic = textOf(*study, QStringLiteral(\"scale\")) == QLatin1String(\"log\");",
    "    const bool logarithmic = false;")],
  [(SP, STUDY)]),

 ("a study written from high to low is swept downwards",
  [(P, "    return sweepFrequencies(std::min(from, to), std::max(from, to), int(std::lround(points)), logarithmic, problem);",
    "    return sweepFrequencies(from, to, int(std::lround(points)), logarithmic, problem);")],
  [(SP, STUDY)]),

 ("a study's frequency that is no number is taken as nothing and swept",
  [(P, "    if (!why.isEmpty()) {\n        if (problem != nullptr) *problem = why;\n        return {};\n    }",
    "    if (false) return {};")],
  [(SP, STUDY)]),

 ("a property that is not set and one that is nonsense are answered alike",
  [(P, "    if (!e.isValid() || !e.isConstant()) {\n        if (ok != nullptr) *ok = false;\n        return fallback;\n    }",
    "    if (!e.isValid() || !e.isConstant()) return fallback;")],
  [(SP, STUDY)]),

 # ---- the quasi-static limit
 ("the limit is of the length rather than the cross-section",
  [(P, "        wide = std::min(box.width(), box.height()) * micrometre;", "        wide = std::max(box.width(), box.height()) * micrometre;")],
  [(SP, LIMIT)]),

 ("the limit takes the whole wavelength rather than a tenth of it",
  [(P, "    if (out.transverse > 0) out.frequency = kLightSpeed / (10.0 * out.transverse * std::sqrt(out.epsilonMax));",
    "    if (out.transverse > 0) out.frequency = kLightSpeed / (out.transverse * std::sqrt(out.epsilonMax));")],
  [(SP, LIMIT)]),

 ("the limit is of free space, whatever the stack is made of",
  [(P, "    if (out.transverse > 0) out.frequency = kLightSpeed / (10.0 * out.transverse * std::sqrt(out.epsilonMax));",
    "    if (out.transverse > 0) out.frequency = kLightSpeed / (10.0 * out.transverse);")],
  [(SP, LIMIT)]),

 ("the deeper of the two dimensions is not taken",
  [(P, "    if (wide > deep) {", "    if (true) {")],
  [(SP, LIMIT)]),

 ("the resonance is of the whole wave rather than the half",
  [(P, "    if (velocity > 0 && length > 0) out.resonance = velocity / (2.0 * length);",
    "    if (velocity > 0 && length > 0) out.resonance = velocity / length;")],
  [(SP, LIMIT)]),

 # ---- what is written out of it
 ("an export writes the first net whatever one it was told",
  [(D, "        index = int(a_extraction->names.indexOf(what.net));", "        index = 0;")],
  [(DOC, WROTE), (CTL, TOOL)]),

 ("an export of a model that is not solved writes one anyway",
  [(D, "    if (!a_extraction || !a_extraction->ok())\n        return refuse(a_solveProblem.isEmpty() ? tr(\"Nothing is solved yet: Solve the model first.\") : a_solveProblem);",
    "    if (false) return refuse(QString());")],
  [(DOC, WROTE)]),

 ("an export's length falls back to nothing rather than to what was solved",
  [(D, "    const double length = what.length > 0 ? what.length : a_solvedLength;", "    const double length = what.length;")],
  [(DOC, WROTE)]),

 ("an export's reference impedance is 50 whatever it says",
  [(D, "        return fem::touchstoneCoupled(cs, length, what.impedance, false, comments);",
    "        return fem::touchstoneCoupled(cs, length, 50.0, false, comments);"),
   (D, "    return fem::touchstone(ls, length, what.impedance, comments);", "    return fem::touchstone(ls, length, 50.0, comments);")],
  [(CTL, TOOL)]),

 ("the file carries no word of what made it or where it stops being right",
  [(D, "    comments << tr(\"%1, an RF 2.5D model of Qucs-S\").arg(QFileInfo(a_DocName).fileName());",
    "    comments.clear();")],
  [(DOC, WROTE), (CTL, TOOL)]),

 ("an export node's own properties are not read",
  [(D, "    out.coupled = textOf(*node, QStringLiteral(\"of\")) == QLatin1String(\"coupled\");\n    out.net = textOf(*node, QStringLiteral(\"net\")).trimmed();",
    "    out.coupled = false;")],
  [(DOC, WROTE)]),

 ("the tab's Write button writes nothing",
  [("rf25panel.cpp", "            const QString path = a_doc->writeExport(tag, &why);", "            const QString path = a_doc->exportPath(tag);")],
  [(DOC, WROTE)]),

 ("the Results node says nothing of the S parameters",
  [("rf25panel.cpp", "            did += a_doc->sParameterSummary();", "")],
  [(DOC, WROTE)]),

 ("the tab says nothing of where the quasi-static answer gives out",
  [(D, '        out << tr("Quasi-static to about %1 GHz (%2, eps_r %3): above that a mode other than quasi-TEM appears and "\n                  "none of this is the whole story.")',
    '        out << tr("%1 %2 %3")')],
  [(DOC, WROTE)]),

 # ---- the tools
 ("the tool answers no S parameters at all",
  [(T, "                if (rlgc.error.isEmpty() && std::abs(rlgc.Zc) > 0) {", "                if (false) {")],
  [(CTL, TOOL)]),

 ("the tool's S parameters are into 50 Ohm whatever was asked for",
  [(T, "                    const std::array<std::complex<double>, 4> sp = fem::lineSParameters(rlgc, length, zref);",
    "                    const std::array<std::complex<double>, 4> sp = fem::lineSParameters(rlgc, length, 50.0);")],
  [(CTL, TOOL)]),

 ("the tool says nothing of the quasi-static limit",
  [(T, '        out.insert(QStringLiteral("quasi-static limit"), said);', "")],
  [(CTL, TOOL)]),

 ("the tool writes an export of a model that is not solved",
  [(T, "    if (!model->extraction() || !model->extraction()->ok())", "    if (false)")],
  [(CTL, TOOL)]),

 ("the tool takes an 'of' that is neither",
  [(T, "        if (of != QLatin1String(\"line\") && of != QLatin1String(\"coupled\"))", "        if (false)")],
  [(CTL, TOOL)]),

 ("the tool ignores the file it was told to write",
  [(T, "    QString where = args.value(QLatin1String(\"file\")).toString().trimmed();", "    QString where;")],
  [(CTL, TOOL)]),

 ("the tool counts the ports of a coupled export as two",
  [(T, "    const int ports = what.coupled ? 2 * int(model->extraction()->names.size()) : 2;\n    out.insert(QStringLiteral(\"ports\"), ports);",
    "    const int ports = 2;\n    out.insert(QStringLiteral(\"ports\"), ports);")],
  [(CTL, TOOL)]),
]

def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is not None and p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))

def main():
    only = os.environ.get("BRK_ONLY_CHECK")
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            if only:
                results.append((name, "anchors ok"))
                continue
            log = os.path.join(RUNS, "brk%02d.log" % i)
            suites = sorted({s for s, _ in tests})
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out,
                                  stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=900,
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen",
                                                    ASAN_OPTIONS="detect_leaks=0"))
                        said.append("%s %s" % (fn[:30], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:30])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-70s %s" % (name[:70], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
