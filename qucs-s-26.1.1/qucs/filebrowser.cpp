/*
 * filebrowser.cpp - the File Browser panel: the file system in the views a
 *                   file manager has, with icons for the files Qucs-S knows
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "filebrowser.h"

#include "apptheme.h"
#include "ink.h"
#include "main.h"
#include "settings.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QColumnView>
#include <QCompleter>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIconEngine>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QProcess>
#include <QRegularExpression>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using qucs_s::files::IconProvider;
using qucs_s::files::Kind;
using qucs_s::files::kindOf;

namespace {

// The settings the browser keeps.
const QString kLocation = QStringLiteral("FileBrowser/location");
const QString kView = QStringLiteral("FileBrowser/view");
const QString kHidden = QStringLiteral("FileBrowser/showHidden");
const QString kQucsOnly = QStringLiteral("FileBrowser/qucsFilesOnly");

QString trf(const char* text)
{
    return QCoreApplication::translate("FileBrowser", text);
}

bool darkTheme()
{
    return qucs_s::ink::isDark(QApplication::palette().color(QPalette::Base));
}

// ----------------------------------------------------------------------
// The kinds of files.

struct KindRow {
    const char* suffixes;   // separated by spaces
    const char* name;
    const char* tag;        // empty: the suffix
    QRgb colour;
    Kind::Glyph glyph;
    bool qucs;
};

const KindRow kKinds[] = {
    {"sch", QT_TRANSLATE_NOOP("FileBrowser", "Qucs-S schematic"), "SCH", 0x2f7fd0, Kind::Schematic, true},
    {"sym", QT_TRANSLATE_NOOP("FileBrowser", "Qucs-S symbol"), "SYM", 0x7a55c9, Kind::Symbol, true},
    {"dpl", QT_TRANSLATE_NOOP("FileBrowser", "Qucs-S data display"), "DPL", 0x2e9a5b, Kind::Plot, true},
    {"dat", QT_TRANSLATE_NOOP("FileBrowser", "Qucs-S dataset"), "DAT", 0xc98a14, Kind::Table, true},
    {"net cir ckt sp spi spc spice", QT_TRANSLATE_NOOP("FileBrowser", "SPICE netlist"), "", 0xd0652a, Kind::Code, true},
    {"lib mod inc sub", QT_TRANSLATE_NOOP("FileBrowser", "SPICE library"), "", 0xb5561f, Kind::Code, true},
    {"va vams", QT_TRANSLATE_NOOP("FileBrowser", "Verilog-A source"), "VA", 0xc83a3a, Kind::Code, true},
    {"osdi", QT_TRANSLATE_NOOP("FileBrowser", "Compiled Verilog-A model"), "OSDI", 0x8c2f2f, Kind::Page, true},
    {"v sv vh svh", QT_TRANSLATE_NOOP("FileBrowser", "Verilog source"), "", 0xb0406d, Kind::Code, true},
    {"vhd vhdl", QT_TRANSLATE_NOOP("FileBrowser", "VHDL source"), "VHD", 0x8a4fb0, Kind::Code, true},
    {"m oct", QT_TRANSLATE_NOOP("FileBrowser", "Octave script"), "M", 0xc57b20, Kind::Code, true},
    {"raw prn plot", QT_TRANSLATE_NOOP("FileBrowser", "Simulation output"), "", 0xb88917, Kind::Table, true},
    {"py", QT_TRANSLATE_NOOP("FileBrowser", "Python script"), "PY", 0x3572a5, Kind::Code, false},
    {"sh bash zsh bat cmd ps1", QT_TRANSLATE_NOOP("FileBrowser", "Shell script"), "", 0x4b8b3b, Kind::Code, false},
    {"c h cc cpp cxx hpp hh hxx java js ts rs go cs rb pl lua tcl jl r", QT_TRANSLATE_NOOP("FileBrowser", "Source code"),
     "", 0x5a6f85, Kind::Code, false},
    {"txt text log readme rst", QT_TRANSLATE_NOOP("FileBrowser", "Text"), "TXT", 0x7d8590, Kind::Text, false},
    {"md markdown", QT_TRANSLATE_NOOP("FileBrowser", "Markdown"), "MD", 0x4d5a66, Kind::Text, false},
    {"tex bib", QT_TRANSLATE_NOOP("FileBrowser", "TeX"), "TEX", 0x3d7a7a, Kind::Text, false},
    {"csv tsv", QT_TRANSLATE_NOOP("FileBrowser", "Table"), "", 0x1f8f4e, Kind::Table, false},
    {"json xml yaml yml toml ini cfg conf", QT_TRANSLATE_NOOP("FileBrowser", "Data"), "", 0x6b7f3a, Kind::Code, false},
    {"html htm", QT_TRANSLATE_NOOP("FileBrowser", "Web page"), "HTML", 0xe0762b, Kind::Code, false},
    {"pdf", QT_TRANSLATE_NOOP("FileBrowser", "PDF document"), "PDF", 0xd93a2b, Kind::Text, false},
    {"png jpg jpeg gif bmp svg tif tiff webp ico icns", QT_TRANSLATE_NOOP("FileBrowser", "Image"), "", 0xd45d9c,
     Kind::Image, false},
    {"zip tar gz tgz bz2 xz 7z rar", QT_TRANSLATE_NOOP("FileBrowser", "Archive"), "", 0x8a6d3b, Kind::Archive, false},
};

const KindRow* rowFor(const QString& suffix)
{
    static const QHash<QString, const KindRow*> bySuffix = [] {
        QHash<QString, const KindRow*> table;
        for (const KindRow& row : kKinds)
            for (const QString& s : QString::fromLatin1(row.suffixes).split(QLatin1Char(' ')))
                table.insert(s, &row);
        return table;
    }();
    return bySuffix.value(suffix, nullptr);
}

QString tagOf(const QString& suffix)
{
    return suffix.toUpper().left(4);
}

// ----------------------------------------------------------------------
// Drawing.

// An icon drawn at the size and pixel ratio asked for, kept in the pixmap
// cache under its key.
class DrawnIcon : public QIconEngine
{
public:
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal ratio = painter->device() != nullptr ? painter->device()->devicePixelRatio() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, ratio));
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        if (size.isEmpty()) return {};
        const QString key = cacheKey() + QStringLiteral("@%1x%2*%3/%4").arg(size.width()).arg(size.height()).arg(scale).arg(int(mode));
        QPixmap pixmap;
        if (QPixmapCache::find(key, &pixmap)) return pixmap;
        pixmap = QPixmap((QSizeF(size) * scale).toSize());
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        {
            QPainter p(&pixmap);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::TextAntialiasing);
            if (mode == QIcon::Disabled) p.setOpacity(0.4);
            draw(p, QRectF(QPointF(0, 0), QSizeF(size)));
        }
        QPixmapCache::insert(key, pixmap);
        return pixmap;
    }
    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }

protected:
    virtual void draw(QPainter& p, const QRectF& box) const = 0;
    /// What the drawing depends on (the theme's colours among it).
    virtual QString cacheKey() const = 0;
};

// What a file holds, drawn in \a art in \a colour.
void drawGlyph(QPainter& p, const QRectF& art, Kind::Glyph glyph, const QColor& colour, const QColor& muted, qreal line)
{
    p.save();
    QPen pen(colour, line, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const qreal l = art.left(), t = art.top(), w = art.width(), h = art.height();
    const qreal cy = t + h / 2;
    switch (glyph) {
    case Kind::Schematic: {
        // A resistor on a wire, a dot at each end.
        QPainterPath path(QPointF(l, cy));
        path.lineTo(l + w * 0.22, cy);
        const int teeth = 5;
        for (int i = 0; i < teeth; ++i) {
            const qreal x = l + w * (0.22 + 0.56 * (i + 0.5) / teeth);
            path.lineTo(x, cy + (i % 2 == 0 ? -h * 0.32 : h * 0.32));
        }
        path.lineTo(l + w * 0.78, cy);
        path.lineTo(l + w, cy);
        p.drawPath(path);
        p.setBrush(colour);
        const qreal r = line * 1.3;
        p.drawEllipse(QPointF(l, cy), r, r);
        p.drawEllipse(QPointF(l + w, cy), r, r);
        break;
    }
    case Kind::Symbol: {
        // An amplifier: a triangle and its leads.
        QPolygonF triangle;
        triangle << QPointF(l + w * 0.25, t) << QPointF(l + w * 0.25, t + h) << QPointF(l + w * 0.8, cy);
        p.drawPolygon(triangle);
        p.drawLine(QPointF(l, t + h * 0.28), QPointF(l + w * 0.25, t + h * 0.28));
        p.drawLine(QPointF(l, t + h * 0.72), QPointF(l + w * 0.25, t + h * 0.72));
        p.drawLine(QPointF(l + w * 0.8, cy), QPointF(l + w, cy));
        break;
    }
    case Kind::Plot: {
        // Axes and a curve.
        p.setPen(QPen(muted, line * 0.8));
        p.drawLine(QPointF(l, t), QPointF(l, t + h));
        p.drawLine(QPointF(l, t + h), QPointF(l + w, t + h));
        p.setPen(pen);
        QPainterPath curve;
        for (int i = 0; i <= 24; ++i) {
            const qreal x = l + w * 0.06 + w * 0.94 * i / 24.0;
            const qreal y = t + h * (0.55 - 0.4 * std::sin(i / 24.0 * 2 * 3.14159265358979) * std::exp(-i / 30.0));
            if (i == 0) curve.moveTo(x, y);
            else curve.lineTo(x, y);
        }
        p.drawPath(curve);
        break;
    }
    case Kind::Table: {
        // A table: its header row filled.
        const QRectF grid(l, t, w, h);
        p.setBrush(QColor(colour.red(), colour.green(), colour.blue(), 60));
        p.drawRect(QRectF(l, t, w, h / 3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(grid);
        p.drawLine(QPointF(l, t + h / 3), QPointF(l + w, t + h / 3));
        p.drawLine(QPointF(l, t + 2 * h / 3), QPointF(l + w, t + 2 * h / 3));
        p.drawLine(QPointF(l + w / 3, t), QPointF(l + w / 3, t + h));
        p.drawLine(QPointF(l + 2 * w / 3, t), QPointF(l + 2 * w / 3, t + h));
        break;
    }
    case Kind::Code: {
        // < / >
        p.drawPolyline(QPolygonF{QPointF(l + w * 0.3, t + h * 0.1), QPointF(l + w * 0.02, cy), QPointF(l + w * 0.3, t + h * 0.9)});
        p.drawPolyline(QPolygonF{QPointF(l + w * 0.7, t + h * 0.1), QPointF(l + w * 0.98, cy), QPointF(l + w * 0.7, t + h * 0.9)});
        p.drawLine(QPointF(l + w * 0.58, t), QPointF(l + w * 0.42, t + h));
        break;
    }
    case Kind::Image: {
        // A mountain and the sun.
        p.drawRoundedRect(QRectF(l, t, w, h), line, line);
        QPolygonF hills;
        hills << QPointF(l, t + h) << QPointF(l + w * 0.38, t + h * 0.35) << QPointF(l + w * 0.62, t + h * 0.7)
              << QPointF(l + w * 0.78, t + h * 0.5) << QPointF(l + w, t + h * 0.85) << QPointF(l + w, t + h);
        p.setBrush(QColor(colour.red(), colour.green(), colour.blue(), 110));
        p.drawPolygon(hills);
        p.setBrush(colour);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(l + w * 0.76, t + h * 0.26), h * 0.12, h * 0.12);
        break;
    }
    case Kind::Archive: {
        // A zip.
        const qreal x = l + w / 2;
        for (int i = 0; i < 5; ++i) {
            const qreal y = t + h * i / 5.0;
            p.drawLine(QPointF(i % 2 == 0 ? x - w * 0.14 : x, y + h * 0.05), QPointF(i % 2 == 0 ? x : x + w * 0.14, y + h * 0.05));
        }
        p.drawRoundedRect(QRectF(x - w * 0.12, t + h * 0.62, w * 0.24, h * 0.36), line, line);
        break;
    }
    case Kind::Text: {
        // Lines of text.
        p.setPen(QPen(muted, line, Qt::SolidLine, Qt::RoundCap));
        const qreal lengths[] = {1.0, 0.86, 1.0, 0.62};
        for (int i = 0; i < 4; ++i) {
            const qreal y = t + h * (0.1 + 0.27 * i);
            p.drawLine(QPointF(l, y), QPointF(l + w * lengths[i], y));
        }
        break;
    }
    case Kind::Page:
    case Kind::Folder:
    case Kind::Project:
        break;
    }
    p.restore();
}

void drawFolder(QPainter& p, const QRectF& box, bool project, bool dark)
{
    const qreal s = std::min(box.width(), box.height());
    const qreal w = s * 0.94, h = s * 0.76;
    const QRectF body(box.center().x() - w / 2, box.center().y() - h / 2 + s * 0.04, w, h);
    const qreal r = std::max(1.0, s * 0.07);
    const QColor back = dark ? QColor(0x3a, 0x74, 0xb3) : QColor(0x4f, 0x94, 0xdd);
    const QColor front = dark ? QColor(0x57, 0x92, 0xd1) : QColor(0x76, 0xb3, 0xef);
    p.setPen(Qt::NoPen);
    // The back with its tab, then the front, a little lower.
    QPainterPath backPath;
    backPath.addRoundedRect(QRectF(body.left(), body.top(), w * 0.44, h * 0.3), r, r);
    backPath.addRoundedRect(QRectF(body.left(), body.top() + h * 0.12, w, h * 0.88), r, r);
    p.setBrush(back);
    p.drawPath(backPath.simplified());
    p.setBrush(front);
    p.drawRoundedRect(QRectF(body.left(), body.top() + h * 0.27, w, h * 0.73), r, r);
    if (!project) return;
    // A project's: Qucs-S's badge, a resistor on it when there is room.
    const qreal br = s < 22 ? s * 0.2 : s * 0.22;
    const QPointF c(body.right() - br * 0.9, body.bottom() - br * 0.9);
    p.setBrush(QColor(0xe0, 0x7b, 0x39));
    p.setPen(QPen(dark ? QColor(0x22, 0x24, 0x27) : Qt::white, std::max(1.0, s / 32.0)));
    p.drawEllipse(c, br, br);
    if (s >= 22) {
        QPainterPath zig(QPointF(c.x() - br * 0.6, c.y()));
        for (int i = 0; i < 4; ++i) zig.lineTo(c.x() - br * 0.6 + br * 1.2 * (i + 0.5) / 4, c.y() + (i % 2 ? br * 0.35 : -br * 0.35));
        zig.lineTo(c.x() + br * 0.6, c.y());
        p.setPen(QPen(Qt::white, std::max(1.0, s / 40.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(zig);
    }
}

void drawFile(QPainter& p, const QRectF& box, const Kind& k, bool dark)
{
    const qreal s = std::min(box.width(), box.height());
    const bool small = s < 24;
    const qreal line = std::max(1.0, s / 28.0);
    const qreal h = s * 0.92 - line;
    const qreal w = h * 0.78;
    const QRectF page(box.center().x() - w / 2 + (small ? 0 : s * 0.04), box.center().y() - h / 2, w, h);
    const qreal fold = w * 0.32;
    const qreal r = std::max(0.8, s * 0.05);
    QPainterPath path;
    path.moveTo(page.left() + r, page.top());
    path.lineTo(page.right() - fold, page.top());
    path.lineTo(page.right(), page.top() + fold);
    path.lineTo(page.right(), page.bottom() - r);
    path.quadTo(page.right(), page.bottom(), page.right() - r, page.bottom());
    path.lineTo(page.left() + r, page.bottom());
    path.quadTo(page.left(), page.bottom(), page.left(), page.bottom() - r);
    path.lineTo(page.left(), page.top() + r);
    path.quadTo(page.left(), page.top(), page.left() + r, page.top());
    path.closeSubpath();
    const QColor paper = dark ? QColor(0x2e, 0x32, 0x39) : QColor(Qt::white);
    const QColor edge = dark ? QColor(0x6c, 0x73, 0x7e) : QColor(0xb4, 0xbb, 0xc4);
    const QColor muted = dark ? QColor(0x8a, 0x92, 0x9c) : QColor(0xa7, 0xae, 0xb7);
    const QColor colour = dark ? k.colour.lighter(118) : k.colour;
    p.setPen(QPen(edge, line));
    p.setBrush(paper);
    p.drawPath(path);
    // The fold.
    QPainterPath corner;
    corner.moveTo(page.right() - fold, page.top());
    corner.lineTo(page.right() - fold, page.top() + fold - r);
    corner.quadTo(page.right() - fold, page.top() + fold, page.right() - fold + r, page.top() + fold);
    corner.lineTo(page.right(), page.top() + fold);
    corner.closeSubpath();
    p.setBrush(dark ? QColor(0x47, 0x4d, 0x57) : QColor(0xe4, 0xe8, 0xed));
    p.drawPath(corner);

    if (small) {
        // Its colour, across the bottom of the page.
        p.save();
        p.setClipPath(path);
        p.fillRect(QRectF(page.left(), page.top() + h * 0.6, w, h * 0.4), colour);
        p.restore();
        p.setPen(QPen(edge, line));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        return;
    }
    // What it holds, then its tag on a band that stands out on the left.
    const QRectF art(page.left() + w * 0.18, page.top() + h * 0.16, w * 0.58, h * 0.3);
    drawGlyph(p, art, k.glyph, colour, muted, std::max(1.0, s / 26.0));
    if (k.tag.isEmpty()) return;
    QFont font = QApplication::font();
    font.setBold(true);
    int px = std::max(6, int(std::lround(s * 0.2)));
    font.setPixelSize(px);
    qreal tw = QFontMetricsF(font).horizontalAdvance(k.tag);
    const qreal most = w + s * 0.08;
    while (tw + s * 0.12 > most && px > 6) {
        font.setPixelSize(--px);
        tw = QFontMetricsF(font).horizontalAdvance(k.tag);
    }
    const qreal bh = s * 0.25;
    QRectF band(std::max(box.left(), page.left() - s * 0.08), page.bottom() - h * 0.36, std::min(most, tw + s * 0.14), bh);
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    p.drawRoundedRect(band, bh * 0.2, bh * 0.2);
    p.setPen(Qt::white);
    p.setFont(font);
    p.drawText(band, Qt::AlignCenter, k.tag);
}

class KindIcon : public DrawnIcon
{
public:
    explicit KindIcon(const Kind& kind) : a_kind(kind) {}
    QIconEngine* clone() const override { return new KindIcon(*this); }
    QString key() const override { return QStringLiteral("qucs-file-kind"); }

protected:
    void draw(QPainter& p, const QRectF& box) const override
    {
        if (a_kind.glyph == Kind::Folder || a_kind.glyph == Kind::Project)
            drawFolder(p, box, a_kind.glyph == Kind::Project, darkTheme());
        else
            drawFile(p, box, a_kind, darkTheme());
    }
    QString cacheKey() const override
    {
        return QStringLiteral("qfb-kind:%1:%2:%3:%4").arg(a_kind.tag, a_kind.colour.name()).arg(int(a_kind.glyph)).arg(darkTheme());
    }

private:
    Kind a_kind;
};

// The buttons' glyphs, in the colour of the text.
enum class Glyph { Back, Forward, Up, Home, Places, Tree, List, Icons, Details, Columns, Recent, More, Search };

class GlyphIcon : public DrawnIcon
{
public:
    explicit GlyphIcon(Glyph glyph) : a_glyph(glyph) {}
    QIconEngine* clone() const override { return new GlyphIcon(*this); }

protected:
    QColor ink() const { return QApplication::palette().color(QPalette::WindowText); }
    QString cacheKey() const override { return QStringLiteral("qfb-glyph:%1:%2").arg(int(a_glyph)).arg(ink().name()); }
    void draw(QPainter& p, const QRectF& box) const override
    {
        const qreal s = std::min(box.width(), box.height());
        const QRectF r(box.center().x() - s / 2, box.center().y() - s / 2, s, s);
        const qreal line = std::max(1.2, s / 10.0);
        const QColor c = ink();
        p.setPen(QPen(c, line, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const auto at = [&r](qreal x, qreal y) { return QPointF(r.left() + r.width() * x, r.top() + r.height() * y); };
        switch (a_glyph) {
        case Glyph::Back:
            p.drawPolyline(QPolygonF{at(0.62, 0.2), at(0.34, 0.5), at(0.62, 0.8)});
            break;
        case Glyph::Forward:
            p.drawPolyline(QPolygonF{at(0.38, 0.2), at(0.66, 0.5), at(0.38, 0.8)});
            break;
        case Glyph::Up:
            p.drawPolyline(QPolygonF{at(0.22, 0.46), at(0.5, 0.18), at(0.78, 0.46)});
            p.drawLine(at(0.5, 0.2), at(0.5, 0.84));
            break;
        case Glyph::Home:
            p.drawPolyline(QPolygonF{at(0.14, 0.5), at(0.5, 0.16), at(0.86, 0.5)});
            p.drawPolyline(QPolygonF{at(0.26, 0.42), at(0.26, 0.84), at(0.74, 0.84), at(0.74, 0.42)});
            p.drawPolyline(QPolygonF{at(0.43, 0.84), at(0.43, 0.6), at(0.57, 0.6), at(0.57, 0.84)});
            break;
        case Glyph::Places:
            // A bookmark.
            p.drawPolygon(QPolygonF{at(0.3, 0.16), at(0.7, 0.16), at(0.7, 0.86), at(0.5, 0.68), at(0.3, 0.86)});
            break;
        case Glyph::Tree:
            p.drawLine(at(0.24, 0.2), at(0.24, 0.8));
            p.drawLine(at(0.24, 0.5), at(0.46, 0.5));
            p.drawLine(at(0.24, 0.8), at(0.46, 0.8));
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            for (qreal y : {0.2, 0.5, 0.8}) p.drawRoundedRect(QRectF(at(y == 0.2 ? 0.14 : 0.52, y - 0.08), QSizeF(s * (y == 0.2 ? 0.2 : 0.34), s * 0.16)), 1, 1);
            break;
        case Glyph::List:
            for (qreal y : {0.24, 0.5, 0.76}) {
                p.drawPoint(at(0.18, y));
                p.drawLine(at(0.34, y), at(0.84, y));
            }
            break;
        case Glyph::Icons:
            for (qreal x : {0.16, 0.56})
                for (qreal y : {0.16, 0.56}) p.drawRoundedRect(QRectF(at(x, y), QSizeF(s * 0.28, s * 0.28)), 1.5, 1.5);
            break;
        case Glyph::Details:
            for (qreal y : {0.24, 0.5, 0.76}) {
                p.drawLine(at(0.12, y), at(0.5, y));
                p.drawLine(at(0.62, y), at(0.88, y));
            }
            break;
        case Glyph::Columns:
            p.drawRoundedRect(QRectF(at(0.12, 0.2), QSizeF(s * 0.76, s * 0.6)), 1.5, 1.5);
            p.drawLine(at(0.37, 0.2), at(0.37, 0.8));
            p.drawLine(at(0.63, 0.2), at(0.63, 0.8));
            break;
        case Glyph::Recent:
            p.drawEllipse(QRectF(at(0.16, 0.16), QSizeF(s * 0.68, s * 0.68)));
            p.drawPolyline(QPolygonF{at(0.5, 0.3), at(0.5, 0.5), at(0.64, 0.6)});
            break;
        case Glyph::More:
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            for (qreal x : {0.22, 0.5, 0.78}) p.drawEllipse(at(x, 0.5), s * 0.08, s * 0.08);
            break;
        case Glyph::Search:
            p.drawEllipse(QRectF(at(0.18, 0.18), QSizeF(s * 0.46, s * 0.46)));
            p.drawLine(at(0.56, 0.56), at(0.82, 0.82));
            break;
        }
    }

private:
    Glyph a_glyph;
};

QIcon glyphIcon(Glyph glyph)
{
    return QIcon(new GlyphIcon(glyph));
}

// The Recent view's rows: the icon, the name, and the folder under it.
class RecentDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        return QSize(160, option.fontMetrics.height() * 2 + 12);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem o = option;
        initStyleOption(&o, index);
        const QString name = o.text;
        const QIcon icon = o.icon;
        o.text.clear();
        o.icon = QIcon();
        QStyle* style = o.widget != nullptr ? o.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &o, p, o.widget);
        const QRect r = o.rect.adjusted(6, 5, -6, -5);
        const int side = r.height();
        icon.paint(p, QRect(r.left(), r.top(), side, side));
        const QRect text = r.adjusted(side + 8, 0, 0, 0);
        const bool selected = o.state & QStyle::State_Selected;
        const QColor ink = o.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        const QColor muted = selected ? ink : qucs_s::apptheme::mix(o.palette.color(QPalette::Base), ink, 0.6);
        p->save();
        p->setFont(o.font);
        p->setPen(ink);
        const QRect top(text.left(), text.top(), text.width(), text.height() / 2);
        p->drawText(top, Qt::AlignLeft | Qt::AlignVCenter, o.fontMetrics.elidedText(name, Qt::ElideRight, top.width()));
        QFont small = o.font;
        small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.88));
        p->setFont(small);
        p->setPen(muted);
        const QRect bottom(text.left(), top.bottom(), text.width(), text.height() - top.height());
        const QString where = index.data(Qt::UserRole + 1).toString();
        p->drawText(bottom, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(small).elidedText(where, Qt::ElideMiddle, bottom.width()));
        p->restore();
    }
};

// A folder as the user knows it: ~ for the home directory.
QString shownPath(const QString& path)
{
    const QString native = QDir::toNativeSeparators(path);
    const QString home = QDir::toNativeSeparators(QDir::homePath());
    if (native == home) return QStringLiteral("~");
    if (native.startsWith(home + QDir::separator())) return QStringLiteral("~") + native.mid(home.size());
    return native;
}

// The folder above \a path, or empty at the top.
QString parentOf(const QString& path)
{
    QDir dir(path);
    if (!dir.cdUp()) return {};
    return QDir::cleanPath(dir.absolutePath());
}

// The size of a file in words ("12 KB").
QString sizeText(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

// ----------------------------------------------------------------------
namespace qucs_s::files {

Kind kindOf(const QFileInfo& info)
{
    Kind kind;
    if (info.isDir()) {
        const bool project = info.fileName().endsWith(QLatin1String("_prj"));
        kind.name = project ? trf("Qucs-S project") : trf("Folder");
        kind.glyph = project ? Kind::Project : Kind::Folder;
        kind.colour = QColor(0x4f94dd);
        kind.tag = project ? QStringLiteral("PRJ") : QString();
        return kind;
    }
    QString suffix = info.suffix().toLower();
    // A simulator's own dataset: name.dat.ngspice, .dat.xyce, .dat.spopus.
    if ((suffix == QLatin1String("ngspice") || suffix == QLatin1String("xyce") || suffix == QLatin1String("spopus"))
        && info.completeSuffix().toLower().startsWith(QLatin1String("dat.")))
        suffix = QStringLiteral("dat");
    if (const KindRow* row = rowFor(suffix)) {
        kind.name = trf(row->name);
        kind.tag = row->tag[0] != '\0' ? QString::fromLatin1(row->tag) : tagOf(suffix);
        kind.colour = QColor(row->colour);
        kind.glyph = row->glyph;
        kind.qucs = row->qucs;
        return kind;
    }
    static const QRegularExpression touchstone(QStringLiteral("^s\\d+p$"));
    if (touchstone.match(suffix).hasMatch()) {
        kind.name = trf("Touchstone S-parameters");
        kind.tag = tagOf(suffix);
        kind.colour = QColor(0x1b9aaa);
        kind.glyph = Kind::Plot;
        kind.qucs = true;
        return kind;
    }
    kind.name = suffix.isEmpty() ? trf("File") : trf("%1 file").arg(suffix.toUpper());
    kind.tag = tagOf(suffix);
    kind.colour = QColor(0x8b949e);
    kind.glyph = Kind::Page;
    return kind;
}

QIcon IconProvider::iconFor(const Kind& kind)
{
    return QIcon(new KindIcon(kind));
}

QIcon IconProvider::icon(IconType type) const
{
    Kind kind;
    kind.colour = QColor(0x8b949e);
    kind.glyph = type == File ? Kind::Page : Kind::Folder;
    return iconFor(kind);
}

QIcon IconProvider::icon(const QFileInfo& info) const
{
    return iconFor(kindOf(info));
}

QString IconProvider::type(const QFileInfo& info) const
{
    return kindOf(info).name;
}

// Names in natural order, case aside: runs of digits by their value (R2
// before R10), the rest by the collator. QCollator's numeric mode does it
// only with ICU, which not every Qt has (not the Linux one of the CI).
static int naturalCompare(const QCollator& collator, QStringView a, QStringView b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digits = a[i].isDigit();
        if (digits != b[j].isDigit()) break;   // a number against a word: as the collator has them
        qsizetype i2 = i, j2 = j;
        while (i2 < a.size() && a[i2].isDigit() == digits) ++i2;
        while (j2 < b.size() && b[j2].isDigit() == digits) ++j2;
        QStringView x = a.sliced(i, i2 - i), y = b.sliced(j, j2 - j);
        int c = 0;
        if (digits) {
            while (x.size() > 1 && x.front() == QLatin1Char('0')) x = x.sliced(1);
            while (y.size() > 1 && y.front() == QLatin1Char('0')) y = y.sliced(1);
            c = x.size() != y.size() ? (x.size() < y.size() ? -1 : 1) : x.compare(y);
        } else {
            c = collator.compare(x, y);
        }
        if (c != 0) return c;
        i = i2;
        j = j2;
    }
    if (i < a.size() && j < b.size()) {
        const int c = collator.compare(a.sliced(i), b.sliced(j));
        if (c != 0) return c;
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j ? -1 : 1;
    return collator.compare(a, b);
}

// The file system as the browser shows it: folders first, then names in
// natural order (or by the column sorted on); files filtered by the name
// typed and, if asked, to those of Qucs-S; in the flat views folders by
// the name too - never those on the way to the folder shown.
class SortProxy : public QSortFilterProxyModel
{
public:
    explicit SortProxy(QObject* parent) : QSortFilterProxyModel(parent)
    {
        a_collator.setCaseSensitivity(Qt::CaseInsensitive);
    }
    void configure(const QString& text, bool qucsOnly, bool flat, const QString& location)
    {
        if (text == a_text && qucsOnly == a_qucsOnly && flat == a_flat && location == a_location) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
#endif
        a_text = text;
        a_qucsOnly = qucsOnly;
        a_flat = flat;
        a_location = location;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        invalidateFilter();
#endif
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        const auto* fs = static_cast<QFileSystemModel*>(sourceModel());
        const QModelIndex index = fs->index(row, 0, parent);
        const QString name = fs->fileName(index);
        if (fs->isDir(index)) {
            const QString path = fs->filePath(index);
            // On the way to the folder shown, or it: always.
            if (a_location == path || a_location.startsWith(path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/')))
                return true;
            return !a_flat || a_text.isEmpty() || name.contains(a_text, Qt::CaseInsensitive);
        }
        if (a_qucsOnly && !kindOf(fs->fileInfo(index)).qucs) return false;
        return a_text.isEmpty() || name.contains(a_text, Qt::CaseInsensitive);
    }
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
    {
        const auto* fs = static_cast<QFileSystemModel*>(sourceModel());
        const bool leftDir = fs->isDir(left), rightDir = fs->isDir(right);
        if (leftDir != rightDir) return sortOrder() == Qt::AscendingOrder ? leftDir : rightDir;
        const auto byName = [&] { return naturalCompare(a_collator, fs->fileName(left), fs->fileName(right)) < 0; };
        switch (left.column()) {
        case 1:
            if (fs->size(left) != fs->size(right)) return fs->size(left) < fs->size(right);
            return byName();
        case 2: {
            const int c = a_collator.compare(fs->type(left), fs->type(right));
            return c != 0 ? c < 0 : byName();
        }
        case 3:
            if (fs->lastModified(left) != fs->lastModified(right)) return fs->lastModified(left) < fs->lastModified(right);
            return byName();
        default:
            return byName();
        }
    }

private:
    QCollator a_collator;
    QString a_text;
    bool a_qucsOnly = false;
    bool a_flat = true;
    QString a_location;
};

} // namespace qucs_s::files

// ----------------------------------------------------------------------
namespace {

// "Show in Finder" for an entry (\a item), "Open in Finder" for a folder.
QString revealLabel(bool item)
{
#if defined(Q_OS_MACOS)
    return item ? trf("Show in Finder") : trf("Open in Finder");
#elif defined(Q_OS_WIN)
    return item ? trf("Show in Explorer") : trf("Open in Explorer");
#else
    return item ? trf("Show in the File Manager") : trf("Open in the File Manager");
#endif
}

// The system's file manager, with \a path selected where it can.
void revealItem(const QString& path)
{
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"), {QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
#else
    const QFileInfo info(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath()));
#endif
}

QToolButton* toolButton(QWidget* parent, const QIcon& icon, const QString& tip, const char* name)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setIconSize(QSize(16, 16));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setObjectName(QLatin1String(name));
    b->setProperty("fbTool", true);   // (styled by the browser, not by the window)
    return b;
}

bool isFlat(FileBrowser::View view)
{
    return view == FileBrowser::View::List || view == FileBrowser::View::Icons || view == FileBrowser::View::Details;
}

// The keys a list handles itself - not the window's shortcuts for them
// (the arrows move a diagram's marker, Backspace deletes on macOS).
bool listKey(const QKeyEvent* key)
{
    switch (key->key()) {
    case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
    case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Backspace: case Qt::Key_F2: case Qt::Key_Space:
        return (key->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier)) == 0;
    default:
        // Typing a name finds it.
        return (key->modifiers() & ~Qt::ShiftModifier) == 0 && !key->text().isEmpty() && key->text().at(0).isPrint();
    }
}

/*!
 * The Columns view. Qt's shows a folder with nothing in it (yet) as its
 * preview column alone, which has no selection model - and when its cursor
 * moves, or a scroll's animation ends, it takes the selection model of its
 * first column: a current entry then (one left from the folder before, or
 * a key's: Right with none went to the top of the file system) crashed it.
 * Here the cursor stays in the folder shown, and there is none while the
 * folder has no column of its own.
 */
