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
#include <QPainter>
#include <QPdfWriter>
#include <QDirIterator>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QTableWidget>
#include <QToolButton>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "claudejobs.h"
#include "claudejobswindow.h"
#include "config.h"
#include "simulationconsole.h"
#include "isolated_settings.h"
#include "main.h"
#include "mcpserver.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "extsimkernels/spicecompat.h"

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

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
            // In every turn, a summary - a model's tools, with a model open,
            // whole -; found by the tool search when needed, its whole
            // description - no describe_tool before its first use.
            const bool core = t.value("_meta").toObject().value("anthropic/alwaysLoad").toBool();
            const bool whole = !core || t.value("name").toString().startsWith("fem_");
            QVERIFY2(!t.value("description").toString().isEmpty() && (whole || t.value("description").toString().size() < 320),
                     qPrintable(t.value("name").toString()));
            if (whole)
                QCOMPARE(t.value("description").toString(),
                         structured(callTool("describe_tool", {{"name", t.value("name").toString()}})).value("description").toString());
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
        QVERIFY(byName.contains("arrange") && byName.value("arrange").value("inputSchema").toObject().value("properties").toObject().contains("preview"));
        QVERIFY(request("initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject{{"elicitation", QJsonObject()}}}})
                    .value("result").toObject().value("instructions").toString().contains("come with their full description"));
        // The whole of one.
        const QJsonObject whole = structured(callTool("describe_tool", {{"name", "get_dataset"}}));
        QVERIFY(whole.value("description").toString().size() > byName.value("get_dataset").value("description").toString().size() * 3);
        QVERIFY(whole.value("inputSchema").toObject().value("properties").toObject().contains("variables"));
        QVERIFY(structured(callTool("describe_tool")).value("tools").toArray().size() == tools.size());
        QVERIFY(callTool("describe_tool", {{"name", "nothing"}}).value("isError").toBool());
    }

    // A file changed on disk while its document has unsaved changes is not
    // loaded again - the window shows those - and the next tool result of
    // each conversation says so (an edit of Claude's with Write or Bash was
    // otherwise not shown, without a word). Told once.
    // What is in every turn follows what is open (the dock's 106 sessions,
    // 19 October: a session of a model searched for its tools three times
    // or more, add_wire went in three sessions' turns): with no model, the
    // wiring; a model open, its tools - whole, as a search would load them
    // - and no wiring while no schematic has parts; one with parts, the
    // wiring again. A kind of diagram's own options folded in add_diagram's
    // published schema - their names said, the whole in describe_tool and
    // in what a call is checked against. The instructions: a task's tools
    // loaded in one search.
    void inEveryTurnIsWhatIsOpen()
    {
        const auto loaded = [this] {
            QHash<QString, QJsonObject> out;
            for (const QJsonValue& v : request("tools/list").value("result").toObject().value("tools").toArray())
                if (v.toObject().value("_meta").toObject().value("anthropic/alwaysLoad").toBool())
                    out.insert(v.toObject().value("name").toString(), v.toObject());
            return out;
        };
        // (Nothing the tests before left open.)
        for (const QJsonValue& d : structured(callTool("get_state")).value("documents").toArray()) {
            const QString title = d.toObject().value("title").toString();
            QJsonObject closed = callTool("close_document", {{"path", title}, {"unsaved", "discard"}});
            QVERIFY2(!closed.value("isError").toBool(), qPrintable(title));
        }
        for (QucsDoc* d : app->allDocuments())
            if (auto* sch = dynamic_cast<Schematic*>(d)) QVERIFY(sch->a_DocComps.empty());
        // (get_state says it - from inside any session, resumed too.)
        const auto said = [this] { return structured(callTool("get_state")).value("tools in every turn").toString(); };
        QHash<QString, QJsonObject> every = loaded();
        QVERIFY(every.contains("get_state") && every.contains("close_document") && every.contains("new_document") && every.contains("list_actions"));
        QVERIFY(every.contains("connect") && every.contains("add_wire"));
        QVERIFY(!every.contains("fem_solve") && !every.contains("fem_model"));
        QVERIFY2(said().contains("the wiring") && !said().contains("a model's"), qPrintable(said()));

        // A model open: its tools, whole; no wiring.
        const QJsonObject model = QJsonDocument::fromJson(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
              "geometry": [{"type": "rectangle", "label": "gap", "size": ["10", "1"]}],
              "materials": [{"label": "glass", "epsilonr": "5", "selection": {"all": true}}],
              "physics": [{"type": "electrostatics", "tag": "es", "features": [
                {"type": "terminal", "name": "1", "selection": {"of": ["gap"], "side": "top"}},
                {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}]}],
            "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})").object();
        const QJsonObject made = callTool("fem_model", {{"action", "new"}, {"path", "every_turn"}, {"model", model}});
        QVERIFY2(!made.value("isError").toBool(), qPrintable(QJsonDocument(made).toJson()));
        every = loaded();
        for (const char* t : {"fem_describe", "fem_model", "fem_edit", "fem_solve", "fem_evaluate", "fem_plot"}) {
            QVERIFY2(every.contains(t), t);
            QCOMPARE(every.value(t).value("description").toString(),
                     structured(callTool("describe_tool", {{"name", QString::fromLatin1(t)}})).value("description").toString());
        }
        QVERIFY(!every.contains("connect") && !every.contains("add_wire") && every.contains("get_state"));
        QVERIFY(!every.contains("fem_export"));
        QVERIFY2(said().contains("a model's (fem_*)") && !said().contains("the wiring"), qPrintable(said()));
        // A schematic with a part: the wiring too.
        QVERIFY(!callTool("new_document", {{"kind", "schematic"}}).value("isError").toBool());
        QVERIFY(!callTool("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}}).value("isError").toBool());
        every = loaded();
        QVERIFY(every.contains("connect") && every.contains("fem_solve"));
        QVERIFY2(said().contains("a model's (fem_*), the wiring"), qPrintable(said()));
        QVERIFY(!every.contains("read_pdf") && !every.contains("show_pdf"));
        // A PDF open: its reading and showing (each session with a paper open
        // searched for them first); closed, not.
        {
            QPdfWriter pdf(dir.filePath("every_turn.pdf"));
            QPainter painter(&pdf);
            painter.drawText(100, 100, "a page");
        }
        QVERIFY(!callTool("open_document", {{"path", dir.filePath("every_turn.pdf")}}).value("isError").toBool());
        every = loaded();
        QVERIFY(every.contains("read_pdf") && every.contains("show_pdf") && every.contains("fem_solve"));
        QVERIFY2(said().contains("a PDF's"), qPrintable(said()));
        QVERIFY(!callTool("close_document", {{"path", dir.filePath("every_turn.pdf")}}).value("isError").toBool());
        every = loaded();
        QVERIFY(!every.contains("read_pdf") && !said().contains("a PDF's"));

        // add_diagram's own options of a kind, folded: their names, the
        // whole in describe_tool, a call checked against it.
        QJsonObject published;
        for (const QJsonValue& v : request("tools/list").value("result").toObject().value("tools").toArray())
            if (v.toObject().value("name").toString() == "add_diagram") published = v.toObject();
        const QJsonObject eye = published.value("inputSchema").toObject().value("properties").toObject().value("eye").toObject();
        QVERIFY2(!eye.contains("properties") && eye.value("description").toString().contains("unit_interval")
                     && eye.value("description").toString().contains("mask (height, width)"),
                 qPrintable(QJsonDocument(eye).toJson()));
        QVERIFY(QJsonDocument(published).toJson(QJsonDocument::Compact).size() < 14000);
        // (Each folded kind's first clause cut outside any bracket:
        // smith_circles' "(null: ...)" was cut open - add_diagram's and
        // edit_diagram's.)
        for (const QJsonValue& v : request("tools/list").value("result").toObject().value("tools").toArray()) {
            const QString name = v.toObject().value("name").toString();
            if (name != "add_diagram" && name != "edit_diagram") continue;
            const QJsonObject properties = v.toObject().value("inputSchema").toObject().value("properties").toObject();
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                const QString d = it.value().toObject().value("description").toString();
                if (d.contains("describe_tool")) QVERIFY2(d.count('(') == d.count(')') && d.count('[') == d.count(']'), qPrintable(name + ": " + d));
            }
        }
        const QJsonObject whole = structured(callTool("describe_tool", {{"name", "add_diagram"}}));
        QVERIFY(whole.value("inputSchema").toObject().value("properties").toObject().value("eye").toObject().value("properties").toObject().contains("span"));
        const QJsonObject wrong = callTool("add_diagram", {{"type", "eye"}, {"eye", QJsonObject{{"span", "wide"}}}});
        QVERIFY(wrong.value("isError").toBool());
        QVERIFY2(QJsonDocument(wrong).toJson().contains("eye.span"), qPrintable(QJsonDocument(wrong).toJson()));

        QVERIFY(control->instructions().contains("load a task's together, in one search"));
        for (const QJsonValue& d : structured(callTool("get_state")).value("documents").toArray())
            callTool("close_document", {{"path", d.toObject().value("title").toString()}, {"unsaved", "discard"}});
        // A project of models, none open (its first prompt is often before
        // one is): a model's tools.
        QVERIFY(!loaded().contains("fem_solve"));
        const QJsonObject project = callTool("new_project", {{"name", "models"}});
        QVERIFY2(!project.value("isError").toBool(), qPrintable(QJsonDocument(project).toJson()));
        QVERIFY(!loaded().contains("fem_solve"));
        QFile lens(QucsSettings.QucsWorkDir.filePath("lens.qfem"));
        QVERIFY(lens.open(QIODevice::WriteOnly));
        lens.write("{}");
        lens.close();
        QVERIFY(loaded().contains("fem_solve"));
        QVERIFY(lens.remove());
    }

    void anEditNotLoadedIsTold()
    {
        const QString file = write("notloaded.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
                                                    "<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        callTool("open_document", {{"path", file}});
        callTool("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}});   // unsaved
        QTest::qWait(1100);
        write("notloaded.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n</Components>\n");
        app->reloadChangedFiles({file});
        QVERIFY(front()->getComponentByName("R1") != nullptr);   // not loaded
        QJsonObject r = callTool("get_state");
        QVERIFY2(structured(r).value("since_last_call").toString().contains("notloaded.sch was changed on disk but not loaded again"),
                 qPrintable(QJsonDocument(structured(r)).toJson()));
        r = callTool("get_state");
        QVERIFY(!structured(r).value("since_last_call").toString().contains("notloaded.sch"));
        callTool("close_document", {{"unsaved", "discard"}});
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
    // max_chars, taken by every tool (the server's, not the tool's): a
    // longer answer comes back with its biggest lists and texts halved -
    // "… n more" - and 'trimmed' says what was cut; a text answer is cut at
    // its end; a short one is as it was.
    void everyToolTakesMaxChars()
    {
        QString text = "<Components>\n";
        for (int k = 0; k < 60; ++k)
            text += QStringLiteral("  <R R%1 1 %2 %3 15 -26 0 1 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
                        .arg(k + 1).arg(100 + (k % 10) * 100).arg(100 + (k / 10) * 100);
        text += "</Components>\n";
        QJsonObject r = callTool("new_document", {{"kind", "schematic"}});
        r = callTool("set_schematic", {{"text", text}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QJsonDocument(r).toJson()));
        const auto first = [](const QJsonObject& result) {
            return result.value("content").toArray().first().toObject().value("text").toString();
        };
        const QString whole = first(callTool("get_schematic"));
        QVERIFY(whole.size() > 5000);
        r = callTool("get_schematic", {{"max_chars", 2000}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(first(r)));
        const QString cut = first(r);
        QVERIFY2(cut.size() <= 2000, qPrintable(QString::number(cut.size())));
        const QJsonObject o = QJsonDocument::fromJson(cut.toUtf8()).object();
        QVERIFY2(o.value("trimmed").toString().startsWith(QStringLiteral("to 2000 characters or less of %1, as max_chars asks: ").arg(whole.size()))
                     && o.value("trimmed").toString().contains("components"),
                 qPrintable(cut));
        const QJsonArray parts = o.value("components").toArray();
        QVERIFY2(!parts.isEmpty() && parts.last().toString().startsWith("… ") && parts.last().toString().endsWith(" more"), qPrintable(cut));
        QVERIFY(structured(r).contains("trimmed"));   // (the structured form is of the text as cut)

        // (The hunt after round 9, B1.) However small: JSON still, with its
        // 'trimmed' - cut as text, it no longer parsed.
        for (const int most : {200, 300, 600, 1000}) {
            r = callTool("get_schematic", {{"max_chars", most}});
            const QString small = first(r);
            const QJsonDocument parsed = QJsonDocument::fromJson(small.toUtf8());
            QVERIFY2(!r.value("isError").toBool() && small.size() <= most && parsed.isObject() && parsed.object().contains("trimmed"),
                     qPrintable(QStringLiteral("%1: %2").arg(most).arg(small)));
            // Cut inside first - a list's single item, a record's fields -
            // not its fields left out at once: the components are there.
            if (most >= 300)
                QVERIFY2(parsed.object().value("components").toArray().size() >= 2, qPrintable(QStringLiteral("%1: %2").arg(most).arg(small)));
        }
        // (B2) An object of many keys - a netlist map's nodes - is cut too:
        // "…": "n more".
        const QString mapped = first(callTool("get_netlist", {{"map", true}}));
        QVERIFY(QJsonDocument::fromJson(mapped.toUtf8()).object().value("nodes").toObject().size() > 60);
        r = callTool("get_netlist", {{"map", true}, {"max_chars", 1500}});
        const QJsonObject map = QJsonDocument::fromJson(first(r).toUtf8()).object();
        QVERIFY2(first(r).size() <= 1500 && map.contains("trimmed") && map.value("nodes").toObject().value(QStringLiteral("…")).toString().endsWith(" more"),
                 qPrintable(first(r)));
        // (B3) A batch's answers, cut together; (B4) and a call's own
        // max_chars in it, its answer cut - refused, it failed the batch.
        const auto texts = [](const QJsonObject& result) {
            QStringList all;
            for (const QJsonValue& v : result.value("content").toArray()) all << v.toObject().value("text").toString();
            return all;
        };
        const QJsonArray twice{QJsonObject{{"tool", "get_schematic"}}, QJsonObject{{"tool", "get_schematic"}}};
        r = callTool("batch", {{"calls", twice}, {"max_chars", 2000}});
        QStringList items = texts(r);
        QVERIFY2(!r.value("isError").toBool() && items.join(QString()).size() <= 2000 && items.size() == 5
                     && QJsonDocument::fromJson(items.at(2).toUtf8()).object().contains("trimmed")
                     && QJsonDocument::fromJson(items.at(4).toUtf8()).object().contains("trimmed"),
                 qPrintable(items.join("\n")));
        r = callTool("batch", {{"calls", QJsonArray{QJsonObject{{"tool", "get_schematic"}, {"arguments", QJsonObject{{"max_chars", 400}}}},
                                                    QJsonObject{{"tool", "get_state"}}}}});
        items = texts(r);
        QVERIFY2(!r.value("isError").toBool() && items.size() == 5 && items.at(2).size() <= 400
                     && QJsonDocument::fromJson(items.at(2).toUtf8()).object().contains("trimmed") && !items.at(4).contains("trimmed"),
                 qPrintable(items.join("\n")));
        r = callTool("batch", {{"calls", QJsonArray{QJsonObject{{"tool", "get_schematic"}, {"arguments", QJsonObject{{"max_chars", 50}}}}}}});
        QVERIFY2(r.value("isError").toBool() && texts(r).join(" ").contains("max_chars is a whole number of characters, 200 or more"),
                 qPrintable(texts(r).join("\n")));

        // A text answer, cut at its end.
        r = callTool("get_netlist", {{"max_chars", 300}});
        QVERIFY2(!r.value("isError").toBool() && first(r).size() <= 300 && first(r).endsWith("a larger max_chars gives them)"), qPrintable(first(r)));
        // Its digits in a text, as a client sends what no schema types (the
        // Claude Code panel sent "1000", and every direct call was refused):
        // as the number - in a batch's call too.
        const QString asNumber = first(r);
        for (const QJsonValue& digits : {QJsonValue("300"), QJsonValue(" 300 ")}) {
            r = callTool("get_netlist", {{"max_chars", digits}});
            QVERIFY2(!r.value("isError").toBool() && first(r) == asNumber, qPrintable(first(r)));
        }
        r = callTool("batch", {{"calls", QJsonArray{QJsonObject{{"tool", "get_netlist"}, {"arguments", QJsonObject{{"max_chars", "300"}}}}}}});
        QVERIFY2(!r.value("isError").toBool() && texts(r).join("\n").contains("a larger max_chars gives them)"), qPrintable(texts(r).join("\n")));
        r = callTool("batch", {{"calls", QJsonArray{QJsonObject{{"tool", "get_netlist"}, {"arguments", QJsonObject{{"max_chars", "abc"}}}}}}});
        QVERIFY2(r.value("isError").toBool() && texts(r).join(" ").contains("not the text \"abc\""), qPrintable(texts(r).join("\n")));
        // A short one, as it was.
        r = callTool("get_state", {{"max_chars", 1000000}});
        QVERIFY(!r.value("isError").toBool() && !first(r).contains("trimmed"));
        // Not a number of characters: refused, saying what was given, nothing done.
        for (const auto& [bad, said] : {std::pair(QJsonValue(50), "not the number 50"), std::pair(QJsonValue(2500.5), "not the number 2500.5"),
                                        std::pair(QJsonValue("abc"), "not the text \"abc\""), std::pair(QJsonValue("4e3"), "not the text \"4e3\""),
                                        std::pair(QJsonValue("-500"), "not the text \"-500\""), std::pair(QJsonValue("150"), "not the text \"150\""),
                                        std::pair(QJsonValue(true), "not true")}) {
            r = callTool("delete", {{"names", QJsonArray{"R1"}}, {"max_chars", bad}});
            QVERIFY2(r.value("isError").toBool() && first(r).contains("max_chars is a whole number of characters, 200 or more") && first(r).contains(said),
                     qPrintable(first(r)));
        }
        QVERIFY(front()->getComponentByName("R1") != nullptr);
        callTool("close_document", {{"unsaved", "discard"}});
    }

    // Structured where the answer is JSON - an object as it is, a list as
    // its items - and an error as the error; a text answer not: Claude Code
    // gives the model a result's structuredContent in place of its text, a
    // text's {"summary"} its every quote and line break escaped (a netlist's
    // \n, a dialog's \").
    void resultsAreStructured()
    {
        QJsonObject r = callTool("new_document", {{"kind", "schematic"}});
        QVERIFY(!r.contains("structuredContent"));
        QVERIFY(r.value("content").toArray().first().toObject().value("text").toString().contains("new document"));
        QVERIFY(!qucs_s::mcp::isJsonAnswer(r));
        r = callTool("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}});
        QCOMPARE(structured(r).value("name").toString(), QStringLiteral("R1"));
        r = callTool("add_component", {{"type", "NoSuchType"}, {"x", 0}, {"y", 0}});
        QVERIFY(r.value("isError").toBool());
        QVERIFY(structured(r).value("error").toString().contains("NoSuchType"));
        QCOMPARE(qucs_s::mcp::structuredOf(QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "[1,2]"}}}}})
                     .value("items").toArray().size(), 2);
        QVERIFY(qucs_s::mcp::isJsonAnswer(QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "[1,2]"}}}}}));
        // A schematic's text, its lines as they are, not escaped in a summary.
        r = callTool("get_schematic", {{"format", "text"}});
        QVERIFY2(!r.value("isError").toBool() && !r.contains("structuredContent"), qPrintable(QJsonDocument(r).toJson()));
        QVERIFY(r.value("content").toArray().first().toObject().value("text").toString().contains("<Components>\n"));
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

    // The tools Claude may use without Claude Code's own check in auto mode
    // (autonomousTools()): tools of the server's that change something -
    // none that drives the window, changes the settings, moves, renames or
    // deletes files, writes libraries, runs a command, a script, a compiler
    // or git. A batch is as its calls: not undoable when one is not, asked
    // about each time when one is; a model, a subcircuit or a netlist's
    // schematic written over ('replace') is not undoable.
    void theToolsAllowedInAutoMode()
    {
        QSet<QString> listed;
        for (const QJsonValue& v : request("tools/list").value("result").toObject().value("tools").toArray())
            listed << v.toObject().value("name").toString();
        const QStringList looks = control->readOnlyTools();
        const QStringList autonomous = control->autonomousTools();
        QVERIFY(autonomous.size() > 40);
        for (const QString& t : autonomous) {
            QVERIFY2(listed.contains(t), qPrintable(t));
            QVERIFY2(!looks.contains(t), qPrintable(t));
            QVERIFY2(!t.startsWith(QLatin1String("git_")), qPrintable(t));
        }
        for (const char* t : {"trigger_action", "set_dialog", "set_ui", "context_menu", "send_input", "console", "set_settings", "set_simulator",
                              "run_script", "trash_file", "rename_file", "clean_scratch", "create_library", "import_library", "build_verilog_a", "xml"})
            QVERIFY2(!autonomous.contains(QString::fromLatin1(t)), t);
        for (const char* t : {"batch", "fem_edit", "fem_model", "fem_solve", "add_component", "simulate", "save_document", "close_document"})
            QVERIFY2(autonomous.contains(QString::fromLatin1(t)), t);
        // A batch, as its calls.
        const auto batch = [](const QString& tool, const QJsonObject& arguments) {
            return QJsonObject{{"calls", QJsonArray{QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}}}},
                                                   QJsonObject{{"tool", tool}, {"arguments", arguments}}}}};
        };
        QVERIFY(control->irreversible("batch", batch("trash_file", {{"path", "x.sch"}})));
        QVERIFY(!control->irreversible("batch", batch("connect", {{"from", "R1.1"}, {"to", "ground"}})));
        QVERIFY(control->askedEachTime("batch", batch("trash_file", {{"path", "x.sch"}})).isEmpty());
        QVERIFY(!control->askedEachTime("batch", batch("console", {{"console", "terminal"}, {"input", "ls"}})).isEmpty());
        // 'replace': a file written over.
        for (const char* t : {"fem_model", "create_subcircuit", "import_netlist"}) {
            QVERIFY2(control->irreversible(QString::fromLatin1(t), {{"replace", true}}), t);
            QVERIFY2(!control->irreversible(QString::fromLatin1(t), {}), t);
        }
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
        // A word: a parameter's name when an equation block defines it; else told.
        r = callTool("edit_component", {{"name", "V9"}, {"properties", QJsonObject{{"U2", "vhigh"}}}});
        QVERIFY(QJsonDocument(structured(r).value("values").toArray()).toJson().contains("no equation block here defines vhigh"));
        callTool("add_component", {{"type", "Eqn"}, {"name", "Eqn9"}, {"x", 600}, {"y", 400}, {"equations", QJsonArray{"vhigh=5"}}});
        r = callTool("edit_component", {{"name", "V9"}, {"properties", QJsonObject{{"U2", "vhigh"}}}});
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

    // A call its client cancels (notifications/cancelled: the user stopped
    // Claude; Claude Code 2.1.309 sends it at once) is stopped where it waits
    // - a wait, a script with the call it waits on, a model's solve - and
    // its answer goes nowhere: its carrier is handed an empty object, which
    // it does not send. (Ignored, a solve went on for minutes after.)
    void aCancelledCallStopsAndIsNotAnswered()
    {
        using Answer = std::shared_ptr<std::optional<QJsonObject>>;
        const auto begin = [this](int id, const QString& tool, const QJsonObject& arguments) {
            Answer answer = std::make_shared<std::optional<QJsonObject>>();
            server->handle({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                            {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}},
                           [answer](const QJsonObject& r) { *answer = r; });
            return answer;
        };
        const auto cancel = [this](int id) {
            server->handle({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", id}, {"reason", "test"}}}},
                           [](const QJsonObject&) {});
        };
        // A wait: over at once.
        Answer answer = begin(9001, "wait_for", {{"event", "file_written"}, {"path", "never.txt"}, {"timeout", 60}});
        QTest::qWait(300);
        QVERIFY(!answer->has_value());
        QElapsedTimer clock;
        clock.start();
        cancel(9001);
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 10000);
        QVERIFY2(answer->value().isEmpty(), qPrintable(QJsonDocument(answer->value()).toJson()));
        QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
        // One not under way (answered, or never asked): nothing happens.
        cancel(9001);
        cancel(424242);
        QVERIFY(!callTool("get_state").value("isError").toBool());

        // A model's long transient: stopped at its next step, the model free.
        QFile example(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/heater_transient.qfem"));
        QVERIFY(example.open(QIODevice::ReadOnly));
        QByteArray text = example.readAll();
        QVERIFY(text.contains("range(0, 2[s], 60[s])"));
        text.replace("range(0, 2[s], 60[s])", "range(0, 0.5[s], 100000[s])");
        const QString model = write("long_transient.qfem", text);
        QVERIFY(!callTool("open_document", {{"path", model}}).value("isError").toBool());
        QVERIFY(!callTool("fem_build", {{"path", model}, {"what", "mesh"}}).value("isError").toBool());
        answer = begin(9003, "fem_solve", {{"path", model}, {"timeout", 600}});
        QTest::qWait(700);
        QVERIFY2(!answer->has_value(), "solved before it was cancelled: the test's transient is too short");
        clock.restart();
        cancel(9003);
        // (Stopped at its next step: within a second here, a few under the
        // sanitizers - never the transient's minutes.)
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
        QVERIFY2(answer->value().isEmpty(), qPrintable(QJsonDocument(answer->value()).toJson()));
        qInfo("the solve ended %lld ms after it was cancelled", clock.elapsed());
        QVERIFY2(clock.elapsed() < 20000, "the solve went on after it was cancelled");
        // (Nothing under way: fem_stop has nothing to stop.)
        const auto idle = [this, &model] { return !structured(callTool("fem_stop", {{"path", model}})).value("stopped").toBool(true); };
        QVERIFY(idle());

        // A script waiting on its first call of that solve: both stopped,
        // and its second never made.
        if (QucsControl::scriptingBuilt()) {
            QTimer::singleShot(700, this, [cancel, &clock] {
                clock.restart();
                cancel(9004);
            });
            const QString quoted = QString::fromUtf8(QJsonDocument(QJsonArray{model}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
            // (Each call's failure caught: a solve begun while the one stopped
            // ends fails at once - "the work it waited behind was stopped" -,
            // and the third then solves the whole transient, unless the
            // script is stopped.)
            answer = begin(9004, "run_script",
                           {{"script", QStringLiteral("for (let i = 0; i < 3; i++) { try { qucs.call('fem_solve', {path: %1, timeout: 600}); } catch (e) {} }")
                                           .arg(quoted)},
                            {"timeout", 600}});
            QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
            QVERIFY2(answer->value().isEmpty(), qPrintable(QJsonDocument(answer->value()).toJson()));
            qInfo("the script ended %lld ms after it was cancelled", clock.elapsed());
            // (A script runs within its call: its answer is there before any wait for it.)
            QVERIFY2(clock.elapsed() < 20000, "the script went on after it was cancelled");
            QVERIFY(idle());
        }

        // A batch whose solve began after its first call: that solve stopped
        // (the batch's, though begun later), none after it - and a call
        // waiting its turn behind the batch, cancelled, taken out at once.
        // (Going on past failures: a second solve fails as the first ends, a
        // third would solve the whole transient.)
        answer = begin(9005, "batch", {{"keep_going", true},
                                       {"calls", QJsonArray{QJsonObject{{"tool", "get_state"}},
                                                            QJsonObject{{"tool", "fem_solve"}, {"arguments", QJsonObject{{"path", model}}}},
                                                            QJsonObject{{"tool", "fem_solve"}, {"arguments", QJsonObject{{"path", model}}}},
                                                            QJsonObject{{"tool", "fem_solve"}, {"arguments", QJsonObject{{"path", model}}}}}}});
        QTest::qWait(700);
        QVERIFY(!answer->has_value());
        const Answer queued = begin(9006, "get_state", {});
        QTest::qWait(100);
        QVERIFY(!queued->has_value());
        cancel(9006);
        QTRY_VERIFY_WITH_TIMEOUT(queued->has_value(), 5000);
        QVERIFY(queued->value().isEmpty());
        QVERIFY(!answer->has_value());   // (the batch still under way)
        clock.restart();
        cancel(9005);
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
        QVERIFY2(answer->value().isEmpty(), qPrintable(QJsonDocument(answer->value()).toJson()));
        qInfo("the batch ended %lld ms after it was cancelled", clock.elapsed());
        QVERIFY2(clock.elapsed() < 20000, "the batch went on after it was cancelled");
        QVERIFY(idle());
        callTool("close_document", {{"path", model}, {"unsaved", "discard"}});
    }

    // Over stdio, against the program itself: qucs-s --mcp-server answers
    // initialize, lists the tools, builds a schematic and reads its netlist
    // map, with no window on screen.
    // A request's id comes back as it was - null too, once answered as 0 -
    // and params that are no object (or a tool's name that is no string)
    // are invalid params, not "no tool".
    void badRequestsAreSaidSo()
    {
        const auto handled = [this](const QJsonObject& message) {
            QJsonObject answer;
            server->handle(message, [&answer](const QJsonObject& r) { answer = r; });
            QElapsedTimer t;
            t.start();
            while (answer.isEmpty() && t.elapsed() < 10000) QTest::qWait(5);
            return answer;
        };
        QJsonObject a = handled({{"jsonrpc", "2.0"}, {"id", QJsonValue()}, {"method", "ping"}});
        QVERIFY(a.contains("id") && a.value("id").isNull());
        a = handled({{"jsonrpc", "2.0"}, {"id", "s"}, {"method", "tools/call"}, {"params", QJsonArray{1, 2}}});
        QCOMPARE(a.value("error").toObject().value("code").toInt(), -32602);
        QCOMPARE(a.value("id").toString(), QStringLiteral("s"));
        a = handled({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "resources/read"}, {"params", QJsonArray{"qucs://state"}}});
        QCOMPARE(a.value("error").toObject().value("code").toInt(), -32602);
        a = handled({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", QJsonObject{{"name", 42}}}});
        QCOMPARE(a.value("error").toObject().value("code").toInt(), -32602);
        a = handled({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "get_state"}, {"arguments", "x"}}}});
        QCOMPARE(a.value("error").toObject().value("code").toInt(), -32602);
        a = handled({{"jsonrpc", "2.0"}, {"id", QJsonObject{{"a", 1}}}, {"method", "ping"}});
        QCOMPARE(a.value("error").toObject().value("code").toInt(), -32600);
    }

    // The Qucs-S tools' jobs, with what really does the work: a model's solve
    // says its own percentage, Stop stops it, and the tools tell of it.
    void theLongToolsAreJobs()
    {
        using namespace qucs_s::claude::jobs;
        Registry& registry = Registry::instance();
        registry.reset();
        using Answer = std::shared_ptr<std::optional<QJsonObject>>;
        int next = 9100;
        const auto begin = [this, &next](const QString& tool, const QJsonObject& arguments) {
            Answer answer = std::make_shared<std::optional<QJsonObject>>();
            server->handle({{"jsonrpc", "2.0"}, {"id", ++next}, {"method", "tools/call"},
                            {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}},
                           [answer](const QJsonObject& r) { *answer = r; });
            return answer;
        };
        const auto jobOf = [this](const QString& tool) {
            const QJsonArray listed = structured(callTool("list_background_jobs")).value("jobs").toArray();
            for (const QJsonValue& v : listed)
                if (v.toObject().value("name").toString().startsWith(tool)) return v.toObject();
            return QJsonObject();
        };

        // In the tools' lists, as their kin are.
        QVERIFY(control->readOnlyTools().contains("list_background_jobs"));
        QVERIFY(control->readOnlyTools().contains("get_background_job"));
        QVERIFY(!control->readOnlyTools().contains("show_background_jobs"));
        QVERIFY(control->autonomousTools().contains("show_background_jobs"));

        // Nothing yet; and what is refused is said.
        QJsonObject none = structured(callTool("list_background_jobs"));
        QCOMPARE(none.value("jobs").toArray().size(), 0);
        QCOMPARE(none.value("going").toInt(), 0);
        QVERIFY(callTool("list_background_jobs", {{"status", "soon"}}).value("isError").toBool());
        QVERIFY(callTool("list_background_jobs", {{"max", 0}}).value("isError").toBool());
        QVERIFY(callTool("get_background_job", {{"id", "job-0"}}).value("isError").toBool());
        QVERIFY(callTool("get_background_job").value("isError").toBool());
        // A quick tool's call is no job, nor the tools that tell of them.
        QVERIFY(!callTool("get_state").value("isError").toBool());
        QCOMPARE(registry.count(), 0);

        // A model's long transient.
        QFile example(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/heater_transient.qfem"));
        QVERIFY(example.open(QIODevice::ReadOnly));
        QByteArray text = example.readAll();
        QVERIFY(text.contains("range(0, 2[s], 60[s])"));
        text.replace("range(0, 2[s], 60[s])", "range(0, 0.5[s], 100000[s])");
        const QString model = write("jobs_transient.qfem", text);
        QVERIFY(!callTool("open_document", {{"path", model}}).value("isError").toBool());
        QVERIFY(!callTool("fem_build", {{"path", model}, {"what", "mesh"}}).value("isError").toBool());
        // (The build above: a job, over, finished.)
        QJsonObject built = jobOf("fem_build");
        QCOMPARE(built.value("status").toString(), QStringLiteral("finished"));
        QCOMPARE(built.value("percent").toInt(), 100);
        QCOMPARE(built.value("kind").toString(), QStringLiteral("build"));
        QCOMPARE(registry.clearFinished(), 1);

        Answer answer = begin("fem_solve", {{"path", model}, {"timeout", 600}});
        QJsonObject solving;
        QTRY_VERIFY_WITH_TIMEOUT((solving = jobOf("fem_solve"), !solving.isEmpty()), 10000);
        QCOMPARE(solving.value("kind").toString(), QStringLiteral("solve"));
        QVERIFY(solving.value("source").toString() == QLatin1String("qucs-s"));
        QVERIFY(solving.value("yours").toBool());   // (the server's conversation)
        QVERIFY(!solving.value("started").toString().isEmpty());
        // Its own percentage, once it has said one: the time steps it has done.
        QTRY_VERIFY_WITH_TIMEOUT((solving = jobOf("fem_solve"), solving.value("percent").isDouble()), 30000);
        QCOMPARE(solving.value("status").toString() == QLatin1String("running") || solving.value("status").toString() == QLatin1String("started"), true);
        QVERIFY(solving.value("percent").toInt() >= 0 && solving.value("percent").toInt() < 100);
        QVERIFY(solving.value("can_stop").toBool());
        // Its details, and the window shows it, live.
        const QJsonObject whole = structured(callTool("get_background_job", {{"id", solving.value("id").toString()}}));
        QVERIFY2(whole.value("detail").toString().contains("jobs_transient.qfem"), qPrintable(QJsonDocument(whole).toJson()));
        QCOMPARE(whole.value("tool").toString(), QStringLiteral("fem_solve"));
        const QJsonObject shown = structured(callTool("show_background_jobs"));
        QVERIFY2(shown.value("shown").toBool(), qPrintable(QJsonDocument(shown).toJson()));
        QCOMPARE(shown.value("window").toObject().value("rows").toInt(), registry.count());
        QCOMPARE(shown.value("window").toObject().value("title").toString(), QStringLiteral("Background Jobs"));
        QVERIFY(ClaudeJobsWindow::existing() != nullptr);
        QVERIFY(ClaudeJobsWindow::existing()->isVisible());
        QVERIFY(ClaudeJobsWindow::existing()->rowOf(solving.value("id").toString()) >= 0);
        QCOMPARE(shown.value("going").toInt(), 1);
        QCOMPARE(shown.value("jobs").toArray().first().toObject().value("id"), solving.value("id"));
        // Only going, only over.
        QCOMPARE(structured(callTool("list_background_jobs", {{"status", "over"}})).value("jobs").toArray().size(), 0);
        QCOMPARE(structured(callTool("list_background_jobs", {{"status", "going"}})).value("jobs").toArray().size(), 1);
        QVERIFY(structured(callTool("list_background_jobs", {{"mine", true}})).value("jobs").toArray().size() == 1);
        // Stopped: from the window's button, and it says so when it has.
        const QString id = solving.value("id").toString();
        QVERIFY(!answer->has_value());
        QVERIFY(registry.stop(id));
        QVERIFY(registry.job(id)->stopAsked);
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
        QCOMPARE(registry.job(id)->status, Status::Stopped);
        QVERIFY(registry.job(id)->ended.isValid());
        QVERIFY(!registry.job(id)->canStop);
        // (Its call answered as any call stopped is: Claude is told.)
        QVERIFY(answer->value().contains("result"));
        QVERIFY2(!structured(callTool("fem_stop", {{"path", model}})).value("stopped").toBool(true), "the model is still at work");

        // Past its call's timeout: answered, the job goes on and is still
        // stopped by its button.
        registry.reset();
        answer = begin("fem_solve", {{"path", model}, {"timeout", 1}});
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
        QVERIFY(answer->value().value("result").toObject().value("isError").toBool());
        QVERIFY(QJsonDocument(answer->value()).toJson().contains("Still solving"));
        QCOMPARE(registry.count(), 1);
        const QString past = registry.jobs().first().id;
        QVERIFY(!isOver(registry.job(past)->status));
        QVERIFY(registry.job(past)->background);
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(past)->canStop, 10000);
        QVERIFY(registry.stop(past));
        QTRY_COMPARE_WITH_TIMEOUT(registry.job(past)->status, Status::Stopped, 30000);

        // Of two in a batch: a count of its calls. Then one queued behind it,
        // stopped alone. The batch stopped: its later call never begun.
        registry.reset();
        answer = begin("batch", {{"keep_going", true},
                                 {"calls", QJsonArray{QJsonObject{{"tool", "get_state"}},
                                                      QJsonObject{{"tool", "fem_solve"}, {"arguments", QJsonObject{{"path", model}}}},
                                                      QJsonObject{{"tool", "fem_solve"}, {"arguments", QJsonObject{{"path", model}}}},
                                                      QJsonObject{{"tool", "get_state"}}}}});
        const Answer behind = begin("simulate", {{"path", model}});
        QTRY_VERIFY_WITH_TIMEOUT(registry.count() >= 1, 10000);
        QString queuedId;
        for (const Job& job : registry.jobs())
            if (job.tool == "simulate") queuedId = job.id;
        QVERIFY(!queuedId.isEmpty());
        QCOMPARE(registry.job(queuedId)->status, Status::Queued);
        QVERIFY(!registry.job(queuedId)->began.isValid());
        QVERIFY(registry.job(queuedId)->queued.isValid());
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(queuedId)->canStop, 10000);
        QVERIFY(registry.stop(queuedId));
        QTRY_VERIFY_WITH_TIMEOUT(behind->has_value(), 5000);
        QCOMPARE(registry.job(queuedId)->status, Status::Stopped);
        QVERIFY(QJsonDocument(behind->value()).toJson().contains("before it began"));
        // The batch's job shows past two seconds, its calls counted.
        QString batchId;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (const Job& job : registry.jobs())
                if (job.tool == "batch") batchId = job.id;
            return !batchId.isEmpty();
        }()), 20000);
        QCOMPARE(registry.job(batchId)->kind, QStringLiteral("batch"));
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(batchId)->percent >= 25, 10000);
        QVERIFY(registry.job(batchId)->percent < 100);
        QVERIFY(registry.stop(batchId));
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
        QCOMPARE(registry.job(batchId)->status, Status::Stopped);

        // A tool refused at once: a job all the same, as it is one of those
        // listed as they begin, and it failed. No Stop.
        registry.reset();
        QVERIFY(callTool("tune", {{"max_runs", 1}}).value("isError").toBool());
        QCOMPARE(registry.count(), 1);
        QCOMPARE(registry.jobs().first().status, Status::Failed);
        QCOMPARE(registry.jobs().first().kind, QStringLiteral("tune"));
        QVERIFY(!registry.jobs().first().outcome.isEmpty());
        QVERIFY(!registry.jobs().first().canStop);
        // A wait is no job, past two seconds too.
        registry.reset();
        answer = begin("wait_for", {{"event", "file_written"}, {"path", "never.txt"}, {"timeout", 3}});
        QTest::qWait(2600);
        QCOMPARE(registry.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 20000);
        // Sent to the background: listed, whatever it is.
        answer = begin("wait_for", {{"event", "file_written"}, {"path", "never.txt"}, {"timeout", 30}});
        control->callSentToBackground(7, "wait_for");
        QCOMPARE(registry.count(), 1);
        QVERIFY(registry.jobs().first().background);
        QCOMPARE(registry.jobs().first().kind, QStringLiteral("tool"));
        QTRY_VERIFY_WITH_TIMEOUT(registry.jobs().first().canStop, 10000);
        QVERIFY(registry.stop(registry.jobs().first().id));
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 10000);
        QVERIFY(isOver(registry.jobs().first().status));

        // Two of a name in one conversation: Stop stops the one.
        registry.reset();
        const QString other = write("jobs_transient_b.qfem", text);
        QVERIFY(!callTool("open_document", {{"path", other}}).value("isError").toBool());
        QVERIFY(!callTool("fem_build", {{"path", other}, {"what", "mesh"}}).value("isError").toBool());
        registry.clearFinished();
        const Answer one = begin("fem_solve", {{"path", model}, {"timeout", 600}});
        const Answer two = begin("fem_solve", {{"path", other}, {"timeout", 600}});
        QTRY_COMPARE_WITH_TIMEOUT(registry.count(), 2, 10000);
        const QList<Job> both = registry.jobs();
        const Job newest = both.first(), oldest = both.last();
        QVERIFY(newest.detail.contains("jobs_transient_b"));
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(newest.id)->canStop && registry.job(oldest.id)->canStop, 10000);
        QVERIFY(registry.stop(newest.id));
        QTRY_VERIFY_WITH_TIMEOUT(two->has_value(), 30000);
        QCOMPARE(registry.job(newest.id)->status, Status::Stopped);
        QVERIFY(!one->has_value());
        QVERIFY(!isOver(registry.job(oldest.id)->status));
        QVERIFY(registry.stop(oldest.id));
        QTRY_VERIFY_WITH_TIMEOUT(one->has_value(), 30000);

