/*
 * The Claude Code dock: the session that drives the claude program over
 * stream-json (claudecode.h), a conversation (claudecodepanel.h) - its
 * tools folded into lines, its math typeset (mathtypeset.h) - the
 * conversations in tabs (claudecodetabs.h), and what the application
 * does with them - the dock works in the workspace folder, the status bar
 * follows the sessions, files Claude changed are loaded again. The
 * program is a shell script here that answers as claude does.
 */
#include <QtTest>
#include <QApplication>
#include <QDockWidget>
#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMap>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFormat>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

#include "claudecode.h"
#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "mathtypeset.h"
#include "config.h"
#include "isolated_settings.h"
#include "main.h"
#include "messagedock.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "schematic.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"

using qucs_s::claude::ModelChoice;
using qucs_s::claude::ModelQuery;
using qucs_s::claude::PermissionRequest;
using qucs_s::claude::Session;
using qucs_s::claude::State;
using qucs_s::claude::TurnResult;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// What claude says in a turn that writes a file: the tool, a permission
// request for it, and - once answered - the result, a reply and the end.
const char* const kWriter = R"SH(#!/bin/sh
printf '%s\n' "$@" > "$QUCS_FAKE_DIR/args"
dir=$(pwd)
while IFS= read -r line; do
  case "$line" in
    *'"type":"user"'*)
      printf '%s\n' "$line" >> "$QUCS_FAKE_DIR/prompts"
      echo '{"type":"system","subtype":"init","session_id":"s-1","model":"claude-test-1","claude_code_version":"9.9"}'
      echo '{"type":"stream_event","event":{"type":"content_block_start","content_block":{"type":"text","text":""}}}'
      echo '{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"I will write "}}}'
      echo '{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"the file."}}}'
      echo '{"type":"assistant","message":{"content":[{"type":"text","text":"I will write the file."}]}}'
      echo '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Write","input":{"file_path":"'"$dir"'/out.txt","content":"hi"}}]}}'
      echo '{"type":"control_request","request_id":"r1","request":{"subtype":"can_use_tool","tool_name":"Write","input":{"file_path":"'"$dir"'/out.txt","content":"hi"},"permission_suggestions":[{"type":"setMode","mode":"acceptEdits","destination":"session"}]}}'
      ;;
    *'"type":"control_response"'*)
      printf '%s\n' "$line" >> "$QUCS_FAKE_DIR/answers"
      case "$line" in
        *'"allow"'*)
          echo '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"written","is_error":false}]}}'
          echo '{"type":"assistant","message":{"content":[{"type":"text","text":"Done: wrote **out.txt**."}]}}'
          echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":1200,"num_turns":2,"result":"Done","total_cost_usd":0.0123,"permission_denials":[],"session_id":"s-1"}'
          ;;
        *)
          echo '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"The user did not allow this.","is_error":true}]}}'
          echo '{"type":"assistant","message":{"content":[{"type":"text","text":"Understood, I left it."}]}}'
          echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":900,"num_turns":2,"result":"x","total_cost_usd":0.004,"permission_denials":[{"tool_name":"Write"}],"session_id":"s-1"}'
          ;;
      esac
      ;;
  esac
done
)SH";

// A turn that goes on until it is interrupted.
const char* const kSlow = R"SH(#!/bin/sh
while IFS= read -r line; do
  case "$line" in
    *'"type":"user"'*)
      echo '{"type":"system","subtype":"init","session_id":"s-2","model":"claude-test-1"}'
      echo '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"sleep 100"}}]}}'
      ;;
    *'"subtype":"interrupt"'*)
      echo '{"type":"result","subtype":"error_during_execution","is_error":true,"duration_ms":300,"num_turns":1,"result":"","total_cost_usd":0.001,"permission_denials":[]}'
      ;;
  esac
done
)SH";

// What claude answers when it is asked which models it offers: its
// default, an alias, a model by its full name - and then it ends, its
// input closed.
const char* const kLister = R"SH(#!/bin/sh
printf '%s\n' "$@" > "$QUCS_FAKE_DIR/lister-args"
while IFS= read -r line; do
  case "$line" in
    *'"subtype":"initialize"'*)
      printf '%s\n' "$line" > "$QUCS_FAKE_DIR/lister-request"
      printf '%s\n' '{"type":"control_response","response":{"subtype":"success","request_id":"qucs-models","response":{"models":[{"value":"default","resolvedModel":"claude-opus-5[1m]","displayName":"Default","description":"Opus 5 with 1M context \u00b7 Best for everyday tasks","supportsAutoMode":true},{"value":"claude-fable-5-1[1m]","resolvedModel":"claude-fable-5-1","displayName":"Fable","description":"Fable 5.1 \u00b7 Most capable","supportsAutoMode":true},{"value":"haiku","resolvedModel":"claude-haiku-4-5-20251001","displayName":"Haiku","description":"Haiku 4.5 \u00b7 Fastest"}]}}}'
      ;;
  esac
done
)SH";

// claude with a host's tool server: at the host's initialize, the MCP
// handshake and tools/list; at the prompt, a permission request for a
// tool that only looks and one for a tool that changes; once that is
// allowed (with all the host's tools), a call of it, then another request
// that needs no asking, then the end. What the session writes back goes
// to mcp-in.
const char* const kToolUser = R"SH(#!/bin/sh
printf '%s\n' "$@" > "$QUCS_FAKE_DIR/mcp-args"
while IFS= read -r line; do
  printf '%s\n' "$line" >> "$QUCS_FAKE_DIR/mcp-in"
  case "$line" in
    *'"subtype":"initialize"'*)
      echo '{"type":"control_request","request_id":"m1","request":{"subtype":"mcp_message","server_name":"fake","message":{"method":"initialize","params":{"protocolVersion":"2025-11-25"},"jsonrpc":"2.0","id":0}}}'
      echo '{"type":"control_request","request_id":"m2","request":{"subtype":"mcp_message","server_name":"fake","message":{"jsonrpc":"2.0","method":"notifications/initialized"}}}'
      echo '{"type":"control_request","request_id":"m3","request":{"subtype":"mcp_message","server_name":"fake","message":{"method":"tools/list","jsonrpc":"2.0","id":1}}}'
      ;;
    *'"type":"user"'*)
      echo '{"type":"system","subtype":"init","session_id":"mcp-1","model":"claude-test-1"}'
      echo '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"u1","name":"mcp__fake__change","input":{"what":"x"}}]}}'
      echo '{"type":"control_request","request_id":"p1","request":{"subtype":"can_use_tool","tool_name":"mcp__fake__look","display_name":"Look","input":{},"permission_suggestions":[]}}'
      echo '{"type":"control_request","request_id":"p2","request":{"subtype":"can_use_tool","tool_name":"mcp__fake__change","display_name":"Change","input":{"what":"x"},"permission_suggestions":[]}}'
      ;;
    *'"request_id":"p2"'*'"allow"'*)
      echo '{"type":"control_request","request_id":"m4","request":{"subtype":"mcp_message","server_name":"fake","message":{"method":"tools/call","params":{"name":"change","arguments":{"what":"x"}},"jsonrpc":"2.0","id":2}}}'
      ;;
    *'"request_id":"m4"'*)
      echo '{"type":"control_request","request_id":"p3","request":{"subtype":"can_use_tool","tool_name":"mcp__fake__change","display_name":"Change","input":{"what":"y"},"permission_suggestions":[]}}'
      ;;
    *'"request_id":"p3"'*'"allow"'*)
      echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":5,"num_turns":1,"result":"ok","total_cost_usd":0.001,"session_id":"mcp-1"}'
      ;;
  esac
done
)SH";

// The host of tools the fake program is told of.
class FakeHost : public qucs_s::claude::ToolHost
{
public:
    QString serverName() const override { return QStringLiteral("fake"); }
    QJsonArray tools() const override
    {
        return {QJsonObject{{"name", "look"}, {"description", "Looks"}, {"inputSchema", QJsonObject{{"type", "object"}}}},
                QJsonObject{{"name", "change"}, {"description", "Changes"}, {"inputSchema", QJsonObject{{"type", "object"}}}}};
    }
    QStringList readOnlyTools() const override { return {QStringLiteral("look")}; }
    QString actionOf(const QString&) const override { return QStringLiteral("change something in the fake"); }
    QString subjectOf(const QString&, const QJsonObject& a) const override
    {
        return QStringLiteral("what: ") + a.value("what").toString()
               + (a.contains("document") ? QStringLiteral(" in ") + a.value("document").toString() : QString());
    }
    QString instructions() const override { return QStringLiteral("Fake instructions."); }
    void callTool(const QString& tool, const QJsonObject& a, std::function<void(const QJsonObject&)> done) override
    {
        calls << tool;
        arguments << a;
        done(QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "changed " + a.value("what").toString()}}}},
                         {"isError", false}});
    }
    // A conversation pinned: "change" is given the document.
    QJsonObject forDocument(const QString& tool, const QJsonObject& a, const QString& document) const override
    {
        if (tool != QLatin1String("change")) return a;
        QJsonObject with = a;
        with.insert("document", document);
        return with;
    }
    QStringList calls;
    QList<QJsonObject> arguments;
};

// A program that fails at once.
const char* const kBroken = "#!/bin/sh\necho 'Invalid API key' >&2\nexit 3\n";

} // namespace

