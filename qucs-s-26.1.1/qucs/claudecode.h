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
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;
class QTimer;

namespace qucs_s::mcp {
class Server;
}

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

/// The image in the file \a path as a prompt gives it to Claude: its bytes
/// as they are when PNG, JPEG, GIF or WebP of 3.75 MB at most (5 MB in
/// base64, the API's limit) and no side over 2000 pixels - else read,
/// scaled down to 2000 pixels at most a side, and written as PNG (as JPEG
/// when that is too large: a photo); \a mediaType image/png and so on.
/// Empty, and why in \a why, when it is no image that reads.
QByteArray imageForClaude(const QString& path, QString* mediaType, QString* why = nullptr);
/// A prompt's message content: \a prompt as it is, or with \a images
/// (their files) each an image block before it - Claude sees them.
QJsonValue promptContent(const QString& prompt, const QStringList& images);

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

/// Tokens the models took and gave: what was read anew, read from the
/// cache and written to it, and what they wrote.
struct TokenUsage {
    qint64 input = 0;
    qint64 output = 0;
    qint64 cacheRead = 0;
    qint64 cacheWrite = 0;
    qint64 total() const { return input + output + cacheRead + cacheWrite; }
    bool isEmpty() const { return total() == 0; }
    TokenUsage& operator+=(const TokenUsage& other);
    bool operator==(const TokenUsage&) const = default;
};
/// A count of tokens as a message gives it: whole, not below nought, not
/// past 1e15 (a garbled 1e308 was converted past qint64's range, and sums
/// of such wrapped round - the fuzzing of 22 October). Sums of them stop at
/// 1e18, so that a total of four never wraps either.
qint64 tokenCount(const QJsonValue& value);
/// The tokens a result reports (its "modelUsage", every model's summed:
/// the program's totals since it started; or, from a program that has
/// none, its "usage": the turn's, of the main loop alone). \a running
/// says which.
TokenUsage tokensOf(const QJsonObject& result, bool* running = nullptr);

/// How a turn ended.
struct TurnResult {
    bool ok = false;
    QString subtype;        ///< success, error_max_turns, error_during_execution
    QString text;           ///< the final reply
    qint64 durationMs = 0;
    double costUsd = 0.0;           ///< this turn's
    double conversationCostUsd = 0.0;   ///< the conversation's so far
    TokenUsage tokens;                  ///< this turn's
    TokenUsage conversationTokens;      ///< the conversation's so far
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
    /// With \a resume: the conversation up to this message alone (its
    /// uuid), the rest left out (--resume-session-at) - a prompt edited.
    QString resumeAt;
    bool fork = false;        ///< with \a resume: as a new session (--fork-session)
    QString appendSystemPrompt;
    QString mcpConfig;        ///< --mcp-config: the host's tool server
    QStringList allowedTools; ///< tools used without asking
    /// The conversation compacted when its context fills (Claude Code's
    /// autoCompactEnabled), whatever the user's own settings say: told
    /// always, as a flag setting (--settings).
    bool autoCompact = true;
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

/// The uses of the host's tools the user chose to be asked about each
/// time (the Claude panel's Extras menu), in every conversation whose
/// mode asks at all (not bypassPermissions).
struct Guards {
    /// A simulation of a schematic that runs commands besides the
    /// simulator: a System Command part, ngspice's shell, an Octave script.
    bool commands = true;
    /// A file read or written outside the workspace and the open project.
    bool outside = true;
};

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
    /// As callTool(), for the conversation \a caller (a number of its
    /// own, the same at each of its calls): a host may tell it what has
    /// changed since its last call that it did not change itself - the
    /// user's edits, a simulation the user ran.
    virtual void callToolFor(quint64 caller, const QString& tool, const QJsonObject& arguments,
                             std::function<void(const QJsonObject&)> done)
    {
        Q_UNUSED(caller);
        callTool(tool, arguments, std::move(done));
    }
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

