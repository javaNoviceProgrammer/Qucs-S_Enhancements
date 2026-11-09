/*
 * plotdigitizer_p.h - what plotdigitizer.cpp and plotdigitizer_trace.cpp share
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PLOTDIGITIZER_P_H
#define QUCS_PLOTDIGITIZER_P_H

#include "plotdigitizer.h"

namespace qucs_s::digitizer {

/// The most pixels a picture is drawn at for one plot.
inline constexpr qint64 kMostPixels = 16000000;
/// Longer than this is a line, shorter a tick (points).
inline constexpr double kMostTickLength = 22;

/// A straight line along one axis: \c at across it, \c from .. \c to along it.
struct HLine {
    double at = 0;
    double from = 0, to = 0;
    double width = 0;
    QColor colour;
};

/// The lines of the vector art, sorted out by length.
void collectVectorLines(const pdf::vec::Graphics& g, const QTransform& work, QList<HLine>* longH, QList<HLine>* longV, QList<HLine>* shortH, QList<HLine>* shortV);

/// The lines found in a picture of a page, set straight first if it is turned.
struct RasterLines {
    bool valid = false;
    QTransform work;       ///< page points to working points
    double angle = 0;
    double scale = 0;
    QList<HLine> longH, longV, shortH, shortV;
    QImage picture;        ///< the picture they were found in, set straight
    QPointF origin;        ///< its corner, in working points
};
RasterLines rasterLines(Source& source, int page, const QRectF& within);
/// The tick marks along the edges of \a frame (working points) in the picture:
/// what sticks out of the frame's line, at a corner too, where a tick runs
/// into the next edge and is no short stroke of its own.
void marksInPicture(const RasterLines& lines, const QRectF& frame, QList<HLine>* shortV, QList<HLine>* shortH);
QList<double> regularMarks(const QList<double>& marks, double tolerance);

bool operator==(const Token& a, const Token& b);

void readAxes(Plot& plot, const QList<Token>& tokens, const QList<HLine>& shortV, const QList<HLine>& shortH, const QList<HLine>& longV, const QList<HLine>& longH);

QString shapeText(Series::Shape s);
QString originText(Series::Origin o);

// --- Shared by the vector and the raster tracing (plotdigitizer_trace.cpp) --

double colourDistance(const QColor& a, const QColor& b);
bool wanted(const Options& o, const QColor& c);
bool achromatic(const QColor& c);
double lengthOf(const QPolygonF& q);
QPointF normal(QPointF d);
QPointF turned(QPointF d, double degrees);
/// (QRectF::intersects() says no for a box with no height: a level line.)
bool overlaps(const QRectF& a, const QRectF& b);
double luminanceDark(QRgb v);

/// Words on a line, joined.
struct Phrase {
    QString text;
    QRectF box;
};
QList<Phrase> phrasesOf(QList<Token> tokens);

/// A part of the page drawn in working coordinates.
struct Pixels {
    int w = 0, h = 0;
    QImage image;             ///< Format_RGB32
    double scale = 1;         ///< pixels a working point
    QPointF origin;           ///< the image's corner, in working points
    QTransform pageToWork;
    QTransform workToPage;

    QPointF toWork(QPointF pixel) const { return origin + pixel / scale; }
    QPointF toPixel(QPointF work) const { return (work - origin) * scale; }
    QPointF toPage(QPointF pixel) const { return workToPage.map(toWork(pixel)); }
};
Pixels workImage(Source& source, const Plot& plot, const QRectF& area, double scale);
/// The runs of dark pixels along rows (or columns): longer than \a minLong
/// points (0: a tick's longest) are lines, the rest joined into ticks.
void linesAlong(const QImage& image, bool rows, double scale, QPointF origin, QList<HLine>* longs, QList<HLine>* shorts, double minLong = 0);
QList<HLine> merged(QList<HLine> lines, double tolerance, double gap);
QList<QRectF> rectanglesOf(const QList<HLine>& h, const QList<HLine>& v, double minWidth, double minHeight);

/// The raster route (plotdigitizer_raster.cpp).
bool extractRaster(Source& source, Plot& plot, const Options& options, QStringList* notes);
bool traceRaster(Source& source, Plot& plot, QPointF seed, const Options& options, Series* out, QString* why);
/// Pieces of a line of a single point between gaps joined to the piece beside
/// them whose end is nearer, within \a reach (points), or taken out. How many.
int foldLonePoints(Series& s, double reach);
/// Every series' points that fall in a region left out, taken out (a piece
/// where they were).
void dropExcluded(Plot& plot, QStringList* notes);

} // namespace qucs_s::digitizer

#endif // QUCS_PLOTDIGITIZER_P_H
