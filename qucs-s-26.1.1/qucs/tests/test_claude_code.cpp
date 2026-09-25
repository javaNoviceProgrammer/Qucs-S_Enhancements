/*
 * The Claude Code dock: the session that drives the claude program over
 * stream-json (claudecode.h), the dock (claudecodepanel.h), and what the
 * application does with them - the dock works in the workspace folder,
 * the status bar follows the session, files Claude changed are loaded
 * again. The program is a shell script here that answers as claude does.
 */
#include <QtTest>
#include <QApplication>
#include <QDockWidget>
#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>

#include "claudecode.h"
#include "claudecodepanel.h"
#include "config.h"
#include "isolated_settings.h"
#include "main.h"
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

        const QStringList all = qucs_s::claude::arguments({"acceptEdits", "opus", "s-9", "Be brief."});
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
        ClaudeCodePanel* panel = app.claudeCode();
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
        app.show();
        chip->click();
        QVERIFY(app.claudeDockWidget()->isHidden());
        chip->click();
        QVERIFY(app.claudeDockWidget()->isVisible());

        // Another workspace: the dock goes along.
        const QString other = fresh("workspace2");
        QucsSettings.qucsWorkspaceDir.setPath(other);
        panel->setDefaultDirectory(QucsSettings.qucsWorkspaceDir.absolutePath());
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
        QVERIFY(app.closeAllFiles());
    }
};

QTEST_MAIN(TestClaudeCode)
#include "test_claude_code.moc"
