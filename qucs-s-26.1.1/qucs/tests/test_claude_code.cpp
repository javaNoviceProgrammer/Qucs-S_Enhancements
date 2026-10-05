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
#include <QFrame>
#include <QCheckBox>
#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QApplication>
#include <QDockWidget>
#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMap>
#include <QMessageBox>
#include <QTableView>
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
#include "claudehistory.h"
#include "mathtypeset.h"
#include "config.h"
#include "isolated_settings.h"
#include "main.h"
#include "messagedock.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "schematic.h"
#include "settings.h"
#include "sheetdoc.h"
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
          echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":1200,"num_turns":2,"result":"Done","total_cost_usd":0.0123,"modelUsage":{"claude-opus-5-5":{"inputTokens":1200,"outputTokens":340,"cacheReadInputTokens":10500,"cacheCreationInputTokens":800,"costUSD":0.0123}},"permission_denials":[],"session_id":"s-1"}'
          ;;
        *)
          echo '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"The user did not allow this.","is_error":true}]}}'
          echo '{"type":"assistant","message":{"content":[{"type":"text","text":"Understood, I left it."}]}}'
          echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":900,"num_turns":2,"result":"x","total_cost_usd":0.004,"modelUsage":{"claude-opus-5-5":{"inputTokens":300,"outputTokens":50,"cacheReadInputTokens":0,"cacheCreationInputTokens":0,"costUSD":0.004}},"permission_denials":[{"tool_name":"Write"}],"session_id":"s-1"}'
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

// claude that changes its model and mode as it runs, when told: a turn
// says which it works with; "claude-refused" it will not change to - and
// a turn under way then ends at the refusal. Each start is a line in
// switcher-starts, what it reads goes to switcher-in.
const char* const kSwitcher = R"SH(#!/bin/sh
printf '%s\n' "$@" > "$QUCS_FAKE_DIR/switcher-args"
echo start >> "$QUCS_FAKE_DIR/switcher-starts"
model=claude-default-1
mode=default
while [ $# -gt 0 ]; do
  case "$1" in
    --model) model=$2 ;;
    --permission-mode) mode=$2 ;;
  esac
  shift
done
busy=
while IFS= read -r line; do
  printf '%s\n' "$line" >> "$QUCS_FAKE_DIR/switcher-in"
  id=$(printf '%s' "$line" | sed -n 's/.*"request_id":"\([^"]*\)".*/\1/p')
  case "$line" in
    *'"type":"user"'*)
      echo '{"type":"system","subtype":"init","session_id":"sw-1","model":"'"$model"'","permissionMode":"'"$mode"'"}'
      case "$line" in
        *'Take long'*) busy=1 ;;
        *) echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":5,"num_turns":1,"result":"ok","total_cost_usd":0.001,"session_id":"sw-1"}' ;;
      esac
      ;;
    *'"subtype":"set_model"'*)
      wanted=$(printf '%s' "$line" | sed -n 's/.*"model":"\([^"]*\)".*/\1/p')
      if [ "$wanted" = claude-refused ]; then
        echo '{"type":"control_response","response":{"subtype":"error","request_id":"'"$id"'","error":"not that one"}}'
      else
        [ "$wanted" = default ] && wanted=claude-default-1
        model=$wanted
        echo '{"type":"control_response","response":{"subtype":"success","request_id":"'"$id"'"}}'
      fi
      if [ -n "$busy" ]; then
        busy=
        echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":5,"num_turns":1,"result":"ok","total_cost_usd":0.001,"session_id":"sw-1"}'
      fi
      ;;
    *'"subtype":"set_permission_mode"'*)
      mode=$(printf '%s' "$line" | sed -n 's/.*"mode":"\([^"]*\)".*/\1/p')
      echo '{"type":"control_response","response":{"subtype":"success","request_id":"'"$id"'","response":{"mode":"'"$mode"'"}}}'
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

// A program whose server asks the user three questions (elicitation): yes
// or no, one of two, a small form; then asks to use the host's tool on
// something that cannot be undone, and on something that can. What the
// session writes back goes to ask-in.
const char* const kAsker = R"SH(#!/bin/sh
while IFS= read -r line; do
  printf '%s\n' "$line" >> "$QUCS_FAKE_DIR/ask-in"
  case "$line" in
    *'"type":"user"'*)
      echo '{"type":"system","subtype":"init","session_id":"ask-1","model":"claude-test-1"}'
      echo '{"type":"control_request","request_id":"e1","request":{"subtype":"elicitation","mcp_server_name":"fake","message":"Write over taken.sch?","requested_schema":{"type":"object","properties":{"confirm":{"type":"boolean"}},"required":["confirm"]}}}'
      echo '{"type":"control_request","request_id":"e2","request":{"subtype":"elicitation","mcp_server_name":"fake","message":"amp.sch has unsaved changes.","requested_schema":{"type":"object","properties":{"choice":{"type":"string","enum":["Save","Discard","Keep it open"]}}}}}'
      echo '{"type":"control_request","request_id":"e3","request":{"subtype":"elicitation","mcp_server_name":"fake","message":"Name the copy","requested_schema":{"type":"object","properties":{"name":{"type":"string","title":"Name"},"count":{"type":"integer"},"open":{"type":"boolean","default":true}}}}}'
      echo '{"type":"control_request","request_id":"e4","request":{"subtype":"elicitation","mcp_server_name":"fake","message":"Never mind","requested_schema":{"type":"object","properties":{"confirm":{"type":"boolean"}}}}}'
      ;;
    *'"request_id":"e4"'*)
      echo '{"type":"control_request","request_id":"p1","request":{"subtype":"can_use_tool","tool_name":"mcp__fake__change","display_name":"Change","input":{"what":"x"},"permission_suggestions":[]}}'
      ;;
    *'"request_id":"p1"'*'"allow"'*)
      echo '{"type":"control_request","request_id":"p2","request":{"subtype":"can_use_tool","tool_name":"mcp__fake__change","display_name":"Change","input":{"what":"wipe"},"permission_suggestions":[]}}'
      ;;
    *'"request_id":"p2"'*)
      echo '{"type":"control_request","request_id":"p3","request":{"subtype":"can_use_tool","tool_name":"mcp__fake__change","display_name":"Change","input":{"what":"y"},"permission_suggestions":[]}}'
      ;;
    *'"request_id":"p3"'*)
      echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":5,"num_turns":1,"result":"ok","total_cost_usd":0.001,"session_id":"ask-1"}'
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

// ... whose "change" cannot be undone when it wipes.
class LastingHost : public FakeHost
{
public:
    bool irreversible(const QString& tool, const QJsonObject& a) const override
    {
        return tool == QLatin1String("change") && a.value("what").toString() == QLatin1String("wipe");
    }
};

// ... whose "change" types a command when it types: asked about every
// time, also where Claude acts on its own.
class TypingHost : public LastingHost
{
public:
    QString askedEachTime(const QString& tool, const QJsonObject& a) const override
    {
        return tool == QLatin1String("change") && a.value("what").toString() == QLatin1String("type")
                   ? QStringLiteral("What is typed runs with your rights.")
                   : QString();
    }
};

// ... with more to say than Claude Code keeps unless told: \a instructions
// characters of them, and a tool of \a description characters.
class WordyHost : public FakeHost
{
public:
    WordyHost(int instructions, int description) : a_instructions(instructions), a_description(description) {}
    QString instructions() const override { return QString(a_instructions, QLatin1Char('i')); }
    QJsonArray tools() const override
    {
        QJsonArray list = FakeHost::tools();
        list.append(QJsonObject{{"name", "explain"}, {"description", QString(a_description, QLatin1Char('d'))}, {"inputSchema", QJsonObject{{"type", "object"}}}});
        return list;
    }

private:
    int a_instructions;
    int a_description;
};

// A program that says how much of a server's instructions it keeps.
const char* const kCapper = "#!/bin/sh\nIFS= read -r line\nprintf '%s' \"${CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH-unset}\" > \"$QUCS_FAKE_DIR/cap\"\nexit 3\n";

// A program that has no conversation to continue (--resume): it says so as
// Claude Code does, and ends; without, it answers, with its commands.
const char* const kResumer = R"SH(#!/bin/sh
printf '%s\n' "$@" >> "$QUCS_FAKE_DIR/resumer-args"
case " $* " in
  *" --resume gone-1 "*)
    read -r line
    echo '{"type":"result","subtype":"error_during_execution","is_error":true,"num_turns":0,"session_id":"gone-1","total_cost_usd":0}'
    echo 'No conversation found with session ID: gone-1' >&2
    exit 1
    ;;
esac
while IFS= read -r line; do
  case "$line" in
    *'"type":"user"'*)
      printf '%s\n' "$line" >> "$QUCS_FAKE_DIR/resumer-prompts"
      echo '{"type":"system","subtype":"init","session_id":"fresh-1","model":"claude-test-1","slash_commands":["compact","context","doctor","my-skill"],"terminal_slash_commands":["doctor"]}'
      echo '{"type":"assistant","message":{"content":[{"type":"text","text":"Fresh."}]}}'
      echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":5,"num_turns":1,"result":"Fresh.","total_cost_usd":0.001,"session_id":"fresh-1"}'
      ;;
  esac
done
)SH";

// Each start a line of its arguments (rewinder-starts), each prompt a line
// (rewinder-prompts) and a reply whose message has a uuid, m<k> for the
// k-th prompt of all. Taken back to "bad-point" (--resume-session-at), it
// says it has no such message, as Claude Code does, and ends; forked, its
// session is forked-<starts>. A prompt with "slow" in it does not end.
const char* const kRewinder = R"SH(#!/bin/sh
echo "$*" >> "$QUCS_FAKE_DIR/rewinder-starts"
sid=s-1
case " $* " in
  *" --resume-session-at bad-point "*)
    read -r line
    echo '{"type":"result","subtype":"error_during_execution","is_error":true,"num_turns":0,"session_id":"x","total_cost_usd":0}'
    echo 'No message found with message.uuid of: bad-point' >&2
    exit 1
    ;;
  *" --fork-session "*) sid=forked-$(wc -l < "$QUCS_FAKE_DIR/rewinder-starts" | tr -d ' ') ;;
  *" --resume "*) sid=$(echo " $* " | sed 's/.* --resume \([^ ]*\) .*/\1/') ;;
esac
while IFS= read -r line; do
  case "$line" in
    *'"type":"user"'*)
      printf '%s\n' "$line" >> "$QUCS_FAKE_DIR/rewinder-prompts"
      k=$(wc -l < "$QUCS_FAKE_DIR/rewinder-prompts" | tr -d ' ')
      echo '{"type":"system","subtype":"init","session_id":"'"$sid"'","model":"claude-test-1"}'
      case "$line" in
        *slow*)
          echo '{"type":"assistant","uuid":"m'"$k"'","message":{"content":[{"type":"tool_use","id":"t'"$k"'","name":"Bash","input":{"command":"sleep 100"}}]}}'
          ;;
        *)
          echo '{"type":"assistant","uuid":"m'"$k"'","message":{"content":[{"type":"text","text":"Reply '"$k"'."}]}}'
          echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":5,"num_turns":1,"result":"ok","total_cost_usd":0.001,"session_id":"'"$sid"'"}'
          ;;
      esac
      ;;
  esac
done
)SH";

// A program that fails at once.
const char* const kBroken = "#!/bin/sh\necho 'Invalid API key' >&2\nexit 3\n";