class ColumnView : public QColumnView
{
public:
    using QColumnView::QColumnView;

    /// Whether the folder shown has a column (not the preview alone).
    bool hasColumn() const
    {
        const auto columns = viewport()->findChildren<QAbstractItemView*>(Qt::FindDirectChildrenOnly);
        return std::any_of(columns.cbegin(), columns.cend(), [this](const QAbstractItemView* c) {
            return !c->isHidden() && c->model() != nullptr && c->rootIndex() == rootIndex();
        });
    }

    void setRootIndex(const QModelIndex& index) override
    {
        QColumnView::setRootIndex(index);
        if (!hasColumn() && currentIndex().isValid()) setCurrentIndex(QModelIndex());
    }

protected:
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override
    {
        if (!hasColumn()) return {};
        const QModelIndex current = currentIndex();
        if (!within(current)) return model()->index(0, 0, rootIndex());
        const QModelIndex to = QColumnView::moveCursor(action, modifiers);
        return within(to) ? to : current;
    }

private:
    // Whether \a index is an entry of the folder shown, or of one in it.
    bool within(const QModelIndex& index) const
    {
        if (!index.isValid()) return false;
        for (QModelIndex p = index.parent(); p.isValid(); p = p.parent())
            if (p == rootIndex()) return true;
        return !rootIndex().isValid();
    }
};

} // namespace

