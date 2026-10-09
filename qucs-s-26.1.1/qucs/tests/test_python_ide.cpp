/*
 * test_python_ide.cpp - the Python editor's work as an IDE's (pythondoc.h,
 * pythonedit.cpp, pythonviews.h, pythonrun.h, qucs_python.cpp, python/):
 * matplotlib's figures in the Python Plots pane; the Python Shell's
 * variables and a value as a table; qucs.display() into a data display;
 * Find All References, Rename Symbol, Go to Symbol; breakpoints with
 * conditions, hits and messages, Pause, Run to Cursor, exceptions where
 * they are raised, values under the mouse and tables while stopped;
 * brackets and quotes closed, the bracket matched, folding; input typed
 * for a script debugged; a type checker's findings.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QDockWidget>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QToolBar>
#include <QTreeWidget>

#include "config.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "messagedock.h"
#include "misc.h"
#include "module.h"
#include "processconsole.h"
#include "pythondoc.h"
#include "pythonrun.h"
#include "pythonviews.h"
#include "qucs.h"
#include "schematic.h"
#include "settings.h"

class TestPythonIde : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QString python;   // python3 on the PATH; empty: none (what runs Python skipped)
    QByteArray noJedi;   // PYTHONPATH: the completer's own words, jedi or not here

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
    // Whether the scripts' Python imports \a module.
    bool has(const QString& module)
    {
        if (python.isEmpty()) return false;
        QProcess p;
        p.setProcessEnvironment(qucs_s::python::scriptEnvironment());
        p.start(python, {"-c", "import " + module});
        return p.waitForFinished(60000) && p.exitCode() == 0;
    }
    // The margin of line numbers (the editor's child left of its text).
    static QWidget* marginOf(PythonDoc* py)
    {
        for (QObject* child : py->children())
            if (auto* w = qobject_cast<QWidget*>(child); w != nullptr && w != py->viewport() && w->metaObject() == &QWidget::staticMetaObject
                                                         && w->x() < py->viewport()->x() && w->height() > 50)
                return w;
        return nullptr;
    }
    static int yOf(PythonDoc* py, QWidget* margin, int line)
    {
        QTextCursor c(py->document()->findBlockByNumber(line - 1));
        return margin->mapFromGlobal(py->viewport()->mapToGlobal(py->cursorRect(c).center())).y();
    }
    PythonSymbolPicker* shownPicker(QWidget* in) const
    {
        for (PythonSymbolPicker* p : in->findChildren<PythonSymbolPicker*>())
            if (p->isVisible()) return p;
        return nullptr;
    }
    PythonDataViewer* viewerOf(const QString& name) const
    {
        for (PythonDataViewer* v : app->findChildren<PythonDataViewer*>())
            if (v->name() == name && v->isVisible()) return v;
        return nullptr;
    }
    // \a text typed, its line breaks as Return (keyClicks types none).
    static void typeText(QWidget* w, const QString& text)
    {
        const QStringList lines = text.split('\n');
        for (qsizetype k = 0; k < lines.size(); ++k) {
            if (k > 0) QTest::keyClick(w, Qt::Key_Return);
            if (!lines.at(k).isEmpty()) QTest::keyClicks(w, lines.at(k));
        }
    }
    // Debugged till it stops: true when it did.
    bool stopped(PythonRunConsole* run, QSignalSpy& paused) { return run->isPaused() || paused.wait(20000); }

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
        write("nojedi/jedi/__init__.py", "raise ImportError('hidden for the test')\n");
        noJedi = QFile::encodeName(dir.filePath("nojedi"));
        if (const QByteArray had = qgetenv("PYTHONPATH"); !had.isEmpty()) noJedi += QDir::listSeparator().toLatin1() + had;
        qputenv("PYTHONPATH", noJedi);
        qputenv("MPLCONFIGDIR", QFile::encodeName(dir.filePath("mpl")));   // (matplotlib's caches here)
        PythonDoc::setTypeChecker(QStringLiteral("off"));   // (typesAreChecked has it on)
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
    // Python Plots

    // A script run with matplotlib: its figures in the pane, not windows -
    // plt.show() writes each and closes it, a figure's show() keeps it -,
    // the newest chosen, saved as a picture, removed; a cell run in the
    // Python Shell shows those it leaves open, as its script's. Plots in Qucs-S off: no
    // backend of the pane's given.
    void figuresGoToThePane()
    {
        QVERIFY(qucs_s::python::inlinePlots());
        QCOMPARE(qucs_s::python::scriptEnvironment().value("MPLBACKEND"), QString("module://_qucs_plots"));
        QVERIFY(QFileInfo(qucs_s::python::moduleFolder() + "/_qucs_plots.py").isFile());
        QVERIFY(QFileInfo(qucs_s::python::plotsFolder()).isDir());
        if (!has("matplotlib")) QSKIP("no matplotlib here");
        PythonPlotsPane* pane = app->pythonPlots();
        QVERIFY(pane != nullptr && pane->count() == 0);
        QVERIFY(!app->pythonPlotsDockWidget()->isVisible());
        PythonDoc* py = open(write("plots/p.py", "import matplotlib.pyplot as plt\n"
                                                 "plt.plot([0, 1, 2], [0, 1, 4]); plt.title('square')\n"
                                                 "plt.figure(); plt.plot([1, 2], [2, 1])\n"
                                                 "plt.show()\n"
                                                 "print('after', plt.get_fignums())\n"
                                                 "f = plt.figure('kept'); plt.plot([3, 4]); f.show()\n"
                                                 "print('kept', plt.get_fignums())\n"));
        QVERIFY(py != nullptr);
        PythonRunConsole* run = app->pythonRunConsole();
        QSignalSpy done(run, &PythonRunConsole::finished);
        QVERIFY(app->runPython(py));
        QVERIFY(done.wait(60000));
        QVERIFY2(run->outputText().contains("after []") && run->outputText().contains("kept [1]"), qPrintable(run->outputText()));
        QTRY_COMPARE_WITH_TIMEOUT(pane->count(), 3, 10000);
        QVERIFY(app->pythonPlotsDockWidget()->isVisible());
        QCOMPARE(pane->current(), 2);   // (the newest)
        QVERIFY2(pane->title(0).startsWith("p.py, figure 1: square"), qPrintable(pane->title(0)));
        QVERIFY(pane->title(2).contains("kept"));
        QVERIFY(!pane->shown().isNull());
        QVERIFY(pane->fitted());
        pane->choose(0);
        QVERIFY(!pane->shown().isNull());
        pane->setFitted(false);   // (at its own size: its pixels at the scale they were drawn at)
        QCOMPARE(pane->shown().deviceIndependentSize().toSize(), QImage(pane->imagePath(0)).size() / 2);
        pane->setFitted(true);
        QVERIFY(pane->save(0, dir.filePath("plots/saved.png")));
        QVERIFY(read(dir.filePath("plots/saved.png")).startsWith("\x89PNG"));
        QVERIFY(pane->save(0, dir.filePath("plots/saved.jpg")));
        QVERIFY(!QImage(dir.filePath("plots/saved.jpg")).isNull());
        const QString first = pane->imagePath(0);
        pane->remove(0);
        QCOMPARE(pane->count(), 2);
        QVERIFY(!QFileInfo::exists(first));

        // A cell in the Python Shell: its figure shown after it.
#ifndef Q_OS_WIN
        PythonDoc* cells = open(write("plots/cells.py", "# %%\nimport matplotlib.pyplot as plt\nplt.plot([5, 6, 7])\n"));
        QVERIFY(cells != nullptr && focused(cells));
        place(cells, 2, 0);
        pythonAction("pythonRunCell")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(pane->count(), 3, 60000);
        QVERIFY2(pane->title(2).startsWith("cells.py, figure"), qPrintable(pane->title(2)));   // (the cell's script)
#endif
        pane->removeAll();
        QCOMPARE(pane->count(), 0);

        // An animation (plt.pause()): its figure's picture replaced in place.
        PythonDoc* moving = open(write("plots/anim.py", "import matplotlib.pyplot as plt\n"
                                                        "for k in range(5):\n"
                                                        "    plt.cla(); plt.plot([0, k]); plt.title('frame %d' % k); plt.pause(0.01)\n"
                                                        "print('open', plt.get_fignums())\n"
                                                        "plt.show()\n"));
        QVERIFY(app->runPython(moving));
        QVERIFY(done.wait(60000));
        QVERIFY2(run->outputText().contains("open [1]"), qPrintable(run->outputText()));   // (paused, kept open)
        QTRY_VERIFY_WITH_TIMEOUT(pane->count() >= 1 && pane->title(pane->count() - 1).contains("frame 4"), 10000);
        QTest::qWait(500);
        QCOMPARE(pane->count(), 1);   // (each frame, and plt.show()'s at the end, in its place)
        QCOMPARE(QDir(qucs_s::python::plotsFolder()).entryList({"*.png"}).size(), pane->count());   // (those replaced gone)
        pane->removeAll();

        // Off: the scripts' own backend (none given).
        QAction* inlinePlots = pythonAction("pythonInlinePlots");
        QVERIFY(inlinePlots != nullptr && inlinePlots->isChecked());
        inlinePlots->trigger();
        QVERIFY(!qucs_s::python::inlinePlots());
        QVERIFY(!qucs_s::python::scriptEnvironment().contains("MPLBACKEND") || qEnvironmentVariableIsSet("MPLBACKEND"));
        inlinePlots->trigger();
        QVERIFY(qucs_s::python::inlinePlots());
    }

    // ------------------------------------------------------------------
    // Python Variables and the Data Viewer

    // The values the Data Viewer is given: an array, a list, lists of lists
    // and of dictionaries, a dictionary of columns - cells numbers,
    // strings or nothing; complex numbers and NaN as text.
    void valuesAsTables()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        QProcess p;
        p.setProcessEnvironment(qucs_s::python::scriptEnvironment());
        p.start(python, {"-c", "import json, _qucs_data as d\n"
                               "print(json.dumps([d.table([1.5, 2, 'x'], 1, 5), d.table([[1, 2], [3]]), "
                               "d.table([{'a': 1}, {'b': 2j}]), d.table({'f': [1, 2], 'g': [float('nan')]}),"
                               "d.summary('v', [1, 2]), d.summary('s', 'text'), d.table(3)]))\n"});
        QVERIFY(p.waitForFinished(60000));
        const QJsonArray answers = QJsonDocument::fromJson(p.readAllStandardOutput()).array();
        QVERIFY2(answers.size() == 7, qPrintable(p.readAllStandardError()));
        const QJsonObject list = answers.at(0).toObject();
        QCOMPARE(list.value("shape").toArray(), (QJsonArray{3, 1}));
        QCOMPARE(list.value("start").toInt(), 1);
        QCOMPARE(list.value("rows").toArray(), (QJsonArray{QJsonArray{2}, QJsonArray{"x"}}));
        QCOMPARE(answers.at(1).toObject().value("rows").toArray(), (QJsonArray{QJsonArray{1, 2}, QJsonArray{3, QJsonValue()}}));
        QCOMPARE(answers.at(2).toObject().value("columns").toArray(), (QJsonArray{"a", "b"}));
        QCOMPARE(answers.at(2).toObject().value("rows").toArray().at(1).toArray().at(1).toString(), QString("0.0+2.0j"));
        QCOMPARE(answers.at(3).toObject().value("rows").toArray().at(0).toArray().at(1).toString(), QString("nan"));
        QCOMPARE(answers.at(4).toObject().value("table").toBool(), true);
        QCOMPARE(answers.at(5).toObject().value("table").toBool(), false);
        QCOMPARE(answers.at(6).toObject().value("shape").toArray(), (QJsonArray{0, 0}));
    }

    // The Python Shell's variables after each command - modules, functions
    // and names of Python's left out -, filtered; one as a table, its rows
    // fetched as they are needed, copied and exported as CSV.
    void theShellsVariables()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
#ifdef Q_OS_WIN
        QSKIP("the shell's console is a terminal's on Unix");
#endif
        ProcessConsole* shell = app->pythonConsole();
        PythonVariablesPane* pane = app->pythonVariables();
        QVERIFY(pane != nullptr);
        shell->sendLine("import os; xs = [1.5, 2.5, 3]; grid = [[1, 2], [3, 4]]; cols = {'f': [1, 2, 3], 'g': [4, 5, 6]}; "
                        "name = 'hi'; big = list(range(1234)); _hidden = 1");
        shell->sendLine("def helper(): pass");
        shell->sendLine("");
        QTRY_VERIFY_WITH_TIMEOUT(pane->rows().contains("xs: list [3] = [1.5, 2.5, 3]"), 30000);
        const QStringList rows = pane->rows();
        QVERIFY2(rows.contains("name: str [2] = 'hi'"), qPrintable(rows.join(" | ")));
        QVERIFY(rows.contains("big: list [1234] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, ...]"));
        for (const char* gone : {"os:", "helper:", "_hidden:", "__builtins__:"})
            QVERIFY2(std::none_of(rows.cbegin(), rows.cend(), [&](const QString& r) { return r.startsWith(gone); }), gone);
        // A command more: shown after it.
        shell->sendLine("xs.append(9)");
        QTRY_VERIFY_WITH_TIMEOUT(pane->rows().contains("xs: list [4] = [1.5, 2.5, 3, 9]"), 30000);
        auto* filter = pane->findChild<QLineEdit*>("pythonVariablesFilter");
        QVERIFY(filter != nullptr);
        filter->setText("co");
        QCOMPARE(pane->rows().size(), 1);
        filter->clear();

        // As a table: a dictionary's columns.
        PythonDataViewer* viewer = app->viewShellTable("cols");
        QVERIFY(viewer != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(viewer->model()->rowCount(), 3, 20000);
        QCOMPARE(viewer->model()->columnCount(), 2);
        QCOMPARE(viewer->model()->headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(), QString("g"));
        QCOMPARE(viewer->model()->text(2, 1), QString("6"));
        QCOMPARE(viewer->selectedText(), QString("1\t4\n2\t5\n3\t6"));
        viewer->close();
        // A long one: its rows as they are scrolled to; every one exported.
        PythonDataViewer* longer = app->viewShellTable("big");
        QTRY_COMPARE_WITH_TIMEOUT(longer->model()->rowCount(), 1234, 20000);
        QVERIFY(!longer->model()->complete());
        QCOMPARE(longer->model()->text(1100, 0), QString());   // (not yet fetched)
        QSignalSpy exported(longer, &PythonDataViewer::exported);
        QDir().mkpath(dir.filePath("vars"));
        QVERIFY(!longer->exportTo(dir.filePath("vars/big.csv")));   // (fetched first)
        QVERIFY(exported.wait(30000));
        QCOMPARE(longer->model()->text(1100, 0), QString("1100"));
        const QList<QByteArray> lines = read(dir.filePath("vars/big.csv")).split('\n');
        QCOMPARE(lines.at(0), QByteArray("value"));
        QCOMPARE(lines.at(1234), QByteArray("1233"));
        longer->close();
        // A million rows: open at once, the last fetched when it is wanted.
        shell->sendLine("huge = list(range(1000000))");
        PythonDataViewer* huge = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(pane->rows().join(' ').contains("huge: list [1000000]"), 30000);
        QElapsedTimer opening;
        opening.start();
        huge = app->viewShellTable("huge");
        QTRY_COMPARE_WITH_TIMEOUT(huge->model()->rowCount(), 1000000, 20000);
        QVERIFY2(opening.elapsed() < 10000, qPrintable(QString::number(opening.elapsed())));
        huge->view()->scrollToBottom();
        QTRY_COMPARE_WITH_TIMEOUT(huge->model()->text(999999, 0), QString("999999"), 20000);
        QVERIFY(!huge->model()->complete());
        huge->close();
        // One that is not there: said.
        PythonDataViewer* none = app->viewShellTable("nothing_here");
        QTRY_VERIFY_WITH_TIMEOUT(none->findChild<QLabel*>("pythonDataAbout")->text().contains("NameError"), 20000);
        none->close();
        // The shell ended: its variables gone; a table of one said so.
        shell->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!shell->isRunning(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(pane->rows().isEmpty(), 20000);
        PythonDataViewer* gone = app->viewShellTable("xs");
        QVERIFY(gone->findChild<QLabel*>("pythonDataAbout")->text().contains("not running"));
        gone->close();
    }

    // ------------------------------------------------------------------
    // qucs.display()

    // A script's results shown in a data display: name.dat written beside
    // it, name.dpl made and opened, a diagram of them on it; run again, the
    // same diagram shows the data read anew - none added.
    void resultsGoToADataDisplay()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("display/sweep.py", "import qucs\n"
                                                       "f = [1e3, 1e4, 1e5]\n"
                                                       "gain = [v / 1e3 for v in f]\n"
                                                       "print(qucs.display({'frequency': f, 'gain': gain}, title='Gain'))\n"));
        QVERIFY(py != nullptr);
        PythonRunConsole* run = app->pythonRunConsole();
        QSignalSpy done(run, &PythonRunConsole::finished);
        QVERIFY(app->runPython(py));
        QVERIFY(done.wait(30000));
        const QString dpl = dir.filePath("display/sweep_results.dpl");
        QVERIFY2(run->outputText().contains("sweep_results.dpl"), qPrintable(run->outputText()));
        QVERIFY(QFileInfo(dir.filePath("display/sweep_results.dat")).isFile());
        Schematic* shown = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((shown = qobject_cast<Schematic*>(app->DocumentTab->currentWidget())) != nullptr
                                     && shown->getDocName() == dpl && shown->a_DocDiags.size() == 1, 20000);
        Diagram* d = shown->a_DocDiags.front();
        QCOMPARE(d->Graphs.size(), 1);
        QCOMPARE(d->Graphs.first()->Var, QString("gain"));
        QCOMPARE(int(d->Graphs.first()->count(0)), 3);
        QCOMPARE(d->Graphs.first()->axisName(0), QString("frequency"));
        // Again, with other values: the same diagram, read anew.
        py->selectAll();
        py->insertPlainText("import qucs\nqucs.display({'frequency': [1, 2, 3, 4], 'gain': [4, 3, 2, 1]})\n");
        QVERIFY(app->runPython(py));
        QVERIFY(done.wait(30000));
        QTRY_VERIFY_WITH_TIMEOUT(shown->a_DocDiags.front()->Graphs.first()->count(0) == 4, 20000);
        QCOMPARE(shown->a_DocDiags.size(), 1);
    }

    // ------------------------------------------------------------------
    // References, Rename Symbol, Go to Symbol

    // Without jedi, by Python's rules of scope: a module's name where it is
    // used (a function's parameter of that name not), its places on the
    // References tab - a click goes there -; a function's local alone;
    // renamed (one edit, Undo takes it back); an attribute: its places,
    // and its renaming refused; a name no Python takes refused.
    void referencesAndRenaming()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("refs/r.py", "gain = 2\n"                          // 1
                                                "def amp(x, gain=3):\n"               // 2
                                                "    return x * gain\n"               // 3
                                                "def total(values):\n"                // 4
                                                "    s = 0\n"                         // 5
                                                "    for v in values:\n"              // 6
                                                "        s += v * gain\n"             // 7
                                                "    return [gain for _ in values], s\n"   // 8
                                                "class Amp:\n"                        // 9
                                                "    gain = 5\n"                      // 10
                                                "    def run(self):\n"                // 11
                                                "        return self.gain + gain\n"   // 12
                                                "print(amp(1), total([1]), gain)\n"));   // 13
        QVERIFY(py != nullptr && focused(py));
        place(py, 1, 1);
        QSignalSpy found(py, &PythonDoc::referencesAnswered);
        QTest::keyClick(py, Qt::Key_F12, Qt::ShiftModifier);   // Find All References
        QVERIFY(found.wait(15000));
        const qucs_s::python::Answer& refs = py->lastReferences();
        QCOMPARE(refs.name, QString("gain"));
        QCOMPARE(refs.scope, QString("module"));
        QList<int> lines;
        for (const auto& r : refs.references) lines << r.line;
        QCOMPARE(lines, (QList<int>{1, 7, 8, 12, 13}));
        QVERIFY(refs.references.first().definition);
        MessageDock* dock = app->messages();
        QVERIFY(dock != nullptr && dock->msgDock->isVisible());
        QCOMPARE(dock->referenceRows().size(), 5);
        QVERIFY2(dock->referenceRows().at(2).startsWith(":8:"), qPrintable(dock->referenceRows().join(" | ")));
        QTreeWidgetItem* row = dock->references->topLevelItem(0)->child(3);
        dock->references->setCurrentItem(row);
        emit dock->references->itemClicked(row, 0);
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(py));
        QCOMPARE(py->textCursor().blockNumber() + 1, 12);
        QCOMPARE(py->textCursor().positionInBlock(), 27);

        // A parameter: its function's alone.
        place(py, 3, 17);
        py->findReferences();
        QVERIFY(found.wait(15000));
        lines.clear();
        for (const auto& r : py->lastReferences().references) lines << r.line;
        QCOMPARE(lines, (QList<int>{2, 3}));
        QCOMPARE(py->lastReferences().scope, QString("function"));

        // Renamed: the module's name, not the parameter nor the class's.
        place(py, 13, 27);
        QSignalSpy renamed(py, &PythonDoc::renameAnswered);
        py->renameSymbol("level");
        QVERIFY(renamed.wait(15000));
        QVERIFY2(py->lastRename().refusal.isEmpty(), qPrintable(py->lastRename().refusal));
        QCOMPARE(py->lastRename().count, 5);
        QCOMPARE(lineText(py, 1), QString("level = 2"));
        QCOMPARE(lineText(py, 2), QString("def amp(x, gain=3):"));
        QCOMPARE(lineText(py, 8), QString("    return [level for _ in values], s"));
        QCOMPARE(lineText(py, 10), QString("    gain = 5"));
        QCOMPARE(lineText(py, 12), QString("        return self.gain + level"));
        QVERIFY2(app->statusBar()->currentMessage().contains("5 place"), qPrintable(app->statusBar()->currentMessage()));
        py->undo();
        QCOMPARE(lineText(py, 1), QString("gain = 2"));   // (one edit)
        QCOMPARE(lineText(py, 13), QString("print(amp(1), total([1]), gain)"));
        // An attribute: its places; renaming it needs jedi.
        place(py, 12, 21);
        py->findReferences();
        QVERIFY(found.wait(15000));
        QCOMPARE(py->lastReferences().scope, QString("attribute"));
        QCOMPARE(py->lastReferences().references.size(), 1);
        py->renameSymbol("level");
        QVERIFY(renamed.wait(15000));
        QVERIFY(py->lastRename().refusal.contains("jedi"));
        QCOMPARE(lineText(py, 12), QString("        return self.gain + gain"));
        // Not a name: refused, and said.
        place(py, 1, 1);
        py->renameSymbol("2bad");
        QVERIFY(renamed.wait(15000));
        QVERIFY(py->lastRename().refusal.contains("no name"));
        QCOMPARE(lineText(py, 1), QString("gain = 2"));
        // The action: the name at the cursor asked for.
        QCOMPARE(py->nameAtCursor(), QString("gain"));
        QVERIFY(pythonAction("pythonRename")->isEnabled());
        // A comprehension's variable is its own, not the module's of its name.
        PythonDoc* comp = open(write("refs/comp.py", "x = 1\nys = [x * 2 for x in range(3)]\nprint(x, ys)\n"));
        QVERIFY(comp != nullptr);
        place(comp, 3, 6);
        QSignalSpy compFound(comp, &PythonDoc::referencesAnswered);
        comp->findReferences();
        QVERIFY(compFound.wait(15000));
        lines.clear();
        for (const auto& r : comp->lastReferences().references) lines << r.line;
        QCOMPARE(lines, (QList<int>{1, 3}));
    }

    // With jedi (when it is here): a function renamed in the module beside
    // the script too - opened, changed, not saved.
    void renamingAcrossModulesWithJedi()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        const EnvironmentReset reset("PYTHONPATH", noJedi.mid(noJedi.indexOf(QDir::listSeparator().toLatin1()) + 1),
                                     noJedi.contains(QDir::listSeparator().toLatin1()));
        if (!has("jedi")) QSKIP("no jedi here");
        const QString lib = write("jrefs/helpers.py", "def scale(v):\n    return v * 2\n");
        PythonDoc* py = open(write("jrefs/main.py", "from helpers import scale\nprint(scale(3))\n"));
        QVERIFY(py != nullptr);
        place(py, 2, 7);
        QSignalSpy found(py, &PythonDoc::referencesAnswered);
        py->findReferences();
        QVERIFY(found.wait(30000));
        QVERIFY2(py->lastReferences().scope == "jedi", qPrintable(py->lastReferences().scope));
        QVERIFY(std::any_of(py->lastReferences().references.cbegin(), py->lastReferences().references.cend(),
                            [](const auto& r) { return r.file.endsWith("helpers.py") && r.line == 1; }));
        QSignalSpy renamed(py, &PythonDoc::renameAnswered);
        py->renameSymbol("double");
        QVERIFY(renamed.wait(30000));
        QVERIFY2(py->lastRename().refusal.isEmpty(), qPrintable(py->lastRename().refusal));
        QCOMPARE(lineText(py, 2), QString("print(double(3))"));
        int at = 0;
        auto* helpers = dynamic_cast<TextDoc*>(app->findDoc(lib, &at));
        QVERIFY(helpers != nullptr);
        QVERIFY(helpers->toPlainText().startsWith("def double(v):"));
        QVERIFY(helpers->getDocChanged());
        QVERIFY(read(lib).startsWith("def scale"));   // (not saved)
    }

    // The script's classes, functions, methods and module variables - and
    // those of the scripts beside it - picked by their letters; the best
    // first; Return goes there, Go Back comes back.
    void goToSymbol()
    {
        const QList<qucs_s::python::Symbol> symbols = qucs_s::python::symbolsOf(
            "import os\nLIMIT = 3\na, b = 1, 2\nclass Amp:\n    def gain(self):\n        x = 1\n        return x\n"
            "def run():\n    pass\nfor k in range(3):\n    pass\nif LIMIT:\n    y = 2\n\"\"\"\nz = 3\n\"\"\"\n");
        QStringList names;
        for (const auto& s : symbols) names << s.kind.left(1) + ":" + (s.container.isEmpty() ? s.name : s.container + "." + s.name);
        QCOMPARE(names, (QStringList{"v:LIMIT", "v:a", "v:b", "c:Amp", "f:Amp.gain", "f:run", "v:k"}));
        QCOMPARE(symbols.at(4).line, 5);
        QCOMPARE(symbols.at(4).column, 8);
        QVERIFY(qucs_s::python::symbolScore("total_gain", "tg") > qucs_s::python::symbolScore("tiny_ugly", "tg"));
        QVERIFY(qucs_s::python::symbolScore("gain", "gain") > qucs_s::python::symbolScore("gain_db", "gain"));
        QCOMPARE(qucs_s::python::symbolScore("gain", "x"), -1);

        write("sym/other.py", "def gain_of_other():\n    pass\n");
        PythonDoc* py = open(write("sym/s.py", "class Amp:\n    def gain(self):\n        return 1\n\ndef run_gain():\n    pass\nlevel = 3\n"));
        QVERIFY(py != nullptr && focused(py));
        place(py, 7, 0);
        QTest::keyClick(py, Qt::Key_F12, Qt::ControlModifier);   // Go to Symbol
        PythonSymbolPicker* picker = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((picker = shownPicker(py)) != nullptr, 5000);
        QVERIFY(picker->shownNames().contains("Amp.gain") && picker->shownNames().contains("gain_of_other"));
        QTest::keyClicks(picker->filterLine(), "gn");
        QCOMPARE(picker->shownNames().first(), QString("Amp.gain"));   // (the script's, the best)
        QVERIFY(!picker->shownNames().contains("level"));
        QTest::keyClick(picker->filterLine(), Qt::Key_Return);
        QTRY_VERIFY(!picker->isVisible());
        QCOMPARE(py->textCursor().blockNumber() + 1, 2);
        QCOMPARE(py->textCursor().positionInBlock(), 8);
        pythonAction("pythonBack")->trigger();
        QCOMPARE(py->textCursor().blockNumber() + 1, 7);
        // Another script's: opened there.
        QTest::keyClick(py, Qt::Key_F12, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT((picker = shownPicker(py)) != nullptr, 5000);
        QTest::keyClicks(picker->filterLine(), "other");
        QCOMPARE(picker->shownNames(), QStringList{"gain_of_other"});
        QTest::keyClick(picker->filterLine(), Qt::Key_Return);
        auto* other = qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(other != nullptr && other->getDocName().endsWith("other.py"));
    }

    // ------------------------------------------------------------------
    // The debugger

    // A breakpoint's condition, its hits, a logpoint's message, one off:
    // as the debugger has them - set in the margin's menu and dialog, drawn
    // in the margin -; changed while it runs.
    void breakpointsWithConditions()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("bpc/c.py", "total = 0\n"                       // 1
                                               "for k in range(10):\n"             // 2
                                               "    total += k\n"                  // 3
                                               "    half = total / 2\n"            // 4
                                               "print('total', total)\n"));        // 5
        QVERIFY(py != nullptr);
        // The margin's menu: Add Breakpoint, Add Conditional Breakpoint,
        // Add Logpoint - then Edit, Disable, Remove.
        QMenu* menu = py->marginMenuAt(3);
        QStringList items;
        for (QAction* a : menu->actions())
            if (!a->isSeparator()) items << a->objectName();
        QCOMPARE(items, (QStringList{"addBreakpoint", "addConditionalBreakpoint", "addLogpoint"}));
        menu->deleteLater();
        qucs_s::python::Breakpoint conditional;
        conditional.line = 3;
        conditional.condition = "k > 5";
        conditional.hit = "2";
        py->setBreakpoint(conditional);
        qucs_s::python::Breakpoint log;
        log.line = 4;
        log.log = "k={k} total={total}";
        py->setBreakpoint(log);
        qucs_s::python::Breakpoint off;
        off.line = 5;
        off.enabled = false;
        py->setBreakpoint(off);
        QCOMPARE(py->breakpoints(), (QList<int>{3, 4, 5}));
        QCOMPARE(py->breakpointAt(3).condition, QString("k > 5"));
        menu = py->marginMenuAt(3);
        items.clear();
        for (QAction* a : menu->actions())
            if (!a->isSeparator()) items << a->objectName();
        QCOMPARE(items, (QStringList{"editBreakpoint", "enableBreakpoint", "removeBreakpoint"}));
        menu->deleteLater();
        {   // Drawn: a diamond for the logpoint (its corner left empty), a
            // dot for the others (a bar across, with a condition); hollow
            // for the one off.
            QWidget* margin = marginOf(py);
            QVERIFY(margin != nullptr);
            const QImage shown = margin->grab().toImage();
            const int room = std::clamp(py->fontMetrics().height(), 12, 18) + 2;
            const auto red = [](const QColor& c) { return c.red() > 150 && c.green() < 110; };
            // (Above the bar across it: red whatever the font's size.)
            const int size = std::min(room, py->fontMetrics().height()) - 4;
            const QColor dot = shown.pixelColor(room / 2, yOf(py, margin, 3) - size / 3);
            QVERIFY2(red(dot), qPrintable(dot.name()));
            const QColor bar = shown.pixelColor(room / 2, yOf(py, margin, 3) - 1);
            QVERIFY2(!red(shown.pixelColor(room / 2, yOf(py, margin, 3) - 1)) || !red(shown.pixelColor(room / 2, yOf(py, margin, 3) + 2)),
                     qPrintable(bar.name()));
            const QColor centre = shown.pixelColor(room / 2, yOf(py, margin, 4));
            QVERIFY2(red(centre), qPrintable(centre.name()));
            const QColor corner = shown.pixelColor(4, yOf(py, margin, 4) - room / 2 + 5);
            QVERIFY2(!red(corner), qPrintable(corner.name()));
            const QColor hollow = shown.pixelColor(room / 2, yOf(py, margin, 5));
            QVERIFY2(!red(hollow), qPrintable(hollow.name()));
        }
        PythonRunConsole* run = app->pythonRunConsole();
        QSignalSpy paused(run, &PythonRunConsole::paused);
        QVERIFY(app->debugPython(py));
        QVERIFY(stopped(run, paused));
        QCOMPARE(run->stopReason(), QString("breakpoint"));
        QCOMPARE(run->stack().first().line, 3);
        QVERIFY2(run->variableRows().contains("k: int = 7"), qPrintable(run->variableRows().join(" | ")));   // (6 the first hit, 7 the second)
        QVERIFY2(run->outputText().contains("k=6 total=21"), qPrintable(run->outputText()));   // (the logpoint, each time)
        QVERIFY(!run->outputText().contains("k=7"));   // (line 4 not reached yet)
        // Changed while it is stopped: no condition, every second hit.
        conditional.condition.clear();
        conditional.hit = "% 2";
        py->setBreakpoint(conditional);
        run->continueRun();
        QVERIFY(paused.wait(20000));
        QVERIFY2(run->variableRows().contains("k: int = 9"), qPrintable(run->variableRows().join(" | ")));   // (k 8 the first hit, 9 the second)
        py->removeBreakpoint(3);
        QSignalSpy done(run, &PythonRunConsole::finished);
        run->continueRun();
        QVERIFY(done.wait(20000));
        QVERIFY(run->outputText().contains("total 45"));   // (line 5's, off: not stopped)
        QVERIFY(run->outputText().contains("k=9 total=45"));
    }

    // Pause stops a loop on one line; Run to Cursor starts debugging and
    // stops at the cursor's line - and, stopped, goes on to another;
    // exceptions where they are raised (caught ones too) when asked.
    void pauseRunToCursorAndRaised()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("pause/p.py", "import os\n"                                  // 1
                                                 "a = 1\n"                                      // 2
                                                 "b = a + 1\n"                                  // 3
                                                 "try:\n"                                       // 4
                                                 "    int('x')\n"                               // 5
                                                 "except ValueError:\n"                         // 6
                                                 "    pass\n"                                   // 7
                                                 "c = b + 1\n"                                  // 8
                                                 "n = 0\n"                                      // 9
                                                 "def spin():\n"                                // 10
                                                 "    global n\n"                               // 11
                                                 "    while not os.access('stop.flag', os.F_OK): n += 1\n"   // 12 (C alone: no call bdb sees)
                                                 "spin()\n"                                     // 13
                                                 "print('end', n > 0)\n"));                     // 14
        QVERIFY(py != nullptr && focused(py));
        PythonRunConsole* run = app->pythonRunConsole();
        QSignalSpy paused(run, &PythonRunConsole::paused);
        QSignalSpy done(run, &PythonRunConsole::finished);
        QAction* runTo = pythonAction("pythonRunToCursor");
        QVERIFY(runTo != nullptr && runTo->isEnabled());
        place(py, 3, 0);
        QTest::keyClick(py, Qt::Key_F10, Qt::ControlModifier | Qt::ShiftModifier);   // Run to Cursor
        QVERIFY(stopped(run, paused));
        QCOMPARE(run->stack().first().line, 3);
        QCOMPARE(py->executionLine(), 3);
        // Exceptions where they are raised: on, at once.
        QAction* raised = pythonAction("pythonBreakOnRaised");
        QVERIFY(raised != nullptr && !raised->isChecked());
        raised->trigger();
        pythonAction("pythonContinue")->trigger();
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stopReason(), QString("raised"));
        QVERIFY2(run->stopException().contains("ValueError"), qPrintable(run->stopException()));
        QCOMPARE(run->stack().first().line, 5);
        raised->trigger();   // (off again)
        // On to line 8, stopped.
        place(py, 8, 0);
        QTest::keyClick(py, Qt::Key_F10, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().first().line, 8);
        QVERIFY(!pythonAction("pythonPause")->isEnabled());
        // Paused in a loop of one line, in a function bdb does not trace (no
        // breakpoint in its file): Ctrl+F2 Pause's while it runs.
        pythonAction("pythonContinue")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(pythonAction("pythonPause")->isEnabled(), 10000);
        QVERIFY(!pythonAction("pythonContinue")->isEnabled() && !pythonAction("pythonDebug")->isEnabled());
        QVERIFY(pythonAction("pythonPause")->isVisible() && !pythonAction("pythonContinue")->isVisible());   // (in its place)
        QTest::qWait(300);
        QVERIFY(focused(py));
        QTest::keyClick(py, Qt::Key_F2, Qt::ControlModifier);   // Pause
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stopReason(), QString("pause"));
        QCOMPARE(run->stack().first().line, 12);
        QCOMPARE(run->stack().first().function, QString("spin"));
        QVERIFY(pythonAction("pythonContinue")->isEnabled());
        QVERIFY(pythonAction("pythonContinue")->isVisible() && !pythonAction("pythonPause")->isVisible());
        write("pause/stop.flag", "");
        run->continueRun();
        QVERIFY(done.wait(20000));
        QVERIFY(run->outputText().contains("end True"));
    }

    // While it is stopped: the value of the name under the mouse - an
    // attribute's with what it is of -; a list of the variables shown as a
    // table; what is typed read by input().
    void valuesTablesAndInputWhileDebugging()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("val/v.py", "class Box:\n"                          // 1
                                               "    def __init__(self):\n"             // 2
                                               "        self.size = 42\n"              // 3
                                               "box = Box()\n"                         // 4
                                               "rows = [[1, 2], [3, 4], [5, 6]]\n"     // 5
                                               "name = input('name? ')\n"              // 6
                                               "print('hello', name, box.size)\n"));   // 7
        QVERIFY(py != nullptr);
        py->toggleBreakpoint(6);
        PythonRunConsole* run = app->pythonRunConsole();
        QSignalSpy paused(run, &PythonRunConsole::paused);
        QVERIFY(app->debugPython(py));
        QVERIFY(stopped(run, paused));
        QCOMPARE(run->stack().first().line, 6);
        // The mouse on size of box.size (line 7).
        QSignalSpy value(py, &PythonDoc::valueShown);
        const QTextBlock seven = py->document()->findBlockByNumber(6);
        QTextCursor at(seven);
        at.setPosition(seven.position() + seven.text().indexOf("size") + 1);
        const QPoint where = py->cursorRect(at).center();
        QHelpEvent tip(QEvent::ToolTip, where, py->viewport()->mapToGlobal(where));
        QApplication::sendEvent(py->viewport(), &tip);
        QVERIFY(value.wait(10000));
        QCOMPARE(py->lastValue(), QString("box.size = 42"));
        {   // A keyword: no value asked for (what it is, as ever).
            const QTextBlock one = py->document()->findBlockByNumber(0);
            QTextCursor kw(one);
            kw.setPosition(one.position() + 2);   // (class)
            const QPoint on = py->cursorRect(kw).center();
            QHelpEvent keyword(QEvent::ToolTip, on, py->viewport()->mapToGlobal(on));
            QSignalSpy help(py, &PythonDoc::helpAnswered);
            QApplication::sendEvent(py->viewport(), &keyword);
            QVERIFY(help.wait(15000));
            QCOMPARE(value.size(), 1);
            // A name of no value in the frame (__init__ is Box's): what it is.
            const QTextBlock two = py->document()->findBlockByNumber(1);
            QTextCursor init(two);
            init.setPosition(two.position() + two.text().indexOf("__init__") + 3);
            const QPoint onInit = py->cursorRect(init).center();
            QHelpEvent noValue(QEvent::ToolTip, onInit, py->viewport()->mapToGlobal(onInit));
            QApplication::sendEvent(py->viewport(), &noValue);
            QVERIFY(value.wait(10000));
            QVERIFY(py->lastValue().isEmpty());
            QVERIFY(help.wait(15000));
            value.clear();
        }
        // rows as a table.
        QTreeWidgetItem* rows = nullptr;
        for (int k = 0; k < run->variablesView()->topLevelItemCount(); ++k)
            if (run->variablesView()->topLevelItem(k)->text(0) == "rows") rows = run->variablesView()->topLevelItem(k);
        QVERIFY(rows != nullptr);
        emit run->variablesView()->itemDoubleClicked(rows, 0);
        PythonDataViewer* viewer = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((viewer = viewerOf("rows")) != nullptr && viewer->model()->rowCount() == 3, 10000);
        QCOMPARE(viewer->model()->columnCount(), 2);
        QCOMPARE(viewer->model()->text(2, 1), QString("6"));
        viewer->close();
        // input(): typed below the output.
        QSignalSpy done(run, &PythonRunConsole::finished);
        run->continueRun();
        QTRY_VERIFY_WITH_TIMEOUT(run->outputText().contains("name? "), 20000);
        QVERIFY(run->inputLine()->isEnabled());
        QTest::keyClicks(run->inputLine(), "Ada");
        QTest::keyClick(run->inputLine(), Qt::Key_Return);
        QVERIFY(done.wait(20000));
        QVERIFY2(run->outputText().contains("hello Ada 42"), qPrintable(run->outputText()));
        // Not stopped: the mouse says what a name is, as ever.
        QVERIFY(!run->isPaused());
        QApplication::sendEvent(py->viewport(), &tip);
        QTest::qWait(300);
        QVERIFY(value.isEmpty());
    }

    // ------------------------------------------------------------------
    // Brackets, quotes, folds

    // Typed: a bracket closed, the cursor between; its closing one typed
    // over; Backspace takes both; a selection wrapped; a quote closed -
    // not after a word's letter (it's), nor in a string -, a third making a
    // triple quote; nothing closed before a word. Off: as typed.
    void bracketsAndQuotesClose()
    {
        QVERIFY(PythonDoc::autoClose());
        PythonDoc* py = open(write("close/c.py", ""));
        QVERIFY(py != nullptr && focused(py));
        QTest::keyClicks(py, "f(");
        QCOMPARE(py->toPlainText(), QString("f()"));
        QCOMPARE(py->textCursor().positionInBlock(), 2);
        QTest::keyClicks(py, "a[1");
        QCOMPARE(py->toPlainText(), QString("f(a[1])"));
        QTest::keyClicks(py, "])");
        QCOMPARE(py->toPlainText(), QString("f(a[1])"));
        QCOMPARE(py->textCursor().positionInBlock(), 7);
        typeText(py, "\nx = {");
        QTest::keyClick(py, Qt::Key_Backspace);
        QCOMPARE(lineText(py, 2), QString("x = "));
        QTest::keyClicks(py, "'");
        QCOMPARE(lineText(py, 2), QString("x = ''"));
        QTest::keyClicks(py, "it's");   // (in a string: the quote typed over the closing one)
        QCOMPARE(lineText(py, 2), QString("x = 'it's"));
        py->moveCursor(QTextCursor::EndOfBlock);
        typeText(py, "\ns = f\"");
        QCOMPARE(lineText(py, 3), QString("s = f\"\""));   // (a prefix: a string)
        QTest::keyClicks(py, "\"\"");
        QCOMPARE(lineText(py, 3), QString("s = f\"\"\"\"\"\""));   // (a triple quote)
        QCOMPARE(py->textCursor().positionInBlock(), 8);
        py->moveCursor(QTextCursor::EndOfBlock);
        typeText(py, "\nword(");
        QCOMPARE(lineText(py, 4), QString("word()"));
        py->moveCursor(QTextCursor::StartOfBlock);
        QTest::keyClicks(py, "(");   // (before a word: typed alone)
        QCOMPARE(lineText(py, 4), QString("(word()"));
        // A selection wrapped, still selected.
        QTextCursor sel(py->document()->findBlockByNumber(3));
        sel.setPosition(sel.position() + 1);
        sel.setPosition(sel.position() + 4, QTextCursor::KeepAnchor);
        py->setTextCursor(sel);
        QTest::keyClicks(py, "[");
        QCOMPARE(lineText(py, 4), QString("([word]()"));
        QCOMPARE(py->textCursor().selectedText(), QString("word"));
        // In a comment: not closed. Off: as typed.
        py->moveCursor(QTextCursor::End);
        typeText(py, "\n# (");
        QCOMPARE(lineText(py, 5), QString("# ("));
        QAction* closing = pythonAction("pythonAutoClose");
        QVERIFY(closing != nullptr && closing->isChecked());
        closing->trigger();
        typeText(py, "\ng(");
        QCOMPARE(lineText(py, 6), QString("g("));
        closing->trigger();
        QVERIFY(PythonDoc::autoClose());
        // Undo takes a pair away whole.
        typeText(py, "\nh(");
        QCOMPARE(lineText(py, 7), QString("h()"));
        py->undo();
        QVERIFY(!py->toPlainText().endsWith(')') && !py->toPlainText().endsWith('('));
    }

    // The bracket at the cursor and its partner marked - strings' and
    // comments' left out -; one with none marked alone.
    void theBracketIsMatched()
    {
        const QHash<int, int> pairs = qucs_s::python::bracketPairs("f(a[1], '(', {2})  # )\n)");
        QCOMPARE(pairs.value(1), 16);
        QCOMPARE(pairs.value(16), 1);
        QCOMPARE(pairs.value(3), 5);
        QCOMPARE(pairs.value(13), 15);
        QVERIFY(!pairs.contains(9));    // (in a string)
        QVERIFY(!pairs.contains(21));   // (in a comment)
        QCOMPARE(pairs.value(23, -2), -1);   // (closes nothing)
        QCOMPARE(qucs_s::python::bracketPairs("x = \"\"\"(\n\"\"\" + (1").value(15, -2), -1);
        QVERIFY(!qucs_s::python::bracketPairs("x = \"\"\"(\n\"\"\" + (1").contains(7));
        PythonDoc* py = open(write("match/m.py", "total = f(a[1], b)\nx = (1\n"));
        QVERIFY(py != nullptr && focused(py));
        place(py, 1, 9);   // before (
        QCOMPARE(py->bracketMarks(), std::make_pair(9, 17));
        QList<QTextEdit::ExtraSelection> marks = py->extraSelections();
        QVERIFY(std::count_if(marks.cbegin(), marks.cend(), [](const QTextEdit::ExtraSelection& s) {
                    return s.format.fontWeight() == QFont::Bold && s.cursor.selectedText().size() == 1;
                }) == 2);
        place(py, 1, 18);   // after )
        QCOMPARE(py->bracketMarks(), std::make_pair(17, 9));
        place(py, 1, 4);
        QCOMPARE(py->bracketMarks(), std::make_pair(-1, -1));
        place(py, 2, 4);   // ( with no partner
        QCOMPARE(py->bracketMarks(), std::make_pair(23, -1));
    }

    // A block, a function, a class, a cell folded away - its lines hidden,
    // a triangle and dots where it is -, opened again; Fold All, Unfold
    // All; the cursor going into a fold opens it; the line it is on changed,
    // it opens; a click on the triangle folds.
    void foldsAndUnfolds()
    {
        const QStringList lines = QString("def f(x):\n    if x:\n        return 1\n\n    return 2\n\nclass A:\n    pass\n"
                                          "# %% two\ny = 1\n\nz = 2\n# %% three\n").split('\n');
        QCOMPARE(qucs_s::python::foldRange(lines, 1), std::make_pair(2, 5));
        QCOMPARE(qucs_s::python::foldRange(lines, 2), std::make_pair(3, 3));
        QCOMPARE(qucs_s::python::foldRange(lines, 3), std::make_pair(0, 0));
        QCOMPARE(qucs_s::python::foldRange(lines, 7), std::make_pair(8, 8));
        QCOMPARE(qucs_s::python::foldRange(lines, 9), std::make_pair(10, 12));
        QCOMPARE(qucs_s::python::foldRange(lines, 13), std::make_pair(0, 0));

        PythonDoc* py = open(write("fold/f.py", lines.join('\n').toUtf8()));
        QVERIFY(py != nullptr && focused(py));
        QVERIFY(py->isFoldable(1) && py->isFoldable(2) && !py->isFoldable(3) && py->isFoldable(9));
        place(py, 3, 8);
        QTest::keyClick(py, Qt::Key_BracketLeft, Qt::ControlModifier | Qt::ShiftModifier);   // Fold: the innermost
        QCOMPARE(py->foldedLines(), QList<int>{2});
        QVERIFY(!py->document()->findBlockByNumber(2).isVisible());
        QVERIFY(py->document()->findBlockByNumber(4).isVisible());
        QCOMPARE(py->textCursor().blockNumber() + 1, 2);   // (on the line folded)
        pythonAction("pythonFold")->trigger();   // again: the function around it
        QCOMPARE(py->foldedLines(), (QList<int>{1, 2}));
        for (int k = 2; k <= 5; ++k) QVERIFY(!py->document()->findBlockByNumber(k - 1).isVisible());
        QVERIFY(py->document()->findBlockByNumber(6).isVisible());
        {   // Drawn: dots after the line folded.
            const QImage shown = py->viewport()->grab().toImage();
            const QRect line = py->cursorRect(QTextCursor(py->document()->findBlockByNumber(0)));
            const int x = py->fontMetrics().horizontalAdvance("def f(x):") + py->fontMetrics().horizontalAdvance("  ") + line.left() + 6;
            bool marked = false;
            for (int dx = 0; dx < 30 && !marked; ++dx) {
                const QColor c = shown.pixelColor(x + dx, line.center().y());
                marked = c != shown.pixelColor(shown.width() - 3, line.center().y()) && c.lightness() < 200;
            }
            QVERIFY(marked);
        }
        pythonAction("pythonUnfold")->trigger();   // (the cursor on line 1 now: its fold)
        QCOMPARE(py->foldedLines(), QList<int>{2});
        QVERIFY(py->document()->findBlockByNumber(4).isVisible());
        pythonAction("pythonUnfoldAll")->trigger();
        QVERIFY(py->foldedLines().isEmpty());
        for (QTextBlock b = py->document()->begin(); b.isValid(); b = b.next()) QVERIFY(b.isVisible());

        pythonAction("pythonFoldAll")->trigger();   // functions, classes, cells
        QCOMPARE(py->foldedLines(), (QList<int>{1, 7, 9}));
        QVERIFY(!py->document()->findBlockByNumber(9).isVisible());
        // The cursor into a fold: it opens.
        place(py, 10, 1);
        QCOMPARE(py->foldedLines(), (QList<int>{1, 7}));
        QVERIFY(py->document()->findBlockByNumber(9).isVisible());
        // Its line changed: it opens.
        place(py, 7, 7);
        py->insertPlainText("B");
        QTRY_COMPARE(py->foldedLines(), QList<int>{1});
        QVERIFY(py->document()->findBlockByNumber(7).isVisible());
        // A line typed in a fold, kept folded.
        QTextCursor in(py->document()->findBlockByNumber(2));
        in.insertText("        pass\n");
        QTRY_VERIFY(!py->document()->findBlockByNumber(2).isVisible());
        QCOMPARE(py->foldedLines(), QList<int>{1});
        // The triangle in the margin: a click folds and opens.
        QWidget* margin = marginOf(py);
        QVERIFY(margin != nullptr);
        QTest::mouseClick(margin, Qt::LeftButton, {}, QPoint(margin->width() - 3, yOf(py, margin, 1)));
        QVERIFY(py->foldedLines().isEmpty());
        QTest::mouseClick(margin, Qt::LeftButton, {}, QPoint(margin->width() - 3, yOf(py, margin, 1)));
        QCOMPARE(py->foldedLines(), QList<int>{1});
        QVERIFY(py->breakpoints().isEmpty());   // (no breakpoint set by it)
        QTest::mouseClick(margin, Qt::LeftButton, {}, QPoint(margin->width() - 3, yOf(py, margin, 1)));
    }

    // ------------------------------------------------------------------
    // The type checker

    // A type checker installed for the script's Python (a stand-in of
    // mypy's, then of pyright's): its findings shown with the check's, as
    // warnings, said on the toolbar; of the text as it is in the editor;
    // Off: gone.
    void typesAreChecked()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        write("tc/fake/mypy/__init__.py", "");
        write("tc/fake/mypy/version.py", "__version__ = '1.99.0'\n");
        write("tc/fake/mypy/api.py",
              "import sys\n"
              "def run(args):\n"
              "    target = args[-1]\n"
              "    shadow = args[args.index('--shadow-file') + 2] if '--shadow-file' in args else target\n"
              "    out = []\n"
              "    for k, line in enumerate(open(shadow).read().split('\\n'), 1):\n"
              "        if ': str = 1' in line:\n"
              "            c = line.index('1') + 1\n"
              "            out.append('%s:%d:%d:%d:%d: error: Incompatible types in assignment  [assignment]' % (target, k, c, k, c))\n"
              "            out.append('%s:%d:%d: note: The variable is str' % (target, k, c))\n"
              "    return '\\n'.join(out) + '\\n', '', 1 if out else 0\n");
        const EnvironmentReset reset("PYTHONPATH", QFile::encodeName(dir.filePath("tc/fake")) + QDir::listSeparator().toLatin1() + noJedi, true);
        PythonDoc::setTypeChecker("auto");
        PythonDoc* py = open(write("tc/t.py", "x: int = 1\n"));
        QVERIFY(py != nullptr);
        QSignalSpy typed(py, &PythonDoc::typeCheckFinished);
        QTextCursor end(py->document());
        end.movePosition(QTextCursor::End);
        end.insertText("name: str = 1\n");   // (not saved: the editor's text checked)
        QVERIFY(typed.wait(30000));
        QCOMPARE(py->lastTypeCheck().tool, QString("mypy 1.99.0"));
        QList<TextDoc::Diagnostic> found;
        for (const TextDoc::Diagnostic& d : py->diagnostics())
            if (d.source == "mypy") found << d;
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().line, 2);
        QCOMPARE(found.first().column, 13);
        QVERIFY(!found.first().error);
        QCOMPARE(found.first().message, QString("Incompatible types in assignment\nThe variable is str (mypy: assignment)"));   // (its note with it)
        QVERIFY2(py->checkedBy().contains("Types checked by mypy 1.99.0"), qPrintable(py->checkedBy()));
        // Fixed: gone with the next check.
        end.movePosition(QTextCursor::End);
        end.movePosition(QTextCursor::Up, QTextCursor::KeepAnchor);
        end.insertText("name: str = 'a'\n");
        QVERIFY(typed.wait(30000));
        QVERIFY(std::none_of(py->diagnostics().cbegin(), py->diagnostics().cend(), [](const auto& d) { return d.source == "mypy"; }));
        // pyright's, chosen.
        write("tc/bin/pyright",
              "#!/bin/sh\nexec python3 -c 'import json, sys\n"
              "t = sys.argv[-1]\n"
              "d = [{\"file\": t, \"severity\": \"error\", \"message\": \"Not assignable\", \"rule\": \"reportAssignmentType\", "
              "\"range\": {\"start\": {\"line\": k, \"character\": 4}, \"end\": {\"line\": k, \"character\": 5}}} "
              "for k, l in enumerate(open(t).read().split(\"\\n\")) if l.startswith(\"bad\")]\n"
              "print(json.dumps({\"version\": \"1.1.400\", \"generalDiagnostics\": d}))' \"$@\"\n");
        QFile::setPermissions(dir.filePath("tc/bin/pyright"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const EnvironmentReset path("PATH", QFile::encodeName(dir.filePath("tc/bin")) + ":" + qgetenv("PATH"), true);
        pythonAction("pythonTypeChecker_pyright")->trigger();
        QCOMPARE(PythonDoc::typeChecker(), QString("pyright"));
        end.movePosition(QTextCursor::End);
        end.insertText("bad = 1\n");
        QTRY_VERIFY_WITH_TIMEOUT(py->lastTypeCheck().tool == "pyright 1.1.400" && py->lastTypeCheck().problems.size() == 1
                                     && !py->typeChecking(), 30000);
        found.clear();
        for (const TextDoc::Diagnostic& d : py->diagnostics())
            if (d.source == "pyright") found << d;
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().line, 3);
        QCOMPARE(found.first().column, 5);
        QVERIFY(found.first().message.endsWith("(pyright: reportAssignmentType)"));
        // Off: gone.
        pythonAction("pythonTypeChecker_off")->trigger();
        QVERIFY(std::none_of(py->diagnostics().cbegin(), py->diagnostics().cend(), [](const auto& d) { return !d.source.isEmpty(); }));
        QVERIFY(!py->checkedBy().contains("pyright"));
    }

private:
    // Sets an environment variable for a test, puts it back after.
    struct EnvironmentReset {
        QByteArray name, was;
        bool had;
        EnvironmentReset(const char* n, const QByteArray& value, bool set) : name(n), was(qgetenv(n)), had(qEnvironmentVariableIsSet(n))
        {
            if (set) qputenv(n, value);
            else qunsetenv(n);
        }
        ~EnvironmentReset()
        {
            if (had) qputenv(name.constData(), was);
            else qunsetenv(name.constData());
        }
    };
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication application(one, argv);
    TestPythonIde test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_python_ide.moc"
