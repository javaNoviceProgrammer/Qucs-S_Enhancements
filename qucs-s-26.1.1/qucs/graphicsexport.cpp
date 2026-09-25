/*
 * graphicsexport.cpp - a schematic, a symbol or a data display as a file,
 * or on the clipboard
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "graphicsexport.h"
#include "schematic.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QMimeData>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSvgGenerator>

#include <algorithm>
#include <cmath>

namespace qucs_s::graphicsexport {

using exportdevices::RelayDevice;
using exportdevices::TextMode;
using exportdevices::TextRun;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("GraphicsExport", text);
}

// The name of the image format for QImageWriter.
QByteArray writerFormat(Format format)
{
    switch (format) {
    case Format::Png:  return "png";
    case Format::Jpeg: return "jpeg";
    case Format::Bmp:  return "bmp";
    case Format::Tiff: return "tiff";
    case Format::Webp: return "webp";
    default:           return {};
    }
}

QString title(Schematic* schematic)
{
    return QFileInfo(schematic->getDocName()).completeBaseName();
}

QString creator()
{
    return QStringLiteral("Qucs-S " PACKAGE_VERSION);
}

// Draws the schematic (or its selection) filling the painter's device.
void paint(Schematic* schematic, QPainter* painter, bool selectionOnly)
{
    schematic->print(nullptr, painter, !selectionOnly, true,
                     QMargins(Margin, Margin, Margin, Margin));
}

bool save(const QString& fileName, const QByteArray& bytes, QString* error)
{
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(fileName), file.errorString());
        return false;
    }
    return true;
}

bool writeImage(Schematic* schematic, const QString& fileName, Format format,
                const Options& options, QString* error)
{
    QImage out = image(schematic, options);
    if (!options.transparent) {
        switch (options.colours) {
        case Colours::Colour:
            out = out.convertToFormat(QImage::Format_RGB32);
            break;
        case Colours::Grayscale:
            out = out.convertToFormat(QImage::Format_Grayscale8);
            break;
        case Colours::Monochrome:
            out = out.convertToFormat(QImage::Format_Mono, Qt::ThresholdDither);
            break;
        }
        // Neither JPEG nor WebP takes one bit per pixel; WebP takes no grey.
        if (format == Format::Jpeg && out.format() == QImage::Format_Mono)
            out = out.convertToFormat(QImage::Format_Grayscale8);
        if (format == Format::Webp && out.format() != QImage::Format_RGB32)
            out = out.convertToFormat(QImage::Format_RGB32);
    }
    QImageWriter writer(fileName, writerFormat(format));
    if (hasQuality(format))
        writer.setQuality(qBound(1, options.quality, 100));
    if (format == Format::Tiff)
        writer.setCompression(1);   // LZW
    if (!writer.write(out)) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(fileName), writer.errorString());
        return false;
    }
    return true;
}

// A number for LaTeX: no exponent, no trailing zeros.
QString number(double v)
{
    if (!std::isfinite(v) || std::abs(v) < 5e-7)
        return QStringLiteral("0");
    QString s = QString::number(v, 'f', 6);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s;
}

// A text of the schematic as LaTeX sets it: with a "$" in it the text is
// meant for LaTeX and stays as it is; otherwise what LaTeX takes as a
// command is escaped.
QString latex(const QString& text)
{
    if (text.contains(QLatin1Char('$')))
        return text;
    QString out;
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '\\': out += QLatin1String("\\textbackslash{}"); break;
        case '{':  out += QLatin1String("\\{"); break;
        case '}':  out += QLatin1String("\\}"); break;
        case '#':  out += QLatin1String("\\#"); break;
        case '%':  out += QLatin1String("\\%"); break;
        case '&':  out += QLatin1String("\\&"); break;
        case '_':  out += QLatin1String("\\_"); break;
        case '^':  out += QLatin1String("\\^{}"); break;
        case '~':  out += QLatin1String("\\~{}"); break;
        case '<':  out += QLatin1String("\\textless{}"); break;
        case '>':  out += QLatin1String("\\textgreater{}"); break;
        case '|':  out += QLatin1String("\\textbar{}"); break;
        default:   out += c; break;
        }
    }
    return out;
}

} // namespace

QList<Format> formats()
{
    const QList<QByteArray> writable = QImageWriter::supportedImageFormats();
    QList<Format> out;
    for (const Format f : {Format::Png, Format::Jpeg, Format::Bmp, Format::Tiff, Format::Webp,
                           Format::Svg, Format::Pdf, Format::Eps, Format::PdfTex}) {
        const QByteArray name = writerFormat(f);
        if (name.isEmpty() || writable.contains(name))
            out << f;
    }
    return out;
}

QString suffix(Format format)
{
    return suffixes(format).constFirst();
}

QStringList suffixes(Format format)
{
    switch (format) {
    case Format::Png:    return {QStringLiteral("png")};
    case Format::Jpeg:   return {QStringLiteral("jpg"), QStringLiteral("jpeg")};
    case Format::Bmp:    return {QStringLiteral("bmp")};
    case Format::Tiff:   return {QStringLiteral("tif"), QStringLiteral("tiff")};
    case Format::Webp:   return {QStringLiteral("webp")};
    case Format::Svg:    return {QStringLiteral("svg")};
    case Format::Pdf:    return {QStringLiteral("pdf")};
    case Format::Eps:    return {QStringLiteral("eps")};
    case Format::PdfTex: return {QStringLiteral("pdf_tex")};
    }
    return {QStringLiteral("png")};
}

QString description(Format format)
{
    switch (format) {
    case Format::Png:    return tr("PNG image");
    case Format::Jpeg:   return tr("JPEG image");
    case Format::Bmp:    return tr("BMP image");
    case Format::Tiff:   return tr("TIFF image");
    case Format::Webp:   return tr("WebP image");
    case Format::Svg:    return tr("SVG vector graphics");
    case Format::Pdf:    return tr("PDF");
    case Format::Eps:    return tr("EPS (Encapsulated PostScript)");
    case Format::PdfTex: return tr("PDF + LaTeX (text set by LaTeX)");
    }
    return {};
}

std::optional<Format> formatOf(const QString& fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    if (ext.isEmpty())
        return std::nullopt;
    for (const Format f : {Format::Png, Format::Jpeg, Format::Bmp, Format::Tiff, Format::Webp,
                           Format::Svg, Format::Pdf, Format::Eps, Format::PdfTex})
        if (suffixes(f).contains(ext))
            return f;
    return std::nullopt;
}

QString withSuffix(const QString& fileName, Format format)
{
    if (fileName.isEmpty())
        return fileName;
    if (const auto current = formatOf(fileName)) {
        if (*current == format)
            return fileName;
        const QString ext = QFileInfo(fileName).suffix();
        return fileName.left(fileName.size() - ext.size()) + suffix(format);
    }
    return fileName + QLatin1Char('.') + suffix(format);
}

QString nameFilter(Format format)
{
    QStringList patterns;
    for (const QString& s : suffixes(format))
        patterns << QStringLiteral("*.") + s;
    return QStringLiteral("%1 (%2)").arg(description(format), patterns.join(QLatin1Char(' ')));
}

QString nameFilters()
{
    QStringList out;
    for (const Format f : formats())
        out << nameFilter(f);
    return out.join(QStringLiteral(";;"));
}

bool isVector(Format format)
{
    switch (format) {
    case Format::Svg:
    case Format::Pdf:
    case Format::Eps:
    case Format::PdfTex:
        return true;
    default:
        return false;
    }
}

bool hasTransparency(Format format)
{
    switch (format) {
    case Format::Jpeg:
    case Format::Bmp:
        return false;
    default:
        return true;
    }
}

bool hasQuality(Format format)
{
    return format == Format::Jpeg || format == Format::Webp;
}

QRect area(Schematic* schematic, bool selectionOnly)
{
    if (schematic == nullptr)
        return {};
    const QRect drawn = schematic->printedArea(!selectionOnly);
    if (!drawn.isValid())
        return {};   // an empty document, no selection
    return drawn.marginsAdded(QMargins(Margin, Margin, Margin, Margin));
}

QSize pixelSize(Schematic* schematic, const Options& options)
{
    const QRect a = area(schematic, options.selectionOnly);
    if (a.isEmpty())
        return {};
    const double scale = options.scale > 0 ? options.scale : 1.0;
    double width = a.width() * scale;
    double height = a.height() * scale;
    // A drawing stretched by one enormous text (a property value of a
    // megabyte: a long PWL list) asked for an image of gigabytes, and
    // twice that again to convert it. Such a drawing is fitted into at
    // most MaxImagePixels, no side longer than MaxImageSide (JPEG takes
    // 65535, and QPainter's raster engine counts in int); paint() fits
    // the drawing to the image.
    const double shrink = std::min({1.0, MaxImageSide / width, MaxImageSide / height,
                                    std::sqrt(MaxImagePixels / (width * height))});
    width *= shrink;
    height *= shrink;
    return QSize(std::max(1, int(std::lround(width))),
                 std::max(1, int(std::lround(height))));
}

QImage image(Schematic* schematic, const Options& options)
{
    const QSize size = pixelSize(schematic, options);
    if (size.isEmpty())
        return {};
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull())
        return {};
    out.fill(options.transparent ? Qt::transparent : Qt::white);
    {
        QPainter target(&out);
        RelayDevice relay(&target, TextMode::Text, options.colours);
        QPainter painter(&relay);
        // Black and white has no shades to smooth an edge with.
        const bool smooth = options.colours != Colours::Monochrome;
        painter.setRenderHint(QPainter::Antialiasing, smooth);
        painter.setRenderHint(QPainter::TextAntialiasing, smooth);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
        paint(schematic, &painter, options.selectionOnly);
    }
    if (options.colours == Colours::Monochrome)
        out = exportdevices::exported(out, Colours::Monochrome);   // the images in it
    const int dotsPerMeter = int(std::lround((options.scale > 0 ? options.scale : 1.0) * 96.0 / 0.0254));
    out.setDotsPerMeterX(dotsPerMeter);
    out.setDotsPerMeterY(dotsPerMeter);
    return out;
}

QByteArray svg(Schematic* schematic, const Options& options)
{
    const QRect a = area(schematic, options.selectionOnly);
    if (a.isEmpty())
        return {};
    const QRect page(QPoint(0, 0), a.size());
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    {
        QSvgGenerator generator;
        generator.setOutputDevice(&buffer);
        generator.setSize(page.size());
        generator.setViewBox(page);
        generator.setResolution(96);
        generator.setTitle(title(schematic));
        generator.setDescription(creator());
        QPainter target(&generator);
        if (!options.transparent)
            target.fillRect(page, Qt::white);
        // A font only this system has is named in the file as the reader's
        // own sans-serif font.
        RelayDevice relay(&target, options.textAsOutlines ? TextMode::Outlines : TextMode::Text,
                          options.colours, QStringLiteral("sans-serif"));
        QPainter painter(&relay);
        paint(schematic, &painter, options.selectionOnly);
    }
    return buffer.data();
}

QByteArray pdf(Schematic* schematic, const Options& options, QList<TextRun>* texts)
{
    const QRect a = area(schematic, options.selectionOnly);
    if (a.isEmpty())
        return {};
    const QRect page(QPoint(0, 0), a.size());
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    {
        QPdfWriter writer(&buffer);
        writer.setResolution(96);
        writer.setPageLayout(QPageLayout(
            QPageSize(QSizeF(page.size()) * 0.75, QPageSize::Point, QString(), QPageSize::ExactMatch),
            QPageLayout::Portrait, QMarginsF()));
        writer.setTitle(title(schematic));
        writer.setCreator(creator());
        QPainter target(&writer);
        if (!options.transparent)
            target.fillRect(page, Qt::white);
        const TextMode mode = texts != nullptr ? TextMode::Collect
                            : options.textAsOutlines ? TextMode::Outlines : TextMode::Text;
        RelayDevice relay(&target, mode, options.colours);
        {
            QPainter painter(&relay);
            paint(schematic, &painter, options.selectionOnly);
        }
        if (texts != nullptr)
            *texts = relay.texts();
    }
    return buffer.data();
}

QByteArray eps(Schematic* schematic, const Options& options)
{
    const QRect a = area(schematic, options.selectionOnly);
    if (a.isEmpty())
        return {};
    const QRect page(QPoint(0, 0), a.size());
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    {
        exportdevices::EpsDevice device(&buffer, page.size(), title(schematic));
        QPainter target(&device);
        if (!options.transparent)
            target.fillRect(page, Qt::white);
        RelayDevice relay(&target, TextMode::Outlines, options.colours);
        QPainter painter(&relay);
        paint(schematic, &painter, options.selectionOnly);
    }
    return buffer.data();
}

QByteArray pdfTex(const QList<TextRun>& texts, QSize size, const QString& pdfName)
{
    const double width = std::max(1, size.width());
    const double height = std::max(1, size.height());
    QString out;
    out += QStringLiteral("%% Creator: %1\n").arg(creator());
    out += QStringLiteral(
        "%% The text of %1, set by LaTeX. In the document (graphicx and xcolor loaded):\n"
        "%%   \\def\\svgwidth{\\columnwidth}\n"
        "%%   \\input{%2}\n"
        "%% \\svgwidth is the width of the picture; without it, its own size. The text is\n"
        "%% as large as in the drawing; \\def\\qucsdocumentfont{} before \\input keeps the\n"
        "%% document's size instead.\n")
        .arg(pdfName, QFileInfo(pdfName).completeBaseName() + QStringLiteral(".pdf_tex"));
    out += QStringLiteral(
        "\\begingroup%\n"
        "  \\makeatletter%\n"
        "  \\providecommand\\color[2][]{%\n"
        "    \\errmessage{(Qucs-S) the text has colours: load the package xcolor}%\n"
        "    \\renewcommand\\color[2][]{}%\n"
        "  }%\n"
        "  \\providecommand\\rotatebox[2]{#2}%\n"
        "  \\ifx\\qucsdocumentfont\\undefined%\n"
        "    \\def\\qucssize#1{\\fontsize{#1\\unitlength}{1.2\\dimexpr#1\\unitlength\\relax}\\selectfont}%\n"
        "  \\else%\n"
        "    \\def\\qucssize#1{}%\n"
        "  \\fi%\n"
        "  \\ifx\\svgwidth\\undefined%\n"
        "    \\setlength{\\unitlength}{%1bp}%\n"
        "    \\ifx\\svgscale\\undefined%\n"
        "      \\relax%\n"
        "    \\else%\n"
        "      \\setlength{\\unitlength}{\\unitlength * \\real{\\svgscale}}%\n"
        "    \\fi%\n"
        "  \\else%\n"
        "    \\setlength{\\unitlength}{\\svgwidth}%\n"
        "  \\fi%\n"
        "  \\global\\let\\svgwidth\\undefined%\n"
        "  \\global\\let\\svgscale\\undefined%\n"
        "  \\makeatother%\n"
        "  \\begin{picture}(1,%2)%\n"
        "    \\put(0,0){\\includegraphics[width=\\unitlength,page=1]{%3}}%\n")
        .arg(number(width * 0.75), number(height / width), pdfName);
    for (const TextRun& run : texts) {
        if (run.text.trimmed().isEmpty())
            continue;
        // The start of the baseline, from the bottom left, in widths; the
        // size of the letters in widths too.
        const double pixels = run.font.pixelSize() > 0 ? run.font.pixelSize()
                                                       : run.font.pointSizeF() * 96.0 / 72.0;
        QString style = QStringLiteral("\\qucssize{%1}").arg(number(pixels / width));
        if (run.font.bold())
            style += QLatin1String("\\bfseries");
        if (run.font.italic())
            style += QLatin1String("\\itshape");
        QString box = QStringLiteral("\\makebox(0,0)[lb]{\\smash{%1{}%2}}").arg(style, latex(run.text));
        if (std::abs(run.angle) > 0.01)
            box = QStringLiteral("\\rotatebox{%1}{%2}").arg(number(run.angle), box);
        out += QStringLiteral("    \\put(%1,%2){\\color[rgb]{%3,%4,%5}%6}%\n")
                   .arg(number(run.position.x() / width), number((height - run.position.y()) / width),
                        number(run.colour.redF()), number(run.colour.greenF()),
                        number(run.colour.blueF()), box);
    }
    out += QStringLiteral("  \\end{picture}%\n\\endgroup%\n");
    return out.toUtf8();
}

QString pdfOf(const QString& pdfTexFileName)
{
    const QString ext = QFileInfo(pdfTexFileName).suffix();
    if (ext.compare(QLatin1String("pdf_tex"), Qt::CaseInsensitive) == 0)
        return pdfTexFileName.left(pdfTexFileName.size() - ext.size()) + QStringLiteral("pdf");
    return pdfTexFileName + QStringLiteral(".pdf");
}

bool write(Schematic* schematic, const QString& fileName, Format format,
           const Options& given, QString* error)
{
    Options options = given;
    if (!hasTransparency(format))
        options.transparent = false;
    const QRect a = area(schematic, options.selectionOnly);
    if (a.isEmpty()) {
        if (error)
            *error = options.selectionOnly ? tr("Nothing is selected.") : tr("The document is empty.");
        return false;
    }
    switch (format) {
    case Format::Png:
    case Format::Jpeg:
    case Format::Bmp:
    case Format::Tiff:
    case Format::Webp:
        return writeImage(schematic, fileName, format, options, error);
    case Format::Svg:
        return save(fileName, svg(schematic, options), error);
    case Format::Pdf:
        return save(fileName, pdf(schematic, options), error);
    case Format::Eps:
        return save(fileName, eps(schematic, options), error);
    case Format::PdfTex: {
        const QString pdfFile = pdfOf(fileName);
        QList<TextRun> texts;
        const QByteArray drawing = pdf(schematic, options, &texts);
        return save(pdfFile, drawing, error)
            && save(fileName, pdfTex(texts, a.size(), QFileInfo(pdfFile).fileName()), error);
    }
    }
    return false;
}

QMimeData* mimeData(Schematic* schematic, const Options& options)
{
    auto* data = new QMimeData;
    data->setImageData(image(schematic, options));
    Options outlines = options;
    outlines.textAsOutlines = true;
    data->setData(QStringLiteral("image/svg+xml"), svg(schematic, outlines));
    data->setData(QStringLiteral("application/pdf"), pdf(schematic, options));
    return data;
}

} // namespace qucs_s::graphicsexport
