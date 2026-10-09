/*
 * test_python_extras.cpp - how a Python script is run, debugged and tidied
 * (pythondoc.h, pythonassist.cpp, pythonrun.h, qucs_python.cpp, python/):
 * Run Settings - arguments, working folder, environment, .env file - for
 * Run and Debug; Interrupt; Debug Library Code; Quick Fix (the light bulb):
 * ruff's fix, an import, a problem ignored on its line; Organize Imports,
 * Format Selection, Format on Save.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QToolBar>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "pythondoc.h"
#include "pythonrun.h"
#include "qucs.h"
#include "settings.h"

class TestPythonExtras : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QString python;   // python3 on the PATH; empty: none (what runs Python skipped)
    QByteArray pythonPath;   // PYTHONPATH: jedi hidden, the stand-in of ruff's first

    QString write(const QString& name, const QByteArray& bytes)
    {
        const QString path = dir.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(bytes);
        return path;
    }
    QByteArray read(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    PythonDoc* open(const QString& path)
    {
        if (!app->gotoPage(path)) return nullptr;
        return qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget());
    }
    bool focused(PythonDoc* py)
    {
        app->showDocument(py);
        app->activateWindow();
        py->setFocus();
        return QTest::qWaitFor([py] { return py->hasFocus(); }, 5000);
    }
    static void place(PythonDoc* py, int line, int column)
    {
        const QTextBlock block = py->document()->findBlockByNumber(line - 1);
        QTextCursor at(block);
        at.setPosition(block.position() + column);
        py->setTextCursor(at);
    }
    static QString lineText(PythonDoc* py, int line) { return py->document()->findBlockByNumber(line - 1).text(); }
    QAction* pythonAction(const QString& name) const
    {
        for (QAction* a : app->findChildren<QAction*>())
            if (a->objectName() == name) return a;
        return nullptr;
    }
    void closeAll()
    {
        for (QucsDoc* doc : app->allDocuments()) doc->setDocChanged(false);
        app->closeAllFiles();
    }
    // Checked as it is (the check's answer of the text as it is now).
    static bool checked(PythonDoc* py)
    {
        QSignalSpy done(py, &PythonDoc::checkFinished);
        return done.wait(20000);
    }
    static QWidget* marginOf(PythonDoc* py)
    {
        for (QObject* child : py->children())
            if (auto* w = qobject_cast<QWidget*>(child); w != nullptr && w != py->viewport() && w->metaObject() == &QWidget::staticMetaObject
                                                         && w->x() < py->viewport()->x() && w->height() > 50)
                return w;
        return nullptr;
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
        // jedi hidden; a stand-in of ruff's: check (F401 with its fix, F821),
        // format (x=1 as x = 1, the lines of --range alone), the imports
        // sorted (check --select I --fix).
        write("tools/jedi/__init__.py", "raise ImportError('hidden for the test')\n");
        write("tools/ruff/__init__.py", "");
        write("tools/ruff/__main__.py",
              "import sys, json, re\n"
              "a = sys.argv[1:]\n"
              "if '--version' in a:\n"
              "    print('ruff 9.9.9-fake'); sys.exit(0)\n"
              "src = sys.stdin.read()\n"
              "lines = src.split('\\n')\n"
              "if a[0] == 'format':\n"
              "    first, last = 1, len(lines)\n"
              "    if '--range' in a:\n"
              "        first, last = (int(n) for n in a[a.index('--range') + 1].split('-'))\n"
              "    for k in range(first - 1, min(last, len(lines))):\n"
              "        lines[k] = re.sub(r'(\\w)=(\\w)', r'\\1 = \\2', lines[k])\n"
              "    sys.stdout.write('\\n'.join(lines)); sys.exit(0)\n"
              "if '--select' in a and a[a.index('--select') + 1] == 'I':\n"
              "    k = 0\n"
              "    while k < len(lines) and lines[k].startswith(('import ', 'from ')):\n"
              "        k += 1\n"
              "    sys.stdout.write('\\n'.join(sorted(lines[:k]) + lines[k:])); sys.exit(0)\n"
              "if '--fix' in a:\n"
              "    sys.stdout.write(src); sys.exit(0)\n"
              "out = []\n"
              "for n, line in enumerate(lines, 1):\n"
              "    m = re.match(r'import (\\w+)$', line)\n"
              "    if m and not re.search(r'\\b%s\\.' % m.group(1), src):\n"
              "        out.append({'code': 'F401', 'message': '`%s` imported but unused' % m.group(1),\n"
              "                    'location': {'row': n, 'column': 8}, 'end_location': {'row': n, 'column': 8 + len(m.group(1))},\n"
              "                    'fix': {'message': 'Remove unused import: `%s`' % m.group(1), 'applicability': 'safe',\n"
              "                            'edits': [{'content': '', 'location': {'row': n, 'column': 1}, 'end_location': {'row': n + 1, 'column': 1}}]}})\n"
              "    for m in re.finditer(r'\\b(sqrt)\\(', line):\n"
              "        if 'import sqrt' not in src and 'def sqrt' not in src:\n"
              "            out.append({'code': 'F821', 'message': 'Undefined name `sqrt`', 'location': {'row': n, 'column': m.start() + 1},\n"
              "                        'end_location': {'row': n, 'column': m.end()}, 'fix': None})\n"
              "print(json.dumps(out))\n"
              "sys.exit(1 if out else 0)\n");
        pythonPath = QFile::encodeName(dir.filePath("tools"));
        if (const QByteArray had = qgetenv("PYTHONPATH"); !had.isEmpty()) pythonPath += QDir::listSeparator().toLatin1() + had;
        qputenv("PYTHONPATH", pythonPath);
        PythonDoc::setAutoClose(false);
        PythonDoc::setTypeChecker("off");
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        QVERIFY(QTest::qWaitForWindowExposed(app));
    }

    void cleanup() { closeAll(); }

    void cleanupTestCase()
    {
        closeAll();
        delete app;
        QucsMain = nullptr;
    }

    // ------------------------------------------------------------------
    // Run Settings

    // Arguments split as a shell splits them; a .env file's lines read -
    // export, quotes, comments, ${NAME}; a folder or a .env file not there
    // said, nothing run.
    void argumentsAndEnvironmentAreRead()
    {
        QCOMPARE(qucs_s::python::splitArguments("--points 101 \"a file.dat\" 'x y' z\\ w"),
                 (QStringList{"--points", "101", "a file.dat", "x y", "z w"}));
        QProcessEnvironment base;
        base.insert("HOME_DIR", "/h");
        const auto read = qucs_s::python::readEnvironment("# a comment\n"
                                                          "export A=1\n"
                                                          "B = \"two words\"\n"
                                                          "C='${HOME_DIR} as is'\n"
                                                          "D=${HOME_DIR}/data # after\n"
                                                          "E=${A}${A}\n"
                                                          "not a line\n",
                                                          base);
        QCOMPARE(read.size(), 5);
        QCOMPARE(read.at(0), (std::pair<QString, QString>{"A", "1"}));
        QCOMPARE(read.at(1).second, QString("two words"));
        QCOMPARE(read.at(2).second, QString("${HOME_DIR} as is"));
        QCOMPARE(read.at(3).second, QString("/h/data"));
        QCOMPARE(read.at(4).second, QString("11"));
        const QString script = write("plan/p.py", "print(1)\n");
        qucs_s::python::setRunSettingsFor(script, {"a b", "missing", "", ""});
        QVERIFY(qucs_s::python::runPlanFor(script).failure.contains("is not there"));
        qucs_s::python::setRunSettingsFor(script, {"a b", "", "", "nope.env"});
        QVERIFY(qucs_s::python::runPlanFor(script).failure.contains("could not be read"));
        qucs_s::python::setRunSettingsFor(script, {});
        QVERIFY(qucs_s::python::runSettingsFor(script).isEmpty());
        QVERIFY(qucs_s::python::runPlanFor(script).failure.isEmpty());
    }

    // Set in its dialog: Run gives the script its arguments, its folder,
    // its variables (the .env file's, then the lines'); Debug too; the Run
    // button says so; a .env file not there: not run, said.
    void runSettingsForRunAndDebug()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        write("rs/work/marker.txt", "here\n");
        write("rs/settings.env", "export LEVEL=from-file\nUNIT=V\n");
        const QString path = write("rs/show.py", "import os, sys\n"
                                                 "print('args', sys.argv[1:])\n"
                                                 "print('cwd', os.path.basename(os.getcwd()), os.path.exists('marker.txt'))\n"
                                                 "print('env', os.environ.get('LEVEL'), os.environ.get('UNIT'), os.environ.get('GAIN'))\n");
        PythonDoc* py = open(path);
        QVERIFY(py != nullptr && focused(py));
        QAction* settings = pythonAction("pythonRunSettings");
        QVERIFY(settings != nullptr && settings->isEnabled());
        // The dialog: filled, accepted.
        QTimer::singleShot(200, this, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr && dialog->objectName() == "pythonRunSettingsDialog");
            dialog->findChild<QLineEdit*>("runArguments")->setText("--points 101 \"a file.dat\"");
            dialog->findChild<QLineEdit*>("runFolder")->setText("work");
            dialog->findChild<QPlainTextEdit*>("runEnvironment")->setPlainText("GAIN=${LEVEL}-3\nLEVEL=line");
            dialog->findChild<QLineEdit*>("runEnvFile")->setText("settings.env");
            dialog->accept();
        });
        settings->trigger();
        const qucs_s::python::RunSettings kept = qucs_s::python::runSettingsFor(path);
        QCOMPARE(kept.arguments, QString("--points 101 \"a file.dat\""));
        QCOMPARE(kept.folder, QString("work"));
        QCOMPARE(kept.envFile, QString("settings.env"));
        QVERIFY(pythonAction("pythonRun")->toolTip().contains("Arguments: --points 101"));

        PythonRunConsole* run = app->pythonRunConsole();
        QSignalSpy done(run, &PythonRunConsole::finished);
        QVERIFY(app->runPython(py));
        QVERIFY(done.wait(20000));
        const QString out = run->outputText();
        QVERIFY2(out.contains("args ['--points', '101', 'a file.dat']"), qPrintable(out));
        QVERIFY2(out.contains("cwd work True"), qPrintable(out));
        QVERIFY2(out.contains("env line V from-file-3"), qPrintable(out));   // (the file's, then the lines')
        QVERIFY2(out.contains("show.py --points 101 'a file.dat'"), qPrintable(out));   // (said as a shell has it)
        // Debug: the same.
        QSignalSpy finished(run, &PythonRunConsole::finished);
        QVERIFY(app->debugPython(py));
        QVERIFY(finished.wait(20000));
        QVERIFY2(run->outputText().contains("args ['--points', '101', 'a file.dat']") && run->outputText().contains("cwd work True"),
                 qPrintable(run->outputText()));
        // A .env file not there: not run, said.
        qucs_s::python::setRunSettingsFor(path, {"", "", "", "gone.env"});
        QVERIFY(!app->runPython(py));
        QVERIFY(run->outputText().contains("gone.env"));
        qucs_s::python::setRunSettingsFor(path, {});
    }

    // ------------------------------------------------------------------
    // Interrupt, Debug Library Code

    // A long call interrupted: a KeyboardInterrupt in the script, said;
    // debugged, it stops where it was - and Interrupt is off while it is
    // stopped.
    void interruptAndDebugLibraryCode()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        if (!PythonRunConsole::canInterrupt()) QSKIP("no SIGINT here");
        PythonDoc* py = open(write("intr/i.py", "import time\n"
                                                "print('sleeping', flush=True)\n"
                                                "try:\n"
                                                "    time.sleep(60)\n"
                                                "except KeyboardInterrupt:\n"
                                                "    print('interrupted', flush=True)\n"
                                                "    raise\n"));
        QVERIFY(py != nullptr);
        PythonRunConsole* run = app->pythonRunConsole();
        QAction* interrupt = pythonAction("pythonInterrupt");
        QVERIFY(interrupt != nullptr && !interrupt->isEnabled());
        QSignalSpy done(run, &PythonRunConsole::finished);
        QVERIFY(app->runPython(py));
        QTRY_VERIFY_WITH_TIMEOUT(interrupt->isEnabled() && run->outputText().contains("sleeping"), 20000);   // (in the sleep)
        interrupt->trigger();
        QVERIFY(done.wait(20000));
        QVERIFY2(run->outputText().contains("interrupted") && run->outputText().contains("KeyboardInterrupt"), qPrintable(run->outputText()));
        QVERIFY2(run->outputText().contains("Interrupted after"), qPrintable(run->outputText()));   // (by SIGINT: not caught)
        QCOMPARE(run->exitCode(), -1);
        QVERIFY(!interrupt->isEnabled());

        // Debugged: stopped where it was interrupted.
        PythonDoc* slow = open(write("intr/slow.py", "import time\nprint('sleeping', flush=True)\ntime.sleep(60)\nprint('after')\n"));
        QSignalSpy paused(run, &PythonRunConsole::paused);
        QVERIFY(app->debugPython(slow));
        QTRY_VERIFY_WITH_TIMEOUT(interrupt->isEnabled() && run->outputText().contains("sleeping"), 20000);
        interrupt->trigger();
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stopReason(), QString("exception"));
        QVERIFY2(run->stopException().contains("KeyboardInterrupt"), qPrintable(run->stopException()));
        QCOMPARE(run->stack().first().line, 3);
        QVERIFY(!interrupt->isEnabled());   // (stopped: it would land in the debugger)
        QVERIFY(!run->findChild<QPushButton*>("pythonRunInterrupt")->isEnabled());   // (the dock's button too)
        run->stop();
        QVERIFY(done.wait(20000));

        // Python's library: stepped over - or, Debug Library Code on, into
        // (opened read-only), and out again.
        PythonDoc* lib = open(write("intr/lib.py", "import textwrap\ntext = textwrap.dedent('  a')\nprint('done', text)\n"));
        QVERIFY(lib != nullptr);
        lib->toggleBreakpoint(2);
        QAction* library = pythonAction("pythonDebugLibrary");
        QVERIFY(library != nullptr && !library->isChecked());
        QVERIFY(app->debugPython(lib));
        QVERIFY(paused.wait(20000));
        run->stepInto();
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().first().line, 3);   // (over textwrap: on to the next line)
        QCOMPARE(run->stack().size(), 1);
        run->stop();
        QVERIFY(done.wait(20000));
        library->trigger();
        QVERIFY(_settings::Get().item<bool>("PythonDebugLibraryCode"));
        QVERIFY(app->debugPython(lib));
        QVERIFY(paused.wait(20000));
        run->stepInto();
        QVERIFY(paused.wait(20000));
        QVERIFY2(run->stack().first().file.endsWith("textwrap.py"), qPrintable(run->stack().first().file));
        QVERIFY(run->stack().first().library);
        auto* opened = qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(opened != nullptr && opened->getDocName().endsWith("textwrap.py"));
        QVERIFY(opened->isLibraryFile() && opened->isReadOnly());
        run->stepOut();
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().first().line, 3);   // (back in the script, after the call)
        QVERIFY(!run->stack().first().library);
        run->continueRun();
        QVERIFY(done.wait(20000));
        QVERIFY(run->outputText().contains("done a"));
        library->trigger();
    }

    // ------------------------------------------------------------------
    // Quick Fix

    // The helpers: edits made, a problem ignored on its line (merged with
    // what is there), the name of an "undefined name".
    void fixesAreMade()
    {
        using Edit = qucs_s::python::Problem::Edit;
        QCOMPARE(qucs_s::python::applyEdits("import os\nx = 1\n", {Edit{1, 1, 2, 1, ""}}), QString("x = 1\n"));
        QCOMPARE(qucs_s::python::applyEdits("ab\ncd\n", {Edit{1, 2, 1, 2, "X"}, Edit{2, 1, 2, 3, "Y"}}), QString("aXb\nY\n"));
        QCOMPARE(qucs_s::python::ignoredOnLine("import os", "F401", ""), QString("import os  # noqa: F401"));
        QCOMPARE(qucs_s::python::ignoredOnLine("import os  # noqa: E501", "F401", ""), QString("import os  # noqa: E501, F401"));
        QCOMPARE(qucs_s::python::ignoredOnLine("import os  # noqa", "F401", ""), QString("import os  # noqa"));
        QCOMPARE(qucs_s::python::ignoredOnLine("x = y", "", ""), QString("x = y  # noqa"));
        QCOMPARE(qucs_s::python::ignoredOnLine("x: str = 1", "assignment", "mypy"), QString("x: str = 1  # type: ignore[assignment]"));
        QCOMPARE(qucs_s::python::ignoredOnLine("x: str = f(1)  # type: ignore[arg-type]", "assignment", "mypy"),
                 QString("x: str = f(1)  # type: ignore[arg-type, assignment]"));
        QCOMPARE(qucs_s::python::ignoredOnLine("y = z", "reportUndefinedVariable", "pyright"), QString("y = z  # pyright: ignore[reportUndefinedVariable]"));
        QCOMPARE(qucs_s::python::undefinedName("Undefined name `np` (F821)"), QString("np"));
        QCOMPARE(qucs_s::python::undefinedName("undefined name 'np'"), QString("np"));
        QCOMPARE(qucs_s::python::undefinedName("Name \"np\" is not defined (mypy: name-defined)"), QString("np"));
        QCOMPARE(qucs_s::python::undefinedName("\"np\" is not defined (pyright: reportUndefinedVariable)"), QString("np"));
        QVERIFY(qucs_s::python::undefinedName("`os` imported but unused (F401)").isEmpty());
    }

    // The light bulb on the cursor's line with a problem - a click on it its
    // menu, not a breakpoint -; ruff's fix (one edit, Undo takes it back),
    // only while the check is of the text as it is; an import for a name
    // not defined; a problem ignored on its line; Alt+Shift+Return.
    void quickFixesOfALine()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("qf/q.py", "import os\nx = 1\nprint(sqrt(2), x)\n"));
        QVERIFY(py != nullptr && focused(py));
        if (!py->checked() || py->checking()) QVERIFY(checked(py));
        QVERIFY2(py->lastCheck().checker == "ruff 9.9.9-fake", qPrintable(py->checkedBy()));
        QCOMPARE(py->diagnostics().size(), 2);
        place(py, 2, 0);
        QCOMPARE(py->bulbLine(), 0);   // (no problem there)
        place(py, 1, 3);
        QCOMPARE(py->bulbLine(), 1);
        {   // Drawn: yellow, where breakpoints go.
            QWidget* margin = marginOf(py);
            QVERIFY(margin != nullptr);
            const QImage shown = margin->grab().toImage();
            const QRect line = py->cursorRect(QTextCursor(py->document()->findBlockByNumber(0)));
            const int y = margin->mapFromGlobal(py->viewport()->mapToGlobal(line.center())).y() - 2;
            const int room = std::clamp(py->fontMetrics().height(), 12, 18) + 2;
            const QColor bulb = shown.pixelColor(room / 2, y);
            QVERIFY2(bulb.red() > 200 && bulb.green() > 150 && bulb.blue() < 120, qPrintable(bulb.name()));
            // A click on it: the menu - not a breakpoint.
            QTest::mouseClick(margin, Qt::LeftButton, {}, QPoint(room / 2, y));
            QTRY_VERIFY_WITH_TIMEOUT(py->fixMenu() != nullptr && py->fixMenu()->isVisible(), 5000);
            QVERIFY(py->breakpoints().isEmpty());
            py->fixMenu()->close();
        }
        QSignalSpy answered(py, &PythonDoc::fixesAnswered);
        py->quickFix(1);
        QCOMPARE(py->fixTitles(), (QStringList{"Fix: Remove unused import: `os` (F401)", "Ignore F401 on this line"}));
        QVERIFY(py->applyFix(0));
        QCOMPARE(py->toPlainText(), QString("x = 1\nprint(sqrt(2), x)\n"));
        py->undo();
        QCOMPARE(lineText(py, 1), QString("import os"));
        // The text changed since the check: its fixes not offered (yet).
        QVERIFY(checked(py));
        QTextCursor end(py->document());
        end.movePosition(QTextCursor::End);
        end.insertText("y = 2\n");
        py->quickFix(1);
        QCOMPARE(py->fixTitles(), QStringList{"Ignore F401 on this line"});
        QVERIFY(py->applyFix(0));
        QCOMPARE(lineText(py, 1), QString("import os  # noqa: F401"));
        QVERIFY(checked(py));
        // A name not defined: its import, from the completer; it goes after the
        // imports at the top.
        py->quickFix(3);
        QVERIFY(answered.wait(15000));
        QVERIFY2(py->fixTitles().first() == "Import: from math import sqrt", qPrintable(py->fixTitles().join(" | ")));   // (math's first)
        QVERIFY(py->fixTitles().contains("Ignore F821 on this line"));
        QVERIFY(py->applyFix(int(py->fixTitles().indexOf("Import: from math import sqrt"))));
        QCOMPARE(lineText(py, 1), QString("import os  # noqa: F401"));
        QCOMPARE(lineText(py, 2), QString("from math import sqrt"));
        QCOMPARE(lineText(py, 4), QString("print(sqrt(2), x)"));
        // Alt+Shift+Return: the menu at the cursor (the line's fixes).
        QVERIFY(checked(py));
        QCOMPARE(py->diagnostics().size(), 0);
        py->selectAll();
        py->insertPlainText("import sys\nz = 0\n");
        QVERIFY(checked(py));
        place(py, 1, 0);
        QVERIFY(focused(py));
        QTest::keyClick(py, Qt::Key_Return, Qt::AltModifier | Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(py->fixMenu() != nullptr && py->fixMenu()->isVisible(), 5000);
        QCOMPARE(py->toPlainText(), QString("import sys\nz = 0\n"));   // (no line break typed)
        QCOMPARE(py->fixMenu()->actions().size(), 2);
        py->fixMenu()->actions().first()->trigger();
        QCOMPARE(py->toPlainText(), QString("z = 0\n"));
    }

    // ------------------------------------------------------------------
    // Organize Imports, Format Selection, Format on Save

    void importsSelectionAndSave()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        const QString path = write("fmt/f.py", "import sys\nimport math\nimport os\na=1\nb=2\nc=3\n");
        PythonDoc* py = open(path);
        QVERIFY(py != nullptr && focused(py));
        QSignalSpy formatted(py, &PythonDoc::formatted);
        QTest::keyClick(py, Qt::Key_O, Qt::ShiftModifier | Qt::AltModifier);   // Organize Imports
        QVERIFY(formatted.wait(20000));
        QVERIFY2(py->toPlainText().startsWith("import math\nimport os\nimport sys\n"), qPrintable(py->toPlainText()));
        QVERIFY(formatted.last().at(0).toString().startsWith("Imports organized by ruff 9.9.9-fake"));
        py->undo();
        QVERIFY(py->toPlainText().startsWith("import sys\n"));   // (one edit)
        // Format Selection: lines 5-6 alone (a selection ending at a line's
        // start leaves that line).
        QTextCursor sel(py->document()->findBlockByNumber(4));
        sel.setPosition(py->document()->findBlockByNumber(6).position(), QTextCursor::KeepAnchor);
        py->setTextCursor(sel);
        QTest::keyClick(py, Qt::Key_F, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(formatted.wait(20000));
        QCOMPARE(py->toPlainText(), QString("import sys\nimport math\nimport os\na=1\nb = 2\nc = 3\n"));
        QVERIFY(formatted.last().at(0).toString().startsWith("Lines 5-6 formatted"));
        // The cursor's line alone.
        place(py, 4, 0);
        pythonAction("pythonFormatSelection")->trigger();
        QVERIFY(formatted.wait(20000));
        QCOMPARE(lineText(py, 4), QString("a = 1"));
        // Format on Save: off, saved as it is; on, formatted first.
        QAction* onSave = pythonAction("pythonFormatOnSave");
        QVERIFY(onSave != nullptr && !onSave->isChecked());
        py->selectAll();
        py->insertPlainText("p=1\n");
        QVERIFY(app->saveFile(py));
        QCOMPARE(read(path), QByteArray("p=1\n"));
        onSave->trigger();
        QVERIFY(PythonDoc::formatOnSave());
        py->selectAll();
        py->insertPlainText("q=2\n");
        QVERIFY(app->saveFile(py));
        QCOMPARE(read(path), QByteArray("q = 2\n"));
        QCOMPARE(py->toPlainText(), QString("q = 2\n"));
        QVERIFY(app->statusBar()->currentMessage().contains("Formatted by ruff 9.9.9-fake and saved"));
        onSave->trigger();
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication application(one, argv);
    TestPythonExtras test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_python_extras.moc"
