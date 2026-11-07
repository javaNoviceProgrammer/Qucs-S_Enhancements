/*
 * claudejobswindow.cpp - the Background Jobs window
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "claudejobswindow.h"

#include "claudejobs.h"
#include "ink.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace qucs_s::claude::jobs;

namespace {

QPointer<ClaudeJobsWindow> g_window;

const char* const kBarName = "claudeJobProgress";
const char* const kPercentName = "claudeJobPercent";
const char* const kStopName = "claudeJobStop";

QString timeText(const QDateTime& when)
{
    if (!when.isValid()) return QStringLiteral("-");
    const QLocale locale;
    return when.date() == QDate::currentDate() ? when.time().toString(QStringLiteral("HH:mm:ss"))
                                               : locale.toString(when, QLocale::ShortFormat);
}

QString kindText(const QString& kind)
{
    // (A translator's table of what Job::kind may be.)
    if (kind == QLatin1String("command")) return ClaudeJobsWindow::tr("Command");
    if (kind == QLatin1String("agent")) return ClaudeJobsWindow::tr("Agent");
    if (kind == QLatin1String("workflow")) return ClaudeJobsWindow::tr("Workflow");
    if (kind == QLatin1String("monitor")) return ClaudeJobsWindow::tr("Monitor");
    if (kind == QLatin1String("task")) return ClaudeJobsWindow::tr("Task");
    if (kind == QLatin1String("simulation")) return ClaudeJobsWindow::tr("Simulation");
    if (kind == QLatin1String("solve")) return ClaudeJobsWindow::tr("Solve");
    if (kind == QLatin1String("build")) return ClaudeJobsWindow::tr("Build");
    if (kind == QLatin1String("tune")) return ClaudeJobsWindow::tr("Tuning");
    if (kind == QLatin1String("script")) return ClaudeJobsWindow::tr("Script");
    if (kind == QLatin1String("batch")) return ClaudeJobsWindow::tr("Batch");
    if (kind == QLatin1String("tool")) return ClaudeJobsWindow::tr("Qucs-S tool");
    return kind;
}

QColor statusColour(Status status, const QPalette& palette)
{
    const bool dark = qucs_s::ink::isDark(palette.color(QPalette::Base));
    switch (status) {
    case Status::Finished: return dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37);
    case Status::Failed: return dark ? QColor(0xf8, 0x51, 0x49) : QColor(0xcf, 0x22, 0x2e);
    case Status::Stopped:
    case Status::Stopping: return dark ? QColor(0xd2, 0x99, 0x22) : QColor(0x9a, 0x67, 0x00);
    case Status::Lost:
    case Status::Queued: return palette.color(QPalette::PlaceholderText);
    case Status::Started:
    case Status::Running: return palette.color(QPalette::Text);
    }
    return palette.color(QPalette::Text);
}

// What a finished job's time reads: from when it began to when it ended.
QString timeTaken(const Job& job)
{
    if (!job.began.isValid()) return QStringLiteral("-");
    const QDateTime until = job.ended.isValid() ? job.ended : QDateTime::currentDateTime();
    return durationText(job.began.secsTo(until));
}

QString stopTip(const Job& job)
{
    if (isOver(job.status)) return {};
    if (job.status == Status::Stopping) return ClaudeJobsWindow::tr("A stop was asked for: this says stopped once its end is seen.");
    if (job.canStop) return ClaudeJobsWindow::tr("Stops this job and nothing else.");
    return job.whyNoStop.isEmpty() ? ClaudeJobsWindow::tr("This job cannot be stopped from here.") : job.whyNoStop;
}

} // namespace

ClaudeJobsWindow* ClaudeJobsWindow::existing()
{
    return g_window;
}

ClaudeJobsWindow* ClaudeJobsWindow::showWindow(QWidget* from)
{
    if (!g_window) {
        // (Above the application's main window, not a floating dock, which
        // hides and comes back by itself.)
        QWidget* parent = from;
        for (QWidget* w = from; w != nullptr; w = w->parentWidget())
            if (qobject_cast<QMainWindow*>(w) != nullptr) parent = w;
        g_window = new ClaudeJobsWindow(parent != nullptr ? parent->window() : nullptr);
    }
    g_window->show();
    if (g_window->isMinimized()) g_window->showNormal();
    g_window->raise();
    g_window->activateWindow();
    return g_window;
}

ClaudeJobsWindow::ClaudeJobsWindow(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("claudeJobsWindow"));
    setWindowTitle(tr("Background Jobs"));
    auto* layout = new QVBoxLayout(this);

    auto* note = new QLabel(tr("What Claude has run, in every conversation: Claude Code's commands and agents, and Qucs-S's "
                               "long tools. A percentage is shown where the job itself reports one; where it cannot, the bar "
                               "runs on and says unknown."),
                            this);
    note->setObjectName(QStringLiteral("claudeJobsNote"));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* split = new QSplitter(Qt::Vertical, this);
    layout->addWidget(split, 1);

    a_table = new QTableWidget(0, Columns, split);
    a_table->setObjectName(QStringLiteral("claudeJobsTable"));
    a_table->setHorizontalHeaderLabels({tr("Job"), tr("Kind"), tr("Conversation"), tr("Status"), tr("Progress"), tr("Began"), tr("Time"), QString()});
    a_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    a_table->setSelectionMode(QAbstractItemView::SingleSelection);
    a_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_table->setWordWrap(false);
    a_table->setTextElideMode(Qt::ElideRight);
    a_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    a_table->setShowGrid(false);
    a_table->setAlternatingRowColors(true);
    a_table->verticalHeader()->hide();
    a_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    split->addWidget(a_table);

    a_details = new QPlainTextEdit(split);
    a_details->setObjectName(QStringLiteral("claudeJobsDetails"));
    a_details->setReadOnly(true);
    a_details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    a_details->setPlaceholderText(tr("Choose a job to see all that is known of it."));
    split->addWidget(a_details);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);

    auto* bottom = new QHBoxLayout;
    a_summary = new QLabel(this);
    a_summary->setObjectName(QStringLiteral("claudeJobsSummary"));
    // (Word wrap: a label of one line asks for its whole width.)
    a_summary->setWordWrap(true);
    bottom->addWidget(a_summary, 1);
    a_clear = new QPushButton(tr("Clear Finished"), this);
    a_clear->setObjectName(QStringLiteral("claudeJobsClear"));
    a_clear->setToolTip(tr("Takes the jobs that have finished, failed or stopped off the list. Those still going stay."));
    a_clear->setAutoDefault(false);
    bottom->addWidget(a_clear);
    auto* close = new QPushButton(tr("Close"), this);
    close->setObjectName(QStringLiteral("claudeJobsClose"));
    close->setAutoDefault(false);
    bottom->addWidget(close);
    layout->addLayout(bottom);

    setWidths();
    resize(1100, 520);
    setMinimumSize(420, 240);

    a_clock = new QTimer(this);
    a_clock->setInterval(1000);
    connect(a_clock, &QTimer::timeout, this, &ClaudeJobsWindow::tick);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(a_clear, &QPushButton::clicked, this, [] { Registry::instance().clearFinished(); });
    connect(a_table, &QTableWidget::itemSelectionChanged, this, &ClaudeJobsWindow::showDetails);

    Registry& registry = Registry::instance();
    connect(&registry, &Registry::added, this, [this](const QString& id) {
        if (const auto job = Registry::instance().job(id)) {
            addRow(*job);
            showSummary();
        }
    });
    connect(&registry, &Registry::changed, this, [this](const QString& id) {
        if (const auto job = Registry::instance().job(id)) {
            showJob(*job);
            showSummary();
            if (a_table->currentRow() == rowOf(id)) showDetails();
        }
    });
    connect(&registry, &Registry::removed, this, &ClaudeJobsWindow::removeJob);
    reload();
}

void ClaudeJobsWindow::setWidths()
{
    QHeaderView* header = a_table->horizontalHeader();
    const QFontMetrics metrics(a_table->font());
    const auto width = [&metrics](const QString& text) { return metrics.horizontalAdvance(text) + 24; };
    // The name takes the room there is: its text is cut, never the window
    // made wider for it (docs/feature_gaps/2026-11-02-long-lines.md). The rest
    // fit what they say.
    header->setMinimumSectionSize(24);
    header->setSectionResizeMode(Name, QHeaderView::Stretch);
    header->setStretchLastSection(false);
    const struct {
        Column column;
        int width;
        QHeaderView::ResizeMode mode;
    } fixed[] = {
        {Kind, width(tr("Qucs-S tool")), QHeaderView::Interactive},
        {Conversation, 120, QHeaderView::Interactive},
        {Status, width(tr("stopping")), QHeaderView::Interactive},
        {Progress, 170, QHeaderView::Interactive},
        {Began, width(QStringLiteral("00:00:00")), QHeaderView::Interactive},
        {Time, width(tr("59 min 59 s")), QHeaderView::Interactive},
        {Stop, width(tr("Stopping")) + 12, QHeaderView::Fixed},
    };
    for (const auto& f : fixed) {
        header->setSectionResizeMode(f.column, f.mode);
        a_table->setColumnWidth(f.column, f.width);
    }
}

void ClaudeJobsWindow::reload()
{
    a_table->setRowCount(0);
    // (Listed the newest first; each put on top.)
    const QList<Job> jobs = Registry::instance().jobs();
    for (qsizetype i = jobs.size() - 1; i >= 0; --i) addRow(jobs.at(i));
    showSummary();
}

int ClaudeJobsWindow::rowOf(const QString& id) const
{
    for (int row = 0; row < a_table->rowCount(); ++row)
        if (const QTableWidgetItem* item = a_table->item(row, Name); item != nullptr && item->data(Qt::UserRole).toString() == id) return row;
    return -1;
}

void ClaudeJobsWindow::addRow(const Job& job)
{
    if (rowOf(job.id) >= 0) return showJob(job);
    // (A selected row stays selected: the rows after it move down.)
    a_table->insertRow(0);
    auto* name = new QTableWidgetItem;
    name->setData(Qt::UserRole, job.id);
    a_table->setItem(0, Name, name);
    for (const Column c : {Kind, Conversation, Status, Began, Time}) a_table->setItem(0, c, new QTableWidgetItem);

    auto* cell = new QWidget(a_table);
    auto* row = new QHBoxLayout(cell);
    row->setContentsMargins(4, 2, 4, 2);
    auto* bar = new QProgressBar(cell);
    bar->setObjectName(QLatin1String(kBarName));
    bar->setTextVisible(false);
    bar->setMaximumHeight(a_table->fontMetrics().height());
    bar->setMinimumWidth(40);
    row->addWidget(bar, 1);
    auto* percent = new QLabel(cell);
    percent->setObjectName(QLatin1String(kPercentName));
    row->addWidget(percent);
    a_table->setCellWidget(0, Progress, cell);

    // (In a cell of its own: the view shows a cell's widget again whenever it
    // lays itself out, so one hidden has to be inside it.)
    auto* stopCell = new QWidget(a_table);
    auto* stopRow = new QHBoxLayout(stopCell);
    stopRow->setContentsMargins(2, 1, 2, 1);
    auto* stop = new QToolButton(stopCell);
    stop->setObjectName(QLatin1String(kStopName));
    stop->setText(tr("Stop"));
    stop->setToolButtonStyle(Qt::ToolButtonTextOnly);
    stopRow->addWidget(stop);
    const QString id = job.id;
    connect(stop, &QToolButton::clicked, this, [this, id] {
        QString why;
        if (!Registry::instance().stop(id, &why) && !why.isEmpty()) a_summary->setText(why);
    });
    a_table->setCellWidget(0, Stop, stopCell);
    showJob(job);
}

void ClaudeJobsWindow::removeJob(const QString& id)
{
    const int row = rowOf(id);
    if (row < 0) return;
    a_table->removeRow(row);
    showSummary();
    showDetails();
}

void ClaudeJobsWindow::showJob(const Job& job)
{
    const int row = rowOf(job.id);
    if (row < 0) return addRow(job);
    const auto set = [this, row](Column column, const QString& text, const QString& tip = {}) {
        QTableWidgetItem* item = a_table->item(row, column);
        if (item->text() != text) item->setText(text);
        const QString wanted = tip.isEmpty() ? text : tip;
        if (item->toolTip() != wanted) item->setToolTip(wanted);
    };
    // (Its name whole in the tool tip, and the view cuts it: the column takes
    // what room there is.)
    QString tip = job.name;
    if (!job.said.isEmpty()) tip += QLatin1Char('\n') + job.said;
    if (!job.outcome.isEmpty()) tip += QLatin1Char('\n') + job.outcome;
    set(Name, job.name, tip);
    set(Kind, kindText(job.kind));
    set(Conversation, job.conversation);
    set(Status, statusText(job.status), job.said);
    a_table->item(row, Status)->setForeground(statusColour(job.status, palette()));
    set(Began, timeText(job.began), job.queued.isValid() ? tr("Queued at %1, began at %2").arg(timeText(job.queued), timeText(job.began)) : QString());
    set(Time, timeTaken(job), job.ended.isValid() ? tr("Ended at %1").arg(timeText(job.ended)) : QString());

    QWidget* cell = a_table->cellWidget(row, Progress);
    auto* bar = cell->findChild<QProgressBar*>(QLatin1String(kBarName));
    auto* percent = cell->findChild<QLabel*>(QLatin1String(kPercentName));
    if (job.percent >= 0) {
        bar->setRange(0, 100);
        bar->setValue(job.percent);
        bar->setVisible(true);
    } else if (isOver(job.status)) {
        // Ended without saying: an empty bar would claim nothing was done.
        bar->setVisible(false);
    } else if (job.status == Status::Queued) {
        bar->setRange(0, 100);
        bar->setValue(0);
        bar->setVisible(true);
    } else {
        // Under way, how far not known: it runs on, and says so.
        bar->setRange(0, 0);
        bar->setVisible(true);
    }
    const QString text = job.status == Status::Queued && job.percent < 0 ? tr("waiting") : percentText(job);
    if (percent->text() != text) percent->setText(text);
    const QString why = job.percent < 0 ? job.whyNoPercent : QString();
    cell->setToolTip(why);
    bar->setToolTip(why);
    percent->setToolTip(why);
    bar->setAccessibleName(tr("Progress of %1: %2").arg(job.name, text));

    auto* stop = a_table->cellWidget(row, Stop)->findChild<QToolButton*>(QLatin1String(kStopName));
    stop->setVisible(!isOver(job.status));
    stop->setEnabled(job.canStop && job.status != Status::Stopping && !isOver(job.status));
    stop->setText(job.status == Status::Stopping ? tr("Stopping") : tr("Stop"));
    const QString stopWhy = stopTip(job);
    if (stop->toolTip() != stopWhy) stop->setToolTip(stopWhy);
}

void ClaudeJobsWindow::tick()
{
    for (const Job& job : Registry::instance().jobs()) {
        if (isOver(job.status)) continue;
        if (const int row = rowOf(job.id); row >= 0) a_table->item(row, Time)->setText(timeTaken(job));
    }
    if (a_table->currentRow() >= 0) showDetails();
}

void ClaudeJobsWindow::showSummary()
{
    int going = 0, queued = 0, over = 0;
    for (const Job& job : Registry::instance().jobs()) {
        if (isOver(job.status)) ++over;
        else if (job.status == Status::Queued) ++queued;
        else ++going;
    }
    a_summary->setText(tr("%1 going, %2 queued, %3 over").arg(going).arg(queued).arg(over));
    a_clear->setEnabled(over > 0);
}

void ClaudeJobsWindow::showDetails()
{
    // (A cell's label follows its row's selection, as an item's text does.)
    for (int row = 0; row < a_table->rowCount(); ++row) {
        const QWidget* cell = a_table->cellWidget(row, Progress);
        auto* label = cell != nullptr ? cell->findChild<QLabel*>(QLatin1String(kPercentName)) : nullptr;
        if (label == nullptr) continue;
        const QPalette::ColorRole role = row == a_table->currentRow() ? QPalette::HighlightedText : QPalette::Text;
        if (label->foregroundRole() != role) label->setForegroundRole(role);
    }
    const QTableWidgetItem* item = a_table->currentRow() >= 0 ? a_table->item(a_table->currentRow(), Name) : nullptr;
    const auto job = item != nullptr ? Registry::instance().job(item->data(Qt::UserRole).toString()) : std::nullopt;
    QString text;
    if (job) {
        QStringList lines;
        lines << job->name;
        lines << tr("Status: %1").arg(statusText(job->status));
        lines << tr("Progress: %1").arg(percentText(*job));
        if (job->percent < 0 && !job->whyNoPercent.isEmpty()) lines << tr("Why no percentage: %1").arg(job->whyNoPercent);
        if (!job->canStop && !isOver(job->status) && !job->whyNoStop.isEmpty()) lines << tr("Why no Stop: %1").arg(job->whyNoStop);
        if (job->queued.isValid()) lines << tr("Queued: %1").arg(QLocale().toString(job->queued, QLocale::LongFormat));
        if (job->began.isValid()) lines << tr("Began: %1").arg(QLocale().toString(job->began, QLocale::LongFormat));
        if (job->ended.isValid()) lines << tr("Ended: %1").arg(QLocale().toString(job->ended, QLocale::LongFormat));
        if (job->began.isValid()) lines << tr("Time: %1").arg(timeTaken(*job));
        lines << (job->background ? tr("In the background: Claude does not wait on it.") : tr("In the foreground: Claude waits on it."));
        if (!job->conversation.isEmpty()) lines << tr("Conversation: %1").arg(job->conversation);
        if (!job->said.isEmpty()) lines << tr("Last said: %1").arg(job->said);
        if (!job->outcome.isEmpty()) lines << tr("Outcome: %1").arg(job->outcome);
        if (!job->outputFile.isEmpty()) lines << tr("Output: %1").arg(job->outputFile);
        if (!job->parentTask.isEmpty()) lines << tr("Run by the agent: %1").arg(job->parentTask);
        if (!job->detail.isEmpty()) lines << QString() << job->detail;
        text = lines.join(QLatin1Char('\n'));
    }
    if (a_details->toPlainText() != text) a_details->setPlainText(text);
}

void ClaudeJobsWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::ApplicationPaletteChange) {
        if (a_table != nullptr)
            for (const Job& job : Registry::instance().jobs()) {
                if (const int row = rowOf(job.id); row >= 0) a_table->item(row, Status)->setForeground(statusColour(job.status, palette()));
            }
    }
}

void ClaudeJobsWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    a_clock->start();
    tick();
}

void ClaudeJobsWindow::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    a_clock->stop();
}