    /// MCP's resources: what can be read without a tool call (resources/
    /// list), and the forms of their URIs (resources/templates/list).
    virtual QJsonArray resources() const { return {}; }
    virtual QJsonArray resourceTemplates() const { return {}; }
    /// A resource's contents (MCP ResourceContents: uri, mimeType, text);
    /// empty, and why in \a error, when there is no such resource.
    virtual QJsonArray readResource(const QString& uri, QString* error)
    {
        if (error != nullptr) *error = QStringLiteral("There is no resource %1.").arg(uri);
        return {};
    }
    /// What changes whenever \a uri's contents would (a revision): those
    /// subscribed to it are told when it does. Empty: it is not there.
    virtual QString resourceVersion(const QString& uri) const
    {
        Q_UNUSED(uri);
        return {};
    }
    /// Whether this use of \a tool cannot be undone - files deleted or
    /// written over, unsaved changes discarded: asked about even when the
    /// host's tools are allowed.
    virtual bool irreversible(const QString& tool, const QJsonObject& arguments) const
    {
        Q_UNUSED(tool);
        Q_UNUSED(arguments);
        return false;
    }
    /// The tools Claude Code lets Claude use without its own check where
    /// Claude acts on its own (auto mode), beyond readOnlyTools(): those
    /// each use of which is undone by Edit > Undo, adds (a document, a
    /// project, a dataset written anew) or shows. A use of one that cannot
    /// be undone or is asked about each time (irreversible(),
    /// askedEachTime()) the dock asks about itself. None unless the host
    /// names them.
    virtual QStringList autonomousTools() const { return {}; }
    /// Why this use of \a tool is asked about every time - also where
    /// Claude acts on its own (auto), not where the user said to ask
    /// nothing (bypassPermissions): a line typed into a console, which runs
    /// with the user's rights outside Claude Code's own rules for commands;
    /// and what the guards the user chose name (setGuards()). Empty: as the
    /// other uses.
    virtual QString askedEachTime(const QString& tool, const QJsonObject& arguments) const
    {
        Q_UNUSED(tool);
        Q_UNUSED(arguments);
        return {};
    }
    /// The guards the user chose, for every conversation.
    virtual void setGuards(const Guards& guards) { Q_UNUSED(guards); }
    /// Whether the conversation \a caller is asked what askedEachTime()
    /// names (its mode is not bypassPermissions): a call of its that comes
    /// to a use the guards name that was not asked about when the call
    /// began - a part with a command placed and simulated in one batch, a
    /// file outside the workspace saved by a script - is then refused.
    virtual void setAsking(quint64 caller, bool asking)
    {
        Q_UNUSED(caller);
        Q_UNUSED(asking);
    }
    /// How the conversation \a caller asks its user a question (MCP
    /// elicitation: a message and a JSON schema of the answer; \a done gets
    /// {action: accept|decline|cancel, content}) while a call of its runs;
    /// null when it cannot.
    using Asker = std::function<void(const QString& message, const QJsonObject& schema,
                                     std::function<void(const QJsonObject& result)> done)>;
    virtual void setAsker(quint64 caller, Asker asker)
    {
        Q_UNUSED(caller);
        Q_UNUSED(asker);
    }
};

