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

#include <algorithm>
#include <cmath>
#include <vector>

namespace qucs_s::digitizer {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("PlotDigitizer", text); }

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

// Text on the plot, words on a line joined.
struct Phrase {
    QString text;
    QRectF box;
};

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

struct Pixels {
    int w = 0, h = 0;
    QImage image;             // Format_RGB32
    double scale = 1;         // pixels a working point
    QPointF origin;           // the image's corner, in working points
    QTransform pageToWork;
    QTransform workToPage;

    QPointF toWork(QPointF pixel) const { return origin + pixel / scale; }
    QPointF toPixel(QPointF work) const { return (work - origin) * scale; }
    QPointF toPage(QPointF pixel) const { return workToPage.map(toWork(pixel)); }
};

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
double skewOf(const QImage& image)
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
void linesAlong(const QImage& image, bool rows, double scale, QPointF origin, QList<HLine>* longs, QList<HLine>* shorts)
{
    const int w = image.width(), h = image.height();
    const int outer = rows ? h : w, inner = rows ? w : h;
    const auto dark = [&](int o, int i) {
        const int x = rows ? i : o, y = rows ? o : i;
        return luminanceDark(reinterpret_cast<const QRgb*>(image.constScanLine(y))[x]) > 55;
    };
    const int longest = int(kMostTickLength * scale);
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
            // Join to an open stroke it overlaps.
            bool joined = false;
            for (Open& op : open) {
                if (op.last != o - 1 || op.end < start || op.start > last) continue;
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

} // namespace

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
    const double angle = skewOf(small);
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
    const auto scan = [&](bool xAxis, double edge, double low, double high, double sign) {
        QList<double> found;
        const double base = xAxis ? lines.origin.y() : lines.origin.x();
        const double along0 = xAxis ? lines.origin.x() : lines.origin.y();
        const int from = int((low - along0 - 1.5) * scale), to = int((high - along0 + 1.5) * scale);
        const int at0 = int((edge - base) * scale);
        const int start = int(sign * 0.9 * scale), depth = std::max(2, int(1.3 * scale));
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
        const int widest = std::max(2, int(3 * scale));
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
            if (peak >= 0.3 && int(j - i) <= widest && weight > 0) found << along0 + (from + moment / weight) / scale;
            i = j;
        }
        return found;
    };
    const auto addMarks = [&](bool xAxis, double edge, double low, double high, double outward, QList<HLine>* out) {
        QList<double> marks = scan(xAxis, edge, low, high, outward);
        // No ticks outside: perhaps inside. (Less sure: a curve may cross there.)
        if (marks.size() < 2) {
            const QList<double> inner = scan(xAxis, edge, low, high, -outward);
            if (inner.size() >= 3) marks = inner;
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

namespace {

// What the picture of a plot's frame holds, prepared for tracing.
struct Traced {
    Pixels px;
    std::vector<uchar> label;    // the colour family of each pixel; 0: none
    std::vector<uchar> alpha;    // how much of it
    QList<QColor> families;      // [i] is label i + 1
    QRgb background = qRgb(255, 255, 255);
    QRectF box;                  // in pixels: where to look

    inline int at(int x, int y) const { return y * px.w + x; }
    inline bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < px.w && y < px.h; }
};

// Pixels to leave alone: the frame's outside, the grid, words on the plot.
void prepare(Traced& t, const Plot& plot, const QList<Token>& words, const Options& options)
{
    Pixels& p = t.px;
    t.label.assign(size_t(p.w) * p.h, 0);
    t.alpha.assign(size_t(p.w) * p.h, 0);
    // The background: the commonest colour just inside the frame.
    {
        QHash<QRgb, int> counts;
        const int stepX = std::max(1, p.w / 400), stepY = std::max(1, p.h / 400);
        for (int y = 0; y < p.h; y += stepY) {
            const QRgb* row = reinterpret_cast<const QRgb*>(p.image.constScanLine(y));
            for (int x = 0; x < p.w; x += stepX) ++counts[row[x] & 0xF0F0F0];
        }
        int most = 0;
        QRgb common = 0xF0F0F0;
        for (auto i = counts.cbegin(); i != counts.cend(); ++i)
            if (i.value() > most) {
                most = i.value();
                common = i.key();
            }
        t.background = (common & 0xF0F0F0) | 0x0F0F0F;
        // (White stays white: it is nearly always that.)
        if (qRed(t.background) >= 0xE0 && qGreen(t.background) >= 0xE0 && qBlue(t.background) >= 0xE0) t.background = qRgb(255, 255, 255);
    }
    const int br = qRed(t.background), bg = qGreen(t.background), bb = qBlue(t.background);

    // Grid lines: rows and columns mostly inked. Where a mark is, a third of
    // the way across will do; elsewhere - a flat curve looks like this too -
    // nearly all of it must be.
    const double frameThickness = 2.0 * p.scale;
    const QRectF box = t.box;
    const int x0 = std::clamp(int(box.left()), 0, p.w), x1 = std::clamp(int(box.right()), 0, p.w);
    const int y0 = std::clamp(int(box.top()), 0, p.h), y1 = std::clamp(int(box.bottom()), 0, p.h);
    std::vector<int> rowInk(size_t(p.h), 0), colInk(size_t(p.w), 0);
    std::vector<uchar> mag(size_t(p.w) * p.h, 0);
    std::vector<float> chroma0(size_t(p.w) * p.h, 0.f), chroma1(size_t(p.w) * p.h, 0.f);
    for (int y = y0; y < y1; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(p.image.constScanLine(y));
        for (int x = x0; x < x1; ++x) {
            const int dr = std::max(0, br - qRed(row[x])), dg = std::max(0, bg - qGreen(row[x])), db = std::max(0, bb - qBlue(row[x]));
            const int m = std::max({dr, dg, db});
            if (m < 30) continue;
            const int sum = dr + dg + db;
            mag[size_t(t.at(x, y))] = uchar(std::min(m, 255));
            chroma0[size_t(t.at(x, y))] = float(dr) / sum;
            chroma1[size_t(t.at(x, y))] = float(dg) / sum;
            ++rowInk[size_t(y)];
            ++colInk[size_t(x)];
        }
    }
    const auto isMark = [&](const Axis& axis, double workPosition) {
        for (const double m : axis.marks)
            if (std::abs(m - workPosition) < 1.2) return true;
        for (const Tick& tick : axis.ticks)
            if (std::abs(tick.position - workPosition) < 1.2) return true;
        return false;
    };
    const double spanX = std::max(1, x1 - x0), spanY = std::max(1, y1 - y0);
    std::vector<uchar> gridRow(size_t(p.h), 0), gridCol(size_t(p.w), 0);
    const bool cartesian = plot.calibration.kind == Kind::Cartesian;
    if (cartesian) {
        for (int y = y0; y < y1; ++y) {
            const double share = rowInk[size_t(y)] / spanX;
            const double workY = p.origin.y() + (y + 0.5) / p.scale;
            if (share >= 0.9 || (share >= 0.33 && isMark(plot.yAxis, workY))) gridRow[size_t(y)] = 1;
        }
        for (int x = x0; x < x1; ++x) {
            const double share = colInk[size_t(x)] / spanY;
            const double workX = p.origin.x() + (x + 0.5) / p.scale;
            if (share >= 0.9 || (share >= 0.33 && isMark(plot.xAxis, workX))) gridCol[size_t(x)] = 1;
        }
    }
    // The colour the grid is drawn in, to leave a curve of another over it.
    double gridChroma0 = 1.0 / 3, gridChroma1 = 1.0 / 3;
    {
        double a = 0, b = 0, n = 0;
        for (int y = y0; y < y1; ++y) {
            if (gridRow[size_t(y)] == 0) continue;
            for (int x = x0; x < x1; x += 3) {
                const size_t i = size_t(t.at(x, y));
                if (mag[i] > 0 && gridCol[size_t(x)] == 0) {
                    a += chroma0[i];
                    b += chroma1[i];
                    ++n;
                }
            }
        }
        if (n > 0) {
            gridChroma0 = a / n;
            gridChroma1 = b / n;
        }
    }
    // (A line's soft edge is fainter than the rows counted: a pixel or two more.)
    std::vector<uchar> nearRow(gridRow.size(), 0), nearCol(gridCol.size(), 0);
    const int soft = std::max(2, int(std::ceil(0.4 * p.scale)));
    for (int y = 0; y < p.h; ++y)
        if (gridRow[size_t(y)] != 0)
            for (int k = std::max(0, y - soft); k <= std::min(p.h - 1, y + soft); ++k) nearRow[size_t(k)] = 1;
    for (int x = 0; x < p.w; ++x)
        if (gridCol[size_t(x)] != 0)
            for (int k = std::max(0, x - soft); k <= std::min(p.w - 1, x + soft); ++k) nearCol[size_t(k)] = 1;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const size_t i = size_t(t.at(x, y));
            if (mag[i] == 0 || (nearRow[size_t(y)] == 0 && nearCol[size_t(x)] == 0)) continue;
            if (std::hypot(chroma0[i] - gridChroma0, chroma1[i] - gridChroma1) < 0.12) mag[i] = 0;
        }
    Q_UNUSED(frameThickness)
    // Ticks inside the frame, by the marks found: a sliver at each edge goes.
    if (!plot.frame.isNull()) {
        const double reach = 8 * p.scale;
        const auto clearColumnAt = [&](double workX, bool bottom) {
            const int cx = int((workX - p.origin.x()) * p.scale);
            const int half = int(std::ceil(1.0 * p.scale));
            const int from = bottom ? std::max(y0, y1 - int(reach)) : y0, to = bottom ? y1 : std::min(y1, y0 + int(reach));
            // Only what goes from the edge inward, unbroken.
            for (int x = std::max(x0, cx - half); x <= std::min(x1 - 1, cx + half); ++x) {
                if (bottom) {
                    for (int y = to - 1; y >= from; --y) {
                        const size_t i = size_t(t.at(x, y));
                        if (mag[i] == 0 && y < to - int(1.5 * p.scale)) break;
                        mag[i] = 0;
                    }
                } else {
                    for (int y = from; y < to; ++y) {
                        const size_t i = size_t(t.at(x, y));
                        if (mag[i] == 0 && y > from + int(1.5 * p.scale)) break;
                        mag[i] = 0;
                    }
                }
            }
        };
        const auto clearRowAt = [&](double workY, bool right) {
            const int cy = int((workY - p.origin.y()) * p.scale);
            const int half = int(std::ceil(1.0 * p.scale));
            const int from = right ? std::max(x0, x1 - int(reach)) : x0, to = right ? x1 : std::min(x1, x0 + int(reach));
            for (int y = std::max(y0, cy - half); y <= std::min(y1 - 1, cy + half); ++y) {
                if (right) {
                    for (int x = to - 1; x >= from; --x) {
                        const size_t i = size_t(t.at(x, y));
                        if (mag[i] == 0 && x < to - int(1.5 * p.scale)) break;
                        mag[i] = 0;
                    }
                } else {
                    for (int x = from; x < to; ++x) {
                        const size_t i = size_t(t.at(x, y));
                        if (mag[i] == 0 && x > from + int(1.5 * p.scale)) break;
                        mag[i] = 0;
                    }
                }
            }
        };
        for (const double m : plot.xAxis.marks) {
            clearColumnAt(m, true);
            clearColumnAt(m, false);
        }
        for (const double m : plot.yAxis.marks) {
            clearRowAt(m, false);
            clearRowAt(m, true);
        }
    }
    // Words on the plot.
    for (const Token& word : words) {
        const QRect r = QRectF(p.toPixel(word.box.topLeft()), p.toPixel(word.box.bottomRight())).adjusted(-p.scale, -p.scale, p.scale, p.scale).toAlignedRect();
        for (int y = std::max(0, r.top()); y <= std::min(p.h - 1, r.bottom()); ++y)
            for (int x = std::max(0, r.left()); x <= std::min(p.w - 1, r.right()); ++x) mag[size_t(t.at(x, y))] = 0;
    }

    // The colour families: where the colour of what is drawn, whatever its
    // coverage, gathers. A pixel half covered by a line lies between the
    // background and the line's colour, so its share of each channel of the
    // difference from the background is the same as the line's own.
    constexpr int bins = 24;
    std::vector<double> hist(size_t(bins) * bins, 0.0);
    double total = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const size_t i = size_t(t.at(x, y));
            if (mag[i] < 60) continue;
            const int a = std::min(bins - 1, int(chroma0[i] * bins)), b = std::min(bins - 1, int(chroma1[i] * bins));
            hist[size_t(a) * bins + b] += mag[i];
            total += mag[i];
        }
    struct Family {
        double c0, c1;
    };
    std::vector<Family> families;
    for (int k = 0; k < 14 && total > 0; ++k) {
        size_t top = 0;
        for (size_t i = 1; i < hist.size(); ++i)
            if (hist[i] > hist[top]) top = i;
        if (hist[top] <= 0 || hist[top] < 0.004 * total) break;
        const int ta = int(top / bins), tb = int(top % bins);
        double sum = 0, ma = 0, mb = 0;
        for (int a = std::max(0, ta - 1); a <= std::min(bins - 1, ta + 1); ++a)
            for (int b = std::max(0, tb - 1); b <= std::min(bins - 1, tb + 1); ++b) {
                const double v = hist[size_t(a) * bins + b];
                sum += v;
                ma += v * (a + 0.5) / bins;
                mb += v * (b + 0.5) / bins;
                hist[size_t(a) * bins + b] = 0;
            }
        if (sum <= 0) break;
        Family f{ma / sum, mb / sum};
        bool close = false;
        for (const Family& o : families) close = close || std::hypot(o.c0 - f.c0, o.c1 - f.c1) < 0.05;
        if (!close) families.push_back(f);
    }
    // A colour asked for is one, and then only those.
    if (!options.colours.isEmpty()) {
        families.clear();
        for (const QColor& c : options.colours) {
            const int dr = std::max(0, br - c.red()), dg = std::max(0, bg - c.green()), db = std::max(0, bb - c.blue());
            const int sum = dr + dg + db;
            if (sum > 0) families.push_back(Family{double(dr) / sum, double(dg) / sum});
        }
    }
    if (families.size() > 250) families.resize(250);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const size_t i = size_t(t.at(x, y));
            const int m = mag[i];
            if (m == 0) continue;
            int best = -1;
            double bestD = m >= 90 ? 0.11 : 0.15;
            for (size_t f = 0; f < families.size(); ++f) {
                const double d = std::hypot(chroma0[i] - families[f].c0, chroma1[i] - families[f].c1);
                if (d < bestD) {
                    bestD = d;
                    best = int(f);
                }
            }
            if (best < 0) continue;
            t.label[i] = uchar(best + 1);
            t.alpha[i] = uchar(m);
        }
    t.families.clear();
    for (const Family& f : families) {
        // (Its colour: of the strongest pixels; the series take theirs from
        // their own.)
        const double c2 = std::max(0.0, 1 - f.c0 - f.c1);
        const double most = std::max({f.c0, f.c1, c2});
        const double k = most > 0 ? 255 / most : 0;
        t.families << QColor(std::clamp(int(br - f.c0 * k), 0, 255), std::clamp(int(bg - f.c1 * k), 0, 255), std::clamp(int(bb - c2 * k), 0, 255));
    }
}