#ifndef Q_OS_WIN
        // A simulation, with a simulator that says a percentage as ngspice does
        // and then takes its time: in the background (answered at once, the job
        // going on, stopped by its button), and past its call's timeout.
        const QString simulatorWas = QucsSettings.NgspiceExecutable;
        const auto back = qScopeGuard([simulatorWas] { QucsSettings.NgspiceExecutable = simulatorWas; });
        const QString slow = write("slow-ngspice.sh", "#!/bin/sh\nprintf '%%42.00%%\\n'\nexec sleep 120\n");
        QFile::setPermissions(slow, QFile::permissions(slow) | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        QucsSettings.NgspiceExecutable = slow;
        const QString sch = dir.filePath("workspace/jobs-simulated.sch");
        QFile::remove(sch);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), sch));
        QVERIFY(!callTool("open_document", {{"path", sch}}).value("isError").toBool());
        registry.reset();
        const QJsonObject begun = structured(callTool("simulate", {{"path", sch}, {"background", true}}));
        QVERIFY2(begun.value("running").toBool(), qPrintable(QJsonDocument(begun).toJson()));
        QCOMPARE(registry.count(), 1);
        const QString simulated = registry.jobs().first().id;
        QCOMPARE(registry.job(simulated)->kind, QStringLiteral("simulation"));
        QVERIFY(registry.job(simulated)->background);
        QTRY_COMPARE_WITH_TIMEOUT(registry.job(simulated)->percent, 42, 20000);
        QVERIFY(!isOver(registry.job(simulated)->status));
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(simulated)->canStop, 10000);
        QVERIFY(registry.stop(simulated));
        QTRY_COMPARE_WITH_TIMEOUT(registry.job(simulated)->status, Status::Stopped, 30000);
        QCOMPARE(registry.job(simulated)->percent, 42);   // (as far as it got)
        // Whose doing, in words: no tool's name for an outcome.
        QVERIFY2(registry.job(simulated)->outcome.startsWith("Stopped by the Background Jobs window."), qPrintable(registry.job(simulated)->outcome));
        // By stop_simulation: said so, and as a sentence.
        QVERIFY(structured(callTool("simulate", {{"path", sch}, {"background", true}})).value("running").toBool());
        const QString byTool = registry.jobs().first().id;
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(byTool)->percent == 42, 20000);
        QVERIFY(!callTool("stop_simulation").value("isError").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(registry.job(byTool)->status, Status::Stopped, 30000);
        QVERIFY2(registry.job(byTool)->outcome.startsWith("Stopped by stop_simulation."), qPrintable(registry.job(byTool)->outcome));

        // As a build of ngspice with a bar prints it (the fork's, the review of
        // 7 November): several in one read, ending in \r. The dock's own bar too.
        {
            const QString barred = write("barred-ngspice.sh", "#!/bin/sh\n"
                                                              "printf ' Reference value :  1.00000e+00  [======                  ]  26%%\\r"
                                                              " Reference value :  2.00000e+00  [=============           ]  53%%\\r'\n"
                                                              "exec sleep 120\n");
            QFile::setPermissions(barred, QFile::permissions(barred) | QFileDevice::ExeOwner | QFileDevice::ExeUser);
            QucsSettings.NgspiceExecutable = barred;
            registry.reset();
            QVERIFY(structured(callTool("simulate", {{"path", sch}, {"background", true}})).value("running").toBool());
            const QString id = registry.jobs().first().id;
            QTRY_COMPARE_WITH_TIMEOUT(registry.job(id)->percent, 53, 20000);
            QCOMPARE(app->simulationConsole()->percent(), 53);
            QCOMPARE(toJson(*registry.job(id), 0, false).value("percent").toInt(), 53);
            QTRY_VERIFY_WITH_TIMEOUT(registry.job(id)->canStop, 10000);
            QVERIFY(registry.stop(id));
            QTRY_COMPARE_WITH_TIMEOUT(registry.job(id)->status, Status::Stopped, 30000);
            QCOMPARE(registry.job(id)->percent, 53);   // (a run stopped part way keeps how far it got)
            QucsSettings.NgspiceExecutable = slow;
        }

        registry.reset();
        answer = begin("simulate", {{"path", sch}, {"timeout", 5}});
        QCOMPARE(registry.count(), 1);
        const QString pastTimeout = registry.jobs().first().id;
        QVERIFY(!registry.job(pastTimeout)->background);
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(pastTimeout)->canStop, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
        QVERIFY(!isOver(registry.job(pastTimeout)->status));   // (it goes on)
        QVERIFY(registry.job(pastTimeout)->background);
        QVERIFY(registry.job(pastTimeout)->canStop);
        QVERIFY(registry.stop(pastTimeout));
        QTRY_COMPARE_WITH_TIMEOUT(registry.job(pastTimeout)->status, Status::Stopped, 30000);

        // In the foreground, stopped: its call answered, and Stopped.
        registry.reset();
        answer = begin("simulate", {{"path", sch}, {"timeout", 300}});
        const QString waited = registry.jobs().first().id;
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(waited)->canStop && registry.job(waited)->percent == 42, 20000);
        QVERIFY(!answer->has_value());
        QVERIFY(registry.stop(waited));
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
        QCOMPARE(registry.job(waited)->status, Status::Stopped);
        callTool("close_document", {{"path", sch}, {"unsaved", "discard"}});
