/*
 * claudejobs.cpp - the jobs Claude has run
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "claudejobs.h"

#include "claudehistory.h"
#include "misc.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>

namespace qucs_s::claude::jobs {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ClaudeJobs", text);
}

// What a damaged or endless message may leave of a job.
constexpr int kNameMost = 300;
constexpr int kDetailMost = 8000;
constexpr int kSaidMost = 500;
constexpr qint64 kKeptMost = 8 * 1024 * 1024;   // a file past this is no list of ours

const char* const kKinds[][2] = {
    {"local_bash", "command"}, {"local_agent", "agent"},     {"local_workflow", "workflow"},
    {"monitor", "monitor"},    {"monitor_mcp", "monitor"},   {"monitor_ws", "monitor"},
    {"remote_agent", "agent"}, {"in_process_teammate", "agent"},
};

QString kindOfTask(const QString& type)
{
    for (const auto& k : kKinds)
        if (type == QLatin1String(k[0])) return QLatin1String(k[1]);
    return QStringLiteral("task");
}

QString whyNoPercentOfTask()
{
    return tr("Claude Code says that it runs and when it ends, not how much of it is done: nothing knows the whole of it.");
}

// A number a message gives, as a whole count not below nought.
qint64 count(const QJsonValue& value)
{
    const double d = value.toDouble(-1);
    return d >= 0 && d < 1e15 ? qint64(d) : -1;
}

QString usageText(const QJsonObject& usage)
{
    QStringList parts;
    if (const qint64 tools = count(usage.value(QLatin1String("tool_uses"))); tools > 0) parts << tr("%1 tool use(s)").arg(tools);
    if (const qint64 tokens = count(usage.value(QLatin1String("total_tokens"))); tokens > 0) parts << tr("%1 tokens").arg(tokens);
    return parts.join(QStringLiteral(", "));
}

} // namespace

bool isOver(Status status)
{
    return status == Status::Finished || status == Status::Failed || status == Status::Stopped || status == Status::Lost;
}

QString statusKey(Status status)
{
    switch (status) {
    case Status::Queued: return QStringLiteral("queued");
    case Status::Started: return QStringLiteral("started");
    case Status::Running: return QStringLiteral("running");
    case Status::Stopping: return QStringLiteral("stopping");
    case Status::Finished: return QStringLiteral("finished");
    case Status::Failed: return QStringLiteral("failed");
    case Status::Stopped: return QStringLiteral("stopped");
    case Status::Lost: return QStringLiteral("lost");
    }
    return QStringLiteral("lost");
}

QString statusText(Status status)
{
    switch (status) {
    case Status::Queued: return tr("queued");
    case Status::Started: return tr("started");
    case Status::Running: return tr("running");
    case Status::Stopping: return tr("stopping");
    case Status::Finished: return tr("finished");
    case Status::Failed: return tr("failed");
    case Status::Stopped: return tr("stopped");
    case Status::Lost: return tr("lost");
    }
    return tr("lost");
}

Status statusOfKey(const QString& key)
{
    for (const Status s : {Status::Queued, Status::Started, Status::Running, Status::Stopping, Status::Finished, Status::Failed, Status::Stopped})
        if (key == statusKey(s)) return s;
    return Status::Lost;
}

QString oneLine(const QString& text, int most)
{
    QString line = text.simplified();
    if (line.size() > most) line = line.left(most - 1) + QChar(0x2026);
    return line;
}

QString durationText(qint64 seconds)
{
    seconds = std::max<qint64>(0, seconds);
    if (seconds < 60) return tr("%1 s").arg(seconds);
    if (seconds < 3600) return tr("%1 min %2 s").arg(seconds / 60).arg(seconds % 60);
    return tr("%1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString percentText(const Job& job)
{
    if (job.percent >= 0) return QStringLiteral("%1 %").arg(job.percent);
    return isOver(job.status) ? QStringLiteral("-") : tr("unknown");
}

QString startedBy(const Job& job)
{
    return job.source == QLatin1String("user") ? QStringLiteral("user") : QStringLiteral("claude");
}

QJsonObject toJson(const Job& job, quint64 caller, bool whole)
{
    QJsonObject o{{QStringLiteral("id"), job.id},
                  {QStringLiteral("source"), job.source},
                  {QStringLiteral("kind"), job.kind},
                  {QStringLiteral("name"), job.name},
                  {QStringLiteral("status"), statusKey(job.status)},
                  {QStringLiteral("percent"), job.percent >= 0 ? QJsonValue(job.percent) : QJsonValue(QJsonValue::Null)},
                  {QStringLiteral("progress"), job.percent >= 0 ? QStringLiteral("%1 %").arg(job.percent)
                                               : isOver(job.status) ? QStringLiteral("not known") : QStringLiteral("unknown")},
                  {QStringLiteral("background"), job.background},
                  {QStringLiteral("can_stop"), job.canStop && !isOver(job.status)},
                  {QStringLiteral("started_by"), startedBy(job)}};
    if (!job.document.isEmpty()) o.insert(QStringLiteral("document"), job.document);
    if (job.percent < 0 && !job.whyNoPercent.isEmpty()) o.insert(QStringLiteral("why_no_percent"), job.whyNoPercent);
    if (!job.canStop && !isOver(job.status) && !job.whyNoStop.isEmpty()) o.insert(QStringLiteral("why_no_stop"), job.whyNoStop);
    if (!job.conversation.isEmpty()) o.insert(QStringLiteral("conversation"), job.conversation);
    if (caller != 0 && job.caller == caller) o.insert(QStringLiteral("yours"), true);
    if (job.queued.isValid()) o.insert(QStringLiteral("queued"), job.queued.toString(Qt::ISODate));
    if (job.began.isValid()) o.insert(QStringLiteral("started"), job.began.toString(Qt::ISODate));
    if (job.ended.isValid()) o.insert(QStringLiteral("ended"), job.ended.toString(Qt::ISODate));
    if (job.began.isValid()) {
        const QDateTime until = job.ended.isValid() ? job.ended : QDateTime::currentDateTime();
        o.insert(QStringLiteral("seconds"), std::max<qint64>(0, job.began.secsTo(until)));
    }
    if (!job.said.isEmpty()) o.insert(QStringLiteral("said"), job.said);
    if (!job.outcome.isEmpty()) o.insert(QStringLiteral("outcome"), job.outcome);
    if (whole) {
        if (!job.detail.isEmpty()) o.insert(QStringLiteral("detail"), job.detail);
        if (!job.tool.isEmpty()) o.insert(QStringLiteral("tool"), job.tool);
        if (!job.taskId.isEmpty()) o.insert(QStringLiteral("task_id"), job.taskId);
        if (!job.outputFile.isEmpty()) o.insert(QStringLiteral("output_file"), job.outputFile);
        if (!job.parentTask.isEmpty()) o.insert(QStringLiteral("parent_task"), job.parentTask);
    }
    return o;
}

// ----------------------------------------------------------------------
Registry& Registry::instance()
{
    // (Never deleted: a conversation or a tool may still tell of its end as
    // the application goes down.)
    static Registry* registry = new Registry;
    return *registry;
}

Registry::Registry(QObject* parent)
    : QObject(parent)
{
}

QList<Job> Registry::jobs() const
{
    QList<Job> list;
    list.reserve(a_entries.size());
    for (qsizetype i = a_entries.size() - 1; i >= 0; --i) list.append(a_entries.at(i).job);
    return list;
}

Registry::Entry* Registry::entry(const QString& id)
{
    for (Entry& e : a_entries)
        if (e.job.id == id) return &e;
    return nullptr;
}

const Registry::Entry* Registry::entry(const QString& id) const
{
    for (const Entry& e : a_entries)
        if (e.job.id == id) return &e;
    return nullptr;
}

std::optional<Job> Registry::job(const QString& id) const
{
    if (const Entry* e = entry(id)) return e->job;
    return std::nullopt;
}

QString Registry::add(Job job, Stop stop)
{
    job.id = QStringLiteral("job-%1").arg(++a_serial);
    if (job.conversation.isEmpty()) job.conversation = a_conversations.value(job.caller);
    if (!job.began.isValid() && job.status != Status::Queued) job.began = QDateTime::currentDateTime();
    const QString id = job.id;
    a_entries.append({std::move(job), std::move(stop)});
    emit added(id);
    trim();
    changedSoon();
    if (const Entry* e = entry(id); e != nullptr && e->job.status == Status::Started) tickSoon();
    return id;
}

void Registry::tickSoon()
{
    if (a_ticker == nullptr) {
        a_ticker = new QTimer(this);
        a_ticker->setInterval(1000);
        connect(a_ticker, &QTimer::timeout, this, &Registry::tick);
    }
    if (!a_ticker->isActive()) a_ticker->start();
}

void Registry::trim()
{
    while (a_entries.size() > kMost) {
        qsizetype at = 0;   // (the oldest; of those over, if any is)
        for (qsizetype i = 0; i < a_entries.size(); ++i)
            if (isOver(a_entries.at(i).job.status)) {
                at = i;
                break;
            }
        const Entry gone = a_entries.takeAt(at);
        for (auto it = a_tasks.begin(); it != a_tasks.end();)
            if (it.value() == gone.job.id) it = a_tasks.erase(it);
            else ++it;
        emit removed(gone.job.id);
    }
}

void Registry::update(const QString& id, const std::function<void(Job&)>& change)
{
    Entry* e = entry(id);
    if (e == nullptr) return;
    const Job before = e->job;
    change(e->job);
    Job& job = e->job;
    job.percent = std::clamp(job.percent, -1, 100);
    if (isOver(before.status)) {
        // Its end stays as it was seen first; what it said after is kept.
        job.status = before.status;
        job.ended = before.ended;
    } else if (isOver(job.status)) {
        if (!job.ended.isValid()) job.ended = QDateTime::currentDateTime();
        if (!job.began.isValid()) job.began = job.ended;
        job.canStop = false;
        e->stop = {};
    } else if (job.status != Status::Queued && !job.began.isValid()) {
        job.began = QDateTime::currentDateTime();
    }
    const bool same = job.status == before.status && job.percent == before.percent && job.said == before.said
                      && job.outcome == before.outcome && job.canStop == before.canStop && job.whyNoStop == before.whyNoStop
                      && job.background == before.background && job.name == before.name && job.detail == before.detail
                      && job.began == before.began && job.ended == before.ended && job.outputFile == before.outputFile
                      && job.stopAsked == before.stopAsked && job.conversation == before.conversation
                      && job.whyNoPercent == before.whyNoPercent && job.document == before.document;
    if (same) return;
    if (job.status == Status::Started) tickSoon();
    const bool statusChanged = job.status != before.status;
    emit changed(id);
    // (Not at each percent: the file holds what ended, and how.)
    if (statusChanged) changedSoon();
}

void Registry::setStop(const QString& id, Stop stop)
{
    if (Entry* e = entry(id)) e->stop = std::move(stop);
}

bool Registry::stop(const QString& id, QString* why)
{
    Entry* e = entry(id);
    const auto refuse = [why](const QString& text) {
        if (why != nullptr) *why = text;
        return false;
    };
    if (e == nullptr) return refuse(tr("There is no such job."));
    if (isOver(e->job.status)) return refuse(tr("It has ended already."));
    if (!e->job.canStop || !e->stop) return refuse(e->job.whyNoStop.isEmpty() ? tr("It cannot be stopped.") : e->job.whyNoStop);
    if (e->job.status == Status::Stopping) return true;   // (asked already)
    e->beforeStop = e->job.status;
    const int serial = ++e->stopSerial;
    // (A copy: stopping may end the job at once, and take its stop away.)
    const Stop asked = e->stop;
    update(id, [](Job& job) {
        job.status = Status::Stopping;
        job.stopAsked = true;
    });
    if (!asked()) {
        stopRefused(id, tr("It could not be asked to stop."));
        return refuse(tr("It could not be asked to stop."));
    }
    // Its end not seen in time: said, and Stop on again. Still not called
    // stopped.
    QTimer::singleShot(a_patience, this, [this, id, serial] {
        Entry* e = entry(id);
        if (e == nullptr || e->stopSerial != serial || e->job.status != Status::Stopping) return;
        const Status back = e->beforeStop;
        update(id, [back](Job& job) {
            job.status = back;
            job.stopAsked = false;
            job.said = tr("Asked to stop; it has not stopped yet.");
        });
    });
    return true;
}

void Registry::stopRefused(const QString& id, const QString& why)
{
    Entry* e = entry(id);
    if (e == nullptr || e->job.status != Status::Stopping) return;
    const Status back = e->beforeStop;
    ++e->stopSerial;
    update(id, [back, why](Job& job) {
        job.status = back;
        job.stopAsked = false;
        job.said = tr("It would not stop: %1").arg(oneLine(why, kSaidMost));
    });
}

int Registry::clearFinished()
{
    QStringList gone;
    for (qsizetype i = a_entries.size() - 1; i >= 0; --i)
        if (isOver(a_entries.at(i).job.status)) {
            gone << a_entries.at(i).job.id;
            a_entries.removeAt(i);
        }
    if (gone.isEmpty()) return 0;
    for (auto it = a_tasks.begin(); it != a_tasks.end();)
        if (gone.contains(it.value())) it = a_tasks.erase(it);
        else ++it;
    for (const QString& id : std::as_const(gone)) emit removed(id);
    changedSoon();
    return int(gone.size());
}

void Registry::nameConversation(quint64 caller, const QString& name)
{
    if (caller == 0 || a_conversations.value(caller) == name) return;
    a_conversations.insert(caller, name);
    QStringList ids;
    for (const Entry& e : std::as_const(a_entries))
        if (e.job.caller == caller) ids << e.job.id;
    for (const QString& id : std::as_const(ids)) update(id, [name](Job& job) { job.conversation = name; });
}

QString Registry::taskJob(quint64 caller, const QString& task) const
{
    return a_tasks.value(QStringLiteral("%1:%2").arg(caller).arg(task));
}

void Registry::taskEvent(quint64 caller, const QJsonObject& m, const QString& detail, const QDateTime& began,
                         const std::function<bool(const QString&)>& stop)
{
    const QString subtype = m.value(QLatin1String("subtype")).toString();
    const QString task = m.value(QLatin1String("task_id")).toString().left(200);
    if (task.isEmpty()) return;
    const QString key = QStringLiteral("%1:%2").arg(caller).arg(task);
    QString id = a_tasks.value(key);
    const QString description = oneLine(m.value(QLatin1String("description")).toString(), kNameMost);

    const auto list = [&](Status status) {
        Job job;
        job.source = QStringLiteral("claude-code");
        job.kind = kindOfTask(m.value(QLatin1String("task_type")).toString());
        job.name = description.isEmpty() ? task : description;
        job.status = status;
        job.whyNoPercent = whyNoPercentOfTask();
        job.caller = caller;
        job.taskId = task;
        job.tool = m.value(QLatin1String("tool_use_id")).toString().left(200);
        job.canStop = bool(stop);
        if (status == Status::Queued) job.queued = QDateTime::currentDateTime();
        else job.began = began;
        id = add(job, stop ? Stop([stop, task] { return stop(task); }) : Stop());
        a_tasks.insert(key, id);
    };

    if (subtype == QLatin1String("task_started")) {
        // (What is no activity, as the program says - a watcher, its own
        // housekeeping - is left out, as it asks of its hosts.)
        if (m.value(QLatin1String("ambient")).toBool() && id.isEmpty()) return;
        if (!id.isEmpty()) {
            if (const Entry* e = entry(id); e != nullptr && isOver(e->job.status)) {
                // Going on again (an agent resumed keeps its id): a new row
                // of its own, the old run's as it ended.
                a_tasks.remove(key);
                id.clear();
            }
        }
        if (id.isEmpty()) list(Status::Started);
        const QString prompt = m.value(QLatin1String("prompt")).toString().left(kDetailMost);
        update(id, [&](Job& job) {
            if (!description.isEmpty()) job.name = description;
            if (!detail.isEmpty()) job.detail = detail.left(kDetailMost);
            else if (!prompt.isEmpty()) job.detail = prompt;
            if (m.contains(QLatin1String("is_backgrounded"))) job.background = m.value(QLatin1String("is_backgrounded")).toBool();
            job.parentTask = m.value(QLatin1String("parent_task_id")).toString().left(200);
        });
    } else if (subtype == QLatin1String("task_updated")) {
        if (id.isEmpty()) return;   // (of no task told of: nothing to name it by)
        const QJsonObject patch = m.value(QLatin1String("patch")).toObject();
        const QString status = patch.value(QLatin1String("status")).toString();
        update(id, [&](Job& job) {
            if (patch.contains(QLatin1String("is_backgrounded"))) job.background = patch.value(QLatin1String("is_backgrounded")).toBool();
            if (patch.value(QLatin1String("description")).isString())
                job.name = oneLine(patch.value(QLatin1String("description")).toString(), kNameMost);
            if (patch.value(QLatin1String("error")).isString())
                job.outcome = oneLine(patch.value(QLatin1String("error")).toString(), kSaidMost);
            if (status == QLatin1String("pending")) {
                if (job.status != Status::Stopping) job.status = Status::Queued;
                if (!job.queued.isValid()) job.queued = QDateTime::currentDateTime();
            } else if (status == QLatin1String("running")) {
                if (job.status != Status::Stopping) job.status = Status::Running;
            } else if (status == QLatin1String("paused")) {
                job.said = tr("paused");
            } else if (status == QLatin1String("completed")) {
                job.status = Status::Finished;
                job.percent = 100;
            } else if (status == QLatin1String("failed")) {
                job.status = Status::Failed;
            } else if (status == QLatin1String("killed")) {
                job.status = Status::Stopped;
            }
        });
    } else if (subtype == QLatin1String("task_progress")) {
        if (id.isEmpty()) list(Status::Running);
        const QJsonObject usage = m.value(QLatin1String("usage")).toObject();
        const QString summary = oneLine(m.value(QLatin1String("summary")).toString(), kSaidMost);
        const QString tool = oneLine(m.value(QLatin1String("last_tool_name")).toString(), 80);
        update(id, [&](Job& job) {
            if (job.status == Status::Started || job.status == Status::Queued) job.status = Status::Running;
            QStringList said;
            if (!summary.isEmpty()) said << summary;
            else if (!description.isEmpty()) said << description;
            if (const QString used = usageText(usage); !used.isEmpty()) said << used;
            if (!tool.isEmpty()) said << tr("last %1").arg(tool);
            if (!said.isEmpty()) job.said = said.join(QStringLiteral(" - "));
        });
    } else if (subtype == QLatin1String("task_notification")) {
        if (id.isEmpty()) list(Status::Running);
        const QString status = m.value(QLatin1String("status")).toString();
        const QString summary = oneLine(m.value(QLatin1String("summary")).toString(), kSaidMost);
        const QString file = m.value(QLatin1String("output_file")).toString().left(4096);
        const QString used = usageText(m.value(QLatin1String("usage")).toObject());
        const QString reason = oneLine(m.value(QLatin1String("reason")).toString(), 80);
        update(id, [&](Job& job) {
            if (!file.isEmpty()) job.outputFile = file;
            // What it says of its end, unless it is no report: notes meant for
            // the model (an agent's report handed back: 'handback', with its
            // own id in them), or its name again (a command's).
            if (m.contains(QLatin1String("handback"))) job.outcome = tr("Its report went to Claude, as a message.");
            else if (!summary.isEmpty() && summary != job.name) job.outcome = summary;
            if (!used.isEmpty()) job.said = used;
            if (!reason.isEmpty()) job.said = reason;
            if (status == QLatin1String("completed")) {
                job.status = Status::Finished;
                job.percent = 100;
            } else if (status == QLatin1String("failed")) {
                job.status = Status::Failed;
            } else if (status == QLatin1String("stopped")) {
                job.status = Status::Stopped;
            } else {
                // (Over, as the message says, in words not known here.)
                job.status = Status::Lost;
                job.said = tr("Claude Code said it ended: %1").arg(oneLine(status, 80));
            }
        });
    }
}

void Registry::programEnded(quint64 caller, const QString& why)
{
    QStringList ids;
    for (const Entry& e : std::as_const(a_entries))
        if (e.job.caller == caller && e.job.source == QLatin1String("claude-code") && !isOver(e.job.status)) ids << e.job.id;
    for (const QString& id : std::as_const(ids))
        update(id, [why](Job& job) {
            job.status = Status::Lost;
            job.said = why;
        });
    for (auto it = a_tasks.begin(); it != a_tasks.end();)
        if (it.key().startsWith(QStringLiteral("%1:").arg(caller))) it = a_tasks.erase(it);
        else ++it;
}

void Registry::tick()
{
    // A job told to have begun, and not to have ended, a second ago: it runs
    // (whoever runs it says when it ends).
    const QDateTime now = QDateTime::currentDateTime();
    QStringList started;
    bool going = false;
    for (const Entry& e : std::as_const(a_entries)) {
        if (e.job.status == Status::Started) {
            going = true;
            if (e.job.began.isValid() && e.job.began.msecsTo(now) >= 1000) started << e.job.id;
        }
    }
    for (const QString& id : std::as_const(started))
        update(id, [](Job& job) {
            if (job.status == Status::Started) job.status = Status::Running;
        });
    if (!going) a_ticker->stop();
}

// ----------------------------------------------------------------------
// Kept across runs

QString Registry::keptFile()
{
    // (Beside the conversations: where a test's isolation puts those.)
    return QDir(history::directory()).filePath(QStringLiteral("background-jobs.json"));
}

void Registry::changedSoon()
{
    if (a_kept.isEmpty()) return;
    if (a_saver == nullptr) {
        a_saver = new QTimer(this);
        a_saver->setSingleShot(true);
        a_saver->setInterval(1000);
        connect(a_saver, &QTimer::timeout, this, &Registry::saveNow);
        connect(qApp, &QCoreApplication::aboutToQuit, this, &Registry::saveNow);
    }
    a_saver->start();
}

void Registry::setKept(const QString& file)
{
    if (file == a_kept) return;
    a_kept = file;
    readKept();
}

void Registry::readKept()
{
    // (A pipe where the file is would wait for ever; one past any list of
    // ours is left: 4a7186b.)
    const QFileInfo info(a_kept);
    if (!misc::isPlainFile(a_kept) || info.size() > kKeptMost) return;
    QFile file(a_kept);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonArray kept = QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("jobs")).toArray();
    QList<Job> read;
    for (const QJsonValue& v : kept) {
        const QJsonObject o = v.toObject();
        const QString name = oneLine(o.value(QLatin1String("name")).toString(), kNameMost);
        if (name.isEmpty()) continue;
        Job job;
        job.source = o.value(QLatin1String("source")).toString().left(40);
        job.kind = o.value(QLatin1String("kind")).toString().left(40);
        job.name = name;
        job.detail = o.value(QLatin1String("detail")).toString().left(kDetailMost);
        job.status = statusOfKey(o.value(QLatin1String("status")).toString());
        job.said = oneLine(o.value(QLatin1String("said")).toString(), kSaidMost);
        job.outcome = oneLine(o.value(QLatin1String("outcome")).toString(), kSaidMost);
        const double percent = o.value(QLatin1String("percent")).toDouble(-1);
        job.percent = percent >= 0 && percent <= 100 ? int(percent) : -1;
        job.whyNoPercent = oneLine(o.value(QLatin1String("why_no_percent")).toString(), kSaidMost);
        job.conversation = oneLine(o.value(QLatin1String("conversation")).toString(), kNameMost);
        job.background = o.value(QLatin1String("background")).toBool();
        job.tool = o.value(QLatin1String("tool")).toString().left(200);
        job.taskId = o.value(QLatin1String("task_id")).toString().left(200);
        job.outputFile = o.value(QLatin1String("output_file")).toString().left(4096);
        job.document = o.value(QLatin1String("document")).toString().left(4096);
        job.queued = QDateTime::fromString(o.value(QLatin1String("queued")).toString(), Qt::ISODate);
        job.began = QDateTime::fromString(o.value(QLatin1String("started")).toString(), Qt::ISODate);
        job.ended = QDateTime::fromString(o.value(QLatin1String("ended")).toString(), Qt::ISODate);
        // What was going when Qucs-S closed is not going now, and nothing
        // said how it ended.
        if (!isOver(job.status)) job.status = Status::Lost;
        if (!job.ended.isValid()) job.ended = job.began.isValid() ? job.began : QDateTime::currentDateTime();
        if (!job.began.isValid()) job.began = job.ended;
        read.append(job);
    }
    // (The newest are last, as they were written.)
    if (read.size() > kMost) read = read.mid(read.size() - kMost);
    for (const Job& job : std::as_const(read)) add(job);
}

void Registry::saveNow()
{
    if (a_kept.isEmpty()) return;
    QJsonArray list;
    for (const Entry& e : std::as_const(a_entries)) {
        QJsonObject o = toJson(e.job, 0, true);
        o.remove(QStringLiteral("can_stop"));
        // (Going on now: if this run ends, it was lost; read back as such.)
        list.append(o);
    }
    QDir().mkpath(QFileInfo(a_kept).absolutePath());
    QSaveFile file(a_kept);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("jobs"), list}}).toJson(QJsonDocument::Compact));
    file.commit();
}

void Registry::reset()
{
    const QStringList ids = [this] {
        QStringList all;
        for (const Entry& e : std::as_const(a_entries)) all << e.job.id;
        return all;
    }();
    a_entries.clear();
    a_tasks.clear();
    a_conversations.clear();
    a_kept.clear();
    a_patience = 20000;
    if (a_saver != nullptr) a_saver->stop();
    for (const QString& id : ids) emit removed(id);
}

} // namespace qucs_s::claude::jobs
