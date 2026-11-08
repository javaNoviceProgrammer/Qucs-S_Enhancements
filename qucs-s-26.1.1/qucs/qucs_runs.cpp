/*
 * qucs_runs.cpp - the runs in the window: Close asks before it stops one,
 *                 a document kept running with its tab closed and brought
 *                 back, the Running Jobs toolbar and menu items, the tabs
 *                 closed lately, and Quit with runs going
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucs.h"

#include "claudejobs.h"
#include "claudejobswindow.h"
#include "misc.h"
#include "qucscontrol.h"
#include "qucsdoc.h"
#include "qucsshortcutmanager.h"
#include "simulationruns.h"
#include "statusbar.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QIconEngine>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

namespace jobs = qucs_s::claude::jobs;

namespace {

// Drawn at each paint from the palette in use: right in a light and a dark
// look, and after a change of theme, with no icon files to keep in step.
class DrawnIcon : public QIconEngine
{
public:
    enum Shape { Jobs, Reopen };
    explicit DrawnIcon(Shape shape) : a_shape(shape) {}
    QIconEngine* clone() const override { return new DrawnIcon(a_shape); }
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const QPalette pal = QApplication::palette();
        QColor ink = pal.color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal s = std::min(rect.width(), rect.height()) / 16.0;
        p->translate(rect.center() - QPointF(8 * s, 8 * s));
        p->scale(s, s);
        QPen pen(ink, 1.4);
        pen.setCapStyle(Qt::RoundCap);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        if (a_shape == Jobs) {
            // Three rows, each a bar begun: the runs and how far each is.
            for (int row = 0; row < 3; ++row) {
                const qreal y = 3.5 + 4.5 * row;
                p->drawLine(QPointF(2, y), QPointF(14, y));
            }
            QPen thick(ink, 2.6);
            thick.setCapStyle(Qt::RoundCap);
            p->setPen(thick);
            p->drawLine(QPointF(2, 3.5), QPointF(10, 3.5));
            p->drawLine(QPointF(2, 8), QPointF(6, 8));
            p->drawLine(QPointF(2, 12.5), QPointF(12.5, 12.5));
        } else {
            // A tab, and an arrow turning back to it.
            QPainterPath tab;
            tab.moveTo(1.5, 14.5);
            tab.lineTo(1.5, 6.5);
            tab.lineTo(3.5, 4.5);
            tab.lineTo(8, 4.5);
            tab.lineTo(9.5, 6.5);
            tab.lineTo(9.5, 14.5);
            p->drawPath(tab);
            p->drawArc(QRectF(7.5, 1.5, 7, 7), 200 * 16, 250 * 16);
            p->setBrush(ink);
            QPolygonF head;
            head << QPointF(14.8, 5.6) << QPointF(12.3, 6.4) << QPointF(14.6, 8.4);
            p->drawPolygon(head);
        }
        p->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

private:
    Shape a_shape;
};

QString shownName(QucsDoc* doc)
{
    const QString name = doc->getDocName();
    return name.isEmpty() ? QObject::tr("untitled") : QFileInfo(name).fileName();
}

// What it is at: "study std1, 33 %".
QString detailOf(const qucs_s::runs::Activity& a)
{
    QString detail = a.what;
    if (a.percent >= 0) detail += (detail.isEmpty() ? QString() : QStringLiteral(", ")) + QStringLiteral("%1 %").arg(a.percent);
    return detail;
}

// "heater.qfem (study std1, 33 %)".
QString describe(const qucs_s::runs::Activity& a, QucsDoc* doc)
{
    const QString detail = detailOf(a);
    return detail.isEmpty() ? shownName(doc) : QStringLiteral("%1 (%2)").arg(shownName(doc), detail);
}

} // namespace

// ----------------------------------------------------------------------
void QucsApp::initRuns()
{
  a_runs = new qucs_s::runs::Tracker(this);
  // A run Claude's tool began is that tool's job (qucscontrol_jobs.cpp).
  a_runs->setClaudeLookup([this](QObject *document, quint64 *caller) {
    return a_control != nullptr ? a_control->jobWorkingOn(document, caller) : QString();
  });

  showJobsAction = new QAction(QIcon(new DrawnIcon(DrawnIcon::Jobs)), tr("Running Jobs..."), this);
  showJobsAction->setObjectName(QStringLiteral("showRunningJobs"));
  showJobsAction->setIconText(tr("Jobs"));
  showJobsAction->setStatusTip(tr("Shows every simulation, solve, sweep and optimization running or lately ended - yours and "
                                  "Claude's - with Stop, and Show to bring a closed tab back"));
  connect(showJobsAction, &QAction::triggered, this, &QucsApp::showRunningJobs);

  reopenClosedAction = new QAction(QIcon(new DrawnIcon(DrawnIcon::Reopen)), tr("Reopen Closed Tab"), this);
  reopenClosedAction->setObjectName(QStringLiteral("reopenClosedTab"));
  reopenClosedAction->setStatusTip(tr("Opens the tab closed last again; one kept running comes back as it is, with its run and "
                                      "its results"));
  connect(reopenClosedAction, &QAction::triggered, this, [this] { reopenClosedTab(0); });

  recentlyClosedMenu = new QMenu(tr("Recently Closed Tabs"), this);
  recentlyClosedMenu->setObjectName(QStringLiteral("recentlyClosedMenu"));
  connect(recentlyClosedMenu, &QMenu::aboutToShow, this, [this] { fillRecentlyClosedMenu(recentlyClosedMenu); });
  reopenClosedAction->setMenu(nullptr);

  // File: beside Open Recent. Simulation: beside Stop.
  if (fileMenu != nullptr) {
    fileMenu->insertAction(fileSave, reopenClosedAction);
    fileMenu->insertMenu(fileSave, recentlyClosedMenu);
    fileMenu->insertSeparator(fileSave);
  }
  if (simMenu != nullptr) simMenu->insertAction(tune, showJobsAction);

  QucsShortcutManager::instance().registerCommand("File.ReopenClosedTab", "File", "Reopen the tab closed last",
                                                  reopenClosedAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
  if (reopenClosedAction->shortcut().isEmpty()) reopenClosedAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));

  runsToolbar = new QToolBar(tr("Running Jobs"));
  runsToolbar->setObjectName("runsToolbar");
  addToolBar(runsToolbar);
  runsToolbar->addAction(showJobsAction);
  auto *reopen = new QToolButton(runsToolbar);
  reopen->setObjectName(QStringLiteral("reopenClosedButton"));
  reopen->setDefaultAction(reopenClosedAction);
  reopen->setMenu(recentlyClosedMenu);
  reopen->setPopupMode(QToolButton::MenuButtonPopup);
  runsToolbar->addWidget(reopen);
  if (auto *jobsButton = qobject_cast<QToolButton *>(runsToolbar->widgetForAction(showJobsAction))) {
    jobsButton->setObjectName(QStringLiteral("showRunningJobsButton"));
    jobsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  }

  // The window's Show: a closed tab back, an open one in front, a file opened.
  // (Guarded: the window's callback outlives this window in a process that
  // makes another, as the tests do.)
  ClaudeJobsWindow::setDocumentShower([self = QPointer<QucsApp>(this)](const jobs::Job &job, bool show) {
    return self && self->showJobDocument(job, show);
  });

  connect(a_runs, &qucs_s::runs::Tracker::changed, this, &QucsApp::updateRunsActions);
  connect(&jobs::Registry::instance(), &jobs::Registry::added, this, &QucsApp::updateRunsActions);
  connect(&jobs::Registry::instance(), &jobs::Registry::changed, this, &QucsApp::updateRunsActions);
  connect(&jobs::Registry::instance(), &jobs::Registry::removed, this, [this](const QString &) {
    releaseClearedDocuments();
    updateRunsActions();
  });
  updateRunsActions();
}

void QucsApp::endRuns()
{
  // As the window's children go - Claude's tools marking their jobs lost, a
  // document's run ending -, the jobs change: not to be told to this, half
  // destroyed, nor asked of it by the jobs window's Show.
  disconnect(&jobs::Registry::instance(), nullptr, this, nullptr);
  if (a_runs != nullptr) disconnect(a_runs, nullptr, this, nullptr);
  ClaudeJobsWindow::setDocumentShower({});
  showJobsAction = nullptr;
}

void QucsApp::updateRunsActions()
{
  if (showJobsAction == nullptr) return;
  int going = 0;
  for (const jobs::Job &job : jobs::Registry::instance().jobs())
    if (!jobs::isOver(job.status)) ++going;
  const QString text = going > 0 ? tr("Jobs (%1 running)").arg(going) : tr("Jobs");
  if (showJobsAction->iconText() != text) showJobsAction->setIconText(text);
  const int closed = a_runs != nullptr ? int(a_runs->closed().size()) : 0;
  reopenClosedAction->setEnabled(closed > 0);
  recentlyClosedMenu->setEnabled(closed > 0);
  QString tip = tr("Reopen Closed Tab");
  if (closed > 0) tip += QStringLiteral(": ") + a_runs->closed().constFirst().title;
  if (reopenClosedAction->toolTip() != tip) reopenClosedAction->setToolTip(tip);
  if (a_status != nullptr) a_status->jobsChanged();
}

void QucsApp::showRunningJobs()
{
  if (a_runs != nullptr) a_runs->pollNow();
  ClaudeJobsWindow::showWindow(this);
}

// ----------------------------------------------------------------------
QucsApp::RunningChoice QucsApp::askAboutRunning(const QList<QucsDoc *> &docs)
{
  if (a_runs == nullptr || a_runs->quitting()) return RunningChoice::Stop;
  QList<QucsDoc *> busy;
  for (QucsDoc *doc : docs)
    if (doc != nullptr && a_runs->isBusy(doc)) busy << doc;
  if (busy.isEmpty()) return RunningChoice::Stop;
  // (Read now: a run begun a moment ago is listed before it is asked about,
  // and its end - while the question waits, say - is seen as its end.)
  a_runs->pollNow();
  // Closed by a tool of Claude's: nobody to ask, and the call asked to close
  // it - as before, its run stops with it.
  if (misc::ErrorCapture::active()) return RunningChoice::Stop;

  QStringList lines;
  for (QucsDoc *doc : std::as_const(busy)) lines << describe(a_runs->activity(doc), doc);
  QMessageBox box(QMessageBox::Question, tr("Close"),
                  busy.size() == 1 ? (detailOf(a_runs->activity(busy.constFirst())).isEmpty()
                                          ? tr("%1 is still running.").arg(shownName(busy.constFirst()))
                                          : tr("%1 is still running: %2.").arg(shownName(busy.constFirst()), detailOf(a_runs->activity(busy.constFirst()))))
                                   : tr("%n document(s) are still running:", nullptr, int(busy.size())) + QLatin1Char('\n')
                                         + lines.join(QLatin1Char('\n')),
                  QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("closeRunningQuestion"));
  box.setInformativeText(busy.size() == 1
                             ? tr("Keep it running in the background: its tab closes, and the run goes on. The Jobs button, "
                                  "Simulation > Running Jobs, Reopen Closed Tab or opening the file again brings it back, "
                                  "with its results.")
                             : tr("Keep them running in the background: their tabs close, and the runs go on. The Jobs "
                                  "button, Simulation > Running Jobs, Reopen Closed Tab or opening a file again brings each "
                                  "back, with its results."));
  QPushButton *keep = box.addButton(busy.size() == 1 ? tr("Keep Running in Background") : tr("Keep Them Running"),
                                    QMessageBox::AcceptRole);
  keep->setObjectName(QStringLiteral("keepRunning"));
  QPushButton *stop = box.addButton(busy.size() == 1 ? tr("Stop and Close") : tr("Stop Them and Close"), QMessageBox::DestructiveRole);
  stop->setObjectName(QStringLiteral("stopAndClose"));
  QPushButton *cancel = box.addButton(QMessageBox::Cancel);
  cancel->setObjectName(QStringLiteral("cancelClose"));
  box.setDefaultButton(keep);
  box.setEscapeButton(cancel);
  box.exec();
  if (box.clickedButton() == keep) return RunningChoice::Keep;
  if (box.clickedButton() == stop) {
    for (QucsDoc *doc : std::as_const(busy)) a_runs->stop(doc);
    return RunningChoice::Stop;
  }
  return RunningChoice::Cancel;
}

void QucsApp::keepRunningWithoutTab(QucsDoc *doc, const QString &title)
{
  QWidget *w = documentWidget(doc);
  if (w == nullptr || a_runs == nullptr) return;
  a_runs->park(w);
  a_runs->noteClosed(doc->getDocName(), title, w);
  statusBar()->showMessage(tr("%1 keeps running in the background: the Jobs button or Ctrl+Shift+T brings it back.")
                               .arg(shownName(doc)),
                           8000);
}

bool QucsApp::bringBackDocument(QWidget *document)
{
  if (a_runs == nullptr || document == nullptr || !a_runs->isParked(document)) return false;
  auto *frame = qobject_cast<QFrame *>(document);
  QucsDoc *doc = docIn(document);
  if (frame == nullptr || doc == nullptr) return false;
  a_runs->unpark(document);
  const int index = addDocumentTab(frame, QString());
  setDocumentChanged(document, doc->getDocChanged());
  doc->becomeCurrent(true);
  DocumentTab->setCurrentIndex(index);
  statusBar()->showMessage(tr("%1 is back, as it was left.").arg(shownName(doc)), 4000);
  updateRunsActions();
  return true;
}

bool QucsApp::showJobDocument(const jobs::Job &job, bool show)
{
  if (a_runs == nullptr) return false;
  QWidget *w = a_runs->documentOfJob(job.id);
  if (w == nullptr && !job.document.isEmpty()) {
    w = a_runs->parkedFile(job.document);
    if (w == nullptr) w = documentWidget(findDoc(job.document));
  }
  if (w != nullptr && a_runs->isParked(w)) return !show || bringBackDocument(w);
  if (w != nullptr && paneOf(w) != nullptr) {
    if (show) showDocument(w);
    return true;
  }
  if (job.document.isEmpty() || !QFileInfo(job.document).isFile()) return false;
  return !show || gotoPage(job.document);
}

void QucsApp::releaseClearedDocuments()
{
  // A finished document held only for its row's results: its row cleared, it
  // goes - unless it has changes not saved, when it comes back to be saved.
  if (a_runs == nullptr) return;
  for (QWidget *w : a_runs->parked()) {
    QucsDoc *doc = docIn(w);
    if (doc == nullptr || a_runs->isBusy(doc)) continue;
    const QString job = a_runs->jobOfParked(w);
    if (job.isEmpty() || jobs::Registry::instance().job(job)) continue;   // (its row still listed)
    if (doc->getDocChanged()) {
      bringBackDocument(w);
      continue;
    }
    a_runs->releaseOne(w);
  }
}

// ----------------------------------------------------------------------
void QucsApp::fillRecentlyClosedMenu(QMenu *menu)
{
  menu->clear();
  const QList<qucs_s::runs::Tracker::Closed> closed = a_runs != nullptr ? a_runs->closed() : QList<qucs_s::runs::Tracker::Closed>();
  if (closed.isEmpty()) {
    menu->addAction(tr("No tab closed lately"))->setEnabled(false);
    return;
  }
  for (const qucs_s::runs::Tracker::Closed &c : closed) {
    QString text = c.title.isEmpty() ? QFileInfo(c.file).fileName() : c.title;
    if (QucsDoc *doc = c.parked ? docIn(c.parked.data()) : nullptr) {
      const qucs_s::runs::Activity a = a_runs->activity(doc);
      text += QStringLiteral("  -  ") + (a.busy ? (a.percent >= 0 ? tr("running, %1 %").arg(a.percent) : tr("running"))
                                                 : tr("kept, with its results"));
    }
    QAction *action = menu->addAction(text.replace(QLatin1Char('&'), QLatin1String("&&")));
    action->setToolTip(QDir::toNativeSeparators(c.file));
    action->setStatusTip(QDir::toNativeSeparators(c.file));
    // (The entry itself, found again: the list may have changed since.)
    connect(action, &QAction::triggered, this, [this, file = c.file, parked = c.parked, when = c.when] {
      const QList<qucs_s::runs::Tracker::Closed> now = a_runs->closed();
      for (int at = 0; at < now.size(); ++at)
        if (now.at(at).file == file && now.at(at).parked == parked && now.at(at).when == when) {
          reopenClosedTab(at);
          return;
        }
    });
  }
  menu->addSeparator();
  QAction *clear = menu->addAction(tr("Clear List"));
  clear->setObjectName(QStringLiteral("clearRecentlyClosed"));
  connect(clear, &QAction::triggered, this, [this] {
    // (Those held stay held: the Jobs window still has them.)
    if (a_runs != nullptr) a_runs->clearClosed();
  });
}

bool QucsApp::reopenClosedTab(int index)
{
  if (a_runs == nullptr) return false;
  const qucs_s::runs::Tracker::Closed c = a_runs->takeClosed(index);
  if (c.parked) return bringBackDocument(c.parked.data());
  if (c.file.isEmpty()) return false;
  if (!QFileInfo(c.file).isFile()) {
    statusBar()->showMessage(tr("%1 is not there any more.").arg(QDir::toNativeSeparators(c.file)), 5000);
    return false;
  }
  return gotoPage(c.file);
}

// ----------------------------------------------------------------------
bool QucsApp::confirmQuitWithRuns()
{
  if (a_runs == nullptr) return true;
  a_runs->pollNow();
  const QStringList going = a_runs->runningNow();
  if (!going.isEmpty() && !misc::ErrorCapture::active()) {
    QMessageBox box(QMessageBox::Warning, tr("Quit"),
                    tr("%n simulation(s) or solve(s) are still running:", nullptr, int(going.size())) + QLatin1Char('\n')
                        + going.join(QLatin1Char('\n')),
                    QMessageBox::NoButton, this);
    box.setObjectName(QStringLiteral("quitWithRunsQuestion"));
    box.setInformativeText(tr("Quitting stops them, and what they have not finished is lost."));
    QPushButton *quit = box.addButton(tr("Stop Them and Quit"), QMessageBox::DestructiveRole);
    quit->setObjectName(QStringLiteral("stopAndQuit"));
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    cancel->setObjectName(QStringLiteral("cancelQuit"));
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() != quit) return false;
  }
  a_runs->setQuitting(true);
  // Those kept without a tab with changes not saved: back in a tab, where the
  // save question that every tab gets asks of them too.
  for (QWidget *w : a_runs->parkedWithChanges()) bringBackDocument(w);
  return true;
}

void QucsApp::quitEnded(bool quitting)
{
  if (a_runs == nullptr) return;
  if (quitting) a_runs->releaseParked();
  else a_runs->setQuitting(false);
}
