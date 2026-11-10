#!/usr/bin/env python3
"""breaks.py: RF 2.5D's Cutout (10 November) undone on purpose, one piece at a
time - the node in a model and a file of before it, a layer renamed in its
regions, its regions read (corners, polygons, circles, what is off, what
cannot be read), what it does to a layer's shapes, its place
first in the processing and in processRead(), what it says, and its menu, its
form, its status and its outline in the tab - each built and its tests run.
Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks the anchors and
writes nothing; BRK_ONLY=3,7 runs those alone; HUNT_RUNS=<dir> for logs;
BRK_BUILD=<dir> the build (the checkout's build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-cutout-breaks")

L = "rf25/rf25_layout.cpp"
M = "rf25/rf25_model.cpp"
P = "rf25panel.cpp"
D = "rf25doc.cpp"
T = "test_rf25_layout"
HAS = (T, "everyModelHasACutout")
READ = (T, "theCutoutIsReadFromItsRegions")
KEEP = (T, "theCutoutKeepsWhatIsInsideAndMasksTheRest")
FIRST = (T, "theProcessingCutsFirst")
FILE = (T, "aLayoutFileIsCutByTheModelsCutout")
DOC = ("test_rf25_doc", "aCutoutSolvesAPartOfTheLayout")

BREAKS = [
 # ---- The model
 ("a file of a build before it given no Cutout",
  [(M, "    child(m.layout(), QStringLiteral(\"rf_cutout\"));\n    m.normalise();", "    m.normalise();")], [HAS]),
 ("a layer renamed not renamed in the regions",
  [(M, "            for (QString& l : layers)\n                if (l == from) l = to;", "")], [HAS]),
 ("a polygon's corners shown for a rectangle",
  [(M, "                  \"shape=polygon\"),", "                  \"shape=rectangle\"),")], [DOC]),
 # ---- Its regions read
 ("the Cutout applied though turned off",
  [(L, "if (node == nullptr || !flagIn(*node, QStringLiteral(\"apply\"), true)) return out;", "if (node == nullptr) return out;")], [READ, FILE, DOC]),
 ("a disabled region applied",
  [(L, "        if (!r.enabled || r.type != QLatin1String(\"rf_region\")) continue;", "        if (r.type != QLatin1String(\"rf_region\")) continue;")], [READ]),
 ("a polygon's closing corner counted as a corner",
  [(L, "            while (region.outline.size() > 1 && region.outline.last() == region.outline.first()) region.outline.removeLast();\n", "")], [READ]),
 ("a circle of 32 sides, whatever its size",
  [(L, "std::clamp(int(std::ceil(M_PI / std::sqrt(2 * sagitta / radius))), 32, 1024)", "32")], [READ]),
 ("a circle of no radius used",
  [(L, "            if (readable && !(radius > 0)) {", "            if (false) {")], [READ]),
 ("a region of no area used",
  [(L, "        if (!(areaOf(region.outline) > 0)) {", "        if (false) {")], [READ]),
 ("a region on no layer of the stack used",
  [(L, "        if (!named) continue;\n", "")], [READ]),
 # ---- What it does to the shapes
 ("a region's layers not heeded",
  [(L, "        if (!r.cuts(layer)) continue;\n", "")], [KEEP]),
 ("what a region takes out left in",
  [(L, "            if (ok) part = left;", "            if (false) part = left;")], [KEEP]),
 ("a shape kept whole handed on as the boolean redrew it",
  [(L, "        if (part.size() == 1 && std::abs(after - before) <= perimeterOf(s) / kScaling) {", "        if (false) {")], [KEEP]),
 # ---- In the processing
 ("the processing not cutting",
  [(L, "        if (!how.cutout.isEmpty()) {\n            int cut = 0, masked = 0;", "        if (false) {\n            int cut = 0, masked = 0;")], [FIRST, FILE, DOC]),
 ("processRead() not reading the Cutout",
  [(L, "    how.cutout = cutoutOf(model, &said);\n", "")], [FILE, DOC]),
 ("what the cutout did on a layer not said",
  [(L, "            if (cut > 0 || masked > 0)\n                out.report.lines", "            if (false)\n                out.report.lines")], [FIRST]),
 ("nothing left not said to be the cutout's doing",
  [(L, "        out.problems << (out.report.masked > 0", "        out.problems << (false")], [FIRST]),
 # ---- The panel and the tab
 ("the Cutout's Add Region under an Add of one item",
  [(P, " || n->type == QLatin1String(\"rf_cutout\");\n        QMenu* add", ";\n        QMenu* add")], [DOC]),
 ("the Cutout's form without its button",
  [(P, "    if (n->type == QLatin1String(\"rf_cutout\")) {\n        auto* row", "    if (false) {\n        auto* row")], [DOC]),
 ("the Cutout's status not said",
  [(P, "                                                          : tr(\"%1 region(s)\").arg(regions);", "                                                          : QString();")], [DOC]),
 ("a region's layers with no picker",
  [(P, "        if (key == QLatin1String(\"layers\") && node.type == QLatin1String(\"rf_region\")) {", "        if (false) {")], [DOC]),
 ("the regions not drawn",
  [(D, "        for (const CutRegion& r : cutout.regions) (r.keep ? kept : taken).shapes << r.outline;", "")], [DOC]),
]


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is None:
            continue
        if p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))


def main():
    os.makedirs(os.path.join(RUNS, "cache"), exist_ok=True)
    if os.environ.get("BRK_ONLY_CHECK"):
        for name, edits, tests in BREAKS:
            apply(edits, None)
        print("%d breaks, every anchor once" % len(BREAKS))
        return 0
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        if which and i not in which:
            continue
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            log = os.path.join(RUNS, "brk%03d.log" % i)
            suites = sorted({t[0] for t in tests})
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    home = os.path.join(RUNS, "home")
                    os.makedirs(home, exist_ok=True)
                    env = dict(os.environ, HOME=home, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, timeout=900, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "BUILD FAILED" in said or "hung" in said or "caught)" not in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