// Follows lines in one colour family.
class Follower
{
public:
    Follower(Traced& t, int family, double thickness, std::vector<uchar>& visited, const std::vector<uchar>* blocked)
        : a_t(t), a_family(family), a_thickness(thickness), a_visited(visited), a_blocked(blocked)
    {
        a_step = std::max(1.5, thickness);
        a_gap = 14 * t.px.scale;
        a_breakAt = 8 * t.px.scale;
    }

    // The ink's strength here, sub-pixel.
    double sample(double x, double y) const
    {
        const int ix = int(std::floor(x - 0.5)), iy = int(std::floor(y - 0.5));
        const double fx = x - 0.5 - ix, fy = y - 0.5 - iy;
        const auto value = [&](int a, int b) -> double {
            if (!a_t.inBounds(a, b)) return 0;
            const size_t i = size_t(a_t.at(a, b));
            return a_t.label[i] == a_family ? a_t.alpha[i] : 0;
        };
        return value(ix, iy) * (1 - fx) * (1 - fy) + value(ix + 1, iy) * fx * (1 - fy) + value(ix, iy + 1) * (1 - fx) * fy + value(ix + 1, iy + 1) * fx * fy;
    }

    // Across the line at \a p: the middle of the ink nearest, and how wide it
    // is. False when there is none within reach.
    bool centre(QPointF p, QPointF direction, QPointF* out, double* width) const
    {
        const QPointF n(-direction.y(), direction.x());
        const double reach = std::max(3.0, 2.5 * a_thickness);
        constexpr double d = 0.5;
        const int count = int(reach / d);
        double values[512];
        const int size = std::min(2 * count + 1, 512);
        const int mid = size / 2;
        double peak = 0;
        for (int k = 0; k < size; ++k) {
            const QPointF q = p + n * ((k - mid) * d);
            values[k] = sample(q.x(), q.y());
            peak = std::max(peak, values[k]);
        }
        if (peak < 45) return false;
        const double floor = std::max(30.0, 0.3 * peak);
        // The run nearest the middle.
        int centreIndex = -1;
        for (int k = 0; k <= mid; ++k) {
            if (values[mid + k] >= floor) {
                centreIndex = mid + k;
                break;
            }
            if (values[mid - k] >= floor) {
                centreIndex = mid - k;
                break;
            }
        }
        if (centreIndex < 0) return false;
        int a = centreIndex, b = centreIndex;
        while (a > 0 && values[a - 1] >= floor) --a;
        while (b + 1 < size && values[b + 1] >= floor) ++b;
        double sum = 0, moment = 0;
        for (int k = a; k <= b; ++k) {
            sum += values[k];
            moment += values[k] * (k - mid) * d;
        }
        if (sum <= 0) return false;
        *out = p + n * (moment / sum);
        *width = (b - a + 1) * d;
        return true;
    }