// ----------------------------------------------------------------------
FileBrowser::FileBrowser(QWidget* parent)
    : QWidget(parent),
      a_icons(new IconProvider),
      a_model(new QFileSystemModel(this)),
      a_proxy(new qucs_s::files::SortProxy(this)),
      a_recentModel(new QStandardItemModel(this)),
      a_statusTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("fileBrowser"));
    a_model->setIconProvider(a_icons);
    a_model->setReadOnly(false);   // (renamed in place)
    a_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    a_proxy->setSourceModel(a_model);
    a_proxy->setDynamicSortFilter(true);
    a_proxy->sort(0, Qt::AscendingOrder);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(3);
    buildToolbar();
    buildViews();

    const auto shortcut = [this](const QKeySequence& keys, void (FileBrowser::*slot)()) {
        auto* s = new QShortcut(keys, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence::Back, &FileBrowser::back);
    shortcut(QKeySequence::Forward, &FileBrowser::forward);
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Up), &FileBrowser::up);
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), &FileBrowser::editPath);

    // What was shown last time.
    QucsSettingsFile settings;
    setShowHidden(settings.value(kHidden, false).toBool());
    setQucsFilesOnly(settings.value(kQucsOnly, false).toBool());
    const int view = settings.value(kView, int(View::List)).toInt();
    setView(view >= int(View::Tree) && view <= int(View::Recent) ? View(view) : View::List);
    const QString where = settings.value(kLocation).toString();
    if (QFileInfo(where).isDir()) go(where, false);
    a_loaded = true;
    restyle();
}

