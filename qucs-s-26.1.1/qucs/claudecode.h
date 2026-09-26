/*
 * claudecode.h - a Claude Code session: the claude program run headless,
 *                its conversation over stream-json both ways
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CLAUDECODE_H
#define QUCS_CLAUDECODE_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;
class QTimer;

namespace qucs_s::claude {

/// Where a session stands: for the dock and the status bar.
enum class State {
    NotFound,   ///< there is no claude program
    Off,        ///< nothing running (none yet, or stopped); a prompt starts it
    Starting,   ///< the program is starting
    Thinking,   ///< a turn is under way: the model is at work
    Working,    ///< a tool runs (the detail says which)
    Waiting,    ///< a tool waits for the user's permission
    Ready,      ///< the turn is over; the session waits for the next prompt
    Failed,     ///< the last turn or the program failed (the detail says why)
};

/// "ready", "thinking", ...: the state in a word or two.
QString stateText(State state);

/// A tool that asks to be used (Bash, Write, Edit, ...).
struct PermissionRequest {
    QString id;             ///< the request's id, for the answer
    QString tool;           ///< Bash, Write, Edit, ...
    QString action;         ///< what it wants, in words: "run a command"
    QString subject;        ///< on what: the command, the file
    QString detail;         ///< more of it: a file's new content, an edit
    QJsonObject input;      ///< the tool's input, as asked
    bool canAllowEdits = false;   ///< it offers "accept edits" for the session
    bool canAllowTools = false;   ///< one of the host's tools (ToolHost): all of them may be allowed at once
};

/// How a turn ended.
struct TurnResult {
    bool ok = false;
    QString subtype;        ///< success, error_max_turns, error_during_execution
    QString text;           ///< the final reply
    qint64 durationMs = 0;
    double costUsd = 0.0;           ///< this turn's
    double conversationCostUsd = 0.0;   ///< the conversation's so far
    int turns = 0;
    int denials = 0;        ///< tool uses that were not allowed
    bool stopped = false;   ///< the user stopped it
    QStringList changedFiles;   ///< files Write or Edit changed in the turn
};

/// What the program is told: -p with stream-json both ways and every
/// partial message, permission prompts to its host (us), and the
/// options that are set.
struct Options {
    QString permissionMode;   ///< empty: ask; acceptEdits, auto, plan, bypassPermissions
    QString model;            ///< empty: the program's default; opus, sonnet, haiku, ...
    QString resume;           ///< a session to continue
    QString appendSystemPrompt;
    QString mcpConfig;        ///< --mcp-config: the host's tool server
    QStringList allowedTools; ///< tools used without asking
};
QStringList arguments(const Options& options);

/// The claude program: \a configured when it is set (a path, or a name on
/// PATH), else claude on PATH, else where its installers put it. Empty
/// when there is none. \a home is the home directory (the account's when
/// empty).
QString findProgram(const QString& configured, const QString& home = QString());

/// What a tool use is about, in one line: a command, a file (relative to
/// \a workDir when inside it), a pattern, a URL.
QString toolSubject(const QString& tool, const QJsonObject& input, const QString& workDir = QString());

/// What Claude is told about where it runs, after its own system prompt.
QString qucsSystemPrompt();

/*!
 * Tools the host application offers Claude: an MCP server that runs in
 * the host. The claude program is told of it as an "sdk" server and sends
 * its messages over the stream it already has (control requests of the
 * subtype "mcp_message", as the Agent SDKs do); the session answers them
 * from here. To Claude the tools are mcp__<serverName>__<tool>.
 */
class ToolHost
{
public:
    virtual ~ToolHost() = default;
    virtual QString serverName() const = 0;
    /// MCP's tools/list: name, description, inputSchema for each.
    virtual QJsonArray tools() const = 0;
    /// The tools that only look: used without asking.
    virtual QStringList readOnlyTools() const = 0;
    /// What a tool does, for "Claude wants to ...": "add a component".
    virtual QString actionOf(const QString& tool) const = 0;
    /// What a use of it is about, in one line.
    virtual QString subjectOf(const QString& tool, const QJsonObject& arguments) const = 0;
    /// What Claude is told of the server (MCP's instructions).
    virtual QString instructions() const = 0;
    /// Calls \a tool; \a done gets MCP's CallToolResult (content,
    /// isError), now or later. Called from the event loop, not from within
    /// the reading of the program's output.
    virtual void callTool(const QString& tool, const QJsonObject& arguments,
                          std::function<void(const QJsonObject&)> done) = 0;
    /// \a arguments for a conversation pinned to \a document (a file;
    /// empty: none): a tool that acts on the document in front when it is
    /// given none is given that one. As they are, unless the host says
    /// otherwise.
    virtual QJsonObject forDocument(const QString& tool, const QJsonObject& arguments, const QString& document) const
    {
        Q_UNUSED(tool);
        Q_UNUSED(document);
        return arguments;
    }
};