    // Marks where it has been, so a line is walked once.
    void stamp(QPointF p, QPointF direction, double width)
    {
        const QPointF n(-direction.y(), direction.x());
        const double half = width / 2 + 1.5;
        for (double along = -a_step / 2 - 0.5; along <= a_step / 2 + 0.5; along += 0.75)
            for (double across = -half; across <= half; across += 0.75) {
                const QPointF q = p + direction * along + n * across;
                const int x = int(q.x()), y = int(q.y());
                if (a_t.inBounds(x, y)) a_visited[size_t(a_t.at(x, y))] = 1;
            }
    }

    bool seen(QPointF p) const
    {
        const int x = int(p.x()), y = int(p.y());
        return a_t.inBounds(x, y) && a_visited[size_t(a_t.at(x, y))] != 0;
    }

    bool isBlocked(QPointF p) const
    {
        if (a_blocked == nullptr) return false;
        const int x = int(p.x()), y = int(p.y());
        return a_t.inBounds(x, y) && (*a_blocked)[size_t(a_t.at(x, y))] != 0;
    }

    // One way from \a start; each point in \a out (not \a start itself),
    // pieces parted in \a breaks.
    void walk(QPointF start, QPointF direction, QList<QPointF>* out, QList<int>* breaks, int* bridged)
    {
        QPointF p = start;
        QPointF d = normal(direction);
        const double most = 3.0 * std::max(a_thickness, 1.5);
        const int retrace = std::max(3, int(std::ceil(std::max(4 * a_thickness, 6.0) / a_step)));
        int overSeen = 0;
        double through = 0;
        for (int guard = 0; guard < 200000; ++guard) {
            QPointF c;
            double width = 0;
            const QPointF q = p + d * a_step;
            bool go = centre(q, d, &c, &width);
            if (go && width > most) {
                // Wider than the line: another crossing it, a marker on it.
                // Straight on, no point made there.
                through += a_step;
                if (through > a_gap) return;
                p = q;
                continue;
            }
            if (go && QLineF(c, p).length() > 2.2 * a_step) go = false;
            bool jumped = false;
            if (!go) {
                // A bend.
                bool turned_ = false;
                for (const double a : {15.0, -15.0, 30.0, -30.0, 45.0, -45.0, 60.0, -60.0, 80.0, -80.0}) {
                    const QPointF dd = turned(d, a);
                    if (centre(p + dd * a_step, dd, &c, &width) && width <= most && QLineF(c, p).length() <= 2.2 * a_step && QPointF::dotProduct(c - p, d) > -0.2 * a_step) {
                        turned_ = true;
                        break;
                    }
                }
                if (!turned_) {
                    // A gap: a dash, a crossing, a mark over the line.
                    bool found = false;
                    for (double g = 2 * a_step; g <= a_gap && !found; g += a_step)
                        for (const double a : {0.0, 4.0, -4.0, 8.0, -8.0, 12.0, -12.0}) {
                            const QPointF dd = turned(d, a);
                            if (centre(p + dd * g, dd, &c, &width) && width <= most) {
                                found = true;
                                jumped = true;
                                ++*bridged;
                                if (g >= a_breakAt) breaks->append(int(out->size()));
                                break;
                            }
                        }
                    if (!found) return;
                }
            }
            through = 0;
            const QPointF move = c - p;
            d = normal(d * (jumped ? 0.9 : 0.72) + normal(move) * (jumped ? 0.1 : 0.28));
            if (seen(c)) {
                if (++overSeen > retrace) {
                    // On a line already walked: back to where that began.
                    for (int k = 0; k < retrace && !out->isEmpty(); ++k) out->removeLast();
                    return;
                }
            } else if (overSeen > 0) {
                // (Between a walked line's dashes it is not seen: one step back
                // only, or a line would be walked twice a dash at a time.)
                --overSeen;
            }
            stamp(c, d, width);
            p = c;
            // No point where the cut across the line is cut short - a dash's end,
            // where its middle is not the line's - nor on landing after a gap.
            if (!isBlocked(c) && !jumped && width >= 0.6 * a_thickness) out->append(c);
        }
    }

