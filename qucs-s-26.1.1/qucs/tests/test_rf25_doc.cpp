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
#include <QThread>
#include <QElapsedTimer>
#include <QTimer>
#include <QFileDialog>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include "config.h"
#include "dialogs/qucssettingsdialog.h"
#include "gpurendering.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "layoutdoc.h"
#include "rf25doc.h"
#include "rf25/rf25_dense.h"
#include "rf25/rf25_dielectric.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_static.h"
#include "rf25settings.h"
#include "settings.h"
#include "rf25panel.h"
#include "schematic.h"
#include "rf25/rf25_sparams.h"

#include <gdstk/gdstk.hpp>

namespace rf = qucs_s::rf25;
using LayoutView = qucs_s::layout::LayoutView;

// gdstk's Vec2 is a union of {x, y} and others: {1, 2} is one, braces or not.
#pragma GCC diagnostic ignored "-Wmissing-braces"

extern QString lastDirOpenSave;   // (qucs.cpp)

namespace {

QString examples()
{
    return QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
}

/// Whether this build is one whose dense solves are many times slower - a
/// sanitized one, as CI's Linux job is (QUCS_TEST_SLOW: its path in any).
bool slowBuild()
{
    if (qEnvironmentVariableIsSet("QUCS_TEST_SLOW")) return true;
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
    return false;
}

/// A GDSII file of one strip \a length by \a width µm on layer 1/0, in
/// cell TOP: a line, as plain as a layout can be.
bool writeStrip(const QString& path, double length, double width)
{
    gdstk::Library lib = {};
    lib.init("STRIP", 1e-6, 1e-9);
    auto* cell = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
    cell->init("TOP");
    lib.cell_array.append(cell);
    auto* strip = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
    *strip = gdstk::rectangle(gdstk::Vec2{0, 0}, gdstk::Vec2{length, width}, gdstk::make_tag(1, 0));
    cell->polygon_array.append(strip);
    const bool ok = lib.write_gds(QFile::encodeName(path).constData(), 0, nullptr) == gdstk::ErrorCode::NoError;
    lib.free_all();
    return ok;
}

/// A strip of \a length by \a width (µm) on layer 1/0 and, at its far end,
/// a square via of \a side on layer 2/0.
bool writeStripWithVia(const QString& path, double length, double width, double side)
{
    gdstk::Library lib = {};
    lib.init("STRIP", 1e-6, 1e-9);
    auto* cell = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
    cell->init("TOP");
    lib.cell_array.append(cell);
    auto* strip = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
    *strip = gdstk::rectangle(gdstk::Vec2{0, 0}, gdstk::Vec2{length, width}, gdstk::make_tag(1, 0));
    cell->polygon_array.append(strip);
    auto* via = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
    *via = gdstk::rectangle(gdstk::Vec2{length - side, (width - side) / 2}, gdstk::Vec2{length, (width + side) / 2},
                            gdstk::make_tag(2, 0));
    cell->polygon_array.append(via);
    const bool ok = lib.write_gds(QFile::encodeName(path).constData(), 0, nullptr) == gdstk::ErrorCode::NoError;
    lib.free_all();
    return ok;
}

/// A GDSII file of \a boxes (µm) on layer 1/0: several conductors side by
/// side, a coplanar line's.
bool writeRectangles(const QString& path, const QList<QRectF>& boxes)
{
    gdstk::Library lib = {};
    lib.init("BOXES", 1e-6, 1e-9);
    auto* cell = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
    cell->init("TOP");
    lib.cell_array.append(cell);
    for (const QRectF& b : boxes) {
        auto* box = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
        *box = gdstk::rectangle(gdstk::Vec2{b.left(), b.top()}, gdstk::Vec2{b.right(), b.bottom()}, gdstk::make_tag(1, 0));
        cell->polygon_array.append(box);
    }
    const bool ok = lib.write_gds(QFile::encodeName(path).constData(), 0, nullptr) == gdstk::ErrorCode::NoError;
    lib.free_all();
    return ok;
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

// A SPICE number as a netlist writes one ("117.3p", "1.2k", "4.6e-14").
double spiceNumber(QString t)
{
    t = t.trimmed().toLower();
    static const QRegularExpression number(QStringLiteral("^([-+]?[0-9]*\\.?[0-9]+(?:e[-+]?[0-9]+)?)(meg|[fpnumkgt])?"));
    const QRegularExpressionMatch m = number.match(t);
    if (!m.hasMatch()) return std::nan("");
    static const QHash<QString, double> scales{{QStringLiteral("f"), 1e-15}, {QStringLiteral("p"), 1e-12}, {QStringLiteral("n"), 1e-9},
                                               {QStringLiteral("u"), 1e-6},  {QStringLiteral("m"), 1e-3},  {QStringLiteral("k"), 1e3},
                                               {QStringLiteral("meg"), 1e6}, {QStringLiteral("g"), 1e9},   {QStringLiteral("t"), 1e12}};
    return m.captured(1).toDouble() * scales.value(m.captured(2), 1.0);
}

// What a netlist's R, L and C lines make between nodes \a pins, node
// \a reference held at nought, at \a w: their Y, the other nodes taken out
// - the netlist read as a circuit, whatever wrote it.
QList<QList<std::complex<double>>> yOfNetlist(const QString& text, const QStringList& pins, const QString& reference, double w,
                                              int* elements)
{
    QStringList nodes = pins;
    struct Element {
        QChar kind;
        QString a, b;
        double value;
    };
    QList<Element> list;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const QChar kind = line.at(0).toUpper();
        if (kind != QLatin1Char('R') && kind != QLatin1Char('L') && kind != QLatin1Char('C')) continue;
        const QStringList f = line.split(QRegularExpression(QStringLiteral("\\s+")));
        if (f.size() < 4 || !std::isfinite(spiceNumber(f.at(3)))) continue;
        list << Element{kind, f.at(1), f.at(2), spiceNumber(f.at(3))};
        for (const QString& n : {f.at(1), f.at(2)})
            if (n != reference && !nodes.contains(n)) nodes << n;
    }
    *elements = int(list.size());
    const int n = int(nodes.size()), p = int(pins.size());
    std::vector<std::vector<std::complex<double>>> y(n, std::vector<std::complex<double>>(n));
    for (const Element& e : std::as_const(list)) {
        const std::complex<double> g = e.kind == QLatin1Char('R') ? std::complex<double>(1.0 / e.value)
                                       : e.kind == QLatin1Char('L') ? 1.0 / std::complex<double>(0, w * e.value)
                                                                    : std::complex<double>(0, w * e.value);
        const int i = e.a == reference ? -1 : int(nodes.indexOf(e.a)), j = e.b == reference ? -1 : int(nodes.indexOf(e.b));
        if (i >= 0) y[i][i] += g;
        if (j >= 0) y[j][j] += g;
        if (i >= 0 && j >= 0) {
            y[i][j] -= g;
            y[j][i] -= g;
        }
    }
    // Each inner node eliminated in turn, the last first (Gauss).
    for (int k = n - 1; k >= p; --k)
        for (int i = 0; i < k; ++i)
            for (int j = 0; j < k; ++j) y[i][j] -= y[i][k] * y[k][j] / y[k][k];
    QList<QList<std::complex<double>>> out(p, QList<std::complex<double>>(p));
    for (int i = 0; i < p; ++i)
        for (int j = 0; j < p; ++j) out[i][j] = y[i][j];
    return out;
}

// Circuit \a c's Y at \a w, from its S.
QList<QList<std::complex<double>>> yOfCircuit(const rf::EquivalentCircuit& c, double w)
{
    const QList<QList<std::complex<double>>> s = c.sAt(w / (2 * M_PI));
    // (A two-port's Y from S into equal references: 1/Z0 (1 - S)(1 + S)^-1.)
    const std::complex<double> one(1, 0);
    const std::complex<double> a = one + s[0][0], b = s[0][1], cc = s[1][0], d = one + s[1][1];
    const std::complex<double> det = a * d - b * cc;
    const std::complex<double> i00 = d / det, i01 = -b / det, i10 = -cc / det, i11 = a / det;
    const std::complex<double> m00 = one - s[0][0], m01 = -s[0][1], m10 = -s[1][0], m11 = one - s[1][1];
    const double r = c.references.at(0);
    return {{(m00 * i00 + m01 * i10) / r, (m00 * i01 + m01 * i11) / r}, {(m10 * i00 + m11 * i10) / r, (m10 * i01 + m11 * i11) / r}};
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
    /// The mesh node \a n sized by its largest cell alone, \a largest, and
    /// not by ten cells across the heater's narrow trace, which is
    /// thousands of cells - a solve of minutes under the sanitizers.
    static void coarsely(rf::Node& n, const char* largest)
    {
        n.props.insert(QStringLiteral("maximum"), QLatin1String(largest));
        n.props.insert(QStringLiteral("cells_across"), 0);
    }
    QString written(const QString& name)
    {
        rf::Model m = rf::Model::blank();
        m.mesh().props.insert(QStringLiteral("cells_across"), 0);
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
        // The ground's surface impedance likewise: its kind alone. (The
        // starter's own ground: a new model has one.)
        QString ground;
        for (const rf::Node& c : doc->model().stackNode().children)
            if (c.type == QLatin1String("rf_ground")) ground = c.tag;
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
        // order they are added in - and the tree is in z from the bottom
        // up, so the ground is first and the cover last. (This held the
        // ground after the layers, the order the phase A check of 24
        // October found the Ground's help and the Stack's at odds over.)
        // A new model has its ground already, and a second is refused.
        const QString ground = doc->model().stackNode().children.front().tag;
        QCOMPARE(doc->model().find(ground)->type, QStringLiteral("rf_ground"));
        {
            misc::ErrorCapture quiet;
            QVERIFY(panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_ground")).isEmpty());
        }
        const QString cover = panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_cover"));
        const QString last = panel()->addNode(QStringLiteral("stack"), QStringLiteral("rf_metal"));
        QStringList order;
        for (const rf::Node& c : doc->model().stackNode().children) order << c.tag;
        QCOMPARE(order.first(), ground);
        QVERIFY(order.indexOf(last) > order.indexOf(ground));
        QCOMPARE(order.last(), cover);
        // (Two metals with nothing between them are a problem of their
        //  own here; the order of the ground and the cover is not.)
        for (const QString& problem : doc->model().stack().problems)
            QVERIFY2(!problem.contains(QStringLiteral("not the first layer")) && !problem.contains(QStringLiteral("not the last layer")),
                     qPrintable(problem));

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
        QCOMPARE(back, QStringList({QStringLiteral("gnd1"), QStringLiteral("sub1"), QStringLiteral("d1"), QStringLiteral("m1")}));
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
        // (The charge's count and cost, which is what a solve is of: the
        // full-wave edges' count is the mesh line's, said as such, not a
        // second number beside the first - the phase B check's N2.)
        QVERIFY2(doc->statusLabel()->text().contains(rf::chargeCostOf(int(mesh->cells.size()))),
                 qPrintable(doc->statusLabel()->text()));
        QVERIFY2(!doc->statusLabel()->text().contains(mesh->cost.said), qPrintable(doc->statusLabel()->text()));
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
                              [](rf::Node& n) { coarsely(n, "20[um]"); },
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
                                  coarsely(n, "8[um]");
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

    // The Cutout: a region added from its menu's Add Region and from its
    // form's button, each a step of Undo; the region's form its shape's
    // lengths alone; the shapes the tab solves cut to it, and it drawn over
    // the layout - whatever else is shown.
    void aCutoutSolvesAPartOfTheLayout()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("cutout.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")},
                    {QStringLiteral("11"), QStringLiteral("0"), QStringLiteral("M2")}});
        QVERIFY(doc->waitForPrepared(60000));
        const std::shared_ptr<const rf::Processed> whole = doc->processed();
        QVERIFY(whole != nullptr && !whole->conductors.isEmpty());
        QRectF all;
        double wholeArea = 0;
        for (const rf::Conductor& c : whole->conductors) {
            all = all.united(c.outline.boundingRect());
            wholeArea += c.area();
        }
        QTreeWidget* tree = panel()->tree();
        QTreeWidgetItem* item = itemOf(tree, QStringLiteral("cutout"));
        QVERIFY(item != nullptr);
        QCOMPARE(item->text(0), QStringLiteral("Cutout"));
        QCOMPARE(item->parent(), itemOf(tree, QStringLiteral("layout")));
        QCOMPARE(item->text(1), QStringLiteral("the whole layout"));

        // Its menu: Add Region, in the menu itself.
        const rf::Node* cutout = doc->model().layout().child(QStringLiteral("rf_cutout"));
        QMenu* menu = panel()->menuFor(QStringLiteral("cutout"));
        QVERIFY(menu != nullptr);
        QAction* addRegion = nullptr;
        for (QAction* a : menu->actions())
            if (a->objectName() == QLatin1String("rfAdd_rf_region")) addRegion = a;
        QVERIFY(addRegion != nullptr);
        QCOMPARE(addRegion->text(), QStringLiteral("Add Region"));
        const int steps = doc->undoStack()->count();
        addRegion->trigger();
        menu->deleteLater();
        QCOMPARE(int(cutout->children.size()), 1);
        QCOMPARE(cutout->children.at(0).type, QStringLiteral("rf_region"));
        QCOMPARE(doc->undoStack()->count(), steps + 1);
        doc->undo();
        QCOMPARE(int(cutout->children.size()), 0);

        // Its form: the same as a button.
        panel()->selectNode(QStringLiteral("cutout"));
        QPushButton* button = nullptr;
        QTRY_VERIFY((button = panel()->settingsArea()->findChild<QPushButton*>(QStringLiteral("rfCutoutAddRegion"))) != nullptr);
        QVERIFY2(reportText().contains(QStringLiteral("the whole layout is solved")), qPrintable(reportText()));
        button->click();
        QCOMPARE(int(cutout->children.size()), 1);
        const QString tag = cutout->children.at(0).tag;
        QTRY_COMPARE(itemOf(tree, QStringLiteral("cutout"))->text(1), QStringLiteral("1 region(s)"));
        QCOMPARE(itemOf(tree, tag)->text(1), QStringLiteral("kept"));

