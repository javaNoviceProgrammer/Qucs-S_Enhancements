/*
 * test_layout_routing.cpp - the metal router in the application
 *                           (layoutrouting.h): Claude's route_* tools on a
 *                           GDS written here - with its tab and without,
 *                           and over stdio from qucs-s --mcp-server -, and
 *                           the layout tab's Route panel: nets clicked,
 *                           keep-outs and guides from the pen, a run shown
 *                           over the layout, a failed net marked, a finding
 *                           gone to, the routes committed and read back.
 *                           No time is asserted.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>

#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

#include <gdstk/gdstk.hpp>

#include <numbers>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "layout.h"
#include "layoutdoc.h"
#include "layoutrouterpanel.h"
#include "layoutrouting.h"
#include "main.h"
#include "markup.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"

using namespace qucs_s::layout;

namespace {

// TOP: pads (6 µm, on 1/0) for two nets crossing, P1's labelled, a pad of
// no net, and an obstacle on 30/0; a placed cell of metal in the way.
void writeChip(const QString& file, bool withRoutesCell = false)
{
    gdstk::Library lib = {};
    lib.init("CHIP", 1e-6, 1e-9);
    auto* top = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
    top->init("TOP");
    const auto rect = [](gdstk::Cell* c, double x0, double y0, double x1, double y1, uint32_t layer) {
        auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
        *p = gdstk::rectangle(gdstk::Vec2{x0, y0}, gdstk::Vec2{x1, y1}, gdstk::make_tag(layer, 0));
        c->polygon_array.append(p);
    };
    for (const auto& [x, y] : {std::pair{0.0, 50.0}, {100.0, 50.0}, {50.0, 0.0}, {50.0, 100.0}, {150.0, 50.0}}) rect(top, x - 3, y - 3, x + 3, y + 3, 1);
    rect(top, 70, 20, 80, 30, 30);   // an obstacle
    auto* label = static_cast<gdstk::Label*>(gdstk::allocate_clear(sizeof(gdstk::Label)));
    label->init("P1");
    label->origin = gdstk::Vec2{100, 50};
    label->tag = gdstk::make_tag(1, 0);
    label->magnification = 1;
    top->label_array.append(label);
    auto* block = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
    block->init("BLOCK");
    rect(block, 0, 0, 8, 8, 1);
    auto* ref = static_cast<gdstk::Reference*>(gdstk::allocate_clear(sizeof(gdstk::Reference)));
    ref->init(block);
    ref->origin = gdstk::Vec2{20, 70};
    top->reference_array.append(ref);
    lib.cell_array.append(top);
    lib.cell_array.append(block);
    if (withRoutesCell) {
        auto* routes = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
        routes->init("ROUTES");
        rect(routes, -10, 40, 160, 42, 1);   // an old route right across: left out
        auto* r2 = static_cast<gdstk::Reference*>(gdstk::allocate_clear(sizeof(gdstk::Reference)));
        r2->init(routes);
        top->reference_array.append(r2);
        lib.cell_array.append(routes);
    }
    lib.write_gds(QFile::encodeName(file).constData(), 0, nullptr);
    lib.free_all();
}

QJsonObject technology()
{
    return QJsonObject{
        {"name", "two metals"},
        {"grid", 1},
        {"metals", QJsonArray{QJsonObject{{"name", "M1"}, {"layer", "1/0"}, {"direction", "h"}}, QJsonObject{{"name", "M2"}, {"layer", "2/0"}, {"direction", "v"}}}},
        {"vias", QJsonArray{QJsonObject{{"name", "V1"}, {"layer", "3/0"}, {"between", QJsonArray{"M1", "M2"}}, {"size", 2}, {"enclosure", 1}, {"spacing", 2}}}},
        {"classes", QJsonArray{QJsonObject{{"name", "signal"}, {"width", 2}}, QJsonObject{{"name", "power"}, {"width", 4}}}},
        {"spacing", QJsonObject{{"same_layer", 2}, {"to_metal", 2}, {"to_terminal", 3}}},
        {"obstacles", QJsonArray{QJsonObject{{"name", "etch"}, {"layer", "30/0"}, {"clearance", 3}}}},
        {"area", QJsonObject{{"margin", 30}}}};
}

QJsonArray xy(double x, double y)
{
    return QJsonArray{x, y};
}

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

} // namespace

class TestLayoutRouting : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 120000) { return control->callNow(tool, args, timeoutMs); }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r) { return QJsonDocument::fromJson(text(r).toUtf8()).object(); }

    LayoutDoc* open(const QString& file)
    {
        if (!app->gotoPage(file)) return nullptr;
        auto* doc = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        if (doc != nullptr && !doc->waitForLoaded(120000)) return nullptr;
        return doc;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
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

    void cleanup() { app->closeAllFiles(); }

    // Claude, with no tab open: set up, routed, reported, committed, a
    // change routed alone, committed again in place - the cell replaced.
    void claudeRoutesALayoutWithTheTools()
    {
        const QString chip = path("chip.gds");
        writeChip(chip, true);
        QVERIFY(control->readOnlyTools().contains("route_report"));
        QVERIFY(!control->readOnlyTools().contains("route_commit"));

        // A technology with a key it does not know: refused, by its name.
        QJsonObject bad = technology();
        bad.insert("gird", 1);
        QJsonObject r = call("route_setup", {{"path", chip}, {"technology", bad}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("'gird'"), qPrintable(text(r)));
        // An argument the tool has not: refused before it runs.
        QVERIFY(failed(call("route_setup", {{"path", chip}, {"technolgy", technology()}})));

        QJsonArray crossPads, upPads, nets;
        crossPads.append(QJsonObject{{"text", "P1"}});
        upPads.append(xy(50, 100));
        nets.append(QJsonObject{{"name", "h"}, {"start", xy(0, 50)}, {"pads", crossPads}});
        nets.append(QJsonObject{{"name", "v"}, {"start", xy(50, 0)}, {"pads", upPads}, {"class", "signal"}});
        r = call("route_setup", {{"path", chip}, {"technology", technology()}, {"nets", nets}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("nets").toInt(), 2);
        QVERIFY2(o.value("problem").isUndefined(), qPrintable(text(r)));
        const QJsonArray terminals = o.value("terminals").toArray();
        QCOMPARE(terminals.size(), 4);
        // P1 read by its label, at the pad's middle.
        QCOMPARE(terminals.at(1).toObject().value("ends at").toArray(), xy(100, 50));
        // The old ROUTES left out, said; five pads and the placed block read.
        QVERIFY2(QJsonDocument(o.value("warnings").toArray()).toJson().contains("ROUTES"), qPrintable(text(r)));
        QCOMPARE(o.value("shapes read").toObject().value("metal").toInt(), 6);
        QVERIFY(QFileInfo::exists(chip + ".route.json"));

        r = call("route_run", {{"path", chip}, {"timeout", 120}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        o = json(r);
        QCOMPARE(o.value("state").toString(), QStringLiteral("done"));
        QCOMPARE(o.value("routed").toInt(), 2);
        QCOMPARE(o.value("clean").toBool(), true);
        QCOMPARE(o.value("findings count").toInt(), 0);
        QVERIFY(o.value("vias").toInt() >= 2);

        o = json(call("route_report", {{"path", chip}, {"what", "passes"}}));
        QVERIFY(!o.value("passes").toArray().isEmpty());
        o = json(call("route_report", {{"path", chip}, {"what", "net"}, {"net", "h"}}));
        QVERIFY(!o.value("runs (metal, x0, y0, x1, y1)").toArray().isEmpty());
        QCOMPARE(o.value("status").toString(), QStringLiteral("routed"));
        o = json(call("route_report", {{"path", chip}, {"what", "nets"}}));
        QCOMPARE(o.value("nets").toArray().size(), 2);
        QVERIFY(failed(call("route_report", {{"path", chip}, {"what", "everything"}})));

        // Where to? Asked; then a copy, read back, its job beside it.
        QVERIFY(failed(call("route_commit", {{"path", chip}})));
        r = call("route_commit", {{"path", chip}, {"save_as", "chip_routed.gds"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        o = json(r);
        QVERIFY(o.value("read back").toString().contains("shapes in the cell"));
        QVERIFY(QFileInfo::exists(path("chip_routed.gds.route.json")));
        QString error;
        auto back = read(path("chip_routed.gds"), &error);
        QVERIFY2(back, qPrintable(error));
        const int routes = back->cellIndex("ROUTES");
        QVERIFY(routes >= 0);
        QCOMPARE(back->cells.at(routes).shapes.size(), o.value("polygons").toInt());

        // v's pad moves: route the changes; h is kept.
        QJsonArray moved;
        moved.append(xy(50, 100));
        QJsonArray vAgain;
        vAgain.append(QJsonObject{{"name", "v"}, {"start", xy(50, 0)}, {"pads", moved}, {"class", "power"}});
        r = call("route_setup", {{"path", chip}, {"nets", vAgain}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("route_run", {{"path", chip}, {"mode", "changed"}});
        o = json(r);
        QCOMPARE(o.value("routed").toInt(), 2);
        QCOMPARE(o.value("kept from the last run").toInt(), 1);
        QCOMPARE(o.value("clean").toBool(), true);

        // In place, twice: one ROUTES, placed once, its old route replaced.
        for (int i = 0; i < 2; ++i) {
            r = call("route_commit", {{"path", chip}, {"in_place", true}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QVERIFY(json(r).value("replaced its earlier contents").toBool());
        }
        back = read(chip, &error);
        QVERIFY(back);
        int placements = 0;
        for (const Instance& in : back->cells.at(back->cellIndex("TOP")).instances) placements += in.cell == back->cellIndex("ROUTES");
        QCOMPARE(placements, 1);
        // Nothing running: route_stop says so.
        QCOMPARE(json(call("route_stop", {{"path", chip}})).value("stopped").toBool(), false);

        // A net that cannot be routed: a finding, and commit refused.
        QJsonArray boxed, lostPad;
        lostPad.append(xy(0, 90));
        boxed.append(QJsonObject{{"name", "lost"}, {"start", xy(150, 50)}, {"pads", lostPad}});
        QJsonArray fence;
        fence.append(QJsonObject{{"name", "fence"}, {"outline", QJsonArray{xy(140, 40), xy(160, 40), xy(160, 60), xy(140, 60), xy(140, 42), xy(142, 42),
                                                                         xy(142, 58), xy(158, 58), xy(158, 42), xy(140, 42)}},
                                 {"let_through", "none"}});
        call("route_setup", {{"path", chip}, {"nets", boxed}, {"keep_outs", fence}});
        o = json(call("route_run", {{"path", chip}, {"mode", "all"}}));
        QCOMPARE(o.value("failed").toInt(), 1);
        QCOMPARE(o.value("clean").toBool(), false);
        bool unroutable = false;
        for (const QJsonValue& f : o.value("findings").toArray()) unroutable = unroutable || f.toObject().value("kind").toString() == "unroutable";
        QVERIFY(unroutable);
        r = call("route_commit", {{"path", chip}, {"in_place", true}});
        QVERIFY(failed(r));
        QVERIFY(text(r).contains("unroutable"));
        QVERIFY(!failed(call("route_commit", {{"path", chip}, {"in_place", true}, {"allow_findings", true}})));

        // Pads paired by least distance (a proposal; not applied).
        call("route_setup", {{"path", chip}, {"remove_nets", QJsonArray{"lost"}}, {"remove_keep_outs", QJsonArray{"*"}}});
        o = json(call("route_assign_pads", {{"path", chip}}));
        QVERIFY(o.value("distance after µm").toDouble() <= o.value("distance before µm").toDouble());
        QCOMPARE(o.value("applied").toBool(), false);
    }

    // The tab's Route panel: technology, nets clicked, a pen ring a
    // keep-out and a pen line a guide, a run shown, a failed net marked,
    // a finding gone to, committed.
    void theRoutePanelRoutesAndCommits()
    {
        MainGuard guard(app);
        const QString chip = path("panel.gds");
        writeChip(chip);
        {
            QFile t(path("tech.json"));
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write(QJsonDocument(technology()).toJson());
        }
        LayoutDoc* doc = open(chip);
        QVERIFY(doc);
        QVERIFY(doc->routeButton());
        QVERIFY(!doc->routerPanel());
        doc->routeButton()->click();
        auto* panel = doc->routerPanel();
        QVERIFY(panel);
        QVERIFY(panel->isVisible());
        QCOMPARE(panel->setTechnology("tech.json"), QString());

        // Two nets, clicked: contact then pad.
        panel->beginNet();
        QCOMPARE(doc->view()->mode(), LayoutView::Mode::PickPoint);
        panel->addPoint({0, 50});
        panel->addPoint({100, 50});
        panel->finishNet();
        QCOMPARE(doc->view()->mode(), LayoutView::Mode::Select);
        panel->beginNet();
        panel->addPoint({50, 0});
        panel->addPoint({50, 100});
        panel->finishNet();
        QCOMPARE(panel->netTable()->rowCount(), 2);
        QCOMPARE(panel->session()->job.nets.size(), 2);

        // The pen: a ring round the left of the obstacle, a line from net2's contact.
        qucs_s::markup::Stroke ring;
        ring.on = doc->view()->drawingPart();
        ring.width = 0.5;
        for (int k = 0; k <= 24; ++k) ring.points << QPointF(120 + 8 * std::cos(k * std::numbers::pi / 12), 80 + 8 * std::sin(k * std::numbers::pi / 12));
        doc->view()->addStroke(ring);
        qucs_s::markup::Stroke line;
        line.on = ring.on;
        line.width = 0.5;
        line.points = {QPointF(50, 0), QPointF(40, 30), QPointF(30, 60)};
        doc->view()->addStroke(line);
        const auto [keeps, guides] = panel->takeStrokes();
        QCOMPARE(keeps, 1);
        QCOMPARE(guides, 1);
        QCOMPARE(panel->keepOutList()->count(), 1);
        QCOMPARE(panel->session()->job.nets.at(1).guide.size() >= 2, true);

        // Run all: each pass shown, then the routes over the layout.
        QSignalSpy shown(panel, &qucs_s::routing::RouterPanel::runShown);
        QCOMPARE(panel->run("all"), QString());
        QVERIFY(panel->cancelButton()->isEnabled());
        QVERIFY(shown.wait(120000));
        QVERIFY2(panel->statusLabel()->text().contains("2 routed"), qPrintable(panel->statusLabel()->text()));
        QVERIFY2(panel->statusLabel()->text().contains("Clean"), qPrintable(panel->statusLabel()->text()));
        QCOMPARE(panel->netTable()->item(0, 3)->text(), QStringLiteral("routed"));
        int drawn = 0;
        for (const LayoutView::Overlay& ov : doc->view()->overlays()) drawn += ov.shapes.size();
        QVERIFY(drawn >= 4);
        QVERIFY(panel->commitButton()->isEnabled());

        // A net boxed in by a keep-out: failed, marked, a finding to go to.
        auto& job = panel->session()->job;
        qucs_s::router::JobKeepOut fence;
        fence.name = "fence";
        fence.letThrough = qucs_s::router::KeepOut::LetThrough::None;
        fence.outline = {{140, 40}, {160, 40}, {160, 60}, {140, 60}, {140, 42}, {142, 42}, {142, 58}, {158, 58}, {158, 42}, {140, 42}};
        job.keepOuts.append(fence);
        qucs_s::router::JobNet lost;
        lost.name = "lost";
        qucs_s::router::JobTerminal a, b;
        a.at = {150, 50};
        a.hasPoint = true;
        b.at = {0, 90};
        b.hasPoint = true;
        lost.terminals = {a, b};
        job.nets.append(lost);
        panel->session()->jobChanged();
        QCOMPARE(panel->run("changed"), QString());
        QVERIFY(shown.wait(120000));
        QCOMPARE(panel->session()->result().failed(), 1);
        QCOMPARE(panel->netTable()->item(2, 3)->text(), QStringLiteral("failed"));
        bool marked = false;
        for (const LayoutView::Overlay& ov : doc->view()->overlays())
            for (const auto& l : ov.labels) marked = marked || l.text.contains("lost failed");
        QVERIFY(marked);
        QVERIFY(panel->findingList()->topLevelItemCount() >= 1);
        QTreeWidgetItem* item = panel->findingList()->topLevelItem(0);
        QCOMPARE(item->text(0), QStringLiteral("unroutable"));
        panel->findingList()->setCurrentItem(item);
        emit panel->findingList()->itemClicked(item, 0);
        // The view shows where it was stopped.
        QVERIFY(panel->statusLabel()->text().contains("lost"));

        // Committed: refused with findings unless allowed; the copy read back.
        QVERIFY(!panel->commit(path("panel_routed.gds"), false).isEmpty());
        QCOMPARE(panel->commit(path("panel_routed.gds"), true), QString());
        QString error;
        auto back = read(path("panel_routed.gds"), &error);
        QVERIFY2(back, qPrintable(error));
        QVERIFY(back->cellIndex("ROUTES") >= 0);

        // Hidden: the overlay goes with it.
        doc->routeButton()->click();
        QVERIFY(!panel->isVisible());
        QVERIFY(doc->view()->overlays().isEmpty());
    }

    // Pictures of the panel for the write-up, where QUCS_ROUTER_PICTURES
    // names a folder (run on the display: QT_QPA_PLATFORM=cocoa). A small
    // chip: three two-metal capacitors, twelve pads, a pen ring made a
    // keep-out, one net with nowhere to go.
    void pictures()
    {
        const QString out = qEnvironmentVariable("QUCS_ROUTER_PICTURES");
        if (out.isEmpty()) QSKIP("QUCS_ROUTER_PICTURES=<folder> to make them");
        MainGuard guard(app);
        const QString chip = path("demo.gds");
        {
            gdstk::Library lib = {};
            lib.init("DEMO", 1e-6, 1e-9);
            auto* top = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
            top->init("TOP");
            const auto rect = [&](double x0, double y0, double x1, double y1, uint32_t layer) {
                auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
                *p = gdstk::rectangle(gdstk::Vec2{x0, y0}, gdstk::Vec2{x1, y1}, gdstk::make_tag(layer, 0));
                top->polygon_array.append(p);
            };
            for (int c = 0; c < 3; ++c) {
                const double x0 = 120 + c * 160, y0 = 140;
                for (int k = 0; k < 9; ++k) {   // the buses' rows of cuts
                    rect(x0 + 2, y0 + 6 + k * 11, x0 + 6, y0 + 10 + k * 11, 3);
                    rect(x0 + 94, y0 + 6 + k * 11, x0 + 98, y0 + 10 + k * 11, 3);
                }
                for (uint32_t m : {1u, 2u}) {
                    rect(x0, y0, x0 + 8, y0 + 100, m);
                    rect(x0 + 92, y0, x0 + 100, y0 + 100, m);
                    for (int f = 0; f < 6; ++f) {
                        rect(x0 + 8, y0 + 4 + f * 16, x0 + 90, y0 + 8 + f * 16, m);
                        rect(x0 + 10, y0 + 12 + f * 16, x0 + 92, y0 + 16 + f * 16, m);
                    }
                }
            }
            for (int i = 0; i < 6; ++i) {
                rect(80 + i * 90 - 20, 0, 80 + i * 90 + 20, 40, 2);
                rect(80 + i * 90 - 20, 340, 80 + i * 90 + 20, 380, 2);
            }
            rect(300, 60, 340, 100, 30);   // an etched area
            lib.cell_array.append(top);
            lib.write_gds(QFile::encodeName(chip).constData(), 0, nullptr);
            lib.free_all();
            QFile t(path("demo_tech.json"));
            QVERIFY(t.open(QIODevice::WriteOnly));
            QJsonObject tech = technology();
            tech.insert("grid", 2);
            tech.insert("classes", QJsonArray{QJsonObject{{"name", "signal"}, {"width", 6}}, QJsonObject{{"name", "power"}, {"width", 10}}});
            tech.insert("vias", QJsonArray{QJsonObject{{"name", "V1"}, {"layer", "3/0"}, {"between", QJsonArray{"M1", "M2"}}, {"size", 4}, {"enclosure", 1}, {"spacing", 4}}});
            tech.insert("spacing", QJsonObject{{"same_layer", 4}, {"to_metal", 4}, {"to_terminal", 6}});
            t.write(QJsonDocument(tech).toJson());
        }
        LayoutDoc* doc = open(chip);
        QVERIFY(doc);
        doc->showRouting();
        auto* panel = doc->routerPanel();
        QCOMPARE(panel->setTechnology("demo_tech.json"), QString());
        auto& job = panel->session()->job;
        const auto net = [&](const QString& name, QPointF a, QPointF b, const QString& cls = QString()) {
            qucs_s::router::JobNet n;
            n.name = name;
            n.netClass = cls;
            qucs_s::router::JobTerminal s, t;
            s.at = a;
            s.hasPoint = true;
            t.at = b;
            t.hasPoint = true;
            n.terminals = {s, t};
            job.nets.append(n);
        };
        for (int c = 0; c < 3; ++c) {
            const double x0 = 120 + c * 160;
            net(QStringLiteral("c%1a").arg(c), {x0 + 4, 190}, {80.0 + (2 * c) * 90, c == 1 ? 360.0 : 20.0});
            net(QStringLiteral("c%1b").arg(c), {x0 + 96, 190}, {80.0 + (2 * c + 1) * 90, c == 1 ? 20.0 : 360.0}, c == 2 ? QStringLiteral("power") : QString());
        }
        net("lost", {320, 80}, {440, 360});   // its start in the etched area
        panel->session()->jobChanged();
        qucs_s::markup::Stroke ring;
        ring.on = doc->view()->drawingPart();
        ring.width = 1;
        ring.color = QColor(220, 30, 30);
        for (int k = 0; k <= 30; ++k) ring.points << QPointF(170 + 70 * std::cos(k * std::numbers::pi / 15), 300 + 22 * std::sin(k * std::numbers::pi / 15));
        doc->view()->addStroke(ring);
        panel->takeStrokes();
        QSignalSpy shown(panel, &qucs_s::routing::RouterPanel::runShown);
        QCOMPARE(panel->run("all"), QString());
        QVERIFY(shown.wait(120000));
        doc->view()->fit();
        QTest::qWait(300);
        QDir().mkpath(out);
        QVERIFY(doc->grab().save(out + "/route-panel.png"));
        if (panel->findingList()->topLevelItemCount() > 0) {
            QTreeWidgetItem* item = panel->findingList()->topLevelItem(0);
            panel->findingList()->setCurrentItem(item);
            emit panel->findingList()->itemClicked(item, 0);
            QTest::qWait(300);
            QVERIFY(doc->grab().save(out + "/route-finding.png"));
        }
    }

    // qucs-s --mcp-server: route_setup, route_run and route_report over
    // stdio, with no window on screen.
    void theToolsServeOverStdio()
    {
        const QString program = QStringLiteral(QUCS_BINARY);
        if (!QFileInfo(program).isExecutable()) QSKIP("qucs-s is not built next to the tests");
        QTemporaryDir home;
        const QString chip = home.filePath("stdio.gds");
        writeChip(chip);
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QUCS_CLAUDE", "/nonexistent/claude");
        env.insert("QUCS_NO_SHELL_ENV", "1");
        env.insert("QUCS_SETTINGS_DIR", home.filePath("settings"));
        env.insert("HOME", home.path());
        p.setProcessEnvironment(env);
        p.start(program, {"--mcp-server", "--workspace", home.path()});
        QVERIFY(p.waitForStarted(10000));
        QJsonArray nets, pads1, pads2;
        pads1.append(xy(100, 50));
        pads2.append(xy(50, 100));
        nets.append(QJsonObject{{"name", "h"}, {"start", xy(0, 50)}, {"pads", pads1}});
        nets.append(QJsonObject{{"name", "v"}, {"start", xy(50, 0)}, {"pads", pads2}});
        const QList<QJsonObject> messages{
            {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject()}}}},
            {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
            {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
            {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
             {"params", QJsonObject{{"name", "route_setup"}, {"arguments", QJsonObject{{"path", chip}, {"technology", technology()}, {"nets", nets}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "route_run"}, {"arguments", QJsonObject{{"path", chip}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
             {"params", QJsonObject{{"name", "route_commit"}, {"arguments", QJsonObject{{"path", chip}, {"save_as", "stdio_routed.gds"}}}}}}};
        QHash<int, QJsonObject> answers;
        for (const QJsonObject& m : messages) {
            p.write(QJsonDocument(m).toJson(QJsonDocument::Compact) + '\n');
            if (!m.contains("id")) continue;
            const int id = m.value("id").toInt();
            QElapsedTimer t;
            t.start();
            while (!answers.contains(id) && t.elapsed() < 120000) {
                p.waitForReadyRead(200);
                while (p.canReadLine()) {
                    const QJsonObject a = QJsonDocument::fromJson(p.readLine()).object();
                    if (a.contains("id")) answers.insert(a.value("id").toInt(), a);
                }
            }
            QVERIFY2(answers.contains(id), qPrintable(QStringLiteral("no answer to %1; stderr: %2").arg(id).arg(QString::fromUtf8(p.readAllStandardError()).right(800))));
        }
        p.closeWriteChannel();
        p.waitForFinished(20000);
        if (p.state() != QProcess::NotRunning) p.kill();
        QStringList listed;
        for (const QJsonValue& t : answers.value(2).value("result").toObject().value("tools").toArray()) listed << t.toObject().value("name").toString();
        for (const char* name : {"route_setup", "route_run", "route_report", "route_commit", "route_stop", "route_assign_pads"}) QVERIFY2(listed.contains(name), name);
        const auto structured = [&](int id) { return answers.value(id).value("result").toObject().value("structuredContent").toObject(); };
        const QJsonObject setup = structured(3);
        QVERIFY2(setup.value("terminals").toArray().size() == 4, qPrintable(QJsonDocument(answers.value(3)).toJson()));
        const QJsonObject run = structured(4);
        QVERIFY2(run.value("routed").toInt() == 2 && run.value("clean").toBool(), qPrintable(QJsonDocument(answers.value(4)).toJson()));
        QVERIFY2(structured(5).value("polygons").toInt() > 0, qPrintable(QJsonDocument(answers.value(5)).toJson()));
        QVERIFY(QFileInfo::exists(home.filePath("stdio_routed.gds")));
    }
};

QTEST_MAIN(TestLayoutRouting)
#include "test_layout_routing.moc"
