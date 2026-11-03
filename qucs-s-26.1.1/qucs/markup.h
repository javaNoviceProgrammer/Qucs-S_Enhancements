/*
 * markup.h - pens to draw with on what a viewer shows - a layout's cell, a
 *            presentation's slide: their strokes, and the button the pen
 *            is picked with
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MARKUP_H
#define QUCS_MARKUP_H

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPolygonF>
#include <QString>
#include <QToolButton>

#include <functional>

class QAction;
class QMenu;
class QPainter;

/*!
 * Pens for the viewers. A stroke is held in the measures of what it is
 * drawn on - a layout's µm (y up), a slide's points from its top left - and
 * on which part of it (a cell, by its name; a slide, by its number), so it
 * pans and zooms with what is under it, and is drawn as broad as the pen
 * was at the zoom it was drawn at, a pixel at the least. The pen in hand
 * is every viewer's, and kept for the next time.
 */
namespace qucs_s::markup {

/// What draws: a pen's line; a highlighter's broad one, what is under it
/// showing through; or the eraser, which takes whole strokes away.
enum class Tool { Pen, Highlighter, Eraser };

/// The pen in hand: its tool, its colour, and how broad a line it draws,
/// pixels on the screen (a pen's, and a highlighter's of its own).
struct Pen {
    Tool tool = Tool::Pen;
    QColor color = QColor(0xe0, 0x1b, 0x24);
    double width = 3;
    double highlighterWidth = 16;
    /// How broad its tool draws, pixels.
    double drawingWidth() const { return tool == Tool::Highlighter ? highlighterWidth : width; }
};

/// A stroke drawn: its points, its part, its colour (a highlighter's let
/// through already) and how broad, in the measures of what it is on.
struct Stroke {
    QString on;
    QPolygonF points;
    QColor color;
    double width = 0;
    bool highlighter = false;
    /// Its points' box, grown by half its width.
    QRectF bounds() const;
};

/// The strokes drawn on a document, the latest last, each change to them
/// undone in turn.
class Drawing
{
public:
    const QList<Stroke>& strokes() const { return a_strokes; }
    bool isEmpty() const { return a_strokes.isEmpty(); }
    /// Those on \a part, the latest last.
    QList<Stroke> on(const QString& part) const;
    int count(const QString& part) const;
    /// The parts drawn on, as they came first.
    QStringList parts() const;
    void add(const Stroke& stroke);
    /// The latest stroke on \a part within \a reach of \a point (its own
    /// half width more) taken away, and whether there was one. The change
    /// is not kept for undo(): changed() is, with what the strokes were.
    bool eraseAt(const QString& part, QPointF point, double reach);
    /// What the strokes were before a change made of several - an eraser
    /// dragged - kept for undo().
    void changed(const QList<Stroke>& before);
    /// Those on \a part taken away (empty: every one); whether any were.
    bool clear(const QString& part = QString());
    /// The last change undone; whether there was one.
    bool undo();
    bool canUndo() const { return !a_undo.isEmpty(); }
    /// Moves on at each change, to a number no drawing has had (nought
    /// before the first): whatever was told of the drawing at one revision
    /// is told again only when it has moved on.
    quint64 revision() const { return a_revision; }

