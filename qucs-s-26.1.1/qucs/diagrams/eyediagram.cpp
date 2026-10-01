/*
 * eyediagram.cpp - the eye diagram of a serial data signal (see
 * eyediagram.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "eyediagram.h"
#include "dataset.h"
#include "ink.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace {

namespace eye = qucs_s::eye;
namespace ds = qucs_s::dataset;

QString tr(const char* text)
{
    return QCoreApplication::translate("EyeDiagram", text);
}

QString fieldText(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 12) : QStringLiteral("-");
}

double fieldValue(const QString& text)
{
    bool ok = false;
    const double v = text.toDouble(&ok);
    return ok && std::isfinite(v) ? v : eye::NaN;
}

// "ngspice/tran.v(rx)" is "v(rx)", as the other diagrams' labels leave out
// the simulator and the analysis.
QString shortName(const QString& var)
{
    QString name = var;
    if (name.contains(QLatin1Char('/'))) name = name.section(QLatin1Char('/'), 1);
    const int dot = int(name.indexOf(QLatin1Char('.')));
    const int paren = int(name.indexOf(QLatin1Char('(')));
    if (dot > 0 && (paren < 0 || dot < paren)) name = name.mid(dot + 1);
    return name;
}

QString unitOf(const Graph* g)
{
    return ds::unitOf(g->Var.mid(g->Var.lastIndexOf(QLatin1Char('/')) + 1));
}

// Each curve of a graph (one per value of a swept parameter): its x, and
// its values (a complex one by its magnitude).
QList<ds::Curve> curvesOf(const Graph* g)
{
    QList<ds::Curve> out;
    const DataX* x = g->axis(0);
    if (g->cPointsY == nullptr || x == nullptr || x->Points == nullptr || x->count <= 0 || g->countY <= 0) return out;
    const double* p = g->cPointsY;
    for (int b = 0; b < g->countY; ++b) {
        ds::Curve c;
        c.x.reserve(x->count);
        c.y.reserve(x->count);
        for (int i = 0; i < x->count; ++i, p += 2) {
            c.x << x->Points[i];
            c.y << (std::fabs(p[1]) > 1e-250 ? std::hypot(p[0], p[1]) : p[0]);
        }
        out << c;
    }
    return out;
}

// A sampling oscilloscope's colours for how often a point is passed, from
// rarely (blue) to most often (red) - the "turbo" map, sampled.
QColor heat(double t)
{
    static const int stops[][3] = {{48, 18, 59},   {68, 81, 191},  {70, 134, 251}, {40, 187, 236},
                                   {27, 229, 181}, {114, 254, 94}, {182, 246, 53}, {237, 207, 57},
                                   {251, 151, 39}, {225, 81, 10},  {122, 4, 3}};
    // (The rarest not the near-black the map starts with.)
    const double at = (0.12 + 0.88 * std::clamp(t, 0.0, 1.0)) * 10.0;
    const int i = std::min(9, int(at));
    const double f = at - i;
    return QColor(int(stops[i][0] + f * (stops[i + 1][0] - stops[i][0])), int(stops[i][1] + f * (stops[i + 1][1] - stops[i][1])),
                  int(stops[i][2] + f * (stops[i + 1][2] - stops[i][2])));
}

// The part of a-b within [0, w] x [0, h] (Liang-Barsky); false when none.
bool clipped(double& x0, double& y0, double& x1, double& y1, double w, double h)
{
    const double dx = x1 - x0, dy = y1 - y0;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {x0, w - x0, y0, h - y0};
    double t0 = 0.0, t1 = 1.0;
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return false;
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0.0) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    const double ax = x0 + t0 * dx, ay = y0 + t0 * dy;
    x1 = x0 + t1 * dx;
    y1 = y0 + t1 * dy;
    x0 = ax;
    y0 = ay;
    return true;
}

// Each pixel the segment a-b of a trace passes counted once: \a last is
// the one the trace was last counted in. (Many points close together - a
// simulator steps short at each corner of its sources - are one pass.)
void accumulate(QVector<float>& hits, int w, int h, QPointF a, QPointF b, qsizetype& last)
{
    double x0 = a.x(), y0 = a.y(), x1 = b.x(), y1 = b.y();
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return;
    if (!clipped(x0, y0, x1, y1, w, h)) return;
    const double dx = x1 - x0, dy = y1 - y0;
    const int steps = std::max(1, int(std::ceil(std::max(std::abs(dx), std::abs(dy)))));
    for (int k = 0; k < steps; ++k) {
        const double t = double(k) / steps;
        const int xi = std::min(w - 1, int(x0 + t * dx));
        const int yi = std::min(h - 1, int(y0 + t * dy));
        if (xi < 0 || yi < 0) continue;
        const qsizetype at = qsizetype(yi) * w + xi;
        if (at == last) continue;
        hits[at] += 1.0f;
        last = at;
    }
}

// A line with an arrowhead at each end, \a head long.
void doubleArrow(QPainter* painter, QPointF a, QPointF b, double head)
{
    painter->drawLine(a, b);
    const QLineF line(a, b);
    if (line.length() < 2.5 * head) return;
    const QPointF u = (b - a) / line.length(), n(-u.y(), u.x());
    for (const auto& [tip, dir] : {std::pair{a, u}, std::pair{b, -u}}) {
        painter->drawLine(tip, tip + dir * head + n * head * 0.5);
        painter->drawLine(tip, tip + dir * head - n * head * 0.5);
    }
}

// Words of \a text a line each, so many characters at most.
QStringList wrapped(const QString& text, int width = 46)
{
    QStringList out;
    QString line;
    for (const QString& word : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (!line.isEmpty() && line.size() + 1 + word.size() > width) {
            out << line;
            line = QStringLiteral("  ");
        }
        line += (line.trimmed().isEmpty() ? QString() : QStringLiteral(" ")) + word;
    }
    if (!line.trimmed().isEmpty()) out << line;
    return out;
}

constexpr qreal kBoxGap = 18.0, kPad = 4.0, kSwatch = 10.0, kSwatchGap = 5.0;
constexpr int kMaxSide = 3000;
constexpr qint64 kMaxPixels = 6000000;

} // namespace

EyeDiagram::EyeDiagram(int cx, int cy)
    : RectDiagram(cx, cy), ui(eye::NaN), start(eye::NaN), threshold(eye::NaN), maskWidth(eye::NaN), maskHeight(eye::NaN),
      m_ui(eye::NaN)
{
    Name = "Eye";
    // Times of a few picoseconds: 50p, not 5e-11.
    notation = qucs_s::numberformat::Notation::Engineering;
    xAxis.log = yAxis.log = zAxis.log = false;
    calcDiagram();
}

Diagram* EyeDiagram::newOne()
{
    return new EyeDiagram();
}

Element* EyeDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Eye Diagram");
    BitmapFile = (char*) "eye";
    if (getNewOne) return new EyeDiagram();
    return nullptr;
}

QList<Diagram::Part> EyeDiagram::themeParts() const
{
    QList<Part> parts = RectDiagram::themeParts();
    parts.removeAll(Part::RightAxis);
    return parts;
}

QString EyeDiagram::engineering(double value, const QString& unit)
{
    if (!std::isfinite(value)) return QStringLiteral("-");
    // Below a thousandth of a femto is rounding: no jitter at all.
    if (std::abs(value) < 1e-18) value = 0.0;
    static const char* const prefixes[] = {"f", "p", "n", "µ", "m", "", "k", "M", "G", "T"};
    int e3 = value == 0.0 ? 0 : std::clamp(int(std::floor(std::log10(std::abs(value)) / 3.0)), -5, 4);
    QString mantissa = QString::number(value / std::pow(10.0, 3 * e3), 'g', 4);
    // 999.96 written as 1000: one prefix up.
    if (std::abs(mantissa.toDouble()) >= 1000.0 && e3 < 4) mantissa = QString::number(value / std::pow(10.0, 3 * ++e3), 'g', 4);
    const QString suffix = QString::fromUtf8(prefixes[e3 + 5]) + unit;
    return suffix.isEmpty() ? mantissa : mantissa + QStringLiteral(" ") + suffix;
}

void EyeDiagram::getAxisLimits(Graph* pg)
{
    pg->yAxisNo = 0;   // one axis, the signal's
    ++yAxis.numGraphs;
    const DataX* x = pg->axis(0);
    if (pg->cPointsY == nullptr || x == nullptr || x->Points == nullptr) return;
    // The values the eye shows: from its start on.
    const double* p = pg->cPointsY;
    for (int b = 0; b < pg->countY; ++b)
        for (int i = 0; i < x->count; ++i, p += 2) {
            if (std::isfinite(start) && x->Points[i] < start) continue;
            const double v = std::fabs(p[1]) > 1e-250 ? std::hypot(p[0], p[1]) : p[0];
            if (!std::isfinite(v)) continue;
            yAxis.min = std::min(yAxis.min, v);
            yAxis.max = std::max(yAxis.max, v);
        }
}

void EyeDiagram::analyse()
{
    m_results.clear();
    m_ui = std::isfinite(ui) && ui > 0.0 ? ui : eye::NaN;
    eye::Options o;
    o.start = start;
    o.levels = levels;
    o.threshold = threshold;
    o.maskWidth = maskWidth;
    o.maskHeight = maskHeight;
    auto one = [&](int i) {
        const QList<qucs_s::dataset::Curve> curves = curvesOf(Graphs.at(i));
        if (curves.isEmpty() || curves.first().x.size() < 2) {
            eye::Result r;
            r.error = tr("no data: simulate, or check the variable's name");
            return r;
        }
        o.ui = m_ui;   // the first graph's, for the others
        return eye::analyse(curves.first(), o);
    };
    for (int i = 0; i < Graphs.size(); ++i) {
        m_results << one(i);
        if (!std::isfinite(m_ui) && m_results.last().ok()) {
            m_ui = m_results.last().ui;
            // The graphs before, whose UI could not be told: at this one's.
            for (int j = 0; j < i; ++j)
                if (!m_results.at(j).ok()) m_results[j] = one(j);
        }
    }
}

int EyeDiagram::calcDiagram()
{
    xAxis.log = yAxis.log = zAxis.log = false;
    zAxis.numGraphs = 0;
    analyse();
    // Time across it: the windows, a few UIs long.
    if (xAxis.autoScale) {
        xAxis.min = 0.0;
        xAxis.max = std::isfinite(m_ui) ? span * m_ui : 1.0;
    }
    ++m_generation;
    return RectDiagram::calcDiagram();
}

QString EyeDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1 %2 %3 %4 %5 %6 %7 %8 %9")
        .arg(fieldText(ui))
        .arg(span)
        .arg(fieldText(start))
        .arg(levels)
        .arg(fieldText(threshold))
        .arg(drawn)
        .arg(measurements ? 1 : 0)
        .arg(fieldText(maskWidth), fieldText(maskHeight));
}

void EyeDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    ui = fieldValue(fields.value(0));
    if (!(ui > 0.0)) ui = eye::NaN;
    const int s = fields.value(1).toInt(&ok);
    span = ok && s >= 1 && s <= MaxSpan ? s : 2;
    start = fieldValue(fields.value(2));
    const int l = fields.value(3).toInt(&ok);
    levels = ok && l == 4 ? 4 : 2;
    threshold = fieldValue(fields.value(4));
    const int d = fields.value(5).toInt(&ok);
    drawn = ok && d == Traces ? Traces : Density;
    const int m = fields.value(6).toInt(&ok);
    measurements = !ok || m != 0;
    maskWidth = fieldValue(fields.value(7));
    maskHeight = fieldValue(fields.value(8));
    if (!(maskWidth > 0.0 && maskWidth <= 1.0) || !(maskHeight > 0.0)) maskWidth = maskHeight = eye::NaN;
}

void EyeDiagram::createAxisLabels()
{
    // Under the x axis, what the windows are - unless it has a label of
    // its own.
    const QString x = xAxis.Label;
    if (xAxis.Label.isEmpty())
        xAxis.Label = std::isfinite(m_ui) ? tr("time, %1 UI of %2").arg(span).arg(engineering(m_ui, QStringLiteral("s")))
                                          : tr("time");
    Diagram::createAxisLabels();
    xAxis.Label = x;
}

QList<EyeDiagram::Line> EyeDiagram::lines() const
{
    QList<Line> out;
    if (!measurements) return out;
    auto add = [&out](const QString& text) {
        for (const QString& l : wrapped(text)) out << Line{l, -1};
    };
    for (int i = 0; i < Graphs.size() && i < m_results.size(); ++i) {
        const Graph* g = Graphs.at(i);
        const eye::Result& r = m_results.at(i);
        out << Line{shortName(g->Var), i};
        // A sweep's curves are all drawn; its first is measured.
        if (g->countY > 1 && g->cPointsY != nullptr) add(tr("measured: the first of its %1 curves").arg(g->countY));
        if (!r.ok()) {
            add(tr("no eye: %1").arg(r.error));
            continue;
        }
        const QString unit = unitOf(g), s = QStringLiteral("s");
        add(tr("UI %1%2").arg(engineering(r.ui, s), r.uiEstimated ? tr(", from the crossings") : QString()));
        if (r.eyes.size() == 1) {
            const eye::Eye& e = r.eyes.first();
            add(tr("height %1").arg(engineering(e.height, unit)));
            add(tr("width %1 (%2 % UI)").arg(engineering(e.width, s)).arg(e.width / r.ui * 100.0, 0, 'f', 1));
            add(tr("jitter %1 rms, %2 p-p").arg(engineering(e.jitterRms, s), engineering(e.jitterPp, s)));
            add(tr("levels %1, %2").arg(engineering(r.levels.at(0), unit), engineering(r.levels.at(1), unit)));
            if (std::isfinite(e.q)) add(tr("Q %1").arg(QString::number(e.q, 'g', 3)));
        } else {
            for (int k = r.eyes.size() - 1; k >= 0; --k) {   // the top eye first, as drawn
                const eye::Eye& e = r.eyes.at(k);
                add(tr("eye %1: height %2, width %3").arg(k + 1).arg(engineering(e.height, unit), engineering(e.width, s)));
            }
            QStringList levels;
            for (double l : r.levels) levels << engineering(l, unit);
            add(tr("levels %1").arg(levels.join(QStringLiteral(", "))));
        }
        if (r.maskHits >= 0)
            add(r.maskHits == 0 ? tr("mask: no UI of %1 through it").arg(r.symbols)
                                : tr("mask: %1 of %2 UIs through it").arg(r.maskHits).arg(r.symbols));
        for (const QString& note : r.notes) add(note);
    }
    return out;
}

QStringList EyeDiagram::measurementLines() const
{
    QStringList out;
    for (const Line& l : lines()) out << l.text;
    return out;
}

QRectF EyeDiagram::boxRect(const QFontMetricsF& metrics) const
{
    const QList<Line> ls = lines();
    if (ls.isEmpty()) return QRectF();
    qreal textWidth = 0.0;
    for (const Line& l : ls) textWidth = std::max(textWidth, metrics.horizontalAdvance(l.text));
    return QRectF(x2 + kBoxGap, -y2, kPad + kSwatch + kSwatchGap + textWidth + kPad, kPad + ls.size() * metrics.height() + kPad);
}

QRectF EyeDiagram::paintedRect(const QFontMetricsF& metrics) const
{
    QRectF r = RectDiagram::paintedRect(metrics);
    const QRectF box = boxRect(metrics);
    if (!box.isNull()) r |= box.translated(cx, cy);
    return r;
}

QImage EyeDiagram::render(const QSize& pixels) const
{
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (!std::isfinite(m_ui) || !(xAxis.up != xAxis.low) || !(yAxis.up != yAxis.low)) return image;
    const int w = pixels.width(), h = pixels.height();
    const double ax = w / (xAxis.up - xAxis.low), ay = h / (yAxis.up - yAxis.low);
    auto toPixel = [&](const QPointF& p) { return QPointF((p.x() - xAxis.low) * ax, h - (p.y() - yAxis.low) * ay); };

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool several = Graphs.size() > 1;
    for (int i = 0; i < Graphs.size(); ++i) {
        const Graph* g = Graphs.at(i);
        const QList<qucs_s::dataset::Curve> curves = curvesOf(g);
        if (curves.isEmpty()) continue;
        const eye::Result r = m_results.value(i);
        const double from = std::isfinite(start) ? start : curves.first().x.value(0);
        // The eye's centre in the middle - or, with no eye found, the
        // windows from the start.
        const double origin = r.ok() ? eye::originFor(r, span) : from;
        const QColor colour = qucs_s::ink::on(g->Color);
        if (drawn == Traces) {
            // Fainter the more there are, so that where many pass is darker.
            double windows = 0.0;
            for (const qucs_s::dataset::Curve& c : curves)
                if (c.x.size() > 1) windows += std::max(0.0, (c.x.last() - std::max(from, c.x.first())) / m_ui) * span;
            QColor pen = colour;
            pen.setAlpha(int(std::clamp(255.0 * 4.0 / std::sqrt(std::max(1.0, windows)), 16.0, 255.0)));
            painter.setPen(QPen(pen, std::max(1.0, double(g->Thick)) * w / std::max(1, x2)));
            QPolygonF line;
            for (const qucs_s::dataset::Curve& c : curves)
                eye::fold(c, from, m_ui, origin, span, [&](const QVector<QPointF>& points) {
                    line.resize(0);
                    for (const QPointF& p : points) line << toPixel(p);
                    painter.drawPolyline(line);
                });
            continue;
        }
        // How many traces pass each pixel.
        QVector<float> hits(qsizetype(w) * h, 0.0f);
        for (const qucs_s::dataset::Curve& c : curves)
            eye::fold(c, from, m_ui, origin, span, [&](const QVector<QPointF>& points) {
                qsizetype last = -1;
                for (int k = 1; k < points.size(); ++k) accumulate(hits, w, h, toPixel(points.at(k - 1)), toPixel(points.at(k)), last);
            });
        const float most = hits.isEmpty() ? 0.0f : *std::max_element(hits.cbegin(), hits.cend());
        if (most <= 0.0f) continue;
        QImage layer(pixels, QImage::Format_ARGB32);
        layer.fill(Qt::transparent);
        const double scale = 1.0 / std::log1p(double(most));
        for (int y = 0; y < h; ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(layer.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const float v = hits.at(qsizetype(y) * w + x);
                if (v <= 0.0f) continue;
                const double t = std::log1p(double(v)) * scale;
                if (several) {   // each graph in its colour, denser more opaque
                    QColor c = colour;
                    c.setAlphaF(0.25 + 0.75 * t);
                    row[x] = c.rgba();
                } else {
                    row[x] = heat(t).rgba();
                }
            }
        }
        painter.drawImage(0, 0, layer);
    }
    return image;
}

void EyeDiagram::paintBehindGraphs(QPainter* painter)
{
    if (!std::isfinite(m_ui) || x2 <= 0 || y2 <= 0) return;
    // Drawn once at the size it is shown at (and again when that, the data
    // or the paper changes).
    const QTransform t = painter->deviceTransform();
    const double sx = std::hypot(t.m11(), t.m12()), sy = std::hypot(t.m21(), t.m22());
    // (Clamped as doubles: a huge or nan scale is no int.)
    auto side = [](double v) { return std::isfinite(v) ? int(std::clamp(std::round(v), 1.0, double(kMaxSide))) : 1; };
    int w = side(x2 * sx), h = side(y2 * sy);
    if (qint64(w) * h > kMaxPixels) {
        const double f = std::sqrt(double(kMaxPixels) / (qint64(w) * h));
        w = std::max(1, int(w * f));
        h = std::max(1, int(h * f));
    }
    const QRgb paper = qucs_s::ink::paper().rgba();
    if (m_image.size() != QSize(w, h) || m_imageGeneration != m_generation || m_imagePaper != paper) {
        m_image = render(QSize(w, h));
        m_imageGeneration = m_generation;
        m_imagePaper = paper;
    }
    painter->save();
    painter->setClipRect(QRectF(0.0, 0.0, x2, y2));
    painter->scale(1.0, -1.0);   // the image's rows go down
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(QRectF(0.0, -y2, x2, y2), m_image);
    painter->restore();
    if (measurements) paintMarks(painter);
}

void EyeDiagram::paintMarks(QPainter* painter) const
{
    // On the first graph's eye: its threshold, its centre, its height and
    // width, and the mask.
    if (m_results.isEmpty() || !m_results.first().ok() || !(xAxis.up != xAxis.low) || !(yAxis.up != yAxis.low)) return;
    const eye::Result& r = m_results.first();
    auto X = [this](double v) { return (v - xAxis.low) / (xAxis.up - xAxis.low) * x2; };
    auto Y = [this](double v) { return (v - yAxis.low) / (yAxis.up - yAxis.low) * y2; };
    const double centre = span * r.ui / 2.0;

    painter->save();
    painter->setClipRect(QRectF(0.0, 0.0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = qucs_s::ink::on(QColor(25, 25, 25));

    if (r.maskHits >= 0) {
        const QPolygonF shape = eye::mask(maskWidth, maskHeight);
        QColor fill = r.maskHits > 0 ? QColor(220, 40, 40) : QColor(60, 110, 200);
        QColor edge = fill.darker(130);
        fill.setAlpha(r.maskHits > 0 ? 110 : 80);
        painter->setPen(QPen(qucs_s::ink::on(edge), 1.2));
        painter->setBrush(fill);
        for (const eye::Eye& e : r.eyes) {
            QPolygonF at;
            for (const QPointF& p : shape) at << QPointF(X(centre + p.x() * r.ui), Y(e.threshold + p.y()));
            painter->drawPolygon(at);
        }
        painter->setBrush(Qt::NoBrush);
    }

    QPen dashed(ink, 1.0);
    dashed.setDashPattern({5.0, 4.0});
    painter->setPen(dashed);
    for (const eye::Eye& e : r.eyes) painter->drawLine(QPointF(0.0, Y(e.threshold)), QPointF(x2, Y(e.threshold)));
    QPen dotted(ink, 1.0);
    dotted.setDashPattern({1.5, 3.0});
    painter->setPen(dotted);
    painter->drawLine(QPointF(X(centre), 0.0), QPointF(X(centre), y2));

    painter->setPen(QPen(ink, 1.6));
    for (const eye::Eye& e : r.eyes) {
        if (e.height > 0.0) doubleArrow(painter, QPointF(X(centre), Y(e.inner)), QPointF(X(centre), Y(e.outer)), 6.0);
        if (e.width > 0.0)
            doubleArrow(painter, QPointF(X(centre + (e.phase + e.latest) * r.ui), Y(e.threshold)),
                        QPointF(X(centre + (e.phase + 1.0 + e.earliest) * r.ui), Y(e.threshold)), 6.0);
    }
    painter->restore();
}

void EyeDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    const QList<Line> ls = lines();
    if (ls.isEmpty()) return;
    const QFontMetricsF fm(painter->font());
    const QRectF box = boxRect(fm);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // In the legend's colours.
    painter->setPen(QPen(colors.of(Part::LegendBorder), 1));
    painter->setBrush(colors.of(Part::LegendBackground));
    painter->drawRect(box);
    const qucs_s::ink::Paper paper(colors.legend);
    qreal top = box.top() + kPad;
    const qreal text = box.left() + kPad + kSwatch + kSwatchGap;
    for (const Line& l : ls) {
        if (l.graph >= 0 && l.graph < Graphs.size()) {
            const qreal mid = top + fm.height() / 2.0;
            const QColor c = qucs_s::ink::on(Graphs.at(l.graph)->Color);
            painter->setPen(QPen(c, 1));
            painter->setBrush(c);
            painter->drawRect(QRectF(box.left() + kPad, mid - kSwatch / 2, kSwatch, kSwatch));
        }
        painter->setPen(colors.of(Part::LegendText));
        painter->drawText(QPointF(text, top + fm.ascent()), l.text);
        top += fm.height();
    }
    painter->restore();
}
