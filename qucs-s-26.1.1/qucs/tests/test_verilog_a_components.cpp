/*
 * A Verilog-A module as a Qucs component (vamodule.h): its parameters read
 * from the library OpenVAF builds (OSDI) or from the source, their
 * descriptions with their units, the JSON files the component is made of -
 * valid JSON, read as it is: descriptions and texts keep their spaces and
 * quotes (the old reader took every space out of the file, a quote in a
 * description made the whole component empty), older files with trailing
 * commas still read. A symbol in a project subfolder, the icon.
 */
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "vamodule.h"
#include "components/component.h"
#include "components/vacomponent.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::vamodule;

namespace {

// Two modules; comments, a local parameter and a variable that are no
// parameters; ranges, a list, a string, attributes with quotes in them.
// (Plain literals: moc reads this file, and a raw string with an apostrophe
// in it throws its lexer off - the class then goes without its moc code.)
const char* kSource =
    "`include \"disciplines.vams\"\n"
    "// parameter real commented = 1;\n"
    "/* parameter real blocked = 2; */\n"
    "module first(a, b);\n"
    "    inout a, b;\n"
    "    electrical a, b;\n"
    "    (* desc=\"The first module's gain\" *) parameter real g = 2;\n"
    "    analog I(a, b) <+ g * V(a, b);\n"
    "endmodule\n"
    "\n"
    "module vdev(p, n);\n"
    "    inout p, n;\n"
    "    electrical p, n;\n"
    "    (*desc=\"Resistance at the nominal temperature\", units=\"Ohm\"*) parameter real r = 1e3 from (0:inf);\n"
    "    (* desc = \"Width\", units = \"m\", type = \"instance\" *) parameter real w = 1u from (0:inf);\n"
    "    parameter integer nf = 1 from [1:inf) exclude 3;\n"
    "    (*desc=\"Nominal \\\"reference\\\" temperature\", units=\"C\"*) parameter real tnom = 27;\n"
    "    parameter real a = 1.0, b = 2e-3 from (0:1);\n"
    "    parameter string kind = \"nmos\";\n"
    "    localparam real hidden = 5;\n"
    "    (*desc=\"Current through it\", units=\"A\"*) real ir;\n"
    "    analog begin\n"
    "        ir = V(p, n) / r;\n"
    "        I(p, n) <+ ir * w * nf * a * b;\n"
    "    end\n"
    "endmodule\n";

const Parameter* find(const VerilogModule& m, const QString& name)
{
    for (const Parameter& p : m.parameters)
        if (p.name == name) return &p;
    return nullptr;
}

QStringList names(const VerilogModule& m)
{
    QStringList n;
    for (const Parameter& p : m.parameters) n << p.name;
    return n;
}

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

// Sets the modification time of \a path.
bool touch(const QString& path, const QDateTime& when)
{
    QFile f(path);
    return f.open(QIODevice::ReadWrite) && f.setFileTime(when, QFileDevice::FileModificationTime);
}

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// OpenVAF, if it is here: QUCS_OPENVAF, else openvaf-r or openvaf on PATH.
QString openVaf()
{
    const QString given = qEnvironmentVariable("QUCS_OPENVAF");
    if (!given.isEmpty()) return given;
    for (const char* name : {"openvaf-r", "openvaf"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty()) return found;
    }
    return QString();
}

// Compiles \a va into \a osdi; empty on success, the output otherwise.
QString compile(const QString& compiler, const QString& va, const QString& osdi)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(compiler, {va, QStringLiteral("-o"), osdi});
    if (!p.waitForFinished(120000) || p.exitCode() != 0 || !QFileInfo::exists(osdi))
        return QString::fromLocal8Bit(p.readAll()) + p.errorString();
    return QString();
}

} // namespace

