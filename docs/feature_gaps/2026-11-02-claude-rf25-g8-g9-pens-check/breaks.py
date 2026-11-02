#!/usr/bin/env python3
"""breaks.py: the check of G8, G9 and the pens of 2 November undone on
purpose, one piece at a time - a port's line and discontinuity said not to
be trusted where its strips do not agree on one line at the top (the
de-embedding's own lines, the calibration's, rf25_solve's answer, the tab's
account), and send_input's layout and slides (their points in what is drawn
on, brought into view, a drag through points, the keys, what the pen drew
said in the answer, what is refused) - each built and its tests run.
Sources are put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/.claude/worktrees/pen-markers/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/g8-g9-pens-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

DE, RD, QR, QI, QC = "rf25/rf25_deembed.cpp", "rf25doc.cpp", "qucscontrol_rf25.cpp", "qucscontrol_input.cpp", "qucscontrol.cpp"
TD, TC, TL, TP = "test_rf25_deembed", "test_qucs_control", "test_layout_doc", "test_pptx_doc"
TOP = "whatItsStripsDoNotAgreeOnAtTheTopIsNotToBeTrusted"
THIRD = "aThirdStripSaysWhereTheStripsAreOneLine"
TOOL = "anRf25ModelIsSolvedFullWaveThroughItsTool"
RAW = "rawInputIsSent"
LAYOUT = "theRawInputDrawsOnALayout"
SLIDE = "theRawInputDrawsOnASlide"

BREAKS = [
 # ---- G8: a port's line and discontinuity, where its strips are not one line at the top
 ("whether its strips agree at the top not kept",
  [(DE, "said.topAgrees = cal.residual.value(i) <= kStripsAgree;", "said.topAgrees = true;")],
  [(TD, TOP), (TC, TOOL)]),
 ("where they first do not agree not kept",
  [(DE, "if (cal.residual.value(j) > kStripsAgree && (said.offFrom == 0", "if (false && (said.offFrom == 0")],
  [(TD, TOP)]),
 ("the top taken as not agreeing wherever any frequency does not",
  [(DE, "said.topAgrees = cal.residual.value(i) <= kStripsAgree;", "said.topAgrees = said.offFrom == 0;")],
  [(TD, TOP)]),
 ("the warning only where the top does not agree",
  [(DE, "        if (said.offFrom > 0)\n            out.warnings", "        if (said.offFrom > 0 && !said.topAgrees)\n            out.warnings")],
  [(TD, TOP)]),
 ("the de-embedding's line of a port not marked",
  [(DE, "                             .arg(plane, said.shift == 0 ? QString() : QStringLiteral(\" \") + line, gaining)\n                             .arg(untrusted);",
    "                             .arg(plane, said.shift == 0 ? QString() : QStringLiteral(\" \") + line, gaining)\n                             .arg(QString());")],
  [(TD, TOP)]),
 ("the line said not to be trusted where the plane is at the face, where no line is",
  [(DE, ".arg(said.givenLine || said.shift == 0 ? tr(\"its own discontinuity\")", ".arg(said.givenLine ? tr(\"its own discontinuity\")")],
  [(TD, TOP)]),
 ("the calibration's own line not marked",
  [(DE, ".arg(out.residual.at(last) > kStripsAgree ? tr(\" - neither to be trusted: the strips do not agree on one line there\")",
    ".arg(false ? tr(\" - neither to be trusted: the strips do not agree on one line there\")")],
  [(TD, THIRD)]),
 ("the calibration's line marked where its strips agree",
  [(DE, ".arg(out.residual.at(last) > kStripsAgree ? tr(\" - neither to be trusted: the strips do not agree on one line there\")",
    ".arg(true ? tr(\" - neither to be trusted: the strips do not agree on one line there\")")],
  [(TD, THIRD)]),
 ("the answer not saying where its line was taken",
  [(QR, "                de.insert(QStringLiteral(\"taken at\"), top);\n", "")],
  [(TC, TOOL)]),
 ("the answer's port not marked",
  [(QR, "                if (!d.topAgrees)\n                    de.insert(QStringLiteral(\"not to be trusted\"),",
    "                if (false)\n                    de.insert(QStringLiteral(\"not to be trusted\"),")],
  [(TC, TOOL)]),
 ("the answer's port marked where its strips agree",
  [(QR, "                if (!d.topAgrees)\n                    de.insert(QStringLiteral(\"not to be trusted\"),",
    "                if (true)\n                    de.insert(QStringLiteral(\"not to be trusted\"),")],
  [(TC, TOOL)]),
 ("the tab's account of the solve not marked",
  [(RD, "                       .arg(w.frequencies.at(last) / 1e9, 0, 'g', 4)\n                       .arg(untrusted);",
    "                       .arg(w.frequencies.at(last) / 1e9, 0, 'g', 4)\n                       .arg(QString());")],
  [(TC, TOOL)]),
 # ---- send_input on a layout and on slides
 ("a layout's points taken as its picture's pixels",
  [(QI, "            if (layoutView) return layoutView->toPixel(p).toPoint();", "            if (layoutView) return p.toPoint();")],
  [(TL, LAYOUT)]),
 ("a slide's points taken from the first slide's top left",
  [(QI, "            return slideView->toViewport(slide, p).toPoint();", "            return slideView->toViewport(0, p).toPoint();")],
  [(TP, SLIDE)]),
 ("a layout's points out of sight not brought into view",
  [(QI, "            else if (layoutView) layoutView->setView(middle, layoutView->scale());", "            else if (layoutView) (void)middle;")],
  [(TL, LAYOUT)]),
 ("slides not scrolled to the slide asked",
  [(QI, "            else slideView->centerOn(slide, middle);", "            else (void)middle;")],
  [(TP, SLIDE)]),
 ("a drag through points not through them",
  [(QI, "const QPoint from = local.at(k - 1), next = local.at(k);", "const QPoint from = local.first(), next = local.last();")],
  [(TL, LAYOUT), (TP, SLIDE)]),
 ("slides laid out after their points are found",
  [(QI, "        root = slideView->viewport();\n        laidOut(root);", "        root = slideView->viewport();")],
  [(TP, SLIDE)]),
 ("what the pen drew not in the answer",
  [(QI, "                if (!drawn.isEmpty()) o.insert(QStringLiteral(\"drawn\"), drawn);\n", "")],
  [(TL, LAYOUT), (TP, SLIDE)]),
 ("every stroke said drawn, the earlier ones too",
  [(QI, "if (r.value(QLatin1String(\"stroke\")).toInt() > drawnBefore.value(part)) drawn.append(r);", "drawn.append(r);")],
  [(TL, LAYOUT)]),
 ("strokes taken away not said",
  [(QI, "                if (drawing.strokes().size() < strokesBefore)\n", "                if (false)\n")],
  [(TL, LAYOUT), (TP, SLIDE)]),
 ("a slide past the last not refused",
  [(QI, "if (!v.isDouble() || n != std::floor(n) || n < 1 || n > count) {", "if (!v.isDouble() || n != std::floor(n) || n < 1) {")],
  [(TP, SLIDE)]),
 ("'drag_to' and 'drag_through' both taken",
  [(QI, "    if (through && args.contains(QLatin1String(\"drag_to\"))) {", "    if (false) {")],
  [(TP, SLIDE)]),
 ("a point of 'drag_through' that is none taken",
  [(QI, "            good = good && xyOf(v, &p);", "            xyOf(v, &p);")],
  [(TL, LAYOUT)]),
 ("what is asked about a drag through points not saying so",
  [(QC, "                              : a.contains(QLatin1String(\"drag_through\")) ? tr(\", dragged through %1 point(s)\").arg(a.value(QLatin1String(\"drag_through\")).toArray().size())\n",
    "                              : false ? QString()\n")],
  [(TC, RAW)]),
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
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        if which and i not in which:
            continue
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
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=CACHE,
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
        print("%-66s %s" % (name[:66], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
