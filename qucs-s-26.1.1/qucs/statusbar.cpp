/*
 * statusbar.cpp - the main window's status bar: what the tool in hand
 * does, the cursor and the diagram value under it, the selection, the
 * grid and the zoom; the schematic's problems, the last simulation and
 * the simulator; whether the document is saved; the theme
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "statusbar.h"

#include "apptheme.h"
#include "autosave.h"
#include "erc.h"
#include "ink.h"
#include "main.h"
#include "messagedock.h"
#include "mouseactions.h"
#include "node.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "textdoc.h"
#include "wire.h"
#include "wirelabel.h"
#include "components/component.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "diagrams/histogramdiagram.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"
#include "numberformat.h"
#include "paintings/painting.h"

#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace qucs_s::status {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("StatusPanel", text);
}

// A minus that lines up with the digits.
QString minus(QString s)
{
    return s.replace(QLatin1Char('-'), QChar(0x2212));
}

bool noPrefix(const QString& unit)
{
    return unit.startsWith(QLatin1String("dB")) || unit == QString(QChar(0x00B0));
}

} // namespace

// ----------------------------------------------------------------------
// A line of chips that drops the least important ones that do not fit,
// instead of making the window wider: each is shown while it is wanted and
// there is room for it and for every more important one.
class ChipRow : public QWidget
{
public:
    explicit ChipRow(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(kSpacing);
        layout->setSizeConstraint(QLayout::SetNoConstraint);
        layout->addStretch(1);   // the chips to the right
    }

    void add(QWidget* chip, int priority)
    {
        layout()->addWidget(chip);
        a_items.append({chip, priority, false});
        chip->hide();
    }

    void setWanted(QWidget* chip, bool wanted)
    {
        for (Item& i : a_items)
            if (i.chip == chip) i.wanted = wanted;
    }

    bool wanted(QWidget* chip) const
    {
        for (const Item& i : a_items)
            if (i.chip == chip) return i.wanted;
        return false;
    }

    QSize sizeHint() const override
    {
        int width = 0, height = 0, shown = 0;
        for (const Item& i : a_items) {
            height = std::max(height, i.chip->sizeHint().height());
            if (!i.wanted) continue;
            width += widthOf(i.chip) + (shown++ > 0 ? kSpacing : 0);
        }
        return QSize(width, height);
    }

    QSize minimumSizeHint() const override { return QSize(0, sizeHint().height()); }

    // Shows what is wanted and fits, most important first.
    void fit()
    {
        QList<const Item*> order;
        for (const Item& i : a_items) order.append(&i);
        std::stable_sort(order.begin(), order.end(),
                         [](const Item* a, const Item* b) { return a->priority < b->priority; });
        QSet<QWidget*> fits;
        int used = 0;
        for (const Item* i : std::as_const(order)) {
            if (!i->wanted) continue;
            const int need = widthOf(i->chip) + (used > 0 ? kSpacing : 0);
            if (used + need > width()) continue;
            used += need;
            fits.insert(i->chip);
        }
        for (const Item& i : std::as_const(a_items)) {
            const bool show = fits.contains(i.chip);
            if (i.chip->isVisibleTo(this) != show) i.chip->setVisible(show);
        }
        const QSize hint = sizeHint();
        if (hint != a_lastHint) {
            a_lastHint = hint;
            updateGeometry();
        }
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        fit();
    }

private:
    static constexpr int kSpacing = 2;
    struct Item {
        QWidget* chip;
        int priority;   // lower: kept longer
        bool wanted;
    };
    QList<Item> a_items;
    QSize a_lastHint;

    static int widthOf(const QWidget* chip)
    {
        return std::max(chip->sizeHint().width(), chip->minimumWidth());
    }
};

// ----------------------------------------------------------------------
// The hint on the left: one line, elided to the room it has, whole in its
// tool tip.
class HintLabel : public QLabel
{
public:
    explicit HintLabel(QWidget* parent) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setTextFormat(Qt::PlainText);
        setContentsMargins(4, 0, 4, 0);
        // Room for the gist of a hint: the chips give way before it does.
        setMinimumWidth(fontMetrics().averageCharWidth() * 30);
    }

    void setHint(const QString& hint)
    {
        if (hint == a_full) return;
        a_full = hint;
        setToolTip(hint);
        elide();
    }

    QString hint() const { return a_full; }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        elide();
    }

private:
    QString a_full;

    void elide()
    {
        const int room = contentsRect().width();
        QLabel::setText(room > 0 ? fontMetrics().elidedText(a_full, Qt::ElideRight, room) : a_full);
    }
};

// ----------------------------------------------------------------------
QString unitOf(const QString& name)
{
    const QString n = name.trimmed().toLower();
    if (n.isEmpty()) return {};
    if (n == QLatin1String("time")) return QStringLiteral("s");
    if (n.contains(QLatin1String("freq"))) return QStringLiteral("Hz");
    if (n.startsWith(QLatin1String("db(")) || n.startsWith(QLatin1String("dbv(")))
        return QStringLiteral("dB");
    static const QRegularExpression phase(QStringLiteral("^(phase|arg|angle|ph)\\("));
    if (phase.match(n).hasMatch()) return QString(QChar(0x00B0));
    static const QRegularExpression voltage(QStringLiteral("(^|[^a-z0-9_])v\\(|\\.vt?$"));
    if (voltage.match(n).hasMatch()) return QStringLiteral("V");
    static const QRegularExpression current(QStringLiteral("(^|[^a-z0-9_])i\\(|\\.it?$"));
    if (current.match(n).hasMatch()) return QStringLiteral("A");
    return {};
}

QString withUnit(double value, const QString& unit, int decimals)
{
    if (!std::isfinite(value)) return QString::number(value);
    const QString space = unit.isEmpty() ? QString() : QStringLiteral(" ");
    if (noPrefix(unit))
        return minus(QString::number(value, 'f', decimals >= 0 ? decimals : 2)) + space + unit;

    static const struct { int exponent; const char16_t* prefix; } prefixes[] = {
        {-15, u"f"}, {-12, u"p"}, {-9, u"n"}, {-6, u"µ"}, {-3, u"m"},
        {0, u""}, {3, u"k"}, {6, u"M"}, {9, u"G"}, {12, u"T"}};
    int exponent = 0;
    if (value != 0.0)
        exponent = std::clamp(3 * int(std::floor(std::log10(std::fabs(value)) / 3.0 + 1e-9)), -15, 12);
    QString mantissa;
    for (int pass = 0; pass < 2; ++pass) {
        const double m = value / std::pow(10.0, exponent);
        mantissa = decimals >= 0 ? QString::number(m, 'f', decimals) : QString::number(m, 'g', 4);
        // 999.96 written as 1000: one prefix up.
        if (pass == 0 && std::fabs(mantissa.toDouble()) >= 1000.0 && exponent < 12) {
            exponent += 3;
            continue;
        }
        break;
    }
    QString prefix;
    for (const auto& p : prefixes)
        if (p.exponent == exponent) prefix = QString::fromUtf16(p.prefix);
    if (unit.isEmpty() && prefix.isEmpty()) return minus(mantissa);
    return minus(mantissa) + QStringLiteral(" ") + prefix + unit;
}

QString readout(const Diagram* diagram, const MappedPoint& p)
{
    if (diagram == nullptr) return {};
    QList<const Graph*> left, right;
    for (const Graph* g : diagram->Graphs)
        (g->yAxisNo == 0 ? left : right).append(g);
    if (left.isEmpty() && right.isEmpty()) return {};

    // A value as the diagram writes its numbers - with an SI prefix and
    // the unit where it writes them automatically or with prefixes.
    const auto number = [&](double value, const QString& unit) {
        using qucs_s::numberformat::Notation;
        const bool prefixed = diagram->notation == Notation::Automatic || diagram->notation == Notation::Engineering;
        if (prefixed && !unit.isEmpty()) return withUnit(value, unit, diagram->notationDecimals);
        if (diagram->notation == Notation::Automatic && diagram->notationDecimals < 0)
            return minus(QString::number(value, 'g', 4)) + (unit.isEmpty() ? QString() : QStringLiteral(" ") + unit);
        return minus(diagram->numberText(value)) + (unit.isEmpty() ? QString() : QStringLiteral(" ") + unit);
    };
    // The unit an axis shows: its dB scale, else the one its graphs share.
    const auto axisUnit = [](const Axis& axis, const QList<const Graph*>& graphs) {
        switch (axis.Units) {
        case Axis::dbUnits: return QStringLiteral("dB");
        case Axis::dBuVUnits: return QStringLiteral("dBµV");
        case Axis::dBmUnits: return QStringLiteral("dBm");
        default: break;
        }
        QString unit;
        for (int i = 0; i < graphs.size(); ++i) {
            const QString var = graphs.at(i)->Var;
            const QString u = unitOf(var.mid(var.lastIndexOf(QLatin1Char('/')) + 1));
            if (i == 0) unit = u;
            else if (u != unit) return QString();
        }
        return unit;
    };
    // A variable without its dataset ("ngspice/ac.v(out)": ac.v(out)).
    const auto bare = [](const QString& var) { return var.mid(var.lastIndexOf(QLatin1Char('/')) + 1); };
    const auto axisName = [&](const QList<const Graph*>& graphs, const QString& several) {
        return graphs.size() == 1 ? bare(graphs.constFirst()->Var) : several;
    };

    // A histogram: the values of its variables along x, how many of them
    // fall in each bar up y.
    if (const auto* histogram = dynamic_cast<const HistogramDiagram*>(diagram)) {
        const QList<const Graph*> all = left + right;
        const QString xName = all.size() == 1 ? bare(all.constFirst()->Var) : QStringLiteral("x");
        const QString yName = histogram->height == HistogramDiagram::Percent ? tr("percent")
                              : histogram->height == HistogramDiagram::Density ? tr("density")
                                                                               : tr("count");
        return xName + QStringLiteral(" ") + number(p.x, axisUnit(Axis{}, all)) + QStringLiteral("  \u00B7  ")
               + yName + QStringLiteral(" ") + number(p.y1, QString());
    }

    const Graph* any = left.isEmpty() ? right.constFirst() : left.constFirst();
    QString xName = bare(any->axisName(0));
    if (xName.isEmpty()) xName = QStringLiteral("x");
    QStringList parts;
    parts << xName + QStringLiteral(" ") + number(p.x, unitOf(xName));
    const bool both = !left.isEmpty() && !right.isEmpty();
    if (!left.isEmpty())
        parts << axisName(left, both ? QStringLiteral("y1") : QStringLiteral("y")) + QStringLiteral(" ")
                     + number(p.y1, axisUnit(diagram->yAxis, left));
    if (!right.isEmpty())
        parts << axisName(right, QStringLiteral("y2")) + QStringLiteral(" ")
                     + number(p.y2, axisUnit(diagram->zAxis, right));
    return parts.join(QStringLiteral("  ·  "));
}

QString age(const QDateTime& then, const QDateTime& now)
{
    const qint64 seconds = then.secsTo(now);
    if (seconds < 45) return tr("just now");
    if (seconds < 3600) {
        const int minutes = std::max(1, int((seconds + 30) / 60));
        return minutes == 1 ? tr("1 min ago") : tr("%1 min ago").arg(minutes);
    }
    const QLocale locale;
    if (then.date() == now.date()) return tr("at %1").arg(locale.toString(then.time(), QLocale::ShortFormat));
    if (then.date().year() == now.date().year())
        return tr("on %1").arg(locale.toString(then.date(), QStringLiteral("d MMM")));
    return tr("on %1").arg(locale.toString(then.date(), QStringLiteral("d MMM yyyy")));
}

QStringList versionArguments(int simulator)
{
    switch (simulator) {
    case spicecompat::simNgspice: return {QStringLiteral("--version")};
    case spicecompat::simXyce: return {QStringLiteral("-v")};
    case spicecompat::simQucsator: return {QStringLiteral("-v")};
    default: return {};
    }
}

QString versionIn(int simulator, const QString& output)
{
    QString pattern;
    switch (simulator) {
    case spicecompat::simNgspice: pattern = QStringLiteral("ngspice-(\\d+(?:\\.\\d+)*)"); break;
    case spicecompat::simXyce: pattern = QStringLiteral("Xyce[^\\n]*?(?:Release\\s+)?(\\d+\\.\\d+(?:\\.\\d+)*)"); break;
    case spicecompat::simQucsator: pattern = QStringLiteral("Qucsator(?:-RF)?\\s+(\\d+\\.\\d+(?:\\.\\d+)*)"); break;
    default: return {};
    }
    const QRegularExpressionMatch m = QRegularExpression(pattern).match(output);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString duration(qint64 milliseconds)
{
    const double seconds = milliseconds / 1000.0;
    const QLocale locale;
    if (seconds < 10.0) return tr("%1 s").arg(locale.toString(seconds, 'f', 2));
    if (seconds < 60.0) return tr("%1 s").arg(locale.toString(seconds, 'f', 1));
    const qint64 whole = (milliseconds + 500) / 1000;
    return tr("%1 min %2 s").arg(whole / 60).arg(whole % 60);
}

} // namespace qucs_s::status

// ======================================================================
using namespace qucs_s::status;
namespace erc = qucs_s::erc;

namespace {

// "1 error", "3 errors".
QString plural(int n, const char* one, const char* many)
{
    return n == 1 ? QCoreApplication::translate("StatusPanel", one)
                  : QCoreApplication::translate("StatusPanel", many).arg(n);
}

// A schematic of a circuit: not a data display, not a symbol file.
bool isCircuit(const Schematic* doc)
{
    return doc != nullptr && !doc->getIsSymbolOnly()
           && QucsDoc::fileSuffix(doc->getDocName()) != QLatin1String("dpl");
}

QString programOf(int simulator)
{
    switch (simulator) {
    case spicecompat::simNgspice: return QucsSettings.NgspiceExecutable;
    case spicecompat::simXyce: return QucsSettings.XyceExecutable;
    case spicecompat::simSpiceOpus: return QucsSettings.SpiceOpusExecutable;
    case spicecompat::simQucsator: return QucsSettings.Qucsator;
    default: return {};
    }
}

// The simulators' versions, kept for the session.
QHash<QString, QString>& versions()
{
    static QHash<QString, QString> known;
    return known;
}

// A tool button no wider than its text and mark need, with room for a
// style sheet's padding and border: a status bar shows many of them.
class Chip : public QToolButton
{
public:
    using QToolButton::QToolButton;

    QSize sizeHint() const override
    {
        QSize size = QToolButton::sizeHint();
        if (toolButtonStyle() == Qt::ToolButtonIconOnly) return size;
        int content = fontMetrics().horizontalAdvance(text());
        if (toolButtonStyle() == Qt::ToolButtonTextBesideIcon && !icon().isNull())
            content += iconSize().width() + 4;
        size.setWidth(std::min(size.width(), content + 16));
        return size;
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
};

QToolButton* makeChip(QWidget* parent, const char* name)
{
    auto* chip = new Chip(parent);
    chip->setObjectName(QLatin1String(name));
    chip->setAutoRaise(true);
    chip->setFocusPolicy(Qt::NoFocus);
    chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
    chip->setIconSize(QSize(10, 10));
    return chip;
}

QLabel* makeLabel(QWidget* parent, const char* name)
{
    auto* label = new QLabel(parent);
    label->setObjectName(QLatin1String(name));
    label->setTextFormat(Qt::PlainText);
    label->setContentsMargins(6, 0, 6, 0);
    return label;
}

// A round mark: filled, or a ring for something under way.
QIcon mark(const QColor& colour, bool ring)
{
    QIcon icon;
    for (qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(10, 10) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        if (ring) {
            p.setPen(QPen(colour, 1.6));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(1.3, 1.3, 7.4, 7.4));
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(colour);
            p.drawEllipse(QRectF(1.0, 1.0, 8.0, 8.0));
        }
        icon.addPixmap(pixmap);
    }
    return icon;
}

// Three rows of three dots: the grid, faint when it is hidden.
QIcon gridMark(QColor colour, bool shown)
{
    if (!shown) colour.setAlphaF(0.35);
    QIcon icon;
    for (qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(10, 10) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                p.drawEllipse(QPointF(1.5 + 3.5 * column, 1.5 + 3.5 * row), 0.95, 0.95);
        icon.addPixmap(pixmap);
    }
    return icon;
}

// The component's name and its first property shown on the schematic.
QString describe(const Component* c)
{
    const Property* shown = nullptr;
    for (const Property* p : c->Props)
        if (p->display) { shown = p; break; }
    if (shown == nullptr && !c->Props.isEmpty()) shown = c->Props.constFirst();
    QString text = c->Name;
    if (shown != nullptr && !shown->Value.isEmpty()) {
        QString value = shown->Value;
        if (value.size() > 32) value = value.left(31) + QChar(0x2026);
        text += QStringLiteral(": %1 = %2").arg(shown->Name, value);
    }
    return text;
}

QString shortcutOf(const QAction* action)
{
    return action == nullptr ? QString() : action->shortcut().toString(QKeySequence::NativeText);
}

} // namespace

// ----------------------------------------------------------------------
StatusPanel::StatusPanel(QucsApp* app) : QObject(app), a_app(app)
{
    QStatusBar* bar = app->statusBar();
    a_hint = new HintLabel(bar);
    a_hint->setObjectName(QStringLiteral("statusHint"));
    bar->addWidget(a_hint, 1);

    a_row = new ChipRow(bar);
    a_row->setObjectName(QStringLiteral("statusChips"));
    a_readout = makeLabel(a_row, "statusReadout");
    a_selection = makeChip(a_row, "statusSelection");
    a_position = makeLabel(a_row, "statusPosition");
    a_grid = makeChip(a_row, "statusGrid");
    a_grid->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    a_zoom = makeChip(a_row, "statusZoom");
    a_problems = makeChip(a_row, "statusProblems");
    a_run = makeChip(a_row, "statusRun");
    a_simulator = makeChip(a_row, "statusSimulator");
    a_saved = makeChip(a_row, "statusSaved");
    a_theme = makeChip(a_row, "statusTheme");
    a_theme->setToolButtonStyle(Qt::ToolButtonIconOnly);
    a_theme->setIconSize(QSize(14, 14));
    // Left to right; the lower the number, the longer a chip stays when
    // the window narrows.
    a_row->add(a_readout, 3);
    a_row->add(a_selection, 5);
    a_row->add(a_position, 2);
    a_row->add(a_grid, 8);
    a_row->add(a_zoom, 4);
    a_row->add(a_problems, 1);
    a_row->add(a_run, 0);
    a_row->add(a_simulator, 6);
    a_row->add(a_saved, 7);
    a_row->add(a_theme, 9);
    bar->addPermanentWidget(a_row, 0);

    a_refreshTimer = new QTimer(this);
    a_refreshTimer->setSingleShot(true);
    a_refreshTimer->setInterval(0);
    connect(a_refreshTimer, &QTimer::timeout, this, &StatusPanel::refresh);
    a_checkTimer = new QTimer(this);
    a_checkTimer->setSingleShot(true);
    connect(a_checkTimer, &QTimer::timeout, this, &StatusPanel::check);
    a_clock = new QTimer(this);
    connect(a_clock, &QTimer::timeout, this, [this] {
        updateRun();
        updateSaved();
        a_row->fit();
    });
    a_clock->start(30000);

    connect(a_selection, &QToolButton::clicked, this, [this] {
        if (a_app->magSel != nullptr) a_app->magSel->trigger();
        scheduleRefresh();
    });
    connect(a_grid, &QToolButton::clicked, this, [this] {
        if (a_app->showGrid != nullptr && a_app->showGrid->isEnabled()) a_app->showGrid->trigger();
        scheduleRefresh();
    });
    connect(a_zoom, &QToolButton::clicked, this, [this] { popUp(a_zoom, zoomMenu()); });
    connect(a_problems, &QToolButton::clicked, this, [this] {
        Schematic* doc = a_app->currentSchematic();
        if (doc == nullptr) return;
        a_app->checkSchematic(doc, true);
        if (!a_app->messages()->issues().isEmpty()) a_app->slotLocateProblem(0);
    });
    connect(a_run, &QToolButton::clicked, this, [this] {
        if (a_runExternal) a_app->simulationConsole()->showConsole();
        else a_app->slotShowLastMsg();
    });
    connect(a_simulator, &QToolButton::clicked, this, [this] { popUp(a_simulator, simulatorMenu()); });
    connect(a_saved, &QToolButton::clicked, this, [this] {
        QucsDoc* doc = a_app->getDoc();
        if (doc != nullptr && (doc->getDocChanged() || doc->getDocName().isEmpty())) a_app->fileSave->trigger();
        scheduleRefresh();
    });
    connect(a_theme, &QToolButton::clicked, this, [this] {
        if (a_app->themeMenu != nullptr) popUp(a_theme, a_app->themeMenu);
    });
    // The simulator switched in the tool bar (the settings are written
    // after the combo box changed: the next turn of the loop sees them).
    connect(a_app->simulatorsCombobox, &QComboBox::currentIndexChanged, this, &StatusPanel::scheduleRefresh);

    // The simulators are asked for their versions once the window shows:
    // never in the command-line modes.
    a_app->installEventFilter(this);
    styleChips();
    updateTheme();
}

StatusPanel::~StatusPanel() = default;

QString StatusPanel::hint() const
{
    return a_hint->hint();
}

QString StatusPanel::simulatorVersion() const
{
    return versions().value(versionKey(QucsSettings.DefaultSimulator));
}

bool StatusPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == a_app && event->type() == QEvent::Show && !a_mayAsk) {
        a_mayAsk = true;
        scheduleRefresh();
    } else if (event->type() == QEvent::Paint) {
        // The document in front was drawn again: its selection, zoom or
        // tool may have changed.
        if (auto* doc = qobject_cast<Schematic*>(a_watched.data()); doc != nullptr && watched == doc->viewport())
            scheduleRefresh();
    }
    return QObject::eventFilter(watched, event);
}

// ----------------------------------------------------------------------
void StatusPanel::setCursor(int x, int y, const QString& readout)
{
    QWidget* front = a_app->DocumentTab != nullptr ? a_app->DocumentTab->currentWidget() : nullptr;
    if (qobject_cast<TextDoc*>(front) != nullptr) {
        a_position->setMinimumWidth(0);
        a_position->setText(tr("Ln %1, Col %2").arg(x).arg(y));
        a_position->setToolTip(tr("Line and column of the cursor"));
    } else {
        a_position->setText(QStringLiteral("x %1   y %2").arg(minus(QString::number(x)), minus(QString::number(y))));
        a_position->setToolTip(tr("The cursor's position on the schematic"));
        // Wide enough for four digits and a sign: it does not jump as the
        // mouse moves.
        const QString widest = QStringLiteral("x %1   y %1").arg(minus(QStringLiteral("-8888")));
        a_position->setMinimumWidth(a_position->fontMetrics().horizontalAdvance(widest)
                                    + a_position->contentsMargins().left() + a_position->contentsMargins().right());
    }
    a_row->setWanted(a_position, front != nullptr);

    if (readout.isEmpty()) {
        a_readout->clear();
        a_readout->setMinimumWidth(0);
    } else {
        a_readout->setText(readout);
        a_readout->setToolTip(tr("The value of the diagram under the cursor"));
        // Only ever wider while over a diagram: it does not jump either.
        a_readout->setMinimumWidth(std::max(a_readout->minimumWidth(), a_readout->sizeHint().width()));
    }
    a_row->setWanted(a_readout, !readout.isEmpty());
    a_row->fit();
}

void StatusPanel::follow(QWidget* document)
{
    if (a_watched == document) return;
    if (QWidget* old = a_watched.data()) {
        disconnect(old, nullptr, this, nullptr);
        if (auto* doc = qobject_cast<Schematic*>(old)) doc->viewport()->removeEventFilter(this);
    }
    a_watched = document;
    if (auto* doc = qobject_cast<Schematic*>(document)) {
        const auto edited = [this] {
            a_checkTimer->start(400);
            scheduleRefresh();
        };
        connect(doc, &Schematic::signalEdited, this, edited);
        connect(doc, &Schematic::signalDocumentRebuilt, this, edited);
        connect(doc, &Schematic::signalFileChanged, this, &StatusPanel::scheduleRefresh);
        doc->viewport()->installEventFilter(this);
    } else if (auto* text = qobject_cast<TextDoc*>(document)) {
        connect(text, &TextDoc::signalFileChanged, this, &StatusPanel::scheduleRefresh);
    }
}

void StatusPanel::documentChanged()
{
    QWidget* front = a_app->DocumentTab != nullptr ? a_app->DocumentTab->currentWidget() : nullptr;
    follow(front);
    a_readout->clear();
    a_readout->setMinimumWidth(0);
    a_row->setWanted(a_readout, false);
    if (front == nullptr) a_row->setWanted(a_position, false);
    a_checkTimer->start(0);
    refresh();
}

void StatusPanel::scheduleRefresh()
{
    if (!a_refreshTimer->isActive()) a_refreshTimer->start();
}

void StatusPanel::refresh()
{
    a_refreshTimer->stop();
    QWidget* front = a_app->DocumentTab != nullptr ? a_app->DocumentTab->currentWidget() : nullptr;
    if (front != a_watched.data()) {
        documentChanged();   // comes back here
        return;
    }
    auto* doc = qobject_cast<Schematic*>(front);
    updateHint(doc);
    updateSelection(doc);
    updateGrid(doc);
    updateZoom(doc);
    if (!isCircuit(doc)) a_row->setWanted(a_problems, false);
    else if (QucsSettings.DefaultSimulator != a_checkedSimulator && !a_checkTimer->isActive())
        a_checkTimer->start(0);   // the checks depend on the simulator
    if (front == nullptr) {
        a_row->setWanted(a_position, false);
        a_row->setWanted(a_readout, false);
    }
    updateRun();
    updateSimulator();
    updateSaved();
    a_row->fit();
}

void StatusPanel::themeChanged()
{
    styleChips();
    updateTheme();
    check();
    refresh();
}

// ----------------------------------------------------------------------
void StatusPanel::styleChips()
{
    // Flat, a shade on the mouse, in every style: the macOS style would
    // give each a bezel. From the palette of the look in use.
    const QPalette& pal = QApplication::palette();
    const QColor window = pal.color(QPalette::Window), text = pal.color(QPalette::WindowText);
    // One size of type for the chips, the labels and the hint: the
    // application's, but not above 11 points (macOS gives labels 13 and
    // tool buttons 10; its own status bars 11).
    const double points = QApplication::font().pointSizeF();
    const QString size = points > 0.0 ? QStringLiteral(" font-size: %1pt;").arg(std::min(points, 11.0)) : QString();
    using qucs_s::apptheme::mix;
    a_row->setStyleSheet(
        QStringLiteral("QToolButton { background: transparent; color: %1; border: 1px solid transparent;"
                       " border-radius: 5px; padding: 1px 6px;%5 }"
                       "QToolButton:hover { background: %2; border-color: %2; }"
                       "QToolButton:pressed { background: %3; border-color: %3; }"
                       "QToolButton:disabled { color: %4; }"
                       "QLabel { color: %1;%5 }")
            .arg(text.name(), mix(window, text, 0.12).name(), mix(window, text, 0.22).name(),
                 mix(window, text, 0.45).name(), size));
    a_hint->setStyleSheet(size.isEmpty() ? QString() : QStringLiteral("QLabel {%1 }").arg(size));
}

QColor StatusPanel::toneColour(Tone tone) const
{
    const QPalette& pal = a_row->palette();
    const bool dark = qucs_s::ink::isDark(pal.color(QPalette::Window));
    switch (tone) {
    case Tone::Ok: return dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37);
    case Tone::Warn: return dark ? QColor(0xd2, 0x99, 0x22) : QColor(0x9a, 0x67, 0x00);
    case Tone::Error: return dark ? QColor(0xf8, 0x51, 0x49) : QColor(0xcf, 0x22, 0x2e);
    case Tone::Busy: return pal.color(QPalette::Highlight);
    case Tone::None: break;
    }
    return pal.color(QPalette::WindowText);
}

void StatusPanel::setChip(QToolButton* chip, const QString& text, Tone tone)
{
    chip->setText(text);
    static const char* const names[] = {"", "ok", "warn", "error", "busy"};
    chip->setProperty("tone", QString::fromLatin1(names[int(tone)]));
    if (tone == Tone::None) {
        chip->setIcon(QIcon());
        chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
    } else {
        chip->setIcon(mark(toneColour(tone), tone == Tone::Busy));
        chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
}

void StatusPanel::popUp(QToolButton* chip, QMenu* menu)
{
    if (menu == nullptr) return;
    // Above the chip: the status bar is at the bottom of the window.
    const QSize size = menu->sizeHint();
    menu->popup(chip->mapToGlobal(QPoint(0, -size.height())));
}

// ----------------------------------------------------------------------
QString StatusPanel::modeHint(Schematic* doc) const
{
    if (doc == nullptr) return {};
    const QucsApp* app = a_app;
    const auto press = app->MousePressAction;
    const auto move = app->MouseMoveAction;
    const QString stop = tr("Esc to stop");
#ifdef Q_OS_MACOS
    const QString add = tr("⌘-click to add");
#else
    const QString add = tr("Ctrl+click to add");
#endif
    const auto join = [](const QStringList& parts) { return parts.join(QStringLiteral("  ·  ")); };

    if (app->TuningMode && press == &MouseActions::MPressTune)
        return join({tr("Click a component to tune it"), tr("the tuner changes its values")});
    if (move == &MouseActions::MMovePaste || move == &MouseActions::MMovePaste2)
        return join({tr("Click to paste"), tr("right-click to rotate"), tr("Esc to cancel")});
    if (press == &MouseActions::MPressElement) {
        const Element* e = app->view != nullptr ? app->view->selElem : nullptr;
        if (e != nullptr && (e->Type & isComponent)) {
            const auto* c = static_cast<const Component*>(e);
            // "ground (reference potential)": ground.
            QString what = c->Description.section(QStringLiteral(" ("), 0, 0).trimmed();
            if (what.isEmpty()) what = c->Name;
            return join({tr("Click to place %1").arg(what), tr("right-click to rotate"), stop});
        }
        if (e != nullptr && e->Type == isDiagram) return join({tr("Click to place the diagram"), stop});
        return join({tr("Click to draw"), stop});
    }
    if (press == &MouseActions::MPressWire1) return join({tr("Click where the wire starts"), stop});
    if (press == &MouseActions::MPressWire2) {
        using PlanType = qucs_s::wire::Planner::PlanType;
        const PlanType plan = doc->a_wirePlanner.planType();
        const QString corner = tr("click to set a corner, double-click to end");
        // ThreeStepYX is the last orthogonal route before the free one.
        if (plan == PlanType::Straight)
            return join({tr("Free wiring: %1").arg(corner), tr("right-click for orthogonal wiring")});
        if (plan == PlanType::ThreeStepYX)
            return join({tr("Orthogonal wiring: %1").arg(corner), tr("right-click for free wiring")});
        return join({tr("Orthogonal wiring: %1").arg(corner), tr("right-click for another route")});
    }
    if (press == &MouseActions::MPressLabel) return join({tr("Click a wire or a node to name its net"), stop});
    if (press == &MouseActions::MPressDelete) return join({tr("Click an element to delete it"), stop});
    if (press == &MouseActions::MPressActivate)
        return join({tr("Click a component to deactivate or activate it"), tr("drag across several"), stop});
    if (press == &MouseActions::MPressRotate) return join({tr("Click an element to rotate it"), stop});
    if (press == &MouseActions::MPressMirrorX)
        return join({tr("Click an element to mirror it about the X axis"), stop});
    if (press == &MouseActions::MPressMirrorY)
        return join({tr("Click an element to mirror it about the Y axis"), stop});
    if (press == &MouseActions::MPressMarker) return join({tr("Click a graph to put a marker on it"), stop});
    if (press == &MouseActions::MPressSetLimits)
        return join({tr("Drag across a diagram to zoom its axes to that area"), stop});
    if (press == &MouseActions::MPressOnGrid) return join({tr("Click an element to put it on the grid"), stop});
    if (press == &MouseActions::MPressMoveText) return join({tr("Drag a component's text to move it"), stop});
    if (press == &MouseActions::MPressZoomIn) return join({tr("Drag a box to zoom into it"), stop});
    if (press == &MouseActions::MPressSelect)
        return join({tr("Double-click to edit"), add, tr("drag on empty space to select")});
    return {};   // a drag in progress: the hint of the tool stays
}

void StatusPanel::updateHint(Schematic* doc)
{
    if (doc == nullptr) {
        a_hint->setHint(QString());
        return;
    }
    const QString hint = modeHint(doc);
    if (!hint.isNull()) a_hint->setHint(hint);
}

void StatusPanel::updateSelection(Schematic* doc)
{
    int components = 0, wires = 0, labels = 0, diagrams = 0, paintings = 0;
    const Component* one = nullptr;
    if (doc != nullptr) {
        if (doc->a_Components != nullptr)
            for (const Component* c : *doc->a_Components)
                if (c->isSelected) { ++components; one = c; }
        if (doc->a_Wires != nullptr)
            for (Wire* w : *doc->a_Wires) {
                if (w->isSelected) ++wires;
                if (w->hasLabel() && w->label()->isSelected) ++labels;
            }
        if (doc->a_Nodes != nullptr)
            for (Node* n : *doc->a_Nodes)
                if (n->hasLabel() && n->label()->isSelected) ++labels;
        if (doc->a_Diagrams != nullptr)
            for (const Diagram* d : *doc->a_Diagrams)
                if (d->isSelected) ++diagrams;
        if (doc->a_Paintings != nullptr)
            for (const Painting* p : *doc->a_Paintings)
                if (p->isSelected) ++paintings;
    }
    const int total = components + wires + labels + diagrams + paintings;
    a_row->setWanted(a_selection, total > 0);
    if (total == 0) return;

    QStringList kinds;
    if (components > 0) kinds << plural(components, "1 component", "%1 components");
    if (wires > 0) kinds << plural(wires, "1 wire", "%1 wires");
    if (labels > 0) kinds << plural(labels, "1 label", "%1 labels");
    if (diagrams > 0) kinds << plural(diagrams, "1 diagram", "%1 diagrams");
    if (paintings > 0) kinds << plural(paintings, "1 painting", "%1 paintings");
    QString text;
    if (total == 1 && one != nullptr) text = describe(one);
    else if (total == 1) text = kinds.constFirst();
    else text = tr("%1 selected").arg(total);
    setChip(a_selection, text, Tone::None);
    QString tip = tr("Selected: %1").arg(kinds.join(QStringLiteral(", ")));
    if (total == 1 && one != nullptr && !one->Description.isEmpty()) tip += QStringLiteral(" (%1)").arg(one->Description);
    const QString key = shortcutOf(a_app->magSel);
    tip += QLatin1Char('\n') + (key.isEmpty() ? tr("Click to zoom to the selection")
                                              : tr("Click to zoom to the selection (%1)").arg(key));
    a_selection->setToolTip(tip);
}

void StatusPanel::updateGrid(Schematic* doc)
{
    a_row->setWanted(a_grid, doc != nullptr);
    if (doc == nullptr) return;
    const int gx = doc->getGridX(), gy = doc->getGridY();
    const bool shown = doc->gridShown();
    a_grid->setText(gx == gy ? tr("Grid %1").arg(gx) : tr("Grid %1×%2").arg(gx).arg(gy));
    a_grid->setIcon(gridMark(a_row->palette().color(QPalette::WindowText), shown));
    a_grid->setProperty("shown", shown);
    const QString key = shortcutOf(a_app->showGrid);
    QString tip = tr("Elements snap to a grid of %1 × %2.").arg(gx).arg(gy) + QLatin1Char('\n')
                  + (shown ? tr("The grid is shown: click to hide it") : tr("The grid is hidden: click to show it"));
    if (!key.isEmpty()) tip += QStringLiteral(" (%1)").arg(key);
    a_grid->setToolTip(tip);
    a_grid->setEnabled(a_app->showGrid != nullptr && a_app->showGrid->isEnabled());
}

void StatusPanel::updateZoom(Schematic* doc)
{
    a_row->setWanted(a_zoom, doc != nullptr);
    if (doc == nullptr) return;
    setChip(a_zoom, QStringLiteral("%1%").arg(qRound(doc->getScale() * 100.0)), Tone::None);
    a_zoom->setToolTip(tr("Zoom: click to fit, zoom to the selection or pick a scale"));
}

QMenu* StatusPanel::zoomMenu()
{
    Schematic* doc = a_app->currentSchematic();
    if (doc == nullptr) return nullptr;
    auto* menu = new QMenu(a_zoom);
    menu->setObjectName(QStringLiteral("statusZoomMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (a_app->magAll != nullptr) menu->addAction(a_app->magAll);
    if (a_app->magSel != nullptr) menu->addAction(a_app->magSel);
    menu->addSeparator();
    const int now = qRound(doc->getScale() * 100.0);
    for (int percent : {25, 50, 100, 200, 400}) {
        QAction* a = menu->addAction(QStringLiteral("%1%").arg(percent));
        a->setCheckable(true);
        a->setChecked(percent == now);
        connect(a, &QAction::triggered, this, [this, percent] {
            if (Schematic* d = a_app->currentSchematic(); d != nullptr && d->getScale() > 0.0)
                d->zoomBy(percent / 100.0 / d->getScale());
            scheduleRefresh();
        });
    }
    menu->addSeparator();
    QAction* in = menu->addAction(tr("Zoom In"));
    connect(in, &QAction::triggered, this, [this] {
        if (Schematic* d = a_app->currentSchematic()) d->zoomBy(2.0);
        scheduleRefresh();
    });
    if (a_app->magMinus != nullptr) menu->addAction(a_app->magMinus);
    return menu;
}

// ----------------------------------------------------------------------
void StatusPanel::check()
{
    a_checkTimer->stop();
    a_checkedSimulator = QucsSettings.DefaultSimulator;
    Schematic* doc = a_app->currentSchematic();
    if (!isCircuit(doc)) {
        a_row->setWanted(a_problems, false);
        a_row->fit();
        return;
    }
    const QList<erc::Issue> issues = erc::check(doc);
    const int errors = erc::errorCount(issues);
    const int warnings = int(issues.size()) - errors;
    if (issues.isEmpty()) {
        setChip(a_problems, tr("No problems"), Tone::Ok);
        a_problems->setToolTip(tr("The electrical rule check finds nothing wrong with the schematic."));
    } else {
        QString text;
        if (errors > 0) text = plural(errors, "1 error", "%1 errors");
        if (warnings > 0)
            text += (text.isEmpty() ? QString() : QStringLiteral(", ")) + plural(warnings, "1 warning", "%1 warnings");
        setChip(a_problems, text, errors > 0 ? Tone::Error : Tone::Warn);
        QStringList lines;
        for (int i = 0; i < std::min<qsizetype>(issues.size(), 6); ++i)
            lines << QStringLiteral("• ") + issues.at(i).message;
        if (issues.size() > 6) lines << tr("and %1 more").arg(issues.size() - 6);
        lines << tr("Click to list them and go to the first");
        a_problems->setToolTip(lines.join(QLatin1Char('\n')));
    }
    a_row->setWanted(a_problems, true);
    a_row->fit();
}

// ----------------------------------------------------------------------
void StatusPanel::watchRun(SimulationRun* run, const QString& document)
{
    if (run == nullptr) return;
    a_runState = RunState::Running;
    a_runExternal = true;
    a_runDocument = document;
    a_runWarnings = 0;
    a_runClock.start();
    connect(run, &SimulationRun::simulated, this, [this](SimulationRun* r) {
        runEnded(r->wasStopped() ? Outcome::Stopped
                 : r->hasError() ? Outcome::Failed
                 : r->warningCount() > 0 ? Outcome::Warned
                                         : Outcome::Succeeded,
                 r->warningCount());
    });
    // Gone without an end (the application closes): not running any more.
    connect(run, &QObject::destroyed, this, [this] {
        if (a_runState == RunState::Running) runEnded(Outcome::Stopped, 0);
    });
    a_clock->start(1000);
    updateRun();
    a_row->fit();
}

void StatusPanel::runStarted(const QString& document)
{
    a_runState = RunState::Running;
    a_runExternal = false;
    a_runDocument = document;
    a_runWarnings = 0;
    a_runClock.start();
    a_clock->start(1000);
    updateRun();
    a_row->fit();
}

void StatusPanel::runEnded(Outcome outcome, int warnings)
{
    if (a_runState != RunState::Running) return;
    a_runState = RunState::Ended;
    a_outcome = outcome;
    a_runWarnings = warnings;
    a_runTime = a_runClock.isValid() ? a_runClock.elapsed() : 0;
    a_clock->start(30000);
    updateRun();
    a_row->fit();
}

void StatusPanel::clearRun()
{
    a_runState = RunState::None;
    a_clock->start(30000);
    updateRun();
    a_row->fit();
}

void StatusPanel::updateRun()
{
    a_row->setWanted(a_run, a_runState != RunState::None);
    if (a_runState == RunState::None) return;
    const QString document = a_runDocument.isEmpty() ? tr("The schematic") : QFileInfo(a_runDocument).fileName();
    const QString console = a_runExternal ? tr("Click to show the simulation console")
                                          : tr("Click to show the simulator's messages");
    if (a_runState == RunState::Running) {
        const qint64 seconds = a_runClock.elapsed() / 1000;
        setChip(a_run, seconds > 0 ? tr("Simulating… %1 s").arg(seconds) : tr("Simulating…"), Tone::Busy);
        a_run->setToolTip(tr("%1 is being simulated.").arg(document) + QLatin1Char('\n') + console);
        return;
    }
    const QString time = duration(a_runTime);
    switch (a_outcome) {
    case Outcome::Succeeded:
        setChip(a_run, tr("Simulated in %1").arg(time), Tone::Ok);
        a_run->setToolTip(tr("%1 was simulated in %2.").arg(document, time) + QLatin1Char('\n') + console);
        break;
    case Outcome::Warned:
        setChip(a_run,
                (a_runWarnings > 0 ? plural(a_runWarnings, "1 warning", "%1 warnings") : tr("Warnings"))
                    + QStringLiteral("  ·  ") + time,
                Tone::Warn);
        a_run->setToolTip(tr("%1 was simulated in %2, with warnings.").arg(document, time) + QLatin1Char('\n')
                          + console);
        break;
    case Outcome::Failed:
        setChip(a_run, tr("Simulation failed"), Tone::Error);
        a_run->setToolTip(tr("The simulation of %1 failed.").arg(document) + QLatin1Char('\n') + console);
        break;
    case Outcome::Stopped:
        setChip(a_run, tr("Simulation stopped"), Tone::Warn);
        a_run->setToolTip(tr("The simulation of %1 was stopped.").arg(document) + QLatin1Char('\n') + console);
        break;
    }
}

// ----------------------------------------------------------------------
QString StatusPanel::versionKey(int simulator) const
{
    QString program = programOf(simulator);
    if (!program.isEmpty() && !QFileInfo(program).isAbsolute()) {
        const QString found = QStandardPaths::findExecutable(program);
        if (!found.isEmpty()) program = found;
    }
    return QStringLiteral("%1|%2|%3").arg(simulator).arg(program,
                                                           QFileInfo(program).lastModified().toString(Qt::ISODate));
}

void StatusPanel::askVersion(int simulator)
{
    const QStringList arguments = versionArguments(simulator);
    const QString program = programOf(simulator);
    if (!a_mayAsk || arguments.isEmpty() || program.isEmpty()) return;
    const QString key = versionKey(simulator);
    if (versions().contains(key) || a_asking.contains(key)) return;
    a_asking.insert(key);
    auto* process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setStandardInputFile(QProcess::nullDevice());   // one that would read waits for nothing
    const auto done = [this, process, key, simulator] {
        versions().insert(key, versionIn(simulator, QString::fromLocal8Bit(process->readAll())));
        a_asking.remove(key);
        process->deleteLater();
        scheduleRefresh();
    };
    connect(process, &QProcess::finished, this, done);
    connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) done();   // no finished() follows
    });
    QTimer::singleShot(3000, process, [process] { process->kill(); });   // finished() follows
    process->start(program, arguments);
}

void StatusPanel::updateSimulator()
{
    const QComboBox* combo = a_app->simulatorsCombobox;
    const int simulator = QucsSettings.DefaultSimulator;
    a_row->setWanted(a_simulator, true);
    if (combo == nullptr || combo->count() == 0 || simulator == spicecompat::simNotSpecified) {
        setChip(a_simulator, tr("No simulator"), Tone::Error);
        a_simulator->setToolTip(tr("No simulator was found. Click to set one up."));
        return;
    }
    askVersion(simulator);
    const QString name = spicecompat::getDefaultSimulatorName(simulator);
    const QString version = versions().value(versionKey(simulator));
    const QString shown = version.isEmpty() ? name : name + QLatin1Char(' ') + version;
    setChip(a_simulator, shown, Tone::None);
    const QString program = programOf(simulator);
    a_simulator->setToolTip(tr("Simulator: %1").arg(shown)
                            + (program.isEmpty() ? QString() : QLatin1Char('\n') + QDir::toNativeSeparators(program))
                            + QLatin1Char('\n') + tr("Click to choose another"));
}

QMenu* StatusPanel::simulatorMenu()
{
    auto* menu = new QMenu(a_simulator);
    menu->setObjectName(QStringLiteral("statusSimulatorMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QComboBox* combo = a_app->simulatorsCombobox;
    auto* group = new QActionGroup(menu);
    for (int i = 0; combo != nullptr && i < combo->count(); ++i) {
        QAction* a = menu->addAction(combo->itemText(i));
        a->setCheckable(true);
        a->setChecked(i == combo->currentIndex());
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] {
            QComboBox* c = a_app->simulatorsCombobox;
            if (i < c->count()) {
                c->setCurrentIndex(i);
                a_app->slotChangeSimulator(i);
            }
            scheduleRefresh();
        });
    }
    if (combo == nullptr || combo->count() == 0) menu->addAction(tr("No simulator found"))->setEnabled(false);
    menu->addSeparator();
    if (a_app->simSettings != nullptr) menu->addAction(a_app->simSettings);
    return menu;
}

// ----------------------------------------------------------------------
void StatusPanel::updateSaved()
{
    QWidget* front = a_app->DocumentTab != nullptr ? a_app->DocumentTab->currentWidget() : nullptr;
    QucsDoc* doc = nullptr;
    if (auto* s = qobject_cast<Schematic*>(front)) doc = s;
    else if (auto* t = qobject_cast<TextDoc*>(front)) doc = t;
    if (doc == nullptr) {
        a_row->setWanted(a_saved, false);
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    const bool changed = doc->getDocChanged();
    const QString file = doc->getDocName();
    const QString key = shortcutOf(a_app->fileSave);
    const QString save = key.isEmpty() ? tr("Click to save it") : tr("Click to save it (%1)").arg(key);
    // The autosaved copy; the index names an untitled document's, as
    // QucsApp::autosaveAll() writes it.
    const QDateTime autosaved =
        changed ? qucs_s::autosave::writtenAt(doc, int(a_app->allDocuments().indexOf(doc))) : QDateTime();
    const QString copy = autosaved.isValid() ? tr("autosaved %1").arg(age(autosaved, now)) : QString();
    if (file.isEmpty()) {
        a_row->setWanted(a_saved, changed);
        if (!changed) return;
        setChip(a_saved, copy.isEmpty() ? tr("Not saved") : tr("Not saved") + QStringLiteral("  ·  ") + copy,
                Tone::Warn);
        a_saved->setToolTip(tr("The document has no file yet.") + QLatin1Char('\n') + save);
    } else if (changed) {
        a_row->setWanted(a_saved, true);
        setChip(a_saved, copy.isEmpty() ? tr("Unsaved changes") : tr("Unsaved") + QStringLiteral("  ·  ") + copy,
                Tone::Warn);
        a_saved->setToolTip(tr("%1 has changes that are not saved.").arg(QDir::toNativeSeparators(file))
                            + QLatin1Char('\n') + save);
    } else {
        a_row->setWanted(a_saved, true);
        const QFileInfo info(file);
        if (!info.exists()) {
            setChip(a_saved, tr("Not on disk"), Tone::Error);
            a_saved->setToolTip(tr("%1 is not there any more.").arg(QDir::toNativeSeparators(file)) + QLatin1Char('\n')
                                + save);
        } else {
            const QDateTime when = info.lastModified();
            setChip(a_saved, tr("Saved %1").arg(age(when, now)), Tone::None);
            a_saved->setToolTip(tr("%1\nsaved %2").arg(QDir::toNativeSeparators(file),
                                                       QLocale().toString(when, QLocale::LongFormat)));
        }
    }
}

void StatusPanel::updateTheme()
{
    const int theme = qucs_s::apptheme::current();
    a_theme->setIcon(qucs_s::apptheme::swatch(theme));
    a_theme->setToolTip(tr("Theme: %1").arg(qucs_s::apptheme::name(theme)) + QLatin1Char('\n')
                        + tr("Click to choose another"));
    a_row->setWanted(a_theme, true);
}
