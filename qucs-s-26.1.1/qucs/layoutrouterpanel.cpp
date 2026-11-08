/*
 * layoutrouterpanel.cpp - the layout tab's Route panel
 *                         (layoutrouterpanel.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layoutrouterpanel.h"

#include "layoutdoc.h"
#include "layoutrouter/router_gds.h"
#include "layoutrouting.h"
#include "markup.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

using namespace qucs_s::router;
using qucs_s::layout::LayoutView;

namespace qucs_s::routing {

namespace {

// A metal's colour over the layout: drawn translucent so the layout shows.
QColor metalColour(int metal)
{
    static const QColor colours[] = {QColor(230, 120, 20), QColor(30, 110, 230), QColor(30, 170, 80), QColor(170, 60, 200),
                                     QColor(200, 170, 0), QColor(0, 170, 190), QColor(140, 90, 40), QColor(220, 60, 120)};
    return colours[metal % 8];
}

QString micro(double v)
{
    return QString::number(v, 'f', 3).remove(QRegularExpression(QStringLiteral("\\.?0+$")));
}

QPushButton* button(const QString& text, const QString& tip, QWidget* parent)
{
    auto* b = new QPushButton(text, parent);
    b->setToolTip(tip);
    b->setAutoDefault(false);
    return b;
}

} // namespace

RouterPanel::RouterPanel(LayoutDoc* doc, QWidget* parent) : QWidget(parent), a_doc(doc)
{
    setObjectName(QStringLiteral("routerPanel"));
    a_session = Session::of(doc->getDocName());
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto* title = new QLabel(tr("<b>Route</b> - lines on the metals, contact to pad"), this);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto* techRow = new QHBoxLayout;
    a_tech = new QLineEdit(this);
    a_tech->setPlaceholderText(tr("Technology file (.json)"));
    a_tech->setToolTip(tr("The process's rules: metals, vias, classes of line, spacing, obstacle layers, the grid (relative to the layout's folder)"));
    auto* browse = new QToolButton(this);
    browse->setText(QStringLiteral("…"));
    browse->setToolTip(tr("Choose the technology file"));
    techRow->addWidget(a_tech, 1);
    techRow->addWidget(browse);
    layout->addLayout(techRow);

    a_nets = new QTableWidget(0, 4, this);
    a_nets->setHorizontalHeaderLabels({tr("Net"), tr("Class"), tr("Pads"), tr("Status")});
    a_nets->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    a_nets->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    a_nets->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    a_nets->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    a_nets->verticalHeader()->hide();
    a_nets->setSelectionBehavior(QAbstractItemView::SelectRows);
    a_nets->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_nets->setToolTip(tr("The nets: a start (contact) and its pads. Select some to route them alone."));
    layout->addWidget(a_nets, 2);

    auto* netRow = new QHBoxLayout;
    a_addNet = button(tr("Add Net"), tr("Click its contact on the layout, then its pads; then Finish"), this);
    auto* csv = button(tr("CSV…"), tr("Nets from a CSV: net, start x, start y, pad x, pad y[, class] - or with a header naming its columns"), this);
    auto* removeNets = button(tr("Remove"), tr("Take the selected nets out of the job"), this);
    netRow->addWidget(a_addNet);
    netRow->addWidget(csv);
    netRow->addWidget(removeNets);
    layout->addLayout(netRow);

    auto* penRow = new QHBoxLayout;
    auto* strokes = button(tr("Use Pen Strokes"), tr("The pen's strokes on the cell routed: a ring becomes a keep-out, a line from a net's contact that net's guide"), this);
    auto* removeKeep = button(tr("Remove Keep-out"), tr("Take the selected keep-outs away"), this);
    penRow->addWidget(strokes);
    penRow->addWidget(removeKeep);
    layout->addLayout(penRow);
    a_keepOuts = new QListWidget(this);
    a_keepOuts->setToolTip(tr("Keep-outs: no line goes in, but a net named in one or with a terminal inside takes its shortest way in"));
    a_keepOuts->setMaximumHeight(70);
    a_keepOuts->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(a_keepOuts);

    auto* runRow = new QHBoxLayout;
    a_mode = new QComboBox(this);
    a_mode->addItem(tr("All nets"), QStringLiteral("all"));
    a_mode->addItem(tr("Selected nets"), QStringLiteral("selected"));
    a_mode->addItem(tr("Changed nets"), QStringLiteral("changed"));
    a_mode->setToolTip(tr("Changed: nets whose terminals, class, guide, the shapes near them or the rules changed since the last run, and nets not routed yet; the others keep their routes"));
    a_run = button(tr("Run"), tr("Route in the background, each pass shown"), this);
    a_cancel = button(tr("Cancel"), tr("Stop: the best pass so far is kept"), this);
    a_cancel->setEnabled(false);
    runRow->addWidget(a_mode, 1);
    runRow->addWidget(a_run);
    runRow->addWidget(a_cancel);
    layout->addLayout(runRow);
    a_progress = new QProgressBar(this);
    a_progress->setTextVisible(false);
    a_progress->setMaximumHeight(8);
    layout->addWidget(a_progress);
    a_status = new QLabel(this);
    a_status->setWordWrap(true);
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(a_status);

    a_findings = new QTreeWidget(this);
    a_findings->setHeaderLabels({tr("Finding"), tr("Net"), tr("Where (µm)")});
    a_findings->setRootIsDecorated(false);
    a_findings->setToolTip(tr("What the check on the shapes drawn found: a click shows it"));
    a_findings->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    layout->addWidget(a_findings, 1);

    a_commit = button(tr("Commit…"), tr("Write the routes into the layout as a cell of their own, placed in the cell routed - replaced on the next commit"), this);
    layout->addWidget(a_commit);

    connect(browse, &QToolButton::clicked, this, &RouterPanel::chooseTechnology);
    connect(a_tech, &QLineEdit::editingFinished, this, [this] {
        if (!a_filling && a_tech->text() != a_session->job.technologyFile) setTechnology(a_tech->text());
    });
    connect(a_addNet, &QPushButton::clicked, this, [this] { a_making ? finishNet() : beginNet(); });
    connect(csv, &QPushButton::clicked, this, &RouterPanel::chooseCsv);
    connect(removeNets, &QPushButton::clicked, this, &RouterPanel::removeSelectedNets);
    connect(strokes, &QPushButton::clicked, this, [this] {
        const auto [keeps, guides] = takeStrokes();
        a_status->setText(tr("%1 keep-out(s) and %2 guide(s) from the pen").arg(keeps).arg(guides));
    });
    connect(removeKeep, &QPushButton::clicked, this, &RouterPanel::removeSelectedKeepOuts);
    connect(a_run, &QPushButton::clicked, this, [this] {
        const QString why = run(a_mode->currentData().toString());
        if (!why.isEmpty()) a_status->setText(why);
    });
    connect(a_cancel, &QPushButton::clicked, this, &RouterPanel::cancel);
    connect(a_findings, &QTreeWidget::itemClicked, this, &RouterPanel::goToFinding);
    connect(a_commit, &QPushButton::clicked, this, &RouterPanel::chooseCommit);
    connect(a_session, &Session::changedJob, this, [this] {
        fillNets();
        fillKeepOuts();
    });
    connect(a_session, &Session::passFinished, this, &RouterPanel::showPass);
    connect(a_session, &Session::runFinished, this, [this] {
        refresh();
        emit runShown();
    });
    if (LayoutView* view = doc->view()) {
        connect(view, &LayoutView::picked, this, [this](QPointF p) {
            if (a_making) addPoint(p);
        });
        connect(view, &LayoutView::cellRequested, this, [this] { QMetaObject::invokeMethod(this, &RouterPanel::updateOverlay, Qt::QueuedConnection); });
    }
    connect(doc, &LayoutDoc::reloaded, this, [this] {
        // The file read again: what was prepared is of the old one.
        a_session->layoutChanged();
        updateOverlay();
    });
    refresh();
}

RouterPanel::~RouterPanel()
{
    if (a_doc && a_doc->view()) {
        if (a_making) a_doc->view()->setMode(LayoutView::Mode::Select);
        a_doc->view()->setOverlays({});
    }
}

QString RouterPanel::routedCellName() const
{
    if (!a_session->job.cell.isEmpty()) return a_session->job.cell;
    if (!a_doc || !a_doc->layout()) return {};
    const int main = a_doc->layout()->mainCell();
    return main >= 0 ? a_doc->layout()->cells.at(main).name : QString();
}

void RouterPanel::refresh()
{
    a_filling = true;
    a_tech->setText(a_session->job.technologyFile.isEmpty() && !a_session->job.technology.isEmpty() ? tr("(given by Claude)")
                                                                                                    : a_session->job.technologyFile);
    a_filling = false;
    fillNets();
    fillKeepOuts();
    fillFindings();
    const bool running = a_session->isRunning();
    a_run->setEnabled(!running);
    a_cancel->setEnabled(running);
    a_commit->setEnabled(!running && a_session->hasResult());
    if (running) {
        a_progress->setRange(0, std::max(1, a_session->job.options.passes));
        const QVector<PassRow> passes = a_session->passesSoFar();
        if (!passes.isEmpty()) showPass(passes.last());
        else a_status->setText(tr("Routing…"));
    } else {
        a_progress->setRange(0, 1);
        a_progress->setValue(a_session->hasResult() ? 1 : 0);
        if (a_session->hasResult()) {
            const Result& r = a_session->result();
            const Verification& v = a_session->verification();
            int vias = 0;
            double length = 0;
            for (const NetResult& n : r.nets) {
                vias += n.vias.size();
                length += a_session->resultProblem()->um(n.length);
            }
            a_status->setText(tr("%1 routed, %2 failed, %3 vias, %4 mm in %5 s (%6, %7 passes). %8%9")
                                  .arg(r.routed())
                                  .arg(r.failed())
                                  .arg(vias)
                                  .arg(micro(length / 1000))
                                  .arg(micro(r.seconds))
                                  .arg(r.stopped)
                                  .arg(r.passes.size())
                                  .arg(v.clean() ? tr("Clean.") : tr("%1 finding(s).").arg(v.findings.size()))
                                  .arg(a_session->resultIsOld() ? tr(" The job has changed since.") : QString()));
        } else if (a_doc && a_doc->layout() && !a_session->job.nets.isEmpty() && a_session->prepared()) {
            int warned = 0;
            for (const TerminalReading& t : a_session->prepared()->terminals()) warned += !t.note.isEmpty();
            a_status->setText(tr("%1 net(s) ready.%2%3")
                                  .arg(a_session->job.nets.size())
                                  .arg(warned ? tr(" %1 terminal(s) cannot be left with their line.").arg(warned) : QString())
                                  .arg(a_session->prepared()->crowding().isEmpty() ? QString() : tr(" Crowded in places.")));
        } else if (a_session->job.nets.isEmpty()) {
            a_status->setText(tr("No nets yet: Add Net, or a CSV."));
        }
    }
    updateOverlay();
}

void RouterPanel::fillNets()
{
    a_filling = true;
    const Job& job = a_session->job;
    Technology tech;
    QString error;
    const bool haveTech = a_session->technology(&tech, &error);
    a_nets->setRowCount(int(job.nets.size()));
    const Problem* p = a_session->resultProblem();
    for (int i = 0; i < job.nets.size(); ++i) {
        const JobNet& n = job.nets.at(i);
        a_nets->setItem(i, 0, new QTableWidgetItem(n.name));
        auto* classes = new QComboBox(a_nets);
        if (haveTech)
            for (const NetClass& c : tech.classes) classes->addItem(QStringLiteral("%1 (%2 µm)").arg(c.name).arg(c.width), c.name);
        const int at = classes->findData(n.netClass.isEmpty() && haveTech ? tech.classes.at(tech.defaultClass).name : n.netClass);
        classes->setCurrentIndex(std::max(0, at));
        connect(classes, &QComboBox::currentIndexChanged, this, [this, name = n.name, classes] {
            if (a_filling) return;
            const int j = a_session->job.netIndex(name);
            if (j < 0) return;
            a_session->job.nets[j].netClass = classes->currentData().toString();
            saveAndPrepare();
        });
        a_nets->setCellWidget(i, 1, classes);
        a_nets->setItem(i, 2, new QTableWidgetItem(QString::number(std::max<qsizetype>(0, n.terminals.size() - 1))));
        QString status = tr("new");
        QString tip;
        bool failed = false;
        if (p && a_session->hasResult()) {
            const int k = a_session->resultNet(n.name);
            if (k >= 0) {
                const NetResult& r = a_session->result().nets.at(k);
                switch (r.status) {
                case NetResult::Status::Routed: status = tr("routed"); break;
                case NetResult::Status::Kept: status = tr("kept"); break;
                case NetResult::Status::Failed: status = tr("failed"), failed = true; break;
                case NetResult::Status::Fixed: status = tr("fixed"); break;
                default: status = tr("not run");
                }
                tip = r.reason;
            }
        }
        auto* s = new QTableWidgetItem(status);
        if (failed) s->setForeground(QColor(210, 40, 40));
        s->setToolTip(tip);
        a_nets->setItem(i, 3, s);
        a_nets->item(i, 0)->setToolTip(tip);
    }
    a_filling = false;
}

void RouterPanel::fillKeepOuts()
{
    a_keepOuts->clear();
    for (const JobKeepOut& k : a_session->job.keepOuts) {
        QString text = k.name;
        if (!k.metals.isEmpty()) text += QStringLiteral(" (%1)").arg(k.metals.join(QStringLiteral(", ")));
        a_keepOuts->addItem(text);
    }
}

void RouterPanel::fillFindings()
{
    a_findings->clear();
    if (!a_session->hasResult()) return;
    const Problem* p = a_session->resultProblem();
    int i = 0;
    for (const Finding& f : a_session->verification().findings) {
        auto* item = new QTreeWidgetItem(a_findings, {f.kind, f.net, QStringLiteral("%1, %2").arg(micro(p->um(f.at.x)), micro(p->um(f.at.y)))});
        item->setToolTip(0, f.text);
        item->setToolTip(1, f.text);
        item->setData(0, Qt::UserRole, i++);
        if (i >= 2000) break;
    }
}

void RouterPanel::updateOverlay()
{
    if (!a_doc || !a_doc->view()) return;
    LayoutView* view = a_doc->view();
    if (!a_session->hasResult() || a_doc->shownCellName() != routedCellName()) {
        view->setOverlays({});
        return;
    }
    const Problem& p = *a_session->resultProblem();
    const Result& r = a_session->result();
    QList<LayoutView::Overlay> overlays;
    QVector<LayoutView::Overlay> byMetal(p.tech.metals.size());
    for (int m = 0; m < p.tech.metals.size(); ++m) {
        byMetal[m].line = metalColour(m);
        QColor fill = metalColour(m);
        fill.setAlpha(110);
        byMetal[m].fill = fill;
        byMetal[m].width = 1;
        byMetal[m].name = tr("routes on %1").arg(p.tech.metals.at(m).name);
    }
    LayoutView::Overlay cuts;
    cuts.line = QColor(20, 20, 20);
    cuts.fill = QColor(250, 250, 250, 200);
    cuts.width = 1;
    cuts.name = tr("vias");
    const auto rect = [&](const Box& b) {
        return QPolygonF(QRectF(QPointF(p.um(b.x0), p.um(b.y0)), QPointF(p.um(b.x1), p.um(b.y1))));
    };
    LayoutView::Overlay failed;
    failed.line = QColor(220, 30, 30);
    failed.width = 2;
    failed.name = tr("nets not routed");
    for (int n = 0; n < r.nets.size() && n < p.nets.size(); ++n) {
        const NetResult& x = r.nets.at(n);
        if (x.status == NetResult::Status::Failed) {
            for (int t = 0; t < p.nets.at(n).terminals.size(); ++t) {
                const Pt at = p.nets.at(n).terminals.at(t).at;
                failed.labels.append({QPointF(p.um(at.x), p.um(at.y)), t == 0 ? tr("%1 failed").arg(p.nets.at(n).name) : tr("%1's pad").arg(p.nets.at(n).name), QPointF()});
            }
            // (Where it was stopped, when that is not its start.)
            const Pt start = p.nets.at(n).terminals.value(0).at;
            if (x.hasWhere && std::hypot(double(x.where.x - start.x), double(x.where.y - start.y)) > double(p.units(p.tech.grid)) * 4)
                failed.labels.append({QPointF(p.um(x.where.x), p.um(x.where.y)), tr("%1 stopped here").arg(p.nets.at(n).name), QPointF(1, 1)});
            continue;
        }
        if (x.status != NetResult::Status::Routed && x.status != NetResult::Status::Kept) continue;
        for (const DrawnShape& s : drawnShapes(p, x, n)) {
            int metal = -1;
            for (int m = 0; m < p.tech.metals.size(); ++m)
                if (p.tech.metals.at(m).layer == s.layer) metal = m;
            QPolygonF poly(s.points);
            if (metal >= 0) byMetal[metal].shapes.append(poly);
            else cuts.shapes.append(poly);
        }
    }
    for (const LayoutView::Overlay& o : byMetal) overlays.append(o);
    overlays.append(cuts);
    // The finding chosen, marked.
    if (QTreeWidgetItem* item = a_findings->currentItem()) {
        const int i = item->data(0, Qt::UserRole).toInt();
        const QVector<Finding>& fs = a_session->verification().findings;
        if (i >= 0 && i < fs.size()) {
            LayoutView::Overlay mark;
            mark.line = QColor(255, 0, 160);
            mark.width = 2;
            mark.name = fs.at(i).text;
            if (!fs.at(i).box.empty()) mark.shapes.append(rect(fs.at(i).box));
            mark.labels.append({QPointF(p.um(fs.at(i).at.x), p.um(fs.at(i).at.y)), fs.at(i).kind, QPointF(1, 1)});
            overlays.append(mark);
        }
    }
    overlays.append(failed);
    view->setOverlays(overlays);
}

void RouterPanel::saveAndPrepare()
{
    a_session->jobChanged();
    QString error;
    a_session->saveJob(&error);
    if (a_doc && a_doc->layout() && !a_session->job.nets.isEmpty()) {
        if (!a_session->prepare(a_doc->layout(), &error)) {
            a_status->setText(error);
            return;
        }
    }
    refresh();
}

void RouterPanel::beginNet()
{
    if (!a_doc || !a_doc->view()) return;
    a_making = true;
    a_points.clear();
    a_addNet->setText(tr("Finish"));
    a_doc->view()->setMode(LayoutView::Mode::PickPoint);
    a_status->setText(tr("Click the net's contact on the layout."));
}

void RouterPanel::addPoint(QPointF point)
{
    if (!a_making) return;
    a_points.append(point);
    a_status->setText(a_points.size() == 1 ? tr("Start at (%1, %2). Now click its pad, or pads; then Finish.").arg(micro(point.x()), micro(point.y()))
                                           : tr("%1 pad(s). Click another, or Finish.").arg(a_points.size() - 1));
}

void RouterPanel::finishNet()
{
    a_making = false;
    a_addNet->setText(tr("Add Net"));
    if (a_doc && a_doc->view()) a_doc->view()->setMode(LayoutView::Mode::Select);
    if (a_points.size() < 2) {
        a_status->setText(tr("A net needs its contact and a pad: none made."));
        return;
    }
    JobNet n;
    int k = int(a_session->job.nets.size()) + 1;
    do n.name = QStringLiteral("net%1").arg(k++);
    while (a_session->job.netIndex(n.name) >= 0);
    for (const QPointF& p : a_points) {
        JobTerminal t;
        t.at = p;
        t.hasPoint = true;
        n.terminals.append(t);
    }
    a_points.clear();
    a_session->job.nets.append(n);
    saveAndPrepare();
}

QPair<int, int> RouterPanel::takeStrokes()
{
    if (!a_doc || !a_doc->view()) return {0, 0};
    LayoutView* view = a_doc->view();
    const QList<markup::Stroke> strokes = view->drawing().on(view->drawingPart());
    int keeps = 0, guides = 0;
    const QStringList selected = selectedNets();
    for (int i = 0; i < strokes.size(); ++i) {
        const markup::Stroke& s = strokes.at(i);
        const markup::Gesture g = markup::gestureOf(s);
        if (g == markup::Gesture::Ring) {
            JobKeepOut k;
            k.name = tr("stroke %1").arg(s.id > 0 ? s.id : i + 1);
            k.outline = strokeOutline(s, false);
            if (k.outline.size() < 3) continue;
            a_session->job.keepOuts.removeIf([&](const JobKeepOut& x) { return x.name == k.name; });
            a_session->job.keepOuts.append(k);
            ++keeps;
        } else if (g == markup::Gesture::Line || g == markup::Gesture::Arrow || g == markup::Gesture::Mark) {
            const QVector<QPointF> guide = strokeGuide(s);
            if (guide.size() < 2) continue;
            // The net whose start it begins nearest (within a tenth of its
            // length, 20 µm at least), else the net selected.
            int best = -1;
            double length = 0, nearest = 1e300;
            for (int k = 1; k < guide.size(); ++k) length += std::hypot(guide[k].x() - guide[k - 1].x(), guide[k].y() - guide[k - 1].y());
            for (int n = 0; n < a_session->job.nets.size(); ++n) {
                const JobTerminal& start = a_session->job.nets.at(n).terminals.value(0);
                if (!start.hasPoint) continue;
                const double d = std::hypot(start.at.x() - guide.first().x(), start.at.y() - guide.first().y());
                if (d < nearest) nearest = d, best = n;
            }
            if (best < 0 || nearest > std::max(20.0, length / 10)) best = selected.size() == 1 ? a_session->job.netIndex(selected.first()) : -1;
            if (best < 0) continue;
            a_session->job.nets[best].guide = guide;
            ++guides;
        }
    }
    if (keeps || guides) saveAndPrepare();
    return {keeps, guides};
}

QString RouterPanel::importCsv(const QString& file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return tr("%1 cannot be read").arg(QDir::toNativeSeparators(file));
    QString error;
    const QVector<JobNet> nets = readNetsCsv(QString::fromUtf8(f.readAll()), &error);
    if (!error.isEmpty()) return QFileInfo(file).fileName() + QStringLiteral(": ") + error;
    for (const JobNet& n : nets) {
        const int i = a_session->job.netIndex(n.name);
        if (i >= 0) a_session->job.nets[i] = n;
        else a_session->job.nets.append(n);
    }
    saveAndPrepare();
    return {};
}

QString RouterPanel::setTechnology(const QString& file)
{
    const Job before = a_session->job;
    a_session->job.technologyFile = file;
    a_session->job.technology = {};
    Technology t;
    QString error;
    if (!a_session->technology(&t, &error)) {
        a_session->job = before;
        a_status->setText(error);
        refresh();
        return error;
    }
    saveAndPrepare();
    return {};
}

QString RouterPanel::run(const QString& mode)
{
    if (!a_doc || !a_doc->layout()) return tr("The layout is not read yet.");
    QString error;
    if ((!a_session->prepared() || a_session->layout() != a_doc->layout()) && !a_session->prepare(a_doc->layout(), &error)) return error;
    const QStringList which = mode == QLatin1String("selected") ? selectedNets() : QStringList();
    if (mode == QLatin1String("selected") && which.isEmpty()) return tr("No nets are selected.");
    if (!a_session->start(which, mode == QLatin1String("changed"), &error)) return error;
    a_progress->setRange(0, std::max(1, a_session->job.options.passes));
    a_progress->setValue(0);
    a_run->setEnabled(false);
    a_cancel->setEnabled(true);
    a_commit->setEnabled(false);
    a_status->setText(tr("Routing…"));
    return {};
}

void RouterPanel::cancel()
{
    a_session->cancel();
    a_status->setText(tr("Stopping: the best pass so far is kept…"));
}

void RouterPanel::showPass(const PassRow& row)
{
    a_progress->setValue(std::min(a_progress->maximum(), row.pass));
    a_status->setText(row.cleanup ? tr("Clean-up: %1 routed, %2 failed").arg(row.routed).arg(row.failed)
                      : row.partial ? tr("Pass %1, stopped part way: %2 contested; cleaning up…").arg(row.pass).arg(row.contested)
                                  : tr("Pass %1: %2 routed, %3 failed, %4 contested (%5 s)")
                                        .arg(row.pass)
                                        .arg(row.routed)
                                        .arg(row.failed)
                                        .arg(row.contested)
                                        .arg(micro(row.seconds)));
}

void RouterPanel::goToFinding(QTreeWidgetItem* item)
{
    if (!item || !a_doc || !a_session->hasResult()) return;
    const int i = item->data(0, Qt::UserRole).toInt();
    const QVector<Finding>& fs = a_session->verification().findings;
    if (i < 0 || i >= fs.size()) return;
    const Problem& p = *a_session->resultProblem();
    Box b = fs.at(i).box.empty() ? Box{fs.at(i).at.x, fs.at(i).at.y, fs.at(i).at.x, fs.at(i).at.y}.grown(p.units(p.tech.grid) * 20) : fs.at(i).box;
    b = b.grown(std::max(b.width(), b.height()) / 2 + p.units(p.tech.grid) * 4);
    if (a_doc->shownCellName() != routedCellName() && a_doc->layout()) a_doc->showCell(a_doc->layout()->cellIndex(routedCellName()), false);
    a_doc->view()->showRegion(QRectF(QPointF(p.um(b.x0), p.um(b.y0)), QPointF(p.um(b.x1), p.um(b.y1))).normalized());
    a_status->setText(fs.at(i).text);
    updateOverlay();
}

QString RouterPanel::commit(const QString& file, bool allowFindings)
{
    if (!a_session->hasResult()) return tr("Nothing is routed yet.");
    if (!a_session->verification().clean() && !allowFindings) return tr("The routes have findings.");
    Commit c;
    c.source = a_session->file();
    c.target = file;
    c.topCell = a_session->job.cell;
    c.cell = a_session->job.routeCell;
    const CommitReport report = commitRoutes(c, *a_session->resultProblem(), a_session->result());
    if (!report.ok) return report.error;
    a_status->setText(tr("%1 net(s), %2 shapes, written as the cell %3 in %4%5")
                          .arg(report.nets)
                          .arg(report.polygons)
                          .arg(c.cell, QFileInfo(file).fileName(), report.replaced ? tr(" (its earlier routes replaced)") : QString()));
    if (QFileInfo(file).absoluteFilePath() == QFileInfo(a_session->file()).absoluteFilePath()) {
        if (a_doc) a_doc->reload();
    } else {
        Session* copy = Session::of(file);
        copy->job = a_session->job;
        copy->jobChanged();
        QString e;
        copy->saveJob(&e);
    }
    return {};
}

void RouterPanel::chooseTechnology()
{
    const QString folder = QFileInfo(a_session->file()).absolutePath();
    const QString file = QFileDialog::getOpenFileName(this, tr("Technology"), folder, tr("Technology (*.json)"));
    if (file.isEmpty()) return;
    const QString relative = QDir(folder).relativeFilePath(file);
    setTechnology(relative.startsWith(QLatin1String("..")) ? file : relative);
}

void RouterPanel::chooseCsv()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Nets"), QFileInfo(a_session->file()).absolutePath(), tr("Nets (*.csv *.txt)"));
    if (file.isEmpty()) return;
    const QString why = importCsv(file);
    if (!why.isEmpty()) a_status->setText(why);
}

void RouterPanel::chooseCommit()
{
    if (!a_session->hasResult()) return;
    const Verification& v = a_session->verification();
    bool allow = false;
    if (!v.clean()) {
        if (QMessageBox::question(this, tr("Commit"),
                                  tr("The check found %1 thing(s), nets not routed among them. Commit the nets routed anyway?").arg(v.findings.size()))
            != QMessageBox::Yes)
            return;
        allow = true;
    }
    const QFileInfo src(a_session->file());
    const QString suggested = src.absolutePath() + QLatin1Char('/') + src.completeBaseName() + QStringLiteral("_routed.") + src.suffix();
    const QString file = QFileDialog::getSaveFileName(this, tr("Commit the Routes"), suggested, tr("Layouts (*.gds *.oas)"));
    if (file.isEmpty()) return;
    const QString why = commit(file, allow);
    if (!why.isEmpty()) QMessageBox::warning(this, tr("Commit"), why);
}

QStringList RouterPanel::selectedNets() const
{
    QStringList out;
    for (const QModelIndex& i : a_nets->selectionModel()->selectedRows())
        if (QTableWidgetItem* item = a_nets->item(i.row(), 0)) out << item->text();
    return out;
}

void RouterPanel::removeSelectedNets()
{
    const QStringList names = selectedNets();
    if (names.isEmpty()) return;
    a_session->job.nets.removeIf([&](const JobNet& n) { return names.contains(n.name); });
    saveAndPrepare();
}

void RouterPanel::removeSelectedKeepOuts()
{
    QList<int> rows;
    for (QListWidgetItem* item : a_keepOuts->selectedItems()) rows << a_keepOuts->row(item);
    if (rows.isEmpty()) return;
    std::sort(rows.begin(), rows.end(), std::greater<>());
    for (int r : rows)
        if (r < a_session->job.keepOuts.size()) a_session->job.keepOuts.removeAt(r);
    saveAndPrepare();
}

} // namespace qucs_s::routing
