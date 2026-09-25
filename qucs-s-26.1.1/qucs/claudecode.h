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
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

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
    QString permissionMode;   ///< empty: ask; acceptEdits, plan, bypassPermissions
    QString model;            ///< empty: the program's default; opus, sonnet, haiku, ...
    QString resume;           ///< a session to continue
    QString appendSystemPrompt;
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
    void setModel(const QString& model);
    QString model() const { return a_model; }
    void setAppendSystemPrompt(const QString& prompt) { a_systemPrompt = prompt; }

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
    /// \a allowEdits, edits need no asking for the rest of the session.
    void answer(const QString& id, bool allow, bool allowEdits = false);
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
    void toolStarted(const QString& id, const QString& tool, const QString& subject);
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
    void withdrawRequests();
};

} // namespace qucs_s::claude

Q_DECLARE_METATYPE(qucs_s::claude::PermissionRequest)
Q_DECLARE_METATYPE(qucs_s::claude::TurnResult)

#endif // QUCS_CLAUDECODE_H