class TestClaudeCode : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString script(const QString& name, const char* text)
    {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(text);
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return f.fileName();
    }

    QString fresh(const QString& name)
    {
        const QString path = dir.filePath(name);
        QDir(path).removeRecursively();
        QDir().mkpath(path);
        return QFileInfo(path).canonicalFilePath();
    }

    // The math typeset in a transcript: each image's TeX (its tool tip).
    static QStringList mathIn(QTextDocument* doc)
    {
        QStringList tex;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextCharFormat f = it.fragment().charFormat();
                if (f.objectType() == qucs_s::math::MathObject::Type) tex << f.toolTip();
            }
        return tex;
    }

    static QString read(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

    void skipWithoutShell()
    {
#ifdef Q_OS_WIN
        QSKIP("the fake claude is a shell script");
#endif
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        qputenv("QUCS_FAKE_DIR", dir.path().toUtf8());
        qputenv("QUCS_CLAUDE", "/nonexistent/claude");   // never the real one
    }

    // -p with stream-json both ways, partial messages and the permission
    // prompts to us; the mode only when it is not "ask", the model, the
    // session to continue.
    void theCommandLineCarriesTheOptions()
    {
        const QStringList plain = qucs_s::claude::arguments({});
        QVERIFY(plain.contains("-p"));
        QCOMPARE(plain.at(plain.indexOf("--input-format") + 1), QStringLiteral("stream-json"));
        QCOMPARE(plain.at(plain.indexOf("--output-format") + 1), QStringLiteral("stream-json"));
        QVERIFY(plain.contains("--verbose"));
        QVERIFY(plain.contains("--include-partial-messages"));
        QCOMPARE(plain.at(plain.indexOf("--permission-prompt-tool") + 1), QStringLiteral("stdio"));
        QVERIFY(!plain.contains("--permission-mode"));
        QVERIFY(!plain.contains("--resume"));
        QVERIFY(!plain.contains("--model"));

        qucs_s::claude::Options options;
        options.permissionMode = "acceptEdits";
        options.model = "opus";
        options.resume = "s-9";
        options.appendSystemPrompt = "Be brief.";
        options.mcpConfig = "{}";
        options.allowedTools = {"mcp__a__b", "mcp__a__c"};
        const QStringList all = qucs_s::claude::arguments(options);
        QCOMPARE(all.at(all.indexOf("--mcp-config") + 1), QStringLiteral("{}"));
        QCOMPARE(all.at(all.indexOf("--allowedTools") + 1), QStringLiteral("mcp__a__b,mcp__a__c"));
        QCOMPARE(all.at(all.indexOf("--permission-mode") + 1), QStringLiteral("acceptEdits"));
        QCOMPARE(all.at(all.indexOf("--model") + 1), QStringLiteral("opus"));
        QCOMPARE(all.at(all.indexOf("--resume") + 1), QStringLiteral("s-9"));
        QCOMPARE(all.at(all.indexOf("--append-system-prompt") + 1), QStringLiteral("Be brief."));
    }

    // The program the settings name, else claude on PATH, else where its
    // installers put it (a desktop start has no PATH of the shell's).
    void theProgramIsFoundWhereItsInstallersPutIt()
    {
        skipWithoutShell();
        const QString home = fresh("home");
        QDir().mkpath(home + "/.local/bin");
        QFile f(home + "/.local/bin/claude");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\n");
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        const QByteArray path = qgetenv("PATH");
        qputenv("PATH", fresh("emptypath").toUtf8());
        QCOMPARE(qucs_s::claude::findProgram(QString(), home), f.fileName());
        QCOMPARE(qucs_s::claude::findProgram(f.fileName(), home), f.fileName());
        QCOMPARE(qucs_s::claude::findProgram(home + "/no/such/claude", home), QString());
        QCOMPARE(qucs_s::claude::findProgram(QString(), fresh("nobody")).isEmpty()
                     || !qucs_s::claude::findProgram(QString(), fresh("nobody")).startsWith(dir.path()),
                 true);
        qputenv("PATH", path);
    }

    // What a tool is about, in one line: a file inside the folder
    // relative to it, a command, a pattern.
    void aToolIsSummedUpInALine()
    {
        const QString work = "/work/proj";
        QCOMPARE(qucs_s::claude::toolSubject("Read", {{"file_path", "/work/proj/a/b.sch"}}, work), QStringLiteral("a/b.sch"));
        QCOMPARE(qucs_s::claude::toolSubject("Edit", {{"file_path", "/elsewhere/c.sch"}}, work), QStringLiteral("/elsewhere/c.sch"));
        QCOMPARE(qucs_s::claude::toolSubject("Bash", {{"command", "ngspice -b x.cir"}}, work), QStringLiteral("ngspice -b x.cir"));
        QVERIFY(qucs_s::claude::toolSubject("Bash", {{"command", "a\nb"}}, work).startsWith("a "));
        QCOMPARE(qucs_s::claude::toolSubject("Grep", {{"pattern", "<R"}, {"path", "/work/proj/sub"}}, work),
                 QStringLiteral("<R  in sub"));
        QCOMPARE(qucs_s::claude::toolSubject("WebFetch", {{"url", "https://x.org"}}, work), QStringLiteral("https://x.org"));
    }

    // The stream, line by line, becomes the session's signals and states.
    void theStreamBecomesSignals()
    {
        Session s;
        s.setProgram("claude");
        s.setWorkingDirectory("/work");
        QSignalSpy started(&s, &Session::sessionStarted);
        QSignalSpy streamed(&s, &Session::replyStreamed);
        QSignalSpy finished(&s, &Session::replyFinished);
        QSignalSpy tools(&s, &Session::toolStarted);
        QSignalSpy toolEnds(&s, &Session::toolFinished);
        QSignalSpy asks(&s, &Session::permissionRequested);
        QSignalSpy notices(&s, &Session::notice);
        QSignalSpy turns(&s, &Session::turnFinished);

        s.handleLine(R"({"type":"system","subtype":"init","session_id":"abc","model":"claude-opus-5-5","claude_code_version":"2.1"})");
        QCOMPARE(started.count(), 1);
        QCOMPARE(s.sessionId(), QStringLiteral("abc"));
        QCOMPARE(s.modelInUse(), QStringLiteral("claude-opus-5-5"));

        s.handleLine(R"({"type":"stream_event","event":{"type":"content_block_start","content_block":{"type":"text","text":""}}})");
        s.handleLine(R"({"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"Hel"}}})");
        s.handleLine(R"({"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"lo"}}})");
        QCOMPARE(streamed.count(), 2);
        QCOMPARE(streamed.last().at(0).toString(), QStringLiteral("Hello"));
        s.handleLine(R"({"type":"assistant","message":{"content":[{"type":"text","text":"Hello"}]}})");
        QCOMPARE(finished.count(), 1);

        // A subagent's text is not the reply.
        s.handleLine(R"({"type":"assistant","parent_tool_use_id":"x","message":{"content":[{"type":"text","text":"inner"}]}})");
        QCOMPARE(finished.count(), 1);

        s.handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Edit","input":{"file_path":"/work/a.sch","old_string":"1k","new_string":"2k"}}]}})");
        QCOMPARE(tools.count(), 1);
        QCOMPARE(tools.last().at(1).toString(), QStringLiteral("Edit"));
        QCOMPARE(tools.last().at(2).toString(), QStringLiteral("a.sch"));

        s.handleLine(R"({"type":"control_request","request_id":"r1","request":{"subtype":"can_use_tool","tool_name":"Edit","input":{"file_path":"/work/a.sch","old_string":"1k","new_string":"2k"},"permission_suggestions":[{"type":"setMode","mode":"acceptEdits","destination":"session"}]}})");
        QCOMPARE(asks.count(), 1);
        const auto request = asks.last().at(0).value<PermissionRequest>();
        QCOMPARE(request.id, QStringLiteral("r1"));
        QCOMPARE(request.tool, QStringLiteral("Edit"));
        QCOMPARE(request.subject, QStringLiteral("a.sch"));
        QVERIFY(request.detail.contains("- 1k"));
        QVERIFY(request.detail.contains("+ 2k"));
        QVERIFY(request.canAllowEdits);
        QCOMPARE(s.state(), State::Waiting);

        s.handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":[{"type":"text","text":"ok"}],"is_error":false}]}})");
        QCOMPARE(toolEnds.count(), 1);
        QCOMPARE(toolEnds.last().at(1).toBool(), false);
        QCOMPARE(toolEnds.last().at(2).toString(), QStringLiteral("ok"));

        s.handleLine(R"({"type":"system","subtype":"permission_denied","tool_name":"Bash","message":"Bash was denied"})");
        s.handleLine("a warning that is not JSON");
        QCOMPARE(notices.count(), 2);

        s.handleLine(R"({"type":"result","subtype":"success","is_error":false,"duration_ms":4200,"num_turns":3,"result":"Hello","total_cost_usd":0.25,"permission_denials":[{"tool_name":"Bash"}]})");
        QCOMPARE(turns.count(), 1);
        const auto result = turns.last().at(0).value<TurnResult>();
        QVERIFY(result.ok);
        QCOMPARE(result.durationMs, qint64(4200));
        QCOMPARE(result.turns, 3);
        QCOMPARE(result.denials, 1);
        QCOMPARE(result.costUsd, 0.25);
        QCOMPARE(result.changedFiles, QStringList{"/work/a.sch"});
        QCOMPARE(s.state(), State::Ready);

        // The program counts its cost from its start: a turn's is what it added.
        s.handleLine(R"({"type":"result","subtype":"success","is_error":false,"duration_ms":10,"num_turns":1,"result":"x","total_cost_usd":0.30})");
        QVERIFY(qAbs(turns.last().at(0).value<TurnResult>().costUsd - 0.05) < 1e-9);
        QVERIFY(qAbs(turns.last().at(0).value<TurnResult>().conversationCostUsd - 0.30) < 1e-9);

        s.handleLine(R"({"type":"result","subtype":"error_max_turns","is_error":true,"duration_ms":1,"num_turns":9,"result":""})");
        QCOMPARE(s.state(), State::Failed);
        QVERIFY(s.detail().contains("too many steps"));
    }

    // A whole turn with the program: the prompt goes in, the permission
    // request comes out and its answer goes back (edits allowed for the
    // session), the turn ends with the file it wrote. Stopped, the program
    // is started again for the next prompt, continuing the conversation.
    void aTurnWithTheProgram()
    {
        skipWithoutShell();
        QFile::remove(dir.filePath("answers"));
        QFile::remove(dir.filePath("prompts"));
        const QString work = fresh("work");
        Session s;
        s.setProgram(script("writer", kWriter));
        s.setWorkingDirectory(work);
        QSignalSpy asks(&s, &Session::permissionRequested);
        QSignalSpy turns(&s, &Session::turnFinished);
        QSignalSpy mode(&s, &Session::permissionModeChanged);

        QVERIFY(s.send("Write hi to out.txt"));
        QVERIFY(s.isBusy());
        QVERIFY(asks.wait(10000));
        QCOMPARE(s.state(), State::Waiting);
        const auto request = asks.last().at(0).value<PermissionRequest>();
        QCOMPARE(request.subject, QStringLiteral("out.txt"));
        s.answer(request.id, true, true);
        QVERIFY(turns.count() == 1 || turns.wait(10000));
        const auto result = turns.last().at(0).value<TurnResult>();
        QVERIFY(result.ok);
        QCOMPARE(result.changedFiles, QStringList{work + "/out.txt"});
        QCOMPARE(s.state(), State::Ready);
        QVERIFY(!s.isBusy());
        QCOMPARE(s.sessionId(), QStringLiteral("s-1"));
        QCOMPARE(mode.count(), 1);
        QCOMPARE(s.permissionMode(), QStringLiteral("acceptEdits"));

        const QString answers = read(dir.filePath("answers"));
        QVERIFY(answers.contains("\"behavior\":\"allow\""));
        QVERIFY(answers.contains("\"request_id\":\"r1\""));
        QVERIFY(answers.contains("\"mode\":\"acceptEdits\""));
        QVERIFY(read(dir.filePath("prompts")).contains("Write hi to out.txt"));
        const QStringList args = read(dir.filePath("args")).split('\n');
        QCOMPARE(args.at(args.indexOf("--permission-prompt-tool") + 1), QStringLiteral("stdio"));
        QVERIFY(args.contains("--append-system-prompt"));
        QVERIFY(!args.contains("--resume"));

        // The program stays for the next prompt.
        QVERIFY(s.isRunning());
        s.stop();
        QVERIFY(!s.isRunning());
        QVERIFY(s.send("And again"));
        QVERIFY(asks.wait(10000));
        const QStringList again = read(dir.filePath("args")).split('\n');
        QCOMPARE(again.at(again.indexOf("--resume") + 1), QStringLiteral("s-1"));
        QCOMPARE(again.at(again.indexOf("--permission-mode") + 1), QStringLiteral("acceptEdits"));
        s.answer(asks.last().at(0).value<PermissionRequest>().id, false);
        QVERIFY(turns.wait(10000));
        QCOMPARE(turns.last().at(0).value<TurnResult>().denials, 1);
        // A new program counts from nothing again; the conversation goes on.
        QVERIFY(qAbs(turns.last().at(0).value<TurnResult>().costUsd - 0.004) < 1e-9);
        QVERIFY(qAbs(turns.last().at(0).value<TurnResult>().conversationCostUsd - 0.0163) < 1e-9);
        QVERIFY(read(dir.filePath("answers")).contains("\"behavior\":\"deny\""));
        s.stop();
    }

    // Stop in the middle of a turn: an interrupt request, and the turn
    // ends as stopped, not failed.
    void aTurnIsStopped()
    {
        skipWithoutShell();
        Session s;
        s.setProgram(script("slow", kSlow));
        s.setWorkingDirectory(fresh("slowwork"));
        QSignalSpy tools(&s, &Session::toolStarted);
        QSignalSpy turns(&s, &Session::turnFinished);
        QVERIFY(s.send("Take your time"));
        QVERIFY(tools.wait(10000));
        QCOMPARE(s.state(), State::Working);
        QCOMPARE(s.detail(), QStringLiteral("Bash"));
        s.interrupt();
        QVERIFY(turns.wait(10000));
        QVERIFY(turns.last().at(0).value<TurnResult>().stopped);
        QCOMPARE(s.state(), State::Ready);
        s.stop();
    }

    // A program that fails says why: its exit code and what it wrote.
    void aFailingProgramIsReported()
    {
        skipWithoutShell();
        Session s;
        s.setProgram(script("broken", kBroken));
        s.setWorkingDirectory(fresh("brokenwork"));
        QSignalSpy failed(&s, &Session::failed);
        QVERIFY(s.send("Hello"));
        QVERIFY(failed.wait(10000));
        QVERIFY(failed.last().at(0).toString().contains("exit code 3"));
        QVERIFY(failed.last().at(0).toString().contains("Invalid API key"));
        QCOMPARE(s.state(), State::Failed);
        QVERIFY(!s.isBusy());

        Session none;
        none.setProgram(QString());
        QCOMPARE(none.state(), State::NotFound);
        QVERIFY(!none.send("Hello"));
    }

    // The host's tools (an "sdk" MCP server) over the program's own
    // stream: the program is told of the server and which tools need no
    // asking; the handshake, tools/list and tools/call are answered from
    // the host; a tool that looks is used without a question, one that
    // changes asks - and once the host's tools are allowed, no more.
    void theHostsToolsAreServedOverTheStream()
    {
        skipWithoutShell();
        QFile::remove(dir.filePath("mcp-in"));
        FakeHost host;
        Session s;
        s.setProgram(script("tooluser", kToolUser));
        s.setWorkingDirectory(fresh("toolwork"));
        s.setToolHost(&host);
        QSignalSpy asks(&s, &Session::permissionRequested);
        QSignalSpy tools(&s, &Session::toolStarted);
        QSignalSpy turns(&s, &Session::turnFinished);
        QVERIFY(s.send("Change x"));
        QVERIFY(asks.wait(10000));
        QCOMPARE(asks.count(), 1);   // not for the tool that looks
        const auto request = asks.last().at(0).value<PermissionRequest>();
        QCOMPARE(request.tool, QStringLiteral("mcp__fake__change"));
        QCOMPARE(request.action, QStringLiteral("change something in the fake"));
        QCOMPARE(request.subject, QStringLiteral("what: x"));
        QVERIFY(request.canAllowTools);
        QCOMPARE(tools.count(), 1);
        QCOMPARE(tools.last().at(2).toString(), QStringLiteral("what: x"));   // the host says what a use is about
        s.answer(request.id, true, false, true);
        QVERIFY(turns.count() == 1 || turns.wait(10000));
        QVERIFY(turns.last().at(0).value<TurnResult>().ok);
        QCOMPARE(asks.count(), 1);   // the second use of it was not asked about
        QVERIFY(s.toolsAllowed());
        QCOMPARE(host.calls, QStringList{"change"});

        const QStringList args = read(dir.filePath("mcp-args")).split('\n');
        const QString config = args.value(args.indexOf("--mcp-config") + 1);
        QVERIFY(config.contains("\"type\":\"sdk\""));
        QVERIFY(config.contains("\"fake\""));
        QCOMPARE(args.value(args.indexOf("--allowedTools") + 1), QStringLiteral("mcp__fake__look"));
        // What the session wrote: the host's initialize, then the answers.
        QMap<QString, QJsonObject> answers;
        bool hostInit = false;
        for (const QString& line : read(dir.filePath("mcp-in")).split('\n', Qt::SkipEmptyParts)) {
            const QJsonObject m = QJsonDocument::fromJson(line.toUtf8()).object();
            if (m.value("type").toString() == "control_request"
                && m.value("request").toObject().value("sdkMcpServers").toArray().contains(QJsonValue("fake")))
                hostInit = true;
            if (m.value("type").toString() == "control_response") {
                const QJsonObject r = m.value("response").toObject();
                answers.insert(r.value("request_id").toString(), r.value("response").toObject());
            }
        }
        QVERIFY(hostInit);
        const QJsonObject init = answers.value("m1").value("mcp_response").toObject();
        QCOMPARE(init.value("id").toInt(-1), 0);
        QCOMPARE(init.value("result").toObject().value("serverInfo").toObject().value("name").toString(), QStringLiteral("fake"));
        QCOMPARE(init.value("result").toObject().value("instructions").toString(), QStringLiteral("Fake instructions."));
        QCOMPARE(init.value("result").toObject().value("protocolVersion").toString(), QStringLiteral("2025-11-25"));
        QVERIFY(answers.contains("m2"));
        const QJsonArray listed = answers.value("m3").value("mcp_response").toObject().value("result").toObject().value("tools").toArray();
        QCOMPARE(listed.size(), 2);
        QCOMPARE(answers.value("p1").value("behavior").toString(), QStringLiteral("allow"));
        QCOMPARE(answers.value("p2").value("behavior").toString(), QStringLiteral("allow"));
        const QJsonObject called = answers.value("m4").value("mcp_response").toObject();
        QCOMPARE(called.value("id").toInt(), 2);
        QCOMPARE(called.value("result").toObject().value("content").toArray().at(0).toObject().value("text").toString(),
                 QStringLiteral("changed x"));
        QCOMPARE(answers.value("p3").value("behavior").toString(), QStringLiteral("allow"));
        s.stop();

        // A new conversation asks again.
        s.reset();
        QVERIFY(!s.toolsAllowed());
    }

    // A conversation pinned to a document: the host is asked what its
    // tools are given for it (ToolHost::forDocument()) - in the question
    // put to the user, the tool's line and the call. Not pinned, the
    // arguments are Claude's.
    void aPinnedConversationsToolsAreGivenItsDocument()
    {
        skipWithoutShell();
        FakeHost host;
        Session s;
        s.setProgram(script("tooluser", kToolUser));
        s.setWorkingDirectory(fresh("pinwork"));
        s.setToolHost(&host);
        QCOMPARE(s.document(), QString());
        s.setDocument("/w/amp.sch");
        QSignalSpy asks(&s, &Session::permissionRequested);
        QSignalSpy tools(&s, &Session::toolStarted);
        QSignalSpy turns(&s, &Session::turnFinished);
        QVERIFY(s.send("Change x"));
        QVERIFY(asks.wait(10000));
        const auto request = asks.last().at(0).value<PermissionRequest>();
        QCOMPARE(request.subject, QStringLiteral("what: x in /w/amp.sch"));
        QCOMPARE(tools.last().at(2).toString(), QStringLiteral("what: x in /w/amp.sch"));
        s.answer(request.id, true, false, true);
        QVERIFY(turns.count() == 1 || turns.wait(10000));
        QCOMPARE(host.calls, QStringList{"change"});
        QCOMPARE(host.arguments.last().value("document").toString(), QStringLiteral("/w/amp.sch"));
        QCOMPARE(host.arguments.last().value("what").toString(), QStringLiteral("x"));
        s.stop();

        // Unpinned: as Claude gave them.
        s.reset();
        s.setDocument(QString());
        host.calls.clear();
        host.arguments.clear();
        QVERIFY(s.send("Change x"));
        QVERIFY(asks.wait(10000));
        QCOMPARE(asks.last().at(0).value<PermissionRequest>().subject, QStringLiteral("what: x"));
        s.answer(asks.last().at(0).value<PermissionRequest>().id, true, false, true);
        QVERIFY(turns.wait(10000));
        QCOMPARE(host.arguments.size(), 1);
        QVERIFY(!host.arguments.last().contains("document"));
        s.stop();
    }

    // The models to choose: what the program offers, named by what they
    // are (its descriptions: "Fable 5.1"), the default first; then the
    // newest of each family it does not offer, by their full names.
    void theModelsAreTheProgramsAndTheNewest()
    {
        const auto model = [](const char* value, const char* resolved, const QString& description, bool autoMode) {
            QJsonObject o{{"value", value}, {"resolvedModel", resolved}, {"displayName", "x"}, {"description", description}};
            if (autoMode) o.insert("supportsAutoMode", true);
            return o;
        };
        const QString dot = QString::fromUtf8(" \u00b7 ");
        const QJsonArray listed{model("default", "claude-opus-5[1m]", "Opus 5 with 1M context" + dot + "Best for everyday tasks", true),
                                model("claude-fable-5-1[1m]", "claude-fable-5-1", "Fable 5.1" + dot + "Most capable", true),
                                model("sonnet", "claude-sonnet-5", "Sonnet 5" + dot + "Efficient", true),
                                model("haiku", "claude-haiku-4-5-20251001", "Haiku 4.5" + dot + "Fastest", false),
                                model("opus", "claude-opus-5", "Opus 5" + dot + "Best for everyday tasks", true)};
        const QList<ModelChoice> choices = qucs_s::claude::modelChoices(listed);
        const auto find = [&choices](const QString& value) -> const ModelChoice* {
            for (const ModelChoice& c : choices)
                if (c.value == value) return &c;
            return nullptr;
        };
        QCOMPARE(choices.first().value, QString());   // the default: no --model
        QCOMPARE(choices.first().name, QStringLiteral("Default (Opus 5 with 1M context)"));
        QVERIFY(choices.first().listed);
        QCOMPARE(find("claude-fable-5-1[1m]")->name, QStringLiteral("Fable 5.1"));
        QCOMPARE(find("claude-fable-5-1[1m]")->description, QStringLiteral("Most capable"));
        QCOMPARE(find("opus")->name, QStringLiteral("Opus 5"));
        QVERIFY(find("sonnet")->autoMode);
        QVERIFY(!find("haiku")->autoMode);
        // The newest it does not offer: Opus 5.5, by its full name; not
        // Fable 5.1, Sonnet 5 or Haiku 4.5 again.
        QVERIFY(find("claude-opus-5-5") != nullptr);
        QCOMPARE(find("claude-opus-5-5")->name, QStringLiteral("Opus 5.5"));
        QVERIFY(!find("claude-opus-5-5")->listed);
        QVERIFY(find("claude-opus-5-5")->autoMode);
        QCOMPARE(choices.size(), listed.size() + 1);

        // A model the program was told of, which it calls a custom model.
        const QList<ModelChoice> custom = qucs_s::claude::modelChoices(
            {model("claude-opus-5-5", "claude-opus-5-5", "Custom model", true)});
        QCOMPARE(custom.at(1).name, QStringLiteral("Opus 5.5"));
        QCOMPARE(std::count_if(custom.cbegin(), custom.cend(), [](const ModelChoice& c) { return c.value == "claude-opus-5-5"; }), 1);

        // Nothing heard from the program: its default, and the newest.
        const QList<ModelChoice> none = qucs_s::claude::modelChoices({});
        QStringList names;
        for (const ModelChoice& c : none) names << c.name;
        QCOMPARE(names, (QStringList{"Default", "Opus 5.5", "Fable 5.1", "Sonnet 5", "Haiku 4.5"}));
        QCOMPARE(qucs_s::claude::modelName("claude-opus-5[1m]"), QStringLiteral("Opus 5"));
    }

    // The program is asked which models it offers - nothing goes to a
    // model - and it ends; the dock's menu is what it said, kept for the
    // next start. A program that does not answer changes nothing.
    void theProgramIsAskedForItsModels()
    {
        skipWithoutShell();
        QFile::remove(dir.filePath("lister-request"));
        ModelQuery query;
        QSignalSpy listed(&query, &ModelQuery::finished);
        query.start(script("lister", kLister), fresh("listwork"));
        QVERIFY(query.isRunning());
        QVERIFY(listed.wait(10000));
        QVERIFY(!query.isRunning());
        QCOMPARE(listed.last().at(0).toJsonArray().size(), 3);
        QVERIFY(read(dir.filePath("lister-request")).contains("\"subtype\":\"initialize\""));
        const QStringList args = read(dir.filePath("lister-args")).split('\n');
        QVERIFY(!args.contains("--model"));
        QVERIFY(!args.contains("--resume"));

        query.start(script("broken", kBroken), QString());
        QVERIFY(listed.wait(10000));
        QVERIFY(listed.last().at(0).toJsonArray().isEmpty());

        {
            ClaudeCodePanel panel;
            panel.setDefaultDirectory(fresh("listdock"));
            panel.session()->setProgram(script("lister", kLister));
            panel.show();
            QSignalSpy asked(panel.modelQuery(), &ModelQuery::finished);
            QVERIFY(asked.wait(10000));
            QStringList names;
            for (QAction* a : panel.modelActions()) names << a->text();
            QCOMPARE(names, (QStringList{"Default (Opus 5 with 1M context)", "Fable 5.1", "Haiku 4.5", "Opus 5.5",
                                         "Sonnet 5", "Other…"}));
        }
        // The next dock has the list at once.
        ClaudeCodePanel next;
        QVERIFY(std::any_of(next.modelActions().cbegin(), next.modelActions().cend(),
                            [](QAction* a) { return a->text() == "Fable 5.1"; }));
    }

    // Auto mode: Claude acts without asking and a safety check stops risky
    // actions. Not every model has it - the menu says so - and a program
    // that falls back to asking is reported.
    void autoModeIsOfferedWhereTheModelHasIt()
    {
        qucs_s::claude::Options options;
        options.permissionMode = "auto";
        const QStringList args = qucs_s::claude::arguments(options);
        QCOMPARE(args.at(args.indexOf("--permission-mode") + 1), QStringLiteral("auto"));

        ClaudeCodePanel panel;
        panel.setDefaultDirectory(fresh("autodock"));
        panel.session()->setProgram("claude");   // (not run)
        const auto action = [](const QList<QAction*>& actions, const QString& data) -> QAction* {
            for (QAction* a : actions)
                if (a->data().toString() == data) return a;
            return nullptr;
        };
        QAction* autoMode = action(panel.permissionActions(), "auto");
        QVERIFY(autoMode != nullptr);
        QCOMPARE(autoMode->text(), QStringLiteral("Auto"));
        autoMode->trigger();
        QVERIFY(autoMode->isChecked());
        QCOMPARE(panel.session()->permissionMode(), QStringLiteral("auto"));
        QVERIFY(panel.modelLabel()->text().endsWith("auto"));
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("safety check"));

        // Haiku has no auto mode (the alias the program offers, or the
        // full name when it has not been asked).
        QAction* haiku = nullptr;
        for (QAction* a : panel.modelActions())
            if (a->text() == "Haiku 4.5") haiku = a;
        QVERIFY(haiku != nullptr);
        haiku->trigger();
        QVERIFY(panel.session()->model().contains("haiku"));
        QVERIFY(!autoMode->isEnabled());
        QVERIFY(autoMode->toolTip().contains("Haiku 4.5"));
        QVERIFY(!panel.modelLabel()->text().contains("auto"));   // it will ask
        action(panel.modelActions(), "claude-opus-5-5")->trigger();
        QVERIFY(autoMode->isEnabled());
        QVERIFY(panel.modelLabel()->text().startsWith("Opus 5.5"));

        // The program says in which mode it works: asking, with a model
        // without auto mode - once, not at each turn.
        Session s;
        s.setProgram("claude");
        s.setPermissionMode("auto");
        QSignalSpy notices(&s, &Session::notice);
        s.handleLine(R"({"type":"system","subtype":"init","session_id":"a","model":"claude-haiku-4-5-20251001","permissionMode":"default"})");
        QCOMPARE(notices.count(), 1);
        QVERIFY(notices.last().at(0).toString().contains("Auto mode is not available with Haiku 4.5"));
        QCOMPARE(s.permissionModeInUse(), QStringLiteral("default"));
        s.handleLine(R"({"type":"system","subtype":"init","session_id":"a","model":"claude-haiku-4-5-20251001","permissionMode":"default"})");
        QCOMPARE(notices.count(), 1);
        Session fine;
        fine.setProgram("claude");
        fine.setPermissionMode("auto");
        QSignalSpy quiet(&fine, &Session::notice);
        fine.handleLine(R"({"type":"system","subtype":"init","session_id":"b","model":"claude-opus-5-5","permissionMode":"auto"})");
        QCOMPARE(quiet.count(), 0);

        // As it was, for the tests after.
        action(panel.permissionActions(), "")->trigger();
        action(panel.modelActions(), "")->trigger();
        QCOMPARE(panel.session()->model(), QString());
        QVERIFY(!panel.modelLabel()->text().contains("auto"));
    }

    // The tools Claude uses in a row are one line, folded: what they did
    // in words ("Ran 2 commands, read amp.sch"). Opened, a line each;
    // each opens on the whole command and what it gave. One tool alone
    // is its own line, folded the same way.
    void toolsFoldIntoALine()
    {
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(fresh("toolwork"));
        panel.resize(420, 700);
        Session* s = panel.session();
        s->setProgram("claude");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"text","text":"Looking."}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"ls -la","description":"List the files"}}]}})");
        s->handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"amp.sch\nfilter.sch","is_error":false}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t2","name":"Bash","input":{"command":"grep -c R amp.sch"}}]}})");
        s->handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t2","content":"7","is_error":false}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t3","name":"Read","input":{"file_path":"/w/amp.sch"}}]}})");
        s->handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t3","content":"<Qucs Schematic>","is_error":false}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"text","text":"Found them."}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t4","name":"Bash","input":{"command":"ngspice -b amp.cir"}}]}})");
        s->handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t4","content":"no such file","is_error":true}]}})");
        panel.renderNow();
        QString text = panel.transcriptText();
        QVERIFY(text.contains("Ran 2 commands, read amp.sch"));
        QVERIFY(!text.contains("ls -la"));
        QVERIFY(!text.contains("grep -c"));
        // One alone: its line, and why it failed; not its output.
        QVERIFY(text.contains("ngspice -b amp.cir"));
        QVERIFY(text.contains("no such file"));
        QVERIFY(text.indexOf("Found them.") < text.indexOf("ngspice"));

        // Opened with a click: a line each, folded.
        emit panel.transcript()->anchorClicked(QUrl("toggle:group:t1"));
        QVERIFY(panel.isExpanded("group:t1"));
        text = panel.transcriptText();
        QVERIFY(text.contains("ls -la"));
        QVERIFY(text.contains("grep -c R amp.sch"));
        QVERIFY(!text.contains("filter.sch"));
        QVERIFY(!text.contains("List the files"));
        // A command opened: the whole of it, its description, what it gave.
        panel.toggle("tool:t1");
        text = panel.transcriptText();
        QVERIFY(text.contains("# List the files"));
        QVERIFY(text.contains("filter.sch"));
        panel.toggle("tool:t1");
        panel.toggle("group:t1");
        QVERIFY(!panel.transcriptText().contains("ls -la"));
        // Folded lines are links that say so.
        bool linked = false;
        QTextDocument* doc = panel.transcript()->document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().anchorHref() == "toggle:group:t1") linked = true;
        QVERIFY(linked);
    }

    // A conversation exported whole - as Markdown (the replies as Claude
    // wrote them, each tool with its input and what it gave), as plain text
    // (the Markdown read, the math as TeX) and as a PDF (drawn as in the
    // dock, on pages) - from ⋯ › Export Conversation, which is there once
    // there is something to export. The dock is as it was after.
    void aConversationIsExported()
    {
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(fresh("exportwork"));
        panel.resize(420, 700);
        Session* s = panel.session();
        QMenu* exports = panel.findChild<QMenu*>("claudeExport");
        QVERIFY(exports != nullptr);
        auto* menu = qobject_cast<QMenu*>(exports->parent());
        QVERIFY(menu != nullptr);
        emit menu->aboutToShow();
        QVERIFY(!exports->menuAction()->isEnabled());
        QCOMPARE(exports->actions().size(), 5);   // PDF, Markdown, text; Include Tool Details
        QVERIFY(ClaudeCodePanel::exportsToolDetails());   // (at first)

        s->setProgram(dir.filePath("no-claude-here"));   // (the prompt is there; the turn fails)
        panel.composer()->setPlainText("What is the cut-off frequency\nof R1 and C1?");
        panel.sendComposer();
        s->setProgram("claude");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"ls -la","description":"List the files"}}]}})");
        s->handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"amp.sch\nfilter.sch","is_error":false}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t2","name":"Bash","input":{"command":"ngspice -b amp.cir"}}]}})");
        s->handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t2","content":"no such file","is_error":true}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"text","text":"The **cut-off** is $f_c = \\frac{1}{2\\pi RC}$:\n\n- R1 = 1 k\n- C1 = 1 u\n\n| R | C |\n|---|---|\n| 1k | 1u |\n\n```\n.ac dec 10 1 1meg\n```"}]}})");
        panel.addNote("A note from Qucs-S.");
        panel.renderNow();
        const QString before = panel.transcriptText();
        emit menu->aboutToShow();
        QVERIFY(exports->menuAction()->isEnabled());

        const QString md = panel.conversationMarkdown();
        QVERIFY2(md.startsWith("# What is the cut-off frequency"), qPrintable(md.left(80)));
        QVERIFY(md.contains("- **Folder:** "));
        QVERIFY(md.contains("### You\n\n> What is the cut-off frequency\n> of R1 and C1?"));
        QVERIFY(md.contains("### Claude"));
        QVERIFY(md.contains("- ✓ **Bash** `ls -la`"));
        QVERIFY(md.contains("  ```\n  ls -la\n  # List the files\n  ```") || md.contains("  ```\n  # List the files\n  ls -la\n  ```"));
        QVERIFY(md.contains("  amp.sch\n  filter.sch"));
        QVERIFY(md.contains("- ✕ **Bash** `ngspice -b amp.cir`"));
        QVERIFY(md.contains("Failed: no such file"));
        QVERIFY(md.contains("The **cut-off** is $f_c = \\frac{1}{2\\pi RC}$"));
        QVERIFY(md.contains("| 1k | 1u |"));
        QVERIFY(md.contains("*A note from Qucs-S.*"));
        QVERIFY(md.indexOf("ls -la") < md.indexOf("The **cut-off**"));

        const QString text = panel.conversationText();
        QVERIFY(text.startsWith("What is the cut-off frequency"));
        QVERIFY(text.contains("You:\nWhat is the cut-off frequency\nof R1 and C1?"));
        QVERIFY(text.contains("Claude:"));
        QVERIFY2(text.contains("The cut-off is $f_c = \\frac{1}{2\\pi RC}$:"), qPrintable(text));
        QVERIFY(!text.contains("**"));
        QVERIFY2(text.contains("• R1 = 1 k\n• C1 = 1 u"), qPrintable(text));
        QVERIFY(text.contains("1k | 1u"));
        QVERIFY(text.contains("    .ac dec 10 1 1meg"));
        QVERIFY(text.contains("  ✓ Bash   ls -la"));
        QVERIFY(text.contains("      │ amp.sch"));
        QVERIFY(text.contains("Failed: no such file"));
        QVERIFY(text.contains("(A note from Qucs-S.)"));

        QString error;
        const QString mdFile = dir.filePath("exported.md");
        QVERIFY(panel.exportConversation(mdFile, ClaudeCodePanel::ExportFormat::Markdown, &error));
        QCOMPARE(read(mdFile), md);
        const QString txtFile = dir.filePath("exported.txt");
        QVERIFY(panel.exportConversation(txtFile, ClaudeCodePanel::ExportFormat::Text, &error));
        QCOMPARE(read(txtFile), text);
        const QString pdfFile = dir.filePath("exported.pdf");
        QVERIFY2(panel.exportConversation(pdfFile, ClaudeCodePanel::ExportFormat::Pdf, &error), qPrintable(error));
        QFile pdf(pdfFile);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        const QByteArray bytes = pdf.readAll();
        QVERIFY(bytes.startsWith("%PDF-"));
        QVERIFY(bytes.size() > 4000);
        QCOMPARE(bytes.count("/Type /Page\n") + bytes.count("/Type /Page "), qsizetype(1));
        QVERIFY(!panel.exportConversation(dir.filePath("no/such/folder/x.md"), ClaudeCodePanel::ExportFormat::Markdown, &error));
        QVERIFY(!error.isEmpty());
        // The dock draws what it drew: its tools folded, its links there.
        panel.renderNow();
        QCOMPARE(panel.transcriptText(), before);
        QVERIFY(!panel.isExpanded("group:t1"));

        // A long one: pages enough for it.
        for (int i = 0; i < 12; ++i)
            s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"text","text":"A paragraph that goes on for a while, so that there is enough of it to fill more than one page of paper when it is written out a good many times over, with $x^2$ in it.\n\nAnd another one after it, just as long, to take up the room that a page has on it."}]}})");
        QVERIFY(panel.exportConversation(pdfFile, ClaudeCodePanel::ExportFormat::Pdf, &error));
        QVERIFY(pdf.seek(0));
        const QByteArray longer = pdf.readAll();
        QVERIFY2(longer.count("/Type /Page\n") + longer.count("/Type /Page ") > 1, "one page only");
    }

    // Export Conversation > Include Tool Details, off: what the boxes that
    // fold hold is left out - each tool's input and what it gave - and a
    // row of tools is the one line that sums it up, as the dock shows it
    // folded; a tool alone keeps its line (and why it failed). The chat is
    // all there. The choice is kept, and every conversation's menu shows it.
    void anExportLeavesTheToolDetailsOutWhenAsked()
    {
        const auto restore = qScopeGuard([] { ClaudeCodePanel::setExportsToolDetails(true); });
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(fresh("exportbrief"));
        panel.resize(420, 700);
        Session* s = panel.session();
        s->setProgram(dir.filePath("no-claude-here"));
        panel.composer()->setPlainText("Check the filter");
        panel.sendComposer();
        s->setProgram("claude");
        // A row of three tools, one failing; a reply; a tool alone, failing; a reply.
        QString output;
        for (int i = 0; i < 300; ++i) output += QStringLiteral("line %1 of what the netlist said\\n").arg(i);
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"r1","name":"Bash","input":{"command":"ls -la"}}]}})");
        s->handleLine(QStringLiteral(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"r1","content":"%1","is_error":false}]}})").arg(output).toUtf8());
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"r2","name":"Read","input":{"file_path":"/w/filter.sch"}}]}})");
        s->handleLine(QStringLiteral(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"r2","content":"%1","is_error":false}]}})").arg(output).toUtf8());
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"r3","name":"Bash","input":{"command":"ngspice -b filter.cir"}}]}})");
        s->handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"r3","content":"no such file","is_error":true}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"text","text":"The filter is a **second-order** low-pass."}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"a1","name":"Bash","input":{"command":"cat filter.cir"}}]}})");
        s->handleLine(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"a1","content":"cannot open","is_error":true}]}})");
        s->handleLine(R"({"type":"assistant","message":{"content":[{"type":"text","text":"Its cut-off is 1 kHz."}]}})");
        panel.renderNow();
        const QString dock = panel.transcriptText();

        // The details, as before.
        const QString fullMd = panel.conversationMarkdown();
        QVERIFY(fullMd.contains("line 29 of what the netlist said"));   // (the dock keeps 30)
        QVERIFY(fullMd.contains("- ✓ **Bash** `ls -la`"));
        QString error;
        const QString fullPdf = dir.filePath("full.pdf");
        QVERIFY2(panel.exportConversation(fullPdf, ClaudeCodePanel::ExportFormat::Pdf, &error), qPrintable(error));

        // Off, from the menu.
        QMenu* exports = panel.findChild<QMenu*>("claudeExport");
        QAction* details = nullptr;
        for (QAction* a : exports->actions())
            if (a->objectName() == QLatin1String("claudeExportToolDetails")) details = a;
        QVERIFY(details != nullptr);
        QVERIFY(details->isCheckable());
        QVERIFY(details->isChecked());
        details->trigger();
        QVERIFY(!details->isChecked());
        QVERIFY(!ClaudeCodePanel::exportsToolDetails());

        const QString md = panel.conversationMarkdown();
        QVERIFY2(md.contains("\n- ✕ **Ran 2 commands, read filter.sch** · 1 failed\n"), qPrintable(md));
        QVERIFY(!md.contains("**Read**"));
        QVERIFY(!md.contains("line 0 of what"));
        QVERIFY(!md.contains("```"));
        QVERIFY(!md.contains("no such file"));
        QVERIFY(md.contains("- ✕ **Bash** `cat filter.cir`"));   // alone: its line
        QVERIFY(md.contains("Failed: cannot open"));
        QVERIFY(md.contains("### You\n\n> Check the filter"));
        QVERIFY(md.contains("The filter is a **second-order** low-pass."));
        QVERIFY(md.contains("Its cut-off is 1 kHz."));
        QVERIFY(md.indexOf("Ran 2 commands") < md.indexOf("second-order"));
        QVERIFY(md.indexOf("second-order") < md.indexOf("cat filter.cir"));
        QVERIFY(md.indexOf("cat filter.cir") < md.indexOf("1 kHz"));

        const QString text = panel.conversationText();
        QVERIFY2(text.contains("\n  ✕ Ran 2 commands, read filter.sch  ·  1 failed\n"), qPrintable(text));
        QVERIFY(!text.contains("│"));
        QVERIFY(!text.contains("line 0 of what"));
        QVERIFY(!text.contains("no such file"));
        QVERIFY(text.contains("  ✕ Bash   cat filter.cir\n      Failed: cannot open"));
        QVERIFY(text.contains("The filter is a second-order low-pass."));
        QVERIFY(text.contains("Its cut-off is 1 kHz."));

        // The files are what these say; the PDF is the shorter by the
        // output left out (60 lines of it: a page and more).
        const QString mdFile = dir.filePath("brief.md");
        QVERIFY(panel.exportConversation(mdFile, ClaudeCodePanel::ExportFormat::Markdown, &error));
        QCOMPARE(read(mdFile), md);
        const QString txtFile = dir.filePath("brief.txt");
        QVERIFY(panel.exportConversation(txtFile, ClaudeCodePanel::ExportFormat::Text, &error));
        QCOMPARE(read(txtFile), text);
        const QString briefPdf = dir.filePath("brief.pdf");
        QVERIFY2(panel.exportConversation(briefPdf, ClaudeCodePanel::ExportFormat::Pdf, &error), qPrintable(error));
        const auto pages = [](const QString& path) {
            QFile f(path);
            const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            return bytes.count("/Type /Page\n") + bytes.count("/Type /Page ");
        };
        QCOMPARE(pages(briefPdf), qsizetype(1));
        QVERIFY2(pages(fullPdf) > 1, qPrintable(QString::number(pages(fullPdf))));

        // The dock is as it was: its tools as the user left them.
        panel.renderNow();
        QCOMPARE(panel.transcriptText(), dock);
        panel.toggle("group:r1");
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("ls -la"));

        // Kept, and shown in another conversation's menu.
        ClaudeCodePanel other;
        QMenu* otherExports = other.findChild<QMenu*>("claudeExport");
        QAction* otherDetails = nullptr;
        for (QAction* a : otherExports->actions())
            if (a->objectName() == QLatin1String("claudeExportToolDetails")) otherDetails = a;
        QVERIFY(otherDetails != nullptr);
        QVERIFY(!otherDetails->isChecked());
        details->trigger();   // on again, in the first
        QVERIFY(ClaudeCodePanel::exportsToolDetails());
        emit qobject_cast<QMenu*>(otherExports->parent())->aboutToShow();
        QVERIFY(otherDetails->isChecked());
        QVERIFY(panel.conversationMarkdown().contains("line 29 of what the netlist said"));
    }

    // TeX math: found in Markdown (not in code, not money), typeset (a
    // fraction in display style is taller than in text), and set in the
    // conversation as images with the TeX in their tool tip.
    void mathIsTypeset()
    {
        using qucs_s::math::findMath;
        auto spans = findMath("The cutoff is $f_c = \\frac{1}{2\\pi RC}$, and\n$$H(s) = \\frac{1}{1+sRC}$$\nwhere.");
        QCOMPARE(spans.size(), 2);
        QCOMPARE(spans.at(0).tex, QStringLiteral("f_c = \\frac{1}{2\\pi RC}"));
        QVERIFY(!spans.at(0).display);
        QVERIFY(spans.at(1).display);
        QCOMPARE(findMath("It costs $5 and $10 a piece.").size(), 0);
        QCOMPARE(findMath("A dollar: \\$x$ is not math").size(), 0);
        QCOMPARE(findMath("Code `$x$` and\n```\n$$y$$\n```\nbut $z$").size(), 1);
        QCOMPARE(findMath("Inline \\(a^2\\) and display \\[b^2\\]").size(), 2);
        QCOMPARE(findMath("$ x $").size(), 0);

        QFont font = QApplication::font();
        const auto text = qucs_s::math::typeset("\\frac{a}{b}", font, Qt::black, false, 1.0);
        const auto display = qucs_s::math::typeset("\\frac{a}{b}", font, Qt::black, true, 1.0);
        QVERIFY(text.ok && display.ok);
        QVERIFY(!text.image.isNull());
        QVERIFY(display.ascent + display.descent > text.ascent + text.descent);
        QVERIFY(qucs_s::math::typeset("\\sum_{n=0}^{\\infty} x^n \\begin{pmatrix} 1 & 2 \\\\ 3 & 4 \\end{pmatrix}", font, Qt::black, true).ok);
        QVERIFY(!qucs_s::math::typeset("\\nosuchcommand x", font, Qt::black, false).ok);   // shown as written
        QVERIFY(!qucs_s::math::typeset("\\frac{a}{", font, Qt::black, false).image.isNull());
        // Its middle on the axis: an image as tall above as below the
        // line's middle.
        qreal height = 0.0;
        const QImage centred = qucs_s::math::centredOnAxis(display, font, &height);
        QVERIFY(height >= display.ascent + display.descent);
        QVERIFY(!centred.isNull());

        ClaudeCodePanel panel;
        panel.setDefaultDirectory(fresh("mathwork"));
        panel.resize(420, 700);
        panel.session()->setProgram("claude");
        panel.session()->handleLine(QJsonDocument(QJsonObject{
            {"type", "assistant"},
            {"message", QJsonObject{{"content", QJsonArray{QJsonObject{
                {"type", "text"},
                {"text", "The cutoff is $f_c = \\frac{1}{2\\pi RC}$.\n\n$$Z = \\sqrt{\\frac{L}{C}}$$\n\n- in a list: $\\omega_0$\n\n`$not$ math`"}}}}}}}).toJson(QJsonDocument::Compact));
        panel.renderNow();
        const QStringList typeset = mathIn(panel.transcript()->document());
        QCOMPARE(typeset, (QStringList{"f_c = \\frac{1}{2\\pi RC}", "Z = \\sqrt{\\frac{L}{C}}", "\\omega_0"}));
        const QString shown = panel.transcriptText();
        QVERIFY(!shown.contains("\\frac"));
        QVERIFY(shown.contains("The cutoff is"));
        QVERIFY(shown.contains("$not$ math"));
        // Display math on its own lines is centred.
        bool centredBlock = false;
        QTextDocument* doc = panel.transcript()->document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            if (b.text().trimmed() == QString(QChar::ObjectReplacementCharacter) && b.blockFormat().alignment() & Qt::AlignHCenter)
                centredBlock = true;
        QVERIFY(centredBlock);
    }

    // Whatever Claude writes between dollars - TeX cut short, braces out of
    // balance, environments not closed, commands nested deep - is set as
    // far as it goes, and nothing breaks.
    void mathSurvivesAnything()
    {
        const char* const tokens[] = {
            "\\frac", "{", "}", "^", "_", "\\left(", "\\right)", "\\left.", "\\right|", "\\begin{pmatrix}",
            "\\end{pmatrix}", "\\begin{aligned}", "\\end{aligned}", "\\begin{cases}", "&", "\\\\", "\\sqrt",
            "[", "]", "x", "1", "\\alpha", "\\sum", "\\int", "\\text{", "\\over", "'", "\\not", "\\hat",
            "\\color{red}", "\\displaystyle", "\\big(", "\\middle|", "\\operatorname*{", "\\SI{", "\\mathbb{",
            "\\", "%", "~", "\\,", "\\tag{", "\\xrightarrow[", "\\underbrace", "\\overset", "\\binom",
            "\\begin{array}{", "\\hspace{", "\\limits", "\\nosuch", "=", "-", "+", "(", ")", "|", "\\{", "\\}",
            " ", "\\substack{", "\\phantom", "\\boxed", "\\mathrm{", "\\end{cases}", "\\right.", "\\;",
        };
        const QFont font = QApplication::font();
        std::mt19937 rng(20260925);
        for (int round = 0; round < 1500; ++round) {
            QString tex;
            const int n = 1 + int(rng() % 40);
            for (int k = 0; k < n; ++k) tex += QString::fromUtf8(tokens[rng() % std::size(tokens)]);
            const auto t = qucs_s::math::typeset(tex, font, Qt::black, round % 2 == 0, 2.0);
            QVERIFY2(!t.image.isNull(), qPrintable(tex));
            QVERIFY2(std::isfinite(t.width) && std::isfinite(t.ascent) && std::isfinite(t.descent), qPrintable(tex));
            qreal height = 0.0;
            QVERIFY(!qucs_s::math::centredOnAxis(t, font, &height).isNull());
        }
        // Deep: nested beyond reason, stopped, not overflowing the stack.
        QVERIFY(!qucs_s::math::typeset(QString("\\not").repeated(5000) + "=", font, Qt::black, false).ok);
        QVERIFY(!qucs_s::math::typeset(QString("{").repeated(5000), font, Qt::black, false).ok);
        QVERIFY(!qucs_s::math::typeset(QString("\\frac{").repeated(3000), font, Qt::black, true).ok);
        // And ordinary things are ordinary: 1.59 is a number, not 1. 59.
        const auto number = qucs_s::math::typeset("1.59", font, Qt::black, false);
        const auto spaced = qucs_s::math::typeset("1,59", font, Qt::black, false);
        QVERIFY(number.width < spaced.width);
    }

    // Conversations in tabs: New opens one beside the first, which keeps
    // its own; the tab says what it is about and how it stands; one that
    // waits for permission behind another marks its tab and is the one
    // the status bar leads to; closing a tab ends its conversation, and
    // the last one leaves a new one.
    void conversationsHaveTabsOfTheirOwn()
    {
        ClaudeCodeTabs tabs;
        const QString work = fresh("tabwork");
        tabs.setDefaultDirectory(work);
        tabs.resize(440, 700);
        tabs.show();
        QCOMPARE(tabs.count(), 1);
        ClaudeCodePanel* first = tabs.current();
        QCOMPARE(first->workingDirectory(), work);
        QCOMPARE(tabs.tabWidget()->tabText(0), QStringLiteral("New conversation"));
        // (Another folder is a new conversation: chosen first.)
        const QString elsewhere = fresh("tabwork2");
        first->setWorkingDirectory(elsewhere);
        first->session()->setProgram(dir.filePath("no-such-claude"));   // (it fails to start: the prompt stays)
        first->composer()->setPlainText("Explain the amplifier");
        first->sendComposer();
        QCOMPARE(tabs.tabWidget()->tabText(0), QStringLiteral("Explain the amplifier"));

        // New, in the header: a second tab, in front, in the same folder
        // as the first when it was chosen there.
        first->newButton()->click();
        QCOMPARE(tabs.count(), 2);
        ClaudeCodePanel* second = tabs.current();
        QVERIFY(second != first);
        QCOMPARE(second->workingDirectory(), elsewhere);
        QVERIFY(second->session() != first->session());
        first->renderNow();
        QVERIFY(first->transcriptText().contains("Explain the amplifier"));
        tabs.newTabButton()->click();
        QCOMPARE(tabs.count(), 3);
        tabs.closeConversation(tabs.current(), false);
        QCOMPARE(tabs.count(), 2);

        // A question behind the tab in front: the tab is marked, the one in
        // front stays; the status bar's choice is the one waiting.
        tabs.showConversation(first);
        second->session()->setProgram("claude");
        second->session()->handleLine(R"({"type":"control_request","request_id":"r1","request":{"subtype":"can_use_tool","tool_name":"Bash","input":{"command":"ls"}}})");
        QCOMPARE(tabs.current(), first);
        QCOMPARE(tabs.needingAttention(), second);
        QCOMPARE(tabs.mostUrgent(), second);
        QVERIFY(tabs.tabWidget()->tabToolTip(1).contains("needs you"));
        // In front, it asks at once.
        second->session()->reset();
        QCOMPARE(tabs.needingAttention(), nullptr);
        tabs.showConversation(second);
        second->session()->handleLine(R"({"type":"control_request","request_id":"r2","request":{"subtype":"can_use_tool","tool_name":"Bash","input":{"command":"ls"}}})");
        QVERIFY(second->permissionCard()->isVisibleTo(second));
        second->session()->reset();

        // A note about files goes to the conversation that changed them.
        connect(&tabs, &ClaudeCodeTabs::filesChanged, &tabs, [&tabs] { tabs.addNote("reloaded here"); });
        tabs.showConversation(second);
        emit first->filesChanged({work + "/a.sch"});
        first->renderNow();
        second->renderNow();
        QVERIFY(first->transcriptText().contains("reloaded here"));
        QVERIFY(!second->transcriptText().contains("reloaded here"));

        // Closed: the last one leaves a new one.
        QVERIFY(tabs.closeConversation(first, false));
        QCOMPARE(tabs.count(), 1);
        QVERIFY(tabs.closeConversation(tabs.current(), false));
        QCOMPARE(tabs.count(), 1);
        QCOMPARE(tabs.tabWidget()->tabText(0), QStringLiteral("New conversation"));
        QCOMPARE(tabs.current()->workingDirectory(), work);
    }

    // A tab renamed: Rename in its menu (a right click) or a double click
    // puts an editor over it; Enter keeps the name, Esc leaves it, a click
    // elsewhere keeps it, empty gives the first prompt back. The name is
    // the conversation's everywhere (its tab, an export) until New.
    void aConversationIsRenamedInItsTab()
    {
        ClaudeCodeTabs tabs;
        tabs.setDefaultDirectory(fresh("renamework"));
        tabs.resize(440, 700);
        tabs.show();
        tabs.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&tabs));
        QTabBar* bar = tabs.tabWidget()->tabBar();
        ClaudeCodePanel* first = tabs.current();
        first->session()->setProgram(dir.filePath("no-such-claude"));
        first->composer()->setPlainText("Explain the amplifier");
        first->sendComposer();
        QCOMPARE(tabs.tabWidget()->tabText(0), QStringLiteral("Explain the amplifier"));

        // The menu of a right click on the tab.
        QStringList items;
        QTimer::singleShot(50, &tabs, [&items] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu == nullptr) return;
            for (QAction* a : menu->actions())
                if (!a->isSeparator()) items << a->text() + (a->isEnabled() ? QString() : QStringLiteral(" (off)"));
            menu->findChild<QAction*>(QStringLiteral("claudeRenameTab"))->trigger();
            menu->close();
        });
        const QPoint at = bar->tabRect(0).center();
        QContextMenuEvent right(QContextMenuEvent::Mouse, at, bar->mapToGlobal(at));
        QApplication::sendEvent(bar, &right);
        QCOMPARE(items, QStringList({"Rename…", "Reset Name (off)", "Close Conversation"}));
        QTRY_VERIFY(tabs.renameEditor() != nullptr);
        QLineEdit* editor = tabs.renameEditor();
        QVERIFY(editor->isVisible());
        QCOMPARE(editor->text(), QStringLiteral("Explain the amplifier"));
        QCOMPARE(editor->selectedText(), editor->text());
        QVERIFY(bar->tabRect(0).intersects(editor->geometry()));
        QVERIFY(bar->rect().contains(editor->geometry()));
        // (Offscreen, the menu leaves no window active; a desktop's stays.)
        tabs.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&tabs));
        QTRY_VERIFY(editor->hasFocus());

        // Enter keeps it; an & is an &, not a shortcut.
        QSignalSpy changes(&tabs, &ClaudeCodeTabs::stateChanged);
        QTest::keyClicks(editor, "  Amplifier   & bias  ");
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(tabs.renameEditor(), nullptr);
        QCOMPARE(first->name(), QStringLiteral("Amplifier & bias"));
        QCOMPARE(first->title(), QStringLiteral("Amplifier & bias"));
        QCOMPARE(tabs.tabWidget()->tabText(0), QStringLiteral("Amplifier && bias"));
        QVERIFY(tabs.tabWidget()->tabToolTip(0).startsWith("Amplifier & bias"));
        QVERIFY(first->conversationMarkdown().startsWith("# Amplifier & bias"));
        QVERIFY(changes.count() > 0);
        QTRY_VERIFY(first->composer()->hasFocus());
        // Another prompt does not rename it.
        first->composer()->setPlainText("And the gain?");
        first->sendComposer();
        QCOMPARE(first->title(), QStringLiteral("Amplifier & bias"));

        // Esc leaves it as it was.
        tabs.renameConversation(first);
        QCOMPARE(tabs.renameEditor()->text(), QStringLiteral("Amplifier & bias"));
        QTest::keyClicks(tabs.renameEditor(), "Forgotten");
        QTest::keyClick(tabs.renameEditor(), Qt::Key_Escape);
        QCOMPARE(tabs.renameEditor(), nullptr);
        QCOMPARE(first->name(), QStringLiteral("Amplifier & bias"));

        // Reset Name, in the menu now: the first prompt names it again.
        {
            std::unique_ptr<QMenu> menu(tabs.tabMenu(0));
            QAction* reset = menu->findChild<QAction*>(QStringLiteral("claudeResetTabName"));
            QVERIFY(reset->isEnabled());
            reset->trigger();
        }
        QCOMPARE(first->name(), QString());
        QCOMPARE(tabs.tabWidget()->tabText(0), QStringLiteral("Explain the amplifier"));
        // Kept as it was shown, it still follows the first prompt; emptied,
        // too.
        tabs.renameConversation(first);
        QTest::keyClick(tabs.renameEditor(), Qt::Key_Return);
        QCOMPARE(first->name(), QString());
        first->setName("Named");
        tabs.renameConversation(first);
        tabs.renameEditor()->clear();
        QTest::keyClick(tabs.renameEditor(), Qt::Key_Return);
        QCOMPARE(first->name(), QString());
        QCOMPARE(first->title(), QStringLiteral("Explain the amplifier"));

        // A double click on a tab; a click elsewhere keeps what was written.
        ClaudeCodePanel* second = tabs.newConversation();
        QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(1).center());
        QVERIFY(tabs.renameEditor() != nullptr);
        QVERIFY(bar->tabRect(1).intersects(tabs.renameEditor()->geometry()));
        tabs.renameEditor()->setText("Filter design");
        tabs.renameEditor()->setFocus();
        second->composer()->setFocus();
        QTRY_COMPARE(second->name(), QStringLiteral("Filter design"));
        QCOMPARE(tabs.renameEditor(), nullptr);
        // Another conversation brought forward keeps it too.
        tabs.renameConversation(second);
        tabs.renameEditor()->setText("Filter design, 2nd order");
        tabs.showConversation(first);
        QCOMPARE(second->name(), QStringLiteral("Filter design, 2nd order"));
        QCOMPARE(tabs.renameEditor(), nullptr);

        // A tab closed while it is renamed; the editor goes with it.
        tabs.renameConversation(second);
        QPointer<QLineEdit> gone = tabs.renameEditor();
        QVERIFY(tabs.closeConversation(second, false));
        QCOMPARE(tabs.renameEditor(), nullptr);
        QTRY_VERIFY(gone == nullptr);
        QCOMPARE(first->name(), QString());

        // New begins a conversation without its name.
        first->setName("Old");
        first->newConversation();
        QCOMPARE(first->name(), QString());
        QCOMPARE(first->title(), QStringLiteral("New conversation"));
    }

    // Pinning, the user's choice: not pinned at first (the prompts name
    // the document in front, as always); the pin by the composer pins the
    // schematic in front, ⋯ > Pin to a Schematic any open one; pinned, the
    // prompts name it (the document chip or not) and the session's tools
    // are given it; one closed is said to be; Unpin, and New, unpin; the
    // tab says it; a Save As moves it.
    void aConversationIsPinnedToASchematic()
    {
        const QString work = fresh("pins");
        const QString a = work + "/amp.sch", b = work + "/filter.sch", t = work + "/notes.txt";
        for (const QString& f : {a, b, t}) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QString front = a;
        QStringList open{a, b};
        ClaudeCodeTabs tabs;
        tabs.setDefaultDirectory(work);
        tabs.setDocumentProvider([&front] { return front; });
        tabs.setSchematicsProvider([&open] { return open; });
        tabs.resize(440, 700);
        tabs.show();
        ClaudeCodePanel* panel = tabs.current();
        QToolButton* pin = panel->pinButton();
        QVERIFY(panel->pinnedDocument().isEmpty());
        QVERIFY(panel->session()->document().isEmpty());
        QVERIFY(pin->isEnabled());
        QVERIFY(!pin->isChecked());
        QVERIFY(pin->text().isEmpty());
        QVERIFY(panel->attachButton()->isVisibleTo(panel));
        QCOMPARE(panel->attachButton()->text(), QStringLiteral("amp.sch"));
        // A text document in front: nothing to pin.
        front = t;
        tabs.refreshDocument();
        QVERIFY(!pin->isEnabled());
        QCOMPARE(panel->pinnableDocument(), QString());

        // Pinned from the composer.
        front = a;
        tabs.refreshDocument();
        QSignalSpy pinned(panel, &ClaudeCodePanel::pinChanged);
        pin->click();
        QCOMPARE(pinned.count(), 1);
        QVERIFY(panel->isPinnedTo(a));
        QCOMPARE(panel->session()->document(), panel->pinnedDocument());
        QVERIFY(pin->isChecked());
        QCOMPARE(pin->text(), QStringLiteral("amp.sch"));
        QVERIFY(pin->property("pinned").toBool());
        QVERIFY(!panel->attachButton()->isVisibleTo(panel));
        QVERIFY(tabs.tabWidget()->tabToolTip(0).contains("Pinned to"));
        QVERIFY(tabs.tabWidget()->tabToolTip(0).contains("amp.sch"));
        // The document in front changes; the pin stays.
        front = b;
        tabs.refreshDocument();
        QVERIFY(panel->isPinnedTo(a));
        QCOMPARE(pin->text(), QStringLiteral("amp.sch"));

        // The menu: every open schematic, the pinned one checked; another
        // chosen.
        QMenu* menu = panel->pinMenu();
        emit menu->aboutToShow();
        QStringList items;
        QAction* other = nullptr;
        QAction* unpin = nullptr;
        for (QAction* x : menu->actions()) {
            if (x->isSeparator()) continue;
            items << x->text() + (x->isChecked() ? "*" : "") + (x->isEnabled() ? "" : " (off)");
            if (x->text() == "filter.sch") other = x;
            if (x->objectName() == "claudeUnpin") unpin = x;
        }
        QCOMPARE(items, QStringList({"amp.sch*", "filter.sch", "Unpin"}));
        other->trigger();
        QVERIFY(panel->isPinnedTo(b));
        QCOMPARE(panel->session()->document(), panel->pinnedDocument());

        // The prompts name it - with the document chip off too - and not
        // the document in front.
#ifndef Q_OS_WIN   // (the fake claude is a shell script)
        {
            QFile::remove(dir.filePath("prompts"));
            panel->session()->setProgram(script("writer", kWriter));
            front = a;
            panel->attachButton()->setChecked(false);
            panel->composer()->setPlainText("Check the gain");
            panel->sendComposer();
            QTRY_VERIFY_WITH_TIMEOUT(read(dir.filePath("prompts")).contains("Check the gain"), 10000);
            const QString sent = read(dir.filePath("prompts"));
            QVERIFY2(sent.contains("pinned to") && sent.contains("filter.sch"), qPrintable(sent));
            QVERIFY(!sent.contains("The document open in Qucs-S"));
            QVERIFY(!sent.contains("amp.sch"));
            panel->session()->stop();
            panel->attachButton()->setChecked(true);
        }
#endif

        // Closed: said so, and still pinned.
        open = {a};
        tabs.refreshDocument();
        QVERIFY(pin->toolTip().contains("not open"));
        emit menu->aboutToShow();
        QStringList later;
        for (QAction* x : menu->actions())
            if (!x->isSeparator()) later << x->text() + (x->isChecked() ? "*" : "");
        QCOMPARE(later, QStringList({"amp.sch", "filter.sch (not open)*", "Unpin"}));

        // Unpinned from the menu: as at first.
        for (QAction* x : menu->actions())
            if (x->objectName() == "claudeUnpin") unpin = x;
        QVERIFY(unpin != nullptr && unpin->isEnabled());
        unpin->trigger();
        QVERIFY(panel->pinnedDocument().isEmpty());
        QVERIFY(panel->session()->document().isEmpty());
        QVERIFY(panel->attachButton()->isVisibleTo(panel));
        QVERIFY(!pin->isChecked());
        QVERIFY(!tabs.tabWidget()->tabToolTip(0).contains("Pinned to"));
        emit menu->aboutToShow();
        for (QAction* x : menu->actions())
            if (x->objectName() == "claudeUnpin") QVERIFY(!x->isEnabled());

        // A Save As moves it; another conversation, pinned to nothing, is
        // not touched; New unpins.
        open = {a, b};
        panel->pinDocument(a);
        ClaudeCodePanel* second = tabs.newConversation();
        QVERIFY(second->pinnedDocument().isEmpty());   // (a new one: not pinned)
        const QString moved = work + "/amp2.sch";
        QVERIFY(QFile::copy(a, moved));
        tabs.documentRenamed(a, moved);
        QVERIFY(panel->isPinnedTo(moved));
        QVERIFY(second->pinnedDocument().isEmpty());
        panel->newConversation();
        QVERIFY(panel->pinnedDocument().isEmpty());
        QVERIFY(panel->session()->document().isEmpty());
    }

    // Renames among everything else the tabs go through - opened, closed,
    // moved, brought forward, begun again, their menus - in any order:
    // one editor at most, over a tab that is there, and every tab says
    // its conversation's title.
    void renamingSurvivesAnything()
    {
        ClaudeCodeTabs tabs;
        tabs.setDefaultDirectory(fresh("renamefuzz"));
        tabs.resize(360, 500);
        tabs.show();
        tabs.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&tabs));
        QTabBar* bar = tabs.tabWidget()->tabBar();
        std::mt19937 rng(20260925);
        const auto pick = [&rng](int n) { return int(rng() % unsigned(n)); };
        const QStringList words{"", " ", "Amplifier", "&&", "a & b", "  spaced   out  ", "Ω filter",
                                QString(150, QLatin1Char('x')), "\t", "New conversation", "émoji 🎛"};
        int renaming = 0;   // rounds with a rename under way
        for (int round = 0; round < 1500; ++round) {
            const QList<ClaudeCodePanel*> all = tabs.panels();
            ClaudeCodePanel* some = all.at(pick(int(all.size())));
            QLineEdit* editor = tabs.renameEditor();
            switch (pick(14)) {
            case 0: tabs.renameConversation(some); break;
            case 1: if (editor) editor->insert(words.at(pick(int(words.size())))); break;   // (typed)
            case 2: if (editor) QTest::keyClick(editor, Qt::Key_Return); break;
            case 3: if (editor) QTest::keyClick(editor, Qt::Key_Escape); break;
            case 4: if (editor) some->composer()->setFocus(); break;
            case 5: if (tabs.count() < 7) tabs.newConversation(); break;
            case 6: tabs.closeConversation(some, false); break;
            case 7: tabs.showConversation(some); break;
            case 8: if (tabs.count() > 1) bar->moveTab(pick(tabs.count()), pick(tabs.count())); break;
            case 9: some->setName(words.at(pick(int(words.size())))); break;
            case 10: some->newConversation(); break;
            case 11: {
                std::unique_ptr<QMenu> menu(tabs.tabMenu(pick(tabs.count())));
                const QList<QAction*> actions = menu->actions();
                QAction* a = actions.at(pick(int(actions.size())));
                if (!a->isSeparator() && a->isEnabled()) a->trigger();
                break;
            }
            case 12: {
                const QRect r = bar->tabRect(pick(tabs.count()));
                QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, r.center());
                break;
            }
            default:
                if (editor) editor->setText(words.at(pick(int(words.size()))));
                QCoreApplication::processEvents();   // (the menus' queued items)
                break;
            }
            QVERIFY(tabs.count() >= 1);
            if (QLineEdit* now = tabs.renameEditor()) {
                ++renaming;
                QVERIFY(now->isVisible());
                QVERIFY(bar->rect().intersects(now->geometry()));
            }
            // (One that finished goes a moment later, hidden.)
            const QList<QLineEdit*> editors = bar->findChildren<QLineEdit*>();
            QCOMPARE(std::count_if(editors.cbegin(), editors.cend(), [](QLineEdit* e) { return !e->isHidden(); }),
                     tabs.renameEditor() != nullptr ? 1 : 0);
            for (int i = 0; i < tabs.count(); ++i) {
                auto* panel = qobject_cast<ClaudeCodePanel*>(tabs.tabWidget()->widget(i));
                QVERIFY(panel != nullptr);
                QCOMPARE(tabs.tabWidget()->tabText(i), QString(panel->title()).replace("&", "&&"));
                QCOMPARE(panel->name(), panel->name().simplified());
            }
        }
        QVERIFY(renaming > 150);
    }

    // The dock: the conversation as it goes, the permission card and its
    // buttons, Enter to send, the suggestions, a file Claude changed.
    void theDockShowsTheConversation()
    {
        skipWithoutShell();
        QFile::remove(dir.filePath("answers"));
        const QString work = fresh("dockwork");
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(work);
        panel.setDocumentProvider([work] { return work + "/amp.sch"; });
        panel.session()->setProgram(script("writer", kWriter));
        panel.resize(420, 700);
        panel.show();
        QCOMPARE(panel.workingDirectory(), work);
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains(QDir::toNativeSeparators(work).section('/', -1)));

        // A suggestion fills the prompt.
        emit panel.transcript()->anchorClicked(QUrl("prompt:0"));
        QVERIFY(panel.composer()->toPlainText().contains("open schematic"));
        QVERIFY(panel.attachButton()->isChecked());

        panel.composer()->setPlainText("Write hi to out.txt");
        QSignalSpy changed(&panel, &ClaudeCodePanel::filesChanged);
        QSignalSpy asks(panel.session(), &Session::permissionRequested);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(panel.composer(), &enter);
        QVERIFY(panel.composer()->toPlainText().isEmpty());
        QCOMPARE(panel.sendButton()->text(), QStringLiteral("Stop"));

        QVERIFY(asks.wait(10000));
        QVERIFY(read(dir.filePath("prompts")).contains("amp.sch"));   // the open document went along
        QVERIFY(panel.permissionCard()->isVisible());
        QVERIFY(panel.allowEditsButton()->isVisible());
        panel.allowButton()->click();
        QVERIFY(!panel.permissionCard()->isVisible());
        QVERIFY(changed.count() == 1 || changed.wait(10000));
        QCOMPARE(changed.last().at(0).toStringList(), QStringList{work + "/out.txt"});
        panel.renderNow();
        const QString text = panel.transcriptText();
        QVERIFY(text.contains("Write hi to out.txt"));
        QVERIFY(text.contains("amp.sch"));
        QVERIFY(text.contains("I will write the file."));
        QVERIFY(text.contains("Write"));
        QVERIFY(text.contains("Done: wrote out.txt."));   // Markdown, drawn
        QVERIFY(text.contains("$0.012"));
        QVERIFY(text.contains("changed out.txt"));
        QCOMPARE(panel.sendButton()->text(), QStringLiteral("Send"));
        QVERIFY(panel.stateLabel()->text().contains("ready"));

        panel.newConversation();
        panel.renderNow();
        QVERIFY(!panel.transcriptText().contains("Write hi"));
        QVERIFY(panel.session()->sessionId().isEmpty());
    }

    // Nothing going on, nothing drawn - also after the look changed.
    void anIdleDockIsQuiet()
    {
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(fresh("quiet"));
        panel.show();
        QTest::qWait(200);
        int drawn = 0;
        connect(panel.transcript()->document(), &QTextDocument::contentsChanged, &panel, [&drawn] { ++drawn; });
        QTest::qWait(400);
        QCOMPARE(drawn, 0);

        QPalette dark = QApplication::palette();
        dark.setColor(QPalette::Base, QColor(0x1b, 0x1c, 0x20));
        dark.setColor(QPalette::Text, QColor(0xe6, 0xe6, 0xe6));
        const QPalette before = QApplication::palette();
        QApplication::setPalette(dark);
        QTest::qWait(300);
        drawn = 0;
        QTest::qWait(400);
        QCOMPARE(drawn, 0);
        QApplication::setPalette(before);
    }

    // In the application: the dock works in the workspace folder and moves
    // with it, the status bar says what Claude does and brings the dock,
    // and a file Claude changed is loaded again - unless it has unsaved
    // changes in Qucs-S.
    void theApplicationWorksWithTheDock()
    {
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.XyceExecutable = "xyce";
        QucsSettings.SpiceOpusExecutable = "spiceopus";
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        const QString workspace = fresh("workspace");
        QucsSettings.qucsWorkspaceDir.setPath(workspace);

        QucsApp app(false);
        MainGuard guard(&app);
        ClaudeCodeTabs* tabs = app.claudeCode();
        QVERIFY(tabs != nullptr);
        ClaudeCodePanel* panel = tabs->current();
        QVERIFY(panel != nullptr);
        QCOMPARE(panel->workingDirectory(), QDir(workspace).absolutePath());
        QVERIFY(app.claudeDockWidget()->isHidden());

        // The status bar.
        auto* chip = app.findChild<QToolButton*>("statusClaude");
        QVERIFY(chip != nullptr);
        panel->session()->setProgram(QString());
        QVERIFY(chip->text().contains("not installed"));
        panel->session()->setProgram("claude");
        panel->session()->handleLine(R"({"type":"control_request","request_id":"r1","request":{"subtype":"can_use_tool","tool_name":"Bash","input":{"command":"ls"}}})");
        QVERIFY(chip->text().contains("needs you"));
        QVERIFY(!app.claudeDockWidget()->isHidden());   // a question brings the dock
        QVERIFY(panel->permissionCard()->isVisibleTo(panel));
        panel->session()->reset();
        QVERIFY(!panel->permissionCard()->isVisibleTo(panel));
        app.resize(1200, 800);
        app.show();
        chip->click();
        QVERIFY(app.claudeDockWidget()->isHidden());
        chip->click();
        QVERIFY(app.claudeDockWidget()->isVisible());
        // It goes down to the status bar, the dock at the bottom beside it.
        QDockWidget* bottom = app.messages()->msgDock;
        bottom->show();
        QTRY_VERIFY(bottom->isVisible() && bottom->height() > 0);
        QTRY_VERIFY(bottom->geometry().right() < app.claudeDockWidget()->geometry().left());
        QVERIFY(app.claudeDockWidget()->geometry().bottom() > bottom->geometry().top());

        // A second conversation waits behind the first: the chip says so
        // (and that another is there), and leads to it.
        ClaudeCodePanel* second = tabs->newConversation();
        tabs->showConversation(panel);
        second->session()->setProgram("claude");
        second->session()->handleLine(R"({"type":"control_request","request_id":"r2","request":{"subtype":"can_use_tool","tool_name":"Bash","input":{"command":"ls"}}})");
        QVERIFY(chip->text().contains("needs you"));
        QVERIFY(chip->toolTip().contains("New conversation"));
        chip->click();
        QCOMPARE(tabs->current(), second);
        QVERIFY(app.claudeDockWidget()->isVisible());
        second->session()->reset();
        QVERIFY(tabs->closeConversation(second, false));
        QCOMPARE(tabs->current(), panel);

        // Another workspace: the dock goes along.
        const QString other = fresh("workspace2");
        QucsSettings.qucsWorkspaceDir.setPath(other);
        tabs->setDefaultDirectory(QucsSettings.qucsWorkspaceDir.absolutePath());
        QCOMPARE(panel->workingDirectory(), QDir(other).absolutePath());

        // A schematic Claude changed.
        const QString file = workspace + "/rc.sch";
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), file));
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);
        QVERIFY(app.gotoPage(file, false, false));
        auto* doc = dynamic_cast<Schematic*>(app.getDoc());
        QVERIFY(doc != nullptr);
        const auto resistance = [doc] {
            for (auto* c : doc->a_DocComps)
                if (c->Name == "R1") return c->Props.front()->Value;
            return QString();
        };
        QCOMPARE(resistance(), QStringLiteral("30"));
        QString text = read(file);
        QVERIFY(text.contains("\"30\""));
        const auto rewrite = [&](const QString& from, const QString& to) {
            text.replace(from, to);
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(text.toUtf8());
        };
        rewrite("\"30\"", "\"47\"");
        app.reloadChangedFiles({file});
        doc = dynamic_cast<Schematic*>(app.getDoc());
        QCOMPARE(resistance(), QStringLiteral("47"));
        panel->renderNow();
        QVERIFY(panel->transcriptText().contains("rc.sch loaded again"));

        // Unsaved changes of its own: left as it is.
        doc->setChanged(true, true);
        rewrite("\"47\"", "\"68\"");
        app.reloadChangedFiles({file});
        QCOMPARE(resistance(), QStringLiteral("47"));
        panel->renderNow();
        QVERIFY(panel->transcriptText().contains("unsaved changes"));
        doc->setChanged(false);

        // Changed while its symbol is edited: loaded, the symbol still in
        // front, and the schematic's nodes are its own.
        app.symEdit->trigger();
        QVERIFY(doc->getSymbolMode());
        doc->setChanged(false);   // (the symbol it had not, made)
        rewrite("\"68\"", "\"56\"");
        app.reloadChangedFiles({file});
        QCOMPARE(resistance(), QStringLiteral("56"));
        QVERIFY(doc->getSymbolMode());
        QVERIFY(doc->a_Nodes->empty());
        QCOMPARE(app.symEdit->text(), QStringLiteral("Edit Schematic"));
        app.symEdit->trigger();
        QVERIFY(!doc->getSymbolMode());
        QVERIFY(!doc->a_DocNodes.empty());
        doc->setChanged(false);   // (made again, the file has none)
        QVERIFY(app.closeAllFiles());
    }
};

QTEST_MAIN(TestClaudeCode)
#include "test_claude_code.moc"
