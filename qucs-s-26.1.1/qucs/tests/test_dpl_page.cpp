// A data display's page: its markers through its data read again by the
// toolbar's button, and what a pen's stroke on its diagrams is read as.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>

#include <cmath>

#include "config.h"
#include "datadisplay.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "diagrams/marker.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "extsimkernels/spicecompat.h"

class TestDplPage : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QJsonObject call(const QString& tool, const QJsonObject& args = {})
    {
        QJsonObject answer;
        bool done = false;
        control->callToolFor(7, tool, args, [&](const QJsonObject& r) {
            answer = r;
            done = true;
        });
        QElapsedTimer t;
        t.start();
        while (!done && t.elapsed() < 30000) QTest::qWait(5);
        return answer;
    }
    QString write(const QString& name, const QByteArray& text)
    {
        QFile f(dir.filePath("workspace/" + name));
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(text);
        return f.fileName();
    }
    // y = x² and z = -10 x over x = 0, 0.25, ... (n samples); with \a w,
    // w over x and p as well.
    static QByteArray dataset(int n, bool withW = true)
    {
        QByteArray s = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep x " + QByteArray::number(n) + ">\n";
        for (int k = 0; k < n; ++k) s += QByteArray::number(k * 0.25) + "\n";
        s += "</indep>\n<dep y x>\n";
        for (int k = 0; k < n; ++k) s += QByteArray::number(k * 0.25 * k * 0.25) + "\n";
        s += "</dep>\n<dep z x>\n";
        for (int k = 0; k < n; ++k) s += QByteArray::number(-2.5 * k) + "\n";
        s += "</dep>\n";
        if (withW) {
            s += "<indep p 3>\n0\n1\n2\n</indep>\n<dep w x p>\n";
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < n; ++k) s += QByteArray::number(std::sin(k * 0.2) * (j + 1)) + "\n";
            s += "</dep>\n";
        }
        return s;
    }
    static QByteArray emptyPage(const QString& data)
    {
        return ("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <DataSet=" + data + ">\n</Properties>\n"
                "<Symbol>\n</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
            .toUtf8();
    }
    // A line drawn with the pen from \a from through \a to, read.
    QJsonObject line(const QString& file, QPointF from, QPointF to)
    {
        QJsonArray through;
        for (int k = 1; k <= 10; ++k) through.append(QJsonArray{from.x() + (to.x() - from.x()) * k / 10, from.y() + (to.y() - from.y()) * k / 10});
        const QJsonObject r = call("send_input", {{"target", "canvas"}, {"path", file}, {"click", QJsonArray{from.x(), from.y()}}, {"drag_through", through}});
        const QJsonObject text = QJsonDocument::fromJson(r.value("content").toArray().at(0).toObject().value("text").toString().toUtf8()).object();
        return text.value("drawn").toArray().at(0).toObject();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        // (The traces named as the plain datasets written here name them.)
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
    }

    // A marker read again with data that is shorter for a while - a
    // shorter run, a dataset caught half written - is on its last sample
    // meanwhile, and back where it was put with the data back: it was
    // moved there for good, and saved so. Also a marker of a sweep over
    // two variables, and one whose variable is gone for a while.
    void aMarkerKeepsItsPlaceThroughShorterData()
    {
        write("m.dat", dataset(41));
        const QString file = write("m.dpl", emptyPage("m.dat"));
        QVERIFY(app->gotoPage(file, false, false));
        QVERIFY(!call("add_diagram", {{"path", file}, {"traces", QJsonArray{"y", "w"}}}).value("isError").toBool());
        QVERIFY(!call("add_marker", {{"path", file}, {"diagram", 1}, {"trace", "y"}, {"at", 9.5}}).value("isError").toBool());
        QVERIFY(!call("add_marker", {{"path", file}, {"diagram", 1}, {"trace", "w"}, {"at", 9.0}}).value("isError").toBool());
        Schematic* page = app->currentSchematic();
        QVERIFY(page != nullptr && page->a_DocDiags.size() == 1);
        const Diagram* d = page->a_DocDiags.front();
        QCOMPARE(d->Graphs.size(), 2);
        Marker* my = d->Graphs.at(0)->Markers.value(0);
        Marker* mw = d->Graphs.at(1)->Markers.value(0);
        QVERIFY(my != nullptr && mw != nullptr);
        QCOMPARE(my->varPos().front(), 9.5);
        QCOMPARE(mw->varPos().front(), 9.0);
        auto* reload = page->pageBar()->reloadButton();

        // Five samples, to x = 1: on the last.
        write("m.dat", dataset(5));
        reload->click();
        QCOMPARE(my->varPos().front(), 1.0);
        QVERIFY2(my->Text.contains("1.000"), qPrintable(my->Text));
        QCOMPARE(mw->varPos().front(), 1.0);
        // Its text says it is on the nearest sample, not where it was put.
        QVERIFY2(my->Text.contains("(nearest: put at x 9.500, beyond the data)"), qPrintable(my->Text));
        QVERIFY2(mw->Text.contains("(nearest: put at x 9.000, beyond the data)"), qPrintable(mw->Text));
        // Saved where it was put.
        QVERIFY2(my->save().startsWith("<Mkr 9.5 "), qPrintable(my->save()));
        // w gone for a while.
        write("m.dat", dataset(5, false));
        reload->click();
        QVERIFY(mw->Text.contains("invalid"));
        // All of it back: both where they were put.
        write("m.dat", dataset(41));
        reload->click();
        QCOMPARE(my->varPos().front(), 9.5);
        QVERIFY2(my->Text.contains("9.500") && my->Text.contains("90.250") && !my->Text.contains("nearest"), qPrintable(my->Text));
        QVERIFY2(!mw->Text.contains("nearest"), qPrintable(mw->Text));
        QCOMPARE(mw->varPos().front(), 9.0);
        QCOMPARE(mw->varPos().size(), size_t(2));

        // Moved by the user (a step to the right): put there - the next
        // short reload and back keep the new place.
        QVERIFY(my->moveLeftRight(false));
        QCOMPARE(my->varPos().front(), 9.75);
        write("m.dat", dataset(5));
        reload->click();
        write("m.dat", dataset(41));
        reload->click();
        QCOMPARE(my->varPos().front(), 9.75);
        // Given a place between two samples, inside the data: on the
        // nearest, as ever, and not said to be beyond it.
        my->setPos(9.3);
        my->createText();
        QCOMPARE(my->varPos().front(), 9.25);
        QVERIFY2(!my->Text.contains("nearest"), qPrintable(my->Text));
        page->setChanged(false);
        call("close_document", {{"path", file}, {"unsaved", "discard"}});
    }

    // A stroke across a stacked diagram's panes: the y values of each pane
    // it crosses, each on that pane's axis - one span of both panes' values
    // (x² in one, -10 x in the other) said nothing. In one pane, as before.
    void aStrokeAcrossStackedPanesReadsEachPane()
    {
        write("s.dat", dataset(41, false));
        const QString file = write("s.dpl", emptyPage("s.dat"));
        QVERIFY(app->gotoPage(file, false, false));
        const QJsonObject added = call("add_diagram", {{"path", file}, {"type", "stacked"}, {"x", 100}, {"y", 500}, {"width", 400}, {"height", 300},
                                                       {"traces", QJsonArray{QJsonObject{{"variable", "y"}, {"pane", 1}},
                                                                             QJsonObject{{"variable", "z"}, {"pane", 2}}}}});
        QVERIFY2(!added.value("isError").toBool(), qPrintable(QJsonDocument(added).toJson()));
        Schematic* page = app->currentSchematic();
        page->setPenActive(true);
        const Diagram* d = page->a_DocDiags.front();
        const double mid = d->cx + d->x2 / 2.0;
        // Top to bottom, inside the plot.
        QJsonObject stroke = line(file, QPointF(mid, d->cy - d->y2 + 5), QPointF(mid, d->cy - 5));
        QJsonObject in = stroke.value("in diagrams").toArray().at(0).toObject();
        QVERIFY2(!in.contains("y") && in.value("panes").toArray().size() == 2, qPrintable(QJsonDocument(in).toJson()));
        const QJsonArray top = in.value("panes").toArray().at(0).toObject().value("y").toArray();
        const QJsonArray bottom = in.value("panes").toArray().at(1).toObject().value("y").toArray();
        QCOMPARE(in.value("panes").toArray().at(0).toObject().value("pane").toInt(), 1);
        // (Each axis a little beyond its data: x² from 0 to 100, -10 x from -100 to 0.)
        QVERIFY2(top.size() == 2 && top.at(0).toDouble() > -30 && top.at(1).toDouble() > 50, qPrintable(QJsonDocument(in).toJson()));
        QVERIFY2(bottom.size() == 2 && bottom.at(0).toDouble() < -50 && bottom.at(1).toDouble() < 30, qPrintable(QJsonDocument(in).toJson()));
        QVERIFY2(qucs_s::datadisplay::sentencesOf(QJsonArray{stroke}).contains(" in pane 1, y "), qPrintable(qucs_s::datadisplay::sentencesOf(QJsonArray{stroke})));
        // Within the top pane: one y, and its pane.
        stroke = line(file, QPointF(d->cx + 20, d->cy - d->y2 + 40), QPointF(d->cx + 110, d->cy - d->y2 + 60));   // (away from the first: not its arrowhead)
        in = stroke.value("in diagrams").toArray().at(0).toObject();
        QVERIFY2(in.contains("y") && !in.contains("panes") && in.value("pane").toInt() == 1, qPrintable(QJsonDocument(in).toJson()));
        page->setPenActive(false);
        page->setChanged(false);
        call("close_document", {{"path", file}, {"unsaved", "discard"}});
    }

    // A data display saved under another name, or renamed, still shows the
    // data it showed: it was pointed at a dataset of its new name (none),
    // its traces empty from the next reload on, and so saved. A schematic
    // saved so is pointed at its new name's, which its next run writes.
    void aDataDisplaySavedAsKeepsItsData()
    {
        write("data.dat", dataset(41, false));
        QByteArray text = emptyPage("data.dat");
        text.replace("<Diagrams>\n", "<Diagrams>\n  <Rect 100 400 300 200 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 \"\" \"\" \"\">\n"
                                     "    <\"y\" #0000ff 2 3 0 0 0>\n  </Rect>\n");
        const QString file = write("view.dpl", text);
        QVERIFY(app->gotoPage(file, false, false));
        Schematic* page = app->currentSchematic();
        QVERIFY(page != nullptr);
        QCOMPARE(page->getDataSet(), QStringLiteral("data.dat"));
        const QString display = page->getDataDisplay();
        const auto drawn = [page] {
            const Graph* g = page->a_DocDiags.front()->Graphs.front();
            return g->cPointsY != nullptr && g->axis(0) != nullptr ? int(g->axis(0)->count) : 0;
        };
        QCOMPARE(drawn(), 41);
        QVERIFY(app->saveDocumentAs(page, dir.filePath("workspace/view_copy.dpl")));
        QCOMPARE(page->getDataSet(), QStringLiteral("data.dat"));
        QCOMPARE(page->getDataDisplay(), display);
        page->pageBar()->reloadButton()->click();
        QCOMPARE(drawn(), 41);
        // Renamed in the file tree.
        QVERIFY(QFile::rename(dir.filePath("workspace/view_copy.dpl"), dir.filePath("workspace/plots.dpl")));
        app->documentsMoved({dir.filePath("workspace/view_copy.dpl")}, {dir.filePath("workspace/plots.dpl")});
        QCOMPARE(page->getDataSet(), QStringLiteral("data.dat"));
        page->setChanged(false);
        call("close_document", {{"path", dir.filePath("workspace/plots.dpl")}, {"unsaved", "discard"}});
        // A schematic saved under another name: its new name's dataset.
        const QString sch = write("amp.sch", emptyPage("amp.dat"));
        QVERIFY(app->gotoPage(sch, false, false));
        Schematic* s = app->currentSchematic();
        QVERIFY(app->saveDocumentAs(s, dir.filePath("workspace/amp2.sch")));
        QCOMPARE(s->getDataSet(), QStringLiteral("amp2.dat"));
        QCOMPARE(s->getDataDisplay(), QStringLiteral("amp2.dpl"));
        s->setChanged(false);
        call("close_document", {{"path", dir.filePath("workspace/amp2.sch")}, {"unsaved", "discard"}});
    }

    // A span ending at an axis' zero reads 0 there, not the arithmetic's
    // -4.857e-17.
    void aSpanToZeroReadsZero()
    {
        write("n.dat", dataset(41, false));
        const QString file = write("n.dpl", emptyPage("n.dat"));
        QVERIFY(app->gotoPage(file, false, false));
        QVERIFY(!call("add_diagram", {{"path", file}, {"x", 100}, {"y", 500}, {"width", 300}, {"height", 200}, {"traces", QJsonArray{"y"}},
                                      {"x_axis", QJsonObject{{"auto", false}, {"from", -1}, {"to", 1}, {"step", 0.5}}},
                                      {"y_axis", QJsonObject{{"auto", false}, {"from", -1}, {"to", 1}, {"step", 0.5}}}})
                     .value("isError")
                     .toBool());
        Schematic* page = app->currentSchematic();
        page->setPenActive(true);
        const Diagram* d = page->a_DocDiags.front();
        const QJsonObject stroke = line(file, QPointF(d->cx - 20, d->cy + 20), QPointF(d->cx + d->x2 / 2.0, d->cy - d->y2 / 2.0));
        const QJsonObject in = stroke.value("in diagrams").toArray().at(0).toObject();
        const QJsonArray x = in.value("x").toArray(), y = in.value("y").toArray();
        QVERIFY2(x.size() == 2 && x.at(1).toDouble() == 0.0 && y.size() == 2 && y.at(1).toDouble() == 0.0, qPrintable(QJsonDocument(in).toJson()));
        page->setPenActive(false);
        page->setChanged(false);
        call("close_document", {{"path", file}, {"unsaved", "discard"}});
    }
};

QTEST_MAIN(TestDplPage)

#include "test_dpl_page.moc"
