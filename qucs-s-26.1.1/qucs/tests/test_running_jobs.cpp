/*
 * Running simulations (simulationruns.h, qucs_runs.cpp): a tab closed while
 * its document runs asks first, and the run goes on without its tab; it is
 * listed with the jobs, found from the toolbar, the status bar and the
 * window, stopped there, and brought back - the same document, with its run
 * and its results - from the window, Reopen Closed Tab or opening its file.
 * Each kind of document with a real run (a 2D and a 3D model, an RF 2.5D
 * model, a schematic with a simulator that takes its time); Quit with runs
 * going; the tabs closed lately; Claude's runs listed once.
 *
 * No test asserts on a time taken: each waits for a state (QTRY), for the
 * reason 3972088 and c6c7eb2 give. A run is closed the moment it has begun,
 * so that it is still running when its tab closes however fast the machine.
 */
#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include "claudejobs.h"
#include "claudejobswindow.h"
#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "multiphysics3ddoc.h"
#include "multiphysicsdoc.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "rf25/rf25_model.h"
#include "rf25doc.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "simulationruns.h"

using namespace qucs_s::claude::jobs;

class TestRunningJobs : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QString answered;   // the buttons pressed in the questions, in order

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    QString write(const QString& name, const QByteArray& text) const
    {
        QFile f(path(name));
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(text);
        return f.fileName();
    }
    qucs_s::runs::Tracker* runs() const { return app->runTracker(); }
    Registry& registry() const { return Registry::instance(); }

    // The next question (a close's, a quit's) answered with its button
    // \a button, as the user would; what it said kept.
    QString asked;
    void answerWith(const QString& button)
    {
        auto* timer = new QTimer(this);
        timer->setInterval(20);
        connect(timer, &QTimer::timeout, this, [this, timer, button] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr) return;
            auto* b = box->findChild<QPushButton*>(button);
            if (b == nullptr) return;
            asked = box->text();
            answered += button + QLatin1Char(' ');
            timer->stop();
            timer->deleteLater();
            b->click();
        });
        timer->start();
    }

    // A 2D model's transient that runs for hours unless stopped.
    QString longTransient(const QString& name)
    {
        QFile example(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/heater_transient.qfem"));
        if (!example.open(QIODevice::ReadOnly)) return {};
        QByteArray text = example.readAll();
        text.replace("range(0, 2[s], 60[s])", "range(0, 0.5[s], 100000[s])");
        return write(name, text);
    }
    MultiphysicsDoc* openModel(const QString& file)
    {
        if (!app->gotoPage(file)) return nullptr;
        return qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
    }
    // The job of the user's run of \a doc's file, the newest.
    std::optional<Job> userJobOf(const QString& file) const
    {
        for (const Job& job : registry().jobs())
            if (job.source == QLatin1String("user") && QFileInfo(job.document).fileName() == QFileInfo(file).fileName()) return job;
        return std::nullopt;
    }
    int tabIndexOf(QWidget* w) const
    {
        for (ContextMenuTabWidget* pane : app->panes())
            if (pane->indexOf(w) >= 0) return pane->indexOf(w);
        return -1;
    }
    void closeTabOf(QWidget* w)
    {
        app->showDocument(w);
        app->slotFileClose(app->DocumentTab->indexOf(w));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsSettings.ExamplesDir = QStringLiteral(QUCS_EXAMPLES_DIR "/");
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.font = QApplication::font();
        QucsSettings.appFont = QApplication::font();
        QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1300, 900);
        app->show();
        QVERIFY(runs() != nullptr);
    }

    void cleanupTestCase()
    {
        delete ClaudeJobsWindow::existing();
        runs()->setQuitting(true);
        for (QWidget* w : runs()->parked())
            if (auto* doc = dynamic_cast<QucsDoc*>(w)) doc->setDocChanged(false);
        runs()->releaseParked();
        for (QucsDoc* doc : app->allDocuments()) {
            runs()->stop(doc);
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
    }

    void init()
    {
        registry().reset();
        answered.clear();
        asked.clear();
    }

    void cleanup()
    {
        // Nothing left running, held or open for the next.
        runs()->setQuitting(true);
        for (QWidget* w : runs()->parked())
            if (auto* doc = dynamic_cast<QucsDoc*>(w)) doc->setDocChanged(false);
        runs()->releaseParked();
        for (QucsDoc* doc : app->allDocuments()) {
            runs()->stop(doc);
            if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) model->waitForJob(60000);
            if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc)) model3->waitForIdle(60000);
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
        runs()->setQuitting(false);
        runs()->clearClosed();
        delete ClaudeJobsWindow::existing();
        registry().reset();
    }

    // In the window: a toolbar of its own with Jobs and Reopen Closed Tab (and
    // its list), Simulation > Running Jobs, File > Reopen Closed Tab with
    // Ctrl+Shift+T and the list of tabs closed lately.
    void theToolbarAndMenus()
    {
        QVERIFY(app->runsToolbar != nullptr);
        QVERIFY(app->toolbars().contains(app->runsToolbar));
        QVERIFY(app->runsToolbar->actions().contains(app->showJobsAction));
        auto* reopen = app->runsToolbar->findChild<QToolButton*>("reopenClosedButton");
        QVERIFY(reopen != nullptr);
        QCOMPARE(reopen->defaultAction(), app->reopenClosedAction);
        QCOMPARE(reopen->menu(), app->recentlyClosedMenu);
        QCOMPARE(app->reopenClosedAction->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
        const auto inMenu = [](QAction* action, const QString& title) {
            for (QObject* o : action->associatedObjects())
                if (auto* menu = qobject_cast<QMenu*>(o); menu != nullptr && menu->title() == title) return true;
            return false;
        };
        QVERIFY(inMenu(app->reopenClosedAction, "&File"));
        QVERIFY(inMenu(app->recentlyClosedMenu->menuAction(), "&File"));
        QVERIFY(inMenu(app->showJobsAction, "&Simulation"));
        // Nothing closed yet: Reopen off.
        QVERIFY(!app->reopenClosedAction->isEnabled());
        // Jobs opens the one window, which has the filter and Show.
        app->showJobsAction->trigger();
        ClaudeJobsWindow* w = ClaudeJobsWindow::existing();
        QVERIFY(w != nullptr && w->isVisible());
        QVERIFY(w->showButton() != nullptr && w->filter() != nullptr);
        QCOMPARE(w->table()->horizontalHeaderItem(ClaudeJobsWindow::StartedBy)->text(), QStringLiteral("Started by"));
        app->showJobsAction->trigger();
        QCOMPARE(ClaudeJobsWindow::existing(), w);
    }

    // A 2D model solving, its tab closed: asked, kept running in the
    // background, still solving, listed and counted; opened again, the same
    // document with its run; closed again and stopped from the window.
    void aModelKeepsRunningWithItsTabClosed()
    {
        const QString file = longTransient("kept.qfem");
        MultiphysicsDoc* doc = openModel(file);
        QVERIFY(doc != nullptr);
        QPointer<MultiphysicsDoc> kept(doc);
        doc->compute();
        QVERIFY(doc->isBusy());
        // Listed as the user's run, at once (a solve is listed as it begins).
        QTRY_VERIFY_WITH_TIMEOUT(userJobOf(file).has_value(), 10000);
        Job job = *userJobOf(file);
        QCOMPARE(job.kind, QStringLiteral("solve"));
        QCOMPARE(startedBy(job), QStringLiteral("user"));
        QVERIFY(QFileInfo(job.document) == QFileInfo(file));
        QVERIFY(job.canStop);
        QCOMPARE(toJson(job, 0, false).value("started_by").toString(), QStringLiteral("user"));

        answerWith("keepRunning");
        closeTabOf(doc);
        QCOMPARE(answered, QStringLiteral("keepRunning "));
        QVERIFY2(asked.contains("kept.qfem") && asked.contains("still running"), qPrintable(asked));
        // Its tab gone, the document not: hidden, held, still solving.
        QVERIFY(kept);
        QCOMPARE(tabIndexOf(doc), -1);
        QVERIFY(app->findDoc(file) == nullptr);
        QVERIFY(runs()->isParked(doc));
        QVERIFY(!doc->isVisible());
        QVERIFY(doc->isBusy());
        QVERIFY(!isOver(registry().job(job.id)->status));
        // Found: among the tabs closed lately, in the status bar's count.
        QVERIFY(app->reopenClosedAction->isEnabled());
        QCOMPARE(runs()->closed().size(), 1);
        QCOMPARE(runs()->closed().first().parked.data(), static_cast<QWidget*>(doc));
        auto* chip = app->statusBar()->findChild<QToolButton*>("statusJobs");
        QVERIFY(chip != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(chip->text().contains("1 running"), 5000);
        QVERIFY(app->showJobsAction->iconText().contains("1 running"));
        QVERIFY2(runs()->runningNow().join('\n').contains("kept.qfem"), qPrintable(runs()->runningNow().join('\n')));
        // Its percentage goes on as it solves, without its tab.
        QTRY_VERIFY_WITH_TIMEOUT(registry().job(job.id)->percent >= 0, 60000);

        // Opened again: the same document, still solving - not a copy.
        QVERIFY(app->gotoPage(file));
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(doc));
        QVERIFY(!runs()->isParked(doc));
        QVERIFY(doc->isBusy());
        QCOMPARE(runs()->closed().size(), 0);   // (used up)
        QCOMPARE(registry().job(job.id)->status == Status::Running || registry().job(job.id)->status == Status::Started, true);

        // Closed again, and stopped from the window's row.
        answerWith("keepRunning");
        closeTabOf(doc);
        QVERIFY(runs()->isParked(doc));
        app->showRunningJobs();
        ClaudeJobsWindow* w = ClaudeJobsWindow::existing();
        const int row = w->rowOf(job.id);
        QVERIFY(row >= 0);
        QCOMPARE(w->table()->item(row, ClaudeJobsWindow::StartedBy)->text(), QStringLiteral("You"));
        auto* stop = w->table()->cellWidget(row, ClaudeJobsWindow::Stop)->findChild<QToolButton*>("claudeJobStop");
        QVERIFY(stop->isEnabled());
        QTest::mouseClick(stop, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(registry().job(job.id)->status, Status::Stopped, 60000);
        QVERIFY(!doc->isBusy());
        // Stopped, and still held - with what it got to - until shown or cleared.
        QVERIFY(kept && runs()->isParked(doc));
        QTRY_VERIFY_WITH_TIMEOUT(chip->text().contains("kept in the background"), 5000);
    }

    // The window's Show brings a closed tab back; Clear Finished lets a
    // finished one go, unless it has changes, which come back to be saved.
    void theWindowShowsAndClears()
    {
        const QString file = longTransient("shown.qfem");
        MultiphysicsDoc* doc = openModel(file);
        QVERIFY(doc != nullptr);
        QPointer<MultiphysicsDoc> kept(doc);
        doc->compute();
        answerWith("keepRunning");
        closeTabOf(doc);
        QVERIFY(runs()->isParked(doc));
        QTRY_VERIFY_WITH_TIMEOUT(userJobOf(file).has_value(), 10000);
        const QString id = userJobOf(file)->id;

        app->showRunningJobs();
        ClaudeJobsWindow* w = ClaudeJobsWindow::existing();
        w->table()->selectRow(w->rowOf(id));
        QVERIFY(w->showButton()->isEnabled());
        QVERIFY(w->details()->toPlainText().contains("Started by: You"));
        QVERIFY(w->details()->toPlainText().contains("shown.qfem"));
        // The filter: a simulation, the user's; not Claude's.
        w->setFilter(ClaudeJobsWindow::Claudes);
        QVERIFY(w->table()->isRowHidden(w->rowOf(id)));
        QVERIFY(!w->showButton()->isEnabled());
        w->setFilter(ClaudeJobsWindow::Simulations);
        QVERIFY(!w->table()->isRowHidden(w->rowOf(id)));
        w->setFilter(ClaudeJobsWindow::Yours);
        QVERIFY(!w->table()->isRowHidden(w->rowOf(id)));
        w->setFilter(ClaudeJobsWindow::AllJobs);
        w->table()->selectRow(w->rowOf(id));
        QTest::mouseClick(w->showButton(), Qt::LeftButton);
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(doc));
        QVERIFY(!runs()->isParked(doc));
        QVERIFY(doc->isBusy());

        // Stopped, closed (kept, as it has the stop's results), then cleared:
        // gone - nothing held for a row no longer listed.
        QVERIFY(registry().stop(id));
        QTRY_VERIFY_WITH_TIMEOUT(isOver(registry().job(id)->status), 60000);
        QVERIFY(doc->waitForJob(60000));
        // (Idle now: closing asks nothing, and it closes as any tab does.)
        closeTabOf(doc);
        QCOMPARE(answered, QStringLiteral("keepRunning "));
        QTRY_VERIFY_WITH_TIMEOUT(kept.isNull(), 10000);

        // One kept with changes not saved: Clear Finished brings it back.
        const QString other = longTransient("changed.qfem");
        MultiphysicsDoc* changed = openModel(other);
        QVERIFY(changed != nullptr);
        QPointer<MultiphysicsDoc> held(changed);
        changed->setDocChanged(true);
        changed->compute();
        answerWith("keepRunning");
        closeTabOf(changed);
        QVERIFY(runs()->isParked(changed));
        QVERIFY(changed->getDocChanged());   // (not asked about: kept with it)
        QTRY_VERIFY_WITH_TIMEOUT(userJobOf(other).has_value(), 10000);
        const QString otherId = userJobOf(other)->id;
        QVERIFY(registry().stop(otherId));
        QTRY_VERIFY_WITH_TIMEOUT(isOver(registry().job(otherId)->status), 60000);
        QVERIFY(changed->waitForJob(60000));
        QCOMPARE(registry().clearFinished() >= 1, true);
        QTRY_VERIFY_WITH_TIMEOUT(held && !runs()->isParked(changed) && tabIndexOf(changed) >= 0, 5000);
        changed->setDocChanged(false);
    }

    // Stop and Close stops the run and closes; Cancel leaves the tab as it was.
    void stopAndCloseOrCancel()
    {
        const QString file = longTransient("stopped.qfem");
        MultiphysicsDoc* doc = openModel(file);
        QVERIFY(doc != nullptr);
        QPointer<MultiphysicsDoc> kept(doc);
        doc->compute();
        QTRY_VERIFY_WITH_TIMEOUT(userJobOf(file).has_value(), 10000);
        const QString id = userJobOf(file)->id;

        answerWith("cancelClose");
        closeTabOf(doc);
        QCOMPARE(answered, QStringLiteral("cancelClose "));
        QVERIFY(kept && tabIndexOf(doc) >= 0 && doc->isBusy());
        QVERIFY(!runs()->isParked(doc));
        QCOMPARE(runs()->closed().size(), 0);

        answerWith("stopAndClose");
        closeTabOf(doc);
        QTRY_VERIFY_WITH_TIMEOUT(kept.isNull(), 60000);
        QTRY_VERIFY_WITH_TIMEOUT(isOver(registry().job(id)->status), 10000);
        QCOMPARE(registry().job(id)->status, Status::Stopped);
        // Among the tabs closed lately, as any tab closed: its file opens again.
        QCOMPARE(runs()->closed().size(), 1);
        QVERIFY(runs()->closed().first().parked.isNull());
        QVERIFY(app->reopenClosedTab(0));
        QVERIFY(app->findDoc(file) != nullptr);
    }

    // A tab with nothing running closes as ever, with no question; Reopen
    // Closed Tab (and Ctrl+Shift+T's action) opens it again, the newest first.
    void reopenClosedTabs()
    {
        const QString a = longTransient("first.qfem");
        const QString b = longTransient("second.qfem");
        QVERIFY(openModel(a) != nullptr);
        QVERIFY(openModel(b) != nullptr);
        closeTabOf(QucsApp::documentWidget(app->findDoc(a)));
        closeTabOf(QucsApp::documentWidget(app->findDoc(b)));
        QVERIFY(answered.isEmpty());
        QVERIFY(app->findDoc(a) == nullptr && app->findDoc(b) == nullptr);
        QCOMPARE(runs()->closed().size(), 2);
        QCOMPARE(runs()->closed().first().title, QStringLiteral("second.qfem"));
        // The list in the menu: the newest first, and Clear List.
        emit app->recentlyClosedMenu->aboutToShow();
        const QList<QAction*> items = app->recentlyClosedMenu->actions();
        QVERIFY(items.size() >= 3);
        QVERIFY(items.at(0)->text().startsWith("second.qfem"));
        QVERIFY(items.at(1)->text().startsWith("first.qfem"));
        app->reopenClosedAction->trigger();
        QVERIFY(app->findDoc(b) != nullptr);
        QVERIFY(app->findDoc(a) == nullptr);
        items.at(1)->trigger();   // (first.qfem, from the menu)
        QVERIFY(app->findDoc(a) != nullptr);
        QCOMPARE(runs()->closed().size(), 0);
        QVERIFY(!app->reopenClosedAction->isEnabled());
        // At most kClosedMost.
        for (int i = 0; i < qucs_s::runs::Tracker::kClosedMost + 3; ++i) runs()->noteClosed(path(QStringLiteral("n%1.sch").arg(i)), QStringLiteral("n%1").arg(i));
        QCOMPARE(runs()->closed().size(), qucs_s::runs::Tracker::kClosedMost);
        // A file gone: said, nothing opened.
        runs()->clearClosed();
        runs()->noteClosed(path("gone.qfem"), "gone.qfem");
        QVERIFY(!app->reopenClosedTab(0));
    }

    // Close All with one running and one not: one question; the running one
    // kept, the other closed.
    void closeAllKeepsWhatRuns()
    {
        const QString running = longTransient("all_running.qfem");
        const QString idle = longTransient("all_idle.qfem");
        MultiphysicsDoc* doc = openModel(running);
        QVERIFY(openModel(idle) != nullptr);
        QPointer<MultiphysicsDoc> kept(doc);
        doc->compute();
        answerWith("keepRunning");
        QVERIFY(app->closeAllFiles());
        QCOMPARE(answered, QStringLiteral("keepRunning "));
        QVERIFY(kept && runs()->isParked(doc) && doc->isBusy());
        QVERIFY(app->findDoc(idle) == nullptr);
        QCOMPARE(runs()->closed().size(), 2);
        // Opening the file brings it back, still running.
        QVERIFY(app->gotoPage(running));
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(doc));
        QVERIFY(doc->isBusy());
    }

    // A 3D model's solve, its tab closed as it begins: it ends without its tab,
    // finished, and comes back with its solution.
    void a3DModelSolvesWithItsTabClosed()
    {
        const QString file = path("kept3d.qfem3d");
        QFile::remove(file);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics3d/chip_heater.qfem3d"), file));
        QFile(file).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QVERIFY(app->gotoPage(file));
        auto* doc = qobject_cast<Multiphysics3DDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForIdle(120000));
        QPointer<Multiphysics3DDoc> kept(doc);
        doc->compute();
        QVERIFY(doc->isBusy());
        answerWith("keepRunning");
        closeTabOf(doc);
        QCOMPARE(answered, QStringLiteral("keepRunning "));
        QVERIFY(kept && runs()->isParked(doc));
        // It ends by itself, without its tab: finished, at 100 %.
        QVERIFY2(doc->waitForIdle(600000), "the 3D solve ended");
        runs()->pollNow();
        std::optional<Job> job;
        QTRY_VERIFY_WITH_TIMEOUT((job = userJobOf(file)).has_value() && isOver(job->status), 10000);
        QCOMPARE(job->status, Status::Finished);
        QCOMPARE(job->percent, 100);
        QCOMPARE(job->kind, QStringLiteral("solve"));
        // Back, with its solution.
        QVERIFY(doc->solution() != nullptr);
        QVERIFY(app->showJobDocument(*job));
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(doc));
        QVERIFY(doc->solution() != nullptr && !doc->solutionStale());
    }

    // An RF 2.5D model's solve, its tab closed as it begins: the same, back
    // with its extraction; and a solve stopped through the window.
    void anRf25ModelSolvesWithItsTabClosed()
    {
        const QString gds = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/thermo_optic_heater.gds");
        if (!QFileInfo::exists(gds)) QSKIP("the example layout is not in this build");
        QFile::remove(path("heater.gds"));
        QVERIFY(QFile::copy(gds, path("heater.gds")));
        // As test_qucs_control's line: a ground, a substrate, one metal.
        qucs_s::rf25::Model m = qucs_s::rf25::Model::blank();
        m.layout().props.insert(QStringLiteral("file"), QStringLiteral("heater.gds"));
        QJsonArray rows{QJsonArray{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")}};
        m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), rows);
        m.mesh().props.insert(QStringLiteral("maximum"), QStringLiteral("20[um]"));
        m.mesh().props.insert(QStringLiteral("cells_across"), 0);
        const QString file = write("kept.qrf25", m.toJson());
        QVERIFY(app->gotoPage(file));
        auto* doc = qobject_cast<RF25Doc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QVERIFY(doc->waitForLayout(120000));
        QVERIFY(doc->waitForPrepared(120000));
        QPointer<RF25Doc> kept(doc);
        doc->solve();
        QVERIFY(doc->isSolving());
        answerWith("keepRunning");
        closeTabOf(doc);
        QCOMPARE(answered, QStringLiteral("keepRunning "));
        QVERIFY(kept && runs()->isParked(doc));
        QVERIFY2(doc->waitForSolved(600000), "the RF 2.5D solve ended");
        runs()->pollNow();
        std::optional<Job> job;
        QTRY_VERIFY_WITH_TIMEOUT((job = userJobOf(file)).has_value() && isOver(job->status), 10000);
        QVERIFY2(job->status == Status::Finished, qPrintable(job->outcome));
        QVERIFY(doc->extraction() != nullptr);
        // Reopen Closed Tab: the same, with its extraction.
        QVERIFY(app->reopenClosedTab(0));
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(doc));
        QVERIFY(doc->extraction() != nullptr);

        // Solved again, stopped through the window's Stop (the registry's).
        doc->solve();
        QVERIFY(doc->isSolving());
        runs()->pollNow();
        std::optional<Job> again;
        QTRY_VERIFY_WITH_TIMEOUT((again = userJobOf(file)).has_value() && again->id != job->id, 10000);
        if (!isOver(registry().job(again->id)->status) && doc->isSolving()) {
            QVERIFY(registry().stop(again->id));
            QTRY_VERIFY_WITH_TIMEOUT(isOver(registry().job(again->id)->status), 60000);
            // (Stopped, or finished first: what happened is what is said.)
            QVERIFY(registry().job(again->id)->status == Status::Stopped || registry().job(again->id)->status == Status::Finished);
        }
    }

