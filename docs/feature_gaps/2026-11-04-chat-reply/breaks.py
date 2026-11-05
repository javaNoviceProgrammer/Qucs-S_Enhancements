#!/usr/bin/env python3
"""breaks.py: Reply in the Claude Code dock's menu (4 November) undone on purpose,
one piece at a time - when it is offered and where, the quote and where it goes,
how a selection reads, what a selection does as the conversation is drawn again,
the permission card's text - each built and its test run. A break counts as
caught only where its test fails at the assertion meant for it: the failing line
of the test, as QtTest's Loc names it, must hold the break's needle. Sources are
put back whatever happens. BRK_ONLY_CHECK=1 checks the anchors and writes
nothing; BRK_ONLY=3,7 runs those alone; HUNT_RUNS=<dir> for logs; BRK_BUILD=<dir>
the build (the checkout's build); BRK_SOURCE=<dir> a copy of qucs-s-26.1.1 to
break instead of the checkout's. Run as a script only: importing it does
nothing."""
import os, re, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/chat-reply-breaks")

P = "claudecodepanel.cpp"
SUITE = "test_claude_code"
OFFER = "replyIsOfferedWhereTextIsSelected"
QUOTES = "replyQuotesTheSelectionInTheComposer"
KEEPS = "replyKeepsWhatTheComposerHolds"
LINES = "replyReadsParagraphsAndLines"
BLOCKS = "replyReadsListsCodeAndTables"
MATH = "replyReadsMathAsItWasWritten"
KINDS = "replyReadsThePromptsAndToolsToo"
LONG = "replyOfALongSelectionIsWhole"
STREAM = "aReplyBeingWrittenCanBeQuoted"
STAYS = "aSelectionStaysAsTheConversationIsDrawn"
DRAG = "aDragSelectsWhatReplyQuotes"
COPY = "copyAndSelectAllStayBesideReply"
TABS = "replyGoesToTheTabItWasChosenIn"
CARD = "replyFromThePermissionCard"
IMAGE = "replyAboveTheUsualEntriesBesideOpenInViewer"

QUOTE_PUT = 'end.insertText(apart + asQuote(text) + QStringLiteral("\\n\\n"));'
OFFER_AT = """    QAction* first = menu->actions().value(0);
    menu->insertAction(first, reply);
    if (first != nullptr) menu->insertSeparator(first);
"""
KEEP_CALL = """    if (chosenAnchor >= 0) chooseAgain(chosenAnchor, chosenPosition, chosenText);
    bar->setValue(a_following ? bar->maximum() : keep);
"""
TWISTY = """    QTextCharFormat knob = twisty;
    knob.setProperty(kWorksTheView, true);
    if (!a_exporting) c.insertText(open ? QStringLiteral("▾ ") : QStringLiteral("▸ "), knob);
"""