#endif

        // Each document as it was found: nothing is left to run.
        delete ClaudeJobsWindow::existing();
        registry.reset();
        callTool("close_document", {{"path", model}, {"unsaved", "discard"}});
        callTool("close_document", {{"path", other}, {"unsaved", "discard"}});
    }

    // The window as the user has it: from the dock's ⋯ menu in the main window,
    // a real solve in it, its own Stop and Clear Finished pressed.
    void theWindowInTheApplication()
    {
        using namespace qucs_s::claude::jobs;
        Registry& registry = Registry::instance();
        registry.reset();
        delete ClaudeJobsWindow::existing();
        auto* tabs = app->findChild<ClaudeCodeTabs*>();
        QVERIFY(tabs != nullptr);
        ClaudeCodePanel* panel = tabs->current();
        QVERIFY(panel != nullptr);
        QAction* item = panel->findChild<QAction*>("claudeShowBackgroundJobs");
        QVERIFY(item != nullptr);
        auto* menu = qobject_cast<QMenu*>(item->parent());
        QVERIFY(menu != nullptr);
        QVERIFY(menu->actions().contains(item));
        item->trigger();
        ClaudeJobsWindow* w = ClaudeJobsWindow::existing();
        QVERIFY(w != nullptr);
        QCOMPARE(w->parentWidget(), static_cast<QWidget*>(app));   // (the main window's, not a dock's)
        QVERIFY(w->isVisible());
        QVERIFY(!w->isModal());
        // Asked again, with it behind: comes forward, the same one.
        item->trigger();
        QCOMPARE(ClaudeJobsWindow::existing(), w);

        // A real solve.
        QFile example(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/heater_transient.qfem"));
        QVERIFY(example.open(QIODevice::ReadOnly));
        QByteArray text = example.readAll();
        text.replace("range(0, 2[s], 60[s])", "range(0, 0.5[s], 100000[s])");
        const QString model = write("window_transient.qfem", text);
        QVERIFY(!callTool("open_document", {{"path", model}}).value("isError").toBool());
        QVERIFY(!callTool("fem_build", {{"path", model}, {"what", "mesh"}}).value("isError").toBool());
        // (Finished: the row a Clear takes.)
        const QString built = registry.jobs().first().id;
        QCOMPARE(registry.job(built)->status, Status::Finished);
        QVERIFY(w->clearButton()->isEnabled());

        auto answer = std::make_shared<std::optional<QJsonObject>>();
        server->handle({{"jsonrpc", "2.0"}, {"id", 9200}, {"method", "tools/call"},
                        {"params", QJsonObject{{"name", "fem_solve"}, {"arguments", QJsonObject{{"path", model}, {"timeout", 600}}}}}},
                       [answer](const QJsonObject& r) { *answer = r; });
        QTRY_COMPARE_WITH_TIMEOUT(registry.count(), 2, 10000);
        const QString solving = registry.jobs().first().id;
        QVERIFY(solving != built);
        // Its row, live: its bar a real percentage, its button on.
        const auto cell = [&](int column) { return w->table()->cellWidget(w->rowOf(solving), column); };
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(solving)->percent >= 0 && registry.job(solving)->canStop, 30000);
        auto* bar = cell(ClaudeJobsWindow::Progress)->findChild<QProgressBar*>("claudeJobProgress");
        QCOMPARE(bar->maximum(), 100);
        QTRY_COMPARE_WITH_TIMEOUT(bar->value(), registry.job(solving)->percent, 10000);
        auto* stop = cell(ClaudeJobsWindow::Stop)->findChild<QToolButton*>("claudeJobStop");
        QVERIFY(stop->isEnabled());

        // Clear Finished takes what is over, not the solve.
        QTest::mouseClick(w->clearButton(), Qt::LeftButton);
        QVERIFY(!registry.job(built).has_value());
        QCOMPARE(w->rowOf(built), -1);
        QVERIFY(w->rowOf(solving) >= 0);
        QVERIFY(!answer->has_value());

        // Its Stop: stopping, then stopped once it has, and its call answered.
        QStringList said;
        const QMetaObject::Connection watching = connect(&registry, &Registry::changed, this, [&](const QString& id) {
            if (id != solving) return;
            const QString now = statusKey(registry.job(id)->status);
            if (said.isEmpty() || said.last() != now) said << now;
        });
        QTest::mouseClick(stop, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 30000);
        disconnect(watching);
        QVERIFY2(said.size() >= 2 && said.first() == "stopping" && said.last() == "stopped", qPrintable(said.join(' ')));
        QCOMPARE(w->table()->item(w->rowOf(solving), ClaudeJobsWindow::Status)->text(), QStringLiteral("stopped"));
        QVERIFY(!cell(ClaudeJobsWindow::Stop)->findChild<QToolButton*>("claudeJobStop")->isVisibleTo(w));

        // Its details, whole.
        w->table()->selectRow(w->rowOf(solving));
        QVERIFY(w->details()->toPlainText().contains("window_transient.qfem"));
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) qInfo("on the %s platform", qPrintable(QGuiApplication::platformName()));

        delete w;
        registry.reset();
        callTool("close_document", {{"path", model}, {"unsaved", "discard"}});
    }

    // Opt in (QUCS_JOBS_NGSPICE=<a real ngspice>): a real transient, its
    // percentage in the job and the dock's bar as it goes.
    void aRealNgspicesPercentage()
    {
        const QString real = qEnvironmentVariable("QUCS_JOBS_NGSPICE");
        if (real.isEmpty()) QSKIP("QUCS_JOBS_NGSPICE names a real ngspice");
        using namespace qucs_s::claude::jobs;
        Registry& registry = Registry::instance();
        registry.reset();
        const QString simulatorWas = QucsSettings.NgspiceExecutable;
        const auto back = qScopeGuard([simulatorWas] { QucsSettings.NgspiceExecutable = simulatorWas; });
        QucsSettings.NgspiceExecutable = real;
        const QString sch = dir.filePath("workspace/jobs-real-rc.sch");
        QVERIFY(!callTool("new_document", {{"kind", "schematic"}}).value("isError").toBool());
        const QJsonObject set = callTool("set_schematic", {{"components", QJsonArray{
            QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}},
            QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}},
            QJsonObject{{"type", "C"}, {"name", "C1"}, {"x", 300}, {"y", 200}},
            QJsonObject{{"type", "GND"}, {"name", "G1"}, {"x", 100}, {"y", 300}},
            QJsonObject{{"type", ".TR"}, {"name", "TR1"}, {"x", 400}, {"y", 300}, {"properties", QJsonObject{{"Stop", "100 s"}, {"Points", "50000000"}}}}}}});
        QVERIFY2(!set.value("isError").toBool(), qPrintable(QJsonDocument(set).toJson()));
        QVERIFY(!callTool("save_document", {{"as", sch}}).value("isError").toBool());
        QVERIFY(!callTool("connect", {{"from", "V1.1"}, {"to", "R1.1"}}).value("isError").toBool());
        callTool("connect", {{"from", "R1.2"}, {"to", "C1.1"}});
        callTool("connect", {{"from", "C1.2"}, {"to", "G1.1"}});
        callTool("connect", {{"from", "V1.2"}, {"to", "G1.1"}});
        const QJsonObject begun = structured(callTool("simulate", {{"path", sch}, {"background", true}}));
        QVERIFY2(begun.value("running").toBool(), qPrintable(QJsonDocument(begun).toJson()));
        const QString id = registry.jobs().first().id;
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(id)->percent > 0 || isOver(registry.job(id)->status), 60000);
        const Job job = *registry.job(id);
        qInfo("the job: %s; the console's bar: %d; the log's last: %s", qPrintable(percentText(job)), app->simulationConsole()->percent(),
              qPrintable(app->simulationConsole()->console()->toPlainText().right(90).simplified()));
        QVERIFY2(!isOver(job.status), qPrintable(job.outcome));
        QVERIFY(job.percent > 0 && job.percent < 100);
        QCOMPARE(app->simulationConsole()->percent() >= job.percent, true);
        QTRY_VERIFY_WITH_TIMEOUT(registry.job(id)->canStop, 10000);
        const int before = registry.job(id)->percent;
        QVERIFY(registry.stop(id));
        QTRY_COMPARE_WITH_TIMEOUT(registry.job(id)->status, Status::Stopped, 30000);
        QVERIFY(registry.job(id)->percent >= before);
        registry.reset();
        callTool("close_document", {{"path", sch}, {"unsaved", "discard"}});
    }

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
        // (And a home of its own: with no workspace in its settings, it
        // makes ~/QucsWorkspace - here, not the user's.)
        env.insert("HOME", home.path());
        // (And the jobs of the window, beside its conversations: not its own.)
        env.insert("QUCS_CLAUDE_HISTORY", home.filePath("history"));
        QDir().mkpath(home.filePath("history"));
        const QByteArray windowsJobs = R"({"jobs":[{"name":"The window's own","status":"finished"}]})";
        {
            QFile jobs(home.filePath("history/background-jobs.json"));
            QVERIFY(jobs.open(QIODevice::WriteOnly));
            jobs.write(windowsJobs);
        }
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
            {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "no/such"}},
            // No window can show here: said, and the list is given.
            {{"jsonrpc", "2.0"}, {"id", 70}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "show_background_jobs"}, {"arguments", QJsonObject{}}}}},
            {{"jsonrpc", "2.0"}, {"id", 71}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "list_background_jobs"}, {"arguments", QJsonObject{}}}}}};
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
        {
            const QJsonObject shown = answers.value(70).value("result").toObject().value("structuredContent").toObject();
            QVERIFY2(shown.contains("shown") && !shown.value("shown").toBool(), qPrintable(QJsonDocument(answers.value(70)).toJson()));
            QVERIFY(shown.value("jobs").isArray());
            QVERIFY(shown.value("note").toString().contains("--mcp-server"));
            const QJsonObject listed = answers.value(71).value("result").toObject().value("structuredContent").toObject();
            QVERIFY2(listed.value("jobs").isArray(), qPrintable(QJsonDocument(answers.value(71)).toJson()));
            QVERIFY(listed.value("note").toString().contains("--mcp-server"));
            // Its own, this run's: not the window's file, which it leaves alone.
            QVERIFY2(!QJsonDocument(answers.value(71)).toJson().contains("The window's own"), qPrintable(QJsonDocument(answers.value(71)).toJson()));
            QFile jobs(home.filePath("history/background-jobs.json"));
            QVERIFY(jobs.open(QIODevice::ReadOnly));
            QCOMPARE(jobs.readAll(), windowsJobs);
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
        // A batch (a list of requests): refused as such, not "Parse error:
        // no error occurred".
        p.write("[{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"ping\"}]\n");
        QJsonObject batch;
        t.restart();
        while (batch.isEmpty() && t.elapsed() < 30000) {
            p.waitForReadyRead(200);
            while (p.canReadLine()) {
                const QJsonObject a = QJsonDocument::fromJson(p.readLine()).object();
                if (a.contains("error")) batch = a;
            }
        }
        QCOMPARE(batch.value("error").toObject().value("code").toInt(), -32600);
        QVERIFY2(!batch.value("error").toObject().value("message").toString().contains("no error"), qPrintable(QJsonDocument(batch).toJson()));
        // A call cancelled (notifications/cancelled): not answered, and the
        // server still ends with its input (its count of open requests
        // closed all the same) - a ping after it answered.
        p.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 11}, {"method", "tools/call"},
                                          {"params", QJsonObject{{"name", "wait_for"},
                                                                 {"arguments", QJsonObject{{"event", "file_written"}, {"path", home.filePath("never")},
                                                                                           {"timeout", 60}}}}}})
                    .toJson(QJsonDocument::Compact) + '\n'
                + QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", 11}}}})
                      .toJson(QJsonDocument::Compact) + '\n'
                + QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 12}, {"method", "ping"}}).toJson(QJsonDocument::Compact) + '\n');
        t.restart();
        QByteArrayList stray;   // lines that are no JSON-RPC message (an empty answer written)
        while (!answers.contains(12) && t.elapsed() < 30000) {
            p.waitForReadyRead(200);
            while (p.canReadLine()) {
                const QByteArray line = p.readLine();
                const QJsonObject a = QJsonDocument::fromJson(line).object();
                if (!a.contains("jsonrpc")) stray << line;
                if (a.contains("id")) answers.insert(a.value("id").toInt(), a);
            }
        }
        QVERIFY(answers.contains(12));
        QVERIFY2(stray.isEmpty(), stray.join().constData());
        t.restart();
        p.closeWriteChannel();   // (the server ends with its input)
        QVERIFY(p.waitForFinished(30000));
        QVERIFY2(t.elapsed() < 20000, qPrintable(QString::number(t.elapsed())));   // (not the wait's 60 s)
        while (p.canReadLine()) {
            const QByteArray line = p.readLine();
            const QJsonObject a = QJsonDocument::fromJson(line).object();
            if (!a.contains("jsonrpc")) stray << line;
            if (a.contains("id")) answers.insert(a.value("id").toInt(), a);
        }
        QVERIFY2(!answers.contains(11), qPrintable(QJsonDocument(answers.value(11)).toJson()));
        QVERIFY2(stray.isEmpty(), stray.join().constData());   // (the wait's end, written as an empty answer)
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(answers.value(1).value("result").toObject().value("capabilities").toObject().contains("resources"));
        QVERIFY(answers.value(2).value("result").toObject().value("tools").toArray().size() >= 60);
        QVERIFY(!answers.value(4).value("result").toObject().value("isError").toBool());
        QVERIFY(QFileInfo::exists(schematic));
        const QJsonObject map = answers.value(6).value("result").toObject().value("structuredContent").toObject();
        QVERIFY2(QJsonDocument(map.value("lines").toArray()).toJson().contains("R1"), qPrintable(QJsonDocument(answers.value(6)).toJson()));
        QCOMPARE(answers.value(7).value("error").toObject().value("code").toInt(), -32601);
    }

    // Its own name qucs-s - its caches', autosaves', crash reports' and
    // conversations' folders - whatever its bundle is called on macOS:
    // Physics Room there, the application menu's title and its About, Hide
    // and Quit. (Qt takes the bundle's name for the application's unless
    // told: the folders would move, and what is in them be found no more.)
    void itsNameIsItsOwn()
    {
        const QString program = QStringLiteral(QUCS_BINARY);
        if (!QFileInfo(program).isExecutable()) QSKIP("qucs-s is not built next to the tests");
        QProcess p;
        p.start(program, {QStringLiteral("--version")});
        QVERIFY(p.waitForFinished(60000));
        const QString version = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        QVERIFY2(version.startsWith(QLatin1String("qucs-s ")), qPrintable(version));
#ifdef Q_OS_MACOS
        QFile plist(QFileInfo(program).absoluteDir().filePath(QStringLiteral("../Info.plist")));
        QVERIFY(plist.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(plist.readAll()).simplified();
        QVERIFY(text.contains(QLatin1String("<key>CFBundleName</key> <string>Physics Room</string>")));
        QVERIFY(text.contains(QLatin1String("<key>CFBundleExecutable</key> <string>qucs-s</string>")));
#endif
    }

    // A pipe named by -i, in every batch mode, is refused like any file that
    // cannot be read. Opening it to see that it could be read waited for a
    // writer, for ever.
    void aPipeGivenAsTheInputIsRefused()
    {
#ifdef Q_OS_WIN
        QSKIP("no pipes in folders here");
#else
        const QString program = QStringLiteral(QUCS_BINARY);
        if (!QFileInfo(program).isExecutable()) QSKIP("qucs-s is not built next to the tests");
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString pipe = home.filePath("pipe.sch");
        QCOMPARE(mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        env.insert("QUCS_CLAUDE", "/nonexistent/claude");
        env.insert("QUCS_NO_SHELL_ENV", "1");
        env.insert("QUCS_SETTINGS_DIR", home.filePath("settings"));
        env.insert("HOME", home.path());
        const QList<QStringList> modes{{"-n"}, {"-n", "--ngspice"}, {"-n", "--xyce"}, {"-n", "--cdl"},
                                       {"-n", "--ngspice", "--run"}, {"-n", "--xyce", "--run"}, {"-p"}};
        for (const QStringList& mode : modes) {
            QProcess p;
            p.setProcessEnvironment(env);
            p.start(program, mode + QStringList{"-i", pipe, "-o", home.filePath("out.png")});
            const bool ended = p.waitForFinished(60000);
            if (!ended) {
                p.kill();
                p.waitForFinished(5000);
            }
            const QString name = mode.join(' ');
            QVERIFY2(ended, qPrintable(name + ": still waiting on the pipe"));
            const QString said = QString::fromUtf8(p.readAllStandardError());
            QVERIFY2(p.exitCode() != 0 && said.contains(QLatin1String("Could not load schematic")),
                     qPrintable(name + ": exit " + QString::number(p.exitCode()) + ": " + said.right(300)));
        }
#endif
    }

    // --workspace DIR: the run works there - not in the settings' workspace
    // (with none, ~/QucsWorkspace: a fuzzer's projects named "'" and {} went
    // there) - and the settings keep theirs.
    void theWorkspaceIsTheOneGiven()
    {
        const QString program = QStringLiteral(QUCS_BINARY);
        if (!QFileInfo(program).isExecutable()) QSKIP("qucs-s is not built next to the tests");
        QTemporaryDir home;
        const QString workspace = home.filePath("its workspace");
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QUCS_CLAUDE", "/nonexistent/claude");
        env.insert("QUCS_NO_SHELL_ENV", "1");
        env.insert("QUCS_SETTINGS_DIR", home.filePath("settings"));
        // (A home of its own: were --workspace not taken, the fallback -
        // ~/QucsWorkspace - is made here, and the test fails, not the
        // user's workspace filled.)
        env.insert("HOME", home.path());
        // Settings that name a scratch folder of their own, as the cache's
        // default does: the run's scratch is in its workspace all the same
        // (its netlists and logs went to the one folder every run shared).
        const QString sharedScratch = home.filePath("shared scratch");
        QDir().mkpath(home.filePath("settings/qucs"));
        {
            QFile ini(home.filePath("settings/qucs/qucs_s.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write(QStringLiteral("[General]\nS4Q_workdir=%1\n").arg(sharedScratch).toUtf8());
        }
#ifndef Q_OS_WIN
        // (An ngspice that does nothing: the netlist is written before it runs.)
        QDir().mkpath(home.filePath("bin"));
        QFile ngspice(home.filePath("bin/ngspice"));
        QVERIFY(ngspice.open(QIODevice::WriteOnly));
        ngspice.write("#!/bin/sh\nexit 0\n");
        ngspice.close();
        ngspice.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        env.insert("PATH", home.filePath("bin") + ':' + env.value("PATH"));
#endif
        // QUCS_LIBRARY_DIR: the libraries of this folder, not the installed
        // ones (the run over every library part tests the source tree's).
        // A library there alone is found.
        QDir().mkpath(home.filePath("libraries"));
        {
            QFile lib(home.filePath("libraries/OnlyHere.lib"));
            QVERIFY(lib.open(QIODevice::WriteOnly));
            lib.write("<Qucs Library 0.0.10 \"OnlyHere\">\n\n<Component Lonely>\n  <Description>\n    a resistor found only here\n"
                      "  </Description>\n  <Model>\n    <R R1 1 0 0 15 -26 0 1 \"4.7k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                      "  </Model>\n</Component>\n");
        }
        env.insert("QUCS_LIBRARY_DIR", home.filePath("libraries"));
        // (A circuit to run, in the workspace.)
        QDir().mkpath(workspace);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/templates_ngspice/S-parameter_active_analysis.sch"), workspace + "/scratchy.sch"));
        p.setProcessEnvironment(env);
        p.start(program, {"--mcp-server", "--workspace", workspace});
        QVERIFY(p.waitForStarted(10000));
        const QList<QJsonObject> messages{
            {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject()}}}},
            {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
            {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "get_state"}, {"arguments", QJsonObject()}}}},
            {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "new_project"}, {"arguments", QJsonObject{{"name", "{}"}, {"open", false}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "new_project"}, {"arguments", QJsonObject{{"name", "amp"}, {"open", false}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "open_document"}, {"arguments", QJsonObject{{"path", "scratchy.sch"}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "simulate"}, {"arguments", QJsonObject{{"operating_point", true}}}}}},
            {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "find_library_component"}, {"arguments", QJsonObject{{"search", "found only here"}}}}}}};
        QHash<int, QJsonObject> answers;
        for (const QJsonObject& m : messages) {
            p.write(QJsonDocument(m).toJson(QJsonDocument::Compact) + '\n');
            if (!m.contains("id")) continue;
            const int id = m.value("id").toInt();
            QElapsedTimer t;
            t.start();
            while (!answers.contains(id) && t.elapsed() < 60000) {
                p.waitForReadyRead(200);
                while (p.canReadLine()) {
                    const QJsonObject a = QJsonDocument::fromJson(p.readLine()).object();
                    if (a.contains("id")) answers.insert(a.value("id").toInt(), a);
                }
            }
            QVERIFY2(answers.contains(id), qPrintable(QStringLiteral("no answer to %1").arg(id)));
        }
        p.closeWriteChannel();
        QVERIFY(p.waitForFinished(30000));
        const QJsonObject state = answers.value(2).value("result").toObject().value("structuredContent").toObject();
        QCOMPARE(QFileInfo(state.value("workspace").toString()).canonicalFilePath(), QFileInfo(workspace).canonicalFilePath());
        QVERIFY(answers.value(3).value("result").toObject().value("isError").toBool());
        QVERIFY(!answers.value(4).value("result").toObject().value("isError").toBool());
        QVERIFY(QFileInfo(workspace + "/amp_prj").isDir());
        // Its netlist in its workspace's scratch folder, none in the one the
        // settings name.
#ifndef Q_OS_WIN
        QVERIFY2(QDirIterator(workspace + "/spice4qucs", {"*.cir"}, QDir::Files, QDirIterator::Subdirectories).hasNext(),
                 qPrintable(QJsonDocument(answers.value(8)).toJson()));
#endif
        QVERIFY2(!QDirIterator(sharedScratch, QDir::Files, QDirIterator::Subdirectories).hasNext(), qPrintable(sharedScratch));
        // The library of QUCS_LIBRARY_DIR.
        const QJsonArray found = answers.value(9).value("result").toObject().value("structuredContent").toObject().value("found").toArray();
        QVERIFY2(found.size() == 1 && found.first().toObject().value("library").toString() == "OnlyHere",
                 qPrintable(QJsonDocument(answers.value(9)).toJson()));
        // The settings' workspace is not this one.
        QDirIterator files(home.filePath("settings"), {"*.ini", "*.conf"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile f(files.next());
            QVERIFY(f.open(QIODevice::ReadOnly));
            QVERIFY2(!f.readAll().contains("its workspace"), qPrintable(f.fileName()));
        }
    }
};

QTEST_MAIN(TestMcpServer)
#include "test_mcp_server.moc"
