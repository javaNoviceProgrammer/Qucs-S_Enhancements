/*
 * test_multiphysics3d_doc.cpp - Multiphysics 3D in the window: its dock tab,
 *                               menu and file kind; a new 3D model and its
 *                               tree; solids added and edited through the
 *                               panel, each a step of Undo; the examples
 *                               opened, their geometry built in the
 *                               background and meshed; the 3D view's camera,
 *                               cut and picture; a selection drawn
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
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSlider>
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
#include "multiphysics3ddoc.h"
#include "multiphysics3dpanel.h"
#include "multiphysicsdoc.h"
#include "multiphysicspanel.h"
#include "qucs.h"

using namespace qucs_s::fem;
// (The schematic's Node is another: the model's by its whole name.)
using FNode = qucs_s::fem::Node;

namespace {

QString examples()
{
    return QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics3d/");
}

QTreeWidgetItem* itemOf(QTreeWidget* tree, const QString& tag)
{
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole + 1).toString() == tag) return *it;
        ++it;
    }
    return nullptr;
}

/// The Add menu's kinds of a panel's node menu.
QStringList addsOf(QMenu* menu)
{
    QStringList adds;
    for (QAction* a : menu->actions())
        if (a->menu())
            for (QAction* b : a->menu()->actions()) adds << b->text();
    delete menu;
    return adds;
}

/// A combo box's values.
QStringList valuesOf(QComboBox* box)
{
    QStringList out;
    for (int i = 0; box && i < box->count(); ++i) out << box->itemData(i).toString();
    return out;
}

QStringList titles(QTreeWidget* tree)
{
    QStringList out;
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        out << (*it)->text(0);
        ++it;
    }
    return out;
}

/// How many pixels of \a image are not its corner's colour.
int drawn(const QImage& image)
{
    const QRgb background = image.pixel(2, 2);
    int n = 0;
    for (int y = 0; y < image.height(); y += 3)
        for (int x = 0; x < image.width(); x += 3) n += image.pixel(x, y) != background ? 1 : 0;
    return n;
}

} // namespace

class TestMultiphysics3DDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    Multiphysics3DPanel* panel() const { return app->multiphysics3DPanel(); }
    Multiphysics3DDoc* front() const { return qobject_cast<Multiphysics3DDoc*>(app->DocumentTab->currentWidget()); }
    Multiphysics3DDoc* open(const QString& file)
    {
        if (!app->gotoPage(file)) return nullptr;
        return front();
    }
    /// An example copied into the workspace.
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
        app->resize(1300, 900);
        app->show();
        QVERIFY(panel() != nullptr);
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
            if (auto* doc = dynamic_cast<Multiphysics3DDoc*>(d)) {
                doc->cancel();
                doc->waitForIdle(30000);
            }
            d->setDocChanged(false);
        }
        app->closeAllFiles();
    }

    /// Its tab in the dock after Multiphysics, a page of its own name; its
    /// menu after Multiphysics'; its examples on the start page.
    void inTheWindow()
    {
        const int mp = app->sidePage(QucsApp::SidePage::Multiphysics);
        const int mp3 = app->sidePage(QucsApp::SidePage::Multiphysics3D);
        QVERIFY(mp >= 0 && mp3 == mp + 1);
        QCOMPARE(QucsApp::sidePageName(QucsApp::SidePage::Multiphysics3D), QStringLiteral("multiphysics 3d"));
        QVERIFY(app->showSidePage(QucsApp::SidePage::Multiphysics3D));
        QCOMPARE(app->currentSidePageName(), QStringLiteral("multiphysics 3d"));
        QVERIFY(app->showSidePageNamed(QStringLiteral("multiphysics")));
        const QList<QAction*> bar = app->menuBar()->actions();
        const int menu2 = int(bar.indexOf(app->multiphysicsMenu()->menuAction()));
        const int menu3 = int(bar.indexOf(app->multiphysics3DMenu()->menuAction()));
        QVERIFY(menu2 >= 0 && menu3 == menu2 + 1);
        QVERIFY(QucsApp::isMultiphysics3DFile(QStringLiteral("a/b.qfem3d")) && QucsApp::isMultiphysics3DFile(QStringLiteral("B.QFEM3D")));
        QVERIFY(!QucsApp::isMultiphysics3DFile(QStringLiteral("a.qfem")) && !QucsApp::isMultiphysicsFile(QStringLiteral("a.qfem3d")));
        QCOMPARE(panel()->pages()->currentWidget(), panel()->pages()->widget(0));
        QCOMPARE(panel()->exampleList()->count(), 6);
    }

    /// A new 3D model: a tab, the tree a 3D model's - its study, its results'
    /// derived values and exports; the component a 3D one.
    void aNewModel()
    {
        Multiphysics3DDoc* doc = app->newMultiphysics3DModel();
        QVERIFY(doc && front() == doc);
        QVERIFY(doc->model().is3D());
        QCOMPARE(app->currentSidePageName(), QStringLiteral("multiphysics 3d"));
        QCOMPARE(panel()->document(), doc);
        QVERIFY(panel()->pages()->currentIndex() == 1);
        const QStringList shown = titles(panel()->tree());
        QVERIFY2(shown.contains(QStringLiteral("untitled.qfem3d")) && shown.contains(QStringLiteral("Component (3D)"))
                     && shown.contains(QStringLiteral("Geometry")) && shown.contains(QStringLiteral("Form Union"))
                     && shown.contains(QStringLiteral("Materials")) && shown.contains(QStringLiteral("Mesh")),
                 qPrintable(shown.join(QStringLiteral(", "))));
        QVERIFY2(shown.contains(QStringLiteral("Results")) && shown.contains(QStringLiteral("Derived Values")) && shown.contains(QStringLiteral("Study 1"))
                     && shown.contains(QStringLiteral("Stationary")),
                 qPrintable(shown.join(QStringLiteral(", "))));
        // Nothing to build yet: not built.
        QVERIFY(doc->waitForIdle());
        QVERIFY(!doc->geometry().topology);
        QCOMPARE(itemOf(panel()->tree(), doc->model().geometry().tag)->text(1), QStringLiteral("not built"));
        // What may be added to its geometry: solids, not a plan's shapes.
        QMenu* menu = panel()->menuFor(doc->model().geometry().tag);
        QStringList adds;
        for (QAction* a : menu->actions())
            if (a->menu())
                for (QAction* b : a->menu()->actions()) adds << b->text();
        delete menu;
        QVERIFY2(adds.contains(QStringLiteral("Box")) && adds.contains(QStringLiteral("Extrude")) && adds.contains(QStringLiteral("Layer Stack"))
                     && adds.contains(QStringLiteral("Union")) && !adds.contains(QStringLiteral("Rectangle")),
                 qPrintable(adds.join(QStringLiteral(", "))));
        // A named selection's level: edges too, in 3D.
        const QString sel = panel()->addNode(doc->model().component().child(QStringLiteral("selections"))->tag, QStringLiteral("selection"));
        QVERIFY(!sel.isEmpty());
        panel()->selectNode(sel);
        QCOMPARE(valuesOf(qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("level")))),
                 (QStringList{QStringLiteral("domain"), QStringLiteral("boundary"), QStringLiteral("point"), QStringLiteral("edge")}));
    }

    /// The 2D panel keeps to a 2D model's kinds: no Box in its geometry's
    /// menu, no edges for its selections.
    void theTwoDPanelKeepsToTwoD()
    {
        MultiphysicsDoc* doc = app->newMultiphysicsModel();
        QVERIFY(doc);
        MultiphysicsPanel* flat = app->multiphysicsPanel();
        const QStringList adds = addsOf(flat->menuFor(doc->model().geometry().tag));
        QVERIFY2(adds.contains(QStringLiteral("Rectangle")) && adds.contains(QStringLiteral("Union")) && !adds.contains(QStringLiteral("Box"))
                     && !adds.contains(QStringLiteral("Extrude")) && !adds.contains(QStringLiteral("Layer Stack")),
                 qPrintable(adds.join(QStringLiteral(", "))));
        const QString sel = flat->addNode(doc->model().component().child(QStringLiteral("selections"))->tag, QStringLiteral("selection"));
        QVERIFY(!sel.isEmpty());
        flat->selectNode(sel);
        QCOMPARE(valuesOf(qobject_cast<QComboBox*>(flat->editorFor(QStringLiteral("level")))),
                 (QStringList{QStringLiteral("domain"), QStringLiteral("boundary"), QStringLiteral("point")}));
    }

    /// Solids added and edited through the panel: the geometry built after
    /// each, each a step of Undo.
    void solidsThroughThePanel()
    {
        Multiphysics3DDoc* doc = app->newMultiphysics3DModel();
        const QString box = panel()->addNode(doc->model().geometry().tag, QStringLiteral("box"));
        QVERIFY(!box.isEmpty());
        QVERIFY(doc->waitForIdle());
        QVERIFY(doc->geometry().ok());
        QCOMPARE(int(doc->geometry().topology->domains.size()), 1);
        QCOMPARE(int(doc->geometry().topology->faces.size()), 6);
        QTreeWidgetItem* geometry = itemOf(panel()->tree(), doc->model().geometry().tag);
        QVERIFY(geometry);
        QTRY_COMPARE(geometry->text(1), QStringLiteral("1 domain"));
        // Its height edited in its settings.
        panel()->selectNode(box);
        auto* height = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("height")));
        QVERIFY(height);
        height->setText(QStringLiteral("2.5"));
        emit height->editingFinished();
        QCOMPARE(doc->model().find(box)->text(QStringLiteral("height")), QStringLiteral("2.5"));
        QVERIFY(doc->waitForIdle());
        QVERIFY(std::abs(doc->geometry().topology->domains[0].volume - 2.5) < 1e-9);
        // An extrude and its sketch: a solid of the sketch's rectangle.
        const QString ext = panel()->addNode(doc->model().geometry().tag, QStringLiteral("extrude"));
        QVERIFY(!ext.isEmpty());
        QMenu* menu = panel()->menuFor(ext);
        QStringList adds;
        for (QAction* a : menu->actions())
            if (a->menu())
                for (QAction* b : a->menu()->actions()) adds << b->text();
        delete menu;
        QVERIFY2(adds.contains(QStringLiteral("Rectangle")) && !adds.contains(QStringLiteral("Box")), qPrintable(adds.join(QStringLiteral(", "))));
        const QString rect = panel()->addNode(ext, QStringLiteral("rectangle"));
        QVERIFY(!rect.isEmpty());
        doc->editNode(rect, [](FNode& n) { n.props.insert(QStringLiteral("position"), QJsonArray{QStringLiteral("3"), QStringLiteral("0")}); },
                      QStringLiteral("move"));
        QVERIFY(doc->waitForIdle());
        QVERIFY2(doc->geometry().ok(), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        QCOMPARE(int(doc->geometry().topology->domains.size()), 2);
        // Undo, redo: the geometry follows. Moved back, the rectangle's solid
        // is in the box: what the two share a domain of its own, under the
        // box's own above it.
        doc->undo();
        QVERIFY(doc->waitForIdle());
        QCOMPARE(doc->model().find(rect)->pair(QStringLiteral("position")), (QStringList{QStringLiteral("0"), QStringLiteral("0")}));
        QCOMPARE(int(doc->geometry().topology->domains.size()), 2);
        QCOMPARE(doc->geometry().topology->domains[0].objects.size(), 2);
        doc->redo();
        QVERIFY(doc->waitForIdle());
        QCOMPARE(int(doc->geometry().topology->domains.size()), 2);
        QCOMPARE(doc->geometry().topology->domains[0].objects.size(), 1);
        // A sketch's object chosen: its solid drawn, the rest as edges.
        panel()->selectNode(rect);
        QCOMPARE(doc->highlightLevel(), Level::Domain);
        QCOMPARE(doc->highlighted().size(), 1);
        // A feature that fails: said, marked in the tree.
        doc->editNode(box, [](FNode& n) { n.props.insert(QStringLiteral("height"), QStringLiteral("0")); }, QStringLiteral("flat"));
        QVERIFY(doc->waitForIdle());
        QVERIFY(!doc->geometry().ok());
        QTRY_COMPARE(itemOf(panel()->tree(), box)->text(1), QStringLiteral("error"));
        QVERIFY(doc->messages().join(QStringLiteral("\n")).contains(QStringLiteral("height must be more than 0")));
    }

    /// The examples: opened in tabs of their own, built and meshed; a
    /// selection drawn; the file written as it was read.
    void theExamples()
    {
        Multiphysics3DDoc* doc = open(copyOf(QStringLiteral("chip_heater.qfem3d")));
        QVERIFY(doc);
        QVERIFY(doc->waitForIdle());
        QVERIFY2(doc->geometry().ok(), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        const Topology3& t = *doc->geometry().topology;
        // The substrate, the oxide, the via in it, the heater on it.
        QCOMPARE(int(t.domains.size()), 4);
        QCOMPARE(t.objectNames, (QStringList{QStringLiteral("substrate"), QStringLiteral("oxide"), QStringLiteral("heater"), QStringLiteral("via")}));
        doc->buildMesh();
        QVERIFY(doc->waitForIdle());
        QVERIFY2(doc->mesh() && !doc->meshStale(), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        QCOMPARE(doc->show(), Multiphysics3DDoc::Show::Mesh);
        QVERIFY(doc->mesh()->prisms.size() > 100);
        QTRY_VERIFY(itemOf(panel()->tree(), doc->model().mesh().tag)->text(1).endsWith(QStringLiteral("prisms")));
        QVERIFY(!doc->view()->scene().triangles.empty());
        QVERIFY(drawn(doc->view()->picture()) > 100);
        // A material's selection (the substrate's layer): its domain drawn.
        doc->showGeometry();
        const FNode* silicon = nullptr;
        for (const FNode& m : doc->model().materials().children)
            if (m.label == QLatin1String("Silicon")) silicon = &m;
        QVERIFY(silicon);
        panel()->selectNode(silicon->tag);
        QCOMPARE(doc->highlightLevel(), Level::Domain);
        QCOMPARE(doc->highlighted(), QVector<int>{0});
        // Written as it was read.
        QFile read(doc->getDocName());
        QVERIFY(read.open(QIODevice::ReadOnly));
        const std::optional<Model> before = Model::fromJson(read.readAll());
        QVERIFY(before);
        QCOMPARE(doc->save(), 0);
        QFile written(doc->getDocName());
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray bytes = written.readAll();
        QVERIFY(bytes.contains("qucs-s multiphysics 3d 1"));
        QCOMPARE(Model::fromJson(bytes)->toJson(), before->toJson());
        // The other: a sketch's ring on a plate.
        Multiphysics3DDoc* ring = open(copyOf(QStringLiteral("extruded_ring.qfem3d")));
        QVERIFY(ring && ring != doc);
        QVERIFY(ring->waitForIdle());
        QVERIFY2(ring->geometry().ok(), qPrintable(ring->messages().join(QStringLiteral("\n"))));
        QCOMPARE(ring->geometry().topology->objectNames, (QStringList{QStringLiteral("plate"), QStringLiteral("band")}));
        QCOMPARE(int(ring->geometry().topology->domains.size()), 2);
    }

    /// The chip heater solved through its tab (Compute; F2): the solution
    /// shown, its faces coloured by ht.T under its legend, its heat the
    /// heater's power, its globals said, none colder than its back; another
    /// plot, the cut's slice; an edit leaves it stale, said, and twice the
    /// power a little more than twice the rise - silicon's k falls as it
    /// warms.
    void aModelSolvedInItsTab()
    {
        Multiphysics3DDoc* doc = open(copyOf(QStringLiteral("chip_heater.qfem3d")));
        QVERIFY(doc && doc->waitForIdle());
        auto* compute = doc->findChild<QToolButton*>(QStringLiteral("mp3Compute"));
        QVERIFY(compute);
        compute->click();
        QVERIFY(doc->waitForIdle(300000));
        QVERIFY2(doc->solution() && !doc->solutionStale(), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        QCOMPARE(doc->show(), Multiphysics3DDoc::Show::Solution);
        const Solution3& s = *doc->solution();
        const double P = s.global(QStringLiteral("ht.P")).value_or(0);
        QVERIFY2(std::abs(P - 10e-3) < 1e-9, qPrintable(s.globalText(QStringLiteral("ht.P"))));
        const double rise = s.global(QStringLiteral("ht.Tmax")).value_or(0) - 293.15;
        QVERIFY2(rise > 0.1 && rise < 100, qPrintable(s.globalText(QStringLiteral("ht.Tmax"))));
        QVERIFY(s.global(QStringLiteral("ht.Tmin")).value_or(0) >= 293.15 - 1e-9);
        const QString said = doc->messages().join(QStringLiteral("\n"));
        QVERIFY2(said.contains(QStringLiteral("ht.Tmax = ")) && said.contains(QStringLiteral("algebraic multigrid")), qPrintable(said));
        // Its faces coloured by their values, under a legend of ht.T.
        int shaded = 0;
        for (const View3D::Triangle& t : doc->view()->scene().triangles) shaded += t.shaded ? 1 : 0;
        QVERIFY(shaded > 100);
        QVERIFY(doc->view()->legend().shown);
        QVERIFY2(doc->view()->legend().title.startsWith(QStringLiteral("ht.T")), qPrintable(doc->view()->legend().title));
        QVERIFY(std::abs(doc->plotRange().second - s.global(QStringLiteral("ht.Tmax")).value_or(0)) < 1e-9 * 300);
        QVERIFY(drawn(doc->view()->picture()) > 100);
        // Another plot; a bad one said, the last good one's range kept.
        doc->setPlotExpression(QStringLiteral("ht.normq"));
        QVERIFY(doc->view()->legend().title.startsWith(QStringLiteral("ht.normq")));
        QVERIFY(doc->plotRange().second > 1e3);
        doc->setPlotExpression(QStringLiteral("ht.nothing"));
        QVERIFY(doc->messages().last().contains(QStringLiteral("ht.nothing")));
        doc->setPlotExpression(QStringLiteral("ht.T"));
        // Cut across z through the oxide: the slice's triangles too.
        const int before = int(doc->view()->scene().triangles.size());
        auto* clip = doc->findChild<QComboBox*>(QStringLiteral("mp3Clip"));
        auto* at = doc->findChild<QSlider*>(QStringLiteral("mp3ClipAt"));
        QVERIFY(clip && at);
        clip->setCurrentIndex(3);
        emit clip->activated(3);
        at->setValue(90);
        QVERIFY2(int(doc->view()->scene().triangles.size()) > before, qPrintable(QStringLiteral("%1, then %2").arg(before).arg(doc->view()->scene().triangles.size())));
        clip->setCurrentIndex(0);
        emit clip->activated(0);
        // An edit: stale, said beside the bar; twice the power, twice the rise
        // (F2 as Compute).
        Model m = doc->model();
        QList<Model::Parameter> ps = m.parameters();
        for (Model::Parameter& p : ps)
            if (p.name == QLatin1String("P")) p.expression = QStringLiteral("20[mW]");
        m.setParameters(ps);
        QVERIFY(doc->setModel(m, QStringLiteral("power")));
        QVERIFY(doc->solutionStale());
        const QLabel* stage = doc->findChild<QLabel*>(QStringLiteral("mp3Stage"));
        QVERIFY(stage && stage->isVisible() && stage->text().contains(QStringLiteral("Compute again")));
        app->slotSimulate(doc);
        QVERIFY(doc->waitForIdle(300000));
        QVERIFY2(doc->solution() && !doc->solutionStale(), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        const double rise2 = doc->solution()->global(QStringLiteral("ht.Tmax")).value_or(0) - 293.15;
        QVERIFY2(rise2 > 2 * rise && rise2 < 2.05 * rise, qPrintable(QStringLiteral("%1, then %2").arg(rise).arg(rise2)));
        QVERIFY(!stage->isVisible());
    }

    /// A step in time computed in the tab: its output times in the Time box,
    /// the last shown; an earlier one chosen, the plot, its range and the
    /// globals are of that time. A model at rest shows no Time box.
    void aModelInTime()
    {
        Multiphysics3DDoc* doc = open(copyOf(QStringLiteral("chip_heater.qfem3d")));
        QVERIFY(doc && doc->waitForIdle());
        auto* times = doc->findChild<QComboBox*>(QStringLiteral("mp3Time"));
        QVERIFY(times && !times->isVisibleTo(doc));
        Model m = doc->model();
        for (FNode& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.clear();
                FNode t = m.make(QStringLiteral("transient"));
                t.props.insert(QStringLiteral("times"), QStringLiteral("range(0, 5[ms], 10[ms])"));
                t.props.insert(QStringLiteral("stepping"), QStringLiteral("fixed"));
                t.props.insert(QStringLiteral("dt"), QStringLiteral("2.5[ms]"));
                c.children.push_back(t);
            }
        QVERIFY(doc->setModel(m, QStringLiteral("in time")));
        doc->compute();
        QVERIFY(doc->waitForIdle(300000));
        const QString said = doc->messages().join(QStringLiteral("\n"));
        QVERIFY2(doc->solution() && !doc->solutionStale(), qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("output times")) && said.contains(QStringLiteral("At t = 10 ms")), qPrintable(said));
        QVERIFY(times->isVisibleTo(doc));
        QCOMPARE(times->count(), 3);
        QCOMPARE(times->currentIndex(), 2);
        QCOMPARE(doc->shownTime(), 2);
        const double hot = doc->plotRange().second, tmax = doc->solution()->global(QStringLiteral("ht.Tmax")).value_or(0);
        QVERIFY2(hot > 293.15 + 0.1 && std::abs(hot - tmax) < 1e-9 * hot, qPrintable(QStringLiteral("%1, %2").arg(hot).arg(tmax)));
        times->setCurrentIndex(1);
        emit times->activated(1);
        QCOMPARE(doc->shownTime(), 1);
        QVERIFY(std::abs(doc->solution()->time() - 5e-3) < 1e-12);
        const double warm = doc->plotRange().second, tmax1 = doc->solution()->global(QStringLiteral("ht.Tmax")).value_or(0);
        QVERIFY2(warm < hot && warm > 293.15 && std::abs(warm - tmax1) < 1e-9 * warm, qPrintable(QStringLiteral("%1, then %2").arg(hot).arg(warm)));
        // The start: every temperature its initial one.
        doc->setShownTime(0);
        QVERIFY2(std::abs(doc->plotRange().second - 293.15) < 1e-9 && std::abs(doc->plotRange().first - 293.15) < 1e-9,
                 qPrintable(QStringLiteral("%1 to %2").arg(doc->plotRange().first).arg(doc->plotRange().second)));
        QCOMPARE(times->currentIndex(), 0);
    }

    /// A Parametric Sweep computed in the tab: its points in the Point box,
    /// the last shown; the first chosen, its rise half the last's (twice the
    /// power, a little more than twice the rise: silicon's k falls as it
    /// warms).
    void aSweptModel()
    {
        Multiphysics3DDoc* doc = open(copyOf(QStringLiteral("chip_heater.qfem3d")));
        QVERIFY(doc && doc->waitForIdle());
        auto* points = doc->findChild<QComboBox*>(QStringLiteral("mp3Point"));
        QVERIFY(points && !points->isVisibleTo(doc));
        Model m = doc->model();
        for (FNode& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                FNode sw = m.make(QStringLiteral("sweep"));
                QJsonArray rows;
                rows.append(QJsonArray{QStringLiteral("P"), QStringLiteral("10, 20"), QStringLiteral("mW")});
                sw.props.insert(QStringLiteral("parameters"), rows);
                c.children.push_back(sw);
            }
        QVERIFY(doc->setModel(m, QStringLiteral("swept")));
        doc->compute();
        QVERIFY(doc->waitForIdle(300000));
        const QString said = doc->messages().join(QStringLiteral("\n"));
        QVERIFY2(doc->solution() && doc->solutions().size() == 2, qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("2 points of P")), qPrintable(said));
        QVERIFY(points->isVisibleTo(doc));
        QCOMPARE(points->count(), 2);
        QCOMPARE(points->currentIndex(), 1);
        QCOMPARE(points->itemText(0), QStringLiteral("P = 10 mW"));
        QCOMPARE(doc->shownPoint(), 1);
        const double hot = doc->plotRange().second - 293.15;
        points->setCurrentIndex(0);
        emit points->activated(0);
        QCOMPARE(doc->shownPoint(), 0);
        const double warm = doc->plotRange().second - 293.15;
        QVERIFY2(hot > 2 * warm && hot < 2.05 * warm, qPrintable(QStringLiteral("%1 K, then %2 K").arg(warm).arg(hot)));
        QVERIFY(std::abs(doc->solution()->global(QStringLiteral("ht.P")).value_or(0) - 10e-3) < 1e-12);
    }

    /// Derived values in the tab: the kinds a 3D model evaluates offered
    /// (not the plane's line parameters), one added through the panel and
    /// evaluated by its button - its row in the table, ht.Tmax's value; a
    /// point evaluation's coordinates x, y and z; in time, a row an output
    /// time, each named by it.
    void derivedValuesInTheTab()
    {
        Multiphysics3DDoc* doc = open(copyOf(QStringLiteral("chip_heater.qfem3d")));
        QVERIFY(doc && doc->waitForIdle());
        QString derived;
        for (const FNode& c : doc->model().root.children)
            if (c.type == QLatin1String("results"))
                for (const FNode& g : c.children)
                    if (g.type == QLatin1String("derived")) derived = g.tag;
        QVERIFY(!derived.isEmpty());
        const QStringList kinds = addsOf(panel()->menuFor(derived));
        QVERIFY2(kinds.contains(QStringLiteral("Global Evaluation")) && kinds.contains(QStringLiteral("Integration")) && kinds.contains(QStringLiteral("Maximum"))
                     && kinds.contains(QStringLiteral("Point Evaluation")) && !kinds.contains(QStringLiteral("Line Parameters")),
                 qPrintable(kinds.join(QStringLiteral(", "))));
        const QString mx = panel()->addNode(derived, QStringLiteral("maximum"));
        QVERIFY(!mx.isEmpty());
        doc->editNode(mx, [](FNode& n) {
            n.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
            n.props.insert(QStringLiteral("expression"), QStringLiteral("ht.T"));
        }, QStringLiteral("max"));
        panel()->selectNode(mx);
        // (Before a solve: said.)
        QVERIFY(doc->evaluate(mx).error.contains(QStringLiteral("not computed yet")));
        doc->compute();
        QVERIFY(doc->waitForIdle(300000));
        QVERIFY2(doc->solution(), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        // (A derived value added after the solve: the solution is of the model
        // still - its results alone changed.)
        QVERIFY(!doc->solutionStale());
        QVERIFY(!panel()->addNode(derived, QStringLiteral("global")).isEmpty());
        QVERIFY(!doc->solutionStale());
        panel()->selectNode(mx);
        // (The form made after a turn of the event loop.)
        QTRY_VERIFY(panel()->findChild<QPushButton*>(QStringLiteral("mp3EvaluateButton")) != nullptr);
        // (Over a 3D model's edges too.)
        const QStringList levels = valuesOf(qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("level"))));
        QVERIFY2(levels.contains(QStringLiteral("edge")) && levels.contains(QStringLiteral("boundary")), qPrintable(levels.join(QStringLiteral(", "))));
        panel()->findChild<QPushButton*>(QStringLiteral("mp3EvaluateButton"))->click();
        QTableWidget* table = doc->table();
        QVERIFY(table && table->isVisibleTo(doc));
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("ht.T"));
        const double tmax = doc->solution()->global(QStringLiteral("ht.Tmax")).value_or(0);
        const DerivedResult r = doc->evaluate(mx);
        QVERIFY2(r.values.size() == 1 && std::abs(r.values.front().value - tmax) < 1e-6, qPrintable(r.error));
        QCOMPARE(table->rowCount(), 1);
        // A point evaluation: three columns.
        const QString pe = panel()->addNode(derived, QStringLiteral("pointeval"));
        panel()->selectNode(pe);
        QTRY_VERIFY(panel()->findChild<QTableWidget*>(QStringLiteral("mp3Points")) != nullptr);
        auto* points = panel()->findChild<QTableWidget*>(QStringLiteral("mp3Points"));
        QCOMPARE(points->columnCount(), 3);
        QCOMPARE(points->horizontalHeaderItem(2)->text(), QStringLiteral("z"));
        // In time: a row an output time.
        Model m = doc->model();
        for (FNode& c : m.root.children)
            if (c.type == QLatin1String("study")) {
                c.children.clear();
                FNode t = m.make(QStringLiteral("transient"));
                t.props.insert(QStringLiteral("times"), QStringLiteral("range(0, 5[ms], 10[ms])"));
                t.props.insert(QStringLiteral("stepping"), QStringLiteral("fixed"));
                t.props.insert(QStringLiteral("dt"), QStringLiteral("5[ms]"));
                c.children.push_back(t);
            }
        QVERIFY(doc->setModel(m, QStringLiteral("in time")));
        doc->compute();
        QVERIFY(doc->waitForIdle(300000));
        // (The time shown, the second, shown still after.)
        doc->setShownTime(1);
        const double shownTmax = doc->solution()->global(QStringLiteral("ht.Tmax")).value_or(0);
        const DerivedResult inTime = doc->evaluate(mx);
        QVERIFY2(inTime.values.size() == 3, qPrintable(inTime.error));
        QCOMPARE(inTime.values.front().name, QStringLiteral("t = 0 s: ht.T"));
        QVERIFY(std::abs(inTime.values.front().value - 293.15) < 1e-9);
        QVERIFY(std::abs(inTime.values[1].value - shownTmax) < 1e-6);
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(doc->shownTime(), 1);
        QCOMPARE(doc->solution()->global(QStringLiteral("ht.Tmax")).value_or(0), shownTmax);
    }

    /// 3D plot groups in the tab: after the first solve two default ones for
    /// its physics - ht.T on its boundaries, and on a slice through the middle
    /// across x alone -, the solution fresh still, each in the Show box; the
    /// first shown by the panel's Plot button, its surface's triangles and
    /// legend; an isosurface and arrows added to the slice's draw more there;
    /// a clip across z cuts the view and draws the cut face; an expression
    /// that does not read said; the group shown removed, the solution shown.
    void plotGroupsInTheTab()
    {
        Multiphysics3DDoc* doc = open(copyOf(QStringLiteral("chip_heater.qfem3d")));
        QVERIFY(doc && doc->waitForIdle());
        doc->compute();
        QVERIFY(doc->waitForIdle(300000));
        QVERIFY2(doc->solution(), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        QStringList groups;
        for (const FNode& c : doc->model().results().children)
            if (c.type == QLatin1String("plotgroup3d")) groups << c.tag;
        QCOMPARE(groups.size(), 2);
        const QString group = groups[0], sliced = groups[1];
        const FNode* g = doc->model().find(group);
        QCOMPARE(g->name(), QStringLiteral("Heat Transfer in Solids (ht)"));
        QVERIFY(g->children.size() == 1 && g->children[0].type == QLatin1String("surface"));
        const FNode* g2 = doc->model().find(sliced);
        QCOMPARE(g2->name(), QStringLiteral("Heat Transfer in Solids, slice (ht)"));
        QVERIFY(g2->children.size() == 1 && g2->children[0].type == QLatin1String("slice"));
        QCOMPARE(g2->children[0].text(QStringLiteral("plane")), QStringLiteral("x"));
        const QString sliceTag = g2->children[0].tag;   // (g and g2 go with the next edit)
        QVERIFY(!doc->solutionStale());
        auto* show = doc->findChild<QComboBox*>(QStringLiteral("mp3Show"));
        QVERIFY(show && show->findData(group) == 3 && show->findData(sliced) == 4);
        // What its results and a group offer: the 3D kinds, not the plane's.
        const QStringList groupsOffered = addsOf(panel()->menuFor(doc->model().results().tag));
        QVERIFY2(groupsOffered.contains(QStringLiteral("3D Plot Group")) && !groupsOffered.contains(QStringLiteral("2D Plot Group")),
                 qPrintable(groupsOffered.join(QStringLiteral(", "))));
        const QStringList plots = addsOf(panel()->menuFor(group));
        QVERIFY2(plots.contains(QStringLiteral("Surface")) && plots.contains(QStringLiteral("Slice")) && plots.contains(QStringLiteral("Isosurface"))
                     && plots.contains(QStringLiteral("Arrow Volume")) && plots.contains(QStringLiteral("Mesh")) && !plots.contains(QStringLiteral("Contour")),
                 qPrintable(plots.join(QStringLiteral(", "))));
        // The first shown by its Plot button.
        panel()->selectNode(group);
        QTRY_VERIFY(panel()->findChild<QPushButton*>(QStringLiteral("mp3PlotButton")) != nullptr);
        panel()->findChild<QPushButton*>(QStringLiteral("mp3PlotButton"))->click();
        QCOMPARE(doc->show(), Multiphysics3DDoc::Show::Plot);
        QCOMPARE(show->currentIndex(), 3);
        auto shaded = [doc] {
            int n = 0;
            for (const View3D::Triangle& t : doc->view()->scene().triangles) n += t.shaded ? 1 : 0;
            return n;
        };
        QVERIFY(shaded() > 100);
        QVERIFY(doc->view()->legend().shown && doc->view()->legend().title.startsWith(QStringLiteral("ht.T")));
        QVERIFY2(doc->plotProblems().isEmpty(), qPrintable(doc->plotProblems().join(QStringLiteral("; "))));
        // The slice's group, then an isosurface and arrows in it.
        QVERIFY(doc->showPlotGroup(sliced));
        const int slice = shaded(), triangles = int(doc->view()->scene().triangles.size()), lines = int(doc->view()->scene().lines.size());
        QVERIFY(slice > 10);
        // (Through the middle across x: each corner there.)
        const Box3 bounds = doc->geometry().topology->bounds;
        const double middle = 0.5 * (bounds.x0 + bounds.x1);
        for (const View3D::Triangle& t : doc->view()->scene().triangles)
            if (t.shaded)
                for (const Vec3& c : t.p) QVERIFY2(std::abs(c.x - middle) < 1e-9 * (bounds.x1 - bounds.x0), qPrintable(QString::number(c.x)));
        Model m = doc->model();
        FNode iso = m.make(QStringLiteral("isosurface"));
        iso.props.insert(QStringLiteral("expression"), QStringLiteral("ht.T"));
        iso.props.insert(QStringLiteral("levels"), 3);
        QVERIFY(m.add(sliced, iso));
        FNode arrows = m.make(QStringLiteral("arrowvolume"));
        arrows.props.insert(QStringLiteral("x"), QStringLiteral("ht.qx"));
        arrows.props.insert(QStringLiteral("y"), QStringLiteral("ht.qy"));
        arrows.props.insert(QStringLiteral("z"), QStringLiteral("ht.qz"));
        QVERIFY(m.add(sliced, arrows));
        QVERIFY(doc->setModel(m, QStringLiteral("more plots")));
        QVERIFY(!doc->solutionStale());
        QCOMPARE(doc->show(), Multiphysics3DDoc::Show::Plot);
        QVERIFY2(int(doc->view()->scene().triangles.size()) > triangles, qPrintable(QStringLiteral("%1, then %2").arg(triangles).arg(doc->view()->scene().triangles.size())));
        QVERIFY2(int(doc->view()->scene().lines.size()) > lines + 100, qPrintable(QStringLiteral("%1, then %2").arg(lines).arg(doc->view()->scene().lines.size())));
        // A clip across z of the first, through the oxide: its cut face.
        QVERIFY(doc->showPlotGroup(group));
        const int whole = int(doc->view()->scene().triangles.size());
        const double top = doc->geometry().topology->bounds.z1;
        doc->editNode(group, [top](FNode& n) {
            n.props.insert(QStringLiteral("clip"), QStringLiteral("z"));
            n.props.insert(QStringLiteral("clipat"), QString::number(top - 1, 'g', 12));
        }, QStringLiteral("clip"));
        QCOMPARE(doc->view()->clipAxis(), 2);
        QVERIFY(std::abs(doc->view()->clipValue() - (top - 1)) < 1e-6 * std::max(1.0, top));
        QVERIFY(int(doc->view()->scene().triangles.size()) > whole);
        // An expression that does not read: said.
        doc->editNode(sliceTag, [](FNode& n) { n.props.insert(QStringLiteral("expression"), QStringLiteral("ht.nothing")); }, QStringLiteral("bad"));
        QVERIFY(doc->showPlotGroup(sliced));
        QVERIFY2(!doc->plotProblems().isEmpty() && doc->plotProblems().first().contains(QStringLiteral("ht.nothing")),
                 qPrintable(doc->plotProblems().join(QStringLiteral("; "))));
        // The group shown removed: the solution shown.
        m = doc->model();
        QVERIFY(m.remove(sliced));
        QVERIFY(doc->setModel(m, QStringLiteral("no group")));
        QCOMPARE(doc->show(), Multiphysics3DDoc::Show::Solution);
        QCOMPARE(show->count(), 4);
    }

    /// A face on the view's cut is drawn whole: each of its pixels kept, not
    /// each by its rounding (a clipped view's cut face was speckled). Its
    /// plane at odd heights between odd bounds, its share of them as a plot
    /// group's clip has it.
    void aFaceOnTheCutIsWhole()
    {
        // (Its red pixels: the cut's own words drawn over it are not.)
        auto red = [](const QImage& image) {
            int n = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    n += c.red() > 80 && c.green() < 60 && c.blue() < 60 ? 1 : 0;
                }
            return n;
        };
        const double z0 = 0.1234567, z1 = 2.3456789;
        int lost = 0;
        QString said;
        for (double z : {0.3, 1.0 / 3, 0.7071067811865476, 1.0987654321, 2.0001}) {
            View3D view;
            view.resize(400, 300);
            View3D::Scene scene;
            // (A fine grid of triangles on it: many pixels, each its own sum.)
            const int n = 20;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const double a = double(i) / n, b = double(j) / n, d = 1.0 / n;
                    scene.triangles.push_back({{Vec3{a, b, z}, Vec3{a + d, b, z}, Vec3{a, b + d, z}}, QColor(200, 30, 30), 0});
                    scene.triangles.push_back({{Vec3{a + d, b, z}, Vec3{a + d, b + d, z}, Vec3{a, b + d, z}}, QColor(200, 30, 30), 0});
                }
            Box3 b;
            b.add(Vec3{0, 0, z0});
            b.add(Vec3{1, 1, z1});
            scene.bounds = b;
            view.setScene(scene, true);
            view.viewFrom(View3D::Preset::Iso);
            const int whole = red(view.picture());
            view.setClip(2, (z - z0) / (z1 - z0));
            const int cut = red(view.picture());
            lost += whole - cut;
            said += QStringLiteral(" z %1: %2 pixels, cut %3;").arg(z).arg(whole).arg(cut);
            QVERIFY(whole > 1000);
        }
        QVERIFY2(lost == 0, qPrintable(said));
    }

    /// A 3D model's physics through the panel: heat, electrostatics and
    /// currents offered on its component (no solid mechanics, no
    /// semiconductor); its study's step without the plane's adaptive
    /// refinement, its nonlinear methods without Newton's.
    void physicsThroughThePanel()
    {
        Multiphysics3DDoc* doc = app->newMultiphysics3DModel();
        const QStringList adds = addsOf(panel()->menuFor(doc->model().component().tag));
        QVERIFY2(adds.contains(QStringLiteral("Heat Transfer in Solids")) && adds.contains(QStringLiteral("Electrostatics"))
                     && adds.contains(QStringLiteral("Electric Currents")) && !adds.contains(QStringLiteral("Solid Mechanics"))
                     && !adds.contains(QStringLiteral("Semiconductor")),
                 qPrintable(adds.join(QStringLiteral(", "))));
        const QString ht = panel()->addNode(doc->model().component().tag, QStringLiteral("heat"));
        QVERIFY(!ht.isEmpty());
        QCOMPARE(doc->model().find(ht)->text(QStringLiteral("order")), QStringLiteral("1"));
        const QStringList features = addsOf(panel()->menuFor(ht));
        QVERIFY2(features.contains(QStringLiteral("Temperature")) && features.contains(QStringLiteral("Heat Flux")), qPrintable(features.join(QStringLiteral(", "))));
        QString step, study;
        for (const FNode& c : doc->model().root.children)
            if (c.type == QLatin1String("study")) {
                study = c.tag;
                for (const FNode& st : c.children) step = st.tag;
            }
        QVERIFY(!step.isEmpty());
        // Its study's steps: at rest and in time, not the plane's others.
        const QStringList steps = addsOf(panel()->menuFor(study));
        QVERIFY2(steps.contains(QStringLiteral("Stationary")) && steps.contains(QStringLiteral("Time Dependent"))
                     && !steps.contains(QStringLiteral("Frequency Domain")) && !steps.contains(QStringLiteral("Mode Analysis")),
                 qPrintable(steps.join(QStringLiteral(", "))));
        const QString inTime = panel()->addNode(study, QStringLiteral("transient"));
        QVERIFY(!inTime.isEmpty());
        panel()->selectNode(inTime);
        QVERIFY(panel()->editorFor(QStringLiteral("times")) != nullptr);
        const QStringList timeMethods = valuesOf(qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("nonlinear"))));
        QVERIFY2(!timeMethods.contains(QStringLiteral("newton")), qPrintable(timeMethods.join(QStringLiteral(", "))));
        panel()->selectNode(step);
        QVERIFY(panel()->editorFor(QStringLiteral("solver")) != nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("adaptive")) == nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("lightloop")) == nullptr);
        const QStringList methods = valuesOf(qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("nonlinear"))));
        QVERIFY2(methods.contains(QStringLiteral("picard")) && !methods.contains(QStringLiteral("newton")), qPrintable(methods.join(QStringLiteral(", "))));
        QVERIFY(qobject_cast<QListWidget*>(panel()->editorFor(QStringLiteral("physics"))) != nullptr);
        // A heat model's solid of the user's k: its anisotropy the plane's
        // alone offered isotropic.
        QString solid;
        for (const FNode& c : doc->model().find(ht)->children)
            if (c.type == QLatin1String("heat/solid")) solid = c.tag;
        doc->editNode(solid, [](FNode& n) { n.props.insert(QStringLiteral("source"), QStringLiteral("user")); }, QStringLiteral("user"));
        panel()->selectNode(solid);
        QCOMPARE(valuesOf(qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("anisotropy")))), QStringList{QStringLiteral("isotropic")});
    }

    /// A 2D model is not opened as a 3D one, nor a 3D one as a 2D one.
    void eachItsOwn()
    {
        QFile f(path(QStringLiteral("flat.qfem3d")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(Model::blank().toJson());
        f.close();
        {
            misc::ErrorCapture capture;
            QVERIFY(!app->gotoPage(path(QStringLiteral("flat.qfem3d"))));
            QVERIFY2(capture.errors().join(QStringLiteral("\n")).contains(QStringLiteral("2D model")), qPrintable(capture.errors().join(QStringLiteral("\n"))));
        }
        QFile g(path(QStringLiteral("solid.qfem")));
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write(Model::blank3d().toJson());
        g.close();
        {
            misc::ErrorCapture capture;
            QVERIFY(!app->gotoPage(path(QStringLiteral("solid.qfem"))));
            QVERIFY2(capture.errors().join(QStringLiteral("\n")).contains(QStringLiteral("3D model")), qPrintable(capture.errors().join(QStringLiteral("\n"))));
        }
    }

    /// The view: presets, fit, the cut, a picture, what is under a point; a
    /// shaded triangle its corners' colours blended across it.
    void theView()
    {
        {
            View3D lone;
            lone.resize(200, 200);
            View3D::Scene scene;
            View3D::Triangle t{{Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}, QColor(Qt::black), 1};
            t.shaded = true;
            t.colors = {QColor(255, 0, 0), QColor(0, 255, 0), QColor(0, 0, 255)};
            scene.triangles.push_back(t);
            lone.setScene(scene, true);
            lone.viewFrom(View3D::Preset::Top);
            const QImage image = lone.picture();
            // (Its middle - the centroid - a mix of all three.)
            const QColor mid = image.pixelColor(image.width() / 2 - image.width() / 12, image.height() / 2 + image.height() / 12);
            QVERIFY2(mid.red() > 20 && mid.green() > 20 && mid.blue() > 20, qPrintable(mid.name()));
        }
        Multiphysics3DDoc* doc = open(copyOf(QStringLiteral("extruded_ring.qfem3d")));
        QVERIFY(doc && doc->waitForIdle());
        View3D* view = doc->view();
        view->viewFrom(View3D::Preset::Top);
        QCOMPARE(view->elevation(), 90.0);
        const QImage top = view->picture();
        QVERIFY(drawn(top) > 100);
        // From the top the ring's face is under the middle of the view.
        const int id = view->idAt(QPoint(view->width() / 2, view->height() / 2));
        QVERIFY2(id >= 0, "the plate under the ring's hole");
        // The nearest face there: the plate's top, not its bottom.
        const Topology3::Face& under = doc->geometry().topology->faces[std::size_t(id)];
        QVERIFY2(under.floor && under.bounds.z0 == 0 && under.bounds.z1 == 0, qPrintable(QString::number(under.bounds.z0)));
        view->viewFrom(View3D::Preset::Iso);
        const QImage iso = view->picture();
        QVERIFY(iso != top);
        // Cut across x at its middle: another picture.
        auto* clip = doc->findChild<QComboBox*>(QStringLiteral("mp3Clip"));
        QVERIFY(clip);
        clip->setCurrentIndex(1);
        emit clip->activated(1);
        QCOMPARE(view->clipAxis(), 0);
        QVERIFY(view->picture() != iso);
        const double scale = view->scale();
        doc->zoomBy(2);
        QVERIFY(std::abs(view->scale() - 2 * scale) < 1e-9 * scale);
        doc->showAll();
        QVERIFY(std::abs(view->scale() - scale) < 1e-6 * scale);
    }
};

QTEST_MAIN(TestMultiphysics3DDoc)
#include "test_multiphysics3d_doc.moc"
