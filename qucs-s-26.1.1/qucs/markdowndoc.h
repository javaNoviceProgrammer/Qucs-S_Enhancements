/*
 * markdowndoc.h - a Markdown document: its text, and the text rendered
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MARKDOWNDOC_H
#define QUCS_MARKDOWNDOC_H

#include "textdoc.h"

#include <QColor>
#include <QFont>
#include <QString>
#include <QUrl>

class QButtonGroup;
class QScrollBar;
class QTextBrowser;
class QTextDocument;
class QTimer;

namespace qucs_s::markdown {

/// The colours a rendering is drawn in.
struct Colours {
    QColor paper = Qt::white;
    QColor ink = Qt::black;
    QColor code;   // the shade under code; derived from paper and ink when invalid
};

/// \a markdown (GitHub's dialect: headings, lists, task lists, tables,
/// code, links, images, emphasis, strike-through) rendered into \a document
/// in \a font, code on a shade, TeX math between dollars typeset
/// (mathtypeset.h). The document's layout must draw math objects
/// (MathObject::install()).
void render(QTextDocument* document, const QString& markdown, const QFont& font, const Colours& colours);

/// The anchor GitHub gives a heading: "Getting Started!" -> "getting-started".
QString anchorOf(const QString& heading);

} // namespace qucs_s::markdown

/*!
 * A Markdown file (.md, .markdown) in a tab: a text document - edited,
 * highlighted, searched, saved as any other - and, beside it or instead of
 * it, the text rendered, following the edits. A bar at the top chooses
 * Edit (the text alone), Split (the text and the rendering side by side,
 * the handle between them dragged to share the width) or Preview (the
 * rendering alone, the text not edited); the mode last chosen is kept
 * for the next file. A link in the rendering goes to its heading, opens
 * another document in Qucs-S, or opens in the system's application.
 */
class MarkdownDoc : public TextDoc
{
    Q_OBJECT

public:
    enum class Mode { Edit = 0, Split, Preview };

    MarkdownDoc(QucsApp* app, const QString& name);
    ~MarkdownDoc() override;

    bool load() override;

    Mode mode() const { return a_mode; }
    void setMode(Mode mode);
    /// The mode a Markdown file opens in: the one chosen last.
    static Mode defaultMode();

    QTextBrowser* preview() const { return a_preview; }
    /// The rendering made again now (it is a moment after each edit).
    void renderNow();
    /// Where a link of the rendering leads (see the class).
    void followLink(const QUrl& url);
    /// The part of the width the text has in Split mode (0.15 to 0.85).
    qreal share() const { return a_share; }
    void setShare(qreal share);

protected:
    QMargins extraMargins() const override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Mode a_mode = Mode::Split;
    bool a_readOnly = false;   // as it was before Preview made it read-only
    qreal a_share = 0.5;
    int a_dragFrom = -1;       // where a drag of the handle started
    QWidget* a_bar = nullptr;
    QButtonGroup* a_modes = nullptr;
    QTextBrowser* a_preview = nullptr;
    QWidget* a_handle = nullptr;
    QScrollBar* a_scroll = nullptr;   // the text's, beside it in Split mode
    QTimer* a_timer = nullptr;

    int barHeight() const;
    /// The text's width in Split mode.
    int textWidth() const;
    void layoutParts();
    void followScroll();
};

#endif // QUCS_MARKDOWNDOC_H
