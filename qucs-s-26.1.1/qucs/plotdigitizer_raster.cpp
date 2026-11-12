/*
 * plotdigitizer_raster.cpp - a plot's curves and markers followed in a
 *                            picture of the page: a scan, or a figure pasted
 *                            in as an image (plotdigitizer.h)
 *
 * The picture is first cleared of what is not data: the grid, the ticks
 * inside the frame, the words pdfium knows of, regions left out (by hand, or
 * found: an inset drawn over the plot, a legend's box). Its pixels are then
 * sorted into families by colour - by the direction of their difference from
 * the background, which an anti-aliased edge shares with the line's core, and
 * by how dark the line they belong to is, so that black and grey are two.
 * Each family's markers are found next - lone ones by their shape, touching
 * ones by the holes of rings or the peaks of filled discs - and its glyphs (a
 * circled label); both are then lifted out and left as "covered": a line of
 * another family is followed through them, as the eye follows it under a row
 * of rings. Last, each line is followed from its leftmost point not yet
 * walked, both ways, through crossings, dash gaps, and round the top of a
 * narrow peak.
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
#include <QHash>
#include <QPdfDocument>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace qucs_s::digitizer {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("PlotDigitizer", text); }

// A colour family: a direction of colour from the background, and a band of
// darkness (of the darkest pixel near each one: a line's soft edge counts as
// the line it is the edge of).
struct Family {
    double c0 = 1.0 / 3, c1 = 1.0 / 3;
    int lo = 0, hi = 255;
    QColor colour;
    /// The colour asked for it is, by its index in Options::colours; -1, one
    /// found in the picture. With colours asked, those found are hidden: their
    /// markers lifted out and their lines no line of an asked colour, but
    /// nothing of them returned.
    int asked = -1;
    bool hidden = false;
};

// What the picture of a plot's frame holds, prepared for tracing.
struct Traced {
    Pixels px;
    std::vector<uchar> label;     // the family of each pixel, from 1; 0: none
    std::vector<uchar> alpha;     // how much of it
    std::vector<uchar> unknown;   // covered by a marker or a glyph: seen through
    QList<Family> families;
    QRgb background = qRgb(255, 255, 255);
    QRectF box;                   // in pixels: where to look
    QRectF markBox;               // in pixels: where markers are looked for (the box and a band past the frame)
    std::vector<uchar> margin;    // labelled for markers alone, outside the box: let go before lines are followed
    QList<QRectF> leftOut;        // page points: found and left out
    QList<QColor> insetColours;   // what the insets left out are filled with

    inline int at(int x, int y) const { return y * px.w + x; }
    inline bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < px.w && y < px.h; }
};

QRectF pageRectOf(const Pixels& p, const QRect& pixels)
{
    const QRectF work(p.toWork(QPointF(pixels.topLeft())), p.toWork(QPointF(pixels.bottomRight() + QPoint(1, 1))));
    return p.workToPage.mapRect(work);
}

QRect pixelRectOf(const Pixels& p, const QRectF& page)
{
    const QRectF work = p.pageToWork.mapRect(page);
    return QRectF(p.toPixel(work.topLeft()), p.toPixel(work.bottomRight())).toAlignedRect();
}

// The commonest colour inside, white staying white.
QRgb backgroundOf(const Pixels& p)
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
    QRgb background = (common & 0xF0F0F0) | 0x0F0F0F;
    if (qRed(background) >= 0xE0 && qGreen(background) >= 0xE0 && qBlue(background) >= 0xE0) background = qRgb(255, 255, 255);
    return background;
}

// An inset: a large region of one colour, filled, not a line - a drawing set
// into the plot. Found on a coarse grid of the picture as it is, before the
// grid lines are taken out of it.
QList<QPair<QRect, QRgb>> insetsOf(const Traced& t, int x0, int y0, int x1, int y1)
{
    QList<QPair<QRect, QRgb>> out;
    const Pixels& p = t.px;
    const int step = std::max(1, int(std::round(p.scale / 2)));
    const int gw = (x1 - x0 + step - 1) / step, gh = (y1 - y0 + step - 1) / step;
    if (gw < 4 || gh < 4) return out;
    const int br = qRed(t.background), bg = qGreen(t.background), bb = qBlue(t.background);
    std::vector<QRgb> cell(size_t(gw) * gh, 0);
    std::vector<uchar> ink(size_t(gw) * gh, 0), seen(size_t(gw) * gh, 0);
    for (int cy = 0; cy < gh; ++cy)
        for (int cx = 0; cx < gw; ++cx) {
            const int x = std::min(x1 - 1, x0 + cx * step + step / 2), y = std::min(y1 - 1, y0 + cy * step + step / 2);
            const QRgb v = reinterpret_cast<const QRgb*>(p.image.constScanLine(y))[x];
            cell[size_t(cy) * gw + cx] = v;
            ink[size_t(cy) * gw + cx] = std::max({std::abs(br - qRed(v)), std::abs(bg - qGreen(v)), std::abs(bb - qBlue(v))}) >= 30;
        }
    std::vector<int> queue;
    for (int start = 0; start < gw * gh; ++start) {
        if (!ink[size_t(start)] || seen[size_t(start)]) continue;
        const QRgb colour = cell[size_t(start)];
        queue.clear();
        queue.push_back(start);
        seen[size_t(start)] = 1;
        int count = 0, ax = gw, ay = gh, bx = -1, by = -1;
        for (size_t k = 0; k < queue.size(); ++k) {
            const int c = queue[k], cx = c % gw, cy = c / gw;
            ++count;
            ax = std::min(ax, cx);
            ay = std::min(ay, cy);
            bx = std::max(bx, cx);
            by = std::max(by, cy);
            const int around[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& d : around) {
                const int nx = cx + d[0], ny = cy + d[1];
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                const size_t n = size_t(ny) * gw + nx;
                if (seen[n] || !ink[n]) continue;
                const QRgb v = cell[n];
                if (std::abs(qRed(v) - qRed(colour)) + std::abs(qGreen(v) - qGreen(colour)) + std::abs(qBlue(v) - qBlue(colour)) > 45) continue;
                seen[n] = 1;
                queue.push_back(int(n));
            }
        }
        const int w = bx - ax + 1, h = by - ay + 1;
        // Large, mostly filled, and standing clear of the axis below it (bars
        // and filled areas stand on it).
        if (w < 0.12 * gw || h < 0.12 * gh) continue;
        if (count < 0.4 * w * h) continue;
        if (by >= gh - 2) continue;
        out << qMakePair(QRect(QPoint(x0 + ax * step, y0 + ay * step), QPoint(std::min(x1 - 1, x0 + (bx + 1) * step), std::min(y1 - 1, y0 + (by + 1) * step))), colour);
    }
    return out;
}

// Boxes drawn inside the frame - a legend's, an inset's: four sides each
// there along most of its length.
QList<QRect> boxesOf(const Traced& t, const QRectF& frameWork)
{
    QList<QRect> out;
    const Pixels& p = t.px;
    QList<HLine> longH, longV, shortH, shortV;
    linesAlong(p.image, true, p.scale, p.origin, &longH, &shortH, 8);
    linesAlong(p.image, false, p.scale, p.origin, &longV, &shortV, 6);
    const QList<HLine> h = merged(longH, 1.0, 1.5), v = merged(longV, 1.0, 1.5);
    const QList<QRectF> rects = rectanglesOf(h, v, 10, 6);
    const auto covered = [](const QList<HLine>& lines, double at, double from, double to) {
        double sum = 0;
        for (const HLine& l : lines)
            if (std::abs(l.at - at) <= 1.5) sum += std::max(0.0, std::min(to, l.to) - std::max(from, l.from));
        return sum >= 0.7 * (to - from);
    };
    for (const QRectF& r : rects) {
        if (frameWork.isNull()) break;
        if (!frameWork.adjusted(2, 2, -2, -2).contains(r)) continue;
        if (r.width() * r.height() > 0.5 * frameWork.width() * frameWork.height()) continue;
        // Its sides drawn - three will do: a thin line can fall between two
        // rows of pixels and read as none - and something inside it: a
        // legend holds words and samples, a square pulse of a curve nothing.
        const int sides = int(covered(h, r.top(), r.left(), r.right())) + int(covered(h, r.bottom(), r.left(), r.right())) +
                          int(covered(v, r.left(), r.top(), r.bottom())) + int(covered(v, r.right(), r.top(), r.bottom()));
        if (sides < 3) continue;
        const QRect pixels = QRectF(p.toPixel(r.topLeft()), p.toPixel(r.bottomRight())).toAlignedRect();
        const QRect inner = pixels.adjusted(int(2 * p.scale), int(2 * p.scale), -int(2 * p.scale), -int(2 * p.scale)).intersected(QRect(0, 0, p.w, p.h));
        int ink = 0;
        for (int y = inner.top(); y <= inner.bottom(); y += 2)
            for (int x = inner.left(); x <= inner.right(); x += 2) ink += luminanceDark(reinterpret_cast<const QRgb*>(p.image.constScanLine(y))[x]) > 55;
        if (inner.isEmpty() || ink < 0.02 * (inner.width() / 2.0) * (inner.height() / 2.0)) continue;
        out << pixels;
    }
    return out;
}

// Pixels to leave alone, and the colour families.
void prepare(Traced& t, const Plot& plot, const QList<Token>& words, const Options& options, QStringList* notes)
{
    Pixels& p = t.px;
    const size_t n = size_t(p.w) * p.h;
    t.label.assign(n, 0);
    t.alpha.assign(n, 0);
    t.unknown.assign(n, 0);
    t.background = backgroundOf(p);
    const int br = qRed(t.background), bg = qGreen(t.background), bb = qBlue(t.background);
    const QRectF box = t.box;
    const int x0 = std::clamp(int(box.left()), 0, p.w), x1 = std::clamp(int(box.right()), 0, p.w);
    const int y0 = std::clamp(int(box.top()), 0, p.h), y1 = std::clamp(int(box.bottom()), 0, p.h);

    // Regions left out: what the user marked, and what is found.
    QList<QRect> leaveOut;
    for (const QRectF& r : plot.exclude) leaveOut << pixelRectOf(p, r);
    if (options.findLeftOut && !plot.frame.isNull()) {
        int insets = 0, boxes = 0;
        const QList<QPair<QRect, QRgb>> found = insetsOf(t, x0, y0, x1, y1);
        for (const auto& [r, colour] : found) {
            t.insetColours << QColor(colour);
            const int margin = int(std::ceil(3 * p.scale));
            const QRect grown = r.adjusted(-margin, -margin, margin, margin);
            leaveOut << grown;
            t.leftOut << pageRectOf(p, grown);
            ++insets;
        }
        for (const QRect& r : boxesOf(t, plot.frame)) {
            bool inside = false;
            for (const QRect& o : std::as_const(leaveOut)) inside = inside || o.contains(r);
            if (inside) continue;
            const int margin = int(std::ceil(1.5 * p.scale));
            const QRect grown = r.adjusted(-margin, -margin, margin, margin);
            leaveOut << grown;
            t.leftOut << pageRectOf(p, grown);
            ++boxes;
        }
        if (insets > 0)
            notes->append(tr("%1 insets drawn over the plot were left out: 'exclude' with a list of regions (even an empty one) says what to leave out instead.").arg(insets));
        if (boxes > 0) notes->append(tr("%1 boxes inside the frame (a legend's) were left out, with what is in them.").arg(boxes));
    }

    std::vector<int> rowInk(size_t(p.h), 0), colInk(size_t(p.w), 0);
    std::vector<uchar> mag(n, 0);
    std::vector<float> chroma0(n, 0.f), chroma1(n, 0.f);
    for (int y = y0; y < y1; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(p.image.constScanLine(y));
        for (int x = x0; x < x1; ++x) {
            const int dr = std::max(0, br - qRed(row[x])), dg = std::max(0, bg - qGreen(row[x])), db = std::max(0, bb - qBlue(row[x]));
            const int m = std::max({dr, dg, db});
            if (m < 30) continue;
            const int sum = dr + dg + db;
            const size_t i = size_t(t.at(x, y));
            mag[i] = uchar(std::min(m, 255));
            chroma0[i] = float(dr) / sum;
            chroma1[i] = float(dg) / sum;
            ++rowInk[size_t(y)];
            ++colInk[size_t(x)];
        }
    }
    for (const QRect& r : std::as_const(leaveOut))
        for (int y = std::max(y0, r.top()); y <= std::min(y1 - 1, r.bottom()); ++y)
            for (int x = std::max(x0, r.left()); x <= std::min(x1 - 1, r.right()); ++x) mag[size_t(t.at(x, y))] = 0;

    // Grid lines: rows and columns mostly inked. Where a mark is, a third of
    // the way across will do; elsewhere - a flat curve looks like this too -
    // nearly all of it must be.
    const auto isMark = [&](const Axis& axis, double workPosition) {
        for (const double m : axis.marks)
            if (std::abs(m - workPosition) < 1.2) return true;
        for (const Tick& tick : axis.ticks)
            if (std::abs(tick.position - workPosition) < 1.2) return true;
        return false;
    };
    const double spanX = std::max(1, x1 - x0), spanY = std::max(1, y1 - y0);
    std::vector<uchar> gridRow(size_t(p.h), 0), gridCol(size_t(p.w), 0);
    if (plot.calibration.kind == Kind::Cartesian) {
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
        // A grid is at most of the marks, not at one: a narrow peak standing
        // over a tick mark inks most of its column too.
        const auto keepIfGrid = [&](std::vector<uchar>& flags, const std::vector<int>& ink, double span, int from, int to, const Axis& axis) {
            int atMarks = 0;
            for (int k = from; k < to; ++k)
                if (flags[size_t(k)] && (k == from || !flags[size_t(k - 1)]) && ink[size_t(k)] < 0.9 * span) ++atMarks;
            const int marks = int(axis.marks.size());
            if (atMarks >= std::max(3, (marks + 1) / 2)) return;
            for (int k = from; k < to; ++k)
                if (ink[size_t(k)] < 0.9 * span) flags[size_t(k)] = 0;
        };
        keepIfGrid(gridRow, rowInk, spanX, y0, y1, plot.yAxis);
        keepIfGrid(gridCol, colInk, spanY, x0, x1, plot.xAxis);
        // A grid line is thin: a band of inked rows - a row of markers along
        // the axis, a curve lying flat on it - is data.
        const int thickest = std::max(3, int(std::ceil(1.5 * p.scale)));
        // And it stands alone: just beside it the rows are clear, where a
        // band of markers has more of itself.
        const int beside = std::max(2, int(std::ceil(0.8 * p.scale)));
        const auto thinOnly = [&](std::vector<uchar>& flags, const std::vector<int>& ink, double span, int from, int to) {
            for (int k = from; k < to;) {
                if (!flags[size_t(k)]) {
                    ++k;
                    continue;
                }
                int e = k;
                while (e < to && flags[size_t(e)]) ++e;
                const double before = k - beside >= from ? ink[size_t(k - beside)] / span : 0;
                const double after = e - 1 + beside < to ? ink[size_t(e - 1 + beside)] / span : 0;
                if (e - k > thickest || before >= 0.5 || after >= 0.5)
                    for (int j = k; j < e; ++j) flags[size_t(j)] = 0;
                k = e;
            }
        };
        thinOnly(gridRow, rowInk, spanX, y0, y1);
        thinOnly(gridCol, colInk, spanY, x0, x1);
    }
    double gridChroma0 = 1.0 / 3, gridChroma1 = 1.0 / 3;
    {
        double a = 0, b = 0, count = 0;
        for (int y = y0; y < y1; ++y) {
            if (gridRow[size_t(y)] == 0) continue;
            for (int x = x0; x < x1; x += 3) {
                const size_t i = size_t(t.at(x, y));
                if (mag[i] > 0 && gridCol[size_t(x)] == 0) {
                    a += chroma0[i];
                    b += chroma1[i];
                    ++count;
                }
            }
        }
        if (count > 0) {
            gridChroma0 = a / count;
            gridChroma1 = b / count;
        }
    }
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

    // Ticks inside the frame, by the marks found: a sliver at each edge goes.
    if (!plot.frame.isNull()) {
        const double reach = 8 * p.scale;
        const int half = int(std::ceil(1.0 * p.scale));
        const int lead = int(1.5 * p.scale);
        // (A tick ends within its reach; a curve that meets the frame there -
        // a peak touching its top - goes on, and stays.)
        const auto endsWithin = [&](int x, int y, int dx, int dy) {
            int k = 0;
            for (; k <= int(reach) && t.inBounds(x + dx * k, y + dy * k); ++k)
                if (mag[size_t(t.at(x + dx * k, y + dy * k))] == 0 && k > lead) return true;
            return false;
        };
        const auto clearColumn = [&](double workX, bool bottom) {
            const int cx = int((workX - p.origin.x()) * p.scale);
            const int from = bottom ? std::max(y0, y1 - int(reach)) : y0, to = bottom ? y1 : std::min(y1, y0 + int(reach));
            if (!endsWithin(std::clamp(cx, x0, x1 - 1), bottom ? y1 - 1 : y0, 0, bottom ? -1 : 1)) return;
            for (int x = std::max(x0, cx - half); x <= std::min(x1 - 1, cx + half); ++x) {
                if (bottom) {
                    for (int y = to - 1; y >= from; --y) {
                        const size_t i = size_t(t.at(x, y));
                        if (mag[i] == 0 && y < to - lead) break;
                        mag[i] = 0;
                    }
                } else {
                    for (int y = from; y < to; ++y) {
                        const size_t i = size_t(t.at(x, y));
                        if (mag[i] == 0 && y > from + lead) break;
                        mag[i] = 0;
                    }
                }
            }
        };
        const auto clearRow = [&](double workY, bool right) {
            const int cy = int((workY - p.origin.y()) * p.scale);
            const int from = right ? std::max(x0, x1 - int(reach)) : x0, to = right ? x1 : std::min(x1, x0 + int(reach));
            if (!endsWithin(right ? x1 - 1 : x0, std::clamp(cy, y0, y1 - 1), right ? -1 : 1, 0)) return;
            for (int y = std::max(y0, cy - half); y <= std::min(y1 - 1, cy + half); ++y) {
                if (right) {
                    for (int x = to - 1; x >= from; --x) {
                        const size_t i = size_t(t.at(x, y));
                        if (mag[i] == 0 && x < to - lead) break;
                        mag[i] = 0;
                    }
                } else {
                    for (int x = from; x < to; ++x) {
                        const size_t i = size_t(t.at(x, y));
                        if (mag[i] == 0 && x > from + lead) break;
                        mag[i] = 0;
                    }
                }
            }
        };
        for (const double m : plot.xAxis.marks) {
            clearColumn(m, true);
            clearColumn(m, false);
        }
        for (const double m : plot.yAxis.marks) {
            clearRow(m, false);
            clearRow(m, true);
        }
    }
    // Words on the plot.
    for (const Token& word : words) {
        const QRect r = QRectF(p.toPixel(word.box.topLeft()), p.toPixel(word.box.bottomRight())).adjusted(-p.scale, -p.scale, p.scale, p.scale).toAlignedRect();
        for (int y = std::max(0, r.top()); y <= std::min(p.h - 1, r.bottom()); ++y)
            for (int x = std::max(0, r.left()); x <= std::min(p.w - 1, r.right()); ++x) mag[size_t(t.at(x, y))] = 0;
    }

    // How dark the line under each pixel is: the darkest pixel near it.
    std::vector<uchar> peak(n, 0);
    {
        const int r = std::max(1, int(std::round(0.4 * p.scale)));
        std::vector<uchar> rows(n, 0);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                uchar m = 0;
                for (int k = std::max(x0, x - r); k <= std::min(x1 - 1, x + r); ++k) m = std::max(m, mag[size_t(t.at(k, y))]);
                rows[size_t(t.at(x, y))] = m;
            }
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                uchar m = 0;
                for (int k = std::max(y0, y - r); k <= std::min(y1 - 1, y + r); ++k) m = std::max(m, rows[size_t(t.at(x, k))]);
                peak[size_t(t.at(x, y))] = m;
            }
    }

    // The colour families: where the colour of what is drawn gathers, whatever
    // its coverage - a pixel half covered by a line lies between the
    // background and the line's colour, so its share of each channel of the
    // difference is the line's own.
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
    // What is drawn, by colour: each hue, split by darkness where two are drawn
    // in it.
    const auto foundFamilies = [&]() {
        std::vector<Family> found;
        std::vector<Family> hues;
        std::vector<double> weights;
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
            Family f;
            f.c0 = ma / sum;
            f.c1 = mb / sum;
            bool close = false;
            for (const Family& o : hues) close = close || std::hypot(o.c0 - f.c0, o.c1 - f.c1) < 0.05;
            if (!close) {
                hues.push_back(f);
                weights.push_back(sum);
            }
        }
        // A hue between two stronger ones, on the line from one to the other,
        // is where they overlap - a red line under black rings - not a colour
        // of its own.
        for (int k = int(hues.size()) - 1; k >= 0; --k) {
            bool mixture = false;
            for (size_t i = 0; i < hues.size() && !mixture; ++i)
                for (size_t j = i + 1; j < hues.size() && !mixture; ++j) {
                    if (int(i) == k || int(j) == k || weights[i] < weights[size_t(k)] || weights[j] < weights[size_t(k)]) continue;
                    const double ax = hues[i].c0, ay = hues[i].c1, bx = hues[j].c0, by = hues[j].c1;
                    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
                    if (l2 <= 0) continue;
                    const double u = ((hues[size_t(k)].c0 - ax) * dx + (hues[size_t(k)].c1 - ay) * dy) / l2;
                    if (u < 0.12 || u > 0.88) continue;
                    mixture = std::hypot(ax + u * dx - hues[size_t(k)].c0, ay + u * dy - hues[size_t(k)].c1) < 0.035;
                }
            if (mixture) {
                hues.erase(hues.begin() + k);
                weights.erase(weights.begin() + k);
            }
        }
        // Each hue split by darkness where two darknesses are drawn in it -
        // a black curve over a grey drawing, a light red note by a dark red
        // line.
        for (const Family& hue : hues) {
            constexpr int levels = 26;
            std::vector<double> counts(levels, 0.0);
            double all = 0;
            for (int y = y0; y < y1; y += 1)
                for (int x = x0; x < x1; ++x) {
                    const size_t i = size_t(t.at(x, y));
                    if (mag[i] < 60 || std::hypot(chroma0[i] - hue.c0, chroma1[i] - hue.c1) >= 0.11) continue;
                    counts[size_t(std::min(levels - 1, peak[i] / 10))] += 1;
                    all += 1;
                }
            std::vector<double> smooth(levels, 0.0);
            for (int k = 0; k < levels; ++k)
                for (int d = -1; d <= 1; ++d)
                    if (k + d >= 0 && k + d < levels) smooth[size_t(k)] += counts[size_t(k + d)];
            QList<int> peaks;
            for (int k = 0; k < levels; ++k) {
                const bool high = (k == 0 || smooth[size_t(k)] > smooth[size_t(k - 1)]) && (k == levels - 1 || smooth[size_t(k)] >= smooth[size_t(k + 1)]);
                if (high && smooth[size_t(k)] >= 0.1 * all) peaks << k;
            }
            std::sort(peaks.begin(), peaks.end(), [&](int a, int b) { return smooth[size_t(a)] > smooth[size_t(b)]; });
            int split = -1;
            if (peaks.size() >= 2 && std::abs(peaks.at(0) - peaks.at(1)) >= 5) {
                const int a = std::min(peaks.at(0), peaks.at(1)), b = std::max(peaks.at(0), peaks.at(1));
                int valley = a;
                for (int k = a; k <= b; ++k)
                    if (smooth[size_t(k)] < smooth[size_t(valley)]) valley = k;
                if (smooth[size_t(valley)] <= 0.6 * std::min(smooth[size_t(a)], smooth[size_t(b)])) split = valley * 10 + 5;
            }
            if (split < 0) found.push_back(hue);
            else {
                Family light = hue, dark = hue;
                light.hi = split;
                dark.lo = split + 1;
                found.push_back(dark);
                found.push_back(light);
            }
        }
            return found;
    };
    std::vector<Family> families;
    if (!options.colours.isEmpty()) {
        // A colour asked for is a family, its darkness and all: black is not
        // grey.
        for (int k = 0; k < options.colours.size(); ++k) {
            const QColor& c = options.colours.at(k);
            const int dr = std::max(0, br - c.red()), dg = std::max(0, bg - c.green()), db = std::max(0, bb - c.blue());
            const int sum = dr + dg + db, m = std::max({dr, dg, db});
            if (sum <= 0) continue;
            Family f;
            f.c0 = double(dr) / sum;
            f.c1 = double(dg) / sum;
            f.lo = int(0.55 * m);
            f.hi = std::min(255, int(1.4 * m) + 30);
            f.asked = k;
            families.push_back(f);
        }
        // And what else is drawn, hidden: black rings over a blue line asked
        // for are rings, not blue, and the line is followed under them. (Where
        // one has an asked colour's hue, it keeps to the darkness outside the
        // asked one's: grey beside black asked for.)
        const size_t asked = families.size();
        for (Family f : foundFamilies()) {
            bool keep = true;
            for (size_t k = 0; k < asked && keep; ++k) {
                const Family& a = families[k];
                if (std::hypot(a.c0 - f.c0, a.c1 - f.c1) >= 0.06) continue;
                if (f.lo >= a.lo && f.hi <= a.hi) keep = false;
                else if (f.lo < a.lo) f.hi = std::min(f.hi, a.lo - 1);
                else f.lo = std::max(f.lo, a.hi + 1);
                keep = keep && f.hi - f.lo >= 20;
            }
            if (!keep) continue;
            f.hidden = true;
            families.push_back(f);
        }
    } else {
        families = foundFamilies();
    }
    if (families.size() > 250) families.resize(250);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const size_t i = size_t(t.at(x, y));
            const int m = mag[i];
            if (m == 0) continue;
            // Faint beside something dark: the inside of a ring, a scan's haze
            // round a line - not the line's soft edge, which is darker.
            if (m < 0.35 * peak[i]) continue;
            int best = -1;
            double bestD = m >= 90 ? 0.11 : 0.15;
            for (size_t f = 0; f < families.size(); ++f) {
                if (peak[i] < families[f].lo || peak[i] > families[f].hi) continue;
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
    // The band round the box, out to just past the frame: labelled for the
    // markers alone - a ring sitting on the axis is half below the box - by
    // firm ink only, and let go before lines are followed.
    t.margin.assign(n, 0);
    if (t.markBox != t.box) {
        const int mx0 = std::clamp(int(t.markBox.left()), 0, p.w), mx1 = std::clamp(int(std::ceil(t.markBox.right())), 0, p.w);
        const int my0 = std::clamp(int(t.markBox.top()), 0, p.h), my1 = std::clamp(int(std::ceil(t.markBox.bottom())), 0, p.h);
        for (int y = my0; y < my1; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(p.image.constScanLine(y));
            for (int x = mx0; x < mx1; ++x) {
                if (x >= x0 && x < x1 && y >= y0 && y < y1) continue;
                bool left = false;
                for (const QRect& r : std::as_const(leaveOut)) left = left || r.contains(x, y);
                if (left) continue;
                const int dr = std::max(0, br - qRed(row[x])), dg = std::max(0, bg - qGreen(row[x])), db = std::max(0, bb - qBlue(row[x]));
                const int m = std::max({dr, dg, db}), sum = dr + dg + db;
                if (m < 90) continue;
                const double c0 = double(dr) / sum, c1 = double(dg) / sum;
                int best = -1;
                double bestD = 0.11;
                for (size_t f = 0; f < families.size(); ++f) {
                    if (m < families[f].lo || m > families[f].hi) continue;
                    const double d = std::hypot(c0 - families[f].c0, c1 - families[f].c1);
                    if (d < bestD) {
                        bestD = d;
                        best = int(f);
                    }
                }
                if (best < 0) continue;
                const size_t i = size_t(t.at(x, y));
                t.label[i] = uchar(best + 1);
                t.alpha[i] = uchar(m);
                t.margin[i] = 1;
            }
        }
    }
    t.families.clear();
    for (Family f : families) {
        // Its colour: its direction from the background, as dark as the
        // middle of its band of darkness, or its darkest when it has none.
        const double c2 = std::max(0.0, 1 - f.c0 - f.c1);
        const double most = std::max({f.c0, f.c1, c2});
        const double darkness = f.hi >= 255 && f.lo <= 0 ? 255 : (f.lo + std::min(255, f.hi)) / 2.0;
        const double k = most > 0 ? std::min(255.0, darkness) / most : 0;
        f.colour = QColor(std::clamp(int(br - f.c0 * k), 0, 255), std::clamp(int(bg - f.c1 * k), 0, 255), std::clamp(int(bb - c2 * k), 0, 255));
        t.families << f;
    }
}

// Follows lines in one colour family.
class Follower
{
public:
    Follower(Traced& t, int family, double thickness, std::vector<int>& stamps, int& clock)
        : a_t(t), a_family(family), a_thickness(thickness), a_stamps(stamps), a_clock(clock)
    {
        a_step = std::max(1.5, thickness);
        a_gap = 14 * t.px.scale;
        a_breakAt = 8 * t.px.scale;
        a_coastMost = 160 * t.px.scale;
        a_recent = int(std::ceil(4 * std::max(thickness, 2.0) / a_step)) + 3;
        a_walkStart = clock + 1;
    }

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

    // Across the line at \a p: the middle of the ink nearest, and how wide.
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

    void stamp(QPointF p, QPointF direction, double width)
    {
        const QPointF n(-direction.y(), direction.x());
        const double half = width / 2 + 1.5;
        for (double along = -a_step / 2 - 0.5; along <= a_step / 2 + 0.5; along += 0.75)
            for (double across = -half; across <= half; across += 0.75) {
                const QPointF q = p + direction * along + n * across;
                const int x = int(q.x()), y = int(q.y());
                if (a_t.inBounds(x, y)) a_stamps[size_t(a_t.at(x, y))] = a_clock;
            }
    }

    // Walked before: by another walk, or by this one longer ago than the last
    // few steps (the other side of a narrow peak lies within a step's stamp
    // of this side's top).
    bool seen(QPointF p) const
    {
        const int x = int(p.x()), y = int(p.y());
        if (!a_t.inBounds(x, y)) return false;
        const int v = a_stamps[size_t(a_t.at(x, y))];
        return v != 0 && (v < a_walkStart || v < a_clock - a_recent);
    }

    bool covered(QPointF p) const
    {
        const int cx = int(p.x()), cy = int(p.y());
        for (int y = cy - 1; y <= cy + 1; ++y)
            for (int x = cx - 1; x <= cx + 1; ++x)
                if (a_t.inBounds(x, y) && a_t.unknown[size_t(a_t.at(x, y))] != 0) return true;
        return false;
    }

    // Gaps crossed with nothing in them: a dashed line's (not a marker's, a
    // glyph's, another line's over it).
    int dashes() const { return a_dashes; }

    // Whether nothing at all is drawn along the middle of \a from to \a to,
    // their ends aside: the paper showing between a line's dashes.
    bool blankBetween(QPointF from, QPointF to) const
    {
        const double length = QLineF(from, to).length();
        int blank = 0, all = 0;
        for (double u = a_thickness; u < length - a_thickness; u += 0.5) {
            const QPointF q = from + (to - from) * (u / length);
            const int x = int(q.x()), y = int(q.y());
            if (!a_t.inBounds(x, y)) continue;
            const size_t i = size_t(a_t.at(x, y));
            ++all;
            blank += a_t.label[i] == 0 && a_t.unknown[i] == 0;
        }
        return all > 0 && blank >= 0.8 * all;
    }

    // One way from \a start; each point in \a out, pieces parted in \a breaks.
    void walk(QPointF start, QPointF direction, QList<QPointF>* out, QList<int>* breaks, int* bridged)
    {
        QPointF p = start;
        QPointF d = normal(direction);
        const double most = 3.0 * std::max(a_thickness, 1.5);
        const int retrace = std::max(3, int(std::ceil(std::max(4 * a_thickness, 6.0) / a_step)));
        int overSeen = 0;
        double through = 0;
        std::deque<QPointF> directions{d};
        std::deque<QPointF> recent{p};
        // The top of a narrow peak: the line comes back down beside itself,
        // not where it has just been.
        const auto turnBack = [&](QPointF from, QPointF* c, double* width) {
            // (The sharper the peak against the line's width, the further down
            // its sides part: looked for down to a few steps.)
            for (const double distance : {1.0, 1.6, 2.4, 3.2, 4.5, 6.0, 8.0})
                for (const double a : {100.0, -100.0, 120.0, -120.0, 140.0, -140.0, 160.0, -160.0, 170.0, -170.0, 176.0, -176.0}) {
                    if (distance > 3.5 && std::abs(a) < 150) continue;
                    const QPointF dd = turned(d, a);
                    QPointF cc;
                    double ww = 0;
                    const bool got = centre(from + dd * (a_step * distance), dd, &cc, &ww);
                    bool fresh = true;
                    for (const QPointF& r : recent) fresh = fresh && QLineF(r, cc).length() > 0.6 * std::max(ww, a_thickness) + 1;
                    if (!got || ww > most || seen(cc)) continue;
                    if (!fresh) continue;
                    *c = cc;
                    *width = ww;
                    return true;
                }
            return false;
        };
        int cooldown = 0;   // steps before the line may turn back again
        for (int guard = 0; guard < 200000; ++guard) {
            QPointF c;
            double width = 0;
            const QPointF q = p + d * a_step;
            if (cooldown > 0) --cooldown;
            bool go = centre(q, d, &c, &width);
            if (go && width > most) {
                // Wider than the line: another crossing it, a marker on it.
                through += a_step;
                if (through > a_gap && !covered(q)) return;
                if (through > a_coastMost) return;
                p = q;
                continue;
            }
            // (Nor half as far again as the line is wide to one side of the
            // way on: the other side of a narrow peak, a step or two away, is
            // another stroke - a walk hopping between them went up both at
            // once, the third recheck's T1.)
            const auto sideways = [&] {
                const QPointF off = c - q;
                return std::abs(off.x() * d.y() - off.y() * d.x()) > std::max(1.5 * a_thickness, 3.0);
            };
            if (go && (QLineF(c, p).length() > 3.6 * a_step || sideways())) go = false;
            bool jumped = false;
            if (!go) {
                bool found = false;
                // A bend.
                for (const double a : {15.0, -15.0, 30.0, -30.0, 45.0, -45.0, 60.0, -60.0, 80.0, -80.0}) {
                    const QPointF dd = turned(d, a);
                    if (centre(p + dd * a_step, dd, &c, &width) && width <= most && QLineF(c, p).length() <= 2.2 * a_step && QPointF::dotProduct(c - p, d) > -0.2 * a_step) {
                        found = true;
                        break;
                    }
                }
                if (!found && cooldown == 0 && turnBack(p, &c, &width)) {
                    found = true;
                    cooldown = 2 * a_recent;
                }
                // A gap: a dash, a crossing, a mark over the line.
                if (!found) {
                    for (double g = 2 * a_step; g <= a_gap && !found; g += a_step)
                        for (const double a : {0.0, 4.0, -4.0, 8.0, -8.0, 12.0, -12.0}) {
                            const QPointF dd = turned(d, a);
                            if (centre(p + dd * g, dd, &c, &width) && width <= most) {
                                found = true;
                                jumped = true;
                                ++*bridged;
                                a_dashes += blankBetween(p, c);
                                if (g >= a_breakAt) breaks->append(int(out->size()));
                                break;
                            }
                        }
                }
                // Under markers or a glyph: on along the curve it was making,
                // to where it comes out.
                if (!found && (covered(p + d * a_step) || covered(p + d * (2 * a_step)))) {
                    found = coast(p, d, directions, &c, &width);
                    if (found) {
                        jumped = true;
                        ++*bridged;
                    }
                }
                if (!found) return;
            }
            through = 0;
            const QPointF move = c - p;
            d = normal(d * (jumped ? 0.85 : 0.72) + normal(move) * (jumped ? 0.15 : 0.28));
            if (seen(c)) {
                if (++overSeen > retrace) {
                    for (int k = 0; k < retrace && !out->isEmpty(); ++k) out->removeLast();
                    return;
                }
            } else if (overSeen > 0) {
                --overSeen;
            }
            ++a_clock;
            stamp(c, d, width);
            p = c;
            directions.push_back(d);
            if (directions.size() > 6) directions.pop_front();
            recent.push_back(c);
            if (recent.size() > size_t(a_recent)) recent.pop_front();
            if (!jumped && width >= 0.6 * a_thickness) out->append(c);
        }
    }

    // On through what covers the line, turning as it was turning; true where
    // its own ink is met again, outside what covers it.
    bool coast(QPointF from, QPointF d, const std::deque<QPointF>& directions, QPointF* c, double* width) const
    {
        // Its turn a step, from the last few steps.
        double turn = 0;
        int counted = 0;
        for (size_t k = 1; k < directions.size(); ++k) {
            const QPointF a = directions[k - 1], b = directions[k];
            turn += std::atan2(a.x() * b.y() - a.y() * b.x(), a.x() * b.x() + a.y() * b.y());
            ++counted;
        }
        turn = counted > 0 ? std::clamp(turn / counted, -0.15, 0.15) : 0;
        const double most = 3.0 * std::max(a_thickness, 1.5);
        QPointF p = from;
        double outside = 0;
        for (double travelled = a_step; travelled < a_coastMost; travelled += a_step) {
            d = turned(d, turn * 180 / M_PI);
            p += d * a_step;
            if (!a_t.inBounds(int(p.x()), int(p.y()))) return false;
            if (covered(p)) outside = 0;
            else if ((outside += a_step) > a_gap) return false;
            for (const double a : {0.0, 8.0, -8.0, 16.0, -16.0, 24.0, -24.0, 32.0, -32.0, 40.0, -40.0}) {
                const QPointF dd = turned(d, a);
                QPointF cc;
                double ww = 0;
                if (!centre(p + dd * a_step, dd, &cc, &ww) || ww > most || covered(cc) || seen(cc)) continue;
                // Where the curve it was making would be, and going its way:
                // not the other side of a narrow peak, met across the markers.
                const QPointF n(-d.y(), d.x());
                if (std::abs(QPointF::dotProduct(cc - p, n)) > std::max(3 * a_thickness, 0.25 * travelled)) continue;
                if (std::abs(QPointF::dotProduct(directionAt(cc), d)) < 0.7) continue;
                *c = cc;
                *width = ww;
                return true;
            }
        }
        return false;
    }

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

    void beginWalk() { a_walkStart = a_clock + 1; }
    void stampNow(QPointF p, QPointF d, double width)
    {
        ++a_clock;
        stamp(p, d, width);
    }

private:
    Traced& a_t;
    int a_family;
    double a_thickness;
    std::vector<int>& a_stamps;
    int& a_clock;
    double a_step = 2, a_gap = 40, a_breakAt = 20, a_coastMost = 400;
    int a_recent = 8;
    int a_walkStart = 1;
    int a_dashes = 0;
};

// A line followed both ways from \a seed: page points, in the order walked,
// left to right where it goes that way.
bool followFrom(Traced& t, int family, double thickness, QPointF seed, std::vector<int>& stamps, int& clock, Series* out)
{
    Follower f(t, family, thickness, stamps, clock);
    const QPointF d = f.directionAt(seed);
    QPointF start = seed;
    double width = 0;
    QPointF c;
    if (f.centre(seed, d, &c, &width)) start = c;
    if (f.seen(start)) {
        f.stampNow(seed, d, std::max(width, 2.0));
        out->points.clear();
        return false;
    }
    QList<QPointF> ahead, behind;
    QList<int> breaksAhead, breaksBehind;
    int bridged = 0;
    f.stampNow(start, d, width);
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
    if (line.size() >= 2 && line.first().x() > line.last().x()) {
        std::reverse(line.begin(), line.end());
        for (int& b : breaks) b = int(line.size()) - b;
        std::sort(breaks.begin(), breaks.end());
    }
    // (In the order it was walked: asFunctions() puts a curve that is a
    // function of x in order of x, keeping that order along it.)
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
    // (Dashed by its gaps with nothing in them: a solid line's, under
    // markers, are no dashes - the third recheck's T2.)
    out->dashed = f.dashes() >= 3;
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

double scaleFor(Source& source, const Plot& plot, const QRectF& workArea, const Options& options)
{
    if (options.scale > 0) return std::clamp(options.scale, 1.0, 16.0);
    double native = 0;
    const QRectF box = plot.pageBox();
    for (const pdf::vec::Picture& pic : source.graphics(plot.page).pictures) {
        const QRectF onPage = pic.corners.boundingRect();
        if (!onPage.intersects(box) || onPage.width() <= 0) continue;
        native = std::max(native, pic.width / std::max(1.0, QLineF(pic.corners.at(0), pic.corners.at(1)).length()));
    }
    const double s = native > 0 ? std::clamp(native, 4.0, 10.0) : 6.0;
    const double fit = std::sqrt(double(kMostPixels) / std::max(1.0, workArea.width() * workArea.height()));
    return std::clamp(std::min(s, fit), 1.0, 16.0);
}

void setUp(Traced& t, Source& source, const Plot& plot, const Options& options, QList<Token>* wordsOut, QStringList* notes)
{
    const QRectF pageBox = plot.pageBox();
    const QRectF workFrame = plot.frame.isNull() ? (plot.work.isIdentity() ? pageBox : plot.work.mapRect(pageBox)) : plot.frame;
    const double scale = scaleFor(source, plot, workFrame, options);
    // (A little past the frame: a ring sitting on the axis hangs over it.)
    const double past = plot.frame.isNull() ? 0 : 1.5;
    t.px = workImage(source, plot, workFrame.adjusted(-past, -past, past, past), scale);
    if (t.px.image.isNull()) return;
    const QRectF framePixels(t.px.toPixel(workFrame.topLeft()), t.px.toPixel(workFrame.bottomRight()));
    const double inset = plot.frame.isNull() ? 0 : 1.1 * scale;
    t.box = framePixels.adjusted(inset, inset, -inset, -inset);
    t.markBox = plot.frame.isNull() ? t.box : framePixels.adjusted(-scale, -scale, scale, scale).intersected(QRectF(0, 0, t.px.w, t.px.h));
    QList<Token> words = source.tokens(plot.page, plot.pageBox());
    if (!plot.work.isIdentity())
        for (Token& w : words) w.box = plot.work.mapRect(w.box);
    QList<Token> onPlot;
    for (const Token& w : std::as_const(words))
        if (workFrame.adjusted(-1, -1, 1, 1).contains(w.box.center())) onPlot << w;
    QStringList ignored;
    prepare(t, plot, onPlot, options, notes != nullptr ? notes : &ignored);
    if (wordsOut != nullptr) *wordsOut = onPlot;
}

// Run lengths of each pixel's family along its row and its column.
void runsOf(const Traced& t, std::vector<int>& hrun, std::vector<int>& vrun)
{
    const int w = t.px.w, h = t.px.h;
    hrun.assign(size_t(w) * h, 0);
    vrun.assign(size_t(w) * h, 0);
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
}

// What a family holds besides lines.
struct Marks {
    QList<QPointF> centres;     // pixels
    double size = 0;            // a marker's, pixels
    int fromChains = 0;         // found where markers touch
    int glyphs = 0;
    double thickness = 1;       // of its lines, pixels
};

// A family's markers and glyphs, found and lifted out of it - left covered,
// for the lines under them to be followed through.
Marks marksOf(Traced& t, int family, const std::vector<int>& hrun, const std::vector<int>& vrun, const std::vector<uchar>* owners)
{
    Marks out;
    const int w = t.px.w, h = t.px.h;
    const double scale = t.px.scale;
    const double frameMin = std::min(w, h);
    const double markerMost = std::clamp(frameMin * 0.07, 7 * scale, 22 * scale);
    struct Piece {
        int area = 0, x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        int holes = 0;
        bool marker = false;
    };
    std::vector<int> ids(size_t(w) * h, 0);
    std::vector<Piece> pieces(1);
    std::vector<int> stack;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(t.at(x, y));
            if (t.label[i] != family || ids[i] != 0) continue;
            const int id = int(pieces.size());
            pieces.emplace_back();
            stack.clear();
            stack.push_back(int(i));
            ids[i] = id;
            while (!stack.empty()) {
                const int cur = stack.back();
                stack.pop_back();
                const int cx = cur % w, cy = cur / w;
                Piece& q = pieces[size_t(id)];
                ++q.area;
                q.x0 = std::min(q.x0, cx);
                q.x1 = std::max(q.x1, cx);
                q.y0 = std::min(q.y0, cy);
                q.y1 = std::max(q.y1, cy);
                const double px = cx + 0.5, py = cy + 0.5;
                q.sx += px;
                q.sy += py;
                q.sxx += px * px;
                q.syy += py * py;
                q.sxy += px * py;
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
    const auto elongation = [](const Piece& q) {
        const double mx = q.sx / q.area, my = q.sy / q.area;
        const double xx = q.sxx / q.area - mx * mx, yy = q.syy / q.area - my * my, xy = q.sxy / q.area - mx * my;
        const double spread = std::sqrt(std::max(0.0, (xx - yy) * (xx - yy) + 4 * xy * xy));
        const double along = (xx + yy + spread) / 2, across = (xx + yy - spread) / 2;
        return along / std::max(across, 1e-6);
    };

    // The holes: what is not of the family and is enclosed by it - the inside
    // of a ring, of a "0", of a circle. Not reached from the open plot: the
    // large stretches of background that meet the edge of the box looked in.
    // (The box's edge is a wall: a ring sitting on the axis is cut by it, the
    // axis's line being outside, and its hole is still a hole.)
    const int bx0 = std::clamp(int(t.markBox.left()), 0, w - 1), bx1 = std::clamp(int(std::ceil(t.markBox.right())) - 1, 0, w - 1);
    const int by0 = std::clamp(int(t.markBox.top()), 0, h - 1), by1 = std::clamp(int(std::ceil(t.markBox.bottom())) - 1, 0, h - 1);
    std::vector<uchar> outside(size_t(w) * h, 0);
    {
        const double open = 4.0 * markerMost * markerMost;
        std::vector<uchar> done(size_t(w) * h, 0);
        std::vector<int> queue;
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x) {
                const size_t start = size_t(t.at(x, y));
                if (done[start] || t.label[start] == family) continue;
                queue.clear();
                queue.push_back(int(start));
                done[start] = 1;
                bool edge = false;
                for (size_t k = 0; k < queue.size(); ++k) {
                    const int cur = queue[k], cx = cur % w, cy = cur / w;
                    edge = edge || cx == bx0 || cx == bx1 || cy == by0 || cy == by1;
                    const int around[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& d : around) {
                        const int nx = cx + d[0], ny = cy + d[1];
                        if (nx < bx0 || nx > bx1 || ny < by0 || ny > by1) continue;
                        const size_t ni = size_t(t.at(nx, ny));
                        if (done[ni] || t.label[ni] == family) continue;
                        done[ni] = 1;
                        queue.push_back(int(ni));
                    }
                }
                if (edge && double(queue.size()) > open)
                    for (const int i : queue) outside[size_t(i)] = 1;
            }
        // Outside the box: neither inside a ring nor reachable - left alone.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (x < bx0 || x > bx1 || y < by0 || y > by1) outside[size_t(t.at(x, y))] = 1;
    }
    struct Hole {
        int area = 0, x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
        double sx = 0, sy = 0;
        int owner = 0;
    };
    std::vector<Hole> holes;
    {
        std::vector<uchar> done(size_t(w) * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(t.at(x, y));
                if (outside[i] || done[i] || t.label[i] == family) continue;
                Hole hole;
                stack.clear();
                stack.push_back(int(i));
                done[i] = 1;
                while (!stack.empty()) {
                    const int cur = stack.back();
                    stack.pop_back();
                    const int cx = cur % w, cy = cur / w;
                    ++hole.area;
                    hole.x0 = std::min(hole.x0, cx);
                    hole.x1 = std::max(hole.x1, cx);
                    hole.y0 = std::min(hole.y0, cy);
                    hole.y1 = std::max(hole.y1, cy);
                    hole.sx += cx + 0.5;
                    hole.sy += cy + 0.5;
                    const int around[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& d : around) {
                        const int nx = cx + d[0], ny = cy + d[1];
                        if (!t.inBounds(nx, ny)) continue;
                        const size_t ni = size_t(t.at(nx, ny));
                        if (t.label[ni] == family) {
                            if (hole.owner == 0) hole.owner = ids[ni];
                            continue;
                        }
                        if (done[ni] || outside[ni]) continue;
                        done[ni] = 1;
                        stack.push_back(int(ni));
                    }
                }
                if (hole.owner > 0) {
                    ++pieces[size_t(hole.owner)].holes;
                    holes.push_back(hole);
                }
            }
    }

    // The family's thinnest strokes, all of it: a line's width, a ring's.
    double stroke = 1;
    {
        std::vector<int> thin;
        for (size_t i = 0; i < t.label.size(); i += 3)
            if (t.label[i] == family && t.alpha[i] >= 90) thin.push_back(std::min(hrun[i], vrun[i]));
        if (!thin.empty()) {
            std::nth_element(thin.begin(), thin.begin() + thin.size() / 2, thin.end());
            stroke = std::max(1, thin[thin.size() / 2]);
        }
    }
    // Lone markers: small, round about (not a piece of a line between two
    // things that cover it), and - a ring, a cross - wider than the family's
    // strokes, or - a disc, a square - filled.
    std::vector<int> lone;
    std::vector<double> sizes;
    for (size_t id = 1; id < pieces.size(); ++id) {
        const Piece& q = pieces[id];
        const double a = q.x1 - q.x0 + 1, b = q.y1 - q.y0 + 1;
        if (std::max(a, b) > markerMost || std::max(a, b) < std::max(3.0, 0.8 * scale)) continue;
        if (std::max(a, b) / std::min(a, b) > 2.0) continue;
        if (elongation(q) > 2.2) continue;
        const bool filled = q.area >= 0.6 * a * b;
        if (!filled && q.holes == 0 && std::max(a, b) < 2.4 * stroke) continue;
        // A bit of a line seen inside or between another family's markers is
        // no marker of its own: it touches them.
        if (q.holes == 0 && owners != nullptr) {
            bool touches = false;
            for (int y = std::max(0, q.y0 - 1); y <= std::min(h - 1, q.y1 + 1) && !touches; ++y)
                for (int x = std::max(0, q.x0 - 1); x <= std::min(w - 1, q.x1 + 1) && !touches; ++x) {
                    const int owner = (*owners)[size_t(t.at(x, y))];
                    touches = owner != 0 && owner != family;
                }
            if (touches) continue;
        }
        lone.push_back(int(id));
        sizes.push_back(std::max(a, b));
    }
    double typical = 0;
    QList<int> loneKept;
    if (lone.size() >= 3) {
        std::vector<double> sorted = sizes;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        typical = sorted[sorted.size() / 2];
        for (size_t k = 0; k < lone.size(); ++k)
            if (std::abs(sizes[k] - typical) <= std::max(0.35 * typical, 1.5)) loneKept << lone[k];
        if (loneKept.size() < 3) loneKept.clear();
    }
    // Hollow markers (rings) have holes; touching ones still do, each its own.
    int hollow = 0;
    std::vector<double> holeAreas;
    for (const int id : std::as_const(loneKept)) {
        if (pieces[size_t(id)].holes > 0) ++hollow;
    }
    for (const Hole& hole : holes)
        if (loneKept.contains(hole.owner) && hole.area >= 2) holeAreas.push_back(hole.area);
    bool rings = !loneKept.isEmpty() && hollow * 2 >= loneKept.size() && !holeAreas.empty();
    double holeTypical = 0;
    if (rings) {
        std::nth_element(holeAreas.begin(), holeAreas.begin() + holeAreas.size() / 2, holeAreas.end());
        holeTypical = holeAreas[holeAreas.size() / 2];
    } else if (loneKept.isEmpty()) {
        // Every marker touching another: many holes of one size, small and round.
        std::vector<double> areas;
        for (const Hole& hole : holes) {
            const double a = hole.x1 - hole.x0 + 1, b = hole.y1 - hole.y0 + 1;
            if (hole.area >= 3 && std::max(a, b) <= markerMost * 0.8 && std::max(a, b) / std::min(a, b) <= 2) areas.push_back(hole.area);
        }
        if (areas.size() >= 6) {
            std::vector<double> sorted = areas;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double median = sorted[sorted.size() / 2];
            int alike = 0;
            for (const double a : areas) alike += a >= 0.5 * median && a <= 2 * median;
            if (alike >= 6 && alike >= 0.6 * areas.size()) {
                rings = true;
                holeTypical = median;
            }
        }
    }
    // The family's ring width, from its thinnest runs.
    double ring = 0;
    {
        std::vector<int> thin;
        for (size_t i = 0; i < t.label.size(); i += 5)
            if (t.label[i] == family && t.alpha[i] >= 90) thin.push_back(std::min(hrun[i], vrun[i]));
        if (!thin.empty()) {
            std::nth_element(thin.begin(), thin.begin() + thin.size() / 4, thin.end());
            ring = thin[thin.size() / 4];
        }
    }
    QList<QPointF> centres;
    if (rings) {
        if (typical <= 0) typical = 2 * std::sqrt(holeTypical / M_PI) + 2 * std::max(1.0, ring);
        for (const Hole& hole : holes) {
            const double a = hole.x1 - hole.x0 + 1, b = hole.y1 - hole.y0 + 1;
            // (A ring overlapped by the next keeps a smaller hole.)
            if (hole.area < 0.12 * holeTypical || hole.area > 2.5 * holeTypical) continue;
            if (std::max(a, b) / std::min(a, b) > 2.2 || std::max(a, b) > markerMost) continue;
            const QPointF centre(hole.sx / hole.area, hole.sy / hole.area);
            const bool owned = !loneKept.contains(hole.owner);
            centres << centre;
            pieces[size_t(hole.owner)].marker = true;
            if (owned) ++out.fromChains;
        }
        // A lone ring whose hole was not found (closed up by the scan).
        for (const int id : std::as_const(loneKept)) {
            const Piece& q = pieces[size_t(id)];
            const QPointF centre((q.x0 + q.x1 + 1) / 2.0, (q.y0 + q.y1 + 1) / 2.0);
            bool near = false;
            for (const QPointF& c : std::as_const(centres)) near = near || QLineF(c, centre).length() < 0.5 * typical;
            if (!near) centres << centre;
            pieces[size_t(id)].marker = true;
        }
    } else if (!loneKept.isEmpty()) {
        for (const int id : std::as_const(loneKept)) {
            const Piece& q = pieces[size_t(id)];
            centres << QPointF((q.x0 + q.x1 + 1) / 2.0, (q.y0 + q.y1 + 1) / 2.0);
            pieces[size_t(id)].marker = true;
        }
    }
    if (typical > 0 && (rings || !loneKept.isEmpty())) {
        // Markers touching each other so closely that their holes are gone,
        // or filled ones, or on a line: the peaks of how far
        // each pixel is from the family's edge, a marker's radius apart.
        // (A ring whose hole is small - its stroke thick for its size, the
        // scan closing it up - or filled by a line of another colour through
        // its middle, as a disc: else the peak of its distance is in its
        // stroke, too near the edge to be told from a line's. So ink of any
        // colour counts, and small pockets of background walled in by it
        // beside this colour; a peak is kept where this colour goes most of
        // the way round it.)
        std::vector<float> dt(size_t(w) * h, 0.f);
        const float big = 1e9f;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) dt[size_t(t.at(x, y))] = t.label[size_t(t.at(x, y))] != 0 ? big : 0.f;
        {
            const double pocket = rings ? 0.5 * holeTypical : 0.05 * typical * typical;
            std::vector<uchar> done(size_t(w) * h, 0);
            std::vector<int> queue;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t start = size_t(t.at(x, y));
                    if (done[start] || t.label[start] != 0) continue;
                    queue.clear();
                    queue.push_back(int(start));
                    done[start] = 1;
                    bool beside = false, open = false;
                    for (size_t k = 0; k < queue.size(); ++k) {
                        const int cur = queue[k], cx = cur % w, cy = cur / w;
                        const int around[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (const auto& d : around) {
                            const int nx = cx + d[0], ny = cy + d[1];
                            if (!t.inBounds(nx, ny)) {
                                open = true;
                                continue;
                            }
                            const size_t ni = size_t(t.at(nx, ny));
                            if (t.label[ni] != 0) {
                                beside = beside || t.label[ni] == family;
                                continue;
                            }
                            if (done[ni]) continue;
                            done[ni] = 1;
                            queue.push_back(int(ni));
                        }
                    }
                    if (beside && !open && double(queue.size()) <= pocket)
                        for (const int i : queue) dt[size_t(i)] = big;
                }
        }
        const auto relax = [&](int x, int y, int nx, int ny, float cost) {
            if (!t.inBounds(nx, ny)) return;
            float& v = dt[size_t(t.at(x, y))];
            v = std::min(v, dt[size_t(t.at(nx, ny))] + cost);
        };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (dt[size_t(t.at(x, y))] == 0) continue;
                relax(x, y, x - 1, y, 1);
                relax(x, y, x, y - 1, 1);
                relax(x, y, x - 1, y - 1, 1.414f);
                relax(x, y, x + 1, y - 1, 1.414f);
            }
        for (int y = h - 1; y >= 0; --y)
            for (int x = w - 1; x >= 0; --x) {
                if (dt[size_t(t.at(x, y))] == 0) continue;
                relax(x, y, x + 1, y, 1);
                relax(x, y, x, y + 1, 1);
                relax(x, y, x + 1, y + 1, 1.414f);
                relax(x, y, x - 1, y + 1, 1.414f);
            }
        struct Peak {
            float v;
            int x, y;
        };
        std::vector<Peak> candidates;
        // (Deeper than half the family's line: a line's middle is no peak.)
        const float least = float(std::max(0.32 * typical, 0.6 * ring));
        // Whether this colour goes round a point, at a marker's stroke: a
        // ring or a disc of it all the way, but where lines of other colours
        // cross it; another colour's own blob, or a line of this one across
        // it, not.
        const double around = std::max(1.0, typical / 2 - std::max(1.0, ring) / 2);
        const auto roundIt = [&](int x, int y) {
            int hits = 0, covered = 0;
            for (int k = 0; k < 16; ++k) {
                const double a = k * M_PI / 8;
                bool hit = false, other = false;
                for (const double r : {around - 1, around, around + 1}) {
                    const int sx = int(std::floor(x + 0.5 + r * std::cos(a))), sy = int(std::floor(y + 0.5 + r * std::sin(a)));
                    if (!t.inBounds(sx, sy)) continue;
                    const int l = t.label[size_t(t.at(sx, sy))];
                    hit = hit || l == family;
                    other = other || (l != 0 && l != family);
                }
                hits += hit;
                covered += !hit && other;
            }
            return hits >= 6 && hits >= 0.7 * (16 - covered);
        };
        for (int y = 1; y + 1 < h; ++y)
            for (int x = 1; x + 1 < w; ++x) {
                const float v = dt[size_t(t.at(x, y))];
                // (Up to nearly a marker's width: a ring a line of another
                // colour runs through, its middle filled by it, is deeper
                // than a lone one - the open items' A2, Fig. 5's steep side.)
                if (v < least || v > 0.95 * typical) continue;
                bool top = true;
                for (int dy = -1; dy <= 1 && top; ++dy)
                    for (int dx = -1; dx <= 1 && top; ++dx) top = dt[size_t(t.at(x + dx, y + dy))] <= v;
                if (top && roundIt(x, y)) candidates.push_back(Peak{v, x, y});
            }
        std::sort(candidates.begin(), candidates.end(), [](const Peak& a, const Peak& b) { return a.v > b.v; });
        QList<QPointF> fromPeaks;
        for (const Peak& pk : candidates) {
            const QPointF c(pk.x + 0.5, pk.y + 0.5);
            bool near = false;
            for (const QPointF& o : std::as_const(centres)) near = near || QLineF(o, c).length() < 0.7 * typical;
            for (const QPointF& o : std::as_const(fromPeaks)) near = near || QLineF(o, c).length() < 0.7 * typical;
            if (!near) fromPeaks << c;
            if (fromPeaks.size() > 5000) break;
        }
        centres << fromPeaks;
        out.fromChains += int(fromPeaks.size());
    } else if (!rings) {
        // No lone markers to learn their size from: marks on a line, found as
        // patches thick both ways where a line is thin both ways.
        std::vector<int> thin;
        for (size_t i = 0; i < t.label.size(); i += 3)
            if (t.label[i] == family && t.alpha[i] >= 90) thin.push_back(std::min(hrun[i], vrun[i]));
        double thickness = std::max(1.0, 0.5 * scale);
        if (thin.size() >= 4) {
            std::nth_element(thin.begin(), thin.begin() + thin.size() / 2, thin.end());
            thickness = std::clamp(double(thin[thin.size() / 2]), 1.0, 12 * scale);
        }
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
                const int id = int(blobs.size());
                blobs.emplace_back();
                stack.clear();
                stack.push_back(int(i));
                blobIds[i] = id;
                while (!stack.empty()) {
                    const int cur = stack.back();
                    stack.pop_back();
                    const int cx = cur % w, cy = cur / w;
                    Blob& b = blobs[size_t(id)];
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
            typical = blobSizes[blobSizes.size() / 2];
            for (size_t k = 1; k < blobs.size(); ++k) {
                const Blob& b = blobs[k];
                const double a = b.x1 - b.x0 + 1, c = b.y1 - b.y0 + 1;
                if (std::abs(std::max(a, c) - typical) > std::max(0.35 * typical, 1.5) || std::max(a, c) / std::min(a, c) > 2) continue;
                centres << QPointF((b.x0 + b.x1 + 1) / 2.0, (b.y0 + b.y1 + 1) / 2.0);
            }
        }
    }
    // Each marker lifted out: covered, for the lines under it.
    out.size = typical;
    const double radius = typical / 2 + std::max(1.0, 0.25 * std::max(1.0, ring));
    for (const QPointF& c : std::as_const(centres)) {
        const int r = int(std::ceil(radius));
        for (int y = int(c.y()) - r; y <= int(c.y()) + r; ++y)
            for (int x = int(c.x()) - r; x <= int(c.x()) + r; ++x) {
                if (!t.inBounds(x, y)) continue;
                if (std::hypot(x + 0.5 - c.x(), y + 0.5 - c.y()) > radius) continue;
                const size_t i = size_t(t.at(x, y));
                t.unknown[i] = 1;
                if (t.label[i] == family) {
                    t.label[i] = 0;
                    t.alpha[i] = 0;
                }
            }
    }
    out.centres = centres;

    // Glyphs: a circled label, a letter with a hole - compact, larger than a
    // marker, smaller than a word, with a hole; and what sits in the hole.
    {
        const double low = std::max(4 * scale, typical > 0 ? 1.6 * typical : 0.0), high = 18 * scale;
        QList<QRect> glyphBoxes;
        for (size_t id = 1; id < pieces.size(); ++id) {
            const Piece& q = pieces[id];
            if (q.marker || q.holes == 0) continue;
            const double a = q.x1 - q.x0 + 1, b = q.y1 - q.y0 + 1;
            if (std::max(a, b) < low || std::max(a, b) > high || std::max(a, b) / std::min(a, b) > 2.5) continue;
            if (elongation(q) > 4) continue;
            glyphBoxes << QRect(QPoint(q.x0, q.y0), QPoint(q.x1, q.y1));
        }
        // Words: small pieces of like height standing on one line, side by
        // side - "Metal", "Air" by a drawing. (Markers are found already.)
        {
            struct Small {
                QRect box;
                int id;
            };
            // (A letter with a hole, an "e", an "a", or a key's square may
            // have been taken for a marker: in a word, it is a letter. A row
            // of markers alone, all alike, is no word.)
            QList<Small> small;
            for (size_t id = 1; id < pieces.size(); ++id) {
                const Piece& q = pieces[id];
                const int a = q.x1 - q.x0 + 1, b = q.y1 - q.y0 + 1;
                if (b < 2.5 * scale || b > 12 * scale || a > 12 * scale) continue;
                small << Small{QRect(QPoint(q.x0, q.y0), QPoint(q.x1, q.y1)), int(id)};
            }
            std::sort(small.begin(), small.end(), [](const Small& a, const Small& b) { return a.box.left() < b.box.left(); });
            QList<bool> used(small.size(), false);
            for (int i = 0; i < small.size(); ++i) {
                if (used.at(i)) continue;
                QList<int> word{i};
                QRect last = small.at(i).box;
                for (int j = i + 1; j < small.size(); ++j) {
                    if (used.at(j)) continue;
                    const QRect r = small.at(j).box;
                    const int height = std::max(last.height(), r.height());
                    if (r.left() - last.right() > 0.8 * height) {
                        if (r.left() - last.right() > 3 * height) break;
                        continue;
                    }
                    if (std::abs(r.bottom() - last.bottom()) > 0.25 * height) continue;
                    if (r.height() < 0.4 * last.height() || r.height() > 2.5 * last.height()) continue;
                    word << j;
                    last = r;
                }
                int letters = 0;
                for (const int k : std::as_const(word)) letters += !pieces[size_t(small.at(k).id)].marker;
                if (word.size() < 3 || letters < 2) continue;
                for (const int k : std::as_const(word)) {
                    used[k] = true;
                    glyphBoxes << small.at(k).box;
                    const QRectF box = QRectF(small.at(k).box).adjusted(-1, -1, 1, 1);
                    for (int c = int(out.centres.size()) - 1; c >= 0; --c)
                        if (box.contains(out.centres.at(c))) out.centres.removeAt(c);
                }
            }
        }
        for (const QRect& g : std::as_const(glyphBoxes)) {
            ++out.glyphs;
            const QRect grown = g.adjusted(-1, -1, 1, 1);
            for (int y = std::max(0, grown.top()); y <= std::min(h - 1, grown.bottom()); ++y)
                for (int x = std::max(0, grown.left()); x <= std::min(w - 1, grown.right()); ++x) {
                    const size_t i = size_t(t.at(x, y));
                    // (Whatever of the family lies in its box: the digit in a circle.)
                    if (t.label[i] == family) {
                        t.label[i] = 0;
                        t.alpha[i] = 0;
                    }
                    t.unknown[i] = 1;
                }
        }
    }

    // How thick the family's lines are, its marks aside.
    std::vector<int> thin;
    for (size_t i = 0; i < t.label.size(); i += 3)
        if (t.label[i] == family && t.alpha[i] >= 90) thin.push_back(std::min(hrun[i], vrun[i]));
    out.thickness = std::max(1.0, 0.5 * scale);
    if (thin.size() >= 4) {
        std::nth_element(thin.begin(), thin.begin() + thin.size() / 2, thin.end());
        out.thickness = std::clamp(double(thin[thin.size() / 2]), 1.0, 12 * scale);
    }
    return out;
}

// Every family's markers and glyphs, lifted out: first each on its own, to
// know where every family's markers lie; then again, a candidate that touches
// another family's markers being a bit of a line seen between them.
QList<Marks> allMarks(Traced& t, const std::vector<int>& hrun, const std::vector<int>& vrun)
{
    const int families = int(t.families.size());
    const std::vector<uchar> label = t.label, alpha = t.alpha;
    std::vector<uchar> owners(t.label.size(), 0);
    for (int family = 1; family <= families; ++family) {
        const Marks m = marksOf(t, family, hrun, vrun, nullptr);
        const double radius = m.size / 2 + 1;
        for (const QPointF& c : m.centres) {
            const int r = int(std::ceil(radius));
            for (int y = int(c.y()) - r; y <= int(c.y()) + r; ++y)
                for (int x = int(c.x()) - r; x <= int(c.x()) + r; ++x)
                    if (t.inBounds(x, y) && std::hypot(x + 0.5 - c.x(), y + 0.5 - c.y()) <= radius) owners[size_t(t.at(x, y))] = uchar(family);
        }
    }
    t.label = label;
    t.alpha = alpha;
    std::fill(t.unknown.begin(), t.unknown.end(), 0);
    QList<Marks> out;
    for (int family = 1; family <= families; ++family) out << marksOf(t, family, hrun, vrun, &owners);
    // The band past the box, labelled for the markers, let go: no line is
    // followed along the frame.
    for (size_t i = 0; i < t.margin.size(); ++i)
        if (t.margin[i] != 0) {
            t.label[i] = 0;
            t.alpha[i] = 0;
        }
    return out;
}

double lengthOfSeries(const Series& s)
{
    double length = 0;
    for (int k = 0; k + 1 < s.points.size(); ++k)
        if (!s.breaks.contains(k + 1)) length += QLineF(s.points.at(k), s.points.at(k + 1)).length();
    return length;
}

// Pieces of one line parted by what covered it, joined where one's end points
// at the other's start: \a found from \a first on are one family's.
int joinPieces(QList<Series>& found, int first, double reach, double touch)
{
    int joined = 0;
    const auto endDirection = [](const QList<QPointF>& p, bool atEnd) {
        const int n = int(p.size());
        if (n < 2) return QPointF(1, 0);
        const int k = std::min(n - 1, 4);
        return atEnd ? normal(p.at(n - 1) - p.at(n - 1 - k)) : normal(p.at(k) - p.at(0));
    };
    for (bool again = true; again;) {
        again = false;
        for (int a = first; a < found.size() && !again; ++a) {
            if (found.at(a).shape != Series::Shape::Line || found.at(a).points.size() < 2) continue;
            int best = -1;
            double bestGap = reach;
            for (int b = first; b < found.size(); ++b) {
                if (a == b || found.at(b).shape != Series::Shape::Line || found.at(b).points.size() < 2) continue;
                const QPointF end = found.at(a).points.last(), start = found.at(b).points.first();
                const double gap = QLineF(end, start).length();
                if (gap > bestGap || start.x() < end.x() - 0.5) continue;
                // (The x of one before the other's: a function of x is not
                // joined to what lies beside it.)
                if (found.at(b).points.first().x() < found.at(a).points.last().x() - 0.5) continue;
                const QPointF across = normal(start - end);
                const QPointF da = endDirection(found.at(a).points, true), db = endDirection(found.at(b).points, false);
                // (Close enough, they meet whichever way they go: the two
                // sides of a narrow peak's top.)
                // (Or one goes up and the other comes down, near: the two sides of
                // a peak whose top neither walk could round - or of a valley.)
                const bool apex = gap <= 3 * touch && ((da.y() < -0.5 && db.y() > 0.5) || (da.y() > 0.5 && db.y() < -0.5));
                if (gap > touch && !apex && (QPointF::dotProduct(da, across) < 0.64 || QPointF::dotProduct(db, across) < 0.64)) continue;
                best = b;
                bestGap = gap;
            }
            if (best < 0) continue;
            Series& s = found[a];
            const Series other = found.at(best);
            s.bridged += other.bridged + 1;
            for (const int b : other.breaks) s.breaks << int(s.points.size()) + b;
            s.points << other.points;
            found.removeAt(best);
            ++joined;
            again = true;
        }
    }
    return joined;
}

// --- Lines as functions of x -------------------------------------------------
//
// A curve on rectangular axes is nearly always a function of x, and the
// pieces a picture gives of it - parted by markers over it, walked up one side
// of a narrow peak and up the other - are put back together as one, in order
// of x. A loop, or two curves of one colour, is not: its pieces are left as
// they were followed.

// A stretch of a line going one way in x, turned left to right (working
// points).
using Run = QList<QPointF>;

double runLength(const Run& r)
{
    double l = 0;
    for (int k = 0; k + 1 < r.size(); ++k) l += QLineF(r.at(k), r.at(k + 1)).length();
    return l;
}

// \a p's distance from \a run; in \a nearest, the point of it that begins
// the stretch nearest \a p; in \a beyond, whether that is past an end of it
// (on from it, not beside it).
double distanceTo(const Run& run, QPointF p, int* nearest = nullptr, bool* beyond = nullptr)
{
    if (nearest != nullptr) *nearest = 0;
    if (beyond != nullptr) *beyond = true;
    if (run.size() == 1) return QLineF(run.first(), p).length();
    double best = 1e18;
    const int last = int(run.size()) - 2;
    for (int k = 0; k + 1 < run.size(); ++k) {
        const QPointF a = run.at(k), ab = run.at(k + 1) - a;
        const double l2 = QPointF::dotProduct(ab, ab);
        const double raw = l2 > 0 ? QPointF::dotProduct(p - a, ab) / l2 : 0;
        const double u = std::clamp(raw, 0.0, 1.0);
        const double d = QLineF(a + ab * u, p).length();
        if (d < best) {
            if (nearest != nullptr) *nearest = k;
            if (beyond != nullptr) *beyond = (k == 0 && raw < 0) || (k == last && raw > 1);
            best = d;
        }
    }
    return best;
}

// The way \a run goes at its point \a k, over \a reach of it each side (a
// unit vector; null where it has no length there).
QPointF headingAt(const Run& run, int k, double reach)
{
    int a = k, b = k;
    for (double l = 0; a > 0 && l < reach; --a) l += QLineF(run.at(a - 1), run.at(a)).length();
    for (double l = 0; b + 1 < run.size() && l < reach; ++b) l += QLineF(run.at(b), run.at(b + 1)).length();
    const QPointF d = run.at(b) - run.at(a);
    const double l = std::hypot(d.x(), d.y());
    return l > 0 ? d / l : QPointF();
}

// A line's runs: cut where it turns back in x by more than \a turn, and where
// more than \a gap parts two of its points (what it crossed there is no part
// of it).
QList<Run> runsOfLine(const QList<QPointF>& points, const QList<int>& breaks, double turn, double gap)
{
    QList<Run> out;
    Run cur;
    int dir = 0, extreme = 0;
    const auto finish = [&] {
        if (cur.size() >= 2 && (dir < 0 || (dir == 0 && cur.last().x() < cur.first().x()))) std::reverse(cur.begin(), cur.end());
        // (A step back of less than \a turn, on a side steeper than the line
        // is wide, is the jitter of its middle: in the order walked, x held.)
        for (int k = 1; k < cur.size(); ++k) cur[k].setX(std::max(cur.at(k).x(), cur.at(k - 1).x()));
        if (!cur.isEmpty()) out << cur;
        cur.clear();
        dir = 0;
        extreme = 0;
    };
    for (int i = 0; i < points.size(); ++i) {
        const QPointF p = points.at(i);
        if (!cur.isEmpty() && (breaks.contains(i) || QLineF(cur.last(), p).length() > gap)) finish();
        // (Where it goes back the way it came - up a side steeper than the
        // line is wide and down again - it is that side walked twice.)
        if (cur.size() >= 3) {
            const QPointF came = cur.last() - cur.at(cur.size() - 3), goes = p - cur.last();
            const double a = std::hypot(came.x(), came.y()), b = std::hypot(goes.x(), goes.y());
            if (a > 0.5 * turn && b > 0 && QPointF::dotProduct(came, goes) < -0.5 * a * b) finish();
        }
        if (cur.isEmpty()) {
            cur << p;
            continue;
        }
        if (dir == 0) {
            cur << p;
            if (p.x() > cur.first().x() + turn) dir = 1;
            else if (p.x() < cur.first().x() - turn) dir = -1;
            if (dir != 0) {
                extreme = 0;
                for (int k = 1; k < cur.size(); ++k)
                    if ((dir > 0 && cur.at(k).x() >= cur.at(extreme).x()) || (dir < 0 && cur.at(k).x() <= cur.at(extreme).x())) extreme = k;
            }
            continue;
        }
        const double ex = cur.at(extreme).x();
        if ((dir > 0 && p.x() >= ex) || (dir < 0 && p.x() <= ex)) {
            cur << p;
            extreme = int(cur.size()) - 1;
        } else if (std::abs(p.x() - ex) > turn) {
            // It turned: what came after the furthest point goes the other way.
            Run rest = cur.mid(extreme + 1);
            rest << p;
            cur = cur.mid(0, extreme + 1);
            const int was = dir;
            finish();
            cur = rest;
            dir = -was;
            extreme = 0;
            for (int k = 1; k < cur.size(); ++k)
                if ((dir > 0 && cur.at(k).x() >= cur.at(extreme).x()) || (dir < 0 && cur.at(k).x() <= cur.at(extreme).x())) extreme = k;
        } else {
            cur << p;
        }
    }
    finish();
    return out;
}

// How \a runs sit against \a group: their points within the x of a run of the
// group, near it (the same line, followed twice) or far from it (another
// line at that x). False where too many are far; else \a fresh holds what is
// new: neither within \a far of the group where it is (doubled, beside it -
// on a side steeper than the line is wide, another walk of it), nor far from
// it there (another line). \a overlap: how many were within its x.
bool fitsWith(const QList<Run>& group, const QList<Run>& runs, double near, double far, double turn, QList<Run>* fresh, int* overlap = nullptr)
{
    int over = 0, apart = 0;
    QList<Run> kept;
    for (const Run& r : runs) {
        Run keep;
        // (What is left of a run between points it doubles is kept where it
        // is more than a straggler or two: those are the far edge of a walk
        // along its other side, and stand out of the line.)
        const auto keepIt = [&] {
            if (keep.size() >= 3 || keep.size() == r.size()) kept << keep;
            keep.clear();
        };
        for (int i = 0; i < r.size(); ++i) {
            const QPointF p = r.at(i);
            bool within = false, close = false, distant = false;
            for (const Run& g : group) {
                if (g.isEmpty()) continue;
                const bool inside = g.size() >= 2 && p.x() > g.first().x() + turn && p.x() < g.last().x() - turn;
                const bool atEnds = !inside && p.x() >= g.first().x() - turn && p.x() <= g.last().x() + turn;
                if (!inside && !atEnds) continue;
                int k = 0;
                bool beyond = false;
                const double d = distanceTo(g, p, &k, &beyond);
                // (At a run's ends too, for what doubles it, where the run is
                // longer than a few steps: a line there.) Further than the
                // line is wide, a double goes the way the group goes there:
                // the other side of a narrow peak, as near near its top, goes
                // the other way in y, and is no double (the third recheck's
                // T1). Nor is what lies on past the run's end, up a steep
                // side where a marker parted the walk: a double is beside it.
                bool doubles = d <= near;
                if (!doubles && !beyond && d <= (inside || g.size() >= 4 ? far : near)) {
                    const QPointF mine = headingAt(r, i, turn), theirs = headingAt(g, k, turn);
                    doubles = mine.isNull() || theirs.isNull() || QPointF::dotProduct(mine, theirs) >= 0.87;
                }
                if (doubles) {
                    close = true;
                    break;
                }
                if (!inside) continue;
                within = true;
                distant = distant || d > far;
            }
            over += within || close;
            apart += within && !close && distant;
            if (!close && !(within && distant)) keep << p;
            else if (!keep.isEmpty()) keepIt();
        }
        if (!keep.isEmpty()) keepIt();
    }
    if (overlap != nullptr) *overlap = over;
    if (apart >= std::max(3, int(0.2 * over))) return false;
    if (fresh != nullptr) *fresh = kept;
    return true;
}

// Whether \a piece, beside \a group with nothing of x in common, carries on
// from it: at a side where the group is, its end where the group's end is,
// or where either's end heads, over a short way, within a few line widths
// (\a far) - more across a longer gap, only if \a longWay. (Another thing of
// the colour at another height - a legend's key above a row of markers -
// does not.)
bool carriesOn(const QList<Run>& group, const QList<Run>& piece, double far, double breakAt, bool longWay)
{
    QPointF first(1e18, 0), last(-1e18, 0);
    for (const Run& r : piece) {
        if (r.isEmpty()) continue;
        if (r.first().x() < first.x()) first = r.first();
        if (r.last().x() > last.x()) last = r.last();
    }
    if (first.x() > last.x()) return false;
    // The group's points nearest each side, and the runs they are in.
    int leftRun = -1, leftAt = -1, rightRun = -1, rightAt = -1;
    for (int g = 0; g < group.size(); ++g)
        for (int k = 0; k < group.at(g).size(); ++k) {
            const QPointF q = group.at(g).at(k);
            if (q.x() <= first.x() && (leftRun < 0 || q.x() > group.at(leftRun).at(leftAt).x())) {
                leftRun = g;
                leftAt = k;
            }
            if (q.x() >= last.x() && (rightRun < 0 || q.x() < group.at(rightRun).at(rightAt).x())) {
                rightRun = g;
                rightAt = k;
            }
        }
    // Where a run heads at its start (\a atStart) or its end, over the last
    // few points there: its y at \a x.
    const auto heading = [&](const Run& r, bool atStart, double x, double* y) {
        if (r.size() < 2) return false;
        const QPointF e = atStart ? r.first() : r.last();
        QPointF o = e;
        for (int k = 0; k < r.size(); ++k) {
            const QPointF q = atStart ? r.at(k) : r.at(r.size() - 1 - k);
            if (std::abs(q.x() - e.x()) > 3) break;
            o = q;
        }
        if (std::abs(o.x() - e.x()) < 0.5) return false;
        *y = e.y() + (e.y() - o.y()) / (e.x() - o.x()) * (x - e.x());
        return true;
    };
    const auto side = [&](int g, int k, QPointF end, bool pieceOnRight) {
        if (g < 0) return false;
        const Run& run = group.at(g);
        const QPointF q = run.at(k);
        const double gap = std::abs(end.x() - q.x());
        if (!longWay && gap > 3 * breakAt) return false;
        const double allowed = std::max(2 * far, 0.3 * gap);
        double off = std::abs(end.y() - q.y());
        if (gap <= 3 * breakAt) {
            double y = 0;
            // (The group's run heads on from its end; the piece's, back.)
            if ((pieceOnRight ? k == run.size() - 1 : k == 0) && heading(run, !pieceOnRight, end.x(), &y)) off = std::min(off, std::abs(y - end.y()));
            for (const Run& r : piece)
                if (!r.isEmpty() && (pieceOnRight ? r.first() == end : r.last() == end) && heading(r, pieceOnRight, q.x(), &y)) off = std::min(off, std::abs(y - q.y()));
        }
        return off <= allowed;
    };
    return side(leftRun, leftAt, first, true) || side(rightRun, rightAt, last, false);
}

// The pieces of one family's lines as functions of x where they are: each
// piece checked, then those that fit together put together. \a width: of the
// line, points. Lines are page points; \a toWork maps them to the frame's.
QList<Series> asFunctions(const QList<Series>& pieces, const QTransform& toWork, double width, int* puttogether, int* loops)
{
    const double turn = std::max(1.5 * width, 0.6), gap = std::max(3 * width, 2.0);
    const double near = std::max(width, 0.5), far = std::max(4 * width, 2.5);
    const double breakAt = 8;
    const QTransform toPage = toWork.inverted();
    struct Piece {
        QList<Run> runs;
        double length = 0;
        int from = 0;   // the series it came from
    };
    QList<Series> out;
    QList<Piece> functions;
    for (int i = 0; i < pieces.size(); ++i) {
        const Series& s = pieces.at(i);
        if (s.shape != Series::Shape::Line || s.points.size() < 2) {
            out << s;
            continue;
        }
        QList<QPointF> work;
        work.reserve(s.points.size());
        for (const QPointF& q : s.points) work << toWork.map(q);
        // (Cut where a step is much longer than its steps are: a jump, across
        // a peak or under a marker, is no part of the line.)
        std::vector<double> steps;
        for (int k = 1; k < work.size(); ++k) steps.push_back(QLineF(work.at(k - 1), work.at(k)).length());
        double usual = gap;
        if (!steps.empty()) {
            std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
            usual = steps[steps.size() / 2];
        }
        QList<Run> runs = runsOfLine(work, s.breaks, turn, std::clamp(1.8 * usual, 1.0, gap));
        std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return runLength(a) > runLength(b); });
        // Its runs, longest first, each where it fits with those before it.
        QList<Run> accepted, rejected;
        double total = 0, off = 0;
        for (const Run& r : std::as_const(runs)) {
            total += runLength(r);
            QList<Run> fresh;
            if (fitsWith(accepted, {r}, near, far, turn, &fresh)) accepted << fresh;
            else {
                rejected << r;
                off += runLength(r);
            }
        }
        if (off > 0.4 * total) {
            // A loop, or two lines met: as it was followed.
            out << s;
            ++*loops;
            continue;
        }
        Piece piece;
        piece.runs = accepted;
        piece.length = total - off;
        piece.from = i;
        functions << piece;
        // What did not fit, a piece of its own (a scrap, mostly).
        for (const Run& r : std::as_const(rejected)) {
            Piece extra;
            extra.runs = {r};
            extra.length = runLength(r);
            extra.from = i;
            functions << extra;
        }
    }
    // Put together, longest first: a piece joins the first group it fits -
    // where it has x in common with it, or carries on from it.
    std::stable_sort(functions.begin(), functions.end(), [](const Piece& a, const Piece& b) { return a.length > b.length; });
    struct Group {
        QList<Run> runs;
        QList<int> from;
    };
    QList<Group> groups;
    for (const Piece& piece : std::as_const(functions)) {
        bool placed = false;
        for (Group& g : groups) {
            QList<Run> fresh;
            int over = 0;
            if (!fitsWith(g.runs, piece.runs, near, far, turn, &fresh, &over)) continue;
            if (over <= 2 && !carriesOn(g.runs, piece.runs, far, breakAt, false)) continue;
            g.runs << fresh;
            g.from << piece.from;
            placed = true;
            ++*puttogether;
            break;
        }
        if (!placed) groups << Group{piece.runs, {piece.from}};
    }
    // Then curves seen in pieces far apart - a stretch hidden under markers
    // between them - put together, the longest first, where they share no x
    // and the one carries on from the other, or they pass through much the
    // same heights (two peaks of one curve; not a legend's key above a row
    // of markers).
    const auto spanOf = [](const Group& g) {
        double lo = 1e18, hi = -1e18;
        for (const Run& r : g.runs)
            if (!r.isEmpty()) {
                lo = std::min(lo, r.first().x());
                hi = std::max(hi, r.last().x());
            }
        return std::make_pair(lo, hi);
    };
    const auto lengthOf = [](const Group& g) {
        double l = 0;
        for (const Run& r : g.runs) l += runLength(r);
        return l;
    };
    const auto heightsMeet = [](const Group& a, const Group& b) {
        const auto heights = [](const Group& g) {
            double lo = 1e18, hi = -1e18;
            for (const Run& r : g.runs)
                for (const QPointF& q : r) {
                    lo = std::min(lo, q.y());
                    hi = std::max(hi, q.y());
                }
            return std::make_pair(lo, hi);
        };
        const auto ha = heights(a), hb = heights(b);
        const double shared = std::min(ha.second, hb.second) - std::max(ha.first, hb.first);
        return shared > 0 && shared >= 0.5 * std::min(ha.second - ha.first, hb.second - hb.first);
    };
    for (bool again = true; again;) {
        again = false;
        std::stable_sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) { return lengthOf(a) > lengthOf(b); });
        for (int a = 0; a < groups.size() && !again; ++a)
            for (int b = a + 1; b < groups.size() && !again; ++b) {
                const auto sa = spanOf(groups.at(a)), sb = spanOf(groups.at(b));
                if (!(sa.second < sb.first || sb.second < sa.first)) continue;
                if (!carriesOn(groups.at(a).runs, groups.at(b).runs, far, breakAt, true) && !heightsMeet(groups.at(a), groups.at(b))) continue;
                groups[a].runs << groups.at(b).runs;
                groups[a].from << groups.at(b).from;
                groups.removeAt(b);
                ++*puttogether;
                again = true;
            }
    }
    for (const Group& g : std::as_const(groups)) {
        // In order of x, each run in the order it was walked: by its first x,
        // the next carrying on from where the last left off. (Sorting every
        // point by x would shuffle a side steeper than the line is wide.)
        // Where runs share more x than the jitter of a line's middle: merged
        // by x, or on a steep side both go up (or down), by y - the order
        // along it - with x held from going back.
        QList<Run> runs;
        for (const Run& r : g.runs)
            if (!r.isEmpty()) runs << r;
        std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.first().x() < b.first().x(); });
        QList<QPointF> all;
        const auto append = [&](const QList<QPointF>& points) {
            for (const QPointF& q : points) all << QPointF(std::max(q.x(), all.isEmpty() ? q.x() : all.last().x()), q.y());
        };
        for (Run r : std::as_const(runs)) {
            // (A run hardly wider than the line - a bit of a steep side -
            // has no way in x of its own: the way that carries on.)
            if (!all.isEmpty() && r.size() >= 2 && r.last().x() - r.first().x() < turn && QLineF(all.last(), r.last()).length() < QLineF(all.last(), r.first()).length()) {
                std::reverse(r.begin(), r.end());
                for (int k = 1; k < r.size(); ++k) r[k].setX(std::max(r.at(k).x(), r.at(k - 1).x()));
            }
            if (all.isEmpty() || r.first().x() >= all.last().x() - 0.5 * turn) {
                append(r);
                continue;
            }
            // What both cover: the last of what is there, from where this
            // starts; this, to where that ends.
            qsizetype from = all.size();
            while (from > 0 && all.at(from - 1).x() > r.first().x() - 0.5 * turn) --from;
            QList<QPointF> both = all.mid(from);
            all.resize(from);
            const double until = both.last().x();
            qsizetype in = 0;
            while (in < r.size() && r.at(in).x() <= until) ++in;
            const QList<QPointF> rIn = r.mid(0, in), rAfter = r.mid(in);
            const double over = std::max(until - r.first().x(), 1e-6);
            const double rdy = rIn.isEmpty() ? 0 : rIn.last().y() - rIn.first().y(), tdy = both.last().y() - both.first().y();
            const bool steep = std::abs(rdy) > 3 * over && std::abs(tdy) > 3 * over && rdy * tdy > 0;
            both << rIn;
            const double way = rdy > 0 ? 1 : -1;
            if (steep) std::stable_sort(both.begin(), both.end(), [way](const QPointF& a, const QPointF& b) { return way * a.y() < way * b.y(); });
            else std::stable_sort(both.begin(), both.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
            append(both);
            append(rAfter);
        }
        // Points of a steep side at much the same x - held from going back,
        // or a sliver apart, past a gap under markers too - in the order
        // along it: by y, from the height before them to the height after.
        // A peak's top, past both, too narrow for its sides to part in x:
        // up to it from the height before, down from it to the height after.
        for (qsizetype i = 0; i < all.size();) {
            qsizetype j = i + 1;
            while (j < all.size() && all.at(j).x() - all.at(j - 1).x() < 0.5 * turn) ++j;
            if (j - i >= 3) {
                // (Past a gap too: which way the side goes.)
                const double before = i > 0 ? all.at(i - 1).y() : all.at(i).y();
                const double after = j < all.size() ? all.at(j).y() : all.at(j - 1).y();
                qsizetype low = i, high = i;
                for (qsizetype k = i; k < j; ++k) {
                    if (all.at(k).y() < all.at(low).y()) low = k;
                    if (all.at(k).y() > all.at(high).y()) high = k;
                }
                const bool dip = all.at(low).y() < std::min(before, after) - 2 * far, rise = all.at(high).y() > std::max(before, after) + 2 * far;
                std::vector<double> xs;
                for (qsizetype k = i; k < j; ++k) xs.push_back(all.at(k).x());
                std::sort(xs.begin(), xs.end());
                if (dip != rise) {
                    // (Heights measured towards the top: \c v.)
                    const double sign = rise ? 1 : -1;
                    const auto v = [sign](double y) { return sign * y; };
                    // (Past the top as followed, on the way down - unless
                    // lower than the line just after: it goes on down from
                    // the top, not back up.)
                    double next = v(after);
                    for (qsizetype k = j; k < all.size() && all.at(k).x() <= all.at(j - 1).x() + far; ++k) next = std::max(next, v(all.at(k).y()));
                    QList<QPointF> up, down;
                    const qsizetype at = rise ? high : low;
                    for (qsizetype k = i; k < j; ++k) {
                        const QPointF p = all.at(k);
                        (k > at && v(p.y()) >= next ? down : up) << p;
                    }
                    std::stable_sort(up.begin(), up.end(), [&](const QPointF& a, const QPointF& b) { return v(a.y()) < v(b.y()); });
                    std::stable_sort(down.begin(), down.end(), [&](const QPointF& a, const QPointF& b) { return v(a.y()) > v(b.y()); });
                    up << down;
                    for (qsizetype k = i; k < j; ++k) all[k] = QPointF(xs[size_t(k - i)], up.at(k - i).y());
                } else if (!dip && std::abs(after - before) > far) {
                    const bool up = after > before;
                    std::stable_sort(all.begin() + i, all.begin() + j, [up](const QPointF& a, const QPointF& b) { return up ? a.y() < b.y() : a.y() > b.y(); });
                    for (qsizetype k = i; k < j; ++k) all[k].setX(xs[size_t(k - i)]);
                }
            }
            i = j;
        }
        if (all.size() < 2) continue;
        // Like the longest of what it was made of.
        int first = g.from.first();
        for (const int k : g.from)
            if (pieces.at(k).points.size() > pieces.at(first).points.size()) first = k;
        Series s = pieces.at(first);
        s.points.clear();
        s.breaks.clear();
        s.bridged = 0;
        QList<int> seen;
        for (const int k : g.from)
            if (!seen.contains(k)) {
                seen << k;
                s.bridged += pieces.at(k).bridged;
            }
        s.bridged += int(g.runs.size()) - 1;
        // (A step much longer than the line's steps is a gap in it - unless it
        // moves less than the line is wide in x: up or down a side, or a peak
        // too narrow for its sides to part, where a line drawn straight is
        // the curve to within its width.)
        for (int k = 0; k < all.size(); ++k) {
            if (k > 0 && QLineF(all.at(k - 1), all.at(k)).length() > breakAt && std::abs(all.at(k).x() - all.at(k - 1).x()) > near) s.breaks << int(s.points.size());
            s.points << toPage.map(all.at(k));
        }
        out << s;
    }
    return out;
}

// Bits at the ends of a line standing alone past a gap, shorter than \a least
// (points), taken out: a legend's sample, a speck, met at the end of a walk.
// (One between two gaps inside the line is a piece of it seen between
// markers, and stays.)
int dropShortBits(Series& s, double least)
{
    if (s.shape != Series::Shape::Line) return 0;
    int dropped = 0;
    for (bool again = true; again && s.points.size() >= 2;) {
        again = false;
        QList<int> starts{0};
        for (const int b : std::as_const(s.breaks))
            if (b > 0 && b < s.points.size()) starts << b;
        std::sort(starts.begin(), starts.end());
        starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
        if (starts.size() < 2) break;
        starts << int(s.points.size());
        const auto lengthOf = [&](int k) {
            double l = 0;
            for (int i = starts.at(k); i + 1 < starts.at(k + 1); ++i) l += QLineF(s.points.at(i), s.points.at(i + 1)).length();
            return l;
        };
        const int last = int(starts.size()) - 2;
        for (const int k : {0, last}) {
            if (lengthOf(k) >= least) continue;
            const int from = starts.at(k), to = starts.at(k + 1);
            s.points.erase(s.points.begin() + from, s.points.begin() + to);
            QList<int> breaks;
            for (const int b : std::as_const(s.breaks)) {
                const int moved = b >= to ? b - (to - from) : b;
                if (moved > 0 && moved < s.points.size() && !breaks.contains(moved)) breaks << moved;
            }
            s.breaks = breaks;
            ++dropped;
            again = true;
            break;
        }
    }
    return dropped;
}

// Whether \a pixels are bits no longer than a marker is across (\a size),
// two or more, parted by steps longer than one - and those steps go up and
// down by more than a marker, or the bits lie by \a centres (markers of the
// colour): the strokes of markers not found put in a row, not a curve.
bool markerBits(const QList<QPointF>& pixels, const QList<QPointF>& centres, double size)
{
    if (pixels.size() < 2) return false;
    int bits = 1, reversals = 0;
    double lastRise = 0;
    QRectF bit(pixels.first(), QSizeF(0, 0));
    for (int i = 1; i < pixels.size(); ++i) {
        const QPointF step = pixels.at(i) - pixels.at(i - 1);
        if (std::hypot(step.x(), step.y()) > size) {
            ++bits;
            bit = QRectF(pixels.at(i), QSizeF(0, 0));
            if (std::abs(step.y()) > size) {
                reversals += lastRise * step.y() < 0;
                lastRise = step.y();
            }
            continue;
        }
        bit = bit.united(QRectF(pixels.at(i), QSizeF(0, 0)));
        if (std::max(bit.width(), bit.height()) > 1.5 * size) return false;
    }
    if (bits < 2) return false;
    if (reversals >= 1) return true;
    std::vector<double> far;
    for (const QPointF& p : pixels) {
        double nearest = 1e18;
        for (const QPointF& c : centres) nearest = std::min(nearest, QLineF(c, p).length());
        far.push_back(nearest);
    }
    std::nth_element(far.begin(), far.begin() + far.size() / 2, far.end());
    return far[far.size() / 2] <= 2 * size;
}

// The stretches of \a s between its gaps that lie mostly on \a centres
// (markers of its colour, pixels, \a size across) taken out: what is left of
// a row of markers once they are lifted out, put together with a line. So
// are stretches of marker-sized bits (markerBits()): markers not found.
int dropStretchesOnMarkers(Series& s, const Traced& t, const QTransform& toWork, const QList<QPointF>& centres, double size)
{
    if (s.shape != Series::Shape::Line || s.points.size() < 2) return 0;
    QList<int> starts{0};
    for (const int b : std::as_const(s.breaks))
        if (b > 0 && b < s.points.size()) starts << b;
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    starts << int(s.points.size());
    QList<QPointF> points;
    QList<int> breaks;
    int dropped = 0;
    for (int k = 0; k + 1 < starts.size(); ++k) {
        const int from = starts.at(k), to = starts.at(k + 1);
        int on = 0;
        QList<QPointF> pixels;
        for (int i = from; i < to; ++i) {
            const QPointF pixel = t.px.toPixel(toWork.map(s.points.at(i)));
            pixels << pixel;
            for (const QPointF& c : centres)
                if (std::abs(c.x() - pixel.x()) <= size && std::abs(c.y() - pixel.y()) <= size && QLineF(c, pixel).length() <= size) {
                    ++on;
                    break;
                }
        }
        if (on >= 0.6 * (to - from) || markerBits(pixels, centres, size)) {
            ++dropped;
            continue;
        }
        if (!points.isEmpty()) breaks << int(points.size());
        for (int i = from; i < to; ++i) points << s.points.at(i);
    }
    if (dropped == 0) return 0;
    s.points = points;
    s.breaks = breaks;
    return dropped;
}

} // namespace

// Pieces of \a s of a single point between gaps - no line to draw, a value
// a reader would have to skip - joined to the piece beside them whose end is
// nearer, where it is within \a reach (points); else taken out. How many.
int foldLonePoints(Series& s, double reach)
{
    if (s.shape != Series::Shape::Line || s.breaks.isEmpty()) return 0;
    int folded = 0;
    for (bool again = true; again;) {
        again = false;
        QList<int> starts{0};
        for (const int b : std::as_const(s.breaks))
            if (b > 0 && b < s.points.size()) starts << b;
        std::sort(starts.begin(), starts.end());
        starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
        starts << int(s.points.size());
        for (int k = 0; k + 1 < starts.size(); ++k) {
            const int at = starts.at(k);
            if (starts.at(k + 1) - at != 1) continue;
            const double before = at > 0 ? QLineF(s.points.at(at - 1), s.points.at(at)).length() : 1e18;
            const double after = at + 1 < s.points.size() ? QLineF(s.points.at(at), s.points.at(at + 1)).length() : 1e18;
            QList<int> breaks;
            if (std::min(before, after) <= reach) {
                // (The gap on its nearer side closed.)
                const int closed = before <= after ? at : at + 1;
                for (const int b : std::as_const(s.breaks))
                    if (b != closed && b > 0 && b < s.points.size() && !breaks.contains(b)) breaks << b;
            } else {
                s.points.removeAt(at);
                for (const int b : std::as_const(s.breaks)) {
                    const int moved = b > at ? b - 1 : b;
                    if (moved > 0 && moved < s.points.size() && !breaks.contains(moved)) breaks << moved;
                }
            }
            s.breaks = breaks;
            ++folded;
            again = true;
            break;
        }
    }
    return folded;
}

bool extractRaster(Source& source, Plot& plot, const Options& options, QStringList* notes)
{
    Traced t;
    QList<Token> words;
    setUp(t, source, plot, options, &words, notes);
    if (t.px.image.isNull() || t.px.w < 8 || t.px.h < 8) return false;
    plot.leftOut = t.leftOut;
    const int w = t.px.w, h = t.px.h;
    plot.renderScale = t.px.scale;
    const double scale = t.px.scale;

    // A legend: words on the plot with a sample of a colour just before them.
    QHash<int, QStringList> legendNames;
    {
        const QList<Phrase> phrases = phrasesOf(words);
        for (const Phrase& ph : phrases) {
            double v = 0;
            if (parseTickLabel(ph.text, &v)) continue;
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
            for (int y = pixels.top(); y <= pixels.bottom(); ++y)
                for (int x = pixels.left(); x <= pixels.right(); ++x) {
                    t.label[size_t(t.at(x, y))] = 0;
                    t.alpha[size_t(t.at(x, y))] = 0;
                }
        }
    }

    // Every family's markers and glyphs first: a line of one family is
    // followed through another's.
    std::vector<int> hrun, vrun;
    runsOf(t, hrun, vrun);
    const int families = int(t.families.size());
    const QList<Marks> marks = allMarks(t, hrun, vrun);
    int fromChains = 0, glyphs = 0;
    for (int f = 0; f < marks.size(); ++f) {
        if (t.families.at(f).hidden) continue;
        fromChains += marks.at(f).fromChains;
        glyphs += marks.at(f).glyphs;
    }

    QList<Series> found;
    QElapsedTimer clock;
    clock.start();
    std::vector<int> stamps(size_t(w) * h, 0);
    int steps = 0, joined = 0, scraps = 0, alongMarkers = 0, puttogether = 0, loops = 0, bits = 0, mostlyGaps = 0, lone = 0;
    for (int family = 1; family <= families; ++family) {
        const Family& fam = t.families.at(family - 1);
        // (A colour not asked for: its markers are lifted out, nothing more.)
        if (fam.hidden) continue;
        const Marks& m = marks.at(family - 1);
        const double thickness = m.thickness;
        const double keepAt = std::max({4 * thickness, 4.0 * scale, 0.02 * std::max(w, h)});
        int tracks = 0;
        const int mostTracks = std::max(1, options.maxSeries);
        QList<Series> pieces;
        for (int x = 0; x < w && tracks < mostTracks * 6; ++x)
            for (int y = 0; y < h && tracks < mostTracks * 6; ++y) {
                const size_t i = size_t(t.at(x, y));
                if (t.label[i] != family || stamps[i] != 0 || t.alpha[i] < 60) continue;
                if (clock.elapsed() > 20000) {
                    notes->append(tr("The trace stopped after 20 s: what was found so far is here."));
                    x = w;
                    break;
                }
                Series s;
                if (!followFrom(t, family, thickness, QPointF(x + 0.5, y + 0.5), stamps, steps, &s)) continue;
                ++tracks;
                if (s.points.size() < 2) continue;
                pieces << s;
            }
        // What is left of a row of the family's own markers, once they are
        // lifted out, is no line: its points lie on them.
        if (!m.centres.isEmpty() && m.size > 0) {
            const double reach = m.size;
            for (int k = int(pieces.size()) - 1; k >= 0; --k) {
                int onMarkers = 0;
                for (const QPointF& q : pieces.at(k).points) {
                    const QPointF pixel = t.px.toPixel(plot.work.map(q));
                    for (const QPointF& c : m.centres)
                        if (std::abs(c.x() - pixel.x()) <= reach && std::abs(c.y() - pixel.y()) <= reach && QLineF(c, pixel).length() <= reach) {
                            ++onMarkers;
                            break;
                        }
                }
                if (onMarkers >= 0.6 * pieces.at(k).points.size()) {
                    pieces.removeAt(k);
                    ++alongMarkers;
                }
            }
        }
        // Pieces of one line, parted by what covered it, joined; each then
        // made a function of x where it is one, and those of one curve put
        // together; then what is still a scrap beside the long lines goes.
        joined += joinPieces(pieces, 0, 40.0, std::max(1.5, 3 * thickness / scale));
        if (plot.calibration.kind == Kind::Cartesian) {
            pieces = asFunctions(pieces, plot.work, thickness / scale, &puttogether, &loops);
            // Put together, a line may hold stretches of what is left of the
            // markers between its gaps: each stretch looked at again. And one
            // that is more gaps than line is no curve: scraps of the colour
            // put in a row.
            if (!m.centres.isEmpty() && m.size > 0)
                for (Series& s : pieces) alongMarkers += dropStretchesOnMarkers(s, t, plot.work, m.centres, m.size);
            // (A line all of whose stretches went is gone.)
            pieces.removeIf([](const Series& s) { return s.shape == Series::Shape::Line && s.points.size() < 2; });
            // (In a colour with markers of its own, where scraps of them
            // are; a line under another colour's markers is seen in pieces.)
            for (int k = int(pieces.size()) - 1; k >= 0 && !m.centres.isEmpty(); --k) {
                const Series& s = pieces.at(k);
                if (s.shape == Series::Shape::Line && s.breaks.size() >= 6 && s.breaks.size() * 10 > s.points.size()) {
                    pieces.removeAt(k);
                    ++mostlyGaps;
                }
            }
            for (Series& s : pieces) bits += dropShortBits(s, keepAt / scale);
            for (Series& s : pieces) lone += foldLonePoints(s, 16.0);
            // A short line beside the colour's own markers - most of it within
            // two markers' width of them - is what is left of some: a ring
            // not found, on a narrow peak's top. (A line drawn through its
            // markers is long.)
            if (!m.centres.isEmpty() && m.size > 0)
                for (int k = int(pieces.size()) - 1; k >= 0; --k) {
                    const Series& s = pieces.at(k);
                    if (s.shape != Series::Shape::Line || lengthOfSeries(s) * scale > 0.25 * std::max(w, h)) continue;
                    std::vector<double> far;
                    for (const QPointF& q : s.points) {
                        const QPointF pixel = t.px.toPixel(plot.work.map(q));
                        double nearest = 1e18;
                        for (const QPointF& c : m.centres) nearest = std::min(nearest, QLineF(c, pixel).length());
                        far.push_back(nearest);
                    }
                    std::nth_element(far.begin(), far.begin() + far.size() / 2, far.end());
                    if (far[far.size() / 2] <= 2 * m.size) {
                        pieces.removeAt(k);
                        ++alongMarkers;
                    }
                }
        }
        double longest = 0;
        for (const Series& s : std::as_const(pieces)) longest = std::max(longest, lengthOfSeries(s));
        for (Series s : std::as_const(pieces)) {
            const double length = lengthOfSeries(s);
            if (length * scale < keepAt || (length < 0.12 * longest && length * scale < 0.08 * std::max(w, h))) {
                ++scraps;
                continue;
            }
            s.asked = fam.asked;
            found << s;
        }
        if (!m.centres.isEmpty()) {
            Series markers;
            QList<QPointF> pagePoints;
            // (Inside the frame: one found in the band past it is no data.)
            const QRectF inFrame = plot.frame.isNull() ? QRectF() : plot.frame.adjusted(-1, -1, 1, 1);
            for (const QPointF& pixel : m.centres)
                if (inFrame.isNull() || inFrame.contains(t.px.toWork(pixel))) pagePoints << t.px.toPage(pixel);
            std::sort(pagePoints.begin(), pagePoints.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
            markers.points = pagePoints;
            markers.shape = Series::Shape::Markers;
            markers.origin = Series::Origin::Raster;
            // Its colour by the darkest of each marker - its stroke - not by
            // its average, which a line through it and the white inside a
            // ring water down.
            QList<int> reds, greens, blues;
            const int r = std::max(1, int(std::ceil(m.size / 2)));
            for (const QPointF& c : m.centres) {
                int best = -1;
                QRgb darkest = 0;
                for (int y = int(c.y()) - r; y <= int(c.y()) + r; ++y)
                    for (int x = int(c.x()) - r; x <= int(c.x()) + r; ++x) {
                        if (!t.inBounds(x, y)) continue;
                        const QRgb v = reinterpret_cast<const QRgb*>(t.px.image.constScanLine(y))[x];
                        const int d = luminanceDark(v);
                        if (d > best) {
                            best = d;
                            darkest = v;
                        }
                    }
                if (best < 0) continue;
                reds << qRed(darkest);
                greens << qGreen(darkest);
                blues << qBlue(darkest);
            }
            markers.colour = fam.colour;
            if (!reds.isEmpty()) {
                std::sort(reds.begin(), reds.end());
                std::sort(greens.begin(), greens.end());
                std::sort(blues.begin(), blues.end());
                markers.colour = QColor(reds.at(reds.size() / 2), greens.at(greens.size() / 2), blues.at(blues.size() / 2));
            }
            markers.width = thickness / scale;
            markers.asked = fam.asked;
            found << markers;
        }
    }
    if (fromChains > 0) notes->append(tr("%1 markers were found where they touch others (by the holes of rings, or the middles of discs).").arg(fromChains));
    if (glyphs > 0) notes->append(tr("%1 glyphs drawn in the picture (a circled label, a letter) were left out, and lines followed through them.").arg(glyphs));
    if (alongMarkers > 0) notes->append(tr("%1 lines, or stretches of lines between gaps, running along rows of markers of their own colour were taken for what is left of the markers, and left out.").arg(alongMarkers));
    if (mostlyGaps > 0) notes->append(tr("%1 lines more gaps than line (a gap every ten points or less) were taken for scraps of their colour put in a row, and left out.").arg(mostlyGaps));
    if (joined > 0) notes->append(tr("%1 pieces of lines parted by what lay over them were joined.").arg(joined));
    if (puttogether > 0) notes->append(tr("%1 pieces of lines of one colour were put together as one curve, in order of x: each line comes back in order of x, with a gap where nothing of it could be seen.").arg(puttogether));
    if (loops > 0) notes->append(tr("%1 lines are not single-valued in x (a loop, or two curves of one colour met): they are in the order they were followed.").arg(loops));
    if (bits > 0) notes->append(tr("%1 short bits standing alone at the ends of lines (a legend's sample, a speck) were left out.").arg(bits));
    if (lone > 0) notes->append(tr("%1 single points between gaps in lines were joined to the line beside them, or left out where none was near.").arg(lone));
    if (scraps > 0) notes->append(tr("%1 scraps much shorter than the lines of their colour were left out.").arg(scraps));

    // Names, from the legend by colour.
    {
        QHash<int, QStringList> left = legendNames;
        for (Series& s : found) {
            int bestFamily = 0;
            double bestD = 1e18;
            for (int f = 0; f < families; ++f) {
                if (t.families.at(f).hidden) continue;
                const double d = colourDistance(t.families.at(f).colour, s.colour);
                if (d < bestD) {
                    bestD = d;
                    bestFamily = f + 1;
                }
            }
            QStringList& names = left[bestFamily];
            if (!names.isEmpty()) s.name = names.takeFirst();
        }
    }
    // Short lines in an inset's own colour are what is left of it - its legend's
    // swatch, an edge the margin missed - not data.
    if (!t.insetColours.isEmpty()) {
        int dropped = 0;
        QList<Series> kept;
        for (const Series& s : std::as_const(found)) {
            bool insetLike = false;
            for (const QColor& c : t.insetColours) insetLike = insetLike || colourDistance(c, s.colour) < 60;
            if (insetLike && lengthOfSeries(s) * scale < 0.3 * w) {
                ++dropped;
                continue;
            }
            kept << s;
        }
        found = kept;
        if (dropped > 0) notes->append(tr("%1 short lines of an inset's own colour were left out with it.").arg(dropped));
    }
    // Only the colours asked for, darkness and all: what came out of another
    // (a grey line asked for black) goes.
    if (!options.colours.isEmpty()) {
        int dropped = 0;
        QList<Series> kept;
        for (const Series& s : std::as_const(found)) {
            double nearest = 1e9;
            for (const QColor& c : options.colours) nearest = std::min(nearest, colourDistance(c, s.colour));
            if (s.shape != Series::Shape::Markers && nearest > 160) {
                ++dropped;
                continue;
            }
            kept << s;
        }
        found = kept;
        if (dropped > 0) notes->append(tr("%1 lines of colours not asked for were left out.").arg(dropped));
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
            QPolygonF inWork;
            for (const QPointF& q : s.points) inWork << plot.work.map(q);
            const QRectF box = inWork.boundingRect();
            const bool axisAligned = box.width() < 0.8 || box.height() < 0.8;
            int onEdges = 0;
            for (const QPointF& q : std::as_const(inWork))
                if (std::abs(q.x() - box.left()) < 1 || std::abs(q.x() - box.right()) < 1 || std::abs(q.y() - box.top()) < 1 || std::abs(q.y() - box.bottom()) < 1) ++onEdges;
            const bool boxOutline = !axisAligned && box.width() > 8 && box.height() > 8 && onEdges >= 0.9 * s.points.size();
            // Along the frame, or an axis drawn inside it: most of it within
            // a line or two of the frame's edges.
            int alongFrame = 0;
            if (!plot.frame.isNull()) {
                const double reach = std::max(2.0, 3 * s.width);
                const QRectF f = plot.frame;
                for (const QPointF& q : std::as_const(inWork))
                    alongFrame += std::abs(q.y() - f.bottom()) <= reach || std::abs(q.y() - f.top()) <= reach || std::abs(q.x() - f.left()) <= reach || std::abs(q.x() - f.right()) <= reach;
            }
            const bool onFrame = alongFrame >= 0.8 * s.points.size();
            if (axisAligned || boxOutline || onFrame) {
                ++dropped;
                continue;
            }
            kept << s;
        }
        found = kept;
        if (dropped > 0) notes->append(tr("%1 straight or boxed black or grey lines, or ones along the frame, were taken for the grid, an axis or a legend's frame and left out.").arg(dropped));
    }
    if (found.size() > options.maxSeries) {
        std::stable_sort(found.begin(), found.end(), [](const Series& a, const Series& b) { return a.points.size() > b.points.size(); });
        notes->append(tr("%1 series were found; the %2 with the most points are kept.").arg(found.size()).arg(options.maxSeries));
        plot.seriesLeftOut = int(found.size()) - options.maxSeries;
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
    plot.series = found;
    plot.route = QStringLiteral("raster");
    return !found.isEmpty();
}

bool traceRaster(Source& source, Plot& plot, QPointF seed, const Options& options, Series* out, QString* why)
{
    Traced t;
    QStringList notes;
    setUp(t, source, plot, options, nullptr, &notes);
    if (t.px.image.isNull()) {
        if (why != nullptr) *why = tr("The page could not be drawn.");
        return false;
    }
    // The markers and glyphs of every family lifted out first, as extract()
    // does: the line under them is followed through.
    std::vector<int> hrun, vrun;
    runsOf(t, hrun, vrun);
    const QList<Marks> marks = allMarks(t, hrun, vrun);
    const QPointF pixel = t.px.toPixel(plot.work.map(seed));
    int bestX = -1, bestY = -1;
    double nearest = 1e18;
    const int radius = int(std::ceil(5 * t.px.scale));
    const int cx = int(pixel.x()), cy = int(pixel.y());
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (!t.inBounds(x, y)) continue;
            const int l = t.label[size_t(t.at(x, y))];
            if (l == 0 || t.families.at(l - 1).hidden) continue;
            const double d = std::hypot(x + 0.5 - pixel.x(), y + 0.5 - pixel.y());
            if (d < nearest) {
                nearest = d;
                bestX = x;
                bestY = y;
            }
        }
    if (bestX < 0) {
        if (why != nullptr) *why = options.colours.isEmpty() ? tr("There is no line within five points of there.") : tr("There is no line of the colours asked for within five points of there.");
        return false;
    }
    const int family = t.label[size_t(t.at(bestX, bestY))];
    // Thickness here: the narrower way across, the middle of what is nearby.
    QList<int> widths;
    const int around = int(std::ceil(3 * t.px.scale));
    for (int y = bestY - around; y <= bestY + around; y += 2)
        for (int x = bestX - around; x <= bestX + around; x += 2) {
            if (!t.inBounds(x, y) || t.label[size_t(t.at(x, y))] != family) continue;
            widths << std::min(hrun[size_t(t.at(x, y))], vrun[size_t(t.at(x, y))]);
        }
    std::sort(widths.begin(), widths.end());
    const double thickness = std::clamp(widths.isEmpty() ? marks.value(family - 1).thickness : double(widths.at(widths.size() * 3 / 4)), 1.0, 12 * t.px.scale);
    std::vector<int> stamps(size_t(t.px.w) * t.px.h, 0);
    int steps = 0;
    Series s;
    followFrom(t, family, thickness, QPointF(bestX + 0.5, bestY + 0.5), stamps, steps, &s);
    // The rest of its colour followed too, and what meets this line joined to
    // it: the other side of a narrow peak whose top it stopped at, the far
    // side of what covers it.
    if (s.points.size() >= 2) {
        QList<Series> pieces{s};
        int tracks = 0;
        for (int x = 0; x < t.px.w && tracks < 200; ++x)
            for (int y = 0; y < t.px.h && tracks < 200; ++y) {
                const size_t i = size_t(t.at(x, y));
                if (t.label[i] != family || stamps[i] != 0 || t.alpha[i] < 60) continue;
                Series other;
                if (!followFrom(t, family, thickness, QPointF(x + 0.5, y + 0.5), stamps, steps, &other)) continue;
                ++tracks;
                if (other.points.size() >= 2) pieces << other;
            }
        joinPieces(pieces, 0, 40.0, std::max(1.5, 3 * thickness / t.px.scale));
        // As extract() gives them: functions of x, the pieces of one curve
        // put together.
        if (plot.calibration.kind == Kind::Cartesian) {
            int together = 0, loops = 0;
            pieces = asFunctions(pieces, plot.work, thickness / t.px.scale, &together, &loops);
        }
        // The one with the seed in it.
        double nearestPiece = 1e18;
        for (const Series& piece : std::as_const(pieces))
            for (const QPointF& q : piece.points) {
                const double d = QLineF(q, seed).length();
                if (d < nearestPiece) {
                    nearestPiece = d;
                    s = piece;
                }
            }
        s.asked = t.families.at(family - 1).asked;
    }
    if (s.points.size() < 2) {
        if (why != nullptr) *why = tr("The line there could not be followed.");
        return false;
    }
    plot.renderScale = t.px.scale;
    plot.leftOut = t.leftOut;
    *out = s;
    return true;
}

void dropExcluded(Plot& plot, QStringList* notes)
{
    QList<QRectF> regions = plot.exclude;
    regions << plot.leftOut;
    if (regions.isEmpty()) return;
    int dropped = 0;
    QList<Series> kept;
    for (const Series& s : std::as_const(plot.series)) {
        Series out = s;
        out.points.clear();
        out.breaks.clear();
        bool gap = false;
        for (int i = 0; i < s.points.size(); ++i) {
            bool inside = false;
            for (const QRectF& r : std::as_const(regions)) inside = inside || r.contains(s.points.at(i));
            if (inside) {
                ++dropped;
                gap = true;
                continue;
            }
            if ((gap || s.breaks.contains(i)) && !out.points.isEmpty()) out.breaks << int(out.points.size());
            gap = false;
            out.points << s.points.at(i);
        }
        if (out.points.size() >= (s.shape == Series::Shape::Markers ? 1 : 2)) kept << out;
    }
    plot.series = kept;
    if (dropped > 0 && notes != nullptr) notes->append(tr("%1 points in the regions left out were taken out.").arg(dropped));
}

} // namespace qucs_s::digitizer
