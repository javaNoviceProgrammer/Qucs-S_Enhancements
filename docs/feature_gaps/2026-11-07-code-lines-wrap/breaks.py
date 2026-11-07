#!/usr/bin/env python3
"""Undoes each piece of the code lines' wrapping and the long words' places to break,
one at a time; its tests must fail.

    python3 breaks.py <build tree> [name ...]

Each break is a text replaced in a source file. The tree is built with it and
test_claude_code's aLongCodeLineWrapsAndStaysOneLine and
aLongWordHasPlacesToBreakWhereverItIs, then test_claude_fuzz, run offscreen; a
break that nothing fails on is said to have survived. The sources are put back
whatever happens.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "..", "qucs-s-26.1.1", "qucs")
PANEL = "claudecodepanel.cpp"
MARKDOWN = "markdowndoc.cpp"
CLEAR = "shaded.clearProperty(QTextFormat::BlockNonBreakableLines);"
PART = "qucs_s::markdown::breakLongRuns(doc, partAt, kTranscriptRuns);"
LONGEST = "constexpr qucs_s::markdown::RunBreaks kTranscriptRuns{2000, 8, true};"

BREAKS = [
    # Code wrapping.
    ("code does not wrap, as it was", PANEL, CLEAR, ""),
    ("wrapped by taking away what says it is code", PANEL, CLEAR,
     CLEAR + " shaded.clearProperty(QTextFormat::BlockCodeFence); shaded.clearProperty(QTextFormat::BlockCodeLanguage);"),
    ("code loses its shade", PANEL, "            shaded.setBackground(col.code);\n", ""),
    ("the first block of a reply left unwrapped", PANEL,
     "for (QTextBlock b = target->findBlock(from); b.isValid() && b.position() <= c.position(); b = b.next()) {\n        const QTextBlockFormat bf = b.blockFormat();\n        if (bf.nonBreakableLines()",
     "for (QTextBlock b = target->findBlock(from).next(); b.isValid() && b.position() <= c.position(); b = b.next()) {\n        const QTextBlockFormat bf = b.blockFormat();\n        if (bf.nonBreakableLines()"),
    ("the document's wrapping turned off", PANEL,
     "    a_view->setOpenLinks(false);\n",
     "    a_view->setOpenLinks(false);\n    a_view->setLineWrapMode(QTextEdit::NoWrap);\n"),
    ("words wrap only between words", PANEL,
     "    a_view->setOpenLinks(false);\n",
     "    a_view->setOpenLinks(false);\n    a_view->setWordWrapMode(QTextOption::WordWrap);\n"),
    # Places to break a long word.
    ("no places to break a long word", PANEL, PART, ""),
    ("not in code", PANEL, LONGEST, "constexpr qucs_s::markdown::RunBreaks kTranscriptRuns{2000, 8, false};"),
    ("broken from a little later", PANEL, LONGEST, "constexpr qucs_s::markdown::RunBreaks kTranscriptRuns{2001, 8, true};"),
    ("broken from a little sooner", PANEL, LONGEST, "constexpr qucs_s::markdown::RunBreaks kTranscriptRuns{1999, 8, true};"),
    ("rows ending far from the edge", PANEL, LONGEST, "constexpr qucs_s::markdown::RunBreaks kTranscriptRuns{2000, 64, true};"),
    ("a word broken into too few places", MARKDOWN, "for (int k = start + how.every; k < end; k += how.every) at.push_back(k);",
     "at.push_back(start + how.every);"),
    ("a run counted from the block's start", MARKDOWN, "                ended(last, i);\n                last = i;\n", "                ended(last, i);\n"),
    ("the last run of a block not broken", MARKDOWN, "        ended(last, size);\n", ""),
    ("the mark of writing left where it was", PANEL, "if (a_replyMark >= 0) a_replyMark = mark.position();", ""),
    # What is read from it.
    ("copied with the breaks in it", PANEL, "                qucs_s::markdown::removeRunBreaks(data);\n", ""),
    ("copied with the text's own spaces of no width taken out", PANEL,
     "        const QTextCursor chosen = textCursor();\n", "        qucs_s::markdown::removeRunBreaks(data);\n        const QTextCursor chosen = textCursor();\n"),
    ("the breaks left in the HTML", MARKDOWN, "    if (data->hasHtml()) data->setHtml(QString(data->html()).remove(mark));\n", ""),
    ("the breaks left in the Markdown", MARKDOWN,
     "    if (data->hasFormat(markdown)) data->setData(markdown, QString::fromUtf8(data->data(markdown)).remove(mark).toUtf8());\n", ""),
    ("the breaks left in the zipped form", MARKDOWN, '    data->removeFormat(QStringLiteral("application/vnd.oasis.opendocument.text"));\n', ""),
    # The composer.
    ("the composer as it was", PANEL, "if (hasLongRun(text)) composer->setWordWrapMode(QTextOption::WrapAnywhere);", ""),
    ("the composer never back to whole words", PANEL,
     "    if (a_input->wordWrapMode() != wrap) a_input->setWordWrapMode(wrap);\n", "    if (wrap == QTextOption::WrapAnywhere) a_input->setWordWrapMode(wrap);\n"),
    ("the composer told only after the text is in, pasting", PANEL,
     "        if (source != nullptr && source->hasText()) expectInComposer(this, source->text());\n", ""),
    ("the composer told only after the text is in, quoting", PANEL,
     "    expectInComposer(a_input, text);\n", ""),
]


def run(build, env, *command):
    return subprocess.run(command, cwd=os.path.join(build, "qucs", "tests"), env=env, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900)


def main():
    build = os.path.abspath(sys.argv[1])
    only = sys.argv[2:]
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_FUZZ_STEPS="400")
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
            built = subprocess.run(["cmake", "--build", build, "--target", "test_claude_code", "test_claude_fuzz"], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if built.returncode != 0:
                print(f"NOT BUILT    {name}\n{built.stdout[-800:]}")
                survived.append(name)
                continue
            test = run(build, env, "./test_claude_code", "aLongCodeLineWrapsAndStaysOneLine", "aLongWordHasPlacesToBreakWhereverItIs")
            fails = [line for line in test.stdout.splitlines() if line.startswith("FAIL!")]
            if test.returncode != 0:
                print(f"caught       {name}: {fails[0][:150] if fails else 'test_claude_code ended ' + str(test.returncode)}")
                continue
            fuzz = run(build, env, "./test_claude_fuzz")
            if fuzz.returncode != 0:
                print(f"caught       {name}: by the fuzz")
                continue
            print(f"SURVIVED     {name}")
            survived.append(name)
        finally:
            with open(path, "w", encoding="utf-8") as f:
                f.write(original)
    subprocess.run(["cmake", "--build", build, "--target", "test_claude_code", "test_claude_fuzz"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    print(f"{len(survived)} survived" + (": " + ", ".join(survived) if survived else ""))
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
