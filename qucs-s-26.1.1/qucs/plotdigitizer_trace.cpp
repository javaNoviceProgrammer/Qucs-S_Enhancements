/*
 * plotdigitizer_trace.cpp - a plot's curves and markers: read from the paths
 *                           of the page where they are vector art, followed
 *                           in a picture of it where they are not
 *                           (plotdigitizer.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "plotdigitizer.h"
#include "plotdigitizer_p.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfSelection>

#include <algorithm>
#include <cmath>
#include <vector>

namespace qucs_s::digitizer {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("PlotDigitizer", text); }

} // namespace

double colourDistance(const QColor& a, const QColor& b)
{
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
}

bool wanted(const Options& o, const QColor& c)
{
    if (o.colours.isEmpty()) return true;
    for (const QColor& w : o.colours)
        if (colourDistance(w, c) < 90) return true;
    return false;
}

bool achromatic(const QColor& c) { return std::max({c.red(), c.green(), c.blue()}) - std::min({c.red(), c.green(), c.blue()}) < 24; }

double lengthOf(const QPolygonF& q)
{
    double l = 0;
    for (int i = 0; i + 1 < q.size(); ++i) l += QLineF(q.at(i), q.at(i + 1)).length();
    return l;
}

QPointF normal(QPointF d)
{
    const double n = std::hypot(d.x(), d.y());
    return n > 1e-12 ? d / n : QPointF(1, 0);
}

QPointF turned(QPointF d, double degrees)
{
    const double a = degrees * M_PI / 180, c = std::cos(a), s = std::sin(a);
    return {d.x() * c - d.y() * s, d.x() * s + d.y() * c};
}

// (QRectF::intersects() says no for a box with no height: a level line.)
bool overlaps(const QRectF& a, const QRectF& b)
{
    return a.right() >= b.left() && a.left() <= b.right() && a.bottom() >= b.top() && a.top() <= b.bottom();
}

QList<Phrase> phrasesOf(QList<Token> tokens)
{
    std::sort(tokens.begin(), tokens.end(), [](const Token& a, const Token& b) { return a.box.center().y() < b.box.center().y(); });
    QList<Phrase> out;
    QList<QList<Token>> lines;
    for (const Token& t : std::as_const(tokens)) {
        if (!lines.isEmpty() && std::abs(lines.last().first().box.center().y() - t.box.center().y()) <= std::max(2.5, 0.4 * t.box.height())) lines.last() << t;
        else lines << QList<Token>{t};
    }
    for (QList<Token>& line : lines) {
        std::sort(line.begin(), line.end(), [](const Token& a, const Token& b) { return a.box.left() < b.box.left(); });
        Phrase cur;
        for (const Token& t : std::as_const(line)) {
            if (!cur.text.isEmpty() && t.box.left() - cur.box.right() > 1.6 * std::max(t.box.height(), 4.0)) {
                out << cur;
                cur = Phrase{};
            }
            cur.text = cur.text.isEmpty() ? t.text : cur.text + QLatin1Char(' ') + t.text;
            cur.box = cur.box.isNull() ? t.box : cur.box.united(t.box);
        }
        if (!cur.text.isEmpty()) out << cur;
    }
    return out;
}

namespace {

// Keeps a run of points inside a box: its pieces, with the points where it
// crosses the edge (Liang and Barsky's).
void clipInto(const QPolygonF& line, const QRectF& box, bool closed, QList<QPointF>* out, QList<int>* breaks)
{
    bool open = false;
    const auto segment = [&](QPointF a, QPointF b) {
        double t0 = 0, t1 = 1;
        const QPointF d = b - a;
        const double ps[4] = {-d.x(), d.x(), -d.y(), d.y()};
        const double qs[4] = {a.x() - box.left(), box.right() - a.x(), a.y() - box.top(), box.bottom() - a.y()};
        for (int i = 0; i < 4; ++i) {
            if (ps[i] == 0) {
                if (qs[i] < 0) return false;
                continue;
            }
            const double r = qs[i] / ps[i];
            if (ps[i] < 0) t0 = std::max(t0, r);
            else t1 = std::min(t1, r);
            if (t0 > t1) return false;
        }
        const QPointF from = a + d * t0, to = a + d * t1;
        if (!open || out->isEmpty() || QLineF(out->last(), from).length() > 1e-6) {
            if (!out->isEmpty()) breaks->append(int(out->size()));
            out->append(from);
        }
        out->append(to);
        open = t1 >= 1;
        return true;
    };
    if (line.size() == 1) {
        if (box.contains(line.first())) out->append(line.first());
        return;
    }
    for (int i = 0; i + 1 < line.size(); ++i)
        if (!segment(line.at(i), line.at(i + 1))) open = false;
    if (closed && line.size() > 2 && !segment(line.last(), line.first())) open = false;
}

// --- From the paths --------------------------------------------------------

struct LinePiece {
    QPolygonF points;
    QColor colour;
    double width = 0;
    QList<double> dash;
    bool closed = false;
    bool curved = false;
    QRectF clip;
};

struct Stamp {
    QPointF at;
    QColor colour;
    double size = 0;
    int form = -1;
    QRectF box;
};

QString styleKey(const QColor& c, double width, const QList<double>& dash)
{
    QString k = c.name() + QLatin1Char('/') + QString::number(int(std::round(width * 20)));
    if (!dash.isEmpty()) {
        k += QLatin1Char('/');
        for (const double d : dash) k += QString::number(int(std::round(d * 4))) + QLatin1Char(',');
    }
    return k;
}

bool nearAMark(double position, const Axis& axis)
{
    for (const double m : axis.marks)
        if (std::abs(m - position) <= 0.8) return true;
    for (const Tick& t : axis.ticks)
        if (std::abs(t.position - position) <= 0.8) return true;
    return false;
}

// The share of points with ink under them, to catch a reading of the file
// that does not match what the page shows. (On the way, it is the cheap check
// that this reader and pdfium see the same page.)
double inkAgreement(const QImage& image, const QRectF& area, double scale, const QList<QPointF>& points, double width)
{
    if (image.isNull() || points.isEmpty()) return -1;
    const int stride = std::max<int>(1, int(points.size()) / 300);
    int asked = 0, inked = 0;
    const int radius = std::clamp(int(std::ceil(width * scale / 2)) + 1, 1, 6);
    const QPointF origin(std::floor(area.left() * scale), std::floor(area.top() * scale));
    for (int i = 0; i < points.size(); i += stride) {
        const QPointF p = points.at(i) * scale - origin;
        const int cx = int(std::floor(p.x())), cy = int(std::floor(p.y()));
        if (cx < radius || cy < radius || cx >= image.width() - radius || cy >= image.height() - radius) continue;
        ++asked;
        bool hit = false;
        for (int y = cy - radius; y <= cy + radius && !hit; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = cx - radius; x <= cx + radius && !hit; ++x) {
                const QRgb v = row[x];
                hit = (255 - qRed(v)) + (255 - qGreen(v)) + (255 - qBlue(v)) > 40;
            }
        }
        inked += hit;
    }
    return asked > 0 ? double(inked) / asked : -1;
}

bool extractVector(Source& source, Plot& plot, const Options& options, QStringList* notes)
{
    const pdf::vec::Graphics& g = source.graphics(plot.page);
    const QRectF frame = plot.pageBox();
    if (frame.isEmpty()) return false;
    const QRectF inside = frame.adjusted(-1.2, -1.2, 1.2, 1.2);
    const double markerMost = std::clamp(std::min(frame.width(), frame.height()) * 0.07, 12.0, 24.0);
    const double area = frame.width() * frame.height();
    const bool polar = plot.calibration.kind == Kind::Polar;

    QList<LinePiece> pieces;
    QList<Stamp> stamps;
    struct Box {
        QRectF box;
        QColor colour;
    };
    QList<Box> rectangles;
    QList<Series> areas;
    int gridLines = 0, outlined = 0;

    for (const pdf::vec::Path& p : g.paths) {
        const QColor colour = p.stroked ? p.stroke : p.fill;
        if (colour.lightness() >= 250 && !(p.stroked && p.filled && p.fill.lightness() < 250)) continue;
        // A path's parts that are all small, stroked or filled, make one
        // mark (a cross, a plus): checked as a whole first.
        QRectF whole;
        int vertexCount = 0;
        for (const pdf::vec::Part& part : p.parts) {
            whole = whole.isNull() ? part.points.boundingRect() : whole.united(part.points.boundingRect());
            vertexCount += int(part.points.size());
        }
        if (whole.isNull() || !overlaps(whole, inside)) continue;
        if (std::max(whole.width(), whole.height()) <= markerMost && std::max(whole.width(), whole.height()) >= 0.6 && p.parts.size() >= 2 && p.parts.size() <= 6 && vertexCount <= 64) {
            stamps << Stamp{p.form >= 0 ? p.origin : whole.center(), colour, std::max(whole.width(), whole.height()), p.form, whole};
            continue;
        }
        for (const pdf::vec::Part& part : p.parts) {
            const QRectF box = part.points.boundingRect();
            if (!overlaps(box, inside)) continue;
            const qsizetype n = part.points.size();
            const double extent = std::max(box.width(), box.height());
            const bool closedOrFilled = part.closed || p.filled;
            // A segment by itself, filled or not, is no marker: it is a tick
            // or a stroke, and comes to that below.
            const bool degenerate = box.width() <= 0.3 || box.height() <= 0.3;
            // A marker: small and closed.
            if (!degenerate && closedOrFilled && extent <= markerMost && extent >= 0.6 && n <= 256 && (p.form >= 0 || extent > 1.2 || part.curved)) {
                stamps << Stamp{p.form >= 0 ? p.origin : box.center(), p.filled ? p.fill : p.stroke, extent, p.form, box};
                continue;
            }
            if (part.rectangle || (n <= 5 && part.closed && !part.curved)) {
                // A frame, a panel, a bar, a legend's box.
                const double share = box.width() * box.height() / std::max(1.0, area);
                if (share > 0.6) continue;
                rectangles << Box{box, p.filled ? p.fill : p.stroke};
                continue;
            }
            if (p.filled && !p.stroked && !degenerate) {
                // An area under a curve, or a stroke written out as an outline.
                double twice = 0, perimeter = 0;
                for (int i = 0; i < n; ++i) {
                    const QPointF a = part.points.at(i), b = part.points.at((i + 1) % n);
                    twice += a.x() * b.y() - b.x() * a.y();
                    perimeter += QLineF(a, b).length();
                }
                if (perimeter > 0 && std::abs(twice) / 2 / perimeter < 1.2 && extent > markerMost) {
                    ++outlined;
                    continue;
                }
                if (extent > markerMost && wanted(options, p.fill)) {
                    Series s;
                    s.colour = p.fill;
                    s.shape = Series::Shape::Area;
                    s.origin = Series::Origin::Vector;
                    clipInto(part.points, inside, false, &s.points, &s.breaks);
                    if (s.points.size() >= 3) areas << s;
                }
                continue;
            }
            if (!p.stroked || n < 2) continue;
            // The frame, grid lines, ticks.
            if (n == 2 && !part.curved) {
                const QPointF a = part.points.first(), b = part.points.last();
                const double dx = std::abs(a.x() - b.x()), dy = std::abs(a.y() - b.y());
                const bool vertical = dx <= 0.25, horizontal = dy <= 0.25;
                // (A tick meets its edge, or crosses it.)
                const auto meets = [](double low, double high, double edge) { return low - 2.5 <= edge && edge <= high + 2.5; };
                if (vertical && dy < kMostTickLength && (meets(box.top(), box.bottom(), frame.top()) || meets(box.top(), box.bottom(), frame.bottom()))) continue;
                if (horizontal && dx < kMostTickLength && (meets(box.left(), box.right(), frame.left()) || meets(box.left(), box.right(), frame.right()))) continue;
                if (vertical && dy >= 0.5 * frame.height() && (std::abs(a.x() - frame.left()) < 1.5 || std::abs(a.x() - frame.right()) < 1.5)) continue;
                if (horizontal && dx >= 0.5 * frame.width() && (std::abs(a.y() - frame.top()) < 1.5 || std::abs(a.y() - frame.bottom()) < 1.5)) continue;
                if (!polar && ((vertical && dy >= 0.45 * frame.height() && nearAMark(a.x(), plot.xAxis)) || (horizontal && dx >= 0.45 * frame.width() && nearAMark(a.y(), plot.yAxis))) && achromatic(colour)) {
                    ++gridLines;
                    continue;
                }
            }
            LinePiece piece;
            piece.points = part.points;
            piece.colour = p.stroke;
            piece.width = p.width;
            piece.dash = p.dash;
            piece.closed = part.closed;
            piece.curved = part.curved;
            piece.clip = p.clip;
            pieces << piece;
        }
    }

    // --- Chains: a curve written in many pieces, a dashed one stroke by stroke.
    QList<QString> order;
    QHash<QString, QList<LinePiece>> groups;
    for (const LinePiece& piece : std::as_const(pieces)) {
        const QString key = styleKey(piece.colour, piece.width, piece.dash);
        if (!groups.contains(key)) order << key;
        groups[key] << piece;
    }
    struct Chain {
        QPolygonF points;
        QList<int> breaks;
        QColor colour;
        double width = 0;
        QList<double> dash;
        QRectF clip;
        bool strokeByStroke = false;
        bool closed = false;
        bool curved = false;
    };
    QList<Chain> chains;
    for (const QString& key : std::as_const(order)) {
        const QList<LinePiece>& group = groups.value(key);
        QList<double> lengths;
        for (const LinePiece& p : group) lengths << lengthOf(p.points);
        std::sort(lengths.begin(), lengths.end());
        const double medianLength = lengths.at(lengths.size() / 2);
        // Many short strokes in the content's order: a dashed line written out.
        const bool dashes = group.size() >= 6 && medianLength < 18;
        const double reach = dashes ? std::clamp(medianLength * 3, 1.5, 20.0) : 0.6;
        Chain cur;
        const auto push = [&] {
            if (!cur.points.isEmpty()) chains << cur;
            cur = Chain{};
        };
        for (const LinePiece& p : group) {
            bool joined = false;
            if (!cur.points.isEmpty() && !cur.closed && !p.closed) {
                const double gap = QLineF(cur.points.last(), p.points.first()).length();
                if (gap <= reach) {
                    joined = true;
                    if (gap > 0.6) cur.strokeByStroke = true;
                }
            }
            if (!joined) {
                push();
                cur.colour = p.colour;
                cur.width = p.width;
                cur.dash = p.dash;
                cur.clip = p.clip;
                cur.closed = p.closed;
            } else {
                // (Where they touch, one point.)
                if (QLineF(cur.points.last(), p.points.first()).length() < 1e-6) {
                    cur.points << p.points.mid(1);
                    cur.curved = cur.curved || p.curved;
                    continue;
                }
            }
            cur.points << p.points;
            cur.curved = cur.curved || p.curved;
        }
        push();
    }

    // Short strokes of one style left apart, crossing in pairs: + and x markers.
    {
        QHash<QString, QList<int>> shortOnes;
        for (int i = 0; i < chains.size(); ++i)
            if (chains.at(i).points.size() == 2 && lengthOf(chains.at(i).points) <= markerMost) shortOnes[chains.at(i).colour.name()] << i;
        QSet<int> used;
        for (auto it = shortOnes.cbegin(); it != shortOnes.cend(); ++it) {
            const QList<int>& ids = it.value();
            if (ids.size() < 4) continue;
            for (int a = 0; a < ids.size(); ++a) {
                if (used.contains(ids.at(a))) continue;
                const QPolygonF& pa = chains.at(ids.at(a)).points;
                const QPointF ma = (pa.at(0) + pa.at(1)) / 2;
                const QPointF da = pa.at(1) - pa.at(0);
                for (int b = a + 1; b < ids.size(); ++b) {
                    if (used.contains(ids.at(b))) continue;
                    const QPolygonF& pb = chains.at(ids.at(b)).points;
                    const QPointF mb = (pb.at(0) + pb.at(1)) / 2;
                    if (QLineF(ma, mb).length() > 0.25 * std::max(lengthOf(pa), 1.0)) continue;
                    const QPointF db = pb.at(1) - pb.at(0);
                    const double cross = std::abs(da.x() * db.y() - da.y() * db.x()) / std::max(1e-9, std::hypot(da.x(), da.y()) * std::hypot(db.x(), db.y()));
                    if (cross < 0.5) continue;
                    used.insert(ids.at(a));
                    used.insert(ids.at(b));
                    const QRectF box = pa.boundingRect().united(pb.boundingRect());
                    stamps << Stamp{(ma + mb) / 2, chains.at(ids.at(a)).colour, std::max(box.width(), box.height()), -1, box};
                    break;
                }
            }
        }
        if (!used.isEmpty()) {
            QList<int> ids(used.begin(), used.end());
            std::sort(ids.begin(), ids.end(), std::greater<int>());
            for (const int i : ids) chains.removeAt(i);
        }
    }

    // --- The legend: words with a sample of a line or a marker just before them.
    QHash<QString, QString> nameOfStyle, nameOfMarker;
    QList<QRectF> legendSamples;
    {
        const QRectF reach = frame.adjusted(-10, -60, 170, 80);
        QList<Token> words;
        for (const Token& t : source.tokens(plot.page, reach))
            if (!t.numeric || !frame.adjusted(-2, -2, 2, 2).contains(t.box.center()) == false) words << t;
        // (Numbers on the plot may be a legend's: "25 °C" comes in two tokens.)
        QList<Token> usable;
        for (const Token& t : std::as_const(words)) {
            const QPointF c = t.box.center();
            const bool besideFrame = c.x() > frame.right() - 1 && c.y() > frame.top() - 20 && c.y() < frame.bottom() + 20;
            if (frame.contains(c) || besideFrame) usable << t;
        }
        const QList<Phrase> phrases = phrasesOf(usable);
        for (const Phrase& phrase : phrases) {
            const double cy = phrase.box.center().y();
            const double h = std::max(phrase.box.height(), 5.0);
            // A line's sample: horizontal, short, ending just before the words.
            for (const pdf::vec::Path& p : g.paths) {
                if (!p.stroked && !p.filled) continue;
                for (const pdf::vec::Part& part : p.parts) {
                    const QRectF box = part.points.boundingRect();
                    if (box.right() > phrase.box.left() + 1 || phrase.box.left() - box.right() > 22) continue;
                    if (std::abs(box.center().y() - cy) > h) continue;
                    if (box.width() > 70 || box.height() > 2 * h) continue;
                    // (Not a legend's frame or the like.)
                    if (box.width() < 1 && box.height() < 1) continue;
                    const bool line = p.stroked && box.height() <= 1.5 && box.width() >= 4 && part.points.size() <= 8;
                    const bool mark = !line && std::max(box.width(), box.height()) <= markerMost;
                    if (!line && !mark) continue;
                    if (line) nameOfStyle.insert(styleKey(p.stroke, p.width, p.dash), phrase.text);
                    if (mark) nameOfMarker.insert((p.filled ? p.fill : p.stroke).name(), phrase.text);
                    legendSamples << box;
                }
            }
        }
    }
    const auto isSample = [&](const QRectF& box) {
        // (By hand: QRectF::contains() says no of a box with no height.)
        for (const QRectF& s : legendSamples)
            if (box.left() >= s.left() - 0.5 && box.right() <= s.right() + 0.5 && box.top() >= s.top() - 0.5 && box.bottom() <= s.bottom() + 0.5) return true;
        return false;
    };

    // --- Series from the chains.
    QList<Series> found;
    int counter = 0;
    QImage ink;
    const double inkScale = 3;
    double agreementSum = 0;
    Q_UNUSED(agreementSum)
    for (const Chain& c : std::as_const(chains)) {
        if (!wanted(options, c.colour)) continue;
        if (isSample(c.points.boundingRect())) continue;
        // A legend's frame, rounded or not: a closed outline round a box.
        if (c.closed && c.curved && achromatic(c.colour)) {
            const QRectF outline = c.points.boundingRect();
            int onEdges = 0;
            for (const QPointF& q : c.points)
                if (std::min({std::abs(q.x() - outline.left()), std::abs(q.x() - outline.right()), std::abs(q.y() - outline.top()), std::abs(q.y() - outline.bottom())}) < 2.5) ++onEdges;
            if (onEdges >= 0.9 * c.points.size()) continue;
        }
        Series s;
        s.colour = c.colour;
        s.origin = Series::Origin::Vector;
        s.width = c.width;
        s.dashed = !c.dash.isEmpty() || c.strokeByStroke;
        QRectF clip = inside;
        if (!c.clip.isNull() && !c.clip.isEmpty()) {
            const QRectF both = c.clip.intersected(inside);
            if (!both.isEmpty()) clip = both;
        }
        clipInto(c.points, clip, c.closed, &s.points, &s.breaks);
        if (s.points.size() < 2) continue;
        double total = 0;
        for (int i = 0; i + 1 < s.points.size(); ++i)
            if (!s.breaks.contains(i + 1)) total += QLineF(s.points.at(i), s.points.at(i + 1)).length();
        if (total < 3) continue;
        // A straight line through the whole frame in grey or black, as long as
        // the frame, at a mark: more of the grid, written in pieces.
        const QRectF box = QPolygonF(s.points).boundingRect();
        if (!polar && achromatic(c.colour) && ((box.width() <= 0.3 && box.height() >= 0.45 * frame.height() && nearAMark(box.center().x(), plot.xAxis)) ||
                                               (box.height() <= 0.3 && box.width() >= 0.45 * frame.width() && nearAMark(box.center().y(), plot.yAxis)))) {
            ++gridLines;
            continue;
        }
        const QString key = styleKey(c.colour, c.width, c.dash);
        s.name = nameOfStyle.value(key);
        ++counter;
        found << s;
    }

    // --- Markers: what repeats.
    {
        QHash<QString, QList<Stamp>> sets;
        QList<QString> setOrder;
        for (const Stamp& stamp : std::as_const(stamps)) {
            if (!wanted(options, stamp.colour) || isSample(stamp.box)) continue;
            if (!inside.contains(stamp.at)) continue;
            const QString key = stamp.colour.name() + QLatin1Char('/') + QString::number(stamp.form >= 0 ? stamp.form + 1000 : int(std::round(stamp.size)));
            if (!sets.contains(key)) setOrder << key;
            sets[key] << stamp;
        }
        for (const QString& key : std::as_const(setOrder)) {
            const QList<Stamp>& set = sets.value(key);
            if (set.size() < 3) continue;
            // (A form drawn at one place each time says nothing by its origin.)
            QPolygonF origins;
            for (const Stamp& st : set) origins << st.at;
            const QRectF spread = origins.boundingRect();
            Series s;
            s.colour = set.first().colour;
            s.origin = Series::Origin::Vector;
            s.shape = Series::Shape::Markers;
            for (const Stamp& st : set) {
                const QPointF at = spread.width() + spread.height() < 0.1 ? st.box.center() : st.at;
                bool duplicate = false;
                for (int i = int(s.points.size()) - 1; i >= std::max<qsizetype>(0, s.points.size() - 4); --i)
                    duplicate = duplicate || QLineF(s.points.at(i), at).length() < 0.05;
                if (!duplicate) s.points << at;
            }
            s.name = nameOfMarker.value(s.colour.name());
            found << s;
        }
    }

    // --- Bars: rectangles of a colour on one base.
    {
        QHash<QString, QList<Box>> sets;
        for (const Box& b : std::as_const(rectangles)) {
            if (!frame.adjusted(-2, -2, 2, 2).contains(b.box.center()) || !wanted(options, b.colour) || isSample(b.box) || b.colour.lightness() >= 250) continue;
            sets[b.colour.name()] << b;
        }
        for (auto it = sets.cbegin(); it != sets.cend(); ++it) {
            const QList<Box>& set = it.value();
            if (set.size() < 3) continue;
            // Upright bars: all ends at the same place at the bottom or the top.
            int atBottom = 0, atTop = 0;
            const double bottom = set.first().box.bottom(), top = set.first().box.top();
            for (const Box& b : set) {
                atBottom += std::abs(b.box.bottom() - bottom) < 0.6;
                atTop += std::abs(b.box.top() - top) < 0.6;
            }
            const bool onBottom = atBottom >= int(set.size()) * 0.8;
            if (!onBottom && atTop < int(set.size()) * 0.8) continue;
            Series s;
            s.colour = set.first().colour;
            s.origin = Series::Origin::Vector;
            s.shape = Series::Shape::Bars;
            QList<Box> sorted = set;
            std::sort(sorted.begin(), sorted.end(), [](const Box& a, const Box& b) { return a.box.left() < b.box.left(); });
            for (const Box& b : std::as_const(sorted)) s.points << QPointF(b.box.center().x(), onBottom ? b.box.top() : b.box.bottom());
            found << s;
        }
    }
    for (const Series& a : std::as_const(areas)) found << a;

    if (gridLines > 0) notes->append(tr("%1 straight grey or black lines at tick marks were taken for the grid and left out.").arg(gridLines));
    if (found.isEmpty()) {
        if (outlined > 0) notes->append(tr("%1 curves are drawn as filled outlines, not as strokes: they are traced from the picture instead.").arg(outlined));
        return false;
    }
    if (outlined > 0) notes->append(tr("%1 more curves are drawn as filled outlines and were left out: ask for route raster to trace them.").arg(outlined));
    // The most that was asked for: the longest.
    if (found.size() > options.maxSeries) {
        std::stable_sort(found.begin(), found.end(), [](const Series& a, const Series& b) { return a.points.size() > b.points.size(); });
        notes->append(tr("%1 series were found; the %2 with the most points are kept.").arg(found.size()).arg(options.maxSeries));
        found = found.mid(0, options.maxSeries);
    }
    int lineNumber = 0, markerNumber = 0;
    QSet<QString> takenNames;
    for (Series& s : found) {
        if (s.shape == Series::Shape::Markers) ++markerNumber;
        else ++lineNumber;
        if (s.name.isEmpty()) s.name = s.shape == Series::Shape::Markers ? tr("Markers %1").arg(markerNumber)
                                       : s.shape == Series::Shape::Area ? tr("Area %1").arg(lineNumber)
                                       : s.shape == Series::Shape::Bars ? tr("Bars %1").arg(lineNumber)
                                                                        : tr("Curve %1").arg(lineNumber);
        QString name = s.name;
        for (int k = 2; takenNames.contains(name); ++k) name = s.name + QLatin1Char(' ') + QString::number(k);
        s.name = name;
        takenNames.insert(name);
    }
    plot.series = found;
    plot.route = QStringLiteral("vector");
    // Do the page and this reading agree?
    ink = source.render(plot.page, inside, inkScale);
    if (!ink.isNull())
        for (Series& s : plot.series) s.agreement = inkAgreement(ink, inside, inkScale, s.points, std::max(s.width, 1.0));
    return true;
}

// --- From a picture --------------------------------------------------------

} // namespace

// A part of the page (working points) drawn in working coordinates.
Pixels workImage(Source& source, const Plot& plot, const QRectF& area, double scale)
{
    Pixels out;
    out.scale = scale;
    out.pageToWork = plot.work;
    out.workToPage = plot.work.inverted();
    const QRect pixels = QRectF(area.topLeft() * scale, area.size() * scale).toAlignedRect();
    out.origin = QPointF(pixels.topLeft()) / scale;
    if (plot.work.isIdentity()) {
        out.image = source.render(plot.page, QRectF(out.origin, QSizeF(pixels.size()) / scale), scale);
        if (out.image.format() != QImage::Format_RGB32 && !out.image.isNull()) out.image = out.image.convertToFormat(QImage::Format_RGB32);
    } else {
        // (The part of the page under it: turned, its corners may stick out.)
        const QRectF onPage = out.workToPage.mapRect(area).intersected(QRectF(QPointF(0, 0), source.pdf()->pagePointSize(plot.page)));
        const QImage page = source.render(plot.page, onPage, scale);
        if (page.isNull()) return out;
        const QPointF pageOrigin = QPointF(QRectF(onPage.topLeft() * scale, onPage.size() * scale).toAlignedRect().topLeft()) / scale;
        QImage turned(pixels.size(), QImage::Format_RGB32);
        turned.fill(Qt::white);
        QPainter painter(&turned);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        // Picture pixels -> page points -> working points -> this picture's pixels.
        QTransform t;
        t.scale(scale, scale);
        t.translate(-out.origin.x(), -out.origin.y());
        t = plot.work * t;
        QTransform fromImage;
        fromImage.translate(pageOrigin.x(), pageOrigin.y());
        fromImage.scale(1 / scale, 1 / scale);
        painter.setTransform(fromImage * t);
        painter.drawImage(0, 0, page);
        painter.end();
        out.image = turned;
    }
    out.w = out.image.width();
    out.h = out.image.height();
    return out;
}

double luminanceDark(QRgb v) { return 255 - (0.299 * qRed(v) + 0.587 * qGreen(v) + 0.114 * qBlue(v)); }

// The angle (degrees) the text and lines of a page lie at, by where their
// dark pixels pile up in rows when the page is turned back by it.
static double skewOf(const QImage& image)
{
    std::vector<QPointF> dark;
    dark.reserve(200000);
    const int w = image.width(), h = image.height();
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < w; ++x)
            if (luminanceDark(row[x]) > 90) dark.emplace_back(x - w / 2.0, y - h / 2.0);
    }
    if (dark.size() < 200) return 0;
    // No more than this many, taken evenly.
    const size_t stride = std::max<size_t>(1, dark.size() / 120000);
    const int bins = 2 * (w + h);
    std::vector<int> histogram(size_t(bins + 2));
    const auto score = [&](double degrees) {
        const double a = degrees * M_PI / 180, c = std::cos(a), s = std::sin(a);
        std::fill(histogram.begin(), histogram.end(), 0);
        for (size_t i = 0; i < dark.size(); i += stride) {
            const int bin = int(dark[i].y() * c - dark[i].x() * s + bins / 2.0);
            if (bin >= 0 && bin < bins) ++histogram[size_t(bin)];
        }
        double sum = 0;
        for (const int n : histogram) sum += double(n) * n;
        return sum;
    };
    double best = 0, top = score(0);
    const double upright = top;
    for (double a = -15; a <= 15.001; a += 0.25) {
        const double v = score(a);
        if (v > top) {
            top = v;
            best = a;
        }
    }
    for (double a = best - 0.25; a <= best + 0.25; a += 0.02) {
        const double v = score(a);
        if (v > top) {
            top = v;
            best = a;
        }
    }
    // Not a turn unless it stands out: a picture of noise has a best angle too.
    if (top < upright * 1.05) return 0;
    return best;
}

// The runs of dark pixels along rows (or along columns): long ones are lines;
// short ones, joined across rows, ticks.
void linesAlong(const QImage& image, bool rows, double scale, QPointF origin, QList<HLine>* longs, QList<HLine>* shorts, double minLong)
{
    const int w = image.width(), h = image.height();
    const int outer = rows ? h : w, inner = rows ? w : h;
    // (Each pixel's darkness once: a page is millions of them.)
    std::vector<uchar> darkness(size_t(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < w; ++x) darkness[size_t(y) * w + x] = luminanceDark(row[x]) > 55;
    }
    const auto dark = [&](int o, int i) { return darkness[rows ? size_t(o) * w + i : size_t(i) * w + o] != 0; };
    const int longest = int((minLong > 0 ? minLong : kMostTickLength) * scale);
    const int shortest = std::max(3, int(1.0 * scale));
    const int widest = std::max(3, int(3.0 * scale));
    const double baseAt = rows ? origin.y() : origin.x(), baseAlong = rows ? origin.x() : origin.y();
    // A short run that goes on across several neighbours is a stroke seen
    // sideways: kept if it is narrow.
    struct Open {
        int start, end;     // along the run's direction
        int first, last;    // across
        double weight, sum;
    };
    std::vector<Open> open, next;
    const auto closeOpen = [&](const Open& o) {
        if (o.last - o.first + 1 > widest) return;
        HLine l;
        l.at = baseAt + (o.sum / std::max(1.0, o.weight) + 0.5) / scale;
        l.from = baseAlong + o.start / scale;
        l.to = baseAlong + (o.end + 1) / scale;
        l.width = (o.last - o.first + 1) / scale;
        shorts->append(l);
    };
    for (int o = 0; o < outer; ++o) {
        next.clear();
        size_t cursor = 0;
        int i = 0;
        while (i < inner) {
            while (i < inner && !dark(o, i)) ++i;
            if (i >= inner) break;
            int start = i, gaps = 0, last = i;
            while (i < inner) {
                if (dark(o, i)) {
                    last = i;
                    gaps = 0;
                } else if (++gaps > 1) break;
                ++i;
            }
            const int length = last - start + 1;
            if (length >= longest) {
                HLine l;
                l.at = baseAt + (o + 0.5) / scale;
                l.from = baseAlong + start / scale;
                l.to = baseAlong + (last + 1) / scale;
                l.width = 1 / scale;
                longs->append(l);
                continue;
            }
            if (length < shortest) continue;
            // Join to an open stroke it overlaps. (The open ones come in order
            // of where they start, as the runs do: no need to look at all.)
            bool joined = false;
            while (cursor < open.size() && open[cursor].end < start) ++cursor;
            for (size_t k = cursor; k < open.size() && open[k].start <= last; ++k) {
                Open& op = open[k];
                if (op.last != o - 1 || op.end < start) continue;
                op.start = std::min(op.start, start);
                op.end = std::max(op.end, last);
                op.last = o;
                op.weight += length;
                op.sum += double(o) * length;
                next.push_back(op);
                op.last = -9;   // (taken)
                joined = true;
                break;
            }
            if (!joined) next.push_back(Open{start, last, o, o, double(length), double(o) * length});
        }
        for (const Open& op : open)
            if (op.last == o - 1) closeOpen(op);
        open.swap(next);
    }
    for (const Open& op : open) closeOpen(op);
}

RasterLines rasterLines(Source& source, int page, const QRectF& within)
{
    RasterLines out;
    QPdfDocument* pdf = source.pdf();
    if (pdf == nullptr) return out;
    const QSizeF paper = pdf->pagePointSize(page);
    const QRectF whole(QPointF(0, 0), paper);
    const QRectF area = within.isNull() ? whole : within.adjusted(-30, -30, 30, 30).intersected(whole);
    if (area.isEmpty()) return out;
    // The page's angle from a small picture of it, then fine.
    const QImage small = source.render(page, area, 1.5);
    if (small.isNull()) return out;
    // Set straight only a scan: a page whose picture is most of it, or that
    // has no text of its own. A figure placed in a typeset page is upright,
    // and the text around it - two columns, a caption - can lean a skew test.
    double angle = 0;
    {
        double share = 0;
        for (const pdf::vec::Picture& pic : source.graphics(page).pictures) {
            const QRectF box = pic.corners.boundingRect().intersected(whole);
            share = std::max(share, box.width() * box.height() / std::max(1.0, whole.width() * whole.height()));
        }
        const bool typeset = source.pdf()->getAllText(page).text().trimmed().size() > 40;
        if (share > 0.5 || !typeset) angle = skewOf(small);
    }
    const double scale = std::clamp(std::sqrt(double(kMostPixels) / 2 / std::max(1.0, area.width() * area.height())), 2.0, 4.0);
    Plot plot;
    plot.page = page;
    if (std::abs(angle) >= 0.04) {
        const QPointF c = whole.center();
        QTransform t;
        t.translate(c.x(), c.y());
        t.rotate(-angle);
        t.translate(-c.x(), -c.y());
        // (Row = y c - x s with the sign skewOf uses: turning the page by it
        // makes the lines level.)
        plot.work = t;
        out.work = t;
        out.angle = angle;
    }
    const QRectF workArea = plot.work.isIdentity() ? area : plot.work.mapRect(area);
    const Pixels pixels = workImage(source, plot, workArea, scale);
    if (pixels.image.isNull()) return out;
    linesAlong(pixels.image, true, scale, pixels.origin, &out.longH, &out.shortH);
    linesAlong(pixels.image, false, scale, pixels.origin, &out.longV, &out.shortV);
    // (What linesAlong calls short along rows lies horizontally; so do the
    // frame's marks for a y axis.)
    out.scale = scale;
    out.picture = pixels.image;
    out.origin = pixels.origin;
    out.valid = true;
    return out;
}

// Marks found in a picture, kept to those evenly spaced when most of them are:
// a marker hanging over the axis, a scan's speck, falls off the ticks' beat.
// (A log axis's minor marks are not evenly spaced; its decades are, and if they
// are not most of the marks, all are kept.)
QList<double> regularMarks(const QList<double>& marks, double tolerance)
{
    if (marks.size() < 5) return marks;
    int bestCount = 0;
    double bestStep = 0, bestStart = 0;
    for (int i = 0; i < marks.size(); ++i)
        for (int j = i + 1; j < marks.size(); ++j) {
            const double step = marks.at(j) - marks.at(i);
            if (step < 3 * tolerance) continue;
            int count = 0;
            for (const double m : marks) {
                const double k = (m - marks.at(i)) / step;
                count += std::abs(k - std::round(k)) * step <= tolerance;
            }
            // (The smaller step of two that explain as many: one beat, not every other.)
            if (count > bestCount || (count == bestCount && step < bestStep)) {
                bestCount = count;
                bestStep = step;
                bestStart = marks.at(i);
            }
        }
    if (bestCount < 4 || bestCount < 0.6 * marks.size()) return marks;
    QList<double> kept;
    for (const double m : marks) {
        const double k = (m - bestStart) / bestStep;
        if (std::abs(k - std::round(k)) * bestStep <= tolerance) kept << m;
    }
    return kept;
}

void marksInPicture(const RasterLines& lines, const QRectF& frame, QList<HLine>* shortV, QList<HLine>* shortH)
{
    const QImage& image = lines.picture;
    const double scale = lines.scale;
    if (image.isNull() || scale <= 0) return;
    const auto darkAt = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) return 0.0;
        return luminanceDark(reinterpret_cast<const QRgb*>(image.constScanLine(y))[x]);
    };
    // Along an edge: \a along runs over it, \a across away from the frame at
    // \a offset.. \a offset + depth. Dark columns, joined, are ticks.
    const auto scan = [&](bool xAxis, double edge, double low, double high, double sign, double startAt = 0.9, double deep = 1.3) {
        QList<double> found;
        const double base = xAxis ? lines.origin.y() : lines.origin.x();
        const double along0 = xAxis ? lines.origin.x() : lines.origin.y();
        const int from = int((low - along0 - 1.5) * scale), to = int((high - along0 + 1.5) * scale);
        const int at0 = int((edge - base) * scale);
        const int start = int(sign * startAt * scale), depth = std::max(2, int(deep * scale));
        // How dark each column is, not whether: the middle of a tick is then
        // found to a fraction of a pixel, from its soft edges.
        std::vector<double> strength;
        strength.reserve(size_t(std::max(0, to - from + 1)));
        for (int a = from; a <= to; ++a) {
            double dark = 0;
            for (int k = 0; k < depth; ++k) {
                const int across = at0 + start + int(sign) * k;
                dark += (xAxis ? darkAt(a, across) : darkAt(across, a)) / 255;
            }
            strength.push_back(dark / depth);
        }
        // (A tick is thin; what is wider - a marker hanging over the axis - is not one.)
        const int widest = std::max(2, int(1.6 * scale));
        for (size_t i = 0; i < strength.size();) {
            if (strength[i] < 0.08) {
                ++i;
                continue;
            }
            // From where it rises to where it falls.
            size_t j = i;
            double weight = 0, moment = 0, peak = 0;
            while (j < strength.size() && strength[j] >= 0.08) {
                weight += strength[j];
                moment += strength[j] * (double(j) + 0.5);
                peak = std::max(peak, strength[j]);
                ++j;
            }
            // Joined to the frame's line: dark too just outside it, where a
            // label's stroke, or a speck, is not.
            double attached = 0;
            const int inner = std::max(1, int(0.5 * startAt * scale)), outer = std::max(inner + 1, int(startAt * scale));
            for (size_t k = i; k < j; ++k)
                for (int d = inner; d < outer; ++d) {
                    const int across = at0 + int(sign) * d, a = from + int(k);
                    attached = std::max(attached, (xAxis ? darkAt(a, across) : darkAt(across, a)) / 255);
                }
            if (peak >= 0.3 && attached >= 0.3 && int(j - i) <= widest && weight > 0) found << along0 + (from + moment / weight) / scale;
            i = j;
        }
        return found;
    };
    const auto addMarks = [&](bool xAxis, double edge, double low, double high, double outward, QList<HLine>* out) {
        // Ticks outside the frame, or inside it (MATLAB's): whichever keeps the
        // steadier beat - outside, a label's stroke can stand by the frame;
        // inside, a curve can cross. Outside when they tie.
        const QList<double> outer = scan(xAxis, edge, low, high, outward), inner = scan(xAxis, edge, low, high, -outward);
        const QList<double> outerKept = regularMarks(outer, 0.6), innerKept = regularMarks(inner, 0.6);
        const auto beat = [](const QList<double>& all, const QList<double>& kept) { return kept.size() < all.size() || all.size() >= 5 ? int(kept.size()) : int(kept.size()) - 1; };
        QList<double> marks = outerKept;
        if (beat(inner, innerKept) > beat(outer, outerKept) && innerKept.size() >= 3) marks = innerKept;
        if (marks.size() < 2 && innerKept.size() >= 3) marks = innerKept;
        // (Ticks inside shorter than the usual strip's start - three
        // quarters of a point, MATLAB's on a small figure - looked for
        // nearer the frame's line, when nothing else is.)
        if (marks.size() < 2) {
            const QList<double> shallow = scan(xAxis, edge, low, high, -outward, 0.3, 0.4);
            const QList<double> shallowKept = regularMarks(shallow, 0.6);
            if (shallowKept.size() >= 3) marks = shallowKept;
        }
        for (const double m : std::as_const(marks)) {
            HLine l;
            l.at = m;
            l.from = edge - 1;
            l.to = edge + 1;
            l.width = 1 / scale;
            out->append(l);
        }
    };
    // (A y axis's marks are horizontal strokes, an x axis's vertical.)
    addMarks(true, frame.bottom(), frame.left(), frame.right(), 1, shortV);
    addMarks(true, frame.top(), frame.left(), frame.right(), -1, shortV);
    addMarks(false, frame.left(), frame.top(), frame.bottom(), -1, shortH);
    addMarks(false, frame.right(), frame.top(), frame.bottom(), 1, shortH);
}

bool traceFrom(Source& source, Plot& plot, QPointF seed, Series* out, QString* why, const Options* options)
{
    if (!plot.pageBox().adjusted(-3, -3, 3, 3).contains(seed)) {
        if (why != nullptr) *why = tr("The point is outside the plot.");
        return false;
    }
    // On a path? Its whole line is at hand, clipped to the frame.
    const pdf::vec::Graphics& g = source.graphics(plot.page);
    const QRectF inside = plot.pageBox().adjusted(-1.2, -1.2, 1.2, 1.2);
    double bestDistance = 4;
    const pdf::vec::Path* bestPath = nullptr;
    const pdf::vec::Part* bestPart = nullptr;
    for (const pdf::vec::Path& p : g.paths) {
        if (!p.stroked || p.stroke.lightness() >= 250) continue;
        for (const pdf::vec::Part& part : p.parts) {
            if (part.points.size() < 2) continue;
            if (part.points.size() <= 5 && (part.rectangle || part.closed)) continue;
            const QRectF box = part.points.boundingRect().adjusted(-4, -4, 4, 4);
            if (!box.contains(seed)) continue;
            for (int i = 0; i + 1 < part.points.size(); ++i) {
                const QPointF a = part.points.at(i), b = part.points.at(i + 1), d = b - a;
                const double l2 = d.x() * d.x() + d.y() * d.y();
                const double u = l2 > 0 ? std::clamp(QPointF::dotProduct(seed - a, d) / l2, 0.0, 1.0) : 0;
                const double distance = QLineF(a + d * u, seed).length();
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestPath = &p;
                    bestPart = &part;
                }
            }
        }
    }
    if (bestPart != nullptr) {
        Series s;
        s.colour = bestPath->stroke;
        s.origin = Series::Origin::Vector;
        s.width = bestPath->width;
        s.dashed = !bestPath->dash.isEmpty();
        QRectF clip = inside;
        if (!bestPath->clip.isNull() && !bestPath->clip.isEmpty()) {
            const QRectF both = bestPath->clip.intersected(inside);
            if (!both.isEmpty()) clip = both;
        }
        clipInto(bestPart->points, clip, bestPart->closed, &s.points, &s.breaks);
        if (s.points.size() >= 2) {
            *out = s;
            return true;
        }
    }
    // In the picture.
    return traceRaster(source, plot, seed, options != nullptr ? *options : Options{}, out, why);
}

bool extract(Source& source, Plot& plot, const Options& options, QString* why)
{
    plot.series.clear();
    if (plot.pageBox().isEmpty()) {
        if (why != nullptr) *why = tr("The plot has no region on the page.");
        return false;
    }
    QStringList notes;
    bool done = false;
    if (!options.seeds.isEmpty()) {
        QList<Series> traced;
        QStringList misses, again;
        int n = 0;
        for (const QPointF& seed : options.seeds) {
            ++n;
            // (A seed on a curve an earlier seed gave already: the same curve.)
            int same = -1;
            for (int k = 0; k < traced.size() && same < 0; ++k)
                for (const QPointF& q : traced.at(k).points)
                    if (QLineF(q, seed).length() <= 1.5) {
                        same = k;
                        break;
                    }
            if (same >= 0) {
                again << tr("seed %1 lies on %2").arg(n).arg(traced.at(same).name);
                continue;
            }
            Series s;
            QString reason;
            if (traceFrom(source, plot, seed, &s, &reason, &options)) {
                s.name = tr("Curve %1").arg(n);
                traced << s;
            } else misses << tr("seed %1: %2").arg(n).arg(reason);
        }
        if (!again.isEmpty()) notes << tr("Seeds on a curve already followed give it once: %1.").arg(again.join(QStringLiteral(", ")));
        plot.series = traced;
        if (!traced.isEmpty()) {
            bool vector = true;
            for (const Series& s : std::as_const(traced)) vector = vector && s.origin == Series::Origin::Vector;
            plot.route = vector ? QStringLiteral("vector") : QStringLiteral("raster");
        }
        notes << misses;
        done = !traced.isEmpty();
    } else {
        const bool tryVector = options.route != Options::Route::Raster && source.hasVectors() && plot.work.isIdentity();
        if (tryVector && extractVector(source, plot, options, &notes)) done = true;
        if (!done && options.route != Options::Route::Vector) done = extractRaster(source, plot, options, &notes);
    }
    // What lies in a region left out goes, whichever way it was found.
    dropExcluded(plot, &notes);
    // In a polar plot, a circle about the centre or a spoke through it is grid.
    if (plot.calibration.kind == Kind::Polar && plot.calibration.valid()) {
        QList<Series> kept;
        int dropped = 0;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape != Series::Shape::Line || s.points.size() < 2) {
                kept << s;
                continue;
            }
            const QList<QPointF> v = values(plot, s);
            double rMin = 1e300, rMax = -1e300;
            double sinSum = 0, cosSum = 0;
            int n = 0;
            for (const QPointF& q : v) {
                if (!std::isfinite(q.x()) || !std::isfinite(q.y())) continue;
                rMin = std::min(rMin, q.x());
                rMax = std::max(rMax, q.x());
                sinSum += std::sin(q.y() * M_PI / 180);
                cosSum += std::cos(q.y() * M_PI / 180);
                ++n;
            }
            if (n == 0) continue;
            const double spreadOfAngle = 1 - std::hypot(sinSum, cosSum) / n;
            const QPointF u0 = plot.calibration.toPage(QPointF(rMax, 0)), u1 = plot.calibration.toPage(QPointF(rMin, 0));
            const double radial = QLineF(u0, u1).length();
            const bool circle = radial < 1.0 && spreadOfAngle > 0.2;
            const bool spoke = spreadOfAngle < 0.0005 && achromatic(s.colour);
            if (circle || spoke) {
                ++dropped;
                continue;
            }
            kept << s;
        }
        if (dropped > 0) notes << tr("%1 circles about the centre and spokes through it were taken for the grid and left out.").arg(dropped);
        plot.series = kept;
    }
    plot.notes << notes;
    if (!done || plot.series.isEmpty()) {
        if (why != nullptr) *why = tr("No curve or markers were found in the region.");
        return false;
    }
    return true;
}

} // namespace qucs_s::digitizer