    // The direction a line goes at \a p: the long axis of the ink nearby.
    QPointF directionAt(QPointF p) const
    {
        const double r = std::max(4.0, 2.5 * a_thickness);
        double xx = 0, xy = 0, yy = 0, w = 0;
        const int cx = int(p.x()), cy = int(p.y()), ir = int(std::ceil(r));
        for (int y = cy - ir; y <= cy + ir; ++y)
            for (int x = cx - ir; x <= cx + ir; ++x) {
                if (!a_t.inBounds(x, y)) continue;
                const size_t i = size_t(a_t.at(x, y));
                if (a_t.label[i] != a_family) continue;
                const double dx = x + 0.5 - p.x(), dy = y + 0.5 - p.y();
                if (dx * dx + dy * dy > r * r) continue;
                const double a = a_t.alpha[i];
                xx += a * dx * dx;
                xy += a * dx * dy;
                yy += a * dy * dy;
                w += a;
            }
        if (w <= 0) return {1, 0};
        const double theta = 0.5 * std::atan2(2 * xy, xx - yy);
        return {std::cos(theta), std::sin(theta)};
    }

    double step() const { return a_step; }

private:
    Traced& a_t;
    int a_family;
    double a_thickness;
    std::vector<uchar>& a_visited;
    const std::vector<uchar>* a_blocked;
    double a_step = 2, a_gap = 40, a_breakAt = 20;
};

