/*
 * graphicsexport.h - a schematic, a symbol or a data display as a file -
 * PNG, JPEG, BMP, TIFF, WebP, SVG, PDF, EPS, or PDF with its text in a
 * LaTeX overlay (PDF+LaTeX) - or on the clipboard; no program besides
 * Qucs-S is needed for any of them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_GRAPHICSEXPORT_H
#define QUCS_GRAPHICSEXPORT_H

#include "exportdevices.h"

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

#include <optional>

class QMimeData;
class Schematic;

namespace qucs_s::graphicsexport {

enum class Format { Png, Jpeg, Bmp, Tiff, Webp, Svg, Pdf, Eps, PdfTex };

using exportdevices::Colours;

struct Options {
    bool selectionOnly = false;
    double scale = 1.0;                  ///< a raster image's pixels per unit of the schematic: 1 is 96 dpi
    Colours colours = Colours::Colour;
    bool transparent = false;            ///< no paper, where the format can have none
    int quality = 90;                    ///< JPEG and WebP, 1 to 100
    bool textAsOutlines = false;         ///< SVG and PDF; EPS always has outlines, PDF+LaTeX's text is LaTeX's
};

/// The margin around the drawing, in units of the schematic.
constexpr int Margin = 30;
/// The largest raster image written: an image is scaled down to fit
/// (400 MB in 32-bit colour, 32000 pixels a side).
constexpr double MaxImagePixels = 100e6;
constexpr double MaxImageSide = 32000;

/// The formats this build writes, in the order they are offered.
QList<Format> formats();
/// The suffix a file of the format gets: "png", "jpg", "pdf_tex".
QString suffix(Format format);
/// Every suffix taken for the format: "jpg" and "jpeg".
QStringList suffixes(Format format);
/// "PNG image", "PDF + LaTeX".
QString description(Format format);
/// The format of a file name, by its suffix (in any case).
std::optional<Format> formatOf(const QString& fileName);
/// \a fileName with the suffix of \a format: a suffix of another format is
/// replaced, anything else is kept and the suffix added.
QString withSuffix(const QString& fileName, Format format);
/// "PNG image (*.png)".
QString nameFilter(Format format);
/// The filters of formats(), for a file dialog.
QString nameFilters();

bool isVector(Format format);
/// The format can leave the paper out.
bool hasTransparency(Format format);
/// The format has a quality setting (lossy compression).
bool hasQuality(Format format);

/// What is drawn, in units of the schematic, the margin included (and a
/// schematic's frame, when it shows one); empty when there is nothing to
/// draw - no selection, with \a selectionOnly.
QRect area(Schematic* schematic, bool selectionOnly);
/// The pixels of a raster image.
QSize pixelSize(Schematic* schematic, const Options& options);

/// Rendered as a raster image, with its resolution set (96 dpi x scale).
QImage image(Schematic* schematic, const Options& options);
QByteArray svg(Schematic* schematic, const Options& options);
/// With \a texts, the text is not drawn but kept there (PDF+LaTeX).
QByteArray pdf(Schematic* schematic, const Options& options,
               QList<exportdevices::TextRun>* texts = nullptr);
QByteArray eps(Schematic* schematic, const Options& options);
/// The LaTeX half of PDF+LaTeX: the picture of \a pdfName (the PDF, beside
/// it) with \a texts on top, for a drawing of \a size units.
QByteArray pdfTex(const QList<exportdevices::TextRun>& texts, QSize size, const QString& pdfName);
/// The PDF that goes with a PDF+LaTeX file: name.pdf beside name.pdf_tex.
QString pdfOf(const QString& pdfTexFileName);

/// Writes the file (PDF+LaTeX: both files); false and why in \a error when
/// it could not.
bool write(Schematic* schematic, const QString& fileName, Format format,
           const Options& options, QString* error = nullptr);

/// The drawing for the clipboard: an image (at \a options' scale), an SVG
/// (text as outlines) and a PDF. The caller owns it.
QMimeData* mimeData(Schematic* schematic, const Options& options);

} // namespace qucs_s::graphicsexport

#endif
