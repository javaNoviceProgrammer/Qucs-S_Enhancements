/*
 * rf25doc.cpp - an RF 2.5D model in a tab of Qucs-S
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25doc.h"

#include "elidedlabel.h"
#include "main.h"
#include "misc.h"
#include "multiphysics/fem_expr.h"
#include "rf25/rf25_deembed.h"
#include "rf25/rf25_inductance.h"
#include "rf25/rf25_sparams.h"
#include "qucs.h"
#include "settings.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QRegularExpression>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPrinter>
#include <QProgressBar>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace qucs_s::rf25;
namespace fem = qucs_s::fem;
using qucs_s::layout::LayerStyle;
using qucs_s::layout::Layout;
using qucs_s::layout::LayoutView;

namespace {

/// How the study asks a full-wave solve to hold its system (its Matrix).
FullWaveControl::Matrix studyMatrix(const Model& model)
{
    const Node* study = model.root.child(QStringLiteral("rf_study"));
    const QString matrix = study != nullptr ? textOf(*study, QStringLiteral("matrix")) : QString();
    if (matrix == QLatin1String("dense")) return FullWaveControl::Matrix::Dense;
    if (matrix == QLatin1String("compressed")) return FullWaveControl::Matrix::Compressed;
    return FullWaveControl::Matrix::Automatic;
}

/// Whether the model's mesh is for a full-wave solve held compressed - the
/// study's Solver full wave, and its Matrix not dense: then the mesh may
/// have as many cells as that holds (MeshSettings::compressed).
bool meshedCompressed(const Model& model)
{
    const Node* study = model.root.child(QStringLiteral("rf_study"));
    return study != nullptr && textOf(*study, QStringLiteral("kind")) == QLatin1String("full_wave")
           && studyMatrix(model) != FullWaveControl::Matrix::Dense;
}

/// How the tab meshes \a model: its Mesh node's settings, held compressed as
/// its study is, and narrow gaps graded - the structure and, for a full-wave
/// study, the strips of its ports' feeds alike (calibrate()). For both
/// studies: a quasi-static one's capacitance is in the charge crowding on
/// both edges of a gap (a coplanar line's 6 µm gaps on four cells across
/// left it 10 % short), and a full-wave one's loss and impedance too in the
/// current crowding there (a measured coplanar line on alumina lost 19 %
/// under its 2D eddy-current solve on the edges' one halving, its impedance
/// 17 % over; 1 % under to 10 % over on the graded edges, its impedance 6 %
/// over).
MeshSettings tabMeshSettings(const Model& model, QStringList* said)
{
    MeshSettings how = meshSettingsOf(model.mesh(), model.parameters(), said);
    how.compressed = meshedCompressed(model);
    how.gaps = true;
    return how;
}

const char* const kShowProcessedKey = "RF25/showProcessed";
const char* const kShowMeshKey = "RF25/showMesh";
const char* const kShowCurrentsKey = "RF25/showCurrents";
const char* const kShowPortsKey = "RF25/showPorts";

/// The current density's colours, dark to bright: viridis, which reads in
/// order in either theme and to a reader who sees no red - its eight
/// anchors, and \a bands colours between them.
QList<QColor> currentRamp(int bands = 8)
{
    static const QList<QColor> anchors{QColor(0x44, 0x01, 0x54), QColor(0x46, 0x32, 0x7e), QColor(0x36, 0x5c, 0x8d),
                                       QColor(0x27, 0x7f, 0x8e), QColor(0x1f, 0xa1, 0x87), QColor(0x4a, 0xc1, 0x6d),
                                       QColor(0xa0, 0xda, 0x39), QColor(0xfd, 0xe7, 0x25)};
    QList<QColor> out;
    for (int b = 0; b < bands; ++b) {
        const double t = bands > 1 ? double(b) / (bands - 1) * (anchors.size() - 1) : 0.0;
        const int i = std::min(int(t), int(anchors.size()) - 2);
        const double u = t - i;
        const QColor& a = anchors.at(i);
        const QColor& c = anchors.at(i + 1);
        out << QColor(int(std::lround(a.red() + u * (c.red() - a.red()))), int(std::lround(a.green() + u * (c.green() - a.green()))),
                      int(std::lround(a.blue() + u * (c.blue() - a.blue()))), 235);
    }
    return out;
}

/// The model's ports as the full-wave solve takes them: an edge port on its
/// metal, facing the way it says; an internal port from the metal it names
/// (or the ground) to the other; each with its own reference and its
/// de-embedding. One that cannot be read is \a problem, and nothing is
/// returned.
/// The model's Return Conductors as the solvers take them: each one's point,
/// in the layout's units, on its metal. One whose point is no number is
/// \a problem, and nothing is returned.
QList<ReturnMark> returnMarksOf(const Model& model, QString* problem)
{
    QList<ReturnMark> out;
    for (const Node& node : model.ports().children) {
        if (!node.enabled || node.type != QLatin1String("rf_return")) continue;
        bool okX = true, okY = true;
        const double x = numberOf(node, QStringLiteral("x"), model, 0, &okX);
        const double y = numberOf(node, QStringLiteral("y"), model, 0, &okY);
        if (!okX || !okY) {
            *problem = QCoreApplication::translate("RF25Doc", "%1's %2 is not a number.")
                           .arg(node.label.isEmpty() ? node.tag : node.label, !okX ? QStringLiteral("x") : QStringLiteral("y"));
            return {};
        }
        out << ReturnMark{QPointF(x, y) / 1e-6, textOf(node, QStringLiteral("conductor")).trimmed()};
    }
    return out;
}

QList<FullWavePort> portsOf(const Model& model, QString* problem)
{
    QList<FullWavePort> out;
    for (const Node& port : model.ports().children) {
        if (!port.enabled) continue;
        const bool edge = port.type == QLatin1String("rf_edge_port");
        if (!edge && port.type != QLatin1String("rf_internal_port")) continue;
        FullWavePort p;
        p.name = port.label.isEmpty() ? port.tag : port.label;
        bool okX = true, okY = true, okZ = true;
        const double x = numberOf(port, QStringLiteral("x"), model, 0, &okX);
        const double y = numberOf(port, QStringLiteral("y"), model, 0, &okY);
        p.impedance = numberOf(port, QStringLiteral("impedance"), model, 50, &okZ);
        if (!okX || !okY || !okZ) {
            *problem = QCoreApplication::translate("RF25Doc", "%1's %2 is not a number.")
                           .arg(p.name, !okX ? QStringLiteral("x") : !okY ? QStringLiteral("y")
                                                                           : QCoreApplication::translate("RF25Doc", "reference impedance"));
            return {};
        }
        // (The ports are in metres; the cells are in the layout's own.)
        p.at = QPointF(x, y) / 1e-6;
        p.pair = textOf(port, QStringLiteral("pair")).trimmed();
        if (edge) {
            p.metal = textOf(port, QStringLiteral("conductor")).trimmed();
            const QString facing = textOf(port, QStringLiteral("facing"));
            if (facing == QLatin1String("+x")) p.facing = QPointF(1, 0);
            else if (facing == QLatin1String("-x")) p.facing = QPointF(-1, 0);
            else if (facing == QLatin1String("+y")) p.facing = QPointF(0, 1);
            else if (facing == QLatin1String("-y")) p.facing = QPointF(0, -1);
            // Its own discontinuity taken out after the solve, by strips of
            // its feed, and its reference plane moved along the line - asked
            // for, or implied by a move.
            bool ok = true;
            p.shift = numberOf(port, QStringLiteral("deembed"), model, 0, &ok);
            if (!ok) {
                *problem = QCoreApplication::translate("RF25Doc", "%1's de-embedding is not a number.").arg(p.name);
                return {};
            }
            p.calibrate = flagOf(port, QStringLiteral("calibrate"));
            bool okStrips = true;
            p.strips = numberOf(port, QStringLiteral("strips"), model, 0, &okStrips);
            if (!okStrips || !(p.strips >= 0)) {
                *problem = QCoreApplication::translate("RF25Doc", "%1's calibration strips are no length: nought, to have "
                                                                  "them chosen, or above.")
                               .arg(p.name);
                return {};
            }
            p.coplanar = textOf(port, QStringLiteral("return")) == QLatin1String("beside");
            if (textOf(port, QStringLiteral("along")) == QLatin1String("line")) {
                bool okZ = true, okE = true;
                p.lineImpedance = numberOf(port, QStringLiteral("line_z0"), model, 50, &okZ);
                p.lineEps = numberOf(port, QStringLiteral("line_eps"), model, 1, &okE);
                if (!okZ || !okE || !(p.lineImpedance > 0) || !(p.lineEps >= 1)) {
                    *problem = QCoreApplication::translate("RF25Doc", "The line %1's reference plane is to move along is "
                                                                      "none: its impedance above nought and its effective "
                                                                      "permittivity at least one.")
                                   .arg(p.name);
                    return {};
                }
            }
        } else {
            p.internal = true;
            p.metal = textOf(port, QStringLiteral("to")).trimmed();
            // Over a rectangle about its point, where it has one.
            bool okW = true, okH = true;
            const double w = numberOf(port, QStringLiteral("width"), model, 0, &okW);
            const double h = numberOf(port, QStringLiteral("height"), model, 0, &okH);
            if (!okW || !okH || w < 0 || h < 0) {
                *problem = QCoreApplication::translate("RF25Doc", "%1's rectangle is none: its width and height are "
                                                                  "lengths of nought or more.")
                               .arg(p.name);
                return {};
            }
            p.size = QSizeF(w, h) / 1e-6;
            const QString from = textOf(port, QStringLiteral("from")).trimmed();
            p.from = from.compare(QLatin1String("ground"), Qt::CaseInsensitive) == 0 ? QString() : from;
        }
        out << p;
    }
    return out;
}

/// A change of the model: the model before and after, whole (it is small).
class ModelCommand : public QUndoCommand
{
public:
    ModelCommand(std::function<void(const Model&)> apply, Model before, Model after, const QString& what)
        : QUndoCommand(what), a_apply(std::move(apply)), a_before(std::move(before)), a_after(std::move(after))
    {}
    void undo() override { a_apply(a_before); }
    void redo() override
    {
        // (The first time, it is applied already.)
        if (a_first) a_first = false;
        else a_apply(a_after);
    }

private:
    std::function<void(const Model&)> a_apply;
    Model a_before, a_after;
    bool a_first = true;
};

} // namespace

// ------------------------------------------------------------------ The stack

namespace qucs_s::rf25 {

StackView::StackView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("rfStackView"));
    setMinimumWidth(90);
    setToolTip(tr("The stack from the bottom up, each layer as tall as it is thick. A click chooses it."));
}

void StackView::setStack(const Stack& stack)
{
    a_stack = stack;
    update();
}

void StackView::setFocused(const QString& tag)
{
    if (a_focus == tag) return;
    a_focus = tag;
    update();
}

QList<QPair<QString, QRectF>> StackView::places() const
{
    QList<QPair<QString, QRectF>> out;
    if (a_stack.layers.isEmpty()) return out;
    const double top = 6, bottom = height() - 6.0, left = 6, right = width() - 6.0;
    const double room = std::max(20.0, bottom - top);
    // The thick layers share the room by thickness, with a floor under
    // each: a 0.6 µm metal beside a 300 µm substrate would be a hair
    // otherwise. The planes and the vias take no room of their own.
    double total = 0;
    int thick = 0;
    for (const Layer& l : a_stack.layers)
        if (l.z1 > l.z0) {
            total += l.z1 - l.z0;
            ++thick;
        }
    const double floorEach = thick > 0 ? std::min(14.0, room / (thick * 2.0)) : 0;
    const double share = std::max(0.0, room - floorEach * thick);
    double y = bottom;
    for (const Layer& l : a_stack.layers) {
        if (l.z1 <= l.z0) continue;
        const double h = floorEach + (total > 0 ? share * (l.z1 - l.z0) / total : 0);
        out.append({l.tag, QRectF(left, y - h, std::max(10.0, right - left), h)});
        y -= h;
    }
    // A ground plane under the stack, a cover over it where they have no
    // thickness, and the vias at the right edge over the room between
    // their metals.
    for (const Layer& l : a_stack.layers) {
        if (l.z1 > l.z0) continue;
        if (l.kind == Layer::Via) {
            const Layer* from = a_stack.named(l.from);
            const Layer* to = a_stack.named(l.to);
            double y0 = l.z0, y1 = l.z0;
            if (from != nullptr && to != nullptr) {
                y0 = std::min(from->z1, to->z1);
                y1 = std::max(from->z0, to->z0);
            }
            // (The z of the layers, into pixels: through the thick layers' places.)
            const auto pixelOf = [&out, this](double z) {
                for (const auto& [tag, place] : out) {
                    const Layer* layer = nullptr;
                    for (const Layer& x : a_stack.layers)
                        if (x.tag == tag) layer = &x;
                    if (layer == nullptr || !(layer->z1 > layer->z0)) continue;
                    if (z >= layer->z0 && z <= layer->z1) {
                        const double part = (z - layer->z0) / (layer->z1 - layer->z0);
                        return place.bottom() - part * place.height();
                    }
                }
                return out.isEmpty() ? 0.0 : out.last().second.top();
            };
            const double a = pixelOf(std::min(y0, y1)), b = pixelOf(std::max(y0, y1));
            out.append({l.tag, QRectF(right - 16, std::min(a, b), 10, std::max(6.0, std::abs(b - a)))});
        } else if (l.kind == Layer::Ground) {
            out.append({l.tag, QRectF(left, bottom, std::max(10.0, right - left), 5)});
        } else {
            out.append({l.tag, QRectF(left, std::max(top - 5, 0.0), std::max(10.0, right - left), 5)});
        }
    }
    return out;
}

void StackView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().color(QPalette::Base));
    if (a_stack.layers.isEmpty()) {
        p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        p.drawText(rect().adjusted(6, 6, -6, -6), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, tr("No layer"));
        return;
    }
    QFont small = QApplication::font();
    small.setPixelSize(std::max(8, small.pixelSize() > 0 ? small.pixelSize() - 2 : 10));
    p.setFont(small);
    const QList<QPair<QString, QRectF>> drawn = places();
    for (const auto& [tag, place] : drawn) {
        int index = 0;
        const Layer* layer = nullptr;
        for (int i = 0; i < a_stack.layers.size(); ++i)
            if (a_stack.layers.at(i).tag == tag) {
                layer = &a_stack.layers.at(i);
                index = i;
            }
        if (layer == nullptr) continue;
        const QColor colour = RF25Doc::layerColor(index);
        p.setBrush(layer->isConductor() ? colour : QColor(colour.red(), colour.green(), colour.blue(), 90));
        p.setPen(QPen(colour.darker(140), tag == a_focus ? 2.0 : 1.0));
        p.drawRect(place);
        if (place.height() >= 11 && place.width() > 30) {
            p.setPen(palette().color(QPalette::Text));
            p.drawText(place.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(layer->name, Qt::ElideRight, int(place.width()) - 8));
        }
    }
}

void StackView::mousePressEvent(QMouseEvent* event)
{
    for (const auto& [tag, place] : places())
        if (place.contains(event->position())) {
            emit layerChosen(tag);
            return;
        }
    QWidget::mousePressEvent(event);
}

} // namespace qucs_s::rf25

// ------------------------------------------------------------------ The tab

/// The layout file being read: on a thread of its own, as a layout's tab
/// reads it, so that a large GDSII file never holds the window.
struct RF25Doc::Job {
    QString path;
    std::shared_ptr<Layout> result;
    QString error;
    std::atomic_bool cancelled{false};
    qucs_s::layout::Progress progress{-1};
};

/// What the solver would be given, made on a thread of its own: the
/// layer map applied and the processing run over the layout as read, and
/// those shapes divided into cells. The model is copied and the layout
/// held, so that an edit while it runs changes neither under it.
struct RF25Doc::PrepareJob {
    Model model;
    std::shared_ptr<const Layout> layout;
    QString called;
    std::shared_ptr<Processed> processed;
    std::shared_ptr<Mesh> mesh;
};

/// A solve on a thread of its own: the shapes and the cells as they
/// stand, and the stack read out of the model, all copied so that an edit
/// while it runs changes nothing under it.
struct RF25Doc::SolveJob {
    // What it solves, stamped as it starts - the model's JSON and the
    // layout file's bytes - for the solution kept beside the model.
    QByteArray modelStamp, layoutStamp;
    Mesh mesh;
    QList<Conductor> conductors;
    Stack stack;
    QList<ReturnMark> returns;   ///< the Return Conductors under Ports
    double length = 0;
    std::shared_ptr<Extraction> result;
    // The loop's inductance, where two ports say what the loop is: the
    // magnetic half of the extraction, solved on the same thread and the
    // same copies as the charge.
    bool loop = false;
    QPointF in, out;
    double frequency = 0;
    QString portsSaid;
    std::shared_ptr<Inductance> inductance;
    // A full-wave solve's: the ports as the model has them, the sweep, and
    // what reading the ports said; and the flag that stops it between
    // frequencies, with the count it keeps of them.
    bool fullWave = false;
    QList<FullWavePort> ports;
    QList<double> frequencies;
    QStringList notes;
    std::shared_ptr<FullWave> wave;
    std::atomic_bool stop{false};
    std::atomic_int done{0};
    // The ports' de-embedding after it: the mesh's settings, so that the
    // strips of their feeds are meshed as the structure was, and whether
    // those strips are being solved; and how far they have got, for the
    // bar - the calibrations to make and begun, and of the one going on the
    // strips begun, the frequencies they have and how many are solved.
    MeshSettings how;
    double top = 0;
    std::atomic_bool calibrating{false};
    std::atomic_int calibrations{0}, calibrationsBegun{0}, stripsBegun{0}, stripFrequencies{0}, calibrated{0};
    FullWaveControl::Matrix matrix = FullWaveControl::Matrix::Automatic;   ///< the study's
    // A quasi-static extraction's: its step, how far into it, and the images
    // of the Green's function it built last (ExtractControl).
    std::atomic_int step{ExtractControl::Starting}, stepDone{0}, stepAll{0}, images{0};

    // The tab's holdSolves() and allowFrequencies(), which a test holds it
    // back by.
    std::shared_ptr<std::atomic_bool> held;
    std::shared_ptr<std::atomic_int> allowed;
};

RF25Doc::RF25Doc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    setObjectName(QStringLiteral("RF25Doc"));
    a_model = starterModel();
    a_undo = new QUndoStack(this);
    connect(a_undo, &QUndoStack::cleanChanged, this, [this](bool clean) {
        setDocChanged(!clean);
        emit signalFileChanged(!clean);
    });
    const auto front = [this] {
        return a_App != nullptr && a_App->DocumentTab != nullptr && a_App->DocumentTab->currentWidget() == this;
    };
    connect(a_undo, &QUndoStack::canUndoChanged, this, [this, front](bool can) {
        if (front()) a_App->undo->setEnabled(can);
    });
    connect(a_undo, &QUndoStack::canRedoChanged, this, [this, front](bool can) {
        if (front()) a_App->redo->setEnabled(can);
    });
    a_prepareSoon = new QTimer(this);
    a_prepareSoon->setSingleShot(true);
    a_prepareSoon->setInterval(300);
    connect(a_prepareSoon, &QTimer::timeout, this, &RF25Doc::prepare);
    // A solve's bar while it runs, and a full-wave solve's frequencies
    // counted for the notes and the panel: a sweep of many is minutes.
    a_progress = new QTimer(this);
    a_progress->setInterval(400);
    connect(a_progress, &QTimer::timeout, this, [this] {
        showProgress();
        // (A quasi-static solve's too, now that it says what it is doing.)
        if (!a_solveJob) return;
        showProblems();
        emit solvedChanged();
    });
    buildUi();
    // The starter model's stack, drawn from the first: only an edit or a
    // read drew it, so a new model's view said "No layer" beside a status
    // line counting three (the phase A check of 24 October).
    a_stack->setStack(a_model.stack());
    showProblems();
    if (app != nullptr) connect(this, SIGNAL(signalFileChanged(bool)), app, SLOT(slotFileChanged(bool)));
}

RF25Doc::~RF25Doc()
{
    // Its undo stack goes with it, emitting its clean and can-undo changes:
    // not to this, half gone (a cast UBSan calls undefined), nor to the
    // window, which may be going too.
    disconnect(a_undo, nullptr, this, nullptr);
    // The read going on is let go: it ends at its next step and drops its
    // result, which nothing holds any more.
    if (a_job) a_job->cancelled = true;
    // The same for the shapes and the mesh being made: the job holds its
    // own copy of the model and of the layout, so it ends on nothing this
    // tab owns.
    a_prepareJob.reset();
    // And a full-wave sweep stops at its next frequency rather than run on
    // for minutes for nothing.
    if (a_solveJob) a_solveJob->stop = true;
}

Model RF25Doc::starterModel()
{
    return Model::blank();
}

QColor RF25Doc::layerColor(int index)
{
    return qucs_s::layout::paletteStyle(std::max(0, index)).fill;
}

QColor RF25Doc::outlineColor(const QColor& fill) const
{
    // Darker on a light canvas, lighter on a dark one: either way a line
    // told from the fill of that very colour under it.
    return a_view != nullptr && a_view->colors().background.lightness() < 128 ? fill.lighter(180) : fill.darker(220);
}

void RF25Doc::buildUi()
{
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    all->setSpacing(0);

    auto* bar = new QWidget(this);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(6, 4, 6, 4);
    row->setSpacing(6);
    const auto button = [&](const QString& text, const QString& tip, const char* name) {
        auto* b = new QToolButton(bar);
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        row->addWidget(b);
        return b;
    };
    a_fit = button(tr("Zoom Extents"), tr("Show the whole layout"), "rfFit");
    connect(a_fit, &QToolButton::clicked, this, [this] { a_view->fit(); });
    a_reread = button(tr("Read the Layout Again"), tr("Read the layout file the model names once more"), "rfReread");
    connect(a_reread, &QToolButton::clicked, this, &RF25Doc::refreshLayout);
    row->addSpacing(10);
    // What the solver would be given, over the layout it came from.
    const QucsSettingsFile settings;
    a_showProcessed = settings.value(QLatin1String(kShowProcessedKey), true).toBool();
    a_showMesh = settings.value(QLatin1String(kShowMeshKey), false).toBool();
    a_showProcessedButton = button(tr("Processed Shapes"),
                                   tr("Draw the shapes as the processing leaves them over the layout they came from: "
                                      "what the mesher is given"),
                                   "rfShowProcessed");
    a_showProcessedButton->setCheckable(true);
    a_showProcessedButton->setChecked(a_showProcessed);
    connect(a_showProcessedButton, &QToolButton::toggled, this, &RF25Doc::setProcessedShown);
    a_showMeshButton = button(tr("Mesh"), tr("Draw the cells the shapes are divided into"), "rfShowMesh");
    a_showMeshButton->setCheckable(true);
    a_showMeshButton->setChecked(a_showMesh);
    connect(a_showMeshButton, &QToolButton::toggled, this, &RF25Doc::setMeshShown);
    a_showCurrents = settings.value(QLatin1String(kShowCurrentsKey), false).toBool();
    a_showCurrentsButton = button(tr("Currents"),
                                  tr("Draw the surface current density a full-wave solve found, over the layout: of one "
                                     "port driven alone at one of its frequencies (the Results node chooses which)"),
                                  "rfShowCurrents");
    a_showCurrentsButton->setCheckable(true);
    a_showCurrentsButton->setChecked(a_showCurrents);
    connect(a_showCurrentsButton, &QToolButton::toggled, this, &RF25Doc::setCurrentsShown);
    a_showPorts = settings.value(QLatin1String(kShowPortsKey), true).toBool();
    a_showPortsButton = button(tr("Ports"),
                               tr("Draw the ports over the layout, each by its name: the edge an edge port drives, with "
                                  "an arrow out of it, a coplanar one's grounds beside it, an internal port's rectangle"),
                               "rfShowPorts");
    a_showPortsButton->setCheckable(true);
    a_showPortsButton->setChecked(a_showPorts);
    connect(a_showPortsButton, &QToolButton::toggled, this, &RF25Doc::setPortsShown);
    row->addSpacing(10);
    // Ports placed by hand on the layout.
    a_edgePortButton = button(tr("Edge Port"),
                              tr("Put an edge port on an edge of a metal's shape: click on the edge (Escape to stop)"),
                              "rfPlaceEdgePort");
    a_edgePortButton->setCheckable(true);
    connect(a_edgePortButton, &QToolButton::toggled, this, [this](bool on) {
        if (on) setPlacing(Placing::EdgePort);
        else if (a_placing == Placing::EdgePort) setPlacing(Placing::None);
    });
    a_internalPortButton = button(tr("Internal Port"),
                                  tr("Put an internal port on a metal: drag a rectangle over the cells it is to drive, or "
                                     "click for the one cell under the point (Escape to stop)"),
                                  "rfPlaceInternalPort");
    a_internalPortButton->setCheckable(true);
    connect(a_internalPortButton, &QToolButton::toggled, this, [this](bool on) {
        if (on) setPlacing(Placing::InternalPort);
        else if (a_placing == Placing::InternalPort) setPlacing(Placing::None);
    });
    row->addSpacing(10);
    // The one piece of work this build does on being asked rather than at
    // every edit: a dense matrix of the unknowns is cubic.
    a_solveButton = button(tr("Solve"),
                           tr("Solve the model by the study's solver: the quasi-static extraction (the conductors' "
                              "capacitance, a line's RLGC) or the full-wave one (the ports' S at each frequency)"),
                           "rfSolve");
    connect(a_solveButton, &QToolButton::clicked, this, [this] {
        if (isSolving()) stopSolving();
        else solve();
    });
    row->addSpacing(10);
    a_layoutLabel = new ElidedLabel(bar);
    a_layoutLabel->setObjectName(QStringLiteral("rfLayoutLabel"));
    a_layoutLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(a_layoutLabel, 1);
    all->addWidget(bar);

    a_splitter = new QSplitter(Qt::Vertical, this);
    a_splitter->setObjectName(QStringLiteral("rfSplitter"));
    a_sides = new QSplitter(Qt::Horizontal, a_splitter);
    a_sides->setObjectName(QStringLiteral("rfSides"));
    a_stack = new StackView(a_sides);
    a_view = new LayoutView(a_sides);
    a_view->setObjectName(QStringLiteral("rfView"));
    connect(a_stack, &StackView::layerChosen, this, &RF25Doc::focusNode);
    connect(a_view, &LayoutView::pointerMoved, this, &RF25Doc::pointerMoved);
    // A port placed where the pointer picks, by the tool in hand; the view
    // back in its own mode (Escape) puts the tool down.
    connect(a_view, &LayoutView::picked, this, [this](QPointF at) {
        QString why;
        const QString tag = a_placing == Placing::EdgePort       ? placeEdgePortAt(at, 0, &why)
                            : a_placing == Placing::InternalPort ? placeInternalPort(QRectF(at, QSizeF(0, 0)), &why)
                                                                 : QString();
        if (tag.isEmpty() && !why.isEmpty()) a_status->setText(why);
    });
    connect(a_view, &LayoutView::boxPicked, this, [this](QRectF box) {
        if (a_placing != Placing::InternalPort) return;
        QString why;
        if (placeInternalPort(box, &why).isEmpty() && !why.isEmpty()) a_status->setText(why);
    });
    connect(a_view, &LayoutView::modeChanged, this, [this](LayoutView::Mode mode) {
        if (mode == LayoutView::Mode::Select && a_placing != Placing::None) setPlacing(Placing::None);
    });
    // The canvas went light or dark: the outlines and the cells' ink are
    // told against it, so they are taken again.
    connect(a_view, &LayoutView::colorsChanged, this, &RF25Doc::showOverlays);
    a_sides->addWidget(a_stack);
    a_sides->addWidget(a_view);
    a_sides->setStretchFactor(0, 0);
    a_sides->setStretchFactor(1, 1);
    a_sides->setSizes({150, 850});
    a_problemsText = new QPlainTextEdit(a_splitter);
    a_problemsText->setObjectName(QStringLiteral("rfProblems"));
    a_problemsText->setReadOnly(true);
    a_problemsText->setMaximumBlockCount(400);
    a_splitter->addWidget(a_sides);
    a_splitter->addWidget(a_problemsText);
    a_splitter->setStretchFactor(0, 5);
    a_splitter->setStretchFactor(1, 1);
    a_splitter->setSizes({560, 150});
    all->addWidget(a_splitter, 1);

    // The status line, and at its end how far a solve has got. The bar is
    // of a fixed width and the line gives way to it, so that the tab's
    // least width - the bar of buttons above's - is what it was, and no
    // taller than the line, so that nothing above it moves.
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 0, 6, 0);
    statusRow->setSpacing(6);
    a_status = new ElidedLabel(this);
    a_status->setObjectName(QStringLiteral("rfStatus"));
    a_status->setContentsMargins(8, 2, 8, 2);
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusRow->addWidget(a_status, 1);
    a_progressBar = new QProgressBar(this);
    a_progressBar->setObjectName(QStringLiteral("rfProgress"));
    a_progressBar->setFixedWidth(160);
    a_progressBar->setMaximumHeight(a_status->fontMetrics().height() + 4);
    a_progressBar->hide();
    statusRow->addWidget(a_progressBar);
    // Its Stop beside it, as the multiphysics tab's row has its Cancel - the
    // Solve button's Stop, where the bar is - and no taller than the line.
    a_progressStop = new QToolButton(this);
    a_progressStop->setObjectName(QStringLiteral("rfProgressStop"));
    a_progressStop->setText(tr("Stop"));
    a_progressStop->setToolTip(tr("Stop the solve, as the Solve button's Stop does"));
    a_progressStop->setAutoRaise(true);
    a_progressStop->setFixedHeight(a_status->fontMetrics().height() + 4);
    a_progressStop->hide();
    connect(a_progressStop, &QToolButton::clicked, this, [this] {
        if (isSolving()) stopSolving();
    });
    statusRow->addWidget(a_progressStop);
    all->addLayout(statusRow);
}

// ------------------------------------------------------------------ QucsDoc

void RF25Doc::setName(const QString& name)
{
    a_DocName = name;
    emit stateChanged();
}

bool RF25Doc::load()
{
    // The file as it is now, taken before it is read: a file written again
    // while this read goes on must not be stamped as the one held (the
    // rule of 24 October, qucsdoc.h).
    const QFileInfo asRead(a_DocName);
    QFile file(a_DocName);
    if (!file.open(QIODevice::ReadOnly)) {
        a_readNotes = file.errorString();
        misc::reportError(tr("Cannot read %1:\n%2").arg(a_DocName, file.errorString()));
        return false;
    }
    QString why;
    const std::optional<Model> m = Model::fromJson(file.readAll(), &why);
    if (!m) {
        a_readNotes = why;
        misc::reportError(tr("Cannot read %1:\n%2").arg(a_DocName, why));
        return false;
    }
    a_readNotes = why;
    a_undo->clear();
    applyModel(*m, true);
    a_undo->setClean();
    setDocChanged(false);
    stampedAs(asRead);
    readKeptSolution();
    refreshLayout();
    return true;
}

bool RF25Doc::reload()
{
    const QFileInfo asRead(a_DocName);
    QFile file(a_DocName);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QString why;
    const std::optional<Model> m = Model::fromJson(file.readAll(), &why);
    if (!m) return false;
    a_readNotes = why;
    const QString focus = a_focus;
    a_undo->clear();   // (its steps were of the model as it was)
    applyModel(*m, true);
    setDocChanged(false);
    emit signalFileChanged(false);
    stampedAs(asRead);
    if (!focus.isEmpty() && a_model.find(focus) != nullptr) focusNode(focus);
    readKeptSolution();
    refreshLayout();
    return true;
}

bool RF25Doc::writeTo(const QString& path)
{
    const QByteArray bytes = a_model.toJson();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

int RF25Doc::save()
{
    if (!writeTo(a_DocName)) {
        misc::reportError(tr("Cannot write %1").arg(a_DocName));
        return -1;
    }
    a_undo->setClean();
    setDocChanged(false);
    emit signalFileChanged(false);
    // The solution shown, where it is of the model as saved, beside it.
    keepSolution();
    // The date and size the file itself has, not the clock's reading of
    // this moment: the two differ by a little, and changedOnDisk() would
    // then take the file this just wrote for another program's.
    stampedFromFile();
    // A layout named beside the model is another file now that the model
    // has moved (Save As), so it is read again.
    refreshLayout();
    emit stateChanged();
    return 0;
}

void RF25Doc::print(QPrinter*, QPainter* painter, bool, bool)
{
    const QImage image = a_view->picture();
    if (image.isNull()) return;
    const QRect page = painter->viewport();
    const QSize fitted = image.size().scaled(page.size(), Qt::KeepAspectRatio);
    painter->drawImage(QRect(page.topLeft(), fitted), image);
}

void RF25Doc::becomeCurrent(bool)
{
    if (a_App != nullptr) {
        a_App->undo->setEnabled(a_undo->canUndo());
        a_App->redo->setEnabled(a_undo->canRedo());
    }
}

double RF25Doc::zoomBy(double factor)
{
    a_view->zoomBy(factor);
    return a_view->scale();
}

void RF25Doc::showAll()
{
    a_view->fit();
}

void RF25Doc::zoomToSelection()
{
    a_view->fit();
}

void RF25Doc::showNoZoom()
{
    a_view->fit();
}

void RF25Doc::undo()
{
    a_undo->undo();
}

void RF25Doc::redo()
{
    a_undo->redo();
}

// ------------------------------------------------------------------ The model

bool RF25Doc::setModel(const Model& model, const QString& what)
{
    if (model.toObject() == a_model.toObject()) return false;
    const Model before = a_model;
    applyModel(model);
    QPointer<RF25Doc> self = this;
    a_undo->push(new ModelCommand([self](const Model& m) {
        if (self) self->applyModel(m);
    }, before, model, what));
    return true;
}

bool RF25Doc::editNode(const QString& tag, const std::function<void(Node&)>& change, const QString& what, QString* error)
{
    Model m = a_model;
    Node* n = m.find(tag);
    if (n == nullptr) {
        if (error != nullptr) *error = tr("there is no node %1").arg(tag);
        return false;
    }
    change(*n);
    setModel(m, what);
    return true;
}

void RF25Doc::applyModel(const Model& model, bool read)
{
    const QString layoutBefore = layoutFile();
    a_model = model;
    a_lastChangeRead = read;
    if (read) contentRead();
    else edited();
    a_stack->setStack(a_model.stack());
    showProblems();
    // (A port added, moved or taken away is drawn at once, before any
    //  preparing it may bring.)
    showOverlays();
    emit modelChanged();
    // The layout file changed under it (another file, another cell): read
    // again, and what is made of it follows that read rather than this
    // edit.
    if (!read && layoutFile() != layoutBefore) refreshLayout();
    else if (!read) showLayout();
    // What the solver would be given is made of the model and the layout
    // both, so an edit of the stack, the map, the processing or the mesh
    // changes it without the layout being read again. A read just started
    // makes it when it ends.
    if (!a_job) schedulePrepare();
}

void RF25Doc::showProblems()
{
    const QStringList said = problems();
    QStringList lines;
    if (said.isEmpty()) {
        lines << tr("The model is whole: a stack, a layout with its map, and ports. Solve finds what the study asks for: "
                    "the quasi-static extraction or the full-wave solve.");
    } else {
        lines << tr("What is still missing:");
        for (const QString& s : said) lines << QStringLiteral("  • ") + s;
    }
    if (!a_layoutProblem.isEmpty()) lines << a_layoutProblem;
    if (!a_readNotes.isEmpty()) lines << tr("Read with notes: %1").arg(a_readNotes);
    // What the layout gives the solver: the processing's steps and the
    // mesh's, each saying what it did, and what the two of them cannot do.
    const QStringList did = preparedLines();
    if (a_prepareJob) {
        lines << tr("Making the shapes and the mesh…");
    } else if (!did.isEmpty()) {
        lines << tr("What the layout gives the solver:");
        for (const QString& line : did) lines << QStringLiteral("  • ") + line;
    }
    // The solve: what it found, or that it is running, or why it is not.
    // (And how long it should take yet, where that can be said.)
    const QString left = timeLeft();
    if (a_solveJob) {
        lines << (!a_solveJob->fullWave       ? countNow().said
                  : a_solveJob->calibrating ? tr("Taking out the ports' own discontinuities: solving strips of their feeds…")
                                            : tr("Solving full-wave: %1 of %2 frequencies…")
                                                  .arg(frequenciesSolved()).arg(frequenciesToSolve()))
                     + (left.isEmpty() ? QString() : tr(" (%1)").arg(left));
    } else if (a_solveWhenPrepared) {
        lines << tr("Solving once the shapes and the mesh of the last edit are made…");
    } else if (a_fullWave && a_fullWave->ok()) {
        lines << tr("What it solves to:");
        for (const QString& line : a_fullWave->lines) lines << QStringLiteral("  • ") + line;
        for (const QString& line : fullWaveSummary()) lines << QStringLiteral("  • ") + line;
    } else if (a_extraction && a_extraction->ok()) {
        lines << tr("What it solves to:");
        for (const QString& line : a_extraction->lines) lines << QStringLiteral("  • ") + line;
        for (const QString& line : lineSummary()) lines << QStringLiteral("  • ") + line;
        for (const QString& line : inductanceSummary()) lines << QStringLiteral("  • ") + line;
    }
    // What became of the solution kept beside the model: shown, or why not.
    if (!a_keptSaid.isEmpty()) lines << a_keptSaid;
    QStringList cannot;
    if (a_processed) cannot += a_processed->problems;
    if (a_mesh) cannot += a_mesh->problems;
    if (!a_solveProblem.isEmpty()) cannot << a_solveProblem;
    if (!cannot.isEmpty()) {
        lines << tr("What stands in the way of a solve:");
        for (const QString& line : cannot) lines << QStringLiteral("  • ") + line;
    }
    a_problemsText->setPlainText(lines.join(QLatin1Char('\n')));
    const Stack s = a_model.stack();
    // (conductors(): the metals and the ground - what the layer map and a
    // port may name. A via is metal too, but is named by its own metals.)
    QString tail = tr("%1 layer(s), %2 of them metals or ground; the stack is %3 high; %4")
                       .arg(s.layers.size())
                       .arg(s.conductors().size())
                       .arg(qucs_s::fem::formatQuantity(s.height(), qucs_s::fem::Dim::length()),
                            said.isEmpty() ? tr("nothing is missing") : tr("%1 thing(s) missing").arg(said.size()));
    // The unknowns, where the mesh has them: what the proposal asks the
    // panel to show before a solve is ever started.
    if (a_prepareJob) tail += tr("; the shapes and the mesh are being made");
    else if (a_mesh && !a_mesh->cells.isEmpty())
        // (The charges the solve has, which the log counts: a metal solved
        //  as its two faces has each cell twice. The line said its own
        //  cells "to solve the charge on" - 924 where the solve had 1848,
        //  a quarter of the matrix it filled.)
        tail += tr("; %1 cell(s), %2")
                    .arg(a_mesh->cells.size())
                    .arg(meshedCompressed(a_model) ? a_mesh->cost.said
                                                   : chargeCostOf(a_mesh->charges > 0 ? a_mesh->charges : int(a_mesh->cells.size())));
    if (a_solveJob && a_solveJob->fullWave && a_solveJob->calibrating)
        tail += tr("; calibrating the ports") + (left.isEmpty() ? QString() : tr(", %1 for this feed").arg(left));
    else if (a_solveJob && a_solveJob->fullWave)
        tail += tr("; solving %1 of %2 frequencies").arg(frequenciesSolved()).arg(frequenciesToSolve())
                + (left.isEmpty() ? QString() : tr(", %1").arg(left));
    else if (a_solveJob)
        tail += tr("; %1").arg(extractStepSaid(a_solveJob->step.load(), a_solveJob->stepDone.load(), a_solveJob->stepAll.load(),
                                               a_solveJob->images.load()));
    else if (a_fullWave && a_fullWave->ok())
        tail += tr("; full wave, %1 port(s) at %2 frequencies").arg(a_fullWave->portNames.size()).arg(a_fullWave->frequencies.size());
    else if (a_inductance && a_inductance->ok()) tail += tr("; the loop is %1 nH").arg(a_inductance->l * 1e9, 0, 'g', 4);
    else if (a_extraction && a_extraction->ok() && !a_extraction->names.isEmpty())
        tail += tr("; %1").arg(lineSummary().value(0, tr("solved")));
    a_status->setText(tail);
}

// ------------------------------------------------------------------ The layout

QString RF25Doc::layoutFile() const
{
    const QString named = textOf(a_model.layout(), QStringLiteral("file")).trimmed();
    if (named.isEmpty()) return {};
    const QFileInfo info(named);
    if (info.isAbsolute()) return info.absoluteFilePath();
    // Beside the model (the property says so); beside nothing while it has
    // no file of its own, where the workspace is what a relative name is
    // read against.
    const QDir dir = a_DocName.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(a_DocName).absoluteDir();
    return QDir::cleanPath(dir.absoluteFilePath(named));
}

void RF25Doc::refreshLayout()
{
    if (a_job) {
        a_job->cancelled = true;
        a_job.reset();
    }
    // The shapes and the mesh were made of the layout as it was: a run
    // going on is let go here, and they are made again from what this
    // read leaves.
    a_prepareJob.reset();
    a_prepareAgain = false;
    const QString path = layoutFile();
    a_layoutProblem.clear();
    // Nothing named, or nothing that can be read: what is shown says so,
    // and nothing is read.
    const auto nothing = [this](const QString& why) {
        a_layout.reset();
        a_layoutRead.clear();
        a_layoutProblem = why;
        prepare();   // (nothing read: it clears the shapes and the mesh)
        showLayout();
        showProblems();
        emit stateChanged();
    };
    if (path.isEmpty()) return nothing(QString());
    if (!QFileInfo::exists(path)) return nothing(tr("The layout file %1 is not there.").arg(QDir::toNativeSeparators(path)));
    if (!qucs_s::layout::isLayoutFile(path))
        return nothing(tr("%1 is not a GDSII or OASIS layout.").arg(QFileInfo(path).fileName()));
    auto job = std::make_shared<Job>();
    job->path = path;
    a_job = job;
    QThread* thread = QThread::create([job] {
        job->result = qucs_s::layout::read(job->path, &job->error, &job->cancelled, &job->progress);
    });
    // (A main thread's stack is 8 MB and a thread's 512 kB on macOS, where
    // a layout's hierarchy is walked by recursion: layoutdoc.cpp's note.)
    thread->setStackSize(8u << 20);
    connect(thread, &QThread::finished, this, [this, job] {
        if (job == a_job) readingDone();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    showLayout();
    emit stateChanged();
}

void RF25Doc::readingDone()
{
    const std::shared_ptr<Job> job = a_job;
    a_job.reset();
    if (!job->result) {
        a_layout.reset();
        a_layoutRead.clear();
        a_layoutProblem = job->error.isEmpty() ? tr("%1 could not be read.").arg(QFileInfo(job->path).fileName()) : job->error;
    } else {
        a_layout = job->result;
        a_layoutRead = job->path;
        a_layoutProblem.clear();
    }
    showLayout();
    showProblems();
    emit stateChanged();
    // What the solver would be given, of the layout as it has just been
    // read. It is the work that follows a read, and it waits for none of
    // the 300 ms a burst of edits is given.
    prepare();
}

bool RF25Doc::waitForLayout(int ms)
{
    QElapsedTimer t;
    t.start();
    while (a_job && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !a_job;
}

QVector<LayerStyle> RF25Doc::stylesFor(const Layout& layout) const
{
    // Which layer of the file goes to which layer of the stack: the map's
    // rows, "layer datatype stack-layer".
    QHash<QString, QString> mapped;   // "layer/datatype" to the stack layer's name
    if (const Node* map = a_model.layout().child(QStringLiteral("rf_layer_map")))
        for (const QJsonValue& row : map->props.value(QLatin1String("rows")).toArray()) {
            const QJsonArray cells = row.toArray();
            if (cells.size() < 3) continue;
            const QString to = cells.at(2).toString().trimmed();
            if (to.isEmpty()) continue;
            const QString key = QStringLiteral("%1/%2").arg(cells.at(0).toString().trimmed(), cells.at(1).toString().trimmed());
            mapped.insert(key, to);
        }
    const Stack stack = a_model.stack();
    QVector<LayerStyle> styles;
    styles.reserve(layout.layers.size());
    for (int i = 0; i < layout.layers.size(); ++i) {
        const qucs_s::layout::LayerKey key = layout.layers.at(i).key;
        const QString to = mapped.value(key.text());
        int at = -1;
        for (int j = 0; j < stack.layers.size(); ++j)
            if (!to.isEmpty() && (stack.layers.at(j).name == to || stack.layers.at(j).tag == to)) at = j;
        LayerStyle style;
        if (at >= 0) {
            const QColor colour = layerColor(at);
            style.frame = colour.darker(140);
            style.fill = colour;
            style.pattern = stack.layers.at(at).isConductor() ? Qt::SolidPattern : Qt::Dense4Pattern;
            style.name = tr("%1 (%2)").arg(key.text(), stack.layers.at(at).name);
        } else {
            // Not sent to the stack: drawn hollow and grey, so that what is
            // left out of the solve is plain to see.
            style.frame = QColor(130, 135, 145);
            style.fill = QColor(130, 135, 145);
            style.pattern = Qt::NoBrush;
            style.name = tr("%1 (not mapped)").arg(key.text());
        }
        styles.append(style);
    }
    return styles;
}

void RF25Doc::showLayout()
{
    const QString named = textOf(a_model.layout(), QStringLiteral("file")).trimmed();
    if (a_job) {
        a_layoutLabel->setText(tr("Reading %1 (%2)…")
                                   .arg(QFileInfo(a_job->path).fileName(),
                                        QLocale().formattedDataSize(QFileInfo(a_job->path).size())));
    } else if (!a_layoutProblem.isEmpty()) {
        a_layoutLabel->setText(a_layoutProblem);
    } else if (named.isEmpty()) {
        a_layoutLabel->setText(tr("The layout names no file yet: its Layout node takes a GDSII or OASIS file."));
    } else if (a_layout) {
        a_layoutLabel->setText(tr("%1: %2 cell(s), %3 layer(s), %4 shape(s)")
                                   .arg(QFileInfo(a_layoutRead).fileName())
                                   .arg(a_layout->cells.size())
                                   .arg(a_layout->layers.size())
                                   .arg(QLocale().toString(qlonglong(a_layout->shapeCount))));
    }
    if (!a_layout) {
        a_view->setLayout(nullptr);
        return;
    }
    const bool again = a_view->layout() == a_layout;
    const QPointF center = a_view->center();
    const double scale = a_view->scale();
    a_view->setLayout(a_layout);
    a_view->setStyles(stylesFor(*a_layout));
    const QString cell = textOf(a_model.layout(), QStringLiteral("cell")).trimmed();
    const int at = cell.isEmpty() ? a_layout->mainCell() : a_layout->cellIndex(cell);
    a_view->setCell(at >= 0 ? at : a_layout->mainCell());
    // The same layout with another map or another colour: the view stays
    // where the user left it; a layout just read is fitted.
    if (again) a_view->setView(center, scale);
    else a_view->fit();
}

// ------------------------------- What the solver would be given

void RF25Doc::schedulePrepare()
{
    a_prepareSoon->start();
}

void RF25Doc::prepare()
{
    a_prepareSoon->stop();
    // One run at a time: a model that changed while this one went on is
    // made again when it ends, so that a burst of edits over a slow
    // layout leaves one run waiting, not one an edit.
    if (a_prepareJob) {
        a_prepareAgain = true;
        return;
    }
    a_prepareAgain = false;
    if (!a_layout) {
        // Nothing read: there is nothing to process, and the layout's own
        // problem says why.
        const bool had = a_processed || a_mesh;
        a_processed.reset();
        a_mesh.reset();
        if (had) {
            showOverlays();
            showProblems();
            emit preparedChanged();
        }
        return;
    }
    auto job = std::make_shared<PrepareJob>();
    job->model = a_model;
    job->layout = a_layout;
    job->called = QFileInfo(a_layoutRead).fileName();
    a_prepareJob = job;
    QThread* thread = QThread::create([job] {
        job->processed = std::make_shared<Processed>(processRead(job->model, *job->layout, job->called));
        QStringList said;
        const MeshSettings how = tabMeshSettings(job->model, &said);
        Mesh mesh = meshOf(job->processed->conductors, how, job->model.stack(), topFrequency(job->model));
        mesh.problems = said + mesh.problems;
        job->mesh = std::make_shared<Mesh>(std::move(mesh));
    });
    // (The hierarchy of a cell is walked by recursion, as the read does
    // it: a thread's 512 kB stack on macOS is not enough.)
    thread->setStackSize(8u << 20);
    connect(thread, &QThread::finished, this, [this, job] {
        if (job == a_prepareJob) prepareDone();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    emit preparedChanged();
}

void RF25Doc::prepareDone()
{
    const std::shared_ptr<PrepareJob> job = a_prepareJob;
    a_prepareJob.reset();
    a_processed = job->processed;
    a_mesh = job->mesh;
    ++a_preparedRuns;
    // The shapes or the cells are not the ones that were solved on, so
    // the capacitance is of something that is no longer there. A solve
    // going on is of them too, and goes with it - and is started again on
    // these, with one that waited for them: it was asked for, and an edit
    // under it is no reason to give it up and say nothing.
    const bool wanted = a_solveJob != nullptr || a_solveWhenPrepared;
    letSolveGo();
    // An edit that changes nothing the solver is given - an export added,
    // its file named - leaves what was solved standing: the same model to
    // solve and the same layout make the same mesh.
    const bool unchanged = (a_fullWave || a_extraction) && !a_solvedModel.isEmpty() && solveStampOf(a_model) == a_solvedModel
                           && layoutStamp() == a_solvedLayout && a_mesh && int(a_mesh->cells.size()) == a_solvedCells;
    if (!unchanged) {
        a_extraction.reset();
        a_fullWave.reset();
        a_waveNotes.clear();
        a_inductance.reset();
        a_portsSaid.clear();
        a_solvedLength = 0;
        a_solveProblem.clear();
        a_showsKept = false;
        a_solvedModel.clear();
        a_keptFile.clear();
    }
    // The solution kept beside the model, read with it: taken once the mesh
    // it was solved on is made again - the same cells; not where a solve
    // is wanted all the same, or the shapes are still to come (no mesh, the
    // layout not read yet) or are to be made again.
    if (a_kept && !a_prepareAgain && a_mesh && !a_mesh->cells.isEmpty()) {
        const qucs_s::rf25::KeptSolution kept = *a_kept;
        a_kept.reset();
        if (wanted) {
            a_keptSaid.clear();
        } else if (solveStampOf(a_model) != kept.modelHash || layoutStamp() != kept.layoutHash) {
            // (Edited, or its layout written again, before the mesh came.)
            a_keptSaid = tr("The solution kept beside the model is of another version of it: it is to be solved again.");
        } else if (int(a_mesh->cells.size()) != kept.cells) {
            a_keptSaid = tr("The solution kept beside the model was solved on a mesh of %1 cell(s), and its mesh is made of %2 "
                            "now: it is to be solved again.")
                             .arg(kept.cells)
                             .arg(a_mesh->cells.size());
        } else {
            a_fullWave = kept.fullWave;
            a_waveNotes = kept.waveNotes;
            a_extraction = kept.extraction;
            a_inductance = kept.inductance;
            a_portsSaid = kept.portsSaid;
            a_solvedLength = kept.solvedLength;
            a_solvedModel = kept.modelHash;
            a_solvedLayout = kept.layoutHash;
            a_solvedCells = kept.cells;
            a_solvedBy = kept.solvedBy;
            a_solvedAt = kept.solvedAt;
            a_showsKept = true;
            a_keptFile = solutionPathOf(a_DocName);
            a_keptSaid = tr("The solution kept beside the model is shown: solved by Qucs-S %1 on %2 UTC. Solve to solve it again.")
                             .arg(kept.solvedBy, kept.solvedAt.toUTC().toString(QStringLiteral("yyyy-MM-dd hh:mm")));
            if (a_fullWave && a_fullWave->ok()) {
                if (a_currentsPort >= a_fullWave->portNames.size()) a_currentsPort = 0;
                if (a_currentsFrequency >= a_fullWave->frequencies.size()) a_currentsFrequency = -1;
            }
        }
    }
    showOverlays();
    showProblems();
    emit preparedChanged();
    if (a_showsKept) emit solvedChanged();
    // It changed while that run went on: again, on what it is now - and a
    // solve wanted waits for that one.
    if (a_prepareAgain) {
        a_solveWhenPrepared = wanted;
        prepare();
    } else if (wanted) {
        solveWith(a_solveRequest);
    }
    showProgress();
}

bool RF25Doc::waitForPrepared(int ms)
{
    QElapsedTimer t;
    t.start();
    // A run waiting its 300 ms is started at once: a test and a tool ask
    // for what the model says now, not for what it said 300 ms ago.
    if (a_prepareSoon->isActive()) prepare();
    while ((a_prepareJob || a_prepareAgain) && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !a_prepareJob;
}

QStringList RF25Doc::preparedLines() const
{
    QStringList lines;
    if (a_processed) lines += a_processed->report.lines;
    if (a_mesh) lines += a_mesh->lines;
    return lines;
}

void RF25Doc::setProcessedShown(bool shown)
{
    if (a_showProcessed == shown) return;
    a_showProcessed = shown;
    if (a_showProcessedButton != nullptr && a_showProcessedButton->isChecked() != shown)
        a_showProcessedButton->setChecked(shown);
    QucsSettingsFile().setValue(QLatin1String(kShowProcessedKey), shown);
    showOverlays();
}

void RF25Doc::setMeshShown(bool shown)
{
    if (a_showMesh == shown) return;
    a_showMesh = shown;
    if (a_showMeshButton != nullptr && a_showMeshButton->isChecked() != shown)
        a_showMeshButton->setChecked(shown);
    QucsSettingsFile().setValue(QLatin1String(kShowMeshKey), shown);
    showOverlays();
}

void RF25Doc::setPortsShown(bool shown)
{
    if (a_showPorts == shown) return;
    a_showPorts = shown;
    if (a_showPortsButton != nullptr && a_showPortsButton->isChecked() != shown) a_showPortsButton->setChecked(shown);
    QucsSettingsFile().setValue(QLatin1String(kShowPortsKey), shown);
    showOverlays();
}

void RF25Doc::setPlacing(Placing placing)
{
    a_placing = placing;
    // The two tools, one at a time; the view picking for the one in hand.
    if (a_edgePortButton != nullptr) {
        const QSignalBlocker quiet(a_edgePortButton);
        a_edgePortButton->setChecked(placing == Placing::EdgePort);
    }
    if (a_internalPortButton != nullptr) {
        const QSignalBlocker quiet(a_internalPortButton);
        a_internalPortButton->setChecked(placing == Placing::InternalPort);
    }
    a_view->setMode(placing == Placing::EdgePort       ? LayoutView::Mode::PickPoint
                    : placing == Placing::InternalPort ? LayoutView::Mode::PickBox
                                                       : LayoutView::Mode::Select);
    if (placing == Placing::EdgePort)
        a_status->setText(tr("Click on an edge of a metal's shape to put an edge port on it; Escape to stop."));
    else if (placing == Placing::InternalPort)
        a_status->setText(tr("Drag a rectangle over a metal for an internal port driving its cells in it, or click for "
                             "the one cell under the point; Escape to stop."));
}

QString RF25Doc::placeEdgePortAt(QPointF at, double reach, QString* why)
{
    const auto fail = [why](const QString& text) {
        if (why != nullptr) *why = text;
        return QString();
    };
    if (!a_processed) return fail(tr("The layout has not been processed: there are no shapes to put a port on."));
    // The metals' shapes alone: a port is on a metal, not a via.
    const Stack stack = a_model.stack();
    QList<Conductor> metals;
    for (const Conductor& c : a_processed->conductors) {
        const Layer* layer = stack.named(c.layer);
        if (layer != nullptr && layer->kind == Layer::Metal) metals << c;
    }
    QLineF edge;
    QPointF outward;
    QString metal;
    if (!faceEdge(metals, at, QString(), QPointF(), &edge, &outward, &metal))
        return fail(tr("There is no shape of a metal to put an edge port on."));
    if (!(reach > 0)) reach = 10 / std::max(LayoutView::MinScale, a_view->scale());
    const QPointF d = edge.p2() - edge.p1();
    const double t = std::clamp(QPointF::dotProduct(at - edge.p1(), d) / std::max(1e-300, QPointF::dotProduct(d, d)), 0.0, 1.0);
    const QPointF onEdge = edge.p1() + t * d;
    if (std::hypot(at.x() - onEdge.x(), at.y() - onEdge.y()) > reach)
        return fail(tr("No edge of a metal's shape is near enough to put an edge port on: click on the edge itself."));
    // One port to an edge: the solve refuses two on one face.
    QString problem;
    for (const FullWavePort& p : portsOf(a_model, &problem)) {
        if (p.internal) continue;
        QLineF theirs;
        if (faceEdge(metals, p.at, p.metal, p.facing, &theirs) && theirs == edge)
            return fail(tr("%1 drives that edge already.").arg(p.name));
    }
    // At its middle, facing out of it the way of the axis it is along.
    const QPointF middle = edge.center();
    QString facing = QStringLiteral("auto");
    if (std::abs(outward.x()) > 0.999) facing = outward.x() > 0 ? QStringLiteral("+x") : QStringLiteral("-x");
    else if (std::abs(outward.y()) > 0.999) facing = outward.y() > 0 ? QStringLiteral("+y") : QStringLiteral("-y");
    Model m = a_model;
    Node port = m.make(QStringLiteral("rf_edge_port"));
    port.props.insert(QStringLiteral("conductor"), metal);
    port.props.insert(QStringLiteral("x"), QStringLiteral("%1[um]").arg(middle.x(), 0, 'g', 9));
    port.props.insert(QStringLiteral("y"), QStringLiteral("%1[um]").arg(middle.y(), 0, 'g', 9));
    port.props.insert(QStringLiteral("facing"), facing);
    const QString tag = port.tag;
    const QString label = port.label;
    QString error;
    if (m.add(QStringLiteral("ports"), port, -1, &error) == nullptr) return fail(error);
    if (!setModel(m, tr("an edge port"))) return fail(tr("The port could not be added."));
    focusNode(tag);
    a_status->setText(tr("%1 put on the edge of %2 from %3, %4 to %5, %6 µm, facing %7.")
                          .arg(label, metal)
                          .arg(edge.p1().x(), 0, 'g', 6)
                          .arg(edge.p1().y(), 0, 'g', 6)
                          .arg(edge.p2().x(), 0, 'g', 6)
                          .arg(edge.p2().y(), 0, 'g', 6)
                          .arg(facing));
    return tag;
}

QString RF25Doc::placeInternalPort(const QRectF& box, QString* why)
{
    const auto fail = [why](const QString& text) {
        if (why != nullptr) *why = text;
        return QString();
    };
    if (!a_processed) return fail(tr("The layout has not been processed: there are no shapes to put a port on."));
    // The topmost metal with a shape under it: under the point, or one that
    // the rectangle overlaps.
    const Stack stack = a_model.stack();
    const QRectF r = box.normalized();
    const bool point = !(r.width() > 0 && r.height() > 0);
    QString metal;
    double highest = -1;
    for (const Conductor& c : a_processed->conductors) {
        const Layer* layer = stack.named(c.layer);
        if (layer == nullptr || layer->kind != Layer::Metal) continue;
        const bool under = point ? c.outline.containsPoint(r.center(), Qt::OddEvenFill)
                                 : !c.outline.intersected(QPolygonF(r)).isEmpty();
        if (!under) continue;
        const double z = metalHeight(stack, c.layer);
        if (z > highest) {
            highest = z;
            metal = c.layer;
        }
    }
    if (metal.isEmpty()) return fail(tr("No shape of a metal is under it to put an internal port on."));
    Model m = a_model;
    Node port = m.make(QStringLiteral("rf_internal_port"));
    port.props.insert(QStringLiteral("to"), metal);
    port.props.insert(QStringLiteral("from"), QStringLiteral("ground"));
    port.props.insert(QStringLiteral("x"), QStringLiteral("%1[um]").arg(r.center().x(), 0, 'g', 9));
    port.props.insert(QStringLiteral("y"), QStringLiteral("%1[um]").arg(r.center().y(), 0, 'g', 9));
    if (!point) {
        port.props.insert(QStringLiteral("width"), QStringLiteral("%1[um]").arg(r.width(), 0, 'g', 9));
        port.props.insert(QStringLiteral("height"), QStringLiteral("%1[um]").arg(r.height(), 0, 'g', 9));
    }
    const QString tag = port.tag;
    const QString label = port.label;
    QString error;
    if (m.add(QStringLiteral("ports"), port, -1, &error) == nullptr) return fail(error);
    if (!setModel(m, tr("an internal port"))) return fail(tr("The port could not be added."));
    focusNode(tag);
    a_status->setText(point ? tr("%1 put on %2 at %3, %4 µm, from the ground: the one cell under the point.")
                                  .arg(label, metal)
                                  .arg(r.center().x(), 0, 'g', 6)
                                  .arg(r.center().y(), 0, 'g', 6)
                            : tr("%1 put on %2 over %3 by %4 µm at %5, %6 µm, from the ground: its cells in the "
                                 "rectangle driven together.")
                                  .arg(label, metal)
                                  .arg(r.width(), 0, 'g', 6)
                                  .arg(r.height(), 0, 'g', 6)
                                  .arg(r.center().x(), 0, 'g', 6)
                                  .arg(r.center().y(), 0, 'g', 6));
    return tag;
}

void RF25Doc::setCurrentsShown(bool shown)
{
    if (a_showCurrents == shown) return;
    a_showCurrents = shown;
    if (a_showCurrentsButton != nullptr && a_showCurrentsButton->isChecked() != shown)
        a_showCurrentsButton->setChecked(shown);
    QucsSettingsFile().setValue(QLatin1String(kShowCurrentsKey), shown);
    showOverlays();
    emit solvedChanged();
}

void RF25Doc::setCurrentsOf(int port, int frequency)
{
    if (port == a_currentsPort && frequency == a_currentsFrequency) return;
    a_currentsPort = std::max(0, port);
    a_currentsFrequency = frequency;
    showOverlays();
    emit solvedChanged();
}

void RF25Doc::showOverlays()
{
    QList<LayoutView::Overlay> overlays;
    const Stack stack = a_model.stack();
    // (The colour a layer's shapes are drawn in, as the stack and the
    // layout viewer have it: a conductor and its cells are the same
    // colour in every tab.)
    const auto colourOf = [&stack](const QString& layer) {
        for (int i = 0; i < stack.layers.size(); ++i)
            if (stack.layers.at(i).name == layer || stack.layers.at(i).tag == layer) return RF25Doc::layerColor(i);
        return QColor(130, 135, 145);
    };
    if (a_showProcessed && a_processed) {
        // Each layer's own overlay, in its own colour told against the
        // canvas, not a tint of it: the shape under the outline is drawn
        // in that very colour, filled, and a tint of a fill is no line at
        // all. What the processing changed is then plain against the
        // original beneath it.
        QMap<QString, QList<QPolygonF>> byLayer;
        for (const Conductor& c : a_processed->conductors) byLayer[c.layer] << c.outline;
        for (auto it = byLayer.constBegin(); it != byLayer.constEnd(); ++it) {
            LayoutView::Overlay overlay;
            overlay.shapes = it.value();
            overlay.line = outlineColor(colourOf(it.key()));
            overlay.width = 2.0;
            overlay.name = tr("%1, processed").arg(it.key());
            overlays << overlay;
        }
    }
    if (a_showMesh && a_mesh) {
        // The cells are geometry, not metal: the canvas's own ink draws
        // them, which is told from every layer's colour and from the
        // background in either theme. Thin, and let through a little, so
        // that a fine mesh does not black out the shape it divides.
        QColor ink = a_view->colors().ink;
        ink.setAlpha(190);
        QMap<QString, QList<QPolygonF>> byLayer;
        for (const MeshCell& c : a_mesh->cells) byLayer[c.layer] << c.outline;
        for (auto it = byLayer.constBegin(); it != byLayer.constEnd(); ++it) {
            LayoutView::Overlay overlay;
            overlay.shapes = it.value();
            overlay.line = ink;
            overlay.width = 1.0;
            overlay.name = tr("%1, the cells").arg(it.key());
            overlays << overlay;
        }
    }
    // A full-wave solve's surface current density, each cell filled by how
    // much of the largest it carries - in decibels over three decades, since
    // the current crowds at the edges and the driven end and on a linear
    // scale the rest of a line is all one colour.
    LayoutView::Legend legend;
    a_currentsSaid.clear();
    if (a_showCurrents && a_fullWave && a_fullWave->ok() && a_mesh) {
        const int nf = int(a_fullWave->frequencies.size()), np = int(a_fullWave->portNames.size());
        const int k = a_currentsFrequency < 0 || a_currentsFrequency >= nf ? nf - 1 : a_currentsFrequency;
        const int port = std::clamp(a_currentsPort, 0, np - 1);
        // (Driven as a circuit drives it: a source of the port's own
        //  reference, the other ports ended in theirs. Driven by an ampere
        //  with the others open, a line's far end is open and shows a
        //  standing wave that no circuit puts on it.)
        const QHash<int, double> density = currentDensity(*a_fullWave, k, port, true);
        double most = 0;
        for (const double j : density) most = std::max(most, j);
        if (most > 0) {
            // (Twenty-four bands, 2.5 dB each: eight looked like steps.)
            const QList<QColor> ramp = currentRamp(24);
            const int bands = int(ramp.size());
            // (The vias' cells in bands of their own, drawn over the metals'
            //  they stand on.)
            QList<LayoutView::Overlay> layer(bands), vias(bands);
            for (int b = 0; b < bands; ++b) {
                layer[b].fill = vias[b].fill = ramp.at(b);
                layer[b].name = tr("the current density");
                vias[b].name = tr("the current round a via");
            }
            for (auto it = density.cbegin(); it != density.cend(); ++it) {
                if (it.key() < 0 || it.key() >= a_mesh->cells.size() || !(it.value() > 0)) continue;
                const double below = 20 * std::log10(it.value() / most);   // dB, at most nought
                const int b = std::clamp(int(std::floor((below + 60) / 60 * bands)), 0, bands - 1);
                (a_fullWave->viaCells.contains(it.key()) ? vias : layer)[b].shapes << a_mesh->cells.at(it.key()).outline;
            }
            for (const QList<LayoutView::Overlay>* each : {&layer, &vias})
                for (const LayoutView::Overlay& o : *each)
                    if (!o.shapes.isEmpty()) overlays << o;
            const double f = a_fullWave->frequencies.at(k);
            legend.title = tr("|J|, %1 driven at %2 GHz, A/m").arg(a_fullWave->portNames.at(port)).arg(f / 1e9, 0, 'g', 4);
            legend.ramp = currentRamp();
            legend.low = QString::number(most * 1e-3, 'g', 3);
            legend.high = QString::number(most, 'g', 3);
            a_currentsSaid = tr("The surface current density at %1 GHz with %2 driven by a wave of a watt from a source of "
                                "its own %3 Ohm, the other ports ended in theirs: up to %4 A/m, drawn from dark to bright "
                                "over three decades below that.")
                                 .arg(f / 1e9, 0, 'g', 4)
                                 .arg(a_fullWave->portNames.at(port))
                                 .arg(a_fullWave->references.at(port), 0, 'g', 4)
                                 .arg(most, 0, 'g', 3);
            if (!a_fullWave->viaCells.isEmpty())
                a_currentsSaid += QLatin1Char(' ') + tr("A via is drawn by its current round its sides, per metre of them.");
        }
    }
    // The ports, by their names, over everything: an edge port's face - the
    // whole edge it drives, as the solve finds it - and an arrow out of it,
    // a coplanar one's grounds beside it, thinner; an internal port's
    // rectangle, or a ring at its point.
    if (a_showPorts && a_processed) {
        const bool dark = a_view->colors().background.lightness() < 128;
        const QColor ink = dark ? QColor(255, 170, 70) : QColor(205, 75, 0);
        LayoutView::Overlay faces, grounds, regions;
        faces.line = grounds.line = regions.line = ink;
        faces.width = 4;
        grounds.width = 2;
        regions.width = 2;
        QColor tint = ink;
        tint.setAlpha(45);
        regions.fill = tint;
        faces.name = tr("the ports");
        grounds.name = tr("the ports' grounds beside them");
        regions.name = tr("the internal ports' rectangles");
        // (Drawn as the grounds are: what the lines return through.)
        LayoutView::Overlay returns = grounds;
        returns.name = tr("the return conductors");
        const QList<Conductor>& shapes = a_processed->conductors;
        // A coplanar port's grounds as the solve finds them: the nearest
        // return conductors either side, past a signal (returnNetsOf()) -
        // each shape a return where the net of a cell inside it is one.
        QList<bool> shapeReturns;
        bool coplanar = false;
        for (const Node& node : a_model.ports().children)
            coplanar = coplanar || (node.enabled && node.type == QLatin1String("rf_edge_port")
                                    && textOf(node, QStringLiteral("return")) == QLatin1String("beside"));
        if (coplanar && a_mesh) {
            QString problem;
            const QList<ReturnMark> marks = returnMarksOf(a_model, &problem);
            const Stack stack = a_model.stack();
            const QList<Net> nets = netsOf(*a_mesh, shapes, stack);
            const QList<bool> isReturn = returnNetsOf(nets, *a_mesh, shapes, stack, marks);
            if (isReturn.contains(true)) {
                for (const Conductor& shape : shapes) {
                    const QRectF box = shape.outline.boundingRect();
                    int net = -1;
                    for (int i = 0; i < nets.size() && net < 0; ++i)
                        for (const int c : nets.at(i).cells) {
                            const MeshCell& cell = a_mesh->cells.at(c);
                            if (cell.layer == shape.layer && box.contains(cell.centre)
                                && shape.outline.containsPoint(cell.centre, Qt::OddEvenFill)) {
                                net = i;
                                break;
                            }
                        }
                    shapeReturns << isReturn.value(net, false);
                }
            }
        }
        for (const Node& node : a_model.ports().children) {
            if (!node.enabled) continue;
            const bool edge = node.type == QLatin1String("rf_edge_port");
            if (!edge && node.type != QLatin1String("rf_internal_port")) continue;
            const QString name = node.label.isEmpty() ? node.tag : node.label;
            bool okX = true, okY = true;
            const double x = numberOf(node, QStringLiteral("x"), a_model, 0, &okX);
            const double y = numberOf(node, QStringLiteral("y"), a_model, 0, &okY);
            if (!okX || !okY) continue;
            const QPointF at = QPointF(x, y) / 1e-6;
            if (edge) {
                const QString facing = textOf(node, QStringLiteral("facing"));
                const QPointF way = facing == QLatin1String("+x")   ? QPointF(1, 0)
                                    : facing == QLatin1String("-x") ? QPointF(-1, 0)
                                    : facing == QLatin1String("+y") ? QPointF(0, 1)
                                    : facing == QLatin1String("-y") ? QPointF(0, -1)
                                                                    : QPointF();
                QLineF face;
                QPointF outward;
                QString layer;
                if (faceEdge(shapes, at, textOf(node, QStringLiteral("conductor")).trimmed(), way, &face, &outward, &layer)) {
                    faces.shapes << QPolygonF(QList<QPointF>{face.p1(), face.p2()});
                    faces.labels << LayoutView::Overlay::Label{face.center(), name, outward};
                    if (textOf(node, QStringLiteral("return")) == QLatin1String("beside"))
                        for (const QLineF& g : besideEdges(shapes, face, outward, layer, shapeReturns.isEmpty() ? nullptr : &shapeReturns))
                            if (!g.isNull()) grounds.shapes << QPolygonF(QList<QPointF>{g.p1(), g.p2()});
                } else {
                    faces.labels << LayoutView::Overlay::Label{at, name, QPointF()};
                }
                continue;
            }
            bool okW = true, okH = true;
            const double w = numberOf(node, QStringLiteral("width"), a_model, 0, &okW) / 1e-6;
            const double h = numberOf(node, QStringLiteral("height"), a_model, 0, &okH) / 1e-6;
            if (okW && okH && w > 0 && h > 0) regions.shapes << QPolygonF(QRectF(at - QPointF(w, h) / 2, QSizeF(w, h)));
            faces.labels << LayoutView::Overlay::Label{at, name, QPointF()};
        }
        // The return conductors, each by its name at its point.
        for (const Node& node : a_model.ports().children) {
            if (!node.enabled || node.type != QLatin1String("rf_return")) continue;
            bool okX = true, okY = true;
            const double x = numberOf(node, QStringLiteral("x"), a_model, 0, &okX);
            const double y = numberOf(node, QStringLiteral("y"), a_model, 0, &okY);
            if (okX && okY)
                returns.labels << LayoutView::Overlay::Label{QPointF(x, y) / 1e-6, node.label.isEmpty() ? node.tag : node.label,
                                                             QPointF()};
        }
        for (const LayoutView::Overlay* o : {&regions, &grounds, &returns, &faces})
            if (!o->shapes.isEmpty() || !o->labels.isEmpty()) overlays << *o;
    }
    a_view->setOverlays(overlays);
    a_view->setLegend(legend);
}

// ------------------------------------------------------ The solve

bool RF25Doc::studyIsFullWave() const
{
    const Node* study = a_model.root.child(QStringLiteral("rf_study"));
    return study != nullptr && textOf(*study, QStringLiteral("kind")) == QLatin1String("full_wave");
}

void RF25Doc::solve()
{
    solveWith(SolveRequest());
}

void RF25Doc::solveWith(const SolveRequest& request)
{
    letSolveGo();
    a_solveProblem.clear();
    a_solveRequest = request;
    const auto nothing = [this](const QString& why) {
        a_extraction.reset();
        a_fullWave.reset();
        a_waveNotes.clear();
        a_inductance.reset();
        a_portsSaid.clear();
        a_solvedLength = 0;
        a_showsKept = false;
        a_keptFile.clear();
        a_solveProblem = why;
        showOverlays();
        showProblems();
        showProgress();
        emit solvedChanged();
    };
    // An edit not yet made into shapes and cells, or being made: the solve
    // is of what the model says now, so it waits for them and starts when
    // they are done. Started at once, it was thrown away when they were,
    // and nothing said why - a Solve straight after an edit, or under the
    // sanitizers' slowness any solve after one, came to nothing.
    if (a_prepareSoon->isActive() || isPreparing()) {
        a_solveWhenPrepared = true;
        if (a_prepareSoon->isActive()) prepare();
        if (a_solveButton != nullptr) a_solveButton->setText(tr("Stop"));
        showProblems();
        showProgress();
        emit solvedChanged();
        return;
    }
    if (!a_mesh || a_mesh->cells.isEmpty())
        return nothing(tr("There is no mesh to solve on: give the model a layout and a layer map first."));
    if (!a_processed) return nothing(tr("The layout has not been processed."));
    auto job = std::make_shared<SolveJob>();
    job->modelStamp = solveStampOf(a_model);
    job->layoutStamp = layoutStamp();
    job->mesh = *a_mesh;
    job->conductors = a_processed->conductors;
    job->stack = a_model.stack();
    // How long the piece that was solved is: the longer side of what it
    // covers, which is a straight line's own length and what makes a
    // capacitance into one per metre.
    QRectF bounds;
    for (const Conductor& c : std::as_const(job->conductors)) bounds = bounds.united(c.outline.boundingRect());
    job->length = std::max(bounds.width(), bounds.height()) * 1e-6;
    {
        QString problem;
        job->returns = returnMarksOf(a_model, &problem);
        if (!problem.isEmpty()) return nothing(problem);
    }
    job->fullWave = request.solver.isEmpty() ? studyIsFullWave() : request.solver == QLatin1String("full_wave");
    if (job->fullWave) {
        // The full-wave solve is driven at the ports, at each of the
        // study's frequencies (or of those asked for).
        QString problem;
        job->ports = portsOf(a_model, &problem);
        if (!problem.isEmpty()) return nothing(problem);
        if (job->ports.isEmpty())
            return nothing(tr("A full-wave solve is driven at its ports, and the model has none: put an edge port or an "
                              "internal port under Ports."));
        QString why;
        job->frequencies = request.frequencies.isEmpty() ? studySweep(a_model, &why) : request.frequencies;
        if (job->frequencies.isEmpty())
            return nothing(why.isEmpty() ? tr("The study asks for no frequencies to solve at.") : why);
        QStringList said;
        job->how = tabMeshSettings(a_model, &said);
        job->matrix = request.matrix.isEmpty() ? studyMatrix(a_model)
                      : request.matrix == QLatin1String("dense")    ? FullWaveControl::Matrix::Dense
                      : request.matrix == QLatin1String("compressed") ? FullWaveControl::Matrix::Compressed
                                                                      : FullWaveControl::Matrix::Automatic;
        job->top = topFrequency(a_model);
        const Node* study = a_model.root.child(QStringLiteral("rf_study"));
        if (request.frequencies.isEmpty() && study != nullptr && study->props.value(QStringLiteral("adaptive")).toBool())
            job->notes << tr("Adding points where the answer changes is not done yet: the study's own %1 are solved.")
                              .arg(job->frequencies.size());
    } else {
        // An inductor's L is of a loop, and what the loop is comes from the
        // ports: the current goes in at one and out at another. Two is what
        // it takes; with one, or none, there is no loop and the quasi-static
        // L of a uniform line is all that can be said.
        QList<QPointF> at;
        for (const Node& port : a_model.ports().children) {
            if (!port.enabled) continue;
            if (port.type != QLatin1String("rf_edge_port") && port.type != QLatin1String("rf_internal_port")) continue;
            bool ok = true;
            const double x = numberOf(port, QStringLiteral("x"), a_model, 0, &ok);
            if (!ok) continue;
            const double y = numberOf(port, QStringLiteral("y"), a_model, 0, &ok);
            if (!ok) continue;
            // (The ports are in metres; the cells are in the layout's own.)
            at << QPointF(x, y) / 1e-6;
        }
        if (at.size() >= 2) {
            job->loop = true;
            job->in = at.at(0);
            job->out = at.at(1);
            job->frequency = topFrequency(a_model);
            if (at.size() > 2)
                job->portsSaid = tr("%1 ports: the loop is taken from the first two.").arg(at.size());
        } else {
            job->portsSaid = at.size() == 1 ? tr("One port: a loop's inductance wants two, so that a current has "
                                                 "somewhere to go in and somewhere to come out.")
                                            : tr("No ports: a loop's inductance wants two of them.");
        }
    }
    job->held = a_solvesHeld;
    job->allowed = a_frequenciesAllowed;
    a_solveJob = job;
    ++*a_solvesAtWork;
    ++a_solvesBegun;
    QThread* thread = QThread::create([job, atWork = a_solvesAtWork] {
        const auto counted = qScopeGuard([&atWork] { --*atWork; });
        // Held by a test, until it lets go; and a solve let go before it
        // computes anything - overtaken by an edit's mesh, or stopped - has
        // nothing to compute.
        while (job->held->load() && !job->stop.load()) QThread::msleep(2);
        if (job->stop.load()) return;
        if (job->fullWave) {
            FullWaveControl control;
            control.stop = &job->stop;
            control.done = &job->done;
            control.allowed = job->allowed.get();
            control.matrix = job->matrix;
            job->wave = std::make_shared<FullWave>(
                fullWaveOf(job->mesh, job->conductors, job->stack, job->ports, job->frequencies, 1e-6, &control, job->returns));
            // The ports that asked: their own discontinuity taken out by
            // strips of their feeds - one calibration for each feed, its
            // metal and its width, and a coplanar one's grounds and vias -
            // and their reference planes moved.
            if (!job->wave->ok()) return;
            FullWaveControl quiet;
            quiet.stop = &job->stop;
            quiet.matrix = job->matrix;
            // (Counted for the bar: the strips as they begin, with their
            //  frequencies, and those frequencies as they are solved.)
            quiet.done = &job->calibrated;
            quiet.allowed = job->allowed.get();
            quiet.begins = [counted = job.get()](int frequencies) {
                counted->stripFrequencies += frequencies;
                ++counted->stripsBegun;
            };
            // (One calibration for feeds alike and strips asked alike.)
            const auto keyOf = [](const Feed& feed, const FullWavePort& port) {
                return feed.key() + QLatin1Char('|') + QString::number(port.strips, 'g', 9);
            };
            {
                // The calibrations to make, counted before the first is: the
                // bar says which of them is going on.
                QSet<QString> keys;
                for (int p = 0; p < job->ports.size(); ++p) {
                    const FullWavePort& port = job->ports.at(p);
                    const Feed feed = job->wave->feeds.value(p);
                    if (port.wantsStrips() && !port.internal && feed.width > 0) keys.insert(keyOf(feed, port));
                }
                job->calibrations = int(keys.size());
            }
            QList<Deembedding> each(job->ports.size());
            QMap<QString, std::shared_ptr<Calibration>> feeds;
            bool asked = false;
            for (int p = 0; p < job->ports.size(); ++p) {
                const FullWavePort& port = job->ports.at(p);
                if (!port.deembeds()) continue;
                asked = true;
                each[p] = Deembedding{nullptr, port.shift, port.lineImpedance, port.lineEps};
                if (!port.wantsStrips()) continue;
                const QString name = job->wave->portNames.at(p);
                const Feed feed = job->wave->feeds.value(p);
                if (port.internal || !(feed.width > 0)) {
                    job->notes << tr("%1 is not on the face of a conductor, so it has no feed to calibrate: it is "
                                     "solved as it is.").arg(name);
                    continue;
                }
                std::shared_ptr<Calibration>& calibration = feeds[keyOf(feed, port)];
                if (!calibration) {
                    job->calibrating = true;
                    // (The bar counts each calibration from nought: its
                    //  strips begun first, so that it is busy meanwhile.)
                    job->stripsBegun = 0;
                    job->stripFrequencies = 0;
                    job->calibrated = 0;
                    ++job->calibrationsBegun;
                    calibration = std::make_shared<Calibration>(
                        calibrate(job->stack, job->how, job->top, feed, job->frequencies, 1e-6, &quiet, port.strips / 1e-6));
                }
                if (job->stop.load()) {
                    job->wave->problems << tr("Stopped while taking out the ports' own discontinuities.");
                    return;
                }
                if (!calibration->ok()) {
                    job->notes << tr("%1's feed could not be calibrated: %2 It is solved as it is.")
                                      .arg(name, calibration->problems.join(QLatin1Char(' ')));
                    continue;
                }
                each[p].calibration = calibration.get();
            }
            if (!asked) return;
            *job->wave = deembedded(*job->wave, each);
            for (const std::shared_ptr<Calibration>& c : std::as_const(feeds))
                if (c->ok()) job->wave->lines << c->lines;
            return;
        }
        // Told to stop, it stops: let go and left to run, an extraction whose
        // stack's Green's function would not end held a core and gigabytes
        // until Qucs-S was killed, and the next solve ran beside it.
        ExtractControl control;
        control.stop = &job->stop;
        control.step = &job->step;
        control.done = &job->stepDone;
        control.all = &job->stepAll;
        control.images = &job->images;
        job->result =
            std::make_shared<Extraction>(extract(job->mesh, job->conductors, job->stack, 1e-6, job->returns, &control));
        if (job->stop.load()) return;
        if (job->loop)
            job->inductance = std::make_shared<Inductance>(
                inductanceOf(job->mesh, job->conductors, job->stack, job->in, job->out, 1e-6, job->frequency));
    });
    thread->setStackSize(8u << 20);
    connect(thread, &QThread::finished, this, [this, job] {
        if (job == a_solveJob) solveDone();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    if (a_solveButton != nullptr) a_solveButton->setText(tr("Stop"));
    showProblems();
    showProgress();
    emit solvedChanged();
}

int RF25Doc::frequenciesSolved() const
{
    return a_solveJob && a_solveJob->fullWave ? a_solveJob->done.load() : 0;
}

int RF25Doc::frequenciesToSolve() const
{
    return a_solveJob && a_solveJob->fullWave ? int(a_solveJob->frequencies.size()) : 0;
}

void RF25Doc::stopSolving()
{
    letSolveGo();
    showProgress();
}

void RF25Doc::letSolveGo()
{
    if (a_solveWhenPrepared && !a_solveJob) {
        a_solveWhenPrepared = false;
        if (a_solveButton != nullptr) a_solveButton->setText(tr("Solve"));
        emit solvedChanged();
        return;
    }
    a_solveWhenPrepared = false;
    if (!a_solveJob) return;
    // Let go: it ends on its own copies, and nothing is waiting for it. A
    // full-wave sweep is told to stop at its next frequency, so that a
    // stopped sweep of minutes does not run on for nothing.
    a_solveJob->stop = true;
    a_solveJob.reset();
    if (a_solveButton != nullptr) a_solveButton->setText(tr("Solve"));
    emit solvedChanged();
}

bool RF25Doc::solvingAlike(const SolveRequest& request) const
{
    if (!isSolving()) return false;
    const auto solverOf = [this](const SolveRequest& r) {
        return r.solver.isEmpty() ? (studyIsFullWave() ? QStringLiteral("full_wave") : QStringLiteral("quasi_static")) : r.solver;
    };
    if (solverOf(a_solveRequest) != solverOf(request)) return false;
    if (solverOf(request) == QLatin1String("quasi_static")) return true;
    const auto matrixOf = [this](const SolveRequest& r) {
        return r.matrix.isEmpty() ? studyMatrix(a_model)
               : r.matrix == QLatin1String("dense")      ? FullWaveControl::Matrix::Dense
               : r.matrix == QLatin1String("compressed") ? FullWaveControl::Matrix::Compressed
                                                         : FullWaveControl::Matrix::Automatic;
    };
    return a_solveRequest.frequencies == request.frequencies && matrixOf(a_solveRequest) == matrixOf(request);
}

QString RF25Doc::solvingSaid() const
{
    return isSolving() ? countNow().said : QString();
}

int RF25Doc::quasiStaticStep() const
{
    return a_solveJob && !a_solveJob->fullWave ? a_solveJob->step.load() : -1;
}

bool RF25Doc::waitForSolvesToEnd(int ms)
{
    QElapsedTimer t;
    t.start();
    while (a_solvesAtWork->load() > 0 && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return a_solvesAtWork->load() == 0;
}

RF25Doc::Count RF25Doc::countNow() const
{
    // Nought of nought is busy: the quasi-static extraction, which cannot
    // say how far it has got, and a solve waiting for its mesh.
    const SolveJob* job = a_solveJob.get();
    Count c;
    c.said = job != nullptr ? tr("Solving…") : tr("Solving once the shapes and the mesh of the last edit are made…");
    if (job != nullptr && job->fullWave && job->calibrating) {
        // A calibration: its strips of the first length, each at the
        // frequencies the first begins with, counted from nought for each
        // calibration. Busy until the first begins, and where those strips
        // do not agree and shorter ones are solved too, which no count can
        // foresee (calibrate()).
        const int begun = job->stripsBegun, planned = job->stripFrequencies;
        if (begun > 0 && begun <= kStripsPerLength) c.of = kStripsPerLength * planned / begun;
        c.done = std::min(job->calibrated.load(), c.of);
        c.said = c.of > 0 ? tr("Taking out the ports' own discontinuities: %1 of %2 frequencies of the strips of their feeds solved")
                                .arg(c.done).arg(c.of)
                          : tr("Taking out the ports' own discontinuities: solving strips of their feeds…");
        if (job->calibrations > 1) c.said += tr(" (feed %1 of %2)").arg(job->calibrationsBegun.load()).arg(job->calibrations.load());
        c.key = QStringLiteral("feed %1").arg(job->calibrationsBegun.load());
    } else if (job != nullptr && job->fullWave) {
        c.of = frequenciesToSolve();
        c.done = std::min(frequenciesSolved(), c.of);
        c.said = tr("Solving full-wave: %1 of %2 frequencies…").arg(c.done).arg(c.of);
        c.key = QStringLiteral("sweep");
    } else if (job != nullptr) {
        // The quasi-static extraction: what it is at, counted where it can
        // count - the stack's Green's functions, the lossy layers - and busy
        // where it cannot, a matrix being filled and factored.
        const int step = job->step.load(), done = job->stepDone.load(), all = job->stepAll.load();
        c.said = tr("Solving quasi-statically: %1…").arg(extractStepSaid(step, done, all, job->images.load()));
        if (all > 0) {
            c.of = all;
            c.done = std::min(done, all);
            c.key = QStringLiteral("static %1").arg(step);
        }
    }
    return c;
}

QString RF25Doc::timeLeft() const
{
    // At the rate it has gone since its count began: what is left of it,
    // each counted one having taken as long as those before it on the
    // whole. Said once one is counted and a second has gone - a guess from
    // less is a wild one.
    const Count c = countNow();
    if (!isSolving() || c.key.isEmpty() || c.key != a_countKey || !a_countSince.isValid() || c.done <= 0 || c.done >= c.of)
        return QString();
    const qint64 gone = a_countSince.elapsed();
    if (gone < 1000) return QString();
    return leftSaid(double(gone) * (c.of - c.done) / c.done / 1000);
}

QString RF25Doc::leftSaid(double seconds)
{
    if (seconds < 10) return tr("a few seconds left");
    if (seconds < 90) return tr("about %1 s left").arg(5 * qRound(seconds / 5));
    if (seconds < 90 * 60) return tr("about %1 min left").arg(qRound(seconds / 60));
    const int minutes = qRound(seconds / 60);
    return tr("about %1 h %2 min left").arg(minutes / 60).arg(minutes % 60);
}

void RF25Doc::showProgress()
{
    if (!isSolving()) {
        a_progress->stop();
        a_progressBar->hide();
        a_progressStop->hide();
        a_solvingSince.invalidate();
        a_countSince.invalidate();
        a_countKey.clear();
        return;
    }
    if (!a_solvingSince.isValid()) a_solvingSince.start();
    if (!a_progress->isActive()) a_progress->start();
    const Count c = countNow();
    // Each count's clock from its start: the sweep's, each feed's strips'.
    if (c.key != a_countKey) {
        a_countKey = c.key;
        a_countSince.start();
    }
    a_progressBar->setRange(0, c.of);
    a_progressBar->setValue(c.done);
    const QString left = timeLeft();
    a_progressBar->setToolTip(left.isEmpty() ? c.said : tr("%1 - %2").arg(c.said, left));
    // Shown once the solve has taken a quarter of a second, it never
    // flashes up and away: a coarse mesh solves in a few milliseconds.
    if (a_progressBar->isHidden() && a_solvingSince.elapsed() >= 250) {
        a_progressBar->show();
        a_progressStop->show();
    }
}

void RF25Doc::solveDone()
{
    const std::shared_ptr<SolveJob> job = a_solveJob;
    a_solveJob.reset();
    if (a_solveButton != nullptr) a_solveButton->setText(tr("Solve"));
    if (job->fullWave) {
        a_extraction.reset();
        a_inductance.reset();
        a_portsSaid.clear();
        a_solvedLength = 0;
        a_fullWave = job->wave;
        a_waveNotes = job->notes;
        a_solveProblem = job->wave && !job->wave->ok() ? job->wave->problems.join(QStringLiteral("; ")) : QString();
        if (a_fullWave && a_fullWave->ok()) {
            if (a_currentsPort >= a_fullWave->portNames.size()) a_currentsPort = 0;
            if (a_currentsFrequency >= a_fullWave->frequencies.size()) a_currentsFrequency = -1;
        }
    } else {
        a_fullWave.reset();
        a_waveNotes.clear();
        a_extraction = job->result;
        a_inductance = job->inductance;
        a_portsSaid = job->portsSaid;
        // A uniform line's own length along it - a short GSSG is wider than
        // it is long - else the longer side of what was solved.
        a_solvedLength = job->result && job->result->perMetre.fromMiddle && job->result->perMetre.length > 0
                             ? job->result->perMetre.length
                             : job->length;
        a_solveProblem = job->result && !job->result->ok() ? job->result->problems.join(QStringLiteral("; ")) : QString();
    }
    // What it was solved from and by; kept beside the model at once where
    // the model on disk is the one solved, else when it is saved.
    a_showsKept = false;
    a_keptSaid.clear();
    a_keptFile.clear();
    a_kept.reset();
    a_solvedModel = job->modelStamp;
    a_solvedLayout = job->layoutStamp;
    a_solvedCells = int(job->mesh.cells.size());
    a_solvedBy = misc::buildName();
    a_solvedAt = QDateTime::currentDateTimeUtc();
    if (!getDocChanged()) keepSolution();
    showOverlays();
    showProblems();
    showProgress();
    emit solvedChanged();
}

QByteArray RF25Doc::layoutStamp() const
{
    const QString path = layoutFile();
    if (path.isEmpty()) return QByteArray();
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? stampOf(file.readAll()) : QByteArray();
}

void RF25Doc::readKeptSolution()
{
    a_kept.reset();
    a_keptSaid.clear();
    a_showsKept = false;
    if (a_DocName.isEmpty()) return;
    const QString path = solutionPathOf(a_DocName);
    if (!QFileInfo::exists(path)) return;
    QString why;
    std::optional<KeptSolution> kept = readSolution(path, &why);
    if (!kept)
        a_keptSaid = tr("The solution beside the model was not read: %1").arg(why);
    else if (kept->modelHash != solveStampOf(a_model))
        a_keptSaid = tr("The solution kept beside the model is of another version of it: it is to be solved again.");
    else if (kept->layoutHash != layoutStamp())
        a_keptSaid = tr("The solution kept beside the model was solved on another version of its layout file: it is to be "
                        "solved again.");
    else
        a_kept = std::move(kept);   // (taken once its mesh is made: prepareDone())
}

void RF25Doc::keepSolution()
{
    // Beside a model that is a file, the solution shown of it as it is - an
    // edit since the solve leaves none shown, or another model's stamp.
    const bool wave = a_fullWave && a_fullWave->ok(), quasi = !a_fullWave && a_extraction && a_extraction->ok();
    if (a_DocName.isEmpty() || (!wave && !quasi) || a_solvedModel.isEmpty() || solveStampOf(a_model) != a_solvedModel) return;
    KeptSolution kept;
    kept.modelHash = a_solvedModel;
    kept.layoutHash = a_solvedLayout;
    kept.cells = a_solvedCells;
    kept.solvedBy = a_solvedBy;
    kept.solvedAt = a_solvedAt;
    if (wave) {
        kept.fullWave = a_fullWave;
        kept.waveNotes = a_waveNotes;
    } else {
        kept.extraction = a_extraction;
        kept.inductance = a_inductance;
        kept.portsSaid = a_portsSaid;
        kept.solvedLength = a_solvedLength;
    }
    QString error;
    if (writeSolution(solutionPathOf(a_DocName), kept, &error))
        a_keptFile = solutionPathOf(a_DocName);
    else
        a_keptSaid = tr("The solution could not be kept beside the model: %1").arg(error);
}

bool RF25Doc::waitForSolved(int ms)
{
    QElapsedTimer t;
    t.start();
    while ((a_solveJob || a_solveWhenPrepared) && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !a_solveJob && !a_solveWhenPrepared;
}

QStringList RF25Doc::lineSummary() const
{
    QStringList out;
    if (!a_extraction || !a_extraction->ok()) return out;
    // At the top of the study's own sweep, which is where a line is
    // worst: the quasi-static values alone where it names none.
    const double f = topFrequency(a_model);
    // Several conductors are coupled lines, whose modes are what there is:
    // one net's own row - its charge with every conductor at a volt - is
    // neither mode, and was given as if it were a line of its own (the
    // phase B check of 24 October). Where there are returns, the signals'
    // lines against them (the GSSG check of 1 November, G1).
    const int signalCount = a_extraction->hasReturns() ? int(a_extraction->signalNets().size()) : int(a_extraction->names.size());
    if (signalCount > 1) {
        const fem::CoupledLines cl = asCoupledLines(*a_extraction, a_solvedLength, f);
        if (!cl.error.isEmpty()) {
            out << tr("As coupled lines: %1.").arg(cl.error);
            return out;
        }
        QStringList modes, returning;
        for (const fem::LineMode& m : cl.modes)
            modes << tr("%1 Z %2 Ohm, eps_eff %3").arg(m.name).arg(m.Z.real(), 0, 'g', 4).arg(m.nEff * m.nEff, 0, 'g', 4);
        for (int i = 0; i < a_extraction->names.size(); ++i)
            if (a_extraction->returns.value(i, false)) returning << a_extraction->names.at(i);
        out << (returning.isEmpty() ? tr("%1 as coupled lines over %2 µm: %3")
                                          .arg(cl.names.join(QStringLiteral(", ")))
                                          .arg(a_solvedLength * 1e6, 0, 'g', 4)
                                          .arg(modes.join(QStringLiteral("; ")))
                                    : tr("%1 as coupled lines against %2, over %3 µm: %4")
                                          .arg(cl.names.join(QStringLiteral(", ")), returning.join(QStringLiteral(", ")))
                                          .arg(a_solvedLength * 1e6, 0, 'g', 4)
                                          .arg(modes.join(QStringLiteral("; "))));
        // Their matrices per metre (G3).
        const auto rows = [](const fem::CoupledLines::Matrix& m, double scale) {
            QStringList all;
            for (const std::vector<double>& r : m) {
                QStringList row;
                for (const double v : r) row << QString::number(v * scale, 'g', 4);
                all << row.join(QStringLiteral(" "));
            }
            return all.join(QStringLiteral("; "));
        };
        out << tr("Per metre: C [%1] pF/m, C0 [%2] pF/m, L [%3] nH/m").arg(rows(cl.C, 1e12), rows(cl.C0, 1e12), rows(cl.L, 1e9));
        if (cl.rlgc) {
            QStringList losses;
            for (const fem::LineMode& m : cl.modes) losses << tr("%1 %2 dB/m").arg(m.name).arg(m.alphaDb, 0, 'g', 3);
            out << tr("At %1 GHz: %2").arg(cl.frequency / 1e9, 0, 'g', 3).arg(losses.join(QStringLiteral(", ")));
        }
        return out;
    }
    for (int i = 0; i < a_extraction->names.size(); ++i) {
        const LineParameters lp = lineParameters(*a_extraction, i, a_solvedLength, f);
        if (!lp.ok()) continue;
        out << tr("%1: %2 pF/m, eps_eff %3, Z0 %4 Ohm over %5 µm")
                   .arg(a_extraction->names.at(i))
                   .arg(lp.c * 1e12, 0, 'g', 4)
                   .arg(lp.epsilonEff, 0, 'g', 4)
                   .arg(lp.z0, 0, 'g', 4)
                   .arg(a_solvedLength * 1e6, 0, 'g', 4);
        if (lp.frequency > 0 && (lp.r > 0 || lp.g > 0))
            out << tr("%1 at %2 GHz: R %3 Ohm/m, L %4 nH/m, G %5 S/m, C %6 pF/m; %7 dB/m")
                       .arg(a_extraction->names.at(i))
                       .arg(lp.frequency / 1e9, 0, 'g', 3)
                       .arg(lp.r, 0, 'g', 4)
                       .arg(lp.l * 1e9, 0, 'g', 4)
                       .arg(lp.g, 0, 'g', 4)
                       .arg(lp.c * 1e12, 0, 'g', 4)
                       .arg(lp.alphaDb, 0, 'g', 3);
    }
    return out;
}

QStringList RF25Doc::fullWaveSummary() const
{
    QStringList out;
    if (!a_fullWave || !a_fullWave->ok()) return out;
    const FullWave& w = *a_fullWave;
    out << a_waveNotes;
    const int np = int(w.portNames.size()), last = int(w.frequencies.size()) - 1;
    QStringList refs;
    for (int p = 0; p < np; ++p) refs << tr("%1 into %2 Ohm").arg(w.portNames.at(p)).arg(w.references.at(p), 0, 'g', 4);
    out << tr("Full wave at %1 frequency(ies) from %2 to %3 GHz; the ports %4.")
               .arg(w.frequencies.size())
               .arg(w.frequencies.first() / 1e9, 0, 'g', 4)
               .arg(w.frequencies.last() / 1e9, 0, 'g', 4)
               .arg(refs.join(QStringLiteral(", ")));
    // At the top of the sweep: every term for a few ports, and for many the
    // first port's column and each port's own reflection.
    const auto dB = [](Complex v) { return 20 * std::log10(std::max(1e-300, std::abs(v))); };
    QStringList terms;
    for (int j = 0; j < np; ++j)
        for (int i = 0; i < np; ++i) {
            if (np > 4 && i != j && j != 0) continue;
            const Complex v = w.s.at(last).at(i).at(j);
            terms << tr("S%1,%2 %3 dB at %4°").arg(i + 1).arg(j + 1).arg(dB(v), 0, 'f', 2).arg(std::arg(v) * 180 / M_PI, 0, 'f', 1);
        }
    out << tr("At %1 GHz: %2.").arg(w.frequencies.at(last) / 1e9, 0, 'g', 4).arg(terms.join(QStringLiteral("; ")));
    // Where each port's reference plane is: de-embedded, at its edge or
    // moved along its feed, whose line is said.
    for (int p = 0; p < np; ++p) {
        const PortDeembedding d = w.deembedding.value(p);
        if (!d.done) continue;
        const QString from = d.calibrated ? tr("its edge") : tr("where it was solved");
        const QString plane = d.shift == 0 ? tr("at %1").arg(from)
                                           : tr("%1 µm %2 from %3").arg(std::abs(d.shift) * 1e6, 0, 'g', 4)
                                                 .arg(d.shift > 0 ? tr("in") : tr("out"), from);
        // (Its strips' line and discontinuity, at the top: where the strips
        //  do not agree on one line there, not to be trusted.)
        const QString untrusted = d.calibrated && !d.topAgrees
                                      ? tr(" - not to be trusted there: its strips do not agree on one line from %1 GHz")
                                            .arg(d.offFrom / 1e9, 0, 'g', 4)
                                      : QString();
        if (d.calibrated && !d.givenLine)
            out << tr("%1 de-embedded: its own discontinuity taken out, its reference plane %2; its feed a line of %3 Ohm, "
                      "eps_eff %4 at %5 GHz%6.")
                       .arg(w.portNames.at(p), plane)
                       .arg(d.impedance.real(), 0, 'g', 4)
                       .arg(d.epsEff, 0, 'g', 4)
                       .arg(w.frequencies.at(last) / 1e9, 0, 'g', 4)
                       .arg(untrusted);
        else
            out << tr("%1 de-embedded: %2, its reference plane %3 along a given line of %4 Ohm, eps_eff %5%6.")
                       .arg(w.portNames.at(p),
                            d.calibrated ? tr("its own discontinuity taken out") : tr("its own discontinuity left in"),
                            plane)
                       .arg(d.impedance.real(), 0, 'g', 4)
                       .arg(d.epsEff, 0, 'g', 4)
                       .arg(d.calibrated && !d.topAgrees
                                ? tr(" - its own discontinuity, taken at %1 GHz, not to be trusted there: its strips do not agree on "
                                     "one line from %2 GHz")
                                      .arg(w.frequencies.at(last) / 1e9, 0, 'g', 4)
                                      .arg(d.offFrom / 1e9, 0, 'g', 4)
                                : QString());
    }
    // Whether the answer is what physics allows: the first sign of a bad port
    // or a mesh too coarse is that it is not. A de-embedding can leave a
    // little of what the calibration strips and the structure's own feed
    // differ by, which is the de-embedding's, not the solve's.
    const bool deembedded = !w.zSolved.isEmpty();
    // (De-embedded, it is S that is held to it: a thru has no Z.)
    const QString matrix = deembedded ? QStringLiteral("S") : QStringLiteral("Z");
    out << (w.asymmetry <= 1e-9
                ? tr("Reciprocal: %1 is symmetric to %2 of its largest term.").arg(matrix).arg(w.asymmetry, 0, 'g', 2)
                : tr("Not reciprocal: %1 is asymmetric by %2 of its largest term - look at the ports and the mesh.")
                      .arg(matrix).arg(w.asymmetry, 0, 'g', 2));
    if (w.gain <= 1e-6)
        out << tr("Passive: of a wave in at any port, at most %1 % comes back out over the sweep.").arg(100 * (1 + w.gain), 0, 'f', 2);
    else if (deembedded && w.solvedGain <= 1e-6)
        // (A thru that passes all of a wave and reflects a little more
        //  gives back twice that reflection: the recheck's 0.12 % was -64
        //  dB of it.)
        out << tr("Passive as solved; de-embedded, at one frequency it gives back %1 % more than it is given - as much as "
                  "a reflection of %2 dB beside a whole transmission gives, and what the calibration strips and this "
                  "structure's own feeds differ by.")
                   .arg(100 * w.gain, 0, 'g', 3)
                   .arg(20 * std::log10(std::max(1e-15, w.gain / 2)), 0, 'f', 0);
    else
        out << tr("Not passive: at %1 GHz it gives back %2 % more than it is given - look at the ports and the mesh.")
                   .arg(w.gainAt / 1e9, 0, 'g', 4)
                   .arg(100 * w.gain, 0, 'g', 3);
    return out;
}

QStringList RF25Doc::sParameterSummary(double reference) const
{
    QStringList out;
    if (!a_extraction || !a_extraction->ok() || !(a_solvedLength > 0)) return out;
    QString why;
    const QList<double> sweep = studySweep(a_model, &why);
    if (sweep.isEmpty()) {
        if (!why.isEmpty()) out << why;
        return out;
    }
    const double f = sweep.last();
    const auto dB = [](std::complex<double> s) { return 20 * std::log10(std::max(1e-300, std::abs(s))); };
    double slowest = 0;   // m/s: the slowest mode's, for where the S parameters first turn over
    const int nets = int(a_extraction->names.size());
    // What is no uniform line has no S parameters as one: said, and where
    // its capacitance stops being the whole story (the phase C check of 1
    // November, F2).
    if (!a_extraction->perMetre.fromMiddle) {
        out << tr("No uniform line, so no S parameters as one: the full-wave solve gives its ports' S.");
        const QuasiStaticLimit limit = quasiStaticLimit(a_model.stack(), a_mesh ? *a_mesh : Mesh(), 1e-6, 0, 0, nets, false);
        if (limit.frequency > 0)
            out << tr("Its capacitance is quasi-static to about %1 GHz (%2, eps_r %3).")
                       .arg(limit.frequency / 1e9, 0, 'g', 3)
                       .arg(limit.what)
                       .arg(limit.epsilonMax, 0, 'g', 3);
        return out;
    }
    // The lines are the signals - every net where none is a return.
    const QList<int> signalNets = a_extraction->signalNets();
    const int lines = int(signalNets.size());
    if (lines > 1) {
        // A coupled set as its 2N-port: the near ends first, then the far
        // ends, so that line 1's through is S(N+1, 1) and what it puts on
        // line 2 is S(2, 1) at the near end and S(N+2, 1) at the far.
        const fem::CoupledLines cl = asCoupledLines(*a_extraction, a_solvedLength, f);
        if (!cl.error.isEmpty()) return out;
        const double z = reference > 0 ? reference : 50.0;
        const std::vector<std::vector<std::complex<double>>> sm = fem::coupledSParameters(cl, a_solvedLength, z);
        const std::size_t n = std::size_t(lines);
        if (sm.size() != 2 * n || cl.names.size() != lines) return out;
        out << tr("%1 as a %2-port into %3 Ohm at %4 GHz: %5 through S%6,1 %7 dB; onto %8 S2,1 %9 dB at the near end, "
                  "S%10,1 %11 dB at the far.")
                   .arg(cl.names.join(QStringLiteral(", ")))
                   .arg(2 * lines)
                   .arg(z, 0, 'g', 4)
                   .arg(f / 1e9, 0, 'g', 3)
                   .arg(cl.names.at(0))
                   .arg(lines + 1)
                   .arg(dB(sm[n][0]), 0, 'f', 2)
                   .arg(cl.names.at(1))
                   .arg(dB(sm[1][0]), 0, 'f', 1)
                   .arg(lines + 2)
                   .arg(dB(sm[n + 1][0]), 0, 'f', 1);
        for (const fem::LineMode& m : cl.modes)
            if (m.nEff > 0) slowest = slowest > 0 ? std::min(slowest, kLightSpeed / m.nEff) : kLightSpeed / m.nEff;
    } else {
        const int index = signalNets.value(0, 0);
        const fem::LineRLGC line = asLine(*a_extraction, index, a_solvedLength, f);
        if (!line.error.isEmpty()) return out;
        // Into ports of its own impedance, which is the match a user is
        // looking for - or into the reference asked for, said as such
        // with its own impedance beside it.
        const double own = line.Zc.real();
        if (!(own > 0)) return out;
        const double z = reference > 0 ? reference : own;
        const std::array<std::complex<double>, 4> sp = fem::lineSParameters(line, a_solvedLength, z);
        out << (reference > 0 ? tr("%1 as a two-port into %2 Ohm at %3 GHz: S11 %4 dB, S21 %5 dB at %6°; its own Zc is "
                                   "%7 Ohm.")
                                    .arg(a_extraction->names.at(index))
                                    .arg(z, 0, 'g', 4)
                                    .arg(f / 1e9, 0, 'g', 3)
                                    .arg(dB(sp[0]), 0, 'f', 1)
                                    .arg(dB(sp[1]), 0, 'f', 2)
                                    .arg(std::arg(sp[1]) * 180 / M_PI, 0, 'f', 1)
                                    .arg(own, 0, 'g', 4)
                              : tr("%1 as a two-port into its own %2 Ohm at %3 GHz: S11 %4 dB, S21 %5 dB at %6°.")
                                    .arg(a_extraction->names.at(index))
                                    .arg(z, 0, 'g', 4)
                                    .arg(f / 1e9, 0, 'g', 3)
                                    .arg(dB(sp[0]), 0, 'f', 1)
                                    .arg(dB(sp[1]), 0, 'f', 2)
                                    .arg(std::arg(sp[1]) * 180 / M_PI, 0, 'f', 1));
        slowest = lineParameters(*a_extraction, index, a_solvedLength).velocity;
    }
    if (out.isEmpty()) return out;
    // And where the quasi-static answer stops being the whole story,
    // which is the one thing a caller must not have to guess at.
    const QuasiStaticLimit limit =
        quasiStaticLimit(a_model.stack(), a_mesh ? *a_mesh : Mesh(), 1e-6, slowest, a_solvedLength, nets);
    if (limit.frequency > 0) {
        out << tr("Quasi-static to about %1 GHz (%2, eps_r %3): above that a mode other than quasi-TEM appears and "
                  "none of this is the whole story.")
                   .arg(limit.frequency / 1e9, 0, 'g', 3)
                   .arg(limit.what)
                   .arg(limit.epsilonMax, 0, 'g', 3);
        if (f > limit.frequency)
            out << tr("The study's own %1 GHz is above that: the numbers are extrapolated, not solved.")
                       .arg(f / 1e9, 0, 'g', 3);
    }
    if (limit.resonance > 0)
        out << tr("This %1 µm of line is a half wave at %2 GHz, which is where its S parameters first turn over.")
                   .arg(a_solvedLength * 1e6, 0, 'g', 4)
                   .arg(limit.resonance / 1e9, 0, 'g', 3);
    return out;
}

namespace {

/// The Touchstone export of \a model known by \a tag, or null.
const Node* touchstoneNode(const qucs_s::rf25::Model& model, const QString& tag)
{
    const Node* node = model.root.find(tag);
    if (node == nullptr || node->type != QLatin1String("rf_touchstone")) return nullptr;
    return node;
}

/// The Equivalent circuit export of \a model known by \a tag, or null.
const Node* circuitNode(const qucs_s::rf25::Model& model, const QString& tag)
{
    const Node* node = model.root.find(tag);
    if (node == nullptr || node->type != QLatin1String("rf_circuit")) return nullptr;
    return node;
}

} // namespace

QString RF25Doc::exportPath(const QString& tag) const
{
    const QDir dir = a_DocName.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(a_DocName).absoluteDir();
    const QString stem = a_DocName.isEmpty() ? QStringLiteral("model") : QFileInfo(a_DocName).completeBaseName();
    // An equivalent circuit: the node's file, else a part beside the model.
    if (const Node* circuit = circuitNode(a_model, tag)) {
        const QString named = textOf(*circuit, QStringLiteral("file")).trimmed();
        if (named.isEmpty()) return QDir::cleanPath(dir.absoluteFilePath(QStringLiteral("%1_%2.sch").arg(stem, tag)));
        const QFileInfo info(named);
        return info.isAbsolute() ? info.absoluteFilePath() : QDir::cleanPath(dir.absoluteFilePath(named));
    }
    const Node* node = touchstoneNode(a_model, tag);
    if (node == nullptr) return {};
    const QString named = textOf(*node, QStringLiteral("file")).trimmed();
    if (!named.isEmpty()) {
        const QFileInfo info(named);
        return info.isAbsolute() ? info.absoluteFilePath() : QDir::cleanPath(dir.absoluteFilePath(named));
    }
    // Named nothing: beside the model, with as many ports as it writes - a
    // full-wave solve's own, or a quasi-static solve's line or lines.
    const bool coupled = textOf(*node, QStringLiteral("of")) == QLatin1String("coupled");
    const int lines = a_extraction ? int(a_extraction->signalNets().size()) : 1;
    const int ports = a_fullWave && a_fullWave->ok() ? int(a_fullWave->portNames.size())
                      : coupled                       ? 2 * std::max(1, lines)
                                                      : 2;
    return QDir::cleanPath(dir.absoluteFilePath(QStringLiteral("%1_%2.s%3p").arg(stem, tag).arg(ports)));
}

bool RF25Doc::exportRequest(const QString& tag, ExportRequest* what, QString* error) const
{
    if (error != nullptr) error->clear();
    const Node* node = touchstoneNode(a_model, tag);
    if (node == nullptr) {
        if (error != nullptr) *error = tr("There is no Touchstone export called %1 in this model.").arg(tag);
        return false;
    }
    ExportRequest out;
    out.coupled = textOf(*node, QStringLiteral("of")) == QLatin1String("coupled");
    out.net = textOf(*node, QStringLiteral("net")).trimmed();
    bool ok = true;
    out.impedance = numberOf(*node, QStringLiteral("impedance"), a_model, 50.0, &ok);
    if (!ok) {
        if (error != nullptr) *error = tr("The export's reference impedance is not a number.");
        return false;
    }
    out.length = numberOf(*node, QStringLiteral("length"), a_model, 0.0, &ok);
    if (!ok) {
        if (error != nullptr) *error = tr("The export's length is not a number.");
        return false;
    }
    out.version2 = textOf(*node, QStringLiteral("version")) == QLatin1String("2.0");
    out.mixed = textOf(*node, QStringLiteral("modes")) == QLatin1String("mixed");
    if (what != nullptr) *what = out;
    return true;
}

namespace {

/// \a text, Touchstone 1.1 of \a ports ports at \a frequencies
/// frequencies, every port into the one reference its option line gives,
/// made Touchstone 2.0: [Version] before the option line, the keywords
/// after it - [Reference] giving every port that one - and [End]. Its data
/// stay as 1.1 writes them, four pairs to a line, which 2.0 allows.
QString touchstone2From(const QString& text, int ports, int frequencies)
{
    const bool first = text.startsWith(QLatin1Char('#'));
    const qsizetype option = first ? 0 : text.indexOf(QLatin1String("\n#")) + 1;
    const qsizetype data = first || option > 0 ? text.indexOf(QLatin1Char('\n'), option) + 1 : 0;
    if (data <= 0) return text;
    // (# Hz S RI R <z>: the reference as the option line writes it.)
    const QString reference = text.mid(option, data - option).trimmed().section(QLatin1Char(' '), -1);
    QStringList references;
    for (int p = 0; p < ports; ++p) references << reference;
    QString keywords = QStringLiteral("[Number of Ports] %1\n").arg(ports);
    if (ports == 2) keywords += QStringLiteral("[Two-Port Data Order] 21_12\n");
    keywords += QStringLiteral("[Number of Frequencies] %1\n[Reference] %2\n[Network Data]\n")
                    .arg(frequencies)
                    .arg(references.join(QLatin1Char(' ')));
    return text.left(option) + QStringLiteral("[Version] 2.0\n") + text.mid(option, data - option) + keywords + text.mid(data)
           + QStringLiteral("[End]\n");
}

} // namespace

QString RF25Doc::exportText(const QString& tag, QString* error) const
{
    if (const Node* node = circuitNode(a_model, tag)) {
        const std::optional<EquivalentCircuit> circuit = equivalentCircuit(error);
        if (!circuit) return {};
        if (error != nullptr) error->clear();
        return circuitText(*circuit, exportPath(tag), textOf(*node, QStringLiteral("name")).trimmed());
    }
    ExportRequest what;
    if (!exportRequest(tag, &what, error)) return {};
    return exportText(what, error);
}

std::optional<EquivalentCircuit> RF25Doc::equivalentCircuit(QString* error) const
{
    const auto none = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return std::optional<EquivalentCircuit>();
    };
    if (!a_fullWave || !a_fullWave->ok())
        return none(tr("An equivalent circuit is fitted to a full-wave solve of the ports: solve the model full-wave first."));
    EquivalentCircuit c = fitCircuit(a_fullWave->portNames, a_fullWave->frequencies, a_fullWave->s, a_fullWave->references);
    if (!c.ok()) return none(c.problems.join(QLatin1Char(' ')));
    return c;
}

QString RF25Doc::circuitText(const EquivalentCircuit& circuit, const QString& path, const QString& name) const
{
    // What it is, of what, by what, and how well - in its comment.
    const QString model = a_DocName.isEmpty() ? tr("an RF 2.5D model") : QFileInfo(a_DocName).fileName();
    QStringList comment;
    comment << tr("An equivalent circuit of %1, fitted to its full-wave solve by Qucs-S %2.").arg(model, misc::buildName());
    comment << circuit.lines;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("sch")) return schematicOf(circuit, comment);
    // (A SPICE name: letters, digits and underscores.)
    QString sub = name.isEmpty() ? (a_DocName.isEmpty() ? QStringLiteral("model") : QFileInfo(a_DocName).completeBaseName()) : name;
    sub.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
    if (sub.isEmpty() || sub.at(0).isDigit()) sub.prepend(QLatin1Char('x'));
    return spiceOf(circuit, sub, comment);
}

QString RF25Doc::exportText(const ExportRequest& what, QString* error) const
{
    const auto refuse = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return QString();
    };
    if (error != nullptr) error->clear();
    // A full-wave solve's ports, whatever the export's Of says - that is a
    // choice among a quasi-static solve's lines, and the S of a full-wave
    // solve is its ports'.
    if (a_fullWave && a_fullWave->ok()) {
        if (!(what.impedance > 0))
            return refuse(tr("A reference impedance of %1 Ohm is none.").arg(what.impedance, 0, 'g', 4));
        QStringList comments;
        comments << tr("%1, an RF 2.5D model of Qucs-S").arg(QFileInfo(a_DocName).fileName());
        comments << tr("Full wave: the mixed-potential integral equation on the mesh, the retarded Green's functions of "
                       "the stack");
        for (const QString& line : a_fullWave->portsSaid) comments << line;
        // Where each reference plane is - a port's definition is where
        // comparisons with other solvers go wrong.
        QStringList planes;
        for (int p = 0; p < a_fullWave->portNames.size(); ++p) {
            const PortDeembedding d = a_fullWave->deembedding.value(p);
            const QString name = a_fullWave->portNames.at(p);
            const QString along = d.givenLine ? tr("along a given line of %1 Ohm, eps_eff %2")
                                                    .arg(d.impedance.real(), 0, 'g', 4).arg(d.epsEff, 0, 'g', 4)
                                              : tr("along its feed");
            if (!d.done)
                planes << tr("%1's at the centres of the cells it drives").arg(name);
            else if (!d.calibrated)
                planes << tr("%1's %2 µm %3 from the centres of the cells it drives, %4, its own discontinuity left in")
                              .arg(name)
                              .arg(std::abs(d.shift) * 1e6, 0, 'g', 4)
                              .arg(d.shift > 0 ? tr("in") : tr("out"), along);
            else if (d.shift == 0)
                planes << tr("%1's at its edge, its own discontinuity taken out").arg(name);
            else
                planes << tr("%1's %2 µm %3 from its edge %4, its own discontinuity taken out")
                              .arg(name)
                              .arg(std::abs(d.shift) * 1e6, 0, 'g', 4)
                              .arg(d.shift > 0 ? tr("in") : tr("out"), along);
        }
        comments << tr("The reference planes: %1").arg(planes.join(QStringLiteral("; ")));
        for (const QString& line : a_waveNotes) comments << line;
        // A differential pair's modes are Touchstone 2.0's alone.
        if (what.mixed) {
            if (a_fullWave->pairs.isEmpty())
                return refuse(tr("Mixed modes are of differential pairs, and the solve has none: pair two ports (a port's "
                                 "Pair with) and solve again."));
            comments << tr("Each port into its own reference, as [Reference] says");
            return touchstone2Of(*a_fullWave, comments, true);
        }
        if (what.version2) {
            comments << tr("Each port into its own reference, as [Reference] says");
            return touchstone2Of(*a_fullWave, comments);
        }
        comments << tr("Every port into %1 Ohm here, whatever its own reference").arg(what.impedance, 0, 'g', 4);
        return touchstoneOf(*a_fullWave, what.impedance, comments);
    }
    if (!a_extraction || !a_extraction->ok())
        return refuse(a_solveProblem.isEmpty() ? tr("Nothing is solved yet: Solve the model first.") : a_solveProblem);
    if (what.mixed)
        return refuse(tr("Mixed modes are of a full-wave solve's differential pairs: a quasi-static solve's coupled lines "
                         "are written by their nets."));
    QString why;
    const QList<double> sweep = studySweep(a_model, &why);
    if (sweep.isEmpty()) return refuse(why.isEmpty() ? tr("The study asks for no frequencies to write.") : why);
    if (!(what.impedance > 0))
        return refuse(tr("A reference impedance of %1 Ohm is none.").arg(what.impedance, 0, 'g', 4));
    const double length = what.length > 0 ? what.length : a_solvedLength;
    if (!(length > 0))
        return refuse(tr("A line's S parameters are of a length of it: the solve found none, so give the export one."));

    QStringList comments;
    comments << tr("%1, an RF 2.5D model of Qucs-S").arg(QFileInfo(a_DocName).fileName());
    comments << tr("Quasi-static: C and C0 from a charge solve, L from C0, R from the metal's surface impedance, G "
                   "from the dielectrics' loss");
    // (Into the export's own reference, which is what its data are of.)
    for (const QString& line : sParameterSummary(what.impedance)) comments << line;

    if (what.coupled) {
        const fem::CoupledSweep cs = coupledSweep(*a_extraction, length, sweep);
        if (!cs.error.isEmpty()) return refuse(cs.error);
        // (The signals' lines, against the returns where there are any.)
        QStringList lines;
        for (const int i : a_extraction->signalNets()) lines << a_extraction->names.at(i);
        comments << tr("Ports 1 to %1 are the near ends of %2, in that order; %3 to %4 their far ends")
                        .arg(lines.size())
                        .arg(lines.join(QStringLiteral(", ")))
                        .arg(lines.size() + 1)
                        .arg(2 * lines.size());
        const QString text = fem::touchstoneCoupled(cs, length, what.impedance, false, comments);
        return what.version2 ? touchstone2From(text, 2 * int(lines.size()), int(cs.points.size())) : text;
    }
    // One net, as a two-port: the one it names, or the first signal.
    int index = a_extraction->signalNets().value(0, 0);
    if (!what.net.isEmpty()) {
        index = int(a_extraction->names.indexOf(what.net));
        if (index < 0)
            return refuse(tr("There is no net called %1: the solve found %2.")
                              .arg(what.net, a_extraction->names.join(QStringLiteral(", "))));
    }
    const fem::LineSweep ls = lineSweep(*a_extraction, index, length, sweep);
    if (!ls.error.isEmpty()) return refuse(ls.error);
    comments << tr("Net %1, port 1 at its start").arg(a_extraction->names.value(index));
    const QString text = fem::touchstone(ls, length, what.impedance, comments);
    return what.version2 ? touchstone2From(text, 2, int(ls.points.size())) : text;
}

QString RF25Doc::writeExport(const QString& tag, QString* error) const
{
    QString why;
    const QString text = exportText(tag, &why);
    if (!why.isEmpty()) {
        if (error != nullptr) *error = why;
        return {};
    }
    const QString path = exportPath(tag);
    if (path.isEmpty()) {
        if (error != nullptr) *error = tr("There is nowhere to write it: give the export a file.");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error != nullptr) *error = tr("%1 could not be written: %2.").arg(path, file.errorString());
        return {};
    }
    file.write(text.toUtf8());
    file.close();
    if (error != nullptr) error->clear();
    return path;
}

QStringList RF25Doc::inductanceSummary() const
{
    QStringList out;
    if (!a_extraction || !a_extraction->ok()) return out;
    if (!a_inductance) {
        if (!a_portsSaid.isEmpty()) out << a_portsSaid;
        return out;
    }
    if (!a_inductance->ok()) {
        out << (a_inductance->problems.isEmpty() ? tr("The loop's inductance came to nothing.")
                                                 : a_inductance->problems.join(QStringLiteral(" ")));
        return out;
    }
    if (!a_portsSaid.isEmpty()) out << a_portsSaid;
    out << tr("The loop is %1 nH over %2 run(s) of current where its inductance outweighs its resistance; %3 nH "
              "and %4 Ohm at direct current.")
               .arg(a_inductance->l * 1e9, 0, 'g', 4)
               .arg(a_inductance->branches)
               .arg(a_inductance->lDc * 1e9, 0, 'g', 4)
               .arg(a_inductance->rDc, 0, 'g', 4);
    if (a_inductance->q > 0)
        out << tr("Q is %1 at %2 GHz, of that inductance over the resistance at direct current.")
                   .arg(a_inductance->q, 0, 'g', 3)
                   .arg(a_inductance->frequency / 1e9, 0, 'g', 3);
    return out;
}

QStringList RF25Doc::solvedLines() const
{
    return a_extraction ? a_extraction->lines : QStringList();
}

double RF25Doc::solvedLength() const
{
    return a_solvedLength;
}

void RF25Doc::pointerMoved(QPointF point, bool inside)
{
    if (!inside || !a_layout) {
        showProblems();
        return;
    }
    a_status->setText(tr("x = %1, y = %2")
                          .arg(qucs_s::layout::micrometres(*a_layout, point.x()),
                               qucs_s::layout::micrometres(*a_layout, point.y())));
}

void RF25Doc::focusNode(const QString& tag)
{
    a_focus = tag;
    a_stack->setFocused(tag);
    emit focusChanged(tag);
}
