/*
 * rf25_stackdrawing.cpp - an RF 2.5D model's stack drawn as a PDF of its
 *                         cross-section, as EMX's --print-process draws a
 *                         process
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * What is drawn is EMX's picture of a process (docs/feature_gaps/
 * 2026-11-09-rf25-stack-files.md, "The drawing"): the levels with their z
 * on the left, each layer's thickness and permittivity on the right, the
 * metals and vias as boxes in columns, and a key of them below. It is laid
 * out first (stackSheetOf), so a test can ask where each thing went, and
 * painted after.
 */
#include "rf25_stackfile.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QLocale>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25::StackDrawing", text);
}

QString g(double value, int digits = 6)
{
    return QString::number(value, 'g', digits);
}

QString heightWords(double metres)
{
    return QStringLiteral("h=%1 µm").arg(g(metres * 1e6));
}

QString epsWords(double epsr)
{
    return QStringLiteral("ε=%1").arg(g(epsr));
}

QString sigmaWords(double sigma)
{
    return QStringLiteral("σ=%1 S/m").arg(g(sigma, 4));
}

/// A resistance with the prefix that reads best: 60.6 mΩ, 2.5 kΩ.
QString ohms(double r)
{
    if (r > 0 && r < 1) return QStringLiteral("%1 mΩ").arg(g(r * 1e3, 3));
    if (r >= 1e3) return QStringLiteral("%1 kΩ").arg(g(r / 1e3, 3));
    return QStringLiteral("%1 Ω").arg(g(r, 3));
}

QFont fontOf(double points, bool bold)
{
    QFont f(QStringLiteral("Helvetica"));
    f.setStyleHint(QFont::SansSerif);
    f.setPointSizeF(points);
    f.setBold(bold);
    return f;
}

/// How wide \a text is in points: measured on a device of 72 dots an inch,
/// the PDF's, whatever the screen is.
double widthOf(const QString& text, double points, bool bold = false)
{
    static const QImage device = [] {
        QImage i(1, 1, QImage::Format_ARGB32);
        i.setDotsPerMeterX(2835);
        i.setDotsPerMeterY(2835);
        return i;
    }();
    double widest = 0;
    for (const QString& line : text.split(QLatin1Char('\n')))
        widest = std::max(widest, QFontMetricsF(fontOf(points, bold), &device).horizontalAdvance(line));
    return widest;
}

/// The size \a text is set at to fit \a width, from \a points down to
/// \a smallest.
double fitted(const QString& text, double points, double width, double smallest, bool bold = false)
{
    while (points > smallest && widthOf(text, points, bold) > width) points = std::max(smallest, points * 0.92);
    return points;
}

} // namespace

// ============================================================ What is drawn

