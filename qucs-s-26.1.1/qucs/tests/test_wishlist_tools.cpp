/*
 * test_wishlist_tools.cpp - Claude's wishlist of 2 October 2026: the Tools
 * menu's synthesis and calculation programs as tools (filters, attenuators,
 * matching, power combiners, line calculation, a receiver's budget); texts
 * drawn over something, noted and moved clear; two-pin parts placed by
 * their pin 1; the manual offline; the workspace's projects; a kept run's
 * equations compared; settings found by key and word.
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
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <memory>

#include "components/component.h"
#include "config.h"
#include "diagrams/diagram.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "textplacement.h"
#include "wire.h"

class TestWishlistTools : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 60000)
    {
        return control->callNow(tool, args, timeoutMs);
    }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r)
    {
        QString first;
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                first = v.toObject().value(QStringLiteral("text")).toString();
                break;
            }
        return QJsonDocument::fromJson(first.toUtf8()).object();
    }
    Schematic* front() const { return app->currentSchematic(); }
    // The netlist a simulation of \a doc would be given now.
    QString netlistOf(const QString& doc) { return text(call("get_netlist", {{"path", doc}})); }
    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    // The notes check_schematic gives of texts drawn over something.
    QStringList overlapNotes(const QString& doc)
    {
        QStringList out;
        for (const QJsonValue& v : json(call("check_schematic", {{"path", doc}})).value("notes").toArray())
            if (v.toObject().contains("box")) out << v.toObject().value("message").toString();
        return out;
    }
    // With ngspice, for the tests that simulate: false when there is none.
    // A machine with no font measures every text as nothing, drawn over
    // nothing (as test_symbol_tools' pin names).
    static bool noFont() { return QFontMetrics(QucsSettings.font).height() <= 0; }
    bool withNgspice()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) return false;
        QucsSettings.NgspiceExecutable = ngspice;
        return true;
    }
    // A variable's value at x, from get_dataset's 'at'.
    double at(const QString& doc, const QString& variable, double x)
    {
        const QJsonObject r = json(call("get_dataset", {{"path", doc}, {"variables", QJsonArray{variable}}, {"at", QJsonArray{x}}}));
        const QJsonArray row = r.value("variables").toArray().first().toObject().value("at").toArray().first().toArray();
        return row.size() >= 2 ? row.at(1).toDouble(NAN) : NAN;
    }
    static QString schematicText(const QString& components, const QString& wires = QString())
    {
        return QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n%1</Components>\n"
                              "<Wires>\n%2</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
            .arg(components, wires);
    }
    bool writeFile(const QString& file, const QString& text)
    {
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(text.toUtf8());
        return true;
    }
    void closeAll()
    {
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
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
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        // The fonts as main() sets them: a part's texts are measured in
        // QucsSettings.font, and the QFont made before the application,
        // left as it was, measures nothing on Linux.
        QucsSettings.font = QApplication::font();
        QucsSettings.appFont = QApplication::font();
        QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        QucsSettings.font.setPointSize(12);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
    }

    void cleanupTestCase()
    {
        closeAll();
        delete app;
        QucsMain = nullptr;
    }

    void cleanup() { closeAll(); }

    // ---- 3a: a kept run's equation variable (a NutmegEq's dB) measured as
    // this run's: its bandwidth was "not measured on both runs", the kept
    // run's curve read as no magnitude and not dB.
    void aKeptRunsEquationIsMeasured()
    {
        if (!withNgspice()) QSKIP("no ngspice here");
        QVERIFY(!failed(call("import_netlist", {{"text", "lowpass\nV1 in 0 DC 0 AC 1\nR1 in out 1k\nC1 out 0 100n\n.ac dec 20 10 1meg\n.end"},
                                                {"save_as", path("kept.sch")}})));
        QVERIFY(!failed(call("add_component", {{"path", "kept.sch"}, {"type", "NutmegEq"}, {"x", 300}, {"y", 400},
                                               {"properties", QJsonObject{{"Simulation", "ac"}}},
                                               {"equations", QJsonArray{"gain_db=dB(v(out))"}}})));
        QJsonObject r = call("simulate", {{"path", "kept.sch"}, {"keep_as", "before_caps"}});
        QVERIFY2(json(r).value("succeeded").toBool(), qPrintable(text(r)));
        QVERIFY(!failed(call("edit_component", {{"path", "kept.sch"}, {"name", "C1"}, {"properties", QJsonObject{{"C", "200n"}}}})));
        r = call("simulate", {{"path", "kept.sch"},
                              {"compare", QJsonObject{{"with", "before_caps"},
                                                      {"measure", QJsonArray{QJsonObject{{"variable", "ac.gain_db"}, {"what", "bandwidth"}},
                                                                             QJsonObject{{"variable", "gain_db"}, {"what", "gain"}, {"field", "dc gain dB"}}}}}}});
        const QJsonArray table = json(r).value("compared").toObject().value("table").toArray();
        QVERIFY2(table.size() == 2, qPrintable(text(r)));
        const QJsonObject bw = table.at(0).toObject();
        QVERIFY2(bw.value("before").isDouble() && bw.value("after").isDouble(), qPrintable(QJsonDocument(bw).toJson()));
        // 1/(2 pi 1k 100n) and 1/(2 pi 1k 200n).
        QVERIFY2(std::abs(bw.value("before").toDouble() / 1591.5 - 1.0) < 0.02 && std::abs(bw.value("after").toDouble() / 795.8 - 1.0) < 0.02,
                 qPrintable(QJsonDocument(bw).toJson()));
        // A field the measurement gives not: which it gives.
        QVERIFY2(table.at(1).toObject().value("note").toString().contains("'field' is one of"), qPrintable(QJsonDocument(table.at(1).toObject()).toJson()));
        // get_dataset's compare: the other run's bandwidth too.
        r = call("get_dataset", {{"path", "kept.sch"}, {"variables", QJsonArray{"ac.gain_db"}}, {"measure", QJsonArray{"bandwidth"}}, {"compare", "before_caps"}});
        const QJsonObject other = json(r).value("variables").toArray().first().toObject().value("other run").toObject();
        QVERIFY2(other.value("measurements").toObject().value("bandwidth").toObject().value("value").isDouble(), qPrintable(text(r)));
    }

    // The kept run's eye folded at the Tbit of the PRBS source that run
    // had, as this run's is - not told from its crossings.
    void aKeptRunsEyeIsFoldedAtItsSource()
    {
        if (!withNgspice()) QSKIP("no ngspice here");
        QVERIFY(writeFile(path("link.sch"),
                          schematicText("<vPRBS V1 1 100 130 18 -26 0 1 \"0 V\" 1 \"1 V\" 1 \"100 ps\" 1 \"0\" 0 \"10 ps\" 0 \"10 ps\" 0 \"7\" 1 \"\" 0 \"NRZ\" 0>\n"
                                        "<GND * 1 100 190 0 0 0 0>\n<R R1 1 160 100 -26 15 0 0 \"50\" 1 \"26.85\" 0 \"european\" 0>\n"
                                        "<C C1 1 220 130 17 -26 0 1 \"0.3 pF\" 1 \"\" 0 \"neutral\" 0>\n<GND * 1 220 190 0 0 0 0>\n"
                                        "<.TR TR1 1 100 300 0 57 0 0 \"lin\" 1 \"0\" 1 \"20n\" 1 \"20001\" 0>\n",
                                        "<100 160 100 190 \"\" 0 0 0 \"\">\n<100 100 130 100 \"\" 0 0 0 \"\">\n<190 100 220 100 \"rx\" 200 70 10 \"\">\n"
                                        "<220 160 220 190 \"\" 0 0 0 \"\">\n")));
        QVERIFY(!failed(call("open_document", {{"path", path("link.sch")}})));
        QJsonObject r = call("simulate", {{"path", "link.sch"}, {"keep_as", "slow"}});
        if (!json(r).value("succeeded").toBool()) QSKIP("this ngspice has no PRBS source");
        QVERIFY(!failed(call("edit_component", {{"path", "link.sch"}, {"name", "C1"}, {"properties", QJsonObject{{"C", "0.1 pF"}}}})));
        QVERIFY(json(call("simulate", {{"path", "link.sch"}})).value("succeeded").toBool());
        r = call("get_dataset", {{"path", "link.sch"}, {"variables", QJsonArray{"tran.v(rx)"}}, {"measure", QJsonArray{"eye"}}, {"compare", "slow"}});
        const QJsonObject eye = json(r).value("variables").toArray().first().toObject().value("other run").toObject()
                                    .value("measurements").toObject().value("eye").toObject();
        QVERIFY2(eye.value("unit interval from").toString().contains("V1's Tbit"), qPrintable(text(r)));
    }

    // ---- 3e: get_settings by key and word, and across the scopes.
    void settingsAreFoundByKeyAndWord()
    {
        QJsonObject r = call("get_settings", {{"scope", "app"}, {"keys", QJsonArray{"Locations/*"}}});
        QJsonArray settings = json(r).value("settings").toArray();
        QVERIFY2(settings.size() >= 5, qPrintable(text(r)));
        for (const QJsonValue& v : settings) QVERIFY(v.toObject().value("key").toString().startsWith("Locations/"));
        QVERIFY(json(r).value("matched").toString().contains(QStringLiteral("%1 of ").arg(settings.size())));
        // A label alone, no scope: found in app's.
        r = call("get_settings", {{"keys", QJsonArray{"Maximum undo operations"}}});
        settings = json(r).value("settings").toArray();
        QVERIFY2(settings.size() == 1 && settings.first().toObject().value("scope").toString() == "app", qPrintable(text(r)));
        // A word, no scope: the simulators' too, each with it.
        r = call("get_settings", {{"search", "ngspice"}});
        settings = json(r).value("settings").toArray();
        bool simulators = false;
        for (const QJsonValue& v : settings) {
            const QString all = QJsonDocument(v.toObject()).toJson();
            QVERIFY2(all.contains("ngspice", Qt::CaseInsensitive), qPrintable(all));
            simulators = simulators || v.toObject().value("scope").toString() == "simulators";
        }
        QVERIFY2(simulators, qPrintable(text(r)));
        // Nothing: said; neither scope nor filter: refused.
        r = call("get_settings", {{"scope", "app"}, {"search", "zzzqqq"}});
        QVERIFY(json(r).value("settings").toArray().isEmpty() && json(r).value("note").toString().contains("Nothing matches"));
        QVERIFY(failed(call("get_settings", {})));
    }

    // ---- 2: a text drawn over something - a wire, a symbol, another
    // text, a label - a note of check_schematic's with both boxes; an
    // op-amp's name in its triangle's empty corner none.
    void textsOverSomethingAreNoted()
    {
        if (noFont()) QSKIP("this platform has no font to measure a text with");
        QVERIFY(writeFile(path("overlap.sch"),
                          schematicText("<R R1 1 200 200 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n"
                                        "<OpAmp OP1 1 500 200 6 22 0 0 \"1e6\" 1 \"15 V\" 0>\n"
                                        "<R R2 1 200 400 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n"
                                        "<R R3 1 220 410 -26 15 0 0 \"2k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n",
                                        // A wire down through R1's text, clear of its pins.
                                        "<190 220 190 290 \"\" 0 0 0 \"\">\n")));
        QVERIFY(!failed(call("open_document", {{"path", path("overlap.sch")}})));
        const QJsonArray notes = json(call("check_schematic", {{"path", "overlap.sch"}})).value("notes").toArray();
        QJsonObject wireNote;
        int texts = 0;
        for (const QJsonValue& v : notes) {
            const QString m = v.toObject().value("message").toString();
            if (m.startsWith("R1's text overlaps the wire 190,220-190,290")) wireNote = v.toObject();
            if (m.contains("'s text overlaps R") && m.contains("'s text")) ++texts;
            QVERIFY2(!m.startsWith("OP1"), qPrintable(m));   // (its name in the triangle's corner)
        }
        QVERIFY2(!wireNote.isEmpty(), qPrintable(QJsonDocument(notes).toJson()));
        QCOMPARE(wireNote.value("component").toString(), QString("R1"));
        QCOMPARE(wireNote.value("box").toArray().size(), 4);
        QCOMPARE(wireNote.value("over").toArray(), (QJsonArray{189, 219, 191, 291}));
        // R2's and R3's texts over each other: said once.
        QCOMPARE(texts, 1);
        // The letters, not the room around them in their line's box: a wire
        // within R1's name's box but past its last letter, or under its
        // letters in the line's descent - none; through the letters, one.
        // (Which room a font leaves differs: DejaVu Sans, Linux's, leaves
        // little after "R1"; every font leaves its descent under it.)
        {
            Component* r1 = front()->getComponentByName("R1");
            const QRect name = qucs_s::textplace::textBoxes(r1).first(), ink = qucs_s::textplace::inkBoxes(r1).first();
            const QString r = QStringLiteral("<R R1 1 200 200 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n");
            // Down beside the name, to its line only, not the value's under it.
            const auto down = [&](int x) { return QStringLiteral("<%1 %2 %1 %3 \"\" 0 0 0 \"\">\n").arg(x).arg(name.top() - 6).arg(name.bottom() - 3); };
            // Across from the left, under the letters, ending under them.
            const auto across = [&](int y) { return QStringLiteral("<%1 %2 %3 %2 \"\" 0 0 0 \"\">\n").arg(name.left() - 10).arg(y).arg(ink.right() - 1); };
            QString clear;
            if (name.right() - ink.right() >= 4) clear = down((ink.right() + name.right() + 1) / 2);
            else if (name.bottom() - ink.bottom() >= 3) clear = across((ink.bottom() + name.bottom() + 1) / 2);
            QVERIFY2(!clear.isEmpty(), qPrintable(QStringLiteral("no room in the line's box beside or under the letters: %1 in %2")
                                                      .arg(QString::number(ink.right()) + "," + QString::number(ink.bottom()),
                                                           QString::number(name.right()) + "," + QString::number(name.bottom()))));
            QVERIFY(writeFile(path("ink.sch"), schematicText(r, clear)));
            QVERIFY(!failed(call("open_document", {{"path", path("ink.sch")}})));
            QVERIFY2(!overlapNotes("ink.sch").join(" ").contains("R1's text"), qPrintable(overlapNotes("ink.sch").join("\n")));
            QVERIFY(writeFile(path("ink2.sch"), schematicText(r, down((ink.left() + ink.right()) / 2))));
            QVERIFY(!failed(call("open_document", {{"path", path("ink2.sch")}})));
            QVERIFY2(overlapNotes("ink2.sch").join(" ").contains("R1's text overlaps the wire"), qPrintable(overlapNotes("ink2.sch").join("\n")));
            QVERIFY(!failed(call("show_document", {{"path", path("overlap.sch")}})));
        }
        // A label's text on a wire.
        QVERIFY(!failed(call("set_label", {{"path", "overlap.sch"}, {"at", QJsonArray{190, 250}}, {"name", "mid"},
                                           {"text_at", QJsonArray{180, 230}}})));
        QVERIFY2(overlapNotes("overlap.sch").join("\n").contains("the label mid overlaps the wire"), qPrintable(overlapNotes("overlap.sch").join("\n")));
    }

    // text_at "auto": the nearest free spot beside it, no other text
    // moving; one clear stays where it is.
    void autoTextGoesClear()
    {
        if (noFont()) QSKIP("this platform has no font to measure a text with");
        QVERIFY(writeFile(path("auto.sch"),
                          schematicText("<R R1 1 200 200 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n"
                                        "<R R2 1 500 200 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n",
                                        "<190 220 190 290 \"\" 0 0 0 \"\">\n")));
        QVERIFY(!failed(call("open_document", {{"path", path("auto.sch")}})));
        Schematic* sch = front();
        const QPoint r2(sch->getComponentByName("R2")->tx, sch->getComponentByName("R2")->ty);
        QJsonObject r = call("edit_component", {{"path", "auto.sch"}, {"name", "R1"}, {"text_at", "auto"}});
        QVERIFY2(!failed(r) && json(r).value("text").toString().contains("clear of the wire 190,220-190,290"), qPrintable(text(r)));
        // The nearest spot, its right first: beside the part (its body ends
        // at 30), level with it - not far along the side.
        const QPoint moved(sch->getComponentByName("R1")->tx, sch->getComponentByName("R1")->ty);
        QVERIFY2(json(r).value("text").toString().contains("right of it") && moved.x() > 30 && moved.x() <= 40 && moved.y() > -30 && moved.y() < 5,
                 qPrintable(QStringLiteral("%1, %2: ").arg(moved.x()).arg(moved.y()) + text(r)));
        QVERIFY2(overlapNotes("auto.sch").isEmpty(), qPrintable(overlapNotes("auto.sch").join("\n")));
        QCOMPARE(QPoint(sch->getComponentByName("R2")->tx, sch->getComponentByName("R2")->ty), r2);
        // Clear: left there.
        r = call("edit_component", {{"path", "auto.sch"}, {"name", "R2"}, {"text_at", "auto"}});
        QVERIFY2(json(r).value("text").toString().contains("left there"), qPrintable(text(r)));
        QCOMPARE(QPoint(sch->getComponentByName("R2")->tx, sch->getComponentByName("R2")->ty), r2);
        // A text a little over its own pin: the nearest spot is where it
        // partly was - its own text is no obstacle - to the right.
        QVERIFY(!failed(call("add_component", {{"path", "auto.sch"}, {"type", "R"}, {"name", "R3"}, {"x", 500}, {"y", 500},
                                               {"text_at", QJsonArray{24, -8}}})));
        QVERIFY2(overlapNotes("auto.sch").join(" ").contains("R3's text overlaps its own symbol"), qPrintable(overlapNotes("auto.sch").join("\n")));
        r = call("edit_component", {{"path", "auto.sch"}, {"name", "R3"}, {"text_at", "auto"}});
        QVERIFY2(json(r).value("text").toString().contains("right of it"), qPrintable(text(r)));
        // Moved so its text is on a wire: said.
        QVERIFY(!failed(call("add_wire", {{"path", "auto.sch"}, {"points", QJsonArray{QJsonArray{490, 330}, QJsonArray{490, 400}}}})));
        r = call("edit_component", {{"path", "auto.sch"}, {"name", "R2"}, {"y", 310}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(overlapNotes("auto.sch").join(" ").contains("R2's text overlaps the wire 490,330-490,400"),
                 qPrintable(overlapNotes("auto.sch").join("\n")));
        QVERIFY2(json(r).value("text").toString().contains("text_at \"auto\" moves it clear"), qPrintable(text(r)));
    }

    // add_component: the type's place for its text when it is clear, else
    // the nearest free spot - as Cvcc beside VCC, the 1 October tidying.
    void aNewPartsTextGoesClear()
    {
        if (noFont()) QSKIP("this platform has no font to measure a text with");
        QVERIFY(writeFile(path("supply.sch"), schematicText("<Vdc VCC 1 640 120 -62 -26 0 1 \"15 V\" 1>\n<GND * 1 640 150 0 0 0 0>\n")));
        QVERIFY(!failed(call("open_document", {{"path", path("supply.sch")}})));
        // Its text where the type puts it, given: on VCC's symbol, noted.
        QJsonObject r = call("add_component", {{"path", "supply.sch"}, {"type", "C"}, {"name", "Cvcc"}, {"x", 700}, {"y", 120}, {"rotation", 3},
                                               {"text_at", QJsonArray{-70, -26}}});
        QVERIFY2(!failed(r) && !json(r).contains("text"), qPrintable(text(r)));
        bool noted = false;
        for (const QString& n : overlapNotes("supply.sch")) noted = noted || (n.contains("Cvcc") && n.contains("VCC's"));
        QVERIFY2(noted, qPrintable(overlapNotes("supply.sch").join("\n")));
        QVERIFY(!failed(call("undo")));
        // Not given: put clear, and said.
        r = call("add_component", {{"path", "supply.sch"}, {"type", "C"}, {"name", "Cvcc"}, {"x", 700}, {"y", 120}, {"rotation", 3}});
        QVERIFY2(!failed(r) && json(r).value("text").toString().contains("clear of VCC"), qPrintable(text(r)));
        QVERIFY2(!overlapNotes("supply.sch").join(" ").contains("Cvcc"), qPrintable(overlapNotes("supply.sch").join("\n")));
        // Clear where its type puts it: nothing said, nothing moved.
        r = call("add_component", {{"path", "supply.sch"}, {"type", "R"}, {"name", "Rfar"}, {"x", 300}, {"y", 400}});
        QVERIFY2(!failed(r) && !json(r).contains("text"), qPrintable(text(r)));
        QCOMPARE(json(r).value("text at").toArray(), (QJsonArray{-26, 15}));
    }

    // arrange 'labels': only texts move - every one drawn over something,
    // none after - parts and wires as they were, every net too.
    void arrangeLabelsMovesOnlyTexts()
    {
        if (noFont()) QSKIP("this platform has no font to measure a text with");
        QStringList lines{"board", "V1 n0 0 DC 5"};
        for (int i = 1; i < 30; ++i)
            lines << QStringLiteral("%1%2 n%3 n%2 %4").arg(QString("RCL").at(i % 3)).arg(i).arg((i - 1) / 2).arg(QStringList{"1k", "10n", "1u"}.at(i % 3));
        lines << ".op" << ".end";
        QVERIFY(!failed(call("import_netlist", {{"text", lines.join("\n")}, {"save_as", path("board.sch")}})));
        Schematic* sch = front();
        // A part's text over its own pin, too.
        QVERIFY(!failed(call("edit_component", {{"path", "board.sch"}, {"name", "R3"}, {"text_at", QJsonArray{24, -8}}})));
        QVERIFY(overlapNotes("board.sch").join(" ").contains("R3's text overlaps its own symbol"));
        const int before = int(overlapNotes("board.sch").size());
        QVERIFY2(before > 5, qPrintable(QString::number(before)));
        const QString circuit = netlistOf("board.sch");
        QStringList where;
        for (Component* c : sch->a_DocComps) where << QStringLiteral("%1 %2,%3").arg(c->Name).arg(c->cx).arg(c->cy);
        for (Wire* w : sch->a_DocWires) where << QStringLiteral("wire %1,%2-%3,%4").arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2);
        QVERIFY(failed(call("arrange", {{"path", "board.sch"}, {"labels", true}, {"spacing", 80}})));   // texts only
        const QJsonObject r = call("arrange", {{"path", "board.sch"}, {"labels", true}});
        QVERIFY2(!failed(r) && json(r).value("moved").toArray().size() >= before, qPrintable(text(r)));
        QVERIFY2(text(r).contains("R3's text: right of it"), qPrintable(text(r)));
        QVERIFY2(overlapNotes("board.sch").isEmpty(), qPrintable(overlapNotes("board.sch").join("\n")));
        QCOMPARE(netlistOf("board.sch"), circuit);
        QStringList now;
        for (Component* c : sch->a_DocComps) now << QStringLiteral("%1 %2,%3").arg(c->Name).arg(c->cx).arg(c->cy);
        for (Wire* w : sch->a_DocWires) now << QStringLiteral("wire %1,%2-%3,%4").arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2);
        QCOMPARE(now, where);
        // One step back.
        QVERIFY(!failed(call("undo")));
        QCOMPARE(int(overlapNotes("board.sch").size()), before);
        // Nothing over anything: nothing moved.
        QVERIFY(!failed(call("redo")));
        QVERIFY(json(call("arrange", {{"path", "board.sch"}, {"labels", true}})).value("arranged").toString().contains("Nothing moved"));
    }

    // Diagrams: one below another, placed by add_diagram, clear of what the
    // upper one draws (its axis label); moved so its title runs into it,
    // said with the y that clears it, and noted by check_schematic.
    void diagramsBelowEachOtherKeepClear()
    {
        if (noFont()) QSKIP("this platform has no font to measure a text with");
        QVERIFY(!failed(call("import_netlist", {{"text", "rc\nV1 in 0 DC 0 AC 1\nR1 in out 1k\nC1 out 0 1n\n.ac dec 10 1k 1meg\n.end"},
                                                {"save_as", path("plots.sch")}})));
        QJsonObject one = json(call("add_diagram", {{"path", "plots.sch"}, {"traces", QJsonArray{"ac.v(out)"}}, {"title", "Output"},
                                                    {"x_axis", QJsonObject{{"label", "frequency (Hz)"}}}}));
        QJsonObject two = json(call("add_diagram", {{"path", "plots.sch"}, {"traces", QJsonArray{"ac.v(in)"}}, {"title", "Input"},
                                                    {"x_axis", QJsonObject{{"label", "frequency (Hz)"}}}}));
        QVERIFY2(!two.value("note").toString().contains("Drawn into another"), qPrintable(two.value("note").toString()));
        // Where it is, said as it is (on the grid).
        QVERIFY2(two.value("note").toString().contains(QStringLiteral("lower left corner at %1, %2.").arg(two.value("x").toInt()).arg(two.value("y").toInt())),
                 qPrintable(two.value("note").toString()));
        QVERIFY(!overlapNotes("plots.sch").join(" ").contains("diagram"));
        // Up against the first: its title on the first's axis label.
        QJsonObject r = call("edit_diagram", {{"path", "plots.sch"}, {"diagram", 2}, {"y", one.value("y").toInt() + 190}});
        const QString note = json(r).value("note").toString();
        QVERIFY2(note.contains("its title runs into diagram 1's x-axis label"), qPrintable(text(r)));
        QVERIFY2(overlapNotes("plots.sch").join(" ").contains("diagram 1: diagram 2's title runs into its x-axis label"),
                 qPrintable(overlapNotes("plots.sch").join("\n")));
        const QRegularExpressionMatch m = QRegularExpression("y (-?\\d+) or more clears it").match(note);
        QVERIFY(m.hasMatch());
        r = call("edit_diagram", {{"path", "plots.sch"}, {"diagram", 2}, {"y", m.captured(1).toInt()}});
        QVERIFY2(!json(r).value("note").toString().contains("Drawn into another"), qPrintable(text(r)));
    }

    // ---- 4: a two-pin part placed by where its pin 1 goes - no table of
    // rotations: a capacitor below a node, pin 1 up, wires straight.
    void pin1PlacesATwoPinPart()
    {
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(call("save_document", {{"as", path("pins.sch")}})));
        const QHash<QString, QPoint> expected{{"top", {0, -30}}, {"bottom", {0, 30}}, {"left", {-30, 0}}, {"right", {30, 0}}};
        int x = 400;
        for (auto it = expected.cbegin(); it != expected.cend(); ++it, x += 100) {
            const QJsonObject r = json(call("add_component", {{"type", "C"}, {"x", x}, {"y", 400}, {"pin1", it.key()}}));
            const QJsonObject pin = r.value("pins").toArray().first().toObject();
            QCOMPARE(QPoint(pin.value("x").toInt() - x, pin.value("y").toInt() - 400), it.value());
        }
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}})));
        QJsonObject r = call("add_component", {{"type", "C"}, {"name", "Cb"}, {"x", 230}, {"y", 200}, {"pin1", "top"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const int before = int(front()->a_DocWires.size());
        QVERIFY(!failed(call("connect", {{"from", "Cb.1"}, {"to", "R1.2"}})));
        QVERIFY(!failed(call("connect", {{"from", "Cb.2"}, {"to", "ground"}})));
        // One straight wire up to R1.2, the ground on C1.2 itself.
        QCOMPARE(int(front()->a_DocWires.size()), before + 1);
        const Wire* w = front()->a_DocWires.back();
        QVERIFY2(w->x1 == w->x2 && w->x1 == 230, qPrintable(QStringLiteral("%1,%2-%3,%4").arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2)));
        // edit_component turns a part by it (its nets kept, as a rotation).
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "Cturn"}, {"x", 700}, {"y", 100}})));
        r = call("edit_component", {{"name", "Cturn"}, {"pin1", "bottom"}});
        QVERIFY2(!failed(r) && json(r).value("rotation").toInt() == 1, qPrintable(text(r)));
        QCOMPARE(json(r).value("pins").toArray().first().toObject().value("y").toInt(), 130);
        // Refused: three pins, no side, both.
        QVERIFY(text(call("add_component", {{"type", "OpAmp"}, {"x", 600}, {"y", 600}, {"pin1", "top"}})).contains("two pins"));
        QVERIFY(failed(call("add_component", {{"type", "C"}, {"x", 600}, {"y", 600}, {"pin1", "up"}})));
        QVERIFY(text(call("add_component", {{"type", "C"}, {"x", 600}, {"y", 600}, {"pin1", "top"}, {"rotation", 1}})).contains("not both"));
    }

    // ---- 1: Tools > Filter synthesis as a tool - its own calculation,
    // run with --json - placed in a new schematic; a 5th-order 0.1 dB
    // Chebyshev at 1 GHz simulates -3 dB at 1 GHz, ripple 0.1 dB.
    void aFilterIsSynthesized()
    {
        QJsonObject r = call("synthesize_filter", {{"response", "chebyshev"}, {"type", "lowpass"}, {"order", 5}, {"fc", "1 GHz"},
                                                   {"ripple", 0.1}, {"impedance", 50}, {"save_as", path("cheb.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QStringList parts = json(r).value("parts").toVariant().toStringList();
        QVERIFY2(parts.contains("C1 4.142pF") && parts.contains("L1 12.38nH") && parts.contains("C2 7.134pF"), qPrintable(parts.join(", ")));
        QCOMPARE(json(r).value("filter").toObject().value("order").toInt(), 5);
        QCOMPARE(json(r).value("schematic").toString(), QString("cheb.sch"));
        // The order from the attenuation: 40 dB an octave above, 7.
        r = call("synthesize_filter", {{"response", "butterworth"}, {"fc", 1e6}, {"fs", 2e6}, {"atten", 40}});
        QVERIFY2(json(r).value("filter").toObject().value("order").toInt() == 7, qPrintable(text(r)));
        QVERIFY(json(r).value("filter").toObject().value("order from").toString().contains("40 dB"));
        // What it cannot be: said.
        QVERIFY(text(call("synthesize_filter", {{"type", "bandpass"}, {"fc", 1e9}, {"order", 3}})).contains("'f2'"));
        QVERIFY(text(call("synthesize_filter", {{"topology", "microstrip_end_coupled"}, {"fc", 1e9}, {"order", 3}})).contains("band-pass"));
        if (!withNgspice()) QSKIP("no ngspice here: designed, not simulated");
        r = call("simulate", {{"path", "cheb.sch"}});
        QVERIFY2(json(r).value("succeeded").toBool(), qPrintable(text(r)));
        r = call("get_dataset", {{"path", "cheb.sch"}, {"variables", QJsonArray{"dBS21"}}, {"measure", QJsonArray{"bandwidth"}}});
        const double f3 = json(r).value("variables").toArray().first().toObject().value("measurements").toObject().value("bandwidth").toObject().value("value").toDouble();
        QVERIFY2(std::abs(f3 / 1e9 - 1.0) < 0.02, qPrintable(text(r)));
        r = call("get_dataset", {{"path", "cheb.sch"}, {"variables", QJsonArray{"dBS21"}}, {"from", 1e8}, {"to", 8.7e8}});
        const double ripple = -json(r).value("variables").toArray().first().toObject().value("min").toDouble();
        QVERIFY2(ripple > 0.09 && ripple <= 0.1005, qPrintable(QString::number(ripple)));
    }

    // Active filter synthesis: a 5th-order Butterworth Sallen-Key from its
    // stop band, -3 dB at 1 kHz. (A unity gain's low-pass was nan.)
    void anActiveFilterIsSynthesized()
    {
        QJsonObject r = call("synthesize_filter", {{"kind", "active"}, {"response", "butterworth"}, {"fc", 1000}, {"fs", 3000}, {"atten", 40},
                                                   {"save_as", path("active.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("filter").toObject().value("order").toInt(), 5);
        QVERIFY(!text(r).contains("nan"));
        // An order given, and no stop band: that order is made - two
        // op-amps for a 4th. With a stop band too, the order still wins,
        // said. A Cauer's order comes from its stop band: one given is
        // said not to be used. A band-stop's order is even.
        r = call("synthesize_filter", {{"kind", "active"}, {"response", "butterworth"}, {"order", 4}, {"fc", 1000}, {"save_as", path("active4.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("filter").toObject().value("order").toInt(), 4);
        int opAmps = 0;
        for (const QString& part : json(r).value("parts").toVariant().toStringList()) opAmps += part.startsWith("OP");
        QCOMPARE(opAmps, 2);
        r = call("synthesize_filter", {{"kind", "active"}, {"response", "chebyshev"}, {"order", 3}, {"fc", 1000}, {"fs", 3000}, {"atten", 40}});
        QCOMPARE(json(r).value("filter").toObject().value("order").toInt(), 3);
        QVERIFY2(json(r).value("filter").toObject().value("note").toString().contains("order given is made"), qPrintable(text(r)));
        // Not used, so an attenuation or a loss at fc no filter has does not
        // matter.
        r = call("synthesize_filter", {{"kind", "active"}, {"response", "butterworth"}, {"order", 4}, {"fc", 1000}, {"atten", 1e6}, {"ap", 0}});
        QVERIFY2(json(r).value("filter").toObject().value("order").toInt() == 4, qPrintable(text(r)));
        r = call("synthesize_filter", {{"kind", "active"}, {"response", "cauer"}, {"topology", "cauer"}, {"order", 2}, {"fc", 1000}, {"fs", 2000}, {"atten", 40}});
        QVERIFY2(json(r).value("filter").toObject().value("note").toString().contains("not used"), qPrintable(text(r)));
        r = call("synthesize_filter", {{"kind", "active"}, {"type", "bandstop"}, {"order", 3}, {"fc", 900}, {"f2", 1100}});
        QVERIFY2(failed(r) && text(r).contains("even"), qPrintable(text(r)));
        QVERIFY(failed(call("synthesize_filter", {{"kind", "active"}, {"response", "butterworth"}, {"fc", 1000}})));   // nor order nor fs
        // A stop band at the corner: no order (the window's formula gave one
        // past what an int holds), refused - not the program stopped.
        r = call("synthesize_filter", {{"kind", "active"}, {"response", "butterworth"}, {"fc", 1000}, {"fs", 1000}});
        QVERIFY2(failed(r) && text(r).contains("cannot be made"), qPrintable(text(r)));
        // A Bessel's order is given, so a band needs no 'transition'.
        r = call("synthesize_filter", {{"kind", "active"}, {"response", "bessel"}, {"type", "bandpass"}, {"order", 4}, {"fc", 900}, {"f2", 1100}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        if (!withNgspice()) QSKIP("no ngspice here: designed, not simulated");
        QVERIFY2(json(call("simulate", {{"path", "active.sch"}})).value("succeeded").toBool(), "");
        r = call("get_dataset", {{"path", "active.sch"}, {"variables", QJsonArray{"ac.v(out)"}}, {"measure", QJsonArray{"bandwidth"}}});
        const double f3 = json(r).value("variables").toArray().first().toObject().value("measurements").toObject().value("bandwidth").toObject().value("value").toDouble();
        QVERIFY2(std::abs(f3 / 1000.0 - 1.0) < 0.02, qPrintable(text(r)));
        // The 4th: -3 dB at fc, and 80 dB down a decade above it.
        QVERIFY2(json(call("simulate", {{"path", "active4.sch"}})).value("succeeded").toBool(), "");
        r = call("get_dataset", {{"path", "active4.sch"}, {"variables", QJsonArray{"ac.v(out)"}}, {"measure", QJsonArray{"bandwidth"}}});
        const QJsonObject v = json(r).value("variables").toArray().first().toObject();
        QVERIFY2(std::abs(v.value("measurements").toObject().value("bandwidth").toObject().value("value").toDouble() / 1000.0 - 1.0) < 0.02,
                 qPrintable(text(r)));
        QCOMPARE(v.value("to").toDouble(), 10000.0);
        QVERIFY2(std::abs(20 * std::log10(v.value("final").toDouble()) + 80) < 1, qPrintable(text(r)));
    }

    // Attenuator synthesis: a 10 dB pi; its equations ngspice's (an Eqn's
    // S[2,1] was a .PARAM ngspice refused).
    void anAttenuatorIsSynthesized()
    {
        QJsonObject r = call("synthesize_attenuator", {{"topology", "pi"}, {"attenuation", 10}, {"save_as", path("pad.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject ohms = json(r).value("attenuator").toObject().value("ohms").toObject();
        QVERIFY(std::abs(ohms.value("R1").toDouble() - 96.25) < 0.01 && std::abs(ohms.value("R2").toDouble() - 71.15) < 0.01);
        QVERIFY2(json(r).value("analyses and equations").toVariant().toStringList().contains("NutmegEq1"), qPrintable(text(r)));
        QVERIFY(json(call("synthesize_attenuator", {{"attenuation", 10}, {"simulator", "qucsator"}})).value("analyses and equations").toVariant().toStringList().contains("Eqn1"));
        // A value that is no number - 1e-300 dB, 1 to the program, its
        // resistors infinite - not placed.
        r = call("synthesize_attenuator", {{"attenuation", 1e-300}});
        QVERIFY2(failed(r) && text(r).contains("no number"), qPrintable(text(r)));
        // Between unequal ones, a least attenuation.
        QVERIFY(text(call("synthesize_attenuator", {{"attenuation", 3}, {"z_in", 50}, {"z_out", 300}})).contains("dB at least"));
        if (!withNgspice()) QSKIP("no ngspice here: designed, not simulated");
        QVERIFY(json(call("simulate", {{"path", "pad.sch"}})).value("succeeded").toBool());
        const double s21 = at("pad.sch", "S21_dB", 1e9);
        QVERIFY2(std::abs(s21 + 10.0) < 0.05, qPrintable(QString::number(s21)));
    }

    // Matching: 10 - j20 to 50 ohm at 900 MHz, an L-section - its series
    // element of no reactance left out, not a 0 H inductor - S11 below
    // -20 dB there. What the calculation would say in a message box, said
    // in the answer (none shown: the test would wait).
    void aMatchIsSynthesized()
    {
        QJsonObject r = call("synthesize_matching", {{"z_load", "10-j20"}, {"z_source", 50}, {"f", "900 MHz"}, {"save_as", path("match.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QStringList parts = json(r).value("parts").toVariant().toStringList();
        QVERIFY2(parts.contains("L1 4.421nH") && !parts.join(" ").contains("0.000H"), qPrintable(parts.join(", ")));
        // For Qucsator, its equations Qucsator's; the settings' after.
        r = call("synthesize_matching", {{"z_load", "10-j20"}, {"f", "900 MHz"}, {"simulator", "qucsator"}});
        QVERIFY2(json(r).value("analyses and equations").toVariant().toStringList().contains("Eqn1"), qPrintable(text(r)));
        QCOMPARE(QucsSettings.DefaultSimulator, int(spicecompat::simNgspice));
        r = call("synthesize_matching", {{"z_load", "0-j20"}, {"f", 1e9}, {"topology", "quarter_wave"}});
        // (In the answer - no message box came up in the window, to be
        // answered for it.)
        QVERIFY2(failed(r) && text(r).startsWith("No matching circuit: The load has not resistive part") && !text(r).contains("message box"),
                 qPrintable(text(r)));
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        // A two-port that is not unconditionally stable: refused, with K.
        r = call("synthesize_matching", {{"f", 1e9}, {"s", QJsonObject{{"s11", "0.9"}, {"s12", "0.5"}, {"s21", "3"}, {"s22", "0.9"}}}});
        QVERIFY2(failed(r) && text(r).contains("not unconditionally stable (K ="), qPrintable(text(r)));
        if (!withNgspice()) QSKIP("no ngspice here: designed, not simulated");
        r = call("simulate", {{"path", "match.sch"}});
        // Stock ngspice runs no S-parameter analysis of one port: 42 stops
        // ("can't allocate -8 bytes"), 44 and later refuse it ("Only one RF
        // Port is found, we need at least two!"). An ngspice that takes one
        // (the enhanced one) runs it.
        const QString last = json(r).value("last lines").toString();
        if (!json(r).value("succeeded").toBool() && (last.contains("Only one RF Port") || last.contains("can't allocate -8 bytes")))
            QSKIP("this ngspice runs no S-parameter analysis of one port");
        QVERIFY2(json(r).value("succeeded").toBool(), qPrintable(text(r)));
        const double s11 = at("match.sch", "S11_dB", 9e8);
        QVERIFY2(s11 < -20.0, qPrintable(QString::number(s11)));
    }

    // A design's inductors and capacitors as the file gives them, each
    // "name value" - and what is wrong with any: an initial current or
    // voltage, a polarised capacitor.
    static QStringList inductorsAndCapacitors(const QString& file, QStringList* wrong)
    {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) return {QStringLiteral("(no file)")};
        QStringList parts;
        for (const QString& line : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
            QString part = line.trimmed();
            if (!part.startsWith("<L ") && !part.startsWith("<C ")) continue;
            std::unique_ptr<Component> c(getComponentFromName(part));
            if (!c) {
                *wrong << "unread: " + line.trimmed();
                continue;
            }
            parts << c->Name + " " + c->Props.at(0)->Value;
            if (!c->Props.at(1)->Value.isEmpty()) *wrong << QStringLiteral("%1 starts at %2 %3").arg(c->Name, c->Props.at(1)->Name, c->Props.at(1)->Value);
            if (c->Model == "C" && c->getProperty("Symbol")->Value != "neutral")
                *wrong << QStringLiteral("%1 is drawn %2").arg(c->Name, c->getProperty("Symbol")->Value);
        }
        return parts;
    }

    // A load of no resistance to speak of (under 1 mOhm) is drawn as an
    // inductor or a capacitor alone: with no initial current or voltage,
    // the capacitor not polarised - they were given a resistor's
    // temperature and symbol, 26.85 A, 26.85 V and the polar symbol. So
    // the lumped quarter-wave attenuators' inductors.
    void aReactiveLoadStartsAtRest()
    {
        for (const char* topology : {"l_section", "single_stub", "double_stub", "lambda8_lambda4"})
            for (const char* load : {"0.0005+j20", "0.0005-j20"}) {
                const QString file = path(QStringLiteral("reactive_%1_%2.sch").arg(topology, QString(load).at(6) == '+' ? "l" : "c"));
                const QJsonObject r = call("synthesize_matching", {{"z_load", load}, {"f", "900 MHz"}, {"topology", topology}, {"save_as", file}});
                QVERIFY2(!failed(r), qPrintable(text(r)));
                QStringList wrong;
                const QStringList parts = inductorsAndCapacitors(file, &wrong);
                // (20 ohm at 900 MHz: 3.537 nH, 8.842 pF.)
                const QString drawn = QString(load).at(6) == '+' ? QStringLiteral("3.537nH") : QStringLiteral("8.842pF");
                QVERIFY2(std::any_of(parts.cbegin(), parts.cend(), [&](const QString& p) { return p.endsWith(" " + drawn); }),
                         qPrintable(topology + QStringLiteral(": ") + parts.join(", ")));
                QVERIFY2(wrong.isEmpty(), qPrintable(topology + QStringLiteral(" ") + load + QStringLiteral(": ") + wrong.join("; ")));
            }
        for (const char* topology : {"quarter_wave_series", "quarter_wave_shunt"}) {
            const QString file = path(QStringLiteral("lumped_%1.sch").arg(topology));
            const QJsonObject r = call("synthesize_attenuator", {{"topology", topology}, {"attenuation", 10}, {"f", 1e9}, {"lumped", true}, {"save_as", file}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QStringList wrong;
            const QStringList parts = inductorsAndCapacitors(file, &wrong);
            QVERIFY2(parts.size() >= 3 && parts.join(" ").contains("H"), qPrintable(topology + QStringLiteral(": ") + parts.join(", ")));
            QVERIFY2(wrong.isEmpty(), qPrintable(topology + QStringLiteral(": ") + wrong.join("; ")));
        }
    }

    // Power combining: a Wilkinson, lumped for ngspice; -3 dB each way.
    void aCombinerIsSynthesized()
    {
        QJsonObject r = call("synthesize_power_combiner", {{"type", "wilkinson"}, {"f", "1 GHz"}, {"save_as", path("wilk.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QStringList parts = json(r).value("parts").toVariant().toStringList();
        QVERIFY2(parts.contains("P3") && parts.contains("R1 100 Ohm"), qPrintable(parts.join(", ")));
        QVERIFY(text(call("synthesize_power_combiner", {{"f", 1e9}, {"implementation", "ideal"}})).contains("lumped elements only"));
        // For Qucsator: ideal lines, its equations Qucsator's; a Bagley of 5.
        r = call("synthesize_power_combiner", {{"f", 1e9}, {"simulator", "qucsator"}});
        QVERIFY2(!failed(r) && json(r).value("combiner").toObject().value("implementation").toString() == "ideal"
                     && json(r).value("analyses and equations").toVariant().toStringList().contains("Eqn1"), qPrintable(text(r)));
        r = call("synthesize_power_combiner", {{"f", 1e9}, {"simulator", "qucsator"}, {"type", "bagley"}, {"ways", 5}});
        QVERIFY2(!failed(r) && json(r).value("parts").toVariant().toStringList().contains("P6"), qPrintable(text(r)));
        QVERIFY(text(call("synthesize_power_combiner", {{"f", 1e9}, {"type", "branchline"}})).contains("only the Wilkinson"));
        if (!withNgspice()) QSKIP("no ngspice here: designed, not simulated");
        QVERIFY(json(call("simulate", {{"path", "wilk.sch"}})).value("succeeded").toBool());
        const double s21 = at("wilk.sch", "S21_dB", 1e9);
        QVERIFY2(std::abs(s21 + 3.01) < 0.05, qPrintable(QString::number(s21)));
    }

    // Line calculation: 50 ohm microstrip on FR4 at 2.4 GHz - its width,
    // and that width analyzed back to 50 ohm.
    void aLineIsCalculated()
    {
        QJsonObject r = call("line_calc", {{"type", "microstrip"}, {"z0", 50}, {"f", "2.4 GHz"}, {"er", 4.4}, {"h", "1.6 mm"}, {"t", "35 um"}, {"angle", 90}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const double w = json(r).value("values").toObject().value("W").toDouble();
        QVERIFY2(w > 2.95e-3 && w < 3.10e-3, qPrintable(text(r)));
        QVERIFY(json(r).value("results").toObject().contains("ErEff"));
        r = call("line_calc", {{"type", "microstrip"}, {"do", "analyze"}, {"w", w}, {"l", 0.017}, {"f", 2.4e9}, {"er", 4.4}, {"h", 1.6e-3}, {"t", 35e-6}});
        const double z0 = json(r).value("values").toObject().value("Z0").toDouble();
        QVERIFY2(std::abs(z0 / 50.0 - 1.0) < 0.005, qPrintable(text(r)));
        QVERIFY(failed(call("line_calc", {{"type", "microstrip"}, {"z0", 50}, {"din", 1e-3}})));   // a coax's
    }

    // A receiver's budget: Friis for the noise figure, the gains added.
    void aReceiverBudgetIsCalculated()
    {
        QJsonObject r = call("receiver_budget", {{"bandwidth", 200000},
                                                 {"stages", QJsonArray{QJsonObject{{"name", "LNA"}, {"gain", 20}, {"nf", 1.5}, {"oip3", 30}},
                                                                       QJsonObject{{"name", "mixer"}, {"gain", -7}, {"nf", 7}, {"iip3", 15}},
                                                                       QJsonObject{{"name", "IF"}, {"gain", 30}, {"nf", 4}, {"oip3", 35}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject system = json(r).value("system").toObject();
        const double f = std::pow(10, 0.15) + (std::pow(10, 0.7) - 1) / 100 + (std::pow(10, 0.4) - 1) / std::pow(10, 1.3);
        QVERIFY2(std::abs(system.value("noise figure, dB").toDouble() - 10 * std::log10(f)) < 0.01, qPrintable(text(r)));
        QCOMPARE(system.value("gain, dB").toDouble(), 43.0);
        QVERIFY(json(r).value("assumed").toArray().size() >= 3);   // (the P1dBs)
        QVERIFY2(system.value("input p1db, dBm").toDouble() > 20, qPrintable(text(r)));   // (none given: no limit)
        r = call("receiver_budget", {{"stages", QJsonArray{QJsonObject{{"gian", 20}}}}});
        QVERIFY2(failed(r) && text(r).contains("gian"), qPrintable(text(r) + QJsonDocument(r).toJson()));
    }

    // The programs of an app bundle are in its Contents/MacOS/bin, wherever
    // the bundle is: a "bin" above it (the repository's bin/macos, a user
    // called robin) once made Contents/MacOS the programs' folder, and no
    // program - Tools' nor these tools' - was found.
    void theProgramsAreFoundWhereverTheAppIs()
    {
        const auto bundle = [](const QString& app) {
            QDir dir(app);   // (as main() makes it on macOS)
            return misc::binDirOf(app, dir);
        };
        QCOMPARE(bundle("/Applications/qucs-s.app/Contents/MacOS"), QString("/Applications/qucs-s.app/Contents/MacOS/bin/"));
        QCOMPARE(bundle("/Users/robin/Applications/qucs-s.app/Contents/MacOS"),
                 QString("/Users/robin/Applications/qucs-s.app/Contents/MacOS/bin/"));
        QCOMPARE(bundle("/Users/x/git/q/bin/macos/qucs-s.app/Contents/MacOS"),
                 QString("/Users/x/git/q/bin/macos/qucs-s.app/Contents/MacOS/bin/"));
        // A prefix install: the programs beside it in prefix/bin.
        QDir prefix("/usr/local/bin");
        prefix.cdUp();
        QCOMPARE(misc::binDirOf("/usr/local/bin", prefix), QString("/usr/local/bin") + QDir::separator());
    }

    // A design into a schematic open, at x, y: one step to undo; its ports
    // numbered on from those there, said.
    void aDesignGoesIntoAnOpenSchematic()
    {
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(call("save_document", {{"as", path("host.sch")}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}})));
        QJsonObject r = call("synthesize_attenuator", {{"attenuation", 6}, {"path", "host.sch"}, {"x", 300}, {"y", 300}});
        QVERIFY2(!failed(r) && json(r).value("placed").toString().contains("host.sch"), qPrintable(text(r)));
        const QRegularExpressionMatch corner = QRegularExpression("corner at (-?\\d+), (-?\\d+)").match(json(r).value("placed").toString());
        QVERIFY(corner.hasMatch() && std::abs(corner.captured(1).toInt() - 300) <= 10 && std::abs(corner.captured(2).toInt() - 300) <= 10);
        // Its parts there, not where the program drew them (from x 50).
        for (Component* c : front()->a_DocComps)
            if (c->Name != "R1" || c->cx != 100) QVERIFY2(c->cx >= 300 && c->cy >= 290, qPrintable(QStringLiteral("%1 at %2, %3").arg(c->Name).arg(c->cx).arg(c->cy)));
        const int parts = int(front()->a_DocComps.size());
        r = call("synthesize_attenuator", {{"attenuation", 6}, {"path", "host.sch"}});
        QVERIFY2(json(r).value("ports").toString().contains("had ports already"), qPrintable(text(r)));
        QVERIFY(int(front()->a_DocComps.size()) > parts);
        QVERIFY(!failed(call("undo")));
        QCOMPARE(int(front()->a_DocComps.size()), parts);
        // A node's label in a design (a wire of no length that a paste puts
        // on the node there): put down on its node.
        QString error;
        const QJsonObject placed = control->placeDesign(
            {{"path", "host.sch"}, {"x", 800}, {"y", 100}},
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n<R R1 1 100 100 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n"
            "</Components>\n<Wires>\n<130 100 130 100 \"tap\" 150 70 0 \"\">\n</Wires>\n", &error);
        QVERIFY2(!placed.isEmpty(), qPrintable(error));
        bool labelled = false;
        for (const QJsonValue& v : json(call("get_schematic", {{"path", "host.sch"}})).value("nets").toArray())
            labelled = labelled || v.toObject().value("net").toString() == "tap";
        QVERIFY(labelled);
    }

    // ---- 5b: the manual, fetched once (with curl, here from a copy given
    // by QUCS_MANUAL_URL) into the cache and searched by section; a page
    // whole; every read_help topic searches it from then on.
    void theManualIsFetchedAndSearched()
    {
        if (QStandardPaths::findExecutable("curl").isEmpty()) QSKIP("no curl here");
        const QString site = dir.filePath("site");
        QVERIFY(writeFile(site + "/searchindex.js",
                          "Search.setIndex({\"docnames\":[\"index\",\"guide/tuning\",\"guide/gone\"],"
                          "\"filenames\":[\"index.rst\",\"guide/tuning.md\",\"guide/gone.md\"]})"));
        QVERIFY(writeFile(site + "/_sources/index.rst.txt", "Welcome\n=======\n\nThe manual.\n"));
        QVERIFY(writeFile(site + "/_sources/guide/tuning.md.txt",
                          "# Tuning\n\nThe tuner changes values live.\n\n## Sliders\n\nEach slider sets one value of a part.\n\n```\n# not a heading\n```\n"));
        qputenv("QUCS_MANUAL_URL", (QUrl::fromLocalFile(site).toString() + "/").toUtf8());
        QJsonObject r = call("read_help", {{"manual", true}, {"topic", "slider value"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("fetched").toString().startsWith("2 pages"), qPrintable(text(r)));
        QVERIFY2(json(r).value("not fetched").toString().contains("1 of its pages"), qPrintable(text(r)));
        const QJsonObject found = json(r).value("manual").toArray().first().toObject();
        QCOMPARE(found.value("section").toString(), QString("Sliders"));
        QCOMPARE(found.value("page").toString(), QString("guide/tuning"));
        QVERIFY(found.value("url").toString().endsWith("guide/tuning.html"));
        QVERIFY(QFileInfo::exists(QDir(misc::cacheDir()).filePath("manual/guide/tuning.md.txt")));
        // (A # in a fenced block is none of its headings.)
        r = call("read_help", {{"manual", true}, {"topic", "not a heading"}});
        QCOMPARE(json(r).value("manual").toArray().first().toObject().value("section").toString(), QString("Sliders"));
        r = call("read_help", {{"page", "guide/tuning"}});
        QVERIFY2(json(r).value("text").toString().startsWith("# Tuning"), qPrintable(text(r)));
        QVERIFY(failed(call("read_help", {{"page", "guide/nothing"}})));
        // Kept: every topic searches it, the copy gone.
        QDir(site).removeRecursively();
        r = call("read_help", {{"topic", "tuner"}});
        QVERIFY2(json(r).value("manual").toArray().first().toObject().value("section").toString() == "Tuning", qPrintable(text(r)));
        QVERIFY2(failed(call("read_help", {{"manual", true}, {"refresh", true}})), "fetched again from nothing");
        qunsetenv("QUCS_MANUAL_URL");
    }

    // ---- 5a: the workspace's projects in get_state, the open one marked;
    // a document of another said not to be of the open one.
    void theStateListsTheProjects()
    {
        const QString empty = schematicText(QString());
        QVERIFY(writeFile(path("alpha_prj/alpha.sch"), empty));
        QVERIFY(writeFile(path("beta_prj/beta.sch"), empty));
        QVERIFY(!failed(call("open_project", {{"name", "alpha"}})));
        QVERIFY(!failed(call("open_document", {{"path", path("beta_prj/beta.sch")}})));
        QVERIFY(!failed(call("open_document", {{"path", path("alpha_prj/alpha.sch")}})));
        const QJsonObject state = json(call("get_state"));
        QStringList projects;
        for (const QJsonValue& v : state.value("projects").toArray())
            projects << v.toObject().value("name").toString() + (v.toObject().value("open").toBool() ? "*" : "");
        QVERIFY2(projects.contains("alpha*") && projects.contains("beta"), qPrintable(projects.join(", ")));
        for (const QJsonValue& v : state.value("documents").toArray()) {
            const QJsonObject d = v.toObject();
            if (d.value("title").toString() == "beta.sch")
                QVERIFY2(d.value("project").toString() == "beta" && d.value("project not open").toString().contains("alpha is open"),
                         qPrintable(QJsonDocument(d).toJson()));
            if (d.value("title").toString() == "alpha.sch")
                QVERIFY(d.value("project").toString() == "alpha" && !d.contains("project not open"));
        }
        app->slotMenuProjClose();
    }
};

QTEST_MAIN(TestWishlistTools)
#include "test_wishlist_tools.moc"