// The tooltip of the text \a text in the document, where it is drawn.
QString toolTipOf(const QTextDocument* doc, const QString& text)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (it.fragment().text() == text) return it.fragment().charFormat().toolTip();
    return QString();
}

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

        s.handleLine(R"({"type":"result","subtype":"success","is_error":false,"duration_ms":4200,"num_turns":3,"result":"Hello","total_cost_usd":0.25,"modelUsage":{"claude-opus-5-5":{"inputTokens":10,"outputTokens":200,"cacheReadInputTokens":3000,"cacheCreationInputTokens":400,"costUSD":0.24},"claude-haiku-4-5":{"inputTokens":5,"outputTokens":15,"cacheReadInputTokens":0,"cacheCreationInputTokens":0,"costUSD":0.01}},"usage":{"input_tokens":10,"output_tokens":200},"permission_denials":[{"tool_name":"Bash"}]})");
        QCOMPARE(turns.count(), 1);
        const auto result = turns.last().at(0).value<TurnResult>();
        QVERIFY(result.ok);
        QCOMPARE(result.durationMs, qint64(4200));
        QCOMPARE(result.turns, 3);
        QCOMPARE(result.denials, 1);
        QCOMPARE(result.costUsd, 0.25);
        // Its tokens: every model's (the main loop's "usage" leaves the
        // others out).
        QCOMPARE(result.tokens.input, qint64(15));
        QCOMPARE(result.tokens.output, qint64(215));
        QCOMPARE(result.tokens.cacheRead, qint64(3000));
        QCOMPARE(result.tokens.cacheWrite, qint64(400));
        QCOMPARE(result.tokens.total(), qint64(3630));
        QCOMPARE(result.conversationTokens, result.tokens);
        QCOMPARE(result.changedFiles, QStringList{"/work/a.sch"});
        QCOMPARE(s.state(), State::Ready);

        // The program counts its cost from its start: a turn's is what it added.
        // Its tokens likewise.
        s.handleLine(R"({"type":"result","subtype":"success","is_error":false,"duration_ms":10,"num_turns":1,"result":"x","total_cost_usd":0.30,"modelUsage":{"claude-opus-5-5":{"inputTokens":20,"outputTokens":260,"cacheReadInputTokens":7000,"cacheCreationInputTokens":500},"claude-haiku-4-5":{"inputTokens":5,"outputTokens":15,"cacheReadInputTokens":0,"cacheCreationInputTokens":0}}})");
        QVERIFY(qAbs(turns.last().at(0).value<TurnResult>().costUsd - 0.05) < 1e-9);
        QVERIFY(qAbs(turns.last().at(0).value<TurnResult>().conversationCostUsd - 0.30) < 1e-9);
        const auto second = turns.last().at(0).value<TurnResult>();
        QCOMPARE(second.tokens.input, qint64(10));
        QCOMPARE(second.tokens.output, qint64(60));
        QCOMPARE(second.tokens.cacheRead, qint64(4000));
        QCOMPARE(second.tokens.cacheWrite, qint64(100));
        QCOMPARE(second.conversationTokens.total(), qint64(3630 + 4170));
        QCOMPARE(s.conversationTokens(), second.conversationTokens);

        s.handleLine(R"({"type":"result","subtype":"error_max_turns","is_error":true,"duration_ms":1,"num_turns":9,"result":""})");
        QCOMPARE(s.state(), State::Failed);
        QVERIFY(s.detail().contains("too many steps"));
        // A sign-in that failed: how to sign in again, which /login in the
        // dock cannot do.
        s.handleLine(R"({"type":"result","subtype":"success","is_error":true,"duration_ms":1,"num_turns":1,"result":"Failed to authenticate. API Error: 401"})");
        QCOMPARE(s.state(), State::Failed);
        QVERIFY2(s.detail().contains("run claude in a terminal"), qPrintable(s.detail()));
        // (A result with no tokens takes none away.)
        QVERIFY(turns.last().at(0).value<TurnResult>().tokens.isEmpty());
        QCOMPARE(s.conversationTokens().total(), qint64(3630 + 4170));
    }

    // The tokens of a result: its "modelUsage" is the program's running
    // total, each turn what it added (a zeroed one, as a crash's, takes
    // nothing away); without one, its "usage" is the turn's own.
    void aTurnsTokensAreCounted()
    {
        using qucs_s::claude::TokenUsage;
        const auto result = [](const char* usage) {
            return QByteArray(R"({"type":"result","subtype":"success","is_error":false,"num_turns":1,"result":"x",)") + usage + "}";
        };
        {
            Session s;
            QSignalSpy turns(&s, &Session::turnFinished);
            s.handleLine(result(R"("modelUsage":{"m":{"inputTokens":100,"outputTokens":10,"cacheReadInputTokens":1000,"cacheCreationInputTokens":50}})"));
            s.handleLine(result(R"("modelUsage":{"m":{"inputTokens":0,"outputTokens":0,"cacheReadInputTokens":0,"cacheCreationInputTokens":0}})"));
            QVERIFY(turns.last().at(0).value<TurnResult>().tokens.isEmpty());
            s.handleLine(result(R"("modelUsage":{"m":{"inputTokens":130,"outputTokens":30,"cacheReadInputTokens":2500,"cacheCreationInputTokens":50}})"));
            const auto r = turns.last().at(0).value<TurnResult>();
            QCOMPARE(r.tokens.total(), qint64(30 + 20 + 1500));
            QCOMPARE(r.conversationTokens.total(), qint64(2710));
        }
        {
            Session s;
            QSignalSpy turns(&s, &Session::turnFinished);
            const QByteArray turn = result(R"("usage":{"input_tokens":7,"output_tokens":9,"cache_read_input_tokens":100,"cache_creation_input_tokens":20})");
            s.handleLine(turn);
            s.handleLine(turn);
            const auto r = turns.last().at(0).value<TurnResult>();
            QCOMPARE(r.tokens.total(), qint64(136));
            QCOMPARE(r.conversationTokens.total(), qint64(272));
            // Continued from a conversation kept, and forgotten with it.
            s.setConversationTotals(TokenUsage{1000, 2000, 3000, 4000}, 1.5);
            s.handleLine(turn);
            QCOMPARE(turns.last().at(0).value<TurnResult>().conversationTokens.total(), qint64(10136));
            QVERIFY(qAbs(s.conversationCost() - 1.5) < 1e-9);
            s.reset();
            QVERIFY(s.conversationTokens().isEmpty());
            QCOMPARE(s.conversationCost(), 0.0);
        }
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
        QCOMPARE(result.tokens.total(), qint64(12840));
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
        QCOMPARE(turns.last().at(0).value<TurnResult>().tokens.total(), qint64(350));
        QCOMPARE(turns.last().at(0).value<TurnResult>().conversationTokens.total(), qint64(13190));
        QVERIFY(read(dir.filePath("answers")).contains("\"behavior\":\"deny\""));
        s.stop();
    }

    // Another model or mode, chosen in a conversation: the program takes
    // it as it runs, for the next turn - not ended and started again. One
    // it will not take, it is started with, once the turn is over.
    void theModelAndModeChangeAsTheProgramRuns()
    {
        skipWithoutShell();
        for (const char* f : {"switcher-starts", "switcher-in"}) QFile::remove(dir.filePath(f));
        const auto starts = [this] { return int(read(dir.filePath("switcher-starts")).count("start")); };
        const auto readIn = [this] { return read(dir.filePath("switcher-in")); };
        Session s;
        s.setProgram(script("switcher", kSwitcher));
        s.setWorkingDirectory(fresh("switchwork"));
        s.setModel("claude-opus-5-5");
        QSignalSpy turns(&s, &Session::turnFinished);
        QSignalSpy modes(&s, &Session::modeInUseChanged);
        QVERIFY(s.send("Hello"));
        QVERIFY(turns.wait(10000));
        QCOMPARE(s.modelInUse(), QStringLiteral("claude-opus-5-5"));
        const QStringList args = read(dir.filePath("switcher-args")).split('\n');
        QCOMPARE(args.value(args.indexOf("--model") + 1), QStringLiteral("claude-opus-5-5"));

        // The model: asked for at once, which one is in use no longer known
        // - and the next turn says.
        s.setModel("claude-fable-5-1");
        QVERIFY(s.isRunning());
        QVERIFY(s.modelInUse().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(readIn().contains(R"("request":{"model":"claude-fable-5-1","subtype":"set_model"})"), 10000);
        QVERIFY(s.send("Again"));
        QVERIFY(turns.wait(10000));
        QCOMPARE(s.modelInUse(), QStringLiteral("claude-fable-5-1"));
        // The default by its name to the program.
        s.setModel(QString());
        QTRY_VERIFY_WITH_TIMEOUT(readIn().contains(R"("model":"default","subtype":"set_model")"), 10000);

        // The mode: the program says the one it works in.
        s.setPermissionMode("acceptEdits");
        QVERIFY(s.permissionModeInUse().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(s.permissionModeInUse(), QStringLiteral("acceptEdits"), 10000);
        QCOMPARE(modes.count(), 1);
        s.setPermissionMode(QString());   // asking: "default" to every version
        QTRY_VERIFY_WITH_TIMEOUT(readIn().contains(R"("mode":"default","subtype":"set_permission_mode")"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(s.permissionModeInUse(), QStringLiteral("default"), 10000);
        QVERIFY(s.isRunning());
        QCOMPARE(starts(), 1);
        // Bypassing, only from its start: not asked, ended.
        s.setPermissionMode("bypassPermissions");
        QTRY_VERIFY_WITH_TIMEOUT(!s.isRunning(), 10000);
        QVERIFY(!readIn().contains("bypassPermissions"));
        s.setPermissionMode(QString());
        QVERIFY(s.send("Hello again"));
        QVERIFY(turns.wait(10000));
        QCOMPARE(starts(), 2);

        // Refused, when no turn is under way: said, ended, and the next
        // prompt starts it with that model, continuing the conversation.
        QSignalSpy notices(&s, &Session::notice);
        s.setModel("claude-refused");
        QTRY_VERIFY_WITH_TIMEOUT(!s.isRunning(), 10000);
        QCOMPARE(notices.count(), 1);
        QCOMPARE(notices.last().at(0).toString(), QStringLiteral("not that one"));
        QVERIFY(s.send("Once more"));
        QVERIFY(turns.wait(10000));
        QCOMPARE(starts(), 3);
        const QStringList again = read(dir.filePath("switcher-args")).split('\n');
        QCOMPARE(again.value(again.indexOf("--model") + 1), QStringLiteral("claude-refused"));
        QCOMPARE(again.value(again.indexOf("--resume") + 1), QStringLiteral("sw-1"));
        QCOMPARE(s.modelInUse(), QStringLiteral("claude-refused"));

        // Refused during a turn: it ends once the turn is over, not before.
        s.setModel("claude-opus-5-5");
        QVERIFY(s.send("Take long"));
        QTRY_COMPARE_WITH_TIMEOUT(s.modelInUse(), QStringLiteral("claude-opus-5-5"), 10000);   // (taken meanwhile)
        s.setModel("claude-refused");
        QVERIFY(turns.wait(10000));
        QTRY_VERIFY_WITH_TIMEOUT(!s.isRunning(), 10000);
        QCOMPARE(starts(), 3);

        // In the dock: the header names the model chosen at once, in the
        // middle of a conversation.
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(fresh("switchdock"));
        panel.session()->setProgram(script("switcher", kSwitcher));
        QSignalSpy docked(panel.session(), &Session::turnFinished);
        panel.composer()->setPlainText("Hello");
        panel.sendComposer();
        QVERIFY(docked.wait(10000));
        QVERIFY(panel.modelLabel()->text().startsWith("Default 1"));
        QAction* fable = nullptr;
        for (QAction* a : panel.modelActions())
            if (a->text() == "Fable 5.1") fable = a;
        QVERIFY(fable != nullptr);
        fable->trigger();
        QVERIFY2(panel.modelLabel()->text().startsWith("Fable 5.1"), qPrintable(panel.modelLabel()->text()));
        QVERIFY(panel.session()->isRunning());
        panel.renderNow();
        QCOMPARE(panel.transcriptText().count("Fable 5.1 from the next prompt on."), 1);
        panel.composer()->setPlainText("/model Fable 5.1");   // (the same: said once more, not twice)
        panel.sendComposer();
        panel.renderNow();
        QCOMPARE(panel.transcriptText().count("from the next prompt on."), 2);
        QCOMPARE(panel.transcriptText().count("The model: Fable 5.1, from the next prompt on."), 1);
        // As it was, for the tests after.
        for (QAction* a : panel.modelActions())
            if (a->data().toString().isEmpty()) a->trigger();
        QVERIFY(!panel.modelLabel()->text().contains("Fable"));
        panel.session()->stop();
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

    // Claude Code keeps 2,048 characters of a server's instructions and of
    // each tool's description unless CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH
    // says more: the program is started with what the host's need, when
    // they need more - and with what the user set, when they set it.
    void theHostsInstructionsAreNotCut()
    {
        using qucs_s::claude::claudeEnvironment;
        const QString name = QStringLiteral("CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH");
        QProcessEnvironment base;
        base.insert("PATH", "/bin");
        QCOMPARE(claudeEnvironment(base, nullptr), base);
        FakeHost brief;
        QCOMPARE(claudeEnvironment(base, &brief), base);
        WordyHost atTheCap(qucs_s::claude::kDescriptionCap, 10);
        QCOMPARE(claudeEnvironment(base, &atTheCap), base);
        WordyHost wordy(5000, 300);
        QCOMPARE(qucs_s::claude::longestDescription(wordy), 5000);
        QCOMPARE(claudeEnvironment(base, &wordy).value(name), QStringLiteral("5000"));
        QCOMPARE(claudeEnvironment(base, &wordy).value("PATH"), QStringLiteral("/bin"));
        WordyHost described(100, 3000);
        QCOMPARE(claudeEnvironment(base, &described).value(name), QStringLiteral("3000"));
        QProcessEnvironment set = base;
        set.insert(name, "1000");
        QCOMPARE(claudeEnvironment(set, &wordy).value(name), QStringLiteral("1000"));

        // The program is started so; without a host, as it was.
        skipWithoutShell();
        const QByteArray was = qgetenv(name.toLatin1());
        const bool wasSet = qEnvironmentVariableIsSet(name.toLatin1());
        qunsetenv(name.toLatin1());
        const auto started = [&](qucs_s::claude::ToolHost* host) {
            QFile::remove(dir.filePath("cap"));
            Session s;
            s.setProgram(script("capper", kCapper));
            s.setWorkingDirectory(fresh("capwork"));
            if (host != nullptr) s.setToolHost(host);
            QSignalSpy failed(&s, &Session::failed);
            if (!s.send("Hello") || !failed.wait(10000)) return QStringLiteral("(not started)");
            return read(dir.filePath("cap"));
        };
        QCOMPARE(started(&wordy), QStringLiteral("5000"));
        QCOMPARE(started(&brief), QStringLiteral("unset"));
        QCOMPARE(started(nullptr), QStringLiteral("unset"));
        qputenv(name.toLatin1(), "1000");
        QCOMPARE(started(&wordy), QStringLiteral("1000"));
        if (wasSet) qputenv(name.toLatin1(), was);
        else qunsetenv(name.toLatin1());
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
        // (Not the server as a whole in every turn: its tools say which are,
        // "anthropic/alwaysLoad" - test_mcp_server.)
        QVERIFY(!config.contains("\"alwaysLoad\""));
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

    // Where Claude acts on its own (auto), a use that cannot be undone goes
    // unasked - but one asked about every time (a line typed into a
    // console: ToolHost::askedEachTime()) is asked, with its reason and no
    // "allow all"; only where nothing is asked (bypassPermissions) not.
    void aConsoleLineIsAskedAboutEvenInAutoMode()
    {
        TypingHost host;
        Session s;
        s.setProgram("claude");
        s.setToolHost(&host);
        s.setPermissionMode("auto");
        QSignalSpy asks(&s, &Session::permissionRequested);
        const auto request = [&s](const QString& id, const QString& what) {
            s.handleLine(QStringLiteral(R"({"type":"control_request","request_id":"%1","request":{"subtype":"can_use_tool","tool_name":"mcp__fake__change","input":{"what":"%2"}}})")
                             .arg(id, what).toUtf8());
        };
        request("r1", "wipe");
        QCOMPARE(asks.count(), 0);
        request("r2", "type");
        QCOMPARE(asks.count(), 1);
        const auto asked = asks.last().at(0).value<PermissionRequest>();
        QCOMPARE(asked.subject, QStringLiteral("what: type"));
        QCOMPARE(asked.detail, QStringLiteral("What is typed runs with your rights."));
        QVERIFY(!asked.canAllowTools);
        s.answer(asked.id, true);
        s.setPermissionMode("bypassPermissions");
        request("r3", "type");
        QCOMPARE(asks.count(), 1);
    }

    // A question the server asks (elicitation) is a card in the dock: yes
    // or no, a button for each choice, a small form - one at a time, each
    // answer written back as Claude Code wants it, Cancel too. A tool on
    // what cannot be undone (ToolHost::irreversible()) is asked about even
    // when the conversation may use the tools, and its question offers no
    // "allow all".
    void theServerAsksAndWhatCannotBeUndoneIsAskedAbout()
    {
        skipWithoutShell();
        QFile::remove(dir.filePath("ask-in"));
        QucsSettingsFile().remove("ClaudeCode/permissionMode");   // asking
        LastingHost host;
        ClaudeCodePanel panel;
        panel.resize(500, 700);
        panel.show();
        panel.setDefaultDirectory(fresh("askwork"));
        Session* s = panel.session();
        s->setProgram(script("asker", kAsker));
        s->setToolHost(&host);
        QSignalSpy asks(s, &Session::elicitationRequested);
        QSignalSpy permissions(s, &Session::permissionRequested);
        QSignalSpy turns(s, &Session::turnFinished);
        QVERIFY(s->send("Copy it"));
        QTRY_COMPARE_WITH_TIMEOUT(asks.count(), 4, 10000);
        QCOMPARE(asks.first().at(1).toString(), QStringLiteral("fake"));
        QFrame* card = panel.askCard();
        const auto visible = [&](const QString& name) -> QToolButton* {
            for (QToolButton* b : card->findChildren<QToolButton*>(name))
                if (b->isVisibleTo(&panel)) return b;
            return nullptr;
        };
        // Yes or no - the first, the others waiting.
        QTRY_VERIFY(card->isVisibleTo(&panel) && visible("claudeAskYes") != nullptr);
        QVERIFY(card->findChild<QLabel*>("claudeCardTitle")->text().contains("taken.sch"));
        QVERIFY(visible("claudeAskNo") != nullptr && visible("claudeAskCancel") != nullptr);
        visible("claudeAskYes")->click();
        // One of three.
        QTRY_VERIFY(visible("claudeAskOption") != nullptr);
        QStringList options;
        QToolButton* discard = nullptr;
        for (QToolButton* b : card->findChildren<QToolButton*>("claudeAskOption"))
            if (b->isVisibleTo(&panel)) {
                options << b->property("option").toString();
                if (b->text() == "Discard") discard = b;
            }
        QCOMPARE(options, (QStringList{"Save", "Discard", "Keep it open"}));
        discard->click();
        // A form.
        QTRY_VERIFY(visible("claudeAskSubmit") != nullptr);
        auto* name = card->findChild<QLineEdit*>("claudeAskField_name");
        auto* count = card->findChild<QLineEdit*>("claudeAskField_count");
        QVERIFY(name != nullptr && count != nullptr && card->findChild<QCheckBox*>("claudeAskField_open")->isChecked());
        name->setText("amp copy");
        count->setText("3");
        visible("claudeAskSubmit")->click();
        // Cancelled.
        QTRY_VERIFY(visible("claudeAskYes") != nullptr && card->findChild<QLabel*>("claudeCardTitle") != nullptr);
        visible("claudeAskCancel")->click();
        QTRY_VERIFY(!card->isVisibleTo(&panel));

        // The tool on what can be undone: asked, and all the host's tools allowed.
        QTRY_COMPARE_WITH_TIMEOUT(permissions.count(), 1, 10000);
        auto request = permissions.last().at(0).value<PermissionRequest>();
        QVERIFY(request.canAllowTools);
        s->answer(request.id, true, false, true);
        QVERIFY(s->toolsAllowed());
        // On what cannot: asked all the same, without "allow all".
        QTRY_COMPARE_WITH_TIMEOUT(permissions.count(), 2, 10000);
        request = permissions.last().at(0).value<PermissionRequest>();
        QCOMPARE(request.subject, QStringLiteral("what: wipe"));
        QVERIFY(!request.canAllowTools);
        QVERIFY(request.detail.contains("cannot be undone"));
        s->answer(request.id, false);
        // And again what can: not asked (the tools are allowed).
        QVERIFY(turns.count() == 1 || turns.wait(10000));
        QCOMPARE(permissions.count(), 2);

        QMap<QString, QJsonObject> answers;
        for (const QString& line : read(dir.filePath("ask-in")).split('\n', Qt::SkipEmptyParts)) {
            const QJsonObject m = QJsonDocument::fromJson(line.toUtf8()).object();
            if (m.value("type").toString() != "control_response") continue;
            const QJsonObject r = m.value("response").toObject();
            answers.insert(r.value("request_id").toString(), r.value("response").toObject());
        }
        QCOMPARE(answers.value("e1"), (QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"confirm", true}}}}));
        QCOMPARE(answers.value("e2"), (QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"choice", "Discard"}}}}));
        const QJsonObject form = answers.value("e3").value("content").toObject();
        QCOMPARE(form.value("name").toString(), QStringLiteral("amp copy"));
        QCOMPARE(form.value("count").toInt(), 3);
        QVERIFY(form.value("open").toBool());
        QCOMPARE(answers.value("e4"), (QJsonObject{{"action", "cancel"}}));
        QCOMPARE(answers.value("p1").value("behavior").toString(), QStringLiteral("allow"));
        QCOMPARE(answers.value("p2").value("behavior").toString(), QStringLiteral("deny"));
        QCOMPARE(answers.value("p3").value("behavior").toString(), QStringLiteral("allow"));
        s->stop();
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

    // Allow All Edits on a card is that conversation's alone, as its note
    // says: the saved permissions - what every new conversation starts
    // with, after a restart too - stay as the Permissions menu set them.
    // They became every conversation's (bug hunt 2026-09-26, B3).
    void allowAllEditsIsOneConversationsAlone()
    {
        QucsSettingsFile().remove("ClaudeCode/permissionMode");   // asking, the default
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(fresh("allowall"));
        panel.session()->setProgram("claude");   // (not run)
        panel.session()->handleLine(R"({"type":"control_request","request_id":"r1","request":{"subtype":"can_use_tool","tool_name":"Edit","input":{"file_path":"/work/a.sch","old_string":"1k","new_string":"2k"},"permission_suggestions":[{"type":"setMode","mode":"acceptEdits","destination":"session"}]}})");
        auto* allowEdits = panel.findChild<QToolButton*>("claudeAllowEdits");
        QVERIFY(allowEdits != nullptr && allowEdits->isVisibleTo(&panel));
        allowEdits->click();
        QCOMPARE(panel.session()->permissionMode(), QStringLiteral("acceptEdits"));
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("rest of this conversation"));
        const auto checked = [](ClaudeCodePanel& p) {
            for (QAction* a : p.permissionActions())
                if (a->isChecked()) return a->data().toString();
            return QStringLiteral("?");
        };
        QCOMPARE(checked(panel), QStringLiteral("acceptEdits"));   // the menu says what this one may do
        QVERIFY(QucsSettingsFile().value("ClaudeCode/permissionMode").toString().isEmpty());

        ClaudeCodePanel another;   // a new tab, or after a restart
        QVERIFY(qucs_s::claude::isAskMode(another.session()->permissionMode()));
        panel.newConversation();   // a new conversation in the same tab
        QVERIFY(qucs_s::claude::isAskMode(panel.session()->permissionMode()));

        // Chosen in the menu: saved, the new conversations' too.
        for (QAction* a : panel.permissionActions())
            if (a->data().toString() == "acceptEdits") a->trigger();
        QCOMPARE(QucsSettingsFile().value("ClaudeCode/permissionMode").toString(), QStringLiteral("acceptEdits"));
        ClaudeCodePanel third;
        QCOMPARE(third.session()->permissionMode(), QStringLiteral("acceptEdits"));
        QucsSettingsFile().remove("ClaudeCode/permissionMode");
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

    // Dollars in code are code: an indented code block (after a blank line,
    // not in a list, whose items go on indented) and HTML <code> and <pre>,
    // as GitHub reads them (bug hunt 2026-09-26, E6: "$HOME/bin:$" was set
    // as a formula in a shell command).
    void codeIsNotMath()
    {
        using qucs_s::math::findMath;
        const auto texts = [](const QString& md) {
            QStringList t;
            for (const auto& s : findMath(md)) t << s.tex;
            return t;
        };
        QCOMPARE(texts("Run it:\n\n    export PATH=$HOME/bin:$PATH\n    cp $SRC/a.sch $DST/\n\nand <code>$A-$B</code>\n"), QStringList());
        QCOMPARE(texts("\tx = $a$ + $b$\n"), QStringList());                                 // a tab, at the start
        QCOMPARE(texts("<pre>\n$HOME/x$\n</pre> then $y$"), QStringList{"y"});
        QCOMPARE(texts("<CODE class=\"sh\">$A$</CODE> $z$"), QStringList{"z"});
        QCOMPARE(texts("<codex>$w$</codex>"), QStringList{"w"});                              // not <code>
        // Indented, not code: in a paragraph (no blank line before), in a list.
        QCOMPARE(texts("The gain\n    is $A_v$ here."), QStringList{"A_v"});
        QCOMPARE(texts("- first\n\n    with $x^2$ in it\n- 1. nested\n\n        $y$\n"), (QStringList{"x^2", "y"}));
        QCOMPARE(texts("1. step\n\n    $\\omega$\n\nAfter.\n\n    $code$\n"), QStringList{"\\omega"});   // the list ended
        // After a code block, math again.
        QCOMPARE(texts("\n    $a$\n\nThen $b$."), QStringList{"b"});
        QCOMPARE(texts("```\n    $a$\n```\n$c$"), QStringList{"c"});
    }

    // A reply's HTML is read whole (the review of 5 October and its re-check).
    // Qt's Markdown importer dropped all the text after a tag it saw opened
    // and never closed, or after a closing tag with nothing open, but the
    // inline code: a <br> in a table's cell blanked the table's other rows
    // and every bullet and paragraph after it; so did a placeholder such
    // as <name>.dat or QList<Span>, a lone <b>, a stray </b>. A void element
    // is closed; a tag of no element, an element not closed in its
    // paragraph, a closing tag that closes nothing, are text - outside code.
    // The cell shows two lines; an <hr> in a paragraph comes between its
    // parts; the reply's text copy loses nothing.
    void aRepliesHtmlIsReadWhole()
    {
        using qucs_s::markdown::htmlBalanced;
        QCOMPARE(htmlBalanced("a<br>b"), QString("a<br/>b"));
        QCOMPARE(htmlBalanced("a<BR>b <Hr> <wbr>"), QString("a<BR/>b <Hr/> <wbr/>"));
        QCOMPARE(htmlBalanced("<img src=\"x.png\" alt=\"x\">"), QString("<img src=\"x.png\" alt=\"x\"/>"));
        QCOMPARE(htmlBalanced("<hr class=a >x"), QString("<hr class=a />x"));
        // Text: no element; an element not closed (in its paragraph); a
        // closing tag with nothing open; one left open inside another.
        const QList<std::pair<QString, QString>> text{
            {"a<p>b", "a&lt;p>b"}, {"a<li>item", "a&lt;li>item"}, {"a<span>x", "a&lt;span>x"}, {"a<u>x", "a&lt;u>x"},
            {"a<b>unclosed bold", "a&lt;b>unclosed bold"}, {"vector<int> x", "vector&lt;int> x"}, {"a <T> b", "a &lt;T> b"},
            {"a<x>b", "a&lt;x>b"}, {"The dataset is <name>.dat.ngspice", "The dataset is &lt;name>.dat.ngspice"},
            {"QList<Span> x", "QList&lt;Span> x"}, {"a <T> b </T> c", "a &lt;T> b &lt;/T> c"}, {"a</b>b", "a&lt;/b>b"},
            {"a<b>x</i>y", "a&lt;b>x&lt;/i>y"}, {"a<b><i>x</b>", "a<b>&lt;i>x</b>"},
            {"a</br>b", "a&lt;/br>b"}, {"<brx> <break>", "&lt;brx> &lt;break>"}, {"a<name/>b", "a&lt;name/>b"},
            {"`<br>` then <br> and <sch>", "`<br>` then <br/> and &lt;sch>"}, {"- item <br>\n\n    <name>\n", "- item <br/>\n\n    &lt;name>\n"}};
        for (const auto& [in, out] : text) QCOMPARE(htmlBalanced(in), out);
        // An element closed in another block - a paragraph, a list's item, a
        // cell beside it, <details> over blank lines - is taken out with its
        // closing tag (Qt moved the text between them); a comment, whatever is
        // in it, is taken out (a tag in it lost the rest), an unclosed one is
        // text.
        const QList<std::pair<QString, QString>> outs{
            {"<b>x\n\ny</b>", "x\n\ny"}, {"- item one <b>opened\n- item two</b> closed", "- item one opened\n- item two closed"},
            {"| col <b>x | y</b> col |", "| col x | y col |"}, {"# A <b>title\nthen</b> text", "# A title\nthen text"},
            {"> quoted <i>a\n> b</i>", "> quoted a\n> b"},
            {"<details>\n<summary>S</summary>\n\nbody\n\n</details>", "\n<summary>S</summary>\n\nbody\n\n"},
            {"<!-- a comment with <b> inside -->\nNext", "\nNext"}, {"text <!-- note <name> here --> more", "text  more"},
            {"a<!--\nmulti <img src=x>\n\nline\n-->b", "ab"}, {"a<!-- c -->b", "ab"},
            {"a<!-- c unclosed <b>x", "a&lt;!-- c unclosed &lt;b>x"}, {"a<!-- c unclosed", "a&lt;!-- c unclosed"},
            // (In <code> and <pre> the tags are HTML still, to Qt.)
            {"<code>a<br>b</code>", "<code>a<br/>b</code>"}, {"<pre>\n<hr> <x>\n</pre>", "<pre>\n<hr/> &lt;x>\n</pre>"}, {"`<!-- <b> -->` stays", "`<!-- <b> -->` stays"},
            {"\n# <b>x\ny</b>\n", "\n# x\ny\n"}};
        for (const auto& [in, out] : outs) QCOMPARE(htmlBalanced(in), out);
        // As written: closed elements (across a line, a cell, a list's items),
        // void ones closed already, autolinks, comments, code, escapes, '<' as
        // text.
        for (const char* same : {"a<br/>b", "a<br />b", "<b>bold</b> <sup>2</sup> <kbd>F9</kbd>", "x < y and y > z", "a <3 b",
                                 "<details>x</details>", "<details>\n<summary>S</summary>\nbody\n</details>", "<b>x\ny</b>", "a<b/>b",
                                 "a<b><i>x</i></b>z", "a<B>x</b>y", "a<b class=\"x\">y</b>z", "<https://example.com> <me@x.org>",
                                 "`<br>` and ``a <name> b``", "```\n<br> <name>\n```\n",
                                 "~~~\n<img src=x> QList<Span>\n~~~", "Code:\n\n    <br> <T>\n", "&lt;br> is written so",
                                 })
            QCOMPARE(htmlBalanced(same), QString(same));
        // Each of the re-check's cases, then a paragraph: kept.
        for (const char* md : {"a<br>b", "a<BR>b", "a<hr>b", "a<wbr>b", "a<img src=\"x.png\">b", "a<p>b", "a<li>item", "a<span>x", "a<u>x",
                               "a<b>unclosed bold", "vector<int> x", "a <T> b", "a<x>b", "a</b>b", "1. The dataset is <name>.dat.ngspice beside the schematic.\n2. still here?",
                               "| A | B |\n|---|---|\n| QList<Span> | y |", "| A | B |\n|---|---|\n| one<br>two | `x` |\n| row2 | y |",
                               "<!-- a comment with <b> inside -->", "<!-- note <name> here -->", "text <!-- c <b> --> more",
                               "<!--\nmulti <img src=x>\nline\n-->", "a<!-- c unclosed <b>x", "<code>a<br>b</code>", "<pre>\n<hr> <x>\n</pre>"}) {
            QTextDocument doc;
            doc.setMarkdown(htmlBalanced(QString::fromUtf8(md) + "\n\nNEXT paragraph **bold**\n"), QTextDocument::MarkdownDialectGitHub);
            QVERIFY2(doc.toPlainText().contains("NEXT paragraph bold"), qPrintable(QString(md) + " -> " + doc.toPlainText()));
        }

        // Each found so by the random replies below, before it held: an HTML
        // block (a line that starts with a block tag) - where a backslash is
        // no escape and each '<' counts -, in a quote or a list's item, ended
        // by the quote's or the item's end; tags in <code> and <pre>; inline
        // code no further than its paragraph, a heading's line, or a run of
        // backticks as long; a backtick fence's line with no other backtick;
        // a comment past an HTML block's end; a backslash at a line's end.
        for (const char* md : {"<hr><sup>", "<hr></details>", "`<T>\n\n`", "`</span>```", "<pre><code></pre>", "<pre></pre><details>",
                               "<!--<pre>", "<code><name></code>", "```<details>`", "```x```<p>", "<hr>\\<hr>", "> <hr>\\<details>",
                               "<p><pre>\\</pre>", "<p><code>`</code>`", "- <pre></pre>\\<br>", "> ```\n</details>```", "> <i>\n`</i>`",
                               "- ```\n</span>", "1. ```\n</b>", "- ~~~\n\n</x>", "`\n<p>`", "`\n<pre>`", "`\n<!--`\\<hr>", "<hr><x",
                               "<hr><!--\n\n<!---->", "\\\n<hr>\\</x>", "# `\n<name>`", "# ```<p>\n*```", "x```\n```</span>```",
                               "<pre></pre>\n    <b>\n</b>", "*\n    ```\n<br>", "<img src=\"a\">\n~~~<name>"}) {
            QTextDocument doc;
            doc.setMarkdown(htmlBalanced(QString::fromUtf8(md) + "\n\nEND OF IT\n"), QTextDocument::MarkdownDialectGitHub);
            QVERIFY2(doc.toPlainText().contains("END OF IT"), qPrintable(QString(md) + " -> " + htmlBalanced(QString::fromUtf8(md))));
        }
        // (A comment past an HTML block's end is taken out once: nothing of
        // what follows it is.)
        QCOMPARE(htmlBalanced("<hr><!--\n\n<!---->\n\nAfter."), QString("<hr/>\n\nAfter."));
        // (A fence ends the paragraph inline code is looked for in: what is in
        // the fence is code, left as it is.)
        QCOMPARE(htmlBalanced("`a\n~~~\n`<b>\n~~~\n"), QString("`a\n~~~\n`<b>\n~~~\n"));

        // Whatever a reply holds of these, a paragraph after it is read.
        {
            const char* const tokens[] = {"<b>", "</b>", "<i>", "</i>", "<br>", "<name>", "</x>", "<!--", "-->", "|", "\n", "\n\n",
                                          "- ", "# ", "> ", "`", "```", "\\", "x", " ", "<", ">", "<details>", "</details>", "<hr>",
                                          "*", "1. ", "<img src=\"a\">", "<p>", "<code>", "</code>", "<pre>", "</pre>", "<T>", "</br>",
                                          "<span>", "</span>", "$", "<sup>", "<br/>", "<b/>", "~~~", "    "};
            std::mt19937 rng(20261005);
            int lost = 0;
            QString first;
            for (int round = 0; round < 3000; ++round) {
                QString md;
                const int count = 1 + int(rng() % 24);
                for (int k = 0; k < count; ++k) md += QString::fromUtf8(tokens[rng() % std::size(tokens)]);
                QTextDocument doc;
                doc.setMarkdown(htmlBalanced(md + "\n\nEND OF IT\n"), QTextDocument::MarkdownDialectGitHub);
                if (!doc.toPlainText().contains("END OF IT")) {
                    if (first.isEmpty()) first = md;
                    ++lost;
                }
            }
            QVERIFY2(lost == 0, qPrintable(QString::number(lost) + " lost; the first: " + first));
        }

        // Across a list's items and a table's cells: each keeps its own text.
        {
            QTextDocument doc;
            doc.setMarkdown(htmlBalanced("- item one <b>opened\n- item two</b> closed\n\n| col <b>x | y</b> col |\n|---|---|\n"),
                            QTextDocument::MarkdownDialectGitHub);
            const QStringList blocks = [&] {
                QStringList b;
                for (QTextBlock it = doc.begin(); it.isValid(); it = it.next()) b << it.text();
                return b;
            }();
            QVERIFY2(blocks.contains("item one opened") && blocks.contains("item two closed") && blocks.contains("col x")
                         && blocks.contains("y col"),
                     qPrintable(blocks.join(" | ")));
        }

        ClaudeCodePanel panel;
        const QString work = fresh("brwork");
        panel.setDefaultDirectory(work);
        panel.resize(520, 800);
        panel.session()->setProgram("claude");
        const QString reply =
            "The Properties dialog now applies values on all three parts.\n\n"
            "| Part | What I changed | Netlist after OK |\n|---|---|---|\n"
            "| Transistor Q1 | Six values in one OK:<br>Bf 250<br>Vaf 75 | pnp<br>Bf=250 Vaf=75 Re=1.5<br>AREA=2 |\n"
            "| Op-amp U7 | its library<BR>and part | XU7 OpAmps_ua741 |\n"
            "| Subcircuit SUB1 | Rs and Cs, a QList<Span> | XSUB1 Rs=2K<br/>Cs=100N |\n\n"
            "After the table:\n\n- **Q1** kept its 51 property rows\n- `rc_sub.sch` reloaded, its pins as before\n"
            "- the dataset is <name>.dat.ngspice in Scratch/<sch>/\n- a path: `~/Desktop/check.md`\n- an item<hr>ruled off\n\n"
            "A rule<hr>then text, a picture <img src=\"x.png\"> here, a <wbr>break.\n\n"
            "Last paragraph, with `<br>` as code and a lone <b> tag.\n";
        panel.session()->handleLine(QJsonDocument(QJsonObject{
            {"type", "assistant"},
            {"message", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", reply}}}}}}}).toJson(QJsonDocument::Compact));
        panel.renderNow();
        const QString shown = panel.transcriptText();
        for (const char* kept : {"Transistor Q1", "Six values in one OK:", "Vaf=75", "Op-amp U7", "and part", "Subcircuit SUB1", "a QList<Span>",
                                 "Cs=100N", "After the table:", "kept its 51 property rows", "reloaded, its pins as before",
                                 "the dataset is <name>.dat.ngspice in Scratch/<sch>/", "a path:", "ruled off", "then text, a picture", "here, a",
                                 "Last paragraph, with", "<br> as code and a lone <b> tag."})
            QVERIFY2(shown.contains(QString::fromUtf8(kept)), qPrintable(QString(kept) + "\n---\n" + shown));
        // The cell: two lines in its block. The rule: its own block, empty,
        // between the paragraph's parts.
        bool twoLines = false, ruleBetween = false;
        QTextDocument* doc = panel.transcript()->document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            if (b.text().startsWith("pnp") && b.text().contains(QChar::LineSeparator) && QTextCursor(b).currentTable() != nullptr) twoLines = true;
            if (b.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth))
                ruleBetween = b.text().isEmpty() && b.previous().text() == "A rule" && b.next().text().startsWith("then text");
        }
        QVERIFY(twoLines);
        QVERIFY(ruleBetween);
        // Copied as text: every row on its line, nothing lost.
        QString error;
        const QString txt = QDir(work).filePath("br.txt");
        QVERIFY(panel.exportConversation(txt, ClaudeCodePanel::ExportFormat::Text, &error));
        QFile f(txt);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString copied = QString::fromUtf8(f.readAll());
        QVERIFY2(copied.contains("Transistor Q1 | Six values in one OK: Bf 250 Vaf 75 | pnp Bf=250 Vaf=75 Re=1.5 AREA=2"), qPrintable(copied));
        QVERIFY2(copied.contains("Op-amp U7 | its library and part | XU7 OpAmps_ua741"), qPrintable(copied));
        for (const char* kept : {"• Q1 kept its 51 property rows", "• rc_sub.sch reloaded", "• the dataset is <name>.dat.ngspice in Scratch/<sch>/",
                                 "ruled off", "A rule\n\n----\n\nthen text, a picture", "Last paragraph, with <br> as code and a lone <b> tag."})
            QVERIFY2(copied.contains(QString::fromUtf8(kept)), qPrintable(QString(kept) + "\n---\n" + copied));
        QVERIFY(!copied.contains(QChar::LineSeparator));
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

    // Kept on disk: saved, read back, listed the latest first, forgotten;
    // which are open; only the latest kept, not those open; ids that would
    // leave the folder refused.
    void theHistoryKeepsConversations()
    {
        namespace history = qucs_s::claude::history;
        QDir(history::directory()).removeRecursively();
        QVERIFY(history::directory().startsWith(dir.path()));   // (not the user's)
        QVERIFY(history::saved().isEmpty());
        QVERIFY(history::save("a1", {{"title", "First"}, {"sessionId", "s-a"}, {"folder", "/w"}, {"entries", QJsonArray{1}}}));
        QTest::qWait(5);
        QVERIFY(history::save("b2", {{"title", "Second"}, {"sessionId", "s-b"}, {"folder", "/w"}}));
        QList<history::Summary> all = history::saved();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all.at(0).id, QStringLiteral("b2"));   // the latest first
        QCOMPARE(all.at(1).title, QStringLiteral("First"));
        QCOMPARE(all.at(1).sessionId, QStringLiteral("s-a"));
        QCOMPARE(history::load("a1").value("entries").toArray().size(), 1);
        QVERIFY(!history::save("../evil", {}));
        QVERIFY(history::load("../evil").isEmpty());

        history::setOpen({"a1", "b2"}, "b2");
        QString current;
        QCOMPARE(history::open(&current), QStringList({"a1", "b2"}));
        QCOMPARE(current, QStringLiteral("b2"));
        // The latest two kept - and a1, open, whatever its age.
        QVERIFY(history::save("c3", {{"title", "Third"}}, 2));
        QVERIFY(history::save("d4", {{"title", "Fourth"}}, 2));
        QStringList ids;
        for (const history::Summary& s : history::saved()) ids << s.id;
        QVERIFY(ids.contains("a1") && ids.contains("d4") && ids.contains("c3"));
        QVERIFY(!ids.contains("b2") || ids.size() <= 4);
        history::remove("a1");
        QVERIFY(history::load("a1").isEmpty());
        QVERIFY(!history::open().contains("a1"));
        QDir(history::directory()).removeRecursively();
    }

    // A session to continue: the next start carries --resume; when Claude
    // Code no longer has it, the prompt goes to a new conversation and a
    // notice says so. The program's commands, less its terminal's, are
    // kept from its start.
    void aSessionIsResumedOrBegunAfresh()
    {
        skipWithoutShell();
        QFile::remove(dir.filePath("resumer-args"));
        QFile::remove(dir.filePath("resumer-prompts"));
        Session s;
        s.setProgram(script("resumer", kResumer));
        s.setWorkingDirectory(fresh("resumework1"));
        QSignalSpy notices(&s, &Session::notice);
        QSignalSpy turns(&s, &Session::turnFinished);
        QSignalSpy failures(&s, &Session::failed);
        s.resume("gone-1");
        QCOMPARE(s.sessionId(), QStringLiteral("gone-1"));
        QVERIFY(s.send("Hello again"));
        QVERIFY(turns.wait(10000));
        QVERIFY(turns.last().at(0).value<TurnResult>().ok);
        QCOMPARE(failures.count(), 0);
        bool told = false;
        for (const QList<QVariant>& n : notices) told = told || n.at(0).toString().contains("no longer has");
        QVERIFY(told);
        const QString args = read(dir.filePath("resumer-args"));
        QVERIFY(args.contains("--resume\ngone-1"));   // tried first...
        QCOMPARE(args.count("--resume"), 1);            // ...then without
        QCOMPARE(read(dir.filePath("resumer-prompts")).count("Hello again"), 1);
        QCOMPARE(s.sessionId(), QStringLiteral("fresh-1"));
        QCOMPARE(s.slashCommands(), QStringList({"compact", "context", "my-skill"}));   // (doctor: its terminal's)
        s.stop();
        // One it has: continued, no notice.
        notices.clear();
        s.resume("fresh-1");
        QVERIFY(s.send("And more"));
        QVERIFY(turns.wait(10000));
        QVERIFY(read(dir.filePath("resumer-args")).contains("--resume\nfresh-1"));
        s.stop();
    }

    // The command line of a prompt edited: the session continued up to a
    // message, as a new one - only when a session is continued. The last
    // message of the conversation, the program's own (not a subagent's).
    void aRewindIsOnTheCommandLine()
    {
        qucs_s::claude::Options o;
        o.resume = "s-1";
        o.resumeAt = "m1";
        o.fork = true;
        const QStringList args = qucs_s::claude::arguments(o);
        const qsizetype at = args.indexOf("--resume");
        QVERIFY(at >= 0);
        QCOMPARE(args.mid(at, 5), QStringList({"--resume", "s-1", "--resume-session-at", "m1", "--fork-session"}));
        o.resume.clear();
        QVERIFY(!qucs_s::claude::arguments(o).contains("--resume-session-at") && !qucs_s::claude::arguments(o).contains("--fork-session"));

        Session s;
        QCOMPARE(s.lastMessageId(), QString());
        s.handleLine(R"({"type":"assistant","uuid":"u1","message":{"content":[{"type":"text","text":"Hi."}]}})");
        QCOMPARE(s.lastMessageId(), QStringLiteral("u1"));
        s.handleLine(R"({"type":"assistant","uuid":"u2","parent_tool_use_id":"t9","message":{"content":[{"type":"text","text":"sub"}]}})");
        QCOMPARE(s.lastMessageId(), QStringLiteral("u1"));   // (a subagent's: its own transcript)
        s.handleLine(R"({"type":"user","uuid":"u3","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"x"}]}})");
        QCOMPARE(s.lastMessageId(), QStringLiteral("u3"));
        s.resume("other");
        QCOMPARE(s.lastMessageId(), QString());   // (not known until said)
    }

    // A prompt edited and sent again (it could be stopped, not changed):
    // Edit beside it sets it and all after it aside and puts it in the
    // composer; Cancel (or Esc) puts all back. Sent, the conversation goes
    // back to where it stood before it - Claude Code started on the session
    // up to the message before it, as a new session - and goes on from the
    // edited prompt. The first prompt: a new conversation. A turn under way
    // is stopped for it. Where the program cannot go back, all is as it
    // was, and the edited prompt is in the box again.
    void aPromptIsEditedAndSentAgain()
    {
        skipWithoutShell();
        QFile::remove(dir.filePath("rewinder-starts"));
        QFile::remove(dir.filePath("rewinder-prompts"));
        const QString work = fresh("rewindwork");
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(work);
        panel.session()->setProgram(script("rewinder", kRewinder));
        QSignalSpy turns(panel.session(), &Session::turnFinished);
        const auto say = [&](const QString& text) {
            panel.composer()->setPlainText(text);
            panel.sendComposer();
            return turns.wait(10000);
        };
        // The prompts, by their place in the conversation.
        const auto prompts = [&panel] {
            QList<QPair<qsizetype, QJsonObject>> list;
            const QJsonArray entries = panel.conversationJson().value("entries").toArray();
            for (qsizetype i = 0; i < entries.size(); ++i)
                if (entries.at(i).toObject().value("kind").toInt() == 0) list << qMakePair(i, entries.at(i).toObject());
            return list;
        };
        const auto lastStart = [this] { return read(dir.filePath("rewinder-starts")).split('\n', Qt::SkipEmptyParts).last(); };
        QVERIFY(say("one"));
        QVERIFY(say("two"));
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("Reply 2."));
        // Where each stood: the first before anything, the second after m1.
        auto list = prompts();
        QCOMPARE(list.size(), 2);
        QVERIFY(list.at(0).second.contains("at") && list.at(0).second.value("at").toString().isEmpty());
        QCOMPARE(list.at(1).second.value("session").toString(), QStringLiteral("s-1"));
        QCOMPARE(list.at(1).second.value("at").toString(), QStringLiteral("m1"));
        const qsizetype two = list.at(1).first;
        // An Edit beside each prompt, not in an export.
        const QString html = panel.transcript()->toHtml();
        QVERIFY2(html.contains("href=\"edit:0\"") && html.contains(QStringLiteral("href=\"edit:%1\"").arg(two)), qPrintable(html.left(2000)));
        QVERIFY(!panel.conversationMarkdown().contains("Edit"));

        // For a look: QUCS_TEST_GRAB=<dir> saves the dock, then editing.
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (!grabDir.isEmpty()) {
            panel.resize(420, 520);
            panel.show();
            panel.renderNow();
            QTest::qWait(50);
            panel.grab().save(grabDir + "/claude-edit-links.png");
        }
        // Edited, a draft in the composer: set aside, and back on Cancel.
        panel.composer()->setPlainText("a draft");
        QVERIFY(panel.editPrompt(two));
        QVERIFY(panel.isEditing() && panel.editBar()->isVisibleTo(&panel));
        if (!grabDir.isEmpty()) {
            QTest::qWait(50);
            panel.grab().save(grabDir + "/claude-editing.png");
        }
        QCOMPARE(panel.composer()->toPlainText(), QStringLiteral("two"));
        panel.renderNow();
        QVERIFY(!panel.transcriptText().contains("Reply 2.") && panel.transcriptText().contains("Reply 1."));
        QCOMPARE(prompts().size(), 2);   // (kept as it is until sent)
        panel.cancelEdit();
        QVERIFY(!panel.isEditing() && !panel.editBar()->isVisibleTo(&panel));
        QCOMPARE(panel.composer()->toPlainText(), QStringLiteral("a draft"));
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("Reply 2."));
        QVERIFY(panel.editPrompt(two));
        QTest::keyClick(panel.composer(), Qt::Key_Escape);
        QVERIFY(!panel.isEditing());
        QCOMPARE(panel.composer()->toPlainText(), QStringLiteral("a draft"));

        // Sent: the program again, on s-1 up to m1, as a new session.
        QVERIFY(panel.editPrompt(two));
        panel.composer()->setPlainText("TWO, edited");
        panel.sendComposer();
        QVERIFY(turns.wait(10000));
        QVERIFY2(lastStart().contains("--resume s-1 --resume-session-at m1 --fork-session"), qPrintable(lastStart()));
        QVERIFY(panel.session()->sessionId().startsWith("forked-"));
        QVERIFY(!panel.isEditing() && !panel.editBar()->isVisibleTo(&panel));
        panel.renderNow();
        QString text = panel.transcriptText();
        QVERIFY2(text.contains("Reply 1.") && text.contains("TWO, edited") && text.contains("Reply 3.") && !text.contains("Reply 2."),
                 qPrintable(text));
        list = prompts();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(1).second.value("text").toString(), QStringLiteral("TWO, edited"));
        QCOMPARE(list.at(1).second.value("at").toString(), QStringLiteral("m1"));   // (where it stands: where the one it replaced stood)
        // The next prompt stands after the forked session's last message.
        QVERIFY(say("three"));
        QCOMPARE(prompts().at(2).second.value("at").toString(), QStringLiteral("m3"));
        QVERIFY(prompts().at(2).second.value("session").toString().startsWith("forked-"));

        // The first prompt edited: a new conversation, nothing continued.
        QVERIFY(panel.editPrompt(0));
        panel.composer()->setPlainText("ONE again");
        panel.sendComposer();
        QVERIFY(turns.wait(10000));
        QVERIFY2(!lastStart().contains("--resume"), qPrintable(lastStart()));
        panel.renderNow();
        text = panel.transcriptText();
        QVERIFY2(text.contains("ONE again") && text.contains("Reply 5.") && !text.contains("Reply 1.") && !text.contains("three"),
                 qPrintable(text));
        QCOMPARE(prompts().size(), 1);

        // A turn under way: stopped for the edit; Cancel puts it back, stopped.
        panel.composer()->setPlainText("slow one");
        panel.sendComposer();
        QTRY_VERIFY_WITH_TIMEOUT(panel.session()->isBusy() && panel.transcriptText().contains("sleep 100"), 10000);
        const qsizetype slow = prompts().last().first;
        QVERIFY(panel.editPrompt(slow));
        QVERIFY(!panel.session()->isBusy());
        panel.cancelEdit();
        const QJsonArray kept = panel.conversationJson().value("entries").toArray();
        QCOMPARE(kept.last().toObject().value("kind").toInt(), 5);   // Summary
        QCOMPARE(kept.last().toObject().value("text").toString(), QStringLiteral("Stopped"));
        for (const QJsonValue& v : kept)
            if (v.toObject().value("kind").toInt() == 2) QVERIFY(v.toObject().value("tool").toInt() != 0);   // (none running)

        // Where the program cannot go back: all as it was, the prompt in the box.
        panel.restoreConversation({{"sessionId", "s-1"}, {"folder", work}, {"entries", QJsonArray{
            QJsonObject{{"kind", 0}, {"text", "first"}, {"session", ""}, {"at", ""}}, QJsonObject{{"kind", 1}, {"text", "Reply first."}},
            QJsonObject{{"kind", 0}, {"text", "second"}, {"session", "s-1"}, {"at", "bad-point"}},
            QJsonObject{{"kind", 1}, {"text", "Reply second."}}}}});
        QSignalSpy refused(panel.session(), &Session::rewindFailed);
        QVERIFY(panel.editPrompt(2));
        panel.composer()->setPlainText("second, edited");
        panel.sendComposer();
        QVERIFY(refused.wait(10000));
        panel.renderNow();
        text = panel.transcriptText();
        QVERIFY2(text.contains("Reply second.") && text.contains("could not go back to before that prompt")
                     && text.contains("No message found with message.uuid of: bad-point") && !text.contains("second, edited"),
                 qPrintable(text));
        QCOMPARE(panel.composer()->toPlainText(), QStringLiteral("second, edited"));
        QCOMPARE(panel.session()->sessionId(), QStringLiteral("s-1"));
        QVERIFY(!panel.isEditing() && panel.canEdit(2));
        panel.session()->stop();
    }

    // Where the prompts of one of Claude Code's own sessions stood, from
    // its file: each the nearest message before it (not an attachment), a
    // command as typed, not a subagent's or a meta line; its last message.
    // Brought back, its prompts can be edited, and the next goes on from
    // the last.
    void aSessionsPromptsKnowWhereTheyStood()
    {
        namespace history = qucs_s::claude::history;
        const QString work = fresh("pointswork");
        QString folderName = work;
        for (QChar& c : folderName)
            if (!(c.isLetterOrNumber() && c.unicode() < 128)) c = '-';
        const QString projects = history::claudeDirectory() + "/projects/" + folderName;
        QVERIFY(projects.startsWith(dir.path()));   // (not the user's)
        QDir().mkpath(projects);
        const QString id = "9o1n75-0000-2222";
        QFile file(projects + "/" + id + ".jsonl");
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto line = [&](const char* uuid, const char* parent, const char* type, const QJsonValue& content, bool meta = false,
                              bool side = false) {
            QJsonObject o{{"type", type}, {"uuid", uuid}, {"parentUuid", parent ? QJsonValue(parent) : QJsonValue()}, {"cwd", work},
                          {"sessionId", id}, {"message", QJsonObject{{"role", type}, {"content", content}}}};
            if (meta) o.insert("isMeta", true);
            if (side) o.insert("isSidechain", true);
            file.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
        };
        const QJsonArray said{QJsonObject{{"type", "text"}, {"text", "Said."}}};
        line("u1", nullptr, "user", "Explain the filter");
        line("a1", "u1", "attachment", QJsonValue());
        line("u2", "a1", "assistant", said);
        line("a2", "u2", "attachment", QJsonValue());
        line("u3", "a2", "user", "Next question");
        line("u4", "u3", "user", "meta stuff", true);
        line("s1", "u3", "assistant", said, false, true);
        line("u5", "u4", "assistant", said);
        line("u6", "u5", "user", "<command-name>/compact</command-name>\n<command-args>keep the math</command-args>");
        line("u7", "u6", "assistant", said);
        file.close();
        const history::SessionPoints points = history::sessionPoints(file.fileName());
        QCOMPARE(points.prompts.size(), 3);
        QCOMPARE(points.prompts.at(0), qMakePair(QStringLiteral("Explain the filter"), QString()));
        QCOMPARE(points.prompts.at(1), qMakePair(QStringLiteral("Next question"), QStringLiteral("u2")));
        QCOMPARE(points.prompts.at(2), qMakePair(QStringLiteral("/compact keep the math"), QStringLiteral("u5")));
        QCOMPARE(points.last, QStringLiteral("u7"));

        ClaudeCodePanel panel;
        panel.setDefaultDirectory(work);
        QVERIFY(panel.importClaudeSession(file.fileName()));
        QCOMPARE(panel.session()->lastMessageId(), QStringLiteral("u7"));
        QStringList at;
        const QJsonArray entries = panel.conversationJson().value("entries").toArray();
        for (qsizetype i = 0; i < entries.size(); ++i)
            if (entries.at(i).toObject().value("kind").toInt() == 0) {
                at << entries.at(i).toObject().value("at").toString();
                QVERIFY(panel.canEdit(i));
            }
        QCOMPARE(at, QStringList({"", "u2", "u5"}));
        // Kept before prompts knew where they stood: placed from the file.
        panel.restoreConversation({{"sessionId", id}, {"folder", work}, {"entries", QJsonArray{
            QJsonObject{{"kind", 0}, {"text", "Explain the filter"}}, QJsonObject{{"kind", 1}, {"text", "Said."}},
            QJsonObject{{"kind", 0}, {"text", "Next question"}}}}});
        QVERIFY(panel.canEdit(0) && panel.canEdit(2));
        QCOMPARE(panel.conversationJson().value("entries").toArray().at(2).toObject().value("at").toString(), QStringLiteral("u2"));
        QCOMPARE(panel.session()->lastMessageId(), QStringLiteral("u7"));
        // One whose file is gone: its prompts not placed, no Edit.
        panel.restoreConversation({{"sessionId", "gone-9"}, {"folder", work}, {"entries", QJsonArray{
            QJsonObject{{"kind", 0}, {"text", "Old prompt"}}}}});
        QVERIFY(!panel.canEdit(0));
        QVERIFY(file.remove());
    }

    // A conversation kept and brought back: what was said, its name, its
    // folder, its pin, its session (continued from the next prompt); a
    // tool that had not finished is said not to have. The tabs keep them
    // as they change and which are open; a new set of tabs brings those
    // back (unless told not to); /quit closes one, which stays kept.
    void aConversationIsKeptAndBroughtBack()
    {
        namespace history = qucs_s::claude::history;
        QDir(history::directory()).removeRecursively();
        const QString work = fresh("keptwork");
        const QString schematic = work + "/amp.sch";
        QFile(schematic).open(QIODevice::WriteOnly);
        {
            ClaudeCodePanel panel;
            panel.setDefaultDirectory(work);
            panel.restoreConversation({{"name", "Amplifier"}, {"folder", work}, {"pinned", schematic}, {"sessionId", "s-kept"},
                                       {"entries", QJsonArray{QJsonObject{{"kind", 0}, {"text", "Check the gain"}},
                                                              QJsonObject{{"kind", 1}, {"text", "It is **20 dB**."}},
                                                              QJsonObject{{"kind", 2}, {"text", "Bash"}, {"extra", "ls"}, {"id", "t1"}, {"tool", 0}},
                                                              QJsonObject{{"kind", 9}, {"text", "junk"}}}}});
            QCOMPARE(panel.title(), QStringLiteral("Amplifier"));
            QVERIFY(panel.isPinnedTo(schematic));
            QCOMPARE(panel.session()->sessionId(), QStringLiteral("s-kept"));
            QCOMPARE(panel.workingDirectory(), work);
            const QString md = panel.conversationMarkdown();
            QVERIFY(md.contains("Check the gain"));
            QVERIFY(md.contains("It is **20 dB**."));
            QVERIFY(md.contains("- ✕ **Bash** `ls`"));
            QVERIFY(md.contains("It had not finished"));
            QVERIFY(!md.contains("junk"));
            // As kept: the same again.
            const QJsonObject kept = panel.conversationJson();
            QCOMPARE(kept.value("title").toString(), QStringLiteral("Amplifier"));
            QCOMPARE(kept.value("sessionId").toString(), QStringLiteral("s-kept"));
            QCOMPARE(kept.value("entries").toArray().size(), 3);
        }

        QString first;
        {
            ClaudeCodeTabs tabs;
            tabs.setDefaultDirectory(work);
            ClaudeCodePanel* panel = tabs.current();
            panel->session()->setProgram(dir.filePath("no-such-claude"));   // (the prompt stays; the turn fails)
            panel->composer()->setPlainText("Explain the filter");
            panel->sendComposer();
            panel->setName("Filter");
            tabs.saveConversations();
            first = panel->conversationId();
            QVERIFY(!first.isEmpty());
            QCOMPARE(history::open(), QStringList{first});
            QCOMPARE(history::saved().first().title, QStringLiteral("Filter"));
            // A second, in front; the empty third not kept.
            ClaudeCodePanel* second = tabs.newConversation();
            second->session()->setProgram(dir.filePath("no-such-claude"));
            second->composer()->setPlainText("Size the capacitor");
            second->sendComposer();
            tabs.newConversation();
            tabs.showConversation(second);
            QTRY_COMPARE(history::open().size(), 2);   // (a moment later)
        }   // (closed: kept)
        {
            ClaudeCodeTabs again;
            again.setDefaultDirectory(work);
            QVERIFY(again.restoreConversations());
            QCOMPARE(again.count(), 2);
            QCOMPARE(again.panels().at(0)->title(), QStringLiteral("Filter"));
            QCOMPARE(again.panels().at(0)->conversationId(), first);
            QCOMPARE(again.current(), again.panels().at(1));   // the one in front
            again.panels().at(0)->renderNow();
            QVERIFY(again.panels().at(0)->transcriptText().contains("Explain the filter"));
            // /quit: closed, and kept to resume.
            ClaudeCodePanel* quitting = again.panels().at(0);
            quitting->composer()->setPlainText("/quit");
            quitting->sendComposer();
            QTRY_COMPARE(again.count(), 1);
            QVERIFY(!history::load(first).isEmpty());
            QCOMPARE(history::open().size(), 1);
            // /resume with its id: back.
            ClaudeCodePanel* front = again.current();
            front->composer()->setPlainText("/resume " + first);
            front->sendComposer();
            QTRY_COMPARE(again.count(), 2);
            QCOMPARE(again.current()->conversationId(), first);
            QCOMPARE(again.current()->title(), QStringLiteral("Filter"));
        }
        // Not reopened when told not to.
        qucs_s::claude::history::setReopenAtStart(false);
        {
            ClaudeCodeTabs none;
            QVERIFY(!none.restoreConversations());
            QCOMPARE(none.count(), 1);
            QVERIFY(!none.current()->hasConversation());
        }
        qucs_s::claude::history::setReopenAtStart(true);
        QDir(history::directory()).removeRecursively();
    }

    // What each turn took is shown as chosen (⋯ > Show Usage), in every
    // conversation at once, in its exports and in /status; kept with the
    // conversation, its totals going on from there. A conversation kept
    // before, whose lines hold their costs as text, shows them as chosen
    // too.
    void usageIsShownAsChosen()
    {
        using qucs_s::claude::TokenUsage;
        {
            QucsSettingsFile settings;
            for (const char* key : {"ClaudeCode/showPromptTokens", "ClaudeCode/showConversationTokens",
                                    "ClaudeCode/showPromptCost", "ClaudeCode/showConversationCost"})
                settings.remove(QLatin1String(key));
        }
        QCOMPARE(ClaudeCodePanel::usageShown(), 0);   // none of it at first
        const QString work = fresh("usagework");
        const QJsonObject tokens{{"input", 2000}, {"output", 1500}, {"cacheRead", 40000}, {"cacheWrite", 3000}};
        const QJsonObject allTokens{{"input", 5000}, {"output", 4000}, {"cacheRead", 300000}, {"cacheWrite", 9000}};
        const QJsonObject conversation{
            {"folder", work},
            {"entries", QJsonArray{QJsonObject{{"kind", 0}, {"text", "Size R1"}},
                                   QJsonObject{{"kind", 1}, {"text", "R1 is 4.7k."}},
                                   // Kept before: its costs are text.
                                   QJsonObject{{"kind", 5}, {"text", "Done in 3.0 s  ·  $0.021  ·  $0.150 in all  ·  4 steps"}},
                                   QJsonObject{{"kind", 0}, {"text", "And R2?"}},
                                   QJsonObject{{"kind", 5}, {"text", "Done in 2.0 s"}, {"result", "changed amp.sch"},
                                               {"tokens", tokens}, {"allTokens", allTokens}, {"cost", 0.046}, {"allCost", 0.196}}}}};
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(work);
        panel.restoreConversation(conversation);
        ClaudeCodePanel other;
        other.setDefaultDirectory(work);
        other.restoreConversation(conversation);
        // Its totals go on from its last turn's.
        QCOMPARE(panel.session()->conversationTokens().total(), qint64(318000));
        QVERIFY(qAbs(panel.session()->conversationCost() - 0.196) < 1e-9);

        // At first the line says how the turn went, alone - the one kept
        // before too, its costs as text taken out.
        panel.renderNow();
        QString text = panel.transcriptText();
        QVERIFY2(text.contains("Done in 3.0 s  ·  4 steps"), qPrintable(text));
        QVERIFY(text.contains("Done in 2.0 s  ·  changed amp.sch"));
        QVERIFY(!text.contains("tokens"));
        QVERIFY(!text.contains("$"));
        QVERIFY(!panel.conversationMarkdown().contains("$"));
        QVERIFY(!panel.conversationMarkdown().contains("tokens"));
        QVERIFY(!panel.conversationMarkdown().contains("- **Tokens:**"));

        // Chosen in the menu of one conversation: shown in both, the
        // other drawn again.
        other.renderNow();
        QVERIFY(!other.transcriptText().contains("tokens"));
        auto* promptCost = panel.findChild<QAction*>(QStringLiteral("claudeShowPromptCost"));
        auto* allCost = panel.findChild<QAction*>(QStringLiteral("claudeShowConversationCost"));
        auto* promptTokens = panel.findChild<QAction*>(QStringLiteral("claudeShowPromptTokens"));
        auto* allTokens_ = panel.findChild<QAction*>(QStringLiteral("claudeShowConversationTokens"));
        QVERIFY(promptCost && allCost && promptTokens && allTokens_);
        for (const QAction* a : {promptCost, allCost, promptTokens, allTokens_}) QVERIFY(!a->isChecked());
        promptTokens->trigger();
        allTokens_->trigger();
        QCOMPARE(ClaudeCodePanel::usageShown(), int(ClaudeCodePanel::PromptTokens | ClaudeCodePanel::ConversationTokens));
        QTRY_VERIFY(other.transcriptText().contains("Done in 2.0 s  ·  46.5k tokens  ·  318k tokens in all  ·  changed amp.sch"));
        QVERIFY(!other.transcriptText().contains("$"));
        QVERIFY(panel.conversationMarkdown().contains("*Done in 2.0 s  ·  46.5k tokens  ·  318k tokens in all  ·  changed amp.sch*"));
        QVERIFY(panel.conversationMarkdown().contains("- **Tokens:** "));
        QVERIFY(!panel.conversationMarkdown().contains("- **Cost:**"));
        QVERIFY(panel.conversationText().contains("(Done in 2.0 s  ·  46.5k tokens  ·  318k tokens in all  ·  changed amp.sch)"));
        promptCost->trigger();
        allCost->trigger();
        promptTokens->trigger();
        QCOMPARE(ClaudeCodePanel::usageShown(),
                 int(ClaudeCodePanel::ConversationTokens | ClaudeCodePanel::PromptCost | ClaudeCodePanel::ConversationCost));
        QTRY_VERIFY(other.transcriptText().contains("Done in 2.0 s  ·  318k tokens in all  ·  $0.046  ·  $0.196 in all  ·  changed amp.sch"));
        // The line kept before shows its costs as the others.
        QVERIFY(other.transcriptText().contains("Done in 3.0 s  ·  $0.021  ·  $0.150 in all  ·  4 steps"));
        QVERIFY(other.conversationMarkdown().contains("- **Cost:** $0.196"));
        other.composer()->setPlainText("/status");
        other.sendComposer();
        other.renderNow();
        QVERIFY(other.transcriptText().contains("Cost: $0.196"));
        QVERIFY(other.transcriptText().contains(QLocale().toString(318000) + " tokens"));

        // Kept with its numbers, and read back as they were.
        const QJsonArray kept = panel.conversationJson().value("entries").toArray();
        const QJsonObject last = kept.last().toObject();
        QCOMPARE(last.value("tokens").toObject(), tokens);
        QCOMPARE(last.value("allCost").toDouble(), 0.196);
        const QJsonObject before = kept.at(2).toObject();
        QCOMPARE(before.value("text").toString(), QStringLiteral("Done in 3.0 s"));
        QCOMPARE(before.value("result").toString(), QStringLiteral("4 steps"));
        QCOMPARE(before.value("cost").toDouble(), 0.021);
        QCOMPARE(before.value("allCost").toDouble(), 0.150);

        // Nothing of it: the line says how the turn went, alone.
        ClaudeCodePanel::setUsageShown(0);
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("Done in 2.0 s  ·  changed amp.sch"));
        QVERIFY(!panel.transcriptText().contains("tokens"));
    }

    // Claude Code's own sessions of the folder: listed (a title given with
    // /rename, else the first prompt), brought back from their file - the
    // prompts, replies and tools; what it adds itself, a subagent's, left
    // out - and continued; /resume with a session's id goes on with it; the
    // list filters, and forgets only the dock's own.
    void claudeCodesOwnSessionsAreResumed()
    {
        namespace history = qucs_s::claude::history;
        QDir(history::directory()).removeRecursively();
        const QString work = fresh("claudesessions");
        QString folderName = work;
        for (QChar& c : folderName)
            if (!(c.isLetterOrNumber() && c.unicode() < 128)) c = '-';
        const QString projects = history::claudeDirectory() + "/projects/" + folderName;
        QVERIFY(projects.startsWith(dir.path()));   // (not the user's)
        QDir().mkpath(projects);
        const QString id = "5e55ion-0000-1111";
        QFile file(projects + "/" + id + ".jsonl");
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto line = [&](const QJsonObject& o) { file.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n"); };
        const auto msg = [](const QJsonValue& content) { return QJsonObject{{"role", "user"}, {"content", content}}; };
        line({{"type", "user"}, {"cwd", work}, {"sessionId", id}, {"message", msg("Explain the filter")}});
        line({{"type", "user"}, {"isMeta", true}, {"cwd", work}, {"message", msg("meta stuff")}});
        line({{"type", "assistant"}, {"cwd", work}, {"message", QJsonObject{{"content", QJsonArray{
             QJsonObject{{"type", "thinking"}, {"thinking", "hmm"}}, QJsonObject{{"type", "text"}, {"text", "It is a **low-pass**."}}}}}}});
        line({{"type", "assistant"}, {"cwd", work}, {"message", QJsonObject{{"content", QJsonArray{
             QJsonObject{{"type", "tool_use"}, {"id", "tu1"}, {"name", "Bash"}, {"input", QJsonObject{{"command", "ls"}}}}}}}}});
        line({{"type", "user"}, {"cwd", work}, {"message", msg(QJsonArray{QJsonObject{{"type", "tool_result"}, {"tool_use_id", "tu1"}, {"content", "a.sch\nb.sch"}}})}});
        line({{"type", "assistant"}, {"cwd", work}, {"message", QJsonObject{{"content", QJsonArray{
             QJsonObject{{"type", "tool_use"}, {"id", "tu2"}, {"name", "Bash"}, {"input", QJsonObject{{"command", "cat x"}}}}}}}}});
        line({{"type", "user"}, {"cwd", work}, {"message", msg(QJsonArray{QJsonObject{{"type", "tool_result"}, {"tool_use_id", "tu2"}, {"is_error", true},
             {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "no such file\nmore"}}}}}})}});
        line({{"type", "user"}, {"cwd", work}, {"message", msg("<command-name>/compact</command-name>\n<command-message>compact</command-message>\n<command-args>keep the math</command-args>")}});
        line({{"type", "user"}, {"cwd", work}, {"message", msg("<local-command-stdout>Compacted</local-command-stdout>")}});
        line({{"type", "assistant"}, {"isSidechain", true}, {"cwd", work}, {"message", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "subagent says"}}}}}}});
        line({{"type", "custom-title"}, {"customTitle", "Filter talk"}, {"sessionId", id}});
        file.close();

        const QList<history::Summary> sessions = history::claudeSessions(work);
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions.first().title, QStringLiteral("Filter talk"));
        QCOMPARE(sessions.first().sessionId, id);
        QCOMPARE(sessions.first().folder, work);
        QCOMPARE(history::claudeSessionFile(id), file.fileName());
        QVERIFY(history::claudeSessionFile("no-such-session").isEmpty());