# Each: what is done, the edits (file, as it is, as it becomes), and the tests:
# (function, what the failing line must hold).
BREAKS = [
 # When it is offered, and where.
 ("Reply offered with nothing selected",
  [(P, "    if (selected.isEmpty()) return;\n    auto* reply", "    auto* reply")],
  [(OFFER, "usual);")]),
 ("Reply under the usual entries",
  [(P, OFFER_AT, "    menu->addAction(reply);\n")],
  [(OFFER, "withReply);")]),
 ("Reply with no separator under it",
  [(P, "    menu->insertAction(first, reply);\n    if (first != nullptr) menu->insertSeparator(first);\n", "    menu->insertAction(first, reply);\n")],
  [(OFFER, "value(1)->isSeparator()")]),
 ("Reply offered for spaces alone",
  [(P, '            if (out.isEmpty()) text.remove(leading);\n', ''),
   (P, "    while (!out.isEmpty() && out.constFirst().trimmed().isEmpty()) out.removeFirst();\n    while (!out.isEmpty() && out.constLast().trimmed().isEmpty()) out.removeLast();\n", "")],
  [(OFFER, 'QStringList({"Copy", "Copy Link Location (off)", "Select All"})')]),
 ("the menu's own Copy taken away by Reply",
  [(P, OFFER_AT, "    QAction* first = menu->actions().value(0);\n    menu->insertAction(first, reply);\n    menu->removeAction(first);\n")],
  [(COPY, 'clipboard()->text(), QStringLiteral("batches are running")')]),
 ("Open in Viewer under Reply and the usual entries",
  [(P, "    menu->insertAction(first, open);\n", "    menu->addAction(open);\n")],
  [(IMAGE, '"Open in Viewer", "Reply"')]),
 # The quote, and where it goes.
 ("what Reply chose sent at once",
  [(P, "    a_input->setTextCursor(end);\n    focusComposer();\n}\n\nQString ClaudeCodePanel::imageAt", "    a_input->setTextCursor(end);\n    focusComposer();\n    sendComposer();\n}\n\nQString ClaudeCodePanel::imageAt")],
  [(QUOTES, "isBusy()")]),
 ("what the composer holds replaced",
  [(P, QUOTE_PUT, 'a_input->setPlainText(asQuote(text) + QStringLiteral("\\n\\n"));\n    end.movePosition(QTextCursor::End);')],
  [(KEEPS, 'replyOn("my draft"),')]),
 ("the quote put in at the composer's cursor",
  [(P, "    QTextCursor end(a_input->document());\n    end.movePosition(QTextCursor::End);\n", "    QTextCursor end = a_input->textCursor();\n")],
  [(KEEPS, 'replyOn("my draft"),')]),
 ("no blank line under the quote",
  [(P, QUOTE_PUT, 'end.insertText(apart + asQuote(text) + QStringLiteral("\\n"));')],
  [(QUOTES, '"> batches are running\\n\\n"));')]),
 ("no blank line over the quote",
  [(P, ': QStringLiteral("\\n\\n");\n    QTextCursor end(a_input->document());', ': QStringLiteral("\\n");\n    QTextCursor end(a_input->document());')],
  [(KEEPS, 'replyOn("my draft"),')]),
 ("a line too many after a draft that ends one",
  [(P, ": draft.endsWith(QLatin1Char('\\n'))                       ? QStringLiteral(\"\\n\")", ": draft.endsWith(QLatin1Char('\\n'))                       ? QStringLiteral(\"\\n\\n\")")],
  [(KEEPS, 'replyOn("my draft\\n"),')]),
 ("a line over the quote in an empty composer",
  [(P, 'const QString apart = draft.isEmpty() || draft.endsWith(QLatin1String("\\n\\n"))', 'const QString apart = draft.endsWith(QLatin1String("\\n\\n"))')],
  [(QUOTES, '"> batches are running\\n\\n"));')]),
 ("the focus left where it was",
  [(P, "    a_input->setTextCursor(end);\n    focusComposer();\n", "    a_input->setTextCursor(end);\n")],
  [(QUOTES, "composer->hasFocus()")]),
 ("the composer's cursor not put under the quote",
  [(P, "    a_input->setTextCursor(end);\n    focusComposer();\n", "    focusComposer();\n")],
  [(KEEPS, "!composer->textCursor().hasSelection()")]),
 ("two steps to undo",
  [(P, QUOTE_PUT, 'end.insertText(apart + asQuote(text));\n    end.insertText(QStringLiteral("\\n\\n"));')],
  [(QUOTES, "QCOMPARE(undone,")]),
 ("to the first conversation's composer, not its own",
  [(P, "[this, selected] { quoteInComposer(selected); }", "[selected] { openPanels().constFirst()->quoteInComposer(selected); }")],
  [(TABS, "second's draft\\n\\n> Said in the second")]),
 ("the quote read when it is chosen, not when asked for",
  [(P, "[this, selected] { quoteInComposer(selected); }", "[this] { quoteInComposer(readSelection(a_view->textCursor())); }")],
  [(STREAM, '"> one.\\n\\n"')]),
 ("a long selection cut",
  [(P, "    QStringList lines = text.split(QLatin1Char('\\n'));\n    for (QString& line : lines) {\n        while", "    QStringList lines = text.left(100000).split(QLatin1Char('\\n'));\n    for (QString& line : lines) {\n        while")],
  [(LONG, '"Paragraph 2999')]),
 # How a selection reads.
 ("zero-width spaces left in",
  [(P, "    return text.remove(QChar(0x200B)).remove(QChar::ObjectReplacementCharacter);", "    return text.remove(QChar::ObjectReplacementCharacter);")],
  [(LINES, 'quoteOf(panel, "zero", "break.")')]),
 ("a line in a block not a line",
  [(P, "    text.replace(QChar::ParagraphSeparator, QLatin1Char('\\n')).replace(QChar::LineSeparator, QLatin1Char('\\n'));\n", "    text.replace(QChar::ParagraphSeparator, QLatin1Char('\\n'));\n")],
  [(LINES, 'quoteOf(panel, "Sixth"')]),
 ("paragraphs not apart",
  [(P, "apart(last != Paragraph || below + bf.topMargin() >= kApart);", "apart(last != Paragraph);")],
  [(LINES, 'quoteOf(panel, "First", "soft broken.")')]),
 ("every line of a prompt apart",
  [(P, "constexpr qreal kApart = 4.0;", "constexpr qreal kApart = 0.0;")],
  [(KINDS, 'quoteOf(panel, "Typed", "three")')]),
 ("tools in a row apart",
  [(P, "constexpr qreal kApart = 4.0;", "constexpr qreal kApart = 3.0;")],
  [(KINDS, 'quoteOf(rows, "Read 2 files", "A note")')]),
 ("a quote's blank line with a space on it",
  [(P, 'line = line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + line;', 'line = QStringLiteral("> ") + line;')],
  [(LINES, 'quoteOf(panel, "First", "soft broken.")')]),
 ("spaces at the ends of its lines left on",
  [(P, "        while (line.endsWith(QLatin1Char(' ')) || line.endsWith(QLatin1Char('\\t'))) line.chop(1);\n", "")],
  [(KINDS, 'quoteOf(panel, "You", "Typed line one")')]),
 ("a selection from a space an indented line",
  [(P, '            if (out.isEmpty()) text.remove(leading);\n', '')],
  [(LINES, 'quoteOf(panel, " and it"')]),
 ("no rule between paragraphs",
  [(P, '            apart(true);\n            out << QStringLiteral("----");\n            last = Paragraph;\n        } else if (QTextList* list = b.textList()) {\n            apart(last != Item);\n            out << itemLines(list, b, quoted, text);\n            last = Item;\n        } else if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {',
       '            last = Paragraph;\n        } else if (QTextList* list = b.textList()) {\n            apart(last != Item);\n            out << itemLines(list, b, quoted, text);\n            last = Item;\n        } else if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {')],
  [(LINES, 'quoteOf(panel, "Fourth", "Fifth")')]),
 ("list items without their marks",
  [(P, "            out << itemLines(list, b, quoted, text);\n            last = Item;\n        } else if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {",
       "            out << text;\n            last = Item;\n        } else if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {")],
  [(BLOCKS, 'quoteOf(panel, "Passing", "why")')]),
 ("every number 1.",
  [(P, "    QString mark = list->itemText(item).trimmed();", '    QString mark = QStringLiteral("1.");\n    if (list->format().style() != QTextListFormat::ListDecimal) mark = list->itemText(item).trimmed();')],
  [(BLOCKS, 'quoteOf(panel, "four", "five")')]),
 ("every bullet a disc",
  [(P, 'mark = list->format().style() == QTextListFormat::ListCircle ? QStringLiteral("◦") : QStringLiteral("•");', 'mark = QStringLiteral("•");')],
  [(BLOCKS, 'quoteOf(panel, "Circled", "After the circle")')]),
 ("a nested item not indented",
  [(P, "    const QString lead(2 * std::max(0, list->format().indent() - 1), QLatin1Char(' '));\n    // (A bullet's", "    const QString lead;\n    // (A bullet's")],
  [(BLOCKS, 'quoteOf(panel, "Points", "Nested")')]),
 ("items apart",
  [(P, "apart(last != Item);\n            out << itemLines(list, b, quoted, text);\n            last = Item;\n        } else if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {",
       "apart(true);\n            out << itemLines(list, b, quoted, text);\n            last = Item;\n        } else if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {")],
  [(BLOCKS, 'quoteOf(panel, "Passing", "why")')]),
 ("code not indented",
  [(P, 'out << quoted + QStringLiteral("    ") + text.replace(QLatin1Char(\'\\n\'), QLatin1Char(\'\\n\') + quoted + QStringLiteral("    "));', "out << quoted + text;")],
  [(BLOCKS, '"Run", "After the code"')]),
 ("code lines apart",
  [(P, 'apart(last != Code);\n            out << quoted + QStringLiteral("    ") + text.replace(', 'apart(true);\n            out << quoted + QStringLiteral("    ") + text.replace(')],
  [(BLOCKS, '"Run", "After the code"')]),
 ("indented code read as a paragraph",
  [(P, "bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {\n            apart(last != Code);\n            out << quoted + QStringLiteral(\"    \") + text.replace(", "bf.hasProperty(QTextFormat::BlockCodeFence)) {\n            apart(last != Code);\n            out << quoted + QStringLiteral(\"    \") + text.replace(")],
  [(BLOCKS, 'quoteOf(panel, "Then", "After indented")')]),
 ("a word of one block read with its block's marks",
  [(P, "    if (withWords == 1) return words.trimmed();\n", "")],
  [(BLOCKS, 'quoteOf(panel, "Open", "why")')]),
 ("a word in a cell read as the whole table, cells as lines",
  [(P, "if (QTextTable* table = withinCell ? nullptr : QTextCursor(b).currentTable()) {", "if (QTextTable* table = withinCell ? QTextCursor(b).currentTable() : nullptr) {")],
  [(BLOCKS, 'quoteOf(panel, "Thr", "ree")')]),
 ("all of a table where some cells are chosen",
  [(P, "            if (selection.hasComplexSelection()) selection.selectedTableCells(&row, &rows, &column, &columns);\n", "")],
  [(BLOCKS, 'quoteOf(panel, "Two", "Three")')]),
 ("a selection from an item's end with its mark",
  [(P, "        if (from == to && out.isEmpty()) continue;   // (chosen from its end: none of it)\n", "")],
  [(BLOCKS, '"> • Open: why\\n>   • Nested\\n\\n"')]),
 ("a selection up to an item's start with its mark",
  [(P, "        if (b.position() == end) break;   // (chosen up to its start: none of it)\n", "")],
  [(BLOCKS, '"> Intro.\\n>\\n> Points:\\n\\n"')]),
 ("a quote in the reply read as text",
  [(P, "    return quote > 0 ? QString(quote, QLatin1Char('>')) + QLatin1Char(' ') : QString();", "    return quote > 0 ? QString() : QString();")],
  [(BLOCKS, 'quoteOf(panel, "Said", "again")')]),
 ("math the place of an object",
  [(P, "        if (format.hasProperty(kWrittenAs)) {\n", "        if (false) {\n")],
  [(MATH, 'quoteOf(panel, "The value", "is low")')]),
 ("display math between one dollar each",
  [(P, 'const QString dollars = sp.display ? QStringLiteral("$$") : QStringLiteral("$");', 'const QString dollars = QStringLiteral("$");')],
  [(MATH, 'quoteOf(panel, "here", "After")')]),
 ("the same formula twice in a row read once",
  [(P, "text += format.stringProperty(kWrittenAs).repeated(end - first);", "text += format.stringProperty(kWrittenAs);")],
  [(MATH, 'quoteOf(panel, "twice", "here")')]),
 ("a picture not named",
  [(P, "        } else if (format.isImageFormat()) {\n", "        } else if (false) {\n")],
  [(IMAGE, "![the plot]")]),
 ("what works the view read as words",
  [(P, "        if (format.hasProperty(kWorksTheView)) continue;\n", "")],
  [(KINDS, 'quoteOf(panel, "You", "Typed line one")'), (STREAM, '"> goes on and on\\n\\n"')]),
 ("a prompt's Edit not marked",
  [(P, "            edit.setProperty(kWorksTheView, true);\n", "")],
  [(KINDS, 'quoteOf(panel, "You", "Typed line one")')]),
 ("the mark of a reply being written not marked",
  [(P, "            cursor.setProperty(kWorksTheView, true);\n", "")],
  [(STREAM, '"> goes on and on\\n\\n"')]),
 ("a row of tools' twisty not marked",
  [(P, "    twisty.setForeground(col.faint);\n" + TWISTY, "    twisty.setForeground(col.faint);\n" + TWISTY.replace("    knob.setProperty(kWorksTheView, true);\n", ""))],
  [(KINDS, '"> ✓  Read 2 files')]),
 ("a tool's twisty not marked",
  [(P, "    twisty.setForeground(more ? col.faint : col.base);\n" + TWISTY, "    twisty.setForeground(more ? col.faint : col.base);\n" + TWISTY.replace("    knob.setProperty(kWorksTheView, true);\n", ""))],
  [(KINDS, 'startsWith(QStringLiteral("> ○  Write"))')]),
 # What a selection does as the conversation is drawn again.
 ("a selection lost at each piece",
  [(P, KEEP_CALL, "    bar->setValue(a_following ? bar->maximum() : keep);\n")],
  [(STREAM, "selectedText(), chosen);")]),
 ("the mark of writing kept in a selection",
  [(P, "const int upTo = a_replyMark >= 0 ? a_replyMark : end;", "const int upTo = end;")],
  [(STREAM, 'QStringLiteral("goes on and on"));')]),
 ("where the mark of writing is left when it is gone",
  [(P, "    a_replyMark = -1;\n\n    QTextCursor c(doc);", "    QTextCursor c(doc);")],
  [(STAYS, 'QStringLiteral("later reply."));')]),
 ("words that moved not found again",
  [(P, "        if (at < 0) choose(from, from);\n", "        if (true) choose(from, from);\n")],
  [(STREAM, 'QStringLiteral("bold"));')]),
 ("the same words ahead rather than the nearest",
  [(P, "behind < 0 ? ahead : ahead - from <= from - behind ? ahead : behind;", "behind < 0 ? ahead : ahead;")],
  [(STREAM, "second - 2")]),
 ("a backward selection forward when moved",
  [(P, "        else choose(int(at + text.size()), int(at));\n", "        else choose(int(at), int(at + text.size()));\n")],
  [(STREAM, "view->textCursor().position(), ts - 2")]),
 ("a selection whose words are gone left as it fell",
  [(P, "        if (at < 0) choose(from, from);\n", "        if (at < 0) return;\n")],
  [(STAYS, "!view->textCursor().hasSelection()")]),
 ("a selection up to the end growing with what comes",
  [(P, "if (chosen.selectionEnd() >= redrawn && (chosen.hasSelection()", "if (chosen.selectionEnd() > redrawn && (chosen.hasSelection()")],
  [(STAYS, 'QStringLiteral("first reply."));')]),
 ("a selection not kept where all is drawn again",
  [(P, "const int redrawn = afresh ? 0 : same < a_drawn.size()", "const int redrawn = afresh ? end + 1 : same < a_drawn.size()")],
  [(STAYS, 'QStringLiteral("first reply."));')]),
 ("the view brought to the selection",
  [(P, KEEP_CALL, "    bar->setValue(a_following ? bar->maximum() : keep);\n    if (chosenAnchor >= 0) chooseAgain(chosenAnchor, chosenPosition, chosenText);\n")],
  [(STAYS, "down->value(), down->maximum()")]),
 ("the view taken to the selection taken for the reader's scrolling",
  [(P, "    a_scrolling = true;\n    QScrollBar* across = a_view->horizontalScrollBar();\n", "    QScrollBar* across = a_view->horizontalScrollBar();\n")],
  [(STAYS, 'And yet more")); QVERIFY(panel.followsTheEnd());')]),
 ("the view brought sideways to the selection",
  [(P, "    across->setValue(acrossAt);\n", "")],
  [(STAYS, "across->value(), 40")]),
 ("where a drag was pressed lost at a piece",
  [(P, "(chosen.hasSelection() || chosen.position() < end)", "chosen.hasSelection()")],
  [(DRAG, 'QStringLiteral("words being"));')]),
 # The permission card's text.
 ("no Reply on the card's subject",
  [(P, "        offerReply(&menu, asRead(a_cardSubject->selectedText()).trimmed());\n", "")],
  [(CARD, 'QStringList({"Reply", "Copy", "Select All"})')]),
 ("no Reply on the card's details",
  [(P, "        offerReply(menu.get(), readSelection(a_cardDetail->textCursor()));\n", "")],
  [(CARD, "items.value(0)")]),
 ("the card's details not read as lines",
  [(P, "offerReply(menu.get(), readSelection(a_cardDetail->textCursor()));", "offerReply(menu.get(), a_cardDetail->textCursor().selectedText());")],
  [(CARD, '"> line one\\n>\\n>   line three\\n\\n"')]),
 ("the subject's lines not read as lines",
  [(P, "offerReply(&menu, asRead(a_cardSubject->selectedText()).trimmed());", "offerReply(&menu, a_cardSubject->selectedText());")],
  [(CARD, '"> one\\n> lin\\n\\n"')]),
 ("the subject's Copy always on",
  [(P, "        copy->setEnabled(a_cardSubject->hasSelectedText());\n", "")],
  [(CARD, 'QStringList({"Copy (off)", "Select All"})')]),
 ("the subject's Copy with Qt's paragraph mark",
  [(P, "QApplication::clipboard()->setText(asRead(a_cardSubject->selectedText()));", "QApplication::clipboard()->setText(a_cardSubject->selectedText());")],
  [(CARD, 'QStringLiteral("one\\nlin")')]),
 ("the subject's Select All doing nothing",
  [(P, "a_cardSubject->setSelection(0, int(a_cardSubject->text().size()));", "a_cardSubject->setSelection(0, 1);")],
  [(CARD, "subject->selectedText(), subject->text()")]),
]


