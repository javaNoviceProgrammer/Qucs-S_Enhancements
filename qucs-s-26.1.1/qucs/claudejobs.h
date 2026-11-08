/*
 * claudejobs.h - the jobs Claude has run: Claude Code's commands and agents,
 *                and Qucs-S's long tools, each with its status and how far
 *                it has got
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CLAUDEJOBS_H
#define QUCS_CLAUDEJOBS_H

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QTimer;

namespace qucs_s::claude::jobs {

/// Where a job stands, each from what was seen of it: a status never moves
/// on a guess. Stopping is a stop asked for; only the job's end says Stopped.
enum class Status {
    Queued,     ///< waits its turn
    Started,    ///< just begun
    Running,    ///< under way
    Stopping,   ///< a stop was asked for; nothing has yet said that it ended
    Finished,   ///< ended, and well
    Failed,     ///< ended, and said it failed
    Stopped,    ///< ended because it was stopped
    Lost,       ///< no longer known: what ran it ended without saying how it ended
};

/// Whether a job in \a status has ended (or is lost): Clear Finished takes it.
bool isOver(Status status);
/// The status as tools give it, always in English: "running".
QString statusKey(Status status);
/// The status as the window shows it, translated.
QString statusText(Status status);
/// A statusKey() read back; Lost for what no status says.
Status statusOfKey(const QString& key);

/// What the window and the tools know of one job.
struct Job {
    QString id;           ///< "job-12": the same as long as it is listed
    QString source;       ///< "claude-code" (the program's task), "qucs-s" (a tool of ours), or "user" (a run the user started)
    QString kind;         ///< command, agent, workflow, monitor, task; simulation, solve, build, tune, script, batch, tool
    QString name;         ///< what it is, in one line
    QString detail;       ///< more of it: the whole command, a prompt, a tool's arguments
    Status status = Status::Started;
    /// How far, 0 to 100, as the job itself reports it; -1: it has not said,
    /// or it cannot. Never worked out from the time it has taken.
    int percent = -1;
    QString whyNoPercent;   ///< why it is unknown, for this kind of job
    QString said;           ///< the last it said of how it goes
    QString outcome;        ///< what it said as it ended (a summary, an error's first line)
    QDateTime queued;       ///< when it began to wait (invalid: it never did)
    QDateTime began;        ///< when it began (invalid: it has not)
    QDateTime ended;        ///< when it ended (invalid: it has not)
    quint64 caller = 0;     ///< the conversation it is of, in this run of Qucs-S (0: an earlier run's)
    QString conversation;   ///< that conversation's name
    bool background = false;   ///< in the background: Claude does not wait on it
    bool canStop = false;      ///< the Stop button is on
    QString whyNoStop;         ///< why it is off, while the job runs
    bool stopAsked = false;    ///< a stop was asked for
    QString tool;              ///< the Qucs-S tool, or the program's tool use's id
    QString taskId;            ///< the program's id of it
    QString outputFile;        ///< where the program writes its output
    QString parentTask;        ///< the agent's task it runs under
    QString document;          ///< the file it works on (a run's, a solve's); empty: none, or not known
};
/// Who started \a job: "user" (a run of the user's own) or "claude".
QString startedBy(const Job& job);

/// One line of \a text, cut to \a most characters.
QString oneLine(const QString& text, int most);
/// "8 s", "2 min 5 s", "1 h 3 min": how long \a seconds is.
QString durationText(qint64 seconds);
/// The percentage as the window shows it: "42 %", or "unknown" while it runs,
/// "-" when it ended without saying.
QString percentText(const Job& job);
/// A job for the tools: {id, source, kind, name, status, percent (null when
/// unknown), progress, why_no_percent, started, ended, seconds, ...}. With
/// \a whole: its detail and output file too; \a caller marks 'yours'.
QJsonObject toJson(const Job& job, quint64 caller, bool whole);

/*!
 * The jobs of this run of Qucs-S, every conversation's, in one list: one
 * window and the tools read it. Whoever runs a job tells it what happens
 * (add(), update(), taskEvent()), and how the job is stopped.
 *
 * Kept (setKept()): those that ended, across runs. A job running when Qucs-S
 * closes comes back Lost.
 */
