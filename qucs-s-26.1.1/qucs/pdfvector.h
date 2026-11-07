/*
 * pdfvector.h - the vector art of a PDF's page: a small reader of the file
 *               and of a page's content stream, for the plot digitizer
 *
 * Qt's PDF module draws a page and gives its text, but not the paths it is
 * drawn with (pdfium's calls are not exported from it). A plot drawn as
 * vector art holds the very vertices of its curves, which no tracing of a
 * picture can match, so this reads them. It reads, it does not draw: the
 * pictures of the pages stay pdfium's. What it cannot read (an encrypted
 * file, a damaged one) it says, and the digitizer works from the picture.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PDFVECTOR_H
#define QUCS_PDFVECTOR_H

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <memory>

namespace qucs_s::pdf::vec {

/// One subpath of a path painted: page points, from the top left of the
/// page as it is shown (its crop box, its rotation).
struct Part {
    QPolygonF points;
    bool closed = false;
    bool curved = false;      ///< has Bézier pieces (flattened here)
    bool rectangle = false;   ///< a 're'
};

/// A path as it was painted (S, f, B...).
struct Path {
    QList<Part> parts;
    bool stroked = false;
    bool filled = false;
    QColor stroke, fill;
    double width = 1;         ///< of the stroke, in points
    QList<double> dash;       ///< in points; empty: solid
    QRectF clip;              ///< the clip's box in force; null: none
    /// Drawn by a form XObject (matplotlib's markers are): which one and
    /// where its origin fell. A form's origin is where the plotter put it,
    /// its data point.
    int form = -1;
    QPointF origin;
};

/// An image placed on the page.
struct Picture {
    QPolygonF corners;   ///< four, in page points
    int width = 0;       ///< its own pixels
    int height = 0;
};

struct Graphics {
    QSizeF size;             ///< the page as it is shown, in points
    QList<Path> paths;
    QList<Picture> pictures;
    bool truncated = false;  ///< more than is read here: the first of it
};

/// A PDF file, read: the bytes are kept for the pages asked of it.
class Reader
{
public:
    /// \a bytes as a PDF, or null with why in \a error (\a encrypted: that
    /// is why, its passwords not being tried here).
    static std::shared_ptr<Reader> open(const QByteArray& bytes, QString* error = nullptr, bool* encrypted = nullptr);
    ~Reader();
    int pageCount() const;
    /// What page \a page (from 0) draws; false with why when it cannot be read.
    bool page(int page, Graphics* out, QString* error = nullptr);

private:
    Reader();
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// The limits of what one page is read to.
inline constexpr qsizetype MostDecoded = qsizetype(256) << 20;   ///< a stream's bytes, decoded
inline constexpr int MostVertices = 8000000;
inline constexpr int MostPaths = 1000000;
inline constexpr int MostDepth = 12;   ///< forms within forms

} // namespace qucs_s::pdf::vec

#endif // QUCS_PDFVECTOR_H
