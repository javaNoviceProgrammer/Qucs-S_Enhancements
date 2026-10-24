#!/usr/bin/env python3
"""breaks.py: each fix of the CI-only round undone on purpose, to see its
test fail. The sources are put back whatever happens. BRK_ONLY_CHECK=1
checks the anchors alone. QUCS_SRC / QUCS_BUILD say where to work: they
default to the worktree this round was verified in, which matters - a
checkout stamps its files alike to the millisecond, which is the very
thing one of these breaks is about."""
import os, subprocess, sys

Q = os.environ.get("QUCS_SRC", "/private/tmp/claude-501/-Users-meisam-git-Qucs-S-Enhancements/"
                               "55c87f7b-e85e-45fa-b87a-60a166ea4350/scratchpad/iso/qucs-s-26.1.1/qucs") + "/"
B = os.environ.get("QUCS_BUILD", "/private/tmp/claude-501/-Users-meisam-git-Qucs-S-Enhancements/"
                                 "55c87f7b-e85e-45fa-b87a-60a166ea4350/scratchpad/iso-build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/ci-only-breaks")
os.makedirs(RUNS, exist_ok=True)

BREAKS = [
 ("the reload asks whether the file is newer, as it did",
  [("qucsdoc.cpp",
    "  if (onDisk != a_lastSaved) return true;",
    "  if (onDisk > a_lastSaved) return true;\n  if (true) return false;")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:aModelReplacedByAFileDatedNoLaterIsStillRead"]),

 ("the date alone decides, with no size behind it",
  [("qucsdoc.cpp",
    "  if (a_lastSize < 0 || a_DocName.isEmpty()) return false;",
    "  if (true) return false;")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:aModelReplacedByAFileDatedNoLaterIsStillRead",
                          "test_mcp_wishlist:aCallRightAfterAWriteSeesTheFile"]),

 ("a stamp forgets the size it was taken with",
  [("qucsdoc.cpp",
    "  a_lastSize = info.exists() ? info.size() : -1;",
    "  a_lastSize = -1;")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:aModelReplacedByAFileDatedNoLaterIsStillRead"]),

 # (The self-reload a clock stamp invites is a race of a millisecond - the
 # write and the clock reading usually land in the same one - so what the
 # test holds to is the stamp itself: a save leaves its file's size as well
 # as its date, and a file put there afterwards at that very date is still
 # told from it.)
 ("a save stamps itself from the clock again",
  [("multiphysicsdoc.cpp",
    "    stampedFromFile();\n    return 0;",
    "    a_lastSaved = QDateTime::currentDateTime();\n    a_lastSize = -1;\n    return 0;")],
  ["test_mcp_wishlist"], ["test_mcp_wishlist:aSaveIsNotReadBackAsAnotherProgramsWrite"]),

 # (A break that reads the box's middle again - the assertion CI's Linux
 # failed on - cannot fail here: whether the caption's glyphs cover the
 # middle pixel is the host font's to decide, and Helvetica leaves it clear
 # on this machine. That is the whole reason the test no longer reads it,
 # and why the break below asks instead that the points read are the box's.)
 ("the box is read outside itself, where the slide is plain paper",
  [("tests/test_pptx_doc.cpp",
    "        // And the box for it is light whatever font its caption is drawn in.\n"
    "        for (const QPointF& inside : {QPointF(565, 62), QPointF(735, 62), QPointF(565, 138), QPointF(735, 138)}) {",
    "        // And the box for it is light whatever font its caption is drawn in.\n"
    "        for (const QPointF& inside : {QPointF(80, 300), QPointF(900, 400)}) {")],
  ["test_pptx_doc"], ["test_pptx_doc:whatTheDeckLacksAndWhatThisMachineLacks"]),

 ("what the deck lacks is not told from what the machine lacks",
  [("tests/test_pptx_doc.cpp",
    "        if (!said.contains(QStringLiteral(\"not installed\"))) kept << said;",
    "        kept << said;")],
  # (picturesDrawn's deck has no text at all, so no font note joins its
  # list and the filter has nothing to do there: this break is the new
  # test's to catch.)
  ["test_pptx_doc"], ["test_pptx_doc:whatTheDeckLacksAndWhatThisMachineLacks"]),
]

def main():
    only = os.environ.get("BRK_ONLY_CHECK")
    results = []
    for i, (name, edits, targets, tests) in enumerate(BREAKS, 1):
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            for path, old, new in edits:
                p = Q + path
                s = open(p, encoding="utf-8").read()
                if s.count(old) != 1:
                    raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:80]))
                back.setdefault(p, s)
                open(p, "w", encoding="utf-8").write(s.replace(old, new))
            if only:
                results.append((name, "anchors ok"))
                continue
            log = os.path.join(RUNS, "brk%02d.log" % i)
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + targets, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                caught = []
                for t in tests:
                    suite, fn = t.split(":")
                    r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0"))
                    caught.append("%s %s" % (t.split(":")[1], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                results.append((name, "; ".join(caught)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-58s %s" % (name[:58], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
