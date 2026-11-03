#!/usr/bin/env python3
"""breaks.py: the fixes of the dock's fuzz and the cancellation of tool
calls undone on purpose, one piece at a time - the cards' labels, buttons and
fields, the attached images' row, the git bar's names, the composer's drops,
the transcript's images fitted, a thumbnail's height; the MCP server's
cancellation, its carriers' empty answers, the dock's held call, and each
tool's stop - each built and its tests run. Sources are put back whatever
happens. BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

W = "/Users/person/git/Qucs-S_Enhancements/.claude/worktrees/chat-mcp"
Q = W + "/qucs-s-26.1.1/qucs/"
B = W + "/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/dock-fuzz-breaks")
CACHE = os.path.join(RUNS, "cache")
os.makedirs(CACHE, exist_ok=True)

P = "claudecodepanel.cpp"
E = "elidedlabel.cpp"
CC = "test_claude_code"
WIDTH = (CC, "longTextsLeaveTheDockNarrow")
DROP = (CC, "filesDroppedOnTheComposerAreTheirPaths")
HELD = (CC, "aCancelledCallIsNeitherAnsweredNorAsked")
GIT = ("test_claude_git", "longNamesLeaveTheDockNarrow")
CANCEL = ("test_mcp_server", "aCancelledCallStopsAndIsNotAnswered")
STDIO = ("test_mcp_server", "theProgramServesOverStdio")
THUMB = ("test_markdown_doc", "aGivenHeightIsKeptAtItsWidth")
def fuzz(first, count, steps=3000):
    """The dock's fuzz over seeds first..first+count-1: caught when one finds it
    (the seeds that found it first replay other steps since the fuzz changed)."""
    return ("test_claude_fuzz", "fuzzTheDock", {"SEEDS": (first, count), "QUCS_FUZZ_STEPS": str(steps)})

BREAKS = [
 # ---- the cards (W1-W3)
 ("the permission card's title a QLabel again",
  [(P, "    a_cardTitle = new WrappedLabel(a_card);\n", "    a_cardTitle = new QLabel(a_card);\n    a_cardTitle->setWordWrap(true);\n")],
  [WIDTH]),
 ("its subject a QLabel again",
  [(P, "    a_cardSubject = new WrappedLabel(a_card);\n",
    "    a_cardSubject = new QLabel(a_card);\n    a_cardSubject->setWordWrap(true);\n    a_cardSubject->setTextFormat(Qt::PlainText);\n")],
  [WIDTH]),
 ("the question's title a QLabel again",
  [(P, "    auto* title = new WrappedLabel(ask.message, a_askCard);\n", "    auto* title = new QLabel(ask.message, a_askCard);\n    title->setWordWrap(true);\n")],
  [WIDTH]),
 ("a form's field titles plain labels",
  [(P, "            form->addRow(new WrappedLabel(label, a_askCard), editor);", "            form->addRow(label, editor);")],
  [WIDTH]),
 ("a choice's box as wide as its longest choice",
  [(P, "                combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);\n                combo->setMinimumContentsLength(8);\n", "")],
  [WIDTH]),
 ("the question's buttons whole",
  [(P, "        auto* b = new ElidedToolButton(a_askCard, Qt::ElideRight);\n", "        auto* b = new QToolButton(a_askCard);\n")],
  [WIDTH]),
 ("the question's buttons of the icon-only style (nothing to cut)",
  [(P, "        b->setToolButtonStyle(Qt::ToolButtonTextOnly);\n", "")],
  [WIDTH]),
 ("Allow Qucs-S Control whole",
  [(P, "    a_allowTools = new ElidedToolButton(a_card, Qt::ElideRight);\n", "    a_allowTools = new QToolButton(a_card);\n")],
  [WIDTH]),
 ("Allow All Edits whole",
  [(P, "    a_allowEdits = new ElidedToolButton(a_card, Qt::ElideRight);\n", "    a_allowEdits = new QToolButton(a_card);\n")],
  [WIDTH]),
 ("the long buttons of the icon-only style",
  [(P, "    a_allowTools->setToolButtonStyle(Qt::ToolButtonTextOnly);\n", ""),
   (P, "    a_allowEdits->setToolButtonStyle(Qt::ToolButtonTextOnly);\n", "")],
  [WIDTH]),
 # ---- WrappedLabel
 ("WrappedLabel's least width its longest word's",
  [(E, "    return QSize(std::min(whole.width(), chrome().width() + fontMetrics().averageCharWidth() * 8), whole.height());",
    "    return whole;")],
  [WIDTH]),
 ("WrappedLabel's size hint as wide as its longest word",
  [(E, "    if (whole.width() <= most) return whole;", "    return whole;")],
  [WIDTH]),
 ("a long word drawn as QLabel draws it (one line, cut)",
  [(E, "    if (!broken()) {\n        QLabel::paintEvent(event);", "    if (true) {\n        QLabel::paintEvent(event);")],
  [WIDTH]),
 ("its height for a width QLabel's (a long word one line)",
  [(E, "    if (QLabel::minimumSizeHint().width() <= width) return QLabel::heightForWidth(width);",
    "    return QLabel::heightForWidth(width);")],
  [WIDTH]),
 # ---- the images' row (W4)
 ("the attached images' row as wide as its pictures",
  [(P, "    a_imageStrip->setFixedHeight(a_imageRow->sizeHint().height()",
    "    a_imageStrip->setMinimumWidth(a_imageRow->sizeHint().width());\n    a_imageStrip->setFixedHeight(a_imageRow->sizeHint().height()")],
  [WIDTH]),
 # ---- the git bar (W5)
 ("the git bar's folder a QLabel again",
  [("claudegitbar.cpp", "    a_folder = new ElidedLabel(this, Qt::ElideRight);", "    a_folder = new QLabel(this);")],
  [GIT]),
 ("its branch a QLabel again",
  [("claudegitbar.cpp", "    a_branch = new ElidedLabel(this, Qt::ElideMiddle);", "    a_branch = new QLabel(this);")],
  [GIT]),
 # ---- the question card's layout (N1)
 ("an empty form's takeAt(0)",
  [(P, "            while (l->count() > 0) {\n                QLayoutItem* inner = l->takeAt(0);",
    "            while (QLayoutItem* inner = l->takeAt(0)) {")],
  [WIDTH]),
 # ---- drops (C1)
 ("drops on the composer left to Qt's own insertion",
  [(P, "            if (watched == a_input->viewport()) {\n                const QString text = droppedText(",
    "            if (false) {\n                const QString text = droppedText(")],
  [DROP]),
 ("a file dropped put in as its URL",
  [(P, "lines << (url.isLocalFile() ? QDir::toNativeSeparators(url.toLocalFile()) : url.toString());", "lines << url.toString();")],
  [DROP]),
 # ---- the transcript's images (A1) and a thumbnail's height (A2)
 ("images fitted to the viewport's width as it is",
  [(P, "    const int width = a_view->contentsRect().width() - (over ? 0 : bar->sizeHint().width());   // (no viewport margins)",
    "    const int width = a_view->viewport()->width() - (bar->isVisible() || over ? 0 : bar->sizeHint().width());")],
  [fuzz(1, 160)]),
 ("a thumbnail's height in its own size's proportion",
  [("markdowndoc.cpp", "        const qreal h = w == wanted && f.doubleProperty(kAuthorHeight) > 0 ? f.doubleProperty(kAuthorHeight) : w * ownHeight / ownWidth;",
    "        const qreal h = w * ownHeight / ownWidth;")],
  [THUMB]),
 # ---- cancellation: the server
 ("notifications/cancelled taken as any notification",
  [("mcpserver.cpp", "    if (method == QLatin1String(\"notifications/cancelled\")) {", "    if (false) {")],
  [CANCEL, HELD, STDIO]),
 ("a cancelled call answered all the same",
  [("mcpserver.cpp", "            if (self->a_cancelled.remove(key)) return reply(QJsonObject());", "            self->a_cancelled.remove(key);")],
  [CANCEL, STDIO]),
 ("the host not told",
  [("mcpserver.cpp", "            a_host->cancelCall(a_caller, tool);\n", "")],
  [CANCEL, HELD]),
 ("stdio writing an empty answer",
  [("mcpstdio.cpp", "            if (!response.isEmpty()) write(response);", "            write(response);")],
  [STDIO]),
 # ---- cancellation: the dock
 ("the dock writing an empty answer",
  [("claudecode.cpp", "        if (rpc.isEmpty()) return;   // (a call the program cancelled: not answered)\n", "")],
  [HELD]),
 ("a held call's question kept when its call is cancelled",
  [("claudecode.cpp", "    if (message.value(QLatin1String(\"method\")).toString() == QLatin1String(\"notifications/cancelled\")) {",
    "    if (false) {")],
  [HELD]),
 # ---- cancellation: the tools
 ("a call waiting its turn not taken out",
  [("qucscontrol.cpp", "    for (qsizetype i = 0; i < a_waiting.size(); ++i)\n        if (a_waiting.at(i).caller == caller && a_waiting.at(i).tool == tool) {",
    "    for (qsizetype i = 0; i < 0; ++i)\n        if (a_waiting.at(i).caller == caller && a_waiting.at(i).tool == tool) {")],
  [CANCEL]),
 ("callToolFor not naming the call under way",
  [("qucscontrol.cpp", "    const Running outerCall = std::exchange(a_running, Running{caller, tool});",
    "    const Running outerCall = a_running;")],
  [CANCEL]),
 ("fem_solve's job not cancelled",
  [("qucscontrol_fem.cpp", "        if (*begun == 1 && d) d->cancelJob();", "        if (false && d) d->cancelJob();")],
  [CANCEL]),
 ("a batch going on after its cancelled call",
  [("qucscontrol.cpp", "        if (run) run->cancel();", "        Q_UNUSED(run);")],
  [CANCEL]),
 ("a batch's later call not the batch's",
  [("qucscontrol.cpp", "        else a_control->callToolWithin(a_call, tool, arguments, after);", "        else a_control->callTool(tool, arguments, after);")],
  [CANCEL]),
 ("wait_for not stopped",
  [("qucscontrol_watch.cpp", "    const int stop = stoppable(running(), [cancelled] { *cancelled = true; });",
    "    const int stop = stoppable(running(), [] {});")],
  [CANCEL, STDIO]),
 ("run_script not interrupted",
  [("qucscontrol_script.cpp", "    const int stop = stoppable(running(), [&engine] { engine.setInterrupted(true); });",
    "    const int stop = stoppable(running(), [] {});")],
  [CANCEL]),
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
            suites = sorted({t[0] for t in tests})
            targets = ["qucs/tests/" + s for s in suites] + ["qucs/qucs-s.app/Contents/MacOS/qucs-s"]
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + targets, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for t in tests:
                    suite, fn = t[0], t[1]
                    extra = dict(t[2]) if len(t) > 2 else {}
                    first, count = extra.pop("SEEDS", (None, 1))
                    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=CACHE, ASAN_OPTIONS="detect_leaks=0",
                               QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="900000", **extra)
                    env.pop("QUCS_TEST_SLOW", None)
                    try:
                        if first is None:
                            r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                               stderr=subprocess.STDOUT, timeout=900, env=env, cwd=RUNS)
                            said.append("%s %s" % (fn[:30], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                            continue
                        # Seeds, eight at a time: caught by the first that fails.
                        caught = None
                        for at in range(first, first + count, 8):
                            procs = [(s, subprocess.Popen([os.path.join(B, "qucs/tests", suite), fn], stdout=subprocess.DEVNULL,
                                                          stderr=subprocess.DEVNULL, env=dict(env, QUCS_FUZZ_SEED=str(s)), cwd=RUNS))
                                     for s in range(at, min(at + 8, first + count))]
                            for s, p in procs:
                                if p.wait(timeout=900) != 0 and caught is None:
                                    caught = s
                            if caught is not None:
                                break
                        said.append("%s %s" % (fn[:30], "failed at seed %d (caught)" % caught if caught is not None
                                               else "PASSED %d seeds - NOT CAUGHT" % count))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:30])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-62s %s" % (name[:62], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said:
            bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":   # (read only, for BREAKS: nothing run)
    sys.exit(main())
