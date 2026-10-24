/*
 * test_rf25_doc.cpp - an RF 2.5D model in the window: its tab (rf25doc.h),
 *                     the RF 2.5D panel (rf25panel.h) - its tree, its
 *                     node menus and the settings of the node chosen -,
 *                     the dock's pages reached by name, the file opened,
 *                     saved and read again, and the layout drawn in the
 *                     stack's colours.
 *
 * Phase A of docs/proposals/2026-10-23-rf-2p5d-moment-method.md: there is
 * no solver, and nothing here asks for one.
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
#include <QComboBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
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
#include "qucs.h"
#include "layoutdoc.h"
#include "rf25doc.h"
#include "rf25panel.h"

namespace rf = qucs_s::rf25;

namespace {

QString examples()
{
    return QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
}

/// The tree's item of \a tag, or null.
QTreeWidgetItem* itemOf(QTreeWidget* tree, const QString& tag)
{
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole + 1).toString() == tag) return *it;
        ++it;
    }
    return nullptr;
}

bool says(const QStringList& said, const char* words)
{
    for (const QString& s : said)
        if (s.contains(QLatin1String(words), Qt::CaseInsensitive)) return true;
    return false;
}

/// A row of a table, built by appending: a braced list of one item is a
/// copy of that item to CI's older AppleClang, not a list of it.
QJsonArray row(const QStringList& cells)
{
    QJsonArray out;
    for (const QString& c : cells) out.append(c);
    return out;
}

QJsonArray rowsOf(const QList<QStringList>& rows)
{
    QJsonArray out;
    for (const QStringList& r : rows) out.append(row(r));
    return out;
}

} // namespace

class TestRf25Doc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    RF25Panel* panel() const { return app->rf25Panel(); }
    RF25Doc* front() const { return qobject_cast<RF25Doc*>(app->DocumentTab->currentWidget()); }
    RF25Doc* open(const QString& file)
    {
        if (!app->gotoPage(file)) return nullptr;
        return front();
    }
    /// A model with a stack of its own in the workspace: a substrate, an
    /// oxide and two metals with a via, a port on M1, and the layer map
    /// sending 31/0 to M1.
    QString written(const QString& name)
    {
        rf::Model m = rf::Model::blank();
        rf::Node ild2 = m.make(QStringLiteral("rf_dielectric"));
        if (m.add(QStringLiteral("stack"), ild2) == nullptr) return {};
        rf::Node metal2 = m.make(QStringLiteral("rf_metal"));
        metal2.props.insert(QStringLiteral("thickness"), QStringLiteral("0.9[um]"));
        rf::Node via = m.make(QStringLiteral("rf_via"));
        via.props.insert(QStringLiteral("from"), QStringLiteral("M1"));
        via.props.insert(QStringLiteral("to"), QStringLiteral("M2"));
        if (m.add(QStringLiteral("stack"), via) == nullptr) return {};
        if (m.add(QStringLiteral("stack"), metal2) == nullptr) return {};
        rf::Node port = m.make(QStringLiteral("rf_edge_port"));
        port.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
        if (m.add(QStringLiteral("ports"), port) == nullptr) return {};
        m.layout().props.insert(QStringLiteral("file"), QStringLiteral("heater.gds"));
        m.layout().child(QStringLiteral("rf_layer_map"))
            ->props.insert(QStringLiteral("rows"), rowsOf({{QStringLiteral("31"), QStringLiteral("0"), QStringLiteral("M1")}}));
        const QString file = path(name);
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(m.toJson());
        f.close();
        return file;
    }
    /// The map's rows set on \a doc, a step of Undo: the shapes of those
    /// layers go to those layers of the stack.
    void mapTo(RF25Doc* doc, const QList<QStringList>& rows)
    {
        const rf::Node* map = doc->model().layout().child(QStringLiteral("rf_layer_map"));
        if (map == nullptr) return;
        doc->editNode(map->tag,
                      [&rows](rf::Node& n) { n.props.insert(QStringLiteral("rows"), rowsOf(rows)); },
                      QStringLiteral("map"));
    }
    /// What the panel says the node chosen did.
    QString reportText() const
    {
        QLabel* label = panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("rfNodeReport"));
        return label != nullptr ? label->text() : QString();
    }
    bool reportSays(const QString& words) const
    {
        return !words.isEmpty() && reportText().contains(words);
    }
    /// The layout of the multiphysics examples, in the workspace.
    QString layoutCopy()
    {
        const QString to = path(QStringLiteral("heater.gds"));
        QFile::remove(to);
        QFile::copy(examples() + QStringLiteral("thermo_optic_heater.gds"), to);
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
            if (auto* rf25 = dynamic_cast<RF25Doc*>(d)) {
                rf25->stopSolving();
                rf25->waitForLayout(30000);
                rf25->waitForPrepared(30000);
            }
            d->setDocChanged(false);
        }
        app->closeAllFiles();
    }

    // A new model: a tab of its own, its tree in the panel - the parts of
    // the tree and the stack a blank model comes with - and the panel's
    // page the tree, not the page that offers to make one.
    void aNewModelOpensInItsOwnTabWithItsTree()
    {
        QCOMPARE(panel()->pages()->currentIndex(), 0);   // (nothing open: New Model, Open Model)
        QVERIFY(panel()->newButton() != nullptr);
        RF25Doc* doc = app->newRf25Model();
        QVERIFY(doc != nullptr);
        QCOMPARE(front(), doc);
        QCOMPARE(panel()->document(), doc);
        QCOMPARE(panel()->pages()->currentIndex(), 1);
        QCOMPARE(app->currentSidePageName(), QStringLiteral("rf 2.5d"));

        QTreeWidget* tree = panel()->tree();
        QCOMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("untitled.qrf25"));
        QStringList top;
        for (int i = 0; i < tree->topLevelItem(0)->childCount(); ++i) top << tree->topLevelItem(0)->child(i)->text(0);
        QCOMPARE(top, QStringList({QStringLiteral("Parameters"), QStringLiteral("Stack"), QStringLiteral("Layout"),
                                   QStringLiteral("Ports"), QStringLiteral("Mesh"), QStringLiteral("Study"),
                                   QStringLiteral("Results"), QStringLiteral("Exports")}));
        // The stack a blank model comes with, and each layer's thickness in
        // the tree's second column.
        QVERIFY(itemOf(tree, QStringLiteral("sub1")) != nullptr);
        QVERIFY(itemOf(tree, QStringLiteral("d1")) != nullptr);
        QTreeWidgetItem* m1 = itemOf(tree, QStringLiteral("m1"));
        QVERIFY(m1 != nullptr);
        QCOMPARE(m1->text(0), QStringLiteral("M1"));
        QVERIFY2(m1->text(1).contains(QStringLiteral("600")) || m1->text(1).contains(QStringLiteral("0.6")),
                 qPrintable(m1->text(1)));
        // What is missing is said, in the tab and on the model's node.
        QVERIFY2(says(doc->problems(), "names no file"), qPrintable(doc->problems().join(" | ")));
        QVERIFY2(says(doc->problems(), "no port"), qPrintable(doc->problems().join(" | ")));
        QVERIFY(doc->problemsText()->toPlainText().contains(QStringLiteral("names no file")));
        panel()->selectNode(doc->model().root.tag);
        QVERIFY(panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("rfNodeProblems")) != nullptr);
        QCOMPARE(itemOf(tree, doc->model().root.tag)->text(1), QStringLiteral("2 missing"));
    }

    // A property typed into the panel is one step of Undo, and the model
    // the tree shows follows it back and forth. The editors read this
    // model's own kinds: a property not set reads as its default, which
    // Node::value() (the multiphysics table) answers nothing for.
    void aPropertyIsEditedAndUndone()
    {
        RF25Doc* doc = app->newRf25Model();
        panel()->selectNode(QStringLiteral("m1"));
        QCOMPARE(panel()->currentTag(), QStringLiteral("m1"));
        auto* thickness = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("thickness")));
        QVERIFY(thickness != nullptr);
        QCOMPARE(thickness->text(), QStringLiteral("0.6[um]"));   // its kind's default, shown
        QVERIFY2(thickness->toolTip().contains(QStringLiteral("600 nm")), qPrintable(thickness->toolTip()));

        thickness->setText(QStringLiteral("2[um]"));
        emit thickness->editingFinished();
        QCOMPARE(rf::textOf(*doc->model().find(QStringLiteral("m1")), QStringLiteral("thickness")), QStringLiteral("2[um]"));
        QCOMPARE(doc->undoStack()->count(), 1);
        QVERIFY(doc->getDocChanged());

        // A conductivity that is no number is said so, and still typed.
        auto* sigma = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("sigma")));
        QVERIFY(sigma != nullptr);
        sigma->setText(QStringLiteral("3.3e7 *"));
        emit sigma->textEdited(sigma->text());
        QVERIFY2(!sigma->toolTip().startsWith(QStringLiteral("=")), qPrintable(sigma->toolTip()));

        doc->undo();
        QCOMPARE(rf::textOf(*doc->model().find(QStringLiteral("m1")), QStringLiteral("thickness")), QStringLiteral("0.6[um]"));
        doc->redo();
        QCOMPARE(rf::textOf(*doc->model().find(QStringLiteral("m1")), QStringLiteral("thickness")), QStringLiteral("2[um]"));

        // A whole number and a choice, each its own step.
        panel()->selectNode(doc->model().mesh().tag);
        auto* perWavelength = qobject_cast<QSpinBox*>(panel()->editorFor(QStringLiteral("per_wavelength")));
        QVERIFY(perWavelength != nullptr);
        QCOMPARE(perWavelength->value(), 20);
        perWavelength->setValue(30);
        emit perWavelength->editingFinished();
        QCOMPARE(rf::integerOf(doc->model().mesh(), QStringLiteral("per_wavelength")), 30);
        doc->undo();
        QCOMPARE(rf::integerOf(doc->model().mesh(), QStringLiteral("per_wavelength")), 20);
    }

    // What a step reads is shown only when the step is on (showIf, the one
    // rule both model trees ask: fem::propertyVisible).
    void whatDependsOnAFlagIsShownWithIt()
    {
        RF25Doc* doc = app->newRf25Model();
        const rf::Node* processing = doc->model().layout().child(QStringLiteral("rf_processing"));
        QVERIFY(processing != nullptr);
        panel()->selectNode(processing->tag);
        QVERIFY(panel()->editorFor(QStringLiteral("snap")) != nullptr);
        QVERIFY2(panel()->editorFor(QStringLiteral("grid")) == nullptr, "the grid is shown while Snap is off");
        auto* snap = qobject_cast<QCheckBox*>(panel()->editorFor(QStringLiteral("snap")));
        QVERIFY(snap != nullptr);
        snap->setChecked(true);
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("grid")) != nullptr);
        // The ground's surface impedance likewise: its kind alone.
        const QString ground = panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_ground"));
        QVERIFY(!ground.isEmpty());
        panel()->selectNode(ground);
        QVERIFY(panel()->editorFor(QStringLiteral("impedance")) == nullptr);
        auto* kind = qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("kind")));
        QVERIFY(kind != nullptr);
        kind->setCurrentIndex(kind->findData(QStringLiteral("impedance")));
        emit kind->activated(kind->currentIndex());
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("impedance")) != nullptr);
    }

    // The tree is edited through the panel - a layer added where it
    // belongs in z, named so that the map and the ports can name it,
    // moved, disabled, deleted - each change one step of Undo. A part of
    // the tree itself is not deleted.
    void theTreeIsEditedInStepsOfUndo()
    {
        RF25Doc* doc = app->newRf25Model();
        const QString oxide = panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_dielectric"));
        const QString metal = panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_metal"));
        QVERIFY(!oxide.isEmpty() && !metal.isEmpty());
        QCOMPARE(doc->model().find(metal)->label, QStringLiteral("M2"));
        QCOMPARE(doc->model().find(oxide)->label, QStringLiteral("ILD2"));
        QCOMPARE(panel()->currentTag(), metal);
        QCOMPARE(doc->undoStack()->count(), 2);

        // A ground belongs under the stack and a cover over it, whatever
        // order they are added in.
        const QString ground = panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_ground"));
        const QString cover = panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_cover"));
        const QString last = panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_metal"));
        QStringList order;
        for (const rf::Node& c : doc->model().stackNode().children) order << c.tag;
        QCOMPARE(order.indexOf(last) < order.indexOf(ground), true);
        QCOMPARE(order.last(), cover);

        // Moved, disabled, deleted - and a part of the tree itself kept.
        QVERIFY(panel()->moveNode(metal, -1));
        QVERIFY(panel()->setNodeEnabled(metal, false));
        QVERIFY(!doc->model().find(metal)->enabled);
        QVERIFY(panel()->duplicateNode(metal));
        QVERIFY(panel()->removeNode(metal));
        QVERIFY(doc->model().find(metal) == nullptr);
        {
            misc::ErrorCapture quiet;   // (the box that says why)
            QVERIFY(!panel()->removeNode(doc->model().mesh().tag));
        }
        QVERIFY(doc->model().find(doc->model().mesh().tag) != nullptr);
        // Each of them back, in turn.
        const int steps = doc->undoStack()->count();
        for (int i = 0; i < steps; ++i) doc->undo();
        QStringList back;
        for (const rf::Node& c : doc->model().stackNode().children) back << c.tag;
        QCOMPARE(back, QStringList({QStringLiteral("sub1"), QStringLiteral("d1"), QStringLiteral("m1")}));
        QVERIFY(!doc->getDocChanged());   // (back where it was saved: clean)
    }

    // A layer is named by the layer map, by the vias that join it and by
    // the ports on it: renamed in the tree, it is renamed in all three, or
    // they would name a layer that is gone. Two layers of one name are
    // refused for the same reason.
    void aLayerRenamedIsRenamedWhereItIsNamed()
    {
        RF25Doc* doc = open(written(QStringLiteral("stack.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY2(doc->model().problems().isEmpty(), qPrintable(doc->model().problems().join(" | ")));
        QVERIFY(panel()->renameNode(QStringLiteral("m1"), QStringLiteral("MET1")));
        const rf::Model& m = doc->model();
        QCOMPARE(m.find(QStringLiteral("m1"))->label, QStringLiteral("MET1"));
        QCOMPARE(rf::textOf(*m.find(QStringLiteral("v1")), QStringLiteral("from")), QStringLiteral("MET1"));
        QCOMPARE(rf::textOf(*m.find(QStringLiteral("p1")), QStringLiteral("conductor")), QStringLiteral("MET1"));
        QCOMPARE(m.layout().child(QStringLiteral("rf_layer_map"))->props.value(QStringLiteral("rows")).toArray()
                     .first().toArray().at(2).toString(),
                 QStringLiteral("MET1"));
        // Nothing is missing after it: every name still names a layer.
        QVERIFY2(m.problems().isEmpty(), qPrintable(m.problems().join(" | ")));
        // The name of another layer is refused.
        {
            misc::ErrorCapture quiet;
            QVERIFY(!panel()->renameNode(QStringLiteral("m2"), QStringLiteral("MET1")));
        }
        QCOMPARE(doc->model().find(QStringLiteral("m2"))->label, QStringLiteral("M2"));
        doc->undo();
        QCOMPARE(doc->model().find(QStringLiteral("m1"))->label, QStringLiteral("M1"));
        QCOMPARE(rf::textOf(*doc->model().find(QStringLiteral("v1")), QStringLiteral("from")), QStringLiteral("M1"));
    }

    // Saved, and read back the same - by the reader and by a tab opening
    // it; saved under another name, the tab keeps that name and the file
    // reads the same.
    void itIsSavedAndReadBackTheSame()
    {
        RF25Doc* doc = app->newRf25Model();
        panel()->addNode(QStringLiteral("ports"), QStringLiteral("rf_edge_port"));
        panel()->selectNode(QStringLiteral("m1"));
        auto* thickness = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("thickness")));
        thickness->setText(QStringLiteral("1.2[um]"));
        emit thickness->editingFinished();
        const QJsonObject before = doc->model().toObject();

        const QString file = path(QStringLiteral("saved.qrf25"));
        doc->setName(file);
        QCOMPARE(doc->save(), 0);
        QVERIFY(!doc->getDocChanged());
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString why;
        const std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
        QVERIFY2(read.has_value(), qPrintable(why));
        QCOMPARE(read->toObject(), before);
        QCOMPARE(QJsonDocument(read->toObject()).object().value(QStringLiteral("format")).toString(), rf::formatName());

        // Saved as another file (what Save As does once the name is
        // chosen): that name, and the same content.
        const QString other = path(QStringLiteral("other.qrf25"));
        doc->setName(other);
        QCOMPARE(doc->save(), 0);
        QCOMPARE(panel()->tree()->topLevelItem(0)->text(0), QStringLiteral("other.qrf25"));
        QFile g(other);
        QVERIFY(g.open(QIODevice::ReadOnly));
        const std::optional<rf::Model> again = rf::Model::fromJson(g.readAll(), &why);
        QVERIFY2(again.has_value(), qPrintable(why));
        QCOMPARE(again->toObject(), before);

        // Opened in a tab of its own, the suffix alone saying which kind of
        // document it is.
        QVERIFY(QucsApp::isRf25File(other));
        QVERIFY(!QucsApp::isRf25File(path(QStringLiteral("x.qfem"))));
        doc->setDocChanged(false);
        app->closeAllFiles();
        RF25Doc* opened = open(other);
        QVERIFY(opened != nullptr);
        QVERIFY(QucsApp::isRf25Document(opened));
        QCOMPARE(opened->model().toObject(), before);
        QCOMPARE(opened->undoStack()->count(), 0);
        QVERIFY(!opened->getDocChanged());
    }

    // A file not written by this build: what it does not read is said, and
    // nothing is opened. A model of the right format with parts missing
    // gets them back (a file of an older build).
    void aFileThatIsNoModelIsRefused()
    {
        const QString bad = path(QStringLiteral("bad.qrf25"));
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"format\": \"qucs-s multiphysics 1\"}\n");
        f.close();
        const int tabs = app->DocumentTab->count();
        {
            misc::ErrorCapture quiet;
            QVERIFY(!app->gotoPage(bad));
        }
        QCOMPARE(app->DocumentTab->count(), tabs);

        // Only the root and the stack: the fixed parts are made again.
        const QString thin = path(QStringLiteral("thin.qrf25"));
        QFile g(thin);
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write(QJsonDocument(QJsonObject{{QStringLiteral("format"), rf::formatName()},
                                          {QStringLiteral("tree"), QJsonObject{{QStringLiteral("type"), QStringLiteral("rf_model")}}}})
                    .toJson());
        g.close();
        RF25Doc* doc = open(thin);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->model().root.child(QStringLiteral("rf_stack")) != nullptr);
        QVERIFY(doc->model().layout().child(QStringLiteral("rf_layer_map")) != nullptr);
        QCOMPARE(itemOf(panel()->tree(), doc->model().stackNode().tag)->text(1), QStringLiteral("0 layer(s)"));
    }

    // Changed on disk by something outside Qucs-S, it is read again where
    // the tab left it; saved here, it is not - the save is stamped with
    // the file it wrote, so the watcher sees no change (the rule of 24
    // October: a date and a size, qucsdoc.h).
    void aFileChangedOnDiskIsReadAgainAndOneSavedHereIsNot()
    {
        const QString file = written(QStringLiteral("watched.qrf25"));
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr);
        panel()->selectNode(QStringLiteral("m2"));
        QVERIFY(panel()->setNodeEnabled(QStringLiteral("m2"), false));
        QVERIFY(doc->undoStack()->count() > 0);
        QCOMPARE(doc->save(), 0);
        QVERIFY(!doc->changedOnDisk(QFileInfo(file).lastModified()));
        QTest::qWait(2500);   // (the watcher's 400 ms, its sync's 1.5 s)
        QVERIFY2(doc->undoStack()->count() > 0, "read again after its own save");

        // Another program writes it: read again, the steps of Undo gone.
        rf::Model other = rf::Model::blank();
        other.root.label = QStringLiteral("From elsewhere");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(other.toJson());
        f.close();
        QVERIFY(doc->changedOnDisk(QFileInfo(file).lastModified()));
        QVERIFY(doc->reload());
        QCOMPARE(doc->model().root.label, QStringLiteral("From elsewhere"));
        QCOMPARE(doc->undoStack()->count(), 0);
        QVERIFY(!doc->getDocChanged());
        QVERIFY(doc->model().find(QStringLiteral("m2")) == nullptr);
        QVERIFY(!doc->changedOnDisk(QFileInfo(file).lastModified()));
    }

    // The dock's pages are reached by page, never by number: the RF 2.5D
    // tab is one of them, it answers to its name, and the pages that were
    // there before it are where they were.
    void theDockReachesItsPagesByName()
    {
        QCOMPARE(QucsApp::sidePageName(QucsApp::SidePage::Rf25), QStringLiteral("rf 2.5d"));
        const int rf25 = app->sidePage(QucsApp::SidePage::Rf25);
        const int multiphysics = app->sidePage(QucsApp::SidePage::Multiphysics);
        QVERIFY(rf25 >= 0 && multiphysics >= 0);
        QVERIFY(rf25 != multiphysics);
        QVERIFY(app->showSidePage(QucsApp::SidePage::Rf25));
        QCOMPARE(app->currentSidePageName(), QStringLiteral("rf 2.5d"));
        QVERIFY(app->showSidePage(QucsApp::SidePage::Multiphysics));
        QCOMPARE(app->currentSidePageName(), QStringLiteral("multiphysics"));
        // Each page by name, there and back - what a session file keeps.
        for (QucsApp::SidePage page : {QucsApp::SidePage::Workspaces, QucsApp::SidePage::Projects, QucsApp::SidePage::Content,
                                       QucsApp::SidePage::Components, QucsApp::SidePage::Libraries,
                                       QucsApp::SidePage::FileBrowser, QucsApp::SidePage::Multiphysics,
                                       QucsApp::SidePage::Rf25}) {
            const QString name = QucsApp::sidePageName(page);
            QVERIFY2(!name.isEmpty(), qPrintable(name));
            QVERIFY2(app->showSidePageNamed(name), qPrintable(name));
            QCOMPARE(app->currentSidePageName(), name);
        }
        QVERIFY(!app->showSidePageNamed(QStringLiteral("no such page")));
        // A new model brings its own page to the front.
        app->showSidePage(QucsApp::SidePage::Components);
        app->newRf25Model();
        QCOMPARE(app->currentSidePageName(), QStringLiteral("rf 2.5d"));
    }

    // The layout file the model names is read in the background and drawn
    // in the layout viewer's own canvas; each layer of the file is drawn in
    // the colour of the stack layer the map sends it to, and one the map
    // leaves out is hollow, so that what is left out of the solve is plain
    // to see.
    void theLayoutIsDrawnInTheStacksColours()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("drawn.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY2(doc->waitForLayout(60000), "the layout was read");
        QVERIFY2(doc->layout() != nullptr, qPrintable(doc->layoutProblem()));
        QVERIFY(doc->layoutProblem().isEmpty());
        QVERIFY(!doc->layout()->layers.isEmpty());
        QCOMPARE(doc->view()->layout(), doc->layout());
        QCOMPARE(doc->view()->styles().size(), doc->layout()->layers.size());
        QVERIFY(doc->view()->cell() >= 0);
        QCOMPARE(itemOf(panel()->tree(), doc->model().layout().tag)->text(1).isEmpty(), false);

        // The map's row is 31/0, which this layout has not: every layer
        // hollow, and none of the stack's colours.
        for (const qucs_s::layout::LayerStyle& s : doc->view()->styles()) {
            QCOMPARE(s.pattern, Qt::NoBrush);
            QVERIFY(s.name.contains(QStringLiteral("not mapped")));
        }
        // The first layer of the file sent to M1: its colour, and the
        // stack's own for that layer.
        const qucs_s::layout::LayerKey key = doc->layout()->layers.first().key;
        QVERIFY(doc->editNode(doc->model().layout().child(QStringLiteral("rf_layer_map"))->tag,
                              [key](rf::Node& n) {
                                  n.props.insert(QStringLiteral("rows"),
                                                 rowsOf({{QString::number(key.layer), QString::number(key.datatype),
                                                          QStringLiteral("M1")}}));
                              },
                              QStringLiteral("map")));
        const qucs_s::layout::LayerStyle first = doc->view()->styles().first();
        QCOMPARE(first.pattern, Qt::SolidPattern);   // (a conductor: filled)
        int at = -1;
        const qucs_s::rf25::Stack stack = doc->model().stack();
        for (int i = 0; i < stack.layers.size(); ++i)
            if (stack.layers.at(i).name == QStringLiteral("M1")) at = i;
        QVERIFY(at >= 0);
        QCOMPARE(first.fill, RF25Doc::layerColor(at));
        QVERIFY(first.name.contains(QStringLiteral("M1")));

        // The stack beside it draws a layer for each of the stack's, and a
        // click on one chooses it in the tree.
        QVERIFY(doc->stackView() != nullptr);
        QCOMPARE(doc->stackView()->stack().layers.size(), stack.layers.size());
        doc->focusNode(QStringLiteral("m2"));
        QCOMPARE(doc->stackView()->focused(), QStringLiteral("m2"));
        QCOMPARE(panel()->currentTag(), QStringLiteral("m2"));

        // A file that is not there, and one that is no layout, are said so
        // and nothing is drawn.
        QVERIFY(doc->editNode(doc->model().layout().tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("file"), QStringLiteral("nowhere.gds")); },
                              QStringLiteral("file")));
        QVERIFY(doc->waitForLayout(30000));
        QVERIFY(doc->layout() == nullptr);
        QVERIFY2(doc->layoutProblem().contains(QStringLiteral("not there")), qPrintable(doc->layoutProblem()));
        QVERIFY(doc->problemsText()->toPlainText().contains(QStringLiteral("not there")));
        QVERIFY(doc->editNode(doc->model().layout().tag,
                              [this](rf::Node& n) { n.props.insert(QStringLiteral("file"), path(QStringLiteral("drawn.qrf25"))); },
                              QStringLiteral("file")));
        QVERIFY(doc->waitForLayout(30000));
        QVERIFY2(doc->layoutProblem().contains(QStringLiteral("not a GDSII")), qPrintable(doc->layoutProblem()));
    }

    // What the solver would be given, made of the layout as read: the
    // shapes processed, those shapes divided into cells, and the unknowns
    // counted - in the tab's notes, in its status line and under the very
    // nodes that decide them.
    void theShapesAndTheMeshAreMadeOfTheLayoutRead()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("made.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY2(doc->waitForLayout(60000), "the layout was read");
        QVERIFY2(doc->layout() != nullptr, qPrintable(doc->layoutProblem()));

        // The map names 31/0, which this layout has not: nothing comes
        // through, and the mesh is empty with it said.
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY(doc->processed() != nullptr);
        QVERIFY(doc->processed()->conductors.isEmpty());
        QVERIFY2(doc->processed()->report.ignored > 0, "the shapes left out went uncounted");
        QVERIFY(doc->mesh() != nullptr);
        QVERIFY(doc->mesh()->cells.isEmpty());
        QVERIFY2(says(doc->mesh()->problems, "no shapes to mesh"), qPrintable(doc->mesh()->problems.join(" | ")));

        // Its layers 1 and 11 sent to M1 and M2: the shapes come through
        // and are divided.
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")},
                    {QStringLiteral("11"), QStringLiteral("0"), QStringLiteral("M2")}});
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY(!doc->isPreparing());
        const std::shared_ptr<const rf::Processed> processed = doc->processed();
        const std::shared_ptr<const rf::Mesh> mesh = doc->mesh();
        QVERIFY2(processed != nullptr && !processed->conductors.isEmpty(),
                 qPrintable(processed ? processed->problems.join(" | ") : QStringLiteral("nothing processed")));
        QVERIFY(processed->report.read > 0);
        QCOMPARE(processed->byLayer().keys(), QStringList({QStringLiteral("M1"), QStringLiteral("M2")}));
        QVERIFY2(mesh != nullptr && !mesh->cells.isEmpty(),
                 qPrintable(mesh ? mesh->problems.join(" | ") : QStringLiteral("no mesh")));
        QVERIFY(mesh->ok());
        QVERIFY(mesh->cellSize > 0);
        // A wavelength at 40 GHz in the substrate, over 20 cells: the
        // cells are far larger than the 80 um the layout is wide, so each
        // conductor is a cell or a few, and the unknowns are the edges
        // they share.
        QVERIFY(mesh->cost.unknowns > 0);
        QVERIFY(!mesh->cost.tooBig);
        QVERIFY(mesh->cost.matrixBytes > 0);

        // The tab's notes say what each step did, and its status line the
        // cells and what they would cost.
        const QString notes = doc->problemsText()->toPlainText();
        QVERIFY2(notes.contains(QStringLiteral("What the layout gives the solver")), qPrintable(notes));
        for (const QString& line : doc->preparedLines()) QVERIFY2(notes.contains(line), qPrintable(line));
        QVERIFY2(doc->statusLabel()->text().contains(mesh->cost.said), qPrintable(doc->statusLabel()->text()));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("%1 cell(s)").arg(mesh->cells.size())),
                 qPrintable(doc->statusLabel()->text()));

        // And the panel says it under the nodes that decide it: the
        // processing's steps under the Processing, the cells and their
        // cost under the Mesh, and what the map let through under the
        // Layout.
        const rf::Node* processing = doc->model().layout().child(QStringLiteral("rf_processing"));
        QVERIFY(processing != nullptr);
        panel()->selectNode(processing->tag);
        QVERIFY2(!processed->report.lines.isEmpty(), "the processing said nothing of what it did");
        for (const QString& line : processed->report.lines) QVERIFY2(reportSays(line), qPrintable(line));
        panel()->selectNode(doc->model().mesh().tag);
        QVERIFY2(reportSays(mesh->cost.said), qPrintable(reportText()));
        panel()->selectNode(doc->model().layout().tag);
        QVERIFY2(reportSays(QStringLiteral("sent to a layer of the stack")), qPrintable(reportText()));
        // A node that decides none of it has no report of its own.
        panel()->selectNode(QStringLiteral("m1"));
        QVERIFY(panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("rfNodeReport")) == nullptr);

        // Read the Layout Again, with nothing else touched: that read
        // alone makes them, with no edit of the model behind it to have
        // asked for a run. (A run already there is not one that happened:
        // the count of runs is what says it did.)
        const int runs = doc->preparedRuns();
        doc->refreshLayout();
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->preparedRuns() > runs, "the read made nothing of the layout by itself");
        QVERIFY2(doc->processed() != nullptr && !doc->processed()->conductors.isEmpty(),
                 "the read did not make them again by itself");
        QVERIFY(doc->mesh() != nullptr && !doc->mesh()->cells.isEmpty());

        // And a layout that has gone takes them with it, by that same
        // button: the shapes of the file before must not be left over a
        // tab that now has none.
        QVERIFY(QFile::remove(path(QStringLiteral("heater.gds"))));
        doc->refreshLayout();
        QVERIFY(doc->waitForLayout(30000));
        QVERIFY(doc->waitForPrepared(30000));
        QVERIFY(doc->layout() == nullptr);
        QVERIFY2(doc->layoutProblem().contains(QStringLiteral("not there")), qPrintable(doc->layoutProblem()));
        QVERIFY2(doc->processed() == nullptr, "the shapes of the layout before were left behind");
        QVERIFY(doc->mesh() == nullptr);
        QVERIFY(doc->view()->overlays().isEmpty());
        QVERIFY(doc->preparedLines().isEmpty());
        QVERIFY(!doc->statusLabel()->text().contains(QStringLiteral("cell(s),")));
    }

    // The mesh is made again when what decides it changes - its own
    // settings, the processing, the stack, the study - and the layout is
    // not read again for it.
    void theMeshIsMadeAgainWithoutReadingTheLayoutAgain()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("again.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")},
                    {QStringLiteral("11"), QStringLiteral("0"), QStringLiteral("M2")}});
        QVERIFY(doc->waitForPrepared(60000));
        const std::shared_ptr<const qucs_s::layout::Layout> read = doc->layout();
        QVERIFY(read != nullptr);
        const int coarse = doc->mesh()->cells.size();
        const double coarseSize = doc->mesh()->cellSize;
        QVERIFY(coarse > 0);

        // A largest cell of 20 um, where a wavelength gave over a hundred:
        // smaller cells, more of them, more unknowns - and the same layout,
        // not read again.
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("maximum"), QStringLiteral("20[um]")); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));
        QCOMPARE(doc->layout(), read);
        QVERIFY2(doc->mesh()->cellSize < coarseSize,
                 qPrintable(QStringLiteral("%1 vs %2").arg(doc->mesh()->cellSize).arg(coarseSize)));
        QVERIFY2(doc->mesh()->cells.size() > coarse,
                 qPrintable(QStringLiteral("%1 vs %2").arg(doc->mesh()->cells.size()).arg(coarse)));
        QVERIFY(doc->mesh()->cost.unknowns > 0);

        // Fewer than four cells to a wavelength carries no current: said,
        // and under the Mesh node.
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("per_wavelength"), 2); },
                              QStringLiteral("per wavelength")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(says(doc->mesh()->problems, "too few"), qPrintable(doc->mesh()->problems.join(" | ")));
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral("stands in the way")),
                 qPrintable(doc->problemsText()->toPlainText()));
        panel()->selectNode(QStringLiteral("m1"));
        panel()->selectNode(doc->model().mesh().tag);
        QLabel* said = panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("rfNodeProblems"));
        QVERIFY(said != nullptr);
        QVERIFY2(said->text().contains(QStringLiteral("too few")), qPrintable(said->text()));

        // The Mesh node left where it is while the mesh is made again: the
        // run ending is what brings its report up to date, not a node
        // chosen anew.
        QCOMPARE(panel()->currentTag(), doc->model().mesh().tag);
        const QString before = reportText();
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) {
                                  n.props.insert(QStringLiteral("per_wavelength"), 20);
                                  n.props.insert(QStringLiteral("maximum"), QStringLiteral("8[um]"));
                              },
                              QStringLiteral("finer")));
        QVERIFY(doc->waitForPrepared(60000));
        QCoreApplication::processEvents();
        QCOMPARE(panel()->currentTag(), doc->model().mesh().tag);
        QVERIFY2(reportSays(doc->mesh()->cost.said), qPrintable(reportText() + QStringLiteral(" | want ") + doc->mesh()->cost.said));
        QVERIFY2(reportText() != before, "the report is the one of the run before");

        // The processing turned off is the processing not run: its steps
        // say nothing of a merge, and the shapes are the file's own.
        const rf::Node* processing = doc->model().layout().child(QStringLiteral("rf_processing"));
        QVERIFY(doc->editNode(processing->tag,
                              [](rf::Node& n) {
                                  n.props.insert(QStringLiteral("merge"), false);
                                  n.props.insert(QStringLiteral("merge_vias"), false);
                                  n.props.insert(QStringLiteral("heal"), false);
                                  n.props.insert(QStringLiteral("drop_slivers"), false);
                              },
                              QStringLiteral("processing")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(!says(doc->processed()->report.lines, "merged"), qPrintable(doc->processed()->report.summary()));
        QCOMPARE(doc->layout(), read);
    }

    // The processed shapes and the cells drawn over the layout they came
    // from, each layer in the colour the stack gives it, and either of the
    // two shown or hidden by its own button.
    void theProcessedShapesAndTheMeshAreDrawnOverTheLayout()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("drawnmesh.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")},
                    {QStringLiteral("11"), QStringLiteral("0"), QStringLiteral("M2")}});
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("maximum"), QStringLiteral("20[um]")); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));

        // The shapes, on by default: one overlay a layer, in that layer's
        // colour, holding that layer's conductors.
        QVERIFY(doc->processedShown());
        QVERIFY(!doc->meshShown());
        QCOMPARE(doc->processedButton()->isChecked(), true);
        QCOMPARE(doc->meshButton()->isChecked(), false);
        const QMap<QString, int> conductors = doc->processed()->byLayer();
        QCOMPARE(doc->view()->overlays().size(), conductors.size());
        const rf::Stack stack = doc->model().stack();
        int at = -1;
        for (int i = 0; i < stack.layers.size(); ++i)
            if (stack.layers.at(i).name == QStringLiteral("M1")) at = i;
        QVERIFY(at >= 0);
        bool foundM1 = false;
        for (const qucs_s::layout::LayoutView::Overlay& o : doc->view()->overlays())
            if (o.name.contains(QStringLiteral("M1"))) {
                foundM1 = true;
                QCOMPARE(o.shapes.size(), conductors.value(QStringLiteral("M1")));
                QCOMPARE(o.line, doc->outlineColor(RF25Doc::layerColor(at)));
                QVERIFY2(o.line != RF25Doc::layerColor(at), "the outline is the fill it lies on");
                QVERIFY(o.name.contains(QStringLiteral("processed")));
            }
        QVERIFY(foundM1);

        // The cells, turned on: their own overlays beside the shapes', one
        // a layer, holding that layer's cells.
        doc->setMeshShown(true);
        QCOMPARE(doc->meshButton()->isChecked(), true);
        const QMap<QString, int> cells = doc->mesh()->byLayer();
        QCOMPARE(doc->view()->overlays().size(), conductors.size() + cells.size());
        int drawnCells = 0;
        for (const qucs_s::layout::LayoutView::Overlay& o : doc->view()->overlays())
            if (o.name.contains(QStringLiteral("the cells"))) {
                drawnCells += o.shapes.size();
                // The cells are geometry: the canvas's ink, not a layer's
                // colour, and let through a little.
                QCOMPARE(o.line.rgb(), doc->view()->colors().ink.rgb());
                QVERIFY(o.line.alpha() < 255);
            }
        QCOMPARE(drawnCells, doc->mesh()->cells.size());
        QVERIFY(drawnCells > conductors.value(QStringLiteral("M1")));

        // The shapes turned off: the cells alone.
        doc->setProcessedShown(false);
        QCOMPARE(doc->view()->overlays().size(), cells.size());
        for (const qucs_s::layout::LayoutView::Overlay& o : doc->view()->overlays())
            QVERIFY(o.name.contains(QStringLiteral("the cells")));
        // Both off: nothing over the layout, which is still drawn.
        doc->setMeshShown(false);
        QVERIFY(doc->view()->overlays().isEmpty());
        QVERIFY(doc->view()->layout() != nullptr);

        // The canvas gone dark: the outline and the ink are told against
        // it, so both are taken again - darker than the fill on a light
        // canvas, lighter on a dark one, and never the fill itself.
        doc->setProcessedShown(true);
        doc->setMeshShown(true);
        const auto lineOf = [doc](const QString& words) {
            for (const qucs_s::layout::LayoutView::Overlay& o : doc->view()->overlays())
                if (o.name.contains(words)) return o.line;
            return QColor();
        };
        doc->view()->setTheme(qucs_s::layout::LayoutView::Theme::Light);
        const QColor onLight = lineOf(QStringLiteral("M1, processed"));
        const QColor inkOnLight = lineOf(QStringLiteral("the cells"));
        QVERIFY(onLight.isValid() && inkOnLight.isValid());
        QVERIFY2(onLight.lightness() < RF25Doc::layerColor(at).lightness(),
                 qPrintable(QStringLiteral("%1 vs %2").arg(onLight.name(), RF25Doc::layerColor(at).name())));
        doc->view()->setTheme(qucs_s::layout::LayoutView::Theme::Dark);
        const QColor onDark = lineOf(QStringLiteral("M1, processed"));
        QVERIFY(onDark.isValid());
        QVERIFY2(onDark.lightness() > RF25Doc::layerColor(at).lightness(),
                 qPrintable(QStringLiteral("%1 vs %2").arg(onDark.name(), RF25Doc::layerColor(at).name())));
        QVERIFY(onDark != onLight);
        QCOMPARE(lineOf(QStringLiteral("the cells")).rgb(), doc->view()->colors().ink.rgb());
        QVERIFY(lineOf(QStringLiteral("the cells")).rgb() != inkOnLight.rgb());
        doc->view()->setTheme(qucs_s::layout::LayoutView::Theme::Application);

        // And they are drawn: the view fitted draws every cell it has, and
        // a view looking somewhere else draws none of them.
        doc->setProcessedShown(true);
        doc->setMeshShown(true);
        doc->view()->fit();
        QVERIFY(!doc->view()->picture().isNull());
        const int shapes = doc->processed()->conductors.size() + doc->mesh()->cells.size();
        QCOMPARE(doc->view()->overlayShapesDrawn(), shapes);
        QCOMPARE(doc->view()->overlayShapesLeftOut(), 0);
        doc->view()->setView(QPointF(1e6, 1e6), doc->view()->scale());
        QVERIFY(!doc->view()->picture().isNull());
        QCOMPARE(doc->view()->overlayShapesDrawn(), 0);
    }

    // The solve: asked for, done in the background, and what it found
    // said in the tab's notes, its status line and under the Results node
    // - with a line's impedance worked out from the capacitance.
    void theModelIsSolvedAndSaysWhatItFound()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("solved.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}});
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("maximum"), QStringLiteral("20[um]")); },
                              QStringLiteral("largest cell")));
        // A metal that conducts and a substrate that loses, so that the
        // solve has R and G to report beside C at the study's frequency.
        QVERIFY(doc->editNode(QStringLiteral("m1"),
                              [](rf::Node& n) {
                                  n.props.insert(QStringLiteral("sigma"), QStringLiteral("5.8e7"));
                                  n.props.insert(QStringLiteral("thickness"), QStringLiteral("3[um]"));
                              },
                              QStringLiteral("copper")));
        QString substrate;
        for (const rf::Node& child : doc->model().stackNode().children)
            if (child.type == QLatin1String("rf_substrate")) substrate = child.tag;
        QVERIFY2(!substrate.isEmpty(), "the blank model has a substrate");
        QVERIFY(doc->editNode(substrate,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("tand"), QStringLiteral("0.002")); },
                              QStringLiteral("a lossy substrate")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY(doc->mesh() != nullptr && !doc->mesh()->cells.isEmpty());

        // Nothing is solved until it is asked for: the shapes and the
        // mesh are made at every edit, a dense matrix is not.
        QVERIFY(doc->extraction() == nullptr);
        QVERIFY(doc->solveButton() != nullptr);
        QCOMPARE(doc->solveButton()->text(), QStringLiteral("Solve"));
        QVERIFY2(panel()->settingsArea() != nullptr, "the panel has a form");
        panel()->selectNode(doc->model().root.child(QStringLiteral("rf_results"))->tag);
        QVERIFY2(reportSays(QStringLiteral("Nothing is solved yet")), qPrintable(reportText()));

        doc->solveButton()->click();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        QCOMPARE(doc->solveButton()->text(), QStringLiteral("Solve"));
        const std::shared_ptr<const rf::Extraction> e = doc->extraction();
        QVERIFY2(e != nullptr && e->ok(), qPrintable(doc->solveProblem()));
        QVERIFY(!e->names.isEmpty());
        QVERIFY(e->unknowns > 0);
        QVERIFY2(e->selfCapacitance(0) > 0, qPrintable(QString::number(e->selfCapacitance(0))));
        // The length it was taken over is the longer side of what was
        // solved, which for this layout is its 1200 µm of wire.
        QVERIFY2(doc->solvedLength() > 1e-3 && doc->solvedLength() < 2e-3,
                 qPrintable(QString::number(doc->solvedLength() * 1e6)));
        // A line's own words: a capacitance per metre and an impedance.
        const QStringList said = doc->lineSummary();
        QVERIFY2(!said.isEmpty(), "it said nothing of the line");
        QVERIFY2(said.first().contains(QStringLiteral("pF/m")) && said.first().contains(QStringLiteral("Z0")),
                 qPrintable(said.first()));
        // And R, L, G and C at the top of the study's own sweep, which a
        // blank model puts at 40 GHz.
        QVERIFY2(said.size() > 1, qPrintable(said.join(" | ")));
        QVERIFY2(said.at(1).contains(QStringLiteral("40 GHz")) && said.at(1).contains(QStringLiteral("R "))
                     && said.at(1).contains(QStringLiteral("nH/m")) && said.at(1).contains(QStringLiteral("dB/m")),
                 qPrintable(said.at(1)));
        // In the notes, in the status line, and under the Results node.
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral("What it solves to")),
                 qPrintable(doc->problemsText()->toPlainText()));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("Z0")), qPrintable(doc->statusLabel()->text()));
        // The Results node was chosen before the solve and has not been
        // chosen again: the solve ending is what brings its report up to
        // date, not a node picked anew.
        QCOMPARE(panel()->currentTag(), doc->model().root.child(QStringLiteral("rf_results"))->tag);
        QCoreApplication::processEvents();
        QVERIFY2(reportSays(QStringLiteral("Z0")), qPrintable(reportText()));
        QVERIFY2(!reportText().contains(QStringLiteral("Nothing is solved yet")), qPrintable(reportText()));

        // An edit throws it away: a capacitance of shapes that are no
        // longer there is worse than none at all.
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("maximum"), QStringLiteral("30[um]")); },
                              QStringLiteral("coarser")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->extraction() == nullptr, "the capacitance of the mesh before was kept");
        QVERIFY(doc->lineSummary().isEmpty());
        QVERIFY(!doc->problemsText()->toPlainText().contains(QStringLiteral("What it solves to")));
    }

    // What the solve says of the model as a circuit: the S parameters of
    // each net, where the quasi-static answer gives out, and the
    // Touchstone the Exports node writes of them.
    void theSolvedModelIsWrittenAsATouchstone()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("written.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}});
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("maximum"), QStringLiteral("20[um]")); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->editNode(QStringLiteral("m1"),
                              [](rf::Node& n) {
                                  n.props.insert(QStringLiteral("sigma"), QStringLiteral("5.8e7"));
                                  n.props.insert(QStringLiteral("thickness"), QStringLiteral("3[um]"));
                              },
                              QStringLiteral("copper")));
        QVERIFY(doc->waitForPrepared(60000));

        // Nothing is solved, so there is nothing to write - and it says
        // so rather than writing an empty file.
        rf::Node touchstone = doc->model().make(QStringLiteral("rf_touchstone"));
        const QString tag = touchstone.tag;
        QVERIFY(!tag.isEmpty());
        {
            rf::Model m = doc->model();
            QVERIFY(m.add(QStringLiteral("exports"), touchstone) != nullptr);
            QVERIFY(doc->setModel(m, QStringLiteral("an export")));
        }
        QString why;
        QVERIFY2(doc->exportText(tag, &why).isEmpty(), "it wrote something of nothing");
        QVERIFY2(why.contains(QStringLiteral("Nothing is solved")) || why.contains(QStringLiteral("no mesh")),
                 qPrintable(why));

        doc->solve();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::Extraction> e = doc->extraction();
        QVERIFY2(e != nullptr && e->ok(), qPrintable(doc->solveProblem()));

        // The S parameters of the line, and where the quasi-static answer
        // stops being the whole story: both in the Results node's report.
        const QStringList sp = doc->sParameterSummary();
        QVERIFY2(!sp.isEmpty(), "it said nothing of the S parameters");
        QVERIFY2(sp.first().contains(QStringLiteral("S11")) && sp.first().contains(QStringLiteral("S21"))
                     && sp.first().contains(QStringLiteral("dB")),
                 qPrintable(sp.first()));
        QVERIFY2(sp.join(QStringLiteral(" | ")).contains(QStringLiteral("Quasi-static to about")),
                 qPrintable(sp.join(" | ")));
        QVERIFY2(sp.join(QStringLiteral(" | ")).contains(QStringLiteral("half wave")), qPrintable(sp.join(" | ")));
        panel()->selectNode(doc->model().root.child(QStringLiteral("rf_results"))->tag);
        QVERIFY2(reportSays(QStringLiteral("S21")), qPrintable(reportText()));
        QVERIFY2(reportSays(QStringLiteral("Quasi-static to about")), qPrintable(reportText()));

        // And the file: a Touchstone of the study's own 41 points, written
        // where the node says - which, naming nothing, is beside the model.
        const QString written = doc->exportText(tag, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QVERIFY2(written.contains(QStringLiteral("# Hz S RI R 50")), qPrintable(written.left(600)));
        QVERIFY2(written.contains(QStringLiteral("written.qrf25")), "the file does not say what made it");
        QVERIFY2(written.contains(QStringLiteral("Quasi-static")), "the file does not say what the solve was");
        QStringList rows;
        for (const QString& row : written.split(QLatin1Char('\n')))
            if (!row.trimmed().isEmpty() && !row.startsWith(QLatin1Char('!')) && !row.startsWith(QLatin1Char('#'))) rows << row;
        QCOMPARE(rows.size(), 41);
        for (const QString& row : rows) QCOMPARE(row.split(QLatin1Char(' '), Qt::SkipEmptyParts).size(), 9);

        const QString path = doc->exportPath(tag);
        QVERIFY2(path.endsWith(QStringLiteral(".s2p")), qPrintable(path));
        QVERIFY2(path.contains(QStringLiteral("written")), qPrintable(path));
        QVERIFY(!QFile::exists(path));
        // The panel's own Write button, which is the only way a user has.
        panel()->selectNode(tag);
        auto* write = panel()->settingsArea()->findChild<QPushButton*>(QStringLiteral("rfWriteExport"));
        QVERIFY2(write != nullptr, "the export node has no Write button");
        write->click();
        QCoreApplication::processEvents();
        auto* said = panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("rfExportSaid"));
        QVERIFY(said != nullptr);
        QVERIFY2(said->text().contains(QStringLiteral("Written to")), qPrintable(said->text()));
        QVERIFY2(QFile::exists(path), qPrintable(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString onDisk = QString::fromUtf8(file.readAll());
        QCOMPARE(onDisk, written);

        // A net that is not there is refused by name, not written as the
        // first one.
        {
            rf::Model m = doc->model();
            rf::Node* node = m.root.find(tag);
            QVERIFY(node != nullptr);
            node->props.insert(QStringLiteral("net"), QStringLiteral("M9"));
            QVERIFY(doc->setModel(m, QStringLiteral("a net that is none")));
        }
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY(doc->exportText(tag, &why).isEmpty());
        QVERIFY2(why.contains(QStringLiteral("no net called M9")), qPrintable(why));
    }

    // What a solve will not do: run with no mesh under it, and run twice
    // at once.
    void aSolveWithNothingToSolveOnIsRefused()
    {
        RF25Doc* doc = app->newRf25Model();
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForPrepared(30000));
        // A new model names no layout, so there are no cells.
        doc->solve();
        QVERIFY(!doc->isSolving());
        QVERIFY(doc->extraction() == nullptr);
        QVERIFY2(doc->solveProblem().contains(QStringLiteral("no mesh")), qPrintable(doc->solveProblem()));
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral("no mesh")),
                 qPrintable(doc->problemsText()->toPlainText()));
        panel()->selectNode(doc->model().root.child(QStringLiteral("rf_results"))->tag);
        QLabel* said = panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("rfNodeProblems"));
        QVERIFY(said != nullptr);
        QVERIFY2(said->text().contains(QStringLiteral("no mesh")), qPrintable(said->text()));
    }

    // A model dropped on the tabs, and one double-clicked in the project
    // view, open in a tab of their own - not in the text editor, although
    // the file is JSON and reads as text.
    void aModelOpensFromADropAndFromTheProjectView()
    {
        const QString file = written(QStringLiteral("dropped.qrf25"));
        app->openDroppedFile(file);
        QTRY_COMPARE(app->DocumentTab->count(), 1);
        QVERIFY(QucsApp::isRf25Document(app->DocumentTab->widget(0)));
        app->closeAllFiles();

        app->openFileFromProjectView(QFileInfo(file), QString());
        QTRY_COMPARE(app->DocumentTab->count(), 1);
        QVERIFY(QucsApp::isRf25Document(app->DocumentTab->widget(0)));
        // Its tree in front, and the panel showing this model.
        QCOMPARE(app->currentSidePageName(), QStringLiteral("rf 2.5d"));
        QCOMPARE(panel()->document(), front());
        // The RF 2.5D menu is in the bar, and what needs a layout read is
        // off while one is being read.
        QVERIFY(app->rf25Menu() != nullptr);
        QVERIFY(app->rf25Menu()->findChild<QAction*>(QStringLiteral("rfNewModelAction")) != nullptr);
    }

    // The window's own Edit > Undo and Redo work on this tab, and its
    // Zoom In, Copy and Find do not fall through to the schematic paths:
    // every list of document kinds in qucs_actions.cpp knows this one.
    void theWindowsEditActionsKnowIt()
    {
        RF25Doc* doc = app->newRf25Model();
        panel()->selectNode(QStringLiteral("m1"));
        auto* thickness = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("thickness")));
        QVERIFY(thickness != nullptr);
        thickness->setText(QStringLiteral("3[um]"));
        emit thickness->editingFinished();
        QCOMPARE(rf::textOf(*doc->model().find(QStringLiteral("m1")), QStringLiteral("thickness")), QStringLiteral("3[um]"));
        QVERIFY(app->undo->isEnabled());
        app->slotEditUndo();
        QCOMPARE(rf::textOf(*doc->model().find(QStringLiteral("m1")), QStringLiteral("thickness")), QStringLiteral("0.6[um]"));
        QVERIFY(app->redo->isEnabled());
        app->slotEditRedo();
        QCOMPARE(rf::textOf(*doc->model().find(QStringLiteral("m1")), QStringLiteral("thickness")), QStringLiteral("3[um]"));

        // Zoomed in and out: the view's scale, and nothing of the
        // schematic's.
        const double scale = doc->view()->scale();
        app->slotZoomIn(true);
        const double zoomed = doc->view()->scale();
        QVERIFY(zoomed > scale);
        app->slotZoomOut();   // (by two, where Zoom In is by one and a half)
        QVERIFY(doc->view()->scale() < zoomed);
        // Find says nothing and does nothing: there is no text in it.
        app->slotEditFind();
        // Copy puts the layout as it is drawn on the clipboard.
        app->slotEditCopy();
        QVERIFY(!QApplication::clipboard()->image().isNull());
    }

    // The panel's node menu adds what may go under the node and nothing
    // else, and it says what it will not do.
    void theNodeMenuOffersWhatMayGoThere()
    {
        RF25Doc* doc = app->newRf25Model();
        QVERIFY(doc != nullptr);
        const auto titles = [](QMenu* menu) {
            QStringList out;
            for (QAction* a : menu->actions()) {
                if (a->menu() != nullptr)
                    for (QAction* b : a->menu()->actions()) out << b->text();
                else if (!a->isSeparator())
                    out << a->text();
            }
            return out;
        };
        QMenu* stack = panel()->menuFor(QStringLiteral("stack"));
        QVERIFY(stack != nullptr);
        const QStringList under = titles(stack);
        QVERIFY2(under.contains(QStringLiteral("Metal")), qPrintable(under.join(" | ")));
        QVERIFY2(under.contains(QStringLiteral("Via")), qPrintable(under.join(" | ")));
        QVERIFY2(!under.contains(QStringLiteral("Touchstone")), qPrintable(under.join(" | ")));
        // The stack is a part of the tree itself: not renamed, not deleted.
        QVERIFY2(!under.contains(QStringLiteral("Delete")), qPrintable(under.join(" | ")));
        stack->deleteLater();

        QMenu* metal = panel()->menuFor(QStringLiteral("m1"));
        QVERIFY(metal != nullptr);
        const QStringList itsOwn = titles(metal);
        QVERIFY2(itsOwn.contains(QStringLiteral("Delete")), qPrintable(itsOwn.join(" | ")));
        QVERIFY2(itsOwn.contains(QStringLiteral("Disable")), qPrintable(itsOwn.join(" | ")));
        metal->deleteLater();

        // A kind may not go where it does not belong, and it is said.
        {
            misc::ErrorCapture quiet;
            QVERIFY(panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_touchstone")).isEmpty());
        }
        QVERIFY(panel()->menuFor(QStringLiteral("no such tag")) == nullptr);
    }

    // The parameters and the layer map are tables: a row added, typed and
    // taken away, each one step of Undo; and the stack reads the
    // parameters - a thickness given as "t_m" follows the table.
    void aTableIsEditedRowByRow()
    {
        RF25Doc* doc = app->newRf25Model();
        const rf::Node* parameters = doc->model().root.child(QStringLiteral("rf_parameters"));
        QVERIFY(parameters != nullptr);
        panel()->selectNode(parameters->tag);
        auto* table = panel()->settingsArea()->findChild<QTableWidget*>(QStringLiteral("rfRows_rows"));
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 0);
        auto* add = panel()->settingsArea()->findChild<QPushButton*>(QStringLiteral("rfRowsAdd_rows"));
        QVERIFY(add != nullptr);
        add->click();
        QCOMPARE(doc->model().parameters().size(), 1);
        QCOMPARE(doc->model().parameters().first().name, QStringLiteral("p1"));

        // Named and given a value, the stack reads it.
        QVERIFY(doc->editNode(parameters->tag,
                              [](rf::Node& n) {
                                  n.props.insert(QStringLiteral("rows"),
                                                 rowsOf({{QStringLiteral("t_m"), QStringLiteral("0.8[um]"),
                                                          QStringLiteral("the metal")}}));
                              },
                              QStringLiteral("parameters")));
        QVERIFY(doc->editNode(QStringLiteral("m1"),
                              [](rf::Node& n) { n.props.insert(QStringLiteral("thickness"), QStringLiteral("t_m")); },
                              QStringLiteral("thickness")));
        const qucs_s::rf25::Stack stack = doc->model().stack();
        QVERIFY2(stack.ok(), qPrintable(stack.problems.join(" | ")));
        QCOMPARE(stack.named(QStringLiteral("M1"))->thickness, 0.8e-6);
        // The panel says what it is, in the metal's own units.
        panel()->selectNode(QStringLiteral("m1"));
        auto* thickness = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("thickness")));
        QVERIFY(thickness != nullptr);
        QVERIFY2(thickness->toolTip().contains(QStringLiteral("800 nm")), qPrintable(thickness->toolTip()));

        // A row taken away: the parameter is gone, and the stack says the
        // thickness is no number any more.
        panel()->selectNode(parameters->tag);
        table = panel()->settingsArea()->findChild<QTableWidget*>(QStringLiteral("rfRows_rows"));
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 1);
        table->setCurrentCell(0, 0);
        auto* remove = panel()->settingsArea()->findChild<QPushButton*>(QStringLiteral("rfRowsRemove_rows"));
        QVERIFY(remove != nullptr);
        remove->click();
        QCOMPARE(doc->model().parameters().size(), 0);
        QVERIFY2(says(doc->model().stack().problems, "not a number"),
                 qPrintable(doc->model().stack().problems.join(" | ")));
        doc->undo();
        QCOMPARE(doc->model().parameters().size(), 1);
    }
};

QTEST_MAIN(TestRf25Doc)
#include "test_rf25_doc.moc"
