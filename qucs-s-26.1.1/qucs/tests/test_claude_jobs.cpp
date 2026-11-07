/*
 * The Background Jobs window and what it reads (claudejobs.h): a job's status
 * from the program's own events, a real percentage against an unknown one,
 * Stop saying stopped only on evidence, Clear Finished, the dock's tabs, what
 * is kept across runs, and damaged input. A fake claude program stands in for
 * the real one; the Qucs-S tools' side is in test_mcp_server.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include <memory>
#include <tuple>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

#include "claudecode.h"
#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "claudejobs.h"
#include "claudejobswindow.h"
#include "isolated_settings.h"

using qucs_s::claude::Session;
using namespace qucs_s::claude::jobs;

// (After the class: moc reads this text as C++, and lost the class in it.)
extern const char* const kFake;

static QByteArray line(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

static QJsonObject started(const QString& task, const QString& description, const char* type = "local_bash")
{
    return {{"type", "system"}, {"subtype", "task_started"}, {"task_id", task}, {"description", description},
            {"task_type", type}, {"is_backgrounded", true}, {"tool_use_id", "use-" + task}};
}

static QJsonObject patched(const QString& task, const QString& status)
{
    return {{"type", "system"}, {"subtype", "task_updated"}, {"task_id", task}, {"patch", QJsonObject{{"status", status}}}};
}

static QJsonObject ended(const QString& task, const QString& status, const QString& summary = {})
{
    return {{"type", "system"}, {"subtype", "task_notification"}, {"task_id", task}, {"status", status},
            {"output_file", "/tmp/" + task + ".output"}, {"summary", summary}};
}

static QString onlyJob()
{
    const QList<Job> jobs = Registry::instance().jobs();
    return jobs.size() == 1 ? jobs.first().id : QString();
}


class TestClaudeJobs : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString fake()
    {
        QFile f(dir.filePath("fake-claude"));
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(kFake);
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return f.fileName();
    }

    QString read(const QString& name)
    {
        QFile f(dir.filePath(name));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

    static QProgressBar* barOf(ClaudeJobsWindow* w, int row)
    {
        return w->table()->cellWidget(row, ClaudeJobsWindow::Progress)->findChild<QProgressBar*>("claudeJobProgress");
    }
    static QLabel* percentOf(ClaudeJobsWindow* w, int row)
    {
        return w->table()->cellWidget(row, ClaudeJobsWindow::Progress)->findChild<QLabel*>("claudeJobPercent");
    }
    static QToolButton* stopOf(ClaudeJobsWindow* w, int row)
    {
        return w->table()->cellWidget(row, ClaudeJobsWindow::Stop)->findChild<QToolButton*>("claudeJobStop");
    }

    // A running job of ours, with \a percent, and what stopping it does.
    QString aJob(const QString& name, int percent = -1, Registry::Stop stop = {})
    {
        Job job;
        job.source = "qucs-s";
        job.kind = "solve";
        job.name = name;
        job.status = Status::Running;
        job.percent = percent;
        job.whyNoPercent = "It says none.";
        job.canStop = bool(stop);
        job.whyNoStop = stop ? QString() : QStringLiteral("It has no way to stop.");
        return Registry::instance().add(job, stop);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        qputenv("QUCS_FAKE_DIR", dir.path().toUtf8());
        qputenv("QUCS_CLAUDE", "/nonexistent/claude");   // never the real one
    }

    void init()
    {
        Registry::instance().reset();
        QFile::remove(dir.filePath("jobs-in"));
    }

    void cleanup()
    {
        delete ClaudeJobsWindow::existing();
        Registry::instance().reset();
    }

    // Each status from the program's own events, and no other way.
    void aTasksStatusIsWhatTheProgramSays()
    {
        Session s;
        QJsonObject begin = started("k1", "Build it all");
        begin.insert("tool_use_id", "t1");
        // (Its command is what its tool use said.)
        s.handleLine(line({{"type", "assistant"},
                           {"message", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "tool_use"}, {"id", "t1"}, {"name", "Bash"},
                                                                                      {"input", QJsonObject{{"command", "make all -j8"}}}}}}}}}));
        s.handleLine(line(begin));
        const QString id = onlyJob();
        QVERIFY(!id.isEmpty());
        Job job = *Registry::instance().job(id);
        QCOMPARE(job.status, Status::Started);
        QCOMPARE(job.name, QStringLiteral("Build it all"));
        QCOMPARE(job.kind, QStringLiteral("command"));
        QCOMPARE(job.source, QStringLiteral("claude-code"));
        QCOMPARE(job.detail, QStringLiteral("make all -j8"));
        QCOMPARE(job.taskId, QStringLiteral("k1"));
        QVERIFY(job.background);
        QVERIFY(job.began.isValid());
        QVERIFY(!job.ended.isValid());

        // Told again: the same job.
        s.handleLine(line(begin));
        QCOMPARE(Registry::instance().count(), 1);

        // Waiting, then going, then what it says of itself.
        s.handleLine(line(patched("k1", "pending")));
        QCOMPARE(Registry::instance().job(id)->status, Status::Queued);
        QVERIFY(Registry::instance().job(id)->queued.isValid());
        s.handleLine(line(patched("k1", "running")));
        QCOMPARE(Registry::instance().job(id)->status, Status::Running);
        s.handleLine(line({{"type", "system"}, {"subtype", "task_progress"}, {"task_id", "k1"}, {"description", "Running the linker"},
                           {"usage", QJsonObject{{"total_tokens", 2000}, {"tool_uses", 3}, {"duration_ms", 900}}}, {"last_tool_name", "Bash"}}));
        job = *Registry::instance().job(id);
        QVERIFY2(job.said.contains("Running the linker") && job.said.contains("3 tool use") && job.said.contains("Bash"), qPrintable(job.said));
        QCOMPARE(job.percent, -1);   // (How much is said; how far is not.)

        // Its end: and nothing after it changes that.
        s.handleLine(line(ended("k1", "completed", "It built.")));
        job = *Registry::instance().job(id);
        QCOMPARE(job.status, Status::Finished);
        QCOMPARE(job.percent, 100);
        QCOMPARE(job.outcome, QStringLiteral("It built."));
        QCOMPARE(job.outputFile, QStringLiteral("/tmp/k1.output"));
        QVERIFY(job.ended.isValid());
        const QDateTime end = job.ended;
        s.handleLine(line(patched("k1", "failed")));
        s.handleLine(line(patched("k1", "running")));
        job = *Registry::instance().job(id);
        QCOMPARE(job.status, Status::Finished);
        QCOMPARE(job.ended, end);

        // Each way to end; an agent resumed is a run of its own, the first
        // as it ended.
        s.handleLine(line(started("k2", "Fail it", "local_agent")));
        s.handleLine(line(ended("k2", "failed", "exit 3")));
        const QString second = Registry::instance().taskJob(s.caller(), "k2");
        QCOMPARE(Registry::instance().job(second)->status, Status::Failed);
        QCOMPARE(Registry::instance().job(second)->kind, QStringLiteral("agent"));
        s.handleLine(line(started("k3", "Kill it")));
        s.handleLine(line(patched("k3", "killed")));
        QCOMPARE(Registry::instance().job(Registry::instance().taskJob(s.caller(), "k3"))->status, Status::Stopped);
        s.handleLine(line(started("k2", "Fail it again", "local_agent")));
        QVERIFY(Registry::instance().taskJob(s.caller(), "k2") != second);
        QCOMPARE(Registry::instance().job(second)->status, Status::Failed);

        // What the program says is no activity is left out; an end of what
        // it never named is shown, from its notification.
        QJsonObject watcher = started("w1", "Watching");
        watcher.insert("ambient", true);
        s.handleLine(line(watcher));
        QVERIFY(Registry::instance().taskJob(s.caller(), "w1").isEmpty());
        QJsonObject resumed = ended("old", "stopped", "From before");
        resumed.insert("reason", "worker_restart");
        s.handleLine(line(resumed));
        const auto old = Registry::instance().job(Registry::instance().taskJob(s.caller(), "old"));
        QVERIFY(old.has_value());
        QCOMPARE(old->status, Status::Stopped);
    }

    // Started moves on to running only because it has not ended.
    void aTaskBegunAndNotEndedRuns()
    {
        Session s;
        s.handleLine(line(started("k1", "Build")));
        const QString id = onlyJob();
        QCOMPARE(Registry::instance().job(id)->status, Status::Started);
        QTRY_COMPARE_WITH_TIMEOUT(Registry::instance().job(id)->status, Status::Running, 10000);
    }

    // The window shows a percentage only where there is one.
    void aRealPercentageAndAnUnknownOne()
    {
        const QString known = aJob("Counts", 42);
        const QString unknown = aJob("Cannot count");
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        const int k = w->rowOf(known), u = w->rowOf(unknown);
        QVERIFY(k >= 0 && u >= 0);
        QCOMPARE(barOf(w, k)->maximum(), 100);
        QCOMPARE(barOf(w, k)->value(), 42);
        QCOMPARE(percentOf(w, k)->text(), QStringLiteral("42 %"));
        // Busy, and says so.
        QCOMPARE(barOf(w, u)->minimum(), 0);
        QCOMPARE(barOf(w, u)->maximum(), 0);
        QCOMPARE(percentOf(w, u)->text(), QStringLiteral("unknown"));
        QCOMPARE(percentOf(w, u)->toolTip(), QStringLiteral("It says none."));

        // Live.
        Registry::instance().update(known, [](Job& job) { job.percent = 57; });
        QCOMPARE(barOf(w, k)->value(), 57);
        QCOMPARE(percentOf(w, k)->text(), QStringLiteral("57 %"));
        // Past the ends: held within them.
        Registry::instance().update(known, [](Job& job) { job.percent = 900; });
        QCOMPARE(Registry::instance().job(known)->percent, 100);
        Registry::instance().update(known, [](Job& job) { job.percent = -12; });
        QCOMPARE(Registry::instance().job(known)->percent, -1);

        // Ended without having said: neither a full bar nor an empty one.
        Registry::instance().update(unknown, [](Job& job) { job.status = Status::Stopped; });
        QVERIFY(!barOf(w, u)->isVisibleTo(w));
        QCOMPARE(percentOf(w, u)->text(), QStringLiteral("-"));
        // Stopped part way: as far as it got.
        const QString part = aJob("Part way", 30);
        Registry::instance().update(part, [](Job& job) { job.status = Status::Stopped; });
        QCOMPARE(percentOf(w, w->rowOf(part))->text(), QStringLiteral("30 %"));
        QCOMPARE(barOf(w, w->rowOf(part))->value(), 30);

        // Finished, percentage or not: all of it.
        const QString whole = aJob("Whole");
        Registry::instance().update(whole, [](Job& job) {
            job.status = Status::Finished;
            job.percent = 100;
        });
        QCOMPARE(percentOf(w, w->rowOf(whole))->text(), QStringLiteral("100 %"));

        // The tools say the same.
        const QJsonObject u2 = toJson(*Registry::instance().job(unknown), 0, false);
        QVERIFY(u2.value("percent").isNull());
        QCOMPARE(u2.value("status").toString(), QStringLiteral("stopped"));
        QCOMPARE(toJson(*Registry::instance().job(part), 0, false).value("percent").toInt(), 30);
    }

    // Stop asks the program, and says stopped once the program says so.
    void stopAsksAndWaitsForTheEnd()
    {
#ifdef Q_OS_WIN
        QSKIP("a shell script stands in for the program");
#endif
        Session s;
        s.setProgram(fake());
        s.setWorkingDirectory(dir.path());
        QSignalSpy turns(&s, &Session::turnFinished);
        QVERIFY(s.send("Build"));
        QVERIFY(turns.wait(10000));
        const QString id = onlyJob();
        QVERIFY(!id.isEmpty());
        QVERIFY(Registry::instance().job(id)->canStop);
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        QToolButton* stop = stopOf(w, w->rowOf(id));
        QVERIFY(stop->isEnabled());
        QVERIFY(stop->isVisibleTo(w));
        QTest::mouseClick(stop, Qt::LeftButton);
        // Asked, by the task's own id...
        QCOMPARE(Registry::instance().job(id)->stopAsked, true);
        QTRY_VERIFY_WITH_TIMEOUT(read("jobs-in").contains("stop_task"), 10000);
        bool asked = false;
        for (const QString& each : read("jobs-in").split('\n', Qt::SkipEmptyParts)) {
            const QJsonObject request = QJsonDocument::fromJson(each.toUtf8()).object().value("request").toObject();
            if (request.value("subtype").toString() == "stop_task") {
                QCOMPARE(request.value("task_id").toString(), QStringLiteral("k1"));
                asked = true;
            }
        }
        QVERIFY(asked);
        // ... and stopped on the program's word.
        QTRY_COMPARE_WITH_TIMEOUT(Registry::instance().job(id)->status, Status::Stopped, 10000);
        QCOMPARE(w->table()->item(w->rowOf(id), ClaudeJobsWindow::Status)->text(), QStringLiteral("stopped"));
        QVERIFY(!stopOf(w, w->rowOf(id))->isVisibleTo(w));
        QVERIFY(Registry::instance().job(id)->ended.isValid());
    }

    // Asked, and nothing says it ended: never called stopped.
    void aStopWithNoEndIsNotAStop()
    {
#ifdef Q_OS_WIN
        QSKIP("a shell script stands in for the program");
#endif
        Registry::instance().setPatience(300);
        Session s;
        s.setProgram(fake());
        s.setWorkingDirectory(dir.path());
        QSignalSpy turns(&s, &Session::turnFinished);
        QVERIFY(s.send("Stubborn"));
        QVERIFY(turns.wait(10000));
        const QString id = onlyJob();
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        QVERIFY(Registry::instance().stop(id));
        QCOMPARE(Registry::instance().job(id)->status, Status::Stopping);
        QToolButton* stop = stopOf(w, w->rowOf(id));
        QVERIFY(!stop->isEnabled());
        QCOMPARE(stop->text(), QStringLiteral("Stopping"));
        // The program's "success" says only that it was asked (it says that of
        // a task over already). Past the patience: as it was, Stop on again.
        QTRY_VERIFY_WITH_TIMEOUT(Registry::instance().job(id)->status != Status::Stopping, 10000);
        const Job job = *Registry::instance().job(id);
        QVERIFY(!isOver(job.status));
        QVERIFY2(job.said.contains("has not stopped"), qPrintable(job.said));
        QVERIFY(!job.stopAsked);
        QVERIFY(stopOf(w, w->rowOf(id))->isEnabled());
    }

    void aStopRefusedIsSaid()
    {
#ifdef Q_OS_WIN
        QSKIP("a shell script stands in for the program");
#endif
        Session s;
        s.setProgram(fake());
        s.setWorkingDirectory(dir.path());
        QSignalSpy turns(&s, &Session::turnFinished);
        QVERIFY(s.send("Refuse"));
        QVERIFY(turns.wait(10000));
        const QString id = onlyJob();
        QVERIFY(Registry::instance().stop(id));
        QTRY_VERIFY_WITH_TIMEOUT(Registry::instance().job(id)->status != Status::Stopping, 10000);
        const Job job = *Registry::instance().job(id);
        QVERIFY(!isOver(job.status));
        QVERIFY2(job.said.contains("No such task"), qPrintable(job.said));
        // (Refused, it restarts nothing: the session's own refusals do.)
        QVERIFY(s.isRunning());
    }

    // No program to ask: refused at once, nothing changed.
    void aStopThatCannotBeAskedGoesNowhere()
    {
        Session s;
        s.handleLine(line(started("k1", "Build")));   // (with no program running)
        const QString id = onlyJob();
        QString why;
        QVERIFY(!Registry::instance().stop(id, &why));
        QVERIFY(!why.isEmpty());
        QVERIFY(!isOver(Registry::instance().job(id)->status));

        // A job with no way to stop: its button off, and why.
        const QString stuck = aJob("Stuck");
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        QToolButton* stop = stopOf(w, w->rowOf(stuck));
        QVERIFY(!stop->isEnabled());
        QCOMPARE(stop->toolTip(), QStringLiteral("It has no way to stop."));
        QVERIFY(!Registry::instance().stop(stuck, &why));
        QCOMPARE(why, QStringLiteral("It has no way to stop."));
        QVERIFY(toJson(*Registry::instance().job(stuck), 0, false).value("why_no_stop").toString().contains("no way"));
    }

    // Stops only the job asked for.
    void stopStopsThatJobAlone()
    {
        int first = 0, second = 0;
        const QString a = aJob("First", -1, [&first] {
            ++first;
            return true;
        });
        const QString b = aJob("Second", -1, [&second] {
            ++second;
            return true;
        });
        QVERIFY(Registry::instance().stop(b));
        QCOMPARE(first, 0);
        QCOMPARE(second, 1);
        QCOMPARE(Registry::instance().job(a)->status, Status::Running);
        QCOMPARE(Registry::instance().job(b)->status, Status::Stopping);
        // Asked again: asked once.
        QVERIFY(Registry::instance().stop(b));
        QCOMPARE(second, 1);
    }

    // The program ending says nothing of how its tasks ended.
    void theProgramEndingLosesItsTasks()
    {
#ifdef Q_OS_WIN
        QSKIP("a shell script stands in for the program");
#endif
        {
            Session s;
            s.setProgram(fake());
            s.setWorkingDirectory(dir.path());
            QSignalSpy turns(&s, &Session::turnFinished);
            QVERIFY(s.send("Build"));
            QVERIFY(turns.wait(10000));
            const QString id = onlyJob();
            s.stop();
            const Job job = *Registry::instance().job(id);
            QCOMPARE(job.status, Status::Lost);
            QVERIFY(!job.canStop);
            QVERIFY2(job.said.contains("without saying"), qPrintable(job.said));
            QVERIFY(job.ended.isValid());
        }
        // A session gone (its tab closed) too.
        Registry::instance().reset();
        auto s = std::make_unique<Session>();
        s->handleLine(line(started("k1", "Build")));
        const QString id = onlyJob();
        s.reset();
        QCOMPARE(Registry::instance().job(id)->status, Status::Lost);
    }

    // Only those over.
    void clearFinishedLeavesWhatRuns()
    {
        const QString going = aJob("Going", 10);
        const QString queued = [] {
            Job job;
            job.name = "Waiting";
            job.status = Status::Queued;
            return Registry::instance().add(job);
        }();
        const QString stopping = aJob("Stopping", -1, [] { return true; });
        QVERIFY(Registry::instance().stop(stopping));
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        QVERIFY(!w->clearButton()->isEnabled());   // (nothing over)
        QVERIFY2(w->summary()->text().contains("2 going") && w->summary()->text().contains("1 queued"), qPrintable(w->summary()->text()));
        for (const Status status : {Status::Finished, Status::Failed, Status::Stopped, Status::Lost}) {
            const QString over = aJob("Over");
            Registry::instance().update(over, [status](Job& job) { job.status = status; });
        }
        QVERIFY(w->clearButton()->isEnabled());
        QCOMPARE(w->table()->rowCount(), 7);
        QTest::mouseClick(w->clearButton(), Qt::LeftButton);
        QCOMPARE(w->table()->rowCount(), 3);
        QCOMPARE(Registry::instance().count(), 3);
        QVERIFY(w->rowOf(going) >= 0);
        QVERIFY(w->rowOf(queued) >= 0);
        QVERIFY(w->rowOf(stopping) >= 0);
        QVERIFY(!w->clearButton()->isEnabled());
        QCOMPARE(Registry::instance().clearFinished(), 0);
    }

    // The window: one, opened from the dock's menu, not modal, and a long name
    // does not widen it.
    void theMenuOpensOneWindow()
    {
        ClaudeCodePanel panel;
        panel.show();
        auto* menuButton = panel.findChild<QToolButton*>("claudeMenu");
        QMenu* menu = menuButton != nullptr ? menuButton->menu() : nullptr;
        if (menu == nullptr)
            for (QToolButton* b : panel.findChildren<QToolButton*>())
                if (b->menu() != nullptr && b->menu()->findChild<QAction*>("claudeShowBackgroundJobs") != nullptr) menu = b->menu();
        QVERIFY(menu != nullptr);
        auto* action = menu->findChild<QAction*>("claudeShowBackgroundJobs");
        QVERIFY(action != nullptr);
        QCOMPARE(action->text(), QStringLiteral("Show Background Jobs"));
        QVERIFY(ClaudeJobsWindow::existing() == nullptr);
        action->trigger();
        ClaudeJobsWindow* w = ClaudeJobsWindow::existing();
        QVERIFY(w != nullptr);
        QVERIFY(w->isVisible());
        QVERIFY(w->isWindow());
        QVERIFY(!w->isModal());
        QCOMPARE(w->windowTitle(), QStringLiteral("Background Jobs"));
        QVERIFY(qobject_cast<QDialog*>(w) == nullptr);   // (no dialog that waits: QucsControl::openDialog())
        // Again: the same, brought back.
        w->hide();
        action->trigger();
        QCOMPARE(ClaudeJobsWindow::existing(), w);
        QVERIFY(w->isVisible());
        // Closed, it comes back the same way.
        w->close();
        QVERIFY(!w->isVisible());
        action->trigger();
        QCOMPARE(ClaudeJobsWindow::existing(), w);
        QVERIFY(w->isVisible());
        delete w;
    }

    void aLongNameDoesNotWidenTheWindow()
    {
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        const int before = w->minimumSizeHint().width();
        const int widthBefore = w->width();
        const QString name(290, QLatin1Char('x'));
        const QString id = aJob(name, 5);
        Registry::instance().update(id, [](Job& job) {
            job.conversation = QString(290, QLatin1Char('c'));
            job.said = QString(400, QLatin1Char('s'));
        });
        const QString words = QString("word ").repeated(1000);
        const QString later = aJob(words.left(300));
        Registry::instance().update(later, [&words](Job& job) { job.detail = words; });
        QApplication::processEvents();
        QCOMPARE(w->minimumSizeHint().width(), before);
        QCOMPARE(w->width(), widthBefore);
        QVERIFY(w->minimumWidth() < 500);
        // (Not only no wider than it was: a window of a few hundred pixels fits
        // a small screen.)
        QVERIFY2(before < 800, qPrintable(QString::number(before)));
        // What the window says below the list, whatever its length (why a
        // stop was refused), takes more lines, not more width.
        w->summary()->setText(words);
        QApplication::processEvents();
        QVERIFY2(w->minimumSizeHint().width() <= before, qPrintable(QString::number(w->minimumSizeHint().width())));
        QCOMPARE(w->width(), widthBefore);
        const QTableWidgetItem* item = w->table()->item(w->rowOf(id), ClaudeJobsWindow::Name);
        QCOMPARE(item->text(), name);   // (whole: the view cuts what it shows)
        QVERIFY(item->toolTip().startsWith(name));
        QVERIFY(!w->table()->wordWrap());

        // Its details, whole.
        w->table()->selectRow(w->rowOf(later));
        QVERIFY(w->details()->toPlainText().contains(words));
        // ...following the job.
        Registry::instance().update(later, [](Job& job) { job.outcome = "It said this."; });
        QVERIFY(w->details()->toPlainText().contains("It said this."));

        // Cut, a name without end from a damaged message.
        Session s;
        s.handleLine(line(started("long", QString(5000, QLatin1Char('n')))));
        const auto cut = Registry::instance().job(Registry::instance().taskJob(s.caller(), "long"));
        QVERIFY(cut->name.size() <= 300);
    }

    // Every conversation's jobs in one list, each named.
    void theTabsShareOneList()
    {
#ifdef Q_OS_WIN
        QSKIP("a shell script stands in for the program");
#endif
        ClaudeCodeTabs tabs;
        tabs.show();
        ClaudeCodePanel* first = tabs.current();
        QVERIFY(first != nullptr);
        ClaudeCodePanel* second = tabs.newConversation();
        QVERIFY(second != first);
        for (ClaudeCodePanel* p : {first, second}) {
            p->session()->setProgram(fake());
            p->session()->setWorkingDirectory(dir.path());
        }
        QSignalSpy turnsA(first->session(), &Session::turnFinished);
        QSignalSpy turnsB(second->session(), &Session::turnFinished);
        QVERIFY(first->session()->send("Build one"));
        QVERIFY(turnsA.wait(10000));
        QVERIFY(second->session()->send("Build two"));
        QVERIFY(turnsB.wait(10000));
        QCOMPARE(Registry::instance().count(), 2);   // (the same task id in each: two jobs)
        const QString a = Registry::instance().taskJob(first->session()->caller(), "k1");
        const QString b = Registry::instance().taskJob(second->session()->caller(), "k1");
        QVERIFY(!a.isEmpty() && !b.isEmpty() && a != b);

        // Named, as each tab is, and again when it is.
        Registry::instance().nameConversation(first->session()->caller(), "The first");
        Registry::instance().nameConversation(second->session()->caller(), "The second");
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(&tabs);
        QCOMPARE(w->table()->item(w->rowOf(a), ClaudeJobsWindow::Conversation)->text(), QStringLiteral("The first"));
        QCOMPARE(w->table()->item(w->rowOf(b), ClaudeJobsWindow::Conversation)->text(), QStringLiteral("The second"));
        QJsonObject o = toJson(*Registry::instance().job(b), second->session()->caller(), false);
        QVERIFY(o.value("yours").toBool());
        QVERIFY(!toJson(*Registry::instance().job(a), second->session()->caller(), false).contains("yours"));

        // Stopping one: the other runs on.
        QVERIFY(Registry::instance().stop(b));
        QTRY_COMPARE_WITH_TIMEOUT(Registry::instance().job(b)->status, Status::Stopped, 10000);
        QVERIFY(!isOver(Registry::instance().job(a)->status));

        // Its tab closed: its row stays, and says it is lost.
        QVERIFY(tabs.closeConversation(first, false));
        QTRY_COMPARE_WITH_TIMEOUT(Registry::instance().job(a)->status, Status::Lost, 10000);
        QVERIFY(w->rowOf(a) >= 0);
        QCOMPARE(w->table()->item(w->rowOf(a), ClaudeJobsWindow::Conversation)->text(), QStringLiteral("The first"));

        // One window, whichever tab asks.
        QCOMPARE(ClaudeJobsWindow::showWindow(second), w);
    }

    // Those that ended come back; what ran comes back lost.
    void whatEndedIsKept()
    {
        const QString file = dir.filePath("kept.json");
        QFile::remove(file);
        Registry::instance().setKept(file);
        const QString over = aJob("Solved", 100);
        Registry::instance().update(over, [](Job& job) {
            job.status = Status::Finished;
            job.outcome = "All solved.";
            job.detail = "the arguments";
        });
        const QString going = aJob("Still going", 12);
        Registry::instance().saveNow();
        Registry::instance().reset();
        QCOMPARE(Registry::instance().count(), 0);
        Registry::instance().setKept(file);
        QCOMPARE(Registry::instance().count(), 2);
        const QList<Job> jobs = Registry::instance().jobs();
        const Job newest = jobs.at(0), oldest = jobs.at(1);
        QCOMPARE(newest.name, QStringLiteral("Still going"));
        QCOMPARE(newest.status, Status::Lost);
        QCOMPARE(newest.percent, 12);
        QVERIFY(!newest.canStop);
        QVERIFY(newest.ended.isValid());
        QCOMPARE(oldest.name, QStringLiteral("Solved"));
        QCOMPARE(oldest.status, Status::Finished);
        QCOMPARE(oldest.outcome, QStringLiteral("All solved."));
        QCOMPARE(oldest.detail, QStringLiteral("the arguments"));
        QVERIFY(oldest.ended.isValid());
        // Cleared, it stays cleared.
        QCOMPARE(Registry::instance().clearFinished(), 2);
        Registry::instance().saveNow();
        Registry::instance().reset();
        Registry::instance().setKept(file);
        QCOMPARE(Registry::instance().count(), 0);
    }

    // What the file may hold that is no list of ours.
    void aDamagedFileIsNoList()
    {
        const QString file = dir.filePath("damaged.json");
        const auto holds = [&](const QByteArray& bytes) {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
            f.close();
            Registry::instance().reset();
            Registry::instance().setKept(file);
        };
        for (const QByteArray& bad : {QByteArray("not json"), QByteArray("[1,2]"), QByteArray("{\"jobs\":7}"), QByteArray("{\"jobs\":[1,\"a\",null,{}]}"),
                                      QByteArray("{\"jobs\":[{\"status\":\"finished\"}]}")}) {
            holds(bad);
            QVERIFY(!QTest::currentTestFailed());
            QCOMPARE(Registry::instance().count(), 0);
        }
        // What a field holds that it should not: kept within bounds.
        holds(R"({"jobs":[{"name":"N","status":"no such","percent":999,"started":"never","ended":12}]})");
        QCOMPARE(Registry::instance().count(), 1);
        const Job job = Registry::instance().jobs().first();
        QCOMPARE(job.status, Status::Lost);
        QCOMPARE(job.percent, -1);
        QVERIFY(job.ended.isValid());
#ifndef Q_OS_WIN
        // A pipe where the file is: opening it would wait for ever (4a7186b).
        QFile::remove(file);
        QCOMPARE(mkfifo(QFile::encodeName(file).constData(), 0600), 0);
        Registry::instance().reset();
        Registry::instance().setKept(file);
        QCOMPARE(Registry::instance().count(), 0);
        Registry::instance().reset();
        QFile::remove(file);
#endif
    }

    // More than the list holds: the oldest over go first, none running before.
    void theListHasAnEnd()
    {
        const QString going = aJob("Oldest, going");
        for (int i = 0; i < Registry::kMost + 20; ++i) {
            const QString id = aJob(QStringLiteral("Over %1").arg(i));
            Registry::instance().update(id, [](Job& job) { job.status = Status::Finished; });
        }
        QCOMPARE(Registry::instance().count(), Registry::kMost);
        QVERIFY(Registry::instance().job(going).has_value());
        QVERIFY(Registry::instance().jobs().first().name.endsWith(QStringLiteral(" %1").arg(Registry::kMost + 19)));

        // A window open through it stays with the list.
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        QCOMPARE(w->table()->rowCount(), Registry::kMost);
        aJob("One more");
        QCOMPARE(w->table()->rowCount(), Registry::kMost);
    }

    // Opt in (QUCS_JOBS_PICTURES=<folder>): the window in each theme, every
    // status in it, as pictures to look at - on a real display when run with
    // the platform's own plugin, not the offscreen one.
    void pictures()
    {
        const QString folder = qEnvironmentVariable("QUCS_JOBS_PICTURES");
        if (folder.isEmpty()) QSKIP("QUCS_JOBS_PICTURES names a folder for the pictures");
        const QString known = aJob("fem_solve: heater_transient.qfem", 42);
        Registry::instance().update(known, [](Job& job) {
            job.said = "42 % - time step 840 of 2000";
            job.conversation = "The heater's transient";
        });
        aJob("rf25_solve: an_rf_stack_with_a_very_long_name_that_goes_on_and_on_and_on_and_on_and_on_and_on_and_on_and_on_and_on_and_on_and_on_and_on.qrf25", 7);
        aJob("Build it all and run every test there is");
        const QString stuck = aJob("tune: amp.sch");
        Registry::instance().update(stuck, [](Job& job) { job.kind = "tune"; });
        Job queued;
        queued.kind = "simulation";
        queued.name = "simulate: amp.sch";
        queued.status = Status::Queued;
        queued.conversation = "The amplifier";
        Registry::instance().add(queued);
        const QString stopping = aJob("Agent: read the whole of the manual", -1, [] { return true; });
        Registry::instance().update(stopping, [](Job& job) { job.kind = "agent"; });
        Registry::instance().stop(stopping);
        for (const auto& [name, status, percent] : {std::tuple{"simulate: filter.sch", Status::Finished, 100}, std::tuple{"Make the docs", Status::Failed, -1},
                                                    std::tuple{"fem_solve: bent.qfem", Status::Stopped, 63}, std::tuple{"sleep 900", Status::Lost, -1}}) {
            const QString id = aJob(name, percent);
            Registry::instance().update(id, [status = status](Job& job) {
                job.status = status;
                job.kind = "command";
            });
        }
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        w->table()->selectRow(w->rowOf(known));
        QVERIFY(QTest::qWaitForWindowExposed(w));
        for (const Qt::ColorScheme scheme : {Qt::ColorScheme::Light, Qt::ColorScheme::Dark}) {
            QGuiApplication::styleHints()->setColorScheme(scheme);
            QTest::qWait(600);
            const QString file = QDir(folder).filePath(scheme == Qt::ColorScheme::Dark ? "jobs-dark.png" : "jobs-light.png");
            QVERIFY(w->grab().save(file));
            qInfo("%s (%s)", qPrintable(file), qPrintable(QGuiApplication::platformName()));
        }
        QGuiApplication::styleHints()->unsetColorScheme();
    }

    // Opt in (QUCS_JOBS_REAL=1): the real claude
    // program, Haiku, a command in the background stopped from the window. It
    // uses tokens, a few; the write-up has what it said.
    void againstClaudeCodeItself()
    {
        if (qEnvironmentVariableIsEmpty("QUCS_JOBS_REAL")) QSKIP("QUCS_JOBS_REAL=1 runs the real program");
        qunsetenv("CLAUDE_CONFIG_DIR");   // (the user's sign-in: useIsolatedSettings took it away)
        Session s;
        const QString program = QStandardPaths::findExecutable("claude");
        QVERIFY(!program.isEmpty());
        s.setProgram(program);
        s.setModel("haiku");
        s.setPermissionMode("acceptEdits");
        QTemporaryDir work;
        s.setWorkingDirectory(work.path());
        connect(&s, &Session::notice, &s, [](const QString& text) { qInfo("notice: %s", qPrintable(text)); });
        connect(&s, &Session::stateChanged, &s, [](qucs_s::claude::State state, const QString& detail) {
            qInfo("state: %s %s", qPrintable(qucs_s::claude::stateText(state)), qPrintable(detail));
        });
        connect(&s, &Session::failed, &s, [](const QString& text) { qInfo("failed: %s", qPrintable(text)); });
        connect(&s, &Session::toolStarted, &s, [](const QString&, const QString& tool, const QString& subject, const QString&) {
            qInfo("tool: %s %s", qPrintable(tool), qPrintable(subject));
        });
        connect(&s, &Session::permissionRequested, &s, [&s](const qucs_s::claude::PermissionRequest& r) { s.answer(r.id, true); });
        QSignalSpy turns(&s, &Session::turnFinished);
        const QString seconds = QString::number(600 + QRandomGenerator::global()->bounded(300));
        const QString command = QStringLiteral("perl -e 'sleep %1'").arg(seconds);
        QVERIFY(s.send(QStringLiteral("Use the Bash tool with run_in_background set to true to run exactly: %1\nThen end your turn with the word done.").arg(command)));
        QTRY_VERIFY_WITH_TIMEOUT(Registry::instance().count() >= 1, 60000);
        const Job job = Registry::instance().jobs().first();
        qInfo("job: %s", QJsonDocument(toJson(job, 0, true)).toJson(QJsonDocument::Compact).constData());
        QCOMPARE(job.kind, QStringLiteral("command"));
        QCOMPARE(job.detail, command);
        QVERIFY(job.canStop);
        QCOMPARE(job.percent, -1);
        const auto alive = [&seconds] {
            QProcess p;
            p.start("pgrep", {"-f", "sleep " + seconds});
            p.waitForFinished(5000);
            return !p.readAllStandardOutput().trimmed().isEmpty();
        };
        QTRY_VERIFY_WITH_TIMEOUT(alive(), 20000);
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        QTest::mouseClick(stopOf(w, w->rowOf(job.id)), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(Registry::instance().job(job.id)->status, Status::Stopped, 30000);
        qInfo("after Stop: %s", QJsonDocument(toJson(*Registry::instance().job(job.id), 0, true)).toJson(QJsonDocument::Compact).constData());
        QTRY_VERIFY_WITH_TIMEOUT(!alive(), 10000);
        s.stop();
    }

    // Whatever a stream holds of tasks: within bounds, and no crash. (The
    // panel's fuzz has them in its stream too.)
    void damagedEvents()
    {
        Session s;
        const QStringList subtypes{"task_started", "task_updated", "task_progress", "task_notification", "task_", "task_nonsense"};
        const QList<QJsonValue> values{QJsonValue(), QJsonValue(true), QJsonValue(-1), QJsonValue(1e308), QJsonValue("k1"), QJsonValue(""),
                                       QJsonValue(QString(10000, QLatin1Char('y'))), QJsonArray{1, "x"}, QJsonObject{{"status", 7}},
                                       QJsonValue("completed"), QJsonValue("killed"), QJsonValue("running"), QJsonValue("pending"),
                                       QJsonObject{{"status", "failed"}, {"description", 3}, {"error", "e"}, {"is_backgrounded", "x"}}};
        const QStringList keys{"task_id", "description", "status", "patch", "usage", "summary", "output_file", "prompt", "tool_use_id",
                               "task_type", "is_backgrounded", "ambient", "reason", "last_tool_name", "parent_task_id"};
        QRandomGenerator random(20261107);
        ClaudeJobsWindow* w = ClaudeJobsWindow::showWindow(nullptr);
        for (int i = 0; i < 4000; ++i) {
            QJsonObject m{{"type", "system"}, {"subtype", subtypes.at(random.bounded(int(subtypes.size())))}};
            if (random.bounded(4) != 0) m.insert("task_id", QStringLiteral("k%1").arg(random.bounded(8)));
            const int fields = random.bounded(6);
            for (int f = 0; f < fields; ++f)
                m.insert(keys.at(random.bounded(int(keys.size()))), values.at(random.bounded(int(values.size()))));
            if (random.bounded(5) == 0) m.insert("patch", QJsonObject{{"status", values.at(9 + random.bounded(4))}});
            s.handleLine(line(m));
            if (random.bounded(50) == 0) Registry::instance().clearFinished();
            if (random.bounded(50) == 0 && Registry::instance().count() > 0)
                Registry::instance().stop(Registry::instance().jobs().first().id);
            if (i % 200 == 0) QApplication::processEvents();
        }
        QVERIFY(Registry::instance().count() <= Registry::kMost);
        QCOMPARE(w->table()->rowCount(), Registry::instance().count());
        for (const Job& job : Registry::instance().jobs()) {
            QVERIFY(job.percent >= -1 && job.percent <= 100);
            QVERIFY(job.name.size() <= 300);
            QVERIFY(job.said.size() <= 1000);
            QVERIFY(!job.ended.isValid() || isOver(job.status));
            QVERIFY(w->rowOf(job.id) >= 0);
        }
    }
};

// A program with one background command: "stop_task" ends it, as Claude Code
// 2.1.311 does (the write-up's probe) - unless the prompt said Stubborn, when
// it answers success and goes on, or Refuse, when it refuses.
const char* const kFake = R"SH(#!/bin/sh
stubborn=
refuse=
while IFS= read -r line; do
  printf '%s\n' "$line" >> "$QUCS_FAKE_DIR/jobs-in"
  id=$(printf '%s' "$line" | sed -n 's,.*"request_id":"\([^"]*\)".*,\1,p')
  case "$line" in
    *'"type":"user"'*)
      case "$line" in *Stubborn*) stubborn=1 ;; esac
      case "$line" in *Refuse*) refuse=1 ;; esac
      echo '{"type":"system","subtype":"init","session_id":"jobs-1","model":"claude-test-1"}'
      echo '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make all -j8","run_in_background":true}}]}}'
      echo '{"type":"system","subtype":"task_started","task_id":"k1","tool_use_id":"t1","description":"Build it all","is_backgrounded":true,"task_type":"local_bash"}'
      echo '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"Running in the background","is_error":false}]}}'
      echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":5,"num_turns":1,"result":"ok","total_cost_usd":0.001,"session_id":"jobs-1"}'
      ;;
    *'"subtype":"stop_task"'*)
      if [ -n "$refuse" ]; then
        echo '{"type":"control_response","response":{"subtype":"error","request_id":"'"$id"'","error":"No such task"}}'
      elif [ -n "$stubborn" ]; then
        echo '{"type":"control_response","response":{"subtype":"success","request_id":"'"$id"'","response":{}}}'
      else
        echo '{"type":"system","subtype":"task_updated","task_id":"k1","patch":{"status":"killed","end_time":1794064106660}}'
        echo '{"type":"system","subtype":"task_notification","task_id":"k1","tool_use_id":"t1","status":"stopped","output_file":"/tmp/k1.output","summary":"Build it all"}'
        echo '{"type":"control_response","response":{"subtype":"success","request_id":"'"$id"'","response":{}}}'
      fi
      ;;
    *'"type":"control_request"'*)
      echo '{"type":"control_response","response":{"subtype":"success","request_id":"'"$id"'","response":{}}}'
      ;;
  esac
done
)SH";

QTEST_MAIN(TestClaudeJobs)
#include "test_claude_jobs.moc"
