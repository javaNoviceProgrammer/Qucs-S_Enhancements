/*
 * layoutrouting.h - the metal router (layoutrouter/router.h) against a
 *                   layout read for the viewer (layout.h): the problem
 *                   built from a cell's shapes and labels, and a routing
 *                   session for each layout file that Claude's route_*
 *                   tools and the viewer's Route panel share - its job,
 *                   kept beside the layout as <layout>.route.json, its run
 *                   in the background, its last result and check
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LAYOUTROUTING_H
#define QUCS_LAYOUTROUTING_H

#include "layout.h"
#include "layoutrouter/router.h"
#include "layoutrouter/router_job.h"
#include "layoutrouter/router_verify.h"

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QPointer>

#include <atomic>
#include <memory>

class QThread;

namespace qucs_s::markup {
struct Stroke;
}

namespace qucs_s::routing {

/// The router's problem for \a job on \a cell of \a layout with \a tech:
/// the shapes on the technology's metals, cuts and obstacle layers (none
/// from the job's route cell, which a commit replaces), its terminals
/// (a label's text found in the cell), classes, guides, keep-outs and
/// options. False, and why in \a error, when a name in the job is not
/// there.
bool buildProblem(const layout::Layout& layout, int cell, const router::Job& job, const router::Technology& tech, router::Problem* out,
                  QStringList* warnings, QString* error);

/// A pen stroke as the router takes it: a ring (or anything closed) as a
/// keep-out's outline - its box when \a box -, a line as a guide.
QVector<QPointF> strokeOutline(const markup::Stroke& stroke, bool box);
QVector<QPointF> strokeGuide(const markup::Stroke& stroke);

class Session : public QObject
{
    Q_OBJECT

public:
    /// The session of \a layoutFile (made the first time, its job read from
    /// beside the file when there is one), or null when not \a create.
    static Session* of(const QString& layoutFile, bool create = true);
    static QString jobFileOf(const QString& layoutFile) { return layoutFile + QStringLiteral(".route.json"); }

    QString file() const { return a_file; }
    QString jobFile() const { return jobFileOf(a_file); }

    /// What is to be routed. Change it, then call jobChanged().
    router::Job job;
    /// The technology the job names: its file (relative to the layout's
    /// folder) or itself. False, and why, when it cannot be read.
    bool technology(router::Technology* t, QString* error) const;
    bool saveJob(QString* error) const;
    bool loadJob(QString* error);
    void jobChanged();
    /// The layout's file was read again: what was prepared is of the old one
    /// (the job, and the last result, stand).
    void layoutChanged() { a_prepared.reset(); }

    /// The problem and a prepared router for the job on \a layout: terminal
    /// readings, crowding and the memory a run needs, without routing.
    bool prepare(std::shared_ptr<const layout::Layout> layout, QString* error);
    std::shared_ptr<const layout::Layout> layout() const { return a_layout; }
    std::shared_ptr<router::Router> prepared() const { return a_prepared; }
    QStringList warnings() const { return a_warnings; }

    /// Routes in the background: \a which by name (all when empty), or
    /// with \a changedOnly what changed since the last result (and the
    /// nets not yet routed), the rest kept. False, and why, when it cannot
    /// start (nothing prepared, a run going on).
    bool start(const QStringList& which, bool changedOnly, QString* error);
    bool isRunning() const { return a_thread != nullptr; }
    void cancel();
    /// Waits for a run to end, \a ms at most, the events going on.
    bool wait(int ms);
    QVector<router::PassRow> passesSoFar() const;
    QDateTime startedAt() const { return a_started; }
    QString runMode() const { return a_mode; }

    bool hasResult() const { return a_resultRouter != nullptr; }
    const router::Result& result() const { return a_result; }
    const router::Verification& verification() const { return a_verification; }
    /// The problem the result was made of (its nets, technology, unit).
    const router::Problem* resultProblem() const;
    QDateTime finishedAt() const { return a_finished; }
    /// Whether the job or layout changed since the result was made.
    bool resultIsOld() const { return a_resultJobRevision != a_jobRevision; }
    /// The routes of the last result, to keep in a run of what changed.
    QVector<router::Kept> keptRoutes() const;
    /// A net's index in the result's problem, or -1.
    int resultNet(const QString& name) const;

    /// Cancels and waits for every session's run (at quit).
    static void stopAll();

signals:
    void changedJob();
    void passFinished(const qucs_s::router::PassRow& row);
    void runFinished();
    void wasPrepared();

private:
    explicit Session(const QString& file);
    ~Session() override;
    QString a_file;
    std::shared_ptr<const layout::Layout> a_layout;
    std::shared_ptr<router::Router> a_prepared;
    QStringList a_warnings;
    quint64 a_jobRevision = 1;

    QThread* a_thread = nullptr;
    std::shared_ptr<std::atomic_bool> a_cancel;
    mutable QMutex a_mutex;
    QVector<router::PassRow> a_live;
    QDateTime a_started;
    QString a_mode;

    std::shared_ptr<router::Router> a_resultRouter;
    router::Result a_result;
    router::Verification a_verification;
    QDateTime a_finished;
    quint64 a_resultJobRevision = 0;
};

} // namespace qucs_s::routing

Q_DECLARE_METATYPE(qucs_s::router::PassRow)

#endif // QUCS_LAYOUTROUTING_H