FileBrowser::~FileBrowser()
{
    // The model asks its provider for icons (in its own thread) until it
    // is gone: the views let go of it, it goes, then the provider.
    a_proxy->setSourceModel(nullptr);
    delete a_model;
    delete a_icons;
}

void FileBrowser::buildToolbar()
{
    auto* layout = static_cast<QVBoxLayout*>(this->layout());
    auto* top = new QHBoxLayout;
    top->setContentsMargins(4, 0, 4, 0);
    top->setSpacing(1);
    a_backButton = toolButton(this, glyphIcon(Glyph::Back), tr("Back"), "fbBack");
    a_forwardButton = toolButton(this, glyphIcon(Glyph::Forward), tr("Forward"), "fbForward");
    a_upButton = toolButton(this, glyphIcon(Glyph::Up), tr("The folder above"), "fbUp");
    a_homeButton = toolButton(this, glyphIcon(Glyph::Home), tr("The workspace folder"), "fbHome");
    a_placesButton = toolButton(this, glyphIcon(Glyph::Places),
                                tr("Places: the workspace, the project, the examples, home, the volumes"), "fbPlacesButton");
    a_placesButton->setPopupMode(QToolButton::InstantPopup);
    auto* places = new QMenu(a_placesButton);
    places->setObjectName(QStringLiteral("fbPlaces"));
    a_placesButton->setMenu(places);
    connect(places, &QMenu::aboutToShow, this, [this, places] { fillPlaces(places); });
    connect(a_backButton, &QToolButton::clicked, this, &FileBrowser::back);
    connect(a_forwardButton, &QToolButton::clicked, this, &FileBrowser::forward);
    connect(a_upButton, &QToolButton::clicked, this, &FileBrowser::up);
    connect(a_homeButton, &QToolButton::clicked, this, &FileBrowser::home);

    // The views, in a menu (the panel is narrow).
    a_viewButton = toolButton(this, glyphIcon(Glyph::List), tr("How the files are shown"), "fbViewButton");
    a_viewButton->setPopupMode(QToolButton::InstantPopup);
    a_viewMenu = new QMenu(a_viewButton);
    a_viewMenu->setObjectName(QStringLiteral("fbViews"));
    a_viewMenu->setToolTipsVisible(true);
    a_viewActions = new QActionGroup(this);
    const struct {
        View view;
        Glyph glyph;
        const char* label;
        const char* tip;
    } views[] = {
        {View::Tree, Glyph::Tree, QT_TR_NOOP("Tree"), QT_TR_NOOP("Folders that open in place")},
        {View::List, Glyph::List, QT_TR_NOOP("List"), QT_TR_NOOP("The folder's entries; a double-click enters a folder")},
        {View::Icons, Glyph::Icons, QT_TR_NOOP("Icons"), QT_TR_NOOP("The folder's entries as large icons")},
        {View::Details, Glyph::Details, QT_TR_NOOP("Details"), QT_TR_NOOP("With size, kind and date - a click on a heading sorts")},
        {View::Columns, Glyph::Columns, QT_TR_NOOP("Columns"), QT_TR_NOOP("A column for each folder on the way")},
        {View::Recent, Glyph::Recent, QT_TR_NOOP("Recent Documents"), QT_TR_NOOP("The documents opened last")},
    };
    for (const auto& v : views) {
        if (v.view == View::Recent) a_viewMenu->addSeparator();
        QAction* a = a_viewMenu->addAction(glyphIcon(v.glyph), tr(v.label));
        a->setCheckable(true);
        a->setData(int(v.view));
        a->setToolTip(tr(v.tip));
        a_viewActions->addAction(a);
    }
    a_viewButton->setMenu(a_viewMenu);
    connect(a_viewActions, &QActionGroup::triggered, this, [this](QAction* a) { setView(View(a->data().toInt())); });

    a_menuButton = toolButton(this, glyphIcon(Glyph::More), tr("Options"), "fbMenuButton");
    a_menuButton->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(a_menuButton);
    menu->setObjectName(QStringLiteral("fbOptions"));
    menu->setToolTipsVisible(true);
    a_hiddenAction = menu->addAction(tr("Show Hidden Files"));
    a_hiddenAction->setCheckable(true);
    connect(a_hiddenAction, &QAction::toggled, this, &FileBrowser::setShowHidden);
    a_qucsAction = menu->addAction(tr("Show Only Qucs-S Files"));
    a_qucsAction->setCheckable(true);
    a_qucsAction->setToolTip(tr("Schematics, symbols, data displays, datasets, netlists and libraries, HDL and "
                                "Verilog-A sources, S-parameters, Octave scripts - and the folders"));
    connect(a_qucsAction, &QAction::toggled, this, &FileBrowser::setQucsFilesOnly);
    menu->addSeparator();
    a_documentAction = menu->addAction(tr("Show the Document in Front"), this, [this] {
        const QString doc = a_document ? a_document() : QString();
        if (!QFileInfo(doc).isFile()) return;
        if (a_view == View::Recent) setView(a_fileView);
        go(doc, true);
    });
    menu->addAction(tr("New Folder…"), this, [this] {
        const QString made = createFolder(a_location);
        if (!made.isEmpty()) rename(made);
    });
    menu->addSeparator();
    menu->addAction(revealLabel(false), this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(a_location)); });
    menu->addAction(tr("Copy the Folder's Path"), this, [this] {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(a_location));
    });
    connect(menu, &QMenu::aboutToShow, this, [this] {
        a_documentAction->setEnabled(a_document && QFileInfo(a_document()).isFile());
    });
    a_menuButton->setMenu(menu);

    top->addWidget(a_backButton);
    top->addWidget(a_forwardButton);
    top->addWidget(a_upButton);
    top->addWidget(a_homeButton);
    top->addWidget(a_placesButton);
    top->addStretch(1);
    top->addWidget(a_viewButton);
    top->addWidget(a_menuButton);
    layout->addLayout(top);

    // The path: a button for each folder on the way, or a line to type one.
    a_pathStack = new QStackedWidget(this);
    a_pathStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    a_crumbBar = new QWidget(a_pathStack);
    a_crumbBar->setObjectName(QStringLiteral("fbCrumbs"));
    a_crumbBar->setToolTip(tr("A click beside the folders (or Ctrl+Shift+G) types a path"));
    a_crumbLayout = new QHBoxLayout(a_crumbBar);
    a_crumbLayout->setContentsMargins(4, 0, 4, 0);
    a_crumbLayout->setSpacing(0);
    a_crumbMore = new QToolButton(a_crumbBar);
    a_crumbMore->setObjectName(QStringLiteral("fbCrumb"));
    a_crumbMore->setText(QStringLiteral("…"));
    a_crumbMore->setToolTip(tr("The folders above"));
    a_crumbMore->setAutoRaise(true);
    a_crumbMore->setPopupMode(QToolButton::InstantPopup);
    a_crumbMore->setMenu(new QMenu(a_crumbMore));
    a_crumbMore->hide();
    a_crumbLayout->addWidget(a_crumbMore);
    a_crumbLayout->addStretch(1);
    a_crumbBar->installEventFilter(this);
    a_pathEdit = new QLineEdit(a_pathStack);
    a_pathEdit->setObjectName(QStringLiteral("fbPathEdit"));
    a_pathEdit->setPlaceholderText(tr("A folder's path"));
    a_pathEdit->setAttribute(Qt::WA_MacShowFocusRect, false);
    a_pathEdit->installEventFilter(this);
    auto* completer = new QCompleter(a_pathEdit);
    auto* folders = new QFileSystemModel(completer);
    folders->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
    folders->setRootPath(QString());
    completer->setModel(folders);
    a_pathEdit->setCompleter(completer);
    connect(a_pathEdit, &QLineEdit::returnPressed, this, [this] {
        QString typed = QDir::fromNativeSeparators(a_pathEdit->text().trimmed());
        if (typed == QLatin1String("~") || typed.startsWith(QLatin1String("~/"))) typed = QDir::homePath() + typed.mid(1);
        const QFileInfo info(typed);
        if (!info.exists()) {
            a_pathEdit->selectAll();
            a_status->setText(tr("There is no %1.").arg(QDir::toNativeSeparators(typed)));
            return;
        }
        a_pathStack->setCurrentWidget(a_crumbBar);
        if (a_view == View::Recent) setView(a_fileView);
        go(info.absoluteFilePath(), true);
        if (QAbstractItemView* v = currentView()) v->setFocus();
    });
    a_pathStack->addWidget(a_crumbBar);
    a_pathStack->addWidget(a_pathEdit);
    a_pathStack->setFixedHeight(std::max(a_pathEdit->sizeHint().height(), a_crumbMore->sizeHint().height() + 2));
    layout->addWidget(a_pathStack);

    // The filter.
    a_filter = new QLineEdit(this);
    a_filter->setObjectName(QStringLiteral("fbFilter"));
    a_filter->setPlaceholderText(tr("Filter by name"));
    a_filter->setClearButtonEnabled(true);
    a_filter->addAction(glyphIcon(Glyph::Search), QLineEdit::LeadingPosition);
    a_filter->setAttribute(Qt::WA_MacShowFocusRect, false);
    connect(a_filter, &QLineEdit::textChanged, this, [this] { refilter(); });
    auto* filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(4, 0, 4, 0);
    filterRow->addWidget(a_filter);
    layout->addLayout(filterRow);
}

