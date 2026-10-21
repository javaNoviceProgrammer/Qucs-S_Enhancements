/*
 * multiphysicspanel.cpp - the Multiphysics panel: the model's tree and the
 *                         settings of the node chosen
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicspanel.h"

#include "fem_lumerical.h"
#include "fem_material_plot.h"
#include "fem_results.h"
#include "fem_solver.h"
#include "ink.h"
#include "main.h"
#include "multiphysicsdoc.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

using namespace qucs_s::fem;

namespace {

constexpr int TagRole = Qt::UserRole + 1;

/// A node's icon: a glyph of its kind, drawn.
QIcon glyph(const QString& icon)
{
    const bool dark = qucs_s::ink::isDark(QApplication::palette().color(QPalette::Base));
    const QString key = QStringLiteral("mp-glyph-%1-%2").arg(icon).arg(dark);
    QPixmap cached;
    if (QPixmapCache::find(key, &cached)) return QIcon(cached);
    const qreal dpr = 2;
    QPixmap pm(int(16 * dpr), int(16 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor green = dark ? QColor(110, 200, 120) : QColor(46, 125, 50);
    const QColor blue = dark ? QColor(110, 170, 240) : QColor(31, 111, 180);
    const QColor orange = dark ? QColor(255, 170, 90) : QColor(211, 84, 0);
    const QColor red = dark ? QColor(240, 110, 100) : QColor(198, 40, 40);
    const QColor gray = dark ? QColor(180, 185, 195) : QColor(95, 100, 110);
    const QColor brown = dark ? QColor(210, 160, 120) : QColor(141, 90, 59);
    const QColor purple = dark ? QColor(185, 150, 235) : QColor(123, 79, 191);
    auto letters = [&](const QString& text, const QColor& c, bool round = false) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        if (round) p.drawEllipse(QRectF(0.5, 0.5, 15, 15));
        else p.drawRoundedRect(QRectF(0.5, 0.5, 15, 15), 3, 3);
        p.setPen(Qt::white);
        QFont f = QApplication::font();
        f.setPixelSize(text.size() > 2 ? 6 : text.size() > 1 ? 8 : 10);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 16, 16), Qt::AlignCenter, text);
    };
    QPen pen(green, 1.4);
    if (icon == QLatin1String("rectangle")) {
        p.setPen(pen);
        p.setBrush(QColor(green.red(), green.green(), green.blue(), 60));
        p.drawRect(QRectF(2, 4, 12, 8));
    } else if (icon == QLatin1String("circle") || icon == QLatin1String("ellipse")) {
        p.setPen(pen);
        p.setBrush(QColor(green.red(), green.green(), green.blue(), 60));
        p.drawEllipse(icon == QLatin1String("circle") ? QRectF(2.5, 2.5, 11, 11) : QRectF(1.5, 4, 13, 8));
    } else if (icon == QLatin1String("polygon")) {
        p.setPen(pen);
        p.setBrush(QColor(green.red(), green.green(), green.blue(), 60));
        p.drawPolygon(QPolygonF{QPointF(2, 13), QPointF(6, 3), QPointF(14, 6), QPointF(12, 13)});
    } else if (icon == QLatin1String("point")) {
        p.setPen(Qt::NoPen);
        p.setBrush(green);
        p.drawEllipse(QPointF(8, 8), 2.5, 2.5);
    } else if (icon == QLatin1String("union") || icon == QLatin1String("difference") || icon == QLatin1String("intersection")) {
        p.setPen(pen);
        p.setBrush(icon == QLatin1String("union") ? QBrush(QColor(green.red(), green.green(), green.blue(), 70)) : QBrush());
        p.drawEllipse(QRectF(1.5, 4, 8, 8));
        p.drawEllipse(QRectF(6.5, 4, 8, 8));
    } else if (icon == QLatin1String("formunion") || icon == QLatin1String("geometry")) {
        p.setPen(pen);
        p.setBrush(QColor(green.red(), green.green(), green.blue(), 60));
        p.drawRect(QRectF(1.5, 5.5, 8, 8));
        p.drawEllipse(QRectF(6.5, 1.5, 8, 8));
    } else if (icon == QLatin1String("move")) letters(QStringLiteral("→"), green);
    else if (icon == QLatin1String("rotate")) letters(QStringLiteral("↻"), green);
    else if (icon == QLatin1String("scale")) letters(QStringLiteral("⤢"), green);
    else if (icon == QLatin1String("mirror")) letters(QStringLiteral("⇋"), green);
    else if (icon == QLatin1String("array")) letters(QStringLiteral("⋯"), green);
    else if (icon == QLatin1String("es")) letters(QStringLiteral("es"), blue, true);
    else if (icon == QLatin1String("ec")) letters(QStringLiteral("ec"), orange, true);
    else if (icon == QLatin1String("ht")) letters(QStringLiteral("ht"), red, true);
    else if (icon == QLatin1String("solid")) letters(QStringLiteral("sm"), dark ? QColor(90, 200, 200) : QColor(0, 121, 121), true);
    else if (icon == QLatin1String("mf")) letters(QStringLiteral("mf"), purple, true);
    else if (icon == QLatin1String("coil")) {
        // Turns of a coil.
        p.setPen(QPen(purple, 1.4));
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 3; ++i) p.drawEllipse(QRectF(2 + 3.5 * i, 4, 5, 8));
    } else if (icon == QLatin1String("magnet")) {
        p.setPen(Qt::NoPen);
        p.setBrush(red);
        p.drawRect(QRectF(1.5, 4, 6.5, 8));
        p.setBrush(blue);
        p.drawRect(QRectF(8, 4, 6.5, 8));
    } else if (icon == QLatin1String("frequency")) letters(QStringLiteral("f"), gray, true);
    else if (icon == QLatin1String("mode")) letters(QStringLiteral("β"), gray, true);
    else if (icon == QLatin1String("emw")) letters(QStringLiteral("emw"), dark ? QColor(240, 150, 210) : QColor(173, 20, 87), true);
    else if (icon == QLatin1String("ewfd")) {
        // A wave.
        p.setPen(QPen(dark ? QColor(240, 150, 210) : QColor(173, 20, 87), 1.6));
        QPainterPath path(QPointF(1, 8));
        for (int i = 0; i < 4; ++i) path.quadTo(QPointF(2.75 + 3.5 * i, i % 2 ? 14 : 2), QPointF(4.5 + 3.5 * i, 8));
        p.drawPath(path);
    } else if (icon == QLatin1String("pml")) {
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 4; ++i) {
            p.setBrush(QColor(blue.red(), blue.green(), blue.blue(), 40 + 50 * i));
            p.drawRect(QRectF(1 + 3.5 * i, 2, 3.5, 12));
        }
    } else if (icon == QLatin1String("semi")) {
        letters(QStringLiteral("pn"), dark ? QColor(230, 190, 110) : QColor(150, 95, 10), true);
    } else if (icon == QLatin1String("gop") || icon == QLatin1String("rayplot")) {
        // Rays brought to a focus by a lens.
        const QColor ray = icon == QLatin1String("rayplot") ? red : (dark ? QColor(255, 210, 90) : QColor(200, 130, 0));
        if (icon == QLatin1String("gop")) {
            p.setPen(QPen(blue, 1.1));
            p.setBrush(QColor(blue.red(), blue.green(), blue.blue(), 50));
            QPainterPath lens(QPointF(6, 2));
            lens.quadTo(QPointF(9.5, 8), QPointF(6, 14));
            lens.quadTo(QPointF(2.5, 8), QPointF(6, 2));
            p.drawPath(lens);
        }
        p.setPen(QPen(ray, 1.2));
        for (double y : {4.0, 8.0, 12.0}) {
            p.drawLine(QPointF(0.5, y), QPointF(6, y));
            p.drawLine(QPointF(6, y), QPointF(15, 8 + (8 - y) * 0.2));
        }
    } else if (icon == QLatin1String("qw")) {
        // A well's band edge with its level and wave.
        const QColor band = dark ? QColor(150, 200, 255) : QColor(25, 90, 170);
        p.setPen(QPen(band, 1.2));
        p.drawPolyline(QPolygonF{QPointF(0.5, 3), QPointF(5, 3), QPointF(5, 13), QPointF(11, 13), QPointF(11, 3), QPointF(15.5, 3)});
        p.setPen(QPen(red, 1.1));
        QPainterPath wave(QPointF(5, 10));
        wave.cubicTo(QPointF(6.5, 5.5), QPointF(9.5, 5.5), QPointF(11, 10));
        p.drawPath(wave);
    } else if (icon == QLatin1String("pde")) letters(QStringLiteral("∂u"), dark ? QColor(170, 210, 120) : QColor(60, 120, 20), true);
    else if (icon == QLatin1String("eigenvalue")) letters(QStringLiteral("λ"), gray, true);
    else if (icon == QLatin1String("states")) letters(QStringLiteral("ψ"), gray, true);
    else if (icon == QLatin1String("spectrum")) letters(QStringLiteral("α"), gray, true);
    else if (icon == QLatin1String("beam")) {
        p.setPen(QPen(dark ? QColor(255, 210, 90) : QColor(200, 130, 0), 1.3));
        for (double y : {4.0, 8.0, 12.0}) {
            p.drawLine(QPointF(2, y), QPointF(13, y));
            p.drawLine(QPointF(13, y), QPointF(10.5, y - 2));
            p.drawLine(QPointF(13, y), QPointF(10.5, y + 2));
        }
    }
    else if (icon.startsWith(QLatin1String("p:"))) letters(icon.mid(2), dark ? QColor(220, 170, 110) : QColor(140, 85, 20));   // a process step
    else if (icon == QLatin1String("crosssection")) {
        // Layers cut across.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(green.red(), green.green(), green.blue(), 90));
        p.drawRect(QRectF(1, 10, 14, 4));
        p.setBrush(green);
        p.drawRect(QRectF(6, 6.5, 4, 3.5));
        p.setBrush(orange);
        p.drawRect(QRectF(4.5, 2, 7, 2.5));
    } else if (icon == QLatin1String("exports")) letters(QStringLiteral("⇪"), gray);
    else if (icon == QLatin1String("touchstone")) letters(QStringLiteral("S"), blue);
    else if (icon == QLatin1String("spice")) letters(QStringLiteral("cir"), orange);
    else if (icon == QLatin1String("qucspart")) letters(QStringLiteral("Q"), green);
    else if (icon == QLatin1String("import")) letters(QStringLiteral("⇩"), green);
    else if (icon == QLatin1String("transient")) letters(QStringLiteral("t"), gray, true);
    else if (icon == QLatin1String("sweep")) letters(QStringLiteral("P"), gray, true);
    else if (icon == QLatin1String("deformation")) letters(QStringLiteral("⤳"), gray);
    else if (icon == QLatin1String("cutline")) {
        p.setPen(QPen(gray, 1.4));
        p.drawLine(QPointF(2, 13), QPointF(14, 3));
        p.setBrush(gray);
        p.drawEllipse(QPointF(2.5, 12.5), 1.6, 1.6);
        p.drawEllipse(QPointF(13.5, 3.5), 1.6, 1.6);
    } else if (icon == QLatin1String("cutpoint")) letters(QStringLiteral("·"), gray);
    else if (icon == QLatin1String("plotgroup1d") || icon == QLatin1String("linegraph") || icon == QLatin1String("globalgraph")
             || icon == QLatin1String("pointgraph")) {
        p.setPen(QPen(gray, 1));
        p.drawLine(QPointF(2, 14), QPointF(14.5, 14));
        p.drawLine(QPointF(2, 14), QPointF(2, 1.5));
        p.setPen(QPen(icon == QLatin1String("plotgroup1d") ? blue : red, 1.5));
        p.drawPolyline(QPolygonF{QPointF(3, 12), QPointF(6, 6), QPointF(9, 9), QPointF(14, 3)});
    }
    else if (icon == QLatin1String("materials") || icon == QLatin1String("material")) letters(QStringLiteral("M"), brown);
    else if (icon == QLatin1String("mesh") || icon == QLatin1String("meshplot")) {
        p.setPen(QPen(purple, 1.1));
        p.drawPolygon(QPolygonF{QPointF(1.5, 14), QPointF(8, 2), QPointF(14.5, 14)});
        p.drawLine(QPointF(4.75, 8), QPointF(11.25, 8));
        p.drawLine(QPointF(4.75, 8), QPointF(8, 14));
        p.drawLine(QPointF(11.25, 8), QPointF(8, 14));
    } else if (icon == QLatin1String("meshsize")) letters(QStringLiteral("h"), purple);
    else if (icon == QLatin1String("meshdistribution")) letters(QStringLiteral("n"), purple);
    else if (icon == QLatin1String("study") || icon == QLatin1String("stationary")) letters(QStringLiteral("="), gray, true);
    else if (icon == QLatin1String("results")) letters(QStringLiteral("R"), gray);
    else if (icon == QLatin1String("plotgroup") || icon == QLatin1String("surface")) {
        QLinearGradient g(0, 0, 16, 0);
        g.setColorAt(0, QColor(30, 60, 200));
        g.setColorAt(0.5, QColor(60, 200, 90));
        g.setColorAt(1, QColor(220, 40, 30));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(1, 3, 14, 10), 2, 2);
    } else if (icon == QLatin1String("contour")) {
        p.setPen(QPen(blue, 1.1));
        p.drawEllipse(QPointF(8, 8), 6.5, 4.5);
        p.drawEllipse(QPointF(8, 8), 3.5, 2.2);
    } else if (icon == QLatin1String("arrow")) {
        p.setPen(QPen(gray, 1.4));
        p.drawLine(QPointF(2, 13), QPointF(13, 3));
        p.drawLine(QPointF(13, 3), QPointF(8, 4));
        p.drawLine(QPointF(13, 3), QPointF(12, 8));
    } else if (icon == QLatin1String("derived") || icon == QLatin1String("global")) letters(QStringLiteral("8.5"), gray);
    else if (icon == QLatin1String("integral")) letters(QStringLiteral("∫"), gray);
    else if (icon == QLatin1String("average")) letters(QStringLiteral("x̄"), gray);
    else if (icon == QLatin1String("maximum")) letters(QStringLiteral("max"), gray);
    else if (icon == QLatin1String("minimum")) letters(QStringLiteral("min"), gray);
    else if (icon == QLatin1String("pointeval")) letters(QStringLiteral("•"), gray);
    else if (icon == QLatin1String("lineparams")) letters(QStringLiteral("Z0"), gray);
    else if (icon == QLatin1String("parameters") || icon == QLatin1String("definitions")) letters(QStringLiteral("Pi"), gray);
    else if (icon == QLatin1String("function")) letters(QStringLiteral("f"), gray);
    else if (icon == QLatin1String("component")) letters(QStringLiteral("2D"), blue);
    else if (icon == QLatin1String("model")) letters(QStringLiteral("◆"), blue);
    else if (icon == QLatin1String("selection")) letters(QStringLiteral("S"), gray);
    else if (icon == QLatin1String("ground")) {
        p.setPen(QPen(blue, 1.4));
        p.drawLine(QPointF(8, 2), QPointF(8, 8));
        p.drawLine(QPointF(2, 8), QPointF(14, 8));
        p.drawLine(QPointF(4.5, 11), QPointF(11.5, 11));
        p.drawLine(QPointF(7, 14), QPointF(9, 14));
    } else if (icon == QLatin1String("terminal")) letters(QStringLiteral("T"), blue);
    else if (icon == QLatin1String("temperature")) letters(QStringLiteral("T"), red);
    else if (icon == QLatin1String("boundaryfeature")) {
        p.setPen(QPen(blue, 2));
        p.drawLine(QPointF(2, 13), QPointF(14, 3));
    } else {   // a domain feature, and anything else
        p.setPen(QPen(blue, 1));
        p.setBrush(QColor(blue.red(), blue.green(), blue.blue(), 70));
        p.drawRect(QRectF(2.5, 2.5, 11, 11));
    }
    p.end();
    QPixmapCache::insert(key, pm);
    return QIcon(pm);
}

QString text(const QJsonValue& v)
{
    if (v.isDouble()) return formatNumber(v.toDouble(), 15);
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

/// A name an expression may use, offered as it is typed: its unit (a
/// parameter's value, a global's where its study has one solution) and
/// what it is.
struct Offered {
    QString name, unit, about;
};

/// What an expression of \a doc's model may name: where \a spatial, its
/// fields' variables (the model's, and a solution's where it has more - a
/// laser's); where \a globals (a result's expression), the globals its
/// studies found - with their values where a study has one solution (its
/// last's would be one of its modes, points or times: their units); its
/// parameters.
QList<Offered> offeredNames(MultiphysicsDoc* doc, bool spatial, bool globals)
{
    QList<Offered> out;
    QSet<QString> seen;
    auto add = [&out, &seen](const QString& name, const QString& unit, const QString& about) {
        if (name.isEmpty() || seen.contains(name)) return;
        seen.insert(name);
        out << Offered{name, unit == QLatin1String("1") ? QString() : unit, about};
    };
    auto unitOf = [](const Variable& v) { return !v.unit.isEmpty() ? v.unit : !v.display.isEmpty() ? v.display : dimName(v.dim); };
    const Model& m = doc->model();
    struct Solved {
        std::shared_ptr<Solution> solution;
        bool stale = false;   // (the model changed since)
        bool one = false;     // (its only solution)
    };
    std::vector<Solved> solved;
    for (const Node* s : m.studies())
        if (std::shared_ptr<Solution> sol = doc->solution(s->tag)) {
            const std::shared_ptr<SolutionSet> set = doc->solutions(s->tag);
            solved.push_back({sol, doc->solutionStale(s->tag), set && instances(*set, true).size() == 1});
        }
    if (spatial) {
        for (const Variable& v : modelVariables(m)) add(v.name, unitOf(v), v.description);
        for (const Solved& s : solved)
            if (!s.stale)
                for (const Variable& v : s.solution->variables()) add(v.name, unitOf(v), v.description);
    }
    if (globals)
        for (const Solved& s : solved)
            for (const Variable& v : s.solution->globals())
                add(v.name, s.one ? QStringLiteral("= ") + s.solution->globalText(v.name) : unitOf(v), v.description);
    const std::shared_ptr<ParameterScope> ps = doc->parameters();
    for (const Model::Parameter& p : m.parameters())
        add(p.name, ps->values.contains(p.name) ? QStringLiteral("= ") + formatQuantity(ps->values.value(p.name), ps->dims.value(p.name)) : QString(),
            p.description.isEmpty() ? QCoreApplication::translate("MultiphysicsPanel", "a parameter") : p.description);
    return out;
}

/// A line edit whose text is checked as an expression as it is typed - and,
/// given what it may name, offers it: after a name and a dot the names that
/// go on from it (emw. lists emw's variables and globals), narrowed as one
/// types; all of them when asked (Ctrl+Space - the Control key on a Mac,
/// the Command key's being Spotlight's). Return or Tab takes the one
/// chosen, Escape closes the list.
class ExpressionEdit : public QLineEdit
{
public:
    ExpressionEdit(const QString& value, std::function<QString(const QString&, bool*)> check, QWidget* parent,
                   std::function<QList<Offered>()> offered = {})
        : QLineEdit(value, parent), a_check(std::move(check)), a_offered(std::move(offered))
    {
        connect(this, &QLineEdit::textChanged, this, [this] { validate(); });
        validate();
        if (!a_offered) return;
        a_names = new QStandardItemModel(this);
        a_completer = new QCompleter(a_names, this);
        a_completer->setWidget(this);
        a_completer->setCompletionMode(QCompleter::PopupCompletion);
        a_completer->setCaseSensitivity(Qt::CaseInsensitive);
        a_completer->setModelSorting(QCompleter::UnsortedModel);
        a_completer->setFilterMode(Qt::MatchStartsWith);
        a_completer->setMaxVisibleItems(12);
        a_completer->setWrapAround(false);
        // (A row for each: the name, its unit or value, what it is.)
        auto* list = new QTreeView;
        list->setObjectName(QStringLiteral("mpCompletions"));
        list->setRootIsDecorated(false);
        list->setHeaderHidden(true);
        list->setUniformRowHeights(true);
        list->setSelectionBehavior(QAbstractItemView::SelectRows);
        list->setTextElideMode(Qt::ElideRight);
        a_completer->setPopup(list);
        connect(a_completer, QOverload<const QModelIndex&>::of(&QCompleter::activated), this,
                [this](const QModelIndex& index) { take(index.siblingAtColumn(0).data(Qt::UserRole).toString()); });
        // (Opened as text is added - a dot typed or pasted -, not as it is
        // deleted; one shown follows either, and the cursor: moved off its
        // word, an arrow key, it goes.)
        a_length = int(text().size());
        connect(this, &QLineEdit::textEdited, this, [this](const QString& now) {
            const bool added = int(now.size()) > a_length;
            a_length = int(now.size());
            offer(false, added);
        });
        connect(this, &QLineEdit::textChanged, this, [this](const QString& now) { a_length = int(now.size()); });
        connect(this, &QLineEdit::cursorPositionChanged, this, [this] {
            if (listing()) offer(false, false);
        });
    }
    void validate()
    {
        if (!a_check) return;   // (its property's tooltip kept)
        bool bad = false;
        const QString said = a_check(text(), &bad);
        setToolTip(said);
        setStyleSheet(bad ? QStringLiteral("QLineEdit { border: 1px solid #c62828; }") : QString());
    }
    bool listing() const { return a_completer != nullptr && a_completer->popup()->isVisible(); }
    /// The names that go on from the word before the cursor - after a
    /// name's dot (not a number's: 1.5), text \a added, or all when \a
    /// asked -, in the list below the box; none, no list.
    void offer(bool asked, bool added = true)
    {
        if (a_completer == nullptr || a_taking || (!asked && !added && !listing())) return;
        const QString before = text().left(cursorPosition());
        qsizetype start = before.size();
        while (start > 0 && (before.at(start - 1).isLetterOrNumber() || before.at(start - 1) == QLatin1Char('_') || before.at(start - 1) == QLatin1Char('.')))
            --start;
        const QString word = before.mid(start);
        const qsizetype dot = word.lastIndexOf(QLatin1Char('.'));
        const QString stem = word.left(dot + 1);   // (emw. - nothing before a dot)
        if (asked || !listing() || int(start) != a_start || stem != a_stem || hasSelectedText()) {
            // (A number's dot - 1.5 - finds none: no name starts with a digit.)
            if (!asked && (dot <= 0 || hasSelectedText())) {
                a_completer->popup()->hide();
                return;
            }
            fill(stem);
            a_start = int(start);
            a_stem = stem;
        }
        a_completer->setCompletionPrefix(word.mid(dot + 1));
        if (a_completer->completionCount() == 0) {
            a_completer->popup()->hide();
            return;
        }
        place();
    }

protected:
    bool event(QEvent* e) override
    {
        // The list's keys while it is shown are its (the completer acts on
        // a key the box leaves): Return and Tab take the name - not the
        // text, nor the next box -, Escape closes it.
        if (e->type() == QEvent::KeyPress && listing() && isListKey(static_cast<QKeyEvent*>(e)->key())) {
            e->ignore();
            return true;
        }
        if (e->type() == QEvent::ShortcutOverride) {
            const auto* key = static_cast<QKeyEvent*>(e);
            if ((listing() && (isListKey(key->key()) || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) || (a_completer && isAsk(key))) {
                e->accept();
                return true;
            }
        }
        return QLineEdit::event(e);
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (a_completer && isAsk(event)) {
            offer(true);
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

private:
    static bool isListKey(int key)
    {
        return key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab || key == Qt::Key_Backtab || key == Qt::Key_Escape;
    }
    static bool isAsk(const QKeyEvent* key)
    {
        const Qt::KeyboardModifiers m = key->modifiers() & ~Qt::KeypadModifier;
        return key->key() == Qt::Key_Space && (m == Qt::ControlModifier || m == Qt::MetaModifier);
    }
    /// The names starting with \a stem, each shown after it.
    void fill(const QString& stem)
    {
        a_names->clear();
        a_names->setColumnCount(3);
        const QColor faint = palette().color(QPalette::PlaceholderText);
        for (const Offered& o : a_offered()) {
            if (!o.name.startsWith(stem, Qt::CaseInsensitive) || o.name.size() == stem.size()) continue;
            auto* name = new QStandardItem(o.name.mid(stem.size()));
            name->setData(o.name, Qt::UserRole);
            auto* unit = new QStandardItem(o.unit);
            auto* about = new QStandardItem(o.about);
            unit->setForeground(faint);
            about->setForeground(faint);
            for (QStandardItem* item : {name, unit, about}) {
                item->setEditable(false);
                item->setToolTip(o.about);
            }
            a_names->appendRow({name, unit, about});
        }
    }
    /// Below the box, as wide as its rows (to 640 px: what each is cut
    /// short, whole in its tooltip), the first chosen.
    void place()
    {
        auto* list = static_cast<QTreeView*>(a_completer->popup());
        const QFontMetrics fm = list->fontMetrics();
        const QAbstractItemModel* shown = a_completer->completionModel();
        int wide[3] = {0, 0, 0};
        for (int r = 0; r < shown->rowCount(); ++r)
            for (int c = 0; c < 3; ++c) wide[c] = std::max(wide[c], fm.horizontalAdvance(shown->index(r, c).data().toString()));
        const int pad = 16;
        list->setColumnWidth(0, wide[0] + pad);
        list->setColumnWidth(1, wide[1] + pad);
        const int all = wide[0] + wide[1] + wide[2] + 3 * pad + list->verticalScrollBar()->sizeHint().width() + 2 * list->frameWidth();
        a_completer->complete(QRect(0, 0, std::clamp(all, width(), std::max(width(), 640)), height()));
        list->setCurrentIndex(shown->index(0, 0));
    }
    /// \a name in place of the word it completes.
    void take(const QString& name)
    {
        if (name.isEmpty()) return;
        a_taking = true;
        setSelection(a_start, cursorPosition() - a_start);
        insert(name);
        a_taking = false;
    }

    std::function<QString(const QString&, bool*)> a_check;
    std::function<QList<Offered>()> a_offered;
    QStandardItemModel* a_names = nullptr;
    QCompleter* a_completer = nullptr;
    int a_start = 0;   // the word's start, the list's stem
    QString a_stem;
    int a_length = 0;   // the text's, as it was
    bool a_taking = false;
};

} // namespace

// ------------------------------------------------------------------ The panel

MultiphysicsPanel::MultiphysicsPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("MultiphysicsPanel"));
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    a_pages = new QStackedWidget(this);
    all->addWidget(a_pages);

    // No model in front: make one, open one.
    a_start = new QWidget(a_pages);
    {
        auto* v = new QVBoxLayout(a_start);
        v->setContentsMargins(10, 10, 10, 10);
        auto* title = new QLabel(tr("<b>Multiphysics</b>"), a_start);
        v->addWidget(title);
        auto* about = new QLabel(tr("A 2D finite element model, in the plane or about an axis: its geometry (drawn, "
                                    "imported from DXF, SVG, GDSII and OASIS, or a layout's cross-section), materials and "
                                    "physics - electrostatics, electric currents, heat at rest and in time, solid mechanics, "
                                    "magnetic fields at rest and in frequency, waveguides' modes and waves with their ports, "
                                    "semiconductors' carriers (drift and diffusion, across heterojunctions) and the light they "
                                    "absorb - photodiodes, electroabsorption modulators -, rays through lenses and prisms, quantum "
                                    "wells' states and their absorption and gain, and PDEs and ODEs of your own - its mesh "
                                    "(adapted too), studies (swept too) and results, lines and terminals as parts of circuits, in a "
                                    "model tree. Open a model (.qfem) to see its tree here."),
                                 a_start);
        about->setWordWrap(true);
        // (Wrapped to the dock's width: its length is not to widen the dock -
        // and narrow the window's documents - nor to make it taller.)
        about->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        about->setMinimumHeight(0);
        v->addWidget(about);
        a_newButton = new QPushButton(tr("New Model"), a_start);
        a_newButton->setObjectName(QStringLiteral("mpNewModel"));
        connect(a_newButton, &QPushButton::clicked, this, &MultiphysicsPanel::newModelRequested);
        auto* open = new QPushButton(tr("Open Model…"), a_start);
        connect(open, &QPushButton::clicked, this, [this] { emit openModelRequested(QString()); });
        auto* buttons = new QHBoxLayout;
        buttons->addWidget(a_newButton);
        buttons->addWidget(open);
        buttons->addStretch(1);
        v->addLayout(buttons);
        v->addWidget(new QLabel(tr("Examples"), a_start));
        a_examples = new QListWidget(a_start);
        a_examples->setObjectName(QStringLiteral("mpExamples"));
        v->addWidget(a_examples, 1);
        v->addWidget(new QLabel(tr("Recent models"), a_start));
        a_recent = new QListWidget(a_start);
        v->addWidget(a_recent, 1);
        for (QListWidget* list : {a_examples, a_recent})
            connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
                emit openModelRequested(item->data(Qt::UserRole).toString());
            });
    }
    a_pages->addWidget(a_start);

    a_splitter = new QSplitter(Qt::Vertical, a_pages);
    a_splitter->setObjectName(QStringLiteral("mpPanelSplitter"));
    a_tree = new QTreeWidget(a_splitter);
    a_tree->setObjectName(QStringLiteral("mpTree"));
    a_tree->setColumnCount(2);
    a_tree->setHeaderHidden(true);
    a_tree->header()->setStretchLastSection(false);
    a_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    a_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    a_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    a_tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    a_tree->installEventFilter(this);
    connect(a_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (a_rebuilding || !item || !a_doc) return;
        a_doc->focusNode(item->data(0, TagRole).toString());
        showSettings();
    });
    connect(a_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = a_tree->itemAt(pos);
        if (!item) return;
        a_tree->setCurrentItem(item);
        if (QMenu* menu = menuFor(item->data(0, TagRole).toString())) {
            menu->exec(a_tree->viewport()->mapToGlobal(pos));
            menu->deleteLater();
        }
    });
    connect(a_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (a_rebuilding || column != 0 || !a_doc) return;
        const QString tag = item->data(0, TagRole).toString();
        const Node* n = a_doc->model().find(tag);
        if (n && item->text(0) != n->name() && !(n->label.isEmpty() && n->kind() && item->text(0).startsWith(n->kind()->title)))
            renameNode(tag, item->text(0));
    });
    a_scroll = new QScrollArea(a_splitter);
    a_scroll->setWidgetResizable(true);
    a_scroll->setFrameShape(QFrame::NoFrame);
    a_form = new QWidget;
    a_form->setObjectName(QStringLiteral("mpSettings"));
    a_scroll->setWidget(a_form);
    a_splitter->addWidget(a_tree);
    a_splitter->addWidget(a_scroll);
    a_splitter->setStretchFactor(0, 1);
    a_splitter->setStretchFactor(1, 1);
    a_splitter->setSizes({500, 420});
    a_pages->addWidget(a_splitter);
    fillStart();
    a_pages->setCurrentWidget(a_start);
}

QString MultiphysicsPanel::userMaterialsFile()
{
    return QDir(QucsSettings.qucsWorkspaceDir.absolutePath()).filePath(QStringLiteral("user_lib/materials.json"));
}

QList<MaterialEntry> MultiphysicsPanel::userMaterials()
{
    if (!QFileInfo::exists(userMaterialsFile())) return {};
    return readMaterials(userMaterialsFile());
}

void MultiphysicsPanel::fillStart()
{
    a_examples->clear();
    const QDir examples(QDir(QucsSettings.ExamplesDir).filePath(QStringLiteral("multiphysics")));
    for (const QFileInfo& f : examples.entryInfoList({QStringLiteral("*.qfem")}, QDir::Files, QDir::Name)) {
        // Its title, and what it is about as its tooltip.
        QFile file(f.absoluteFilePath());
        QJsonObject o;
        if (file.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(file.read(1 << 20)).object();
        const QString title = o.value(QStringLiteral("title")).toString();
        auto* item = new QListWidgetItem(glyph(QStringLiteral("model")), title.isEmpty() ? f.completeBaseName() : title, a_examples);
        item->setData(Qt::UserRole, f.absoluteFilePath());
        item->setToolTip(o.value(QStringLiteral("description")).toString());
    }
    a_recent->clear();
    for (const QString& r : QucsSettings.RecentDocs)
        if (r.endsWith(QLatin1String(".qfem"), Qt::CaseInsensitive) && QFileInfo::exists(r)) {
            auto* item = new QListWidgetItem(glyph(QStringLiteral("model")), QFileInfo(r).fileName(), a_recent);
            item->setData(Qt::UserRole, r);
            item->setToolTip(r);
        }
}

void MultiphysicsPanel::setDocument(MultiphysicsDoc* doc)
{
    if (a_doc == doc) return;
    for (const QMetaObject::Connection& c : a_connections) disconnect(c);
    a_connections.clear();
    a_doc = doc;
    if (!doc) {
        fillStart();
        a_pages->setCurrentWidget(a_start);
        return;
    }
    a_connections << connect(doc, &MultiphysicsDoc::modelChanged, this, [this] {
        rebuildTree();
        scheduleSettings();
    });
    a_connections << connect(doc, &MultiphysicsDoc::stateChanged, this, [this] {
        updateStatusColumn();
        scheduleSettings();
    });
    a_connections << connect(doc, &MultiphysicsDoc::focusChanged, this, [this](const QString& tag) {
        if (currentTag() != tag) selectNode(tag);
    });
    // Closed: let go without comparing it (a QPointer's comparison casts what
    // is by now only a QWidget, which UBSan says is undefined).
    a_connections << connect(doc, &QObject::destroyed, this, [this] {
        for (const QMetaObject::Connection& c : a_connections) disconnect(c);
        a_connections.clear();
        a_doc.clear();
        fillStart();
        a_pages->setCurrentWidget(a_start);
    });
    a_pages->setCurrentWidget(a_splitter);
    a_shownTag.clear();
    rebuildTree();
    selectNode(doc->focusedNode().isEmpty() ? doc->model().geometry().tag : doc->focusedNode());
}

// ------------------------------------------------------------------ The tree

QTreeWidgetItem* MultiphysicsPanel::itemOf(const QString& tag) const
{
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        if ((*it)->data(0, TagRole).toString() == tag) return *it;
        ++it;
    }
    return nullptr;
}

QString MultiphysicsPanel::currentTag() const
{
    return a_tree->currentItem() ? a_tree->currentItem()->data(0, TagRole).toString() : QString();
}

void MultiphysicsPanel::selectNode(const QString& tag)
{
    if (QTreeWidgetItem* item = itemOf(tag)) {
        a_tree->setCurrentItem(item);
        a_tree->scrollToItem(item);
    }
}

void MultiphysicsPanel::addItems(QTreeWidgetItem* parent, const Node& node)
{
    // Those of a kind under one node numbered: Terminal 1, 2.
    QHash<QString, int> ofType, seen;
    for (const Node& c : node.children)
        if (c.label.isEmpty()) ++ofType[c.type];
    for (const Node& c : node.children) {
        auto* item = new QTreeWidgetItem(parent);
        const NodeKind* k = c.kind();
        QString shown = c.name();
        if (c.label.isEmpty() && k && ofType.value(c.type) > 1) shown = k->title + QLatin1Char(' ') + QString::number(++seen[c.type]);
        item->setText(0, shown);
        item->setIcon(0, glyph(k ? k->icon : QString()));
        item->setData(0, TagRole, c.tag);
        Qt::ItemFlags flags = item->flags();
        if (k && !k->fixed) flags |= Qt::ItemIsEditable;
        item->setFlags(flags);
        QStringList tip;
        if (k) tip << k->title;
        if (k && k->property(QStringLiteral("selection")))
            tip << describeSelection(c.selection(), k->property(QStringLiteral("level")) ? levelOf(c.text(QStringLiteral("level"))) : k->selection);
        item->setToolTip(0, tip.join(QLatin1String(": ")));
        if (!c.enabled) {
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        }
        addItems(item, c);
    }
}

void MultiphysicsPanel::rebuildTree()
{
    if (!a_doc) return;
    a_rebuilding = true;
    const QString current = currentTag();
    QSet<QString> collapsed;
    {
        QTreeWidgetItemIterator it(a_tree);
        while (*it) {
            if (!(*it)->isExpanded() && (*it)->childCount() > 0) collapsed.insert((*it)->data(0, TagRole).toString());
            ++it;
        }
    }
    const bool first = a_tree->topLevelItemCount() == 0;
    a_tree->clear();
    const Model& m = a_doc->model();
    auto* root = new QTreeWidgetItem(a_tree);
    root->setText(0, a_doc->getDocName().isEmpty() ? tr("untitled.qfem") : QFileInfo(a_doc->getDocName()).fileName());
    root->setIcon(0, glyph(QStringLiteral("model")));
    root->setData(0, TagRole, m.root.tag);
    addItems(root, m.root);
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        const QString tag = (*it)->data(0, TagRole).toString();
        // Plots and features shut, at first; as they were, after.
        const Node* n = m.find(tag);
        const bool closedFirst = n && (n->type == QLatin1String("plotgroup") || n->type == QLatin1String("study"));
        (*it)->setExpanded(first ? !closedFirst : !collapsed.contains(tag));
        ++it;
    }
    if (QTreeWidgetItem* item = itemOf(current)) a_tree->setCurrentItem(item);
    a_rebuilding = false;
    updateStatusColumn();
}

void MultiphysicsPanel::updateStatusColumn()
{
    if (!a_doc) return;
    // The root: the model's file, as it is named now (saved since, renamed).
    if (QTreeWidgetItem* root = a_tree->topLevelItem(0))
        root->setText(0, a_doc->getDocName().isEmpty() ? tr("untitled.qfem") : QFileInfo(a_doc->getDocName()).fileName());
    const QColor grey = QApplication::palette().color(QPalette::Disabled, QPalette::Text);
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        ++it;
        const QString tag = item->data(0, TagRole).toString();
        const Node* n = a_doc->model().find(tag);
        if (!n) continue;
        QString status;
        QColor color = grey;
        if (n->type == QLatin1String("geometry")) {
            // (A process flow's built in the background: being built.)
            if (a_doc->geometryBeingBuilt()) status = tr("being built");
            else if (a_doc->geometryStale()) status = tr("not built");
            else if (!a_doc->geometry().ok()) {
                status = tr("failed");
                color = QColor(198, 40, 40);
            } else {
                const int n = int(a_doc->geometry().topology->domains.size());
                status = n == 1 ? tr("1 domain") : tr("%1 domains").arg(n);
            }
        } else if (n->type == QLatin1String("mesh")) {
            // (Made by the job running - Build Mesh, or a study whose mesh is
            // old -: being made, not "not built".)
            status = a_doc->meshBeingMade() ? tr("being made")
                     : a_doc->meshStale() || !a_doc->mesh()
                         ? tr("not built")
                         : tr("%1 triangles").arg(QLocale().toString(qlonglong(a_doc->mesh()->triangles.size())));
        } else if (n->type == QLatin1String("study")) {
            if (a_doc->computing() == tag) status = tr("computing");
            else if (a_doc->solution(tag)) status = a_doc->solutionStale(tag) ? tr("old") : tr("solved");
        } else if (!a_doc->geometryStale() && a_doc->geometry().errors.contains(tag)) {
            status = tr("error");
            color = QColor(198, 40, 40);
            item->setToolTip(0, a_doc->geometry().errors.value(tag));
        }
        item->setText(1, status);
        item->setForeground(1, color);
    }
}

QMenu* MultiphysicsPanel::menuFor(const QString& tag)
{
    if (!a_doc) return nullptr;
    const Model& m = a_doc->model();
    const Node* n = m.find(tag);
    if (!n) return nullptr;
    const NodeKind* k = n->kind();
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("mpNodeMenu"));
    // What may be added under it.
    if (k) {
        if (n->type == QLatin1String("materials")) {
            QMenu* lib = menu->addMenu(tr("Add Material from Library"));
            QHash<QString, QMenu*> categories;
            QList<MaterialEntry> entries = userMaterials();
            entries << builtinMaterials();
            for (const MaterialEntry& e : entries) {
                QMenu*& cat = categories[e.category];
                if (!cat) cat = lib->addMenu(e.category);
                QAction* a = cat->addAction(e.name, this, [this, tag, e] { addNode(tag, QStringLiteral("material:") + e.name); });
                a->setToolTip(e.description);
            }
            menu->addAction(glyph(QStringLiteral("material")), tr("Blank Material"), this, [this, tag] { addNode(tag, QStringLiteral("material")); });
        } else if (!k->children.isEmpty()) {
            QMenu* add = n->type == QLatin1String("model") ? menu : menu->addMenu(tr("Add"));
            for (const QString& type : k->children) {
                const NodeKind* ck = nodeKind(type);
                if (!ck || ck->fixed) continue;
                if (ck->unique && n->child(type)) continue;
                add->addAction(glyph(ck->icon), n->type == QLatin1String("model") ? tr("Add %1").arg(ck->title) : ck->title, this,
                               [this, tag, type] { addNode(tag, type); });
            }
        }
        menu->addSeparator();
    }
    // What it does.
    if (n->type == QLatin1String("geometry") || (k && k->group == QLatin1String("geometry")))
        menu->addAction(tr("Build All"), this, [this] {
            // (A process flow's in the background, shown once built.)
            QPointer<MultiphysicsDoc> doc = a_doc;
            a_doc->requestGeometry([doc](bool built) {
                if (doc && built) doc->showGeometry();
            });
        });
    if (n->type == QLatin1String("mesh") || (k && k->group == QLatin1String("mesh"))) menu->addAction(tr("Build Mesh"), a_doc.data(), &MultiphysicsDoc::buildMesh);
    if (n->type == QLatin1String("study")) {
        menu->addAction(tr("Compute"), this, [this, tag] { a_doc->compute(tag); });
        if (a_doc->solution(tag)) menu->addAction(tr("Clear Solutions"), a_doc.data(), &MultiphysicsDoc::clearSolutions);
    }
    if (n->type == QLatin1String("plotgroup")) menu->addAction(tr("Plot"), this, [this, tag] {
            QString why;
            if (!a_doc->showPlotGroup(tag, &why)) QMessageBox::information(this, tr("Plot"), why);
        });
    // (A mode analysis's mode as it is shown: its fields on a grid, to a
    // file - MATLAB, NumPy, CSV.)
    bool modes = false;
    for (const Node* p : m.physics()) modes = modes || p->type == QLatin1String("modes");
    if (n->type == QLatin1String("plotgroup") && modes) {
        QAction* exporting = menu->addAction(tr("Export Mode Profile..."), this, [this, tag] {
            const QString model = a_doc->getDocName();
            const Node* pg = a_doc->model().find(tag);
            const int mode = pg ? pg->integer(QStringLiteral("mode")) : 0;
            const QString base = (model.isEmpty() ? QStringLiteral("mode") : QFileInfo(model).completeBaseName()) +
                                 (mode > 0 ? QStringLiteral("_mode%1").arg(mode) : QStringLiteral("_mode")) + QStringLiteral(".mat");
            const QDir dir = model.isEmpty() ? QDir::home() : QFileInfo(model).absoluteDir();
            const QString path = QFileDialog::getSaveFileName(this, tr("Export Mode Profile"), dir.filePath(base),
                                                              tr("MATLAB, SciPy, Octave, Lumerical™ (*.mat);;NumPy (*.npz);;CSV (*.csv)"));
            if (path.isEmpty()) return;
            QString why;
            const QString said = a_doc->exportModeProfile(tag, path, &why);
            if (said.isEmpty()) QMessageBox::information(this, tr("Export Mode Profile"), why);
        });
        exporting->setObjectName(QStringLiteral("mpExportModeProfile"));
    }
    if (k && k->group == QLatin1String("derived")) menu->addAction(tr("Evaluate"), this, [this, tag] { a_doc->evaluate(tag); });
    if (n->type == QLatin1String("material"))
        menu->addAction(tr("Plot Properties..."), this, [this, tag] {
            if (const Node* mat = a_doc->model().find(tag)) plotProperties(*mat);
        });
    if (n->type == QLatin1String("material"))
        menu->addAction(tr("Add to My Materials"), this, [this, tag] {
            const Node* mat = a_doc->model().find(tag);
            if (!mat) return;
            QList<MaterialEntry> mine = userMaterials();
            const MaterialEntry e = entryOf(*mat);
            mine.erase(std::remove_if(mine.begin(), mine.end(), [&](const MaterialEntry& x) { return x.name == e.name; }), mine.end());
            mine << e;
            QDir().mkpath(QFileInfo(userMaterialsFile()).absolutePath());
            QString why;
            if (!writeMaterials(userMaterialsFile(), mine, &why)) QMessageBox::warning(this, tr("My Materials"), why);
        });
    if (!menu->actions().isEmpty() && !menu->actions().last()->isSeparator()) menu->addSeparator();
    const bool editable = k && !k->fixed;
    if (editable) {
        menu->addAction(tr("Rename"), this, [this, tag] {
            if (QTreeWidgetItem* item = itemOf(tag)) a_tree->editItem(item, 0);
        });
        if (!n->isDefault) menu->addAction(tr("Duplicate"), this, [this, tag] { duplicateNode(tag); });
        menu->addAction(n->enabled ? tr("Disable") : tr("Enable"), this, [this, tag, on = !n->enabled] { setNodeEnabled(tag, on); });
        menu->addAction(tr("Move Up"), this, [this, tag] { moveNode(tag, -1); });
        menu->addAction(tr("Move Down"), this, [this, tag] { moveNode(tag, 1); });
        if (!n->isDefault) {
            QAction* del = menu->addAction(tr("Delete"), this, [this, tag] { removeNode(tag); });
            del->setShortcut(QKeySequence::Delete);
        }
    }
    return menu;
}

QString MultiphysicsPanel::addNode(const QString& parentTag, const QString& type)
{
    if (!a_doc) return {};
    Model m = a_doc->model();
    Node node;
    QString what;
    if (type.startsWith(QLatin1String("material:"))) {
        const QString name = type.mid(9);
        std::optional<MaterialEntry> e = findMaterial(name, userMaterials());
        if (!e) return {};
        node = materialNode(m, *e);
        what = tr("Add %1").arg(e->name);
    } else {
        node = m.make(type);
        const NodeKind* k = node.kind();
        if (!k) return {};
        what = tr("Add %1").arg(k->title);
        // A study's step solves its physics; a plot group plots the first study.
        if (type == QLatin1String("plotgroup")) {
            const std::vector<const Node*> studies = m.studies();
            if (!studies.empty()) node.props.insert(QStringLiteral("study"), studies.front()->tag);
        }
    }
    QString why;
    Node* added = m.add(parentTag, node, -1, &why);
    if (!added) {
        QMessageBox::information(this, tr("Add"), why);
        return {};
    }
    const QString tag = added->tag;
    a_doc->setModel(m, what);
    selectNode(tag);
    // A new node with a selection to pick: picked in the Graphics view.
    const Node* fresh = a_doc->model().find(tag);
    if (fresh && fresh->kind() && fresh->kind()->property(QStringLiteral("selection"))
        && fresh->selection().contains(QStringLiteral("numbers")) && fresh->selection().value(QStringLiteral("numbers")).toArray().isEmpty()) {
        a_doc->setPicking(true);
        if (a_pick) a_pick->setChecked(true);
    }
    return tag;
}

bool MultiphysicsPanel::removeNode(const QString& tag)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    const Node* n = m.find(tag);
    if (!n) return false;
    const QString name = n->name();
    QString why;
    if (!m.remove(tag, &why)) {
        QMessageBox::information(this, tr("Delete"), why);
        return false;
    }
    a_doc->setModel(m, tr("Delete %1").arg(name));
    return true;
}

bool MultiphysicsPanel::duplicateNode(const QString& tag)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    int index = -1;
    Node* parent = m.parentOf(tag, &index);
    if (!parent) return false;
    Node copy = parent->children[std::size_t(index)];
    const NodeKind* k = copy.kind();
    if (!k || k->fixed || copy.isDefault) return false;
    // New tags throughout; a geometry object a name of its own.
    std::function<void(Node&)> clearTags = [&](Node& n) {
        n.tag.clear();
        for (Node& c : n.children) clearTags(c);
    };
    clearTags(copy);
    if (k->group != QLatin1String("geometry")) copy.label = copy.label.isEmpty() ? QString() : copy.label + tr(" (copy)");
    Node* added = m.add(parent->tag, copy, index + 1);
    if (!added) return false;
    const QString newTag = added->tag;
    a_doc->setModel(m, tr("Duplicate %1").arg(parent->children[std::size_t(index)].name()));
    selectNode(newTag);
    return true;
}

bool MultiphysicsPanel::moveNode(const QString& tag, int by)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    int index = -1;
    Node* parent = m.parentOf(tag, &index);
    if (!parent) return false;
    const int to = index + by;
    if (to < 0 || to >= int(parent->children.size())) return false;
    // Not past a fixed node (Form Union stays last).
    const NodeKind* other = parent->children[std::size_t(to)].kind();
    if (other && other->fixed) return false;
    std::swap(parent->children[std::size_t(index)], parent->children[std::size_t(to)]);
    a_doc->setModel(m, by < 0 ? tr("Move Up") : tr("Move Down"));
    selectNode(tag);
    return true;
}

bool MultiphysicsPanel::setNodeEnabled(const QString& tag, bool enabled)
{
    if (!a_doc) return false;
    return a_doc->editNode(tag, [enabled](Node& n) { n.enabled = enabled; }, enabled ? tr("Enable") : tr("Disable"));
}

bool MultiphysicsPanel::renameNode(const QString& tag, const QString& label)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    Node* n = m.find(tag);
    if (!n) return false;
    const QString name = label.trimmed();
    const NodeKind* k = n->kind();
    if (k && k->group == QLatin1String("geometry") && n->type != QLatin1String("form union")) {
        // A geometry object: a name, and every rule that names it renamed.
        static const QRegularExpression ok(QStringLiteral("^[A-Za-z_][A-Za-z0-9_ .-]*$"));
        if (name.isEmpty() || !ok.match(name).hasMatch() || (m.objectNames().contains(name) && name != n->label)) {
            QMessageBox::information(this, tr("Rename"), tr("%1 cannot be its name: a name of its own, letters and digits.").arg(name));
            rebuildTree();
            return false;
        }
        const QString old = n->label;
        n->label = name;
        m.renameObject(old, name);
    } else {
        n->label = name == (k ? k->title : QString()) ? QString() : name;
    }
    a_doc->setModel(m, tr("Rename"));
    return true;
}

bool MultiphysicsPanel::eventFilter(QObject* watched, QEvent* event)
{
    // (An item's name being edited: its editor has the keys, not the tree.)
    if (watched == a_tree && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const QString tag = currentTag();
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && !tag.isEmpty()) {
            removeNode(tag);
            return true;
        }
        if (key->key() == Qt::Key_F2 && !tag.isEmpty()) {
            if (QTreeWidgetItem* item = itemOf(tag)) a_tree->editItem(item, 0);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ------------------------------------------------------------------ The settings

void MultiphysicsPanel::scheduleSettings()
{
    if (a_settingsPending) return;
    a_settingsPending = true;
    QTimer::singleShot(0, this, [this] {
        a_settingsPending = false;
        showSettings();
    });
}

bool MultiphysicsPanel::visible(const Node& node, const PropertyDef& p) const
{
    if (p.showIf.isEmpty()) return true;
    // Alternatives split by |, each conditions that all hold, split by &.
    for (const QString& alternative : p.showIf.split(QLatin1Char('|'))) {
        bool all = true;
        for (const QString& cond : alternative.split(QLatin1Char('&'))) {
            const bool negate = cond.contains(QLatin1String("!="));
            const QStringList kv = cond.split(negate ? QStringLiteral("!=") : QStringLiteral("="));
            if (kv.size() != 2) continue;
            const bool equal = node.text(kv[0].trimmed()) == kv[1].trimmed();
            all = all && (negate ? !equal : equal);
        }
        if (all) return true;
    }
    return false;
}

void MultiphysicsPanel::commit(const QString& key, const QJsonValue& value, const QString& what)
{
    if (!a_doc || a_shownTag.isEmpty()) return;
    const Node* n = a_doc->model().find(a_shownTag);
    if (!n || n->value(key) == value) return;
    a_focusKey = key;
    const NodeKind* k = n->kind();
    const PropertyDef* p = k ? k->property(key) : nullptr;
    a_doc->editNode(a_shownTag, [key, value](Node& node) { node.props.insert(key, value); },
                    what.isEmpty() ? tr("Change %1").arg(p ? p->label : key) : what);
}

void MultiphysicsPanel::showSettings()
{
    if (!a_doc) return;
    const QString tag = currentTag();
    const Model& m = a_doc->model();
    const Node* n = m.find(tag);
    // The editor with the focus keeps it when only its value came back.
    QWidget* focused = QApplication::focusWidget();
    const bool editingHere = focused && a_form->isAncestorOf(focused) && tag == a_shownTag;
    if (editingHere && (qobject_cast<QLineEdit*>(focused) || qobject_cast<QTableWidget*>(focused) || focused->parent()->inherits("QTableWidget")))
        return;   // (its commit rebuilds the form when it has ended)
    a_shownTag = tag;
    a_editors.clear();
    a_pick = nullptr;
    delete a_form;
    a_form = new QWidget;
    a_form->setObjectName(QStringLiteral("mpSettings"));
    auto* v = new QVBoxLayout(a_form);
    v->setContentsMargins(8, 6, 8, 6);
    if (!n) {
        a_scroll->setWidget(a_form);
        return;
    }
    const NodeKind* k = n->kind();
    auto* title = new QLabel(QStringLiteral("<b>%1</b> <span style='color:gray'>(%2)</span>")
                                 .arg((k ? k->title : n->type).toHtmlEscaped(), n->tag.toHtmlEscaped()),
                             a_form);
    v->addWidget(title);
    if (k && !k->help.isEmpty()) {
        auto* help = new QLabel(k->help, a_form);
        help->setWordWrap(true);
        QPalette pal = help->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::Disabled, QPalette::WindowText));
        help->setPalette(pal);
        v->addWidget(help);
    }
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft);
    if (k && !k->fixed && n->type != QLatin1String("model")) {
        const bool object = k->group == QLatin1String("geometry");
        auto* label = new QLineEdit(object ? n->label : n->name(), a_form);
        label->setObjectName(QStringLiteral("mpLabel"));
        connect(label, &QLineEdit::editingFinished, this, [this, tag, label] {
            const Node* now = a_doc ? a_doc->model().find(tag) : nullptr;
            if (now && label->text() != now->name() && label->text() != now->label) renameNode(tag, label->text());
        });
        form->addRow(object ? tr("Object name") : tr("Label"), label);
        a_editors.insert(QStringLiteral("label"), label);
    }
    if (n->type == QLatin1String("model")) {
        auto* title2 = new QLineEdit(n->label, a_form);
        connect(title2, &QLineEdit::editingFinished, this, [this, tag, title2] {
            a_doc->editNode(tag, [t = title2->text()](Node& node) { node.label = t; }, tr("Change Title"));
        });
        form->addRow(tr("Title"), title2);
    }
    if (k)
        for (const PropertyDef& p : k->properties) {
            if (!visible(*n, p)) continue;
            QWidget* editor = makeEditor(*n, p);
            if (!editor) continue;
            editor->setToolTip(editor->toolTip().isEmpty() ? p.tooltip : editor->toolTip());
            a_editors.insert(p.key, editor);
            if (p.kind == PropertyDef::Selection || p.kind == PropertyDef::Rows || p.kind == PropertyDef::Points) {
                auto* caption = new QLabel(QStringLiteral("<b>%1</b>").arg(p.label.toHtmlEscaped()), a_form);
                form->addRow(caption);
                form->addRow(editor);
            } else {
                auto* label = new QLabel(p.label, a_form);
                label->setToolTip(p.tooltip);
                label->setWordWrap(true);
                form->addRow(label, editor);
            }
        }
    v->addLayout(form);
    if (n->type == QLatin1String("materials")) v->addWidget(materialsLibrary(*n));
    addActions(v, *n);
    v->addStretch(1);
    a_scroll->setWidget(a_form);
    if (QWidget* again = a_editors.value(a_focusKey)) {
        // Back to where the user was.
        again->setFocus();
        a_focusKey.clear();
    }
}

QWidget* MultiphysicsPanel::makeEditor(const Node& node, const PropertyDef& p)
{
    const QString key = p.key;
    switch (p.kind) {
    case PropertyDef::Expression: {
        // Checked as typed: its value and unit in the tooltip.
        QPointer<MultiphysicsDoc> doc = a_doc;
        const bool spatial = p.spatial;
        const Dim expected = p.dim();
        const bool length = p.unit == QLatin1String("length");
        // (A material's - a wave feature's - property split by ';': a
        // tensor's items.)
        const bool tensorial = node.type == QLatin1String("material") || node.type.endsWith(QLatin1String("/wave"));
        auto check = [doc, spatial, expected, length, tensorial](const QString& text, bool* bad) -> QString {
            if (!doc) return {};
            if (text.trimmed().isEmpty()) return QCoreApplication::translate("MultiphysicsPanel", "not given");
            const std::shared_ptr<ParameterScope> ps = doc->parameters();
            Scope local(&ps->scope);
            if (spatial)
                for (const Variable& v : modelVariables(doc->model())) local.addVariable(v.name, v.dim);
            // (A material's texts read T, the temperature they are at - a
            // heat physics' field, else checked at 300 K.)
            if (local.variable(QStringLiteral("T")) < 0 && !local.constant(QStringLiteral("T")))
                local.setConstant(QStringLiteral("T"), 300, Dim::temperature());
            // (Items split by ';': a PDE's, one for each variable - each checked.)
            const QStringList items = text.split(QLatin1Char(';'));
            for (int i = 0; items.size() > 1 && i < items.size(); ++i) {
                const Expression part = Expression::compile(items[i].trimmed().isEmpty() ? QStringLiteral("0") : items[i], local);
                if (!part.isValid()) {
                    *bad = !(spatial && items[i].contains(QLatin1Char('.')));
                    return part.error();
                }
            }
            if (items.size() > 1 && tensorial) {
                QStringList values;
                for (const QString& item : items) {
                    const Expression part = Expression::compile(item, local);
                    values << (part.isConstant() ? formatQuantity(part.constant(), part.dim().isNone() ? expected : part.dim())
                                                 : QCoreApplication::translate("MultiphysicsPanel", "varies"));
                }
                const int n = int(items.size());
                if (n != 3 && n != 6 && n != 9) {
                    *bad = true;
                    return QCoreApplication::translate("MultiphysicsPanel",
                                                       "%1 items: a tensor has 3 (along x, y, z - a material's, its crystal's axes), 6 "
                                                       "(xx; yy; zz; yz; xz; xy) or 9 (row by row)")
                        .arg(n);
                }
                return (n == 3 ? QCoreApplication::translate("MultiphysicsPanel", "along x, y, z (a material's: its crystal's axes, turned by its orientation): %1")
                        : n == 6 ? QCoreApplication::translate("MultiphysicsPanel", "symmetric, xx; yy; zz; yz; xz; xy: %1")
                                 : QCoreApplication::translate("MultiphysicsPanel", "row by row: %1"))
                    .arg(values.join(QLatin1String(", ")));
            }
            if (items.size() > 1) return QCoreApplication::translate("MultiphysicsPanel", "%1 items, one for each variable").arg(items.size());
            const Expression e = Expression::compile(text, local);
            if (!e.isValid()) {
                // A global (es.C11) is known once solved.
                *bad = !(spatial && text.contains(QLatin1Char('.')));
                return e.error();
            }
            QString said;
            if (e.isConstant()) {
                const Dim d = e.dim().isNone() && !expected.isNone() ? expected : e.dim();
                said = length && e.dim().isNone() ? QCoreApplication::translate("MultiphysicsPanel", "= %1 (the geometry's unit)").arg(formatNumber(e.constant(), 6))
                                                  : QStringLiteral("= %1").arg(formatQuantity(e.constant(), d));
            } else {
                said = QCoreApplication::translate("MultiphysicsPanel", "varies; in %1").arg(dimName(e.dim()));
            }
            if (!e.dim().isNone() && !(e.dim() == expected) && !expected.isNone())
                said += QCoreApplication::translate("MultiphysicsPanel", " - but %1 is expected").arg(dimName(expected));
            if (!e.warnings().isEmpty()) said += QStringLiteral("\n") + e.warnings().join(QLatin1Char('\n'));
            return said;
        };
        // A semiconductor feature's parameter not set on it: its material's
        // where it gives one (shown, not set: typed, it is the feature's).
        QString materialName;
        // (As built: not built here, as the form is made.)
        const QString inherited = a_doc && a_doc->builtGeometry().topology
                                      ? inheritedParameter(a_doc->model(), *a_doc->builtGeometry().topology, node, key, &materialName)
                                      : QString();
        // The names it may use offered as it is typed (emw. lists emw's):
        // a result's - a plot's, a derived value's - the globals too.
        const NodeKind* kind = node.kind();
        const bool result = kind
                            && QStringList{QStringLiteral("plot"), QStringLiteral("graph"), QStringLiteral("derived"), QStringLiteral("dataset"),
                                           QStringLiteral("export"), QStringLiteral("results")}
                                   .contains(kind->group);
        auto offered = [doc, spatial, result] { return doc ? offeredNames(doc, spatial, result) : QList<Offered>(); };
        auto* edit = new ExpressionEdit(inherited.isEmpty() ? node.text(key) : QString(), check, a_form, offered);
        edit->setObjectName(QStringLiteral("mpEdit_") + key);
        if (!inherited.isEmpty()) edit->setPlaceholderText(tr("%1 (%2's)").arg(inherited, materialName));
        else if (p.unit == QLatin1String("length")) edit->setPlaceholderText(tr("in the geometry's unit, or with one: 3[mm]"));
        else if (!p.unit.isEmpty() && p.unit != QLatin1String("angle")) edit->setPlaceholderText(p.unit);
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text().trimmed()); });
        return edit;
    }
    case PropertyDef::Pair: {
        auto* w = new QWidget(a_form);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        const QStringList values = node.pair(key);
        QLineEdit* x = new QLineEdit(values.value(0), w);
        QLineEdit* y = new QLineEdit(values.value(1), w);
        x->setObjectName(QStringLiteral("mpEdit_") + key + QStringLiteral("_x"));
        y->setObjectName(QStringLiteral("mpEdit_") + key + QStringLiteral("_y"));
        x->setPlaceholderText(QStringLiteral("x"));
        y->setPlaceholderText(QStringLiteral("y"));
        h->addWidget(x);
        h->addWidget(y);
        auto done = [this, key, x, y] { commit(key, QJsonArray{x->text().trimmed(), y->text().trimmed()}); };
        connect(x, &QLineEdit::editingFinished, this, done);
        connect(y, &QLineEdit::editingFinished, this, done);
        return w;
    }
    case PropertyDef::Text:
    case PropertyDef::Terminal: {
        // (A result's expressions of the globals - a Global Evaluation's, a
        // global graph's and its x -: the globals and parameters offered as
        // typed, emw. listing emw's.)
        const NodeKind* kind = node.kind();
        const bool ofGlobals = p.kind == PropertyDef::Text && kind
                               && (kind->group == QLatin1String("derived") || kind->group == QLatin1String("graph"))
                               && (key == QLatin1String("expression") || key == QLatin1String("x"));
        QPointer<MultiphysicsDoc> doc = a_doc;
        QLineEdit* edit = ofGlobals ? new ExpressionEdit(node.text(key), {}, a_form,
                                                         [doc] { return doc ? offeredNames(doc, false, true) : QList<Offered>(); })
                                    : new QLineEdit(node.text(key), a_form);
        edit->setObjectName(QStringLiteral("mpEdit_") + key);
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text()); });
        return edit;
    }
    case PropertyDef::Choice: {
        auto* box = new QComboBox(a_form);
        box->setObjectName(QStringLiteral("mpEdit_") + key);
        for (int i = 0; i < p.choices.size(); ++i) box->addItem(p.choiceLabels.value(i, p.choices[i]), p.choices[i]);
        box->setCurrentIndex(std::max(0, box->findData(node.text(key))));
        connect(box, &QComboBox::activated, this, [this, key, box](int) { commit(key, box->currentData().toString()); });
        return box;
    }
    case PropertyDef::Bool: {
        auto* check = new QCheckBox(a_form);
        check->setObjectName(QStringLiteral("mpEdit_") + key);
        check->setChecked(node.flag(key));
        connect(check, &QCheckBox::toggled, this, [this, key](bool on) { commit(key, on); });
        return check;
    }
    case PropertyDef::Integer: {
        auto* spin = new QSpinBox(a_form);
        spin->setObjectName(QStringLiteral("mpEdit_") + key);
        spin->setRange(0, 1000000);
        spin->setValue(node.integer(key));
        connect(spin, &QSpinBox::editingFinished, this, [this, key, spin] { commit(key, spin->value()); });
        return spin;
    }
    case PropertyDef::Selection: return selectionEditor(node, p);
    case PropertyDef::Rows: return parametersEditor(node, p);
    case PropertyDef::Points: return pointsEditor(node, p);
    case PropertyDef::Objects: {
        auto* w = new QWidget(a_form);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(node.list(key).join(QLatin1String(", ")), w);
        edit->setObjectName(QStringLiteral("mpEdit_") + key);
        edit->setPlaceholderText(tr("objects' names, split by commas"));
        auto* pickObjects = new QToolButton(w);
        pickObjects->setText(QStringLiteral("…"));
        pickObjects->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(pickObjects);
        const QStringList chosen = node.list(key);
        for (const QString& name : a_doc->model().objectNames(node.tag)) {
            QAction* a = menu->addAction(name);
            a->setCheckable(true);
            a->setChecked(chosen.contains(name));
            connect(a, &QAction::toggled, this, [this, key, name](bool on) {
                const Node* now = a_doc ? a_doc->model().find(a_shownTag) : nullptr;
                if (!now) return;
                QStringList list = now->list(key);
                if (on && !list.contains(name)) list << name;
                if (!on) list.removeAll(name);
                commit(key, QJsonArray::fromStringList(list));
            });
        }
        pickObjects->setMenu(menu);
        h->addWidget(edit, 1);
        h->addWidget(pickObjects);
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] {
            QStringList list;
            for (const QString& s : edit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) list << s.trimmed();
            commit(key, QJsonArray::fromStringList(list));
        });
        return w;
    }
    case PropertyDef::Physics: {
        auto* list = new QListWidget(a_form);
        list->setObjectName(QStringLiteral("mpEdit_") + key);
        const QStringList chosen = node.list(key);
        for (const Node* ph : a_doc->model().physics()) {
            auto* item = new QListWidgetItem(QStringLiteral("%1 (%2)").arg(ph->name(), ph->tag), list);
            item->setData(Qt::UserRole, ph->tag);
            item->setCheckState(chosen.isEmpty() || chosen.contains(ph->tag) ? Qt::Checked : Qt::Unchecked);
        }
        list->setMaximumHeight(std::min(120, 22 * (list->count() + 1)));
        connect(list, &QListWidget::itemChanged, this, [this, key, list] {
            QStringList on;
            bool all = true;
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->checkState() == Qt::Checked) on << list->item(i)->data(Qt::UserRole).toString();
                else all = false;
            }
            commit(key, all ? QJsonArray() : QJsonArray::fromStringList(on));
        });
        return list;
    }
    case PropertyDef::Study: {
        auto* box = new QComboBox(a_form);
        box->setObjectName(QStringLiteral("mpEdit_") + key);
        box->addItem(tr("(the first)"), QString());
        for (const Node* s : a_doc->model().studies()) box->addItem(s->name(), s->tag);
        box->setCurrentIndex(std::max(0, box->findData(node.text(key))));
        connect(box, &QComboBox::activated, this, [this, key, box](int) { commit(key, box->currentData().toString()); });
        return box;
    }
    case PropertyDef::File: {
        auto* w = new QWidget(a_form);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(node.text(key), w);
        edit->setObjectName(QStringLiteral("mpEdit_") + key);
        const bool layout = node.type == QLatin1String("crosssection");
        const bool written = node.kind() && node.kind()->group == QLatin1String("export");
        const bool script = written && node.type.startsWith(QLatin1String("lumerical"));
        const bool profile = node.type == QLatin1String("overlap");
        edit->setPlaceholderText(layout    ? tr("a GDSII or OASIS layout, beside the model")
                                 : written ? tr("empty: model_%1 beside the model").arg(node.tag)
                                 : profile ? tr("a mode profile (.mat) a Mode Profile export wrote, beside the model")
                                           : tr("a DXF, SVG, GDSII or OASIS file, beside the model"));
        auto* browse = new QToolButton(w);
        browse->setText(QStringLiteral("…"));
        browse->setObjectName(QStringLiteral("mpBrowse_") + key);
        h->addWidget(edit, 1);
        h->addWidget(browse);
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text().trimmed()); });
        connect(browse, &QToolButton::clicked, this, [this, key, edit, layout, written, script, profile] {
            const QString model = a_doc ? a_doc->getDocName() : QString();
            const QDir dir = model.isEmpty() ? QDir::home() : QFileInfo(model).absoluteDir();
            const QString layouts = tr("Layouts (*.gds *.gds2 *.gdsii *.gds.gz *.oas *.oasis)");
            const QString chosen =
                written ? QFileDialog::getSaveFileName(this, tr("Export to"), dir.absolutePath(),
                                                       script ? tr("Lumerical™ scripts (*.lsf);;All files (*)") : tr("All files (*)"))
                : profile ? QFileDialog::getOpenFileName(this, tr("A Mode Profile"), dir.absolutePath(), tr("Mode profiles (*.mat);;All files (*)"))
                : layout ? QFileDialog::getOpenFileName(this, tr("A Layout"), dir.absolutePath(), layouts + tr(";;All files (*)"))
                         : QFileDialog::getOpenFileName(this, tr("Import a Drawing or a Layout"), dir.absolutePath(),
                                                        tr("Drawings and layouts (*.dxf *.svg *.gds *.gds2 *.gdsii *.gds.gz *.oas *.oasis);;"
                                                           "DXF (*.dxf);;SVG (*.svg);;") + layouts + tr(";;All files (*)"));
            if (chosen.isEmpty()) return;
            // Relative to the model, when it is saved: the two move together.
            const QString relative = dir.relativeFilePath(chosen);
            const QString value = model.isEmpty() ? chosen : relative;
            edit->setText(value);
            commit(key, value);
        });
        return w;
    }
    case PropertyDef::Instant:
    case PropertyDef::SweepPoint: {
        // Of the study's solutions, as computed.
        auto* box = new QComboBox(a_form);
        box->setObjectName(QStringLiteral("mpEdit_") + key);
        const std::shared_ptr<SolutionSet> set = a_doc ? a_doc->solutions(node.text(QStringLiteral("study"))) : nullptr;
        if (p.kind == PropertyDef::SweepPoint) {
            box->addItem(tr("(the last)"), 0);
            if (set && set->solutions.size() > 1)
                for (std::size_t i = 0; i < set->solutions.size(); ++i)
                    box->addItem(set->solutions[i]->label.isEmpty() ? tr("Solution %1").arg(i + 1) : set->solutions[i]->label, int(i) + 1);
            const int at = box->findData(node.integer(key));
            box->setCurrentIndex(std::max(0, at));
            if (at < 0 && node.integer(key) > 0) box->addItem(tr("Solution %1 (not computed)").arg(node.integer(key)), node.integer(key));
            connect(box, &QComboBox::activated, this, [this, key, box](int) { commit(key, box->currentData().toInt()); });
        } else {
            box->setEditable(true);
            box->addItem(tr("(the last)"), QString());
            // The solution's times, or (a "frequency") its frequencies.
            const bool frequency = key == QLatin1String("frequency");
            if (set && !set->solutions.empty()) {
                const Solution& s = *set->solutions.front();
                if (s.frequencyDomain == frequency)
                    for (int k = 0; k < s.snapshotCount(); ++k)
                        box->addItem(formatQuantity(s.snapshotValue(k), s.snapshotDim()), QString::number(s.snapshotValue(k), 'g', 17));
            }
            const QString now = node.text(key);
            const int at = box->findData(now);
            if (at >= 0) box->setCurrentIndex(at);
            else if (now.isEmpty()) box->setCurrentIndex(0);
            else box->setEditText(now);
            connect(box, &QComboBox::activated, this, [this, key, box](int) { commit(key, box->currentData().toString()); });
            connect(box->lineEdit(), &QLineEdit::editingFinished, this, [this, key, box] {
                const QString text = box->currentText().trimmed();
                const int at = box->findText(text);
                commit(key, at >= 0 ? box->itemData(at).toString() : text);
            });
        }
        if (!set) box->setToolTip(tr("Compute the study to choose among its solutions"));
        return box;
    }
    case PropertyDef::Dataset: {
        auto* box = new QComboBox(a_form);
        box->setObjectName(QStringLiteral("mpEdit_") + key);
        box->addItem(tr("(none)"), QString());
        for (const Node& r : a_doc->model().results().children)
            if (r.type == QLatin1String("cutline") || r.type == QLatin1String("cutpoint"))
                box->addItem(QStringLiteral("%1 (%2)").arg(r.name(), r.tag), r.tag);
        box->setCurrentIndex(std::max(0, box->findData(node.text(key))));
        connect(box, &QComboBox::activated, this, [this, key, box](int) { commit(key, box->currentData().toString()); });
        return box;
    }
    case PropertyDef::Reference: {
        // A node of the kinds it names, anywhere in the results.
        auto* box = new QComboBox(a_form);
        box->setObjectName(QStringLiteral("mpEdit_") + key);
        box->addItem(tr("(none)"), QString());
        std::function<void(const Node&)> walk = [&](const Node& n) {
            if (p.choices.contains(n.type)) box->addItem(QStringLiteral("%1 (%2)").arg(n.name(), n.tag), n.tag);
            for (const Node& c : n.children) walk(c);
        };
        walk(a_doc->model().results());
        const int at = box->findData(node.text(key));
        if (at < 0 && !node.text(key).isEmpty()) box->addItem(tr("%1 (not in the model)").arg(node.text(key)), node.text(key));
        box->setCurrentIndex(std::max(0, box->findData(node.text(key))));
        connect(box, &QComboBox::activated, this, [this, key, box](int) { commit(key, box->currentData().toString()); });
        return box;
    }
    }
    return nullptr;
}

QWidget* MultiphysicsPanel::selectionEditor(const Node& node, const PropertyDef& p)
{
    const NodeKind* k = node.kind();
    Level level = p.level != Level::None ? p.level : k ? k->selection : Level::Domain;
    if (k && k->property(QStringLiteral("level"))) level = levelOf(node.text(QStringLiteral("level")));
    const QJsonObject rule = node.selection();
    auto* w = new QWidget(a_form);
    w->setObjectName(QStringLiteral("mpSelection"));
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    // The kinds of rule this level has.
    struct Kind {
        QString key, label;
    };
    QList<Kind> kinds = {{QStringLiteral("numbers"), level == Level::Domain ? tr("Domains chosen") : level == Level::Boundary ? tr("Boundaries chosen") : tr("Points chosen")},
                         {QStringLiteral("all"), level == Level::Domain ? tr("All domains") : level == Level::Boundary ? tr("All boundaries") : tr("All points")}};
    if (level == Level::Domain) {
        kinds << Kind{QStringLiteral("objects"), tr("Domains in objects")} << Kind{QStringLiteral("only"), tr("Objects' own domains")};
    } else {
        kinds << Kind{QStringLiteral("of"), tr("On objects' edges")};
        if (level == Level::Boundary)
            kinds << Kind{QStringLiteral("between"), tr("Between objects")} << Kind{QStringLiteral("exterior"), tr("Exterior boundaries")}
                  << Kind{QStringLiteral("interior"), tr("Interior boundaries")};
    }
    kinds << Kind{QStringLiteral("box"), tr("In a box")} << Kind{QStringLiteral("named"), tr("A named selection")};
    QString current = QStringLiteral("numbers");
    for (const Kind& kd : kinds)
        if (rule.contains(kd.key)) current = kd.key;
    auto* box = new QComboBox(w);
    box->setObjectName(QStringLiteral("mpSelectionKind"));
    for (const Kind& kd : kinds) box->addItem(kd.label, kd.key);
    box->setCurrentIndex(std::max(0, box->findData(current)));
    v->addWidget(box);
    const QString objects = rule.value(QStringLiteral("objects")).toVariant().toStringList().join(QLatin1String(", "));
    // The rule's arguments.
    auto* args = new QLineEdit(w);
    args->setObjectName(QStringLiteral("mpSelectionArgs"));
    QComboBox* side = nullptr;
    auto joined = [](const QJsonValue& value) {
        QStringList list;
        if (value.isString()) list << value.toString();
        for (const QJsonValue& x : value.toArray()) list << (x.isArray() ? text(x.toArray().first()) : text(x));
        return list.join(QLatin1String(", "));
    };
    if (current == QLatin1String("objects") || current == QLatin1String("only") || current == QLatin1String("of")) {
        args->setText(joined(rule.value(current)));
        args->setPlaceholderText(tr("objects' names, split by commas"));
        v->addWidget(args);
    } else if (current == QLatin1String("between")) {
        const QJsonArray pair = rule.value(QStringLiteral("between")).toArray();
        args->setText(joined(pair.at(0)) + QStringLiteral("; ") + joined(pair.at(1)));
        args->setPlaceholderText(tr("objects; objects"));
        v->addWidget(args);
    } else if (current == QLatin1String("box")) {
        args->setText(joined(rule.value(QStringLiteral("box"))));
        args->setPlaceholderText(tr("x0, y0, x1, y1 (the geometry's unit)"));
        v->addWidget(args);
    } else if (current == QLatin1String("named")) {
        args->setText(rule.value(QStringLiteral("named")).toString());
        args->setPlaceholderText(tr("the selection's name"));
        v->addWidget(args);
    } else {
        args->hide();
    }
    if (current == QLatin1String("of")) {
        side = new QComboBox(w);
        side->setObjectName(QStringLiteral("mpSelectionSide"));
        side->addItem(tr("All of them"), QString());
        side->addItem(tr("Those facing down"), QStringLiteral("bottom"));
        side->addItem(tr("Those facing up"), QStringLiteral("top"));
        side->addItem(tr("Those facing left"), QStringLiteral("left"));
        side->addItem(tr("Those facing right"), QStringLiteral("right"));
        side->setCurrentIndex(std::max(0, side->findData(rule.value(QStringLiteral("side")).toString())));
        v->addWidget(side);
    }
    // What it picks now.
    QString picked;
    if (a_doc && !a_doc->geometryStale() && a_doc->geometry().topology) {
        QString why;
        const QVector<int> got = resolveSelection(*a_doc->geometry().topology, a_doc->model(), level, rule, &why);
        QStringList numbers;
        for (int i : got) numbers << QString::number(i + 1);
        picked = numbers.isEmpty() ? tr("none") : numbers.join(QLatin1String(", "));
        if (!why.isEmpty()) picked += QStringLiteral(" <span style='color:#c62828'>(%1)</span>").arg(why.toHtmlEscaped());
    } else {
        picked = tr("(build the geometry to see them)");
    }
    auto* shown = new QLabel(QStringLiteral("%1: %2").arg(describeSelection(rule, level).toHtmlEscaped(), picked), w);
    shown->setObjectName(QStringLiteral("mpSelectionShown"));
    shown->setWordWrap(true);
    v->addWidget(shown);
    a_pick = new QPushButton(tr("Pick in the Graphics View"), w);
    a_pick->setObjectName(QStringLiteral("mpPick"));
    a_pick->setCheckable(true);
    a_pick->setChecked(a_doc && a_doc->picking());
    a_pick->setToolTip(tr("A click on a %1 in the Graphics view adds it, or takes it out: the selection becomes those numbers")
                           .arg(levelName(level)));
    v->addWidget(a_pick);
    connect(a_pick, &QPushButton::toggled, this, [this](bool on) {
        if (a_doc) a_doc->setPicking(on);
    });
    auto ruleFrom = [args, side, box, level]() -> QJsonObject {
        const QString kind = box->currentData().toString();
        auto names = [](const QString& textIn) {
            QJsonArray a;
            for (const QString& s : textIn.split(QLatin1Char(','), Qt::SkipEmptyParts)) a.append(s.trimmed());
            return a;
        };
        if (kind == QLatin1String("all")) return {{QStringLiteral("all"), true}};
        if (kind == QLatin1String("exterior") || kind == QLatin1String("interior")) return {{kind, true}};
        if (kind == QLatin1String("objects") || kind == QLatin1String("only")) return {{kind, names(args->text())}};
        if (kind == QLatin1String("of")) {
            QJsonObject o{{QStringLiteral("of"), names(args->text())}};
            if (side && !side->currentData().toString().isEmpty()) o.insert(QStringLiteral("side"), side->currentData().toString());
            return o;
        }
        if (kind == QLatin1String("between")) {
            const QStringList two = args->text().split(QLatin1Char(';'));
            return {{QStringLiteral("between"), QJsonArray{names(two.value(0)), names(two.value(1))}}};
        }
        if (kind == QLatin1String("box")) {
            QJsonArray a;
            for (const QString& s : args->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) a.append(s.trimmed().toDouble());
            while (a.size() < 4) a.append(0);
            return {{QStringLiteral("box"), a}};
        }
        if (kind == QLatin1String("named")) return {{QStringLiteral("named"), args->text().trimmed()}};
        Q_UNUSED(level);
        return {{QStringLiteral("numbers"), QJsonArray()}};
    };
    connect(box, &QComboBox::activated, this, [this, ruleFrom, rule, box] {
        // A rule of another kind: from the numbers it picks now, where it can be.
        const QString kind = box->currentData().toString();
        QJsonObject fresh = ruleFrom();
        if (kind == QLatin1String("numbers") && a_doc && !a_doc->geometryStale() && a_doc->geometry().topology) {
            const Node* now = a_doc->model().find(a_shownTag);
            const NodeKind* nk = now ? now->kind() : nullptr;
            Level lv = nk ? nk->selection : Level::Domain;
            if (nk && nk->property(QStringLiteral("level"))) lv = levelOf(now->text(QStringLiteral("level")));
            QJsonArray numbers;
            for (int i : resolveSelection(*a_doc->geometry().topology, a_doc->model(), lv, rule)) numbers.append(i + 1);
            fresh = {{QStringLiteral("numbers"), numbers}};
        }
        commit(QStringLiteral("selection"), fresh, tr("Change Selection"));
    });
    connect(args, &QLineEdit::editingFinished, this, [this, ruleFrom] { commit(QStringLiteral("selection"), ruleFrom(), tr("Change Selection")); });
    if (side) connect(side, &QComboBox::activated, this, [this, ruleFrom] { commit(QStringLiteral("selection"), ruleFrom(), tr("Change Selection")); });
    return w;
}

QWidget* MultiphysicsPanel::parametersEditor(const Node& node, const PropertyDef& p)
{
    const QString key = p.key;
    const bool parameters = node.type == QLatin1String("parameters");
    auto* w = new QWidget(a_form);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* table = new QTableWidget(w);
    table->setObjectName(QStringLiteral("mpRows"));
    const QStringList headers = parameters ? QStringList{tr("Name"), tr("Expression"), tr("Value"), tr("Description")}
                                : p.choiceLabels.isEmpty() ? QStringList{QStringLiteral("x"), QStringLiteral("f(x)")}
                                                           : p.choiceLabels;
    const int columns = int(headers.size());
    table->setColumnCount(int(headers.size()));
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    const QJsonArray rows = node.value(key).toArray();
    table->setRowCount(int(rows.size()));
    std::shared_ptr<ParameterScope> ps = a_doc ? a_doc->parameters() : nullptr;
    for (int r = 0; r < rows.size(); ++r) {
        if (parameters) {
            const QJsonObject o = rows.at(r).toObject();
            const QString name = o.value(QStringLiteral("name")).toString();
            table->setItem(r, 0, new QTableWidgetItem(name));
            table->setItem(r, 1, new QTableWidgetItem(o.value(QStringLiteral("expression")).toString()));
            QString value;
            QColor color = QApplication::palette().color(QPalette::Text);
            if (ps && ps->errors.contains(name)) {
                value = ps->errors.value(name);
                color = QColor(198, 40, 40);
            } else if (ps && ps->values.contains(name)) {
                value = formatQuantity(ps->values.value(name), ps->dims.value(name));
            }
            auto* shown = new QTableWidgetItem(value);
            shown->setFlags(shown->flags() & ~Qt::ItemIsEditable);
            shown->setForeground(color);
            shown->setToolTip(value);
            table->setItem(r, 2, shown);
            table->setItem(r, 3, new QTableWidgetItem(o.value(QStringLiteral("description")).toString()));
        } else {
            const QJsonArray cells = rows.at(r).toArray();
            for (int c = 0; c < columns; ++c) table->setItem(r, c, new QTableWidgetItem(text(cells.at(c))));
        }
    }
    table->resizeColumnsToContents();
    table->setMinimumHeight(std::min(320, 30 + 26 * std::max(3, int(rows.size()) + 1)));
    v->addWidget(table);
    auto collect = [table, parameters, columns]() {
        QJsonArray out;
        for (int r = 0; r < table->rowCount(); ++r) {
            auto cell = [&](int c) { return table->item(r, c) ? table->item(r, c)->text().trimmed() : QString(); };
            if (parameters) {
                if (cell(0).isEmpty() && cell(1).isEmpty()) continue;
                QJsonObject o{{QStringLiteral("name"), cell(0)}, {QStringLiteral("expression"), cell(1)}};
                if (!cell(3).isEmpty()) o.insert(QStringLiteral("description"), cell(3));
                out.append(o);
            } else {
                if (cell(0).isEmpty() && cell(1).isEmpty()) continue;
                QJsonArray row;
                for (int c = 0; c < columns; ++c) row.append(cell(c));
                out.append(row);
            }
        }
        return out;
    };
    connect(table, &QTableWidget::itemChanged, this, [this, key, collect](QTableWidgetItem* item) {
        if (item->column() == 2) return;
        a_focusKey = key;
        commit(key, collect(), tr("Change Parameters"));
    });
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), w);
    add->setObjectName(QStringLiteral("mpRowsAdd"));
    auto* remove = new QPushButton(tr("Remove"), w);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    v->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, [this, key, collect, parameters, node] {
        QJsonArray rows2 = collect();
        if (parameters) {
            // A name of its own.
            QSet<QString> names;
            for (const QJsonValue& r : rows2) names.insert(r.toObject().value(QStringLiteral("name")).toString());
            QString name = QStringLiteral("p1");
            for (int i = 2; names.contains(name); ++i) name = QStringLiteral("p%1").arg(i);
            rows2.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("expression"), QStringLiteral("1")}});
        } else if (node.type == QLatin1String("sweep")) {
            // The first parameter not swept yet, its value as it is.
            QStringList swept;
            for (const QJsonValue& r : rows2) swept << r.toArray().at(0).toString();
            QString name;
            for (const Model::Parameter& par : a_doc->model().parameters())
                if (!swept.contains(par.name)) {
                    name = par.name;
                    break;
                }
            rows2.append(QJsonArray{name, QStringLiteral("1, 2, 3"), QString()});
        } else {
            rows2.append(QJsonArray{QStringLiteral("0"), QStringLiteral("0")});
        }
        Q_UNUSED(node);
        commit(key, rows2, tr("Add a Row"));
    });
    connect(remove, &QPushButton::clicked, this, [this, key, table, collect] {
        const int r = table->currentRow();
        if (r < 0) return;
        QJsonArray rows2 = collect();
        if (r < rows2.size()) rows2.removeAt(r);
        commit(key, rows2, tr("Remove a Row"));
    });
    return w;
}

QWidget* MultiphysicsPanel::pointsEditor(const Node& node, const PropertyDef& p)
{
    const QString key = p.key;
    auto* w = new QWidget(a_form);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* table = new QTableWidget(w);
    table->setObjectName(QStringLiteral("mpPoints"));
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({QStringLiteral("x"), QStringLiteral("y")});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    const QJsonArray rows = node.value(key).toArray();
    table->setRowCount(int(rows.size()));
    for (int r = 0; r < rows.size(); ++r) {
        const QJsonArray pair = rows.at(r).toArray();
        table->setItem(r, 0, new QTableWidgetItem(text(pair.at(0))));
        table->setItem(r, 1, new QTableWidgetItem(text(pair.at(1))));
    }
    table->setMinimumHeight(std::min(300, 30 + 26 * std::max(3, int(rows.size()) + 1)));
    v->addWidget(table);
    auto collect = [table]() {
        QJsonArray out;
        for (int r = 0; r < table->rowCount(); ++r) {
            const QString x = table->item(r, 0) ? table->item(r, 0)->text().trimmed() : QString();
            const QString y = table->item(r, 1) ? table->item(r, 1)->text().trimmed() : QString();
            if (x.isEmpty() && y.isEmpty()) continue;
            out.append(QJsonArray{x.isEmpty() ? QStringLiteral("0") : x, y.isEmpty() ? QStringLiteral("0") : y});
        }
        return out;
    };
    connect(table, &QTableWidget::itemChanged, this, [this, key, collect] { commit(key, collect(), tr("Change Points")); });
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), w);
    auto* remove = new QPushButton(tr("Remove"), w);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    v->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, [this, key, collect] {
        QJsonArray rows2 = collect();
        rows2.append(QJsonArray{QStringLiteral("0"), QStringLiteral("0")});
        commit(key, rows2, tr("Add a Point"));
    });
    connect(remove, &QPushButton::clicked, this, [this, key, table, collect] {
        const int r = table->currentRow();
        QJsonArray rows2 = collect();
        if (r >= 0 && r < rows2.size()) rows2.removeAt(r);
        commit(key, rows2, tr("Remove a Point"));
    });
    return w;
}

QWidget* MultiphysicsPanel::materialsLibrary(const Node& node)
{
    auto* w = new QWidget(a_form);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 6, 0, 0);
    v->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("The library")), w));
    auto* list = new QTreeWidget(w);
    list->setObjectName(QStringLiteral("mpLibrary"));
    list->setHeaderHidden(true);
    list->setRootIsDecorated(true);
    QHash<QString, QTreeWidgetItem*> categories;
    QList<MaterialEntry> entries = userMaterials();
    entries << builtinMaterials();
    for (const MaterialEntry& e : entries) {
        QTreeWidgetItem*& cat = categories[e.category];
        if (!cat) {
            cat = new QTreeWidgetItem(list, {e.category});
            cat->setExpanded(true);
            cat->setFlags(Qt::ItemIsEnabled);
        }
        auto* item = new QTreeWidgetItem(cat, {e.name});
        item->setIcon(0, glyph(QStringLiteral("material")));
        item->setToolTip(0, e.description);
        item->setData(0, Qt::UserRole, e.name);
    }
    list->setMinimumHeight(220);
    v->addWidget(list);
    auto* add = new QPushButton(tr("Add Material"), w);
    add->setObjectName(QStringLiteral("mpAddMaterial"));
    v->addWidget(add);
    const QString tag = node.tag;
    auto addChosen = [this, list, tag] {
        if (QTreeWidgetItem* item = list->currentItem(); item && !item->data(0, Qt::UserRole).toString().isEmpty())
            addNode(tag, QStringLiteral("material:") + item->data(0, Qt::UserRole).toString());
    };
    connect(add, &QPushButton::clicked, this, addChosen);
    connect(list, &QTreeWidget::itemDoubleClicked, this, addChosen);
    // The library exchanged: My Materials from a file - a library's (.json),
    // a Lumerical project's (.ldev: its electrical and thermal database;
    // .lms, .fsp: its optical materials) - and to one.
    auto* exchange = new QHBoxLayout;
    auto* import = new QPushButton(tr("Import Materials..."), w);
    import->setObjectName(QStringLiteral("mpImportMaterials"));
    import->setToolTip(tr("Into My Materials: a library file's (.json), or a Lumerical™ project's materials (.ldev, .lms, .fsp)"));
    auto* exportMine = new QPushButton(tr("Export My Materials..."), w);
    exportMine->setObjectName(QStringLiteral("mpExportMaterials"));
    exchange->addWidget(import);
    exchange->addWidget(exportMine);
    v->addLayout(exchange);
    connect(import, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Materials"), QString(),
                                                          tr("Materials (*.json *.ldev *.lms *.fsp);;A library (*.json);;Lumerical™ projects (*.ldev *.lms *.fsp)"));
        if (path.isEmpty()) return;
        QString why;
        QStringList notes;
        const QList<MaterialEntry> taken = path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive) ? readMaterials(path, &why) : lumericalMaterials(path, &why, &notes);
        if (taken.isEmpty()) {
            QMessageBox::information(this, tr("Import Materials"), why.isEmpty() ? tr("%1 has no materials").arg(path) : why);
            return;
        }
        QList<MaterialEntry> mine = userMaterials();
        for (const MaterialEntry& e : taken) {
            mine.erase(std::remove_if(mine.begin(), mine.end(), [&](const MaterialEntry& x) { return x.name == e.name; }), mine.end());
            mine << e;
        }
        QDir().mkpath(QFileInfo(userMaterialsFile()).absolutePath());
        if (!writeMaterials(userMaterialsFile(), mine, &why)) {
            QMessageBox::warning(this, tr("Import Materials"), why);
            return;
        }
        QMessageBox::information(this, tr("Import Materials"),
                                 tr("%n material(s) into My Materials.", nullptr, int(taken.size())) + (notes.isEmpty() ? QString() : QStringLiteral("\n\n") + notes.join(QLatin1Char('\n'))));
        showSettings();
    });
    connect(exportMine, &QPushButton::clicked, this, [this] {
        const QList<MaterialEntry> mine = userMaterials();
        if (mine.isEmpty()) {
            QMessageBox::information(this, tr("Export My Materials"), tr("My Materials is empty: add a model's material to it (its menu) or import some."));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, tr("Export My Materials"), QStringLiteral("materials.json"), tr("A library (*.json)"));
        QString why;
        if (!path.isEmpty() && !writeMaterials(path, mine, &why)) QMessageBox::warning(this, tr("Export My Materials"), why);
    });
    auto* plot = new QPushButton(tr("Plot Properties..."), w);
    plot->setObjectName(QStringLiteral("mpPlotMaterial"));
    plot->setToolTip(tr("The chosen material's properties against the temperature, the doping, its composition, the wavelength or the field"));
    v->addWidget(plot);
    connect(plot, &QPushButton::clicked, this, [this, list] {
        QTreeWidgetItem* item = list->currentItem();
        if (!item || item->data(0, Qt::UserRole).toString().isEmpty() || !a_doc) return;
        if (const std::optional<MaterialEntry> e = findMaterial(item->data(0, Qt::UserRole).toString(), userMaterials()))
            plotProperties(materialNode(a_doc->model(), *e));
    });
    return w;
}

void MultiphysicsPanel::plotProperties(const Node& material)
{
    if (!a_doc) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Plot Properties: %1").arg(material.name()));
    auto* form = new QFormLayout(&dialog);
    // Each variable's range, scale and the properties first shown.
    struct Variable {
        const char* key;
        QString label, from, to;
        bool log;
        QStringList shown;
    };
    const std::vector<Variable> variables = {
        {"T", tr("Temperature T"), QStringLiteral("200[K]"), QStringLiteral("500[K]"), false,
         {QStringLiteral("Eg"), QStringLiteral("ni"), QStringLiteral("mun"), QStringLiteral("mup")}},
        {"N", tr("Doping N (donors and acceptors)"), QStringLiteral("1e14[1/cm^3]"), QStringLiteral("1e20[1/cm^3]"), true,
         {QStringLiteral("mun"), QStringLiteral("mup"), QStringLiteral("taun"), QStringLiteral("taup")}},
        {"x", tr("Composition x (an alloy's)"), QStringLiteral("0"), QStringLiteral("1"), false,
         {QStringLiteral("Eg"), QStringLiteral("chi"), QStringLiteral("n")}},
        {"lambda", tr("Wavelength λ"), QStringLiteral("0.4[um]"), QStringLiteral("2[um]"), false, {QStringLiteral("n"), QStringLiteral("kappa")}},
        {"F", tr("Field F"), QStringLiteral("1e4[V/cm]"), QStringLiteral("1e6[V/cm]"), true,
         {QStringLiteral("mun"), QStringLiteral("mup"), QStringLiteral("alphan"), QStringLiteral("alphap")}},
    };
    auto* variable = new QComboBox(&dialog);
    for (const Variable& v : variables) variable->addItem(v.label, QString::fromLatin1(v.key));
    auto* from = new QLineEdit(&dialog);
    auto* to = new QLineEdit(&dialog);
    auto* points = new QSpinBox(&dialog);
    points->setRange(2, 2001);
    points->setValue(51);
    auto* log = new QCheckBox(tr("Logarithmic"), &dialog);
    auto* T = new QLineEdit(QStringLiteral("300[K]"), &dialog);
    auto* N = new QLineEdit(QStringLiteral("0[1/cm^3]"), &dialog);
    auto* lambda = new QLineEdit(QStringLiteral("1.55[um]"), &dialog);
    auto* F = new QLineEdit(QStringLiteral("0[V/cm]"), &dialog);
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("mpPlotProperties"));
    for (const QString& key : plottableProperties()) {
        auto* item = new QListWidgetItem(key, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    auto choose = [=](int i) {
        const Variable& v = variables[std::size_t(std::max(0, i))];
        from->setText(v.from);
        to->setText(v.to);
        log->setChecked(v.log);
        for (int r = 0; r < list->count(); ++r) list->item(r)->setCheckState(v.shown.contains(list->item(r)->text()) ? Qt::Checked : Qt::Unchecked);
    };
    choose(0);
    connect(variable, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, choose);
    form->addRow(tr("Against"), variable);
    form->addRow(tr("From"), from);
    form->addRow(tr("To"), to);
    form->addRow(tr("Points"), points);
    form->addRow(QString(), log);
    form->addRow(tr("Held: T"), T);
    form->addRow(tr("N"), N);
    form->addRow(tr("λ"), lambda);
    form->addRow(tr("F"), F);
    form->addRow(tr("Properties"), list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Plot"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const Scope& scope = a_doc->parameters()->scope;
    QString why;
    auto value = [&](QLineEdit* e, const Dim& dim) { return evaluateConstant(e->text(), scope, dim, &why).value_or(NAN); };
    MaterialSweep sweep;
    sweep.variable = variable->currentData().toString();
    const Dim dim = sweep.variable == QLatin1String("T") ? Dim::temperature() : sweep.variable == QLatin1String("N") ? Dim::of(-3)
                  : sweep.variable == QLatin1String("lambda") ? Dim::length() : sweep.variable == QLatin1String("F") ? Dim::of(1, 1, -3, -1) : Dim();
    sweep.from = value(from, dim);
    sweep.to = value(to, dim);
    sweep.points = points->value();
    sweep.log = log->isChecked();
    sweep.T = value(T, Dim::temperature());
    sweep.N = value(N, Dim::of(-3));
    sweep.lambda = value(lambda, Dim::length());
    sweep.F = value(F, Dim::of(1, 1, -3, -1));
    QStringList keys;
    for (int r = 0; r < list->count(); ++r)
        if (list->item(r)->checkState() == Qt::Checked) keys << list->item(r)->text();
    if (!why.isEmpty() || keys.isEmpty()) {
        QMessageBox::information(this, tr("Plot Properties"), why.isEmpty() ? tr("Check a property to plot.") : why);
        return;
    }
    if (a_doc->plotMaterial(material, keys, sweep, &why).isEmpty()) QMessageBox::information(this, tr("Plot Properties"), why);
}

void MultiphysicsPanel::addActions(QVBoxLayout* layout, const Node& node)
{
    const NodeKind* k = node.kind();
    auto* row = new QHBoxLayout;
    auto action = [&](const QString& text, const char* name, std::function<void()> f) {
        auto* b = new QPushButton(text, a_form);
        b->setObjectName(QString::fromLatin1(name));
        connect(b, &QPushButton::clicked, this, f);
        row->addWidget(b);
    };
    const QString tag = node.tag;
    if (node.type == QLatin1String("geometry") || (k && k->group == QLatin1String("geometry")))
        action(tr("Build All"), "mpBuildAll", [this] {
            QPointer<MultiphysicsDoc> doc = a_doc;
            a_doc->requestGeometry([doc](bool built) {
                if (doc && built) doc->showGeometry();
            });
        });
    if (k && k->group == QLatin1String("process"))
        action(tr("Show After This Step"), "mpShowStep", [this, tag] { a_doc->showStep(tag); });
    if ((k && k->group == QLatin1String("process")) || node.type == QLatin1String("process")) {
        // Its dopants then: a colour map of the net doping, a profile down
        // the window's middle (SIMS's form).
        auto plot = [this, tag](bool profile) {
            QString why;
            MultiphysicsDoc::ProcessPlot p;
            p.profile = profile;
            const QString made = a_doc->makeProcessPlot(tag, p, &why);
            if (made.isEmpty() || !a_doc->showPlotGroup(made, &why)) QMessageBox::information(this, tr("Its Dopants"), why);
        };
        action(tr("Plot Its Dopants"), "mpPlotDopants", [plot] { plot(false); });
        action(tr("Its Profile (SIMS)"), "mpDopantProfile", [plot] { plot(true); });
    }
    if (k && k->group == QLatin1String("process") && node.text(QStringLiteral("mask")) == QLatin1String("layer"))
        action(tr("Show Its Mask in the Layout"), "mpShowMask", [this, tag] {
            QString why;
            if (!a_doc->showMaskInLayout(tag, &why)) QMessageBox::information(this, tr("Its Mask"), why);
        });
    if (node.type == QLatin1String("process")) {
        action(tr("Cut Along the Layout's Ruler"), "mpFlowCut", [this, tag] {
            QString why;
            if (!a_doc->cutAlongRuler(tag, &why)) QMessageBox::information(this, tr("Its Cut"), why);
        });
        action(tr("Make Its Materials"), "mpFlowMaterials", [this, tag] {
            const int made = a_doc->makeFlowMaterials(tag);
            if (made == 0) QMessageBox::information(this, tr("Its Materials"), tr("Every region of the flow has a material already."));
        });
    }
    if (node.type == QLatin1String("mesh") || (k && k->group == QLatin1String("mesh"))) action(tr("Build Mesh"), "mpBuildMeshButton", [this] { a_doc->buildMesh(); });
    if (node.type == QLatin1String("study")) action(tr("Compute"), "mpComputeButton", [this, tag] { a_doc->compute(tag); });
    if (k && k->group == QLatin1String("step")) {
        Model m = a_doc->model();
        int at = -1;
        const Node* study = m.parentOf(tag, &at);
        const QString studyTag = study ? study->tag : QString();
        action(tr("Compute"), "mpComputeButton", [this, studyTag] { a_doc->compute(studyTag); });
    }
    if (node.type == QLatin1String("plotgroup") || node.type == QLatin1String("plotgroup1d")) action(tr("Plot"), "mpPlotButton", [this, tag] {
            QString why;
            if (!a_doc->showPlotGroup(tag, &why)) QMessageBox::information(this, tr("Plot"), why);
        });
    if (k && k->group == QLatin1String("derived")) action(tr("Evaluate"), "mpEvaluateButton", [this, tag] { a_doc->evaluate(tag); });
    if (k && k->group == QLatin1String("export")) {
        action(tr("Export"), "mpExportButton", [this, tag] {
            QString why;
            if (a_doc->runExport(tag, &why).isEmpty()) QMessageBox::information(this, tr("Export"), why);
        });
        if (node.type == QLatin1String("qucspart"))
            action(tr("Copy Part"), "mpCopyPartButton", [this, tag] {
                QString why;
                if (!a_doc->copyPart(tag, &why)) QMessageBox::information(this, tr("Copy Part"), why);
            });
    }
    if (node.type == QLatin1String("component"))
        for (const QString& type : physicsTypes())
            if (const NodeKind* pk = nodeKind(type)) action(tr("Add %1").arg(pk->title), "mpAddPhysics", [this, tag, type] { addNode(tag, type); });
    row->addStretch(1);
    layout->addLayout(row);
}