#ifndef Q_OS_WIN
        // The folder through a link (a workspace on another disk; /tmp and
        // /var on macOS): Claude Code files its sessions under the real
        // path, and they are found (bug hunt 2026-09-26, E4). One filed
        // under the path as spelled is found too - the newest first.
        const QString link = dir.filePath("claudesessions-link");
        QFile::remove(link);
        QVERIFY(QFile::link(work, link));
        QList<history::Summary> viaLink = history::claudeSessions(link);
        QCOMPARE(viaLink.size(), 1);
        QCOMPARE(viaLink.first().sessionId, id);
        QString spelled = QDir::cleanPath(QFileInfo(link).absoluteFilePath());
        for (QChar& c : spelled)
            if (!(c.isLetterOrNumber() && c.unicode() < 128)) c = '-';
        QVERIFY(spelled != folderName);
        QDir().mkpath(history::claudeDirectory() + "/projects/" + spelled);
        QFile later(history::claudeDirectory() + "/projects/" + spelled + "/1a7e5e55-2222.jsonl");
        QVERIFY(later.open(QIODevice::WriteOnly));
        later.write(QJsonDocument(QJsonObject{{"type", "user"}, {"cwd", link}, {"sessionId", "1a7e5e55-2222"}, {"message", msg("Later talk")}})
                        .toJson(QJsonDocument::Compact) + "\n");
        later.close();
        QVERIFY(later.open(QIODevice::ReadWrite));
        QVERIFY(later.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
        later.close();
        viaLink = history::claudeSessions(link);
        QCOMPARE(viaLink.size(), 2);
        QCOMPARE(viaLink.first().sessionId, QStringLiteral("1a7e5e55-2222"));
        QCOMPARE(viaLink.last().sessionId, id);
        QVERIFY(QFile::remove(later.fileName()));
        QFile::remove(link);
#endif

        ClaudeCodeTabs tabs;
        tabs.setDefaultDirectory(work);
        QVERIFY(history::save("kept1", {{"title", "Kept one"}, {"folder", work}, {"sessionId", "s-k"},
                                        {"entries", QJsonArray{QJsonObject{{"kind", 0}, {"text", "Old prompt"}}}}}));
        const QList<history::Summary> all = tabs.resumable(work);
        QCOMPARE(all.size(), 2);
        QCOMPARE(all.at(0).id, QStringLiteral("kept1"));
        QCOMPARE(all.at(1).claudeFile, file.fileName());

        // The list: filtered; the dock's one forgotten; Claude Code's kept.
        QScopedPointer<QDialog> dialog(tabs.resumeDialog(work, QString()));
        auto* list = dialog->findChild<QTreeWidget*>("claudeResumeList");
        auto* filter = dialog->findChild<QLineEdit*>("claudeResumeFilter");
        auto* forget = dialog->findChild<QPushButton*>("claudeResumeDelete");
        QVERIFY(list && filter && forget);
        QCOMPARE(list->topLevelItemCount(), 2);
        filter->setText("filter talk");
        QVERIFY(list->topLevelItem(0)->isHidden());
        QVERIFY(!list->topLevelItem(1)->isHidden());
        QVERIFY(!forget->isEnabled());   // (Claude Code's own)
        filter->clear();
        list->setCurrentItem(list->topLevelItem(0));
        QVERIFY(forget->isEnabled());
        forget->click();
        QCOMPARE(list->topLevelItemCount(), 1);
        QVERIFY(history::load("kept1").isEmpty());

        // Brought back from its file, and continued.
        ClaudeCodePanel* panel = tabs.resume(sessions.first());
        QVERIFY(panel != nullptr);
        QCOMPARE(panel->title(), QStringLiteral("Filter talk"));
        QCOMPARE(panel->session()->sessionId(), id);
        QCOMPARE(panel->workingDirectory(), work);
        const QString md = panel->conversationMarkdown();
        QVERIFY(md.contains("> Explain the filter"));
        QVERIFY(md.contains("It is a **low-pass**."));
        QVERIFY(md.contains("- ✓ **Bash** `ls`"));
        QVERIFY(md.contains("a.sch"));
        QVERIFY(md.contains("- ✕ **Bash** `cat x`"));
        QVERIFY(md.contains("Failed: no such file"));
        QVERIFY(md.contains("> /compact keep the math"));
        QVERIFY(!md.contains("meta stuff"));
        QVERIFY(!md.contains("subagent says"));
        QVERIFY(!md.contains("Compacted"));
        QVERIFY(!md.contains("hmm"));
        // Open already: the same, in front.
        QCOMPARE(tabs.resume(sessions.first()), panel);
        const int tabsNow = tabs.count();
        // /resume with its id: that one (open: in front).
        tabs.newConversation()->composer()->setPlainText("/resume " + id);
        tabs.current()->sendComposer();
        QTRY_COMPARE(tabs.current(), panel);
        QCOMPARE(tabs.count(), tabsNow + 1);
        QDir(history::directory()).removeRecursively();
    }

    // Slash commands: the dock's own run here (never sent to Claude), Claude
    // Code's sent as typed - no note of the document, which would be taken
    // for arguments; a path is no command. Typing "/" offers them: the
    // arrows choose, Tab puts one in, Enter runs it, Esc puts them away.
    void slashCommandsAreUnderstood()
    {
        QucsSettingsFile().remove("ClaudeCode/slashCommands");
        const QString work = fresh("slashwork");
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(work);
        panel.setDocumentProvider([work] { return work + "/amp.sch"; });
        panel.resize(440, 700);
        panel.show();
        QSignalSpy closing(&panel, &ClaudeCodePanel::closeRequested);
        QSignalSpy resuming(&panel, &ClaudeCodePanel::resumeRequested);
        QSignalSpy ending(&panel, &ClaudeCodePanel::conversationEnding);

        // The dock's.
        QVERIFY(panel.runCommand("/help"));
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("/resume"));
        QVERIFY(panel.transcriptText().contains("/compact"));   // Claude Code's, named
        QVERIFY(panel.runCommand("/status"));
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("not started yet"));
        QVERIFY(panel.runCommand("/rename Filter work"));
        QCOMPARE(panel.title(), QStringLiteral("Filter work"));
        QVERIFY(panel.runCommand("/permissions plan"));
        bool plan = false;
        for (QAction* a : panel.permissionActions()) plan = plan || (a->isChecked() && a->data().toString() == "plan");
        QVERIFY(plan);
        QVERIFY(panel.runCommand("/permissions ask"));
        QString some;
        for (QAction* a : panel.modelActions())
            if (!a->data().toString().isEmpty() && some.isEmpty()) some = a->data().toString();
        QVERIFY(!some.isEmpty());
        QVERIFY(panel.runCommand("/model " + some));
        QCOMPARE(panel.session()->model(), some);
        QVERIFY(panel.runCommand("/quit"));
        QCOMPARE(closing.count(), 1);
        QVERIFY(panel.runCommand("/resume filter"));
        QCOMPARE(resuming.last().at(0).toString(), QStringLiteral("filter"));
        QVERIFY(panel.runCommand("/clear"));
        QVERIFY(ending.count() >= 1);
        QVERIFY(!panel.hasConversation());
        QCOMPARE(panel.title(), QStringLiteral("New conversation"));
        // Not the dock's: Claude Code's, or none.
        QVERIFY(!panel.runCommand("/compact keep the math"));
        QVERIFY(!panel.runCommand("/Users/me/amp.sch explain it"));
        QVERIFY(!panel.runCommand("plain words"));

