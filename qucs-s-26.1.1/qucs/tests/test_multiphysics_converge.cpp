/*
 * test_multiphysics_converge.cpp - a 2D model's mesh convergence check in the
 *                                  window and through Claude's tools: its node
 *                                  in the tree and its form, a run as a job
 *                                  with its page filled as it goes, Cancel,
 *                                  the model's mesh untouched, the result kept
 *                                  in the file and shown again with no solve,
 *                                  old after a change, the refined mesh shown
 *                                  and kept (a step of Undo), the menu's action
 *                                  on a model without a check; fem_converge's
 *                                  adaptive method, its result and keep,
 *                                  fem_stop, fem_describe, and its uniform
 *                                  method as before.
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
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>
#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "fem_adapt.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "multiphysicsconvergence.h"
#include "multiphysicsdoc.h"
#include "multiphysicspanel.h"
#include "multiphysicsview.h"
#include "qucs.h"
#include "qucscontrol.h"

using namespace qucs_s::fem;

namespace {

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

void shoot(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("QUCS_MP_SHOTS");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    w->grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
}

/// An L-shaped gap between electrodes (its corner's field singular), with the
/// checks \a section.
QByteArray corner(const QByteArray& section)
{
    return QByteArray(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
      "geometry": [{"type": "rectangle", "name": "square", "size": ["2", "2"]},
                   {"type": "rectangle", "name": "cut", "position": ["1", "1"], "size": ["1", "1"]},
                   {"type": "difference", "name": "plate", "input": ["square"], "tools": ["cut"]}],
      "materials": [{"label": "m", "epsilonr": "1", "selection": {"all": true}}],
      "physics": [{"type": "electrostatics", "tag": "es", "order": "1", "features": [
        {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"box": [0.99, 0.99, 2.01, 2.01]}},
        {"type": "ground", "selection": {"box": [-0.01, -0.01, 0.01, 2.01]}},
        {"type": "ground", "selection": {"box": [-0.01, -0.01, 2.01, 0.01]}}]}],
      "mesh": {"size": "normal", "custom": true, "hmax": "0.25"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}])")
           + (section.isEmpty() ? QByteArray() : QByteArray(R"(, "convergences": [)") + section + "]") + "}";
}

const QByteArray kCheck = R"({"type": "convergence", "tag": "conv1", "label": "The energy", "tolerance": "1e-3", "values": [["es.W", "", ""], ["es.C11", "", ""]]})";
// Far more than a test waits for - tens of millions of unknowns at the rate it
// shows, not so many that it stops on the projection: for a stop.
const QByteArray kLong = R"({"type": "convergence", "tag": "conv1", "tolerance": "1e-7", "maxmeshes": 40, "maxunknowns": "1e9", "values": [["es.W", "", ""]]})";

} // namespace

class TestMultiphysicsConverge : public QObject
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
    MultiphysicsDoc* model(const QByteArray& section, QString* file = nullptr)
    {
        const QString name = path(QStringLiteral("c%1.qfem").arg(++files));
        QFile f(name);
        if (!f.open(QIODevice::WriteOnly)) return nullptr;
        f.write(corner(section));
        f.close();
        if (file) *file = name;
        return open(name);
    }
    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 120000)
    {
        return control->callNow(tool, args, slowBuild() ? std::max(timeoutMs, 600000) : timeoutMs);
    }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r) { return QJsonDocument::fromJson(text(r).toUtf8()).object(); }
    QTreeWidgetItem* topItem(const QString& name) const
    {
        QTreeWidget* tree = panel()->findChild<QTreeWidget*>();
        for (int i = 0; tree && i < tree->topLevelItem(0)->childCount(); ++i)
            if (tree->topLevelItem(0)->child(i)->text(0) == name) return tree->topLevelItem(0)->child(i);
        return nullptr;
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

    // A model without one: no page, nothing in its file; the tree's node there.
    void aModelWithoutOne()
    {
        QString file;
        MultiphysicsDoc* doc = model(QByteArray(), &file);
        QVERIFY(doc);
        panel()->setDocument(doc);
        QCOMPARE(doc->bottomTabs()->indexOf(doc->convergencePage()), -1);
        QVERIFY(!doc->convergenceResult());
        doc->converge();
        QVERIFY(!doc->isBusy());
        QVERIFY(doc->lastError().contains(QStringLiteral("no mesh convergence check")));
        QVERIFY(topItem(QStringLiteral("Mesh Convergence")));
        QCOMPARE(doc->save(), 0);
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("\"convergences\""));
    }

    // Added from the tree: after the studies, before the optimizations; its form
    // and table, a row filled from what the solution has; a step of Undo.
    void addedFromTheTree()
    {
        MultiphysicsDoc* doc = model(QByteArray());
        QVERIFY(doc);
        panel()->setDocument(doc);
        QTreeWidget* tree = panel()->findChild<QTreeWidget*>();
        QStringList topLevel;
        for (int i = 0; i < tree->topLevelItem(0)->childCount(); ++i) topLevel << tree->topLevelItem(0)->child(i)->text(0);
        const int at = topLevel.indexOf(QStringLiteral("Mesh Convergence"));
        QVERIFY2(at > topLevel.indexOf(QStringLiteral("Study")) && at < topLevel.indexOf(QStringLiteral("Optimization")), qPrintable(topLevel.join(',')));
        QMenu* menu = panel()->menuFor(QStringLiteral("convergences"));
        QVERIFY(menu);
        bool offered = false;
        for (QAction* a : menu->actions()) offered = offered || a->text() == QLatin1String("Add Mesh Convergence");
        QVERIFY(offered);
        delete menu;
        // Solved first: the row's value one the results have.
        doc->compute();
        QVERIFY(doc->waitForJob(jobWait()));
        const int steps = doc->undoStack()->count();
        const QString tag = panel()->addNode(QStringLiteral("convergences"), QStringLiteral("convergence"));
        QCOMPARE(tag, QStringLiteral("conv1"));
        QCOMPARE(doc->undoStack()->count(), steps + 1);
        QCOMPARE(topItem(QStringLiteral("Mesh Convergence"))->childCount(), 1);
        QVERIFY(doc->bottomTabs()->indexOf(doc->convergencePage()) >= 0);
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("tolerance")) != nullptr);
        for (const char* key : {"study", "tolerance", "refine", "physics", "maxunknowns", "maxmeshes", "check", "sweep", "values"})
            QVERIFY2(panel()->editorFor(QString::fromLatin1(key)) != nullptr, key);
        QVERIFY(!panel()->editorFor(QStringLiteral("result")));
        auto* refine = qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("refine")));
        QVERIFY(refine);
        for (const char* r : {"auto", "goal", "uniform"}) QVERIFY2(refine->findData(QString::fromLatin1(r)) >= 0, r);
        QWidget* editor = panel()->editorFor(QStringLiteral("values"));
        auto* add = editor ? editor->findChild<QPushButton*>(QStringLiteral("mpRowsAdd")) : nullptr;
        QVERIFY(add);
        add->click();
        QTRY_COMPARE(doc->model().find(tag)->value(QStringLiteral("values")).toArray().size(), 1);
        const QString first = doc->model().find(tag)->value(QStringLiteral("values")).toArray().at(0).toArray().at(0).toString();
        QVERIFY2(first.startsWith(QStringLiteral("es.")), qPrintable(first));
        // Its buttons.
        QVERIFY(panel()->settingsArea()->findChild<QPushButton*>(QStringLiteral("mpConvergeButton")));
        shoot(panel(), QStringLiteral("convergence_tree"));
    }

    // Run: a job, its page filled as it goes and at the end; the result in the
    // node as a step of Undo; the model's own mesh as it was.
    void aRunAndItsPage()
    {
        MultiphysicsDoc* doc = model(kCheck);
        QVERIFY(doc);
        panel()->setDocument(doc);
        doc->buildMesh();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->mesh());
        const std::shared_ptr<const Mesh> before = doc->mesh();
        const int triangles = int(before->triangles.size());
        const int steps = doc->undoStack()->count();
        doc->converge(QStringLiteral("conv1"));
        QVERIFY(doc->isBusy());
        QCOMPARE(doc->converging(), QStringLiteral("conv1"));
        ConvergencePage* page = doc->convergencePage();
        QCOMPARE(doc->bottomTabs()->currentWidget(), page);
        // (Its rows as they come: at least one before the end.)
        QTRY_VERIFY_WITH_TIMEOUT(page->rowsTable()->rowCount() >= 1 || !doc->isBusy(), jobWait());
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->converging().isEmpty());
        const std::optional<ConvergenceResult> r = doc->convergenceResult(QStringLiteral("conv1"));
        QVERIFY(r);
        QVERIFY2(r->converged, qPrintable(r->summary));
        QCOMPARE(doc->undoStack()->count(), steps + 1);
        QCOMPARE(doc->undoStack()->text(doc->undoStack()->count() - 1), QStringLiteral("Mesh Convergence Result"));
        QVERIFY(page->summaryLabel()->text().contains(QStringLiteral("Converged")));
        QCOMPARE(page->rowsTable()->rowCount(), int(r->rows.size()));
        QCOMPARE(page->valuesTable()->rowCount(), 2);
        QVERIFY(page->notesEdit()->toPlainText().contains(QStringLiteral("What stands behind the verdict")));
        QVERIFY(page->notesEdit()->toPlainText().contains(QStringLiteral("check")));
        QCOMPARE(int(page->plot()->series().size()), 2);
        QVERIFY(page->keepButton()->isEnabled());
        QVERIFY(page->showMeshButton()->isEnabled());
        // The model's mesh is as it was.
        QCOMPARE(doc->mesh(), before);
        QCOMPARE(int(doc->mesh()->triangles.size()), triangles);
        QVERIFY(!doc->meshStale());
        // The tree says it.
        QTRY_COMPARE(topItem(QStringLiteral("Mesh Convergence"))->child(0)->text(1), QStringLiteral("converged"));
        shoot(app, QStringLiteral("convergence_page"));
        // Shown: the refined mesh in the view.
        QVERIFY(doc->showRefinedMesh(QStringLiteral("conv1")));
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Mesh);
        QVERIFY(int(doc->refinedMesh(QStringLiteral("conv1"))->triangles.size()) > triangles);
        shoot(app, QStringLiteral("convergence_refined_mesh"));
    }

    // Keep Refined Mesh: an Adapted Sizes feature in the mesh node, a step of
    // Undo; Undo takes it out.
    void keepRefinedMesh()
    {
        MultiphysicsDoc* doc = model(kCheck);
        QVERIFY(doc);
        panel()->setDocument(doc);
        doc->converge(QStringLiteral("conv1"));
        QVERIFY(doc->waitForJob(jobWait()));
        const int steps = doc->undoStack()->count();
        doc->convergencePage()->keepButton()->click();
        QCOMPARE(doc->undoStack()->count(), steps + 1);
        int adapted = 0;
        for (const qucs_s::fem::Node& c : doc->model().mesh().children) adapted += c.type == QLatin1String("mesh/adapted");
        QCOMPARE(adapted, 1);
        // Built, a finer mesh than the model's was.
        doc->buildMesh();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->mesh() && doc->mesh()->triangles.size() > 1000);
        // Its result is of the model before now: old, and not kept twice.
        QVERIFY(doc->convergenceStale(QStringLiteral("conv1")));
        QString why;
        QVERIFY(!doc->keepRefinedMesh(QStringLiteral("conv1"), &why));
        QVERIFY(why.contains(QStringLiteral("before a change")));
        doc->undoStack()->undo();
        adapted = 0;
        for (const qucs_s::fem::Node& c : doc->model().mesh().children) adapted += c.type == QLatin1String("mesh/adapted");
        QCOMPARE(adapted, 0);
    }

    // Kept in the file: shown again on opening, with no solve; old after a change.
    void savedAndOpened()
    {
        QString file;
        MultiphysicsDoc* doc = model(kCheck, &file);
        QVERIFY(doc);
        doc->converge(QStringLiteral("conv1"));
        QVERIFY(doc->waitForJob(jobWait()));
        QCOMPARE(doc->save(), 0);
        app->closeAllFiles();
        doc = open(file);
        QVERIFY(doc);
        panel()->setDocument(doc);
        QVERIFY(!doc->solution());
        const std::optional<ConvergenceResult> r = doc->convergenceResult(QStringLiteral("conv1"));
        QVERIFY(r && r->converged);
        QVERIFY(!doc->convergenceStale(QStringLiteral("conv1")));
        QVERIFY(doc->bottomTabs()->indexOf(doc->convergencePage()) >= 0);
        QCOMPARE(doc->convergencePage()->rowsTable()->rowCount(), int(r->rows.size()));
        // (No refined mesh in memory after opening: not shown, said why.)
        QVERIFY(!doc->convergencePage()->showMeshButton()->isEnabled());
        QString why;
        QVERIFY(!doc->showRefinedMesh(QStringLiteral("conv1"), &why));
        QVERIFY(why.contains(QStringLiteral("not kept")));
        // But kept: its sizes are in the result.
        QVERIFY(doc->convergencePage()->keepButton()->isEnabled());
        // A change: old.
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](qucs_s::fem::Node& n) { n.props.insert(QStringLiteral("hmax"), QStringLiteral("0.2")); }, QStringLiteral("x")));
        QVERIFY(doc->convergenceStale(QStringLiteral("conv1")));
        QTRY_VERIFY(doc->convergencePage()->summaryLabel()->text().contains(QStringLiteral("before a change")));
        QTRY_COMPARE(topItem(QStringLiteral("Mesh Convergence"))->child(0)->text(1), QStringLiteral("old"));
    }

    // Cancel: stopped, what it had kept and said.
    void cancel()
    {
        MultiphysicsDoc* doc = model(kLong);
        QVERIFY(doc);
        doc->converge(QStringLiteral("conv1"));
        QVERIFY(doc->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(doc->convergencePage()->rowsTable()->rowCount() >= 2, jobWait());
        doc->cancelJob();
        QVERIFY(doc->waitForJob(jobWait()));
        const std::optional<ConvergenceResult> r = doc->convergenceResult(QStringLiteral("conv1"));
        QVERIFY(r);
        QCOMPARE(r->status, QStringLiteral("cancelled"));
        QVERIFY(!r->converged);
        QVERIFY(r->rows.size() >= 2);
        QVERIFY(doc->convergencePage()->summaryLabel()->text().contains(QStringLiteral("Not converged")));
    }

    // What cannot be run: said at once, no job.
    void aBadCheck()
    {
        MultiphysicsDoc* doc = model(R"({"type": "convergence", "tag": "conv1", "tolerance": "5", "values": [["es.W", "", ""]]})");
        QVERIFY(doc);
        doc->converge(QStringLiteral("conv1"));
        QVERIFY(!doc->isBusy());
        QVERIFY(doc->lastError().contains(QStringLiteral("tolerance")));
        QVERIFY(doc->convergencePage()->summaryLabel()->text().contains(QStringLiteral("could not be run")));
        // A value the results lack: the job runs, and says what they have.
        MultiphysicsDoc* other = model(R"({"type": "convergence", "tag": "conv1", "values": [["es.nosuch", "", ""]]})");
        other->converge(QStringLiteral("conv1"));
        QVERIFY(other->waitForJob(jobWait()));
        QVERIFY(other->lastError().contains(QStringLiteral("es.C11")));
    }

    // The menu's action on a model without a check: one added, shown; solved
    // first, its value the results' first global, and run.
    void theMenusAction()
    {
        MultiphysicsDoc* doc = model(QByteArray());
        QVERIFY(doc);
        panel()->setDocument(doc);
        QAction* action = app->findChild<QAction*>(QStringLiteral("mpConvergeAction"));
        QVERIFY(action);
        doc->compute();
        QVERIFY(doc->waitForJob(jobWait()));
        action->trigger();
        QVERIFY(convergenceNode(doc->model(), QString()));
        QVERIFY(doc->isBusy());
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->convergenceResult());
    }

    // fem_converge, adaptive: the node made from the call, run, its answer; the
    // result read again; keep; fem_describe lists it.
    void theTool()
    {
        QString file;
        MultiphysicsDoc* doc = model(QByteArray(), &file);
        QVERIFY(doc);
        QJsonObject r = call("fem_converge", {{"path", file}, {"method", "adaptive"}, {"values", QJsonArray{"es.W"}}, {"tolerance", 1e-3}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("convergence").toString(), QStringLiteral("conv1"));
        QVERIFY(o.value("converged").toBool());
        QCOMPARE(o.value("status").toString(), QStringLiteral("converged"));
        QVERIFY(o.value("evidence").toArray().size() >= 2);
        const QJsonObject value = o.value("values").toArray().at(0).toObject();
        QCOMPARE(value.value("value").toString(), QStringLiteral("es.W"));
        QVERIFY(value.value("settled").toBool());
        QVERIFY(value.contains("estimated limit") && value.contains("estimated error %") && value.contains("trust"));
        QVERIFY(o.value("meshes").toArray().size() >= 3);
        QCOMPARE(o.value("meshes").toArray().last().toObject().value("kind").toString(), QStringLiteral("check"));
        QVERIFY(o.value("mesh").toString().contains(QStringLiteral("as it was")));
        QVERIFY(convergenceNode(doc->model(), QStringLiteral("conv1")));
        // Read again: nothing solved.
        r = call("fem_converge", {{"path", file}, {"method", "adaptive"}, {"action", "result"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("solves").toInt(), o.value("solves").toInt());
        // fem_describe: its verdict.
        r = call("fem_describe", {{"path", file}});
        QVERIFY(text(r).contains(QStringLiteral("\"convergences\"")));
        // Keep.
        r = call("fem_converge", {{"path", file}, {"method", "adaptive"}, {"action", "keep"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        int adapted = 0;
        for (const qucs_s::fem::Node& c : doc->model().mesh().children) adapted += c.type == QLatin1String("mesh/adapted");
        QCOMPARE(adapted, 1);
        // The settings of a second run go into the node; goal-oriented said.
        r = call("fem_converge", {{"path", file}, {"method", "adaptive"}, {"by", "goal"}, {"max_meshes", 3}, {"tolerance", 1e-9}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        o = json(r);
        QCOMPARE(o.value("status").toString(), QStringLiteral("meshes"));
        QCOMPARE(doc->model().find(QStringLiteral("conv1"))->text(QStringLiteral("refine")), QStringLiteral("goal"));
        QVERIFY(o.value("refined by").toString().contains(QStringLiteral("energy")));
        // Bad input, said before anything runs.
        for (const QJsonObject& bad : {QJsonObject{{"method", "bogus"}}, QJsonObject{{"method", "adaptive"}, {"tolerance", 2}},
                                       QJsonObject{{"method", "adaptive"}, {"by", "x"}}, QJsonObject{{"method", "adaptive"}, {"max_meshes", 1}},
                                       QJsonObject{{"method", "adaptive"}, {"action", "x"}}, QJsonObject{{"method", "adaptive"}, {"convergence", "conv9"}},
                                       QJsonObject{{"method", "adaptive"}, {"sweep", "middle"}}, QJsonObject{{"method", "adaptive"}, {"values", QJsonArray{""}}}}) {
            QJsonObject args = bad;
            args.insert("path", file);
            r = call("fem_converge", args);
            QVERIFY2(failed(r), qPrintable(QJsonDocument(bad).toJson()));
        }
        // A model without a check and no values: said.
        QString other;
        model(QByteArray(), &other);
        r = call("fem_converge", {{"path", other}, {"method", "adaptive"}});
        QVERIFY(failed(r) && text(r).contains(QStringLiteral("values")));
    }

    // Past its timeout it goes on; fem_stop stops it, the meshes so far kept.
    void stoppedByTheTool()
    {
        QString file;
        MultiphysicsDoc* doc = model(kLong, &file);
        QVERIFY(doc);
        QJsonObject r = call("fem_converge", {{"path", file}, {"method", "adaptive"}, {"timeout", 1}}, 30000);
        if (failed(r) && text(r).contains(QStringLiteral("Still checking"))) {
            qInfo("stopped by fem_stop: %s", qPrintable(text(r).left(120)));
            QVERIFY(doc->isBusy());
            QVERIFY(text(call("fem_describe", {{"path", file}})).contains(QStringLiteral("at work")));
            r = call("fem_stop", {{"path", file}, {"timeout", 60}}, jobWait());
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QVERIFY(!doc->isBusy());
            const std::optional<ConvergenceResult> kept = doc->convergenceResult(QStringLiteral("conv1"));
            QVERIFY(kept && kept->status == QLatin1String("cancelled"));
        } else {
            // (A machine fast enough to reach its limits in a second.)
            QVERIFY2(!failed(r), qPrintable(text(r)));
        }
    }

    // The uniform method as it was: the default, its answer's keys.
    void theUniformMethodAsBefore()
    {
        QString file;
        model(QByteArray(), &file);
        QJsonObject r = call("fem_converge", {{"path", file}, {"values", QJsonArray{"es.W"}}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject o = json(r);
        QVERIFY(o.contains("between the last two meshes"));
        QCOMPARE(o.value("meshes").toArray().size(), 2);
        r = call("fem_converge", {{"path", file}});
        QVERIFY(failed(r) && text(r).contains(QStringLiteral("'values'")));
    }
};

QTEST_MAIN(TestMultiphysicsConverge)
#include "test_multiphysics_converge.moc"
