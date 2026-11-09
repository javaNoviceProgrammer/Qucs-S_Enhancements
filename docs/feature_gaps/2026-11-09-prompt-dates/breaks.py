#!/usr/bin/env python3
"""Undoes each piece of Show Prompt Dates, one at a time; its test must fail.

    python3 breaks.py <build tree> [name ...]

Each break is a text replaced in a source file. The tree is built with it and
test_claude_code's promptDatesShowInEachPromptsCorner run, offscreen; a break
that it does not fail on is said to have survived. The sources are put back
whatever happens.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "..", "qucs-s-26.1.1", "qucs")
PANEL = "claudecodepanel.cpp"

BREAKS = [
    ("a prompt's time not kept", PANEL, "    e.when = QDateTime::currentMSecsSinceEpoch();\n", ""),
    ("its time not saved", PANEL, '        if (e.when > 0) o.insert(QStringLiteral("when"), e.when);\n', ""),
    ("its time not brought back", PANEL,
     '        if (e.kind == Entry::You) e.when = std::max<qint64>(0, o.value(QLatin1String("when")).toInteger(0));\n', ""),
    ("Claude Code's time not read", PANEL, "                if (sent.isValid()) e.when = sent.toMSecsSinceEpoch();\n", ""),
    ("the day before the month", PANEL, 'toString(QStringLiteral("MM/dd/yyyy"))', 'toString(QStringLiteral("dd/MM/yyyy"))'),
    ("a day not the prompt's", PANEL, "QDateTime::fromMSecsSinceEpoch(when).toLocalTime().date()", "QDateTime::fromMSecsSinceEpoch(when).toUTC().addDays(400).date()"),
    ("in the bubble, on the prompt's first line", PANEL,
     "const QTextBlock caption = document->findBlock(frame->firstPosition()).previous();",
     "const QTextBlock caption = document->findBlock(frame->firstPosition());"),
    ("drawn at the left", PANEL, "rightToLeft ? box.left() : box.right() - width", "box.left()"),
    ("short of the bubble's edge", PANEL, "rightToLeft ? box.left() : box.right() - width", "rightToLeft ? box.left() : box.right() - width - 8"),
    ("placed as if the caption's layout were the document's", PANEL,
     "const qreal top = captionBox.top() - lines->boundingRect().top();", "const qreal top = lines->position().y() + 30;"),
    ("over the caption's words when narrow", PANEL, "        if (!clear) continue;\n", ""),
    ("never drawn", PANEL, "        paintPromptDates(&painter, shown, a_dateFont, a_dateColour);\n", ""),
    ("the setting not a reason to draw again", PANEL, "    s << promptDatesShown();\n    return key;", "    return key;"),
    ("other conversations not told", PANEL,
     "    QucsSettingsFile().setValue(kShowPromptDates, on);\n    for (ClaudeCodePanel* panel : std::as_const(openPanels())) panel->scheduleRender();",
     "    QucsSettingsFile().setValue(kShowPromptDates, on);"),
    ("the menu's tick not kept", PANEL, "        dates->setChecked(promptDatesShown());\n", ""),
    ("no time in the tool tip", PANEL, "QLocale().toString(QDateTime::fromMSecsSinceEpoch(d.when).toLocalTime(), QLocale::LongFormat)", "d.text"),
    ("the date in the document, copied with it", PANEL,
     "        c.insertFrame(frame);\n",
     "        if (e.when > 0 && promptDatesShown()) c.insertText(QStringLiteral(\"  \") + promptDateText(e.when), muted);\n        c.insertFrame(frame);\n"),
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
            test = run(build, env, "./test_claude_code", "promptDatesShowInEachPromptsCorner")
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
