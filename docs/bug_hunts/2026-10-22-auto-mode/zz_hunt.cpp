// Throwaway probe of the hunt of 22 October (afternoon): can a tool allowed in
// auto mode (QucsControl::autonomousTools()) lose data without irreversible()
// saying so? Not registered for good; removed after the hunt.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "mcpserver.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "qucsdoc.h"
#include "schematic.h"
#include "claudecode.h"
#include "claudecodepanel.h"
#include <QLabel>
#include <QToolButton>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include "extsimkernels/spicecompat.h"

using qucs_s::mcp::Server;

class ZzHunt : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;
    Server* server = nullptr;
    int nextId = 100;

    QJsonObject callTool(const QString& tool, const QJsonObject& arguments = {})
    {
        QJsonObject answer;
        bool answered = false;
        server->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", ++nextId}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}},
                       [&](const QJsonObject& r) {
                           answer = r.value("result").toObject();
                           answered = true;
                       });
        QElapsedTimer t;
        t.start();
        while (!answered && t.elapsed() < 400000) QTest::qWait(5);
        return answer;
    }
    static QString text(const QJsonObject& r)
    {
        return r.value("content").toArray().at(0).toObject().value("text").toString().left(260).replace('\n', ' ');
    }
    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    static QByteArray read(const QString& file)
    {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("(none)");
    }
    static void write(const QString& file, const QByteArray& bytes)
    {
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
    // One probe's line: the call, whether irreversible() says it, its answer, what became of the file.
    void probe(const char* label, const QString& tool, const QJsonObject& args, const QString& file = QString())
    {
        const QByteArray before = file.isEmpty() ? QByteArray() : read(file);
        const bool lasting = control->irreversible(tool, args);
        const QJsonObject r = callTool(tool, args);
        const QByteArray after = file.isEmpty() ? QByteArray() : read(file);
        qInfo("PROBE %s | irreversible %d | error %d | %s%s", label, int(lasting), int(r.value("isError").toBool()), qPrintable(text(r)),
              file.isEmpty() ? "" : (before == after ? " | FILE KEPT" : qPrintable(QStringLiteral(" | FILE CHANGED (%1 -> %2 bytes)").arg(before.size()).arg(after.size()))));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("ngspice", {QDir::homePath() + "/bin"});
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
        server = new Server(control, this);
        server->setSend([](const QJsonObject&) {});
        server->setCaller(7);
        server->setCall([this](const QString& tool, const QJsonObject& args, std::function<void(const QJsonObject&)> done) {
            control->callToolFor(7, tool, args, done);
        });
        // (A client that cannot ask: a tool that would ask gets "cancel".)
        QJsonObject init;
        server->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-06-18"}}}},
                       [&init](const QJsonObject& r) { init = r; });
    }

    void probes()
    {
        const QString empty = QStringLiteral("<Qucs Schematic 27.0.3>\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        // A. synthesize_*: save_as a file that is there.
        write(path("keep.sch"), "KEEP THIS\n");
        probe("A synthesize_attenuator save_as existing", "synthesize_attenuator",
              {{"attenuation", 10}, {"topology", "pi"}, {"z_in", 50}, {"z_out", 50}, {"save_as", path("keep.sch")}}, path("keep.sch"));
        qInfo("PROBE A whole: %s", qPrintable(QString::fromUtf8(QJsonDocument(callTool("get_state")).toJson(QJsonDocument::Compact)).left(300)));
        qInfo("PROBE A files: %s", qPrintable(QDir(dir.filePath("workspace")).entryList(QDir::Files).join(", ")));
        write(path("keep2.sch"), "KEEP THIS\n");
        probe("A2 synthesize_filter save_as existing", "synthesize_filter",
              {{"fc", "1GHz"}, {"order", 3}, {"save_as", path("keep2.sch")}}, path("keep2.sch"));
        // B. import_data over a dataset there: a simulation's, and another's.
        write(path("amp.sch"), empty.toUtf8());
        write(path("amp.dat"), "<Qucs Dataset 27.0.3>\n<indep x 2>\n  +1\n  +2\n</indep>\n<dep v x>\n  +5\n  +6\n</dep>\n");
        write(path("meas.csv"), "f,g\n1,10\n2,20\n3,30\n");
        callTool("open_document", {{"path", path("amp.sch")}});
        probe("B import_data name = the schematic's own dataset", "import_data", {{"path", path("amp.sch")}, {"file", path("meas.csv")}, {"name", "amp"}}, path("amp.dat"));
        write(path("other.dat"), "<Qucs Dataset 27.0.3>\n<indep x 2>\n  +1\n  +2\n</indep>\n<dep w x>\n  +7\n  +8\n</dep>\n");
        probe("B2 import_data name = a dataset of no import", "import_data", {{"path", path("amp.sch")}, {"file", path("meas.csv")}, {"name", "other"}}, path("other.dat"));
        // C. open_document of a schematic with unsaved changes whose file changed on disk.
        write(path("c.sch"), empty.toUtf8());
        callTool("open_document", {{"path", path("c.sch")}});
        callTool("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}});
        QTest::qWait(1100);
        write(path("c.sch"), empty.toUtf8().replace("<Components>\n", "<Components>\n  <C C9 1 300 100 -26 17 0 0 \"1 pF\" 1 \"\" 0 \"neutral\" 0>\n"));
        QTest::qWait(1500);
        probe("C open_document again", "open_document", {{"path", path("c.sch")}});
        Schematic* c = app->currentSchematic();
        qInfo("PROBE C after: %s, %d parts, unsaved %d", c ? qPrintable(c->getDocName()) : "-", c ? int(c->a_DocComps.size()) : -1, c ? int(c->getDocChanged()) : -1);
        probe("C2 show_document", "show_document", {{"path", path("c.sch")}});
        c = app->currentSchematic();
        qInfo("PROBE C2 after: %d parts, unsaved %d", c ? int(c->a_DocComps.size()) : -1, c ? int(c->getDocChanged()) : -1);
        // D. a model changed, closed discarding: irreversible?
        QJsonObject r = callTool("fem_model", {{"action", "new"}, {"path", path("m.qfem")}, {"model", QJsonDocument::fromJson(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
              "geometry": [{"type": "rectangle", "label": "gap", "size": ["10", "1"]}],
              "materials": [{"label": "glass", "epsilonr": "5", "selection": {"all": true}}],
              "physics": [{"type": "electrostatics", "tag": "es", "features": [
                {"type": "terminal", "name": "1", "selection": {"of": ["gap"], "side": "top"}},
                {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}]}],
            "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})").object()}});
        qInfo("PROBE D made: %s", qPrintable(text(r)));
        r = callTool("fem_model", {{"action", "save"}, {"path", path("m.qfem")}});
        qInfo("PROBE D saved: %s", qPrintable(text(r)));
        r = callTool("fem_edit", {{"path", path("m.qfem")}, {"action", "parameters"}, {"parameters", QJsonArray{QJsonObject{{"name", "w"}, {"value", "2"}}}}});
        qInfo("PROBE D edited: %s", qPrintable(text(r)));
        for (QucsDoc* d : app->allDocuments())
            if (d->getDocName().endsWith("m.qfem")) qInfo("PROBE D model unsaved %d", int(d->getDocChanged()));
        qInfo("PROBE D irreversible(close discard) %d", int(control->irreversible("close_document", {{"path", path("m.qfem")}, {"unsaved", "discard"}})));
        // E. fem_model open of a model open with unsaved changes.
        probe("E fem_model open (unsaved open)", "fem_model", {{"action", "open"}, {"path", path("m.qfem")}});
        for (QucsDoc* d : app->allDocuments())
            if (d->getDocName().endsWith("m.qfem")) qInfo("PROBE E model unsaved %d", int(d->getDocChanged()));
        // F. new_document data_display over a .dpl there.
        write(path("amp.dpl"), "KEEP DPL\n");
        probe("F new_document data_display (dpl there)", "new_document", {{"kind", "data_display"}, {"path", path("amp.sch")}}, path("amp.dpl"));
        // G. export_* to a schematic that is open with unsaved changes.
        probe("G export_netlist save_as an open schematic", "export_netlist", {{"path", path("c.sch")}, {"save_as", path("amp.sch")}, {"replace", true}}, path("amp.sch"));
        // H. copy_document into a folder that has the name.
        QDir().mkpath(path("into"));
        write(path("into/c.sch"), "KEEP COPY\n");
        probe("H copy_document to a folder holding the name", "copy_document", {{"path", path("c.sch")}, {"to", path("into")}}, path("into/c.sch"));
        // I. fem_export of a touchstone node to a file that is there.
        QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/coax_rlgc.qfem"), path("coax.qfem"));
        r = callTool("open_document", {{"path", path("coax.qfem")}});
        write(path("keep.s2p"), "KEEP S2P\n");
        r = callTool("fem_edit", {{"path", path("coax.qfem")}, {"action", "set"}, {"tag", "ts1"}, {"props", QJsonObject{{"file", path("keep.s2p")}}}});
        qInfo("PROBE I edited: %s", qPrintable(text(r)));
        r = callTool("fem_solve", {{"path", path("coax.qfem")}, {"timeout", 300}});
        qInfo("PROBE I solved: %s", qPrintable(text(r).left(80)));
        probe("I fem_export ts1 to a file there", "fem_export", {{"path", path("coax.qfem")}, {"export", "ts1"}}, path("keep.s2p"));
        r = callTool("fem_edit", {{"path", path("coax.qfem")}, {"action", "set"}, {"tag", "ts1"}, {"props", QJsonObject{{"file", path("c.sch")}}}});
        qInfo("PROBE I2 edited: %s", qPrintable(text(r)));
        probe("I2 fem_export ts1 to an open schematic", "fem_export", {{"path", path("coax.qfem")}, {"export", "ts1"}}, path("c.sch"));
        probe("I3 fem_export ts1 copy", "fem_export", {{"path", path("coax.qfem")}, {"export", "ts1"}, {"copy", true}});
    }

    // Files outside the workspace, through tools allowed in auto mode.
    void filesOutside()
    {
        QDir().mkpath(dir.filePath("outside/LaunchAgents"));
        const QString made = dir.filePath("outside/LaunchAgents/agent.plist");
        QJsonObject r = callTool("new_document", {{"kind", "text"}});
        qInfo("PROBE L new text: %s", qPrintable(text(r)));
        r = callTool("edit_text", {{"edits", QJsonArray{QJsonObject{{"lines", QJsonArray{1, 0}}, {"text", "<plist>anything at all</plist>"}}}}});
        qInfo("PROBE L edit_text: %s", qPrintable(text(r)));
        qInfo("PROBE L irreversible(save_document as a new file outside) %d", int(control->irreversible("save_document", {{"as", made}})));
        r = callTool("save_document", {{"as", made}});
        qInfo("PROBE L save as: %s | file: %s", qPrintable(text(r)), read(made).left(60).constData());
        const QString rc = dir.filePath("outside/dot_zshrc");
        write(rc, "export PATH=/usr/bin\n");
        r = callTool("open_document", {{"path", rc}});
        qInfo("PROBE L2 open: %s", qPrintable(text(r)));
        r = callTool("edit_text", {{"path", rc}, {"edits", QJsonArray{QJsonObject{{"find", "/usr/bin"}, {"replace", "/evil:$PATH"}}}}});
        qInfo("PROBE L2 edit_text: %s", qPrintable(text(r)));
        qInfo("PROBE L2 irreversible(save_document) %d", int(control->irreversible("save_document", {{"path", rc}})));
        r = callTool("save_document", {{"path", rc}});
        qInfo("PROBE L2 save: %s | file now: %s", qPrintable(text(r)), read(rc).left(60).constData());
        qInfo("PROBE L3 irreversible(export_netlist new file outside) %d", int(control->irreversible("export_netlist", {{"save_as", dir.filePath("outside/new.cir")}})));
    }

    // A schematic that runs a command when simulated: what the dock would
    // see of simulating it. (Nothing simulated: no analysis, no call.)
    void commandsCarried()
    {
        const QString file = path("cmd.sch");
        write(file, "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Components>\n"
                    "  <CMD CMD1 1 100 200 -30 20 0 0 \"echo qucs-test\" 1 \"no\" 1 \"no\" 0>\n"
                    "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        qInfo("PROBE M open: %s", qPrintable(text(callTool("open_document", {{"path", file}}))));
        qInfo("PROBE M CheckCommands %d", int(QucsSettings.CheckCommands));
        for (const char* tool : {"simulate", "tune"}) {
            const QJsonObject a{{"path", file}, {"allow_commands", true}};
            qInfo("PROBE M %s: autonomous %d, irreversible %d, askedEachTime \"%s\"", tool, int(control->autonomousTools().contains(tool)),
                  int(control->irreversible(tool, a)), qPrintable(control->askedEachTime(tool, a)));
        }
        const QJsonObject batch{{"calls", QJsonArray{QJsonObject{{"tool", "simulate"}, {"arguments", QJsonObject{{"path", file}}}}}}};
        qInfo("PROBE M batch(simulate): irreversible %d, askedEachTime \"%s\"", int(control->irreversible("batch", batch)), qPrintable(control->askedEachTime("batch", batch)));
        // A command part placed by a tool allowed in auto mode.
        const QJsonObject placed = callTool("add_component", {{"path", file}, {"type", "CMD"}, {"x", 300}, {"y", 100}, {"preview", true}});
        qInfo("PROBE M add_component CMD (preview): error %d %s", int(placed.value("isError").toBool()), qPrintable(text(placed)));
    }

    // get_state's projects by name: an entry passed as it is to the tools
    // that take a project.
    void projectEntries()
    {
        QDir().mkpath(path("alpha_prj"));
        write(path("alpha_prj/a.sch"), "<Qucs Schematic " PACKAGE_VERSION ">\n");
        QDir().mkpath(path("plain"));
        write(path("plain/p.sch"), "<Qucs Schematic " PACKAGE_VERSION ">\n");
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
        const QJsonObject state = QJsonDocument::fromJson(text(callTool("get_state")).toUtf8()).object();
        QJsonDocument full(callTool("get_state").value("content").toArray().at(0).toObject());
        const QJsonObject st = QJsonDocument::fromJson(full.object().value("text").toString().toUtf8()).object();
        qInfo("PROBE N projects: %s", QJsonDocument(st.value("projects").toArray()).toJson(QJsonDocument::Compact).constData());
        for (const QJsonValue& v : st.value("projects").toArray()) {
            const QJsonObject r = callTool("open_project", {{"name", v.toString()}});
            qInfo("PROBE N open_project \"%s\": error %d %s", qPrintable(v.toString()), int(r.value("isError").toBool()), qPrintable(text(r).left(160)));
            const QJsonObject l = callTool("list_documents", {{"folder", v.toString()}});
            qInfo("PROBE N list_documents folder \"%s\": error %d %s", qPrintable(v.toString()), int(l.value("isError").toBool()), qPrintable(text(l).left(120)));
        }
    }

    // The panel when a mode switch between turns starts Claude Code again.
    void panelModeSwitch()
    {
        const QString prog = dir.filePath("quick.sh");
        write(prog, "#!/bin/sh\nwhile IFS= read -r line; do\n  case \"$line\" in\n    *'\"type\":\"user\"'*)\n"
                    "      echo '{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"q-1\",\"model\":\"m\"}'\n"
                    "      echo '{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"Done.\"}]}}'\n"
                    "      echo '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"duration_ms\":5,\"num_turns\":1,\"result\":\"ok\",\"total_cost_usd\":0.001,\"session_id\":\"q-1\"}'\n"
                    "      ;;\n  esac\ndone\n");
        QFile::setPermissions(prog, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(dir.filePath("workspace"));
        panel.session()->setProgram(prog);
        panel.session()->setToolHost(control);
        panel.resize(440, 700);
        panel.show();
        QSignalSpy turns(panel.session(), &qucs_s::claude::Session::turnFinished);
        panel.composer()->setPlainText("Hello");
        panel.sendComposer();
        QVERIFY(turns.wait(10000));
        QTest::qWait(300);
        qInfo("PROBE O after the turn: state \"%s\", running %d", qPrintable(panel.stateLabel()->text()), int(panel.session()->isRunning()));
        for (QAction* a : panel.permissionActions())
            if (a->data().toString() == QLatin1String("auto")) a->trigger();
        QTest::qWait(500);
        panel.renderNow();
        qInfo("PROBE O after Auto: state \"%s\", running %d, mode %s", qPrintable(panel.stateLabel()->text()), int(panel.session()->isRunning()), qPrintable(panel.session()->permissionMode()));
        const QString t = panel.transcriptText();
        qInfo("PROBE O transcript's end: %s", qPrintable(t.right(200).replace('\n', " | ")));
        panel.session()->stop();
    }

    // undo's 'files' after the user changed a file a call wrote.
    void undoFiles()
    {
        const QString file = path("undo_me.sch");
        write(file, "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        callTool("open_document", {{"path", file}});
        QJsonObject r = callTool("export_netlist", {{"path", file}, {"save_as", path("written.cir")}});
        qInfo("PROBE P export: %s", qPrintable(text(r)));
        QTest::qWait(1100);
        write(path("written.cir"), "* the user's own edit\n");
        qInfo("PROBE P irreversible(undo files) %d", int(control->irreversible("undo", {{"files", true}})));
        r = callTool("undo", {{"files", true}});
        qInfo("PROBE P undo files: error %d %s | file now: %s", int(r.value("isError").toBool()), qPrintable(text(r)), QFileInfo::exists(path("written.cir")) ? read(path("written.cir")).left(40).constData() : "(gone)");
    }

    // new_project with a name that climbs out of the workspace.
    void projectNames()
    {
        for (const char* name : {"../escaped", "sub/inner", "/tmp/zz_hunt_abs_project"}) {
            const QJsonObject r = callTool("new_project", {{"name", name}, {"open", false}});
            qInfo("PROBE Q new_project \"%s\": error %d %s", name, int(r.value("isError").toBool()), qPrintable(text(r).left(200)));
        }
        qInfo("PROBE Q outside the workspace: escaped %d, escaped_prj %d, abs %d", int(QFileInfo::exists(dir.filePath("escaped"))), int(QFileInfo::exists(dir.filePath("escaped_prj"))),
              int(QFileInfo::exists("/tmp/zz_hunt_abs_project") || QFileInfo::exists("/tmp/zz_hunt_abs_project_prj")));
    }

    // A streamed piece in a conversation showing 300 plots: each render looks
    // at every image file shown.
    void manyImages()
    {
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(dir.filePath("workspace"));
        panel.resize(440, 700);
        panel.show();
        QDir().mkpath(path("plots"));
        QImage im(40, 30, QImage::Format_RGB32);
        im.fill(Qt::darkGreen);
        auto feed = [&panel](const QJsonObject& m) { panel.session()->handleLine(QJsonDocument(m).toJson(QJsonDocument::Compact)); };
        for (int i = 0; i < 300; ++i) {
            const QString f = path(QStringLiteral("plots/p%1.png").arg(i));
            im.save(f);
            feed({{"type", "assistant"}, {"message", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", QStringLiteral("Plot %1:\n\n![p](%2)").arg(i).arg(QUrl::fromLocalFile(f).toString())}}}}}}});
        }
        panel.renderNow();
        feed({{"type", "stream_event"}, {"event", QJsonObject{{"type", "content_block_start"}, {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}}}});
        QElapsedTimer t;
        qint64 most = 0, total = 0;
        for (int k = 0; k < 20; ++k) {
            feed({{"type", "stream_event"}, {"event", QJsonObject{{"type", "content_block_delta"}, {"delta", QJsonObject{{"type", "text_delta"}, {"text", "more "}}}}}});
            t.start();
            panel.renderNow();
            const qint64 us = t.nsecsElapsed() / 1000;
            most = std::max(most, us);
            total += us;
        }
        qInfo("PROBE R a streamed piece with 300 images shown: %lld us on average, %lld at most", total / 20, most);
    }

    // A file outside the workspace read through open_document and get_text.
    void filesRead()
    {
        QDir().mkpath(dir.filePath("outside/.aws"));
        const QString secret = dir.filePath("outside/.aws/credentials");
        write(secret, "[default]\naws_secret_access_key = NOT-A-REAL-KEY\n");
        QJsonObject r = callTool("open_document", {{"path", secret}});
        qInfo("PROBE S open: error %d %s", int(r.value("isError").toBool()), qPrintable(text(r).left(140)));
        r = callTool("get_text", {{"path", secret}});
        qInfo("PROBE S get_text: error %d %s", int(r.value("isError").toBool()), qPrintable(text(r).left(200)));
        qInfo("PROBE S autonomous(open_document) %d, read-only(get_text) %d", int(control->autonomousTools().contains("open_document")), int(control->readOnlyTools().contains("get_text")));
    }

    // A document saved over a change made to its file outside Qucs-S.
    void savedOverOutsideChange()
    {
        // A text document.
        const QString txt = path("notes.txt");
        write(txt, "line one\n");
        callTool("open_document", {{"path", txt}});
        callTool("edit_text", {{"path", txt}, {"edits", QJsonArray{QJsonObject{{"find", "one"}, {"replace", "one, edited in Qucs-S"}}}}});
        QTest::qWait(1100);
        write(txt, "line one\nTHE USER'S OWN LINE, written elsewhere\n");
        QTest::qWait(1500);
        qInfo("PROBE T irreversible(save_document text) %d", int(control->irreversible("save_document", {{"path", txt}})));
        QJsonObject r = callTool("save_document", {{"path", txt}});
        qInfo("PROBE T text save: error %d %s | file: %s", int(r.value("isError").toBool()), qPrintable(text(r).left(160)), read(txt).replace('\n', " / ").constData());
        // A model.
        QJsonObject made = callTool("fem_model", {{"action", "new"}, {"path", path("t.qfem")}, {"model", QJsonDocument::fromJson(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "mm",
              "geometry": [{"type": "rectangle", "label": "gap", "size": ["10", "1"]}],
              "materials": [{"label": "glass", "epsilonr": "5", "selection": {"all": true}}],
              "physics": [{"type": "electrostatics", "tag": "es", "features": [
                {"type": "terminal", "name": "1", "selection": {"of": ["gap"], "side": "top"}},
                {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}]}],
            "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})").object()}});
        callTool("fem_model", {{"action", "save"}, {"path", path("t.qfem")}});
        callTool("fem_edit", {{"path", path("t.qfem")}, {"action", "parameters"}, {"parameters", QJsonArray{QJsonObject{{"name", "w"}, {"value", "2"}}}}});
        QTest::qWait(1100);
        QByteArray outside = read(path("t.qfem"));
        outside.replace("\"glass\"", "\"glass changed outside\"");
        write(path("t.qfem"), outside);
        QTest::qWait(1500);
        qInfo("PROBE T irreversible(fem_model save) %d", int(control->irreversible("fem_model", {{"action", "save"}, {"path", path("t.qfem")}})));
        r = callTool("fem_model", {{"action", "save"}, {"path", path("t.qfem")}});
        qInfo("PROBE T model save: error %d %s | the outside change kept %d", int(r.value("isError").toBool()), qPrintable(text(r).left(200)), int(read(path("t.qfem")).contains("glass changed outside")));
    }

    // Long runs without a break in the chat's transcript (the viewer gives
    // them breaks since a856e00).
    void longRunsInTheChat()
    {
        for (const char* how : {"plain"}) {
            for (int size : {64000, 128000}) {
                ClaudeCodePanel panel;
                panel.setDefaultDirectory(dir.filePath("workspace"));
                panel.resize(440, 700);
                panel.show();
                QString run;
                for (int i = 0; i < size; ++i) run += QChar(QLatin1Char('a' + (i * 7) % 26));
                const QString body = QString::fromLatin1(how) == "plain" ? run : QString::fromLatin1(how) == "quoted" ? "> > > " + run : "```\n" + run + "\n```";
                panel.session()->handleLine(QJsonDocument(QJsonObject{{"type", "assistant"}, {"message", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", body}}}}}}}).toJson(QJsonDocument::Compact));
                QElapsedTimer t;
                t.start();
                panel.renderNow();
                qInfo("PROBE U %s run of %d letters: %lld ms", how, size, t.elapsed());
            }
        }
    }

    // make_symbol over a symbol drawn by hand, then undo.
    void symbolRedrawn()
    {
        const QString file = path("sub1.sch");
        write(file, "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n"
                    "  <Line -20 -30 40 0 #ff0000 3 1>\n  <Ellipse -15 -15 30 30 #00ff00 2 1 #c0c0c0 1 0>\n  <.PortSym -30 0 1 0>\n</Symbol>\n<Components>\n"
                    "  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n  <R R1 1 200 100 15 -26 0 1 \"1 kOhm\" 1 \"\" 0 \"neutral\" 0>\n</Components>\n<Wires>\n"
                    "  <100 100 200 70 \"\" 0 0 0 \"\">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        callTool("open_document", {{"path", file}});
        Schematic* sch = app->currentSchematic();
        const auto symbol = [sch] { return sch ? int(sch->a_SymbolPaints.size()) : -1; };
        qInfo("PROBE V symbol parts before %d", symbol());
        QJsonObject r = callTool("make_symbol", {{"path", file}});
        qInfo("PROBE V make_symbol: error %d | symbol parts %d", int(r.value("isError").toBool()), symbol());
        qInfo("PROBE V answer: %s", r.value("content").toArray().at(0).toObject().value("text").toString().toUtf8().constData());
        r = callTool("undo", {{"path", file}});
        qInfo("PROBE V undo: error %d %s | symbol parts %d", int(r.value("isError").toBool()), qPrintable(text(r).left(160)), symbol());
        if (sch) {
            const bool undone = sch->undo();
            qInfo("PROBE V the window's Edit > Undo: %d | symbol parts %d, in symbol mode %d", int(undone), symbol(), int(sch->getSymbolMode()));
        }
        r = callTool("undo_history", {{"path", file}});
        qInfo("PROBE V undo_history: %s", qPrintable(text(r).left(300)));
    }

    // "Each use undone by Edit > Undo": a call, one undo, the schematic as before?
    void oneUndoEach()
    {
        const QByteArray rc = "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n"
                              "  <Vdc V1 1 100 200 18 -26 0 1 \"1 V\" 1>\n  <R R1 1 200 120 -26 15 0 0 \"1 kOhm\" 1 \"\" 0 \"neutral\" 0>\n"
                              "  <C C1 1 300 200 17 -26 0 1 \"1 nF\" 1 \"\" 0 \"neutral\" 0>\n  <GND * 1 100 260 0 0 0 0>\n  <GND * 1 300 260 0 0 0 0>\n"
                              "  <.DC DC1 1 100 350 0 46 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
                              "</Components>\n<Wires>\n  <100 120 100 170 \"\" 0 0 0 \"\">\n  <100 120 170 120 \"in\" 130 90 0 \"\">\n  <230 120 300 120 \"out\" 260 90 0 \"\">\n"
                              "  <300 120 300 170 \"\" 0 0 0 \"\">\n  <100 230 100 260 \"\" 0 0 0 \"\">\n  <300 230 300 260 \"\" 0 0 0 \"\">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
        const auto strip = [](QString t) { return t.remove(QRegularExpression(QStringLiteral("  <View=[^>]*>\n"))); };
        struct Case {
            const char* tool;
            QJsonObject args;
        };
        const QList<Case> cases = {
            {"create_subcircuit", {{"names", QJsonArray{"R1", "C1"}}, {"save_as", "sub_rc"}}},
            {"replace_component", {{"name", "R1"}, {"type", "L"}}},
            {"rename_net", {{"from", "out"}, {"to", "vout"}}},
            {"add_analysis", {{"kind", "tran"}, {"plot", QJsonArray{"out"}}}},
            {"set_subcircuit_parameters", {{"parameters", QJsonArray{QJsonObject{{"name", "Rs"}, {"default", "50"}}}}}},
            {"move", {{"names", QJsonArray{"R1"}}, {"dx", 40}, {"dy", 0}}},
            {"delete", {{"names", QJsonArray{"C1"}}}},
            {"add_component", {{"type", "R"}, {"x", 500}, {"y", 100}}},
            {"edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "2 kOhm"}}}}},
            {"connect", {{"from", "R1.2"}, {"to", "ground"}}},
            {"set_label", {{"at", "C1.1"}, {"name", "lbl"}}},
            {"add_painting", {{"type", "text"}, {"text", "hello"}, {"x", 400}, {"y", 400}}},
            {"probe", {{"what", "out"}}},
            {"add_diagram", {{"type", "rect"}, {"x", 500}, {"y", 500}, {"traces", QJsonArray{"out.V"}}}},
        };
        int n = 0;
        for (const Case& c : cases) {
            const QString file = path(QStringLiteral("undo_%1.sch").arg(++n));
            write(file, rc);
            callTool("open_document", {{"path", file}});
            Schematic* sch = app->currentSchematic();
            const QString before = strip(sch->documentText());
            QJsonObject a = c.args;
            a.insert("path", file);
            const QJsonObject r = callTool(c.tool, a);
            const QString after = strip(sch->documentText());
            const QJsonObject u = callTool("undo", {{"path", file}});
            const QString back = strip(sch->documentText());
            if (QString::fromLatin1(c.tool) == "add_analysis") {
                qInfo("PROBE W add_analysis answer: %s", r.value("content").toArray().at(0).toObject().value("text").toString().toUtf8().constData());
                const QJsonObject u2 = callTool("undo", {{"path", file}});
                qInfo("PROBE W add_analysis second undo: %s | as before %d", qPrintable(text(u2).left(60)), int(strip(sch->documentText()) == before));
            }
            qInfo("PROBE W %-26s call error %d (%s) | changed %d | undo: %s | as before %d", c.tool, int(r.value("isError").toBool()),
                  r.value("isError").toBool() ? qPrintable(text(r).left(120)) : "ok", int(after != before), qPrintable(text(u).left(40)), int(back == before));
            sch->setChanged(false);
            sch->setDocChanged(false);
            callTool("close_document", {{"path", file}, {"unsaved", "discard"}});
        }
    }

    // The dock in auto mode with the real host: a call that discards unsaved
    // changes, held - its card, denied.
    void heldInThePanel()
    {
        const QString file = path("held.sch");
        write(file, "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        callTool("open_document", {{"path", file}});
        callTool("add_component", {{"path", file}, {"type", "R"}, {"x", 100}, {"y", 100}});
        const QString prog = dir.filePath("closer.sh");
        const QString call = QStringLiteral(R"({"type":"control_request","request_id":"m9","request":{"subtype":"mcp_message","server_name":"%1","message":{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"close_document","arguments":{"path":"%2","unsaved":"discard"},"_meta":{"claudecode/toolUseId":"u9"}}}}})")
                                 .arg(control->serverName(), file);
        write(prog, ("#!/bin/sh\nwhile IFS= read -r line; do\n  printf '%s\\n' \"$line\" >> \"" + dir.filePath("closer-in") + "\"\n  case \"$line\" in\n    *'\"type\":\"user\"'*)\n"
                     "      echo '{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"c-1\",\"model\":\"m\"}'\n"
                     "      echo '{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"u9\",\"name\":\"mcp__" + control->serverName() + "__close_document\",\"input\":{}}]}}'\n"
                     "      echo '" + call + "'\n      ;;\n  esac\ndone\n").toUtf8());
        QFile::setPermissions(prog, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(dir.filePath("workspace"));
        panel.session()->setProgram(prog);
        panel.session()->setToolHost(control);
        panel.session()->setPermissionMode("auto");
        panel.resize(440, 700);
        panel.show();
        panel.composer()->setPlainText("Close it");
        panel.sendComposer();
        QTRY_VERIFY_WITH_TIMEOUT(panel.permissionCard()->isVisible(), 10000);
        QStringList texts;
        for (QLabel* l : panel.permissionCard()->findChildren<QLabel*>()) if (l->isVisible()) texts << l->text();
        qInfo("PROBE X card: %s", qPrintable(texts.join(" | ").left(400)));
        for (QPlainTextEdit* e : panel.permissionCard()->findChildren<QPlainTextEdit*>())
            qInfo("PROBE X detail box (%s, visible %d): %s", qPrintable(e->objectName()), int(e->isVisible()), qPrintable(e->toPlainText().left(300).replace('\n', " / ")));
        QStringList buttons;
        QToolButton* deny = nullptr;
        for (QToolButton* b : panel.permissionCard()->findChildren<QToolButton*>())
            if (b->isVisible()) {
                buttons << b->objectName() + "=" + b->text();
                if (b->objectName() == QLatin1String("claudeDeny")) deny = b;
            }
        qInfo("PROBE X buttons: %s", qPrintable(buttons.join(", ")));
        if (deny) deny->click();
        QTest::qWait(500);
        bool open = false;
        for (QucsDoc* d : app->allDocuments()) open = open || d->getDocName() == file;
        qInfo("PROBE X denied: the document still open %d; the program was told: %s", int(open), qPrintable(QString::fromUtf8(read(dir.filePath("closer-in"))).section("mcp_response", 1).left(200)));
        panel.session()->stop();
    }

    // A mode switch between turns while a task runs in the background.
    void modeSwitchWithABackgroundTask()
    {
        const QString prog = dir.filePath("bg.sh");
        write(prog, "#!/bin/sh\nwhile IFS= read -r line; do\n  case \"$line\" in\n    *'\"type\":\"user\"'*)\n"
                    "      echo '{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"bg-1\",\"model\":\"m\"}'\n"
                    "      echo '{\"type\":\"system\",\"subtype\":\"task_started\",\"task_id\":\"k1\",\"tool_use_id\":\"b1\",\"description\":\"make all\",\"is_backgrounded\":true,\"task_type\":\"local_bash\"}'\n"
                    "      echo '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"duration_ms\":5,\"num_turns\":1,\"result\":\"ok\",\"total_cost_usd\":0.001,\"session_id\":\"bg-1\"}'\n"
                    "      ;;\n  esac\ndone\n");
        QFile::setPermissions(prog, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qucs_s::claude::Session s;
        s.setProgram(prog);
        s.setWorkingDirectory(dir.filePath("workspace"));
        s.setToolHost(control);
        s.setPermissionMode("default");
        QSignalSpy turns(&s, &qucs_s::claude::Session::turnFinished);
        QSignalSpy notices(&s, &qucs_s::claude::Session::notice);
        QVERIFY(s.send("Build it in the background"));
        QVERIFY(turns.wait(10000));
        QTest::qWait(200);
        qInfo("PROBE J after the turn: running %d, busy %d", int(s.isRunning()), int(s.isBusy()));
        s.setPermissionMode("auto");
        QTest::qWait(300);
        qInfo("PROBE J after switching to auto between turns, a task in the background: running %d; notices %lld", int(s.isRunning()), qint64(notices.count()));
        s.stop();
    }

    void cleanupTestCase()
    {
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
        delete server;
        delete app;
        QucsMain = nullptr;
    }
};

QTEST_MAIN(ZzHunt)
#include "zz_hunt.moc"
