#!/usr/bin/env python3
"""breaks.py: each fix of the N round undone on purpose, to see its test
fail. One edit (or a few) per break, the named targets built, the named
tests run; every one of them must fail. The sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors alone, building nothing.
HUNT_RUNS=<dir> says where the logs go."""
import os, re, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/hunt-n-breaks")
os.makedirs(RUNS, exist_ok=True)

# (file, old, new) edits; targets to build; "suite:function" tests
BREAKS = [
 ("N1 the plural of a level's name is an \"s\" added again",
  [("multiphysics/fem_geometry.cpp",
    'problems << tr("a rule for %1 is all, numbers, %2box or named")\n                        .arg(levelNames(level),',
    'problems << tr("a rule for %1s is all, numbers, %2box or named")\n                        .arg(levelName(level),')],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:theModelAnswersSayWhatEachNumberIs"]),

 ("N2 'reference' as text is a file's name again",
  [("qucscontrol_fem.cpp",
    "    bool number = false;\n    const double one = name.toDouble(&number);\n    if (number && std::isfinite(one)) {",
    "    bool number = false;\n    const double one = name.toDouble(&number);\n    if (false && number && std::isfinite(one)) {")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:theModelAnswersSayWhatEachNumberIs"]),

 ("N3 a value JSON has no number for is written as a null 'SI' again",
  [("qucscontrol_fem.cpp",
    "    into->insert(QStringLiteral(\"value\"), text);\n    if (std::isfinite(value)) {",
    "    into->insert(QStringLiteral(\"value\"), text);\n    if (true) {")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:theModelAnswersSayWhatEachNumberIs"]),

 ("N6 a timeout given as text is not read",
  [("qucscontrol.cpp",
    "    const auto given = [&t](int otherwise) {\n        if (t.isString()) {",
    "    const auto given = [&t](int otherwise) {\n        if (false && t.isString()) {")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:whatACallAskedForIsAccountedFor"]),

 ("N7 a field of one letter is a near miss again",
  [("qucscontrol.cpp",
    "            if (std::min(a.size(), b.size()) < 3 && std::abs(a.size() - b.size()) > 1) continue;",
    "            if (false) continue;")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:whatACallAskedForIsAccountedFor"]),

 ("N8 read_pdf refuses \"all\" again",
  [("qucscontrol_design.cpp",
    'else if (p.isString() && (word == QLatin1String("all") || word == QLatin1String("*"))) {',
    'else if (false) {')],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:whatACallAskedForIsAccountedFor"]),

 ("N8 a page that is not there is passed over again",
  [("qucscontrol_design.cpp",
    '    if (!notThere.isEmpty())\n        leftOut << tr("pages %1: %2 has pages 1 to %3")',
    '    if (false)\n        leftOut << tr("pages %1: %2 has pages 1 to %3")')],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:whatACallAskedForIsAccountedFor"]),

 ("N9 a place off the model plane is cut to its edge again",
  [("qucscontrol.cpp",
    '    if (!v.isDouble()) return {};   // (the tool itself says what it takes)',
    '    if (true) return {};   // (the tool itself says what it takes)')],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:everyPlaceOffTheSchematicIsRefused"]),

 ("N9 a part is moved off the plane again (edit_component alone)",
  [("qucscontrol.cpp",
    '    for (const QString& off : {offThePlane(args.value(QLatin1String("x")), "x"), offThePlane(args.value(QLatin1String("y")), "y")})\n        if (!off.isEmpty()) {\n            *why = off;\n            return nullptr;\n        }',
    '    for (const QString& off : {QString(), QString()})\n        if (!off.isEmpty()) {\n            *why = off;\n            return nullptr;\n        }')],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:everyPlaceOffTheSchematicIsRefused"]),

 ("N9 a diagram is drawn off the plane again",
  [("qucscontrol_results.cpp",
    '    for (const QString& why : {offThePlane(args.value(QLatin1String("x")), "x"), offThePlane(args.value(QLatin1String("y")), "y")})\n        if (!why.isEmpty()) {\n            *error = why;\n            return false;\n        }',
    '    for (const QString& why : {QString(), QString()})\n        if (!why.isEmpty()) {\n            *error = why;\n            return false;\n        }')],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:everyPlaceOffTheSchematicIsRefused"]),

 ("N11 'at' as a list is read as nothing given again",
  [("qucscontrol_results.cpp",
    "    if (at.isArray() || at.isObject()) {\n        // (fem_evaluate's 'at' is a list of places; a marker sits at one x",
    "    if (false) {\n        // (fem_evaluate's 'at' is a list of places; a marker sits at one x")],
  ["test_diagram_tools"], ["test_diagram_tools:loopDiagrams"]),

 ("N13 a count that does not match is passed over again",
  [("dataset.cpp",
    "        if (declared.at(i) < 0 || declared.at(i) == v.re.size()) continue;",
    "        if (true) continue;")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:aDatasetSaysWhenItsCountDoesNotMatch"]),

 ("N13 a dep's count is said even where it matches",
  [("dataset.cpp",
    "        if (declared.at(i) < 0 || declared.at(i) == v.re.size()) continue;",
    "        if (declared.at(i) < 0) continue;")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:aDatasetSaysWhenItsCountDoesNotMatch"]),
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
    for i, (name, edits, targets, tests) in enumerate(BREAKS, 1):
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            if only:
                results.append((name, "anchors ok"))
                continue
            log = os.path.join(RUNS, "brk%02d.log" % i)
            with open(log, "w") as out:
                build = subprocess.run(["ninja", "-C", B] + targets, stdout=out, stderr=subprocess.STDOUT)
                if build.returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                caught = []
                for t in tests:
                    suite, fn = t.split(":")
                    r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn],
                                       stdout=out, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0"))
                    caught.append("%s %s" % (t, "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                results.append((name, "; ".join(caught)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
