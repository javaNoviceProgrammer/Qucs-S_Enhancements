/*
 * mathtypeset.h - TeX math typeset into an image, and found in Markdown
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MATHTYPESET_H
#define QUCS_MATHTYPESET_H

#include <QColor>
#include <QFont>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QTextFormat>
#include <QTextObjectInterface>

class QTextDocument;

/*!
 * Math as Claude writes it - TeX between dollars - drawn as TeX would:
 * fractions over one another, roots under their sign, scripts up and
 * down, sums and integrals with their limits, delimiters as tall as what
 * they hold, matrices, cases, aligned equations, accents, Greek and the
 * symbols of amsmath. A box layout of its own (TeX's styles, spacing
 * between atoms, the math axis), painted with the fonts at hand: a serif
 * for the letters (STIX Two, Times, Cambria, ...), whatever has the
 * symbol for the rest. What it does not know it shows as it was written.
 */
namespace qucs_s::math {

/// Typeset math: the image (at the device pixel ratio asked for) and its
/// size in logical pixels, the baseline \a ascent below the top.
struct Typeset {
    QImage image;
    qreal width = 0.0;
    qreal ascent = 0.0;
    qreal descent = 0.0;
    bool ok = false;   ///< it parsed; not ok: drawn anyway, as far as it went
};

/// Typesets \a tex (what is between the dollars) in the size of \a font:
/// \a display for $$...$$ (bigger operators, limits above and below).
Typeset typeset(const QString& tex, const QFont& font, const QColor& colour, bool display, qreal devicePixelRatio = 1.0);
/// Its size alone (width, ascent, descent - as typeset() at 1.0 gives
/// them), no image drawn.
Typeset measure(const QString& tex, const QFont& font, bool display);

/// \a math in an image whose middle is on the math axis of text in
/// \a font: set in a line with QTextCharFormat::AlignMiddle, its baseline
/// is the line's. \a height is the image's, in logical pixels.
QImage centredOnAxis(const Typeset& math, const QFont& font, qreal* height);

/*!
 * Math as an object in a QTextDocument, drawn at the size it was typeset
 * in (an image would be rounded to whole pixels and scaled by the ratio
 * of the screen's dots per inch to 96, which moves it off the baseline).
 */
class MathObject : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    static constexpr int Type = QTextFormat::UserObject + 77;
    enum Property { ImageProperty = QTextFormat::UserProperty + 770, SizeProperty };

    explicit MathObject(QObject* parent = nullptr) : QObject(parent) {}
    /// \a document's layout draws math objects from now on.
    static void install(QTextDocument* document);
    /// The format of an object that shows \a math in a line of text in
    /// \a font, on its baseline; \a tex is its tool tip.
    static QTextCharFormat format(const Typeset& math, const QFont& font, const QString& tex);

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int posInDocument,
                    const QTextFormat& format) override;
};

/// Math in Markdown, outside code: $$...$$ and \[...\] (display), $...$
/// and \(...\) (inline). A $ opens when a character other than a space
/// follows it and closes after one, not before a digit - so "$5 and $10"
/// is money.
struct Span {
    qsizetype start = 0;
    qsizetype length = 0;
    QString tex;
    bool display = false;
};
QList<Span> findMath(const QString& markdown);

} // namespace qucs_s::math

class QTextDocument;

namespace qucs_s::markdown {

/// \a markdown with its HTML as Qt's Markdown importer reads it whole,
/// outside code. The importer counts open tags across the document and
/// drops all the text after one never closed - or after a closing tag with
/// nothing open - but inline code: a <br> in a table's cell, a placeholder
/// such as <name>.dat or QList<Span>, a lone <b>, a comment with a tag in
/// it, lost the rest of a reply or a README. A void element written open
/// (<br>, <img src="x.png">, any case) is closed (<br/>); a tag of no HTML
/// element (<name>, <T>), an element not closed, and a closing tag that
/// closes nothing, are text (their '<' escaped); an element closed in
/// another block - a list's next item, a cell beside it, <details> over
/// blank lines - is taken out with its closing tag (Qt moved text between
/// the blocks); a comment is taken out. Elements closed in their block
/// (<b>x</b>, <kbd>, <details>) and autolinks stay. For each setMarkdown()
/// of a reply or a Markdown file.
QString htmlBalanced(const QString& markdown);

/// An <hr> inside a paragraph: Qt puts the text after it into the rule's
/// block, drawn above the rule. That text is moved to a block of its own
/// after the rule (in a table's cell too: a line more in it). After
/// setMarkdown().
void rulesApart(QTextDocument* document);

} // namespace qucs_s::markdown

#endif // QUCS_MATHTYPESET_H