/// What Claude Code keeps of an MCP server's instructions and of each tool's
/// description unless CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH says more (in
/// characters): the rest is cut, "… [truncated]".
constexpr int kDescriptionCap = 2048;
/// The longest of \a host's instructions and tool descriptions, in
/// characters as Claude Code counts them (UTF-16).
int longestDescription(const ToolHost& host);
/// The environment the claude program is started in: \a base, with
/// CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH set to longestDescription() when
/// \a host has more than kDescriptionCap to say - unless \a base sets it
/// already (the user's, kept). Claude Code reads it from 2.1.280; an older
/// one cuts at kDescriptionCap whatever it says.
QProcessEnvironment claudeEnvironment(const QProcessEnvironment& base, const ToolHost* host);

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
 * gives it), the end of the turn. Another mode or model it takes as it
 * runs, for the turns after (a program that will not is started again
 * with it once its turn is over). Stopped, the program ends; the next
 * prompt starts it again, continuing the conversation (--resume).
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

    /// For the turns after this one: a running program is told at once.
    void setPermissionMode(const QString& mode);
    QString permissionMode() const { return a_mode; }
    /// The mode the program said it works in (not every model has every
    /// mode: auto falls back to asking); empty until it says, and again
    /// after another is chosen.
    QString permissionModeInUse() const { return a_modeInUse; }
    /// For the turns after this one, as the mode.
    void setModel(const QString& model);
    QString model() const { return a_model; }
    /// Auto-compact (Options::autoCompact), on unless set off: a running
    /// program is told at once (apply_flag_settings; started again with
    /// it after the turn when it refuses).
    void setAutoCompact(bool on);
    bool autoCompact() const { return a_autoCompact; }
    /// The guards the user chose (Guards): given to the host, at once and
    /// whenever it is set.
    void setGuards(const Guards& guards);
    Guards guards() const { return a_guards; }
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
    /// The next start continues the conversation \a sessionId (--resume):
    /// one kept from before Qucs-S was closed, or one of Claude Code's
    /// own. When Claude Code no longer has it, the prompt is sent again to
    /// a new conversation (and a notice says so).
    void resume(const QString& sessionId);
    /// The last message of the conversation (its uuid, as the program
    /// keeps it): where it stands now, to go back to before the next
    /// prompt. Empty before the first reply, and when not known (a session
    /// continued, until setLastMessageId()).
    QString lastMessageId() const { return a_lastMessageId; }
    void setLastMessageId(const QString& id) { a_lastMessageId = id; }
    /// Goes back to where the conversation \a sessionId stood at the
    /// message \a messageId, to go on from there as a new session (the
    /// old one kept whole): the program ends, and the next prompt starts
    /// it so (--resume --resume-session-at --fork-session). With no
    /// \a messageId: a new conversation, nothing said before. When the
    /// program cannot go back there, rewindFailed() says why, and the
    /// conversation is as it was.
    void rewind(const QString& sessionId, const QString& messageId);
    /// What the conversation has taken so far - one kept from before, its
    /// last turn's totals -: the next turns' are added to it.
    void setConversationTotals(const TokenUsage& tokens, double costUsd);
    TokenUsage conversationTokens() const { return a_conversationTokens; }
    double conversationCost() const { return a_conversationCost; }
    /// The slash commands the program runs here (not those only its
    /// terminal has), as it said when it started: "compact", "context",
    /// skills...; empty before it has.
    QStringList slashCommands() const { return a_slashCommands; }
    /// The model and the program's version, as it said when it started;
    /// the model is empty after another is chosen, until it says again.
    QString modelInUse() const { return a_modelInUse; }
    QString version() const { return a_version; }

    /// Sends a prompt - with \a images, files Claude is shown
    /// (promptContent()) -, starting the program first when it does not
    /// run. False when it cannot be sent: no program, a turn under way,
    /// nothing to send.
    bool send(const QString& prompt, const QStringList& images = {});
    /// Answers the permission request \a id: \a allow it or not; with
    /// \a allowEdits, edits need no asking for the rest of the session;
    /// with \a allowTools, the host's tools (the other requests for them
    /// are answered too).
    void answer(const QString& id, bool allow, bool allowEdits = false, bool allowTools = false);
    /// Answers a question an MCP server asked the user (elicitation):
    /// \a action accept (with \a content, the answer), decline or cancel.
    void answerElicitation(const QString& id, const QString& action, const QJsonObject& content = {});
    /// Stops the turn under way (the program ends if it does not stop).
    void interrupt();
    /// Whether a tool Claude waits on can be sent to the background now: a
    /// command or an agent of the program's own, or one of the host's tools.
    bool canSendToBackground() const;
    /// Sends the tools Claude waits on to the background, so that the turn
    /// goes on - and ends - without them. The program's own commands and
    /// agents it is asked to run on by themselves (as its terminal's
    /// Ctrl+B does); it says when one ends, and goes on with a turn of its
    /// own. A host's tool is answered at once that it goes on, and its own
    /// answer is a message to Claude when it comes (backgroundFinished()).
    /// How many it sent: 0 when none was waited on.
    int sendToBackground();
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
    /// The program said which mode it now works in (permissionModeInUse()).
    void modeInUseChanged();
    /// The reply being written, so far (as a whole, not the new part).
    void replyStreamed(const QString& text);
    /// A finished part of the reply.
    void replyFinished(const QString& text);
    /// A tool Claude uses: what about (a line) and more of it (the whole
    /// command, the edit).
    void toolStarted(const QString& id, const QString& tool, const QString& subject, const QString& detail);
    void toolFinished(const QString& id, bool failed, const QString& output);
    void permissionRequested(const qucs_s::claude::PermissionRequest& request);
    /// An MCP server - Qucs-S's own, when a file would be written over or
    /// unsaved changes lost - asks the user: \a message, and \a schema of
    /// the answer (a JSON schema object: its properties). answerElicitation()
    /// answers it.
    void elicitationRequested(const QString& id, const QString& server, const QString& message, const QJsonObject& schema);
    /// A permission request is no longer asked (the turn was stopped).
    void permissionWithdrawn(const QString& id);
    /// Something to tell the user: a permission denied, a limit reached.
    void notice(const QString& text);
    void turnFinished(const qucs_s::claude::TurnResult& result);
    /// A host's tool sent to the background has finished: \a output, given
    /// to Claude in a message - unless Claude Code had ended (\a told false).
    void backgroundFinished(const QString& tool, bool failed, const QString& output, bool told);
    /// The program could not start or ended unexpectedly.
    void failed(const QString& message);
    /// rewind() could not go back where it was asked to (the program's
    /// words): the prompt was not sent, and the conversation is as before.
    void rewindFailed(const QString& message);

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
    void handleControlResponse(const QJsonObject& m);
    /// Asks the running program to change something as it runs: a
    /// control request of \a subtype, with \a fields.
    void askProgram(const QString& subtype, const QJsonObject& fields);
    /// A notice when the program works in \a inUse, not the mode asked for.
    void reportMode(const QString& inUse);
    void handleMcpMessage(const QString& requestId, const QJsonObject& request);
    /// An MCP message of the program's to the host's server answered by
    /// \a reply: a tool call kept by the tool use it answers.
    void callHost(const QJsonObject& message, const std::function<void(const QJsonObject&)>& reply);
    void allowRequest(const QString& id, const QJsonObject& input);
    /// Whether the dock lets a use of the host's \a tool go unasked in the
    /// mode it is in now (what it answers the program's can_use_tool).
    bool hostAllowsUnasked(const QString& tool, const QJsonObject& input) const;
    /// Whether a call the program lets through by a rule - a tool that only
    /// looks, one allowed with auto mode - is asked about by the dock
    /// itself: a use that cannot be undone or is asked about each time, or
    /// the mode changed since the program started.
    bool asksItself(const QString& tool, const QJsonObject& input) const;
    /// The question of a use of the host's \a tool.
    PermissionRequest hostRequest(const QString& id, const QString& tool, const QJsonObject& input) const;
    /// The host's tool that \a tool (mcp__server__name) is, or empty.
    QString hostTool(const QString& tool) const;
    void handleResult(const QJsonObject& m);
    void handleSystem(const QJsonObject& m);
    /// A turn begins: one of the user's (send()), or one the program
    /// begins by itself (a task sent to the background has ended).
    void beginTurn();
    void endTurn();
    /// Writes a user message of \a content (\a prompt: its text).
    bool sendContent(const QJsonValue& content, const QString& prompt);
    /// Asks the program to send the tool use \a id to the background.
    void askBackground(const QString& id);
    bool asksBackground(const QString& id) const;   // (asked, not answered yet)

    QProcess* a_process = nullptr;
    QByteArray a_buffer;       // output not yet a whole line
    QByteArray a_stderr;       // the tail of the program's error output
    QTimer* a_stopTimer;       // an interrupted turn that does not stop

    QString a_program;
    QString a_workDir;
    QString a_mode;
    QString a_model;
    bool a_autoCompact = true;
    Guards a_guards;
    QString a_systemPrompt;
    bool a_restartAfterTurn = false;   // the program would not take another mode or model
    QHash<QString, QString> a_asked;   // askProgram() not answered yet: request id -> subtype

    State a_state = State::Off;
    QString a_detail;
    bool a_busy = false;
    QElapsedTimer a_turnClock;

    QString a_sessionId;
    QString a_resumedFrom;     // the session a start continued
    bool a_initSeen = false;   // the program said it started
    bool a_resumeFailed = false;   // it could not continue a_resumedFrom
    QString a_lastPrompt;      // (sent again to a new conversation then)
    QString a_lastMessageId;   // the conversation's last message (lastMessageId())
    QString a_rewindAt;        // the next start goes back to this message (rewind())
    QString a_rewoundFrom;     // the last message before the rewind (put back when it fails)
    QString a_rewoundSession;  // the session before it
    // Who this conversation is to the tools' host: a number no other
    // conversation of this run has (ToolHost::callToolFor()).
    const quint64 a_caller = nextCaller();

