/*
 * stackeddiagram.cpp - stacked panes: Cartesian plots one above the other
 * that share one x axis
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "stackeddiagram.h"

#include "main.h"
#include "marker.h"
#include "misc.h"
#include "mnemo.h"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace {

Axis freshAxis(bool grid)
{
    Axis a;
    a.min = a.low = 0.0;
    a.max = a.up = 1.0;
    a.log = false;
    a.numGraphs = 0;
    a.GridOn = grid;
    a.Units = Axis::NoUnits;
    a.autoScale = true;
    a.limit_min = 0.0;
    a.limit_max = 1.0;
    a.step = 1.0;
    return a;
}

// An axis' settings as fields of the saved line (a label without spaces:
// percent-encoded, after an L so that an empty one is a field too).
QString axisFields(const Axis& a)
{
    const auto number = [](double v) { return QString::number(v, 'g', 15); };
    return QStringLiteral(" %1 %2 %3 %4 %5 %6 %7 L%8")
        .arg(a.autoScale ? 1 : 0)
        .arg(number(a.limit_min), number(a.step), number(a.limit_max))
        .arg(a.log ? 1 : 0)
        .arg(a.Units)
        .arg(a.GridOn ? 1 : 0)
        .arg(QString::fromLatin1(a.Label.toUtf8().toPercentEncoding()));
}

constexpr int FieldsPerAxis = 8;

void readAxis(Axis& a, const QStringList& fields, int at)
{
    if (fields.size() < at + FieldsPerAxis) return;
    bool ok = false;
    a.autoScale = fields.at(at).toInt() != 0;
    const double from = fields.at(at + 1).toDouble(&ok);
    if (ok && std::isfinite(from)) a.limit_min = from;
    const double step = fields.at(at + 2).toDouble(&ok);
    if (ok && std::isfinite(step) && step != 0.0) a.step = step;
    const double to = fields.at(at + 3).toDouble(&ok);
    if (ok && std::isfinite(to)) a.limit_max = to;
    a.log = fields.at(at + 4).toInt() != 0;
    const int units = fields.at(at + 5).toInt(&ok);
    a.Units = ok && units >= Axis::NoUnits && units <= Axis::dBmUnits ? units : Axis::NoUnits;
    a.GridOn = fields.at(at + 6).toInt() != 0;
    const QString label = fields.at(at + 7);
    a.Label = label.startsWith(QLatin1Char('L')) ? QString::fromUtf8(QByteArray::fromPercentEncoding(label.mid(1).toLatin1()))
                                                 : QString();
    if (!a.autoScale && a.limit_min == a.limit_max) a.autoScale = true;   // no range: of no use
}

} // namespace

StackedDiagram::StackedDiagram(int cx, int cy) : RectDiagram(cx, cy)
{
    x2 = 320;    // initial size: room for two panes
    y2 = 320;
    x3 = x2 + 7;
    Name = "Stacked";
    m_panes = {Pane{freshAxis(true), freshAxis(false)}, Pane{freshAxis(true), freshAxis(false)}};
    calcDiagram();
}

Diagram* StackedDiagram::newOne()
{
    return new StackedDiagram();
}

Element* StackedDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Stacked panes");
    BitmapFile = (char*)"stacked";
    if (getNewOne) return new StackedDiagram();
    return nullptr;
}

void StackedDiagram::setPaneCount(int n)
{
    n = std::clamp(n, 1, MaxPanes);
    while (m_panes.size() < n) {
        Pane p = m_panes.isEmpty() ? Pane{freshAxis(true), freshAxis(false)} : m_panes.constLast();
        p.left.Label.clear();
        p.right.Label.clear();
        p.left.numGraphs = p.right.numGraphs = 0;
        m_panes.append(p);
    }
    m_panes.resize(n);
    for (Graph* g : Graphs)
        if (g->pane >= n) g->pane = n - 1;
}

int StackedDiagram::paneOf(const Graph* g) const
{
    return std::clamp(g->pane, 0, std::max(0, paneCount() - 1));
}

int StackedDiagram::paneHeight() const
{
    const int n = std::max(1, paneCount());
    return std::max(10, (y2 - (n - 1) * Gap) / n);
}

int StackedDiagram::paneBottom(int i) const
{
    return (paneCount() - 1 - i) * (paneHeight() + Gap);
}

int StackedDiagram::paneAt(double y) const
{
    const int h = paneHeight();
    for (int i = 0; i < paneCount(); ++i) {
        const int b = paneBottom(i);
        if (y >= b && y <= b + h) return i;
    }
    return -1;
}

int StackedDiagram::paneOfAxis(const Axis* axis, bool* right) const
{
    for (int i = 0; i < paneCount(); ++i) {
        if (axis == &m_panes.at(i).left || axis == &m_panes.at(i).right) {
            if (right) *right = axis == &m_panes.at(i).right;
            return i;
        }
    }
    return -1;
}

const Axis* StackedDiagram::graphAxis(const Graph* g) const
{
    if (m_panes.isEmpty()) return Diagram::graphAxis(g);
    const Pane& p = m_panes.at(paneOf(g));
    return g->yAxisNo == 0 ? &p.left : &p.right;
}

const Axis* StackedDiagram::limitAxis(const qucs_s::limits::Limit& limit) const
{
    if (limit.pane < 0 || limit.pane >= paneCount()) return nullptr;
    return limit.axis == 0 ? &m_panes.at(limit.pane).left : &m_panes.at(limit.pane).right;
}

Diagram::Part StackedDiagram::partOf(const Axis* axis) const
{
    bool right = false;
    if (paneOfAxis(axis, &right) >= 0) return right ? Part::RightAxis : Part::YAxis;
    return Diagram::partOf(axis);
}

bool StackedDiagram::drawsGraph(int valid, const Graph* g) const
{
    return (valid & (1 << (2 * paneOf(g) + (g->yAxisNo == 0 ? 0 : 1)))) != 0;
}

void StackedDiagram::clearExtraRanges()
{
    for (Pane& p : m_panes)
        for (Axis* a : {&p.left, &p.right}) {
            a->min = DBL_MAX;
            a->max = -DBL_MAX;
            a->numGraphs = 0;
        }
}

void StackedDiagram::settleExtraRanges()
{
    for (Pane& p : m_panes)
        for (Axis* a : {&p.left, &p.right})
            if (a->min > a->max) {
                a->min = 0.0;
                a->max = 1.0;
            }
}

void StackedDiagram::calcCoordinate(const double* xD, const double* yD, const double* zD, float* px, float* py,
                                    Axis const* pa) const
{
    const int i = paneOfAxis(pa);
    if (i < 0) {
        RectDiagram::calcCoordinate(xD, yD, zD, px, py, pa);
        return;
    }
    // As a Cartesian diagram's, the y in the pane's height, from its bottom.
    double x = *xD;
    double yr = yD[0];
    const double yi = yD[1];
    if (xAxis.log) {
        x /= xAxis.low;
        if (x <= 0.0) *px = -1e5;   // "negative infinity"
        else *px = float(std::log10(x) / std::log10(xAxis.up / xAxis.low) * double(x2));
    } else {
        *px = float((x - xAxis.low) / (xAxis.up - xAxis.low) * double(x2));
    }
    const double h = paneHeight(), b = paneBottom(i);
    if (pa->log) {
        yr = std::sqrt(yr * yr + yi * yi);
        if (yr <= 0.0) *py = -1e5;
        else *py = float(b + std::log10(yr / std::fabs(pa->low)) / std::log10(pa->up / pa->low) * h);
    } else {
        if (std::fabs(yi) > 1e-250) yr = std::sqrt(yr * yr + yi * yi);   // a complex value: its magnitude
        *py = float(b + (yr - pa->low) / (pa->up - pa->low) * h);
    }
    if (!std::isfinite(*px)) *px = 0.0;
    if (!std::isfinite(*py)) *py = 0.0;
}

void StackedDiagram::calcData(Graph* g)
{
    // Clipped to its pane.
    const int i = paneOf(g);
    clipBand = true;
    clipLow = float(paneBottom(i));
    clipHigh = clipLow + float(paneHeight());
    RectDiagram::calcData(g);
    clipBand = false;
}

template <class Make> auto StackedDiagram::inBand(int bottom, int height, Make make)
{
    QList<qucs::Line*> lines;
    lines.swap(Lines);
    QList<Text*> texts;
    texts.swap(Texts);
    const int fullHeight = y2;
    y2 = height;
    auto result = make();
    y2 = fullHeight;
    QList<qucs::Line*> grid, rest;
    for (qucs::Line* l : std::as_const(Lines)) {
        l->y1 += bottom;
        l->y2 += bottom;
        (l->part == static_cast<unsigned char>(Part::Grid) ? grid : rest).append(l);
    }
    for (Text* t : std::as_const(Texts)) t->y += bottom;
    Lines = grid + lines + rest;
    Texts = texts + Texts;
    return result;
}

int StackedDiagram::calcDiagram()
{
    qDeleteAll(Lines);
    Lines.clear();
    qDeleteAll(Texts);
    Texts.clear();
    qDeleteAll(Arcs);
    Arcs.clear();

    y1 = QucsSettings.font.pointSize() + 6;
    x1 = 10;
    x3 = x2 + 7;
    int valid = 0;
    if (m_panes.isEmpty()) setPaneCount(1);

    // Each step with the sign of its range (if the user gave it wrong).
    const auto fixStep = [](Axis& a) {
        a.step = std::fabs(a.step);
        if (a.limit_min > a.limit_max) a.step *= -1.0;
    };
    fixStep(xAxis);
    for (Pane& p : m_panes) {
        fixStep(p.left);
        fixStep(p.right);
    }

    const int n = paneCount(), h = paneHeight();
    // The x grid in each pane; its numbers under the bottom one.
    bool xValid = true;
    for (int i = 0; i < n; ++i)
        xValid = inBand(paneBottom(i), h, [&] {
            const bool ok = createXGrid();
            if (i != n - 1) {
                qDeleteAll(Texts);
                Texts.clear();
            }
            return ok;
        });

    // Each pane's y axes; its numbers as wide as the widest pane's.
    int left = x1, right = x3;
    if (xValid) {
        for (int i = 0; i < n; ++i) {
            Pane& p = m_panes[i];
            if (p.right.numGraphs > 0 && inBand(paneBottom(i), h, [&] { return calcYAxis(&p.right, x2); })) valid |= 1 << (2 * i + 1);
            right = std::max(right, x3);
            if (p.left.numGraphs > 0 && inBand(paneBottom(i), h, [&] { return calcYAxis(&p.left, 0); })) valid |= 1 << (2 * i);
            left = std::max(left, x1);
        }
    }
    x1 = left;
    x3 = right;

    // Each pane's frame.
    for (int i = 0; i < n; ++i) {
        const int b = paneBottom(i), t = b + h;
        Lines.append(as(Part::Frame, new qucs::Line(0, t, x2, t, QPen(Qt::black, 0))));
        Lines.append(as(Part::Frame, new qucs::Line(x2, t, x2, b, QPen(Qt::black, 0))));
        Lines.append(as(Part::Frame, new qucs::Line(0, b, x2, b, QPen(Qt::black, 0))));
        Lines.append(as(Part::Frame, new qucs::Line(0, t, 0, b, QPen(Qt::black, 0))));
    }
    return valid;
}

void StackedDiagram::calcLimits()
{
    RectDiagram::calcLimits();
    const int h = paneHeight();
    for (Pane& p : m_panes)
        for (Axis* a : {&p.left, &p.right}) {
            if (!a->autoScale) continue;
            int i;
            double b, c, d;
            if (a->log) {
                calcAxisLogScale(a, i, b, c, d, h);
                a->step = 1.0;
            } else {
                calcAxisScale(a, b, c, d, a->step, double(h));
            }
            a->limit_min = a->low;
            a->limit_max = a->up;
        }
}

MappedPoint StackedDiagram::pointToValue(const QPointF& point)
{
    MappedPoint result{};
    const auto along = [](double t, const Axis& a) {
        if (a.log) return std::pow(10.0, std::log10(a.low) + t * (std::log10(a.up) - std::log10(a.low)));
        return a.low + t * (a.up - a.low);
    };
    result.x = along(point.x() / double(x2), xAxis);
    int i = paneAt(point.y());
    if (i < 0) {
        // Between two: the nearer one.
        double nearest = DBL_MAX;
        for (int k = 0; k < paneCount(); ++k) {
            const double d = std::min(std::fabs(point.y() - paneBottom(k)), std::fabs(point.y() - paneBottom(k) - paneHeight()));
            if (d < nearest) {
                nearest = d;
                i = k;
            }
        }
    }
    result.pane = i;
    if (i < 0) return result;
    const double t = (point.y() - paneBottom(i)) / double(paneHeight());
    result.y1 = along(t, m_panes.at(i).left);
    result.y2 = along(t, m_panes.at(i).right);
    return result;
}

void StackedDiagram::setLimitsBySelectionRect(QRectF select)
{
    // The x range of the box, and the y range of the pane it is in.
    const MappedPoint low = pointToValue(select.bottomLeft());
    const MappedPoint high = pointToValue(select.topRight());
    double a, b, c;
    xAxis.limit_min = low.x;
    xAxis.limit_max = high.x;
    calcAxisScale(&xAxis, a, b, c, xAxis.step, double(x2));
    xAxis.autoScale = false;
    const int i = paneAt(select.center().y());
    if (i < 0) return;
    const MappedPoint top = pointToValue(QPointF(select.left(), std::min(select.bottom(), double(paneBottom(i) + paneHeight()))));
    const MappedPoint bottom = pointToValue(QPointF(select.left(), std::max(select.top(), double(paneBottom(i)))));
    Pane& p = m_panes[i];
    p.left.limit_min = bottom.y1;
    p.left.limit_max = top.y1;
    calcAxisScale(&p.left, a, b, c, p.left.step, double(paneHeight()));
    p.left.autoScale = false;
    p.right.limit_min = bottom.y2;
    p.right.limit_max = top.y2;
    calcAxisScale(&p.right, a, b, c, p.right.step, double(paneHeight()));
    p.right.autoScale = false;
}

QStringList StackedDiagram::valuesAt(double x, int paneIndex) const
{
    QStringList lines;
    for (const Graph* g : Graphs) {
        if (paneOf(g) != paneIndex || g->cPointsY == nullptr || g->axis(0) == nullptr) continue;
        const DataX* xs = g->axis(0);
        const double* xp = xs->Points;
        const int count = xs->count;
        double value = std::nan("");
        for (int k = 0; k + 1 < count; ++k) {
            const double xa = xp[k], xb = xp[k + 1];
            if ((x - xa) * (x - xb) > 0.0) continue;
            const auto shown = [&](int at) {
                const double re = g->cPointsY[2 * at], im = g->cPointsY[2 * at + 1];
                return std::fabs(im) > 1e-250 ? std::hypot(re, im) : re;
            };
            const double t = xb == xa ? 0.0 : (x - xa) / (xb - xa);
            value = shown(k) + t * (shown(k + 1) - shown(k));
            break;
        }
        if (count == 1 && x == xp[0]) value = std::fabs(g->cPointsY[1]) > 1e-250 ? std::hypot(g->cPointsY[0], g->cPointsY[1]) : g->cPointsY[0];
        const Axis* a = graphAxis(g);
        if (std::isfinite(value) && a->log && a->Units != Axis::NoUnits) value = qucs::num2db(value, a->Units);
        const QString name = g->withValuePart(g->Var.contains(QLatin1Char('/')) ? g->Var.section(QLatin1Char('/'), 1) : g->Var);
        lines << name + QStringLiteral(": ") + (std::isfinite(value) ? numberText(value) : QStringLiteral("-"));
    }
    return lines;
}

QString StackedDiagram::extraMarkerText(Marker const* m) const
{
    // Every other trace where the marker is: the panes read out together.
    if (m->varPos().empty() || m->graph() == nullptr) return {};
    const double x = m->varPos().front();
    QStringList lines;
    for (int i = 0; i < paneCount(); ++i)
        for (const QString& line : valuesAt(x, i))
            lines << line;
    // (Its own trace is in the text already.)
    const Graph* own = m->graph();
    const QString ownName = own->withValuePart(own->Var.contains(QLatin1Char('/')) ? own->Var.section(QLatin1Char('/'), 1) : own->Var) + QStringLiteral(": ");
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const QString& l) { return l.startsWith(ownName); }), lines.end());
    return lines.isEmpty() ? QString() : QStringLiteral("\n") + lines.join(QLatin1Char('\n'));
}

void StackedDiagram::createAxisLabels()
{
    QFontMetrics metrics(QucsSettings.font, nullptr);
    const int lineSpacing = metrics.lineSpacing();

    // Under the bottom pane: the x axis' label, or each graph's x variable.
    int y = -y1, wmax = 0;
    const int middle = x2 >> 1;
    if (xAxis.Label.isEmpty()) {
        QStringList shown;
        for (Graph* g : Graphs) {
            const DataX* d = g->axis(0);
            if (!d || shown.contains(d->Var)) continue;
            shown << d->Var;
            y -= lineSpacing;
            const int w = metrics.boundingRect(d->Var).width() >> 1;
            wmax = std::max(wmax, w);
            Texts.append(as(Part::XAxis, new Text(middle - w, y, d->Var, Qt::black, 12.0)));
        }
    } else {
        QString text;
        encode_String(xAxis.Label, text);
        y -= lineSpacing;
        const int w = metrics.boundingRect(text).width() >> 1;
        wmax = std::max(wmax, w);
        Texts.append(as(Part::XAxis, new Text(middle - w, y, text, Qt::black, 12.0)));
    }
    Bounding_y2 = 0;
    Bounding_y1 = y - lineSpacing;
    Bounding_x2 = std::max(0, wmax - middle);
    Bounding_x1 = Bounding_x2;

    // A graph's name without the dataset and the analysis when they are
    // the same for all.
    QStringList kernels, analyses;
    for (const Graph* g : Graphs) {
        if (!g->Var.contains(QLatin1Char('/'))) continue;
        const QString kernel = g->Var.section(QLatin1Char('/'), 0, 0);
        const QString analysis = g->Var.section(QLatin1Char('/'), 1).section(QLatin1Char('.'), 0, 0);
        if (!kernels.contains(kernel)) kernels << kernel;
        if (!analyses.contains(analysis)) analyses << analysis;
    }
    const auto nameOf = [&](const Graph* g) {
        QString name = g->Var;
        if (!QucsSettings.fullTraceName) {
            if (kernels.size() == 1) name = name.mid(name.indexOf(QLatin1Char('/')) + 1);
            if (analyses.size() == 1) name = name.mid(name.indexOf(QLatin1Char('.')) + 1);
        }
        return g->withValuePart(name);
    };

    // Each pane's labels beside it, turned, at its middle: its axis' label,
    // or its graphs' names in their colours.
    const int h = paneHeight();
    int leftmost = 0, rightmost = 0;
    for (int i = 0; i < paneCount(); ++i) {
        const int centre = paneBottom(i) + h / 2;
        for (const bool right : {false, true}) {
            const Axis& a = right ? m_panes.at(i).right : m_panes.at(i).left;
            const Part part = right ? Part::RightAxis : Part::YAxis;
            int x = right ? x3 : -x1;
            const auto put = [&](const QString& text, const QColor& color, Part p) {
                const int w = metrics.boundingRect(text).width() >> 1;
                if (right) {
                    Texts.append(as(p, new Text(x, centre + w, text, color, 12.0, 0.0, -1.0)));
                    x += lineSpacing;
                } else {
                    Texts.append(as(p, new Text(x, centre - w, text, color, 12.0, 0.0, 1.0)));
                    x -= lineSpacing;
                }
                // Above the top or below the bottom, when longer than the pane.
                Bounding_y2 = std::max(Bounding_y2, centre + w - y2);
                Bounding_y1 = std::min(Bounding_y1, centre - w);
            };
            if (!a.Label.isEmpty()) {
                QString text;
                encode_String(a.Label, text);
                put(text, Qt::black, part);
            } else {
                for (const Graph* g : Graphs) {
                    if (paneOf(g) != i || (g->yAxisNo != 0) != right) continue;
                    if (g->cPointsY) put(nameOf(g), g->colorsEachCurve() ? QColor(Qt::black) : g->Color,
                                         g->colorsEachCurve() ? part : Part::None);
                    else put(g->Var + INVALID_STR, g->Color, Part::None);
                }
            }
            if (right) rightmost = std::max(rightmost, x - x2);
            else leftmost = std::max(leftmost, -x);
        }
    }
    Bounding_x1 = std::max(Bounding_x1, leftmost);
    Bounding_x2 = std::max(Bounding_x2, rightmost);
}

QPainterPath StackedDiagram::plotAreaShape() const
{
    QPainterPath path;
    const int h = paneHeight();
    for (int i = 0; i < paneCount(); ++i) path.addRect(QRectF(0, -(paneBottom(i) + h), x2, h));
    return path;
}

void StackedDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    // Where each marker is, across every pane: the markers are shared.
    QList<int> xs;
    for (const Graph* g : Graphs)
        for (const Marker* m : g->Markers)
            if (m->cx > 0 && m->cx < x2 && !xs.contains(m->cx)) xs << m->cx;
    if (xs.isEmpty()) return;
    painter->save();
    QPen pen(colors.of(Part::Frame), 0, Qt::DashLine);
    painter->setPen(pen);
    const int h = paneHeight();
    for (const int x : std::as_const(xs))
        for (int i = 0; i < paneCount(); ++i) {
            const int b = paneBottom(i);
            painter->drawLine(QLineF(x, -(b + h), x, -b));
        }
    painter->restore();
}

QString StackedDiagram::extraSaveFields() const
{
    QString s = QStringLiteral(" %1").arg(paneCount());
    for (const Pane& p : m_panes) s += axisFields(p.left) + axisFields(p.right);
    return s;
}

void StackedDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    const int n = fields.value(0).toInt(&ok);
    m_panes.clear();
    setPaneCount(ok ? n : 2);
    for (int i = 0; i < paneCount(); ++i) {
        readAxis(m_panes[i].left, fields, 1 + 2 * i * FieldsPerAxis);
        readAxis(m_panes[i].right, fields, 1 + (2 * i + 1) * FieldsPerAxis);
    }
}
