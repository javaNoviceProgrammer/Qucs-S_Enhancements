/*
 * datadisplay.h - a schematic's or data display's page: its toolbar - the
 *                 zoom, a data display's data read again, the pen
 *                 (markup.h) - and what the pen drew on it, read against
 *                 what is on the page for Claude
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DATADISPLAY_H
#define QUCS_DATADISPLAY_H

#include <QJsonArray>
#include <QString>
#include <QWidget>

class QToolButton;
class Schematic;

namespace qucs_s::markup {
class PenButton;
}

namespace qucs_s::datadisplay {

/*!
 * The toolbar over a schematic's or data display's page, in the room the
 * page leaves above its canvas (Schematic::setMargins()): zoom out, zoom
 * in, all of it in sight, a data display's datasets read again; and on the
 * right, the pen - pressed,
 * a drag on the page draws on it with the pen in hand (its menu: the pen,
 * the highlighter, the eraser, colours, how broad, undo, clear), as in a
 * layout's and a presentation's tab.
 */
class Bar : public QWidget
{
    Q_OBJECT

public:
    explicit Bar(Schematic* page);
    markup::PenButton* penButton() const { return a_pen; }
    QToolButton* zoomOutButton() const { return a_zoomOut; }
    QToolButton* zoomInButton() const { return a_zoomIn; }
    QToolButton* fitButton() const { return a_fit; }
    QToolButton* reloadButton() const { return a_reload; }
    /// The pen's menu as the page's drawing is.
    void updatePen();
    /// The buttons as the page is: a data display's, or a schematic's.
    void updateKind();

protected:
    void changeEvent(QEvent* event) override;

private:
    void restyle();

    Schematic* a_page;
    markup::PenButton* a_pen = nullptr;
    QToolButton* a_zoomOut = nullptr;
    QToolButton* a_zoomIn = nullptr;
    QToolButton* a_fit = nullptr;
    QToolButton* a_reload = nullptr;
    QWidget* a_reloadGap = nullptr;
};

/// What the pen drew on \a page - its strokes on the page, then on its
/// symbol's ("page" says which: page or symbol) - read as one would read it
/// (markup::readPage()): for each stroke, in the order drawn, its number,
/// how it was drawn, its tool, colour and box, in the part's coordinates
/// (as get_schematic gives them, y down), what it is round, on or points
/// at - the components by their refs, the wires by their ends, net labels,
/// paintings and diagrams by their numbers (on the symbol, its paintings) -
/// and, for each diagram it is in ("in diagrams"), the diagram's number and
/// title, the values its points span on the axes ("x", "y", the right
/// axis' "y right"; a stacked diagram's "pane"), the traces it is round,
/// lies on or crosses with the values of theirs it takes in, and where its
/// tip or point is - its values and the nearest trace's point. With
/// \a points, its points simplified.
QJsonArray readDrawing(const Schematic* page, bool points = false);

/// The readings a sentence each, a line each, for a prompt: the latest
/// \a most, the earlier ones counted; those marked addressed left out, and
/// counted.
QString sentencesOf(const QJsonArray& readings, int most = 30);

} // namespace qucs_s::datadisplay

#endif