/// Whether \a mode is the one where Claude asks before acting: not named,
/// or as the program names it ("default", "manual").
bool isAskMode(const QString& mode);

/// "claude-haiku-4-5-20251001": Haiku 4.5; "opus": Opus.
QString modelName(const QString& id);

/// A model to choose: one the program offers, or one Qucs-S knows of.
struct ModelChoice {
    QString value;          ///< what --model is given; empty: the program's default
    QString name;           ///< "Fable 5.1", "Opus 5 with 1M context"
    QString description;    ///< what it is for: "Efficient for routine tasks"
    QString resolved;       ///< the model it is: claude-fable-5-1
    bool autoMode = false;  ///< it works in auto mode
    bool listed = false;    ///< the program offered it
};

/// The models the program offers (the "models" of its answer to
/// "initialize"; the default first), then the newest of each family that
/// it does not offer, by their full names.
QList<ModelChoice> modelChoices(const QJsonArray& listed);

/*!
 * Asks the claude program which models it offers: it is started, asked
 * ("initialize", as its SDKs do) and its input closed, so that it ends
 * after the answer. Nothing goes to the model.
 */
class ModelQuery : public QObject
{
    Q_OBJECT

public:
    explicit ModelQuery(QObject* parent = nullptr);
    ~ModelQuery() override;

    /// Asks \a program, run in \a dir (the home directory when empty).
    void start(const QString& program, const QString& dir);
    bool isRunning() const { return a_process != nullptr; }

signals:
    /// Its "models"; empty when it did not answer.
    void finished(const QJsonArray& models);

private:
    void finish();

    QProcess* a_process = nullptr;
    QByteArray a_output;
    QTimer* a_timeout;
};

/*!
 * A conversation with Claude Code. The first prompt starts the claude
 * program in the working directory; it stays for the next prompts (a turn
 * each) and reports as it goes: the reply as it is written, each tool
 * used and how it went, a tool that needs the user's permission (answer()
 * gives it), the end of the turn. Stopped, or changed to another mode or
 * model, the program ends; the next prompt starts it again, continuing
 * the conversation (--resume).
 */
class Session : public QObject
{
    Q_OBJECT

public:
    explicit Session(QObject* parent = nullptr);
    ~Session() override;

    /// The program to run: a path, or empty when there is none.
    void setProgram(const QString& program);
    QString program() const { return a_program; }

    /// Where Claude works. Another directory starts a new conversation.
    void setWorkingDirectory(const QString& dir);
    QString workingDirectory() const { return a_workDir; }

    /// For the next start; a running program ends after its turn.
    void setPermissionMode(const QString& mode);
    QString permissionMode() const { return a_mode; }
    /// The mode the program said it works in (not every model has every
    /// mode: auto falls back to asking); empty until it says.
    QString permissionModeInUse() const { return a_modeInUse; }
    void setModel(const QString& model);
    QString model() const { return a_model; }
    void setAppendSystemPrompt(const QString& prompt) { a_systemPrompt = prompt; }
    /// The host's tools, offered from the next start on (not owned; it
    /// outlives the session).
    void setToolHost(ToolHost* host);
    ToolHost* toolHost() const { return a_host; }
    /// The document the conversation is pinned to (its file), or empty:
    /// the host's tools that act on the document in front when they are
    /// given none act on this one (ToolHost::forDocument()).
    void setDocument(const QString& path) { a_document = path; }
    QString document() const { return a_document; }
    /// The host's tools are used without asking for the rest of the
    /// conversation (answer() with \a allowTools, or set here).
    void setToolsAllowed(bool allowed) { a_toolsAllowed = allowed; }
    bool toolsAllowed() const { return a_toolsAllowed; }

    State state() const { return a_state; }
    QString detail() const { return a_detail; }
    bool isBusy() const;
    bool isRunning() const;
    /// Since the turn under way began (0 when none is).
    qint64 turnElapsed() const;

    QString sessionId() const { return a_sessionId; }
    /// The model and the program's version, as it said when it started.
    QString modelInUse() const { return a_modelInUse; }
    QString version() const { return a_version; }