class Registry : public QObject
{
    Q_OBJECT

public:
    /// The most listed: beyond it the oldest go first, those over before any
    /// still running.
    static constexpr int kMost = 500;

    /// Asks the job to stop; false when it cannot be asked.
    using Stop = std::function<bool()>;

    static Registry& instance();

    /// The jobs, the newest first.
    QList<Job> jobs() const;
    std::optional<Job> job(const QString& id) const;
    int count() const { return int(a_entries.size()); }

    /// Lists \a job (its id given here), and says how it stops. Its id.
    QString add(Job job, Stop stop = {});
    /// Changes the job \a id; told only when something changed. An
    /// ended job's end stays as it is (its status and time), unless \a change
    /// is begin()'s.
    void update(const QString& id, const std::function<void(Job&)>& change);
    /// The way to stop it, from now on.
    void setStop(const QString& id, Stop stop);
    /// Stop pressed: asks the job to stop. Its status is Stopping until its
    /// end is seen - or, after setPatience(), back to what it was, the button
    /// on again. False, and why in \a why, when it cannot be asked.
    bool stop(const QString& id, QString* why = nullptr);
    /// The job said it would not stop (\a why): it goes on as it was.
    void stopRefused(const QString& id, const QString& why);
    /// How long a stop asked for may take before it is said to have no
    /// effect yet (ms): 20 s unless set.
    void setPatience(int ms) { a_patience = ms; }
    /// Takes away those over - finished, failed, stopped, lost - and none
    /// still going. How many.
    int clearFinished();

    /// What the conversation \a caller is called, for its jobs.
    void nameConversation(quint64 caller, const QString& name);
    QString conversationName(quint64 caller) const { return a_conversations.value(caller); }

    /// A message of the program's about its tasks (type "system", subtype
    /// task_started, task_updated, task_progress, task_notification) in the
    /// conversation \a caller. \a detail: what its tool use said (the
    /// command); \a began: when that tool use came, which a foreground
    /// command's task comes seconds after (invalid: not known); \a stop: how a
    /// task id is stopped. A damaged message is left out.
    void taskEvent(quint64 caller, const QJsonObject& message, const QString& detail, const QDateTime& began,
                   const std::function<bool(const QString& task)>& stop);
    /// The task \a task of \a caller, by the program's id: its job's id, or
    /// empty.
    QString taskJob(quint64 caller, const QString& task) const;
    /// The program of the conversation \a caller ended: its tasks not told
    /// to have ended are Lost, for \a why.
    void programEnded(quint64 caller, const QString& why);

    /// Keeps those that end in \a file, from now on, reading what it holds.
    /// (The dock's; a file that cannot be read is left, and nothing read.)
    void setKept(const QString& file);
    /// The default file: beside the kept conversations (history::directory()).
    static QString keptFile();
    /// Writes the file now, the running ones as Lost.
    void saveNow();

    /// Forgets everything, the file not named any more (the tests').
    void reset();

signals:
    void added(const QString& id);
    void changed(const QString& id);
    void removed(const QString& id);

private:
    explicit Registry(QObject* parent = nullptr);

    struct Entry {
        Job job;
        Stop stop;
        int stopSerial = 0;
        Status beforeStop = Status::Running;
    };
    Entry* entry(const QString& id);
    const Entry* entry(const QString& id) const;
    void trim();
    void tick();
    void tickSoon();
    void changedSoon();
    void readKept();

    QList<Entry> a_entries;              // the oldest first
    QHash<QString, QString> a_tasks;     // "<caller>:<task id>" -> job id
    QHash<quint64, QString> a_conversations;
    quint64 a_serial = 0;
    int a_patience = 20000;
    QString a_kept;                      // the file; empty: nothing kept
    QTimer* a_saver = nullptr;           // a moment after the last change
    QTimer* a_ticker = nullptr;          // Started becomes Running
};

} // namespace qucs_s::claude::jobs

#endif // QUCS_CLAUDEJOBS_H