def apply(edits, back):
    for f, old, new in edits:
        p = Q + f
        s = open(p, encoding="utf-8").read()
        n = s.count(old)
        if n != 1:
            raise SystemExit("anchor found %d times in %s: %r" % (n, f, old[:100]))
        if back is not None:
            back.setdefault(p, s)
            open(p, "w", encoding="utf-8").write(s.replace(old, new))


def failing_line(output, fn):
    """The test's source lines QtTest names for the failure of \\a fn: its line and the
    one before it (a macro over two lines), or None. A crash has no such line."""
    m = re.search(r"FAIL!  : \w+::%s\(.*?Loc: \[(.*?)\((\d+)\)\]" % re.escape(fn), output, re.S)
    if not m:
        return None
    lines = open(m.group(1), encoding="utf-8").read().split("\n")
    at = int(m.group(2))
    return lines[at - 2].strip() + " " + lines[at - 1].strip()


def main():
    os.makedirs(os.path.join(RUNS, "cache"), exist_ok=True)
    if os.environ.get("BRK_ONLY_CHECK"):
        for i, (name, edits, tests) in enumerate(BREAKS, 1):
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
            log = os.path.join(RUNS, "brk%02d.log" % i)
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B, "qucs/tests/" + SUITE], stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                    results.append((i, name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for fn, needle in tests:
                    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CACHE_DIR=os.path.join(RUNS, "cache"),
                               ASAN_OPTIONS="detect_leaks=0", QUCS_SETTINGS_DIR=os.path.join(RUNS, "settings"),
                               S4Q_workdir=os.path.join(RUNS, "s4q"), QUCS_TRASH_DIR=os.path.join(RUNS, "trash"),
                               QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="600000")
                    env.pop("QUCS_TEST_SLOW", None)
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", SUITE), fn], stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, timeout=600, env=env, cwd=RUNS)
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:34])
                        continue
                    output = r.stdout.decode("utf-8", "replace")
                    out.write(output)
                    if r.returncode == 0:
                        said.append("%s PASSED - NOT CAUGHT" % fn[:34])
                        continue
                    line = failing_line(output, fn)
                    if line is None:
                        said.append("%s crashed (exit %d), no failing line" % (fn[:34], r.returncode))
                    elif needle in line:
                        said.append("caught at %s" % line[-90:])
                    else:
                        said.append("CAUGHT ELSEWHERE, %s: %s" % (fn[:34], line[-120:]))
                results.append((i, name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for i, name, said in results:
        print("%2d %-58s %s" % (i, name[:58], said))
        if "caught at" not in said or "CAUGHT ELSEWHERE" in said or "NOT CAUGHT" in said or "crashed" in said or "hung" in said or "BUILD FAILED" in said:
            bad += 1
    print("\n%d of %d not caught where meant" % (bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