// A line followed both ways from \a seed, in page points.
bool followFrom(Traced& t, int family, double thickness, QPointF seed, std::vector<uchar>& visited, const std::vector<uchar>* blocked, Series* out)
{
    Follower f(t, family, thickness, visited, blocked);
    const QPointF d = f.directionAt(seed);
    QPointF start = seed;
    double width = 0;
    QPointF c;
    if (f.centre(seed, d, &c, &width)) start = c;
    // The edge of a line already walked is no line of its own.
    if (f.seen(start)) {
        f.stamp(seed, d, std::max(width, 2.0));
        out->points.clear();
        return false;
    }
    QList<QPointF> ahead, behind;
    QList<int> breaksAhead, breaksBehind;
    int bridged = 0;
    f.stamp(start, d, width);
    f.walk(start, d, &ahead, &breaksAhead, &bridged);
    f.walk(start, QPointF(-d.x(), -d.y()), &behind, &breaksBehind, &bridged);
    QList<QPointF> line;
    QList<int> breaks;
    line.reserve(ahead.size() + behind.size() + 1);
    for (int i = int(behind.size()) - 1; i >= 0; --i) line << behind.at(i);
    for (const int b : std::as_const(breaksBehind)) breaks << int(behind.size()) - b;
    line << start;
    const int shift = int(line.size());
    line << ahead;
    for (const int b : std::as_const(breaksAhead)) breaks << shift + b;
    std::sort(breaks.begin(), breaks.end());
    // Left to right, when that is how it goes.
    if (line.size() >= 2 && line.first().x() > line.last().x()) {
        std::reverse(line.begin(), line.end());
        for (int& b : breaks) b = int(line.size()) - b;
        std::sort(breaks.begin(), breaks.end());
    }
    out->points.clear();
    out->points.reserve(line.size());
    for (const QPointF& p : std::as_const(line)) out->points << t.px.toPage(p);
    out->breaks.clear();
    for (const int b : std::as_const(breaks))
        if (b > 0 && b < line.size() && !out->breaks.contains(b)) out->breaks << b;
    out->bridged = bridged;
    out->origin = Series::Origin::Raster;
    out->shape = Series::Shape::Line;
    out->width = thickness / t.px.scale;
    out->dashed = bridged >= 3;
    // Its colour: its darkest pixels'.
    QList<int> reds, greens, blues;
    for (int i = 0; i < line.size(); i += std::max<int>(1, int(line.size()) / 60)) {
        const int x = int(line.at(i).x()), y = int(line.at(i).y());
        if (!t.inBounds(x, y)) continue;
        const QRgb v = reinterpret_cast<const QRgb*>(t.px.image.constScanLine(y))[x];
        reds << qRed(v);
        greens << qGreen(v);
        blues << qBlue(v);
    }
    if (!reds.isEmpty()) {
        std::sort(reds.begin(), reds.end());
        std::sort(greens.begin(), greens.end());
        std::sort(blues.begin(), blues.end());
        out->colour = QColor(reds.at(reds.size() / 3), greens.at(greens.size() / 3), blues.at(blues.size() / 3));
    }
    return true;
}

// What to trace at, in pixels a point: enough to see a thin line's middle,
// and a scan's own resolution.
double scaleFor(Source& source, const Plot& plot, const QRectF& workArea, const Options& options)
{
    if (options.scale > 0) return std::clamp(options.scale, 1.0, 16.0);
    double native = 0;
    const QRectF box = plot.pageBox();
    for (const pdf::vec::Picture& pic : source.graphics(plot.page).paths.isEmpty() ? source.graphics(plot.page).pictures : source.graphics(plot.page).pictures) {
        const QRectF onPage = pic.corners.boundingRect();
        if (!onPage.intersects(box) || onPage.width() <= 0) continue;
        native = std::max(native, pic.width / std::max(1.0, QLineF(pic.corners.at(0), pic.corners.at(1)).length()));
    }
    double s = native > 0 ? std::clamp(native, 4.0, 10.0) : 6.0;
    const double fit = std::sqrt(double(kMostPixels) / std::max(1.0, workArea.width() * workArea.height()));
    return std::clamp(std::min(s, fit), 1.0, 16.0);
}

void setUpTraced(Traced& t, Source& source, const Plot& plot, const Options& options, QList<Token>* wordsOut)
{
    const QRectF pageBox = plot.pageBox();
    const QRectF workFrame = plot.frame.isNull() ? (plot.work.isIdentity() ? pageBox : plot.work.mapRect(pageBox)) : plot.frame;
    const double scale = scaleFor(source, plot, workFrame, options);
    t.px = workImage(source, plot, workFrame, scale);
    if (t.px.image.isNull()) return;
    // A little inside the frame: its own line and no more.
    const double inset = plot.frame.isNull() ? 0 : 1.1 * scale;
    t.box = QRectF(QPointF(0, 0), QSizeF(t.px.w, t.px.h)).adjusted(inset, inset, -inset, -inset);
    QList<Token> words = source.tokens(plot.page, plot.pageBox());
    if (!plot.work.isIdentity())
        for (Token& w : words) w.box = plot.work.mapRect(w.box);
    QList<Token> onPlot;
    for (const Token& w : std::as_const(words))
        if (workFrame.adjusted(-1, -1, 1, 1).contains(w.box.center())) onPlot << w;
    prepare(t, plot, onPlot, options);
    if (wordsOut != nullptr) *wordsOut = onPlot;
}

