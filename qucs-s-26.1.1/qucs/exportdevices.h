/*
 * exportdevices.h - the paint devices behind File > Export: one that
 * draws on another painter - text as it is, as outlines or kept for a
 * LaTeX overlay, colours as they are, in grey or in black and white - and
 * one that writes Encapsulated PostScript, which Qt 6 no longer does
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_EXPORTDEVICES_H
#define QUCS_EXPORTDEVICES_H

#include <QColor>
#include <QFont>
#include <QImage>
#include <QList>
#include <QPaintDevice>
#include <QPointF>
#include <QSize>
#include <QString>

#include <memory>

class QIODevice;
class QPainter;

namespace qucs_s::exportdevices {

enum class Colours {
    Colour,
    Grayscale,
    Monochrome    ///< black lines and text; a fill black when dark, white otherwise
};

/// What a colour is used for: in black and white a line (or text) is
/// black unless it is white, a fill is white unless it is dark.
enum class Role { Line, Fill };

/// \a colour as it is exported.
QColor exported(const QColor& colour, Colours colours, Role role);
/// \a image as it is exported (its alpha kept).
QImage exported(const QImage& image, Colours colours);

enum class TextMode {
    Text,       ///< as text, the target's own way
    Outlines,   ///< the outlines of the glyphs: looks the same everywhere
    Collect     ///< not drawn; kept for texts() (a LaTeX overlay)
};

/// A piece of text drawn, in the coordinates of the device.
struct TextRun {
    QPointF position;     ///< the start of its baseline
    double angle = 0;     ///< degrees, counterclockwise as seen
    QString text;
    QFont font;
    QColor colour;
};

class RelayEngine;

/// Draws everything painted on it through the painter given, which is
/// active on its own device; this device has that device's metrics.
class RelayDevice : public QPaintDevice
{
public:
    /// With TextMode::Text a font of a family no other system has (macOS's
    /// ".AppleSystemUIFont", named with a dot) is drawn in \a portableFamily
    /// when one is given - what an SVG needs, a PDF embeds the font.
    RelayDevice(QPainter* target, TextMode text, Colours colours,
                const QString& portableFamily = QString());
    ~RelayDevice() override;

    QPaintEngine* paintEngine() const override;
    /// The texts kept with TextMode::Collect, in the order drawn.
    QList<TextRun> texts() const;

protected:
    int metric(PaintDeviceMetric metric) const override;

private:
    QPainter* m_target;
    std::unique_ptr<RelayEngine> m_engine;
};

class EpsEngine;

/// One page of Encapsulated PostScript (LanguageLevel 2) written to \a out
/// when painting ends: a unit of the device is 1/96 inch (0.75 pt), the
/// bounding box its size. Text comes out as outlines.
class EpsDevice : public QPaintDevice
{
public:
    EpsDevice(QIODevice* out, QSize size, const QString& title = QString());
    ~EpsDevice() override;

    QPaintEngine* paintEngine() const override;

protected:
    int metric(PaintDeviceMetric metric) const override;

private:
    QSize m_size;
    std::unique_ptr<EpsEngine> m_engine;
};

} // namespace qucs_s::exportdevices

#endif
