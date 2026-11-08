/*
 * simulationruns.h - every simulation, solve, sweep and optimization that runs,
 *                    the user's and Claude's, in the one list of jobs
 *                    (claudejobs.h); documents kept running with their tab
 *                    closed; the tabs closed lately
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_SIMULATIONRUNS_H
#define QUCS_SIMULATIONRUNS_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QucsApp;
class QucsDoc;
class QTimer;
class QWidget;
class Schematic;
class SimMessage;
class SimulationRun;

namespace qucs_s::runs {

/// What a document is doing now, as read from it.
struct Activity {
    bool busy = false;      ///< closing it now would lose work
    bool listed = false;    ///< a run to list as it begins (a solve, a sweep); else listed once it has taken a while
    QString kind;           ///< as a job's kind: simulation, solve, optimization, convergence, mesh, build
    QString what;           ///< in words, for the job's name: "study std1", "ngspice"
    int percent = -1;       ///< 0 to 100 as its own bar shows it; -1: it says none
    QString said;           ///< what it says of how it goes
    QString whyNoPercent;   ///< why there is no percentage, for this kind
};

/// What \a doc - a 2D or 3D multiphysics model, an RF 2.5D model - runs now.
/// A schematic's simulation is told to the tracker as it starts (it lives in
/// the console, not in the document): nothing for one here.
Activity activityOf(QucsDoc* doc);

/// Asks \a doc's run to stop (Cancel, as its own button does); false when it
/// has none to stop, or none that can be stopped from here.
bool stopActivity(QucsDoc* doc);

/*!
 * The runs of the window's documents: each listed in the jobs' Registry as it
 * begins (a moment's rebuild after an edit only past kListedPast), with how far
 * it has got and a Stop, and ended with how it ended. A run Claude's tool began
 * is that tool's job already (QucsControl): the lookup finds it, and the run is
 * not listed twice.
 *
 * Documents kept running without their tab are held here (park()), hidden in
 * the main window, until their tab comes back (unpark()), they are let go
 * (releaseParked()), or - finished and unchanged - more are kept than
 * kIdleParkedMost.
 */
class Tracker : public QObject
{
    Q_OBJECT

public:
    /// Rebuilds and other short work are listed once they take this long.
    static constexpr int kListedPast = 2000;   // ms
    /// How often the documents are read.
    static constexpr int kPollEvery = 400;     // ms
    /// The tabs closed lately that are remembered.
    static constexpr int kClosedMost = 10;
    /// Finished documents without a tab kept for their results, at most.
    static constexpr int kIdleParkedMost = 8;

    explicit Tracker(QucsApp* app);
    ~Tracker() override;

    // What a document runs.
    Activity activity(QucsDoc* doc) const;
    bool isBusy(QucsDoc* doc) const { return activity(doc).busy; }
    /// Asks the run of \a doc to stop; false when nothing could be asked.
    bool stop(QucsDoc* doc);
    /// What runs now, a line each: "amp.sch - simulation (ngspice), 42 %".
    QStringList runningNow() const;

    // A schematic's simulation, told as it starts (QucsApp's two places).
    void watchSpiceRun(SimulationRun* run, Schematic* schematic);
    void watchQucsatorRun(SimMessage* sim, QWidget* document);

    // Documents kept running without their tab.
    /// Takes \a document, its tab already gone: hidden, held, its run going on.
    void park(QWidget* document);
    bool isParked(const QObject* document) const;
    QList<QWidget*> parked() const;
    /// The one held for \a file (its name as given, or the same file), else null.
    QWidget* parkedFile(const QString& file) const;
    /// Lets go of \a document's holding: the caller gives it a tab.
    void unpark(QWidget* document);
    /// Those held with changes not saved.
    QList<QWidget*> parkedWithChanges() const;
    /// Every one held deleted (its run stopped as its document goes).
    void releaseParked();

    // The tabs closed lately, the newest first.
    struct Closed {
        QString file;                ///< its file (empty: none)
        QString title;               ///< its tab's words
        QPointer<QWidget> parked;    ///< held, running or finished: the same document comes back
        QDateTime when;
    };
    void noteClosed(const QString& file, const QString& title, QWidget* parked = nullptr);
    QList<Closed> closed() const;
    /// The \a index-th of closed() taken off the list.
    Closed takeClosed(int index);
    void clearClosed();

    // Quitting: no question of keeping a run, the documents' own save questions.
    void setQuitting(bool quitting) { a_quitting = quitting; }
    bool quitting() const { return a_quitting; }

    /// The job listing \a document's run (its last, while it is still listed);
    /// empty: none.
    QString jobOf(const QObject* document) const;
    /// The document of job \a id while it is known here (open or held), else null.
    QWidget* documentOfJob(const QString& id) const;
    /// The job of the latest run of \a document, ended or not; empty: none.
    QString jobOfParked(const QObject* document) const;
    /// \a document, held, let go: its run stopped, deleted.
    void releaseOne(QWidget* document);

    /// Claude's job for a run of \a document beginning now (a tool's that
    /// follows it: simulate, fem_solve...), its id; empty: none. \a caller is
    /// set to the conversation whose call is under way on it, if any.
    using ClaudeLookup = std::function<QString(QObject* document, quint64* caller)>;
    void setClaudeLookup(ClaudeLookup lookup) { a_claude = std::move(lookup); }

    /// Reads every document now (the timer does, every kPollEvery).
    void pollNow();

signals:
    /// Something runs, ended, was kept or brought back, or a tab closed.
    void changed();

private:
    struct Run {
        QPointer<QObject> document;
        QString file;                  // its file as it began, for the job's name
        QString job;                   // the listed job; empty: not yet
        bool claudes = false;          // a job of Claude's tool: it ends that job
        quint64 caller = 0;            // a call of Claude's under way on it as it began
        quint64 stopsBefore = 0;       // a model's stops as it began
        QElapsedTimer age;
        Activity last;
        QPointer<SimulationRun> spice;
        QPointer<SimMessage> qucsator;
        bool spiceRun = false, qucsatorRun = false;
        bool stopAsked = false;
    };

    QList<QucsDoc*> documents() const;
    Run* runOf(const QObject* document);
    const Run* runOf(const QObject* document) const;
    void begin(QucsDoc* doc, const Activity& now);
    void list(Run& run);
    void end(QObject* document, int status, const QString& outcome, int percent = -1);
    void endModelRun(Run& run, QucsDoc* doc);
    void follow(QObject* document);
    void rememberJob(const Run& run);
    void trimParked();
    void pollSoon();
    QString titleOf(QObject* document) const;
    bool stopRun(QObject* document);

    QucsApp* a_app;
    QWidget* a_holder = nullptr;     // the parent of those held: hidden
    QList<QPointer<QWidget>> a_parked;   // the oldest first
    QList<Closed> a_closed;              // the newest first
    QList<Run> a_runs;
    QSet<QObject*> a_followed;           // documents whose signals are watched
    QList<QPair<QString, QPointer<QObject>>> a_jobDocuments;   // each listed run's job and its document, the oldest first
    ClaudeLookup a_claude;
    QTimer* a_poll = nullptr;
    QTimer* a_soon = nullptr;
    bool a_quitting = false;
};

} // namespace qucs_s::runs

#endif // QUCS_SIMULATIONRUNS_H
