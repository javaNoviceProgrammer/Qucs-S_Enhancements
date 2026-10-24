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
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QCompleter>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
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
#include "fem_material_plot.h"
#include "fem_materials.h"
#include "fem_modeprofile.h"
#include "multiphysicspanel.h"
#include "fem_sparse.h"
#include "multiphysicssettings.h"
#include "dataimport.h"
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

/// How long a solve is waited for: a sanitized Debug build's dense eigen
/// solves and Newton's steps are tens of times slower, more on CI's runners
/// (ctest's 900 s guards the whole).
int jobWait()
{
    return slowBuild() ? 600000 : 120000;
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
            if (auto* m = dynamic_cast<MultiphysicsDoc*>(d)) m->waitForJob(jobWait());
            d->setDocChanged(false);   // (a data display too: no Save dialog)
        }
        app->closeAllFiles();
    }

    // ---- The panel, the tab

    // A tab of the left dock: with no model in front, it offers a new
    // one, to open one, the examples. (Its place was asserted as a number
    // until the Workspaces panel went in above Projects and moved it, and
    // as the last tab until the RF 2.5D panel went in after it; the page is
    // asked for by page now, which is what SidePage is for.)
    void thePanelHasItsOwnDockTab()
    {
        QVERIFY(panel() != nullptr);
        auto* tabs = panel()->parentWidget() ? qobject_cast<QTabWidget*>(panel()->parentWidget()->parentWidget()) : nullptr;
        QVERIFY(tabs != nullptr);
        QCOMPARE(tabs->tabText(tabs->indexOf(panel())), QStringLiteral("Multiphysics"));
        QCOMPARE(tabs->indexOf(panel()), app->sidePage(QucsApp::SidePage::Multiphysics));
        QCOMPARE(panel()->pages()->currentIndex(), 0);
        QVERIFY(panel()->exampleList()->count() >= 3);
        app->showMultiphysicsPanel();
        QCOMPARE(app->currentSidePageName(), QStringLiteral("multiphysics"));
        shoot(app, "00-start");
        // The Multiphysics menu, before View.
        QMenu* menu = app->multiphysicsMenu();
        QVERIFY(menu != nullptr);
        QVERIFY(app->menuBar()->actions().contains(menu->menuAction()));
    }

    // The Multiphysics menu's Settings…: the Graphics view drawn with the
    // GPU (Qt's RHI) or the CPU - the setting the view's own menu sets
    // (Viewers/multiphysicsGpu), kept on OK, not on Cancel; what keeps the GPU from
    // drawing here said (offscreen: no GPU backend; QUCS_GPU=0). The item no
    // role: on macOS "Settings…" would be taken to the application's menu.
    void theSettingsHaveTheGpu()
    {
        QAction* item = app->multiphysicsMenu()->findChild<QAction*>(QStringLiteral("mpSettingsAction"));
        QVERIFY(item != nullptr);
        QCOMPARE(item->text(), QStringLiteral("Settings…"));
        QCOMPARE(item->menuRole(), QAction::NoRole);
        QVERIFY(app->multiphysicsMenu()->actions().contains(item));
        // (Opened from the menu - modal: closed from inside.)
        bool opened = false;
        QTimer::singleShot(0, this, [&opened] {
            if (auto* d = qobject_cast<MultiphysicsSettingsDialog*>(QApplication::activeModalWidget())) {
                opened = d->gpuBox() != nullptr;
                d->reject();
            }
        });
        item->trigger();
        QVERIFY(opened);
        const bool before = MultiphysicsSettingsDialog::gpuWanted();
        {
            MultiphysicsSettingsDialog d(app);
            QCOMPARE(d.gpuBox()->isChecked(), before);
            QCOMPARE(d.gpuBox()->text(), QStringLiteral("Draw with the GPU"));
            if (!GraphicsView::gpuAvailable())
                QVERIFY2(d.gpuStatus()->text().contains(QLatin1String("No GPU backend here")), qPrintable(d.gpuStatus()->text()));
            else
                QCOMPARE(d.gpuStatus()->text(), before ? QStringLiteral("The GPU draws the Graphics view.") : QStringLiteral("The CPU draws the Graphics view."));
            d.gpuBox()->setChecked(!before);
            d.reject();   // (Cancel: nothing kept.)
        }
        QCOMPARE(MultiphysicsSettingsDialog::gpuWanted(), before);
        {
            MultiphysicsSettingsDialog d(app);
            d.gpuBox()->setChecked(false);
            d.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(d.result(), int(QDialog::Accepted));
        }
        QVERIFY(!MultiphysicsSettingsDialog::gpuWanted());
        // (Every view the CPU's: none draws with the GPU after.)
        for (QWidget* w : QApplication::allWidgets())
            if (auto* view = qobject_cast<GraphicsView*>(w)) QVERIFY(!view->gpu());
        // (A view open, where there is a GPU: drawn by it again on OK.)
        GraphicsView own;
        own.resize(200, 150);
        {
            MultiphysicsSettingsDialog d(app);
            QVERIFY(!d.gpuBox()->isChecked());
            d.gpuBox()->setChecked(true);
            d.apply();
        }
        QVERIFY(MultiphysicsSettingsDialog::gpuWanted());
        QCOMPARE(own.gpu(), GraphicsView::gpuAvailable());
        // QUCS_GPU=0 said, whatever the setting.
        qputenv("QUCS_GPU", "0");
        {
            MultiphysicsSettingsDialog d(app);
            QVERIFY2(d.gpuStatus()->text().contains(QLatin1String("QUCS_GPU=0 is set")), qPrintable(d.gpuStatus()->text()));
        }
        qunsetenv("QUCS_GPU");
    }

    // The settings' sparse direct solver: Automatic by default; Eigen's
    // chosen, kept and given to the solver on OK (not on Cancel), each
    // solve's line saying which ran; QUCS_FEM_SOLVER said over it; this
    // macOS's LU said.
    void theSettingsHaveTheSolver()
    {
        qunsetenv("QUCS_FEM_SOLVER");
        QCOMPARE(MultiphysicsSettingsDialog::solverWanted(), 0);
        {
            MultiphysicsSettingsDialog d(app);
            QComboBox* box = d.solverBox();
            QVERIFY(box != nullptr);
            QCOMPARE(box->count(), 3);
            QCOMPARE(box->currentIndex(), 0);
            QVERIFY(box->itemText(0).startsWith(QLatin1String("Automatic")));
            const QString status = d.solverStatusLabel()->text();
            qInfo("solver: %s", qPrintable(status));
            QVERIFY2(acceleratedLuHindrance().isEmpty() ? status.contains(QLatin1String("Accelerate's LU and Cholesky from 20,000"))
                                                        : status.contains(QLatin1String("Eigen's LU")),
                     qPrintable(status));
            box->setCurrentIndex(1);
            // (A build without Accelerate - Linux's, Windows' - says so.)
            QCOMPARE(d.solverStatusLabel()->text(), accelerateInBuild() ? QStringLiteral("Eigen's LU and Cholesky (LDLT) at every size.")
                                                                        : QStringLiteral("Eigen's LU and Cholesky (LDLT) at every size: this build "
                                                                                         "has no Accelerate."));
            d.reject();
        }
        QCOMPARE(MultiphysicsSettingsDialog::solverWanted(), 0);
        QVERIFY(directSolver() == DirectSolver::Automatic);
        {
            MultiphysicsSettingsDialog d(app);
            d.solverBox()->setCurrentIndex(1);
            d.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        }
        QCOMPARE(MultiphysicsSettingsDialog::solverWanted(), 1);
        QVERIFY(directSolver() == DirectSolver::Eigen);
        // A solve says it: Eigen's, though its matrix is Accelerate's size.
        const QJsonObject model = QJsonDocument::fromJson(R"({"format": "qucs-s multiphysics 1",
            "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "label": "gap", "size": ["10", "1"]}],
              "materials": [{"label": "glass", "epsilonr": "5", "selection": {"all": true}}],
              "physics": [{"type": "electrostatics", "tag": "es", "features": [
                {"type": "terminal", "name": "1", "selection": {"of": ["gap"], "side": "top"}},
                {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}],
              "mesh": {"size": "fine", "custom": true, "hmax": "0.02"}}],
            "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})").object();
        QJsonObject r = call("fem_model", {{"action", "new"}, {"path", "solver_choice"}, {"model", model}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_solve");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(text(r).contains(QLatin1String("with Eigen's Cholesky (LDLT)")), qPrintable(text(r)));
        // Back to Automatic: Accelerate's where this macOS has it.
        {
            MultiphysicsSettingsDialog d(app);
            QCOMPARE(d.solverBox()->currentIndex(), 1);
            d.solverBox()->setCurrentIndex(0);
            d.apply();
        }
        QVERIFY(directSolver() == DirectSolver::Automatic);
        r = call("fem_solve");
        QVERIFY2(!failed(r), qPrintable(text(r)));
#if defined(__APPLE__)
        QVERIFY2(text(r).contains(QLatin1String("with Accelerate's Cholesky")), qPrintable(text(r)));
#endif
        // The environment's over the setting, said.
        qputenv("QUCS_FEM_SOLVER", "eigen");
        {
            MultiphysicsSettingsDialog d(app);
            QVERIFY2(d.solverStatusLabel()->text().startsWith(QLatin1String("QUCS_FEM_SOLVER=eigen is set, whatever this says: Eigen's")),
                     qPrintable(d.solverStatusLabel()->text()));
        }
        qunsetenv("QUCS_FEM_SOLVER");
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        // (The constants with their units: the update check of 48d5fa8, 6.)
        QVERIFY2(format.value("format").toString().contains("q0 or e_const (the elementary charge, C)") && !format.value("format").toString().contains("%CONSTANTS%"),
                 qPrintable(format.value("format").toString()));
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
        QVERIFY(!json(r).contains("hint"));
        // A constant's value as a bare number: pointed out (the result has no
        // unit of it).
        r = call("fem_evaluate", {{"expression", "es.C11/(8.854e-12*5)"}});
        QVERIFY2(!failed(r) && json(r).value("hint").toString().contains("8.854e-12 is eps0's value without its unit"), qPrintable(text(r)));
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
        // Its semiconductor models' parameters its own: set (Auger's), not
        // refused; a key no material has refused, the library's named.
        // Solved with them: none said "not read" (a library Silicon said 59).
        const QString silicon = json(r).value("added").toObject().value("tag").toString();
        r = call("fem_edit", {{"action", "set"}, {"tag", silicon}, {"props", QJsonObject{{"caun", "3e-31[cm^6/s]"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_edit", {{"action", "set"}, {"tag", silicon}, {"props", QJsonObject{{"Cbgnx", "0.5"}}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("no property Cbgnx") && text(r).contains("Cbgn, ") && text(r).contains("caun"), qPrintable(text(r)));
        r = call("fem_solve");
        QVERIFY2(!failed(r) && !text(r).contains("has no property"), qPrintable(text(r)));
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
        QTRY_VERIFY(doc->cancelButton()->isVisible());
        doc->cancelButton()->click();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->solution() == nullptr);
        QVERIFY(doc->lastError().contains(QStringLiteral("cancelled")));
        QVERIFY(!doc->cancelButton()->isVisible());
    }

    // A job's progress in a row below the toolbar, once the job has taken a
    // quarter of a second: its stage on one line, elided to the room it has
    // and whole in its tool tip (in the bar, Fusion wrapped it to two lines
    // and cut them off), the bar its percentage.
    void aJobsStageInItsRow()
    {
        MultiphysicsDoc* doc = open(copyOf("gesi_eam.qfem"));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->progressRow()->isHidden());
        QElapsedTimer clock;
        clock.start();
        doc->compute(QStringLiteral("std2"));
        QVERIFY(doc->isBusy());
        QTRY_VERIFY(doc->progressRow()->isVisible());
        QVERIFY2(clock.elapsed() >= 250, qPrintable(QString::number(clock.elapsed())));
        // At a point of the sweep: "Va = 0 V, lambda = 1.51 µm: ...".
        QLabel* stage = doc->stageLabel();
        QTRY_VERIFY_WITH_TIMEOUT(stage->text().contains(QStringLiteral("lambda = ")), jobWait());
        const auto fits = [stage](bool whole) {
            const QString shown = stage->text(), all = stage->toolTip();
            const int room = stage->contentsRect().width();
            const int needs = stage->fontMetrics().horizontalAdvance(all);
            if (shown.contains(QLatin1Char('\n')) || stage->fontMetrics().horizontalAdvance(shown) > room) return false;
            if (whole) return needs <= room && shown == all;
            return needs > room && shown.endsWith(QChar(0x2026)) && all.startsWith(shown.chopped(1));
        };
        QTRY_VERIFY2(fits(true), qPrintable(stage->text()));
        // The room of a narrow window (the toolbar's width leaves it more).
        stage->setFixedWidth(100);
        QTRY_VERIFY2(fits(false), qPrintable(stage->text()));
        const QProgressBar* bar = doc->progressBar();
        QVERIFY2(bar->text().endsWith(QLatin1Char('%')) && bar->fontMetrics().horizontalAdvance(bar->text()) < bar->contentsRect().width(),
                 qPrintable(bar->text()));
        QVERIFY(doc->isBusy());
        doc->cancelButton()->click();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->progressRow()->isHidden());
        // Its stage the same, elided anew as its room changes.
        doc->progressRow()->show();
        stage->setMinimumWidth(0);
        stage->setMaximumWidth(QWIDGETSIZE_MAX);
        QTRY_VERIFY2(fits(true), qPrintable(stage->text()));
        stage->setFixedWidth(100);
        QTRY_VERIFY2(fits(false), qPrintable(stage->text()));
        stage->setMinimumWidth(0);
        stage->setMaximumWidth(QWIDGETSIZE_MAX);
        doc->progressRow()->hide();
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

    // A point source: where it is marked in the Graphics view when it is
    // chosen; solved by the tools, its power and Purcell factor told.
    void aPointSourceMarkedAndSolved()
    {
        const QString file = path(QStringLiteral("dipole_box.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "x0", "expression": "137[nm]"}],
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "label": "pml", "base": "center", "size": ["6", "6"]},
                         {"type": "rectangle", "label": "box", "base": "center", "size": ["4", "4"]}, {"type": "form union"}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "waves", "tag": "ewfd", "polarization": "hz", "features": [
              {"type": "pml", "selection": {"only": ["pml"]}},
              {"type": "dipole", "label": "Emitter", "position": ["x0", "0.071"], "px": "1e-20[C]"}]}],
            "mesh": {"size": "fine", "custom": true, "hmax": "0.1"}}],
          "studies": [{"type": "study", "label": "Study 1", "steps": [{"type": "frequency", "frequencies": "299.792458[THz]"}]}]})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->view()->marks().isEmpty());
        const QString dipole = tagOf(doc->model(), QStringLiteral("waves/dipole"), QStringLiteral("Emitter"));
        QVERIFY(!dipole.isEmpty());
        panel()->selectNode(dipole);
        QCOMPARE(doc->view()->marks().size(), 1);
        QVERIFY(std::abs(doc->view()->marks().first().x() - 0.137) < 1e-12);
        QVERIFY(std::abs(doc->view()->marks().first().y() - 0.071) < 1e-12);
        // Drawn there: the view with it and without it (nothing else
        // changed) differ about it, and only there.
        GraphicsView* view = doc->view();
        view->fit();
        const QImage drawn = view->grab().toImage();
        const QPoint at = (view->toPixel(view->marks().first()) * drawn.devicePixelRatio()).toPoint();
        const QVector<QPointF> marks = view->marks();
        view->setMarks({});
        const QImage unmarked = view->grab().toImage();
        view->setMarks(marks);
        int near = 0, far = 0;
        for (int y = 0; y < drawn.height(); ++y)
            for (int x = 0; x < drawn.width(); ++x)
                if (drawn.pixel(x, y) != unmarked.pixel(x, y)) ++(std::abs(x - at.x()) <= 12 * drawn.devicePixelRatio() && std::abs(y - at.y()) <= 12 * drawn.devicePixelRatio() ? near : far);
        QVERIFY2(near > 20 && far == 0, qPrintable(QStringLiteral("%1 near, %2 far").arg(near).arg(far)));
        panel()->selectNode(QStringLiteral("ewfd"));
        QVERIFY(doc->view()->marks().isEmpty());
        // Moved by its parameter: the mark follows.
        QVERIFY(doc->editNode(dipole, [](qucs_s::fem::Node& n) {
            n.props.insert(QStringLiteral("position"), QJsonArray{QStringLiteral("-1"), QStringLiteral("x0*2")});
        }, QStringLiteral("position")));
        panel()->selectNode(dipole);
        QCOMPARE(doc->view()->marks().size(), 1);
        QVERIFY(std::abs(doc->view()->marks().first().x() + 1) < 1e-12 && std::abs(doc->view()->marks().first().y() - 0.274) < 1e-12);
        // Solved by the tools: its power and the Purcell factor.
        const QJsonObject r = call("fem_solve");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject globals = json(r).value("globals").toObject();
        for (const char* g : {"ewfd.Psource", "ewfd.Psource0", "ewfd.Purcell"}) QVERIFY2(globals.contains(QLatin1String(g)), qPrintable(text(r)));
        const double purcell = globals.value("ewfd.Purcell").toString().toDouble();
        QVERIFY2(std::abs(purcell - 1) < 0.01, qPrintable(globals.value("ewfd.Purcell").toString()));
    }

    // A study in time: its plot at the last output time, a box to choose
    // another (the plot group's setting, a step of Undo), Derived Values at
    // the time it asks for.
    void aStudyInTimeIsShownAtItsTimes()
    {
        MultiphysicsDoc* doc = open(copyOf("heater_transient.qfem"));
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        // The y axis in the traces' unit (a global's and two points').
        QCOMPARE(shown->a_DocDiags.front()->yAxis.Label, QStringLiteral("ht.Tmax (degC), T_1 (degC), T_2 (degC)"));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        // Its Derived Values at each frequency: twelve each (n_eff, the
        // quasi-static εeff among them; R in Ω/m).
        int rows = 0;
        bool ohmsPerMetre = false;
        for (int i = 0; i < doc->table()->rowCount(); ++i)
            if (doc->table()->item(i, 0)->text() == QLatin1String("Line Parameters")) {
                ++rows;
                for (int c = 1; c < doc->table()->columnCount(); ++c)
                    if (doc->table()->item(i, c) && doc->table()->item(i, c)->text().endsWith(QStringLiteral("Ω/m"))) ohmsPerMetre = true;
            }
        QCOMPARE(rows, 12 * n);
        QVERIFY(ohmsPerMetre);
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

    // Coupled lines in the window (the reviewer's round of 18 October): a
    // ground-signal-signal-ground pair solved - its default graphs each
    // mode's Z0 and α over frequency, its Qucs part a coupled line part,
    // copied for a paste.
    void aCoupledPairsGraphsAndPart()
    {
        const QJsonObject model = QJsonDocument::fromJson(R"({"format": "qucs-s multiphysics 1",
            "components": [{"unit": "um",
              "geometry": [{"type": "rectangle", "label": "air", "position": ["-300", "0"], "size": ["600", "300"]},
                           {"type": "rectangle", "label": "sub", "position": ["-300", "-300"], "size": ["600", "300"]},
                           {"type": "rectangle", "label": "s1", "position": ["-35", "0"], "size": ["20", "5"]},
                           {"type": "rectangle", "label": "s2", "position": ["15", "0"], "size": ["20", "5"]},
                           {"type": "rectangle", "label": "gL", "position": ["-145", "0"], "size": ["100", "5"]},
                           {"type": "rectangle", "label": "gR", "position": ["45", "0"], "size": ["100", "5"]}],
              "materials": [{"material": "Air", "selection": {"only": ["air"]}}, {"material": "Silicon dioxide", "selection": {"only": ["sub"]}},
                            {"material": "Gold", "selection": {"only": ["s1", "s2", "gL", "gR"]}}],
              "physics": [{"type": "electrostatics", "tag": "es", "selection": {"all": true, "except": {"objects": ["s1", "s2", "gL", "gR"]}}, "features": [
                             {"type": "terminal", "name": "1", "selection": {"of": ["s1"]}}, {"type": "terminal", "name": "2", "selection": {"of": ["s2"]}},
                             {"type": "ground", "selection": {"of": ["gL", "gR"]}}]},
                          {"type": "magnetic", "tag": "mf", "order": "1", "features": [
                             {"type": "coil", "name": "1", "selection": {"only": ["s1"]}}, {"type": "coil", "name": "2", "selection": {"only": ["s2"]}},
                             {"type": "coil", "name": "G", "selection": {"only": ["gL", "gR"]}}]}],
              "mesh": {"size": "normal", "custom": true, "hmax": "40", "features": [
                {"type": "size", "level": "domain", "selection": {"only": ["s1", "s2", "gL", "gR"]}, "hmax": "4"},
                {"type": "size", "level": "boundary", "selection": {"of": ["s1", "s2", "gL", "gR"]}, "hmax": "2"}]}}],
            "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary", "physics": ["es"]},
                                                                  {"type": "frequency", "physics": ["mf"], "frequencies": "1[GHz], 20[GHz]"}]}],
            "results": {"derived": [{"type": "lineparams", "tag": "lp", "method": "rlgc", "terminal": "1, 2", "conductor": "1, 2", "return": "G"}],
                        "exports": [{"type": "qucspart", "tag": "qp1", "line": "lp", "length": "10[mm]"}]}})").object();
        QJsonObject r = call("fem_model", {{"action", "new"}, {"path", "gssg_pair"}, {"model", model}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* doc = qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        // Its graphs over frequency: each mode's.
        const QString z0 = tagOf(doc->model(), QStringLiteral("plotgroup1d"), QStringLiteral("Z0 (lp)"));
        QVERIFY(!z0.isEmpty());
        QCOMPARE(doc->model().find(z0)->children.front().text(QStringLiteral("expression")), QStringLiteral("lp.Z0_even; lp.Z0_odd"));
        QString why;
        const QString display = doc->showGraphs(tagOf(doc->model(), QStringLiteral("plotgroup1d"), QStringLiteral("Attenuation (lp)")), &why, false);
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset data;
        QVERIFY(data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why));
        QVERIFY(data.find(QStringLiteral("alpha_even")) && data.find(QStringLiteral("alpha_odd")));
        QCOMPARE(data.find(QStringLiteral("alpha_odd"))->size(), 2);
        // Its part: a coupled line part, copied.
        QVERIFY2(doc->copyPart(QStringLiteral("qp1"), &why), qPrintable(why));
        QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral("<CTLIN Line1 1 200 100")));
        const QString sch = doc->runExport(QStringLiteral("qp1"), &why);
        QVERIFY2(!sch.isEmpty(), qPrintable(why));
        QVERIFY(app->gotoPage(sch));
        auto* part = qobject_cast<Schematic*>(app->DocumentTab->currentWidget());
        QVERIFY(part != nullptr);
        QStringList models;
        for (Component* c : part->a_DocComps) models << c->Model;
        QCOMPARE(models.count(QStringLiteral("CTLIN")), 1);
        QCOMPARE(models.count(QStringLiteral("Port")), 4);
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

    // A process flow along a layout's ruler: its cut the ruler's, a silicon
    // substrate and its material; its steps' masks the layout's layers -
    // by the .lyp's names too -, shown there alone with the cut; its cut
    // along another ruler; a shape picked in the layout naming the steps it
    // is the mask of; its regions' materials made.
    // A flow's dopants through Claude's tools and the panel: boron
    // implanted into n-type silicon and driven in - fem_build to the implant
    // says its moments; fem_evaluate at the anneal its column's junction,
    // sheet resistance and dose kept (the dose to 1e-3: no loss at an inert
    // surface); fem_plot its net doping as a colour map and its profile down
    // the middle on a log axis, a measured SIMS CSV beside it; the panel's
    // buttons on a step make the same.
    // A process flow of a substrate, an implant and an anneal of \a time,
    // its electrostatics solved: \a name in the workspace.
    QString flowModel(const QString& name, const QString& time)
    {
        const QString file = path(name);
        const QString grid = slowBuild() ? QStringLiteral(R"("dx": "100[nm]", "dy": "4[nm]")") : QStringLiteral(R"("dx": "50[nm]", "dy": "2[nm]")");
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        f.write(QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
          "geometry": [{"type": "process", "tag": "pf1", "width": "2", %1, "depth": "1", "features": [
              {"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2", "dopant": "P", "concentration": "1e15"},
              {"type": "implant", "tag": "imp", "species": "B", "energy": "40", "dose": "1e15"},
              {"type": "diffuse", "tag": "anl", "temperature": "1000[degC]", "time": "%2"}]},
            {"type": "form union", "tag": "fin"}],
          "materials": [{"tag": "m1", "material": "Silicon", "selection": {"all": true}}],
          "physics": [{"type": "electrostatics", "tag": "es", "features": [
              {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["si"], "side": "top"}},
              {"type": "ground", "selection": {"of": ["si"], "side": "bottom"}}]}],
          "mesh": {"size": "normal", "custom": true, "hmax": "0.2"}}],
          "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}]})")
                    .arg(grid, time)
                    .toUtf8());
        return file;
    }

    // A model saved here is not read again by the window's watcher: its
    // save is dated by the clock, not by the file (a change on disk is a date
    // newer than it - or, that date in the future, any other: the hunt of 21
    // October, B5). Read again, its steps of Undo were gone.
    void aModelSavedHereIsNotReadAgain()
    {
        MultiphysicsDoc* doc = open(copyOf(QStringLiteral("bratu_ode.qfem")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->editNode(QStringLiteral("cln1"), [](qucs_s::fem::Node& n) { n.props.insert(QStringLiteral("points"), 400); }, QStringLiteral("points")));
        QVERIFY(doc->undoStack()->count() > 0);
        QCOMPARE(doc->save(), 0);
        QTest::qWait(2500);   // (the watcher's 400 ms, its sync's 1.5 s)
        QVERIFY2(doc->undoStack()->count() > 0, "read again");
        QVERIFY(!app->statusBar()->currentMessage().contains(QLatin1String("loaded again")));
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // fem_build waits for a process flow's geometry up to its 'timeout', as
    // for a mesh: past it, how far it is (its step) - it went on (it waited
    // without a limit: the hunt of 21 October, B4).
    void aFlowsBuildWaitedForUpToItsTimeout()
    {
        MultiphysicsDoc* doc = open(flowModel(QStringLiteral("flow_timeout.qfem"), QStringLiteral("600[min]")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->geometryBeingBuilt());
        QElapsedTimer t;
        t.start();
        const QJsonObject r = call("fem_build", {{"timeout", 1}}, jobWait());
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("The geometry is still being built after 1 s"))
                     && text(r).contains(QLatin1String("Building the geometry")) && text(r).contains(QLatin1String("it goes on")),
                 qPrintable(text(r)));
        QVERIFY2(t.elapsed() < 3000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(doc->geometryBeingBuilt());
        QVERIFY2(!failed(call("fem_stop")), "stopped");
        QVERIFY(!doc->building());
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // A call waiting for a model's job is told when that job ends, whatever
    // is called after it meanwhile: it waited in a loop of its own, so a
    // later call - a long one's, or a dialog's - held it back until it was
    // over (a call sent to the background answered only then: the hunt of
    // 21 October, B6).
    void aWaitingCallIsNotHeldBackByLaterOnes()
    {
        const QString quick = flowModel(QStringLiteral("flow_quick.qfem"), QStringLiteral("10[min]"));
        const QString slow = flowModel(QStringLiteral("flow_slow.qfem"), QStringLiteral("600[min]"));
        MultiphysicsDoc* a = open(quick);
        MultiphysicsDoc* b = open(slow);
        QVERIFY(a && b);
        QElapsedTimer t;
        t.start();
        qint64 solvedAt = -1;
        QJsonObject solved;
        control->callTool(QStringLiteral("fem_solve"), {{"path", quick}, {"timeout", int(jobWait() / 1000)}}, [&](const QJsonObject& r) {
            solvedAt = t.elapsed();
            solved = r;
        });
        QCOMPARE(solvedAt, qint64(-1));   // (not answered within the call: no loop of its own)
        // A later call that waits longer - the slow flow's geometry -, its
        // own loop (callNow's) running meanwhile.
        const QJsonObject built = call("fem_build", {{"path", slow}, {"timeout", int(jobWait() / 1000)}}, jobWait());
        const qint64 builtAt = t.elapsed();
        QVERIFY2(!failed(built), qPrintable(text(built)));
        QVERIFY2(solvedAt >= 0 && !failed(solved), qPrintable(text(solved)));
        QVERIFY2(solvedAt < builtAt - 500, qPrintable(QStringLiteral("solved at %1 ms, the later build at %2 ms").arg(solvedAt).arg(builtAt)));
        for (MultiphysicsDoc* d : {a, b}) {
            d->setDocChanged(false);
            app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(d)));
        }
    }

    // A tool that waits by itself, called through callNow with a shorter
    // timeout of the caller's: waited for as long as the tool waits (its
    // "timeout"), not given up on - fem_solve, asynchronous since 27968ca,
    // was cut at a test's 120 s on CI's sanitized Linux, and its answer at
    // the job's end wrote into callNow's frame, gone: a
    // stack-use-after-return (CI of 38491ef).
    void aToolsOwnWaitIsWaitedFor()
    {
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("fem_solve"), {}), 600);
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("fem_solve"), {{"timeout", 5}}), 5);
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("fem_build"), {}), 300);
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("fem_stop"), {}), 30);
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("simulate"), {{"timeout", 45}}), 45);
        // (The other tools that wait by themselves, by their defaults too: cut
        // at a caller's 30 s - the hunt of 22 October, N3.)
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("simulate"), {}), 120);
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("wait_for"), {}), 60);
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("git_remote"), {}), 300);
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("git_clone"), {}), 600);
        QCOMPARE(QucsControl::waitsFor(QStringLiteral("get_state"), {}), 0);
        const QString quick = flowModel(QStringLiteral("flow_own_wait.qfem"), QStringLiteral("10[min]"));
        MultiphysicsDoc* doc = open(quick);
        QVERIFY(doc);
        QElapsedTimer t;
        t.start();
        const QJsonObject r = control->callNow(QStringLiteral("fem_solve"), {{"path", quick}, {"timeout", int(jobWait() / 1000)}}, 1);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).contains(QLatin1String("globals")), qPrintable(text(r)));
        qInfo("solved through callNow in %lld ms, its own timeout 1 ms", t.elapsed());
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // A wait for a model's job when the model goes: deleted in the very answer
    // to the wait (the client gone, the application quitting closes every
    // document in it), or while the wait is under way (closed then) - told
    // once, the second from the event loop once the model is gone, its count
    // never taken off a freed model. (The first read the freed model - a
    // heap-use-after-free under ASan, at every quit of a client during a
    // solve; the second told the call of a model half destroyed: the hunt of
    // 22 October, B1.)
    void aWaitOutlivesItsModel()
    {
        const QString inAnswer = flowModel(QStringLiteral("flow_gone_in_answer.qfem"), QStringLiteral("600[min]"));
        MultiphysicsDoc* doc = open(inAnswer);
        QVERIFY(doc != nullptr && doc->geometryBeingBuilt());
        QPointer<MultiphysicsDoc> d(doc);
        int told = 0;
        bool ended = false;
        doc->whenJobEnds(jobWait(), [&](bool e) {
            ++told;
            ended = e;
            if (d) {
                d->setDocChanged(false);
                delete d.data();   // (as closeAllFiles does as the application quits)
            }
        });
        doc->cancelJob();
        QTRY_VERIFY_WITH_TIMEOUT(told > 0, jobWait());
        QTest::qWait(50);
        QCOMPARE(told, 1);
        QVERIFY(ended && d.isNull());

        // Deleted while a fem_build waits for its geometry: told it was
        // closed, once, from the event loop.
        const QString underWay = flowModel(QStringLiteral("flow_gone_waiting.qfem"), QStringLiteral("600[min]"));
        doc = open(underWay);
        QVERIFY(doc != nullptr && doc->geometryBeingBuilt());
        int answers = 0;
        QJsonObject answer;
        control->callTool(QStringLiteral("fem_build"), {{"path", underWay}, {"timeout", 600}}, [&](const QJsonObject& r) {
            ++answers;
            answer = r;
        });
        QCOMPARE(answers, 0);
        doc->setDocChanged(false);
        delete doc;
        QTRY_VERIFY_WITH_TIMEOUT(answers > 0, 10000);
        QTest::qWait(50);
        QCOMPARE(answers, 1);
        QVERIFY2(failed(answer) && text(answer) == QLatin1String("The model was closed."), qPrintable(text(answer)));
    }

    // fem_stop stops what it stops alone, and what waits behind it: a fem_solve
    // waiting for the model to be free is told it was stopped, not started,
    // and fem_stop answers once the job it stopped ends. (The waiting solve
    // started as the stopped job ended, and fem_stop waited for it too and
    // said "stopped" once it was solved: the hunt of 22 October, B2.) The
    // window's Cancel is the same stop.
    void aStopStopsWhatWaitsBehindIt()
    {
        // (A junction swept over 1600 biases: a solve of some seconds and
        // more, stopped at its next point.)
        const QString file = path(QStringLiteral("stop_queue.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1", "parameters": [{"name": "Va", "expression": "0[V]"}],
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "p", "position": ["0", "0"], "size": ["1", "0.5"]},
                         {"type": "rectangle", "name": "n", "position": ["0", "0.5"], "size": ["1", "0.5"]}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["p"]}},
              {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"only": ["n"]}},
              {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["p"], "side": "bottom"}},
              {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "top"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.05"}}],
          "studies": [{"type": "study", "tag": "s", "steps": [{"type": "sweep", "parameters": [["Va", "range(0, 0.0005, 0.8)", "V"]]}, {"type": "stationary"}]}]})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        int first = 0, queued = 0;
        QJsonObject firstAnswer, queuedAnswer;
        control->callTool(QStringLiteral("fem_solve"), {{"path", file}, {"timeout", 600}}, [&](const QJsonObject& r) {
            ++first;
            firstAnswer = r;
        });
        QVERIFY(doc->isBusy());
        control->callTool(QStringLiteral("fem_solve"), {{"path", file}, {"timeout", 600}}, [&](const QJsonObject& r) {
            ++queued;
            queuedAnswer = r;
        });
        QCOMPARE(queued, 0);
        QElapsedTimer t;
        t.start();
        const QJsonObject stopped = call("fem_stop", {{"path", file}});
        QVERIFY2(!failed(stopped) && json(stopped).value("stopped").toBool(), qPrintable(text(stopped)));
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        QTRY_VERIFY_WITH_TIMEOUT(queued > 0 && first > 0, 10000);
        // (Sanitized, a point may outlast the queued call's second of waiting:
        // it says so instead - and starts nothing either.)
        QVERIFY2(failed(queuedAnswer)
                     && (text(queuedAnswer).contains(QLatin1String("Not solved: the work it waited behind was stopped"))
                         || (slowBuild() && text(queuedAnswer).contains(QLatin1String("It is busy")))),
                 qPrintable(text(queuedAnswer)));
        QVERIFY2(failed(firstAnswer), qPrintable(text(firstAnswer)));
        QTest::qWait(300);
        QVERIFY2(!doc->isBusy(), "a job started after the stop");
        // The window's Cancel: the same stop.
        int again = 0, afterCancel = 0;
        QJsonObject cancelled;
        control->callTool(QStringLiteral("fem_solve"), {{"path", file}, {"timeout", 600}}, [&](const QJsonObject&) { ++again; });
        QVERIFY(doc->isBusy());
        control->callTool(QStringLiteral("fem_solve"), {{"path", file}, {"timeout", 600}}, [&](const QJsonObject& r) {
            ++afterCancel;
            cancelled = r;
        });
        doc->cancelJob();
        QTRY_VERIFY_WITH_TIMEOUT(afterCancel > 0 && again > 0, 10000);
        QVERIFY2(failed(cancelled)
                     && (text(cancelled).contains(QLatin1String("Not solved: the work it waited behind was stopped"))
                         || (slowBuild() && text(cancelled).contains(QLatin1String("It is busy")))),
                 qPrintable(text(cancelled)));
        QTest::qWait(300);
        QVERIFY2(!doc->isBusy(), "a job started after Cancel");
        // Another caller, waiting before fem_stop came, starting a solve as
        // the stopped job ends (as the queued fem_solve did): fem_stop answers
        // as the job it stopped ends, not once the other is solved (it waited
        // for any job, and said "stopped" after that solve).
        int solving = 0, stopAnswers = 0;
        control->callTool(QStringLiteral("fem_solve"), {{"path", file}, {"timeout", 600}}, [&](const QJsonObject&) { ++solving; });
        QVERIFY(doc->isBusy());
        QPointer<MultiphysicsDoc> d(doc);
        doc->whenJobEnds(jobWait(), [d](bool) {
            if (d) d->compute(QStringLiteral("s"));
        });
        control->callTool(QStringLiteral("fem_stop"), {{"path", file}}, [&](const QJsonObject&) { ++stopAnswers; });
        QTRY_VERIFY_WITH_TIMEOUT(stopAnswers > 0 && solving > 0, 10000);
        QVERIFY(doc->isBusy());   // (the other's solve)
        QVERIFY2(!failed(call("fem_stop", {{"path", file}})), "stopped again");
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy(), 10000);
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // A whole run's derived value - Harmonics, of all its times - is listed
    // once a point, all its values (H1 first), not at each output time: 30
    // harmonics over 81 times were 4941 values, fem_solve showing the last
    // 40, H1 and H2 not among them (the hunt of 22 October, B5).
    void harmonicsListedOnceARun()
    {
        if (slowBuild()) QSKIP("in time: Release builds only (minutes sanitized)");
        const QString file = path(QStringLiteral("bar_harmonics.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
              {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
              {"type": "ohmic", "name": "2", "V0": "10[mV]*sin(2*pi*1[GHz]*t) + 2[mV]*sin(2*pi*2[GHz]*t)", "selection": {"of": ["bar"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.1"}}],
          "studies": [{"type": "study", "tag": "s", "steps": [{"type": "stationary"},
                        {"type": "transient", "times": "range(0, 25[ps], 2[ns])", "stepping": "fixed", "dt": "5[ps]"}]}],
          "results": {"derived": [{"type": "harmonics", "tag": "h30", "study": "s", "expression": "semi.I_2", "f0": "1[GHz]", "count": 30, "periods": 1}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        const QJsonObject r = call("fem_solve", {{"path", file}, {"timeout", 600}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject h = json(r).value("derived values").toArray().at(0).toObject();
        const QJsonObject values = h.value("values").toObject();
        QVERIFY2(values.size() == 61 && values.contains("H1") && values.contains("H2") && values.contains("THD") && values.contains("mean"),
                 qPrintable(QJsonDocument(h).toJson()));
        for (auto it = values.begin(); it != values.end(); ++it) QVERIFY2(!it.key().contains(QLatin1String("t = ")), qPrintable(it.key()));
        QVERIFY2(h.value("note").toString().startsWith(QLatin1String("over 1 periods of 1e+09 Hz, 41 output times; past H19")), qPrintable(QJsonDocument(h).toJson()));
        // Its times asked as the answers write them, or refused, said (B9).
        QJsonObject e = call("fem_evaluate", {{"path", file}, {"expression", "semi.I_2"}, {"time", "0.5 ns"}});
        QVERIFY2(!failed(e) && json(e).value("solutions").toString() == QLatin1String("t = 500 ps"), qPrintable(text(e)));
        e = call("fem_evaluate", {{"path", file}, {"expression", "semi.I_2"}, {"time", "5[ns]"}});
        QVERIFY2(failed(e) && text(e) == QLatin1String("time: \"5[ns]\" lies past its outputs: from t = 0 s to t = 2 ns"), qPrintable(text(e)));
        e = call("fem_evaluate", {{"path", file}, {"expression", "semi.I_2"}, {"time", "1[V]"}});
        QVERIFY2(failed(e) && text(e) == QLatin1String("time: \"1[V]\" is not a time"), qPrintable(text(e)));
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // fem_evaluate's photon energy or time: text it cannot read, a value of
    // another unit or a plain number for a spectrum, one past the outputs -
    // refused, said; "1.5 eV" as its answers write one read; with 'all', at
    // that energy at each point. (Each gave the last output or the nearest
    // end without a word, a plain number taken as hertz; with 'all' the
    // energy was ignored, every output given: the hunt of 22 October, B9 and
    // N4.) And fem_plot of a global over the outputs graphs it against them
    // (a surface of it, one colour at the last energy: N11).
    void outputsAskedAsAnswersSayThem()
    {
        const QString file = path(QStringLiteral("asked_outputs.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1", "parameters": [{"name": "F", "expression": "0[V/m]"}],
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "8"]},
                         {"type": "rectangle", "name": "well", "position": ["0", "8"], "size": ["10", "9.4"]},
                         {"type": "rectangle", "name": "above", "position": ["0", "17.4"], "size": ["10", "8"]}],
            "materials": [{"material": "barrier", "compound": "AlGaAs", "x": "0.3", "selection": {"only": ["below", "above"]}},
                          {"material": "well", "compound": "GaAs", "x": "0", "selection": {"only": ["well"]}}],
            "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.25[nm]", "F": "F"}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "tag": "s", "steps": [{"type": "sweep", "parameters": [["F", "0, 1e7", "V/m"]]},
                        {"type": "spectrum", "energies": "linspace(1.40[eV], 1.60[eV], 5)", "states": 4, "linewidth": "5[meV]", "lineshape": "gaussian"}]}]})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        QJsonObject r = call("fem_solve", {{"path", file}, {"study", "s"}, {"timeout", 600}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto at = [&](const QString& frequency, bool all = false) {
            QJsonObject args{{"path", file}, {"study", "s"}, {"expression", "qw.alphaTE"}, {"frequency", frequency}};
            if (all) args.insert("all", true);
            return call("fem_evaluate", args);
        };
        for (const char* ok : {"1.45[eV]", "1.45 eV", "855.1[nm]"}) {
            r = at(QString::fromLatin1(ok));
            QVERIFY2(!failed(r) && json(r).value("solutions").toString().endsWith(QLatin1String("E = 1.45 eV")), qPrintable(QStringLiteral("%1: %2").arg(ok, text(r))));
        }
        r = at("x");
        QVERIFY2(failed(r) && text(r).startsWith(QLatin1String("frequency: \"x\" is not read:")), qPrintable(text(r)));
        r = at("1.5");
        QVERIFY2(failed(r) && text(r).startsWith(QLatin1String("frequency: \"1.5\" is not a photon energy, wavelength or frequency")), qPrintable(text(r)));
        r = at("1[V]");
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("is not a photon energy")), qPrintable(text(r)));
        r = at("3[eV]");
        QVERIFY2(failed(r) && text(r) == QLatin1String("frequency: \"3[eV]\" lies past its outputs: from E = 1.4 eV to E = 1.6 eV"), qPrintable(text(r)));
        r = at("1.45[eV]", true);
        const QJsonArray each = json(r).value("values").toArray();
        QVERIFY2(!failed(r) && each.size() == 2 && each.at(0).toObject().value("name").toString().contains(QLatin1String("E = 1.45 eV"))
                     && each.at(1).toObject().value("name").toString().contains(QLatin1String("F = 10 MV/m, E = 1.45 eV")),
                 qPrintable(text(r)));
        // The plot of a global: a graph against the photon energy.
        r = call("fem_plot", {{"path", file}, {"study", "s"}, {"expression", "qw.alphaTE"}});
        QVERIFY2(!failed(r) && json(r).contains(QLatin1String("display")), qPrintable(text(r)));
        const qucs_s::fem::Node* g = doc->model().find(json(r).value("plot group").toString());
        QVERIFY(g != nullptr && g->type == QLatin1String("plotgroup1d") && !g->children.empty() && g->children.front().type == QLatin1String("globalgraph"));
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // The panel shows a flag given as an expression of the parameters as its
    // text, to edit - and what is shown only while the flag is on as shown:
    // "excitons": "ex" (on) was an unchecked box, the exciton model hidden
    // (the hunt of 22 October, B8). "true" typed makes it a box again. Under
    // the continuum "bound", "pmin" is shown: it is read there (N2).
    void thePanelShowsAnExpressionFlag()
    {
        const QString file = path(QStringLiteral("panel_flags.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1", "parameters": [{"name": "ex", "expression": "1"}],
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "8"]},
                         {"type": "rectangle", "name": "well", "position": ["0", "8"], "size": ["10", "9.4"]},
                         {"type": "rectangle", "name": "above", "position": ["0", "17.4"], "size": ["10", "8"]}],
            "materials": [{"material": "barrier", "compound": "AlGaAs", "x": "0.3", "selection": {"only": ["below", "above"]}},
                          {"material": "well", "compound": "GaAs", "x": "0", "selection": {"only": ["well"]}}],
            "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.5[nm]"}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "steps": [{"type": "spectrum", "tag": "sp", "energies": "1.45[eV]", "excitons": "ex", "continuum": "bound"}]}]})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        panel()->selectNode(QStringLiteral("sp"));
        QWidget* area = panel()->settingsArea();
        auto* flag = area->findChild<QLineEdit*>(QStringLiteral("mpEdit_excitons"));
        QVERIFY2(flag != nullptr && area->findChild<QCheckBox*>(QStringLiteral("mpEdit_excitons")) == nullptr, "not a line edit");
        QCOMPARE(flag->text(), QStringLiteral("ex"));
        QVERIFY(area->findChild<QWidget*>(QStringLiteral("mpEdit_excitonmodel")) != nullptr);
        QVERIFY(area->findChild<QWidget*>(QStringLiteral("mpEdit_pmin")) != nullptr);
        // "true" typed: a flag as such, a box again.
        flag->setText(QStringLiteral("true"));
        emit flag->editingFinished();
        QCOMPARE(doc->model().find(QStringLiteral("sp"))->value(QStringLiteral("excitons")), QJsonValue(true));
        // (The form made again after the change, from the event loop.)
        QTRY_VERIFY(panel()->settingsArea()->findChild<QCheckBox*>(QStringLiteral("mpEdit_excitons")) != nullptr);
        QVERIFY(panel()->settingsArea()->findChild<QCheckBox*>(QStringLiteral("mpEdit_excitons"))->isChecked());
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // A process flow's geometry being built is not waited for by the calls
    // that read or change the model: fem_describe, fem_edit, fem_model set
    // answer at once, the build said - and a stopped build stays stopped.
    // (fem_describe waited for the whole build in a loop of its own, 9.1 s
    // here, minutes sanitized, and after fem_stop built it all again: the
    // hunt of 22 October, B3.)
    void aFlowsBuildIsNotWaitedForByReads()
    {
        const QString file = flowModel(QStringLiteral("flow_reads.qfem"), QStringLiteral("600[min]"));
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr && doc->geometryBeingBuilt());
        QElapsedTimer t;
        t.start();
        QJsonObject r = call("fem_describe", {{"path", file}});
        QVERIFY2(!failed(r) && json(r).value("geometry").toString().startsWith(QLatin1String("being built in the background")), qPrintable(text(r)));
        r = call("fem_describe", {{"path", file}, {"what", "geometry"}});
        QVERIFY2(failed(r) && text(r).startsWith(QLatin1String("The geometry is being built in the background")), qPrintable(text(r)));
        r = call("fem_edit", {{"path", file}, {"action", "set"}, {"tag", "anl"}, {"props", QJsonObject{{"time", "590[min]"}}}});
        QVERIFY2(!failed(r) && json(r).value("geometry").toString() == QLatin1String("being built in the background (fem_build waits for it)"),
                 qPrintable(text(r)));
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(doc->geometryBeingBuilt());
        // Stopped, it stays so: a read starts nothing.
        r = call("fem_stop", {{"path", file}});
        QVERIFY2(!failed(r) && json(r).value("stopped").toBool(), qPrintable(text(r)));
        t.restart();
        r = call("fem_describe", {{"path", file}});
        QVERIFY2(!failed(r) && json(r).value("geometry").toString() == QLatin1String("not built (fem_build builds it)"), qPrintable(text(r)));
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
        QTest::qWait(300);
        QVERIFY2(!doc->building(), "built again by a read");
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // A process flow's geometry built in the background - its anneals take
    // seconds to minutes (71 s an opening for one of the fuzz of 21
    // October's, the window frozen): the model open at once, its structure
    // there once built, the window going on meanwhile and its progress said;
    // Cancel stopping it in its anneal, the geometry left stale, not built
    // again by itself; a change of the model as it is built stopping that
    // build for one of the model as it is; the tools waiting for it - a
    // mesh, a study, after it -; fem_stop stopping it; a step chosen shown
    // once built; closed while it is built, at once.
    void aFlowsGeometryIsBuiltInTheBackground()
    {
        const QString file = path(QStringLiteral("flow_background.qfem"));
        {
            // (Under the sanitizers, a coarser grid: the same a fifth of the time.)
            const QString grid = slowBuild() ? QStringLiteral(R"("dx": "100[nm]", "dy": "4[nm]")") : QStringLiteral(R"("dx": "50[nm]", "dy": "2[nm]")");
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
              "geometry": [{"type": "process", "tag": "pf1", "width": "2", %1, "depth": "1", "features": [
                  {"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2", "dopant": "P", "concentration": "1e15"},
                  {"type": "implant", "tag": "imp", "species": "B", "energy": "40", "dose": "1e15"},
                  {"type": "diffuse", "tag": "anl", "temperature": "1000[degC]", "time": "30[min]"}]},
                {"type": "form union", "tag": "fin"}],
              "materials": [{"tag": "m1", "material": "Silicon", "selection": {"all": true}}],
              "physics": [{"type": "electrostatics", "tag": "es", "features": [
                  {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["si"], "side": "top"}},
                  {"type": "ground", "selection": {"of": ["si"], "side": "bottom"}}]}],
              "mesh": {"size": "normal", "custom": true, "hmax": "0.2"}}],
              "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}]})")
                        .arg(grid)
                        .toUtf8());
        }
        auto said = [](MultiphysicsDoc* d, const QString& text) {
            for (const QString& m : d->messages())
                if (m.contains(text)) return true;
            return false;
        };
        auto anneal = [](const QString& time) {
            return [time](qucs_s::fem::Node& n) { n.props.insert(QStringLiteral("time"), time); };
        };
        // (What stops at once stops within this: a build here takes 1.6 s,
        // ten under the sanitizers.)
        const qint64 prompt = slowBuild() ? 5000 : 500;
        auto count = [](MultiphysicsDoc* d, const QString& text) { return int(d->messages().filter(text).size()); };
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        // Open, not built yet: being built, its progress shown.
        QVERIFY(doc->geometryBeingBuilt());
        QVERIFY(doc->geometryStale());
        // (Shown once it has taken a quarter of a second.)
        QVERIFY(QTest::qWaitFor([doc] { return !doc->progressRow()->isHidden(); }, jobWait()));
        QVERIFY(doc->geometryBeingBuilt());
        int ticks = 0;
        QStringList progress;
        QTimer tick;
        tick.setInterval(10);
        connect(&tick, &QTimer::timeout, this, [&ticks, &progress, doc] {
            ++ticks;
            if (const QString p = doc->jobProgress(); !p.isEmpty()) progress << p;
        });
        tick.start();
        QVERIFY(doc->waitForJob(jobWait()));
        tick.stop();
        QVERIFY2(ticks > 0, "the window went on");
        QVERIFY2(!progress.filter(QStringLiteral("Building the geometry: Diffuse (Anneal), step 3 of 3")).isEmpty(), qPrintable(progress.join(QLatin1Char('\n'))));
        QVERIFY(!doc->geometryStale() && doc->geometry().ok() && doc->builtGeometry().process);
        QVERIFY(said(doc, QStringLiteral("Geometry: 1 domains")));
        // Cancelled in its anneal: at once, stale, said; not built again.
        QVERIFY(doc->editNode(QStringLiteral("anl"), anneal(QStringLiteral("31[min]")), QStringLiteral("time")));
        QVERIFY(QTest::qWaitFor([doc] { return doc->jobProgress().contains(QLatin1String("step 3 of 3")); }, jobWait()));
        QElapsedTimer stopping;
        stopping.start();
        doc->cancelJob();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(stopping.elapsed() < prompt, qPrintable(QString::number(stopping.elapsed())));
        QVERIFY(doc->geometryStale());
        QVERIFY(said(doc, QStringLiteral("The geometry's build was cancelled")));
        QTest::qWait(400);
        QVERIFY(!doc->building() && doc->geometryStale());
        // A change as it is built: that build stopped (not shown), one of the
        // model as it is.
        const int builtBefore = count(doc, QStringLiteral("Geometry: 1 domains"));
        doc->requestGeometry();
        QVERIFY(doc->geometryBeingBuilt());
        QVERIFY(doc->editNode(QStringLiteral("anl"), anneal(QStringLiteral("20[min]")), QStringLiteral("time")));
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(!doc->geometryStale());
        QCOMPARE(count(doc, QStringLiteral("Geometry: 1 domains")), builtBefore + 1);
        QString annealSaid;
        for (const ProcessStepReport& r : doc->geometry().processSteps)
            if (r.tag == QLatin1String("anl")) annealSaid = r.said;
        QVERIFY2(annealSaid.contains(QLatin1String("20 min")), qPrintable(annealSaid));
        // Build Mesh, Compute and the toolbar's Build Geometry: at once, the
        // mesh made, the study solved, once the geometry is built.
        QVERIFY(doc->editNode(QStringLiteral("anl"), anneal(QStringLiteral("19[min]")), QStringLiteral("time")));
        doc->buildMesh();
        QVERIFY(doc->geometryBeingBuilt() && !doc->isBusy());
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(!doc->meshStale() && doc->mesh());
        QVERIFY(doc->editNode(QStringLiteral("anl"), anneal(QStringLiteral("18[min]")), QStringLiteral("time")));
        doc->compute();
        QVERIFY(doc->geometryBeingBuilt() && !doc->isBusy());
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(doc->solution() && !doc->solutionStale());
        auto* build = doc->findChild<QToolButton*>(QStringLiteral("mpBuildGeometry"));
        QVERIFY(build);
        build->click();
        QVERIFY(doc->geometryBeingBuilt());
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(!doc->geometryStale());
        // The tools wait: a mesh, a study, after it.
        QVERIFY(doc->editNode(QStringLiteral("anl"), anneal(QStringLiteral("21[min]")), QStringLiteral("time")));
        QJsonObject r = call("fem_build", {{"what", "mesh"}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!doc->geometryStale() && !doc->meshStale() && doc->mesh());
        QVERIFY(doc->editNode(QStringLiteral("anl"), anneal(QStringLiteral("22[min]")), QStringLiteral("time")));
        r = call("fem_solve", {{"timeout", jobWait() / 1000}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(doc->solution() && !doc->solutionStale());
        // fem_stop: the geometry being built stopped.
        QVERIFY(doc->editNode(QStringLiteral("anl"), anneal(QStringLiteral("23[min]")), QStringLiteral("time")));
        doc->requestGeometry();
        QVERIFY(doc->geometryBeingBuilt());
        r = call("fem_stop");
        QVERIFY2(!failed(r) && json(r).value("stopped").toBool(), qPrintable(text(r)));
        QVERIFY(!doc->building() && doc->geometryStale());
        // A step chosen in the panel: built in the background, shown.
        doc->focusNode(QStringLiteral("imp"));
        QVERIFY(doc->building() && !doc->geometryBeingBuilt());
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY(said(doc, QStringLiteral("After Implant:")));
        // Closed while it is built: at once.
        doc->requestGeometry();
        QVERIFY(doc->geometryBeingBuilt());
        doc->setDocChanged(false);
        QElapsedTimer closing;
        closing.start();
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
        QVERIFY2(closing.elapsed() < prompt, qPrintable(QString::number(closing.elapsed())));
        QTest::qWait(100);
        // Closed while a tool waits on its build (the window going on, the
        // tab can be): the build stopped, the tool told, the model not
        // deleted under it - once it is done.
        QPointer<MultiphysicsDoc> held = open(file);
        QVERIFY(held && held->geometryBeingBuilt());
        QTimer::singleShot(150, this, [this, held] {
            if (!held) return;
            QVERIFY(held->beingWaitedFor());
            held->setDocChanged(false);
            app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(held)));
        });
        r = call("fem_build", {{"path", file}}, jobWait());
        QVERIFY2(failed(r) && (text(r).contains(QLatin1String("closed")) || text(r).contains(QLatin1String("cancelled"))), qPrintable(text(r)));
        QTRY_VERIFY(held.isNull());
        QVERIFY(app->DocumentTab->indexOf(QucsApp::documentWidget(held)) < 0);
    }

    // A process flow's model exported as a Lumerical script: its geometry
    // and mesh built for it in the background, the window going on, its
    // progress said - the script written.
    void aFlowsExportIsBuiltInTheBackground()
    {
        const QString file = path(QStringLiteral("flow_export.qfem"));
        {
            const QString grid = slowBuild() ? QStringLiteral(R"("dx": "100[nm]", "dy": "4[nm]")") : QStringLiteral(R"("dx": "50[nm]", "dy": "2[nm]")");
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
              "geometry": [{"type": "process", "tag": "pf1", "width": "2", %1, "depth": "1", "features": [
                  {"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2", "dopant": "P", "concentration": "1e15"},
                  {"type": "implant", "tag": "imp", "species": "B", "energy": "40", "dose": "1e15"},
                  {"type": "diffuse", "tag": "anl", "temperature": "1000[degC]", "time": "30[min]"}]},
                {"type": "form union", "tag": "fin"}],
              "materials": [{"tag": "m1", "material": "Silicon", "selection": {"all": true}}],
              "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "ohmic", "name": "a", "V0": "0[V]", "selection": {"of": ["si"], "side": "top"}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["si"], "side": "bottom"}}]}],
              "mesh": {"size": "normal", "custom": true, "hmax": "0.2"}}]})")
                        .arg(grid)
                        .toUtf8());
        }
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        QJsonObject r = call("fem_edit", {{"action", "add"}, {"parent", "exports"}, {"type", "lumericalcharge"}}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString tag = json(r).value("added").toObject().value("tag").toString();
        QVERIFY(doc->waitForJob(jobWait()));
        int ticks = 0;
        QStringList progress;
        QTimer tick;
        tick.setInterval(10);
        connect(&tick, &QTimer::timeout, this, [&ticks, &progress, doc] {
            ++ticks;
            if (const QString p = doc->jobProgress(); !p.isEmpty()) progress << p;
        });
        tick.start();
        r = call("fem_export", {{"export", tag}}, jobWait());
        tick.stop();
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("begins").toString().contains(QStringLiteral("a Lumerical™ CHARGE script")), qPrintable(text(r)));
        QVERIFY2(ticks > 0, "the window went on");
        QVERIFY2(!progress.filter(QStringLiteral("Exporting")).isEmpty(), qPrintable(progress.join(QLatin1Char('\n'))));
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // fem_build of a geometry built and not changed since: as it is - its
    // process flow not run again (18.7 s for one of the fuzz of 21 October,
    // N2), its mesh still current. Its materials changed - a flow reads
    // them, a III-V's composition -: built again; a model without a flow's
    // not.
    void aBuiltGeometryIsNotBuiltAgain()
    {
        const QString file = path(QStringLiteral("flow_built.qfem"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
              "geometry": [{"type": "process", "tag": "pf1", "width": "2", "dx": "100[nm]", "dy": "4[nm]", "depth": "1", "features": [
                  {"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2", "dopant": "P", "concentration": "1e15"},
                  {"type": "diffuse", "tag": "anl", "temperature": "1000[degC]", "time": "10[min]", "source": "B", "surface": "1e19"}]},
                {"type": "form union", "tag": "fin"}],
              "materials": [{"tag": "m1", "material": "Silicon", "selection": {"all": true}}],
              "mesh": {"size": "normal", "custom": true, "hmax": "0.2"}}]})");
        }
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        QJsonObject r = call("fem_build", {{"what", "mesh"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!doc->meshStale());
        r = call("fem_build", {});
        QVERIFY2(!failed(r) && text(r).contains(QLatin1String("domains")), qPrintable(text(r)));
        QVERIFY(!doc->meshStale());
        // (Built again or not: its topology another, after the 120 ms the
        // view waits.)
        const Topology* was = doc->geometry().topology.get();
        r = call("fem_edit", {{"action", "set"}, {"tag", "m1"}, {"props", QJsonObject{{"x", "0.3"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QTest::qWait(400);
        QVERIFY(doc->geometry().topology.get() != was);
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
        // (Without a flow: the geometry as it was.)
        const QString plain = path(QStringLiteral("plain_built.qfem"));
        {
            QFile f(plain);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
              "geometry": [{"type": "rectangle", "tag": "r1", "size": ["2", "1"]}, {"type": "form union", "tag": "fin"}],
              "materials": [{"tag": "m1", "material": "Silicon", "selection": {"all": true}}]}]})");
        }
        doc = open(plain);
        QVERIFY(doc != nullptr);
        r = call("fem_build", {});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const Topology* plainWas = doc->geometry().topology.get();
        r = call("fem_edit", {{"action", "set"}, {"tag", "m1"}, {"props", QJsonObject{{"x", "0.3"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QTest::qWait(400);
        QVERIFY(doc->geometry().topology.get() == plainWas);
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // A mesh's hmax without Customize: not used, and said - fem_build's
    // answer's warnings (the update check of 21 October: a 1 by 40 µm
    // sample's hmax 0.1, 0.05 and 0.025 the same 234 triangles, not a word).
    // Customized: used, nothing said.
    void aMeshsUnusedSizesAreSaid()
    {
        const QString file = path(QStringLiteral("mesh_unused.qfem"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
              "geometry": [{"type": "rectangle", "tag": "r1", "size": ["40", "1"]}, {"type": "form union", "tag": "fin"}],
              "materials": [{"tag": "m1", "material": "Silicon", "selection": {"all": true}}],
              "mesh": {"tag": "mesh1", "size": "normal", "hmax": "0.1"}}]})");
        }
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        QJsonObject r = call("fem_build", {{"what", "mesh"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const int preset = json(r).value("triangles").toInt();
        const QJsonArray warnings = json(r).value("warnings").toArray();
        QVERIFY2(warnings.size() == 1
                     && warnings.first().toString()
                            == QStringLiteral("Mesh's hmax is not used: Customize is off (\"custom\": true) - the normal preset's sizes taken"),
                 qPrintable(text(r)));
        r = call("fem_edit", {{"action", "set"}, {"tag", "mesh1"}, {"props", QJsonObject{{"custom", true}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_build", {{"what", "mesh"}});
        QVERIFY2(!failed(r) && !json(r).contains(QLatin1String("warnings")), qPrintable(text(r)));
        qInfo("hmax 0.1 µm: the preset's %d triangles, customized %d", preset, json(r).value("triangles").toInt());
        QVERIFY2(json(r).value("triangles").toInt() > 10 * preset, qPrintable(text(r)));
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    void aFlowsDopantsThroughTheTools()
    {
        const QString file = path(QStringLiteral("flow_dopants.qfem"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
              "geometry": [{"type": "process", "tag": "pf1", "width": "2", "dx": "100[nm]", "dy": "4[nm]", "depth": "1", "features": [
                  {"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2", "dopant": "P", "concentration": "1e15"},
                  {"type": "implant", "tag": "imp", "label": "Boron", "species": "B", "energy": "40", "dose": "1e15"},
                  {"type": "diffuse", "tag": "anl", "label": "Drive-in", "temperature": "1000[degC]", "time": "10[min]"}]},
                {"type": "form union", "tag": "fin"}],
              "materials": [{"material": "Silicon", "selection": {"all": true}}],
              "mesh": {"size": "normal", "custom": true, "hmax": "0.1"}}]})");
        }
        {
            QFile csv(path(QStringLiteral("flow_sims.csv")));
            QVERIFY(csv.open(QIODevice::WriteOnly | QIODevice::Truncate));
            csv.write("depth,B\n0,1e20\n1e-7,5e19\n2e-7,1e19\n3e-7,1e18\n4e-7,1e17\n");
        }
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        QJsonObject r = call("fem_build", {{"until", "imp"}});
        QVERIFY2(!failed(r) && text(r).contains(QLatin1String("Rp 0.145")) && text(r).contains(QLatin1String("Plummer")), qPrintable(text(r)));
        r = call("fem_evaluate", {{"step", "Drive-in"}, {"expression", "pf1.xj; pf1.Rs; pf1.dose_B; pf1.tox"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray values = json(r).value("values").toArray();
        auto valueOf = [&](const char* name) {
            for (const QJsonValue& v : values)
                if (v.toObject().value("name").toString() == QLatin1String(name)) return v.toObject().value("SI").toDouble(NAN);
            return double(NAN);
        };
        const double xj = valueOf("pf1.xj"), rs = valueOf("pf1.Rs"), dose = valueOf("pf1.dose_B");
        QVERIFY2(xj > 0.3e-6 && xj < 1.2e-6 && rs > 20 && rs < 500 && std::abs(dose / 1e19 - 1) < 1e-3 && valueOf("pf1.tox") == 0, qPrintable(text(r)));
        // The net doping changes sign there: acceptors 10 nm above, donors
        // 10 nm below.
        r = call("fem_evaluate", {{"step", "anl"}, {"expression", "pf1.net"}, {"at", QJsonArray{QJsonArray{1.0, -xj * 1e6 + 0.01}, QJsonArray{1.0, -xj * 1e6 - 0.01}}}});
        QVERIFY2(!failed(r) && json(r).value("values").toArray().at(0).toObject().value("SI").toDouble() < 0
                     && json(r).value("values").toArray().at(1).toObject().value("SI").toDouble() > 0,
                 qPrintable(text(r)));
        // At another x, the same (the implant is uniform across).
        r = call("fem_evaluate", {{"step", "anl"}, {"x", 0.3}, {"expression", "pf1.xj; pf1.x"}});
        QVERIFY2(!failed(r) && std::abs(json(r).value("values").toArray().at(0).toObject().value("SI").toDouble() / xj - 1) < 1e-6
                     && std::abs(json(r).value("values").toArray().at(1).toObject().value("SI").toDouble() / 0.3e-6 - 1) < 1e-9,
                 qPrintable(text(r)));
        r = call("fem_evaluate", {{"step", "nothing"}, {"expression", "pf1.xj"}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("Drive-in (anl)")), qPrintable(text(r)));
        // Points: boron near its peak, its net there negative (an acceptor).
        QJsonArray nearPeak;
        nearPeak.append(QJsonArray{1.0, -0.15});
        r = call("fem_evaluate", {{"step", "anl"}, {"expression", "pf1.net"}, {"at", nearPeak}});
        QVERIFY2(!failed(r) && json(r).value("values").toArray().at(0).toObject().value("SI").toDouble() < -1e24, qPrintable(text(r)));
        // A colour map, a profile with the measured one.
        r = call("fem_plot", {{"step", "anl"}});
        QVERIFY2(!failed(r) && json(r).value("shown").toString().contains(QLatin1String("pf1.net")), qPrintable(text(r)));
        r = call("fem_plot", {{"step", "anl"}, {"sims", true}, {"active", true}, {"measured", path(QStringLiteral("flow_sims.csv"))}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject o = json(r);
        QVERIFY2(o.value("graphs").toArray().size() == 4 && o.value("measured").toObject().value("traces").toArray().size() == 1, qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(o.value("dataset").toString()));
        // (Its diagram's y axis logarithmic, SIMS's form.)
        auto* shown = qobject_cast<Schematic*>(app->DocumentTab->currentWidget());
        QVERIFY(shown != nullptr && !shown->a_DocDiags.empty() && shown->a_DocDiags.front()->yAxis.log);
        // The panel's buttons on the implant: a map and a profile made.
        app->DocumentTab->setCurrentWidget(doc);
        panel()->selectNode(QStringLiteral("imp"));
        const int before = int(doc->model().results().children.size());
        QTRY_VERIFY(panel()->findChild<QPushButton*>(QStringLiteral("mpPlotDopants")) != nullptr);
        panel()->findChild<QPushButton*>(QStringLiteral("mpPlotDopants"))->click();
        QTRY_COMPARE(int(doc->model().results().children.size()), before + 1);
        panel()->findChild<QPushButton*>(QStringLiteral("mpDopantProfile"))->click();
        QTRY_COMPARE(int(doc->model().results().children.size()), before + 3);   // (its cut line and its group)
    }

    void aLayoutsProcessFlow()
    {
        const QString gds = copyOf("thermo_optic_heater.gds");
        {
            QFile lyp(QFileInfo(gds).absoluteDir().filePath(QStringLiteral("thermo_optic_heater.lyp")));
            QVERIFY(lyp.open(QIODevice::WriteOnly | QIODevice::Truncate));
            lyp.write("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<layer-properties>\n"
                      " <properties><frame-color>#ff0000</frame-color><fill-color>#ff0000</fill-color><name>waveguide</name><source>1/0@1</source></properties>\n"
                      " <properties><frame-color>#00ff00</frame-color><source>Heater 11/0@1</source></properties>\n"
                      "</layer-properties>\n");
        }
        LayoutLayer named;
        QVERIFY(layoutLayerNamed(QStringLiteral("waveguide"), gds, &named) && named.layer == 1 && named.datatype == 0);
        QVERIFY(layoutLayerNamed(QStringLiteral("Heater"), gds, &named) && named.layer == 11);
        QVERIFY(!layoutLayerNamed(QStringLiteral("nothing"), gds, &named));
        QVERIFY(app->gotoPage(gds));
        auto* layout = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(layout != nullptr);
        QVERIFY(layout->waitForLoaded());
        const QLineF cut(-20, 0, 20, 0);
        layout->view()->addRuler(cut);
        QString why;
        MultiphysicsDoc* doc = layout->processFlow(cut, &why);
        QVERIFY2(doc != nullptr, qPrintable(why));
        Model m = doc->model();
        const qucs_s::fem::Node& flow = m.geometry().children.front();
        QCOMPARE(flow.type, QStringLiteral("process"));
        QCOMPARE(flow.text("file"), QFileInfo(gds).absoluteFilePath());
        QCOMPARE(flow.text("cell"), QStringLiteral("PHASE_SHIFTER"));
        QCOMPARE(flow.children.front().type, QStringLiteral("process/substrate"));
        QCOMPARE(int(m.materials().children.size()), 1);
        // SOI's layers, a rib etched where the waveguide is not drawn - its
        // mask the layer by its name.
        const QString flowTag = flow.tag;
        auto step = [&m](const char* json) {
            const QJsonObject o = QJsonDocument::fromJson(json).object();
            qucs_s::fem::Node n = m.make(QStringLiteral("process/") + o.value("type").toString());
            n.label = o.value("label").toString();
            for (auto it = o.begin(); it != o.end(); ++it)
                if (it.key() != QLatin1String("type") && it.key() != QLatin1String("label")) n.props.insert(it.key(), it.value());
            return n;
        };
        QVERIFY(m.add(flowTag, step(R"({"type": "deposit", "label": "box", "material": "Silicon dioxide", "thickness": "2", "mode": "planar"})")));
        QVERIFY(m.add(flowTag, step(R"({"type": "deposit", "label": "core", "material": "Silicon", "thickness": "0.22", "mode": "planar"})")));
        const qucs_s::fem::Node* rib = m.add(flowTag, step(R"({"type": "etch", "label": "rib", "stop": "Silicon dioxide", "mask": "layer",
                                                                 "layer": "waveguide", "polarity": "undrawn"})"));
        QVERIFY(rib);
        const QString ribTag = rib->tag;
        QVERIFY(doc->setModel(m, QStringLiteral("steps")));
        QVERIFY2(doc->buildGeometry(&why), qPrintable(why));
        QVERIFY(doc->geometry().topology->objectNames.contains(QStringLiteral("core")));
        // The waveguide left where its layer is: its width the layer's under the cut.
        double coreWidth = 0;
        for (const ProcessRegion& r : doc->geometry().process->regions)
            if (r.name == QLatin1String("core")) coreWidth = r.area() / 0.22;
        QVERIFY2(coreWidth > 0.1 && coreWidth < 5, qPrintable(QString::number(coreWidth)));
        // Its materials: oxide new, silicon's the substrate's extended to
        // the core; then none left to make.
        QCOMPARE(doc->makeFlowMaterials(flowTag), 2);
        QCOMPARE(int(doc->model().materials().children.size()), 2);
        QCOMPARE(doc->makeFlowMaterials(flowTag), 0);
        // Its mask in the layout: that layer alone, the cut on it.
        QVERIFY2(doc->showMaskInLayout(ribTag, &why), qPrintable(why));
        int shown = 0;
        for (const qucs_s::layout::LayerStyle& st : layout->view()->styles()) shown += st.visible;
        QCOMPARE(shown, 1);
        QCOMPARE(layout->view()->rulers().size(), 1);
        // A shape of that layer picked: the steps it is the mask of.
        QVERIFY(layout->view()->selectAt(layout->view()->toPixel(QPointF(0, 0))));
        QTRY_VERIFY2(layout->infoLabel()->text().contains(QStringLiteral("the mask of rib")), qPrintable(layout->infoLabel()->text()));
        // The cut along another ruler, of the layout's tab.
        layout->view()->addRuler(QLineF(-15, 1, 15, 1));
        QVERIFY2(doc->cutAlongRuler(flowTag, &why), qPrintable(why));
        QCOMPARE(doc->model().find(flowTag)->pair(QStringLiteral("start")), (QStringList{"-15", "1"}));
        QCOMPARE(doc->model().find(flowTag)->pair(QStringLiteral("end")), (QStringList{"15", "1"}));
        // A step shown as it leaves the structure; fem_build until it.
        const GeometryBuild partial = doc->showBuiltUpTo(ribTag, &why);
        QVERIFY2(partial.ok() && !partial.processSteps.empty() && partial.processSteps.back().tag == ribTag, qPrintable(why));
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

    // A model read from its file is no one's edit, the next one is; a new
    // model's first edit is counted as its others are (it was taken for the
    // content it was opened with, and lost: the check of 31ba73b).
    void aModelsFirstEditIsCounted()
    {
        MultiphysicsDoc* doc = open(coaxCopy());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->revision(), quint64(1));
        QVERIFY(doc->recentEdits().isEmpty());
        Model m = doc->model();
        QList<Model::Parameter> params = m.parameters();
        params[0].expression = QStringLiteral("0.6[mm]");
        m.setParameters(params);
        QVERIFY(doc->setModel(m, QStringLiteral("a")));
        QCOMPARE(doc->revision(), quint64(2));
        QCOMPARE(doc->recentEdits().size(), 1);
        MultiphysicsDoc* fresh = app->newMultiphysicsModel();
        QVERIFY(fresh != nullptr);
        QCOMPARE(fresh->revision(), quint64(0));
        QVERIFY(fresh->setModel(m, QStringLiteral("pasted")));
        QCOMPARE(fresh->revision(), quint64(1));
        QCOMPARE(fresh->recentEdits().size(), 1);
        QCOMPARE(fresh->recentEdits().last().by, quint64(0));   // (the user's: no conversation's call)
    }

    // A Derived Values node's values through fem_evaluate - its tag (each
    // value), tag.name (one), at each frequency with 'all', named as asked
    // among other expressions (they were "not a parameter, a variable or a
    // function here": the check of 31ba73b, 1) -; and a 1D plot group's
    // dataset not stale as it is written (its display, written by the same
    // plot, "edited after" it: 2), stale once the model changes or is
    // solved again.
    void derivedValuesEvaluatedAndTheirDataset()
    {
        open(coaxCopy());
        QJsonObject r = call("fem_solve", {{"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_evaluate", {{"expression", "lp1.R"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonArray values = json(r).value("values").toArray();
        QCOMPARE(values.size(), 1);
        QCOMPARE(values.at(0).toObject().value("name").toString(), QStringLiteral("lp1.R"));
        const double R = values.at(0).toObject().value("SI").toDouble();
        QVERIFY2(values.at(0).toObject().value("value").toString().endsWith(QStringLiteral("Ω/m")), qPrintable(text(r)));
        r = call("fem_evaluate", {{"expression", "lp1.R"}, {"all", true}});
        values = json(r).value("values").toArray();
        QCOMPARE(int(values.size()), coaxFrequencies());
        QVERIFY2(values.at(0).toObject().value("name").toString().endsWith(QLatin1String(": lp1.R")), qPrintable(text(r)));
        // (The last frequency's the one alone gave.)
        QCOMPARE(values.last().toObject().value("SI").toDouble(), R);
        r = call("fem_evaluate", {{"expression", "lp1"}, {"frequency", "100[kHz]"}});
        QStringList names;
        for (const QJsonValue& v : json(r).value("values").toArray()) names << v.toObject().value("name").toString();
        QVERIFY2(names.size() > 5 && names.contains(QStringLiteral("lp1.Z0")) && names.contains(QStringLiteral("lp1.L")), qPrintable(text(r)));
        r = call("fem_evaluate", {{"expression", "z = lp1.Z0; es.C11"}});
        names.clear();
        for (const QJsonValue& v : json(r).value("values").toArray()) names << v.toObject().value("name").toString();
        QCOMPARE(names, (QStringList{QStringLiteral("z"), QStringLiteral("es.C11")}));
        r = call("fem_evaluate", {{"expression", "lp1.nothing"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QLatin1String("has no value nothing: it has")) && text(r).contains(QLatin1String("Z0")), qPrintable(text(r)));

        r = call("fem_plot", {{"plot", tagOf(open(path("coax_rlgc.qfem"))->model(), "plotgroup1d", "Resistance per metre")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString dat = json(r).value("dataset").toString();
        QVERIFY2(dat.endsWith(QLatin1String(".dat")), qPrintable(text(r)));
        r = call("get_dataset", {{"path", json(r).value("display").toString()}});
        QVERIFY2(!failed(r) && !json(r).contains("stale"), qPrintable(text(r)));
        r = call("fem_edit", {{"path", "coax_rlgc.qfem"}, {"action", "parameters"},
                              {"parameters", QJsonArray{QJsonObject{{"name", "a"}, {"expression", "0.6[mm]"}}, QJsonObject{{"name", "b"}, {"expression", "1.75[mm]"}},
                                                        QJsonObject{{"name", "c"}, {"expression", "2[mm]"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("get_dataset", {{"path", dat}});
        QVERIFY2(json(r).value("stale").toString().contains(QLatin1String("coax_rlgc.qfem changed since")), qPrintable(text(r)));
        r = call("fem_solve", {{"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("get_dataset", {{"path", dat}});
        QVERIFY2(json(r).value("stale").toString().contains(QLatin1String("was solved again at")), qPrintable(text(r)));
    }

    // The model panel through Claude's tools, as the reviewer drove it (the
    // check of 31ba73b's addendum): a tree's row by get_ui's number - which
    // chose another, the shown rows' -, by a path from any row; a field set
    // is done with (it kept its old text); a field's Ctrl+A the field's, not
    // the window's Select All; the list a name and a dot open read in
    // send_input's answer, get_ui (area list) and a screenshot, its keys -
    // send_input's into it, set_ui taking a row - and the field's after it.
    void thePanelThroughTheTools()
    {
        MultiphysicsDoc* doc = open(coaxCopy());
        QVERIFY(doc != nullptr);
        const QString area = QStringLiteral("dock:Multiphysics");
        auto controls = [&] {
            QMap<QString, QJsonObject> out;
            for (const QJsonValue& v : json(call("get_ui", {{"area", area}})).value("controls").toArray()) out.insert(v.toObject().value("label").toString(), v.toObject());
            return out;
        };
        QJsonArray rows = controls().value(QStringLiteral("mpTree")).value("rows").toArray();
        int row = -1;
        for (int i = 0, group = -1; i < rows.size() && row < 0; ++i) {
            const QString name = rows.at(i).toArray().at(0).toString();
            if (name == QLatin1String("Electric Field")) group = i;
            if (group >= 0 && name == QLatin1String("Surface")) row = i;
        }
        QVERIFY2(row > 0, qPrintable(QJsonDocument(rows).toJson()));
        QJsonObject r = call("set_ui", {{"area", area}, {"set", QJsonArray{QJsonObject{{"control", "mpTree"}, {"value", row}}}}});
        QVERIFY2(text(r).contains(QLatin1String("> Results > Electric Field > Surface")), qPrintable(text(r)));
        QCOMPARE(controls().value(QStringLiteral("Expression")).value("value").toString(), QStringLiteral("es.normE"));
        r = call("set_ui", {{"area", area}, {"set", QJsonArray{QJsonObject{{"control", "mpTree"}, {"value", "Results > Current Density > Surface"}}}}});
        QVERIFY2(text(r).contains(QLatin1String("Current Density > Surface")), qPrintable(text(r)));
        QCOMPARE(controls().value(QStringLiteral("Expression")).value("value").toString(), QStringLiteral("mf.normJ"));
        r = call("set_ui", {{"area", area}, {"set", QJsonArray{QJsonObject{{"control", "mpTree"}, {"value", "Electric Field > Surface"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("set_ui", {{"area", area}, {"set", QJsonArray{QJsonObject{{"control", "Expression"}, {"value", "es.normE*2"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QString expression;
        for (const qucs_s::fem::Node& g : doc->model().results().children)
            if (g.name() == QLatin1String("Electric Field"))
                for (const qucs_s::fem::Node& p : g.children)
                    if (p.type == QLatin1String("surface")) expression = p.text(QStringLiteral("expression"));
        QCOMPARE(expression, QStringLiteral("es.normE*2"));

        // (A window's Select All of Ctrl+A, live wherever the focus is.)
        auto* window = new QAction(app);
        window->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_A));
        window->setShortcutContext(Qt::ApplicationShortcut);
        app->addAction(window);
        int fired = 0;
        connect(window, &QAction::triggered, this, [&fired] { ++fired; });
        const QJsonArray at = controls().value(QStringLiteral("Expression")).value("at").toArray();
        const QJsonArray click{at.at(0).toInt() + at.at(2).toInt() / 2, at.at(1).toInt() + at.at(3).toInt() / 2};
        // (Another window in front, the user's: the keys reach the panel's
        // field all the same - a completer closes its list at a key while
        // its field has not the focus, as in a window not active.)
        auto* elsewhere = new QWidget;
        elsewhere->resize(200, 100);
        auto* itsField = new QLineEdit(elsewhere);   // (the focus its own)
        elsewhere->show();
        elsewhere->activateWindow();
        QTRY_VERIFY(QApplication::activeWindow() == elsewhere);
        itsField->setFocus();
        QTRY_VERIFY(QApplication::focusWidget() == itsField);
        r = call("send_input", {{"target", area}, {"click", click}, {"keys", "Ctrl+A"}, {"text", "3*es."}});
        delete window;
        QCOMPARE(fired, 0);
        // (Its answer's first text: then the picture's.)
        const auto answer = [](const QJsonObject& reply) {
            for (const QJsonValue& c : reply.value("content").toArray())
                if (c.toObject().value("type").toString() == QLatin1String("text"))
                    return QJsonDocument::fromJson(c.toObject().value("text").toString().toUtf8()).object();
            return QJsonObject();
        };
        QJsonObject said = answer(r);
        QVERIFY2(said.value("given").toString().contains(QLatin1String(": the field's")), qPrintable(text(r)));
        QJsonObject list = said.value("list").toObject();
        QCOMPARE(list.value("of").toString(), QStringLiteral("Expression"));
        QCOMPARE(list.value("text").toString(), QStringLiteral("3*es."));
        QStringList offered;
        for (const QJsonValue& v : list.value("rows").toArray()) offered << v.toArray().at(0).toString();
        QVERIFY2(offered.size() > 3 && offered.contains(QStringLiteral("es.normE")), qPrintable(offered.join(", ")));
        r = call("get_ui", {{"area", "list"}});
        QVERIFY2(!failed(r) && json(r).value("chosen").toInt() == 0, qPrintable(text(r)));
        r = call("send_input", {{"target", area}, {"keys", "Down, Return"}});
        QVERIFY2(answer(r).value("given").toString().count(QLatin1String("in the list")) == 2, qPrintable(text(r)));
        QCOMPARE(controls().value(QStringLiteral("Expression")).value("value").toString(), QStringLiteral("3*") + offered.at(1));
        // (The list gone, the keyboard's next key the field's.)
        QCOMPARE(QApplication::focusWidget(), panel()->editorFor(QStringLiteral("expression")));
        // (The other window in front again: a letter after the dot goes
        // from the list on to the field, which keeps the list open. The keys
        // given before the panel's window is active again, as on a loaded
        // runner, past the half second send_input waits for it - CI's Linux:
        // they went to the panel, not its field, Qt's focus the other
        // window's: no list.)
        elsewhere->activateWindow();
        QTRY_VERIFY(QApplication::activeWindow() == elsewhere);
        control->setActivationWait(0);
        r = call("send_input", {{"target", area}, {"text", "+es.n"}});
        control->setActivationWait(500);
        list = answer(r).value("list").toObject();
        QVERIFY2(list.value("text").toString() == QStringLiteral("3*") + offered.at(1) + QStringLiteral("+es.n"), qPrintable(text(r)));
        for (const QJsonValue& v : list.value("rows").toArray()) QVERIFY2(v.toArray().at(0).toString().startsWith(QLatin1String("es.n")), qPrintable(text(r)));
        // (Its field without Qt's focus while its list is open - its window's
        // activation not come yet, as on CI's macOS runner, or behind another
        // program's: a letter typed is the field's all the same, its list
        // kept - it went through the list, which closed at it.)
        panel()->editorFor(QStringLiteral("expression"))->clearFocus();
        r = call("send_input", {{"target", area}, {"text", "o"}});
        list = answer(r).value("list").toObject();
        QVERIFY2(list.value("text").toString() == QStringLiteral("3*") + offered.at(1) + QStringLiteral("+es.no"), qPrintable(text(r)));
        r = call("screenshot", {{"area", "window"}});
        QVERIFY2(text(r).contains(QLatin1String("the list open below Expression")), qPrintable(text(r)));
        r = call("set_ui", {{"area", "list"}, {"set", QJsonArray{QJsonObject{{"control", "row"}, {"value", "normE"}}}}});
        QVERIFY2(json(r).value("took").toString() == QLatin1String("es.normE")
                     && json(r).value("text").toString() == QStringLiteral("3*") + offered.at(1) + QStringLiteral("+es.normE"),
                 qPrintable(text(r)));
        QVERIFY(failed(call("get_ui", {{"area", "list"}})));
        QTRY_COMPARE(QApplication::focusWidget(), panel()->editorFor(QStringLiteral("expression")));
        delete elsewhere;
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        // Against the frequency, a curve per mode (a dispersion).
        QCOMPARE(neff->dependencies, (QStringList{QStringLiteral("frequency"), QStringLiteral("mode")}));
        const double te20 = std::sqrt(1 - std::pow(299792458.0 / (22.86e-3 * 20e9), 2));
        QVERIFY2(std::abs(neff->re.at(3) - te20) < 2e-3, qPrintable(QString::number(neff->re.at(3))));
        // A wavelength for the plot's frequency.
        panel()->selectNode(doc->shownPlotGroup());
        QVERIFY(qobject_cast<QComboBox*>(panel()->editorFor("frequency")) != nullptr);
        QVERIFY(panel()->editorFor("mode") != nullptr);
    }

    // A guide's two modes swept over its width: a global graph over the width,
    // a curve per mode (its values in that order), named as asked or by its
    // label, emw.loss in dB/cm; Plot against the modes; one mode no axis of
    // it. A study added and deleted leaves the others' solutions as they
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
            "materials": [{"label": "lossy air", "epsilonr": "1", "n": "1", "kappa": "1e-4", "selection": {"all": true}}],
            "physics": [{"type": "modes", "tag": "emw", "order": "1"}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "tag": "std1", "label": "Over the width",
                       "steps": [{"type": "sweep", "parameters": [["a", "40, 45, 50", "mm"]]},
                                 {"type": "mode", "frequency": "10[GHz]", "modes": 2, "neff": "1"}]},
                      {"type": "study", "tag": "std2", "label": "At 15 GHz", "steps": [{"type": "mode", "frequency": "15[GHz]", "modes": 1, "neff": "1"}]},
                      {"type": "study", "tag": "std3", "label": "One mode over the width",
                       "steps": [{"type": "sweep", "parameters": [["a", "40, 45, 50", "mm"]]},
                                 {"type": "mode", "frequency": "10[GHz]", "modes": 1, "neff": "1"}]}],
          "results": {"plots": [{"type": "plotgroup1d", "tag": "pgw", "label": "Index against Width", "study": "std1",
                                 "plots": [{"type": "globalgraph", "expression": "neff_x100 = emw.neff*100; emw.neff"},
                                           {"type": "globalgraph", "label": "Index", "expression": "emw.neff"},
                                           {"type": "globalgraph", "expression": "emw.loss", "unit": "dB/cm"}]},
                                {"type": "plotgroup1d", "tag": "pgm", "label": "Against the modes", "study": "std1", "against": "steps",
                                 "plots": [{"type": "globalgraph", "expression": "emw.neff"}]},
                                {"type": "plotgroup1d", "tag": "pg1", "label": "One mode", "study": "std3",
                                 "plots": [{"type": "globalgraph", "expression": "emw.neff"}]}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        for (const char* study : {"std1", "std2", "std3"}) {
            doc->compute(QString::fromLatin1(study));
            QVERIFY(doc->waitForJob(jobWait()));
            QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        }
        QString why;
        auto dataOf = [&](const QString& group, qucs_s::dataset::Dataset& data) {
            const QString display = doc->showGraphs(group, &why, false);
            return !display.isEmpty() && data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why);
        };
        qucs_s::dataset::Dataset data;
        QVERIFY2(dataOf(QStringLiteral("pgw"), data), qPrintable(why));
        const qucs_s::dataset::Variable* named = data.find(QStringLiteral("neff_x100"));
        const qucs_s::dataset::Variable* neff = data.find(QStringLiteral("emw.neff"));
        const qucs_s::dataset::Variable* labelled = data.find(QStringLiteral("Index"));
        const qucs_s::dataset::Variable* loss = data.find(QStringLiteral("emw.loss"));
        QVERIFY(named && neff && labelled && loss);
        // Over the width, a curve per mode: the width the faster.
        const QStringList overWidth{QStringLiteral("a"), QStringLiteral("mode")};
        QCOMPARE(neff->dependencies, overWidth);
        QCOMPARE(named->dependencies, overWidth);
        QCOMPARE(labelled->dependencies, overWidth);
        QCOMPARE(neff->size(), 6);
        const std::vector<double> widths{40e-3, 45e-3, 50e-3};
        for (int i = 0; i < 3; ++i) {
            // TE10 at each width, then TE20.
            const double te10 = std::sqrt(1 - std::pow(299792458.0 / (2 * widths[std::size_t(i)] * 10e9), 2));
            const double te20 = std::sqrt(1 - std::pow(299792458.0 / (widths[std::size_t(i)] * 10e9), 2));
            QVERIFY2(std::abs(neff->re.at(i) - te10) < 3e-3, qPrintable(QString::number(neff->re.at(i))));
            QVERIFY2(std::abs(neff->re.at(3 + i) - te20) < 5e-3, qPrintable(QString::number(neff->re.at(3 + i))));
        }
        for (int i = 0; i < 6; ++i) {
            QVERIFY(std::abs(named->re.at(i) - 100 * neff->re.at(i)) < 1e-9);
            QCOMPARE(labelled->re.at(i), neff->re.at(i));
        }
        // emw.loss (dB/m) in dB/cm: the second width's TE10.
        Solution& second = *doc->solutions(QStringLiteral("std1"))->solutions[1];
        second.select(0);
        QVERIFY(std::abs(loss->re.at(1) - *second.global(QStringLiteral("emw.loss")) / 100) < 1e-12);
        QVERIFY(loss->re.at(1) > 0);
        // Plotted against the modes: as stored (TE20 at the first width second).
        const QVector<double> overWidthValues = neff->re;
        QVERIFY2(dataOf(QStringLiteral("pgm"), data), qPrintable(why));
        QCOMPARE(data.find(QStringLiteral("emw.neff"))->dependencies, (QStringList{QStringLiteral("mode"), QStringLiteral("a")}));
        QCOMPARE(data.find(QStringLiteral("emw.neff"))->re.at(1), overWidthValues.at(3));
        // One mode: no axis of it.
        QVERIFY2(dataOf(QStringLiteral("pg1"), data), qPrintable(why));
        QCOMPARE(data.find(QStringLiteral("emw.neff"))->dependencies, QStringList{QStringLiteral("a")});
        QVERIFY(data.find(QStringLiteral("mode")) == nullptr);
        // emw.loss (dB/m) in dB/cm in Claude's fem_evaluate too, as a global
        // and at a point; a unit it cannot take: its own, and why.
        QJsonObject r = call("fem_evaluate", {{"study", "std1"}, {"expression", "emw.loss"}, {"unit", "dB/cm"}});
        QVERIFY2(json(r).value("values").toArray().at(0).toObject().value("value").toString().endsWith(" dB/cm"), qPrintable(text(r)));
        QJsonArray middle;
        middle.append(QJsonArray{25, 5});
        r = call("fem_evaluate", {{"study", "std1"}, {"expression", "emw.loss"}, {"unit", "dB/cm"}, {"at", middle}});
        QVERIFY2(json(r).value("values").toArray().at(0).toObject().value("value").toString().endsWith(" dB/cm"), qPrintable(text(r)));
        r = call("fem_evaluate", {{"study", "std1"}, {"expression", "emw.loss"}, {"unit", "dB/furlong"}});
        QVERIFY2(json(r).value("values").toArray().at(0).toObject().value("value").toString().endsWith(" dB/m")
                     && json(r).value("error").toString().contains("dB/furlong"),
                 qPrintable(text(r)));
        // Named in Claude's fem_evaluate too: a global's, at a point.
        r = call("fem_evaluate", {{"study", "std2"}, {"expression", "twice = 2*emw.neff; emw.neff"}});
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

    // A transient swept over its heat: its graph over the time, a curve per
    // power (read over its own axis, not the sweep's); Plot against the sweep
    // turns it, its values with it.
    void graphsInTimeOverASweep()
    {
        const QString file = path(QStringLiteral("slab_sweep.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1", "parameters": [{"name": "P", "expression": "1e5[W/m^3]"}],
          "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "slab", "size": ["10", "2"]}],
            "materials": [{"label": "m", "k": "1[W/(m*K)]", "rho": "1000[kg/m^3]", "Cp": "1000[J/(kg*K)]", "selection": {"all": true}}],
            "physics": [{"type": "heat", "tag": "ht", "features": [
              {"type": "init", "T0": "300[K]", "selection": {"all": true}},
              {"type": "source", "Q0": "P", "selection": {"all": true}},
              {"type": "temperature", "T0": "300[K]", "selection": {"of": ["slab"], "side": "left"}}]}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "sweep", "parameters": [["P", "1e5, 2e5", "W/m^3"]]},
                                                                 {"type": "transient", "times": "0, 10, 20"}]}],
          "results": {"plots": [{"type": "cutpoint", "tag": "cpt1", "coordinates": [["5", "1"]]},
                                {"type": "cutline", "tag": "cln1", "start": ["0", "1"], "end": ["10", "1"], "points": 11},
                                {"type": "plotgroup1d", "tag": "pgt", "label": "In time", "study": "std1",
                                 "plots": [{"type": "globalgraph", "expression": "ht.Tmax"},
                                           {"type": "pointgraph", "label": "Tmid", "data": "cpt1", "expression": "T"}]},
                                {"type": "plotgroup1d", "tag": "pgp", "label": "Against the power", "study": "std1", "against": "sweep",
                                 "plots": [{"type": "globalgraph", "expression": "ht.Tmax"},
                                           {"type": "pointgraph", "label": "Tmid", "data": "cpt1", "expression": "T"}]},
                                {"type": "plotgroup1d", "tag": "pgl", "label": "Along", "study": "std1",
                                 "plots": [{"type": "linegraph", "label": "Tline", "data": "cln1", "expression": "T", "xaxis": "x"}]}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QString why;
        qucs_s::dataset::Dataset data;
        QString display = doc->showGraphs(QStringLiteral("pgt"), &why, false);
        QVERIFY2(!display.isEmpty() && data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why), qPrintable(why));
        const qucs_s::dataset::Variable* tmax = data.find(QStringLiteral("ht.Tmax"));
        QVERIFY(tmax != nullptr);
        QCOMPARE(tmax->dependencies, (QStringList{QStringLiteral("time"), QStringLiteral("P")}));
        // Rising in time, twice as fast at twice the power.
        const QVector<double> inTime = tmax->re;
        QVERIFY(std::abs(inTime.at(0) - 300) < 1e-9 && inTime.at(1) > inTime.at(0) + 0.1 && inTime.at(2) > inTime.at(1));
        QVERIFY2(std::abs((inTime.at(4) - 300) / (inTime.at(1) - 300) - 2) < 1e-3, qPrintable(QString::number(inTime.at(4))));
        // A point's, named by its graph's label, over the same.
        const qucs_s::dataset::Variable* mid = data.find(QStringLiteral("Tmid"));
        QVERIFY(mid != nullptr);
        QCOMPARE(mid->dependencies, (QStringList{QStringLiteral("time"), QStringLiteral("P")}));
        const QVector<double> midInTime = mid->re;
        QVERIFY(std::abs(midInTime.at(0) - 300) < 1e-9 && midInTime.at(4) > midInTime.at(1));
        display = doc->showGraphs(QStringLiteral("pgp"), &why, false);
        QVERIFY2(!display.isEmpty() && data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why), qPrintable(why));
        tmax = data.find(QStringLiteral("ht.Tmax"));
        QVERIFY(tmax != nullptr);
        QCOMPARE(tmax->dependencies, (QStringList{QStringLiteral("P"), QStringLiteral("time")}));
        QCOMPARE(tmax->re.at(2), inTime.at(1));   // (P 1e5, t 10 s)
        QCOMPARE(tmax->re.at(3), inTime.at(4));   // (P 2e5, t 10 s)
        QVERIFY(data.find(QStringLiteral("Tmid")));
        QCOMPARE(data.find(QStringLiteral("Tmid"))->dependencies, (QStringList{QStringLiteral("P"), QStringLiteral("time")}));
        QCOMPARE(data.find(QStringLiteral("Tmid"))->re.at(3), midInTime.at(4));
        // A line's, named so, its axis first.
        display = doc->showGraphs(QStringLiteral("pgl"), &why, false);
        QVERIFY2(!display.isEmpty() && data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why), qPrintable(why));
        QVERIFY(data.find(QStringLiteral("Tline")));
        QCOMPARE(data.find(QStringLiteral("Tline"))->dependencies, (QStringList{QStringLiteral("x"), QStringLiteral("time"), QStringLiteral("P")}));
    }

    // A sweep of two parameters: of all their combinations an axis each, the
    // last the faster (T over the conductivity, a curve per power); one of a
    // value not an axis (T over the power, not over an index of the points);
    // of specified combinations the one changing, an index of two changing.
    void aSweepOfTwoParameters()
    {
        const QString file = path(QStringLiteral("slab_two.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "P", "expression": "1e5[W/m^3]"}, {"name": "kt", "expression": "1[W/(m*K)]"}],
          "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "slab", "size": ["10", "2"]}],
            "materials": [{"label": "m", "k": "kt", "selection": {"all": true}}],
            "physics": [{"type": "heat", "tag": "ht", "features": [
              {"type": "source", "Q0": "P", "selection": {"all": true}},
              {"type": "temperature", "T0": "300[K]", "selection": {"of": ["slab"], "side": "left"}}]}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "sweep", "tag": "swp", "parameters": []},
                                                                 {"type": "stationary"}]}],
          "results": {"plots": [{"type": "plotgroup1d", "tag": "pgs", "label": "Swept", "study": "std1",
                                 "plots": [{"type": "globalgraph", "expression": "ht.Tmax"}]}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        QString why;
        qucs_s::dataset::Dataset data;
        auto sweep = [&](const QJsonArray& rows, const QString& combination) {
            Model m = doc->model();
            m.find(QStringLiteral("swp"))->props.insert(QStringLiteral("parameters"), rows);
            m.find(QStringLiteral("swp"))->props.insert(QStringLiteral("combination"), combination);
            if (!doc->setModel(m, QStringLiteral("swept"))) return false;
            doc->compute();
            if (!doc->waitForJob(jobWait()) || !doc->lastError().isEmpty()) return false;
            const QString display = doc->showGraphs(QStringLiteral("pgs"), &why, false);
            return !display.isEmpty() && data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why);
        };
        auto row = [](const char* name, const char* values, const char* unit) {
            return QJsonArray{QString::fromLatin1(name), QString::fromLatin1(values), QString::fromLatin1(unit)};
        };
        auto axis = [&](const char* name) {
            const qucs_s::dataset::Variable* v = data.find(QString::fromLatin1(name));
            return v && v->independent ? v->re : QVector<double>();
        };
        // (Rows appended: a braced list of one list is a copy to some compilers.)
        QJsonArray rows;
        rows.append(row("kt", "2", "W/(m*K)"));
        rows.append(row("P", "1e5, 2e5, 3e5", "W/m^3"));
        QVERIFY2(sweep(rows, QStringLiteral("all")), qPrintable(why + doc->lastError()));
        const qucs_s::dataset::Variable* tmax = data.find(QStringLiteral("ht.Tmax"));
        QVERIFY(tmax != nullptr);
        QCOMPARE(tmax->dependencies, QStringList{QStringLiteral("P")});
        QCOMPARE(axis("P"), (QVector<double>{1e5, 2e5, 3e5}));
        QVERIFY(!data.find(QStringLiteral("sweep")) && !data.find(QStringLiteral("kt")));
        const QVector<double> byPower = tmax->re;
        QVERIFY2(byPower.size() == 3 && byPower.at(0) > 300.1 && std::abs((byPower.at(2) - 300) / (byPower.at(0) - 300) - 3) < 1e-6,
                 qPrintable(QString::number(byPower.value(2))));
        // Both of several values: T over kt (the faster, along x), a curve per P.
        rows = QJsonArray();
        rows.append(row("P", "1e5, 2e5, 3e5", "W/m^3"));
        rows.append(row("kt", "1, 2", "W/(m*K)"));
        QVERIFY2(sweep(rows, QStringLiteral("all")), qPrintable(why + doc->lastError()));
        tmax = data.find(QStringLiteral("ht.Tmax"));
        QVERIFY(tmax != nullptr);
        QCOMPARE(tmax->dependencies, (QStringList{QStringLiteral("kt"), QStringLiteral("P")}));
        QCOMPARE(axis("kt"), (QVector<double>{1, 2}));
        QCOMPARE(axis("P"), (QVector<double>{1e5, 2e5, 3e5}));
        QCOMPARE(tmax->re.size(), 6);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 2; ++j) {
                // (T - 300 K as P/kt, each value where its axes say.)
                const double expected = (byPower.at(0) - 300) * 2 * (i + 1) / (j + 1);
                QVERIFY2(std::abs(tmax->re.at(j + 2 * i) - 300 - expected) < 1e-6 * expected,
                         qPrintable(QStringLiteral("P %1 kt %2: %3").arg(i).arg(j).arg(tmax->re.at(j + 2 * i))));
            }
        // Specified combinations, the power alone changing: over it, as given.
        rows = QJsonArray();
        rows.append(row("kt", "2, 2, 2", "W/(m*K)"));
        rows.append(row("P", "3e5, 1e5, 3e5", "W/m^3"));
        QVERIFY2(sweep(rows, QStringLiteral("specified")), qPrintable(why + doc->lastError()));
        QCOMPARE(data.find(QStringLiteral("ht.Tmax"))->dependencies, QStringList{QStringLiteral("P")});
        QCOMPARE(axis("P"), (QVector<double>{3e5, 1e5, 3e5}));   // (as given, each its point)
        QCOMPARE(data.find(QStringLiteral("ht.Tmax"))->re.at(0), byPower.at(2));
        // Both changing together: an index of the points.
        rows = QJsonArray();
        rows.append(row("kt", "1, 2", "W/(m*K)"));
        rows.append(row("P", "1e5, 2e5", "W/m^3"));
        QVERIFY2(sweep(rows, QStringLiteral("specified")), qPrintable(why + doc->lastError()));
        QCOMPARE(data.find(QStringLiteral("ht.Tmax"))->dependencies, QStringList{QStringLiteral("sweep")});
        QCOMPARE(axis("sweep"), (QVector<double>{1, 2}));
        // (Three of the four combinations; all four, the first the faster.)
        for (const auto& [conductivities, powers] : {std::pair{"1, 1, 2", "1e5, 2e5, 1e5"}, std::pair{"1, 2, 1, 2", "1e5, 1e5, 2e5, 2e5"}}) {
            rows = QJsonArray();
            rows.append(row("kt", conductivities, "W/(m*K)"));
            rows.append(row("P", powers, "W/m^3"));
            QVERIFY2(sweep(rows, QStringLiteral("specified")), qPrintable(why + doc->lastError()));
            QCOMPARE(data.find(QStringLiteral("ht.Tmax"))->dependencies, QStringList{QStringLiteral("sweep")});
            QCOMPARE(axis("sweep").size(), QString::fromLatin1(powers).count(QLatin1Char(',')) + 1);
        }
    }

    // A plot group shown again after its graph was renamed: its diagram
    // replaced where it was, not left above the new one with its trace
    // invalid; a diagram of the user's kept. Its y axis in the traces' units
    // (a label's own, an SI one, none for a number), a legend for two.
    void aPlotGroupShownAgain()
    {
        const QString file = path(QStringLiteral("guide_again.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1", "parameters": [{"name": "a", "expression": "22.86[mm]"}],
          "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "label": "guide", "size": ["a/1[mm]", "10.16"]}, {"type": "form union"}],
            "materials": [{"label": "lossy air", "epsilonr": "1", "n": "1", "kappa": "1e-4", "selection": {"all": true}}],
            "physics": [{"type": "modes", "tag": "emw", "order": "1"}], "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "sweep", "parameters": [["a", "40, 45, 50", "mm"]]},
                                                                 {"type": "mode", "frequency": "10[GHz]", "modes": 1, "neff": "1"}]}],
          "results": {"plots": [{"type": "cutline", "tag": "cln1", "start": ["0", "5"], "end": ["a/1[mm]", "5"], "points": 21},
                                {"type": "plotgroup1d", "tag": "pgl", "label": "Loss against width", "study": "std1",
                                 "plots": [{"type": "globalgraph", "tag": "gg1", "expression": "emw.loss/100"}]},
                                {"type": "plotgroup1d", "tag": "pga", "label": "Across", "study": "std1",
                                 "plots": [{"type": "linegraph", "data": "cln1", "expression": "emw.normE"}]}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute();
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        QString why;
        auto showAgain = [&]() {
            app->gotoPage(doc->getDocName());
            const QString display = doc->showGraphs(QStringLiteral("pgl"), &why);
            return display;
        };
        auto diagrams = [&](const QString& display) {
            return json(call("get_schematic", {{"path", display}})).value("diagrams").toArray();
        };
        const QString display = showAgain();
        QVERIFY2(!display.isEmpty(), qPrintable(why));
        QJsonArray d = diagrams(display);
        QCOMPARE(d.size(), 1);
        QCOMPARE(d.at(0).toObject().value("traces").toArray().at(0).toObject().value("variable").toString(), QStringLiteral("emw.loss_100"));
        // (A dB/m global over 100: no SI unit to say, 1/m.)
        QCOMPARE(d.at(0).toObject().value("y_axis").toObject().value("label").toString(), QString());
        // (Moved and made larger by the user: kept so.)
        QVERIFY(!failed(call("edit_diagram", {{"path", display}, {"diagram", 1}, {"x", 120}, {"y", 520}, {"width", 360}, {"height", 200}})));
        // Renamed by a label, in dB/cm: one diagram, of Loss, its axis so.
        Model m = doc->model();
        m.find(QStringLiteral("gg1"))->label = QStringLiteral("Loss (dB/cm)");
        m.find(QStringLiteral("gg1"))->props.insert(QStringLiteral("expression"), QStringLiteral("emw.loss"));
        m.find(QStringLiteral("gg1"))->props.insert(QStringLiteral("unit"), QStringLiteral("dB/cm"));
        QVERIFY(doc->setModel(m, QStringLiteral("relabelled")));
        QCOMPARE(showAgain(), display);
        d = diagrams(display);
        QCOMPARE(d.size(), 1);
        QJsonObject only = d.at(0).toObject();
        QCOMPARE(only.value("traces").toArray().size(), 1);
        QCOMPARE(only.value("traces").toArray().at(0).toObject().value("variable").toString(), QStringLiteral("Loss"));
        QCOMPARE(only.value("y_axis").toObject().value("label").toString(), QStringLiteral("Loss (dB/cm)"));
        QVERIFY2(only.value("x").toInt() == 120 && only.value("y").toInt() == 520 && only.value("width").toInt() == 360 && only.value("height").toInt() == 200,
                 qPrintable(QJsonDocument(only).toJson(QJsonDocument::Compact)));
        // As the paper check's display had it: another diagram of the plot
        // group's title, of a trace since gone, beside the current one -
        // gone when it is shown again, the current one kept.
        QVERIFY(!failed(call("add_diagram", {{"path", display}, {"title", "Loss against width"}, {"x", 400}, {"y", 600},
                                             {"traces", QJsonArray{"guide_again_pgl:Loss"}}})));
        QCOMPARE(diagrams(display).size(), 2);
        showAgain();
        d = diagrams(display);
        QCOMPARE(d.size(), 1);
        QCOMPARE(d.at(0).toObject().value("traces").toArray().at(0).toObject().value("variable").toString(), QStringLiteral("Loss"));
        QCOMPARE(d.at(0).toObject().value("x").toInt(), 120);
        // A diagram of the user's beside it.
        QVERIFY(!failed(call("add_diagram", {{"path", display}, {"title", "Mine"}, {"x", 400}, {"y", 240}, {"traces", QJsonArray{"guide_again_pgl:Loss"}}})));
        // A second graph: the plot group's diagram replaced (two traces, a
        // number's without a unit, a legend), the user's kept.
        qucs_s::fem::Node neff = m.make(QStringLiteral("globalgraph"));
        neff.props.insert(QStringLiteral("expression"), QStringLiteral("emw.neff"));
        QVERIFY(m.add(QStringLiteral("pgl"), neff) != nullptr);
        QVERIFY(doc->setModel(m, QStringLiteral("a graph added")));
        showAgain();
        d = diagrams(display);
        QCOMPARE(d.size(), 2);
        QStringList titles;
        for (const QJsonValue& v : d) titles << v.toObject().value("title").toString();
        QVERIFY2(titles.contains("Mine") && titles.contains("Loss against width"), qPrintable(titles.join(", ")));
        for (const QJsonValue& v : d)
            if (v.toObject().value("title").toString() == QLatin1String("Loss against width")) {
                QCOMPARE(v.toObject().value("traces").toArray().size(), 2);
                QCOMPARE(v.toObject().value("y_axis").toObject().value("label").toString(), QStringLiteral("Loss (dB/cm), emw.neff"));
                QCOMPARE(v.toObject().value("legend").toString(), QStringLiteral("top_right"));
            }
        // Its unit changed, its traces not: the axis said anew.
        m.find(QStringLiteral("gg1"))->label.clear();
        QVERIFY(doc->setModel(m, QStringLiteral("unlabelled")));
        showAgain();
        m = doc->model();
        m.find(QStringLiteral("gg1"))->label = QString();
        m.find(QStringLiteral("gg1"))->props.insert(QStringLiteral("unit"), QStringLiteral("dB/m"));
        QVERIFY(doc->setModel(m, QStringLiteral("in dB/m")));
        showAgain();
        bool said = false;
        for (const QJsonValue& v : diagrams(display))
            if (v.toObject().value("title").toString() == QLatin1String("Loss against width"))
                said = v.toObject().value("y_axis").toObject().value("label").toString() == QLatin1String("emw.loss (dB/m), emw.neff");
        QVERIFY(said);
        // A line graph's in SI when it has no unit.
        app->gotoPage(doc->getDocName());
        const QString across = doc->showGraphs(QStringLiteral("pga"), &why);
        QVERIFY2(!across.isEmpty(), qPrintable(why));
        QCOMPARE(diagrams(across).at(0).toObject().value("y_axis").toObject().value("label").toString(), QStringLiteral("emw.normE (V/m)"));
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
    // A laser's light against its current: a 1D plot group's x a global
    // (semi.I_anode) in place of its one axis, each point's value; without,
    // the Laser L-I step's own axis, its voltages (V).
    void lightAgainstCurrent()
    {
        const QString file = path(QStringLiteral("toy_laser.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um", "space": "axisymmetric",
            "geometry": [{"type": "rectangle", "name": "nclad", "position": ["0", "0"], "size": ["1", "0.49"]},
                         {"type": "rectangle", "name": "ring", "position": ["0", "0.49"], "size": ["1", "0.02"]},
                         {"type": "rectangle", "name": "active", "position": ["0", "0.49"], "size": ["0.2", "0.02"]},
                         {"type": "rectangle", "name": "pclad", "position": ["0", "0.51"], "size": ["1", "0.49"]}],
            "materials": [{"material": "Aluminium gallium arsenide", "x": "0.6", "n": "3.5", "kappa": "1e-3", "selection": {"only": ["nclad", "pclad", "ring"]}},
                          {"material": "Gallium arsenide", "n": "3.5", "kappa": "1e-3", "selection": {"only": ["active"]}}],
            "physics": [{"type": "resonator", "tag": "emr", "m": 0, "features": [{"type": "wave", "selection": {"all": true}}]},
                        {"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "dopant": "donor", "N0": "1e18[1/cm^3]", "selection": {"only": ["nclad"]}},
              {"type": "doping", "dopant": "acceptor", "N0": "1e18[1/cm^3]", "selection": {"only": ["pclad"]}},
              {"type": "radiative", "B": "1e-10[cm^3/s]", "selection": {"only": ["active"]}},
              {"type": "laser", "gain": "linear", "a": "5e-16[cm^2]", "Ntr": "2e18[1/cm^3]", "ng": "3.5", "selection": {"only": ["active"]}},
              {"type": "ohmic", "name": "anode", "V0": "0[V]", "selection": {"of": ["pclad"], "side": "top"}},
              {"type": "ohmic", "name": "cathode", "V0": "0[V]", "selection": {"of": ["nclad"], "side": "bottom"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.1",
                     "features": [{"type": "layered", "hlateral": "0.05", "count": 4, "selection": {"all": true}}]}}],
          "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "eigenfrequency", "physics": ["emr"], "count": 1, "around": "9.1[um]"},
                                   {"type": "laser", "physics": ["semi"], "contact": "anode", "voltages": "1.50, 1.54, 1.56, 1.58, 1.60"}]}],
          "results": {"plots": [{"type": "plotgroup1d", "tag": "pgli", "study": "std1", "x": "semi.I_anode",
                                 "plots": [{"type": "globalgraph", "expression": "semi.Pout"}]},
                                {"type": "plotgroup1d", "tag": "pgiv", "study": "std1",
                                 "plots": [{"type": "globalgraph", "expression": "semi.I_anode"}]}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute(QStringLiteral("std1"));
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        Solution& s = *doc->solutions(QStringLiteral("std1"))->solutions.back();
        QCOMPARE(s.snapshotCount(), 5);
        std::vector<double> I, P, V;
        for (int k = 0; k < 5; ++k) {
            s.select(k);
            I.push_back(*s.global(QStringLiteral("semi.I_anode")));
            P.push_back(*s.global(QStringLiteral("semi.Pout")));
            V.push_back(s.snapshotValue(k));
        }
        QString why;
        auto dataOf = [&](const QString& group, qucs_s::dataset::Dataset& data) {
            const QString display = doc->showGraphs(group, &why, false);
            return !display.isEmpty() && data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why);
        };
        qucs_s::dataset::Dataset li, iv;
        QVERIFY2(dataOf(QStringLiteral("pgli"), li), qPrintable(why));
        const qucs_s::dataset::Variable* pout = li.find(QStringLiteral("semi.Pout"));
        QVERIFY(pout);
        QCOMPARE(pout->dependencies.size(), 1);
        const qucs_s::dataset::Variable* x = li.find(pout->dependencies.front());
        QVERIFY2(x && x->independent && pout->dependencies.front().contains(QLatin1String("I_anode")), qPrintable(pout->dependencies.join(' ')));
        for (int k = 0; k < 5; ++k) {
            QCOMPARE(x->re.at(k), I[std::size_t(k)]);
            QCOMPARE(pout->re.at(k), P[std::size_t(k)]);
        }
        QVERIFY2(dataOf(QStringLiteral("pgiv"), iv), qPrintable(why));
        const qucs_s::dataset::Variable* cur = iv.find(QStringLiteral("semi.I_anode"));
        QVERIFY(cur && cur->dependencies == QStringList{QStringLiteral("voltage")});
        for (int k = 0; k < 5; ++k) QCOMPARE(iv.find(QStringLiteral("voltage"))->re.at(k), V[std::size_t(k)]);
    }

    void graphsOfTheChosenMode()
    {
        // A slab (µm) 2 wide, its thickness swept, two modes at 1550 nm: its
        // step chooses the more TM of them. A 1D plot group's graphs and a
        // derived value take that one alone - automatically, the step having
        // chosen -, each mode when asked; a Perturbation's VπL along the sweep.
        const QString file = path(QStringLiteral("slab_chosen.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "t", "expression": "0.3[um]"}],
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "label": "clad", "position": ["-1.5", "-1"], "size": ["3", "2"]},
                         {"type": "rectangle", "label": "core", "position": ["-1", "0"], "size": ["2", "t/1[um]"]}],
            "materials": [{"label": "oxide", "n": "1.45", "selection": {"only": ["clad"]}},
                          {"label": "silicon", "n": "3.4", "selection": {"only": ["core"]}}],
            "physics": [{"type": "modes", "tag": "emw", "order": "1"}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.15"}}],
          "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "sweep", "parameters": [["t", "0.3, 0.4", "um"]]},
                                 {"type": "mode", "frequency": "1550[nm]", "modes": 2, "neff": "3.4", "choose": "tm"}]}],
          "results": {"plots": [{"type": "plotgroup1d", "tag": "pga", "label": "Chosen", "study": "std1",
                                 "plots": [{"type": "globalgraph", "expression": "emw.neff; emw.TEfraction"},
                                           {"type": "globalgraph", "expression": "pert1.VpiL"}]},
                                {"type": "plotgroup1d", "tag": "pgall", "label": "Each", "study": "std1", "modes": "all",
                                 "plots": [{"type": "globalgraph", "expression": "emw.neff"}]}],
                      "derived": [{"type": "global", "tag": "gev1", "study": "std1", "expression": "emw.neff"},
                                  {"type": "global", "tag": "gev2", "study": "std1", "expression": "emw.neff", "solutions": "all"},
                                  {"type": "perturbation", "tag": "pert1", "study": "std1", "dn": "1e-3", "selection": {"only": ["core"]}, "bias": "t"}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute(QStringLiteral("std1"));
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        // Each point's chosen snapshot: one.
        const std::shared_ptr<SolutionSet> set = doc->solutions(QStringLiteral("std1"));
        QCOMPARE(int(set->solutions.size()), 2);
        double tm0[2] = {NAN, NAN}, all[4] = {NAN, NAN, NAN, NAN}, te[2][2];
        int second = 0;
        for (int p = 0; p < 2; ++p) {
            Solution& s = *set->solutions[std::size_t(p)];
            QCOMPARE(s.snapshotCount(), 2);
            int chosen = -1;
            for (int k = 0; k < 2; ++k) {
                s.select(k);
                all[k * 2 + p] = *s.global(QStringLiteral("emw.neff"));
                te[p][k] = *s.global(QStringLiteral("emw.TEfraction"));
                if (s.snapshotChosen(k)) {
                    QCOMPARE(chosen, -1);
                    chosen = k;
                    tm0[p] = *s.global(QStringLiteral("emw.neff"));
                }
            }
            // (The more TM of the two.)
            QVERIFY(chosen >= 0 && te[p][chosen] <= te[p][1 - chosen]);
            second += chosen == 1 ? 1 : 0;
        }
        // (Not the first: what is shown is not what was before.)
        QVERIFY(second > 0);
        QString why;
        auto dataOf = [&](const QString& group, qucs_s::dataset::Dataset& data) {
            const QString display = doc->showGraphs(group, &why, false);
            return !display.isEmpty() && data.read(display.left(display.size() - 4) + QStringLiteral(".dat"), &why);
        };
        qucs_s::dataset::Dataset data;
        QVERIFY2(dataOf(QStringLiteral("pga"), data), qPrintable(why));
        const qucs_s::dataset::Variable* neff = data.find(QStringLiteral("emw.neff"));
        QVERIFY(neff && data.find(QStringLiteral("mode")) == nullptr);
        QCOMPARE(neff->dependencies, QStringList{QStringLiteral("t")});
        QCOMPARE(neff->size(), 2);
        for (int p = 0; p < 2; ++p) QCOMPARE(neff->re.at(p), tm0[p]);
        for (int p = 0; p < 2; ++p) QCOMPARE(data.find(QStringLiteral("emw.TEfraction"))->re.at(p), std::min(te[p][0], te[p][1]));
        // The perturbation's VπL along the sweep: none at its first point.
        const qucs_s::dataset::Variable* vpil = data.find(QStringLiteral("VpiL"));
        QVERIFY2(vpil && vpil->size() == 2 && std::isnan(vpil->re.at(0)) && vpil->re.at(1) > 0, qPrintable(why));
        // Each mode when asked: an axis of them.
        QVERIFY2(dataOf(QStringLiteral("pgall"), data), qPrintable(why));
        neff = data.find(QStringLiteral("emw.neff"));
        QCOMPARE(neff->dependencies, (QStringList{QStringLiteral("t"), QStringLiteral("mode")}));
        for (int i = 0; i < 4; ++i) QCOMPARE(neff->re.at(i), all[i]);
        // A derived value: the chosen alone by default, each when asked.
        DerivedResult r = doc->evaluate(QStringLiteral("gev1"));
        QVERIFY2(r.error.isEmpty() && r.values.size() == 2, qPrintable(r.error + QString::number(r.values.size())));
        for (int p = 0; p < 2; ++p) QCOMPARE(r.values[p].value, tm0[p]);
        r = doc->evaluate(QStringLiteral("gev2"));
        QCOMPARE(r.values.size(), 4);
    }

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

    // A mode's profile from its plot group's menu (Export Mode Profile...):
    // the mode it shows, its six components on a grid over the window - its
    // perfectly matched layers left out - as a .mat read back; a Mode
    // Profile node's through fem_export (a binary file: its summary said,
    // not its bytes); a Mode Overlap's values through fem_evaluate.
    void aModesProfileExportedAndOverlapped()
    {
        const QString file = path(QStringLiteral("wire_profile.qfem"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "label": "pml", "base": "center", "size": ["4", "4"]},
                         {"type": "rectangle", "label": "oxide", "base": "center", "size": ["3", "3"]},
                         {"type": "rectangle", "label": "core", "base": "center", "size": ["0.5", "0.22"]},
                         {"type": "form union"}],
            "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["oxide", "pml"]}},
                          {"label": "silicon", "n": "3.476", "selection": {"only": ["core"]}}],
            "physics": [{"type": "modes", "tag": "wg", "order": "1", "features": [{"type": "pml", "selection": {"only": ["pml"]}}]}],
            "mesh": {"size": "coarse", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.06"},
                                                    {"type": "size", "level": "domain", "selection": {"only": ["oxide"]}, "hmax": "0.4"}]}}],
          "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 2, "neff": "2.8"}]}],
          "results": {"plots": [{"type": "plotgroup", "tag": "pg1", "label": "Second Mode", "study": "std1", "mode": 2,
                                 "plots": [{"type": "surface", "expression": "wg.normE"}]}],
                      "derived": [{"type": "overlap", "tag": "ovl1", "study": "std1", "with": "gaussian", "waist": "0.6[um]", "solutions": "all"}],
                      "exports": [{"type": "modeprofile", "tag": "mpr1", "study": "std1", "solutions": "all", "nx": "41", "ny": "31",
                                   "format": "mat", "file": "wire_profile.mat"}]}})json");
        f.close();
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        doc->compute(QStringLiteral("std1"));
        QVERIFY(doc->waitForJob(jobWait()));
        QVERIFY2(doc->lastError().isEmpty(), qPrintable(doc->lastError()));
        // The plot group's menu has it.
        QMenu* menu = panel()->menuFor(QStringLiteral("pg1"));
        QVERIFY(menu != nullptr);
        bool there = false;
        for (QAction* a : menu->actions()) there = there || a->objectName() == QLatin1String("mpExportModeProfile");
        delete menu;
        QVERIFY(there);
        // The mode it shows: the second, its effective index the solution's.
        const QString mat = path(QStringLiteral("wire_mode2.mat"));
        QString why;
        const QString said = doc->exportModeProfile(QStringLiteral("pg1"), mat, &why);
        QVERIFY2(!said.isEmpty(), qPrintable(why));
        QFile in(mat);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const std::optional<ModeProfile> p = readProfileMat(in.readAll(), 1, &why);
        QVERIFY2(p.has_value(), qPrintable(why));
        QCOMPARE(p->mode, 2);
        QJsonObject r = call("fem_evaluate", {{"expression", "wg.neff"}, {"mode", 2}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const double neff2 = json(r).value("values").toArray().at(0).toObject().value("SI").toDouble();
        QVERIFY2(std::abs(p->neff.real() - neff2) < 1e-9 && neff2 > 1.444 && neff2 < 3.476, qPrintable(QString::number(p->neff.real())));
        // Over the window (the oxide's 3 µm), not its layers.
        QCOMPARE(p->nx(), 201);
        QVERIFY2(std::abs(p->x.front() + 1.5e-6) < 1e-12 && std::abs(p->x.back() - 1.5e-6) < 1e-12, qPrintable(QString::number(p->x.front())));
        QVERIFY2(std::abs(p->y.front() + 1.5e-6) < 1e-12 && std::abs(p->y.back() - 1.5e-6) < 1e-12, qPrintable(QString::number(p->y.front())));
        QVERIFY2(std::abs(0.5 * p->flux().real() - 1) < 0.05, qPrintable(QString::number(0.5 * p->flux().real())));
        // The node through fem_export: both modes; the summary for the bytes.
        r = call("fem_export", {{"export", "mpr1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("written").toString().endsWith(QLatin1String("wire_profile.mat")));
        const QString begins = json(r).value("begins").toString();
        QVERIFY2(begins.contains(QLatin1String("[41 31 2]")) && begins.contains(QLatin1String("a 41 by 31 grid")), qPrintable(begins));
        // The overlap: of each mode with a Gaussian along x - the first (TE)
        // 0.46, the second (TM: its Ex odd) nought but for the mesh's
        // asymmetry. (A coarse mesh, first-order elements: a complex problem
        // with its layers, solved by Eigen's LU on the sanitizers' builds.)
        const DerivedResult o = doc->evaluate(QStringLiteral("ovl1"));
        QVERIFY2(o.error.isEmpty(), qPrintable(o.error));
        std::vector<double> overlaps;
        for (const DerivedValue& v : o.values)
            if (v.name.endsWith(QLatin1String(": overlap"))) overlaps.push_back(v.value);
        QCOMPARE(int(overlaps.size()), 2);
        qInfo("the modes with a Gaussian of 0.6 um along x: %.4f, %.3g", overlaps[0], overlaps[1]);
        QVERIFY2(overlaps[0] > 0.3 && overlaps[0] < 0.6 && overlaps[1] < 1e-3, qPrintable(QStringLiteral("%1 %2").arg(overlaps[0]).arg(overlaps[1])));
        r = call("fem_evaluate", {{"expression", "ovl1"}});
        QVERIFY2(!failed(r) && text(r).contains(QLatin1String("loss")) && text(r).contains(QLatin1String("overlapE")), qPrintable(text(r)));
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
        // Over Va alone, not the frequency step's one frequency (a data
        // display draws over a variable's first dependency).
        QCOMPARE(ia->dependencies, QStringList{QStringLiteral("Va")});
        QVERIFY(data.find(QStringLiteral("frequency")) == nullptr);
        const QString cv = doc->showGraphs(tagOf(doc->model(), "plotgroup1d", "Capacitance against Voltage"), &why, false);
        QVERIFY2(!cv.isEmpty(), qPrintable(why));
        qucs_s::dataset::Dataset cvData;
        QVERIFY(cvData.read(cv.left(cv.size() - 4) + QStringLiteral(".dat"), &why));
        QVERIFY(cvData.find(QStringLiteral("semi.Ckk")));
        QCOMPARE(cvData.find(QStringLiteral("semi.Ckk"))->dependencies, QStringList{QStringLiteral("Va")});
        QCOMPARE(cvData.find(QStringLiteral("semi.Ckk"))->size(), 3);
        QVERIFY(cvData.find(QStringLiteral("frequency")) == nullptr);
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(bare->waitForJob(jobWait()));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
            "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.5[nm]"}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "label": "Study 1", "steps": [{"type": "states", "states": 3}]}]})json");
        f.close();
        MultiphysicsDoc* bare = open(file);
        QVERIFY(bare != nullptr);
        bare->compute();
        QVERIFY(bare->waitForJob(jobWait()));
        QVERIFY2(bare->lastError().isEmpty(), qPrintable(bare->lastError()));
        QVERIFY(!tagOf(bare->model(), "plotgroup", "Electron Probability (qw)").isEmpty());
        const QString diagram = tagOf(bare->model(), "plotgroup1d", "Band Diagram (qw)");
        QVERIFY(!diagram.isEmpty());
        QVERIFY(!tagOf(bare->model(), "plotgroup1d", "The States").isEmpty());
        QVERIFY2(!bare->showGraphs(diagram, &why, false).isEmpty(), qPrintable(why));
        // Its electrons alone (a Schrödinger-Poisson region's): no holes'
        // probability nor their levels in the band diagram - not plots of
        // what is defined nowhere.
        const QString electrons = path(QStringLiteral("electrons_well.qfem"));
        QFile e(electrons);
        QVERIFY(e.open(QIODevice::WriteOnly | QIODevice::Truncate));
        e.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "label": "barrier", "size": ["10", "30"]},
                         {"type": "rectangle", "label": "well", "position": ["0", "10"], "size": ["10", "10"]}, {"type": "form union"}],
            "materials": [{"material": "Aluminium gallium arsenide", "selection": {"all": true}},
                          {"material": "Gallium arsenide", "selection": {"only": ["well"]}}],
            "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.5[nm]", "heavy": false, "light": false}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "label": "Study 1", "steps": [{"type": "states", "states": 3}]}]})json");
        e.close();
        MultiphysicsDoc* lone = open(electrons);
        QVERIFY(lone != nullptr);
        lone->compute();
        QVERIFY(lone->waitForJob(jobWait()));
        QVERIFY2(lone->lastError().isEmpty(), qPrintable(lone->lastError()));
        QVERIFY(!tagOf(lone->model(), "plotgroup", "Electron Probability (qw)").isEmpty());
        QVERIFY(tagOf(lone->model(), "plotgroup", "Heavy Hole Probability (qw)").isEmpty());
        const qucs_s::fem::Node* band = lone->model().find(tagOf(lone->model(), "plotgroup1d", "Band Diagram (qw)"));
        QVERIFY(band != nullptr);
        QStringList drawn;
        for (const qucs_s::fem::Node& g : band->children) drawn << g.text(QStringLiteral("expression"));
        QCOMPARE(drawn, (QStringList{QStringLiteral("qw.Ec"), QStringLiteral("qw.Ev"), QStringLiteral("qw.wave_e")}));
        for (const qucs_s::fem::Node& g : lone->model().results().children)
            if (g.type == QLatin1String("plotgroup")) QVERIFY2(lone->showPlotGroup(g.tag), qPrintable(g.label));
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

    // The update checks of 8bd5709 and 110e8b9, in the tools: a resonator's
    // globals said at the mode kept (they were "the last"), its figures'
    // note no VCSEL's for a cylinder; a sweep's seconds to the millisecond
    // (0.4460000000000003), the sweep's said in its log; a 1D plot whose x
    // does not read refused (drawn against its axis, as it was), its x in a
    // unit asked (xunit); an energy density in J/m³ (it was Pa).
    void theUpdateChecksInTheTools()
    {
        auto write = [&](const QString& name, const QString& json) {
            QFile f(path(name));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(json.toUtf8());
        };
        write(QStringLiteral("cylinder.qfem"), QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "mm", "space": "axisymmetric",
            "geometry": [{"type": "rectangle", "name": "cav", "position": ["0", "0"], "size": ["10", "20"]}],
            "materials": [{"label": "air", "n": "1", "selection": {"all": true}}],
            "physics": [{"type": "resonator", "tag": "emr", "m": 1, "features": [{"type": "wave", "selection": {"all": true}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "steps": [{"type": "eigenfrequency", "count": 3, "around": "15[GHz]", "keep": 1}]}]})json"));
        QVERIFY(open(path(QStringLiteral("cylinder.qfem"))) != nullptr);
        QJsonObject r = call("fem_solve", {{"path", "cylinder.qfem"}, {"timeout", 120}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject solved = json(r);
        QCOMPARE(solved.value("frequencies").toObject().value("globals at").toString(), QStringLiteral("mode 1, the one kept for the steps after"));
        QCOMPARE(solved.value("globals").toObject().value("emr.mode").toString(), QStringLiteral("1"));
        QVERIFY2(!solved.value("figures").toString().contains(QLatin1String("VCSEL")), qPrintable(solved.value("figures").toString()));
        QJsonArray atPoint;   // (a list of one point: QJsonArray{QJsonArray{...}} is a copy)
        atPoint.append(QJsonArray{5, 10});
        r = call("fem_evaluate", {{"path", "cylinder.qfem"}, {"expression", "emr.we"}, {"at", atPoint}});
        QVERIFY2(!failed(r) && text(r).contains(QString::fromUtf8("J/m³")) && !text(r).contains(QLatin1String(" Pa")), qPrintable(text(r)));
        write(QStringLiteral("bars.qfem"), QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "L", "expression": "10[mm]"}],
          "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "bar", "size": ["L", "1"]}],
            "materials": [{"label": "m", "sigma": "1e6[S/m]", "selection": {"all": true}}],
            "physics": [{"type": "currents", "tag": "ec", "features": [
              {"type": "terminal", "name": "1", "V0": "1[mV]", "selection": {"of": ["bar"], "side": "left"}},
              {"type": "ground", "selection": {"of": ["bar"], "side": "right"}}]}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["L", "10, 20, 40", "mm"]]}, {"type": "stationary"}]}],
          "results": {"plots": [{"type": "plotgroup1d", "tag": "badx", "x": "nosuch", "plots": [{"type": "globalgraph", "expression": "ec.G11"}]},
                                {"type": "plotgroup1d", "tag": "inmA", "x": "ec.I_1", "xunit": "mA", "plots": [{"type": "globalgraph", "expression": "ec.G11"}]}]}})json"));
        QVERIFY(open(path(QStringLiteral("bars.qfem"))) != nullptr);
        r = call("fem_solve", {{"path", "bars.qfem"}, {"timeout", 120}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const double seconds = json(r).value("seconds").toDouble();
        // (Exactly the double nearest a whole number of ms: 0.4460000000000003
        // is 3e-16 off it.)
        QVERIFY2(seconds == std::round(seconds * 1000) / 1000, qPrintable(QString::number(seconds, 'g', 17)));
        const QJsonArray log = json(r).value("log").toArray();
        QVERIFY2(log.last().toString().startsWith(QLatin1String("The sweep: 3 points in")), qPrintable(log.last().toString()));
        // Its points at once: said, its seconds the sweep's own (theirs
        // overlapping, summed they were more).
        {
            QFile bars(path(QStringLiteral("bars.qfem")));
            QVERIFY(bars.open(QIODevice::ReadOnly));
            QByteArray text = bars.readAll();
            bars.close();
            QVERIFY(text.contains(R"("parameters": [["L", "10, 20, 40", "mm"]])") || text.contains(R"("parameters":[["L","10, 20, 40","mm"]])"));
            text.replace(R"("parameters": [["L", "10, 20, 40", "mm"]])", R"("parameters": [["L", "10, 20, 40", "mm"]], "parallel": 3)");
            text.replace(R"("parameters":[["L","10, 20, 40","mm"]])", R"("parameters":[["L","10, 20, 40","mm"]],"parallel":3)");
            write(QStringLiteral("bars_at_once.qfem"), QString::fromUtf8(text));
            QVERIFY(open(path(QStringLiteral("bars_at_once.qfem"))) != nullptr);
            const QJsonObject at = json(call("fem_solve", {{"path", "bars_at_once.qfem"}, {"timeout", 120}}));
            const QJsonArray lines = at.value("log").toArray();
            QVERIFY2(lines.last().toString().startsWith(QLatin1String("The sweep: 3 points, 3 at once, in")), qPrintable(lines.last().toString()));
            QVERIFY(at.value("seconds").toDouble() >= 0);
        }
        r = call("fem_plot", {{"path", "bars.qfem"}, {"plot", "badx"}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("its x, nosuch,")), qPrintable(text(r)));
        r = call("fem_plot", {{"path", "bars.qfem"}, {"plot", "inmA"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QFile dataset(json(r).value("dataset").toString());
        QVERIFY(dataset.open(QIODevice::ReadOnly));
        const QString data = QString::fromUtf8(dataset.readAll());
        // (1 mV over 10 mm of a 1 mm bar of 1 MS/m, a metre thick: 100 A -
        // 1e5 mA.)
        const QStringList lines = data.split(QLatin1Char('\n'));
        const int at = int(lines.indexOf(QStringLiteral("<indep ec.I_1 3>")));
        QVERIFY2(at >= 0, qPrintable(data.left(400)));
        QVERIFY2(std::abs(lines.at(at + 1).trimmed().toDouble() / 1e5 - 1) < 1e-9, qPrintable(lines.at(at + 1)));
    }

    // The MQW calibration's tool notes:
    // - a plot group's tag a study's already: renamed, the reading said so,
    //   and the plot groups there are named when the tag is asked for;
    // - two models open, one's display in front: a call without 'path' is
    //   that model's; something else in front, the model last acted on;
    // - all the states of a sweep: a global the same at each state once per
    //   point, 'state' (or 'mode') taking one alone;
    // - solved (its default plots made), the model still as saved: written
    //   anew on disk, read again.
    void claudesToolsAfterTheCalibration()
    {
        auto write = [&](const QString& name, const QString& plotTag, const QString& title) {
            const QString json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1", "title": "%3",
              "parameters": [{"name": "F", "expression": "0[V/m]"}],
              "components": [{"unit": "nm", "geometry": [{"type": "rectangle", "name": "well", "size": ["20", "10"]}],
                "physics": [{"type": "quantum", "tag": "qw", "F": "F", "features": [
                  {"type": "material", "source": "user", "Ec": "0[eV]", "Ev": "-1.5[eV]", "me": "0.067", "mhh": "0.35", "mlh": "0.09",
                   "Ep": "25[eV]", "nr": "3.6", "selection": {"all": true}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
              "studies": [{"type": "study", "tag": "conv", "steps": [{"type": "sweep", "parameters": [["F", "0, 1e6", "V/m"]]},
                                                                     {"type": "states", "states": 3}]}],
              "results": {"plots": [{"type": "plotgroup1d", "tag": "%2", "label": "Levels", "study": "conv",
                                     "plots": [{"type": "globalgraph", "expression": "qw.Ee1"}]}]}})json")
                                     .arg(plotTag, title);
            QFile f(path(name));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(json.toUtf8());
        };
        write(QStringLiteral("calib_a.qfem"), QStringLiteral("conv"), QStringLiteral("A"));
        write(QStringLiteral("calib_b.qfem"), QStringLiteral("pg1"), QStringLiteral("B"));
        MultiphysicsDoc* a = open(path(QStringLiteral("calib_a.qfem")));
        QVERIFY(a != nullptr);
        QJsonObject r = call("fem_describe", {{"path", "calib_a.qfem"}});
        QVERIFY2(json(r).value("read with notes").toString().contains(QLatin1String("Levels's tag conv is another node's already: it is pg1d1")),
                 qPrintable(text(r)));
        r = call("fem_solve", {{"path", "calib_a.qfem"}, {"timeout", 120}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_plot", {{"path", "calib_a.qfem"}, {"plot", "conv"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QLatin1String("conv is Study, not a plot group")) && text(r).contains(QLatin1String("pg1d1 (Levels)")), qPrintable(text(r)));
        // B's display in front.
        MultiphysicsDoc* b = open(path(QStringLiteral("calib_b.qfem")));
        QVERIFY(b != nullptr);
        r = call("fem_solve", {{"path", "calib_b.qfem"}, {"timeout", 120}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_plot", {{"path", "calib_b.qfem"}, {"plot", "pg1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget()) == nullptr);
        // (A the one last acted on: the display in front says B all the same.)
        QVERIFY(!failed(call("fem_describe", {{"path", "calib_a.qfem"}})));
        QVERIFY(qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget()) == nullptr);
        r = call("fem_describe", {});
        QVERIFY2(!failed(r) && json(r).value("file").toString().endsWith(QLatin1String("calib_b.qfem")), qPrintable(text(r)));
        // A global the same at each state: once per point.
        r = call("fem_evaluate", {{"path", "calib_b.qfem"}, {"expression", "qw.Ee1"}, {"all", true}});
        QJsonArray values = json(r).value("values").toArray();
        QCOMPARE(values.size(), 2);
        QCOMPARE(values.at(0).toObject().value("name").toString(), QStringLiteral("F = 0 V/m: qw.Ee1"));
        QVERIFY(json(r).value("note").toString().contains(QLatin1String("once for the point")));
        r = call("fem_evaluate", {{"path", "calib_b.qfem"}, {"expression", "qw.E_e"}, {"all", true}});
        QCOMPARE(json(r).value("values").toArray().size(), 6);
        r = call("fem_evaluate", {{"path", "calib_b.qfem"}, {"expression", "qw.E_e"}, {"all", true}, {"state", 2}});
        values = json(r).value("values").toArray();
        QCOMPARE(values.size(), 2);
        QVERIFY(values.at(1).toObject().value("name").toString().contains(QLatin1String("state 2")));
        const QJsonObject e2 = json(call("fem_evaluate", {{"path", "calib_b.qfem"}, {"expression", "qw.Ee2"}, {"point", 1}}));
        QCOMPARE(values.at(0).toObject().value("SI").toDouble(), e2.value("values").toArray().at(0).toObject().value("SI").toDouble());
        // A text file in front: the model last acted on - B, then A.
        {
            QFile t(path(QStringLiteral("calib_notes.txt")));
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write("notes\n");
        }
        QVERIFY(app->gotoPage(path(QStringLiteral("calib_notes.txt"))));
        r = call("fem_describe", {});
        QVERIFY2(!failed(r) && json(r).value("file").toString().endsWith(QLatin1String("calib_b.qfem")), qPrintable(text(r)));
        QVERIFY(!failed(call("fem_describe", {{"path", "calib_a.qfem"}})));
        QVERIFY(app->gotoPage(path(QStringLiteral("calib_notes.txt"))));
        r = call("fem_describe", {});
        QVERIFY2(!failed(r) && json(r).value("file").toString().endsWith(QLatin1String("calib_a.qfem")), qPrintable(text(r)));
        // Solved, its default plots made: still as saved; written anew, read again.
        QVERIFY(!b->getDocChanged());
        QTest::qWait(1100);
        write(QStringLiteral("calib_b.qfem"), QStringLiteral("pg1"), QStringLiteral("B changed on disk"));
        QTRY_COMPARE_WITH_TIMEOUT(b->model().root.label, QStringLiteral("B changed on disk"), 10000);
    }

    // The quantum wells' numbers as one reads them (the update check of 13
    // October, B3): energies in eV - the globals, the solve's, a Derived
    // Values node's -, a unit for each expression of several or one for
    // those it fits, qw.k in 1/nm, qw.rsp in 1/(s cm³ eV) converted, the
    // Kramers-Kronig warning's window in eV; a global the same at each
    // photon energy given once for its point.
    // The tree while a study solves: its mesh, made by it, "being made" (not
    // "not built"), the study "computing"; after, the mesh's triangles and
    // "solved". A model saved under a name: its name at the tree's root.
    void theTreeWhileItSolves()
    {
        auto status = [&](const QString& name) {
            QTreeWidgetItemIterator it(panel()->tree());
            while (*it) {
                if ((*it)->text(0) == name) return (*it)->text(1);
                ++it;
            }
            return QStringLiteral("(no %1)").arg(name);
        };
        MultiphysicsDoc* doc = open(copyOf(QStringLiteral("diode_reverse_recovery.qfem")));
        QVERIFY(doc != nullptr);
        QCOMPARE(status(QStringLiteral("Mesh")), QStringLiteral("not built"));
        const QString study = doc->model().studies().front()->name();
        QJsonObject r = call("fem_solve", {{"timeout", 1}});
        QVERIFY2(failed(r) && doc->isBusy(), qPrintable(text(r)));
        QCOMPARE(status(QStringLiteral("Mesh")), QStringLiteral("being made"));
        QCOMPARE(status(study), QStringLiteral("computing"));
        // (Stopped once its mesh is made - past its first 30 % -: stopped
        // while meshing, as CI Linux's sanitized build was, there is none.)
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() || doc->jobProgress().section(QLatin1Char(' '), 0, 0).toInt() >= 30, 600000);
        r = call("fem_stop", {{"timeout", 600}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(status(QStringLiteral("Mesh")).endsWith(QLatin1String("triangles")));
        QVERIFY2(status(study).isEmpty(), qPrintable(status(study)));
        // Made untitled, then saved: the root its file's name.
        r = call("fem_model", {{"action", "new"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(panel()->tree()->topLevelItem(0)->text(0), QStringLiteral("untitled.qfem"));
        const QString named = path(QStringLiteral("named_later.qfem"));
        QFile::remove(named);
        r = call("fem_model", {{"action", "save"}, {"path", named}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(panel()->tree()->topLevelItem(0)->text(0), QStringLiteral("named_later.qfem"));
    }

    // fem_model save names its model by 'path', as the other tools do -
    // whichever model is in front: two conversations, each with its own
    // model, the other's brought to the front no longer written over this
    // one's file (it was: the model in front saved to 'path', renamed so).
    // 'as' saves it under another file ('replace' over one there); a path no
    // file has is the untitled model in front's new file, never a titled
    // one's; 'new' says its title, which names it till it has a file.
    // fem_solve's sweep: each point's globals its chosen mode's, as
    // fem_evaluate's (its last point said its last mode: two modes asked,
    // the last point mode 2, the others mode 1). A mode not there refused.
    void aSweepsModesAsChosen()
    {
        const QString file = path(QStringLiteral("sweep_modes.qfem"));
        QFile::remove(file);
        QJsonArray rows;   // (by append: a braced list of one list is a copy to some compilers)
        rows.append(QJsonArray{"nc", "3.4, 3.45, 3.5", ""});
        const QJsonObject model{{"format", "qucs-s multiphysics 1"},
                                {"parameters", QJsonArray{QJsonObject{{"name", "nc"}, {"expression", "3.4"}}}},
                                {"components", QJsonArray{QJsonObject{
                                    {"unit", "um"},
                                    {"geometry", QJsonArray{QJsonObject{{"type", "rectangle"}, {"name", "clad"}, {"position", QJsonArray{"-2", "-2"}}, {"size", QJsonArray{"4", "4"}}},
                                                            QJsonObject{{"type", "rectangle"}, {"name", "core"}, {"position", QJsonArray{"-0.5", "-0.25"}}, {"size", QJsonArray{"1", "0.5"}}}}},
                                    {"materials", QJsonArray{QJsonObject{{"label", "clad"}, {"n", "1.44"}, {"selection", QJsonObject{{"only", QJsonArray{"clad"}}}}},
                                                             QJsonObject{{"label", "core"}, {"n", "nc"}, {"selection", QJsonObject{{"only", QJsonArray{"core"}}}}}}},
                                    {"physics", QJsonArray{QJsonObject{{"type", "modes"}, {"tag", "emw"}}}},
                                    {"mesh", QJsonObject{{"size", "coarser"}}}}}},
                                {"studies", QJsonArray{QJsonObject{{"type", "study"},
                                                                   {"steps", QJsonArray{QJsonObject{{"type", "sweep"}, {"parameters", rows}},
                                                                                        QJsonObject{{"type", "mode"}, {"frequency", "1550[nm]"}, {"modes", 2}, {"neff", "3"}}}}}}}};
        QJsonObject r = call("fem_model", {{"action", "new"}, {"path", file}, {"model", model}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_solve", {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray each = json(r).value("sweep").toObject().value("each").toArray();
        QCOMPARE(each.size(), 3);
        for (const QJsonValue& p : each) QCOMPARE(p.toObject().value("globals").toObject().value("emw.mode").toString(), QStringLiteral("1"));
        QCOMPARE(json(r).value("globals").toObject().value("emw.mode").toString(), QStringLiteral("1"));
        // Its seconds: the sweep's, each point's together (the last one's said).
        {
            double total = 0, last = 0;
            for (QucsDoc* d : app->allDocuments())
                if (auto* m = dynamic_cast<MultiphysicsDoc*>(d); m && QFileInfo(m->getDocName()) == QFileInfo(file))
                    for (const auto& one : m->solutions(QString())->solutions) {
                        total += one->seconds;
                        last = one->seconds;
                    }
            QVERIFY2(std::abs(json(r).value("seconds").toDouble() - total) < 1e-9 && total > last, qPrintable(QStringLiteral("%1 %2").arg(total).arg(last)));
        }
        // A mode it has not: refused, said (mode 3 of 2 gave the 2nd, 0 the
        // 1st; a plot group took "mode": 9 and showed the last).
        for (int mode : {3, 0}) {
            r = call("fem_evaluate", {{"path", file}, {"expression", "emw.neff"}, {"mode", mode}});
            QVERIFY2(failed(r) && text(r).contains("from 1 to 2"), qPrintable(text(r)));
        }
        r = call("fem_evaluate", {{"path", file}, {"expression", "emw.neff"}, {"mode", 2}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_plot", {{"path", file}, {"expression", "emw.normE"}, {"mode", 9}});
        QVERIFY2(failed(r) && text(r).contains("from 1 to 2"), qPrintable(text(r)));
        for (QucsDoc* d : app->allDocuments())
            if (QFileInfo(d->getDocName()) == QFileInfo(file)) {
                d->setDocChanged(false);
                app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(d)));
                break;
            }
    }

    // A resonator's: fem_solve says its seven figures are its mesh's; its
    // plots made for it no arrows (|E| and E_φ).
    void aResonatorsFiguresAndPlots()
    {
        const QString file = path(QStringLiteral("pillbox.qfem"));
        QFile::remove(file);
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um", "space": "axisymmetric",
            "geometry": [{"type": "rectangle", "name": "box", "size": ["1", "1"]}],
            "materials": [{"label": "glass", "n": "3.5", "kappa": "1e-3", "selection": {"all": true}}],
            "physics": [{"type": "resonator", "tag": "emr", "m": 0}],
            "mesh": {"size": "coarse"}}],
          "studies": [{"type": "study", "steps": [{"type": "eigenfrequency", "count": 2, "around": "9[um]"}]}]})json");
        f.close();
        QJsonObject r = call("fem_solve", {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("figures").toString().contains("finer mesh"), qPrintable(text(r).left(600)));
        MultiphysicsDoc* doc = nullptr;
        for (QucsDoc* d : app->allDocuments())
            if (auto* m = dynamic_cast<MultiphysicsDoc*>(d); m && QFileInfo(m->getDocName()) == QFileInfo(file)) doc = m;
        QVERIFY(doc);
        int groups = 0;
        for (const qucs_s::fem::Node& g : doc->model().results().children) {
            if (g.type != QLatin1String("plotgroup")) continue;
            ++groups;
            for (const qucs_s::fem::Node& p : g.children) QVERIFY2(p.type != QLatin1String("arrow"), qPrintable(g.name()));
        }
        QVERIFY(groups >= 2);
        doc->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(doc)));
    }

    // fem_model new over a model's file open in a tab ('replace'): that tab
    // takes the new model - one tab of the file, the tools on it (a second
    // tab was made, the tools found the first: its old model, nothing said);
    // its changes not saved refused, not thrown away.
    void newOverAnOpenModel()
    {
        const QString file = path(QStringLiteral("replaced.qfem"));
        QFile::remove(file);
        auto model = [](const char* value) {
            return QJsonObject{{"format", "qucs-s multiphysics 1"},
                               {"parameters", QJsonArray{QJsonObject{{"name", "p"}, {"expression", QString::fromLatin1(value)}}}},
                               {"components", QJsonArray{QJsonObject{{"geometry", QJsonArray{QJsonObject{{"type", "rectangle"}, {"size", QJsonArray{"1", "1"}}}}}}}}};
        };
        auto tabsOf = [&]() {
            QList<MultiphysicsDoc*> out;
            for (QucsDoc* d : app->allDocuments())
                if (auto* m = dynamic_cast<MultiphysicsDoc*>(d); m && QFileInfo(m->getDocName()) == QFileInfo(file)) out << m;
            return out;
        };
        auto parameterOf = [](MultiphysicsDoc* d) {
            for (const auto& p : d->model().parameters())
                if (p.name == QLatin1String("p")) return p.expression;
            return QString();
        };
        QJsonObject r = call("fem_model", {{"action", "new"}, {"path", file}, {"model", model("1")}, {"replace", true}});
        QVERIFY2(!failed(r) && tabsOf().size() == 1, qPrintable(text(r)));
        MultiphysicsDoc* first = tabsOf().front();
        r = call("fem_model", {{"action", "new"}, {"path", file}, {"model", model("2")}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(tabsOf().size(), 1);
        QCOMPARE(tabsOf().front(), first);
        QCOMPARE(parameterOf(first), QStringLiteral("2"));
        QVERIFY(!first->getDocChanged());
        // (The tools on it: the file's model.)
        r = call("fem_describe", {{"path", file}, {"what", "model"}});
        QVERIFY2(!failed(r) && text(r).contains("\"2\""), qPrintable(text(r).left(400)));
        // Its changes not saved: refused, kept.
        r = call("fem_edit", {{"path", file}, {"action", "parameters"}, {"parameters", QJsonArray{QJsonObject{{"name", "p"}, {"expression", "3"}}}}});
        QVERIFY2(!failed(r) && first->getDocChanged(), qPrintable(text(r)));
        r = call("fem_model", {{"action", "new"}, {"path", file}, {"model", model("4")}, {"replace", true}});
        QVERIFY2(failed(r) && text(r).contains("not saved"), qPrintable(text(r)));
        QCOMPARE(parameterOf(first), QStringLiteral("3"));
        QCOMPARE(tabsOf().size(), 1);
        first->setDocChanged(false);
        app->slotFileClose(app->DocumentTab->indexOf(QucsApp::documentWidget(first)));
    }

    // fem_model new by a relative path (the reviewer's round of 18
    // October): its folders made; a folder of the workspace's name, not the
    // open project's, said; a write that fails leaves no tab.
    void claudesNewModelsFolders()
    {
        const QString project = path(QStringLiteral("projA"));
        QDir().mkpath(project);
        QDir().mkpath(path(QStringLiteral("gssg_rf")));
        const QString was = QucsSettings.QucsWorkDir.absolutePath();
        QucsSettings.QucsWorkDir.setPath(project);
        auto restore = qScopeGuard([&] { QucsSettings.QucsWorkDir.setPath(was); });
        auto tabs = [&]() {
            int n = 0;
            for (QucsDoc* d : app->allDocuments())
                if (dynamic_cast<MultiphysicsDoc*>(d)) ++n;
            return n;
        };
        // A folder there is none of: made, in the project.
        QJsonObject r = call("fem_model", {{"action", "new"}, {"path", "deep/er/a.qfem"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(project + QStringLiteral("/deep/er/a.qfem")));
        QVERIFY2(json(r).value("note").toString().contains("made"), qPrintable(text(r)));
        // The workspace's folder's name: it went to the project's, said so.
        r = call("fem_model", {{"action", "new"}, {"path", "gssg_rf/b.qfem"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(project + QStringLiteral("/gssg_rf/b.qfem")));
        const QString note = json(r).value("note").toString();
        QVERIFY2(note.contains("gssg_rf is a folder of the workspace") && note.contains("projA"), qPrintable(note));
        // A folder that cannot be written: refused, no tab left of it.
        const QString locked = project + QStringLiteral("/locked");
        QDir().mkpath(locked);
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        auto unlock = qScopeGuard([&] { QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); });
        const int before = tabs();
        r = call("fem_model", {{"action", "new"}, {"path", "locked/c.qfem"}});
        QVERIFY2(failed(r) && text(r).contains("cannot be written"), qPrintable(text(r)));
        QCOMPARE(tabs(), before);
        // A folder where a file is: cannot be made, said.
        QFile blocker(project + QStringLiteral("/afile"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        r = call("fem_model", {{"action", "new"}, {"path", "afile/d.qfem"}});
        QVERIFY2(failed(r) && text(r).contains("cannot be made"), qPrintable(text(r)));
        QCOMPARE(tabs(), before);
    }

    // fem_describe what=format of a group: its kinds, the text's paragraphs
    // that name them, no library; 'text' all or none. Each kind's
    // properties a line each, its fields said once ('properties as'): a
    // group's kinds were two thirds JSON's names and marks.
    void claudesFormatOfAGroup()
    {
        QJsonObject r = call("fem_describe", {{"what", "format"}});
        const QString whole = json(r).value("format").toString();
        const int all = int(text(r).size());
        QVERIFY(json(r).contains("materials"));
        r = call("fem_describe", {{"what", "format"}, {"group", "mf"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject mf = json(r);
        const QString part = mf.value("format").toString();
        QVERIFY2(part.contains("Magnetic fields (\"magnetic\", tag mf)") && part.size() < whole.size() / 3, qPrintable(part.left(300)));
        QVERIFY(!part.contains("Semiconductors:"));
        QVERIFY(!mf.contains("materials"));
        QVERIFY2(mf.value("note").toString().contains("'text' all"), qPrintable(mf.value("note").toString()));
        QVERIFY(int(text(r).size()) < all / 4);
        QStringList types;
        for (const QJsonValue& k : mf.value("kinds").toArray()) types << k.toObject().value("type").toString();
        QVERIFY2(types.contains("magnetic/coil") && !types.contains("rectangle"), qPrintable(types.join(", ")));
        QVERIFY(mf.value("properties as").toString().startsWith("key: label [unit]"));
        r = call("fem_describe", {{"what", "format"}, {"group", "semi"}});
        QStringList lines;
        for (const QJsonValue& k : json(r).value("kinds").toArray())
            if (k.toObject().value("type").toString() == "semiconductor/model")
                for (const QJsonValue& p : k.toObject().value("properties").toArray()) lines << p.toString();
        QVERIFY2(lines.contains("Eg: Band gap Eg [eV] = 1.12[eV]; when source=user") && lines.contains("source: From = material; one of material, user")
                     && lines.contains("selection: Selection (selection) = {\"all\":true}"),
                 qPrintable(lines.join("\n")));
        // The derived values: the line's paragraph among them.
        r = call("fem_describe", {{"what", "format"}, {"group", "derived"}});
        QVERIFY2(json(r).value("format").toString().contains("lineparams"), qPrintable(text(r).left(300)));
        // All of it, or none of it.
        r = call("fem_describe", {{"what", "format"}, {"group", "mf"}, {"text", "all"}});
        QCOMPARE(json(r).value("format").toString(), whole);
        r = call("fem_describe", {{"what", "format"}, {"text", "none"}});
        QVERIFY(!json(r).contains("format") && json(r).contains("kinds"));
        // The library's materials with a group of materials.
        r = call("fem_describe", {{"what", "format"}, {"group", "material"}});
        QVERIFY(json(r).contains("materials"));
    }

    void savingTheModelItsPathNames()
    {
        auto read = [](const QString& file) {
            QFile f(file);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        const QString a = copyOf(QStringLiteral("drum_modes.qfem")), b = copyOf(QStringLiteral("pn_diode.qfem"));
        MultiphysicsDoc* docA = open(a);
        MultiphysicsDoc* docB = open(b);
        QVERIFY(docA && docB && docA != docB);
        const QByteArray bBefore = read(b);
        // A changed by its conversation, through its path; B in front.
        QJsonObject r = call("fem_edit", {{"path", a}, {"action", "parameters"}, {"parameters", QJsonArray{QJsonObject{{"name", "mineA"}, {"expression", "7"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        app->DocumentTab->setCurrentWidget(docB);
        r = call("fem_model", {{"action", "save"}, {"path", a}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(read(a).contains("mineA"));
        QCOMPARE(read(b), bBefore);
        QCOMPARE(json(r).value("model").toString(), QStringLiteral("drum_modes.qfem"));
        QVERIFY(QFileInfo(docB->getDocName()) == QFileInfo(b) && QFileInfo(docA->getDocName()) == QFileInfo(a));
        QVERIFY(!docA->getDocChanged());
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(docB));
        // By its title too.
        call("fem_edit", {{"path", a}, {"action", "parameters"}, {"parameters", QJsonArray{QJsonObject{{"name", "mineA"}, {"expression", "8"}}}}});
        r = call("fem_model", {{"action", "save"}, {"path", "drum_modes.qfem"}});
        QVERIFY2(!failed(r) && !docA->getDocChanged() && read(b) == bBefore, qPrintable(text(r)));
        // 'as': B under another file, kept as its file; over a file there
        // only with 'replace'.
        const QString c = path(QStringLiteral("pn_copy.qfem"));
        QFile::remove(c);
        r = call("fem_model", {{"action", "save"}, {"path", b}, {"as", c}});
        QVERIFY2(!failed(r) && QFileInfo(docB->getDocName()) == QFileInfo(c) && read(c).size() > 0, qPrintable(text(r)));
        r = call("fem_model", {{"action", "save"}, {"path", c}, {"as", b}});
        QVERIFY2(failed(r) && text(r).contains("replace"), qPrintable(text(r)));
        r = call("fem_model", {{"action", "save"}, {"path", c}, {"as", a}, {"replace", true}});
        QVERIFY2(failed(r) && text(r).contains("open"), qPrintable(text(r)));   // (A's file, open)
        r = call("fem_model", {{"action", "save"}, {"path", c}, {"as", b}, {"replace", true}});
        QVERIFY2(!failed(r) && QFileInfo(docB->getDocName()) == QFileInfo(b), qPrintable(text(r)));
        // A path no file has, a titled model in front: refused, nothing
        // renamed or written.
        const QString d = path(QStringLiteral("not_yet.qfem"));
        QFile::remove(d);
        r = call("fem_model", {{"action", "save"}, {"path", d}});
        QVERIFY2(failed(r) && text(r).contains("pn_diode.qfem") && !QFileInfo::exists(d), qPrintable(text(r)));
        QVERIFY(QFileInfo(docB->getDocName()) == QFileInfo(b));
        // An untitled model: its title from 'new' names it; saved by it with
        // 'as', whichever model is in front.
        r = call("fem_model", {{"action", "new"}});
        const QString title = json(r).value("title").toString();
        QVERIFY2(!failed(r) && !title.isEmpty(), qPrintable(text(r)));
        auto* fresh = qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
        app->DocumentTab->setCurrentWidget(docA);
        r = call("fem_model", {{"action", "save"}, {"path", title}, {"as", d}});
        QVERIFY2(!failed(r) && fresh && QFileInfo(fresh->getDocName()) == QFileInfo(d), qPrintable(text(r)));
        QVERIFY(QFileInfo(docA->getDocName()) == QFileInfo(a));
    }

    // Claude stops a solve that goes on past fem_solve's timeout: fem_stop,
    // as the tab's Cancel - asked, waited for, what it said returned; with
    // nothing running, said so. The tab's own Cancel among get_ui's
    // document controls while it solves (the areas say so), and set_ui
    // presses it.
    void claudeStopsASolve()
    {
        MultiphysicsDoc* doc = open(copyOf(QStringLiteral("diode_reverse_recovery.qfem")));
        QVERIFY(doc != nullptr);
        QJsonObject r = call("fem_stop");
        QVERIFY2(!failed(r) && !json(r).value("stopped").toBool() && text(r).contains("Nothing is being built or solved"), qPrintable(text(r)));
        r = call("fem_solve", {{"timeout", 1}});
        QVERIFY2(failed(r) && text(r).contains("fem_stop stops it"), qPrintable(text(r)));
        QVERIFY(doc->isBusy());
        r = call("fem_stop", {{"timeout", 600}});
        QVERIFY2(!failed(r) && json(r).value("stopped").toBool() && text(r).contains("Cancelling"), qPrintable(text(r)));
        QVERIFY(!doc->isBusy());
        QVERIFY2(doc->lastError().contains("cancelled"), qPrintable(doc->lastError()));
        // The tab's Cancel, by the UI tools.
        QVERIFY(text(call("get_ui")).contains("its Cancel"));
        r = call("fem_solve", {{"timeout", 1}});
        QVERIFY(failed(r) && doc->isBusy());
        r = call("get_ui", {{"area", "document"}});
        QVERIFY2(text(r).contains("\"label\":\"Cancel\""), qPrintable(text(r)));
        r = call("set_ui", {{"area", "document"}, {"press", "Cancel"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(doc->waitForJob(600000));
        QVERIFY2(doc->lastError().contains("cancelled"), qPrintable(doc->lastError()));
        // A long step at rest stopped between its Newton steps, not once it
        // is done: the avalanche junction at 0 V, then taken at once to 54 V
        // on a fine mesh (its contacts' voltage in steps, in the second
        // point's solve), solved whole, then a copy asked to stop a tenth of
        // the way in - stopped in a fraction of the rest, said as cancelled
        // (not as a solution not found).
        auto atOnce = [&](const QString& name, const QString& points = QStringLiteral("0, 54")) {
            const QString file = copyOf(QStringLiteral("avalanche_breakdown.qfem"));
            const QString to = path(name);
            QFile::remove(to);
            QFile::rename(file, to);
            QFile f(to);
            f.open(QIODevice::ReadOnly);
            QString t = QString::fromUtf8(f.readAll());
            f.close();
            t.replace(QStringLiteral("\"10, 20, 30, 40, 46, 48, 50, 51, 52, 53, 54\""), QStringLiteral("\"%1\"").arg(points));
            t.replace(QStringLiteral("\"hmax\": \"0.05\""), slowBuild() ? QStringLiteral("\"hmax\": \"0.1\"") : QStringLiteral("\"hmax\": \"0.015\""));
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write(t.toUtf8());
            f.close();
            return to;
        };
        MultiphysicsDoc* whole = open(atOnce(QStringLiteral("at_once_whole.qfem")));
        QVERIFY(whole != nullptr);
        QElapsedTimer clock;
        clock.start();
        whole->compute();
        QVERIFY(whole->waitForJob(600000));
        QVERIFY2(whole->lastError().isEmpty(), qPrintable(whole->lastError()));
        const qint64 full = clock.elapsed();
        MultiphysicsDoc* stopped = open(atOnce(QStringLiteral("at_once_stopped.qfem")));
        QVERIFY(stopped != nullptr);
        stopped->compute();
        QTest::qWait(int(std::max<qint64>(full / 10, 50)));
        QVERIFY(stopped->isBusy());
        clock.restart();
        r = call("fem_stop", {{"timeout", 600}});
        const qint64 after = clock.elapsed();
        qInfo("at rest: solved whole in %lld ms; asked to stop a tenth of the way in, stopped %lld ms after", full, after);
        QVERIFY2(!failed(r) && json(r).value("stopped").toBool(), qPrintable(text(r)));
        // (Its first point kept, the sweep stopped at its second: said so,
        // not an error - its points the study's - as cancelled, not as a
        // solution not found.)
        const std::shared_ptr<SolutionSet> kept = stopped->solutions({});
        QVERIFY2(stopped->lastError().isEmpty() && kept && kept->solutions.size() == 1, qPrintable(stopped->lastError()));
        QVERIFY2(kept->stopped.contains(QLatin1String("(point 2 of 2): ")) && kept->stopped.contains(QLatin1String("cancelled"))
                     && !kept->stopped.contains(QLatin1String("no solution found")) && kept->stopped.contains(QLatin1String("its point before kept")),
                 qPrintable(kept->stopped));
        QVERIFY2(after < full / 3, qPrintable(QStringLiteral("%1 ms of %2").arg(after).arg(full)));
        // How far, while it solves: fem_solve's "still solving" and
        // fem_describe's "at work" say its point of the sweep, its contact's
        // voltage and step, Newton's step and residual (the Lumerical APD's
        // review: "Solving Stationary" alone for 42 minutes); stopped, its
        // points before kept - fem_describe says where it stopped, fem_solve
        // gives them.
        MultiphysicsDoc* far = open(atOnce(QStringLiteral("at_once_far.qfem"), QStringLiteral("0, 10, 54")));
        QVERIFY(far != nullptr);
        r = call("fem_solve", {{"timeout", 1}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("Still solving after 1 s, ")) && text(r).contains(QLatin1String(" % - ")), qPrintable(text(r)));
        QString atWork;
        for (int k = 0; k < 600 && far->isBusy(); ++k) {
            atWork = json(call("fem_describe")).value("at work").toString();
            if (atWork.contains(QLatin1String("(point 3 of 3): ")) && atWork.contains(QLatin1String("Newton's step"))) break;
            QTest::qWait(100);
        }
        QVERIFY2(atWork.contains(QLatin1String("(point 3 of 3): ")) && atWork.contains(QLatin1String(" of -54 V (step "))
                     && atWork.contains(QLatin1String("Newton's step")) && atWork.contains(QLatin1String("its residual")),
                 qPrintable(atWork));
        r = call("fem_stop", {{"timeout", 600}});
        QVERIFY2(!failed(r) && json(r).value("stopped").toBool(), qPrintable(text(r)));
        QVERIFY2(text(r).contains(QLatin1String("its 2 points before kept")), qPrintable(text(r)));
        QJsonArray studies = json(call("fem_describe")).value("studies").toArray();
        QVERIFY2(!studies.isEmpty() && studies.at(0).toObject().value("stopped").toString().contains(QLatin1String("(point 3 of 3): cancelled")),
                 qPrintable(QJsonDocument(studies).toJson()));
        QVERIFY(json(call("fem_describe")).value("at work").isUndefined());
    }

    // A well's studies through the tools (the check of a4ff84a, 6): each
    // study solved adds its kind's default plots once - one of each name, none
    // for a study that has plot groups of its own (1D ones too), the valence
    // bands their E(k), not a spectrum's; fem_evaluate's 'point' a bands
    // step's k, one past the last refused; fem_plot's display rid of an
    // older group's diagram, the user's kept; a brief batch's save says what.
    void aWellsStudiesThroughTheTools()
    {
        const QString model = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm", "geometry": [{"type": "rectangle", "name": "well", "size": ["20", "10"]}],
            "physics": [{"type": "quantum", "tag": "qw", "valence": "kp4", "features": [
              {"type": "material", "source": "user", "Ec": "0[eV]", "Ev": "-1.5[eV]", "me": "0.067", "mhh": "0.35", "mlh": "0.09",
               "mhhp": "0.11", "mlhp": "0.21", "gamma3": "2.93", "Ep": "25[eV]", "nr": "3.6", "epsilonr": "12.9", "selection": {"all": true}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "tag": "spec", "steps": [{"type": "spectrum", "energies": "linspace(1.60[eV], 1.70[eV], 3)", "states": 2,
                         "linewidth": "5[meV]", "kpoints": 21}]},
                      {"type": "study", "tag": "own", "steps": [{"type": "states", "states": 2}]},
                      {"type": "study", "tag": "states", "steps": [{"type": "states", "states": 2}]},
                      {"type": "study", "tag": "bands", "steps": [{"type": "bands", "kmax": "1[1/nm]", "points": 5, "states": 2}]}],
          "results": {"plots": [{"type": "plotgroup1d", "tag": "mine", "label": "My levels", "study": "own",
                                 "plots": [{"type": "globalgraph", "tag": "gm", "expression": "qw.E_e"}]}]}})json");
        QFile f(path(QStringLiteral("wells_studies.qfem")));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(model.toUtf8());
        f.close();
        MultiphysicsDoc* doc = open(path(QStringLiteral("wells_studies.qfem")));
        QVERIFY(doc != nullptr);
        const auto count = [doc](const char* type, const QString& label) {
            int n = 0;
            for (const qucs_s::fem::Node& g : doc->model().results().children) n += g.type == QLatin1String(type) && g.name() == label;
            return n;
        };
        const auto groups = [doc](const QString& study) {
            QStringList out;
            for (const qucs_s::fem::Node& g : doc->model().results().children)
                if (g.text(QStringLiteral("study")) == study) out << g.name();
            return out;
        };
        const auto solve = [this](const char* study) {
            const QJsonObject r = call("fem_solve", {{"path", "wells_studies.qfem"}, {"study", study}, {"timeout", 600}});
            return failed(r) ? text(r) : QString();
        };
        // The spectrum: its set.
        QString why = solve("spec");
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(groups(QStringLiteral("spec")), (QStringList{"Electron Probability (qw)", "Heavy Hole Probability (qw)", "The Spectrum"}));
        // (Its absorption in 1/cm, as the panel shows it and plots made by
        // hand: it was 1/m, 100 times another's - the update check of 21
        // October, 5.)
        {
            const qucs_s::fem::Node& alpha = doc->model().find(tagOf(doc->model(), QStringLiteral("plotgroup1d"), QStringLiteral("The Spectrum")))->children.front();
            QVERIFY2(alpha.text(QStringLiteral("expression")) == QLatin1String("qw.alphaTE; qw.alphaTM") && alpha.text(QStringLiteral("unit")) == QLatin1String("1/cm"),
                     qPrintable(alpha.text(QStringLiteral("expression")) + QLatin1Char(' ') + alpha.text(QStringLiteral("unit"))));
        }
        // A study with a plot group of the user's (a 1D one): none added.
        why = solve("own");
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(groups(QStringLiteral("own")), QStringList{"My levels"});
        // The states: their band diagram and levels; the probabilities not again.
        why = solve("states");
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(groups(QStringLiteral("states")), (QStringList{"Band Diagram (qw)", "The States"}));
        QCOMPARE(count("plotgroup", QStringLiteral("Electron Probability (qw)")), 1);
        QCOMPARE(count("cutline", QStringLiteral("Growth Axis (qw)")), 1);
        // Its groups deleted, its cut line kept: made again, along that line.
        {
            Model m = doc->model();
            for (const char* label : {"Band Diagram (qw)", "The States"})
                QVERIFY(m.remove(tagOf(m, QStringLiteral("plotgroup1d"), QLatin1String(label))));
            QVERIFY(doc->setModel(m, QStringLiteral("deleted")));
        }
        why = solve("states");
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(groups(QStringLiteral("states")), (QStringList{"Band Diagram (qw)", "The States"}));
        QCOMPARE(count("cutline", QStringLiteral("Growth Axis (qw)")), 1);
        const QString axis = tagOf(doc->model(), QStringLiteral("cutline"), QStringLiteral("Growth Axis (qw)"));
        for (const qucs_s::fem::Node& p : doc->model().find(tagOf(doc->model(), QStringLiteral("plotgroup1d"), QStringLiteral("Band Diagram (qw)")))->children)
            QCOMPARE(p.text(QStringLiteral("data")), axis);
        // The bands: their E(k) in eV, not a spectrum's absorption.
        why = solve("bands");
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(groups(QStringLiteral("bands")), QStringList{"The Valence Bands"});
        const QString bands = tagOf(doc->model(), QStringLiteral("plotgroup1d"), QStringLiteral("The Valence Bands"));
        const qucs_s::fem::Node& ek = doc->model().find(bands)->children.front();
        QVERIFY2(ek.text(QStringLiteral("expression")) == QLatin1String("qw.Ev*") && ek.text(QStringLiteral("unit")) == QLatin1String("eV"),
                 qPrintable(ek.text(QStringLiteral("expression"))));
        QCOMPARE(count("plotgroup1d", QStringLiteral("The Spectrum")), 1);
        QCOMPARE(count("plotgroup", QStringLiteral("Electron Probability (qw)")), 1);

        // 'point': the bands' k by number; one past the last refused.
        QJsonObject r = call("fem_evaluate", {{"path", "wells_studies.qfem"}, {"study", "bands"}, {"expression", "qw.k"}, {"point", 2}});
        QVERIFY2(!failed(r) && json(r).value("solutions").toString() == QLatin1String("k = 0.25 /nm"), qPrintable(text(r)));
        QCOMPARE(json(r).value("values").toArray().at(0).toObject().value("SI").toDouble(), 2.5e8);
        r = call("fem_evaluate", {{"path", "wells_studies.qfem"}, {"study", "bands"}, {"expression", "qw.k"}, {"point", 6}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("its valence bands have 5 values of k")), qPrintable(text(r)));
        r = call("fem_evaluate", {{"path", "wells_studies.qfem"}, {"study", "states"}, {"expression", "qw.E_e"}, {"point", 2}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("the study has one solution, no sweep")), qPrintable(text(r)));

        // Its display: an older group's diagram (a trace its dataset no
        // longer has, another title) gone and said; the user's own kept.
        r = call("fem_plot", {{"path", "wells_studies.qfem"}, {"plot", bands}});
        QVERIFY2(!failed(r) && !json(r).contains("removed from its display"), qPrintable(text(r)));
        const QString display = json(r).value("display").toString();
        QVERIFY(!failed(call("add_diagram", {{"path", display}, {"traces", QJsonArray{"qucsator/qw.E_old1", "qucsator/qw.Ev1"}}, {"title", "An older group"}})));
        QVERIFY(!failed(call("add_diagram", {{"path", display}, {"traces", QJsonArray{"qucsator/qw.Ev1"}}, {"title", "Mine"}})));
        r = call("fem_plot", {{"path", "wells_studies.qfem"}, {"plot", bands}});
        const QJsonArray removed = json(r).value("removed from its display").toArray();
        QVERIFY2(!failed(r) && removed.size() == 1 && removed.at(0).toString().contains(QStringLiteral("“An older group” (its qw.E_old1 not in")),
                 qPrintable(text(r)));
        QStringList titles;
        for (const QJsonValue& d : json(call("get_schematic", {{"path", display}})).value("diagrams").toArray()) titles << d.toObject().value("title").toString();
        QVERIFY2(titles.contains(QLatin1String("Mine")) && titles.contains(QLatin1String("The Valence Bands")) && !titles.contains(QLatin1String("An older group")),
                 qPrintable(titles.join(" | ")));

        // A brief batch's save: which model it saved.
        // (And a node set, by its name; a build by its counts: not "done (3
        // fields)".)
        r = call("batch", {{"brief", true}, {"calls", QJsonArray{QJsonObject{{"tool", "fem_model"}, {"arguments", QJsonObject{{"action", "save"}, {"path", "wells_studies.qfem"}}}},
                                                                 QJsonObject{{"tool", "fem_edit"}, {"arguments", QJsonObject{{"path", "wells_studies.qfem"}, {"action", "set"},
                                                                                                                              {"tag", "mine"}, {"label", "My levels, again"}}}},
                                                                 QJsonObject{{"tool", "fem_build"}, {"arguments", QJsonObject{{"path", "wells_studies.qfem"}}}}}}});
        QVERIFY2(text(r).contains(QLatin1String("[1] fem_model: saved ")) && text(r).contains(QLatin1String("wells_studies.qfem")), qPrintable(text(r)));
        QVERIFY2(text(r).contains(QLatin1String("[2] fem_edit: set My levels, again (mine)")), qPrintable(text(r)));
        QVERIFY2(text(r).contains(QLatin1String("[3] fem_build: boundaries 4, domains 1")), qPrintable(text(r)));
    }

    void claudesToolsShowTheWellsInEV()
    {
        const QString model = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "F", "expression": "0[V/m]"}, {"name": "N", "expression": "0[1/cm^3]"}],
          "components": [{"unit": "nm", "geometry": [{"type": "rectangle", "name": "well", "size": ["20", "10"]}],
            "physics": [{"type": "quantum", "tag": "qw", "F": "F", "valence": "kp4", "features": [
              {"type": "material", "source": "user", "Ec": "0[eV]", "Ev": "-1.5[eV]", "me": "0.067", "mhh": "0.35", "mlh": "0.09",
               "mhhp": "0.11", "mlhp": "0.21", "gamma3": "2.93", "Ep": "25[eV]", "nr": "3.6", "epsilonr": "12.9", "selection": {"all": true}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "tag": "spec", "steps": [{"type": "sweep", "parameters": [["F", "0, 1e6", "V/m"]]},
                        {"type": "spectrum", "energies": "linspace(1.55[eV], 1.75[eV], 5)", "states": 2, "linewidth": "5[meV]",
                         "lineshape": "gaussian", "excitons": true, "kpoints": 21}]},
                      {"type": "study", "tag": "gain", "steps": [{"type": "spectrum", "energies": "linspace(1.60[eV], 1.70[eV], 3)", "states": 2,
                         "linewidth": "5[meV]", "density": "3e18[1/cm^3]", "kpoints": 21}]},
                      {"type": "study", "tag": "bands", "steps": [{"type": "bands", "kmax": "1[1/nm]", "points": 3, "states": 2}]}],
          "results": {"derived": [{"type": "global", "tag": "gev1", "label": "Exciton", "study": "spec",
                                   "expression": "qw.Eb; qw.Ex; qw.gX", "solutions": "all"}],
                      "plots": [{"type": "plotgroup1d", "tag": "pgk", "study": "bands", "plots": [{"type": "globalgraph", "tag": "gk", "expression": "qw.Ev1"}]},
                                {"type": "plotgroup1d", "tag": "pgf", "study": "bands", "plots": [{"type": "globalgraph", "tag": "gf", "expression": "qw.Ev*"}]},
                                {"type": "plotgroup1d", "tag": "pgg", "study": "gain", "plots": [{"type": "globalgraph", "tag": "gg", "expression": "qw.gTE; qw.Fc"}]},
                                {"type": "plotgroup1d", "tag": "pgs", "study": "spec", "plots": [{"type": "globalgraph", "tag": "gs", "expression": "qw.alphaTE; qw.dnTE"}]}]}})json");
        QFile f(path(QStringLiteral("ev_wells.qfem")));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(model.toUtf8());
        f.close();
        QVERIFY(open(path(QStringLiteral("ev_wells.qfem"))) != nullptr);
        QJsonObject r = call("fem_solve", {{"path", "ev_wells.qfem"}, {"study", "spec"}, {"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject solved = json(r);
        // The derived values: three a field, not three at each of five energies.
        const QJsonObject derived = solved.value("derived values").toArray().at(0).toObject();
        const QJsonObject values = derived.value("values").toObject();
        QVERIFY2(values.size() == 6, qPrintable(QJsonDocument(derived).toJson()));
        QVERIFY(derived.value("note").toString().contains(QLatin1String("once for the point")));
        QVERIFY2(values.value("F = 0 V/m: qw.Eb").toString().endsWith(QLatin1String(" eV")), qPrintable(QJsonDocument(values).toJson()));
        QVERIFY(values.value("F = 1 MV/m: qw.Ex").toString().endsWith(QLatin1String(" eV")));
        QVERIFY(!values.value("F = 0 V/m: qw.gX").toString().contains(QLatin1String("eV")));
        // The solve's globals and the Kramers-Kronig warning's window.
        QVERIFY2(solved.value("globals").toObject().value("qw.Ee1hh1").toString().endsWith(QLatin1String(" eV")), qPrintable(text(r)));
        QVERIFY2(solved.value("globals").toObject().value("qw.Ee1").toString().endsWith(QLatin1String(" eV")), qPrintable(text(r)));
        QVERIFY2(text(r).contains(QLatin1String("between 1.55 eV and 1.75 eV")) && !text(r).contains(QLatin1String("1.75 V")), qPrintable(text(r)));
        // (A point past the sweep's last: refused, not its last answered -
        // the check of a4ff84a, 6.1.)
        r = call("fem_evaluate", {{"path", "ev_wells.qfem"}, {"study", "spec"}, {"expression", "qw.Eb"}, {"point", 3}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("its sweep has 2 points")), qPrintable(text(r)));
        // Units: one for those it fits, one for each, none for a number.
        QJsonArray v = json(call("fem_evaluate", {{"path", "ev_wells.qfem"}, {"study", "spec"}, {"expression", "qw.Ex; qw.Eb"}, {"unit", "meV"}}))
                           .value("values").toArray();
        QVERIFY2(v.at(0).toObject().value("value").toString().endsWith(QLatin1String(" meV")) && v.at(1).toObject().value("value").toString().endsWith(QLatin1String(" meV")),
                 qPrintable(QJsonDocument(v).toJson()));
        const double ex = v.at(0).toObject().value("SI").toDouble();
        QVERIFY(std::abs(v.at(0).toObject().value("value").toString().section(QLatin1Char(' '), 0, 0).toDouble() - ex / 1.602176634e-22) < 0.05);
        v = json(call("fem_evaluate", {{"path", "ev_wells.qfem"}, {"study", "spec"}, {"expression", "qw.Ex; qw.Eb"}, {"unit", "eV; meV"}}))
                .value("values").toArray();
        QVERIFY(v.at(0).toObject().value("value").toString().endsWith(QLatin1String(" eV")) && v.at(1).toObject().value("value").toString().endsWith(QLatin1String(" meV")));
        r = call("fem_evaluate", {{"path", "ev_wells.qfem"}, {"study", "spec"}, {"expression", "qw.Eb; qw.gX"}, {"unit", "meV"}});
        v = json(r).value("values").toArray();
        QVERIFY2(v.at(0).toObject().value("value").toString().endsWith(QLatin1String(" meV")) && !v.at(1).toObject().value("value").toString().contains(QLatin1String("meV"))
                     && !json(r).contains("errors"),
                 qPrintable(text(r)));
        r = call("fem_evaluate", {{"path", "ev_wells.qfem"}, {"study", "spec"}, {"expression", "qw.gX; qw.Fw"}, {"unit", "meV"}});
        QVERIFY2(text(r).contains(QLatin1String("meV is a unit of none of them")), qPrintable(text(r)));
        // The spontaneous emission per cm³ and eV, converted from SI's.
        r = call("fem_solve", {{"path", "ev_wells.qfem"}, {"study", "gain"}, {"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        v = json(call("fem_evaluate", {{"path", "ev_wells.qfem"}, {"study", "gain"}, {"expression", "qw.rsp; qw.Rsp"}})).value("values").toArray();
        const QJsonObject rsp = v.at(0).toObject(), Rsp = v.at(1).toObject();
        QVERIFY2(rsp.value("value").toString().endsWith(QLatin1String(" 1/(s*cm^3*eV)")) && Rsp.value("value").toString().endsWith(QLatin1String(" 1/(s*cm^3)")),
                 qPrintable(QJsonDocument(v).toJson()));
        const double shownRsp = rsp.value("value").toString().section(QLatin1Char(' '), 0, 0).toDouble();
        QVERIFY2(std::abs(shownRsp / (rsp.value("SI").toDouble() * 1e-6 * 1.602176634e-19) - 1) < 1e-3, qPrintable(QString::number(shownRsp)));
        QVERIFY(std::abs(Rsp.value("value").toString().section(QLatin1Char(' '), 0, 0).toDouble() / (Rsp.value("SI").toDouble() * 1e-6) - 1) < 1e-3);
        // k in 1/nm: 0.5 at the middle of 0 to 1 /nm.
        r = call("fem_solve", {{"path", "ev_wells.qfem"}, {"study", "bands"}, {"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        v = json(call("fem_evaluate", {{"path", "ev_wells.qfem"}, {"study", "bands"}, {"expression", "qw.k; qw.Ev1"}, {"frequency", "5e8"}})).value("values").toArray();
        QVERIFY2(v.at(0).toObject().value("value").toString() == QLatin1String("0.5 1/nm") && v.at(1).toObject().value("value").toString().endsWith(QLatin1String(" eV")),
                 qPrintable(QJsonDocument(v).toJson()));
        // A plot group's dataset in the units its graphs show - its k axis in
        // 1/nm as qw.k, the energies in eV - and each variable's unit said in
        // it (the review's: eV beside k in 1/m); a display's x axis says its.
        auto dataset = [&](const char* plot, qucs_s::dataset::Dataset& data, QJsonObject& units) {
            const QJsonObject shown = call("fem_plot", {{"path", "ev_wells.qfem"}, {"plot", plot}});
            if (failed(shown)) return text(shown);
            const QString dat = path(QStringLiteral("ev_wells_%1.dat").arg(QLatin1String(plot)));
            QString why;
            if (!data.read(dat, &why)) return why;
            qucs_s::dataimport::Origin origin;
            if (!qucs_s::dataimport::originOf(dat, &origin)) return QStringLiteral("no origin");
            units = origin.units;
            return QString();
        };
        qucs_s::dataset::Dataset bands;
        QJsonObject units;
        QString why = dataset("pgk", bands, units);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        const qucs_s::dataset::Variable* k = bands.find(QStringLiteral("k"));
        QVERIFY2(k && k->size() == 3 && std::abs(k->re.at(1) - 0.5) < 1e-12 && std::abs(k->re.at(2) - 1) < 1e-12,
                 qPrintable(k ? QString::number(k->re.value(2)) : QStringLiteral("no k")));
        QCOMPARE(units.value("k").toString(), QStringLiteral("1/nm"));
        QCOMPARE(units.value("qw.Ev1").toString(), QStringLiteral("eV"));
        const qucs_s::dataset::Variable* ev1 = bands.find(QStringLiteral("qw.Ev1"));
        QVERIFY(ev1 && ev1->size() == 3 && ev1->dependencies == QStringList{QStringLiteral("k")} && std::abs(ev1->re.at(0)) < 2);   // (eV, not J)
        QFile dpl(path(QStringLiteral("ev_wells_pgk.dpl")));
        QVERIFY(dpl.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(dpl.readAll()).contains(QLatin1String("k (1/nm)")), "the display's x axis");
        // qw.Ev*: each state the step found (2, Kramers' pairs both), in eV.
        qucs_s::dataset::Dataset family;
        why = dataset("pgf", family, units);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        for (int j = 1; j <= 4; ++j) {
            const qucs_s::dataset::Variable* ev = family.find(QStringLiteral("qw.Ev%1").arg(j));
            QVERIFY2(ev && ev->size() == 3 && ev->dependencies == QStringList{QStringLiteral("k")}, qPrintable(QString::number(j)));
            QCOMPARE(units.value(QStringLiteral("qw.Ev%1").arg(j)).toString(), QStringLiteral("eV"));
        }
        QVERIFY(!family.find(QStringLiteral("qw.Ev5")) && !family.find(QStringLiteral("qw.Ev*")));
        QCOMPARE(family.find(QStringLiteral("qw.Ev1"))->re, ev1->re);
        // The spectrum's: its photon energies in eV, a field swept in V/m,
        // a gain per m and a quasi-Fermi level in eV.
        qucs_s::dataset::Dataset gain;
        why = dataset("pgg", gain, units);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        const qucs_s::dataset::Variable* E = gain.find(QStringLiteral("photon_energy"));
        QVERIFY2(E && E->size() == 3 && std::abs(E->re.at(0) - 1.6) < 1e-9, E ? qPrintable(QString::number(E->re.value(0))) : "no E");
        QCOMPARE(units.value("photon_energy").toString(), QStringLiteral("eV"));
        QCOMPARE(units.value("qw.gTE").toString(), QStringLiteral("1/m"));
        QCOMPARE(units.value("qw.Fc").toString(), QStringLiteral("eV"));
        qucs_s::dataset::Dataset swept;
        why = dataset("pgs", swept, units);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(units.value("F").toString(), QStringLiteral("V/m"));
        QCOMPARE(units.value("photon_energy").toString(), QStringLiteral("eV"));
        QVERIFY2(units.contains("qw.dnTE") && units.value("qw.dnTE").toString().isEmpty(), qPrintable(units.value("qw.dnTE").toString()));   // (a number: none)
    }

    // Claude imports a Lumerical MQW project (data/lumerical_sample.ldev):
    // fem_model's import writes the model to 'to' and opens it, what was read
    // said; over a file there only with 'replace'; its spectrum solves (under
    // the sanitizers its bands: its k·p spectrum minutes there).
    // The library through Claude and the panel: a material's properties
    // along a variable (fem_describe what=material), a Lumerical project's
    // materials and a library file into My Materials (fem_model materials);
    // a semiconductor feature's parameter not set shown as its material's (its
    // placeholder), a material's properties plotted (a dataset, a display).
    // A crystal in the panel and to Claude: lithium niobate from the
    // library - its orientation (an x-cut wafer's) and tensors on its node;
    // its εr "44; 44; 28" said as along the axes, each item's value; two
    // items, no tensor's, marked. fem_describe: its index along each axis.
    void aCrystalInThePanel()
    {
        app->multiphysicsMenu()->findChild<QAction*>(QStringLiteral("mpNewModelAction"))->trigger();
        auto* doc = qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        const QString mat = panel()->addNode(doc->model().materials().tag, QStringLiteral("material:Lithium niobate"));
        QVERIFY(!mat.isEmpty());
        const qucs_s::fem::Node* node = doc->model().find(mat);
        QCOMPARE(node->text(QStringLiteral("epsilonr")), QStringLiteral("44; 44; 28"));
        QCOMPARE(node->text(QStringLiteral("orientation")), QStringLiteral("cut"));
        QCOMPARE(node->text(QStringLiteral("cut")), QStringLiteral("x"));
        QVERIFY(node->text(QStringLiteral("r")).contains(QLatin1String("r33=30.8[pm/V]")));
        panel()->selectNode(mat);
        auto* eps = panel()->settingsArea()->findChild<QLineEdit*>(QStringLiteral("mpEdit_epsilonr"));
        QVERIFY(eps);
        QVERIFY2(eps->toolTip().contains(QLatin1String("along x, y, z")) && eps->toolTip().contains(QLatin1String("28")), qPrintable(eps->toolTip()));
        eps->setText(QStringLiteral("44; 28"));
        QVERIFY2(eps->toolTip().contains(QLatin1String("a tensor has 3")) && eps->styleSheet().contains(QLatin1String("#c62828")), qPrintable(eps->toolTip()));
        eps->setText(QStringLiteral("44; 44; 28"));
        QVERIFY(eps->styleSheet().isEmpty());
        const QJsonObject r = call("fem_describe", {{"what", "material"}, {"material", "Lithium niobate"}, {"vs", "lambda"}, {"from", "1.5[um]"},
                                                    {"to", "1.6[um]"}, {"points", 3}, {"properties", QJsonArray{"n"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray props = json(r).value("properties").toArray();
        QCOMPARE(props.size(), 3);
        QCOMPARE(props.at(2).toObject().value("axis").toString(), QStringLiteral("x3"));
        QVERIFY(props.at(0).toObject().value("values").toArray().at(1).toDouble() > props.at(2).toObject().value("values").toArray().at(1).toDouble());
    }

    // The names an expression may use offered as it is typed (asked for on
    // 19 October): after "emw." the mode analysis's variables - Ex,
    // normE, Sz..., shown after the stem, with their units and what they
    // are -, narrowed as the name goes on; Return takes the one chosen in
    // place of the word, the box committed when it is finished, not while
    // listed; deleting opens no list. Solved, a result's list has its
    // globals - with their values where the study has one solution (emw.neff
    // = 2.4...), else their units (two modes: emw.loss dB/m) -; a number's
    // dot (1.5)
    // offers nothing; Ctrl+Space every name - the parameters with theirs;
    // Escape closes the list.
    void namesOfferedAsTyped()
    {
        const QString file = copyOf(QStringLiteral("soi_waveguide_modes.qfem"));
        {
            // (One wavelength, one mode, a coarse mesh: the globals, quickly -
            // 385 triangles, neff 2.439; on the example's fine mesh the two
            // solves took 234 s under the sanitizers.)
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString text = QString::fromUtf8(f.readAll());
            f.close();
            text.replace(QStringLiteral("\"1500[nm], 1550[nm], 1600[nm]\", \"modes\": 2"), QStringLiteral("\"1550[nm]\", \"modes\": 1"))
                .replace(QStringLiteral("\"size\": \"fine\""), QStringLiteral("\"size\": \"coarse\""))
                .replace(QStringLiteral("\"hmax\": \"0.03\""), QStringLiteral("\"hmax\": \"0.2\""))
                .replace(QStringLiteral("\"hmax\": \"0.12\""), QStringLiteral("\"hmax\": \"1.0\""));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(text.toUtf8());
        }
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        auto tagOf = [doc](const QString& type, const QString& expression) {
            QString found;
            std::function<void(const qucs_s::fem::Node&)> walk = [&walk, &found, &type, &expression](const qucs_s::fem::Node& n) {
                if (found.isEmpty() && n.type == type && (expression.isEmpty() || n.text(QStringLiteral("expression")) == expression)) found = n.tag;
                for (const qucs_s::fem::Node& c : n.children) walk(c);
            };
            walk(doc->model().root);
            return found;
        };
        const QString surface = tagOf(QStringLiteral("surface"), QStringLiteral("emw.normE"));
        QVERIFY(!surface.isEmpty());
        panel()->selectNode(surface);
        auto* edit = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("expression")));
        QVERIFY(edit != nullptr);
        auto* completer = edit->findChild<QCompleter*>();
        QVERIFY(completer != nullptr);
        QAbstractItemView* list = completer->popup();
        // (Each row's name, unit and what it is - of the completer now.)
        auto rows = [&completer] {
            QList<QStringList> out;
            const QAbstractItemModel* m = completer->completionModel();
            for (int r = 0; r < m->rowCount(); ++r) out << QStringList{m->index(r, 0).data().toString(), m->index(r, 1).data().toString(), m->index(r, 2).data().toString()};
            return out;
        };
        auto names = [&rows] {
            QStringList out;
            for (const QStringList& row : rows()) out << row.at(0);
            return out;
        };
        edit->activateWindow();
        edit->setFocus();
        edit->selectAll();
        QTest::keyClicks(edit, QStringLiteral("2*emw."));
        QTRY_VERIFY(list->isVisible());
        QCOMPARE(list->currentIndex().row(), 0);   // (Return takes the first)
        QStringList shown = names();
        for (const char* name : {"Ex", "Ey", "Ez", "normE", "Sz"}) QVERIFY2(shown.contains(QLatin1String(name)), qPrintable(shown.join(", ")));
        QVERIFY2(!shown.contains(QStringLiteral("x")) && !shown.contains(QStringLiteral("emw.Ex")), qPrintable(shown.join(", ")));
        for (const QStringList& row : rows())
            if (row.at(0) == QLatin1String("normE")) QVERIFY2(row.at(1) == QLatin1String("V/m") && !row.at(2).isEmpty(), qPrintable(row.join(" | ")));
        QVERIFY(list->width() >= edit->width());
        QCOMPARE(doc->model().find(surface)->text(QStringLiteral("expression")), QStringLiteral("emw.normE"));
        QTest::keyClicks(edit, QStringLiteral("no"));
        shown = names();
        QVERIFY2(!shown.isEmpty() && shown.contains(QStringLiteral("normE")), qPrintable(shown.join(", ")));
        for (const QString& n : shown) QVERIFY2(n.startsWith(QLatin1String("no"), Qt::CaseInsensitive), qPrintable(shown.join(", ")));
        list->setCurrentIndex(completer->completionModel()->index(int(shown.indexOf(QStringLiteral("normE"))), 0));
        QTest::keyClick(list, Qt::Key_Return);
        QVERIFY(!list->isVisible());
        QCOMPARE(edit->text(), QStringLiteral("2*emw.normE"));
        QCOMPARE(doc->model().find(surface)->text(QStringLiteral("expression")), QStringLiteral("emw.normE"));
        // A name's dot, not a number's; nothing after the name: no list.
        QTest::keyClicks(edit, QStringLiteral("+1."));
        QVERIFY(!list->isVisible());
        // Deleted back into a name: no list (Return finishes the box).
        QTest::keyClick(edit, Qt::Key_Backspace);
        QTest::keyClick(edit, Qt::Key_Backspace);
        QTest::keyClick(edit, Qt::Key_Backspace);
        QCOMPARE(edit->text(), QStringLiteral("2*emw.normE"));
        QVERIFY(!list->isVisible());
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_COMPARE(doc->model().find(surface)->text(QStringLiteral("expression")), QStringLiteral("2*emw.normE"));

        // Solved: a result's list has the globals and their values.
        const QJsonObject r = call("fem_solve", {}, jobWait());
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString global = tagOf(QStringLiteral("global"), QString());
        QVERIFY(!global.isEmpty());
        panel()->selectNode(global);
        edit = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("expression")));
        QVERIFY(edit != nullptr);
        completer = edit->findChild<QCompleter*>();
        QVERIFY(completer != nullptr);
        list = completer->popup();
        edit->setFocus();
        edit->end(false);
        QTest::keyClicks(edit, QStringLiteral("; emw.n"));
        QTRY_VERIFY(list->isVisible());
        bool neff = false;
        for (const QStringList& row : rows())
            if (row.at(0) == QLatin1String("neff")) neff = row.at(1).startsWith(QLatin1String("= 2.4")) && !row.at(2).isEmpty();
        QStringList said;
        for (const QStringList& row : rows()) said << row.join(QLatin1String(" | "));
        QVERIFY2(neff, qPrintable(said.join("\n")));
        QTest::keyClick(list, Qt::Key_Escape);
        QVERIFY(!list->isVisible());
        QVERIFY(edit->text().endsWith(QLatin1String("; emw.n")));
        edit->undo();
        // Ctrl+Space (the Control key on a Mac): every name - a Global
        // Evaluation's the globals, not a point's names -, the parameters
        // with their values.
        edit->selectAll();
        QTest::keyClick(edit, Qt::Key_Delete);
        // (The box's keys, over a window's shortcut of them - the Python
        // menu's, a VHDL entity's, where enabled.)
#ifdef Q_OS_MACOS
        const Qt::KeyboardModifier control = Qt::MetaModifier;
#else
        const Qt::KeyboardModifier control = Qt::ControlModifier;
#endif
        auto* window = new QAction(app);
        window->setShortcut(QKeySequence(control | Qt::Key_Space));
        window->setShortcutContext(Qt::ApplicationShortcut);   // (offscreen too)
        app->addAction(window);
        int fired = 0;
        connect(window, &QAction::triggered, this, [&fired] { ++fired; });
        QTest::keyClick(edit, Qt::Key_Space, control);
        delete window;
        QCOMPARE(fired, 0);
        QTRY_VERIFY(list->isVisible());
        shown = names();
        QVERIFY2(shown.contains(QStringLiteral("emw.neff")) && !shown.contains(QStringLiteral("emw.normE")) && !shown.contains(QStringLiteral("x")),
                 qPrintable(shown.join(", ")));
        bool parameter = false;
        for (const QStringList& row : rows())
            if (row.at(0) == QLatin1String("w")) parameter = row.at(1) == QLatin1String("= 500 nm") && row.at(2) == QLatin1String("the wire's width");
        QVERIFY2(parameter, qPrintable(shown.join(", ")));
        QTest::keyClick(list, Qt::Key_Escape);
        QVERIFY(edit->text().isEmpty());
        edit->undo();
        // Two modes: a value of one of them is not said, the unit is.
        const QString step = tagOf(QStringLiteral("mode"), QString());
        QVERIFY(!step.isEmpty());
        QJsonObject edited = call("fem_edit", {{"action", "set"}, {"tag", step}, {"props", QJsonObject{{"modes", 2}}}});
        QVERIFY2(!failed(edited), qPrintable(text(edited)));
        edited = call("fem_solve", {}, jobWait());
        QVERIFY2(!failed(edited), qPrintable(text(edited)));
        panel()->selectNode(global);
        edit = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("expression")));
        QVERIFY(edit != nullptr);
        completer = edit->findChild<QCompleter*>();
        list = completer->popup();
        edit->setFocus();
        edit->end(false);
        QTest::keyClicks(edit, QStringLiteral("; emw."));
        QTRY_VERIFY(list->isVisible());
        said.clear();
        for (const QStringList& row : rows()) said << row.join(QLatin1String(" | "));
        for (const QStringList& row : rows()) {
            QVERIFY2(!row.at(1).startsWith(QLatin1Char('=')), qPrintable(said.join("\n")));
            if (row.at(0) == QLatin1String("loss")) QVERIFY2(row.at(1) == QLatin1String("dB/m"), qPrintable(said.join("\n")));
        }
        QTest::keyClick(list, Qt::Key_Escape);
        // A surface's: a point's names, the globals too.
        panel()->selectNode(surface);
        edit = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("expression")));
        QVERIFY(edit != nullptr);
        completer = edit->findChild<QCompleter*>();
        list = completer->popup();
        edit->setFocus();
        edit->selectAll();
        QTest::keyClicks(edit, QStringLiteral("emw."));
        QTRY_VERIFY(list->isVisible());
        shown = names();
        QVERIFY2(shown.contains(QStringLiteral("normE")) && shown.contains(QStringLiteral("neff")), qPrintable(shown.join(", ")));
        QTest::keyClick(list, Qt::Key_Escape);
        doc->setDocChanged(false);
    }

    void claudeAndTheLibrary()
    {
        QJsonObject r = call("fem_describe", {{"what", "material"}, {"material", "Gallium arsenide"}, {"vs", "N"}, {"from", "1e15[1/cm^3]"}, {"to", "1e19[1/cm^3]"},
                                              {"points", 5}, {"properties", QJsonArray{"mun"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("variable").toString(), QStringLiteral("N"));
        const QJsonArray at = o.value("at").toArray(), mu = o.value("properties").toArray().at(0).toObject().value("values").toArray();
        QCOMPARE(at.size(), 5);
        for (int i = 0; i < 5; ++i) {
            const double N = at.at(i).toDouble(), expected = 40 + (8500 - 40) / (1 + N / 8.1e16);
            QVERIFY2(std::abs(mu.at(i).toDouble() / expected - 1) < 1e-5, qPrintable(text(r)));
        }
        r = call("fem_describe", {{"what", "material"}, {"material", "Silicon"}, {"vs", "x"}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("no alloy")), qPrintable(text(r)));
        // (The format's list: a long text, silicon's index, said by its length.)
        r = call("fem_describe", {{"what", "format"}});
        QJsonObject silicon;
        for (const QJsonValue m : json(r).value("materials").toArray())
            if (m.toObject().value("name").toString() == QLatin1String("Silicon")) silicon = m.toObject().value("properties").toObject();
        QVERIFY2(silicon.value("n").toString().contains(QLatin1String("fem_describe what=material")) && silicon.value("Eg").toString().startsWith(QLatin1String("1.12[eV] - ")),
                 qPrintable(QJsonDocument(silicon).toJson()));
        r = call("fem_describe", {{"what", "material"}, {"material", "Unobtainium"}});
        QVERIFY2(failed(r), qPrintable(text(r)));
        // A Lumerical project's materials into My Materials, then looked at.
        r = call("fem_model", {{"action", "materials"}, {"path", QStringLiteral(QUCS_TEST_DATA "/lumerical_device.ldev")}});
        QVERIFY2(!failed(r) && json(r).value("materials").toArray().contains(QStringLiteral("Si (Silicon)")), qPrintable(text(r)));
        QVERIFY(findMaterial(QStringLiteral("Si (Silicon)"), MultiphysicsPanel::userMaterials()));
        r = call("fem_describe", {{"what", "material"}, {"material", "Si (Silicon)"}, {"vs", "T"}, {"from", "300[K]"}, {"to", "400[K]"}, {"points", 2}, {"properties", QJsonArray{"Eg"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray eg = json(r).value("properties").toArray().at(0).toObject().value("values").toArray();
        QVERIFY2(std::abs(eg.at(0).toDouble() - (1.169 - 4.73e-4 * 9e4 / 936)) < 1e-5 && std::abs(eg.at(1).toDouble() - (1.169 - 4.73e-4 * 16e4 / 1036)) < 1e-5, qPrintable(text(r)));
        // A library file's.
        const QString lib = path(QStringLiteral("mine.json"));
        QVERIFY(writeMaterials(lib, {MaterialEntry{QStringLiteral("Mine"), QStringLiteral("Mine"), QString(), QJsonObject{{"epsilonr", "3.3"}}, true}}));
        r = call("fem_model", {{"action", "materials"}, {"path", lib}});
        QVERIFY2(!failed(r) && findMaterial(QStringLiteral("Mine"), MultiphysicsPanel::userMaterials()), qPrintable(text(r)));
        r = call("fem_model", {{"action", "materials"}, {"path", QStringLiteral(QUCS_TEST_DATA "/klayout_sample.oas")}});
        QVERIFY2(failed(r), qPrintable(text(r)));
        // The panel: a GaAs bar's material model - its Caughey and Thomas
        // µmin not set: its field empty, GaAs's shown in it.
        const QString file = path(QStringLiteral("gaas_bar.qfem"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "bar", "size": ["10", "1"]}],
                "materials": [{"material": "Gallium arsenide", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "model", "tag": "gaasmodel", "mobility": "doping", "nrefn": "1e17[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "0.01[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json");
        }
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc);
        QVERIFY(doc->geometry().topology);
        panel()->selectNode(QStringLiteral("gaasmodel"));
        auto* munmin = panel()->settingsArea()->findChild<QLineEdit*>(QStringLiteral("mpEdit_munmin"));
        auto* nrefn = panel()->settingsArea()->findChild<QLineEdit*>(QStringLiteral("mpEdit_nrefn"));
        QVERIFY(munmin && nrefn);
        QVERIFY2(munmin->text().isEmpty() && munmin->placeholderText().contains(QLatin1String("40[cm^2/(V*s)]")) && munmin->placeholderText().contains(QLatin1String("Gallium arsenide's")),
                 qPrintable(munmin->text() + QLatin1Char('|') + munmin->placeholderText()));
        QCOMPARE(nrefn->text(), QStringLiteral("1e17[1/cm^3]"));   // (set on the feature: its own)
        // Its material's properties plotted: a dataset and a display.
        const qucs_s::fem::Node* mat = nullptr;
        for (const qucs_s::fem::Node& m : doc->model().materials().children) mat = &m;
        QVERIFY(mat);
        MaterialSweep sweep;
        sweep.variable = QStringLiteral("T");
        sweep.points = 5;
        QString why;
        const QString dpl = doc->plotMaterial(*mat, {QStringLiteral("Eg"), QStringLiteral("mun")}, sweep, &why);
        QVERIFY2(!dpl.isEmpty() && QFileInfo::exists(QString(dpl).replace(QLatin1String(".dpl"), QLatin1String(".dat"))), qPrintable(why));
        QFile dat(QString(dpl).replace(QLatin1String(".dpl"), QLatin1String(".dat")));
        QVERIFY(dat.open(QIODevice::ReadOnly));
        const QByteArray content = dat.readAll();
        QVERIFY2(content.contains("<indep T 5>") && content.contains("<dep Eg T>") && content.contains("<dep mun T>"), content.left(300).constData());
    }

    void claudeImportsLumerical()
    {
        // The mark at its first use, as its owner's guidelines ask: the
        // menu's entry, its tip and its help, and the three exports' kinds.
        const QAction* menu = app->findChild<QAction*>(QStringLiteral("importLumerical"));
        QVERIFY(menu != nullptr);
        for (const QString& said : {menu->text(), menu->statusTip(), menu->whatsThis()})
            QVERIFY2(said.contains(QStringLiteral("Lumerical™")), qPrintable(said));
        for (const char* kind : {"lumericalmode", "lumericalcharge", "lumericalmqw"})
            QVERIFY2(nodeKind(QLatin1String(kind)) && nodeKind(QLatin1String(kind))->title.startsWith(QStringLiteral("Lumerical™ ")), kind);
        const QString to = path(QStringLiteral("lumerical_imported.qfem"));
        QFile::remove(to);
        QJsonObject r = call("fem_model", {{"action", "import"}, {"path", QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldev")}, {"to", to}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject said = json(r);
        QVERIFY2(said.value("read").toArray().at(0).toString().contains(QLatin1String("3 layers")) && QFileInfo::exists(to), qPrintable(text(r)));
        QCOMPARE(said.value("geometry").toString(), QStringLiteral("built"));
        r = call("fem_model", {{"action", "import"}, {"path", QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldev")}, {"to", to}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("is there already")), qPrintable(text(r)));
        r = call("fem_model", {{"action", "import"}, {"path", QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldev")}, {"to", to}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        {
            // A conversation's import over the model open in its tab: its own
            // write, not told back as the file's; a write from outside told
            // once, then not again (the MQW benchmarks' round 2, its nit 5).
            const quint64 us = 303;
            const auto as = [this](quint64 who, const QString& tool, const QJsonObject& args = {}) { return control->callNow(tool, args, 120000, who); };
            QVERIFY(!failed(as(us, "fem_describe", {{"path", to}})));
            QVERIFY(!failed(as(us, "fem_model", {{"action", "import"}, {"path", QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldev")}, {"to", to}, {"replace", true}})));
            for (int i = 0; i < 2; ++i) {
                const QJsonObject again = as(us, "get_state");
                QVERIFY2(!text(again).contains("Since your last call"), qPrintable(text(again).right(300)));
            }
            MultiphysicsDoc* tab = nullptr;
            for (QucsDoc* d : app->allDocuments())
                if (auto* m = dynamic_cast<MultiphysicsDoc*>(d); m && QFileInfo(m->getDocName()) == QFileInfo(to)) tab = m;
            QVERIFY(tab != nullptr);
            const auto revision = tab->revision();
            QTest::qWait(1100);   // (a modification time of its own)
            QFile file(to);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QByteArray bytes = file.readAll();
            file.close();
            bytes.replace("\"Valence bands\"", "\"Valence bands, edited\"");
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(bytes);
            file.close();
            QTRY_VERIFY_WITH_TIMEOUT(tab->revision() > revision, 10000);
            QJsonObject told = as(us, "get_state");
            QVERIFY2(text(told).contains("Since your last call") && text(told).contains("changed on disk"), qPrintable(text(told).right(400)));
            told = as(us, "get_state");
            QVERIFY2(!text(told).contains("Since your last call"), qPrintable(text(told).right(400)));
        }
        r = call("fem_solve", {{"path", to}, {"study", slowBuild() ? "bands" : "gain"}, {"timeout", 300}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("fem_model", {{"action", "import"}, {"path", QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldf")}, {"to", path(QStringLiteral("none.qfem"))}});
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("a Lumerical data file")), qPrintable(text(r)));
        // CHARGE projects (DEVICE 7.1's, 7.17's), a MODE project, a HEAT
        // project: each a model, built.
        const std::pair<const char*, const char*> projects[] = {{"lumerical_charge.ldev", "CHARGE cross-section"}, {"lumerical_device.ldev", "CHARGE cross-section"},
                                                                {"lumerical_mode.lms", "FDE region"}, {"lumerical_heat.ldev", "HEAT cross-section"}};
        for (const auto& [project, kind] : projects) {
            const QString model = path(QFileInfo(QString::fromLatin1(project)).completeBaseName() + QStringLiteral(".qfem"));
            r = call("fem_model", {{"action", "import"}, {"path", QStringLiteral(QUCS_TEST_DATA "/") + QString::fromLatin1(project)}, {"to", model}, {"replace", true}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QCOMPARE(json(r).value("geometry").toString(), QStringLiteral("built"));
            QVERIFY2(json(r).value("read").toArray().at(0).toString().contains(QLatin1String(kind)), qPrintable(text(r)));
        }
        // To a folder not there: made. To one it cannot write in: said, and no
        // tab left in the file's name, unsaved (a later import of it met it).
        const QString deep = path(QStringLiteral("imports/new/heat.qfem"));
        r = call("fem_model", {{"action", "import"}, {"path", QStringLiteral(QUCS_TEST_DATA "/lumerical_heat.ldev")}, {"to", deep}});
        QVERIFY2(!failed(r) && QFileInfo::exists(deep), qPrintable(text(r)));
        const QString locked = path(QStringLiteral("locked"));
        QVERIFY(QDir().mkpath(locked));
        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const int tabs = app->DocumentTab->count();
        r = call("fem_model", {{"action", "import"}, {"path", QStringLiteral(QUCS_TEST_DATA "/lumerical_heat.ldev")}, {"to", locked + QStringLiteral("/heat.qfem")}});
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QVERIFY2(failed(r) && text(r).contains(QLatin1String("cannot be written")), qPrintable(text(r)));
        QCOMPARE(app->DocumentTab->count(), tabs);
        for (int i = 0; i < app->DocumentTab->count(); ++i)
            if (auto* d = qobject_cast<MultiphysicsDoc*>(app->DocumentTab->widget(i))) QVERIFY(!d->getDocName().endsWith(QLatin1String("locked/heat.qfem")));
    }

    // Claude exports a model as a Lumerical script - fem_edit adds the node
    // under "exports", fem_export writes it (no study need be computed),
    // the panel shows it -, and imports the script again.
    void claudeExportsLumerical()
    {
        const QString file = copyOf(QStringLiteral("pn_diode.qfem"));
        MultiphysicsDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        QJsonObject r = call("fem_edit", {{"action", "add"}, {"parent", "exports"}, {"type", "lumericalcharge"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString tag = json(r).value("added").toObject().value("tag").toString();
        QVERIFY(tag.startsWith(QLatin1String("lc")));
        r = call("fem_export", {{"export", tag}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString written = json(r).value("written").toString();
        QCOMPARE(QFileInfo(written).fileName(), QStringLiteral("pn_diode_%1.lsf").arg(tag));
        QVERIFY2(json(r).value("begins").toString().startsWith(QStringLiteral("# pn_diode_%1.lsf - a Lumerical™ CHARGE script").arg(tag)), qPrintable(text(r)));
        QFile f(written);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(f.readAll()).contains(QStringLiteral("save(\"pn_diode_%1.ldev\");").arg(tag)));
        panel()->selectNode(tag);
        QVERIFY(panel()->findChild<QWidget*>(QStringLiteral("mpExportButton")) != nullptr);
        r = call("fem_model", {{"action", "import"}, {"path", written}, {"to", path(QStringLiteral("diode_back.qfem"))}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("read").toArray().at(0).toString().contains(QLatin1String("2 structures, 2 doping objects, 2 contacts")), qPrintable(text(r)));
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
        QVERIFY(doc->waitForJob(jobWait()));
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
        QVERIFY(drum->waitForJob(jobWait()));
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
