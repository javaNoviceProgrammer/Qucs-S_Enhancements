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

#include <QByteArray>
#include <QCache>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QTextObjectInterface>
#include <QUrl>

class QButtonGroup;
class QScrollBar;
class QTextBrowser;
class QTextDocument;
class QTimer;

class QTextEdit;
class QAbstractTextDocumentLayout;

namespace qucs_s::gpu {
class Canvas2D;
}

namespace qucs_s::markdown {

/// The colours a rendering is drawn in.
struct Colours {
    QColor paper = Qt::white;
    QColor ink = Qt::black;
    QColor code;   // the shade under code; derived from paper and ink when invalid
};

/// \a markdown (GitHub's dialect: headings, lists, task lists, tables,
/// code, links, images, emphasis, strike-through) rendered into \a document
/// in \a font, code on a shade, TeX math typeset (mathtypeset.h: between
/// dollars, a math block, a bare equation environment) at \a
/// devicePixelRatio - the screen's, else it is scaled up there, blurred.
/// The document's layout must draw math objects (MathObject::install()).
void render(QTextDocument* document, const QString& markdown, const QFont& font, const Colours& colours,
            qreal devicePixelRatio = 1.0);

/// The anchor GitHub gives a heading: "Getting Started!" -> "getting-started".
QString anchorOf(const QString& heading);

/// \a document's images no wider than the room their paragraph has when
/// the document is \a width wide (its margins and the paragraph's indent
/// left out; in a table, its cell's - the columns with no image their
/// text's width at most, those with one the rest alike): each at its own
/// size where that fits, else scaled down, its proportions kept - never
/// enlarged; a width its Markdown or HTML gave the most. Their own sizes
/// from the document's resources (the browser's files). Whether any
/// changed.
bool fitImages(QTextDocument* document, qreal width);

/*!
 * A document's images drawn smoothly: each scaled to the device pixels it
 * covers (an average of the pixels it takes in, scaled down; between them,
 * scaled up), on whole pixels, kept until that size changes. Qt's own
 * draws an image's own pixels into its rectangle a nearest one to each, so
 * a plot fitted to a narrow panel lost its lines and letters. Sizes, a
 * missing image and print (an image's own pixels in a PDF) are Qt's.
 */
class SmoothImages : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    /// \a document's layout draws its images so from now on.
    static void install(QTextDocument* document);

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int posInDocument,
                    const QTextFormat& format) override;

private:
    SmoothImages(QObject* parent, QTextObjectInterface* qts);
    QTextObjectInterface* a_qts;   // Qt's own handler
    QCache<QString, QImage> a_scaled;
};

/*!
 * A block quote's bar, a level's each, in the margin Qt's Markdown sets it
 * in by - which alone marked it (the update check of 7451b1c): drawn after
 * the view's own painting (its document's background covered them before
 * it), in its text's colour, faint.
 */
class QuoteBars : public QObject
{
public:
    /// \a edit's quotes drawn with their bars from now on (\a on; not:
    /// none drawn by it - PreviewTiles draws them on the GPU).
    static void install(QTextEdit* edit, bool on = true);
    /// Where \a edit draws its bars, in its viewport's coordinates.
    static QList<QRectF> bars(const QTextEdit* edit);
    /// Their colour: \a edit's text's, faint.
    static QColor colour(const QTextEdit* edit);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit QuoteBars(QTextEdit* edit);
    QTextEdit* a_edit;
    bool a_inside = false;   // (its view's own painting, sent on by it)
};

/*!
 * \a edit's viewport on the GPU (gpurendering.h): its document drawn as
 * QTextEdit draws it - scrolled, its selection (and extra ones), in its
 * palette - by the CPU, in tiles the GPU keeps as textures (rows of the
 * viewport's width, TileHeight high): a scroll draws none again, only the
 * rows it brings into sight. The quotes' bars over them, each frame. The
 * tiles drawn again when what they show changes: the layout's update (an
 * edit, an image or a formula typeset), the selection, the palette, the
 * width, the horizontal scroll, the pixel ratio. (A link focused by the
 * keyboard is marked as a selection is, not by the style's focus format.)
 */
class PreviewTiles : public QObject
{
public:
    explicit PreviewTiles(QTextEdit* edit);
    static constexpr int TileHeight = 256;
    void draw(gpu::Canvas2D& c);
    /// Tiles drawn by the CPU since the start (for the tests).
    int drawn() const { return a_drawn; }
    /// Tiles kept now (for the tests).
    int kept() const { return int(a_tiles.size()); }

private:
    QByteArray state() const;
    QImage tile(int row, int width, qreal ratio, const QColor& background);
    QTextEdit* a_edit;
    QPointer<QAbstractTextDocumentLayout> a_layout;   // its update() told
    quint64 a_updates = 0;
    QByteArray a_state;   // what the tiles show
    QHash<int, QImage> a_tiles;   // by row
    int a_drawn = 0;
};

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
    /// The preview drawn by the GPU: its viewport Qt's RHI's (Metal,
    /// Direct3D, OpenGL), its text and images drawn by the CPU in tiles the
    /// GPU keeps and scrolls (PreviewTiles; Settings… > Viewers:
    /// Viewers/markdownGpu, off unless chosen).
    void setPreviewGpu(bool on);
    bool previewGpu() const;
    /// Its tiles (null on the CPU).
    const qucs_s::markdown::PreviewTiles* previewTiles() const { return a_previewTiles; }
    /// The rendering made again now (it is a moment after each edit).
    void renderNow();
    /// The editor's colours and the rendering's, in the theme's paper and
    /// ink (black on white under the platform's themes, light or dark).
    void applyDocumentColors() override;
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
    QPointer<qucs_s::markdown::PreviewTiles> a_previewTiles;   // on the GPU
    QWidget* a_handle = nullptr;
    QScrollBar* a_scroll = nullptr;   // the text's, beside it in Split mode
    QTimer* a_timer = nullptr;
    bool a_fitting = false;   // its images being fitted (a resize they make not followed)

    int barHeight() const;
    /// The text's width in Split mode.
    int textWidth() const;
    void layoutParts();
    void followScroll();
    /// The rendering's images to its width.
    void fitImages();
};

#endif // QUCS_MARKDOWNDOC_H