StackDrawing stackDrawingOf(const Model& model, const QString& name)
{
    StackDrawing out;
    const QString label = model.root.label.trimmed();
    out.title = tr("The stack of %1").arg(!name.trimmed().isEmpty() ? name.trimmed()
                                          : !label.isEmpty()        ? label
                                                                    : tr("an RF 2.5D model"));
    const Stack stack = model.stack();
    if (!stack.ok()) {
        out.problems = stack.problems;
        return out;
    }
    const Layer* ground = nullptr;
    const Layer* cover = nullptr;
    for (const Layer& l : stack.layers) {
        if (l.kind == Layer::Ground) ground = &l;
        if (l.kind == Layer::Cover) cover = &l;
    }
    if (ground != nullptr && ground->variant != QLatin1String("open")) {
        const QString who = ground->name.isEmpty() ? tr("Ground") : ground->name;
        if (ground->variant == QLatin1String("metal"))
            out.ground = tr("%1: a metal plane, %2").arg(who, sigmaWords(ground->sigma));
        else if (ground->variant == QLatin1String("impedance"))
            out.ground = tr("%1: a plane of %2/sq").arg(who, ohms(ground->sigma));
        else
            out.ground = tr("%1: a perfect plane").arg(who);
    }

    // The layers between the levels: a metal's room is a layer of what
    // fills it, the dielectric above it, as the files written have it.
    for (int i = 0; i < stack.layers.size(); ++i) {
        const Layer& l = stack.layers.at(i);
        StackDrawing::Band b;
        b.name = l.name;
        b.z0 = l.z0;
        b.z1 = l.z1;
        switch (l.kind) {
        case Layer::Substrate:
            b.kind = StackDrawing::Band::Substrate;
            b.words << heightWords(l.thickness) << epsWords(l.epsr);
            if (l.tand > 0) b.words << QStringLiteral("tan δ=%1").arg(g(l.tand, 4));
            if (l.sigma > 0) b.words << sigmaWords(l.sigma) << QStringLiteral("ρ=%1 Ω-cm").arg(g(100 / l.sigma, 4));
            break;
        case Layer::Dielectric:
            b.kind = StackDrawing::Band::Dielectric;
            b.words << heightWords(l.thickness) << epsWords(l.epsr);
            if (l.tand > 0) b.words << QStringLiteral("tan δ=%1").arg(g(l.tand, 4));
            break;
        case Layer::Metal: {
            b.kind = StackDrawing::Band::Room;
            const Layer* fill = fillAbove(stack, i);
            b.words << heightWords(l.thickness)
                    << (fill != nullptr ? tr("%1 (%2)").arg(epsWords(fill->epsr), fill->name) : tr("%1 (air)").arg(epsWords(1)));
            if (fill != nullptr && fill->tand > 0) b.words << QStringLiteral("tan δ=%1").arg(g(fill->tand, 4));
            break;
        }
        case Layer::Cover:
            if (l.variant != QLatin1String("perfect") || !(l.thickness > 0)) continue;
            b.kind = StackDrawing::Band::Gap;
            b.words << heightWords(l.thickness) << epsWords(1);
            break;
        default:
            continue;
        }
        out.bands << b;
    }
    out.lid = cover != nullptr && cover->variant == QLatin1String("perfect");
    out.top = out.lid ? tr("%1: a perfect metal lid").arg(cover->name.isEmpty() ? tr("Lid") : cover->name)
                      : QStringLiteral("h=∞, ε=1");

    // The boxes, in columns: a via in a column with the metals it joins,
    // each via going where one of its metals is already drawn if it can
    // (so a run of vias stands in one column, as EMX draws it), a metal no
    // via joins in the first column with room for it.
    struct Item {
        QString key;
        QString name;
        bool via = false;
        double z0 = 0, z1 = 0;
    };
    QList<QList<Item>> columns;
    const auto has = [&columns](int c, const QString& key) {
        return std::any_of(columns.at(c).cbegin(), columns.at(c).cend(), [&key](const Item& it) { return it.key == key; });
    };
    const auto clear = [&columns](int c, double z0, double z1) {
        constexpr double eps = 1e-12;
        return std::none_of(columns.at(c).cbegin(), columns.at(c).cend(),
                            [&](const Item& it) { return z0 < it.z1 - eps && it.z0 < z1 - eps; });
    };
    const auto metalNamed = [&stack](const QString& n) -> const Layer* {
        for (const Layer& l : stack.layers)
            if (l.kind == Layer::Metal && l.name == n) return &l;
        return nullptr;
    };
    const auto metalItem = [](const Layer& l) { return Item{QStringLiteral("m:") + l.name, l.name, false, l.z0, l.z1}; };
    struct Joined {
        const Layer* via;
        const Layer* lower;   // null: the ground
        const Layer* upper;
        double z0, z1;
    };
    QList<Joined> joined;
    for (const Layer& v : stack.layers) {
        if (v.kind != Layer::Via) continue;
        const Layer* a = stack.isGround(v.from) ? nullptr : metalNamed(v.from);
        const Layer* b = stack.isGround(v.to) ? nullptr : metalNamed(v.to);
        if (a == nullptr && b == nullptr) continue;
        if (b == nullptr || (a != nullptr && a->z0 > b->z0)) std::swap(a, b);
        if (a == nullptr && ground != nullptr && ground->variant == QLatin1String("open")) continue;   // (no plane to end on)
        joined << Joined{&v, a, b, a != nullptr ? a->z1 : 0, b->z0};
    }
    std::stable_sort(joined.begin(), joined.end(), [](const Joined& x, const Joined& y) { return x.z0 < y.z0; });
    for (const Joined& j : std::as_const(joined)) {
        QList<Item> wanted;
        if (j.lower != nullptr) wanted << metalItem(*j.lower);
        wanted << Item{QStringLiteral("v:") + j.via->name, j.via->name, true, j.z0, j.z1};
        wanted << metalItem(*j.upper);
        const auto fits = [&](int c) {
            return std::all_of(wanted.cbegin(), wanted.cend(),
                               [&](const Item& it) { return (!it.via && has(c, it.key)) || clear(c, it.z0, it.z1); });
        };
        int chosen = -1;
        for (int c = 0; c < columns.size() && chosen < 0; ++c)
            if (fits(c) && j.lower != nullptr && has(c, wanted.first().key)) chosen = c;
        for (int c = 0; c < columns.size() && chosen < 0; ++c)
            if (fits(c) && has(c, wanted.last().key)) chosen = c;
        for (int c = 0; c < columns.size() && chosen < 0; ++c)
            if (fits(c)) chosen = c;
        if (chosen < 0) {
            columns.append(QList<Item>());
            chosen = int(columns.size()) - 1;
        }
        for (const Item& it : std::as_const(wanted))
            if (!has(chosen, it.key)) columns[chosen] << it;
    }
    for (const Layer& l : stack.layers) {
        if (l.kind != Layer::Metal) continue;
        const QString key = QStringLiteral("m:") + l.name;
        bool drawn = false;
        for (int c = 0; c < columns.size() && !drawn; ++c) drawn = has(c, key);
        if (drawn) continue;
        int chosen = -1;
        for (int c = 0; c < columns.size() && chosen < 0; ++c)
            if (clear(c, l.z0, l.z1)) chosen = c;
        if (chosen < 0) {
            columns.append(QList<Item>());
            chosen = int(columns.size()) - 1;
        }
        columns[chosen] << metalItem(l);
    }
    out.columns = std::max(1, int(columns.size()));
    for (int c = 0; c < columns.size(); ++c)
        for (const Item& it : columns.at(c)) out.boxes << StackDrawing::Box{it.name, it.via, it.z0, it.z1, c};

    // The key, from the top down, as the drawing reads.
    for (auto l = stack.layers.crbegin(); l != stack.layers.crend(); ++l) {
        if (l->kind == Layer::Metal) {
            QString line = tr("%1: t=%2 µm, %3, %4/sq").arg(l->name, g(l->thickness * 1e6), sigmaWords(l->sigma),
                                                           ohms(l->sigma > 0 && l->thickness > 0 ? 1 / (l->sigma * l->thickness) : 0));
            if (l->twoSheets) line += tr(", solved as its two faces");
            if (l->roughness > 0) line += tr(", roughness %1 µm rms").arg(g(l->roughness * 1e6, 4));
            out.metals << line;
        }
    }
    for (auto j = joined.crbegin(); j != joined.crend(); ++j) {
        const Node* node = model.find(j->via->tag);
        double fill = node != nullptr ? textOf(*node, QStringLiteral("fill")).toDouble() : 1;
        if (!(fill > 0) || fill > 1) fill = 1;
        QString line = tr("%1: %2 to %3, h=%4 µm, %5")
                           .arg(j->via->name, j->lower != nullptr ? j->lower->name : (ground->name.isEmpty() ? tr("the ground") : ground->name),
                                j->upper->name, g((j->z1 - j->z0) * 1e6), sigmaWords(j->via->sigma));
        if (fill < 1) line += tr(", %1% filled").arg(g(fill * 100, 3));
        out.vias << line;
    }
    return out;
}