public:
    quint64 caller() const { return a_caller; }

private:
    static quint64 nextCaller()
    {
        static quint64 last = 0;
        return ++last;
    }
    QStringList a_slashCommands;
    QString a_modelInUse;
    QString a_modeInUse;
    QString a_version;
    bool a_stopping = false;   // stop() is ending the program
    double a_reportedCost = 0.0;       // the program's total so far (it counts from its start)
    double a_conversationCost = 0.0;
    TokenUsage a_reportedTokens;       // (as its cost)
    TokenUsage a_conversationTokens;
    bool a_interrupted = false;   // the user stopped the turn under way

    QString a_streamed;        // the reply being written
    bool a_streaming = false;
    QHash<QString, QString> a_tools;        // tool use id -> tool name, this turn
    QHash<QString, QString> a_editedFiles;  // tool use id -> file, until its result
    QStringList a_changedFiles;             // this turn's
    QHash<QString, PermissionRequest> a_pending;   // permission requests not yet answered
    QSet<QString> a_running;                // this turn's tool uses not yet answered
    // A call of the host's tools, by the tool use it answers (Claude Code
    // says which: "claudecode/toolUseId"), until it is answered.
    struct HostCall {
        QJsonValue rpcId;
        QString tool;
        std::function<void(const QJsonObject&)> reply;
        bool background = false;   // answered already: sent to the background
    };
    QHash<QString, HostCall> a_hostCalls;
    void hostCallDone(const HostCall& call, const QJsonObject& rpc);
    QHash<QString, QString> a_backgroundAsked;   // background_tasks not answered yet: request id -> tool use id
    QSet<QString> a_backgroundAskedOfTask;       // those asked when its task was known
    QSet<QString> a_backgroundWanted;            // tool uses sent to the background, until they are
    QHash<QString, QString> a_tasks;             // the program's tasks (commands, agents): id -> what
    QHash<QString, QString> a_taskOf;            // a tool use's task
    QSet<QString> a_backgroundTasks;             // those in the background
    ToolHost* a_host = nullptr;
    mcp::Server* a_mcp = nullptr;   // the host's tools as an MCP server, over this stream
    // The host's tools the program was started with allowed beyond those
    // that only look (autonomousTools(), in auto mode).
    QStringList a_autoAllowed;
    // A call the dock asks about itself (asksItself()), until answered:
    // allowed, it is made; not, answered as not allowed.
    struct HeldCall {
        QJsonObject message;
        std::function<void(const QJsonObject&)> reply;
    };
    QHash<QString, HeldCall> a_held;
    void refuseHeld(const HeldCall& call, const QString& why);
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
