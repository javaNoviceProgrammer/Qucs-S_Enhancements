/*
 * test_multiphysics_doc.cpp - the multiphysics solver in the window: the
 *                             Multiphysics panel (its tree, menus and
 *                             settings), a model's tab (built, meshed,
 *                             solved in the background; its plots, its
 *                             table), picking in the Graphics view, Undo,
 *                             saving and reading, the Multiphysics menu,
 *                             and Claude's fem tools
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
#include <QClipboard>
#include <QComboBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "layoutdoc.h"
#include "multiphysicsdoc.h"
#include "multiphysicspanel.h"
#include "dataset.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"

using namespace qucs_s::fem;

namespace {

QString examples()
{
    return QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
}

/// The view as an image, saved where QUCS_MP_SHOTS says (to look at).
void shoot(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("QUCS_MP_SHOTS");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    w->grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
}

/// A Debug or sanitized build (CI's Linux): ten to thirty times slower -
/// the coax solved at fewer frequencies there.
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

int countColors(const QImage& image)
{
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 3)
        for (int x = 0; x < image.width(); x += 3) colors.insert(image.pixel(x, y));
    return int(colors.size());
}

} // namespace

class TestMultiphysicsDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    MultiphysicsPanel* panel() const { return app->multiphysicsPanel(); }
    MultiphysicsDoc* open(const QString& file)
    {
        if (!app->gotoPage(file)) return nullptr;
        return qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
    }
    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 120000)
    {
        return control->callNow(tool, args, timeoutMs);
    }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r) { return QJsonDocument::fromJson(text(r).toUtf8()).object(); }
    /// The coax example in the workspace: at 9 frequencies, or (a slow
    /// build) 3 - 1 kHz, 100 kHz and 10 MHz.
    int coaxFrequencies() const { return slowBuild() ? 3 : 9; }
    QString coaxCopy()
    {
        const QString to = copyOf(QStringLiteral("coax_rlgc.qfem"));
        if (slowBuild()) {
            QFile f(to);
            f.open(QIODevice::ReadOnly);
            QString text = QString::fromUtf8(f.readAll());
            f.close();
            text.replace(QStringLiteral("logspace(1[kHz], 10[MHz], 9)"), QStringLiteral("logspace(1[kHz], 10[MHz], 3)"));
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write(text.toUtf8());
        }
        return to;
    }
    /// A copy of example \a name in the workspace, to change.
    QString copyOf(const QString& name)
    {
        const QString to = path(name);
        QFile::remove(to);
        QFile::copy(examples() + name, to);
        QFile(to).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return to;
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
            if (auto* m = dynamic_cast<MultiphysicsDoc*>(d)) m->waitForJob(120000);
            d->setDocChanged(false);   // (a data display too: no Save dialog)
        }
        app->closeAllFiles();
    }

    // ---- The panel, the tab

    // The left dock's sixth tab: with no model in front, it offers a new
    // one, to open one, the examples.
    void thePanelIsTheDocksSixthTab()
    {
        QVERIFY(panel() != nullptr);
        auto* tabs = panel()->parentWidget() ? qobject_cast<QTabWidget*>(panel()->parentWidget()->parentWidget()) : nullptr;
        QVERIFY(tabs != nullptr);
        QCOMPARE(tabs->tabText(tabs->indexOf(panel())), QStringLiteral("Multiphysics"));
        QCOMPARE(tabs->indexOf(panel()), 5);
        QCOMPARE(panel()->pages()->currentIndex(), 0);
        QVERIFY(panel()->exampleList()->count() >= 3);
        app->showMultiphysicsPanel();
        shoot(app, "00-start");
        // The Multiphysics menu, before View.
        QMenu* menu = app->multiphysicsMenu();
        QVERIFY(menu != nullptr);
        QVERIFY(app->menuBar()->actions().contains(menu->menuAction()));
    }

    // An example opened: its tree in the panel, its geometry built and
    // shown, the panel brought to the front.
    void anExampleOpensWithItsTree()
    {
        MultiphysicsDoc* doc = open(examples() + "microstrip.qfem");
        QVERIFY(doc != nullptr);
        QCOMPARE(panel()->document(), doc);
        QCOMPARE(panel()->pages()->currentIndex(), 1);
        QTreeWidget* tree = panel()->tree();
        QStringList names;
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            names << (*it)->text(0);
            ++it;
        }
        for (const char* n : {"microstrip.qfem", "Global Definitions", "Parameters", "Geometry", "air", "substrate", "trace",
                              "Form Union", "Materials", "FR-4", "Electrostatics", "Strip", "Ground plane", "Mesh", "Study 1", "Results",
                              "Electric Potential", "Derived Values", "Line Parameters"})
            QVERIFY2(names.contains(QString::fromLatin1(n)), n);
        QVERIFY(!doc->geometryStale());
        QVERIFY(doc->geometry().ok());
        QCOMPARE(int(doc->geometry().topology->domains.size()), 3);
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Geometry);
        shoot(app, "01-microstrip-geometry");
        // A geometry feature chosen: its object's domains shown.
        panel()->selectNode(doc->model().find(QStringLiteral("r3")) ? QStringLiteral("r3") : QString());
        for (const qucs_s::fem::Node& f : doc->model().geometry().children)
            if (f.label == QLatin1String("trace")) panel()->selectNode(f.tag);
        QCOMPARE(doc->view()->highlightLevel(), Level::Domain);
        QCOMPARE(doc->view()->highlighted().size(), 1);
        // Switching to a schematic: the panel offers to start again.
        app->slotFileNew();
        QCOMPARE(panel()->document(), nullptr);
        QCOMPARE(panel()->pages()->currentIndex(), 0);
    }

    // Compute: meshed and solved in the background; the plot groups and
    // the table filled; the colors on the view.
    void computeMeshesSolvesAndPlots()
    {
        MultiphysicsDoc* doc = open(examples() + "microstrip.qfem");
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->isBusy() || doc->solution());
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QVERIFY(doc->solution() != nullptr);
        QVERIFY(doc->mesh() != nullptr);
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Results);
        QVERIFY(doc->plotScene() && doc->plotScene()->surface);
        // The derived values in the table: Z0 near 50 Ω.
        QTableWidget* table = doc->table();
        bool z0 = false;
        for (int r = 0; r < table->rowCount(); ++r)
            if (table->item(r, 1)->text() == QLatin1String("Z0")) {
                z0 = true;
                const double ohms = table->item(r, 2)->text().section(QLatin1Char(' '), 0, 0).toDouble();
                QVERIFY2(std::abs(ohms - 50.3) < 1, qPrintable(table->item(r, 2)->text()));
            }
        QVERIFY(z0);
        // The color map drawn: many colors on the view.
        const QImage picture = doc->view()->picture();
        QVERIFY2(countColors(picture) > 60, "the color map is not drawn");
        shoot(app, "02-microstrip-potential");
        // The other plot group: the field, its arrows.
        QString why;
        for (const qucs_s::fem::Node& g : doc->model().results().children)
            if (g.label == QLatin1String("Electric Field")) QVERIFY2(doc->showPlotGroup(g.tag, &why), qPrintable(why));
        QVERIFY(!doc->plotScene()->arrows.empty());
        shoot(app, "03-microstrip-field");
        // The mesh shown.
        doc->showMesh();
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Mesh);
        shoot(app, "04-microstrip-mesh");
        // The study's status in the tree: solved.
        QTreeWidgetItemIterator it(panel()->tree());
        bool solved = false;
        while (*it) {
            if ((*it)->text(0) == QLatin1String("Study 1")) solved = (*it)->text(1) == QLatin1String("solved");
            ++it;
        }
        QVERIFY(solved);
    }

    // A model changed after it was solved: its solution kept on show,
    // marked old; the geometry built again at once.
    void anEditMakesTheSolutionOld()
    {
        MultiphysicsDoc* doc = open(examples() + "coax.qfem");
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY(doc->solution());
        QVERIFY(!doc->solutionStale());
        Model m = doc->model();
        QList<Model::Parameter> params = m.parameters();
        params[0].expression = QStringLiteral("0.5[mm]");
        m.setParameters(params);
        QVERIFY(doc->setModel(m, QStringLiteral("a")));
        QVERIFY(doc->solutionStale());
        QVERIFY(doc->solution() != nullptr);
        QVERIFY(doc->meshStale());
        // Undo: the parameter as it was (the solution stays marked: its
        // model changed once).
        QVERIFY(doc->undoStack()->canUndo());
        doc->undo();
        QCOMPARE(doc->model().parameters().first().expression, QStringLiteral("0.45[mm]"));
        doc->redo();
        QCOMPARE(doc->model().parameters().first().expression, QStringLiteral("0.5[mm]"));
        QVERIFY(doc->getDocChanged());
    }

    // ---- Building a model in the panel

    // A new model from scratch, as a user makes one: a rectangle, a circle
    // in it, a material, electrostatics with a terminal picked in the
    // view, computed.
    void aModelIsBuiltInThePanel()
    {
        app->multiphysicsMenu()->findChild<QAction*>(QStringLiteral("mpNewModelAction"))->trigger();
        auto* doc = qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QCOMPARE(panel()->document(), doc);
        const QString geometry = doc->model().geometry().tag;
        const QString box = panel()->addNode(geometry, QStringLiteral("rectangle"));
        QVERIFY(!box.isEmpty());
        QCOMPARE(panel()->currentTag(), box);
        // Its size set in the settings, as typed.
        auto* w = panel()->settingsArea()->findChild<QLineEdit*>(QStringLiteral("mpEdit_size_x"));
        auto* h = panel()->settingsArea()->findChild<QLineEdit*>(QStringLiteral("mpEdit_size_y"));
        QVERIFY(w && h);
        w->setText(QStringLiteral("10"));
        emit w->editingFinished();
        QTRY_COMPARE(doc->model().find(box)->pair(QStringLiteral("size")).first(), QStringLiteral("10"));
        h = panel()->settingsArea()->findChild<QLineEdit*>(QStringLiteral("mpEdit_size_y"));
        h->setText(QStringLiteral("4"));
        emit h->editingFinished();
        QTRY_COMPARE(doc->model().find(box)->pair(QStringLiteral("size")).last(), QStringLiteral("4"));
        const QString disk = panel()->addNode(geometry, QStringLiteral("circle"));
        QVERIFY(!disk.isEmpty());
        QVERIFY(doc->editNode(disk, [](qucs_s::fem::Node& n) {
            n.props.insert(QStringLiteral("center"), QJsonArray{QStringLiteral("5"), QStringLiteral("2")});
            n.props.insert(QStringLiteral("radius"), QStringLiteral("1"));
        }, QStringLiteral("circle")));
        // The geometry built at once.
        QTRY_VERIFY(!doc->geometryStale());
        QVERIFY(doc->geometry().ok());
        QCOMPARE(int(doc->geometry().topology->domains.size()), 2);
        // A material from the library, on the box's own domain.
        const QString mat = panel()->addNode(doc->model().materials().tag, QStringLiteral("material:FR-4"));
        QVERIFY(!mat.isEmpty());
        QCOMPARE(doc->model().find(mat)->text(QStringLiteral("epsilonr")), QStringLiteral("4.4"));
        QVERIFY(doc->picking());
        QCOMPARE(doc->view()->pickLevel(), Level::Domain);
        // Picked in the Graphics view: a click on the box, away from the disk.
        GraphicsView* view = doc->view();
        view->fit();
        const QPointF at = view->toPixel(QPointF(1.5, 1));
        QCOMPARE(view->entityAt(Level::Domain, at), 0);
        QTest::mouseClick(view, Qt::LeftButton, {}, at.toPoint());
        QTRY_COMPARE(doc->model().find(mat)->selection().value(QStringLiteral("numbers")).toArray(), QJsonArray{1});
        // Another material on the disk: picked too; its domain a click away.
        const QString metal = panel()->addNode(doc->model().materials().tag, QStringLiteral("material:Copper"));
        QTest::mouseClick(view, Qt::LeftButton, {}, view->toPixel(QPointF(5, 2)).toPoint());
        QTRY_COMPARE(doc->model().find(metal)->selection().value(QStringLiteral("numbers")).toArray(), QJsonArray{2});
        // Electrostatics, a terminal on the circle's boundaries, the box's bottom grounded.
        const QString es = panel()->addNode(doc->model().component().tag, QStringLiteral("electrostatics"));
        QCOMPARE(es, QStringLiteral("es"));
        QVERIFY(doc->editNode(es, [](qucs_s::fem::Node& n) {
            n.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("r1")}}});
        }, QStringLiteral("domains")));
        const QString term = panel()->addNode(es, QStringLiteral("electrostatics/terminal"));
        QVERIFY(doc->editNode(term, [](qucs_s::fem::Node& n) {
            n.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("of"), QJsonArray{QStringLiteral("c1")}}});
        }, QStringLiteral("terminal")));
        const QString ground = panel()->addNode(es, QStringLiteral("electrostatics/ground"));
        QVERIFY(doc->editNode(ground, [](qucs_s::fem::Node& n) {
            n.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("of"), QJsonArray{QStringLiteral("r1")}}, {QStringLiteral("side"), QStringLiteral("bottom")}});
        }, QStringLiteral("ground")));
        panel()->selectNode(term);
        QCOMPARE(doc->view()->highlightLevel(), Level::Boundary);
        QCOMPARE(doc->view()->highlighted().size(), 4);
        shoot(app, "05-built-in-panel");
        // Compute: the default plots made.
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QStringList groups;
        for (const qucs_s::fem::Node& g : doc->model().results().children)
            if (g.type == QLatin1String("plotgroup")) groups << g.name();
        QCOMPARE(groups, (QStringList{QStringLiteral("Electric Potential (es)"), QStringLiteral("Electric Field (es)")}));
        QVERIFY(doc->solution()->global(QStringLiteral("es.C11")).value_or(0) > 0);
        shoot(app, "06-built-potential");
        // Saved: a .qfem that reads back the same.
        doc->setName(path("built.qfem"));
        QCOMPARE(doc->save(), 0);
        QVERIFY(!doc->getDocChanged());
        QFile file(path("built.qfem"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString why;
        const std::optional<Model> back = Model::fromJson(file.readAll(), &why);
        QVERIFY2(back, qPrintable(why));
        QCOMPARE(back->toJson(), doc->model().toJson());
    }

    // The tree's menus: what may be added where; copy, disable, delete.
    void theTreeMenus()
    {
        MultiphysicsDoc* doc = open(copyOf("coax.qfem"));
        QVERIFY(doc != nullptr);
        QMenu* menu = panel()->menuFor(doc->model().geometry().tag);
        QVERIFY(menu != nullptr);
        QStringList actions;
        std::function<void(QMenu*)> collect = [&](QMenu* m) {
            for (QAction* a : m->actions()) {
                actions << a->text();
                if (a->menu()) collect(a->menu());
            }
        };
        collect(menu);
        for (const char* a : {"Rectangle", "Circle", "Polygon", "Difference", "Build All"}) QVERIFY2(actions.contains(QString::fromLatin1(a)), a);
        delete menu;
        menu = panel()->menuFor(QStringLiteral("es"));
        actions.clear();
        collect(menu);
        for (const char* a : {"Terminal", "Ground", "Floating Potential", "Disable", "Delete"}) QVERIFY2(actions.contains(QString::fromLatin1(a)), a);
        delete menu;
        // A default feature: not deleted.
        QString zeroCharge;
        for (const qucs_s::fem::Node& f : doc->model().find(QStringLiteral("es"))->children)
            if (f.isDefault && f.type == QLatin1String("electrostatics/zerocharge")) zeroCharge = f.tag;
        QVERIFY(!zeroCharge.isEmpty());
        menu = panel()->menuFor(zeroCharge);
        actions.clear();
        collect(menu);
        QVERIFY(!actions.contains(QStringLiteral("Delete")));
        delete menu;
        // Duplicate, disable, delete: each a step of Undo.
        const int steps = doc->undoStack()->count();
        QString core;
        for (const qucs_s::fem::Node& f : doc->model().geometry().children)
            if (f.label == QLatin1String("core")) core = f.tag;
        QVERIFY(panel()->duplicateNode(core));
        QVERIFY(doc->model().objectNames().contains(QStringLiteral("core2")));
        QVERIFY(panel()->setNodeEnabled(QStringLiteral("es"), false));
        QVERIFY(!doc->model().find(QStringLiteral("es"))->enabled);
        QVERIFY(panel()->removeNode(panel()->currentTag()));
        QCOMPARE(doc->undoStack()->count(), steps + 3);
        doc->undo();
        doc->undo();
        doc->undo();
        QVERIFY(doc->model().find(QStringLiteral("es"))->enabled);
        QVERIFY(!doc->model().objectNames().contains(QStringLiteral("core2")));
        // A geometry object renamed: the rules that named it follow.
        QVERIFY(panel()->renameNode(core, QStringLiteral("inner")));
        QString terminal;
        for (const qucs_s::fem::Node& f : doc->model().find(QStringLiteral("es"))->children)
            if (f.type == QLatin1String("electrostatics/terminal")) terminal = f.tag;
        QCOMPARE(doc->model().find(terminal)->selection().value(QStringLiteral("of")).toArray().first().toString(), QStringLiteral("inner"));
    }

    // The settings of a node: its properties as the kind says; an
    // expression checked as typed; those shown only when another says.
    void theSettingsForm()
    {
        MultiphysicsDoc* doc = open(copyOf("resistor_heating.qfem"));
        QVERIFY(doc != nullptr);
        QString flux;
        for (const qucs_s::fem::Node& f : doc->model().find(QStringLiteral("ht"))->children)
            if (f.type == QLatin1String("heat/flux")) flux = f.tag;
        panel()->selectNode(flux);
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("h")) != nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("q0")) == nullptr);   // (a convective flux)
        auto* h = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("h")));
        QVERIFY(h->toolTip().contains(QStringLiteral("15")));
        h->setText(QStringLiteral("hair*"));
        QVERIFY(h->styleSheet().contains(QStringLiteral("c62828")));   // red: it does not compile
        h->setText(QStringLiteral("2*hair"));
        QVERIFY(!h->styleSheet().contains(QStringLiteral("c62828")));
        emit h->editingFinished();
        QTRY_COMPARE(doc->model().find(flux)->text(QStringLiteral("h")), QStringLiteral("2*hair"));
        // The kind of flux changed: its fields too.
        auto* kind = qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("kind")));
        QVERIFY(kind != nullptr);
        kind->setCurrentIndex(kind->findData(QStringLiteral("general")));
        emit kind->activated(kind->currentIndex());
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("q0")) != nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("h")) == nullptr);
        // The parameters' table: their values beside them.
        panel()->selectNode(QStringLiteral("parameters"));
        QTRY_VERIFY(panel()->settingsArea()->findChild<QTableWidget*>(QStringLiteral("mpRows")) != nullptr);
        auto* rows = panel()->settingsArea()->findChild<QTableWidget*>(QStringLiteral("mpRows"));
        QCOMPARE(rows->item(0, 0)->text(), QStringLiteral("L"));
        QCOMPARE(rows->item(0, 2)->text(), QStringLiteral("2 mm"));
        shoot(app, "07-settings");
        // A material's settings, a terminal's selection editor.
        for (const qucs_s::fem::Node& mat : doc->model().materials().children)
            if (mat.label == QLatin1String("Resistive film")) panel()->selectNode(mat.tag);
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("sigma")) != nullptr);
        shoot(app, "07b-material");
        for (const qucs_s::fem::Node& f : doc->model().find(QStringLiteral("ec"))->children)
            if (f.type == QLatin1String("currents/terminal")) panel()->selectNode(f.tag);
        QTRY_VERIFY(panel()->settingsArea()->findChild<QWidget*>(QStringLiteral("mpSelection")) != nullptr);
        auto* shown = panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("mpSelectionShown"));
        QVERIFY(shown && shown->text().contains(QStringLiteral("boundaries of padL (left)")));
        shoot(app, "07c-terminal");
    }

    // The coupled example: solved in turn, its temperature shown.
    void aCoupledExampleSolves()
    {
        MultiphysicsDoc* doc = open(examples() + "resistor_heating.qfem");
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        const double r = doc->solution()->global(QStringLiteral("ec.R")).value_or(0);
        QVERIFY2(std::abs(r - 0.2011) < 0.002, qPrintable(QString::number(r)));
        const double tmax = doc->solution()->global(QStringLiteral("ht.Tmax")).value_or(0);
        QVERIFY2(tmax > 300 && tmax < 360, qPrintable(QString::number(tmax)));
        shoot(app, "08-resistor-temperature");
    }

    // ---- Claude's tools

    // A model written whole by fem_model, changed by fem_edit, solved,
    // evaluated, plotted - the answers as the panel would show.
    void claudesToolsBuildAndSolveAModel()
    {
        // The format first.
        QJsonObject r = call("fem_describe", {{"what", "format"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject format = json(r);
        QVERIFY(format.value("format").toString().contains("selection"));
        QStringList types;
        for (const QJsonValue& k : format.value("kinds").toArray()) types << k.toObject().value("type").toString();
        for (const char* t : {"rectangle", "electrostatics/terminal", "heat/flux", "lineparams"}) QVERIFY2(types.contains(QString::fromLatin1(t)), t);
        QVERIFY(format.value("materials").toArray().size() >= 15);
        // A parallel-plate capacitor, written whole and saved.
        const QJsonObject model = QJsonDocument::fromJson(R"({"format": "qucs-s multiphysics 1",
            "parameters": [{"name": "d", "expression": "1[mm]"}, {"name": "w", "expression": "10[mm]"}],
            "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "label": "gap", "size": ["w", "d"]}],
              "materials": [{"label": "glass", "epsilonr": "5", "selection": {"all": true}}],
              "physics": [{"type": "electrostatics", "tag": "es", "order": "1", "features": [
                {"type": "terminal", "name": "1", "selection": {"of": ["gap"], "side": "top"}},
                {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}],
              "mesh": {"size": "coarse"}}],
            "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})").object();
        r = call("fem_model", {{"action", "new"}, {"path", "plates"}, {"model", model}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(path("plates.qfem")));
        QCOMPARE(json(r).value("geometry").toString(), QStringLiteral("built"));
        // Its geometry: one domain, the top boundary picked by the rule.
        r = call("fem_describe", {{"what", "geometry"}, {"selection", QJsonObject{{"of", QJsonArray{"gap"}}, {"side", "top"}}}, {"level", "boundary"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("domains").toInt(), 1);
        QCOMPARE(json(r).value("selection picks").toArray().size(), 1);
        // Solved: C = ε w / d per metre.
        r = call("fem_solve");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString c11 = json(r).value("globals").toObject().value("es.C11").toString();
        QVERIFY2(c11.startsWith("442.7") && c11.endsWith("pF"), qPrintable(c11));
        // Evaluated at a point, over the domain, as a global.
        QJsonArray middle;   // (appended: a braced list of one list is a copy to some compilers)
        middle.append(QJsonArray{5, 0.5});
        r = call("fem_evaluate", {{"expression", "V"}, {"at", middle}, {"unit", "mV"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("values").toArray().first().toObject().value("value").toString(), QStringLiteral("500 mV"));
        r = call("fem_evaluate", {{"expression", "es.normE"}, {"over", "domain"}, {"kind", "maximum"}, {"unit", "kV/m"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("values").toArray().first().toObject().value("value").toString().startsWith("1 kV/m"));
        r = call("fem_evaluate", {{"expression", "es.C11/(eps0*5)"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(std::abs(json(r).value("values").toArray().first().toObject().value("SI").toDouble() - 10) < 1e-6);
        // Changed: the dielectric twice as thick - one step of Undo; solved again.
        r = call("fem_edit", {{"action", "parameters"}, {"parameters", QJsonArray{QJsonObject{{"name", "d"}, {"expression", "2[mm]"}},
                                                                                QJsonObject{{"name", "w"}, {"expression", "10[mm]"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_solve");
        QVERIFY2(json(r).value("globals").toObject().value("es.C11").toString().startsWith("221.4"), qPrintable(text(r)));
        // A node added, a property that is not there refused, the kinds told.
        r = call("fem_edit", {{"action", "add"}, {"parent", "es"}, {"type", "surfacecharge"}, {"props", QJsonObject{{"rhos", "1[uC/m^2]"}, {"selection", QJsonObject{{"all", true}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("added").toObject().value("type").toString(), QStringLiteral("electrostatics/surfacecharge"));
        r = call("fem_edit", {{"action", "set"}, {"tag", json(r).value("added").toObject().value("tag").toString()}, {"props", QJsonObject{{"voltage", "1"}}}});
        QVERIFY(failed(r));
        QVERIFY(text(r).contains("rhos"));
        r = call("fem_edit", {{"action", "add"}, {"parent", "geometry"}, {"type", "hexagon"}});
        QVERIFY(failed(r));
        // A library material.
        r = call("fem_edit", {{"action", "add"}, {"parent", "materials"}, {"type", "material:Silicon"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("added").toObject().value("props").toObject().value("epsilonr").toString(), QStringLiteral("11.7"));
        // A plot of an expression made and shown.
        r = call("fem_plot", {{"expression", "V"}, {"contours", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("range").toString().contains("V"));
        auto* doc = qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Results);
        // Described: its tree and studies.
        r = call("fem_describe");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("studies").toArray().first().toObject().value("solved").toBool(), true);
        QVERIFY(json(r).value("unsaved").toBool());
        r = call("fem_model", {{"action", "save"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!doc->getDocChanged());
    }

    // An error as the model has it: said, nothing half done.
    void claudesToolsSayWhatIsWrong()
    {
        const QJsonObject model = QJsonDocument::fromJson(R"({"format": "qucs-s multiphysics 1",
            "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "label": "r", "size": ["1", "1"]}],
              "physics": [{"type": "heat", "tag": "ht", "features": [{"type": "temperature", "selection": {"all": true}}]}]}],
            "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})").object();
        QJsonObject r = call("fem_model", {{"action", "new"}, {"model", model}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_solve");
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("no material"), qPrintable(text(r)));
        r = call("fem_evaluate", {{"expression", "T"}});
        QVERIFY(failed(r));
        QVERIFY(text(r).contains("not solved"));
        // A geometry that does not build: where it stopped.
        r = call("fem_edit", {{"action", "add"}, {"parent", "geometry"}, {"type", "circle"}, {"props", QJsonObject{{"radius", "-1"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("geometry").toString().contains("radius"), qPrintable(text(r)));
    }

    // Cancel: a job stopped, the model left as it was.
    void aJobIsCancelled()
    {
        MultiphysicsDoc* doc = open(copyOf("coax.qfem"));
        QVERIFY(doc != nullptr);
        // A fine mesh, slow enough to be cancelled.
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](qucs_s::fem::Node& n) {
            n.props.insert(QStringLiteral("custom"), true);
            n.props.insert(QStringLiteral("hmax"), QStringLiteral("0.002"));
        }, QStringLiteral("fine")));
        doc->compute();
        QVERIFY(doc->isBusy());
        QVERIFY(doc->cancelButton()->isVisible());
        doc->cancelJob();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY(doc->solution() == nullptr);
        QVERIFY(doc->lastError().contains(QStringLiteral("cancelled")));
        QVERIFY(!doc->cancelButton()->isVisible());
    }

    // ---- Phase 2: time, sweeps, mechanics, axisymmetry, drawings, graphs

    /// The tag of the node of type \a type labelled \a label.
    static QString tagOf(const Model& m, const QString& type, const QString& label)
    {
        std::function<QString(const qucs_s::fem::Node&)> find = [&](const qucs_s::fem::Node& n) -> QString {
            if (n.type == type && n.name() == label) return n.tag;
            for (const qucs_s::fem::Node& c : n.children)
                if (const QString t = find(c); !t.isEmpty()) return t;
            return QString();
        };
        return find(m.root);
    }

    // A study in time: its plot at the last output time, a box to choose
    // another (the plot group's setting, a step of Undo), Derived Values at
    // the time it asks for.
    void aStudyInTimeIsShownAtItsTimes()
    {
        MultiphysicsDoc* doc = open(copyOf("heater_transient.qfem"));
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QCOMPARE(doc->solution()->snapshotCount(), 31);
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Results);
        QVERIFY2(doc->plotScene()->title.contains("t = 60 s"), qPrintable(doc->plotScene()->title));
        QVERIFY(doc->instanceBox()->isVisible());
        QCOMPARE(doc->instanceBox()->count(), 31);
        // Another time, chosen in the box.
        const int at = doc->instanceBox()->findText(QStringLiteral("t = 10 s"));
        QVERIFY(at >= 0);
        doc->instanceBox()->setCurrentIndex(at);
        emit doc->instanceBox()->activated(at);
        QVERIFY2(doc->plotScene()->title.contains("t = 10 s"), qPrintable(doc->plotScene()->title));
        const double max10 = doc->plotScene()->values.max;
        doc->undo();
        QVERIFY(doc->plotScene()->title.contains("t = 60 s"));
        QVERIFY(doc->plotScene()->values.max > max10);
        shoot(app, "11-heater-in-time");
        // "Hottest" at the time it asks for: the last, one row.
        int rows = 0;
        for (int r = 0; r < doc->table()->rowCount(); ++r)
            if (doc->table()->item(r, 0)->text() == QLatin1String("Hottest")) ++rows;
        QCOMPARE(rows, 1);
        // The plot group's settings: its times to choose from.
        panel()->selectNode(tagOf(doc->model(), "plotgroup", "Temperature"));
        auto* times = qobject_cast<QComboBox*>(panel()->editorFor("time"));
        QVERIFY(times != nullptr);
        QCOMPARE(times->count(), 32);   // (the last, and each)
    }

    // A 1D Plot Group: a Qucs dataset beside the model and a data display of
    // it, read as Qucs reads it.
    void graphsGoToAQucsDataDisplay()
    {
        MultiphysicsDoc* doc = open(copyOf("heater_transient.qfem"));
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QString why;
        const QString display = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Temperature in Time"), &why);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        QCOMPARE(QFileInfo(display).fileName(), QStringLiteral("heater_transient_%1.dpl").arg(tagOf(doc->model(), "plotgroup1d", "Temperature in Time")));
        QVERIFY(QFileInfo::exists(display));
        // The display in front, a diagram of the graphs.
        auto* shown = qobject_cast<Schematic*>(app->DocumentTab->currentWidget());
        QVERIFY(shown != nullptr);
        QCOMPARE(shown->a_DocDiags.size(), 1);
        QCOMPARE(int(shown->a_DocDiags.front()->Graphs.size()), 3);
        shoot(app, "12-heater-graph");
        // The dataset: time, the hottest point over it, each cut point's.
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* time = data.find(QStringLiteral("time"));
        const qucs_s::dataset::Variable* tmax = data.find(QStringLiteral("ht.Tmax"));
        QVERIFY(time && tmax && data.find(QStringLiteral("T_1")) && data.find(QStringLiteral("T_2")));
        QCOMPARE(time->size(), 31);
        QCOMPARE(tmax->dependencies, QStringList{QStringLiteral("time")});
        QVERIFY(std::abs(tmax->re.first() - 20) < 1e-9);   // (degC, as it asks)
        QVERIFY(tmax->re.last() > 40);
        // Along the board: x, and T at each time.
        app->gotoPage(doc->getDocName());
        const QString along = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Along the Board"), &why, false);
        QVERIFY2(!along.isEmpty(), qPrintable(why));
        QVERIFY(data.read(along.left(along.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* t = data.find(QStringLiteral("T"));
        QVERIFY(t != nullptr);
        QCOMPARE(t->dependencies, (QStringList{QStringLiteral("x"), QStringLiteral("time")}));
        QCOMPARE(t->size(), 200 * 31);
    }

    // A sweep: each width solved (meshed again), shown one at a time, its
    // Derived Values at each, Z0 against the width as a graph.
    void aSweepSolvesEachPoint()
    {
        MultiphysicsDoc* doc = open(copyOf("microstrip_sweep.qfem"));
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(300000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QCOMPARE(int(doc->solutions()->solutions.size()), 9);
        QCOMPARE(doc->instanceBox()->count(), 9);
        QVERIFY2(doc->plotScene()->title.contains("w = 5 mm"), qPrintable(doc->plotScene()->title));
        int z0 = 0;
        for (int r = 0; r < doc->table()->rowCount(); ++r)
            if (doc->table()->item(r, 1)->text().endsWith(QLatin1String(": Z0"))) ++z0;
        QCOMPARE(z0, 9);
        QString why;
        const QString display = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Z0 against the Width"), &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* w = data.find(QStringLiteral("w"));
        const qucs_s::dataset::Variable* z = data.find(QStringLiteral("Z0"));
        QVERIFY(w && z);
        QCOMPARE(w->size(), 9);
        QVERIFY(std::abs(w->re.first() - 1e-3) < 1e-12);
        QVERIFY(z->re.first() > z->re.last());
        shoot(app, "13-microstrip-sweep");
        // The sweep's settings: a parameter, its values, their unit.
        int at = -1;
        Model m = doc->model();
        const qucs_s::fem::Node* study = m.studies().front();
        QString sweepTag;
        for (const qucs_s::fem::Node& c : study->children)
            if (c.type == QLatin1String("sweep")) sweepTag = c.tag;
        Q_UNUSED(at);
        panel()->selectNode(sweepTag);
        auto* rows = panel()->settingsArea()->findChild<QTableWidget*>("mpRows");
        QVERIFY(rows != nullptr);
        QCOMPARE(rows->columnCount(), 3);
        QCOMPARE(rows->item(0, 1)->text(), QStringLiteral("range(1, 0.5, 5)"));
        shoot(app, "13b-sweep-settings");
    }

    // Solid mechanics: drawn where it has moved.
    void aSolidIsDrawnDeformed()
    {
        MultiphysicsDoc* doc = open(copyOf("bimetal_strip.qfem"));
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QVERIFY2(doc->plotScene()->title.contains("deformed"), qPrintable(doc->plotScene()->title));
        // The plot's points below where the strip was (it curls down).
        double low = 0;
        for (QPointF p : doc->plotScene()->values.points) low = std::min(low, p.y());
        QVERIFY2(low < -0.5, qPrintable(QString::number(low)));
        const double tip = doc->solution()->global(QStringLiteral("solid.dmax")).value_or(0);
        QVERIFY2(std::abs(tip - 5.84e-4) < 3e-5, qPrintable(QString::number(tip)));
        shoot(app, "14-bimetal");
    }

    // About an axis: the via's capacitance, round it whole.
    void anAxisymmetricModelSolves()
    {
        MultiphysicsDoc* doc = open(copyOf("tsv_axisymmetric.qfem"));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->model().axisymmetric());
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        const double c = doc->solution()->global(QStringLiteral("es.C11")).value_or(0);
        QVERIFY2(c > 110e-15 && c < 140e-15, qPrintable(QString::number(c)));
        shoot(app, "15-via");
        // The component's settings: its space, the thickness hidden.
        panel()->selectNode(doc->model().component().tag);
        QVERIFY(panel()->editorFor("space") != nullptr);
        QVERIFY(panel()->editorFor("thickness") == nullptr);
    }

    // A drawing imported, and read again when it changes.
    void anImportIsReadAgainWhenItsFileChanges()
    {
        QFile::remove(path("heatsink.svg"));
        QVERIFY(QFile::copy(examples() + "heatsink.svg", path("heatsink.svg")));
        QFile(path("heatsink.svg")).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        MultiphysicsDoc* doc = open(copyOf("heatsink_import.qfem"));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->geometry().ok());
        const double before = doc->geometry().topology->domains.front().area + doc->geometry().topology->domains.back().area;
        // Its settings: the file, with a button to choose one.
        panel()->selectNode(tagOf(doc->model(), "import", "heatsink"));
        QVERIFY(panel()->editorFor("file") != nullptr);
        QVERIFY(panel()->settingsArea()->findChild<QToolButton*>("mpBrowse_file") != nullptr);
        // Its fins half as tall.
        QFile f(path("heatsink.svg"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray svg = f.readAll();
        f.close();
        QString text = QString::fromUtf8(svg);
        text.replace(QRegularExpression(QStringLiteral("V 1(\\s)")), QStringLiteral("V 11\\1"));
        text.replace(QRegularExpression(QStringLiteral("(A 1 1 0 0 0 [0-9.]+) 1(\\s)")), QStringLiteral("\\1 11\\2"));
        svg = text.toUtf8();
        QCOMPARE(svg.count("V 11"), 6);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(svg);
        f.close();
        QTRY_VERIFY_WITH_TIMEOUT(!doc->geometryStale() && doc->geometry().topology
                                     && std::abs(doc->geometry().topology->domains.front().area + doc->geometry().topology->domains.back().area - before) > 50,
                                 10000);
        QVERIFY(doc->messages().join('\n').contains("heatsink.svg changed"));
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        shoot(app, "16-heatsink");
    }

    // Claude's tools: a study in time and a sweep - times, points, all of
    // them, graphs.
    void claudesToolsInTimeAndSweeps()
    {
        open(copyOf("heater_transient.qfem"));
        QJsonObject r = call("fem_solve");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject solved = json(r);
        QCOMPARE(solved.value("times").toObject().value("outputs").toInt(), 31);
        QJsonArray points;   // (appended: a braced list of one list is a copy to some compilers)
        points.append(QJsonArray{0, 0.8});
        r = call("fem_evaluate", {{"expression", "T"}, {"at", points}, {"all", true}, {"unit", "degC"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("values").toArray().size(), 31);
        r = call("fem_evaluate", {{"expression", "ht.Tmax"}, {"time", "10[s]"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("solutions").toString().contains("t = 10 s"), qPrintable(text(r)));
        r = call("fem_plot", {{"plot", tagOf(open(path("heater_transient.qfem"))->model(), "plotgroup", "Temperature")}, {"time", "20"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("solution").toString().contains("t = 20 s"), qPrintable(text(r)));
        r = call("fem_plot", {{"plot", tagOf(open(path("heater_transient.qfem"))->model(), "plotgroup1d", "Temperature in Time")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("display").toString().endsWith(".dpl"));
        // A sweep's points.
        open(copyOf("microstrip_sweep.qfem"));
        r = call("fem_solve", {{"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("sweep").toObject().value("solutions").toInt(), 9);
        r = call("fem_evaluate", {{"expression", "es.C11"}, {"point", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("solutions").toString().contains("w = 1 mm"));
        // The new kinds, described.
        r = call("fem_describe", {{"what", "format"}});
        QStringList types;
        for (const QJsonValue& k : json(r).value("kinds").toArray()) types << k.toObject().value("type").toString();
        for (const char* t : {"solid", "solid/load", "transient", "sweep", "import", "plotgroup1d", "linegraph", "heat/radiation"})
            QVERIFY2(types.contains(QString::fromLatin1(t)), t);
    }

    // ---- Phase 3: magnetic fields in frequency, lines into circuits, layouts

    // The coax's R, L, G, C: shown at each frequency, as graphs over it, as a
    // Touchstone file and as a Qucs part.
    void aLineOverFrequency()
    {
        MultiphysicsDoc* doc = open(coaxCopy());
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(300000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QVERIFY(doc->solution()->frequencyDomain);
        const int n = coaxFrequencies();
        QCOMPARE(doc->solution()->snapshotCount(), n);
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Results);
        QVERIFY2(doc->plotScene()->title.contains("f = 10 MHz"), qPrintable(doc->plotScene()->title));
        // Another frequency, chosen in the box: the plot group's "frequency".
        QCOMPARE(doc->instanceBox()->count(), n);
        const int at = doc->instanceBox()->findText(QStringLiteral("f = 100 kHz"));
        QVERIFY(at >= 0);
        doc->instanceBox()->setCurrentIndex(at);
        emit doc->instanceBox()->activated(at);
        QVERIFY2(doc->plotScene()->title.contains("f = 100 kHz"), qPrintable(doc->plotScene()->title));
        QVERIFY(std::abs(doc->model().find(doc->shownPlotGroup())->text("frequency").toDouble() - 1e5) < 1);
        shoot(app, "31-coax-current");
        doc->undo();
        QVERIFY(doc->plotScene()->title.contains("f = 10 MHz"));
        // The plot group's settings: its frequencies to choose from, no times.
        panel()->selectNode(doc->shownPlotGroup());
        auto* frequencies = qobject_cast<QComboBox*>(panel()->editorFor("frequency"));
        QVERIFY(frequencies != nullptr);
        QCOMPARE(frequencies->count(), n + 1);   // (the last, and each)
        QCOMPARE(qobject_cast<QComboBox*>(panel()->editorFor("time"))->count(), 1);
        // R over frequency: a dataset of "frequency".
        QString why;
        const QString display = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Resistance per metre"), &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* f = data.find(QStringLiteral("frequency"));
        const qucs_s::dataset::Variable* r = data.find(QStringLiteral("R"));
        QVERIFY(f && r);
        QCOMPARE(f->size(), n);
        QCOMPARE(r->dependencies, QStringList{QStringLiteral("frequency")});
        QVERIFY(r->re.last() > 5 * r->re.first());
        // Its Derived Values at each frequency: ten each.
        int rows = 0;
        for (int i = 0; i < doc->table()->rowCount(); ++i)
            if (doc->table()->item(i, 0)->text() == QLatin1String("Line Parameters")) ++rows;
        QCOMPARE(rows, 10 * n);
        // A metre of it as a Touchstone file, beside the model.
        const QString ts = doc->runExport(QStringLiteral("ts1"), &why);
        QVERIFY2(!ts.isEmpty(), qPrintable(why));
        QCOMPARE(QFileInfo(ts).fileName(), QStringLiteral("coax_rlgc_ts1.s2p"));
        QFile tsFile(ts);
        QVERIFY(tsFile.open(QIODevice::ReadOnly));
        const QString tsText = QString::fromUtf8(tsFile.readAll());
        QVERIFY(tsText.contains(QStringLiteral("# Hz S RI R 50")));
        // As a Qucs subcircuit, which Qucs reads: an RLCG part between ports.
        const QString sch = doc->runExport(QStringLiteral("qp1"), &why);
        QVERIFY2(!sch.isEmpty(), qPrintable(why));
        // (Named as the subcircuit is; its dataset and display as it is, so
        // Qucs opens it without asking.)
        QCOMPARE(QFileInfo(sch).fileName(), QStringLiteral("coax_1m.sch"));
        QFile schFile(sch);
        QVERIFY(schFile.open(QIODevice::ReadOnly));
        const QString schText = QString::fromUtf8(schFile.readAll());
        QVERIFY(schText.contains(QStringLiteral("<DataSet=coax_1m.dat>")) && schText.contains(QStringLiteral("<DataDisplay=coax_1m.dpl>")));
        QVERIFY(app->gotoPage(sch));
        auto* part = qobject_cast<Schematic*>(app->DocumentTab->currentWidget());
        QVERIFY(part != nullptr);
        QStringList models;
        for (Component* c : part->a_DocComps) models << c->Model;
        QCOMPARE(models.count(QStringLiteral("RLCG")), 1);
        QCOMPARE(models.count(QStringLiteral("Port")), 2);
        // Copied, for a paste.
        QVERIFY2(doc->copyPart(QStringLiteral("qp1"), &why), qPrintable(why));
        QVERIFY(QApplication::clipboard()->text().startsWith(QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">")));
        // The export's settings: its line among the Line Parameters; its buttons.
        app->gotoPage(doc->getDocName());
        panel()->selectNode(QStringLiteral("ts1"));
        auto* line = qobject_cast<QComboBox*>(panel()->editorFor("line"));
        QVERIFY(line != nullptr);
        QVERIFY(line->findData(QStringLiteral("lp1")) >= 0);
        QVERIFY(panel()->findChild<QPushButton*>(QStringLiteral("mpExportButton")) != nullptr);
        panel()->selectNode(QStringLiteral("qp1"));
        QVERIFY(panel()->findChild<QPushButton*>(QStringLiteral("mpCopyPartButton")) != nullptr);
    }

    // A layout cut across along a ruler: a model of its layer stack, its
    // materials from the library.
    void aLayoutsCrossSection()
    {
        const QString gds = copyOf("thermo_optic_heater.gds");
        QVERIFY(app->gotoPage(gds));
        auto* layout = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(layout != nullptr);
        QVERIFY(layout->waitForLoaded());
        const QLineF cut(-20, 0, 20, 0);
        // The stack a cut suggests: the layers under it (the pads are not).
        const LayoutShapes shapes = readLayoutShapes(gds);
        const QList<StackRow> suggested = suggestedStack(shapes, cut);
        QStringList names;
        for (const StackRow& r : suggested) names << r.name;
        QVERIFY2(names.contains("layer_1_0") && names.contains("layer_11_0") && !names.contains("layer_12_0"), qPrintable(names.join(' ')));
        const QList<StackRow> rows = {{QStringLiteral("box"), QString(), -2, 2, QStringLiteral("Silicon dioxide")},
                                      {QStringLiteral("cladding"), QString(), 0, 3, QStringLiteral("Silicon dioxide")},
                                      {QStringLiteral("wg"), QStringLiteral("1/0"), 0, 0.22, QStringLiteral("Silicon")},
                                      {QStringLiteral("heater"), QStringLiteral("11/0"), 2, 0.12, QString()}};
        QString why;
        MultiphysicsDoc* doc = layout->crossSection(cut, &rows, &why);
        QVERIFY2(doc != nullptr, qPrintable(why));
        QCOMPARE(app->DocumentTab->currentWidget(), doc);
        const Model& m = doc->model();
        QCOMPARE(m.component().text("unit"), QStringLiteral("um"));
        const qucs_s::fem::Node& xs = m.geometry().children.front();
        QCOMPARE(xs.type, QStringLiteral("crosssection"));
        QCOMPARE(xs.text("file"), QFileInfo(gds).absoluteFilePath());
        QCOMPARE(xs.text("cell"), QStringLiteral("PHASE_SHIFTER"));
        // Two materials, each on its rows.
        QCOMPARE(int(m.materials().children.size()), 2);
        doc->showGeometry();
        QTRY_VERIFY(doc->view()->topology() != nullptr);
        const QStringList objects = doc->view()->topology()->objectNames;
        for (const char* o : {"box", "cladding", "wg", "heater"}) QVERIFY2(objects.contains(QString::fromLatin1(o)), o);
        shoot(app, "32-cross-section");
    }

    // Claude's tools in frequency, and fem_export.
    void claudesToolsInFrequency()
    {
        open(coaxCopy());
        QJsonObject r = call("fem_solve", {{"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("frequencies").toObject().value("outputs").toInt(), coaxFrequencies());
        r = call("fem_evaluate", {{"expression", "mf.R_1_1"}, {"frequency", "100[kHz]"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("solutions").toString().contains("f = 100 kHz"), qPrintable(text(r)));
        r = call("fem_plot", {{"plot", tagOf(open(path("coax_rlgc.qfem"))->model(), "plotgroup", "Current Density")}, {"frequency", "1e5"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("solution").toString().contains("f = 100 kHz"), qPrintable(text(r)));
        r = call("fem_export", {{"export", "ts1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("written").toString().endsWith(".s2p"));
        QVERIFY(json(r).value("begins").toString().contains("# Hz S RI R 50"));
        r = call("fem_export", {{"export", "qp1"}, {"copy", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_export");
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("ts1"), qPrintable(text(r)));
        // The new kinds, described.
        r = call("fem_describe", {{"what", "format"}});
        QStringList types;
        for (const QJsonValue& k : json(r).value("kinds").toArray()) types << k.toObject().value("type").toString();
        for (const char* t : {"magnetic", "magnetic/coil", "magnetic/magnet", "frequency", "crosssection", "touchstone", "spice", "qucspart"})
            QVERIFY2(types.contains(QString::fromLatin1(t)), t);
        QVERIFY(json(r).value("format").toString().contains("Magnetic fields"));
    }

    // A guide's modes at two frequencies: each in the box, the plot group's
    // mode and frequency; its default plots; the modes as a dataset.
    void aWaveguidesModes()
    {
        const QString file = path(QStringLiteral("guide_modes.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "label": "guide", "size": ["22.86", "10.16"]}, {"type": "form union"}],
            "materials": [{"material": "Air", "selection": {"all": true}}],
            "physics": [{"type": "modes", "tag": "emw", "order": "1"}],
            "mesh": {"size": "normal"}}],
          "studies": [{"type": "study", "label": "Study 1", "steps": [{"type": "mode", "frequency": "15[GHz], 20[GHz]", "modes": 2, "neff": "1"}]}]})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QVERIFY(doc->solution()->modeAnalysis);
        QCOMPARE(doc->solution()->snapshotCount(), 4);
        // Its default plots: the field, the power, the modes.
        const Model& m = doc->model();
        for (const char* label : {"Electric Field (emw)", "Power Flow (emw)"})
            QVERIFY2(!tagOf(m, "plotgroup", label).isEmpty(), label);
        QVERIFY(!tagOf(m, "plotgroup1d", "The Modes").isEmpty());
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Results);
        QVERIFY2(doc->plotScene()->title.contains("f = 20 GHz, mode 1"), qPrintable(doc->plotScene()->title));
        // Each mode in the box; another chosen: the plot group's.
        QCOMPARE(doc->instanceBox()->count(), 4);
        const int at = doc->instanceBox()->findText(QStringLiteral("f = 15 GHz, mode 2"));
        QVERIFY(at >= 0);
        doc->instanceBox()->setCurrentIndex(at);
        emit doc->instanceBox()->activated(at);
        QVERIFY2(doc->plotScene()->title.contains("f = 15 GHz, mode 2"), qPrintable(doc->plotScene()->title));
        const qucs_s::fem::Node* shown = doc->model().find(doc->shownPlotGroup());
        QCOMPARE(shown->integer("mode"), 2);
        QVERIFY(std::abs(shown->text("frequency").toDouble() - 15e9) < 1);
        shoot(app, "33-guide-mode");
        // TE20 at 20 GHz: its index, as the box shows it.
        QString why;
        const QString display = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "The Modes"), &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* mode = data.find(QStringLiteral("mode"));
        const qucs_s::dataset::Variable* frequency = data.find(QStringLiteral("frequency"));
        const qucs_s::dataset::Variable* neff = data.find(QStringLiteral("emw.neff"));
        QVERIFY(mode && frequency && neff);
        QCOMPARE(mode->size(), 2);
        QCOMPARE(frequency->size(), 2);
        QCOMPARE(neff->dependencies, (QStringList{QStringLiteral("mode"), QStringLiteral("frequency")}));
        const double te20 = std::sqrt(1 - std::pow(299792458.0 / (22.86e-3 * 20e9), 2));
        QVERIFY2(std::abs(neff->re.at(3) - te20) < 2e-3, qPrintable(QString::number(neff->re.at(3))));
        // A wavelength for the plot's frequency.
        panel()->selectNode(doc->shownPlotGroup());
        QVERIFY(qobject_cast<QComboBox*>(panel()->editorFor("frequency")) != nullptr);
        QVERIFY(panel()->editorFor("mode") != nullptr);
    }

    // A guide's one mode swept over its width: a global graph over the width
    // (the one mode last - a data display draws over the first), named as
    // asked; a study added and deleted leaves the others' solutions as they
    // were, one changed is old alone.
    void graphsOverASweepOfModes()
    {
        const QString file = path(QStringLiteral("guide_sweep.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "a", "expression": "22.86[mm]"}],
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "label": "guide", "size": ["a/1[mm]", "10.16"]}, {"type": "form union"}],
            "materials": [{"material": "Air", "selection": {"all": true}}],
            "physics": [{"type": "modes", "tag": "emw", "order": "1"}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "tag": "std1", "label": "Over the width",
                       "steps": [{"type": "sweep", "parameters": [["a", "20, 22.86, 25", "mm"]]},
                                 {"type": "mode", "frequency": "20[GHz]", "modes": 1, "neff": "1"}]},
                      {"type": "study", "tag": "std2", "label": "At 15 GHz", "steps": [{"type": "mode", "frequency": "15[GHz]", "modes": 1, "neff": "1"}]}],
          "results": {"plots": [{"type": "plotgroup1d", "tag": "pgw", "label": "Index against Width", "study": "std1",
                                 "plots": [{"type": "globalgraph", "expression": "neff_x100 = emw.neff*100; emw.neff"}]}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        for (const char* study : {"std1", "std2"}) {
            doc->compute(QString::fromLatin1(study));
            QVERIFY(doc->waitForJob(120000));
            QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        }
        QString why;
        const QString display = doc->showGraphs(QStringLiteral("pgw"), &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* named = data.find(QStringLiteral("neff_x100"));
        const qucs_s::dataset::Variable* neff = data.find(QStringLiteral("emw.neff"));
        QVERIFY(named && neff);
        QCOMPARE(named->dependencies, (QStringList{QStringLiteral("a"), QStringLiteral("mode")}));
        QCOMPARE(neff->dependencies, (QStringList{QStringLiteral("a"), QStringLiteral("mode")}));
        QCOMPARE(neff->size(), 3);
        for (int i = 0; i < 3; ++i) {
            const double a = std::vector<double>{20e-3, 22.86e-3, 25e-3}[std::size_t(i)];
            const double te10 = std::sqrt(1 - std::pow(299792458.0 / (2 * a * 20e9), 2));
            QVERIFY2(std::abs(neff->re.at(i) - te10) < 3e-3, qPrintable(QString::number(neff->re.at(i))));
            QVERIFY(std::abs(named->re.at(i) - 100 * neff->re.at(i)) < 1e-9);
        }
        // Named in Claude's fem_evaluate too: a global's, at a point.
        QJsonObject r = call("fem_evaluate", {{"study", "std2"}, {"expression", "twice = 2*emw.neff; emw.neff"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray values = json(r).value("values").toArray();
        QCOMPARE(values.size(), 2);
        QCOMPARE(values.at(0).toObject().value("name").toString(), QStringLiteral("twice"));
        QVERIFY(std::abs(values.at(0).toObject().value("SI").toDouble() - 2 * values.at(1).toObject().value("SI").toDouble()) < 1e-12);
        // (A comparison is no name: ==, >=.)
        r = call("fem_evaluate", {{"study", "std2"}, {"expression", "same = emw.neff == emw.neff; a == a; emw.neff >= 0"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray compared = json(r).value("values").toArray();
        QCOMPARE(compared.size(), 3);
        int k = 0;
        for (const char* name : {"same", "a == a", "emw.neff >= 0"}) {
            QCOMPARE(compared.at(k).toObject().value("name").toString(), QString::fromLatin1(name));
            QCOMPARE(compared.at(k++).toObject().value("SI").toDouble(), 1.0);
        }
        QJsonArray centre;
        centre.append(QJsonArray{11.43, 5.08});
        r = call("fem_evaluate", {{"study", "std2"}, {"expression", "E = emw.normE"}, {"at", centre}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("values").toArray().at(0).toObject().value("name").toString(), QStringLiteral("E"));
        // A study added and deleted: no solution old.
        QVERIFY(!doc->solutionStale(QStringLiteral("std1")) && !doc->solutionStale(QStringLiteral("std2")));
        Model m = doc->model();
        const QString parent = m.parentOf(QStringLiteral("std1"))->tag;
        qucs_s::fem::Node added = m.make(QStringLiteral("study"));
        added.tag = QStringLiteral("std9");
        QVERIFY(m.add(parent, added) != nullptr);
        QVERIFY(doc->setModel(m, QStringLiteral("a study added")));
        QVERIFY(m.remove(QStringLiteral("std9")));
        QVERIFY(doc->setModel(m, QStringLiteral("deleted")));
        QVERIFY(!doc->solutionStale(QStringLiteral("std1")));
        QVERIFY(!doc->solutionStale(QStringLiteral("std2")));
        r = call("fem_evaluate", {{"study", "std2"}, {"expression", "emw.neff"}});
        QVERIFY2(!json(r).contains("note"), qPrintable(text(r)));
        // A study changed: its solution old, the other's not; renamed: neither.
        m.find(QStringLiteral("std2"))->children.front().props.insert(QStringLiteral("frequency"), QStringLiteral("16[GHz]"));
        m.find(QStringLiteral("std1"))->label = QStringLiteral("Over a");
        QVERIFY(doc->setModel(m, QStringLiteral("changed")));
        QVERIFY(doc->solutionStale(QStringLiteral("std2")));
        QVERIFY(!doc->solutionStale(QStringLiteral("std1")));
        r = call("fem_evaluate", {{"study", "std2"}, {"expression", "emw.neff"}});
        QVERIFY2(json(r).value("note").toString().contains("changed"), qPrintable(text(r)));
    }

    // The iris filter: its S-parameters over frequency, a Touchstone file of
    // its ports.
    void aFiltersSMatrix()
    {
        const QString file = copyOf(QStringLiteral("iris_filter.qfem"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString text = QString::fromUtf8(f.readAll());
            f.close();
            // (A slow build: three frequencies, on a coarser mesh.)
            text.replace(QStringLiteral("linspace(8[GHz], 12[GHz], 41)"),
                         slowBuild() ? QStringLiteral("linspace(8[GHz], 12[GHz], 3)") : QStringLiteral("linspace(8[GHz], 12[GHz], 5)"));
            if (slowBuild()) text.replace(QStringLiteral("\"hmax\": \"1.2\""), QStringLiteral("\"hmax\": \"2.5\""));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(text.toUtf8());
        }
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(300000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        const int count = slowBuild() ? 3 : 5;
        QCOMPARE(doc->solution()->snapshotCount(), count);
        QVERIFY2(doc->plotScene()->title.contains("f = 10 GHz"), qPrintable(doc->plotScene()->title));
        shoot(app, "34-iris-filter");
        QString why;
        const QString display = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "S-parameters"), &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* s21 = data.find(QStringLiteral("ewfd.S21dB"));
        QVERIFY(s21 && data.find(QStringLiteral("ewfd.S11dB")));
        QCOMPARE(s21->dependencies, QStringList{QStringLiteral("frequency")});
        QVERIFY2(s21->re.at(count / 2) > -1 && s21->re.at(0) < -6, qPrintable(QStringLiteral("%1 %2").arg(s21->re.at(count / 2)).arg(s21->re.at(0))));
        const QString ts = doc->runExport(QStringLiteral("ts1"), &why);
        QVERIFY2(!ts.isEmpty(), qPrintable(why));
        QCOMPARE(QFileInfo(ts).fileName(), QStringLiteral("iris_filter_ts1.s2p"));
        QFile tsFile(ts);
        QVERIFY(tsFile.open(QIODevice::ReadOnly));
        const QString tsText = QString::fromUtf8(tsFile.readAll());
        QVERIFY(tsText.contains(QStringLiteral("# Hz S RI R 50")));
        QCOMPARE(tsText.count(QRegularExpression(QStringLiteral("^[0-9]"), QRegularExpression::MultilineOption)), count);
        // Its settings: of the ports, of this physics.
        panel()->selectNode(QStringLiteral("ts1"));
        auto* of = qobject_cast<QComboBox*>(panel()->editorFor("of"));
        QVERIFY(of != nullptr);
        QCOMPARE(of->currentData().toString(), QStringLiteral("ports"));
        QVERIFY(panel()->editorFor("physics") != nullptr);
    }

    // Claude's tools: the modes solved, a mode evaluated and plotted.
    void claudesToolsForModes()
    {
        open(copyOf(QStringLiteral("wr90_modes.qfem")));
        QJsonObject r = call("fem_solve", {{"timeout", 120}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray modes = json(r).value("modes").toArray();
        QCOMPARE(modes.size(), 5);
        QCOMPARE(modes.at(0).toObject().value("mode").toInt(), 1);
        QVERIFY(modes.at(0).toObject().contains("emw.neff"));
        r = call("fem_evaluate", {{"expression", "emw.neff"}, {"mode", 2}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("solutions").toString().contains("mode 2"), qPrintable(text(r)));
        QVERIFY(std::abs(json(r).value("values").toArray().at(0).toObject().value("SI").toDouble() - 0.75501) < 1e-3);
        r = call("fem_plot", {{"plot", tagOf(open(path("wr90_modes.qfem"))->model(), "plotgroup", "Magnetic Field")}, {"mode", 3}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("solution").toString().contains("mode 3"), qPrintable(text(r)));
        r = call("fem_describe", {{"what", "format"}});
        QStringList types;
        for (const QJsonValue& k : json(r).value("kinds").toArray()) types << k.toObject().value("type").toString();
        for (const char* t : {"modes", "modes/wave", "modes/pml", "waves", "waves/port", "waves/scattering", "mode"})
            QVERIFY2(types.contains(QString::fromLatin1(t)), t);
    }

    // A diode's sweep in the window: its carriers drawn, its I-V a dataset.
    void aDiodeSwept()
    {
        const QString file = copyOf(QStringLiteral("pn_diode.qfem"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString text = QString::fromUtf8(f.readAll());
            f.close();
            text.replace(QStringLiteral("-2, -1, -0.5, 0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7"), QStringLiteral("-1, 0.3, 0.5"));
            // (A slow build: a coarser junction.)
            if (slowBuild()) text.replace(QStringLiteral("\"hmax\": \"0.02\""), QStringLiteral("\"hmax\": \"0.06\""));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(text.toUtf8());
        }
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(300000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Results);
        QVERIFY2(doc->plotScene()->surface, qPrintable(doc->plotScene()->problems.join(QLatin1String("; "))));
        QVERIFY(doc->plotScene()->problems.isEmpty());
        shoot(app, "35-diode-electrons");
        QString why;
        const QString display = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Current against Voltage"), &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* va = data.find(QStringLiteral("Va"));
        const qucs_s::dataset::Variable* ia = data.find(QStringLiteral("semi.I_a"));
        QVERIFY(va && ia);
        QCOMPARE(va->size(), 3);
        QVERIFY(ia->re.at(2) > 100 * ia->re.at(1) && ia->re.at(1) > 0 && ia->re.at(0) < 0);
        // Over Va, not the frequency step's one frequency: a data display
        // draws over a variable's first dependency.
        QCOMPARE(ia->dependencies, (QStringList{QStringLiteral("Va"), QStringLiteral("frequency")}));
        const QString cv = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Capacitance against Voltage"), &why, false);
        QVERIFY2(!cv.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset cvData;
        QVERIFY(cvData.read(cv.left(cv.size() - 4) + QStringLiteral(".dat"), &why));
        QVERIFY(cvData.find(QStringLiteral("semi.Ckk")));
        QCOMPARE(cvData.find(QStringLiteral("semi.Ckk"))->dependencies, (QStringList{QStringLiteral("Va"), QStringLiteral("frequency")}));
        // The bands along the diode: a dataset over the cut line.
        const QString bands = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Bands"), &why, false);
        QVERIFY2(!bands.isEmpty(), qPrintable(why));
    }

    // Rays in the window: each drawn in its wavelength's color; a model of
    // rays alone gets the trajectories as its default plot.
    void raysDrawn()
    {
        MultiphysicsDoc* doc = open(copyOf(QStringLiteral("prism_dispersion.qfem")));
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Results);
        QCOMPARE(int(doc->plotScene()->rays.size()), 1);
        const PlotScene::Rays& rays = doc->plotScene()->rays.front();
        QVERIFY(rays.paths.size() >= 21);
        QCOMPARE(rays.by, QStringLiteral("wavelength"));
        shoot(app, "36-prism");
        const QString file = path(QStringLiteral("bare_rays.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm",
            "geometry": [{"type": "rectangle", "label": "air", "position": ["-10", "-10"], "size": ["40", "20"]},
                         {"type": "circle", "label": "ball", "center": ["10", "0"], "radius": "5"}, {"type": "form union"}],
            "materials": [{"material": "Air", "selection": {"all": true}}, {"material": "Glass (BK7)", "selection": {"only": ["ball"]}}],
            "physics": [{"type": "rays", "tag": "gop", "features": [
              {"type": "beam", "start": ["-5", "-4"], "end": ["-5", "4"], "angle": "0", "count": 9}]}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "label": "Study 1", "steps": [{"type": "stationary"}]}]})json");
        f.close();
        MultiphysicsDoc* bare = open(file);
        QVERIFY(bare != nullptr);
        bare->compute();
        QVERIFY(bare->waitForJob(120000));
        QVERIFY2(bare->lastError().isEmpty(), qPrintable(bare->lastError()));
        QVERIFY(!tagOf(bare->model(), "plotgroup", "Ray Trajectories (gop)").isEmpty());
        QVERIFY(!bare->plotScene()->rays.empty());
        shoot(app, "37-ball-lens");
    }

    // Claude's tools: a photodiode solved, its photocurrent read; the kinds described.
    void claudesToolsForDevicesAndRays()
    {
        const QString file = copyOf(QStringLiteral("photodiode.qfem"));
        if (slowBuild()) {
            // (A slow build: a coarser mesh.)
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString text = QString::fromUtf8(f.readAll());
            f.close();
            text.replace(QStringLiteral("\"hmax\": \"0.8\""), QStringLiteral("\"hmax\": \"2\""));
            text.replace(QStringLiteral("\"hmax\": \"0.1\""), QStringLiteral("\"hmax\": \"0.3\""));
            text.replace(QStringLiteral("\"hmax\": \"0.15\""), QStringLiteral("\"hmax\": \"0.3\""));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(text.toUtf8());
        }
        open(file);
        QJsonObject r = call("fem_solve", {{"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_evaluate", {{"expression", "semi.I_a"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("values").toArray().at(0).toObject().value("SI").toDouble() < -1e-4);
        r = call("fem_evaluate", {{"expression", "gop.Pabs"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_describe", {{"what", "format"}});
        QStringList types;
        for (const QJsonValue& k : json(r).value("kinds").toArray()) types << k.toObject().value("type").toString();
        for (const char* t : {"semiconductor", "semiconductor/ohmic", "semiconductor/doping", "rays", "rays/beam", "rays/detector", "rayplot"})
            QVERIFY2(types.contains(QString::fromLatin1(t)), t);
        QVERIFY(json(r).value("format").toString().contains("Semiconductors"));
    }

    // Quantum wells in the window: a well's states drawn on its band
    // diagram (a dataset along the growth axis, a curve for each state), its
    // absorption over the photon's energy; a bare model's default plots.
    void quantumWells()
    {
        MultiphysicsDoc* doc = open(copyOf(QStringLiteral("gaas_quantum_well.qfem")));
        QVERIFY(doc != nullptr);
        doc->compute(QStringLiteral("std1"));
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QCOMPARE(doc->view()->show(), GraphicsView::Show::Results);
        QString why;
        const QString display = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Band Diagram"), &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* state = data.find(QStringLiteral("state"));
        const qucs_s::dataset::Variable* wave = data.find(QStringLiteral("qw.wave_e"));
        QStringList names;
        for (const qucs_s::dataset::Variable& v : data.variables()) names << v.name;
        QVERIFY2(state && wave, qPrintable(names.join(QLatin1String(", "))));
        QCOMPARE(state->size(), 4);
        QCOMPARE(wave->size(), 4 * 1000);
        // (Each state's curve its level with |ψ|² on it, in eV.)
        double least = 1e300;
        for (double v : wave->re)
            if (std::isfinite(v)) least = std::min(least, v);
        QVERIFY(least > 0.5 && least < 1.0);
        QVERIFY(doc->showPlotGroup(tagOf(doc->model(), "plotgroup", "Electron Probability")));
        QVERIFY(doc->plotScene()->surface);
        QVERIFY(doc->plotScene()->problems.isEmpty());
        shoot(app, "38-quantum-well");
        doc->compute(QStringLiteral("std2"));
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        const QString spectrum = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Absorption"), &why, false);
        QVERIFY2(!spectrum.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset absorption;
        QVERIFY(absorption.read(spectrum.left(spectrum.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* energy = absorption.find(QStringLiteral("photon_energy"));
        const qucs_s::dataset::Variable* te = absorption.find(QStringLiteral("qw.alphaTE"));
        QVERIFY(energy && te);
        QCOMPARE(energy->size(), 201);
        QVERIFY(std::abs(energy->re.front() - 1.45) < 1e-9 && std::abs(energy->re.back() - 1.85) < 1e-9);
        QVERIFY(*std::max_element(te->re.begin(), te->re.end()) > 3000);   // (per cm)
        // A model of a well alone: its default plots - the probability, the
        // band diagram along the growth axis, the states' energies.
        const QString file = path(QStringLiteral("bare_well.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "label": "barrier", "size": ["10", "30"]},
                         {"type": "rectangle", "label": "well", "position": ["0", "10"], "size": ["10", "10"]}, {"type": "form union"}],
            "materials": [{"material": "Aluminium gallium arsenide", "selection": {"all": true}},
                          {"material": "Gallium arsenide", "selection": {"only": ["well"]}}],
            "physics": [{"type": "quantum", "tag": "qw"}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "label": "Study 1", "steps": [{"type": "states", "states": 3}]}]})json");
        f.close();
        MultiphysicsDoc* bare = open(file);
        QVERIFY(bare != nullptr);
        bare->compute();
        QVERIFY(bare->waitForJob(120000));
        QVERIFY2(bare->lastError().isEmpty(), qPrintable(bare->lastError()));
        QVERIFY(!tagOf(bare->model(), "plotgroup", "Electron Probability (qw)").isEmpty());
        const QString diagram = tagOf(bare->model(), "plotgroup1d", "Band Diagram (qw)");
        QVERIFY(!diagram.isEmpty());
        QVERIFY(!tagOf(bare->model(), "plotgroup1d", "The States").isEmpty());
        QVERIFY2(!bare->showGraphs(diagram, &why, false).isEmpty(), qPrintable(why));
    }

    // Claude's tools: a well's states listed, a transition read; the kinds described.
    void claudesToolsForQuantumWells()
    {
        open(copyOf(QStringLiteral("gaas_quantum_well.qfem")));
        QJsonObject r = call("fem_solve", {{"timeout", 120}, {"study", "std1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray states = json(r).value("states").toArray();
        QCOMPARE(states.size(), 4);
        QVERIFY(states.at(0).toObject().value("qw.E_e").toString().endsWith(QLatin1String(" eV")));
        r = call("fem_evaluate", {{"expression", "qw.Ee1hh1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const double e = json(r).value("values").toArray().at(0).toObject().value("SI").toDouble() / 1.602176634e-19;
        QVERIFY2(e > 1.44 && e < 1.48, qPrintable(QString::number(e)));
        r = call("fem_solve", {{"timeout", 120}, {"study", "std2"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("spectrum").toObject().value("outputs").toInt(), 201);
        QVERIFY(json(r).value("spectrum").toObject().value("from").toString().startsWith(QLatin1String("E = 1.45")));
        r = call("fem_describe", {{"what", "format"}});
        QStringList types;
        for (const QJsonValue& k : json(r).value("kinds").toArray()) types << k.toObject().value("type").toString();
        for (const char* t : {"quantum", "quantum/material", "quantum/flux", "states", "spectrum"}) QVERIFY2(types.contains(QString::fromLatin1(t)), t);
        QVERIFY(json(r).value("format").toString().contains("Quantum wells"));
    }

    // A PDE of one's own in the window: a 1D ODE's default plot a line graph
    // along its line (a dataset), a 2D one's a color map, an eigenproblem's
    // eigenvalues a graph over the modes.
    void aPdeOfOnesOwn()
    {
        const QString file = path(QStringLiteral("own_ode.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "m",
            "geometry": [{"type": "rectangle", "label": "bar", "size": ["1", "0.1"]}],
            "physics": [{"type": "pde", "tag": "c", "space": "1d", "features": [
              {"type": "coefficient", "f": "exp(u)", "selection": {"all": true}},
              {"type": "dirichlet", "r": "0", "selection": {"of": ["bar"], "side": "left"}},
              {"type": "dirichlet", "r": "0", "selection": {"of": ["bar"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.05"}}],
          "studies": [{"type": "study", "label": "Study 1", "steps": [{"type": "stationary"}]}]})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(120000));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        const QString solution = tagOf(doc->model(), "plotgroup1d", "Solution (c)");
        QVERIFY(!solution.isEmpty());
        QString why;
        const QString display = doc->showGraphs(solution, &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* u = data.find(QStringLiteral("c.u"));
        QVERIFY(u && u->size() == 500);
        QVERIFY(std::abs(*std::max_element(u->re.begin(), u->re.end()) - 0.1405392) < 1e-5);
        // The drum: a color map of its first mode, its eigenvalues over the modes.
        MultiphysicsDoc* drum = open(copyOf(QStringLiteral("drum_modes.qfem")));
        QVERIFY(drum != nullptr);
        drum->compute();
        QVERIFY(drum->waitForJob(120000));
        QVERIFY2(drum->lastError().isEmpty(), qPrintable(drum->lastError()));
        QCOMPARE(drum->view()->show(), GraphicsView::Show::Results);
        QVERIFY(drum->plotScene()->surface);
        shoot(app, "41-drum-mode");
        const QString eig = drum->showGraphs(tagOf(drum->model(), "plotgroup1d", "Eigenvalues"), &why, false);
        QVERIFY2(!eig.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset lambdas;
        QVERIFY(lambdas.read(eig.left(eig.size() - 4) + QStringLiteral(".dat"), &why));
        const qucs_s::dataset::Variable* l = lambdas.find(QStringLiteral("c.lambda"));
        QVERIFY(l && l->size() == 6 && std::abs(l->re.front() - 5.7832) < 1e-3);
    }

    // Claude's tools: a drum's eigenvalues listed; the kinds described.
    void claudesToolsForPdes()
    {
        open(copyOf(QStringLiteral("drum_modes.qfem")));
        QJsonObject r = call("fem_solve", {{"timeout", 120}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray modes = json(r).value("modes").toArray();
        QCOMPARE(modes.size(), 6);
        QVERIFY(modes.at(0).toObject().value("c.lambda").toString().startsWith(QLatin1String("5.78")));
        r = call("fem_describe", {{"what", "format"}});
        QStringList types;
        for (const QJsonValue& k : json(r).value("kinds").toArray()) types << k.toObject().value("type").toString();
        for (const char* t : {"pde", "pde/coefficient", "pde/general", "pde/dirichlet", "pde/flux", "eigenvalue"}) QVERIFY2(types.contains(QString::fromLatin1(t)), t);
        QVERIFY(json(r).value("format").toString().contains("Your own PDE"));
    }

    // The pn-junction paper check's notes (2026-10-11): fem_solve says a
    // Newton's method that did not converge; a global derived value without
    // a study finds the study that has it; fem_evaluate splits ';' at points
    // and, without a study, takes one that is solved; emw.loss in dB/m; an
    // integral of a function without a unit says so.
    void aPaperChecksNotes()
    {
        const QString file = path(QStringLiteral("paper_check.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "functions": [{"type": "analytic", "name": "fq", "arguments": "T", "argunits": "K", "expression": "T/293.15[K]"}],
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "oxide", "position": ["-1", "-1.5"], "size": ["2.4", "3.22"]},
                         {"type": "rectangle", "name": "core", "size": ["0.4", "0.22"]}],
            "materials": [{"label": "oxide", "n": "1.444", "k": "1.4[W/(m*K)]*(T/293.15[K])^3", "selection": {"only": ["oxide"]}},
                          {"label": "silicon", "n": "3.476", "kappa": "1e-4", "dndT": "1.86e-4[1/K]", "k": "1.4[W/(m*K)]*(T/293.15[K])^3", "selection": {"only": ["core"]}}],
            "physics": [{"type": "heat", "tag": "ht", "features": [
                           {"type": "source", "Q0": "3e15[W/m^3]", "selection": {"only": ["core"]}},
                           {"type": "temperature", "T0": "293.15[K]", "selection": {"exterior": true}}]},
                        {"type": "modes", "tag": "emw", "features": [
                           {"type": "wave", "thermooptic": true, "T": "ht.T", "selection": {"only": ["core"]}}]}],
            "mesh": {"size": "normal", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.05"}]}}],
          "studies": [{"type": "study", "tag": "std1", "label": "Heat", "steps": [{"type": "stationary", "physics": ["ht"], "maxiter": 1}]},
                      {"type": "study", "tag": "std2", "label": "Modes", "steps": [{"type": "mode", "physics": ["ht", "emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3"}]}],
          "results": {"derived": [
            {"type": "global", "tag": "gev1", "label": "neff", "expression": "emw.neff"},
            {"type": "integral", "tag": "int1", "label": "fq", "study": "std1", "level": "domain", "selection": {"only": ["core"]}, "expression": "fq(T)"}]}})json");
        f.close();
        open(file);
        // The modes first: their loss in dB/m; fem_evaluate without a study the modes' (the heat's unsolved).
        QJsonObject r = call("fem_solve", {{"timeout", 120}, {"study", "std2"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("modes").toArray().at(0).toObject().value("emw.loss").toString().endsWith(QLatin1String(" dB/m")));
        QVERIFY(json(r).value("globals").toObject().value("emw.loss").toString().endsWith(QLatin1String(" dB/m")));
        QVERIFY(!json(r).contains(QLatin1String("warnings")));
        r = call("fem_evaluate", {{"expression", "emw.neff"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("values").toArray().at(0).toObject().value("SI").toDouble() > 2);
        // The heat in one Newton step: said among fem_solve's warnings.
        r = call("fem_solve", {{"timeout", 120}, {"study", "std1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(text(r).contains(QLatin1String("did not converge")), qPrintable(text(r)));
        QVERIFY(json(r).value("warnings").toArray().size() >= 1);
        // The derived values: the global of the modes' study (the first has none), the integral's note.
        QJsonObject neff, integral;
        for (const QJsonValue& d : json(r).value("derived values").toArray())
            (d.toObject().value("tag").toString() == QLatin1String("gev1") ? neff : integral) = d.toObject();
        QVERIFY2(!neff.contains(QLatin1String("error")) && neff.value("note").toString().contains(QLatin1String("Modes")),
                 qPrintable(QString::fromUtf8(QJsonDocument(neff).toJson())));
        QVERIFY2(integral.value("note").toString().contains(QLatin1String("fq has no unit")), qPrintable(QString::fromUtf8(QJsonDocument(integral).toJson())));
        // fem_evaluate: two expressions at two points; with no study, the first's (it has T).
        r = call("fem_evaluate", {{"expression", "T; 2*T"}, {"at", QJsonArray{QJsonArray{0.2, 0.1}, QJsonArray{0.9, 1.0}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray values = json(r).value("values").toArray();
        QCOMPARE(values.size(), 4);
        QCOMPARE(values.at(0).toObject().value("name").toString(), QStringLiteral("T"));
        QVERIFY(std::abs(values.at(1).toObject().value("SI").toDouble() - 2 * values.at(0).toObject().value("SI").toDouble()) < 1e-9);
        // An expression no study has: the first study named.
        r = call("fem_evaluate", {{"expression", "nothing.here"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QLatin1String("the first study")), qPrintable(text(r)));
    }
};

QTEST_MAIN(TestMultiphysicsDoc)
#include "test_multiphysics_doc.moc"
