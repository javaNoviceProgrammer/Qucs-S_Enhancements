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

bool operator==(const Token& a, const Token& b);

void readAxes(Plot& plot, const QList<Token>& tokens, const QList<HLine>& shortV, const QList<HLine>& shortH, const QList<HLine>& longV, const QList<HLine>& longH);

QString shapeText(Series::Shape s);
QString originText(Series::Origin o);

} // namespace qucs_s::digitizer

#endif // QUCS_PLOTDIGITIZER_P_H
