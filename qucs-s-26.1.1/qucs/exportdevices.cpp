/*
 * exportdevices.cpp - the paint devices behind File > Export
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "exportdevices.h"

#include <QDateTime>
#include <QIODevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTextItem>
#include <QtMath>

#include <algorithm>
#include <climits>
#include <cmath>

namespace qucs_s::exportdevices {

namespace {

// A metric of another device: QPaintDevice::metric() is protected, a
// pointer to it is not.
struct MetricOf : QPaintDevice {
    static int get(const QPaintDevice* device, PaintDeviceMetric metric)
    {
        return (device->*(&MetricOf::metric))(metric);
    }
};

QBrush exported(QBrush brush, Colours colours, Role role)
{
    if (colours == Colours::Colour)
        return brush;
    switch (brush.style()) {
    case Qt::NoBrush:
        return brush;
    case Qt::LinearGradientPattern:
    case Qt::RadialGradientPattern:
    case Qt::ConicalGradientPattern: {
        QGradient gradient = *brush.gradient();
        QGradientStops stops = gradient.stops();
        for (auto& stop : stops)
            stop.second = exported(stop.second, colours, role);
        gradient.setStops(stops);
        QBrush out(gradient);
        out.setTransform(brush.transform());
        return out;
    }
    case Qt::TexturePattern: {
        QBrush out(exported(brush.textureImage(), colours));
        out.setTransform(brush.transform());
        return out;
    }
    default:
        brush.setColor(exported(brush.color(), colours, role));
        return brush;
    }
}

QPen exported(QPen pen, Colours colours)
{
    if (colours != Colours::Colour)
        pen.setBrush(exported(pen.brush(), colours, Role::Line));
    return pen;
}

} // namespace

QColor exported(const QColor& colour, Colours colours, Role role)
{
    if (colours == Colours::Colour || !colour.isValid())
        return colour;
    const QRgb rgb = colour.rgb();
    const int grey = qGray(rgb);
    int level = grey;
    if (colours == Colours::Monochrome) {
        if (role == Role::Fill) {
            level = grey < 128 ? 0 : 255;
        } else {
            const int spread = std::max({qRed(rgb), qGreen(rgb), qBlue(rgb)})
                             - std::min({qRed(rgb), qGreen(rgb), qBlue(rgb)});
            level = grey >= 240 && spread < 32 ? 255 : 0;
        }
    }
    QColor out(level, level, level);
    out.setAlpha(colour.alpha());
    return out;
}

QImage exported(const QImage& image, Colours colours)
{
    if (colours == Colours::Colour || image.isNull())
        return image;
    QImage out = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < out.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const int grey = qGray(line[x]);
            const int level = colours == Colours::Grayscale ? grey : (grey < 128 ? 0 : 255);
            line[x] = qRgba(level, level, level, qAlpha(line[x]));
        }
    }
    return out;
}

// ------------------------------------------------------------------ relay

class RelayEngine : public QPaintEngine
{
public:
    RelayEngine(QPainter* target, TextMode text, Colours colours, const QString& family)
        : QPaintEngine(AllFeatures), m_target(target), m_text(text), m_colours(colours),
          m_family(family)
    {
    }

    QList<TextRun> texts() const { return m_texts; }

    bool begin(QPaintDevice*) override
    {
        // The painter on this device starts from the defaults; so does the
        // target, and every change is passed on.
        m_target->save();
        m_target->setPen(QPen());
        m_target->setBrush(Qt::NoBrush);
        m_target->resetTransform();
        m_target->setClipping(false);
        m_target->setOpacity(1.0);
        m_target->setBackgroundMode(Qt::TransparentMode);
        setActive(true);
        return true;
    }

    bool end() override
    {
        m_target->restore();
        setActive(false);
        return true;
    }

    Type type() const override { return User; }

    void updateState(const QPaintEngineState& state) override
    {
        const DirtyFlags flags = state.state();
        // A clip is given in the coordinates of the transformation it was
        // set with: the transformation goes first.
        if (flags & (DirtyTransform | DirtyClipPath | DirtyClipRegion))
            m_target->setTransform(state.transform());
        if (flags & DirtyPen)
            m_target->setPen(exported(state.pen(), m_colours));
        if (flags & DirtyBrush)
            m_target->setBrush(exported(state.brush(), m_colours, m_glyphs ? Role::Line : Role::Fill));
        if (flags & DirtyBrushOrigin)
            m_target->setBrushOrigin(state.brushOrigin());
        if (flags & DirtyBackground)
            m_target->setBackground(exported(state.backgroundBrush(), m_colours, Role::Fill));
        if (flags & DirtyBackgroundMode)
            m_target->setBackgroundMode(state.backgroundMode());
        if (flags & DirtyFont)
            m_target->setFont(state.font());
        if (flags & DirtyHints) {
            m_target->setRenderHints(m_target->renderHints(), false);
            m_target->setRenderHints(state.renderHints(), true);
        }
        if (flags & DirtyOpacity)
            m_target->setOpacity(state.opacity());
        if (flags & DirtyClipPath) {
            if (state.clipOperation() == Qt::NoClip)
                m_target->setClipping(false);
            else
                m_target->setClipPath(state.clipPath(), state.clipOperation());
        }
        if (flags & DirtyClipRegion) {
            if (state.clipOperation() == Qt::NoClip)
                m_target->setClipping(false);
            else
                m_target->setClipRegion(state.clipRegion(), state.clipOperation());
        }
        // A restore to a state without a clip says NoClip and still has
        // clipping enabled: the clip is off.
        if (flags & DirtyClipEnabled)
            m_target->setClipping(state.isClipEnabled() && state.clipOperation() != Qt::NoClip);
    }

    void drawPath(const QPainterPath& path) override { m_target->drawPath(path); }

    void drawPolygon(const QPointF* points, int count, PolygonDrawMode mode) override
    {
        switch (mode) {
        case PolylineMode: m_target->drawPolyline(points, count); break;
        case ConvexMode:   m_target->drawConvexPolygon(points, count); break;
        case OddEvenMode:  m_target->drawPolygon(points, count, Qt::OddEvenFill); break;
        case WindingMode:  m_target->drawPolygon(points, count, Qt::WindingFill); break;
        }
    }

    void drawPolygon(const QPoint* points, int count, PolygonDrawMode mode) override
    {
        switch (mode) {
        case PolylineMode: m_target->drawPolyline(points, count); break;
        case ConvexMode:   m_target->drawConvexPolygon(points, count); break;
        case OddEvenMode:  m_target->drawPolygon(points, count, Qt::OddEvenFill); break;
        case WindingMode:  m_target->drawPolygon(points, count, Qt::WindingFill); break;
        }
    }

    void drawLines(const QLineF* lines, int count) override { m_target->drawLines(lines, count); }
    void drawLines(const QLine* lines, int count) override { m_target->drawLines(lines, count); }
    void drawRects(const QRectF* rects, int count) override { m_target->drawRects(rects, count); }
    void drawRects(const QRect* rects, int count) override { m_target->drawRects(rects, count); }
    void drawEllipse(const QRectF& rect) override { m_target->drawEllipse(rect); }
    void drawEllipse(const QRect& rect) override { m_target->drawEllipse(rect); }
    void drawPoints(const QPointF* points, int count) override { m_target->drawPoints(points, count); }
    void drawPoints(const QPoint* points, int count) override { m_target->drawPoints(points, count); }

    void drawPixmap(const QRectF& rect, const QPixmap& pixmap, const QRectF& source) override
    {
        if (m_colours == Colours::Colour)
            m_target->drawPixmap(rect, pixmap, source);
        else
            m_target->drawImage(rect, exported(pixmap.toImage(), m_colours), source);
    }

    void drawImage(const QRectF& rect, const QImage& image, const QRectF& source,
                   Qt::ImageConversionFlags flags) override
    {
        m_target->drawImage(rect, exported(image, m_colours), source, flags);
    }

    void drawTiledPixmap(const QRectF& rect, const QPixmap& pixmap, const QPointF& offset) override
    {
        if (m_colours == Colours::Colour)
            m_target->drawTiledPixmap(rect, pixmap, offset);
        else
            m_target->drawTiledPixmap(rect, QPixmap::fromImage(exported(pixmap.toImage(), m_colours)), offset);
    }

    void drawTextItem(const QPointF& p, const QTextItem& item) override
    {
        switch (m_text) {
        case TextMode::Outlines:
            // QPaintEngine fills the outlines of the glyphs with the pen's
            // brush through this device's painter: coloured as a line.
            m_glyphs = true;
            QPaintEngine::drawTextItem(p, item);
            m_glyphs = false;
            m_target->setBrush(exported(state->brush(), m_colours, Role::Fill));
            return;
        case TextMode::Collect: {
            const QTransform& m = state->transform();
            TextRun run;
            run.position = m.map(p);
            run.angle = qRadiansToDegrees(std::atan2(-m.m12(), m.m11()));
            run.text = item.text();
            run.font = item.font();
            run.colour = exported(state->pen().color(), m_colours, Role::Line);
            m_texts.append(run);
            return;
        }
        case TextMode::Text:
            if (!m_family.isEmpty() && item.font().family().startsWith(QLatin1Char('.'))) {
                QFont font = item.font();
                font.setFamilies({m_family});
                m_target->save();
                m_target->setFont(font);
                m_target->drawText(p, item.text());
                m_target->restore();
            } else {
                m_target->drawTextItem(p, item);
            }
            return;
        }
    }

private:
    QPainter* m_target;
    TextMode m_text;
    Colours m_colours;
    QString m_family;
    bool m_glyphs = false;
    QList<TextRun> m_texts;
};

RelayDevice::RelayDevice(QPainter* target, TextMode text, Colours colours,
                         const QString& portableFamily)
    : m_target(target),
      m_engine(std::make_unique<RelayEngine>(target, text, colours, portableFamily))
{
}

RelayDevice::~RelayDevice() = default;

QPaintEngine* RelayDevice::paintEngine() const
{
    return m_engine.get();
}

QList<TextRun> RelayDevice::texts() const
{
    return m_engine->texts();
}

int RelayDevice::metric(PaintDeviceMetric metric) const
{
    return MetricOf::get(m_target->device(), metric);
}

// -------------------------------------------------------------- PostScript

namespace {

QByteArray num(double v)
{
    if (!std::isfinite(v) || std::abs(v) < 0.0005)
        return QByteArrayLiteral("0");
    QByteArray s = QByteArray::number(v, 'f', 3);
    while (s.endsWith('0'))
        s.chop(1);
    if (s.endsWith('.'))
        s.chop(1);
    return s;
}

QByteArray point(const QPointF& p)
{
    return num(p.x()) + ' ' + num(p.y());
}

// A DSC comment's text: printable ASCII.
QByteArray dscText(const QString& text)
{
    QByteArray out;
    for (const QChar c : text)
        out += c.unicode() >= 0x20 && c.unicode() < 0x7f ? char(c.unicode()) : '?';
    return out;
}

// The share of the pattern Qt::Dense<n>Pattern covers.
double coverage(Qt::BrushStyle style)
{
    switch (style) {
    case Qt::Dense1Pattern: return 0.94;
    case Qt::Dense2Pattern: return 0.88;
    case Qt::Dense3Pattern: return 0.63;
    case Qt::Dense4Pattern: return 0.50;
    case Qt::Dense5Pattern: return 0.37;
    case Qt::Dense6Pattern: return 0.12;
    case Qt::Dense7Pattern: return 0.06;
    default: return 1.0;
    }
}

} // namespace

class EpsEngine : public QPaintEngine
{
public:
    EpsEngine(QIODevice* out, QSize size, const QString& title)
        : QPaintEngine(PrimitiveTransform | PatternTransform | PixmapTransform | PainterPaths
                       | PatternBrush | AlphaBlend | Antialiasing | ConstantOpacity),
          m_out(out), m_size(size), m_title(title)
    {
    }

    bool begin(QPaintDevice*) override
    {
        m_body.clear();
        m_transform = QTransform();
        m_pen = QPen();
        m_brush = QBrush();
        m_opacity = 1.0;
        m_clipEnabled = false;
        m_hasClip = false;
        m_clip = QPainterPath();
        setActive(true);
        return true;
    }

    bool end() override
    {
        setActive(false);
        const double w = m_size.width() * 0.75;
        const double h = m_size.height() * 0.75;
        QByteArray out;
        out += "%!PS-Adobe-3.0 EPSF-3.0\n";
        out += "%%BoundingBox: 0 0 " + QByteArray::number(qCeil(w)) + ' '
             + QByteArray::number(qCeil(h)) + '\n';
        out += "%%HiResBoundingBox: 0 0 " + num(w) + ' ' + num(h) + '\n';
        out += "%%Creator: Qucs-S\n";
        if (!m_title.isEmpty())
            out += "%%Title: " + dscText(m_title) + '\n';
        out += "%%CreationDate: " + QDateTime::currentDateTime().toString(Qt::ISODate).toLatin1() + '\n';
        out += "%%LanguageLevel: 2\n%%Pages: 1\n%%DocumentData: Clean7Bit\n%%EndComments\n";
        out += "%%BeginProlog\n"
               "/qucs_s 8 dict def qucs_s begin\n"
               "/m {moveto} bind def /l {lineto} bind def /c {curveto} bind def\n"
               "/h {closepath} bind def /rg {setrgbcolor} bind def\n"
               "end\n"
               "%%EndProlog\n";
        // A unit of the device is 1/96 inch, y downwards.
        out += "%%Page: 1 1\nqucs_s begin\nsave\n0 " + num(h) + " translate 0.75 -0.75 scale\n";
        out += m_body;
        out += "restore\nend\nshowpage\n%%Trailer\n%%EOF\n";
        m_body.clear();
        return m_out != nullptr && m_out->write(out) == out.size();
    }

    Type type() const override { return User; }

    void updateState(const QPaintEngineState& state) override
    {
        const DirtyFlags flags = state.state();
        if (flags & DirtyTransform)
            m_transform = state.transform();
        if (flags & DirtyPen)
            m_pen = state.pen();
        if (flags & DirtyBrush)
            m_brush = state.brush();
        if (flags & DirtyOpacity)
            m_opacity = state.opacity();
        if (flags & DirtyClipEnabled)
            m_clipEnabled = state.isClipEnabled();
        if (flags & DirtyClipPath)
            clip(state.transform().map(state.clipPath()), state.clipOperation());
        if (flags & DirtyClipRegion) {
            QPainterPath path;
            path.addRegion(state.clipRegion());
            clip(state.transform().map(path), state.clipOperation());
        }
    }

    void drawPath(const QPainterPath& path) override { draw(path, true); }

    void drawPolygon(const QPointF* points, int count, PolygonDrawMode mode) override
    {
        if (count < 2)
            return;
        QPainterPath path(points[0]);
        for (int i = 1; i < count; ++i)
            path.lineTo(points[i]);
        if (mode == PolylineMode) {
            draw(path, false);
            return;
        }
        path.closeSubpath();
        path.setFillRule(mode == OddEvenMode ? Qt::OddEvenFill : Qt::WindingFill);
        draw(path, true);
    }

    void drawPixmap(const QRectF& rect, const QPixmap& pixmap, const QRectF& source) override
    {
        drawImage(rect, pixmap.toImage(), source, Qt::AutoColor);
    }

    void drawImage(const QRectF& rect, const QImage& image, const QRectF& source,
                   Qt::ImageConversionFlags) override
    {
        if (clippedAway() || rect.isEmpty())
            return;
        const QImage part = image.copy(source.toAlignedRect()).convertToFormat(QImage::Format_ARGB32);
        if (part.isNull())
            return;
        const QByteArray w = QByteArray::number(part.width());
        const QByteArray h = QByteArray::number(part.height());
        const QTransform& m = m_transform;
        const bool clipped = beginClip();
        m_body += "gsave\n[" + num(m.m11()) + ' ' + num(m.m12()) + ' ' + num(m.m21()) + ' '
                + num(m.m22()) + ' ' + num(m.dx()) + ' ' + num(m.dy()) + "] concat\n"
                + point(rect.topLeft()) + " translate " + num(rect.width()) + ' '
                + num(rect.height()) + " scale\n"
                + "/DeviceRGB setcolorspace\n"
                + "<< /ImageType 1 /Width " + w + " /Height " + h
                + " /BitsPerComponent 8 /Decode [0 1 0 1 0 1] /ImageMatrix [" + w + " 0 0 " + h
                + " 0 0] /DataSource currentfile /ASCIIHexDecode filter >>\nimage\n";
        static const char hex[] = "0123456789abcdef";
        int column = 0;
        for (int y = 0; y < part.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(part.constScanLine(y));
            for (int x = 0; x < part.width(); ++x) {
                // On white paper, as the colours are.
                const double a = qAlpha(line[x]) / 255.0 * m_opacity;
                for (const int v : {qRed(line[x]), qGreen(line[x]), qBlue(line[x])}) {
                    const int blended = qBound(0, qRound(a * v + (1 - a) * 255), 255);
                    m_body += hex[blended >> 4];
                    m_body += hex[blended & 15];
                }
                if (++column == 32) {
                    m_body += '\n';
                    column = 0;
                }
            }
        }
        m_body += ">\ngrestore\n";
        if (clipped)
            m_body += "grestore\n";
    }

private:
    void clip(const QPainterPath& path, Qt::ClipOperation operation)
    {
        switch (operation) {
        case Qt::NoClip:
            m_hasClip = false;
            m_clip = QPainterPath();
            return;
        case Qt::ReplaceClip:
            m_clip = path;
            break;
        case Qt::IntersectClip:
            m_clip = m_hasClip ? m_clip.intersected(path) : path;
            break;
        }
        m_hasClip = true;
        m_clipEnabled = true;
    }

    bool clipping() const { return m_clipEnabled && m_hasClip; }
    bool clippedAway() const { return clipping() && m_clip.isEmpty(); }

    bool beginClip()
    {
        if (!clipping())
            return false;
        m_body += "gsave\n" + ops(m_clip)
                + (m_clip.fillRule() == Qt::WindingFill ? "clip" : "eoclip") + " newpath\n";
        return true;
    }

    // What is seen of a colour, as "r g b rg", on white paper.
    QByteArray colour(const QColor& c) const
    {
        const double a = c.alphaF() * m_opacity;
        const auto on = [a](double v) { return a * v + (1 - a); };
        return num(on(c.redF())) + ' ' + num(on(c.greenF())) + ' ' + num(on(c.blueF())) + " rg\n";
    }

    bool visible(const QColor& c) const { return c.isValid() && c.alpha() > 0 && m_opacity > 0; }

    // The operators that build a path given in device coordinates. A
    // subpath that returns to its start is closed, so that it joins there.
    static QByteArray ops(const QPainterPath& path)
    {
        QByteArray out;
        QPointF start, last;
        int segments = 0;
        const auto close = [&] {
            if (segments > 1 && QLineF(start, last).length() < 1e-6)
                out += "h\n";
        };
        for (int i = 0; i < path.elementCount(); ++i) {
            const QPainterPath::Element& e = path.elementAt(i);
            switch (e.type) {
            case QPainterPath::MoveToElement:
                close();
                start = last = e;
                segments = 0;
                out += point(e) + " m\n";
                break;
            case QPainterPath::LineToElement:
                last = e;
                ++segments;
                out += point(e) + " l\n";
                break;
            case QPainterPath::CurveToElement:
                if (i + 2 < path.elementCount()) {
                    const QPointF c2 = path.elementAt(i + 1);
                    const QPointF end = path.elementAt(i + 2);
                    out += point(e) + ' ' + point(c2) + ' ' + point(end) + " c\n";
                    last = end;
                    ++segments;
                    i += 2;
                }
                break;
            case QPainterPath::CurveToDataElement:
                break;
            }
        }
        close();
        return out;
    }

    void draw(const QPainterPath& path, bool fillIt)
    {
        if (path.isEmpty() || clippedAway())
            return;
        const QPainterPath device = m_transform.map(path);
        const bool clipped = beginClip();
        if (fillIt)
            fill(device);
        stroke(device);
        if (clipped)
            m_body += "grestore\n";
    }

    void fill(const QPainterPath& path)
    {
        const Qt::BrushStyle style = m_brush.style();
        const QByteArray op = path.fillRule() == Qt::WindingFill ? "fill\n" : "eofill\n";
        QColor c = m_brush.color();
        switch (style) {
        case Qt::NoBrush:
            return;
        case Qt::HorPattern:
        case Qt::VerPattern:
        case Qt::CrossPattern:
        case Qt::BDiagPattern:
        case Qt::FDiagPattern:
        case Qt::DiagCrossPattern:
            hatch(path, style, c);
            return;
        case Qt::LinearGradientPattern:
        case Qt::RadialGradientPattern:
        case Qt::ConicalGradientPattern: {
            // Painted by QPainter as an image; if one comes anyway, the
            // colour halfway.
            const QGradientStops stops = m_brush.gradient()->stops();
            if (!stops.isEmpty())
                c = stops.at(stops.size() / 2).second;
            break;
        }
        case Qt::TexturePattern: {
            const QImage texture = m_brush.textureImage().scaled(1, 1, Qt::IgnoreAspectRatio,
                                                                 Qt::SmoothTransformation);
            if (!texture.isNull())
                c = QColor::fromRgba(texture.pixel(0, 0));
            break;
        }
        default:
            c.setAlphaF(c.alphaF() * coverage(style));
            break;
        }
        if (visible(c))
            m_body += colour(c) + ops(path) + op;
    }

    // Lines 8 units apart - Qt's patterns repeat every 8 pixels - clipped
    // to the shape, lined up across shapes.
    void hatch(const QPainterPath& path, Qt::BrushStyle style, const QColor& c)
    {
        if (!visible(c))
            return;
        const QRectF box = path.boundingRect();
        const double step = 8.0;
        if (box.width() + box.height() > 400000.0)
            return;
        QByteArray lines;
        const auto line = [&](QPointF a, QPointF b) { lines += point(a) + " m " + point(b) + " l\n"; };
        if (style == Qt::HorPattern || style == Qt::CrossPattern)
            for (double y = std::floor(box.top() / step) * step; y <= box.bottom(); y += step)
                line({box.left(), y}, {box.right(), y});
        if (style == Qt::VerPattern || style == Qt::CrossPattern)
            for (double x = std::floor(box.left() / step) * step; x <= box.right(); x += step)
                line({x, box.top()}, {x, box.bottom()});
        if (style == Qt::BDiagPattern || style == Qt::DiagCrossPattern)   // "/": x + y = k
            for (double k = std::floor((box.left() + box.top()) / step) * step;
                 k <= box.right() + box.bottom(); k += step)
                line({box.left(), k - box.left()}, {box.right(), k - box.right()});
        if (style == Qt::FDiagPattern || style == Qt::DiagCrossPattern)   // "\": y - x = k
            for (double k = std::floor((box.top() - box.right()) / step) * step;
                 k <= box.bottom() - box.left(); k += step)
                line({box.left(), box.left() + k}, {box.right(), box.right() + k});
        m_body += "gsave\n" + ops(path) + (path.fillRule() == Qt::WindingFill ? "clip" : "eoclip")
                + " newpath\n" + colour(c) + "1 setlinewidth 0 setlinecap [] 0 setdash\n" + lines
                + "stroke\ngrestore\n";
    }

    void stroke(const QPainterPath& path)
    {
        if (m_pen.style() == Qt::NoPen)
            return;
        const QColor c = m_pen.color();
        if (!visible(c))
            return;
        // A cosmetic pen is as wide on the device whatever the scale.
        double width = m_pen.widthF();
        if (m_pen.isCosmetic())
            width = width > 0 ? width : 1.0;
        else
            width *= std::sqrt(std::abs(m_transform.determinant()));
        QByteArray s = "gsave\n" + colour(c) + num(width) + " setlinewidth ";
        switch (m_pen.capStyle()) {
        case Qt::FlatCap:  s += "0 setlinecap "; break;
        case Qt::RoundCap: s += "1 setlinecap "; break;
        default:           s += "2 setlinecap "; break;
        }
        switch (m_pen.joinStyle()) {
        case Qt::RoundJoin: s += "1 setlinejoin "; break;
        case Qt::BevelJoin: s += "2 setlinejoin "; break;
        default:
            // PostScript measures a miter across the join, Qt from the
            // joint point.
            s += "0 setlinejoin " + num(std::max(1.0, 2 * m_pen.miterLimit())) + " setmiterlimit ";
            break;
        }
        if (m_pen.style() != Qt::SolidLine) {
            const double unit = std::max(1.0, width);   // the pattern is in widths
            s += '[';
            for (const double v : m_pen.dashPattern())
                s += num(v * unit) + ' ';
            s += "] " + num(m_pen.dashOffset() * unit) + " setdash";
        }
        m_body += s + '\n' + ops(path) + "stroke\ngrestore\n";
    }

    QIODevice* m_out;
    QSize m_size;
    QString m_title;
    QByteArray m_body;
    QTransform m_transform;
    QPen m_pen;
    QBrush m_brush;
    double m_opacity = 1.0;
    bool m_clipEnabled = false;
    bool m_hasClip = false;
    QPainterPath m_clip;   // in device coordinates
};

EpsDevice::EpsDevice(QIODevice* out, QSize size, const QString& title)
    : m_size(size), m_engine(std::make_unique<EpsEngine>(out, size, title))
{
}

EpsDevice::~EpsDevice() = default;

QPaintEngine* EpsDevice::paintEngine() const
{
    return m_engine.get();
}

int EpsDevice::metric(PaintDeviceMetric metric) const
{
    switch (metric) {
    case PdmWidth:
        return m_size.width();
    case PdmHeight:
        return m_size.height();
    case PdmWidthMM:
        return qRound(m_size.width() * 25.4 / 96.0);
    case PdmHeightMM:
        return qRound(m_size.height() * 25.4 / 96.0);
    case PdmNumColors:
        return INT_MAX;
    case PdmDepth:
        return 32;
    case PdmDpiX:
    case PdmDpiY:
    case PdmPhysicalDpiX:
    case PdmPhysicalDpiY:
        return 96;
    case PdmDevicePixelRatio:
        return 1;
    case PdmDevicePixelRatioScaled:
        return qRound(devicePixelRatioFScale());
    default:
        return QPaintDevice::metric(metric);
    }
}

} // namespace qucs_s::exportdevices
