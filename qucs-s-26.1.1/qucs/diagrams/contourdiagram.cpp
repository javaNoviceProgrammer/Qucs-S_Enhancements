/*
 * contourdiagram.cpp - the contour map: a value over two swept parameters
 * in colour, its iso-lines labelled, the region that passes a spec
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "contourdiagram.h"

#include "ink.h"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kMaxSide = 8192;
constexpr qint64 kMaxPixels = 8 * 1024 * 1024;

QString fieldText(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 15) : QStringLiteral("-");
}

double fieldValue(const QString& s)
{
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok && std::isfinite(v) ? v : NaN;
}

// "ngspice/sw.v(out)" is "sw.v(out)".
QString bare(const QString& var)
{
    return var.mid(var.lastIndexOf(QLatin1Char('/')) + 1);
}

// A colour of a map sampled at 11 points, 0 to 1, between them straight.
QColor sampled(const int (*stops)[3], double t)
{
    const double at = std::clamp(t, 0.0, 1.0) * 10.0;
    const int i = std::min(9, int(at));
    const double f = at - i;
    return QColor(int(std::lround(stops[i][0] + f * (stops[i + 1][0] - stops[i][0]))),
                  int(std::lround(stops[i][1] + f * (stops[i + 1][1] - stops[i][1]))),
                  int(std::lround(stops[i][2] + f * (stops[i + 1][2] - stops[i][2]))));
}

// \a v to three significant digits: a colour bar's ends.
double threeDigits(double v)
{
    if (!std::isfinite(v) || v == 0.0) return v;
    const double scale = std::pow(10.0, 2 - std::floor(std::log10(std::fabs(v))));
    return std::round(v * scale) / scale;
}

// Where \a x falls among \a xs (rising): the index of the point below it
// and how far on to the next (in log10 when \a log); false outside.
bool cellOf(const QVector<double>& xs, double x, bool log, int* i, double* f)
{
    const int n = int(xs.size());
    if (n < 2 || !(x >= xs.first() && x <= xs.last())) return false;
    int k = int(std::upper_bound(xs.cbegin(), xs.cend(), x) - xs.cbegin()) - 1;
    k = std::clamp(k, 0, n - 2);
    double a = xs.at(k), b = xs.at(k + 1), v = x;
    if (log) {
        if (!(a > 0 && b > 0 && v > 0)) return false;
        a = std::log10(a);
        b = std::log10(b);
        v = std::log10(v);
    }
    *i = k;
    *f = b != a ? (v - a) / (b - a) : 0.0;
    return true;
}

} // namespace

ContourDiagram::ContourDiagram(int cx, int cy) : RectDiagram(cx, cy), passMin(NaN), passMax(NaN)
{
    x2 = 360;
    y2 = 260;
    x3 = x2 + 7;
    Name = "Contour";
    calcDiagram();
}

Diagram* ContourDiagram::newOne()
{
    return new ContourDiagram();
}

Element* ContourDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Contour Map");
    BitmapFile = (char*)"contour";
    if (getNewOne) return new ContourDiagram();
    return nullptr;
}

QStringList ContourDiagram::mapNames()
{
    return {QStringLiteral("viridis"), QStringLiteral("turbo"), QStringLiteral("grey")};
}

double ContourDiagram::Grid::valueAt(double px, double py, bool logX, bool logY) const
{
    int i = 0, j = 0;
    double s = 0, t = 0;
    if (!ok() || !cellOf(x, px, logX, &i, &s) || !cellOf(y, py, logY, &j, &t)) return NaN;
    const double a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d)) return NaN;
    return (1 - s) * (1 - t) * a + s * (1 - t) * b + s * t * c + (1 - s) * t * d;
}

ContourDiagram::Grid ContourDiagram::gridOf(const Graph* g)
{
    Grid grid;
    const DataX* xs = g->axis(0);
    const DataX* ys = g->axis(1);
    if (g->cPointsY == nullptr || xs == nullptr || xs->Points == nullptr || xs->count < 1) {
        grid.error = QObject::tr("no data: simulate, or check the variable's name");
        return grid;
    }
    if (ys == nullptr || ys->Points == nullptr || ys->count < 2 || xs->count < 2) {
        grid.error = QObject::tr("swept over %1 alone: a contour map needs two sweeps (a parameter sweep about it)").arg(bare(xs->Var));
        return grid;
    }
    const int nx = xs->count, ny = ys->count;
    grid.xName = xs->Var;
    grid.yName = ys->Var;
    grid.slices = std::max(1, g->countY / ny);
    // Each sweep rising (one swept down turned round).
    QVector<int> ix(nx), iy(ny);
    std::iota(ix.begin(), ix.end(), 0);
    std::iota(iy.begin(), iy.end(), 0);
    std::stable_sort(ix.begin(), ix.end(), [&](int a, int b) { return xs->Points[a] < xs->Points[b]; });
    std::stable_sort(iy.begin(), iy.end(), [&](int a, int b) { return ys->Points[a] < ys->Points[b]; });
    for (int i : ix) grid.x << xs->Points[i];
    for (int j : iy) grid.y << ys->Points[j];
    grid.v.resize(nx * ny);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const qint64 k = qint64(iy.at(j)) * nx + ix.at(i);
            const double re = g->cPointsY[2 * k], im = g->cPointsY[2 * k + 1];
            // (As the other diagrams take it: a complex value's magnitude.)
            const double v = std::fabs(im) > 1e-250 ? std::hypot(re, im) : re;
            grid.v[j * nx + i] = std::isfinite(v) ? v : NaN;
        }
    return grid;
}

QVector<double> ContourDiagram::levelsIn(double low, double high, int count)
{
    QVector<double> out;
    if (count <= 0 || !std::isfinite(low) || !std::isfinite(high) || !(high > low)) return out;
    // The round step whose lines come nearest the count (the fewer when
    // two are as near).
    const double raw = (high - low) / count;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    double step = 10 * magnitude;
    int best = -1;
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        const double s = m * magnitude;
        const int lines = int(std::ceil(high / s - 1e-9) - std::floor(low / s + 1e-9)) - 1;
        if (best < 0 || std::abs(lines - count) < std::abs(best - count) || (std::abs(lines - count) == std::abs(best - count) && lines < best)) {
            best = lines;
            step = s;
        }
    }
    for (double v = std::ceil(low / step) * step; v < high && out.size() <= 2 * MaxLevels; v += step) {
        const double level = std::abs(v) < 1e-9 * step ? 0.0 : v;
        if (level > low) out << level;
    }
    return out;
}

bool ContourDiagram::passes(double v) const
{
    return (!std::isfinite(passMin) || v >= passMin) && (!std::isfinite(passMax) || v <= passMax);
}

double ContourDiagram::passing() const
{
    if (!hasBand() || m_grids.isEmpty() || !m_grids.first().ok()) return NaN;
    int all = 0, good = 0;
    for (double v : m_grids.first().v)
        if (std::isfinite(v)) {
            ++all;
            if (passes(v)) ++good;
        }
    return all > 0 ? double(good) / all : NaN;
}

QColor ContourDiagram::colourAt(double t) const
{
    static const int viridis[11][3] = {{68, 1, 84},    {72, 36, 117},  {65, 68, 135},  {53, 95, 141},
                                       {42, 120, 142}, {33, 145, 140}, {34, 168, 132}, {68, 191, 112},
                                       {122, 209, 81}, {189, 223, 38}, {253, 231, 37}};
    static const int turbo[11][3] = {{48, 18, 59},   {68, 81, 191},  {70, 134, 251}, {40, 187, 236},
                                     {27, 229, 181}, {114, 254, 94}, {182, 246, 53}, {237, 207, 57},
                                     {251, 151, 39}, {225, 81, 10},  {122, 4, 3}};
    static const int grey[11][3] = {{30, 30, 30},    {51, 51, 51},    {72, 72, 72},    {93, 93, 93},
                                    {114, 114, 114}, {135, 135, 135}, {156, 156, 156}, {177, 177, 177},
                                    {198, 198, 198}, {219, 219, 219}, {240, 240, 240}};
    return sampled(map == Turbo ? turbo : map == Grey ? grey : viridis, t);
}

QPointF ContourDiagram::pointOf(double x, double y) const
{
    const double yd[2] = {y, 0};
    float px = 0, py = 0;
    calcCoordinate(&x, yd, nullptr, &px, &py, &yAxis);
    return QPointF(px, py);
}

QVector<QLineF> ContourDiagram::isoLine(const Grid& g, double level) const
{
    QVector<QLineF> out;
    if (!g.ok()) return out;
    const int nx = int(g.x.size()), ny = int(g.y.size());
    QVector<double> px(nx), py(ny);
    for (int i = 0; i < nx; ++i) px[i] = pointOf(g.x.at(i), g.y.first()).x();
    for (int j = 0; j < ny; ++j) py[j] = pointOf(g.x.first(), g.y.at(j)).y();
    // Marching squares: each cell's corners above or below the level; a
    // saddle by its centre.
    const auto cut = [&](int i0, int j0, int i1, int j1) {
        const double a = g.at(i0, j0), b = g.at(i1, j1);
        const double f = b != a ? (level - a) / (b - a) : 0.5;
        return QPointF(px.at(i0) + f * (px.at(i1) - px.at(i0)), py.at(j0) + f * (py.at(j1) - py.at(j0)));
    };
    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i) {
            const double a = g.at(i, j), b = g.at(i + 1, j), c = g.at(i + 1, j + 1), d = g.at(i, j + 1);
            if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d)) continue;
            const int code = (a > level ? 1 : 0) | (b > level ? 2 : 0) | (c > level ? 4 : 0) | (d > level ? 8 : 0);
            if (code == 0 || code == 15) continue;
            const QPointF bottom = cut(i, j, i + 1, j), right = cut(i + 1, j, i + 1, j + 1);
            const QPointF top = cut(i, j + 1, i + 1, j + 1), left = cut(i, j, i, j + 1);
            const bool centre = (a + b + c + d) / 4 > level;
            switch (code) {
            case 1: case 14: out << QLineF(left, bottom); break;
            case 2: case 13: out << QLineF(bottom, right); break;
            case 3: case 12: out << QLineF(left, right); break;
            case 4: case 11: out << QLineF(right, top); break;
            case 6: case 9: out << QLineF(bottom, top); break;
            case 7: case 8: out << QLineF(left, top); break;
            case 5:   // a and c above
                if (centre) out << QLineF(bottom, right) << QLineF(top, left);
                else out << QLineF(left, bottom) << QLineF(right, top);
                break;
            case 10:  // b and d above
                if (centre) out << QLineF(left, bottom) << QLineF(right, top);
                else out << QLineF(bottom, right) << QLineF(top, left);
                break;
            }
        }
    return out;
}

void ContourDiagram::getAxisLimits(Graph* g)
{
    g->yAxisNo = 0;   // up y: the second sweep
    ++yAxis.numGraphs;
    // Its grid, kept for the layout; along and up it, its range. (A trace
    // that makes none - of one sweep - is no map: left out of the range.)
    const Grid grid = gridFor(g);
    m_byGraph.insert(g, grid);
    if (!grid.ok()) return;
    for (double x : grid.x)
        if (std::isfinite(x)) {
            xAxis.min = std::min(xAxis.min, x);
            xAxis.max = std::max(xAxis.max, x);
        }
    for (double y : grid.y)
        if (std::isfinite(y)) {
            yAxis.min = std::min(yAxis.min, y);
            yAxis.max = std::max(yAxis.max, y);
        }
}

void ContourDiagram::autoRange(double lo, double hi)
{
    const double pad = hi > lo ? 0.0 : std::max(1.0, std::abs(lo)) * 0.5;
    zAxis.low = lo - pad;
    zAxis.up = hi + pad;
}

QString ContourDiagram::colourName() const
{
    return Graphs.isEmpty() ? QString() : bare(Graphs.first()->Var);
}

int ContourDiagram::calcDiagram()
{
    zAxis.numGraphs = 0;   // (the colour bar: no right axis)
    m_grids.clear();
    for (const Graph* g : Graphs) m_grids << (m_byGraph.contains(g) ? m_byGraph.value(g) : gridFor(g));
    // The colour range: the first grid's, unless the z axis' is given.
    double lo = NaN, hi = NaN;
    if (!m_grids.isEmpty() && m_grids.first().ok())
        for (double v : m_grids.first().v)
            if (std::isfinite(v)) {
                lo = std::isfinite(lo) ? std::min(lo, v) : v;
                hi = std::isfinite(hi) ? std::max(hi, v) : v;
            }
    if (!zAxis.autoScale && zAxis.limit_max > zAxis.limit_min) {
        zAxis.low = zAxis.limit_min;
        zAxis.up = zAxis.limit_max;
    } else if (std::isfinite(lo)) {
        autoRange(lo, hi);
    } else {
        zAxis.low = 0;
        zAxis.up = 1;
    }
    ++m_generation;
    return RectDiagram::calcDiagram();
}

void ContourDiagram::createAxisLabels()
{
    const QString x = xAxis.Label, y = yAxis.Label;
    if (!m_grids.isEmpty() && m_grids.first().ok()) {
        if (x.isEmpty()) xAxis.Label = bare(m_grids.first().xName);
        if (y.isEmpty()) yAxis.Label = bare(m_grids.first().yName);
    }
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

QImage ContourDiagram::render(const QSize& pixels) const
{
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (m_grids.isEmpty() || !m_grids.first().ok() || !(xAxis.up != xAxis.low) || !(yAxis.up != yAxis.low)) return image;
    const Grid& g = m_grids.first();
    const int w = pixels.width(), h = pixels.height();
    // Each column's x and each row's y (the inverse of the axes' scales),
    // their cells and how far into them.
    const auto along = [](const Axis& a, double f) {
        return a.log ? a.low * std::pow(a.up / a.low, f) : a.low + f * (a.up - a.low);
    };
    QVector<int> ci(w, -1), rj(h, -1);
    QVector<double> cs(w), rt(h);
    for (int c = 0; c < w; ++c)
        if (!cellOf(g.x, along(xAxis, (c + 0.5) / w), xAxis.log, &ci[c], &cs[c])) ci[c] = -1;
    for (int r = 0; r < h; ++r)
        if (!cellOf(g.y, along(yAxis, 1.0 - (r + 0.5) / h), yAxis.log, &rj[r], &rt[r])) rj[r] = -1;
    const double span = zAxis.up - zAxis.low;
    const bool band = hasBand();
    // Hatched: a stripe every 8 pixels (of the picture as shown).
    const int stripe = std::max(5, int(std::lround(10.0 * w / std::max(1, x2))));
    const QRgb hatch = qRgb(45, 45, 45);
    const QColor grey(150, 150, 150);
    for (int r = 0; r < h; ++r) {
        if (rj.at(r) < 0) continue;
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(r));
        const int j = rj.at(r);
        const double t = rt.at(r);
        for (int c = 0; c < w; ++c) {
            const int i = ci.at(c);
            if (i < 0) continue;
            const double s = cs.at(c);
            const double a = g.at(i, j), b = g.at(i + 1, j), cc = g.at(i + 1, j + 1), d = g.at(i, j + 1);
            if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(cc) || !std::isfinite(d)) continue;
            const double v = (1 - s) * (1 - t) * a + s * (1 - t) * b + s * t * cc + (1 - s) * t * d;
            const bool fails = band && !passes(v);
            if (fails && (c + r) % stripe < std::max(1, stripe / 5)) {
                row[c] = hatch;
                continue;
            }
            if (!filled) {
                if (fails) row[c] = qRgba(90, 90, 90, 60);
                continue;
            }
            QColor colour = colourAt(span > 0 ? (v - zAxis.low) / span : 0.5);
            // (What fails, faded towards grey under its hatching.)
            if (fails)
                colour = QColor((colour.red() + grey.red()) / 2, (colour.green() + grey.green()) / 2, (colour.blue() + grey.blue()) / 2);
            row[c] = colour.rgba();
        }
    }
    return image;
}

void ContourDiagram::paintBehindGraphs(QPainter* painter)
{
    if (x2 <= 0 || y2 <= 0) return;
    // (In its coordinates: origin at the lower left corner, y up.) The map,
    // drawn once at the size it is shown at.
    const QTransform t = painter->deviceTransform();
    const double sx = std::hypot(t.m11(), t.m12()), sy = std::hypot(t.m21(), t.m22());
    auto side = [](double v) { return std::isfinite(v) ? int(std::clamp(std::round(v), 1.0, double(kMaxSide))) : 1; };
    int w = side(x2 * sx), h = side(y2 * sy);
    if (qint64(w) * h > kMaxPixels) {
        const double f = std::sqrt(double(kMaxPixels) / (qint64(w) * h));
        w = std::max(1, int(w * f));
        h = std::max(1, int(h * f));
    }
    if (m_image.size() != QSize(w, h) || m_imageGeneration != m_generation) {
        m_image = render(QSize(w, h));
        m_imageGeneration = m_generation;
    }
    painter->save();
    painter->setClipRect(QRectF(0.0, 0.0, x2, y2));
    painter->save();
    painter->scale(1.0, -1.0);   // the image's rows go down
    painter->drawImage(QRectF(0.0, -y2, x2, y2), m_image);
    painter->restore();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // The iso-lines: the map's dark, the others' in their colours, dashed;
    // labelled apart from each other.
    const QFontMetricsF fm(painter->font());
    QList<QRectF> placed;
    // (Clear of the share that passes, in the upper left corner.)
    if (hasBand()) placed << QRectF(0, y2 - fm.height() - 8, fm.horizontalAdvance(QObject::tr("passes at 100 % of the points")) + 12, fm.height() + 8);
    const QColor paper = qucs_s::ink::paper();
    for (int k = 0; k < m_grids.size() && k < Graphs.size(); ++k) {
        const Grid& g = m_grids.at(k);
        if (!g.ok()) continue;
        double lo = NaN, hi = NaN;
        if (k == 0) {
            lo = zAxis.low;
            hi = zAxis.up;
        } else {
            for (double v : g.v)
                if (std::isfinite(v)) {
                    lo = std::isfinite(lo) ? std::min(lo, v) : v;
                    hi = std::isfinite(hi) ? std::max(hi, v) : v;
                }
        }
        const QColor ink = k == 0 && filled ? QColor(20, 20, 20, 170) : qucs_s::ink::on(Graphs.at(k)->Color);
        const QPen pen(ink, std::max(1, Graphs.at(k)->Thick), k == 0 ? Qt::SolidLine : Qt::DashLine, Qt::RoundCap);
        for (double level : levelsIn(lo, hi, levels)) {
            const QVector<QLineF> segments = isoLine(g, level);
            painter->setPen(pen);
            for (const QLineF& s : segments) painter->drawLine(s);
            if (!labels || segments.isEmpty()) continue;
            const QString text = numberText(k == 0 ? &zAxis : &yAxis, level);
            const QSizeF size(fm.horizontalAdvance(text) + 4, fm.height());
            int count = 0;
            for (int s = int(segments.size()) / 2, n = 0; n < segments.size() && count < 3; ++n, s = (s + 7) % int(segments.size())) {
                const QPointF mid = segments.at(s).center();
                const QRectF box(mid.x() - size.width() / 2, mid.y() - size.height() / 2, size.width(), size.height());
                if (!QRectF(0, 0, x2, y2).contains(box)) continue;
                bool clear = true;
                for (const QRectF& other : std::as_const(placed))
                    if (other.adjusted(-30, -12, 30, 12).intersects(box)) clear = false;
                if (!clear) continue;
                placed << box;
                ++count;
                painter->save();
                painter->translate(mid);
                painter->scale(1, -1);
                painter->setPen(Qt::NoPen);
                QColor back = paper;
                back.setAlpha(200);
                painter->setBrush(back);
                painter->drawRect(QRectF(-size.width() / 2, -size.height() / 2, size.width(), size.height()));
                painter->setPen(k == 0 ? QColor(20, 20, 20) : ink);
                painter->drawText(QPointF(-size.width() / 2 + 2, -size.height() / 2 + fm.ascent()), text);
                painter->restore();
            }
        }
    }
    // The pass band's edges, bold.
    if (!m_grids.isEmpty() && m_grids.first().ok()) {
        painter->setPen(QPen(QColor(0, 0, 0), 2.5, Qt::SolidLine, Qt::RoundCap));
        for (double edge : {passMin, passMax})
            if (std::isfinite(edge))
                for (const QLineF& s : isoLine(m_grids.first(), edge)) painter->drawLine(s);
    }
    painter->restore();
}

QRectF ContourDiagram::paintedRect(const QFontMetricsF& metrics) const
{
    // (y down.) The colour bar and its numbers right of the frame.
    QRectF r = RectDiagram::paintedRect(metrics);
    double widest = 0;
    for (double v : levelsIn(zAxis.low, zAxis.up, 6)) widest = std::max(widest, metrics.horizontalAdvance(numberText(&zAxis, v)));
    widest = std::max({widest, metrics.horizontalAdvance(numberText(&zAxis, threeDigits(zAxis.low))),
                       metrics.horizontalAdvance(numberText(&zAxis, threeDigits(zAxis.up)))});
    return r.united(QRectF(x2, -y2 - metrics.height(), 30 + widest + metrics.height() + 6, y2 + 2 * metrics.height()).translated(cx, cy));
}

void ContourDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    // (y down.) The colour bar: the map up the z axis' range, its numbers,
    // the pass band's edges across it, the first graph's name beside it.
    painter->save();
    const QFontMetricsF fm(painter->font());
    const QRectF bar(x2 + 10, -y2, 14, y2);
    if (filled) {
        QLinearGradient gradient(bar.bottomLeft(), bar.topLeft());
        for (int i = 0; i <= 10; ++i) gradient.setColorAt(i / 10.0, colourAt(i / 10.0));
        painter->fillRect(bar, gradient);
    }
    painter->setPen(QPen(colors.of(Part::Frame), 0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(bar);
    const double span = zAxis.up - zAxis.low;
    const auto yOf = [&](double v) { return span > 0 ? -(v - zAxis.low) / span * y2 : -y2 / 2.0; };
    double widest = 0;
    painter->setPen(colors.of(Part::RightAxis));
    QVector<double> ticks = levelsIn(zAxis.low, zAxis.up, 6);
    ticks.prepend(threeDigits(zAxis.low));
    ticks.append(threeDigits(zAxis.up));
    double lastY = 1e9;
    for (int i = 0; i < ticks.size(); ++i) {
        const double v = ticks.at(i), y = yOf(v);
        // (The ends' numbers left out when a round one is near.)
        if ((i == ticks.size() - 1 && std::abs(lastY - y) < fm.height()) || (i == 1 && std::abs(yOf(ticks.first()) - y) < fm.height() && ticks.size() > 2)) {
            if (i == 1) {
                lastY = y;
                painter->drawLine(QPointF(bar.right(), y), QPointF(bar.right() + 3, y));
                const QString text = numberText(&zAxis, v);
                widest = std::max(widest, fm.horizontalAdvance(text));
                painter->drawText(QPointF(bar.right() + 5, y + fm.ascent() / 2 - 1), text);
            }
            continue;
        }
        painter->drawLine(QPointF(bar.right(), y), QPointF(bar.right() + 3, y));
        const QString text = numberText(&zAxis, v);
        widest = std::max(widest, fm.horizontalAdvance(text));
        painter->drawText(QPointF(bar.right() + 5, y + fm.ascent() / 2 - 1), text);
        lastY = y;
    }
    painter->setPen(QPen(QColor(0, 0, 0), 2.5));
    for (double edge : {passMin, passMax})
        if (std::isfinite(edge) && span > 0 && edge >= zAxis.low && edge <= zAxis.up)
            painter->drawLine(QPointF(bar.left() - 3, yOf(edge)), QPointF(bar.right() + 3, yOf(edge)));
    // The name the colours are of: the z axis' label, else the first graph.
    QString name = zAxis.Label;
    if (name.isEmpty()) name = colourName();
    if (!name.isEmpty()) {
        painter->setPen(colors.of(Part::RightAxis));
        painter->translate(bar.right() + 8 + widest + fm.ascent(), -y2 / 2.0 + fm.horizontalAdvance(name) / 2);
        painter->rotate(-90);
        painter->drawText(QPointF(0, 0), name);
    }
    painter->restore();
    // The share that passes, in the upper left corner.
    const double share = passing();
    if (std::isfinite(share)) {
        painter->save();
        const QString text = QObject::tr("passes at %1 % of the points").arg(QString::number(100 * share, 'f', share > 0 && share < 0.01 ? 2 : 0));
        const QRectF box(4, -y2 + 4, fm.horizontalAdvance(text) + 8, fm.height() + 4);
        painter->setPen(QPen(colors.of(Part::LegendBorder), 1));
        painter->setBrush(colors.of(Part::LegendBackground));
        painter->drawRect(box);
        painter->setPen(share >= 1.0 ? QColor(0, 120, 0) : share <= 0.0 ? QColor(190, 0, 0) : colors.of(Part::LegendText));
        painter->drawText(QPointF(box.left() + 4, box.top() + 2 + fm.ascent()), text);
        painter->restore();
    }
}

QString ContourDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1 %2 %3 %4 %5 %6")
        .arg(levels)
        .arg(map)
        .arg(filled ? 1 : 0)
        .arg(labels ? 1 : 0)
        .arg(fieldText(passMin), fieldText(passMax));
}

void ContourDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    const int n = fields.value(0).toInt(&ok);
    levels = ok && n >= 0 && n <= MaxLevels ? n : 8;
    const int m = fields.value(1).toInt(&ok);
    map = ok && m >= Viridis && m <= Grey ? m : Viridis;
    filled = fields.value(2) != QLatin1String("0");
    labels = fields.value(3) != QLatin1String("0");
    passMin = fieldValue(fields.value(4));
    passMax = fieldValue(fields.value(5));
}