// ============================================================ Laid out

double StackSheet::yOf(double z) const
{
    if (levels.isEmpty()) return 0;
    if (z <= levels.first()) return levelY.first();
    for (int i = 1; i < levels.size(); ++i)
        if (z <= levels.at(i)) {
            const double span = levels.at(i) - levels.at(i - 1);
            return span > 0 ? levelY.at(i - 1) + (z - levels.at(i - 1)) / span * (levelY.at(i) - levelY.at(i - 1)) : levelY.at(i);
        }
    return levelY.last();
}

StackSheet stackSheetOf(const StackDrawing& d, const QSizeF& size, const QString& footer)
{
    StackSheet s;
    s.size = size;
    const double W = size.width(), H = size.height(), m = 36;
    const auto text = [](StackSheet::Page& page, const QRectF& r, const QString& t, double points, int align, bool bold = false,
                         bool grey = false) { page.texts << StackSheet::Text{r, t, points, align, bold, grey}; };
    const int left = Qt::AlignLeft | Qt::AlignVCenter;
    StackSheet::Page first;
    text(first, QRectF(m, m, W - 2 * m, 16), d.title, fitted(d.title, 12, W - 2 * m, 7, true), left, true);
    const QString scale = tr("Lengths in µm, z from the ground plane up. Not to scale: each layer is drawn by the cube root of its thickness.");
    text(first, QRectF(m, m + 16, W - 2 * m, 11), scale, fitted(scale, 7, W - 2 * m, 4.5), left, false, true);

    // Across: the levels' z, the boxes' columns, what is said of each layer.
    const double zw = 46, lineX = m + zw + 4, right = W - m;
    const double rw = std::min(180.0, (W - 2 * m) * 0.34), labelX = right - rw;
    const double boxX0 = lineX + 10, boxX1 = labelX - 10;
    // Down: the title, the air or the lid, the layers, the ground, the key.
    const double topLine = m + 16 + 11 + 8 + 18;
    const double bottom = H - m - (footer.isEmpty() ? 0 : 12);
    const double groundH = 18;
    const qsizetype keyRows = std::max(d.metals.size(), d.vias.size()) + 1;
    const double keyH = double(keyRows) * 10.5 + 10;
    const int n = int(d.bands.size());
    double drawH = bottom - keyH - groundH - topLine;
    const bool keyApart = drawH < std::max(220.0, n * 7.0);
    if (keyApart) drawH = bottom - groundH - topLine;
    drawH = std::min(drawH, std::max(160.0, n * 90.0));
    // Each layer's height goes as the cube root of its thickness, so a thick
    // substrate does not leave the thin layers above it no room (300 µm some
    // eight times 0.5 µm), and none is thinner than a line of small print.
    const double thinnest = std::min(7.0, drawH / std::max(1, n));
    const auto heightOf = [&](const StackDrawing::Band& b, double k) {
        return std::max(thinnest, k * std::cbrt(std::max(0.0, (b.z1 - b.z0) * 1e6)));
    };
    const auto total = [&](double k) {
        double t = 0;
        for (const StackDrawing::Band& b : d.bands) t += heightOf(b, k);
        return t;
    };
    double k = 0;
    if (total(0) < drawH) {
        double lo = 0, hi = 1;
        for (int i = 0; i < 80 && total(hi) < drawH; ++i) hi *= 2;
        for (int i = 0; i < 60; ++i) {
            const double mid = (lo + hi) / 2;
            (total(mid) < drawH ? lo : hi) = mid;
        }
        k = lo;
    }
    double y = topLine + std::min(drawH, total(k));
    const double groundY = y;
    s.levels << (d.bands.isEmpty() ? 0 : d.bands.first().z0);
    s.levelY << y;
    for (const StackDrawing::Band& b : d.bands) {
        const double h = heightOf(b, k);
        s.bandRects << QRectF(lineX, y - h, right - lineX, h);
        y -= h;
        s.levels << b.z1;
        s.levelY << y;
    }
    const double topY = y;

    // The levels, and their z where there is room to say it.
    double said = 1e9;
    for (int i = 0; i < s.levels.size(); ++i) {
        const double ly = s.levelY.at(i);
        first.lines << StackSheet::Line{QLineF(lineX, ly, right, ly), i == 0 && d.ground.isEmpty()};
        if (i == 0 || said - ly >= 8.5) {
            text(first, QRectF(m, ly - 5, zw, 10), QString::number(s.levels.at(i) * 1e6, 'f', 3), 7, Qt::AlignRight | Qt::AlignVCenter);
            said = ly;
        }
    }
    // What each layer is, beside it: a line of each word where they fit,
    // else one line, as small as it must be.
    for (int i = 0; i < n; ++i) {
        const QRectF r = s.bandRects.at(i);
        const QStringList& words = d.bands.at(i).words;
        const QRectF where(labelX + 2, r.top(), rw - 2, r.height());
        if (r.height() >= words.size() * 9.5 + 1) {
            const QString lines = words.join(QLatin1Char('\n'));
            text(first, where, lines, fitted(lines, 8, rw - 4, 4.5), left);
        } else {
            const QString line = words.join(QStringLiteral(", "));
            text(first, where, line, fitted(line, std::clamp(r.height() * 0.78, 3.5, 8.0), rw - 4, 3.5), left);
        }
    }
    // The metals and vias.
    const double cw = std::min(110.0, (boxX1 - boxX0) / std::max(1, d.columns));
    for (const StackDrawing::Box& b : d.boxes) {
        const double x = boxX0 + b.column * cw;
        const double y1 = s.yOf(b.z1), y0 = s.yOf(b.z0);
        const QRectF r = b.via ? QRectF(x + cw * 0.2, y1, cw * 0.6, y0 - y1) : QRectF(x + cw * 0.06, y1, cw * 0.88, y0 - y1);
        first.shapes << StackSheet::Shape{b.via ? StackSheet::Shape::Via : StackSheet::Shape::Metal, r, b.name};
        if (r.height() >= 2.5)
            text(first, r, b.name, fitted(b.name, std::clamp(r.height() * 0.72, 3.5, 8.5), r.width() - 2, 3), Qt::AlignCenter);
    }
    // The ground under it all, and what closes it at the top.
    if (!d.ground.isEmpty()) {
        first.shapes << StackSheet::Shape{StackSheet::Shape::Ground, QRectF(lineX, groundY, right - lineX, 4), QString()};
        text(first, QRectF(labelX + 2, groundY + 4, rw - 2, 12), d.ground, fitted(d.ground, 7.5, rw - 4, 4.5), left);
    } else {
        const QString open = tr("No ground plane: open below");
        text(first, QRectF(labelX + 2, groundY + 1, rw - 2, 12), open, fitted(open, 7.5, rw - 4, 4.5), left, false, true);
    }
    if (d.lid) {
        first.shapes << StackSheet::Shape{StackSheet::Shape::Lid, QRectF(lineX, topY - 4, right - lineX, 4), QString()};
        text(first, QRectF(labelX + 2, topY - 17, rw - 2, 12), d.top, fitted(d.top, 7.5, rw - 4, 4.5), left);
    } else {
        text(first, QRectF(labelX + 2, topY - 13, rw - 2, 12), d.top, 8, Qt::AlignLeft | Qt::AlignBottom);
    }

    // The key: the metals on the left, the vias on the right.
    s.pages << first;
    double keyTop = groundY + groundH + 4;
    if (keyApart) {
        StackSheet::Page key;
        const QString heading = tr("%1: its metals and vias").arg(d.title);
        text(key, QRectF(m, m, W - 2 * m, 16), heading, fitted(heading, 12, W - 2 * m, 7, true), left, true);
        s.pages << key;
        keyTop = m + 26;
    }
    StackSheet::Page& keyPage = s.pages.last();
    const double colW = (W - 2 * m - 12) / 2;
    // (A column's lines all at the size its longest fits.)
    const auto column = [&](double x, const QString& heading, const QStringList& lines) {
        if (lines.isEmpty()) return;
        text(keyPage, QRectF(x, keyTop, colW, 11), heading, 8.5, left, true);
        double points = 8;
        for (const QString& line : lines) points = std::min(points, fitted(line, 8, colW, 4.5));
        for (int i = 0; i < lines.size(); ++i) text(keyPage, QRectF(x, keyTop + 12 + i * 10.5, colW, 10), lines.at(i), points, left);
    };
    column(m, tr("Metals"), d.metals);
    column(m + colW + 12, tr("Vias"), d.vias);
    for (int i = 0; i < s.pages.size() && !footer.isEmpty(); ++i) {
        const QString foot = s.pages.size() > 1 ? tr("%1 Page %2 of %3.").arg(footer).arg(i + 1).arg(s.pages.size()) : footer;
        text(s.pages[i], QRectF(m, H - m - 10, W - 2 * m, 10), foot, fitted(foot, 6.5, W - 2 * m, 4), left, false, true);
    }
    return s;
}

