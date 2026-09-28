/*
 * Claude's Qucs-S tools as an MCP server (mcpserver.h), driven by its own
 * protocol - JSON-RPC messages in, answers and the server's own messages
 * out - on the application, and over stdio against the real program
 * (qucs-s --mcp-server): the tools listed with their annotations, their
 * tiers and summaries (describe_tool the whole); results structured; the
 * resources - the state, a schematic's text, netlist, netlist map and
 * dataset - read, subscribed to and told of when they change (also when
 * another program writes the file); the user asked (elicitation) before a
 * file is written over; a change previewed and put back; a schematic
 * diffed; the JSON form of set_schematic, checked against the types; the
 * values and traps told.
 */
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "config.h"
#include "isolated_settings.h"
#include "main.h"
#include "mcpserver.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "extsimkernels/spicecompat.h"

using qucs_s::mcp::Server;

class TestMcpServer : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;
    Server* server = nullptr;
    QList<QJsonObject> sent;   // the server's own messages
    int nextId = 100;

    // A request, and its answer (the event loop run until it comes).
    QJsonObject request(const QString& method, const QJsonObject& params = {})
    {
        QJsonObject answer;
        bool answered = false;
        const int id = ++nextId;
        QJsonObject message{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.isEmpty()) message.insert("params", params);
        server->handle(message, [&](const QJsonObject& r) {
            answer = r;
            answered = true;
        });
        QElapsedTimer t;
        t.start();
        while (!answered && t.elapsed() < 30000) QTest::qWait(5);
        if (!answered) qWarning("no answer to %s", qPrintable(method));
        return answer;
    }
    QJsonObject callTool(const QString& tool, const QJsonObject& arguments = {})
    {
        return request("tools/call", {{"name", tool}, {"arguments", arguments}}).value("result").toObject();
    }
    // A schematic's text without where its view is.
    static QString parts(QString text) { return text.remove(QRegularExpression(QStringLiteral("  <View=[^>]*>\n"))); }
    static QJsonObject structured(const QJsonObject& result) { return result.value("structuredContent").toObject(); }
    Schematic* front() const { return app->currentSchematic(); }
    QString write(const QString& name, const QByteArray& text)
    {
        QFile f(dir.filePath("workspace/" + name));
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(text);
        return f.fileName();
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
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        server = new Server(control, this);
        server->setSend([this](const QJsonObject& m) { sent << m; });
        server->setCaller(7);
        server->setCall([this](const QString& tool, const QJsonObject& args, std::function<void(const QJsonObject&)> done) {
            control->callToolFor(7, tool, args, done);
        });
        // A client that can ask its user.
        const QJsonObject init = request("initialize", {{"protocolVersion", "2025-06-18"},
                                                        {"capabilities", QJsonObject{{"elicitation", QJsonObject()}}}});
        const QJsonObject caps = init.value("result").toObject().value("capabilities").toObject();
        QVERIFY(caps.contains("tools"));
        QVERIFY(caps.value("resources").toObject().value("subscribe").toBool());
        QVERIFY(caps.value("resources").toObject().value("listChanged").toBool());
        QVERIFY(init.value("result").toObject().value("instructions").toString().contains("describe_tool"));
        QVERIFY(server->canElicit());
    }

    void cleanupTestCase()
    {
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
        delete server;
        server = nullptr;
        delete app;
        QucsMain = nullptr;
    }

    // Every tool: a name, a summary (the whole with describe_tool), an
    // object schema, MCP's annotations - those that look, those that only
    // add, those that may change or take away - and its tier.
    void theToolsAreAnnotatedAndTiered()
    {
        const QJsonArray tools = request("tools/list").value("result").toObject().value("tools").toArray();
        QVERIFY(tools.size() >= 60);
        QHash<QString, QJsonObject> byName;
        for (const QJsonValue& v : tools) {
            const QJsonObject t = v.toObject();
            byName.insert(t.value("name").toString(), t);
            QVERIFY2(!t.value("description").toString().isEmpty() && t.value("description").toString().size() < 320,
                     qPrintable(t.value("name").toString()));
            QCOMPARE(t.value("inputSchema").toObject().value("type").toString(), QStringLiteral("object"));
            QVERIFY(t.value("annotations").toObject().contains("readOnlyHint"));
            QVERIFY(!t.value("annotations").toObject().value("openWorldHint").toBool());
            const QJsonObject meta = t.value("_meta").toObject();
            QVERIFY2(meta.value("anthropic/alwaysLoad").toBool() || !meta.value("anthropic/searchHint").toString().isEmpty(),
                     qPrintable(t.value("name").toString()));
        }
        const auto annotation = [&](const char* tool, const char* key) { return byName.value(tool).value("annotations").toObject().value(key).toBool(); };
        QVERIFY(annotation("get_schematic", "readOnlyHint"));
        QVERIFY(!annotation("get_schematic", "destructiveHint"));
        QVERIFY(annotation("delete", "destructiveHint"));
        QVERIFY(annotation("clean_scratch", "destructiveHint"));
        QVERIFY(!annotation("add_component", "destructiveHint"));
        QVERIFY(!annotation("add_component", "readOnlyHint"));
        // Those most sessions use are in every turn; the rest found when needed.
        QVERIFY(byName.value("get_state").value("_meta").toObject().value("anthropic/alwaysLoad").toBool());
        QVERIFY(byName.value("add_component").value("_meta").toObject().value("anthropic/alwaysLoad").toBool());
        QVERIFY(!byName.value("tune").value("_meta").toObject().contains("anthropic/alwaysLoad"));
        QVERIFY(byName.value("tune").value("_meta").toObject().value("anthropic/searchHint").toString().contains("tune"));
        // The schematic-changing tools take 'preview'.
        QVERIFY(byName.value("add_component").value("inputSchema").toObject().value("properties").toObject().contains("preview"));
        QVERIFY(!byName.value("simulate").value("inputSchema").toObject().value("properties").toObject().contains("preview"));
        QCOMPARE(byName.contains("run_script"), QucsControl::scriptingBuilt());
        QVERIFY(byName.contains("diff") && byName.contains("describe_tool"));
        // The whole of one.
        const QJsonObject whole = structured(callTool("describe_tool", {{"name", "get_dataset"}}));
        QVERIFY(whole.value("description").toString().size() > byName.value("get_dataset").value("description").toString().size() * 3);
        QVERIFY(whole.value("inputSchema").toObject().value("properties").toObject().contains("variables"));
        QVERIFY(structured(callTool("describe_tool")).value("tools").toArray().size() == tools.size());
        QVERIFY(callTool("describe_tool", {{"name", "nothing"}}).value("isError").toBool());
    }

    // With no simulator chosen (none found at the start), a netlist and a
    // simulation are refused with the reason, not with nothing.
    void noSimulatorIsSaidSo()
    {
        callTool("new_document", {{"kind", "schematic"}});
        callTool("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}});
        callTool("save_document", {{"as", dir.filePath("workspace/nosim.sch")}, {"replace", true}});
        const int before = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
        const QJsonObject netlist = callTool("get_netlist", {{"map", true}});
        const QJsonObject simulated = callTool("simulate");
        QucsSettings.DefaultSimulator = before;
        QVERIFY2(netlist.value("isError").toBool() && structured(netlist).value("error").toString().contains("No simulator is chosen"),
                 qPrintable(QJsonDocument(netlist).toJson()));
        QVERIFY2(simulated.value("isError").toBool() && structured(simulated).value("error").toString().contains("set_simulator"),
                 qPrintable(QJsonDocument(simulated).toJson()));
        callTool("close_document", {{"unsaved", "discard"}});
    }

    // Results are structured: a tool's JSON as it is, a text as a summary,
    // an error as the error - and a note of what the tool did beside it.
    void resultsAreStructured()
    {
        QJsonObject r = callTool("new_document", {{"kind", "schematic"}});
        QVERIFY(structured(r).value("summary").toString().contains("new document"));
        r = callTool("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}});
        QCOMPARE(structured(r).value("name").toString(), QStringLiteral("R1"));
        r = callTool("add_component", {{"type", "NoSuchType"}, {"x", 0}, {"y", 0}});
        QVERIFY(r.value("isError").toBool());
        QVERIFY(structured(r).value("error").toString().contains("NoSuchType"));
        QCOMPARE(qucs_s::mcp::structuredOf(QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "[1,2]"}}}}})
                     .value("items").toArray().size(), 2);
        callTool("close_document", {{"unsaved", "discard"}});
    }

    // Resources: the state; of a saved schematic its text (unsaved changes
    // too), netlist, netlist map; a dataset once there is one. Subscribed:
    // told when they change - also when another program writes the file -
    // and told when what there is changes.
    void resourcesAreReadAndWatched()
    {
        callTool("new_document", {{"kind", "schematic"}});
        callTool("set_schematic", {{"components", QJsonArray{QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}},
                                                             QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 150}, {"properties", QJsonObject{{"R", "1 kOhm"}}}},
                                                             QJsonObject{{"type", "GND"}, {"x", 100}, {"y", 260}},
                                                             QJsonObject{{"type", ".DC"}, {"name", "DC1"}, {"x", 300}, {"y", 300}}}}});
        const QString file = dir.filePath("workspace/res.sch");
        QVERIFY(!callTool("save_document", {{"as", file}}).value("isError").toBool());
        sent.clear();
        server->poll();   // (a document came: the list changed)
        QVERIFY(std::any_of(sent.cbegin(), sent.cend(), [](const QJsonObject& m) { return m.value("method").toString() == "notifications/resources/list_changed"; }));
        QStringList uris;
        for (const QJsonValue& v : request("resources/list").value("result").toObject().value("resources").toArray()) uris << v.toObject().value("uri").toString();
        const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(file));
        QVERIFY(uris.contains("qucs://state"));
        QVERIFY2(uris.contains("qucs://schematic/" + enc) && uris.contains("qucs://netlist/" + enc) && uris.contains("qucs://netlist-map/" + enc),
                 qPrintable(uris.join(", ")));
        QVERIFY(!uris.contains("qucs://dataset/" + enc));   // (not simulated)
        const auto read = [&](const QString& uri) { return request("resources/read", {{"uri", uri}}); };
        QJsonObject r = read("qucs://schematic/" + enc);
        QCOMPARE(r.value("result").toObject().value("contents").toArray().first().toObject().value("text").toString(), front()->documentText());
        r = read("qucs://netlist/" + enc);
        const QString netlist = r.value("result").toObject().value("contents").toArray().first().toObject().value("text").toString();
        QVERIFY2(netlist.contains("\nR1 ") && netlist.contains("\nV1 "), qPrintable(netlist));
        r = read("qucs://netlist-map/" + enc);
        const QJsonObject map = QJsonDocument::fromJson(r.value("result").toObject().value("contents").toArray().first().toObject().value("text").toString().toUtf8()).object();
        bool r1 = false;
        for (const QJsonValue& l : map.value("lines").toArray()) r1 = r1 || l.toObject().value("part").toString() == "R1";
        QVERIFY(r1);
        QVERIFY(!map.value("nodes").toObject().isEmpty());
        QVERIFY(read("qucs://state").value("result").toObject().value("contents").toArray().first().toObject().value("text").toString().contains("documents"));
        QCOMPARE(read("qucs://schematic/nowhere").value("error").toObject().value("code").toInt(), -32002);
        QCOMPARE(read("http://else").value("error").toObject().value("code").toInt(), -32002);
        // Subscribed: an edit is told.
        request("resources/subscribe", {{"uri", "qucs://schematic/" + enc}});
        sent.clear();
        server->poll();
        QVERIFY(sent.isEmpty());   // (nothing changed yet)
        callTool("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 400}, {"y", 150}});
        server->poll();
        QVERIFY(std::any_of(sent.cbegin(), sent.cend(), [&](const QJsonObject& m) {
            return m.value("method").toString() == "notifications/resources/updated"
                   && m.value("params").toObject().value("uri").toString() == "qucs://schematic/" + enc;
        }));
        // Saved, then written by another program: loaded again, and told.
        callTool("save_document");
        QTest::qWait(2000);   // (a later time on the file; watched again, as a file saved is written anew)
        QString text = front()->documentText();
        text.replace("\"1 kOhm\"", "\"2.2 kOhm\"");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text.toUtf8());
        f.close();
        sent.clear();
        QTRY_VERIFY_WITH_TIMEOUT(front()->documentText().contains("2.2 kOhm"), 10000);
        server->poll();
        QVERIFY(std::any_of(sent.cbegin(), sent.cend(), [](const QJsonObject& m) { return m.value("method").toString() == "notifications/resources/updated"; }));
        request("resources/unsubscribe", {{"uri", "qucs://schematic/" + enc}});
        // A dataset: its resource then.
        write("res.dat.ngspice", "<Qucs Dataset " PACKAGE_VERSION ">\n<indep v1 2>\n0\n1\n</indep>\n");
        uris.clear();
        for (const QJsonValue& v : request("resources/list").value("result").toObject().value("resources").toArray()) uris << v.toObject().value("uri").toString();
        QVERIFY(uris.contains("qucs://dataset/" + enc));
        callTool("close_document", {{"unsaved", "discard"}});
    }

    // Before a file is written over, the user is asked through the client
    // (elicitation/create): yes - written; no - refused, the file left.
    void theUserIsAskedBeforeAFileIsWrittenOver()
    {
        callTool("new_document", {{"kind", "schematic"}});
        callTool("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}});
        const QString target = write("taken.sch", "<Qucs Schematic 1.0>\n");
        // The client's answer, once the question is out.
        const auto answerWith = [this](bool yes) {
            QTimer::singleShot(0, this, [this, yes] {
                for (const QJsonObject& m : std::as_const(sent))
                    if (m.value("method").toString() == "elicitation/create") {
                        QVERIFY(m.value("params").toObject().value("message").toString().contains("taken.sch"));
                        server->handle({{"jsonrpc", "2.0"}, {"id", m.value("id")},
                                        {"result", QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"confirm", yes}}}}}},
                                       [](const QJsonObject&) {});
                        return;
                    }
                QFAIL("no question was asked");
            });
        };
        sent.clear();
        answerWith(false);
        QJsonObject r = callTool("save_document", {{"as", target}});
        QVERIFY2(r.value("isError").toBool(), qPrintable(QJsonDocument(r).toJson()));
        QFile kept(target);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("<Qucs Schematic 1.0>\n"));
        kept.close();
        sent.clear();
        answerWith(true);
        r = callTool("save_document", {{"as", target}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QJsonDocument(r).toJson()));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QVERIFY(kept.readAll().contains("<R R1"));
        // 'replace' says so without asking.
        sent.clear();
        r = callTool("save_document", {{"as", target}, {"replace", true}});
        QVERIFY(!r.value("isError").toBool());
        QVERIFY(std::none_of(sent.cbegin(), sent.cend(), [](const QJsonObject& m) { return m.value("method").toString() == "elicitation/create"; }));
        callTool("close_document", {{"unsaved", "discard"}});
        // What cannot be undone is known to the dock (asked about again).
        QVERIFY(control->irreversible("clean_scratch", {}));
        QVERIFY(control->irreversible("save_document", {{"as", target}}));
        QVERIFY(!control->irreversible("save_document", {{"as", dir.filePath("workspace/none.sch")}}));
        QVERIFY(!control->irreversible("add_component", {{"type", "R"}}));
    }

    // A change previewed: what it would do, and nothing done - the parts,
    // the undo history and the unsaved flag as they were. And diffed.
    void aChangeIsPreviewedAndDiffed()
    {
        callTool("new_document", {{"kind", "schematic"}});
        callTool("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}});
        const QString file = dir.filePath("workspace/prev.sch");
        callTool("save_document", {{"as", file}});
        Schematic* sch = front();
        const QString before = sch->documentText();
        const int undo = sch->undoIndex();
        QJsonObject r = callTool("add_component", {{"type", "C"}, {"name", "C7"}, {"x", 300}, {"y", 100}, {"preview", true}});
        QJsonObject o = structured(r);
        QVERIFY2(o.value("preview").toBool() && !o.value("changed").toBool(), qPrintable(QJsonDocument(o).toJson()));
        QVERIFY(QJsonDocument(o.value("would change").toArray()).toJson().contains("C7"));
        QCOMPARE(o.value("its answer").toObject().value("name").toString(), QStringLiteral("C7"));
        QCOMPARE(sch->documentText(), before);
        QVERIFY(sch->getComponentByName("C7") == nullptr);
        QCOMPARE(sch->undoIndex(), undo);
        QVERIFY(!sch->getDocChanged());
        // A batch too (told when it is over).
        r = callTool("batch", {{"preview", true}, {"calls", QJsonArray{QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "L"}, {"name", "L3"}, {"x", 500}, {"y", 100}}}},
                                                                     QJsonObject{{"tool", "edit_component"}, {"arguments", QJsonObject{{"name", "R1"}, {"properties", QJsonObject{{"R", "47k"}}}}}}}}});
        o = structured(r);
        const QString would = QJsonDocument(o.value("would change").toArray()).toJson();
        QVERIFY2(would.contains("L3") && would.contains("47k"), qPrintable(would));
        QCOMPARE(sch->documentText(), before);
        QVERIFY(structured(callTool("simulate", {{"preview", true}})).value("error").toString().contains("no preview"));
        // Diffed: its unsaved change, steps back, another file.
        callTool("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "10k"}}}});
        o = structured(callTool("diff"));
        QVERIFY2(!o.value("same").toBool() && QJsonDocument(o.value("changes").toArray()).toJson().contains("10k"), qPrintable(QJsonDocument(o).toJson()));
        o = structured(callTool("diff", {{"steps", 1}}));
        QVERIFY(QJsonDocument(o.value("changes").toArray()).toJson().contains("10k"));
        o = structured(callTool("diff", {{"against", file}}));
        QVERIFY(!o.value("same").toBool());
        callTool("save_document");
        QVERIFY(structured(callTool("diff")).value("same").toBool());
        QVERIFY(callTool("diff", {{"steps", 99}}).value("isError").toBool());
        callTool("close_document", {{"unsaved", "discard"}});
    }

    // The JSON form: parts by named properties, checked against the type
    // (a line's values are positional); what get_schematic's json gives is
    // taken back as it is; a value that does not read as a number and a
    // type's trap are told.
    void theJsonFormIsCheckedAndRoundTrips()
    {
        callTool("new_document", {{"kind", "schematic"}});
        QJsonObject r = callTool("set_schematic", {{"components", QJsonArray{
                                                        QJsonObject{{"type", "R"}, {"x", 100}, {"y", 100}, {"properties", QJsonObject{{"R", "1,5k"}}}},
                                                        QJsonObject{{"type", "R"}, {"x", 200}, {"y", 100}, {"rotation", 1}},
                                                        QJsonObject{{"type", "SpiceOptions"}, {"name", "OPT1"}, {"x", 100}, {"y", 300},
                                                                    {"equations", QJsonArray{"noinit", "reltol=1e-4"}}}}},
                                                   {"wires", QJsonArray{QJsonObject{{"from", QJsonArray{130, 100}}, {"to", QJsonArray{200, 100}}, {"label", "mid"}},
                                                                        QJsonObject{{"at", QJsonArray{70, 100}}, {"label", "in"}}}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QJsonDocument(r).toJson()));
        Schematic* sch = front();
        QVERIFY(sch->getComponentByName("R1") != nullptr && sch->getComponentByName("R2") != nullptr);   // (named in turn)
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("1,5k"));
        QVERIFY2(QJsonDocument(structured(r).value("values").toArray()).toJson().contains("1,5k"), qPrintable(QJsonDocument(r).toJson()));
        QVERIFY(sch->getComponentByName("OPT1")->getProperty("noinit") != nullptr);
        QVERIFY(sch->documentText().contains("\"mid\""));
        // Taken back as it gives it.
        const QString text = sch->documentText();
        const QJsonObject form = structured(callTool("get_schematic", {{"format", "json"}}));
        QCOMPARE(form.value("components").toArray().size(), 3);
        r = callTool("set_schematic", {{"components", form.value("components")}, {"wires", form.value("wires")}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QJsonDocument(r).toJson()));
        QCOMPARE(front()->documentText(), text);
        // Refused, and the schematic left: a property the type has not, a
        // type there is not, two of one name, a slanted wire.
        const auto refused = [&](const QJsonObject& args, const char* says) {
            const QJsonObject res = callTool("set_schematic", args);
            QVERIFY2(res.value("isError").toBool() && structured(res).value("error").toString().contains(says),
                     qPrintable(QJsonDocument(res).toJson()));
            QCOMPARE(front()->documentText(), text);
        };
        refused({{"components", QJsonArray{QJsonObject{{"type", "R"}, {"x", 0}, {"y", 0}, {"properties", QJsonObject{{"Rx", "1"}}}}}}}, "components[0]");
        refused({{"components", QJsonArray{QJsonObject{{"type", "Nope"}, {"x", 0}, {"y", 0}}}}}, "Nope");
        refused({{"components", QJsonArray{QJsonObject{{"type", "R"}, {"name", "Ra"}, {"x", 0}, {"y", 0}}, QJsonObject{{"type", "C"}, {"name", "Ra"}, {"x", 50}, {"y", 0}}}}}, "Ra");
        refused({{"wires", QJsonArray{QJsonObject{{"from", QJsonArray{0, 0}}, {"to", QJsonArray{10, 10}}}}}}, "straight");
        refused({{"text", "<Components>\n</Components>"}, {"components", QJsonArray()}}, "not both");
        // A trap told as a part is placed; a value that is not a number.
        r = callTool("add_component", {{"type", "Vpulse"}, {"name", "V9"}, {"x", 600}, {"y", 100}});
        QVERIFY(structured(r).value("watch").toString().contains("Vrect"));
        r = callTool("edit_component", {{"name", "V9"}, {"properties", QJsonObject{{"U2", "1.2.3"}}}});
        QVERIFY(QJsonDocument(structured(r).value("values").toArray()).toJson().contains("1.2.3"));
        r = callTool("edit_component", {{"name", "V9"}, {"properties", QJsonObject{{"U2", "vhigh"}}}});   // (a parameter's name)
        QVERIFY(!structured(r).contains("values"));
        callTool("close_document", {{"unsaved", "discard"}});
    }

    // run_script: loops and conditions between the calls, its log and its
    // value; a failure told with its line, and with 'atomic' put back.
    void aScriptRunsTheTools()
    {
        if (!QucsControl::scriptingBuilt()) QSKIP("no Qt Qml: run_script is not built");
        callTool("new_document", {{"kind", "schematic"}});
        QJsonObject r = callTool("run_script", {{"script",
            "let placed = []; for (let i = 1; i <= 4; i++) { const p = qucs.call('add_component', {type: 'R', name: 'R' + i, x: 100 * i, y: 100}); "
            "if (i % 2 == 0) placed.push(p.name); } qucs.log('even ones: ' + placed.length); placed"}});
        QJsonObject o = structured(r);
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QJsonDocument(r).toJson()));
        QCOMPARE(o.value("result").toArray(), (QJsonArray{"R2", "R4"}));
        QCOMPARE(o.value("calls").toInt(), 4);
        QCOMPARE(o.value("log").toArray().first().toString(), QStringLiteral("even ones: 2"));
        QVERIFY(front()->getComponentByName("R4") != nullptr);
        const QString before = front()->documentText();
        r = callTool("run_script", {{"atomic", true}, {"script", "qucs.call('add_component', {type: 'C', name: 'C1', x: 0, y: 300});\nqucs.call('add_component', {type: 'Nope'});"}});
        QVERIFY(r.value("isError").toBool());
        o = structured(r);
        QVERIFY(o.value("error").toString().contains("Nope"));
        QCOMPARE(o.value("line").toInt(), 2);
        QCOMPARE(parts(front()->documentText()), parts(before));   // put back (the view may have grown)
        // Stopped when it runs too long; not from within itself.
        r = callTool("run_script", {{"script", "while (true) {}"}, {"timeout", 1}});
        QVERIFY(structured(r).value("error").toString().contains("stopped"));
        r = callTool("run_script", {{"script", "qucs.call('run_script', {script: '1'})"}});
        QVERIFY(structured(r).value("error").toString().contains("cannot run from a script"));
        callTool("close_document", {{"unsaved", "discard"}});
    }

    // Over stdio, against the program itself: qucs-s --mcp-server answers
    // initialize, lists the tools, builds a schematic and reads its netlist
    // map, with no window on screen.
    void theProgramServesOverStdio()
    {
        const QString program = QStringLiteral(QUCS_BINARY);
        if (!QFileInfo(program).isExecutable()) QSKIP("qucs-s is not built next to the tests");
        QTemporaryDir home;
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QUCS_CLAUDE", "/nonexistent/claude");
        env.insert("QUCS_NO_SHELL_ENV", "1");
        // Settings of its own - a first start, not the user's preferences
        // (nor their recent files) - and an ngspice to find: a machine
        // without one (a CI runner) has no simulator to write a netlist for.
        env.insert("QUCS_SETTINGS_DIR", home.filePath("settings"));
#ifndef Q_OS_WIN
        QDir().mkpath(home.filePath("bin"));
        QFile ngspice(home.filePath("bin/ngspice"));
        QVERIFY(ngspice.open(QIODevice::WriteOnly));
        ngspice.write("#!/bin/sh\nexit 0\n");
        ngspice.close();
        ngspice.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        env.insert("PATH", home.filePath("bin") + ':' + env.value("PATH"));
#endif
        p.setProcessEnvironment(env);
        p.start(program, {"--mcp-server"});
        QVERIFY(p.waitForStarted(10000));
        const QString schematic = home.filePath("stdio.sch");
        const QList<QJsonObject> messages{
            {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject()}}}},
            {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
            {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
            {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "new_document"}, {"arguments", QJsonObject{{"kind", "schematic"}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "set_schematic"}, {"arguments", QJsonObject{{"components", QJsonArray{
                QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}}, QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 150}},
                QJsonObject{{"type", ".DC"}, {"name", "DC1"}, {"x", 300}, {"y", 300}}}}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "save_document"}, {"arguments", QJsonObject{{"as", schematic}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "get_netlist"}, {"arguments", QJsonObject{{"map", true}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "no/such"}}};
        QHash<int, QJsonObject> answers;
        for (const QJsonObject& m : messages) {
            p.write(QJsonDocument(m).toJson(QJsonDocument::Compact) + '\n');
            if (!m.contains("id")) continue;
            const int id = m.value("id").toInt();
            // One at a time, as a client waits for each answer.
            QElapsedTimer t;
            t.start();
            while (!answers.contains(id) && t.elapsed() < 60000) {
                p.waitForReadyRead(200);
                while (p.canReadLine()) {
                    const QJsonObject a = QJsonDocument::fromJson(p.readLine()).object();
                    if (a.contains("id")) answers.insert(a.value("id").toInt(), a);
                }
            }
            QVERIFY2(answers.contains(id), qPrintable(QStringLiteral("no answer to %1; stderr: %2").arg(id).arg(QString::fromUtf8(p.readAllStandardError()).right(800))));
        }
        // Sent together, without waiting: taken in turn - the list after
        // a save finds the file saved.
        const QString moved = home.filePath("moved.sch");
        p.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 8}, {"method", "tools/call"},
                                          {"params", QJsonObject{{"name", "save_document"}, {"arguments", QJsonObject{{"as", moved}}}}}})
                    .toJson(QJsonDocument::Compact) + '\n'
                + QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 9}, {"method", "resources/list"}}).toJson(QJsonDocument::Compact) + '\n');
        QElapsedTimer t;
        t.start();
        while (!(answers.contains(8) && answers.contains(9)) && t.elapsed() < 60000) {
            p.waitForReadyRead(200);
            while (p.canReadLine()) {
                const QJsonObject a = QJsonDocument::fromJson(p.readLine()).object();
                if (a.contains("id")) answers.insert(a.value("id").toInt(), a);
            }
        }
        QVERIFY(answers.contains(9));
        QVERIFY2(QJsonDocument(answers.value(9)).toJson().contains(QUrl::toPercentEncoding(moved)), qPrintable(QJsonDocument(answers.value(9)).toJson()));
        p.closeWriteChannel();   // (the server ends with its input)
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(answers.value(1).value("result").toObject().value("capabilities").toObject().contains("resources"));
        QVERIFY(answers.value(2).value("result").toObject().value("tools").toArray().size() >= 60);
        QVERIFY(!answers.value(4).value("result").toObject().value("isError").toBool());
        QVERIFY(QFileInfo::exists(schematic));
        const QJsonObject map = answers.value(6).value("result").toObject().value("structuredContent").toObject();
        QVERIFY2(QJsonDocument(map.value("lines").toArray()).toJson().contains("R1"), qPrintable(QJsonDocument(answers.value(6)).toJson()));
        QCOMPARE(answers.value(7).value("error").toObject().value("code").toInt(), -32601);
    }
};

QTEST_MAIN(TestMcpServer)
#include "test_mcp_server.moc"