class TestVerilogAComponents : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void theSourceIsRead()
    {
        const VerilogModule m = readSource(QString::fromUtf8(kSource), "vdev");
        QCOMPARE(m.name, QStringLiteral("vdev"));
        QCOMPARE(names(m), QStringList({"r", "w", "nf", "tnom", "a", "b", "kind"}));
        const Parameter* r = find(m, "r");
        QCOMPARE(r->value, QStringLiteral("1e3"));   // the range is not part of it
        QCOMPARE(r->description, QStringLiteral("Resistance at the nominal temperature"));
        QCOMPARE(r->units, QStringLiteral("Ohm"));
        QVERIFY(!r->instance);
        QVERIFY(find(m, "w")->instance);
        QCOMPARE(find(m, "w")->value, QStringLiteral("1u"));
        QCOMPARE(find(m, "nf")->value, QStringLiteral("1"));
        QCOMPARE(find(m, "tnom")->description, QStringLiteral("Nominal \"reference\" temperature"));
        QCOMPARE(find(m, "b")->value, QStringLiteral("2e-3"));
        QCOMPARE(find(m, "kind")->value, QStringLiteral("\"nmos\""));

        // Another module by its name, any case; the first when there is none.
        QCOMPARE(names(readSource(QString::fromUtf8(kSource), "FIRST")), QStringList({"g"}));
        QCOMPARE(readSource(QString::fromUtf8(kSource), "nosuch").name, QStringLiteral("first"));
        QVERIFY(readSource("no module here").name.isEmpty());
    }

    // The properties: descriptions with their units, instance parameters
    // shown - and back through the component, every character intact.
    void thePropertiesFile()
    {
        const VerilogModule m = readSource(QString::fromUtf8(kSource), "vdev");
        const QJsonObject props = propsObject(m);
        QCOMPARE(props["Model"].toString(), QStringLiteral("vdev"));
        const QJsonArray list = props["property"].toArray();
        QCOMPARE(list.size(), 7);
        QCOMPARE(list[0].toObject()["desc"].toString(), QStringLiteral("Resistance at the nominal temperature [Ohm]"));
        QCOMPARE(list[1].toObject()["display"].toString(), QStringLiteral("true"));
        QCOMPARE(list[2].toObject()["desc"].toString(), QStringLiteral("-"));   // neither description nor units

        const QString file = dir.filePath("vdev_props.json");
        QVERIFY(write(file, QJsonDocument(props).toJson()));
        vacomponent c(file);
        QCOMPARE(c.Model, QStringLiteral("vdev"));
        QCOMPARE(c.Description, QStringLiteral("vdev verilog device"));
        QCOMPARE(c.Props.size(), 7);
        QCOMPARE(c.Props.at(3)->Description, QStringLiteral("Nominal \"reference\" temperature [C]"));
        QCOMPARE(c.Props.at(6)->Value, QStringLiteral("\"nmos\""));
    }

    // What older versions wrote: trailing commas, and the rest as it is.
    void olderFilesStillRead()
    {
        const QByteArray old =
            "{\n  \"description\" : \"vres verilog device\",\n  \"property\" : [\n"
            "    { \"name\" : \"r\", \"value\" : \"1000\", \"display\" : \"false\", \"desc\" : \"Resistance, in Ohm ]\"},\n"
            "  ],\n\n  \"tx\" : 4,\n  \"ty\" : 4,\n  \"Model\" : \"vres\",\n  \"BitmapFile\" : \"vres\",\n}";
        QString why;
        const QJsonObject json = parseJson(old, &why);
        QVERIFY2(!json.isEmpty(), qPrintable(why));
        QCOMPARE(json["description"].toString(), QStringLiteral("vres verilog device"));
        // A comma and a bracket inside a string are text.
        QCOMPARE(json["property"].toArray()[0].toObject()["desc"].toString(), QStringLiteral("Resistance, in Ohm ]"));
        QCOMPARE(json["tx"].toInt(), 4);

        QVERIFY(parseJson("{\"a\": ", &why).isEmpty());
        QVERIFY2(why.contains("character"), qPrintable(why));
        QVERIFY(parseJson("[1, 2]", &why).isEmpty());   // not an object
        QVERIFY(!why.isEmpty());

        // The symbol's text as a JSON string.
        QCOMPARE(jsonString("Say \"hi\" \\ there"), QStringLiteral("\"Say \\\"hi\\\" \\\\ there\""));
        QCOMPARE(merged(QJsonObject{{"tx", 4}, {"Model", "m"}}, QJsonObject{{"tx", -30}, {"x1", 1}}),
                 QJsonObject({{"tx", -30}, {"Model", "m"}, {"x1", 1}}));
    }

    // Saving the symbol of a Verilog-A file in a project subfolder writes
    // the three files next to it; the component made of them has the
    // parameters, the text of the symbol, and its ports.
    void aSymbolInASubfolder()
    {
        const QString sub = dir.filePath("project_prj/models");
        QVERIFY(write(sub + "/vdev.va", kSource));
        QVERIFY(write(sub + "/vdev.sym",
                      "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <DataDisplay=vdev.va>\n</Properties>\n"
                      "<Symbol>\n"
                      "  <Rectangle -20 -30 40 60 #000080 2 1 #c0c0c0 1 0>\n"
                      "  <.PortSym -30 0 1 0 P1>\n"
                      "  <.PortSym 30 0 2 180 P2>\n"
                      "  <Text -15 -20 12 #000000 0 \"V out \"x\" \\ 2\">\n"
                      "  <.ID -20 34 T>\n"
                      "</Symbol>\n"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("project_prj"));   // elsewhere than the symbol
        Schematic sym(nullptr, sub + "/vdev.sym");
        QVERIFY(sym.load());
        sym.setDataDisplay("vdev.va");
        sym.setChanged(true);
        QVERIFY(sym.save() >= 0);
        for (const char* made : {"/vdev_props.json", "/vdev_sym.json", "/vdev_symbol.json"})
            QVERIFY2(QFileInfo::exists(sub + made), made);

        QString why;
        QVERIFY2(!parseJson(read(sub + "/vdev_sym.json"), &why).isEmpty(), qPrintable(why));
        vacomponent c(sub + "/vdev_symbol.json");
        QCOMPARE(c.Model, QStringLiteral("vdev"));
        QCOMPARE(c.Props.size(), 7);
        QCOMPARE(c.Props.at(0)->Description, QStringLiteral("Resistance at the nominal temperature [Ohm]"));
        QCOMPARE(c.Texts.size(), 1);
        // As Qucs keeps texts (misc::convert2ASCII: a backslash doubled) -
        // through JSON with its quotes and spaces.
        QCOMPARE(c.Texts.first()->s, QStringLiteral("V out \"x\" \\\\ 2"));
        QCOMPARE(c.Ports.size(), 2);

        // The icon, and nothing else, changes.
        QVERIFY2(setIcon(sub + "/vdev_symbol.json", "vdev icon", &why), qPrintable(why));
        const QJsonObject after = parseJson(read(sub + "/vdev_symbol.json"));
        QCOMPARE(after["BitmapFile"].toString(), QStringLiteral("vdev icon"));
        QCOMPARE(after["Model"].toString(), QStringLiteral("vdev"));
        QCOMPARE(after["property"].toArray().size(), 7);
        QVERIFY(!setIcon(sub + "/missing.json", "x", &why));
        QVERIFY(!why.isEmpty());
    }

    // With OpenVAF: the library's parameters - units, instance ones, the
    // defaults as the model sets them, the module of the name - and the
    // library is used only while it is not older than the source.
    void theLibraryIsRead()
    {
        const QString compiler = openVaf();
        if (compiler.isEmpty()) QSKIP("OpenVAF is not installed (QUCS_OPENVAF, openvaf-r, openvaf)");
        const QString lib = dir.filePath("lib");
        QVERIFY(write(lib + "/vdev.va", kSource));
        const QString failed = compile(compiler, lib + "/vdev.va", lib + "/vdev.osdi");
        QVERIFY2(failed.isEmpty(), qPrintable(failed));

        VerilogModule m;
        QString why;
        QVERIFY2(readOsdi(lib + "/vdev.osdi", "vdev", &m, &why), qPrintable(why));
        QCOMPARE(m.name, QStringLiteral("vdev"));
        QVERIFY2(!names(m).contains("$mfactor"), qPrintable(names(m).join(", ")));
        for (const char* name : {"r", "w", "nf", "tnom", "a", "b", "kind"})
            QVERIFY2(find(m, name) != nullptr, name);
        QCOMPARE(find(m, "r")->value, QStringLiteral("1000"));
        QCOMPARE(find(m, "r")->units, QStringLiteral("Ohm"));
        QCOMPARE(find(m, "tnom")->description, QStringLiteral("Nominal \"reference\" temperature"));
        QVERIFY(find(m, "w")->instance);
        QVERIFY(!find(m, "r")->instance);
        QCOMPARE(find(m, "w")->value, QStringLiteral("1e-06"));
        QCOMPARE(find(m, "nf")->value, QStringLiteral("1"));
        // The text, not a pointer's bytes - quoted: ngspice's .model takes
        // kind="nmos" and refuses kind=nmos.
        QCOMPARE(find(m, "kind")->value, QStringLiteral("\"nmos\""));

        // Two modules in one library: the one of the name.
        VerilogModule other;
        QVERIFY(readOsdi(lib + "/vdev.osdi", "First", &other));
        QCOMPARE(other.name, QStringLiteral("first"));
        QCOMPARE(names(other), QStringList({"g"}));

        // Not a library.
        QVERIFY(!readOsdi(lib + "/vdev.va", "vdev", &m, &why));
        QVERIFY(!why.isEmpty());
        QVERIFY(!readOsdi(lib + "/none.osdi", "vdev", &m, &why));

        // The symbol's save: the library while it is as new as the source.
        QVERIFY(write(lib + "/vdev.sym", "<Qucs Schematic " PACKAGE_VERSION ">\n<Symbol>\n"
                                         "  <.PortSym -30 0 1 0 P1>\n  <.PortSym 30 0 2 180 P2>\n</Symbol>\n"));
        QVERIFY(touch(lib + "/vdev.va", QDateTime::currentDateTime().addSecs(-60)));
        {
            Schematic sym(nullptr, lib + "/vdev.sym");
            QVERIFY(sym.load());
            QCOMPARE(sym.savePropsJSON(), 0);
        }
        // w is 1e-06 to the library, 1u in the source.
        const auto widthOf = [&lib] {
            for (const QJsonValue& v : parseJson(read(lib + "/vdev_props.json"))["property"].toArray())
                if (v.toObject()["name"].toString() == "w") return v.toObject()["value"].toString();
            return QString();
        };
        QCOMPARE(widthOf(), QStringLiteral("1e-06"));      // from the library
        QVERIFY(touch(lib + "/vdev.va", QDateTime::currentDateTime().addSecs(60)));
        {
            Schematic sym(nullptr, lib + "/vdev.sym");
            QVERIFY(sym.load());
            QCOMPARE(sym.savePropsJSON(), 0);
        }
        QCOMPARE(widthOf(), QStringLiteral("1u"));         // the source is newer: from it
    }
};

QTEST_MAIN(TestVerilogAComponents)
#include "test_verilog_a_components.moc"