#ifndef Q_OS_WIN   // (the fake claude is a shell script)
        QFile::remove(dir.filePath("prompts"));
        panel.session()->setProgram(script("writer", kWriter));
        panel.composer()->setPlainText("/compact keep the math");
        panel.sendComposer();
        QTRY_VERIFY_WITH_TIMEOUT(read(dir.filePath("prompts")).contains("keep the math"), 10000);
        QVERIFY(!read(dir.filePath("prompts")).contains("The document open in Qucs-S"));
        panel.session()->stop();
        QTRY_VERIFY(!panel.session()->isBusy());
        panel.session()->reset();
        QFile::remove(dir.filePath("prompts"));
        panel.composer()->setPlainText("/Users/me/amp.sch explain it");
        panel.sendComposer();
        QTRY_VERIFY_WITH_TIMEOUT(read(dir.filePath("prompts")).contains("explain it"), 10000);
        QVERIFY(read(dir.filePath("prompts")).contains("The document open in Qucs-S"));   // (a prompt)
        panel.session()->stop();
        QTRY_VERIFY(!panel.session()->isBusy());
        panel.session()->reset();
#endif

        // The list as "/" is typed.
        QListWidget* list = panel.commandList();
        panel.composer()->setPlainText("/re");
        QVERIFY(list->isVisible());
        QStringList offered;
        for (int i = 0; i < list->count(); ++i) offered << list->item(i)->data(Qt::UserRole).toString();
        QCOMPARE(offered.mid(0, 3), QStringList({"resume", "rename", "review"}));
        QVERIFY(offered.contains("security-review"));   // (containing it, after)
        QTest::keyClick(panel.composer(), Qt::Key_Down);
        QTest::keyClick(panel.composer(), Qt::Key_Tab);
        QCOMPARE(panel.composer()->toPlainText(), QStringLiteral("/rename "));
        QVERIFY(!list->isVisible());
        panel.composer()->setPlainText("/sta");
        QVERIFY(list->isVisible());
        QTest::keyClick(panel.composer(), Qt::Key_Escape);
        QVERIFY(!list->isVisible());
        panel.composer()->setPlainText("/hel");
        QTest::keyClick(panel.composer(), Qt::Key_Return);   // runs /help
        QVERIFY(panel.composer()->toPlainText().isEmpty());
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("Commands"));
        panel.composer()->setPlainText("/nothing-like-it");
        QVERIFY(!list->isVisible());
        panel.composer()->setPlainText("/re x");
        QVERIFY(!list->isVisible());
        // Claude Code's, as it said at its start, kept for the list.
        QucsSettingsFile().setValue("ClaudeCode/slashCommands", QStringList{"my-skill", "compact"});
        panel.composer()->setPlainText("/my");
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("my-skill"));
        panel.composer()->clear();
        QucsSettingsFile().remove("ClaudeCode/slashCommands");
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
        // What the turn took: none of it at first (as usageShown() has it
        // before any is chosen).
        QCOMPARE(ClaudeCodePanel::usageShown(), 0);
        QVERIFY2(text.contains("Done in 1.2 s  ·  2 steps  ·  changed out.txt"), qPrintable(text));
        QVERIFY(!text.contains("tokens"));
        QVERIFY(!text.contains("$"));
        // Its tokens, as soon as chosen; each kind of them in its tooltip.
        ClaudeCodePanel::setUsageShown(ClaudeCodePanel::PromptTokens | ClaudeCodePanel::ConversationTokens);
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("Done in 1.2 s  ·  12.8k tokens  ·  2 steps  ·  changed out.txt"));
        QVERIFY(!panel.transcriptText().contains("tokens in all"));   // (the conversation's is the prompt's)
        QVERIFY(toolTipOf(panel.transcript()->document(), "12.8k tokens").contains(QLocale().toString(10500)));
        // The costs shown, and the tokens not, as soon as chosen.
        ClaudeCodePanel::setUsageShown(ClaudeCodePanel::PromptCost | ClaudeCodePanel::ConversationCost);
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("Done in 1.2 s  ·  $0.012  ·  2 steps  ·  changed out.txt"));
        QVERIFY(!panel.transcriptText().contains("tokens"));
        ClaudeCodePanel::setUsageShown(ClaudeCodePanel::ConversationTokens);
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("12.8k tokens in all"));
        ClaudeCodePanel::setUsageShown(0);
        panel.renderNow();
        QVERIFY(panel.transcriptText().contains("Done in 1.2 s  ·  2 steps  ·  changed out.txt"));
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
        QDir(qucs_s::claude::history::directory()).removeRecursively();   // (none to reopen)

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
        // Whose edit, to the tools: the file's, when nothing says whose - a
        // conversation's, when its turn reports it changed the file.
        QCOMPARE(doc->recentEdits().last().by, QucsDoc::kOnDisk);
        rewrite("\"47\"", "\"39\"");
        emit panel->filesChanged({file});
        doc = dynamic_cast<Schematic*>(app.getDoc());
        QCOMPARE(resistance(), QStringLiteral("39"));
        QCOMPARE(doc->recentEdits().last().by, panel->session()->caller());
        rewrite("\"39\"", "\"47\"");
        app.reloadChangedFiles({file});
        QCOMPARE(resistance(), QStringLiteral("47"));

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

        // A spreadsheet Claude changed (bug hunt 2026-09-26, E1: "could not
        // be loaded again", the old table kept - and saved over Claude's):
        // loaded again, the cell in front kept.
        const QString csv = workspace + "/results.csv";
        const auto writeCsv = [&](const QByteArray& text) {
            QFile f(csv);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(text);
        };
        writeCsv("t,v\n0,1\n1,2\n2,3\n");
        QVERIFY(app.gotoPage(csv, false, false));
        auto* sheet = dynamic_cast<SheetDoc*>(app.getDoc());
        QVERIFY(sheet != nullptr);
        sheet->view()->setCurrentIndex(sheet->view()->model()->index(2, 1));
        writeCsv("t,v,w\n0,1,9\n1,2,8\n2,3,7\n");
        app.reloadChangedFiles({csv});
        QCOMPARE(sheet->sheet().columnCount(), 3);
        QCOMPARE(sheet->sheet().at(2, 2).text, QStringLiteral("8"));
        QCOMPARE(sheet->view()->currentIndex().row(), 2);
        QCOMPARE(sheet->view()->currentIndex().column(), 1);
        QVERIFY(!sheet->getDocChanged());
        panel->renderNow();
        QVERIFY(panel->transcriptText().contains("results.csv loaded again"));

        // Unsaved changes of its own: not loaded, and its Save asks before
        // writing over what Claude wrote - Cancel keeps Claude's.
        sheet->setCell(0, 0, "time");
        QVERIFY(sheet->getDocChanged());
        QTest::qWait(1100);   // (a newer time on the file than its load)
        writeCsv("t,v\n5,5\n");
        app.reloadChangedFiles({csv});
        QCOMPARE(sheet->sheet().at(0, 0).text, QStringLiteral("time"));
        const auto saveAnswering = [&](const QString& button) {
            QString asked;
            QTimer answer;
            connect(&answer, &QTimer::timeout, [&] {
                auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (box == nullptr || box->objectName() != "writeOverChanged") return;
                asked = box->text();
                for (QAbstractButton* b : box->buttons())
                    if (b->text() == button) b->click();
                answer.stop();
            });
            answer.start(20);
            const bool saved = app.saveFile(sheet);
            return std::make_pair(saved, asked);
        };
        auto [saved, asked] = saveAnswering("Cancel");
        QVERIFY(!saved);
        QVERIFY2(asked.contains("results.csv was changed by another program"), qPrintable(asked));
        QCOMPARE(read(csv), QStringLiteral("t,v\n5,5\n"));
        std::tie(saved, asked) = saveAnswering("Write Over It");
        QVERIFY(saved);
        QVERIFY(read(csv).startsWith("time,"));
        std::tie(saved, asked) = saveAnswering("Write Over It");   // (saved here since: not asked)
        QVERIFY(saved && asked.isEmpty());

        // Written by another program: followed too, as schematics are.
        QTest::qWait(1100);
        writeCsv("t,v\n7,7\n");
        QTRY_COMPARE_WITH_TIMEOUT(sheet->sheet().at(1, 0).text, QStringLiteral("7"), 10000);
        QVERIFY(app.closeAllFiles());
    }
};

QTEST_MAIN(TestClaudeCode)
#include "test_claude_code.moc"
