/*
 * test_multiphysics_optimize.cpp - a multiphysics model's optimization in the
 *                                  window and through Claude's tools: the node
 *                                  in the tree and its settings, a run in the
 *                                  background with its progress and Cancel,
 *                                  the page of what it found, a step of Undo,
 *                                  saved and read again, the optimum written
 *                                  in; fem_edit, fem_optimize, fem_stop,
 *                                  fem_describe. Bounded by evaluations, not
 *                                  by any time.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "fem_optimize.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "multiphysicsdoc.h"
#include "multiphysicsoptimization.h"
#include "multiphysicspanel.h"
#include "qucs.h"
#include "qucscontrol.h"

using namespace qucs_s::fem;

namespace {

constexpr double Eps0 = 8.8541878188e-12;

bool slowBuild()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
#ifndef NDEBUG
    return true;
#else
    return false;
#endif
}

int jobWait()
{
    return slowBuild() ? 600000 : 120000;
}

/// The window as an image, saved where QUCS_MP_SHOTS says (to look at).
void shoot(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("QUCS_MP_SHOTS");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    w->grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
}

/// C = eps0 er w / d per metre, whatever the mesh: its Optimization \a section.
QByteArray capacitor(const QByteArray& section)
{
    return QByteArray(R"({"format": "qucs-s multiphysics 1", "parameters": [
        {"name": "w", "expression": "4[mm]"}, {"name": "d", "expression": "1[mm]"}, {"name": "er", "expression": "4"}],
      "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "gap", "size": ["w", "d"]}],
        "materials": [{"label": "dielectric", "epsilonr": "er", "selection": {"all": true}}],
        "physics": [{"type": "electrostatics", "tag": "es", "order": "1", "features": [
          {"type": "terminal", "name": "1", "V0": "5[V]", "selection": {"of": ["gap"], "side": "top"}},
          {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "max(d, w)/4"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}],
      "optimizations": [)") + section + R"(]})";
}

const QByteArray kTune = R"({"type": "optimization", "tag": "opt1", "label": "The gap", "method": "tune", "maxiter": 30,
    "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "200[pF]"], ["es.W", "MON"]]})";

// Never ends on its own - annealing's schedule is its only stop -: for a stop.
const QByteArray kLong = R"({"type": "optimization", "tag": "opt1", "method": "sa", "maxiter": 1000000, "seed": 2,
    "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "200[pF]"], ["es.W", "MON"]]})";

} // namespace

class TestMultiphysicsOptimize : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;
    int files = 0;

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    MultiphysicsPanel* panel() const { return app->multiphysicsPanel(); }
    MultiphysicsDoc* open(const QString& file)
    {
        if (!app->gotoPage(file)) return nullptr;
        return qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
    }
    /// A model of its own in the workspace, open.
    MultiphysicsDoc* model(const QByteArray& section)
    {
        const QString file = path(QStringLiteral("m%1.qfem").arg(++files));
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly)) return nullptr;
        f.write(capacitor(section));
        f.close();
        return open(file);
    }
    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 120000)
    {
        return control->callNow(tool, args, slowBuild() ? std::max(timeoutMs, 600000) : timeoutMs);
    }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r) { return QJsonDocument::fromJson(text(r).toUtf8()).object(); }
    static double valueNamed(const QJsonArray& list, const QString& name)
    {
        for (const QJsonValue& v : list)
            if (v.toObject().value(QStringLiteral("name")).toString() == name) return v.toObject().value(QStringLiteral("SI")).toDouble(NAN);
        return NAN;
    }
    static QString expressionOf(const MultiphysicsDoc* doc, const QString& name)
    {
        for (const Model::Parameter& p : doc->model().parameters())
            if (p.name == name) return p.expression;
        return QString();
    }
    template <typename T>
    T* inForm(const char* name) const
    {
        return panel()->settingsArea()->findChild<T*>(QString::fromLatin1(name));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsSettings.ExamplesDir = QStringLiteral(QUCS_EXAMPLES_DIR "/");
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.font = QApplication::font();
        QucsSettings.appFont = QApplication::font();
        QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1400, 900);
        app->show();
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
    }

    void cleanupTestCase()
    {
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
    }

    void cleanup()
    {
        for (QucsDoc* d : app->allDocuments()) {
            if (auto* m = dynamic_cast<MultiphysicsDoc*>(d)) {
                m->cancelJob();
                m->waitForJob(jobWait());
            }
            d->setDocChanged(false);
        }
        app->closeAllFiles();
    }

    // A model without one: nothing of it to see, and it says so if asked.
    void noOptimizationNoTab()
    {
        const QString file = path(QStringLiteral("old.qfem"));
        QFile::remove(file);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/microstrip.qfem"), file));
        QFile(file).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc);
        QVERIFY(doc->readNotes().isEmpty());
        QCOMPARE(doc->bottomTabs()->count(), 2);
        QVERIFY(doc->optimizeButton()->isHidden());
        QVERIFY(!doc->optimizationResult());
        doc->optimize();
        QVERIFY(!doc->isBusy());
        QVERIFY2(doc->lastError().contains(QStringLiteral("no optimization")), qPrintable(doc->lastError()));
        QVERIFY(!doc->model().toObject().contains(QStringLiteral("optimizations")));
        // Saved: nothing new in the file.
        QCOMPARE(doc->save(), 0);
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("\"optimizations\""));
    }

    // Added from the tree, as any node: its settings, its tables with their
    // choices, its buttons; a step of Undo.
    void addedFromTheTree()
    {
        const QByteArray section;
        MultiphysicsDoc* doc = model(section);
        QVERIFY(doc);
        panel()->setDocument(doc);
        QMenu* menu = panel()->menuFor(QStringLiteral("model"));
        QVERIFY(menu);
        QAction* add = nullptr;
        for (QAction* a : menu->actions())
            if (a->text() == QLatin1String("Add Optimization")) add = a;
        QVERIFY(add);
        delete menu;
        const int steps = doc->undoStack()->count();
        const QString tag = panel()->addNode(QStringLiteral("model"), QStringLiteral("optimization"));
        QCOMPARE(tag, QStringLiteral("opt1"));
        QCOMPARE(doc->undoStack()->count(), steps + 1);
        QVERIFY(!doc->optimizeButton()->isHidden());
        QCOMPARE(doc->bottomTabs()->count(), 3);
        QCOMPARE(doc->bottomTabs()->tabText(2), QStringLiteral("Optimization"));
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("method")) != nullptr);
        for (const char* key : {"study", "mode", "method", "maxiter", "tol", "seed", "polish", "starts", "constraints", "ctol", "parallel", "refresh", "variables", "objectives"})
            QVERIFY2(panel()->editorFor(QString::fromLatin1(key)) != nullptr, key);
        QVERIFY(!panel()->editorFor(QStringLiteral("result")));       // (the run's)
        QVERIFY(!panel()->editorFor(QStringLiteral("variation")));    // (design centering's)
        QVERIFY(!panel()->editorFor(QStringLiteral("strategy")));     // (differential evolution's)
        auto* method = qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("method")));
        QVERIFY(method);
        for (const char* m : {"auto", "de", "pso", "sa", "cmaes", "bayes", "nm", "tr", "lm", "nsga2", "tune"})
            QVERIFY2(method->findData(QString::fromLatin1(m)) >= 0, m);
        // Chosen: its own settings shown.
        method->setCurrentIndex(method->findData(QStringLiteral("de")));
        emit method->activated(method->currentIndex());
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("strategy")) != nullptr);
        QCOMPARE(doc->model().find(tag)->text(QStringLiteral("method")), QStringLiteral("de"));
        // A row of its parameters: the first of the model's, over a factor of
        // two each way, with its unit.
        auto addRow = [this](const char* key) {
            QWidget* editor = panel()->editorFor(QString::fromLatin1(key));
            auto* button = editor ? editor->findChild<QPushButton*>(QStringLiteral("mpRowsAdd")) : nullptr;
            if (button) button->click();
            return button != nullptr;
        };
        QVERIFY(addRow("variables"));
        QTRY_COMPARE(doc->model().find(tag)->value(QStringLiteral("variables")).toArray().size(), 1);
        QJsonArray row = doc->model().find(tag)->value(QStringLiteral("variables")).toArray().at(0).toArray();
        QCOMPARE(row.at(0).toString(), QStringLiteral("w"));
        QCOMPARE(row.at(2).toString(), QStringLiteral("2[mm]"));
        QCOMPARE(row.at(3).toString(), QStringLiteral("8[mm]"));
        QCOMPARE(row.at(4).toString(), QStringLiteral("LIN_DOUBLE"));
        // Its cells of choices are boxes: changed there, the node changes.
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("variables")) != nullptr
                    && panel()->editorFor(QStringLiteral("variables"))->findChild<QTableWidget*>() != nullptr
                    && panel()->editorFor(QStringLiteral("variables"))->findChild<QTableWidget*>()->rowCount() == 1);
        auto* table = panel()->editorFor(QStringLiteral("variables"))->findChild<QTableWidget*>();
        auto* type = qobject_cast<QComboBox*>(table->cellWidget(0, 4));
        QVERIFY(type);
        QCOMPARE(type->currentText(), QStringLiteral("LIN_DOUBLE"));
        type->setCurrentIndex(type->findText(QStringLiteral("LOG_DOUBLE")));
        emit type->activated(type->currentIndex());
        QTRY_COMPARE(doc->model().find(tag)->value(QStringLiteral("variables")).toArray().at(0).toArray().at(4).toString(), QStringLiteral("LOG_DOUBLE"));
        // An objective: a row, its kind a box.
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("objectives")) != nullptr);
        QVERIFY(addRow("objectives"));
        QTRY_COMPARE(doc->model().find(tag)->value(QStringLiteral("objectives")).toArray().size(), 1);
        QCOMPARE(doc->model().find(tag)->value(QStringLiteral("objectives")).toArray().at(0).toArray().at(1).toString(), QStringLiteral("MIN"));
        // Its menu and its buttons.
        QTRY_VERIFY(inForm<QPushButton>("mpOptimizeButton") != nullptr);
        QVERIFY(inForm<QPushButton>("mpApplyOptimumButton") != nullptr);
        menu = panel()->menuFor(tag);
        QStringList texts;
        for (QAction* a : menu->actions()) texts << a->text();
        QVERIFY2(texts.contains(QStringLiteral("Run")), qPrintable(texts.join(QLatin1Char(','))));
        delete menu;
        // Undone to before it: no node, no page.
        while (doc->undoStack()->index() > steps) doc->undo();
        QVERIFY(!doc->model().find(tag));
        QCOMPARE(doc->bottomTabs()->count(), 2);
        QVERIFY(doc->optimizeButton()->isHidden());
        // The Multiphysics menu has it.
        QAction* action = nullptr;
        for (QAction* a : app->findChildren<QAction*>(QStringLiteral("mpOptimizeAction"))) action = a;
        QVERIFY(action);
    }

    // In the background: the window goes on, its bar and its stage say how far,
    // its page shows each solve as it comes; what it found, a step of Undo.
    void runsInTheBackground()
    {
        MultiphysicsDoc* doc = model(R"({"type": "optimization", "tag": "opt1", "label": "The gap", "method": "de", "maxiter": 8, "population": 8, "seed": 3,
            "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "200[pF]"], ["es.W", "MON"]]})");
        QVERIFY(doc);
        panel()->setDocument(doc);
        QVERIFY(!doc->optimizationResult(QStringLiteral("opt1")));
        // (Nothing run yet, said.)
        QVERIFY2(doc->optimizationPage()->summaryLabel()->text().contains(QStringLiteral("not run")), qPrintable(doc->optimizationPage()->summaryLabel()->text()));
        // (However quick the machine: the window's own events run while it is
        // busy - not how many of them, which is a time.)
        int ticks = 0;
        QTimer window;
        connect(&window, &QTimer::timeout, this, [&] { ticks += doc->isBusy() ? 1 : 0; });
        window.start(0);
        const int steps = doc->undoStack()->count();
        doc->optimizeButton()->click();
        QVERIFY(doc->isBusy());
        QCOMPARE(doc->optimizing(), QStringLiteral("opt1"));
        QVERIFY(!doc->optimizeButton()->isEnabled());
        QCOMPARE(doc->bottomTabs()->currentWidget(), static_cast<QWidget*>(doc->optimizationPage()));
        QVERIFY2(doc->optimizationPage()->summaryLabel()->text().contains(QStringLiteral("running")), qPrintable(doc->optimizationPage()->summaryLabel()->text()));
        QVERIFY(doc->waitForJob(jobWait()));
        window.stop();
        QVERIFY2(ticks > 0, qPrintable(QString::number(ticks)));   // (the window was not frozen)
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QVERIFY(doc->optimizing().isEmpty());
        QVERIFY(doc->optimizeButton()->isEnabled());
        const std::optional<OptimizationResult> r = doc->optimizationResult(QStringLiteral("opt1"));
        QVERIFY(r);
        QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        QVERIFY(r->hasBest());
        const double exact = Eps0 * 4 * 4e-3 / 200e-12;
        QVERIFY2(std::abs(r->parameters.first().value - exact) < 0.1 * exact, qPrintable(r->summary));
        // Its page: why it stopped, the best values, the history, a step of Undo.
        OptimizationPage* page = doc->optimizationPage();
        QVERIFY2(page->summaryLabel()->text().contains(QStringLiteral("The gap")), qPrintable(page->summaryLabel()->text()));
        QVERIFY2(page->summaryLabel()->text().contains(r->summary.toHtmlEscaped()), qPrintable(page->summaryLabel()->text()));
        QCOMPARE(page->bestTable()->rowCount(), 3);
        QCOMPARE(page->bestTable()->item(0, 0)->text(), QStringLiteral("d"));
        QCOMPARE(page->bestTable()->item(0, 1)->text(), r->parameters.first().text);
        QCOMPARE(page->bestTable()->item(1, 0)->text(), QStringLiteral("es.C11"));
        QCOMPARE(page->historyTable()->rowCount(), r->solves);
        QVERIFY(page->historyTable()->columnCount() >= 5);
        QVERIFY(page->historyTable()->horizontalHeaderItem(1)->text().startsWith(QStringLiteral("d")));
        QCOMPARE(int(page->plot()->best().size()), r->solves);
        QVERIFY(page->plot()->best().back() <= page->plot()->best().front());
        QVERIFY(page->applyButton()->isEnabled());
        QVERIFY(!page->notesEdit()->toPlainText().isNull());
        // The tree says so.
        panel()->selectNode(QStringLiteral("opt1"));
        QList<QTreeWidgetItem*> items = panel()->tree()->findItems(QStringLiteral("The gap"), Qt::MatchExactly | Qt::MatchRecursive, 0);
        QCOMPARE(items.size(), 1);
        QTRY_COMPARE(items.first()->text(1), r->status);
        // One step of Undo, the document changed; and back.
        QCOMPARE(doc->undoStack()->count(), steps + 1);
        QVERIFY(doc->getDocChanged());
        doc->undo();
        QVERIFY(!doc->optimizationResult(QStringLiteral("opt1")));
        QVERIFY2(page->summaryLabel()->text().contains(QStringLiteral("not run")), qPrintable(page->summaryLabel()->text()));
        doc->redo();
        QVERIFY(doc->optimizationResult(QStringLiteral("opt1")));
        QCOMPARE(page->historyTable()->rowCount(), r->solves);
        // Messages say it too; the model's solutions are not touched.
        QVERIFY(doc->messages().join(QLatin1Char(' ')).contains(r->summary));
        QVERIFY(!doc->solution());
        doc->bottomTabs()->setCurrentWidget(page);
        panel()->selectNode(QStringLiteral("opt1"));
        QTest::qWait(50);
        shoot(app, QStringLiteral("optimization_de"));
    }

    // Cancel: at its next check, the best point so far kept and usable.
    void cancelKeepsTheBest()
    {
        MultiphysicsDoc* doc = model(kLong);
        QVERIFY(doc);
        doc->optimize();
        QVERIFY(doc->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(doc->optimizationPage()->historyTable()->rowCount() >= 5, jobWait());
        QTRY_VERIFY_WITH_TIMEOUT(!doc->progressRow()->isHidden(), jobWait());
        QVERIFY(!doc->stageLabel()->text().isEmpty());
        doc->cancelButton()->click();
        QVERIFY(doc->waitForJob(jobWait()));
        const std::optional<OptimizationResult> r = doc->optimizationResult();
        QVERIFY(r);
        QCOMPARE(r->status, QStringLiteral("interrupted"));
        QVERIFY(r->hasBest());
        QVERIFY(r->solves >= 5);
        QVERIFY(r->canApply());
        QVERIFY(doc->optimizationPage()->applyButton()->isEnabled());
        QVERIFY2(r->summary.contains(QStringLiteral("INTERRUPTED")), qPrintable(r->summary));
        QVERIFY(doc->stops() > 0);
        QVERIFY(!doc->isBusy());
        QVERIFY(doc->progressRow()->isHidden());
    }

    // A setup that cannot be run: said at once, no job.
    void aBadSetupIsSaidAtOnce()
    {
        MultiphysicsDoc* doc = model(R"({"type": "optimization", "tag": "opt1",
            "variables": [["d", "", "3[mm]", "1[mm]"]], "objectives": [["es.C11", "MIN"]]})");
        QVERIFY(doc);
        const int steps = doc->undoStack()->count();
        doc->optimize();
        QVERIFY(!doc->isBusy());
        QVERIFY2(doc->lastError().contains(QStringLiteral("above the minimum")), qPrintable(doc->lastError()));
        QVERIFY2(doc->optimizationPage()->summaryLabel()->text().contains(QStringLiteral("above the minimum")), qPrintable(doc->optimizationPage()->summaryLabel()->text()));
        QCOMPARE(doc->undoStack()->count(), steps);
        QVERIFY(!doc->optimizationPage()->applyButton()->isEnabled());
        QVERIFY(!doc->optimizationResult());
        // No such one.
        doc->optimize(QStringLiteral("nosuch"));
        QVERIFY2(doc->lastError().contains(QStringLiteral("nosuch")), qPrintable(doc->lastError()));
    }

    // What it asks that the results have not: the run ends, nothing kept.
    void whatTheResultsLackIsSaid()
    {
        MultiphysicsDoc* doc = model(R"({"type": "optimization", "tag": "opt1", "method": "nm",
            "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.nosuch", "MIN"]]})");
        QVERIFY(doc);
        doc->optimize();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().contains(QStringLiteral("es.nosuch")), qPrintable(doc->lastError()));
        QVERIFY(!doc->optimizationResult());
    }

    // Saved, closed, opened: its setup and what it found, as they were.
    void savedAndOpenedAgain()
    {
        MultiphysicsDoc* doc = model(kTune);
        QVERIFY(doc);
        const QString file = doc->getDocName();
        doc->optimize();
        QVERIFY(doc->waitForJob(jobWait()));
        const std::optional<OptimizationResult> found = doc->optimizationResult();
        QVERIFY(found && found->error.isEmpty());
        QVERIFY(doc->getDocChanged());
        QCOMPARE(doc->save(), 0);
        QVERIFY(!doc->getDocChanged());
        const QJsonObject before = doc->model().toObject();
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
        // On disk: each row of its history one line.
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        f.close();
        QVERIFY(bytes.contains("\"optimizations\""));
        QVERIFY(bytes.contains("\"history\""));
        doc = open(file);
        QVERIFY(doc);
        QVERIFY2(doc->readNotes().isEmpty(), qPrintable(doc->readNotes()));
        QCOMPARE(doc->model().toObject(), before);
        QVERIFY(!doc->getDocChanged());
        const std::optional<OptimizationResult> back = doc->optimizationResult();
        QVERIFY(back);
        QCOMPARE(back->status, found->status);
        QCOMPARE(back->summary, found->summary);
        QCOMPARE(back->parameters.first().value, found->parameters.first().value);
        QCOMPARE(int(back->history.size()), int(found->history.size()));
        QVERIFY(!doc->optimizationStale());
        // Shown; nothing needs solving to see it.
        QVERIFY2(doc->optimizationPage()->summaryLabel()->text().contains(found->summary.toHtmlEscaped()), qPrintable(doc->optimizationPage()->summaryLabel()->text()));
        QCOMPARE(doc->optimizationPage()->historyTable()->rowCount(), int(found->history.size()));
        QVERIFY(doc->optimizationPage()->applyButton()->isEnabled());
        QCOMPARE(doc->bottomTabs()->count(), 3);
        // Another parameter changed: old, said.
        QList<Model::Parameter> ps = doc->model().parameters();
        for (Model::Parameter& p : ps)
            if (p.name == QLatin1String("er")) p.expression = QStringLiteral("5");
        Model changed = doc->model();
        changed.setParameters(ps);
        doc->setModel(changed, QStringLiteral("er"));
        QVERIFY(doc->optimizationStale());
        QVERIFY2(doc->optimizationPage()->summaryLabel()->text().contains(QStringLiteral("changed since")), qPrintable(doc->optimizationPage()->summaryLabel()->text()));
    }

    // Apply Optimum: into the parameters, a step of Undo, and solved there.
    void theOptimumIsAppliedAndSolved()
    {
        MultiphysicsDoc* doc = model(kTune);
        QVERIFY(doc);
        QString why;
        QVERIFY(!doc->applyOptimum(QStringLiteral("opt1"), 0, nullptr, &why));
        QVERIFY2(why.contains(QStringLiteral("not been run")), qPrintable(why));
        doc->optimize();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        const std::optional<OptimizationResult> r = doc->optimizationResult();
        QVERIFY(r);
        const int steps = doc->undoStack()->count();
        QCOMPARE(expressionOf(doc, QStringLiteral("d")), QStringLiteral("1[mm]"));
        doc->optimizationPage()->applyButton()->click();
        QVERIFY(expressionOf(doc, QStringLiteral("d")).endsWith(QStringLiteral("[mm]")));
        QVERIFY(expressionOf(doc, QStringLiteral("d")) != QLatin1String("1[mm]"));
        QCOMPARE(doc->undoStack()->count(), steps + 1);
        // It computes its study: the value the run said it would be.
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QVERIFY(doc->solution());
        QVERIFY2(std::abs(*doc->solution()->global(QStringLiteral("es.C11")) - 200e-12) < 1e-3 * 200e-12, qPrintable(doc->messages().join(QLatin1Char('\n'))));
        // Its result stays of the same problem: not old.
        QVERIFY(!doc->optimizationStale());
        // Undone: the parameter back, the run's result kept.
        doc->undoStack()->setIndex(steps);
        QCOMPARE(expressionOf(doc, QStringLiteral("d")), QStringLiteral("1[mm]"));
        QVERIFY(doc->optimizationResult());
    }

    // A Pareto front: a table to choose from; the design chosen is written in.
    void aParetoFrontIsChosenFrom()
    {
        MultiphysicsDoc* doc = model(R"({"type": "optimization", "tag": "opt1", "method": "nsga2", "maxiter": 5, "population": 10, "seed": 3,
            "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "MAX"], ["d", "MAX"]]})");
        QVERIFY(doc);
        doc->optimize();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        const std::optional<OptimizationResult> r = doc->optimizationResult();
        QVERIFY(r);
        QVERIFY(r->front.size() >= 2);
        OptimizationPage* page = doc->optimizationPage();
        QTableWidget* front = page->frontTable();
        QVERIFY(!front->isHidden());
        QCOMPARE(front->rowCount(), int(r->front.size()));
        QCOMPARE(int(page->plot()->front().size()), int(r->front.size()));
        // Nothing chosen: nothing to write in.
        QVERIFY(!page->applyButton()->isEnabled());
        QString why;
        QVERIFY(!doc->applyOptimum(QStringLiteral("opt1"), 0, nullptr, &why));
        QVERIFY2(why.contains(QStringLiteral("Pareto")), qPrintable(why));
        front->selectRow(1);
        QCOMPARE(page->chosenDesign(), 2);
        QVERIFY(page->applyButton()->isEnabled());
        shoot(app, QStringLiteral("optimization_front"));
        page->applyButton()->click();
        QVERIFY(doc->waitForJob(jobWait()));
        const std::shared_ptr<ParameterScope> ps = doc->parameters();
        QVERIFY(std::abs(ps->values.value(QStringLiteral("d")) - r->front[1].values[0]) < 1e-9 * r->front[1].values[0]);
    }

    // The model changed while it runs: what it found is of the model as it was, said.
    void editedWhileItRuns()
    {
        MultiphysicsDoc* doc = model(R"({"type": "optimization", "tag": "opt1", "method": "de", "maxiter": 30, "population": 10, "seed": 3,
            "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "200[pF]"]]})");
        QVERIFY(doc);
        doc->optimize();
        QVERIFY(doc->isBusy());
        QList<Model::Parameter> ps = doc->model().parameters();
        for (Model::Parameter& p : ps)
            if (p.name == QLatin1String("er")) p.expression = QStringLiteral("9");
        Model changed = doc->model();
        changed.setParameters(ps);
        doc->setModel(changed, QStringLiteral("er"));
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->optimizationResult());
        QVERIFY(doc->optimizationStale());
        QCOMPARE(expressionOf(doc, QStringLiteral("er")), QStringLiteral("9"));
        // The optimization deleted while it runs: nothing to keep it in.
        MultiphysicsDoc* other = model(kLong);
        QVERIFY(other);
        other->optimize();
        QVERIFY(other->isBusy());
        Model without = other->model();
        QVERIFY(without.remove(QStringLiteral("opt1")));
        other->setModel(without, QStringLiteral("no"));
        QCOMPARE(other->bottomTabs()->count(), 2);
        other->cancelJob();
        QVERIFY(other->waitForJob(jobWait()));
        QVERIFY(!other->optimizationResult());
    }

    // Closed while it runs: stopped; nothing left waiting.
    void closedWhileItRuns()
    {
        MultiphysicsDoc* doc = model(kLong);
        QVERIFY(doc);
        doc->optimize();
        QVERIFY(doc->isBusy());
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
        QTRY_VERIFY_WITH_TIMEOUT(app->allDocuments().isEmpty() || dynamic_cast<MultiphysicsDoc*>(app->allDocuments().first()) == nullptr, jobWait());
    }

    // Claude's tools: what a schematic's optimum has through simulate and an
    // optimize block, here through fem_edit and fem_optimize.
    void theToolsRunIt()
    {
        MultiphysicsDoc* doc = model({});
        QVERIFY(doc);
        const QString file = doc->getDocName();
        // None yet: what to do, said.
        QJsonObject r = call("fem_optimize", {{"path", file}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QStringLiteral("fem_edit")), qPrintable(text(r)));
        // Made with fem_edit, as any node.
        QJsonArray variables, objectives, row, target, shown;
        row.append(QStringLiteral("d"));
        row.append(QString());
        row.append(QStringLiteral("0.2[mm]"));
        row.append(QStringLiteral("3[mm]"));
        variables.append(row);
        target.append(QStringLiteral("es.C11"));
        target.append(QStringLiteral("TARGET"));
        target.append(QStringLiteral("200[pF]"));
        objectives.append(target);
        shown.append(QStringLiteral("es.W"));
        shown.append(QStringLiteral("MON"));
        objectives.append(shown);
        QJsonObject props{{QStringLiteral("method"), QStringLiteral("cmaes")},
                          {QStringLiteral("maxiter"), 15},
                          {QStringLiteral("population"), 6},
                          {QStringLiteral("seed"), 2},
                          {QStringLiteral("polish"), true},
                          {QStringLiteral("variables"), variables},
                          {QStringLiteral("objectives"), objectives}};
        r = call("fem_edit", {{"action", "add"}, {"path", file}, {"parent", "model"}, {"type", "optimization"}, {"props", props}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // A property it does not take: refused, as any node's.
        r = call("fem_edit", {{"action", "add"}, {"path", file}, {"parent", "model"}, {"type", "optimization"}, {"props", QJsonObject{{"nosuch", 1}}}});
        QVERIFY2(failed(r) || text(r).contains(QStringLiteral("nosuch")), qPrintable(text(r)));
        if (doc->model().find(QStringLiteral("opt2"))) QVERIFY(!failed(call("fem_edit", {{"action", "delete"}, {"path", file}, {"tag", "opt2"}})));
        // A setup that cannot be run: said, nothing started.
        QJsonArray backwards;
        backwards.append(QJsonArray{QStringLiteral("d"), QString(), QStringLiteral("3[mm]"), QStringLiteral("0.2[mm]")});
        r = call("fem_edit", {{"action", "set"}, {"path", file}, {"tag", "opt1"}, {"props", QJsonObject{{"variables", backwards}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_optimize", {{"path", file}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QStringLiteral("above the minimum")), qPrintable(text(r)));
        QVERIFY(!doc->isBusy());
        r = call("fem_edit", {{"action", "set"}, {"path", file}, {"tag", "opt1"}, {"props", QJsonObject{{"variables", variables}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // Not run yet: its result says so; apply too.
        r = call("fem_optimize", {{"path", file}, {"action", "result"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QStringLiteral("not been run")), qPrintable(text(r)));
        r = call("fem_optimize", {{"path", file}, {"action", "apply"}});
        QVERIFY(failed(r));
        // Its argument's kinds.
        for (const QJsonObject& bad : {QJsonObject{{"action", "nosuch"}}, QJsonObject{{"point", 0}}, QJsonObject{{"history", -5}}, QJsonObject{{"timeout", 0}}, QJsonObject{{"optimization", "nosuch"}}}) {
            QJsonObject args = bad;
            args.insert(QStringLiteral("path"), file);
            r = call("fem_optimize", args);
            QVERIFY2(failed(r), qPrintable(QJsonDocument(bad).toJson()));
        }
        // Run, waited for.
        r = call("fem_optimize", {{"path", file}, {"timeout", int(jobWait() / 1000)}}, jobWait() + 5000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("optimization").toString(), QStringLiteral("opt1"));
        QCOMPARE(o.value("method").toString(), QStringLiteral("cmaes"));
        QVERIFY2(o.value("status").toString() == QLatin1String("converged"), qPrintable(text(r)));
        const double exact = Eps0 * 4 * 4e-3 / 200e-12;
        QVERIFY2(std::abs(valueNamed(o.value("best").toArray(), QStringLiteral("d")) - exact) < 1e-3 * exact, qPrintable(text(r)));
        QVERIFY2(std::abs(valueNamed(o.value("objectives there").toArray(), QStringLiteral("es.C11")) - 200e-12) < 1e-3 * 200e-12, qPrintable(text(r)));
        QVERIFY(o.value("solves").toInt() > 5);
        QVERIFY(o.value("search").toArray().size() >= 1);
        QJsonObject history = o.value("history").toObject();
        QCOMPARE(history.value("columns").toArray().at(1).toString(), QStringLiteral("d"));
        QCOMPARE(history.value("rows").toArray().size(), 20);
        QVERIFY(o.value("applied").toString().startsWith(QStringLiteral("no")));
        // Its result read again; its history as asked.
        r = call("fem_optimize", {{"path", file}, {"action", "result"}, {"history", 0}});
        QVERIFY(!json(r).contains(QStringLiteral("history")));
        QCOMPARE(json(r).value("summary").toString(), o.value("summary").toString());
        r = call("fem_optimize", {{"path", file}, {"action", "result"}, {"history", -1}});
        QCOMPARE(json(r).value("history").toObject().value("rows").toArray().size(), o.value("solves").toInt());
        // fem_describe: the tree without its rows; what it found, in a line.
        r = call("fem_describe", {{"path", file}, {"what", "model"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(!text(r).contains(QStringLiteral("\"history\"")), qPrintable(text(r).left(2000)));
        const QJsonArray described = json(r).value("optimizations").toArray();
        QCOMPARE(described.size(), 1);
        QCOMPARE(described.at(0).toObject().value("status").toString(), QStringLiteral("converged"));
        QVERIFY(text(r).contains(QStringLiteral("last result")));
        // Its format says what it takes.
        r = call("fem_describe", {{"what", "format"}});
        QVERIFY2(text(r).contains(QStringLiteral("optimizations")), "the format's paragraph");
        QVERIFY2(text(r).contains(QStringLiteral("objectiveweight")), "its kind's properties");
        // Written in.
        r = call("fem_optimize", {{"path", file}, {"action", "apply"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("written in").toArray().size(), 1);
        QVERIFY2(json(r).value("written in").toArray().at(0).toString().startsWith(QStringLiteral("d = ")), qPrintable(text(r)));
        QVERIFY(expressionOf(doc, QStringLiteral("d")).endsWith(QStringLiteral("[mm]")));
        // Solved there.
        r = call("fem_solve", {{"path", file}, {"timeout", int(jobWait() / 1000)}}, jobWait() + 5000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(std::abs(*doc->solution()->global(QStringLiteral("es.C11")) - 200e-12) < 1e-3 * 200e-12);
        // Undone, its step one (and what solving there added).
        while (doc->undoStack()->canUndo() && expressionOf(doc, QStringLiteral("d")) != QLatin1String("1[mm]")) doc->undo();
        QCOMPARE(expressionOf(doc, QStringLiteral("d")), QStringLiteral("1[mm]"));
        QVERIFY(doc->optimizationResult());
        // Run and written in, in one call.
        r = call("fem_optimize", {{"path", file}, {"apply_optimum", true}, {"history", 0}, {"timeout", int(jobWait() / 1000)}}, jobWait() + 5000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("written in").toArray().size(), 1);
        QVERIFY(expressionOf(doc, QStringLiteral("d")) != QLatin1String("1[mm]"));
    }

    // Past its timeout it goes on: fem_describe says how far, fem_stop stops it,
    // its best point so far read after.
    void theToolsStopIt()
    {
        MultiphysicsDoc* doc = model(kLong);
        QVERIFY(doc);
        const QString file = doc->getDocName();
        QJsonObject r = call("fem_optimize", {{"path", file}, {"timeout", 1}}, 30000);
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QStringLiteral("Still optimizing")), qPrintable(text(r)));
        QVERIFY(doc->isBusy());
        r = call("fem_describe", {{"path", file}, {"what", "model"}});
        QVERIFY2(json(r).contains(QStringLiteral("at work")), qPrintable(text(r).left(1000)));
        QVERIFY2(json(r).value("at work").toString().contains(QStringLiteral("evaluation")), qPrintable(json(r).value("at work").toString()));
        QVERIFY(json(r).value("optimizations").toArray().at(0).toObject().value("running").toBool());
        // Busy: another waits behind it, or says so.
        r = call("fem_optimize", {{"path", file}, {"action", "apply"}});
        QVERIFY(failed(r));
        r = call("fem_stop", {{"path", file}, {"timeout", 60}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("stopped").toBool());
        QVERIFY(!doc->isBusy());
        r = call("fem_optimize", {{"path", file}, {"action", "result"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("status").toString(), QStringLiteral("interrupted"));
        QVERIFY(json(r).contains(QStringLiteral("best")));
    }

    void theToolsChooseFromAFront()
    {
        MultiphysicsDoc* doc = model(R"({"type": "optimization", "tag": "opt1", "method": "nsga2", "maxiter": 4, "population": 8, "seed": 3,
            "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "MAX"], ["d", "MAX"]]})");
        QVERIFY(doc);
        const QString file = doc->getDocName();
        QJsonObject r = call("fem_optimize", {{"path", file}, {"apply_optimum", true}, {"timeout", int(jobWait() / 1000)}}, jobWait() + 5000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // No design said: not written in, and why.
        QVERIFY2(json(r).value("not written in").toString().contains(QStringLiteral("Pareto")), qPrintable(text(r)));
        QCOMPARE(expressionOf(doc, QStringLiteral("d")), QStringLiteral("1[mm]"));
        const QJsonArray front = json(r).value("Pareto front").toArray();
        QVERIFY(front.size() >= 2);
        QCOMPARE(front.at(0).toObject().value("point").toInt(), 1);
        QVERIFY(front.at(0).toObject().value("values").toObject().contains(QStringLiteral("d")));
        QVERIFY(front.at(0).toObject().value("objectives").toObject().contains(QStringLiteral("es.C11")));
        r = call("fem_optimize", {{"path", file}, {"action", "apply"}, {"point", 2}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const double d = doc->parameters()->values.value(QStringLiteral("d"));
        QVERIFY(std::abs(d - front.at(1).toObject().value("values").toObject().value("d").toDouble()) < 1e-9 * d);
        r = call("fem_optimize", {{"path", file}, {"action", "apply"}, {"point", 1000}});
        QVERIFY(failed(r));
    }

    // A whole model given, its section read; a key it does not read refused.
    void aWholeModelGiven()
    {
        const QJsonObject whole = QJsonDocument::fromJson(capacitor(kTune)).object();
        const QString file = path(QStringLiteral("whole.qfem"));
        QJsonObject r = call("fem_model", {{"action", "new"}, {"model", whole}, {"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* doc = qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc);
        QVERIFY(optimizationNode(doc->model(), QStringLiteral("opt1")));
        r = call("fem_optimize", {{"timeout", int(jobWait() / 1000)}}, jobWait() + 5000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("status").toString(), QStringLiteral("converged"));
    }

    // Not a 3D model's.
    void aThreeDModelsIsRefused()
    {
        QJsonObject r = call("fem_model", {{"action", "new"}, {"space", "3D"}, {"path", path(QStringLiteral("three.qfem3d"))}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_optimize", {});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QStringLiteral("2D")), qPrintable(text(r)));
        r = call("fem_edit", {{"action", "add"}, {"parent", "model"}, {"type", "optimization"}});
        QVERIFY(failed(r));
    }
};

QTEST_MAIN(TestMultiphysicsOptimize)
#include "test_multiphysics_optimize.moc"
