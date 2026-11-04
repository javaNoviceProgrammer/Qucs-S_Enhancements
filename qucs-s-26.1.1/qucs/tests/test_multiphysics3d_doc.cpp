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
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
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
        QCOMPARE(panel()->exampleList()->count(), 5);
    }

    /// A new 3D model: a tab, the tree a 3D model's - its study, no results;
    /// the component a 3D one.
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
        QVERIFY2(!shown.contains(QStringLiteral("Results")) && shown.contains(QStringLiteral("Study 1")) && shown.contains(QStringLiteral("Stationary")),
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
        QString step;
        for (const FNode& c : doc->model().root.children)
            if (c.type == QLatin1String("study"))
                for (const FNode& st : c.children) step = st.tag;
        QVERIFY(!step.isEmpty());
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
