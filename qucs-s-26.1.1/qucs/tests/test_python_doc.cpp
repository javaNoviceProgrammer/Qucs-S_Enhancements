/*
 * test_python_doc.cpp - the Python editor (pythondoc.h, pythonrun.h,
 * qucs_python.cpp): a .py file opens as a PythonDoc, with what a text
 * document does; a syntax error and a warning are found by the
 * interpreter, at their places, drawn, listed on the Problems tab and gone
 * when fixed; a check overtaken by an edit is not shown; with no
 * interpreter, said once; ruff or pyflakes when there; the toolbar shown
 * for a script and its interpreters; Run saves, runs and shows the output,
 * the exit code, the traceback's places as links; Stop; Run in Shell
 * leaves the variables; Return, Tab and Shift+Tab indent as Python does.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCompleter>
#include <QDockWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QToolBar>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "messagedock.h"
#include "misc.h"
#include "module.h"
#include "processconsole.h"
#include "pythondoc.h"
#include "pythonrun.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "settings.h"

using qucs_s::python::indentStep;
using qucs_s::python::nextIndent;

namespace {

// Puts back an environment variable as it was.
struct EnvironmentGuard {
    QByteArray name, was;
    bool had;
    explicit EnvironmentGuard(const char* n) : name(n), was(qgetenv(n)), had(qEnvironmentVariableIsSet(n)) {}
    ~EnvironmentGuard()
    {
        if (had) qputenv(name.constData(), was);
        else qunsetenv(name.constData());
    }
};

} // namespace

class TestPythonDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QString python;   // python3 on the PATH; empty: none (the checks and runs skipped)

    QString write(const QString& name, const QByteArray& bytes)
    {
        const QString path = dir.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(bytes);
        return path;
    }
    static QByteArray read(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    PythonDoc* open(const QString& path)
    {
        if (!app->gotoPage(path)) return nullptr;
        return qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget());
    }
    // Waits for \a py's check to answer (the one going, or the next).
    static bool checked(PythonDoc* py, int ms = 20000)
    {
        QSignalSpy spy(py, &PythonDoc::checkFinished);
        return spy.wait(ms);
    }
    // \a py's text set as typed (one edit) and its check waited for.
    static bool retyped(PythonDoc* py, const QString& text)
    {
        QSignalSpy spy(py, &PythonDoc::checkFinished);
        py->selectAll();
        py->insertPlainText(text);
        return spy.wait(20000);
    }
    // A key to the script - the list's keys to its list of completions
    // while that is shown. (Characters to the text, which keeps the
    // keyboard on a desktop while its list is shown; the offscreen
    // platform activates the list's window instead, and the completer
    // closes a list whose text lost the keyboard.)
    static void press(PythonDoc* py, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        static const QList<int> listKeys{Qt::Key_Return, Qt::Key_Enter, Qt::Key_Tab, Qt::Key_Escape, Qt::Key_Up, Qt::Key_Down};
        QWidget* target = py->completing() && listKeys.contains(key) ? static_cast<QWidget*>(py->completer()->popup())
                                                                       : static_cast<QWidget*>(py);
        QTest::keyClick(target, Qt::Key(key), modifiers);
    }
    static void typeText(PythonDoc* py, const QString& text)
    {
        for (const QChar c : text) {
            if (c == QLatin1Char('\n')) press(py, Qt::Key_Return);   // (no character keyClicks types)
            else QTest::keyClicks(py, QString(c));
        }
    }
    // The script in front, with the keyboard, its cursor at the end.
    bool focused(PythonDoc* py)
    {
        app->showDocument(py);
        app->activateWindow();
        py->setFocus();
        QTextCursor end(py->document());
        end.movePosition(QTextCursor::End);
        py->setTextCursor(end);
        return QTest::qWaitFor([py] { return py->hasFocus(); }, 5000);
    }
    QAction* pythonAction(const QString& name) const
    {
        for (QAction* a : app->findChildren<QAction*>())
            if (a->objectName() == name) return a;
        return nullptr;
    }
    QStringList problemRows() const
    {
        QStringList rows;
        for (int i = 0; i < app->messages()->problems->count(); ++i) rows << app->messages()->problems->item(i)->text();
        return rows;
    }
    void closeAll()
    {
        for (QucsDoc* doc : app->allDocuments()) doc->setDocChanged(false);
        app->closeAllFiles();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.PythonExecutable.clear();
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        python = QStandardPaths::findExecutable("python3");
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        QVERIFY(QTest::qWaitForWindowExposed(app));
    }

    // (A case that failed leaves no list open, nor its documents.)
    void cleanup()
    {
        for (QucsDoc* doc : app->allDocuments())
            if (auto* py = dynamic_cast<PythonDoc*>(doc)) py->completer()->popup()->hide();
        closeAll();
    }

    void cleanupTestCase()
    {
        closeAll();
        delete app;
        QucsMain = nullptr;
    }

    // The indentation and the check's answer, by themselves.
    void indentingAndReading()
    {
        QCOMPARE(indentStep("x = 1\n"), QString("    "));
        QCOMPARE(indentStep("def f():\n\treturn 1\n"), QString("\t"));
        QCOMPARE(indentStep("def f():\n  \n    return 1\n"), QString("    "));   // (a blank line says nothing)
        const QString four = "    ";
        QCOMPARE(nextIndent("def f():", four), four);
        QCOMPARE(nextIndent("    if x:  # yes", four), QString(8, ' '));
        QCOMPARE(nextIndent("    return x", four), QString());
        QCOMPARE(nextIndent("        pass", four), four);
        QCOMPARE(nextIndent("    raise ValueError('a:')", four), QString());
        QCOMPARE(nextIndent("    x = 'a:'", four), four);            // (a colon in a string)
        QCOMPARE(nextIndent("    print('#:')", four), four);          // (a # in a string is no comment)
        QCOMPARE(nextIndent("if s == '#':", four), four);
        QCOMPARE(nextIndent("returned = 1", four), QString());        // (a word that begins as return)
        QCOMPARE(nextIndent("\t\treturn", "\t"), QString("\t"));
        QCOMPARE(nextIndent("      ", four), QString(6, ' '));        // (the cursor in the indentation)

        QVERIFY(!qucs_s::python::readCheck("Traceback (most recent call last):").failure.isEmpty());
        const qucs_s::python::Check c = qucs_s::python::readCheck(
            R"({"python": "3.12.1", "checker": "ruff 0.6.9", "problems": [{"line": 3, "column": 5, "endLine": 3, "endColumn": 7,
                "error": false, "message": "`os` imported but unused", "code": "F401"}]})");
        QVERIFY(c.failure.isEmpty());
        QCOMPARE(c.python, QString("3.12.1"));
        QCOMPARE(c.problems.size(), 1);
        QCOMPARE(c.problems.first().endColumn, 7);
        QCOMPARE(c.problems.first().code, QString("F401"));
        // In the text's order.
        const qucs_s::python::Check two = qucs_s::python::readCheck(
            R"({"python": "3.14.0", "checker": "", "problems": [{"line": 4, "column": 0, "message": "b"}, {"line": 2, "column": 0, "message": "a"}]})");
        QCOMPARE(two.problems.first().message, QString("a"));
    }

    // A .py (and a .pyw) opens as a PythonDoc - from the Content panel too,
    // whatever the text editor of the settings - with its encoding and
    // line ends kept, no settings file beside it, and no document settings.
    void aScriptIsAPythonDoc()
    {
        const QString path = write("text/latin.py", "# caf\xe9\r\nx = 1\r\n");
        PythonDoc* py = open(path);
        QVERIFY(py != nullptr);
        QVERIFY(QucsApp::isTextDocument(py));
        QVERIFY(py->toPlainText().startsWith(QString::fromUtf8("# caf\xc3\xa9")));
        QTextCursor end(py->document());
        end.movePosition(QTextCursor::End);
        end.insertText("y = 2\n");
        QVERIFY(app->saveFile(py));
        QCOMPARE(read(path), QByteArray("# caf\xe9\r\nx = 1\r\ny = 2\r\n"));
        QVERIFY(!QFileInfo::exists(path + ".cfg"));   // (a text document's settings file: none)
        QVERIFY(QMetaObject::invokeMethod(app, "slotFileSettings"));
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QVERIFY2(app->statusBar()->currentMessage().contains("no document settings"), qPrintable(app->statusBar()->currentMessage()));

        QVERIFY(open(write("text/gui.pyw", "print(1)\n")) != nullptr);
#ifndef Q_OS_WIN
        // (Not the text editor of the settings, an outside one here.)
        const QString marker = dir.filePath("text/editor-asked");
        const QString editor = write("text/editor.sh", QStringLiteral("#!/bin/sh\ntouch '%1'\n").arg(marker).toUtf8());
        QFile::setPermissions(editor, QFile::permissions(editor) | QFileDevice::ExeOwner);
        const QString editorWas = QucsSettings.Editor;
        QucsSettings.Editor = editor;
#endif
        app->openFileFromProjectView(QFileInfo(write("text/panel.py", "print(2)\n")), QString());
        QVERIFY(qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget()) != nullptr);
#ifndef Q_OS_WIN
        QTest::qWait(500);
        QVERIFY(!QFileInfo::exists(marker));
        QucsSettings.Editor = editorWas;
#endif
        closeAll();
    }

    // A syntax error: at its line and column, to its end; underlined,
    // dotted, said over it, listed on the Problems tab - and gone when
    // fixed. A warning of the compiler, at its line. What checked it is
    // said.
    void errorsAndWarningsAreFound()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        // (A module of the script's folder named as one the check imports
        // is not the check's: it runs elsewhere.)
        write("check/json.py", "raise SystemExit('the folder of the script')\n");
        PythonDoc* py = open(write("check/broken.py", "x = 1\nprint(x 2)\n"));
        QVERIFY(py != nullptr);
        QVERIFY(py->checking() || py->checked());   // (at once, not after the delay of an edit)
        QVERIFY(checked(py));
        QVERIFY2(py->lastCheck().failure.isEmpty(), qPrintable(py->lastCheck().failure));
        QList<TextDoc::Diagnostic> found = py->diagnostics();
        QCOMPARE(found.size(), 1);
        QVERIFY(found.first().error);
        QCOMPARE(found.first().line, 2);
        QVERIFY(found.first().column > 0);
        QVERIFY(py->checkedBy().startsWith("Python 3."));
        const QStringList version = py->lastCheck().python.split('.');
        const bool spans = version.value(1).toInt() >= 10;   // (a Python that gives where it ends)
        const auto underline = [py] {
            for (const QTextEdit::ExtraSelection& s : py->extraSelections())
                if (s.format.underlineStyle() == QTextCharFormat::WaveUnderline) return s.cursor;
            return QTextCursor();
        };
        if (spans) {
            QCOMPARE(found.first().column, 7);
            QCOMPARE(found.first().endLine, 2);
            QCOMPARE(found.first().endColumn, 10);
            QVERIFY2(found.first().message.contains("comma"), qPrintable(found.first().message));
            // Drawn: a wavy line under "x 2", a dot in the margin.
            QCOMPARE(underline().selectedText(), QString("x 2"));
        }
        QVERIFY(!underline().isNull());
        QVERIFY(py->lineNumberAreaWidth() > QFontMetrics(py->font()).horizontalAdvance('9') + 3);
        QTextCursor at(py->document()->findBlockByNumber(1));
        QVERIFY(py->diagnosticsAtY(py->cursorRect(at).center().y()).startsWith("Error: "));
        // Listed.
        QVERIFY2(problemRows().size() == 1 && problemRows().first().startsWith("broken.py, line 2: "), qPrintable(problemRows().join(" | ")));
        QCOMPARE(app->messages()->textProblemsDocument(), static_cast<TextDoc*>(py));
        QCOMPARE(app->messages()->builderTabs->tabText(2), QString("Problems (1)"));

        // One Python gives no end of: to its line's end.
        QVERIFY(retyped(py, "x = 1\nprint((x, 2)\n"));
        QCOMPARE(py->diagnostics().size(), 1);
        if (spans) QVERIFY2(py->diagnostics().first().message.contains("never closed"), qPrintable(py->diagnostics().first().message));
        if (py->diagnostics().first().endLine == 0)
            QCOMPARE(underline().selectedText(), QString("print((x, 2)").mid(py->diagnostics().first().column - 1));

        // Fixed: nothing found.
        QVERIFY(retyped(py, "x = 1\nprint((x, 2))\n"));
        QVERIFY(py->diagnostics().isEmpty());
        QCOMPARE(problemRows(), QStringList{"No problems found in broken.py."});

        // The compiler's warnings: is with a literal, an escape that is none.
        QVERIFY(retyped(py, "x = 1\nif x is 1:\n    pass\ny = '\\d'\n"));
        found = py->diagnostics();
        QList<int> lines;
        for (const TextDoc::Diagnostic& d : std::as_const(found)) {
            QVERIFY(!d.error);
            lines << d.line;
        }
        QCOMPARE(lines, (QList<int>{2, 4}));
        QVERIFY2(py->checkedBy().contains("compiler alone") || !py->lastCheck().checker.isEmpty(), qPrintable(py->checkedBy()));
        // The diagnostics move with the text until the next check.
        QTextCursor top(py->document());
        top.insertText("\n");
        QCOMPARE(py->diagnostics().first().line, 3);
        closeAll();
    }

    // A check still going when the text changes is stopped: only the last
    // one answers. (A Python slowed down by a second, to be overtaken.)
    void aCheckOvertakenIsNotShown()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
#ifdef Q_OS_WIN
        QSKIP("a shell script stands in for a slow Python");
#endif
        const QString slow = write("slow/python", QStringLiteral("#!/bin/sh\nsleep 1\nexec '%1' \"$@\"\n").arg(python).toUtf8());
        QFile::setPermissions(slow, QFile::permissions(slow) | QFileDevice::ExeOwner);
        PythonDoc* py = open(write("slow/s.py", "x = 1\n"));
        QVERIFY(py != nullptr);
        QVERIFY(checked(py));
        QSignalSpy answers(py, &PythonDoc::checkFinished);
        py->setInterpreter(slow);   // checked again, a second slower
        QVERIFY(py->checking());
        QTest::qWait(200);
        py->selectAll();
        py->insertPlainText("def f(:\n");   // ... overtaken
        QVERIFY(!py->checking());
        QVERIFY(answers.wait(10000));
        QTest::qWait(1500);   // (no other answer comes)
        QCOMPARE(answers.count(), 1);
        QCOMPARE(py->diagnostics().size(), 1);
        QVERIFY(py->diagnostics().first().error);
        closeAll();
    }

    // No Python at the path chosen: said once in the status bar, and the
    // check's tooltip says why; nothing drawn.
    void noInterpreterIsSaidOnce()
    {
        PythonDoc* py = open(write("none/n.py", "def f(:\n"));
        QVERIFY(py != nullptr);
        if (py->checking()) checked(py);
        app->statusBar()->clearMessage();
        py->setInterpreter(dir.filePath("none/no-such-python"));
        QVERIFY(checked(py));
        QVERIFY2(py->lastCheck().failure.startsWith("No Python at"), qPrintable(py->lastCheck().failure));
        QVERIFY(py->diagnostics().isEmpty());
        QVERIFY2(app->statusBar()->currentMessage().startsWith("No Python at"), qPrintable(app->statusBar()->currentMessage()));
        QVERIFY(py->checkedBy().startsWith("No Python at"));
        QVERIFY2(problemRows().size() == 1 && problemRows().first().startsWith("n.py was not checked: No Python at"),
                 qPrintable(problemRows().join(" | ")));
        app->statusBar()->clearMessage();
        py->checkNow();
        QVERIFY(checked(py));
        QVERIFY(app->statusBar()->currentMessage().isEmpty());   // (once)
        closeAll();
    }

    // ruff, else pyflakes, when the interpreter has one (stand-ins of
    // theirs on PYTHONPATH): their findings as warnings, said what checked
    // it. None when the compiler finds a syntax error.
    void ruffOrPyflakesWhenThere()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        EnvironmentGuard pythonPath("PYTHONPATH"), path("PATH");
        qputenv("PATH", "/usr/bin:/bin");   // (no ruff of this machine's)
        const QString checkers = dir.filePath("checkers");
        write("checkers/pyflakes/__init__.py", "__version__ = '9.9.9'\n");
        write("checkers/pyflakes/api.py",
              "class M:\n"
              "    def __init__(self, lineno, col, message, args):\n"
              "        self.lineno, self.col, self.message, self.message_args = lineno, col, message, args\n"
              "def check(code, filename, reporter):\n"
              "    for n, line in enumerate(code.splitlines(), 1):\n"
              "        if 'import os' in line:\n"
              "            reporter.flake(M(n, line.index('os'), '%r imported but unused', ('os',)))\n"
              "    return 0\n");
        qputenv("PYTHONPATH", QFile::encodeName(checkers));
        PythonDoc* py = open(write("lint/l.py", "x = 1\nimport os\n"));
        QVERIFY(py != nullptr);
        if (!py->checked() || py->checking()) QVERIFY(checked(py));
        QCOMPARE(py->lastCheck().checker, QString("pyflakes 9.9.9"));
        QCOMPARE(py->diagnostics().size(), 1);
        QCOMPARE(py->diagnostics().first().line, 2);
        QCOMPARE(py->diagnostics().first().column, 8);   // (pyflakes' 7, from 0)
        QCOMPARE(py->diagnostics().first().message, QString("'os' imported but unused"));
        QVERIFY(!py->diagnostics().first().error);
        QVERIFY(py->checkedBy().endsWith(" and pyflakes 9.9.9"));

        // ruff first, given the script's path.
        write("checkers/ruff/__init__.py", "");
        write("checkers/ruff/__main__.py",
              "import sys, json, os\n"
              "if '--version' in sys.argv:\n"
              "    print('ruff 0.0.0-fake')\n"
              "    sys.exit(0)\n"
              "name = sys.argv[sys.argv.index('--stdin-filename') + 1]\n"
              "out = []\n"
              "for n, line in enumerate(sys.stdin.read().splitlines(), 1):\n"
              "    if 'import os' in line:\n"
              "        c = line.index('os') + 1\n"
              "        out.append({'code': 'F401', 'message': '`os` imported but unused in ' + os.path.basename(name),\n"
              "                    'location': {'row': n, 'column': c}, 'end_location': {'row': n, 'column': c + 2}})\n"
              "print(json.dumps(out))\n"
              "sys.exit(1 if out else 0)\n");
        py->checkNow();
        QVERIFY(checked(py));
        QCOMPARE(py->lastCheck().checker, QString("ruff 0.0.0-fake"));
        QCOMPARE(py->diagnostics().size(), 1);
        const TextDoc::Diagnostic d = py->diagnostics().first();
        QCOMPARE(d.message, QString("`os` imported but unused in l.py (F401)"));
        QCOMPARE(d.column, 8);
        QCOMPARE(d.endColumn, 10);

        // A syntax error: theirs not asked.
        QVERIFY(retyped(py, "import os\ndef f(:\n"));
        QCOMPARE(py->diagnostics().size(), 1);
        QVERIFY(py->diagnostics().first().error);
        QVERIFY(py->lastCheck().checker.isEmpty());
        closeAll();
    }

    // Each line's first message after its text, faintly, when asked for
    // (the toolbar's Messages at Line Ends, kept, for every script).
    void messagesAtLineEnds()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("ends/e.py", "x = 1\nprint((x\ny = 2\n"));
        QVERIFY(py != nullptr);
        if (!py->checked() || py->checking()) QVERIFY(checked(py));
        QVERIFY(!py->diagnostics().isEmpty());
        QTextCursor home(py->document()->lastBlock());
        py->setTextCursor(home);   // (the current line's shade elsewhere)
        const auto reddish = [py](int line) {
            const QImage image = py->viewport()->grab().toImage();
            QTextCursor end(py->document()->findBlockByNumber(line - 1));
            end.movePosition(QTextCursor::EndOfBlock);
            const QRect at = py->cursorRect(end);
            int count = 0;
            for (int y = at.top(); y <= at.bottom(); ++y)
                for (int x = at.right() + 4; x < image.width(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    count += c.red() > c.green() + 60 && c.red() > c.blue() + 60;
                }
            return count;
        };
        const int line = py->diagnostics().first().line;
        QAction* ends = nullptr;
        for (QAction* a : app->pythonToolbarWidget()->actions())
            if (a->objectName() == "pythonMessagesAtLineEnds") ends = a;
        QVERIFY(ends != nullptr);
        QVERIFY(!ends->isChecked());
        QCOMPARE(reddish(line), 0);
        ends->trigger();
        QVERIFY(py->diagnosticsAtLineEnds());
        QVERIFY(_settings::Get().item<bool>("PythonMessagesAtLineEnds"));
        QVERIFY(reddish(line) > 20);
        PythonDoc* other = open(write("ends/f.py", "print(1)\n"));   // opened with it on
        QVERIFY(other != nullptr && other->diagnosticsAtLineEnds());
        ends->trigger();
        QVERIFY(!other->diagnosticsAtLineEnds());
        app->showDocument(py);
        QVERIFY(!py->diagnosticsAtLineEnds());
        QCOMPARE(reddish(line), 0);
        closeAll();
    }

    // Return indents after a colon and takes a level away after return
    // (one undo takes it all back); Tab indents to the next level, a
    // selection's lines; Shift+Tab takes it away; a file indented with tabs
    // keeps tabs.
    void indentsAsPythonDoes()
    {
        PythonDoc::setCompleteAsYouType(false);   // (the keys to the text alone)
        const auto asYouType = qScopeGuard([] { PythonDoc::setCompleteAsYouType(true); });
        PythonDoc* py = open(write("indent/i.py", ""));
        QVERIFY(py != nullptr);
        py->setFocus();
        QTest::keyClicks(py, "def f():");
        QTest::keyClick(py, Qt::Key_Return);
        QCOMPARE(py->toPlainText(), QString("def f():\n    "));
        py->undo();
        QCOMPARE(py->toPlainText(), QString("def f():"));
        QTest::keyClick(py, Qt::Key_Return);
        QTest::keyClicks(py, "return 1");
        QTest::keyClick(py, Qt::Key_Return);
        QCOMPARE(py->toPlainText(), QString("def f():\n    return 1\n"));
        QTest::keyClick(py, Qt::Key_Tab);
        QCOMPARE(py->document()->lastBlock().text(), QString("    "));
        QTest::keyClick(py, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(py->document()->lastBlock().text(), QString());
        QTest::keyClicks(py, "ab");
        QTest::keyClick(py, Qt::Key_Tab);
        QCOMPARE(py->document()->lastBlock().text(), QString("ab  "));
        // The first two lines selected: shifted, and back.
        QTextCursor lines(py->document());
        lines.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
        lines.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        py->setTextCursor(lines);
        QTest::keyClick(py, Qt::Key_Tab);
        QCOMPARE(py->toPlainText(), QString("    def f():\n        return 1\nab  "));
        QTest::keyClick(py, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(py->toPlainText(), QString("def f():\n    return 1\nab  "));
        // Selected to the start of a line: that line is not one of them; an
        // empty line among them is left empty.
        py->selectAll();
        py->insertPlainText("a = 1\n\nb = 2\nc = 3\n");
        QTextCursor upTo(py->document());
        upTo.setPosition(py->document()->findBlockByNumber(3).position(), QTextCursor::KeepAnchor);
        py->setTextCursor(upTo);
        QTest::keyClick(py, Qt::Key_Tab);
        QCOMPARE(py->toPlainText(), QString("    a = 1\n\n    b = 2\nc = 3\n"));
        closeAll();

        PythonDoc* tabs = open(write("indent/t.py", "def g():\n\tpass\n"));
        QVERIFY(tabs != nullptr);
        QTextCursor end(tabs->document());
        end.movePosition(QTextCursor::End);
        tabs->setTextCursor(end);
        QTest::keyClicks(tabs, "def h():");
        QTest::keyClick(tabs, Qt::Key_Return);
        QCOMPARE(tabs->document()->lastBlock().text(), QString("\t"));
        // A selection replaced: indented as its start's line says.
        tabs->selectAll();
        QTest::keyClicks(tabs, "if x: pass");
        QTextCursor sel(tabs->document());
        sel.setPosition(5);   // " pass" selected
        sel.setPosition(10, QTextCursor::KeepAnchor);
        tabs->setTextCursor(sel);
        QTest::keyClick(tabs, Qt::Key_Return);
        QCOMPARE(tabs->toPlainText(), QString("if x:\n    "));
        closeAll();
    }

    // The toolbar: shown while a script is in front, hidden for a
    // schematic; hidden from the Toolbars menu, it stays so. Its
    // interpreters: a virtual environment beside the script first (the
    // default), those on the PATH, Browse; one chosen is the script's.
    void theToolbarFollowsTheDocument()
    {
        QToolBar* bar = app->pythonToolbarWidget();
        QVERIFY(bar != nullptr && app->toolbars().contains(bar));
        PythonDoc* py = open(write("bar/b.py", "print(1)\n"));
        QVERIFY(py != nullptr);
        QVERIFY(bar->isVisible());
        app->slotFileNew();   // a schematic in front
        QVERIFY(!bar->isVisible());
        app->showDocument(py);
        QVERIFY(bar->isVisible());
        bar->toggleViewAction()->trigger();   // hidden, as from the menu
        QVERIFY(!bar->isVisible());
        QVERIFY(!_settings::Get().item<bool>("PythonToolbar"));
        app->slotFileNew();
        app->showDocument(py);
        QVERIFY(!bar->isVisible());   // (kept)
        bar->toggleViewAction()->trigger();
        QVERIFY(bar->isVisible());
        QVERIFY(_settings::Get().item<bool>("PythonToolbar"));

        QComboBox* list = app->pythonInterpreterList();
        QCOMPARE(list->itemText(list->count() - 1), QString("Browse..."));
        // The one Application Settings name, and those chosen with Browse.
        const QString chosen = write("bar/other/python3.99", "#!/bin/sh\n");
        QFile::setPermissions(chosen, QFile::permissions(chosen) | QFileDevice::ExeOwner);
        _settings::Get().setItem<QStringList>("PythonInterpreters", {chosen});
        if (!python.isEmpty()) QucsSettings.PythonExecutable = python;
        app->slotFileNew();
        app->showDocument(py);
        if (!python.isEmpty()) {
            QCOMPARE(list->itemData(0).toString(), QFileInfo(python).absoluteFilePath());
            QVERIFY2(list->itemText(0).endsWith("(Application Settings)"), qPrintable(list->itemText(0)));
        }
#ifndef Q_OS_WIN
        QCOMPARE(list->itemText(list->count() - 2), QString("python3.99 (chosen)"));
#endif
        QucsSettings.PythonExecutable.clear();
        _settings::Get().setItem<QStringList>("PythonInterpreters", {});
        app->slotFileNew();
        app->showDocument(py);
        QCOMPARE(list->currentData().toString(), py->interpreter());
        if (!python.isEmpty()) {
            QVERIFY(list->findData(QFileInfo(python).absoluteFilePath()) >= 0);
#ifndef Q_OS_WIN
            const QString venv = dir.filePath("bar/env/.venv/bin/python");
            QDir().mkpath(QFileInfo(venv).absolutePath());
            QVERIFY(QFile::link(python, venv));
            PythonDoc* inEnv = open(write("bar/env/e.py", "print(2)\n"));
            QVERIFY(inEnv != nullptr);
            QCOMPARE(inEnv->interpreter(), venv);
            QCOMPARE(list->currentIndex(), 0);
            QVERIFY(list->currentText().contains(".venv beside the script"));
            // Another chosen: the script's from now on.
            const int other = list->findData(QFileInfo(python).absoluteFilePath());
            list->setCurrentIndex(other);
            emit list->activated(other);
            QCOMPARE(inEnv->interpreter(), QFileInfo(python).absoluteFilePath());
            QVERIFY(inEnv->interpreterChosen());
#endif
        }
        closeAll();
        QVERIFY(!bar->isVisible() || qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget()) != nullptr);
    }

    // Run (F2, Simulate's key, with a script in front): saved first, run in
    // its folder, its output and its exit code; a traceback's place a link
    // that goes there; input() gets the end of the file; Stop ends one
    // that does not; Stop only while one runs.
    void runShowsTheOutputAndTheExitCode()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonRunConsole* console = app->pythonRunConsole();
        QVERIFY(console != nullptr);
        QAction* stop = nullptr;
        for (QAction* a : app->pythonToolbarWidget()->actions())
            if (a->objectName() == "pythonStop") stop = a;
        QVERIFY(stop != nullptr && !stop->isEnabled());

        const QString path = write("run/r.py", "import os, sys\n");
        PythonDoc* py = open(path);
        QVERIFY(py != nullptr);
        QTextCursor end(py->document());
        end.movePosition(QTextCursor::End);
        end.insertText("print('hello from', os.path.basename(os.getcwd()))\nsys.exit(3)\n");
        QSignalSpy done(console, &PythonRunConsole::finished);
        app->simulate->trigger();   // F2
        QVERIFY(done.wait(20000));
        QVERIFY(read(path).contains("sys.exit(3)"));   // saved first
        QCOMPARE(console->exitCode(), 3);
        QVERIFY2(console->outputText().contains("hello from run"), qPrintable(console->outputText()));
        QVERIFY2(console->outputText().contains("Exit code 3 after"), qPrintable(console->outputText()));
        QVERIFY(app->pythonRunDockWidget()->isVisible());
        QVERIFY(!stop->isEnabled());

        // A traceback: its place a link.
        PythonDoc* boom = open(write("run/boom.py", "x = 1\n\ndef f():\n    raise ValueError('no')\n\nf()\n"));
        QVERIFY(boom != nullptr);
        QVERIFY(app->runPython(boom));
        QVERIFY(done.wait(20000));
        QCOMPARE(console->exitCode(), 1);
        QVERIFY(console->outputText().contains("ValueError: no"));
        QPlainTextEdit* out = console->outputView();
        QTextCursor link;
        for (QTextBlock b = out->document()->begin(); b.isValid() && link.isNull(); b = b.next())
            for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().anchorHref().endsWith("boom.py#4")) {
                    link = QTextCursor(out->document());
                    link.setPosition(it.fragment().position() + 1);
                    break;
                }
        QVERIFY2(!link.isNull(), qPrintable(console->outputText()));
        app->showDocument(py);   // (elsewhere, to be brought back)
        out->ensureCursorVisible();
        const QPoint at = out->cursorRect(link).center();
        QCOMPARE(out->anchorAt(at).section('#', -1), QString("4"));
        QTest::mouseClick(out->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(boom));
        QCOMPARE(boom->textCursor().blockNumber(), 3);

        // input(): the end of the file, not a wait.
        PythonDoc* ask = open(write("run/ask.py", "try:\n    input()\nexcept EOFError:\n    print('no input')\n"));
        QVERIFY(app->runPython(ask));
        QVERIFY(done.wait(20000));
        QCOMPARE(console->exitCode(), 0);
        QVERIFY(console->outputText().contains("no input"));

        // A progress line written over (carriage returns): as it ends.
        PythonDoc* progress = open(write("run/progress.py", "import sys\nfor p in (10, 50, 100):\n    sys.stdout.write(f'\\r{p}%')\nprint()\nprint('done')\n"));
        QVERIFY(app->runPython(progress));
        QVERIFY(done.wait(20000));
        QVERIFY2(console->outputText().contains("\n100%\ndone\n"), qPrintable(console->outputText()));
        QVERIFY(!console->outputText().contains("10%"));

        // A run while one goes: that one ended, this one run.
        const QString stillHere = dir.filePath("run/still-here");
        PythonDoc* first = open(write("run/long.py", QStringLiteral("import time\nprint('long', flush=True)\ntime.sleep(1.5)\n"
                                                                    "open(%1, 'w').close()\ntime.sleep(60)\n")
                                                         .arg(ProcessConsole::quotedForPython(stillHere)).toUtf8()));
        QVERIFY(app->runPython(first));
        QTRY_VERIFY_WITH_TIMEOUT(console->outputText().contains("long"), 20000);
        QVERIFY(app->runPython(ask));
        QVERIFY(done.wait(20000));
        QCOMPARE(console->script(), ask->getDocName());
        QVERIFY(console->outputText().contains("no input"));
        QVERIFY(!console->outputText().contains("long"));
        QTest::qWait(2000);
        QVERIFY(!QFileInfo::exists(stillHere));   // (it was ended)

        // Stop.
        PythonDoc* sleeper = open(write("run/sleep.py", "import time\nprint('start', flush=True)\ntime.sleep(60)\n"));
        QVERIFY(app->runPython(sleeper));
        QTRY_VERIFY_WITH_TIMEOUT(console->outputText().contains("start"), 20000);
        QVERIFY(stop->isEnabled());
        QElapsedTimer clock;
        clock.start();
        stop->trigger();
        QVERIFY(done.wait(10000));
        QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));   // (asked, not killed later)
        QVERIFY(console->wasStopped());
        QCOMPARE(console->exitCode(), -1);
        QVERIFY(console->outputText().contains("Stopped after"));
        QVERIFY(!stop->isEnabled());
        closeAll();
    }

    // Run in Shell: in the Python Shell, in the script's folder, its
    // variables there after it.
    void runInShellLeavesTheVariables()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
#ifdef Q_OS_WIN
        QSKIP("the shell's console is a terminal's on Unix");
#endif
        PythonDoc* py = open(write("shell/sh.py", "answer = 42\nprint('ran', __file__.endswith('sh.py'))\n"));
        QVERIFY(py != nullptr);
        QVERIFY(app->runPythonInShell(py));
        ProcessConsole* shell = app->pythonConsole();
        QVERIFY(app->pythonDockWidget()->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("ran True"), 20000);
        shell->sendLine("print(answer * 2)");
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("84"), 20000);
        shell->sendLine("import os; print('cwd=' + os.path.realpath(os.getcwd()))");
        const QString folder = QFileInfo(dir.filePath("shell")).canonicalFilePath();
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("cwd=" + folder), 20000);
        shell->stop();
        closeAll();
    }

    // The completer by itself: Python's names, the script's, a module's
    // members (a standard one's, one beside the script read), the modules
    // after import, the names that follow a name and a dot elsewhere,
    // nothing after a number's dot.
    void theCompleterAnswers()
    {
        QCOMPARE(qucs_s::python::wordStart("x = math.sq"), 9);
        QCOMPARE(qucs_s::python::wordStart("x = "), 4);
        QVERIFY(qucs_s::python::inStringOrComment("s = 'pri"));
        QVERIFY(qucs_s::python::inStringOrComment("x = 1  # pri"));
        QVERIFY(!qucs_s::python::inStringOrComment("s = 'a#b' + pri"));
        QVERIFY(!qucs_s::python::inStringOrComment("s = 'it\\'s' + pri"));
        const qucs_s::python::Completions c = qucs_s::python::readCompletions(
            R"js({"id": 7, "engine": "jedi 1.0", "items": [{"name": "sqrt", "type": "function", "description": "def sqrt(x)"}]})js");
        QCOMPARE(c.id, 7);
        QCOMPARE(c.engine, QString("jedi 1.0"));
        QCOMPARE(c.items.size(), 1);
        QCOMPARE(c.items.first().description, QString("def sqrt(x)"));
        QCOMPARE(qucs_s::python::readCompletions("not json").id, -1);

        if (python.isEmpty()) QSKIP("no python3 here");
        write("complete/mymod.py", "def helper():\n    pass\n\nclass Thing:\n    pass\n");
        const QString script = dir.filePath("complete/s.py");
        QProcess p;
        p.setWorkingDirectory(dir.path());
        p.start(python, {"-u", "-c", qucs_s::python::completerProgram()});
        QVERIFY(p.waitForStarted(10000));
        int id = 0;
        const auto ask = [&](const QString& source) {
            const QStringList lines = source.split('\n');
            p.write(QJsonDocument(QJsonObject{{"id", ++id}, {"source", source}, {"line", lines.size()}, {"column", lines.last().size()},
                                              {"path", script}})
                        .toJson(QJsonDocument::Compact) + '\n');
            QByteArray line;
            while (!line.endsWith('\n') && p.waitForReadyRead(20000)) line += p.readLine();
            const qucs_s::python::Completions answer = qucs_s::python::readCompletions(line);
            QStringList names;
            for (const qucs_s::python::Completion& item : answer.items) names << item.name + ":" + item.type;
            return answer.id == id ? names : QStringList{"(no answer)"};
        };
        QVERIFY(ask("pri").contains("print:function"));
        QVERIFY(ask("wh").contains("while:keyword"));
        QCOMPARE(ask("alpha_value = 1\nalp"), QStringList{"alpha_value:statement"});
        QVERIFY(ask("import math\nmath.sq").contains("sqrt:function"));
        QVERIFY(ask("import collections\ncollections.Ord").contains("OrderedDict:class"));
        QVERIFY(ask("import ma").contains("math:module"));
        QCOMPARE(ask("import mymod\nmymod.he"), QStringList{"helper:function"});
        QCOMPARE(ask("from mymod import Th"), QStringList{"Thing:class"});
        QCOMPARE(ask("x = obj.first\ny = obj.second\nobj."), (QStringList{"first:statement", "second:statement"}));
        QCOMPARE(ask("1."), QStringList());
        p.closeWriteChannel();
        QVERIFY(p.waitForFinished(10000));
    }

    // As one types: the list after two letters of a name; Return takes the
    // word chosen (no new line); a dot lists a module's members, typing
    // narrows them, Tab takes one; Escape closes it, a character no name
    // has closes it. Nothing in a string, a comment, after one letter, or
    // after a number's dot.
    void completesAsYouType()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        QVERIFY(PythonDoc::completeAsYouType());
        PythonDoc* py = open(write("complete/c.py", "alpha_value = 1\n"));
        QVERIFY(py != nullptr);
        QVERIFY(focused(py));
        typeText(py, "alp");
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        QCOMPARE(py->completionNames(), QStringList{"alpha_value"});
        QVERIFY(py->completer()->popup()->currentIndex().isValid());
        press(py, Qt::Key_Return);
        QVERIFY(!py->completing());
        QCOMPARE(py->toPlainText(), QString("alpha_value = 1\nalpha_value"));
        press(py, Qt::Key_Return);   // (the list closed: a new line)
        QCOMPARE(py->toPlainText(), QString("alpha_value = 1\nalpha_value\n"));

        typeText(py, "import math\nmath.");
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        QVERIFY(py->completionNames().contains("sqrt"));
        const int tall = py->completer()->popup()->height();
        typeText(py, "sq");
        QVERIFY2(py->completionNames() == QStringList{"sqrt"}, qPrintable(py->completionNames().join(", ") + " | " + py->document()->lastBlock().text()));
        QVERIFY(py->completer()->popup()->height() < tall);   // (as long as its words)
        press(py, Qt::Key_Tab);
        QVERIFY(py->document()->lastBlock().text() == "math.sqrt");

        press(py, Qt::Key_Return);
        typeText(py, "pr");
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        QVERIFY(py->completionNames().contains("print"));
        press(py, Qt::Key_Escape);
        QVERIFY(!py->completing());
        QCOMPARE(py->document()->lastBlock().text(), QString("pr"));
        typeText(py, "i");
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        typeText(py, "(");
        QVERIFY(!py->completing());
        QCOMPARE(py->document()->lastBlock().text(), QString("pri("));

        // Nothing offered here.
        QSignalSpy answers(py, &PythonDoc::completionsAnswered);
        // ("pr" then Return before the moment is up: the line is new.)
        for (const char* text : {"\ns = 'pri", "\n# pri", "\nw", "\nx = 1.", "\nx = 12", "\npr\n"}) {
            typeText(py, text);
            QTest::qWait(PythonDoc::kCompleteDelay + 400);
            QVERIFY2(!py->completing(), text);
        }
        QCOMPARE(answers.count(), 0);   // (not asked)
        // A word typed whole: nothing to add, no list.
        typeText(py, "\nwhile");
        QVERIFY(answers.wait(15000));
        QVERIFY(!py->completing());
        closeAll();
    }

    // Ctrl+Space asks - after a single letter too, with Complete as You Type
    // off -; so does Show Completions in Simulation > Python.
    void askedForWithControlSpace()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        QAction* asYouType = pythonAction("pythonCompleteAsYouType");
        QAction* show = pythonAction("pythonComplete");
        QVERIFY(asYouType != nullptr && show != nullptr);
        QVERIFY(asYouType->isChecked());
        asYouType->trigger();
        QVERIFY(!PythonDoc::completeAsYouType());
        PythonDoc* py = open(write("complete/ask.py", ""));
        QVERIFY(py != nullptr);
        QVERIFY(focused(py));
        typeText(py, "pri");
        QTest::qWait(PythonDoc::kCompleteDelay + 400);
        QVERIFY(!py->completing());
        QTest::keyClick(py, Qt::Key_Space, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        QVERIFY(py->completionNames().contains("print"));
        press(py, Qt::Key_Escape);
        typeText(py, "\nw");
        show->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        QVERIFY(py->completionNames().contains("while") && py->completionNames().contains("with"));
        press(py, Qt::Key_Escape);
        QVERIFY(py->completedBy().startsWith("Completed with the script's names"));
        // Another Python: the completer is its (none at that path: no list).
        py->setInterpreter(dir.filePath("complete/no-such-python"));
        show->trigger();
        QTest::qWait(1000);
        QVERIFY(!py->completing());
        py->setInterpreter(QString());
        show->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        press(py, Qt::Key_Escape);
        asYouType->trigger();
        QVERIFY(PythonDoc::completeAsYouType());
        closeAll();
    }

    // jedi when the interpreter has it (a stand-in on PYTHONPATH): its
    // words for the place asked about, said what completed it. An answer
    // for a word left behind is not shown.
    void jediWhenThere()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        EnvironmentGuard pythonPath("PYTHONPATH");
        write("jedi/jedi/__init__.py",
              "import os, time\n"
              "__version__ = '9.9.9'\n"
              "class C:\n"
              "    def __init__(self, name, type):\n"
              "        self.name = self.name_with_symbols = name\n"
              "        self.type, self.description = type, type + ' ' + name\n"
              "class Script:\n"
              "    def __init__(self, code=None, path=None):\n"
              "        self.code, self.path = code, path\n"
              "    def complete(self, line, column):\n"
              "        if 'slow' in self.code:\n"
              "            time.sleep(1.0)\n"
              "        return [C('fa_L%dC%d' % (line, column), 'statement'), C('fake_from_jedi', 'function'),\n"
              "                C('fa_' + os.path.basename(self.path or ''), 'module')]\n");
        qputenv("PYTHONPATH", QFile::encodeName(dir.filePath("jedi")));
        PythonDoc* py = open(write("complete/j.py", "x = 1\n"));
        QVERIFY(py != nullptr);
        QVERIFY(focused(py));
        typeText(py, "fa");
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        QCOMPARE(py->completionNames(), (QStringList{"fa_L2C2", "fake_from_jedi", "fa_j.py"}));
        QCOMPARE(py->completionEngine(), QString("jedi 9.9.9"));
        QCOMPARE(py->completedBy(), QString("Completed by jedi 9.9.9."));
        QCOMPARE(py->completer()->popup()->model()->index(1, 0).data(Qt::ToolTipRole).toString(), QString("function fake_from_jedi"));
        press(py, Qt::Key_Escape);

        // Slow: the cursor on another line before it answers.
        py->insertPlainText("\nslow = 1\n");
        QSignalSpy answered(py, &PythonDoc::completionsAnswered);
        typeText(py, "fa");
        QTest::qWait(PythonDoc::kCompleteDelay + 200);   // (asked)
        press(py, Qt::Key_Return);
        QVERIFY(answered.wait(10000));
        QVERIFY(!py->completing());
        closeAll();
    }

    // Claude's: the actions in Simulation > Python, which trigger_action
    // runs; a script read and edited as any text document.
    void claudeRunsAndChecksAScript()
    {
        auto* control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const QString actions = QucsControl::textOf(control->callNow("list_actions", {}, 20000));
        for (const char* path : {"Simulation > Python > Run", "Simulation > Python > Stop", "Simulation > Python > Run in Shell",
                                 "Simulation > Python > Check", "Simulation > Python > Messages at Line Ends",
                                 "Simulation > Python > Show Completions", "Simulation > Python > Complete as You Type"})
            QVERIFY2(actions.contains(path), path);
        if (python.isEmpty()) QSKIP("no python3 here: not run");
        PythonDoc* py = open(write("claude/c.py", "print('from claude')\n"));
        QVERIFY(py != nullptr);
        QSignalSpy done(app->pythonRunConsole(), &PythonRunConsole::finished);
        QJsonObject r = control->callNow("trigger_action", {{"action", "Simulation > Python > Run"}}, 20000);
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        QVERIFY(done.count() > 0 || done.wait(20000));
        QVERIFY(app->pythonRunConsole()->outputText().contains("from claude"));
        r = control->callNow("edit_text", {{"path", py->getDocName()},
                                           {"edits", QJsonArray{QJsonObject{{"find", "print('from claude')"}, {"replace", "print('from claude'"}}}}},
                             20000);
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        QVERIFY(checked(py));
        QVERIFY(!py->diagnostics().isEmpty() && py->diagnostics().first().error);
        closeAll();
    }

    // The Problems tab's rows go to their line; they go with their script.
    void aProblemGoesToItsLine()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("where/w.py", "x = 1\ny = 2\nz = (\n"));
        QVERIFY(py != nullptr);
        if (!py->checked() || py->checking()) QVERIFY(checked(py));
        QCOMPARE(app->messages()->textProblems().size(), 1);
        py->setTextCursor(QTextCursor(py->document()));
        QListWidget* rows = app->messages()->problems;
        rows->setCurrentRow(0);
        emit rows->itemClicked(rows->item(0));
        QCOMPARE(py->textCursor().blockNumber() + 1, py->diagnostics().first().line);
        // Another script in front: its problems; back: these again.
        PythonDoc* fine = open(write("where/fine.py", "x = 1\n"));
        QVERIFY(fine != nullptr);
        if (!fine->checked() || fine->checking()) QVERIFY(checked(fine));
        QCOMPARE(app->messages()->textProblemsDocument(), static_cast<TextDoc*>(fine));
        app->showDocument(py);
        QCOMPARE(app->messages()->textProblemsDocument(), static_cast<TextDoc*>(py));
        QCOMPARE(app->messages()->textProblems().size(), 1);
        // A schematic's check takes the tab.
        app->messages()->showProblems(nullptr, {}, false);
        QVERIFY(app->messages()->textProblemsDocument() == nullptr);
        QVERIFY(app->messages()->textProblems().isEmpty());
        app->showDocument(py);
        // Checked again when it changes on disk and is read again.
        write("where/w.py", "x = 1\n");
        QSignalSpy spy(py, &PythonDoc::checkFinished);
        QVERIFY(app->reloadDocument(py));
        QVERIFY(spy.count() > 0 || spy.wait(20000));
        QVERIFY(py->diagnostics().isEmpty());
        closeAll();
        QVERIFY(app->messages()->textProblemsDocument() == nullptr);
        QCOMPARE(rows->count(), 0);
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication application(one, argv);
    TestPythonDoc test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_python_doc.moc"