bool extractRaster(Source& source, Plot& plot, const Options& options, QStringList* notes)
{
    Traced t;
    QList<Token> words;
    setUpTraced(t, source, plot, options, &words);
    if (t.px.image.isNull() || t.px.w < 8 || t.px.h < 8) return false;
    const int w = t.px.w, h = t.px.h;
    plot.renderScale = t.px.scale;
    const double scale = t.px.scale;

    // A legend: words on the plot with a sample of a colour just before them.
    QHash<int, QStringList> legendNames;
    {
        const QList<Phrase> phrases = phrasesOf(words);
        for (const Phrase& ph : phrases) {
            bool numeric = false;
            double v = 0;
            numeric = parseTickLabel(ph.text, &v);
            if (numeric) continue;
            const QRectF band(ph.box.left() - 45, ph.box.center().y() - std::max(ph.box.height() * 0.7, 4.0), 43, 2 * std::max(ph.box.height() * 0.7, 4.0));
            const QRect pixels = QRectF(t.px.toPixel(band.topLeft()), t.px.toPixel(band.bottomRight())).toAlignedRect().intersected(QRect(0, 0, w, h));
            QHash<int, int> counts;
            for (int y = pixels.top(); y <= pixels.bottom(); ++y)
                for (int x = pixels.left(); x <= pixels.right(); ++x) {
                    const int l = t.label[size_t(t.at(x, y))];
                    if (l != 0) ++counts[l];
                }
            int top = 0, most = 0;
            for (auto i = counts.cbegin(); i != counts.cend(); ++i)
                if (i.value() > most) {
                    most = i.value();
                    top = i.key();
                }
            if (top == 0 || most < 3 * scale) continue;
            legendNames[top] << ph.text;
            // The sample goes, and so does what is beside it: only the sample
            // is there, as far as the eye can tell.
            for (int y = pixels.top(); y <= pixels.bottom(); ++y)
                for (int x = pixels.left(); x <= pixels.right(); ++x) {
                    t.label[size_t(t.at(x, y))] = 0;
                    t.alpha[size_t(t.at(x, y))] = 0;
                }
        }
    }

    // Run lengths, to tell a line from what is thick.
    std::vector<int> hrun(size_t(w) * h, 0), vrun(size_t(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        int x = 0;
        while (x < w) {
            const int l = t.label[size_t(t.at(x, y))];
            if (l == 0) {
                ++x;
                continue;
            }
            int e = x + 1;
            while (e < w && t.label[size_t(t.at(e, y))] == l) ++e;
            for (int k = x; k < e; ++k) hrun[size_t(t.at(k, y))] = e - x;
            x = e;
        }
    }
    for (int x = 0; x < w; ++x) {
        int y = 0;
        while (y < h) {
            const int l = t.label[size_t(t.at(x, y))];
            if (l == 0) {
                ++y;
                continue;
            }
            int e = y + 1;
            while (e < h && t.label[size_t(t.at(x, e))] == l) ++e;
            for (int k = y; k < e; ++k) vrun[size_t(t.at(x, k))] = e - y;
            y = e;
        }
    }

    QList<Series> found;
    int lineCount = 0, markerCount = 0;
    QElapsedTimer clock;
    clock.start();
    const int families = int(t.families.size());
    std::vector<uchar> visited(size_t(w) * h, 0);
    std::vector<int> stack;
    stack.reserve(4096);
    const double frameMin = std::min(w, h);
    for (int family = 1; family <= families; ++family) {
        // Connected pieces of it, whole.
        std::vector<int> ids(size_t(w) * h, 0);
        struct Piece {
            int area = 0, x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
            double sx = 0, sy = 0;
        };
        std::vector<Piece> piecesOf(1);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(t.at(x, y));
                if (t.label[i] != family || ids[i] != 0) continue;
                const int id = int(piecesOf.size());
                piecesOf.emplace_back();
                Piece& piece = piecesOf.back();
                stack.clear();
                stack.push_back(int(i));
                ids[i] = id;
                while (!stack.empty()) {
                    const int cur = stack.back();
                    stack.pop_back();
                    const int cx = cur % w, cy = cur / w;
                    ++piece.area;
                    piece.x0 = std::min(piece.x0, cx);
                    piece.x1 = std::max(piece.x1, cx);
                    piece.y0 = std::min(piece.y0, cy);
                    piece.y1 = std::max(piece.y1, cy);
                    piece.sx += cx + 0.5;
                    piece.sy += cy + 0.5;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = cx + dx, ny = cy + dy;
                            if (!t.inBounds(nx, ny)) continue;
                            const size_t ni = size_t(t.at(nx, ny));
                            if (t.label[ni] == family && ids[ni] == 0) {
                                ids[ni] = id;
                                stack.push_back(int(ni));
                            }
                        }
                }
            }
        // Marks: pieces small and about as wide as tall, alike in size, their
        // ink not drawn out along a line. (A dash slanting has a square box too;
        // a word's speck is smaller than a mark.)
        const double markerMost = std::clamp(frameMin * 0.07, 14 * scale * 0.5, 22 * scale);
        std::vector<int> candidates;
        std::vector<double> sizes;
        for (size_t id = 1; id < piecesOf.size(); ++id) {
            const Piece& q = piecesOf[id];
            const double a = q.x1 - q.x0 + 1, b = q.y1 - q.y0 + 1;
            if (std::max(a, b) > markerMost || std::max(a, b) < std::max(3.0, 0.8 * scale)) continue;
            if (std::max(a, b) / std::min(a, b) > 2.0) continue;
            // Its long axis against its short one, by the moments of its ink.
            double xx = 0, xy = 0, yy = 0;
            const double mx = q.sx / q.area, my = q.sy / q.area;
            for (int y = q.y0; y <= q.y1; ++y)
                for (int x = q.x0; x <= q.x1; ++x)
                    if (ids[size_t(t.at(x, y))] == int(id)) {
                        const double dx = x + 0.5 - mx, dy = y + 0.5 - my;
                        xx += dx * dx;
                        xy += dx * dy;
                        yy += dy * dy;
                    }
            const double spread = std::sqrt(std::max(0.0, (xx - yy) * (xx - yy) + 4 * xy * xy));
            const double along = (xx + yy + spread) / 2, across = (xx + yy - spread) / 2;
            if (along > 3.5 * std::max(across, 1e-9)) continue;
            candidates.push_back(int(id));
            sizes.push_back(std::max(a, b));
        }
        std::vector<uchar> isMarkerPiece(piecesOf.size(), 0);
        Series markers;
        if (candidates.size() >= 3) {
            std::vector<double> sorted = sizes;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double typical = sorted[sorted.size() / 2];
            for (size_t k = 0; k < candidates.size(); ++k) {
                if (std::abs(sizes[k] - typical) > std::max(0.35 * typical, 1.5)) continue;
                const Piece& q = piecesOf[size_t(candidates[k])];
                isMarkerPiece[size_t(candidates[k])] = 1;
                // The middle of its box: a hollow mark's mass is its ring, and
                // any mark's centre is its box's.
                markers.points << QPointF((q.x0 + q.x1 + 1) / 2.0, (q.y0 + q.y1 + 1) / 2.0);
            }
        }
        // How thick this family's lines are, its marks aside.
        std::vector<int> thin;
        for (size_t i = 0; i < t.label.size(); i += 3)
            if (t.label[i] == family && t.alpha[i] >= 90 && isMarkerPiece[size_t(ids[i])] == 0) thin.push_back(std::min(hrun[i], vrun[i]));
        double thickness = std::max(1.0, 0.5 * scale);
        if (thin.size() >= 4) {
            std::nth_element(thin.begin(), thin.begin() + thin.size() / 2, thin.end());
            thickness = std::clamp(double(thin[thin.size() / 2]), 1.0, 12 * scale);
        }
        // Marks on a line: thick where a line is thin both ways.
        std::vector<uchar> blocked(size_t(w) * h, 0);
        {
            const double thick = 2.2 * thickness;
            std::vector<int> blobIds(size_t(w) * h, 0);
            struct Blob {
                int area = 0, x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
            };
            std::vector<Blob> blobs(1);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(t.at(x, y));
                    if (t.label[i] != family || blobIds[i] != 0 || hrun[i] < thick || vrun[i] < thick) continue;
                    if (isMarkerPiece[size_t(ids[i])] != 0) continue;
                    const int id = int(blobs.size());
                    blobs.emplace_back();
                    Blob& b = blobs.back();
                    stack.clear();
                    stack.push_back(int(i));
                    blobIds[i] = id;
                    while (!stack.empty()) {
                        const int cur = stack.back();
                        stack.pop_back();
                        const int cx = cur % w, cy = cur / w;
                        ++b.area;
                        b.x0 = std::min(b.x0, cx);
                        b.x1 = std::max(b.x1, cx);
                        b.y0 = std::min(b.y0, cy);
                        b.y1 = std::max(b.y1, cy);
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = cx + dx, ny = cy + dy;
                                if (!t.inBounds(nx, ny)) continue;
                                const size_t ni = size_t(t.at(nx, ny));
                                if (t.label[ni] == family && blobIds[ni] == 0 && hrun[ni] >= thick && vrun[ni] >= thick) {
                                    blobIds[ni] = id;
                                    stack.push_back(int(ni));
                                }
                            }
                    }
                }
            std::vector<double> blobSizes;
            for (size_t k = 1; k < blobs.size(); ++k) {
                const Blob& b = blobs[k];
                const double a = b.x1 - b.x0 + 1, c = b.y1 - b.y0 + 1;
                if (std::max(a, c) <= markerMost && std::max(a, c) / std::min(a, c) <= 2 && b.area >= thick * thick * 0.5) blobSizes.push_back(std::max(a, c));
            }
            if (blobSizes.size() >= 3) {
                std::nth_element(blobSizes.begin(), blobSizes.begin() + blobSizes.size() / 2, blobSizes.end());
                const double typical = blobSizes[blobSizes.size() / 2];
                for (size_t k = 1; k < blobs.size(); ++k) {
                    const Blob& b = blobs[k];
                    const double a = b.x1 - b.x0 + 1, c = b.y1 - b.y0 + 1;
                    if (std::abs(std::max(a, c) - typical) > std::max(0.35 * typical, 1.5) || std::max(a, c) / std::min(a, c) > 2) continue;
                    markers.points << QPointF((b.x0 + b.x1 + 1) / 2.0, (b.y0 + b.y1 + 1) / 2.0);
                    for (int y = b.y0; y <= b.y1; ++y)
                        for (int x = b.x0; x <= b.x1; ++x) blocked[size_t(t.at(x, y))] = 1;
                }
            }
        }
        // What is left: lines, each followed from its leftmost point not
        // walked yet.
        const double keepAt = std::max({4 * thickness, 4.0 * scale, 0.02 * std::max(w, h)});
        const int foundBefore = int(found.size());
        int tracks = 0;
        const int mostTracks = std::max(1, options.maxSeries);
        for (int x = 0; x < w && tracks < mostTracks * 4; ++x)
            for (int y = 0; y < h && tracks < mostTracks * 4; ++y) {
                const size_t i = size_t(t.at(x, y));
                if (t.label[i] != family || visited[i] != 0 || t.alpha[i] < 60) continue;
                if (isMarkerPiece[size_t(ids[i])] != 0) continue;
                if (blocked[i] != 0) continue;
                if (clock.elapsed() > 20000) {
                    notes->append(tr("The trace stopped after 20 s: what was found so far is here."));
                    x = w;
                    break;
                }
                Series s;
                followFrom(t, family, thickness, QPointF(x + 0.5, y + 0.5), visited, &blocked, &s);
                double length = 0;
                for (int k = 0; k + 1 < s.points.size(); ++k) length += QLineF(s.points.at(k), s.points.at(k + 1)).length();
                ++tracks;
                if (length * scale < keepAt) continue;
                found << s;
            }
        // A scrap beside a long line - where it touches the frame, or a stroke
        // of a word - is no curve; a short line alone may be one.
        {
            double longest = 0;
            const auto lengthIn = [](const Series& s) {
                double length = 0;
                for (int k = 0; k + 1 < s.points.size(); ++k) length += QLineF(s.points.at(k), s.points.at(k + 1)).length();
                return length;
            };
            for (int k = foundBefore; k < found.size(); ++k) longest = std::max(longest, lengthIn(found.at(k)));
            int scraps = 0;
            for (int k = int(found.size()) - 1; k >= foundBefore; --k)
                if (lengthIn(found.at(k)) < 0.12 * longest && lengthIn(found.at(k)) * scale < 0.08 * std::max(w, h)) {
                    found.removeAt(k);
                    ++scraps;
                }
            if (scraps > 0) notes->append(tr("%1 scraps much shorter than the lines of their colour were left out.").arg(scraps));
        }
        if (!markers.points.isEmpty()) {
            QList<QPointF> pagePoints;
            for (const QPointF& pixel : std::as_const(markers.points)) pagePoints << t.px.toPage(pixel);
            std::sort(pagePoints.begin(), pagePoints.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
            markers.points = pagePoints;
            markers.shape = Series::Shape::Markers;
            markers.origin = Series::Origin::Raster;
            markers.colour = t.families.value(family - 1);
            // (A mark's strongest pixels show its colour better than the family's.)
            markers.width = thickness / scale;
            found << markers;
        }
        // The legend's names for this family's series.
        QStringList names = legendNames.value(family);
        if (!names.isEmpty()) {
            QList<int> ofFamily;
            // (The family's own series are the latest added.)
            Q_UNUSED(ofFamily)
        }
    }

    // Names, from the legend by colour.
    {
        // Which family each series is of: nearest by colour.
        QHash<int, QStringList> left = legendNames;
        for (Series& s : found) {
            int bestFamily = 0;
            double bestD = 1e18;
            for (int f = 0; f < families; ++f) {
                const double d = colourDistance(t.families.at(f), s.colour);
                if (d < bestD) {
                    bestD = d;
                    bestFamily = f + 1;
                }
            }
            QStringList& names = left[bestFamily];
            if (!names.isEmpty()) s.name = names.takeFirst();
        }
    }
    // A straight black or grey line at a mark that survived is grid; so is a
    // rectangle, a legend's frame. Said, not hidden.
    {
        int dropped = 0;
        QList<Series> kept;
        for (const Series& s : std::as_const(found)) {
            if (s.shape != Series::Shape::Line || !achromatic(s.colour)) {
                kept << s;
                continue;
            }
            // (Its box by hand: QRectF's union takes a point for nothing.)
            QPolygonF inWork;
            for (const QPointF& q : s.points) inWork << plot.work.map(q);
            const QRectF box = inWork.boundingRect();
            const bool axisAligned = box.width() < 0.8 || box.height() < 0.8;
            // Its points on a box's four sides, nearly all: a legend's frame.
            int onEdges = 0;
            for (const QPointF& q0 : s.points) {
                const QPointF q = plot.work.map(q0);
                if (std::abs(q.x() - box.left()) < 1 || std::abs(q.x() - box.right()) < 1 || std::abs(q.y() - box.top()) < 1 || std::abs(q.y() - box.bottom()) < 1) ++onEdges;
            }
            const bool boxOutline = !axisAligned && box.width() > 8 && box.height() > 8 && onEdges >= 0.9 * s.points.size();
            if (axisAligned || boxOutline) {
                ++dropped;
                continue;
            }
            kept << s;
        }
        found = kept;
        if (dropped > 0) notes->append(tr("%1 straight or boxed black or grey lines were taken for the grid or a legend's frame and left out.").arg(dropped));
    }
    if (found.size() > options.maxSeries) {
        std::stable_sort(found.begin(), found.end(), [](const Series& a, const Series& b) { return a.points.size() > b.points.size(); });
        notes->append(tr("%1 series were found; the %2 with the most points are kept.").arg(found.size()).arg(options.maxSeries));
        found = found.mid(0, options.maxSeries);
    }
    int lineNumber = 0, markerNumber = 0;
    QSet<QString> taken;
    for (Series& s : found) {
        if (s.shape == Series::Shape::Markers) ++markerNumber;
        else ++lineNumber;
        if (s.name.isEmpty()) s.name = s.shape == Series::Shape::Markers ? tr("Markers %1").arg(markerNumber) : tr("Curve %1").arg(lineNumber);
        QString name = s.name;
        for (int k = 2; taken.contains(name); ++k) name = s.name + QLatin1Char(' ') + QString::number(k);
        s.name = name;
        taken.insert(name);
    }
    Q_UNUSED(lineCount)
    Q_UNUSED(markerCount)
    plot.series = found;
    plot.route = QStringLiteral("raster");
    return !found.isEmpty();
}

} // namespace