    static constexpr int MostUndone = 100;

private:
    void remember();
    QList<Stroke> a_strokes;
    QList<QList<Stroke>> a_undo;   // what the strokes were before each change
    quint64 a_revision = 0;
};

/// Draws \a strokes: \a toPixel takes a point of theirs to the painter's,
/// \a pixelsPerUnit pixels a measure of theirs. None under a pixel broad.
void paint(QPainter& p, const QList<Stroke>& strokes, const std::function<QPointF(QPointF)>& toPixel, double pixelsPerUnit);

/// A stroke begun with \a pen on \a part at \a point, \a pixelsPerUnit
/// pixels a measure at the zoom it is drawn at.
Stroke begin(const Pen& pen, const QString& part, QPointF point, double pixelsPerUnit);
/// How near to \a point an eraser reaches, pixels.
constexpr double EraserReach = 6;
/// The least move, pixels, a stroke takes a new point at.
constexpr double LeastStep = 1.5;

// ----------------------------------------------------------------------
// Reading what was drawn - for Claude, without a picture

/// How a stroke was drawn, as one reading it takes it: a dot; a ring round
/// something, back near where it began; a line, straight or nearly; an
/// arrow, a line with its head drawn at its end (in the stroke itself - a
/// V's one side or both, a triangle - or as a short stroke of its own
/// beside it); a corner, two straight arms at an angle - an L, a V, a
/// caret -, which marks what is at its point; or a mark of another shape -
/// a tick, a cross, a scribble.
enum class Gesture { Dot, Ring, Line, Arrow, Mark, Arrowhead, Corner };
QString nameOf(Gesture gesture);
/// \a stroke's gesture, alone; an arrow's tip in \a tip (a line's end, a
/// corner's point).
Gesture gestureOf(const Stroke& stroke, QPointF* tip = nullptr);
/// The gestures of \a strokes, drawn on one part in that order: a line
/// with a short stroke at one of its ends drawn just after it is an arrow
/// pointing there, the short one its head (\a headOf: the arrow's index,
/// else -1); \a tips each one's tip.
QList<Gesture> gesturesOf(const QList<Stroke>& strokes, QList<QPointF>* tips, QList<int>* headOf);
/// \a points simplified to within \a tolerance (Douglas and Peucker's): a
/// ring's few dozen points, a line's two.
QPolygonF simplified(const QPolygonF& points, double tolerance);
/// How much of \a box lies inside the closed polygon \a ring, 0 to 1.
double insideShare(const QPolygonF& ring, const QRectF& box);
/// The points along \a stroke's middle line, close enough together - half
/// its width, or a 400th of its length - that nothing it lies on is missed
/// between them.
QPolygonF alongOf(const Stroke& stroke);

/// What a stroke can mark on a page, where it is drawn: a paragraph or a
/// table's cell and each of its words, a picture, a chart, a shape.
struct Target {
    QString kind;                                  ///< "paragraph", "cell", "picture", "chart", "shape"
    QString text;                                  ///< its text; a picture's alt text or name, a chart's title
    QPolygonF outline;
    QList<std::pair<QString, QPolygonF>> words;    ///< a paragraph's or cell's, each where it is drawn
};

/// The strokes drawn on one page (a slide: y down) read against what is on
/// it, \a targets: for each, in the order drawn, its number, how it was
/// drawn (gesturesOf()), its tool, colour and box, and what it marks - what
/// a ring is round (whole, or some of its words), the words a line
/// underlines or strikes through, what a highlighter or a mark lies on,
/// what a line or an arrow begins at and points at, what a dot is on - and,
/// \a points, its points simplified.
QJsonArray readPage(const QList<Stroke>& strokes, const QList<Target>& targets, bool points);
/// What every reading has: the stroke's number on its part, how it was
/// drawn, its tool, colour and box, and (\a points) its points simplified.
QJsonObject readingOf(const Stroke& stroke, int number, Gesture gesture, bool points);
/// How near a stroke's own ends something must be to be what it points at
/// or begins at, in its measures.
double reachOf(const Stroke& stroke);
/// A reading in a sentence, for a prompt; \a where says on what ("slide 2",
/// "cell TOP"), \a unit its measures ("points", "µm").
QString sentenceOf(const QJsonObject& reading, const QString& where, const QString& unit);
/// Readings (a presentation's, \a slides; else a layout's) a sentence each,
/// a line each - the latest \a most of them, the earlier ones counted.
QString sentencesOf(const QJsonArray& readings, bool slides, int most = 30);

/// The colours the pen's menu offers, and their names.
QList<std::pair<QColor, QString>> colors();

/// The pen in hand, every viewer's (Viewers/penTool, Viewers/penColor,
/// Viewers/penWidth, Viewers/highlighterWidth).
Pen currentPen();
void setCurrentPen(const Pen& pen);

/// Tells whatever shows the pen in hand that it changed.
class Pens : public QObject
{
    Q_OBJECT
public:
    static Pens* instance();
signals:
    void changed();
};

/*!
 * The toolbar's pen. Pressed, what the viewer shows is drawn on with the
 * pen in hand; its menu - the arrow beside it - picks the pen, the
 * highlighter or the eraser, a colour or any other, how broad, and undoes
 * the last change and clears the drawings, of the part shown or all. Its
 * icon is the pen in hand, in its colour.
 */
class PenButton : public QToolButton
{
    Q_OBJECT

public:
    /// \a part names what is drawn on in the menu's entries: "Slide", "Cell".
    PenButton(const QString& part, QWidget* parent = nullptr);
    /// The menu's entries are as the drawing is: \a canUndo, \a here strokes
    /// on the part shown, \a all on every part.
    void setDrawingState(bool canUndo, int here, int all);
    QMenu* penMenu() const { return a_menu; }
    /// What Other Colour… asks with, given the colour in hand - the viewer's
    /// own, so that its tab outlives the dialog (and a test's); invalid: none
    /// chosen. Unset: a colour dialog.
    void setColorChooser(std::function<QColor(const QColor&)> chooser);
    /// A pen's icon, drawn in the text's colour of the moment, its tip and
    /// the line under it in the pen's colour.
    static QIcon icon(const Pen& pen);

signals:
    void undoRequested();
    void clearPartRequested();
    void clearAllRequested();

private:
    void fillMenu();
    void updateMenu();
    void penChanged();
    QMenu* a_menu = nullptr;
    QString a_part;
    std::function<QColor(const QColor&)> a_chooser;
    QList<QAction*> a_tools, a_colors, a_widths;
    QAction* a_other = nullptr;
    QAction* a_undo = nullptr;
    QAction* a_clearHere = nullptr;
    QAction* a_clearAll = nullptr;
    bool a_canUndo = false;
    int a_here = 0;
    int a_all = 0;
};

} // namespace qucs_s::markup

#endif
