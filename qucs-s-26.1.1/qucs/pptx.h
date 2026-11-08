/*
 * pptx.h - PowerPoint presentations (.pptx) read and drawn: each slide as
 *          its master, its layout and itself give it - shapes, text,
 *          pictures, tables, groups, charts - with its notes and its text
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PPTX_H
#define QUCS_PPTX_H

#include <QImage>
#include <QList>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <memory>

class QPainter;

namespace qucs_s::pptx {

/// Whether a file of that name is a presentation read here: Office Open
/// XML's (.pptx, .pptm, .ppsx, .ppsm, .potx, .potm). Not PowerPoint 97's
/// binary .ppt.
bool isPresentationFile(const QString& name);

/// What a slide holds, as read: its text and notes, and what of it is not
/// drawn as PowerPoint draws it.
struct SlideInfo {
    QString title;            ///< its title placeholder's text (empty: none)
    QStringList paragraphs;   ///< its text, paragraph by paragraph, in the order drawn (a table's by row)
    QString notes;            ///< the speaker notes
    bool hidden = false;      ///< left out of a slide show
    int pictures = 0;
    int tables = 0;
    int charts = 0;
    /// What is drawn otherwise, or not at all: a picture in EMF (a box in
    /// its place), a chart of a kind not drawn, a shape of a kind drawn as
    /// a rectangle.
    QStringList notShown;
};

/// A part of a slide where it is drawn, in points from the slide's top
/// left: a paragraph of text, a table's cell, a picture, a chart, a shape.
/// What a mark drawn on the slide is read against (markup.h).
struct SlidePart {
    enum Kind { Paragraph, Cell, Picture, Chart, Shape };
    Kind kind = Shape;
    QPolygonF outline;   ///< as drawn, turned with what holds it
    /// A paragraph's or a cell's text; a picture's alt text, else its name;
    /// a chart's title; a shape's name.
    QString text;
    /// A paragraph's or a cell's words, each where it is drawn.
    QList<std::pair<QString, QPolygonF>> words;
};

/// A presentation read: its slides, drawn on demand.
class Presentation
{
public:
    /// \a bytes (a .pptx file's) read; null and why when they are not a
    /// presentation (not a ZIP archive, no presentation part, no slides
    /// readable - one that has none is read, with none).
    static std::shared_ptr<Presentation> read(const QByteArray& bytes, QString* error = nullptr);
    ~Presentation();
    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;

    /// A slide's size, in points (a 16:9 slide 960 x 540).
    QSizeF slideSize() const;
    int slideCount() const;
    /// Slide \a index's text, notes and what of it is not drawn (from 0).
    const SlideInfo& slide(int index) const;
    /// Its title, as its file's properties give it (empty: none).
    QString title() const;

    /// Slide \a index drawn \a scale pixels a point (1: 72 pixels an inch),
    /// on its own background. A null image for a slide that is not there.
    QImage render(int index, qreal scale) const;
    /// The part \a area of slide \a index drawn \a scale pixels a point (in
    /// pixels from that drawing's top left; what falls off the slide left
    /// out): no more than that drawn, so a slide too large for one image is
    /// drawn where it is seen. A null image where none of it is left, or
    /// it is more than 120 million pixels.
    QImage render(int index, qreal scale, const QRect& area) const;
    /// A slide drawn \a scale pixels a point: its size in pixels.
    QSize pixels(qreal scale) const;
    /// The pictures slide \a index shows decoded as drawing it \a scale
    /// pixels a point will want them, so that drawing finds them ready. The
    /// one call that may run on another thread than the drawing's (a
    /// picture is decoded once, the others wait for it).
    void prepare(int index, qreal scale) const;
    /// Slide \a index drawn by \a painter into \a target (in its
    /// coordinates; the slide's aspect kept, in the middle - placed()):
    /// through an image at the resolution \a painter's device has there.
    void render(int index, QPainter* painter, const QRectF& target) const;
    /// Where a slide drawn into \a target lands: its aspect kept, in the
    /// middle.
    QRectF placed(const QRectF& target) const;
    /// Slide \a index's parts, where they are drawn - its paragraphs and
    /// their words, its tables' cells, its pictures, charts and shapes, those
    /// underneath first.
    QList<SlidePart> parts(int index) const;

    /// The slides whose text has \a text (case aside), and how often each.
    QList<std::pair<int, int>> find(const QString& text) const;

    struct Data;

private:
    Presentation();
    std::unique_ptr<Data> d;
};

} // namespace qucs_s::pptx

#endif // QUCS_PPTX_H