#ifndef Q_OS_WIN
    // A schematic's simulation (a simulator that says a percentage, then
    // takes its time), its tab closed: the run goes on, listed with its
    // percentage; reopened, the same schematic; stopped from the window.
    void aSchematicKeepsSimulatingWithItsTabClosed()
    {
        const QString simulatorWas = QucsSettings.NgspiceExecutable;
        const auto back = qScopeGuard([simulatorWas] { QucsSettings.NgspiceExecutable = simulatorWas; });
        const QString slow = write("slow-ngspice.sh", "#!/bin/sh\nprintf '%%42.00%%\\n'\nexec sleep 120\n");
        QFile::setPermissions(slow, QFile::permissions(slow) | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        QucsSettings.NgspiceExecutable = slow;
        // (Named as its dataset and data page are: no question of renaming them.)
        const QString file = path("RCL_resonance.sch");
        QFile::remove(file);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), file));
        QFile(file).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QVERIFY(app->gotoPage(file));
        Schematic* sch = app->currentSchematic();
        QVERIFY(sch != nullptr);
        QPointer<Schematic> kept(sch);
        app->slotSimulate();
        QTRY_VERIFY_WITH_TIMEOUT(userJobOf(file).has_value(), 10000);
        const QString id = userJobOf(file)->id;
        QCOMPARE(registry().job(id)->kind, QStringLiteral("simulation"));
        QVERIFY(registry().job(id)->name.contains("ngspice"));
        QVERIFY(runs()->isBusy(sch));

        answerWith("keepRunning");
        closeTabOf(sch);
        QCOMPARE(answered, QStringLiteral("keepRunning "));
        QVERIFY(kept && runs()->isParked(sch));
        QVERIFY(app->simulationConsole()->isRunning());
        QTRY_COMPARE_WITH_TIMEOUT(registry().job(id)->percent, 42, 20000);
        QVERIFY(!isOver(registry().job(id)->status));

        // Opened again: the same schematic, still simulating.
        QVERIFY(app->gotoPage(file));
        QCOMPARE(app->currentSchematic(), sch);
        QVERIFY(app->simulationConsole()->isRunning());

        // Closed again, and stopped from the window.
        answerWith("keepRunning");
        closeTabOf(sch);
        app->showRunningJobs();
        ClaudeJobsWindow* w = ClaudeJobsWindow::existing();
        auto* stop = w->table()->cellWidget(w->rowOf(id), ClaudeJobsWindow::Stop)->findChild<QToolButton*>("claudeJobStop");
        QVERIFY(stop->isEnabled());
        QTest::mouseClick(stop, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(registry().job(id)->status, Status::Stopped, 30000);
        QCOMPARE(registry().job(id)->percent, 42);
        QVERIFY(!app->simulationConsole()->isRunning());
        // Its data page was not opened for it, nor its tab: still held.
        QVERIFY(kept && runs()->isParked(sch));
    }

    // Qucsator's run (its own window, SimMessage) likewise: kept, listed,
    // stopped through the window.
    void aQucsatorRunKeepsGoingWithItsTabClosed()
    {
        const int simulatorWas = QucsSettings.DefaultSimulator;
        const QString qucsatorWas = QucsSettings.Qucsator;
        const auto back = qScopeGuard([simulatorWas, qucsatorWas] {
            QucsSettings.DefaultSimulator = simulatorWas;
            QucsSettings.Qucsator = qucsatorWas;
        });
        const QString slow = write("slow-qucsator.sh", "#!/bin/sh\nexec sleep 120\n");
        QFile::setPermissions(slow, QFile::permissions(slow) | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        QucsSettings.Qucsator = slow;
        const QString file = path("bridge.sch");
        QFile::remove(file);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/qucsator/General Electronics/bridge.sch"), file));
        QFile(file).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QVERIFY(app->gotoPage(file));
        Schematic* sch = app->currentSchematic();
        QVERIFY(sch != nullptr);
        QPointer<Schematic> kept(sch);
        app->slotSimulate();
        QTRY_VERIFY_WITH_TIMEOUT(userJobOf(file).has_value(), 10000);
        const QString id = userJobOf(file)->id;
        QVERIFY(registry().job(id)->name.contains("Qucsator"));
        QTRY_VERIFY_WITH_TIMEOUT(runs()->isBusy(sch), 10000);
        answerWith("keepRunning");
        closeTabOf(sch);
        QCOMPARE(answered, QStringLiteral("keepRunning "));
        QVERIFY(kept && runs()->isParked(sch));
        QVERIFY(!isOver(registry().job(id)->status));
        QVERIFY(registry().stop(id));
        QTRY_COMPARE_WITH_TIMEOUT(registry().job(id)->status, Status::Stopped, 30000);
        QVERIFY(kept && runs()->isParked(sch));
        // (Its window, the Qucsator's own, closed.)
        for (QDialog* d : app->findChildren<QDialog*>())
            if (d->inherits("SimMessage")) d->close();
    }
#endif

    // Claude's run is that tool's job, listed once, with its document; the
    // user closing its tab keeps it running - not lost - and it can be shown.
    // The tools tell the user's runs apart.
    void claudesRunIsListedOnce()
    {
        QucsControl* control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const QString file = longTransient("claudes.qfem");
        QVERIFY(openModel(file) != nullptr);
        auto* doc = qobject_cast<MultiphysicsDoc*>(app->DocumentTab->currentWidget());
        QPointer<MultiphysicsDoc> kept(doc);
        std::optional<QJsonObject> answer;
        control->callToolFor(7, "fem_solve", {{"path", file}, {"timeout", 600}}, [&answer](const QJsonObject& r) { answer = r; });
        QTRY_VERIFY_WITH_TIMEOUT(doc->isBusy(), 10000);
        runs()->pollNow();
        QTRY_VERIFY_WITH_TIMEOUT(registry().count() == 1 && !registry().jobs().first().document.isEmpty(), 10000);
        const Job job = registry().jobs().first();
        QCOMPARE(job.source, QStringLiteral("qucs-s"));
        QCOMPARE(startedBy(job), QStringLiteral("claude"));
        QVERIFY(QFileInfo(job.document) == QFileInfo(file));
        QVERIFY(!userJobOf(file).has_value());

        // The tools: started_by, document; the user's alone, Claude's alone.
        std::optional<QJsonObject> listed;
        control->callToolFor(8, "list_background_jobs", {}, [&listed](const QJsonObject& r) { listed = r; });
        QTRY_VERIFY(listed.has_value());
        const QJsonObject all = QJsonDocument::fromJson(listed->value("content").toArray().first().toObject().value("text").toString().toUtf8()).object();
        QCOMPARE(all.value("jobs").toArray().size(), 1);
        QCOMPARE(all.value("jobs").toArray().first().toObject().value("started_by").toString(), QStringLiteral("claude"));
        QVERIFY(all.value("jobs").toArray().first().toObject().value("document").toString().endsWith("claudes.qfem"));
        listed.reset();
        control->callToolFor(8, "list_background_jobs", {{"started_by", "user"}}, [&listed](const QJsonObject& r) { listed = r; });
        QTRY_VERIFY(listed.has_value());
        QCOMPARE(QJsonDocument::fromJson(listed->value("content").toArray().first().toObject().value("text").toString().toUtf8())
                     .object().value("jobs").toArray().size(), 0);
        listed.reset();
        control->callToolFor(8, "list_background_jobs", {{"started_by", "someone"}}, [&listed](const QJsonObject& r) { listed = r; });
        QTRY_VERIFY(listed.has_value());
        QVERIFY(listed->value("isError").toBool());

        // Closed by the user: kept running, Claude's job going on (not lost).
        answerWith("keepRunning");
        closeTabOf(doc);
        QVERIFY(kept && runs()->isParked(doc) && doc->isBusy());
        QTest::qWait(600);   // (several readings of the documents and of the calls)
        QVERIFY(!isOver(registry().job(job.id)->status));
        QCOMPARE(registry().count(), 1);
        // A tool asked of it while it has no tab: told where it is.
        std::optional<QJsonObject> state;
        control->callToolFor(8, "fem_describe", {{"path", file}}, [&state](const QJsonObject& r) { state = r; });
        QTRY_VERIFY(state.has_value());
        if (state->value("isError").toBool())
            QVERIFY2(QJsonDocument(*state).toJson().contains("open_document brings"), QJsonDocument(*state).toJson().constData());
        // Show brings it back; then Claude's stop by its job.
        QVERIFY(app->showJobDocument(*registry().job(job.id)));
        QCOMPARE(app->DocumentTab->currentWidget(), static_cast<QWidget*>(doc));
        QVERIFY(registry().stop(job.id));
        QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 60000);
        QCOMPARE(registry().job(job.id)->status, Status::Stopped);

        // show_background_jobs with a filter.
        std::optional<QJsonObject> shown;
        control->callToolFor(8, "show_background_jobs", {{"filter", "user"}}, [&shown](const QJsonObject& r) { shown = r; });
        QTRY_VERIFY(shown.has_value());
        QVERIFY(!shown->value("isError").toBool());
        QCOMPARE(ClaudeJobsWindow::existing()->filter()->currentData().toInt(), int(ClaudeJobsWindow::Yours));
        shown.reset();
        control->callToolFor(8, "show_background_jobs", {{"filter", "some"}}, [&shown](const QJsonObject& r) { shown = r; });
        QTRY_VERIFY(shown.has_value());
        QVERIFY(shown->value("isError").toBool());
    }

    // A tool of Claude's closing a running tab: nobody to ask; it closes, its
    // run stopped, as before.
    void aToolClosingARunningTabStopsIt()
    {
        QucsControl* control = app->findChild<QucsControl*>();
        const QString file = longTransient("toolclosed.qfem");
        MultiphysicsDoc* doc = openModel(file);
        QPointer<MultiphysicsDoc> kept(doc);
        doc->compute();
        QVERIFY(doc->isBusy());
        std::optional<QJsonObject> closed;
        control->callToolFor(7, "close_document", {{"path", file}}, [&closed](const QJsonObject& r) { closed = r; });
        QTRY_VERIFY(closed.has_value());
        QVERIFY2(!closed->value("isError").toBool(), QJsonDocument(*closed).toJson().constData());
        QVERIFY(answered.isEmpty());
        QVERIFY(!runs()->isParked(doc));
        QTRY_VERIFY_WITH_TIMEOUT(kept.isNull(), 60000);
    }

    // Quit with runs going: asked; Cancel stays, everything as it was. Stop
    // Them and Quit: a held one with changes back in a tab for the save
    // question; every held one let go, its run stopped.
    void quitWithRunsGoing()
    {
        const QString file = longTransient("quit.qfem");
        MultiphysicsDoc* doc = openModel(file);
        QPointer<MultiphysicsDoc> kept(doc);
        doc->compute();
        const QString other = longTransient("quit_changed.qfem");
        MultiphysicsDoc* changed = openModel(other);
        QPointer<MultiphysicsDoc> held(changed);
        changed->setDocChanged(true);
        changed->compute();
        answerWith("keepRunning");
        QVERIFY(app->closeAllFiles());
        QVERIFY(runs()->isParked(doc) && runs()->isParked(changed));

        // The window closed (as Cmd-Q does): asked, cancelled - nothing changes.
        answerWith("cancelQuit");
        QCloseEvent close;
        QApplication::sendEvent(app, &close);
        QVERIFY(!close.isAccepted());
        QCOMPARE(answered, QStringLiteral("keepRunning cancelQuit "));
        QVERIFY2(asked.contains("quit.qfem") && asked.contains("quit_changed.qfem"), qPrintable(asked));
        QVERIFY(!runs()->quitting());
        QVERIFY(kept && held && runs()->isParked(doc) && runs()->isParked(changed));
        QVERIFY(doc->isBusy());

        // Stop Them and Quit (the steps of the window's close, after its
        // question): the changed one back in a tab, asked about there; the
        // rest let go, stopped.
        answerWith("stopAndQuit");
        QVERIFY(app->confirmQuitWithRuns());
        QVERIFY(runs()->quitting());
        QVERIFY(held && !runs()->isParked(changed) && tabIndexOf(changed) >= 0);
        QVERIFY(runs()->isParked(doc));
        changed->setDocChanged(false);   // (as Don't Save in the save question)
        QVERIFY(app->closeAllFiles());   // (no question of keeping it running: quitting)
        QCOMPARE(answered, QStringLiteral("keepRunning cancelQuit stopAndQuit "));
        app->quitEnded(true);
        QTRY_VERIFY_WITH_TIMEOUT(kept.isNull() && held.isNull(), 60000);
        QCOMPARE(runs()->parked().size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(([this] {
            for (const Job& job : registry().jobs())
                if (!isOver(job.status)) return false;
            return true;
        }()), 10000);
        runs()->setQuitting(false);
        // Nothing running: no question.
        answered.clear();
        QVERIFY(app->confirmQuitWithRuns());
        QVERIFY(answered.isEmpty());
        runs()->setQuitting(false);
    }

    // Opt in (QUCS_RUNS_PICTURES=<folder>): the window, the toolbar and the
    // status bar's count, and the Close question, with real runs, light and dark.
    void pictures()
    {
        const QString folder = qEnvironmentVariable("QUCS_RUNS_PICTURES");
        if (folder.isEmpty()) QSKIP("QUCS_RUNS_PICTURES names a folder for the pictures");
        QDir().mkpath(folder);
        const QString closedFile = longTransient("heater_transient.qfem");
        const QString openFile = longTransient("bent_beam.qfem");
        MultiphysicsDoc* closedDoc = openModel(closedFile);
        MultiphysicsDoc* openDoc = openModel(openFile);
        QVERIFY(closedDoc != nullptr && openDoc != nullptr);
        openDoc->compute();
        closedDoc->compute();
        QTRY_VERIFY_WITH_TIMEOUT(userJobOf(closedFile).has_value() && userJobOf(openFile).has_value(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(registry().job(userJobOf(closedFile)->id)->percent >= 0, 60000);
        // The question, as it shows.
        app->showDocument(closedDoc);
        auto* timer = new QTimer(this);
        timer->setInterval(50);
        connect(timer, &QTimer::timeout, this, [this, timer, folder] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr || !box->isVisible()) return;
            timer->stop();
            timer->deleteLater();
            QTest::qWait(400);
            box->grab().save(QDir(folder).filePath("close-question.png"));
            box->findChild<QPushButton*>("keepRunning")->click();
        });
        timer->start();
        closeTabOf(closedDoc);
        QVERIFY(runs()->isParked(closedDoc));
        // Made-up rows beside the real ones: one of Claude's, one ended.
        Job claudes;
        claudes.source = "qucs-s";
        claudes.kind = "simulation";
        claudes.name = "simulate: amp.sch";
        claudes.conversation = "The amplifier";
        claudes.document = path("amp.sch");
        claudes.status = Status::Running;
        claudes.percent = 63;
        claudes.canStop = true;
        registry().add(claudes, [] { return true; });
        Job ended;
        ended.source = "user";
        ended.kind = "simulation";
        ended.name = "filter.sch: simulation (ngspice)";
        ended.document = path("filter.sch");
        ended.status = Status::Finished;
        ended.percent = 100;
        registry().add(ended);
        QTest::qWait(1000);
        app->showRunningJobs();
        ClaudeJobsWindow* w = ClaudeJobsWindow::existing();
        w->table()->selectRow(w->rowOf(userJobOf(closedFile)->id));
        QVERIFY(QTest::qWaitForWindowExposed(w));
        for (const Qt::ColorScheme scheme : {Qt::ColorScheme::Light, Qt::ColorScheme::Dark}) {
            QGuiApplication::styleHints()->setColorScheme(scheme);
            QTest::qWait(1200);
            const QString dark = scheme == Qt::ColorScheme::Dark ? "-dark" : "-light";
            QVERIFY(w->grab().save(QDir(folder).filePath("jobs" + dark + ".png")));
            // (The main window in the dark only: switched under it here, a light
            // capture keeps the dark tool bars of the look it began in.)
            if (scheme == Qt::ColorScheme::Dark) QVERIFY(app->grab().save(QDir(folder).filePath("main-window" + dark + ".png")));
            emit app->recentlyClosedMenu->aboutToShow();
            qInfo("%s (%s)", qPrintable(folder), qPrintable(QGuiApplication::platformName()));
        }
        QGuiApplication::styleHints()->unsetColorScheme();
    }

    // Finished documents held for their results: at most kIdleParkedMost,
    // the oldest let go first; a running one never.
    void heldDocumentsHaveALimit()
    {
        QList<QPointer<MultiphysicsDoc>> docs;
        for (int i = 0; i < qucs_s::runs::Tracker::kIdleParkedMost + 2; ++i) {
            MultiphysicsDoc* doc = openModel(longTransient(QStringLiteral("idle%1.qfem").arg(i)));
            QVERIFY(doc != nullptr);
            docs << doc;
            // (Held as a closed running tab is: kept without its tab.)
            const int index = app->DocumentTab->indexOf(doc);
            app->DocumentTab->removeTab(index);
            runs()->park(doc);
        }
        runs()->pollNow();
        QCOMPARE(int(runs()->parked().size()), qucs_s::runs::Tracker::kIdleParkedMost);
        QVERIFY(docs.at(0).isNull() && docs.at(1).isNull());
        QVERIFY(!docs.last().isNull());
    }
};

QTEST_MAIN(TestRunningJobs)
#include "test_running_jobs.moc"
