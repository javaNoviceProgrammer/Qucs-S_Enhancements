/*
 * test_python_tools.cpp - what the Python editor does with a script beyond
 * the check and the completion (pythondoc.h, pythonassist.cpp, pythonrun.h,
 * qucs_python.cpp, python/): the qucs module reads a dataset - text and
 * binary, with numpy and without - writes one Qucs-S reads, and simulates a
 * schematic; the call's signature above it; what a name is; Go to
 * Definition and Go Back; the outline; the cells, and lines run in the
 * Python Shell; Format Document and Fix Problems; breakpoints; the
 * debugger, stepped, its frames, variables and evaluations, and stopped by
 * an exception.
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QToolTip>
#include <QTreeWidget>

#include "config.h"
#include "dataset.h"
#include "datasetfile.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "processconsole.h"
#include "pythondoc.h"
#include "pythonrun.h"
#include "qucs.h"
#include "settings.h"

#include <cmath>

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

class TestPythonTools : public QObject
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
    // The cursor at line \a line (from 1), column \a column (from 0).
    static void place(PythonDoc* py, int line, int column)
    {
        const QTextBlock block = py->document()->findBlockByNumber(line - 1);
        QTextCursor at(block);
        at.setPosition(block.position() + column);
        py->setTextCursor(at);
    }
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
    // \a code run by python3 in the scripts' environment (the qucs module
    // on its path) and \a more on top; its output, or what it said.
    QString runPython(const QString& code, const QProcessEnvironment& more = {}, int ms = 60000)
    {
        QProcess p;
        QProcessEnvironment environment = qucs_s::python::scriptEnvironment();
        for (const QString& key : more.keys()) environment.insert(key, more.value(key));
        p.setProcessEnvironment(environment);
        p.setWorkingDirectory(dir.path());
        p.start(python, {"-c", code});
        if (!p.waitForFinished(ms)) return QStringLiteral("(did not finish)");
        return QString::fromUtf8(p.readAllStandardOutput()).trimmed() + QString::fromUtf8(p.readAllStandardError()).trimmed();
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
        write("nojedi/jedi/__init__.py", "raise ImportError('hidden for the test')\n");
        noJedi = QFile::encodeName(dir.filePath("nojedi"));
        qputenv("PYTHONPATH", noJedi);
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
    // The qucs module

    // On the path of what runs a script, written from the resources; a
    // dataset in text and in binary (as Qucs-S writes them) read into arrays
    // of the right shape - the outer sweep first - and kind; without numpy,
    // lists; a schematic's newest dataset; one written read by Qucs-S.
    void theModuleReadsADataset()
    {
        const QString folder = qucs_s::python::moduleFolder();
        QVERIFY(QFileInfo(folder + "/qucs.py").isFile());
        QVERIFY(QFileInfo(folder + "/_qucs_shell.py").isFile());
        QVERIFY(qucs_s::python::scriptEnvironment().value("PYTHONPATH").startsWith(folder + QDir::listSeparator()));
        QVERIFY(qucs_s::python::shellEnvironment().contains("PYTHONPATH=" + folder + QDir::listSeparator() + QString::fromLocal8Bit(noJedi)));
        // (Not in a test's program: qucs.simulate() would run it.)
        QVERIFY(!qucs_s::python::scriptEnvironment().contains("QUCS_S_EXECUTABLE"));
        if (python.isEmpty()) QSKIP("no python3 here");

        // Text: two sweeps, complex values.
        write("data/sweep.dat",
              "<Qucs Dataset 26.1.7>\n<indep f 3>\n1\n2\n3\n</indep>\n<indep C 2>\n+1e-12\n+2e-12\n</indep>\n"
              "<dep v f C>\n+1+j1\n+2-j2\n+3+j0\n+4+j4\n+5-j5\n-j6\n</dep>\n<dep g f>\n0.5\n0.25\n0.125\n</dep>\n");
        // Binary, as the simulators' datasets are written by default.
        {
            QFile out(dir.filePath("data/bin.dat.ngspice"));
            QVERIFY(out.open(QIODevice::ReadWrite | QIODevice::Truncate));
            qucs_s::datasetfile::BinaryWriter w(&out);
            w.begin("indep time 4");
            for (const double t : {0.0, 1e-9, 2e-9, 3e-9}) w.real(t);
            w.end();
            w.begin("dep tran.v(out) time");
            for (const double v : {0.0, 0.5, 0.75, 0.875}) w.real(v);
            w.end();
            w.begin("indep frequency 2");
            w.real(1e6);
            w.real(2e6);
            w.end();
            w.begin("dep ac.v(out) frequency");
            w.complex(1, -1);
            w.complex(0.5, 0.25);
            w.end();
            QString error;
            QVERIFY2(w.finish(&error), qPrintable(error));
        }
        const QString read = runPython(
            "import json, qucs, numpy\n"
            "d = qucs.load('data/sweep.dat')\n"
            "v = d['v']\n"
            "b = qucs.load('data/bin.dat.ngspice')\n"
            "print(json.dumps({'names': list(d), 'shape': list(v.shape), 'complex': v.dtype.kind == 'c',\n"
            "  'v10': [v[1][0].real, v[1][0].imag], 'v12': [v[1][2].real, v[1][2].imag], 'over': d.dependencies('v'),\n"
            "  'g': list(d['g']), 'indep': d.independent, 'dep': d.dependent,\n"
            "  'bnames': list(b), 'tran': list(b['tran.v(out)']), 'ac1': [b['ac.v(out)'][1].real, b['ac.v(out)'][1].imag],\n"
            "  'bover': b.dependencies('ac.v(out)'), 'said': str(d).split(chr(10))[0]}))\n");
        const QJsonObject o = QJsonDocument::fromJson(read.toUtf8()).object();
        QVERIFY2(!o.isEmpty(), qPrintable(read));
        QCOMPARE(o["names"].toArray(), (QJsonArray{"f", "C", "v", "g"}));
        QCOMPARE(o["shape"].toArray(), (QJsonArray{2, 3}));   // C (outer), f (inner: the first in the file)
        QVERIFY(o["complex"].toBool());
        QCOMPARE(o["v10"].toArray(), (QJsonArray{4.0, 4.0}));
        QCOMPARE(o["v12"].toArray(), (QJsonArray{0.0, -6.0}));
        QCOMPARE(o["over"].toArray(), (QJsonArray{"C", "f"}));
        QCOMPARE(o["g"].toArray(), (QJsonArray{0.5, 0.25, 0.125}));
        QCOMPARE(o["indep"].toArray(), (QJsonArray{"f", "C"}));
        QCOMPARE(o["dep"].toArray(), (QJsonArray{"v", "g"}));
        QCOMPARE(o["bnames"].toArray(), (QJsonArray{"time", "tran.v(out)", "frequency", "ac.v(out)"}));
        QCOMPARE(o["tran"].toArray(), (QJsonArray{0.0, 0.5, 0.75, 0.875}));
        QCOMPARE(o["ac1"].toArray(), (QJsonArray{0.5, 0.25}));
        QCOMPARE(o["bover"].toArray(), (QJsonArray{"frequency"}));
        QCOMPARE(o["said"].toString(), QString("sweep.dat: 4 variables"));

        // Without numpy: nested lists, the same values.
        write("nonumpy/numpy/__init__.py", "raise ImportError('hidden for the test')\n");
        QProcessEnvironment without;
        without.insert("PYTHONPATH", qucs_s::python::moduleFolder() + QDir::listSeparator() + dir.filePath("nonumpy"));
        const QString lists = runPython("import qucs\n"
                                        "d = qucs.load('data/sweep.dat'); b = qucs.load('data/bin.dat.ngspice')\n"
                                        "print(type(d['v']).__name__, d['v'][1][2], b['ac.v(out)'][0], b['tran.v(out)'][3])\n",
                                        without);
        QCOMPARE(lists, QString("list -6j (1-1j) 0.875"));

        // A schematic: its dataset as it names it, the newest of the
        // simulators' (or the one asked for).
        write("data/amp.sch", "<Qucs Schematic 26.1.7>\n<Properties>\n  <DataSet=results.dat>\n</Properties>\n");
        QFile::copy(dir.filePath("data/sweep.dat"), dir.filePath("data/results.dat.xyce"));
        QTest::qWait(1100);   // (a newer file's time)
        QFile::copy(dir.filePath("data/bin.dat.ngspice"), dir.filePath("data/results.dat.ngspice"));
        QCOMPARE(runPython("import qucs; print(qucs.load('data/amp.sch').independent)"), QString("['time', 'frequency']"));
        QCOMPARE(runPython("import qucs; print(qucs.load('data/amp.sch', 'xyce').independent)"), QString("['f', 'C']"));
        QVERIFY(runPython("import qucs; qucs.load('data/amp.sch', 'spiceopus')").contains("FileNotFoundError"));
        QVERIFY(runPython("import qucs; qucs.load('data/amp.sch', 'pspice')").contains("ValueError"));

        // Written: read by Qucs-S as it reads a simulator's, sweeps and all.
        const QString saved = runPython(
            "import qucs, numpy as np\n"
            "f = np.array([1.0, 2.0, 3.0]); c = np.array([10.0, 20.0])\n"
            "v = np.array([[1+1j, 2, 3], [4, 5-5j, 6.5]])\n"
            "qucs.save('data/out.dat', {'c': c, 'f': f, 'v': v, 'gain': [0.1, 0.2, 0.3]},\n"
            "          independent=['c', 'f'], dependencies={'gain': ['f']})\n"
            "r = qucs.load('data/out.dat')\n"
            "print(bool((r['v'] == v).all()), r.dependencies('v'), r.dependencies('gain'))\n"
            "try:\n"
            "    qucs.save('data/bad.dat', {'f': f, 'w': [1, 2]}, independent=['f'])\n"
            "except ValueError as e:\n"
            "    print('refused:', e)\n");
        QVERIFY2(saved.startsWith("True ['c', 'f'] ['f']"), qPrintable(saved));
        QVERIFY2(saved.contains("refused: 'w' has 2 values, not the 3"), qPrintable(saved));
        QVERIFY(!QFileInfo::exists(dir.filePath("data/bad.dat")));
        qucs_s::dataset::Dataset back;
        QString error;
        QVERIFY2(back.read(dir.filePath("data/out.dat"), &error), qPrintable(error));
        const qucs_s::dataset::Variable* v = back.find("v");
        QVERIFY(v != nullptr && v->isComplex());
        QCOMPARE(v->dependencies, (QStringList{"f", "c"}));   // (the file's order: the fastest first)
        QCOMPARE(v->size(), 6);
        QCOMPARE(v->re.at(4), 5.0);
        QCOMPARE(v->im.at(4), -5.0);
        QCOMPARE(v->re.at(5), 6.5);
        QCOMPARE(back.find("gain")->dependencies, QStringList{"f"});
    }

    // qucs.simulate(): the schematic simulated by Qucs-S's own program
    // (headless), its dataset returned; an error said.
    void theModuleSimulates()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        if (QStandardPaths::findExecutable("ngspice").isEmpty()) QSKIP("no ngspice here");
        QVERIFY(QDir().mkpath(dir.filePath("sim")));
        QVERIFY(QFile::copy(QStringLiteral(QUCS_PYTHON_SOURCE "/rc_tran_ac.sch"), dir.filePath("sim/rc.sch")));
        QProcessEnvironment isolated;
        isolated.insert("QUCS_S_EXECUTABLE", QStringLiteral(QUCS_BINARY));
        isolated.insert("HOME", dir.filePath("simhome"));
        isolated.insert("QUCS_SETTINGS_DIR", dir.filePath("simsettings"));
        isolated.insert("QUCS_TRASH_DIR", dir.filePath("simtrash"));
        isolated.insert("QUCS_CACHE_DIR", dir.filePath("simcache"));
        QDir().mkpath(dir.filePath("simhome"));
        const QString said = runPython("import qucs\n"
                                       "d = qucs.simulate('sim/rc.sch')\n"
                                       "print(d.path.endswith('rc_tran_ac.dat.ngspice'), d.independent, len(d['tran.v(vn3)']) > 10)\n"
                                       "try:\n"
                                       "    qucs.simulate('sim/none.sch')\n"
                                       "except RuntimeError as e:\n"
                                       "    print('refused')\n",
                                       isolated, 120000);
        QVERIFY2(said.startsWith("True ['frequency', 'time'] True"), qPrintable(said));
        QVERIFY2(said.contains("refused"), qPrintable(said));
    }

    // The completer knows the module: its functions after qucs. (read,
    // not run, as any module beside a script).
    void theModuleIsCompleted()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("mod/c.py", "import qucs\n"));
        QVERIFY(py != nullptr);
        QVERIFY(focused(py));
        py->moveCursor(QTextCursor::End);
        QTest::keyClicks(py, "qucs.lo");
        py->complete(true);
        QTRY_VERIFY_WITH_TIMEOUT(py->completing(), 15000);
        QCOMPARE(py->completionNames(), QStringList{"load"});
        py->completer()->popup()->hide();
    }

    // Run gives the script the module: on its path, from the resources.
    void runHasTheModule()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("mod/uses.py", "import qucs\nprint('module at', qucs.__file__)\n"));
        QVERIFY(py != nullptr);
        QSignalSpy done(app->pythonRunConsole(), &PythonRunConsole::finished);
        QVERIFY(app->runPython(py));
        QVERIFY(done.count() > 0 || done.wait(20000));
        QCOMPARE(app->pythonRunConsole()->exitCode(), 0);
        QVERIFY2(app->pythonRunConsole()->outputText().contains("module at " + QDir(qucs_s::python::moduleFolder()).filePath("qucs.py")),
                 qPrintable(app->pythonRunConsole()->outputText()));
    }

    // ------------------------------------------------------------------
    // Signatures, help, definitions

    // As one types a call's bracket: its parameters above it, the one being
    // written in bold, a keyword argument's own; gone when the call is
    // closed, on Escape. Asked for (Show Signature), anywhere in a call.
    void theCallsSignature()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        PythonDoc* py = open(write("sig/s.py", "import math\n\ndef gain(x, y=3, *, scale=1.0):\n    \"\"\"The gain.\"\"\"\n"
                                              "    return x * y * scale\n\n"));
        QVERIFY(py != nullptr);
        QVERIFY(focused(py));
        py->moveCursor(QTextCursor::End);
        QTest::keyClicks(py, "gain(");
        QTRY_VERIFY_WITH_TIMEOUT(py->signatureShown(), 15000);
        QCOMPARE(py->lastSignature().name, QString("gain"));
        QCOMPARE(py->lastSignature().params, (QStringList{"x", "y=3", "*", "scale=1.0"}));
        QCOMPARE(py->lastSignature().index, 0);
        QVERIFY(py->signatureTip()->text().contains("<b><u>x</u></b>"));
        QVERIFY(py->signatureTip()->text().contains("The gain."));
        QSignalSpy answered(py, &PythonDoc::signatureAnswered);
        QTest::keyClicks(py, "1, ");
        QTRY_COMPARE_WITH_TIMEOUT(py->lastSignature().index, 1, 15000);
        QVERIFY(py->signatureTip()->text().contains("<b><u>y=3</u></b>"));
        // The cursor moved back in the call: the argument it is in.
        QTest::keyClick(py, Qt::Key_Left);
        QTest::keyClick(py, Qt::Key_Left);
        QTest::keyClick(py, Qt::Key_Left);
        QTRY_COMPARE_WITH_TIMEOUT(py->lastSignature().index, 0, 15000);
        py->moveCursor(QTextCursor::EndOfBlock);
        QTRY_COMPARE_WITH_TIMEOUT(py->lastSignature().index, 1, 15000);
        // A string's commas and brackets are no call's: still the argument y.
        {
            QSignalSpy again(py, &PythonDoc::signatureAnswered);
            QTest::keyClicks(py, "'a, (b'");
            QTRY_VERIFY_WITH_TIMEOUT(again.count() > 0, 15000);
            QTest::qWait(PythonDoc::kCompleteDelay + 400);   // (the last answer in)
            QVERIFY(py->signatureShown());
            QCOMPARE(py->lastSignature().name, QString("gain"));
            QCOMPARE(py->lastSignature().index, 1);
            for (int k = 0; k < 7; ++k) QTest::keyClick(py, Qt::Key_Backspace);   // (the string gone)
        }
        QTest::keyClicks(py, "scale=");
        QTRY_COMPARE_WITH_TIMEOUT(py->lastSignature().index, 3, 15000);
        // Just above the call's line, at its bracket.
        {
            QTextCursor bracket = py->textCursor();
            bracket.setPosition(bracket.block().position() + 4);   // gain(
            const QPoint at = py->viewport()->mapToGlobal(py->cursorRect(bracket).topLeft());
            const QRect tip = py->signatureTip()->geometry();
            QVERIFY2(tip.bottom() <= at.y() && tip.bottom() >= at.y() - 8, qPrintable(QString("%1 %2").arg(tip.bottom()).arg(at.y())));
            QVERIFY2(std::abs(tip.left() - at.x()) <= 2, qPrintable(QString("%1 %2").arg(tip.left()).arg(at.x())));
        }
        QTest::keyClicks(py, "2)");
        QTRY_VERIFY_WITH_TIMEOUT(!py->signatureShown(), 15000);

        // A standard module's function; Escape takes it away.
        QTest::keyClick(py, Qt::Key_Return);
        QTest::keyClicks(py, "math.sqrt(");
        QTRY_VERIFY_WITH_TIMEOUT(py->signatureShown(), 15000);
        QCOMPARE(py->lastSignature().name, QString("sqrt"));
        QTest::keyClick(py, Qt::Key_Escape);
        QVERIFY(!py->signatureShown());
        QVERIFY(py->hasFocus());   // (the window's Escape not taken)

        // Asked for, with as-you-type off.
        PythonDoc::setCompleteAsYouType(false);
        QTest::keyClicks(py, "2");
        QTest::keyClick(py, Qt::Key_Return);
        QTest::keyClicks(py, "gain(7, ");
        QTest::qWait(PythonDoc::kCompleteDelay + 400);
        QVERIFY(!py->signatureShown());
        pythonAction("pythonSignature")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(py->signatureShown(), 15000);
        QCOMPARE(py->lastSignature().index, 1);
        PythonDoc::setCompleteAsYouType(true);
        // Not in a string.
        QTest::keyClick(py, Qt::Key_Escape);
        QTest::keyClick(py, Qt::Key_Return);
        QTest::keyClicks(py, "s = 'gain(");
        QTest::qWait(PythonDoc::kCompleteDelay + 600);
        QVERIFY(!py->signatureShown());
        // A method called on an object (the completer's own words: the one
        // method of that name of the script's classes).
        PythonDoc::setCompleteAsYouType(true);
        QTest::keyClick(py, Qt::Key_Escape);
        py->moveCursor(QTextCursor::End);
        QTest::keyClick(py, Qt::Key_Return);
        QTest::keyClicks(py, "class Amp:");
        QTest::keyClick(py, Qt::Key_Return);
        QTest::keyClicks(py, "def boost(self, by, *, db=False):");
        QTest::keyClick(py, Qt::Key_Return);
        QTest::keyClicks(py, "return by");
        QTest::keyClick(py, Qt::Key_Return);
        QTest::keyClick(py, Qt::Key_Backtab);
        QTest::keyClick(py, Qt::Key_Backtab);
        QTest::keyClicks(py, "Amp().boost(");
        QTRY_VERIFY_WITH_TIMEOUT(py->signatureShown() && py->lastSignature().name == "boost", 15000);
        QCOMPARE(py->lastSignature().params, (QStringList{"by", "*", "db=False"}));
    }

    // The mouse resting on a name: what it is (a standard function, the
    // script's own, a module beside it), the line's errors with it; nothing
    // on the paper after a line, in a comment.
    void whatANameIs()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        write("help/mymod.py", "\"\"\"Helpers.\"\"\"\ndef helper(a, b=2):\n    \"\"\"Adds them.\"\"\"\n    return a + b\n");
        PythonDoc* py = open(write("help/h.py", "import math, mymod\n\ndef gain(x):\n    \"\"\"The gain of x.\"\"\"\n    return x\n\n"
                                               "y = math.sqrt(gain(2)) + mymod.helper(1)  # sqrt\nw = gain(\n"));
        QVERIFY(py != nullptr);
        const auto at = [py](int line, int column) {
            const QTextBlock block = py->document()->findBlockByNumber(line - 1);
            QTextCursor c(block);
            c.setPosition(block.position() + column);
            return py->cursorRect(c).center() + QPoint(2, 0);
        };
        QSignalSpy answered(py, &PythonDoc::helpAnswered);
        QVERIFY(py->showHelpAt(at(7, 10)));   // sqrt
        QVERIFY(answered.wait(15000));
        QVERIFY2(py->lastHelp().startsWith("math.sqrt(x, /)\nReturn the square root of x."), qPrintable(py->lastHelp()));
        QVERIFY(py->showHelpAt(at(7, 15)));   // gain
        QVERIFY(answered.wait(15000));
        QCOMPARE(py->lastHelp(), QString("def gain(x)\nThe gain of x."));
        QVERIFY(py->showHelpAt(at(7, 34)));   // helper
        QVERIFY(answered.wait(15000));
        QCOMPARE(py->lastHelp(), QString("def helper(a, b=2)\nAdds them."));
        QVERIFY(!py->showHelpAt(at(7, 45)));   // the comment's sqrt
        {   // A function after a bracket left open, which takes the lines after it in.
            PythonDoc* open2 = open(write("help/open.py", "data = [1,\nresult = 2\ndef later(q):\n    \"\"\"Later.\"\"\"\n    return q\n\nlater(1)\n"));
            QVERIFY(open2 != nullptr);
            QSignalSpy later(open2, &PythonDoc::helpAnswered);
            QTextCursor c(open2->document()->findBlockByNumber(6));
            QVERIFY(open2->showHelpAt(open2->cursorRect(c).center() + QPoint(4, 0)));
            QVERIFY(later.wait(15000));
            QCOMPARE(open2->lastHelp(), QString("def later(q)\nLater."));
            app->showDocument(py);
        }
        QVERIFY(!py->showHelpAt(QPoint(py->viewport()->width() - 5, at(1, 1).y())));   // past the line (its last word mymod)
        // As the mouse gives it: a tooltip, the line's error with it.
        if (!py->checked() || py->checking()) {
            QSignalSpy check(py, &PythonDoc::checkFinished);
            QVERIFY(check.wait(20000));
        }
        QVERIFY(!py->diagnostics().isEmpty());
        const int errorLine = py->diagnostics().first().line;
        const QString lineText = py->document()->findBlockByNumber(errorLine - 1).text();
        QVERIFY(lineText.startsWith("w = gain("));
        QHelpEvent hover(QEvent::ToolTip, at(errorLine, 5), py->viewport()->mapToGlobal(at(errorLine, 5)));
        QApplication::sendEvent(py->viewport(), &hover);
        QVERIFY(answered.wait(15000));
        QTRY_VERIFY_WITH_TIMEOUT(QToolTip::isVisible(), 5000);
        QVERIFY2(QToolTip::text().contains("Error:") && QToolTip::text().contains("<b>def gain(x)</b>"), qPrintable(QToolTip::text()));
        QToolTip::hideText();
    }

    // F12 (and Ctrl+click): the script's own function, a module beside it
    // (opened there), Python's library (opened read-only, not checked); a
    // builtin said; Go Back where it left, again and again.
    void goesToTheDefinition()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        write("def/mymod.py", "x = 1\n\ndef helper(a):\n    return a\n");
        PythonDoc* py = open(write("def/d.py", "import json, mymod\n\ndef gain(x):\n    return x\n\n"
                                              "y = gain(1) + mymod.helper(2)\nprint(json.dumps(y))\n"));
        {   // A method of an object: the one of the script's classes so named.
            PythonDoc* m = open(write("def/m.py", "class Amp:\n    def boost(self, by):\n        return by\n\nAmp().boost(1)\n"));
            QVERIFY(m != nullptr);
            QSignalSpy found(m, &PythonDoc::definitionAnswered);
            place(m, 5, 8);   // boost
            m->goToDefinition();
            QVERIFY(found.wait(15000));
            QCOMPARE(m->textCursor().blockNumber() + 1, 2);
            pythonAction("pythonBack")->trigger();
            QCOMPARE(m->textCursor().blockNumber() + 1, 5);
            app->showDocument(py);
        }
        {   // A variable: where it is given its value.
            QSignalSpy first(py, &PythonDoc::definitionAnswered);
            place(py, 7, 18);   // y, in print(json.dumps(y))
            py->goToDefinition();
            QVERIFY(first.wait(15000));
            QCOMPARE(py->textCursor().blockNumber() + 1, 6);
            pythonAction("pythonBack")->trigger();
        }
        QVERIFY(py != nullptr);
        QVERIFY(focused(py));
        QSignalSpy answered(py, &PythonDoc::definitionAnswered);
        place(py, 6, 5);   // gain
        QTest::keyClick(py, Qt::Key_F12);
        QVERIFY(answered.wait(15000));
        QCOMPARE(py->lastDefinition().file, QString());
        QCOMPARE(py->textCursor().blockNumber() + 1, 3);
        QCOMPARE(py->textCursor().positionInBlock(), 0);

        // Ctrl+click on helper: the module beside, at its def.
        place(py, 6, 0);
        QTextCursor helper(py->document()->findBlockByNumber(5));
        helper.setPosition(helper.block().position() + 22);
        QTest::mouseClick(py->viewport(), Qt::LeftButton, Qt::ControlModifier, py->cursorRect(helper).center());
        QTRY_VERIFY_WITH_TIMEOUT(app->DocumentTab->currentWidget() != py, 15000);
        auto* mod = qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(mod != nullptr && mod->getDocName().endsWith("mymod.py"));
        QCOMPARE(mod->textCursor().blockNumber() + 1, 3);
        QVERIFY(!mod->isLibraryFile() && !mod->isReadOnly());

        // Python's library: read-only, said so, not checked.
        app->showDocument(py);
        place(py, 7, 13);   // dumps
        pythonAction("pythonDefinition")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget()) != py
                                     && qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget()) != mod, 15000);
        auto* library = qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(library != nullptr && library->getDocName().endsWith("json/__init__.py"));
        QVERIFY(library->isLibraryFile() && library->isReadOnly());
        QVERIFY(library->findChild<QLabel*>("pythonLibraryNote")->isVisible());
        QVERIFY(library->textCursor().block().text().startsWith("def dumps("));
        QVERIFY(library->diagnostics().isEmpty() && !library->checking());
        QTest::qWait(1500);
        QVERIFY(!library->checked());   // (the check of its opening stopped, none after)

        // Back, and back again.
        QAction* back = pythonAction("pythonBack");
        QVERIFY(back->isEnabled());
        library->setFocus();
        back->trigger();
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(py));
        QCOMPARE(py->textCursor().blockNumber() + 1, 7);
        back->trigger();
        QCOMPARE(py->textCursor().blockNumber() + 1, 6);
        QCOMPARE(py->textCursor().positionInBlock(), 22);   // (the click's place)
        back->trigger();
        QCOMPARE(py->textCursor().blockNumber() + 1, 6);
        QCOMPARE(py->textCursor().positionInBlock(), 5);
        QVERIFY(!back->isEnabled());

        // A builtin: said, nowhere to go.
        place(py, 7, 2);   // print
        pythonAction("pythonDefinition")->trigger();
        QVERIFY(answered.wait(15000));
        QVERIFY(py->lastDefinition().builtin);
        QVERIFY2(app->statusBar()->currentMessage().contains("print is built into Python"), qPrintable(app->statusBar()->currentMessage()));
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(py));
        // A stub (jedi's builtins are in one): a Python script too.
        QVERIFY(QucsApp::isPythonFile("builtins.pyi"));
        QVERIFY(open(write("def/stub.pyi", "def f(x: int) -> int: ...\n")) != nullptr);
    }

    // jedi's answers, when the interpreter has it: a stand-in's (as jedi
    // gives them: its signatures, help, definitions), said as jedi's; and
    // this machine's jedi, when it has one, for the script's own names.
    void jedisSignaturesHelpAndDefinitions()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        const auto ask = [this](const QByteArray& pythonPath, const QString& kind, const QString& source, int line, int column) {
            QProcess p;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            if (pythonPath.isEmpty()) environment.remove("PYTHONPATH");
            else environment.insert("PYTHONPATH", QString::fromLocal8Bit(pythonPath));
            p.setProcessEnvironment(environment);
            p.setWorkingDirectory(qucs_s::python::neutralFolder());
            p.start(python, {"-u", "-c", qucs_s::python::completerProgram()});
            if (!p.waitForStarted(10000)) return qucs_s::python::Answer();
            p.write(QJsonDocument(QJsonObject{{"id", 1}, {"kind", kind}, {"source", source}, {"line", line}, {"column", column},
                                              {"path", dir.filePath("jedi/s.py")}})
                        .toJson(QJsonDocument::Compact) + '\n');
            QByteArray answer;
            while (!answer.endsWith('\n') && p.waitForReadyRead(30000)) answer += p.readLine();
            p.closeWriteChannel();
            p.waitForFinished(10000);
            return qucs_s::python::readAnswer(answer);
        };
        write("standin/jedi/__init__.py",
              "__version__ = '9.9.9'\n"
              "class P:\n"
              "    def __init__(self, s): self.s = s\n"
              "    def to_string(self): return self.s\n"
              "class Sig:\n"
              "    name, index, bracket_start = 'fake', 1, (2, 4)\n"
              "    params = [P('a'), P('b=2')]\n"
              "    def docstring(self, raw=False): return 'A fake.\\n\\nMore.'\n"
              "class Name:\n"
              "    def __init__(self, path, line):\n"
              "        self.name, self.full_name, self.type = 'fake', 'mod.fake', 'function'\n"
              "        self.module_path, self.line, self.column = path, line, 4\n"
              "    def docstring(self): return 'fake(a, b=2)\\n\\nA fake.'\n"
              "    def in_builtin_module(self): return self.module_path is None\n"
              "class Script:\n"
              "    def __init__(self, code=None, path=None): self.code, self.path = code, path\n"
              "    def get_signatures(self, line, column): return [Sig()]\n"
              "    def help(self, line, column): return [Name(None, None)]\n"
              "    def goto(self, line, column, follow_imports=False):\n"
              "        return [Name(None, None)] if 'builtin' in self.code else [Name(self.path, 7)]\n"
              "    def infer(self, line, column): return []\n");
        const QByteArray standin = QFile::encodeName(dir.filePath("standin"));
        qucs_s::python::Answer a = ask(standin, "signature", "x = fake(1, ", 1, 12);
        QCOMPARE(a.engine, QString("jedi 9.9.9"));
        QVERIFY(a.signature.valid);
        QCOMPARE(a.signature.params, (QStringList{"a", "b=2"}));
        QCOMPARE(a.signature.index, 1);
        QCOMPARE(a.signature.doc, QString("A fake."));
        QCOMPARE(a.signature.openLine, 2);
        QCOMPARE(a.signature.openColumn, 4);
        a = ask(standin, "help", "fake", 1, 1);
        QCOMPARE(a.help.title, QString("mod.fake"));
        QCOMPARE(a.help.text, QString("fake(a, b=2)\n\nA fake."));
        a = ask(standin, "definition", "fake", 1, 1);
        QVERIFY(a.place.valid && !a.place.builtin);
        QCOMPARE(a.place.file, QString());   // (the script itself)
        QCOMPARE(a.place.line, 7);
        QCOMPARE(a.place.column, 4);
        a = ask(standin, "definition", "builtin", 1, 1);
        QVERIFY(a.place.builtin);

        // This machine's jedi.
        QProcess probe;
        QProcessEnvironment plain = QProcessEnvironment::systemEnvironment();
        plain.remove("PYTHONPATH");   // (not the test's stand-ins)
        probe.setProcessEnvironment(plain);
        probe.start(python, {"-c", "import jedi"});
        probe.waitForFinished(20000);
        if (probe.exitCode() != 0) QSKIP("no jedi here");
        const QString script = "def gain(x, y=3):\n    '''The gain.'''\n    return x * y\n\ngain(1, ";
        a = ask({}, "signature", script, 5, 8);
        QVERIFY2(a.engine.startsWith("jedi "), qPrintable(a.engine));
        QCOMPARE(a.signature.name, QString("gain"));
        QCOMPARE(a.signature.index, 1);
        QCOMPARE(a.signature.doc, QString("The gain."));
        a = ask({}, "help", script, 5, 1);
        QVERIFY2(a.help.text.contains("The gain."), qPrintable(a.help.text));
        a = ask({}, "definition", script, 5, 1);
        QCOMPARE(a.place.file, QString());
        QCOMPARE(a.place.line, 1);
        a = ask({}, "definition", "import json\njson.dumps", 2, 7);
        QVERIFY(a.place.library);
    }

    // ------------------------------------------------------------------
    // The outline, the cells

    void theOutline()
    {
        const QList<qucs_s::python::OutlineEntry> outline = qucs_s::python::outlineOf(
            "import os\n\nclass Amp:\n    \"\"\"An amplifier.\"\"\"\n    def __init__(self):\n        pass\n\n"
            "    # a comment, as far left as nothing\n    async def gain(self, f):\n        return f\n\nx = 1\n\ndef main():\n    pass\n");
        QCOMPARE(outline.size(), 4);
        QCOMPARE(outline.at(0).name, QString("Amp"));
        QCOMPARE(outline.at(0).kind, QString("class"));
        QCOMPARE(outline.at(0).line, 3);
        QCOMPARE(outline.at(0).lastLine, 10);
        QCOMPARE(outline.at(1).name, QString("__init__"));
        QCOMPARE(outline.at(1).depth, 1);
        QCOMPARE(outline.at(1).lastLine, 6);
        QCOMPARE(outline.at(2).name, QString("gain"));
        QCOMPARE(outline.at(2).kind, QString("def"));
        QCOMPARE(outline.at(2).lastLine, 10);
        QCOMPARE(outline.at(3).name, QString("main"));
        QCOMPARE(outline.at(3).lastLine, 15);

        PythonDoc* py = open(write("outline/o.py", "x = 1\n\nclass Amp:\n    def gain(self):\n        return 2\n\ny = 3\n"));
        QVERIFY(py != nullptr);
        QComboBox* list = py->outlineList();
        QVERIFY(list->isVisible());
        QVERIFY(list->parentWidget()->geometry().bottom() < py->viewport()->geometry().top());   // (above the text)
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->itemText(2), QString("    def gain"));
        place(py, 5, 8);
        QCOMPARE(list->currentIndex(), 2);
        place(py, 7, 0);
        QCOMPARE(list->currentIndex(), 0);
        emit list->activated(1);
        QCOMPARE(py->textCursor().blockNumber() + 1, 3);
        // As it is edited.
        py->moveCursor(QTextCursor::End);
        py->insertPlainText("def more():\n    pass\n");
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 4, 3000);
        QCOMPARE(list->itemText(3), QString("def more"));
    }

    // Cells: # %% and its kin begin one; each runs in the Python Shell, in
    // its variables, a traceback at the script's line, the last expression's
    // value shown; Run Cell and Advance goes on to the next. Run Selection
    // or Line (Shift+Return): the selection, or the line and on.
    void cellsAndLinesRunInTheShell()
    {
        QVERIFY(qucs_s::python::isCellMarker("# %%"));
        QVERIFY(qucs_s::python::isCellMarker("  #%% setup"));
        QVERIFY(qucs_s::python::isCellMarker("# In[3]:"));
        QVERIFY(qucs_s::python::isCellMarker("# <codecell>"));
        QVERIFY(!qucs_s::python::isCellMarker("x = 1  # %%"));
        QVERIFY(!qucs_s::python::isCellMarker("# 100%% done"));
        const QString cells = "a = 1\n# %% two\nb = 2\nc = 3\n# %%\nd = 4\n";
        QCOMPARE(qucs_s::python::cellAround(cells, 1), std::make_pair(1, 1));
        QCOMPARE(qucs_s::python::cellAround(cells, 3), std::make_pair(2, 4));
        QCOMPARE(qucs_s::python::cellAround(cells, 2), std::make_pair(2, 4));
        QCOMPARE(qucs_s::python::cellAround(cells, 6), std::make_pair(5, 6));
        QCOMPARE(qucs_s::python::cellAround("x = 1\ny = 2\n", 2), std::make_pair(1, 2));
        if (python.isEmpty()) QSKIP("no python3 here");
#ifdef Q_OS_WIN
        QSKIP("the shell's console is a terminal's on Unix");
#endif
        PythonDoc* py = open(write("cells/c.py", "# %% setup\nimport os\nbase = 20\n# %% use\n    \nresult = base * 2\nresult + 1\n"
                                                "# %% fail\nprint('before')\nboom = 1 / 0\n"));
        QVERIFY(py != nullptr);
        QVERIFY(focused(py));
        ProcessConsole* shell = app->pythonConsole();
        place(py, 3, 0);
        QTest::keyClick(py, Qt::Key_Return, Qt::ControlModifier);   // Run Cell
        QVERIFY(app->pythonDockWidget()->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("c.py, cell of lines 1-3"), 20000);
        QCOMPARE(py->textCursor().blockNumber() + 1, 3);   // (not advanced)
        QCOMPARE(py->toPlainText().count('\n'), 10);         // (no line break typed)
        pythonAction("pythonRunCellAdvance")->trigger();     // the same cell, then on
        QCOMPARE(py->textCursor().blockNumber() + 1, 5);
        pythonAction("pythonRunCellAdvance")->trigger();     // use: 41 shown, as the shell shows a value
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("\n41"), 20000);
        QCOMPARE(py->textCursor().blockNumber() + 1, 9);
        pythonAction("pythonRunCell")->trigger();             // fail: at the script's line
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("ZeroDivisionError"), 20000);
        QVERIFY(shell->outputText().contains("before"));
        QVERIFY2(shell->outputText().contains(QStringLiteral("File \"%1\", line 10").arg(QFileInfo(py->getDocName()).absoluteFilePath())),
                 qPrintable(shell->outputText()));
        QVERIFY(!shell->outputText().contains("_qucs_shell.py\", line"));   // (the runner's frames left out)

        // A line, and on to the next; Shift+Return the action's, not a break.
        place(py, 6, 3);
        QTest::keyClick(py, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(py->toPlainText().count('\n'), 10);
        QCOMPARE(py->textCursor().blockNumber() + 1, 7);
        QTest::keyClick(py, Qt::Key_Return, Qt::ShiftModifier);   // result + 1: shown
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().count("\n41") >= 2, 20000);
        // A selection, indented in the script, its lines as they are.
        write("cells/sel.py", "def f():\n    a = 5\n    print('a is', a)\n");
        PythonDoc* sel = open(dir.filePath("cells/sel.py"));
        QVERIFY(sel != nullptr && focused(sel));
        QTextCursor two(sel->document()->findBlockByNumber(1));
        two.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
        two.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        sel->setTextCursor(two);
        pythonAction("pythonRunSelection")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("a is 5"), 20000);
        QVERIFY(shell->outputText().contains("sel.py, lines 2-3"));
        QCOMPARE(sel->textCursor().blockNumber() + 1, 3);   // (a selection: not moved)
        // A syntax error: at the script's line too.
        write("cells/bad.py", "a = 1\nb = 2\nc = (3,\n")
            ;
        PythonDoc* bad = open(dir.filePath("cells/bad.py"));
        QVERIFY(bad != nullptr && focused(bad));
        QTextCursor lines(bad->document()->findBlockByNumber(1));
        lines.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
        lines.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        bad->setTextCursor(lines);
        pythonAction("pythonRunSelection")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("SyntaxError"), 20000);
        QVERIFY2(shell->outputText().contains(QStringLiteral("File \"%1\", line 3").arg(QFileInfo(bad->getDocName()).absoluteFilePath())),
                 qPrintable(shell->outputText()));
        shell->sendLine("print('base is', base, 'cwd', os.path.basename(os.getcwd()))");
        QTRY_VERIFY_WITH_TIMEOUT(shell->outputText().contains("base is 20 cwd cells"), 20000);
        // The cells ruled off: a line above each # %%, across the text.
        app->showDocument(py);
        {
            const QImage shown = py->viewport()->grab().toImage();
            const int y = py->cursorRect(QTextCursor(py->document()->findBlockByNumber(3))).top();   // # %% use
            QCOMPARE(shown.pixelColor(shown.width() / 2, y), QColor(0x8a, 0x94, 0xa6));
            QVERIFY(shown.pixelColor(shown.width() / 2, y + 4) != QColor(0x8a, 0x94, 0xa6));
            const int below = py->cursorRect(QTextCursor(py->document()->findBlockByNumber(5))).top();
            QVERIFY(shown.pixelColor(shown.width() / 2, below) != QColor(0x8a, 0x94, 0xa6));
        }
        // The shell's own line: Shift+Return there is the shell's.
        const qsizetype ran = shell->outputText().count("sel.py, lines 2-3");
        shell->inputLine()->setFocus();
        QTest::keyClick(shell->inputLine(), Qt::Key_Return, Qt::ShiftModifier);
        const qsizetype cellRuns = shell->outputText().count("cell of lines");
        QTest::keyClick(shell->inputLine(), Qt::Key_Return, Qt::ControlModifier);   // (Run Cell's, in a script)
        QTest::qWait(500);
        QCOMPARE(shell->outputText().count("sel.py, lines 2-3"), ran);
        QCOMPARE(shell->outputText().count("cell of lines"), cellRuns);
        QVERIFY(QDir(qucs_s::python::moduleFolder() + "/jobs").entryList(QDir::Files).isEmpty());   // (each taken away)
        shell->stop();
    }

    // ------------------------------------------------------------------
    // Format Document, Fix Problems

    void formatAndFix()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        EnvironmentGuard pythonPath("PYTHONPATH"), path("PATH");
        qputenv("PATH", "/usr/bin:/bin");   // (no ruff or black of this machine's)
        PythonDoc* py = open(write("fmt/f.py", "import os\nx=1\ny=x+2\n"));
        QVERIFY(py != nullptr);
        QSignalSpy formatted(py, &PythonDoc::formatted);
        // Neither installed: said.
        pythonAction("pythonFormat")->trigger();
        QVERIFY(formatted.wait(20000));
        QVERIFY2(formatted.last().at(0).toString().contains("needs ruff or black"), qPrintable(formatted.last().at(0).toString()));
        QVERIFY2(app->statusBar()->currentMessage().contains("needs ruff or black"), qPrintable(app->statusBar()->currentMessage()));
        pythonAction("pythonFix")->trigger();
        QVERIFY(formatted.wait(20000));
        QVERIFY(formatted.last().at(0).toString().contains("needs ruff"));
        QCOMPARE(py->toPlainText(), QString("import os\nx=1\ny=x+2\n"));

        // black alone.
        write("fmt/tools/black/__init__.py", "");
        write("fmt/tools/black/__main__.py",
              "import sys, re\n"
              "if '--version' in sys.argv:\n    print('black, 0.0-fake'); sys.exit(0)\n"
              "sys.stdout.write(re.sub(r'(?<=\\w)([=+])(?=\\w)', r' \\1 ', sys.stdin.read()))\n");
        qputenv("PYTHONPATH", QFile::encodeName(dir.filePath("fmt/tools")) + QDir::listSeparator().toLatin1() + noJedi);
        place(py, 3, 1);
        py->format();
        QVERIFY(formatted.wait(20000));
        QCOMPARE(formatted.last().at(0).toString(), QString("Formatted by black, 0.0-fake (Undo takes it back)."));
        QCOMPARE(py->toPlainText(), QString("import os\nx = 1\ny = x + 2\n"));
        QCOMPARE(py->textCursor().blockNumber() + 1, 3);   // (the cursor kept to its line)
        py->undo();
        QCOMPARE(py->toPlainText(), QString("import os\nx=1\ny=x+2\n"));   // (one edit)
        py->redo();

        // ruff first; its fixes; a script it cannot read; nothing to do.
        write("fmt/tools/ruff/__init__.py", "");
        write("fmt/tools/ruff/__main__.py",
              "import sys, os\n"
              "if '--version' in sys.argv:\n    print('ruff 0.0.0-fake'); sys.exit(0)\n"
              "src = sys.stdin.read()\n"
              "name = sys.argv[sys.argv.index('--stdin-filename') + 1]\n"
              "assert os.path.realpath(os.getcwd()) == os.path.realpath(os.path.dirname(name)), 'its folder'\n"
              "if sys.argv[1] == 'format':\n"
              "    if 'def (' in src:\n        sys.stderr.write('error: Failed to parse at 1:5\\n'); sys.exit(2)\n"
              "    sys.stdout.write(src.replace(' = ', '='))\n"
              "elif sys.argv[1] == 'check' and '--fix' in sys.argv:\n"
              "    sys.stdout.write(''.join(l for l in src.splitlines(True) if 'import os' not in l))\n");
        py->fixProblems();
        QVERIFY(formatted.wait(20000));
        QCOMPARE(formatted.last().at(0).toString(), QString("Fixed by ruff 0.0.0-fake (Undo takes it back)."));
        QCOMPARE(py->toPlainText(), QString("x = 1\ny = x + 2\n"));
        py->fixProblems();
        QVERIFY(formatted.wait(20000));
        QCOMPARE(formatted.last().at(0).toString(), QString("Nothing for ruff 0.0.0-fake to fix."));
        py->format();
        QVERIFY(formatted.wait(20000));
        QCOMPARE(py->toPlainText(), QString("x=1\ny=x + 2\n"));
        py->selectAll();
        py->insertPlainText("def (:\n");
        py->format();
        QVERIFY(formatted.wait(20000));
        QCOMPARE(formatted.last().at(0).toString(), QString("error: Failed to parse at 1:5"));
        QCOMPARE(py->toPlainText(), QString("def (:\n"));
        // Edited while it formats: left as it is.
        write("fmt/tools/ruff/__main__.py",
              "import sys, time\n"
              "if '--version' in sys.argv:\n    print('ruff slow'); sys.exit(0)\n"
              "src = sys.stdin.read(); time.sleep(1.0); sys.stdout.write('formatted\\n')\n");
        py->selectAll();
        py->insertPlainText("a=1\n");
        py->format();
        QTest::qWait(200);
        py->moveCursor(QTextCursor::End);
        py->insertPlainText("b=2\n");
        QVERIFY(formatted.wait(20000));
        QVERIFY(formatted.last().at(0).toString().contains("changed while it was being formatted"));
        QCOMPARE(py->toPlainText(), QString("a=1\nb=2\n"));
    }

    // ------------------------------------------------------------------
    // Breakpoints, the debugger

    // A click in the line numbers' margin sets a breakpoint and takes it
    // away; they move with the text, stay through a reload; Toggle
    // Breakpoint (Ctrl+F9) at the cursor's line.
    void breakpointsInTheMargin()
    {
        PythonDoc* py = open(write("bp/b.py", "a = 1\nb = 2\nc = 3\nd = 4\n"));
        QVERIFY(py != nullptr);
        QWidget* margin = nullptr;
        for (QObject* child : py->children())
            if (auto* w = qobject_cast<QWidget*>(child); w != nullptr && w != py->viewport() && w->metaObject() == &QWidget::staticMetaObject
                                                         && w->x() < py->viewport()->x() && w->height() > 50)
                margin = w;
        QVERIFY(margin != nullptr);
        const auto yOf = [py, margin](int line) {
            QTextCursor c(py->document()->findBlockByNumber(line - 1));
            return margin->mapFromGlobal(py->viewport()->mapToGlobal(py->cursorRect(c).center())).y();
        };
        QSignalSpy changed(py, &PythonDoc::breakpointsChanged);
        QTest::mouseClick(margin, Qt::LeftButton, {}, QPoint(4, yOf(2)));
        QTest::mouseClick(margin, Qt::LeftButton, {}, QPoint(margin->width() - 2, yOf(4)));   // (on its number too)
        QCOMPARE(py->breakpoints(), (QList<int>{2, 4}));
        QCOMPARE(changed.count(), 2);
        {   // Drawn: a red dot at the left of a line with one, none at another.
            const QImage shown = margin->grab().toImage();
            const int x = (py->viewport()->x() > 0 ? 0 : 0) + std::clamp(py->fontMetrics().height(), 12, 18) / 2 + 1;
            const QColor dot = shown.pixelColor(x, yOf(2));
            QVERIFY2(dot.red() > 150 && dot.green() < 100, qPrintable(dot.name()));
            const QColor none = shown.pixelColor(x, yOf(3));
            QVERIFY2(none.red() < 150 || none.green() > 150, qPrintable(none.name()));
        }
        QTest::mouseClick(margin, Qt::LeftButton, {}, QPoint(4, yOf(2)));
        QCOMPARE(py->breakpoints(), QList<int>{4});
        // With the text.
        place(py, 1, 0);
        py->insertPlainText("new = 0\n");
        QCOMPARE(py->breakpoints(), QList<int>{5});
        QVERIFY(focused(py));
        place(py, 2, 0);
        QTest::keyClick(py, Qt::Key_F9, Qt::ControlModifier);
        QCOMPARE(py->breakpoints(), (QList<int>{2, 5}));
        QVERIFY(app->saveFile(py));
        QVERIFY(app->reloadDocument(py));
        QCOMPARE(py->breakpoints(), (QList<int>{2, 5}));
    }

    // Debug (Ctrl+F2): stopped at a breakpoint, the line marked, the stack
    // and the variables shown; Step Into a function of a module beside it
    // (opened there), a frame further out looked at, Step Out, Step Over; a
    // breakpoint taken away and one set while it is stopped; a list
    // opened; an expression evaluated, a statement run; Continue (Ctrl+F2)
    // to the end. An exception no one catches: stopped where it is raised,
    // its traceback said.
    void theDebuggerStopsAndSteps()
    {
        if (python.isEmpty()) QSKIP("no python3 here");
        write("dbg/helper.py", "def twice(v):\n    r = v * 2\n    return r\n");
        // (A module beside the script named as one the debugger uses: not its.)
        write("dbg/queue.py", "raise RuntimeError('the debugger took the script folder\\'s queue')\n");
        PythonDoc* py = open(write("dbg/main.py",
                                   "import helper\n"                        // 1
                                   "total = 0\n"                            // 2
                                   "for k in range(3):\n"                   // 3
                                   "    total += helper.twice(k)\n"         // 4
                                   "data = [1, 2, {'k': 3}]\n"              // 5
                                   "print('total', total)\n"                // 6
                                   "# the end\n"                            // 7
                                   "result = helper.twice(total)\n"         // 8
                                   "print('done', result)\n"));             // 9
        QVERIFY(py != nullptr);
        py->toggleBreakpoint(4);
        PythonRunConsole* run = app->pythonRunConsole();
        QSignalSpy paused(run, &PythonRunConsole::paused);
        QVERIFY(focused(py));
        QTest::keyClick(py, Qt::Key_F2, Qt::ControlModifier);   // Debug
        QVERIFY(run->isDebugging());
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stopReason(), QString("breakpoint"));
        QCOMPARE(run->stack().size(), 1);
        QCOMPARE(run->stack().first().line, 4);
        QCOMPARE(py->executionLine(), 4);
        const auto lineColour = [](PythonDoc* doc, int line) {
            const QImage shown = doc->viewport()->grab().toImage();
            const QRect box = doc->cursorRect(QTextCursor(doc->document()->findBlockByNumber(line - 1)));
            return shown.pixelColor(shown.width() - 4, box.center().y());
        };
        {
            const QColor yellow = lineColour(py, 4);
            QVERIFY2(yellow.red() > yellow.blue() + 20 && yellow.green() > yellow.blue() + 10, qPrintable(yellow.name()));
        }
        QVERIFY(run->debugPanel()->isVisible());
        QVERIFY(run->variableRows().contains("k: int = 0"));
        QVERIFY(run->variableRows().contains("total: int = 0"));
        QVERIFY(!run->variableRows().join(' ').contains("helper:"));   // (a module, at the top level: left out)
        QVERIFY(pythonAction("pythonContinue")->isEnabled() && !pythonAction("pythonDebug")->isEnabled());

        // Into helper.twice: its module opened there, marked; main's frame
        // looked at; out again.
        pythonAction("pythonStepInto")->trigger();
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().size(), 2);
        QCOMPARE(run->stack().first().function, QString("twice"));
        auto* helper = qobject_cast<PythonDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(helper != nullptr && helper->getDocName().endsWith("helper.py"));
        QCOMPARE(helper->executionLine(), 2);
        QCOMPARE(py->executionLine(), 0);
        QCOMPARE(run->variableRows(), QStringList{"v: int = 0"});
        QSignalSpy variables(run, &PythonRunConsole::variablesShown);
        run->stackView()->setCurrentRow(1);
        QVERIFY(variables.wait(10000));
        QCOMPARE(py->executionLine(), 4);
        {
            const QColor green = lineColour(py, 4);
            QVERIFY2(green.green() > green.red() + 20 && green.green() > green.blue() + 10, qPrintable(green.name()));
        }
        QVERIFY(run->variableRows().contains("k: int = 0"));
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(py));
        {   // Evaluated in the frame looked at (k is main's, not twice's).
            QSignalSpy outer(run, &PythonRunConsole::evaluated);
            run->evaluate("k + 40");
            QVERIFY(outer.wait(10000));
            QCOMPARE(run->lastEvaluated(), QString("40"));
        }
        pythonAction("pythonStepOut")->trigger();
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().size(), 1);
        QCOMPARE(run->stack().first().line, 3);
        QCOMPARE(helper->executionLine(), 0);
        pythonAction("pythonStepOver")->trigger();
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().first().line, 4);   // (the breakpoint's line, stepped to)
        QVERIFY(run->variableRows().contains("k: int = 1"));
        QVERIFY(run->variableRows().contains("total: int = 0"));

        // The breakpoint taken away, one set on a comment (the next line
        // with code stops); on.
        py->toggleBreakpoint(4);
        py->toggleBreakpoint(7);
        QTest::keyClick(py, Qt::Key_F2, Qt::ControlModifier);   // Continue
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().first().line, 8);
        QVERIFY(run->outputText().contains("total 6"));
        QTreeWidget* tree = run->variablesView();
        QTreeWidgetItem* data = nullptr;
        for (int k = 0; k < tree->topLevelItemCount(); ++k)
            if (tree->topLevelItem(k)->text(0) == "data") data = tree->topLevelItem(k);
        QVERIFY(data != nullptr);
        QCOMPARE(data->text(1), QString("list (3)"));
        tree->expandItem(data);
        QVERIFY(variables.wait(10000));
        QCOMPARE(data->childCount(), 3);
        QCOMPARE(data->child(2)->text(0), QString("[2]"));
        QCOMPARE(data->child(2)->text(1), QString("dict (1)"));
        tree->expandItem(data->child(2));
        QVERIFY(variables.wait(10000));
        QCOMPARE(data->child(2)->child(0)->text(0), QString("'k'"));
        QCOMPARE(data->child(2)->child(0)->text(2), QString("3"));

        QSignalSpy evaluated(run, &PythonRunConsole::evaluated);
        run->evaluateLine()->setText("total * 10");
        emit run->evaluateLine()->returnPressed();
        QVERIFY(evaluated.wait(10000));
        QCOMPARE(run->lastEvaluated(), QString("60"));
        run->evaluate("total = 100");   // a statement, run in the frame
        QVERIFY(evaluated.wait(10000));
        QCOMPARE(run->lastEvaluated(), QString("(done)"));
        QTRY_VERIFY_WITH_TIMEOUT(run->variableRows().contains("total: int = 100"), 10000);
        run->evaluate("nothing_here");
        QVERIFY(evaluated.wait(10000));
        QVERIFY2(run->lastEvaluated().startsWith("NameError"), qPrintable(run->lastEvaluated()));

        QSignalSpy finished(run, &PythonRunConsole::finished);
        pythonAction("pythonContinue")->trigger();
        QVERIFY(finished.wait(20000));
        QCOMPARE(run->exitCode(), 0);
        QVERIFY(run->outputText().contains("done 200"));
        QVERIFY(!run->isDebugging() && !run->debugPanel()->isVisible());
        QCOMPARE(py->executionLine(), 0);
        QVERIFY(pythonAction("pythonDebug")->isEnabled() && !pythonAction("pythonContinue")->isEnabled());

        // An exception: stopped where raised, after its traceback.
        PythonDoc* bad = open(write("dbg/bad.py", "def f(n):\n    rest = 10 - n\n    return 1 / rest\n\nprint(f(10))\n"));
        QVERIFY(bad != nullptr);
        QVERIFY(app->debugPython(bad));
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stopReason(), QString("exception"));
        QCOMPARE(run->stopException(), QString("ZeroDivisionError: division by zero"));
        QCOMPARE(run->stack().size(), 2);
        QCOMPARE(run->stack().first().line, 3);
        QCOMPARE(bad->executionLine(), 3);
        QVERIFY(run->variableRows().contains("rest: int = 0"));
        QVERIFY(run->outputText().contains("ZeroDivisionError: division by zero"));
        QVERIFY(!run->outputText().contains("bdb.py"));   // (the debugger's frames left out)
        run->continueRun();
        QVERIFY(finished.wait(20000));
        QCOMPARE(run->exitCode(), 1);
        QCOMPARE(bad->executionLine(), 0);

        // Step Into a call of Python's library: stepped over, on in the script.
        PythonDoc* lib = open(write("dbg/lib.py", "import json\nx = json.dumps([1, 2])\ny = len(x)\n"));
        QVERIFY(lib != nullptr);
        lib->toggleBreakpoint(2);
        QVERIFY(app->debugPython(lib));
        QVERIFY(paused.wait(20000));
        run->stepInto();
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().size(), 1);
        QCOMPARE(run->stack().first().line, 3);
        QVERIFY(run->stack().first().file.endsWith("lib.py"));
        run->continueRun();
        QVERIFY(finished.wait(20000));

        // A breakpoint set while it runs: it stops there - none anywhere when
        // it started.
        py->setBreakpoints({});
        lib->setBreakpoints({});
        PythonDoc* loop = open(write("dbg/loop.py", "import time\nfor i in range(400):\n    time.sleep(0.02)\n    x = i\nprint('end')\n"));
        QVERIFY(loop != nullptr);
        QVERIFY(app->debugPython(loop));
        QTest::qWait(400);
        QVERIFY(run->isDebugging() && !run->isPaused());
        loop->toggleBreakpoint(4);
        QVERIFY(paused.wait(20000));
        QCOMPARE(run->stack().first().line, 4);
        QVERIFY(run->variableRows().first().startsWith("i: int = "));
        loop->toggleBreakpoint(4);
        run->continueRun();
        QVERIFY(finished.wait(30000));
        QVERIFY(run->outputText().contains("end"));

        // Stop while stopped; a run (F2) after it, as before.
        bad->toggleBreakpoint(2);
        QVERIFY(app->debugPython(bad));
        QVERIFY(paused.wait(20000));
        pythonAction("pythonStop")->trigger();
        QVERIFY(finished.wait(20000));
        QVERIFY(run->wasStopped());
        QCOMPARE(bad->executionLine(), 0);
        QVERIFY(app->runPython(py));
        QVERIFY(finished.wait(20000));
        QVERIFY(run->outputText().contains("done 12"));
        QVERIFY(!run->isDebugging());
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication application(one, argv);
    TestPythonTools test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_python_tools.moc"