// ============================================================ Painted

void paintStackPage(QPainter& p, const StackSheet::Page& page)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const StackSheet::Line& l : page.lines) {
        QPen pen(Qt::black, 0.5);
        if (l.dashed) pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.drawLine(l.line);
    }
    for (const StackSheet::Shape& s : page.shapes) {
        const bool plane = s.kind == StackSheet::Shape::Ground || s.kind == StackSheet::Shape::Lid;
        p.setPen(plane ? Qt::NoPen : QPen(Qt::black, 0.5));
        p.setBrush(plane ? QColor(70, 70, 70) : s.kind == StackSheet::Shape::Via ? QColor(242, 242, 242) : QColor(222, 222, 222));
        p.drawRect(s.rect);
    }
    p.setBrush(Qt::NoBrush);
    for (const StackSheet::Text& t : page.texts) {
        p.setFont(fontOf(t.points, t.bold));
        p.setPen(t.grey ? QColor(90, 90, 90) : QColor(Qt::black));
        p.drawText(t.rect, t.align, t.text);
    }
    p.restore();
}

StackWritten stackPdfOf(const Model& model, const QString& name, const QString& madeBy)
{
    StackWritten out;
    const StackDrawing d = stackDrawingOf(model, name);
    if (!d.ok()) {
        out.problems = d.problems;
        return out;
    }
    const QPageSize page(QLocale().measurementSystem() == QLocale::ImperialUSSystem ? QPageSize::Letter : QPageSize::A4);
    const QString by = madeBy.isEmpty() ? QStringLiteral("Qucs-S") : madeBy;
    const StackSheet sheet = stackSheetOf(d, page.size(QPageSize::Point), tr("Drawn by %1.").arg(by));
    QBuffer buffer(&out.data);
    buffer.open(QIODevice::WriteOnly);
    {
        QPdfWriter writer(&buffer);
        writer.setPageSize(page);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setResolution(72);
        writer.setTitle(d.title);
        writer.setCreator(by);
        QPainter painter(&writer);
        for (int i = 0; i < sheet.pages.size(); ++i) {
            if (i > 0) writer.newPage();
            paintStackPage(painter, sheet.pages.at(i));
        }
        painter.end();
    }
    out.pages = int(sheet.pages.size());
    out.layers = int(d.bands.size());
    out.vias = int(d.vias.size());
    if (d.lid) out.notes << tr("The lid is drawn, though RF 2.5D does not solve a metal lid yet.");
    if (out.pages > 1)
        out.notes << tr("The key of the metals and vias is on a page of its own: the %1 layers fill the first.").arg(out.layers);
    return out;
}

} // namespace qucs_s::rf25