bool traceFrom(Source& source, Plot& plot, QPointF seed, Series* out, QString* why)
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
    Traced t;
    Options all;
    setUpTraced(t, source, plot, all, nullptr);
    if (t.px.image.isNull()) {
        if (why != nullptr) *why = tr("The page could not be drawn.");
        return false;
    }
    const QPointF pixel = t.px.toPixel(plot.work.map(seed));
    int bestX = -1, bestY = -1;
    double nearest = 1e18;
    const int radius = int(std::ceil(5 * t.px.scale));
    const int cx = int(pixel.x()), cy = int(pixel.y());
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (!t.inBounds(x, y) || t.label[size_t(t.at(x, y))] == 0) continue;
            const double d = std::hypot(x + 0.5 - pixel.x(), y + 0.5 - pixel.y());
            if (d < nearest) {
                nearest = d;
                bestX = x;
                bestY = y;
            }
        }
    if (bestX < 0) {
        if (why != nullptr) *why = tr("There is no line within five points of there.");
        return false;
    }
    const int family = t.label[size_t(t.at(bestX, bestY))];
    // Thickness here: the narrower way across, the middle of what is nearby (a
    // click lands on a line's soft edge as often as on its middle).
    QList<int> widths;
    const int around = int(std::ceil(3 * t.px.scale));
    for (int y = bestY - around; y <= bestY + around; y += 2)
        for (int x = bestX - around; x <= bestX + around; x += 2) {
            if (!t.inBounds(x, y) || t.label[size_t(t.at(x, y))] != family) continue;
            int across = 1, down = 1;
            for (int k = x + 1; t.inBounds(k, y) && t.label[size_t(t.at(k, y))] == family; ++k) ++across;
            for (int k = x - 1; t.inBounds(k, y) && t.label[size_t(t.at(k, y))] == family; --k) ++across;
            for (int k = y + 1; t.inBounds(x, k) && t.label[size_t(t.at(x, k))] == family; ++k) ++down;
            for (int k = y - 1; t.inBounds(x, k) && t.label[size_t(t.at(x, k))] == family; --k) ++down;
            widths << std::min(across, down);
        }
    std::sort(widths.begin(), widths.end());
    const double thickness = std::clamp(widths.isEmpty() ? 1.0 : double(widths.at(widths.size() * 3 / 4)), 1.0, 12 * t.px.scale);
    std::vector<uchar> visited(size_t(t.px.w) * t.px.h, 0);
    Series s;
    followFrom(t, family, thickness, QPointF(bestX + 0.5, bestY + 0.5), visited, nullptr, &s);
    if (s.points.size() < 2) {
        if (why != nullptr) *why = tr("The line there could not be followed.");
        return false;
    }
    plot.renderScale = t.px.scale;
    *out = s;
    return true;
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
        QStringList misses;
        int n = 0;
        for (const QPointF& seed : options.seeds) {
            ++n;
            Series s;
            QString reason;
            if (traceFrom(source, plot, seed, &s, &reason)) {
                s.name = tr("Curve %1").arg(n);
                traced << s;
            } else misses << tr("seed %1: %2").arg(n).arg(reason);
        }
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