void FileBrowser::buildViews()
{
    auto* layout = static_cast<QVBoxLayout*>(this->layout());
    a_stack = new QStackedWidget(this);
    const auto common = [this](QAbstractItemView* v, const char* name) {
        v->setObjectName(QLatin1String(name));
        v->setFrameShape(QFrame::NoFrame);
        v->setContextMenuPolicy(Qt::CustomContextMenu);
        v->setDragEnabled(true);
        v->setDragDropMode(QAbstractItemView::DragOnly);
        v->setDefaultDropAction(Qt::CopyAction);
        v->setEditTriggers(QAbstractItemView::EditKeyPressed);
        v->setSelectionMode(QAbstractItemView::SingleSelection);
        v->setAttribute(Qt::WA_MacShowFocusRect, false);
        v->installEventFilter(this);
        connect(v, &QAbstractItemView::activated, this, &FileBrowser::onActivated);
        connect(v, &QWidget::customContextMenuRequested, this, [this, v](const QPoint& pos) { showContextMenu(v, pos); });
        a_stack->addWidget(v);
    };

    a_tree = new QTreeView;
    a_tree->setModel(a_proxy);
    for (int c = 1; c < a_proxy->columnCount(); ++c) a_tree->hideColumn(c);
    a_tree->setHeaderHidden(true);
    a_tree->setIconSize(QSize(18, 18));
    a_tree->setIndentation(14);
    a_tree->setAnimated(true);
    a_tree->setUniformRowHeights(true);
    a_tree->setExpandsOnDoubleClick(false);   // (activate() opens and folds)
    common(a_tree, "fbTree");

    a_list = new QListView;
    a_list->setModel(a_proxy);
    a_list->setIconSize(QSize(18, 18));
    a_list->setUniformItemSizes(true);
    common(a_list, "fbList");

    a_grid = new QListView;
    a_grid->setModel(a_proxy);
    a_grid->setViewMode(QListView::IconMode);
    a_grid->setIconSize(QSize(48, 48));
    a_grid->setGridSize(QSize(96, 90));
    a_grid->setSpacing(4);
    a_grid->setWordWrap(true);
    a_grid->setTextElideMode(Qt::ElideMiddle);
    a_grid->setResizeMode(QListView::Adjust);
    a_grid->setMovement(QListView::Static);   // (the grid sizes the cells; the first item must not)
    common(a_grid, "fbIcons");

    a_details = new QTreeView;
    a_details->setModel(a_proxy);
    a_details->setRootIsDecorated(false);
    a_details->setItemsExpandable(false);
    a_details->setUniformRowHeights(true);
    a_details->setIconSize(QSize(18, 18));
    a_details->setSortingEnabled(true);
    a_details->sortByColumn(0, Qt::AscendingOrder);
    a_details->header()->setStretchLastSection(false);
    a_details->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    for (int c = 1; c < a_proxy->columnCount(); ++c) a_details->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    a_details->viewport()->installEventFilter(this);
    connect(a_details->header(), &QHeaderView::sectionResized, this, [this](int section) {
        if (section != 0) fitDetails();
    });
    common(a_details, "fbDetails");

    a_columns = new ColumnView;
    a_columns->setModel(a_proxy);
    a_columns->setIconSize(QSize(16, 16));
    a_columns->setColumnWidths(QList<int>(24, 150));
    // A file selected: what it is, as Finder shows it.
    a_preview = new QWidget;
    a_preview->setObjectName(QStringLiteral("fbPreview"));
    auto* preview = new QVBoxLayout(a_preview);
    preview->setContentsMargins(10, 14, 10, 10);
    preview->setSpacing(6);
    a_previewIcon = new QLabel(a_preview);
    a_previewIcon->setAlignment(Qt::AlignHCenter);
    a_previewName = new QLabel(a_preview);
    a_previewName->setObjectName(QStringLiteral("fbPreviewName"));
    a_previewName->setAlignment(Qt::AlignHCenter);
    a_previewName->setWordWrap(true);
    a_previewName->setTextFormat(Qt::PlainText);
    a_previewFacts = new QLabel(a_preview);
    a_previewFacts->setObjectName(QStringLiteral("fbPreviewFacts"));
    a_previewFacts->setAlignment(Qt::AlignHCenter);
    a_previewFacts->setWordWrap(true);
    a_previewFacts->setTextFormat(Qt::PlainText);
    preview->addWidget(a_previewIcon);
    preview->addWidget(a_previewName);
    preview->addWidget(a_previewFacts);
    preview->addStretch(1);
    a_preview->setMinimumWidth(140);
    a_columns->setPreviewWidget(a_preview);   // (the view owns it)
    connect(a_columns, &QColumnView::updatePreviewWidget, this, &FileBrowser::fillPreview);
    common(a_columns, "fbColumns");

    a_recent = new QListView;
    a_recent->setModel(a_recentModel);
    a_recent->setItemDelegate(new RecentDelegate(a_recent));
    a_recent->setIconSize(QSize(30, 30));
    a_recent->setUniformItemSizes(true);
    common(a_recent, "fbRecent");
    a_recent->setDragEnabled(false);
    a_recent->setEditTriggers(QAbstractItemView::NoEditTriggers);

    for (QAbstractItemView* v : {static_cast<QAbstractItemView*>(a_tree), static_cast<QAbstractItemView*>(a_list),
                                 static_cast<QAbstractItemView*>(a_grid), static_cast<QAbstractItemView*>(a_details),
                                 static_cast<QAbstractItemView*>(a_columns), static_cast<QAbstractItemView*>(a_recent)})
        connect(v->selectionModel(), &QItemSelectionModel::currentChanged, this, [this, v](const QModelIndex& now) {
            if (v != currentView()) return;
            a_selected = pathOf(now);
            a_statusTimer->start();
        });
    layout->addWidget(a_stack, 1);

    a_status = new QLabel(this);
    a_status->setObjectName(QStringLiteral("fbStatus"));
    a_status->setTextFormat(Qt::PlainText);
    a_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QFont small = a_status->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.9));
    a_status->setFont(small);
    layout->addWidget(a_status);

    a_statusTimer->setSingleShot(true);
    a_statusTimer->setInterval(60);
    connect(a_statusTimer, &QTimer::timeout, this, &FileBrowser::updateStatus);
    const auto later = [this] { a_statusTimer->start(); };
    connect(a_proxy, &QAbstractItemModel::rowsInserted, this, later);
    connect(a_proxy, &QAbstractItemModel::rowsRemoved, this, later);
    connect(a_proxy, &QAbstractItemModel::layoutChanged, this, later);
    connect(a_proxy, &QAbstractItemModel::modelReset, this, later);
    connect(a_model, &QFileSystemModel::directoryLoaded, this, later);
    connect(a_model, &QFileSystemModel::directoryLoaded, this, [this](const QString& path) {
        if (!a_columnsPending || QDir::cleanPath(path) != a_location) return;
        a_columnsPending = false;
        a_columns->setRootIndex(a_proxy->mapFromSource(a_model->index(a_location)));
        if (a_view == View::Columns) selectPath(a_selected);
    });
}

