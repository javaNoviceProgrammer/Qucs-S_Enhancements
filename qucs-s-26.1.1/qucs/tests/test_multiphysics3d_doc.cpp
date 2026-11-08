/*
 * test_multiphysics3d_doc.cpp - Multiphysics 3D in the window: its dock tab,
 *                               menu and file kind; a new 3D model and its
 *                               tree; solids added and edited through the
 *                               panel, each a step of Undo; the examples
 *                               opened, their geometry built in the
 *                               background and meshed; the 3D view's camera,
 *                               cut and picture; a selection drawn;
 *                               Claude's fem tools on a 3D model
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
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
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
#include "fem3_estimate.h"
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
#include "qucscontrol.h"

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

/// A block of 2 x 1 x 0.5 mm, k 2 W/(m K), its bottom held at 300 K and q
/// (1e4 W/m^2) flowing in through its top, its walls insulated: T = 300 K +
/// q z / k, linear in z - exact on order 1. As a tool takes it, JSON.
QJsonObject heatedBlock()
{
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), QStringLiteral("mm"));
    Model::Parameter q;
    q.name = QStringLiteral("q");
    q.expression = QStringLiteral("1e4[W/m^2]");
    m.setParameters({q});
    FNode box = m.make(QStringLiteral("box"));
    box.label = QStringLiteral("blk");
    box.props.insert(QStringLiteral("position"), QJsonArray{QStringLiteral("0"), QStringLiteral("0")});
    box.props.insert(QStringLiteral("size"), QJsonArray{QStringLiteral("2"), QStringLiteral("1")});
    box.props.insert(QStringLiteral("z"), QStringLiteral("0"));
    box.props.insert(QStringLiteral("height"), QStringLiteral("0.5"));
    m.add(m.geometry().tag, box);
    FNode heat = m.make(QStringLiteral("heat"));
    heat.props.insert(QStringLiteral("order"), QStringLiteral("1"));
    const QString ht = heat.tag;
    m.add(m.component().tag, heat);
    for (FNode& c : m.find(ht)->children)
        if (c.type == QLatin1String("heat/solid")) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            c.props.insert(QStringLiteral("k"), QStringLiteral("2[W/(m*K)]"));
        }
    auto side = [](const QString& which) {
        QJsonArray of;
        of.append(QStringLiteral("blk"));
        return QJsonObject{{QStringLiteral("of"), of}, {QStringLiteral("side"), which}};
    };
    FNode bottom = m.make(QStringLiteral("heat/temperature"));
    bottom.props.insert(QStringLiteral("selection"), side(QStringLiteral("bottom")));
    bottom.props.insert(QStringLiteral("T0"), QStringLiteral("300[K]"));
    m.add(ht, bottom);
    FNode top = m.make(QStringLiteral("heat/flux"));
    top.props.insert(QStringLiteral("selection"), side(QStringLiteral("top")));
    top.props.insert(QStringLiteral("kind"), QStringLiteral("general"));
    top.props.insert(QStringLiteral("q0"), QStringLiteral("q"));
    m.add(ht, top);
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QStringLiteral("0.5"));
    return QJsonDocument::fromJson(m.toJson()).object();
}

/// A straight silicon wire (0.5 x 0.22 µm, 6 µm long) in oxide, in µm: an
/// eigenmode expansion along x, its ports' TE0 at both ends, a Wavelength
/// Domain step of three wavelengths, a Touchstone export of its S-matrix.
QJsonObject straightWire()
{
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
    auto box = [&m](const QString& label, double y, double z, double w, double h) {
        FNode b = m.make(QStringLiteral("box"));
        b.label = label;
        b.props.insert(QStringLiteral("position"), QJsonArray{QStringLiteral("0"), QString::number(y)});
        b.props.insert(QStringLiteral("size"), QJsonArray{QStringLiteral("6"), QString::number(w)});
        b.props.insert(QStringLiteral("z"), QString::number(z));
        b.props.insert(QStringLiteral("height"), QString::number(h));
        m.add(m.geometry().tag, b);
    };
    box(QStringLiteral("oxide"), -1.5, -1, 3, 2.22);
    box(QStringLiteral("core"), -0.25, 0, 0.5, 0.22);
    auto material = [&m](const QString& label, const QString& n, const QString& on) {
        FNode mat = m.make(QStringLiteral("material"));
        mat.label = label;
        mat.props.insert(QStringLiteral("n"), n);
        mat.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), QJsonArray{on}}});
        m.add(m.materials().tag, mat);
    };
    material(QStringLiteral("oxide"), QStringLiteral("1.444"), QStringLiteral("oxide"));
    material(QStringLiteral("silicon"), QStringLiteral("3.476"), QStringLiteral("core"));
    FNode eme = m.make(QStringLiteral("eme"));
    eme.props.insert(QStringLiteral("modes"), 8);
    eme.props.insert(QStringLiteral("resolution"), QStringLiteral("10"));
    const QString tag = eme.tag;
    m.add(m.component().tag, eme);
    for (const char* end : {"start", "end"}) {
        FNode port = m.make(QStringLiteral("eme/port"));
        port.props.insert(QStringLiteral("name"), QLatin1String(end) == QLatin1String("start") ? QStringLiteral("1") : QStringLiteral("2"));
        port.props.insert(QStringLiteral("end"), QString::fromLatin1(end));
        m.add(tag, port);
    }
    for (FNode& c : m.root.children)
        if (c.type == QLatin1String("study")) {
            c.children.clear();
            FNode step = m.make(QStringLiteral("wavelength"));
            step.props.insert(QStringLiteral("wavelengths"), QStringLiteral("linspace(1500[nm], 1600[nm], 3)"));
            c.children.push_back(step);
        }
    FNode ts = m.make(QStringLiteral("touchstone"));
    FNode* exports = m.results().child(QStringLiteral("exports"));
    if (exports) m.add(exports->tag, ts);
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QStringLiteral("1"));
    return QJsonDocument::fromJson(m.toJson()).object();
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
    /// A tool called as Claude calls it, its answer waited for.
    QJsonObject call(const QString& tool, const QJsonObject& args) { return app->findChild<QucsControl*>()->callNow(tool, args, 300000); }
    static bool failed(const QJsonObject& r) { return r.value(QStringLiteral("isError")).toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r) { return QJsonDocument::fromJson(text(r).toUtf8()).object(); }
    /// An answer's values (fem_evaluate's) in SI, in their order.
    static QList<double> si(const QJsonObject& r)
    {
        QList<double> out;
        for (const QJsonValue& v : json(r).value(QStringLiteral("values")).toArray()) out << v.toObject().value(QStringLiteral("SI")).toDouble(qQNaN());
        return out;
    }
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

    /// Exports in the tab: a 3D model's Export offers VTU, Field on a Grid and
    /// a Touchstone File (an eigenmode expansion's S-matrix), not the plane's
    /// Mode Profile; a VTU node's Export button writes model_tag.vtu beside
    /// the model, said; a NumPy grid its .npy and its _xy.npy; before a solve,
    /// refused.
    void exportsInTheTab()
    {
        const QString file = copyOf(QStringLiteral("chip_heater.qfem3d"));
        Multiphysics3DDoc* doc = open(file);
        QVERIFY(doc && doc->waitForIdle());
        QString exports;
        for (const FNode& c : doc->model().results().children)
            if (c.type == QLatin1String("exports")) exports = c.tag;
        QVERIFY(!exports.isEmpty());
        const QStringList offered = addsOf(panel()->menuFor(exports));
        QVERIFY2(offered.contains(QStringLiteral("VTU (ParaView)")) && offered.contains(QStringLiteral("Field on a Grid"))
                     && offered.contains(QStringLiteral("Touchstone File")) && !offered.contains(QStringLiteral("Mode Profile")),
                 qPrintable(offered.join(QStringLiteral(", "))));
        const QString vtu = panel()->addNode(exports, QStringLiteral("vtu"));
        QVERIFY(!vtu.isEmpty());
        QVERIFY(doc->runExport(vtu).isEmpty());
        QVERIFY(doc->messages().last().contains(QStringLiteral("not computed yet")));
        doc->compute();
        QVERIFY(doc->waitForIdle(300000));
        QVERIFY2(doc->solution(), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        panel()->selectNode(vtu);
        QTRY_VERIFY(panel()->findChild<QPushButton*>(QStringLiteral("mp3ExportButton")) != nullptr);
        panel()->findChild<QPushButton*>(QStringLiteral("mp3ExportButton"))->click();
        const QString written = QFileInfo(file).absoluteDir().filePath(QStringLiteral("%1_%2.vtu").arg(QFileInfo(file).completeBaseName(), vtu));
        QVERIFY2(QFileInfo::exists(written), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        QFile f(written);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.read(64).startsWith("<?xml"));
        QVERIFY2(doc->messages().last().contains(QStringLiteral("written")), qPrintable(doc->messages().last()));
        // A NumPy grid: its coordinates beside it.
        const QString grid = panel()->addNode(exports, QStringLiteral("fieldexport"));
        doc->editNode(grid, [](FNode& n) {
            n.props.insert(QStringLiteral("expressions"), QStringLiteral("ht.T"));
            n.props.insert(QStringLiteral("format"), QStringLiteral("npy"));
            n.props.insert(QStringLiteral("nx"), 9);
            n.props.insert(QStringLiteral("ny"), 7);
            n.props.insert(QStringLiteral("nz"), 5);
        }, QStringLiteral("grid"));
        const QString npy = doc->runExport(grid);
        QVERIFY2(npy.endsWith(QStringLiteral(".npy")) && QFileInfo::exists(npy), qPrintable(doc->messages().join(QStringLiteral("\n"))));
        QVERIFY(QFileInfo::exists(QFileInfo(npy).absoluteDir().filePath(QFileInfo(npy).completeBaseName() + QStringLiteral("_xy.npy"))));
        QVERIFY(!doc->solutionStale());
    }

    /// Claude's fem tools on a 3D model: one made whole as JSON (the heated
    /// block, T linear in z), its geometry built - its domain, faces and
    /// edges, a rule's pick -, meshed and solved; its values at points,
    /// along a line, over its domain, a face and an edge, its globals, named
    /// and against a reference, each exact; an edit of its height built and
    /// solved again; its format and variables; saved; what is a 2D model's
    /// refused, said.
    void theToolsOnA3DModel()
    {
        const QString file = path(QStringLiteral("tools3d.qfem3d"));
        QFile::remove(file);
        // A blank one first, by 'space': a 3D tab.
        QJsonObject r = call(QStringLiteral("fem_model"), {{"action", "new"}, {"space", "3D"}});
        QVERIFY2(!failed(r) && json(r).value(QStringLiteral("space")).toString() == QLatin1String("3D") && front(), qPrintable(text(r)));
        r = call(QStringLiteral("fem_model"), {{"action", "new"}, {"model", heatedBlock()}, {"path", "tools3d"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(file));
        Multiphysics3DDoc* doc = front();
        QVERIFY(doc && doc->getDocName() == file);
        // Its geometry: one block's domain, its six faces - two floors - and
        // twelve edges; the rule {of blk, side top} picks its top floor.
        r = call(QStringLiteral("fem_build"), {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value(QStringLiteral("domains")).toInt(), 1);
        QCOMPARE(o.value(QStringLiteral("faces")).toInt(), 6);
        QCOMPARE(o.value(QStringLiteral("edges")).toInt(), 12);
        QCOMPARE(o.value(QStringLiteral("points")).toInt(), 8);
        QCOMPARE(o.value(QStringLiteral("bounds")).toArray().at(5).toString(), QStringLiteral("0.5"));
        int floors = 0, longEdge = 0;
        for (const QJsonValue& f : o.value(QStringLiteral("face list")).toArray()) floors += f.toObject().value(QStringLiteral("kind")).toString() == QLatin1String("floor");
        QCOMPARE(floors, 2);
        for (const QJsonValue& e : o.value(QStringLiteral("edge list")).toArray())
            if (e.toObject().value(QStringLiteral("length")).toString() == QLatin1String("2") && !longEdge) longEdge = e.toObject().value(QStringLiteral("number")).toInt();
        QVERIFY(longEdge > 0);
        QJsonArray blk;
        blk.append(QStringLiteral("blk"));
        const QJsonObject topRule{{"of", blk}, {"side", "top"}};
        r = call(QStringLiteral("fem_describe"), {{"path", file}, {"what", "geometry"}, {"selection", topRule}, {"level", "boundary"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray picked = json(r).value(QStringLiteral("selection picks")).toObject().value(QStringLiteral("numbers")).toArray();
        QCOMPARE(picked.size(), 1);
        const QJsonObject topFace = json(r).value(QStringLiteral("face list")).toArray().at(picked.at(0).toInt() - 1).toObject();
        QVERIFY2(topFace.value(QStringLiteral("kind")).toString() == QLatin1String("floor")
                     && topFace.value(QStringLiteral("bounds")).toArray().at(2).toString() == QLatin1String("0.5"),
                 qPrintable(QJsonDocument(topFace).toJson()));
        // Its mesh, its prisms in layers.
        r = call(QStringLiteral("fem_build"), {{"path", file}, {"what", "mesh"}});
        QVERIFY2(!failed(r) && json(r).value(QStringLiteral("prisms")).toInt() > 0 && json(r).value(QStringLiteral("layers")).toInt() >= 1, qPrintable(text(r)));
        // Solved: its globals; ht.Tmax 300 K + q h / k = 302.5 K.
        r = call(QStringLiteral("fem_solve"), {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        o = json(r);
        QVERIFY2(o.value(QStringLiteral("globals")).toObject().contains(QStringLiteral("ht.Tmax")) && o.value(QStringLiteral("unknowns")).toInt() > 0, qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "ht.Tmax; dT = ht.Tmax - ht.Tmin"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QList<double> v = si(r);
        QCOMPARE(v.size(), 2);
        QVERIFY2(std::abs(v[0] - 302.5) < 1e-6 && std::abs(v[1] - 2.5) < 1e-6, qPrintable(text(r)));
        QCOMPARE(json(r).value(QStringLiteral("values")).toArray().at(1).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("dT"));
        // At points (x, y, z in mm), along a line, over the domain, its top
        // and an edge.
        QJsonArray at;
        at.append(QJsonArray{1.0, 0.5, 0.25});
        at.append(QJsonArray{0.1, 0.9, 0.5});
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "T"}, {"at", at}});
        v = si(r);
        QVERIFY2(v.size() == 2 && std::abs(v[0] - 301.25) < 1e-6 && std::abs(v[1] - 302.5) < 1e-6, qPrintable(text(r)));
        // Several, split by ';', each named if wanted (the DMG's drive of
        // 4b238d6: they were one expression, refused).
        QJsonArray one;
        one.append(QJsonArray{1.0, 0.5, 0.25});
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "T; rise = T - 300[K]"}, {"at", one}});
        v = si(r);
        QVERIFY2(v.size() == 2 && std::abs(v[0] - 301.25) < 1e-6 && std::abs(v[1] - 1.25) < 1e-6, qPrintable(text(r)));
        QVERIFY2(json(r).value(QStringLiteral("values")).toArray().at(1).toObject().value(QStringLiteral("name")).toString().startsWith(QStringLiteral("rise at (1")),
                 qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "T; 2*T"}, {"over", "boundary"}, {"selection", topRule}, {"kind", "average"}});
        v = si(r);
        QVERIFY2(v.size() == 2 && std::abs(v[0] - 302.5) < 1e-6 && std::abs(v[1] - 605) < 1e-6, qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "T"},
                                                  {"along", QJsonObject{{"from", QJsonArray{1.0, 0.5, 0.0}}, {"to", QJsonArray{1.0, 0.5, 0.5}}, {"points", 5}}}});
        v = si(r);
        QVERIFY2(v.size() == 5, qPrintable(text(r)));
        for (int i = 0; i < 5; ++i) QVERIFY2(std::abs(v[i] - (300 + 2.5 * i / 4)) < 1e-6, qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "1"}, {"over", "domain"}});
        v = si(r);
        QVERIFY2(v.size() == 1 && std::abs(v[0] - 1e-9) < 1e-18, qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "T"}, {"over", "boundary"}, {"selection", topRule}, {"kind", "average"}});
        v = si(r);
        QVERIFY2(v.size() == 1 && std::abs(v[0] - 302.5) < 1e-6, qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "1"}, {"over", "edge"}, {"selection", QJsonObject{{"numbers", QJsonArray{longEdge, longEdge}}}}});
        v = si(r);
        QVERIFY2(v.size() == 1 && std::abs(v[0] - 2e-3) < 1e-12, qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "T"}, {"over", "domain"}, {"kind", "maximum"}});
        v = si(r);
        QVERIFY2(v.size() == 1 && std::abs(v[0] - 302.5) < 1e-6, qPrintable(text(r)));
        // Against a reference: its difference.
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "ht.Tmax"}, {"reference", 302.5}});
        o = json(r);
        QVERIFY2(!failed(r) && o.value(QStringLiteral("against the reference")).toObject().value(QStringLiteral("values measured")).toInt() == 1, qPrintable(text(r)));
        QVERIFY(std::abs(o.value(QStringLiteral("values")).toArray().at(0).toObject().value(QStringLiteral("difference")).toDouble(1)) < 1e-6);
        // A field without a place, a 2D model's arguments, another study:
        // said.
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "T"}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("varies over the model")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "T"}, {"frequency", "1[GHz]"}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("'frequency' is a 2D model's")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "ht.Tmax"}, {"study", "nostudy"}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("fem_solve study=nostudy")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_build"), {{"path", file}, {"until", "blk1"}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("built whole")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_converge"), {{"path", file}, {"values", QJsonArray{QStringLiteral("ht.Tmax"), QStringLiteral("ht.Tmin")}}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("2D models alone")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_stop"), {{"path", file}});
        QVERIFY2(!failed(r) && !json(r).value(QStringLiteral("stopped")).toBool(), qPrintable(text(r)));
        // The model as the tools describe it: 3D, solved; its variables.
        r = call(QStringLiteral("fem_describe"), {{"path", file}});
        o = json(r);
        QVERIFY2(o.value(QStringLiteral("space")).toString() == QLatin1String("3D") && o.value(QStringLiteral("solved")).isObject()
                     && o.value(QStringLiteral("geometry")).toString() == QLatin1String("1 domains, 6 faces, 12 edges, 8 points"),
                 qPrintable(text(r)));
        r = call(QStringLiteral("fem_describe"), {{"path", file}, {"what", "variables"}});
        QVERIFY2(!failed(r) && text(r).contains(QStringLiteral("\"ht.Tmax\"")), qPrintable(text(r)));
        // Its format: a 3D model's kinds - the solids, an Extrude's sketch -,
        // not the plane's alone; its file's paragraph first.
        r = call(QStringLiteral("fem_describe"), {{"path", file}, {"what", "format"}, {"text", "all"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList types;
        for (const QJsonValue& k : json(r).value(QStringLiteral("kinds")).toArray()) types << k.toObject().value(QStringLiteral("type")).toString();
        QVERIFY2(types.contains(QStringLiteral("box")) && types.contains(QStringLiteral("plotgroup3d")) && types.contains(QStringLiteral("rectangle"))
                     && !types.contains(QStringLiteral("contour")) && !types.contains(QStringLiteral("plotgroup")),
                 qPrintable(types.join(QStringLiteral(", "))));
        QVERIFY(json(r).value(QStringLiteral("format")).toString().startsWith(QStringLiteral("A 3D model (.qfem3d")));
        r = call(QStringLiteral("fem_describe"), {{"path", file}, {"what", "format"}, {"space", "2D"}, {"text", "none"}});
        types.clear();
        for (const QJsonValue& k : json(r).value(QStringLiteral("kinds")).toArray()) types << k.toObject().value(QStringLiteral("type")).toString();
        QVERIFY(types.contains(QStringLiteral("contour")) && types.contains(QStringLiteral("box")));
        // An edit: a block twice as high, built again, 305 K at its top.
        QString blkTag;
        for (const FNode& c : doc->model().geometry().children)
            if (c.label == QLatin1String("blk")) blkTag = c.tag;
        QVERIFY(!blkTag.isEmpty());
        r = call(QStringLiteral("fem_edit"), {{"path", file}, {"action", "set"}, {"tag", blkTag}, {"props", QJsonObject{{"height", "1"}}}});
        QVERIFY2(!failed(r) && text(r).contains(QStringLiteral("being built again")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_build"), {{"path", file}});
        QVERIFY2(!failed(r) && json(r).value(QStringLiteral("bounds")).toArray().at(5).toString() == QLatin1String("1"), qPrintable(text(r)));
        r = call(QStringLiteral("fem_solve"), {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        v = si(call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "ht.Tmax"}}));
        QVERIFY2(v.size() == 1 && std::abs(v[0] - 305) < 1e-6, qPrintable(QString::number(v.value(0))));
        // A 2D model's tab stays the 2D tools': a 3D one is not made of it.
        r = call(QStringLiteral("fem_edit"), {{"path", file}, {"action", "set"}, {"tag", doc->model().component().tag}, {"props", QJsonObject{{"space", "2D"}}}});
        QVERIFY2(failed(r) && doc->model().is3D(), qPrintable(text(r)));
        // Saved, as another file.
        r = call(QStringLiteral("fem_model"), {{"path", file}, {"action", "save"}, {"as", "tools3d_copy"}});
        QVERIFY2(!failed(r) && QFileInfo::exists(path(QStringLiteral("tools3d_copy.qfem3d"))), qPrintable(text(r)));
        QFile saved(path(QStringLiteral("tools3d_copy.qfem3d")));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().contains("qucs-s multiphysics 3d 1"));
    }

    /// Light through a 3D model by the tools: a straight wire's eigenmode
    /// expansion solved over three wavelengths - fem_solve's optics (its
    /// S-parameter ports, |S21|² 0 dB at each), fem_evaluate at a wavelength
    /// (eme.S21dB, the index there), the Wavelength box of the tab, and the
    /// Touchstone export: an .s2p of three frequencies, rising, its ports
    /// named.
    void theToolsSolveAnEigenmodeExpansion()
    {
        const QString file = path(QStringLiteral("wire_eme.qfem3d"));
        QFile::remove(file);
        QJsonObject r = call(QStringLiteral("fem_model"), {{"action", "new"}, {"model", straightWire()}, {"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Multiphysics3DDoc* doc = front();
        QVERIFY(doc);
        r = call(QStringLiteral("fem_solve"), {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(doc->solution() && doc->solution()->spectral && doc->solution()->snapshotCount() == 3);
        const QJsonArray optics = json(r).value(QStringLiteral("optics")).toArray();
        QCOMPARE(optics.size(), 1);
        const QJsonObject o = optics.at(0).toObject();
        QCOMPARE(o.value(QStringLiteral("S-parameter ports")).toArray().size(), 2);
        QCOMPARE(o.value(QStringLiteral("wavelengths")).toArray().size(), 3);
        const QString s21 = o.value(QStringLiteral("wavelengths")).toArray().at(1).toObject().value(QStringLiteral("fed")).toObject().value(QStringLiteral("1")).toArray().at(1).toString();
        QVERIFY2(s21.startsWith(QStringLiteral("S21 ")) && s21.contains(QStringLiteral(" dB")), qPrintable(s21));
        // (No S-parameter among the globals listed: they are optics'.)
        const QString tag = doc->model().physics().front()->tag;
        QVERIFY(json(r).value(QStringLiteral("globals")).toObject().contains(tag + QStringLiteral(".neff_1_TE0")));
        QVERIFY(!json(r).value(QStringLiteral("globals")).toObject().contains(tag + QStringLiteral(".S21")));
        // At a wavelength: |S21| in dB nought, the index of 1.55 µm.
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", tag + QStringLiteral(".S21dB; ") + tag + QStringLiteral(".lambda0")}, {"frequency", "1550[nm]"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QList<double> v = si(r);
        QVERIFY2(v.size() == 2 && std::abs(v[0]) < 1e-6 && std::abs(v[1] - 1.55e-6) < 1e-12, qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", tag + QStringLiteral(".neff_1_TE0")}, {"all", true}});
        v = si(r);
        QVERIFY2(v.size() == 3 && v[0] > v[1] && v[1] > v[2], qPrintable(text(r)));
        QVERIFY2(text(r).contains(QStringLiteral("λ = ")), qPrintable(text(r)));
        // Its Touchstone file.
        QString exportTag;
        if (const FNode* ex = doc->model().results().child(QStringLiteral("exports")))
            for (const FNode& c : ex->children)
                if (c.type == QLatin1String("touchstone")) exportTag = c.tag;
        QVERIFY(!exportTag.isEmpty());
        r = call(QStringLiteral("fem_export"), {{"path", file}, {"export", exportTag}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString written = path(QStringLiteral("wire_eme_%1.s2p").arg(exportTag));
        QFile ts(written);
        QVERIFY2(ts.open(QIODevice::ReadOnly), qPrintable(written));
        const QStringList lines = QString::fromUtf8(ts.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QStringList data;
        bool option = false;
        for (const QString& l : lines) {
            if (l.startsWith(QLatin1Char('#'))) option = l.trimmed() == QLatin1String("# Hz S RI R 50");
            else if (!l.startsWith(QLatin1Char('!'))) data << l;
        }
        QVERIFY(option);
        QCOMPARE(data.size(), 3);
        QVERIFY(data[0].split(QLatin1Char(' ')).first().toDouble() < data[2].split(QLatin1Char(' ')).first().toDouble());
        QVERIFY(lines.join(QLatin1Char('\n')).contains(QStringLiteral("! Port 2: optical port 2's TE0, at the end")));
        // Its spectrum: written when it was solved, shown by fem_plot - a
        // dataset of S11dB, S21dB... against lambda, a display of them.
        const QString dat = path(QStringLiteral("wire_eme_spectrum.dat"));
        QVERIFY(QFileInfo::exists(dat));
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"spectrum", true}});
        QVERIFY2(!failed(r) && json(r).value(QStringLiteral("spectrum")).toString().endsWith(QStringLiteral("wire_eme_spectrum.dat")), qPrintable(text(r)));
        QFile spectrum(dat);
        QVERIFY(spectrum.open(QIODevice::ReadOnly));
        const QByteArray content = spectrum.readAll();
        QVERIFY(content.contains("<indep lambda 3>") && content.contains("<dep S21dB lambda>") && content.contains("<dep neff_1_TE0 lambda>"));
        QVERIFY(QFileInfo::exists(path(QStringLiteral("wire_eme_spectrum.dpl"))));
    }

    /// Claude's plots and exports of a 3D model: a 3D Plot Group made of an
    /// expression - its surface, a slice, an isosurface and a clip - and
    /// shown, its legend the field's range; the camera; the mesh shown; a
    /// VTU export added and written; a sweep solved, each point's value.
    void theToolsPlotAndExportA3DModel()
    {
        const QString file = path(QStringLiteral("tools3d_plot.qfem3d"));
        QFile::remove(file);
        QJsonObject r = call(QStringLiteral("fem_model"), {{"action", "new"}, {"model", heatedBlock()}, {"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Multiphysics3DDoc* doc = front();
        QVERIFY(doc);
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"expression", "T"}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("fem_solve first")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_solve"), {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonArray slices;
        slices.append(QJsonObject{{"z", 0.25}});
        QJsonArray levels;
        levels.append(301.0);
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"expression", "T"}, {"title", "The block's T"}, {"slices", slices},
                                              {"isosurfaces", levels}, {"clip", QJsonObject{{"x", 1.5}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value(QStringLiteral("shown")).toString(), QStringLiteral("The block's T"));
        QVERIFY2(!o.contains(QStringLiteral("problems")), qPrintable(text(r)));
        const QJsonArray range = o.value(QStringLiteral("legend")).toObject().value(QStringLiteral("SI")).toArray();
        QVERIFY2(range.size() == 2 && std::abs(range.at(0).toDouble() - 300) < 1e-6 && std::abs(range.at(1).toDouble() - 302.5) < 1e-6, qPrintable(text(r)));
        const QString group = o.value(QStringLiteral("plot group")).toString();
        const FNode* g = doc->model().find(group);
        QVERIFY(g && g->type == QLatin1String("plotgroup3d") && g->text(QStringLiteral("clip")) == QLatin1String("x")
                && g->text(QStringLiteral("clipat")) == QLatin1String("1.5"));
        QStringList kinds;
        for (const FNode& c : g->children) kinds << c.type;
        QCOMPARE(kinds, QStringList({QStringLiteral("surface"), QStringLiteral("slice"), QStringLiteral("isosurface")}));
        QVERIFY(g->children[1].text(QStringLiteral("plane")) == QLatin1String("z") && g->children[1].text(QStringLiteral("at")) == QLatin1String("0.25"));
        QVERIFY(g->children[2].text(QStringLiteral("entry")) == QLatin1String("values") && g->children[2].text(QStringLiteral("values")) == QLatin1String("301"));
        QCOMPARE(doc->show(), Multiphysics3DDoc::Show::Plot);
        QCOMPARE(doc->view()->clipAxis(), 0);
        // One step of Undo.
        QVERIFY(doc->undoStack()->undoText().contains(QStringLiteral("Plot T")));
        // Without its surface: a slice alone, two planes across x - the
        // rise above 300 K, its legend 0 to 2.5 K.
        QJsonArray across;
        across.append(QJsonObject{{"x", QJsonArray{0.5, 1.5}}});
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"expression", "T - 300[K]"}, {"surface", false}, {"slices", across}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray rise = json(r).value(QStringLiteral("legend")).toObject().value(QStringLiteral("SI")).toArray();
        QVERIFY2(rise.size() == 2 && std::abs(rise.at(0).toDouble()) < 1e-6 && std::abs(rise.at(1).toDouble() - 2.5) < 1e-6, qPrintable(text(r)));
        const FNode* alone = doc->model().find(json(r).value(QStringLiteral("plot group")).toString());
        QVERIFY(alone && alone->children.size() == 1 && alone->children[0].type == QLatin1String("slice"));
        QCOMPARE(alone->children[0].text(QStringLiteral("plane")), QStringLiteral("x"));
        QCOMPARE(alone->children[0].text(QStringLiteral("at")), QStringLiteral("0.5, 1.5"));
        // The camera from above; the mesh.
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"camera", "top"}});
        QVERIFY2(!failed(r) && json(r).value(QStringLiteral("camera")).toObject().value(QStringLiteral("elevation")).toDouble() == 90, qPrintable(text(r)));
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"show", "mesh"}, {"azimuth", 30}, {"elevation", 20}, {"zoom", 2}});
        QVERIFY2(!failed(r) && doc->show() == Multiphysics3DDoc::Show::Mesh, qPrintable(text(r)));
        QCOMPARE(doc->view()->azimuth(), 30.0);
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"plot", group}});
        QVERIFY2(!failed(r) && doc->show() == Multiphysics3DDoc::Show::Plot && doc->plotGroup() == group, qPrintable(text(r)));
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"expression", "T"}, {"contours", true}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("'contours' is a 2D model's")), qPrintable(text(r)));
        // A VTU export added by fem_edit and written.
        r = call(QStringLiteral("fem_export"), {{"path", file}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("none yet")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_edit"), {{"path", file}, {"action", "add"}, {"parent", "exports"}, {"type", "vtu"}, {"props", QJsonObject{{"expressions", "T"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QString vtu;
        for (const FNode& c : doc->model().results().child(QStringLiteral("exports"))->children)
            if (c.type == QLatin1String("vtu")) vtu = c.tag;
        QVERIFY(!vtu.isEmpty());
        r = call(QStringLiteral("fem_export"), {{"path", file}, {"export", vtu}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString written = json(r).value(QStringLiteral("written")).toString();
        QVERIFY2(written.endsWith(QStringLiteral(".vtu")) && QFileInfo::exists(written), qPrintable(text(r)));
        QVERIFY2(json(r).value(QStringLiteral("begins")).toString().contains(QStringLiteral("<VTKFile")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_export"), {{"path", file}, {"export", vtu}, {"copy", true}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("'copy'")), qPrintable(text(r)));
        // A sweep of q: each point's ht.Tmax, 302.5 and 305 K.
        QString study;
        for (const FNode& c : doc->model().root.children)
            if (c.type == QLatin1String("study")) study = c.tag;
        QJsonArray rows;
        rows.append(QJsonArray{QStringLiteral("q"), QStringLiteral("1e4, 2e4"), QStringLiteral("W/m^2")});
        r = call(QStringLiteral("fem_edit"), {{"path", file}, {"action", "add"}, {"parent", study}, {"type", "sweep"}, {"props", QJsonObject{{"parameters", rows}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call(QStringLiteral("fem_solve"), {{"path", file}});
        QVERIFY2(!failed(r) && json(r).value(QStringLiteral("sweep")).toArray().size() == 2, qPrintable(text(r)));
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "ht.Tmax"}, {"all", true}});
        QList<double> v = si(r);
        QVERIFY2(v.size() == 2 && std::abs(v[0] - 302.5) < 1e-6 && std::abs(v[1] - 305) < 1e-6, qPrintable(text(r)));
        QVERIFY2(json(r).value(QStringLiteral("values")).toArray().at(0).toObject().value(QStringLiteral("name")).toString().startsWith(QStringLiteral("q = ")),
                 qPrintable(text(r)));
        v = si(call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "ht.Tmax"}, {"point", 1}}));
        QVERIFY(v.size() == 1 && std::abs(v[0] - 302.5) < 1e-6);
        r = call(QStringLiteral("fem_evaluate"), {{"path", file}, {"expression", "ht.Tmax"}, {"point", 3}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("the sweep has 2 points")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_plot"), {{"path", file}, {"plot", group}, {"point", 1}});
        QVERIFY2(!failed(r) && std::abs(json(r).value(QStringLiteral("legend")).toObject().value(QStringLiteral("SI")).toArray().at(1).toDouble() - 302.5) < 1e-6,
                 qPrintable(text(r)));
    }

    /// What a solve will hold and take, said before it: Build Mesh says each
    /// physics' estimate and the limit; fem_build gives them; Multiphysics 3D
    /// > Settings keeps the limit (half of this machine's by default); a
    /// solve past it is not solved - an earlier solution not taken for it -
    /// and forced, solved; a solver for this solve alone; a 2D model's
    /// fem_solve refuses both, said.
    void estimatesAndTheLimit()
    {
        Multiphysics3DDoc::setMemoryLimitGB(-1);
        QVERIFY(std::abs(Multiphysics3DDoc::memoryLimit() - physicalMemory() / 2) < 1);
        const QString file = path(QStringLiteral("limits3d.qfem3d"));
        QFile::remove(file);
        QJsonObject r = call(QStringLiteral("fem_model"), {{"action", "new"}, {"model", heatedBlock()}, {"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Multiphysics3DDoc* doc = front();
        QVERIFY(doc && doc->waitForIdle());
        // Build Mesh (the menu's): its estimate said, within the limit.
        QAction* mesh = app->findChild<QAction*>(QStringLiteral("mp3BuildMeshAction"));
        QVERIFY(mesh);
        mesh->trigger();
        QVERIFY(doc->waitForIdle(60000));
        const QString said = doc->messages().join(QStringLiteral("\n"));
        QVERIFY2(said.contains(QStringLiteral("Estimated: ht (order 1): ")) && said.contains(QStringLiteral("Within the memory limit of")), qPrintable(said));
        QCOMPARE(int(doc->estimates().size()), 1);
        QVERIFY(doc->estimates().front().unknowns == qint64(doc->mesh()->nodes.size()));
        // fem_build's: the same, with the limit.
        r = call(QStringLiteral("fem_build"), {{"path", file}, {"what", "mesh"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject estimate = json(r).value(QStringLiteral("estimates")).toArray().at(0).toObject();
        QCOMPARE(estimate.value(QStringLiteral("physics")).toString(), QStringLiteral("ht"));
        QCOMPARE(estimate.value(QStringLiteral("unknowns")).toDouble(), double(doc->estimates().front().unknowns));
        QCOMPARE(estimate.value(QStringLiteral("nonzeros")).toDouble(), double(doc->estimates().front().nonzeros));
        QVERIFY2(json(r).value(QStringLiteral("memory limit")).toString().contains(QStringLiteral("Multiphysics 3D > Settings")), qPrintable(text(r)));
        QVERIFY(!json(r).contains(QStringLiteral("past the limit")));
        // The Settings: its value the limit; OK keeps another; the button
        // half of this machine's again.
        QAction* settings = app->findChild<QAction*>(QStringLiteral("mp3SettingsAction"));
        QVERIFY(settings);
        settings->trigger();
        QDialog* dialog = nullptr;
        QTRY_VERIFY((dialog = app->findChild<QDialog*>(QStringLiteral("multiphysics3DSettings"))) != nullptr);
        auto* limit = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("mp3MemoryLimit"));
        QVERIFY(limit);
        QVERIFY(std::abs(limit->value() - physicalMemory() / 2 / 1073741824.0) < 0.06);
        limit->setValue(3.5);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QVERIFY(std::abs(Multiphysics3DDoc::memoryLimit() - 3.5 * 1073741824.0) < 1);
        settings->trigger();
        QTRY_VERIFY((dialog = app->findChild<QDialog*>(QStringLiteral("multiphysics3DSettings"))) != nullptr && dialog->isVisible());
        dialog->findChild<QPushButton*>(QStringLiteral("mp3MemoryDefault"))->click();
        QVERIFY(std::abs(dialog->findChild<QDoubleSpinBox*>(QStringLiteral("mp3MemoryLimit"))->value() - physicalMemory() / 2 / 1073741824.0) < 0.06);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        QVERIFY(std::abs(Multiphysics3DDoc::memoryLimit() - 3.5 * 1073741824.0) < 1);
        // Solved within it, then refused past a limit under its estimate -
        // its earlier solution not taken for it -, then forced.
        r = call(QStringLiteral("fem_solve"), {{"path", file}});
        QVERIFY2(!failed(r) && json(r).value(QStringLiteral("estimates")).toArray().size() == 1, qPrintable(text(r)));
        Multiphysics3DDoc::setMemoryLimitGB(doc->estimates().front().bytes() / 2 / 1073741824.0);
        r = call(QStringLiteral("fem_solve"), {{"path", file}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("past the limit of")) && text(r).contains(QStringLiteral("a coarser mesh")), qPrintable(text(r)));
        QVERIFY(doc->messages().join(QStringLiteral("\n")).contains(QStringLiteral("Not solved: it would take some")));
        r = call(QStringLiteral("fem_solve"), {{"path", file}, {"force", true}});
        QVERIFY2(!failed(r) && text(r).contains(QStringLiteral("Solved past the limit, as asked (force)")), qPrintable(text(r)));
        Multiphysics3DDoc::setMemoryLimitGB(-1);
        // A direct solve for this solve alone: the model left as it is.
        r = call(QStringLiteral("fem_solve"), {{"path", file}, {"solver", "direct"}});
        QVERIFY2(!failed(r) && text(r).contains(QStringLiteral("Cholesky (METIS order)")), qPrintable(text(r)));
        QVERIFY(!doc->model().toJson().contains("\"direct\""));
        r = call(QStringLiteral("fem_solve"), {{"path", file}, {"solver", "lu"}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("'solver' is auto, direct or iterative")), qPrintable(text(r)));
        // A 2D model's fem_solve: neither taken.
        QVERIFY(!failed(call(QStringLiteral("fem_model"), {{"action", "new"}})));
        r = call(QStringLiteral("fem_solve"), {{"solver", "direct"}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("'solver' is a 3D model's")), qPrintable(text(r)));
        r = call(QStringLiteral("fem_solve"), {{"force", true}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("'force' is a 3D model's")), qPrintable(text(r)));
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
        // (Newton's method in 3D too: a field that reads itself.)
        const QStringList timeMethods = valuesOf(qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("nonlinear"))));
        QVERIFY2(timeMethods.contains(QStringLiteral("newton")) && timeMethods.contains(QStringLiteral("picard")), qPrintable(timeMethods.join(QStringLiteral(", "))));
        panel()->selectNode(step);
        QVERIFY(panel()->editorFor(QStringLiteral("solver")) != nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("adaptive")) == nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("lightloop")) == nullptr);
        const QStringList methods = valuesOf(qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("nonlinear"))));
        QVERIFY2(methods.contains(QStringLiteral("picard")) && methods.contains(QStringLiteral("newton")), qPrintable(methods.join(QStringLiteral(", "))));
        QVERIFY(qobject_cast<QListWidget*>(panel()->editorFor(QStringLiteral("physics"))) != nullptr);
        // A heat model's solid of the user's k: a tensor in the model's
        // axes offered, its zz and (symmetric) xz and yz a 3D model's.
        QString solid;
        for (const FNode& c : doc->model().find(ht)->children)
            if (c.type == QLatin1String("heat/solid")) solid = c.tag;
        doc->editNode(solid, [](FNode& n) { n.props.insert(QStringLiteral("source"), QStringLiteral("user")); }, QStringLiteral("user"));
        panel()->selectNode(solid);
        QCOMPARE(valuesOf(qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("anisotropy")))),
                 QStringList({QStringLiteral("isotropic"), QStringLiteral("diagonal"), QStringLiteral("symmetric")}));
        doc->editNode(solid, [](FNode& n) { n.props.insert(QStringLiteral("anisotropy"), QStringLiteral("symmetric")); }, QStringLiteral("tensor"));
        panel()->selectNode(solid);
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("kzz")) != nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("kxz")) != nullptr && panel()->editorFor(QStringLiteral("kyz")) != nullptr);
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
