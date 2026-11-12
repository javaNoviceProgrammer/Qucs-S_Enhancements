#!/usr/bin/env python3
"""breaks.py: RF 2.5D's own shapes (Layout > Shapes, 12 November) undone on
purpose, one piece at a time - their outlines, their place in the processing,
a model of them alone, how the tab draws, fits and chooses them, and how the
Draw tools and a call draw them - each built and its tests run. Sources are
put back whatever happens, and the suites built again after. BRK_ONLY_CHECK=1
checks the anchors and writes nothing; BRK_ONLY=3 runs that alone;
HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir> the build (the checkout's
build-rel). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build-rel")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-own-shapes-breaks")

L = "rf25/rf25_layout.cpp"
M = "rf25/rf25_model.cpp"
T = "rf25doc.cpp"
D = "rf25doc_draw.cpp"
UNIT = ("test_rf25_layout", "theModelsOwnShapesAreItsParameters")
TAB = ("test_rf25_doc", "aModelsOwnShapesAreDrawnSolvedAndVaried")

BREAKS = [
 ("the model's own shapes not added to the file's in the processing",
  [(L, "        byLayer[s.layer] << s.outline;\n        ++own;", "        ++own;")], [UNIT, TAB]),
 ("a rectangle placed by its centre placed by its corner",
  [(L, "    const QPointF corner = textOf(n, QStringLiteral(\"anchor\")) == QLatin1String(\"centre\") ? at - QPointF(w / 2, h / 2) : at;",
       "    const QPointF corner = at;")], [UNIT]),
 ("a path's ends always flush",
  [(L, "            path.elements[0].end_type = ends == QLatin1String(\"round\")      ? gdstk::EndType::Round",
       "            path.elements[0].end_type = false                                ? gdstk::EndType::Round")], [UNIT]),
 ("a length that is no number read as nought, not refused",
  [(L, "                if (readable)\n                    say(tr(\"Shapes: %1's %2 is not a number (%3).\")",
       "                if (false)\n                    say(tr(\"Shapes: %1's %2 is not a number (%3).\")"),
   (L, "                readable = false;\n                return 0.0;\n            }\n            return e.constant() / 1e-6;\n        };\n        const auto lengthOf = [&](const char* key, const QString& what)",
       "                return 0.0;\n            }\n            return e.constant() / 1e-6;\n        };\n        const auto lengthOf = [&](const char* key, const QString& what)")],
  [UNIT]),
 ("a shape on a ground plane taken",
  [(L, "        if (layer->kind != Layer::Metal && layer->kind != Layer::Via && layer->kind != Layer::Dielectric && layer->kind != Layer::Substrate) {\n            say(tr(\"Shapes: %1 is on %2, which is no metal, via, dielectric or substrate",
       "        if (false) {\n            say(tr(\"Shapes: %1 is on %2, which is no metal, via, dielectric or substrate")], [UNIT]),
 ("a model of its own shapes alone still said to want a file",
  [(M, "    if (textOf(layout(), QStringLiteral(\"file\")).trimmed().isEmpty() && !shapes)",
       "    if (textOf(layout(), QStringLiteral(\"file\")).trimmed().isEmpty())")], [UNIT, TAB]),
 ("no empty layout under a model of its own shapes alone",
  [(T, "        if (alone && !a_layout) {\n            a_layout = ownShapesLayout();", "        if (false) {\n            a_layout = ownShapesLayout();")], [TAB]),
 ("the own shapes not drawn on the tab",
  [(T, "            overlay.name = tr(\"%1, the model's own shapes\").arg(it.key());\n            overlays << overlay;",
       "            overlay.name = tr(\"%1, the model's own shapes\").arg(it.key());")], [TAB]),
 ("Zoom Extents of a model of its own shapes alone fitting the empty layout",
  [(T, "    if (a_layout && a_layout == ownShapesLayout()) {\n        QRectF bounds;", "    if (false) {\n        QRectF bounds;")], [TAB]),
 ("a click on an own shape choosing nothing",
  [(D, "                if (hit != nullptr) {\n                    focusNode(hit->tag);", "                if (false) {\n                    focusNode(hit->tag);")], [TAB]),
 ("the Draw menu's choice not given to the tools",
  [(D, "    if (!a_drawParametric) return edits;", "    return edits;")], [TAB]),
 ("own shapes and the file's drawn in one call",
  [(D, "        if (own > 0 && own != edits.size())", "        if (false)")], [TAB]),
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
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn, "-platform", "offscreen"], stdout=out,
                                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=1200, env=env, cwd=RUNS)
                        said.append("%s %s" % (fn[:34], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    # The suites built again from the sources as they are: a break's build
    # left behind fails a later run for nothing.
    subprocess.run(["ninja", "-C", B] + sorted({t[0] for _, _, tests in BREAKS for t in tests}),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
