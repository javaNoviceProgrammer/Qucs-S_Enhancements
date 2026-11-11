#!/usr/bin/env python3
"""Undoes each piece of Conversation Cache, one at a time; its tests must fail.

    python3 breaks.py <build tree> [name ...]

Each break is a text replaced in a source file. The tree is built with it and
test_claude_code's three Conversation Cache tests and theCommandLineCarriesTheOptions
run, offscreen; a break that they do not fail on is said to have survived. The sources are put back
whatever happens.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "..", "qucs-s-26.1.1", "qucs")
PANEL = "claudecodepanel.cpp"
SESSION = "claudecode.cpp"

BREAKS = [
    # The choice.
    ("another default", PANEL, "    return CacheChoice::CompactIdle;   // (the default, the user's choice)", "    return CacheChoice::Warn;"),
    ("the menu's choices not exclusive", PANEL, "    cacheChoices->setExclusive(true);", "    cacheChoices->setExclusive(false);"),
    ("the menu's tick not kept", PANEL,
     "        for (QAction* a : cacheChoices->actions()) a->setChecked(a->data().toInt() == int(cacheChoice()));", ""),
    ("other conversations not told", PANEL,
     "    for (ClaudeCodePanel* panel : std::as_const(openPanels())) panel->applyCacheChoice();", ""),
    # Compact Earlier.
    ("Compact Earlier tells nothing", PANEL,
     "a_session->setAutoCompactWindow(choice == CacheChoice::CompactEarly ? kEarlyCompactWindow : 0);",
     "a_session->setAutoCompactWindow(0);"),
    ("the window not on the command line", SESSION,
     "    if (options.autoCompactWindow > 0) flags.insert(QStringLiteral(\"autoCompactWindow\"), options.autoCompactWindow);\n", ""),
    ("the window always on the command line", SESSION,
     "    if (options.autoCompactWindow > 0) flags.insert(QStringLiteral(\"autoCompactWindow\"), options.autoCompactWindow);",
     "    flags.insert(QStringLiteral(\"autoCompactWindow\"), options.autoCompactWindow);"),
    # What it knows of a conversation.
    ("its context not read", SESSION, "        if (!usage.isEmpty())\n            a_turnContext =", "        if (false)\n            a_turnContext ="),
    ("a subagent's context taken for it", SESSION,
     "    if (!subagent) {\n        const QJsonObject usage", "    if (true) {\n        const QJsonObject usage"),
    ("compacted not said", SESSION, "    if (subtype == QLatin1String(\"compact_boundary\")) a_turnContext = 0;\n", ""),
    ("its context not kept", PANEL, '    if (a_contextTokens > 0) cacheFacts.insert(QStringLiteral("contextTokens"), a_contextTokens);\n', ""),
    ("its last turn's time not brought back", PANEL,
     '    a_lastTurnAt = std::max<qint64>(0, cacheFacts.value(QLatin1String("lastTurnAt")).toInteger(0));\n', ""),
    ("Claude Code's file's context not read", PANEL, "                if (at.isValid()) lastTurnAt = at.toMSecsSinceEpoch();\n", ""),
    # Warn.
    ("never held", PANEL, "    if (holdsColdSend(text, withDocument, images)) return false;\n", ""),
    ("held while warm", PANEL, "    if (idle < kCacheLifetimeMs) return false;\n", ""),
    ("held whatever its length", PANEL,
     "    if (cacheChoice() != CacheChoice::Warn || a_contextTokens < kLongContext || a_lastTurnAt <= 0) return false;",
     "    if (cacheChoice() != CacheChoice::Warn || a_lastTurnAt <= 0) return false;"),
    ("no price said", PANEL, "tr(\", about %1 at API prices\").arg(cost(a_contextTokens * 2.0 * price / 1e6))", "QString()"),
    ("the price a 5-minute write's", PANEL, "cost(a_contextTokens * 2.0 * price / 1e6)", "cost(a_contextTokens * 1.25 * price / 1e6)"),
    ("Send Anyway held again", PANEL, "        a_coldSendAllowed = true;\n        if (sendText", "        if (sendText"),
    ("compacted, the prompt not sent", PANEL,
     "            QTimer::singleShot(0, this, [this, next = *next] { sendText(next.text, next.withDocument, next.images); });", ""),
    ("the composer kept after it went", PANEL, "        if (sendText(held.text, held.withDocument, held.images)) taken();", "        sendText(held.text, held.withDocument, held.images);"),
    # Compact idle.
    ("never compacted", PANEL, "    a_session->send(QStringLiteral(\"/compact\"));\n    updateState();\n}", "    updateState();\n}"),
    ("compacted whatever its length", PANEL,
     "    if (cacheChoice() != CacheChoice::CompactIdle || a_contextTokens < kLongContext || a_lastTurnAt <= 0",
     "    if (cacheChoice() != CacheChoice::CompactIdle || a_lastTurnAt <= 0"),
    ("compacted when its cache has gone", PANEL, "    if (now >= a_lastTurnAt + kCacheLifetimeMs - 60000) return;\n", ""),
    ("at once, not at 50 minutes", PANEL, "a_idleCompact->start(int(std::max<qint64>(0, a_lastTurnAt + kCompactIdleAfterMs - now)));",
     "a_idleCompact->start(0);"),
    ("not armed by a turn", PANEL, "    armIdleCompact();\n    showNextRequest();\n    updateState();\n    if (a_gitBar", "    showNextRequest();\n    updateState();\n    if (a_gitBar"),
    ("not armed when brought back", PANEL,
     '    a_lastModel = cacheFacts.value(QLatin1String("model")).toString();\n    a_held.reset();\n    a_afterCompact.reset();\n    a_cacheNote->hide();\n    armIdleCompact();\n',
     '    a_lastModel = cacheFacts.value(QLatin1String("model")).toString();\n    a_held.reset();\n    a_afterCompact.reset();\n    a_cacheNote->hide();\n'),
]




def run(build, env, *command):
    return subprocess.run(command, cwd=os.path.join(build, "qucs", "tests"), env=env, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900)


def main():
    build = os.path.abspath(sys.argv[1])
    only = sys.argv[2:]
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    survived = []
    for name, file, old, new in BREAKS:
        if only and name not in only:
            continue
        path = os.path.join(SOURCE, file)
        with open(path, encoding="utf-8") as f:
            original = f.read()
        if original.count(old) != 1:
            print(f"NOT APPLIED  {name}: its text is there {original.count(old)} times")
            survived.append(name)
            continue
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(original.replace(old, new))
            built = subprocess.run(["cmake", "--build", build, "--target", "test_claude_code"], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if built.returncode != 0:
                print(f"NOT BUILT    {name}\n{built.stdout[-800:]}")
                survived.append(name)
                continue
            test = run(build, env, "./test_claude_code", "theConversationCacheIsOneOfThree", "aColdLongConversationIsHeld",
                       "aLongIdleConversationIsCompactedWhileWarm", "theCommandLineCarriesTheOptions")
            fails = [line for line in test.stdout.splitlines() if line.startswith("FAIL!")]
            if test.returncode != 0:
                print(f"caught       {name}: {fails[0][:150] if fails else 'test_claude_code ended ' + str(test.returncode)}")
                continue
            print(f"SURVIVED     {name}")
            survived.append(name)
        finally:
            with open(path, "w", encoding="utf-8") as f:
                f.write(original)
    subprocess.run(["cmake", "--build", build, "--target", "test_claude_code"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    print(f"{len(survived)} survived" + (": " + ", ".join(survived) if survived else ""))
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