        // The region's form: a rectangle's corners; a polygon's corners as a
        // table in their place.
        panel()->selectNode(tag);
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("x0")) != nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("points")) == nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("radius")) == nullptr);
        QVERIFY(panel()->settingsArea()->findChild<QToolButton*>(QStringLiteral("rfPick_layers")) != nullptr);
        auto* shape = qobject_cast<QComboBox*>(panel()->editorFor(QStringLiteral("shape")));
        QVERIFY(shape != nullptr);
        shape->setCurrentIndex(shape->findData(QStringLiteral("polygon")));
        emit shape->activated(shape->currentIndex());
        QTRY_VERIFY(panel()->editorFor(QStringLiteral("points")) != nullptr);
        QVERIFY(panel()->editorFor(QStringLiteral("x0")) == nullptr);
        doc->undo();
        QCOMPARE(rf::textOf(*doc->model().find(tag), QStringLiteral("shape")), QStringLiteral("rectangle"));

        // Over the left half of the layout: what the tab solves is in it.
        const QRectF left(all.left() - 1, all.top() - 1, all.width() / 2 + 1, all.height() + 2);
        QVERIFY(doc->editNode(tag,
                              [&left](rf::Node& n) {
                                  n.props.insert(QStringLiteral("x0"), QStringLiteral("%1[um]").arg(left.left(), 0, 'g', 12));
                                  n.props.insert(QStringLiteral("y0"), QStringLiteral("%1[um]").arg(left.top(), 0, 'g', 12));
                                  n.props.insert(QStringLiteral("x1"), QStringLiteral("%1[um]").arg(left.right(), 0, 'g', 12));
                                  n.props.insert(QStringLiteral("y1"), QStringLiteral("%1[um]").arg(left.bottom(), 0, 'g', 12));
                              },
                              QStringLiteral("region")));
        QVERIFY(doc->waitForPrepared(60000));
        double halfArea = 0;
        for (const rf::Conductor& c : doc->processed()->conductors) {
            halfArea += c.area();
            QVERIFY2(c.outline.boundingRect().right() <= left.right() + 1e-3, qPrintable(QString::number(c.outline.boundingRect().right())));
        }
        QVERIFY2(halfArea > 0 && halfArea < wholeArea * 0.95, qPrintable(QStringLiteral("%1 of %2").arg(halfArea).arg(wholeArea)));
        QVERIFY(doc->mesh() != nullptr);
        for (const auto& cell : doc->mesh()->cells) QVERIFY(cell.outline.boundingRect().right() <= left.right() + 1e-3);
        // Drawn: its outline, with the processed shapes and the ports off.
        doc->setProcessedShown(false);
        doc->setPortsShown(false);
        bool drawn = false;
        for (const qucs_s::layout::LayoutView::Overlay& o : doc->view()->overlays())
            if (o.name.contains(QLatin1String("cutout's regions that keep"))) {
                drawn = true;
                QCOMPARE(o.shapes.size(), 1);
                QVERIFY(qAbs(o.shapes.first().boundingRect().width() - left.width()) < 1e-6);
            }
        QVERIFY(drawn);
        // What it did, under the Cutout.
        panel()->selectNode(QStringLiteral("cutout"));
        QTRY_VERIFY2(reportText().contains(QStringLiteral("masked")), qPrintable(reportText()));

        // Not applied: the whole layout again, and nothing drawn of it.
        QVERIFY(doc->editNode(QStringLiteral("cutout"), [](rf::Node& n) { n.props.insert(QStringLiteral("apply"), false); },
                              QStringLiteral("apply")));
        QVERIFY(doc->waitForPrepared(60000));
        QCOMPARE(doc->processed()->conductors.size(), whole->conductors.size());
        QTRY_COMPARE(itemOf(tree, QStringLiteral("cutout"))->text(1), QStringLiteral("not applied"));
        for (const qucs_s::layout::LayoutView::Overlay& o : doc->view()->overlays()) QVERIFY(!o.name.contains(QLatin1String("cutout")));
        doc->setProcessedShown(true);
        doc->setPortsShown(true);
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
                              [](rf::Node& n) { coarsely(n, "20[um]"); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));
        // (The ports, drawn over these, are thePortsAreDrawnByTheirNames'.)
        doc->setPortsShown(false);

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
        doc->setPortsShown(true);
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
                              [](rf::Node& n) { coarsely(n, "20[um]"); },
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
                              [](rf::Node& n) { coarsely(n, "30[um]"); },
                              QStringLiteral("coarser")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->extraction() == nullptr, "the capacitance of the mesh before was kept");
        QVERIFY(doc->lineSummary().isEmpty());
        QVERIFY(!doc->problemsText()->toPlainText().contains(QStringLiteral("What it solves to")));
    }

    // Two conductors are coupled lines, and the tab says so: their modes
    // and the 4-port, not each net as a line of its own - its own row, its
    // charge with both at a volt, is neither mode (the phase B check's
    // N4). And the starter's ground under them, so that they are lines.
    void aPairIsShownByItsModes()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("pair.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        // M1 and M2 with nothing joining them: the via out.
        QString via;
        for (const rf::Node& child : doc->model().stackNode().children)
            if (child.type == QLatin1String("rf_via")) via = child.tag;
        QVERIFY(!via.isEmpty());
        QVERIFY(panel()->removeNode(via));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")},
                    {QStringLiteral("11"), QStringLiteral("0"), QStringLiteral("M2")}});
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { coarsely(n, "30[um]"); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::Extraction> e = doc->extraction();
        QVERIFY2(e != nullptr && e->ok(), qPrintable(doc->solveProblem()));
        QCOMPARE(e->names.size(), 2);
        QVERIFY(e->grounded);
        const QStringList said = doc->lineSummary();
        QVERIFY2(!said.isEmpty() && said.first().contains(QStringLiteral("as coupled lines")), qPrintable(said.join(" | ")));
        QVERIFY2(said.first().contains(QStringLiteral(" Z ")) && said.first().contains(QStringLiteral("eps_eff")),
                 qPrintable(said.first()));
        for (const QString& line : said)
            QVERIFY2(!line.contains(QStringLiteral("Ohm over")), qPrintable(line));
        const QStringList sp = doc->sParameterSummary();
        QVERIFY2(!sp.isEmpty() && sp.first().contains(QStringLiteral("4-port into 50 Ohm"))
                     && sp.first().contains(QStringLiteral("near end")) && sp.first().contains(QStringLiteral("far")),
                 qPrintable(sp.join(" | ")));
        QVERIFY2(sp.join(QStringLiteral(" | ")).contains(QStringLiteral("conductors span"))
                     || sp.join(QStringLiteral(" | ")).contains(QStringLiteral("deep")),
                 qPrintable(sp.join(" | ")));
        // In the Results node's report, as the tab shows it.
        panel()->selectNode(doc->model().root.child(QStringLiteral("rf_results"))->tag);
        QVERIFY2(reportSays(QStringLiteral("as coupled lines")), qPrintable(reportText()));

        // Its Touchstone 2.0, a 4-port: every port into the export's own
        // reference, as [Reference] says, no two-port's data order, and the
        // same data as 1.1's, by rows.
        RF25Doc::ExportRequest asked;
        asked.coupled = true;
        asked.length = 1e-3;
        QString why;
        const QString one = doc->exportText(asked, &why);
        QVERIFY2(why.isEmpty() && !one.isEmpty(), qPrintable(why));
        asked.version2 = true;
        const QString two = doc->exportText(asked, &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QStringList keywords, rows, rows1;
        for (const QString& row : two.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            if (row.startsWith(QLatin1Char('[')) || row.startsWith(QLatin1Char('#'))) keywords << row;
            else if (!row.startsWith(QLatin1Char('!'))) rows << row;
        for (const QString& row : one.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            if (!row.startsWith(QLatin1Char('!')) && !row.startsWith(QLatin1Char('#'))) rows1 << row;
        const int frequencies = int(rows.size()) / 4;
        QCOMPARE(keywords, (QStringList{QStringLiteral("[Version] 2.0"), QStringLiteral("# Hz S RI R 50"),
                                        QStringLiteral("[Number of Ports] 4"),
                                        QStringLiteral("[Number of Frequencies] %1").arg(frequencies),
                                        QStringLiteral("[Reference] 50 50 50 50"), QStringLiteral("[Network Data]"),
                                        QStringLiteral("[End]")}));
        QVERIFY(frequencies > 0 && rows.size() == 4 * frequencies);
        QCOMPARE(rows, rows1);
    }

    // What the solve says of the model as a circuit: the S parameters of
    // each net, where the quasi-static answer gives out, and the
    // Touchstone the Exports node writes of them.
    // What is no uniform line - the heater's two pads - has no S parameters
    // as one: the tab says so and why, with where its capacitance stops
    // being the whole story, and its Touchstone is refused with the same
    // reason rather than written as a line of its length (the phase C check
    // of 1 November, F2).
    void whatIsNoLineHasNoSParameters()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("pads.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("12"), QStringLiteral("0"), QStringLiteral("M1")}});
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { coarsely(n, "20[um]"); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));
        rf::Node touchstone = doc->model().make(QStringLiteral("rf_touchstone"));
        const QString tag = touchstone.tag;
        {
            rf::Model m = doc->model();
            QVERIFY(m.add(QStringLiteral("exports"), touchstone) != nullptr);
            QVERIFY(doc->setModel(m, QStringLiteral("an export")));
        }
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::Extraction> e = doc->extraction();
        QVERIFY2(e != nullptr && e->ok(), qPrintable(doc->solveProblem()));
        QVERIFY(!e->perMetre.fromMiddle);
        const QStringList sp = doc->sParameterSummary();
        QVERIFY2(!sp.isEmpty() && sp.first().startsWith(QStringLiteral("No uniform line")), qPrintable(sp.join(" | ")));
        QVERIFY2(sp.join(' ').contains(QStringLiteral("is no line")), qPrintable(sp.join(" | ")));
        QString why;
        QVERIFY(doc->exportText(tag, &why).isEmpty());
        QVERIFY2(why.contains(QStringLiteral("no uniform line")), qPrintable(why));
        // Nor modes per metre, of its two pads as coupled lines.
        QCOMPARE(e->names.size(), 2);
        const qucs_s::fem::CoupledLines cl = rf::asCoupledLines(*e, doc->solvedLength(), 10e9);
        QVERIFY2(cl.error.contains(QStringLiteral("no uniform line")), qPrintable(cl.error));
    }

    void theSolvedModelIsWrittenAsATouchstone()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("written.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}});
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { coarsely(n, "20[um]"); },
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
        // Its comment's S parameters are into the data's own 50 Ohm, said,
        // with the line's own impedance beside it - not into the line's
        // own, as the tab's are, beside data that are not (the phase B
        // check's N1).
        QVERIFY2(written.contains(QStringLiteral("into 50 Ohm")) && written.contains(QStringLiteral("its own Zc is")),
                 qPrintable(written.left(900)));
        QVERIFY2(sp.first().contains(QStringLiteral("into its own")), qPrintable(sp.first()));
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
        // The form made again after the click - the solve's end, deferred,
        // came then on CI's slow Linux runner (d7facd8) - still says it.
        const QPointer<QLabel> before = panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("rfExportSaid"));
        emit doc->solvedChanged();
        write->click();
        QCoreApplication::processEvents();
        QVERIFY2(before.isNull(), "the form was not made again");
        auto* said = panel()->settingsArea()->findChild<QLabel*>(QStringLiteral("rfExportSaid"));
        QVERIFY(said != nullptr);
        QVERIFY2(said->text().contains(QStringLiteral("Written to")), qPrintable(said->text()));
        QVERIFY(said->isVisibleTo(panel()));
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

    // The loop's inductance, which wants two ports to say what the loop
    // is - and says so where there are not two.
    void theLoopsInductanceIsSolvedFromItsPorts()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("loop.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}});
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { coarsely(n, "20[um]"); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->editNode(QStringLiteral("m1"),
                              [](rf::Node& n) {
                                  n.props.insert(QStringLiteral("sigma"), QStringLiteral("5.8e7"));
                                  n.props.insert(QStringLiteral("thickness"), QStringLiteral("3[um]"));
                              },
                              QStringLiteral("copper")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY(doc->mesh() != nullptr && !doc->mesh()->cells.isEmpty());
        // Where the layout is, so the ports can be put on its two ends.
        QRectF bounds;
        for (const rf::Conductor& c : doc->processed()->conductors) bounds = bounds.united(c.outline.boundingRect());
        QVERIFY(bounds.width() > 0);
        // (This layout's layer 1 is a wire that runs in y - 0.5 µm wide
        //  and 1200 µm long - so its two ends are its top and bottom.)
        QVERIFY2(bounds.height() > bounds.width(), "the fixture is a wire running in y");

        // One port is no loop: the model made by written() has one, and
        // it says what is wanted rather than answering.
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->extraction() != nullptr && doc->extraction()->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(doc->inductance() == nullptr, "it found a loop with one port");
        QVERIFY2(doc->inductanceSummary().join(QLatin1Char(' ')).contains(QStringLiteral("wants two")),
                 qPrintable(doc->inductanceSummary().join(" | ")));

        // A second port at the other end of the layout, and there is a
        // loop: the current goes in at one and out at the other.
        {
            rf::Model m = doc->model();
            rf::Node* first = nullptr;
            for (rf::Node& p : m.ports().children)
                if (p.type == QLatin1String("rf_edge_port")) first = &p;
            QVERIFY(first != nullptr);
            first->props.insert(QStringLiteral("x"), QStringLiteral("%1[um]").arg(bounds.center().x()));
            first->props.insert(QStringLiteral("y"), QStringLiteral("%1[um]").arg(bounds.top()));
            rf::Node second = m.make(QStringLiteral("rf_edge_port"));
            second.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
            second.props.insert(QStringLiteral("x"), QStringLiteral("%1[um]").arg(bounds.center().x()));
            second.props.insert(QStringLiteral("y"), QStringLiteral("%1[um]").arg(bounds.bottom()));
            QVERIFY(m.add(QStringLiteral("ports"), second) != nullptr);
            QVERIFY(doc->setModel(m, QStringLiteral("a second port")));
        }
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        const std::shared_ptr<const rf::Inductance> loop = doc->inductance();
        QVERIFY2(loop != nullptr, qPrintable(doc->inductanceSummary().join(" | ")));
        QVERIFY2(loop->ok(), qPrintable(loop->problems.join(" | ")));
        // A wire of some hundreds of micrometres is some hundreds of
        // picohenries, and it has a resistance to go with it.
        QVERIFY2(loop->l > 1e-12 && loop->l < 1e-7, qPrintable(QStringLiteral("%1 nH").arg(loop->l * 1e9)));
        QVERIFY2(loop->rDc > 0, "copper with no resistance");
        QVERIFY2(loop->branches > 0 && loop->nodes > 0, "no runs of current");
        QCOMPARE(loop->current.size(), loop->branches);
        // Q at the study's own frequency, which a blank model puts at
        // 40 GHz, and it is wL/R.
        QVERIFY2(loop->q > 0, "no Q");
        QVERIFY2(std::abs(loop->q - 2 * M_PI * loop->frequency * loop->l / loop->rDc) < 1e-9 * loop->q, "Q is not wL/R");

        // In the notes, in the status line and under the Results node.
        const QStringList said = doc->inductanceSummary();
        QVERIFY2(said.join(QLatin1Char(' ')).contains(QStringLiteral("The loop is")), qPrintable(said.join(" | ")));
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral("The loop is")),
                 qPrintable(doc->problemsText()->toPlainText()));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("the loop is")), qPrintable(doc->statusLabel()->text()));
        panel()->selectNode(doc->model().root.child(QStringLiteral("rf_results"))->tag);
        QVERIFY2(reportSays(QStringLiteral("The loop is")), qPrintable(reportText()));

        // An edit throws it away with the capacitance, for the same
        // reason: a loop of shapes that have gone is worse than none.
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { coarsely(n, "30[um]"); },
                              QStringLiteral("coarser")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->inductance() == nullptr, "the loop of the mesh before was kept");
        QVERIFY(doc->inductanceSummary().isEmpty());
    }

    // A new model's stack is drawn from the first: only an edit or a read
    // drew it, so the view said "No layer" beside a status line counting
    // three (the phase A check of 24 October).
    void aNewModelsStackIsDrawnAtOnce()
    {
        RF25Doc* doc = app->newRf25Model();
        QVERIFY(doc != nullptr);
        QVERIFY(doc->stackView() != nullptr);
        const rf::Stack drawn = doc->stackView()->stack();
        const rf::Stack held = doc->model().stack();
        QVERIFY2(!held.layers.isEmpty(), "the starter model has no layers to draw");
        QCOMPARE(drawn.layers.size(), held.layers.size());
        for (int i = 0; i < held.layers.size(); ++i) QCOMPARE(drawn.layers.at(i).name, held.layers.at(i).name);
    }

    // The tree's two columns have names, for a reader of the window: it
    // was given Qt's own "1" and "2".
    void theTreesColumnsAreNamed()
    {
        RF25Doc* doc = app->newRf25Model();
        QVERIFY(doc != nullptr);
        QTreeWidget* tree = panel()->tree();
        QVERIFY(tree != nullptr);
        QCOMPARE(tree->columnCount(), 2);
        QCOMPARE(tree->headerItem()->text(0), QStringLiteral("Node"));
        QCOMPARE(tree->headerItem()->text(1), QStringLiteral("Status"));
        // Still out of sight, as it was meant to be.
        QVERIFY(tree->isHeaderHidden());
    }

    // An untitled model's Save starts in the workspace, as the open
    // dialogs do, and not in the home folder - with no project open and
    // nothing opened or saved yet.
    void anUntitledModelIsSavedIntoTheWorkspace()
    {
        app->closeAllFiles();
        QVERIFY2(app->ProjName.isEmpty(), "a project is open in this fixture");
        const QString lastBefore = lastDirOpenSave;
        lastDirOpenSave.clear();
        RF25Doc* doc = app->newRf25Model();
        QVERIFY(doc != nullptr);
        QVERIFY(doc->getDocName().isEmpty());
        // (A modal dialog runs a loop of its own: what answers it is set
        //  going before the call that opens it, and runs in that loop.)
        const bool nativeBefore = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        QString startedIn;
        bool seen = false;
        QTimer::singleShot(400, this, [&] {
            if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                seen = true;
                startedIn = dialog->directory().absolutePath();
                dialog->reject();
            }
        });
        const bool saved = app->saveAs();
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeBefore);
        lastDirOpenSave = lastBefore;
        QVERIFY2(seen, "no file dialog came up");
        QVERIFY(!saved);
        const QString workspace = QFileInfo(QucsSettings.qucsWorkspaceDir.absolutePath()).canonicalFilePath();
        QVERIFY2(QFileInfo(startedIn).canonicalFilePath() == workspace,
                 qPrintable(QStringLiteral("it started in %1, not the workspace %2").arg(startedIn, workspace)));
        QVERIFY2(QFileInfo(startedIn).canonicalFilePath() != QFileInfo(QDir::homePath()).canonicalFilePath(),
                 "it started in the home folder");
    }

    // A file read again is shown in the panel's form at once: a tool call
    // reads a changed file before it answers, and the form was made again
    // only on the next turn of the event loop - so the first get_ui after
    // a file was mended still listed what it had mended.
    void aFileReadAgainIsShownInTheFormAtOnce()
    {
        rf::Model broken = rf::Model::blank();
        for (rf::Node& c : broken.stackNode().children)
            if (c.type == QLatin1String("rf_metal")) c.props.insert(QStringLiteral("thickness"), QStringLiteral("nonsense"));
        const QString file = path(QStringLiteral("mended.qrf25"));
        const auto put = [&file](const QByteArray& json) {
            QFile f(file);
            return f.open(QIODevice::WriteOnly) && f.write(json) == json.size();
        };
        QVERIFY(put(broken.toJson()));
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr);
        panel()->selectNode(doc->model().root.tag);
        QCoreApplication::processEvents();
        const auto listed = [this] {
            const auto labels = panel()->settingsArea()->findChildren<QLabel*>();
            for (QLabel* l : labels)
                if (l->text().contains(QStringLiteral("is not a number"))) return true;
            return false;
        };
        QVERIFY2(listed(), "the form does not list the bad thickness to begin with");
        // Mended on disk and read again - and the form looked at before the
        // event loop has turned, as a tool call that read it first does.
        QVERIFY(put(rf::Model::blank().toJson()));
        QVERIFY(doc->reload());
        QVERIFY2(!listed(), "the form still lists what the file no longer says");
        // An edit is still left to the next turn: it comes from one of the
        // form's own fields, which must not be destroyed in its own signal.
        QVERIFY(doc->lastChangeWasRead());
        QVERIFY(doc->editNode(QStringLiteral("m1"),
                              [](rf::Node& n) { n.props.insert(QStringLiteral("thickness"), QStringLiteral("1[um]")); },
                              QStringLiteral("an edit")));
        QVERIFY(!doc->lastChangeWasRead());
    }

    // What a solve will not do: run with no mesh under it, and run twice
    // at once.
    // A Solve straight after an edit is of the edit: it waits for the
    // shapes and the mesh to be made of it and starts when they are.
    // Started at once on the mesh before, it was thrown away when they
    // were done and nothing said why - under the sanitizers' slowness the
    // Touchstone test's solve, after its export node was added, came to
    // nothing every time (CI's Linux job, b62b55d).
    void aSolveStraightAfterAnEditIsOfTheEdit()
    {
        layoutCopy();
        RF25Doc* doc = open(written(QStringLiteral("straight.qrf25")));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        mapTo(doc, {{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}});
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { coarsely(n, "30[um]"); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));
        const int coarse = int(doc->mesh()->cells.size());
        // The edit, and Solve before its 300 ms are up.
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { coarsely(n, "15[um]"); },
                              QStringLiteral("finer")));
        doc->solve();
        QVERIFY2(doc->isSolving(), "a solve after an edit was not waiting for it");
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral("once the shapes and the mesh")),
                 qPrintable(doc->problemsText()->toPlainText()));
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::Extraction> e = doc->extraction();
        QVERIFY2(e != nullptr && e->ok(), qPrintable(doc->solveProblem()));
        // Of the edit's mesh, not the one before it.
        QVERIFY(doc->mesh() != nullptr);
        QVERIFY2(int(doc->mesh()->cells.size()) > coarse, "the finer mesh is not finer");
        QCOMPARE(e->unknowns, int(doc->mesh()->cells.size()));
        // A solve going on when an edit's shapes and mesh are made is of
        // what is gone: it starts again on them, rather than coming to
        // nothing. (Held, so that it is still going on when they are made
        // in any build. It was a mesh fine enough to be caught at it, whose
        // solve is a second here and minutes under the sanitizers: CI's
        // Linux job ran past the wait at 3ae9608.)
        const int finer = int(doc->mesh()->cells.size());
        doc->holdSolves(true);
        doc->solve();
        QVERIFY(doc->isSolving());
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { coarsely(n, "20[um]"); },
                              QStringLiteral("a little coarser")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(int(doc->mesh()->cells.size()) != finer, "the edit's mesh is the one before");
        QVERIFY2(doc->isSolving(), "a solve overtaken by an edit's mesh was let go");
        doc->holdSolves(false);
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::Extraction> again = doc->extraction();
        QVERIFY2(again != nullptr && again->ok(), qPrintable(doc->solveProblem()));
        QCOMPARE(again->unknowns, int(doc->mesh()->cells.size()));
        // A solve going on is let go by a Stop, and nothing of it arrives.
        doc->holdSolves(true);
        doc->solve();
        QVERIFY(doc->isSolving());
        doc->stopSolving();
        QVERIFY(!doc->isSolving());
        doc->holdSolves(false);
        QTest::qWait(100);
        QVERIFY2(doc->extraction() == again, "a stopped solve's answer arrived");
        // A solve waiting for the mesh is stopped as one going on is.
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { coarsely(n, "30[um]"); },
                              QStringLiteral("coarser")));
        doc->solve();
        QVERIFY(doc->isSolving());
        doc->stopSolving();
        QVERIFY(!doc->isSolving());
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(!doc->isSolving() && doc->extraction() == nullptr, "a stopped solve went on");
    }

    /// A coplanar line in the workspace as \a name - its signal 50 µm, its
    /// gaps 6, its grounds 150, 1000 µm long - on a stack that takes its
    /// time: a mirror of forty films of 0.1 and 0.2 µm, εr 28 and 3.9, on
    /// 500 µm of silicon, nothing under it; its metal solved as its two
    /// faces, four cells across and none over 25 µm.
    QString slowLine(const QString& name)
    {
        if (!writeRectangles(path(name + QStringLiteral(".gds")),
                             {QRectF(QPointF(0, -25), QPointF(1000, 25)), QRectF(QPointF(0, 31), QPointF(1000, 181)),
                              QRectF(QPointF(0, -181), QPointF(1000, -31))}))
            return {};
        rf::Model m = rf::Model::blank();
        std::vector<rf::Node>& stack = m.stackNode().children;
        for (rf::Node& n : stack) {
            if (n.type == QLatin1String("rf_ground")) n.props.insert(QStringLiteral("kind"), QStringLiteral("open"));
            if (n.type == QLatin1String("rf_substrate")) {
                n.props.insert(QStringLiteral("thickness"), QStringLiteral("500[um]"));
                n.props.insert(QStringLiteral("epsr"), QStringLiteral("11.7"));
            }
            if (n.type == QLatin1String("rf_metal")) {
                n.props.insert(QStringLiteral("thickness"), QStringLiteral("1[um]"));
                n.props.insert(QStringLiteral("model"), QStringLiteral("faces"));
            }
        }
        stack.erase(std::remove_if(stack.begin(), stack.end(), [](const rf::Node& n) { return n.type == QLatin1String("rf_dielectric"); }),
                    stack.end());
        for (int i = 0; i < 40; ++i) {
            rf::Node film;
            film.type = QStringLiteral("rf_dielectric");
            film.tag = QStringLiteral("film%1").arg(i + 1);
            film.label = QStringLiteral("F%1").arg(i + 1);
            film.props.insert(QStringLiteral("thickness"), i % 2 == 0 ? QStringLiteral("0.1[um]") : QStringLiteral("0.2[um]"));
            film.props.insert(QStringLiteral("epsr"), i % 2 == 0 ? QStringLiteral("28") : QStringLiteral("3.9"));
            // (Lossless: each lossy layer is its own Green's function more,
            //  a second and a half each of a mirror's.)
            film.props.insert(QStringLiteral("tand"), QStringLiteral("0"));
            stack.insert(stack.end() - 1, film);
        }
        m.layout().props.insert(QStringLiteral("file"), name + QStringLiteral(".gds"));
        m.layout().child(QStringLiteral("rf_layer_map"))
            ->props.insert(QStringLiteral("rows"), rowsOf({{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}}));
        m.mesh().props.insert(QStringLiteral("cells_across"), 4);
        m.mesh().props.insert(QStringLiteral("maximum"), QStringLiteral("25[um]"));
        m.normalise();
        const QString file = path(name + QStringLiteral(".qrf25"));
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(m.toJson());
        return file;
    }

    // A quasi-static solve says what it is doing, and a Stop stops it. It
    // said "Solving…" and nothing more, and its Stop let it go to run on to
    // its end: a coplanar line on thin-film lithium niobate, whose stack's
    // Green's function did not end, held a core and gigabytes until Qucs-S
    // was killed, and a second solve ran beside it (the review of 3
    // November). And the status line counts the charges the solve has: a
    // metal solved as its two faces has each cell twice (it said 924 to
    // solve the charge on, where the solve had 1848).
    void aQuasiStaticSolveSaysWhatItDoesAndStops()
    {
        const QString model = slowLine(QStringLiteral("slowline"));
        QVERIFY(!model.isEmpty());
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->mesh() != nullptr && doc->mesh()->ok(), qPrintable(doc->problemsText()->toPlainText()));
        const int cells = int(doc->mesh()->cells.size());
        QCOMPARE(doc->mesh()->charges, 2 * cells);
        // A quasi-static study's mesh graded at the line's narrow gaps, where
        // its capacitance is (test_rf25_mesh, test_rf25_static).
        QVERIFY2(doc->mesh()->lines.join(' ').contains(QStringLiteral("face another conductor across a gap")),
                 qPrintable(doc->mesh()->lines.join(" | ")));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("%1 cell(s), %2").arg(cells).arg(rf::chargeCostOf(2 * cells))),
                 qPrintable(doc->statusLabel()->text()));
        const int begun = doc->solvesBegun();
        doc->solve();
        QVERIFY(doc->isSolving());
        QCOMPARE(doc->solvesBegun(), begun + 1);
        QVERIFY2(QTest::qWaitFor([doc] { return doc->quasiStaticStep() >= rf::ExtractControl::Greens; }, 60000), "it never began");
        QVERIFY2(doc->solvingSaid().contains(QStringLiteral("building the stack's Green's functions")), qPrintable(doc->solvingSaid()));
        QVERIFY2(QTest::qWaitFor([doc] { return doc->statusLabel()->text().contains(QStringLiteral("Green's functions")); }, 5000),
                 qPrintable(doc->statusLabel()->text()));
        QVERIFY2(QTest::qWaitFor([doc] {
                     return doc->problemsText()->toPlainText().contains(
                         QStringLiteral("Solving quasi-statically: building the stack's Green's functions"));
                 }, 5000),
                 qPrintable(doc->problemsText()->toPlainText()));
        QCOMPARE(doc->solvesAtWork(), 1);
        // Stopped as its stack's Green's functions are built: its thread
        // ends then, not when the solve would have.
        QElapsedTimer t;
        t.start();
        doc->stopSolving();
        QVERIFY(!doc->isSolving());
        QVERIFY2(QTest::qWaitFor([doc] { return doc->solvesAtWork() == 0; }, slowBuild() ? 60000 : 5000),
                 qPrintable(QStringLiteral("still at work %1 ms after the Stop").arg(t.elapsed())));
        qWarning("a quasi-static solve stopped %lld ms after its Stop", (long long)t.elapsed());
        QVERIFY(doc->extraction() == nullptr);
        QVERIFY(doc->solvingSaid().isEmpty());
        // A full-wave study's so too, where the current crowds as the charge
        // does (test_rf25_measured): the same cells.
        QVERIFY(doc->editNode(doc->model().root.child(QStringLiteral("rf_study"))->tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("kind"), QStringLiteral("full_wave")); },
                              QStringLiteral("full wave")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->mesh()->lines.join(' ').contains(QStringLiteral("face another conductor across a gap")),
                 qPrintable(doc->mesh()->lines.join(" | ")));
        QCOMPARE(int(doc->mesh()->cells.size()), cells);
    }

    /// A microstrip to solve full-wave, in the workspace as \a name: 1000 by
    /// 50 µm on 100 µm of 9.8 over a ground, an edge port at each end, and a
    /// study of three frequencies from 1 to 5 GHz by the full-wave solver.
    QString fullWaveLine(const QString& name)
    {
        if (!writeStrip(path(QStringLiteral("strip.gds")), 1000, 50)) return {};
        rf::Model m = rf::Model::blank();
        std::vector<rf::Node>& stack = m.stackNode().children;
        stack.erase(std::remove_if(stack.begin(), stack.end(),
                                   [](const rf::Node& n) { return n.type == QLatin1String("rf_dielectric"); }),
                    stack.end());
        for (rf::Node& n : stack)
            if (n.type == QLatin1String("rf_substrate")) {
                n.props.insert(QStringLiteral("thickness"), QStringLiteral("100[um]"));
                n.props.insert(QStringLiteral("epsr"), QStringLiteral("9.8"));
            }
        m.layout().props.insert(QStringLiteral("file"), QStringLiteral("strip.gds"));
        m.layout().child(QStringLiteral("rf_layer_map"))
            ->props.insert(QStringLiteral("rows"), rowsOf({{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}}));
        m.mesh().props.insert(QStringLiteral("cells_across"), 3);
        m.mesh().props.insert(QStringLiteral("maximum"), QStringLiteral("100[um]"));
        for (const QPair<const char*, const char*>& at : {qMakePair("0", "P1"), qMakePair("1000[um]", "P2")}) {
            rf::Node port = m.make(QStringLiteral("rf_edge_port"));
            port.label = QLatin1String(at.second);
            port.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
            port.props.insert(QStringLiteral("x"), QLatin1String(at.first));
            port.props.insert(QStringLiteral("y"), QStringLiteral("25[um]"));
            if (m.add(QStringLiteral("ports"), port) == nullptr) return {};
        }
        rf::Node* study = m.root.child(QStringLiteral("rf_study"));
        if (study == nullptr) return {};
        study->props.insert(QStringLiteral("kind"), QStringLiteral("full_wave"));
        study->props.insert(QStringLiteral("from"), QStringLiteral("1[GHz]"));
        study->props.insert(QStringLiteral("to"), QStringLiteral("5[GHz]"));
        study->props.insert(QStringLiteral("points"), 3);
        if (m.add(QStringLiteral("exports"), m.make(QStringLiteral("rf_touchstone"))) == nullptr) return {};
        const QString file = path(name);
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(m.toJson());
        f.close();
        return file;
    }

    // A line shorted to the ground at its far end by a via, the via a layer
    // of the GDS mapped to a via of the stack from "ground" to M1: solved
    // full-wave through the tab, a short - an inductance at a low frequency
    // - with the via said, and drawn by its current round its sides over
    // the metal it stands on. The quasi-static extraction says it cannot
    // hold a conductor at the ground's potential, and names the full wave.
    void aViaToTheGroundIsSolvedFullWaveAndDrawn()
    {
        QVERIFY(writeStripWithVia(path(QStringLiteral("shorted.gds")), 1000, 50, 50));
        rf::Model m = rf::Model::blank();
        std::vector<rf::Node>& stack = m.stackNode().children;
        stack.erase(std::remove_if(stack.begin(), stack.end(),
                                   [](const rf::Node& n) { return n.type == QLatin1String("rf_dielectric"); }),
                    stack.end());
        for (rf::Node& n : stack)
            if (n.type == QLatin1String("rf_substrate")) {
                n.props.insert(QStringLiteral("thickness"), QStringLiteral("100[um]"));
                n.props.insert(QStringLiteral("epsr"), QStringLiteral("9.8"));
            }
        rf::Node via = m.make(QStringLiteral("rf_via"));
        via.label = QStringLiteral("V0");
        via.props.insert(QStringLiteral("from"), QStringLiteral("ground"));
        via.props.insert(QStringLiteral("to"), QStringLiteral("M1"));
        via.props.insert(QStringLiteral("sigma"), QStringLiteral("5.8e7"));
        QVERIFY(m.add(QStringLiteral("stack"), via) != nullptr);
        QVERIFY2(m.stack().ok(), qPrintable(m.stack().problems.join(" | ")));
        m.layout().props.insert(QStringLiteral("file"), QStringLiteral("shorted.gds"));
        QJsonArray rows;
        rows.append(row({QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}));
        rows.append(row({QStringLiteral("2"), QStringLiteral("0"), QStringLiteral("V0")}));
        m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), rows);
        m.mesh().props.insert(QStringLiteral("cells_across"), 2);
        m.mesh().props.insert(QStringLiteral("maximum"), QStringLiteral("100[um]"));
        rf::Node port = m.make(QStringLiteral("rf_edge_port"));
        port.label = QStringLiteral("P1");
        port.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
        port.props.insert(QStringLiteral("x"), QStringLiteral("0"));
        port.props.insert(QStringLiteral("y"), QStringLiteral("25[um]"));
        QVERIFY(m.add(QStringLiteral("ports"), port) != nullptr);
        rf::Node* study = m.root.child(QStringLiteral("rf_study"));
        QVERIFY(study != nullptr);
        study->props.insert(QStringLiteral("kind"), QStringLiteral("full_wave"));
        study->props.insert(QStringLiteral("from"), QStringLiteral("0.1[GHz]"));
        study->props.insert(QStringLiteral("to"), QStringLiteral("2[GHz]"));
        study->props.insert(QStringLiteral("points"), 2);
        const QString file = path(QStringLiteral("shorted.qrf25"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(m.toJson());
        }
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->mesh() != nullptr && !doc->mesh()->cells.isEmpty(), qPrintable(doc->layoutProblem()));
        int viaCells = 0;
        for (const rf::MeshCell& c : doc->mesh()->cells) viaCells += c.layer == QLatin1String("V0") ? 1 : 0;
        QVERIFY2(viaCells > 0, "the via is meshed");

        doc->solveButton()->click();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(says(w->lines, "of the runs are vias") && says(w->lines, "to the ground"), qPrintable(w->lines.join(" | ")));
        // A short: at 100 MHz nearly all reflected, and inductive - the
        // millimetre of line and the via, half a nanohenry, with an ohm of
        // the thin metal's loss.
        const std::complex<double> z = w->z.at(0).at(0).at(0);
        QVERIFY2(std::abs(w->s.at(0).at(0).at(0)) > 0.95 && z.imag() > 0 && std::abs(z) < 5,
                 qPrintable(QStringLiteral("Z %1 + %2 j").arg(z.real()).arg(z.imag())));
        const QStringList summary = doc->fullWaveSummary();
        QVERIFY2(says(summary, "Passive"), qPrintable(summary.join(" | ")));

        doc->setCurrentsShown(true);
        int round = 0;
        for (const qucs_s::layout::LayoutView::Overlay& o : doc->view()->overlays())
            if (o.fill.isValid() && o.name.contains(QStringLiteral("round a via"))) round += int(o.shapes.size());
        QCOMPARE(round, viaCells);
        // (Drawn after the metals', over them.)
        const QList<qucs_s::layout::LayoutView::Overlay> overlays = doc->view()->overlays();
        int lastMetal = -1, firstVia = -1;
        for (int i = 0; i < overlays.size(); ++i) {
            if (overlays.at(i).name.contains(QStringLiteral("round a via"))) {
                if (firstVia < 0) firstVia = i;
            } else if (overlays.at(i).fill.isValid()) {
                lastMetal = i;
            }
        }
        QVERIFY2(firstVia > lastMetal, qPrintable(QStringLiteral("metal to %1, via from %2").arg(lastMetal).arg(firstVia)));
        QVERIFY2(doc->currentsSaid().contains(QStringLiteral("round its sides")), qPrintable(doc->currentsSaid()));

        // Quasi-static the via holds the strip at the ground's potential: a
        // return, solved, and no line of its own (the GSSG review of 1
        // November - once refused, "solve it full-wave").
        RF25Doc::SolveRequest quasi;
        quasi.solver = QStringLiteral("quasi_static");
        doc->solveWith(quasi);
        QVERIFY(doc->waitForSolved(300000));
        const std::shared_ptr<const rf::Extraction> e = doc->extraction();
        QVERIFY2(e != nullptr && e->ok(), qPrintable(doc->solveProblem()));
        QCOMPARE(e->returns, QList<bool>{true});
        QVERIFY2(e->returnsSaid.contains(QStringLiteral("joined to the ground plane by a via")), qPrintable(e->returnsSaid));
        app->closeAllFiles();
    }

    // A study by the full-wave solver: Solve drives the ports at each of its
    // frequencies, and the tab, the panel and the export give the ports' S;
    // the currents are drawn on the layout; a stop stops it; an edit throws
    // it away; and a solve asked for by the other solver is that solver's.
    // The phase C check of 1 November, F6, through the tab: an edge port's
    // Return read - a coplanar line's three strips driven from its signal
    // against the grounds beside it. One that asks has its own
    // discontinuity taken out by strips of its coplanar feed, grounds and
    // all (the recheck of the same afternoon: they were not solved, and it
    // kept it). And a lone strip has nothing beside it to return through,
    // which is said rather than solved.
    // A press, a few moves and a release of the left button in \a view, at
    // its pixels: a drag, or where \a from and \a to are one, a click.
    static void drag(QWidget* view, QPointF from, QPointF to, Qt::KeyboardModifiers keys = Qt::NoModifier)
    {
        const auto send = [view, keys](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons held) {
            QMouseEvent e(type, at, view->mapToGlobal(at), button, held, keys);
            QCoreApplication::sendEvent(view, &e);
        };
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 5; ++i) send(QEvent::MouseMove, from + (to - from) * (i / 5.0), Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    }
    /// The micrometres of a length the tab wrote ("120.4[um]").
    static double micrometres(const rf::Node& n, const char* key)
    {
        QString text = n.props.value(QLatin1String(key)).toString();
        text.remove(QStringLiteral("[um]"));
        return text.toDouble();
    }

    // The ports drawn over the layout by their names (the user's request of
    // 1 November, "so that it's easy to understand them"): an edge port's
    // face - the whole edge it drives, as the solve finds it - with an
    // arrow out of it; a coplanar one's grounds beside it; an internal
    // port's rectangle. The Ports button hides them.
    void thePortsAreDrawnByTheirNames()
    {
        QVERIFY(writeRectangles(path(QStringLiteral("cpwports.gds")),
                                {QRectF(QPointF(0, -50), QPointF(1000, 50)), QRectF(QPointF(0, 100), QPointF(1000, 350)),
                                 QRectF(QPointF(0, -350), QPointF(1000, -100))}));
        const QString model = fullWaveLine(QStringLiteral("cpwports.qrf25"));
        QVERIFY(!model.isEmpty());
        {
            QFile f(model);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString why;
            std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
            QVERIFY2(read.has_value(), qPrintable(why));
            rf::Model m = *read;
            f.close();
            m.layout().props.insert(QStringLiteral("file"), QStringLiteral("cpwports.gds"));
            // Each at a corner of the signal, where two edges meet and its
            // facing says which it drives: its whole end.
            for (rf::Node& port : m.ports().children) {
                const bool first = port.label == QLatin1String("P1");
                port.props.insert(QStringLiteral("y"), first ? QStringLiteral("-50[um]") : QStringLiteral("50[um]"));
                port.props.insert(QStringLiteral("facing"), first ? QStringLiteral("-x") : QStringLiteral("+x"));
                if (first) port.props.insert(QStringLiteral("return"), QStringLiteral("beside"));
            }
            rf::Node inner = m.make(QStringLiteral("rf_internal_port"));
            inner.props.insert(QStringLiteral("x"), QStringLiteral("500[um]"));
            inner.props.insert(QStringLiteral("y"), QStringLiteral("0"));
            inner.props.insert(QStringLiteral("to"), QStringLiteral("M1"));
            inner.props.insert(QStringLiteral("width"), QStringLiteral("120[um]"));
            inner.props.insert(QStringLiteral("height"), QStringLiteral("60[um]"));
            QVERIFY(m.add(QStringLiteral("ports"), inner) != nullptr);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(m.toJson());
        }
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        const auto overlay = [doc](const QString& name) -> const LayoutView::Overlay* {
            for (const LayoutView::Overlay& o : doc->view()->overlays())
                if (o.name == name) return &o;
            return nullptr;
        };
        QVERIFY(doc->portsShown() && doc->portsButton()->isChecked());
        const LayoutView::Overlay* ports = overlay(QStringLiteral("the ports"));
        QVERIFY(ports != nullptr);
        QCOMPARE(ports->labels.size(), 3);
        QMap<QString, LayoutView::Overlay::Label> byName;
        for (const LayoutView::Overlay::Label& l : ports->labels) byName.insert(l.text, l);
        QCOMPARE(byName.value(QStringLiteral("P1")).at, QPointF(0, 0));
        QCOMPARE(byName.value(QStringLiteral("P1")).towards, QPointF(-1, 0));
        QCOMPARE(byName.value(QStringLiteral("P2")).at, QPointF(1000, 0));
        QCOMPARE(byName.value(QStringLiteral("P2")).towards, QPointF(1, 0));
        QCOMPARE(byName.value(QStringLiteral("P3")).at, QPointF(500, 0));
        QCOMPARE(byName.value(QStringLiteral("P3")).towards, QPointF(0, 0));
        // Each edge port's face is the signal's whole end.
        QCOMPARE(ports->shapes.size(), 2);
        for (const QPolygonF& face : ports->shapes) {
            const QRectF b = face.boundingRect();
            QVERIFY2((b.left() == 0 || b.left() == 1000) && b.width() == 0 && b.top() == -50 && b.bottom() == 50,
                     qPrintable(QStringLiteral("%1, %2 to %3, %4").arg(b.left()).arg(b.top()).arg(b.right()).arg(b.bottom())));
        }
        // The coplanar one's grounds beside it, on its edge's line; the
        // other returns through the ground, and has none drawn.
        const LayoutView::Overlay* grounds = overlay(QStringLiteral("the ports' grounds beside them"));
        QVERIFY(grounds != nullptr);
        QCOMPARE(grounds->shapes.size(), 2);
        QList<QRectF> beside;
        for (const QPolygonF& g : grounds->shapes) beside << g.boundingRect();
        std::sort(beside.begin(), beside.end(), [](const QRectF& a, const QRectF& b) { return a.top() < b.top(); });
        QCOMPARE(beside.at(0), QRectF(QPointF(0, -350), QPointF(0, -100)));
        QCOMPARE(beside.at(1), QRectF(QPointF(0, 100), QPointF(0, 350)));
        const LayoutView::Overlay* regions = overlay(QStringLiteral("the internal ports' rectangles"));
        QVERIFY(regions != nullptr);
        QCOMPARE(regions->shapes.size(), 1);
        QCOMPARE(regions->shapes.first().boundingRect(), QRectF(440, -30, 120, 60));
        // And drawn, not only listed: the faces' bars and the grounds' are in
        // the ports' colour where they lie - a line along x or y has a box of
        // no width, and the view once took it for out of sight.
        {
            const QColor ink = ports->line;
            doc->view()->showRegion(QRectF(QPointF(-200, -400), QPointF(1200, 400)));
            const QImage frame = doc->view()->grab().toImage();
            const double dpr = frame.devicePixelRatio();
            const auto inkAlong = [doc, &frame, &ink, dpr](QPointF from, QPointF to) {
                int hits = 0;
                for (int i = 1; i < 20; ++i) {
                    const QPointF at = doc->view()->toPixel(from + (to - from) * (i / 20.0)) * dpr;
                    for (int dx = -2; dx <= 2; ++dx) {
                        const QColor c = frame.pixelColor(std::clamp(int(at.x()) + dx, 0, frame.width() - 1),
                                                          std::clamp(int(at.y()), 0, frame.height() - 1));
                        if (std::abs(c.red() - ink.red()) < 40 && std::abs(c.green() - ink.green()) < 40
                            && std::abs(c.blue() - ink.blue()) < 40) {
                            ++hits;
                            break;
                        }
                    }
                }
                return hits;
            };
            QVERIFY2(inkAlong(QPointF(1000, -45), QPointF(1000, 45)) > 15, "P2's face is not drawn");
            QVERIFY2(inkAlong(QPointF(0, 110), QPointF(0, 340)) > 15, "P1's ground beside it is not drawn");
        }
        doc->portsButton()->click();
        QVERIFY(!doc->portsShown());
        QVERIFY(overlay(QStringLiteral("the ports")) == nullptr);
        doc->portsButton()->click();
        QVERIFY(overlay(QStringLiteral("the ports")) != nullptr);
    }

    // A coplanar port's grounds drawn where it returns (the GSSG review of
    // 1 November, G5): on a GSSG whose grounds are marked as Return
    // Conductors - named G1, G2 as they are put down, and drawn by their
    // names - S1's port's grounds are the two grounds' edges, past S2, as
    // the solve has them; not the nearest conductor either side. And the
    // Touchstone node's Modes read into the export.
    void aCoplanarPortsGroundsAreDrawnAsItsReturns()
    {
        QVERIFY(writeRectangles(path(QStringLiteral("gssgports.gds")),
                                {QRectF(QPointF(0, 25), QPointF(1000, 75)), QRectF(QPointF(0, -75), QPointF(1000, -25)),
                                 QRectF(QPointF(0, 110), QPointF(1000, 410)), QRectF(QPointF(0, -410), QPointF(1000, -110))}));
        const QString model = fullWaveLine(QStringLiteral("gssgports.qrf25"));
        QVERIFY(!model.isEmpty());
        {
            QFile f(model);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString why;
            std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
            QVERIFY2(read.has_value(), qPrintable(why));
            rf::Model m = *read;
            f.close();
            m.layout().props.insert(QStringLiteral("file"), QStringLiteral("gssgports.gds"));
            for (rf::Node& port : m.ports().children) {
                const bool first = port.label == QLatin1String("P1");
                port.props.insert(QStringLiteral("y"), QStringLiteral("50[um]"));
                port.props.insert(QStringLiteral("facing"), first ? QStringLiteral("-x") : QStringLiteral("+x"));
                if (first) port.props.insert(QStringLiteral("return"), QStringLiteral("beside"));
            }
            for (const char* y : {"260[um]", "-260[um]"}) {
                rf::Node mark = m.make(QStringLiteral("rf_return"));
                QCOMPARE(mark.label, QString::fromLatin1(y[0] == '-' ? "G2" : "G1"));
                mark.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
                mark.props.insert(QStringLiteral("x"), QStringLiteral("500[um]"));
                mark.props.insert(QStringLiteral("y"), QLatin1String(y));
                QVERIFY(m.add(QStringLiteral("ports"), mark) != nullptr);
            }
            m.root.child(QStringLiteral("rf_exports"))->children.front().props.insert(QStringLiteral("modes"), QStringLiteral("mixed"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(m.toJson());
        }
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        const auto overlay = [doc](const QString& name) -> const LayoutView::Overlay* {
            for (const LayoutView::Overlay& o : doc->view()->overlays())
                if (o.name == name) return &o;
            return nullptr;
        };
        const LayoutView::Overlay* grounds = overlay(QStringLiteral("the ports' grounds beside them"));
        QVERIFY(grounds != nullptr);
        QList<QRectF> beside;
        for (const QPolygonF& g : grounds->shapes) beside << g.boundingRect();
        std::sort(beside.begin(), beside.end(), [](const QRectF& a, const QRectF& b) { return a.top() < b.top(); });
        QCOMPARE(beside, (QList<QRectF>{QRectF(QPointF(0, -410), QPointF(0, -110)), QRectF(QPointF(0, 110), QPointF(0, 410))}));
        const LayoutView::Overlay* returns = overlay(QStringLiteral("the return conductors"));
        QVERIFY(returns != nullptr);
        QStringList names;
        for (const LayoutView::Overlay::Label& l : returns->labels) names << l.text;
        QCOMPARE(names, (QStringList{"G1", "G2"}));
        QCOMPARE(returns->labels.first().at, QPointF(500, 260));
        // The export node's Modes.
        RF25Doc::ExportRequest what;
        QVERIFY(doc->exportRequest(doc->model().root.child(QStringLiteral("rf_exports"))->children.front().tag, &what));
        QVERIFY(what.mixed);
        // And the solve has the marks: S1's port returns through the grounds
        // past S2 - over the ground plane nothing else makes them returns.
        RF25Doc::SolveRequest one;
        one.frequencies = {2e9};
        doc->solveWith(one);
        QVERIFY(doc->waitForSolved(300000));
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(w->portsSaid.value(0).contains(QStringLiteral("which is no return")), qPrintable(w->portsSaid.join(" | ")));
    }

    // A port placed by hand on the layout (the user's request of 1
    // November, "similar to how EMX handles ports"): with the Edge Port
    // tool a click on an edge of a metal's shape puts an edge port on that
    // edge, at its middle, facing out of it, the panel showing it and an
    // undo taking it away - not a second on the same edge, nor one where
    // no edge is near. With the Internal Port tool a rectangle dragged over
    // a metal puts an internal port over it, a click a point one. Escape
    // puts the tool down. And the solve drives the rectangle's cells.
    void aPortIsPlacedByHandOnTheLayout()
    {
        const QString model = fullWaveLine(QStringLiteral("handports.qrf25"));
        QVERIFY(!model.isEmpty());
        {
            QFile f(model);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString why;
            std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
            QVERIFY2(read.has_value(), qPrintable(why));
            rf::Model m = *read;
            f.close();
            m.ports().children.clear();
            rf::Node* study = m.root.child(QStringLiteral("rf_study"));
            QVERIFY(study != nullptr);
            study->props.insert(QStringLiteral("to"), QStringLiteral("1[GHz]"));
            study->props.insert(QStringLiteral("points"), 1);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(m.toJson());
        }
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        LayoutView* view = doc->view();
        view->showRegion(QRectF(QPointF(-100, -150), QPointF(1100, 200)));
        view->repaint();
        const auto portsOfModel = [doc] { return doc->model().ports().children; };
        QCOMPARE(int(portsOfModel().size()), 0);

        doc->edgePortButton()->click();
        QCOMPARE(doc->placing(), RF25Doc::Placing::EdgePort);
        QCOMPARE(view->mode(), LayoutView::Mode::PickPoint);
        drag(view, view->toPixel(QPointF(1000, 10)), view->toPixel(QPointF(1000, 10)));
        QCOMPARE(int(portsOfModel().size()), 1);
        rf::Node edge = portsOfModel().front();
        QCOMPARE(edge.type, QStringLiteral("rf_edge_port"));
        QCOMPARE(edge.props.value(QStringLiteral("conductor")).toString(), QStringLiteral("M1"));
        QCOMPARE(edge.props.value(QStringLiteral("x")).toString(), QStringLiteral("1000[um]"));
        QCOMPARE(edge.props.value(QStringLiteral("y")).toString(), QStringLiteral("25[um]"));
        QCOMPARE(edge.props.value(QStringLiteral("facing")).toString(), QStringLiteral("+x"));
        QCOMPARE(doc->focusedNode(), edge.tag);
        // Again on that edge: not a second; and nowhere near an edge,
        // nothing - each said.
        drag(view, view->toPixel(QPointF(1000, 40)), view->toPixel(QPointF(1000, 40)));
        QCOMPARE(int(portsOfModel().size()), 1);
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("drives that edge already")), qPrintable(doc->statusLabel()->text()));
        drag(view, view->toPixel(QPointF(500, 150)), view->toPixel(QPointF(500, 150)));
        QCOMPARE(int(portsOfModel().size()), 1);
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("No edge")), qPrintable(doc->statusLabel()->text()));
        doc->undo();
        QCOMPARE(int(portsOfModel().size()), 0);
        doc->undoStack()->redo();
        QCOMPARE(int(portsOfModel().size()), 1);
        // It is drawn as soon as it is put.
        bool drawn = false;
        for (const LayoutView::Overlay& o : view->overlays())
            for (const LayoutView::Overlay::Label& l : o.labels) drawn = drawn || l.text == edge.label;
        QVERIFY(drawn);

        doc->internalPortButton()->click();
        QCOMPARE(doc->placing(), RF25Doc::Placing::InternalPort);
        QVERIFY(!doc->edgePortButton()->isChecked());
        QCOMPARE(view->mode(), LayoutView::Mode::PickBox);
        drag(view, view->toPixel(QPointF(400, 10)), view->toPixel(QPointF(520, 40)));
        QCOMPARE(int(portsOfModel().size()), 2);
        const rf::Node inner = portsOfModel().at(1);
        QCOMPARE(inner.type, QStringLiteral("rf_internal_port"));
        QCOMPARE(inner.props.value(QStringLiteral("to")).toString(), QStringLiteral("M1"));
        QCOMPARE(inner.props.value(QStringLiteral("from")).toString(), QStringLiteral("ground"));
        QVERIFY2(std::abs(micrometres(inner, "width") - 120) < 4 && std::abs(micrometres(inner, "height") - 30) < 4
                     && std::abs(micrometres(inner, "x") - 460) < 4 && std::abs(micrometres(inner, "y") - 25) < 4,
                 qPrintable(QStringLiteral("%1 by %2 at %3, %4").arg(micrometres(inner, "width")).arg(micrometres(inner, "height"))
                                .arg(micrometres(inner, "x")).arg(micrometres(inner, "y"))));
        drag(view, view->toPixel(QPointF(700, 25)), view->toPixel(QPointF(700, 25)));
        QCOMPARE(int(portsOfModel().size()), 3);
        QVERIFY(!portsOfModel().at(2).props.contains(QStringLiteral("width")));
        QTest::keyClick(view, Qt::Key_Escape);
        QCOMPARE(doc->placing(), RF25Doc::Placing::None);
        QCOMPARE(view->mode(), LayoutView::Mode::Select);
        QVERIFY(!doc->internalPortButton()->isChecked());

        doc->solve();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(w->portsSaid.join(' ').contains(QStringLiteral("cell(s) of M1 under its")), qPrintable(w->portsSaid.join(" | ")));
    }

    // The shapes drawn on the tab (F4 of the open items of 8 November): a
    // model that names no layout begins one beside itself at its first
    // shape - its Layout naming it, the map sending a layer of it to the
    // layer drawn on -, each shape one step of Undo that puts the file and
    // the model back. A rectangle dragged, its corners snapped; a polygon
    // clicked corner by corner, Backspace taking one back, Enter closing
    // it; a via clicked on a via of the stack (refused where there is none);
    // the shape selected deleted by Delete; what cannot be drawn refused,
    // and nothing drawn. The strip drawn solves as the same strip read from
    // a GDS made elsewhere: the same S.
    void aModelsShapesAreDrawnOnItsTab()
    {
        const QString reference = fullWaveLine(QStringLiteral("drawnref.qrf25"));
        QVERIFY(!reference.isEmpty());
        const QString model = path(QStringLiteral("drawn.qrf25"));
        QFile::remove(path(QStringLiteral("drawn.gds")));
        {
            QFile f(reference);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString why;
            std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
            QVERIFY2(read.has_value(), qPrintable(why));
            rf::Model m = *read;
            m.layout().props.remove(QStringLiteral("file"));
            m.layout().child(QStringLiteral("rf_layer_map"))->props.remove(QStringLiteral("rows"));
            m.ports().children.clear();
            rf::Node* study = m.root.child(QStringLiteral("rf_study"));
            QVERIFY(study != nullptr);
            study->props.insert(QStringLiteral("to"), QStringLiteral("1[GHz]"));
            study->props.insert(QStringLiteral("points"), 1);
            QFile out(model);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(m.toJson());
        }
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->layoutFile().isEmpty());
        QCOMPARE(doc->drawLayer(), QStringLiteral("M1"));
        LayoutView* view = doc->view();
        view->showRegion(QRectF(QPointF(-100, -150), QPointF(1100, 400)));

        // A rectangle dragged: the file begun, named, mapped; its corners
        // snapped as the tool snaps them.
        doc->drawButton()->click();
        QCOMPARE(doc->drawTool(), RF25Doc::DrawTool::Rectangle);
        QCOMPARE(view->mode(), LayoutView::Mode::PickBox);
        const QPointF a = view->toPixel(QPointF(3, 2)), b = view->toPixel(QPointF(1002, 51));
        const QRectF want = QRectF(doc->drawSnapped(view->toLayout(a)), doc->drawSnapped(view->toLayout(b))).normalized();
        QVERIFY(want.width() > 500 && want.height() > 0);
        drag(view, a, b);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        const QString gds = path(QStringLiteral("drawn.gds"));
        QVERIFY2(QFileInfo::exists(gds), qPrintable(doc->statusLabel()->text()));
        QCOMPARE(doc->model().layout().props.value(QStringLiteral("file")).toString(), QStringLiteral("drawn.gds"));
        QCOMPARE(doc->layoutFile(), QFileInfo(gds).absoluteFilePath());
        const QJsonArray rows = doc->model().layout().child(QStringLiteral("rf_layer_map"))->props.value(QStringLiteral("rows")).toArray();
        QCOMPARE(rows, QJsonArray{row({QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")})});
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("Begun drawn.gds beside the model")), qPrintable(doc->statusLabel()->text()));
        QVERIFY(doc->processed() != nullptr);
        QCOMPARE(int(doc->processed()->conductors.size()), 1);
        QCOMPARE(doc->processed()->conductors.first().layer, QStringLiteral("M1"));
        const QRectF drawn = doc->processed()->conductors.first().outline.boundingRect();
        QVERIFY2(std::abs(drawn.left() - want.left()) < 1e-6 && std::abs(drawn.right() - want.right()) < 1e-6
                     && std::abs(drawn.top() - want.top()) < 1e-6 && std::abs(drawn.bottom() - want.bottom()) < 1e-6,
                 qPrintable(QStringLiteral("%1 %2 %3 %4").arg(drawn.left()).arg(drawn.top()).arg(drawn.right()).arg(drawn.bottom())));
        // Undone: no file, no name, no row; done again: all three back.
        doc->undo();
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(!QFileInfo::exists(gds));
        QVERIFY(doc->layoutFile().isEmpty());
        QVERIFY(doc->model().layout().child(QStringLiteral("rf_layer_map"))->props.value(QStringLiteral("rows")).toArray().isEmpty());
        doc->undoStack()->redo();
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(QFileInfo::exists(gds) && doc->layout() != nullptr && doc->layout()->shapeCount == 1);
        doc->undo();
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(!QFileInfo::exists(gds));

        // The strip as the reference's, drawn exactly.
        QStringList done;
        QString why;
        QVERIFY2(doc->drawShapes(QJsonArray{QJsonObject{{"add", "rectangle"}, {"layer", "M1"}, {"box", QJsonArray{0, 0, 1000, 50}}}}, &done, &why),
                 qPrintable(why));
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY2(done.first().startsWith(QStringLiteral("Drew in drawn.gds: a rectangle of M1 from (0, 0) to (1000, 50) µm")), qPrintable(done.join(" | ")));
        QFile stripFile(gds);
        QVERIFY(stripFile.open(QIODevice::ReadOnly));
        const QByteArray bytes = stripFile.readAll();
        stripFile.close();

        // What cannot be drawn: refused, each said, nothing drawn.
        const auto refused = [&](const QJsonObject& e, const QString& words) {
            QString no;
            const bool drew = doc->drawShapes(QJsonArray{e}, nullptr, &no);
            QFile f(gds);
            const bool same = f.open(QIODevice::ReadOnly) && f.readAll() == bytes;
            return !drew && no.contains(words) && same ? QString() : QStringLiteral("%1: %2").arg(drew).arg(no);
        };
        QString said = refused(QJsonObject{{"add", "rectangle"}, {"layer", "Ground"}, {"box", QJsonArray{0, 0, 10, 10}}},
                               QStringLiteral("Ground is no metal, via, dielectric or substrate"));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        said = refused(QJsonObject{{"add", "rectangle"}, {"layer", "M9"}, {"box", QJsonArray{0, 0, 10, 10}}}, QStringLiteral("M9 is no layer of the stack"));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        said = refused(QJsonObject{{"add", "rectangle"}, {"layer", "M1"}, {"box", QJsonArray{0, 0, 10, 0}}}, QStringLiteral("no area"));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        said = refused(QJsonObject{{"add", "circle"}, {"layer", "M1"}}, QStringLiteral("a rectangle, a polygon, a path or a via"));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        said = refused(QJsonObject{{"add", "rectangle"}, {"colour", "red"}}, QStringLiteral("'colour' is not a key"));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        said = refused(QJsonObject{{"add", "via"}, {"at", QJsonArray{5, 5}}}, QStringLiteral("M1 is no via of the stack"));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        said = refused(QJsonObject{{"delete", "shape"}, {"layer", "M1"}, {"at", QJsonArray{500, 300}}}, QStringLiteral("no shape of M1's own at (500, 300)"));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        said = refused(QJsonObject{{"add", "polygon"}, {"layer", "M1"}, {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}}, QStringLiteral("3 at least"));
        QVERIFY2(said.isEmpty(), qPrintable(said));

        // A polygon clicked corner by corner: one taken back by Backspace,
        // closed by Enter - a stub off the strip.
        doc->setDrawTool(RF25Doc::DrawTool::Polygon);
        QCOMPARE(view->mode(), LayoutView::Mode::PickPoint);
        QVERIFY(doc->drawButton()->isChecked());
        const QList<QPointF> corners{{400, 50}, {600, 50}, {600, 300}, {777, 333}, {400, 300}};
        for (const QPointF& c : corners) {
            const QPointF at = view->toPixel(c);
            drag(view, at, at);
        }
        QCOMPARE(int(doc->polygonDrawn().size()), 5);
        QTest::keyClick(view, Qt::Key_Backspace);
        QCOMPARE(int(doc->polygonDrawn().size()), 4);
        // (Drawn while it is drawn: an overlay of its own.)
        bool shown = false;
        for (const LayoutView::Overlay& o : view->overlays()) shown = shown || o.name == QStringLiteral("the polygon being drawn");
        QVERIFY(shown);
        QTest::keyClick(view, Qt::Key_Backspace);
        QCOMPARE(int(doc->polygonDrawn().size()), 3);
        drag(view, view->toPixel(QPointF(400, 300)), view->toPixel(QPointF(400, 300)));
        QTest::keyClick(view, Qt::Key_Return);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->polygonDrawn().isEmpty() && doc->layout() != nullptr && doc->layout()->shapeCount == 2, qPrintable(doc->statusLabel()->text()));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("a polygon of M1 of 4 corner(s)")), qPrintable(doc->statusLabel()->text()));
        // (Merged with the strip it touches: one conductor.)
        QCOMPARE(int(doc->processed()->conductors.size()), 1);
        // A double click closes one too; a click on its first corner too.
        for (const QPointF& c : QList<QPointF>{{100, 200}, {200, 200}, {200, 250}}) {
            const QPointF at = view->toPixel(c);
            drag(view, at, at);
        }
        // (A view of its own, not the layout fitted: kept as it is.)
        view->setView(QPointF(321, 123), view->scale() * 1.37);
        const QPointF centre = view->center();
        const double scale = view->scale();
        QTest::mouseDClick(view, Qt::LeftButton, Qt::NoModifier, view->toPixel(QPointF(100, 250)).toPoint());
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY2(doc->layout() != nullptr && doc->layout()->shapeCount == 3, qPrintable(doc->statusLabel()->text()));
        // (The view as it was drawn on, not fitted to the layout read again.)
        QVERIFY(view->center() == centre && view->scale() == scale);
        // A corner within a few pixels of a shape's is that corner, though it
        // is on no grid; a click on the first corner closes a polygon.
        QVERIFY2(doc->drawShapes(QJsonArray{QJsonObject{{"add", "rectangle"}, {"layer", "M1"}, {"box", QJsonArray{800.37, 200.13, 850.37, 250.13}}}}, &done, &why),
                 qPrintable(why));
        QVERIFY(doc->waitForLayout(60000));
        const QPointF near = doc->drawSnapped(QPointF(800.37, 200.13) + QPointF(1.5, -1.5) / view->scale());
        QVERIFY2(QLineF(near, QPointF(800.37, 200.13)).length() < 1e-6, qPrintable(QStringLiteral("%1, %2").arg(near.x()).arg(near.y())));
        for (const QPointF& c : QList<QPointF>{{650, 200}, {700, 200}, {700, 250}, {650, 200}}) {
            const QPointF at = view->toPixel(c);
            drag(view, at, at);
        }
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY2(doc->polygonDrawn().isEmpty() && doc->layout()->shapeCount == 5, qPrintable(doc->statusLabel()->text()));
        QVERIFY2(doc->drawShapes(QJsonArray{QJsonObject{{"delete", "shape"}, {"layer", "M1"}, {"box", QJsonArray{640, 190, 860, 260}}}}, &done, &why),
                 qPrintable(why));
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY2(doc->layout()->shapeCount == 3 && done.first().contains(QStringLiteral("deleted 2 shape(s) of M1 wholly inside")), qPrintable(done.join(" | ")));
        // The port tools and these, one at a time.
        doc->edgePortButton()->click();
        QCOMPARE(doc->drawTool(), RF25Doc::DrawTool::None);
        QVERIFY(!doc->drawButton()->isChecked());
        doc->setDrawTool(RF25Doc::DrawTool::Polygon);
        QCOMPARE(doc->placing(), RF25Doc::Placing::None);
        QVERIFY(!doc->edgePortButton()->isChecked());
        // (Escape puts the tool down.)
        QTest::keyClick(view, Qt::Key_Escape);
        QCOMPARE(doc->drawTool(), RF25Doc::DrawTool::None);
        QVERIFY(!doc->drawButton()->isChecked());
        QCOMPARE(view->mode(), LayoutView::Mode::Select);

        // That one selected and deleted by Delete.
        QVERIFY(view->selectAt(view->toPixel(QPointF(180, 210))));
        QTest::keyClick(view, Qt::Key_Delete);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY2(doc->layout()->shapeCount == 2, qPrintable(doc->statusLabel()->text()));
        // And the stub by its point.
        QVERIFY2(doc->drawShapes(QJsonArray{QJsonObject{{"delete", "shape"}, {"layer", "M1"}, {"at", QJsonArray{500, 200}}}}, &done, &why), qPrintable(why));
        QVERIFY(doc->waitForLayout(60000));
        QCOMPARE(doc->layout()->shapeCount, quint64(1));

        // A via, on a via of the stack: the Via tool's click, its own row.
        {
            rf::Model m = doc->model();
            rf::Node via = m.make(QStringLiteral("rf_via"));
            via.label = QStringLiteral("V0");
            via.props.insert(QStringLiteral("from"), QStringLiteral("ground"));
            via.props.insert(QStringLiteral("to"), QStringLiteral("M1"));
            QVERIFY(m.add(QStringLiteral("stack"), via) != nullptr);
            QVERIFY(doc->setModel(m, QStringLiteral("a via")));
        }
        doc->setViaSize(20);
        // The menu, filled as it opens (twice, the second time anew): its
        // tools, the stack's metals and vias, the via's size.
        QMenu* menu = doc->drawButton()->menu();
        QVERIFY(menu != nullptr);
        emit menu->aboutToShow();
        emit menu->aboutToShow();
        QStringList items;
        QAction* viaTool = nullptr;
        for (QAction* act : menu->actions()) {
            items << act->text();
            if (act->text() == QStringLiteral("Via")) viaTool = act;
        }
        QVERIFY2(items.contains(QStringLiteral("Rectangle")) && items.contains(QStringLiteral("Polygon")) && viaTool != nullptr
                     && items.contains(QStringLiteral("Layer: M1")) && items.contains(QStringLiteral("Via Size: 20 µm…")),
                 qPrintable(items.join(" | ")));
        QMenu* layers = menu->findChild<QMenu*>(QStringLiteral("rfDrawLayers"));
        QVERIFY(layers != nullptr);
        QStringList named;
        for (QAction* act : layers->actions()) named << act->text();
        QCOMPARE(named, (QStringList{QStringLiteral("V0 (a via)"), QStringLiteral("M1"), QStringLiteral("Substrate (where it is)")}));   // (the stack's top first)
        viaTool->trigger();
        QCOMPARE(doc->drawTool(), RF25Doc::DrawTool::Via);
        QVERIFY(doc->drawButton()->isChecked());
        // (The via chosen in the stack is the layer drawn on.)
        doc->focusNode(doc->model().stack().named(QStringLiteral("V0"))->tag);
        QCOMPARE(doc->drawLayer(), QStringLiteral("V0"));
        doc->setDrawTool(RF25Doc::DrawTool::Via);
        drag(view, view->toPixel(QPointF(900, 25)), view->toPixel(QPointF(900, 25)));
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("a via of V0 at (")) && doc->statusLabel()->text().contains(QStringLiteral("20 µm square")),
                 qPrintable(doc->statusLabel()->text()));
        QVERIFY(doc->model().layout().child(QStringLiteral("rf_layer_map"))->props.value(QStringLiteral("rows")).toArray().contains(
            row({QStringLiteral("2"), QStringLiteral("0"), QStringLiteral("V0")})));
        int vias = 0;
        for (const rf::Conductor& c : doc->processed()->conductors) vias += c.layer == QStringLiteral("V0");
        QCOMPARE(vias, 1);
        // Undone - the via and nothing else.
        doc->undo();
        QVERIFY(doc->waitForLayout(60000));
        QCOMPARE(doc->layout()->shapeCount, quint64(1));
        doc->undo();   // (the via layer)
        QTest::keyClick(view, Qt::Key_Escape);

        // The strip drawn and the strip read: the same S.
        QVERIFY(!doc->placeEdgePortAt(QPointF(0, 25)).isEmpty());
        QVERIFY(!doc->placeEdgePortAt(QPointF(1000, 25)).isEmpty());
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok(), qPrintable(doc->solveProblem()));
        RF25Doc* ref = open(reference);
        QVERIFY(ref != nullptr && ref->waitForLayout(60000) && ref->waitForPrepared(60000));
        RF25Doc::SolveRequest one;
        one.frequencies = {1e9};
        ref->solveWith(one);
        QVERIFY2(ref->waitForSolved(300000), "the reference's solve ended");
        const std::shared_ptr<const rf::FullWave> r = ref->fullWave();
        QVERIFY2(r != nullptr && r->ok(), qPrintable(ref->solveProblem()));
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                QVERIFY2(std::abs(w->s.at(0).at(i).at(j) - r->s.at(0).at(i).at(j)) < 1e-6,
                         qPrintable(QStringLiteral("S%1%2: %3 against %4").arg(i + 1).arg(j + 1).arg(std::abs(w->s.at(0).at(i).at(j))).arg(std::abs(r->s.at(0).at(i).at(j)))));
    }

    // The Ruler, as a layout's tab has it: taken up from the toolbar, a
    // drag over the layout measures, its ends snapped to the strip's
    // corners, the ruler labelled on the canvas and said in the status line
    // - its length, dx and dy, its ends. It and the port tools, the drawing
    // tools and the pen are one at a time, each putting the other down. The
    // rulers stay while the layout is read again in place (a shape drawn);
    // Escape clears them, then puts the Ruler down. Put down by its button,
    // the rulers stay, as in a layout's tab.
    void theRulerMeasuresTheLayout()
    {
        const QString model = fullWaveLine(QStringLiteral("ruler.qrf25"));
        QVERIFY(!model.isEmpty());
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        LayoutView* view = doc->view();
        view->showRegion(QRectF(QPointF(-100, -150), QPointF(1100, 200)));
        view->repaint();
        QToolButton* ruler = doc->rulerButton();
        QVERIFY(ruler != nullptr);
        QCOMPARE(ruler->objectName(), QStringLiteral("rfRuler"));
        QCOMPARE(ruler->text(), QStringLiteral("Ruler"));
        QVERIFY(ruler->isCheckable() && !ruler->isChecked());
        QVERIFY(ruler->toolTip().contains(QStringLiteral("drag to measure")));
        // (On the toolbar, beside Zoom Extents and Read the Layout Again.)
        QToolButton* fit = doc->findChild<QToolButton*>(QStringLiteral("rfFit"));
        QVERIFY(fit != nullptr && fit->parentWidget() == ruler->parentWidget());
        QVERIFY(ruler->parentWidget()->layout()->indexOf(ruler) > ruler->parentWidget()->layout()->indexOf(fit));

        ruler->click();
        QVERIFY(ruler->isChecked());
        QCOMPARE(view->mode(), LayoutView::Mode::Ruler);
        QVERIFY2(doc->statusLabel()->text().startsWith(QStringLiteral("Drag over the layout to measure")), qPrintable(doc->statusLabel()->text()));
        // The strip's lower edge, end to end: a few pixels off each corner,
        // snapped to it.
        drag(view, view->toPixel(QPointF(0, 0)) + QPointF(2, -2), view->toPixel(QPointF(1000, 0)) + QPointF(-2, 2));
        QCOMPARE(int(view->rulers().size()), 1);
        const QLineF measured = view->rulers().last();
        QVERIFY2(QLineF(measured.p1(), QPointF(0, 0)).length() < 1e-6 && QLineF(measured.p2(), QPointF(1000, 0)).length() < 1e-6,
                 qPrintable(QStringLiteral("%1, %2 to %3, %4").arg(measured.x1()).arg(measured.y1()).arg(measured.x2()).arg(measured.y2())));
        const QString said = QStringLiteral("Ruler: 1000 µm (dx 1000, dy 0) from 0, 0 to 1000, 0");
        QCOMPARE(doc->rulerSaid(), said);
        QVERIFY2(doc->statusLabel()->text().startsWith(said), qPrintable(doc->statusLabel()->text()));
        // Its width, across: a second ruler, the last one said.
        drag(view, view->toPixel(QPointF(500, 0)), view->toPixel(QPointF(500, 50)));
        QCOMPARE(int(view->rulers().size()), 2);
        QCOMPARE(doc->rulerSaid(), QStringLiteral("Ruler: 50 µm (dx 0, dy 50) from 500, 0 to 500, 50"));
        // The pointer over the layout: where it is said after the ruler.
        {
            const QPointF at = view->toPixel(QPointF(250, 25));
            QMouseEvent move(QEvent::MouseMove, at, view->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &move);
        }
        QVERIFY2(doc->statusLabel()->text().startsWith(QStringLiteral("Ruler: 50 µm")) && doc->statusLabel()->text().contains(QStringLiteral("x = ")),
                 qPrintable(doc->statusLabel()->text()));

        // One tool at a time: a port tool puts the Ruler down, the Ruler
        // the port tool; so the pen and a drawing tool.
        doc->edgePortButton()->click();
        QCOMPARE(doc->placing(), RF25Doc::Placing::EdgePort);
        QCOMPARE(view->mode(), LayoutView::Mode::PickPoint);
        QVERIFY(!ruler->isChecked());
        ruler->click();
        QCOMPARE(doc->placing(), RF25Doc::Placing::None);
        QVERIFY(!doc->edgePortButton()->isChecked());
        QCOMPARE(view->mode(), LayoutView::Mode::Ruler);
        QVERIFY(ruler->isChecked());
        doc->penButton()->click();
        QCOMPARE(view->mode(), LayoutView::Mode::Draw);
        QVERIFY(!ruler->isChecked());
        ruler->click();
        QCOMPARE(view->mode(), LayoutView::Mode::Ruler);
        QVERIFY(!doc->penButton()->isChecked());
        doc->setDrawTool(RF25Doc::DrawTool::Rectangle);
        QCOMPARE(view->mode(), LayoutView::Mode::PickBox);
        QVERIFY(!ruler->isChecked());
        ruler->click();
        QVERIFY(doc->drawTool() == RF25Doc::DrawTool::None);
        QVERIFY(!doc->drawButton()->isChecked());
        QCOMPARE(view->mode(), LayoutView::Mode::Ruler);
        // The view put in the Ruler's mode another way: the port tool in
        // hand put down all the same, the view left measuring.
        doc->setPlacing(RF25Doc::Placing::InternalPort);
        view->setMode(LayoutView::Mode::Ruler);
        QCOMPARE(doc->placing(), RF25Doc::Placing::None);
        QCOMPARE(view->mode(), LayoutView::Mode::Ruler);
        QVERIFY(ruler->isChecked() && !doc->internalPortButton()->isChecked());
        QCOMPARE(int(view->rulers().size()), 2);

        // A shape drawn: the layout read again where it is shown, the
        // rulers kept.
        QStringList done;
        QString why;
        QVERIFY2(doc->drawShapes(QJsonArray{QJsonObject{{QStringLiteral("add"), QStringLiteral("rectangle")},
                                                        {QStringLiteral("layer"), QStringLiteral("M1")},
                                                        {QStringLiteral("box"), QJsonArray{200, 100, 300, 150}}}},
                                 &done, &why),
                 qPrintable(why));
        QVERIFY(doc->waitForLayout(60000));
        QCOMPARE(int(view->rulers().size()), 2);
        QCOMPARE(doc->rulerSaid(), QStringLiteral("Ruler: 50 µm (dx 0, dy 50) from 500, 0 to 500, 50"));

        // Escape: the rulers cleared (the status line back to the model's),
        // then the Ruler put down.
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Escape);
        QVERIFY(view->rulers().isEmpty());
        QVERIFY(doc->rulerSaid().isEmpty());
        QVERIFY2(!doc->statusLabel()->text().startsWith(QStringLiteral("Ruler:")), qPrintable(doc->statusLabel()->text()));
        QCOMPARE(view->mode(), LayoutView::Mode::Ruler);
        QTest::keyClick(view, Qt::Key_Escape);
        QCOMPARE(view->mode(), LayoutView::Mode::Select);
        QVERIFY(!ruler->isChecked());
        // Put down by its button, what it measured stays.
        ruler->click();
        drag(view, view->toPixel(QPointF(0, 0)), view->toPixel(QPointF(0, 50)));
        QCOMPARE(int(view->rulers().size()), 1);
        ruler->click();
        QCOMPARE(view->mode(), LayoutView::Mode::Select);
        QCOMPARE(int(view->rulers().size()), 1);
        QCOMPARE(doc->rulerSaid(), QStringLiteral("Ruler: 50 µm (dx 0, dy 50) from 0, 0 to 0, 50"));
    }

    // RF 2.5D's layout drawn by the GPU or the CPU as a setting of its own
    // says (Viewers/rf25Gpu, the CPU's unless chosen), apart from the
    // layout viewer's: RF 2.5D > Settings… has it, with what draws it said
    // under it - the GPU and its backend, the CPU, or what keeps the GPU
    // from drawing here (offscreen, QUCS_GPU=0) -, kept on OK and not on
    // Cancel; Settings… > Viewers has it too. Each open model's view drawn
    // as chosen, at once; the layout viewer's setting does not move it.
    void theLayoutIsDrawnByTheGpuAsItsOwnSettingSays()
    {
        using qucs_s::gpu::Viewer;
        const auto back = qScopeGuard([] {
            QucsSettingsFile().remove(qucs_s::gpu::settingKey(Viewer::RF25));
            QucsSettingsFile().remove(qucs_s::gpu::settingKey(Viewer::Layout));
            emit qucs_s::gpu::Notifier::instance()->changed(Viewer::RF25);
            emit qucs_s::gpu::Notifier::instance()->changed(Viewer::Layout);
        });
        QCOMPARE(qucs_s::gpu::settingKey(Viewer::RF25), QStringLiteral("Viewers/rf25Gpu"));
        QCOMPARE(qucs_s::gpu::viewerName(Viewer::RF25), QStringLiteral("RF 2.5D layout"));
        QVERIFY(!qucs_s::gpu::wanted(Viewer::RF25));
        RF25Doc* doc = app->newRf25Model();
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        QVERIFY(view->gpuViewer() == Viewer::RF25);
        LayoutView plain;   // (a layout's tab's: the layout viewer's setting)
        QVERIFY(plain.gpuViewer() == Viewer::Layout);
        const QString hindrance = qucs_s::gpu::hindrance();
        qInfo("%s", qPrintable(hindrance.isEmpty() ? QStringLiteral("The GPU can draw here: %1").arg(qucs_s::gpu::backendName()) : hindrance));
        const auto drawnAsChosen = [&] {
            QCOMPARE(view->gpu(), qucs_s::gpu::used(Viewer::RF25));
            QCOMPARE(plain.gpu(), qucs_s::gpu::used(Viewer::Layout));
        };
        drawnAsChosen();
        QVERIFY(!view->gpu());
        QSignalSpy told(qucs_s::gpu::Notifier::instance(), &qucs_s::gpu::Notifier::changed);
        {
            rf::RF25SettingsDialog d(app);
            QVERIFY(d.gpuBox() != nullptr && d.gpuStatusLabel() != nullptr);
            QCOMPARE(d.gpuBox()->objectName(), QStringLiteral("rf25GpuBox"));
            QCOMPARE(d.gpuBox()->text(), QStringLiteral("Draw with the GPU"));
            QVERIFY(!d.gpuBox()->isChecked());
            QCOMPARE(d.gpuStatusLabel()->text(), hindrance.isEmpty() ? QStringLiteral("The CPU draws the layout.") : hindrance);
            d.gpuBox()->setChecked(true);
            QCOMPARE(d.gpuStatusLabel()->text(),
                     hindrance.isEmpty() ? QStringLiteral("The GPU draws the layout (%1).").arg(qucs_s::gpu::backendName()) : hindrance);
            d.reject();   // (Cancel: nothing kept.)
        }
        QVERIFY(!qucs_s::gpu::wanted(Viewer::RF25));
        QCOMPARE(told.count(), 0);
        {
            rf::RF25SettingsDialog d(app);
            d.gpuBox()->setChecked(true);
            d.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(d.result(), int(QDialog::Accepted));
        }
        QVERIFY(qucs_s::gpu::wanted(Viewer::RF25));
        QVERIFY(!qucs_s::gpu::wanted(Viewer::Layout));
        QCOMPARE(QucsSettingsFile().value(QStringLiteral("Viewers/rf25Gpu")).toBool(), true);
        QCOMPARE(told.count(), 1);
        QVERIFY(told.at(0).at(0).value<Viewer>() == Viewer::RF25);
        drawnAsChosen();
        QCOMPARE(view->gpu(), hindrance.isEmpty());
        {
            rf::RF25SettingsDialog d(app);
            QVERIFY(d.gpuBox()->isChecked());
        }
        // The layout viewer's setting: the model's view as its own says.
        qucs_s::gpu::setWanted(Viewer::RF25, false);
        qucs_s::gpu::setWanted(Viewer::Layout, true);
        drawnAsChosen();
        QVERIFY(!view->gpu());
        // Settings… > Viewers: the same setting.
        {
            QucsSettingsDialog dlg(app);
            auto* box = dlg.findChild<QCheckBox*>(QStringLiteral("gpuRF25"));
            QVERIFY(box != nullptr);
            QCOMPARE(box->text(), qucs_s::gpu::viewerName(Viewer::RF25));
            QVERIFY(box->toolTip().contains(QStringLiteral("RF 2.5D > Settings")));
            QVERIFY(!box->isChecked());
            QVERIFY(dlg.findChild<QCheckBox*>(QStringLiteral("gpuLayout"))->isChecked());
            box->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        }
        QVERIFY(qucs_s::gpu::wanted(Viewer::RF25));
        drawnAsChosen();
        {
            rf::RF25SettingsDialog d(app);
            QVERIFY(d.gpuBox()->isChecked());
        }
        // QUCS_GPU=0: the CPU, whatever is chosen, said.
        qputenv("QUCS_GPU", "0");
        {
            rf::RF25SettingsDialog d(app);
            QVERIFY2(d.gpuStatusLabel()->text().contains(QStringLiteral("QUCS_GPU=0")), qPrintable(d.gpuStatusLabel()->text()));
        }
        emit qucs_s::gpu::Notifier::instance()->changed(Viewer::RF25);
        QVERIFY(!view->gpu());
        qunsetenv("QUCS_GPU");
    }

    // A model solved without its tab (rf25_solve.h), as a sweep's point or an
    // optimization's candidate is: from the model and its layout as read,
    // processed, meshed, set up and solved on the caller's thread - the tab's
    // own answer, full-wave (each S and Z) and quasi-static (the charge, the
    // line per metre, the loop's inductance), to the last digit. A stop set
    // before it starts gives nothing; a layout of nothing on the stack, no
    // mesh, and says why.
    void aModelSolvedWithoutItsTabIsTheTabsAnswer()
    {
        const QString file = fullWaveLine(QStringLiteral("untabbed.qrf25"));
        QVERIFY(!file.isEmpty());
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        const QString folder = QFileInfo(file).absolutePath();
        const std::shared_ptr<const qucs_s::layout::Layout> read = doc->layout();
        QVERIFY(read != nullptr);
        RF25Doc::SolveRequest wave;
        wave.solver = QStringLiteral("full_wave");
        doc->solveWith(wave);
        QVERIFY(doc->waitForSolved(300000));
        const std::shared_ptr<const rf::FullWave> tab = doc->fullWave();
        QVERIFY2(tab != nullptr && tab->ok(), qPrintable(doc->solveProblem()));
        {
            rf::SolveJob job;
            const rf::ModelSolution m = rf::solveModel(job, doc->model(), *read, QStringLiteral("strip.gds"), folder, wave);
            QVERIFY2(m.ok() && m.wave != nullptr, qPrintable(m.problem));
            QCOMPARE(int(m.mesh->cells.size()), int(doc->mesh()->cells.size()));
            QCOMPARE(m.wave->frequencies, tab->frequencies);
            QCOMPARE(m.wave->portNames, tab->portNames);
            for (int k = 0; k < tab->frequencies.size(); ++k)
                for (int i = 0; i < 2; ++i)
                    for (int j = 0; j < 2; ++j) {
                        QVERIFY2(std::abs(m.wave->s.at(k).at(i).at(j) - tab->s.at(k).at(i).at(j)) < 1e-12, qPrintable(QStringLiteral("S%1%2 at %3").arg(i + 1).arg(j + 1).arg(k)));
                        QVERIFY(std::abs(m.wave->z.at(k).at(i).at(j) - tab->z.at(k).at(i).at(j)) <= 1e-12 * std::abs(tab->z.at(k).at(i).at(j)));
                    }
            QCOMPARE(m.wave->lines.value(0), tab->lines.value(0));   // (the mesh's note first)
        }
        RF25Doc::SolveRequest quasi;
        quasi.solver = QStringLiteral("quasi_static");
        doc->solveWith(quasi);
        QVERIFY(doc->waitForSolved(300000));
        const std::shared_ptr<const rf::Extraction> e = doc->extraction();
        QVERIFY2(e != nullptr && e->ok(), qPrintable(doc->solveProblem()));
        {
            rf::SolveJob job;
            const rf::ModelSolution m = rf::solveModel(job, doc->model(), *read, QStringLiteral("strip.gds"), folder, quasi);
            QVERIFY2(m.ok() && m.extraction != nullptr, qPrintable(m.problem));
            QCOMPARE(m.extraction->names, e->names);
            QCOMPARE(m.extraction->c, e->c);
            QCOMPARE(m.extraction->perMetre.maxwell, e->perMetre.maxwell);
            QCOMPARE(m.length, doc->solvedLength());
            QVERIFY(m.inductance != nullptr && doc->inductance() != nullptr);
            QCOMPARE(m.inductance->l, doc->inductance()->l);
        }
        // A coplanar line, short and wide - three strips 150 µm long across
        // 332 µm, its gaps 6 µm -, quasi-static: its mesh graded at the gaps
        // as the tab grades them, and its length its own along the line, not
        // the longer side solved.
        {
            QVERIFY(writeRectangles(path(QStringLiteral("cpw.gds")),
                                    {QRectF(0, 0, 150, 150), QRectF(0, 156, 150, 20), QRectF(0, 182, 150, 150)}));
            rf::Model cpw = doc->model();
            cpw.layout().props.insert(QStringLiteral("file"), QStringLiteral("cpw.gds"));
            cpw.ports().children.clear();
            cpw.root.child(QStringLiteral("rf_study"))->props.insert(QStringLiteral("kind"), QStringLiteral("quasi_static"));
            const QString cpwFile = path(QStringLiteral("cpw.qrf25"));
            {
                QFile f(cpwFile);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write(cpw.toJson());
            }
            RF25Doc* line = open(cpwFile);
            QVERIFY(line != nullptr);
            QVERIFY(line->waitForLayout(60000));
            QVERIFY(line->waitForPrepared(60000));
            line->solve();
            QVERIFY(line->waitForSolved(300000));
            const std::shared_ptr<const rf::Extraction> le = line->extraction();
            QVERIFY2(le != nullptr && le->ok(), qPrintable(line->solveProblem()));
            QVERIFY(le->perMetre.fromMiddle);
            rf::SolveJob job;
            const rf::ModelSolution m = rf::solveModel(job, line->model(), *line->layout(), QStringLiteral("cpw.gds"), folder);
            QVERIFY2(m.ok() && m.extraction != nullptr, qPrintable(m.problem));
            QCOMPARE(int(m.mesh->cells.size()), int(line->mesh()->cells.size()));
            QCOMPARE(m.extraction->c, le->c);
            QCOMPARE(m.length, line->solvedLength());
            QVERIFY2(std::abs(m.length - 150e-6) < 1e-9, qPrintable(QString::number(m.length * 1e6)));
        }
        // A stop set before it begins: nothing, and said so.
        {
            rf::SolveJob job;
            job.stop = true;
            const rf::ModelSolution m = rf::solveModel(job, doc->model(), *read, QStringLiteral("strip.gds"), folder, quasi);
            QVERIFY(m.stopped && !m.ok());
            QVERIFY(m.extraction == nullptr && m.wave == nullptr);
        }
        // A map that sends nothing to the stack: no mesh, and why.
        {
            rf::Model none = doc->model();
            none.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), rowsOf({{QStringLiteral("9"), QStringLiteral("0"), QStringLiteral("M1")}}));
            rf::SolveJob job;
            const rf::ModelSolution m = rf::solveModel(job, none, *read, QStringLiteral("strip.gds"), folder, quasi);
            QVERIFY(!m.ok());
            QVERIFY2(m.problem.contains(QStringLiteral("no mesh to solve on")), qPrintable(m.problem));
        }
        // A convergence check is the tab's.
        {
            rf::SolveJob job;
            RF25Doc::SolveRequest check;
            check.levels = {2, 3};
            QVERIFY(!rf::solveModel(job, doc->model(), *read, QStringLiteral("strip.gds"), folder, check).ok());
        }
    }

    // A substrate that ends (F2 of the open items of 8 November), drawn on
    // the tab: a rectangle drawn on the Substrate is where it is - the map
    // sending its layer of the file there, the processing keeping it apart
    // from the conductors as the substrate's extent, the tab outlining it.
    // A strip's open stub over it ending 30 µm beside the strip holds less
    // than over the whole substrate, as the quasi-static solve and the
    // full-wave one's capacitance at a low frequency both say, alike within
    // 3 % - as they are without the end -, and its line per metre as the 2D
    // multiphysics engine's cross-section does. Undone, the whole substrate
    // again.
    void aSubstrateThatEndsIsSolvedByBothSolvers()
    {
        const QString model = fullWaveLine(QStringLiteral("ended.qrf25"));
        QVERIFY(!model.isEmpty());
        {
            QFile f(model);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString why;
            std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
            QVERIFY2(read.has_value(), qPrintable(why));
            rf::Model m = *read;
            f.close();
            m.layout().props.insert(QStringLiteral("file"), QStringLiteral("ended.gds"));
            // One port: an open stub, its capacitance at a low frequency.
            m.ports().children.erase(m.ports().children.begin() + 1);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(m.toJson());
        }
        QVERIFY(writeStrip(path(QStringLiteral("ended.gds")), 1000, 50));
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        const double f0 = 0.5e9;
        double perMetre = 0;
        const auto capacitances = [&](double* quasi, double* wave, QStringList* said) {
            RF25Doc::SolveRequest q;
            q.solver = QStringLiteral("quasi_static");
            doc->solveWith(q);
            if (!doc->waitForSolved(300000) || !doc->extraction() || !doc->extraction()->ok()) return false;
            *quasi = doc->extraction()->maxwell.at(0).at(0);
            *said = doc->extraction()->lines;
            perMetre = doc->extraction()->perMetre.maxwell.isEmpty() ? 0 : doc->extraction()->perMetre.maxwell.at(0).at(0);
            RF25Doc::SolveRequest w;
            w.solver = QStringLiteral("full_wave");
            w.frequencies = {f0};
            doc->solveWith(w);
            if (!doc->waitForSolved(300000) || !doc->fullWave() || !doc->fullWave()->ok()) return false;
            *wave = -1 / (2 * M_PI * f0 * doc->fullWave()->z.at(0).at(0).at(0).imag());
            *said += doc->fullWave()->lines;
            return true;
        };
        double wholeQ = 0, wholeW = 0, endedQ = 0, endedW = 0;
        QStringList said;
        QVERIFY2(capacitances(&wholeQ, &wholeW, &said), qPrintable(doc->solveProblem()));
        QVERIFY2(std::abs(wholeW / wholeQ - 1) < 0.03, qPrintable(QStringLiteral("%1 against %2 fF").arg(wholeW * 1e15).arg(wholeQ * 1e15)));
        const double wholePerMetre = perMetre;
        QVERIFY(wholePerMetre > 0);

        // Drawn on the Substrate: where it is.
        QStringList done;
        QString why;
        QVERIFY2(doc->drawShapes(QJsonArray{QJsonObject{{"add", "rectangle"}, {"layer", "Substrate"}, {"box", QJsonArray{-600, -400, 1600, 80}}}}, &done, &why),
                 qPrintable(why));
        QVERIFY(doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY(doc->processed() != nullptr && doc->processed()->extents.contains(QStringLiteral("Substrate")));
        QCOMPARE(int(doc->processed()->conductors.size()), 1);
        bool outlined = false;
        for (const LayoutView::Overlay& o : doc->view()->overlays()) outlined = outlined || o.name.startsWith(QStringLiteral("where Substrate is"));
        QVERIFY(outlined);
        QVERIFY2(capacitances(&endedQ, &endedW, &said), qPrintable(doc->solveProblem()));
        const QString lines = said.join(QStringLiteral(" | "));
        QVERIFY2(endedQ < 0.99 * wholeQ && endedQ > 0.5 * wholeQ, qPrintable(QStringLiteral("%1 against %2 fF | %3").arg(endedQ * 1e15).arg(wholeQ * 1e15).arg(lines)));
        QVERIFY2(std::abs(endedW / endedQ - 1) < 0.03, qPrintable(QStringLiteral("%1 against %2 fF | %3").arg(endedW * 1e15).arg(endedQ * 1e15).arg(lines)));
        QVERIFY2(lines.contains(QStringLiteral("Substrate ends: eps_r 9.8 inside its shapes, 1 beyond them")) && lines.contains(QStringLiteral("The ended dielectric's faces in")),
                 qPrintable(lines));
        // Its line's per metre against the 2D multiphysics engine's
        // cross-section - the strip 0.5 µm thick, the substrate ending 30 µm
        // beside it, air beyond, in a box 6 mm wide; at two mesh sizes 125.95
        // and 111.61 pF/m, a ratio of 0.886 - within 3 %.
        QVERIFY2(std::abs(perMetre / wholePerMetre / 0.886 - 1) < 0.03,
                 qPrintable(QStringLiteral("%1 against %2 pF/m").arg(perMetre * 1e12).arg(wholePerMetre * 1e12)));
        qWarning().noquote() << QStringLiteral("stub: whole %1 / %2 fF, ended %3 / %4 fF; per metre %5 and %6 pF/m")
                                    .arg(wholeQ * 1e15).arg(wholeW * 1e15).arg(endedQ * 1e15).arg(endedW * 1e15)
                                    .arg(wholePerMetre * 1e12).arg(perMetre * 1e12);

        // Undone: the whole substrate.
        doc->undo();
        QVERIFY(doc->waitForLayout(60000) && doc->waitForPrepared(60000));

        // A block of substrate beyond the stub's open end, nothing under the
        // stub, room for its panels and no more: the correction with the
        // layer drawn everywhere - under the stub too, its cells among its
        // panels - refused, and so no per metre, and said why, where the
        // layer everywhere's cross-section and the whole correction as drawn
        // counted the layer twice. The whole as it is.
        {
            QVERIFY2(doc->drawShapes(QJsonArray{QJsonObject{{"add", "rectangle"}, {"layer", "Substrate"}, {"box", QJsonArray{1100, -50, 1200, 100}}}}, &done, &why),
                     qPrintable(why));
            QVERIFY(doc->waitForLayout(60000) && doc->waitForPrepared(60000));
            double q0 = 0, w0 = 0;
            QStringList said0;
            QVERIFY2(capacitances(&q0, &w0, &said0), qPrintable(doc->solveProblem()));
            const QString lines0 = said0.join(QStringLiteral(" | "));
            QVERIFY2(perMetre > 0, qPrintable(lines0));
            const QRegularExpressionMatch panels = QRegularExpression(QStringLiteral("faces in (\\d+) panel")).match(lines0);
            QVERIFY2(panels.hasMatch(), qPrintable(lines0));
            rf::setDielectricMostPanels(panels.captured(1).toInt());
            RF25Doc::SolveRequest q;
            q.solver = QStringLiteral("quasi_static");
            doc->solveWith(q);
            const bool solved = doc->waitForSolved(300000) && doc->extraction() && doc->extraction()->ok();
            rf::setDielectricMostPanels(0);
            QVERIFY2(solved, qPrintable(doc->solveProblem()));
            const QString said2 = doc->extraction()->lines.join(QStringLiteral(" | "));
            QVERIFY2(doc->extraction()->perMetre.maxwell.isEmpty() && said2.contains(QStringLiteral("No per metre of the line")), qPrintable(said2));
            QVERIFY(std::abs(doc->extraction()->maxwell.at(0).at(0) / q0 - 1) < 1e-9);
            doc->undo();
            QVERIFY(doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        }
        QVERIFY(doc->processed()->extents.isEmpty());
        RF25Doc::SolveRequest q;
        q.solver = QStringLiteral("quasi_static");
        doc->solveWith(q);
        QVERIFY(doc->waitForSolved(300000) && doc->extraction() && doc->extraction()->ok());
        QVERIFY(std::abs(doc->extraction()->maxwell.at(0).at(0) / wholeQ - 1) < 1e-9);
    }

    // A strip of 300 µm on 254 µm of 6.15 over a ground, the substrate cut
    // flush with it - the strip's edges on its walls' tops, where the field
    // is most singular -, against the 2D multiphysics engine's
    // cross-sections of the same (the strip 0.2 µm thick, in a box 8 mm wide
    // with 4 mm of air): 83.30 pF/m flush, 123.39 with the substrate
    // everywhere. The ratio of the two within 3.5 % of the engine's at ten
    // cells across and 2.5 % at twenty (2.8 and 2.1 %), the two meshes
    // within 1 % of each other. (Its panels coarsened, those next to the
    // strip too, past 4000: at twenty-eight cells across its line jumped
    // 10 %, and at thirty read more than the substrate everywhere; the walls'
    // eight rows read it 2.3 % high where it converged.) The line 3 mm: at
    // 1.5 mm the end's difference summed along it is a part in a hundred
    // short of the endless line's.
    void aStripFlushWithItsMesaIsThe2DEnginesLine()
    {
        QVERIFY(writeStrip(path(QStringLiteral("flush.gds")), 3000, 300));
        rf::Model m = rf::Model::blank();
        std::vector<rf::Node>& stack = m.stackNode().children;
        stack.erase(std::remove_if(stack.begin(), stack.end(), [](const rf::Node& n) { return n.type == QLatin1String("rf_dielectric"); }),
                    stack.end());
        for (rf::Node& n : stack)
            if (n.type == QLatin1String("rf_substrate")) {
                n.props.insert(QStringLiteral("thickness"), QStringLiteral("254[um]"));
                n.props.insert(QStringLiteral("epsr"), QStringLiteral("6.15"));
            }
        m.layout().props.insert(QStringLiteral("file"), QStringLiteral("flush.gds"));
        m.layout().child(QStringLiteral("rf_layer_map"))
            ->props.insert(QStringLiteral("rows"), rowsOf({{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}}));
        m.ports().children.clear();
        const QString model = path(QStringLiteral("flush.qrf25"));
        const auto write = [&](int across) {
            m.mesh().props.insert(QStringLiteral("cells_across"), across);
            QFile f(model);
            if (!f.open(QIODevice::WriteOnly)) return false;
            f.write(m.toJson());
            return true;
        };
        QVERIFY(write(5));
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        const auto perMetre = [&]() {
            RF25Doc::SolveRequest q;
            q.solver = QStringLiteral("quasi_static");
            doc->solveWith(q);
            if (!doc->waitForSolved(300000) || !doc->extraction() || !doc->extraction()->ok() || doc->extraction()->perMetre.maxwell.isEmpty())
                return 0.0;
            return doc->extraction()->perMetre.maxwell.at(0).at(0);
        };
        QList<double> ratios;
        for (const int across : {10, 20}) {
            QVERIFY(doc->editNode(doc->model().mesh().tag, [across](rf::Node& n) { n.props.insert(QStringLiteral("cells_across"), across); },
                                  QStringLiteral("across")));
            QVERIFY(doc->waitForPrepared(60000));
            const double whole = perMetre();
            QVERIFY2(whole > 0, qPrintable(doc->solveProblem()));
            QStringList done;
            QString why;
            QVERIFY2(doc->drawShapes(QJsonArray{QJsonObject{{"add", "rectangle"}, {"layer", "Substrate"}, {"box", QJsonArray{-2000, 0, 5000, 300}}}}, &done, &why),
                     qPrintable(why));
            QVERIFY(doc->waitForLayout(60000) && doc->waitForPrepared(60000));
            const double flush = perMetre();
            QVERIFY2(flush > 0, qPrintable(doc->solveProblem()));
            const QString lines = doc->extraction()->lines.join(QStringLiteral(" | "));
            QVERIFY2(!lines.contains(QStringLiteral("grow by")), qPrintable(lines));
            ratios << flush / whole;
            QVERIFY2(std::abs(flush / whole / (83.30 / 123.39) - 1) < (across == 10 ? 0.035 : 0.025),
                     qPrintable(QStringLiteral("%1 cells across: %2 pF/m flush, %3 everywhere | %4").arg(across).arg(flush * 1e12).arg(whole * 1e12).arg(lines)));
            qWarning().noquote() << QStringLiteral("%1 across: flush %2, everywhere %3 pF/m").arg(across).arg(flush * 1e12).arg(whole * 1e12);
            doc->undo();
            QVERIFY(doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        }
        QVERIFY2(std::abs(ratios.at(1) / ratios.at(0) - 1) < 0.01, qPrintable(QStringLiteral("%1 against %2").arg(ratios.at(1)).arg(ratios.at(0))));
        doc->setDocChanged(false);
        app->closeAllFiles();
    }

    // A TRL calibration (F3 of the open items of 8 November): 150 µm pads
    // and 100 µm of feed at each end - the fixture - and standards made with
    // them, each its own model: a thru (the feeds joined), lines 500 and 1500
    // µm longer, a reflect (each fixture open). The device, an 800 µm line
    // between the fixtures, solved with them: the pads' reflection taken out,
    // what is left a matched line whose phase is the calibration's own
    // gamma over 800 µm; the lines' effective permittivity what a microstrip
    // of 50 µm on 100 µm of 9.8 has; said.
    void aTrlCalibrationTakesThePadsOut()
    {
        const QString base = fullWaveLine(QStringLiteral("trlbase.qrf25"));
        QVERIFY(!base.isEmpty());
        QFile f(base);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString why;
        const std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
        QVERIFY2(read.has_value(), qPrintable(why));
        f.close();
        // A pad at x0 to x0 + 150, its feed's line from 150 on: the fixtures
        // with a line of \a line between their feeds' ends (the feeds' 100 µm
        // each beside it); a gap of \a gap for the reflect, each fixture open.
        const auto made = [&](const QString& name, double line, bool open, rf::Model* into = nullptr) {
            QList<QRectF> rects;
            const double inner = 200 + line;   // the feeds and the line
            rects << QRectF(0, -50, 150, 150);
            if (open) {
                rects << QRectF(150, 0, 100, 50) << QRectF(400, 0, 100, 50) << QRectF(500, -50, 150, 150);
            } else {
                rects << QRectF(150, 0, inner, 50) << QRectF(150 + inner, -50, 150, 150);
            }
            const double end = open ? 650 : 300 + inner;
            if (!writeRectangles(path(name + QStringLiteral(".gds")), rects)) return QString();
            rf::Model m = *read;
            m.layout().props.insert(QStringLiteral("file"), name + QStringLiteral(".gds"));
            m.ports().children.clear();
            for (const QPair<double, const char*>& at : {qMakePair(0.0, "P1"), qMakePair(end, "P2")}) {
                rf::Node port = m.make(QStringLiteral("rf_edge_port"));
                port.label = QLatin1String(at.second);
                port.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
                port.props.insert(QStringLiteral("x"), QStringLiteral("%1[um]").arg(at.first));
                port.props.insert(QStringLiteral("y"), QStringLiteral("25[um]"));
                if (m.add(QStringLiteral("ports"), port) == nullptr) return QString();
            }
            rf::Node* study = m.root.child(QStringLiteral("rf_study"));
            study->props.insert(QStringLiteral("from"), QStringLiteral("5[GHz]"));
            study->props.insert(QStringLiteral("to"), QStringLiteral("25[GHz]"));
            study->props.insert(QStringLiteral("points"), 3);
            if (into != nullptr) {
                *into = m;
                return QString(QStringLiteral("ok"));
            }
            QFile out(path(name + QStringLiteral(".qrf25")));
            if (!out.open(QIODevice::WriteOnly)) return QString();
            out.write(m.toJson());
            return out.fileName();
        };
        QVERIFY(!made(QStringLiteral("trl_thru"), 0, false).isEmpty());
        QVERIFY(!made(QStringLiteral("trl_line1"), 500, false).isEmpty());
        QVERIFY(!made(QStringLiteral("trl_line2"), 1500, false).isEmpty());
        QVERIFY(!made(QStringLiteral("trl_open"), 0, true).isEmpty());
        rf::Model dut;
        QVERIFY(!made(QStringLiteral("trl_dut"), 800, false, &dut).isEmpty());
        rf::Node trl = dut.make(QStringLiteral("rf_trl"));
        trl.props.insert(QStringLiteral("thru"), QStringLiteral("trl_thru.qrf25"));
        trl.props.insert(QStringLiteral("lines"), rowsOf({{QStringLiteral("trl_line1.qrf25"), QStringLiteral("500[um]")},
                                                          {QStringLiteral("trl_line2.qrf25"), QStringLiteral("1500[um]")}}));
        trl.props.insert(QStringLiteral("reflect"), QStringLiteral("trl_open.qrf25"));
        QVERIFY(dut.add(QStringLiteral("ports"), trl) != nullptr);
        QVERIFY2(!dut.problems().join(' ').contains(QStringLiteral("TRL")), qPrintable(dut.problems().join(" | ")));
        {
            // (Named nothing to be calibrated by: said.)
            rf::Model bare = dut;
            for (rf::Node& n : bare.ports().children)
                if (n.type == QLatin1String("rf_trl")) n.props.insert(QStringLiteral("thru"), QString());
            QVERIFY(bare.problems().join(' ').contains(QStringLiteral("The TRL calibration names no thru")));
        }
        const QString model = path(QStringLiteral("trl_dut.qrf25"));
        {
            QFile out(model);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(dut.toJson());
        }
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY2(doc->waitForSolved(600000), "the solve ended");
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok(), qPrintable(doc->solveProblem() + (w ? w->problems.join(" | ") : QString())));
        const QString lines = w->lines.join(QStringLiteral(" | "));
        static const QRegularExpression eps(QStringLiteral("effective permittivity ([0-9.]+) at the top"));
        const QRegularExpressionMatch m = eps.match(lines);
        QVERIFY2(m.hasMatch(), qPrintable(lines));
        const double eeff = m.captured(1).toDouble();
        QVERIFY2(eeff > 6.0 && eeff < 7.2, qPrintable(lines));
        QVERIFY2(w->warnings.join(' ').contains(QStringLiteral("lines' own impedance")), qPrintable(w->warnings.join(" | ")));
        // (The reflect an open: its angle near nought.)
        static const QRegularExpression angle(QStringLiteral("The reflect found to reflect ([0-9.]+) at (-?[0-9.]+) degrees"));
        const QRegularExpressionMatch r = angle.match(lines);
        QVERIFY2(r.hasMatch() && std::abs(r.captured(2).toDouble()) < 45 && r.captured(1).toDouble() > 0.8, qPrintable(lines));
        for (int k = 0; k < w->frequencies.size(); ++k) {
            const double fk = w->frequencies.at(k);
            const auto& s = w->s.at(k);
            // (As solved, the pads reflect - ten times what is left at the
            // least; calibrated, a matched line.)
            const std::complex<double> raw11 = [&] {
                const auto& z = w->zSolved.at(k);
                Eigen::Matrix2cd Z;
                Z << z.at(0).at(0), z.at(0).at(1), z.at(1).at(0), z.at(1).at(1);
                const Eigen::Matrix2cd one = Eigen::Matrix2cd::Identity();
                return ((Z - 50.0 * one) * (Z + 50.0 * one).inverse())(0, 0);
            }();
            const double phase = std::arg(s.at(1).at(0)), expected = -2 * M_PI * fk * std::sqrt(eeff) / 299792458.0 * 800e-6;
            const double off = std::remainder(phase - expected, 2 * M_PI);
            QVERIFY2(std::abs(s.at(0).at(0)) < 0.03 && std::abs(s.at(1).at(1)) < 0.03 && std::abs(std::abs(s.at(1).at(0)) - 1) < 0.03
                         && std::abs(off) < 0.06 && std::abs(raw11) > 10 * std::abs(s.at(0).at(0)),
                     qPrintable(QStringLiteral("%1 GHz: S11 %2, S22 %3, |S21| %4, its phase %5 off; as solved |S11| %6 | %7")
                                    .arg(fk / 1e9).arg(std::abs(s.at(0).at(0))).arg(std::abs(s.at(1).at(1))).arg(std::abs(s.at(1).at(0)))
                                    .arg(off).arg(std::abs(raw11)).arg(lines)));
        }
        qWarning().noquote() << lines;
    }

    // A port's returns given as a list (the feature request of 4 November):
    // terminals under Ports - a ground pad's patch each - and the ports'
    // Return "terminals" with their names, as a list in one and a text in
    // the other. Read by the solve, the terminals drawn with a line from each
    // port to each of its returns, what each drives said and its tips' shares
    // of its current, the Touchstone saying where its reference plane is, and
    // the solve kept beside the model with its returns. A terminal renamed is
    // renamed among the returns; the panel's Returns list offers the
    // terminals and the other ports; with the Internal Port tool a Shift-drag
    // puts a terminal among the returns of the port drawn last, one step of
    // Undo; a return that is not there is refused, by name.
    void aPortReturnsThroughTheTerminalsItNames()
    {
        QVERIFY(writeRectangles(path(QStringLiteral("gsgtips.gds")),
                                {QRectF(QPointF(0, -25), QPointF(600, 25)), QRectF(QPointF(0, 50), QPointF(600, 200)),
                                 QRectF(QPointF(0, -200), QPointF(600, -50))}));
        const QString model = fullWaveLine(QStringLiteral("gsgtips.qrf25"));
        QVERIFY(!model.isEmpty());
        {
            QFile f(model);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString why;
            std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
            QVERIFY2(read.has_value(), qPrintable(why));
            rf::Model m = *read;
            f.close();
            m.layout().props.insert(QStringLiteral("file"), QStringLiteral("gsgtips.gds"));
            m.ports().children.clear();
            const auto square = [](rf::Node& n, const char* x, const char* y) {
                n.props.insert(QStringLiteral("x"), QLatin1String(x));
                n.props.insert(QStringLiteral("y"), QLatin1String(y));
                n.props.insert(QStringLiteral("width"), QStringLiteral("20[um]"));
                n.props.insert(QStringLiteral("height"), QStringLiteral("20[um]"));
            };
            for (const auto& [x, label] : {std::make_pair("40[um]", "P1"), std::make_pair("560[um]", "P2")}) {
                rf::Node port = m.make(QStringLiteral("rf_internal_port"));
                port.label = QLatin1String(label);
                port.props.insert(QStringLiteral("to"), QStringLiteral("M1"));
                port.props.insert(QStringLiteral("return"), QStringLiteral("terminals"));
                square(port, x, "0");
                if (port.label == QLatin1String("P1"))
                    port.props.insert(QStringLiteral("returns"), QJsonArray::fromStringList({QStringLiteral("T1"), QStringLiteral("T2")}));
                else
                    port.props.insert(QStringLiteral("returns"), QStringLiteral("T3, T4"));
                QVERIFY(m.add(QStringLiteral("ports"), port) != nullptr);
            }
            for (const auto& [x, y] : {std::make_pair("40[um]", "125[um]"), std::make_pair("40[um]", "-125[um]"),
                                       std::make_pair("560[um]", "125[um]"), std::make_pair("560[um]", "-125[um]")}) {
                rf::Node t = m.make(QStringLiteral("rf_terminal"));
                t.props.insert(QStringLiteral("metal"), QStringLiteral("M1"));
                square(t, x, y);
                QVERIFY(m.add(QStringLiteral("ports"), t) != nullptr);
            }
            QCOMPARE(m.ports().children.back().label, QStringLiteral("T4"));
            rf::Node* study = m.root.child(QStringLiteral("rf_study"));
            QVERIFY(study != nullptr);
            study->props.insert(QStringLiteral("from"), QStringLiteral("10[GHz]"));
            study->props.insert(QStringLiteral("to"), QStringLiteral("10[GHz]"));
            study->props.insert(QStringLiteral("points"), 1);
            QVERIFY2(m.problems().filter(QStringLiteral("port")).isEmpty(), qPrintable(m.problems().join(" | ")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(m.toJson());
        }
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        LayoutView* view = doc->view();
        const auto overlay = [view](const QString& name) -> const LayoutView::Overlay* {
            for (const LayoutView::Overlay& o : view->overlays())
                if (o.name == name) return &o;
            return nullptr;
        };
        // The terminals' squares by their names, and a line from each port
        // to each of its returns.
        const LayoutView::Overlay* terminals = overlay(QStringLiteral("the terminals"));
        const LayoutView::Overlay* links = overlay(QStringLiteral("the ports' lines to their returns"));
        QVERIFY(terminals != nullptr && links != nullptr);
        QStringList named;
        for (const LayoutView::Overlay::Label& l : terminals->labels) named << l.text;
        QCOMPARE(named, (QStringList{"T1", "T2", "T3", "T4"}));
        QCOMPARE(terminals->shapes.size(), 4);
        QCOMPARE(terminals->shapes.first().boundingRect(), QRectF(QPointF(30, 115), QPointF(50, 135)));
        QCOMPARE(links->shapes.size(), 4);
        QCOMPARE(links->shapes.first(), QPolygonF(QList<QPointF>{QPointF(40, 0), QPointF(40, 125)}));

        doc->solve();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(w->portsSaid.value(0).contains(QStringLiteral("against T1 (")) && w->portsSaid.value(1).contains(QStringLiteral("and T4 ("))
                     && w->portsSaid.value(0).contains(QStringLiteral("tied: a ground-signal-ground probe's")),
                 qPrintable(w->portsSaid.join(" | ")));
        QVERIFY(w->namesReturns.value(0) && w->namesReturns.value(1));
        QVERIFY2(std::abs(std::abs(w->returnsOf.value(0).value(0).share) - 0.5) < 1e-6, qPrintable(QString::number(std::abs(w->returnsOf.value(0).value(0).share))));
        // The Touchstone's comments: what each drives, its tips' shares and
        // where its reference plane is.
        RF25Doc::ExportRequest what;
        QVERIFY(doc->exportRequest(doc->model().root.child(QStringLiteral("rf_exports"))->children.front().tag, &what));
        QString error;
        const QString touchstone = doc->exportText(what, &error);
        QVERIFY2(!touchstone.isEmpty(), qPrintable(error));
        QVERIFY2(touchstone.contains(QStringLiteral("! At 10 GHz P1's current comes back 50.0 % through T1, 50.0 % through T2."))
                     && touchstone.contains(QStringLiteral("P1's at the cells it drives and those it returns through, a source between them: the probe's own loop not in it")),
                 qPrintable(touchstone.left(3000)));
        // Kept beside the model with its returns, and read back with them.
        QString why;
        const std::optional<rf::KeptSolution> kept = rf::readSolution(rf::solutionPathOf(model), &why);
        QVERIFY2(kept && kept->fullWave, qPrintable(why));
        QCOMPARE(kept->fullWave->namesReturns, w->namesReturns);
        QCOMPARE(kept->fullWave->returnsOf.size(), 2);
        QCOMPARE(kept->fullWave->returnsOf.at(1).at(1).name, QStringLiteral("T4"));
        QCOMPARE(kept->fullWave->returnsOf.at(0).at(0).share, w->returnsOf.at(0).at(0).share);
        QCOMPARE(kept->fullWave->returnsOf.at(0).at(0).cells, w->returnsOf.at(0).at(0).cells);

        // A terminal renamed is renamed where a port names it.
        const QString t1 = doc->model().ports().children.at(2).tag;
        QVERIFY(panel()->renameNode(t1, QStringLiteral("G1")));
        QCOMPARE(rf::namesOf(doc->model().ports().children.at(0), QStringLiteral("returns")), (QStringList{"G1", "T2"}));
        doc->undo();
        QCOMPARE(rf::namesOf(doc->model().ports().children.at(0), QStringLiteral("returns")), (QStringList{"T1", "T2"}));
        // The panel's Returns: the list as a text, and a menu of the
        // terminals and the other port, each in or out.
        const QString p1 = doc->model().ports().children.at(0).tag;
        panel()->selectNode(p1);
        auto* returns = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("returns")));
        QVERIFY(returns != nullptr);
        QCOMPARE(returns->text(), QStringLiteral("T1, T2"));
        QVERIFY(panel()->editorFor(QStringLiteral("from")) == nullptr);   // (hidden: it returns through its list)
        auto* pick = panel()->findChild<QToolButton*>(QStringLiteral("rfPick_returns"));
        QVERIFY(pick != nullptr && pick->menu() != nullptr);
        QStringList offered;
        QAction* t2 = nullptr;
        QAction* p2 = nullptr;
        for (QAction* a : pick->menu()->actions()) {
            offered << a->text();
            if (a->data().toString() == QLatin1String("T2")) t2 = a;
            if (a->data().toString() == QLatin1String("P2")) p2 = a;
        }
        QCOMPARE(offered, (QStringList{"T1", "T2", "T3", "T4", "P2 (a port, in series)"}));
        QVERIFY(t2 != nullptr && t2->isChecked() && p2 != nullptr && !p2->isChecked());
        t2->setChecked(false);
        QCOMPARE(rf::namesOf(*doc->model().find(p1), QStringLiteral("returns")), (QStringList{"T1"}));
        doc->undo();
        QCOMPARE(rf::namesOf(*doc->model().find(p1), QStringLiteral("returns")), (QStringList{"T1", "T2"}));
        panel()->selectNode(p1);
        returns = qobject_cast<QLineEdit*>(panel()->editorFor(QStringLiteral("returns")));
        QVERIFY(returns != nullptr);
        returns->setText(QStringLiteral("T1 T9"));
        emit returns->editingFinished();
        QCOMPARE(rf::namesOf(*doc->model().find(p1), QStringLiteral("returns")), (QStringList{"T1", "T9"}));
        QVERIFY2(doc->model().problems().join(' ').contains(QStringLiteral("The port P1 returns through T9, which is no terminal or port of the model")),
                 qPrintable(doc->model().problems().join(" | ")));
        // Solved so, it is refused by name.
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->solveProblem().contains(QStringLiteral("P1 returns through T9, which is no terminal or port of the model")),
                 qPrintable(doc->solveProblem()));
        doc->undo();

        // The Internal Port tool: its rectangle, then a Shift-drag its tip.
        view->showRegion(QRectF(QPointF(-100, -250), QPointF(700, 250)));
        view->repaint();
        const int before = int(doc->model().ports().children.size());
        doc->internalPortButton()->click();
        QCOMPARE(view->mode(), LayoutView::Mode::PickBox);
        drag(view, view->toPixel(QPointF(290, -10)), view->toPixel(QPointF(310, 10)));
        QCOMPARE(int(doc->model().ports().children.size()), before + 1);
        const rf::Node drawn = doc->model().ports().children.back();
        QCOMPARE(drawn.type, QStringLiteral("rf_internal_port"));
        drag(view, view->toPixel(QPointF(290, 115)), view->toPixel(QPointF(310, 135)), Qt::ShiftModifier);
        QCOMPARE(int(doc->model().ports().children.size()), before + 2);
        const rf::Node tip = doc->model().ports().children.back();
        QCOMPARE(tip.type, QStringLiteral("rf_terminal"));
        QCOMPARE(tip.label, QStringLiteral("T5"));
        QCOMPARE(tip.props.value(QStringLiteral("metal")).toString(), QStringLiteral("M1"));
        QVERIFY2(std::abs(micrometres(tip, "y") - 125) < 4 && std::abs(micrometres(tip, "width") - 20) < 4,
                 qPrintable(QStringLiteral("%1, %2").arg(micrometres(tip, "y")).arg(micrometres(tip, "width"))));
        const rf::Node* owner = doc->model().find(drawn.tag);
        QVERIFY(owner != nullptr);
        QCOMPARE(rf::textOf(*owner, QStringLiteral("return")), QStringLiteral("terminals"));
        QCOMPARE(rf::namesOf(*owner, QStringLiteral("returns")), (QStringList{"T5"}));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("returns through T5")), qPrintable(doc->statusLabel()->text()));
        // A second tip, and one step of Undo takes it - and only it - away.
        drag(view, view->toPixel(QPointF(290, -135)), view->toPixel(QPointF(310, -115)), Qt::ShiftModifier);
        QCOMPARE(rf::namesOf(*doc->model().find(drawn.tag), QStringLiteral("returns")), (QStringList{"T5", "T6"}));
        doc->undo();
        QCOMPARE(int(doc->model().ports().children.size()), before + 2);
        QCOMPARE(rf::namesOf(*doc->model().find(drawn.tag), QStringLiteral("returns")), (QStringList{"T5"}));
        doc->undo();
        QCOMPARE(int(doc->model().ports().children.size()), before + 1);
        QCOMPARE(rf::textOf(*doc->model().find(drawn.tag), QStringLiteral("return")), QStringLiteral("from"));
        QTest::keyClick(view, Qt::Key_Escape);
        QCOMPARE(doc->placing(), RF25Doc::Placing::None);
    }

    void aCoplanarPortIsReadAndItsFeedCalibrated()
    {
        QVERIFY(writeRectangles(path(QStringLiteral("cpw.gds")),
                                {QRectF(QPointF(0, -50), QPointF(1000, 50)), QRectF(QPointF(0, 100), QPointF(1000, 350)),
                                 QRectF(QPointF(0, -350), QPointF(1000, -100))}));
        const QString model = fullWaveLine(QStringLiteral("cpw.qrf25"));
        QVERIFY(!model.isEmpty());
        {
            QFile f(model);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString why;
            std::optional<rf::Model> read = rf::Model::fromJson(f.readAll(), &why);
            QVERIFY2(read.has_value(), qPrintable(why));
            rf::Model m = *read;
            f.close();
            m.layout().props.insert(QStringLiteral("file"), QStringLiteral("cpw.gds"));
            std::vector<rf::Node>& stack = m.stackNode().children;
            for (rf::Node& n : stack)
                if (n.type == QLatin1String("rf_substrate")) n.props.insert(QStringLiteral("thickness"), QStringLiteral("200[um]"));
            for (rf::Node& port : m.ports().children) {
                port.props.insert(QStringLiteral("y"), QStringLiteral("0"));
                port.props.insert(QStringLiteral("return"), QStringLiteral("beside"));
                if (port.label != QLatin1String("P1")) continue;
                port.props.insert(QStringLiteral("calibrate"), true);
                // (Its strips' length given under the sanitizers: chosen - eight
                // heights and twice the feed, and shorter ones tried - their
                // dense solves took 60 s here and past the 300 s waited for on
                // CI's Linux, d7facd8.)
                if (slowBuild()) port.props.insert(QStringLiteral("strips"), QStringLiteral("400[um]"));
            }
            rf::Node* study = m.root.child(QStringLiteral("rf_study"));
            QVERIFY(study != nullptr);
            study->props.insert(QStringLiteral("from"), QStringLiteral("10[GHz]"));
            study->props.insert(QStringLiteral("to"), QStringLiteral("10[GHz]"));
            study->props.insert(QStringLiteral("points"), 1);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(m.toJson());
        }
        RF25Doc* doc = open(model);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        // Its signal's sides and its grounds' inner ones graded where they
        // face the 50 µm gaps, and the strips of its feed meshed as it is,
        // so that the cells at the port are the cells at its own.
        QCOMPARE(doc->mesh()->facing, 4);
        doc->solve();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(w->portsSaid.join(' ').contains(QStringLiteral("against the conductors beside it")),
                 qPrintable(w->portsSaid.join(" | ")));
        const QString said = w->lines.join(QStringLiteral(" | "));
        QVERIFY2(said.contains(QStringLiteral("Three strips of the coplanar feed")) && said.contains(QStringLiteral("no vias"))
                     && said.contains(QStringLiteral("P1 de-embedded")),
                 qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("The strips meshed as the structure is: 4 side(s) facing the feed's gaps")),
                 qPrintable(said));
        QVERIFY(w->deembedding.value(0).done && w->deembedding.value(0).calibrated && w->deembedding.value(0).coplanar);
        QVERIFY(!w->deembedding.value(1).done);
        // The same port on a lone strip: nothing beside it, said.
        const QString lone = fullWaveLine(QStringLiteral("lone.qrf25"));
        RF25Doc* single = open(lone);
        QVERIFY(single != nullptr);
        QVERIFY(single->waitForLayout(60000));
        for (const rf::Node& port : single->model().ports().children)
            QVERIFY(single->editNode(port.tag, [](rf::Node& n) { n.props.insert(QStringLiteral("return"), QStringLiteral("beside")); },
                                     QStringLiteral("return")));
        QVERIFY(single->waitForPrepared(60000));
        single->solve();
        QVERIFY(single->waitForSolved(300000));
        QVERIFY2(single->solveProblem().contains(QStringLiteral("none either side")), qPrintable(single->solveProblem()));
    }

    // Phase E in the tab: a full-wave study's mesh is cut for a solve held
    // compressed (its status says the compressed cost, not the charge
    // solve's), the study's Matrix - shown only for a full-wave study - says
    // how its system is held, and a solve asked for one way for itself
    // leaves the model as it was. Held compressed, its S is the dense one's.
    void aFullWaveStudysMatrixIsItsOwn()
    {
        const QString file = fullWaveLine(QStringLiteral("held.qrf25"));
        QVERIFY(!file.isEmpty());
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->mesh() != nullptr && doc->mesh()->cost.said.contains(QStringLiteral("held compressed")),
                 qPrintable(doc->mesh() != nullptr ? doc->mesh()->cost.said : QString()));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("held compressed")), qPrintable(doc->statusLabel()->text()));
        const rf::Node* study = doc->model().root.child(QStringLiteral("rf_study"));
        QVERIFY(study != nullptr);
        QCOMPARE(rf::textOf(*study, QStringLiteral("matrix")), QStringLiteral("automatic"));
        panel()->selectNode(study->tag);
        QVERIFY2(panel()->editorFor(QStringLiteral("matrix")) != nullptr, "no Matrix under a full-wave study");
        RF25Doc* fresh = app->newRf25Model();
        panel()->selectNode(fresh->model().root.child(QStringLiteral("rf_study"))->tag);
        QVERIFY2(panel()->editorFor(QStringLiteral("matrix")) == nullptr, "a Matrix under a quasi-static study");
        app->DocumentTab->setCurrentWidget(doc);

        // Automatic: this line is a few hundred unknowns, dense.
        doc->solveButton()->click();
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::FullWave> dense = doc->fullWave();
        QVERIFY2(dense != nullptr && dense->ok() && !dense->compressed, qPrintable(doc->solveProblem()));
        // Compressed, for one solve: the model left as it was.
        RF25Doc::SolveRequest held;
        held.matrix = QStringLiteral("compressed");
        doc->solveWith(held);
        QVERIFY(doc->waitForSolved(300000));
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok() && w->compressed, qPrintable(doc->solveProblem()));
        QCOMPARE(rf::textOf(*doc->model().root.child(QStringLiteral("rf_study")), QStringLiteral("matrix")), QStringLiteral("automatic"));
        QVERIFY2(says(w->lines, "The system held compressed"), qPrintable(w->lines.join(" | ")));
        for (int k = 0; k < w->frequencies.size(); ++k)
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) QVERIFY(std::abs(w->s[k][i][j] - dense->s[k][i][j]) < 2e-6);
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral("held compressed")),
                 qPrintable(doc->problemsText()->toPlainText()));
        // The study's own Matrix, dense: the mesh as the charge solve's, and
        // the solve dense.
        QVERIFY(doc->editNode(study->tag, [](rf::Node& n) { n.props.insert(QStringLiteral("matrix"), QStringLiteral("dense")); },
                              QStringLiteral("dense")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(!doc->mesh()->cost.said.contains(QStringLiteral("held compressed")), qPrintable(doc->mesh()->cost.said));
        doc->solveButton()->click();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok() && !doc->fullWave()->compressed, qPrintable(doc->solveProblem()));
        QVERIFY(doc->editNode(study->tag, [](rf::Node& n) { n.props.insert(QStringLiteral("matrix"), QStringLiteral("compressed")); },
                              QStringLiteral("compressed")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solveButton()->click();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok() && doc->fullWave()->compressed, qPrintable(doc->solveProblem()));
    }

    void aFullWaveStudySolvesThePortsAndDrawsTheCurrents()
    {
        const QString file = fullWaveLine(QStringLiteral("wave.qrf25"));
        QVERIFY(!file.isEmpty());
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->mesh() != nullptr && !doc->mesh()->cells.isEmpty(), qPrintable(doc->layoutProblem()));
        QVERIFY(doc->studyIsFullWave());
        panel()->selectNode(doc->model().root.child(QStringLiteral("rf_results"))->tag);
        QVERIFY2(reportSays(QStringLiteral("the ports' S")), qPrintable(reportText()));

        doc->solveButton()->click();
        QVERIFY(doc->isSolving());
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        const std::shared_ptr<const rf::FullWave> w = doc->fullWave();
        QVERIFY2(w != nullptr && w->ok(), qPrintable(doc->solveProblem()));
        QVERIFY(doc->extraction() == nullptr);
        QCOMPARE(w->frequencies, (QList<double>{1e9, 3e9, 5e9}));
        QCOMPARE(w->portNames, (QStringList{QStringLiteral("P1"), QStringLiteral("P2")}));
        QCOMPARE(w->references, (QList<double>{50, 50}));
        // A 50 Ohm-ish line of a millimetre: it passes nearly everything,
        // reciprocally and passively.
        const double through = std::abs(w->s.last()[1][0]);
        QVERIFY2(through > 0.8 && through <= 1, qPrintable(QString::number(through)));
        const QStringList summary = doc->fullWaveSummary();
        QVERIFY2(says(summary, "Full wave at 3 frequency") && says(summary, "S2,1") && says(summary, "Reciprocal")
                     && says(summary, "Passive"),
                 qPrintable(summary.join(" | ")));
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral("What it solves to")),
                 qPrintable(doc->problemsText()->toPlainText()));
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("full wave, 2 port(s) at 3 frequencies")),
                 qPrintable(doc->statusLabel()->text()));
        QCoreApplication::processEvents();
        QVERIFY2(reportSays(QStringLiteral("S2,1")), qPrintable(reportText()));

        // The currents: drawn from the panel, of a port and a frequency it chooses.
        auto* shown = panel()->settingsArea()->findChild<QCheckBox*>(QStringLiteral("rfCurrentsShown"));
        auto* port = panel()->settingsArea()->findChild<QComboBox*>(QStringLiteral("rfCurrentsPort"));
        auto* at = panel()->settingsArea()->findChild<QComboBox*>(QStringLiteral("rfCurrentsFrequency"));
        QVERIFY(shown != nullptr && port != nullptr && at != nullptr);
        QCOMPARE(port->count(), 2);
        QCOMPARE(at->count(), 3);
        doc->setCurrentsShown(false);
        QVERIFY(doc->view()->legend().isEmpty());
        doc->setCurrentsShown(true);
        int filled = 0;
        for (const qucs_s::layout::LayoutView::Overlay& o : doc->view()->overlays())
            if (o.fill.isValid()) filled += int(o.shapes.size());
        QVERIFY2(filled > 0, "no cell filled by its current");
        QVERIFY2(doc->view()->legend().title.contains(QStringLiteral("P1")) && doc->view()->legend().title.contains(QStringLiteral("5 GHz")),
                 qPrintable(doc->view()->legend().title));
        QVERIFY2(doc->currentsSaid().contains(QStringLiteral("P1")), qPrintable(doc->currentsSaid()));
        doc->setCurrentsOf(1, 0);
        QVERIFY2(doc->view()->legend().title.contains(QStringLiteral("P2")) && doc->view()->legend().title.contains(QStringLiteral("1 GHz")),
                 qPrintable(doc->view()->legend().title));
        // Driven as a circuit drives it: the scale's top is the largest
        // density of a source of P2's own 50 Ohm, P1 ended in its.
        double most = 0;
        for (const double j : rf::currentDensity(*w, 0, 1, true)) most = std::max(most, j);
        QCOMPARE(doc->view()->legend().high, QString::number(most, 'g', 3));
        QVERIFY(doc->currentsButton() != nullptr && doc->currentsButton()->isChecked());

        // The Touchstone: the ports' 2-port at the frequencies solved.
        QString why;
        const QString text = doc->exportText(RF25Doc::ExportRequest(), &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QVERIFY2(text.contains(QStringLiteral("! Ports: 1 P1, 2 P2")) && text.contains(QStringLiteral("# Hz S RI R 50"))
                     && text.contains(QStringLiteral("Full wave")),
                 qPrintable(text.left(600)));
        int lines = 0;
        for (const QString& line : text.split(QLatin1Char('\n')))
            if (!line.isEmpty() && !line.startsWith(QLatin1Char('!')) && !line.startsWith(QLatin1Char('#'))) ++lines;
        QCOMPARE(lines, 3);
        const QString ts = doc->model().root.child(QStringLiteral("rf_exports"))->children.front().tag;
        QVERIFY2(doc->exportPath(ts).endsWith(QStringLiteral(".s2p")), qPrintable(doc->exportPath(ts)));

        // The other solver, for one solve: the model is left as it was.
        RF25Doc::SolveRequest quasi;
        quasi.solver = QStringLiteral("quasi_static");
        doc->solveWith(quasi);
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->extraction() != nullptr && doc->extraction()->ok(), qPrintable(doc->solveProblem()));
        QVERIFY(doc->fullWave() == nullptr);
        QVERIFY(doc->view()->legend().isEmpty());
        QVERIFY(doc->studyIsFullWave());
        // And a full-wave solve of one frequency alone, then one stopped
        // as soon as it began: nothing kept, nothing said wrongly.
        RF25Doc::SolveRequest one;
        one.solver = QStringLiteral("full_wave");
        one.frequencies = {2e9};
        doc->solveWith(one);
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->frequencies == QList<double>{2e9}, qPrintable(doc->solveProblem()));
        RF25Doc::SolveRequest many;
        for (int k = 1; k <= 30; ++k) many.frequencies << k * 1e9;
        doc->solveWith(many);
        QCOMPARE(doc->frequenciesToSolve(), 30);
        doc->stopSolving();
        QVERIFY(!doc->isSolving());
        QVERIFY(doc->fullWave() == nullptr || doc->fullWave()->frequencies == QList<double>{2e9});
        QCOMPARE(doc->frequenciesToSolve(), 0);

        // A port that asks for a de-embedding: its own discontinuity taken
        // out by two strips of its feed, its reference plane moved 20 µm in,
        // and said - in the Results, the solve's lines and the export.
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY(doc->fullWave() != nullptr && doc->fullWave()->ok());
        const QString p1 = doc->model().ports().children.front().tag;
        QVERIFY(doc->editNode(p1, [](rf::Node& n) { n.props.insert(QStringLiteral("deembed"), QStringLiteral("20[um]")); },
                              QStringLiteral("de-embed")));
        // (The edit throws the solve away: shapes that are no longer the
        //  ones solved are worse than none.)
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY(doc->fullWave() == nullptr);
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(says(doc->fullWaveSummary(), "P1 de-embedded") && says(doc->fullWaveSummary(), " in from its edge"),
                 qPrintable(doc->fullWaveSummary().join(" | ")));
        QVERIFY(doc->fullWave()->deembedding.value(0).done && !doc->fullWave()->deembedding.value(1).done);
        QCOMPARE(doc->fullWave()->deembedding.value(0).shift, 20e-6);
        QCOMPARE(doc->fullWave()->zSolved.size(), doc->fullWave()->z.size());
        QVERIFY2(says(doc->fullWave()->lines, "Three strips of the feed"), qPrintable(doc->fullWave()->lines.join(" | ")));
        {
            QString why;
            const QString text = doc->exportText(RF25Doc::ExportRequest(), &why);
            QVERIFY2(text.contains(QStringLiteral("P1's 20 µm in from its edge along its feed"))
                         && text.contains(QStringLiteral("P2's at the centres of the cells it drives")),
                     qPrintable(text.left(900)));
            // Touchstone 2.0 through the export's own Version: each port into
            // its own reference.
            const QString ts = doc->model().root.child(QStringLiteral("rf_exports"))->children.front().tag;
            QVERIFY(doc->editNode(ts, [](rf::Node& n) { n.props.insert(QStringLiteral("version"), QStringLiteral("2.0")); },
                                  QStringLiteral("version")));
            RF25Doc::ExportRequest asked;
            QVERIFY(doc->exportRequest(ts, &asked, &why) && asked.version2);
            QVERIFY2(doc->exportText(asked, &why).contains(QStringLiteral("[Reference] 50 50")), qPrintable(why));
        }
        // Strips of a length asked - shorter than eight heights, as for a
        // substrate thick beside the wavelength - solved as asked; a length
        // that is none, refused.
        QVERIFY(doc->editNode(p1, [](rf::Node& n) { n.props.insert(QStringLiteral("strips"), QStringLiteral("300[um]")); },
                              QStringLiteral("strips")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));
        QCOMPARE(doc->fullWave()->deembedding.value(0).length, 300.0);
        QCOMPARE(doc->fullWave()->deembedding.value(0).strips.size(), 1);
        QVERIFY(doc->editNode(p1, [](rf::Node& n) { n.props.insert(QStringLiteral("strips"), QStringLiteral("-1[um]")); },
                              QStringLiteral("no strips")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->solveProblem().contains(QStringLiteral("calibration strips are no length")), qPrintable(doc->solveProblem()));
        QVERIFY(doc->editNode(p1, [](rf::Node& n) { n.props.insert(QStringLiteral("strips"), QStringLiteral("0")); },
                              QStringLiteral("strips chosen")));
        QVERIFY(doc->waitForPrepared(60000));
        // Moved along a line given instead: no strips solved, the port's own
        // discontinuity left in, and said; a line that is none, refused.
        QVERIFY(doc->editNode(p1, [](rf::Node& n) {
            n.props.insert(QStringLiteral("along"), QStringLiteral("line"));
            n.props.insert(QStringLiteral("line_z0"), QStringLiteral("50"));
            n.props.insert(QStringLiteral("line_eps"), QStringLiteral("6.5"));
        }, QStringLiteral("along a line")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(says(doc->fullWaveSummary(), "along a given line of 50 Ohm") && says(doc->fullWaveSummary(), "discontinuity left in"),
                 qPrintable(doc->fullWaveSummary().join(" | ")));
        QVERIFY2(!says(doc->fullWave()->lines, "Three strips of the feed"), qPrintable(doc->fullWave()->lines.join(" | ")));
        QVERIFY(doc->editNode(p1, [](rf::Node& n) { n.props.insert(QStringLiteral("line_eps"), QStringLiteral("0.5")); },
                              QStringLiteral("no line")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->solveProblem().contains(QStringLiteral("is none")), qPrintable(doc->solveProblem()));
        QVERIFY(doc->editNode(p1, [](rf::Node& n) {
            n.props.insert(QStringLiteral("along"), QStringLiteral("feed"));
            n.props.insert(QStringLiteral("line_eps"), QStringLiteral("1"));
        }, QStringLiteral("along the feed")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));

        // A port's metal and the way it faces reach the solve: facing +y,
        // P1 drives the strip's long side, not its end; on a metal that is
        // not there, nothing.
        const int atTheEnd = int(doc->fullWave()->portsSaid.value(0).section(QLatin1Char(' '), 3, 3).toInt());
        QVERIFY(doc->editNode(p1, [](rf::Node& n) {
            n.props.insert(QStringLiteral("facing"), QStringLiteral("+y"));
            n.props.insert(QStringLiteral("deembed"), QStringLiteral("0"));
        }, QStringLiteral("facing")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));
        const int alongTheSide = int(doc->fullWave()->portsSaid.value(0).section(QLatin1Char(' '), 3, 3).toInt());
        QVERIFY2(atTheEnd > 0 && alongTheSide > 2 * atTheEnd,
                 qPrintable(QStringLiteral("%1 and %2 cells").arg(atTheEnd).arg(alongTheSide)));
        QVERIFY(doc->editNode(p1, [](rf::Node& n) {
            n.props.insert(QStringLiteral("facing"), QStringLiteral("auto"));
            n.props.insert(QStringLiteral("conductor"), QStringLiteral("M9"));
        }, QStringLiteral("no such metal")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->solveProblem().contains(QStringLiteral("on no cell")), qPrintable(doc->solveProblem()));

        // With no ports, a full-wave solve has nothing to drive.
        for (const rf::Node& p : std::vector<rf::Node>(doc->model().ports().children))
            QVERIFY(doc->editNode(p.tag, [](rf::Node& n) { n.enabled = false; }, QStringLiteral("off")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(60000));
        QVERIFY2(doc->solveProblem().contains(QStringLiteral("driven at its ports")), qPrintable(doc->solveProblem()));
    }

    // A solve shows how far it has got, in a bar at the end of the status
    // line, and none is there while nothing solves (the user's question of
    // 2 November: why has RF 2.5D no progress bar while it solves?). Busy
    // for the quasi-static extraction, which cannot count; the full-wave
    // solve's frequencies solved of those to solve, then its ports'
    // calibration strips'. It goes with a Stop, the button as it was, and
    // stays where an edit's mesh starts the solve again. It asks the
    // window for no width and moves nothing above it, and the status line
    // says what it said.
    // The solution kept beside the model (the assessment of 2 November: "the
    // solution is not kept with the model"). A full-wave solve of a model on
    // disk writes "<model>.solution", stamped with the model and its layout;
    // opened again, the model shows it once its mesh is made - no solve -
    // and says so: its account, its Touchstone, its currents and every term
    // of it those of the solve, and written back the same bytes. Edited and
    // saved, its layout written again, the file damaged: not taken, and why
    // said. A quasi-static solve is kept as well; an unsaved model's solve
    // only once it is saved.
    // The study's "Add points where it changes": a sweep of 41 points over a
    // line a third of a wavelength long at its top, its ports calibrated by
    // strips of their feeds, solved at a few of them and read at the rest -
    // the strips solved at those few alone -, within the tolerance of the
    // whole sweep solved; kept beside the model with which it solved; the
    // mesh said first; a sweep of 12 points or fewer solved whole.
    void anAdaptiveSweepSolvesFewOfItsFrequencies()
    {
        const QString file = fullWaveLine(QStringLiteral("adaptive.qrf25"));
        QVERIFY(!file.isEmpty());
        QFile::remove(rf::solutionPathOf(file));
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        rf::Model m = doc->model();
        rf::Node* study = m.root.child(QStringLiteral("rf_study"));
        QVERIFY(study != nullptr);
        study->props.insert(QStringLiteral("to"), QStringLiteral("41[GHz]"));
        study->props.insert(QStringLiteral("points"), 41);
        study->props.insert(QStringLiteral("adaptive"), true);
        for (rf::Node& port : m.ports().children)
            if (port.type == QLatin1String("rf_edge_port")) port.props.insert(QStringLiteral("calibrate"), true);
        QVERIFY(doc->setModel(m, QStringLiteral("adaptive")));
        QVERIFY(doc->waitForPrepared(60000));
        // Its Tolerance shown with it, and only with it.
        {
            const qucs_s::fem::NodeKind* kind = rf::nodeKind(QStringLiteral("rf_study"));
            QVERIFY(kind != nullptr);
            const qucs_s::fem::PropertyDef* tolerance = kind->property(QStringLiteral("adaptive_tolerance"));
            QVERIFY(tolerance != nullptr);
            QVERIFY(qucs_s::fem::propertyVisible(*doc->model().root.child(QStringLiteral("rf_study")), *tolerance, kind));
            rf::Node off = *doc->model().root.child(QStringLiteral("rf_study"));
            off.props.insert(QStringLiteral("adaptive"), false);
            QVERIFY(!qucs_s::fem::propertyVisible(off, *tolerance, kind));
        }
        doc->solve();
        QVERIFY(doc->waitForSolved(600000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));
        const rf::FullWave adapted = *doc->fullWave();
        QCOMPARE(adapted.frequencies.size(), 41);
        QVERIFY2(adapted.solvedFrequencies.size() >= 9 && adapted.solvedFrequencies.size() < 25,
                 qPrintable(QString::number(adapted.solvedFrequencies.size())));
        QCOMPARE(adapted.solvedFrequencies.first(), 1e9);
        QCOMPARE(adapted.solvedFrequencies.last(), 41e9);
        QVERIFY2(adapted.lines.first().startsWith(QStringLiteral("Solved on ")), qPrintable(adapted.lines.first()));
        QVERIFY2(adapted.lines.first().contains(QStringLiteral("3 across the narrowest conductor, 50 µm")), qPrintable(adapted.lines.first()));
        QVERIFY2(says(adapted.lines, "Adding points where the answer changes: "), qPrintable(adapted.lines.join('\n')));
        QVERIFY2(says(adapted.lines, "Its strips solved at the "), qPrintable(adapted.lines.join('\n')));
        QVERIFY(adapted.deembedding.value(0).calibrated);
        // The whole sweep, solved for this solve alone: the same S within a
        // little of the tolerance, at every frequency.
        RF25Doc::SolveRequest whole;
        whole.adaptive = 0;
        doc->solveWith(whole);
        QVERIFY(doc->waitForSolved(600000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));
        const rf::FullWave full = *doc->fullWave();
        QVERIFY(full.solvedFrequencies.isEmpty());
        QCOMPARE(full.frequencies, adapted.frequencies);
        double worst = 0;
        for (int k = 0; k < full.frequencies.size(); ++k)
            for (int r = 0; r < 2; ++r)
                for (int c = 0; c < 2; ++c) worst = std::max(worst, std::abs(full.s[k][r][c] - adapted.s[k][r][c]));
        QVERIFY2(worst < 5e-3, qPrintable(QString::number(worst)));
        qInfo("41 points: %d solved, S within %.2g of the whole sweep", int(adapted.solvedFrequencies.size()), worst);
        // Solved again as the study says, kept, and opened again: shown with
        // what it solved.
        doc->solve();
        QVERIFY(doc->waitForSolved(600000));
        const QList<double> solvedAt = doc->fullWave()->solvedFrequencies;
        QVERIFY(!solvedAt.isEmpty());
        QCOMPARE(doc->save(), 0);
        app->closeAllFiles();
        doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY2(doc->showsKeptSolution(), qPrintable(doc->keptSaid()));
        QCOMPARE(doc->fullWave()->solvedFrequencies, solvedAt);
        // Twelve points: too few to gain by it, solved whole and said.
        m = doc->model();
        m.root.child(QStringLiteral("rf_study"))->props.insert(QStringLiteral("points"), 12);
        QVERIFY(doc->setModel(m, QStringLiteral("twelve")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(600000));
        QVERIFY(doc->fullWave() != nullptr && doc->fullWave()->ok());
        QVERIFY(doc->fullWave()->solvedFrequencies.isEmpty());
        QVERIFY2(says(doc->fullWave()->lines, "The study's 12 frequencies are too few for adding points to save anything"),
                 qPrintable(doc->fullWave()->lines.join('\n')));
        // A Tolerance that is none is refused, and why said.
        m = doc->model();
        m.root.child(QStringLiteral("rf_study"))->props.insert(QStringLiteral("points"), 41);
        m.root.child(QStringLiteral("rf_study"))->props.insert(QStringLiteral("adaptive_tolerance"), QStringLiteral("2"));
        QVERIFY(doc->setModel(m, QStringLiteral("tolerance")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(60000));
        QVERIFY2(doc->solveProblem().startsWith(QStringLiteral("The study's Tolerance is to be a number between nought and one")),
                 qPrintable(doc->solveProblem()));
        doc->setDocChanged(false);
        app->closeAllFiles();
    }

    // RF 2.5D > Check Mesh Convergence: the line solved on meshes of 2, 4 and
    // 8 cells across, full-wave at the top of its sweep and quasi-statically,
    // each value's reading and verdict said; the solution shown left as it
    // was; counted on the bar as it goes; gone with an edit of the model.
    void aMeshConvergenceCheckSaysHowFarTheMeshIsFromTheLimit()
    {
        const QString file = fullWaveLine(QStringLiteral("converge.qrf25"));
        QVERIFY(!file.isEmpty());
        QFile::remove(rf::solutionPathOf(file));
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        // (Cells along the line as long as the largest cell lets them: it,
        //  not the cells across, sets their size, and a check refines it.)
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { n.props.insert(QStringLiteral("stretch"), QStringLiteral("20")); },
                              QStringLiteral("stretch")));
        QVERIFY(doc->waitForPrepared(60000));
        // What it checks unasked: a quarter, a half and all of the model's 3.
        QCOMPARE(doc->convergenceLevels(), (QList<int>{1, 2, 3}));
        QCOMPARE(rf::defaultLevels(10), (QList<int>{3, 5, 10}));
        QCOMPARE(rf::defaultLevels(2), (QList<int>{2, 4, 8}));
        QCOMPARE(rf::defaultLevels(0), (QList<int>{1, 2, 4}));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        const std::shared_ptr<const rf::FullWave> before = doc->fullWave();
        QVERIFY(before != nullptr && before->ok());
        // The menu's action, there and ready.
        QAction* action = nullptr;
        for (QAction* a : app->findChildren<QAction*>())
            if (a->objectName() == QLatin1String("rfConvergeAction")) action = a;
        QVERIFY(action != nullptr);
        RF25Doc::SolveRequest check;
        check.levels = {2, 4, 8};
        doc->holdSolves(true);
        doc->solveWith(check);
        QVERIFY(doc->isCheckingConvergence());
        int done = 0, of = 0;
        QVERIFY2(doc->solveCount(&done, &of).startsWith(QStringLiteral("Checking mesh convergence, mesh 1 of 3 (2 cells across): ")),
                 qPrintable(doc->solveCount(&done, &of)));
        doc->holdSolves(false);
        QVERIFY(doc->waitForSolved(600000));
        QVERIFY(!doc->isCheckingConvergence());
        const std::shared_ptr<const rf::MeshConvergence> c = doc->convergence();
        QVERIFY2(c != nullptr && c->ok(), c ? qPrintable(c->problems.join(' ')) : "none");
        QCOMPARE(doc->fullWave(), before);
        QVERIFY(c->fullWave);
        QCOMPARE(c->frequencies, QList<double>{5e9});
        QCOMPARE(c->levels.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(c->levels.at(i).asked, check.levels.at(i));
            QCOMPARE(c->levels.at(i).across, check.levels.at(i));
            QVERIFY(std::abs(c->levels.at(i).cell - 50.0 / check.levels.at(i)) < 1e-6 * 50);
        }
        QVERIFY(c->levels.at(0).cells < c->levels.at(1).cells && c->levels.at(1).cells < c->levels.at(2).cells);
        // The largest cell, 100 µm in the model, 150 and 37.5 at 2 and 8 across.
        QVERIFY2(c->levels.at(0).largest > 110 && c->levels.at(2).largest < 40,
                 qPrintable(QStringLiteral("%1 %2").arg(c->levels.at(0).largest).arg(c->levels.at(2).largest)));
        QStringList names;
        for (const rf::Watched& w : c->values) names << w.name;
        QCOMPARE(names, (QStringList{QStringLiteral("S11 at 5 GHz"), QStringLiteral("S21 at 5 GHz"), QStringLiteral("S22 at 5 GHz")}));
        for (const rf::Watched& w : c->values) {
            QCOMPARE(w.values.size(), 3);
            QVERIFY(w.verdict.kind != rf::Convergence::Kind::TwoMeshes);
        }
        QVERIFY2(c->lines.first().startsWith(QStringLiteral("Mesh convergence over 3 mesh(es) of 2, 4 and 8 cells across the narrowest conductor, each the model's own refined as a whole - 0.667, 1.33 and 2.67 times as fine, its largest cells, cells per wavelength and cells at the gaps with it, at 5 GHz: of 3 value(s), ")),
                 qPrintable(c->lines.first()));
        QVERIFY2(c->lines.at(1).startsWith(QStringLiteral("Mesh 1: 2 across, ")), qPrintable(c->lines.at(1)));
        QVERIFY2(c->lines.at(4).startsWith(QStringLiteral("S11 at 5 GHz: ")), qPrintable(c->lines.at(4)));
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral("Its mesh convergence:\n  • Mesh convergence over 3 mesh(es) of 2, 4 and 8")),
                 qPrintable(doc->problemsText()->toPlainText()));
        for (const QString& line : c->lines) qInfo("%s", qPrintable(line));
        // Quasi-statically: each net's capacitance, and the line's.
        RF25Doc::SolveRequest quasi = check;
        quasi.solver = QStringLiteral("quasi_static");
        doc->solveWith(quasi);
        QVERIFY(doc->waitForSolved(600000));
        const std::shared_ptr<const rf::MeshConvergence> q = doc->convergence();
        QVERIFY2(q != nullptr && q->ok(), q ? qPrintable(q->problems.join(' ')) : "none");
        QVERIFY(!q->fullWave);
        QVERIFY(q->frequencies.isEmpty());
        QVERIFY2(q->values.first().name.startsWith(QStringLiteral("C of ")), qPrintable(q->values.first().name));
        QCOMPARE(q->values.first().unit, QStringLiteral("fF"));
        QVERIFY2(q->lines.first().startsWith(QStringLiteral("Mesh convergence over 3 mesh(es) of 2, 4 and 8 cells across the narrowest conductor, each the model's own refined as a whole - 0.667, 1.33 and 2.67 times as fine, its largest cells, cells per wavelength and cells at the gaps with it: of ")),
                 qPrintable(q->lines.first()));
        // The capacitance converges as the cells do.
        QCOMPARE(int(q->values.first().verdict.kind), int(rf::Convergence::Kind::Converging));
        QCOMPARE(doc->fullWave(), before);
        // An edit of the mesh: the check is of another model, and goes.
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { n.props.insert(QStringLiteral("cells_across"), 4); },
                              QStringLiteral("across")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY(doc->convergence() == nullptr);
        doc->setDocChanged(false);
        app->closeAllFiles();
    }

    void theSolutionIsKeptBesideTheModel()
    {
        const QString file = fullWaveLine(QStringLiteral("kept.qrf25"));
        QVERIFY(!file.isEmpty());
        const QString kept = rf::solutionPathOf(file);
        QCOMPARE(kept, file + QStringLiteral(".solution"));
        QFile::remove(kept);
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(!QFileInfo::exists(kept) && doc->keptSaid().isEmpty() && !doc->showsKeptSolution(), "kept before a solve");
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));
        QVERIFY2(QFileInfo::exists(kept), "the solve not kept beside the model");
        const rf::FullWave solved = *doc->fullWave();
        const QStringList account = doc->fullWaveSummary();
        RF25Doc::ExportRequest asked;
        QString why;
        const QString touchstone = doc->exportText(asked, &why);
        QVERIFY2(why.isEmpty() && !touchstone.isEmpty(), qPrintable(why));
        // Its stamps: the model's JSON, the layout file's bytes, the mesh's
        // cells, the build.
        const std::optional<rf::KeptSolution> read = rf::readSolution(kept, &why);
        QVERIFY2(read.has_value(), qPrintable(why));
        QCOMPARE(read->modelHash, rf::solveStampOf(doc->model()));
        QFile layout(path(QStringLiteral("strip.gds")));
        QVERIFY(layout.open(QIODevice::ReadOnly));
        QCOMPARE(read->layoutHash, rf::stampOf(layout.readAll()));
        layout.close();
        QCOMPARE(read->cells, int(doc->mesh()->cells.size()));
        QCOMPARE(read->solvedBy, misc::buildName());
        QVERIFY2(read->solvedBy.startsWith(QStringLiteral(PACKAGE_VERSION)), qPrintable(read->solvedBy));
        QVERIFY(read->fullWave != nullptr && read->extraction == nullptr);
        // Written back, the same bytes: what is read is what was written.
        QFile first(kept);
        QVERIFY(first.open(QIODevice::ReadOnly));
        const QByteArray bytes = first.readAll();
        first.close();
        const QString again = path(QStringLiteral("again.solution"));
        QVERIFY2(rf::writeSolution(again, *read, &why), qPrintable(why));
        QFile second(again);
        QVERIFY(second.open(QIODevice::ReadOnly));
        QVERIFY2(second.readAll() == bytes, "written back, other bytes");
        second.close();

        // Opened again: shown with no solve, and said so - the account, the
        // Touchstone, the currents and every term the solve's.
        app->closeAllFiles();
        doc = open(file);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->showsKeptSolution() && !doc->isSolving(), qPrintable(doc->keptSaid()));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), "nothing shown");
        QVERIFY2(doc->keptSaid().startsWith(QStringLiteral("The solution kept beside the model is shown: solved by Qucs-S "))
                     && doc->keptSaid().endsWith(QStringLiteral("UTC. Solve to solve it again.")),
                 qPrintable(doc->keptSaid()));
        QVERIFY2(doc->problemsText()->toPlainText().contains(doc->keptSaid()), qPrintable(doc->problemsText()->toPlainText()));
        QCOMPARE(doc->fullWaveSummary(), account);
        QCOMPARE(doc->exportText(asked, &why), touchstone);
        const rf::FullWave& shown = *doc->fullWave();
        QCOMPARE(shown.frequencies, solved.frequencies);
        QCOMPARE(shown.s, solved.s);
        QCOMPARE(shown.z, solved.z);
        QCOMPARE(shown.currents, solved.currents);
        QCOMPARE(shown.portNames, solved.portNames);
        QCOMPARE(shown.references, solved.references);
        QCOMPARE(shown.lines, solved.lines);
        QCOMPARE(shown.warnings, solved.warnings);
        QCOMPARE(shown.portsSaid, solved.portsSaid);
        QCOMPARE(shown.viaCells, solved.viaCells);
        QCOMPARE(shown.ends, solved.ends);
        QCOMPARE(shown.geometry.size(), solved.geometry.size());
        for (int i = 0; i < solved.geometry.size(); ++i)
            QVERIFY(shown.geometry.at(i).from == solved.geometry.at(i).from && shown.geometry.at(i).to == solved.geometry.at(i).to
                    && shown.geometry.at(i).z0 == solved.geometry.at(i).z0 && shown.geometry.at(i).width == solved.geometry.at(i).width);
        QCOMPARE(shown.runs, solved.runs);
        QCOMPARE(shown.cells, solved.cells);
        QVERIFY(shown.asymmetry == solved.asymmetry && shown.gain == solved.gain && shown.secondWave == solved.secondWave);
        // A solve again is a solve, and says nothing of what was kept.
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY(!doc->showsKeptSolution() && doc->keptSaid().isEmpty());
        // An export added changes nothing the solver is given: the solve
        // stands, and once saved is still the one kept beside the model.
        {
            rf::Model m = doc->model();
            QVERIFY(m.add(QStringLiteral("exports"), m.make(QStringLiteral("rf_circuit"))) != nullptr);
            QVERIFY(doc->setModel(m, QStringLiteral("an export")));
        }
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), "an export added cleared the solve");
        QCOMPARE(doc->save(), 0);
        app->closeAllFiles();
        doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY2(doc->showsKeptSolution(), qPrintable(doc->keptSaid()));

        // Edited and saved with no solve after: not taken, and why said.
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { n.props.insert(QStringLiteral("maximum"), QStringLiteral("90[um]")); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY(doc->fullWave() == nullptr);
        QCOMPARE(doc->save(), 0);
        app->closeAllFiles();
        doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY2(!doc->showsKeptSolution() && doc->fullWave() == nullptr
                     && doc->keptSaid().contains(QStringLiteral("is of another version of it")),
                 qPrintable(doc->keptSaid()));
        // Solved again (kept), its layout then written again: not taken.
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY(rf::readSolution(kept, &why).has_value());
        app->closeAllFiles();
        QVERIFY(writeStrip(path(QStringLiteral("strip.gds")), 1000, 60));
        doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY2(!doc->showsKeptSolution() && doc->keptSaid().contains(QStringLiteral("another version of its layout file")),
                 qPrintable(doc->keptSaid()));
        app->closeAllFiles();
        QVERIFY(writeStrip(path(QStringLiteral("strip.gds")), 1000, 50));
        // Solved on a mesh of other cells - a newer mesher's, the model and
        // its layout the same: not taken, and why said.
        {
            std::optional<rf::KeptSolution> other = rf::readSolution(kept, &why);
            QVERIFY2(other.has_value(), qPrintable(why));
            other->cells += 1;
            // (The layout written again is other bytes - its date.)
            QFile now(path(QStringLiteral("strip.gds")));
            QVERIFY(now.open(QIODevice::ReadOnly));
            other->layoutHash = rf::stampOf(now.readAll());
            QVERIFY2(rf::writeSolution(kept, *other, &why), qPrintable(why));
        }
        doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY2(!doc->showsKeptSolution() && doc->keptSaid().contains(QStringLiteral("was solved on a mesh of")),
                 qPrintable(doc->keptSaid()));
        app->closeAllFiles();
        // Of another format - a later build's: not read, and why said.
        {
            QFile later(kept);
            QVERIFY(later.open(QIODevice::WriteOnly));
            QDataStream s(&later);
            s.setVersion(QDataStream::Qt_6_0);
            s.writeRawData("QRF25SOL", 8);
            s << quint32(4) << QByteArray("a later build's");
        }
        doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY2(!doc->showsKeptSolution() && doc->keptSaid().contains(QStringLiteral("a solution of another version of Qucs-S")),
                 qPrintable(doc->keptSaid()));
        app->closeAllFiles();
        // Damaged: not read, and why said.
        QFile broken(kept);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("QRF25SOL then nothing a solve wrote");
        broken.close();
        doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY2(!doc->showsKeptSolution() && doc->keptSaid().startsWith(QStringLiteral("The solution beside the model was not read:")),
                 qPrintable(doc->keptSaid()));

        // A quasi-static solve is kept the same way, its extraction shown again.
        RF25Doc::SolveRequest quasi;
        quasi.solver = QStringLiteral("quasi_static");
        doc->solveWith(quasi);
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->extraction() != nullptr && doc->extraction()->ok(), qPrintable(doc->solveProblem()));
        const QStringList lines = doc->lineSummary();
        app->closeAllFiles();
        doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        QVERIFY2(doc->showsKeptSolution() && doc->extraction() != nullptr && doc->fullWave() == nullptr, qPrintable(doc->keptSaid()));
        QCOMPARE(doc->lineSummary(), lines);

        // An unsaved model's solve: kept once it is saved, not before.
        const QByteArray before = rf::readSolution(kept, &why)->modelHash;
        QVERIFY(doc->editNode(doc->model().mesh().tag, [](rf::Node& n) { n.props.insert(QStringLiteral("maximum"), QStringLiteral("80[um]")); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY(doc->fullWave() != nullptr && doc->fullWave()->ok());
        QCOMPARE(rf::readSolution(kept, &why)->modelHash, before);   // (the model on disk is not the one solved)
        QCOMPARE(doc->save(), 0);
        QCOMPARE(rf::readSolution(kept, &why)->modelHash, rf::solveStampOf(doc->model()));
        QVERIFY(rf::readSolution(kept, &why)->fullWave != nullptr);
        app->closeAllFiles();
    }

    // An equivalent circuit written from the solve (the assessment of 2
    // November: "one-click equivalent-circuit export (Touchstone -> RLC
    // subcircuit, its fit error said)"): an Equivalent circuit node of the
    // line's full-wave solve writes a Qucs-S part beside the model, which
    // Qucs-S reads as a schematic and netlists - its R, L and C between its
    // ports' nets the circuit's own Y -; as a SPICE subcircuit the same; the
    // panel's Write says where and how well; nothing before a full-wave solve.
    void anEquivalentCircuitIsWrittenFromTheSolve()
    {
        const QString file = fullWaveLine(QStringLiteral("circuit.qrf25"));
        QVERIFY(!file.isEmpty());
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr && doc->waitForLayout(60000) && doc->waitForPrepared(60000));
        rf::Node node = doc->model().make(QStringLiteral("rf_circuit"));
        const QString tag = node.tag;
        {
            rf::Model m = doc->model();
            QVERIFY(m.add(QStringLiteral("exports"), node) != nullptr);
            QVERIFY(doc->setModel(m, QStringLiteral("an equivalent circuit")));
        }
        QVERIFY(doc->waitForPrepared(60000));
        QString why;
        QVERIFY(doc->exportText(tag, &why).isEmpty());
        QVERIFY2(why.contains(QStringLiteral("solve the model full-wave first")), qPrintable(why));
        doc->solve();
        QVERIFY(doc->waitForSolved(300000));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));
        const std::optional<rf::EquivalentCircuit> circuit = doc->equivalentCircuit(&why);
        QVERIFY2(circuit.has_value() && circuit->ports == 2, qPrintable(why));
        // (A millimetre of line at 1 to 5 GHz is short: a pi holds.)
        QVERIFY2(circuit->worst < 0.05, qPrintable(circuit->lines.join(" | ")));
        qWarning().noquote() << circuit->lines.join(QStringLiteral(" | "));

        // A part beside the model, read by Qucs-S and netlisted: between its
        // ports' nets, p1 and p2, the circuit's Y to its four figures.
        const QString part = doc->writeExport(tag, &why);
        QVERIFY2(part == path(QStringLiteral("circuit_%1.sch").arg(tag)), qPrintable(part + why));
        Schematic sch(nullptr, part);
        QVERIFY(sch.load());
        QVERIFY(QDir().mkpath(path(QStringLiteral("kernel"))));
        Ngspice kernel(&sch);
        kernel.setWorkdir(path(QStringLiteral("kernel")));
        kernel.SaveNetlist(path(QStringLiteral("kernel/net.cir")), false);
        QFile net(path(QStringLiteral("kernel/net.cir")));
        QVERIFY(net.open(QIODevice::ReadOnly));
        const QString netlist = QString::fromUtf8(net.readAll());
        int elements = 0;
        for (const double f : {1e9, 3e9, 5e9}) {
            const auto got = yOfNetlist(netlist, {QStringLiteral("p1"), QStringLiteral("p2")}, QStringLiteral("0"), 2 * M_PI * f, &elements);
            const auto want = yOfCircuit(*circuit, 2 * M_PI * f);
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    QVERIFY2(std::abs(got[i][j] - want[i][j]) <= 1e-3 * std::abs(want[i][j]),
                             qPrintable(QStringLiteral("Y%1%2 at %3 GHz: %4 vs %5; the netlist:\n%6")
                                            .arg(i + 1).arg(j + 1).arg(f / 1e9).arg(std::abs(got[i][j])).arg(std::abs(want[i][j])).arg(netlist)));
        }
        QVERIFY2(elements >= 3, qPrintable(netlist));

        // A SPICE subcircuit, its name as asked: the same circuit.
        {
            rf::Model m = doc->model();
            m.find(tag)->props.insert(QStringLiteral("file"), QStringLiteral("circuit.cir"));
            m.find(tag)->props.insert(QStringLiteral("name"), QStringLiteral("line one"));
            QVERIFY(doc->setModel(m, QStringLiteral("its file")));
        }
        QVERIFY(doc->waitForPrepared(60000));
        // (An export's file named changes nothing the solver is given: what
        //  was solved stands.)
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), "an export's file named cleared the solve");
        const QString spice = doc->writeExport(tag, &why);
        QVERIFY2(spice == path(QStringLiteral("circuit.cir")), qPrintable(spice + why));
        QFile cir(spice);
        QVERIFY(cir.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(cir.readAll());
        QVERIFY2(text.contains(QStringLiteral(".subckt line_one p1 p2 ref")) && text.contains(QStringLiteral("An equivalent circuit of circuit.qrf25")),
                 qPrintable(text));
        const auto got = yOfNetlist(text, {QStringLiteral("p1"), QStringLiteral("p2")}, QStringLiteral("ref"), 2 * M_PI * 3e9, &elements);
        const auto want = yOfCircuit(*doc->equivalentCircuit(), 2 * M_PI * 3e9);
        QVERIFY2(std::abs(got[0][1] - want[0][1]) <= 1e-5 * std::abs(want[0][1]), qPrintable(text));

        // The panel's Write: where, and how well.
        panel()->selectNode(tag);
        auto* write = panel()->findChild<QPushButton*>(QStringLiteral("rfWriteExport"));
        QVERIFY(write != nullptr);
        write->click();
        auto* said = panel()->findChild<QLabel*>(QStringLiteral("rfExportSaid"));
        QVERIFY(said != nullptr);
        QVERIFY2(said->text().startsWith(QStringLiteral("Written to ")) && said->text().contains(QStringLiteral("differs from the solve's")),
                 qPrintable(said->text()));
        // (Saved: closed with no question of saving waiting.)
        QCOMPARE(doc->save(), 0);
        app->closeAllFiles();
    }

    // How the time a solve should take yet is said: to five seconds under a
    // minute and a half, in minutes under an hour and a half, else hours and
    // minutes; a few seconds under ten.
    void theTimeLeftIsSaidRoundly()
    {
        QCOMPARE(RF25Doc::leftSaid(0.4), QStringLiteral("a few seconds left"));
        QCOMPARE(RF25Doc::leftSaid(9.9), QStringLiteral("a few seconds left"));
        QCOMPARE(RF25Doc::leftSaid(10), QStringLiteral("about 10 s left"));
        QCOMPARE(RF25Doc::leftSaid(42), QStringLiteral("about 40 s left"));
        QCOMPARE(RF25Doc::leftSaid(88), QStringLiteral("about 90 s left"));
        QCOMPARE(RF25Doc::leftSaid(90), QStringLiteral("about 2 min left"));
        QCOMPARE(RF25Doc::leftSaid(400), QStringLiteral("about 7 min left"));
        QCOMPARE(RF25Doc::leftSaid(5399), QStringLiteral("about 90 min left"));
        QCOMPARE(RF25Doc::leftSaid(5400), QStringLiteral("about 1 h 30 min left"));
        QCOMPARE(RF25Doc::leftSaid(4 * 3600 + 5 * 60), QStringLiteral("about 4 h 5 min left"));
    }

    void aSolveShowsHowFarItHasGot()
    {
        const QString file = fullWaveLine(QStringLiteral("progress.qrf25"));
        QVERIFY(!file.isEmpty());
        RF25Doc* doc = open(file);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(60000));
        QVERIFY(doc->waitForPrepared(60000));
        QProgressBar* bar = doc->progressBar();
        QVERIFY(bar != nullptr);
        QVERIFY(bar->isHidden());
        auto* splitter = doc->findChild<QSplitter*>(QStringLiteral("rfSplitter"));
        QVERIFY(splitter != nullptr);
        // (The tab's own layout: its layout() is the GDS file's.)
        QLayout* tab = static_cast<QWidget*>(doc)->layout();
        tab->activate();
        QCoreApplication::processEvents();
        const int wide = app->width(), high = splitter->height();

        // The quasi-static extraction, held: busy, once it has taken a moment.
        RF25Doc::SolveRequest quasi;
        quasi.solver = QStringLiteral("quasi_static");
        doc->holdSolves(true);
        doc->solveWith(quasi);
        QVERIFY(doc->isSolving());
        QVERIFY2(bar->isHidden(), "the bar flashed up at once");
        // (What the tab and the window ask while it solves, before the bar
        //  is shown: the button's Stop is narrower than its Solve, which its
        //  bar of buttons takes in at its next layout request. Those alone
        //  are sent - no timer's tick, which would show the bar.)
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        tab->activate();
        QVERIFY(bar->isHidden());
        const int least = doc->minimumSizeHint().width(), window = app->minimumSizeHint().width();
        QTRY_VERIFY(bar->isVisible());
        QCOMPARE(bar->minimum(), 0);
        QCOMPARE(bar->maximum(), 0);
        // (What it is doing, in words: held before it computes, starting.)
        QVERIFY2(doc->statusLabel()->text().endsWith(QStringLiteral("; ") + rf::extractStepSaid(rf::ExtractControl::Starting, 0, 0, 0)),
                 qPrintable(doc->statusLabel()->text()));
        QVERIFY2(bar->toolTip().startsWith(QStringLiteral("Solving quasi-statically: starting")), qPrintable(bar->toolTip()));
        tab->activate();
        QCoreApplication::processEvents();
        QVERIFY2(bar->geometry().left() > doc->statusLabel()->geometry().right() && bar->geometry().right() < doc->width(),
                 "the bar is not at the end of the status line");
        QCOMPARE(doc->minimumSizeHint().width(), least);
        QVERIFY2(app->minimumSizeHint().width() <= window && app->width() == wide,
                 qPrintable(QStringLiteral("the window asks %1 px, was %2; is %3 wide, was %4")
                                .arg(app->minimumSizeHint().width()).arg(window).arg(app->width()).arg(wide)));
        QCOMPARE(splitter->height(), high);
        doc->holdSolves(false);
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        QVERIFY(bar->isHidden());
        QVERIFY2(doc->extraction() != nullptr && doc->extraction()->ok(), qPrintable(doc->solveProblem()));

        // The full-wave solve, its frequencies allowed one at a time: the
        // bar counts them, as the status line does.
        doc->allowFrequencies(0);
        doc->solveButton()->click();
        QCOMPARE(doc->solveButton()->text(), QStringLiteral("Stop"));
        QTRY_VERIFY(bar->isVisible());
        QCOMPARE(bar->maximum(), 3);
        QCOMPARE(bar->value(), 0);
        // Several at a time, none counted until it ends: said, so that a
        // count standing at nought is not taken for a solve standing still
        // (the review of 2 November, W1: "0 of 11" throughout).
        if (QThread::idealThreadCount() > 1)
            QTRY_VERIFY2(doc->solvingSaid().contains(QStringLiteral(" at a time, each counted as it ends")), qPrintable(doc->solvingSaid()));
        // (The solver's waits bounded by qWaitFor: a QTRY that fails waits
        //  twice as long again to say whether it would have passed later.)
        doc->allowFrequencies(1);
        QVERIFY2(QTest::qWaitFor([doc] { return doc->frequenciesSolved() >= 1; }, 120000), "the frequency allowed not solved");
        QCOMPARE(doc->frequenciesSolved(), 1);
        QTRY_COMPARE(bar->value(), 1);
        QCOMPARE(bar->maximum(), 3);
        QVERIFY2(doc->statusLabel()->text().contains(QStringLiteral("; solving 1 of 3 frequencies")),
                 qPrintable(doc->statusLabel()->text()));
        QVERIFY2(bar->toolTip().contains(QStringLiteral("1 of 3")), qPrintable(bar->toolTip()));
        // How long it should take yet, at the rate it has gone, once a
        // second has gone (the assessment of 2 November: "no ... time
        // estimate"): in the status line, the notes and the bar's tool tip.
        // Its Stop beside the bar, shown with it.
        QVERIFY(doc->progressStop()->isVisible());
        QTest::qWait(1100);
        QTRY_VERIFY2(!doc->timeLeft().isEmpty(), "no time left said");
        const QRegularExpression said(QStringLiteral("^(a few seconds left|about \\d+ s left|about \\d+ min left|about \\d+ h \\d+ min left)$"));
        QVERIFY2(said.match(doc->timeLeft()).hasMatch(), qPrintable(doc->timeLeft()));
        QTRY_VERIFY2(doc->statusLabel()->text().contains(QStringLiteral("; solving 1 of 3 frequencies, ")
                                                         + doc->timeLeft().section(QLatin1Char(' '), 0, 0)),
                     qPrintable(doc->statusLabel()->text()));
        QTRY_VERIFY2(bar->toolTip().contains(QStringLiteral(" left")), qPrintable(bar->toolTip()));
        QVERIFY2(doc->problemsText()->toPlainText().contains(QStringLiteral(" left)")), qPrintable(doc->problemsText()->toPlainText()));
        doc->allowFrequencies(-1);
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        QVERIFY(bar->isHidden());
        QCOMPARE(doc->solveButton()->text(), QStringLiteral("Solve"));
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok(), qPrintable(doc->solveProblem()));

        // Stopped: the bar goes with the solve at once - by the Solve
        // button's Stop, and by the Stop beside the bar, which goes with it.
        doc->holdSolves(true);
        doc->solveButton()->click();
        QTRY_VERIFY(bar->isVisible());
        const int releases = misc::memoryReleases();
        doc->solveButton()->click();
        QVERIFY(!doc->isSolving());
        // And the memory its thread freed offered back to the system once
        // the thread has gone (the review of 2 November, W3: 18.9 GB held).
        doc->holdSolves(false);
        QVERIFY(doc->waitForSolvesToEnd(60000));
        QTRY_VERIFY2(misc::memoryReleases() > releases, "no release of the freed memory after the stopped solve's thread went");
        doc->holdSolves(true);
        QVERIFY(bar->isHidden() && doc->progressStop()->isHidden());
        QCOMPARE(doc->solveButton()->text(), QStringLiteral("Solve"));
        QTest::qWait(500);
        QVERIFY(bar->isHidden());
        doc->solveButton()->click();
        QTRY_VERIFY(bar->isVisible() && doc->progressStop()->isVisible());
        QVERIFY2(doc->progressStop()->height() <= doc->statusLabel()->fontMetrics().height() + 4, "the Stop is taller than the line");
        doc->progressStop()->click();
        QVERIFY(!doc->isSolving());
        QVERIFY(bar->isHidden() && doc->progressStop()->isHidden());
        QCOMPARE(doc->solveButton()->text(), QStringLiteral("Solve"));

        // An edit's mesh overtaking a solve going on starts it again on the
        // mesh: the bar stays, rather than going and coming back a moment
        // later.
        doc->solve();
        QTRY_VERIFY(bar->isVisible());
        QVERIFY(doc->editNode(doc->model().mesh().tag,
                              [](rf::Node& n) { n.props.insert(QStringLiteral("maximum"), QStringLiteral("90[um]")); },
                              QStringLiteral("largest cell")));
        QVERIFY(doc->waitForPrepared(60000));
        QVERIFY(doc->isSolving());
        QVERIFY2(bar->isVisible(), "the bar went when the solve was started again");
        doc->holdSolves(false);
        QVERIFY2(doc->waitForSolved(300000), "the solve ended");
        QVERIFY(bar->isHidden());

        // Both ports de-embedded, each by strips of a length of its own: two
        // calibrations after the sweep, each counted from nought - its three
        // strips at the sweep's three frequencies (a length asked, no shorter
        // ones beside them) - the bar saying which, and its count all that
        // they solve: allowed that many, the solve ends.
        const QString p1 = doc->model().ports().children.front().tag, p2 = doc->model().ports().children.back().tag;
        QVERIFY(doc->editNode(p1, [](rf::Node& n) {
            n.props.insert(QStringLiteral("deembed"), QStringLiteral("20[um]"));
            n.props.insert(QStringLiteral("strips"), QStringLiteral("300[um]"));
        }, QStringLiteral("de-embed P1")));
        QVERIFY(doc->editNode(p2, [](rf::Node& n) {
            n.props.insert(QStringLiteral("deembed"), QStringLiteral("20[um]"));
            n.props.insert(QStringLiteral("strips"), QStringLiteral("400[um]"));
        }, QStringLiteral("de-embed P2")));
        QVERIFY(doc->waitForPrepared(60000));
        doc->allowFrequencies(3);
        doc->solve();
        QVERIFY2(QTest::qWaitFor([doc] { return doc->statusLabel()->text().endsWith(QStringLiteral("; calibrating the ports")); },
                                 120000),
                 qPrintable(doc->statusLabel()->text()));
        // (Busy while the first strip is meshed and set up; then counted. The
        //  bar itself waited for, shown and counting the strips: the status
        //  line can say the ports are being calibrated before it does. This
        //  small model's sweep can end before the window, slow to come back
        //  from starting the solve's thread, starts the bar's quarter of a
        //  second - the macOS CI of v27.0.6 -, and a tick reads the solve for
        //  the bar just before it reads it for the status line.)
        QVERIFY2(QTest::qWaitFor([bar] { return bar->isVisible() && bar->maximum() > 0 && bar->toolTip().contains(QStringLiteral("(feed 1 of 2)")); },
                                 120000),
                 qPrintable(QStringLiteral("the strips' frequencies not counted: %1, of %2, \"%3\"")
                                .arg(bar->isVisible() ? QStringLiteral("shown") : QStringLiteral("hidden"))
                                .arg(bar->maximum())
                                .arg(bar->toolTip())));
        QCOMPARE(bar->maximum(), 9);
        QCOMPARE(bar->value(), 0);
        doc->allowFrequencies(1);
        QVERIFY2(QTest::qWaitFor([bar] { return bar->value() >= 1; }, 120000), "the strip's frequency allowed not counted");
        QCOMPARE(bar->value(), 1);
        QCOMPARE(bar->maximum(), 9);
        QVERIFY2(bar->toolTip().contains(QStringLiteral("1 of 9 frequencies of the strips")), qPrintable(bar->toolTip()));
        // The rest of the first: the second begins, from nought.
        doc->allowFrequencies(8);
        QVERIFY2(QTest::qWaitFor([bar] { return bar->toolTip().contains(QStringLiteral("(feed 2 of 2)")) && bar->maximum() > 0; },
                                 120000),
                 qPrintable(bar->toolTip()));
        QCOMPARE(bar->maximum(), 9);
        QCOMPARE(bar->value(), 0);
        doc->allowFrequencies(9);
        QVERIFY2(doc->waitForSolved(120000), "the strips solved more than the bar counted");
        QVERIFY(bar->isHidden());
        doc->allowFrequencies(-1);
        QVERIFY2(doc->fullWave() != nullptr && doc->fullWave()->ok() && doc->fullWave()->deembedding.value(0).done
                     && doc->fullWave()->deembedding.value(1).done,
                 qPrintable(doc->solveProblem()));
        QVERIFY2(says(doc->fullWave()->lines, "Three strips of the feed"), qPrintable(doc->fullWave()->lines.join(" | ")));
    }

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
        // HFSS 3D Layout's, each by a mark at its first use; what has no model
        // to act on off (the menu says so as it opens).
        QAction* hfssExport = app->rf25Menu()->findChild<QAction*>(QStringLiteral("rfHfssExportAction"));
        QAction* hfssImport = app->rf25Menu()->findChild<QAction*>(QStringLiteral("rfHfssImportAction"));
        QAction* compare = app->rf25Menu()->findChild<QAction*>(QStringLiteral("rfCompareAction"));
        QVERIFY(hfssExport != nullptr && hfssImport != nullptr && compare != nullptr);
        QVERIFY2(hfssExport->text().contains(QStringLiteral("HFSS\u2122 3D Layout")), qPrintable(hfssExport->text()));
        QVERIFY(hfssImport->text().contains(QStringLiteral("HFSS\u2122 3D Layout")));
        QVERIFY(hfssExport->statusTip().contains(QStringLiteral("Ansys\u00ae")));
        QMetaObject::invokeMethod(app->rf25Menu(), "aboutToShow");
        QVERIFY(hfssExport->isEnabled());
        QVERIFY(hfssImport->isEnabled());
        QVERIFY(!compare->isEnabled());   // (nothing solved)
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
        app->slotZoomIn();
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

    // RF 2.5D > Settings…: the dense LU (Automatic: Accelerate's LAPACK
    // where this macOS has it), how large a full wave is held dense, and a
    // sweep's memory - Automatic by default, kept and given to the solver
    // on OK and not on Cancel, given again at a start (main's
    // applySettings()); Automatic's values said, with the LU chosen;
    // QUCS_RF25_LU and QUCS_FEM_SOLVER=eigen said over the choice. The item
    // no role: on macOS "Settings…" would be taken to the application's
    // menu.
    void theSettingsHaveTheLuTheLimitAndTheMemory()
    {
        qunsetenv("QUCS_RF25_LU");
        qunsetenv("QUCS_FEM_SOLVER");
        const auto automatic = qScopeGuard([] {
            QucsSettingsFile().remove(QStringLiteral("RF25"));
            rf::setDenseLu(rf::DenseLu::Automatic);
            rf::setDenseLimit(0);
            rf::setSweepMemory(0);
        });
        QAction* item = app->rf25Menu()->findChild<QAction*>(QStringLiteral("rf25SettingsAction"));
        QVERIFY(item != nullptr);
        QCOMPARE(item->text(), QStringLiteral("Settings…"));
        QCOMPARE(item->menuRole(), QAction::NoRole);
        QVERIFY(app->rf25Menu()->actions().contains(item));
        // (Opened from the menu - modal: closed from inside.)
        bool opened = false;
        QTimer::singleShot(0, this, [&opened] {
            if (auto* d = qobject_cast<rf::RF25SettingsDialog*>(QApplication::activeModalWidget())) {
                opened = d->luBox() != nullptr && d->limitBox() != nullptr && d->memoryBox() != nullptr;
                d->reject();
            }
        });
        item->trigger();
        QVERIFY(opened);
        QCOMPARE(rf::RF25SettingsDialog::luWanted(), 0);
        QCOMPARE(rf::RF25SettingsDialog::limitWanted(), 0);
        QCOMPARE(rf::RF25SettingsDialog::memoryWanted(), 0.0);
        QCOMPARE(rf::denseLimit(), rf::automaticDenseLimit());
        QCOMPARE(rf::sweepMemory(), rf::automaticSweepMemory());
        const QString accelerated = QStringLiteral("Accelerate's LAPACK factorizes the dense systems.");
        {
            rf::RF25SettingsDialog d(app);
            QCOMPARE(d.luBox()->count(), 4);
            QCOMPARE(d.luBox()->itemData(3).toString(), QStringLiteral("openblas"));
            QCOMPARE(d.luBox()->itemText(3), rf::openBlasLuHere() ? QStringLiteral("OpenBLAS's LAPACK")
                                                                  : QStringLiteral("OpenBLAS's LAPACK - not in this build"));
            QCOMPARE(d.luBox()->currentIndex(), 0);
            QVERIFY(d.luBox()->itemText(0).startsWith(QLatin1String("Automatic")));
            QCOMPARE(d.limitBox()->value(), 0);
            QCOMPARE(d.limitBox()->text(), QStringLiteral("Automatic"));
            QCOMPARE(d.memoryBox()->value(), 0.0);
            QCOMPARE(d.memoryBox()->text(), QStringLiteral("Automatic"));
            const QString lu = d.luStatusLabel()->text();
            const QString byOpenBlas = QStringLiteral("OpenBLAS's LAPACK factorizes the dense systems (%1).").arg(rf::openBlasLuAbout());
            QVERIFY2(rf::accelerateLuHere()  ? lu == accelerated
                     : rf::openBlasLuHere() ? lu == byOpenBlas
                                            : lu.startsWith(QLatin1String("Eigen's LU factorizes the dense systems")),
                     qPrintable(lu));
            // OpenBLAS's chosen: said, where the build has it.
            d.luBox()->setCurrentIndex(3);
            QVERIFY2(rf::openBlasLuHere() ? d.luStatusLabel()->text() == byOpenBlas
                                          : d.luStatusLabel()->text().startsWith(QLatin1String("Eigen's LU factorizes the dense "
                                                                                               "systems: this build has no OpenBLAS")),
                     qPrintable(d.luStatusLabel()->text()));
            const QString size = d.sizeStatusLabel()->text();
            qInfo("%s", qPrintable(size));
            QVERIFY2(size.startsWith(QStringLiteral("Automatic: held dense up to %1 runs and cells.").arg(rf::automaticDenseLimit())),
                     qPrintable(size));
            QCOMPARE(rf::automaticDenseLimit(), 6000);
            QVERIFY2(size.contains(QStringLiteral("%1 GB (Automatic)").arg(rf::automaticSweepMemory() / 1e9, 0, 'f', 1)), qPrintable(size));
            // Eigen's chosen: said; Automatic's limit the same whichever LU.
            d.luBox()->setCurrentIndex(1);
            QCOMPARE(d.luStatusLabel()->text(), QStringLiteral("Eigen's LU factorizes the dense systems."));
            QCOMPARE(d.sizeStatusLabel()->text(), size);
            d.limitBox()->setValue(1500);
            d.memoryBox()->setValue(2.5);
            d.reject();   // (Cancel: nothing kept.)
        }
        QCOMPARE(rf::RF25SettingsDialog::luWanted(), 0);
        QCOMPARE(rf::RF25SettingsDialog::limitWanted(), 0);
        QCOMPARE(rf::RF25SettingsDialog::memoryWanted(), 0.0);
        QVERIFY(rf::denseLu() == rf::DenseLu::Automatic);
        QCOMPARE(rf::denseLimit(), rf::automaticDenseLimit());
        // OK: kept, and given to the solver. A system of 1500 (1000 runs and
        // 500 cells) is some 21 MB: 2.5 GB holds 119 at a time, as the
        // threads allow.
        {
            rf::RF25SettingsDialog d(app);
            d.luBox()->setCurrentIndex(1);
            d.limitBox()->setValue(1500);
            d.memoryBox()->setValue(2.5);
            QCOMPARE(d.sizeStatusLabel()->text(), QStringLiteral("Held dense up to 1500 runs and cells. A sweep's frequencies at once may "
                                                                 "hold 2.5 GB: 119 of that size at a time (some 21 MB each), as the "
                                                                 "threads allow."));
            d.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(d.result(), int(QDialog::Accepted));
        }
        QCOMPARE(rf::RF25SettingsDialog::luWanted(), 1);
        QCOMPARE(rf::RF25SettingsDialog::limitWanted(), 1500);
        QCOMPARE(rf::RF25SettingsDialog::memoryWanted(), 2.5);
        QVERIFY(rf::denseLu() == rf::DenseLu::Eigen);
        QCOMPARE(rf::denseLimit(), 1500);
        QCOMPARE(rf::sweepMemory(), 2.5e9);
        // As kept, given again at a start.
        rf::setDenseLu(rf::DenseLu::Automatic);
        rf::setDenseLimit(0);
        rf::setSweepMemory(0);
        rf::RF25SettingsDialog::applySettings();
        QVERIFY(rf::denseLu() == rf::DenseLu::Eigen);
        QCOMPARE(rf::denseLimit(), 1500);
        QCOMPARE(rf::sweepMemory(), 2.5e9);
        {
            rf::RF25SettingsDialog d(app);
            QCOMPARE(d.luBox()->currentIndex(), 1);
            QCOMPARE(d.limitBox()->value(), 1500);
            QCOMPARE(d.memoryBox()->value(), 2.5);
        }
        // OpenBLAS's kept as "openblas" and given, built with it or not (an
        // LU asked of a library not here is Eigen's).
        {
            rf::RF25SettingsDialog d(app);
            d.luBox()->setCurrentIndex(3);
            d.apply();
        }
        QCOMPARE(QucsSettingsFile().value(QStringLiteral("RF25/lu")).toString(), QStringLiteral("openblas"));
        QCOMPARE(rf::RF25SettingsDialog::luWanted(), 3);
        QVERIFY(rf::denseLu() == rf::DenseLu::OpenBlas);
        QVERIFY(rf::denseLuInEffect() == (rf::openBlasLuHere() ? rf::DenseLu::OpenBlas : rf::DenseLu::Eigen));
        // The environment's over the choice, said.
        qputenv("QUCS_RF25_LU", "accelerate");
        {
            rf::RF25SettingsDialog d(app);
            const QString lu = d.luStatusLabel()->text();
            QVERIFY2(lu.startsWith(QLatin1String("QUCS_RF25_LU=accelerate is set, whatever this says: ")), qPrintable(lu));
        }
        qunsetenv("QUCS_RF25_LU");
        qputenv("QUCS_FEM_SOLVER", "eigen");
        {
            rf::RF25SettingsDialog d(app);
            d.luBox()->setCurrentIndex(0);
            QCOMPARE(d.luStatusLabel()->text(), QStringLiteral("QUCS_FEM_SOLVER=eigen is set, whatever this says: Eigen's LU factorizes "
                                                               "the dense systems."));
        }
        qunsetenv("QUCS_FEM_SOLVER");
        // Back to Automatic, all three.
        {
            rf::RF25SettingsDialog d(app);
            d.luBox()->setCurrentIndex(0);
            d.limitBox()->setValue(0);
            d.memoryBox()->setValue(0);
            d.apply();
        }
        QVERIFY(rf::denseLu() == rf::DenseLu::Automatic);
        QCOMPARE(rf::denseLimit(), rf::automaticDenseLimit());
        QCOMPARE(rf::sweepMemory(), rf::automaticSweepMemory());
        QCOMPARE(rf::RF25SettingsDialog::limitWanted(), 0);
    }
};

QTEST_MAIN(TestRf25Doc)
#include "test_rf25_doc.moc"