// ----------------------------------------------------------------------
// Where it is.

void FileBrowser::setLocation(const QString& path)
{
    if (a_view == View::Recent) setView(a_fileView);
    go(path, true);
}

void FileBrowser::go(const QString& path, bool remember)
{
    const QFileInfo info(path);
    const QString folder = QDir::cleanPath(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
    if (!QFileInfo(folder).isDir()) return;
    if (folder != a_location) {
        if (remember && !a_location.isEmpty()) {
            a_back << a_location;
            if (a_back.size() > 100) a_back.removeFirst();
            a_forward.clear();
        }
        a_location = folder;
        a_model->setRootPath(folder);
        refilter();
        applyRoot();
        rebuildCrumbs();
        emit locationChanged(folder);
        save();
    }
    if (info.isFile()) selectPath(info.absoluteFilePath());
    updateButtons();
    updateStatus();
}

void FileBrowser::applyRoot()
{
    const QModelIndex root = a_proxy->mapFromSource(a_model->index(a_location));
    // A folder still loading has no entries yet: the Columns view makes its
    // first column the preview, and keeps it when they come - it is given
    // the folder again once it is loaded.
    a_columnsPending = !a_proxy->hasChildren(root);
    a_previewName->clear();
    a_previewFacts->clear();
    a_previewIcon->clear();
    for (QAbstractItemView* v : {static_cast<QAbstractItemView*>(a_tree), static_cast<QAbstractItemView*>(a_list),
                                 static_cast<QAbstractItemView*>(a_grid), static_cast<QAbstractItemView*>(a_details),
                                 static_cast<QAbstractItemView*>(a_columns)})
        v->setRootIndex(root);
}

void FileBrowser::back()
{
    if (a_back.isEmpty()) return;
    const QString from = a_location;
    const QString to = a_back.takeLast();
    a_forward << from;
    if (a_view == View::Recent) setView(a_fileView);
    go(to, false);
    selectPath(from);
}

void FileBrowser::forward()
{
    if (a_forward.isEmpty()) return;
    a_back << a_location;
    const QString to = a_forward.takeLast();
    if (a_view == View::Recent) setView(a_fileView);
    go(to, false);
}

bool FileBrowser::canGoUp() const
{
    return !parentOf(a_location).isEmpty();
}

void FileBrowser::up()
{
    const QString above = parentOf(a_location);
    if (above.isEmpty()) return;
    const QString from = a_location;
    if (a_view == View::Recent) setView(a_fileView);
    go(above, true);
    selectPath(from);   // (where one came from)
}

void FileBrowser::home()
{
    const QString to = a_home.isEmpty() ? QDir::homePath() : a_home;
    if (a_view == View::Recent) setView(a_fileView);
    go(to, true);
}

void FileBrowser::setHomePath(const QString& path)
{
    a_home = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (a_location.isEmpty() || !QFileInfo(a_location).isDir()) go(a_home.isEmpty() ? QDir::homePath() : a_home, false);
}

void FileBrowser::setProjectPath(const QString& path)
{
    a_project = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void FileBrowser::setRecentFiles(const QStringList& files)
{
    a_recentFiles = files;
    if (a_view == View::Recent) fillRecent();
}

void FileBrowser::setDocumentProvider(std::function<QString()> provider)
{
    a_document = std::move(provider);
}

void FileBrowser::fillPlaces(QMenu* menu)
{
    menu->clear();
    const auto add = [this, menu](const QString& label, const QString& path, bool project = false) {
        if (path.isEmpty() || !QFileInfo(path).isDir()) return;
        Kind kind;
        kind.glyph = project ? Kind::Project : Kind::Folder;
        QAction* a = menu->addAction(IconProvider::iconFor(kind), label, this, [this, path] {
            if (a_view == View::Recent) setView(a_fileView);
            go(path, true);
        });
        a->setToolTip(QDir::toNativeSeparators(path));
    };
    add(tr("Workspace"), a_home);
    if (!a_project.isEmpty()) {
        QString name = QFileInfo(a_project).fileName();
        if (name.endsWith(QLatin1String("_prj"))) name.chop(4);
        add(tr("Project %1").arg(name), a_project, true);
    }
    add(tr("Examples"), QucsSettings.ExamplesDir);
    menu->addSeparator();
    add(tr("Home"), QDir::homePath());
    for (const auto location : {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if (path != QDir::homePath()) add(QStandardPaths::displayName(location), path);
    }
    menu->addSeparator();
#ifdef Q_OS_WIN
    for (const QFileInfo& drive : QDir::drives()) add(QDir::toNativeSeparators(drive.absolutePath()), drive.absolutePath());
#else
    add(tr("Computer"), QStringLiteral("/"));
    for (const QStorageInfo& volume : QStorageInfo::mountedVolumes()) {
        const QString root = volume.rootPath();
        if (volume.isValid() && volume.isReady()
            && (root.startsWith(QLatin1String("/Volumes/")) || root.startsWith(QLatin1String("/media/"))
                || root.startsWith(QLatin1String("/run/media/")) || root.startsWith(QLatin1String("/mnt/"))))
            add(volume.displayName(), root);
    }
#endif
    menu->setToolTipsVisible(true);
}

// ----------------------------------------------------------------------
// The path's buttons.

void FileBrowser::rebuildCrumbs()
{
    qDeleteAll(a_crumbButtons);
    a_crumbButtons.clear();
    qDeleteAll(a_crumbSeparators);
    a_crumbSeparators.clear();
    if (a_location.isEmpty()) return;
    // From the home folder when under it, else from the top.
    const QString homeDir = QDir::cleanPath(QDir::homePath());
    QStringList chain;
    for (QString p = a_location; !p.isEmpty(); p = parentOf(p)) {
        chain.prepend(p);
        if (p == homeDir) break;
    }
    int at = 1;   // (after the … of the folders left out)
    for (int i = 0; i < chain.size(); ++i) {
        const QString path = chain.at(i);
        if (i > 0) {
            auto* separator = new QLabel(QStringLiteral("›"), a_crumbBar);
            separator->setObjectName(QStringLiteral("fbCrumbSep"));
            a_crumbLayout->insertWidget(at++, separator);
            a_crumbSeparators << separator;
        }
        QString label = path == homeDir ? tr("Home") : QFileInfo(path).fileName();
        if (label.isEmpty()) label = QDir::toNativeSeparators(path);   // the top, a drive
        auto* b = new QToolButton(a_crumbBar);
        b->setObjectName(i == chain.size() - 1 ? QStringLiteral("fbCrumbCurrent") : QStringLiteral("fbCrumb"));
        b->setText(label);
        b->setProperty("label", label);
        b->setProperty("path", path);
        b->setToolTip(QDir::toNativeSeparators(path));
        b->setAutoRaise(true);
        if (path == homeDir) {
            b->setIcon(glyphIcon(Glyph::Home));
            b->setIconSize(QSize(12, 12));
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        connect(b, &QToolButton::clicked, this, [this, path] {
            if (a_view == View::Recent) setView(a_fileView);
            go(path, true);
        });
        a_crumbLayout->insertWidget(at++, b);
        a_crumbButtons << b;
    }
    layoutCrumbs();
}

void FileBrowser::layoutCrumbs()
{
    const int n = int(a_crumbButtons.size());
    if (n == 0) {
        a_crumbMore->hide();
        return;
    }
    const QMargins m = a_crumbLayout->contentsMargins();
    const int room = std::max(40, a_crumbBar->width() - m.left() - m.right());
    const int more = a_crumbMore->sizeHint().width() + (a_crumbSeparators.isEmpty() ? 0 : a_crumbSeparators.constFirst()->sizeHint().width());
    // The folder shown, shortened if it must.
    QToolButton* last = a_crumbButtons.constLast();
    const QString label = last->property("label").toString();
    last->setText(last->fontMetrics().elidedText(label, Qt::ElideMiddle, std::max(24, room - (n > 1 ? more : 0) - 20)));
    // Then the folders above it, from the nearest, while there is room.
    int used = 0;
    int first = n - 1;
    for (int i = n - 1; i >= 0; --i) {
        const int w = a_crumbButtons.at(i)->sizeHint().width() + (i > 0 ? a_crumbSeparators.at(i - 1)->sizeHint().width() : 0);
        if (i < n - 1 && used + w + (i > 0 ? more : 0) > room) break;
        used += w;
        first = i;
    }
    QMenu* menu = a_crumbMore->menu();
    menu->clear();
    for (int i = 0; i < n; ++i) {
        const bool shown = i >= first;
        a_crumbButtons.at(i)->setVisible(shown);
        if (i > 0) a_crumbSeparators.at(i - 1)->setVisible(shown);
        if (!shown) {
            const QString path = a_crumbButtons.at(i)->property("path").toString();
            menu->addAction(a_crumbButtons.at(i)->icon(), a_crumbButtons.at(i)->text(), this, [this, path] {
                if (a_view == View::Recent) setView(a_fileView);
                go(path, true);
            });
        }
    }
    a_crumbMore->setVisible(first > 0);
}

void FileBrowser::editPath()
{
    a_pathEdit->setText(QDir::toNativeSeparators(a_location));
    a_pathStack->setCurrentWidget(a_pathEdit);
    a_pathEdit->setFocus();
    a_pathEdit->selectAll();
}

// ----------------------------------------------------------------------
// The views.

void FileBrowser::setView(View view)
{
    const QString keep = a_selected;
    a_view = view;
    if (view != View::Recent) a_fileView = view;
    a_stack->setCurrentWidget(viewFor(view));
    refilter();
    if (view == View::Recent) fillRecent();
    for (QAction* a : a_viewActions->actions())
        if (a->data().toInt() == int(view)) {
            a->setChecked(true);
            a_viewButton->setIcon(a->icon());
            a_viewButton->setToolTip(tr("Shown as: %1").arg(a->text()));
        }
    selectPath(keep);
    updateButtons();
    updateStatus();
    save();
}

QAbstractItemView* FileBrowser::viewFor(View view) const
{
    switch (view) {
    case View::Tree: return a_tree;
    case View::List: return a_list;
    case View::Icons: return a_grid;
    case View::Details: return a_details;
    case View::Columns: return a_columns;
    case View::Recent: return a_recent;
    }
    return a_list;
}

QAbstractItemView* FileBrowser::currentView() const
{
    return viewFor(a_view);
}

void FileBrowser::refilter()
{
    a_proxy->configure(a_filter->text().trimmed(), a_qucsOnly, isFlat(a_view), a_location);
    if (a_view == View::Recent) fillRecent();
    a_statusTimer->start();
}

void FileBrowser::setFilterText(const QString& text)
{
    a_filter->setText(text);
    refilter();
}

void FileBrowser::setShowHidden(bool on)
{
    a_showHidden = on;
    const QSignalBlocker quiet(a_hiddenAction);
    a_hiddenAction->setChecked(on);
    QDir::Filters filter = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs;
    if (on) filter |= QDir::Hidden;
    a_model->setFilter(filter);
    save();
}

void FileBrowser::setQucsFilesOnly(bool on)
{
    a_qucsOnly = on;
    const QSignalBlocker quiet(a_qucsAction);
    a_qucsAction->setChecked(on);
    refilter();
    save();
}

void FileBrowser::fillRecent()
{
    a_recentModel->clear();
    const QString text = a_filter->text().trimmed();
    for (const QString& file : std::as_const(a_recentFiles)) {
        const QFileInfo info(file);
        if (!info.isFile()) continue;
        if (!text.isEmpty() && !info.fileName().contains(text, Qt::CaseInsensitive)) continue;
        auto* item = new QStandardItem(IconProvider::iconFor(kindOf(info)), info.fileName());
        item->setData(QDir::cleanPath(info.absoluteFilePath()), Qt::UserRole);
        item->setData(shownPath(info.absolutePath()), Qt::UserRole + 1);
        item->setToolTip(QDir::toNativeSeparators(info.absoluteFilePath()));
        item->setEditable(false);
        a_recentModel->appendRow(item);
    }
    a_statusTimer->start();
}

QString FileBrowser::pathOf(const QModelIndex& index) const
{
    if (!index.isValid()) return {};
    if (index.model() == a_recentModel) return index.data(Qt::UserRole).toString();
    const QModelIndex source = a_proxy->mapToSource(index.sibling(index.row(), 0));
    return source.isValid() ? QDir::cleanPath(a_model->filePath(source)) : QString();
}

QModelIndex FileBrowser::indexOf(const QString& path) const
{
    if (a_view == View::Recent) {
        for (int r = 0; r < a_recentModel->rowCount(); ++r)
            if (a_recentModel->item(r)->data(Qt::UserRole).toString() == QDir::cleanPath(path)) return a_recentModel->index(r, 0);
        return {};
    }
    return a_proxy->mapFromSource(a_model->index(path));
}

bool FileBrowser::inView(const QString& path) const
{
    if (a_view == View::Recent) return indexOf(path).isValid();
    const QString inside = a_location.endsWith(QLatin1Char('/')) ? a_location : a_location + QLatin1Char('/');
    const QString clean = QDir::cleanPath(path);
    if (!clean.startsWith(inside)) return false;
    // The flat views show the folder's own entries only.
    return !isFlat(a_view) || QDir::cleanPath(QFileInfo(clean).absolutePath()) == a_location;
}

QStringList FileBrowser::shownNames() const
{
    QStringList names;
    if (a_view == View::Recent) {
        for (int r = 0; r < a_recentModel->rowCount(); ++r) names << a_recentModel->item(r)->text();
        return names;
    }
    const QModelIndex root = currentView()->rootIndex();
    for (int r = 0; r < a_proxy->rowCount(root); ++r) names << a_proxy->index(r, 0, root).data().toString();
    return names;
}

QString FileBrowser::selectedPath() const
{
    const QItemSelectionModel* selection = currentView()->selectionModel();
    if (selection == nullptr) return {};
    for (const QModelIndex& index : selection->selectedIndexes())
        if (index.column() == 0) return pathOf(index);
    return {};
}

void FileBrowser::selectPath(const QString& path)
{
    if (path.isEmpty() || !inView(path)) return;
    a_selected = QDir::cleanPath(path);
    QAbstractItemView* v = currentView();
    const QModelIndex index = indexOf(path);
    if (!index.isValid()) return;
    // (Columns: once its folder has a column - selected again then.)
    if (v == a_columns && !static_cast<ColumnView*>(a_columns)->hasColumn()) return;
    if (v == a_tree)
        for (QModelIndex p = index.parent(); p.isValid() && p != a_tree->rootIndex(); p = p.parent()) a_tree->expand(p);
    v->setCurrentIndex(index);
    v->scrollTo(index);
}

void FileBrowser::fillPreview(const QModelIndex& index)
{
    const QFileInfo info(pathOf(index));
    const int side = 64;
    a_previewIcon->setPixmap(IconProvider::iconFor(kindOf(info)).pixmap(QSize(side, side), devicePixelRatioF()));
    a_previewName->setText(info.fileName());
    QStringList facts{kindOf(info).name};
    if (info.isFile()) facts << sizeText(info.size());
    facts << tr("Modified %1").arg(QLocale().toString(info.lastModified(), QLocale::ShortFormat));
    a_previewFacts->setText(facts.join(QLatin1Char('\n')));
}

void FileBrowser::fitDetails()
{
    QHeaderView* header = a_details->header();
    int others = 0;
    for (int c = 1; c < header->count(); ++c)
        if (!header->isSectionHidden(c)) others += header->sectionSize(c);
    const int width = std::max(140, a_details->viewport()->width() - others);
    if (header->sectionSize(0) != width) header->resizeSection(0, width);
}

void FileBrowser::onActivated(const QModelIndex& index)
{
    activate(pathOf(index));
}

void FileBrowser::activate(const QString& path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists()) return;
    if (!info.isDir()) {
        emit openRequested(QDir::cleanPath(info.absoluteFilePath()));
        return;
    }
    switch (a_view) {
    case View::Tree: {
        const QModelIndex index = indexOf(path);
        if (index.isValid() && inView(path)) a_tree->setExpanded(index, !a_tree->isExpanded(index));
        else go(path, true);
        break;
    }
    case View::Columns:
        if (!inView(path)) go(path, true);
        else if (static_cast<ColumnView*>(a_columns)->hasColumn()) a_columns->setCurrentIndex(indexOf(path));
        break;
    case View::Recent:
        setView(a_fileView);
        go(path, true);
        break;
    default:
        go(path, true);
        break;
    }
}

// ----------------------------------------------------------------------
// What can be done with an entry.

QString FileBrowser::createFolder(const QString& parent)
{
    QDir dir(parent);
    if (parent.isEmpty() || !dir.exists()) return {};
    QString name = tr("New Folder");
    for (int n = 2; dir.exists(name); ++n) name = tr("New Folder %1").arg(n);
    if (!dir.mkdir(name)) return {};
    return QDir::cleanPath(dir.absoluteFilePath(name));
}

void FileBrowser::rename(const QString& path)
{
    QAbstractItemView* v = currentView();
    if (v != a_recent && inView(path)) {
        // In place, where it is.
        const QModelIndex index = indexOf(path);
        if (index.isValid()) {
            selectPath(path);
            v->edit(index);
            return;
        }
    }
    const QFileInfo info(path);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename"), tr("The new name of “%1”:").arg(info.fileName()),
                                               QLineEdit::Normal, info.fileName(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == info.fileName()) return;
    const QString target = info.dir().filePath(name);
    if (QFileInfo::exists(target) || !QDir().rename(path, target))
        QMessageBox::warning(this, tr("File Browser"), tr("“%1” could not be renamed to “%2”.").arg(info.fileName(), name));
}

void FileBrowser::moveToTrash(const QString& path)
{
    const QFileInfo info(path);
    if (QMessageBox::question(this, tr("File Browser"), tr("Move “%1” to the trash?").arg(info.fileName()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    if (!QFile::moveToTrash(path))
        QMessageBox::warning(this, tr("File Browser"), tr("“%1” could not be moved to the trash.").arg(info.fileName()));
}

QMenu* FileBrowser::contextMenuFor(const QString& path)
{
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("fbContextMenu"));
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists()) {
        // The folder shown.
        menu->addAction(tr("New Folder…"), this, [this] {
            const QString made = createFolder(a_location);
            if (!made.isEmpty()) rename(made);
        });
        menu->addSeparator();
        menu->addAction(revealLabel(false), this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(a_location)); });
        menu->addAction(tr("Copy Path"), this, [this] { QApplication::clipboard()->setText(QDir::toNativeSeparators(a_location)); });
        return menu;
    }
    if (info.isDir()) {
        QAction* open = menu->addAction(tr("Open"), this, [this, path] {
            if (a_view == View::Recent) setView(a_fileView);
            go(path, true);
        });
        menu->setDefaultAction(open);
        menu->addSeparator();
        menu->addAction(tr("New Folder…"), this, [this, path] {
            const QString made = createFolder(path);
            if (made.isEmpty()) return;
            if (!inView(made)) go(path, true);
            rename(made);
        });
    } else {
        QAction* open = menu->addAction(tr("Open"), this, [this, path] { emit openRequested(path); });
        menu->setDefaultAction(open);
        menu->addAction(tr("Open with the System's Application"), this, [path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        if (a_view == View::Recent)
            menu->addAction(tr("Show in the Folder"), this, [this, path] {
                setView(a_fileView);
                go(path, true);
            });
    }
    menu->addSeparator();
    menu->addAction(revealLabel(true), this, [path] { revealItem(path); });
    menu->addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
    menu->addSeparator();
    menu->addAction(tr("Rename…"), this, [this, path] { rename(path); });
    menu->addAction(tr("Move to Trash…"), this, [this, path] { moveToTrash(path); });
    return menu;
}

void FileBrowser::showContextMenu(QAbstractItemView* view, const QPoint& pos)
{
    const QModelIndex index = view->indexAt(pos);
    const QString path = index.isValid() ? pathOf(index) : QString();
    if (index.isValid()) view->setCurrentIndex(index);
    QMenu* menu = contextMenuFor(path);
    menu->exec(view->viewport()->mapToGlobal(pos));
    menu->deleteLater();
}

// ----------------------------------------------------------------------

void FileBrowser::updateButtons()
{
    a_backButton->setEnabled(canGoBack());
    a_forwardButton->setEnabled(canGoForward());
    a_upButton->setEnabled(canGoUp());
}

void FileBrowser::updateStatus()
{
    if (a_view == View::Recent) {
        const int n = a_recentModel->rowCount();
        a_status->setText(n == 0 ? tr("No recent documents") : n == 1 ? tr("1 recent document") : tr("%1 recent documents").arg(n));
        return;
    }
    const QString selected = selectedPath();
    const QFileInfo info(selected);
    if (!selected.isEmpty() && info.isFile()) {
        a_status->setText(QStringLiteral("%1  ·  %2  ·  %3")
                              .arg(kindOf(info).name, sizeText(info.size()),
                                   QLocale().toString(info.lastModified(), QLocale::ShortFormat)));
        return;
    }
    int folders = 0, files = 0;
    const QModelIndex root = a_list->rootIndex();
    for (int r = 0; r < a_proxy->rowCount(root); ++r) {
        if (a_model->isDir(a_proxy->mapToSource(a_proxy->index(r, 0, root)))) ++folders;
        else ++files;
    }
    QString text = QStringLiteral("%1, %2").arg(folders == 1 ? tr("1 folder") : tr("%1 folders").arg(folders),
                                                files == 1 ? tr("1 file") : tr("%1 files").arg(files));
    if (!a_filter->text().trimmed().isEmpty() || a_qucsOnly) text += QStringLiteral("  ·  ") + tr("filtered");
    a_status->setText(text);
}

void FileBrowser::save() const
{
    if (!a_loaded) return;
    QucsSettingsFile settings;
    settings.setValue(kLocation, a_location);
    settings.setValue(kView, int(a_view));
    settings.setValue(kHidden, a_showHidden);
    settings.setValue(kQucsOnly, a_qucsOnly);
}

void FileBrowser::restyle()
{
    using qucs_s::apptheme::mix;
    const QPalette pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QString sheet =
        QStringLiteral(
            "QToolButton[fbTool=\"true\"] { border: none; border-radius: 5px; padding: 3px; background: transparent; }"
            "QToolButton[fbTool=\"true\"]:hover { background: %2; }"
            "QToolButton[fbTool=\"true\"]:pressed, QToolButton[fbTool=\"true\"]:open { background: %5; }"
            "QToolButton[fbTool=\"true\"]::menu-indicator { image: none; width: 0px; }"
            "QToolButton#fbCrumb, QToolButton#fbCrumbCurrent { border: none; border-radius: 4px; padding: 1px 4px;"
            " background: transparent; }"
            "QToolButton#fbCrumb { color: %1; }"
            "QToolButton#fbCrumb:hover, QToolButton#fbCrumbCurrent:hover { background: %2; color: %3; }"
            "QToolButton#fbCrumbCurrent { color: %3; font-weight: 600; }"
            "QToolButton#fbCrumb::menu-indicator { image: none; width: 0px; }"
            "QLabel#fbCrumbSep { color: %4; padding: 0 1px; }"
            "QLabel#fbStatus { color: %1; padding: 2px 6px 3px 6px; }"
            "QLabel#fbPreviewName { font-weight: 600; }"
            "QLabel#fbPreviewFacts { color: %1; }")
            .arg(mix(window, text, 0.68).name(), mix(window, text, 0.1).name(), text.name(), mix(window, text, 0.38).name(),
                 mix(window, text, 0.18).name());
    if (sheet != styleSheet()) setStyleSheet(sheet);
    // The icons follow the theme: drawn again.
    for (QAbstractItemView* v : a_stack->findChildren<QAbstractItemView*>()) v->viewport()->update();
}

void FileBrowser::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange)
        QTimer::singleShot(0, this, &FileBrowser::restyle);
}

void FileBrowser::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutCrumbs();
}

bool FileBrowser::eventFilter(QObject* watched, QEvent* event)
{
    if (a_details != nullptr && watched == a_details->viewport() && event->type() == QEvent::Resize) {
        fitDetails();
    } else if (watched == a_crumbBar) {
        if (event->type() == QEvent::Resize) layoutCrumbs();
        if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            editPath();
            return true;
        }
    } else if (watched == a_pathEdit) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            a_pathStack->setCurrentWidget(a_crumbBar);
            return true;
        }
        if (event->type() == QEvent::FocusOut && static_cast<QFocusEvent*>(event)->reason() != Qt::PopupFocusReason)
            a_pathStack->setCurrentWidget(a_crumbBar);
    } else if (qobject_cast<QAbstractItemView*>(watched) != nullptr
               && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Its keys before the window's shortcuts for them.
        if (event->type() == QEvent::ShortcutOverride && listKey(key)) {
            event->accept();
            return true;
        }
        // (While a name is edited its keys go to the editor, not here.)
        if (event->type() == QEvent::KeyPress && key->key() == Qt::Key_Backspace && key->modifiers() == Qt::NoModifier
            && isFlat(a_view)) {
            up();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