    /// Sends a prompt, starting the program first when it does not run.
    /// False when it cannot be sent: no program, a turn under way.
    bool send(const QString& prompt);
    /// Answers the permission request \a id: \a allow it or not; with
    /// \a allowEdits, edits need no asking for the rest of the session;
    /// with \a allowTools, the host's tools (the other requests for them
    /// are answered too).
    void answer(const QString& id, bool allow, bool allowEdits = false, bool allowTools = false);
    /// Stops the turn under way (the program ends if it does not stop).
    void interrupt();
    /// Ends the program; the next prompt continues the conversation.
    void stop();
    /// Ends the program and forgets the conversation.
    void reset();

    /// Takes one line of the program's output (for tests: feeds the
    /// stream without a program).
    void handleLine(const QByteArray& line);

signals:
    void stateChanged(qucs_s::claude::State state, const QString& detail);
    void sessionStarted(const QString& sessionId, const QString& model);
    /// The permission mode changed from within (edits allowed for the
    /// session from a permission request).
    void permissionModeChanged(const QString& mode);
    /// The reply being written, so far (as a whole, not the new part).
    void replyStreamed(const QString& text);
    /// A finished part of the reply.
    void replyFinished(const QString& text);
    /// A tool Claude uses: what about (a line) and more of it (the whole
    /// command, the edit).
    void toolStarted(const QString& id, const QString& tool, const QString& subject, const QString& detail);
    void toolFinished(const QString& id, bool failed, const QString& output);
    void permissionRequested(const qucs_s::claude::PermissionRequest& request);
    /// A permission request is no longer asked (the turn was stopped).
    void permissionWithdrawn(const QString& id);
    /// Something to tell the user: a permission denied, a limit reached.
    void notice(const QString& text);
    void turnFinished(const qucs_s::claude::TurnResult& result);
    /// The program could not start or ended unexpectedly.
    void failed(const QString& message);

private:
    void start();
    void write(const QJsonObject& message);
    void setState(State state, const QString& detail = QString());
    void readOutput();
    void processFinished(int exitCode);
    void handleAssistant(const QJsonObject& m);
    void handleUser(const QJsonObject& m);
    void handleStreamEvent(const QJsonObject& m);
    void handleControlRequest(const QJsonObject& m);
    void handleMcpMessage(const QString& requestId, const QJsonObject& request);
    void allowRequest(const QString& id, const QJsonObject& input);
    /// The host's tool that \a tool (mcp__server__name) is, or empty.
    QString hostTool(const QString& tool) const;
    void handleResult(const QJsonObject& m);
    void handleSystem(const QJsonObject& m);
    void endTurn();

    QProcess* a_process = nullptr;
    QByteArray a_buffer;       // output not yet a whole line
    QByteArray a_stderr;       // the tail of the program's error output
    QTimer* a_stopTimer;       // an interrupted turn that does not stop

    QString a_program;
    QString a_workDir;
    QString a_mode;
    QString a_model;
    QString a_systemPrompt;
    bool a_restartAfterTurn = false;   // a mode or model changed during a turn

    State a_state = State::Off;
    QString a_detail;
    bool a_busy = false;
    QElapsedTimer a_turnClock;

    QString a_sessionId;
    QString a_modelInUse;
    QString a_modeInUse;
    QString a_version;
    bool a_stopping = false;   // stop() is ending the program
    double a_reportedCost = 0.0;       // the program's total so far (it counts from its start)
    double a_conversationCost = 0.0;
    bool a_interrupted = false;   // the user stopped the turn under way

    QString a_streamed;        // the reply being written
    bool a_streaming = false;
    QHash<QString, QString> a_tools;        // tool use id -> tool name, this turn
    QHash<QString, QString> a_editedFiles;  // tool use id -> file, until its result
    QStringList a_changedFiles;             // this turn's
    QHash<QString, PermissionRequest> a_pending;   // permission requests not yet answered
    ToolHost* a_host = nullptr;
    QString a_document;        // pinned: the host's tools act on it
    bool a_toolsAllowed = false;
    /// \a input of the host's \a tool, for the document pinned.
    QJsonObject forDocument(const QString& tool, const QJsonObject& input) const;
    void withdrawRequests();
};

} // namespace qucs_s::claude

Q_DECLARE_METATYPE(qucs_s::claude::PermissionRequest)
Q_DECLARE_METATYPE(qucs_s::claude::TurnResult)

#endif // QUCS_CLAUDECODE_H
