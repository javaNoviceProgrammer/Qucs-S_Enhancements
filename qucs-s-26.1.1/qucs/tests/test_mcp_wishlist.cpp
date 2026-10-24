/*
 * test_mcp_wishlist.cpp - Claude's merged MCP wishlist of 22 October 2026:
 * a value implausible for its unit refused (N1); an input read under
 * another name or left out said (N2); how a call would be treated before
 * it is made (N3); an axis' own notation (N4); a plot group's styling kept
 * when it is shown again (N5); a filled box its own paper, so that a
 * screen picture matches the window (N6); mesh convergence and reference
 * numbers in one call (N7, N8); 'ground' reusing a ground placed and left
 * (N9); a script's error saying what happened (N10); a singular argument
 * read as the list it names (N11); the PDF viewer's selection as data
 * (N12); a smaller picture (N13); what a screen picture shows (N14).
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
#include <QPainter>
#include <QPageSize>
#include <QTextDocument>
#include <QPdfWriter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <array>
#include <cmath>

#include "components/component.h"
#include "config.h"
#include "diagrams/diagram.h"
#include "extsimkernels/spicecompat.h"
#include "ink.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "multiphysics/fem_expr.h"
#include "multiphysicsdoc.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "textdoc.h"

using FemNode = qucs_s::fem::Node;
using qucs_s::fem::Dim;
using qucs_s::fem::Scope;
using qucs_s::fem::evaluateConstant;

class TestMcpWishlist : public QObject
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
    /// A call that must not fail, with its answer when it does.
    QJsonObject must(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 60000)
    {
        const QJsonObject r = call(tool, args, timeoutMs);
        if (failed(r) || r.isEmpty())
            qWarning("%s %s -> %s", qPrintable(tool), QJsonDocument(args).toJson(QJsonDocument::Compact).constData(),
                     QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
        return r;
    }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r)
    {
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text"))
                return QJsonDocument::fromJson(v.toObject().value(QStringLiteral("text")).toString().toUtf8()).object();
        return {};
    }
    /// The first picture of a result, as an image.
    static QImage picture(const QJsonObject& r)
    {
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("image")) {
                QImage image;
                image.loadFromData(QByteArray::fromBase64(v.toObject().value(QStringLiteral("data")).toString().toLatin1()));
                return image;
            }
        return {};
    }
    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    bool writeFile(const QString& file, const QString& what)
    {
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(what.toUtf8());
        return true;
    }
    /// A PDF of one page with four lines at known heights (points from the
    /// bottom: 700, 660, 620, 580), written by hand so that their boxes
    /// are where the test says.
    static bool writeLinedPdf(const QString& path)
    {
        // (The first line is drawn in two goes, so that the page has a
        // line of more than one text run - what splits a box on a real
        // page, where an italic word or a symbol does it.)
        const QList<std::tuple<QByteArray, int, int>> lines{{"First line of the page,", 50, 700},
                                                            {"near the top.", 230, 700},
                                                            {"Second line, a little lower down.", 50, 660},
                                                            {"Third line, lower still.", 50, 620},
                                                            {"Fourth and last line of this page.", 50, 580}};
        QByteArray content;
        for (const auto& [words, x, y] : lines)
            content += "BT /F1 14 Tf " + QByteArray::number(x) + " " + QByteArray::number(y) + " Td (" + words + ") Tj ET ";
        QList<QByteArray> objects{"<< /Type /Catalog /Pages 2 0 R >>",
                                  "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                                  "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> "
                                  "/Contents 4 0 R >>",
                                  "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream",
                                  "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
        QByteArray out = "%PDF-1.4\n";
        QList<int> offsets;
        for (int i = 0; i < objects.size(); ++i) {
            offsets << out.size();
            out += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
        }
        const int start = out.size();
        out += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
        for (int o : std::as_const(offsets)) out += QByteArray::number(o).rightJustified(10, '0') + " 00000 n \n";
        out += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n"
               + QByteArray::number(start) + "\n%%EOF\n";
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(out);
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

    // N1. A number with no unit is read in SI. One far outside what the
    // unit expected is ever worth was taken and solved with: a dlinewidth
    // of "2e-10" (energy per field) is 1.2 GeV per V/m, and the gain of a
    // well with it took all the memory there was.
    void aBareNumberOutsideItsUnitIsRefused()
    {
        const Scope& b = Scope::builtins();
        QString why;
        // Energy per field (C·m): a bare 2e-10 is nothing anyone means.
        QVERIFY(!evaluateConstant(QStringLiteral("2e-10"), b, Dim::of(1, 0, 1, 1), &why).has_value());
        QVERIFY2(why.contains(QStringLiteral("read in SI")) && why.contains(QStringLiteral("brackets")), qPrintable(why));
        // Its unit by the name it has, not spelled out of the base units,
        // and no "give the number in SI" of a number that already is
        // (the check of 23 October, 3).
        QVERIFY2(why.contains(QString::fromUtf8("C\u00b7m")) && !why.contains(QString::fromUtf8("m\u00b7s\u00b7A")), qPrintable(why));
        QVERIFY2(why.contains(QStringLiteral("within that range")) && !why.contains(QStringLiteral("the number in SI")), qPrintable(why));
        // With its unit: read, and nothing said.
        why.clear();
        const std::optional<double> meant = evaluateConstant(QStringLiteral("1[meV]/(1e7[V/m])"), b, Dim::of(1, 0, 1, 1), &why);
        QVERIFY2(meant.has_value(), qPrintable(why));
        QVERIFY(std::abs(*meant - 1.602176634e-22 / 1e7) < 1e-34);
        // An energy: a bare 6.6e-3 is 41 PeV, not 6.6 meV.
        why.clear();
        QVERIFY(!evaluateConstant(QStringLiteral("6.6e-3"), b, Dim::of(2, 1, -2), &why).has_value());
        QVERIFY2(why.contains(QStringLiteral("an energy")), qPrintable(why));
        QVERIFY(evaluateConstant(QStringLiteral("6.6[meV]"), b, Dim::of(2, 1, -2), nullptr).has_value());
        // Values anyone means, and 0, go through: a temperature, a time, a
        // length (a geometry's numbers are lengths), a voltage.
        for (const auto& [number, dim] : std::vector<std::pair<const char*, Dim>>{
                 {"300", Dim::of(0, 0, 0, 0, 1)}, {"0", Dim::of(0, 0, 0, 0, 1)}, {"1e-12", Dim::of(0, 0, 1)},
                 {"3.3", Dim::voltage()}, {"-3.3", Dim::voltage()}, {"1e-6", Dim::length()}, {"5", Dim::length()},
                 {"0", Dim::of(1, 0, 1, 1)}, {"1e7", Dim::of(1, 1, -3, -1)}, {"9.1e-31", Dim::of(0, 1)}})
            QVERIFY2(evaluateConstant(QLatin1String(number), b, dim, &why).has_value(), qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(number), why)));
        // A unit with no scale to judge by (a density spans everything a
        // device has): nothing refused.
        QVERIFY(evaluateConstant(QStringLiteral("1e16"), b, Dim::of(-3), nullptr).has_value());
    }

    // N11. A singular where the tool takes a list of that name: read as
    // that list, and said - 'delete' with 'name' was refused, "Meant
    // names?", while the tools that take one take 'name'.
    void aSingularIsReadAsTheListItNames()
    {
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n<R R1 1 100 100 15 -26 0 1 \"1k\" 1>\n"
                                                                       "<R R2 1 200 100 15 -26 0 1 \"2k\" 1>\n</Components>\n<Wires>\n</Wires>\n")}})));
        const QJsonObject deleted = call("delete", {{"name", "R2"}});
        QVERIFY2(!failed(deleted), qPrintable(text(deleted)));
        QVERIFY2(text(deleted).contains(QStringLiteral("Deleted R2")), qPrintable(text(deleted)));
        // Said, not silently: the answer carries what was renamed.
        QVERIFY2(text(deleted).contains(QStringLiteral("name was read as names")), qPrintable(text(deleted)));
        QCOMPARE(json(call("get_schematic")).value("components").toArray().size(), 1);
        // A field that takes one of several forms (read_pdf's 'pages'
        // takes [3, 4], "2-5" or 7) is given the value as it is, and so is
        // one that is no list at all (undo's 'steps' is a number).
        QVERIFY(control->asPlural(QStringLiteral("read_pdf"), QJsonObject{{"page", 1}}).value("pages").toInt() == 1);
        QVERIFY(control->asPlural(QStringLiteral("delete"), QJsonObject{{"name", "R2"}}).value("names").toArray().size() == 1);
        QCOMPARE(control->asPlural(QStringLiteral("undo"), QJsonObject{{"step", 2}}).value("steps").toInt(), 2);
        QCOMPARE(control->asPlural(QStringLiteral("fem_plot"), QJsonObject{{"color", "viridis"}}).value("colors").toString(),
                 QStringLiteral("viridis"));
        // Read as that field, not as a list of one: a number wrapped would
        // be refused by its type (and two steps would not be undone).
        QVERIFY(!failed(must("new_document")));
        QVERIFY(!failed(must("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(must("add_component", {{"type", "C"}, {"x", 200}, {"y", 100}})));
        const QJsonObject undone = call("undo", {{"step", 2}});
        QVERIFY2(!failed(undone), qPrintable(text(undone)));
        QVERIFY2(text(undone).contains(QStringLiteral("step was read as steps")), qPrintable(text(undone)));
        QCOMPARE(json(call("get_schematic")).value("components").toArray().size(), 0);
        // Nothing to read that way: the tool's own argument, a plural
        // already given, a singular of a field of one type.
        QVERIFY(control->asPlural(QStringLiteral("delete"), QJsonObject{{"names", QJsonArray{"R1"}}}).isEmpty());
        QVERIFY(control->asPlural(QStringLiteral("delete"), QJsonObject{{"name", "R1"}, {"names", QJsonArray{"R2"}}}).isEmpty());
        QVERIFY(control->asPlural(QStringLiteral("add_component"), QJsonObject{{"type", "R"}}).isEmpty());
        // (A name of nothing is no singular of anything: synthesize_matching
        // takes 's', which "" + "s" would otherwise be read as.)
        QVERIFY(control->asPlural(QStringLiteral("synthesize_matching"), QJsonObject{{"", QJsonObject{}}}).isEmpty());
        // A number given as text where a number is taken, which is how a
        // client sends a field the tool does not declare: read as that
        // number, not left to be read as 0 (the check of 23 October, 5).
        QVERIFY(!failed(must("new_document")));
        QVERIFY(!failed(must("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(must("add_component", {{"type", "C"}, {"x", 200}, {"y", 100}})));
        const QJsonObject asText = call("undo", {{"step", "2"}});
        QVERIFY2(!failed(asText), qPrintable(text(asText)));
        QVERIFY2(text(asText).contains(QStringLiteral("Undone 2 steps")), qPrintable(text(asText)));
        QVERIFY2(text(asText).contains(QStringLiteral("given as text")), qPrintable(text(asText)));
        QCOMPARE(json(call("get_schematic")).value("components").toArray().size(), 0);
        QCOMPARE(control->asNumbers(QStringLiteral("undo"), QJsonObject{{"steps", "3"}}).value("steps").toInt(), 3);
        QCOMPARE(control->asNumbers(QStringLiteral("get_dataset"), QJsonObject{{"from", "1e-9"}}).value("from").toDouble(), 1e-9);
        // What is not a whole number for a whole number, what is not a
        // number at all, and a field that takes text are left alone.
        QVERIFY(control->asNumbers(QStringLiteral("undo"), QJsonObject{{"steps", "1.5"}}).isEmpty());
        QVERIFY(control->asNumbers(QStringLiteral("undo"), QJsonObject{{"steps", "a lot"}}).isEmpty());
        QVERIFY(control->asNumbers(QStringLiteral("fem_evaluate"), QJsonObject{{"time", "500[ms]"}}).isEmpty());
        QVERIFY(control->asNumbers(QStringLiteral("open_document"), QJsonObject{{"path", "1"}}).isEmpty());
        QVERIFY(failed(call("get_dataset", {{"path", "none.dat"}, {"points", "abc"}})));
        // And a word that is no singular of anything it takes is refused
        // as before, with what it does take.
        const QJsonObject no = call("delete", {{"nmae", "R1"}});
        QVERIFY(failed(no));
        QVERIFY2(text(no).contains(QStringLiteral("takes no nmae")), qPrintable(text(no)));
    }

    // N2. An input read under another name is said: import_data turned
    // 'v-out' into 'v_out' without a word, so a column asked for by the
    // name the file has could not be found.
    void aRenamedInputIsSaid()
    {
        QVERIFY(writeFile(path("meas.csv"), QStringLiteral("time,v-out,i-in\n0,0,0\n1e-9,0.5,1e-3\n2e-9,1,2e-3\n")));
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n</Components>\n<Wires>\n</Wires>\n")}})));
        QVERIFY(!failed(call("save_document", {{"as", "n2.sch"}})));
        const QJsonObject imported = json(call("import_data", {{"file", "meas.csv"}}));
        QVERIFY2(!imported.isEmpty(), "import_data answered nothing");
        const QJsonObject renamed = imported.value("renamed").toObject();
        QCOMPARE(renamed.value("v-out").toString(), QStringLiteral("v_out"));
        QCOMPARE(renamed.value("i-in").toString(), QStringLiteral("i_in"));
        // The columns themselves are the names used, as before.
        QVERIFY(imported.value("columns").toArray().contains(QJsonValue(QStringLiteral("v_out"))));
        // A file whose columns are names already: nothing said.
        QVERIFY(writeFile(path("plain.csv"), QStringLiteral("time,vout\n0,0\n1e-9,1\n")));
        QVERIFY(!json(call("import_data", {{"file", "plain.csv"}})).contains("renamed"));
    }

    // N3. How a call will be treated before it is made: the guards the
    // user chose, and what they name for those very arguments. The
    // settings file had to be read to learn the file guard was off.
    void theGuardsAreToldBeforeACallIsMade()
    {
        const QJsonObject guards = json(call("get_state")).value("guards").toObject();
        QVERIFY2(!guards.isEmpty(), "get_state said nothing of the guards");
        QVERIFY(guards.contains("commands") && guards.contains("files outside the workspace"));
        QVERIFY(guards.value("what they watch").toString().contains(QStringLiteral("commands:")));
        // describe_tool says what they name for one call, without making it.
        const QJsonObject outside = json(call("describe_tool", {{"name", "open_document"},
                                                                {"arguments", QJsonObject{{"path", "/etc/hosts"}}}}));
        const QJsonObject thisCall = outside.value("this call").toObject();
        QVERIFY2(!thisCall.isEmpty(), qPrintable(QJsonDocument(outside).toJson()));
        QCOMPARE(thisCall.value("treated as").toString(), QStringLiteral("asked about each time"));
        QVERIFY2(thisCall.value("why").toString().contains(QStringLiteral("Outside the workspace")), qPrintable(thisCall.value("why").toString()));
        QVERIFY2(thisCall.value("why").toString().contains(QStringLiteral("hosts")), qPrintable(thisCall.value("why").toString()));
        // One the guards name nothing in: let through, and said so.
        const QJsonObject plain = json(call("describe_tool", {{"name", "add_component"}, {"arguments", QJsonObject{{"type", "R"}}}}));
        QVERIFY(plain.value("this call").toObject().value("treated as").toString().startsWith(QStringLiteral("let through")));
        // A tool that only looks, and the guards without arguments.
        QVERIFY(json(call("describe_tool", {{"name", "get_schematic"}, {"arguments", QJsonObject{}}}))
                    .value("this call").toObject().value("treated as").toString() == QStringLiteral("let through"));
        QVERIFY(!json(call("describe_tool", {{"name", "get_schematic"}})).contains("this call"));
        QVERIFY(!json(call("describe_tool", {{"name", "get_schematic"}})).value("guards").toObject().isEmpty());
    }

    // N9. 'connect' to "ground" reused a ground already placed and left
    // connected to nothing, instead of adding a second one beside it.
    void groundReusesOneLeftUnwired()
    {
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n<R R1 1 100 100 15 -26 0 1 \"1k\" 1>\n"
                                                                       "</Components>\n<Wires>\n</Wires>\n")}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 100}, {"y", 160}})));
        const QJsonObject done = call("connect", {{"from", "R1.1"}, {"to", "ground"}});
        QVERIFY2(!failed(done), qPrintable(text(done)));
        QVERIFY2(text(done).contains(QStringLiteral("100, 160")), qPrintable(text(done)));
        const QJsonObject after = json(call("get_schematic"));
        int one = 0;
        for (const QJsonValue& v : after.value("components").toArray())
            if (v.toObject().value("type").toString() == QLatin1String("GND")) {
                ++one;
                QVERIFY(v.toObject().value("pins").toArray().first().toObject().value("connected").toBool());
            }
        QCOMPARE(one, 1);
        // Both pins on one net now.
        bool together = false;
        for (const QJsonValue& v : after.value("nets").toArray()) {
            const QJsonArray pins = v.toObject().value("pins").toArray();
            if (pins.contains(QJsonValue(QStringLiteral("R1.1"))) && pins.contains(QJsonValue(QStringLiteral("GND.1")))) together = true;
        }
        QVERIFY2(together, qPrintable(QJsonDocument(after.value("nets").toArray()).toJson()));
        // Only one placed and left is taken: each of the three things that
        // rule a ground out has a schematic of its own, and in each a
        // ground of its own is placed as before - two grounds after.
        const auto grounds = [this] {
            int n = 0;
            for (const QJsonValue& v : json(call("get_schematic")).value("components").toArray())
                if (v.toObject().value("type").toString() == QLatin1String("GND")) ++n;
            return n;
        };
        // In line with the pin (the same x) but out of reach: 470 units.
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n<R R2 1 100 100 15 -26 0 1 \"1k\" 1>\n"
                                                                       "</Components>\n<Wires>\n</Wires>\n")}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 100}, {"y", 600}})));
        QVERIFY(!failed(call("connect", {{"from", "R2.1"}, {"to", "ground"}})));
        QCOMPARE(grounds(), 2);
        // Within reach (120 units) but in neither line.
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n<R R3 1 100 100 15 -26 0 1 \"1k\" 1>\n"
                                                                       "</Components>\n<Wires>\n</Wires>\n")}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 160}, {"y", 190}})));
        QVERIFY(!failed(call("connect", {{"from", "R3.1"}, {"to", "ground"}})));
        QCOMPARE(grounds(), 2);
        // In line and within reach, but with a wire of the user's own on
        // it: theirs to keep, not one left over.
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n<R R4 1 100 100 15 -26 0 1 \"1k\" 1>\n"
                                                                       "<GND * 1 100 160 0 0 0 0>\n</Components>\n"
                                                                       "<Wires>\n<100 160 200 160 \"\" 0 0 0 \"\">\n</Wires>\n")}})));
        QVERIFY(!failed(call("connect", {{"from", "R4.1"}, {"to", "ground"}})));
        QCOMPARE(grounds(), 2);
    }

    // N4. An axis writes its numbers in its own notation, where that is
    // not the diagram's: x in SI prefixes while y is decimal. Kept in the
    // file, and given back by null.
    void anAxisWritesItsNumbersItsOwnWay()
    {
        QVERIFY(writeFile(path("li.csv"), QStringLiteral("bias,semi.I_anode\n0,0\n1,1e-3\n2,2e-3\n")));
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n</Components>\n<Wires>\n</Wires>\n")}})));
        QVERIFY(!failed(call("save_document", {{"as", "n4.sch"}})));
        QVERIFY(!failed(call("import_data", {{"file", "li.csv"}})));
        // (A trace may be an expression of the dataset's variables, which
        // is how a current is shown in mA without copying the data.)
        const QJsonObject made = json(call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"li:semi.I_anode*1e3"}},
                                                           {"x", 100}, {"y", 300},
                                                           {"x_axis", QJsonObject{{"notation", "decimal"}, {"decimals", 2}}},
                                                           {"y_axis", QJsonObject{{"notation", "engineering"}}}}));
        QCOMPARE(made.value("traces").toArray().first().toObject().value("points").toInt(), 3);
        QCOMPARE(made.value("x_axis").toObject().value("notation").toString(), QStringLiteral("decimal"));
        QCOMPARE(made.value("x_axis").toObject().value("decimals").toInt(), 2);
        QCOMPARE(made.value("y_axis").toObject().value("notation").toString(), QStringLiteral("engineering"));
        // The diagram writes its numbers so.
        Schematic* sch = app->currentSchematic();
        QVERIFY(sch != nullptr && !sch->a_DocDiags.empty());
        Diagram* d = sch->a_DocDiags.front();
        QCOMPARE(d->numberText(&d->xAxis, 1500.0), QStringLiteral("1500.00"));
        QCOMPARE(d->numberText(&d->yAxis, 1500.0), QStringLiteral("1.5k"));
        // Saved and read again: kept.
        QVERIFY(!failed(call("save_document")));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", "n4.sch"}})));
        sch = app->currentSchematic();
        QVERIFY(sch != nullptr && !sch->a_DocDiags.empty());
        d = sch->a_DocDiags.front();
        QCOMPARE(d->numberText(&d->xAxis, 1500.0), QStringLiteral("1500.00"));
        QCOMPARE(d->numberText(&d->yAxis, 1500.0), QStringLiteral("1.5k"));
        // null gives it the diagram's back.
        QVERIFY(!failed(call("edit_diagram", {{"diagram", 1}, {"x_axis", QJsonObject{{"notation", QJsonValue()},
                                                                                     {"decimals", QJsonValue()}}}})));
        QCOMPARE(json(call("get_schematic")).value("diagrams").toArray().first().toObject()
                     .value("x_axis").toObject().contains("notation"),
                 false);
        QCOMPARE(app->currentSchematic()->a_DocDiags.front()->numberText(&app->currentSchematic()->a_DocDiags.front()->xAxis, 1500.0),
                 QStringLiteral("1.5k"));
    }

    // N6 (and the drawing bug behind it). A box filled with a colour of
    // its own is paper itself: black words on a white box stay black on
    // the dark canvas, where they were drawn pale grey and the picture of
    // the window did not match what 'screen' drew.
    void aFilledBoxIsItsOwnPaper()
    {
        const QColor dark = qucs_s::ink::darkPaperColour();
        {
            const qucs_s::ink::Paper paper(dark);
            // On the dark canvas: a colour without contrast is lightened.
            QVERIFY(qucs_s::ink::on(QColor(Qt::black)).lightness() > 100);
        }
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n</Components>\n<Wires>\n</Wires>\n")}})));
        const QJsonObject box = call("add_painting", {{"type", "text_box"}, {"x", 100}, {"y", 100},
                                                      {"width", 220}, {"height", 90}, {"text", "Anode current"},
                                                      {"filled", true}, {"fill_color", "#ffffff"}, {"text_color", "#000000"}});
        QVERIFY2(!failed(box), qPrintable(text(box)));
        Schematic* sch = app->currentSchematic();
        QVERIFY(sch != nullptr && !sch->a_Paintings->empty());
        // Painted by itself, so that only its own pixels are counted:
        // inside its white fill the words are dark. (The box is at
        // 100,100, 220 x 90, so it lands there drawn without a transform.)
        const auto insideTheBox = [](const QImage& picture) {
            int white = 0, ink = 0, pale = 0;
            for (int y = 112; y < 188 && y < picture.height(); ++y)
                for (int x = 112; x < 318 && x < picture.width(); ++x) {
                    const QColor c = picture.pixelColor(x, y);
                    if (c.lightness() > 240) ++white;
                    else if (c.lightness() < 90) ++ink;
                    else if (c.lightness() > 110 && c.lightness() < 220) ++pale;
                }
            return std::array<int, 3>{white, ink, pale};
        };
        const auto painted = [&sch](const QColor& paper) {
            QImage picture(400, 220, QImage::Format_ARGB32);
            picture.fill(paper);
            QPainter painter(&picture);
            painter.setFont(misc::canvasFont(QucsSettings.font));
            const qucs_s::ink::Paper on(paper);
            sch->a_Paintings->front()->paint(&painter);
            return picture;
        };
        const std::array<int, 3> onDark = insideTheBox(painted(dark));
        QVERIFY2(onDark[0] > 5000, qPrintable(QStringLiteral("the white fill was not drawn (%1 light pixels)").arg(onDark[0])));
        QVERIFY2(onDark[1] > 40 && onDark[1] > onDark[2],
                 qPrintable(QStringLiteral("its words were not drawn dark on it (%1 dark, %2 pale)").arg(onDark[1]).arg(onDark[2])));
        // On light paper it is the same drawing, as prints always had it.
        const int onPaper = insideTheBox(painted(Qt::white))[1];
        QVERIFY2(std::abs(onDark[1] - onPaper) <= onPaper / 4,
                 qPrintable(QStringLiteral("dark on the canvas %1, on paper %2").arg(onDark[1]).arg(onPaper)));
        // The canvas drawn dark: what the window shows.
        misc::setWidgetBackgroundColor(sch->viewport(), dark);
        QCoreApplication::processEvents();
        const QImage drawn = sch->viewport()->grab().toImage();
        QVERIFY(!drawn.isNull());
        // And the picture 'screen' takes is that very drawing, pixel for
        // pixel (MCP wishlist N6: it drew the words black where the window
        // showed them pale grey).
        const QJsonObject shot = call("screenshot", {{"area", "screen"}, {"max_width", 4000}});
        QVERIFY2(!failed(shot), qPrintable(text(shot)));
        const QImage shown = picture(shot);
        QVERIFY(!shown.isNull());
        QCOMPARE(shown.size(), drawn.size());
        int apart = 0;
        for (int y = 0; y < shown.height(); ++y)
            for (int x = 0; x < shown.width(); ++x)
                if (shown.pixelColor(x, y).rgb() != drawn.pixelColor(x, y).rgb()) ++apart;
        QVERIFY2(apart == 0, qPrintable(QStringLiteral("%1 pixels of %2 differ from the canvas")
                                            .arg(apart).arg(shown.width() * shown.height())));
        misc::setWidgetBackgroundColor(sch->viewport(), misc::paperColor());
    }

    // N13. A picture as small as it is useful: its longer side 1500
    // pixels unless told otherwise, and 'max_width', 'scale', 'gray' and
    // 'format' smaller again. A Retina window's shot came back 2782 x
    // 2372, larger than it could be used.
    void aPictureIsAsSmallAsAsked()
    {
        QVERIFY(!failed(call("new_document")));
        QVERIFY(!failed(must("set_schematic", {{"text", QStringLiteral("<Components>\n<R R1 1 100 100 15 -26 0 1 \"1k\" 1>\n"
                                                                       "</Components>\n<Wires>\n</Wires>\n")}})));
        const QJsonObject windowShot = call("screenshot", {{"area", "window"}});
        const QImage window_ = picture(windowShot);
        QVERIFY(!window_.isNull());
        QVERIFY2(std::max(window_.width(), window_.height()) <= 1500,
                 qPrintable(QStringLiteral("%1 x %2").arg(window_.width()).arg(window_.height())));
        const QJsonObject small = call("screenshot", {{"area", "window"}, {"max_width", 400}, {"gray", true}, {"format", "jpeg"}});
        QVERIFY2(!failed(small), qPrintable(text(small)));
        const QImage asked = picture(small);
        QVERIFY(!asked.isNull());
        QVERIFY2(std::max(asked.width(), asked.height()) <= 400,
                 qPrintable(QStringLiteral("%1 x %2").arg(asked.width()).arg(asked.height())));
        // A JPEG by its type and by its bytes (its SOI marker FF D8 FF).
        QByteArray bytes;
        bool said = false;
        for (const QJsonValue& v : small.value("content").toArray())
            if (v.toObject().value("type").toString() == QLatin1String("image")) {
                said = v.toObject().value("mimeType").toString() == QLatin1String("image/jpeg");
                bytes = QByteArray::fromBase64(v.toObject().value("data").toString().toLatin1());
            }
        QVERIFY(said);
        QVERIFY2(bytes.startsWith(QByteArray::fromHex("ffd8ff")), bytes.left(8).toHex().constData());
        // And a PNG where none was asked for (its signature).
        QByteArray png;
        for (const QJsonValue& v : call("screenshot", {{"area", "window"}, {"max_width", 300}}).value("content").toArray())
            if (v.toObject().value("type").toString() == QLatin1String("image"))
                png = QByteArray::fromBase64(v.toObject().value("data").toString().toLatin1());
        QVERIFY2(png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")), png.left(8).toHex().constData());
        // The size said is the size sent, beside the size on screen.
        QVERIFY2(text(small).contains(QStringLiteral("sent %1 x %2").arg(asked.width()).arg(asked.height())),
                 qPrintable(text(small)));
        QVERIFY2(text(windowShot).contains(QStringLiteral("sent %1 x %2").arg(window_.width()).arg(window_.height()))
                     || text(windowShot).contains(QStringLiteral("the window, %1 x %2").arg(window_.width()).arg(window_.height())),
                 qPrintable(text(windowShot)));
        // A scale of its own, and what is refused.
        const QImage half = picture(call("screenshot", {{"area", "window"}, {"scale", 0.25}}));
        QVERIFY(!half.isNull() && half.width() < window_.width());
        for (const QJsonObject& wrong : {QJsonObject{{"max_width", 10}}, QJsonObject{{"scale", 9.0}},
                                         QJsonObject{{"format", "tiff"}}}) {
            QJsonObject args = wrong;
            args.insert("area", "window");
            QVERIFY2(failed(call("screenshot", args)), qPrintable(QJsonDocument(args).toJson(QJsonDocument::Compact)));
        }
    }

    // N14. What a picture of a text tab shows, in the document's own
    // measures: the lines in view, so that get_text can follow them. The
    // answer said only "as its window shows it".
    void aScreenPictureSaysWhatItShows()
    {
        QString many;
        for (int i = 1; i <= 400; ++i) many += QStringLiteral("* line %1 of the probe netlist\n").arg(i);
        QVERIFY(writeFile(path("lines.cir"), many));
        QVERIFY(!failed(call("open_document", {{"path", "lines.cir"}})));
        const QJsonObject shot = call("screenshot", {{"area", "screen"}});
        QVERIFY2(!failed(shot), qPrintable(text(shot)));
        QVERIFY2(text(shot).contains(QStringLiteral("Lines ")) && text(shot).contains(QStringLiteral("in view")), qPrintable(text(shot)));
        QVERIFY2(text(shot).contains(QStringLiteral("of 401")) || text(shot).contains(QStringLiteral("of 400")), qPrintable(text(shot)));
    }

    // N5. A plot group shown again keeps the diagram that is there, and an
    // axis label of the user's own words is not written over by the group's
    // - only one that is the same label in another unit. Its ranges,
    // colours, markers and paintings stay in any case.
    void aPlotGroupKeepsWhatTheUserSet()
    {
        QVERIFY(writeFile(path("curve.csv"), QStringLiteral("bias,semi.I_anode\n0,0\n1,1e-3\n2,2e-3\n")));
        QVERIFY(!failed(must("new_document")));
        QVERIFY(!failed(must("save_document", {{"as", "n5.sch"}})));
        QVERIFY(!failed(must("import_data", {{"file", "curve.csv"}})));
        // On its data display, as a 1D plot group's graphs are shown.
        QVERIFY(!failed(must("add_diagram", {{"document", "data_display"}, {"type", "rect"},
                                             {"traces", QJsonArray{"curve:semi.I_anode"}},
                                             {"x", 100}, {"y", 300},
                                             {"y_axis", QJsonObject{{"label", "I (A)"}}},
                                             {"x_axis", QJsonObject{{"label", "bias (V)"}}}})));
        Schematic* sch = nullptr;
        for (QucsDoc* doc : app->allDocuments())
            if (doc->getDocName().endsWith(QLatin1String("n5.dpl"))) sch = dynamic_cast<Schematic*>(doc);
        QVERIFY(sch != nullptr && !sch->a_DocDiags.empty());
        Diagram* d = sch->a_DocDiags.front();
        // The same label in another unit: written over, as before.
        QStringList removed, kept;
        QJsonObject request{{"display", path("n5.dpl")}, {"dataset", path("curve.dat")},
                            {"traces", QJsonArray{"curve:semi.I_anode"}}, {"type", "rect"},
                            {"replace", true}, {"title", d->title}, {"ylabel", "I (mA)"}, {"xlabel", "bias (V)"}};
        QVERIFY(app->showPythonDisplay(request, &removed, &kept).isEmpty());
        QCOMPARE(d->yAxis.Label, QStringLiteral("I (mA)"));
        QVERIFY(kept.isEmpty());
        // Words of the user's own: kept, and said.
        d->yAxis.Label = QStringLiteral("Anode current");
        request.insert("ylabel", "I (A)");
        kept.clear();
        QVERIFY(app->showPythonDisplay(request, &removed, &kept).isEmpty());
        QCOMPARE(d->yAxis.Label, QStringLiteral("Anode current"));
        QCOMPARE(kept.size(), 1);
        QVERIFY2(kept.first().contains(QStringLiteral("Anode current")), qPrintable(kept.first()));
        // An empty label is not the user's: filled in.
        d->yAxis.Label.clear();
        kept.clear();
        QVERIFY(app->showPythonDisplay(request, &removed, &kept).isEmpty());
        QCOMPARE(d->yAxis.Label, QStringLiteral("I (A)"));
        QVERIFY(kept.isEmpty());
        // And the diagram itself is the one that was there: its range, its
        // trace's colour and its markers were not made anew.
        QVERIFY(!failed(must("edit_diagram", {{"diagram", 1}, {"y_axis", QJsonObject{{"from", 0.0}, {"to", 0.005}}}})));
        QVERIFY(!failed(must("edit_trace", {{"diagram", 1}, {"trace", 1}, {"color", "#00aa44"}})));
        kept.clear();
        QVERIFY(app->showPythonDisplay(request, &removed, &kept).isEmpty());
        QCOMPARE(sch->a_DocDiags.size(), std::size_t(1));
        QCOMPARE(sch->a_DocDiags.front(), d);
        QCOMPARE(d->yAxis.limit_max, 0.005);
        QVERIFY(!d->yAxis.autoScale);
        QCOMPARE(d->Graphs.first()->Color.name(), QStringLiteral("#00aa44"));
    }

    // N7 and N8. Mesh convergence in one call, and reference numbers in
    // one step: the VCSEL threshold, the laser currents and the a_box wall
    // error each needed a loop of its own, and every benchmark a script.
    void aMeshConvergesAndIsMeasured()
    {
        const QString model = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/coax.qfem");
        QVERIFY(QFile::copy(model, path("coax.qfem")));
        QVERIFY(!failed(must("open_document", {{"path", "coax.qfem"}})));
        // Measured against a reference number: the difference, in per cent.
        QVERIFY(!failed(must("fem_solve", {{"timeout", 300}})));
        const QJsonObject against = json(must("fem_evaluate", {{"expression", "es.C11"}, {"reference", 1e-10}}));
        const QJsonObject value = against.value("values").toArray().first().toObject();
        QCOMPARE(value.value("reference").toDouble(), 1e-10);
        QVERIFY2(std::abs(value.value("difference %").toDouble() + 2.96) < 0.1,
                 qPrintable(QJsonDocument(value).toJson(QJsonDocument::Compact)));
        QVERIFY(against.value("against the reference").toObject().value("values measured").toInt() == 1);
        // A table's file of names and numbers, with a header and a comment.
        QVERIFY(writeFile(path("refs.csv"), QStringLiteral("# the closed form\nname,value\nes.C11,9.7e-11\nes.W,4.85e-12\n")));
        const QJsonObject table = json(must("fem_evaluate", {{"expression", "es.C11; es.W"}, {"reference", "refs.csv"}}));
        QCOMPARE(table.value("against the reference").toObject().value("references").toInt(), 2);
        QCOMPARE(table.value("against the reference").toObject().value("values measured").toInt(), 2);
        // What is refused, and a reference no value answers to.
        QVERIFY(failed(call("fem_evaluate", {{"expression", "es.C11"}, {"reference", "no-such-file.csv"}})));
        QVERIFY(failed(call("fem_evaluate", {{"expression", "es.C11"}, {"reference", QJsonObject{{"es.C11", "a lot"}}}})));
        const QJsonObject none = json(must("fem_evaluate", {{"expression", "es.C11; es.W"},
                                                            {"reference", QJsonObject{{"nothing", 1.0}}}}));
        QVERIFY2(none.value("against the reference").toObject().contains("none matched"),
                 qPrintable(QJsonDocument(none).toJson(QJsonDocument::Compact)));

        // The mesh, finer twice: each mesh's elements and values, and how
        // far they moved between the last two.
        const bool wasChanged = app->getDoc()->getDocChanged();
        const QJsonObject converged = json(must("fem_converge", {{"values", QJsonArray{"es.C11", "lp1.Z0"}},
                                                                 {"refine", QJsonArray{2, 4}}, {"timeout", 600}}, 900000));
        const QJsonArray meshes = converged.value("meshes").toArray();
        QCOMPARE(meshes.size(), 3);
        QCOMPARE(meshes.at(0).toObject().value("mesh").toString(), QStringLiteral("as it is"));
        // Each finer mesh has more of everything.
        for (int i = 1; i < meshes.size(); ++i) {
            QVERIFY(meshes.at(i).toObject().value("triangles").toInt() > meshes.at(i - 1).toObject().value("triangles").toInt());
            QVERIFY(meshes.at(i).toObject().value("largest element").toString().toDouble()
                    < meshes.at(i - 1).toObject().value("largest element").toString().toDouble());
            for (const char* watched : {"es.C11", "lp1.Z0"})
                QVERIFY2(meshes.at(i).toObject().value("values").toObject().value(QLatin1String(watched)).toObject().contains("SI"),
                         watched);
        }
        // A coax's capacitance is a closed form: it has settled.
        QVERIFY2(converged.value("converged").toBool(), qPrintable(QJsonDocument(converged).toJson(QJsonDocument::Compact)));
        // The mesh really is finer, and the answer says by how much (the
        // check of 23 October, 1: it called values "converged" that could
        // not move, on a mesh that had hardly changed).
        QVERIFY2(converged.value("the finest mesh").toString().contains(QStringLiteral("times the unknowns")),
                 qPrintable(converged.value("the finest mesh").toString()));
        QVERIFY(meshes.last().toObject().value("unknowns").toInt()
                > 2 * meshes.at(meshes.size() - 2).toObject().value("unknowns").toInt());
        QVERIFY(!converged.contains("not shown"));
        const QJsonObject moved = converged.value("between the last two meshes").toObject();
        QVERIFY(moved.value("es.C11").toObject().value("converged").toBool());
        QVERIFY(std::abs(moved.value("es.C11").toObject().value("changed by %").toDouble()) < 0.01);
        // The mesh node holds what it held, and the model is as unsaved as
        // it was: nothing of the user's is left changed.
        auto* femDoc = dynamic_cast<MultiphysicsDoc*>(app->getDoc());
        QVERIFY(femDoc != nullptr);
        QVERIFY2(femDoc->model().mesh().text(QStringLiteral("hmax")).trimmed().isEmpty(),
                 qPrintable(femDoc->model().mesh().text(QStringLiteral("hmax"))));
        QVERIFY(!femDoc->model().mesh().props.value(QStringLiteral("custom")).toBool());
        QCOMPARE(app->getDoc()->getDocChanged(), wasChanged);
        // A reference row no value answers to is named, not only counted
        // (the check of 23 October, 4).
        QVERIFY(writeFile(path("left.csv"), QStringLiteral("name,value\nes.C11,9.7e-11\nZ,5\n")));
        const QJsonObject over = json(must("fem_evaluate", {{"expression", "es.C11"}, {"reference", "left.csv"}}));
        QCOMPARE(over.value("ignored").toObject().keys(), QStringList{QStringLiteral("Z")});
        QVERIFY2(over.value("ignored").toObject().value("Z").toString().contains(QStringLiteral("es.C11")),
                 qPrintable(QJsonDocument(over).toJson(QJsonDocument::Compact)));
        // What is refused.
        QVERIFY(failed(call("fem_converge", {{"values", QJsonArray{}}})));
        QVERIFY(failed(call("fem_converge", {{"values", QJsonArray{"es.C11"}}, {"refine", QJsonArray{0.5}}})));
        QVERIFY(failed(call("fem_converge", {{"values", QJsonArray{"es.C11"}}, {"tolerance", 0}})));
    }

    // The check of 23 October, 1 and 2: a mesh that hardly changes shows
    // nothing, and a value implausible for its unit is said where it is
    // written and not only when a solve reads it.
    void aWeakRefinementShowsNothingAndAnEditIsChecked()
    {
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/coax.qfem"), path("weak.qfem")));
        QVERIFY(!failed(must("open_document", {{"path", "weak.qfem"}})));
        // A refinement of five per cent: the values cannot move, so
        // nothing is shown either way and 'converged' says so.
        const QJsonObject weak = json(must("fem_converge", {{"values", QJsonArray{"es.C11"}},
                                                            {"refine", QJsonArray{1.05}}, {"timeout", 600}}, 900000));
        QCOMPARE(weak.value("converged").toBool(), false);
        QVERIFY2(weak.value("not shown").toString().contains(QStringLiteral("could not move")),
                 qPrintable(QJsonDocument(weak).toJson(QJsonDocument::Compact)));
        QVERIFY(weak.value("the finest mesh").toString().contains(QStringLiteral("times the unknowns")));
        closeAll();

        // Every size the mesh is given is divided, not its own alone: a
        // model whose mesh is set by Size features grows all the same.
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/coax_rlgc.qfem"), path("local.qfem")));
        QVERIFY(!failed(must("open_document", {{"path", "local.qfem"}})));
        const QJsonObject local = json(must("fem_converge", {{"values", QJsonArray{"es.C11"}},
                                                             {"refine", QJsonArray{2}}, {"timeout", 900}}, 1200000));
        QVERIFY2(local.value("the finest mesh").toString().contains(QStringLiteral("sizes divided")),
                 qPrintable(local.value("the finest mesh").toString()));
        const QJsonArray grew = local.value("meshes").toArray();
        QCOMPARE(grew.size(), 2);
        QVERIFY2(grew.at(1).toObject().value("unknowns").toInt() > 2 * grew.at(0).toObject().value("unknowns").toInt(),
                 qPrintable(QJsonDocument(grew).toJson(QJsonDocument::Compact)));
        QVERIFY(local.value("converged").toBool());
        closeAll();

        // A mesh set by a Distribution's count, and a node that is not
        // custom but still holds what an earlier try left in its hidden
        // fields: the count is multiplied, and the stale size is not
        // applied by turning custom on to set the largest element.
        QVERIFY(writeFile(path("counted.qfem"), QStringLiteral(R"json({"format": "qucs-s multiphysics 1", "components": [{
            "unit": "mm", "geometry": [{"type": "rectangle", "name": "box", "position": ["0", "0"], "size": ["10", "10"]},
                                       {"type": "rectangle", "name": "dot", "position": ["5", "5"], "size": ["0.2", "0.2"]}],
            "materials": [{"material": "Air", "selection": {"all": true}}],
            "physics": [{"type": "electrostatics", "tag": "es", "features": [
                {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["dot"]}},
                {"type": "ground", "label": "Bottom", "selection": {"of": ["box"], "side": "bottom"}}]}],
            "mesh": {"size": "coarse", "custom": false, "hmin": "4", "growth": "1.01",
                     "features": [{"type": "size", "tag": "sz1", "level": "domain", "selection": {"only": ["dot"]}, "hmax": "0.05"},
                                  {"type": "distribution", "tag": "dis1", "count": 12,
                                   "selection": {"of": ["box"], "side": "top"}}]}}],
            "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}]})json")));
        QVERIFY(!failed(must("open_document", {{"path", "counted.qfem"}})));
        const QJsonObject counted = json(must("fem_converge", {{"values", QJsonArray{"es.C11"}},
                                                               {"refine", QJsonArray{2}}, {"timeout", 600}}, 900000));
        const QJsonObject set = counted.value("meshes").toArray().at(1).toObject().value("sizes set").toObject();
        QVERIFY2(!set.isEmpty(), qPrintable(QJsonDocument(counted).toJson(QJsonDocument::Compact)));
        // The Distribution's elements doubled with the sizes halved - the
        // mesh's own and the Size feature's on the small rectangle.
        QCOMPARE(set.value("dis1.count").toInt(), 24);
        QCOMPARE(set.value("sz1.hmax").toString(), QStringLiteral("0.025"));
        QCOMPARE(set.value("hmax").toString(), QStringLiteral("0.5"));
        // The smallest element is the preset's, divided - not the 4 mm an
        // earlier try left in a field that was not shown, which the node
        // still holds though it is not custom.
        QVERIFY2(set.value("hmin").toString().toDouble() > 0 && set.value("hmin").toString().toDouble() < 0.1,
                 qPrintable(QJsonDocument(set).toJson(QJsonDocument::Compact)));
        // And the mesh really is finer for it: the stale 4 mm applied as a
        // smallest element holds the small rectangle's own elements away,
        // and the unknowns then grow 1.7 times where they grow 2.6 here.
        const QJsonArray both = counted.value("meshes").toArray();
        const int before = both.at(0).toObject().value("unknowns").toInt();
        const int after = both.at(1).toObject().value("unknowns").toInt();
        QVERIFY2(after > 22 * before / 10 && after < 10 * before,
                 qPrintable(QJsonDocument(both).toJson(QJsonDocument::Compact)));
        // Each factor divides the sizes the model began with, not the
        // ones the mesh before it was given, and the mesh node and its
        // features are put back whole (the re-check of 23 October, 2).
        const QJsonObject twice = json(must("fem_converge", {{"values", QJsonArray{"es.C11"}},
                                                             {"refine", QJsonArray{2, 4}}, {"timeout", 900}}, 1200000));
        const QJsonArray steps = twice.value("meshes").toArray();
        QCOMPARE(steps.size(), 3);
        const QJsonObject at2 = steps.at(1).toObject().value("sizes set").toObject();
        const QJsonObject at4 = steps.at(2).toObject().value("sizes set").toObject();
        QCOMPARE(at2.value("hmax").toString().toDouble(), at4.value("hmax").toString().toDouble() * 2);
        QCOMPARE(at2.value("dis1.count").toInt() * 2, at4.value("dis1.count").toInt());
        QCOMPARE(at2.value("sz1.hmax").toString().toDouble(), at4.value("sz1.hmax").toString().toDouble() * 2);
        // The smallest element goes with them, or it holds the finer
        // meshes back and the steps come out uneven.
        QVERIFY2(at2.contains("hmin") && at2.value("hmin").toString().toDouble()
                                             == at4.value("hmin").toString().toDouble() * 2,
                 qPrintable(QJsonDocument(at2).toJson(QJsonDocument::Compact)));
        // The features are as they were, not as the finest mesh left them.
        auto* put = dynamic_cast<MultiphysicsDoc*>(app->getDoc());
        QVERIFY(put != nullptr);
        const FemNode* size = put->model().find(QStringLiteral("sz1"));
        QVERIFY2(size != nullptr && size->text(QStringLiteral("hmax")) == QStringLiteral("0.05"),
                 size == nullptr ? "no sz1" : qPrintable(size->text(QStringLiteral("hmax"))));
        const FemNode* dis = put->model().find(QStringLiteral("dis1"));
        QVERIFY(dis != nullptr && dis->props.value(QStringLiteral("count")).toInt() == 12);

        // A value that evaluates at no mesh is not convergence either: the
        // table is empty, and "converged" would be a word about nothing.
        const QJsonObject nothing = json(must("fem_converge", {{"values", QJsonArray{"es.nosuch"}},
                                                               {"refine", QJsonArray{2}}, {"timeout", 600}}, 900000));
        QCOMPARE(nothing.value("converged").toBool(), false);
        QVERIFY2(nothing.value("not shown").toString().contains(QStringLiteral("No value could be compared")),
                 qPrintable(QJsonDocument(nothing).toJson(QJsonDocument::Compact)));
        auto* stale = dynamic_cast<MultiphysicsDoc*>(app->getDoc());
        QVERIFY(stale != nullptr);
        QCOMPARE(stale->model().mesh().text(QStringLiteral("hmin")), QStringLiteral("4"));
        QVERIFY(!stale->model().mesh().props.value(QStringLiteral("custom")).toBool());
        closeAll();

        // A number implausible for the unit its property is in: said by
        // the edit that writes it, and not a refusal.
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/gaas_quantum_well.qfem"), path("well.qfem")));
        QVERIFY(!failed(must("open_document", {{"path", "well.qfem"}})));
        const QJsonObject wrote = json(must("fem_edit", {{"action", "set"}, {"tag", "spec1"},
                                                         {"props", QJsonObject{{"dlinewidth", "2e-10"}}}}));
        const QJsonArray said = wrote.value("check").toArray();
        QCOMPARE(said.size(), 1);
        QVERIFY2(said.first().toString().contains(QStringLiteral("dlinewidth"))
                     && said.first().toString().contains(QString::fromUtf8("C·m")),
                 qPrintable(said.first().toString()));
        // Written all the same (it may name a parameter yet to come): said
        // is not refused.
        auto* model = dynamic_cast<MultiphysicsDoc*>(app->getDoc());
        QVERIFY(model != nullptr && model->model().find(QStringLiteral("spec1")) != nullptr);
        QCOMPARE(model->model().find(QStringLiteral("spec1"))->text(QStringLiteral("dlinewidth")), QStringLiteral("2e-10"));
        // With its unit, and for a value that is plausible: nothing said.
        QVERIFY(!json(must("fem_edit", {{"action", "set"}, {"tag", "spec1"},
                                        {"props", QJsonObject{{"dlinewidth", "1[meV]/(1e7[V/m])"}}}})).contains("check"));
        QVERIFY(!json(must("fem_edit", {{"action", "set"}, {"tag", "spec1"},
                                        {"props", QJsonObject{{"linewidth", "6.6[meV]"}}}})).contains("check"));
        QVERIFY(json(must("fem_edit", {{"action", "set"}, {"tag", "spec1"},
                                       {"props", QJsonObject{{"linewidth", "6.6e-3"}}}})).contains("check"));
    }

    // The check of 23 October, 6 and 7: a PDF's lines end in one line
    // ending, and 'region' keeps the text to the lines it holds - so the
    // lines a selection gives are read by one call.
    void aRegionKeepsThePdfsLinesAndOneLineEnding()
    {
        const QString file = path("lines.pdf");
        QVERIFY(writeLinedPdf(file));
        const QJsonObject whole = json(must("read_pdf", {{"path", file}}));
        const QString all = whole.value("pages").toArray().first().toObject().value("text").toString();
        QVERIFY2(!all.contains(QLatin1Char('\r')), qPrintable(all.left(80)));
        QCOMPARE(all.split(QLatin1Char('\n')).size(), 4);
        // The top half: the lines whose box meets it, and no other.
        const QJsonObject part = json(must("read_pdf", {{"path", file}, {"region", QJsonArray{40, 120, 560, 200}}}));
        const QString kept = part.value("pages").toArray().first().toObject().value("text").toString();
        QCOMPARE(kept.split(QLatin1Char('\n')), (QStringList{all.split(QLatin1Char('\n')).at(0), all.split(QLatin1Char('\n')).at(1)}));
        QVERIFY2(part.value("of the region").toString().contains(QStringLiteral("whose box meets it")),
                 qPrintable(part.value("of the region").toString()));
        QCOMPARE(part.value("region").toArray(), (QJsonArray{40, 120, 560, 200}));
        // The lower half gives the other two.
        const QJsonObject low = json(must("read_pdf", {{"path", file}, {"region", QJsonArray{40, 210, 560, 300}}}));
        QCOMPARE(low.value("pages").toArray().first().toObject().value("text").toString().split(QLatin1Char('\n')),
                 (QStringList{all.split(QLatin1Char('\n')).at(2), all.split(QLatin1Char('\n')).at(3)}));
        // A page whose lines are made of several runs - an italic word
        // among upright ones, as a paper's are - cannot be paired box to
        // line, and the text between the region's corners is read instead
        // (the re-check of 23 October, 1). Written by Qt, which gives its
        // font the widths that reading between two points needs.
        const QString rich = path("rich.pdf");
        {
            QPdfWriter writer(rich);
            writer.setPageSize(QPageSize(QPageSize::A4));
            QTextDocument doc;
            QFont font(QStringLiteral("Helvetica"));
            font.setPointSize(13);
            doc.setDefaultFont(font);
            doc.setHtml(QStringLiteral("<p>Alpha one of the page and it runs on for a while so that it wraps onto a second "
                                       "line of its own.</p><p>Beta two with an <i>italic</i> word in the middle of it, and "
                                       "more words after that one.</p><p>Gamma three, the last of them.</p>"));
            doc.print(&writer);
        }
        QVERIFY(QFileInfo(rich).size() > 0);
        const QString allRich = json(must("read_pdf", {{"path", rich}})).value("pages").toArray().first()
                                    .toObject().value("text").toString();
        QVERIFY2(allRich.contains(QStringLiteral("italic")), qPrintable(allRich.left(200)));
        const QJsonObject middle = json(must("read_pdf", {{"path", rich}, {"region", QJsonArray{0.05, 0.08, 0.95, 0.17}}}));
        const QString someOfIt = middle.value("pages").toArray().first().toObject().value("text").toString();
        // Its boxes are not one to a line, so the region cannot be applied
        // that way; what comes back is of the page and the answer says how
        // it was read. (Which of the two other ways it takes is the
        // document's to decide: reading between two points answers nothing
        // on a PDF a writer made here, and does answer on a real paper -
        // the one the re-check used, tried by hand.)
        QVERIFY2(!someOfIt.isEmpty(), "nothing came back for the region");
        for (const QString& line : someOfIt.split(QLatin1Char('\n')))
            QVERIFY2(line.trimmed().isEmpty() || allRich.contains(line.trimmed()), qPrintable(line));
        QVERIFY2(!middle.value("of the region").toString().isEmpty(), "the answer did not say how it read the region");

        // A region that starts in the margin, which is where one drawn by
        // hand usually starts: the two points are moved onto the first and
        // last run it meets, since Qt reads between the characters nearest
        // them and a corner in the margin is near none (the second
        // re-check of 23 October, 1). Here the whole page is one margin
        // wider than the text, and the text comes back all the same.
        const QJsonObject wide = json(must("read_pdf", {{"path", rich}, {"region", QJsonArray{0.0, 0.08, 1.0, 0.17}}}));
        const QString fromMargin = wide.value("pages").toArray().first().toObject().value("text").toString();
        QVERIFY2(!fromMargin.isEmpty(), qPrintable(wide.value("of the region").toString()));
        // Less than the page: with the region's own corners, which lie in
        // the margin, nothing is found there and the whole page comes back.
        QVERIFY2(fromMargin.size() < allRich.size(),
                 qPrintable(QStringLiteral("%1 of %2 chars, by \"%3\"").arg(fromMargin.size()).arg(allRich.size())
                                .arg(wide.value("of the region").toString().left(40))));
        QVERIFY2(!wide.value("of the region").toString().startsWith(QStringLiteral("nothing")),
                 qPrintable(wide.value("of the region").toString()));
        // A region with no text of the page in it says so, and is not the
        // page (a corner above the first line; the foot has the page
        // number Qt's writer puts there).
        const QJsonObject empty = json(must("read_pdf", {{"path", rich}, {"region", QJsonArray{0.01, 0.005, 0.06, 0.02}}}));
        QCOMPARE(empty.value("pages").toArray().first().toObject().value("text").toString(), QString());
        QVERIFY2(empty.value("of the region").toString().startsWith(QStringLiteral("nothing")),
                 qPrintable(empty.value("of the region").toString()));

        // A region outside the page is refused, as it is for a picture.
        QVERIFY(failed(call("read_pdf", {{"path", file}, {"region", QJsonArray{900, 900, 1000, 1000}}})));
    }

    // The second re-check of 23 October, 3: a model whose file changes
    // under it is read again, and the conversation is told - as a
    // schematic's is. (The report was of a tab that kept what it had; it
    // does not happen here, and the one case where a reload is refused -
    // unsaved changes - says so in the answer.)
    void aModelTabFollowsItsFileOnDisk()
    {
        const QString from = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
        QVERIFY(QFile::copy(from + QStringLiteral("coax.qfem"), path("follows.qfem")));
        QVERIFY(!failed(must("open_document", {{"path", "follows.qfem"}})));
        auto* doc = dynamic_cast<MultiphysicsDoc*>(app->getDoc());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->model().objectNames().size(), 2);   // shield, core
        const quint64 was = doc->revision();

        // Another model written over it, as a command of Claude's or
        // another program would.
        QVERIFY(QFile::remove(path("follows.qfem")));
        QVERIFY(QFile::copy(from + QStringLiteral("coax_rlgc.qfem"), path("follows.qfem")));
        QTRY_VERIFY_WITH_TIMEOUT(doc->model().objectNames().size() == 3, 15000);   // outer, dielectric, inner
        QVERIFY(doc->revision() > was);
        QVERIFY(!doc->getDocChanged());
        // The tools see the model of the file, not the one that was there.
        const QJsonObject now = json(must("fem_describe", {{"what", "tree"}}));
        const QJsonArray objects = now.value("geometry").toObject().value("objects").toArray();
        QVERIFY2(objects.contains(QJsonValue(QStringLiteral("dielectric"))),
                 qPrintable(QJsonDocument(objects).toJson(QJsonDocument::Compact)));
    }

    // The third re-check of 23 October, nit 1: a call made straight after
    // a write sees the file. The watcher waits 400 ms for a file written
    // in pieces, which a tool call outruns - so a call of its own reads
    // again as it begins.
    void aCallRightAfterAWriteSeesTheFile()
    {
        const QString from = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
        QVERIFY(QFile::copy(from + QStringLiteral("coax.qfem"), path("race.qfem")));
        QVERIFY(!failed(must("open_document", {{"path", "race.qfem"}})));
        auto* doc = dynamic_cast<MultiphysicsDoc*>(app->getDoc());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->model().objectNames().size(), 2);   // shield, core
        const quint64 was = doc->revision();

        // Written over with no pause at all, and read in the same breath.
        QVERIFY(QFile::remove(path("race.qfem")));
        QVERIFY(QFile::copy(from + QStringLiteral("coax_rlgc.qfem"), path("race.qfem")));
        // Nothing has run the event loop, so the watcher cannot have.
        QCOMPARE(doc->model().objectNames().size(), 2);
        const QJsonObject now = json(must("fem_describe", {{"what", "tree"}}));
        const QJsonArray objects = now.value("geometry").toObject().value("objects").toArray();
        QVERIFY2(objects.contains(QJsonValue(QStringLiteral("dielectric"))),
                 qPrintable(QJsonDocument(objects).toJson(QJsonDocument::Compact)));
        QCOMPARE(doc->model().objectNames().size(), 3);   // outer, dielectric, inner
        QVERIFY(doc->revision() > was);
        QVERIFY(!doc->getDocChanged());

        // A tab with unsaved changes keeps them, and the call that found
        // the file changed says so.
        QVERIFY(writeFile(path("held.sch"), QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n"
                                                           "<Symbol>\n</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n"
                                                           "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")));
        QVERIFY(!failed(must("open_document", {{"path", "held.sch"}})));
        auto* sch = dynamic_cast<Schematic*>(app->getDoc());
        QVERIFY(sch != nullptr);
        QVERIFY(!failed(must("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}})));
        QVERIFY(sch->getDocChanged());
        QVERIFY(writeFile(path("held.sch"), QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n"
                                                           "<Symbol>\n</Symbol>\n<Components>\n<R R1 1 300 300 ...>\n</Components>\n"
                                                           "<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")));
        // (A conversation of its own, since that is who the notes are for.)
        const QString told = text(control->callNow(QStringLiteral("get_netlist"), {}, 60000, 7));
        QVERIFY2(told.contains(QStringLiteral("unsaved changes in Qucs-S")), qPrintable(told.left(800)));
        QVERIFY(sch->getDocChanged());   // kept, not thrown away
        QCOMPARE(sch->a_Components->size(), size_t(1));
    }

    // N10. A script stopped says what it was doing, and the time the user
    // took to answer a card is not counted against its own timeout.
    void aStoppedScriptSaysWhatItWasDoing()
    {
        if (!control->scriptingBuilt()) QSKIP("this build has no JavaScript engine");
        QVERIFY(!failed(must("new_document")));
        const QJsonObject stopped = json(call("run_script", {{"script", "while (true) { }"}, {"timeout", 1}}, 30000));
        QVERIFY2(stopped.value("error").toString().contains(QStringLiteral("it had called no tool")),
                 qPrintable(stopped.value("error").toString()));
        QVERIFY(stopped.value("error").toString().contains(QStringLiteral("stopped after 1 s of its own")));
        // One stopped inside a call says which call it was.
        const QJsonObject inCall = json(call("run_script", {{"script", "for (let i = 0; i < 100000; i++) "
                                                                       "qucs.call('add_component', {type: 'R', x: 100, y: 100});"},
                                                            {"timeout", 1}}, 60000));
        // (Between its calls, or inside one: either way it names the call.)
        QVERIFY2(inCall.value("error").toString().contains(QStringLiteral("add_component")),
                 qPrintable(inCall.value("error").toString()));
        QVERIFY2(inCall.value("error").toString().contains(QStringLiteral("its call")),
                 qPrintable(inCall.value("error").toString()));
        // A tool's refusal inside a script names it once: the tool says its
        // own name already (the re-check of 23 October, 3).
        const QJsonObject refused = json(call("run_script",
                                              {{"script", "qucs.call('get_dataset', {path: 'none.dat', points: 'abc'})"}},
                                              30000));
        const QString why = refused.value("error").toString();
        QVERIFY2(why.contains(QStringLiteral("get_dataset")), qPrintable(why));
        QCOMPARE(why.count(QStringLiteral("get_dataset:")), 1);
        QVERIFY(inCall.value("calls").toInt() > 1);
        // The user's time is not the script's: a card answered after a
        // while does not stop a script whose own timeout is shorter, and
        // 'seconds' is the script's own.
        QVERIFY(writeFile(path("taken.sch"), QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n"
                                                            "<Symbol>\n</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n"
                                                            "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")));
        QVERIFY(!failed(must("new_document")));
        control->setAsker(0, [](const QString&, const QJsonObject&, const std::function<void(const QJsonObject&)>& answer) {
            // (As the user would: after a while, from the event loop.)
            QTimer::singleShot(1600, [answer] { answer(QJsonObject{{QStringLiteral("action"), QStringLiteral("accept")},
                                                                   {QStringLiteral("content"), QJsonObject{{QStringLiteral("confirm"), true}}}}); });
        });
        const qint64 before = control->askedUserMs();
        const QJsonObject waited = json(call("run_script", {{"script", "qucs.call('save_document', {as: 'taken.sch'}); 1"},
                                                            {"timeout", 1}}, 60000));
        control->setAsker(0, nullptr);
        QVERIFY2(!waited.contains("error"), qPrintable(QJsonDocument(waited).toJson(QJsonDocument::Compact)));
        // Longer than the script's own 'timeout': counted against it, the
        // script would have been stopped.
        QVERIFY2(control->askedUserMs() - before >= 1400, qPrintable(QString::number(control->askedUserMs() - before)));
        QVERIFY2(waited.value("waited for the user").toDouble() >= 1.4,
                 qPrintable(QJsonDocument(waited).toJson(QJsonDocument::Compact)));
        // Its own time leaves that out, so its 1 s timeout was not reached.
        QVERIFY2(waited.value("seconds").toDouble() < 1.0, qPrintable(QJsonDocument(waited).toJson(QJsonDocument::Compact)));
    }

    // N12. What the user selected, as data: a text tab's selected lines
    // (a PDF's text with its boxes in page points, where Qt's PDF module
    // is built in - test_pdf_viewer covers that side).
    void theSelectionIsGivenAsData()
    {
        QVERIFY(writeFile(path("select.cir"), QStringLiteral("* one\n* two\n* three\n* four\n")));
        QVERIFY(!failed(call("open_document", {{"path", "select.cir"}})));
        QVERIFY(!json(call("get_state")).value("documents").toArray().first().toObject().contains("selection"));
        auto* text = dynamic_cast<TextDoc*>(app->getDoc());
        QVERIFY(text != nullptr);
        QTextCursor cursor = text->textCursor();
        cursor.setPosition(text->document()->findBlockByNumber(1).position());
        cursor.setPosition(text->document()->findBlockByNumber(2).position() + 7, QTextCursor::KeepAnchor);
        text->setTextCursor(cursor);
        const QJsonObject selected = json(call("get_state")).value("documents").toArray().first().toObject()
                                         .value("selection").toObject();
        QVERIFY2(!selected.isEmpty(), "get_state said nothing of the selection");
        QCOMPARE(selected.value("from line").toInt(), 2);
        QCOMPARE(selected.value("to line").toInt(), 3);
        QVERIFY2(selected.value("text").toString().contains(QStringLiteral("two")), qPrintable(selected.value("text").toString()));
        // get_ui area document says it too, where it was looked for first.
        QVERIFY(!json(call("get_ui", {{"area", "document"}})).value("selection").toObject().isEmpty());
    }
};

QTEST_MAIN(TestMcpWishlist)
#include "test_mcp_wishlist.moc"
